#include "ContainerClosePacketSender.h"

#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>

namespace build_import {
namespace {

constexpr uintptr_t kConstructorRva = 0x0A3D3EBCULL;
constexpr uintptr_t kVtableRva = 0x128E4168ULL;
constexpr uintptr_t kSenderRva = 0x0A785BB0ULL;
constexpr size_t kPacketSize = 0x38U;

constexpr size_t kContainerIdOffset = 0x2CU;
constexpr size_t kContainerTypeOffset = 0x2DU;
constexpr size_t kServerInitiatedCloseOffset = 0x2EU;
constexpr size_t kPayloadPaddingOffset = 0x2FU;

constexpr std::array<uint8_t, 32U> kConstructorPrologue{{
    0x68U, 0x22U, 0xFCU, 0xB0U, 0x1FU, 0x20U, 0x00U, 0x79U,
    0x00U, 0x1DU, 0x40U, 0xFDU, 0x88U, 0x28U, 0x04U, 0xB0U,
    0x08U, 0xA1U, 0x05U, 0x91U, 0x08U, 0x00U, 0x00U, 0xF9U,
    0xE8U, 0xFFU, 0x9EU, 0x52U, 0x08U, 0x58U, 0x00U, 0x79U,
}};

constexpr std::array<uint8_t, 32U> kSenderPrologue{{
    0xFDU, 0x7BU, 0xBBU, 0xA9U, 0xFCU, 0x0BU, 0x00U, 0xF9U,
    0xF8U, 0x5FU, 0x02U, 0xA9U, 0xF6U, 0x57U, 0x03U, 0xA9U,
    0xF4U, 0x4FU, 0x04U, 0xA9U, 0xFDU, 0x03U, 0x00U, 0x91U,
    0xFFU, 0xC3U, 0x08U, 0xD1U, 0x55U, 0xD0U, 0x3BU, 0xD5U,
}};

using PacketConstructor = void (*)(void* packet);
using PacketDeletingDestructor = void (*)(void* packet);
using SendToServer = void (*)(void* sender, void* packet);

bool fail(std::string* error, const char* detail) {
    if (error) *error = detail;
    return false;
}

bool isExactMinecraftExecutableAddress(uintptr_t module_base, uintptr_t address) {
    if (!module_base || address < module_base) return false;
    const uintptr_t offset = address - module_base;
    uintptr_t resolved = 0;
    return ResolveMinecraftExecutableOffset(module_base, offset, 4U, &resolved) &&
           resolved == address;
}

template <size_t N>
bool matchesInstructionFingerprint(uintptr_t address,
                                   const std::array<uint8_t, N>& expected) {
    return address != 0 && std::memcmp(reinterpret_cast<const void*>(address),
                                       expected.data(), expected.size()) == 0;
}

class PacketLifetime {
public:
    PacketLifetime(void* packet, PacketDeletingDestructor destroy)
        : packet_(packet), destroy_(destroy) {}

    ~PacketLifetime() {
        if (packet_) destroy_(packet_);
    }

    PacketLifetime(const PacketLifetime&) = delete;
    PacketLifetime& operator=(const PacketLifetime&) = delete;

private:
    void* packet_ = nullptr;
    PacketDeletingDestructor destroy_ = nullptr;
};

}  // namespace

bool ContainerClosePacketSender::send(uint8_t container_id,
                                      uint8_t container_type,
                                      std::string* error) {
#if !defined(__aarch64__)
    (void)container_id;
    (void)container_type;
    return fail(error, "container-close packet sending is supported only on arm64-v8a");
#else
    const uintptr_t module_base = Main::getBaseAddress();
    if (!module_base) return fail(error, "libminecraftpe.so is not loaded");

    uintptr_t constructor_address = 0;
    uintptr_t sender_address = 0;
    if (!ResolveMinecraftExecutableOffset(module_base, kConstructorRva,
                                          kConstructorPrologue.size(),
                                          &constructor_address) ||
        !ResolveMinecraftExecutableOffset(module_base, kSenderRva,
                                          kSenderPrologue.size(),
                                          &sender_address) ||
        !matchesInstructionFingerprint(constructor_address,
                                       kConstructorPrologue)) {
        return fail(error, "ContainerClosePacket ABI profile does not match this game version");
    }
    if (module_base > std::numeric_limits<uintptr_t>::max() - kVtableRva) {
        return fail(error, "invalid libminecraftpe.so base address");
    }
    const bool sender_fingerprint_matches =
        matchesInstructionFingerprint(sender_address, kSenderPrologue) ||
        CapturedLoopbackPacketSenderOriginalPrologueMatches(
            kSenderPrologue.data(), kSenderPrologue.size());
    if (!sender_fingerprint_matches) {
        return fail(error, "ContainerClosePacket sender ABI does not match this game version");
    }

    void* const sender_instance = GetCapturedLoopbackPacketSender();
    if (!sender_instance) {
        return fail(error, "LoopbackPacketSender is not ready; wait for a live game packet");
    }

    void* const packet = ::operator new(kPacketSize, std::nothrow);
    if (!packet) return fail(error, "unable to allocate ContainerClosePacket");

    reinterpret_cast<PacketConstructor>(constructor_address)(packet);

    const uintptr_t expected_vtable = module_base + kVtableRva;
    const uintptr_t actual_vtable = *reinterpret_cast<const uintptr_t*>(packet);
    if (actual_vtable != expected_vtable) {
        ::operator delete(packet);
        return fail(error, "ContainerClosePacket vtable does not match this game version");
    }

    const auto* const vtable = reinterpret_cast<const uintptr_t*>(actual_vtable);
    const uintptr_t deleting_destructor_address = vtable[1];
    if (!isExactMinecraftExecutableAddress(module_base,
                                           deleting_destructor_address)) {
        ::operator delete(packet);
        return fail(error, "ContainerClosePacket deleting destructor is unavailable");
    }
    PacketLifetime lifetime(
        packet,
        reinterpret_cast<PacketDeletingDestructor>(deleting_destructor_address));

    auto* const bytes = static_cast<uint8_t*>(packet);
    bytes[kContainerIdOffset] = container_id;
    bytes[kContainerTypeOffset] = container_type;
    bytes[kServerInitiatedCloseOffset] = 0U;
    bytes[kPayloadPaddingOffset] = 0U;

    reinterpret_cast<SendToServer>(sender_address)(sender_instance, packet);
    return true;
#endif
}

}  // namespace build_import
