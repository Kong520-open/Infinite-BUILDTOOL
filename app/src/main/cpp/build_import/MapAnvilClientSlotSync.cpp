#include "MapAnvilClientSlotSync.h"

#include "ProjectionPrinterInventoryClientSync.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace build_import {
namespace {

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

void appendVarUInt(std::string* output, uint32_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7FU);
        value >>= 7U;
        if (value != 0U) byte |= 0x80U;
        output->push_back(static_cast<char>(byte));
    } while (value != 0U);
}

void appendVarInt(std::string* output, int32_t value) {
    const uint32_t sign_mask = value < 0 ?
        std::numeric_limits<uint32_t>::max() : 0U;
    appendVarUInt(output, (static_cast<uint32_t>(value) << 1U) ^ sign_mask);
}

bool inBounds(std::string_view packet, size_t offset, size_t length) {
    return offset <= packet.size() && length <= packet.size() - offset;
}

}  // namespace

bool BuildMapAnvilClientInputSlotRefresh(
    const MapAnvilClientSlotSyncRequest& request,
    std::string_view occupied_source_packet,
    std::string_view occupied_anvil_packet,
    MapAnvilClientSlotSyncPackets* output,
    std::string* error) {
    if (output) *output = {};
    if (!output || request.source_hotbar_slot > 8U ||
        request.anvil_window_id == 0U || request.anvil_window_id == 0xFFU ||
        request.expected_runtime_item_id <= 0 ||
        request.expected_source_network_stack_id <= 0 ||
        request.expected_map_uuid == -1 ||
        request.confirmed_destination_network_stack_id <= 0) {
        return fail(error, "anvil client input refresh request is invalid");
    }
    try {
        ProjectionPrinterNativeMapSlotPacket source;
        ProjectionPrinterNativeMapSlotPacket destination;
        if (!DecodeProjectionPrinterNativeMapSlotPacket(
                occupied_source_packet, 0U, request.source_hotbar_slot,
                &source) ||
            !DecodeProjectionPrinterNativeMapSlotPacket(
                occupied_anvil_packet, request.anvil_window_id,
                request.anvil_physical_slot, &destination)) {
            return fail(error, "anvil client input InventorySlot envelope is invalid");
        }
        if (source.wire_header != destination.wire_header ||
            source.item.runtime_item_id != request.expected_runtime_item_id ||
            destination.item.runtime_item_id != request.expected_runtime_item_id ||
            source.item.count != 1U || destination.item.count != 1U ||
            source.item.aux != destination.item.aux ||
            source.item.network_stack_id !=
                request.expected_source_network_stack_id ||
            destination.item.network_stack_id !=
                request.expected_source_network_stack_id ||
            source.metadata.map_uuid != request.expected_map_uuid ||
            destination.metadata.map_uuid != request.expected_map_uuid) {
            return fail(error, "anvil client input map identity changed");
        }
        if (!inBounds(occupied_source_packet, source.item_data_offset,
                      source.item_data_length) ||
            !inBounds(occupied_anvil_packet, destination.item_data_offset,
                      destination.item_data_length) ||
            !inBounds(occupied_anvil_packet,
                      destination.network_stack_id_offset,
                      destination.network_stack_id_length) ||
            source.item_data_length != destination.item_data_length ||
            occupied_source_packet.substr(source.item_data_offset,
                                          source.item_data_length) !=
                occupied_anvil_packet.substr(destination.item_data_offset,
                                             destination.item_data_length)) {
            return fail(error, "anvil client input ItemData pre-images differ");
        }

        MapAnvilClientSlotSyncPackets built;
        built.empty_source_packet.assign(
            occupied_source_packet.data(), source.item_data_offset);
        built.empty_source_packet.push_back('\0');  // zigzag-varint air ID 0
        built.occupied_anvil_input_packet.assign(occupied_anvil_packet);
        if (request.confirmed_destination_network_stack_id !=
            request.expected_source_network_stack_id) {
            std::string confirmed_id;
            appendVarInt(&confirmed_id,
                         request.confirmed_destination_network_stack_id);
            built.occupied_anvil_input_packet.replace(
                destination.network_stack_id_offset,
                destination.network_stack_id_length, confirmed_id);
        }

        // Verify the finished pair with the same strict decoder. This catches
        // offset or varint-length mistakes before the caller could present it.
        ProjectionPrinterNativeSlotItem empty_source;
        ProjectionPrinterNativeMapSlotPacket filled_destination;
        if (!DecodeProjectionPrinterNativeInventorySlotItem(
                built.empty_source_packet, request.source_hotbar_slot,
                &empty_source) || empty_source.occupied ||
            !DecodeProjectionPrinterNativeMapSlotPacket(
                built.occupied_anvil_input_packet,
                request.anvil_window_id, request.anvil_physical_slot,
                &filled_destination) ||
            filled_destination.item.runtime_item_id !=
                request.expected_runtime_item_id ||
            filled_destination.item.count != 1U ||
            filled_destination.item.network_stack_id !=
                request.confirmed_destination_network_stack_id ||
            filled_destination.metadata.map_uuid !=
                request.expected_map_uuid) {
            return fail(error, "anvil client input transformed packet is invalid");
        }
        *output = std::move(built);
        if (error) error->clear();
        return true;
    } catch (...) {
        *output = {};
        return fail(error, "anvil client input refresh allocation failed");
    }
}

}  // namespace build_import
