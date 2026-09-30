#include "../MapChestStorageSender.h"

#include <cassert>
#include <stdexcept>
#include <string>

namespace {

struct JournalProbe {
    int calls = 0;
    int32_t last_request_id = 0;
    bool succeed = true;
    bool throw_exception = false;
};

bool journalBeforeSend(int32_t request_id, void* context, std::string* error) {
    auto* probe = static_cast<JournalProbe*>(context);
    ++probe->calls;
    probe->last_request_id = request_id;
    if (probe->throw_exception) throw std::runtime_error("journal unavailable");
    if (!probe->succeed) {
        if (error) *error = "durable journal write failed";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    using namespace build_import;
    FilledMapToSingleChestRequest request;
    request.expected_world_context = "stable:v1:test|0";
    request.expected_dimension_token = 0U;
    request.capture_token = 91U;
    request.chest_x = 235;
    request.chest_y = 22;
    request.chest_z = 126;
    request.expected_window_id = 5U;
    request.destination_slot = 0U;
    request.source_hotbar_slot = 6;
    request.source_network_stack_id = 76;
    request.expected_runtime_item_id = 358;
    request.expected_map_uuid = -532575944698LL;
    JournalProbe journal;
    request.pre_send = journalBeforeSend;
    request.pre_send_context = &journal;

    ContainerCaptureResult capture;
    capture.token = request.capture_token;
    capture.x = request.chest_x;
    capture.y = request.chest_y;
    capture.z = request.chest_z;
    capture.container_id = request.expected_window_id;
    capture.container_type = 0U;
    capture.container_opened = true;
    capture.slot_count = 27U;
    capture.has_full_container_name = true;
    capture.full_container_name = 0U;
    CapturedContainerItem existing;
    existing.slot = 1U;
    existing.numeric_id = 358;
    existing.count = 1U;
    existing.has_network_stack_id = true;
    existing.network_stack_id = 77;
    existing.has_map_uuid = true;
    existing.map_uuid = -532575944699LL;
    capture.items.push_back(existing);

    FilledMapChestSourceEvidence source;
    source.selected_hotbar_slot = 6;
    source.network_stack_id = 76;
    source.runtime_item_id = 358;
    source.count = 1U;
    source.item_identifier = "minecraft:map";
    source.has_map_uuid = true;
    source.map_uuid = request.expected_map_uuid;

    FilledMapChestWorldEvidence world;
    world.world_context = request.expected_world_context;
    world.dimension_token = 0U;
    world.block_identifier = "minecraft:chest";

    std::string error;
    assert(ValidateFilledMapSingleChestPreflight(request, capture, source,
                                                  world, &error));
    assert(error.empty());

    assert(ArmFilledMapChestDispatchBeforeSend(request, -17, &error));
    assert(journal.calls == 1 && journal.last_request_id == -17 &&
           error.empty());
    assert(!ArmFilledMapChestDispatchBeforeSend(request, 0, &error));
    assert(journal.calls == 1);
    journal.succeed = false;
    assert(!ArmFilledMapChestDispatchBeforeSend(request, -19, &error));
    assert(journal.calls == 2 && journal.last_request_id == -19 &&
           !error.empty());
    journal.succeed = true;
    journal.throw_exception = true;
    assert(!ArmFilledMapChestDispatchBeforeSend(request, -21, &error));
    assert(journal.calls == 3 && !error.empty());
    journal.throw_exception = false;

    auto no_journal = request;
    no_journal.pre_send = nullptr;
    assert(!ValidateFilledMapSingleChestPreflight(no_journal, capture, source,
                                                   world, &error));

    auto wrong_request = request;
    wrong_request.expected_world_context = "stable:v1:another|0";
    assert(!ValidateFilledMapSingleChestPreflight(wrong_request, capture, source,
                                                   world, &error));
    wrong_request = request;
    wrong_request.expected_world_context = "stable:v1:test|1";
    assert(!ValidateFilledMapSingleChestPreflight(wrong_request, capture, source,
                                                    world, &error));
    wrong_request = request;
    wrong_request.expected_dimension_token = 0x5678U;
    assert(ValidateFilledMapSingleChestPreflight(wrong_request, capture, source,
                                                 world, &error));
    wrong_request = request;
    wrong_request.expected_window_id = 7U;
    assert(!ValidateFilledMapSingleChestPreflight(wrong_request, capture, source,
                                                   world, &error));
    wrong_request = request;
    wrong_request.destination_slot = 1U;
    assert(!ValidateFilledMapSingleChestPreflight(wrong_request, capture, source,
                                                   world, &error));
    wrong_request = request;
    wrong_request.expected_runtime_item_id = 999;
    assert(!ValidateFilledMapSingleChestPreflight(wrong_request, capture, source,
                                                   world, &error));
    wrong_request = request;
    wrong_request.expected_map_uuid += 1;
    assert(!ValidateFilledMapSingleChestPreflight(wrong_request, capture, source,
                                                   world, &error));

    auto wrong_source = source;
    wrong_source.count = 2U;
    assert(!ValidateFilledMapSingleChestPreflight(request, capture, wrong_source,
                                                   world, &error));
    wrong_source = source;
    wrong_source.item_identifier = "minecraft:empty_map";
    assert(!ValidateFilledMapSingleChestPreflight(request, capture, wrong_source,
                                                   world, &error));
    wrong_source = source;
    wrong_source.has_map_uuid = false;
    assert(!ValidateFilledMapSingleChestPreflight(request, capture, wrong_source,
                                                   world, &error));

    auto wrong_world = world;
    wrong_world.block_identifier = "minecraft:trapped_chest";
    assert(!ValidateFilledMapSingleChestPreflight(request, capture, source,
                                                   wrong_world, &error));
    wrong_world = world;
    wrong_world.world_context = "stable:v1:test|1";
    assert(!ValidateFilledMapSingleChestPreflight(request, capture, source,
                                                    wrong_world, &error));
    wrong_world = world;
    wrong_world.dimension_token = 0x5678U;
    assert(ValidateFilledMapSingleChestPreflight(request, capture, source,
                                                 wrong_world, &error));

    auto wrong_capture = capture;
    wrong_capture.items[0].map_uuid = request.expected_map_uuid;
    assert(!ValidateFilledMapSingleChestPreflight(request, wrong_capture, source,
                                                   world, &error));
    wrong_capture = capture;
    wrong_capture.container_closed = true;
    assert(!ValidateFilledMapSingleChestPreflight(request, wrong_capture, source,
                                                   world, &error));
    wrong_capture = capture;
    wrong_capture.slot_count = 54U;
    assert(!ValidateFilledMapSingleChestPreflight(request, wrong_capture, source,
                                                   world, &error));
    wrong_capture = capture;
    wrong_capture.full_container_name = 7U;
    assert(!ValidateFilledMapSingleChestPreflight(request, wrong_capture, source,
                                                   world, &error));
    wrong_capture = capture;
    wrong_capture.has_dynamic_container_id = true;
    wrong_capture.dynamic_container_id = 42U;
    assert(!ValidateFilledMapSingleChestPreflight(request, wrong_capture, source,
                                                   world, &error));

    FilledMapChestSubmission submission;
    submission.request_id = -17;
    submission.response_session_generation = 7;
    submission.response_generation_before_send = 40;
    ProjectionPrinterInventoryResponse response;
    response.valid = true;
    response.status = 0;
    response.layout =
        ProjectionPrinterInventoryResponseLayout::V859SlotHotbarSlotAmountNetworkId;
    response.request_id = -17;
    response.session_generation = 7;
    response.response_generation = 41;
    ProjectionPrinterInventoryResponseSlot source_slot;
    source_slot.container_id = 29;
    source_slot.slot = 6;
    source_slot.count = 0;
    ProjectionPrinterInventoryResponseSlot chest_slot;
    chest_slot.container_id = 7;
    chest_slot.slot = 0;
    chest_slot.count = 1;
    chest_slot.network_stack_id = 76;
    chest_slot.custom_name = "地图 1行1列";
    response.slots = {source_slot, chest_slot};
    assert(ValidateFilledMapChestAcceptedResponse(
        request, submission, response, "地图 1行1列", &error));
    assert(!ValidateFilledMapChestAcceptedResponse(
        request, submission, response, "地图 1行2列", &error));
    response.response_generation = 40;
    assert(!ValidateFilledMapChestAcceptedResponse(
        request, submission, response, "地图 1行1列", &error));
    response.response_generation = 41;
    response.slots[1].container_id = 12;
    assert(!ValidateFilledMapChestAcceptedResponse(
        request, submission, response, "地图 1行1列", &error));
    return 0;
}
