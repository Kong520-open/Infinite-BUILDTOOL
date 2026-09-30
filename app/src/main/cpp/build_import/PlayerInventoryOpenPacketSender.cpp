#include "PlayerInventoryOpenPacketSender.h"

#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include "../tp/MinecraftUpdateHook.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace build_import {
namespace {

// Verified against libminecraftpe build
// ccf9c31f3121a46e89d868dcedb239466b3ac726 (Bedrock 1.21.120 / protocol 859).
// The game itself uses this exact InteractPacket constructor for its normal
// OpenInventory path: action=6, LocalPlayer::getRuntimeID(), Vec3::ZERO.
constexpr uintptr_t kInteractPacketConstructorRva = 0x0B4BA2A0ULL;
constexpr uintptr_t kActorRuntimeIdRva = 0x0D8E2B18ULL;
constexpr uintptr_t kSendToServerRva = 0x0A785BB0ULL;
constexpr uintptr_t kInteractPacketVtableRva = 0x12950E18ULL;
constexpr uintptr_t kInteractPacketCompleteDestructorRva = 0x0771B7A8ULL;
constexpr uintptr_t kInteractPacketDeletingDestructorRva = 0x0B4F3AC4ULL;
constexpr uintptr_t kInteractPacketIdFunctionRva = 0x0B4BA304ULL;
constexpr size_t kInteractPacketSize = 0x48U;
constexpr uint8_t kInteractOpenInventoryAction = 6U;

constexpr std::array<uint8_t, 32U> kInteractPacketConstructorFingerprint{{
    0xFDU, 0x7BU, 0xBDU, 0xA9U, 0xF6U, 0x57U, 0x01U, 0xA9U,
    0xF4U, 0x4FU, 0x02U, 0xA9U, 0xFDU, 0x03U, 0x00U, 0x91U,
    0xF3U, 0x03U, 0x03U, 0xAAU, 0xF4U, 0x03U, 0x02U, 0xAAU,
    0xF5U, 0x03U, 0x01U, 0x2AU, 0xF6U, 0x03U, 0x00U, 0xAAU,
}};
constexpr std::array<uint8_t, 24U> kActorRuntimeIdFingerprint{{
    0xFDU, 0x7BU, 0xBFU, 0xA9U, 0xFDU, 0x03U, 0x00U, 0x91U,
    0x04U, 0x00U, 0x00U, 0x94U, 0x00U, 0x00U, 0x40U, 0xF9U,
    0xFDU, 0x7BU, 0xC1U, 0xA8U, 0xC0U, 0x03U, 0x5FU, 0xD6U,
}};
constexpr std::array<uint8_t, 32U> kSendToServerFingerprint{{
    0xFDU, 0x7BU, 0xBBU, 0xA9U, 0xFCU, 0x0BU, 0x00U, 0xF9U,
    0xF8U, 0x5FU, 0x02U, 0xA9U, 0xF6U, 0x57U, 0x03U, 0xA9U,
    0xF4U, 0x4FU, 0x04U, 0xA9U, 0xFDU, 0x03U, 0x00U, 0x91U,
    0xFFU, 0xC3U, 0x08U, 0xD1U, 0x55U, 0xD0U, 0x3BU, 0xD5U,
}};

using InteractPacketConstructor = void (*)(void* packet, uint8_t action,
                                           uint64_t actor_runtime_id,
                                           const Vec3* position);
using ActorRuntimeId = uint64_t (*)(void* actor);
using SendToServer = void (*)(void* sender, void* packet);

bool fail(std::string* error, const char* detail) {
    if (error) *error = detail;
    return false;
}

template <size_t N>
bool matchesInstructionFingerprint(uintptr_t address,
                                   const std::array<uint8_t, N>& expected) {
    return address != 0 && std::memcmp(reinterpret_cast<const void*>(address),
                                       expected.data(), expected.size()) == 0;
}

bool isExactMinecraftExecutableAddress(uintptr_t module_base, uintptr_t address) {
    if (!module_base || address < module_base) return false;
    uintptr_t resolved = 0;
    return ResolveMinecraftExecutableOffset(module_base, address - module_base, 4U, &resolved) &&
        resolved == address;
}

bool resolveAndCheck(uintptr_t module_base, uintptr_t rva, size_t size,
                     uintptr_t* address) {
    return address && ResolveMinecraftExecutableOffset(module_base, rva, size, address);
}

}  // namespace

bool PlayerInventoryOpenPacketSender::send(std::string* error) {
#if !defined(__aarch64__)
    return fail(error, "silent player-inventory open is supported only on arm64-v8a");
#else
    const uintptr_t module_base = Main::getBaseAddress();
    if (!module_base) return fail(error, "libminecraftpe.so is not loaded");

    uintptr_t constructor_address = 0;
    uintptr_t runtime_id_address = 0;
    uintptr_t sender_address = 0;
    if (!resolveAndCheck(module_base, kInteractPacketConstructorRva,
                         kInteractPacketConstructorFingerprint.size(), &constructor_address) ||
        !resolveAndCheck(module_base, kActorRuntimeIdRva,
                         kActorRuntimeIdFingerprint.size(), &runtime_id_address) ||
        !resolveAndCheck(module_base, kSendToServerRva,
                         kSendToServerFingerprint.size(), &sender_address) ||
        !matchesInstructionFingerprint(constructor_address,
                                       kInteractPacketConstructorFingerprint) ||
        !matchesInstructionFingerprint(runtime_id_address, kActorRuntimeIdFingerprint)) {
        return fail(error, "OpenInventory packet ABI profile does not match this game version");
    }
    const bool sender_fingerprint_matches =
        matchesInstructionFingerprint(sender_address, kSendToServerFingerprint) ||
        CapturedLoopbackPacketSenderOriginalPrologueMatches(
            kSendToServerFingerprint.data(), kSendToServerFingerprint.size());
    if (!sender_fingerprint_matches) {
        return fail(error, "OpenInventory packet sender ABI does not match this game version");
    }

    if (module_base > std::numeric_limits<uintptr_t>::max() -
                          kInteractPacketVtableRva) {
        return fail(error, "invalid libminecraftpe.so base address");
    }
    const uintptr_t expected_vtable = module_base + kInteractPacketVtableRva;
    const auto* const vtable = reinterpret_cast<const uintptr_t*>(expected_vtable);
    if (!IsMemoryReadable(vtable, 4U * sizeof(uintptr_t)) ||
        vtable[0] != module_base + kInteractPacketCompleteDestructorRva ||
        vtable[1] != module_base + kInteractPacketDeletingDestructorRva ||
        vtable[2] != module_base + kInteractPacketIdFunctionRva ||
        !isExactMinecraftExecutableAddress(module_base, vtable[0]) ||
        !isExactMinecraftExecutableAddress(module_base, vtable[1]) ||
        !isExactMinecraftExecutableAddress(module_base, vtable[2])) {
        return fail(error, "InteractPacket vtable ABI does not match this game version");
    }

    void* const sender_instance = GetCapturedLoopbackPacketSender();
    if (!sender_instance) {
        return fail(error, "LoopbackPacketSender is not ready; wait for a live game packet");
    }
    void* const player = GetLocalPlayerPointer();
    if (!player || !IsMemoryReadable(player, 0x18U)) {
        return fail(error, "the local player is unavailable");
    }
    const uint64_t runtime_id = reinterpret_cast<ActorRuntimeId>(runtime_id_address)(player);
    if (runtime_id == 0U) {
        return fail(error, "the local player runtime ID is not ready");
    }

    // The game's own OpenInventory path constructs this stack packet, invokes
    // LoopbackPacketSender synchronously, then returns.  InteractPacket's
    // complete destructor is a no-op in this verified profile, so never route
    // a stack allocation through its deleting destructor.
    alignas(8) std::array<uint8_t, kInteractPacketSize> packet_storage{};
    const Vec3 zero{};
    void* const packet = packet_storage.data();
    reinterpret_cast<InteractPacketConstructor>(constructor_address)(
        packet, kInteractOpenInventoryAction, runtime_id, &zero);
    if (*reinterpret_cast<const uintptr_t*>(packet) != expected_vtable) {
        return fail(error, "constructed InteractPacket vtable does not match this game version");
    }
    reinterpret_cast<SendToServer>(sender_address)(sender_instance, packet);
    if (error) error->clear();
    return true;
#endif
}

}  // namespace build_import
