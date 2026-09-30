#ifndef MINECRAFT_MAIN_H
#define MINECRAFT_MAIN_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <android/log.h>

// LOGE 宏由各翻译单元自行定义，避免重定义冲突
// 如需日志，请在 .cpp 文件中定义:
//   #define LOG_TAG "YourTag"
//   #define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

class Main {
public:
    static std::atomic<uintptr_t> baseAddress;

    static uintptr_t getBaseAddress() noexcept {
        return baseAddress.load(std::memory_order_acquire);
    }

    static void setBaseAddress(uintptr_t value) noexcept {
        baseAddress.store(value, std::memory_order_release);
    }
};

// Resolves a version-specific offset only when the complete requested range
// belongs to an executable PT_LOAD segment of the loaded Minecraft image.
bool ResolveMinecraftExecutableOffset(uintptr_t base_address, uintptr_t offset,
                                      size_t required_size, uintptr_t* address);

// Serializes the update/render hooks required by standalone projections with
// the rest of the process-wide Dobby initialization sequence.
bool EnsureBuildProjectionHooksReady();

#endif // MINECRAFT_MAIN_H
