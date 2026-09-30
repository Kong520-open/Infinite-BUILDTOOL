#include "CommandBlockPacketSender.h"

#include "CommandBlockPacketAbiProfile.h"

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

constexpr size_t kBlockPositionOffset = 0x2CU;
constexpr size_t kModeOffset = 0x38U;
constexpr size_t kRedstoneModeOffset = 0x3AU;
constexpr size_t kConditionalOffset = 0x3BU;
constexpr size_t kMinecartRuntimeIdOffset = 0x40U;
constexpr size_t kCommandOffset = 0x48U;
constexpr size_t kLastOutputOffset = 0x60U;
constexpr size_t kNameOffset = 0x78U;
constexpr size_t kFilteredNameOffset = 0x90U;
constexpr size_t kFilteredTextFromServerOffset = 0xA8U;
constexpr size_t kTickDelayOffset = 0xB0U;
constexpr size_t kOutputTrackedOffset = 0xB4U;
constexpr size_t kExecuteOnFirstTickOffset = 0xB5U;
constexpr size_t kIsBlockOffset = 0xB6U;

struct WireBlockPos {
    int32_t x;
    int32_t y;
    int32_t z;
};

static_assert(sizeof(WireBlockPos) == 12U, "unexpected BlockPos layout");
static_assert(kIsBlockOffset < 0xB8U,
              "CommandBlockUpdatePacket layout exceeds allocation");

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

const CommandBlockPacketAbiProfile* findPacketProfile(uintptr_t module_base,
                                                       uintptr_t* constructor_address,
                                                       uintptr_t* sender_address) {
    if (constructor_address) *constructor_address = 0;
    if (sender_address) *sender_address = 0;
    for (const CommandBlockPacketAbiProfile& profile : kCommandBlockPacketAbiProfiles) {
        uintptr_t constructor = 0;
        uintptr_t sender = 0;
        if (!ResolveMinecraftExecutableOffset(module_base, profile.constructor_rva, 4U,
                                              &constructor) ||
            !ResolveMinecraftExecutableOffset(module_base, profile.sender_rva, 4U,
                                              &sender) ||
            !matchesInstructionFingerprint(constructor, profile.constructor_prologue)) {
            continue;
        }
        if (constructor_address) *constructor_address = constructor;
        if (sender_address) *sender_address = sender;
        return &profile;
    }
    return nullptr;
}

template <typename T>
T& packetField(void* packet, size_t offset) {
    auto* bytes = static_cast<uint8_t*>(packet);
    return *reinterpret_cast<T*>(bytes + offset);
}

// The command-block default constructor owns all embedded std::string objects.
// Once the verified deleting destructor is available it is responsible for both
// destructing those strings and freeing the allocation made with operator new.
class PacketLifetime {
public:
    PacketLifetime(void* packet, PacketDeletingDestructor destroy)
        : packet_(packet), destroy_(destroy) {}

    ~PacketLifetime() {
        if (packet_) {
            destroy_(packet_);
        }
    }

    PacketLifetime(const PacketLifetime&) = delete;
    PacketLifetime& operator=(const PacketLifetime&) = delete;

private:
    void* packet_ = nullptr;
    PacketDeletingDestructor destroy_ = nullptr;
};

}  // namespace

bool CommandBlockPacketSender::send(const CommandBlockRecord& record, std::string* error) {
#if !defined(__aarch64__)
    (void)record;
    return fail(error, "command-block packet sending is supported only on arm64-v8a");
#else
    // The current command-block packet embeds libc++'s 24-byte arm64 string
    // layout at four fixed offsets.  Compiling this class for an ABI with a
    // different string layout would silently corrupt the game-owned object.
    static_assert(sizeof(std::string) == 0x18U,
                  "unexpected arm64 std::string layout for CommandBlockUpdatePacket");

    if (!isValidCommandBlockMode(record.mode)) {
        return fail(error, "command-block mode must be 0 (impulse), 1 (repeat), or 2 (chain)");
    }

    const uintptr_t module_base = Main::getBaseAddress();
    if (!module_base) {
        return fail(error, "libminecraftpe.so is not loaded");
    }
    uintptr_t constructor_address = 0;
    uintptr_t sender_address = 0;
    const CommandBlockPacketAbiProfile* const profile =
        findPacketProfile(module_base, &constructor_address, &sender_address);
    if (!profile) {
        return fail(error, "CommandBlockUpdatePacket ABI profile does not match this game version");
    }
    if (module_base > std::numeric_limits<uintptr_t>::max() - profile->vtable_rva) {
        return fail(error, "invalid libminecraftpe.so base address");
    }
    const bool sender_fingerprint_matches =
        matchesInstructionFingerprint(sender_address, profile->sender_prologue) ||
        CapturedLoopbackPacketSenderOriginalPrologueMatches(
            profile->sender_prologue.data(), profile->sender_prologue.size());
    if (!sender_fingerprint_matches) {
        return fail(error, "CommandBlockUpdatePacket ABI fingerprint does not match this game version");
    }

    void* const sender_instance = GetCapturedLoopbackPacketSender();
    if (!sender_instance) {
        return fail(error, "LoopbackPacketSender is not ready; wait for a live game packet");
    }

    void* const packet = ::operator new(profile->packet_size, std::nothrow);
    if (!packet) return fail(error, "unable to allocate CommandBlockUpdatePacket");

    // Calling the real constructor is mandatory.  It installs the packet's
    // exact vtable and initializes the four embedded game-lib std::strings.
    reinterpret_cast<PacketConstructor>(constructor_address)(packet);

    const uintptr_t expected_vtable = module_base + profile->vtable_rva;
    const uintptr_t actual_vtable = *reinterpret_cast<const uintptr_t*>(packet);
    if (actual_vtable != expected_vtable) {
        // No dynamic strings were assigned before this check, so raw release is
        // safe even when the runtime is not the profile this implementation was
        // recovered from.
        ::operator delete(packet);
        return fail(error, "CommandBlockUpdatePacket vtable does not match this game version");
    }

    // In the Android Itanium C++ ABI, virtual slot 1 is the deleting
    // destructor.  Verify it belongs to executable memory in the same Minecraft
    // image before retaining it for RAII cleanup.
    const auto* vtable = reinterpret_cast<const uintptr_t*>(actual_vtable);
    const uintptr_t deleting_destructor_address = vtable[1];
    if (!isExactMinecraftExecutableAddress(module_base, deleting_destructor_address)) {
        ::operator delete(packet);
        return fail(error, "CommandBlockUpdatePacket deleting destructor is unavailable");
    }
    PacketLifetime lifetime(packet,
                            reinterpret_cast<PacketDeletingDestructor>(deleting_destructor_address));

    // The 0x3C..0x3F gap and FilteredText's trailing bytes are padding in the
    // recovered layout.  Clear them so no stale allocator data is exposed to a
    // packet serializer that copies the object or its FilteredText field.
    auto* const bytes = static_cast<uint8_t*>(packet);
    std::memset(bytes + 0x3CU, 0, 4U);
    std::memset(bytes + kFilteredTextFromServerOffset + 1U, 0, 7U);

    packetField<WireBlockPos>(packet, kBlockPositionOffset) = {record.x, record.y, record.z};
    packetField<uint16_t>(packet, kModeOffset) = record.mode;
    packetField<bool>(packet, kRedstoneModeOffset) = record.redstone_mode;
    packetField<bool>(packet, kConditionalOffset) = record.conditional;
    packetField<uint64_t>(packet, kMinecartRuntimeIdOffset) = 0U;
    packetField<std::string>(packet, kCommandOffset) = record.command;
    packetField<std::string>(packet, kLastOutputOffset) = record.last_output;
    packetField<std::string>(packet, kNameOffset) = record.name;
    packetField<std::string>(packet, kFilteredNameOffset) = record.filtered_name;
    packetField<bool>(packet, kFilteredTextFromServerOffset) = false;
    packetField<int32_t>(packet, kTickDelayOffset) = record.tick_delay;
    packetField<bool>(packet, kOutputTrackedOffset) = record.output_tracked;
    packetField<bool>(packet, kExecuteOnFirstTickOffset) = record.executing_on_first_tick;
    packetField<bool>(packet, kIsBlockOffset) = true;

    // sendToServer serializes the packet synchronously on the game thread.  It
    // must not be called from a parser/worker thread, and PacketLifetime tears
    // the object down immediately after this call returns.
    reinterpret_cast<SendToServer>(sender_address)(sender_instance, packet);
    return true;
#endif
}

}  // namespace build_import
