#include "MapVisibleAnvilWindowSession.h"

#include <limits>
#include <utility>

#if defined(__ANDROID__)
#include "ContainerClosePacketSender.h"
#include "ContainerOpenPacketSender.h"
#endif

namespace build_import {
namespace {

uint64_t addDeadline(uint64_t now, uint64_t duration) noexcept {
    return now > std::numeric_limits<uint64_t>::max() - duration
        ? std::numeric_limits<uint64_t>::max() : now + duration;
}

bool callbacksComplete(const MapVisibleAnvilWindowOps& ops) noexcept {
    return ops.verify_context && ops.prepare_open && ops.send_open &&
        ops.send_close && ops.arm_capture && ops.cancel_capture &&
        ops.poll_capture && ops.poll_quarantine &&
        ops.mark_quarantine_close_sent;
}

}  // namespace

bool MapVisibleAnvilWindowSession::begin(
        const MapVisibleAnvilWindowRequest& request,
        const MapVisibleAnvilWindowOps& ops, uint64_t now_ms,
        std::string* error) {
    if (error) error->clear();
    if (started_ || request.token == 0U || request.face < 0 ||
        request.face > 5 || !callbacksComplete(ops)) {
        if (error) *error = "invalid or already-used visible anvil session";
        return false;
    }
    started_ = true;
    request_ = request;
    ops_ = ops;
    deadline_ms_ = addDeadline(now_ms, kQuarantineTimeoutMs);
    state_ = MapVisibleAnvilWindowState::WaitingForQuarantine;
    return true;
}

bool MapVisibleAnvilWindowSession::serviceQuarantine() {
    ContainerCaptureQuarantine quarantine;
    if (!ops_.poll_quarantine(ops_.context, &quarantine)) return false;
    if (quarantine.container_opened && !quarantine.container_closed &&
        !quarantine.close_sent) {
        // A process-wide mailbox may still quarantine another owner's chest
        // or an old-world window. Wait for it to settle/expire; never close
        // somebody else's numeric window ID from this anvil session.
        if (quarantine.token != request_.token ||
            !quarantine.from_visible_anvil) return true;
        if (quarantine.container_type != 5U) {
            error_ = "late window is not an anvil; refusing numeric close";
            state_ = MapVisibleAnvilWindowState::Failed;
            return true;
        }
        std::string context_error;
        if (!ops_.verify_context(ops_.context, request_, &context_error)) {
            error_ = context_error.empty()
                ? "anvil world changed before late-open cleanup"
                : std::move(context_error);
            state_ = MapVisibleAnvilWindowState::Failed;
            return true;
        }
        if (quarantine.container_id == 0U ||
            quarantine.container_id == 0xFFU) {
            error_ = "late container has no safe window ID for cleanup";
            state_ = MapVisibleAnvilWindowState::Failed;
            return true;
        }
        std::string close_error;
        const bool sent = ops_.send_close(ops_.context, quarantine.container_id,
                                          quarantine.container_type,
                                          &close_error);
        // Mark before leaving even when native send failed. A close ABI may
        // have reached sendToServer before its outcome became uncertain.
        ops_.mark_quarantine_close_sent(ops_.context, quarantine.container_id,
                                        quarantine.container_type);
        if (!sent) {
            error_ = close_error.empty()
                ? "late container close outcome is uncertain"
                : "late container close outcome is uncertain: " + close_error;
            state_ = MapVisibleAnvilWindowState::Failed;
        }
    }
    return true;
}

bool MapVisibleAnvilWindowSession::validateOpenIdentity(
        const ContainerCaptureResult& observed) const {
    return observed.token == request_.token &&
        observed.x == request_.x && observed.y == request_.y &&
        observed.z == request_.z && observed.container_opened &&
        observed.container_id != 0U && observed.container_id != 0xFFU &&
        observed.container_type == 5U;
}

bool MapVisibleAnvilWindowSession::validateEmptyAnvil(
        const ContainerCaptureResult& observed) const {
    // The live manual trace proved type 5 and three slots. It did not prove
    // anvil FullContainerName, so do not equate it with request slot type 0.
    return validateOpenIdentity(observed) && !observed.container_closed &&
        observed.error.empty() && observed.slot_count == 3U &&
        observed.items.empty();
}

void MapVisibleAnvilWindowSession::cancelCapture(
        uint64_t now_ms, MapVisibleAnvilWindowState terminal) {
    if (capture_armed_) {
        ops_.cancel_capture(ops_.context, request_.token);
        capture_armed_ = false;
    }
    ContainerCaptureQuarantine quarantine;
    if (ops_.poll_quarantine(ops_.context, &quarantine)) {
        after_quarantine_ = terminal;
        deadline_ms_ = addDeadline(now_ms, kQuarantineTimeoutMs);
        state_ = MapVisibleAnvilWindowState::Quarantining;
    } else {
        state_ = terminal;
    }
}

bool MapVisibleAnvilWindowSession::closeObservedWindow(
        uint64_t now_ms, MapVisibleAnvilWindowState terminal) {
    if (!opened_ || server_closed_) {
        cancelCapture(now_ms, server_closed_
            ? MapVisibleAnvilWindowState::Failed : terminal);
        return !server_closed_;
    }
    if (opened_id_ == 0U || opened_id_ == 0xFFU) {
        error_ += error_.empty() ? "invalid anvil window ID"
                                 : "; invalid anvil window ID";
        cancelCapture(now_ms, MapVisibleAnvilWindowState::Failed);
        return false;
    }
    std::string context_error;
    if (!ops_.verify_context(ops_.context, request_, &context_error)) {
        error_ += error_.empty() ? "anvil world changed before close"
                                 : "; anvil world changed before close";
        if (!context_error.empty()) error_ += ": " + context_error;
        // A numeric window ID from the old world must never be sent to a new
        // world/session. Stock client teardown remains in charge there.
        cancelCapture(now_ms, MapVisibleAnvilWindowState::Failed);
        return false;
    }
    if (close_attempted_) return false;
    close_attempted_ = true;
    std::string close_error;
    if (!ops_.send_close(ops_.context, opened_id_, 5U, &close_error)) {
        error_ += error_.empty() ? "anvil close outcome is uncertain"
                                 : "; anvil close outcome is uncertain";
        if (!close_error.empty()) error_ += ": " + close_error;
        deadline_ms_ = addDeadline(now_ms, kCloseUncertainTimeoutMs);
        state_ = MapVisibleAnvilWindowState::CloseUncertain;
        return false;
    }
    // Critical ordering: the stock client received Open, so send its normal
    // client Close before cancelling the passive capture. Cancel must never
    // install a hidden quarantine over a visible anvil's late Slot/Close.
    cancelCapture(now_ms, terminal);
    return true;
}

void MapVisibleAnvilWindowSession::fail(
        const std::string& reason, uint64_t now_ms,
        const ContainerCaptureResult* observed) {
    error_ = reason;
    ContainerCaptureResult fresh;
    if (!observed && capture_armed_ &&
        ops_.poll_capture(ops_.context, request_.token, &fresh) !=
            ContainerCapturePollState::Inactive) {
        observed = &fresh;
    }
    if (observed && observed->container_opened &&
        validateOpenIdentity(*observed)) {
        opened_ = true;
        opened_id_ = observed->container_id;
        server_closed_ = observed->container_closed;
    }
    if (opened_ && !server_closed_) {
        (void)closeObservedWindow(now_ms, MapVisibleAnvilWindowState::Failed);
    } else {
        cancelCapture(now_ms, MapVisibleAnvilWindowState::Failed);
    }
}

MapVisibleAnvilWindowState MapVisibleAnvilWindowSession::tick(uint64_t now_ms) {
    if (state_ == MapVisibleAnvilWindowState::Idle ||
        state_ == MapVisibleAnvilWindowState::Completed ||
        state_ == MapVisibleAnvilWindowState::Failed) return state_;

    if (state_ == MapVisibleAnvilWindowState::WaitingForQuarantine ||
        state_ == MapVisibleAnvilWindowState::Quarantining) {
        const bool waiting = serviceQuarantine();
        if (state_ == MapVisibleAnvilWindowState::Failed) return state_;
        if (waiting) {
            if (now_ms >= deadline_ms_) {
                error_ = "container quarantine did not settle in time";
                state_ = MapVisibleAnvilWindowState::Failed;
            }
            return state_;
        }
        if (state_ == MapVisibleAnvilWindowState::Quarantining) {
            state_ = after_quarantine_;
            return state_;
        }
        std::string preflight_error;
        if (!ops_.verify_context(ops_.context, request_, &preflight_error)) {
            error_ = preflight_error.empty()
                ? "anvil world context changed before open"
                : std::move(preflight_error);
            state_ = MapVisibleAnvilWindowState::Failed;
            return state_;
        }
        const void* native_block = nullptr;
        if (!ops_.prepare_open(ops_.context, request_, &native_block,
                               &preflight_error) || !native_block) {
            error_ = preflight_error.empty()
                ? "fresh native anvil preflight rejected the request"
                : std::move(preflight_error);
            state_ = MapVisibleAnvilWindowState::Failed;
            return state_;
        }
        if (!ops_.arm_capture(ops_.context, request_.token,
                              request_.x, request_.y, request_.z)) {
            error_ = "visible anvil mailbox is already owned";
            state_ = MapVisibleAnvilWindowState::Failed;
            return state_;
        }
        capture_armed_ = true;
        std::string send_error;
        if (!ops_.send_open(ops_.context, request_, native_block,
                            &send_error)) {
            fail(send_error.empty() ? "anvil ClickBlock failed" : send_error,
                 now_ms);
            return state_;
        }
        deadline_ms_ = addDeadline(now_ms, kOpenTimeoutMs);
        state_ = MapVisibleAnvilWindowState::WaitingForOpen;
        return state_;
    }

    ContainerCaptureResult observed;
    const ContainerCapturePollState poll =
        ops_.poll_capture(ops_.context, request_.token, &observed);
    if (poll == ContainerCapturePollState::Inactive) {
        error_ = "visible anvil mailbox lost exclusive ownership";
        capture_armed_ = false;
        state_ = MapVisibleAnvilWindowState::Failed;
        return state_;
    }
    if (observed.container_opened) {
        if (!validateOpenIdentity(observed)) {
            error_ = "opened window does not match the requested anvil";
            cancelCapture(now_ms, MapVisibleAnvilWindowState::Failed);
            return state_;
        }
        opened_ = true;
        opened_id_ = observed.container_id;
        server_closed_ = observed.container_closed;
    }
    std::string context_error;
    if (!ops_.verify_context(ops_.context, request_, &context_error)) {
        error_ = context_error.empty()
            ? "anvil world context changed"
            : std::move(context_error);
        // Never send a stale window number into a different world.
        cancelCapture(now_ms, MapVisibleAnvilWindowState::Failed);
        return state_;
    }
    if (state_ == MapVisibleAnvilWindowState::CloseUncertain) {
        if (server_closed_ || now_ms >= deadline_ms_) {
            cancelCapture(now_ms, MapVisibleAnvilWindowState::Failed);
        }
        return state_;
    }
    if (poll == ContainerCapturePollState::Failed) {
        fail(observed.error.empty() ? "anvil content packet was invalid"
                                    : observed.error, now_ms, &observed);
        return state_;
    }
    if (server_closed_) {
        fail("anvil window closed before automatic rename finished",
             now_ms, &observed);
        return state_;
    }
    if (state_ == MapVisibleAnvilWindowState::Open) {
        if (now_ms >= deadline_ms_) {
            fail("visible anvil window lease expired", now_ms, &observed);
        }
        return state_;
    }
    if (poll == ContainerCapturePollState::Ready) {
        if (!validateEmptyAnvil(observed)) {
            fail("anvil is not a fresh empty three-slot window",
                 now_ms, &observed);
            return state_;
        }
        capture_ = std::move(observed);
        deadline_ms_ = addDeadline(now_ms, kOpenLeaseMs);
        state_ = MapVisibleAnvilWindowState::Open;
        return state_;
    }
    if (state_ == MapVisibleAnvilWindowState::WaitingForOpen &&
        poll == ContainerCapturePollState::WaitingForContent) {
        deadline_ms_ = addDeadline(now_ms, kContentTimeoutMs);
        state_ = MapVisibleAnvilWindowState::WaitingForContent;
    }
    if (now_ms >= deadline_ms_) {
        fail(state_ == MapVisibleAnvilWindowState::WaitingForOpen
                 ? "anvil ContainerOpen timed out"
                 : "anvil InventoryContent timed out",
             now_ms, &observed);
    }
    return state_;
}

bool MapVisibleAnvilWindowSession::close(uint64_t now_ms,
                                         std::string* error) {
    if (error) error->clear();
    if (state_ != MapVisibleAnvilWindowState::Open) {
        if (error) *error = "no verified open anvil window can be closed";
        return false;
    }
    ContainerCaptureResult observed;
    const ContainerCapturePollState poll =
        ops_.poll_capture(ops_.context, request_.token, &observed);
    if (poll == ContainerCapturePollState::Inactive) {
        error_ = "visible anvil mailbox lost ownership before close";
        capture_armed_ = false;
        state_ = MapVisibleAnvilWindowState::Failed;
    } else if (!validateOpenIdentity(observed)) {
        error_ = "anvil window changed before close";
        cancelCapture(now_ms, MapVisibleAnvilWindowState::Failed);
    } else if (observed.container_closed) {
        error_ = "anvil window was closed before automatic close";
        server_closed_ = true;
        cancelCapture(now_ms, MapVisibleAnvilWindowState::Failed);
    } else if (poll == ContainerCapturePollState::Failed) {
        fail(observed.error.empty() ? "anvil capture failed before close"
                                    : observed.error, now_ms, &observed);
    } else if (closeObservedWindow(now_ms,
                                   MapVisibleAnvilWindowState::Completed)) {
        return true;
    }
    if (error) *error = error_;
    return false;
}

void MapVisibleAnvilWindowSession::abort(uint64_t now_ms,
                                         const std::string& reason) {
    if (state_ == MapVisibleAnvilWindowState::Idle ||
        state_ == MapVisibleAnvilWindowState::Completed ||
        state_ == MapVisibleAnvilWindowState::Failed ||
        state_ == MapVisibleAnvilWindowState::CloseUncertain) return;
    if (state_ == MapVisibleAnvilWindowState::WaitingForQuarantine) {
        error_ = reason.empty() ? "anvil window cancelled" : reason;
        state_ = MapVisibleAnvilWindowState::Failed;
        return;
    }
    if (state_ == MapVisibleAnvilWindowState::Quarantining) {
        after_quarantine_ = MapVisibleAnvilWindowState::Failed;
        if (!reason.empty()) error_ = reason;
        return;
    }
    fail(reason.empty() ? "anvil window cancelled" : reason, now_ms);
}

const ContainerCaptureResult* MapVisibleAnvilWindowSession::capture() const noexcept {
    return state_ == MapVisibleAnvilWindowState::Open ? &capture_ : nullptr;
}

bool MapVisibleAnvilWindowSession::verifiedLiveCapture(
        uint64_t ticket, uint64_t now_ms, ContainerCaptureResult* output,
        std::string* error) const {
    if (output) *output = {};
    if (error) error->clear();
    if (!output || !ticket || state_ != MapVisibleAnvilWindowState::Open ||
        !capture_armed_ || !opened_ || server_closed_ ||
        now_ms >= deadline_ms_ ||
        ticket != request_.token ||
        !validateEmptyAnvil(capture_) ||
        capture_.container_id != opened_id_) {
        if (error) *error = "anvil window lease or initial capture is unavailable";
        return false;
    }
    std::string context_error;
    if (!ops_.verify_context(ops_.context, request_, &context_error)) {
        if (error) *error = context_error.empty()
            ? "anvil world or dimension changed"
            : std::move(context_error);
        return false;
    }
    ContainerCaptureResult current;
    const ContainerCapturePollState poll =
        ops_.poll_capture(ops_.context, ticket, &current);
    // Once the input map is placed, the content is no longer empty. The
    // window identity, three-slot shape, and numeric ID must remain exact.
    if (poll != ContainerCapturePollState::Ready ||
        !validateOpenIdentity(current) || current.container_closed ||
        !current.error.empty() || current.slot_count != 3U ||
        current.container_id != opened_id_) {
        if (error) *error = "fresh anvil window capture changed or closed";
        return false;
    }
    *output = std::move(current);
    return true;
}

#if defined(__ANDROID__)
namespace {

bool nativeVerifyContext(void* context,
                         const MapVisibleAnvilWindowRequest& request,
                         std::string* error) {
    const auto* preflight = static_cast<MapVisibleAnvilNativePreflight*>(context);
    if (!preflight || !preflight->verify_context) {
        if (error) *error = "native anvil world verifier is missing";
        return false;
    }
    return preflight->verify_context(preflight->context, request, error);
}

bool nativePrepareOpen(void* context,
                       const MapVisibleAnvilWindowRequest& request,
                       const void** native_block, std::string* error) {
    const auto* preflight = static_cast<MapVisibleAnvilNativePreflight*>(context);
    if (!preflight || !preflight->prepare_open) {
        if (error) *error = "native anvil Block preflight is missing";
        return false;
    }
    return preflight->prepare_open(preflight->context, request,
                                   native_block, error);
}

bool nativeOpen(void*, const MapVisibleAnvilWindowRequest& request,
                const void* native_block, std::string* error) {
    return ContainerOpenPacketSender::send(request.x, request.y, request.z,
                                           native_block, request.face, error);
}

bool nativeClose(void*, uint8_t id, uint8_t type, std::string* error) {
    return ContainerClosePacketSender::send(id, type, error);
}

bool nativeArm(void*, uint64_t token, int32_t x, int32_t y, int32_t z) {
    return ArmVisibleAnvilCapture(token, x, y, z);
}

void nativeCancel(void*, uint64_t token) { CancelContainerCapture(token); }

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

MapVisibleAnvilWindowOps MakeNativeMapVisibleAnvilWindowOps(
        MapVisibleAnvilNativePreflight* preflight) noexcept {
    return {preflight, &nativeVerifyContext, &nativePrepareOpen,
            &nativeOpen, &nativeClose, &nativeArm, &nativeCancel,
            &nativePoll, &nativePollQuarantine, &nativeMarkQuarantine};
}
#endif

}  // namespace build_import
