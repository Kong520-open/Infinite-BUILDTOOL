#ifndef INFINITE_TEXTURE_BUILD_IMPORT_CHECKPOINT_H
#define INFINITE_TEXTURE_BUILD_IMPORT_CHECKPOINT_H

#include "BuildImportTypes.h"

#include <optional>
#include <string>

namespace build_import {

constexpr uint32_t kBuildImportCheckpointCurrentVersion = 13;
constexpr uint32_t kBuildImportCheckpointOldestSupportedVersion = 4;
// Version 6 introduced the current global phase scheduler. Version 7 adds
// region-grid persistence, version 8 appends final-verification settings,
// version 9 persists block-state degradation notices, version 10 adds the
// exact native simulation-chunk work-unit range, version 11 appends the
// optional deny-layer setting and its resolved one-block-high bounds, and
// version 12 persists the source/access profile used for restricted imports;
// version 13 persists the pixel-art automatic-map request.
// Neither change invalidates the v6 scheduler cursor.
constexpr uint32_t kBuildImportCheckpointCurrentSchedulerVersion = 6;

struct CheckpointSnapshot {
    // In-memory snapshots use the current scheduler semantics. A loaded older
    // snapshot retains its source version so restoration can migrate safely.
    uint32_t format_version = kBuildImportCheckpointCurrentVersion;
    ImportIdentity identity;
    WorldContext world;
    ImportConfig config;
    ImportState state = ImportState::Idle;
    uint32_t phase_index = 0;
    uint32_t chunk_index = 0;
    bool has_active_unit = false;
    ChunkCoord active_coord;
    ImportPhase active_phase = ImportPhase::Structure;
    uint64_t active_spool_offset = 0;
    uint64_t completed_command_count = 0;
    uint64_t completed_block_count = 0;
    uint64_t verification_sample_index = 0;
    std::string degradation_notice;
};

class BuildImportCheckpoint {
public:
    static bool saveAtomically(const std::string& path, const CheckpointSnapshot& snapshot,
                               std::string* error = nullptr);
    static std::optional<CheckpointSnapshot> load(const std::string& path,
                                                  std::string* error = nullptr);
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BUILD_IMPORT_CHECKPOINT_H
