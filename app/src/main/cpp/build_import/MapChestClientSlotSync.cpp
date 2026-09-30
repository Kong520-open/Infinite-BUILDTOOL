#include "MapChestClientSlotSync.h"

#include "ProjectionPrinterInventoryClientSync.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace build_import {
namespace {

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

}  // namespace

bool BuildMapChestClientSourceSlotRefresh(
    const MapChestClientSlotSyncRequest& request,
    std::string_view occupied_source_packet,
    std::string* empty_source_packet,
    std::string* error) {
    if (empty_source_packet) empty_source_packet->clear();
    if (!empty_source_packet || request.source_hotbar_slot > 8U ||
        request.expected_runtime_item_id <= 0 ||
        request.expected_source_network_stack_id <= 0 ||
        request.expected_map_uuid == -1) {
        return fail(error, "chest client source-slot refresh request is invalid");
    }
    try {
        ProjectionPrinterNativeMapSlotPacket source;
        if (!DecodeProjectionPrinterNativeMapSlotPacket(
                occupied_source_packet, 0U, request.source_hotbar_slot,
                &source)) {
            return fail(error, "chest client source InventorySlot envelope is invalid");
        }
        if (!source.item.occupied || source.item.count != 1U ||
            source.item.runtime_item_id != request.expected_runtime_item_id ||
            !source.item.has_network_stack_id ||
            source.item.network_stack_id !=
                request.expected_source_network_stack_id ||
            !source.metadata.has_map_uuid ||
            source.metadata.map_uuid != request.expected_map_uuid) {
            return fail(error, "chest client source map identity changed");
        }
        if (source.item_data_offset >= occupied_source_packet.size() ||
            source.item_data_length !=
                occupied_source_packet.size() - source.item_data_offset) {
            return fail(error, "chest client source ItemData extent is invalid");
        }

        std::string built(occupied_source_packet.substr(0U,
                                                        source.item_data_offset));
        built.push_back('\0');  // signed zigzag-varint runtime air ID 0

        ProjectionPrinterNativeSlotItem empty;
        if (!DecodeProjectionPrinterNativeInventorySlotItem(
                built, request.source_hotbar_slot, &empty) ||
            empty.occupied || empty.runtime_item_id != 0 ||
            empty.count != 0U || empty.has_network_stack_id ||
            empty.network_stack_id != 0) {
            return fail(error, "chest client source empty-slot packet is invalid");
        }
        *empty_source_packet = std::move(built);
        if (error) error->clear();
        return true;
    } catch (...) {
        empty_source_packet->clear();
        return fail(error, "chest client source-slot refresh allocation failed");
    }
}

}  // namespace build_import
