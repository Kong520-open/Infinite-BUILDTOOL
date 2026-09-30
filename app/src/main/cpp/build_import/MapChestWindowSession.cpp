#include "MapChestWindowSession.h"

#include <array>
#include <limits>
#include <utility>

#if defined(__ANDROID__)
#include "ContainerClosePacketSender.h"
#include "ContainerOpenPacketSender.h"
#include <android/log.h>
#endif

namespace build_import {
namespace {

uint64_t addDeadline(uint64_t now, uint64_t duration) noexcept {
    return now > std::numeric_limits<uint64_t>::max() - duration
        ? std::numeric_limits<uint64_t>::max() : now + duration;
}

bool callbacksComplete(const MapChestWindowOps& ops) noexcept {
    return ops.prepare_open && ops.send_open && ops.send_close &&
        ops.arm_capture && ops.cancel_capture && ops.poll_capture &&
        ops.poll_quarantine && ops.mark_quarantine_close_sent;
}

void logCloseEvent(const char* event, uint64_t token, uint8_t id,
                   uint8_t type) noexcept {
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "Infinitecz_MapChest",
        "[close] %s token=%llu window=%u type=%u", event,
        static_cast<unsigned long long>(token),
        static_cast<unsigned>(id), static_cast<unsigned>(type));
#else
    (void)event;
    (void)token;
    (void)id;
    (void)type;
#endif
}

}  // namespace

bool MapChestWindowSession::begin(const MapChestWindowRequest& request,
                                  const MapChestWindowOps& ops,
                                  uint64_t now_ms, std::string* error) {
    if (error) error->clear();
    if (started_ || request.token == 0U ||
        request.face < 0 || request.face > 5 || !callbacksComplete(ops)) {
        if (error) *error = "invalid or already-used chest window session";
        return false;
    }
    started_ = true;
    request_ = request;
    ops_ = ops;
    deadline_ms_ = addDeadline(now_ms, kQuarantineTimeoutMs);
    state_ = MapChestWindowState::WaitingForQuarantine;
    return true;
}

bool MapChestWindowSession::serviceQuarantine() {
    ContainerCaptureQuarantine quarantine;
    if (!ops_.poll_quarantine(ops_.context, &quarantine)) return false;
    if (quarantine.container_opened && !quarantine.container_closed &&
        !quarantine.content_captured && !quarantine.close_sent) {
        std::string close_error;
        const bool closed = ops_.send_close(ops_.context, quarantine.container_id,
                                            quarantine.container_type, &close_error);
        // This is a one-shot best-effort close of a delayed open. Never spin
        // the native close ABI on every tick if it is unavailable.
        ops_.mark_quarantine_close_sent(ops_.context, quarantine.container_id,
                                        quarantine.container_type);
        if (!closed) {
            error_ = close_error.empty()
                ? "could not close a quarantined late chest window"
                : "could not close a quarantined late chest window: " + close_error;
            state_ = MapChestWindowState::Failed;
        }
    }
    return true;
}

bool MapChestWindowSession::validateOpenIdentity(
        const ContainerCaptureResult& observed) const {
    return observed.token == request_.token &&
        observed.x == request_.x && observed.y == request_.y &&
        observed.z == request_.z && observed.container_opened &&
        observed.container_id != 0U && observed.container_id != 0xFFU &&
        observed.container_type == 0U;
}

bool MapChestWindowSession::matchesOpenedWindow(
        const ContainerCaptureResult& observed) const {
    return opened_ && observed.token == request_.token &&
        observed.x == request_.x && observed.y == request_.y &&
        observed.z == request_.z && observed.container_opened &&
        observed.container_id == opened_id_ &&
        observed.container_type == opened_type_;
}

bool MapChestWindowSession::validateChestCapture(
        const ContainerCaptureResult& observed) const {
    if (!validateOpenIdentity(observed) || observed.container_closed ||
        !observed.error.empty() || observed.slot_count != 27U ||
        !observed.has_full_container_name || observed.full_container_name != 0U ||
        observed.has_dynamic_container_id || observed.dynamic_container_id != 0U ||
        observed.items.size() > 27U) return false;
    std::array<bool, 27U> occupied{};
    for (const auto& item : observed.items) {
        if (item.slot >= occupied.size() || occupied[item.slot] ||
            item.numeric_id == 0 || item.count == 0U) return false;
        occupied[item.slot] = true;
    }
    return true;
}

void MapChestWindowSession::cancelAndQuarantine(
        uint64_t now_ms, MapChestWindowState terminal) {
    if (capture_armed_) {
        ops_.cancel_capture(ops_.context, request_.token);
        capture_armed_ = false;
    }
    after_quarantine_ = terminal;
    deadline_ms_ = addDeadline(now_ms, kQuarantineTimeoutMs);
    state_ = MapChestWindowState::Quarantining;
}

bool MapChestWindowSession::closeObservedWindow(
        uint64_t now_ms, MapChestWindowState terminal) {
    if (opened_ && !server_closed_ && !close_sent_) {
        if (opened_id_ == 0U || opened_id_ == 0xFFU) {
            error_ += error_.empty() ? "window ID is invalid; manual close required"
                                     : "; window ID is invalid; manual close required";
            state_ = MapChestWindowState::CloseFailed;
            return false;
        }
        std::string close_error;
        if (!ops_.send_close(ops_.context, opened_id_, opened_type_, &close_error)) {
            logCloseEvent("send-failed", request_.token, opened_id_, opened_type_);
            error_ += error_.empty() ? "native chest close failed"
                                     : "; native chest close failed";
            if (!close_error.empty()) error_ += ": " + close_error;
            state_ = MapChestWindowState::CloseFailed;
            return false;
        }
        close_sent_ = true;
        close_sent_at_ms_ = now_ms;
        logCloseEvent("sent-awaiting-server", request_.token,
                      opened_id_, opened_type_);
    }
    if (opened_ && !server_closed_) {
        after_quarantine_ = terminal;
        deadline_ms_ = addDeadline(now_ms, kServerCloseTimeoutMs);
        state_ = MapChestWindowState::WaitingForServerClose;
        return true;
    }
    cancelAndQuarantine(now_ms, terminal);
    return true;
}

void MapChestWindowSession::fail(const std::string& reason, uint64_t now_ms,
                                 const ContainerCaptureResult* observed) {
    error_ = reason;
    if (observed && observed->container_opened) {
        opened_ = true;
        opened_id_ = observed->container_id;
        opened_type_ = observed->container_type;
        server_closed_ = observed->container_closed;
    }
    (void)closeObservedWindow(now_ms, MapChestWindowState::Failed);
}

MapChestWindowState MapChestWindowSession::tick(uint64_t now_ms) {
    if (state_ == MapChestWindowState::Idle ||
        state_ == MapChestWindowState::Completed ||
        state_ == MapChestWindowState::Failed) return state_;

    if (state_ == MapChestWindowState::WaitingForQuarantine ||
        state_ == MapChestWindowState::Quarantining) {
        const bool waiting = serviceQuarantine();
        if (state_ == MapChestWindowState::Failed) return state_;
        if (waiting) {
            if (now_ms >= deadline_ms_) {
                error_ = "container packet quarantine did not settle in time";
                state_ = MapChestWindowState::Failed;
            }
            return state_;
        }
        if (state_ == MapChestWindowState::Quarantining) {
            state_ = after_quarantine_;
            return state_;
        }
        std::string preflight_error;
        const void* native_block = nullptr;
        if (!ops_.prepare_open(ops_.context, request_, &native_block,
                               &preflight_error) || !native_block) {
            error_ = preflight_error.empty()
                ? "single-chest native preflight rejected the request"
                : std::move(preflight_error);
            state_ = MapChestWindowState::Failed;
            return state_;
        }
        ops_.arm_capture(ops_.context, request_.token,
                         request_.x, request_.y, request_.z);
        capture_armed_ = true;
        std::string send_error;
        if (!ops_.send_open(ops_.context, request_, native_block,
                            &send_error)) {
            fail(send_error.empty() ? "packet-only chest open failed" : send_error,
                 now_ms, nullptr);
            return state_;
        }
        deadline_ms_ = addDeadline(now_ms, kOpenTimeoutMs);
        state_ = MapChestWindowState::WaitingForOpen;
        return state_;
    }

    ContainerCaptureResult observed;
    const ContainerCapturePollState poll =
        ops_.poll_capture(ops_.context, request_.token, &observed);
    if (state_ == MapChestWindowState::WaitingForServerClose ||
        state_ == MapChestWindowState::CloseFailed) {
        // Keep the hidden mailbox armed until this exact window has actually
        // closed on the server. Otherwise a fresh ClickBlock may be sent
        // while the server still owns the previous container session.
        if (poll != ContainerCapturePollState::Inactive &&
            matchesOpenedWindow(observed) && observed.container_closed) {
            server_closed_ = true;
            logCloseEvent("server-confirmed", request_.token,
                          opened_id_, opened_type_);
            cancelAndQuarantine(now_ms, after_quarantine_);
        } else if (state_ == MapChestWindowState::WaitingForServerClose &&
                   now_ms >= deadline_ms_) {
            error_ += error_.empty()
                ? "server did not confirm the exact chest close"
                : "; server did not confirm the exact chest close";
            after_quarantine_ = MapChestWindowState::Failed;
            state_ = MapChestWindowState::CloseFailed;
            logCloseEvent("server-timeout", request_.token,
                          opened_id_, opened_type_);
        }
        return state_;
    }
    if (poll == ContainerCapturePollState::Inactive) {
        // Another owner may have overwritten the process-wide mailbox. Do
        // not close an unrelated current window using a stale numeric ID.
        error_ = "chest capture mailbox lost exclusive ownership";
        state_ = MapChestWindowState::Failed;
        capture_armed_ = false;
        return state_;
    }
    if (observed.container_opened) {
        opened_ = true;
        opened_id_ = observed.container_id;
        opened_type_ = observed.container_type;
        server_closed_ = observed.container_closed;
        if (!validateOpenIdentity(observed)) {
            fail("opened window is not the requested single chest",
                 now_ms, &observed);
            return state_;
        }
    }
    if (poll == ContainerCapturePollState::Failed) {
        fail(observed.error.empty() ? "chest content packet was invalid"
                                    : observed.error, now_ms, &observed);
        return state_;
    }
    if (observed.container_closed) {
        fail("chest window closed before the operation finished",
             now_ms, &observed);
        return state_;
    }

    if (state_ == MapChestWindowState::CapturedOpen) {
        if (now_ms >= deadline_ms_) {
            fail("captured chest window lease expired", now_ms, &observed);
        }
        return state_;
    }
    if (poll == ContainerCapturePollState::Ready) {
        if (!validateChestCapture(observed)) {
            fail("chest content is not an ordinary 27-slot chest snapshot",
                 now_ms, &observed);
            return state_;
        }
        capture_ = std::move(observed);
        deadline_ms_ = addDeadline(now_ms, kCaptureLeaseMs);
        state_ = MapChestWindowState::CapturedOpen;
        return state_;
    }
    if (state_ == MapChestWindowState::WaitingForOpen &&
        poll == ContainerCapturePollState::WaitingForContent) {
        deadline_ms_ = addDeadline(now_ms, kContentTimeoutMs);
        state_ = MapChestWindowState::WaitingForContent;
    }
    if (now_ms >= deadline_ms_) {
        fail(state_ == MapChestWindowState::WaitingForOpen
                 ? "ContainerOpen packet timed out"
                 : "InventoryContent packet timed out",
             now_ms, &observed);
    }
    return state_;
}

bool MapChestWindowSession::close(uint64_t now_ms, std::string* error) {
    if (error) error->clear();
    if (ui_close_expected_) {
        if (error) *error = "visible chest UI close has already been delegated";
        return false;
    }
    if (state_ != MapChestWindowState::CapturedOpen &&
        state_ != MapChestWindowState::CloseFailed) {
        if (error) *error = "no captured chest window can be closed";
        return false;
    }
    if (state_ == MapChestWindowState::CloseFailed && close_sent_ &&
        !server_closed_) {
        if (error) *error = "chest close was already sent; its server result is uncertain";
        return false;
    }
    ContainerCaptureResult observed;
    const ContainerCapturePollState poll =
        ops_.poll_capture(ops_.context, request_.token, &observed);
    if (poll == ContainerCapturePollState::Inactive) {
        error_ = "chest capture mailbox lost exclusive ownership before close";
        state_ = MapChestWindowState::Failed;
        capture_armed_ = false;
        if (error) *error = error_;
        return false;
    }
    if (observed.container_opened) {
        if (!validateOpenIdentity(observed)) {
            fail("chest window changed before close", now_ms, &observed);
            if (error) *error = error_;
            return false;
        }
        server_closed_ = observed.container_closed;
    }
    const MapChestWindowState terminal =
        state_ == MapChestWindowState::CloseFailed
            ? MapChestWindowState::Failed : MapChestWindowState::Completed;
    const bool closed = closeObservedWindow(now_ms, terminal);
    if (!closed && error) *error = error_;
    return closed;
}

bool MapChestWindowSession::expectUiClose(uint64_t now_ms,
                                          std::string* error) {
    if (error) error->clear();
    if (state_ != MapChestWindowState::CapturedOpen) {
        if (error) *error = "no captured chest window can await a UI close";
        return false;
    }

    // The caller has delegated closing to the stock client's Back path. A
    // later fallback may send an exact Close ONCE, but only after that Back
    // was really dispatched and the original window is still live.
    ui_close_expected_ = true;
    ContainerCaptureResult observed;
    const ContainerCapturePollState poll =
        ops_.poll_capture(ops_.context, request_.token, &observed);
    if (poll == ContainerCapturePollState::Inactive) {
        error_ = "chest capture mailbox lost exclusive ownership before UI close";
        state_ = MapChestWindowState::Failed;
        capture_armed_ = false;
        if (error) *error = error_;
        return false;
    }
    if (!matchesOpenedWindow(observed)) {
        error_ = "chest window changed before UI close";
        after_quarantine_ = MapChestWindowState::Failed;
        state_ = MapChestWindowState::CloseFailed;
        if (error) *error = error_;
        return false;
    }

    after_quarantine_ = MapChestWindowState::Completed;
    deadline_ms_ = addDeadline(now_ms, kUiCloseAwaitTimeoutMs);
    state_ = MapChestWindowState::WaitingForServerClose;
    logCloseEvent("ui-close-awaiting-server", request_.token,
                  opened_id_, opened_type_);
    return true;
}

bool MapChestWindowSession::sendExactCloseAfterUiBack(
        uint64_t now_ms, std::string* error) {
    if (error) error->clear();
    if (!ui_close_expected_ || ui_fallback_attempted_ || close_sent_ ||
        state_ != MapChestWindowState::WaitingForServerClose) {
        if (error) *error = "visible chest exact-close fallback is not eligible";
        return false;
    }
    ContainerCaptureResult observed;
    const ContainerCapturePollState poll =
        ops_.poll_capture(ops_.context, request_.token, &observed);
    if (poll != ContainerCapturePollState::Inactive &&
        matchesOpenedWindow(observed) && observed.container_closed) {
        // A server close may race the caller's outbound observation. Consume
        // its exact receipt instead of sending a second close.
        (void)tick(now_ms);
        return true;
    }
    if (poll != ContainerCapturePollState::Ready ||
        !matchesOpenedWindow(observed) ||
        !validateChestCapture(observed)) {
        error_ = "visible chest identity changed before exact-close fallback";
        after_quarantine_ = MapChestWindowState::Failed;
        state_ = MapChestWindowState::CloseFailed;
        if (error) *error = error_;
        return false;
    }
    ui_fallback_attempted_ = true;
    close_sent_ = true;
    close_sent_at_ms_ = now_ms;
    std::string send_error;
    if (!ops_.send_close(ops_.context, opened_id_, opened_type_,
                         &send_error)) {
        error_ = "visible chest exact-close fallback send is uncertain";
        if (!send_error.empty()) error_ += ": " + send_error;
        after_quarantine_ = MapChestWindowState::Failed;
        state_ = MapChestWindowState::CloseFailed;
        logCloseEvent("ui-back-fallback-send-failed", request_.token,
                      opened_id_, opened_type_);
        if (error) *error = error_;
        return false;
    }
    deadline_ms_ = addDeadline(now_ms, kServerCloseTimeoutMs);
    logCloseEvent("ui-back-fallback-sent", request_.token,
                  opened_id_, opened_type_);
    return true;
}

bool MapChestWindowSession::acceptObservedOutboundUiClose(
        uint64_t now_ms, std::string* error) {
    if (error) error->clear();
    if (!ui_close_expected_ || state_ != MapChestWindowState::WaitingForServerClose) {
        if (error) *error = "visible chest outbound close cannot be accepted";
        return false;
    }
    ContainerCaptureResult observed;
    const ContainerCapturePollState poll =
        ops_.poll_capture(ops_.context, request_.token, &observed);
    if (poll != ContainerCapturePollState::Inactive &&
        matchesOpenedWindow(observed) && observed.container_closed) {
        (void)tick(now_ms);
        return true;
    }
    if (poll != ContainerCapturePollState::Ready ||
        !matchesOpenedWindow(observed) ||
        !validateChestCapture(observed)) {
        error_ = "visible chest changed before outbound close isolation";
        after_quarantine_ = MapChestWindowState::Failed;
        state_ = MapChestWindowState::CloseFailed;
        if (error) *error = error_;
        return false;
    }
    close_sent_ = true;
    logCloseEvent("outbound-observed-isolating", request_.token,
                  opened_id_, opened_type_);
    cancelAndQuarantine(now_ms, MapChestWindowState::Completed);
    return true;
}

bool MapChestWindowSession::acceptSubmittedCloseWithoutInbound(
        uint64_t now_ms, std::string* error) {
    if (error) error->clear();
    if (ui_close_expected_ || !close_sent_ ||
        state_ != MapChestWindowState::WaitingForServerClose ||
        now_ms < addDeadline(close_sent_at_ms_, kNoEchoCloseIsolationMs) ||
        !capture_.container_opened || capture_.container_closed ||
        capture_.token != request_.token ||
        capture_.container_id != opened_id_ ||
        capture_.container_type != opened_type_ ||
        !validateChestCapture(capture_)) {
        if (error) *error = "hidden proof close lacks a sent packet, exact capture or isolation";
        return false;
    }
    ContainerCaptureResult observed;
    const ContainerCapturePollState poll =
        ops_.poll_capture(ops_.context, request_.token, &observed);
    if (poll != ContainerCapturePollState::Inactive &&
        matchesOpenedWindow(observed) && observed.container_closed) {
        (void)tick(now_ms);
        return true;
    }
    if (poll != ContainerCapturePollState::Ready ||
        !matchesOpenedWindow(observed) ||
        !validateChestCapture(observed)) {
        error_ = "hidden proof window changed before no-echo close isolation";
        after_quarantine_ = MapChestWindowState::Failed;
        state_ = MapChestWindowState::CloseFailed;
        if (error) *error = error_;
        return false;
    }
    logCloseEvent("hidden-close-sent-isolating", request_.token,
                  opened_id_, opened_type_);
    cancelAndQuarantine(now_ms, MapChestWindowState::Completed);
    return true;
}

const ContainerCaptureResult* MapChestWindowSession::capture() const noexcept {
    return state_ == MapChestWindowState::CapturedOpen ? &capture_ : nullptr;
}

#if defined(__ANDROID__)
namespace {

bool nativePrepare(void* context, const MapChestWindowRequest& request,
                   const void** native_block, std::string* error) {
    const auto* preflight = static_cast<MapChestNativePreflight*>(context);
    if (!preflight || !preflight->prepare_open) {
        if (error) *error = "native chest preflight callback is missing";
        return false;
    }
    return preflight->prepare_open(preflight->context, request,
                                   native_block, error);
}

bool nativeOpen(void*, const MapChestWindowRequest& request,
                const void* native_block, std::string* error) {
    return ContainerOpenPacketSender::send(request.x, request.y, request.z,
                                           native_block, request.face, error);
}

bool nativeClose(void*, uint8_t id, uint8_t type, std::string* error) {
    return ContainerClosePacketSender::send(id, type, error);
}

void nativeArm(void*, uint64_t token, int32_t x, int32_t y, int32_t z) {
    ArmContainerCapture(token, x, y, z);
}

void nativeCancel(void*, uint64_t token) {
    CancelContainerCapture(token);
}

ContainerCapturePollState nativePoll(void*, uint64_t token,
                                     ContainerCaptureResult* result) {
    return PollContainerCapture(token, result);
}

bool nativePollQuarantine(void*, ContainerCaptureQuarantine* result) {
    return PollContainerCaptureQuarantine(result);
}

void nativeMarkQuarantine(void*, uint8_t id, uint8_t type) {
    MarkContainerCaptureQuarantineCloseSent(id, type);
}

}  // namespace

MapChestWindowOps MakeNativeMapChestWindowOps(
        MapChestNativePreflight* preflight) noexcept {
    return {preflight, &nativePrepare, &nativeOpen, &nativeClose,
            &nativeArm, &nativeCancel, &nativePoll,
            &nativePollQuarantine, &nativeMarkQuarantine};
}
#endif

}  // namespace build_import
