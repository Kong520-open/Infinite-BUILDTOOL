#include "../MapStoragePipelineDriver.h"
#include "../MapTileNaming.h"
#include "../NativeWorldAccess.h"

#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

using namespace build_import;

// Linked journal/survey implementations contain unrelated world readers.
// The driver never calls these host-test stubs.
namespace build_import {
bool NativeWorldReader::getBlock(int32_t, int32_t, int32_t,
                                 NativeBlockInfo*) { return false; }
bool NativeWorldAccess::getBlock(int32_t, int32_t, int32_t,
                                 NativeBlockInfo*) { return false; }
}

namespace {

constexpr int64_t kUuid = -532575944698LL;

MapStorageCoordinatorInput inputFor(uint64_t cursor = 0,
                                    uint32_t columns = 2, uint32_t rows = 1) {
    MapStorageCoordinatorInput input;
    input.world_id = "stable:v1:driver-test|0";
    input.dimension_id = 0;
    input.artwork_bounds = {0, 20, 0, 20, 20, 20};
    input.columns = columns;
    input.rows = rows;
    input.tile_count = static_cast<uint64_t>(columns) * rows;
    input.checkpoint_tile_cursor = cursor;
    input.has_pending_map_use_marker = cursor < input.tile_count;
    input.pending_map_use_cursor = cursor;
    input.expected_map_uuid = kUuid;
    return input;
}

MapPairPlacementRecord confirmedPair(const MapStorageCoordinatorInput& input) {
    MapPairPlacementRecord pair;
    pair.world_id = input.world_id;
    pair.dimension_id = input.dimension_id;
    pair.tile_count = input.tile_count;
    pair.artwork_bounds = input.artwork_bounds;
    pair.pair.chest = {30, 21, 0};
    pair.pair.anvil = {31, 21, 0};
    pair.phase = MapPairPlacementPhase::PairConfirmed;
    return pair;
}

MapAnvilRenameRecord renamed(const MapStorageCoordinatorInput& input) {
    MapAnvilRenameRecord rename;
    rename.world_id = input.world_id;
    rename.dimension_id = input.dimension_id;
    rename.tile_cursor = input.checkpoint_tile_cursor;
    rename.tile_count = input.tile_count;
    rename.columns = input.columns;
    rename.rows = input.rows;
    rename.anvil_x = 31;
    rename.anvil_y = 21;
    rename.map_runtime_item_id = 358;
    rename.map_uuid = kUuid;
    rename.input_source_network_stack_id = 71;
    rename.input_request_id = -9;
    rename.input_response = {8, 20, 14, 3, 6};
    rename.craft_input_network_stack_id = 81;
    rename.craft_request_id = -11;
    rename.craft_destination_hotbar_slot = 1;
    rename.craft_response = {8, 21, 14, 3, -1};
    rename.craft_accepted_output_network_stack_id = 72;
    rename.renamed_network_stack_id = 72;
    rename.phase = MapAnvilRenamePhase::RenamedMapConfirmed;
    assert(FormatMapTileName(rename.tile_cursor, rename.columns,
                             rename.rows, &rename.expected_title));
    return rename;
}

MapChestTransferRecord stored(const MapStorageCoordinatorInput& input) {
    MapChestTransferRecord chest;
    chest.world_id = input.world_id;
    chest.dimension_id = input.dimension_id;
    chest.tile_cursor = input.checkpoint_tile_cursor;
    chest.tile_count = input.tile_count;
    chest.chest_x = 30;
    chest.chest_y = 21;
    chest.chest_z = 0;
    chest.chest_index = 0;
    chest.chest_slot = static_cast<uint8_t>(input.checkpoint_tile_cursor);
    chest.source_runtime_item_id = 358;
    chest.source_network_stack_id = 72;
    chest.source_map_uuid = kUuid;
    chest.pre_send_capture_token = 7;
    chest.request_id = -13;
    chest.phase = MapChestTransferPhase::ReopenConfirmed;
    return chest;
}

struct Probe {
    int transitions = 0;
    int reconciles = 0;
    MapStorageNextStep last = MapStorageNextStep::Unsafe;
    MapStorageExternalResult transition_result =
        MapStorageExternalResult::DurableTransition;
    MapStorageExternalResult reconcile_result =
        MapStorageExternalResult::NoChange;
    bool throw_transition = false;
};

MapStorageExternalResult transition(void* context, MapStorageNextStep step,
        const MapStorageCoordinatorInput&,
        const MapStorageCoordinatorDecision&, std::string*) {
    auto* probe = static_cast<Probe*>(context);
    ++probe->transitions;
    probe->last = step;
    if (probe->throw_transition) throw std::runtime_error("uncertain send");
    return probe->transition_result;
}

MapStorageExternalResult reconcile(void* context, MapStorageNextStep step,
        const MapStorageCoordinatorInput&,
        const MapStorageCoordinatorDecision&, std::string*) {
    auto* probe = static_cast<Probe*>(context);
    ++probe->reconciles;
    probe->last = step;
    return probe->reconcile_result;
}

MapStoragePipelineOps opsFor(Probe* probe) {
    return {probe, &transition, &reconcile};
}

}  // namespace

int main() {
    Probe probe;
    MapStoragePipelineDriver driver;
    auto input = inputFor();
    auto ops = opsFor(&probe);
    auto result = driver.tick(input, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(result.decision.next == MapStorageNextStep::AwaitPairJournal);
    assert(probe.transitions == 1 && probe.reconciles == 0);
    assert(driver.tick(input, ops).status ==
           MapStorageDriveStatus::AwaitingJournalRefresh);
    assert(probe.transitions == 1); // stale snapshot cannot start pair twice

    {
        Probe support_probe;
        MapStoragePipelineDriver support_driver;
        auto support_input = input;
        MapPairPlacementRecord support_pair = confirmedPair(support_input);
        support_pair.phase = MapPairPlacementPhase::SupportSelected;
        support_pair.support_created = true;
        support_input.pair = &support_pair;
        const auto support_ops = opsFor(&support_probe);
        auto support_result = support_driver.tick(support_input, support_ops);
        assert(support_result.decision.next ==
               MapStorageNextStep::SurveyPairSupport);
        assert(support_probe.transitions == 1 && support_probe.reconciles == 0);
        assert(support_driver.tick(support_input, support_ops).status ==
               MapStorageDriveStatus::AwaitingJournalRefresh);
        support_pair.phase = MapPairPlacementPhase::SupportDispatchArmed;
        support_pair.active_command_uuid = "support_uuid";
        support_result = support_driver.tick(support_input, support_ops);
        assert(support_result.decision.next ==
               MapStorageNextStep::ReconcilePairSupportNoResend);
        assert(support_probe.transitions == 1 && support_probe.reconciles == 1);
        support_pair.phase = MapPairPlacementPhase::SupportConfirmed;
        support_pair.active_command_uuid.clear();
        support_result = support_driver.tick(support_input, support_ops);
        assert(support_result.decision.next ==
               MapStorageNextStep::SurveyPairChest);
        assert(support_probe.transitions == 2);
    }

    MapPairPlacementRecord pair = confirmedPair(input);
    pair.phase = MapPairPlacementPhase::ChestDispatchArmed;
    input.pair = &pair;
    result = driver.tick(input, ops);
    assert(result.status == MapStorageDriveStatus::WaitingForEvidence);
    assert(result.decision.next == MapStorageNextStep::ReconcilePairChestNoResend);
    assert(probe.transitions == 1 && probe.reconciles == 1);
    assert(driver.tick(input, ops).status == MapStorageDriveStatus::WaitingForEvidence);
    assert(probe.transitions == 1 && probe.reconciles == 2);

    pair.phase = MapPairPlacementPhase::PairConfirmed;
    result = driver.tick(input, ops);
    assert(result.decision.next == MapStorageNextStep::AwaitRenameJournal);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(probe.transitions == 2);

    MapAnvilRenameRecord rename = renamed(input);
    rename.phase = MapAnvilRenamePhase::InputResponseAccepted;
    rename.craft_request_id = 0;
    rename.renamed_network_stack_id = 0;
    input.rename = &rename;
    result = driver.tick(input, ops);
    assert(result.decision.next == MapStorageNextStep::ReconcileAnvilInputNoResend);
    assert(probe.transitions == 2 && probe.reconciles == 3);
    rename = renamed(input);
    result = driver.tick(input, ops);
    assert(result.decision.next == MapStorageNextStep::AwaitChestJournal);
    assert(probe.transitions == 3);

    MapChestTransferRecord chest = stored(input);
    chest.phase = MapChestTransferPhase::ResponseAccepted;
    input.chest = &chest;
    result = driver.tick(input, ops);
    assert(result.decision.next == MapStorageNextStep::ReopenChestNoResend);
    assert(probe.transitions == 3 && probe.reconciles == 4);
    chest.phase = MapChestTransferPhase::ReopenConfirmed;
    result = driver.tick(input, ops);
    assert(result.decision.next == MapStorageNextStep::CommitTileCursor);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(input.checkpoint_tile_cursor == 0); // callback owns durable commit
    assert(driver.tick(input, ops).status ==
           MapStorageDriveStatus::AwaitingJournalRefresh);
    assert(probe.transitions == 4);

    input.checkpoint_tile_cursor = 1;
    input.expected_map_uuid = -1; // no longer held once stored
    result = driver.tick(input, ops);
    assert(result.decision.next == MapStorageNextStep::ClearCommittedRenameJournal);
    assert(probe.transitions == 5);
    input.rename = nullptr;
    result = driver.tick(input, ops);
    assert(result.decision.next == MapStorageNextStep::ClearCommittedChestJournal);
    assert(probe.transitions == 6);
    input.chest = nullptr;
    result = driver.tick(input, ops);
    assert(result.decision.next == MapStorageNextStep::FinalizeCommittedMapUse);
    assert(probe.transitions == 7);
    input.has_pending_map_use_marker = false;
    result = driver.tick(input, ops);
    assert(result.decision.next == MapStorageNextStep::AwaitMapUseMarker);
    assert(result.status == MapStorageDriveStatus::WaitingForEvidence);
    assert(probe.transitions == 7);
    input.has_pending_map_use_marker = true;
    input.pending_map_use_cursor = 1;
    input.expected_map_uuid = kUuid;
    result = driver.tick(input, ops);
    assert(result.decision.next == MapStorageNextStep::AwaitRenameJournal);
    assert(probe.transitions == 8);

    MapStoragePipelineDriver uncertain_driver;
    probe.throw_transition = true;
    result = uncertain_driver.tick(input, ops);
    assert(result.status == MapStorageDriveStatus::OneTransitionAttempted);
    assert(!result.error.empty());
    const int before_uncertain_retry = probe.transitions;
    assert(uncertain_driver.tick(input, ops).status ==
           MapStorageDriveStatus::AwaitingJournalRefresh);
    assert(probe.transitions == before_uncertain_retry);
    probe.throw_transition = false;

    auto completed = inputFor(2);
    pair = confirmedPair(completed);
    completed.pair = &pair;
    result = driver.tick(completed, ops);
    assert(result.decision.next == MapStorageNextStep::ClearPairJournal);
    completed.pair = nullptr;
    result = driver.tick(completed, ops);
    assert(result.status == MapStorageDriveStatus::Complete);

    auto wrong_world = inputFor();
    pair = confirmedPair(wrong_world);
    pair.world_id = "another-world";
    wrong_world.pair = &pair;
    const int before_unsafe = probe.transitions;
    result = driver.tick(wrong_world, ops);
    assert(result.status == MapStorageDriveStatus::Unsafe);
    assert(probe.transitions == before_unsafe);

    // Crossing the 27-slot boundary creates one extra chest before the map
    // rename may begin. Its ambiguous placement is reconciliation-only.
    auto many = inputFor(27, 28, 1);
    pair = confirmedPair(many);
    many.pair = &pair;
    Probe many_probe;
    MapStoragePipelineDriver many_driver;
    auto many_ops = opsFor(&many_probe);
    result = many_driver.tick(many, many_ops);
    assert(result.decision.next == MapStorageNextStep::AwaitExtraChestJournal);
    assert(many_probe.transitions == 1);
    MapExtraChestPlacementRecord extra;
    extra.world_id = many.world_id;
    extra.dimension_id = many.dimension_id;
    extra.tile_count = many.tile_count;
    extra.artwork_bounds = many.artwork_bounds;
    extra.chest_index = 1;
    extra.chest = {33, 21, 0};
    extra.phase = MapExtraChestPlacementPhase::SupportSelected;
    extra.support_created = true;
    many.extra_chests.push_back(extra);
    result = many_driver.tick(many, many_ops);
    assert(result.decision.next == MapStorageNextStep::SurveyExtraSupport);
    assert(many_probe.transitions == 2);
    many.extra_chests[0].phase =
        MapExtraChestPlacementPhase::SupportDispatchArmed;
    result = many_driver.tick(many, many_ops);
    assert(result.decision.next ==
           MapStorageNextStep::ReconcileExtraSupportNoResend);
    assert(many_probe.transitions == 2 && many_probe.reconciles == 1);
    many.extra_chests[0].phase = MapExtraChestPlacementPhase::SupportConfirmed;
    result = many_driver.tick(many, many_ops);
    assert(result.decision.next == MapStorageNextStep::SurveyExtraChest);
    assert(many_probe.transitions == 3);
    // A legacy selected extra-chest sidecar still resumes its chest send.
    extra.phase = MapExtraChestPlacementPhase::Selected;
    extra.support_created = false;
    many.extra_chests[0] = extra;
    result = many_driver.tick(many, many_ops);
    assert(result.decision.next == MapStorageNextStep::SurveyExtraChest);
    assert(many_probe.transitions == 3);
    many.extra_chests[0].phase = MapExtraChestPlacementPhase::DispatchArmed;
    result = many_driver.tick(many, many_ops);
    assert(result.decision.next == MapStorageNextStep::ReconcileExtraChestNoResend);
    assert(many_probe.transitions == 3 && many_probe.reconciles == 2);
    many.extra_chests[0].phase = MapExtraChestPlacementPhase::Confirmed;
    result = many_driver.tick(many, many_ops);
    assert(result.decision.next == MapStorageNextStep::AwaitRenameJournal);

    auto all_stored = inputFor(55, 55, 1);
    pair = confirmedPair(all_stored);
    all_stored.pair = &pair;
    extra.world_id = all_stored.world_id;
    extra.tile_count = all_stored.tile_count;
    extra.artwork_bounds = all_stored.artwork_bounds;
    extra.phase = MapExtraChestPlacementPhase::Confirmed;
    all_stored.extra_chests.push_back(extra);
    extra.chest_index = 2;
    extra.chest = {36, 21, 0};
    all_stored.extra_chests.push_back(extra);
    Probe cleanup_probe;
    MapStoragePipelineDriver cleanup_driver;
    auto cleanup_ops = opsFor(&cleanup_probe);
    result = cleanup_driver.tick(all_stored, cleanup_ops);
    assert(result.decision.next ==
           MapStorageNextStep::ClearExtraChestJournalsDescending);
    assert(cleanup_probe.transitions == 1);
    all_stored.extra_chests.pop_back();
    result = cleanup_driver.tick(all_stored, cleanup_ops);
    assert(result.decision.next ==
           MapStorageNextStep::ClearExtraChestJournalsDescending);
    assert(cleanup_probe.transitions == 2); // size changed: next journal only
    all_stored.extra_chests.clear();
    assert(cleanup_driver.tick(all_stored, cleanup_ops).decision.next ==
           MapStorageNextStep::ClearPairJournal);
    return 0;
}
