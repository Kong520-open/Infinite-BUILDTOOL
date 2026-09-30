#include "LoopbackPacketSenderCapture.h"
#include "../build_import/CommandBlockPacketAbiProfile.h"
#include "../build_import/ProjectionPrinterSilentRotation.h"
#include "../build_import/MapTextureObservation.h"
#include "../build_import/MapManualOutboundTrace.h"
#include "../build_import/MapNativeAnvilResultActionBridge.h"
#include "../build_import/MapChestUiCloseBridge.h"
#include "../main.h"
#include "../log_control.h"
#include "dobby.h"
#include <unistd.h>
#include <string>
#include <mutex>
#include <fcntl.h>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>

#define LOG_TAG "LoopbackSenderCapture"

static std::atomic<void*> g_loopbackPacketSender(nullptr);
static std::array<uint8_t, 32> g_loopbackSendToServerOriginalPrologue{};
static std::atomic<bool> g_loopbackSendToServerOriginalPrologueCaptured(false);
static std::atomic<bool> g_senderOnlyCaptureInstalled(false);
static std::mutex g_senderOnlyCaptureInstallMutex;

void* GetCapturedLoopbackPacketSender() {
    return g_loopbackPacketSender.load();
}

bool CapturedLoopbackPacketSenderOriginalPrologueMatches(const uint8_t* expected,
                                                         size_t size) {
    if (!expected || size == 0 || size > g_loopbackSendToServerOriginalPrologue.size() ||
        !g_loopbackSendToServerOriginalPrologueCaptured.load(std::memory_order_acquire)) {
        return false;
    }
    return std::memcmp(g_loopbackSendToServerOriginalPrologue.data(), expected, size) == 0;
}

// Thread-local pipe to safely check memory readability
struct ThreadPipe {
    int readFd = -1;
    int writeFd = -1;
    
    ThreadPipe() {
        int pipefd[2];
        if (pipe(pipefd) == 0) {
            readFd = pipefd[0];
            writeFd = pipefd[1];
            fcntl(writeFd, F_SETFL, O_NONBLOCK);
        }
    }
    
    ~ThreadPipe() {
        if (readFd != -1) close(readFd);
        if (writeFd != -1) close(writeFd);
    }
};

static thread_local ThreadPipe tl_pipe;

// Function pointer to original sendToServer
using SendToServerFunction = void (*)(void* self, void* packet);
static std::atomic<SendToServerFunction> g_senderOnlyOriginalSendToServer(nullptr);

// Safe memory readability check using thread-local pipe (chunked to prevent pipe overflow on large blocks)
bool IsMemoryReadable(const void* ptr, size_t size) {
    if (!ptr || tl_pipe.writeFd == -1 || tl_pipe.readFd == -1) return false;
    
    const char* bytePtr = (const char*)ptr;
    size_t offset = 0;
    while (offset < size) {
        size_t chunkSize = (size - offset > 4096) ? 4096 : (size - offset);
        ssize_t written = write(tl_pipe.writeFd, bytePtr + offset, chunkSize);
        if (written != (ssize_t)chunkSize) {
            return false;
        }
        
        // Drain the pipe immediately to keep it empty
        char buf[4096];
        size_t toRead = chunkSize;
        while (toRead > 0) {
            size_t chunk = toRead > sizeof(buf) ? sizeof(buf) : toRead;
            read(tl_pipe.readFd, buf, chunk);
            toRead -= chunk;
        }
        
        offset += chunkSize;
    }
    return true;
}

// Safely get class name using RTTI (Itanium ABI) without invoking any virtual functions
std::string SafeGetClassName(const void* packet) {
    if (!packet) return "";
    
    // 1. Read vtable pointer
    if (!IsMemoryReadable(packet, 8)) return "";
    void** vtable = *(void***)packet;
    if (!vtable) return "";
    
    // 2. Read type_info pointer from vtable[-1]
    if (!IsMemoryReadable(vtable - 1, 8)) return "";
    void* typeInfo = vtable[-1];
    if (!typeInfo) return "";
    
    // 3. In Itanium ABI, type_info has name pointer at offset 8
    if (!IsMemoryReadable((char*)typeInfo + 8, 8)) return "";
    const char* namePtr = *(const char**)((char*)typeInfo + 8);
    if (!namePtr) return "";
    
    // 4. Safely read mangled name character by character
    std::string mangledName;
    for (int i = 0; i < 128; i++) {
        char c;
        if (!IsMemoryReadable(namePtr + i, 1)) break;
        c = namePtr[i];
        if (c == '\0') break;
        mangledName.push_back(c);
    }
    return mangledName;
}

// Capture the sender and inspect only packets required by building tools.
struct PendingMapChestOutboundClose {
    bool matches = false;
    uint8_t window_id = 0;
    uint8_t window_type = 0;
};

PendingMapChestOutboundClose inspectPendingMapChestOutboundClose(
        const void* packet) noexcept {
    constexpr uintptr_t kContainerCloseVtableRva = 0x128E4168ULL;
    constexpr size_t kContainerClosePacketBytes = 0x2FU;
    constexpr size_t kWindowIdOffset = 0x2CU;
    constexpr size_t kWindowTypeOffset = 0x2DU;
    constexpr size_t kServerInitiatedOffset = 0x2EU;
    PendingMapChestOutboundClose result;
    if (!packet || !build_import::HasPendingMapChestUiCloseWindow() ||
        !IsMemoryReadable(packet, kContainerClosePacketBytes)) return result;
    const uintptr_t base = Main::getBaseAddress();
    if (!base || base > UINTPTR_MAX - kContainerCloseVtableRva ||
        *static_cast<const uintptr_t*>(packet) !=
            base + kContainerCloseVtableRva) return result;
    const auto* bytes = static_cast<const uint8_t*>(packet);
    if (bytes[kServerInitiatedOffset] != 0U ||
        bytes[kWindowIdOffset] == 0U || bytes[kWindowIdOffset] == 0xFFU ||
        bytes[kWindowTypeOffset] != 0U) return result;
    result.matches = true;
    result.window_id = bytes[kWindowIdOffset];
    result.window_type = bytes[kWindowTypeOffset];
    return result;
}

static void captureLoopbackPacketSenderOnly(void* self, void* packet) {
    if (self != nullptr) {
        g_loopbackPacketSender.store(self, std::memory_order_release);
    }
    if (!build_import::BeforeMapNativeAnvilResultOutboundPacket(packet)) {
        return; // Only this thread's explicitly armed native result action.
    }
    if (packet && build_import::IsMapTextureDiagnosticsArmed()) {
        const std::string class_name = SafeGetClassName(packet);
        if (class_name.find("MapInfoRequestPacket") != std::string::npos) {
            build_import::RecordOutboundMapInfoRequestAttempt();
        }
    }
    build_import::ObserveMapManualOutboundPacket(packet);
    const PendingMapChestOutboundClose chest_close =
        inspectPendingMapChestOutboundClose(packet);
    const build_import::ProjectionPrinterSilentRotation::Ticket rotation_ticket =
        build_import::ProjectionPrinterSilentRotation::onBeforeOutgoingPacket(packet);
    const SendToServerFunction original =
        g_senderOnlyOriginalSendToServer.load(std::memory_order_acquire);
    if (original != nullptr) {
        original(self, packet);
        if (chest_close.matches) {
            build_import::ObserveMapChestOutboundClose(
                chest_close.window_id, chest_close.window_type);
        }
    }
    if (rotation_ticket != 0U) {
        build_import::ProjectionPrinterSilentRotation::onAfterOutgoingPacket(rotation_ticket);
    }
}

const build_import::CommandBlockPacketAbiProfile*
findLoopbackSenderCaptureProfile(uintptr_t base_addr, uintptr_t* target_address) {
    if (target_address) *target_address = 0;
    if (base_addr == 0) return nullptr;
    for (const build_import::CommandBlockPacketAbiProfile& profile :
         build_import::kCommandBlockPacketAbiProfiles) {
        uintptr_t constructor_address = 0;
        uintptr_t address = 0;
        if (!ResolveMinecraftExecutableOffset(base_addr, profile.constructor_rva, 4U,
                                              &constructor_address) ||
            !ResolveMinecraftExecutableOffset(base_addr, profile.sender_rva, 4U, &address) ||
            !IsMemoryReadable(reinterpret_cast<const void*>(constructor_address),
                              profile.constructor_prologue.size()) ||
            !IsMemoryReadable(reinterpret_cast<const void*>(address),
                              profile.sender_prologue.size()) ||
            std::memcmp(reinterpret_cast<const void*>(constructor_address),
                        profile.constructor_prologue.data(),
                        profile.constructor_prologue.size()) != 0 ||
            std::memcmp(reinterpret_cast<const void*>(address),
                        profile.sender_prologue.data(),
                        profile.sender_prologue.size()) != 0) {
            continue;
        }
        if (target_address) *target_address = address;
        return &profile;
    }
    return nullptr;
}

bool InitLoopbackPacketSenderCapture(uintptr_t baseAddr) {
    if (baseAddr == 0) {
        LOGE("Loopback sender capture cannot start: game address is unavailable");
        return false;
    }
    if (g_senderOnlyCaptureInstalled.load(std::memory_order_acquire)) return true;

    std::lock_guard<std::mutex> lock(g_senderOnlyCaptureInstallMutex);
    if (g_senderOnlyCaptureInstalled.load(std::memory_order_relaxed)) return true;

    uintptr_t targetAddr = 0;
    const build_import::CommandBlockPacketAbiProfile* const profile =
        findLoopbackSenderCaptureProfile(baseAddr, &targetAddr);
    if (!profile || targetAddr == 0) {
        LOGE("Loopback sender capture has no verified CommandBlockUpdatePacket ABI profile");
        return false;
    }
    std::array<uint8_t, 32> originalPrologue{};
    if (!IsMemoryReadable(reinterpret_cast<const void*>(targetAddr), originalPrologue.size())) {
        LOGE("Loopback sender capture is not readable yet; it will be retried");
        return false;
    }
    std::memcpy(originalPrologue.data(), reinterpret_cast<const void*>(targetAddr),
                originalPrologue.size());

    void* original = nullptr;
    const int result = DobbyHook(reinterpret_cast<void*>(targetAddr),
                                 reinterpret_cast<void*>(captureLoopbackPacketSenderOnly),
                                 &original);
    if (result != 0 || original == nullptr) {
        LOGE("Loopback sender capture hook installation failed, result=%d", result);
        return false;
    }

    g_senderOnlyOriginalSendToServer.store(
        reinterpret_cast<SendToServerFunction>(original), std::memory_order_release);
    g_loopbackSendToServerOriginalPrologue = originalPrologue;
    g_loopbackSendToServerOriginalPrologueCaptured.store(true, std::memory_order_release);
    g_senderOnlyCaptureInstalled.store(true, std::memory_order_release);
    LOGI("Building-tools LoopbackPacketSender capture hook installed (%s)", profile->name);
    return true;
}
