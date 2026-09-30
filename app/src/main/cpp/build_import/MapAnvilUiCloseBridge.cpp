#include "MapAnvilUiCloseBridge.h"

#include "MapNativeAnvilManagerProbe.h"
#include "../main.h"

#include <atomic>

namespace build_import {
namespace {

std::atomic<uint64_t> g_pending_ticket{0};
std::atomic<uint64_t> g_claimed_ticket{0};
std::atomic<uint64_t> g_dispatched_ticket{0};

bool exactLiveScreen(uint64_t ticket) noexcept {
    if (!ticket) return false;
    MapNativeAnvilManagerSnapshot snapshot;
    return ReadMapNativeAnvilManagerProbe(Main::getBaseAddress(), ticket,
                                          &snapshot) &&
        !snapshot.expired && !snapshot.ambiguous &&
        !snapshot.screen_ambiguous && snapshot.live &&
        snapshot.screen_live && snapshot.vtable_matches &&
        snapshot.screen_vtable_matches && snapshot.screen_manager_matches &&
        snapshot.constructor_hits == 1U &&
        snapshot.screen_constructor_hits == 1U &&
        snapshot.destructor_hits == 0U &&
        snapshot.screen_destructor_hits == 0U;
}

}  // namespace

bool QueueMapAnvilUiClose(uint64_t ticket) noexcept {
    if (!exactLiveScreen(ticket)) return false;
    if (g_claimed_ticket.load(std::memory_order_acquire) != 0) return false;
    g_dispatched_ticket.store(0, std::memory_order_release);
    uint64_t expected = 0;
    if (!g_pending_ticket.compare_exchange_strong(expected, ticket,
            std::memory_order_acq_rel)) return false;
    return true;
}

uint64_t TakePendingMapAnvilUiCloseRequest() noexcept {
    const uint64_t ticket = g_pending_ticket.load(std::memory_order_acquire);
    if (!ticket) return 0;
    if (!exactLiveScreen(ticket)) {
        uint64_t expected = ticket;
        g_pending_ticket.compare_exchange_strong(expected, 0,
                                                  std::memory_order_acq_rel);
        return 0;
    }
    uint64_t expected = ticket;
    if (!g_pending_ticket.compare_exchange_strong(expected, 0,
                                                   std::memory_order_acq_rel))
        return 0;
    g_claimed_ticket.store(ticket, std::memory_order_release);
    return ticket;
}

bool IsMapAnvilUiCloseStillSafe(uint64_t ticket) noexcept {
    return ticket != 0 &&
        g_claimed_ticket.load(std::memory_order_acquire) == ticket &&
        exactLiveScreen(ticket);
}

void MarkMapAnvilUiCloseDispatched(uint64_t ticket) noexcept {
    uint64_t expected = ticket;
    if (ticket && g_claimed_ticket.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel)) {
        g_dispatched_ticket.store(ticket, std::memory_order_release);
    }
}

bool WasMapAnvilUiCloseDispatched(uint64_t ticket) noexcept {
    return ticket != 0 &&
        g_dispatched_ticket.load(std::memory_order_acquire) == ticket;
}

void CancelMapAnvilUiClose(uint64_t ticket) noexcept {
    if (!ticket) return;
    uint64_t expected = ticket;
    g_pending_ticket.compare_exchange_strong(expected, 0,
                                              std::memory_order_acq_rel);
    expected = ticket;
    g_claimed_ticket.compare_exchange_strong(expected, 0,
                                              std::memory_order_acq_rel);
    expected = ticket;
    g_dispatched_ticket.compare_exchange_strong(expected, 0,
                                                 std::memory_order_acq_rel);
}

}  // namespace build_import
