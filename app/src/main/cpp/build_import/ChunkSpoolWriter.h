#ifndef INFINITE_TEXTURE_CHUNK_SPOOL_WRITER_H
#define INFINITE_TEXTURE_CHUNK_SPOOL_WRITER_H

#include "BlockMapper.h"

#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace build_import {

struct ParsedBlock {
    int32_t world_x = 0;
    int32_t world_y = 0;
    int32_t world_z = 0;
    BlockSpec spec;
};

// Writes compact native records by chunk and phase. The reader/merger can
// replay one spool at a time, keeping blueprint memory bounded.
class ChunkSpoolWriter {
public:
    using CancelCheck = std::function<bool()>;
    using ProgressCallback = std::function<void(uint64_t completed, uint64_t total)>;

    ChunkSpoolWriter(std::string directory, int32_t chunk_size,
                     size_t maximum_chunk_descriptors = 0);
    ~ChunkSpoolWriter();

    // Registers the complete source volume, partitioned by X/Z chunk. This
    // records overwrite bounds for air-only areas without writing air blocks
    // to placement spools.
    bool includeVolume(const BlockBounds& world_bounds, std::string* error = nullptr,
                       const CancelCheck& cancel_check = {});
    // Registers only missing X/Z partitions for an auxiliary footprint. Unlike
    // includeVolume(), pre-existing descriptors retain their own source bounds
    // so a sparse multi-region source does not widen its normal clear or
    // verification volume just to support a horizontal auxiliary layer.
    bool includeMissingFootprint(const BlockBounds& world_bounds,
                                 std::string* error = nullptr,
                                 const CancelCheck& cancel_check = {});
    bool append(const ParsedBlock& block, std::string* error = nullptr);
    std::vector<ChunkDescriptor> finish(std::string* error = nullptr,
                                        const CancelCheck& cancel_check = {},
                                        const ProgressCallback& progress_callback = {});
    void discard();
    bool usedStagedRouting() const { return staged_routing_; }
    uint64_t reopenedSpoolCount() const { return reopened_spool_count_; }

private:
    struct OpenSpool;
    struct RouteMetadata {
        // Exact keys are only needed until the route is known to require
        // another partitioning level; saturated buckets stop paying set costs.
        std::set<std::pair<ChunkCoord, ImportPhase>> unique_routes;
        ChunkCoord last_route_coord{};
        ImportPhase last_route_phase = ImportPhase::Structure;
        bool exceeds_open_spool_limit = false;
        bool has_last_route = false;
    };

    std::string spoolPath(const ChunkCoord& coord, ImportPhase phase) const;
    std::string routingPath(uint32_t depth, uint64_t group, uint32_t bucket) const;
    bool includeVolumeInternal(const BlockBounds& world_bounds,
                               bool extend_existing_bounds,
                               std::string* error,
                               const CancelCheck& cancel_check);
    ChunkDescriptor* descriptorFor(const ChunkCoord& coord);
    OpenSpool* getOrCreate(const ChunkCoord& coord, ImportPhase phase,
                           const std::string& path, std::string* error);
    bool ensureDirectoryReady(std::string* error);
    bool switchToStagedRouting(std::string* error);
    bool appendStaged(const ParsedBlock& block, const ChunkCoord& coord,
                      uint32_t name_id, uint8_t flags, std::string* error);
    bool routeStagedFile(const std::string& path, uint32_t depth,
                         const RouteMetadata& metadata,
                         const CancelCheck& cancel_check, std::string* error);
    static void rememberRoute(RouteMetadata* metadata, const ChunkCoord& coord,
                              ImportPhase phase);
    bool closeOpenSpools(std::string* error);
    bool paletteIdFor(const std::string& name, uint32_t* id, std::string* error);
    bool writePalette(std::string* error);
    std::string palettePath() const;

    std::string directory_;
    int32_t chunk_size_;
    size_t maximum_chunk_descriptors_ = 0;
    std::map<ChunkCoord, ChunkDescriptor> descriptors_;
    std::map<std::pair<ChunkCoord, ImportPhase>, OpenSpool> open_spools_;
    std::map<uint32_t, OpenSpool> routing_buckets_;
    // Bound simultaneously open file descriptors even for a blueprint that
    // touches thousands of chunks. Evicted spools are reopened in append mode.
    std::set<std::string> initialized_paths_;
    std::set<std::string> routing_paths_;
    std::unordered_map<std::string, uint32_t> palette_indices_;
    std::vector<std::string> palette_names_;
    uint64_t access_serial_ = 0;
    uint64_t routing_group_serial_ = 1;
    uint64_t reopened_spool_count_ = 0;
    ChunkDescriptor* cached_descriptor_ = nullptr;
    ChunkCoord cached_descriptor_coord_{};
    OpenSpool* cached_open_spool_ = nullptr;
    ChunkCoord cached_open_spool_coord_{};
    ImportPhase cached_open_spool_phase_ = ImportPhase::Structure;
    OpenSpool* cached_route_ = nullptr;
    uint32_t cached_route_bucket_ = 0;
    bool has_cached_descriptor_ = false;
    bool has_cached_open_spool_ = false;
    bool has_cached_route_ = false;
    bool directory_ready_ = false;
    bool staged_routing_ = false;
    bool finished_ = false;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_CHUNK_SPOOL_WRITER_H
