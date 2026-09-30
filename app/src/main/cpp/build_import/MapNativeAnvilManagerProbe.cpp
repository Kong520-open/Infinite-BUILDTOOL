#include "MapNativeAnvilManagerProbe.h"
#include "MapVisibleAnvilWindowSession.h"

#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__aarch64__)
#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include "dobby.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <elf.h>
#include <limits>
#include <link.h>
#include <mutex>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace build_import {

#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__aarch64__)
namespace {

// libminecraftpe.so Build ID ccf9c31f3121a46e89d868dcedb239466b3ac726.
// The first site is after AnvilContainerManagerController construction, with
// x19 holding its address. The second is its destructor entry, where x0 is
// the dying manager. Both sites are verified before Dobby instrumentation.
constexpr uintptr_t kConstructedRva = 0x088F20CCULL;
constexpr uintptr_t kDestructorRva = 0x088F24B4ULL;
// The deleting-destructor wrapper returns from the complete C++ destructor
// at this site, before it frees the object's storage; x19 is the object.
constexpr uintptr_t kDeleteCompletedRva = 0x088F2538ULL;
// The complete destructor itself tail-branches to the common base destructor
// at 0x0DB8D360. Its final normal-path epilogue is after all object cleanup,
// and still has the object in x19. Unlike the deleting wrapper, this site is
// also reached when the owner destroys the object through a non-deleting path.
constexpr uintptr_t kManagerCompleteEpilogueRva = 0x0DB8D50CULL;
constexpr uintptr_t kManagerVtableRva = 0x127DE658ULL;
// The screen site is on the normal-return path after its constructor's
// post-manager initialization calls. x19 still contains the screen address.
// The screen owns the manager shared_ptr at screen+0xE40.
constexpr uintptr_t kScreenConstructedRva = 0x05FCE588ULL;
constexpr uintptr_t kScreenDestructorRva = 0x05FCF8DCULL;
constexpr uintptr_t kScreenDeleteCompletedRva = 0x05FCF990ULL;
// The screen's complete destructor likewise tail-branches to its common base
// destructor at 0x066A32E8. x19 identifies the screen at its final epilogue.
constexpr uintptr_t kScreenCompleteEpilogueRva = 0x066A35BCULL;
constexpr uintptr_t kScreenVtableRva = 0x125D27E8ULL;
constexpr uintptr_t kCoordinateControllerVtableRva = 0x127DFD68ULL;
constexpr uint8_t kExpectedBuildId[20] = {
    0xCC,0xF9,0xC3,0x1F,0x31,0x21,0xA4,0x6E,0x89,0xD8,
    0x68,0xDC,0xED,0xB2,0x39,0x46,0x6B,0x3A,0xC7,0x26,
};
constexpr uint8_t kConstructedFingerprint[32] = {
    0xC8,0x16,0x40,0xF9,0xA9,0x83,0x5F,0xF8,0x1F,0x01,0x09,0xEB,0x61,0x06,0x00,0x54,
    0xF4,0x4F,0x46,0xA9,0xF7,0x23,0x40,0xF9,0xF6,0x57,0x45,0xA9,0xFD,0x7B,0x43,0xA9,
};
constexpr uint8_t kDestructorFingerprint[32] = {
    0xFD,0x7B,0xBE,0xA9,0xF3,0x0B,0x00,0xF9,0xFD,0x03,0x00,0x91,0x08,0xC0,0x48,0x39,
    0xF3,0x03,0x00,0xAA,0x69,0xF7,0x04,0x90,0x29,0x61,0x19,0x91,0x09,0x00,0x00,0xF9,
};
constexpr uint8_t kDeleteCompletedFingerprint[32] = {
    0xE0,0x03,0x13,0xAA,0xF3,0x0B,0x40,0xF9,0xFD,0x7B,0xC2,0xA8,0xB4,0x8D,0x57,0x15,
    0xFF,0x03,0x01,0xD1,0xFD,0x7B,0x02,0xA9,0xF4,0x4F,0x03,0xA9,0xFD,0x83,0x00,0x91,
};
constexpr uint8_t kManagerCompleteEpilogueFingerprint[32] = {
    0xF4,0x4F,0x46,0xA9,0xF7,0x23,0x40,0xF9,0xF6,0x57,0x45,0xA9,0xFD,0x7B,0x43,0xA9,
    0xFF,0xC3,0x01,0x91,0xC0,0x03,0x5F,0xD6,0xE0,0x03,0x14,0xAA,0x3E,0x89,0x26,0x95,
};
constexpr uint8_t kScreenConstructedFingerprint[32] = {
    0xF4,0x4F,0x4C,0xA9,0xF6,0x57,0x4B,0xA9,0xF8,0x5F,0x4A,0xA9,0xFA,0x67,0x49,0xA9,
    0xFD,0x7B,0x48,0xA9,0xFF,0x43,0x03,0x91,0xC0,0x03,0x5F,0xD6,0xEF,0x2C,0xFD,0x97,
};
constexpr uint8_t kScreenDestructorFingerprint[32] = {
    0xFD,0x7B,0xBE,0xA9,0xF4,0x4F,0x01,0xA9,0xFD,0x03,0x00,0x91,0x08,0x30,0x06,0xF0,
    0x08,0xA1,0x1F,0x91,0xF3,0x03,0x00,0xAA,0x09,0x61,0x0A,0x91,0x08,0x00,0x00,0xF9,
};
constexpr uint8_t kScreenDeleteCompletedFingerprint[32] = {
    0xE0,0x03,0x13,0xAA,0xF3,0x0B,0x40,0xF9,0xFD,0x7B,0xC2,0xA8,0x9E,0x18,0xFC,0x15,
    0xFD,0x7B,0xBE,0xA9,0xF3,0x0B,0x00,0xF9,0xFD,0x03,0x00,0x91,0x13,0xC0,0x1F,0xD1,
};
constexpr uint8_t kScreenCompleteEpilogueFingerprint[32] = {
    0xF4,0x4F,0x45,0xA9,0xF6,0x57,0x44,0xA9,0xF8,0x5F,0x43,0xA9,0xFA,0x67,0x42,0xA9,
    0xFD,0x7B,0x41,0xA9,0xFF,0x83,0x01,0x91,0xC0,0x03,0x5F,0xD6,0x88,0x02,0x40,0xF9,
};
constexpr auto kLease = std::chrono::seconds(45);

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

bool isExactBuild(uintptr_t base) noexcept {
    BuildIdQuery query{base, false};
    dl_iterate_phdr(inspectBuildId, &query);
    return query.matched;
}

template<typename T>
bool readNativeField(uintptr_t object, uintptr_t offset, T* output) noexcept {
    if (!output || !object ||
        object > std::numeric_limits<uintptr_t>::max() - offset ||
        object + offset > std::numeric_limits<uintptr_t>::max() - sizeof(T))
        return false;
    const void* address = reinterpret_cast<const void*>(object + offset);
    if (!IsMemoryReadable(address, sizeof(T))) return false;
    std::memcpy(output, address, sizeof(T));
    return true;
}

std::mutex g_arm_mutex;
std::mutex g_callback_mutex;
bool g_hooks_installed = false;
bool g_install_failed = false;
bool g_has_prior_session = false;
uint64_t g_last_ticket = 0;
uintptr_t g_installed_base = 0;
std::atomic<uint64_t> g_ticket{0};
std::atomic<bool> g_transitioning{false};
std::atomic<bool> g_explicitly_disarmed{false};
std::atomic<bool> g_unowned_constructor{false};
std::atomic<uint32_t> g_callbacks_in_flight{0};
std::atomic<uintptr_t> g_base{0};
std::atomic<int64_t> g_deadline_ns{0};
std::atomic<uintptr_t> g_manager{0};
std::atomic<uint32_t> g_constructor_hits{0};
std::atomic<uint32_t> g_destructor_hits{0};
std::atomic<uint32_t> g_complete_epilogue_hits{0};
std::atomic<uint32_t> g_delete_completed_hits{0};
std::atomic<bool> g_live{false};
std::atomic<bool> g_ambiguous{false};
std::atomic<bool> g_manager_vtable_matches{false};
std::atomic<long> g_manager_tid{0};
std::atomic<uintptr_t> g_screen{0};
std::atomic<uintptr_t> g_screen_manager{0};
std::atomic<uint32_t> g_screen_constructor_hits{0};
std::atomic<uint32_t> g_screen_destructor_hits{0};
std::atomic<uint32_t> g_screen_complete_epilogue_hits{0};
std::atomic<uint32_t> g_screen_delete_completed_hits{0};
std::atomic<bool> g_screen_live{false};
std::atomic<bool> g_screen_ambiguous{false};
std::atomic<bool> g_screen_vtable_matches{false};
std::atomic<long> g_screen_tid{0};

int64_t steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool matchSite(uintptr_t base, uintptr_t rva,
               const uint8_t (&fingerprint)[32], uintptr_t* address) noexcept {
    return address &&
        ResolveMinecraftExecutableOffset(base, rva, 32U, address) &&
        IsMemoryReadable(reinterpret_cast<const void*>(*address), 32U) &&
        std::memcmp(reinterpret_cast<const void*>(*address), fingerprint, 32U) == 0;
}

bool active() noexcept {
    return !g_transitioning.load(std::memory_order_acquire) &&
        g_ticket.load(std::memory_order_acquire) != 0U &&
        steadyNowNs() < g_deadline_ns.load(std::memory_order_acquire);
}

struct CallbackScope final {
    CallbackScope() noexcept {
        g_callbacks_in_flight.fetch_add(1U);
        entered_during_transition = g_transitioning.load();
    }
    ~CallbackScope() noexcept {
        g_callbacks_in_flight.fetch_sub(1U);
    }
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
    bool entered_during_transition = false;
};

MapNativeAnvilRearmEvidence retirementEvidence() noexcept {
    MapNativeAnvilRearmEvidence evidence{};
    evidence.explicitly_disarmed =
        g_explicitly_disarmed.load(std::memory_order_acquire);
    evidence.unowned_constructor_observed =
        g_unowned_constructor.load(std::memory_order_acquire);
    evidence.manager_vtable_matches =
        g_manager_vtable_matches.load(std::memory_order_acquire);
    evidence.screen_vtable_matches =
        g_screen_vtable_matches.load(std::memory_order_acquire);
    const uintptr_t manager = g_manager.load(std::memory_order_acquire);
    evidence.screen_manager_matches = manager != 0U &&
        manager == g_screen_manager.load(std::memory_order_acquire);
    evidence.ambiguous = g_ambiguous.load(std::memory_order_acquire) ||
        g_screen_ambiguous.load(std::memory_order_acquire);
    evidence.manager_constructors =
        g_constructor_hits.load(std::memory_order_acquire);
    evidence.screen_constructors =
        g_screen_constructor_hits.load(std::memory_order_acquire);
    evidence.manager_destructors =
        g_destructor_hits.load(std::memory_order_acquire);
    evidence.screen_destructors =
        g_screen_destructor_hits.load(std::memory_order_acquire);
    evidence.manager_complete_destructor_epilogues =
        g_complete_epilogue_hits.load(std::memory_order_acquire);
    evidence.screen_complete_destructor_epilogues =
        g_screen_complete_epilogue_hits.load(std::memory_order_acquire);
    evidence.manager_delete_completions =
        g_delete_completed_hits.load(std::memory_order_acquire);
    evidence.screen_delete_completions =
        g_screen_delete_completed_hits.load(std::memory_order_acquire);
    evidence.callbacks_in_flight =
        g_callbacks_in_flight.load(std::memory_order_acquire);
    return evidence;
}

void ConstructedCallback(void*, DobbyRegisterContext* context) noexcept {
    CallbackScope scope;
    std::lock_guard<std::mutex> callback_lock(g_callback_mutex);
    if (scope.entered_during_transition || !active()) {
        g_unowned_constructor.store(true, std::memory_order_release);
        return;
    }
    if (!context) {
        g_ambiguous.store(true, std::memory_order_release);
        return;
    }
    const uintptr_t manager = context->general.regs.x19;
    if (!manager) return;
    const uint32_t previous =
        g_constructor_hits.fetch_add(1U, std::memory_order_acq_rel);
    if (previous != 0U) {
        g_ambiguous.store(true, std::memory_order_release);
        return;
    }
    g_manager.store(manager, std::memory_order_release);
    g_manager_tid.store(::syscall(SYS_gettid), std::memory_order_release);
    uintptr_t vtable = 0;
    if (IsMemoryReadable(reinterpret_cast<const void*>(manager), sizeof(vtable))) {
        std::memcpy(&vtable, reinterpret_cast<const void*>(manager), sizeof(vtable));
    }
    g_manager_vtable_matches.store(
        vtable == g_base.load(std::memory_order_acquire) + kManagerVtableRva,
        std::memory_order_release);
    g_live.store(true, std::memory_order_release);
}

void DestructorCallback(void*, DobbyRegisterContext* context) noexcept {
    CallbackScope scope;
    std::lock_guard<std::mutex> callback_lock(g_callback_mutex);
    if (!context) return;
    const uintptr_t manager = context->general.regs.x0;
    if (manager != 0U &&
        manager == g_manager.load(std::memory_order_acquire)) {
        g_destructor_hits.fetch_add(1U, std::memory_order_acq_rel);
        g_live.store(false, std::memory_order_release);
    }
}

void DeleteCompletedCallback(void*, DobbyRegisterContext* context) noexcept {
    CallbackScope scope;
    std::lock_guard<std::mutex> callback_lock(g_callback_mutex);
    if (!context) return;
    const uintptr_t manager = context->general.regs.x19;
    if (manager != 0U &&
        manager == g_manager.load(std::memory_order_acquire)) {
        g_delete_completed_hits.fetch_add(1U, std::memory_order_acq_rel);
        g_live.store(false, std::memory_order_release);
    }
}

void ManagerCompleteEpilogueCallback(void*,
                                     DobbyRegisterContext* context) noexcept {
    CallbackScope scope;
    std::lock_guard<std::mutex> callback_lock(g_callback_mutex);
    if (!context) return;
    const uintptr_t manager = context->general.regs.x19;
    if (manager != 0U &&
        manager == g_manager.load(std::memory_order_acquire)) {
        g_complete_epilogue_hits.fetch_add(1U, std::memory_order_acq_rel);
    }
}

void ScreenConstructedCallback(void*, DobbyRegisterContext* context) noexcept {
    CallbackScope scope;
    std::lock_guard<std::mutex> callback_lock(g_callback_mutex);
    if (scope.entered_during_transition || !active()) {
        g_unowned_constructor.store(true, std::memory_order_release);
        return;
    }
    if (!context) {
        g_screen_ambiguous.store(true, std::memory_order_release);
        return;
    }
    const uintptr_t screen = context->general.regs.x19;
    if (!screen ||
        !IsMemoryReadable(reinterpret_cast<const void*>(screen), sizeof(uintptr_t)) ||
        !IsMemoryReadable(reinterpret_cast<const void*>(screen + 0xE40U),
                          sizeof(uintptr_t))) return;
    const uint32_t previous =
        g_screen_constructor_hits.fetch_add(1U, std::memory_order_acq_rel);
    if (previous != 0U) {
        g_screen_ambiguous.store(true, std::memory_order_release);
        return;
    }
    uintptr_t vtable = 0, manager = 0;
    std::memcpy(&vtable, reinterpret_cast<const void*>(screen), sizeof(vtable));
    std::memcpy(&manager, reinterpret_cast<const void*>(screen + 0xE40U),
                sizeof(manager));
    g_screen.store(screen, std::memory_order_release);
    g_screen_manager.store(manager, std::memory_order_release);
    g_screen_tid.store(::syscall(SYS_gettid), std::memory_order_release);
    g_screen_vtable_matches.store(
        vtable == g_base.load(std::memory_order_acquire) + kScreenVtableRva,
        std::memory_order_release);
    g_screen_live.store(true, std::memory_order_release);
}

void ScreenDestructorCallback(void*, DobbyRegisterContext* context) noexcept {
    CallbackScope scope;
    std::lock_guard<std::mutex> callback_lock(g_callback_mutex);
    if (!context) return;
    const uintptr_t screen = context->general.regs.x0;
    if (screen != 0U &&
        screen == g_screen.load(std::memory_order_acquire)) {
        g_screen_destructor_hits.fetch_add(1U, std::memory_order_acq_rel);
        g_screen_live.store(false, std::memory_order_release);
    }
}

void ScreenDeleteCompletedCallback(void*,
                                   DobbyRegisterContext* context) noexcept {
    CallbackScope scope;
    std::lock_guard<std::mutex> callback_lock(g_callback_mutex);
    if (!context) return;
    const uintptr_t screen = context->general.regs.x19;
    if (screen != 0U &&
        screen == g_screen.load(std::memory_order_acquire)) {
        g_screen_delete_completed_hits.fetch_add(1U, std::memory_order_acq_rel);
        g_screen_live.store(false, std::memory_order_release);
    }
}

void ScreenCompleteEpilogueCallback(void*,
                                    DobbyRegisterContext* context) noexcept {
    CallbackScope scope;
    std::lock_guard<std::mutex> callback_lock(g_callback_mutex);
    if (!context) return;
    const uintptr_t screen = context->general.regs.x19;
    if (screen != 0U &&
        screen == g_screen.load(std::memory_order_acquire)) {
        g_screen_complete_epilogue_hits.fetch_add(1U,
                                                  std::memory_order_acq_rel);
    }
}

bool installHooks(uintptr_t base) noexcept {
    if (g_hooks_installed) return base == g_installed_base;
    if (g_install_failed) return false;
    uintptr_t constructed = 0, destructor = 0, delete_completed = 0;
    uintptr_t complete_epilogue = 0;
    uintptr_t screen_constructed = 0, screen_destructor = 0;
    uintptr_t screen_delete_completed = 0, screen_complete_epilogue = 0;
    if (!matchSite(base, kConstructedRva, kConstructedFingerprint,
                   &constructed) ||
        !matchSite(base, kDestructorRva, kDestructorFingerprint,
                   &destructor) ||
        !matchSite(base, kDeleteCompletedRva, kDeleteCompletedFingerprint,
                   &delete_completed) ||
        !matchSite(base, kManagerCompleteEpilogueRva,
                   kManagerCompleteEpilogueFingerprint,
                   &complete_epilogue) ||
        !matchSite(base, kScreenConstructedRva, kScreenConstructedFingerprint,
                   &screen_constructed) ||
        !matchSite(base, kScreenDestructorRva, kScreenDestructorFingerprint,
                   &screen_destructor) ||
        !matchSite(base, kScreenDeleteCompletedRva,
                   kScreenDeleteCompletedFingerprint,
                   &screen_delete_completed) ||
        !matchSite(base, kScreenCompleteEpilogueRva,
                   kScreenCompleteEpilogueFingerprint,
                   &screen_complete_epilogue)) return false;
    // A partial Dobby installation is not retried in this process.
    g_install_failed = true;
    dobby_set_near_trampoline(true);
    if (DobbyInstrument(reinterpret_cast<void*>(constructed),
                        ConstructedCallback) != 0 ||
        DobbyInstrument(reinterpret_cast<void*>(destructor),
                        DestructorCallback) != 0 ||
        DobbyInstrument(reinterpret_cast<void*>(delete_completed),
                        DeleteCompletedCallback) != 0 ||
        DobbyInstrument(reinterpret_cast<void*>(complete_epilogue),
                        ManagerCompleteEpilogueCallback) != 0 ||
        DobbyInstrument(reinterpret_cast<void*>(screen_constructed),
                        ScreenConstructedCallback) != 0 ||
        DobbyInstrument(reinterpret_cast<void*>(screen_destructor),
                        ScreenDestructorCallback) != 0 ||
        DobbyInstrument(reinterpret_cast<void*>(screen_delete_completed),
                        ScreenDeleteCompletedCallback) != 0 ||
        DobbyInstrument(reinterpret_cast<void*>(screen_complete_epilogue),
                        ScreenCompleteEpilogueCallback) != 0) return false;
    g_hooks_installed = true;
    g_installed_base = base;
    g_install_failed = false;
    return true;
}

}  // namespace
#endif

bool ArmMapNativeAnvilManagerProbe(uintptr_t minecraft_base,
                                   uint64_t ticket) noexcept {
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__aarch64__)
    if (!minecraft_base || !ticket || !isExactBuild(minecraft_base) ||
        minecraft_base > std::numeric_limits<uintptr_t>::max() -
                             kManagerVtableRva ||
        minecraft_base > std::numeric_limits<uintptr_t>::max() -
                             kScreenVtableRva) return false;
    std::lock_guard<std::mutex> lock(g_arm_mutex);
    if (!IsFreshMapNativeAnvilProbeTicket(g_last_ticket, ticket) ||
        g_ticket.load(std::memory_order_acquire) != 0U ||
        g_transitioning.exchange(true)) return false;
    // No lease timeout can retire a pointer. Only explicit disarm, complete
    // complete-destructor epilogues for both objects, and quiescent hooks can
    // make a subsequent ticket eligible. Unknown constructors while no ticket
    // was armed permanently poison this process's probe.
    const bool hooks_ready = installHooks(minecraft_base);
    if (!hooks_ready) {
        g_transitioning.store(false);
        return false;
    }
    // Hook installation itself is done without this mutex. Once installed,
    // callbacks serialize their own state writes with arm/disarm transitions.
    std::lock_guard<std::mutex> callback_lock(g_callback_mutex);
    const bool ready =
        !g_unowned_constructor.load(std::memory_order_acquire) &&
        g_callbacks_in_flight.load() == 0U &&
        (!g_has_prior_session || CanSequentiallyRearmMapNativeAnvilProbe(
            retirementEvidence()));
    if (!ready) {
        g_transitioning.store(false);
        return false;
    }
    g_manager.store(0U, std::memory_order_relaxed);
    g_base.store(minecraft_base, std::memory_order_relaxed);
    g_constructor_hits.store(0U, std::memory_order_relaxed);
    g_destructor_hits.store(0U, std::memory_order_relaxed);
    g_complete_epilogue_hits.store(0U, std::memory_order_relaxed);
    g_delete_completed_hits.store(0U, std::memory_order_relaxed);
    g_live.store(false, std::memory_order_relaxed);
    g_ambiguous.store(false, std::memory_order_relaxed);
    g_manager_vtable_matches.store(false, std::memory_order_relaxed);
    g_manager_tid.store(0, std::memory_order_relaxed);
    g_screen.store(0U, std::memory_order_relaxed);
    g_screen_manager.store(0U, std::memory_order_relaxed);
    g_screen_constructor_hits.store(0U, std::memory_order_relaxed);
    g_screen_destructor_hits.store(0U, std::memory_order_relaxed);
    g_screen_complete_epilogue_hits.store(0U, std::memory_order_relaxed);
    g_screen_delete_completed_hits.store(0U, std::memory_order_relaxed);
    g_screen_live.store(false, std::memory_order_relaxed);
    g_screen_ambiguous.store(false, std::memory_order_relaxed);
    g_screen_vtable_matches.store(false, std::memory_order_relaxed);
    g_screen_tid.store(0, std::memory_order_relaxed);
    g_explicitly_disarmed.store(false, std::memory_order_release);
    g_deadline_ns.store(steadyNowNs() +
        std::chrono::duration_cast<std::chrono::nanoseconds>(kLease).count(),
        std::memory_order_release);
    g_ticket.store(ticket, std::memory_order_release);
    g_has_prior_session = true;
    g_last_ticket = ticket;
    g_transitioning.store(false);
    // A callback that entered during the transition either finished and set
    // the unowned flag, or remains counted in-flight. Do not return a usable
    // ticket if either condition occurred.
    if (g_unowned_constructor.load(std::memory_order_acquire) ||
        g_callbacks_in_flight.load() != 0U) {
        g_ticket.store(0U, std::memory_order_release);
        g_explicitly_disarmed.store(true, std::memory_order_release);
        return false;
    }
    return true;
#else
    (void)minecraft_base;
    (void)ticket;
    return false;
#endif
}

void DisarmMapNativeAnvilManagerProbe(uint64_t ticket) noexcept {
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__aarch64__)
    std::lock_guard<std::mutex> lock(g_arm_mutex);
    std::lock_guard<std::mutex> callback_lock(g_callback_mutex);
    if (ticket && g_ticket.load(std::memory_order_acquire) == ticket) {
        g_ticket.store(0U, std::memory_order_release);
        g_explicitly_disarmed.store(true, std::memory_order_release);
        g_live.store(false, std::memory_order_release);
        g_screen_live.store(false, std::memory_order_release);
        // Keep object identities until both complete-destructor epilogues have
        // been observed. Their hooks may fire after disarm while UI settles.
    }
#else
    (void)ticket;
#endif
}

bool ReadMapNativeAnvilManagerProbe(
    uintptr_t minecraft_base, uint64_t ticket,
    MapNativeAnvilManagerSnapshot* output) noexcept {
    if (!output) return false;
    *output = {};
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__aarch64__)
    if (!minecraft_base || !ticket ||
        g_ticket.load(std::memory_order_acquire) != ticket ||
        g_transitioning.load(std::memory_order_acquire) ||
        g_base.load(std::memory_order_acquire) != minecraft_base ||
        minecraft_base > std::numeric_limits<uintptr_t>::max() -
                             kManagerVtableRva) return false;
    output->ticket = ticket;
    output->constructor_hits = g_constructor_hits.load(std::memory_order_acquire);
    output->destructor_hits = g_destructor_hits.load(std::memory_order_acquire);
    output->screen_constructor_hits =
        g_screen_constructor_hits.load(std::memory_order_acquire);
    output->screen_destructor_hits =
        g_screen_destructor_hits.load(std::memory_order_acquire);
    output->ambiguous = g_ambiguous.load(std::memory_order_acquire) ||
        g_unowned_constructor.load(std::memory_order_acquire);
    output->screen_ambiguous =
        g_screen_ambiguous.load(std::memory_order_acquire);
    output->expired = steadyNowNs() >=
        g_deadline_ns.load(std::memory_order_acquire);
    output->live = !output->expired && !output->ambiguous &&
        output->constructor_hits == 1U && output->destructor_hits == 0U &&
        g_live.load(std::memory_order_acquire);
    output->screen_live = !output->expired && !output->screen_ambiguous &&
        output->screen_constructor_hits == 1U &&
        output->screen_destructor_hits == 0U &&
        g_screen_live.load(std::memory_order_acquire);
    output->vtable_matches = output->live &&
        g_manager_vtable_matches.load(std::memory_order_acquire);
    output->screen_vtable_matches = output->screen_live &&
        g_screen_vtable_matches.load(std::memory_order_acquire);
    // Compare only the addresses captured by lifetime hooks. Never read a
    // screen or manager object from a concurrent Read call: either may be in
    // destruction before the observer thread runs.
    output->screen_manager_matches = output->live && output->screen_live &&
        g_manager.load(std::memory_order_acquire) != 0U &&
        g_manager.load(std::memory_order_acquire) ==
            g_screen_manager.load(std::memory_order_acquire);
    return true;
#else
    (void)minecraft_base;
    (void)ticket;
    return false;
#endif
}

bool ReadMapNativeAnvilProbeRetirement(
    uintptr_t minecraft_base, uint64_t last_ticket,
    MapNativeAnvilRearmEvidence* output) noexcept {
    if (!output) return false;
    *output = {};
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__aarch64__)
    std::lock_guard<std::mutex> lock(g_arm_mutex);
    std::lock_guard<std::mutex> callback_lock(g_callback_mutex);
    if (!minecraft_base || !last_ticket || !g_has_prior_session ||
        minecraft_base != g_installed_base || last_ticket != g_last_ticket) {
        return false;
    }
    *output = retirementEvidence();
    return true;
#else
    (void)minecraft_base;
    (void)last_ticket;
    return false;
#endif
}

bool BorrowMapNativeAnvilScreenForCurrentThread(uintptr_t minecraft_base,
                                                uint64_t ticket,
                                                uintptr_t* screen) noexcept {
    if (!screen) return false;
    *screen = 0;
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__aarch64__)
    MapNativeAnvilManagerSnapshot snapshot{};
    if (!ReadMapNativeAnvilManagerProbe(minecraft_base, ticket, &snapshot) ||
        !snapshot.live || !snapshot.screen_live ||
        !snapshot.vtable_matches || !snapshot.screen_vtable_matches ||
        !snapshot.screen_manager_matches) return false;
    const long tid = ::syscall(SYS_gettid);
    if (tid <= 0 || tid != g_manager_tid.load(std::memory_order_acquire) ||
        tid != g_screen_tid.load(std::memory_order_acquire)) return false;
    const uintptr_t captured_screen = g_screen.load(std::memory_order_acquire);
    const uintptr_t captured_manager = g_manager.load(std::memory_order_acquire);
    if (!captured_screen || !captured_manager ||
        !IsMemoryReadable(reinterpret_cast<const void*>(captured_screen),
                          sizeof(uintptr_t)) ||
        !IsMemoryReadable(reinterpret_cast<const void*>(captured_screen + 0xE40U),
                          sizeof(uintptr_t)) ||
        !IsMemoryReadable(reinterpret_cast<const void*>(captured_manager),
                          sizeof(uintptr_t))) return false;
    uintptr_t screen_vtable = 0, manager_vtable = 0, screen_manager = 0;
    std::memcpy(&screen_vtable, reinterpret_cast<const void*>(captured_screen),
                sizeof(screen_vtable));
    std::memcpy(&screen_manager,
                reinterpret_cast<const void*>(captured_screen + 0xE40U),
                sizeof(screen_manager));
    std::memcpy(&manager_vtable, reinterpret_cast<const void*>(captured_manager),
                sizeof(manager_vtable));
    if (screen_vtable != minecraft_base + kScreenVtableRva ||
        manager_vtable != minecraft_base + kManagerVtableRva ||
        screen_manager != captured_manager) return false;
    *screen = captured_screen;
    return true;
#else
    (void)minecraft_base;
    (void)ticket;
    return false;
#endif
}

bool BorrowMapNativeAnvilManagerForVerifiedWindow(
        uintptr_t minecraft_base, uint64_t ticket, uint64_t now_ms,
        const MapVisibleAnvilWindowSession& session,
        uintptr_t* manager) noexcept {
    if (!manager) return false;
    *manager = 0U;
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__aarch64__)
    if (!minecraft_base || !ticket || !isExactBuild(minecraft_base) ||
        minecraft_base > std::numeric_limits<uintptr_t>::max() -
                             kCoordinateControllerVtableRva) return false;
    ContainerCaptureResult capture;
    try {
        if (!session.verifiedLiveCapture(ticket, now_ms, &capture)) return false;
    } catch (...) {
        // Mailbox snapshots allocate vectors/strings. Never let allocation
        // failure escape a noexcept native borrow into the game tick.
        return false;
    }
    uintptr_t screen = 0U;
    if (!BorrowMapNativeAnvilScreenForCurrentThread(minecraft_base, ticket,
                                                    &screen)) return false;

    // 0x05FCE3B8 calls 0x066A3FDC, which copies constructor x3 BlockPos to
    // screen+0x938/+0x940 and sets screen+0x950 to variant 1. 0x05FCE3D8
    // stores the UI manager shared_ptr at screen+0xE40. 0x0DB8CF80 stores
    // its nested controller shared_ptr at manager+0x18. The same base
    // constructor 0x0DB8CF80 stores x1 at manager+0x18/+0x20; the live
    // getter probe confirmed +0x18 has vtable 0x127DFD68, whereas +0x30
    // is a scalar (0x0B in the observed window). Its 0x0891520C
    // constructor copies the same BlockPos to +0x130/+0x138 and sets type 5
    // at +0x51.  The screen factory 0x0603B738 deliberately passes 0xFF to
    // that constructor; 0x0DBC07A8 stores this unbound ID at +0x50.  Later,
    // the original ContainerOpen handler 0x0771B2D0 checks the packet's
    // type byte [capture+0x39] against controller vtable+0x20, then
    // 0x0771B2E0 passes its ID byte [capture+0x38] to vtable+0x18.  The
    // anvil vtable 0x127DFD68 resolves these slots to 0x0DBC16CC (type
    // getter) and 0x0DBC16C4 (strb window ID to controller+0x50).  Thus
    // +0x50 is meaningful only after that original binding; reject 0xFF
    // or any ID that differs from the fresh ContainerOpen capture.
    uintptr_t ui_manager = 0U, controller = 0U, controller_vtable = 0U;
    if (!readNativeField(screen, 0xE40U, &ui_manager) || !ui_manager ||
        !readNativeField(ui_manager, 0x18U, &controller) || !controller ||
        !readNativeField(controller, 0U, &controller_vtable) ||
        controller_vtable != minecraft_base + kCoordinateControllerVtableRva)
        return false;
    MapNativeAnvilWindowFields fields{};
    fields.capture_x = capture.x;
    fields.capture_y = capture.y;
    fields.capture_z = capture.z;
    fields.capture_id = capture.container_id;
    fields.capture_type = capture.container_type;
    if (!readNativeField(screen, 0x938U, &fields.screen_x) ||
        !readNativeField(screen, 0x93CU, &fields.screen_y) ||
        !readNativeField(screen, 0x940U, &fields.screen_z) ||
        !readNativeField(screen, 0x950U, &fields.screen_target_kind) ||
        !readNativeField(controller, 0x130U, &fields.controller_x) ||
        !readNativeField(controller, 0x134U, &fields.controller_y) ||
        !readNativeField(controller, 0x138U, &fields.controller_z) ||
        !readNativeField(controller, 0x50U, &fields.controller_id) ||
        !readNativeField(controller, 0x51U, &fields.controller_type) ||
        !MatchMapNativeAnvilWindowFields(fields)) return false;
    // Bracket native reads with a second mailbox/world check. The network
    // thread may have delivered Close or replaced a window while fields were
    // read; only the exact same numeric session is borrowable.
    ContainerCaptureResult latest;
    try {
        if (!session.verifiedLiveCapture(ticket, now_ms, &latest)) return false;
    } catch (...) {
        return false;
    }
    if (latest.container_id != capture.container_id ||
        latest.container_type != capture.container_type ||
        latest.x != capture.x || latest.y != capture.y ||
        latest.z != capture.z) return false;
    *manager = ui_manager;
    return true;
#else
    (void)minecraft_base;
    (void)ticket;
    (void)now_ms;
    (void)session;
    return false;
#endif
}

}  // namespace build_import
