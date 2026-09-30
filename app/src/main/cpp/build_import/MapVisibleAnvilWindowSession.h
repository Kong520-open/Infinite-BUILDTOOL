#ifndef INFINITE_TEXTURE_MAP_VISIBLE_ANVIL_WINDOW_SESSION_H
#define INFINITE_TEXTURE_MAP_VISIBLE_ANVIL_WINDOW_SESSION_H

#include "ContainerCaptureMailbox.h"

#include <cstdint>
#include <string>

namespace build_import {

// One automatic anvil window. The caller supplies an exclusive mailbox token
// and a live-world identity to its callbacks; a native Block pointer is never
// retained across ticks. The stock client sees this window briefly so its
// anvil controller can create the genuine recipe/output identities.
struct MapVisibleAnvilWindowRequest {
    uint64_t token = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int face = 1;
};

// All callbacks run on the local-player game thread except packet observation
// inside the capture mailbox. verify_context must reject a changed world or
// dimension. prepare_open must re-read the target as an actual anvil, check
// interaction range and return its fresh engine Block pointer. send_open and
// send_close are one-shot packet sends; false is treated as uncertain for
// close and never retried. arm_capture must be ArmVisibleAnvilCapture, not the
// hidden chest capture, and the mailbox must have exclusive ownership.
struct MapVisibleAnvilWindowOps {
    void* context = nullptr;
    bool (*verify_context)(void*, const MapVisibleAnvilWindowRequest&,
                           std::string*) = nullptr;
    bool (*prepare_open)(void*, const MapVisibleAnvilWindowRequest&,
                         const void**, std::string*) = nullptr;
    bool (*send_open)(void*, const MapVisibleAnvilWindowRequest&, const void*,
                      std::string*) = nullptr;
    bool (*send_close)(void*, uint8_t, uint8_t, std::string*) = nullptr;
    bool (*arm_capture)(void*, uint64_t, int32_t, int32_t, int32_t) = nullptr;
    void (*cancel_capture)(void*, uint64_t) = nullptr;
    ContainerCapturePollState (*poll_capture)(
        void*, uint64_t, ContainerCaptureResult*) = nullptr;
    bool (*poll_quarantine)(void*, ContainerCaptureQuarantine*) = nullptr;
    void (*mark_quarantine_close_sent)(void*, uint8_t, uint8_t) = nullptr;
};

enum class MapVisibleAnvilWindowState : uint8_t {
    Idle,
    WaitingForQuarantine,
    WaitingForOpen,
    WaitingForContent,
    Open,
    Quarantining,
    Completed,
    Failed,
    CloseUncertain,
};

class MapVisibleAnvilWindowSession final {
public:
    bool begin(const MapVisibleAnvilWindowRequest& request,
               const MapVisibleAnvilWindowOps& ops, uint64_t now_ms,
               std::string* error = nullptr);
    MapVisibleAnvilWindowState tick(uint64_t now_ms);

    // The caller may submit separately journalled input/craft/output requests
    // while Open. This class never sends those requests or infers their ACK.
    // close() emits at most one client close before cancelling capture.
    bool close(uint64_t now_ms, std::string* error = nullptr);
    void abort(uint64_t now_ms, const std::string& reason);

    MapVisibleAnvilWindowState state() const noexcept { return state_; }
    uint8_t openedWindowId() const noexcept { return opened_ ? opened_id_ : 0U; }
    const ContainerCaptureResult* capture() const noexcept;
    // Re-poll the exclusive mailbox on the caller's authorized game tick and
    // re-verify world/dimension before a native UI controller is borrowed.
    // Unlike capture(), this does not return a potentially stale Open snapshot.
    bool verifiedLiveCapture(uint64_t ticket, uint64_t now_ms,
                             ContainerCaptureResult* output,
                             std::string* error = nullptr) const;
    const std::string& error() const noexcept { return error_; }

    static constexpr uint64_t kQuarantineTimeoutMs = 2500;
    static constexpr uint64_t kOpenTimeoutMs = 4000;
    static constexpr uint64_t kContentTimeoutMs = 4000;
    static constexpr uint64_t kOpenLeaseMs = 15000;
    static constexpr uint64_t kCloseUncertainTimeoutMs = 2500;

private:
    bool serviceQuarantine();
    void cancelCapture(uint64_t now_ms, MapVisibleAnvilWindowState terminal);
    bool closeObservedWindow(uint64_t now_ms, MapVisibleAnvilWindowState terminal);
    void fail(const std::string& reason, uint64_t now_ms,
              const ContainerCaptureResult* observed = nullptr);
    bool validateOpenIdentity(const ContainerCaptureResult& observed) const;
    bool validateEmptyAnvil(const ContainerCaptureResult& observed) const;

    bool started_ = false;
    bool capture_armed_ = false;
    bool close_attempted_ = false;
    bool opened_ = false;
    bool server_closed_ = false;
    uint8_t opened_id_ = 0;
    uint64_t deadline_ms_ = 0;
    MapVisibleAnvilWindowState state_ = MapVisibleAnvilWindowState::Idle;
    MapVisibleAnvilWindowState after_quarantine_ = MapVisibleAnvilWindowState::Failed;
    MapVisibleAnvilWindowRequest request_{};
    MapVisibleAnvilWindowOps ops_{};
    ContainerCaptureResult capture_{};
    std::string error_;
};

#if defined(__ANDROID__)
// Production adapter: the caller supplies a preflight that pins world and
// validates a fresh anvil Block pointer on the game tick. This factory never
// opens a window by itself and never enables the map pipeline.
struct MapVisibleAnvilNativePreflight {
    void* context = nullptr;
    bool (*verify_context)(void*, const MapVisibleAnvilWindowRequest&,
                           std::string*) = nullptr;
    bool (*prepare_open)(void*, const MapVisibleAnvilWindowRequest&,
                         const void**, std::string*) = nullptr;
};
MapVisibleAnvilWindowOps MakeNativeMapVisibleAnvilWindowOps(
    MapVisibleAnvilNativePreflight* preflight) noexcept;
#endif

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_VISIBLE_ANVIL_WINDOW_SESSION_H
