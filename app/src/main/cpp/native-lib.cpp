#include <jni.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <link.h>
#include <mutex>
#include <thread>
#include <unistd.h>

#include "main.h"
#include "log_control.h"
#include "native_auth.h"
#include "tp/TpModule.h"
#include "tp/LightningEffect.h"
#include "tp/MinecraftUpdateHook.h"
#include "tp/LoopbackPacketSenderCapture.h"
#include "build_import/BuildProjectionRenderer.h"
#include "build_import/MapAnvilUiCloseBridge.h"
#include "build_import/MapChestUiCloseBridge.h"

#define LOG_TAG "BuildToolsNative"

std::atomic<uintptr_t> Main::baseAddress{0};

namespace {
struct MinecraftExecutableAddressQuery {
    uintptr_t module_base = 0;
    uintptr_t address = 0;
    size_t required_size = 0;
    bool module_found = false;
    bool executable = false;
};

bool isMinecraftModulePath(const char* path) {
    if (!path) return false;
    const char* file_name = std::strrchr(path, '/');
    file_name = file_name ? file_name + 1 : path;
    return std::strcmp(file_name, "libminecraftpe.so") == 0 ||
           std::strcmp(file_name, "libminecraftpe.so (deleted)") == 0;
}

int findMinecraftExecutableAddress(dl_phdr_info* info, size_t, void* opaque) {
    auto* query = static_cast<MinecraftExecutableAddressQuery*>(opaque);
    if (!query || static_cast<uintptr_t>(info->dlpi_addr) != query->module_base ||
        !isMinecraftModulePath(info->dlpi_name)) {
        return 0;
    }
    query->module_found = true;
    for (ElfW(Half) index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr)& header = info->dlpi_phdr[index];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) == 0) continue;
        if (query->module_base > std::numeric_limits<uintptr_t>::max() -
                                     static_cast<uintptr_t>(header.p_vaddr)) {
            continue;
        }
        const uintptr_t start = query->module_base + static_cast<uintptr_t>(header.p_vaddr);
        if (start > std::numeric_limits<uintptr_t>::max() -
                        static_cast<uintptr_t>(header.p_memsz)) {
            continue;
        }
        const uintptr_t end = start + static_cast<uintptr_t>(header.p_memsz);
        if (query->address >= start && query->address <= end &&
            query->required_size <= static_cast<size_t>(end - query->address)) {
            query->executable = true;
            return 1;
        }
    }
    return 1;
}

std::atomic<bool> hook_initialization_requested{false};
std::atomic<bool> hooks_initialized{false};
std::mutex hook_initialization_mutex;

void initializeHooksIfReady() {
    std::lock_guard<std::mutex> lock(hook_initialization_mutex);
    if (!hook_initialization_requested.load()) return;
    const uintptr_t base_address = Main::getBaseAddress();
    if (base_address == 0) return;

    if (!InitLoopbackPacketSenderCapture(base_address)) {
        LOGE("Command sender capture is not ready; retrying on next request");
    }
    if (!hooks_initialized.exchange(true)) {
        LOGI("Minecraft library ready; initializing building tool hooks");
    }
    if (!LightningEffect::isReceiveHookReady() && !LightningEffect::init(base_address)) {
        LOGE("Command acknowledgement hook is not ready");
    }
    if (!InitMinecraftUpdateHook(base_address)) {
        LOGE("Game tick hook is not ready");
    }
    if (!build_import::InitBuildProjectionHook(base_address)) {
        LOGE("Projection render hook is not ready");
    }
}

uintptr_t getModuleBase(const char* name) {
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[512];
    uintptr_t base = 0;
    while (std::fgets(line, sizeof(line), maps)) {
        if (std::strstr(line, name)) {
            base = static_cast<uintptr_t>(std::strtoull(line, nullptr, 16));
            break;
        }
    }
    std::fclose(maps);
    return base;
}

void waitForMinecraft() {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const uintptr_t base = getModuleBase("libminecraftpe.so");
        if (base != 0) {
            Main::setBaseAddress(base);
            initializeHooksIfReady();
            return;
        }
        usleep(500000);
    }
    LOGE("Minecraft library was not found");
}

void ensureBuildToolsHooks(JNIEnv*, jclass) {
    if (Main::getBaseAddress() == 0) {
        Main::setBaseAddress(getModuleBase("libminecraftpe.so"));
    }
    hook_initialization_requested.store(true);
    initializeHooksIfReady();
}

jlong takePendingMapAnvilUiCloseRequest(JNIEnv*, jclass) {
    return static_cast<jlong>(build_import::TakePendingMapAnvilUiCloseRequest());
}
jboolean isMapAnvilUiCloseStillSafe(JNIEnv*, jclass, jlong ticket) {
    return build_import::IsMapAnvilUiCloseStillSafe(static_cast<uint64_t>(ticket))
        ? JNI_TRUE : JNI_FALSE;
}
void markMapAnvilUiCloseDispatched(JNIEnv*, jclass, jlong ticket) {
    build_import::MarkMapAnvilUiCloseDispatched(static_cast<uint64_t>(ticket));
}
jlong takePendingMapChestUiCloseRequest(JNIEnv*, jclass) {
    return static_cast<jlong>(build_import::TakePendingMapChestUiCloseRequest());
}
jboolean isMapChestUiCloseStillSafe(JNIEnv*, jclass, jlong ticket) {
    return build_import::IsMapChestUiCloseStillSafe(static_cast<uint64_t>(ticket))
        ? JNI_TRUE : JNI_FALSE;
}
void markMapChestUiCloseDispatched(JNIEnv*, jclass, jlong ticket) {
    build_import::MarkMapChestUiCloseDispatched(static_cast<uint64_t>(ticket));
}

const JNINativeMethod native_methods[] = {
    {"ensureBuildToolsHooks", "()V", reinterpret_cast<void*>(ensureBuildToolsHooks)},
    {"takePendingMapAnvilUiCloseRequest", "()J",
     reinterpret_cast<void*>(takePendingMapAnvilUiCloseRequest)},
    {"isMapAnvilUiCloseStillSafe", "(J)Z",
     reinterpret_cast<void*>(isMapAnvilUiCloseStillSafe)},
    {"markMapAnvilUiCloseDispatched", "(J)V",
     reinterpret_cast<void*>(markMapAnvilUiCloseDispatched)},
    {"takePendingMapChestUiCloseRequest", "()J",
     reinterpret_cast<void*>(takePendingMapChestUiCloseRequest)},
    {"isMapChestUiCloseStillSafe", "(J)Z",
     reinterpret_cast<void*>(isMapChestUiCloseStillSafe)},
    {"markMapChestUiCloseDispatched", "(J)V",
     reinterpret_cast<void*>(markMapChestUiCloseDispatched)},
};
}  // namespace

bool ResolveMinecraftExecutableOffset(uintptr_t base_address, uintptr_t offset,
                                      size_t required_size, uintptr_t* address) {
    if (!address) return false;
    *address = 0;
    if (!base_address || !offset || !required_size ||
        base_address > std::numeric_limits<uintptr_t>::max() - offset) {
        return false;
    }
    const uintptr_t target = base_address + offset;
    if ((target & 0x3U) != 0) return false;
    MinecraftExecutableAddressQuery query{
        base_address, target, required_size, false, false,
    };
    dl_iterate_phdr(findMinecraftExecutableAddress, &query);
    if (!query.module_found || !query.executable) return false;
    *address = target;
    return true;
}

bool EnsureBuildProjectionHooksReady() {
    std::lock_guard<std::mutex> lock(hook_initialization_mutex);
    const uintptr_t base_address = Main::getBaseAddress();
    if (base_address == 0) return false;
    return InitMinecraftUpdateHook(base_address) &&
           build_import::InitBuildProjectionHook(base_address);
}

// Compatibility for native building-tool call sites. Access is local and
// never depends on an account, ticket, server, or expiry timer.
bool IsBuildToolsAuthorized() { return true; }
bool IsNativeSessionAuthorized() { return true; }
bool IsNativeOperationAuthorized() { return true; }

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    jclass clazz = env->FindClass("com/vdl/kong520/NativeCore");
    if (!clazz) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return JNI_ERR;
    }
    const jint registered = env->RegisterNatives(
        clazz, native_methods, sizeof(native_methods) / sizeof(native_methods[0]));
    env->DeleteLocalRef(clazz);
    if (registered != JNI_OK) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return JNI_ERR;
    }
    TpModule::setJavaVM(vm);
    std::thread(waitForMinecraft).detach();
    return JNI_VERSION_1_6;
}
