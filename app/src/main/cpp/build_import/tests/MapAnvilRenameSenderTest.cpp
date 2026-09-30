#include "../MapAnvilRenameSender.h"

#include <cassert>
#include <stdexcept>
#include <string>

namespace {

struct PreSendState {
    int calls = 0;
    int32_t last_id = 0;
    bool allow = true;
    bool throw_exception = false;
};

bool preSend(int32_t id, void* context, std::string* error) {
    auto* state = static_cast<PreSendState*>(context);
    ++state->calls;
    state->last_id = id;
    if (state->throw_exception) throw std::runtime_error("journal failure");
    if (!state->allow) {
        if (error) *error = "journal refused";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    using namespace build_import;
    MapAnvilRenameTask task;
    task.expected_world_context = "stable:v1:map-test|0";
    task.expected_dimension_token = 0x1234U;
    task.expected_session_generation = 7U;
    task.window_token = 55U;
    task.recipe_ticket = 91U;
    task.expected_window_id = 3U;
    task.anvil_x = 236;
    task.anvil_y = 22;
    task.anvil_z = 126;
    task.source_hotbar_slot = 6;
    task.source_network_stack_id = 71;
    task.expected_runtime_item_id = 358;
    task.expected_map_uuid = -532575944698LL;
    task.tile_cursor = 0U;
    task.columns = 2U;
    task.rows = 2U;

    MapAnvilRenameWorldEvidence world;
    world.world_context = task.expected_world_context;
    world.dimension_token = task.expected_dimension_token;
    world.block_identifier = "minecraft:anvil";

    MapAnvilRenameWindowEvidence window;
    window.session_generation = task.expected_session_generation;
    window.token = task.window_token;
    window.window_id = task.expected_window_id;
    window.container_type = 5U;
    window.x = task.anvil_x;
    window.y = task.anvil_y;
    window.z = task.anvil_z;
    window.opened = true;
    window.content_observed = true;
    window.slot_count = 3U;
    window.input_slot_empty = true;

    MapAnvilHeldMapEvidence held;
    held.selected_hotbar_slot = task.source_hotbar_slot;
    held.network_stack_id = task.source_network_stack_id;
    held.runtime_item_id = task.expected_runtime_item_id;
    held.count = 1U;
    held.item_identifier = "minecraft:map";
    held.has_map_uuid = true;
    held.map_uuid = task.expected_map_uuid;

    MapAnvilInputDraft input_draft;
    std::string error;
    assert(PrepareMapAnvilInputDraft(task, world, window, held,
                                     &input_draft, &error));
    assert(error.empty());
    assert(input_draft.source_container == 29U);
    assert(input_draft.source_slot == 6U);
    assert(input_draft.source_network_stack_id == 71);
    assert(input_draft.destination_container == 0U);
    assert(input_draft.destination_slot == 1U);
    assert(input_draft.amount == 1U);
    assert(input_draft.expected_title == "地图 1行1列");
    assert(input_draft.expected_title.size() == 15U);

    MapAnvilInputPlaceRequest place_request;
    place_request.task = task;
    PreSendState pre_send_state;
    place_request.pre_send = preSend;
    place_request.pre_send_context = &pre_send_state;
    assert(!ArmMapAnvilInputDispatchBeforeSend(place_request, 1, &error));
    assert(!ArmMapAnvilInputDispatchBeforeSend(place_request, -2, &error));
    assert(pre_send_state.calls == 0);
    assert(ArmMapAnvilInputDispatchBeforeSend(place_request, -3, &error));
    assert(error.empty() && pre_send_state.calls == 1 &&
           pre_send_state.last_id == -3);
    pre_send_state.allow = false;
    assert(!ArmMapAnvilInputDispatchBeforeSend(place_request, -5, &error));
    assert(error == "journal refused" && pre_send_state.calls == 2);
    pre_send_state.allow = true;
    pre_send_state.throw_exception = true;
    assert(!ArmMapAnvilInputDispatchBeforeSend(place_request, -7, &error));
    assert(error == "the anvil input DispatchArmed journal callback threw");
    place_request.pre_send = nullptr;
    assert(!ArmMapAnvilInputDispatchBeforeSend(place_request, -9, &error));

    MapAnvilInputSubmission input_submission;
    input_submission.request_id = -3;
    input_submission.response_session_generation = task.expected_session_generation;
    input_submission.response_generation_before_send = 10U;
    ProjectionPrinterInventoryResponse input_response;
    input_response.valid = true;
    input_response.status = 0U;
    input_response.layout = ProjectionPrinterInventoryResponseLayout::
        V859SlotHotbarSlotAmountNetworkId;
    input_response.entry_count = 1U;
    input_response.successful_entry_count = 1U;
    input_response.request_id = -3;
    input_response.session_generation = task.expected_session_generation;
    input_response.response_generation = 11U;
    ProjectionPrinterInventoryResponseSlot response_source;
    response_source.container_id = 29U;
    response_source.slot = 6U;
    response_source.count = 0U;
    response_source.network_stack_id = 0;
    ProjectionPrinterInventoryResponseSlot response_destination;
    response_destination.container_id = 0U;
    response_destination.slot = 1U;
    response_destination.count = 1U;
    response_destination.network_stack_id = 71;
    input_response.slots = {response_source, response_destination};
    assert(ValidateMapAnvilInputAcceptedResponse(
        place_request, input_submission, input_response, &error));
    assert(error.empty());
    auto stale_input_response = input_response;
    stale_input_response.response_generation = 10U;
    assert(!ValidateMapAnvilInputAcceptedResponse(
        place_request, input_submission, stale_input_response, &error));
    auto wrong_input_response = input_response;
    wrong_input_response.slots[1].network_stack_id = 72;
    assert(ValidateMapAnvilInputAcceptedResponse(
        place_request, input_submission, wrong_input_response, &error));
    wrong_input_response.slots[1].network_stack_id = 0;
    assert(!ValidateMapAnvilInputAcceptedResponse(
        place_request, input_submission, wrong_input_response, &error));
    wrong_input_response = input_response;
    wrong_input_response.slots[0].count = 1U;
    assert(!ValidateMapAnvilInputAcceptedResponse(
        place_request, input_submission, wrong_input_response, &error));
    wrong_input_response = input_response;
    wrong_input_response.slots[1].container_id = 7U;
    assert(!ValidateMapAnvilInputAcceptedResponse(
        place_request, input_submission, wrong_input_response, &error));
    wrong_input_response = input_response;
    wrong_input_response.rejected = true;
    assert(!ValidateMapAnvilInputAcceptedResponse(
        place_request, input_submission, wrong_input_response, &error));

    // Manual v859 craft receipt: one accepted response with the anvil input
    // emptied and one named result in container 12. Its observed nine-byte
    // title is not a protocol constant; automatic output must match the full
    // calculated tile title, then separately pass native UUID/name readback.
    MapAnvilCraftSubmission craft_submission;
    craft_submission.request_id = -9;
    craft_submission.response_session_generation = task.expected_session_generation;
    craft_submission.response_generation_before_send = 20U;
    craft_submission.window_token = task.window_token;
    craft_submission.window_id = task.expected_window_id;
    craft_submission.dynamic_recipe_network_id = 3304U;
    craft_submission.consumed_input_network_stack_id = 71;
    craft_submission.destination_hotbar_slot = 0;
    ProjectionPrinterInventoryResponse craft_response;
    craft_response.valid = true;
    craft_response.status = 0U;
    craft_response.layout = ProjectionPrinterInventoryResponseLayout::
        V859SlotHotbarSlotAmountNetworkId;
    craft_response.entry_count = 1U;
    craft_response.successful_entry_count = 1U;
    craft_response.request_id = craft_submission.request_id;
    craft_response.session_generation = task.expected_session_generation;
    craft_response.response_generation = 21U;
    ProjectionPrinterInventoryResponseSlot emptied_input;
    emptied_input.container_id = 0U;
    emptied_input.slot = 1U;
    emptied_input.count = 0U;
    emptied_input.network_stack_id = 0;
    ProjectionPrinterInventoryResponseSlot named_output;
    named_output.container_id = 12U;
    named_output.slot = 0U;
    named_output.count = 1U;
    named_output.network_stack_id = 72;
    named_output.custom_name = input_draft.expected_title;
    craft_response.slots = {emptied_input, named_output};
    int32_t accepted_output_network_stack_id = 0;
    assert(ValidateMapAnvilCraftAcceptedResponse(
        task, craft_submission, craft_response, &error,
        &accepted_output_network_stack_id));
    assert(accepted_output_network_stack_id == 72);
    assert(error.empty());
    // The game's actual result click may use any empty hotbar slot. The ACK
    // must match the slot carried by that exact outbound request.
    craft_submission.destination_hotbar_slot = 1;
    craft_response.slots[1].slot = 1U;
    craft_response.slots[1].hotbar_slot = 1U;
    assert(ValidateMapAnvilCraftAcceptedResponse(
        task, craft_submission, craft_response, &error,
        &accepted_output_network_stack_id));
    craft_response.slots[1].slot = 0U;
    assert(!ValidateMapAnvilCraftAcceptedResponse(
        task, craft_submission, craft_response, &error));
    craft_submission.destination_hotbar_slot = 0;
    craft_response.slots[1].slot = 0U;
    craft_response.slots[1].hotbar_slot = 0U;
    const auto reject_craft_response = [&](const ProjectionPrinterInventoryResponse& candidate) {
        accepted_output_network_stack_id = 999;
        assert(!ValidateMapAnvilCraftAcceptedResponse(
            task, craft_submission, candidate, &error,
            &accepted_output_network_stack_id));
        assert(accepted_output_network_stack_id == 0);
        assert(!error.empty());
    };
    auto wrong_craft_response = craft_response;
    wrong_craft_response.request_id = -13;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.session_generation += 1U;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.response_generation = 20U;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.valid = false;
    wrong_craft_response.rejected = true;
    wrong_craft_response.status = 4U;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.layout = ProjectionPrinterInventoryResponseLayout::None;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.entry_count = 2U;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.successful_entry_count = 0U;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.slots.pop_back();
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.slots.push_back(named_output);
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.slots[0].has_dynamic_container_id = true;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.slots[1].dynamic_container_id = 3U;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.slots[0].slot = 0U;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.slots[1].container_id = 29U;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.slots[0].count = 1U;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.slots[0].network_stack_id = 7;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.slots[1].count = 2U;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.slots[1].network_stack_id = 0;
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.slots[1].custom_name.clear();
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.slots[1].custom_name = "test_name"; // nine bytes, wrong title
    reject_craft_response(wrong_craft_response);
    wrong_craft_response = craft_response;
    wrong_craft_response.slots[1].custom_name += " ";
    reject_craft_response(wrong_craft_response);
    auto wrong_craft_submission = craft_submission;
    wrong_craft_submission.request_id = -11;
    assert(!ValidateMapAnvilCraftAcceptedResponse(
        task, wrong_craft_submission, craft_response, &error));
    wrong_craft_submission = craft_submission;
    wrong_craft_submission.request_id = -10;
    assert(!ValidateMapAnvilCraftAcceptedResponse(
        task, wrong_craft_submission, craft_response, &error));
    wrong_craft_submission = craft_submission;
    wrong_craft_submission.response_session_generation += 1U;
    assert(!ValidateMapAnvilCraftAcceptedResponse(
        task, wrong_craft_submission, craft_response, &error));
    wrong_craft_submission = craft_submission;
    wrong_craft_submission.window_token += 1U;
    assert(!ValidateMapAnvilCraftAcceptedResponse(
        task, wrong_craft_submission, craft_response, &error));
    wrong_craft_submission = craft_submission;
    wrong_craft_submission.window_id += 1U;
    assert(!ValidateMapAnvilCraftAcceptedResponse(
        task, wrong_craft_submission, craft_response, &error));
    auto invalid_craft_task = task;
    invalid_craft_task.expected_map_uuid = -1;
    assert(!ValidateMapAnvilCraftAcceptedResponse(
        invalid_craft_task, craft_submission, craft_response, &error));

    auto wrong_window = window;
    wrong_window.container_type = 0U;
    assert(!PrepareMapAnvilInputDraft(task, world, wrong_window, held,
                                      &input_draft, &error));
    wrong_window = window;
    wrong_window.token += 1U;
    assert(!PrepareMapAnvilInputDraft(task, world, wrong_window, held,
                                      &input_draft, &error));
    wrong_window = window;
    wrong_window.slot_count = 27U;
    assert(!PrepareMapAnvilInputDraft(task, world, wrong_window, held,
                                      &input_draft, &error));
    wrong_window = window;
    wrong_window.closed = true;
    assert(!PrepareMapAnvilInputDraft(task, world, wrong_window, held,
                                      &input_draft, &error));
    wrong_window = window;
    wrong_window.input_slot_empty = false;
    assert(!PrepareMapAnvilInputDraft(task, world, wrong_window, held,
                                      &input_draft, &error));

    auto wrong_world = world;
    wrong_world.block_identifier = "minecraft:chest";
    assert(!PrepareMapAnvilInputDraft(task, wrong_world, window, held,
                                      &input_draft, &error));
    wrong_world = world;
    wrong_world.dimension_token += 1U;
    assert(PrepareMapAnvilInputDraft(task, wrong_world, window, held,
                                     &input_draft, &error));
    wrong_world = world;
    wrong_world.world_context = "stable:v1:another-world|0";
    assert(!PrepareMapAnvilInputDraft(task, wrong_world, window, held,
                                      &input_draft, &error));
    auto worn_world = world;
    worn_world.block_identifier = "minecraft:chipped_anvil";
    assert(PrepareMapAnvilInputDraft(task, worn_world, window, held,
                                     &input_draft, &error));

    auto wrong_held = held;
    wrong_held.count = 2U;
    assert(!PrepareMapAnvilInputDraft(task, world, window, wrong_held,
                                      &input_draft, &error));
    wrong_held = held;
    wrong_held.item_identifier = "minecraft:empty_map";
    assert(!PrepareMapAnvilInputDraft(task, world, window, wrong_held,
                                      &input_draft, &error));
    wrong_held = held;
    wrong_held.map_uuid += 1;
    assert(!PrepareMapAnvilInputDraft(task, world, window, wrong_held,
                                      &input_draft, &error));

    window.input_slot_empty = false;
    MapAnvilInputMapEvidence input;
    input.session_generation = task.expected_session_generation;
    input.window_token = task.window_token;
    input.window_id = task.expected_window_id;
    input.slot = 1U;
    input.runtime_item_id = task.expected_runtime_item_id;
    input.network_stack_id = task.source_network_stack_id + 1;
    input.count = 1U;
    input.has_map_uuid = true;
    input.map_uuid = task.expected_map_uuid;
    MapAnvilRecipeObservation recipe;
    recipe.ticket = task.recipe_ticket;
    recipe.window_id = task.expected_window_id;
    recipe.map_uuid = task.expected_map_uuid;
    recipe.recipe_network_id = 9876U;  // Explicitly not the manual sample ID.

    MapAnvilCraftDraft craft;
    assert(!PrepareMapAnvilCraftDraft(task, world, window, input,
                                      MapAnvilRecipeProbeStatus::Observed,
                                      recipe, &craft, &error));
    assert(error == "the actual anvil rename recipe source is not verified");
    assert(craft.dynamic_recipe_network_id == 0U && craft.expected_title.empty());
    assert(!SendMapAnvilRename(craft, &error));
    assert(!error.empty());

    assert(!PrepareMapAnvilCraftDraft(task, world, window, input,
                                      MapAnvilRecipeProbeStatus::Armed,
                                      recipe, &craft, &error));
    auto wrong_recipe = recipe;
    wrong_recipe.ticket += 1U;
    assert(!PrepareMapAnvilCraftDraft(task, world, window, input,
                                      MapAnvilRecipeProbeStatus::Observed,
                                      wrong_recipe, &craft, &error));
    wrong_recipe = recipe;
    wrong_recipe.map_uuid += 1;
    assert(!PrepareMapAnvilCraftDraft(task, world, window, input,
                                      MapAnvilRecipeProbeStatus::Observed,
                                      wrong_recipe, &craft, &error));
    wrong_recipe = recipe;
    wrong_recipe.recipe_network_id = 0U;
    assert(!PrepareMapAnvilCraftDraft(task, world, window, input,
                                      MapAnvilRecipeProbeStatus::Observed,
                                      wrong_recipe, &craft, &error));
    auto wrong_input = input;
    wrong_input.network_stack_id = 0;
    assert(!PrepareMapAnvilCraftDraft(task, world, window, wrong_input,
                                      MapAnvilRecipeProbeStatus::Observed,
                                      recipe, &craft, &error));
    wrong_input = input;
    wrong_input.window_token += 1U;
    assert(!PrepareMapAnvilCraftDraft(task, world, window, wrong_input,
                                      MapAnvilRecipeProbeStatus::Observed,
                                      recipe, &craft, &error));
    wrong_input = input;
    wrong_input.map_uuid += 1;
    assert(!PrepareMapAnvilCraftDraft(task, world, window, wrong_input,
                                      MapAnvilRecipeProbeStatus::Observed,
                                      recipe, &craft, &error));
    return 0;
}
