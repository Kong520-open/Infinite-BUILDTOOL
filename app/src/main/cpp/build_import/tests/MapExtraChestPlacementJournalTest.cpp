#include "MapExtraChestPlacementJournal.h"

#include <cassert>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace build_import;

namespace build_import {
// This journal test supplies explicit block evidence, not a live reader.
bool NativeWorldAccess::getBlock(int32_t, int32_t, int32_t,
                                 NativeBlockInfo*) { return false; }
bool NativeWorldReader::getBlock(int32_t, int32_t, int32_t,
                                 NativeBlockInfo*) { return false; }
}

namespace {

MapExtraChestReadback chestAt(const MapChestPosition& position) {
    return {position, true, NativeBlockInfo{"minecraft:chest", 0, 54}};
}

MapExtraChestReadback stoneBelow(const MapChestPosition& chest) {
    return {{chest.x, chest.y - 1, chest.z}, true,
            NativeBlockInfo{"minecraft:stone", 0, 1}};
}

int64_t distance(const MapChestPosition& a, const MapChestPosition& b) {
    const int64_t dx = static_cast<int64_t>(a.x) - b.x;
    const int64_t dy = static_cast<int64_t>(a.y) - b.y;
    const int64_t dz = static_cast<int64_t>(a.z) - b.z;
    return (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) +
           (dz < 0 ? -dz : dz);
}

MapChestPosition farCandidate(const BlockBounds& bounds,
                              const MapChestPosition& first,
                              const MapChestPosition& second) {
    for (const MapChestPosition& candidate : EnumerateMapChestCandidates(bounds)) {
        if (distance(candidate, first) > 1 &&
            distance(candidate, second) > 1) return candidate;
    }
    assert(false);
    return {};
}

MapExtraChestPlacementRecord loaded(const std::string& state,
                                    uint16_t index) {
    MapExtraChestPlacementRecord record;
    std::string error;
    assert(LoadMapExtraChestPlacementJournal(state, index, &record, &error) ==
           MapExtraChestPlacementLoad::Loaded);
    assert(error.empty());
    return record;
}

}  // namespace

int main() {
    MapTileChestAddress address;
    assert(ResolveMapTileChestAddress(0, 55, &address) &&
           address.chest_index == 0 && address.slot == 0);
    assert(ResolveMapTileChestAddress(26, 55, &address) &&
           address.chest_index == 0 && address.slot == 26);
    assert(ResolveMapTileChestAddress(27, 55, &address) &&
           address.chest_index == 1 && address.slot == 0);
    assert(ResolveMapTileChestAddress(53, 55, &address) &&
           address.chest_index == 1 && address.slot == 26);
    assert(ResolveMapTileChestAddress(54, 55, &address) &&
           address.chest_index == 2 && address.slot == 0);
    assert(ResolveMapTileChestAddress(65535, 65536, &address) &&
           address.chest_index == 2427 && address.slot == 6);
    assert(!ResolveMapTileChestAddress(55, 55, &address));
    assert(!ResolveMapTileChestAddress(0, 65537, &address));
    assert(!ResolveMapTileChestAddress(0, 28, nullptr));

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("map_extra_chest_plan_test_" + std::to_string(unique));
    assert(std::filesystem::create_directory(directory));
    const std::string state = (directory / "map_creation.state").string();
    const BlockBounds bounds{0, 20, 0, 20, 20, 20};
    const auto pair_candidates = EnumerateMapChestAnvilCandidates(bounds);
    assert(!pair_candidates.empty());
    MapPairPlacementRecord pair;
    pair.world_id = "stable:v1:extra-test|0";
    pair.dimension_id = 0;
    pair.tile_count = 28;
    pair.artwork_bounds = bounds;
    pair.pair = pair_candidates.front();
    pair.phase = MapPairPlacementPhase::PairConfirmed;

    MapExtraChestPlacementRecord selected;
    selected.world_id = pair.world_id;
    selected.dimension_id = pair.dimension_id;
    selected.tile_count = pair.tile_count;
    selected.artwork_bounds = pair.artwork_bounds;
    selected.chest_index = 1;
    selected.chest = farCandidate(bounds, pair.pair.chest, pair.pair.anvil);
    selected.phase = MapExtraChestPlacementPhase::SupportSelected;
    selected.support_created = true;
    std::string error;

    MapExtraChestPlacementRecord invalid = selected;
    invalid.chest_index = 2;
    assert(!BeginMapExtraChestPlacementJournal(state, invalid, &error));
    invalid = selected;
    invalid.chest = {5, 20, 5};
    assert(!BeginMapExtraChestPlacementJournal(state, invalid, &error));
    invalid = selected;
    invalid.tile_count = 27;
    assert(!BeginMapExtraChestPlacementJournal(state, invalid, &error));
    invalid = selected;
    invalid.support_created = false;
    assert(!BeginMapExtraChestPlacementJournal(state, invalid, &error));
    assert(BeginMapExtraChestPlacementJournal(state, selected, &error));
    assert(!BeginMapExtraChestPlacementJournal(state, selected, &error));

    MapExtraChestPlacementRecord record = loaded(state, 1);
    assert(record.support_created);
    assert(ClassifyMapExtraChestPlacementResume(
               record, pair.world_id, 0, 28, bounds, 1) ==
           MapExtraChestPlacementResume::SurveySelectedSupport);
    assert(ClassifyMapExtraChestPlacementResume(
               record, "wrong-world", 0, 28, bounds, 1) ==
           MapExtraChestPlacementResume::Unsafe);
    assert(!ArmMapExtraChestPlacementDispatch(state, record,
                                              "chest-before-support", &error));
    assert(!ArmMapExtraChestSupportDispatch(state, record, "bad uuid", &error));
    assert(ArmMapExtraChestSupportDispatch(
        state, record, "extra-chest-1-support-abc", &error));
    record = loaded(state, 1);
    assert(record.phase == MapExtraChestPlacementPhase::SupportDispatchArmed);
    assert(record.support_created);
    assert(!record.support_retry_used); // legacy reserved byte decodes as zero
    assert(record.command_uuid == "extra-chest-1-support-abc");
    assert(ClassifyMapExtraChestPlacementResume(
               record, pair.world_id, 0, 28, bounds, 1) ==
           MapExtraChestPlacementResume::AmbiguousSupportDispatch);
    const MapExtraChestReadback exact_support = stoneBelow(selected.chest);
    MapExtraChestReadback wrong_support = exact_support;
    ++wrong_support.position.x;
    MapExtraChestReadback air_support = exact_support;
    air_support.block.name = "minecraft:air";
    assert(!RearmMapExtraChestSupportDispatchOnce(
        state, record, "extra-chest-1-support-retry", wrong_support, &error));
    assert(!RearmMapExtraChestSupportDispatchOnce(
        state, record, record.command_uuid, air_support, &error));
    assert(RearmMapExtraChestSupportDispatchOnce(
        state, record, "extra-chest-1-support-retry", air_support, &error));
    record = loaded(state, 1);
    assert(record.support_retry_used);
    assert(record.command_uuid == "extra-chest-1-support-retry");
    assert(!RearmMapExtraChestSupportDispatchOnce(
        state, record, "extra-chest-1-support-third", air_support, &error));
    assert(!ConfirmMapExtraChestSupport(
        state, record, "stale-support-ack", MapPairCommandAck::Accepted,
        exact_support, &error));
    assert(!ConfirmMapExtraChestSupport(
        state, record, record.command_uuid, MapPairCommandAck::Pending,
        exact_support, &error));
    assert(!ConfirmMapExtraChestSupport(
        state, record, record.command_uuid, MapPairCommandAck::Accepted,
        wrong_support, &error));
    assert(!ConfirmMapExtraChestSupport(
        state, record, record.command_uuid, MapPairCommandAck::Accepted,
        air_support, &error));
    assert(ConfirmMapExtraChestSupport(
        state, record, record.command_uuid, MapPairCommandAck::Accepted,
        exact_support, &error));
    record = loaded(state, 1);
    assert(record.phase == MapExtraChestPlacementPhase::SupportConfirmed);
    assert(record.support_created);
    assert(record.support_retry_used);
    assert(ClassifyMapExtraChestPlacementResume(
               record, pair.world_id, 0, 28, bounds, 1) ==
           MapExtraChestPlacementResume::VerifySupportBeforeChest);
    assert(!ArmMapExtraChestPlacementDispatch(
        state, record, record.command_uuid, &error));
    assert(ArmMapExtraChestPlacementDispatch(
        state, record, "extra-chest-1-command-abc", &error));
    record = loaded(state, 1);
    assert(record.phase == MapExtraChestPlacementPhase::DispatchArmed);
    assert(record.support_created);
    assert(record.command_uuid == "extra-chest-1-command-abc");
    assert(ClassifyMapExtraChestPlacementResume(
               record, pair.world_id, 0, 28, bounds, 1) ==
           MapExtraChestPlacementResume::AmbiguousDispatchNoResend);
    assert(!ArmMapExtraChestPlacementDispatch(
        state, selected, "extra-chest-1-command-def", &error));

    const MapExtraChestReadback exact = chestAt(selected.chest);
    MapExtraChestReadback wrong_position = exact;
    ++wrong_position.position.x;
    MapExtraChestReadback air = exact;
    air.block.name = "minecraft:air";
    assert(!ConfirmMapExtraChestPlacement(
        state, record, "stale-ack", MapPairCommandAck::Accepted, exact, &error));
    assert(!ConfirmMapExtraChestPlacement(
        state, record, record.command_uuid, MapPairCommandAck::Pending,
        exact, &error));
    assert(!ConfirmMapExtraChestPlacement(
        state, record, record.command_uuid, MapPairCommandAck::Rejected,
        exact, &error));
    assert(!ConfirmMapExtraChestPlacement(
        state, record, record.command_uuid, MapPairCommandAck::Accepted,
        wrong_position, &error));
    assert(!ConfirmMapExtraChestPlacement(
        state, record, record.command_uuid, MapPairCommandAck::Accepted,
        air, &error));
    assert(ConfirmMapExtraChestPlacement(
        state, record, record.command_uuid, MapPairCommandAck::Accepted,
        exact, &error));
    record = loaded(state, 1);
    assert(record.phase == MapExtraChestPlacementPhase::Confirmed);
    assert(record.support_created);
    assert(record.command_uuid == "extra-chest-1-command-abc");
    assert(ClassifyMapExtraChestPlacementResume(
               record, pair.world_id, 0, 28, bounds, 1) ==
           MapExtraChestPlacementResume::VerifyConfirmedChest);
    assert(!ConfirmMapExtraChestPlacement(
        state, record, record.command_uuid, MapPairCommandAck::Accepted,
        exact, &error));

    // Pair and every previous chest must be confirmed in this world and
    // native-read back. A new candidate cannot touch any prior chest.
    assert(ValidateMapExtraChestPlanChain(
        pair, {}, {chestAt(pair.pair.chest)}, record, &error));
    assert(!ValidateMapExtraChestPlanChain(pair, {}, {}, record, &error));
    assert(!ValidateMapExtraChestPlanChain(
        pair, {}, {chestAt(pair.pair.anvil)}, record, &error));
    MapPairPlacementRecord unconfirmed_pair = pair;
    unconfirmed_pair.phase = MapPairPlacementPhase::ChestConfirmed;
    assert(!ValidateMapExtraChestPlanChain(
        unconfirmed_pair, {}, {chestAt(pair.pair.chest)}, record, &error));
    MapExtraChestPlacementRecord duplicate = record;
    duplicate.chest = pair.pair.chest;
    assert(!ValidateMapExtraChestPlanChain(
        pair, {}, {chestAt(pair.pair.chest)}, duplicate, &error));
    MapExtraChestPlacementRecord adjacent = record;
    for (const MapChestPosition& candidate : EnumerateMapChestCandidates(bounds)) {
        if (candidate == pair.pair.chest ||
            distance(candidate, pair.pair.chest) != 1) continue;
        adjacent.chest = candidate;
        break;
    }
    assert(distance(adjacent.chest, pair.pair.chest) == 1);
    assert(!ValidateMapExtraChestPlanChain(
        pair, {}, {chestAt(pair.pair.chest)}, adjacent, &error));

    pair.tile_count = 55;
    MapExtraChestPlacementRecord prior = record;
    prior.tile_count = 55;
    MapExtraChestPlacementRecord second = prior;
    second.chest_index = 2;
    second.phase = MapExtraChestPlacementPhase::Selected;
    second.support_created = false;
    second.support_retry_used = false;
    second.command_uuid.clear();
    second.chest = farCandidate(bounds, pair.pair.chest, prior.chest);
    const std::vector<MapExtraChestReadback> readbacks{
        chestAt(pair.pair.chest), chestAt(prior.chest)};
    assert(ValidateMapExtraChestPlanChain(
        pair, {prior}, readbacks, second, &error));
    assert(!ValidateMapExtraChestPlanChain(
        pair, {}, readbacks, second, &error));
    MapExtraChestPlacementRecord stale_prior = prior;
    stale_prior.world_id = "other-world";
    assert(!ValidateMapExtraChestPlanChain(
        pair, {stale_prior}, readbacks, second, &error));
    std::vector<MapExtraChestReadback> missing_readback = readbacks;
    missing_readback[1].available = false;
    assert(!ValidateMapExtraChestPlanChain(
        pair, {prior}, missing_readback, second, &error));
    second.phase = MapExtraChestPlacementPhase::DispatchArmed;
    second.command_uuid = prior.command_uuid;
    assert(!ValidateMapExtraChestPlanChain(
        pair, {prior}, readbacks, second, &error));

    // The on-disk arming path also refuses a reused command UUID or a gap.
    const std::string chain_state = (directory / "chain.state").string();
    MapExtraChestPlacementRecord first_selected = prior;
    first_selected.phase = MapExtraChestPlacementPhase::Selected;
    first_selected.support_created = false;
    first_selected.support_retry_used = false;
    first_selected.command_uuid.clear();
    assert(BeginMapExtraChestPlacementJournal(chain_state, first_selected,
                                               &error));
    assert(ArmMapExtraChestPlacementDispatch(
        chain_state, first_selected, "extra-chest-1-command-abc", &error));
    MapExtraChestPlacementRecord first_armed = loaded(chain_state, 1);
    assert(ConfirmMapExtraChestPlacement(
        chain_state, first_armed, first_armed.command_uuid,
        MapPairCommandAck::Accepted, chestAt(first_armed.chest), &error));
    MapExtraChestPlacementRecord second_selected = second;
    second_selected.phase = MapExtraChestPlacementPhase::Selected;
    second_selected.command_uuid.clear();
    assert(BeginMapExtraChestPlacementJournal(chain_state, second_selected,
                                               &error));
    assert(!ArmMapExtraChestPlacementDispatch(
        chain_state, second_selected, first_armed.command_uuid, &error));
    assert(ArmMapExtraChestPlacementDispatch(
        chain_state, second_selected, "extra-chest-2-command-xyz", &error));
    MapExtraChestPlacementRecord second_armed = loaded(chain_state, 2);
    assert(ConfirmMapExtraChestPlacement(
        chain_state, second_armed, second_armed.command_uuid,
        MapPairCommandAck::Accepted, chestAt(second_armed.chest), &error));
    // A completed job can remove sidecars highest index first; if interrupted,
    // the final checkpoint allows its already-removed suffix to be recognized.
    assert(ClearCompletedMapExtraChestPlacementJournal(
        chain_state, loaded(chain_state, 2), 55, &error));
    assert(ClearCompletedMapExtraChestPlacementJournal(
        chain_state, loaded(chain_state, 1), 55, &error));

    assert(!ClearCompletedMapExtraChestPlacementJournal(state, record, 27, &error));
    assert(ClearCompletedMapExtraChestPlacementJournal(state, record, 28, &error));
    MapExtraChestPlacementRecord missing;
    assert(LoadMapExtraChestPlacementJournal(state, 1, &missing, &error) ==
           MapExtraChestPlacementLoad::Missing);

    const std::string native_state = (directory / "native.state").string();
    assert(BeginMapExtraChestPlacementJournal(native_state, selected, &error));
    auto native_record = loaded(native_state, 1);
    assert(ArmMapExtraChestSupportDispatch(
        native_state, native_record, "native-extra-support", &error));
    native_record = loaded(native_state, 1);
    assert(BlockMapExtraChestSupportRetry(native_state, native_record, &error));
    native_record = loaded(native_state, 1);
    assert(native_record.support_retry_used);
    assert(!BlockMapExtraChestSupportRetry(native_state, native_record, &error));
    assert(!RearmMapExtraChestSupportDispatchOnce(
        native_state, native_record, "native-extra-support-retry", air_support, &error));
    assert(!ConfirmMapExtraChestSupportByNativeReadback(
        native_state, native_record, air_support, &error));
    assert(!ConfirmMapExtraChestSupportByNativeReadback(
        native_state, native_record, wrong_support, &error));
    assert(ConfirmMapExtraChestSupportByNativeReadback(
        native_state, native_record, exact_support, &error));
    native_record = loaded(native_state, 1);
    assert(ArmMapExtraChestPlacementDispatch(
        native_state, native_record, "native-extra-chest", &error));
    native_record = loaded(native_state, 1);
    assert(!ConfirmMapExtraChestPlacementByNativeReadback(
        native_state, native_record, air, &error));
    assert(!ConfirmMapExtraChestPlacementByNativeReadback(
        native_state, native_record, wrong_position, &error));
    assert(ConfirmMapExtraChestPlacementByNativeReadback(
        native_state, native_record, exact, &error));
    native_record = loaded(native_state, 1);
    assert(native_record.phase == MapExtraChestPlacementPhase::Confirmed);
    assert(ClearCompletedMapExtraChestPlacementJournal(
        native_state, native_record, 28, &error));

    const std::string platform_state =
        (directory / "platform_extra.state").string();
    assert(BeginMapExtraChestPlacementJournal(
        platform_state, selected, &error));
    auto platform = loaded(platform_state, 1);
    assert(platform.platform_corner == 0U);
    assert(UpgradeMapExtraChestSupportSelectedPlatform(
        platform_state, platform, 1U, &error));
    platform = loaded(platform_state, 1);
    assert(platform.platform_corner == 1U);
    assert(ArmMapExtraChestSupportDispatch(
        platform_state, platform, "platform-extra-support", &error));
    platform = loaded(platform_state, 1);
    assert(!UpgradeMapExtraChestSupportSelectedPlatform(
        platform_state, platform, 2U, &error));
    std::array<MapChestPosition, 4> platform_cells;
    assert(BuildMapExtraSupportPlatformCells(
        bounds, platform.chest, platform.platform_corner,
        &platform_cells));
    std::array<MapExtraChestReadback, 3> air_pad;
    std::array<MapExtraChestReadback, 3> stone_pad;
    for (size_t i = 0; i < air_pad.size(); ++i) {
        air_pad[i] = {platform_cells[i + 1U], true,
                      NativeBlockInfo{"minecraft:air", 0, 0}};
        stone_pad[i] = {platform_cells[i + 1U], true,
                        NativeBlockInfo{"minecraft:stone", 0, 1}};
    }
    assert(!RearmMapExtraChestSupportDispatchOnce(
        platform_state, platform, "platform-extra-retry",
        air_support, &error));
    assert(RearmMapExtraChestSupportDispatchOnce(
        platform_state, platform, "platform-extra-retry",
        air_support, &error, &air_pad));
    platform = loaded(platform_state, 1);
    assert(platform.support_retry_used && platform.platform_corner == 1U);
    assert(!ConfirmMapExtraChestSupportByNativeReadback(
        platform_state, platform, exact_support, &error));
    assert(ConfirmMapExtraChestSupportByNativeReadback(
        platform_state, platform, exact_support, &error,
        &stone_pad));
    assert(std::filesystem::remove(
        MapExtraChestPlacementJournalPath(platform_state, 1)));

    // A crash leftover .tmp is a hard stop, not a reason to reselect/retry.
    const std::string sidecar = MapExtraChestPlacementJournalPath(state, 1);
    {
        std::ofstream temporary(sidecar + ".tmp", std::ios::binary);
        temporary.put('x');
    }
    assert(LoadMapExtraChestPlacementJournal(state, 1, &missing, &error) ==
           MapExtraChestPlacementLoad::Unsafe);
    assert(!BeginMapExtraChestPlacementJournal(state, selected, &error));
    assert(std::filesystem::remove(sidecar + ".tmp"));

    const std::string corrupt_state = (directory / "corrupt.state").string();
    assert(BeginMapExtraChestPlacementJournal(corrupt_state, selected, &error));
    const std::string corrupt_sidecar =
        MapExtraChestPlacementJournalPath(corrupt_state, 1);
    {
        std::fstream file(corrupt_sidecar,
                          std::ios::binary | std::ios::in | std::ios::out);
        assert(file);
        file.seekp(36);
        file.put('\x7f');
    }
    assert(LoadMapExtraChestPlacementJournal(corrupt_state, 1, &missing,
                                              &error) ==
           MapExtraChestPlacementLoad::Unsafe);
    assert(std::filesystem::remove(corrupt_sidecar));
    assert(std::filesystem::remove(directory));
}
