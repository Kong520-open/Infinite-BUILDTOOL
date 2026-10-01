#ifndef INFINITE_TEXTURE_BUILD_IMPORT_RUNTIME_H
#define INFINITE_TEXTURE_BUILD_IMPORT_RUNTIME_H

#include "BuildImportController.h"
#include "BuildImportThroughput.h"
#include "BuildImportUndoStore.h"
#include "BdxParser.h"
#include "CommandBlockSpool.h"
#include "DeferredImportDataSpool.h"
#include "McworldParser.h"
#include "NativeWorldAccess.h"
#include "CommandFeedbackGate.h"
#include "MidiCommandMusicParser.h"
#include "CommandSpool.h"
#include "LevelChunkEvidenceCache.h"
#include "LitematicParser.h"
#include "MapInventoryTransfer.h"
#include "MapChestProductionPreflight.h"
#include "MapStorageChestCaptureAdapter.h"
#include "MapStorageAnvilAdapter.h"
#include "MapStoragePipelineDriver.h"
#include "MapVisibleAnvilProductionPreflight.h"
#include "PixelArtParser.h"
#include "SchematicParser.h"
#include "VerificationRepairTracker.h"

#include <atomic>
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace build_import {

class CommandBatchPrefetcher;

struct BuildImportStartRequest {
    SchematicParseOptions parse;
    PixelArtParseOptions pixel_art;
    ImportSourceType source_type = ImportSourceType::Schematic;
    ImportIdentity identity;
    WorldContext world;
    ImportConfig config;
};

class BuildImportRuntime {
public:
    static BuildImportRuntime& instance();
    bool start(BuildImportStartRequest request, std::string* error = nullptr);
    bool undoLastImport(const std::string& storage_directory,
                        const std::string& spool_directory,
                        const WorldContext& context,
                        std::string* error = nullptr);
    bool discardUndoClaim(const std::string& storage_directory,
                          const std::string& spool_directory,
                          std::string* error = nullptr);
    bool restore(const std::string& spool_directory, const WorldContext& context,
                 std::string* error = nullptr);
    void onGameTick();
    void onWorldContextChanged(const WorldContext& context);
    void pause();
    bool resume(const WorldContext& context, std::string* error = nullptr);
    void cancel();
    ImportState state() const;
    std::string status() const;
    uint64_t totalBlockCount() const;
    uint64_t importedBlockCount() const;
    // Returns true when the packet is an importer-owned silent command result
    // that must not be forwarded into the game's command-feedback UI.
    bool onRawNetworkPacket(const std::string& packet);

private:
    enum class ExecuteStage {
        None,
        Prepare,
        Wait,
        Clear,
        Build,
        Drain,
        Cleanup,
        Verify,
        // Deferred block-actor and entity data is applied only after ordinary
        // placement and its optional repair pass. These stages retain the
        // controller in Verifying so pause/context checkpoints cannot expose
        // a structure whose metadata is only partially restored.
        CommandBlockPrepare,
        CommandBlockWait,
        CommandBlockWrite,
        SignPrepare,
        SignWait,
        SignOpenWait,
        SignFaceVerify,
        SignWrite,
        ContainerPrepare,
        ContainerWait,
        ContainerWrite,
        EntityPrepare,
        EntityWait,
        EntityWrite,
        MapPrepare,
        MapWait,
        MapSelect,
        MapSettle,
        // Reserved for the post-coverage rename/chest transaction. The
        // legacy cursor commit remains active until the live packet pilot
        // proves the silent path; merely entering this stage never sends.
        MapStorageHandoff,
    };
    enum class RpcResultState { Unavailable, Pending, Rejected, Accepted };
    enum class ChunkProbeState { Waiting, Ready, Rejected, TransportFailure };

    BuildImportRuntime() = default;
    ~BuildImportRuntime();

    void parseWorker(BuildImportStartRequest request, uint64_t generation);
    void undoPlanningWorker(BuildImportUndoManifest manifest, uint64_t generation);
    void beginNextUnit();
    void resetActiveUnitRuntime(bool preserve_data_window = false);
    void resetVerificationRuntime();
    void handleGameTickFailure(const char* detail) noexcept;
    void resetCurrentRun(bool discard_files);
    bool persistUndoSnapshot(std::string* error = nullptr);
    void releaseUndoClaimNoThrow() noexcept;
    bool retireUndoClaim(std::string* warning = nullptr);
    bool invalidateAvailableUndoRecord(std::string* warning = nullptr);
    void scheduleActiveUnitRecovery(const std::string& reason);
    void releaseLoadedRegion(bool send_cleanup);
    bool flushPendingCleanupCommands(std::chrono::steady_clock::time_point now);
    void serviceHeldRegionCleanups(std::chrono::steady_clock::time_point now);
    std::optional<std::string> takeHeldRegionCleanup(const std::string& command);
    void maybePrefetchNextRegionPrepare(std::chrono::steady_clock::time_point now);
    void startChunkLoadProbe(bool wait_for_region);
    BlockBounds activeChunkProbeBounds() const;
    bool activeChunkProbeReadable(std::chrono::steady_clock::time_point now);
    ChunkProbeState pollServerChunkProbe(const BlockBounds& bounds,
                                         std::chrono::steady_clock::time_point now,
                                         std::string* error = nullptr);
    void resetServerChunkProbe();
    static bool nativeChunksReadable(const BlockBounds& bounds);
    bool levelChunkEvidenceForBounds(const BlockBounds& bounds, bool* any_evidence);
    void noteDataCommandsSent(size_t command_count, uint64_t block_count);
    bool dataWindowNeedsDrain() const;
    void startDataDrain(ImportPhase phase, ExecuteStage resume_stage);
    bool tryPipelineDataDrain(std::chrono::steady_clock::time_point now);
    bool servicePipelinedDataBarrier(std::chrono::steady_clock::time_point now);
    void absorbPipelinedDataBarrier();
    bool dataCommandsDrained(std::chrono::steady_clock::time_point now,
                             bool settle_repair_phase = true);
    bool repairPhaseDrained(ImportPhase phase,
                            std::chrono::steady_clock::time_point now);
    std::string takeDrainFailure();
    void preparePlacementBatch();
    void resetPlacementBatch();
    void maybeLogPerformanceTelemetry(std::chrono::steady_clock::time_point now);

    bool ensureRpcTransport();
    void invalidateRpcTransport() noexcept;
    bool commandFeedbackSuppressionRequested() const;
    bool driveCommandFeedback(std::chrono::steady_clock::time_point now);
    void resetCommandFeedbackTracking();
    bool executeCommand(const std::string& command);
    bool executeCommands(const std::vector<std::string>& commands,
                         size_t* sent_count = nullptr);
    bool executeTrackedCommands(const std::vector<std::string>& commands,
                                const std::vector<std::string>& uuids,
                                size_t* sent_count = nullptr);
    bool collectPythonRpcAcks(const std::vector<std::string>& uuids);
    RpcResultState pollTrackedCommands(const std::vector<std::string>& uuids,
                                       bool* any_rejected = nullptr,
                                       std::string* matched_uuid = nullptr);
    std::string nextRpcUuid();
    void resetDrainBarrier();
    static std::string commandFor(const PlannedCommand& command);
    static std::string verificationProbeCommandFor(
        const VerificationPlanSample& sample);

    void beginFinalVerification();
    void tickFinalVerification(std::chrono::steady_clock::time_point now);
    void beginVerificationRegion(const VerificationPlanSample& sample,
                                 std::chrono::steady_clock::time_point now);
    void scheduleChunkRepair(const ChunkCoord& chunk, size_t sample_index,
                             std::chrono::steady_clock::time_point now);
    void tickChunkRepair(std::chrono::steady_clock::time_point now);
    void beginVerificationServerProbe(size_t sample_index,
                                      std::chrono::steady_clock::time_point now);
    bool serviceVerificationServerProbe(
        std::chrono::steady_clock::time_point now);
    void resetVerificationServerProbe();
    void finishFinalVerification(bool success, const std::string& detail = {});
    void pauseFinalVerification(const std::string& detail);
    bool beginCommandBlockWrite(std::string* error = nullptr);
    void tickCommandBlockWrite(std::chrono::steady_clock::time_point now);
    bool beginCommandBlockCell(std::chrono::steady_clock::time_point now,
                               std::string* error = nullptr);
    bool commandBlockCellReady(std::chrono::steady_clock::time_point now,
                               std::string* error = nullptr);
    bool persistCommandBlockCursor(bool force, std::string* error = nullptr);
    void resetCommandBlockRuntime();
    void finishCommandBlockWrite();
    void pauseCommandBlockWrite(const std::string& detail);
    bool beginSignWrite(std::string* error = nullptr);
    void tickSignWrite(std::chrono::steady_clock::time_point now);
    bool persistSignCursor(bool force, std::string* error = nullptr);
    void resetSignShellProbe();
    void resetSignRuntime();
    void finishSignWrite();
    void pauseSignWrite(const std::string& detail);
    void retryOrPauseSignWrite(const std::string& detail);
    bool beginContainerWrite(std::string* error = nullptr);
    void tickContainerWrite(std::chrono::steady_clock::time_point now);
    bool persistContainerCursor(bool force, std::string* error = nullptr);
    void resetContainerRuntime();
    void finishContainerWrite();
    void pauseContainerWrite(const std::string& detail);
    bool beginEntityWrite(std::string* error = nullptr);
    void tickEntityWrite(std::chrono::steady_clock::time_point now);
    bool persistEntityCursor(bool force, std::string* error = nullptr);
    void resetEntityRuntime();
    void finishEntityWrite();
    void pauseEntityWrite(const std::string& detail);
    bool beginMapCreation(std::string* error = nullptr);
    void tickMapCreation(std::chrono::steady_clock::time_point now);
    void tickMapStorageHandoff();
    bool captureMapStorageChest(const MapChestPosition& position, bool reopen,
                                ContainerCaptureResult* capture,
                                std::string* error);
    void serviceMapStorageChest(std::chrono::steady_clock::time_point now) noexcept;
    void serviceMapStorageAnvil(std::chrono::steady_clock::time_point now) noexcept;
    bool observeMapStorageAnvil(int32_t x, int32_t y, int32_t z,
                                bool allow_open,
                                MapAnvilRenameWindowEvidence* evidence,
                                std::string* error);
    bool readMapStorageAnvilNativeInput(const MapAnvilRenameRecord& record,
                                       MapAnvilInputProof* proof,
                                       std::string* error);
    bool commitStoredMapCursor(uint64_t before, uint64_t after,
                               const MapChestTransferRecord& chest,
                               std::string* error);
    bool finalizeStoredMapUse(uint64_t old_cursor, uint64_t committed_cursor,
                              std::string* error);
    void clearCompletedMapUseRuntime();
    void pauseMapCreation(const std::string& detail);
    void finishMapCreation();
    void resetMapCreationRuntime();
    bool pollNativeEvents();

    mutable std::mutex mutex_;
    mutable std::recursive_mutex lifecycle_mutex_;
    BuildImportController controller_;
    BlockMapper mapper_;
    std::thread worker_;
    std::atomic<bool> worker_running_{false};
    std::atomic<uint64_t> run_generation_{0};
    std::atomic<bool> restore_in_progress_{false};

    std::unique_ptr<CommandBatchPrefetcher> command_prefetcher_;
    std::optional<WorkUnit> active_unit_;
    std::optional<ChunkCoord> loaded_region_;
    std::string loaded_region_cleanup_command_;
    uint64_t active_unit_imported_start_ = 0;
    uint64_t active_unit_emitted_blocks_ = 0;
    uint64_t active_command_index_ = 0;
    ExecuteStage stage_ = ExecuteStage::None;
    size_t prepare_index_ = 0;
    size_t clear_index_ = 0;
    std::vector<std::string> prepare_commands_;
    std::vector<PlannedCommand> clear_plan_;
    std::vector<std::string> batch_commands_;
    std::vector<uint32_t> batch_block_counts_;
    std::string rpc_payload_buffer_;
    std::string rpc_code_buffer_;
    bool drain_to_build_ = false;
    ExecuteStage drain_resume_stage_ = ExecuteStage::None;
    ImportPhase drain_phase_ = ImportPhase::Structure;

    std::string status_;
    std::string degradation_notice_;
    uint64_t total_block_count_ = 0;
    uint64_t imported_block_count_ = 0;
    int32_t blocks_per_second_ = 20;
    std::atomic<bool> suppress_command_feedback_{true};
    double available_block_tokens_ = 1.0;
    double available_command_tokens_ = 1.0;
    size_t max_batch_commands_ = 1;
    BuildImportThroughputGovernor throughput_governor_{20};
    uint64_t unbarriered_command_count_ = 0;
    uint64_t unbarriered_block_count_ = 0;
    std::chrono::steady_clock::time_point data_window_started_at_{};
    // A full mid-unit ACK window is closed by a background ("pipelined")
    // barrier so placement continues while the ACK is in flight. At most one
    // such barrier is outstanding; a second full window blocks as before.
    std::vector<std::string> pipelined_barrier_uuids_;
    std::chrono::steady_clock::time_point pipelined_barrier_sent_at_{};
    std::chrono::steady_clock::time_point pipelined_barrier_poll_at_{};
    std::chrono::steady_clock::time_point pipelined_barrier_deadline_{};
    std::chrono::steady_clock::time_point active_unit_last_send_at_{};
    std::chrono::steady_clock::time_point phase_settle_ready_at_{};
    std::chrono::milliseconds pending_resume_settle_delay_{0};
    uint32_t deferred_window_units_ = 0;
    std::optional<ImportPhase> deferred_window_phase_;
    std::vector<std::pair<ChunkCoord, size_t>> deferred_completed_units_;
    uint64_t deferred_undo_volume_ = 0;

    std::string spool_directory_;
    std::string undo_storage_directory_;
    std::string undo_record_id_;
    std::string undo_claim_job_id_;
    uint64_t durable_undo_block_count_ = 0;
    WorldContext current_world_;
    bool undo_run_active_ = false;
    bool region_command_spools_ = false;
    bool adaptive_region_grid_ = false;
    bool recovery_pending_ = false;
    std::chrono::steady_clock::time_point region_add_ready_at_{};
    std::chrono::steady_clock::time_point cleanup_retry_at_{};
    std::vector<std::string> pending_cleanup_commands_;
    // A dynamic-phase region that finished with a same-phase region handoff
    // keeps its ticking area alive (its removal is "held") for the settle
    // window while the pipeline already loads and builds the next region,
    // instead of stalling every tick until the settle deadline.
    struct HeldRegionCleanup {
        std::string command;
        std::chrono::steady_clock::time_point release_at;
    };
    std::deque<HeldRegionCleanup> held_region_cleanups_;
    std::chrono::steady_clock::time_point held_region_cleanup_retry_at_{};
    std::chrono::steady_clock::time_point hold_active_region_release_until_{};
    // While a completed unit's final drain barrier is in flight, the next
    // region's prepare batch (tickingarea add + tp) may already be sent so
    // chunk streaming overlaps the ACK wait. beginNextUnit adopts a fresh
    // matching prefetch instead of re-sending the batch.
    std::optional<ChunkCoord> prefetched_prepare_region_;
    std::chrono::steady_clock::time_point prefetched_prepare_sent_at_{};
    std::vector<std::string> cleanup_barrier_uuids_;
    size_t cleanup_barrier_command_count_ = 0;
    std::chrono::steady_clock::time_point cleanup_barrier_poll_at_{};
    std::chrono::steady_clock::time_point cleanup_barrier_deadline_{};
    uint32_t cleanup_barrier_failure_count_ = 0;
    CommandFeedbackGate command_feedback_gate_;
    CommandFeedbackGate::Action command_feedback_action_ = CommandFeedbackGate::Action::None;
    std::string command_feedback_uuid_;
    std::chrono::steady_clock::time_point command_feedback_poll_at_{};
    std::chrono::steady_clock::time_point command_feedback_deadline_{};
    std::chrono::steady_clock::time_point command_feedback_retry_at_{};
    uint32_t command_feedback_failure_count_ = 0;
    bool command_feedback_suppression_unavailable_ = false;
    bool in_game_tick_ = false;
    // Dynamic-phase recovery can rewind through every preceding partition.
    // Key retries by work unit so that replaying those intermediate units
    // cannot reset the count for a permanently failing distant partition.
    std::map<ChunkCoord, std::array<uint32_t, kImportPhaseCount>> unit_retry_counts_;
    std::set<ChunkCoord> confirmed_region_chunks_;
    bool loaded_region_fully_confirmed_ = false;
    BlockBounds loaded_region_probe_bounds_{};
    std::chrono::steady_clock::time_point loaded_region_started_at_{};
    std::chrono::steady_clock::time_point region_wait_started_at_{};

    std::chrono::steady_clock::time_point telemetry_window_started_at_{};
    uint64_t telemetry_data_commands_ = 0;
    uint64_t telemetry_data_blocks_ = 0;
    uint64_t telemetry_rpc_batches_ = 0;
    uint64_t telemetry_rpc_commands_ = 0;
    uint64_t telemetry_python_send_microseconds_ = 0;
    uint64_t telemetry_batch_build_microseconds_ = 0;
    uint64_t telemetry_barrier_count_ = 0;
    uint64_t telemetry_barrier_microseconds_ = 0;
    uint64_t telemetry_region_wait_count_ = 0;
    uint64_t telemetry_region_wait_microseconds_ = 0;
    uint64_t telemetry_prefetch_starvations_ = 0;

    bool final_verification_started_ = false;
    bool verification_pending_ = false;
    std::vector<VerificationChunkPlan> verification_chunk_plans_;
    std::vector<VerificationPlanSample> verification_samples_;
    size_t verification_sample_index_ = 0;
    size_t verification_unverifiable_chunks_ = 0;
    size_t verification_verified_chunks_ = 0;
    std::chrono::steady_clock::time_point verification_sample_ready_at_{};
    BlockBounds verification_load_bounds_;
    std::optional<ChunkCoord> verification_region_coord_;
    std::optional<ChunkCoord> verification_readable_chunk_;
    uint32_t verification_region_load_retries_ = 0;
    std::optional<size_t> verification_mismatch_sample_index_;
    uint32_t verification_mismatch_observations_ = 0;
    std::optional<size_t> verification_server_probe_sample_index_;
    std::string verification_server_probe_uuid_;
    std::chrono::steady_clock::time_point verification_server_probe_poll_at_{};
    std::chrono::steady_clock::time_point verification_server_probe_deadline_{};
    std::optional<ChunkCoord> verification_repair_chunk_;
    VerificationRepairTracker verification_repair_tracker_;
    size_t verification_repair_sample_begin_ = 0;
    size_t verification_repair_phase_ = phaseIndex(ImportPhase::Structure);
    size_t verification_repair_chunk_cursor_ = 0;
    // Command-plan paths already replayed in the current repair phase; with
    // region spools many chunks share one plan file and this keeps the
    // duplicate check linear in the repair chunk count.
    std::set<std::string> verification_repair_replayed_paths_;
    std::vector<ChunkCoord> verification_repair_chunks_;
    std::vector<ChunkCoord> verification_repair_halo_chunks_;
    std::vector<PlannedCommand> verification_repair_clear_plan_;
    size_t verification_repair_clear_index_ = 0;
    size_t verification_repair_probe_cursor_ = 0;
    uint32_t verification_repair_probe_samples_ = 0;
    std::chrono::steady_clock::time_point verification_repair_probe_ready_at_{};
    std::chrono::steady_clock::time_point verification_repair_probe_deadline_{};
    uint64_t verification_repair_emitted_blocks_ = 0;
    uint32_t verification_repair_transport_failures_ = 0;
    bool verification_repair_force_drain_ = false;
    bool verification_repair_phase_dirty_ = false;
    std::chrono::steady_clock::time_point verification_repair_settle_ready_at_{};
    std::unique_ptr<CommandSpoolReader> verification_repair_reader_;
    // Repair only the chunks selected by the verification pass.
    std::set<ChunkCoord> verification_repair_reader_chunk_filter_;

    // Deferred command-block payloads are stored separately from the fill
    // command stream. A record is only advanced after its native update packet
    // has been handed to the captured LoopbackPacketSender. The sidecar is
    // flushed once per bounded game-tick burst; a crash can only replay that
    // idempotent tail, never skip an unsent record.
    std::unique_ptr<CommandBlockSpoolReader> command_block_reader_;
    std::optional<CommandBlockRecord> command_block_pending_record_;
    std::string command_block_spool_path_;
    std::string command_block_state_path_;
    BlockBounds command_block_cell_bounds_;
    BlockBounds command_block_load_window_bounds_;
    uint64_t command_block_record_count_ = 0;
    uint64_t command_block_cursor_ = 0;
    uint64_t command_block_persisted_cursor_ = 0;
    int32_t command_block_cell_x_ = 0;
    int32_t command_block_cell_y_ = 0;
    int32_t command_block_cell_z_ = 0;
    uint32_t command_block_cell_load_retries_ = 0;
    uint32_t command_block_target_retries_ = 0;
    bool command_block_cell_target_verified_ = false;
    bool command_block_cell_window_reused_ = false;
    std::chrono::steady_clock::time_point command_block_cell_ready_at_{};
    std::chrono::steady_clock::time_point command_block_cell_deadline_{};
    bool command_block_writer_active_ = false;
    bool command_block_writer_completed_ = false;
    // This reader is used only from onGameTick. Reusing it avoids rebuilding
    // BlockLegacy metadata for every deferred CommandBlockUpdatePacket.
    NativeWorldReader command_block_world_reader_;

    // Sign block-actor payloads are applied after command blocks and before
    // containers. A cursor advances only after a native snapshot confirms all
    // exported persistent fields, so an interrupted write can be replayed.
    std::unique_ptr<SignSpoolReader> sign_reader_;
    std::optional<SignRecord> sign_pending_record_;
    std::string sign_state_path_;
    uint64_t sign_record_count_ = 0;
    uint64_t sign_cursor_ = 0;
    uint64_t sign_persisted_cursor_ = 0;
    int32_t sign_cell_x_ = 0;
    int32_t sign_cell_y_ = 0;
    int32_t sign_cell_z_ = 0;
    BlockBounds sign_cell_bounds_;
    uint32_t sign_target_attempts_ = 0;
    std::chrono::steady_clock::time_point sign_target_ready_at_{};
    std::string sign_shell_probe_uuid_;
    std::chrono::steady_clock::time_point sign_shell_probe_poll_at_{};
    std::chrono::steady_clock::time_point sign_shell_probe_deadline_{};
    std::chrono::steady_clock::time_point sign_edit_session_deadline_{};
    std::chrono::steady_clock::time_point sign_verify_poll_at_{};
    std::chrono::steady_clock::time_point sign_verify_deadline_{};
    std::string sign_last_verification_error_;
    uint64_t sign_edit_session_token_ = 0;
    uint64_t next_sign_edit_session_token_ = 1;
    uint32_t sign_edit_probe_index_ = 0;
    bool sign_front_written_ = false;
    bool sign_back_written_ = false;
    bool sign_active_face_front_ = true;
    bool sign_face_includes_waxed_ = false;
    bool sign_writer_active_ = false;
    bool sign_writer_completed_ = false;
    NativeWorldReader sign_world_reader_;

    // Container inventory records are emitted only after the placed shell has
    // passed final verification. The durable cursor is advanced only after the
    // server acknowledges the absolute /replaceitem command, so a restart may
    // replay one idempotent slot but can never skip it.
    std::unique_ptr<ContainerItemSpoolReader> container_item_reader_;
    std::optional<ContainerItemRecord> container_item_pending_record_;
    std::string container_item_state_path_;
    uint64_t container_item_record_count_ = 0;
    uint64_t container_item_cursor_ = 0;
    uint64_t container_item_persisted_cursor_ = 0;
    int32_t container_item_cell_x_ = 0;
    int32_t container_item_cell_y_ = 0;
    int32_t container_item_cell_z_ = 0;
    BlockBounds container_item_cell_bounds_;
    int32_t container_item_target_x_ = 0;
    int32_t container_item_target_y_ = 0;
    int32_t container_item_target_z_ = 0;
    uint32_t container_item_target_retries_ = 0;
    std::string container_item_pending_uuid_;
    std::chrono::steady_clock::time_point container_item_cell_ready_at_{};
    std::chrono::steady_clock::time_point container_item_next_target_ready_at_{};
    std::chrono::steady_clock::time_point container_item_ack_poll_at_{};
    std::chrono::steady_clock::time_point container_item_ack_deadline_{};
    bool container_item_writer_active_ = false;
    bool container_item_writer_completed_ = false;

    // Entity payloads contain only a normalized entity type, location,
    // rotation and sanitized display name. Their cursor is separate from the
    // block plan because entity creation must never enter /fill repair spools.
    std::unique_ptr<EntitySpoolReader> entity_reader_;
    std::optional<EntityRecord> entity_pending_record_;
    std::string entity_state_path_;
    uint64_t entity_record_count_ = 0;
    uint64_t entity_cursor_ = 0;
    uint64_t entity_persisted_cursor_ = 0;
    int32_t entity_cell_x_ = 0;
    int32_t entity_cell_y_ = 0;
    int32_t entity_cell_z_ = 0;
    BlockBounds entity_cell_bounds_;
    uint32_t entity_cell_load_retries_ = 0;
    std::string entity_pending_uuid_;
    std::chrono::steady_clock::time_point entity_ack_poll_at_{};
    std::chrono::steady_clock::time_point entity_ack_deadline_{};
    std::chrono::steady_clock::time_point entity_cell_ready_at_{};
    std::chrono::steady_clock::time_point entity_cell_deadline_{};
    bool entity_writer_active_ = false;
    bool entity_writer_completed_ = false;

    // Automatic map creation is a post-import operation. Its tiny cursor is
    // kept beside the import spool so a paused task can resume at the next
    // map without replaying already-created maps.
    bool map_creation_requested_ = false;
    bool map_creation_active_ = false;
    bool map_creation_completed_ = false;
    // Latched at map-stage entry so toggling the opt-in property cannot switch
    // an in-flight storage transaction back to the inventory-only path.
    bool map_storage_pipeline_active_ = false;
    BlockBounds map_source_bounds_;
    uint64_t map_tile_columns_ = 0;
    uint64_t map_tile_rows_ = 0;
    uint64_t map_tile_count_ = 0;
    uint64_t map_tile_cursor_ = 0;
    uint64_t map_tile_persisted_cursor_ = 0;
    int32_t map_surface_y_ = 0;
    int32_t map_target_x_ = 0;
    int32_t map_target_z_ = 0;
    uint32_t map_slot_attempts_ = 0;
    MapInventoryTransfer map_inventory_transfer_;
    // Process-local source identity for a renamed map being moved/selected.
    // It never authorizes a chest Place; that step re-reads UUID and title.
    int32_t map_storage_renamed_transfer_network_id_ = 0;
    int64_t map_storage_renamed_transfer_uuid_ = -1;
    uint64_t map_storage_renamed_transfer_cursor_ = 0;
    // A durable sidecar owns the non-idempotent /give operation; these fields
    // only pace ACK/readback polling in the current process and authorize no
    // resend after a paused run or crash.
    std::string map_blank_supply_active_uuid_;
    bool map_blank_supply_ack_received_ = false;
    std::chrono::steady_clock::time_point map_blank_supply_poll_at_{};
    std::chrono::steady_clock::time_point map_blank_supply_deadline_{};
    int32_t map_use_hotbar_slot_ = -1;
    int32_t map_use_network_stack_id_ = 0;
    uint16_t map_use_empty_count_before_ = 0;
    uint32_t map_use_filled_total_before_ = 0;
    std::array<int32_t, 36> map_use_filled_network_ids_before_{};
    int32_t map_use_new_filled_network_id_ = 0;
    int64_t map_use_new_filled_uuid_ = -1;
    int32_t map_use_held_filled_network_id_ = 0;
    uint64_t map_use_texture_sequence_before_ = 0;
    bool map_use_item_confirmed_ = false;
    // The storage driver is inert unless the storage test gate was latched.
    // A tracked RPC ACK is consumed by pollTrackedCommands, so retain only
    // the current placement command's result until its native readback has
    // been journaled. The cache is process-local and never authorizes resend.
    MapStoragePipelineDriver map_storage_driver_;
    std::chrono::steady_clock::time_point map_storage_log_at_{};
    std::unique_ptr<MapStorageChestCaptureAdapter> map_storage_chest_capture_;
    MapChestStrictPreflightContext map_storage_chest_preflight_;
#if defined(__ANDROID__)
    MapChestNativePreflight map_storage_chest_native_preflight_;
#endif
    WorldContext map_storage_chest_world_;
    uintptr_t map_storage_chest_dimension_token_ = 0;
    bool map_storage_chest_stop_requested_ = false;
    bool map_storage_chest_terminal_failure_ = false;
    uint64_t map_storage_chest_client_sync_ticket_ = 0;
    uint64_t map_storage_chest_client_sync_token_ = 0;
    int32_t map_storage_chest_client_sync_request_id_ = 0;
    bool map_storage_chest_client_sync_confirmed_ = false;
    std::chrono::steady_clock::time_point map_storage_chest_client_sync_deadline_{};
    std::string map_storage_chest_client_sync_error_;
    std::unique_ptr<MapVisibleAnvilWindowSession> map_storage_anvil_window_;
    MapVisibleAnvilStrictPreflightContext map_storage_anvil_preflight_;
#if defined(__ANDROID__)
    MapVisibleAnvilNativePreflight map_storage_anvil_native_preflight_;
#endif
    WorldContext map_storage_anvil_world_;
    uintptr_t map_storage_anvil_dimension_token_ = 0;
    uint64_t map_storage_anvil_token_ = 0;
    bool map_storage_anvil_stop_requested_ = false;
    bool map_storage_anvil_local_close_requested_ = false;
    bool map_storage_anvil_handoff_ready_ = false;
    bool map_storage_anvil_probe_disarmed_ = false;
    bool map_storage_anvil_retirement_reported_ = false;
    bool map_storage_anvil_terminal_failure_ = false;
    bool map_storage_anvil_name_attempted_ = false;
    bool map_storage_anvil_name_submitted_ = false;
    std::string map_storage_anvil_name_title_;
    std::chrono::steady_clock::time_point map_storage_anvil_retirement_started_at_{};
    std::chrono::steady_clock::time_point map_storage_anvil_local_close_started_at_{};
    // A prior task may have closed both container windows while their native
    // lifecycle objects are still retiring. Bound the next-task wait instead
    // of mistaking an owned Completed session for an open window.
    std::chrono::steady_clock::time_point map_storage_prior_window_wait_started_at_{};
    std::string map_storage_rpc_ack_uuid_;
    MapPairCommandAck map_storage_rpc_ack_ = MapPairCommandAck::Pending;
    std::string map_storage_rpc_poll_uuid_;
    std::chrono::steady_clock::time_point map_storage_rpc_poll_deadline_{};
    // A /tp is idempotent, but its delivery is asynchronous. Hold one target
    // until native player position proves arrival or a bounded timeout pauses
    // the task; never open a chest/anvil using the old map-center position.
    MapChestPosition map_storage_travel_target_{};
    bool map_storage_travel_pending_ = false;
    std::chrono::steady_clock::time_point map_storage_travel_ready_at_{};
    std::chrono::steady_clock::time_point map_storage_travel_deadline_{};
    // A pre-placement landing is distinct from the later confirmed-chest
    // interaction travel. Never dispatch a fill at an exterior site while
    // the player is still at the map centre and its chunk may be unloaded.
    MapChestPosition map_storage_placement_target_{};
    MapChestPosition map_storage_placement_floor_{};
    bool map_storage_placement_pending_ = false;
    bool map_storage_placement_ready_ = false;
    std::chrono::steady_clock::time_point map_storage_placement_ready_at_{};
    std::chrono::steady_clock::time_point map_storage_placement_arrival_at_{};
    std::chrono::steady_clock::time_point map_storage_placement_deadline_{};
    std::chrono::steady_clock::time_point map_storage_placement_log_at_{};
    std::string map_state_path_;
    std::chrono::steady_clock::time_point map_target_ready_at_{};
    std::chrono::steady_clock::time_point map_target_deadline_{};
    std::chrono::steady_clock::time_point map_slot_retry_at_{};
    std::chrono::steady_clock::time_point map_inventory_deadline_{};
    std::chrono::steady_clock::time_point map_settle_ready_at_{};
    std::chrono::steady_clock::time_point map_settle_log_at_{};
    std::chrono::steady_clock::time_point map_texture_log_at_{};
    std::chrono::steady_clock::time_point map_use_deadline_{};
    std::chrono::steady_clock::time_point map_use_hold_ready_at_{};

    std::chrono::steady_clock::time_point chunk_load_probe_deadline_{};
    std::chrono::steady_clock::time_point chunk_early_probe_at_{};
    std::chrono::steady_clock::time_point chunk_ready_at_{};
    uint32_t chunk_readable_samples_ = 0;
    uint32_t chunk_required_readable_samples_ = 3;
    bool chunk_loaded_ = false;
    BlockBounds server_chunk_probe_bounds_{};
    std::vector<std::string> server_chunk_probe_uuids_;
    std::chrono::steady_clock::time_point server_chunk_probe_poll_at_{};
    std::chrono::steady_clock::time_point server_chunk_probe_retry_at_{};
    std::chrono::steady_clock::time_point server_chunk_probe_deadline_{};
    bool server_chunk_probe_confirmed_ = false;
    bool server_chunk_probe_transport_failed_ = false;
    bool server_chunk_probe_ack_timed_out_ = false;

    bool rpc_transport_ready_ = false;
    bool rpc_pointer_transport_ready_ = false;
    bool rpc_pointer_transport_disabled_ = false;
    std::string last_rpc_send_debug_;
    std::string last_rpc_poll_debug_;
    std::atomic<bool> native_rpc_ack_observed_{false};
    uint64_t next_rpc_uuid_ = 1;
    std::vector<std::string> drain_barrier_uuids_;
    std::chrono::steady_clock::time_point drain_barrier_poll_at_{};
    std::chrono::steady_clock::time_point drain_barrier_deadline_{};
    std::chrono::steady_clock::time_point drain_barrier_sent_at_{};
    std::chrono::steady_clock::time_point drain_settle_ready_at_{};
    bool drain_barrier_confirmed_ = false;
    std::string drain_failure_;
    std::mutex native_event_mutex_;
    LevelChunkEvidenceCache received_level_chunks_;
    std::map<std::string, bool> received_rpc_acks_;
    std::string pending_command_error_;
    std::atomic<bool> native_context_change_pending_{false};
    uintptr_t native_dimension_token_ = 0;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BUILD_IMPORT_RUNTIME_H
