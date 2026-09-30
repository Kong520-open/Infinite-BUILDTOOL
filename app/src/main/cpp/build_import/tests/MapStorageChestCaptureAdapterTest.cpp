#include "../MapStorageChestCaptureAdapter.h"

#include <cassert>
#include <cstdint>
#include <string>

namespace {

using namespace build_import;

struct Fake {
    uint64_t next = 500U;
    uint64_t active_token = 0;
    bool context_ok = true;
    bool claim_ok = true;
    bool close_ok = true;
    bool ui_queue_ok = true;
    bool ui_dispatched = false;
    bool ui_outbound = false;
    bool observe_fallback_outbound = true;
    bool quarantine = false;
    int claims = 0;
    int opens = 0;
    int closes = 0;
    int cancels = 0;
    int ui_requests = 0;
    int ui_cancels = 0;
    uint64_t ui_token = 0;
    ContainerCapturePollState poll = ContainerCapturePollState::WaitingForOpen;
    ContainerCaptureResult observed;
    ContainerCaptureQuarantine quarantine_value;

    static Fake& get(void* context) { return *static_cast<Fake*>(context); }
    static uint64_t token(void* context) { return ++get(context).next; }
    static bool verify(void* context, const MapChestPosition&,
                       uint64_t token, std::string* error) {
        Fake& fake = get(context);
        if (!fake.context_ok || token == 0U) {
            if (error) *error = "live world or dimension changed";
            return false;
        }
        return true;
    }
    static bool claim(void* context, uint64_t token, int32_t x, int32_t y,
                      int32_t z) {
        Fake& fake = get(context);
        ++fake.claims;
        if (!fake.claim_ok || fake.active_token != 0U || fake.quarantine)
            return false;
        fake.active_token = token;
        fake.poll = ContainerCapturePollState::WaitingForOpen;
        fake.observed = {};
        fake.observed.token = token;
        fake.observed.x = x;
        fake.observed.y = y;
        fake.observed.z = z;
        return true;
    }
    static bool prepare(void*, const MapChestWindowRequest&,
                        const void** native_block, std::string*) {
        if (native_block) *native_block = reinterpret_cast<const void*>(0x1234);
        return true;
    }
    static bool open(void* context, const MapChestWindowRequest&,
                     const void* native_block, std::string*) {
        Fake& fake = get(context);
        assert(native_block == reinterpret_cast<const void*>(0x1234));
        ++fake.opens;
        return true;
    }
    static bool close(void* context, uint8_t, uint8_t,
                      std::string* error) {
        Fake& fake = get(context);
        ++fake.closes;
        if (fake.observe_fallback_outbound && fake.ui_dispatched &&
            fake.ui_token == fake.active_token) fake.ui_outbound = true;
        if (!fake.close_ok && error) *error = "native close failed";
        return fake.close_ok;
    }
    static bool requestUiClose(void* context, uint64_t token, uint8_t id,
                               uint8_t type) {
        Fake& fake = get(context);
        ++fake.ui_requests;
        assert(token == fake.active_token);
        assert(id == fake.observed.container_id && type == 0U);
        if (!fake.ui_queue_ok) return false;
        fake.ui_token = token;
        return true;
    }
    static bool uiCloseDispatched(void* context, uint64_t token) {
        Fake& fake = get(context);
        return fake.ui_token == token && fake.ui_dispatched;
    }
    static bool uiCloseOutboundObserved(void* context, uint64_t token) {
        Fake& fake = get(context);
        return fake.ui_token == token && fake.ui_outbound;
    }
    static void cancelUiClose(void* context, uint64_t token) {
        Fake& fake = get(context);
        if (fake.ui_token == token) ++fake.ui_cancels;
        fake.ui_token = 0U;
    }
    static void legacyArm(void*, uint64_t, int32_t, int32_t, int32_t) {
        assert(false && "adapter must use atomic try-arm, not legacy arm");
    }
    static void cancel(void* context, uint64_t token) {
        Fake& fake = get(context);
        ++fake.cancels;
        if (fake.active_token != token) return;
        fake.active_token = 0U;
        fake.quarantine = true;
        fake.quarantine_value.container_id = fake.observed.container_id;
        fake.quarantine_value.container_type = fake.observed.container_type;
        fake.quarantine_value.container_opened = fake.observed.container_opened;
        fake.quarantine_value.container_closed = fake.observed.container_closed;
        fake.quarantine_value.content_captured =
            fake.poll == ContainerCapturePollState::Ready;
    }
    static ContainerCapturePollState pollCapture(
            void* context, uint64_t token, ContainerCaptureResult* result) {
        Fake& fake = get(context);
        if (fake.active_token != token) {
            if (result) *result = {};
            return ContainerCapturePollState::Inactive;
        }
        if (result) *result = fake.observed;
        return fake.poll;
    }
    static bool pollQuarantine(void* context,
                               ContainerCaptureQuarantine* result) {
        Fake& fake = get(context);
        if (result) *result = fake.quarantine_value;
        return fake.quarantine;
    }
    static void markQuarantine(void* context, uint8_t, uint8_t) {
        get(context).quarantine_value.close_sent = true;
    }
    MapStorageChestCaptureConfig config() {
        MapStorageChestCaptureConfig value;
        value.window_ops = {this, &prepare, &open, &close, &legacyArm,
                            &cancel, &pollCapture, &pollQuarantine,
                            &markQuarantine};
        value.guard_context = this;
        value.try_arm_capture = &claim;
        value.verify_context = &verify;
        value.token_context = this;
        value.next_token = &token;
        value.ui_context = this;
        value.request_ui_close = &requestUiClose;
        value.ui_close_dispatched = &uiCloseDispatched;
        value.ui_close_outbound_observed = &uiCloseOutboundObserved;
        value.cancel_ui_close = &cancelUiClose;
        return value;
    }
    void ready(uint8_t id) {
        assert(active_token != 0U);
        poll = ContainerCapturePollState::Ready;
        observed.container_opened = true;
        observed.container_id = id;
        observed.container_type = 0U;
        observed.slot_count = 27U;
        observed.has_full_container_name = true;
        observed.full_container_name = 0U;
    }
};

constexpr MapChestPosition kChest{10, 64, -5};
static_assert(MapStorageChestCaptureAdapter::kInitialContentSettleMs == 300U,
              "initial chest content must settle briefly before transfer");
constexpr uint64_t kSettledAt =
    1U + MapStorageChestCaptureAdapter::kInitialContentSettleMs;

void initialCaptureWaitsForServerSettle() {
    Fake fake;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    std::string error;
    assert(!adapter.capture(kChest, false, 0U, &capture));
    fake.ready(20U);
    assert(!adapter.capture(kChest, false, 1U, &capture, &error));
    assert(error == "waiting for initial chest content to settle");
    assert(!adapter.capture(kChest, true, kSettledAt - 1U,
                            &capture, &error));
    assert(error == "waiting for initial chest content to settle");
    assert(fake.closes == 0 && fake.opens == 1);
    assert(adapter.capture(kChest, false, kSettledAt, &capture));
    assert(capture.container_id == 20U && fake.closes == 0);
}

void initialSettleNeverExtendsTheOpenWindowLease() {
    Fake fake;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, false, 0U, &capture));
    fake.ready(23U);
    assert(adapter.tick(1U) == MapStorageChestCaptureState::InitialReady);
    assert(!adapter.capture(kChest, false,
                            1U + MapChestWindowSession::kCaptureLeaseMs,
                            &capture));
    assert(fake.closes == 1 && fake.opens == 1);
}

void twoStagesReuseThenFreshReopen() {
    Fake fake;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, false, 0U, &capture));
    assert(fake.claims == 1 && fake.opens == 1);
    fake.ready(21U);
    assert(adapter.tick(1U) == MapStorageChestCaptureState::InitialReady);
    assert(adapter.capture(kChest, false, kSettledAt, &capture));
    const uint64_t prepared_token = capture.token;
    assert(prepared_token == adapter.initial_token());
    assert(adapter.capture(kChest, false, kSettledAt + 1U, &capture));
    assert(capture.token == prepared_token && fake.opens == 1 &&
           fake.closes == 0);

    // No Place action is sent by the adapter. A coordinator may journal and
    // submit exactly one Place between these calls; reopen only reads proof.
    assert(!adapter.capture(kChest, true, kSettledAt + 2U, &capture));
    assert(fake.ui_requests == 1 && fake.closes == 0 && fake.opens == 1);
    assert(adapter.tick(kSettledAt + 3U) == MapStorageChestCaptureState::ClosingInitial);
    assert(fake.cancels == 0 && fake.claims == 1);
    fake.observed.container_closed = true;
    assert(adapter.tick(kSettledAt + 4U) == MapStorageChestCaptureState::ClosingInitial);
    assert(fake.cancels == 1 && fake.opens == 1);
    fake.quarantine = false;
    assert(adapter.tick(kSettledAt + 5U) == MapStorageChestCaptureState::ClosingInitial);
    assert(fake.opens == 1 && "server Close alone cannot open the proof window");
    fake.ui_dispatched = true;
    const uint64_t ui_confirmed_at = kSettledAt + 6U;
    assert(adapter.tick(ui_confirmed_at) == MapStorageChestCaptureState::ClosingInitial);
    assert(adapter.tick(ui_confirmed_at +
                        MapStorageChestCaptureAdapter::kUiCloseRetireSettleMs - 1U) ==
           MapStorageChestCaptureState::ClosingInitial);
    const uint64_t reopen_at = ui_confirmed_at +
        MapStorageChestCaptureAdapter::kUiCloseRetireSettleMs;
    assert(adapter.tick(reopen_at) == MapStorageChestCaptureState::OpeningReopen);
    assert(fake.ui_cancels == 1 && "visible UI ticket must be retired");
    assert(adapter.tick(reopen_at + 1U) == MapStorageChestCaptureState::OpeningReopen);
    assert(fake.claims == 2 && fake.opens == 2);
    fake.ready(22U);
    assert(adapter.tick(reopen_at + 2U) == MapStorageChestCaptureState::ClosingReopen);
    assert(fake.closes == 1);
    assert(fake.cancels == 1);
    fake.observed.container_closed = true;
    assert(adapter.tick(reopen_at + 3U) == MapStorageChestCaptureState::ClosingReopen);
    fake.quarantine = false;
    assert(adapter.tick(reopen_at + 4U) == MapStorageChestCaptureState::ProofReady);
    assert(adapter.capture(kChest, true, reopen_at + 5U, &capture));
    assert(capture.token != prepared_token &&
           capture.token == adapter.reopen_token() &&
           capture.container_id == 22U && fake.opens == 2);
    assert(!adapter.capture(kChest, true, reopen_at + 6U, &capture));
    assert(fake.opens == 3 && "proof cannot be reused for a second verification");
}

void acceptedPlaceClosesVisibleWindowWithoutReopening() {
    Fake fake;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    assert(adapter.closeAfterPlace(kChest, 0U)); // recovered job: no live window
    assert(fake.opens == 0 && fake.closes == 0);
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, false, 1U, &capture));
    fake.ready(24U);
    assert(adapter.tick(2U) == MapStorageChestCaptureState::InitialReady);
    const uint64_t ready_at =
        2U + MapStorageChestCaptureAdapter::kInitialContentSettleMs;
    assert(adapter.capture(kChest, false, ready_at, &capture));
    assert(!adapter.closeAfterPlace(kChest, ready_at + 1U));
    assert(adapter.state() == MapStorageChestCaptureState::ClosingInitial);
    assert(fake.ui_requests == 1 && fake.opens == 1 && fake.closes == 0);
    fake.ui_dispatched = true;
    fake.ui_outbound = true; // stock Back sent the exact ContainerClose
    const uint64_t observed_at = ready_at + 2U;
    assert(adapter.tick(observed_at) == MapStorageChestCaptureState::ClosingInitial);
    assert(adapter.tick(observed_at +
                        MapStorageChestCaptureAdapter::kUiCloseOutboundSettleMs) ==
           MapStorageChestCaptureState::ClosingInitial);
    fake.quarantine = false;
    assert(adapter.tick(observed_at +
                        MapStorageChestCaptureAdapter::kUiCloseOutboundSettleMs + 1U) ==
           MapStorageChestCaptureState::ClosingInitial);
    const uint64_t closed_at = observed_at +
        MapStorageChestCaptureAdapter::kUiCloseOutboundSettleMs + 1U +
        MapStorageChestCaptureAdapter::kUiCloseRetireSettleMs;
    assert(adapter.tick(closed_at) ==
           MapStorageChestCaptureState::ClosedAfterPlace);
    assert(adapter.closeAfterPlace(kChest, closed_at + 1U));
    assert(fake.opens == 1 && fake.claims == 1 && fake.closes == 0 &&
           fake.ui_requests == 1 && fake.ui_cancels == 1);

    // The first transfer window is already closed. Retiring its adapter must
    // not send another Close, and a later map tile can claim a fresh mailbox.
    adapter.stop(closed_at + 2U);
    assert(adapter.state() == MapStorageChestCaptureState::Stopped);
    assert(fake.opens == 1 && fake.closes == 0);
    MapStorageChestCaptureAdapter next_tile;
    assert(next_tile.configure(fake.config()));
    assert(!next_tile.capture(kChest, false, closed_at + 3U, &capture));
    assert(fake.claims == 2 && fake.opens == 2);
}

void directReopenAfterRestart() {
    Fake fake;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, true, 0U, &capture));
    assert(adapter.initial_token() == 0U && fake.opens == 1);
    fake.ready(31U);
    assert(!adapter.capture(kChest, true, 1U, &capture));
    assert(fake.closes == 1);
    fake.observed.container_closed = true;
    assert(adapter.tick(2U) == MapStorageChestCaptureState::ClosingReopen);
    fake.quarantine = false;
    assert(adapter.capture(kChest, true, 3U, &capture));
    assert(capture.token == adapter.reopen_token() && capture.container_id == 31U);
}

void hiddenProofCloseWithoutInboundStillReturnsOnlyFreshCapture() {
    Fake fake;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, true, 0U, &capture));
    const uint64_t token = adapter.reopen_token();
    assert(token != 0U && adapter.initial_token() == 0U && fake.opens == 1);
    fake.ready(32U);
    CapturedContainerItem named_map;
    named_map.slot = 0U;
    named_map.numeric_id = 593;
    named_map.count = 1U;
    named_map.has_map_uuid = true;
    named_map.map_uuid = -101;
    named_map.name_status = MapItemNameStatus::Present;
    named_map.name_source = MapItemNameSource::DisplayName;
    named_map.name_candidate = "地图 1行1列";
    fake.observed.items.push_back(named_map);
    assert(!adapter.capture(kChest, true, 1U, &capture));
    assert(adapter.state() == MapStorageChestCaptureState::ClosingReopen);
    assert(fake.closes == 1 && fake.cancels == 0);
    assert(adapter.tick(1U +
                        MapChestWindowSession::kNoEchoCloseIsolationMs - 1U) ==
           MapStorageChestCaptureState::ClosingReopen);
    assert(fake.cancels == 0 && fake.closes == 1);
    assert(adapter.tick(1U +
                        MapChestWindowSession::kNoEchoCloseIsolationMs) ==
           MapStorageChestCaptureState::ClosingReopen);
    assert(fake.cancels == 1 && fake.closes == 1);
    fake.quarantine = false;
    assert(adapter.tick(2U +
                        MapChestWindowSession::kNoEchoCloseIsolationMs) ==
           MapStorageChestCaptureState::ProofReady);
    assert(adapter.capture(kChest, true, 3U +
                           MapChestWindowSession::kNoEchoCloseIsolationMs,
                           &capture));
    assert(capture.token == token && capture.container_id == 32U &&
           capture.items.size() == 1U &&
           capture.items[0].map_uuid == -101 &&
           capture.items[0].name_candidate == "地图 1行1列");
    assert(fake.opens == 1 && fake.closes == 1);
}

void claimFailureNeverEmitsOpen() {
    Fake fake;
    fake.claim_ok = false;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, false, 0U, &capture));
    assert(fake.claims == 1 && fake.opens == 0 && fake.closes == 0);
    assert(adapter.tick(1U) == MapStorageChestCaptureState::Failed);
    assert(adapter.safeToDiscardAfterFailure());
}

void foreignQuarantineCannotBeClosedByThisJob() {
    Fake fake;
    fake.quarantine = true;
    fake.quarantine_value.token = 9001U;
    fake.quarantine_value.container_id = 77U;
    fake.quarantine_value.container_type = 0U;
    fake.quarantine_value.container_opened = true;
    fake.quarantine_value.content_captured = false;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, false, 0U, &capture));
    assert(fake.closes == 0 && fake.opens == 0 && fake.claims == 0);
    fake.quarantine = false;
    assert(adapter.tick(1U) == MapStorageChestCaptureState::OpeningInitial);
    assert(fake.opens == 1 && fake.closes == 0);
}

void stopBeforeOpenDoesNotCreateAWindow() {
    Fake fake;
    fake.quarantine = true;
    fake.quarantine_value.token = 9002U;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, false, 0U, &capture));
    assert(adapter.state() == MapStorageChestCaptureState::OpeningInitial);
    assert(fake.claims == 0 && fake.opens == 0);
    adapter.stop(1U);
    fake.quarantine = false;
    assert(adapter.tick(2U) == MapStorageChestCaptureState::Stopped);
    assert(fake.claims == 0 && fake.opens == 0 && fake.closes == 0);
}

void worldChangePreventsStaleWindowClose() {
    Fake fake;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, false, 0U, &capture));
    fake.ready(41U);
    assert(adapter.tick(1U) == MapStorageChestCaptureState::InitialReady);
    assert(adapter.capture(kChest, false, kSettledAt, &capture));
    fake.context_ok = false;
    assert(!adapter.capture(kChest, true, kSettledAt + 1U, &capture));
    assert(fake.closes == 0 && fake.opens == 1);
    assert(adapter.state() == MapStorageChestCaptureState::Failed);
    assert(!adapter.safeToDiscardAfterFailure());
}

void invalidWorldNeverOpensWindow() {
    Fake fake;
    fake.context_ok = false;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, false, 0U, &capture));
    assert(fake.claims == 0 && fake.opens == 0 && fake.closes == 0);
    assert(adapter.safeToDiscardAfterFailure());
}

void closeFailureNeverAutomaticallyRetries() {
    Fake fake;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, false, 0U, &capture));
    fake.ready(51U);
    assert(adapter.tick(1U) == MapStorageChestCaptureState::InitialReady);
    assert(adapter.capture(kChest, false, kSettledAt, &capture));
    fake.ui_queue_ok = false;
    assert(!adapter.capture(kChest, true, kSettledAt + 1U, &capture));
    assert(fake.ui_requests == 1 && fake.closes == 1 && fake.opens == 1);
    for (uint64_t time = kSettledAt + 2U;
         time < kSettledAt + 9U; ++time) adapter.tick(time);
    assert(fake.ui_requests == 1 && fake.closes == 1 && fake.opens == 1);
}

void targetChangeStopsWithoutOpeningAnotherChest() {
    Fake fake;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, false, 0U, &capture));
    fake.ready(61U);
    assert(adapter.tick(1U) == MapStorageChestCaptureState::InitialReady);
    assert(adapter.capture(kChest, false, kSettledAt, &capture));
    assert(!adapter.capture({11, 64, -5}, false, kSettledAt + 1U, &capture));
    assert(fake.ui_requests == 1 && fake.closes == 0 && fake.opens == 1);
    fake.ui_dispatched = true;
    fake.observed.container_closed = true;
    assert(adapter.tick(kSettledAt + 2U) == MapStorageChestCaptureState::Stopping);
    fake.quarantine = false;
    assert(adapter.tick(kSettledAt + 3U) == MapStorageChestCaptureState::Stopped);
}

void missingCloseReceiptUsesOneExactFallbackThenReopensForProof() {
    Fake fake;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, false, 0U, &capture));
    fake.ready(71U);
    assert(adapter.tick(1U) == MapStorageChestCaptureState::InitialReady);
    assert(adapter.capture(kChest, false, kSettledAt, &capture));
    assert(!adapter.capture(kChest, true, kSettledAt + 1U, &capture));
    assert(adapter.state() == MapStorageChestCaptureState::ClosingInitial);
    assert(fake.ui_requests == 1 && fake.closes == 0 && fake.opens == 1 &&
           fake.claims == 1);
    fake.ui_dispatched = true;

    // A different container's close must not authorize a fallback or proof.
    fake.observed.container_id = 72U;
    fake.observed.container_closed = true;
    assert(adapter.tick(kSettledAt + 2U) == MapStorageChestCaptureState::ClosingInitial);
    assert(fake.opens == 1 && fake.cancels == 0);

    fake.observed.container_id = 71U;
    fake.observed.container_closed = false;
    const uint64_t fallback_at = kSettledAt + 2U +
        MapStorageChestCaptureAdapter::kUiCloseNativeGraceMs;
    assert(adapter.tick(fallback_at) == MapStorageChestCaptureState::ClosingInitial);
    assert(fake.closes == 1 && fake.opens == 1 && fake.ui_outbound);
    assert(adapter.tick(fallback_at + 1U) == MapStorageChestCaptureState::ClosingInitial);
    const uint64_t isolated_at = fallback_at + 1U +
        MapStorageChestCaptureAdapter::kUiCloseOutboundSettleMs;
    assert(adapter.tick(isolated_at) == MapStorageChestCaptureState::ClosingInitial);
    assert(fake.cancels == 1 && fake.closes == 1 && fake.opens == 1);
    fake.quarantine = false;
    assert(adapter.tick(isolated_at + 1U) == MapStorageChestCaptureState::ClosingInitial);
    const uint64_t proof_at = isolated_at + 1U +
        MapStorageChestCaptureAdapter::kUiCloseRetireSettleMs;
    assert(adapter.tick(proof_at) == MapStorageChestCaptureState::OpeningReopen);
    assert(fake.closes == 1 && fake.opens == 1);
    assert(adapter.tick(proof_at + 1U) == MapStorageChestCaptureState::OpeningReopen);
    assert(fake.opens == 2 && fake.claims == 2);
}

void stockBackOutboundNeedsNoFallback() {
    Fake fake;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, false, 0U, &capture));
    fake.ready(91U);
    assert(adapter.tick(1U) == MapStorageChestCaptureState::InitialReady);
    assert(adapter.capture(kChest, false, kSettledAt, &capture));
    assert(!adapter.capture(kChest, true, kSettledAt + 1U, &capture));
    fake.ui_dispatched = true;
    fake.ui_outbound = true;  // observed from stock Back sendToServer
    const uint64_t observed_at = kSettledAt + 2U;
    assert(adapter.tick(observed_at) == MapStorageChestCaptureState::ClosingInitial);
    assert(fake.closes == 0 && fake.opens == 1);
    assert(adapter.tick(observed_at +
                        MapStorageChestCaptureAdapter::kUiCloseOutboundSettleMs) ==
           MapStorageChestCaptureState::ClosingInitial);
    assert(fake.cancels == 1 && fake.closes == 0);
    fake.quarantine = false;
    assert(adapter.tick(observed_at +
                        MapStorageChestCaptureAdapter::kUiCloseOutboundSettleMs + 1U) ==
           MapStorageChestCaptureState::ClosingInitial);
    const uint64_t proof_at = observed_at +
        MapStorageChestCaptureAdapter::kUiCloseOutboundSettleMs + 1U +
        MapStorageChestCaptureAdapter::kUiCloseRetireSettleMs;
    assert(adapter.tick(proof_at) == MapStorageChestCaptureState::OpeningReopen);
    assert(fake.closes == 0 && fake.opens == 1);
}

void unobservedFallbackNeverOpensProof() {
    Fake fake;
    fake.observe_fallback_outbound = false;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, false, 0U, &capture));
    fake.ready(92U);
    assert(adapter.tick(1U) == MapStorageChestCaptureState::InitialReady);
    assert(adapter.capture(kChest, false, kSettledAt, &capture));
    assert(!adapter.capture(kChest, true, kSettledAt + 1U, &capture));
    fake.ui_dispatched = true;
    const uint64_t dispatched_at = kSettledAt + 2U;
    assert(adapter.tick(dispatched_at) == MapStorageChestCaptureState::ClosingInitial);
    assert(adapter.tick(dispatched_at +
                        MapStorageChestCaptureAdapter::kUiCloseNativeGraceMs) ==
           MapStorageChestCaptureState::ClosingInitial);
    assert(fake.closes == 1 && fake.opens == 1 && !fake.ui_outbound);
    assert(adapter.tick(dispatched_at +
                        MapStorageChestCaptureAdapter::kUiCloseOutboundTimeoutMs) ==
           MapStorageChestCaptureState::Failed);
    assert(fake.closes == 1 && fake.opens == 1 &&
           adapter.error().find("outbound ContainerClose") != std::string::npos);
}

void serverCloseWithoutUiBackFailsClosed() {
    Fake fake;
    MapStorageChestCaptureAdapter adapter;
    assert(adapter.configure(fake.config()));
    ContainerCaptureResult capture;
    assert(!adapter.capture(kChest, false, 0U, &capture));
    fake.ready(81U);
    assert(adapter.tick(1U) == MapStorageChestCaptureState::InitialReady);
    assert(adapter.capture(kChest, false, kSettledAt, &capture));
    const uint64_t close_started = kSettledAt + 1U;
    assert(!adapter.capture(kChest, true, close_started, &capture));
    fake.observed.container_closed = true;
    assert(adapter.tick(close_started + 1U) ==
           MapStorageChestCaptureState::ClosingInitial);
    fake.quarantine = false;
    assert(adapter.tick(close_started + 2U) ==
           MapStorageChestCaptureState::ClosingInitial);
    assert(fake.opens == 1 && fake.closes == 0);
    assert(adapter.tick(close_started +
                        MapStorageChestCaptureAdapter::kUiCloseDispatchTimeoutMs) ==
           MapStorageChestCaptureState::Failed);
    assert(fake.ui_cancels == 1 && fake.opens == 1);
}

}  // namespace

int main() {
    initialCaptureWaitsForServerSettle();
    initialSettleNeverExtendsTheOpenWindowLease();
    twoStagesReuseThenFreshReopen();
    acceptedPlaceClosesVisibleWindowWithoutReopening();
    directReopenAfterRestart();
    hiddenProofCloseWithoutInboundStillReturnsOnlyFreshCapture();
    claimFailureNeverEmitsOpen();
    foreignQuarantineCannotBeClosedByThisJob();
    stopBeforeOpenDoesNotCreateAWindow();
    worldChangePreventsStaleWindowClose();
    invalidWorldNeverOpensWindow();
    closeFailureNeverAutomaticallyRetries();
    targetChangeStopsWithoutOpeningAnotherChest();
    missingCloseReceiptUsesOneExactFallbackThenReopensForProof();
    stockBackOutboundNeedsNoFallback();
    unobservedFallbackNeverOpensProof();
    serverCloseWithoutUiBackFailsClosed();
}
