#include "MapChestStorageSender.h"

namespace build_import {

bool ValidateFilledMapSingleChestPreflight(
    const FilledMapToSingleChestRequest& request,
    const ContainerCaptureResult& capture,
    const FilledMapChestSourceEvidence& source,
    const FilledMapChestWorldEvidence& world,
    std::string* error) {
    const auto refuse = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (!request.pre_send) {
        return refuse("a durable pre-send journal callback is required");
    }
    if (request.expected_world_context.empty() ||
        request.expected_world_context == "unknown" ||
        world.world_context != request.expected_world_context) {
        return refuse("the current world no longer matches the chest task");
    }
    if (world.block_identifier != "minecraft:chest" &&
        world.block_identifier != "chest") {
        return refuse("the designated block is not an ordinary chest");
    }
    if (request.source_hotbar_slot < 0 || request.source_hotbar_slot > 8 ||
        request.source_network_stack_id <= 0 ||
        request.expected_runtime_item_id <= 0 ||
        request.expected_map_uuid == -1 ||
        source.selected_hotbar_slot != request.source_hotbar_slot ||
        source.network_stack_id != request.source_network_stack_id ||
        source.runtime_item_id != request.expected_runtime_item_id ||
        source.count != 1U || !source.has_map_uuid ||
        source.map_uuid != request.expected_map_uuid) {
        return refuse("the held filled-map stack changed before chest storage");
    }
    const std::string& identifier = source.item_identifier;
    if (identifier != "minecraft:map" && identifier != "map" &&
        identifier != "minecraft:filled_map" && identifier != "filled_map" &&
        identifier != "minecraft:locator_map" && identifier != "locator_map") {
        return refuse("the held item is not a resolved filled map");
    }
    if (request.expected_window_id == 0U ||
        request.expected_window_id == 0xFFU ||
        capture.container_id != request.expected_window_id ||
        request.destination_slot >= 27U) {
        return refuse("the chest window or destination slot changed");
    }
    uint8_t first_empty_slot = 0;
    if (!SelectEmptySingleChestSlot(capture, request.capture_token,
                                    request.chest_x, request.chest_y,
                                    request.chest_z, &first_empty_slot, error)) {
        return false;
    }
    if (first_empty_slot != request.destination_slot) {
        return refuse("the selected chest slot is no longer the first empty slot");
    }
    for (const CapturedContainerItem& item : capture.items) {
        if (item.has_map_uuid && item.map_uuid == request.expected_map_uuid) {
            return refuse("the same map UUID is already present in the chest");
        }
    }
    if (error) error->clear();
    return true;
}

bool ArmFilledMapChestDispatchBeforeSend(
    const FilledMapToSingleChestRequest& request, int32_t request_id,
    std::string* error) {
    if (!request.pre_send || request_id >= 0 ||
        (static_cast<uint32_t>(request_id) & 1U) == 0U) {
        if (error) *error = "the journal callback or native request ID is invalid";
        return false;
    }
    try {
        if (!request.pre_send(request_id, request.pre_send_context, error)) {
            if (error && error->empty()) {
                *error = "the map-chest DispatchArmed journal write failed";
            }
            return false;
        }
    } catch (...) {
        if (error) *error = "the map-chest DispatchArmed journal callback threw";
        return false;
    }
    if (error) error->clear();
    return true;
}

bool ValidateFilledMapChestAcceptedResponse(
    const FilledMapToSingleChestRequest& request,
    const FilledMapChestSubmission& submission,
    const ProjectionPrinterInventoryResponse& response,
    std::string_view expected_item_name, std::string* error) {
    const auto refuse = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (submission.request_id >= 0 ||
        (static_cast<uint32_t>(submission.request_id) & 1U) == 0U ||
        submission.response_session_generation == 0U ||
        response.request_id != submission.request_id ||
        response.session_generation != submission.response_session_generation ||
        response.response_generation <= submission.response_generation_before_send) {
        return refuse("the chest Place response is stale or belongs to another request");
    }
    if (!response.valid || response.rejected || response.status != 0U ||
        response.layout !=
            ProjectionPrinterInventoryResponseLayout::V859SlotHotbarSlotAmountNetworkId) {
        return refuse("the chest Place response was rejected or used an unknown layout");
    }
    const ProjectionPrinterInventoryResponseSlot* source = nullptr;
    const ProjectionPrinterInventoryResponseSlot* destination = nullptr;
    for (const ProjectionPrinterInventoryResponseSlot& slot : response.slots) {
        if (slot.has_dynamic_container_id || slot.dynamic_container_id != 0U) {
            return refuse("the chest Place response refers to a dynamic container");
        }
        if (slot.container_id == 29U &&
            slot.slot == static_cast<uint8_t>(request.source_hotbar_slot)) {
            if (source) return refuse("the chest Place response repeats its source slot");
            source = &slot;
        }
        if (slot.container_id == 7U &&
            slot.slot == request.destination_slot) {
            if (destination) return refuse("the chest Place response repeats its destination slot");
            destination = &slot;
        }
    }
    if (!source || !destination || source->count != 0U ||
        destination->count != 1U || destination->network_stack_id <= 0) {
        return refuse("the chest Place response did not confirm source and destination slots");
    }
    if (!expected_item_name.empty() && destination->custom_name != expected_item_name) {
        return refuse("the stored map name differs from the expected row and column");
    }
    if (error) error->clear();
    return true;
}

}  // namespace build_import
