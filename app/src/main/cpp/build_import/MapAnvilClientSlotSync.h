#ifndef INFINITE_TEXTURE_MAP_ANVIL_CLIENT_SLOT_SYNC_H
#define INFINITE_TEXTURE_MAP_ANVIL_CLIENT_SLOT_SYNC_H

#include <cstdint>
#include <string>
#include <string_view>

namespace build_import {

// A pure byte transformation for presenting an already accepted anvil-input
// Place to the local client. The two occupied packets must have been written
// from the SAME live map stack by the game's v859 InventorySlot serializer.
// This does not send, queue, inject, or modify any game inventory state.
struct MapAnvilClientSlotSyncRequest {
    uint8_t source_hotbar_slot = 0;       // player inventory ID 0, slot 0..8
    uint8_t anvil_window_id = 0;         // bound visible window, 1..254
    uint8_t anvil_physical_slot = 0;     // caller-supplied physical slot
    int32_t expected_runtime_item_id = 0;
    int32_t expected_source_network_stack_id = 0;
    int64_t expected_map_uuid = -1;
    int32_t confirmed_destination_network_stack_id = 0;
};

struct MapAnvilClientSlotSyncPackets {
    std::string empty_source_packet;
    std::string occupied_anvil_input_packet;
};

// Verifies both complete packet envelopes and the exact map identity (runtime
// ID, count 1, aux, source net ID, map_uuid, and identical original ItemData).
// The source packet changes ONLY its final ItemData to the empty varint 0;
// the destination changes ONLY the ItemStackNetId varint to the response ID.
// The caller must independently establish server ACK and window lifetime.
bool BuildMapAnvilClientInputSlotRefresh(
    const MapAnvilClientSlotSyncRequest& request,
    std::string_view occupied_source_packet,
    std::string_view occupied_anvil_packet,
    MapAnvilClientSlotSyncPackets* output,
    std::string* error = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_ANVIL_CLIENT_SLOT_SYNC_H
