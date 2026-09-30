#include "../MapStorageAnvilAdapter.h"
#include "../MapTileNaming.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

using namespace build_import;

namespace {

struct Probe {
    std::string state_path;
    bool input_placed = false;
    bool output_collected = false;
    bool input_response_seen = false;
    bool craft_response_seen = false;
    bool stale_input_response = false;
    bool reject_craft_response = false;
    bool wrong_native_output_network_stack_id = false;
    bool craft_enabled = false;
    int input_submits = 0;
    int craft_submits = 0;
    int name_submits = 0;
    int accepted_client_input_syncs = 0;
    std::string native_input_failure;
};

bool readBaseline(void* context, uint64_t* session, uint64_t* generation,
                  std::string*) {
    auto* probe = static_cast<Probe*>(context);
    *session = 8;
    *generation = probe->input_response_seen ? 21 : 20;
    return true;
}

bool readResponse(void* context, int32_t request_id,
                  ProjectionPrinterInventoryResponse* result,
                  std::string*) {
    auto* probe = static_cast<Probe*>(context);
    if ((request_id == -9 && !probe->input_response_seen) ||
        (request_id == -11 && !probe->craft_response_seen)) return false;
    result->valid = !(request_id == -11 && probe->reject_craft_response);
    result->rejected = !result->valid;
    result->status = result->valid ? 0 : 4;
    result->layout = ProjectionPrinterInventoryResponseLayout::
        V859SlotHotbarSlotAmountNetworkId;
    result->entry_count = 1;
    result->successful_entry_count = 1;
    result->session_generation = 8;
    result->response_generation = request_id == -9
        ? (probe->stale_input_response ? 20 : 21) : 22;
    result->request_id = request_id;
    ProjectionPrinterInventoryResponseSlot source;
    ProjectionPrinterInventoryResponseSlot destination;
    if (request_id == -9) {
        source.container_id = 29;
        source.slot = 6;
        destination.container_id = 0;
        destination.slot = 1;
        destination.count = 1;
        destination.network_stack_id = 81;
    } else if (request_id == -11) {
        source.container_id = 0;
        source.slot = 1;
        destination.container_id = 12;
        destination.slot = 1;
        destination.hotbar_slot = 1;
        destination.count = 1;
        destination.network_stack_id = 72;
        destination.custom_name = "地图 1行1列";
    } else {
        return false;
    }
    result->slots = {source, destination};
    return true;
}

bool readWorld(void*, const std::string& expected_world,
               int32_t dimension, int32_t x, int32_t y, int32_t z,
               MapAnvilRenameWorldEvidence* result, std::string*) {
    assert(expected_world == "stable:v1:anvil-adapter|0");
    assert(dimension == 0 && x == 236 && y == 22 && z == 126);
    result->world_context = expected_world;
    result->dimension_token = 0x1234;
    result->block_identifier = "minecraft:anvil";
    return true;
}

bool readHeld(void*, MapAnvilHeldMapEvidence* result, std::string*) {
    result->selected_hotbar_slot = 6;
    result->network_stack_id = 71;
    result->runtime_item_id = 358;
    result->count = 1;
    result->item_identifier = "minecraft:map";
    result->has_map_uuid = true;
    result->map_uuid = 1234567;
    return true;
}

bool visibleWindow(void* context, int32_t x, int32_t y, int32_t z,
                   MapAnvilRenameWindowEvidence* result, std::string*) {
    auto* probe = static_cast<Probe*>(context);
    result->session_generation = 8;
    result->token = 10;
    result->window_id = 3;
    result->container_type = 5;
    result->x = x;
    result->y = y;
    result->z = z;
    result->opened = true;
    result->content_observed = true;
    result->slot_count = 3;
    result->input_slot_empty = !probe->input_placed;
    return true;
}

bool readInput(void* context, const MapAnvilRenameWindowEvidence& window,
               MapAnvilInputProof* result, std::string* error) {
    auto* probe = static_cast<Probe*>(context);
    if (!probe->native_input_failure.empty()) {
        if (error) *error = probe->native_input_failure;
        return false;
    }
    if (!probe->input_placed) return false;
    result->world_id = "stable:v1:anvil-adapter|0";
    result->dimension_id = 0;
    result->anvil_x = window.x;
    result->anvil_y = window.y;
    result->anvil_z = window.z;
    result->fresh_window_token = window.token;
    result->window_id = window.window_id;
    result->input_slot = 1;
    result->count = 1;
    result->runtime_item_id = 358;
    result->network_stack_id = 81;
    result->map_uuid = 1234567;
    result->native_readback = true;
    return true;
}

bool syncAcceptedClientInput(void* context,
                             const MapAnvilRenameRecord& record,
                             const MapAnvilRenameWindowEvidence& window,
                             std::string*) {
    auto* probe = static_cast<Probe*>(context);
    ++probe->accepted_client_input_syncs;
    assert(record.phase == MapAnvilRenamePhase::InputResponseAccepted);
    assert(record.input_response.source_hotbar_slot == 6);
    assert(record.input_source_network_stack_id == 71);
    assert(record.map_uuid == 1234567);
    assert(window.window_id == record.input_response.window_id);
    assert(window.token == record.input_response.window_token);
    return true;
}

bool submitInput(void* context, const MapAnvilInputPlaceRequest& request,
                 MapAnvilInputSubmission* result, std::string*) {
    auto* probe = static_cast<Probe*>(context);
    ++probe->input_submits;
    assert(request.task.expected_map_uuid == 1234567);
    assert(request.task.source_hotbar_slot == 6);
    assert(request.pre_send(-9, request.pre_send_context, nullptr));
    MapAnvilRenameRecord record;
    assert(LoadMapAnvilRenameJournal(probe->state_path, &record) ==
           MapAnvilRenameLoad::Loaded);
    assert(record.phase == MapAnvilRenamePhase::InputDispatchArmed);
    probe->input_placed = true;
    result->request_id = -9;
    result->response_session_generation = 8;
    return true;
}

bool submitName(void* context, const MapAnvilRenameWindowEvidence&,
                const MapAnvilRenameRecord& record, std::string*) {
    auto* probe = static_cast<Probe*>(context);
    ++probe->name_submits;
    assert(record.expected_title == "地图 1行1列");
    return true;
}

bool readPreview(void*, const MapAnvilRenameWindowEvidence&,
                 MapStorageAnvilCraftPreview* result, std::string*) {
    result->native_readback = true;
    result->dynamic_recipe_network_id = 3304; // Test fixture, never production default.
    result->input_map_uuid = 1234567;
    result->output_map_uuid = 1234567;
    result->output_title = "地图 1行1列";
    return true;
}

bool submitCraft(void* context, const MapAnvilRenameRecord& confirmed,
                 const MapAnvilRenameWindowEvidence&,
                 const MapAnvilInputProof& input,
                 const MapStorageAnvilCraftPreview&,
                 std::string*) {
    auto* probe = static_cast<Probe*>(context);
    ++probe->craft_submits;
    assert(ArmMapAnvilCraftClick(probe->state_path, confirmed, input));
    MapAnvilRenameRecord record;
    assert(LoadMapAnvilRenameJournal(probe->state_path, &record) ==
           MapAnvilRenameLoad::Loaded);
    assert(record.phase == MapAnvilRenamePhase::CraftClickArmed);
    return true;
}

bool readOutput(void* context, MapAnvilOutputProof* result, std::string*) {
    auto* probe = static_cast<Probe*>(context);
    if (!probe->output_collected) return false;
    result->world_id = "stable:v1:anvil-adapter|0";
    result->dimension_id = 0;
    result->count = 1;
    result->runtime_item_id = 358;
    result->network_stack_id =
        probe->wrong_native_output_network_stack_id ? 73 : 72;
    result->map_uuid = 1234567;
    result->name_source = MapItemNameSource::DisplayName;
    result->name = "地图 1行1列";
    result->native_readback = true;
    return true;
}

MapAnvilRenameRecord load(const std::string& path) {
    MapAnvilRenameRecord result;
    assert(LoadMapAnvilRenameJournal(path, &result) ==
           MapAnvilRenameLoad::Loaded);
    return result;
}

}  // namespace

int main() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
        ("map_anvil_adapter_" + std::to_string(nonce));
    assert(std::filesystem::create_directories(directory));
    Probe probe;
    probe.state_path = (directory / "map_creation.state").string();
    MapStorageAnvilAdapter adapter;
    adapter.map_state_path = probe.state_path;
    adapter.hooks.context = &probe;
    adapter.hooks.read_response_baseline = &readBaseline;
    adapter.hooks.read_exact_response = &readResponse;
    adapter.hooks.read_world = &readWorld;
    adapter.hooks.read_held_map = &readHeld;
    adapter.hooks.ensure_visible_window = &visibleWindow;
    adapter.hooks.read_visible_window = &visibleWindow;
    adapter.hooks.read_native_input = &readInput;
    adapter.hooks.sync_accepted_client_input = &syncAcceptedClientInput;
    adapter.hooks.submit_input = &submitInput;
    adapter.hooks.submit_name_text = &submitName;
    adapter.hooks.read_native_craft_preview = &readPreview;
    adapter.hooks.read_native_output = &readOutput;

    MapPairPlacementRecord pair;
    pair.phase = MapPairPlacementPhase::PairConfirmed;
    pair.pair.anvil = {236, 22, 126};
    MapStorageCoordinatorInput snapshot;
    snapshot.world_id = "stable:v1:anvil-adapter|0";
    snapshot.dimension_id = 0;
    snapshot.columns = 1;
    snapshot.rows = 1;
    snapshot.tile_count = 1;
    snapshot.checkpoint_tile_cursor = 0;
    snapshot.has_pending_map_use_marker = true;
    snapshot.pending_map_use_cursor = 0;
    snapshot.expected_map_uuid = 1234567;
    snapshot.pair = &pair;
    MapStorageCoordinatorDecision decision;
    assert(FormatMapTileName(0, 1, 1, &decision.expected_title));
    std::string error;
    assert(RunMapStorageAnvilTransition(
        &adapter, MapStorageNextStep::AwaitRenameJournal,
        snapshot, decision, &error) ==
        MapStorageExternalResult::DurableTransition);
    auto record = load(probe.state_path);
    assert(record.phase == MapAnvilRenamePhase::Prepared);
    snapshot.rename = &record;
    assert(RunMapStorageAnvilTransition(
        &adapter, MapStorageNextStep::PreflightAnvilInput,
        snapshot, decision, &error) ==
        MapStorageExternalResult::DurableTransition);
    assert(probe.input_submits == 1);
    record = load(probe.state_path);
    snapshot.rename = &record;
    assert(RunMapStorageAnvilReconcileNoResend(
        &adapter, MapStorageNextStep::ReconcileAnvilInputNoResend,
        snapshot, decision, &error) ==
        MapStorageExternalResult::NoChange);
    assert(load(probe.state_path).phase == MapAnvilRenamePhase::InputDispatchArmed);
    assert(probe.input_submits == 1); // No second Place.
    probe.input_response_seen = true;
    probe.stale_input_response = true;
    assert(RunMapStorageAnvilReconcileNoResend(
        &adapter, MapStorageNextStep::ReconcileAnvilInputNoResend,
        snapshot, decision, &error) ==
        MapStorageExternalResult::DispatchOutcomeUnknown);
    assert(load(probe.state_path).phase == MapAnvilRenamePhase::InputDispatchArmed);
    probe.stale_input_response = false;
    assert(RunMapStorageAnvilReconcileNoResend(
        &adapter, MapStorageNextStep::ReconcileAnvilInputNoResend,
        snapshot, decision, &error) ==
        MapStorageExternalResult::DurableTransition);
    record = load(probe.state_path);
    snapshot.rename = &record;
    probe.native_input_failure = "native anvil input is unreadable";
    assert(RunMapStorageAnvilReconcileNoResend(
        &adapter, MapStorageNextStep::ReconcileAnvilInputNoResend,
        snapshot, decision, &error) ==
        MapStorageExternalResult::NoChange);
    assert(probe.accepted_client_input_syncs == 0);
    assert(load(probe.state_path).phase ==
           MapAnvilRenamePhase::InputResponseAccepted);

    probe.native_input_failure = "native anvil input is not synchronized on client";
    assert(RunMapStorageAnvilReconcileNoResend(
        &adapter, MapStorageNextStep::ReconcileAnvilInputNoResend,
        snapshot, decision, &error) ==
        MapStorageExternalResult::NoChange);
    assert(probe.accepted_client_input_syncs == 1);
    assert(load(probe.state_path).phase ==
           MapAnvilRenamePhase::InputResponseAccepted);
    assert(RunMapStorageAnvilReconcileNoResend(
        &adapter, MapStorageNextStep::ReconcileAnvilInputNoResend,
        snapshot, decision, &error) ==
        MapStorageExternalResult::NoChange);
    assert(probe.accepted_client_input_syncs == 2);  // once per reconcile
    assert(probe.input_submits == 1);  // never resend accepted Place
    assert(load(probe.state_path).phase ==
           MapAnvilRenamePhase::InputResponseAccepted);

    probe.native_input_failure.clear();
    assert(RunMapStorageAnvilReconcileNoResend(
        &adapter, MapStorageNextStep::ReconcileAnvilInputNoResend,
        snapshot, decision, &error) ==
        MapStorageExternalResult::DurableTransition);
    assert(probe.accepted_client_input_syncs == 2);
    record = load(probe.state_path);
    snapshot.rename = &record;
    assert(RunMapStorageAnvilTransition(
        &adapter, MapStorageNextStep::PreflightAnvilCraft,
        snapshot, decision, &error) ==
        MapStorageExternalResult::NoChange);
    assert(probe.name_submits == 0 && probe.craft_submits == 0);
    adapter.hooks.submit_craft_and_collect = &submitCraft;
    assert(RunMapStorageAnvilTransition(
        &adapter, MapStorageNextStep::PreflightAnvilCraft,
        snapshot, decision, &error) ==
        MapStorageExternalResult::DurableTransition);
    assert(probe.name_submits == 1 && probe.craft_submits == 1);
    record = load(probe.state_path);
    snapshot.rename = &record;
    assert(RunMapStorageAnvilReconcileNoResend(
        &adapter, MapStorageNextStep::ReconcileAnvilCraftNoResend,
        snapshot, decision, &error) ==
        MapStorageExternalResult::NoChange);
    assert(load(probe.state_path).phase == MapAnvilRenamePhase::CraftClickArmed);
    assert(probe.craft_submits == 1); // Queued click is never replayed.
    MapAnvilResponseCorrelation craft_correlation;
    craft_correlation.session_generation = 8;
    craft_correlation.response_generation_before_send = 21;
    craft_correlation.window_token = 10;
    craft_correlation.window_id = 3;
    assert(ArmMapAnvilCraftDispatch(probe.state_path, record,
                                    MapAnvilInputProof{
                                        "stable:v1:anvil-adapter|0", 0,
                                        236, 22, 126, 10, 3, 1, 1, 358,
                                        81, 1234567, true},
                                    -11, craft_correlation, 1));
    probe.output_collected = true;
    record = load(probe.state_path);
    snapshot.rename = &record;
    assert(load(probe.state_path).phase == MapAnvilRenamePhase::CraftDispatchArmed);
    assert(probe.craft_submits == 1); // No second craft or output collection.
    probe.craft_response_seen = true;
    probe.reject_craft_response = true;
    assert(RunMapStorageAnvilReconcileNoResend(
        &adapter, MapStorageNextStep::ReconcileAnvilCraftNoResend,
        snapshot, decision, &error) ==
        MapStorageExternalResult::DispatchOutcomeUnknown);
    assert(load(probe.state_path).phase == MapAnvilRenamePhase::CraftDispatchArmed);
    probe.reject_craft_response = false;
    assert(RunMapStorageAnvilReconcileNoResend(
        &adapter, MapStorageNextStep::ReconcileAnvilCraftNoResend,
        snapshot, decision, &error) ==
        MapStorageExternalResult::DurableTransition);
    record = load(probe.state_path);
    snapshot.rename = &record;
    assert(record.craft_accepted_output_network_stack_id == 72);
    probe.wrong_native_output_network_stack_id = true;
    assert(RunMapStorageAnvilReconcileNoResend(
        &adapter, MapStorageNextStep::ReconcileAnvilCraftNoResend,
        snapshot, decision, &error) ==
        MapStorageExternalResult::NoChange);
    assert(load(probe.state_path).phase ==
           MapAnvilRenamePhase::CraftResponseAccepted);
    probe.wrong_native_output_network_stack_id = false;
    assert(RunMapStorageAnvilReconcileNoResend(
        &adapter, MapStorageNextStep::ReconcileAnvilCraftNoResend,
        snapshot, decision, &error) ==
        MapStorageExternalResult::DurableTransition);
    record = load(probe.state_path);
    assert(record.phase == MapAnvilRenamePhase::RenamedMapConfirmed);
    std::filesystem::remove_all(directory);
}
