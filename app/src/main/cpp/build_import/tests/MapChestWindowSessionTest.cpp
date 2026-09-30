#include "MapChestWindowSession.h"

#include <cassert>
#include <cstdint>
#include <string>

namespace {

using namespace build_import;

struct Fake {
    bool allowed = true;
    bool open_ok = true;
    bool close_ok = true;
    bool armed = false;
    bool quarantine_active = false;
    int opens = 0;
    int closes = 0;
    int arms = 0;
    int cancels = 0;
    int marks = 0;
    uint8_t last_close_id = 0;
    uint8_t last_close_type = 0;
    ContainerCapturePollState poll = ContainerCapturePollState::WaitingForOpen;
    ContainerCaptureResult result;
    ContainerCaptureQuarantine quarantine;

    static Fake& get(void* context) { return *static_cast<Fake*>(context); }
    static bool prepareOpen(void* context, const MapChestWindowRequest&,
                            const void** native_block, std::string* error) {
        Fake& fake = get(context);
        if (!fake.allowed && error) *error = "exclusive capture unavailable";
        if (native_block) *native_block = fake.allowed
            ? reinterpret_cast<const void*>(0x1234) : nullptr;
        return fake.allowed;
    }
    static bool sendOpen(void* context, const MapChestWindowRequest&,
                         const void* native_block,
                         std::string*) {
        Fake& fake = get(context);
        assert(native_block == reinterpret_cast<const void*>(0x1234));
        ++fake.opens;
        return fake.open_ok;
    }
    static bool sendClose(void* context, uint8_t id, uint8_t type,
                          std::string* error) {
        Fake& fake = get(context);
        ++fake.closes;
        fake.last_close_id = id;
        fake.last_close_type = type;
        if (!fake.close_ok && error) *error = "close ABI unavailable";
        return fake.close_ok;
    }
    static void arm(void* context, uint64_t token, int32_t x, int32_t y,
                    int32_t z) {
        Fake& fake = get(context);
        ++fake.arms;
        fake.armed = true;
        fake.result.token = token;
        fake.result.x = x;
        fake.result.y = y;
        fake.result.z = z;
    }
    static void cancel(void* context, uint64_t) {
        Fake& fake = get(context);
        ++fake.cancels;
        fake.armed = false;
        fake.quarantine_active = true;
        fake.quarantine.container_id = fake.result.container_id;
        fake.quarantine.container_type = fake.result.container_type;
        fake.quarantine.container_opened = fake.result.container_opened;
        fake.quarantine.container_closed = fake.result.container_closed;
        fake.quarantine.content_captured =
            fake.poll == ContainerCapturePollState::Ready;
        fake.quarantine.close_sent = fake.closes != 0;
    }
    static ContainerCapturePollState pollCapture(
            void* context, uint64_t, ContainerCaptureResult* result) {
        Fake& fake = get(context);
        if (!fake.armed) {
            if (result) *result = {};
            return ContainerCapturePollState::Inactive;
        }
        if (result) *result = fake.result;
        return fake.poll;
    }
    static bool pollQuarantine(void* context,
                               ContainerCaptureQuarantine* result) {
        Fake& fake = get(context);
        if (result) *result = fake.quarantine;
        return fake.quarantine_active;
    }
    static void mark(void* context, uint8_t, uint8_t) {
        Fake& fake = get(context);
        ++fake.marks;
        fake.quarantine.close_sent = true;
    }
    MapChestWindowOps ops() {
        return {this, &prepareOpen, &sendOpen, &sendClose, &arm, &cancel,
                &pollCapture, &pollQuarantine, &mark};
    }
    void opened(uint8_t type = 0U) {
        poll = ContainerCapturePollState::WaitingForContent;
        result.container_opened = true;
        result.container_id = 3;
        result.container_type = type;
    }
    void ready() {
        poll = ContainerCapturePollState::Ready;
        result.container_opened = true;
        result.container_id = 3;
        result.container_type = 0;
        result.slot_count = 27;
        result.has_full_container_name = true;
        result.full_container_name = 0;
    }
};

MapChestWindowRequest request(uint64_t token = 42) {
    MapChestWindowRequest value;
    value.token = token;
    value.x = 12;
    value.y = 30;
    value.z = -9;
    return value;
}

void happyPathAndOneShot() {
    Fake fake;
    MapChestWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
    assert(fake.arms == 1 && fake.opens == 1);
    fake.opened();
    assert(session.tick(100) == MapChestWindowState::WaitingForContent);
    fake.ready();
    assert(session.tick(101) == MapChestWindowState::CapturedOpen);
    assert(session.capture() && session.capture()->slot_count == 27);
    assert(session.tick(102) == MapChestWindowState::CapturedOpen);
    assert(fake.opens == 1 && fake.closes == 0);
    assert(session.close(103));
    assert(session.state() == MapChestWindowState::WaitingForServerClose);
    assert(fake.closes == 1 && fake.cancels == 0);
    assert(session.capture() == nullptr);
    assert(session.tick(104) == MapChestWindowState::WaitingForServerClose);
    assert(fake.closes == 1 && fake.cancels == 0);
    fake.result.container_closed = true;
    assert(session.tick(105) == MapChestWindowState::Quarantining);
    assert(fake.cancels == 1);
    fake.quarantine_active = false;
    assert(session.tick(106) == MapChestWindowState::Completed);
    assert(!session.begin(request(43), fake.ops(), 107));
    assert(fake.opens == 1 && fake.closes == 1);
}

void rejectsWrongWindowAndContent() {
    {
        Fake fake;
        MapChestWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
        fake.opened(5);
        assert(session.tick(1) == MapChestWindowState::WaitingForServerClose);
        assert(fake.closes == 1);
        fake.result.container_closed = true;
        assert(session.tick(2) == MapChestWindowState::Quarantining);
        fake.quarantine_active = false;
        assert(session.tick(3) == MapChestWindowState::Failed);
    }
    {
        Fake fake;
        MapChestWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
        fake.ready();
        fake.result.x = 99;
        assert(session.tick(1) == MapChestWindowState::WaitingForServerClose);
        assert(fake.closes == 1);
    }
    {
        Fake fake;
        MapChestWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
        fake.ready();
        fake.result.slot_count = 54;
        assert(session.tick(1) == MapChestWindowState::WaitingForServerClose);
        assert(fake.closes == 1);
    }
    {
        Fake fake;
        MapChestWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
        fake.ready();
        fake.result.has_dynamic_container_id = true;
        assert(session.tick(1) == MapChestWindowState::WaitingForServerClose);
        assert(fake.closes == 1 && fake.cancels == 0);
    }
}

void boundedTimeoutAndLateOpenCleanup() {
    Fake fake;
    MapChestWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
    assert(session.tick(MapChestWindowSession::kOpenTimeoutMs) ==
           MapChestWindowState::Quarantining);
    assert(fake.opens == 1 && fake.closes == 0 && fake.cancels == 1);
    fake.quarantine.container_opened = true;
    fake.quarantine.container_id = 8;
    fake.quarantine.container_type = 0;
    assert(session.tick(4001) == MapChestWindowState::Quarantining);
    assert(fake.closes == 1 && fake.marks == 1);
    assert(session.tick(4002) == MapChestWindowState::Quarantining);
    assert(fake.closes == 1);
    fake.quarantine_active = false;
    assert(session.tick(4003) == MapChestWindowState::Failed);
}

void contentTimeoutAndLeaseExpiry() {
    {
        Fake fake;
        MapChestWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
        fake.opened();
        assert(session.tick(3900) == MapChestWindowState::WaitingForContent);
        assert(session.tick(7900) == MapChestWindowState::WaitingForServerClose);
        assert(fake.closes == 1);
    }
    {
        Fake fake;
        MapChestWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
        fake.ready();
        assert(session.tick(1) == MapChestWindowState::CapturedOpen);
        assert(session.tick(1 + MapChestWindowSession::kCaptureLeaseMs) ==
               MapChestWindowState::WaitingForServerClose);
        assert(fake.closes == 1);
    }
}

void closeFailureIsNotRetriedByTick() {
    Fake fake;
    MapChestWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
    fake.ready();
    assert(session.tick(1) == MapChestWindowState::CapturedOpen);
    fake.close_ok = false;
    assert(!session.close(2));
    assert(session.state() == MapChestWindowState::CloseFailed);
    assert(session.tick(3) == MapChestWindowState::CloseFailed);
    assert(fake.closes == 1 && fake.cancels == 0);
    fake.close_ok = true;
    assert(session.close(4));
    assert(session.state() == MapChestWindowState::WaitingForServerClose);
    assert(fake.closes == 2 && fake.cancels == 0);
    fake.result.container_closed = true;
    assert(session.tick(5) == MapChestWindowState::Quarantining);
    fake.quarantine_active = false;
    assert(session.tick(6) == MapChestWindowState::Failed);
}

void exactServerCloseRequiredAndTimeoutIsGuarded() {
    Fake fake;
    MapChestWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
    fake.ready();
    assert(session.tick(1) == MapChestWindowState::CapturedOpen);
    assert(session.close(2));
    assert(session.state() == MapChestWindowState::WaitingForServerClose);
    assert(fake.closes == 1 && fake.cancels == 0);

    // A close receipt for another numeric window must not retire this one.
    fake.result.container_id = 4;
    fake.result.container_closed = true;
    assert(session.tick(3) == MapChestWindowState::WaitingForServerClose);
    assert(fake.cancels == 0 && fake.closes == 1);
    fake.result.container_id = 3;
    fake.result.container_type = 5;
    assert(session.tick(4) == MapChestWindowState::WaitingForServerClose);
    assert(fake.cancels == 0 && fake.closes == 1);

    // A missing exact receipt is an uncertain server state, not permission to
    // drop the capture or open a second chest.
    fake.result.container_closed = false;
    assert(session.tick(2 + MapChestWindowSession::kServerCloseTimeoutMs) ==
           MapChestWindowState::CloseFailed);
    assert(fake.cancels == 0 && fake.opens == 1 && fake.closes == 1);
    assert(!session.close(3 + MapChestWindowSession::kServerCloseTimeoutMs));
    assert(fake.closes == 1);

    // Even after timeout an exact late receipt can retire the guarded window,
    // but the operation remains failed and must not claim completion.
    fake.result.container_type = 0;
    fake.result.container_closed = true;
    assert(session.tick(4 + MapChestWindowSession::kServerCloseTimeoutMs) ==
           MapChestWindowState::Quarantining);
    fake.quarantine_active = false;
    assert(session.tick(5 + MapChestWindowSession::kServerCloseTimeoutMs) ==
           MapChestWindowState::Failed);
}

void externalUiCloseWaitsForExactServerReceipt() {
    Fake fake;
    MapChestWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
    fake.ready();
    assert(session.tick(1) == MapChestWindowState::CapturedOpen);

    assert(session.expectUiClose(2));
    assert(session.state() == MapChestWindowState::WaitingForServerClose);
    assert(fake.closes == 0 && fake.cancels == 0);
    assert(session.capture() == nullptr);
    assert(!session.expectUiClose(3));
    assert(!session.close(3));
    assert(fake.closes == 0);

    // Another window's Close cannot complete this visible chest session.
    fake.result.container_id = 4;
    fake.result.container_closed = true;
    assert(session.tick(4) == MapChestWindowState::WaitingForServerClose);
    fake.result.container_id = 3;
    fake.result.container_type = 5;
    assert(session.tick(5) == MapChestWindowState::WaitingForServerClose);
    assert(fake.closes == 0 && fake.cancels == 0);

    fake.result.container_type = 0;
    assert(session.tick(6) == MapChestWindowState::Quarantining);
    assert(fake.cancels == 1 && fake.closes == 0);
    fake.quarantine_active = false;
    assert(session.tick(7) == MapChestWindowState::Completed);
}

void externalUiCloseTimeoutNeverSendsPacket() {
    Fake fake;
    MapChestWindowSession session;
    std::string error;
    assert(!session.expectUiClose(0, &error));
    assert(!error.empty() && fake.closes == 0);
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
    fake.ready();
    assert(session.tick(1) == MapChestWindowState::CapturedOpen);
    assert(session.expectUiClose(2));
    assert(session.tick(2 + MapChestWindowSession::kUiCloseAwaitTimeoutMs) ==
           MapChestWindowState::CloseFailed);
    assert(fake.closes == 0 && fake.cancels == 0 && fake.opens == 1);
    assert(!session.expectUiClose(3 + MapChestWindowSession::kUiCloseAwaitTimeoutMs));
    assert(!session.close(3 + MapChestWindowSession::kUiCloseAwaitTimeoutMs));
    assert(fake.closes == 0);

    // A late exact receipt releases the mailbox but must not report success.
    fake.result.container_closed = true;
    assert(session.tick(4 + MapChestWindowSession::kUiCloseAwaitTimeoutMs) ==
           MapChestWindowState::Quarantining);
    fake.quarantine_active = false;
    assert(session.tick(5 + MapChestWindowSession::kUiCloseAwaitTimeoutMs) ==
           MapChestWindowState::Failed);
    assert(fake.closes == 0);
}

void exactOutboundUiCloseCanCompleteWithoutInboundEcho() {
    Fake fake;
    MapChestWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
    fake.ready();
    assert(session.tick(1) == MapChestWindowState::CapturedOpen);
    assert(session.expectUiClose(2));
    assert(session.acceptObservedOutboundUiClose(502));
    assert(session.state() == MapChestWindowState::Quarantining);
    assert(fake.cancels == 1 && fake.closes == 0);
    fake.quarantine_active = false;
    assert(session.tick(503) == MapChestWindowState::Completed);
    assert(!session.acceptObservedOutboundUiClose(504));
    assert(fake.closes == 0);
}

void exactFallbackIsOneShotAndRequiresSameWindow() {
    {
        Fake fake;
        MapChestWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
        fake.ready();
        assert(session.tick(1) == MapChestWindowState::CapturedOpen);
        assert(session.expectUiClose(2));
        assert(session.sendExactCloseAfterUiBack(402));
        assert(fake.closes == 1 && fake.last_close_id == 3U &&
               fake.last_close_type == 0U);
        assert(!session.sendExactCloseAfterUiBack(403));
        assert(fake.closes == 1);
        assert(session.acceptObservedOutboundUiClose(904));
        assert(fake.closes == 1);
        fake.quarantine_active = false;
        assert(session.tick(905) == MapChestWindowState::Completed);
    }
    {
        Fake fake;
        MapChestWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
        fake.ready();
        assert(session.tick(1) == MapChestWindowState::CapturedOpen);
        assert(session.expectUiClose(2));
        fake.result.container_id = 4U;
        assert(!session.sendExactCloseAfterUiBack(402));
        assert(fake.closes == 0 &&
               session.state() == MapChestWindowState::CloseFailed);
        assert(!session.sendExactCloseAfterUiBack(403));
        assert(fake.closes == 0);
    }
    {
        Fake fake;
        MapChestWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
        fake.ready();
        assert(session.tick(1) == MapChestWindowState::CapturedOpen);
        assert(session.expectUiClose(2));
        fake.close_ok = false;
        assert(!session.sendExactCloseAfterUiBack(402));
        assert(fake.closes == 1 &&
               session.state() == MapChestWindowState::CloseFailed);
        fake.close_ok = true;
        assert(!session.sendExactCloseAfterUiBack(403));
        assert(fake.closes == 1);
    }
}

void hiddenProofDirectCloseCanFinishWithoutInboundEcho() {
    Fake fake;
    MapChestWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
    fake.ready();
    assert(session.tick(1) == MapChestWindowState::CapturedOpen);
    assert(session.capture() && session.capture()->token == request().token);
    assert(session.close(2));
    assert(fake.closes == 1 && session.state() ==
           MapChestWindowState::WaitingForServerClose);
    std::string error;
    assert(!session.acceptSubmittedCloseWithoutInbound(
        2 + MapChestWindowSession::kNoEchoCloseIsolationMs - 1U, &error));
    assert(!error.empty() && fake.cancels == 0 && fake.closes == 1);
    assert(session.acceptSubmittedCloseWithoutInbound(
        2 + MapChestWindowSession::kNoEchoCloseIsolationMs, &error));
    assert(error.empty() && fake.cancels == 1 && fake.closes == 1);
    fake.quarantine_active = false;
    assert(session.tick(3 + MapChestWindowSession::kNoEchoCloseIsolationMs) ==
           MapChestWindowState::Completed);
    assert(!session.acceptSubmittedCloseWithoutInbound(
        4 + MapChestWindowSession::kNoEchoCloseIsolationMs));
    assert(fake.closes == 1);
}

void hiddenProofNoEchoRejectsChangedWindow() {
    Fake fake;
    MapChestWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapChestWindowState::WaitingForOpen);
    fake.ready();
    assert(session.tick(1) == MapChestWindowState::CapturedOpen);
    assert(session.close(2));
    fake.result.container_id = 4U;
    assert(!session.acceptSubmittedCloseWithoutInbound(
        2 + MapChestWindowSession::kNoEchoCloseIsolationMs));
    assert(session.state() == MapChestWindowState::CloseFailed);
    assert(fake.cancels == 0 && fake.closes == 1);
}

void externalQuarantineAndPreflight() {
    Fake fake;
    fake.quarantine_active = true;
    MapChestWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapChestWindowState::WaitingForQuarantine);
    assert(fake.opens == 0 && fake.arms == 0);
    fake.quarantine.container_opened = true;
    fake.quarantine.container_id = 7;
    assert(session.tick(1) == MapChestWindowState::WaitingForQuarantine);
    assert(fake.closes == 1 && fake.marks == 1);
    fake.quarantine_active = false;
    fake.allowed = false;
    assert(session.tick(2) == MapChestWindowState::Failed);
    assert(fake.opens == 0 && fake.arms == 0);
}

}  // namespace

int main() {
    happyPathAndOneShot();
    rejectsWrongWindowAndContent();
    boundedTimeoutAndLateOpenCleanup();
    contentTimeoutAndLeaseExpiry();
    closeFailureIsNotRetriedByTick();
    exactServerCloseRequiredAndTimeoutIsGuarded();
    externalUiCloseWaitsForExactServerReceipt();
    externalUiCloseTimeoutNeverSendsPacket();
    exactOutboundUiCloseCanCompleteWithoutInboundEcho();
    exactFallbackIsOneShotAndRequiresSameWindow();
    hiddenProofDirectCloseCanFinishWithoutInboundEcho();
    hiddenProofNoEchoRejectsChangedWindow();
    externalQuarantineAndPreflight();
}
