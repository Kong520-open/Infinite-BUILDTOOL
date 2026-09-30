#include "ProjectionPrinterNativeHotbarSelection.h"

#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include "../tp/MinecraftUpdateHook.h"
#include "dobby.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

namespace build_import {
namespace {

// These offsets belong only to the verified arm64 Bedrock 1.21.120 image
// (protocol 859, SHA-1 ccf9c31f3121a46e89d868dcedb239466b3ac726).  Every
// entry is fingerprinted before it can be hooked or called.
constexpr uintptr_t kHudScreenControllerConstructorRva = 0x06DE2B9CULL;
constexpr uintptr_t kHudScreenControllerDestructorRva = 0x06DE7E44ULL;
constexpr uintptr_t kHudScreenControllerSelectSlotRva = 0x06DF391CULL;
constexpr uintptr_t kHudScreenControllerTickRva = 0x06DE94A8ULL;
constexpr uintptr_t kClientInstanceScreenModelSelectSlotRva = 0x06AA6C54ULL;
constexpr uintptr_t kHudScreenControllerModelOffset = 0x920ULL;
constexpr uintptr_t kHudScreenControllerVtableRva = 0x126DB970ULL;

constexpr std::array<uint8_t, 24U> kHudConstructorFingerprint{{
    0xFDU, 0x7BU, 0xBAU, 0xA9U, 0xFCU, 0x6FU, 0x01U, 0xA9U,
    0xFAU, 0x67U, 0x02U, 0xA9U, 0xF8U, 0x5FU, 0x03U, 0xA9U,
    0xF6U, 0x57U, 0x04U, 0xA9U, 0xF4U, 0x4FU, 0x05U, 0xA9U,
}};
constexpr std::array<uint8_t, 24U> kHudDestructorFingerprint{{
    0xFDU, 0x7BU, 0xBDU, 0xA9U, 0xF5U, 0x0BU, 0x00U, 0xF9U,
    0xF4U, 0x4FU, 0x02U, 0xA9U, 0xFDU, 0x03U, 0x00U, 0x91U,
    0xA8U, 0xC7U, 0x05U, 0x90U, 0x08U, 0xC1U, 0x25U, 0x91U,
}};
constexpr std::array<uint8_t, 24U> kHudSelectSlotFingerprint{{
    0xFFU, 0x43U, 0x02U, 0xD1U, 0xFDU, 0x7BU, 0x06U, 0xA9U,
    0xF6U, 0x57U, 0x07U, 0xA9U, 0xF4U, 0x4FU, 0x08U, 0xA9U,
    0xFDU, 0x83U, 0x01U, 0x91U, 0x55U, 0xD0U, 0x3BU, 0xD5U,
}};
constexpr std::array<uint8_t, 24U> kHudTickFingerprint{{
    0xFFU, 0x43U, 0x02U, 0xD1U, 0xE8U, 0x2BU, 0x00U, 0xFDU,
    0xFDU, 0xFBU, 0x05U, 0xA9U, 0xF7U, 0x37U, 0x00U, 0xF9U,
    0xF6U, 0x57U, 0x07U, 0xA9U, 0xF4U, 0x4FU, 0x08U, 0xA9U,
}};
constexpr std::array<uint8_t, 24U> kSelectSlotFingerprint{{
    0xFDU, 0x7BU, 0xBDU, 0xA9U, 0xF5U, 0x0BU, 0x00U, 0xF9U,
    0xF4U, 0x4FU, 0x02U, 0xA9U, 0xFDU, 0x03U, 0x00U, 0x91U,
    0xF5U, 0x03U, 0x00U, 0xAAU, 0x00U, 0x28U, 0x40U, 0xF9U,
}};

using HudScreenControllerConstructor = void (*)(void*, void*);
using HudScreenControllerDestructor = void (*)(void*);
using HudScreenControllerSelectSlot = void (*)(void*, int32_t, int32_t);
using HudScreenControllerTick = void (*)(void*);
using SelectSlotFunction = void (*)(void*, int32_t, int32_t);

std::atomic<void*> g_original_hud_constructor{nullptr};
std::atomic<void*> g_original_hud_destructor{nullptr};
std::atomic<void*> g_original_hud_select_slot{nullptr};
std::atomic<void*> g_original_hud_tick{nullptr};
std::atomic<void*> g_hud_owner{nullptr};
std::atomic<void*> g_screen_model{nullptr};
std::atomic<uintptr_t> g_minecraft_base{0U};
std::atomic<uintptr_t> g_select_slot_address{0U};
std::atomic<bool> g_constructor_hooked{false};
std::atomic<bool> g_destructor_hooked{false};
std::atomic<bool> g_hud_select_hooked{false};
std::atomic<bool> g_hud_tick_hooked{false};
std::atomic<bool> g_ready{false};
std::mutex g_install_mutex;

bool setError(std::string* error, const char* message) noexcept {
    if (error) {
        try {
            *error = message;
        } catch (...) {
            // The selection path must stay fail-closed even if diagnostic
            // allocation is unavailable.
        }
    }
    return false;
}

bool readPointerField(void* object, uintptr_t offset, void** output) noexcept {
    if (!output) return false;
    *output = nullptr;
    if (!object || offset > std::numeric_limits<uintptr_t>::max() -
                              reinterpret_cast<uintptr_t>(object)) {
        return false;
    }
    const auto* const field = reinterpret_cast<const void*>(
        reinterpret_cast<uintptr_t>(object) + offset);
    if (!IsMemoryReadable(field, sizeof(*output))) return false;
    std::memcpy(output, field, sizeof(*output));
    return true;
}

template <size_t N>
bool resolveFingerprint(uintptr_t base, uintptr_t rva,
                        const std::array<uint8_t, N>& fingerprint,
                        uintptr_t* output) noexcept {
    uintptr_t address = 0U;
    return ResolveMinecraftExecutableOffset(base, rva, fingerprint.size(), &address) &&
        std::memcmp(reinterpret_cast<const void*>(address), fingerprint.data(),
                    fingerprint.size()) == 0 &&
        ((output ? (*output = address, true) : false));
}

bool isExecutableAddressInCurrentImage(uintptr_t base, uintptr_t address) noexcept {
    if (!base || address < base) return false;
    uintptr_t resolved = 0U;
    return ResolveMinecraftExecutableOffset(base, address - base, sizeof(uint32_t),
                                            &resolved) &&
        resolved == address;
}

bool isExpectedHudOwner(uintptr_t base, void* owner) noexcept {
    if (!base || !owner || !IsMemoryReadable(owner, sizeof(uintptr_t))) return false;
    uintptr_t vtable = 0U;
    std::memcpy(&vtable, owner, sizeof(vtable));
    return vtable == base + kHudScreenControllerVtableRva;
}

bool cachedModelStillMatchesOwner(uintptr_t base, void* owner, void* model) noexcept {
    if (!base || !owner || !model || !isExpectedHudOwner(base, owner)) return false;
    void* current_model = nullptr;
    if (!readPointerField(owner, kHudScreenControllerModelOffset, &current_model) ||
        current_model != model || !IsMemoryReadable(model, sizeof(uintptr_t))) {
        return false;
    }
    uintptr_t vtable = 0U;
    std::memcpy(&vtable, model, sizeof(vtable));
    if (!vtable || !IsMemoryReadable(reinterpret_cast<const void*>(vtable),
                                     sizeof(uintptr_t))) {
        return false;
    }
    uintptr_t first_virtual = 0U;
    std::memcpy(&first_virtual, reinterpret_cast<const void*>(vtable),
                sizeof(first_virtual));
    return isExecutableAddressInCurrentImage(base, first_virtual);
}

void clearCachedPairIfMatches(void* owner, void* model) noexcept {
    if (g_hud_owner.load(std::memory_order_acquire) == owner &&
        g_screen_model.load(std::memory_order_acquire) == model) {
        g_screen_model.store(nullptr, std::memory_order_release);
        g_hud_owner.store(nullptr, std::memory_order_release);
    }
}

void cacheModelFromConstructedHud(void* owner) noexcept {
    void* model = nullptr;
    const uintptr_t base = g_minecraft_base.load(std::memory_order_acquire);
    if (!isExpectedHudOwner(base, owner) ||
        !readPointerField(owner, kHudScreenControllerModelOffset, &model) ||
        !model || !IsMemoryReadable(model, sizeof(uintptr_t))) {
        return;
    }
    // Publish model first and owner second. Selection runs only on the game
    // thread, while these atomics also make revoke/destroy observations safe.
    g_screen_model.store(model, std::memory_order_release);
    g_hud_owner.store(owner, std::memory_order_release);
}

void HookHudScreenControllerConstructor(void* owner, void* model_argument) {
    const auto original = reinterpret_cast<HudScreenControllerConstructor>(
        g_original_hud_constructor.load(std::memory_order_acquire));
    if (original) original(owner, model_argument);
    // The current verified constructor receives the screen model as its first
    // explicit argument. Read its finished owned field instead of retaining a
    // borrowed shared_ptr ABI object from the hook boundary.
    cacheModelFromConstructedHud(owner);
}

void HookHudScreenControllerDestructor(void* owner) {
    // Invalidate before running stock destruction: a request from a later
    // LocalPlayer tick can never observe a released ScreenModel.
    const void* const cached_owner = g_hud_owner.load(std::memory_order_acquire);
    const void* const cached_model = g_screen_model.load(std::memory_order_acquire);
    if (cached_owner == owner) {
        clearCachedPairIfMatches(owner, const_cast<void*>(cached_model));
    }
    const auto original = reinterpret_cast<HudScreenControllerDestructor>(
        g_original_hud_destructor.load(std::memory_order_acquire));
    if (original) original(owner);
}

void HookHudScreenControllerSelectSlot(void* owner, int32_t slot, int32_t container_id) {
    // The authorization panel may load this module after the HUD itself was
    // constructed. The established keyboard-event fallback still reaches this
    // original controller method, so capture its already-live model before
    // forwarding exactly the stock selection call. Subsequent ticks use the
    // direct, fully native ScreenModel path below.
    cacheModelFromConstructedHud(owner);
    const auto original = reinterpret_cast<HudScreenControllerSelectSlot>(
        g_original_hud_select_slot.load(std::memory_order_acquire));
    if (original) original(owner, slot, container_id);
}

void HookHudScreenControllerTick(void* owner) {
    const auto original = reinterpret_cast<HudScreenControllerTick>(
        g_original_hud_tick.load(std::memory_order_acquire));
    if (original) original(owner);
    // This update method is the verified per-frame HUD path. Capturing after
    // stock code lets a module loaded after HUD construction recover the live
    // owner/model pair without needing a user interaction.
    cacheModelFromConstructedHud(owner);
}

}  // namespace

bool InitProjectionPrinterNativeHotbarSelection(uintptr_t minecraft_base) noexcept {
#if !defined(__aarch64__)
    (void)minecraft_base;
    return false;
#else
    try {
        if (!minecraft_base) return false;
        std::lock_guard<std::mutex> lock(g_install_mutex);
        const uintptr_t previous_base = g_minecraft_base.load(std::memory_order_acquire);
        if (previous_base != 0U && previous_base != minecraft_base) return false;
        if (previous_base == 0U) {
            g_minecraft_base.store(minecraft_base, std::memory_order_release);
        }

        uintptr_t constructor_address = 0U;
        uintptr_t destructor_address = 0U;
        uintptr_t hud_select_slot_address = 0U;
        uintptr_t hud_tick_address = 0U;
        uintptr_t select_slot_address = 0U;
        if (!resolveFingerprint(minecraft_base, kHudScreenControllerConstructorRva,
                                kHudConstructorFingerprint, &constructor_address) ||
            !resolveFingerprint(minecraft_base, kHudScreenControllerDestructorRva,
                                kHudDestructorFingerprint, &destructor_address) ||
            !resolveFingerprint(minecraft_base, kHudScreenControllerTickRva,
                                kHudTickFingerprint, &hud_tick_address) ||
            !resolveFingerprint(minecraft_base, kClientInstanceScreenModelSelectSlotRva,
                                kSelectSlotFingerprint, &select_slot_address)) {
            g_ready.store(false, std::memory_order_release);
            return false;
        }
        const bool can_hook_existing_hud_selection = resolveFingerprint(
            minecraft_base, kHudScreenControllerSelectSlotRva,
            kHudSelectSlotFingerprint, &hud_select_slot_address);

        if (!g_constructor_hooked.load(std::memory_order_acquire)) {
            void* original = nullptr;
            const int result = DobbyHook(
                reinterpret_cast<void*>(constructor_address),
                reinterpret_cast<void*>(HookHudScreenControllerConstructor), &original);
            if (result != 0 || !original) {
                g_ready.store(false, std::memory_order_release);
                return false;
            }
            g_original_hud_constructor.store(original, std::memory_order_release);
            g_constructor_hooked.store(true, std::memory_order_release);
        }
        if (!g_destructor_hooked.load(std::memory_order_acquire)) {
            void* original = nullptr;
            const int result = DobbyHook(
                reinterpret_cast<void*>(destructor_address),
                reinterpret_cast<void*>(HookHudScreenControllerDestructor), &original);
            if (result != 0 || !original) {
                g_ready.store(false, std::memory_order_release);
                return false;
            }
            g_original_hud_destructor.store(original, std::memory_order_release);
            g_destructor_hooked.store(true, std::memory_order_release);
        }
        if (!g_hud_tick_hooked.load(std::memory_order_acquire)) {
            void* original = nullptr;
            const int result = DobbyHook(
                reinterpret_cast<void*>(hud_tick_address),
                reinterpret_cast<void*>(HookHudScreenControllerTick), &original);
            if (result != 0 || !original) {
                g_ready.store(false, std::memory_order_release);
                return false;
            }
            g_original_hud_tick.store(original, std::memory_order_release);
            g_hud_tick_hooked.store(true, std::memory_order_release);
        }
        if (can_hook_existing_hud_selection &&
            !g_hud_select_hooked.load(std::memory_order_acquire)) {
            void* original = nullptr;
            const int result = DobbyHook(
                reinterpret_cast<void*>(hud_select_slot_address),
                reinterpret_cast<void*>(HookHudScreenControllerSelectSlot), &original);
            if (result == 0 && original) {
                g_original_hud_select_slot.store(original, std::memory_order_release);
                g_hud_select_hooked.store(true, std::memory_order_release);
            }
        }

        g_select_slot_address.store(select_slot_address, std::memory_order_release);
        const bool ready = g_constructor_hooked.load(std::memory_order_acquire) &&
            g_destructor_hooked.load(std::memory_order_acquire) &&
            g_hud_tick_hooked.load(std::memory_order_acquire);
        g_ready.store(ready, std::memory_order_release);
        return ready;
    } catch (...) {
        g_ready.store(false, std::memory_order_release);
        return false;
    }
#endif
}

void ClearProjectionPrinterNativeHotbarSelection() noexcept {
    g_screen_model.store(nullptr, std::memory_order_release);
    g_hud_owner.store(nullptr, std::memory_order_release);
}

bool RequestProjectionPrinterNativeHotbarSelection(int32_t slot, std::string* error) {
#if !defined(__aarch64__)
    (void)slot;
    return setError(error, "当前设备不支持原生热栏选择");
#else
    try {
        if (slot < 0 || slot > 8) {
            return setError(error, "热栏槽位超出范围");
        }
        if (!IsMinecraftUpdateGameThread()) {
            return setError(error, "未在游戏主线程调用热栏选择");
        }
        if (!g_ready.load(std::memory_order_acquire)) {
            return setError(error, "原生热栏选择通路未就绪");
        }
        const uintptr_t base = g_minecraft_base.load(std::memory_order_acquire);
        const uintptr_t select_address =
            g_select_slot_address.load(std::memory_order_acquire);
        const void* const owner = g_hud_owner.load(std::memory_order_acquire);
        const void* const model = g_screen_model.load(std::memory_order_acquire);
        if (!base || !select_address || !owner || !model ||
            !cachedModelStillMatchesOwner(base, const_cast<void*>(owner),
                                          const_cast<void*>(model))) {
            clearCachedPairIfMatches(const_cast<void*>(owner), const_cast<void*>(model));
            return setError(error, "等待游戏 HUD 热栏模型就绪");
        }
        if (!isExecutableAddressInCurrentImage(base, select_address)) {
            return setError(error, "原生热栏选择入口校验失败");
        }
        // ContainerID 0 is the verified original hotbar path for this build.
        reinterpret_cast<SelectSlotFunction>(select_address)(const_cast<void*>(model), slot, 0);
        return true;
    } catch (...) {
        return setError(error, "原生热栏选择调用被安全取消");
    }
#endif
}

}  // namespace build_import
