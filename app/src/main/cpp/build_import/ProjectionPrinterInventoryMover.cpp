#include "ProjectionPrinterInventoryMover.h"
#include "ProjectionPrinterInventoryClientSync.h"
#include "ProjectionPrinterInventoryDiagnostics.h"
#include "ItemRuntimeRegistry.h"
#include "MapAnvilRenameSender.h"
#include "MapAnvilClientSyncDelivery.h"
#include "MapChestClientSyncDelivery.h"
#include "MapChestStorageSender.h"
#include "NativeWorldAccess.h"
#include "ProjectionPrinterInventoryMailbox.h"

#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include "../tp/MinecraftUpdateHook.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <elf.h>
#include <link.h>
#include <limits>
#include <string>
#include <utility>

namespace build_import {
namespace {

// This profile is deliberately bound to the verified libminecraftpe build
// ccf9c31f3121a46e89d868dcedb239466b3ac726.  Every callable address below
// additionally has an executable-segment and prologue fingerprint gate; a
// different client build therefore fails closed instead of emitting an
// ItemStackRequest with guessed native layouts.
constexpr uintptr_t kGetSelectedItemRva = 0x0DC399FCULL;
constexpr uintptr_t kGetInventoryRva = 0x0DC25658ULL;
constexpr uintptr_t kGetPackedSelectedStateRva = 0x0DC36E44ULL;
constexpr uintptr_t kDefaultNetIdVariantRva = 0x0DACE134ULL;
constexpr uintptr_t kNetIdPredicateRva = 0x0DACE690ULL;
constexpr uintptr_t kPlaceActionConstructorRva = 0x0DAA2390ULL;
constexpr uintptr_t kSwapActionConstructorRva = 0x0DAA2468ULL;
constexpr uintptr_t kItemStackRequestVtableRva = 0x12AAA500ULL;
constexpr uintptr_t kPlaceActionVtableRva = 0x12AA8130ULL;
constexpr uintptr_t kSwapActionVtableRva = 0x12AA8178ULL;
constexpr uintptr_t kActionCompleteDestructorRva = 0x0DAAE378ULL;
constexpr uintptr_t kPlaceActionDeletingDestructorRva = 0x0DAAE168ULL;
constexpr uintptr_t kSwapActionDeletingDestructorRva = 0x0DAAE218ULL;

constexpr size_t kInventoryContainerReadyOffset = 0x118U;
constexpr size_t kInventoryContainerOffset = 0x120U;
constexpr size_t kContainerGetSlotVtableOffset = 0x40U;
constexpr size_t kItemStackNetVariantOffset = 0xE8U;
constexpr size_t kNetVariantNetworkIdOffset = 0x00U;
constexpr size_t kNetVariantTagOffset = 0x10U;
constexpr size_t kActionAllocationSize = 0x60U;
constexpr uint8_t kHotbarContainer = 29U;
constexpr uint8_t kInventoryContainer = 30U;

// The reference sorter owns a request packet directly. It does not register a
// client-screen prediction scope. Registering one here lets a later failed
// screen-finalization response retire its predictive containers and notify the
// HUD with stale slot data after our local InventorySlot refresh.
constexpr uintptr_t kRequestDataConstructorRva = 0x0DAA4018ULL;
constexpr uintptr_t kRequestDataDestructorRva = 0x0DAA3EB4ULL;
constexpr uintptr_t kRequestDataAddActionRva = 0x0DAA5A18ULL;
constexpr uintptr_t kRequestBatchConstructorRva = 0x0DAA31C4ULL;
constexpr uintptr_t kRequestBatchDestructorRva = 0x0DAA3134ULL;
constexpr uintptr_t kRequestBatchAddRequestRva = 0x0DAA3E60ULL;
constexpr uintptr_t kRequestPacketConstructorRva = 0x0B4C4A04ULL;
constexpr uintptr_t kRequestPacketDestructorRva = 0x0B4C4944ULL;
constexpr uintptr_t kRequestPacketDeletingDestructorRva = 0x0B4C498CULL;
constexpr uintptr_t kRequestPacketIdFunctionRva = 0x0B4C4A40ULL;
constexpr uintptr_t kRequestPacketVtableRva = 0x12951140ULL;
constexpr uintptr_t kGameAllocateRva = 0x1252FA00ULL;
constexpr uintptr_t kGameDeallocateRva = 0x1252FA20ULL;
constexpr uintptr_t kRequestCounterInstructionsRva = 0x0DAD342CULL;
constexpr uintptr_t kRequestCounterRva = 0x133D31F0ULL;
constexpr uintptr_t kRequestSendToServerRva = 0x0A785BB0ULL;
constexpr size_t kRequestDataSize = 0x48U;
constexpr size_t kRequestBatchSize = 0x18U;
constexpr size_t kRequestPacketSize = 0x38U;

constexpr std::array<uint8_t, 32U> kRequestDataConstructorFingerprint{{
    0xFD,0x7B,0xBE,0xA9,0xF3,0x0B,0x00,0xF9,0xFD,0x03,0x00,0x91,0xF3,0x03,0x00,0xAA,
    0x67,0xA3,0x00,0x94,0x08,0x00,0x80,0x12,0x7F,0x7E,0x01,0xA9,0x7F,0x12,0x00,0xF9,
}};
constexpr std::array<uint8_t, 32U> kRequestDataDestructorFingerprint{{
    0xFD,0x7B,0xBD,0xA9,0xF5,0x0B,0x00,0xF9,0xF4,0x4F,0x02,0xA9,0xFD,0x03,0x00,0x91,
    0x14,0x18,0x40,0xF9,0xF3,0x03,0x00,0xAA,0x54,0x02,0x00,0xB4,0x75,0x1E,0x40,0xF9,
}};
constexpr std::array<uint8_t, 32U> kActionCompleteDestructorFingerprint{{
    0xFF,0x03,0x01,0xD1,0xFD,0x7B,0x01,0xA9,0xF6,0x57,0x02,0xA9,0xF4,0x4F,0x03,0xA9,
    0xFD,0x43,0x00,0x91,0x54,0xD0,0x3B,0xD5,0xF3,0x03,0x00,0xAA,0xC9,0x7F,0x02,0xD0,
}};
constexpr std::array<uint8_t, 32U> kRequestDataAddActionFingerprint{{
    0xFD,0x7B,0xBE,0xA9,0xF3,0x0B,0x00,0xF9,0xFD,0x03,0x00,0x91,0xF3,0x03,0x00,0xAA,
    0x00,0x1C,0x40,0xF9,0x68,0x22,0x40,0xF9,0x1F,0x00,0x08,0xEB,0x02,0x01,0x00,0x54,
}};
constexpr std::array<uint8_t, 12U> kRequestBatchConstructorFingerprint{{
    0x1F,0x7C,0x00,0xA9,0x1F,0x08,0x00,0xF9,0xC0,0x03,0x5F,0xD6,
}};
constexpr std::array<uint8_t, 32U> kRequestBatchDestructorFingerprint{{
    0xFD,0x7B,0xBD,0xA9,0xF6,0x57,0x01,0xA9,0xF4,0x4F,0x02,0xA9,0xFD,0x03,0x00,0x91,
    0x15,0x00,0x40,0xF9,0x75,0x03,0x00,0xB4,0x16,0x04,0x40,0xF9,0xF3,0x03,0x00,0xAA,
}};
constexpr std::array<uint8_t, 32U> kRequestBatchAddRequestFingerprint{{
    0xFD,0x7B,0xBE,0xA9,0xF3,0x0B,0x00,0xF9,0xFD,0x03,0x00,0x91,0xF3,0x03,0x00,0xAA,
    0x00,0x04,0x40,0xF9,0x68,0x0A,0x40,0xF9,0x1F,0x00,0x08,0xEB,0x02,0x01,0x00,0x54,
}};
constexpr std::array<uint8_t, 32U> kRequestPacketConstructorFingerprint{{
    0xFD,0x7B,0xBE,0xA9,0xF4,0x4F,0x01,0xA9,0xFD,0x03,0x00,0x91,0xF3,0x03,0x01,0xAA,
    0xF4,0x03,0x00,0xAA,0xBC,0xDE,0xC9,0x97,0x68,0xA4,0x03,0xB0,0x08,0x01,0x05,0x91,
}};
constexpr std::array<uint8_t, 32U> kRequestPacketDestructorFingerprint{{
    0xFD,0x7B,0xBE,0xA9,0xF3,0x0B,0x00,0xF9,0xFD,0x03,0x00,0x91,0x13,0x18,0x40,0xF9,
    0x68,0xA4,0x03,0xB0,0x08,0x01,0x05,0x91,0x08,0x00,0x00,0xF9,0x1F,0x18,0x00,0xF9,
}};
constexpr std::array<uint8_t, 16U> kGameAllocateFingerprint{{
    0x70,0x44,0x00,0xB0,0x11,0x22,0x45,0xF9,0x10,0x02,0x29,0x91,0x20,0x02,0x1F,0xD6,
}};
constexpr std::array<uint8_t, 16U> kGameDeallocateFingerprint{{
    0x70,0x44,0x00,0xB0,0x11,0x2A,0x45,0xF9,0x10,0x42,0x29,0x91,0x20,0x02,0x1F,0xD6,
}};
constexpr std::array<uint8_t, 44U> kRequestCounterInstructionsFingerprint{{
    0x08,0xC8,0x02,0x90,0xAA,0x7E,0x02,0xF0,0x4A,0x01,0x14,0x91,0x09,0xF1,0x41,0xB9,
    0x00,0x09,0x80,0x52,0xEA,0x13,0x00,0xF9,0x3F,0x05,0x00,0x31,0x29,0xB1,0x9F,0x5A,
    0x29,0x09,0x00,0x51,0x09,0xF1,0x01,0xB9,0xE9,0x2B,0x00,0xB9,
}};
constexpr std::array<uint8_t, 32U> kRequestSendToServerFingerprint{{
    0xFD,0x7B,0xBB,0xA9,0xFC,0x0B,0x00,0xF9,0xF8,0x5F,0x02,0xA9,0xF6,0x57,0x03,0xA9,
    0xF4,0x4F,0x04,0xA9,0xFD,0x03,0x00,0x91,0xFF,0xC3,0x08,0xD1,0x55,0xD0,0x3B,0xD5,
}};

// Verified on the same ccf9c31f game image as the mover. The simple slot
// constructor takes (this, ContainerID, slot, ItemStack const&), copies the
// actual ItemStack into NetworkItemStackDescriptor and supplies the player
// container/storage context. Native complete destructors release only fields,
// never our stack-allocated object storage.
constexpr uintptr_t kClientSlotConstructorRva = 0x0B4BE504ULL;
constexpr uintptr_t kClientSlotWriteRva = 0x0B4BE6F8ULL;
constexpr uintptr_t kClientSlotCompleteDestructorRva = 0x092FE898ULL;
constexpr uintptr_t kClientSlotVtableRva = 0x12950F58ULL;
constexpr uintptr_t kBinaryStreamConstructorRva = 0x120B7D7CULL;
constexpr uintptr_t kBinaryStreamCompleteDestructorRva = 0x120C44A4ULL;
constexpr uintptr_t kBinaryStreamVtableRva = 0x12D61178ULL;
constexpr size_t kNativeClientSlotSize = 0xE8U;
constexpr size_t kNativeBinaryStreamSize = 0x48U;
constexpr size_t kNativeStreamStringOffset = 0x08U;
constexpr size_t kNativeStreamOutputPointerOffset = 0x40U;
constexpr size_t kMaximumNativeSlotPacketBytes = 10U * 1024U * 1024U;

// Native map-UUID reader seen at the game's own map comparison call site
// 0x0F03AA08/0x0F03AA10 in the same ccf9c31f game image. The first accessor
// loads ItemStackBase's CompoundTag pointer; the second reads TAG_Long
// "map_uuid" and returns -1 if it is absent. Both are read-only.
constexpr uintptr_t kItemStackUserDataRva = 0x0E2C41A8ULL;
constexpr uintptr_t kMapUuidFromCompoundRva = 0x0E2B2B10ULL;
// AnvilContainerManagerController's UI recomputation calls this getter with
// index 0 for its real input stack. The single-hook Frida boundary trace
// confirmed this call boundary without modifying the game's ItemStack.
constexpr uintptr_t kAnvilInputGetterRva = 0x0DB8F5C4ULL;
constexpr uintptr_t kAnvilManagerVtableRva = 0x127DE658ULL;
constexpr uintptr_t kAnvilPreviewItemOffset = 0x130ULL;
constexpr size_t kAnvilPreviewItemSize = 0xE8U;
constexpr uintptr_t kAnvilRecipeIdOffset = 0x218ULL;
static_assert(kAnvilPreviewItemSize == kItemStackNetVariantOffset,
              "anvil preview must end before the ItemStack network variant");
constexpr uintptr_t kAnvilRecipeAssignRva = 0x088F2960ULL;
constexpr uintptr_t kAnvilRecipeUseRva = 0x088F625CULL;
constexpr std::array<uint8_t, 32U> kAnvilRecipeAssignFingerprint{{
    0x60,0x62,0x08,0x91,0xE1,0xC3,0x12,0x91,0x83,0x82,0x54,0x95,0xE0,0xC3,0x12,0x91,
    0x78,0x82,0x54,0x95,0xA8,0xE2,0x41,0x39,0xA8,0x05,0x00,0x34,0xA8,0x06,0x40,0xF9,
}};
constexpr std::array<uint8_t, 32U> kAnvilRecipeUseFingerprint{{
    0x61,0x62,0x08,0x91,0xE2,0x03,0x1F,0x2A,0xF5,0x03,0x00,0xAA,0x25,0xC3,0x45,0x95,
    0xF5,0x0B,0x00,0xF9,0xE1,0x43,0x00,0x91,0xE0,0x03,0x14,0xAA,0x47,0xBE,0x46,0x95,
}};
constexpr uintptr_t kItemInstanceDescriptorCopyRva = 0x0E2B7A38ULL;
constexpr std::array<uint8_t, 32U> kItemInstanceDescriptorCopyFingerprint{{
    0xFF,0x03,0x02,0xD1,0xFD,0x7B,0x03,0xA9,0xFA,0x67,0x04,0xA9,0xF8,0x5F,0x05,0xA9,
    0xF6,0x57,0x06,0xA9,0xF4,0x4F,0x07,0xA9,0xFD,0xC3,0x00,0x91,0x57,0xD0,0x3B,0xD5,
}};
constexpr std::array<uint8_t, 32U> kAnvilInputGetterFingerprint{{
    0xFF,0x83,0x01,0xD1,0xFD,0x7B,0x04,0xA9,0xF4,0x4F,0x05,0xA9,0xFD,0x03,0x01,0x91,
    0x54,0xD0,0x3B,0xD5,0xF3,0x03,0x00,0xAA,0xE0,0x33,0x00,0x91,0x88,0x16,0x40,0xF9,
}};
constexpr std::array<uint8_t, 8U> kItemStackUserDataFingerprint{{
    0x00U, 0x08U, 0x40U, 0xF9U, 0xC0U, 0x03U, 0x5FU, 0xD6U,
}};
constexpr std::array<uint8_t, 32U> kMapUuidFromCompoundFingerprint{{
    0xFDU, 0x7BU, 0xBEU, 0xA9U, 0xF3U, 0x0BU, 0x00U, 0xF9U,
    0xFDU, 0x03U, 0x00U, 0x91U, 0xE0U, 0x01U, 0x00U, 0xB4U,
    0xA1U, 0x99U, 0xFAU, 0xF0U, 0x21U, 0x64U, 0x23U, 0x91U,
    0x02U, 0x01U, 0x80U, 0x52U, 0x83U, 0x00U, 0x80U, 0x52U,
}};

constexpr std::array<uint8_t, 32U> kClientSlotConstructorFingerprint{{
    0xFFU, 0x43U, 0x05U, 0xD1U, 0xFDU, 0x7BU, 0x11U, 0xA9U,
    0xFCU, 0x5FU, 0x12U, 0xA9U, 0xF6U, 0x57U, 0x13U, 0xA9U,
    0xF4U, 0x4FU, 0x14U, 0xA9U, 0xFDU, 0x43U, 0x04U, 0x91U,
    0x57U, 0xD0U, 0x3BU, 0xD5U, 0xF3U, 0x03U, 0x00U, 0xAAU,
}};
constexpr std::array<uint8_t, 32U> kClientSlotWriteFingerprint{{
    0xFFU, 0x43U, 0x01U, 0xD1U, 0xFDU, 0x7BU, 0x02U, 0xA9U,
    0xF5U, 0x1BU, 0x00U, 0xF9U, 0xF4U, 0x4FU, 0x04U, 0xA9U,
    0xFDU, 0x83U, 0x00U, 0x91U, 0xF3U, 0x03U, 0x01U, 0xAAU,
    0x41U, 0xEBU, 0x8FU, 0xD2U, 0x55U, 0xD0U, 0x3BU, 0xD5U,
}};
constexpr std::array<uint8_t, 32U> kClientSlotDestructorFingerprint{{
    0xFFU, 0x43U, 0x01U, 0xD1U, 0xFDU, 0x7BU, 0x01U, 0xA9U,
    0xF7U, 0x13U, 0x00U, 0xF9U, 0xF6U, 0x57U, 0x03U, 0xA9U,
    0xF4U, 0x4FU, 0x04U, 0xA9U, 0xFDU, 0x43U, 0x00U, 0x91U,
    0x54U, 0xD0U, 0x3BU, 0xD5U, 0x49U, 0xD5U, 0x04U, 0xD0U,
}};
constexpr std::array<uint8_t, 32U> kBinaryStreamConstructorFingerprint{{
    0x48U, 0x65U, 0x00U, 0xD0U, 0x08U, 0xE1U, 0x05U, 0x91U,
    0xE9U, 0x03U, 0x00U, 0xAAU, 0x0AU, 0x24U, 0x00U, 0x91U,
    0x1FU, 0x7CU, 0x01U, 0xA9U, 0x3FU, 0x8DU, 0x00U, 0xF8U,
    0x1FU, 0xFCU, 0x02U, 0xA9U, 0x0AU, 0x10U, 0x00U, 0xF9U,
}};
constexpr std::array<uint8_t, 32U> kBinaryStreamDestructorFingerprint{{
    0x08U, 0x20U, 0x40U, 0x39U, 0xE9U, 0x64U, 0x00U, 0xB0U,
    0x29U, 0x41U, 0x05U, 0x91U, 0x09U, 0x00U, 0x00U, 0xF9U,
    0x48U, 0x00U, 0x00U, 0x37U, 0xC0U, 0x03U, 0x5FU, 0xD6U,
    0x00U, 0x0CU, 0x40U, 0xF9U, 0x58U, 0xADU, 0x11U, 0x14U,
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
constexpr std::array<uint8_t, 32U> kDefaultNetIdVariantFingerprint{{
    0x1FU, 0x00U, 0x00U, 0xB9U, 0x1FU, 0x10U, 0x00U, 0xB9U,
    0xC0U, 0x03U, 0x5FU, 0xD6U, 0xFFU, 0x03U, 0x01U, 0xD1U,
    0xFDU, 0x7BU, 0x01U, 0xA9U, 0xF5U, 0x13U, 0x00U, 0xF9U,
    0xF4U, 0x4FU, 0x03U, 0xA9U, 0xFDU, 0x43U, 0x00U, 0x91U,
}};
constexpr std::array<uint8_t, 32U> kNetIdPredicateFingerprint{{
    0x08U, 0x10U, 0x40U, 0xB9U, 0x09U, 0x00U, 0x40U, 0xB9U,
    0x1FU, 0x01U, 0x00U, 0x71U, 0x24U, 0x09U, 0x40U, 0x7AU,
    0xE0U, 0xD7U, 0x9FU, 0x1AU, 0xC0U, 0x03U, 0x5FU, 0xD6U,
    0x08U, 0x10U, 0x40U, 0xB9U, 0x1FU, 0x01U, 0x00U, 0x71U,
}};
constexpr std::array<uint8_t, 32U> kPlaceActionConstructorFingerprint{{
    0xFDU, 0x7BU, 0xBDU, 0xA9U, 0xF6U, 0x57U, 0x01U, 0xA9U,
    0xF4U, 0x4FU, 0x02U, 0xA9U, 0xFDU, 0x03U, 0x00U, 0x91U,
    0x28U, 0x20U, 0x80U, 0x52U, 0x01U, 0x2CU, 0x00U, 0x39U,
    0xF3U, 0x03U, 0x00U, 0xAAU, 0x08U, 0x10U, 0x00U, 0x79U,
}};
constexpr std::array<uint8_t, 32U> kSwapActionConstructorFingerprint{{
    0xFDU, 0x7BU, 0xBDU, 0xA9U, 0xF6U, 0x57U, 0x01U, 0xA9U,
    0xF4U, 0x4FU, 0x02U, 0xA9U, 0xFDU, 0x03U, 0x00U, 0x91U,
    0xE8U, 0x6BU, 0xFAU, 0xB0U, 0xF3U, 0x03U, 0x00U, 0xAAU,
    0xF4U, 0x03U, 0x00U, 0xAAU, 0x00U, 0x7DU, 0x40U, 0xFDU,
}};

struct alignas(8) NetIdVariant {
    std::array<uint8_t, 0x18U> bytes{};
};

struct FullContainerName {
    uint8_t container_name = 0;
    std::array<uint8_t, 3U> padding_before_dynamic{};
    uint32_t dynamic_container_id = 0;
    uint8_t has_dynamic_container_id = 0;
    std::array<uint8_t, 3U> padding_after_dynamic{};
};

struct alignas(8) SlotInfo {
    FullContainerName container;
    uint8_t slot = 0;
    std::array<uint8_t, 3U> padding{};
    NetIdVariant net_id_variant;
};

static_assert(sizeof(NetIdVariant) == 0x18U, "unexpected ItemStackNetIdVariant ABI");
static_assert(sizeof(FullContainerName) == 0x0CU, "unexpected FullContainerName ABI");
static_assert(offsetof(SlotInfo, slot) == 0x0CU, "unexpected SlotInfo slot ABI");
static_assert(offsetof(SlotInfo, net_id_variant) == 0x10U,
              "unexpected SlotInfo NetIdVariant ABI");
static_assert(sizeof(SlotInfo) == 0x28U, "unexpected SlotInfo ABI");

using GetSelectedItem = const void* (*)(void* player);
using GetInventory = void* (*)(void* player);
using GetPackedSelectedState = uint64_t (*)(void* inventory);
using ContainerGetSlot = const void* (*)(void* container, int32_t slot);
using DefaultNetIdVariant = void (*)(NetIdVariant* variant);
using PlaceActionConstructor = void (*)(void* action, uint8_t amount,
                                         const SlotInfo* from, const SlotInfo* to);
using SwapActionConstructor = void (*)(void* action, const SlotInfo* from,
                                        const SlotInfo* to);

struct ResolvedAbi {
    uintptr_t get_selected_item = 0;
    uintptr_t get_inventory = 0;
    uintptr_t get_packed_selected_state = 0;
    uintptr_t default_net_id_variant = 0;
    uintptr_t place_action_constructor = 0;
    uintptr_t swap_action_constructor = 0;
    uintptr_t item_stack_request_vtable = 0;
    uintptr_t place_action_vtable = 0;
    uintptr_t swap_action_vtable = 0;
};

bool fail(std::string* error, const char* detail) {
    static thread_local const char* previous_detail = nullptr;
    static thread_local std::chrono::steady_clock::time_point last_log;
    const auto now = std::chrono::steady_clock::now();
    if (previous_detail != detail || now - last_log >= std::chrono::seconds(5)) {
        LogProjectionPrinterInventoryDiagnostic("mover stopped before completion: %s", detail);
        previous_detail = detail;
        last_log = now;
    }
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

#if defined(__ANDROID__) && defined(__aarch64__)
constexpr uint8_t kVerifiedMinecraftBuildId[20] = {
    0xCC,0xF9,0xC3,0x1F,0x31,0x21,0xA4,0x6E,0x89,0xD8,
    0x68,0xDC,0xED,0xB2,0x39,0x46,0x6B,0x3A,0xC7,0x26,
};

size_t alignBuildNote(size_t value) noexcept {
    return (value + 3U) & ~size_t{3U};
}

struct MinecraftBuildIdQuery {
    uintptr_t base = 0;
    bool matched = false;
};

int findVerifiedMinecraftBuildId(dl_phdr_info* info, size_t, void* opaque) {
    auto* query = static_cast<MinecraftBuildIdQuery*>(opaque);
    if (!info || !query || static_cast<uintptr_t>(info->dlpi_addr) != query->base ||
        !info->dlpi_name) return 0;
    const char* leaf = std::strrchr(info->dlpi_name, '/');
    leaf = leaf ? leaf + 1 : info->dlpi_name;
    if (std::strcmp(leaf, "libminecraftpe.so") != 0) return 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const auto& segment = info->dlpi_phdr[i];
        if (segment.p_type != PT_NOTE || segment.p_memsz == 0U ||
            segment.p_memsz > 4096U ||
            query->base > std::numeric_limits<uintptr_t>::max() - segment.p_vaddr) {
            continue;
        }
        const auto* note = reinterpret_cast<const uint8_t*>(
            query->base + static_cast<uintptr_t>(segment.p_vaddr));
        const size_t size = static_cast<size_t>(segment.p_memsz);
        if (!IsMemoryReadable(note, size)) continue;
        size_t offset = 0;
        while (size - offset >= sizeof(Elf32_Nhdr)) {
            Elf32_Nhdr header{};
            std::memcpy(&header, note + offset, sizeof(header));
            offset += sizeof(header);
            const size_t name_size = alignBuildNote(header.n_namesz);
            const size_t desc_size = alignBuildNote(header.n_descsz);
            if (name_size > size - offset || desc_size > size - offset - name_size)
                break;
            const uint8_t* name = note + offset;
            const uint8_t* desc = note + offset + name_size;
            if (header.n_type == NT_GNU_BUILD_ID && header.n_namesz == 4U &&
                header.n_descsz == sizeof(kVerifiedMinecraftBuildId) &&
                std::memcmp(name, "GNU", 4U) == 0 &&
                std::memcmp(desc, kVerifiedMinecraftBuildId,
                            sizeof(kVerifiedMinecraftBuildId)) == 0) {
                query->matched = true;
                return 1;
            }
            offset += name_size + desc_size;
        }
    }
    return 1;
}

bool isVerifiedMinecraftBuild(uintptr_t base) {
    MinecraftBuildIdQuery query{base, false};
    dl_iterate_phdr(findVerifiedMinecraftBuildId, &query);
    return query.matched;
}
#endif

bool isExactMinecraftExecutableAddress(uintptr_t module_base, uintptr_t address) {
    if (!module_base || address < module_base) return false;
    const uintptr_t offset = address - module_base;
    uintptr_t resolved = 0;
    return ResolveMinecraftExecutableOffset(module_base, offset, 4U, &resolved) &&
        resolved == address;
}

bool vtableEntriesMatch(uintptr_t module_base, uintptr_t vtable,
                         uintptr_t complete_destructor_rva,
                         uintptr_t deleting_destructor_rva);

template <size_t N>
bool resolveAndMatch(uintptr_t module_base, uintptr_t rva,
                     const std::array<uint8_t, N>& fingerprint,
                     uintptr_t* output) {
    uintptr_t address = 0;
    const bool executable = ResolveMinecraftExecutableOffset(
        module_base, rva, fingerprint.size(), &address);
    if (!executable || std::memcmp(reinterpret_cast<const void*>(address),
                                  fingerprint.data(), fingerprint.size()) != 0) {
        static thread_local uintptr_t previous_rva = 0;
        static thread_local std::chrono::steady_clock::time_point last_log;
        const auto now = std::chrono::steady_clock::now();
        if (previous_rva != rva || now - last_log >= std::chrono::seconds(5)) {
            std::array<char, 129U> expected_hex{};
            std::array<char, 129U> actual_hex{};
            const bool readable = executable &&
                IsMemoryReadable(reinterpret_cast<const void*>(address), fingerprint.size());
            const size_t bytes = std::min(fingerprint.size(), size_t{64U});
            for (size_t index = 0; index < bytes; ++index) {
                std::snprintf(expected_hex.data() + index * 2U, 3U, "%02x",
                              static_cast<unsigned int>(fingerprint[index]));
                if (readable) {
                    std::snprintf(actual_hex.data() + index * 2U, 3U, "%02x",
                        static_cast<unsigned int>(reinterpret_cast<const uint8_t*>(address)[index]));
                }
            }
            LogProjectionPrinterInventoryDiagnostic(
                "ABI fingerprint rva=0x%llx executable=%d readable=%d expected=%s actual=%s",
                static_cast<unsigned long long>(rva), executable, readable,
                expected_hex.data(), readable ? actual_hex.data() : "unavailable");
            previous_rva = rva;
            last_log = now;
        }
        return false;
    }
    if (output) *output = address;
    return true;
}

bool resolveAbi(uintptr_t module_base, ResolvedAbi* output) {
    if (!output) return false;
    ResolvedAbi abi;
    uintptr_t ignored_net_id_predicate = 0;
    uintptr_t ignored_action_destructor = 0;
    if (!resolveAndMatch(module_base, kGetSelectedItemRva, kGetSelectedItemFingerprint,
                         &abi.get_selected_item) ||
        !resolveAndMatch(module_base, kGetInventoryRva, kGetInventoryFingerprint,
                         &abi.get_inventory) ||
        !resolveAndMatch(module_base, kGetPackedSelectedStateRva,
                         kGetPackedSelectedStateFingerprint,
                         &abi.get_packed_selected_state) ||
        !resolveAndMatch(module_base, kDefaultNetIdVariantRva,
                         kDefaultNetIdVariantFingerprint, &abi.default_net_id_variant) ||
        !resolveAndMatch(module_base, kNetIdPredicateRva, kNetIdPredicateFingerprint,
                          &ignored_net_id_predicate) ||
        !resolveAndMatch(module_base, kActionCompleteDestructorRva,
                         kActionCompleteDestructorFingerprint, &ignored_action_destructor) ||
        !resolveAndMatch(module_base, kPlaceActionConstructorRva,
                         kPlaceActionConstructorFingerprint,
                         &abi.place_action_constructor) ||
         !resolveAndMatch(module_base, kSwapActionConstructorRva,
                          kSwapActionConstructorFingerprint, &abi.swap_action_constructor) ||
         !checkedAddress(module_base, kItemStackRequestVtableRva,
                         &abi.item_stack_request_vtable) ||
        !checkedAddress(module_base, kPlaceActionVtableRva, &abi.place_action_vtable) ||
        !checkedAddress(module_base, kSwapActionVtableRva, &abi.swap_action_vtable) ||
         !IsMemoryReadable(reinterpret_cast<const void*>(abi.item_stack_request_vtable),
                           sizeof(uintptr_t)) ||
        !IsMemoryReadable(reinterpret_cast<const void*>(abi.place_action_vtable),
                          2U * sizeof(uintptr_t)) ||
        !IsMemoryReadable(reinterpret_cast<const void*>(abi.swap_action_vtable),
                          2U * sizeof(uintptr_t)) ||
        !vtableEntriesMatch(module_base, abi.place_action_vtable,
                            kActionCompleteDestructorRva,
                            kPlaceActionDeletingDestructorRva) ||
        !vtableEntriesMatch(module_base, abi.swap_action_vtable,
                            kActionCompleteDestructorRva,
                            kSwapActionDeletingDestructorRva)) {
        return false;
    }
    *output = abi;
    return true;
}

bool objectHasVtable(const void* object, uintptr_t expected_vtable) {
    if (!object || !expected_vtable || !IsMemoryReadable(object, sizeof(uintptr_t))) {
        return false;
    }
    uintptr_t actual_vtable = 0;
    std::memcpy(&actual_vtable, object, sizeof(actual_vtable));
    return actual_vtable == expected_vtable;
}

bool vtableEntriesMatch(uintptr_t module_base, uintptr_t vtable,
                         uintptr_t complete_destructor_rva,
                         uintptr_t deleting_destructor_rva) {
    uintptr_t expected_complete = 0;
    uintptr_t expected_deleting = 0;
    if (!checkedAddress(module_base, complete_destructor_rva, &expected_complete) ||
        !checkedAddress(module_base, deleting_destructor_rva, &expected_deleting) ||
        !isExactMinecraftExecutableAddress(module_base, expected_complete) ||
        !isExactMinecraftExecutableAddress(module_base, expected_deleting) ||
        !IsMemoryReadable(reinterpret_cast<const void*>(vtable),
                          2U * sizeof(uintptr_t))) {
        return false;
    }
    uintptr_t actual_complete = 0;
    uintptr_t actual_deleting = 0;
    std::memcpy(&actual_complete, reinterpret_cast<const void*>(vtable),
                sizeof(actual_complete));
    std::memcpy(&actual_deleting,
                reinterpret_cast<const void*>(vtable + sizeof(uintptr_t)),
                sizeof(actual_deleting));
    return actual_complete == expected_complete && actual_deleting == expected_deleting;
}

struct NativeRequestPacketAbi {
    uintptr_t data_constructor = 0;
    uintptr_t data_destructor = 0;
    uintptr_t data_add_action = 0;
    uintptr_t batch_constructor = 0;
    uintptr_t batch_destructor = 0;
    uintptr_t batch_add_request = 0;
    uintptr_t packet_constructor = 0;
    uintptr_t packet_destructor = 0;
    uintptr_t packet_vtable = 0;
    uintptr_t allocate = 0;
    uintptr_t deallocate = 0;
    uintptr_t counter = 0;
    uintptr_t send_to_server = 0;
};

struct WritableMinecraftImageQuery {
    uintptr_t base = 0;
    uintptr_t offset = 0;
    size_t size = 0;
    bool writable = false;
};

int findWritableMinecraftImageOffset(dl_phdr_info* info, size_t, void* context) {
    auto* query = static_cast<WritableMinecraftImageQuery*>(context);
    if (!info || !query || static_cast<uintptr_t>(info->dlpi_addr) != query->base)
        return 0;
    if (!info->dlpi_phdr) return 1;
    for (size_t index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr)& segment = info->dlpi_phdr[index];
        if (segment.p_type == PT_LOAD && (segment.p_flags & PF_W) != 0U &&
            query->offset >= segment.p_vaddr && query->size <= segment.p_memsz &&
            query->offset - segment.p_vaddr <= segment.p_memsz - query->size) {
            query->writable = true;
            break;
        }
    }
    return 1;
}

bool isWritableMinecraftImageOffset(uintptr_t base, uintptr_t offset, size_t size) {
    uintptr_t address = 0;
    if (!base || !size || !checkedAddress(base, offset, &address) ||
        size > std::numeric_limits<uintptr_t>::max() - address) return false;
    // Match ResolveMinecraftExecutableOffset: consult the loader's live
    // program headers, not a presumed ELF file header at the module base.
    // The counter lives in BSS, so p_memsz (not p_filesz) bounds are required.
    WritableMinecraftImageQuery query{base, offset, size, false};
    dl_iterate_phdr(findWritableMinecraftImageOffset, &query);
    return query.writable && IsMemoryReadable(reinterpret_cast<const void*>(address), size);
}

bool resolveRequestPacketAbi(uintptr_t base, NativeRequestPacketAbi* output,
                             std::string* error) {
    if (!output) return fail(error, "独立交换校验 R00：输出参数无效");
    NativeRequestPacketAbi abi;
    uintptr_t counter_instructions = 0;
    // Keep each failure short enough for the compact printer window. A
    // combined 'ABI mismatch' concealed which gate stopped the unsent request.
    if (!resolveAndMatch(base, kRequestDataConstructorRva,
                         kRequestDataConstructorFingerprint, &abi.data_constructor))
        return fail(error, "独立交换校验 R01：请求数据构造");
    if (!resolveAndMatch(base, kRequestDataDestructorRva,
                         kRequestDataDestructorFingerprint, &abi.data_destructor))
        return fail(error, "独立交换校验 R02：请求数据释放");
    if (!resolveAndMatch(base, kRequestDataAddActionRva,
                         kRequestDataAddActionFingerprint, &abi.data_add_action))
        return fail(error, "独立交换校验 R03：请求动作添加");
    if (!resolveAndMatch(base, kRequestBatchConstructorRva,
                         kRequestBatchConstructorFingerprint, &abi.batch_constructor))
        return fail(error, "独立交换校验 R04：请求批次构造");
    if (!resolveAndMatch(base, kRequestBatchDestructorRva,
                         kRequestBatchDestructorFingerprint, &abi.batch_destructor))
        return fail(error, "独立交换校验 R05：请求批次释放");
    if (!resolveAndMatch(base, kRequestBatchAddRequestRva,
                         kRequestBatchAddRequestFingerprint, &abi.batch_add_request))
        return fail(error, "独立交换校验 R06：请求批次添加");
    if (!resolveAndMatch(base, kRequestPacketConstructorRva,
                         kRequestPacketConstructorFingerprint, &abi.packet_constructor))
        return fail(error, "独立交换校验 R07：请求包构造");
    if (!resolveAndMatch(base, kRequestPacketDestructorRva,
                         kRequestPacketDestructorFingerprint, &abi.packet_destructor))
        return fail(error, "独立交换校验 R08：请求包释放");
    if (!resolveAndMatch(base, kGameAllocateRva, kGameAllocateFingerprint, &abi.allocate))
        return fail(error, "独立交换校验 R09：游戏内存分配");
    if (!resolveAndMatch(base, kGameDeallocateRva, kGameDeallocateFingerprint, &abi.deallocate))
        return fail(error, "独立交换校验 R10：游戏内存释放");
    if (!resolveAndMatch(base, kRequestCounterInstructionsRva,
                         kRequestCounterInstructionsFingerprint, &counter_instructions))
        return fail(error, "独立交换校验 R11：请求编号生成");
    if (!checkedAddress(base, kRequestCounterRva, &abi.counter) ||
        !isWritableMinecraftImageOffset(base, kRequestCounterRva, sizeof(int32_t)))
        return fail(error, "独立交换校验 R12：请求编号内存");
    if (!checkedAddress(base, kRequestPacketVtableRva, &abi.packet_vtable) ||
        !IsMemoryReadable(reinterpret_cast<const void*>(abi.packet_vtable),
                          3U * sizeof(uintptr_t)))
        return fail(error, "独立交换校验 R13：请求包虚表内存");
    if (!vtableEntriesMatch(base, abi.packet_vtable, kRequestPacketDestructorRva,
                            kRequestPacketDeletingDestructorRva))
        return fail(error, "独立交换校验 R14：请求包析构虚表");
    uintptr_t packet_id_function = 0;
    std::memcpy(&packet_id_function,
                reinterpret_cast<const void*>(abi.packet_vtable + 2U * sizeof(uintptr_t)),
                sizeof(packet_id_function));
    if (packet_id_function != base + kRequestPacketIdFunctionRva ||
        !isExactMinecraftExecutableAddress(base, packet_id_function))
        return fail(error, "独立交换校验 R15：请求包类型虚表");
    if (!ResolveMinecraftExecutableOffset(base, kRequestSendToServerRva,
                                           kRequestSendToServerFingerprint.size(),
                                           &abi.send_to_server) ||
        (std::memcmp(reinterpret_cast<const void*>(abi.send_to_server),
                     kRequestSendToServerFingerprint.data(),
                     kRequestSendToServerFingerprint.size()) != 0 &&
         !CapturedLoopbackPacketSenderOriginalPrologueMatches(
             kRequestSendToServerFingerprint.data(), kRequestSendToServerFingerprint.size()))) {
        return fail(error, "独立交换校验 R16：游戏发包入口");
    }
    *output = abi;
    return true;
}

// This owner is also the exact single-pointer unique_ptr argument accepted by
// the native vector/packet move constructors. They clear pointer_ on transfer,
// including their allocation-failure unwind path. The source owner therefore
// never deletes an object already owned by a native parent.
class NativeRequestObjectOwner {
public:
    NativeRequestObjectOwner(uintptr_t deallocate, uintptr_t complete_destructor)
        : deallocate_(deallocate), complete_destructor_(complete_destructor) {}
    ~NativeRequestObjectOwner() noexcept {
        if (!pointer_) return;
        if (constructed_) reinterpret_cast<void (*)(void*)>(complete_destructor_)(pointer_);
        reinterpret_cast<void (*)(void*)>(deallocate_)(pointer_);
    }
    NativeRequestObjectOwner(const NativeRequestObjectOwner&) = delete;
    NativeRequestObjectOwner& operator=(const NativeRequestObjectOwner&) = delete;
    void allocate(uintptr_t allocate, size_t size) {
        pointer_ = reinterpret_cast<void* (*)(size_t)>(allocate)(size);
    }
    void* get() const noexcept { return pointer_; }
    void** ownerAddress() noexcept { return &pointer_; }
    void markConstructed() noexcept { constructed_ = true; }
private:
    void* pointer_ = nullptr;
    uintptr_t deallocate_ = 0;
    uintptr_t complete_destructor_ = 0;
    bool constructed_ = false;
};

bool copyVariant(const void* item_stack, NetIdVariant* output) {
    if (!item_stack || !output ||
        !IsMemoryReadable(item_stack, kItemStackNetVariantOffset + sizeof(NetIdVariant))) {
        return false;
    }
    std::memcpy(output, static_cast<const uint8_t*>(item_stack) +
                             kItemStackNetVariantOffset,
                sizeof(*output));
    return true;
}

int32_t variantInt32(const NetIdVariant& variant, size_t offset) {
    int32_t value = 0;
    if (offset > variant.bytes.size() - sizeof(value)) return 0;
    std::memcpy(&value, variant.bytes.data() + offset, sizeof(value));
    return value;
}

bool isOrdinaryNetworkVariant(const NetIdVariant& variant,
                              int32_t expected_network_stack_id) {
    return expected_network_stack_id > 0 &&
        variantInt32(variant, kNetVariantTagOffset) == 0 &&
        variantInt32(variant, kNetVariantNetworkIdOffset) == expected_network_stack_id;
}

bool isDefaultEmptyNetworkVariant(const NetIdVariant& variant) {
    return variantInt32(variant, kNetVariantTagOffset) == 0 &&
        variantInt32(variant, kNetVariantNetworkIdOffset) == 0;
}

bool getPlayerInventoryContainer(const ResolvedAbi& abi, void* player,
                                 void** output_container, std::string* error) {
    if (!output_container || !player ||
        !IsMemoryReadable(player, 0x18U)) {
        return fail(error, "the local player inventory is unavailable");
    }
    void* const inventory = reinterpret_cast<GetInventory>(abi.get_inventory)(player);
    if (!inventory || !IsMemoryReadable(inventory, kInventoryContainerOffset + sizeof(void*))) {
        return fail(error, "the local player inventory component is unavailable");
    }
    uint8_t ready = 0;
    std::memcpy(&ready, static_cast<const uint8_t*>(inventory) +
                          kInventoryContainerReadyOffset, sizeof(ready));
    if (ready != 0U) {
        // In this build the selected-item accessor also treats every nonzero
        // value as a fallback ItemStack path rather than the live +0x120
        // player container.  Refuse it instead of guessing what it represents.
        return fail(error, "the live player inventory container is currently unavailable");
    }
    void* container = nullptr;
    std::memcpy(&container, static_cast<const uint8_t*>(inventory) +
                             kInventoryContainerOffset, sizeof(container));
    if (!container || !IsMemoryReadable(container,
                                        kContainerGetSlotVtableOffset + sizeof(uintptr_t))) {
        return fail(error, "the player inventory container is unavailable");
    }
    *output_container = container;
    return true;
}

bool getLiveInventoryStack(uintptr_t module_base, void* container, int32_t slot,
                           const void** output, std::string* error) {
    if (!output || !container || slot < 0 || slot >= 36) {
        return fail(error, "the requested player inventory slot is invalid");
    }
    uintptr_t vtable = 0;
    std::memcpy(&vtable, container, sizeof(vtable));
    if (!vtable || !IsMemoryReadable(reinterpret_cast<const void*>(
                      vtable + kContainerGetSlotVtableOffset), sizeof(uintptr_t))) {
        return fail(error, "the player inventory container vtable is unavailable");
    }
    uintptr_t get_slot = 0;
    std::memcpy(&get_slot, reinterpret_cast<const void*>(
                              vtable + kContainerGetSlotVtableOffset), sizeof(get_slot));
    if (!isExactMinecraftExecutableAddress(module_base, get_slot)) {
        return fail(error, "the player inventory slot getter does not match Minecraft");
    }
    const void* stack = reinterpret_cast<ContainerGetSlot>(get_slot)(container, slot);
    if (!stack || !IsMemoryReadable(stack,
                                    kItemStackNetVariantOffset + sizeof(NetIdVariant))) {
        return fail(error, "the live player ItemStack is unavailable");
    }
    *output = stack;
    return true;
}

bool makeSlotInfo(uint8_t container_name, uint8_t slot, const NetIdVariant& variant,
                  SlotInfo* output) {
    if (!output) return false;
    *output = {};
    output->container.container_name = container_name;
    output->container.dynamic_container_id = 0;
    output->container.has_dynamic_container_id = 0;
    output->slot = slot;
    output->net_id_variant = variant;
    return true;
}

bool loadAnvilInputEvidence(const MapAnvilInputPlaceRequest& request,
                            MapAnvilInputDraft* draft, std::string* error) {
    if (!draft || !request.pre_send) {
        return fail(error, "anvil input requires a durable journal callback");
    }
    const MapAnvilRenameTask& task = request.task;
    const uint64_t session_generation =
        GetProjectionPrinterInventoryMailboxSessionGeneration();
    if (!session_generation || session_generation != task.expected_session_generation) {
        return fail(error, "anvil input network session changed");
    }
    MapAnvilRenameWorldEvidence world;
    if (!QueryWorldContextOnGameThread(&world.world_context, 1000)) {
        return fail(error, "the live anvil world context is unavailable");
    }
    NativeBlockInfo block;
    if (!NativeWorldAccess::getBlock(task.anvil_x, task.anvil_y,
                                      task.anvil_z, &block)) {
        return fail(error, "the designated anvil block cannot be read");
    }
    world.block_identifier = std::move(block.name);

    ContainerCaptureResult capture;
    if (PollContainerCapture(task.window_token, &capture) !=
        ContainerCapturePollState::Ready) {
        return fail(error, "a fresh, captured anvil window is unavailable");
    }
    MapAnvilRenameWindowEvidence window;
    window.session_generation = session_generation;
    window.token = capture.token;
    window.window_id = capture.container_id;
    window.container_type = capture.container_type;
    window.x = capture.x;
    window.y = capture.y;
    window.z = capture.z;
    window.opened = capture.container_opened;
    window.closed = capture.container_closed;
    window.content_observed = true;
    window.slot_count = capture.slot_count;
    window.input_slot_empty = true;
    // This sender is for a newly opened empty anvil only. A nonempty initial
    // window is unsafe even if slot 1 happened to be vacant; later slot
    // updates cannot retroactively validate the first Place preflight.
    if (!capture.items.empty()) {
        return fail(error, "the captured anvil contains an existing item");
    }

    ProjectionPrinterNativeInventorySnapshot native;
    if (!ReadProjectionPrinterNativeInventorySnapshot(&native, error) ||
        !native.ready || task.source_hotbar_slot < 0 ||
        task.source_hotbar_slot > 8) {
        return fail(error, "the held map inventory snapshot is unavailable");
    }
    const auto& stack = native.slots[static_cast<size_t>(task.source_hotbar_slot)];
    MapAnvilHeldMapEvidence held;
    held.selected_hotbar_slot = native.selected_hotbar_slot;
    held.network_stack_id = stack.network_stack_id;
    held.runtime_item_id = stack.runtime_item_id;
    held.count = stack.count;
    if (stack.occupied && stack.has_network_stack_id &&
        ResolveItemRuntimeId(stack.runtime_item_id, &held.item_identifier) ==
            ItemRuntimeResolveStatus::Ready) {
        int64_t map_uuid = -1;
        if (ReadProjectionPrinterNativeMapUuid(task.source_hotbar_slot,
                                               stack.network_stack_id,
                                               &map_uuid, nullptr)) {
            held.has_map_uuid = true;
            held.map_uuid = map_uuid;
        }
    }
    if (!PrepareMapAnvilInputDraft(task, world, window, held, draft, error)) {
        return false;
    }
    return VerifyProjectionPrinterSelectedHotbarItem(
        task.source_hotbar_slot, task.source_network_stack_id, error);
}

struct NativeClientRefreshAbi {
    uintptr_t slot_constructor = 0;
    uintptr_t slot_write = 0;
    uintptr_t slot_destructor = 0;
    uintptr_t slot_vtable = 0;
    uintptr_t stream_constructor = 0;
    uintptr_t stream_destructor = 0;
    uintptr_t stream_vtable = 0;
};

bool resolveClientRefreshAbi(uintptr_t module_base, NativeClientRefreshAbi* output) {
    if (!output) return false;
    NativeClientRefreshAbi abi;
    if (!resolveAndMatch(module_base, kClientSlotConstructorRva,
                         kClientSlotConstructorFingerprint, &abi.slot_constructor) ||
        !resolveAndMatch(module_base, kClientSlotWriteRva,
                         kClientSlotWriteFingerprint, &abi.slot_write) ||
        !resolveAndMatch(module_base, kClientSlotCompleteDestructorRva,
                         kClientSlotDestructorFingerprint, &abi.slot_destructor) ||
        !resolveAndMatch(module_base, kBinaryStreamConstructorRva,
                         kBinaryStreamConstructorFingerprint, &abi.stream_constructor) ||
        !resolveAndMatch(module_base, kBinaryStreamCompleteDestructorRva,
                         kBinaryStreamDestructorFingerprint, &abi.stream_destructor) ||
        !checkedAddress(module_base, kClientSlotVtableRva, &abi.slot_vtable) ||
        !checkedAddress(module_base, kBinaryStreamVtableRva, &abi.stream_vtable)) return false;
    *output = abi;
    return true;
}

class NativeCompleteDestructorGuard {
public:
    NativeCompleteDestructorGuard(void* object, uintptr_t destructor)
        : object_(object), destructor_(destructor) {}
    ~NativeCompleteDestructorGuard() {
        reinterpret_cast<void (*)(void*)>(destructor_)(object_);
    }
    NativeCompleteDestructorGuard(const NativeCompleteDestructorGuard&) = delete;
    NativeCompleteDestructorGuard& operator=(const NativeCompleteDestructorGuard&) = delete;
private:
    void* object_;
    uintptr_t destructor_;
};

bool writeNativeClientSlot(const NativeClientRefreshAbi& abi, int32_t slot,
                            const void* stack, std::string* output,
                            std::string* error,
                            uint8_t inventory_id = 0U) {
    if (!output || !stack) return fail(error, "native slot refresh has no live ItemStack");
    alignas(8) std::array<uint8_t, kNativeClientSlotSize> packet{};
    alignas(8) std::array<uint8_t, kNativeBinaryStreamSize> stream{};
    reinterpret_cast<void (*)(void*, int8_t, uint32_t, const void*)>(
        abi.slot_constructor)(packet.data(), static_cast<int8_t>(inventory_id),
                              static_cast<uint32_t>(slot), stack);
    NativeCompleteDestructorGuard packet_guard(packet.data(), abi.slot_destructor);
    if (!objectHasVtable(packet.data(), abi.slot_vtable)) {
        return fail(error, "native InventorySlot constructor has an unexpected vtable");
    }
    reinterpret_cast<void (*)(void*)>(abi.stream_constructor)(stream.data());
    NativeCompleteDestructorGuard stream_guard(stream.data(), abi.stream_destructor);
    if (!objectHasVtable(stream.data(), abi.stream_vtable)) {
        return fail(error, "native BinaryStream constructor has an unexpected vtable");
    }
    reinterpret_cast<void (*)(void*, void*)>(abi.slot_write)(packet.data(), stream.data());

    // Read the verified libc++ string ABI as bytes, then COPY to module-owned
    // storage. Only the game's complete destructor frees its string. We never
    // cast the game string to our std::string or transfer its heap allocation.
    const uint8_t* native_string = nullptr;
    std::memcpy(&native_string, stream.data() + kNativeStreamOutputPointerOffset,
                sizeof(native_string));
    if (native_string != stream.data() + kNativeStreamStringOffset) {
        return fail(error, "native BinaryStream output pointer changed");
    }
    const bool long_string = (native_string[0] & 1U) != 0U;
    size_t length = static_cast<size_t>(native_string[0] >> 1U);
    const char* data = reinterpret_cast<const char*>(native_string + 1U);
    if (long_string) {
        std::memcpy(&length, native_string + 8U, sizeof(length));
        std::memcpy(&data, native_string + 16U, sizeof(data));
    } else if (length > 22U) {
        return fail(error, "native BinaryStream short string is invalid");
    }
    if (length == 0U || length >= kMaximumNativeSlotPacketBytes ||
        !data || !IsMemoryReadable(data, length)) {
        return fail(error, "native InventorySlot serialization is unavailable");
    }
    output->assign(1U, static_cast<char>(0x32U)); // Packet::write emits body only.
    output->append(data, length);
    return true;
}

}  // namespace

bool VerifyProjectionPrinterSelectedHotbarItem(
    int32_t expected_hotbar_slot, int32_t expected_network_stack_id,
    std::string* error) {
#if !defined(__aarch64__)
    (void)expected_hotbar_slot;
    (void)expected_network_stack_id;
    return fail(error, "projection-printer inventory verification is supported only on arm64-v8a");
#else
    if (expected_hotbar_slot < 0 || expected_hotbar_slot > 8 ||
        expected_network_stack_id <= 0) {
        return fail(error, "the expected selected printer ItemStack is invalid");
    }
    const uintptr_t module_base = Main::getBaseAddress();
    if (!module_base) return fail(error, "libminecraftpe.so is not loaded");
    ResolvedAbi abi;
    if (!resolveAbi(module_base, &abi)) {
        return fail(error, "projection-printer inventory ABI profile does not match this game version");
    }
    void* const player = GetLocalPlayerPointer();
    if (!player) return fail(error, "the local player is unavailable");
    void* const inventory = reinterpret_cast<GetInventory>(abi.get_inventory)(player);
    if (!inventory || !IsMemoryReadable(inventory, 0x11CU)) {
        return fail(error, "the local player inventory is unavailable");
    }
    const int32_t selected_slot = static_cast<int32_t>(
        reinterpret_cast<GetPackedSelectedState>(abi.get_packed_selected_state)(inventory) >> 32U);
    if (selected_slot != expected_hotbar_slot) {
        return fail(error, "the selected hotbar slot changed before ItemUse could be sent");
    }
    const void* const selected_item =
        reinterpret_cast<GetSelectedItem>(abi.get_selected_item)(player);
    NetIdVariant variant;
    if (!copyVariant(selected_item, &variant) ||
        !isOrdinaryNetworkVariant(variant, expected_network_stack_id)) {
        return fail(error, "the selected printer ItemStack changed before ItemUse could be sent");
    }
    if (error) error->clear();
    return true;
#endif
}

bool ReadProjectionPrinterLiveInventorySnapshot(
    ProjectionPrinterLiveInventorySnapshot* output, std::string* error) {
#if !defined(__aarch64__)
    if (output) *output = {};
    return fail(error, "projection-printer live inventory snapshots are supported only on arm64-v8a");
#else
    if (!output) return fail(error, "the live inventory snapshot output is unavailable");
    *output = {};

    const uintptr_t module_base = Main::getBaseAddress();
    if (!module_base) return fail(error, "libminecraftpe.so is not loaded");
    ResolvedAbi abi;
    if (!resolveAbi(module_base, &abi)) {
        return fail(error, "projection-printer inventory ABI profile does not match this game version");
    }
    void* const player = GetLocalPlayerPointer();
    if (!player ||
        !IsMemoryReadable(player, 0x18U)) {
        return fail(error, "the local player is unavailable");
    }
    void* container = nullptr;
    if (!getPlayerInventoryContainer(abi, player, &container, error)) return false;

    ProjectionPrinterLiveInventorySnapshot snapshot;
    for (size_t index = 0; index < snapshot.slots.size(); ++index) {
        const void* stack = nullptr;
        if (!getLiveInventoryStack(module_base, container, static_cast<int32_t>(index),
                                   &stack, error)) {
            return false;
        }
        NetIdVariant variant;
        if (!copyVariant(stack, &variant)) {
            return fail(error, "the live player ItemStack network identity is unavailable");
        }
        const int32_t tag = variantInt32(variant, kNetVariantTagOffset);
        const int32_t network_stack_id = variantInt32(variant, kNetVariantNetworkIdOffset);
        // The only two variants this printer can prove safe are the ordinary
        // positive stack ID and the engine's empty {0, 0} variant.  A future
        // variant layout must fail closed rather than being mistaken for an
        // empty hotbar slot.
        if (tag != 0 || network_stack_id < 0) {
            return fail(error, "the live player ItemStack uses an unsupported network identity");
        }
        ProjectionPrinterLiveInventorySlot& destination = snapshot.slots[index];
        destination.occupied = network_stack_id > 0;
        destination.has_network_stack_id = network_stack_id > 0;
        destination.network_stack_id = network_stack_id;
    }
    snapshot.ready = true;
    *output = std::move(snapshot);
    if (error) error->clear();
    return true;
#endif
}

bool ReadProjectionPrinterNativeInventorySnapshot(
    ProjectionPrinterNativeInventorySnapshot* output, std::string* error) {
#if !defined(__aarch64__)
    if (output) *output = {};
    return fail(error, "native inventory item snapshots are supported only on arm64-v8a");
#else
    if (!output) return fail(error, "the native inventory item snapshot output is unavailable");
    *output = {};
    try {
        const uintptr_t module_base = Main::getBaseAddress();
        if (!module_base) return fail(error, "libminecraftpe.so is not loaded");
        ResolvedAbi abi;
        NativeClientRefreshAbi refresh_abi;
        if (!resolveAbi(module_base, &abi) ||
            !resolveClientRefreshAbi(module_base, &refresh_abi)) {
            return fail(error, "native inventory item ABI does not match this game version");
        }
        void* const player = GetLocalPlayerPointer();
        if (!player || !IsMemoryReadable(player, 0x18U)) {
            return fail(error, "the local player is unavailable");
        }
        void* container = nullptr;
        if (!getPlayerInventoryContainer(abi, player, &container, error)) return false;
        void* const inventory = reinterpret_cast<GetInventory>(abi.get_inventory)(player);
        if (!inventory || !IsMemoryReadable(inventory, 0x11CU)) {
            return fail(error, "the local player inventory is unavailable");
        }
        const int32_t selected_slot = static_cast<int32_t>(
            reinterpret_cast<GetPackedSelectedState>(abi.get_packed_selected_state)(inventory) >>
            32U);
        if (selected_slot < 0 || selected_slot > 8) {
            return fail(error, "the selected hotbar slot is invalid");
        }

        ProjectionPrinterNativeInventorySnapshot snapshot;
        snapshot.selected_hotbar_slot = selected_slot;
        for (size_t index = 0; index < snapshot.slots.size(); ++index) {
            const void* stack = nullptr;
            if (!getLiveInventoryStack(module_base, container,
                                       static_cast<int32_t>(index), &stack, error)) {
                return false;
            }
            NetIdVariant variant;
            if (!copyVariant(stack, &variant)) {
                return fail(error, "the live ItemStack network identity is unavailable");
            }
            const int32_t tag = variantInt32(variant, kNetVariantTagOffset);
            const int32_t network_id = variantInt32(variant, kNetVariantNetworkIdOffset);
            if (tag != 0 || network_id < 0) {
                return fail(error, "the live ItemStack uses an unsupported network identity");
            }
            auto& destination = snapshot.slots[index];
            if (network_id == 0) continue;

            std::string packet;
            if (!writeNativeClientSlot(refresh_abi, static_cast<int32_t>(index),
                                       stack, &packet, error)) return false;
            ProjectionPrinterNativeSlotItem decoded;
            if (!DecodeProjectionPrinterNativeInventorySlotItem(
                    packet, static_cast<uint8_t>(index), &decoded) ||
                !decoded.occupied || decoded.runtime_item_id == 0 ||
                decoded.count == 0U || !decoded.has_network_stack_id ||
                decoded.network_stack_id != network_id) {
                return fail(error, "native ItemStack serialization changed during inventory read");
            }
            destination.occupied = true;
            destination.runtime_item_id = decoded.runtime_item_id;
            destination.count = decoded.count;
            destination.aux = decoded.aux;
            destination.has_network_stack_id = true;
            destination.network_stack_id = network_id;
        }
        snapshot.ready = true;
        *output = std::move(snapshot);
        if (error) error->clear();
        return true;
    } catch (...) {
        return fail(error, "native inventory items could not be serialized");
    }
#endif
}

bool ReadProjectionPrinterNativeSelectedHotbarSlotPacket(
    std::string* packet, int32_t* selected_slot, std::string* error) {
    if (packet) packet->clear();
    if (selected_slot) *selected_slot = -1;
#if !defined(__aarch64__)
    return fail(error, "native selected ItemStack reads are supported only on arm64-v8a");
#else
    if (!packet || !selected_slot) {
        return fail(error, "native selected ItemStack output is unavailable");
    }
    try {
        const uintptr_t module_base = Main::getBaseAddress();
        if (!module_base) return fail(error, "libminecraftpe.so is not loaded");
        ResolvedAbi abi;
        NativeClientRefreshAbi refresh_abi;
        if (!resolveAbi(module_base, &abi) ||
            !resolveClientRefreshAbi(module_base, &refresh_abi)) {
            return fail(error, "native selected ItemStack ABI does not match this game version");
        }
        void* const player = GetLocalPlayerPointer();
        if (!player || !IsMemoryReadable(player, 0x18U)) {
            return fail(error, "the local player is unavailable");
        }
        void* container = nullptr;
        if (!getPlayerInventoryContainer(abi, player, &container, error)) return false;
        void* const inventory = reinterpret_cast<GetInventory>(abi.get_inventory)(player);
        if (!inventory || !IsMemoryReadable(inventory, 0x11CU)) {
            return fail(error, "the local player inventory is unavailable");
        }
        const auto packed_selection = [&]() {
            return reinterpret_cast<GetPackedSelectedState>(
                abi.get_packed_selected_state)(inventory);
        };
        const int32_t slot = static_cast<int32_t>(packed_selection() >> 32U);
        if (slot < 0 || slot > 8) {
            return fail(error, "the selected hotbar slot is invalid");
        }
        const void* stack = nullptr;
        if (!getLiveInventoryStack(module_base, container, slot, &stack, error)) return false;
        NetIdVariant before;
        NetIdVariant selected_before;
        if (!copyVariant(stack, &before) ||
            !copyVariant(reinterpret_cast<GetSelectedItem>(abi.get_selected_item)(player),
                         &selected_before) ||
            variantInt32(before, kNetVariantTagOffset) != 0 ||
            variantInt32(before, kNetVariantNetworkIdOffset) <= 0 ||
            !isOrdinaryNetworkVariant(
                selected_before, variantInt32(before, kNetVariantNetworkIdOffset))) {
            return fail(error, "the selected live ItemStack identity is unavailable");
        }
        std::string captured;
        if (!writeNativeClientSlot(refresh_abi, slot, stack, &captured, error)) return false;
        ProjectionPrinterNativeSlotItem decoded;
        if (!DecodeProjectionPrinterNativeInventorySlotItem(
                captured, static_cast<uint8_t>(slot), &decoded) ||
            !decoded.occupied || decoded.runtime_item_id <= 0 || decoded.count == 0U ||
            !decoded.has_network_stack_id ||
            decoded.network_stack_id != variantInt32(before, kNetVariantNetworkIdOffset)) {
            return fail(error, "the selected native ItemStack serialized inconsistently");
        }
        const void* stack_after = nullptr;
        NetIdVariant after;
        NetIdVariant selected_after;
        if (static_cast<int32_t>(packed_selection() >> 32U) != slot ||
            !getLiveInventoryStack(module_base, container, slot, &stack_after, error) ||
            !copyVariant(stack_after, &after) ||
            !copyVariant(reinterpret_cast<GetSelectedItem>(abi.get_selected_item)(player),
                         &selected_after) ||
            !isOrdinaryNetworkVariant(after,
                variantInt32(before, kNetVariantNetworkIdOffset)) ||
            !isOrdinaryNetworkVariant(selected_after,
                variantInt32(before, kNetVariantNetworkIdOffset))) {
            return fail(error, "the selected live ItemStack changed during readback");
        }
        std::string confirmed;
        if (!writeNativeClientSlot(refresh_abi, slot, stack_after, &confirmed, error) ||
            confirmed != captured) {
            return fail(error, "the selected live ItemStack metadata changed during readback");
        }
        *packet = std::move(captured);
        *selected_slot = slot;
        if (error) error->clear();
        return true;
    } catch (...) {
        return fail(error, "the selected native ItemStack could not be serialized");
    }
#endif
}

bool ReadProjectionPrinterNativeMapUuid(
    int32_t inventory_slot, int32_t expected_network_stack_id,
    int64_t* map_uuid, std::string* error) {
#if !defined(__aarch64__)
    (void)inventory_slot;
    (void)expected_network_stack_id;
    if (map_uuid) *map_uuid = -1;
    return fail(error, "native map UUID reads are supported only on arm64-v8a");
#else
    if (map_uuid) *map_uuid = -1;
    if (!map_uuid || inventory_slot < 0 || inventory_slot >= 36 ||
        expected_network_stack_id <= 0) {
        return fail(error, "native map UUID request is invalid");
    }
    const uintptr_t module_base = Main::getBaseAddress();
    if (!module_base) return fail(error, "libminecraftpe.so is not loaded");
    ResolvedAbi abi;
    uintptr_t stack_user_data = 0;
    uintptr_t compound_map_uuid = 0;
    if (!resolveAbi(module_base, &abi) ||
        !resolveAndMatch(module_base, kItemStackUserDataRva,
                         kItemStackUserDataFingerprint, &stack_user_data) ||
        !resolveAndMatch(module_base, kMapUuidFromCompoundRva,
                         kMapUuidFromCompoundFingerprint, &compound_map_uuid)) {
        return fail(error, "native map UUID ABI does not match this game version");
    }
    void* const player = GetLocalPlayerPointer();
    void* container = nullptr;
    if (!getPlayerInventoryContainer(abi, player, &container, error)) return false;
    const void* stack = nullptr;
    if (!getLiveInventoryStack(module_base, container, inventory_slot,
                               &stack, error)) return false;
    NetIdVariant variant;
    if (!copyVariant(stack, &variant) ||
        !isOrdinaryNetworkVariant(variant, expected_network_stack_id)) {
        return fail(error, "the filled map ItemStack changed before reading map_uuid");
    }
    const void* compound = reinterpret_cast<const void* (*)(const void*)>(
        stack_user_data)(stack);
    if (!compound || !IsMemoryReadable(compound, sizeof(uintptr_t))) {
        return fail(error, "the filled map has no readable CompoundTag");
    }
    const int64_t value = reinterpret_cast<int64_t (*)(const void*)>(
        compound_map_uuid)(compound);
    if (value == -1) return fail(error, "the filled map has no map_uuid");
    *map_uuid = value;
    if (error) error->clear();
    return true;
#endif
}

bool CaptureMapAnvilLocalInputSlotPackets(
        int32_t source_hotbar_slot, int32_t expected_network_stack_id,
        int32_t expected_runtime_item_id, int64_t expected_map_uuid,
        uint8_t anvil_window_id, uint8_t anvil_inventory_slot,
        std::string* source_occupied_packet,
        std::string* anvil_occupied_packet, std::string* error) {
    if (source_occupied_packet) source_occupied_packet->clear();
    if (anvil_occupied_packet) anvil_occupied_packet->clear();
#if !defined(__ANDROID__) || !defined(__aarch64__)
    (void)source_hotbar_slot; (void)expected_network_stack_id;
    (void)expected_runtime_item_id; (void)expected_map_uuid;
    (void)anvil_window_id; (void)anvil_inventory_slot;
    return fail(error, "anvil local input capture requires Android arm64-v8a");
#else
    if (!source_occupied_packet || !anvil_occupied_packet ||
        !IsMinecraftUpdateGameThread() ||
        source_hotbar_slot < 0 || source_hotbar_slot > 8 ||
        expected_network_stack_id <= 0 || expected_runtime_item_id <= 0 ||
        expected_map_uuid == -1 || anvil_window_id == 0U ||
        anvil_window_id == 0xFFU || anvil_inventory_slot > 2U) {
        return fail(error, "anvil local input capture request is invalid");
    }
    try {
        ProjectionPrinterNativeInventorySnapshot native;
        if (!ReadProjectionPrinterNativeInventorySnapshot(&native, error) ||
            !native.ready || native.selected_hotbar_slot != source_hotbar_slot) {
            return fail(error, "anvil local input source is not the held stack");
        }
        const auto& item = native.slots[static_cast<size_t>(source_hotbar_slot)];
        if (!item.occupied || !item.has_network_stack_id ||
            item.network_stack_id != expected_network_stack_id ||
            item.runtime_item_id != expected_runtime_item_id ||
            item.count != 1U) {
            return fail(error, "anvil local input source map identity changed");
        }
        int64_t native_uuid = -1;
        if (!ReadProjectionPrinterNativeMapUuid(
                source_hotbar_slot, expected_network_stack_id,
                &native_uuid, error) || native_uuid != expected_map_uuid) {
            return fail(error, "anvil local input source map UUID changed");
        }
        const uintptr_t module_base = Main::getBaseAddress();
        ResolvedAbi abi;
        NativeClientRefreshAbi refresh_abi;
        if (!module_base || !isVerifiedMinecraftBuild(module_base) ||
            !resolveAbi(module_base, &abi) ||
            !resolveClientRefreshAbi(module_base, &refresh_abi)) {
            return fail(error, "anvil local input serializer ABI does not match");
        }
        void* player_container = nullptr;
        if (!getPlayerInventoryContainer(
                abi, GetLocalPlayerPointer(), &player_container, error)) return false;
        const void* stack = nullptr;
        NetIdVariant before;
        if (!getLiveInventoryStack(module_base, player_container,
                                   source_hotbar_slot, &stack, error) ||
            !copyVariant(stack, &before) ||
            !isOrdinaryNetworkVariant(before, expected_network_stack_id) ||
            !writeNativeClientSlot(refresh_abi, source_hotbar_slot, stack,
                                   source_occupied_packet, error) ||
            !writeNativeClientSlot(refresh_abi, anvil_inventory_slot, stack,
                                   anvil_occupied_packet, error,
                                   anvil_window_id)) {
            source_occupied_packet->clear();
            anvil_occupied_packet->clear();
            return fail(error, "anvil local input source serialization failed");
        }
        const void* stable_stack = nullptr;
        NetIdVariant after;
        if (!getLiveInventoryStack(module_base, player_container,
                                   source_hotbar_slot, &stable_stack, error) ||
            stable_stack != stack || !copyVariant(stable_stack, &after) ||
            !isOrdinaryNetworkVariant(after, expected_network_stack_id) ||
            before.bytes != after.bytes) {
            source_occupied_packet->clear();
            anvil_occupied_packet->clear();
            return fail(error, "anvil local input source changed during serialization");
        }
        ProjectionPrinterNativeSlotItem decoded;
        ItemExtraMapMetadata metadata;
        if (!DecodeProjectionPrinterNativeInventorySlotMapMetadata(
                *source_occupied_packet,
                static_cast<uint8_t>(source_hotbar_slot),
                &decoded, &metadata) || !decoded.occupied ||
            decoded.count != 1U ||
            decoded.runtime_item_id != expected_runtime_item_id ||
            !decoded.has_network_stack_id ||
            decoded.network_stack_id != expected_network_stack_id ||
            !metadata.has_map_uuid ||
            metadata.map_uuid != expected_map_uuid) {
            source_occupied_packet->clear();
            anvil_occupied_packet->clear();
            return fail(error, "anvil local input source packet is not the expected map");
        }
        if (error) error->clear();
        return true;
    } catch (...) {
        source_occupied_packet->clear();
        anvil_occupied_packet->clear();
        return fail(error, "anvil local input native capture failed");
    }
#endif
}

bool ReadProjectionPrinterNativeAnvilInputMap(
    uintptr_t verified_anvil_manager, int32_t expected_runtime_item_id,
    int64_t expected_map_uuid,
    ProjectionPrinterNativeAnvilInputMapSnapshot* output,
    std::string* error) {
    if (output) *output = {};
#if !defined(__ANDROID__) || !defined(__aarch64__)
    (void)verified_anvil_manager;
    (void)expected_runtime_item_id;
    (void)expected_map_uuid;
    return fail(error, "native anvil input readback requires Android arm64-v8a");
#else
    if (!output || !verified_anvil_manager || expected_runtime_item_id <= 0 ||
        expected_map_uuid == -1) {
        return fail(error, "native anvil input readback request is invalid");
    }
    try {
        const uintptr_t module_base = Main::getBaseAddress();
        if (!module_base || !isVerifiedMinecraftBuild(module_base)) {
            return fail(error, "native anvil input game Build ID does not match");
        }
        uintptr_t manager_vtable = 0;
        if (!checkedAddress(module_base, kAnvilManagerVtableRva, &manager_vtable) ||
            !objectHasVtable(reinterpret_cast<const void*>(verified_anvil_manager),
                             manager_vtable)) {
            return fail(error, "native anvil input manager is not live");
        }
        uintptr_t getter = 0, user_data = 0, map_uuid_from_compound = 0;
        NativeClientRefreshAbi refresh_abi;
        if (!resolveAndMatch(module_base, kAnvilInputGetterRva,
                             kAnvilInputGetterFingerprint, &getter) ||
            !resolveAndMatch(module_base, kItemStackUserDataRva,
                             kItemStackUserDataFingerprint, &user_data) ||
            !resolveAndMatch(module_base, kMapUuidFromCompoundRva,
                             kMapUuidFromCompoundFingerprint,
                             &map_uuid_from_compound) ||
            !resolveClientRefreshAbi(module_base, &refresh_abi)) {
            return fail(error, "native anvil input ItemStack ABI does not match");
        }
        const auto live_input = [&]() -> const void* {
            return reinterpret_cast<const void* (*)(void*, int32_t)>(getter)(
                reinterpret_cast<void*>(verified_anvil_manager), 0);
        };
        const auto native_uuid = [&](const void* stack, int64_t* uuid) -> bool {
            if (!uuid) return false;
            *uuid = -1;
            const void* compound = reinterpret_cast<const void* (*)(const void*)>(
                user_data)(stack);
            if (!compound || !IsMemoryReadable(compound, sizeof(uintptr_t)))
                return false;
            *uuid = reinterpret_cast<int64_t (*)(const void*)>(
                map_uuid_from_compound)(compound);
            return *uuid != -1;
        };

        // Slot 0 is the manager's input getter index. This synthetic
        // InventorySlot packet is never sent to the game or server; slot 0
        // lets the existing strict decoder parse the game's own ItemStack
        // serialization without guessing the C++ ItemStack layout.
        const void* first_stack = live_input();
        NetIdVariant first_variant;
        if (!copyVariant(first_stack, &first_variant)) {
            return fail(error, "native anvil input ItemStack is unavailable");
        }
        const int32_t first_id = variantInt32(first_variant, kNetVariantNetworkIdOffset);
        if (!isOrdinaryNetworkVariant(first_variant, first_id)) {
            uint8_t first_count = 0U;
            if (first_stack && IsMemoryReadable(
                    static_cast<const uint8_t*>(first_stack) + 0x78U,
                    sizeof(first_count))) {
                std::memcpy(&first_count,
                            static_cast<const uint8_t*>(first_stack) + 0x78U,
                            sizeof(first_count));
            }
            LogProjectionPrinterInventoryDiagnostic(
                "map_anvil input native_unready net=%d tag=%d count=%u empty_sentinel=%d",
                first_id,
                variantInt32(first_variant, kNetVariantTagOffset),
                static_cast<unsigned>(first_count),
                reinterpret_cast<uintptr_t>(first_stack) ==
                    module_base + 0x133E2C08ULL ? 1 : 0);
            // This build exposes an unsynchronized input ItemStack with
            // count=1 but a default net variant (tag=0, id=0). Count alone
            // cannot prove the accepted server Place reached the UI. Only
            // the journaled exact ACK may authorize the one-shot local sync.
            if (reinterpret_cast<uintptr_t>(first_stack) ==
                    module_base + 0x133E2C08ULL ||
                (first_id == 0 &&
                 variantInt32(first_variant, kNetVariantTagOffset) == 0)) {
                return fail(error, "native anvil input is not synchronized on client");
            }
            return fail(error, "native anvil input network identity is invalid");
        }
        std::string first_packet;
        if (!writeNativeClientSlot(refresh_abi, 0, first_stack,
                                   &first_packet, error)) return false;
        ProjectionPrinterNativeSlotItem item;
        ItemExtraMapMetadata metadata;
        if (!DecodeProjectionPrinterNativeInventorySlotMapMetadata(
                first_packet, 0, &item, &metadata) || !item.occupied ||
            item.count != 1U || item.runtime_item_id != expected_runtime_item_id ||
            !item.has_network_stack_id || item.network_stack_id != first_id ||
            !metadata.has_map_uuid || metadata.map_uuid != expected_map_uuid) {
            return fail(error, "native anvil input is not the expected one-map stack");
        }
        std::string identifier;
        if (ResolveItemRuntimeId(item.runtime_item_id, &identifier) !=
                ItemRuntimeResolveStatus::Ready ||
            (identifier != "minecraft:map" && identifier != "map" &&
             identifier != "minecraft:filled_map" &&
             identifier != "filled_map" &&
             identifier != "minecraft:locator_map" &&
             identifier != "locator_map")) {
            return fail(error, "native anvil input item is not a filled map");
        }
        int64_t direct_uuid = -1;
        if (!native_uuid(first_stack, &direct_uuid) ||
            direct_uuid != metadata.map_uuid) {
            return fail(error, "native anvil input NBT disagrees with serialization");
        }

        const void* second_stack = live_input();
        NetIdVariant second_variant;
        std::string second_packet;
        int64_t second_uuid = -1;
        if (second_stack != first_stack ||
            !objectHasVtable(reinterpret_cast<const void*>(verified_anvil_manager),
                             manager_vtable) ||
            !copyVariant(second_stack, &second_variant) ||
            !isOrdinaryNetworkVariant(second_variant, first_id) ||
            !writeNativeClientSlot(refresh_abi, 0, second_stack,
                                   &second_packet, error) ||
            second_packet != first_packet ||
            !native_uuid(second_stack, &second_uuid) ||
            second_uuid != expected_map_uuid) {
            return fail(error, "native anvil input changed during readback");
        }
        ProjectionPrinterNativeAnvilInputMapSnapshot confirmed;
        confirmed.runtime_item_id = item.runtime_item_id;
        confirmed.network_stack_id = item.network_stack_id;
        confirmed.count = item.count;
        confirmed.map_uuid = metadata.map_uuid;
        confirmed.item_identifier = std::move(identifier);
        *output = std::move(confirmed);
        if (error) error->clear();
        return true;
    } catch (...) {
        *output = {};
        return fail(error, "native anvil input readback failed");
    }
#endif
}

bool ReadProjectionPrinterNativeAnvilPreviewMap(
    uintptr_t verified_anvil_manager, int32_t expected_runtime_item_id,
    int64_t expected_map_uuid, const std::string& expected_full_display_name,
    ProjectionPrinterNativeAnvilPreviewMapSnapshot* output,
    std::string* error) {
    if (output) *output = {};
#if !defined(__ANDROID__) || !defined(__aarch64__)
    (void)verified_anvil_manager;
    (void)expected_runtime_item_id;
    (void)expected_map_uuid;
    (void)expected_full_display_name;
    return fail(error, "native anvil preview readback requires Android arm64-v8a");
#else
    if (!output || !verified_anvil_manager ||
        expected_runtime_item_id <= 0 || expected_map_uuid == -1 ||
        expected_full_display_name.empty() ||
        expected_full_display_name.size() > 256U) {
        return fail(error, "native anvil preview readback request is invalid");
    }
    try {
        const uintptr_t module_base = Main::getBaseAddress();
        if (!module_base || !isVerifiedMinecraftBuild(module_base)) {
            return fail(error, "native anvil preview game Build ID does not match");
        }
        uintptr_t manager_vtable = 0, preview_address = 0, recipe_address = 0;
        uintptr_t descriptor_copy = 0, user_data = 0, map_uuid_from_compound = 0;
        uintptr_t recipe_assign = 0, recipe_use = 0;
        NativeClientRefreshAbi refresh_abi;
        if (!checkedAddress(module_base, kAnvilManagerVtableRva,
                            &manager_vtable) ||
            !objectHasVtable(reinterpret_cast<const void*>(verified_anvil_manager),
                             manager_vtable) ||
            !checkedAddress(verified_anvil_manager, kAnvilPreviewItemOffset,
                            &preview_address) ||
            !IsMemoryReadable(reinterpret_cast<const void*>(preview_address),
                              kAnvilPreviewItemSize) ||
            !checkedAddress(verified_anvil_manager, kAnvilRecipeIdOffset,
                            &recipe_address) ||
            !IsMemoryReadable(reinterpret_cast<const void*>(recipe_address),
                              sizeof(uint32_t)) ||
            !resolveAndMatch(module_base, kAnvilRecipeAssignRva,
                             kAnvilRecipeAssignFingerprint, &recipe_assign) ||
            !resolveAndMatch(module_base, kAnvilRecipeUseRva,
                             kAnvilRecipeUseFingerprint, &recipe_use) ||
            !resolveAndMatch(module_base, kItemInstanceDescriptorCopyRva,
                             kItemInstanceDescriptorCopyFingerprint,
                             &descriptor_copy) ||
            !resolveAndMatch(module_base, kItemStackUserDataRva,
                             kItemStackUserDataFingerprint, &user_data) ||
            !resolveAndMatch(module_base, kMapUuidFromCompoundRva,
                             kMapUuidFromCompoundFingerprint,
                             &map_uuid_from_compound) ||
            !resolveClientRefreshAbi(module_base, &refresh_abi)) {
            return fail(error, "native anvil preview ItemInstance ABI does not match");
        }
        // The InventorySlot ctor calls this verified base copy. The stock
        // result-click later passes the verified manager+0x218 field into its
        // CraftRecipeOptional ctor. This snapshot records that field only as
        // a diagnostic; the actual click action must supply authoritative ID.
        (void)descriptor_copy;
        (void)recipe_assign;
        (void)recipe_use;
        const auto* preview = reinterpret_cast<const uint8_t*>(preview_address);
        const auto native_uuid = [&]() -> int64_t {
            const void* compound = reinterpret_cast<const void* (*)(const void*)>(
                user_data)(preview);
            if (!compound || !IsMemoryReadable(compound, sizeof(uintptr_t)))
                return -1;
            return reinterpret_cast<int64_t (*)(const void*)>(
                map_uuid_from_compound)(compound);
        };
        uint8_t first_count = 0;
        uint32_t first_recipe_id = 0;
        std::memcpy(&first_count, preview + 0x78U, sizeof(first_count));
        std::memcpy(&first_recipe_id,
                    reinterpret_cast<const void*>(recipe_address),
                    sizeof(first_recipe_id));
        if (first_count != 1U) {
            return fail(error, "native anvil result preview is not one item");
        }

        // The manager owns only a 0xE8-byte ItemInstance at +0x130. The
        // InventorySlot constructor ALSO copies an ItemStack NetIdVariant at
        // source+0xE8. Passing preview directly makes it interpret the
        // adjacent recipe field at manager+0x218 as a variant discriminator;
        // this build then indexes a jump table with that value and crashes.
        // Supply a bounded read-only ItemStack view with a default net ID.
        // The preview has no server stack identity; UUID/name are checked
        // below from its serialized ItemInstance data and native user data.
        const auto write_preview_slot = [&](std::string* packet) -> bool {
            alignas(16) std::array<uint8_t,
                kItemStackNetVariantOffset + sizeof(NetIdVariant)> stack{};
            std::memcpy(stack.data(), preview, kAnvilPreviewItemSize);
            return writeNativeClientSlot(refresh_abi, 0, stack.data(),
                                         packet, error);
        };
        std::string first_packet;
        LogProjectionPrinterInventoryDiagnostic(
            "map_anvil preview serialize begin pass=1");
        if (!write_preview_slot(&first_packet)) return false;
        LogProjectionPrinterInventoryDiagnostic(
            "map_anvil preview serialize ok pass=1 bytes=%zu",
            first_packet.size());
        ProjectionPrinterNativeSlotItem item;
        ItemExtraMapMetadata metadata;
        if (!DecodeProjectionPrinterNativeInventorySlotPreviewMapMetadata(
                first_packet, 0, &item, &metadata) || !item.occupied ||
            item.count != 1U || item.runtime_item_id != expected_runtime_item_id ||
            !metadata.has_map_uuid || metadata.map_uuid != expected_map_uuid ||
            metadata.name_status != MapItemNameStatus::Present ||
            metadata.name_source != MapItemNameSource::DisplayName ||
            metadata.display_name != expected_full_display_name ||
            native_uuid() != expected_map_uuid) {
            return fail(error, "native anvil preview UUID or DisplayName does not match");
        }
        std::string identifier;
        if (ResolveItemRuntimeId(item.runtime_item_id, &identifier) !=
                ItemRuntimeResolveStatus::Ready ||
            (identifier != "minecraft:map" && identifier != "map" &&
             identifier != "minecraft:filled_map" &&
             identifier != "filled_map" &&
             identifier != "minecraft:locator_map" &&
             identifier != "locator_map")) {
            return fail(error, "native anvil preview item is not a filled map");
        }
        uint8_t second_count = 0;
        uint32_t second_recipe_id = 0;
        std::string second_packet;
        if (!objectHasVtable(reinterpret_cast<const void*>(verified_anvil_manager),
                             manager_vtable) ||
            !IsMemoryReadable(preview, kAnvilPreviewItemSize) ||
            !write_preview_slot(&second_packet)) {
            return fail(error, "native anvil result preview changed during readback");
        }
        LogProjectionPrinterInventoryDiagnostic(
            "map_anvil preview serialize ok pass=2 bytes=%zu",
            second_packet.size());
        std::memcpy(&second_count, preview + 0x78U, sizeof(second_count));
        std::memcpy(&second_recipe_id,
                    reinterpret_cast<const void*>(recipe_address),
                    sizeof(second_recipe_id));
        if (second_count != first_count || second_packet != first_packet ||
            second_recipe_id != first_recipe_id ||
            native_uuid() != expected_map_uuid) {
            return fail(error, "native anvil result preview changed during readback");
        }
        ProjectionPrinterNativeAnvilPreviewMapSnapshot confirmed;
        confirmed.runtime_item_id = item.runtime_item_id;
        confirmed.count = item.count;
        confirmed.map_uuid = metadata.map_uuid;
        confirmed.dynamic_recipe_network_id =
            first_recipe_id == std::numeric_limits<uint32_t>::max()
                ? 0U : first_recipe_id;
        confirmed.item_identifier = std::move(identifier);
        confirmed.display_name = std::move(metadata.display_name);
        *output = std::move(confirmed);
        if (error) error->clear();
        return true;
    } catch (...) {
        *output = {};
        return fail(error, "native anvil result preview readback failed");
    }
#endif
}

bool ReadProjectionPrinterNativeInventoryFilledMapMatch(
    int32_t expected_runtime_item_id, int64_t expected_map_uuid,
    const std::string& expected_full_display_name,
    ProjectionPrinterNativeInventoryFilledMapMatch* output,
    std::string* error) {
    if (output) *output = {};
#if !defined(__ANDROID__) || !defined(__aarch64__)
    (void)expected_runtime_item_id;
    (void)expected_map_uuid;
    (void)expected_full_display_name;
    return fail(error, "native renamed-map inventory scan requires Android arm64-v8a");
#else
    if (!output || expected_runtime_item_id <= 0 || expected_map_uuid == -1 ||
        expected_full_display_name.empty() ||
        expected_full_display_name.size() > 256U) {
        return fail(error, "native renamed-map inventory scan request is invalid");
    }
    try {
        const uintptr_t module_base = Main::getBaseAddress();
        if (!module_base || !isVerifiedMinecraftBuild(module_base)) {
            return fail(error, "native renamed-map game Build ID does not match");
        }
        std::string identifier;
        if (ResolveItemRuntimeId(expected_runtime_item_id, &identifier) !=
                ItemRuntimeResolveStatus::Ready ||
            (identifier != "minecraft:map" && identifier != "map" &&
             identifier != "minecraft:filled_map" &&
             identifier != "filled_map" &&
             identifier != "minecraft:locator_map" &&
             identifier != "locator_map")) {
            return fail(error, "native renamed-map runtime ID is not a filled map");
        }
        ResolvedAbi abi;
        NativeClientRefreshAbi refresh_abi;
        uintptr_t user_data = 0, map_uuid_from_compound = 0;
        if (!resolveAbi(module_base, &abi) ||
            !resolveClientRefreshAbi(module_base, &refresh_abi) ||
            !resolveAndMatch(module_base, kItemStackUserDataRva,
                             kItemStackUserDataFingerprint, &user_data) ||
            !resolveAndMatch(module_base, kMapUuidFromCompoundRva,
                             kMapUuidFromCompoundFingerprint,
                             &map_uuid_from_compound)) {
            return fail(error, "native renamed-map ItemStack ABI does not match");
        }
        void* const player = GetLocalPlayerPointer();
        void* container = nullptr;
        if (!getPlayerInventoryContainer(abi, player, &container, error))
            return false;

        struct SlotCapture {
            int32_t network_stack_id = 0;
            std::string packet;
        };
        using Scan = std::array<SlotCapture,
                                kProjectionPrinterLiveInventorySlotCount>;
        const auto capture = [&](Scan* slots, int32_t* matched_slot,
                                 int32_t* matched_network_id) -> bool {
            if (!slots || !matched_slot || !matched_network_id) return false;
            *matched_slot = -1;
            *matched_network_id = 0;
            size_t aggregate_bytes = 0U;
            for (size_t index = 0; index < slots->size(); ++index) {
                const void* stack = nullptr;
                if (!getLiveInventoryStack(module_base, container,
                        static_cast<int32_t>(index), &stack, error)) return false;
                NetIdVariant variant;
                if (!copyVariant(stack, &variant)) {
                    return fail(error, "native renamed-map slot identity is unavailable");
                }
                const int32_t net_id =
                    variantInt32(variant, kNetVariantNetworkIdOffset);
                if (variantInt32(variant, kNetVariantTagOffset) != 0 ||
                    net_id < 0) {
                    return fail(error, "native renamed-map slot identity is unsupported");
                }
                auto& captured = (*slots)[index];
                captured.network_stack_id = net_id;
                if (net_id == 0) continue;
                if (!writeNativeClientSlot(refresh_abi,
                        static_cast<int32_t>(index), stack,
                        &captured.packet, error)) return false;
                constexpr size_t kMaxNativeMapScanBytes = 4U * 1024U * 1024U;
                if (captured.packet.size() >
                        kMaxNativeMapScanBytes - aggregate_bytes) {
                    return fail(error, "native renamed-map inventory is too large to verify");
                }
                aggregate_bytes += captured.packet.size();
                ProjectionPrinterNativeSlotItem item;
                if (!DecodeProjectionPrinterNativeInventorySlotItem(
                        captured.packet, static_cast<uint8_t>(index), &item) ||
                    !item.occupied || !item.has_network_stack_id ||
                    item.network_stack_id != net_id || item.count == 0U) {
                    return fail(error, "native renamed-map slot serialization changed");
                }
                if (item.runtime_item_id != expected_runtime_item_id) continue;
                ItemExtraMapMetadata metadata;
                if (!DecodeProjectionPrinterNativeInventorySlotMapMetadata(
                        captured.packet, static_cast<uint8_t>(index),
                        &item, &metadata)) {
                    return fail(error, "native filled-map metadata is unavailable");
                }
                if (metadata.map_uuid != expected_map_uuid ||
                    metadata.name_status != MapItemNameStatus::Present ||
                    metadata.name_source != MapItemNameSource::DisplayName ||
                    metadata.display_name != expected_full_display_name) continue;
                if (*matched_slot >= 0 || item.count != 1U) {
                    return fail(error, "native renamed-map match is not unique or is stacked");
                }
                const void* compound =
                    reinterpret_cast<const void* (*)(const void*)>(user_data)(stack);
                if (!compound || !IsMemoryReadable(compound, sizeof(uintptr_t)) ||
                    reinterpret_cast<int64_t (*)(const void*)>(
                        map_uuid_from_compound)(compound) != expected_map_uuid) {
                    return fail(error, "native renamed-map UUID disagrees with CompoundTag");
                }
                *matched_slot = static_cast<int32_t>(index);
                *matched_network_id = net_id;
            }
            return true;
        };

        Scan first, second;
        int32_t first_slot = -1, first_net_id = 0;
        int32_t second_slot = -1, second_net_id = 0;
        if (!capture(&first, &first_slot, &first_net_id)) return false;
        if (first_slot < 0) {
            return fail(error, "native renamed-map is absent from player inventory");
        }
        if (!capture(&second, &second_slot, &second_net_id)) return false;
        if (first_slot != second_slot || first_net_id != second_net_id) {
            return fail(error, "native renamed-map moved during inventory scan");
        }
        for (size_t index = 0; index < first.size(); ++index) {
            if (first[index].network_stack_id != second[index].network_stack_id ||
                first[index].packet != second[index].packet) {
                return fail(error, "native inventory changed during renamed-map scan");
            }
        }
        void* container_after = nullptr;
        if (!getPlayerInventoryContainer(abi, player, &container_after, error) ||
            container_after != container) {
            return fail(error, "native renamed-map inventory container changed");
        }
        ProjectionPrinterNativeInventoryFilledMapMatch confirmed;
        confirmed.inventory_slot = first_slot;
        confirmed.runtime_item_id = expected_runtime_item_id;
        confirmed.network_stack_id = first_net_id;
        confirmed.count = 1U;
        confirmed.map_uuid = expected_map_uuid;
        confirmed.item_identifier = std::move(identifier);
        confirmed.display_name = expected_full_display_name;
        *output = std::move(confirmed);
        if (error) error->clear();
        return true;
    } catch (...) {
        *output = {};
        return fail(error, "native renamed-map inventory scan failed");
    }
#endif
}

bool ReadProjectionPrinterNativeInventoryMapUuidAbsent(
    int32_t expected_runtime_item_id, int64_t expected_map_uuid,
    bool* absent, std::string* error) {
    if (absent) *absent = false;
#if !defined(__ANDROID__) || !defined(__aarch64__)
    (void)expected_runtime_item_id;
    (void)expected_map_uuid;
    return fail(error, "native map UUID absence proof requires Android arm64-v8a");
#else
    if (!absent || expected_runtime_item_id <= 0 || expected_map_uuid == -1) {
        return fail(error, "native map UUID absence proof request is invalid");
    }
    if (!IsMinecraftUpdateGameThread()) {
        return fail(error, "native map UUID absence proof requires the game thread");
    }
    try {
        const uintptr_t module_base = Main::getBaseAddress();
        if (!module_base || !isVerifiedMinecraftBuild(module_base)) {
            return fail(error, "native map UUID absence proof game Build ID does not match");
        }
        const auto is_filled_map = [](const std::string& identifier) {
            return identifier == "minecraft:map" || identifier == "map" ||
                identifier == "minecraft:filled_map" || identifier == "filled_map" ||
                identifier == "minecraft:locator_map" || identifier == "locator_map";
        };
        std::string expected_identifier;
        if (ResolveItemRuntimeId(expected_runtime_item_id, &expected_identifier) !=
                ItemRuntimeResolveStatus::Ready ||
            !is_filled_map(expected_identifier)) {
            return fail(error, "native map UUID absence proof runtime ID is not a filled map");
        }
        ResolvedAbi abi;
        NativeClientRefreshAbi refresh_abi;
        uintptr_t user_data = 0, map_uuid_from_compound = 0;
        if (!resolveAbi(module_base, &abi) ||
            !resolveClientRefreshAbi(module_base, &refresh_abi) ||
            !resolveAndMatch(module_base, kItemStackUserDataRva,
                             kItemStackUserDataFingerprint, &user_data) ||
            !resolveAndMatch(module_base, kMapUuidFromCompoundRva,
                             kMapUuidFromCompoundFingerprint,
                             &map_uuid_from_compound)) {
            return fail(error, "native map UUID absence proof ItemStack ABI does not match");
        }
        void* const player = GetLocalPlayerPointer();
        void* container = nullptr;
        if (!getPlayerInventoryContainer(abi, player, &container, error)) return false;

        struct SlotCapture {
            int32_t network_stack_id = 0;
            std::array<uint8_t, 0x18U> network_variant{};
            int64_t native_map_uuid = -1;
            std::string identifier;
            std::string packet;
        };
        using Scan = std::array<SlotCapture,
                                kProjectionPrinterLiveInventorySlotCount>;
        const auto capture = [&](Scan* slots, bool* found) -> bool {
            if (!slots || !found) {
                return fail(error, "native map UUID absence proof scan output is unavailable");
            }
            *found = false;
            size_t aggregate_bytes = 0U;
            constexpr size_t kMaxNativeMapScanBytes = 4U * 1024U * 1024U;
            for (size_t index = 0; index < slots->size(); ++index) {
                const void* stack = nullptr;
                if (!getLiveInventoryStack(module_base, container,
                        static_cast<int32_t>(index), &stack, error)) return false;
                NetIdVariant before, after;
                if (!copyVariant(stack, &before)) {
                    return fail(error, "native map UUID absence proof slot identity is unavailable");
                }
                const int32_t net_id =
                    variantInt32(before, kNetVariantNetworkIdOffset);
                auto& captured = (*slots)[index];
                captured.network_stack_id = net_id;
                captured.network_variant = before.bytes;
                // The native ItemStack's map UUID is authoritative for this
                // absence check. After an accepted chest Place, the network
                // variant can lag behind the serializer (e.g. retain its old
                // net ID for an already-empty source). Read userData even
                // when the serialized slot looks empty, so that mismatch
                // cannot hide the exact map UUID.
                const void* compound =
                    reinterpret_cast<const void* (*)(const void*)>(user_data)(stack);
                if (compound) {
                    if (!IsMemoryReadable(compound, sizeof(uintptr_t))) {
                        return fail(error, "native map UUID absence proof CompoundTag is unreadable");
                    }
                    captured.native_map_uuid =
                        reinterpret_cast<int64_t (*)(const void*)>(
                            map_uuid_from_compound)(compound);
                    if (captured.native_map_uuid == expected_map_uuid) {
                        *found = true;
                    }
                }
                // Serialize even network-ID-zero slots: an unreadable occupied
                // stack must not be silently treated as an empty slot.
                if (!writeNativeClientSlot(refresh_abi, static_cast<int32_t>(index),
                                           stack, &captured.packet, error)) return false;
                if (captured.packet.size() >
                        kMaxNativeMapScanBytes - aggregate_bytes) {
                    return fail(error, "native map UUID absence proof inventory is too large");
                }
                aggregate_bytes += captured.packet.size();
                ProjectionPrinterNativeSlotItem item;
                if (!DecodeProjectionPrinterNativeInventorySlotItem(
                        captured.packet, static_cast<uint8_t>(index), &item)) {
                    return fail(error, "native map UUID absence proof slot cannot be decoded");
                }
                const int32_t wire_net_id = item.has_network_stack_id
                    ? item.network_stack_id : 0;
                if (net_id != wire_net_id) {
                    LogProjectionPrinterInventoryDiagnostic(
                        "map_uuid_scan identity-lag slot=%zu native_net=%d wire_net=%d occupied=%d runtime=%d count=%u",
                        index, net_id, wire_net_id, item.occupied ? 1 : 0,
                        item.runtime_item_id, static_cast<unsigned>(item.count));
                }
                if (item.occupied) {
                    if (item.runtime_item_id <= 0 || item.count == 0U) {
                        return fail(error, "native map UUID absence proof occupied slot is invalid");
                    }
                    if (ResolveItemRuntimeId(item.runtime_item_id,
                                             &captured.identifier) !=
                            ItemRuntimeResolveStatus::Ready) {
                        return fail(error, "native map UUID absence proof item identity is unavailable");
                    }
                    if (is_filled_map(captured.identifier)) {
                        ProjectionPrinterNativeSlotItem map_item;
                        ItemExtraMapMetadata metadata;
                        if (!DecodeProjectionPrinterNativeInventorySlotMapMetadata(
                                captured.packet, static_cast<uint8_t>(index),
                                &map_item, &metadata) ||
                            map_item.runtime_item_id != item.runtime_item_id ||
                            map_item.count != item.count ||
                            !metadata.has_map_uuid || metadata.map_uuid == -1) {
                            return fail(error, "native map UUID absence proof filled-map metadata is unavailable");
                        }
                        if (captured.native_map_uuid == -1 ||
                            captured.native_map_uuid != metadata.map_uuid) {
                            return fail(error, "native map UUID absence proof UUID readers disagree");
                        }
                    }
                } else if (item.runtime_item_id != 0 || item.count != 0U ||
                           item.has_network_stack_id) {
                    return fail(error, "native map UUID absence proof empty packet is invalid");
                }
                if (!copyVariant(stack, &after) || before.bytes != after.bytes) {
                    return fail(error, "native map UUID absence proof stack changed during read");
                }
            }
            return true;
        };

        Scan first, second;
        bool first_found = false, second_found = false;
        if (!capture(&first, &first_found) ||
            !capture(&second, &second_found)) return false;
        if (first_found != second_found) {
            return fail(error, "native map UUID presence changed during absence proof");
        }
        for (size_t index = 0; index < first.size(); ++index) {
            if (first[index].network_stack_id != second[index].network_stack_id ||
                first[index].network_variant != second[index].network_variant ||
                first[index].native_map_uuid != second[index].native_map_uuid ||
                first[index].identifier != second[index].identifier ||
                first[index].packet != second[index].packet) {
                return fail(error, "native inventory changed during map UUID absence proof");
            }
        }
        void* container_after = nullptr;
        std::string identifier_after;
        if (GetLocalPlayerPointer() != player ||
            !getPlayerInventoryContainer(abi, player, &container_after, error) ||
            container_after != container ||
            ResolveItemRuntimeId(expected_runtime_item_id,
                                 &identifier_after) != ItemRuntimeResolveStatus::Ready ||
            identifier_after != expected_identifier) {
            return fail(error, "native map UUID absence proof player or registry changed");
        }
        *absent = !second_found;
        if (error) error->clear();
        return true;
    } catch (...) {
        return fail(error, "native map UUID absence proof scan failed");
    }
#endif
}

bool ReadProjectionPrinterNativeSelectedFilledMap(
    int32_t expected_runtime_item_id, int64_t expected_map_uuid,
    ProjectionPrinterNativeSelectedFilledMapSnapshot* output,
    std::string* error) {
    if (output) *output = {};
#if !defined(__ANDROID__) || !defined(__aarch64__)
    (void)expected_runtime_item_id;
    (void)expected_map_uuid;
    return fail(error, "native selected filled-map readback requires Android arm64-v8a");
#else
    if (!output || expected_runtime_item_id < 0 || expected_map_uuid == -1) {
        return fail(error, "native selected filled-map request is invalid");
    }
    try {
        const uintptr_t module_base = Main::getBaseAddress();
        if (!module_base || !isVerifiedMinecraftBuild(module_base)) {
            return fail(error, "native selected filled-map game Build ID does not match");
        }
        std::string first_packet;
        int32_t first_slot = -1;
        if (!ReadProjectionPrinterNativeSelectedHotbarSlotPacket(
                &first_packet, &first_slot, error)) return false;
        ProjectionPrinterNativeSlotItem item;
        ItemExtraMapMetadata metadata;
        if (first_slot < 0 || first_slot > 8 ||
            !DecodeProjectionPrinterNativeInventorySlotMapMetadata(
                first_packet, static_cast<uint8_t>(first_slot),
                &item, &metadata) ||
            !item.occupied || item.count != 1U || item.runtime_item_id <= 0 ||
            (expected_runtime_item_id > 0 &&
             item.runtime_item_id != expected_runtime_item_id) ||
            !item.has_network_stack_id || item.network_stack_id <= 0 ||
            !metadata.has_map_uuid || metadata.map_uuid != expected_map_uuid) {
            return fail(error, "selected ItemStack is not the expected one-map stack");
        }
        std::string identifier;
        if (ResolveItemRuntimeId(item.runtime_item_id, &identifier) !=
                ItemRuntimeResolveStatus::Ready ||
            (identifier != "minecraft:map" && identifier != "map" &&
             identifier != "minecraft:filled_map" &&
             identifier != "filled_map" &&
             identifier != "minecraft:locator_map" &&
             identifier != "locator_map")) {
            return fail(error, "selected ItemStack is not a filled map");
        }
        int64_t direct_uuid = -1;
        if (!ReadProjectionPrinterNativeMapUuid(first_slot,
                item.network_stack_id, &direct_uuid, error) ||
            direct_uuid != expected_map_uuid) {
            return fail(error, "selected filled-map NBT UUID does not match");
        }
        std::string second_packet;
        int32_t second_slot = -1;
        if (!ReadProjectionPrinterNativeSelectedHotbarSlotPacket(
                &second_packet, &second_slot, error)) return false;
        int64_t second_uuid = -1;
        if (second_slot != first_slot || second_packet != first_packet ||
            !ReadProjectionPrinterNativeMapUuid(second_slot,
                item.network_stack_id, &second_uuid, error) ||
            second_uuid != expected_map_uuid) {
            return fail(error, "selected filled map changed during readback");
        }
        ProjectionPrinterNativeSelectedFilledMapSnapshot confirmed;
        confirmed.selected_hotbar_slot = first_slot;
        confirmed.runtime_item_id = item.runtime_item_id;
        confirmed.network_stack_id = item.network_stack_id;
        confirmed.count = item.count;
        confirmed.map_uuid = expected_map_uuid;
        confirmed.item_identifier = std::move(identifier);
        *output = std::move(confirmed);
        if (error) error->clear();
        return true;
    } catch (...) {
        *output = {};
        return fail(error, "native selected filled-map readback failed");
    }
#endif
}

bool PrepareProjectionPrinterLiveInventoryClientSyncMove(
    const ProjectionPrinterInventoryMoveRequest& request,
    ProjectionPrinterInventoryClientSyncPreparedMove* prepared,
    std::string* error) {
    if (prepared) *prepared = {};
#if !defined(__aarch64__)
    (void)request;
    return fail(error, "native client inventory refresh is supported only on arm64-v8a");
#else
    if (!prepared || request.source_inventory_slot < 9 || request.source_inventory_slot > 35 ||
        request.destination_hotbar_slot < 0 || request.destination_hotbar_slot > 8 ||
        request.expected_source_network_stack_id <= 0 || request.expected_source_count == 0U) {
        return fail(error, "native client inventory refresh request is invalid");
    }
    try {
        const uintptr_t module_base = Main::getBaseAddress();
        ResolvedAbi abi;
        NativeClientRefreshAbi refresh_abi;
        if (!module_base || !resolveAbi(module_base, &abi) ||
            !resolveClientRefreshAbi(module_base, &refresh_abi)) {
            return fail(error, "native inventory refresh ABI does not match this game version");
        }
        void* const player = GetLocalPlayerPointer();
        void* container = nullptr;
        if (!getPlayerInventoryContainer(abi, player, &container, error)) return false;
        const void* source = nullptr;
        const void* destination = nullptr;
        if (!getLiveInventoryStack(module_base, container, request.source_inventory_slot,
                                    &source, error) ||
            !getLiveInventoryStack(module_base, container, request.destination_hotbar_slot,
                                    &destination, error)) return false;
        NetIdVariant source_variant;
        NetIdVariant destination_variant;
        if (!copyVariant(source, &source_variant) ||
            !isOrdinaryNetworkVariant(source_variant, request.expected_source_network_stack_id) ||
            !copyVariant(destination, &destination_variant) ||
            (request.expected_destination_occupied ?
                !isOrdinaryNetworkVariant(destination_variant,
                    request.expected_destination_network_stack_id) :
                !isDefaultEmptyNetworkVariant(destination_variant))) {
            return fail(error, "live inventory stacks changed before client refresh");
        }
        std::string source_packet;
        std::string destination_packet;
        if (!writeNativeClientSlot(refresh_abi, request.source_inventory_slot,
                                    destination, &source_packet, error) ||
            !writeNativeClientSlot(refresh_abi, request.destination_hotbar_slot,
                                    source, &destination_packet, error)) return false;
        ProjectionPrinterInventoryClientSyncMove move;
        move.source_inventory_slot = static_cast<uint8_t>(request.source_inventory_slot);
        move.destination_hotbar_slot = static_cast<uint8_t>(request.destination_hotbar_slot);
        move.expected_source_network_stack_id = request.expected_source_network_stack_id;
        move.expected_source_count = request.expected_source_count;
        move.expected_destination_occupied = request.expected_destination_occupied;
        move.expected_destination_network_stack_id = request.expected_destination_network_stack_id;
        return PrepareProjectionPrinterInventoryClientSyncNativeMove(
            move, std::move(source_packet), std::move(destination_packet), prepared, error);
    } catch (...) {
        return fail(error, "native client inventory refresh could not be serialized");
    }
#endif
}

bool MoveProjectionPrinterBackpackItemToHotbar(
    const ProjectionPrinterInventoryMoveRequest& request, std::string* error,
    int32_t* request_id) {
#if !defined(__aarch64__)
    (void)request;
    if (request_id) *request_id = 0;
    return fail(error, "projection-printer inventory moves are supported only on arm64-v8a");
#else
    if (request_id) *request_id = 0;
    if (!IsMinecraftUpdateGameThread()) {
        return fail(error, "printer inventory moves must run on the local-player game tick");
    }
    if (request.source_inventory_slot < 9 || request.source_inventory_slot > 35 ||
        request.destination_hotbar_slot < 0 || request.destination_hotbar_slot > 8 ||
        request.expected_source_network_stack_id <= 0 || request.expected_source_count == 0U ||
        request.expected_source_count > std::numeric_limits<uint8_t>::max() ||
        (request.expected_destination_occupied &&
         request.expected_destination_network_stack_id <= 0)) {
        return fail(error, "the requested printer inventory move is invalid");
    }
    const uintptr_t module_base = Main::getBaseAddress();
    if (!module_base) return fail(error, "libminecraftpe.so is not loaded");
    ResolvedAbi abi;
    if (!resolveAbi(module_base, &abi)) {
        return fail(error, "projection-printer inventory ABI profile does not match this game version");
    }
    void* const player = GetLocalPlayerPointer();
    if (!player ||
        !IsMemoryReadable(player, 0x18U)) {
        return fail(error, "the local player is unavailable");
    }
    void* container = nullptr;
    if (!getPlayerInventoryContainer(abi, player, &container, error)) return false;
    const void* source_stack = nullptr;
    const void* destination_stack = nullptr;
    if (!getLiveInventoryStack(module_base, container, request.source_inventory_slot,
                               &source_stack, error) ||
        !getLiveInventoryStack(module_base, container, request.destination_hotbar_slot,
                               &destination_stack, error)) {
        return false;
    }
    NetIdVariant source_variant;
    NetIdVariant destination_variant;
    if (!copyVariant(source_stack, &source_variant) ||
        !isOrdinaryNetworkVariant(source_variant,
                                  request.expected_source_network_stack_id)) {
        return fail(error, "the source backpack ItemStack changed before it could be moved");
    }
    if (!copyVariant(destination_stack, &destination_variant)) {
        return fail(error, "the destination hotbar ItemStack is unavailable");
    }
    if (request.expected_destination_occupied) {
        if (!isOrdinaryNetworkVariant(destination_variant,
                                      request.expected_destination_network_stack_id)) {
            return fail(error, "the destination hotbar ItemStack changed before it could be moved");
        }
    } else if (!isDefaultEmptyNetworkVariant(destination_variant)) {
        return fail(error, "the destination hotbar slot is no longer empty");
    }

    SlotInfo source_info;
    SlotInfo destination_info;
    if (!makeSlotInfo(kInventoryContainer,
                      static_cast<uint8_t>(request.source_inventory_slot),
                      source_variant, &source_info)) {
        return fail(error, "unable to prepare the source ItemStack request slot");
    }
    if (!makeSlotInfo(kHotbarContainer,
                      static_cast<uint8_t>(request.destination_hotbar_slot),
                      destination_variant, &destination_info)) {
        return fail(error, "unable to prepare the destination ItemStack request slot");
    }

    NativeRequestPacketAbi packet_abi;
    if (!resolveRequestPacketAbi(module_base, &packet_abi, error)) return false;
    void* const sender = GetCapturedLoopbackPacketSender();
    if (!sender) return fail(error, "LoopbackPacketSender is not ready");

    // Reserve from the same counter as the stock client, without opening a
    // prediction scope. The reference's wire IDs 1,5,9 decode to -1,-3,-5;
    // they are not positive signed request IDs. Sharing the game's counter
    // avoids collisions with ordinary user inventory operations.
    int32_t counter = 0;
    std::memcpy(&counter, reinterpret_cast<const void*>(packet_abi.counter), sizeof(counter));
    const int32_t normalized = counter < -1 ? counter : -1;
    if ((static_cast<uint32_t>(normalized) & 1U) == 0U ||
        normalized < std::numeric_limits<int32_t>::min() + 2) {
        return fail(error, "the native ItemStack request counter is invalid or exhausted");
    }
    const int32_t submitted_id = normalized - 2;
    std::memcpy(reinterpret_cast<void*>(packet_abi.counter), &submitted_id, sizeof(submitted_id));

    struct alignas(8) NativeRequestId {
        uintptr_t vtable;
        int32_t value;
        uint32_t padding;
    };
    static_assert(sizeof(NativeRequestId) == 0x10U, "unexpected native request ID ABI");
    const NativeRequestId native_id{abi.item_stack_request_vtable, submitted_id, 0U};
    LogProjectionPrinterInventoryDiagnostic(
        "move begin id=%d source=%d net=%d count=%u destination=%d occupied=%d net=%d",
        submitted_id, request.source_inventory_slot, request.expected_source_network_stack_id,
        static_cast<unsigned int>(request.expected_source_count), request.destination_hotbar_slot,
        request.expected_destination_occupied, request.expected_destination_network_stack_id);

    // Allocate every heap object in the game's allocator domain. Native
    // RequestData owns the action, Batch owns RequestData, and the stack Packet
    // owns Batch. All handoffs use a pointer-sized unique_ptr ABI and clear
    // their source owner, so both normal destruction and allocation failures
    // free each object exactly once.
    try {
        NativeRequestObjectOwner action(packet_abi.deallocate,
                                         module_base + kActionCompleteDestructorRva);
        action.allocate(packet_abi.allocate, kActionAllocationSize);
        if (!action.get()) return fail(error, "unable to allocate a native request action");
        const uintptr_t action_vtable = request.expected_destination_occupied
            ? abi.swap_action_vtable : abi.place_action_vtable;
        if (request.expected_destination_occupied) {
            reinterpret_cast<SwapActionConstructor>(abi.swap_action_constructor)(
                action.get(), &source_info, &destination_info);
        } else {
            reinterpret_cast<PlaceActionConstructor>(abi.place_action_constructor)(
                action.get(), static_cast<uint8_t>(request.expected_source_count),
                &source_info, &destination_info);
        }
        action.markConstructed();
        if (!objectHasVtable(action.get(), action_vtable)) {
            return fail(error, "the native request action vtable changed");
        }

        NativeRequestObjectOwner data(packet_abi.deallocate, packet_abi.data_destructor);
        data.allocate(packet_abi.allocate, kRequestDataSize);
        if (!data.get()) return fail(error, "unable to allocate native request data");
        reinterpret_cast<void (*)(void*, const NativeRequestId*)>(
            packet_abi.data_constructor)(data.get(), &native_id);
        data.markConstructed();
        if (!objectHasVtable(data.get(), abi.item_stack_request_vtable)) {
            return fail(error, "the native request data vtable changed");
        }
        int32_t constructed_id = 0;
        std::memcpy(&constructed_id, static_cast<const uint8_t*>(data.get()) + 0x08U,
                    sizeof(constructed_id));
        if (constructed_id != submitted_id) {
            return fail(error, "the native request data did not retain its request ID");
        }
        reinterpret_cast<void (*)(void*, void**)>(packet_abi.data_add_action)(
            data.get(), action.ownerAddress());
        if (action.get()) return fail(error, "native request data did not accept its action");

        NativeRequestObjectOwner batch(packet_abi.deallocate, packet_abi.batch_destructor);
        batch.allocate(packet_abi.allocate, kRequestBatchSize);
        if (!batch.get()) return fail(error, "unable to allocate a native request batch");
        reinterpret_cast<void (*)(void*)>(packet_abi.batch_constructor)(batch.get());
        batch.markConstructed();
        reinterpret_cast<void (*)(void*, void**)>(packet_abi.batch_add_request)(
            batch.get(), data.ownerAddress());
        if (data.get()) return fail(error, "native request batch did not accept its request");

        alignas(8) std::array<uint8_t, kRequestPacketSize> packet{};
        reinterpret_cast<void (*)(void*, void**)>(packet_abi.packet_constructor)(
            packet.data(), batch.ownerAddress());
        NativeCompleteDestructorGuard packet_guard(packet.data(), packet_abi.packet_destructor);
        if (batch.get() || !objectHasVtable(packet.data(), packet_abi.packet_vtable)) {
            return fail(error, "the native request packet did not accept its batch");
        }
        reinterpret_cast<void (*)(void*, void*)>(packet_abi.send_to_server)(sender, packet.data());
        LogProjectionPrinterInventoryDiagnostic("move sendToServer returned id=%d", submitted_id);
    } catch (...) {
        return fail(error, "the independent native ItemStack request could not be sent");
    }
    if (request_id) *request_id = submitted_id;
    if (error) error->clear();
    return true;
#endif
}

bool SendFilledMapToSingleChest(
    const FilledMapToSingleChestRequest& request,
    FilledMapChestSubmission* submission, std::string* error) {
    if (submission) *submission = {};
    if (error) error->clear();
#if !defined(__aarch64__)
    (void)request;
    return fail(error, "filled-map chest storage is supported only on arm64-v8a");
#else
    if (!submission || !IsMinecraftUpdateGameThread()) {
        return fail(error, "filled-map chest storage requires a local-player tick");
    }
    std::string current_world;
    if (!QueryWorldContextOnGameThread(&current_world, 1000)) {
        return fail(error, "the live world context could not be verified");
    }
    FilledMapChestWorldEvidence world;
    world.world_context = std::move(current_world);
    NativeBlockInfo block;
    if (!NativeWorldAccess::getBlock(request.chest_x, request.chest_y,
                                      request.chest_z, &block)) {
        return fail(error, "the chest block could not be read from the live world");
    }
    world.block_identifier = std::move(block.name);

    ProjectionPrinterNativeInventorySnapshot native;
    if (!ReadProjectionPrinterNativeInventorySnapshot(&native, error) ||
        !native.ready || request.source_hotbar_slot < 0 ||
        request.source_hotbar_slot > 8) {
        return fail(error, "the live filled-map hotbar slot is unavailable");
    }
    const auto& native_source =
        native.slots[static_cast<size_t>(request.source_hotbar_slot)];
    FilledMapChestSourceEvidence source;
    source.selected_hotbar_slot = native.selected_hotbar_slot;
    source.network_stack_id = native_source.network_stack_id;
    source.runtime_item_id = native_source.runtime_item_id;
    source.count = native_source.count;
    if (native_source.occupied && native_source.has_network_stack_id &&
        ResolveItemRuntimeId(native_source.runtime_item_id,
                             &source.item_identifier) == ItemRuntimeResolveStatus::Ready) {
        int64_t map_uuid = -1;
        if (ReadProjectionPrinterNativeMapUuid(
                request.source_hotbar_slot, native_source.network_stack_id,
                &map_uuid, nullptr)) {
            source.has_map_uuid = true;
            source.map_uuid = map_uuid;
        }
    }

    ContainerCaptureResult capture;
    if (PollContainerCapture(request.capture_token, &capture) !=
        ContainerCapturePollState::Ready) {
        return fail(error, "the matching single-chest capture is not ready for storage");
    }
    if (!ValidateFilledMapSingleChestPreflight(request, capture, source,
                                                world, error)) return false;
    if (!VerifyProjectionPrinterSelectedHotbarItem(
            request.source_hotbar_slot, request.source_network_stack_id, error)) {
        return false;
    }

    const uintptr_t module_base = Main::getBaseAddress();
    if (!module_base) return fail(error, "libminecraftpe.so is not loaded");
    ResolvedAbi abi;
    if (!resolveAbi(module_base, &abi)) {
        return fail(error, "filled-map chest storage ABI does not match this game version");
    }
    void* const player = GetLocalPlayerPointer();
    if (!player || !IsMemoryReadable(player, 0x18U)) {
        return fail(error, "the local player is unavailable");
    }
    void* player_container = nullptr;
    if (!getPlayerInventoryContainer(abi, player, &player_container, error)) return false;
    const void* source_stack = nullptr;
    if (!getLiveInventoryStack(module_base, player_container,
                               request.source_hotbar_slot, &source_stack, error)) {
        return false;
    }
    NetIdVariant source_variant;
    if (!copyVariant(source_stack, &source_variant) ||
        !isOrdinaryNetworkVariant(source_variant,
                                  request.source_network_stack_id)) {
        return fail(error, "the filled-map stack changed before Place could be sent");
    }
    int64_t final_map_uuid = -1;
    if (!ReadProjectionPrinterNativeMapUuid(request.source_hotbar_slot,
                                            request.source_network_stack_id,
                                            &final_map_uuid, error) ||
        final_map_uuid != request.expected_map_uuid) {
        return fail(error, "the filled-map UUID changed before Place could be sent");
    }
    NetIdVariant destination_variant;
    reinterpret_cast<DefaultNetIdVariant>(abi.default_net_id_variant)(
        &destination_variant);
    if (!isDefaultEmptyNetworkVariant(destination_variant)) {
        return fail(error, "the native empty chest-slot network variant is invalid");
    }
    SlotInfo source_info;
    SlotInfo destination_info;
    constexpr uint8_t kChestRequestContainer = 7U;
    if (!makeSlotInfo(kHotbarContainer,
                      static_cast<uint8_t>(request.source_hotbar_slot),
                      source_variant, &source_info) ||
        !makeSlotInfo(kChestRequestContainer, request.destination_slot,
                      destination_variant, &destination_info)) {
        return fail(error, "unable to prepare filled-map chest request slots");
    }

    NativeRequestPacketAbi packet_abi;
    if (!resolveRequestPacketAbi(module_base, &packet_abi, error)) return false;
    void* const sender = GetCapturedLoopbackPacketSender();
    if (!sender) return fail(error, "LoopbackPacketSender is not ready");
    // The window can close while native ABI resolution runs. Recheck the
    // token, open state and empty target immediately before reserving an ID.
    ContainerCaptureResult current_capture;
    if (PollContainerCapture(request.capture_token, &current_capture) !=
        ContainerCapturePollState::Ready) {
        return fail(error, "the chest window changed before submission");
    }
    std::string final_world_context;
    NativeBlockInfo final_chest_block;
    if (!QueryWorldContextOnGameThread(&final_world_context, 1000) ||
        final_world_context != request.expected_world_context ||
        !NativeWorldAccess::getBlock(request.chest_x, request.chest_y,
                                     request.chest_z, &final_chest_block)) {
        return fail(error, "the live world or chest changed before submission");
    }
    world.world_context = std::move(final_world_context);
    world.block_identifier = std::move(final_chest_block.name);
    if (!ValidateFilledMapSingleChestPreflight(request, current_capture,
                                                source, world, error)) return false;
    ProjectionPrinterNativeInventorySnapshot final_native;
    if (!ReadProjectionPrinterNativeInventorySnapshot(&final_native, nullptr) ||
        !final_native.ready ||
        final_native.selected_hotbar_slot != request.source_hotbar_slot ||
        final_native.slots[static_cast<size_t>(request.source_hotbar_slot)].count != 1U ||
        final_native.slots[static_cast<size_t>(request.source_hotbar_slot)]
            .runtime_item_id != request.expected_runtime_item_id ||
        final_native.slots[static_cast<size_t>(request.source_hotbar_slot)]
            .network_stack_id != request.source_network_stack_id) {
        return fail(error, "the filled-map count or hotbar identity changed before submission");
    }

    int32_t counter = 0;
    std::memcpy(&counter, reinterpret_cast<const void*>(packet_abi.counter),
                sizeof(counter));
    const int32_t normalized = counter < -1 ? counter : -1;
    if ((static_cast<uint32_t>(normalized) & 1U) == 0U ||
        normalized < std::numeric_limits<int32_t>::min() + 2) {
        return fail(error, "the native ItemStack request counter is invalid or exhausted");
    }
    const int32_t submitted_id = normalized - 2;
    std::memcpy(reinterpret_cast<void*>(packet_abi.counter), &submitted_id,
                sizeof(submitted_id));
    struct alignas(8) NativeRequestId {
        uintptr_t vtable;
        int32_t value;
        uint32_t padding;
    };
    static_assert(sizeof(NativeRequestId) == 0x10U,
                  "unexpected native request ID ABI");
    const NativeRequestId native_id{
        abi.item_stack_request_vtable, submitted_id, 0U};
    const uint64_t response_generation =
        GetProjectionPrinterInventoryMailboxSessionGeneration();
    ProjectionPrinterInventoryResponse prior_response;
    const uint64_t response_generation_before_send =
        GetProjectionPrinterInventoryResponse(&prior_response) &&
        prior_response.session_generation == response_generation
            ? prior_response.response_generation : 0U;
    bool send_invoked = false;
    bool submission_uncertain = false;
    try {
        NativeRequestObjectOwner action(packet_abi.deallocate,
                                         module_base + kActionCompleteDestructorRva);
        action.allocate(packet_abi.allocate, kActionAllocationSize);
        if (!action.get()) return fail(error, "unable to allocate chest Place action");
        reinterpret_cast<PlaceActionConstructor>(abi.place_action_constructor)(
            action.get(), 1U, &source_info, &destination_info);
        action.markConstructed();
        if (!objectHasVtable(action.get(), abi.place_action_vtable)) {
            return fail(error, "native chest Place action vtable changed");
        }

        NativeRequestObjectOwner data(packet_abi.deallocate,
                                      packet_abi.data_destructor);
        data.allocate(packet_abi.allocate, kRequestDataSize);
        if (!data.get()) return fail(error, "unable to allocate chest request data");
        reinterpret_cast<void (*)(void*, const NativeRequestId*)>(
            packet_abi.data_constructor)(data.get(), &native_id);
        data.markConstructed();
        if (!objectHasVtable(data.get(), abi.item_stack_request_vtable)) {
            return fail(error, "native chest request data vtable changed");
        }
        int32_t constructed_id = 0;
        std::memcpy(&constructed_id,
                    static_cast<const uint8_t*>(data.get()) + 0x08U,
                    sizeof(constructed_id));
        if (constructed_id != submitted_id) {
            return fail(error, "native chest request data lost its request ID");
        }
        reinterpret_cast<void (*)(void*, void**)>(packet_abi.data_add_action)(
            data.get(), action.ownerAddress());
        if (action.get()) return fail(error, "chest request did not accept Place action");

        NativeRequestObjectOwner batch(packet_abi.deallocate,
                                       packet_abi.batch_destructor);
        batch.allocate(packet_abi.allocate, kRequestBatchSize);
        if (!batch.get()) return fail(error, "unable to allocate chest request batch");
        reinterpret_cast<void (*)(void*)>(packet_abi.batch_constructor)(batch.get());
        batch.markConstructed();
        reinterpret_cast<void (*)(void*, void**)>(packet_abi.batch_add_request)(
            batch.get(), data.ownerAddress());
        if (data.get()) return fail(error, "chest batch did not accept request data");

        alignas(8) std::array<uint8_t, kRequestPacketSize> packet{};
        reinterpret_cast<void (*)(void*, void**)>(packet_abi.packet_constructor)(
            packet.data(), batch.ownerAddress());
        NativeCompleteDestructorGuard packet_guard(
            packet.data(), packet_abi.packet_destructor);
        if (batch.get() || !objectHasVtable(packet.data(), packet_abi.packet_vtable)) {
            return fail(error, "native chest packet did not accept its batch");
        }
        // Preserve the game's original source-slot bytes before the server
        // transfer. They authorize only a later client-local empty-slot
        // presentation update after this exact request is accepted.
        std::string occupied_source_packet;
        int32_t selected_slot = -1;
        if (!ReadProjectionPrinterNativeSelectedHotbarSlotPacket(
                &occupied_source_packet, &selected_slot, error) ||
            selected_slot != request.source_hotbar_slot) {
            return fail(error, "the selected map changed before chest client sync preparation");
        }
        MapChestClientSyncCandidate client_sync;
        client_sync.request_id = submitted_id;
        client_sync.session_generation = response_generation;
        client_sync.response_generation_before_send =
            response_generation_before_send;
        client_sync.window_token = request.capture_token;
        client_sync.window_id = request.expected_window_id;
        client_sync.source_hotbar_slot =
            static_cast<uint8_t>(request.source_hotbar_slot);
        client_sync.destination_slot = request.destination_slot;
        client_sync.runtime_item_id = request.expected_runtime_item_id;
        client_sync.source_network_stack_id =
            request.source_network_stack_id;
        client_sync.map_uuid = request.expected_map_uuid;
        client_sync.occupied_source_packet =
            std::move(occupied_source_packet);
        if (!PrepareMapChestClientSyncCandidate(
                std::move(client_sync), error)) return false;
        LogProjectionPrinterInventoryDiagnostic(
            "map_chest local_prepared request=%d window=%u source=%d dest=%u net=%d",
            submitted_id, static_cast<unsigned>(request.expected_window_id),
            request.source_hotbar_slot,
            static_cast<unsigned>(request.destination_slot),
            request.source_network_stack_id);
        if (!ArmFilledMapChestDispatchBeforeSend(request, submitted_id, error)) {
            CancelMapChestClientSyncWindow(request.capture_token);
            LogProjectionPrinterInventoryDiagnostic(
                "map_chest Place aborted before send id=%d journal=failed",
                submitted_id);
            return false;
        }
        // Never call this twice for one map UUID without server-side
        // reconciliation. A normal return means submitted, not stored.
        send_invoked = true;
        reinterpret_cast<void (*)(void*, void*)>(packet_abi.send_to_server)(
            sender, packet.data());
    } catch (...) {
        if (!send_invoked) {
            CancelMapChestClientSyncWindow(request.capture_token);
            return fail(error, "native chest Place could not be constructed");
        }
        // Once sendToServer was invoked its outcome is uncertain. Returning
        // the ID prevents a caller from interpreting this as safe to retry.
        submission_uncertain = true;
        if (error) *error = "chest Place submission outcome is unknown; reconcile before retry";
    }
    submission->request_id = submitted_id;
    submission->response_session_generation = response_generation;
    submission->response_generation_before_send = response_generation_before_send;
    submission->submission_outcome_uncertain = submission_uncertain;
    LogProjectionPrinterInventoryDiagnostic(
        "map_chest Place invoked id=%d window=%u source=%d dest=%u uncertain=%d",
        submitted_id, static_cast<unsigned int>(request.expected_window_id),
        request.source_hotbar_slot,
        static_cast<unsigned int>(request.destination_slot),
        submission_uncertain ? 1 : 0);
    if (error && !submission_uncertain) error->clear();
    return true;
#endif
}

bool SendMapAnvilInputPlace(const MapAnvilInputPlaceRequest& request,
                            MapAnvilInputSubmission* submission,
                            std::string* error) {
    if (submission) *submission = {};
    if (error) error->clear();
#if !defined(__aarch64__)
    (void)request;
    return fail(error, "anvil input Place is supported only on arm64-v8a");
#else
    if (!submission || !IsMinecraftUpdateGameThread()) {
        return fail(error, "anvil input Place requires a local-player tick");
    }
    MapAnvilInputDraft draft;
    if (!loadAnvilInputEvidence(request, &draft, error)) return false;

    const uintptr_t module_base = Main::getBaseAddress();
    if (!module_base) return fail(error, "libminecraftpe.so is not loaded");
    ResolvedAbi abi;
    if (!resolveAbi(module_base, &abi)) {
        return fail(error, "anvil input Place ABI does not match this game version");
    }
    void* const player = GetLocalPlayerPointer();
    void* player_container = nullptr;
    if (!getPlayerInventoryContainer(abi, player, &player_container, error)) return false;
    const void* source_stack = nullptr;
    if (!getLiveInventoryStack(module_base, player_container,
                               request.task.source_hotbar_slot,
                               &source_stack, error)) return false;
    NetIdVariant source_variant;
    if (!copyVariant(source_stack, &source_variant) ||
        !isOrdinaryNetworkVariant(source_variant,
                                  request.task.source_network_stack_id)) {
        return fail(error, "the held map ItemStack changed before anvil Place");
    }
    NetIdVariant destination_variant;
    reinterpret_cast<DefaultNetIdVariant>(abi.default_net_id_variant)(
        &destination_variant);
    if (!isDefaultEmptyNetworkVariant(destination_variant)) {
        return fail(error, "the native empty anvil-slot variant is invalid");
    }
    SlotInfo source_info;
    SlotInfo destination_info;
    if (!makeSlotInfo(draft.source_container, draft.source_slot,
                      source_variant, &source_info) ||
        !makeSlotInfo(draft.destination_container, draft.destination_slot,
                      destination_variant, &destination_info)) {
        return fail(error, "unable to prepare anvil input request slots");
    }

    NativeRequestPacketAbi packet_abi;
    if (!resolveRequestPacketAbi(module_base, &packet_abi, error)) return false;
    void* const sender = GetCapturedLoopbackPacketSender();
    if (!sender) return fail(error, "LoopbackPacketSender is not ready");

    // Recheck all authoritative state immediately before reserving a native
    // request ID. No UI is opened here; the caller owns the packet-only
    // window and must keep its capture token alive until this returns.
    MapAnvilInputDraft final_draft;
    if (!loadAnvilInputEvidence(request, &final_draft, error) ||
        final_draft.source_container != draft.source_container ||
        final_draft.source_slot != draft.source_slot ||
        final_draft.source_network_stack_id != draft.source_network_stack_id ||
        final_draft.destination_container != draft.destination_container ||
        final_draft.destination_slot != draft.destination_slot ||
        final_draft.amount != draft.amount ||
        final_draft.expected_title != draft.expected_title) {
        return fail(error, "the anvil input changed before submission");
    }
    // The second high-level snapshot does not retain the native ItemStack
    // pointer. Verify its exact network identity once more before handoff.
    const void* final_source_stack = nullptr;
    NetIdVariant final_source_variant;
    if (!getLiveInventoryStack(module_base, player_container,
                               request.task.source_hotbar_slot,
                               &final_source_stack, error) ||
        !copyVariant(final_source_stack, &final_source_variant) ||
        !isOrdinaryNetworkVariant(final_source_variant,
                                  request.task.source_network_stack_id)) {
        return fail(error, "the held map changed before anvil packet construction");
    }
    source_info.net_id_variant = final_source_variant;

    int32_t counter = 0;
    std::memcpy(&counter, reinterpret_cast<const void*>(packet_abi.counter),
                sizeof(counter));
    const int32_t normalized = counter < -1 ? counter : -1;
    if ((static_cast<uint32_t>(normalized) & 1U) == 0U ||
        normalized < std::numeric_limits<int32_t>::min() + 2) {
        return fail(error, "the native anvil request counter is invalid or exhausted");
    }
    const int32_t submitted_id = normalized - 2;
    std::memcpy(reinterpret_cast<void*>(packet_abi.counter), &submitted_id,
                sizeof(submitted_id));
    struct alignas(8) NativeRequestId {
        uintptr_t vtable;
        int32_t value;
        uint32_t padding;
    };
    static_assert(sizeof(NativeRequestId) == 0x10U,
                  "unexpected native request ID ABI");
    const NativeRequestId native_id{
        abi.item_stack_request_vtable, submitted_id, 0U};
    const uint64_t response_session_generation =
        GetProjectionPrinterInventoryMailboxSessionGeneration();
    if (!response_session_generation ||
        response_session_generation != request.task.expected_session_generation) {
        return fail(error, "anvil input response session changed before submission");
    }
    ProjectionPrinterInventoryResponse prior_response;
    const uint64_t response_generation_before_send =
        GetProjectionPrinterInventoryResponse(&prior_response) &&
            prior_response.session_generation == response_session_generation
            ? prior_response.response_generation : 0U;

    bool send_invoked = false;
    bool submission_uncertain = false;
    try {
        NativeRequestObjectOwner action(packet_abi.deallocate,
                                         module_base + kActionCompleteDestructorRva);
        action.allocate(packet_abi.allocate, kActionAllocationSize);
        if (!action.get()) return fail(error, "unable to allocate anvil Place action");
        reinterpret_cast<PlaceActionConstructor>(abi.place_action_constructor)(
            action.get(), draft.amount, &source_info, &destination_info);
        action.markConstructed();
        if (!objectHasVtable(action.get(), abi.place_action_vtable)) {
            return fail(error, "the native anvil Place action vtable changed");
        }

        NativeRequestObjectOwner data(packet_abi.deallocate,
                                      packet_abi.data_destructor);
        data.allocate(packet_abi.allocate, kRequestDataSize);
        if (!data.get()) return fail(error, "unable to allocate anvil request data");
        reinterpret_cast<void (*)(void*, const NativeRequestId*)>(
            packet_abi.data_constructor)(data.get(), &native_id);
        data.markConstructed();
        if (!objectHasVtable(data.get(), abi.item_stack_request_vtable)) {
            return fail(error, "the native anvil request data vtable changed");
        }
        int32_t constructed_id = 0;
        std::memcpy(&constructed_id,
                    static_cast<const uint8_t*>(data.get()) + 0x08U,
                    sizeof(constructed_id));
        if (constructed_id != submitted_id) {
            return fail(error, "the native anvil request data lost its request ID");
        }
        reinterpret_cast<void (*)(void*, void**)>(packet_abi.data_add_action)(
            data.get(), action.ownerAddress());
        if (action.get()) return fail(error, "the anvil request did not accept Place");

        NativeRequestObjectOwner batch(packet_abi.deallocate,
                                       packet_abi.batch_destructor);
        batch.allocate(packet_abi.allocate, kRequestBatchSize);
        if (!batch.get()) return fail(error, "unable to allocate anvil request batch");
        reinterpret_cast<void (*)(void*)>(packet_abi.batch_constructor)(batch.get());
        batch.markConstructed();
        reinterpret_cast<void (*)(void*, void**)>(packet_abi.batch_add_request)(
            batch.get(), data.ownerAddress());
        if (data.get()) return fail(error, "the anvil batch did not accept its request");

        alignas(8) std::array<uint8_t, kRequestPacketSize> packet{};
        reinterpret_cast<void (*)(void*, void**)>(packet_abi.packet_constructor)(
            packet.data(), batch.ownerAddress());
        NativeCompleteDestructorGuard packet_guard(
            packet.data(), packet_abi.packet_destructor);
        if (batch.get() || !objectHasVtable(packet.data(), packet_abi.packet_vtable)) {
            return fail(error, "the native anvil packet did not accept its batch");
        }
        ContainerCaptureResult dispatch_window;
        if (PollContainerCapture(request.task.window_token, &dispatch_window) !=
                ContainerCapturePollState::Ready ||
            dispatch_window.container_closed ||
            dispatch_window.container_id != request.task.expected_window_id ||
            dispatch_window.container_type != 5U ||
            dispatch_window.slot_count != 3U ||
            !dispatch_window.items.empty() ||
            GetProjectionPrinterInventoryMailboxSessionGeneration() !=
                response_session_generation) {
            return fail(error, "the anvil input window changed before journal dispatch");
        }
        // The manual request's Container(0)/slot 1 is NOT the physical
        // InventorySlot index. The native anvil manager reads input at 0,
        // material at 1, and the three-slot window reads output at 2. Use
        // physical 0 only for a one-shot, client-only presentation correction
        // after the exact server response is durably accepted. No packet is
        // delivered before that acceptance and native readback remains the
        // only proof that the correction reached the live input model.
        constexpr uint8_t kAnvilInputPhysicalSlot = 0U;
        MapAnvilClientSyncCandidate client_sync;
        client_sync.request_id = submitted_id;
        client_sync.session_generation = response_session_generation;
        client_sync.response_generation_before_send =
            response_generation_before_send;
        client_sync.window_token = request.task.window_token;
        client_sync.window_id = request.task.expected_window_id;
        client_sync.source_hotbar_slot =
            static_cast<uint8_t>(request.task.source_hotbar_slot);
        client_sync.anvil_physical_slot_explicit = true;
        client_sync.anvil_physical_slot = kAnvilInputPhysicalSlot;
        client_sync.runtime_item_id = request.task.expected_runtime_item_id;
        client_sync.source_network_stack_id =
            request.task.source_network_stack_id;
        client_sync.map_uuid = request.task.expected_map_uuid;
        if (!CaptureMapAnvilLocalInputSlotPackets(
                request.task.source_hotbar_slot,
                request.task.source_network_stack_id,
                request.task.expected_runtime_item_id,
                request.task.expected_map_uuid,
                request.task.expected_window_id,
                kAnvilInputPhysicalSlot,
                &client_sync.occupied_source_packet,
                &client_sync.occupied_anvil_packet, error)) return false;
        if (!PrepareMapAnvilClientSyncCandidate(
                std::move(client_sync), error)) return false;
        LogProjectionPrinterInventoryDiagnostic(
            "map_anvil local_prepared request=%d window=%u physical_input=%u source=%d net=%d",
            submitted_id,
            static_cast<unsigned>(request.task.expected_window_id),
            static_cast<unsigned>(kAnvilInputPhysicalSlot),
            request.task.source_hotbar_slot,
            request.task.source_network_stack_id);
        if (!ArmMapAnvilInputDispatchBeforeSend(request, submitted_id, error)) {
            CancelMapAnvilClientSyncWindow(request.task.window_token);
            LogProjectionPrinterInventoryDiagnostic(
                "map_anvil input Place aborted before send id=%d journal=failed",
                submitted_id);
            return false;
        }
        send_invoked = true;
        reinterpret_cast<void (*)(void*, void*)>(packet_abi.send_to_server)(
            sender, packet.data());
    } catch (...) {
        if (!send_invoked) {
            return fail(error, "native anvil input Place could not be constructed");
        }
        submission_uncertain = true;
        if (error) *error =
            "anvil input submission outcome is unknown; reconcile before retry";
    }
    submission->request_id = submitted_id;
    submission->response_session_generation = response_session_generation;
    submission->response_generation_before_send = response_generation_before_send;
    submission->submission_outcome_uncertain = submission_uncertain;
    LogProjectionPrinterInventoryDiagnostic(
        "map_anvil input Place invoked id=%d window=%u hotbar=%d uncertain=%d",
        submitted_id, static_cast<unsigned int>(request.task.expected_window_id),
        request.task.source_hotbar_slot, submission_uncertain ? 1 : 0);
    if (error && !submission_uncertain) error->clear();
    return true;
#endif
}

}  // namespace build_import
