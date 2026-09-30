#include "BuildImportController.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <limits>

namespace build_import {
namespace {

constexpr uint32_t kPreDependentPhaseCount = 5;

// Some servers reject a single /tp spanning more than an allowed distance
// ("teleported too far"). A long approach to a region centre is therefore
// emitted as a chain of bounded hops; the command cap keeps one prepare batch
// small even for extreme travel distances.
constexpr int32_t kTeleportHopBlocks = 96;
constexpr int64_t kMaximumTeleportHopCommands = 512;

bool usesSerpentineRegionOrder(const std::string& options_hash) {
    return options_hash == "clear-region-command-plan-v7" ||
           options_hash == "preserve-region-command-plan-v7" ||
           options_hash == "clear-region-grid-v8" ||
           options_hash == "preserve-region-grid-v8" ||
           options_hash == "clear-region-grid-v9" ||
           options_hash == "preserve-region-grid-v9";
}

bool usesAdaptiveRegionGrid(const std::string& options_hash) {
    return options_hash == "clear-region-grid-v8" ||
           options_hash == "preserve-region-grid-v8" ||
           options_hash == "clear-region-grid-v9" ||
           options_hash == "preserve-region-grid-v9";
}

bool descriptorOrderLess(const ChunkDescriptor& left, const ChunkDescriptor& right,
                         int32_t span, const ChunkCoord& region_grid_origin,
                         bool serpentine) {
    // v6 checkpoints persisted a cursor into the original global (z, x)
    // ordering. Region grouping is a v7 plan property; applying it while
    // restoring a legacy plan can move that cursor to a different partition.
    if (!serpentine) return left.coord < right.coord;
    const ChunkCoord left_region = regionForChunk(left.coord, span, region_grid_origin);
    const ChunkCoord right_region = regionForChunk(right.coord, span, region_grid_origin);
    if (left_region.z != right_region.z) return left_region.z < right_region.z;
    if (left_region.x != right_region.x) {
        const bool reverse_row =
            (static_cast<uint32_t>(left_region.z) & 1U) != 0;
        return reverse_row ? left_region.x > right_region.x
                           : left_region.x < right_region.x;
    }
    if (left.coord.z != right.coord.z) return left.coord.z < right.coord.z;
    return left.coord.x < right.coord.x;
}

int32_t clampToInt32(int64_t value) {
    if (value < std::numeric_limits<int32_t>::min()) return std::numeric_limits<int32_t>::min();
    if (value > std::numeric_limits<int32_t>::max()) return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(value);
}

bool phaseHasWork(const ChunkDescriptor& descriptor, uint32_t phase_index) {
    return phase_index > phaseIndex(ImportPhase::Clear) &&
           phase_index < kImportPhaseCount &&
           hasSpoolForPhase(descriptor, static_cast<ImportPhase>(phase_index));
}

bool hasAnyPlacementWork(const ChunkDescriptor& descriptor) {
    for (const ImportPhase phase : kPlacementPhaseOrder) {
        if (phaseHasWork(descriptor, static_cast<uint32_t>(phaseIndex(phase)))) return true;
    }
    return false;
}

bool isClearOnlyChunk(const ChunkDescriptor& descriptor, OverwritePolicy policy) {
    return policy == OverwritePolicy::ClearImportedBounds &&
           descriptor.imported_bounds.isValid() && !hasAnyPlacementWork(descriptor);
}

bool intersectsDenyFoundation(const ChunkDescriptor& descriptor,
                              const ImportConfig& config) {
    if (!config.place_deny_layer || !config.deny_layer_bounds.isValid() ||
        !descriptor.imported_bounds.isValid()) {
        return false;
    }
    const BlockBounds& foundation = config.deny_layer_bounds;
    const BlockBounds& imported = descriptor.imported_bounds;
    return foundation.min_x <= imported.max_x && foundation.max_x >= imported.min_x &&
           foundation.min_z <= imported.max_z && foundation.max_z >= imported.min_z;
}

bool isFoundationOnlyChunk(const ChunkDescriptor& descriptor,
                           const ImportConfig& config) {
    return intersectsDenyFoundation(descriptor, config) &&
           !hasAnyPlacementWork(descriptor);
}

bool isAuxiliaryOnlyChunk(const ChunkDescriptor& descriptor,
                          const ImportConfig& config) {
    return isClearOnlyChunk(descriptor, config.overwrite_policy) ||
           isFoundationOnlyChunk(descriptor, config);
}

bool isFirstPlacementWork(const ChunkDescriptor& descriptor, ImportPhase phase) {
    if (!hasSpoolForPhase(descriptor, phase)) return false;
    for (const ImportPhase candidate : kPlacementPhaseOrder) {
        if (candidate == phase) return true;
        if (hasSpoolForPhase(descriptor, candidate)) return false;
    }
    return false;
}

bool isReplayUnsafePhase(ImportPhase phase) {
    return phase == ImportPhase::Gravity || phase == ImportPhase::Attachment ||
           phase == ImportPhase::DependentAttachment || phase == ImportPhase::Fluid;
}

bool requiresDurableAcquireCheckpoint(ImportPhase phase) {
    // Clear and Structure commands are idempotent. The previously committed
    // cursor already points at this unit, so a hard process exit safely replays
    // it from the beginning. Dynamic and attachment phases keep the active-unit
    // marker because replaying their side effects without rollback is unsafe.
    return phase != ImportPhase::Clear && phase != ImportPhase::Structure;
}

bool hasCompletedReplayUnsafeWork(const std::vector<ChunkDescriptor>& descriptors,
                                  uint32_t phase_index, uint32_t chunk_index) {
    if (phase_index >= kImportPhaseCount) return false;
    const size_t cursor_order = placementOrderIndex(static_cast<ImportPhase>(phase_index));
    if (cursor_order >= kPlacementPhaseOrder.size()) return false;
    for (size_t order = 0; order <= cursor_order; ++order) {
        const ImportPhase phase = kPlacementPhaseOrder[order];
        if (!isReplayUnsafePhase(phase)) continue;
        const size_t limit = order < cursor_order
            ? descriptors.size() : std::min<size_t>(chunk_index, descriptors.size());
        for (size_t chunk = 0; chunk < limit; ++chunk) {
            if (phaseHasWork(descriptors[chunk], static_cast<uint32_t>(phaseIndex(phase)))) {
                return true;
            }
        }
    }
    return false;
}

bool cursorHasWork(const ChunkDescriptor& descriptor, uint32_t phase_index,
                   const ImportConfig& config) {
    if (phaseHasWork(descriptor, phase_index)) return true;
    // A schematic volume may contain a completely empty logical partition.
    // Schedule its auxiliary work at the Structure cursor so overwrite
    // reproduces air and the optional deny foundation still covers empty
    // source-volume partitions without introducing a global sweep.
    return phase_index == phaseIndex(ImportPhase::Structure) &&
           isAuxiliaryOnlyChunk(descriptor, config);
}

bool completedBlocksBeforeCursor(const std::vector<ChunkDescriptor>& descriptors,
                                 uint32_t phase_index, uint32_t chunk_index,
                                 bool terminal, uint64_t* output) {
    if (!output) return false;
    const size_t cursor_order = terminal ? kPlacementPhaseOrder.size() :
        placementOrderIndex(static_cast<ImportPhase>(phase_index));
    if (!terminal && cursor_order >= kPlacementPhaseOrder.size()) return false;
    uint64_t total = 0;
    for (size_t order = 0; order < kPlacementPhaseOrder.size(); ++order) {
        const uint32_t phase = static_cast<uint32_t>(phaseIndex(kPlacementPhaseOrder[order]));
        const size_t limit = terminal || order < cursor_order
            ? descriptors.size() : (order == cursor_order ? chunk_index : 0);
        for (size_t chunk = 0; chunk < limit; ++chunk) {
            if (!phaseHasWork(descriptors[chunk], phase)) continue;
            const uint64_t count = descriptors[chunk].phase_block_counts[phase];
            if (count > std::numeric_limits<uint64_t>::max() - total) return false;
            total += count;
        }
    }
    *output = total;
    return true;
}

bool firstPlacementCursor(const std::vector<ChunkDescriptor>& descriptors,
                           const ImportConfig& config,
                           uint32_t* phase_index, uint32_t* chunk_index) {
    if (!phase_index || !chunk_index) return false;
    for (const ImportPhase ordered_phase : kPlacementPhaseOrder) {
        const uint32_t phase = static_cast<uint32_t>(phaseIndex(ordered_phase));
        for (uint32_t chunk = 0; chunk < descriptors.size(); ++chunk) {
            if (!cursorHasWork(descriptors[chunk], phase, config)) continue;
            *phase_index = phase;
            *chunk_index = chunk;
            return true;
        }
    }
    return false;
}

}  // namespace

bool BuildImportController::startPlanning(ImportIdentity identity, WorldContext world, ImportConfig config,
                                          std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (identity.job_id.empty() || identity.source_file.empty() || world.world_id.empty()) {
        if (error) *error = "job id, source file, and world id are required";
        return false;
    }
    if (!validateConfig(config, error)) return false;

    CheckpointSnapshot initial_snapshot;
    initial_snapshot.identity = identity;
    initial_snapshot.world = world;
    initial_snapshot.config = config;
    initial_snapshot.state = ImportState::Planning;
    if (!BuildImportCheckpoint::saveAtomically(config.checkpoint_path,
                                               initial_snapshot, error)) {
        return false;
    }

    identity_ = std::move(identity);
    world_ = std::move(world);
    config_ = std::move(config);
    chunks_.clear();
    planning_chunk_coords_.clear();
    state_ = ImportState::Planning;
    failure_reason_.clear();
    degradation_notice_.clear();
    phase_index_ = 0;
    chunk_index_ = 0;
    next_sequence_ = 1;
    completed_command_count_ = 0;
    completed_block_count_ = 0;
    active_spool_offset_ = 0;
    verification_sample_index_ = 0;
    active_unit_.reset();
    deferred_baseline_.reset();
    return true;
}

bool BuildImportController::restoreFromCheckpoint(const CheckpointSnapshot& snapshot,
                                                  std::vector<ChunkDescriptor> descriptors,
                                                  std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot.format_version < kBuildImportCheckpointOldestSupportedVersion ||
        snapshot.format_version > kBuildImportCheckpointCurrentVersion ||
        snapshot.identity.job_id.empty() || snapshot.world.world_id.empty() || descriptors.empty() ||
        !validateConfig(snapshot.config, error, true)) {
        if (error && error->empty()) *error = "invalid checkpoint restoration data";
        return false;
    }
    const bool legacy_format =
        snapshot.format_version < kBuildImportCheckpointCurrentSchedulerVersion;
    const bool legacy_terminal_position = legacy_format &&
        snapshot.chunk_index == 0 && snapshot.phase_index == kPreDependentPhaseCount &&
        snapshot.state != ImportState::Idle && snapshot.state != ImportState::Planning;
    const bool verification_position =
        snapshot.chunk_index == descriptors.size() && snapshot.phase_index == 0 &&
        (snapshot.state == ImportState::Verifying || snapshot.state == ImportState::Completed ||
         snapshot.state == ImportState::Failed ||
         snapshot.state == ImportState::Paused || snapshot.state == ImportState::ClosedForContextChange);
    const int32_t span = regionSpanForImportConfig(snapshot.config);
    const bool serpentine = usesSerpentineRegionOrder(snapshot.identity.options_hash);
    const ChunkCoord region_grid_origin = usesAdaptiveRegionGrid(snapshot.identity.options_hash)
        ? snapshot.config.region_grid_origin : ChunkCoord{};
    for (ChunkDescriptor& descriptor : descriptors) {
        descriptor.region_grid_origin = region_grid_origin;
    }
    std::sort(descriptors.begin(), descriptors.end(),
              [span, region_grid_origin, serpentine](const ChunkDescriptor& left,
                                                      const ChunkDescriptor& right) {
                  return descriptorOrderLess(left, right, span, region_grid_origin, serpentine);
              });

    const bool legacy_replay_position = legacy_format && !verification_position &&
        !legacy_terminal_position &&
        (snapshot.state == ImportState::Running || snapshot.state == ImportState::Paused ||
         snapshot.state == ImportState::ClosedForContextChange ||
         snapshot.state == ImportState::Failed);
    const bool placement_position =
        snapshot.phase_index >= phaseIndex(ImportPhase::Structure) &&
        snapshot.phase_index < kImportPhaseCount &&
        snapshot.chunk_index < descriptors.size() &&
        cursorHasWork(descriptors[snapshot.chunk_index], snapshot.phase_index,
                      snapshot.config) &&
        (snapshot.state == ImportState::Running || snapshot.state == ImportState::Paused ||
         snapshot.state == ImportState::ClosedForContextChange ||
         snapshot.state == ImportState::Failed);
    const bool rollback_position = !legacy_format &&
        snapshot.config.overwrite_policy == OverwritePolicy::ClearImportedBounds &&
        snapshot.phase_index == phaseIndex(ImportPhase::Clear) &&
        snapshot.chunk_index < descriptors.size() &&
        descriptors[snapshot.chunk_index].imported_bounds.isValid() &&
        (snapshot.state == ImportState::Running || snapshot.state == ImportState::Paused ||
         snapshot.state == ImportState::ClosedForContextChange ||
         snapshot.state == ImportState::Failed);
    const bool unsafe_position = !legacy_format &&
        ((snapshot.has_active_unit && isReplayUnsafePhase(snapshot.active_phase)) ||
         hasCompletedReplayUnsafeWork(
             descriptors, snapshot.phase_index, snapshot.chunk_index));
    if (unsafe_position &&
        snapshot.config.overwrite_policy != OverwritePolicy::ClearImportedBounds) {
        if (error) {
            *error = "cannot safely restore an interrupted dynamic-block phase without overwrite";
        }
        return false;
    }
    const bool dynamic_rollback = unsafe_position &&
        snapshot.config.overwrite_policy == OverwritePolicy::ClearImportedBounds;
    const bool valid_terminal_state = verification_position || legacy_terminal_position;
    if (descriptors.size() > std::numeric_limits<uint32_t>::max() ||
        snapshot.state == ImportState::Idle || snapshot.state == ImportState::Planning ||
        (!legacy_replay_position && !placement_position && !rollback_position &&
         !valid_terminal_state) ||
        ((snapshot.state == ImportState::Verifying || snapshot.state == ImportState::Completed) &&
         !valid_terminal_state) ||
        (!legacy_format && snapshot.verification_sample_index != 0 &&
         !valid_terminal_state)) {
        if (error) *error = "checkpoint state and cursor do not match its chunk spool catalog";
        return false;
    }
    if (!legacy_replay_position &&
        snapshot.active_spool_offset > snapshot.completed_command_count) {
        if (error) *error = "checkpoint active command offset exceeds its completed count";
        return false;
    }
    if (rollback_position &&
        (snapshot.completed_block_count != 0 || snapshot.verification_sample_index != 0)) {
        if (error) *error = "dynamic rollback checkpoint contains committed placement progress";
        return false;
    }
    if (snapshot.has_active_unit) {
        const bool clear_only_active = placement_position &&
            snapshot.phase_index == phaseIndex(ImportPhase::Structure) &&
            snapshot.active_phase == ImportPhase::Clear &&
            isAuxiliaryOnlyChunk(descriptors[snapshot.chunk_index], snapshot.config);
        const bool rollback_active = rollback_position &&
            snapshot.active_phase == ImportPhase::Clear &&
            snapshot.active_coord == descriptors[snapshot.chunk_index].coord;
        if (valid_terminal_state ||
            (!legacy_replay_position && placement_position &&
             ((!clear_only_active &&
               phaseIndex(snapshot.active_phase) != snapshot.phase_index) ||
              !(snapshot.active_coord == descriptors[snapshot.chunk_index].coord))) ||
            (rollback_position && !rollback_active)) {
            if (error) *error = "checkpoint active unit does not match its scheduler cursor";
            return false;
        }
    }
    uint64_t expected_completed_blocks = 0;
    const bool terminal = valid_terminal_state;
    if (!legacy_replay_position && !rollback_position &&
        (!completedBlocksBeforeCursor(descriptors, snapshot.phase_index,
                                      snapshot.chunk_index, terminal,
                                      &expected_completed_blocks) ||
         snapshot.completed_block_count != expected_completed_blocks)) {
        if (error) *error = "checkpoint completed block count does not match its cursor";
        return false;
    }

    uint32_t first_phase_index = 0;
    uint32_t first_chunk_index = 0;
    if (legacy_replay_position &&
        !firstPlacementCursor(descriptors, snapshot.config,
                               &first_phase_index, &first_chunk_index)) {
        if (error) *error = "legacy checkpoint has no importable placement spool";
        return false;
    }

    ImportIdentity restored_identity = snapshot.identity;
    WorldContext restored_world = snapshot.world;
    ImportConfig restored_config = snapshot.config;
    restored_config.region_grid_origin = region_grid_origin;
    const uint32_t restored_phase_index = dynamic_rollback
        ? static_cast<uint32_t>(phaseIndex(ImportPhase::Clear))
        : (valid_terminal_state ? 0 :
           (legacy_replay_position ? first_phase_index : snapshot.phase_index));
    const uint32_t restored_chunk_index = valid_terminal_state
        ? static_cast<uint32_t>(descriptors.size())
        : (dynamic_rollback ? 0U :
           (legacy_replay_position ? first_chunk_index : snapshot.chunk_index));
    const uint64_t replayed_command_count = !legacy_replay_position && snapshot.has_active_unit
        ? snapshot.active_spool_offset : 0;
    const uint64_t restored_command_count = legacy_replay_position || dynamic_rollback
        ? 0 : snapshot.completed_command_count - replayed_command_count;
    const uint64_t restored_block_count = legacy_replay_position || dynamic_rollback
        ? 0 : snapshot.completed_block_count;

    // Persist the complete candidate before replacing live scheduler state.
    // After this succeeds the remaining commit consists only of noexcept moves
    // and scalar assignments, so a failed restore cannot destroy the old job.
    CheckpointSnapshot restored_snapshot = snapshot;
    restored_snapshot.identity = restored_identity;
    restored_snapshot.world = restored_world;
    restored_snapshot.config = restored_config;
    restored_snapshot.state = ImportState::Paused;
    restored_snapshot.phase_index = restored_phase_index;
    restored_snapshot.chunk_index = restored_chunk_index;
    restored_snapshot.has_active_unit = false;
    restored_snapshot.active_coord = {};
    restored_snapshot.active_phase = ImportPhase::Structure;
    restored_snapshot.active_spool_offset = 0;
    restored_snapshot.completed_command_count = restored_command_count;
    restored_snapshot.completed_block_count = restored_block_count;
    restored_snapshot.verification_sample_index = legacy_format || dynamic_rollback
        ? 0 : snapshot.verification_sample_index;
    if (!BuildImportCheckpoint::saveAtomically(restored_config.checkpoint_path,
                                               restored_snapshot, error)) {
        return false;
    }

    identity_ = std::move(restored_identity);
    world_ = std::move(restored_world);
    config_ = std::move(restored_config);
    chunks_ = std::move(descriptors);
    planning_chunk_coords_.clear();
    phase_index_ = restored_phase_index;
    chunk_index_ = restored_chunk_index;
    next_sequence_ = 1;
    completed_command_count_ = restored_command_count;
    completed_block_count_ = restored_block_count;
    active_spool_offset_ = 0;
    verification_sample_index_ = legacy_format || dynamic_rollback
        ? 0 : snapshot.verification_sample_index;
    active_unit_.reset();
    deferred_baseline_.reset();
    failure_reason_.clear();
    degradation_notice_ = snapshot.degradation_notice;
    // Resume performs the mandatory world/dimension verification.
    state_ = ImportState::Paused;
    return true;
}

bool BuildImportController::setPlanningDegradationNotice(std::string notice,
                                                           std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ImportState::Planning) {
        if (error) *error = "degradation metadata can only be set during planning";
        return false;
    }
    degradation_notice_ = std::move(notice);
    return true;
}

bool BuildImportController::setPlanningDenyLayerBounds(BlockBounds bounds,
                                                        std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ImportState::Planning) {
        if (error) *error = "deny-layer bounds can only be set during planning";
        return false;
    }
    if (!config_.place_deny_layer) {
        if (error) *error = "deny-layer bounds were supplied while the option is disabled";
        return false;
    }
    if (!bounds.isValid() || bounds.min_y != bounds.max_y) {
        if (error) *error = "deny-layer bounds must be a valid single-height cuboid";
        return false;
    }
    const BlockBounds previous_bounds = config_.deny_layer_bounds;
    config_.deny_layer_bounds = bounds;
    if (persistCheckpointLocked(error)) return true;
    config_.deny_layer_bounds = previous_bounds;
    return false;
}

bool BuildImportController::addChunk(ChunkDescriptor descriptor, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ImportState::Planning) {
        if (error) *error = "chunks can only be added during planning";
        return false;
    }
    if (!descriptor.imported_bounds.isValid()) {
        if (error) *error = "chunk has invalid imported bounds";
        return false;
    }
    const ChunkCoord coord = descriptor.coord;
    if (!planning_chunk_coords_.insert(coord).second) {
        if (error) *error = "duplicate chunk descriptor";
        return false;
    }
    try {
        chunks_.push_back(std::move(descriptor));
    } catch (...) {
        planning_chunk_coords_.erase(coord);
        throw;
    }
    return true;
}

bool BuildImportController::finishPlanning(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ImportState::Planning) {
        if (error) *error = "import is not in planning state";
        return false;
    }
    if (chunks_.empty()) {
        state_ = ImportState::Failed;
        failure_reason_ = "parser produced no importable chunks";
        persistCheckpointLocked(nullptr);
        if (error) *error = failure_reason_;
        return false;
    }
    if (config_.place_deny_layer &&
        (!config_.deny_layer_bounds.isValid() ||
         config_.deny_layer_bounds.min_y != config_.deny_layer_bounds.max_y)) {
        if (error) *error = "enabled deny layer has unresolved or invalid foundation bounds";
        return false;
    }
    const ChunkCoord previous_grid_origin = config_.region_grid_origin;
    if (usesAdaptiveRegionGrid(identity_.options_hash)) {
        const ChunkCoord selected_origin = chunks_.front().region_grid_origin;
        for (const ChunkDescriptor& descriptor : chunks_) {
            if (!(descriptor.region_grid_origin == selected_origin)) {
                if (error) *error = "region command descriptors use different grid origins";
                return false;
            }
        }
        config_.region_grid_origin = selected_origin;
    } else {
        // v4-v7 plans are world-origin based. Do not accidentally apply a
        // stale value carried by a caller while resuming their old spool names.
        config_.region_grid_origin = {};
    }
    sortChunksLocked();
    const ImportState previous_state = state_;
    const uint32_t previous_phase_index = phase_index_;
    const uint32_t previous_chunk_index = chunk_index_;
    state_ = ImportState::Running;
    phase_index_ = static_cast<uint32_t>(phaseIndex(ImportPhase::Structure));
    chunk_index_ = 0;
    normalizeCursorLocked();
    if (persistCheckpointLocked(error)) {
        planning_chunk_coords_.clear();
        return true;
    }
    state_ = previous_state;
    phase_index_ = previous_phase_index;
    chunk_index_ = previous_chunk_index;
    config_.region_grid_origin = previous_grid_origin;
    return false;
}

std::optional<WorkUnit> BuildImportController::acquireNextUnit(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ImportState::Running || active_unit_) return std::nullopt;
    const ImportState previous_state = state_;
    const uint32_t previous_phase_index = phase_index_;
    const uint32_t previous_chunk_index = chunk_index_;
    normalizeCursorLocked();
    if (state_ != ImportState::Running) {
        if (!persistCheckpointLocked(error)) {
            state_ = previous_state;
            phase_index_ = previous_phase_index;
            chunk_index_ = previous_chunk_index;
        }
        return std::nullopt;
    }
    if (deferred_baseline_ &&
        requiresDurableAcquireCheckpoint(static_cast<ImportPhase>(phase_index_))) {
        if (error) *error = "deferred idempotent units must be committed before a dynamic phase";
        return std::nullopt;
    }
    active_unit_ = createUnitLocked();
    if (!active_unit_) {
        if (error) *error = "scheduler could not create its next work unit";
        return std::nullopt;
    }
    if (requiresDurableAcquireCheckpoint(active_unit_->phase) &&
        !persistCheckpointLocked(error)) {
        active_unit_.reset();
        state_ = previous_state;
        phase_index_ = previous_phase_index;
        chunk_index_ = previous_chunk_index;
        return std::nullopt;
    }
    return active_unit_;
}

std::optional<WorkUnit> BuildImportController::peekNextUnit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ImportState::Running || active_unit_) return std::nullopt;
    return makeUnitForCursorLocked(next_sequence_);
}

bool BuildImportController::completeActiveUnit(uint64_t imported_blocks, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deferred_baseline_) {
        if (error) *error = "deferred units must be committed or rolled back first";
        return false;
    }
    if (state_ != ImportState::Running || !active_unit_) {
        if (error) *error = "there is no active work unit";
        return false;
    }
    const uint64_t expected_blocks = active_unit_->phase == ImportPhase::Clear
        ? 0 : active_unit_->block_count;
    if (imported_blocks != expected_blocks) {
        if (error) *error = "completed work-unit block count does not match its command spool";
        return false;
    }
    if (imported_blocks > std::numeric_limits<uint64_t>::max() - completed_block_count_) {
        if (error) *error = "completed import block count overflows";
        return false;
    }
    const std::optional<WorkUnit> completed_unit = active_unit_;
    const ImportState previous_state = state_;
    const uint32_t previous_phase_index = phase_index_;
    const uint32_t previous_chunk_index = chunk_index_;
    const uint64_t previous_spool_offset = active_spool_offset_;
    const uint64_t previous_block_count = completed_block_count_;
    active_unit_.reset();
    active_spool_offset_ = 0;
    completed_block_count_ += imported_blocks;
    advanceLocked();
    normalizeCursorLocked();
    if (persistCheckpointLocked(error)) return true;
    active_unit_ = completed_unit;
    state_ = previous_state;
    phase_index_ = previous_phase_index;
    chunk_index_ = previous_chunk_index;
    active_spool_offset_ = previous_spool_offset;
    completed_block_count_ = previous_block_count;
    return false;
}

bool BuildImportController::completeActiveUnitDeferred(uint64_t imported_blocks,
                                                       std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != ImportState::Running || !active_unit_) {
        if (error) *error = "there is no active work unit";
        return false;
    }
    if (active_unit_->phase != ImportPhase::Clear &&
        active_unit_->phase != ImportPhase::Structure) {
        if (error) *error = "only Clear and Structure units can be completed without a checkpoint";
        return false;
    }
    const uint64_t expected_blocks = active_unit_->phase == ImportPhase::Clear
        ? 0 : active_unit_->block_count;
    if (imported_blocks != expected_blocks) {
        if (error) *error = "completed work-unit block count does not match its command spool";
        return false;
    }
    if (imported_blocks > std::numeric_limits<uint64_t>::max() - completed_block_count_) {
        if (error) *error = "completed import block count overflows";
        return false;
    }
    if (!deferred_baseline_) {
        DeferredBaseline baseline;
        baseline.state = state_;
        baseline.failure_reason = failure_reason_;
        baseline.phase_index = phase_index_;
        baseline.chunk_index = chunk_index_;
        baseline.next_sequence = active_unit_->sequence;
        baseline.completed_command_count = completed_command_count_ -
            std::min(completed_command_count_, active_spool_offset_);
        baseline.completed_block_count = completed_block_count_;
        baseline.verification_sample_index = verification_sample_index_;
        deferred_baseline_.emplace(std::move(baseline));
    }
    active_unit_.reset();
    active_spool_offset_ = 0;
    completed_block_count_ += imported_blocks;
    advanceLocked();
    normalizeCursorLocked();
    return true;
}

bool BuildImportController::commitDeferredUnits(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!deferred_baseline_) return true;
    if (active_unit_) {
        if (error) *error = "cannot commit deferred units while another unit is active";
        return false;
    }
    if (BuildImportCheckpoint::saveAtomically(config_.checkpoint_path,
                                               checkpointSnapshotLocked(), error)) {
        deferred_baseline_.reset();
        return true;
    }
    rollbackDeferredUnitsLocked();
    return false;
}

void BuildImportController::rollbackDeferredUnits() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        rollbackDeferredUnitsLocked();
    } catch (...) {
        // The baseline consists only of scalar state and movable strings. This
        // guard keeps recovery callers noexcept even if mutex acquisition fails.
    }
}

bool BuildImportController::hasDeferredUnits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return deferred_baseline_.has_value();
}

void BuildImportController::recordActiveProgress(uint64_t completed_commands) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_unit_) {
        // This offset is in-memory diagnostic progress. Durable checkpoints
        // keep the unit boundary and resume replays the whole partition.
        const uint64_t previous_offset = active_spool_offset_;
        active_spool_offset_ = completed_commands;
        if (completed_commands > previous_offset) {
            completed_command_count_ += completed_commands - previous_offset;
        }
        // The server acknowledges commands asynchronously. Keep this offset
        // in memory for UI/debugging only; durable checkpoints are committed
        // at work-unit boundaries so an interrupted unit is replayed safely.
    }
}

bool BuildImportController::rewindActiveProgress(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deferred_baseline_) {
        rollbackDeferredUnitsLocked();
        return true;
    }
    if (active_unit_) {
        completed_command_count_ -= std::min(completed_command_count_, active_spool_offset_);
        active_spool_offset_ = 0;
        return persistCheckpointLocked(error);
    }
    return true;
}

bool BuildImportController::recordVerificationProgress(uint64_t sample_index, bool persist,
                                                       std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deferred_baseline_) {
        if (error) *error = "deferred units must be committed before verification";
        return false;
    }
    if (state_ != ImportState::Verifying && state_ != ImportState::Paused &&
        state_ != ImportState::ClosedForContextChange) {
        if (error) *error = "verification progress can only be recorded for a verification task";
        return false;
    }
    const uint64_t previous = verification_sample_index_;
    verification_sample_index_ = sample_index;
    if (!persist || persistCheckpointLocked(error)) return true;
    verification_sample_index_ = previous;
    return false;
}

bool BuildImportController::finishVerification(bool success, std::string reason,
                                               std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (deferred_baseline_) {
        if (error) *error = "deferred units must be committed before verification";
        return false;
    }
    if (state_ != ImportState::Verifying) {
        if (error) *error = "import is not in final verification";
        return false;
    }
    const ImportState previous_state = state_;
    const std::string previous_reason = failure_reason_;
    state_ = success ? ImportState::Completed : ImportState::Failed;
    failure_reason_ = success ? std::string() :
        (reason.empty() ? "final verification failed" : std::move(reason));
    if (persistCheckpointLocked(error)) return true;
    state_ = previous_state;
    failure_reason_ = previous_reason;
    return false;
}

bool BuildImportController::failActiveUnit(std::string reason, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    rollbackDeferredUnitsLocked();
    state_ = ImportState::Failed;
    failure_reason_ = reason.empty() ? "active work unit failed" : std::move(reason);
    return persistCheckpointLocked(error);
}

bool BuildImportController::pause(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    rollbackDeferredUnitsLocked();
    if (state_ == ImportState::Running || state_ == ImportState::Verifying) {
        return checkpointInterruptionLocked(ImportState::Paused, {}, error);
    }
    return state_ == ImportState::Paused;
}

void BuildImportController::emergencyPauseNoCheckpoint() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        rollbackDeferredUnitsLocked();
        if (state_ == ImportState::Running || state_ == ImportState::Verifying) {
            state_ = ImportState::Paused;
        }
    } catch (...) {
        // Last-resort JNI/game-tick containment. Callers still retain their
        // active reader and report that durable checkpointing failed.
    }
}

bool BuildImportController::resume(const WorldContext& current_world, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    rollbackDeferredUnitsLocked();
    if (state_ != ImportState::Paused && state_ != ImportState::ClosedForContextChange) {
        if (error) *error = "import is not paused or closed for a context change";
        return false;
    }
    if (!(current_world == world_)) {
        if (error) *error = "current world or dimension does not match checkpoint";
        return false;
    }
    const bool unsafe_position =
        (active_unit_ && isReplayUnsafePhase(active_unit_->phase)) ||
        hasCompletedReplayUnsafeWork(chunks_, phase_index_, chunk_index_);
    if (unsafe_position && config_.overwrite_policy != OverwritePolicy::ClearImportedBounds) {
        if (error) {
            *error = "interrupted dynamic blocks cannot be resumed safely without overwrite";
        }
        return false;
    }
    // Structure/Clear commands are idempotent. Dynamic phases require a full
    // top-down clear and global phase replay before execution can continue.
    const std::optional<WorkUnit> previous_active_unit = active_unit_;
    const uint64_t previous_spool_offset = active_spool_offset_;
    const uint64_t previous_command_count = completed_command_count_;
    const ImportState previous_state = state_;
    const std::string previous_failure_reason = failure_reason_;
    const uint32_t previous_phase_index = phase_index_;
    const uint32_t previous_chunk_index = chunk_index_;
    const uint64_t previous_block_count = completed_block_count_;
    const uint64_t previous_verification_index = verification_sample_index_;
    completed_command_count_ -= std::min(completed_command_count_, active_spool_offset_);
    active_unit_.reset();
    active_spool_offset_ = 0;
    if (unsafe_position) {
        phase_index_ = static_cast<uint32_t>(phaseIndex(ImportPhase::Clear));
        chunk_index_ = 0;
        completed_command_count_ = 0;
        completed_block_count_ = 0;
        verification_sample_index_ = 0;
    }
    state_ = chunk_index_ >= chunks_.size() ? ImportState::Verifying : ImportState::Running;
    normalizeCursorLocked();
    failure_reason_.clear();
    if (persistCheckpointLocked(error)) return true;
    active_unit_ = previous_active_unit;
    active_spool_offset_ = previous_spool_offset;
    completed_command_count_ = previous_command_count;
    state_ = previous_state;
    failure_reason_ = previous_failure_reason;
    phase_index_ = previous_phase_index;
    chunk_index_ = previous_chunk_index;
    completed_block_count_ = previous_block_count;
    verification_sample_index_ = previous_verification_index;
    return false;
}

void BuildImportController::cancel(bool delete_checkpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.clear();
    planning_chunk_coords_.clear();
    active_unit_.reset();
    phase_index_ = 0;
    chunk_index_ = 0;
    completed_command_count_ = 0;
    completed_block_count_ = 0;
    active_spool_offset_ = 0;
    verification_sample_index_ = 0;
    deferred_baseline_.reset();
    failure_reason_.clear();
    degradation_notice_.clear();
    state_ = ImportState::Idle;
    if (delete_checkpoint && !config_.checkpoint_path.empty()) {
        std::remove(config_.checkpoint_path.c_str());
    }
}

bool BuildImportController::onWorldContextChanged(const WorldContext& current_world,
                                                  std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if ((state_ == ImportState::Planning || state_ == ImportState::Running ||
         state_ == ImportState::Paused || state_ == ImportState::Verifying) &&
        !(current_world == world_)) {
        rollbackDeferredUnitsLocked();
        return checkpointInterruptionLocked(
            ImportState::ClosedForContextChange,
            "world or dimension changed; checkpoint saved and import closed", error);
    }
    return true;
}

ImportState BuildImportController::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string BuildImportController::failureReason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failure_reason_;
}

bool BuildImportController::worldMatches(const WorldContext& world) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return world_ == world;
}

uint64_t BuildImportController::completedBlockCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completed_block_count_;
}

BuildImportRuntimeMetadata BuildImportController::runtimeMetadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    BuildImportRuntimeMetadata metadata;
    metadata.chunk_size = config_.chunk_size;
    metadata.simulation_chunk_range = config_.simulation_chunk_range;
    metadata.region_span = regionSpanForImportConfig(config_);
    metadata.chunk_load_radius = config_.chunk_load_radius;
    metadata.chunk_wait_ticks = config_.chunk_wait_ticks;
    metadata.region_grid_origin = config_.region_grid_origin;
    metadata.ticking_area_min_y = config_.ticking_area_min_y;
    metadata.ticking_area_max_y = config_.ticking_area_max_y;
    metadata.overwrite_policy = config_.overwrite_policy;
    metadata.verify_after_import = config_.verify_after_import;
    metadata.verification_precision = config_.verification_precision;
    metadata.phase_index = phase_index_;
    metadata.verification_sample_index = verification_sample_index_;
    return metadata;
}

CheckpointSnapshot BuildImportController::checkpointSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return checkpointSnapshotLocked();
}

CheckpointSnapshot BuildImportController::checkpointSnapshotLocked() const {
    CheckpointSnapshot snapshot;
    snapshot.identity = identity_;
    snapshot.world = world_;
    snapshot.config = config_;
    snapshot.state = state_;
    snapshot.phase_index = phase_index_;
    snapshot.chunk_index = chunk_index_;
    snapshot.completed_command_count = completed_command_count_;
    snapshot.completed_block_count = completed_block_count_;
    snapshot.verification_sample_index = verification_sample_index_;
    snapshot.degradation_notice = degradation_notice_;
    snapshot.active_spool_offset = active_spool_offset_;
    snapshot.has_active_unit = active_unit_.has_value();
    if (active_unit_) {
        snapshot.active_coord = active_unit_->coord;
        snapshot.active_phase = active_unit_->phase;
    }
    return snapshot;
}

std::optional<CheckpointSnapshot> BuildImportController::loadCheckpoint(std::string* error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (config_.checkpoint_path.empty()) {
        if (error) *error = "checkpoint path is not configured";
        return std::nullopt;
    }
    return BuildImportCheckpoint::load(config_.checkpoint_path, error);
}

std::vector<std::string> BuildImportController::makePrepareCommands(const WorkUnit& unit) const {
    return makePrepareCommands(unit, false, 0, 0);
}

std::vector<std::string> BuildImportController::makePrepareCommands(
        const WorkUnit& unit, bool has_player_position, int32_t player_x,
        int32_t player_z) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string name = areaName(identity_.job_id, unit.region_coord);
    const int64_t center_x = (static_cast<int64_t>(unit.load_bounds.min_x) + unit.load_bounds.max_x) / 2;
    const int64_t center_z = (static_cast<int64_t>(unit.load_bounds.min_z) + unit.load_bounds.max_z) / 2;
    const int64_t teleport_y = static_cast<int64_t>(unit.imported_bounds.max_y) +
        static_cast<int64_t>(config_.teleport_y_offset);
    std::vector<std::string> commands;
    commands.push_back(
        "/tickingarea add " + std::to_string(unit.load_bounds.min_x) + " " +
            std::to_string(config_.ticking_area_min_y) + " " + std::to_string(unit.load_bounds.min_z) + " " +
            std::to_string(unit.load_bounds.max_x) + " " + std::to_string(config_.ticking_area_max_y) + " " +
            std::to_string(unit.load_bounds.max_z) + " " +
            name + " true");
    if (has_player_position) {
        // Commands execute in order on the server, so every hop starts from
        // the previous hop's landing point and stays within the bounded
        // distance a movement guard will accept.
        const double dx = static_cast<double>(center_x) - player_x;
        const double dz = static_cast<double>(center_z) - player_z;
        const double distance = std::hypot(dx, dz);
        if (std::isfinite(distance) &&
            distance > static_cast<double>(kTeleportHopBlocks)) {
            const int64_t hop_count = std::min<int64_t>(
                kMaximumTeleportHopCommands,
                static_cast<int64_t>(distance /
                                     static_cast<double>(kTeleportHopBlocks)));
            for (int64_t index = 1; index <= hop_count; ++index) {
                const double fraction = static_cast<double>(index) /
                    static_cast<double>(hop_count + 1);
                const int64_t hop_x = static_cast<int64_t>(
                    std::floor(static_cast<double>(player_x) + dx * fraction));
                const int64_t hop_z = static_cast<int64_t>(
                    std::floor(static_cast<double>(player_z) + dz * fraction));
                commands.push_back("/tp @s " + std::to_string(hop_x) + " " +
                    std::to_string(teleport_y) + " " + std::to_string(hop_z));
            }
        }
    }
    commands.push_back("/tp @s " + std::to_string(center_x) + " " +
        std::to_string(teleport_y) + " " + std::to_string(center_z));
    return commands;
}

std::optional<std::string> BuildImportController::makeClearCommand(const WorkUnit& unit) const {
    const std::vector<std::string> commands = makeClearCommands(unit);
    return commands.empty() ? std::nullopt : std::optional<std::string>(commands.front());
}

std::vector<std::string> BuildImportController::makeClearCommands(const WorkUnit& unit) const {
    const std::vector<PlannedCommand> plan = makeClearPlan(unit);
    std::vector<std::string> commands;
    commands.reserve(plan.size());
    for (const PlannedCommand& command : plan) {
        commands.push_back("/fill " + std::to_string(command.bounds.min_x) + " " +
            std::to_string(command.bounds.min_y) + " " + std::to_string(command.bounds.min_z) +
            " " + std::to_string(command.bounds.max_x) + " " +
            std::to_string(command.bounds.max_y) + " " + std::to_string(command.bounds.max_z) +
            " minecraft:air");
    }
    return commands;
}

std::vector<PlannedCommand> BuildImportController::makeClearPlan(const WorkUnit& unit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unit.phase != ImportPhase::Clear ||
        config_.overwrite_policy != OverwritePolicy::ClearImportedBounds ||
        !unit.imported_bounds.isValid()) {
        return {};
    }
    const BlockBounds& bounds = unit.imported_bounds;
    // Scale fill volume with the selected target rate while keeping one server
    // operation small enough to avoid a world-tick spike. Sustained throughput
    // comes from adaptive RPC batching rather than a single huge /fill.
    const int64_t command_limit = fillBlockLimitForRate(config_.blocks_per_second);
    const int64_t width = static_cast<int64_t>(bounds.max_x) - bounds.min_x + 1;
    const int64_t height = static_cast<int64_t>(bounds.max_y) - bounds.min_y + 1;
    const int64_t depth = static_cast<int64_t>(bounds.max_z) - bounds.min_z + 1;
    const int64_t x_step = std::min(width, command_limit);
    const int64_t z_step = std::min(depth, std::max<int64_t>(1, command_limit / x_step));
    const int64_t y_step = std::min(height,
        std::max<int64_t>(1, command_limit / (x_step * z_step)));
    std::vector<PlannedCommand> commands;
    // Clear top-down. Removing support from below first can make sand/gravel
    // fall into an already-cleared slice and survive the partition reset.
    for (int64_t y_end = bounds.max_y; y_end >= bounds.min_y;) {
        const int64_t y_count = std::min<int64_t>(y_end - bounds.min_y + 1, y_step);
        const int64_t y = y_end - y_count + 1;
        for (int64_t z = bounds.min_z; z <= bounds.max_z; z += z_step) {
            const int64_t z_count = std::min<int64_t>(bounds.max_z - z + 1, z_step);
            for (int64_t x = bounds.min_x; x <= bounds.max_x; x += x_step) {
                const int64_t x_count = std::min<int64_t>(bounds.max_x - x + 1, x_step);
                const uint32_t volume = static_cast<uint32_t>(x_count * y_count * z_count);
                commands.push_back({
                    {static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(z),
                     static_cast<int32_t>(x + x_count - 1),
                     static_cast<int32_t>(y_end),
                     static_cast<int32_t>(z + z_count - 1)},
                    "minecraft:air", 0, volume});
            }
        }
        y_end = y - 1;
    }
    return commands;
}

std::vector<PlannedCommand> BuildImportController::makeDenyFoundationPlan(
        const WorkUnit& unit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!unit.place_deny_foundation || !config_.place_deny_layer ||
        !config_.deny_layer_bounds.isValid() ||
        config_.deny_layer_bounds.min_y != config_.deny_layer_bounds.max_y ||
        !unit.imported_bounds.isValid()) {
        return {};
    }

    const BlockBounds& foundation = config_.deny_layer_bounds;
    // Command spools are aggregated into simulation-sized work regions. A
    // source descriptor may cover only a narrow part of its region (especially
    // for sparse multi-region litematics), while the optional foundation owns
    // the complete source-footprint slice of that work region. Intersecting
    // descriptor.imported_bounds here would therefore leave seams between
    // sparse descriptors. Keep source bounds untouched for normal placement
    // and verification, and derive the auxiliary X/Z slice from the region.
    const int64_t chunk_size = std::max<int32_t>(1, config_.chunk_size);
    const int64_t span = std::max<int32_t>(1, unit.region_span);
    const int64_t region_min_x = regionMinChunkCoordinate(
        unit.region_coord.x, unit.region_span, unit.region_grid_origin.x) * chunk_size;
    const int64_t region_min_z = regionMinChunkCoordinate(
        unit.region_coord.z, unit.region_span, unit.region_grid_origin.z) * chunk_size;
    const int64_t region_max_x = region_min_x + span * chunk_size - 1;
    const int64_t region_max_z = region_min_z + span * chunk_size - 1;
    const int64_t min_x = std::max<int64_t>(foundation.min_x, region_min_x);
    const int64_t max_x = std::min<int64_t>(foundation.max_x, region_max_x);
    const int64_t min_z = std::max<int64_t>(foundation.min_z, region_min_z);
    const int64_t max_z = std::min<int64_t>(foundation.max_z, region_max_z);
    if (min_x > max_x || min_z > max_z) return {};

    // The foundation is a single horizontal layer. Split it with the same
    // bounded /fill budget as ordinary clears so it cannot create one giant
    // server-tick spike on very large imports.
    const int64_t command_limit = fillBlockLimitForRate(config_.blocks_per_second);
    const int64_t width = max_x - min_x + 1;
    const int64_t depth = max_z - min_z + 1;
    const int64_t x_step = std::min(width, command_limit);
    const int64_t z_step = std::min(depth, std::max<int64_t>(1, command_limit / x_step));
    std::vector<PlannedCommand> commands;
    for (int64_t z = min_z; z <= max_z; z += z_step) {
        const int64_t z_count = std::min<int64_t>(max_z - z + 1, z_step);
        for (int64_t x = min_x; x <= max_x; x += x_step) {
            const int64_t x_count = std::min<int64_t>(max_x - x + 1, x_step);
            const uint32_t block_count = static_cast<uint32_t>(x_count * z_count);
            commands.push_back({
                {static_cast<int32_t>(x), foundation.min_y, static_cast<int32_t>(z),
                 static_cast<int32_t>(x + x_count - 1), foundation.max_y,
                 static_cast<int32_t>(z + z_count - 1)},
                "minecraft:deny", 0, block_count});
        }
    }
    return commands;
}

std::string BuildImportController::makeCleanupCommand(const WorkUnit& unit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return "/tickingarea remove " + areaName(identity_.job_id, unit.region_coord);
}

bool BuildImportController::persistCheckpointLocked(std::string* error) const {
    if (deferred_baseline_) {
        if (error) *error = "cannot checkpoint an uncommitted deferred cursor";
        return false;
    }
    return BuildImportCheckpoint::saveAtomically(config_.checkpoint_path,
                                                  checkpointSnapshotLocked(), error);
}

void BuildImportController::rollbackDeferredUnitsLocked() noexcept {
    if (!deferred_baseline_) return;
    DeferredBaseline baseline = std::move(*deferred_baseline_);
    deferred_baseline_.reset();
    state_ = baseline.state;
    failure_reason_.swap(baseline.failure_reason);
    phase_index_ = baseline.phase_index;
    chunk_index_ = baseline.chunk_index;
    next_sequence_ = baseline.next_sequence;
    completed_command_count_ = baseline.completed_command_count;
    completed_block_count_ = baseline.completed_block_count;
    verification_sample_index_ = baseline.verification_sample_index;
    active_unit_.reset();
    active_spool_offset_ = 0;
}

bool BuildImportController::checkpointInterruptionLocked(
        ImportState target_state, std::string reason, std::string* error) {
    if (target_state != ImportState::Paused &&
        target_state != ImportState::ClosedForContextChange) {
        if (error) *error = "invalid interruption checkpoint state";
        return false;
    }
    CheckpointSnapshot candidate = checkpointSnapshotLocked();
    candidate.state = target_state;
    const bool unsafe_position =
        (active_unit_ && isReplayUnsafePhase(active_unit_->phase)) ||
        hasCompletedReplayUnsafeWork(chunks_, phase_index_, chunk_index_);
    const bool full_rollback = unsafe_position &&
        config_.overwrite_policy == OverwritePolicy::ClearImportedBounds;
    if (full_rollback) {
        candidate.phase_index = static_cast<uint32_t>(phaseIndex(ImportPhase::Clear));
        candidate.chunk_index = 0;
        candidate.has_active_unit = false;
        candidate.active_coord = {};
        candidate.active_phase = ImportPhase::Structure;
        candidate.active_spool_offset = 0;
        candidate.completed_command_count = 0;
        candidate.completed_block_count = 0;
        candidate.verification_sample_index = 0;
    }
    if (!BuildImportCheckpoint::saveAtomically(config_.checkpoint_path, candidate, error)) {
        return false;
    }
    state_ = target_state;
    failure_reason_ = std::move(reason);
    if (full_rollback) {
        phase_index_ = candidate.phase_index;
        chunk_index_ = candidate.chunk_index;
        active_unit_.reset();
        active_spool_offset_ = 0;
        completed_command_count_ = 0;
        completed_block_count_ = 0;
        verification_sample_index_ = 0;
    }
    return true;
}

bool BuildImportController::validateConfig(const ImportConfig& config, std::string* error,
                                           bool allow_legacy_checkpoint) const {
    if (config.chunk_size <= 0 || config.chunk_size > 64 ||
        config.chunk_load_radius < 0 || config.chunk_load_radius > 3 ||
        config.chunk_wait_ticks < 0 ||
        config.blocks_per_second < 1 ||
        config.blocks_per_second > kMaximumBlocksPerSecond ||
        !isValidVerificationPrecision(config.verification_precision) ||
        config.ticking_area_min_y > config.ticking_area_max_y) {
        if (error) *error = "invalid chunk loading or vertical bounds configuration";
        return false;
    }
    if (config.simulation_chunk_range == 0) {
        if (allow_legacy_checkpoint) return true;
        if (error) {
            *error = "new imports require a simulation chunk range from 4 to 8";
        }
        return false;
    }
    if (!isValidSimulationChunkRange(config.simulation_chunk_range) ||
        config.chunk_size != ImportConfig::kVanillaChunkSize) {
        if (error) {
            *error = "simulation chunk range must be 4 to 8 native chunks (16 blocks each)";
        }
        return false;
    }
    return true;
}

void BuildImportController::advanceLocked() {
    ++chunk_index_;
    if (chunk_index_ >= chunks_.size()) {
        chunk_index_ = 0;
        if (phase_index_ == phaseIndex(ImportPhase::Clear)) {
            phase_index_ = static_cast<uint32_t>(phaseIndex(ImportPhase::Structure));
            return;
        }
        const ImportPhase next = nextPlacementPhase(static_cast<ImportPhase>(phase_index_));
        if (next == ImportPhase::Count) {
            chunk_index_ = static_cast<uint32_t>(chunks_.size());
            phase_index_ = 0;
            state_ = ImportState::Verifying;
        } else {
            phase_index_ = static_cast<uint32_t>(phaseIndex(next));
        }
    }
}

void BuildImportController::normalizeCursorLocked() {
    if (state_ != ImportState::Running) return;
    while (phase_index_ < kImportPhaseCount) {
        const ImportPhase phase = static_cast<ImportPhase>(phase_index_);
        if (phase == ImportPhase::Clear) {
            if (config_.overwrite_policy != OverwritePolicy::ClearImportedBounds) {
                state_ = ImportState::Paused;
                failure_reason_ =
                    "dynamic recovery requires overwrite-enabled global clearing";
                return;
            }
            while (chunk_index_ < chunks_.size() &&
                   !chunks_[chunk_index_].imported_bounds.isValid()) {
                ++chunk_index_;
            }
            if (chunk_index_ < chunks_.size()) return;
            phase_index_ = static_cast<uint32_t>(phaseIndex(ImportPhase::Structure));
            chunk_index_ = 0;
            continue;
        }
        while (chunk_index_ < chunks_.size()) {
            if (cursorHasWork(chunks_[chunk_index_], phase_index_, config_)) {
                return;
            }
            ++chunk_index_;
        }
        const ImportPhase next = nextPlacementPhase(phase);
        if (next == ImportPhase::Count) break;
        phase_index_ = static_cast<uint32_t>(phaseIndex(next));
        chunk_index_ = 0;
    }
    chunk_index_ = static_cast<uint32_t>(chunks_.size());
    phase_index_ = 0;
    state_ = ImportState::Verifying;
}

void BuildImportController::sortChunksLocked() {
    const int32_t span = regionSpanForImportConfig(config_);
    const bool serpentine = usesSerpentineRegionOrder(identity_.options_hash);
    std::sort(chunks_.begin(), chunks_.end(),
              [span, region_grid_origin = config_.region_grid_origin,
               serpentine](const ChunkDescriptor& left,
                          const ChunkDescriptor& right) {
                  return descriptorOrderLess(left, right, span, region_grid_origin, serpentine);
              });
}

std::optional<WorkUnit> BuildImportController::createUnitLocked() {
    std::optional<WorkUnit> unit = makeUnitForCursorLocked(next_sequence_);
    if (unit) ++next_sequence_;
    return unit;
}

std::optional<WorkUnit> BuildImportController::makeUnitForCursorLocked(
        uint64_t sequence) const {
    if (phase_index_ >= kImportPhaseCount || chunk_index_ >= chunks_.size()) return std::nullopt;
    const ChunkDescriptor& descriptor = chunks_[chunk_index_];
    const ImportPhase phase = static_cast<ImportPhase>(phase_index_);
    WorkUnit unit;
    unit.sequence = sequence;
    unit.coord = descriptor.coord;
    unit.region_span = regionSpanForImportConfig(config_);
    unit.region_grid_origin = config_.region_grid_origin;
    unit.region_coord = regionForChunk(unit.coord, unit.region_span,
                                       unit.region_grid_origin);
    unit.phase = phase;
    unit.imported_bounds = descriptor.imported_bounds;
    unit.load_bounds = loadBoundsFor(unit.region_coord, unit.region_span,
                                     unit.region_grid_origin, config_);
    unit.wait_ticks = config_.chunk_wait_ticks;
    if (phase == ImportPhase::Structure &&
        isAuxiliaryOnlyChunk(descriptor, config_)) {
        unit.phase = ImportPhase::Clear;
        unit.place_deny_foundation = intersectsDenyFoundation(descriptor, config_);
    } else if (phase != ImportPhase::Clear) {
        unit.spool_path = descriptor.command_paths[phaseIndex(phase)];
        unit.block_count = descriptor.phase_block_counts[phaseIndex(phase)];
        // A structure may consist entirely of gravity/attachment/fluid blocks.
        // Put its foundation in the first real source phase, rather than
        // forcing an empty structure spool into the scheduler.
        unit.place_deny_foundation =
            intersectsDenyFoundation(descriptor, config_) &&
            isFirstPlacementWork(descriptor, phase);
        if (config_.overwrite_policy == OverwritePolicy::ClearImportedBounds) {
            unit.clear_before_build = true;
            const size_t current_order = placementOrderIndex(phase);
            for (size_t earlier = 0; earlier < current_order; ++earlier) {
                if (hasSpoolForPhase(descriptor, kPlacementPhaseOrder[earlier])) {
                    unit.clear_before_build = false;
                    break;
                }
            }
        }
    }
    return unit;
}

BlockBounds BuildImportController::loadBoundsFor(const ChunkCoord& region_coord, int32_t region_span,
                                                  const ChunkCoord& region_grid_origin,
                                                  const ImportConfig& config) {
    const int64_t chunk_size = config.chunk_size;
    const int64_t span = std::max<int32_t>(1, region_span);
    // Keep a one-chunk halo loaded around the work region. Attachments,
    // liquids and neighbor updates at a region edge can otherwise target an
    // unloaded chunk even though the command's own block is inside the area.
    const int64_t region_min_x = regionMinChunkCoordinate(
        region_coord.x, region_span, region_grid_origin.x) * chunk_size;
    const int64_t region_min_z = regionMinChunkCoordinate(
        region_coord.z, region_span, region_grid_origin.z) * chunk_size;
    const int64_t min_x = region_min_x - chunk_size;
    const int64_t min_z = region_min_z - chunk_size;
    const int64_t max_x = region_min_x + (span + 1) * chunk_size - 1;
    const int64_t max_z = region_min_z + (span + 1) * chunk_size - 1;
    return {clampToInt32(min_x), config.ticking_area_min_y, clampToInt32(min_z),
            clampToInt32(max_x), config.ticking_area_max_y, clampToInt32(max_z)};
}

std::string BuildImportController::areaName(const std::string& job_id, const ChunkCoord& region_coord) {
    (void)job_id;
    (void)region_coord;
    // Only one import runtime can be active. A stable name lets a later
    // process remove an area left behind by a crash before loading new work.
    return "infinitecz_build";
}

}  // namespace build_import
