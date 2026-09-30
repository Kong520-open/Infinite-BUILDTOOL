#ifndef INFINITE_TEXTURE_MAP_CHEST_WINDOW_SESSION_H
#define INFINITE_TEXTURE_MAP_CHEST_WINDOW_SESSION_H

#include "ContainerCaptureMailbox.h"

#include <cstdint>
#include <string>

namespace build_import {

// One window only. The caller owns the game-thread/exclusive-mailbox/native
// block preflight and must keep the same world and block identity while the
// session runs. A new session must never be started while another capture is
// armed. The native Block pointer is deliberately NOT stored in this request:
// it must be re-read on the very tick that emits ClickBlock.
struct MapChestWindowRequest {
    uint64_t token = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int face = 1;
};

// Production callbacks should delegate to ContainerOpenPacketSender (the
// packet-only ClickBlock path, never a local screen), the capture mailbox,
// and ContainerClosePacketSender. All callbacks run on the game tick thread.
// prepare_open must enforce exclusive ownership of the process-wide mailbox,
// live world, range, and return a freshly checked single-chest native Block
// pointer. That pointer is used immediately by send_open on the same tick.
struct MapChestWindowOps {
    void* context = nullptr;
    bool (*prepare_open)(void*, const MapChestWindowRequest&, const void**,
                         std::string*) = nullptr;
    bool (*send_open)(void*, const MapChestWindowRequest&, const void*,
                      std::string*) = nullptr;
    bool (*send_close)(void*, uint8_t, uint8_t, std::string*) = nullptr;
    void (*arm_capture)(void*, uint64_t, int32_t, int32_t, int32_t) = nullptr;
    void (*cancel_capture)(void*, uint64_t) = nullptr;
    ContainerCapturePollState (*poll_capture)(
        void*, uint64_t, ContainerCaptureResult*) = nullptr;
    bool (*poll_quarantine)(void*, ContainerCaptureQuarantine*) = nullptr;
    void (*mark_quarantine_close_sent)(void*, uint8_t, uint8_t) = nullptr;
};

enum class MapChestWindowState : uint8_t {
    Idle,
    WaitingForQuarantine,
    WaitingForOpen,
    WaitingForContent,
    CapturedOpen,
    WaitingForServerClose,
    Quarantining,
    Completed,
    Failed,
    CloseFailed,
};

class MapChestWindowSession final {
public:
    // begin merely registers an intent. tick first settles any older mailbox
    // quarantine, then arms the mailbox before emitting exactly one ClickBlock.
    bool begin(const MapChestWindowRequest& request, const MapChestWindowOps& ops,
               uint64_t now_ms, std::string* error = nullptr);
    MapChestWindowState tick(uint64_t now_ms);

    // A successful capture remains open for a short bounded lease so the
    // caller may submit ONE separately journalled ItemStackRequest. This
    // class never sends, retries, or infers success for that request. Calling
    // close (or lease expiry) sends at most one client close, waits for the
    // exact server Close while retaining the capture, then quarantines its
    // late packet tail. An explicit second send is permitted only after the
    // native close ABI rejected the first attempt.
    bool close(uint64_t now_ms, std::string* error = nullptr);

    // The stock client has already been asked to close the visible chest UI
    // (for example by its Back key path). Do not send another ContainerClose:
    // retain this exact window capture until its matching server Close arrives.
    // A missing or mismatched receipt fails closed and never authorizes reopen.
    bool expectUiClose(uint64_t now_ms, std::string* error = nullptr);

    // After the stock Back was actually dispatched, the caller may use this
    // ONCE if no matching outbound ContainerClose was observed. It sends an
    // exact close only while this same captured 27-slot window is still open;
    // a failed native send is uncertain and is never retried automatically.
    bool sendExactCloseAfterUiBack(uint64_t now_ms,
                                   std::string* error = nullptr);

    // The sender hook observed this exact window's outbound Close after the
    // stock Back, and the caller allowed a bounded settle interval. An inbound
    // Close is optional in this game version: release the visible capture so
    // a distinct read-only reopen can prove the stored map. This never sends.
    bool acceptObservedOutboundUiClose(uint64_t now_ms,
                                       std::string* error = nullptr);

    // For a hidden, read-only proof window only: after its complete 27-slot
    // capture has been retained by the caller and its ONE direct Close send
    // returned successfully, allow completion without an inbound Close echo
    // after a bounded isolation. This does not prove any map identity; the
    // caller must verify its retained fresh capture separately.
    bool acceptSubmittedCloseWithoutInbound(uint64_t now_ms,
                                            std::string* error = nullptr);

    MapChestWindowState state() const noexcept { return state_; }
    const ContainerCaptureResult* capture() const noexcept;
    const std::string& error() const noexcept { return error_; }

    static constexpr uint64_t kQuarantineTimeoutMs = 2500;
    static constexpr uint64_t kOpenTimeoutMs = 4000;
    static constexpr uint64_t kContentTimeoutMs = 4000;
    static constexpr uint64_t kCaptureLeaseMs = 15000;
    static constexpr uint64_t kServerCloseTimeoutMs = 4000;
    static constexpr uint64_t kUiCloseAwaitTimeoutMs = 8000;
    static constexpr uint64_t kNoEchoCloseIsolationMs = 500;

private:
    bool serviceQuarantine();
    bool closeObservedWindow(uint64_t now_ms, MapChestWindowState terminal);
    void cancelAndQuarantine(uint64_t now_ms, MapChestWindowState terminal);
    void fail(const std::string& reason, uint64_t now_ms,
              const ContainerCaptureResult* observed);
    bool validateOpenIdentity(const ContainerCaptureResult& observed) const;
    bool matchesOpenedWindow(const ContainerCaptureResult& observed) const;
    bool validateChestCapture(const ContainerCaptureResult& observed) const;

    bool started_ = false;
    bool capture_armed_ = false;
    bool close_sent_ = false;
    bool ui_close_expected_ = false;
    bool ui_fallback_attempted_ = false;
    uint64_t deadline_ms_ = 0;
    uint64_t close_sent_at_ms_ = 0;
    MapChestWindowState state_ = MapChestWindowState::Idle;
    MapChestWindowState after_quarantine_ = MapChestWindowState::Failed;
    MapChestWindowRequest request_{};
    MapChestWindowOps ops_{};
    ContainerCaptureResult capture_{};
    uint8_t opened_id_ = 0;
    uint8_t opened_type_ = 0;
    bool opened_ = false;
    bool server_closed_ = false;
    std::string error_;
};

#if defined(__ANDROID__)
// Optional production wiring to the same packet-only sender/mailbox used by
// BuildExportRuntime. The caller must supply and retain a preflight context
// for the whole session; this factory itself does not arm or open anything.
struct MapChestNativePreflight {
    void* context = nullptr;
    bool (*prepare_open)(void*, const MapChestWindowRequest&, const void**,
                         std::string*) = nullptr;
};
MapChestWindowOps MakeNativeMapChestWindowOps(
    MapChestNativePreflight* preflight) noexcept;
#endif

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_CHEST_WINDOW_SESSION_H
