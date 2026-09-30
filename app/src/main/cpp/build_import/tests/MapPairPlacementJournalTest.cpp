#include "MapPairPlacementJournal.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace build_import;

namespace {

void load(const std::string& path, MapPairPlacementRecord* output) {
    std::string error;
    assert(LoadMapPairPlacementJournal(path, output, &error) ==
           MapPairPlacementLoad::Loaded);
    assert(error.empty());
}

MapPairPlacementResume resume(const MapPairPlacementRecord& record) {
    return ClassifyMapPairPlacementResume(
        record, "stable:v1:pair-test|0", 0, record.tile_count,
        record.artwork_bounds);
}

}  // namespace

namespace build_import {

// The journal test uses explicit block evidence, never a live world reader.
bool NativeWorldAccess::getBlock(int32_t, int32_t, int32_t,
                                 NativeBlockInfo*) { return false; }
bool NativeWorldReader::getBlock(int32_t, int32_t, int32_t,
                                 NativeBlockInfo*) { return false; }

}  // namespace build_import

int main() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("map_pair_plan_test_" + std::to_string(unique));
    assert(std::filesystem::create_directory(directory));
    const std::string state = (directory / "map_creation.state").string();
    const std::string sidecar = MapPairPlacementJournalPath(state);
    const BlockBounds bounds{0, 20, 0, 20, 20, 20};
    const auto candidates = EnumerateMapChestAnvilCandidates(bounds);
    assert(!candidates.empty());

    MapPairPlacementRecord plan;
    plan.world_id = "stable:v1:pair-test|0";
    plan.dimension_id = 0;
    plan.tile_count = 28;
    plan.artwork_bounds = bounds;
    plan.pair = candidates.front();
    plan.phase = MapPairPlacementPhase::SupportSelected;
    plan.support_created = true;
    std::string error;
    MapPairPlacementRecord loaded;
    MapPairPlacementRecord over_limit = plan;
    over_limit.tile_count = 65537;
    assert(!BeginMapPairPlacementJournal(state, over_limit, &error));
    MapPairPlacementRecord unknown_phase = plan;
    unknown_phase.phase = static_cast<MapPairPlacementPhase>(9);
    assert(!BeginMapPairPlacementJournal(state, unknown_phase, &error));
    assert(LoadMapPairPlacementJournal(state, &loaded, &error) ==
           MapPairPlacementLoad::Missing);
    assert(BeginMapPairPlacementJournal(state, plan, &error));
    assert(!BeginMapPairPlacementJournal(state, plan, &error));
    load(state, &loaded);
    assert(loaded.phase == MapPairPlacementPhase::SupportSelected);
    assert(loaded.support_created);
    assert(resume(loaded) == MapPairPlacementResume::SurveySelectedSupport);
    assert(ClassifyMapPairPlacementResume(loaded, "wrong-world", 0, 28, bounds) ==
           MapPairPlacementResume::Unsafe);
    assert(!ArmMapPairPlacementDispatch(state, loaded,
                                         MapPairPlacementStep::Anvil,
                                         "anvil-before-chest", &error));
    assert(!ArmMapPairPlacementDispatch(state, loaded,
                                         MapPairPlacementStep::Chest,
                                         "invalid command id", &error));
    assert(!ArmMapPairPlacementDispatch(state, loaded,
                                        MapPairPlacementStep::Chest,
                                        "chest-before-support", &error));

    assert(ArmMapPairPlacementDispatch(state, loaded,
                                        MapPairPlacementStep::Support,
                                        "support-command-1", &error));
    load(state, &loaded);
    assert(loaded.phase == MapPairPlacementPhase::SupportDispatchArmed);
    assert(!loaded.support_retry_used); // old version-2 reserved byte is zero
    assert(resume(loaded) == MapPairPlacementResume::AmbiguousSupportDispatch);
    assert(loaded.active_command_uuid == "support-command-1");
    assert(!ArmMapPairPlacementDispatch(state, loaded,
                                        MapPairPlacementStep::Support,
                                        "support-command-2", &error));
    const NativeBlockInfo stone{"minecraft:stone", 0, 1};
    const NativeBlockInfo air{"minecraft:air", 0, 0};
    assert(!RearmMapPairSupportDispatchOnce(
        state, loaded, "support-command-2", &stone, &air, &error));
    assert(!RearmMapPairSupportDispatchOnce(
        state, loaded, "support-command-1", &air, &air, &error));
    assert(RearmMapPairSupportDispatchOnce(
        state, loaded, "support-command-2", &air, &air, &error));
    load(state, &loaded);
    assert(loaded.support_retry_used);
    assert(loaded.active_command_uuid == "support-command-2");
    assert(!RearmMapPairSupportDispatchOnce(
        state, loaded, "support-command-3", &air, &air, &error));
    assert(!ConfirmMapPairPlacementStep(state, loaded,
                                        MapPairPlacementStep::Support,
                                        "wrong-uuid", MapPairCommandAck::Accepted,
                                        &stone, &stone, &error));
    assert(!ConfirmMapPairPlacementStep(state, loaded,
                                        MapPairPlacementStep::Support,
                                        "support-command-2", MapPairCommandAck::Pending,
                                        &stone, &stone, &error));
    assert(!ConfirmMapPairPlacementStep(state, loaded,
                                        MapPairPlacementStep::Support,
                                        "support-command-2", MapPairCommandAck::Accepted,
                                        &stone, &air, &error));
    assert(ConfirmMapPairPlacementStep(state, loaded,
                                       MapPairPlacementStep::Support,
                                       "support-command-2", MapPairCommandAck::Accepted,
                                       &stone, &stone, &error));
    load(state, &loaded);
    assert(loaded.phase == MapPairPlacementPhase::SupportConfirmed);
    assert(loaded.support_created);
    assert(loaded.support_retry_used);
    assert(resume(loaded) == MapPairPlacementResume::VerifySupportBeforeChest);
    assert(loaded.active_command_uuid.empty());

    assert(ArmMapPairPlacementDispatch(state, loaded,
                                        MapPairPlacementStep::Chest,
                                        "chest-command-1", &error));
    load(state, &loaded);
    assert(loaded.phase == MapPairPlacementPhase::ChestDispatchArmed);
    assert(loaded.active_command_uuid == "chest-command-1");
    assert(resume(loaded) == MapPairPlacementResume::AmbiguousChestDispatch);
    assert(!ArmMapPairPlacementDispatch(state, plan,
                                         MapPairPlacementStep::Chest,
                                         "chest-command-2", &error));
    const NativeBlockInfo chest{"minecraft:chest", 0, 54};
    const NativeBlockInfo anvil{"minecraft:anvil", 0, 145};
    assert(!ConfirmMapPairPlacementStep(state, loaded,
                                         MapPairPlacementStep::Chest,
                                         "wrong-ack-command",
                                         MapPairCommandAck::Accepted,
                                         &chest, nullptr, &error));
    assert(!ConfirmMapPairPlacementStep(state, loaded,
                                         MapPairPlacementStep::Chest,
                                         "chest-command-1",
                                         MapPairCommandAck::Pending,
                                         &chest, nullptr, &error));
    assert(!ConfirmMapPairPlacementStep(state, loaded,
                                         MapPairPlacementStep::Chest,
                                         "chest-command-1",
                                         MapPairCommandAck::Accepted,
                                         &air, nullptr, &error));
    assert(ConfirmMapPairPlacementStep(state, loaded,
                                        MapPairPlacementStep::Chest,
                                        "chest-command-1",
                                        MapPairCommandAck::Accepted,
                                        &chest, nullptr, &error));
    load(state, &loaded);
    assert(resume(loaded) == MapPairPlacementResume::VerifyChestBeforeAnvil);
    assert(loaded.active_command_uuid.empty());
    assert(ArmMapPairPlacementDispatch(state, loaded,
                                        MapPairPlacementStep::Anvil,
                                        "anvil-command-1", &error));
    load(state, &loaded);
    assert(resume(loaded) == MapPairPlacementResume::AmbiguousAnvilDispatch);
    assert(loaded.active_command_uuid == "anvil-command-1");
    assert(!ConfirmMapPairPlacementStep(state, loaded,
                                         MapPairPlacementStep::Anvil,
                                         "chest-command-1",
                                         MapPairCommandAck::Accepted,
                                         &chest, &anvil, &error));
    assert(!ConfirmMapPairPlacementStep(state, loaded,
                                         MapPairPlacementStep::Anvil,
                                         "anvil-command-1",
                                         MapPairCommandAck::Accepted,
                                         &chest, &air, &error));
    assert(ConfirmMapPairPlacementStep(state, loaded,
                                        MapPairPlacementStep::Anvil,
                                        "anvil-command-1",
                                        MapPairCommandAck::Accepted,
                                        &chest, &anvil, &error));
    load(state, &loaded);
    assert(resume(loaded) == MapPairPlacementResume::VerifyPairBeforeStorage);
    assert(loaded.support_created);
    assert(loaded.support_retry_used);
    assert(loaded.active_command_uuid.empty());
    assert(!ClearCompletedMapPairPlacementJournal(state, loaded, 1, &error));
    assert(ClearCompletedMapPairPlacementJournal(state, loaded, 28, &error));
    assert(LoadMapPairPlacementJournal(state, &loaded, &error) ==
           MapPairPlacementLoad::Missing);

    // Journals created by earlier builds still decode with their original
    // phase numbers and can use their direct chest -> anvil transition.
    MapPairPlacementRecord legacy = plan;
    legacy.phase = MapPairPlacementPhase::Selected;
    legacy.support_created = false;
    assert(BeginMapPairPlacementJournal(state, legacy, &error));
    load(state, &loaded);
    assert(resume(loaded) == MapPairPlacementResume::SurveySelectedPair);
    assert(!loaded.support_created);
    assert(!loaded.support_retry_used);
    assert(!ArmMapPairPlacementDispatch(state, loaded,
                                        MapPairPlacementStep::Support,
                                        "legacy-support", &error));
    assert(ArmMapPairPlacementDispatch(state, loaded,
                                       MapPairPlacementStep::Chest,
                                       "legacy-chest", &error));
    load(state, &loaded);
    assert(ConfirmMapPairPlacementStep(state, loaded,
                                       MapPairPlacementStep::Chest,
                                       "legacy-chest", MapPairCommandAck::Accepted,
                                       &chest, nullptr, &error));
    load(state, &loaded);
    assert(ArmMapPairPlacementDispatch(state, loaded,
                                       MapPairPlacementStep::Anvil,
                                       "legacy-anvil", &error));
    load(state, &loaded);
    assert(ConfirmMapPairPlacementStep(state, loaded,
                                       MapPairPlacementStep::Anvil,
                                       "legacy-anvil", MapPairCommandAck::Accepted,
                                       &chest, &anvil, &error));
    load(state, &loaded);
    assert(ClearCompletedMapPairPlacementJournal(state, loaded, 28, &error));

    // A missing RPC receipt may be recovered from an exact native world
    // state, but never from air, a partial support platform, or stale phase.
    assert(BeginMapPairPlacementJournal(state, plan, &error));
    load(state, &loaded);
    assert(ArmMapPairPlacementDispatch(state, loaded,
                                       MapPairPlacementStep::Support,
                                       "native-support", &error));
    load(state, &loaded);
    assert(BlockMapPairSupportRetry(state, loaded, &error));
    load(state, &loaded);
    assert(loaded.support_retry_used);
    assert(!BlockMapPairSupportRetry(state, loaded, &error));
    assert(!RearmMapPairSupportDispatchOnce(
        state, loaded, "native-support-retry", &air, &air, &error));
    assert(!ConfirmMapPairPlacementStepByNativeReadback(
        state, loaded, MapPairPlacementStep::Support, &stone, &air, &error));
    assert(ConfirmMapPairPlacementStepByNativeReadback(
        state, loaded, MapPairPlacementStep::Support, &stone, &stone, &error));
    assert(!ConfirmMapPairPlacementStepByNativeReadback(
        state, loaded, MapPairPlacementStep::Support, &stone, &stone, &error));
    load(state, &loaded);
    assert(ArmMapPairPlacementDispatch(state, loaded,
                                       MapPairPlacementStep::Chest,
                                       "native-chest", &error));
    load(state, &loaded);
    assert(!ConfirmMapPairPlacementStepByNativeReadback(
        state, loaded, MapPairPlacementStep::Chest, &air, nullptr, &error));
    assert(ConfirmMapPairPlacementStepByNativeReadback(
        state, loaded, MapPairPlacementStep::Chest, &chest, nullptr, &error));
    load(state, &loaded);
    assert(ArmMapPairPlacementDispatch(state, loaded,
                                       MapPairPlacementStep::Anvil,
                                       "native-anvil", &error));
    load(state, &loaded);
    assert(!ConfirmMapPairPlacementStepByNativeReadback(
        state, loaded, MapPairPlacementStep::Anvil, &chest, &air, &error));
    assert(ConfirmMapPairPlacementStepByNativeReadback(
        state, loaded, MapPairPlacementStep::Anvil, &chest, &anvil, &error));
    load(state, &loaded);
    assert(loaded.phase == MapPairPlacementPhase::PairConfirmed);
    assert(ClearCompletedMapPairPlacementJournal(state, loaded, 28, &error));

    // An unarmed old support plan may adopt a 2x2 pad, but its four-cell
    // proof and retry are then durable obligations. An Armed legacy plan
    // above retained platform_side=0 and its original two-cell geometry.
    const std::string platform_state =
        (directory / "platform_map.state").string();
    assert(BeginMapPairPlacementJournal(platform_state, plan, &error));
    MapPairPlacementRecord platform;
    load(platform_state, &platform);
    assert(platform.platform_side == 0U);
    assert(UpgradeMapPairSupportSelectedPlatform(
        platform_state, platform, 1U, &error));
    load(platform_state, &platform);
    assert(platform.platform_side == 1U);
    assert(!UpgradeMapPairSupportSelectedPlatform(
        platform_state, platform, 2U, &error));
    assert(ArmMapPairPlacementDispatch(
        platform_state, platform, MapPairPlacementStep::Support,
        "platform-support", &error));
    load(platform_state, &platform);
    assert(platform.platform_side == 1U);
    assert(!UpgradeMapPairSupportSelectedPlatform(
        platform_state, platform, 2U, &error));
    assert(!RearmMapPairSupportDispatchOnce(
        platform_state, platform, "platform-retry", &air, &air, &error));
    assert(RearmMapPairSupportDispatchOnce(
        platform_state, platform, "platform-retry", &air, &air,
        &error, &air, &air));
    load(platform_state, &platform);
    assert(platform.support_retry_used && platform.platform_side == 1U);
    assert(!ConfirmMapPairPlacementStepByNativeReadback(
        platform_state, platform, MapPairPlacementStep::Support,
        &stone, &stone, &error));
    assert(!ConfirmMapPairPlacementStepByNativeReadback(
        platform_state, platform, MapPairPlacementStep::Support,
        &stone, &stone, &error, &stone, &air));
    assert(ConfirmMapPairPlacementStepByNativeReadback(
        platform_state, platform, MapPairPlacementStep::Support,
        &stone, &stone, &error, &stone, &stone));
    load(platform_state, &platform);
    assert(platform.phase == MapPairPlacementPhase::SupportConfirmed &&
           platform.platform_side == 1U);
    assert(std::filesystem::remove(
        MapPairPlacementJournalPath(platform_state)));

    // A crash leaving a temporary file is never silently cleaned up.
    {
        std::ofstream temporary(sidecar + ".tmp", std::ios::binary);
        temporary.put('x');
    }
    assert(LoadMapPairPlacementJournal(state, &loaded, &error) ==
           MapPairPlacementLoad::Unsafe);
    assert(!BeginMapPairPlacementJournal(state, plan, &error));
    assert(std::filesystem::remove(sidecar + ".tmp"));
    assert(std::filesystem::remove(directory));
}
