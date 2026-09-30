#include "../MapVisibleAnvilWindowSession.h"

#include <cassert>
#include <cstdint>
#include <string>

namespace {

using namespace build_import;

struct Fake {
    bool world_ok = true;
    bool prepare_ok = true;
    bool arm_ok = true;
    bool open_ok = true;
    bool close_ok = true;
    bool armed = false;
    bool quarantine_active = false;
    int opens = 0;
    int closes = 0;
    int arms = 0;
    int cancels = 0;
    int marks = 0;
    std::string order;
    ContainerCapturePollState poll = ContainerCapturePollState::WaitingForOpen;
    ContainerCaptureResult result;
    ContainerCaptureQuarantine quarantine;

    static Fake& get(void* context) { return *static_cast<Fake*>(context); }

    static bool verify(void* context, const MapVisibleAnvilWindowRequest&,
                       std::string* error) {
        Fake& fake = get(context);
        if (!fake.world_ok && error) *error = "world changed";
        return fake.world_ok;
    }
    static bool prepare(void* context, const MapVisibleAnvilWindowRequest&,
                        const void** native_block, std::string* error) {
        Fake& fake = get(context);
        if (native_block) *native_block = fake.prepare_ok
            ? reinterpret_cast<const void*>(0x1234) : nullptr;
        if (!fake.prepare_ok && error) *error = "target is not anvil";
        return fake.prepare_ok;
    }
    static bool sendOpen(void* context, const MapVisibleAnvilWindowRequest&,
                         const void* native_block, std::string* error) {
        Fake& fake = get(context);
        assert(native_block == reinterpret_cast<const void*>(0x1234));
        ++fake.opens;
        fake.order += 'O';
        if (!fake.open_ok && error) *error = "click failed";
        return fake.open_ok;
    }
    static bool sendClose(void* context, uint8_t id, uint8_t type,
                          std::string* error) {
        Fake& fake = get(context);
        assert(id != 0U && id != 0xFFU);
        assert(type == 5U || fake.quarantine.token != 0U);
        ++fake.closes;
        fake.order += 'C';
        if (!fake.close_ok && error) *error = "close ABI unavailable";
        return fake.close_ok;
    }
    static bool arm(void* context, uint64_t token, int32_t x, int32_t y,
                    int32_t z) {
        Fake& fake = get(context);
        ++fake.arms;
        if (!fake.arm_ok) return false;
        fake.armed = true;
        fake.result.token = token;
        fake.result.x = x;
        fake.result.y = y;
        fake.result.z = z;
        fake.order += 'A';
        return true;
    }
    static void cancel(void* context, uint64_t token) {
        Fake& fake = get(context);
        ++fake.cancels;
        fake.order += 'X';
        fake.armed = false;
        if (!fake.result.container_opened) {
            fake.quarantine_active = true;
            fake.quarantine = {};
            fake.quarantine.token = token;
            fake.quarantine.from_visible_anvil = true;
        }
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
    MapVisibleAnvilWindowOps ops() {
        return {this, &verify, &prepare, &sendOpen, &sendClose, &arm,
                &cancel, &pollCapture, &pollQuarantine, &mark};
    }
    void opened(uint8_t type = 5U, uint8_t id = 4U) {
        poll = ContainerCapturePollState::WaitingForContent;
        result.container_opened = true;
        result.container_id = id;
        result.container_type = type;
    }
    void ready() {
        opened();
        poll = ContainerCapturePollState::Ready;
        result.slot_count = 3U;
    }
};

MapVisibleAnvilWindowRequest request(uint64_t token = 42U) {
    MapVisibleAnvilWindowRequest result;
    result.token = token;
    result.x = 12;
    result.y = 30;
    result.z = -9;
    return result;
}

void happyPathAndCloseOrder() {
    Fake fake;
    MapVisibleAnvilWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapVisibleAnvilWindowState::WaitingForOpen);
    assert(fake.order == "AO" && fake.arms == 1 && fake.opens == 1);
    fake.opened();
    assert(session.tick(100) == MapVisibleAnvilWindowState::WaitingForContent);
    fake.ready();
    assert(session.tick(101) == MapVisibleAnvilWindowState::Open);
    assert(session.capture() && session.capture()->slot_count == 3U &&
           session.capture()->items.empty());
    assert(session.close(102));
    assert(session.state() == MapVisibleAnvilWindowState::Completed);
    assert(fake.order == "AOCX" && fake.closes == 1 && fake.cancels == 1);
    assert(session.capture() == nullptr);
    assert(!session.close(103));
    assert(session.tick(104) == MapVisibleAnvilWindowState::Completed);
    assert(!session.begin(request(43), fake.ops(), 105));
}

void liveCaptureRequiresFreshWorldAndSameNumericWindow() {
    Fake fake;
    MapVisibleAnvilWindowSession session;
    ContainerCaptureResult observed;
    std::string error;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapVisibleAnvilWindowState::WaitingForOpen);
    fake.ready();
    assert(session.tick(1) == MapVisibleAnvilWindowState::Open);
    assert(session.verifiedLiveCapture(42U, 2U, &observed, &error));
    assert(observed.container_id == 4U && observed.container_type == 5U);
    assert(!session.verifiedLiveCapture(43U, 2U, &observed, &error));
    assert(observed.token == 0U);

    // The input slot may become occupied while the same window remains open.
    CapturedContainerItem map;
    map.slot = 0U;
    map.numeric_id = 358;
    map.count = 1U;
    fake.result.items.push_back(map);
    assert(session.verifiedLiveCapture(42U, 2U, &observed, &error));
    assert(observed.items.size() == 1U);
    fake.world_ok = false;
    assert(!session.verifiedLiveCapture(42U, 2U, &observed, &error));
    fake.world_ok = true;
    fake.result.container_id = 9U;
    assert(!session.verifiedLiveCapture(42U, 2U, &observed, &error));
    fake.result.container_id = 4U;
    fake.result.container_closed = true;
    assert(!session.verifiedLiveCapture(42U, 2U, &observed, &error));
    fake.result.container_closed = false;
    fake.poll = ContainerCapturePollState::WaitingForContent;
    assert(!session.verifiedLiveCapture(42U, 2U, &observed, &error));
    fake.poll = ContainerCapturePollState::Ready;
    assert(!session.verifiedLiveCapture(
        42U, 1U + MapVisibleAnvilWindowSession::kOpenLeaseMs,
        &observed, &error));
}

void rejectsWrongWindowAndOccupiedAnvil() {
    {
        Fake fake;
        MapVisibleAnvilWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapVisibleAnvilWindowState::WaitingForOpen);
        fake.opened(0U);
        assert(session.tick(1) == MapVisibleAnvilWindowState::Failed);
        assert(fake.closes == 0 && fake.cancels == 1);
    }
    {
        Fake fake;
        MapVisibleAnvilWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapVisibleAnvilWindowState::WaitingForOpen);
        fake.ready();
        fake.result.slot_count = 2U;
        assert(session.tick(1) == MapVisibleAnvilWindowState::Failed);
        assert(fake.order == "AOCX");
    }
    {
        Fake fake;
        MapVisibleAnvilWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapVisibleAnvilWindowState::WaitingForOpen);
        fake.ready();
        CapturedContainerItem item;
        item.slot = 1U;
        item.numeric_id = 358;
        item.count = 1U;
        fake.result.items.push_back(item);
        assert(session.tick(1) == MapVisibleAnvilWindowState::Failed);
        assert(fake.closes == 1 && fake.cancels == 1);
    }
}

void preOpenTimeoutClosesOwnedLateWindowOnce() {
    Fake fake;
    MapVisibleAnvilWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapVisibleAnvilWindowState::WaitingForOpen);
    assert(session.tick(MapVisibleAnvilWindowSession::kOpenTimeoutMs) ==
           MapVisibleAnvilWindowState::Quarantining);
    assert(fake.closes == 0 && fake.cancels == 1);
    fake.quarantine.container_opened = true;
    fake.quarantine.container_id = 8U;
    fake.quarantine.container_type = 5U;
    assert(session.tick(4001) == MapVisibleAnvilWindowState::Quarantining);
    assert(fake.closes == 1 && fake.marks == 1);
    assert(session.tick(4002) == MapVisibleAnvilWindowState::Quarantining);
    assert(fake.closes == 1);
    fake.quarantine_active = false;
    assert(session.tick(4003) == MapVisibleAnvilWindowState::Failed);
}

void foreignQuarantineIsNeverClosed() {
    Fake fake;
    fake.quarantine_active = true;
    fake.quarantine.token = 777U;
    fake.quarantine.container_opened = true;
    fake.quarantine.container_id = 9U;
    fake.quarantine.container_type = 0U;
    MapVisibleAnvilWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(1) == MapVisibleAnvilWindowState::WaitingForQuarantine);
    assert(fake.closes == 0 && fake.opens == 0);
    fake.quarantine_active = false;
    assert(session.tick(2) == MapVisibleAnvilWindowState::WaitingForOpen);
    assert(fake.opens == 1);
}

void contentTimeoutAndLeaseExpiry() {
    {
        Fake fake;
        MapVisibleAnvilWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapVisibleAnvilWindowState::WaitingForOpen);
        fake.opened();
        assert(session.tick(3900) == MapVisibleAnvilWindowState::WaitingForContent);
        assert(session.tick(3900 + MapVisibleAnvilWindowSession::kContentTimeoutMs) ==
               MapVisibleAnvilWindowState::Failed);
        assert(fake.closes == 1 && fake.cancels == 1);
    }
    {
        Fake fake;
        MapVisibleAnvilWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapVisibleAnvilWindowState::WaitingForOpen);
        fake.ready();
        assert(session.tick(1) == MapVisibleAnvilWindowState::Open);
        assert(session.tick(1 + MapVisibleAnvilWindowSession::kOpenLeaseMs) ==
               MapVisibleAnvilWindowState::Failed);
        assert(fake.closes == 1 && fake.cancels == 1);
    }
}

void uncertainCloseNeverRetries() {
    Fake fake;
    MapVisibleAnvilWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapVisibleAnvilWindowState::WaitingForOpen);
    fake.ready();
    assert(session.tick(1) == MapVisibleAnvilWindowState::Open);
    fake.close_ok = false;
    assert(!session.close(2));
    assert(session.state() == MapVisibleAnvilWindowState::CloseUncertain);
    assert(fake.closes == 1 && fake.cancels == 0);
    fake.close_ok = true;
    assert(!session.close(3));
    assert(session.tick(4) == MapVisibleAnvilWindowState::CloseUncertain);
    assert(fake.closes == 1);
    assert(session.tick(2 + MapVisibleAnvilWindowSession::kCloseUncertainTimeoutMs) ==
           MapVisibleAnvilWindowState::Failed);
    assert(fake.closes == 1 && fake.cancels == 1);
}

void worldChangeDoesNotCloseStaleWindow() {
    Fake fake;
    MapVisibleAnvilWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapVisibleAnvilWindowState::WaitingForOpen);
    fake.ready();
    assert(session.tick(1) == MapVisibleAnvilWindowState::Open);
    fake.world_ok = false;
    assert(session.tick(2) == MapVisibleAnvilWindowState::Failed);
    assert(fake.closes == 0 && fake.cancels == 1);
}

void failedPreflightAndArmNeverSendOpen() {
    {
        Fake fake;
        fake.prepare_ok = false;
        MapVisibleAnvilWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapVisibleAnvilWindowState::Failed);
        assert(fake.opens == 0 && fake.arms == 0);
    }
    {
        Fake fake;
        fake.arm_ok = false;
        MapVisibleAnvilWindowSession session;
        assert(session.begin(request(), fake.ops(), 0));
        assert(session.tick(0) == MapVisibleAnvilWindowState::Failed);
        assert(fake.opens == 0 && fake.arms == 1 && fake.cancels == 0);
    }
}

void abortFindsArrivedOpenBeforeTick() {
    Fake fake;
    MapVisibleAnvilWindowSession session;
    assert(session.begin(request(), fake.ops(), 0));
    assert(session.tick(0) == MapVisibleAnvilWindowState::WaitingForOpen);
    fake.opened();
    session.abort(1, "user cancelled job");
    assert(session.state() == MapVisibleAnvilWindowState::Failed);
    assert(fake.order == "AOCX");
}

}  // namespace

int main() {
    happyPathAndCloseOrder();
    liveCaptureRequiresFreshWorldAndSameNumericWindow();
    rejectsWrongWindowAndOccupiedAnvil();
    preOpenTimeoutClosesOwnedLateWindowOnce();
    foreignQuarantineIsNeverClosed();
    contentTimeoutAndLeaseExpiry();
    uncertainCloseNeverRetries();
    worldChangeDoesNotCloseStaleWindow();
    failedPreflightAndArmNeverSendOpen();
    abortFindsArrivedOpenBeforeTick();
}
