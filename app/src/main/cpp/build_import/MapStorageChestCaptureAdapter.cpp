#include "MapStorageChestCaptureAdapter.h"

#include <limits>
#include <utility>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace build_import {
namespace {

bool complete(const MapChestWindowOps& ops) noexcept {
    return ops.prepare_open && ops.send_open && ops.send_close &&
        ops.arm_capture && ops.cancel_capture && ops.poll_capture &&
        ops.poll_quarantine && ops.mark_quarantine_close_sent;
}

bool terminalFailure(MapChestWindowState state) noexcept {
    return state == MapChestWindowState::Failed ||
        state == MapChestWindowState::CloseFailed;
}

void logUiClose(const char* event, uint64_t token, uint8_t id,
                uint8_t type, const char* method) noexcept {
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "Infinitecz_MapChest",
        "[ui-close] %s token=%llu window=%u type=%u method=%s", event,
        static_cast<unsigned long long>(token),
        static_cast<unsigned>(id), static_cast<unsigned>(type), method);
#else
    (void)event;
    (void)token;
    (void)id;
    (void)type;
    (void)method;
#endif
}

}  // namespace

bool MapStorageChestCaptureAdapter::configure(
        const MapStorageChestCaptureConfig& config, std::string* error) {
    if (error) error->clear();
    if (configured_ || !complete(config.window_ops) || !config.next_token ||
        !config.try_arm_capture || !config.verify_context ||
        !config.request_ui_close || !config.ui_close_dispatched ||
        !config.ui_close_outbound_observed ||
        config.click_face < 0 || config.click_face > 5) {
        if (error) *error = "invalid or already configured chest capture adapter";
        return false;
    }
    config_ = config;
    configured_ = true;
    return true;
}

MapChestWindowOps MapStorageChestCaptureAdapter::wrappedOps() noexcept {
    return {this, &wrappedPrepare, &wrappedOpen, &wrappedClose, &wrappedArm,
            &wrappedCancel, &wrappedPoll, &wrappedPollQuarantine,
            &wrappedMarkQuarantine};
}

bool MapStorageChestCaptureAdapter::wrappedPrepare(
        void* context, const MapChestWindowRequest& request,
        const void** native_block, std::string* error) {
    auto* self = static_cast<MapStorageChestCaptureAdapter*>(context);
    if (!self->config_.verify_context(self->config_.guard_context,
            {request.x, request.y, request.z}, request.token, error)) {
        return false;
    }
    return self->config_.window_ops.prepare_open(
        self->config_.window_ops.context, request, native_block, error);
}

bool MapStorageChestCaptureAdapter::wrappedOpen(
        void* context, const MapChestWindowRequest& request,
        const void* native_block, std::string* error) {
    auto* self = static_cast<MapStorageChestCaptureAdapter*>(context);
    ContainerCaptureResult observed;
    // ArmContainerCapture is void and can silently refuse while a visible
    // anvil owns the shared mailbox. Never emit ClickBlock without proving
    // that this exact token/position actually owns the capture first.
    if (!self->arm_succeeded_ ||
        !self->config_.verify_context(self->config_.guard_context,
            {request.x, request.y, request.z}, request.token, error) ||
        self->config_.window_ops.poll_capture(
            self->config_.window_ops.context, request.token, &observed) !=
            ContainerCapturePollState::WaitingForOpen ||
        observed.token != request.token || observed.x != request.x ||
        observed.y != request.y || observed.z != request.z ||
        observed.container_opened || observed.container_closed) {
        if (error) *error = "chest capture mailbox did not accept the open token";
        return false;
    }
    self->open_packet_attempted_ = true;
    return self->config_.window_ops.send_open(
        self->config_.window_ops.context, request, native_block, error);
}

bool MapStorageChestCaptureAdapter::wrappedClose(
        void* context, uint8_t id, uint8_t type, std::string* error) {
    auto* self = static_cast<MapStorageChestCaptureAdapter*>(context);
    if (!self->config_.verify_context(self->config_.guard_context,
                                      self->position_, self->active_token_, error)) {
        return false;
    }
    return self->config_.window_ops.send_close(
        self->config_.window_ops.context, id, type, error);
}

void MapStorageChestCaptureAdapter::wrappedArm(
        void* context, uint64_t token, int32_t x, int32_t y, int32_t z) {
    auto* self = static_cast<MapStorageChestCaptureAdapter*>(context);
    self->arm_succeeded_ = self->config_.try_arm_capture(
        self->config_.guard_context, token, x, y, z);
}

void MapStorageChestCaptureAdapter::wrappedCancel(void* context,
                                                   uint64_t token) {
    auto* self = static_cast<MapStorageChestCaptureAdapter*>(context);
    self->config_.window_ops.cancel_capture(
        self->config_.window_ops.context, token);
}

ContainerCapturePollState MapStorageChestCaptureAdapter::wrappedPoll(
        void* context, uint64_t token, ContainerCaptureResult* result) {
    auto* self = static_cast<MapStorageChestCaptureAdapter*>(context);
    return self->config_.window_ops.poll_capture(
        self->config_.window_ops.context, token, result);
}

bool MapStorageChestCaptureAdapter::wrappedPollQuarantine(
        void* context, ContainerCaptureQuarantine* result) {
    auto* self = static_cast<MapStorageChestCaptureAdapter*>(context);
    const bool active = self->config_.window_ops.poll_quarantine(
        self->config_.window_ops.context, result);
    if (active && result && result->token != self->active_token_) {
        // A foreign capture owns this late packet tail. Wait for it to settle
        // but never send a close for its numeric window ID from this job.
        result->close_sent = true;
    }
    return active;
}

void MapStorageChestCaptureAdapter::wrappedMarkQuarantine(
        void* context, uint8_t id, uint8_t type) {
    auto* self = static_cast<MapStorageChestCaptureAdapter*>(context);
    self->config_.window_ops.mark_quarantine_close_sent(
        self->config_.window_ops.context, id, type);
}

bool MapStorageChestCaptureAdapter::matchesPosition(
        const MapChestPosition& position) const noexcept {
    return position_set_ && position == position_;
}

bool MapStorageChestCaptureAdapter::beginWindow(
        const MapChestPosition& position, bool reopen, uint64_t now_ms) {
    const uint64_t token = config_.next_token(config_.token_context);
    if (token == 0U || token == last_token_) {
        fail("chest capture token factory returned zero or reused a token");
        return false;
    }
    session_ = MapChestWindowSession{};
    const MapChestWindowRequest request{
        token, position.x, position.y, position.z, config_.click_face};
    std::string begin_error;
    if (!session_.begin(request, wrappedOps(), now_ms, &begin_error)) {
        fail(begin_error.empty() ? "chest window could not start" : begin_error);
        return false;
    }
    last_token_ = token;
    active_token_ = token;
    initial_content_observed_at_ms_ = 0U;
    ui_close_deadline_ms_ = 0U;
    ui_close_ready_at_ms_ = 0U;
    ui_close_dispatched_at_ms_ = 0U;
    ui_close_outbound_observed_at_ms_ = 0U;
    reopen_close_submitted_at_ms_ = 0U;
    ui_close_window_id_ = 0U;
    ui_close_window_type_ = 0U;
    ui_close_request_pending_ = false;
    ui_close_ticket_active_ = false;
    ui_close_dispatched_ = false;
    ui_close_outbound_observed_ = false;
    ui_close_fallback_attempted_ = false;
    close_after_place_requested_ = false;
    arm_succeeded_ = false;
    position_ = position;
    position_set_ = true;
    if (reopen) {
        reopen_token_ = token;
        state_ = MapStorageChestCaptureState::OpeningReopen;
    } else {
        initial_token_ = token;
        reopen_token_ = 0;
        state_ = MapStorageChestCaptureState::OpeningInitial;
    }
    return true;
}

bool MapStorageChestCaptureAdapter::requestInitialUiClose(
        uint64_t now_ms, std::string* error) {
    if (error) error->clear();
    if (!config_.verify_context(config_.guard_context, position_,
                                initial_token_, error)) {
        if (error && error->empty()) *error = "visible chest world context changed";
        return false;
    }
    const ContainerCaptureResult* captured = session_.capture();
    if (!captured || captured->token != initial_token_ ||
        captured->container_id == 0U || captured->container_id == 0xFFU ||
        captured->container_type != 0U) {
        if (error) *error = "visible chest identity changed before UI close";
        return false;
    }
    if (!config_.request_ui_close(config_.ui_context, initial_token_,
                                  captured->container_id,
                                  captured->container_type)) {
        // Queue rejection must never cause a fresh ClickBlock. Best-effort
        // close the already-open server window, then fail this job.
        std::string ignored;
        (void)session_.close(now_ms, &ignored);
        if (error) *error = "visible chest UI Back was not queued";
        return false;
    }
    ui_close_request_pending_ = true;
    ui_close_ticket_active_ = true;
    ui_close_dispatched_ = false;
    ui_close_window_id_ = captured->container_id;
    ui_close_window_type_ = captured->container_type;
    ui_close_deadline_ms_ =
        now_ms > std::numeric_limits<uint64_t>::max() - kUiCloseDispatchTimeoutMs
            ? std::numeric_limits<uint64_t>::max()
            : now_ms + kUiCloseDispatchTimeoutMs;
    std::string session_error;
    if (!session_.expectUiClose(now_ms, &session_error)) {
        retireInitialUiTicket();
        ui_close_request_pending_ = false;
        std::string ignored;
        (void)session_.close(now_ms, &ignored);
        if (error) *error = session_error.empty()
            ? "visible chest could not wait for UI close" : session_error;
        return false;
    }
    return true;
}

void MapStorageChestCaptureAdapter::pollInitialUiClose(uint64_t now_ms) {
    if (ui_close_request_pending_ &&
        config_.ui_close_dispatched(config_.ui_context, initial_token_)) {
        ui_close_request_pending_ = false;
        ui_close_dispatched_ = true;
        ui_close_dispatched_at_ms_ = now_ms;
        logUiClose("back-dispatched", initial_token_, ui_close_window_id_,
                   ui_close_window_type_, "stock-back");
    }
    if (ui_close_ticket_active_ && ui_close_dispatched_ &&
        !ui_close_outbound_observed_ &&
        config_.ui_close_outbound_observed(config_.ui_context, initial_token_)) {
        ui_close_outbound_observed_ = true;
        ui_close_outbound_observed_at_ms_ = now_ms;
        logUiClose("outbound-observed", initial_token_, ui_close_window_id_,
                   ui_close_window_type_,
                   ui_close_fallback_attempted_ ? "exact-fallback" : "stock-back");
    }
}

void MapStorageChestCaptureAdapter::retireInitialUiTicket() {
    if (ui_close_ticket_active_ && config_.cancel_ui_close) {
        config_.cancel_ui_close(config_.ui_context, initial_token_);
    }
    ui_close_ticket_active_ = false;
}

void MapStorageChestCaptureAdapter::fail(const std::string& reason) {
    retireInitialUiTicket();
    ui_close_request_pending_ = false;
    if (error_.empty()) error_ = reason;
    state_ = MapStorageChestCaptureState::Failed;
}

void MapStorageChestCaptureAdapter::drive(uint64_t now_ms) {
    pollInitialUiClose(now_ms);
    if (state_ == MapStorageChestCaptureState::Failed) {
        // A close may time out after the transfer has already reached the
        // server. Keep observing this exact hidden window so a late server
        // Close can release the process-wide mailbox; never reopen or resend.
        const auto window = session_.state();
        if (window == MapChestWindowState::WaitingForServerClose ||
            window == MapChestWindowState::CloseFailed ||
            window == MapChestWindowState::Quarantining) {
            (void)session_.tick(now_ms);
        }
        return;
    }
    if (state_ == MapStorageChestCaptureState::Idle ||
        state_ == MapStorageChestCaptureState::Stopped ||
        state_ == MapStorageChestCaptureState::ProofReady ||
        state_ == MapStorageChestCaptureState::ClosedAfterPlace) return;

    if (state_ == MapStorageChestCaptureState::ClosingInitial &&
        !ui_close_dispatched_ && ui_close_deadline_ms_ != 0U &&
        now_ms >= ui_close_deadline_ms_) {
        fail("visible chest UI Back was not dispatched");
        return;
    }

    const MapChestWindowState window = session_.tick(now_ms);
    if (state_ == MapStorageChestCaptureState::Stopping) {
        if (window == MapChestWindowState::CapturedOpen) {
            std::string close_error;
            const bool visible_initial = active_token_ == initial_token_ &&
                initial_token_ != 0U;
            const bool closed = visible_initial
                ? requestInitialUiClose(now_ms, &close_error)
                : session_.close(now_ms, &close_error);
            if (!closed) {
                fail(close_error.empty() ? "automatic chest close failed"
                                         : close_error);
            }
        } else if (window == MapChestWindowState::Completed ||
                   window == MapChestWindowState::Failed) {
            if (window == MapChestWindowState::Completed &&
                ui_close_deadline_ms_ != 0U && !ui_close_dispatched_) {
                if (now_ms >= ui_close_deadline_ms_) {
                    fail("visible chest UI Back was not dispatched");
                }
            } else {
                retireInitialUiTicket();
                state_ = MapStorageChestCaptureState::Stopped;
            }
        } else if (window == MapChestWindowState::CloseFailed) {
            fail(session_.error());
        }
        return;
    }

    if (terminalFailure(window)) {
        const std::string detail = session_.error().empty()
            ? "automatic chest window failed" : session_.error();
        fail(state_ == MapStorageChestCaptureState::OpeningReopen ||
             state_ == MapStorageChestCaptureState::ClosingReopen
                 ? "fresh chest reopen had no proof: " + detail : detail);
        return;
    }
    switch (state_) {
        case MapStorageChestCaptureState::OpeningInitial:
            if (window == MapChestWindowState::CapturedOpen) {
                initial_content_observed_at_ms_ = now_ms;
                state_ = MapStorageChestCaptureState::InitialReady;
            }
            break;
        case MapStorageChestCaptureState::InitialReady:
            // A lost capture or expired lease cannot authorize Place. The
            // session sends its own one-shot close before failing.
            break;
        case MapStorageChestCaptureState::ClosingInitial:
            if (window == MapChestWindowState::WaitingForServerClose &&
                ui_close_dispatched_) {
                if (ui_close_outbound_observed_ &&
                    now_ms - ui_close_outbound_observed_at_ms_ >=
                        kUiCloseOutboundSettleMs) {
                    std::string close_error;
                    if (!config_.verify_context(config_.guard_context, position_,
                                                initial_token_, &close_error)) {
                        fail(close_error.empty()
                            ? "visible chest world changed before outbound close isolation"
                            : close_error);
                        break;
                    }
#if defined(__ANDROID__)
                    if (!IsExactOpenVisibleChestCapture(
                            initial_token_, ui_close_window_id_,
                            ui_close_window_type_, position_.x,
                            position_.y, position_.z)) {
                        // The server may have closed between our first poll
                        // and this check. Consume that exact receipt before
                        // deciding whether another UI displaced the chest.
                        if (session_.tick(now_ms) ==
                                MapChestWindowState::WaitingForServerClose) {
                            fail("visible chest changed before outbound close isolation");
                        }
                        break;
                    }
#endif
                    if (!session_.acceptObservedOutboundUiClose(
                            now_ms, &close_error)) {
                        fail(close_error.empty()
                            ? "exact outbound chest close could not be isolated"
                            : close_error);
                        break;
                    }
                    logUiClose("outbound-isolated", initial_token_,
                               ui_close_window_id_, ui_close_window_type_,
                               ui_close_fallback_attempted_
                                   ? "exact-fallback" : "stock-back");
                } else if (!ui_close_outbound_observed_ &&
                           !ui_close_fallback_attempted_ &&
                           now_ms - ui_close_dispatched_at_ms_ >=
                               kUiCloseNativeGraceMs) {
                    std::string close_error;
                    if (!config_.verify_context(config_.guard_context, position_,
                                                initial_token_, &close_error)) {
                        fail(close_error.empty()
                            ? "visible chest world changed before exact-close fallback"
                            : close_error);
                        break;
                    }
#if defined(__ANDROID__)
                    if (!IsExactOpenVisibleChestCapture(
                            initial_token_, ui_close_window_id_,
                            ui_close_window_type_, position_.x,
                            position_.y, position_.z)) {
                        if (session_.tick(now_ms) ==
                                MapChestWindowState::WaitingForServerClose) {
                            fail("visible chest changed before exact-close fallback");
                        }
                        break;
                    }
#endif
                    ui_close_fallback_attempted_ = true;
                    if (!session_.sendExactCloseAfterUiBack(
                            now_ms, &close_error)) {
                        fail(close_error.empty()
                            ? "exact outbound ContainerClose was not submitted"
                            : "exact outbound ContainerClose was not submitted: " +
                                  close_error);
                        break;
                    }
                    logUiClose("fallback-submitted", initial_token_,
                               ui_close_window_id_, ui_close_window_type_,
                               "exact-fallback");
                } else if (!ui_close_outbound_observed_ &&
                           ui_close_fallback_attempted_ &&
                           now_ms - ui_close_dispatched_at_ms_ >=
                               kUiCloseOutboundTimeoutMs) {
                    fail("exact outbound ContainerClose was not observed after Back and one fallback");
                }
            }
            if (window == MapChestWindowState::Completed) {
                if (ui_close_dispatched_) {
                    if (ui_close_ready_at_ms_ == 0U) {
                        ui_close_ready_at_ms_ =
                            now_ms > std::numeric_limits<uint64_t>::max() -
                                     kUiCloseRetireSettleMs
                                ? std::numeric_limits<uint64_t>::max()
                                : now_ms + kUiCloseRetireSettleMs;
                    }
                    if (now_ms >= ui_close_ready_at_ms_) {
                        logUiClose(close_after_place_requested_
                                       ? "placed-close-ready"
                                       : "fresh-reopen-allowed", initial_token_,
                                   ui_close_window_id_, ui_close_window_type_,
                                   ui_close_outbound_observed_
                                       ? (ui_close_fallback_attempted_
                                              ? "exact-fallback" : "stock-back")
                                       : "server-inbound");
                        retireInitialUiTicket();
                        if (close_after_place_requested_) {
                            session_ = MapChestWindowSession{};
                            state_ = MapStorageChestCaptureState::ClosedAfterPlace;
                        } else {
                            (void)beginWindow(position_, true, now_ms);
                        }
                    }
                } else if (now_ms >= ui_close_deadline_ms_) {
                    fail("visible chest UI Back was not dispatched");
                }
            }
            break;
        case MapStorageChestCaptureState::OpeningReopen:
            if (window == MapChestWindowState::CapturedOpen) {
                const auto* captured = session_.capture();
                if (!captured || captured->token != reopen_token_ ||
                    captured->token == initial_token_) {
                    fail("fresh chest reopen did not have a distinct capture token");
                    break;
                }
                proof_ = *captured;
                std::string close_error;
                if (!session_.close(now_ms, &close_error)) {
                    fail(close_error.empty() ? "fresh chest reopen close failed"
                                             : close_error);
                    break;
                }
                reopen_close_submitted_at_ms_ = now_ms;
                logUiClose("fresh-proof-close-submitted", reopen_token_,
                           captured->container_id, captured->container_type,
                           "hidden-direct");
                state_ = MapStorageChestCaptureState::ClosingReopen;
            }
            break;
        case MapStorageChestCaptureState::ClosingReopen:
            if (window == MapChestWindowState::WaitingForServerClose &&
                now_ms - reopen_close_submitted_at_ms_ >=
                    MapChestWindowSession::kNoEchoCloseIsolationMs) {
                if (proof_.token != reopen_token_ ||
                    proof_.token == initial_token_ ||
                    proof_.container_id == 0U ||
                    proof_.container_id == 0xFFU ||
                    proof_.container_type != 0U ||
                    proof_.slot_count != 27U ||
                    proof_.container_closed || !proof_.error.empty()) {
                    fail("fresh chest reopen had no exact captured proof before close");
                    break;
                }
                std::string close_error;
                if (!config_.verify_context(config_.guard_context, position_,
                                            reopen_token_, &close_error) ||
                    !session_.acceptSubmittedCloseWithoutInbound(
                        now_ms, &close_error)) {
                    fail(close_error.empty()
                        ? "fresh chest proof close was not isolated"
                        : close_error);
                    break;
                }
                logUiClose("fresh-proof-close-isolated", reopen_token_,
                           proof_.container_id, proof_.container_type,
                           "hidden-direct");
            }
            if (window == MapChestWindowState::Completed) {
                logUiClose("fresh-proof-ready", reopen_token_,
                           proof_.container_id, proof_.container_type,
                           "hidden-direct");
                session_ = MapChestWindowSession{};
                state_ = MapStorageChestCaptureState::ProofReady;
            }
            break;
        default:
            break;
    }
}

MapStorageChestCaptureState MapStorageChestCaptureAdapter::tick(
        uint64_t now_ms) noexcept {
    try {
        if (!configured_) return state_;
        if (have_tick_ && now_ms < last_tick_ms_) {
            stop(last_tick_ms_);
            if (error_.empty()) error_ = "chest capture clock moved backwards";
            return state_;
        }
        have_tick_ = true;
        last_tick_ms_ = now_ms;
        drive(now_ms);
    } catch (...) {
        try {
            fail("chest capture adapter threw while processing a game tick");
        } catch (...) {
            state_ = MapStorageChestCaptureState::Failed;
        }
    }
    return state_;
}

void MapStorageChestCaptureAdapter::stop(uint64_t now_ms) noexcept {
    try {
        if (state_ == MapStorageChestCaptureState::Idle ||
            state_ == MapStorageChestCaptureState::ProofReady ||
            state_ == MapStorageChestCaptureState::ClosedAfterPlace) {
            proof_ = {};
            state_ = MapStorageChestCaptureState::Stopped;
            return;
        }
        if (state_ == MapStorageChestCaptureState::Stopped ||
            state_ == MapStorageChestCaptureState::Failed) return;
        if (session_.state() == MapChestWindowState::WaitingForQuarantine) {
            // begin() has not claimed the mailbox or emitted ClickBlock yet.
            // Driving the window after cancellation would open a chest that
            // the caller explicitly wanted to abandon.
            session_ = MapChestWindowSession{};
            proof_ = {};
            state_ = MapStorageChestCaptureState::Stopped;
            return;
        }
        state_ = MapStorageChestCaptureState::Stopping;
        (void)tick(now_ms);
    } catch (...) {
        state_ = MapStorageChestCaptureState::Failed;
    }
}

bool MapStorageChestCaptureAdapter::closeAfterPlace(
        const MapChestPosition& position, uint64_t now_ms,
        std::string* error) {
    if (error) error->clear();
    if (!configured_) {
        if (error) *error = "chest capture adapter is not configured";
        return false;
    }
    (void)tick(now_ms);
    if (state_ == MapStorageChestCaptureState::ClosedAfterPlace ||
        state_ == MapStorageChestCaptureState::Idle) {
        return true;
    }
    if (state_ == MapStorageChestCaptureState::Failed ||
        state_ == MapStorageChestCaptureState::Stopped) {
        if (error) *error = error_.empty() ? "chest capture stopped" : error_;
        return false;
    }
    if (!matchesPosition(position) ||
        !config_.verify_context(config_.guard_context, position,
                                active_token_, error)) {
        stop(now_ms);
        if (error && error->empty()) *error = "chest context changed before close";
        return false;
    }
    if (state_ == MapStorageChestCaptureState::InitialReady) {
        if (session_.state() != MapChestWindowState::CapturedOpen) {
            if (error) *error = "initial chest window expired before close";
            return false;
        }
        std::string close_error;
        if (!requestInitialUiClose(now_ms, &close_error)) {
            fail(close_error.empty() ? "initial chest close failed" : close_error);
            if (error) *error = error_;
            return false;
        }
        close_after_place_requested_ = true;
        state_ = MapStorageChestCaptureState::ClosingInitial;
    } else if (state_ != MapStorageChestCaptureState::ClosingInitial ||
               !close_after_place_requested_) {
        if (error) *error = "chest close requested outside the transfer window";
        return false;
    }
    (void)tick(now_ms);
    if (state_ == MapStorageChestCaptureState::ClosedAfterPlace) return true;
    if (error) *error = error_.empty() ? "waiting for visible chest close" : error_;
    return false;
}

bool MapStorageChestCaptureAdapter::capture(
        const MapChestPosition& position, bool reopen, uint64_t now_ms,
        ContainerCaptureResult* result, std::string* error) {
    if (result) *result = {};
    if (error) error->clear();
    if (!configured_ || !result) {
        if (error) *error = "chest capture adapter is not configured";
        return false;
    }
    (void)tick(now_ms);
    if (state_ == MapStorageChestCaptureState::Failed ||
        state_ == MapStorageChestCaptureState::Stopped) {
        if (error) *error = error_.empty() ? "chest capture is stopped" : error_;
        return false;
    }
    if (state_ != MapStorageChestCaptureState::Idle &&
        !matchesPosition(position)) {
        stop(now_ms);
        if (error) *error = "chest capture target changed during an open window";
        return false;
    }
    if (state_ != MapStorageChestCaptureState::Idle &&
        !config_.verify_context(config_.guard_context, position,
                                active_token_, error)) {
        stop(now_ms);
        if (error && error->empty()) *error = "chest world context changed";
        return false;
    }
    if (state_ == MapStorageChestCaptureState::Idle) {
        if (!beginWindow(position, reopen, now_ms)) {
            if (error) *error = error_;
            return false;
        }
        (void)tick(now_ms);
    }
    if (!reopen) {
        if (state_ == MapStorageChestCaptureState::InitialReady) {
            if (now_ms - initial_content_observed_at_ms_ <
                kInitialContentSettleMs) {
                if (error) *error = "waiting for initial chest content to settle";
                return false;
            }
            const ContainerCaptureResult* current = session_.capture();
            if (current && current->token == initial_token_) {
                *result = *current;
                return true;
            }
        }
        if (state_ != MapStorageChestCaptureState::OpeningInitial &&
            state_ != MapStorageChestCaptureState::InitialReady) {
            if (error) *error = "initial chest capture requested during reopen";
        } else if (error) {
            *error = error_.empty() ? "waiting for initial chest content" : error_;
        }
        return false;
    }
    if (state_ == MapStorageChestCaptureState::InitialReady) {
        if (now_ms - initial_content_observed_at_ms_ <
            kInitialContentSettleMs) {
            if (error) *error = "waiting for initial chest content to settle";
            return false;
        }
        if (session_.state() != MapChestWindowState::CapturedOpen) {
            if (error) *error = "initial chest window expired before reopen";
            return false;
        }
        std::string close_error;
        if (!requestInitialUiClose(now_ms, &close_error)) {
            fail(close_error.empty() ? "initial chest close failed" : close_error);
            if (error) *error = error_;
            return false;
        }
        state_ = MapStorageChestCaptureState::ClosingInitial;
    } else if (state_ == MapStorageChestCaptureState::OpeningInitial) {
        stop(now_ms);
        if (error) *error = "reopen requested before initial chest capture was ready";
        return false;
    }
    if (state_ == MapStorageChestCaptureState::ProofReady) {
        if (proof_.token == 0U || proof_.token != reopen_token_ ||
            proof_.token == initial_token_ || !matchesPosition(position)) {
            fail("fresh chest reopen proof identity changed");
            if (error) *error = error_;
            return false;
        }
        *result = std::move(proof_);
        proof_ = {};
        position_set_ = false;
        state_ = MapStorageChestCaptureState::Idle;
        return true;
    }
    if (error) *error = error_.empty() ? "waiting for fresh chest reopen" : error_;
    return false;
}

}  // namespace build_import
