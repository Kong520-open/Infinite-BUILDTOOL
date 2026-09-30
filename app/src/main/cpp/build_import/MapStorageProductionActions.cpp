#include "MapStorageProductionActions.h"

#include "MapChestNativeSurvey.h"
#include "MapChestStorageVerification.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace build_import {
namespace {

MapStorageExternalResult wait(std::string* error, const char* reason) {
    if (error) *error = reason;
    return MapStorageExternalResult::NoChange;
}

MapStorageExternalResult uncertain(std::string* error, const char* reason) {
    if (error && error->empty()) *error = reason;
    return MapStorageExternalResult::DispatchOutcomeUnknown;
}

bool live(MapStorageProductionActions* actions,
          const MapStorageCoordinatorInput& snapshot,
          std::string* error) {
    if (!actions || actions->map_state_path.empty() ||
        !actions->hooks.require_live_world) {
        if (error) *error = "map storage production context is incomplete";
        return false;
    }
    return actions->hooks.require_live_world(
        actions->hooks.context, snapshot.world_id, snapshot.dimension_id,
        error);
}

bool openReader(MapStorageProductionActions* actions, std::string* error) {
    if (!actions->reader || !actions->reader->open()) {
        if (error) *error = "fresh native world reader is unavailable";
        return false;
    }
    return true;
}

bool safeChestSurvey(const MapChestCandidateSurvey& survey) {
    if (survey.target != MapChestCell::Air ||
        survey.above != MapChestCell::Air ||
        survey.below != MapChestCell::SolidSupport) return false;
    const MapChestCell neighbors[] = {
        survey.north, survey.south, survey.west, survey.east};
    for (const MapChestCell cell : neighbors) {
        if (cell == MapChestCell::Unknown || cell == MapChestCell::Chest) {
            return false;
        }
    }
    return true;
}

bool safeAirFloorSurvey(const MapChestCandidateSurvey& survey) {
    if (survey.target != MapChestCell::Air ||
        survey.above != MapChestCell::Air ||
        survey.below != MapChestCell::Air) return false;
    const MapChestCell neighbors[] = {
        survey.north, survey.south, survey.west, survey.east};
    for (const MapChestCell cell : neighbors) {
        if (cell == MapChestCell::Unknown || cell == MapChestCell::Chest) {
            return false;
        }
    }
    return true;
}

bool readChest(MapStorageProductionActions* actions,
               const MapChestPosition& position,
               MapExtraChestReadback* result) {
    if (!result || !actions->reader) return false;
    result->position = position;
    result->available = actions->reader->getBlock(
        position.x, position.y, position.z, &result->block);
    return result->available;
}

bool exactStoneBelow(MapStorageProductionActions* actions,
                     const MapChestPosition& position) {
    if (!actions || !actions->reader) return false;
    NativeBlockInfo below;
    return actions->reader->getBlock(
        position.x, position.y - 1, position.z, &below) &&
        below.name == "minecraft:stone";
}

bool priorChestReadbacks(MapStorageProductionActions* actions,
                         const MapStorageCoordinatorInput& snapshot,
                         size_t prior_extra_count,
                         std::vector<MapExtraChestReadback>* result) {
    if (!result || !snapshot.pair ||
        prior_extra_count > snapshot.extra_chests.size()) return false;
    result->clear();
    result->reserve(prior_extra_count + 1U);
    MapExtraChestReadback pair_chest;
    if (!readChest(actions, snapshot.pair->pair.chest, &pair_chest)) return false;
    result->push_back(std::move(pair_chest));
    for (size_t i = 0; i < prior_extra_count; ++i) {
        MapExtraChestReadback extra;
        if (!readChest(actions, snapshot.extra_chests[i].chest, &extra)) {
            return false;
        }
        result->push_back(std::move(extra));
    }
    return true;
}

MapChestPosition selectedChest(const MapStorageCoordinatorInput& snapshot,
                               const MapStorageCoordinatorDecision& decision) {
    if (decision.chest_address.chest_index == 0U) {
        return snapshot.pair->pair.chest;
    }
    return snapshot.extra_chests[decision.chest_address.chest_index - 1U].chest;
}

bool matchingPersistedExtra(MapStorageProductionActions* actions,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision,
        std::string* error) {
    const uint16_t index = decision.chest_address.chest_index;
    if (index == 0U) return true;
    if (index > snapshot.extra_chests.size()) {
        if (error) *error = "target extra chest journal is unavailable";
        return false;
    }
    const auto& expected = snapshot.extra_chests[index - 1U];
    MapExtraChestPlacementRecord persisted;
    if (LoadMapExtraChestPlacementJournal(actions->map_state_path, index,
                                          &persisted, error) !=
            MapExtraChestPlacementLoad::Loaded ||
        persisted.phase != MapExtraChestPlacementPhase::Confirmed ||
        expected.phase != persisted.phase ||
        persisted.world_id != snapshot.world_id ||
        persisted.dimension_id != snapshot.dimension_id ||
        persisted.tile_count != snapshot.tile_count ||
        persisted.chest_index != index ||
        persisted.artwork_bounds.min_x != snapshot.artwork_bounds.min_x ||
        persisted.artwork_bounds.min_y != snapshot.artwork_bounds.min_y ||
        persisted.artwork_bounds.min_z != snapshot.artwork_bounds.min_z ||
        persisted.artwork_bounds.max_x != snapshot.artwork_bounds.max_x ||
        persisted.artwork_bounds.max_y != snapshot.artwork_bounds.max_y ||
        persisted.artwork_bounds.max_z != snapshot.artwork_bounds.max_z ||
        !(persisted.chest == expected.chest) ||
        persisted.support_created != expected.support_created ||
        persisted.platform_corner != expected.platform_corner ||
        persisted.command_uuid != expected.command_uuid ||
        !openReader(actions, error)) {
        if (error && error->empty()) {
            *error = "durable target extra chest differs from the live plan";
        }
        return false;
    }
    NativeBlockInfo chest;
    if (!actions->reader->getBlock(persisted.chest.x, persisted.chest.y,
                                    persisted.chest.z, &chest) ||
        chest.name != "minecraft:chest" ||
        (persisted.support_created &&
         !exactStoneBelow(actions, persisted.chest))) {
        if (error) *error = "target extra chest or its support changed";
        return false;
    }
    if (persisted.platform_corner != 0U) {
        std::array<MapChestPosition, 4> cells;
        if (!BuildMapExtraSupportPlatformCells(
                persisted.artwork_bounds, persisted.chest,
                persisted.platform_corner, &cells) ||
            !ExactMapSupportPlatformStoneWithReader(actions->reader, cells)) {
            if (error) *error = "extra chest standing platform changed";
            return false;
        }
    }
    return true;
}

bool freshHeldMap(MapStorageProductionActions* actions,
                  const MapStorageCoordinatorInput& snapshot,
                  const MapStorageCoordinatorDecision& decision,
                  MapStorageHeldMapEvidence* held, std::string* error) {
    if (!actions->hooks.read_held_map || !snapshot.rename || !held ||
        !actions->hooks.read_held_map(actions->hooks.context, held, error)) {
        if (error && error->empty()) *error = "renamed map native readback is unavailable";
        return false;
    }
    const auto& source = held->source;
    if (!held->native_readback ||
        held->name_status != MapItemNameStatus::Present ||
        held->name_source != MapItemNameSource::DisplayName ||
        held->name != decision.expected_title ||
        source.selected_hotbar_slot < 0 || source.selected_hotbar_slot > 8 ||
        source.count != 1U || source.network_stack_id <= 0 ||
        source.runtime_item_id != snapshot.rename->map_runtime_item_id ||
        !source.has_map_uuid || source.map_uuid != snapshot.expected_map_uuid ||
        (source.item_identifier != "minecraft:map" &&
         source.item_identifier != "map" &&
         source.item_identifier != "minecraft:filled_map" &&
         source.item_identifier != "filled_map" &&
         source.item_identifier != "minecraft:locator_map" &&
         source.item_identifier != "locator_map")) {
        if (error) *error = "held map lacks exact native UUID and renamed title";
        return false;
    }
    return true;
}

bool matchingPersistedPair(MapStorageProductionActions* actions,
        const MapStorageCoordinatorInput& snapshot, std::string* error) {
    if (!snapshot.pair) return false;
    MapPairPlacementRecord persisted;
    const auto loaded = LoadMapPairPlacementJournal(actions->map_state_path,
                                                    &persisted, error);
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
    if (loaded == MapPairPlacementLoad::Missing &&
        actions->debug_preexisting_pair_verified &&
        snapshot.pair->phase == MapPairPlacementPhase::PairConfirmed &&
        snapshot.pair->pair == actions->debug_preexisting_pair &&
        snapshot.pair->world_id == snapshot.world_id &&
        snapshot.pair->dimension_id == snapshot.dimension_id &&
        snapshot.pair->tile_count == snapshot.tile_count) {
        if (error) error->clear();
        return true;
    }
#endif
    if (loaded != MapPairPlacementLoad::Loaded ||
        persisted.phase != MapPairPlacementPhase::PairConfirmed ||
        persisted.world_id != snapshot.world_id ||
        persisted.dimension_id != snapshot.dimension_id ||
        persisted.tile_count != snapshot.tile_count ||
        persisted.artwork_bounds.min_x != snapshot.artwork_bounds.min_x ||
        persisted.artwork_bounds.min_y != snapshot.artwork_bounds.min_y ||
        persisted.artwork_bounds.min_z != snapshot.artwork_bounds.min_z ||
        persisted.artwork_bounds.max_x != snapshot.artwork_bounds.max_x ||
        persisted.artwork_bounds.max_y != snapshot.artwork_bounds.max_y ||
        persisted.artwork_bounds.max_z != snapshot.artwork_bounds.max_z ||
        !(persisted.pair == snapshot.pair->pair) ||
        persisted.support_created != snapshot.pair->support_created ||
        persisted.platform_side != snapshot.pair->platform_side) {
        if (error) *error = "durable chest/anvil pair changed before storage";
        return false;
    }
    if (persisted.platform_side != 0U) {
        std::array<MapChestPosition, 4> cells;
        if (!BuildMapPairSupportPlatformCells(
                persisted.artwork_bounds, persisted.pair,
                persisted.platform_side, &cells) ||
            !openReader(actions, error) ||
            !ExactMapSupportPlatformStoneWithReader(actions->reader, cells)) {
            if (error) *error = "chest/anvil standing platform changed";
            return false;
        }
    }
    if (persisted.support_created &&
        (!openReader(actions, error) ||
         !exactStoneBelow(actions, persisted.pair.chest) ||
         !exactStoneBelow(actions, persisted.pair.anvil))) {
        if (error) *error = "auto-placed pair support stone changed before storage";
        return false;
    }
    return true;
}

bool matchingPersistedRename(MapStorageProductionActions* actions,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision,
        std::string* error) {
    if (!snapshot.rename) return false;
    MapAnvilRenameRecord persisted;
    if (LoadMapAnvilRenameJournal(actions->map_state_path,
                                  &persisted, error) !=
            MapAnvilRenameLoad::Loaded ||
        persisted.phase != MapAnvilRenamePhase::RenamedMapConfirmed ||
        persisted.world_id != snapshot.world_id ||
        persisted.dimension_id != snapshot.dimension_id ||
        persisted.tile_cursor != snapshot.rename->tile_cursor ||
        persisted.tile_count != snapshot.tile_count ||
        persisted.map_uuid != snapshot.rename->map_uuid ||
        persisted.map_runtime_item_id != snapshot.rename->map_runtime_item_id ||
        !snapshot.pair ||
        persisted.anvil_x != snapshot.pair->pair.anvil.x ||
        persisted.anvil_y != snapshot.pair->pair.anvil.y ||
        persisted.anvil_z != snapshot.pair->pair.anvil.z ||
        persisted.expected_title != decision.expected_title) {
        if (error) *error = "durable renamed-map journal changed before storage";
        return false;
    }
    return true;
}

bool matchingPersistedChest(MapStorageProductionActions* actions,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision,
        std::string* error) {
    if (!snapshot.chest || !matchingPersistedRename(
            actions, snapshot, decision, error)) return false;
    MapChestTransferRecord persisted;
    if (LoadMapChestTransferJournal(actions->map_state_path,
                                    &persisted, error) !=
            MapChestJournalLoad::Loaded ||
        (persisted.phase != MapChestTransferPhase::ReopenConfirmed &&
         persisted.phase != MapChestTransferPhase::InventoryConfirmed &&
         persisted.phase != MapChestTransferPhase::AcceptedAndClosed) ||
        persisted.world_id != snapshot.world_id ||
        persisted.dimension_id != snapshot.dimension_id ||
        persisted.tile_cursor != snapshot.chest->tile_cursor ||
        persisted.tile_count != snapshot.tile_count ||
        persisted.chest_x != snapshot.chest->chest_x ||
        persisted.chest_y != snapshot.chest->chest_y ||
        persisted.chest_z != snapshot.chest->chest_z ||
        persisted.chest_index != decision.chest_address.chest_index ||
        persisted.chest_slot != decision.chest_address.slot ||
        persisted.source_runtime_item_id != snapshot.rename->map_runtime_item_id ||
        persisted.source_map_uuid != snapshot.rename->map_uuid ||
        persisted.request_id != snapshot.chest->request_id) {
        if (error) *error = "durable chest proof changed before cursor commit";
        return false;
    }
    return true;
}

bool captureChest(MapStorageProductionActions* actions,
                  const MapChestPosition& chest, bool reopen,
                  ContainerCaptureResult* capture, std::string* error) {
    if (!actions->hooks.capture_chest ||
        !actions->hooks.capture_chest(actions->hooks.context, chest,
                                      reopen, capture, error)) {
        if (error && error->empty()) {
            *error = reopen ? "visible chest close is not complete"
                            : "single-chest capture is unavailable";
        }
        return false;
    }
    return true;
}

MapStorageExternalResult beginPair(MapStorageProductionActions* actions,
        const MapStorageCoordinatorInput& snapshot, std::string* error) {
    if (!openReader(actions, error)) return MapStorageExternalResult::NoChange;
    const auto candidates = EnumerateMapChestAnvilCandidates(snapshot.artwork_bounds);
    MapChestAnvilPosition chosen;
    uint8_t platform_side = 0U;
    bool found = false;
    for (const auto& candidate : candidates) {
        const auto chest_survey = SurveyMapChestCandidateWithReader(
            actions->reader, candidate.chest);
        const auto anvil_survey = SurveyMapChestCandidateWithReader(
            actions->reader, candidate.anvil);
        if (safeAirFloorSurvey(chest_survey) &&
            safeAirFloorSurvey(anvil_survey) &&
            SelectMapPairSupportPlatformSideWithReader(
                snapshot.artwork_bounds, candidate, actions->reader,
                &platform_side)) {
            chosen = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        return wait(error, "no safe native-surveyed chest/anvil support site is available");
    }
    MapPairPlacementRecord selected;
    selected.world_id = snapshot.world_id;
    selected.dimension_id = snapshot.dimension_id;
    selected.tile_count = snapshot.tile_count;
    selected.artwork_bounds = snapshot.artwork_bounds;
    selected.pair = chosen;
    selected.phase = MapPairPlacementPhase::SupportSelected;
    selected.support_created = true;
    selected.platform_side = platform_side;
    if (!BeginMapPairPlacementJournal(actions->map_state_path,
                                      selected, error)) {
        return uncertain(error, "pair journal creation outcome is uncertain");
    }
    return MapStorageExternalResult::DurableTransition;
}

MapStorageExternalResult beginExtra(MapStorageProductionActions* actions,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision, std::string* error) {
    if (!matchingPersistedPair(actions, snapshot, error) ||
        !openReader(actions, error)) {
        return MapStorageExternalResult::NoChange;
    }
    const uint16_t index = decision.chest_address.chest_index;
    if (index == 0U || snapshot.extra_chests.size() != index - 1U) {
        return wait(error, "extra chest prefix does not match target index");
    }
    std::vector<MapExtraChestReadback> readbacks;
    if (!priorChestReadbacks(actions, snapshot, snapshot.extra_chests.size(),
                             &readbacks)) {
        return wait(error, "confirmed chest prefix is not readable");
    }
    for (const auto& position : EnumerateMapChestCandidates(
             snapshot.artwork_bounds)) {
        const auto survey = SurveyMapChestCandidateWithReader(
            actions->reader, position);
        if (!safeAirFloorSurvey(survey)) continue;
        uint8_t platform_corner = 0U;
        if (!SelectMapExtraSupportPlatformCornerWithReader(
                snapshot.artwork_bounds, position, actions->reader,
                &platform_corner)) continue;
        MapExtraChestPlacementRecord selected;
        selected.world_id = snapshot.world_id;
        selected.dimension_id = snapshot.dimension_id;
        selected.tile_count = snapshot.tile_count;
        selected.artwork_bounds = snapshot.artwork_bounds;
        selected.chest_index = index;
        selected.chest = position;
        selected.phase = MapExtraChestPlacementPhase::SupportSelected;
        selected.support_created = true;
        selected.platform_corner = platform_corner;
        std::string candidate_error;
        if (!ValidateMapExtraChestPlanChain(
                *snapshot.pair, snapshot.extra_chests, readbacks,
                selected, &candidate_error)) continue;
        if (!BeginMapExtraChestPlacementJournal(
                actions->map_state_path, selected, error)) {
            return uncertain(error, "extra chest journal creation outcome is uncertain");
        }
        return MapStorageExternalResult::DurableTransition;
    }
    return wait(error, "no live safe extra-chest site is available");
}

bool nextCommandUuid(MapStorageProductionActions* actions,
                     std::string* uuid, std::string* error) {
    return actions->hooks.new_command_uuid &&
        actions->hooks.new_command_uuid(actions->hooks.context, uuid, error) &&
        !uuid->empty();
}

bool approvePlacement(MapStorageProductionActions* actions,
                      const std::string& command, std::string* error) {
    return actions->hooks.approve_placement_command &&
        actions->hooks.approve_placement_command(
            actions->hooks.context, command, error);
}

MapStorageExternalResult sendPair(MapStorageProductionActions* actions,
        const MapStorageCoordinatorInput& snapshot, MapPairPlacementStep step,
        std::string* error) {
    if (!snapshot.pair || !openReader(actions, error)) {
        return MapStorageExternalResult::NoChange;
    }
    if (step == MapPairPlacementStep::Support &&
        snapshot.pair->platform_side == 0U) {
        uint8_t side = 0U;
        if (!SelectMapPairSupportPlatformSideWithReader(
                snapshot.artwork_bounds, snapshot.pair->pair,
                actions->reader, &side)) {
            return wait(error, "unarmed pair lacks a safe 2x2 standing platform");
        }
        if (!UpgradeMapPairSupportSelectedPlatform(
                actions->map_state_path, *snapshot.pair, side, error)) {
            return uncertain(error, "pair platform upgrade outcome is uncertain");
        }
        return MapStorageExternalResult::DurableTransition;
    }
    MapPairPlacementCommand command;
    const bool chest_accepted =
        snapshot.pair->phase == MapPairPlacementPhase::ChestConfirmed;
    if (PrepareMapPairPlacementCommandWithReader(
            snapshot.artwork_bounds, snapshot.pair->pair, step,
            chest_accepted, actions->reader, &command,
            snapshot.pair->support_created) !=
        MapPairPlacementPreparation::Ready) {
        return wait(error, "pair placement lacks safe fresh native preflight");
    }
    if (snapshot.pair->platform_side != 0U) {
        std::array<MapChestPosition, 4> cells;
        if (!BuildMapPairSupportPlatformCells(
                snapshot.artwork_bounds, snapshot.pair->pair,
                snapshot.pair->platform_side, &cells) ||
            !(step == MapPairPlacementStep::Support
                ? FreshMapSupportPlatformAirWithReader(actions->reader, cells)
                : ExactMapSupportPlatformStoneWithReader(actions->reader, cells))) {
            return wait(error, "pair standing platform lacks exact native preflight");
        }
        if (step == MapPairPlacementStep::Support) {
            command.text = BuildMapSupportPlatformFillCommand(cells);
        }
    }
    if (!approvePlacement(actions, command.text, error)) {
        return wait(error, "pair fill command syntax has not been approved");
    }
    std::string uuid;
    if (!nextCommandUuid(actions, &uuid, error)) {
        return wait(error, "tracked RPC command UUID is unavailable");
    }
    if (!ArmMapPairPlacementDispatch(actions->map_state_path,
                                     *snapshot.pair, step, uuid, error)) {
        return uncertain(error, "pair command arm outcome is uncertain");
    }
    if (!actions->hooks.dispatch_tracked_rpc ||
        !actions->hooks.dispatch_tracked_rpc(actions->hooks.context, uuid,
                                             command.text, error)) {
        return uncertain(error, "pair command dispatch outcome is uncertain");
    }
    return MapStorageExternalResult::DurableTransition;
}

MapStorageExternalResult sendExtra(MapStorageProductionActions* actions,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision, bool support,
        std::string* error) {
    const uint16_t index = decision.chest_address.chest_index;
    if (index == 0U || index > snapshot.extra_chests.size() ||
        !matchingPersistedPair(actions, snapshot, error) ||
        !openReader(actions, error)) {
        return MapStorageExternalResult::NoChange;
    }
    const auto& extra = snapshot.extra_chests[index - 1U];
    if (support && extra.platform_corner == 0U) {
        uint8_t corner = 0U;
        if (!SelectMapExtraSupportPlatformCornerWithReader(
                snapshot.artwork_bounds, extra.chest,
                actions->reader, &corner)) {
            return wait(error, "unarmed extra chest lacks a safe 2x2 standing platform");
        }
        if (!UpgradeMapExtraChestSupportSelectedPlatform(
                actions->map_state_path, extra, corner, error)) {
            return uncertain(error, "extra chest platform upgrade outcome is uncertain");
        }
        return MapStorageExternalResult::DurableTransition;
    }
    std::vector<MapExtraChestReadback> readbacks;
    std::vector<MapExtraChestPlacementRecord> prior(
        snapshot.extra_chests.begin(), snapshot.extra_chests.begin() + index - 1U);
    const auto survey = SurveyMapChestCandidateWithReader(
        actions->reader, extra.chest);
    if (!priorChestReadbacks(actions, snapshot, prior.size(), &readbacks) ||
        !ValidateMapExtraChestPlanChain(*snapshot.pair, prior, readbacks,
                                        extra, error) ||
        (support ?
            (extra.phase != MapExtraChestPlacementPhase::SupportSelected ||
             !safeAirFloorSurvey(survey)) :
            ((extra.phase != MapExtraChestPlacementPhase::SupportConfirmed &&
              extra.phase != MapExtraChestPlacementPhase::Selected) ||
             !safeChestSurvey(survey) ||
             (extra.support_created &&
              !exactStoneBelow(actions, extra.chest))))) {
        return wait(error, "extra chest lacks safe fresh native preflight");
    }
    std::string command = support
        ? "/fill " + std::to_string(extra.chest.x) + " " +
              std::to_string(extra.chest.y - 1) + " " +
              std::to_string(extra.chest.z) + " " +
              std::to_string(extra.chest.x) + " " +
              std::to_string(extra.chest.y - 1) + " " +
              std::to_string(extra.chest.z) + " minecraft:stone"
        : BuildSingleCellMapChestFillCommand(extra.chest);
    if (extra.platform_corner != 0U) {
        std::array<MapChestPosition, 4> cells;
        if (!BuildMapExtraSupportPlatformCells(
                snapshot.artwork_bounds, extra.chest,
                extra.platform_corner, &cells) ||
            !(support
                ? FreshMapSupportPlatformAirWithReader(actions->reader, cells)
                : ExactMapSupportPlatformStoneWithReader(actions->reader, cells))) {
            return wait(error, "extra chest standing platform lacks exact native preflight");
        }
        if (support) command = BuildMapSupportPlatformFillCommand(cells);
    }
    if (!approvePlacement(actions, command, error)) {
        return wait(error, "extra chest fill command syntax has not been approved");
    }
    std::string uuid;
    if (!nextCommandUuid(actions, &uuid, error)) {
        return wait(error, "tracked RPC command UUID is unavailable");
    }
    if (!(support
            ? ArmMapExtraChestSupportDispatch(actions->map_state_path,
                                              extra, uuid, error)
            : ArmMapExtraChestPlacementDispatch(actions->map_state_path,
                                                extra, uuid, error))) {
        return uncertain(error, support
            ? "extra chest support arm outcome is uncertain"
            : "extra chest arm outcome is uncertain");
    }
    if (!actions->hooks.dispatch_tracked_rpc ||
        !actions->hooks.dispatch_tracked_rpc(actions->hooks.context, uuid,
                                             command, error)) {
        return uncertain(error, "extra chest dispatch outcome is uncertain");
    }
    return MapStorageExternalResult::DurableTransition;
}

enum class PlacementReceipt : uint8_t {
    Pending,
    Accepted,
    ExplicitFailure,
    TransportUnavailable,
};

bool isAir(const NativeBlockInfo* block) {
    return block && (block->name == "minecraft:air" ||
                     block->name == "minecraft:cave_air" ||
                     block->name == "minecraft:void_air");
}

void logSupportRetry(const char* kind, const std::string& old_uuid,
                     const std::string& new_uuid,
                     const MapChestPosition& position) {
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "Infinitecz_BuildImport",
                        "[map-placement] support-retry kind=%s old=%s new=%s at=(%d,%d,%d)",
                        kind, old_uuid.c_str(), new_uuid.c_str(),
                        position.x, position.y, position.z);
#else
    (void)kind;
    (void)old_uuid;
    (void)new_uuid;
    (void)position;
#endif
}

void logNativePlacementRecovery(const char* kind, const std::string& uuid,
                                const MapChestPosition& position) {
#if defined(__ANDROID__)
    // The durable phase changes immediately after this call, so a successful
    // native recovery emits only one line per already-Armed placement.
    __android_log_print(ANDROID_LOG_INFO, "Infinitecz_BuildImport",
                        "[map-placement] native-no-ack kind=%s uuid=%s at=(%d,%d,%d)",
                        kind, uuid.c_str(), position.x, position.y, position.z);
#else
    (void)kind;
    (void)uuid;
    (void)position;
#endif
}

void logNativePlacementTimeout(const char* kind, const std::string& uuid,
                               const MapChestPosition& position,
                               const NativeBlockInfo* first,
                               const NativeBlockInfo* second) {
#if defined(__ANDROID__)
    // A timeout without proof immediately pauses the task, so this is one
    // bounded diagnostic line rather than a per-tick retry log.
    __android_log_print(ANDROID_LOG_INFO, "Infinitecz_BuildImport",
                        "[map-placement] no-ack-no-proof kind=%s uuid=%s at=(%d,%d,%d) first=%s second=%s",
                        kind, uuid.c_str(), position.x, position.y, position.z,
                        first ? first->name.c_str() : "<unreadable>",
                        second ? second->name.c_str() : "<unreadable>");
#else
    (void)kind;
    (void)uuid;
    (void)position;
    (void)first;
    (void)second;
#endif
}

bool exactPairReadback(MapPairPlacementStep step,
                       const NativeBlockInfo* chest,
                       const NativeBlockInfo* anvil) {
    if (!chest) return false;
    if (step == MapPairPlacementStep::Support) {
        return chest->name == "minecraft:stone" && anvil &&
               anvil->name == "minecraft:stone";
    }
    if (chest->name != "minecraft:chest") return false;
    if (step == MapPairPlacementStep::Chest) return true;
    return step == MapPairPlacementStep::Anvil && anvil &&
           anvil->name == "minecraft:anvil";
}

PlacementReceipt acceptedReceipt(MapStorageProductionActions* actions,
                                 const std::string& expected_uuid,
                                 std::string* error) {
    if (!actions->hooks.poll_tracked_rpc) {
        if (error) *error = "tracked placement ACK hook is unavailable";
        return PlacementReceipt::ExplicitFailure;
    }
    MapStorageTrackedRpcReceipt receipt;
    if (!actions->hooks.poll_tracked_rpc(
            actions->hooks.context, expected_uuid, &receipt, error)) {
        if (error && error->empty()) *error = "tracked placement ACK poll failed";
        // Only the runtime's known missing-receipt outcomes may fall back to
        // exact native world state. Invalid polls and explicit UUID mismatch
        // remain hard failures even if a block happens to match.
        if (error && (*error == "tracked map placement ACK timed out; journal retained" ||
                      *error == "tracked map placement ACK transport is unavailable")) {
            return PlacementReceipt::TransportUnavailable;
        }
        return PlacementReceipt::ExplicitFailure;
    }
    if (receipt.ack == MapPairCommandAck::Pending) {
        if (!receipt.returned_uuid.empty() &&
            receipt.returned_uuid != expected_uuid) {
            if (error) *error = "tracked placement ACK UUID changed";
            return PlacementReceipt::ExplicitFailure;
        }
        return PlacementReceipt::Pending;
    }
    if (receipt.returned_uuid != expected_uuid ||
        receipt.ack != MapPairCommandAck::Accepted) {
        if (error) *error = "tracked placement command was rejected or mismatched; journal retained";
        return PlacementReceipt::ExplicitFailure;
    }
    return PlacementReceipt::Accepted;
}

MapStorageExternalResult reconcilePair(MapStorageProductionActions* actions,
        const MapStorageCoordinatorInput& snapshot, MapPairPlacementStep step,
        std::string* error) {
    if (!snapshot.pair || snapshot.pair->active_command_uuid.empty()) {
        return MapStorageExternalResult::NoChange;
    }
    const PlacementReceipt receipt = acceptedReceipt(
        actions, snapshot.pair->active_command_uuid, error);
    if (receipt == PlacementReceipt::ExplicitFailure) {
        if (step == MapPairPlacementStep::Support &&
            !snapshot.pair->support_retry_used &&
            !BlockMapPairSupportRetry(actions->map_state_path,
                                     *snapshot.pair, error)) {
            return uncertain(error, "pair support rejection marker persistence is uncertain");
        }
        return uncertain(error, "tracked pair placement outcome is uncertain; never resend");
    }
    if (!openReader(actions, error)) {
        if (receipt == PlacementReceipt::TransportUnavailable) {
            return uncertain(error, "tracked pair placement lacks a receipt and native readback");
        }
        return MapStorageExternalResult::NoChange;
    }
    NativeBlockInfo chest;
    NativeBlockInfo anvil;
    const bool support = step == MapPairPlacementStep::Support;
    const NativeBlockInfo* chest_read = actions->reader->getBlock(
        snapshot.pair->pair.chest.x,
        snapshot.pair->pair.chest.y - (support ? 1 : 0),
        snapshot.pair->pair.chest.z, &chest) ? &chest : nullptr;
    const NativeBlockInfo* anvil_read =
        (support || step == MapPairPlacementStep::Anvil) &&
        actions->reader->getBlock(snapshot.pair->pair.anvil.x,
                                  snapshot.pair->pair.anvil.y - (support ? 1 : 0),
                                  snapshot.pair->pair.anvil.z, &anvil)
            ? &anvil : nullptr;
    std::array<MapChestPosition, 4> platform_cells;
    std::array<NativeBlockInfo, 2> pad_blocks;
    const NativeBlockInfo* pad_first = nullptr;
    const NativeBlockInfo* pad_second = nullptr;
    if (support && snapshot.pair->platform_side != 0U &&
        BuildMapPairSupportPlatformCells(
            snapshot.artwork_bounds, snapshot.pair->pair,
            snapshot.pair->platform_side, &platform_cells)) {
        pad_first = actions->reader->getBlock(
            platform_cells[2].x, platform_cells[2].y,
            platform_cells[2].z, &pad_blocks[0]) ? &pad_blocks[0] : nullptr;
        pad_second = actions->reader->getBlock(
            platform_cells[3].x, platform_cells[3].y,
            platform_cells[3].z, &pad_blocks[1]) ? &pad_blocks[1] : nullptr;
    }
    const bool exact_native = exactPairReadback(step, chest_read, anvil_read) &&
        (!support || snapshot.pair->platform_side == 0U ||
         (pad_first && pad_second &&
          pad_first->name == "minecraft:stone" &&
          pad_second->name == "minecraft:stone"));
    if (receipt != PlacementReceipt::Accepted) {
        if (!exact_native) {
            if (receipt == PlacementReceipt::TransportUnavailable) {
                if (support && !snapshot.pair->support_retry_used &&
                    isAir(chest_read) && isAir(anvil_read) &&
                    (snapshot.pair->platform_side == 0U ||
                     (isAir(pad_first) && isAir(pad_second)))) {
                    const MapChestPosition support_cell{
                        snapshot.pair->pair.chest.x,
                        snapshot.pair->pair.chest.y - 1,
                        snapshot.pair->pair.chest.z};
                    if (!actions->hooks.allow_support_retry) {
                        return uncertain(error, "pair support retry lacks a live near-site guard");
                    }
                    if (error) error->clear();
                    if (!actions->hooks.allow_support_retry(
                            actions->hooks.context, support_cell, error)) {
                        return wait(error, "waiting for nearby loaded pair support site");
                    }
                    MapPairPlacementCommand command;
                    if (PrepareMapPairPlacementCommandWithReader(
                            snapshot.artwork_bounds, snapshot.pair->pair,
                            MapPairPlacementStep::Support, false,
                            actions->reader, &command, true) !=
                            MapPairPlacementPreparation::Ready ||
                        (snapshot.pair->platform_side != 0U &&
                         !FreshMapSupportPlatformAirWithReader(
                             actions->reader, platform_cells))) {
                        return uncertain(error, "pair support retry lacks safe native preflight or approved fill");
                    }
                    if (snapshot.pair->platform_side != 0U) {
                        command.text = BuildMapSupportPlatformFillCommand(
                            platform_cells);
                    }
                    if (!approvePlacement(actions, command.text, error)) {
                        return uncertain(error, "pair support retry fill is not approved");
                    }
                    std::string replacement_uuid;
                    if (!nextCommandUuid(actions, &replacement_uuid, error)) {
                        return wait(error, "pair support retry UUID is unavailable");
                    }
                    if (!RearmMapPairSupportDispatchOnce(
                            actions->map_state_path, *snapshot.pair,
                            replacement_uuid, chest_read, anvil_read,
                            error, pad_first, pad_second)) {
                        return uncertain(error, "pair support retry journal outcome is uncertain");
                    }
                    const std::string old_uuid = snapshot.pair->active_command_uuid;
                    if (error) error->clear();
                    if (!actions->hooks.dispatch_tracked_rpc ||
                        !actions->hooks.dispatch_tracked_rpc(
                            actions->hooks.context, replacement_uuid,
                            command.text, error)) {
                        return uncertain(error, "pair support retry dispatch outcome is uncertain");
                    }
                    logSupportRetry("pair", old_uuid, replacement_uuid,
                                    support_cell);
                    if (error) error->clear();
                    return MapStorageExternalResult::DurableTransition;
                }
                const MapChestPosition position = step == MapPairPlacementStep::Support
                    ? MapChestPosition{snapshot.pair->pair.chest.x,
                                       snapshot.pair->pair.chest.y - 1,
                                       snapshot.pair->pair.chest.z}
                    : step == MapPairPlacementStep::Anvil
                        ? snapshot.pair->pair.anvil : snapshot.pair->pair.chest;
                logNativePlacementTimeout("pair", snapshot.pair->active_command_uuid,
                                          position, chest_read, anvil_read);
                return uncertain(error, "tracked pair placement lacks a receipt and exact native readback");
            }
            return MapStorageExternalResult::NoChange;
        }
        if (error) error->clear();
        if (!ConfirmMapPairPlacementStepByNativeReadback(
                actions->map_state_path, *snapshot.pair, step,
                chest_read, anvil_read, error, pad_first,
                pad_second)) {
            return uncertain(error, "pair native recovery persistence is uncertain");
        }
        const MapChestPosition position = step == MapPairPlacementStep::Support
            ? MapChestPosition{snapshot.pair->pair.chest.x,
                               snapshot.pair->pair.chest.y - 1,
                               snapshot.pair->pair.chest.z}
            : step == MapPairPlacementStep::Anvil
                ? snapshot.pair->pair.anvil : snapshot.pair->pair.chest;
        logNativePlacementRecovery("pair", snapshot.pair->active_command_uuid,
                                   position);
        return MapStorageExternalResult::DurableTransition;
    }
    if (!exact_native || VerifyMapPairPlacementAfterAck(
            step, MapPairCommandAck::Accepted, chest_read, anvil_read) !=
        MapPairPlacementVerification::Confirmed) {
        return wait(error, "accepted pair command lacks exact native block readback");
    }
    if (!ConfirmMapPairPlacementStep(
            actions->map_state_path, *snapshot.pair, step,
            snapshot.pair->active_command_uuid, MapPairCommandAck::Accepted,
            chest_read, anvil_read, error, pad_first, pad_second)) {
        return uncertain(error, "pair confirmation persistence is uncertain");
    }
    return MapStorageExternalResult::DurableTransition;
}

MapStorageExternalResult reconcileExtra(MapStorageProductionActions* actions,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision, bool support,
        std::string* error) {
    const uint16_t index = decision.chest_address.chest_index;
    if (index == 0U || index > snapshot.extra_chests.size()) {
        return wait(error, "extra chest journal is unavailable");
    }
    const auto& extra = snapshot.extra_chests[index - 1U];
    if (extra.command_uuid.empty()) {
        return MapStorageExternalResult::NoChange;
    }
    const PlacementReceipt receipt = acceptedReceipt(
        actions, extra.command_uuid, error);
    if (receipt == PlacementReceipt::ExplicitFailure) {
        if (support && !extra.support_retry_used &&
            !BlockMapExtraChestSupportRetry(actions->map_state_path,
                                            extra, error)) {
            return uncertain(error, "extra support rejection marker persistence is uncertain");
        }
        return uncertain(error, "tracked extra chest outcome is uncertain; never resend");
    }
    if (!openReader(actions, error)) {
        if (receipt == PlacementReceipt::TransportUnavailable) {
            return uncertain(error, "tracked extra chest lacks a receipt and native readback");
        }
        return MapStorageExternalResult::NoChange;
    }
    const MapChestPosition position = support
        ? MapChestPosition{extra.chest.x, extra.chest.y - 1, extra.chest.z}
        : extra.chest;
    MapExtraChestReadback readback;
    const bool primary_read = readChest(actions, position, &readback);
    std::array<MapChestPosition, 4> platform_cells;
    std::array<MapExtraChestReadback, 3> pad_readbacks;
    const bool platform_geometry = !support || extra.platform_corner == 0U ||
        BuildMapExtraSupportPlatformCells(
            snapshot.artwork_bounds, extra.chest,
            extra.platform_corner, &platform_cells);
    bool pads_exact_stone = platform_geometry;
    bool pads_exact_air = platform_geometry;
    if (support && extra.platform_corner != 0U && platform_geometry) {
        for (size_t i = 0; i < pad_readbacks.size(); ++i) {
            const bool read = readChest(actions, platform_cells[i + 1U],
                                        &pad_readbacks[i]);
            pads_exact_stone = pads_exact_stone && read &&
                pad_readbacks[i].block.name == "minecraft:stone";
            pads_exact_air = pads_exact_air && read &&
                isAir(&pad_readbacks[i].block);
        }
    }
    const auto* pad_evidence = support && extra.platform_corner != 0U
        ? &pad_readbacks : nullptr;
    const bool exact_native = primary_read &&
        readback.block.name ==
            (support ? "minecraft:stone" : "minecraft:chest") &&
        (!support || pads_exact_stone) &&
        (support || !extra.support_created ||
         exactStoneBelow(actions, extra.chest));
    if (!exact_native) {
        if (receipt == PlacementReceipt::TransportUnavailable) {
            if (support && !extra.support_retry_used && readback.available &&
                isAir(&readback.block) && pads_exact_air) {
                if (!actions->hooks.allow_support_retry) {
                    return uncertain(error, "extra support retry lacks a live near-site guard");
                }
                if (error) error->clear();
                if (!actions->hooks.allow_support_retry(
                        actions->hooks.context, position, error)) {
                    return wait(error, "waiting for nearby loaded extra support site");
                }
                if (!safeAirFloorSurvey(SurveyMapChestCandidateWithReader(
                        actions->reader, extra.chest)) ||
                    (extra.platform_corner != 0U &&
                     !FreshMapSupportPlatformAirWithReader(
                         actions->reader, platform_cells))) {
                    return uncertain(error, "extra support retry lacks safe native preflight");
                }
                const std::string command = extra.platform_corner != 0U
                    ? BuildMapSupportPlatformFillCommand(platform_cells)
                    : "/fill " +
                    std::to_string(position.x) + " " +
                    std::to_string(position.y) + " " +
                    std::to_string(position.z) + " " +
                    std::to_string(position.x) + " " +
                    std::to_string(position.y) + " " +
                    std::to_string(position.z) + " minecraft:stone";
                if (!approvePlacement(actions, command, error)) {
                    return uncertain(error, "extra support retry fill is not approved");
                }
                std::string replacement_uuid;
                if (!nextCommandUuid(actions, &replacement_uuid, error)) {
                    return wait(error, "extra support retry UUID is unavailable");
                }
                if (!RearmMapExtraChestSupportDispatchOnce(
                        actions->map_state_path, extra, replacement_uuid,
                        readback, error, pad_evidence)) {
                    return uncertain(error, "extra support retry journal outcome is uncertain");
                }
                const std::string old_uuid = extra.command_uuid;
                if (error) error->clear();
                if (!actions->hooks.dispatch_tracked_rpc ||
                    !actions->hooks.dispatch_tracked_rpc(
                        actions->hooks.context, replacement_uuid,
                        command, error)) {
                    return uncertain(error, "extra support retry dispatch outcome is uncertain");
                }
                logSupportRetry("extra", old_uuid, replacement_uuid,
                                position);
                if (error) error->clear();
                return MapStorageExternalResult::DurableTransition;
            }
            logNativePlacementTimeout(support ? "extra-support" : "extra-chest",
                                      extra.command_uuid, position,
                                      readback.available ? &readback.block : nullptr,
                                      nullptr);
            return uncertain(error, "tracked extra chest lacks a receipt and exact native readback");
        }
        if (receipt == PlacementReceipt::Pending) {
            return MapStorageExternalResult::NoChange;
        }
        return wait(error, support
            ? "accepted extra chest support lacks exact native readback"
            : "accepted extra chest lacks exact native readback");
    }
    if (receipt != PlacementReceipt::Accepted) {
        if (error) error->clear();
        const bool confirmed = support
            ? ConfirmMapExtraChestSupportByNativeReadback(
                  actions->map_state_path, extra, readback, error,
                  pad_evidence)
            : ConfirmMapExtraChestPlacementByNativeReadback(
                  actions->map_state_path, extra, readback, error);
        if (!confirmed) {
            return uncertain(error, support
                ? "extra chest support native recovery persistence is uncertain"
                : "extra chest native recovery persistence is uncertain");
        }
        logNativePlacementRecovery(support ? "extra-support" : "extra-chest",
                                   extra.command_uuid, position);
        return MapStorageExternalResult::DurableTransition;
    }
    if (!(support
            ? ConfirmMapExtraChestSupport(
                  actions->map_state_path, extra, extra.command_uuid,
                  MapPairCommandAck::Accepted, readback, error,
                  pad_evidence)
            : ConfirmMapExtraChestPlacement(
                  actions->map_state_path, extra, extra.command_uuid,
                  MapPairCommandAck::Accepted, readback, error))) {
        return uncertain(error, support
            ? "extra chest support confirmation persistence is uncertain"
            : "extra chest confirmation persistence is uncertain");
    }
    return MapStorageExternalResult::DurableTransition;
}

MapStorageExternalResult beginChestTransfer(MapStorageProductionActions* actions,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision, std::string* error) {
    if (!matchingPersistedPair(actions, snapshot, error) ||
        !matchingPersistedExtra(actions, snapshot, decision, error) ||
        !snapshot.rename ||
        !matchingPersistedRename(actions, snapshot, decision, error)) {
        return wait(error, "confirmed renamed map or chest plan is missing");
    }
    const MapChestPosition position = selectedChest(snapshot, decision);
    MapStorageHeldMapEvidence held;
    ContainerCaptureResult capture;
    if (!freshHeldMap(actions, snapshot, decision, &held, error) ||
        !captureChest(actions, position, false, &capture, error)) {
        return MapStorageExternalResult::NoChange;
    }
    uint8_t empty_slot = 0;
    if (!SelectEmptySingleChestSlot(capture, capture.token, position.x,
                                    position.y, position.z, &empty_slot,
                                    error) ||
        empty_slot != decision.chest_address.slot) {
        return wait(error, "deterministic chest slot is not the first empty slot");
    }
    MapChestTransferRecord prepared;
    prepared.world_id = snapshot.world_id;
    prepared.dimension_id = snapshot.dimension_id;
    prepared.tile_cursor = snapshot.checkpoint_tile_cursor;
    prepared.tile_count = snapshot.tile_count;
    prepared.chest_x = position.x;
    prepared.chest_y = position.y;
    prepared.chest_z = position.z;
    prepared.chest_index = decision.chest_address.chest_index;
    prepared.chest_slot = decision.chest_address.slot;
    prepared.source_runtime_item_id = held.source.runtime_item_id;
    prepared.source_network_stack_id = held.source.network_stack_id;
    prepared.source_map_uuid = held.source.map_uuid;
    prepared.pre_send_capture_token = capture.token;
    if (!BeginMapChestTransferJournal(actions->map_state_path,
                                      prepared, error)) {
        return uncertain(error, "chest transfer journal creation is uncertain");
    }
    return MapStorageExternalResult::DurableTransition;
}

struct ChestArmContext {
    const std::string* map_state_path = nullptr;
    const MapChestTransferRecord* prepared = nullptr;
    bool called = false;
    bool armed = false;
};

bool armChestBeforeSend(int32_t request_id, void* context,
                        std::string* error) {
    auto* arm = static_cast<ChestArmContext*>(context);
    if (!arm || !arm->map_state_path || !arm->prepared || arm->called) {
        if (error) *error = "chest pre-send arm callback was reused";
        return false;
    }
    arm->called = true;
    arm->armed = ArmMapChestTransferDispatch(
        *arm->map_state_path, *arm->prepared, request_id, error);
    return arm->armed;
}

MapStorageExternalResult submitChestTransfer(MapStorageProductionActions* actions,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision, std::string* error) {
    if (!matchingPersistedPair(actions, snapshot, error) ||
        !matchingPersistedExtra(actions, snapshot, decision, error) ||
        !snapshot.chest || !snapshot.rename ||
        snapshot.chest->phase != MapChestTransferPhase::Prepared ||
        !actions->hooks.submit_chest_place) {
        return wait(error, "prepared chest transfer or native sender is unavailable");
    }
    const MapChestPosition position = selectedChest(snapshot, decision);
    MapStorageHeldMapEvidence held;
    ContainerCaptureResult capture;
    if (!freshHeldMap(actions, snapshot, decision, &held, error) ||
        !captureChest(actions, position, false, &capture, error)) {
        return MapStorageExternalResult::NoChange;
    }
    uint8_t empty_slot = 0;
    if (!SelectEmptySingleChestSlot(capture, capture.token,
                                    position.x, position.y, position.z,
                                    &empty_slot, error) ||
        empty_slot != decision.chest_address.slot) {
        return wait(error, "target chest slot is no longer empty");
    }
    if (capture.token != snapshot.chest->pre_send_capture_token ||
        held.source.network_stack_id != snapshot.chest->source_network_stack_id) {
        if (!RefreshPreparedMapChestTransferJournal(
                actions->map_state_path, *snapshot.chest,
                held.source.network_stack_id, capture.token, error)) {
            return uncertain(error, "prepared chest refresh outcome is uncertain");
        }
        return MapStorageExternalResult::DurableTransition;
    }
    FilledMapToSingleChestRequest request;
    request.expected_world_context = snapshot.world_id;
    request.capture_token = capture.token;
    request.chest_x = position.x;
    request.chest_y = position.y;
    request.chest_z = position.z;
    request.expected_window_id = capture.container_id;
    request.destination_slot = decision.chest_address.slot;
    request.source_hotbar_slot = held.source.selected_hotbar_slot;
    request.source_network_stack_id = held.source.network_stack_id;
    request.expected_runtime_item_id = held.source.runtime_item_id;
    request.expected_map_uuid = held.source.map_uuid;
    ChestArmContext arm{&actions->map_state_path, snapshot.chest, false, false};
    request.pre_send = &armChestBeforeSend;
    request.pre_send_context = &arm;
    FilledMapChestSubmission submission;
    const bool sent = actions->hooks.submit_chest_place(
        actions->hooks.context, request, &submission, error);
    if (!arm.called || !arm.armed || !sent ||
        submission.submission_outcome_uncertain) {
        return uncertain(error, "chest Place outcome is uncertain; never resend");
    }
    return MapStorageExternalResult::DurableTransition;
}

MapStorageExternalResult reopenChestTransfer(MapStorageProductionActions* actions,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision, std::string* error) {
    if (!matchingPersistedPair(actions, snapshot, error) ||
        !matchingPersistedExtra(actions, snapshot, decision, error) ||
        !snapshot.chest || !snapshot.rename ||
        !matchingPersistedRename(actions, snapshot, decision, error)) {
        return wait(error, "chest or renamed map journal is unavailable");
    }
    const MapChestPosition position = selectedChest(snapshot, decision);
    if (!actions->hooks.capture_chest) {
        return wait(error, "chest close is unavailable");
    }
    ContainerCaptureResult closed_window;
    if (!actions->hooks.capture_chest(actions->hooks.context, position, true,
                                      &closed_window, error)) {
        return MapStorageExternalResult::NoChange;
    }
    // The live runtime persists ResponseAccepted only after the exact server
    // response has confirmed the named map's source and destination slots.
    // After a restart an Armed journal may have lost that ACK. Such a request
    // must never be sent again or treated as accepted merely because its
    // original window is gone.
    MapChestTransferRecord accepted;
    if (LoadMapChestTransferJournal(actions->map_state_path, &accepted,
                                    error) != MapChestJournalLoad::Loaded ||
        accepted.phase != MapChestTransferPhase::ResponseAccepted ||
        accepted.world_id != snapshot.world_id ||
        accepted.dimension_id != snapshot.dimension_id ||
        accepted.tile_cursor != snapshot.checkpoint_tile_cursor ||
        accepted.tile_count != snapshot.tile_count ||
        accepted.chest_x != position.x || accepted.chest_y != position.y ||
        accepted.chest_z != position.z ||
        accepted.chest_index != decision.chest_address.chest_index ||
        accepted.chest_slot != decision.chest_address.slot ||
        accepted.source_map_uuid != snapshot.chest->source_map_uuid ||
        accepted.source_runtime_item_id !=
            snapshot.chest->source_runtime_item_id ||
        accepted.request_id != snapshot.chest->request_id) {
        if (error && error->empty()) {
            *error = "chest Place has no persisted exact accepted response; do not resend";
        }
        return MapStorageExternalResult::ErrorBeforeDispatch;
    }
    // The exact accepted response already proves source count 0 and chest
    // destination count 1. In a live session the close hook also requires
    // the local source-slot sync and original-window close. On recovery the
    // old process/window is gone, while the durably accepted response remains
    // authoritative. A second full inventory walk is unreliable for this
    // client's occupied slots and adds no server-side guarantee.
    const MapChestAcceptedCloseProof proof{accepted.request_id, true};
    if (!ConfirmMapChestTransferAcceptedClose(
            actions->map_state_path, accepted, proof, error)) {
        return uncertain(error, "accepted chest close could not be persisted");
    }
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "Infinitecz_BuildImport",
                        "[map-chest-proof] accepted-and-closed request=%d uuid=%lld tile=%llu chest=(%d,%d,%d) slot=%u",
                        accepted.request_id,
                        static_cast<long long>(accepted.source_map_uuid),
                        static_cast<unsigned long long>(accepted.tile_cursor + 1U),
                        position.x, position.y, position.z,
                        static_cast<unsigned>(accepted.chest_slot));
#endif
    return MapStorageExternalResult::DurableTransition;
}

MapStorageExternalResult transition(void* context, MapStorageNextStep step,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision, std::string* error) {
    auto* actions = static_cast<MapStorageProductionActions*>(context);
    if (!live(actions, snapshot, error)) {
        return MapStorageExternalResult::NoChange;
    }
    if (IsMapStorageAnvilStep(step)) {
        return actions->anvil_adapter
            ? RunMapStorageAnvilTransition(actions->anvil_adapter, step,
                                            snapshot, decision, error)
            : wait(error, "verified anvil transition adapter is unavailable");
    }
    switch (step) {
        case MapStorageNextStep::AwaitPairJournal:
            return beginPair(actions, snapshot, error);
        case MapStorageNextStep::SurveyPairSupport:
            return sendPair(actions, snapshot, MapPairPlacementStep::Support,
                            error);
        case MapStorageNextStep::SurveyPairChest:
            return sendPair(actions, snapshot, MapPairPlacementStep::Chest, error);
        case MapStorageNextStep::SurveyPairAnvil:
            return sendPair(actions, snapshot, MapPairPlacementStep::Anvil, error);
        case MapStorageNextStep::AwaitExtraChestJournal:
            return beginExtra(actions, snapshot, decision, error);
        case MapStorageNextStep::SurveyExtraSupport:
            return sendExtra(actions, snapshot, decision, true, error);
        case MapStorageNextStep::SurveyExtraChest:
            return sendExtra(actions, snapshot, decision, false, error);
        case MapStorageNextStep::AwaitChestJournal:
            return beginChestTransfer(actions, snapshot, decision, error);
        case MapStorageNextStep::PreflightChestTransfer:
            return submitChestTransfer(actions, snapshot, decision, error);
        case MapStorageNextStep::CommitTileCursor:
            if (!snapshot.chest || !actions->hooks.commit_cursor_fsynced ||
                !matchingPersistedChest(actions, snapshot, decision, error)) {
                return wait(error, "durable map cursor commit is unavailable");
            }
            return actions->hooks.commit_cursor_fsynced(
                    actions->hooks.context, snapshot.checkpoint_tile_cursor,
                    snapshot.checkpoint_tile_cursor + 1U,
                    *snapshot.chest, error)
                ? MapStorageExternalResult::DurableTransition
                : uncertain(error, "map cursor commit outcome is uncertain");
        case MapStorageNextStep::ClearCommittedRenameJournal:
            if (!snapshot.rename || !snapshot.chest ||
                !ClearCommittedMapAnvilRenameJournal(
                    actions->map_state_path, *snapshot.rename, *snapshot.chest,
                    snapshot.checkpoint_tile_cursor, error)) {
                return uncertain(error, "rename journal cleanup outcome is uncertain");
            }
            return MapStorageExternalResult::DurableTransition;
        case MapStorageNextStep::ClearCommittedChestJournal:
            if (!snapshot.chest || !ClearCommittedMapChestTransferJournal(
                    actions->map_state_path, *snapshot.chest,
                    snapshot.checkpoint_tile_cursor, error)) {
                return uncertain(error, "chest journal cleanup outcome is uncertain");
            }
            return MapStorageExternalResult::DurableTransition;
        case MapStorageNextStep::FinalizeCommittedMapUse:
            if (!actions->hooks.finalize_committed_map_use) {
                return wait(error, "durable map-use marker finalizer is unavailable");
            }
            {
                MapAnvilRenameRecord rename;
                MapChestTransferRecord chest;
                if (LoadMapAnvilRenameJournal(actions->map_state_path,
                                               &rename, error) !=
                        MapAnvilRenameLoad::Missing ||
                    LoadMapChestTransferJournal(actions->map_state_path,
                                                &chest, error) !=
                        MapChestJournalLoad::Missing) {
                    return wait(error, "storage journals remain before map-use finalization");
                }
            }
            return actions->hooks.finalize_committed_map_use(
                    actions->hooks.context, snapshot.pending_map_use_cursor,
                    snapshot.checkpoint_tile_cursor, error)
                ? MapStorageExternalResult::DurableTransition
                : uncertain(error, "map-use marker finalization outcome is uncertain");
        case MapStorageNextStep::ClearExtraChestJournalsDescending:
            if (snapshot.extra_chests.empty() ||
                !ClearCompletedMapExtraChestPlacementJournal(
                    actions->map_state_path, snapshot.extra_chests.back(),
                    snapshot.checkpoint_tile_cursor, error)) {
                return uncertain(error, "extra chest journal cleanup outcome is uncertain");
            }
            return MapStorageExternalResult::DurableTransition;
        case MapStorageNextStep::ClearPairJournal:
            if (!snapshot.pair || !ClearCompletedMapPairPlacementJournal(
                    actions->map_state_path, *snapshot.pair,
                    snapshot.checkpoint_tile_cursor, error)) {
                return uncertain(error, "pair journal cleanup outcome is uncertain");
            }
            return MapStorageExternalResult::DurableTransition;
        default:
            return wait(error, "anvil action requires a separate verified adapter");
    }
}

MapStorageExternalResult reconcile(void* context, MapStorageNextStep step,
        const MapStorageCoordinatorInput& snapshot,
        const MapStorageCoordinatorDecision& decision, std::string* error) {
    auto* actions = static_cast<MapStorageProductionActions*>(context);
    if (!live(actions, snapshot, error)) {
        return MapStorageExternalResult::NoChange;
    }
    if (IsMapStorageAnvilStep(step)) {
        return actions->anvil_adapter
            ? RunMapStorageAnvilReconcileNoResend(actions->anvil_adapter,
                                                   step, snapshot, decision,
                                                   error)
            : wait(error, "verified anvil reconciliation adapter is unavailable");
    }
    switch (step) {
        case MapStorageNextStep::ReconcilePairSupportNoResend:
            return reconcilePair(actions, snapshot, MapPairPlacementStep::Support,
                                 error);
        case MapStorageNextStep::ReconcilePairChestNoResend:
            return reconcilePair(actions, snapshot, MapPairPlacementStep::Chest,
                                 error);
        case MapStorageNextStep::ReconcilePairAnvilNoResend:
            return reconcilePair(actions, snapshot, MapPairPlacementStep::Anvil,
                                 error);
        case MapStorageNextStep::ReconcileExtraChestNoResend:
            return reconcileExtra(actions, snapshot, decision, false, error);
        case MapStorageNextStep::ReconcileExtraSupportNoResend:
            return reconcileExtra(actions, snapshot, decision, true, error);
        case MapStorageNextStep::ReopenChestNoResend:
            return reopenChestTransfer(actions, snapshot, decision, error);
        default:
            return wait(error, "anvil reconciliation requires a separate verified adapter");
    }
}

}  // namespace

MapStoragePipelineOps MakeMapStorageProductionOps(
        MapStorageProductionActions* actions) noexcept {
    return {actions, &transition, &reconcile};
}

}  // namespace build_import
