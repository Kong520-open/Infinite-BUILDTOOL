#ifndef INFINITE_TEXTURE_MAP_STORAGE_CHEST_CAPTURE_ADAPTER_H
#define INFINITE_TEXTURE_MAP_STORAGE_CHEST_CAPTURE_ADAPTER_H

#include "MapChestPlacement.h"
#include "MapChestWindowSession.h"

#include <cstdint>
#include <string>

namespace build_import {

// One adapter belongs to one map-storage job and is driven only from its
// authorized LocalPlayer tick. It does not submit or retry ItemStackRequest.
// The caller must keep this object alive, call tick even while its coordinator
// is between steps, and call stop when the job is cancelled or paused.
struct MapStorageChestCaptureConfig {
    MapChestWindowOps window_ops;
    // Both callbacks are mandatory. try_arm_capture must be an atomic,
    // exclusive claim (production: TryArmHiddenChestCapture), not the legacy
    // void ArmContainerCapture. verify_context must compare the original
    // world ID/dimension and authorized game tick before open AND close.
    // prepare_open additionally checks player range and a freshly read
    // ordinary-chest native Block pointer on that same tick.
    void* guard_context = nullptr;
    bool (*try_arm_capture)(void*, uint64_t, int32_t, int32_t, int32_t) = nullptr;
    bool (*verify_context)(void*, const MapChestPosition&, uint64_t,
                           std::string*) = nullptr;
    void* token_context = nullptr;
    uint64_t (*next_token)(void*) = nullptr;
    // Only the first, visible transfer window needs a local screen Back.
    // request_ui_close queues that Back for this exact window; a queued
    // request is not proof that the game actually dispatched it. The adapter
    // waits for ui_close_dispatched and either the exact inbound Close or a
    // captured exact outbound Close plus a short isolation before opening a
    // distinct hidden proof window. A missing outbound after Back permits
    // only one same-window direct fallback; no Place is ever resent.
    // cancel_ui_close is optional and retires a still-queued Back on failure.
    void* ui_context = nullptr;
    bool (*request_ui_close)(void*, uint64_t, uint8_t, uint8_t) = nullptr;
    bool (*ui_close_dispatched)(void*, uint64_t) = nullptr;
    bool (*ui_close_outbound_observed)(void*, uint64_t) = nullptr;
    void (*cancel_ui_close)(void*, uint64_t) = nullptr;
    int click_face = 1;
};

enum class MapStorageChestCaptureState : uint8_t {
    Idle,
    OpeningInitial,
    InitialReady,
    ClosingInitial,
    OpeningReopen,
    ClosingReopen,
    ProofReady,
    Stopping,
    Stopped,
    Failed,
    ClosedAfterPlace,
};

class MapStorageChestCaptureAdapter final {
public:
    // Give the server's newly opened container a short settle interval before
    // the first map Place. This remains inside the window's 15-second lease.
    static constexpr uint64_t kInitialContentSettleMs = 300U;
    static constexpr uint64_t kUiCloseDispatchTimeoutMs = 5000U;
    static constexpr uint64_t kUiCloseRetireSettleMs = 250U;
    static constexpr uint64_t kUiCloseNativeGraceMs = 400U;
    static constexpr uint64_t kUiCloseOutboundSettleMs = 500U;
    static constexpr uint64_t kUiCloseOutboundTimeoutMs = 2500U;

    MapStorageChestCaptureAdapter() = default;
    MapStorageChestCaptureAdapter(const MapStorageChestCaptureAdapter&) = delete;
    MapStorageChestCaptureAdapter& operator=(const MapStorageChestCaptureAdapter&) = delete;
    MapStorageChestCaptureAdapter(MapStorageChestCaptureAdapter&&) = delete;
    MapStorageChestCaptureAdapter& operator=(MapStorageChestCaptureAdapter&&) = delete;

    bool configure(const MapStorageChestCaptureConfig& config,
                   std::string* error = nullptr);

    // `reopen=false` keeps the same captured, open window/token across the
    // Prepared-journal and Place-submission stages. `reopen=true` first closes
    // that window, drains packet quarantine, and opens a new window/token.
    // If this adapter was reconstructed after a process restart, `reopen=true`
    // can start a fresh proof window directly. A proof is returned only after
    // its own close has completed, and only once; another call opens anew.
    bool capture(const MapChestPosition& position, bool reopen, uint64_t now_ms,
                 ContainerCaptureResult* result, std::string* error = nullptr);

    // Close the one visible transfer window after the accepted Place without
    // opening a second (hidden) chest. An Idle adapter after process recovery
    // has no live window to close and succeeds without sending ClickBlock.
    bool closeAfterPlace(const MapChestPosition& position, uint64_t now_ms,
                         std::string* error = nullptr);

    // Must run on every live game tick, including while a durable journal
    // transition is pending, so an abandoned window reaches its close lease.
    MapStorageChestCaptureState tick(uint64_t now_ms) noexcept;
    void stop(uint64_t now_ms) noexcept;

    MapStorageChestCaptureState state() const noexcept { return state_; }
    // Only a failure before attempting ClickBlock is known not to have
    // created a server window. Other failures retain their guard until a
    // fresh process/session because packet-send failure can be ambiguous.
    bool safeToDiscardAfterFailure() const noexcept {
        return state_ == MapStorageChestCaptureState::Failed &&
            !open_packet_attempted_;
    }
    uint64_t initial_token() const noexcept { return initial_token_; }
    uint64_t reopen_token() const noexcept { return reopen_token_; }
    const std::string& error() const noexcept { return error_; }

private:
    bool beginWindow(const MapChestPosition& position, bool reopen,
                     uint64_t now_ms);
    bool requestInitialUiClose(uint64_t now_ms, std::string* error);
    void pollInitialUiClose(uint64_t now_ms);
    void retireInitialUiTicket();
    bool matchesPosition(const MapChestPosition& position) const noexcept;
    void fail(const std::string& reason);
    void drive(uint64_t now_ms);
    MapChestWindowOps wrappedOps() noexcept;

    static bool wrappedPrepare(void*, const MapChestWindowRequest&,
                               const void**, std::string*);
    static bool wrappedOpen(void*, const MapChestWindowRequest&, const void*,
                            std::string*);
    static bool wrappedClose(void*, uint8_t, uint8_t, std::string*);
    static void wrappedArm(void*, uint64_t, int32_t, int32_t, int32_t);
    static void wrappedCancel(void*, uint64_t);
    static ContainerCapturePollState wrappedPoll(
        void*, uint64_t, ContainerCaptureResult*);
    static bool wrappedPollQuarantine(void*, ContainerCaptureQuarantine*);
    static void wrappedMarkQuarantine(void*, uint8_t, uint8_t);

    MapStorageChestCaptureConfig config_{};
    MapChestWindowSession session_{};
    MapChestPosition position_{};
    ContainerCaptureResult proof_{};
    MapStorageChestCaptureState state_ = MapStorageChestCaptureState::Idle;
    uint64_t initial_token_ = 0;
    uint64_t reopen_token_ = 0;
    uint64_t active_token_ = 0;
    uint64_t last_token_ = 0;
    uint64_t last_tick_ms_ = 0;
    uint64_t initial_content_observed_at_ms_ = 0;
    uint64_t ui_close_deadline_ms_ = 0;
    uint64_t ui_close_ready_at_ms_ = 0;
    uint64_t ui_close_dispatched_at_ms_ = 0;
    uint64_t ui_close_outbound_observed_at_ms_ = 0;
    uint64_t reopen_close_submitted_at_ms_ = 0;
    uint8_t ui_close_window_id_ = 0;
    uint8_t ui_close_window_type_ = 0;
    bool ui_close_request_pending_ = false;
    bool ui_close_ticket_active_ = false;
    bool ui_close_dispatched_ = false;
    bool ui_close_outbound_observed_ = false;
    bool ui_close_fallback_attempted_ = false;
    bool configured_ = false;
    bool arm_succeeded_ = false;
    bool have_tick_ = false;
    bool position_set_ = false;
    bool open_packet_attempted_ = false;
    bool close_after_place_requested_ = false;
    std::string error_;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_STORAGE_CHEST_CAPTURE_ADAPTER_H
