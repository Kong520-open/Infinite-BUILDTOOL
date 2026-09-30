#include "MapAnvilNativeActionTrace.h"

#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && \
    defined(__ANDROID__) && defined(__aarch64__)
#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include "dobby.h"

#include <elf.h>
#include <link.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#endif

namespace build_import {

#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && \
    defined(__ANDROID__) && defined(__aarch64__)
namespace {

// libminecraftpe.so Build ID ccf9c31f3121a46e89d868dcedb239466b3ac726.
constexpr uint8_t kExpectedBuildId[20] = {
    0xCC,0xF9,0xC3,0x1F,0x31,0x21,0xA4,0x6E,0x89,0xD8,
    0x68,0xDC,0xED,0xB2,0x39,0x46,0x6B,0x3A,0xC7,0x26,
};
// The direct AnvilScreenController vtable+0x1E8 handler. The stock result
// callback reaches this site through a different thunk than 0x0603EDEC.
// x0 is the screen, w1 and w3 are values, and x2 is the item-group string.
constexpr uintptr_t kResultHandlerRva = 0x05FD22E8ULL;
constexpr uint8_t kResultHandlerFingerprint[32] = {
    0xFF,0xC3,0x02,0xD1,0xFD,0x7B,0x07,0xA9,0xF7,0x43,0x00,0xF9,0xF6,0x57,0x09,0xA9,
    0xF4,0x4F,0x0A,0xA9,0xFD,0xC3,0x01,0x91,0x57,0xD0,0x3B,0xD5,0x49,0x00,0x80,0x52,
};
// After AnvilContainerManagerController's type-2 recipe lookup copied its
// dynamic recipe ID to the caller's optional<uint32_t> and set present=1.
// x19 still points to that eight-byte optional. This is observation only.
constexpr uintptr_t kRecipeLookupObservedRva = 0x0DA7AF24ULL;
constexpr uint8_t kRecipeLookupObservedFingerprint[32] = {
    0x74,0xFC,0xFF,0xB4,0x81,0x22,0x00,0x91,0x00,0x00,0x80,0x92,0x5C,0xC2,0x2A,0x95,
    0xE0,0xFB,0xFF,0xB5,0x88,0x02,0x40,0xF9,0xE0,0x03,0x14,0xAA,0x08,0x09,0x40,0xF9,
};
constexpr uintptr_t kScreenVtableRva = 0x125D27E8ULL;
constexpr uintptr_t kManagerVtableRva = 0x127DE658ULL;
constexpr auto kLease = std::chrono::seconds(90);
constexpr char kResultGroup[] = "anvil_result_items";

std::mutex g_install_mutex;
bool g_hooks_installed = false;
bool g_install_failed = false;
uintptr_t g_installed_base = 0;
std::atomic<uint64_t> g_ticket{0};
std::atomic<bool> g_unbound{false};
std::atomic<uintptr_t> g_base{0};
std::atomic<int64_t> g_deadline_ns{0};
std::atomic<long> g_arm_tid{0};
std::atomic<uint32_t> g_callback_calls{0};
std::atomic<uint32_t> g_recipe_lookup_calls{0};
std::atomic<bool> g_result_claimed{false};
std::atomic<bool> g_result_seen{false};
std::atomic<bool> g_result_on_arm_thread{false};
std::atomic<int32_t> g_result_w1{-1};
std::atomic<int32_t> g_result_w3{-1};
std::atomic<uint32_t> g_recipe_count{0};
std::array<std::atomic<uint32_t>, 8> g_recipe_ids{};

int64_t steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

size_t align4(size_t value) noexcept { return (value + 3U) & ~size_t{3U}; }

struct BuildIdQuery {
    uintptr_t base;
    bool matched = false;
};

int inspectBuildId(dl_phdr_info* info, size_t, void* opaque) noexcept {
    auto* query = static_cast<BuildIdQuery*>(opaque);
    if (!info || !query || static_cast<uintptr_t>(info->dlpi_addr) != query->base ||
        !info->dlpi_name) return 0;
    const char* leaf = std::strrchr(info->dlpi_name, '/');
    leaf = leaf ? leaf + 1 : info->dlpi_name;
    if (std::strcmp(leaf, "libminecraftpe.so") != 0) return 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const auto& segment = info->dlpi_phdr[i];
        if (segment.p_type != PT_NOTE || segment.p_memsz == 0U ||
            segment.p_memsz > 4096U ||
            query->base > std::numeric_limits<uintptr_t>::max() - segment.p_vaddr)
            continue;
        const auto* note = reinterpret_cast<const uint8_t*>(
            query->base + static_cast<uintptr_t>(segment.p_vaddr));
        const size_t size = static_cast<size_t>(segment.p_memsz);
        if (!IsMemoryReadable(note, size)) continue;
        size_t offset = 0;
        while (size - offset >= sizeof(Elf32_Nhdr)) {
            Elf32_Nhdr header{};
            std::memcpy(&header, note + offset, sizeof(header));
            offset += sizeof(header);
            const size_t name_size = align4(header.n_namesz);
            const size_t desc_size = align4(header.n_descsz);
            if (name_size > size - offset || desc_size > size - offset - name_size)
                break;
            const uint8_t* name = note + offset;
            const uint8_t* desc = note + offset + name_size;
            if (header.n_type == NT_GNU_BUILD_ID && header.n_namesz == 4U &&
                header.n_descsz == sizeof(kExpectedBuildId) &&
                std::memcmp(name, "GNU", 4U) == 0 &&
                std::memcmp(desc, kExpectedBuildId, sizeof(kExpectedBuildId)) == 0) {
                query->matched = true;
                return 1;
            }
            offset += name_size + desc_size;
        }
    }
    return 1;
}

bool exactBuild(uintptr_t base) noexcept {
    BuildIdQuery query{base};
    dl_iterate_phdr(inspectBuildId, &query);
    return query.matched;
}

bool matchSite(uintptr_t base, uintptr_t rva,
               const uint8_t (&fingerprint)[32], uintptr_t* address) noexcept {
    return address && ResolveMinecraftExecutableOffset(base, rva, 32U, address) &&
        IsMemoryReadable(reinterpret_cast<const void*>(*address), 32U) &&
        std::memcmp(reinterpret_cast<const void*>(*address), fingerprint, 32U) == 0;
}

bool active() noexcept {
    return g_ticket.load(std::memory_order_acquire) != 0U &&
        steadyNowNs() < g_deadline_ns.load(std::memory_order_acquire);
}

bool readResultGroup(uintptr_t address) noexcept {
    if (!address || !IsMemoryReadable(reinterpret_cast<const void*>(address), 24U))
        return false;
    uint8_t bytes[24]{};
    std::memcpy(bytes, reinterpret_cast<const void*>(address), sizeof(bytes));
    const size_t expected_length = sizeof(kResultGroup) - 1U;
    if ((bytes[0] & 1U) == 0U) {
        const size_t length = bytes[0] >> 1U;
        return length == expected_length &&
            std::memcmp(bytes + 1U, kResultGroup, expected_length) == 0;
    }
    size_t length = 0;
    uintptr_t data = 0;
    std::memcpy(&length, bytes + 8U, sizeof(length));
    std::memcpy(&data, bytes + 16U, sizeof(data));
    return length == expected_length && data &&
        IsMemoryReadable(reinterpret_cast<const void*>(data), length) &&
        std::memcmp(reinterpret_cast<const void*>(data), kResultGroup,
                    expected_length) == 0;
}

bool liveAnvilScreen(uintptr_t screen, uintptr_t base) noexcept {
    if (!screen || !base ||
        base > std::numeric_limits<uintptr_t>::max() - kManagerVtableRva ||
        screen > std::numeric_limits<uintptr_t>::max() - 0xE40U ||
        !IsMemoryReadable(reinterpret_cast<const void*>(screen), 8U) ||
        !IsMemoryReadable(reinterpret_cast<const void*>(screen + 0xE40U), 8U))
        return false;
    uintptr_t screen_vtable = 0, manager = 0, manager_vtable = 0;
    std::memcpy(&screen_vtable, reinterpret_cast<const void*>(screen), 8U);
    std::memcpy(&manager, reinterpret_cast<const void*>(screen + 0xE40U), 8U);
    if (screen_vtable != base + kScreenVtableRva || !manager ||
        !IsMemoryReadable(reinterpret_cast<const void*>(manager), 8U)) return false;
    std::memcpy(&manager_vtable, reinterpret_cast<const void*>(manager), 8U);
    return manager_vtable == base + kManagerVtableRva;
}

void ResultEventCallback(void*, DobbyRegisterContext* context) noexcept {
    if (!context || !active()) return;
    const uint32_t call = g_callback_calls.fetch_add(1U, std::memory_order_relaxed);
    if (call >= 128U || g_result_seen.load(std::memory_order_acquire)) return;
    const uintptr_t screen = context->general.regs.x0;
    const int32_t w1 = static_cast<int32_t>(context->general.regs.x1);
    const uintptr_t group = context->general.regs.x2;
    const int32_t w3 = static_cast<int32_t>(context->general.regs.x3);
    const uintptr_t base = g_base.load(std::memory_order_acquire);
    if (!liveAnvilScreen(screen, base) || !readResultGroup(group)) return;
    // Keep the exact values from the stock callback. A manual result take on
    // this build used w1=INT_MAX and w3=0; this trace never replays it.
    bool unclaimed = false;
    if (!g_result_claimed.compare_exchange_strong(unclaimed, true,
                                                   std::memory_order_acq_rel)) return;
    g_result_w1.store(w1, std::memory_order_release);
    g_result_w3.store(w3, std::memory_order_release);
    const long callback_tid = ::syscall(SYS_gettid);
    g_result_on_arm_thread.store(
        callback_tid > 0 && callback_tid == g_arm_tid.load(std::memory_order_acquire),
        std::memory_order_release);
    g_result_seen.store(true, std::memory_order_release);
}

void RecipeLookupObservedCallback(void*, DobbyRegisterContext* context) noexcept {
    // Without a bound window, this site can see another screen's recipe.
    if (!context || !active() || g_unbound.load(std::memory_order_acquire)) return;
    const uint32_t call = g_recipe_lookup_calls.fetch_add(
        1U, std::memory_order_relaxed);
    if (call >= 128U) return;
    const uintptr_t output = context->general.regs.x19;
    if (!output || !IsMemoryReadable(reinterpret_cast<const void*>(output), 8U))
        return;
    uint32_t id = 0;
    uint8_t present = 0;
    std::memcpy(&id, reinterpret_cast<const void*>(output), 4U);
    std::memcpy(&present, reinterpret_cast<const void*>(output + 4U), 1U);
    if (present != 1U || id == 0U) return;
    const uint32_t index = g_recipe_count.fetch_add(1U, std::memory_order_acq_rel);
    if (index < g_recipe_ids.size()) {
        g_recipe_ids[index].store(id, std::memory_order_release);
    }
}

bool installHooks(uintptr_t base) noexcept {
    if (g_hooks_installed) return base == g_installed_base;
    if (g_install_failed) return false;
    uintptr_t click = 0, recipe = 0;
    if (!matchSite(base, kResultHandlerRva,
                   kResultHandlerFingerprint, &click) ||
        !matchSite(base, kRecipeLookupObservedRva,
                   kRecipeLookupObservedFingerprint, &recipe)) return false;
    // A partially installed pair stays inert permanently; Dobby hooks are
    // not assumed removable on this game image.
    g_install_failed = true;
    dobby_set_near_trampoline(true);
    if (DobbyInstrument(reinterpret_cast<void*>(click), ResultEventCallback) != 0 ||
        DobbyInstrument(reinterpret_cast<void*>(recipe),
                        RecipeLookupObservedCallback) != 0) return false;
    g_hooks_installed = true;
    g_installed_base = base;
    g_install_failed = false;
    return true;
}

bool armTrace(uintptr_t minecraft_base, uint64_t ticket,
              bool unbound) noexcept {
    if (!minecraft_base || !ticket || !exactBuild(minecraft_base)) return false;
    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (g_ticket.load(std::memory_order_acquire) != 0U ||
        !installHooks(minecraft_base)) return false;
    g_callback_calls.store(0U, std::memory_order_relaxed);
    g_recipe_lookup_calls.store(0U, std::memory_order_relaxed);
    g_result_claimed.store(false, std::memory_order_relaxed);
    g_result_seen.store(false, std::memory_order_relaxed);
    g_result_on_arm_thread.store(false, std::memory_order_relaxed);
    g_result_w1.store(-1, std::memory_order_relaxed);
    g_result_w3.store(-1, std::memory_order_relaxed);
    g_recipe_count.store(0U, std::memory_order_relaxed);
    for (auto& id : g_recipe_ids) id.store(0U, std::memory_order_relaxed);
    g_base.store(minecraft_base, std::memory_order_release);
    g_unbound.store(unbound, std::memory_order_release);
    g_arm_tid.store(::syscall(SYS_gettid), std::memory_order_release);
    g_deadline_ns.store(steadyNowNs() +
        std::chrono::duration_cast<std::chrono::nanoseconds>(kLease).count(),
        std::memory_order_release);
    g_ticket.store(ticket, std::memory_order_release);
    return true;
}

}  // namespace
#endif

bool ArmMapAnvilNativeActionTrace(uintptr_t minecraft_base, uint64_t ticket,
                                  uint8_t window_id, int64_t map_uuid) noexcept {
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && \
    defined(__ANDROID__) && defined(__aarch64__)
    if (!window_id || window_id == 0xFFU || map_uuid == -1) return false;
    return armTrace(minecraft_base, ticket, false);
#else
    (void)minecraft_base; (void)ticket; (void)window_id; (void)map_uuid;
    return false;
#endif
}

bool ArmMapAnvilUnboundNativeActionTrace(uintptr_t minecraft_base,
                                         uint64_t ticket) noexcept {
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && \
    defined(__ANDROID__) && defined(__aarch64__)
    return armTrace(minecraft_base, ticket, true);
#else
    (void)minecraft_base; (void)ticket;
    return false;
#endif
}

bool ReadMapAnvilNativeActionTrace(
        uint64_t ticket, MapAnvilNativeActionTraceSnapshot* output) noexcept {
    if (!output) return false;
    *output = {};
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && \
    defined(__ANDROID__) && defined(__aarch64__)
    if (!ticket || g_ticket.load(std::memory_order_acquire) != ticket) return false;
    output->callback_calls = g_callback_calls.load(std::memory_order_acquire);
    output->recipe_lookup_calls = g_recipe_lookup_calls.load(std::memory_order_acquire);
    output->result_click_seen = g_result_seen.load(std::memory_order_acquire);
    output->result_click_on_arm_thread =
        g_result_on_arm_thread.load(std::memory_order_acquire);
    output->result_w1 = g_result_w1.load(std::memory_order_acquire);
    output->result_w3 = g_result_w3.load(std::memory_order_acquire);
    output->recipe_count = static_cast<uint8_t>(std::min<size_t>(
        g_recipe_count.load(std::memory_order_acquire), output->recipe_ids.size()));
    for (uint8_t i = 0; i < output->recipe_count; ++i) {
        output->recipe_ids[i] = g_recipe_ids[i].load(std::memory_order_acquire);
    }
    return true;
#else
    (void)ticket;
    return false;
#endif
}

void DisarmMapAnvilNativeActionTrace(uint64_t ticket) noexcept {
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && \
    defined(__ANDROID__) && defined(__aarch64__)
    if (ticket && g_ticket.load(std::memory_order_acquire) == ticket) {
        g_ticket.store(0U, std::memory_order_release);
    }
#else
    (void)ticket;
#endif
}

}  // namespace build_import
