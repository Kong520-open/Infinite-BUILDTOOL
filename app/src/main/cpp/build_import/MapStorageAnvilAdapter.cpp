#include "MapStorageAnvilAdapter.h"

#include "MapTileNaming.h"

#include <utility>

namespace build_import {
namespace {

MapStorageExternalResult wait(std::string* error, const char* reason) {
    if (error && error->empty()) *error = reason;
    return MapStorageExternalResult::NoChange;
}

MapStorageExternalResult uncertain(std::string* error, const char* reason) {
    if (error && error->empty()) *error = reason;
    return MapStorageExternalResult::DispatchOutcomeUnknown;
}

bool filledMap(const std::string& name) {
    return name == "minecraft:map" || name == "map" ||
        name == "minecraft:filled_map" || name == "filled_map" ||
        name == "minecraft:locator_map" || name == "locator_map";
}

bool sameCorrelation(const MapAnvilResponseCorrelation& a,
                     const MapAnvilResponseCorrelation& b) noexcept {
    return a.session_generation == b.session_generation &&
        a.response_generation_before_send == b.response_generation_before_send &&
        a.window_token == b.window_token && a.window_id == b.window_id &&
        a.source_hotbar_slot == b.source_hotbar_slot;
}

bool responseBaseline(const MapStorageAnvilHooks& hooks,
                      const MapAnvilRenameWindowEvidence& opened,
                      int32_t source_hotbar_slot,
                      MapAnvilResponseCorrelation* correlation,
                      std::string* error) {
    if (!correlation || !opened.session_generation || !opened.token ||
        !opened.window_id || opened.window_id == 0xFFU) {
        if (error) *error = "anvil response baseline lacks a live window";
        return false;
    }
    uint64_t session = 0;
    uint64_t generation = 0;
    if (!hooks.read_response_baseline ||
        !hooks.read_response_baseline(hooks.context, &session,
                                      &generation, error)) {
        if (error && error->empty()) *error = "anvil response baseline reader is unavailable";
        return false;
    }
    if (!session || session != opened.session_generation) {
        if (error) *error = "anvil response session differs from the open window";
        return false;
    }
    *correlation = {session, generation, opened.token, opened.window_id,
                    source_hotbar_slot};
    if (error) error->clear();
    return true;
}

bool exactResponse(const MapStorageAnvilHooks& hooks, int32_t request_id,
                   ProjectionPrinterInventoryResponse* response,
                   std::string* error) {
    if (!response || request_id >= 0 ||
        (static_cast<uint32_t>(request_id) & 1U) == 0U) {
        if (error) *error = "anvil response request ID is invalid";
        return false;
    }
    const bool found = hooks.read_exact_response &&
        hooks.read_exact_response(hooks.context, request_id, response, error);
    if (!found && error && error->empty()) {
        *error = "exact anvil ItemStackResponse has not arrived";
    }
    return found;
}

bool persisted(const MapStorageAnvilAdapter& adapter,
               const MapStorageCoordinatorInput& snapshot,
               const MapStorageCoordinatorDecision& decision,
               MapAnvilRenameRecord* record, std::string* error) {
    if (!record || !snapshot.rename || !snapshot.pair ||
        LoadMapAnvilRenameJournal(adapter.map_state_path, record, error) !=
            MapAnvilRenameLoad::Loaded ||
        record->world_id != snapshot.world_id ||
        record->dimension_id != snapshot.dimension_id ||
        record->tile_cursor != snapshot.checkpoint_tile_cursor ||
        record->tile_count != snapshot.tile_count ||
        record->map_uuid != snapshot.rename->map_uuid ||
        record->anvil_x != snapshot.pair->pair.anvil.x ||
        record->anvil_y != snapshot.pair->pair.anvil.y ||
        record->anvil_z != snapshot.pair->pair.anvil.z ||
        record->expected_title != decision.expected_title ||
        record->phase != snapshot.rename->phase ||
        record->input_request_id != snapshot.rename->input_request_id ||
        record->craft_request_id != snapshot.rename->craft_request_id ||
        record->craft_input_network_stack_id !=
            snapshot.rename->craft_input_network_stack_id ||
        record->craft_destination_hotbar_slot !=
            snapshot.rename->craft_destination_hotbar_slot ||
        record->craft_accepted_output_network_stack_id !=
            snapshot.rename->craft_accepted_output_network_stack_id ||
        record->renamed_network_stack_id !=
            snapshot.rename->renamed_network_stack_id ||
        !sameCorrelation(record->input_response,
                         snapshot.rename->input_response) ||
        !sameCorrelation(record->craft_response,
                         snapshot.rename->craft_response)) {
        if (error) *error = "durable anvil journal differs from the current tile";
        return false;
    }
    return true;
}

bool world(const MapStorageAnvilAdapter& adapter,
           const MapStorageCoordinatorInput& snapshot,
           MapAnvilRenameWorldEvidence* evidence, std::string* error) {
    if (!adapter.hooks.read_world || !snapshot.pair || !evidence) {
        if (error) *error = "native anvil world reader is unavailable";
        return false;
    }
    const auto& anvil = snapshot.pair->pair.anvil;
    if (!adapter.hooks.read_world(adapter.hooks.context, snapshot.world_id,
                                  snapshot.dimension_id, anvil.x, anvil.y,
                                  anvil.z, evidence, error) ||
        evidence->world_context != snapshot.world_id ||
        (evidence->block_identifier != "minecraft:anvil" &&
         evidence->block_identifier != "anvil" &&
         evidence->block_identifier != "minecraft:chipped_anvil" &&
         evidence->block_identifier != "chipped_anvil" &&
         evidence->block_identifier != "minecraft:damaged_anvil" &&
         evidence->block_identifier != "damaged_anvil")) {
        if (error && error->empty()) *error = "live anvil block or world changed";
        return false;
    }
    return true;
}

bool held(const MapStorageAnvilAdapter& adapter,
          const MapStorageCoordinatorInput& snapshot,
          MapAnvilHeldMapEvidence* evidence, std::string* error) {
    if (!adapter.hooks.read_held_map || !evidence ||
        !adapter.hooks.read_held_map(adapter.hooks.context, evidence, error) ||
        evidence->selected_hotbar_slot < 0 ||
        evidence->selected_hotbar_slot > 8 ||
        evidence->network_stack_id <= 0 ||
        evidence->runtime_item_id <= 0 || evidence->count != 1U ||
        !filledMap(evidence->item_identifier) ||
        !evidence->has_map_uuid || evidence->map_uuid == -1 ||
        evidence->map_uuid != snapshot.expected_map_uuid) {
        if (error && error->empty()) *error = "exact unstacked held map is unavailable";
        return false;
    }
    return true;
}

bool window(const MapStorageAnvilAdapter& adapter,
            const MapAnvilRenameRecord& record,
            bool may_open, MapAnvilRenameWindowEvidence* evidence,
            std::string* error) {
    const auto reader = may_open ? adapter.hooks.ensure_visible_window
                                 : adapter.hooks.read_visible_window;
    if (!reader || !evidence ||
        !reader(adapter.hooks.context, record.anvil_x, record.anvil_y,
                record.anvil_z, evidence, error) ||
        evidence->session_generation == 0U || evidence->token == 0U ||
        evidence->window_id == 0U || evidence->window_id == 0xFFU ||
        evidence->container_type != 5U ||
        evidence->x != record.anvil_x || evidence->y != record.anvil_y ||
        evidence->z != record.anvil_z || !evidence->opened ||
        evidence->closed || !evidence->content_observed ||
        evidence->slot_count != 3U) {
        if (error && error->empty()) *error = "live three-slot anvil window is unavailable";
        return false;
    }
    return true;
}

bool input(const MapStorageAnvilAdapter& adapter,
           const MapAnvilRenameRecord& record,
           const MapAnvilRenameWindowEvidence& opened,
           MapAnvilInputProof* proof, std::string* error) {
    if (!adapter.hooks.read_native_input || !proof ||
        !adapter.hooks.read_native_input(adapter.hooks.context, opened,
                                         proof, error) ||
        !proof->native_readback || proof->fresh_window_token != opened.token ||
        proof->window_id != opened.window_id ||
        proof->world_id != record.world_id ||
        proof->dimension_id != record.dimension_id ||
        proof->anvil_x != record.anvil_x ||
        proof->anvil_y != record.anvil_y ||
        proof->anvil_z != record.anvil_z || proof->input_slot != 1U ||
        proof->count != 1U ||
        proof->runtime_item_id != record.map_runtime_item_id ||
        proof->network_stack_id <= 0 ||
        proof->map_uuid != record.map_uuid) {
        if (error && error->empty()) *error = "native anvil input lacks the exact map UUID";
        return false;
    }
    return true;
}

struct InputArm {
    const std::string* map_state_path = nullptr;
    const MapAnvilRenameRecord* prepared = nullptr;
    const MapStorageAnvilHooks* hooks = nullptr;
    const MapAnvilRenameWindowEvidence* opened = nullptr;
    int32_t source_hotbar_slot = -1;
    bool called = false;
    bool armed = false;
};

bool armInput(int32_t request_id, void* context, std::string* error) {
    auto* arm = static_cast<InputArm*>(context);
    if (!arm || !arm->map_state_path || !arm->prepared || !arm->hooks ||
        !arm->opened || arm->called) {
        if (error) *error = "anvil input journal arm callback was reused";
        return false;
    }
    arm->called = true;
    MapAnvilResponseCorrelation correlation;
    if (!responseBaseline(*arm->hooks, *arm->opened,
                          arm->source_hotbar_slot, &correlation, error)) {
        return false;
    }
    arm->armed = ArmMapAnvilInputDispatch(*arm->map_state_path,
        *arm->prepared, request_id, correlation, error);
    return arm->armed;
}

MapStorageExternalResult begin(MapStorageAnvilAdapter* adapter,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision, std::string* error) {
    if (!snapshot.pair || snapshot.rename || !snapshot.has_pending_map_use_marker ||
        snapshot.pending_map_use_cursor != snapshot.checkpoint_tile_cursor ||
        snapshot.expected_map_uuid == -1 ||
        snapshot.pair->phase != MapPairPlacementPhase::PairConfirmed) {
        return wait(error, "map-use marker or confirmed anvil pair is unavailable");
    }
    MapAnvilRenameWorldEvidence live_world;
    MapAnvilHeldMapEvidence live_map;
    if (!world(*adapter, snapshot, &live_world, error) ||
        !held(*adapter, snapshot, &live_map, error)) {
        return MapStorageExternalResult::NoChange;
    }
    std::string title;
    if (!FormatMapTileName(snapshot.checkpoint_tile_cursor,
                           snapshot.columns, snapshot.rows, &title) ||
        title != decision.expected_title) {
        return wait(error, "map tile title changed before rename journal");
    }
    MapAnvilRenameRecord prepared;
    prepared.world_id = snapshot.world_id;
    prepared.dimension_id = snapshot.dimension_id;
    prepared.tile_cursor = snapshot.checkpoint_tile_cursor;
    prepared.tile_count = snapshot.tile_count;
    prepared.columns = snapshot.columns;
    prepared.rows = snapshot.rows;
    prepared.anvil_x = snapshot.pair->pair.anvil.x;
    prepared.anvil_y = snapshot.pair->pair.anvil.y;
    prepared.anvil_z = snapshot.pair->pair.anvil.z;
    prepared.map_runtime_item_id = live_map.runtime_item_id;
    prepared.map_uuid = live_map.map_uuid;
    prepared.input_source_network_stack_id = live_map.network_stack_id;
    prepared.expected_title = std::move(title);
    if (!BeginMapAnvilRenameJournal(adapter->map_state_path,
                                    prepared, error)) {
        return uncertain(error, "anvil journal creation outcome is uncertain");
    }
    return MapStorageExternalResult::DurableTransition;
}

MapStorageExternalResult placeInput(MapStorageAnvilAdapter* adapter,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision, std::string* error) {
    MapAnvilRenameRecord record;
    MapAnvilRenameWorldEvidence live_world;
    MapAnvilHeldMapEvidence live_map;
    MapAnvilRenameWindowEvidence opened;
    if (!persisted(*adapter, snapshot, decision, &record, error) ||
        record.phase != MapAnvilRenamePhase::Prepared ||
        snapshot.expected_map_uuid != record.map_uuid ||
        !world(*adapter, snapshot, &live_world, error) ||
        !held(*adapter, snapshot, &live_map, error) ||
        !window(*adapter, record, true, &opened, error)) {
        return MapStorageExternalResult::NoChange;
    }
    MapAnvilRenameTask task;
    task.expected_world_context = record.world_id;
    task.expected_session_generation = opened.session_generation;
    task.window_token = opened.token;
    task.recipe_ticket = opened.token;
    task.expected_window_id = opened.window_id;
    task.anvil_x = record.anvil_x;
    task.anvil_y = record.anvil_y;
    task.anvil_z = record.anvil_z;
    task.source_hotbar_slot = live_map.selected_hotbar_slot;
    task.source_network_stack_id = record.input_source_network_stack_id;
    task.expected_runtime_item_id = record.map_runtime_item_id;
    task.expected_map_uuid = record.map_uuid;
    task.tile_cursor = record.tile_cursor;
    task.columns = record.columns;
    task.rows = record.rows;
    MapAnvilInputDraft draft;
    if (!PrepareMapAnvilInputDraft(task, live_world, opened,
                                   live_map, &draft, error) ||
        !adapter->hooks.submit_input) {
        return wait(error, "verified anvil input sender is unavailable");
    }
    InputArm arm{&adapter->map_state_path, &record, &adapter->hooks,
                 &opened, live_map.selected_hotbar_slot, false, false};
    MapAnvilInputPlaceRequest request;
    request.task = std::move(task);
    request.pre_send = &armInput;
    request.pre_send_context = &arm;
    MapAnvilInputSubmission submission;
    const bool submitted = adapter->hooks.submit_input(
        adapter->hooks.context, request, &submission, error);
    if (arm.called) {
        // A failed native send after a durable arm may nevertheless have
        // reached the server. The only safe next step is no-resend readback.
        if (!arm.armed || !submitted ||
            submission.submission_outcome_uncertain) {
            return uncertain(error, "anvil input Place outcome is uncertain; never resend");
        }
        return MapStorageExternalResult::DurableTransition;
    }
    if (submitted) {
        return uncertain(error, "anvil input sender returned without journaling");
    }
    return MapStorageExternalResult::NoChange;
}

MapStorageExternalResult reconcileInput(MapStorageAnvilAdapter* adapter,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision, std::string* error) {
    MapAnvilRenameRecord record;
    if (!persisted(*adapter, snapshot, decision, &record, error)) {
        return MapStorageExternalResult::NoChange;
    }
    if (record.phase == MapAnvilRenamePhase::InputDispatchArmed) {
        MapAnvilRenameWorldEvidence live_world;
        MapAnvilRenameWindowEvidence opened;
        if (!world(*adapter, snapshot, &live_world, error) ||
            !window(*adapter, record, false, &opened, error) ||
            opened.session_generation != record.input_response.session_generation ||
            opened.token != record.input_response.window_token ||
            opened.window_id != record.input_response.window_id) {
            return wait(error, "anvil window changed before input acceptance");
        }
        ProjectionPrinterInventoryResponse response;
        if (!exactResponse(adapter->hooks, record.input_request_id,
                           &response, error)) {
            return wait(error, "awaiting exact accepted anvil input response; never resend");
        }
        MapAnvilInputPlaceRequest request;
        request.task.expected_session_generation =
            record.input_response.session_generation;
        request.task.source_hotbar_slot =
            record.input_response.source_hotbar_slot;
        request.task.source_network_stack_id =
            record.input_source_network_stack_id;
        request.task.expected_map_uuid = record.map_uuid;
        MapAnvilInputSubmission submission;
        submission.request_id = record.input_request_id;
        submission.response_session_generation =
            record.input_response.session_generation;
        submission.response_generation_before_send =
            record.input_response.response_generation_before_send;
        if (!ValidateMapAnvilInputAcceptedResponse(
                request, submission, response, error)) {
            return uncertain(error, "anvil input response was rejected or ambiguous");
        }
        return NoteMapAnvilInputAccepted(adapter->map_state_path, record,
                                          record.input_request_id, error)
            ? MapStorageExternalResult::DurableTransition
            : uncertain(error, "accepted anvil input could not be journaled");
    }
    if (record.phase != MapAnvilRenamePhase::InputResponseAccepted) {
        return wait(error, "anvil input is not awaiting native confirmation");
    }
    MapAnvilRenameWorldEvidence live_world;
    MapAnvilRenameWindowEvidence opened;
    MapAnvilInputProof proof;
    if (!world(*adapter, snapshot, &live_world, error) ||
        !window(*adapter, record, false, &opened, error)) {
        return MapStorageExternalResult::NoChange;
    }
    std::string input_error;
    if (!input(*adapter, record, opened, &proof, &input_error)) {
        if (input_error == "native anvil input is not synchronized on client" &&
            adapter->hooks.sync_accepted_client_input) {
            if (adapter->hooks.sync_accepted_client_input(
                    adapter->hooks.context, record, opened, error)) {
                return wait(error,
                    "awaiting native anvil input after accepted local slot sync");
            }
        } else if (error) {
            *error = std::move(input_error);
        }
        return MapStorageExternalResult::NoChange;
    }
    return ConfirmMapAnvilInput(adapter->map_state_path, record, proof, error)
        ? MapStorageExternalResult::DurableTransition
        : uncertain(error, "anvil input confirmation persistence is uncertain");
}

MapStorageExternalResult craft(MapStorageAnvilAdapter* adapter,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision, std::string* error) {
    MapAnvilRenameRecord record;
    MapAnvilRenameWorldEvidence live_world;
    MapAnvilRenameWindowEvidence opened;
    MapAnvilInputProof proof;
    if (!persisted(*adapter, snapshot, decision, &record, error) ||
        record.phase != MapAnvilRenamePhase::InputConfirmed ||
        !world(*adapter, snapshot, &live_world, error) ||
        !window(*adapter, record, false, &opened, error) ||
        !input(*adapter, record, opened, &proof, error) ||
        !adapter->hooks.submit_name_text ||
        !adapter->hooks.read_native_craft_preview ||
        !adapter->hooks.submit_craft_and_collect) {
        return wait(error, "verified anvil name/preview/result action is unavailable");
    }
    if (!adapter->hooks.submit_name_text(adapter->hooks.context, opened,
                                          record, error)) {
        return MapStorageExternalResult::NoChange;
    }
    MapStorageAnvilCraftPreview preview;
    if (!adapter->hooks.read_native_craft_preview(
            adapter->hooks.context, opened, &preview, error) ||
        !preview.native_readback ||
        preview.input_map_uuid != record.map_uuid ||
        preview.output_map_uuid != record.map_uuid ||
        preview.output_title != record.expected_title) {
        return wait(error, "native anvil output preview is not exact");
    }
    const bool clicked = adapter->hooks.submit_craft_and_collect(
        adapter->hooks.context, record, opened, proof, preview, error);
    MapAnvilRenameRecord after;
    if (LoadMapAnvilRenameJournal(adapter->map_state_path, &after) !=
        MapAnvilRenameLoad::Loaded ||
        after.world_id != record.world_id ||
        after.tile_cursor != record.tile_cursor ||
        after.map_uuid != record.map_uuid ||
        after.input_request_id != record.input_request_id) {
        return uncertain(error, "anvil click journal outcome is uncertain; never resend");
    }
    // The game's result callback can enqueue its packet for the next tick.
    // Once ClickArmed is durable, a second click is forbidden even though no
    // request ID exists yet. The outbound hook will arm Dispatch before send.
    if (after.phase == MapAnvilRenamePhase::CraftClickArmed ||
        after.phase == MapAnvilRenamePhase::CraftDispatchArmed) {
        if (error) error->clear();
        return MapStorageExternalResult::DurableTransition;
    }
    if (clicked) {
        return uncertain(error, "anvil result handler ran without durable click journal");
    }
    return MapStorageExternalResult::NoChange;
}

MapStorageExternalResult reconcileCraft(MapStorageAnvilAdapter* adapter,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision, std::string* error) {
    MapAnvilRenameRecord record;
    if (!persisted(*adapter, snapshot, decision, &record, error)) {
        return MapStorageExternalResult::NoChange;
    }
    if (record.phase == MapAnvilRenamePhase::CraftClickArmed) {
        // No request has been observed yet. Do not read the now-consumed
        // native input, invoke the handler again, or close its window.
        return wait(error, "awaiting the queued native anvil result request; never resend");
    }
    if (record.phase == MapAnvilRenamePhase::CraftDispatchArmed) {
        MapAnvilRenameWorldEvidence live_world;
        if (!world(*adapter, snapshot, &live_world, error)) {
            return MapStorageExternalResult::NoChange;
        }
        ProjectionPrinterInventoryResponse response;
        if (!exactResponse(adapter->hooks, record.craft_request_id,
                           &response, error)) {
            return wait(error, "awaiting exact accepted anvil craft response; never resend");
        }
        MapAnvilRenameTask task;
        task.expected_session_generation =
            record.craft_response.session_generation;
        task.window_token = record.craft_response.window_token;
        task.expected_window_id = record.craft_response.window_id;
        task.expected_map_uuid = record.map_uuid;
        task.tile_cursor = record.tile_cursor;
        task.columns = record.columns;
        task.rows = record.rows;
        MapAnvilCraftSubmission submission;
        submission.request_id = record.craft_request_id;
        submission.response_session_generation =
            record.craft_response.session_generation;
        submission.response_generation_before_send =
            record.craft_response.response_generation_before_send;
        submission.window_token = record.craft_response.window_token;
        submission.window_id = record.craft_response.window_id;
        submission.destination_hotbar_slot =
            record.craft_destination_hotbar_slot;
        int32_t accepted_output_network_stack_id = 0;
        if (!ValidateMapAnvilCraftAcceptedResponse(
                task, submission, response, error,
                &accepted_output_network_stack_id)) {
            return uncertain(error, "anvil craft response was rejected or ambiguous");
        }
        return NoteMapAnvilCraftAccepted(adapter->map_state_path, record,
                                          record.craft_request_id,
                                          accepted_output_network_stack_id, error)
            ? MapStorageExternalResult::DurableTransition
            : uncertain(error, "accepted anvil craft could not be journaled");
    }
    if (record.phase != MapAnvilRenamePhase::CraftResponseAccepted) {
        return wait(error, "anvil craft is not awaiting native output confirmation");
    }
    MapAnvilRenameWorldEvidence live_world;
    MapAnvilOutputProof output;
    if (!world(*adapter, snapshot, &live_world, error) ||
        !adapter->hooks.read_native_output ||
        !adapter->hooks.read_native_output(adapter->hooks.context,
                                           &output, error) ||
        !output.native_readback || output.world_id != record.world_id ||
        output.dimension_id != record.dimension_id ||
        output.count != 1U ||
        output.runtime_item_id != record.map_runtime_item_id ||
        output.network_stack_id <= 0 ||
        output.network_stack_id !=
            record.craft_accepted_output_network_stack_id ||
        output.map_uuid != record.map_uuid ||
        output.name_source != MapItemNameSource::DisplayName ||
        output.name != record.expected_title) {
        return wait(error, "server-confirmed renamed map is not held natively");
    }
    return ConfirmMapAnvilRenamedOutput(
        adapter->map_state_path, record, output, error)
        ? MapStorageExternalResult::DurableTransition
        : uncertain(error, "renamed map confirmation persistence is uncertain");
}

}  // namespace

bool IsMapStorageAnvilStep(MapStorageNextStep step) noexcept {
    switch (step) {
        case MapStorageNextStep::AwaitRenameJournal:
        case MapStorageNextStep::PreflightAnvilInput:
        case MapStorageNextStep::ReconcileAnvilInputNoResend:
        case MapStorageNextStep::PreflightAnvilCraft:
        case MapStorageNextStep::ReconcileAnvilCraftNoResend:
            return true;
        default:
            return false;
    }
}

MapStorageExternalResult RunMapStorageAnvilTransition(
        MapStorageAnvilAdapter* adapter, MapStorageNextStep step,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision,
        std::string* error) {
    if (error) error->clear();
    if (!adapter || adapter->map_state_path.empty() ||
        !IsMapStorageAnvilStep(step)) {
        return wait(error, "anvil transition adapter is unavailable");
    }
    switch (step) {
        case MapStorageNextStep::AwaitRenameJournal:
            return begin(adapter, snapshot, decision, error);
        case MapStorageNextStep::PreflightAnvilInput:
            return placeInput(adapter, snapshot, decision, error);
        case MapStorageNextStep::PreflightAnvilCraft:
            return craft(adapter, snapshot, decision, error);
        default:
            return wait(error, "anvil reconciliation cannot submit a packet");
    }
}

MapStorageExternalResult RunMapStorageAnvilReconcileNoResend(
        MapStorageAnvilAdapter* adapter, MapStorageNextStep step,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision,
        std::string* error) {
    if (error) error->clear();
    if (!adapter || adapter->map_state_path.empty() ||
        !IsMapStorageAnvilStep(step)) {
        return wait(error, "anvil reconciliation adapter is unavailable");
    }
    switch (step) {
        case MapStorageNextStep::ReconcileAnvilInputNoResend:
            return reconcileInput(adapter, snapshot, decision, error);
        case MapStorageNextStep::ReconcileAnvilCraftNoResend:
            return reconcileCraft(adapter, snapshot, decision, error);
        default:
            return wait(error, "anvil transition cannot run in no-resend mode");
    }
}

}  // namespace build_import
