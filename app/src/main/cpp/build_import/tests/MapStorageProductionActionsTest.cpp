#include "../MapStorageProductionActions.h"
#include "../MapTileNaming.h"

#include <cassert>
#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>

using namespace build_import;

namespace {

std::unordered_map<std::string, std::string> g_blocks;

std::string positionKey(int32_t x, int32_t y, int32_t z) {
    return std::to_string(x) + "," + std::to_string(y) + "," +
           std::to_string(z);
}

struct Probe {
    std::string state_path;
    bool placement_approved = false;
    bool ack_ready = false;
    bool ack_rejected = false;
    bool wrong_ack_uuid = false;
    bool ack_timeout = false;
    bool support_retry_ready = false;
    int dispatched = 0;
    int sequence = 0;
    bool map_submitted = false;
    bool close_ready = false;
    bool no_live_original_window = false;
    int chest_place_count = 0;
    int chest_open_count = 0;
    int chest_close_count = 0;
    bool marker_present = true;
    uint64_t durable_cursor = 0;
    uint64_t capture_token = 101;
};

bool liveWorld(void*, const std::string& world, int32_t dimension,
               std::string*) {
    return world == "stable:v1:actions-test|0" && dimension == 0;
}

bool newUuid(void* context, std::string* uuid, std::string*) {
    auto* probe = static_cast<Probe*>(context);
    *uuid = "map_actions_" + std::to_string(++probe->sequence);
    return true;
}

bool approvePlacement(void* context, const std::string& command, std::string*) {
    const auto* probe = static_cast<const Probe*>(context);
    assert(command.find("/fill ") == 0U);
    assert(command.find("minecraft:stone") != std::string::npos ||
           command.find("minecraft:chest") != std::string::npos ||
           command.find("minecraft:anvil") != std::string::npos);
    return probe->placement_approved;
}

bool sendTracked(void* context, const std::string& uuid,
                 const std::string&, std::string*) {
    auto* probe = static_cast<Probe*>(context);
    MapPairPlacementRecord persisted;
    assert(LoadMapPairPlacementJournal(
               probe->state_path, &persisted) == MapPairPlacementLoad::Loaded);
    if (persisted.phase == MapPairPlacementPhase::PairConfirmed) {
        MapExtraChestPlacementRecord extra;
        assert(LoadMapExtraChestPlacementJournal(probe->state_path, 1,
                                                &extra) ==
               MapExtraChestPlacementLoad::Loaded);
        assert(extra.phase == MapExtraChestPlacementPhase::SupportDispatchArmed ||
               extra.phase == MapExtraChestPlacementPhase::DispatchArmed);
        assert(extra.command_uuid == uuid);
    } else {
        assert(persisted.active_command_uuid == uuid);
        assert(persisted.phase == MapPairPlacementPhase::SupportDispatchArmed ||
               persisted.phase == MapPairPlacementPhase::ChestDispatchArmed ||
               persisted.phase == MapPairPlacementPhase::AnvilDispatchArmed);
    }
    ++probe->dispatched;
    return true;
}

bool pollTracked(void* context, const std::string& uuid,
                 MapStorageTrackedRpcReceipt* receipt, std::string* error) {
    const auto* probe = static_cast<const Probe*>(context);
    if (probe->ack_timeout) {
        if (error) *error = "tracked map placement ACK timed out; journal retained";
        return false;
    }
    receipt->returned_uuid = probe->wrong_ack_uuid ? "another_uuid" : uuid;
    receipt->ack = probe->ack_rejected ? MapPairCommandAck::Rejected
                 : probe->ack_ready ? MapPairCommandAck::Accepted
                                    : MapPairCommandAck::Pending;
    return true;
}

bool allowSupportRetry(void* context, const MapChestPosition&,
                       std::string*) {
    return static_cast<Probe*>(context)->support_retry_ready;
}

bool readHeldMap(void*, MapStorageHeldMapEvidence* held, std::string*) {
    held->source.selected_hotbar_slot = 6;
    held->source.network_stack_id = 72;
    held->source.runtime_item_id = 358;
    held->source.count = 1;
    held->source.item_identifier = "minecraft:map";
    held->source.has_map_uuid = true;
    held->source.map_uuid = -532575944698LL;
    held->dimension_token = 0x1234U;
    held->name_status = MapItemNameStatus::Present;
    held->name_source = MapItemNameSource::DisplayName;
    held->name = "地图 1行1列";
    held->native_readback = true;
    return true;
}

bool captureChest(void* context, const MapChestPosition& position,
                  bool reopen, ContainerCaptureResult* capture,
                  std::string*) {
    auto* probe = static_cast<Probe*>(context);
    if (reopen) {
        ++probe->chest_close_count;
        // A restart discards the original UI/window. The durable exact
        // accepted response survives, and there is nothing left to close.
        if (!probe->close_ready && !probe->no_live_original_window) return false;
        *capture = {}; // close-only: no second chest snapshot/open
        return true;
    }
    ++probe->chest_open_count;
    *capture = {};
    capture->token = probe->capture_token;
    capture->x = position.x;
    capture->y = position.y;
    capture->z = position.z;
    capture->container_id = 5U;
    capture->container_type = 0U;
    capture->container_opened = true;
    capture->slot_count = 27U;
    capture->has_full_container_name = true;
    capture->full_container_name = 0U;
    return true;
}

bool submitChest(void* context, const FilledMapToSingleChestRequest& request,
                 FilledMapChestSubmission* submission, std::string*) {
    auto* probe = static_cast<Probe*>(context);
    assert(request.expected_world_context == "stable:v1:actions-test|0");
    assert(request.capture_token == probe->capture_token);
    assert(request.source_hotbar_slot == 6 && request.destination_slot == 0U);
    assert(request.pre_send);
    std::string arm_error;
    assert(request.pre_send(-13, request.pre_send_context, &arm_error));
    MapChestTransferRecord armed;
    assert(LoadMapChestTransferJournal(probe->state_path, &armed) ==
           MapChestJournalLoad::Loaded);
    assert(armed.phase == MapChestTransferPhase::DispatchArmed);
    std::string ack_error;
    assert(NoteMapChestTransferAccepted(probe->state_path, armed, -13,
                                        &ack_error));
    submission->request_id = -13;
    probe->map_submitted = true;
    ++probe->chest_place_count;
    return true;
}

bool commitCursor(void* context, uint64_t before, uint64_t after,
                  const MapChestTransferRecord& chest, std::string*) {
    auto* probe = static_cast<Probe*>(context);
    assert(before == probe->durable_cursor && after == before + 1U);
    assert(chest.phase == MapChestTransferPhase::AcceptedAndClosed);
    probe->durable_cursor = after; // host mock of fsynced DDS1 update
    return true;
}

bool finalizeMarker(void* context, uint64_t old_cursor,
                    uint64_t committed_cursor, std::string*) {
    auto* probe = static_cast<Probe*>(context);
    assert(old_cursor == 0U && committed_cursor == 1U);
    assert(probe->durable_cursor == committed_cursor);
    assert(probe->marker_present);
    probe->marker_present = false;
    return true;
}

MapStorageCoordinatorInput inputFor() {
    MapStorageCoordinatorInput input;
    input.world_id = "stable:v1:actions-test|0";
    input.dimension_id = 0;
    input.artwork_bounds = {0, 20, 0, 20, 20, 20};
    input.columns = 2;
    input.rows = 1;
    input.tile_count = 2;
    input.checkpoint_tile_cursor = 0;
    input.has_pending_map_use_marker = true;
    input.pending_map_use_cursor = 0;
    input.expected_map_uuid = -532575944698LL;
    return input;
}

}  // namespace

namespace build_import {
bool NativeWorldReader::open() { return true; }
bool NativeWorldReader::getBlock(int32_t x, int32_t y, int32_t z,
                                 NativeBlockInfo* output) {
    if (!output) return false;
    const auto it = g_blocks.find(positionKey(x, y, z));
    output->name = it != g_blocks.end() ? it->second
                   : y <= 19 ? "minecraft:stone" : "minecraft:air";
    return true;
}
bool NativeWorldAccess::getBlock(int32_t, int32_t, int32_t,
                                 NativeBlockInfo*) { return false; }
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::path(argv[1]) /
        ("map_actions_host_" + std::to_string(nonce));
    assert(std::filesystem::create_directories(directory));

    Probe probe;
    probe.state_path = (directory / "map_creation.state").string();
    NativeWorldReader reader;
    MapStorageProductionActions actions;
    actions.map_state_path = probe.state_path;
    actions.reader = &reader;
    actions.hooks.context = &probe;
    actions.hooks.require_live_world = &liveWorld;
    actions.hooks.new_command_uuid = &newUuid;
    actions.hooks.approve_placement_command = &approvePlacement;
    actions.hooks.dispatch_tracked_rpc = &sendTracked;
    actions.hooks.poll_tracked_rpc = &pollTracked;
    actions.hooks.capture_chest = &captureChest;
    actions.hooks.read_held_map = &readHeldMap;
    actions.hooks.submit_chest_place = &submitChest;
    actions.hooks.commit_cursor_fsynced = &commitCursor;
    actions.hooks.finalize_committed_map_use = &finalizeMarker;

    MapStoragePipelineDriver driver;
    auto snapshot = inputFor();
    const auto ops = MakeMapStorageProductionOps(&actions);
    auto result = driver.tick(snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(result.decision.next == MapStorageNextStep::AwaitPairJournal);

    MapPairPlacementRecord pair;
    assert(LoadMapPairPlacementJournal(probe.state_path, &pair) ==
           MapPairPlacementLoad::Loaded);
    assert(pair.phase == MapPairPlacementPhase::SupportSelected);
    assert(pair.platform_side != 0U);
    snapshot.pair = &pair;
    result = driver.tick(snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::SurveyPairSupport);
    assert(result.status == MapStorageDriveStatus::WaitingForEvidence);
    assert(probe.dispatched == 0); // unapproved fill syntax blocks dispatch

    probe.placement_approved = true;
    result = driver.tick(snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(probe.dispatched == 1);
    assert(LoadMapPairPlacementJournal(probe.state_path, &pair) ==
           MapPairPlacementLoad::Loaded);
    assert(pair.phase == MapPairPlacementPhase::SupportDispatchArmed);
    assert(driver.tick(snapshot, ops).status ==
           MapStorageDriveStatus::WaitingForEvidence); // no ACK yet
    probe.ack_timeout = true;
    result = driver.tick(snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted &&
           !result.error.empty()); // no readback: no false confirmation
    assert(probe.dispatched == 1);
    probe.ack_timeout = false;
    probe.ack_ready = true;
    probe.wrong_ack_uuid = true;
    result = driver.tick(snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted &&
           !result.error.empty()); // wrong RPC UUID pauses without resend
    assert(LoadMapPairPlacementJournal(probe.state_path, &pair) ==
           MapPairPlacementLoad::Loaded);
    assert(pair.support_retry_used); // rejection survives restart/resume
    probe.wrong_ack_uuid = false;
    probe.ack_rejected = true;
    result = driver.tick(snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted &&
           !result.error.empty()); // rejected RPC also pauses without resend
    probe.ack_rejected = false;
    probe.ack_ready = false;
    assert(driver.tick(snapshot, ops).status ==
           MapStorageDriveStatus::WaitingForEvidence); // neither proof exists
    assert(probe.dispatched == 1); // never resend Armed support

    g_blocks[positionKey(pair.pair.chest.x, pair.pair.chest.y - 1,
                         pair.pair.chest.z)] = "minecraft:stone";
    result = driver.tick(snapshot, ops);
    assert(result.status == MapStorageDriveStatus::WaitingForEvidence);
    g_blocks[positionKey(pair.pair.anvil.x, pair.pair.anvil.y - 1,
                         pair.pair.anvil.z)] = "minecraft:stone";
    std::array<MapChestPosition, 4> pair_platform;
    assert(BuildMapPairSupportPlatformCells(
        snapshot.artwork_bounds, pair.pair, pair.platform_side,
        &pair_platform));
    // Two old support cells do not prove the new four-cell platform.
    probe.ack_timeout = true;
    result = driver.tick(snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted &&
           !result.error.empty());
    probe.ack_timeout = false;
    for (size_t i = 2; i < pair_platform.size(); ++i) {
        const auto& cell = pair_platform[i];
        g_blocks[positionKey(cell.x, cell.y, cell.z)] = "minecraft:stone";
    }
    probe.wrong_ack_uuid = true;
    result = driver.tick(snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted &&
           !result.error.empty()); // exact blocks cannot override wrong UUID
    assert(probe.dispatched == 1);
    probe.wrong_ack_uuid = false;
    probe.ack_timeout = true;
    result = driver.tick(snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(result.error.empty()); // timeout + exact native state recovers
    probe.ack_timeout = false;
    assert(LoadMapPairPlacementJournal(probe.state_path, &pair) ==
           MapPairPlacementLoad::Loaded);
    assert(pair.phase == MapPairPlacementPhase::SupportConfirmed);
    result = driver.tick(snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::SurveyPairChest);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(probe.dispatched == 2);
    assert(LoadMapPairPlacementJournal(probe.state_path, &pair) ==
           MapPairPlacementLoad::Loaded);
    assert(pair.phase == MapPairPlacementPhase::ChestDispatchArmed);
    assert(driver.tick(snapshot, ops).status ==
           MapStorageDriveStatus::WaitingForEvidence); // chest still air

    g_blocks[positionKey(pair.pair.chest.x, pair.pair.chest.y,
                         pair.pair.chest.z)] = "minecraft:chest";
    result = driver.tick(snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(LoadMapPairPlacementJournal(probe.state_path, &pair) ==
           MapPairPlacementLoad::Loaded);
    assert(pair.phase == MapPairPlacementPhase::ChestConfirmed);
    result = driver.tick(snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::SurveyPairAnvil);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(probe.dispatched == 3);
    assert(LoadMapPairPlacementJournal(probe.state_path, &pair) ==
           MapPairPlacementLoad::Loaded);
    assert(pair.phase == MapPairPlacementPhase::AnvilDispatchArmed);
    assert(driver.tick(snapshot, ops).status ==
           MapStorageDriveStatus::WaitingForEvidence); // anvil still air
    g_blocks[positionKey(pair.pair.anvil.x, pair.pair.anvil.y,
                         pair.pair.anvil.z)] = "minecraft:anvil";
    result = driver.tick(snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(LoadMapPairPlacementJournal(probe.state_path, &pair) ==
           MapPairPlacementLoad::Loaded);
    assert(pair.phase == MapPairPlacementPhase::PairConfirmed);
    result = driver.tick(snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::AwaitRenameJournal);
    assert(result.status == MapStorageDriveStatus::WaitingForEvidence);
    assert(probe.dispatched == 3); // anvil crafting is not guessed here

    // The anvil implementation is intentionally external. Populate its
    // existing journal through the already-tested proof APIs, then test the
    // production chest actions from that durable confirmed state onward.
    MapAnvilRenameRecord rename;
    rename.world_id = snapshot.world_id;
    rename.dimension_id = snapshot.dimension_id;
    rename.tile_cursor = 0;
    rename.tile_count = snapshot.tile_count;
    rename.columns = snapshot.columns;
    rename.rows = snapshot.rows;
    rename.anvil_x = pair.pair.anvil.x;
    rename.anvil_y = pair.pair.anvil.y;
    rename.anvil_z = pair.pair.anvil.z;
    rename.map_runtime_item_id = 358;
    rename.map_uuid = snapshot.expected_map_uuid;
    rename.input_source_network_stack_id = 71;
    assert(FormatMapTileName(0, 2, 1, &rename.expected_title));
    std::string error;
    assert(BeginMapAnvilRenameJournal(probe.state_path, rename, &error));
    MapAnvilResponseCorrelation input_correlation;
    input_correlation.session_generation = 8;
    input_correlation.response_generation_before_send = 20;
    input_correlation.window_token = 10;
    input_correlation.window_id = 3;
    input_correlation.source_hotbar_slot = 6;
    assert(ArmMapAnvilInputDispatch(probe.state_path, rename, -9,
                                     input_correlation, &error));
    assert(LoadMapAnvilRenameJournal(probe.state_path, &rename) ==
           MapAnvilRenameLoad::Loaded);
    assert(NoteMapAnvilInputAccepted(probe.state_path, rename, -9, &error));
    assert(LoadMapAnvilRenameJournal(probe.state_path, &rename) ==
           MapAnvilRenameLoad::Loaded);
    MapAnvilInputProof input_proof;
    input_proof.world_id = rename.world_id;
    input_proof.dimension_id = rename.dimension_id;
    input_proof.anvil_x = rename.anvil_x;
    input_proof.anvil_y = rename.anvil_y;
    input_proof.anvil_z = rename.anvil_z;
    input_proof.fresh_window_token = 10;
    input_proof.window_id = 3;
    input_proof.input_slot = 1;
    input_proof.count = 1;
    input_proof.runtime_item_id = 358;
    input_proof.network_stack_id = 71;
    input_proof.map_uuid = rename.map_uuid;
    input_proof.native_readback = true;
    assert(ConfirmMapAnvilInput(probe.state_path, rename, input_proof, &error));
    assert(LoadMapAnvilRenameJournal(probe.state_path, &rename) ==
           MapAnvilRenameLoad::Loaded);
    assert(ArmMapAnvilCraftClick(probe.state_path, rename, input_proof, &error));
    assert(LoadMapAnvilRenameJournal(probe.state_path, &rename) ==
           MapAnvilRenameLoad::Loaded);
    MapAnvilResponseCorrelation craft_correlation = input_correlation;
    craft_correlation.response_generation_before_send = 21;
    craft_correlation.source_hotbar_slot = -1;
    assert(ArmMapAnvilCraftDispatch(probe.state_path, rename,
                                     input_proof, -11,
                                     craft_correlation, 1U, &error));
    assert(LoadMapAnvilRenameJournal(probe.state_path, &rename) ==
           MapAnvilRenameLoad::Loaded);
    assert(NoteMapAnvilCraftAccepted(probe.state_path, rename, -11, 72, &error));
    assert(LoadMapAnvilRenameJournal(probe.state_path, &rename) ==
           MapAnvilRenameLoad::Loaded);
    MapAnvilOutputProof output_proof;
    output_proof.world_id = rename.world_id;
    output_proof.dimension_id = rename.dimension_id;
    output_proof.count = 1;
    output_proof.runtime_item_id = 358;
    output_proof.network_stack_id = 72;
    output_proof.map_uuid = rename.map_uuid;
    output_proof.name_source = MapItemNameSource::DisplayName;
    output_proof.name = rename.expected_title;
    output_proof.native_readback = true;
    assert(ConfirmMapAnvilRenamedOutput(probe.state_path, rename,
                                         output_proof, &error));
    assert(LoadMapAnvilRenameJournal(probe.state_path, &rename) ==
           MapAnvilRenameLoad::Loaded);
    assert(rename.phase == MapAnvilRenamePhase::RenamedMapConfirmed);
    snapshot.rename = &rename;

    result = driver.tick(snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::AwaitChestJournal);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    MapChestTransferRecord chest;
    assert(LoadMapChestTransferJournal(probe.state_path, &chest) ==
           MapChestJournalLoad::Loaded);
    assert(chest.phase == MapChestTransferPhase::Prepared);
    snapshot.chest = &chest;
    probe.capture_token = 103U; // old captured window expired before send
    result = driver.tick(snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::PreflightChestTransfer);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(!probe.map_submitted); // Prepared refresh is journal-only
    assert(LoadMapChestTransferJournal(probe.state_path, &chest) ==
           MapChestJournalLoad::Loaded);
    assert(chest.phase == MapChestTransferPhase::Prepared &&
           chest.pre_send_capture_token == 103U);
    result = driver.tick(snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(probe.map_submitted);
    assert(probe.chest_place_count == 1);
    assert(LoadMapChestTransferJournal(probe.state_path, &chest) ==
           MapChestJournalLoad::Loaded);
    assert(chest.phase == MapChestTransferPhase::ResponseAccepted);

    result = driver.tick(snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::ReopenChestNoResend);
    assert(result.status == MapStorageDriveStatus::WaitingForEvidence);
    const int open_count_after_place = probe.chest_open_count;
    assert(probe.chest_close_count == 1);

    // Simulate a process restart with only the durable phase-3 accepted
    // response remaining. The original chest window no longer exists, so
    // reconciliation must settle without another Open, Place or full scan.
    const auto resumed_state_path =
        (directory / "resumed_map_creation.state").string();
    assert(std::filesystem::copy_file(
        MapPairPlacementJournalPath(probe.state_path),
        MapPairPlacementJournalPath(resumed_state_path)));
    assert(std::filesystem::copy_file(
        MapAnvilRenameJournalPath(probe.state_path),
        MapAnvilRenameJournalPath(resumed_state_path)));
    assert(std::filesystem::copy_file(
        MapChestTransferJournalPath(probe.state_path),
        MapChestTransferJournalPath(resumed_state_path)));
    Probe resumed_probe = probe;
    resumed_probe.state_path = resumed_state_path;
    resumed_probe.no_live_original_window = true;
    MapStorageProductionActions resumed_actions = actions;
    resumed_actions.map_state_path = resumed_state_path;
    resumed_actions.hooks.context = &resumed_probe;
    auto resumed_snapshot = snapshot;
    MapStoragePipelineDriver resumed_driver;
    const auto resumed_ops = MakeMapStorageProductionOps(&resumed_actions);
    auto resumed_result = resumed_driver.tick(resumed_snapshot, resumed_ops);
    assert(resumed_result.decision.next == MapStorageNextStep::ReopenChestNoResend);
    assert(resumed_result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(resumed_probe.chest_open_count == open_count_after_place);
    assert(resumed_probe.chest_place_count == 1);
    MapChestTransferRecord resumed_chest;
    assert(LoadMapChestTransferJournal(resumed_state_path, &resumed_chest) ==
           MapChestJournalLoad::Loaded);
    assert(resumed_chest.phase == MapChestTransferPhase::AcceptedAndClosed);
    resumed_snapshot.chest = &resumed_chest;
    resumed_result = resumed_driver.tick(resumed_snapshot, resumed_ops);
    assert(resumed_result.decision.next == MapStorageNextStep::CommitTileCursor);
    assert(resumed_result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(resumed_probe.durable_cursor == 1U);

    // Normal live completion waits for the original visible close, then
    // commits using the same accepted response and no second chest Open.
    probe.close_ready = true;
    result = driver.tick(snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(probe.chest_open_count == open_count_after_place);
    assert(probe.chest_place_count == 1);
    assert(LoadMapChestTransferJournal(probe.state_path, &chest) ==
           MapChestJournalLoad::Loaded);
    assert(chest.phase == MapChestTransferPhase::AcceptedAndClosed);

    result = driver.tick(snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::CommitTileCursor);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(probe.durable_cursor == 1U);
    snapshot.checkpoint_tile_cursor = 1;
    snapshot.expected_map_uuid = -1; // item now lives in the chest
    result = driver.tick(snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::ClearCommittedRenameJournal);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(LoadMapAnvilRenameJournal(probe.state_path, &rename) ==
           MapAnvilRenameLoad::Missing);
    snapshot.rename = nullptr;
    result = driver.tick(snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::ClearCommittedChestJournal);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(LoadMapChestTransferJournal(probe.state_path, &chest) ==
           MapChestJournalLoad::Missing);
    snapshot.chest = nullptr;
    result = driver.tick(snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::FinalizeCommittedMapUse);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(!probe.marker_present);
    snapshot.has_pending_map_use_marker = false;
    result = driver.tick(snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::AwaitMapUseMarker);
    assert(result.status == MapStorageDriveStatus::WaitingForEvidence);

    // A 28th tile must select a separate persisted chest. Its Armed fills
    // can recover from exact native stone/chest readback without an ACK.
    probe.state_path = (directory / "extra_map.state").string();
    actions.map_state_path = probe.state_path;
    auto extra_snapshot = inputFor();
    extra_snapshot.columns = 28;
    extra_snapshot.tile_count = 28;
    extra_snapshot.checkpoint_tile_cursor = 27;
    extra_snapshot.pending_map_use_cursor = 27;
    MapPairPlacementRecord extra_pair = pair;
    extra_pair.tile_count = 28;
    extra_pair.phase = MapPairPlacementPhase::Selected;
    extra_pair.support_created = false;
    extra_pair.support_retry_used = false;
    extra_pair.platform_side = 0U;  // this fixture models a legacy pair
    extra_pair.active_command_uuid.clear();
    assert(BeginMapPairPlacementJournal(probe.state_path, extra_pair, &error));
    assert(ArmMapPairPlacementDispatch(probe.state_path, extra_pair,
                                       MapPairPlacementStep::Chest,
                                       "extra_pair_chest", &error));
    assert(LoadMapPairPlacementJournal(probe.state_path, &extra_pair) ==
           MapPairPlacementLoad::Loaded);
    NativeBlockInfo confirmed_chest;
    confirmed_chest.name = "minecraft:chest";
    NativeBlockInfo confirmed_anvil;
    confirmed_anvil.name = "minecraft:anvil";
    assert(ConfirmMapPairPlacementStep(
        probe.state_path, extra_pair, MapPairPlacementStep::Chest,
        "extra_pair_chest", MapPairCommandAck::Accepted,
        &confirmed_chest, nullptr, &error));
    assert(LoadMapPairPlacementJournal(probe.state_path, &extra_pair) ==
           MapPairPlacementLoad::Loaded);
    assert(ArmMapPairPlacementDispatch(probe.state_path, extra_pair,
                                       MapPairPlacementStep::Anvil,
                                       "extra_pair_anvil", &error));
    assert(LoadMapPairPlacementJournal(probe.state_path, &extra_pair) ==
           MapPairPlacementLoad::Loaded);
    assert(ConfirmMapPairPlacementStep(
        probe.state_path, extra_pair, MapPairPlacementStep::Anvil,
        "extra_pair_anvil", MapPairCommandAck::Accepted,
        &confirmed_chest, &confirmed_anvil, &error));
    assert(LoadMapPairPlacementJournal(probe.state_path, &extra_pair) ==
           MapPairPlacementLoad::Loaded);
    extra_snapshot.pair = &extra_pair;
    MapStoragePipelineDriver extra_driver;
    result = extra_driver.tick(extra_snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::AwaitExtraChestJournal);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    MapExtraChestPlacementRecord extra;
    assert(LoadMapExtraChestPlacementJournal(probe.state_path, 1, &extra) ==
           MapExtraChestPlacementLoad::Loaded);
    assert(extra.phase == MapExtraChestPlacementPhase::SupportSelected);
    assert(extra.platform_corner != 0U);
    extra_snapshot.extra_chests.push_back(extra);
    result = extra_driver.tick(extra_snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::SurveyExtraSupport);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(LoadMapExtraChestPlacementJournal(probe.state_path, 1, &extra) ==
           MapExtraChestPlacementLoad::Loaded);
    assert(extra.phase == MapExtraChestPlacementPhase::SupportDispatchArmed);
    extra_snapshot.extra_chests[0] = extra;
    result = extra_driver.tick(extra_snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::ReconcileExtraSupportNoResend);
    assert(result.status == MapStorageDriveStatus::WaitingForEvidence);
    actions.hooks.allow_support_retry = &allowSupportRetry;
    probe.ack_timeout = true;
    probe.support_retry_ready = false;
    const int before_extra_retry = probe.dispatched;
    result = extra_driver.tick(extra_snapshot, ops);
    assert(result.status == MapStorageDriveStatus::WaitingForEvidence);
    assert(probe.dispatched == before_extra_retry);
    probe.support_retry_ready = true;
    result = extra_driver.tick(extra_snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(result.error.empty());
    assert(probe.dispatched == before_extra_retry + 1);
    assert(LoadMapExtraChestPlacementJournal(probe.state_path, 1, &extra) ==
           MapExtraChestPlacementLoad::Loaded);
    assert(extra.phase == MapExtraChestPlacementPhase::SupportDispatchArmed);
    assert(extra.support_retry_used);
    extra_snapshot.extra_chests[0] = extra;
    result = extra_driver.tick(extra_snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted &&
           !result.error.empty()); // the second timeout cannot retry again
    assert(probe.dispatched == before_extra_retry + 1);
    g_blocks[positionKey(extra.chest.x, extra.chest.y - 1, extra.chest.z)] =
        "minecraft:stone";
    std::array<MapChestPosition, 4> extra_platform;
    assert(BuildMapExtraSupportPlatformCells(
        extra_snapshot.artwork_bounds, extra.chest,
        extra.platform_corner, &extra_platform));
    for (size_t i = 1; i < extra_platform.size(); ++i) {
        const auto& cell = extra_platform[i];
        g_blocks[positionKey(cell.x, cell.y, cell.z)] = "minecraft:stone";
    }
    result = extra_driver.tick(extra_snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(LoadMapExtraChestPlacementJournal(probe.state_path, 1, &extra) ==
           MapExtraChestPlacementLoad::Loaded);
    assert(extra.phase == MapExtraChestPlacementPhase::SupportConfirmed);
    probe.ack_timeout = false;
    extra_snapshot.extra_chests[0] = extra;
    result = extra_driver.tick(extra_snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::SurveyExtraChest);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(LoadMapExtraChestPlacementJournal(probe.state_path, 1, &extra) ==
           MapExtraChestPlacementLoad::Loaded);
    assert(extra.phase == MapExtraChestPlacementPhase::DispatchArmed);
    extra_snapshot.extra_chests[0] = extra;
    result = extra_driver.tick(extra_snapshot, ops);
    assert(result.decision.next == MapStorageNextStep::ReconcileExtraChestNoResend);
    assert(result.status == MapStorageDriveStatus::WaitingForEvidence);
    g_blocks[positionKey(extra.chest.x, extra.chest.y, extra.chest.z)] =
        "minecraft:chest";
    result = extra_driver.tick(extra_snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(LoadMapExtraChestPlacementJournal(probe.state_path, 1, &extra) ==
           MapExtraChestPlacementLoad::Loaded);
    assert(extra.phase == MapExtraChestPlacementPhase::Confirmed);

    // A prior support attempt with missing ACK and exact air can be retried
    // once, but only after the runtime's explicit near-site guard is ready.
    g_blocks.clear();
    probe.state_path = (directory / "pair_retry_map.state").string();
    actions.map_state_path = probe.state_path;
    probe.ack_ready = false;
    probe.ack_timeout = false;
    probe.support_retry_ready = false;
    auto retry_snapshot = inputFor();
    MapStoragePipelineDriver retry_driver;
    result = retry_driver.tick(retry_snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    MapPairPlacementRecord retry_pair;
    assert(LoadMapPairPlacementJournal(probe.state_path, &retry_pair) ==
           MapPairPlacementLoad::Loaded);
    retry_snapshot.pair = &retry_pair;
    result = retry_driver.tick(retry_snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(LoadMapPairPlacementJournal(probe.state_path, &retry_pair) ==
           MapPairPlacementLoad::Loaded);
    const std::string original_uuid = retry_pair.active_command_uuid;
    const int before_pair_retry = probe.dispatched;
    probe.ack_timeout = true;
    result = retry_driver.tick(retry_snapshot, ops);
    assert(result.status == MapStorageDriveStatus::WaitingForEvidence);
    assert(probe.dispatched == before_pair_retry);
    probe.support_retry_ready = true;
    result = retry_driver.tick(retry_snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(result.error.empty());
    assert(probe.dispatched == before_pair_retry + 1);
    assert(LoadMapPairPlacementJournal(probe.state_path, &retry_pair) ==
           MapPairPlacementLoad::Loaded);
    assert(retry_pair.phase == MapPairPlacementPhase::SupportDispatchArmed);
    assert(retry_pair.support_retry_used);
    assert(retry_pair.active_command_uuid != original_uuid);
    result = retry_driver.tick(retry_snapshot, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted &&
           !result.error.empty());
    assert(probe.dispatched == before_pair_retry + 1);
    return 0;
}
