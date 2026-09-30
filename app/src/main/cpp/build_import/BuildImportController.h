#ifndef INFINITE_TEXTURE_BUILD_IMPORT_CONTROLLER_H
#define INFINITE_TEXTURE_BUILD_IMPORT_CONTROLLER_H

#include "BuildImportCheckpoint.h"
#include "CommandSpool.h"

#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace build_import {

struct BuildImportRuntimeMetadata {
    int32_t chunk_size = ImportConfig::kVanillaChunkSize;
    int32_t simulation_chunk_range = ImportConfig::kDefaultSimulationChunkRange;
    // Cached once by the controller so every runtime path (execution,
    // verification, and repair) uses the same core work-unit side length.
    int32_t region_span = ImportConfig::kDefaultSimulationChunkRange;
    // Legacy checkpoint diagnostic only. New code must use region_span.
    int32_t chunk_load_radius = 1;
    int32_t chunk_wait_ticks = 60;
    ChunkCoord region_grid_origin;
    int32_t ticking_area_min_y = 0;
    int32_t ticking_area_max_y = 255;
    OverwritePolicy overwrite_policy = OverwritePolicy::PreserveExisting;
    bool verify_after_import = true;
    VerificationPrecision verification_precision = VerificationPrecision::Thorough;
    uint32_t phase_index = 0;
    uint64_t verification_sample_index = 0;
};

class BuildImportController {
public:
    bool startPlanning(ImportIdentity identity, WorldContext world, ImportConfig config,
                       std::string* error = nullptr);
    // Rebuild a paused scheduler after process restart from persistent spools.
    bool restoreFromCheckpoint(const CheckpointSnapshot& snapshot,
                               std::vector<ChunkDescriptor> descriptors,
                               std::string* error = nullptr);
    bool setPlanningDegradationNotice(std::string notice, std::string* error = nullptr);
    // The parser resolves the global source volume after startPlanning has
    // already written its initial checkpoint. Keep the optional deny layer
    // mutable only while the plan is being assembled, then persist it before
    // command spools are admitted.
    bool setPlanningDenyLayerBounds(BlockBounds bounds, std::string* error = nullptr);
    bool addChunk(ChunkDescriptor descriptor, std::string* error = nullptr);
    bool finishPlanning(std::string* error = nullptr);

    // The caller prepares the returned load area, reads spool_path, emits the
    // phase commands, and marks the unit complete only after it has drained.
    std::optional<WorkUnit> acquireNextUnit(std::string* error = nullptr);
    // Inspect the normalized scheduler cursor without allocating a sequence,
    // activating a unit, or touching the durable checkpoint.
    std::optional<WorkUnit> peekNextUnit() const;
    bool completeActiveUnit(uint64_t imported_blocks = 0, std::string* error = nullptr);
    // Clear and Structure units may be acknowledged as a group. Their cursor
    // remains memory-only until commitDeferredUnits succeeds atomically.
    bool completeActiveUnitDeferred(uint64_t imported_blocks = 0,
                                    std::string* error = nullptr);
    bool commitDeferredUnits(std::string* error = nullptr);
    void rollbackDeferredUnits() noexcept;
    bool hasDeferredUnits() const;
    bool failActiveUnit(std::string reason, std::string* error = nullptr);
    // Track fully issued placement commands for diagnostics. Durable recovery
    // deliberately replays the complete active partition.
    void recordActiveProgress(uint64_t completed_commands);
    // Rewind the active unit without inflating the completed-command counter.
    // Used when asynchronous command output reports a failure after batching.
    bool rewindActiveProgress(std::string* error = nullptr);
    bool recordVerificationProgress(uint64_t sample_index, bool persist,
                                    std::string* error = nullptr);
    bool finishVerification(bool success, std::string reason = {},
                            std::string* error = nullptr);

    bool pause(std::string* error = nullptr);
    void emergencyPauseNoCheckpoint() noexcept;
    bool resume(const WorldContext& current_world, std::string* error = nullptr);
    void cancel(bool delete_checkpoint = true);
    bool onWorldContextChanged(const WorldContext& current_world,
                               std::string* error = nullptr);

    ImportState state() const;
    std::string failureReason() const;
    bool worldMatches(const WorldContext& world) const;
    uint64_t completedBlockCount() const;
    BuildImportRuntimeMetadata runtimeMetadata() const;
    CheckpointSnapshot checkpointSnapshot() const;
    std::optional<CheckpointSnapshot> loadCheckpoint(std::string* error = nullptr) const;

    std::vector<std::string> makePrepareCommands(const WorkUnit& unit) const;
    // When the caller knows the local player position, a long approach is
    // split into bounded /tp hops so servers that reject long-distance
    // teleports still move the player to the region centre.
    std::vector<std::string> makePrepareCommands(const WorkUnit& unit,
                                                 bool has_player_position,
                                                 int32_t player_x,
                                                 int32_t player_z) const;
    std::optional<std::string> makeClearCommand(const WorkUnit& unit) const;
    std::vector<std::string> makeClearCommands(const WorkUnit& unit) const;
    std::vector<PlannedCommand> makeClearPlan(const WorkUnit& unit) const;
    std::vector<PlannedCommand> makeDenyFoundationPlan(const WorkUnit& unit) const;
    std::string makeCleanupCommand(const WorkUnit& unit) const;

private:
    CheckpointSnapshot checkpointSnapshotLocked() const;
    bool persistCheckpointLocked(std::string* error = nullptr) const;
    bool checkpointInterruptionLocked(ImportState target_state,
                                      std::string reason,
                                      std::string* error = nullptr);
    bool validateConfig(const ImportConfig& config, std::string* error,
                        bool allow_legacy_checkpoint = false) const;
    void sortChunksLocked();
    void advanceLocked();
    void normalizeCursorLocked();
    std::optional<WorkUnit> createUnitLocked();
    std::optional<WorkUnit> makeUnitForCursorLocked(uint64_t sequence) const;
    void rollbackDeferredUnitsLocked() noexcept;
    static BlockBounds loadBoundsFor(const ChunkCoord& region_coord, int32_t region_span,
                                     const ChunkCoord& region_grid_origin,
                                     const ImportConfig& config);
    static std::string areaName(const std::string& job_id, const ChunkCoord& region_coord);

    mutable std::mutex mutex_;
    ImportIdentity identity_;
    WorldContext world_;
    ImportConfig config_;
    std::vector<ChunkDescriptor> chunks_;
    // Duplicate detection must not linearly rescan every preceding partition:
    // large pixel walls can contain tens of thousands of logical chunks.
    // The index exists only while descriptors are being admitted.
    std::set<ChunkCoord> planning_chunk_coords_;
    ImportState state_ = ImportState::Idle;
    std::string failure_reason_;
    std::string degradation_notice_;
    uint32_t phase_index_ = 0;
    uint32_t chunk_index_ = 0;
    uint64_t next_sequence_ = 1;
    uint64_t completed_command_count_ = 0;
    uint64_t completed_block_count_ = 0;
    uint64_t active_spool_offset_ = 0;
    uint64_t verification_sample_index_ = 0;
    std::optional<WorkUnit> active_unit_;
    struct DeferredBaseline {
        ImportState state = ImportState::Idle;
        std::string failure_reason;
        uint32_t phase_index = 0;
        uint32_t chunk_index = 0;
        uint64_t next_sequence = 1;
        uint64_t completed_command_count = 0;
        uint64_t completed_block_count = 0;
        uint64_t verification_sample_index = 0;
    };
    std::optional<DeferredBaseline> deferred_baseline_;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BUILD_IMPORT_CONTROLLER_H
