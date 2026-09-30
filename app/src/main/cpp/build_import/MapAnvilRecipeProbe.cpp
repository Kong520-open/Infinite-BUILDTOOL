#include "MapAnvilRecipeProbe.h"

#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include "dobby.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>

namespace build_import {
namespace {

// Verified only for libminecraftpe.so Build ID
// ccf9c31f3121a46e89d868dcedb239466b3ac726. The getter's x8 is its
// indirect result storage; x1/x2 are const ItemStack& inputs. At both copy
// sites x19 holds that result storage and x1 points to Recipe.networkId.
constexpr uintptr_t kGetterRva = 0x0DA7CDB8ULL;
constexpr uintptr_t kRecipeCopySiteARva = 0x0DA7D1E0ULL;
constexpr uintptr_t kRecipeCopySiteBRva = 0x0DA7D254ULL;
constexpr uintptr_t kGetterReturnRva = 0x0DA7D2A8ULL;
constexpr uintptr_t kRecipeMemberRva = 0x0DE0FE18ULL;
constexpr uintptr_t kRecipeCopyRva = 0x0DE0FE2CULL;
constexpr uintptr_t kItemStackUserDataRva = 0x0E2C41A8ULL;
constexpr uintptr_t kMapUuidFromCompoundRva = 0x0E2B2B10ULL;

constexpr std::array<uint8_t, 32> kGetterFingerprint{{
    0xFD,0x7B,0xBA,0xA9,0xFC,0x6F,0x01,0xA9,0xFA,0x67,0x02,0xA9,0xF8,0x5F,0x03,0xA9,
    0xF6,0x57,0x04,0xA9,0xF4,0x4F,0x05,0xA9,0xFD,0x03,0x00,0x91,0xFF,0xC3,0x0A,0xD1,
}};
constexpr std::array<uint8_t, 32> kCopySiteAFingerprint{{
    0xE0,0x03,0x13,0xAA,0x12,0x4B,0x0E,0x94,0xB0,0xFF,0xFF,0x17,0x88,0x02,0x40,0xF9,
    0x08,0x21,0x40,0xF9,0xE0,0x03,0x14,0xAA,0x00,0x01,0x3F,0xD6,0x09,0x20,0x40,0xA9,
}};
constexpr std::array<uint8_t, 32> kCopySiteBFingerprint{{
    0xE0,0x03,0x13,0xAA,0xF5,0x4A,0x0E,0x94,0xE0,0x83,0x00,0x91,0x4E,0x3C,0x0E,0x94,
    0xE0,0x17,0x40,0xF9,0x60,0x00,0x00,0xB4,0xE0,0x1B,0x00,0xF9,0xEC,0xC9,0x2A,0x95,
}};
constexpr std::array<uint8_t, 4> kReturnFingerprint{{0xC0,0x03,0x5F,0xD6}};
constexpr std::array<uint8_t, 8> kRecipeMemberFingerprint{{
    0x00,0xF0,0x00,0x91,0xC0,0x03,0x5F,0xD6,
}};
constexpr std::array<uint8_t, 12> kRecipeCopyFingerprint{{
    0x28,0x00,0x40,0xB9,0x08,0x00,0x00,0xB9,0xC0,0x03,0x5F,0xD6,
}};
constexpr std::array<uint8_t, 8> kItemStackUserDataFingerprint{{
    0x00,0x08,0x40,0xF9,0xC0,0x03,0x5F,0xD6,
}};
constexpr std::array<uint8_t, 32> kMapUuidFingerprint{{
    0xFD,0x7B,0xBE,0xA9,0xF3,0x0B,0x00,0xF9,0xFD,0x03,0x00,0x91,0xE0,0x01,0x00,0xB4,
    0xA1,0x99,0xFA,0xF0,0x21,0x64,0x23,0x91,0x02,0x01,0x80,0x52,0x83,0x00,0x80,0x52,
}};

constexpr uint32_t kContainerOpenPacketId = 0x2EU;

MapAnvilRecipeProbeState g_state;
std::mutex g_arm_mutex;
std::atomic<uint64_t> g_active_ticket{0};
std::atomic<int64_t> g_expected_map_uuid{-1};
std::atomic<uintptr_t> g_item_stack_user_data{0};
std::atomic<uintptr_t> g_map_uuid_from_compound{0};
bool g_instrumentation_installed = false;
bool g_partial_install_failed = false;

#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
struct AtomicDiagnostics {
    std::atomic<uint16_t> getter_entries{0};
    std::atomic<uint16_t> getter_guard_rejected{0};
    std::atomic<uint16_t> first_uuid_read{0};
    std::atomic<uint16_t> second_uuid_read{0};
    std::atomic<uint16_t> first_uuid_match{0};
    std::atomic<uint16_t> second_uuid_match{0};
    std::atomic<uint16_t> getter_correlated{0};
    std::atomic<uint16_t> copy_entries{0};
    std::atomic<uint16_t> copy_without_getter{0};
    std::atomic<uint16_t> copy_storage_mismatch{0};
    std::atomic<uint16_t> copy_network_id_unreadable{0};
    std::atomic<uint16_t> copy_observed{0};
};
AtomicDiagnostics g_diagnostics;

void count(std::atomic<uint16_t>& counter) noexcept {
    uint16_t current = counter.load(std::memory_order_relaxed);
    while (current < 255U &&
           !counter.compare_exchange_weak(current, current + 1U,
                                          std::memory_order_relaxed)) {}
}

void clearDiagnostics() noexcept {
    g_diagnostics.getter_entries.store(0, std::memory_order_relaxed);
    g_diagnostics.getter_guard_rejected.store(0, std::memory_order_relaxed);
    g_diagnostics.first_uuid_read.store(0, std::memory_order_relaxed);
    g_diagnostics.second_uuid_read.store(0, std::memory_order_relaxed);
    g_diagnostics.first_uuid_match.store(0, std::memory_order_relaxed);
    g_diagnostics.second_uuid_match.store(0, std::memory_order_relaxed);
    g_diagnostics.getter_correlated.store(0, std::memory_order_relaxed);
    g_diagnostics.copy_entries.store(0, std::memory_order_relaxed);
    g_diagnostics.copy_without_getter.store(0, std::memory_order_relaxed);
    g_diagnostics.copy_storage_mismatch.store(0, std::memory_order_relaxed);
    g_diagnostics.copy_network_id_unreadable.store(0, std::memory_order_relaxed);
    g_diagnostics.copy_observed.store(0, std::memory_order_relaxed);
}
#else
void clearDiagnostics() noexcept {}
#endif

struct GetterThreadObservation {
    uint64_t ticket = 0;
    int64_t map_uuid = -1;
    uintptr_t output_storage = 0;
};
thread_local GetterThreadObservation g_getter_thread;

bool matchesContainerOpen(const MapAnvilWindowEvidence& evidence) noexcept {
    if (!evidence.anvil_block_and_window_confirmed) return false;
    const std::string_view wire = evidence.container_open_packet;
    uint32_t header = 0;
    size_t offset = 0;
    for (uint32_t i = 0; i < 5 && offset < wire.size(); ++i) {
        const uint8_t byte = static_cast<uint8_t>(wire[offset++]);
        if (i == 4 && (byte & 0xF0U) != 0) return false;
        header |= static_cast<uint32_t>(byte & 0x7FU) << (i * 7U);
        if ((byte & 0x80U) == 0) {
            return (header & 0x3FFU) == kContainerOpenPacketId &&
                   wire.size() - offset >= 2 &&
                   static_cast<uint8_t>(wire[offset]) == evidence.window_id &&
                   static_cast<uint8_t>(wire[offset + 1]) ==
                       evidence.observed_container_type;
        }
    }
    return false;
}

template <size_t N>
bool resolveAndMatch(uintptr_t base, uintptr_t rva,
                     const std::array<uint8_t, N>& fingerprint,
                     uintptr_t* resolved) noexcept {
    if (!resolved ||
        !ResolveMinecraftExecutableOffset(base, rva, N, resolved) ||
        !IsMemoryReadable(reinterpret_cast<const void*>(*resolved), N)) return false;
    return std::memcmp(reinterpret_cast<const void*>(*resolved),
                       fingerprint.data(), N) == 0;
}

bool readMapUuidFromItemStack(uintptr_t item_stack, int64_t* output) noexcept {
    if (!output || item_stack == 0 ||
        !IsMemoryReadable(reinterpret_cast<const void*>(item_stack), 16)) return false;
    const uintptr_t user_data = g_item_stack_user_data.load(std::memory_order_acquire);
    const uintptr_t map_uuid_reader =
        g_map_uuid_from_compound.load(std::memory_order_acquire);
    if (!user_data || !map_uuid_reader) return false;
    const void* const compound = reinterpret_cast<const void* (*)(const void*)>(
        user_data)(reinterpret_cast<const void*>(item_stack));
    if (!compound || !IsMemoryReadable(compound, sizeof(uintptr_t))) return false;
    const int64_t uuid = reinterpret_cast<int64_t (*)(const void*)>(
        map_uuid_reader)(compound);
    if (uuid == -1) return false;
    *output = uuid;
    return true;
}

void GetterEntry(void*, DobbyRegisterContext* context) noexcept {
#if defined(__aarch64__)
    g_getter_thread = {};
    const uint64_t ticket = g_active_ticket.load(std::memory_order_acquire);
    if (ticket == 0) return;
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
    count(g_diagnostics.getter_entries);
#endif
    if (!context || context->general.regs.x8 == 0) {
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
        count(g_diagnostics.getter_guard_rejected);
#endif
        return;
    }
    const int64_t expected = g_expected_map_uuid.load(std::memory_order_acquire);
    const auto now = std::chrono::steady_clock::now();
    if (expected == -1 || !g_state.Expects(ticket, expected, now)) {
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
        count(g_diagnostics.getter_guard_rejected);
#endif
        return;
    }
    int64_t first_uuid = -1;
    int64_t second_uuid = -1;
    const bool first_read =
        readMapUuidFromItemStack(context->general.regs.x1, &first_uuid);
    const bool second_read =
        readMapUuidFromItemStack(context->general.regs.x2, &second_uuid);
    const bool first_matches = first_read && first_uuid == expected;
    const bool second_matches = second_read && second_uuid == expected;
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
    if (first_read) count(g_diagnostics.first_uuid_read);
    if (second_read) count(g_diagnostics.second_uuid_read);
    if (first_matches) count(g_diagnostics.first_uuid_match);
    if (second_matches) count(g_diagnostics.second_uuid_match);
#endif
    if (!first_matches && !second_matches) return;
    // x0 (the AnvilContainerScreenSimulation*) is intentionally not read.
    g_getter_thread = {ticket, expected, context->general.regs.x8};
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
    count(g_diagnostics.getter_correlated);
#endif
#else
    (void)context;
#endif
}

void RecipeCopySite(void*, DobbyRegisterContext* context) noexcept {
#if defined(__aarch64__)
    const uint64_t ticket = g_active_ticket.load(std::memory_order_acquire);
    if (ticket == 0) return;
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
    count(g_diagnostics.copy_entries);
#endif
    if (!context || g_getter_thread.ticket == 0 ||
        g_getter_thread.ticket != ticket) {
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
        count(g_diagnostics.copy_without_getter);
#endif
        return;
    }
    if (context->general.regs.x19 != g_getter_thread.output_storage) {
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
        count(g_diagnostics.copy_storage_mismatch);
#endif
        return;
    }
    if (context->general.regs.x1 == 0 ||
        !IsMemoryReadable(reinterpret_cast<const void*>(context->general.regs.x1),
                          sizeof(uint32_t))) {
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
        count(g_diagnostics.copy_network_id_unreadable);
#endif
        return;
    }
    uint32_t recipe_network_id = 0;
    std::memcpy(&recipe_network_id,
                reinterpret_cast<const void*>(context->general.regs.x1),
                sizeof(recipe_network_id));
    g_state.Observe(g_getter_thread.ticket, g_getter_thread.map_uuid,
                    recipe_network_id, std::chrono::steady_clock::now());
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
    count(g_diagnostics.copy_observed);
#endif
#else
    (void)context;
#endif
}

bool installReadOnlyHooks(uintptr_t base) noexcept {
#if !defined(__aarch64__)
    (void)base;
    return false;
#else
    if (g_instrumentation_installed) return true;
    if (g_partial_install_failed) return false;
    uintptr_t getter = 0, copy_a = 0, copy_b = 0, unused = 0;
    uintptr_t user_data = 0, map_uuid_reader = 0;
    if (!resolveAndMatch(base, kGetterRva, kGetterFingerprint, &getter) ||
        !resolveAndMatch(base, kRecipeCopySiteARva, kCopySiteAFingerprint, &copy_a) ||
        !resolveAndMatch(base, kRecipeCopySiteBRva, kCopySiteBFingerprint, &copy_b) ||
        !resolveAndMatch(base, kGetterReturnRva, kReturnFingerprint, &unused) ||
        !resolveAndMatch(base, kRecipeMemberRva, kRecipeMemberFingerprint, &unused) ||
        !resolveAndMatch(base, kRecipeCopyRva, kRecipeCopyFingerprint, &unused) ||
        !resolveAndMatch(base, kItemStackUserDataRva,
                         kItemStackUserDataFingerprint, &user_data) ||
        !resolveAndMatch(base, kMapUuidFromCompoundRva,
                         kMapUuidFingerprint, &map_uuid_reader)) return false;
    g_item_stack_user_data.store(user_data, std::memory_order_release);
    g_map_uuid_from_compound.store(map_uuid_reader, std::memory_order_release);
    // If an installation is only partially successful, callbacks remain
    // permanently inert and this process never retries. No unhook is assumed.
    g_partial_install_failed = true;
    dobby_set_near_trampoline(true);
    if (DobbyInstrument(reinterpret_cast<void*>(getter), GetterEntry) != 0 ||
        DobbyInstrument(reinterpret_cast<void*>(copy_a), RecipeCopySite) != 0 ||
        DobbyInstrument(reinterpret_cast<void*>(copy_b), RecipeCopySite) != 0) {
        return false;
    }
    g_instrumentation_installed = true;
    g_partial_install_failed = false;
    return true;
#endif
}

}  // namespace

bool ArmMapAnvilRecipeProbe(uintptr_t minecraft_base, uint64_t ticket,
                            int64_t expected_map_uuid,
                            const MapAnvilWindowEvidence& window) noexcept {
    if (!minecraft_base || !ticket || expected_map_uuid == -1 ||
        !matchesContainerOpen(window)) return false;
    std::lock_guard<std::mutex> lock(g_arm_mutex);
    if (g_active_ticket.load(std::memory_order_acquire) != 0 ||
        !installReadOnlyHooks(minecraft_base) ||
        !g_state.Arm(ticket, window.window_id, expected_map_uuid,
                     std::chrono::steady_clock::now())) return false;
    clearDiagnostics();
    g_expected_map_uuid.store(expected_map_uuid, std::memory_order_release);
    g_active_ticket.store(ticket, std::memory_order_release);
    return true;
}

void DisarmMapAnvilRecipeProbe(uint64_t ticket) noexcept {
    std::lock_guard<std::mutex> lock(g_arm_mutex);
    if (ticket == 0 ||
        g_active_ticket.load(std::memory_order_acquire) != ticket) return;
    g_active_ticket.store(0, std::memory_order_release);
    g_expected_map_uuid.store(-1, std::memory_order_release);
    g_state.Disarm(ticket);
}

MapAnvilRecipeProbeStatus ReadMapAnvilRecipeProbe(
    uint64_t ticket, int64_t expected_map_uuid,
    MapAnvilRecipeObservation* observation) noexcept {
    if (observation) *observation = {};
    if (ticket == 0 || ticket != g_active_ticket.load(std::memory_order_acquire)) {
        return MapAnvilRecipeProbeStatus::Unavailable;
    }
    return g_state.Read(ticket, expected_map_uuid,
                        std::chrono::steady_clock::now(), observation);
}

bool ReadMapAnvilRecipeProbeDiagnostics(
    uint64_t ticket, MapAnvilRecipeProbeDiagnostics* diagnostics) noexcept {
    if (!diagnostics) return false;
    *diagnostics = {};
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
    if (ticket == 0 ||
        ticket != g_active_ticket.load(std::memory_order_acquire)) return false;
    diagnostics->getter_entries =
        g_diagnostics.getter_entries.load(std::memory_order_relaxed);
    diagnostics->getter_guard_rejected =
        g_diagnostics.getter_guard_rejected.load(std::memory_order_relaxed);
    diagnostics->first_uuid_read =
        g_diagnostics.first_uuid_read.load(std::memory_order_relaxed);
    diagnostics->second_uuid_read =
        g_diagnostics.second_uuid_read.load(std::memory_order_relaxed);
    diagnostics->first_uuid_match =
        g_diagnostics.first_uuid_match.load(std::memory_order_relaxed);
    diagnostics->second_uuid_match =
        g_diagnostics.second_uuid_match.load(std::memory_order_relaxed);
    diagnostics->getter_correlated =
        g_diagnostics.getter_correlated.load(std::memory_order_relaxed);
    diagnostics->copy_entries =
        g_diagnostics.copy_entries.load(std::memory_order_relaxed);
    diagnostics->copy_without_getter =
        g_diagnostics.copy_without_getter.load(std::memory_order_relaxed);
    diagnostics->copy_storage_mismatch =
        g_diagnostics.copy_storage_mismatch.load(std::memory_order_relaxed);
    diagnostics->copy_network_id_unreadable =
        g_diagnostics.copy_network_id_unreadable.load(std::memory_order_relaxed);
    diagnostics->copy_observed =
        g_diagnostics.copy_observed.load(std::memory_order_relaxed);
    return true;
#else
    (void)ticket;
    return false;
#endif
}

}  // namespace build_import
