#include "ProjectionPrinterInventoryPresentationGate.h"

#include "../main.h"
#include "../log_control.h"
#include "dobby.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>

#define LOG_TAG "PrinterInventoryPresentation"

namespace build_import {
namespace {

// Verified against the supplied Bedrock 1.21.120 / protocol 859 image:
//   RTTI 0x127234F8 -> LegacyClientNetworkHandler::handle(...ContainerOpenPacket...)
//   that lambda's operator() is 0x0771A8F4;
//   0x0771AE7C reads the captured packet's type at [x19 + 0x39];
//   only type 0xFF reaches the call at 0x0771AEA4;
//   x0 = [x19 + 0x98] is LocalPlayer, x8 = vtable[0x6F8].
// The LocalPlayer vtable at 0x1270DF68 (RTTI "11LocalPlayer") resolves this
// virtual to 0x074CBC98. It constructs/pushes the inventory screen, returning
// bool. The following tbnz accepts true and skips the failed-open close path.
//
// Cancel this exact UI call, after the normal ContainerOpen receive/dispatch
// work, just as the reference sorter's ContainerOpen handler event does. The
// earlier approach forced this presentation function's slow screen factory
// and changed is_showing_menu. That flag does not prevent ScreenManager::push,
// so the backpack still rendered. No screen or menu getter is hooked now.
constexpr uintptr_t kContainerOpenInventoryUiBranchRva = 0x0771AE7CULL;
constexpr uintptr_t kContainerOpenInventoryUiCallRva = 0x0771AEA4ULL;
constexpr uintptr_t kLocalPlayerOpenInventoryRva = 0x074CBC98ULL;
constexpr std::array<uint8_t, 52U> kContainerOpenInventoryUiFingerprint{{
    0x68U, 0xE6U, 0x40U, 0x39U,  // ldrb w8, [x19, #0x39]
    0x1FU, 0x3DU, 0x00U, 0x71U,  // cmp w8, #0xf
    0x80U, 0x04U, 0x00U, 0x54U,
    0x1FU, 0x65U, 0x00U, 0x71U,  // cmp w8, #0x19
    0x60U, 0x02U, 0x00U, 0x54U,
    0x1FU, 0xFDU, 0x03U, 0x71U,  // cmp w8, #0xff
    0xE1U, 0x04U, 0x00U, 0x54U,  // b.ne other container types
    0x60U, 0x4EU, 0x40U, 0xF9U,  // ldr x0, [x19, #0x98]
    0x08U, 0x00U, 0x40U, 0xF9U,  // ldr x8, [x0]
    0x08U, 0x7DU, 0x43U, 0xF9U,  // ldr x8, [x8, #0x6f8]
    0x00U, 0x01U, 0x3FU, 0xD6U,  // blr x8
    0xC0U, 0x1EU, 0x00U, 0x37U,  // tbnz w0, #0, successful open
    0x68U, 0xE2U, 0x40U, 0x39U,  // ldrb w8, [x19, #0x38]
}};
constexpr std::array<uint8_t, 32U> kLocalPlayerOpenInventoryFingerprint{{
    0xFFU, 0xC3U, 0x01U, 0xD1U,  // sub sp, sp, #0x70
    0xFDU, 0x7BU, 0x04U, 0xA9U,
    0xF5U, 0x2BU, 0x00U, 0xF9U,
    0xF4U, 0x4FU, 0x06U, 0xA9U,
    0xFDU, 0x03U, 0x01U, 0x91U,
    0x55U, 0xD0U, 0x3BU, 0xD5U,
    0xF4U, 0x03U, 0x00U, 0xAAU,
    0xA8U, 0x16U, 0x40U, 0xF9U,
}};

constexpr uint32_t kContainerOpenPacketId = 0x2EU;
constexpr uint8_t kPlayerInventoryContainerType = 0xFFU;

struct PresentationTicket {
    uint64_t generation = 0U;
    uint8_t container_id = 0U;
    uint8_t container_type = 0U;
};

std::atomic<bool> g_installed{false};
std::atomic<uintptr_t> g_expected_open_inventory_target{0U};
std::mutex g_install_mutex;
std::mutex g_ticket_mutex;
PresentationTicket g_ticket;
uint64_t g_latest_generation = 0U;
uint64_t g_completed_generation = 0U;
bool g_session_active = false;
thread_local uint64_t g_suppressed_call_generation = 0U;

bool readPlayerContainerOpenIdentity(std::string_view packet,
                                     uint8_t* container_id,
                                     uint8_t* container_type) noexcept {
    if (!container_id || !container_type) return false;
    uint32_t header = 0U;
    size_t offset = 0U;
    for (uint32_t index = 0U; index < 5U && offset < packet.size(); ++index) {
        const uint8_t byte = static_cast<uint8_t>(packet[offset++]);
        if (index == 4U && (byte & 0xF0U) != 0U) return false;
        header |= static_cast<uint32_t>(byte & 0x7FU) << (index * 7U);
        if ((byte & 0x80U) == 0U) {
            if ((header & 0x3FFU) != kContainerOpenPacketId ||
                packet.size() - offset < 2U) {
                return false;
            }
            *container_id = static_cast<uint8_t>(packet[offset]);
            *container_type = static_cast<uint8_t>(packet[offset + 1U]);
            return *container_type == kPlayerInventoryContainerType;
        }
    }
    return false;
}

template <size_t Size>
bool matchesFingerprint(uintptr_t address,
                        const std::array<uint8_t, Size>& fingerprint) noexcept {
    return address != 0U && std::memcmp(reinterpret_cast<const void*>(address),
                                        fingerprint.data(), fingerprint.size()) == 0;
}

// ABI verified by the caller: one LocalPlayer* argument and a bool result.
// Returning true acknowledges the screen-open handling without creating a
// screen. It does not send packets, mutate inventory, or own a game object.
extern "C" __attribute__((noinline)) bool SuppressPrinterOwnedInventoryUi(
    void* /*local_player*/) noexcept {
    const uint64_t generation = g_suppressed_call_generation;
    g_suppressed_call_generation = 0U;
    std::lock_guard<std::mutex> lock(g_ticket_mutex);
    if (generation != 0U && g_session_active && generation == g_latest_generation) {
        g_completed_generation = generation;
    }
    return true;
}

void ContainerOpenInventoryUiCallPreHandler(void* /*address*/,
                                          DobbyRegisterContext* context) noexcept {
#if defined(__aarch64__)
    if (!context || !g_installed.load(std::memory_order_acquire) ||
        context->general.regs.x0 == 0U ||
        context->general.regs.x8 !=
            g_expected_open_inventory_target.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_ticket_mutex);
    if (!g_session_active || g_ticket.generation == 0U ||
        g_ticket.generation != g_latest_generation) {
        return;
    }

    // x19 is the stock handler's live lambda object. The fingerprinted code
    // just read these exact fields, so this does not chase an inferred pointer
    // or reinterpret a C++ packet/shared_ptr layout. Match both captured wire
    // bytes and the player used by the original virtual call before claiming
    // the one-shot printer ticket. Manual/open-container UI remains stock.
    const uintptr_t capture = context->general.regs.x19;
    if (capture == 0U || capture > std::numeric_limits<uintptr_t>::max() - 0xA0U) return;
    uint8_t container_id = 0U;
    uint8_t container_type = 0U;
    uintptr_t local_player = 0U;
    std::memcpy(&container_id, reinterpret_cast<const void*>(capture + 0x38U), 1U);
    std::memcpy(&container_type, reinterpret_cast<const void*>(capture + 0x39U), 1U);
    std::memcpy(&local_player, reinterpret_cast<const void*>(capture + 0x98U),
                sizeof(local_player));
    if (container_id != g_ticket.container_id ||
        container_type != g_ticket.container_type ||
        local_player != context->general.regs.x0) {
        return;
    }
    g_suppressed_call_generation = g_ticket.generation;
    g_ticket = {};
    context->general.regs.x8 =
        reinterpret_cast<uintptr_t>(&SuppressPrinterOwnedInventoryUi);
#else
    (void)context;
#endif
}

}  // namespace

bool InitProjectionPrinterInventoryPresentationGate(uintptr_t minecraft_base) noexcept {
#if !defined(__aarch64__)
    (void)minecraft_base;
    return false;
#else
    try {
        std::lock_guard<std::mutex> lock(g_install_mutex);
        if (g_installed.load(std::memory_order_acquire)) return true;
        uintptr_t branch_target = 0U;
        uintptr_t open_inventory_target = 0U;
        if (!ResolveMinecraftExecutableOffset(minecraft_base,
                                               kContainerOpenInventoryUiBranchRva,
                                               kContainerOpenInventoryUiFingerprint.size(),
                                               &branch_target) ||
            !matchesFingerprint(branch_target, kContainerOpenInventoryUiFingerprint) ||
            !ResolveMinecraftExecutableOffset(minecraft_base,
                                               kLocalPlayerOpenInventoryRva,
                                               kLocalPlayerOpenInventoryFingerprint.size(),
                                               &open_inventory_target) ||
            !matchesFingerprint(open_inventory_target, kLocalPlayerOpenInventoryFingerprint)) {
            LOGE("printer ContainerOpen inventory UI call ABI fingerprint did not match");
            return false;
        }
        const uintptr_t call_target = branch_target +
            (kContainerOpenInventoryUiCallRva - kContainerOpenInventoryUiBranchRva);
        g_expected_open_inventory_target.store(open_inventory_target,
                                                std::memory_order_release);
        dobby_set_near_trampoline(true);
        const int result = DobbyInstrument(reinterpret_cast<void*>(call_target),
                                           ContainerOpenInventoryUiCallPreHandler);
        if (result != 0) {
            g_expected_open_inventory_target.store(0U, std::memory_order_release);
            LOGE("printer ContainerOpen inventory UI gate installation failed: %d", result);
            return false;
        }
        g_installed.store(true, std::memory_order_release);
        LOGI("printer ContainerOpen inventory UI gate installed at %p",
             reinterpret_cast<void*>(call_target));
        return true;
    } catch (...) {
        return false;
    }
#endif
}

bool IsProjectionPrinterInventoryPresentationGateInstalled() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

bool IsProjectionPrinterInventoryPresentationGateReadyForOpen() noexcept {
    std::lock_guard<std::mutex> lock(g_ticket_mutex);
    return g_installed.load(std::memory_order_acquire) && !g_session_active;
}

bool ArmProjectionPrinterInventoryPresentationGate(std::string_view packet) noexcept {
    uint8_t container_id = 0U;
    uint8_t container_type = 0U;
    if (!readPlayerContainerOpenIdentity(packet, &container_id, &container_type)) return false;
    std::lock_guard<std::mutex> lock(g_ticket_mutex);
    if (!g_installed.load(std::memory_order_acquire) || g_session_active ||
        g_latest_generation == std::numeric_limits<uint64_t>::max()) {
        return false;
    }
    g_ticket.generation = ++g_latest_generation;
    g_ticket.container_id = container_id;
    g_ticket.container_type = container_type;
    g_completed_generation = 0U;
    g_session_active = true;
    // This is tied to the verified native packet and session cancellation, not
    // a 400 ms guess. A busy game tick must not let an accepted printer packet
    // regain permission to push a visible inventory screen.
    return true;
}

uint64_t GetProjectionPrinterInventoryPresentationGateArmGeneration() noexcept {
    std::lock_guard<std::mutex> lock(g_ticket_mutex);
    return g_latest_generation;
}

bool HasProjectionPrinterInventoryPresentationGateCompleted(uint64_t arm_generation) noexcept {
    std::lock_guard<std::mutex> lock(g_ticket_mutex);
    return arm_generation != 0U && g_session_active &&
        g_latest_generation == arm_generation && g_completed_generation == arm_generation;
}

bool GetProjectionPrinterInventoryPresentationGateCompletedGenerationAfter(
    uint64_t previous_generation, uint64_t* output_generation) noexcept {
    if (!output_generation) return false;
    *output_generation = 0U;
    std::lock_guard<std::mutex> lock(g_ticket_mutex);
    if (!g_session_active || g_completed_generation == 0U ||
        g_completed_generation != g_latest_generation ||
        g_completed_generation <= previous_generation) {
        return false;
    }
    *output_generation = g_completed_generation;
    return true;
}

void ExpectProjectionPrinterInventoryPresentationClose(uint8_t /*container_id*/,
                                                       uint8_t /*container_type*/) noexcept {
    // No invisible screen is created, so there is no presentation lifecycle
    // to await. The inventory session mailbox still owns protocol close state.
}

void ObserveProjectionPrinterInventoryPresentationPacket(std::string_view /*packet*/) noexcept {
    // Slot/response/close packets continue through the untouched stock handler.
}

void ClearProjectionPrinterInventoryPresentationGate() noexcept {
    std::lock_guard<std::mutex> lock(g_ticket_mutex);
    g_ticket = {};
    g_completed_generation = 0U;
    g_session_active = false;
    // Keep g_latest_generation monotonic. A replacement call already in flight
    // can no longer complete this session, nor validate any later session.
}

void ServiceProjectionPrinterInventoryPresentationGateOnGameThread() noexcept {
    // Ticket lifetime follows the owning session and its explicit cancellation.
}

}  // namespace build_import
