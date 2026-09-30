#include "MapChestUiCloseBridge.h"

#include "ContainerCaptureMailbox.h"

#include <chrono>
#include <mutex>

namespace build_import {
namespace {

constexpr auto kTicketLifetime = std::chrono::seconds(1);

enum class Phase : uint8_t { Idle, Pending, Claimed, Dispatched };

struct Ticket {
    uint64_t token = 0;
    uint8_t window_id = 0;
    uint8_t window_type = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    std::chrono::steady_clock::time_point expires_at{};
    Phase phase = Phase::Idle;
    bool safety_checked = false;
    bool outbound_close_observed = false;
};

std::mutex g_mutex;
Ticket g_ticket;
// A rejected or cancelled ticket is never queued again for the same window.
uint64_t g_last_queued_token = 0;

bool live(const Ticket& ticket) noexcept {
    return IsExactOpenVisibleChestCapture(
        ticket.token, ticket.window_id, ticket.window_type,
        ticket.x, ticket.y, ticket.z);
}

bool expired(const Ticket& ticket) noexcept {
    return ticket.phase != Phase::Dispatched &&
        std::chrono::steady_clock::now() >= ticket.expires_at;
}

}  // namespace

bool QueueMapChestUiClose(uint64_t token, uint8_t window_id,
                          uint8_t window_type, int32_t x, int32_t y,
                          int32_t z) noexcept {
    try {
        if (!IsExactOpenVisibleChestCapture(token, window_id, window_type,
                                             x, y, z)) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (token == 0U || token <= g_last_queued_token) return false;
        if (g_ticket.phase == Phase::Pending ||
            g_ticket.phase == Phase::Claimed) {
            if (!expired(g_ticket)) return false;
            g_ticket = {};
        }
        g_ticket = {token, window_id, window_type, x, y, z,
                    std::chrono::steady_clock::now() + kTicketLifetime,
                    Phase::Pending, false, false};
        g_last_queued_token = token;
        return true;
    } catch (...) {
        return false;
    }
}

uint64_t TakePendingMapChestUiCloseRequest() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_ticket.phase != Phase::Pending) return 0U;
        if (expired(g_ticket) || !live(g_ticket)) {
            g_ticket = {};
            return 0U;
        }
        g_ticket.phase = Phase::Claimed;
        g_ticket.safety_checked = false;
        return g_ticket.token;
    } catch (...) {
        return 0U;
    }
}

bool IsMapChestUiCloseStillSafe(uint64_t token) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (token == 0U || g_ticket.phase != Phase::Claimed ||
            g_ticket.token != token) return false;
        if (expired(g_ticket) || !live(g_ticket)) {
            g_ticket = {};
            return false;
        }
        g_ticket.safety_checked = true;
        return true;
    } catch (...) {
        return false;
    }
}

void MarkMapChestUiCloseDispatched(uint64_t token) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (token != 0U && g_ticket.token == token &&
            g_ticket.phase == Phase::Claimed && g_ticket.safety_checked &&
            !expired(g_ticket)) {
            // Back may have synchronously caused the matching server Close,
            // so an open-mailbox check here would discard a successful key.
            g_ticket.phase = Phase::Dispatched;
        }
    } catch (...) {
    }
}

bool WasMapChestUiCloseDispatched(uint64_t token) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        return token != 0U && g_ticket.token == token &&
            g_ticket.phase == Phase::Dispatched;
    } catch (...) {
        return false;
    }
}

bool HasPendingMapChestUiCloseWindow() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_ticket.token != 0U && g_ticket.safety_checked &&
            (g_ticket.phase == Phase::Claimed ||
             g_ticket.phase == Phase::Dispatched);
    } catch (...) {
        return false;
    }
}

void ObserveMapChestOutboundClose(uint8_t window_id,
                                  uint8_t window_type) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_ticket.safety_checked ||
            (g_ticket.phase != Phase::Claimed &&
             g_ticket.phase != Phase::Dispatched) ||
            g_ticket.window_id != window_id ||
            g_ticket.window_type != window_type ||
            (g_ticket.phase == Phase::Claimed && expired(g_ticket)) ||
            !live(g_ticket)) return;
        g_ticket.outbound_close_observed = true;
    } catch (...) {
    }
}

bool WasMapChestOutboundCloseObserved(uint64_t token) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        return token != 0U && g_ticket.token == token &&
            g_ticket.phase == Phase::Dispatched &&
            g_ticket.outbound_close_observed;
    } catch (...) {
        return false;
    }
}

void CancelMapChestUiClose(uint64_t token) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (token != 0U && g_ticket.token == token) g_ticket = {};
    } catch (...) {
    }
}

}  // namespace build_import
