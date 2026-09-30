#include "RenderCameraTracker.h"

#include "../main.h"
#include "../tp/FunctionsAddress.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include "dobby.h"

#include <android/log.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

#define LOG_TAG "Infinitecz_CameraTracker"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace build_import {
namespace {

// Current game ABI: the Level::_render owner stores a camera-state pointer at
// owner[172], and that state stores its world origin in floats 903..905.
constexpr uintptr_t kCameraStateOffset = 172U * sizeof(void*);
constexpr uintptr_t kCameraPositionOffset = 903U * sizeof(float);

using LevelRenderFunction = void* (*)(void*, void*, void*);

std::mutex g_hook_mutex;
std::atomic<bool> g_hook_installed{false};
void* g_level_render_original = nullptr;
std::atomic<uint32_t> g_position_sequence{0};
std::atomic<uint32_t> g_position_x{0};
std::atomic<uint32_t> g_position_y{0};
std::atomic<uint32_t> g_position_z{0};
std::atomic<bool> g_position_valid{false};

uint32_t floatBits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bitsFloat(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool readCameraPosition(const void* render_owner, Vec3* output) {
    if (!render_owner || !output) return false;
    const uintptr_t owner_address = reinterpret_cast<uintptr_t>(render_owner);
    if (owner_address > std::numeric_limits<uintptr_t>::max() - kCameraStateOffset) {
        return false;
    }

    const auto* state_field = reinterpret_cast<const void*>(
        owner_address + kCameraStateOffset);
    void* camera_state = nullptr;
    if (!IsMemoryReadable(state_field, sizeof(camera_state))) return false;
    std::memcpy(&camera_state, state_field, sizeof(camera_state));

    const uintptr_t state_address = reinterpret_cast<uintptr_t>(camera_state);
    if (!state_address ||
        state_address > std::numeric_limits<uintptr_t>::max() - kCameraPositionOffset) {
        return false;
    }
    const auto* position = reinterpret_cast<const void*>(
        state_address + kCameraPositionOffset);
    if (!IsMemoryReadable(position, sizeof(*output))) return false;
    std::memcpy(output, position, sizeof(*output));
    return std::isfinite(output->x) && std::isfinite(output->y) &&
        std::isfinite(output->z);
}

void publishCameraPosition(const Vec3& position) {
    // A sequence lock avoids exposing a half-updated tuple to the game-tick
    // thread without putting a mutex on the render path.
    g_position_sequence.fetch_add(1, std::memory_order_acq_rel);
    g_position_x.store(floatBits(position.x), std::memory_order_relaxed);
    g_position_y.store(floatBits(position.y), std::memory_order_relaxed);
    g_position_z.store(floatBits(position.z), std::memory_order_relaxed);
    g_position_valid.store(true, std::memory_order_release);
    g_position_sequence.fetch_add(1, std::memory_order_release);
}

void* LevelRenderHook(void* first, void* second, void* third) noexcept {
    const auto original = reinterpret_cast<LevelRenderFunction>(
        __atomic_load_n(&g_level_render_original, __ATOMIC_ACQUIRE));
    void* result = original ? original(first, second, third) : nullptr;
    Vec3 camera_position;
    if (readCameraPosition(first, &camera_position)) {
        publishCameraPosition(camera_position);
    }
    return result;
}

}  // namespace

bool GetLatestRenderCameraPosition(float* x, float* y, float* z) noexcept {
    if (!x || !y || !z || !g_position_valid.load(std::memory_order_acquire)) {
        return false;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t begin = g_position_sequence.load(std::memory_order_acquire);
        if ((begin & 1U) != 0U) continue;
        const float current_x = bitsFloat(g_position_x.load(std::memory_order_relaxed));
        const float current_y = bitsFloat(g_position_y.load(std::memory_order_relaxed));
        const float current_z = bitsFloat(g_position_z.load(std::memory_order_relaxed));
        const uint32_t end = g_position_sequence.load(std::memory_order_acquire);
        if (begin != end || (end & 1U) != 0U) continue;
        if (!std::isfinite(current_x) || !std::isfinite(current_y) ||
            !std::isfinite(current_z)) {
            return false;
        }
        *x = current_x;
        *y = current_y;
        *z = current_z;
        return true;
    }
    return false;
}

bool InitRenderCameraTrackerHook(uintptr_t base_address) {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (g_hook_installed.load(std::memory_order_acquire)) return true;

    uintptr_t target = 0;
    if (!ResolveMinecraftExecutableOffset(base_address, FunctionsAddress::Level_Render_Hook,
                                          16, &target)) {
        LOGE("Level::_render target is unavailable: base=%p offset=%p",
             reinterpret_cast<void*>(base_address),
             reinterpret_cast<void*>(FunctionsAddress::Level_Render_Hook));
        return false;
    }
    const int result = DobbyHook(reinterpret_cast<void*>(target),
                                 reinterpret_cast<void*>(LevelRenderHook),
                                 &g_level_render_original);
    const void* original = __atomic_load_n(&g_level_render_original, __ATOMIC_ACQUIRE);
    if (result != 0 || !original) {
        LOGE("Level::_render camera tracker hook failed: target=%p result=%d",
             reinterpret_cast<void*>(target), result);
        return false;
    }
    g_hook_installed.store(true, std::memory_order_release);
    LOGI("Level::_render camera tracker installed at %p", reinterpret_cast<void*>(target));
    return true;
}

}  // namespace build_import
