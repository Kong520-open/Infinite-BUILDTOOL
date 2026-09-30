#include "ChunkSpoolWriter.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace build_import {
namespace {

constexpr size_t kMaxOpenSpools = 96;
constexpr uint32_t kRoutingFanout = 32;
constexpr uint32_t kMaximumRoutingDepth = 32;
constexpr uint64_t kCancellationCheckInterval = 1024;
constexpr uint64_t kStagedRoutingReopenThreshold = 128;
constexpr size_t kSpoolStreamBufferSize = 64 * 1024;

bool cancellationRequested(const ChunkSpoolWriter::CancelCheck& cancel_check,
                           std::string* error) {
    if (!cancel_check || !cancel_check()) return false;
    if (error) *error = "import cancelled";
    return true;
}

uint64_t mix64(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

uint32_t routingBucket(const ChunkCoord& coord, ImportPhase phase, uint32_t depth) {
    uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(coord.x)) << 32) |
                   static_cast<uint32_t>(coord.z);
    key ^= static_cast<uint64_t>(static_cast<uint8_t>(phase)) * 0xd6e8feb86659fd93ULL;
    key = mix64(key ^ (static_cast<uint64_t>(depth) * 0xa0761d6478bd642fULL));
    return static_cast<uint32_t>(key % kRoutingFanout);
}

bool writeRawHeader(std::ofstream& stream, uint32_t kind, uint32_t record_size) {
    const RawSpoolHeaderV2 header{kRawSpoolMagic, kRawSpoolVersion, record_size, kind};
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    return static_cast<bool>(stream);
}

bool readRawHeader(std::ifstream& stream, uint32_t expected_kind,
                   uint32_t expected_record_size, std::string* error) {
    RawSpoolHeaderV2 header{};
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream || header.magic != kRawSpoolMagic ||
        header.version != kRawSpoolVersion || header.kind != expected_kind ||
        header.record_size != expected_record_size) {
        if (error) *error = "staged chunk route has an invalid header";
        return false;
    }
    return true;
}

bool readStagedRecord(std::ifstream& stream, StagedRawSpoolRecordV3* record,
                      bool* at_end, std::string* error) {
    *at_end = false;
    stream.read(reinterpret_cast<char*>(record), sizeof(*record));
    if (!stream) {
        if (stream.eof() && stream.gcount() == 0) {
            *at_end = true;
            return true;
        }
        if (error) *error = "cannot read staged chunk route";
        return false;
    }
    if (record->phase <= static_cast<uint8_t>(ImportPhase::Clear) ||
        record->phase >= static_cast<uint8_t>(ImportPhase::Count) ||
        record->block.reserved != 0 || record->reserved[0] != 0 ||
        record->reserved[1] != 0 || record->reserved[2] != 0) {
        if (error) *error = "staged chunk route contains an invalid record";
        return false;
    }
    return true;
}

bool replaceFileAtomically(const std::string& source, const std::string& destination) {
#if defined(_WIN32)
    return MoveFileExA(source.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    return std::rename(source.c_str(), destination.c_str()) == 0;
#endif
}

int createDirectory(const char* path) {
#if defined(_WIN32)
    return ::_mkdir(path);
#else
    return ::mkdir(path, 0700);
#endif
}

bool ensureDirectory(const std::string& directory) {
    if (directory.empty()) return false;
    std::string partial;
    for (size_t index = 0; index <= directory.size(); ++index) {
        if (index != directory.size() && directory[index] != '/' && directory[index] != '\\') {
            partial += directory[index];
            continue;
        }
        if (!partial.empty() && partial != "." &&
            createDirectory(partial.c_str()) != 0 && errno != EEXIST) return false;
        if (index != directory.size()) {
            if (partial.empty() && directory[index] == '/') partial = "/";
            else if (!partial.empty() && partial.back() != '/') partial += '/';
        }
    }
    return true;
}

void extendBounds(BlockBounds* bounds, const ParsedBlock& block) {
    if (!bounds->isValid()) {
        *bounds = {block.world_x, block.world_y, block.world_z,
                   block.world_x, block.world_y, block.world_z};
        return;
    }
    bounds->min_x = std::min(bounds->min_x, block.world_x);
    bounds->min_y = std::min(bounds->min_y, block.world_y);
    bounds->min_z = std::min(bounds->min_z, block.world_z);
    bounds->max_x = std::max(bounds->max_x, block.world_x);
    bounds->max_y = std::max(bounds->max_y, block.world_y);
    bounds->max_z = std::max(bounds->max_z, block.world_z);
}

void extendBounds(BlockBounds* bounds, const BlockBounds& addition) {
    if (!addition.isValid()) return;
    if (!bounds->isValid()) {
        *bounds = addition;
        return;
    }
    bounds->min_x = std::min(bounds->min_x, addition.min_x);
    bounds->min_y = std::min(bounds->min_y, addition.min_y);
    bounds->min_z = std::min(bounds->min_z, addition.min_z);
    bounds->max_x = std::max(bounds->max_x, addition.max_x);
    bounds->max_y = std::max(bounds->max_y, addition.max_y);
    bounds->max_z = std::max(bounds->max_z, addition.max_z);
}

}  // namespace

struct ChunkSpoolWriter::OpenSpool {
    std::unique_ptr<char[]> stream_buffer;
    std::ofstream stream;
    std::string path;
    uint64_t last_access = 0;
    RouteMetadata route_metadata;
};

ChunkSpoolWriter::ChunkSpoolWriter(std::string directory, int32_t chunk_size,
                                   size_t maximum_chunk_descriptors)
    : directory_(std::move(directory)), chunk_size_(chunk_size),
      maximum_chunk_descriptors_(maximum_chunk_descriptors) {}

ChunkSpoolWriter::~ChunkSpoolWriter() {
    if (!finished_) discard();
}

bool ChunkSpoolWriter::includeVolume(const BlockBounds& world_bounds, std::string* error,
                                     const CancelCheck& cancel_check) {
    return includeVolumeInternal(world_bounds, true, error, cancel_check);
}

bool ChunkSpoolWriter::includeMissingFootprint(const BlockBounds& world_bounds,
                                               std::string* error,
                                               const CancelCheck& cancel_check) {
    return includeVolumeInternal(world_bounds, false, error, cancel_check);
}

bool ChunkSpoolWriter::includeVolumeInternal(const BlockBounds& world_bounds,
                                             bool extend_existing_bounds,
                                             std::string* error,
                                             const CancelCheck& cancel_check) {
    if (finished_ || chunk_size_ <= 0 || !world_bounds.isValid()) {
        if (error) *error = "invalid volume or closed spool writer";
        return false;
    }
    const int32_t min_chunk_x = floorDiv(world_bounds.min_x, chunk_size_);
    const int32_t max_chunk_x = floorDiv(world_bounds.max_x, chunk_size_);
    const int32_t min_chunk_z = floorDiv(world_bounds.min_z, chunk_size_);
    const int32_t max_chunk_z = floorDiv(world_bounds.max_z, chunk_size_);
    const uint64_t chunk_columns =
        static_cast<uint64_t>(static_cast<int64_t>(max_chunk_x) - min_chunk_x + 1);
    const uint64_t chunk_rows =
        static_cast<uint64_t>(static_cast<int64_t>(max_chunk_z) - min_chunk_z + 1);
    if (maximum_chunk_descriptors_ != 0) {
        if (descriptors_.size() > maximum_chunk_descriptors_ ||
            chunk_columns > maximum_chunk_descriptors_ ||
            chunk_rows > maximum_chunk_descriptors_ ||
            chunk_columns > maximum_chunk_descriptors_ / chunk_rows) {
            if (error) *error = "source volume spans too many logical chunks";
            return false;
        }

        const uint64_t requested_chunks = chunk_columns * chunk_rows;
        const size_t available_descriptors =
            maximum_chunk_descriptors_ - descriptors_.size();
        if (requested_chunks > available_descriptors) {
            size_t new_descriptors = 0;
            uint64_t inspected_chunks = 0;
            for (int64_t chunk_z = min_chunk_z; chunk_z <= max_chunk_z; ++chunk_z) {
                for (int64_t chunk_x = min_chunk_x; chunk_x <= max_chunk_x; ++chunk_x) {
                    if ((inspected_chunks++ % kCancellationCheckInterval) == 0 &&
                        cancellationRequested(cancel_check, error)) {
                        return false;
                    }
                    const ChunkCoord coord{
                        static_cast<int32_t>(chunk_x), static_cast<int32_t>(chunk_z)};
                    if (descriptors_.find(coord) == descriptors_.end() &&
                        ++new_descriptors > available_descriptors) {
                        if (error) *error = "source volume spans too many logical chunks";
                        return false;
                    }
                }
            }
        }
    }
    uint64_t visited_chunks = 0;
    for (int64_t chunk_z = min_chunk_z; chunk_z <= max_chunk_z; ++chunk_z) {
        const int64_t chunk_min_z = chunk_z * chunk_size_;
        const int64_t chunk_max_z = chunk_min_z + chunk_size_ - 1;
        for (int64_t chunk_x = min_chunk_x; chunk_x <= max_chunk_x; ++chunk_x) {
            if ((visited_chunks++ % kCancellationCheckInterval) == 0 &&
                cancellationRequested(cancel_check, error)) {
                return false;
            }
            const int64_t chunk_min_x = chunk_x * chunk_size_;
            const int64_t chunk_max_x = chunk_min_x + chunk_size_ - 1;
            const BlockBounds intersection{
                static_cast<int32_t>(std::max<int64_t>(world_bounds.min_x, chunk_min_x)),
                world_bounds.min_y,
                static_cast<int32_t>(std::max<int64_t>(world_bounds.min_z, chunk_min_z)),
                static_cast<int32_t>(std::min<int64_t>(world_bounds.max_x, chunk_max_x)),
                world_bounds.max_y,
                static_cast<int32_t>(std::min<int64_t>(world_bounds.max_z, chunk_max_z)),
            };
            const ChunkCoord coord{static_cast<int32_t>(chunk_x), static_cast<int32_t>(chunk_z)};
            const auto found = descriptors_.find(coord);
            if (found == descriptors_.end()) {
                ChunkDescriptor descriptor;
                descriptor.coord = coord;
                extendBounds(&descriptor.imported_bounds, intersection);
                descriptors_.emplace(coord, std::move(descriptor));
            } else if (extend_existing_bounds || !found->second.imported_bounds.isValid()) {
                extendBounds(&found->second.imported_bounds, intersection);
            }
        }
    }
    return true;
}

bool ChunkSpoolWriter::append(const ParsedBlock& block, std::string* error) {
    const uint8_t phase = static_cast<uint8_t>(block.spec.phase);
    if (finished_ || chunk_size_ <= 0 ||
        phase <= static_cast<uint8_t>(ImportPhase::Clear) ||
        phase >= static_cast<uint8_t>(ImportPhase::Count) ||
        block.spec.command_name.empty() ||
        block.spec.command_name.size() > UINT16_MAX) {
        if (error) *error = "invalid block or closed spool writer";
        return false;
    }
    const ChunkCoord coord = chunkForBlock(block.world_x, block.world_z, chunk_size_);
    const uint8_t flags = 0x04 | (block.spec.can_fill ? 0x01 : 0x00) |
                          (block.spec.single_layer_only ? 0x02 : 0x00) |
                          (block.spec.stateful ? 0x08 : 0x00);
    uint32_t name_id = 0;
    if (!paletteIdFor(block.spec.command_name, &name_id, error)) return false;
    ChunkDescriptor* descriptor = descriptorFor(coord);
    if (!descriptor) {
        if (error) *error = "cannot allocate chunk descriptor";
        return false;
    }
    const size_t phase_index = phaseIndex(block.spec.phase);
    const bool first_phase_record = !descriptor->has_phase[phase_index];
    const std::string new_path = first_phase_record
        ? spoolPath(coord, block.spec.phase) : std::string();
    const std::string& final_path = first_phase_record
        ? new_path : descriptor->spool_paths[phase_index];
    if (final_path.empty()) {
        if (error) *error = "chunk descriptor is missing its spool path";
        return false;
    }
    const std::pair<ChunkCoord, ImportPhase> spool_key{coord, block.spec.phase};
    if (!staged_routing_ && initialized_paths_.find(final_path) != initialized_paths_.end() &&
        open_spools_.find(spool_key) == open_spools_.end()) {
        ++reopened_spool_count_;
        if (reopened_spool_count_ >= kStagedRoutingReopenThreshold &&
            !switchToStagedRouting(error)) return false;
    }
    if (staged_routing_) {
        if (!appendStaged(block, coord, name_id, flags, error)) return false;
    } else {
        OpenSpool* spool = getOrCreate(coord, block.spec.phase, final_path, error);
        if (!spool) return false;
        const RawSpoolRecordV3 record{block.world_x, block.world_y, block.world_z,
                                     name_id, block.spec.aux, flags, 0};
        spool->stream.write(reinterpret_cast<const char*>(&record), sizeof(record));
        if (!spool->stream) {
            if (error) *error = "cannot write chunk spool";
            return false;
        }
    }

    descriptor->coord = coord;
    extendBounds(&descriptor->imported_bounds, block);
    descriptor->has_phase[phase_index] = true;
    if (first_phase_record) descriptor->spool_paths[phase_index] = new_path;
    return true;
}

std::vector<ChunkDescriptor> ChunkSpoolWriter::finish(
        std::string* error, const CancelCheck& cancel_check,
        const ProgressCallback& progress_callback) {
    if (cancellationRequested(cancel_check, error)) return {};
    if (!closeOpenSpools(error)) return {};
    if (staged_routing_) {
        std::vector<std::pair<std::string, RouteMetadata>> top_level_routes;
        top_level_routes.reserve(routing_buckets_.size());
        for (auto& entry : routing_buckets_) {
            entry.second.stream.flush();
            entry.second.stream.close();
            if (!entry.second.stream) {
                if (error) *error = "cannot finalize staged chunk route";
                return {};
            }
            if (!entry.second.route_metadata.exceeds_open_spool_limit &&
                entry.second.route_metadata.unique_routes.empty()) {
                if (error) *error = "staged chunk route is missing its metadata";
                return {};
            }
            top_level_routes.emplace_back(
                entry.second.path, std::move(entry.second.route_metadata));
        }
        routing_buckets_.clear();
        cached_route_ = nullptr;
        has_cached_route_ = false;
        if (progress_callback) progress_callback(0, top_level_routes.size());
        size_t completed_paths = 0;
        for (const auto& route : top_level_routes) {
            if (!routeStagedFile(route.first, 1, route.second,
                                 cancel_check, error)) return {};
            if (progress_callback) {
                progress_callback(++completed_paths, top_level_routes.size());
            }
        }
        if (!closeOpenSpools(error)) return {};
    } else if (progress_callback) {
        progress_callback(1, 1);
    }
    if (!palette_names_.empty() && !writePalette(error)) return {};
    initialized_paths_.clear();
    routing_paths_.clear();
    finished_ = true;
    std::vector<ChunkDescriptor> result;
    result.reserve(descriptors_.size());
    for (const auto& entry : descriptors_) result.push_back(entry.second);
    return result;
}

bool ChunkSpoolWriter::closeOpenSpools(std::string* error) {
    for (auto& entry : open_spools_) {
        entry.second.stream.flush();
        entry.second.stream.close();
        if (!entry.second.stream) {
            if (error) *error = "cannot finalize chunk spool";
            return false;
        }
    }
    open_spools_.clear();
    cached_open_spool_ = nullptr;
    has_cached_open_spool_ = false;
    return true;
}

bool ChunkSpoolWriter::switchToStagedRouting(std::string* error) {
    if (staged_routing_) return true;
    if (!closeOpenSpools(error)) return false;
    staged_routing_ = true;
    return true;
}

bool ChunkSpoolWriter::appendStaged(const ParsedBlock& block, const ChunkCoord& coord,
                                    uint32_t name_id, uint8_t flags, std::string* error) {
    if (!ensureDirectoryReady(error)) return false;
    const uint32_t bucket = routingBucket(coord, block.spec.phase, 0);
    OpenSpool* route = has_cached_route_ && cached_route_bucket_ == bucket
        ? cached_route_ : nullptr;
    if (!route) {
        auto found = routing_buckets_.find(bucket);
        if (found != routing_buckets_.end()) route = &found->second;
    }
    if (!route) {
        OpenSpool new_route;
        new_route.path = routingPath(0, 0, bucket);
        new_route.stream_buffer = std::make_unique<char[]>(kSpoolStreamBufferSize);
        new_route.stream.rdbuf()->pubsetbuf(
            new_route.stream_buffer.get(), kSpoolStreamBufferSize);
        new_route.stream.open(new_route.path, std::ios::binary | std::ios::trunc);
        if (!new_route.stream ||
            !writeRawHeader(new_route.stream, kRawSpoolKindStaged,
                             sizeof(StagedRawSpoolRecordV3))) {
            if (error) *error = "cannot create staged chunk route";
            return false;
        }
        routing_paths_.insert(new_route.path);
        route = &routing_buckets_.emplace(bucket, std::move(new_route)).first->second;
    }
    cached_route_ = route;
    cached_route_bucket_ = bucket;
    has_cached_route_ = true;
    const RawSpoolRecordV3 block_record{
        block.world_x, block.world_y, block.world_z, name_id,
        block.spec.aux, flags, 0,
    };
    const StagedRawSpoolRecordV3 staged{
        block_record, static_cast<uint8_t>(block.spec.phase), {0, 0, 0}};
    route->stream.write(reinterpret_cast<const char*>(&staged), sizeof(staged));
    if (!route->stream) {
        if (error) *error = "cannot write staged chunk route";
        return false;
    }
    rememberRoute(&route->route_metadata, coord, block.spec.phase);
    return true;
}

bool ChunkSpoolWriter::routeStagedFile(const std::string& path, uint32_t depth,
                                       const RouteMetadata& metadata,
                                       const CancelCheck& cancel_check,
                                       std::string* error) {
    if (!metadata.exceeds_open_spool_limit) {
        auto input_buffer = std::make_unique<char[]>(kSpoolStreamBufferSize);
        std::ifstream input;
        input.rdbuf()->pubsetbuf(input_buffer.get(), kSpoolStreamBufferSize);
        input.open(path, std::ios::binary);
        if (!input) {
            if (error) *error = "cannot open staged chunk route";
            return false;
        }
        if (!readRawHeader(input, kRawSpoolKindStaged,
                            sizeof(StagedRawSpoolRecordV3), error)) return false;
        uint64_t record_index = 0;
        StagedRawSpoolRecordV3 record{};
        while (true) {
            if ((record_index++ % kCancellationCheckInterval) == 0 &&
                cancellationRequested(cancel_check, error)) return false;
            bool at_end = false;
            if (!readStagedRecord(input, &record, &at_end, error)) return false;
            if (at_end) break;
            const ChunkCoord coord = chunkForBlock(record.block.x, record.block.z, chunk_size_);
            const ImportPhase phase = static_cast<ImportPhase>(record.phase);
            ChunkDescriptor* descriptor = descriptorFor(coord);
            const size_t phase_index = phaseIndex(phase);
            if (!descriptor || !descriptor->has_phase[phase_index] ||
                descriptor->spool_paths[phase_index].empty()) {
                if (error) *error = "staged route is missing its chunk descriptor";
                return false;
            }
            const std::string& final_path = descriptor->spool_paths[phase_index];
            OpenSpool* output = getOrCreate(coord, phase, final_path, error);
            if (!output) return false;
            output->stream.write(reinterpret_cast<const char*>(&record.block), sizeof(record.block));
            if (!output->stream) {
                if (error) *error = "cannot write routed chunk spool";
                return false;
            }
        }
        if (!closeOpenSpools(error)) return false;
        input.close();
        if (std::remove(path.c_str()) != 0) {
            if (error) *error = "cannot remove staged chunk route";
            return false;
        }
        routing_paths_.erase(path);
        return true;
    }

    if (depth > kMaximumRoutingDepth) {
        if (error) *error = "staged chunk routing exceeded its collision depth";
        return false;
    }
    const uint64_t group = routing_group_serial_++;
    std::array<std::unique_ptr<char[]>, kRoutingFanout> output_buffers;
    std::array<std::ofstream, kRoutingFanout> outputs;
    std::array<std::string, kRoutingFanout> child_paths;
    std::array<RouteMetadata, kRoutingFanout> child_metadata;
    std::array<bool, kRoutingFanout> has_child{};
    auto input_buffer = std::make_unique<char[]>(kSpoolStreamBufferSize);
    std::ifstream input;
    input.rdbuf()->pubsetbuf(input_buffer.get(), kSpoolStreamBufferSize);
    input.open(path, std::ios::binary);
    if (!input) {
        if (error) *error = "cannot reopen staged chunk route for partitioning";
        return false;
    }
    if (!readRawHeader(input, kRawSpoolKindStaged,
                        sizeof(StagedRawSpoolRecordV3), error)) return false;
    uint64_t record_index = 0;
    StagedRawSpoolRecordV3 record{};
    while (true) {
        if ((record_index++ % kCancellationCheckInterval) == 0 &&
            cancellationRequested(cancel_check, error)) return false;
        bool at_end = false;
        if (!readStagedRecord(input, &record, &at_end, error)) return false;
        if (at_end) break;
        const ImportPhase phase = static_cast<ImportPhase>(record.phase);
        const ChunkCoord coord = chunkForBlock(record.block.x, record.block.z, chunk_size_);
        const uint32_t bucket = routingBucket(coord, phase, depth);
        if (!has_child[bucket]) {
            const std::string child = routingPath(depth, group, bucket);
            output_buffers[bucket] = std::make_unique<char[]>(kSpoolStreamBufferSize);
            outputs[bucket].rdbuf()->pubsetbuf(
                output_buffers[bucket].get(), kSpoolStreamBufferSize);
            outputs[bucket].open(child, std::ios::binary | std::ios::trunc);
            if (!outputs[bucket] ||
                !writeRawHeader(outputs[bucket], kRawSpoolKindStaged,
                                sizeof(StagedRawSpoolRecordV3))) {
                if (error) *error = "cannot create staged chunk sub-route";
                return false;
            }
            routing_paths_.insert(child);
            child_paths[bucket] = child;
            has_child[bucket] = true;
        }
        outputs[bucket].write(reinterpret_cast<const char*>(&record), sizeof(record));
        if (!outputs[bucket]) {
            if (error) *error = "cannot partition staged chunk route";
            return false;
        }
        rememberRoute(&child_metadata[bucket], coord, phase);
    }
    for (uint32_t bucket = 0; bucket < kRoutingFanout; ++bucket) {
        if (!has_child[bucket]) continue;
        outputs[bucket].flush();
        outputs[bucket].close();
        if (!outputs[bucket]) {
            if (error) *error = "cannot finalize staged chunk sub-route";
            return false;
        }
    }
    input.close();
    if (std::remove(path.c_str()) != 0) {
        if (error) *error = "cannot remove staged chunk parent route";
        return false;
    }
    routing_paths_.erase(path);
    for (uint32_t bucket = 0; bucket < kRoutingFanout; ++bucket) {
        if (!has_child[bucket]) continue;
        if (!routeStagedFile(child_paths[bucket], depth + 1, child_metadata[bucket],
                             cancel_check, error)) return false;
    }
    return true;
}

void ChunkSpoolWriter::rememberRoute(RouteMetadata* metadata, const ChunkCoord& coord,
                                     ImportPhase phase) {
    if (metadata->exceeds_open_spool_limit) return;
    if (metadata->has_last_route && metadata->last_route_coord == coord &&
        metadata->last_route_phase == phase) return;
    metadata->last_route_coord = coord;
    metadata->last_route_phase = phase;
    metadata->has_last_route = true;
    metadata->unique_routes.emplace(coord, phase);
    if (metadata->unique_routes.size() > kMaxOpenSpools) {
        metadata->unique_routes.clear();
        metadata->exceeds_open_spool_limit = true;
    }
}

void ChunkSpoolWriter::discard() {
    for (auto& entry : open_spools_) {
        entry.second.stream.close();
        if (!entry.second.path.empty()) std::remove(entry.second.path.c_str());
    }
    open_spools_.clear();
    cached_open_spool_ = nullptr;
    has_cached_open_spool_ = false;
    for (auto& entry : routing_buckets_) entry.second.stream.close();
    routing_buckets_.clear();
    cached_route_ = nullptr;
    has_cached_route_ = false;
    for (const std::string& path : routing_paths_) {
        if (!path.empty()) std::remove(path.c_str());
    }
    routing_paths_.clear();
    initialized_paths_.clear();
    std::remove(palettePath().c_str());
    std::remove((palettePath() + ".tmp").c_str());
    palette_indices_.clear();
    palette_names_.clear();
    for (const auto& entry : descriptors_) {
        for (const std::string& path : entry.second.spool_paths) {
            if (!path.empty()) std::remove(path.c_str());
        }
    }
    descriptors_.clear();
    cached_descriptor_ = nullptr;
    has_cached_descriptor_ = false;
    finished_ = true;
}

std::string ChunkSpoolWriter::spoolPath(const ChunkCoord& coord, ImportPhase phase) const {
    return directory_ + "/chunk_" + std::to_string(coord.x) + "_" + std::to_string(coord.z) +
           "_phase_" + std::to_string(static_cast<uint8_t>(phase)) + ".bsp";
}

std::string ChunkSpoolWriter::routingPath(uint32_t depth, uint64_t group,
                                          uint32_t bucket) const {
    return directory_ + "/_route_" + std::to_string(depth) + "_" +
           std::to_string(group) + "_" + std::to_string(bucket) + ".tmp";
}

ChunkDescriptor* ChunkSpoolWriter::descriptorFor(const ChunkCoord& coord) {
    if (has_cached_descriptor_ && cached_descriptor_coord_ == coord) {
        return cached_descriptor_;
    }
    auto found = descriptors_.find(coord);
    if (found == descriptors_.end()) {
        found = descriptors_.emplace(coord, ChunkDescriptor{}).first;
        found->second.coord = coord;
    }
    cached_descriptor_coord_ = coord;
    cached_descriptor_ = &found->second;
    has_cached_descriptor_ = true;
    return cached_descriptor_;
}

bool ChunkSpoolWriter::ensureDirectoryReady(std::string* error) {
    if (directory_ready_) return true;
    directory_ready_ = ensureDirectory(directory_);
    if (!directory_ready_ && error) *error = "cannot create spool directory";
    return directory_ready_;
}

ChunkSpoolWriter::OpenSpool* ChunkSpoolWriter::getOrCreate(
        const ChunkCoord& coord, ImportPhase phase, const std::string& path,
        std::string* error) {
    if (has_cached_open_spool_ && cached_open_spool_coord_ == coord &&
        cached_open_spool_phase_ == phase) {
        cached_open_spool_->last_access = ++access_serial_;
        return cached_open_spool_;
    }
    const std::pair<ChunkCoord, ImportPhase> key{coord, phase};
    auto existing = open_spools_.find(key);
    if (existing != open_spools_.end()) {
        existing->second.last_access = ++access_serial_;
        cached_open_spool_ = &existing->second;
        cached_open_spool_coord_ = coord;
        cached_open_spool_phase_ = phase;
        has_cached_open_spool_ = true;
        return cached_open_spool_;
    }
    if (!ensureDirectoryReady(error)) return nullptr;
    // Large blueprints commonly interleave more than 32 chunk/phase streams.
    // Keeping a wider bounded cache avoids repeated close/reopen/append churn
    // without risking unbounded file-descriptor use on Android.
    if (open_spools_.size() >= kMaxOpenSpools) {
        auto oldest = std::min_element(open_spools_.begin(), open_spools_.end(),
            [](const auto& left, const auto& right) { return left.second.last_access < right.second.last_access; });
        oldest->second.stream.flush();
        oldest->second.stream.close();
        if (!oldest->second.stream) {
            if (error) *error = "cannot flush chunk spool";
            return nullptr;
        }
        if (has_cached_open_spool_ && cached_open_spool_ == &oldest->second) {
            cached_open_spool_ = nullptr;
            has_cached_open_spool_ = false;
        }
        open_spools_.erase(oldest);
    }
    OpenSpool spool;
    spool.path = path;
    const bool first_open = initialized_paths_.insert(spool.path).second;
    spool.stream_buffer = std::make_unique<char[]>(kSpoolStreamBufferSize);
    spool.stream.rdbuf()->pubsetbuf(spool.stream_buffer.get(), kSpoolStreamBufferSize);
    spool.stream.open(spool.path, std::ios::binary | (first_open ? std::ios::trunc : std::ios::app));
    if (!spool.stream ||
        (first_open && !writeRawHeader(spool.stream, kRawSpoolKindBlocks,
                                       sizeof(RawSpoolRecordV3)))) {
        if (error) *error = "cannot open chunk spool";
        return nullptr;
    }
    spool.last_access = ++access_serial_;
    cached_open_spool_ = &open_spools_.emplace(key, std::move(spool)).first->second;
    cached_open_spool_coord_ = coord;
    cached_open_spool_phase_ = phase;
    has_cached_open_spool_ = true;
    return cached_open_spool_;
}

bool ChunkSpoolWriter::paletteIdFor(const std::string& name, uint32_t* id,
                                    std::string* error) {
    const auto found = palette_indices_.find(name);
    if (found != palette_indices_.end()) {
        *id = found->second;
        return true;
    }
    if (palette_names_.size() >= std::numeric_limits<uint32_t>::max()) {
        if (error) *error = "raw block palette contains too many names";
        return false;
    }
    const uint32_t next = static_cast<uint32_t>(palette_names_.size());
    palette_names_.push_back(name);
    palette_indices_.emplace(palette_names_.back(), next);
    *id = next;
    return true;
}

std::string ChunkSpoolWriter::palettePath() const {
    return directory_ + "/" + kRawPaletteFileName;
}

bool ChunkSpoolWriter::writePalette(std::string* error) {
    if (!ensureDirectoryReady(error)) return false;
    const std::string destination = palettePath();
    const std::string temporary = destination + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    const RawPaletteHeaderV2 header{
        kRawPaletteMagic, kRawPaletteVersion,
        static_cast<uint32_t>(palette_names_.size()), 0};
    stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
    for (const std::string& name : palette_names_) {
        const uint16_t length = static_cast<uint16_t>(name.size());
        stream.write(reinterpret_cast<const char*>(&length), sizeof(length));
        stream.write(name.data(), length);
    }
    stream.flush();
    stream.close();
    if (!stream || !replaceFileAtomically(temporary, destination)) {
        std::remove(temporary.c_str());
        if (error) *error = "cannot finalize raw block palette";
        return false;
    }
    return true;
}

}  // namespace build_import
