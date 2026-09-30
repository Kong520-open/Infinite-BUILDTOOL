#include "BuildImportUndoStore.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace build_import {
namespace {

constexpr uint32_t kMagic = 0x4F444E55U;  // "UNDO" in little-endian form.

bool writeUint32(std::ostream& stream, uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU),
    };
    stream.write(bytes, sizeof(bytes));
    return static_cast<bool>(stream);
}

bool writeInt32(std::ostream& stream, int32_t value) {
    return writeUint32(stream, static_cast<uint32_t>(value));
}

bool readUint32(std::istream& stream, uint32_t* value) {
    if (!value) return false;
    unsigned char bytes[4]{};
    stream.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    if (!stream) return false;
    *value = static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8U) |
        (static_cast<uint32_t>(bytes[2]) << 16U) |
        (static_cast<uint32_t>(bytes[3]) << 24U);
    return true;
}

bool readInt32(std::istream& stream, int32_t* value) {
    if (!value) return false;
    uint32_t raw = 0;
    if (!readUint32(stream, &raw)) return false;
    *value = static_cast<int32_t>(raw);
    return true;
}

bool writeString(std::ostream& stream, const std::string& value) {
    if (value.size() > BuildImportUndoStore::kMaximumWorldIdLength ||
        value.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    const uint32_t size = static_cast<uint32_t>(value.size());
    if (!writeUint32(stream, size)) return false;
    if (size != 0) stream.write(value.data(), static_cast<std::streamsize>(size));
    return static_cast<bool>(stream);
}

bool readString(std::istream& stream, size_t maximum_size, std::string* value) {
    if (!value) return false;
    uint32_t size = 0;
    if (!readUint32(stream, &size) || size > maximum_size) {
        return false;
    }
    value->assign(size, '\0');
    if (size != 0) {
        stream.read(&(*value)[0], static_cast<std::streamsize>(size));
    }
    return static_cast<bool>(stream);
}

bool writeBounds(std::ostream& stream, const BlockBounds& bounds) {
    return writeInt32(stream, bounds.min_x) && writeInt32(stream, bounds.min_y) &&
        writeInt32(stream, bounds.min_z) && writeInt32(stream, bounds.max_x) &&
        writeInt32(stream, bounds.max_y) && writeInt32(stream, bounds.max_z);
}

bool readBounds(std::istream& stream, BlockBounds* bounds) {
    return bounds && readInt32(stream, &bounds->min_x) &&
        readInt32(stream, &bounds->min_y) && readInt32(stream, &bounds->min_z) &&
        readInt32(stream, &bounds->max_x) && readInt32(stream, &bounds->max_y) &&
        readInt32(stream, &bounds->max_z);
}

bool validateManifest(const BuildImportUndoManifest& manifest, std::string* error) {
    if (manifest.world.world_id.empty() ||
        manifest.world.world_id.size() > BuildImportUndoStore::kMaximumWorldIdLength) {
        if (error) *error = "undo world context is invalid";
        return false;
    }
    if (manifest.chunk_size != ImportConfig::kVanillaChunkSize ||
        !isValidSimulationChunkRange(manifest.simulation_chunk_range) ||
        manifest.blocks_per_second < 1 ||
        manifest.blocks_per_second > kMaximumBlocksPerSecond) {
        if (error) *error = "undo import configuration is invalid";
        return false;
    }
    if (manifest.regions.empty() ||
        manifest.regions.size() > BuildImportUndoStore::kMaximumRegionCount) {
        if (error) *error = "undo region count is invalid";
        return false;
    }
    if (manifest.record_id.empty() ||
        manifest.record_id.size() > BuildImportUndoStore::kMaximumRecordIdLength ||
        manifest.claim_job_id.size() > BuildImportUndoStore::kMaximumClaimJobIdLength) {
        if (error) *error = "undo record identity is invalid";
        return false;
    }
    const bool unsafe_claim_path = manifest.claim_job_id == "." ||
        manifest.claim_job_id == ".." ||
        manifest.claim_job_id.find_first_of("/\\") != std::string::npos;
    if ((manifest.state == BuildImportUndoRecordState::Available &&
         !manifest.claim_job_id.empty()) ||
        (manifest.state == BuildImportUndoRecordState::Claimed &&
         (manifest.claim_job_id.empty() || unsafe_claim_path)) ||
        (manifest.state != BuildImportUndoRecordState::Available &&
         manifest.state != BuildImportUndoRecordState::Claimed &&
         manifest.state != BuildImportUndoRecordState::Consumed)) {
        if (error) *error = "undo record lifecycle state is invalid";
        return false;
    }

    std::set<ChunkCoord> coordinates;
    for (const BuildImportUndoRegion& region : manifest.regions) {
        if (!region.bounds.isValid()) {
            if (error) *error = "undo region bounds are invalid";
            return false;
        }
        const ChunkCoord minimum = chunkForBlock(
            region.bounds.min_x, region.bounds.min_z, ImportConfig::kVanillaChunkSize);
        const ChunkCoord maximum = chunkForBlock(
            region.bounds.max_x, region.bounds.max_z, ImportConfig::kVanillaChunkSize);
        if (!(minimum == region.coord) || !(maximum == region.coord)) {
            if (error) *error = "undo region bounds are outside their chunk";
            return false;
        }
        if (!coordinates.insert(region.coord).second) {
            if (error) *error = "undo manifest contains duplicate chunks";
            return false;
        }
    }
    return true;
}

void hashBytes(uint64_t* hash, const void* data, size_t size) {
    if (!hash || (!data && size != 0U)) return;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t index = 0; index < size; ++index) {
        *hash ^= bytes[index];
        *hash *= 1099511628211ULL;
    }
}

template <typename T>
void hashValue(uint64_t* hash, const T& value) {
    hashBytes(hash, &value, sizeof(value));
}

std::string legacyRecordId(const BuildImportUndoManifest& manifest) {
    uint64_t hash = 1469598103934665603ULL;
    hashBytes(&hash, manifest.world.world_id.data(), manifest.world.world_id.size());
    hashValue(&hash, manifest.world.dimension_id);
    hashValue(&hash, manifest.chunk_size);
    hashValue(&hash, manifest.simulation_chunk_range);
    hashValue(&hash, manifest.blocks_per_second);
    for (const BuildImportUndoRegion& region : manifest.regions) {
        hashValue(&hash, region.coord.x);
        hashValue(&hash, region.coord.z);
        hashValue(&hash, region.bounds.min_x);
        hashValue(&hash, region.bounds.min_y);
        hashValue(&hash, region.bounds.min_z);
        hashValue(&hash, region.bounds.max_x);
        hashValue(&hash, region.bounds.max_y);
        hashValue(&hash, region.bounds.max_z);
    }
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "legacy-%016llx",
                  static_cast<unsigned long long>(hash));
    return buffer;
}

bool validTransition(BuildImportUndoRecordState from, BuildImportUndoRecordState to) {
    return (from == BuildImportUndoRecordState::Available &&
            (to == BuildImportUndoRecordState::Claimed ||
             to == BuildImportUndoRecordState::Consumed)) ||
           (from == BuildImportUndoRecordState::Claimed &&
            (to == BuildImportUndoRecordState::Available ||
             to == BuildImportUndoRecordState::Consumed));
}

bool replaceFileAtomically(const std::string& source, const std::string& destination) {
#if defined(_WIN32)
    return MoveFileExA(source.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    return std::rename(source.c_str(), destination.c_str()) == 0;
#endif
}

bool removeIfPresent(const std::string& path) {
    errno = 0;
    return std::remove(path.c_str()) == 0 || errno == ENOENT;
}

}  // namespace

std::string BuildImportUndoStore::filePath(const std::string& directory) {
    if (directory.empty()) return {};
    const char last = directory.back();
    if (last == '/' || last == '\\') return directory + kFileName;
    return directory + "/" + kFileName;
}

bool BuildImportUndoStore::buildNormalizedRegions(
        const std::vector<BlockBounds>& imported_bounds,
        const std::optional<BlockBounds>& deny_foundation,
        std::vector<BuildImportUndoRegion>* regions,
        std::string* error) {
    if (error) error->clear();
    if (!regions) {
        if (error) *error = "undo region output is missing";
        return false;
    }
    regions->clear();
    std::map<ChunkCoord, BlockBounds> normalized;
    for (const BlockBounds& bounds : imported_bounds) {
        if (!bounds.isValid()) continue;
        const ChunkCoord minimum = chunkForBlock(
            bounds.min_x, bounds.min_z, ImportConfig::kVanillaChunkSize);
        const ChunkCoord maximum = chunkForBlock(
            bounds.max_x, bounds.max_z, ImportConfig::kVanillaChunkSize);
        for (int64_t chunk_z = minimum.z; chunk_z <= maximum.z; ++chunk_z) {
            const int64_t chunk_min_z = chunk_z * ImportConfig::kVanillaChunkSize;
            const int64_t chunk_max_z = chunk_min_z + ImportConfig::kVanillaChunkSize - 1;
            for (int64_t chunk_x = minimum.x; chunk_x <= maximum.x; ++chunk_x) {
                const int64_t chunk_min_x = chunk_x * ImportConfig::kVanillaChunkSize;
                const int64_t chunk_max_x = chunk_min_x + ImportConfig::kVanillaChunkSize - 1;
                const BlockBounds intersection{
                    static_cast<int32_t>(std::max<int64_t>(bounds.min_x, chunk_min_x)),
                    bounds.min_y,
                    static_cast<int32_t>(std::max<int64_t>(bounds.min_z, chunk_min_z)),
                    static_cast<int32_t>(std::min<int64_t>(bounds.max_x, chunk_max_x)),
                    bounds.max_y,
                    static_cast<int32_t>(std::min<int64_t>(bounds.max_z, chunk_max_z)),
                };
                const ChunkCoord coord{static_cast<int32_t>(chunk_x),
                                       static_cast<int32_t>(chunk_z)};
                const auto inserted = normalized.emplace(coord, intersection);
                if (!inserted.second) {
                    BlockBounds& existing = inserted.first->second;
                    existing.min_x = std::min(existing.min_x, intersection.min_x);
                    existing.min_y = std::min(existing.min_y, intersection.min_y);
                    existing.min_z = std::min(existing.min_z, intersection.min_z);
                    existing.max_x = std::max(existing.max_x, intersection.max_x);
                    existing.max_y = std::max(existing.max_y, intersection.max_y);
                    existing.max_z = std::max(existing.max_z, intersection.max_z);
                }
                if (normalized.size() > kMaximumRegionCount) {
                    if (error) *error = "completed import spans too many undo chunks";
                    return false;
                }
            }
        }
    }
    if (normalized.empty()) {
        if (error) *error = "completed import has no undo regions";
        return false;
    }

    if (deny_foundation) {
        const BlockBounds& foundation = *deny_foundation;
        if (!foundation.isValid() || foundation.min_y != foundation.max_y ||
            foundation.max_y == std::numeric_limits<int32_t>::max()) {
            if (error) *error = "completed import has invalid deny-foundation bounds";
            return false;
        }
        const ChunkCoord minimum = chunkForBlock(
            foundation.min_x, foundation.min_z, ImportConfig::kVanillaChunkSize);
        const ChunkCoord maximum = chunkForBlock(
            foundation.max_x, foundation.max_z, ImportConfig::kVanillaChunkSize);
        for (int64_t chunk_z = minimum.z; chunk_z <= maximum.z; ++chunk_z) {
            const int64_t chunk_min_z = chunk_z * ImportConfig::kVanillaChunkSize;
            const int64_t chunk_max_z = chunk_min_z + ImportConfig::kVanillaChunkSize - 1;
            for (int64_t chunk_x = minimum.x; chunk_x <= maximum.x; ++chunk_x) {
                const int64_t chunk_min_x = chunk_x * ImportConfig::kVanillaChunkSize;
                const int64_t chunk_max_x = chunk_min_x + ImportConfig::kVanillaChunkSize - 1;
                const BlockBounds expected{
                    static_cast<int32_t>(std::max<int64_t>(foundation.min_x, chunk_min_x)),
                    foundation.min_y,
                    static_cast<int32_t>(std::max<int64_t>(foundation.min_z, chunk_min_z)),
                    static_cast<int32_t>(std::min<int64_t>(foundation.max_x, chunk_max_x)),
                    foundation.max_y,
                    static_cast<int32_t>(std::min<int64_t>(foundation.max_z, chunk_max_z)),
                };
                const ChunkCoord coord{static_cast<int32_t>(chunk_x),
                                       static_cast<int32_t>(chunk_z)};
                const auto found = normalized.find(coord);
                if (found == normalized.end() ||
                    found->second.min_x != expected.min_x ||
                    found->second.max_x != expected.max_x ||
                    found->second.min_z != expected.min_z ||
                    found->second.max_z != expected.max_z ||
                    found->second.min_y != foundation.max_y + 1) {
                    if (error) {
                        *error = "deny foundation does not match the completed import footprint";
                    }
                    return false;
                }
                found->second.min_y = foundation.min_y;
            }
        }
    }

    regions->reserve(normalized.size());
    for (const auto& entry : normalized) {
        regions->push_back({entry.first, entry.second});
    }
    return true;
}

bool BuildImportUndoStore::saveAtomically(const std::string& directory,
                                          const BuildImportUndoManifest& manifest,
                                          std::string* error) {
    if (error) error->clear();
    const std::string path = filePath(directory);
    if (path.empty()) {
        if (error) *error = "undo storage directory is empty";
        return false;
    }
    if (!validateManifest(manifest, error)) return false;

    const std::string temporary_path = path + ".tmp";
    std::ofstream stream(temporary_path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error) *error = "cannot open undo temporary file";
        return false;
    }

    bool success = writeUint32(stream, kMagic) &&
        writeUint32(stream, kBuildImportUndoStoreCurrentVersion) &&
        writeString(stream, manifest.world.world_id) &&
        writeInt32(stream, manifest.world.dimension_id) &&
        writeInt32(stream, manifest.chunk_size) &&
        writeInt32(stream, manifest.simulation_chunk_range) &&
        writeInt32(stream, manifest.blocks_per_second) &&
        writeUint32(stream, static_cast<uint32_t>(manifest.regions.size()));
    for (const BuildImportUndoRegion& region : manifest.regions) {
        if (!success) break;
        success = writeInt32(stream, region.coord.x) &&
            writeInt32(stream, region.coord.z) && writeBounds(stream, region.bounds);
    }
    if (success) {
        success = writeString(stream, manifest.record_id) &&
            writeUint32(stream, static_cast<uint32_t>(manifest.state)) &&
            writeString(stream, manifest.claim_job_id);
    }
    stream.flush();
    success = success && static_cast<bool>(stream);
    stream.close();
    success = success && !stream.fail();

    if (!success) {
        std::remove(temporary_path.c_str());
        if (error) *error = "cannot write undo manifest";
        return false;
    }
    if (!replaceFileAtomically(temporary_path, path)) {
        std::remove(temporary_path.c_str());
        if (error) *error = "cannot atomically replace undo manifest";
        return false;
    }
    return true;
}

std::optional<BuildImportUndoManifest> BuildImportUndoStore::load(
        const std::string& directory, std::string* error) {
    if (error) error->clear();
    const std::string path = filePath(directory);
    if (path.empty()) {
        if (error) *error = "undo storage directory is empty";
        return std::nullopt;
    }

    std::ifstream stream(path, std::ios::binary);
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t region_count = 0;
    BuildImportUndoManifest manifest;
    bool success = stream && readUint32(stream, &magic) && readUint32(stream, &version) &&
        magic == kMagic && (version == 1U || version == kBuildImportUndoStoreCurrentVersion) &&
        readString(stream, kMaximumWorldIdLength, &manifest.world.world_id) &&
        readInt32(stream, &manifest.world.dimension_id) &&
        readInt32(stream, &manifest.chunk_size) &&
        readInt32(stream, &manifest.simulation_chunk_range) &&
        readInt32(stream, &manifest.blocks_per_second) &&
        readUint32(stream, &region_count) &&
        region_count > 0 && region_count <= kMaximumRegionCount;

    if (success) manifest.regions.reserve(region_count);
    for (uint32_t index = 0; success && index < region_count; ++index) {
        BuildImportUndoRegion region;
        success = readInt32(stream, &region.coord.x) &&
            readInt32(stream, &region.coord.z) && readBounds(stream, &region.bounds);
        if (success) manifest.regions.push_back(std::move(region));
    }

    if (success && version >= 2U) {
        uint32_t state = 0;
        success = readString(stream, kMaximumRecordIdLength, &manifest.record_id) &&
            readUint32(stream, &state) &&
            readString(stream, kMaximumClaimJobIdLength, &manifest.claim_job_id);
        manifest.state = static_cast<BuildImportUndoRecordState>(state);
    } else if (success) {
        manifest.record_id = legacyRecordId(manifest);
        manifest.state = BuildImportUndoRecordState::Available;
        manifest.claim_job_id.clear();
    }

    const bool has_trailing_data = success &&
        stream.peek() != std::char_traits<char>::eof();
    if (!success || has_trailing_data || !validateManifest(manifest, nullptr)) {
        if (error) *error = "undo manifest is missing, corrupt, or unsupported";
        return std::nullopt;
    }
    return manifest;
}

BuildImportUndoTransitionResult BuildImportUndoStore::transitionState(
        const std::string& directory, const std::string& record_id,
        BuildImportUndoRecordState expected_state,
        const std::string& expected_claim_job_id,
        BuildImportUndoRecordState next_state,
        const std::string& next_claim_job_id,
        std::string* error) {
    if (error) error->clear();
    if (!validTransition(expected_state, next_state)) {
        if (error) *error = "invalid undo record lifecycle transition";
        return BuildImportUndoTransitionResult::Mismatch;
    }
    std::string load_error;
    std::optional<BuildImportUndoManifest> manifest = load(directory, &load_error);
    if (!manifest) {
        if (error) *error = load_error;
        return BuildImportUndoTransitionResult::MissingOrInvalid;
    }
    if (manifest->record_id != record_id || manifest->state != expected_state ||
        manifest->claim_job_id != expected_claim_job_id) {
        if (error) *error = "undo record identity or lifecycle state changed";
        return BuildImportUndoTransitionResult::Mismatch;
    }
    manifest->state = next_state;
    manifest->claim_job_id = next_claim_job_id;
    std::string write_error;
    if (!saveAtomically(directory, *manifest, &write_error)) {
        if (error) *error = write_error;
        return BuildImportUndoTransitionResult::WriteFailed;
    }
    return BuildImportUndoTransitionResult::Updated;
}

bool BuildImportUndoStore::remove(const std::string& directory, std::string* error) {
    if (error) error->clear();
    const std::string path = filePath(directory);
    if (path.empty()) {
        if (error) *error = "undo storage directory is empty";
        return false;
    }
    if (!removeIfPresent(path) || !removeIfPresent(path + ".tmp")) {
        if (error) *error = "cannot remove undo manifest";
        return false;
    }
    return true;
}

}  // namespace build_import
