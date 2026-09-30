#ifndef INFINITE_TEXTURE_MAP_CHEST_CLIENT_SLOT_SYNC_H
#define INFINITE_TEXTURE_MAP_CHEST_CLIENT_SLOT_SYNC_H

#include <cstdint>
#include <string>
#include <string_view>

namespace build_import {

// Binds a local player-slot refresh to the exact map accepted by a chest Place.
// The caller must independently verify the matching server response and chest
// window; this pure byte converter neither sends nor injects packets.
struct MapChestClientSlotSyncRequest {
    uint8_t source_hotbar_slot = 0;  // player inventory ID 0, slot 0..8
    int32_t expected_runtime_item_id = 0;
    int32_t expected_source_network_stack_id = 0;
    int64_t expected_map_uuid = -1;
};

// The input is a COMPLETE InventorySlot packet emitted by the game's own
// v859 serializer from the live occupied source ItemStack. Verify its exact
// inventory/slot/map identity, then replace ONLY its final ItemData with the
// single zero varint for air. Never derive a packet from an ItemStackResponse
// slot namespace or from cached inventory text.
bool BuildMapChestClientSourceSlotRefresh(
    const MapChestClientSlotSyncRequest& request,
    std::string_view occupied_source_packet,
    std::string* empty_source_packet,
    std::string* error = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_CHEST_CLIENT_SLOT_SYNC_H
