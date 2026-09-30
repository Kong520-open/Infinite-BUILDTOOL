#include "../MapChestWorldPlacement.h"

#include <cassert>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>

using namespace build_import;

namespace {

using Key = std::tuple<int32_t, int32_t, int32_t>;
std::map<Key, std::string> g_blocks;

void put(int32_t x, int32_t y, int32_t z, const char* name) {
    g_blocks[Key{x, y, z}] = name;
}

void emptySupportedSite(const MapChestPosition& position) {
    put(position.x, position.y, position.z, "minecraft:air");
    put(position.x, position.y + 1, position.z, "minecraft:air");
    put(position.x, position.y - 1, position.z, "minecraft:stone");
    put(position.x - 1, position.y, position.z, "minecraft:air");
    put(position.x + 1, position.y, position.z, "minecraft:air");
    put(position.x, position.y, position.z - 1, "minecraft:air");
    put(position.x, position.y, position.z + 1, "minecraft:air");
}

void resetWorld(const MapChestAnvilPosition& pair) {
    g_blocks.clear();
    emptySupportedSite(pair.chest);
    emptySupportedSite(pair.anvil);
}

}  // namespace

namespace build_import {

bool NativeWorldAccess::getBlock(int32_t x, int32_t y, int32_t z,
                                 NativeBlockInfo* output) {
    if (!output) return false;
    const auto found = g_blocks.find(Key{x, y, z});
    if (found == g_blocks.end()) return false;
    output->name = found->second;
    output->aux = 0;
    output->legacy_id = 0;
    return true;
}

bool NativeWorldReader::getBlock(int32_t x, int32_t y, int32_t z,
                                 NativeBlockInfo* output) {
    return NativeWorldAccess::getBlock(x, y, z, output);
}

}  // namespace build_import

int main() {
    const BlockBounds artwork{0, 64, 0, 0, 64, 0};
    const MapChestAnvilPosition pair =
        EnumerateMapChestAnvilCandidates(artwork).front();
    NativeWorldReader reader;
    MapPairPlacementCommand command;
    resetWorld(pair);

    assert(PrepareMapPairPlacementCommandWithReader(
        artwork, pair, MapPairPlacementStep::Chest, false,
        &reader, &command) == MapPairPlacementPreparation::Ready);
    assert(command.step == MapPairPlacementStep::Chest);
    assert(command.position == pair.chest);
    assert(command.text == BuildSingleCellMapChestFillCommand(pair.chest));
    assert(command.text == "/fill " + std::to_string(pair.chest.x) + " " +
           std::to_string(pair.chest.y) + " " + std::to_string(pair.chest.z) +
           " " + std::to_string(pair.chest.x) + " " +
           std::to_string(pair.chest.y) + " " +
           std::to_string(pair.chest.z) + " minecraft:chest");
    assert(command.requires_live_syntax_validation);
    assert(VerifyMapPairPlacementAfterAckWithReader(
        pair, MapPairPlacementStep::Chest, MapPairCommandAck::Pending,
        &reader) == MapPairPlacementVerification::PendingAck);
    assert(VerifyMapPairPlacementAfterAckWithReader(
        pair, MapPairPlacementStep::Chest, MapPairCommandAck::Rejected,
        &reader) == MapPairPlacementVerification::Rejected);
    assert(VerifyMapPairPlacementAfterAckWithReader(
        pair, MapPairPlacementStep::Chest, MapPairCommandAck::Accepted,
        &reader) == MapPairPlacementVerification::PendingNativeReadback);

    put(pair.chest.x, pair.chest.y, pair.chest.z, "minecraft:chest");
    assert(VerifyMapPairPlacementAfterAckWithReader(
        pair, MapPairPlacementStep::Chest, MapPairCommandAck::Accepted,
        &reader) == MapPairPlacementVerification::Confirmed);
    assert(PrepareMapPairPlacementCommandWithReader(
        artwork, pair, MapPairPlacementStep::Anvil, false,
        &reader, &command) == MapPairPlacementPreparation::ChestAckRequired);
    assert(PrepareMapPairPlacementCommandWithReader(
        artwork, pair, MapPairPlacementStep::Anvil, true,
        &reader, &command) == MapPairPlacementPreparation::Ready);
    assert(command.step == MapPairPlacementStep::Anvil);
    assert(command.position == pair.anvil);
    assert(command.text == "/fill " + std::to_string(pair.anvil.x) + " " +
           std::to_string(pair.anvil.y) + " " + std::to_string(pair.anvil.z) +
           " " + std::to_string(pair.anvil.x) + " " +
           std::to_string(pair.anvil.y) + " " +
           std::to_string(pair.anvil.z) + " minecraft:anvil");
    assert(command.requires_live_syntax_validation);
    assert(VerifyMapPairPlacementAfterAckWithReader(
        pair, MapPairPlacementStep::Anvil, MapPairCommandAck::Accepted,
        &reader) == MapPairPlacementVerification::PendingNativeReadback);

    put(pair.anvil.x, pair.anvil.y, pair.anvil.z, "minecraft:anvil");
    assert(VerifyMapPairPlacementAfterAckWithReader(
        pair, MapPairPlacementStep::Anvil, MapPairCommandAck::Accepted,
        &reader) == MapPairPlacementVerification::Confirmed);

    resetWorld(pair);
    put(pair.chest.x, pair.chest.y - 1, pair.chest.z, "minecraft:air");
    put(pair.anvil.x, pair.anvil.y - 1, pair.anvil.z, "minecraft:air");
    assert(PrepareMapPairPlacementCommandWithReader(
        artwork, pair, MapPairPlacementStep::Support, false,
        &reader, &command) == MapPairPlacementPreparation::Ready);
    assert(command.step == MapPairPlacementStep::Support);
    assert(command.text == "/fill " + std::to_string(pair.chest.x) + " " +
           std::to_string(pair.chest.y - 1) + " " +
           std::to_string(pair.chest.z) + " " +
           std::to_string(pair.anvil.x) + " " +
           std::to_string(pair.anvil.y - 1) + " " +
           std::to_string(pair.anvil.z) + " minecraft:stone");
    assert(VerifyMapPairPlacementAfterAckWithReader(
        pair, MapPairPlacementStep::Support, MapPairCommandAck::Pending,
        &reader) == MapPairPlacementVerification::PendingAck);
    assert(VerifyMapPairPlacementAfterAckWithReader(
        pair, MapPairPlacementStep::Support, MapPairCommandAck::Accepted,
        &reader) == MapPairPlacementVerification::PendingNativeReadback);
    put(pair.chest.x, pair.chest.y - 1, pair.chest.z, "minecraft:stone");
    assert(VerifyMapPairPlacementAfterAckWithReader(
        pair, MapPairPlacementStep::Support, MapPairCommandAck::Accepted,
        &reader) == MapPairPlacementVerification::PendingNativeReadback);
    put(pair.anvil.x, pair.anvil.y - 1, pair.anvil.z, "minecraft:stone");
    assert(VerifyMapPairPlacementAfterAckWithReader(
        pair, MapPairPlacementStep::Support, MapPairCommandAck::Accepted,
        &reader) == MapPairPlacementVerification::Confirmed);
    assert(PrepareMapPairPlacementCommandWithReader(
        artwork, pair, MapPairPlacementStep::Chest, false,
        &reader, &command) == MapPairPlacementPreparation::Ready);
    assert(PrepareMapPairPlacementCommandWithReader(
        artwork, pair, MapPairPlacementStep::Chest, false,
        &reader, &command, true) == MapPairPlacementPreparation::Ready);
    put(pair.anvil.x, pair.anvil.y - 1, pair.anvil.z, "minecraft:dirt");
    assert(VerifyMapPairPlacementAfterAckWithReader(
        pair, MapPairPlacementStep::Support, MapPairCommandAck::Accepted,
        &reader) == MapPairPlacementVerification::ConflictingWorldBlock);
    assert(PrepareMapPairPlacementCommandWithReader(
        artwork, pair, MapPairPlacementStep::Chest, false,
        &reader, &command, true) == MapPairPlacementPreparation::Unsafe);
    assert(PrepareMapPairPlacementCommandWithReader(
        artwork, pair, MapPairPlacementStep::Chest, false,
        &reader, &command) == MapPairPlacementPreparation::Ready);

    resetWorld(pair);
    put(pair.chest.x - 1, pair.chest.y, pair.chest.z,
        "minecraft:trapped_chest");
    const std::string old_command = command.text;
    assert(PrepareMapPairPlacementCommandWithReader(
        artwork, pair, MapPairPlacementStep::Chest, false,
        &reader, &command) == MapPairPlacementPreparation::Unsafe);
    assert(command.text == old_command);

    resetWorld(pair);
    g_blocks.erase(Key{pair.anvil.x, pair.anvil.y - 1, pair.anvil.z});
    assert(PrepareMapPairPlacementCommandWithReader(
        artwork, pair, MapPairPlacementStep::Chest, false,
        &reader, &command) == MapPairPlacementPreparation::Unavailable);

    resetWorld(pair);
    put(pair.chest.x, pair.chest.y, pair.chest.z, "minecraft:trapped_chest");
    assert(PrepareMapPairPlacementCommandWithReader(
        artwork, pair, MapPairPlacementStep::Anvil, true,
        &reader, &command) == MapPairPlacementPreparation::ChestNotConfirmed);
    assert(VerifyMapPairPlacementAfterAckWithReader(
        pair, MapPairPlacementStep::Chest, MapPairCommandAck::Accepted,
        &reader) == MapPairPlacementVerification::ConflictingWorldBlock);

    resetWorld(pair);
    MapChestAnvilPosition altered = pair;
    ++altered.anvil.x;
    assert(PrepareMapPairPlacementCommandWithReader(
        artwork, altered, MapPairPlacementStep::Chest, false,
        &reader, &command) == MapPairPlacementPreparation::InvalidPlan);

    // The support span also works for a north/south pair, not just x-adjacent
    // blocks. This is one fill command, never a square covering other cells.
    const MapChestAnvilPosition z_pair =
        EnumerateMapChestAnvilCandidates(artwork)[1];
    resetWorld(z_pair);
    put(z_pair.chest.x, z_pair.chest.y - 1, z_pair.chest.z,
        "minecraft:air");
    put(z_pair.anvil.x, z_pair.anvil.y - 1, z_pair.anvil.z,
        "minecraft:air");
    assert(PrepareMapPairPlacementCommandWithReader(
        artwork, z_pair, MapPairPlacementStep::Support, false,
        &reader, &command) == MapPairPlacementPreparation::Ready);
    assert(command.text == "/fill " + std::to_string(z_pair.chest.x) + " " +
           std::to_string(z_pair.chest.y - 1) + " " +
           std::to_string(z_pair.chest.z) + " " +
           std::to_string(z_pair.anvil.x) + " " +
           std::to_string(z_pair.anvil.y - 1) + " " +
           std::to_string(z_pair.anvil.z) + " minecraft:stone");

    // The new pair platform has two additional independent standing cells.
    // Legacy callers above retain their exact two-cell command.
    std::array<MapChestPosition, 4> platform;
    assert(BuildMapPairSupportPlatformCells(artwork, pair, 1U, &platform));
    assert(!BuildMapPairSupportPlatformCells(artwork, pair, 0U, &platform));
    MapChestPosition standing;
    assert(MapPairPlatformStandingCell(artwork, pair, 1U, &standing));
    assert(standing == platform[3] && standing.y == pair.chest.y - 1);
    g_blocks.clear();
    for (const auto& cell : platform) {
        put(cell.x, cell.y, cell.z, "minecraft:air");
        put(cell.x, cell.y + 1, cell.z, "minecraft:air");
        put(cell.x, cell.y + 2, cell.z, "minecraft:air");
    }
    uint8_t selected_side = 0U;
    assert(FreshMapSupportPlatformAirWithReader(&reader, platform));
    assert(SelectMapPairSupportPlatformSideWithReader(
        artwork, pair, &reader, &selected_side) && selected_side == 1U);
    assert(BuildMapSupportPlatformFillCommand(platform).find("minecraft:stone") !=
           std::string::npos);
    put(platform[3].x, platform[3].y, platform[3].z, "minecraft:dirt");
    assert(!FreshMapSupportPlatformAirWithReader(&reader, platform));
    put(platform[3].x, platform[3].y, platform[3].z, "minecraft:stone");
    for (size_t i = 0; i < 3; ++i) {
        const auto& cell = platform[i];
        put(cell.x, cell.y, cell.z, "minecraft:stone");
    }
    assert(ExactMapSupportPlatformStoneWithReader(&reader, platform));
    put(platform[2].x, platform[2].y, platform[2].z, "minecraft:air");
    assert(!ExactMapSupportPlatformStoneWithReader(&reader, platform));

    std::array<MapChestPosition, 4> extra_platform;
    assert(BuildMapExtraSupportPlatformCells(
        artwork, pair.chest, 1U, &extra_platform));
    assert(MapExtraPlatformStandingCell(
        artwork, pair.chest, 1U, &standing) &&
        standing == extra_platform[3]);
    assert(!BuildMapExtraSupportPlatformCells(
        artwork, pair.chest, 0U, &extra_platform));
    return 0;
}
