#include "MapAnvilRenameSender.h"

#include "MapTileNaming.h"

#include <utility>

namespace build_import {
namespace {

bool refuse(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

bool isFilledMapIdentifier(const std::string& identifier) {
    return identifier == "minecraft:map" || identifier == "map" ||
           identifier == "minecraft:filled_map" || identifier == "filled_map" ||
           identifier == "minecraft:locator_map" || identifier == "locator_map";
}

bool isAnvilIdentifier(const std::string& identifier) {
    return identifier == "minecraft:anvil" || identifier == "anvil" ||
           identifier == "minecraft:chipped_anvil" ||
           identifier == "chipped_anvil" ||
           identifier == "minecraft:damaged_anvil" ||
           identifier == "damaged_anvil";
}

bool validateCommon(const MapAnvilRenameTask& task,
                    const MapAnvilRenameWorldEvidence& world,
                    const MapAnvilRenameWindowEvidence& window,
                    std::string* title, std::string* error) {
    if (!title || task.expected_world_context.empty() ||
        task.expected_world_context == "unknown" ||
        task.expected_session_generation == 0 || task.window_token == 0 ||
        task.recipe_ticket == 0 || task.expected_window_id == 0 ||
        task.expected_window_id == 0xFFU || task.expected_runtime_item_id <= 0 ||
        task.source_hotbar_slot < 0 || task.source_hotbar_slot > 8 ||
        task.source_network_stack_id <= 0 ||
        task.expected_map_uuid == -1) {
        return refuse(error, "the anvil rename task has an invalid identity");
    }
    if (world.world_context != task.expected_world_context ||
        !isAnvilIdentifier(world.block_identifier)) {
        return refuse(error, "the designated anvil or live world changed");
    }
    if (window.session_generation != task.expected_session_generation ||
        window.token != task.window_token ||
        window.window_id != task.expected_window_id ||
        window.container_type != 5U ||
        window.x != task.anvil_x || window.y != task.anvil_y ||
        window.z != task.anvil_z || !window.opened || window.closed ||
        !window.content_observed || window.slot_count != 3U) {
        return refuse(error, "the fresh three-slot anvil window is unavailable");
    }
    if (!FormatMapTileName(task.tile_cursor, task.columns, task.rows, title)) {
        return refuse(error, "the requested row-column map title is invalid");
    }
    return true;
}

}  // namespace

bool PrepareMapAnvilInputDraft(const MapAnvilRenameTask& task,
                               const MapAnvilRenameWorldEvidence& world,
                               const MapAnvilRenameWindowEvidence& window,
                               const MapAnvilHeldMapEvidence& held,
                               MapAnvilInputDraft* draft,
                               std::string* error) {
    if (!draft) return refuse(error, "the anvil input draft output is unavailable");
    *draft = {};
    std::string title;
    if (!validateCommon(task, world, window, &title, error)) return false;
    if (!window.input_slot_empty || task.source_hotbar_slot < 0 ||
        task.source_hotbar_slot > 8 || task.source_network_stack_id <= 0 ||
        held.selected_hotbar_slot != task.source_hotbar_slot ||
        held.network_stack_id != task.source_network_stack_id ||
        held.runtime_item_id != task.expected_runtime_item_id ||
        held.count != 1U || !isFilledMapIdentifier(held.item_identifier) ||
        !held.has_map_uuid || held.map_uuid != task.expected_map_uuid) {
        return refuse(error, "the unstacked held map or empty anvil input changed");
    }
    MapAnvilInputDraft prepared;
    prepared.source_slot = static_cast<uint8_t>(task.source_hotbar_slot);
    prepared.source_network_stack_id = task.source_network_stack_id;
    prepared.expected_title = std::move(title);
    *draft = std::move(prepared);
    if (error) error->clear();
    return true;
}

bool ArmMapAnvilInputDispatchBeforeSend(
    const MapAnvilInputPlaceRequest& request, int32_t request_id,
    std::string* error) {
    if (!request.pre_send || request_id >= 0 ||
        (static_cast<uint32_t>(request_id) & 1U) == 0U) {
        return refuse(error, "the anvil input journal callback or request ID is invalid");
    }
    try {
        if (!request.pre_send(request_id, request.pre_send_context, error)) {
            if (error && error->empty()) {
                *error = "the anvil input DispatchArmed journal write failed";
            }
            return false;
        }
    } catch (...) {
        return refuse(error, "the anvil input DispatchArmed journal callback threw");
    }
    if (error) error->clear();
    return true;
}

bool ValidateMapAnvilInputAcceptedResponse(
    const MapAnvilInputPlaceRequest& request,
    const MapAnvilInputSubmission& submission,
    const ProjectionPrinterInventoryResponse& response,
    std::string* error) {
    const auto refuse_input = [error](const char* reason) {
        if (error) *error = reason;
        return false;
    };
    if (request.task.source_hotbar_slot < 0 ||
        request.task.source_hotbar_slot > 8 ||
        request.task.source_network_stack_id <= 0 ||
        request.task.expected_map_uuid == -1 ||
        submission.request_id >= 0 ||
        (static_cast<uint32_t>(submission.request_id) & 1U) == 0U ||
        submission.response_session_generation == 0U ||
        submission.response_session_generation !=
            request.task.expected_session_generation ||
        response.request_id != submission.request_id ||
        response.session_generation != submission.response_session_generation ||
        response.response_generation <=
            submission.response_generation_before_send) {
        return refuse_input("the anvil input response is stale or belongs to another request");
    }
    if (!response.valid || response.rejected || response.status != 0U ||
        response.layout !=
            ProjectionPrinterInventoryResponseLayout::
                V859SlotHotbarSlotAmountNetworkId ||
        response.entry_count != 1U ||
        response.successful_entry_count != 1U ||
        response.slots.size() != 2U) {
        return refuse_input("the anvil input Place was rejected or has an unknown response layout");
    }
    const ProjectionPrinterInventoryResponseSlot* source = nullptr;
    const ProjectionPrinterInventoryResponseSlot* destination = nullptr;
    for (const ProjectionPrinterInventoryResponseSlot& slot : response.slots) {
        if (slot.has_dynamic_container_id || slot.dynamic_container_id != 0U) {
            return refuse_input("the anvil input response used a dynamic container");
        }
        if (slot.container_id == 29U &&
            slot.slot == static_cast<uint8_t>(request.task.source_hotbar_slot)) {
            if (source) return refuse_input("the anvil input response repeats its source slot");
            source = &slot;
        } else if (slot.container_id == 0U && slot.slot == 1U) {
            if (destination) {
                return refuse_input("the anvil input response repeats its destination slot");
            }
            destination = &slot;
        } else {
            return refuse_input("the anvil input response contains an unexpected slot");
        }
    }
    if (!source || !destination || source->count != 0U ||
        source->network_stack_id != 0 || destination->count != 1U ||
        destination->network_stack_id <= 0) {
        return refuse_input("the anvil input response did not confirm a populated input slot");
    }
    if (error) error->clear();
    return true;
}

bool ValidateMapAnvilCraftAcceptedResponse(
    const MapAnvilRenameTask& task,
    const MapAnvilCraftSubmission& submission,
    const ProjectionPrinterInventoryResponse& response,
    std::string* error,
    int32_t* accepted_output_network_stack_id) {
    if (accepted_output_network_stack_id) {
        *accepted_output_network_stack_id = 0;
    }
    const auto refuse_craft = [error](const char* reason) {
        if (error) *error = reason;
        return false;
    };
    if (task.expected_session_generation == 0U ||
        task.expected_map_uuid == -1 ||
        task.window_token == 0U || task.expected_window_id == 0U ||
        task.expected_window_id == 0xFFU ||
        submission.request_id >= 0 ||
        (static_cast<uint32_t>(submission.request_id) & 1U) == 0U ||
        submission.response_session_generation !=
            task.expected_session_generation ||
        submission.window_token != task.window_token ||
        submission.window_id != task.expected_window_id ||
        submission.destination_hotbar_slot < 0 ||
        submission.destination_hotbar_slot > 8 ||
        response.request_id != submission.request_id ||
        response.session_generation != submission.response_session_generation ||
        response.response_generation <=
            submission.response_generation_before_send) {
        return refuse_craft("the anvil craft response is stale or belongs to another request");
    }
    std::string expected_title;
    if (!FormatMapTileName(task.tile_cursor, task.columns, task.rows,
                           &expected_title)) {
        return refuse_craft("the anvil craft target title is invalid");
    }
    if (!response.valid || response.rejected || response.status != 0U ||
        response.layout !=
            ProjectionPrinterInventoryResponseLayout::
                V859SlotHotbarSlotAmountNetworkId ||
        response.entry_count != 1U ||
        response.successful_entry_count != 1U ||
        response.slots.size() != 2U) {
        return refuse_craft("the anvil craft was rejected or has an ambiguous response layout");
    }
    const ProjectionPrinterInventoryResponseSlot* input = nullptr;
    const ProjectionPrinterInventoryResponseSlot* output = nullptr;
    for (const ProjectionPrinterInventoryResponseSlot& slot : response.slots) {
        if (slot.has_dynamic_container_id || slot.dynamic_container_id != 0U) {
            return refuse_craft("the anvil craft response used a dynamic container");
        }
        if (slot.container_id == 0U && slot.slot == 1U) {
            if (input) return refuse_craft("the anvil craft response repeats its input slot");
            input = &slot;
        } else if (slot.container_id == 12U &&
                   slot.slot == submission.destination_hotbar_slot) {
            if (output) return refuse_craft("the anvil craft response repeats its output slot");
            output = &slot;
        } else {
            return refuse_craft("the anvil craft response contains an unexpected slot");
        }
    }
    if (!input || !output || input->count != 0U ||
        input->network_stack_id != 0 || output->count != 1U ||
        output->network_stack_id <= 0 ||
        output->hotbar_slot != submission.destination_hotbar_slot ||
        output->custom_name != expected_title) {
        return refuse_craft("the anvil craft response did not confirm the expected title");
    }
    if (accepted_output_network_stack_id) {
        *accepted_output_network_stack_id = output->network_stack_id;
    }
    if (error) error->clear();
    return true;
}

bool PrepareMapAnvilCraftDraft(const MapAnvilRenameTask& task,
                               const MapAnvilRenameWorldEvidence& world,
                               const MapAnvilRenameWindowEvidence& window,
                               const MapAnvilInputMapEvidence& input,
                               MapAnvilRecipeProbeStatus probe_status,
                               const MapAnvilRecipeObservation& recipe,
                               MapAnvilCraftDraft* draft,
                               std::string* error) {
    if (!draft) return refuse(error, "the anvil craft draft output is unavailable");
    *draft = {};
    std::string title;
    if (!validateCommon(task, world, window, &title, error)) return false;
    if (window.input_slot_empty ||
        input.session_generation != task.expected_session_generation ||
        input.window_token != task.window_token ||
        input.window_id != task.expected_window_id || input.slot != 1U ||
        input.runtime_item_id != task.expected_runtime_item_id ||
        input.network_stack_id <= 0 ||
        input.count != 1U ||
        !input.has_map_uuid || input.map_uuid != task.expected_map_uuid) {
        return refuse(error, "the map in this anvil input was not confirmed");
    }
    if (probe_status != MapAnvilRecipeProbeStatus::Observed ||
        recipe.ticket != task.recipe_ticket ||
        recipe.window_id != task.expected_window_id ||
        recipe.map_uuid != task.expected_map_uuid ||
        recipe.recipe_network_id == 0U) {
        return refuse(error, "a matching live recipe lookup has not been observed");
    }
    // Live rename requests left the old map-items getter and both copy sites
    // untouched. Do not promote that unrelated observation to rename proof.
    return refuse(error,
        "the actual anvil rename recipe source is not verified");
}

bool SendMapAnvilRename(const MapAnvilCraftDraft& draft,
                        std::string* error) {
    (void)draft;
    return refuse(error,
        "automatic anvil rename is disabled: Consume/output native ABI is unverified");
}

}  // namespace build_import
