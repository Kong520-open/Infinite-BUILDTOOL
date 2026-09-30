#include "../MapStorageCoordinator.h"
#include "../MapTileNaming.h"
#include "../NativeWorldAccess.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace build_import;

// The host test links the journal implementations but never performs a live
// world survey. Their unused Android-world helpers still need link targets on
// MinGW, so keep these two read-only stubs local to this test executable.
namespace build_import {
bool NativeWorldReader::getBlock(int32_t, int32_t, int32_t,
                                 NativeBlockInfo*) { return false; }
bool NativeWorldAccess::getBlock(int32_t, int32_t, int32_t,
                                 NativeBlockInfo*) { return false; }
}

namespace {

constexpr int64_t kMapUuid = -532575944698LL;

MapStorageCoordinatorInput base(uint64_t cursor = 0,
                                uint32_t columns = 2, uint32_t rows = 1) {
    MapStorageCoordinatorInput input;
    input.world_id = "stable:v1:storage-test|0";
    input.dimension_id = 0;
    input.artwork_bounds = {0, 20, 0, 20, 20, 20};
    input.columns = columns;
    input.rows = rows;
    input.tile_count = static_cast<uint64_t>(columns) * rows;
    input.checkpoint_tile_cursor = cursor;
    input.has_pending_map_use_marker = cursor < input.tile_count;
    input.pending_map_use_cursor = cursor;
    input.expected_map_uuid = kMapUuid;
    return input;
}

MapPairPlacementRecord pairFor(const MapStorageCoordinatorInput& input,
                               MapPairPlacementPhase phase) {
    MapPairPlacementRecord pair;
    pair.world_id = input.world_id;
    pair.dimension_id = input.dimension_id;
    pair.tile_count = input.tile_count;
    pair.artwork_bounds = input.artwork_bounds;
    pair.pair.chest = {30, 21, 0};
    pair.pair.anvil = {31, 21, 0};
    pair.phase = phase;
    return pair;
}

MapAnvilRenameRecord renameFor(const MapStorageCoordinatorInput& input,
                               MapAnvilRenamePhase phase) {
    MapAnvilRenameRecord rename;
    rename.world_id = input.world_id;
    rename.dimension_id = input.dimension_id;
    rename.tile_cursor = input.checkpoint_tile_cursor;
    rename.tile_count = input.tile_count;
    rename.columns = input.columns;
    rename.rows = input.rows;
    rename.anvil_x = 31;
    rename.anvil_y = 21;
    rename.anvil_z = 0;
    rename.map_runtime_item_id = 358;
    rename.map_uuid = kMapUuid;
    rename.input_source_network_stack_id = 71;
    rename.phase = phase;
    rename.input_request_id = phase == MapAnvilRenamePhase::Prepared ? 0 : -9;
    if (phase != MapAnvilRenamePhase::Prepared) {
        rename.input_response = {8, 20, 14, 3, 6};
    }
    const bool click_armed =
        phase == MapAnvilRenamePhase::CraftClickArmed ||
        phase == MapAnvilRenamePhase::CraftDispatchArmed ||
        phase == MapAnvilRenamePhase::CraftResponseAccepted ||
        phase == MapAnvilRenamePhase::RenamedMapConfirmed;
    if (click_armed) rename.craft_input_network_stack_id = 81;
    const bool craft_dispatched =
        phase == MapAnvilRenamePhase::CraftDispatchArmed ||
        phase == MapAnvilRenamePhase::CraftResponseAccepted ||
        phase == MapAnvilRenamePhase::RenamedMapConfirmed;
    if (craft_dispatched) {
        rename.craft_request_id = -11;
        rename.craft_destination_hotbar_slot = 1;
        rename.craft_response = {8, 21, 14, 3, -1};
    }
    if (phase == MapAnvilRenamePhase::CraftResponseAccepted ||
        phase == MapAnvilRenamePhase::RenamedMapConfirmed) {
        rename.craft_accepted_output_network_stack_id = 72;
    }
    rename.renamed_network_stack_id =
        phase == MapAnvilRenamePhase::RenamedMapConfirmed ? 72 : 0;
    assert(FormatMapTileName(rename.tile_cursor, rename.columns,
                             rename.rows, &rename.expected_title));
    return rename;
}

MapAnvilInputProof nativeInputFor(const MapAnvilRenameRecord& rename) {
    MapAnvilInputProof proof;
    proof.world_id = rename.world_id;
    proof.dimension_id = rename.dimension_id;
    proof.anvil_x = rename.anvil_x;
    proof.anvil_y = rename.anvil_y;
    proof.anvil_z = rename.anvil_z;
    proof.fresh_window_token = 14;
    proof.window_id = 3;
    proof.input_slot = 1;
    proof.count = 1;
    proof.runtime_item_id = rename.map_runtime_item_id;
    proof.network_stack_id = rename.input_source_network_stack_id + 10;
    proof.map_uuid = rename.map_uuid;
    proof.native_readback = true;
    return proof;
}

MapChestTransferRecord chestFor(const MapStorageCoordinatorInput& input,
                                MapChestTransferPhase phase,
                                MapChestPosition position = {30, 21, 0}) {
    MapChestTransferRecord chest;
    chest.world_id = input.world_id;
    chest.dimension_id = input.dimension_id;
    chest.tile_cursor = input.checkpoint_tile_cursor;
    chest.tile_count = input.tile_count;
    chest.chest_x = position.x;
    chest.chest_y = position.y;
    chest.chest_z = position.z;
    chest.chest_index = static_cast<uint16_t>(chest.tile_cursor / 27U);
    chest.chest_slot = static_cast<uint8_t>(chest.tile_cursor % 27U);
    chest.source_runtime_item_id = 358;
    chest.source_network_stack_id = 72;
    chest.source_map_uuid = kMapUuid;
    chest.pre_send_capture_token = 7;
    chest.request_id = phase == MapChestTransferPhase::Prepared ? 0 : -13;
    chest.phase = phase;
    return chest;
}

MapStorageNextStep next(const MapStorageCoordinatorInput& input) {
    MapStorageCoordinatorDecision decision;
    std::string error;
    const bool classified = ClassifyMapStorageCoordinator(input, &decision, &error);
    if (!classified) {
        std::fprintf(stderr, "classification failed: cursor=%llu error=%s\n",
                     static_cast<unsigned long long>(input.checkpoint_tile_cursor),
                     error.c_str());
    }
    assert(classified);
    assert(error.empty());
    return decision.next;
}

void unsafe(const MapStorageCoordinatorInput& input) {
    MapStorageCoordinatorDecision decision;
    std::string error;
    assert(!ClassifyMapStorageCoordinator(input, &decision, &error));
    assert(decision.next == MapStorageNextStep::Unsafe);
    assert(!error.empty());
}

}  // namespace

int main() {
    auto input = base();
    assert(next(input) == MapStorageNextStep::AwaitPairJournal);
    MapPairPlacementRecord pair = pairFor(
        input, MapPairPlacementPhase::SupportSelected);
    pair.support_created = true;
    input.pair = &pair;
    assert(next(input) == MapStorageNextStep::SurveyPairSupport);
    pair.phase = MapPairPlacementPhase::SupportDispatchArmed;
    assert(next(input) == MapStorageNextStep::ReconcilePairSupportNoResend);
    pair.phase = MapPairPlacementPhase::SupportConfirmed;
    assert(next(input) == MapStorageNextStep::SurveyPairChest);
    // Legacy journals that predate support fill remain classifiable.
    pair.phase = MapPairPlacementPhase::Selected;
    pair.support_created = false;
    assert(next(input) == MapStorageNextStep::SurveyPairChest);
    pair.phase = MapPairPlacementPhase::ChestDispatchArmed;
    assert(next(input) == MapStorageNextStep::ReconcilePairChestNoResend);
    pair.phase = MapPairPlacementPhase::ChestConfirmed;
    assert(next(input) == MapStorageNextStep::SurveyPairAnvil);
    pair.phase = MapPairPlacementPhase::AnvilDispatchArmed;
    assert(next(input) == MapStorageNextStep::ReconcilePairAnvilNoResend);
    pair.phase = MapPairPlacementPhase::PairConfirmed;
    assert(next(input) == MapStorageNextStep::AwaitRenameJournal);

    MapAnvilRenameRecord rename = renameFor(input, MapAnvilRenamePhase::Prepared);
    input.rename = &rename;
    assert(next(input) == MapStorageNextStep::PreflightAnvilInput);
    input.expected_map_uuid = -1;
    unsafe(input); // journal Prepared never substitutes for a live source
    input.expected_map_uuid = kMapUuid;
    rename = renameFor(input, MapAnvilRenamePhase::InputDispatchArmed);
    assert(next(input) == MapStorageNextStep::ReconcileAnvilInputNoResend);
    input.expected_map_uuid = -1; // map may now be in the anvil input
    assert(next(input) == MapStorageNextStep::ReconcileAnvilInputNoResend);
    input.expected_map_uuid = kMapUuid;
    rename = renameFor(input, MapAnvilRenamePhase::InputResponseAccepted);
    assert(next(input) == MapStorageNextStep::ReconcileAnvilInputNoResend);
    input.expected_map_uuid = -1;
    assert(next(input) == MapStorageNextStep::ReconcileAnvilInputNoResend);
    input.expected_map_uuid = kMapUuid;
    rename = renameFor(input, MapAnvilRenamePhase::InputConfirmed);
    unsafe(input); // held-map UUID alone is not an anvil input proof
    MapAnvilInputProof fresh_input = nativeInputFor(rename);
    input.fresh_anvil_input = &fresh_input;
    assert(next(input) == MapStorageNextStep::PreflightAnvilCraft);
    input.expected_map_uuid = -1;
    assert(next(input) == MapStorageNextStep::PreflightAnvilCraft);
    fresh_input.native_readback = false;
    unsafe(input); // copied packet/journal identity cannot authorize craft
    fresh_input.native_readback = true;
    ++fresh_input.map_uuid;
    unsafe(input);
    fresh_input.map_uuid = kMapUuid;
    fresh_input.network_stack_id = 0;
    unsafe(input);
    fresh_input.network_stack_id = rename.input_source_network_stack_id + 10;
    fresh_input.fresh_window_token = 0;
    unsafe(input);
    fresh_input.fresh_window_token = 14;
    assert(next(input) == MapStorageNextStep::PreflightAnvilCraft);
    input.fresh_anvil_input = nullptr;
    input.expected_map_uuid = kMapUuid;
    rename = renameFor(input, MapAnvilRenamePhase::CraftClickArmed);
    assert(next(input) == MapStorageNextStep::ReconcileAnvilCraftNoResend);
    input.expected_map_uuid = -1;
    assert(next(input) == MapStorageNextStep::ReconcileAnvilCraftNoResend);
    input.expected_map_uuid = kMapUuid;
    rename = renameFor(input, MapAnvilRenamePhase::CraftDispatchArmed);
    assert(next(input) == MapStorageNextStep::ReconcileAnvilCraftNoResend);
    input.expected_map_uuid = -1;
    assert(next(input) == MapStorageNextStep::ReconcileAnvilCraftNoResend);
    input.expected_map_uuid = kMapUuid;
    rename = renameFor(input, MapAnvilRenamePhase::CraftResponseAccepted);
    assert(next(input) == MapStorageNextStep::ReconcileAnvilCraftNoResend);
    input.expected_map_uuid = -1;
    assert(next(input) == MapStorageNextStep::ReconcileAnvilCraftNoResend);
    input.expected_map_uuid = kMapUuid;
    rename = renameFor(input, MapAnvilRenamePhase::RenamedMapConfirmed);
    assert(next(input) == MapStorageNextStep::AwaitChestJournal);
    input.expected_map_uuid = -1;
    unsafe(input); // confirmed journal alone cannot start a fresh chest transfer
    input.expected_map_uuid = kMapUuid; // caller needs a fresh native UUID/title scan

    MapChestTransferRecord chest = chestFor(input, MapChestTransferPhase::Prepared);
    input.chest = &chest;
    assert(next(input) == MapStorageNextStep::PreflightChestTransfer);
    input.expected_map_uuid = -1;
    unsafe(input); // Prepared does not allow another Place without live source
    input.expected_map_uuid = kMapUuid;
    chest.phase = MapChestTransferPhase::DispatchArmed;
    chest.request_id = -13;
    assert(next(input) == MapStorageNextStep::ReopenChestNoResend);
    input.expected_map_uuid = -1; // source map may already be in the chest
    assert(next(input) == MapStorageNextStep::ReopenChestNoResend);
    input.expected_map_uuid = kMapUuid;
    chest.phase = MapChestTransferPhase::ResponseAccepted;
    assert(next(input) == MapStorageNextStep::ReopenChestNoResend);
    input.expected_map_uuid = -1;
    assert(next(input) == MapStorageNextStep::ReopenChestNoResend);
    chest.source_map_uuid += 1;
    unsafe(input); // two durable UUIDs disagree, even without inventory
    chest.source_map_uuid = kMapUuid;
    rename.world_id = "another-world";
    unsafe(input);
    rename.world_id = input.world_id;
    input.expected_map_uuid = kMapUuid;
    chest.phase = MapChestTransferPhase::InventoryConfirmed;
    input.expected_map_uuid = -1;
    assert(next(input) == MapStorageNextStep::CommitTileCursor);
    chest.phase = MapChestTransferPhase::AcceptedAndClosed;
    assert(next(input) == MapStorageNextStep::CommitTileCursor);
    chest.phase = MapChestTransferPhase::ReopenConfirmed;
    assert(next(input) == MapStorageNextStep::CommitTileCursor);
    input.expected_map_uuid = kMapUuid;
    MapStorageCoordinatorDecision decision;
    std::string error;
    assert(ClassifyMapStorageCoordinator(input, &decision, &error));
    assert(decision.next == MapStorageNextStep::CommitTileCursor);
    assert(decision.expected_title == "地图 1行1列");
    assert(input.checkpoint_tile_cursor == 0); // classification cannot commit
    input.checkpoint_tile_cursor = 1;
    input.expected_map_uuid = -1; // map is already in the chest, not held
    assert(next(input) == MapStorageNextStep::ClearCommittedRenameJournal);
    chest.phase = MapChestTransferPhase::InventoryConfirmed;
    assert(next(input) == MapStorageNextStep::ClearCommittedRenameJournal);
    chest.phase = MapChestTransferPhase::AcceptedAndClosed;
    assert(next(input) == MapStorageNextStep::ClearCommittedRenameJournal);
    rename.map_uuid += 1;
    unsafe(input); // persisted rename and confirmed chest must agree
    rename.map_uuid = kMapUuid;
    input.rename = nullptr;
    assert(next(input) == MapStorageNextStep::ClearCommittedChestJournal);
    input.chest = nullptr;
    assert(next(input) == MapStorageNextStep::FinalizeCommittedMapUse);
    input.has_pending_map_use_marker = false;
    assert(next(input) == MapStorageNextStep::AwaitMapUseMarker);
    input.has_pending_map_use_marker = true;
    input.pending_map_use_cursor = 1;
    input.expected_map_uuid = kMapUuid;
    assert(next(input) == MapStorageNextStep::AwaitRenameJournal);

    input.expected_map_uuid = -1;
    unsafe(input); // uncommitted next tile cannot borrow a journal UUID

    input = base();
    pair = pairFor(input, MapPairPlacementPhase::PairConfirmed);
    input.pair = &pair;
    rename = renameFor(input, MapAnvilRenamePhase::RenamedMapConfirmed);
    input.rename = &rename;
    rename.expected_title += ' ';
    unsafe(input); // even a trailing space is not the exact tile title
    rename.expected_title.pop_back();
    rename.map_uuid += 1;
    unsafe(input);
    rename.map_uuid = kMapUuid;
    rename.world_id = "another-world";
    unsafe(input);
    rename.world_id = input.world_id;
    chest = chestFor(input, MapChestTransferPhase::DispatchArmed);
    input.chest = &chest;
    chest.source_map_uuid += 1;
    unsafe(input);
    chest.source_map_uuid = kMapUuid;
    input.checkpoint_tile_cursor = 1;
    unsafe(input); // armed chest cannot justify a cursor advance

    input = base(27, 28, 1);
    pair = pairFor(input, MapPairPlacementPhase::PairConfirmed);
    input.pair = &pair;
    assert(next(input) == MapStorageNextStep::AwaitExtraChestJournal);
    MapExtraChestPlacementRecord extra;
    extra.world_id = input.world_id;
    extra.dimension_id = input.dimension_id;
    extra.tile_count = input.tile_count;
    extra.artwork_bounds = input.artwork_bounds;
    extra.chest_index = 1;
    extra.chest = {33, 21, 0};
    extra.phase = MapExtraChestPlacementPhase::SupportSelected;
    extra.support_created = true;
    input.extra_chests.push_back(extra);
    assert(next(input) == MapStorageNextStep::SurveyExtraSupport);
    input.extra_chests[0].phase = MapExtraChestPlacementPhase::SupportDispatchArmed;
    assert(next(input) == MapStorageNextStep::ReconcileExtraSupportNoResend);
    input.extra_chests[0].phase = MapExtraChestPlacementPhase::SupportConfirmed;
    assert(next(input) == MapStorageNextStep::SurveyExtraChest);
    // Existing pre-support sidecars still resume without a support resend.
    extra.phase = MapExtraChestPlacementPhase::Selected;
    extra.support_created = false;
    input.extra_chests[0] = extra;
    assert(next(input) == MapStorageNextStep::SurveyExtraChest);
    input.extra_chests[0].phase = MapExtraChestPlacementPhase::DispatchArmed;
    assert(next(input) == MapStorageNextStep::ReconcileExtraChestNoResend);
    input.extra_chests[0].phase = MapExtraChestPlacementPhase::Confirmed;
    assert(next(input) == MapStorageNextStep::AwaitRenameJournal);
    rename = renameFor(input, MapAnvilRenamePhase::RenamedMapConfirmed);
    input.rename = &rename;
    chest = chestFor(input, MapChestTransferPhase::ReopenConfirmed,
                     input.extra_chests[0].chest);
    input.chest = &chest;
    assert(next(input) == MapStorageNextStep::CommitTileCursor);
    chest.phase = MapChestTransferPhase::InventoryConfirmed;
    input.expected_map_uuid = -1;
    assert(next(input) == MapStorageNextStep::CommitTileCursor);
    chest.phase = MapChestTransferPhase::AcceptedAndClosed;
    assert(next(input) == MapStorageNextStep::CommitTileCursor);
    input.expected_map_uuid = kMapUuid;
    chest.chest_slot = 1;
    unsafe(input);

    input = base(1);
    pair = pairFor(input, MapPairPlacementPhase::PairConfirmed);
    input.pair = &pair;
    input.checkpoint_tile_cursor = 2;
    input.expected_map_uuid = -1;
    assert(next(input) == MapStorageNextStep::FinalizeCommittedMapUse);
    input.has_pending_map_use_marker = false;
    assert(next(input) == MapStorageNextStep::ClearPairJournal);
    input.pair = nullptr;
    assert(next(input) == MapStorageNextStep::Complete);
}
