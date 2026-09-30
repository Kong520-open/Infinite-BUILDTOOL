#include "MapStorageCoordinator.h"

#include "MapTileNaming.h"

#include <limits>

namespace build_import {
namespace {

bool refuse(MapStorageCoordinatorDecision* decision, std::string* error,
            const char* reason) {
    if (decision) *decision = {};
    if (error) *error = reason;
    return false;
}

bool finish(MapStorageCoordinatorDecision* decision,
            MapStorageNextStep step, const MapTileChestAddress& address,
            const std::string& title, std::string* error) {
    decision->next = step;
    decision->chest_address = address;
    decision->expected_title = title;
    if (error) error->clear();
    return true;
}

bool matchesRename(const MapAnvilRenameRecord& rename,
                   const MapStorageCoordinatorInput& input,
                   uint64_t tile, int64_t map_uuid,
                   const std::string& title) {
    return rename.tile_cursor == tile &&
        rename.tile_count == input.tile_count &&
        rename.columns == input.columns && rename.rows == input.rows &&
        rename.world_id == input.world_id &&
        rename.dimension_id == input.dimension_id &&
        rename.map_uuid == map_uuid &&
        input.pair &&
        rename.anvil_x == input.pair->pair.anvil.x &&
        rename.anvil_y == input.pair->pair.anvil.y &&
        rename.anvil_z == input.pair->pair.anvil.z &&
        rename.expected_title == title;
}

bool matchesChest(const MapChestTransferRecord& chest,
                  const MapStorageCoordinatorInput& input,
                  uint64_t tile, int64_t map_uuid,
                  const MapTileChestAddress& address,
                  const MapChestPosition& location) {
    return chest.tile_cursor == tile &&
        chest.tile_count == input.tile_count &&
        chest.world_id == input.world_id &&
        chest.dimension_id == input.dimension_id &&
        chest.source_map_uuid == map_uuid &&
        chest.chest_index == address.chest_index &&
        chest.chest_slot == address.slot &&
        chest.chest_x == location.x && chest.chest_y == location.y &&
        chest.chest_z == location.z;
}

bool matchesFreshAnvilInput(const MapAnvilRenameRecord& rename,
                            const MapAnvilInputProof* proof) {
    return proof && proof->native_readback &&
        proof->fresh_window_token != 0U &&
        proof->window_id != 0U && proof->window_id != 0xFFU &&
        proof->world_id == rename.world_id &&
        proof->dimension_id == rename.dimension_id &&
        proof->anvil_x == rename.anvil_x &&
        proof->anvil_y == rename.anvil_y &&
        proof->anvil_z == rename.anvil_z &&
        proof->input_slot == 1U && proof->count == 1U &&
        proof->runtime_item_id == rename.map_runtime_item_id &&
        proof->network_stack_id > 0 &&
        proof->map_uuid == rename.map_uuid;
}

}  // namespace

bool ClassifyMapStorageCoordinator(
        const MapStorageCoordinatorInput& input,
        MapStorageCoordinatorDecision* decision, std::string* error) {
    if (!decision) {
        if (error) *error = "map storage decision output is unavailable";
        return false;
    }
    *decision = {};
    if (input.world_id.empty() || input.dimension_id < 0 ||
        input.dimension_id > 255 || input.columns == 0U || input.rows == 0U ||
        !input.artwork_bounds.isValid() ||
        input.tile_count == 0U || input.tile_count > kMaximumMapTileCount ||
        static_cast<uint64_t>(input.columns) * input.rows != input.tile_count ||
        input.checkpoint_tile_cursor > input.tile_count) {
        return refuse(decision, error, "map storage world or tile plan is invalid");
    }

    // Cleanup of a previously committed tile has priority over starting the
    // next tile. The chest journal remains until after the rename journal is
    // removed, because the latter's clear gate requires that chest proof.
    const bool committed_chest = input.chest &&
        input.chest->tile_cursor < input.tile_count &&
        input.checkpoint_tile_cursor == input.chest->tile_cursor + 1U;
    const bool committed_marker = input.has_pending_map_use_marker &&
        input.checkpoint_tile_cursor > 0U &&
        input.pending_map_use_cursor + 1U == input.checkpoint_tile_cursor;
    if (input.has_pending_map_use_marker &&
        (input.pending_map_use_cursor >= input.tile_count ||
         input.pending_map_use_cursor > input.checkpoint_tile_cursor ||
         input.checkpoint_tile_cursor - input.pending_map_use_cursor > 1U ||
         (committed_chest && !committed_marker) ||
         (committed_marker && input.chest && !committed_chest))) {
        return refuse(decision, error,
                      "map-use marker does not match the durable tile cursor");
    }
    if (!input.has_pending_map_use_marker && (input.rename || input.chest)) {
        return refuse(decision, error,
                      "map transaction exists without its map-use marker");
    }
    const uint64_t tile = committed_chest
        ? input.chest->tile_cursor
        : committed_marker ? input.pending_map_use_cursor
                           : input.checkpoint_tile_cursor;

    if (!input.has_pending_map_use_marker && tile < input.tile_count) {
        if (input.pair && ClassifyMapPairPlacementResume(
                *input.pair, input.world_id, input.dimension_id,
                input.tile_count, input.artwork_bounds) !=
            MapPairPlacementResume::VerifyPairBeforeStorage) {
            return refuse(decision, error,
                          "map pair is incomplete without an active map-use marker");
        }
        for (size_t i = 0; i < input.extra_chests.size(); ++i) {
            if (i >= std::numeric_limits<uint16_t>::max() ||
                ClassifyMapExtraChestPlacementResume(
                    input.extra_chests[i], input.world_id, input.dimension_id,
                    input.tile_count, input.artwork_bounds,
                    static_cast<uint16_t>(i + 1U)) !=
                    MapExtraChestPlacementResume::VerifyConfirmedChest) {
                return refuse(decision, error,
                              "extra chest is incomplete without an active map-use marker");
            }
        }
        return finish(decision, MapStorageNextStep::AwaitMapUseMarker,
                      {}, {}, error);
    }

    if (tile == input.tile_count) {
        if (input.rename || input.chest) {
            return refuse(decision, error,
                          "a completed map plan still has an unpaired tile journal");
        }
        if (!input.pair) {
            if (!input.extra_chests.empty()) {
                return refuse(decision, error,
                              "extra chest journals outlived their pair plan");
            }
            return finish(decision, MapStorageNextStep::Complete, {}, {}, error);
        }
        if (ClassifyMapPairPlacementResume(
                *input.pair, input.world_id, input.dimension_id,
                input.tile_count, input.artwork_bounds) !=
            MapPairPlacementResume::VerifyPairBeforeStorage) {
            return refuse(decision, error,
                          "completed map plan has an unconfirmed pair journal");
        }
        for (size_t i = 0; i < input.extra_chests.size(); ++i) {
            if (i >= std::numeric_limits<uint16_t>::max() ||
                ClassifyMapExtraChestPlacementResume(
                    input.extra_chests[i], input.world_id, input.dimension_id,
                    input.tile_count, input.artwork_bounds,
                    static_cast<uint16_t>(i + 1U)) !=
                    MapExtraChestPlacementResume::VerifyConfirmedChest) {
                return refuse(decision, error,
                              "completed map plan has an unconfirmed extra chest");
            }
        }
        return finish(decision, input.extra_chests.empty()
            ? MapStorageNextStep::ClearPairJournal
            : MapStorageNextStep::ClearExtraChestJournalsDescending,
            {}, {}, error);
    }

    MapTileChestAddress address;
    std::string title;
    if (!ResolveMapTileChestAddress(tile, input.tile_count, &address) ||
        !FormatMapTileName(tile, input.columns, input.rows, &title)) {
        return refuse(decision, error, "map storage tile address or title is invalid");
    }
    if (committed_marker && !input.rename && !input.chest) {
        if (!input.pair ||
            ClassifyMapPairPlacementResume(
                *input.pair, input.world_id, input.dimension_id,
                input.tile_count, input.artwork_bounds) !=
                MapPairPlacementResume::VerifyPairBeforeStorage ||
            input.extra_chests.size() != address.chest_index) {
            return refuse(decision, error,
                          "committed map marker lacks its confirmed chest plan");
        }
        for (size_t i = 0; i < input.extra_chests.size(); ++i) {
            if (ClassifyMapExtraChestPlacementResume(
                    input.extra_chests[i], input.world_id, input.dimension_id,
                    input.tile_count, input.artwork_bounds,
                    static_cast<uint16_t>(i + 1U)) !=
                MapExtraChestPlacementResume::VerifyConfirmedChest) {
                return refuse(decision, error,
                              "committed map marker has an unconfirmed extra chest");
            }
        }
        return finish(decision, MapStorageNextStep::FinalizeCommittedMapUse,
                      address, title, error);
    }
    // Once a request is Armed, the map may have left the player inventory.
    // A durable journal UUID may identify ONLY a no-resend reconciliation or
    // a previously verified chest commit. It must never authorize a fresh
    // input/craft/chest Place or a new placement command.
    int64_t map_uuid = input.expected_map_uuid;
    if (map_uuid == -1 && committed_chest &&
        (input.chest->phase == MapChestTransferPhase::ReopenConfirmed ||
         input.chest->phase == MapChestTransferPhase::InventoryConfirmed ||
         input.chest->phase == MapChestTransferPhase::AcceptedAndClosed)) {
        map_uuid = input.chest->source_map_uuid;
    }
    if (map_uuid == -1 && !committed_chest && input.rename &&
        !input.chest &&
        (input.rename->phase == MapAnvilRenamePhase::InputDispatchArmed ||
         input.rename->phase == MapAnvilRenamePhase::InputResponseAccepted ||
         input.rename->phase == MapAnvilRenamePhase::CraftClickArmed ||
         input.rename->phase == MapAnvilRenamePhase::CraftDispatchArmed ||
         input.rename->phase == MapAnvilRenamePhase::CraftResponseAccepted)) {
        map_uuid = input.rename->map_uuid;
    }
    if (!committed_chest && input.rename && !input.chest &&
        input.rename->phase == MapAnvilRenamePhase::InputConfirmed) {
        if (!matchesFreshAnvilInput(*input.rename,
                                    input.fresh_anvil_input)) {
            return refuse(decision, error,
                "fresh native anvil input proof is required before craft");
        }
        if (map_uuid == -1) map_uuid = input.fresh_anvil_input->map_uuid;
    }
    if (map_uuid == -1 && !committed_chest && input.rename && input.chest &&
        input.rename->phase == MapAnvilRenamePhase::RenamedMapConfirmed &&
        (input.chest->phase == MapChestTransferPhase::DispatchArmed ||
         input.chest->phase == MapChestTransferPhase::ResponseAccepted ||
         input.chest->phase == MapChestTransferPhase::ReopenConfirmed ||
         input.chest->phase == MapChestTransferPhase::InventoryConfirmed ||
         input.chest->phase == MapChestTransferPhase::AcceptedAndClosed) &&
        input.rename->map_uuid == input.chest->source_map_uuid) {
        map_uuid = input.chest->source_map_uuid;
    }
    if (map_uuid == -1) {
        return refuse(decision, error, "current map UUID is unavailable");
    }
    if (!input.pair) {
        if (input.rename || input.chest || !input.extra_chests.empty()) {
            return refuse(decision, error,
                          "map transaction exists without a pair placement journal");
        }
        return finish(decision, MapStorageNextStep::AwaitPairJournal,
                      address, title, error);
    }
    const MapPairPlacementResume pair = ClassifyMapPairPlacementResume(
        *input.pair, input.world_id, input.dimension_id,
        input.tile_count, input.artwork_bounds);
    if (pair == MapPairPlacementResume::Unsafe) {
        return refuse(decision, error, "map pair belongs to another world or plan");
    }
    if (pair != MapPairPlacementResume::VerifyPairBeforeStorage) {
        if (input.rename || input.chest || !input.extra_chests.empty()) {
            return refuse(decision, error,
                          "storage advanced before chest and anvil placement confirmed");
        }
        MapStorageNextStep next = MapStorageNextStep::Unsafe;
        switch (pair) {
            case MapPairPlacementResume::SurveySelectedSupport:
                next = MapStorageNextStep::SurveyPairSupport; break;
            case MapPairPlacementResume::AmbiguousSupportDispatch:
                next = MapStorageNextStep::ReconcilePairSupportNoResend; break;
            case MapPairPlacementResume::VerifySupportBeforeChest:
                next = MapStorageNextStep::SurveyPairChest; break;
            case MapPairPlacementResume::SurveySelectedPair:
                next = MapStorageNextStep::SurveyPairChest; break;
            case MapPairPlacementResume::AmbiguousChestDispatch:
                next = MapStorageNextStep::ReconcilePairChestNoResend; break;
            case MapPairPlacementResume::VerifyChestBeforeAnvil:
                next = MapStorageNextStep::SurveyPairAnvil; break;
            case MapPairPlacementResume::AmbiguousAnvilDispatch:
                next = MapStorageNextStep::ReconcilePairAnvilNoResend; break;
            default: break;
        }
        return next != MapStorageNextStep::Unsafe &&
            finish(decision, next, address, title, error);
    }

    if (input.extra_chests.size() > address.chest_index) {
        return refuse(decision, error,
                      "an extra chest plan exists ahead of the current tile");
    }
    for (size_t i = 0; i < input.extra_chests.size(); ++i) {
        const MapExtraChestPlacementResume extra =
            ClassifyMapExtraChestPlacementResume(
                input.extra_chests[i], input.world_id, input.dimension_id,
                input.tile_count, input.artwork_bounds,
                static_cast<uint16_t>(i + 1U));
        if (extra == MapExtraChestPlacementResume::Unsafe ||
            (i + 1U < address.chest_index &&
             extra != MapExtraChestPlacementResume::VerifyConfirmedChest)) {
            return refuse(decision, error,
                          "extra chest prefix is incomplete or belongs to another plan");
        }
    }
    if (address.chest_index > input.extra_chests.size()) {
        if (input.rename || input.chest) {
            return refuse(decision, error,
                          "map transaction has no target extra chest journal");
        }
        return finish(decision, MapStorageNextStep::AwaitExtraChestJournal,
                      address, title, error);
    }
    MapChestPosition chest_position = input.pair->pair.chest;
    if (address.chest_index != 0U) {
        const MapExtraChestPlacementRecord& current =
            input.extra_chests[address.chest_index - 1U];
        const MapExtraChestPlacementResume extra =
            ClassifyMapExtraChestPlacementResume(
                current, input.world_id, input.dimension_id,
                input.tile_count, input.artwork_bounds, address.chest_index);
        if (extra != MapExtraChestPlacementResume::VerifyConfirmedChest) {
            if (input.rename || input.chest) {
                return refuse(decision, error,
                              "map transaction advanced before its extra chest confirmed");
            }
            MapStorageNextStep next = MapStorageNextStep::Unsafe;
            switch (extra) {
                case MapExtraChestPlacementResume::SurveySelectedSupport:
                    next = MapStorageNextStep::SurveyExtraSupport; break;
                case MapExtraChestPlacementResume::AmbiguousSupportDispatch:
                    next = MapStorageNextStep::ReconcileExtraSupportNoResend; break;
                case MapExtraChestPlacementResume::VerifySupportBeforeChest:
                case MapExtraChestPlacementResume::SurveySelectedSite:
                    next = MapStorageNextStep::SurveyExtraChest; break;
                case MapExtraChestPlacementResume::AmbiguousDispatchNoResend:
                    next = MapStorageNextStep::ReconcileExtraChestNoResend; break;
                default: break;
            }
            return next != MapStorageNextStep::Unsafe &&
                finish(decision, next, address, title, error);
        }
        chest_position = current.chest;
    }

    if (input.rename && !matchesRename(*input.rename, input, tile,
                                       map_uuid, title)) {
        return refuse(decision, error,
                      "map rename UUID or exact row-column title differs");
    }
    if (input.chest &&
        (!matchesChest(*input.chest, input, tile, map_uuid,
                       address, chest_position) ||
         (input.rename &&
          input.chest->source_runtime_item_id !=
              input.rename->map_runtime_item_id))) {
        return refuse(decision, error,
                      "map chest UUID, item or target slot differs");
    }

    if (committed_chest) {
        if (ClassifyMapChestTransferRecovery(
                *input.chest, input.world_id, input.dimension_id,
                input.checkpoint_tile_cursor, input.tile_count,
                chest_position.x, chest_position.y, chest_position.z,
                address.slot) !=
            MapChestRecoveryAction::ClearJournalAfterCursorCommit) {
            return refuse(decision, error,
                          "committed map lacks confirmed chest transfer proof");
        }
        if (input.rename) {
            if (ClassifyMapAnvilRenameRecovery(
                    *input.rename, input.world_id, input.dimension_id,
                    input.checkpoint_tile_cursor, input.tile_count) !=
                MapAnvilRenameRecovery::ClearAfterStorageCommit) {
                return refuse(decision, error,
                              "committed map lacks confirmed rename proof");
            }
            return finish(decision,
                MapStorageNextStep::ClearCommittedRenameJournal,
                address, title, error);
        }
        return finish(decision,
            MapStorageNextStep::ClearCommittedChestJournal,
            address, title, error);
    }
    if (input.rename && input.rename->tile_cursor + 1U ==
            input.checkpoint_tile_cursor && !input.chest) {
        return refuse(decision, error,
                      "rename journal outlived its required chest proof");
    }
    if (!input.rename) {
        if (input.chest) {
            return refuse(decision, error,
                          "chest transfer exists before exact map rename");
        }
        return finish(decision, MapStorageNextStep::AwaitRenameJournal,
                      address, title, error);
    }
    const MapAnvilRenameRecovery rename = ClassifyMapAnvilRenameRecovery(
        *input.rename, input.world_id, input.dimension_id,
        input.checkpoint_tile_cursor, input.tile_count);
    switch (rename) {
        case MapAnvilRenameRecovery::FreshInputPreflight:
            return finish(decision, MapStorageNextStep::PreflightAnvilInput,
                          address, title, error);
        case MapAnvilRenameRecovery::ReconcileInputNoResend:
            return finish(decision,
                          MapStorageNextStep::ReconcileAnvilInputNoResend,
                          address, title, error);
        case MapAnvilRenameRecovery::FreshCraftPreflight:
            return finish(decision, MapStorageNextStep::PreflightAnvilCraft,
                          address, title, error);
        case MapAnvilRenameRecovery::ReconcileCraftClickNoResend:
            return finish(decision,
                          MapStorageNextStep::ReconcileAnvilCraftNoResend,
                          address, title, error);
        case MapAnvilRenameRecovery::ReconcileCraftNoResend:
            return finish(decision,
                          MapStorageNextStep::ReconcileAnvilCraftNoResend,
                          address, title, error);
        case MapAnvilRenameRecovery::ProceedToChestStorage:
            break;
        default:
            return refuse(decision, error,
                          "map rename state cannot continue from this cursor");
    }
    if (!input.chest) {
        return finish(decision, MapStorageNextStep::AwaitChestJournal,
                      address, title, error);
    }
    const MapChestRecoveryAction chest = ClassifyMapChestTransferRecovery(
        *input.chest, input.world_id, input.dimension_id,
        input.checkpoint_tile_cursor, input.tile_count,
        chest_position.x, chest_position.y, chest_position.z,
        address.slot);
    switch (chest) {
        case MapChestRecoveryAction::FreshPreflightRequired:
            return finish(decision, MapStorageNextStep::PreflightChestTransfer,
                          address, title, error);
        case MapChestRecoveryAction::ReopenChestNoResend:
            return finish(decision, MapStorageNextStep::ReopenChestNoResend,
                          address, title, error);
        case MapChestRecoveryAction::CommitTileCursorNoResend:
            // The durable chest journal carries either legacy fresh-reopen
            // proof, an inventory absence proof, or an accepted Place with
            // the original chest window positively closed.
            return finish(decision, MapStorageNextStep::CommitTileCursor,
                          address, title, error);
        default:
            return refuse(decision, error,
                          "map chest state cannot continue from this cursor");
    }
}

}  // namespace build_import
