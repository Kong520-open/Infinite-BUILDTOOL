#include "ContainerOpenPacketSender.h"
#include "ProjectionPrinterInventoryMover.h"

#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include "../tp/MinecraftUpdateHook.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>

namespace build_import {
namespace {

constexpr uintptr_t kTransactionFactoryRva = 0x0E372F78ULL;
constexpr uintptr_t kPacketConstructorRva = 0x0B4BF958ULL;
constexpr uintptr_t kPacketVtableRva = 0x12950FF8ULL;
constexpr uintptr_t kPacketCompleteDestructorRva = 0x0B4BFA40ULL;
constexpr uintptr_t kPacketDeletingDestructorRva = 0x0B4BFAD4ULL;
constexpr uintptr_t kSenderRva = 0x0A785BB0ULL;
constexpr uintptr_t kGetSelectedItemRva = 0x0DC399FCULL;
constexpr uintptr_t kGetInventoryRva = 0x0DC25658ULL;
constexpr uintptr_t kGetPackedSelectedStateRva = 0x0DC36E44ULL;
constexpr uintptr_t kGetPlayerPositionRva = 0x0D8DFF5CULL;
constexpr uintptr_t kSetTransactionItemRva = 0x092C109CULL;
constexpr uintptr_t kSetBlockRuntimeIdRva = 0x0E34DC90ULL;

constexpr size_t kPacketSize = 0x68U;
constexpr int32_t kItemUseTransactionType = 2;
constexpr int32_t kMinimumHotbarSlot = 0;
constexpr int32_t kMaximumHotbarSlot = 8;

constexpr size_t kActionTypeOffset = 0x50U;
constexpr size_t kTriggerTypeOffset = 0x54U;
constexpr size_t kBlockPositionOffset = 0x58U;
constexpr size_t kBlockFaceOffset = 0x68U;
constexpr size_t kHotbarSlotOffset = 0x6CU;
constexpr size_t kPlayerPositionOffset = 0xC0U;
constexpr size_t kClickPositionOffset = 0xCCU;
constexpr size_t kPredictedResultOffset = 0xD8U;
constexpr size_t kItemUseTransactionSize = 0xE0U;

constexpr std::array<uint8_t, 32U> kTransactionFactoryFingerprint{{
    0xFDU, 0x7BU, 0xBDU, 0xA9U, 0xF5U, 0x0BU, 0x00U, 0xF9U,
    0xF4U, 0x4FU, 0x02U, 0xA9U, 0xFDU, 0x03U, 0x00U, 0x91U,
    0xF4U, 0x03U, 0x08U, 0xAAU, 0x1FU, 0x08U, 0x00U, 0x71U,
    0xF3U, 0x03U, 0x1FU, 0xAAU, 0xACU, 0x02U, 0x00U, 0x54U,
}};
constexpr std::array<uint8_t, 32U> kPacketConstructorFingerprint{{
    0xFDU, 0x7BU, 0xBDU, 0xA9U, 0xF5U, 0x0BU, 0x00U, 0xF9U,
    0xF4U, 0x4FU, 0x02U, 0xA9U, 0xFDU, 0x03U, 0x00U, 0x91U,
    0xF3U, 0x03U, 0x02U, 0x2AU, 0xF4U, 0x03U, 0x01U, 0xAAU,
    0xF5U, 0x03U, 0x00U, 0xAAU, 0xE5U, 0xF2U, 0xC9U, 0x97U,
}};
constexpr std::array<uint8_t, 32U> kSenderFingerprint{{
    0xFDU, 0x7BU, 0xBBU, 0xA9U, 0xFCU, 0x0BU, 0x00U, 0xF9U,
    0xF8U, 0x5FU, 0x02U, 0xA9U, 0xF6U, 0x57U, 0x03U, 0xA9U,
    0xF4U, 0x4FU, 0x04U, 0xA9U, 0xFDU, 0x03U, 0x00U, 0x91U,
    0xFFU, 0xC3U, 0x08U, 0xD1U, 0x55U, 0xD0U, 0x3BU, 0xD5U,
}};
constexpr std::array<uint8_t, 32U> kGetSelectedItemFingerprint{{
    0x08U, 0x0CU, 0x46U, 0xF9U, 0x09U, 0x61U, 0x44U, 0x39U,
    0x89U, 0x00U, 0x00U, 0x34U, 0x40U, 0xBDU, 0x02U, 0xB0U,
    0x00U, 0x20U, 0x30U, 0x91U, 0xC0U, 0x03U, 0x5FU, 0xD6U,
    0x00U, 0x91U, 0x40U, 0xF9U, 0x01U, 0x11U, 0x40U, 0xB9U,
}};
constexpr std::array<uint8_t, 32U> kGetInventoryFingerprint{{
    0x00U, 0x0CU, 0x46U, 0xF9U, 0xC0U, 0x03U, 0x5FU, 0xD6U,
    0xFDU, 0x7BU, 0xBBU, 0xA9U, 0xFAU, 0x67U, 0x01U, 0xA9U,
    0xF8U, 0x5FU, 0x02U, 0xA9U, 0xF6U, 0x57U, 0x03U, 0xA9U,
    0xF4U, 0x4FU, 0x04U, 0xA9U, 0xFDU, 0x03U, 0x00U, 0x91U,
}};
constexpr std::array<uint8_t, 32U> kGetPackedSelectedStateFingerprint{{
    0x08U, 0x60U, 0x44U, 0x39U, 0x09U, 0x10U, 0x40U, 0xB9U,
    0x00U, 0x81U, 0x09U, 0xAAU, 0xC0U, 0x03U, 0x5FU, 0xD6U,
    0x29U, 0x04U, 0x40U, 0xF9U, 0xEAU, 0x5DU, 0x81U, 0x52U,
    0x2AU, 0xB7U, 0xA0U, 0x72U, 0x28U, 0x41U, 0x40U, 0xB9U,
}};
constexpr std::array<uint8_t, 32U> kGetPlayerPositionFingerprint{{
    0x00U, 0x44U, 0x41U, 0xF9U, 0xC0U, 0x03U, 0x5FU, 0xD6U,
    0x08U, 0x4CU, 0x41U, 0xF9U, 0x20U, 0x00U, 0x40U, 0xFDU,
    0x00U, 0x05U, 0x00U, 0xFDU, 0xC0U, 0x03U, 0x5FU, 0xD6U,
    0x00U, 0x4CU, 0x41U, 0xF9U, 0xC0U, 0x03U, 0x5FU, 0xD6U,
}};
constexpr std::array<uint8_t, 32U> kSetTransactionItemFingerprint{{
    0xFFU, 0x43U, 0x02U, 0xD1U, 0xFDU, 0x7BU, 0x06U, 0xA9U,
    0xF6U, 0x57U, 0x07U, 0xA9U, 0xF4U, 0x4FU, 0x08U, 0xA9U,
    0xFDU, 0x83U, 0x01U, 0x91U, 0x54U, 0xD0U, 0x3BU, 0xD5U,
    0xF3U, 0x03U, 0x00U, 0xAAU, 0xE0U, 0x23U, 0x00U, 0x91U,
}};
constexpr std::array<uint8_t, 32U> kSetBlockRuntimeIdFingerprint{{
    0xFDU, 0x7BU, 0xBEU, 0xA9U, 0xF3U, 0x0BU, 0x00U, 0xF9U,
    0xFDU, 0x03U, 0x00U, 0x91U, 0xF3U, 0x03U, 0x00U, 0xAAU,
    0xE0U, 0x03U, 0x01U, 0xAAU, 0x9DU, 0x5BU, 0x08U, 0x94U,
    0x08U, 0x00U, 0x40U, 0xB9U, 0xE0U, 0x03U, 0x13U, 0xAAU,
}};

struct BlockPos {
    int32_t x;
    int32_t y;
    int32_t z;
};

struct Vec3 {
    float x;
    float y;
    float z;
};

static_assert(sizeof(BlockPos) == 12U, "unexpected BlockPos ABI");
static_assert(sizeof(Vec3) == 12U, "unexpected Vec3 ABI");
static_assert(kPredictedResultOffset < kItemUseTransactionSize,
              "ItemUse transaction field exceeds native allocation");

using PacketConstructor = void (*)(void* packet, void** transaction_owner,
                                   bool client_side);
using PacketDeletingDestructor = void (*)(void* packet);
using SendToServer = void (*)(void* sender, void* packet);
using GetSelectedItem = const void* (*)(void* player);
using GetInventory = void* (*)(void* player);
using GetPackedSelectedState = uint64_t (*)(void* inventory);
using GetPlayerPosition = const Vec3* (*)(void* player);
using SetTransactionItem = void* (*)(void* transaction, const void* item_stack);
using SetBlockRuntimeId = void* (*)(void* transaction, const void* block);

#if defined(__aarch64__)
extern "C" void BuildToolCreateItemUseTransaction(
    uintptr_t factory, int32_t type, void** output);
#endif

struct ResolvedAbi {
    uintptr_t transaction_factory = 0;
    uintptr_t packet_constructor = 0;
    uintptr_t sender = 0;
    uintptr_t get_selected_item = 0;
    uintptr_t get_inventory = 0;
    uintptr_t get_packed_selected_state = 0;
    uintptr_t get_player_position = 0;
    uintptr_t set_transaction_item = 0;
    uintptr_t set_block_runtime_id = 0;
};

bool fail(std::string* error, const char* detail) {
    if (error) *error = detail;
    return false;
}

bool checkedAddress(uintptr_t base, uintptr_t offset, uintptr_t* output) {
    if (!output || !base || base > std::numeric_limits<uintptr_t>::max() - offset) {
        return false;
    }
    *output = base + offset;
    return true;
}

bool isExactMinecraftExecutableAddress(uintptr_t module_base, uintptr_t address) {
    if (!module_base || address < module_base) return false;
    const uintptr_t offset = address - module_base;
    uintptr_t resolved = 0;
    return ResolveMinecraftExecutableOffset(module_base, offset, 4U, &resolved) &&
           resolved == address;
}

template <size_t N>
bool resolveAndMatch(uintptr_t module_base, uintptr_t rva,
                     const std::array<uint8_t, N>& fingerprint,
                     uintptr_t* output) {
    uintptr_t address = 0;
    if (!ResolveMinecraftExecutableOffset(module_base, rva, fingerprint.size(),
                                          &address) ||
        std::memcmp(reinterpret_cast<const void*>(address), fingerprint.data(),
                    fingerprint.size()) != 0) {
        return false;
    }
    if (output) *output = address;
    return true;
}

bool resolveAbi(uintptr_t module_base, ResolvedAbi* output) {
    if (!output) return false;
    ResolvedAbi abi;
    if (!resolveAndMatch(module_base, kTransactionFactoryRva,
                         kTransactionFactoryFingerprint,
                         &abi.transaction_factory) ||
        !resolveAndMatch(module_base, kPacketConstructorRva,
                         kPacketConstructorFingerprint,
                         &abi.packet_constructor) ||
        !ResolveMinecraftExecutableOffset(module_base, kSenderRva,
                                          kSenderFingerprint.size(),
                                          &abi.sender) ||
        !resolveAndMatch(module_base, kGetSelectedItemRva,
                         kGetSelectedItemFingerprint,
                         &abi.get_selected_item) ||
        !resolveAndMatch(module_base, kGetInventoryRva,
                         kGetInventoryFingerprint,
                         &abi.get_inventory) ||
        !resolveAndMatch(module_base, kGetPackedSelectedStateRva,
                         kGetPackedSelectedStateFingerprint,
                         &abi.get_packed_selected_state) ||
        !resolveAndMatch(module_base, kGetPlayerPositionRva,
                         kGetPlayerPositionFingerprint,
                         &abi.get_player_position) ||
        !resolveAndMatch(module_base, kSetTransactionItemRva,
                         kSetTransactionItemFingerprint,
                         &abi.set_transaction_item) ||
        !resolveAndMatch(module_base, kSetBlockRuntimeIdRva,
                         kSetBlockRuntimeIdFingerprint,
                         &abi.set_block_runtime_id)) {
        return false;
    }
    if (std::memcmp(reinterpret_cast<const void*>(abi.sender),
                    kSenderFingerprint.data(), kSenderFingerprint.size()) != 0 &&
        !CapturedLoopbackPacketSenderOriginalPrologueMatches(
            kSenderFingerprint.data(), kSenderFingerprint.size())) {
        return false;
    }
    *output = abi;
    return true;
}

bool destroyPolymorphicObject(uintptr_t module_base, void* object) noexcept {
    if (!object || !IsMemoryReadable(object, sizeof(uintptr_t))) return false;
    uintptr_t vtable = 0;
    std::memcpy(&vtable, object, sizeof(vtable));
    if (!vtable || !IsMemoryReadable(reinterpret_cast<const void*>(vtable),
                                     2U * sizeof(uintptr_t))) {
        return false;
    }
    uintptr_t deleting_destructor = 0;
    std::memcpy(&deleting_destructor,
                reinterpret_cast<const void*>(vtable + sizeof(uintptr_t)),
                sizeof(deleting_destructor));
    if (!isExactMinecraftExecutableAddress(module_base, deleting_destructor)) {
        return false;
    }
    reinterpret_cast<PacketDeletingDestructor>(deleting_destructor)(object);
    return true;
}

class NativeObjectLifetime {
public:
    NativeObjectLifetime(uintptr_t module_base, void* object)
        : module_base_(module_base), object_(object) {}

    ~NativeObjectLifetime() {
        if (object_) destroyPolymorphicObject(module_base_, object_);
    }

    NativeObjectLifetime(const NativeObjectLifetime&) = delete;
    NativeObjectLifetime& operator=(const NativeObjectLifetime&) = delete;

    void release() noexcept { object_ = nullptr; }

private:
    uintptr_t module_base_ = 0;
    void* object_ = nullptr;
};

template <typename T>
void writeField(void* object, size_t offset, const T& value) {
    std::memcpy(static_cast<uint8_t*>(object) + offset, &value, sizeof(value));
}

Vec3 clickPositionForFace(int face) {
    switch (face) {
        case 0: return {0.5F, 0.0F, 0.5F};
        case 1: return {0.5F, 1.0F, 0.5F};
        case 2: return {0.5F, 0.5F, 0.0F};
        case 3: return {0.5F, 0.5F, 1.0F};
        case 4: return {0.0F, 0.5F, 0.5F};
        case 5: return {1.0F, 0.5F, 0.5F};
        default: return {0.5F, 0.5F, 0.5F};
    }
}

}  // namespace

static bool sendItemUse(bool click_air, int32_t x, int32_t y, int32_t z,
                           const void* native_block, int face,
                           std::string* error, int32_t expected_hotbar_slot,
                           bool has_expected_network_stack_id,
                           int32_t expected_network_stack_id,
                           const ItemUseClickPosition* click_position) {
#if !defined(__aarch64__)
    (void)click_air;
    (void)x;
    (void)y;
    (void)z;
    (void)native_block;
    (void)face;
    (void)expected_hotbar_slot;
    (void)has_expected_network_stack_id;
    (void)expected_network_stack_id;
    (void)click_position;
    return fail(error, "packet-only container opening is supported only on arm64-v8a");
#else
    if (!click_air && (face < 0 || face > 5)) {
        return fail(error, "container ClickBlock face must be in the range 0..5");
    }
    if (click_position &&
        (!std::isfinite(click_position->x) || !std::isfinite(click_position->y) ||
         !std::isfinite(click_position->z) || click_position->x < 0.0F ||
         click_position->x > 1.0F || click_position->y < 0.0F ||
         click_position->y > 1.0F || click_position->z < 0.0F ||
         click_position->z > 1.0F)) {
        return fail(error, "container ClickBlock hit position must be inside the support block");
    }
    if (!click_air &&
        (!native_block || !IsMemoryReadable(native_block, sizeof(uintptr_t)))) {
        return fail(error, "native container Block is unavailable");
    }

    const uintptr_t module_base = Main::getBaseAddress();
    if (!module_base) return fail(error, "libminecraftpe.so is not loaded");

    ResolvedAbi abi;
    if (!resolveAbi(module_base, &abi)) {
        return fail(error,
                    "packet-only container ABI profile does not match this game version");
    }
    const bool sender_fingerprint_matches =
        std::memcmp(reinterpret_cast<const void*>(abi.sender),
                    kSenderFingerprint.data(), kSenderFingerprint.size()) == 0 ||
        CapturedLoopbackPacketSenderOriginalPrologueMatches(
            kSenderFingerprint.data(), kSenderFingerprint.size());
    if (!sender_fingerprint_matches) {
        return fail(error, "container packet sender ABI does not match this game version");
    }

    uintptr_t expected_packet_vtable = 0;
    uintptr_t expected_complete_destructor = 0;
    uintptr_t expected_deleting_destructor = 0;
    if (!checkedAddress(module_base, kPacketVtableRva, &expected_packet_vtable) ||
        !checkedAddress(module_base, kPacketCompleteDestructorRva,
                        &expected_complete_destructor) ||
        !checkedAddress(module_base, kPacketDeletingDestructorRva,
                        &expected_deleting_destructor) ||
        !IsMemoryReadable(reinterpret_cast<const void*>(expected_packet_vtable),
                          2U * sizeof(uintptr_t)) ||
        !isExactMinecraftExecutableAddress(module_base,
                                           expected_complete_destructor) ||
        !isExactMinecraftExecutableAddress(module_base,
                                           expected_deleting_destructor)) {
        return fail(error, "InventoryTransactionPacket lifetime ABI is unavailable");
    }

    uintptr_t complete_destructor = 0;
    uintptr_t deleting_destructor = 0;
    std::memcpy(&complete_destructor,
                reinterpret_cast<const void*>(expected_packet_vtable),
                sizeof(complete_destructor));
    std::memcpy(&deleting_destructor,
                reinterpret_cast<const void*>(expected_packet_vtable +
                                               sizeof(uintptr_t)),
                sizeof(deleting_destructor));
    if (complete_destructor != expected_complete_destructor ||
        deleting_destructor != expected_deleting_destructor) {
        return fail(error, "InventoryTransactionPacket vtable does not match this game version");
    }

    void* const sender_instance = GetCapturedLoopbackPacketSender();
    if (!sender_instance) {
        return fail(error, "LoopbackPacketSender is not ready; wait for a live game packet");
    }
    void* const player = GetLocalPlayerPointer();
    if (!player) {
        return fail(error, "local player is unavailable; wait for a live game tick");
    }

    const void* const selected_item =
        reinterpret_cast<GetSelectedItem>(abi.get_selected_item)(player);
    void* const inventory = reinterpret_cast<GetInventory>(abi.get_inventory)(player);
    const Vec3* const live_player_position =
        reinterpret_cast<GetPlayerPosition>(abi.get_player_position)(player);
    if (!selected_item || !IsMemoryReadable(selected_item, sizeof(uintptr_t))) {
        return fail(error, "the local player's selected ItemStack is unavailable");
    }
    if (!inventory || !IsMemoryReadable(inventory, 0x11CU)) {
        return fail(error, "the local player's inventory is unavailable");
    }
    if (!live_player_position ||
        !IsMemoryReadable(live_player_position, sizeof(Vec3))) {
        return fail(error, "the local player's position is unavailable");
    }

    const uint64_t packed_selected_state =
        reinterpret_cast<GetPackedSelectedState>(
            abi.get_packed_selected_state)(inventory);
    const int32_t hotbar_slot =
        static_cast<int32_t>(packed_selected_state >> 32U);
    if (hotbar_slot < kMinimumHotbarSlot || hotbar_slot > kMaximumHotbarSlot) {
        return fail(error, "the selected hotbar slot is outside the range 0..8");
    }
    if (expected_hotbar_slot >= 0 && hotbar_slot != expected_hotbar_slot) {
        return fail(error, "the selected hotbar slot changed before ItemUse could be sent");
    }
    if (has_expected_network_stack_id &&
        !VerifyProjectionPrinterSelectedHotbarItem(hotbar_slot,
                                                   expected_network_stack_id,
                                                   error)) {
        return false;
    }
    const Vec3 player_position = *live_player_position;

    void* transaction = nullptr;
    try {
        BuildToolCreateItemUseTransaction(abi.transaction_factory,
                                          kItemUseTransactionType,
                                          &transaction);
    } catch (...) {
        return fail(error, "the engine could not allocate an ItemUse transaction");
    }
    if (!transaction ||
        !IsMemoryReadable(transaction, kItemUseTransactionSize)) {
        return fail(error, "the engine returned an invalid ItemUse transaction");
    }
    NativeObjectLifetime transaction_lifetime(module_base, transaction);

    if (reinterpret_cast<SetTransactionItem>(abi.set_transaction_item)(
            transaction, selected_item) != transaction) {
        return fail(error, "the selected ItemStack could not be copied into ItemUse");
    }

    const BlockPos block_position{x, y, z};
    const Vec3 default_click_position = click_air
        ? Vec3{0.0F, 0.0F, 0.0F}
        : clickPositionForFace(face);
    const Vec3 effective_click_position = click_position
        ? Vec3{click_position->x, click_position->y, click_position->z}
        : default_click_position;
    const int32_t action_type = click_air ? 1 : 0;
    const uint8_t trigger_type = 0U;
    const uint8_t block_face = click_air ? 255U : static_cast<uint8_t>(face);
    const uint8_t predicted_success = click_air ? 0U : 1U;
    writeField(transaction, kActionTypeOffset, action_type);
    writeField(transaction, kTriggerTypeOffset, trigger_type);
    writeField(transaction, kBlockPositionOffset, block_position);
    writeField(transaction, kBlockFaceOffset, block_face);
    writeField(transaction, kHotbarSlotOffset, hotbar_slot);
    writeField(transaction, kPlayerPositionOffset, player_position);
    writeField(transaction, kClickPositionOffset, effective_click_position);
    writeField(transaction, kPredictedResultOffset, predicted_success);
    if (!click_air && reinterpret_cast<SetBlockRuntimeId>(abi.set_block_runtime_id)(
            transaction, native_block) != transaction) {
        return fail(error, "the target block runtime ID could not be written into ClickBlock");
    }

    void* const packet = ::operator new(kPacketSize, std::nothrow);
    if (!packet) return fail(error, "unable to allocate InventoryTransactionPacket");

    void* transaction_owner = transaction;
    try {
        reinterpret_cast<PacketConstructor>(abi.packet_constructor)(
            packet, &transaction_owner, true);
    } catch (...) {
        ::operator delete(packet);
        return fail(error, "the engine could not construct InventoryTransactionPacket");
    }
    if (transaction_owner == nullptr) transaction_lifetime.release();

    uintptr_t actual_packet_vtable = 0;
    std::memcpy(&actual_packet_vtable, packet, sizeof(actual_packet_vtable));
    if (actual_packet_vtable != expected_packet_vtable) {
        if (transaction_owner != nullptr) {
            ::operator delete(packet);
        }
        return fail(error, "InventoryTransactionPacket constructor did not take ownership");
    }
    NativeObjectLifetime packet_lifetime(module_base, packet);
    if (transaction_owner != nullptr) {
        return fail(error, "InventoryTransactionPacket did not take transaction ownership");
    }

    reinterpret_cast<SendToServer>(abi.sender)(sender_instance, packet);
    if (error) error->clear();
    return true;
#endif
}

bool ContainerOpenPacketSender::send(int32_t x, int32_t y, int32_t z,
                                       const void* native_block, int face,
                                       std::string* error,
                                       int32_t expected_hotbar_slot,
                                       bool has_expected_network_stack_id,
                                       int32_t expected_network_stack_id) {
    return sendItemUse(false, x, y, z, native_block, face, error,
                         expected_hotbar_slot, has_expected_network_stack_id,
                         expected_network_stack_id, nullptr);
}

bool ContainerOpenPacketSender::sendWithClick(
        int32_t x, int32_t y, int32_t z, const void* native_block, int face,
        const ItemUseClickPosition& click_position, std::string* error,
        int32_t expected_hotbar_slot, bool has_expected_network_stack_id,
        int32_t expected_network_stack_id) {
    return sendItemUse(false, x, y, z, native_block, face, error,
                       expected_hotbar_slot, has_expected_network_stack_id,
                       expected_network_stack_id, &click_position);
}

bool ContainerOpenPacketSender::useSelectedItem(
    std::string* error, int32_t expected_hotbar_slot,
    bool has_expected_network_stack_id, int32_t expected_network_stack_id) {
    return sendItemUse(true, 0, 0, 0, nullptr, 255, error, expected_hotbar_slot,
                       has_expected_network_stack_id, expected_network_stack_id, nullptr);
}

}  // namespace build_import
