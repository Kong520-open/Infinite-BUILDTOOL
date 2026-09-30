#ifndef INFINITE_TEXTURE_BUILD_EXPORT_RUNTIME_H
#define INFINITE_TEXTURE_BUILD_EXPORT_RUNTIME_H

#include "BuildExportCheckpoint.h"
#include "CommandBlockSpool.h"
#include "NativeWorldAccess.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace build_import {

struct ContainerCaptureResult;

enum class BuildExportState : uint8_t {
    Idle = 0,
    Preparing = 1,
    LoadingRegion = 2,
    Scanning = 3,
    Writing = 4,
    Completed = 5,
    Failed = 6,
    Cancelled = 7,
    // Kept out of the original 0..7 range so existing Java state mappings
    // remain valid. Manual exports pause here until the player reaches the
    // active scan region.
    WaitingForPlayer = 8,
    // Voxel scanning has finished and coordinate-specific container contents
    // are being requested from the server one container at a time.
    CapturingContainers = 9,
};

// How the exporter moves the local player between scan regions.  Keep these
// values stable: they are passed across the JNI boundary and are persisted by
// the UI as a user-facing choice.
enum class BuildExportTravelMode : int32_t {
    Automatic = 1,
    SemiAutomatic = 2,
    Disabled = 3,
};

inline bool isValidBuildExportTravelMode(int32_t value) noexcept {
    return value >= static_cast<int32_t>(BuildExportTravelMode::Automatic) &&
        value <= static_cast<int32_t>(BuildExportTravelMode::Disabled);
}

struct BuildExportStartRequest {
    std::string output_path;
    int32_t first_x = 0;
    int32_t first_y = 0;
    int32_t first_z = 0;
    int32_t second_x = 0;
    int32_t second_y = 0;
    int32_t second_z = 0;
    // Stable logical identity returned by TpModule.getWorldId(). It is used
    // only for cross-process checkpoint authorization; dimension_token_ still
    // detects live in-process dimension switches.
    std::string world_id;
    int32_t dimension_id = 0;
    // Explicit UI choice. Automatic is the default so a newly-created request
    // matches the export form's default option; Disabled remains available as
    // the explicit no-teleport mode. Each requested TP still proves server
    // permission by observing a real move to the target region.
    BuildExportTravelMode travel_mode = BuildExportTravelMode::Automatic;
    // Explicit UI choice for "restart". Existing checkpoint artifacts are
    // preserved unless this is true and all request validation/allocation has
    // already succeeded.
    bool replace_checkpoint = false;
    // Number of vanilla 16x16 chunks along each horizontal side of one scan
    // region.  A value of 4 therefore produces a 64x64-block scan region.
    // Kept explicit in the request rather than inferred from view distance so
    // exports remain predictable on servers with a reduced simulation range.
    int32_t simulation_chunk_range = 4;
    // When false, containers are exported as blocks only.  No block-entity
    // inventory payload is retained and no ContainerOpen interaction is sent.
    bool export_container_items = true;
};

// Coordinates and progress information for the region the player should
// approach in manual (non-teleporting) mode. The target is valid while an
// export has a current scan batch; distance is horizontal, rounded to blocks,
// and -1 means the player position is currently unavailable.
struct BuildExportTravelTarget {
    bool valid = false;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int32_t distance_blocks = -1;
    bool waiting = false;
    uint32_t batch_index = 0;
    uint32_t batch_count = 0;
};

class BuildExportRuntime {
public:
    static constexpr uint64_t kMaximumBlockCount = 16ULL * 1024ULL * 1024ULL;

    static BuildExportRuntime& instance();
    ~BuildExportRuntime();

    bool start(BuildExportStartRequest request, std::string* error = nullptr);
    bool resume(const std::string& output_path, const std::string& world_id,
                int32_t dimension_id, BuildExportTravelMode travel_mode,
                std::string* error = nullptr);
    // Compatibility overload for native callers from the pre-mode API.
    bool resume(const std::string& output_path, const std::string& world_id,
                int32_t dimension_id, bool allow_teleport,
                bool teleport_requested, std::string* error = nullptr);
    bool hasCheckpoint(const std::string& output_path) const;
    bool discardCheckpoint(const std::string& output_path,
                           std::string* error = nullptr);
    void onGameTick();
    // Refreshes only the cached local-player position used by UI corner/anchor
    // capture.  It never advances an export state machine and is safe to call
    // for the limited, import-only building-tools profile.
    void refreshPlayerPositionCache();
    // Requests a game-thread refresh and waits only for the shared cache.  This
    // keeps JNI callers off the Python/world path while making a first position
    // capture reliable before an export has begun.
    bool awaitPlayerBlockPosition(int32_t* x, int32_t* y, int32_t* z,
                                  std::chrono::milliseconds timeout);
    void cancel();

    BuildExportState state() const;
    std::string status() const;
    uint64_t totalBlockCount() const;
    uint64_t processedBlockCount() const;
    bool playerBlockPosition(int32_t* x, int32_t* y, int32_t* z) const;
    bool teleportAllowed() const;
    BuildExportTravelMode travelMode() const;
    BuildExportTravelMode teleportMode() const { return travelMode(); }
    // Grants one semi-automatic region permission and starts the existing
    // bounded-hop TP verification. It is intentionally a no-op for automatic
    // and disabled modes so a stale overlay click cannot move the player.
    bool requestNextRegionTeleport();
    bool waitingForPlayer() const;
    bool travelTarget(BuildExportTravelTarget* output) const;
    // Convenience form for JNI callers that prefer primitive out parameters.
    bool travelTarget(int32_t* x, int32_t* y, int32_t* z,
                      int32_t* distance_blocks, bool* waiting) const;

private:
#if defined(BUILD_IMPORT_RUNTIME_TESTS)
    friend struct BuildExportRuntimeContainerCaptureTestAccess;
#endif

    struct CachedBlockStateKey {
        const void* type_token = nullptr;
        uint16_t aux = 0;

        bool operator==(const CachedBlockStateKey& other) const {
            return type_token == other.type_token && aux == other.aux;
        }
    };

    struct CachedBlockStateKeyHash {
        size_t operator()(const CachedBlockStateKey& key) const {
            size_t seed = std::hash<const void*>{}(key.type_token);
            seed ^= std::hash<uint16_t>{}(key.aux) + static_cast<size_t>(0x9e3779b9U) +
                (seed << 6U) + (seed >> 2U);
            return seed;
        }
    };

    struct ScanBatch {
        int32_t min_x = 0;
        int32_t max_x = -1;
        int32_t min_z = 0;
        int32_t max_z = -1;
        uint64_t cursor = 0;
        uint64_t processed_start = 0;
        uint64_t exported_start = 0;
        size_t palette_start = 1;
        size_t raw_start = 0;
        uint32_t retries = 0;
        uint32_t readable_samples = 0;
        int wait_ticks = 0;
        bool manual_retry_wait = false;
        // Runtime-only state. A checkpoint is always resumed as a fresh
        // verification request, never as a stale in-flight server command.
        bool teleport_verification_pending = false;
    };

    struct ContainerCaptureTarget {
        uint64_t block_index = 0;
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
    };

    enum class ContainerCapturePhase : uint8_t {
        Inactive = 0,
        Navigate = 1,
        WaitingForTeleport = 2,
        Settling = 3,
        WaitingForPackets = 4,
        Cooldown = 5,
    };

    BuildExportRuntime() = default;
    BuildExportRuntime(const BuildExportRuntime&) = delete;
    BuildExportRuntime& operator=(const BuildExportRuntime&) = delete;

    void updateCachedPlayerPosition();
    void beginBatchLoadLocked(bool reset_progress = true);
    void retryBatchLocked(const std::string& reason);
    bool requestBatchTeleportLocked(ScanBatch& batch);
    bool shrinkTeleportHopLimitLocked();
    bool batchReadableLocked(NativeWorldReader* reader) const;
    bool scanBatchLocked(NativeWorldReader* reader);
    bool rebuildContainerTargetsLocked(std::string* error = nullptr);
    bool beginContainerCaptureLocked();
    void tickContainerCaptureLocked();
    void retryContainerCaptureLocked(const std::string& reason);
    void completeContainerBatchLocked();
    void resetContainerCaptureLocked() noexcept;
    bool playerNearCurrentContainerLocked(int32_t* distance_blocks = nullptr) const;
    bool validateCurrentContainerLocked(const void** native_block,
                                         bool* changed,
                                         std::string* error = nullptr);
    bool applyContainerCaptureLocked(const ContainerCaptureResult& result,
                                     std::string* error = nullptr);
    uint64_t nextContainerCaptureTokenLocked();
    bool playerNearCurrentBatchLocked(int32_t* distance_blocks = nullptr) const;
    void setWaitingForPlayerStatusLocked(const std::string& reason = {});
    void disableTeleportLocked(const std::string& reason);
    void beginWriteLocked();
    void rebuildCommandBlocksFromPaletteLocked();
    void failLocked(const std::string& reason);
    bool commitCheckpointLocked(bool force, std::string* error = nullptr);
    bool appendJournalLocked(const std::vector<uint16_t>& values,
                             std::string* error = nullptr);
    bool replayJournalLocked(uint64_t entry_count, std::string* error = nullptr);
    bool restoreSnapshotLocked(BuildExportCheckpointSnapshot snapshot,
                               std::string output_path,
                               std::string checkpoint_path,
                               BuildExportTravelMode travel_mode,
                               std::string* error);
    static bool createBatches(int32_t min_x, int32_t max_x, int32_t min_z,
                               int32_t max_z, int32_t height,
                               int32_t batch_size,
                               std::vector<ScanBatch>* batches,
                               std::string* error = nullptr);
    static uint64_t batchVolume(const ScanBatch& batch, int32_t height);
    static bool isStableWorldContext(const std::string& world_id,
                                     int32_t dimension_id);
    void releaseScanMemoryLocked();
    void setStatus(const std::string& status);
    static bool nativeRawIdentityAuthoritative(std::string_view name);
    static uint16_t rawSnapshotAuxForExport(uint16_t native_aux,
                                            bool client_aux_available,
                                            uint16_t client_aux,
                                            std::string_view leaf = {},
                                            std::string_view state_json = {});
    static bool isPlaceholderName(const std::string& name);
    static bool isActiveState(BuildExportState state);

    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex status_mutex_;
    mutable std::mutex position_mutex_;
    std::condition_variable position_refresh_condition_;
    // Serializes the last cancellation check with final .infinity publication.
    std::shared_ptr<std::mutex> publish_mutex_ = std::make_shared<std::mutex>();
    std::atomic<BuildExportState> state_{BuildExportState::Idle};
    std::atomic<uint64_t> generation_{0};
    std::atomic<bool> writer_running_{false};
    std::atomic<bool> publication_committed_{false};
    std::thread writer_thread_;

    std::string status_;
    // New checkpoints and journals are keyed by this extensionless
    // user-selected stem. Publication always appends the canonical .infinity
    // extension.
    std::string output_path_;
    // Normally equal to output_path_. A resumed legacy checkpoint may keep an
    // old extension-bearing sidecar key until it is completed or discarded,
    // while output_path_ remains the normalized publication stem.
    std::string checkpoint_path_;
    std::string published_output_path_;
    std::string display_name_;
    int32_t min_x_ = 0;
    int32_t min_y_ = 0;
    int32_t min_z_ = 0;
    int32_t max_x_ = -1;
    int32_t max_y_ = -1;
    int32_t max_z_ = -1;
    int32_t width_ = 0;
    int32_t height_ = 0;
    int32_t length_ = 0;
    std::string world_id_;
    int32_t dimension_id_ = 0;
    uintptr_t dimension_token_ = 0;
    uint32_t missing_player_ticks_ = 0;
    // In automatic mode this remains true for the lifetime of the active
    // export. In semi-automatic mode it is a per-region grant set by
    // requestNextRegionTeleport() (or by detecting that the player arrived
    // manually) and cleared when that region is complete.
    bool allow_teleport_ = false;
    BuildExportTravelMode travel_mode_ = BuildExportTravelMode::Automatic;
    bool teleport_permission_unverified_ = false;
    // Bounded-hop travel toward the active batch centre. Some servers reject a
    // single /tp spanning more than an allowed distance; 0 means "try the
    // direct teleport first" and the limit halves after each rejected probe.
    int32_t teleport_hop_limit_ = 0;
    bool hop_target_valid_ = false;
    bool hop_target_final_ = false;
    float hop_target_x_ = 0.0f;
    float hop_target_y_ = 0.0f;
    float hop_target_z_ = 0.0f;

    // Horizontal side length of the active scan region, in blocks.  This is
    // serialized through BuildExportCheckpointSnapshot::batch_size so an
    // interrupted export is resumed with exactly the same traversal plan.
    // 64 is the legacy/default value (four vanilla chunks per side).
    int32_t batch_size_ = 64;

    std::vector<ScanBatch> batches_;
    size_t current_batch_ = 0;
    NativeWorldReader world_reader_;
    uint64_t block_state_source_generation_ = 0;
    std::vector<std::string> palette_;
    std::unordered_map<std::string, uint32_t> palette_ids_;
    std::unordered_map<CachedBlockStateKey, uint32_t,
                       CachedBlockStateKeyHash> block_state_ids_;
    // State JSON is shared by all coordinates that reference the same native
    // Block instance; an empty value is still cached as an attempted query so
    // a client-only SDK does not invoke Python once per voxel.
    std::unordered_map<CachedBlockStateKey, std::string,
                       CachedBlockStateKeyHash> raw_state_json_;
    // GetBlock() is normally the stable SDK authority for legacy identity and
    // aux. Some stateful families are flattened by that API; their native
    // identity policy is applied when raw records are materialized below.
    std::unordered_map<CachedBlockStateKey, std::string,
                       CachedBlockStateKeyHash> raw_client_identifiers_;
    std::unordered_map<CachedBlockStateKey, uint16_t,
                       CachedBlockStateKeyHash> raw_client_aux_;
    std::unordered_set<CachedBlockStateKey, CachedBlockStateKeyHash>
        raw_state_attempted_;
    std::vector<uint16_t> block_indices_;
    std::vector<SchematicRawBlock> raw_blocks_;
    std::unordered_map<uint64_t, size_t> raw_block_lookup_;
    // This is a per-export content policy, persisted in the checkpoint so a
    // resumed export cannot unexpectedly begin opening containers.
    bool export_container_items_ = true;
    std::vector<ContainerCaptureTarget> container_targets_;
    uint64_t container_cursor_ = 0;
    uint32_t container_attempt_ = 0;
    uint64_t container_request_sequence_ = 0;
    uint64_t container_request_token_ = 0;
    uint8_t container_id_ = 0;
    uint8_t container_type_ = 0;
    int container_wait_ticks_ = 0;
    bool container_capture_pending_ = false;
    bool container_window_open_ = false;
    bool checkpoint_data_dirty_ = false;
    ContainerCapturePhase container_phase_ = ContainerCapturePhase::Inactive;
    // Command-block text is not part of the compact block journal. Keep the
    // records separately so a BDX writer can preserve the editable payload;
    // resumed checkpoints rebuild the structural records from the palette.
    std::vector<CommandBlockRecord> command_blocks_;
    // Per-tick journal staging buffer, retained so its capacity is reused.
    std::vector<uint16_t> journal_scratch_;
    std::atomic<uint64_t> total_blocks_{0};
    std::atomic<uint64_t> processed_blocks_{0};
    uint64_t exported_blocks_ = 0;
    uint64_t journal_entries_ = 0;
    uint64_t committed_journal_entries_ = 0;
    size_t committed_batch_ = 0;
    uint64_t committed_batch_cursor_ = 0;
    size_t committed_palette_size_ = 0;
    uint64_t committed_exported_blocks_ = 0;
    size_t committed_raw_size_ = 0;
    bool checkpoint_initialized_ = false;
    bool checkpoint_commit_in_progress_ = false;
    bool preserve_cursor_on_prepare_ = false;
    std::chrono::steady_clock::time_point last_checkpoint_at_{};

    bool cached_position_valid_ = false;
    int32_t cached_x_ = 0;
    int32_t cached_y_ = 0;
    int32_t cached_z_ = 0;
    std::chrono::steady_clock::time_point cached_position_at_{};
    bool position_refresh_requested_ = false;
    uint64_t position_refresh_generation_ = 0;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BUILD_EXPORT_RUNTIME_H
