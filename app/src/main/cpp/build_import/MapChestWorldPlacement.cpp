#include "MapChestWorldPlacement.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace build_import {
namespace {

bool isPlannedPair(const BlockBounds& artwork_bounds,
                   const MapChestAnvilPosition& pair) {
    const std::vector<MapChestAnvilPosition> candidates =
        EnumerateMapChestAnvilCandidates(artwork_bounds);
    for (const MapChestAnvilPosition& candidate : candidates) {
        if (candidate == pair) return true;
    }
    return false;
}

bool hasUnknownCell(const MapChestCandidateSurvey& survey) {
    return survey.target == MapChestCell::Unknown ||
           survey.above == MapChestCell::Unknown ||
           survey.below == MapChestCell::Unknown ||
           survey.north == MapChestCell::Unknown ||
           survey.south == MapChestCell::Unknown ||
           survey.west == MapChestCell::Unknown ||
           survey.east == MapChestCell::Unknown;
}

bool emptySupportedSite(const MapChestCandidateSurvey& survey) {
    if (survey.target != MapChestCell::Air ||
        survey.above != MapChestCell::Air ||
        survey.below != MapChestCell::SolidSupport) return false;
    return survey.north != MapChestCell::Chest &&
           survey.south != MapChestCell::Chest &&
           survey.west != MapChestCell::Chest &&
           survey.east != MapChestCell::Chest;
}

bool emptyAirFloorSite(const MapChestCandidateSurvey& survey) {
    if (survey.target != MapChestCell::Air ||
        survey.above != MapChestCell::Air ||
        survey.below != MapChestCell::Air) return false;
    return survey.north != MapChestCell::Chest &&
           survey.south != MapChestCell::Chest &&
           survey.west != MapChestCell::Chest &&
           survey.east != MapChestCell::Chest;
}

bool exactPlacedChest(const MapChestCandidateSurvey& survey,
                      std::string_view identifier) {
    if (identifier != "minecraft:chest" ||
        survey.target != MapChestCell::Chest ||
        survey.above != MapChestCell::Air ||
        survey.below != MapChestCell::SolidSupport) return false;
    return survey.north != MapChestCell::Chest &&
           survey.south != MapChestCell::Chest &&
           survey.west != MapChestCell::Chest &&
           survey.east != MapChestCell::Chest;
}

bool safeAnvilBesidePlannedChest(const MapChestCandidateSurvey& survey,
                                 const MapChestAnvilPosition& pair) {
    if (survey.target != MapChestCell::Air ||
        survey.above != MapChestCell::Air ||
        survey.below != MapChestCell::SolidSupport) return false;
    const std::array<MapChestCell, 4> cells{{
        survey.north, survey.south, survey.west, survey.east,
    }};
    const std::array<MapChestPosition, 4> neighbors{{
        {pair.anvil.x, pair.anvil.y, pair.anvil.z - 1},
        {pair.anvil.x, pair.anvil.y, pair.anvil.z + 1},
        {pair.anvil.x - 1, pair.anvil.y, pair.anvil.z},
        {pair.anvil.x + 1, pair.anvil.y, pair.anvil.z},
    }};
    for (size_t index = 0; index < cells.size(); ++index) {
        if (cells[index] == MapChestCell::Chest &&
            !(neighbors[index] == pair.chest)) return false;
        if (cells[index] != MapChestCell::Chest &&
            neighbors[index] == pair.chest) return false;
    }
    return true;
}

std::string fillCommand(const MapChestPosition& first,
                        const MapChestPosition& last,
                        std::string_view block_name) {
    return "/fill " + std::to_string(first.x) + " " +
        std::to_string(first.y) + " " + std::to_string(first.z) + " " +
        std::to_string(last.x) + " " + std::to_string(last.y) + " " +
        std::to_string(last.z) + " " + std::string(block_name);
}

std::string supportFillCommand(const MapChestAnvilPosition& pair) {
    const MapChestPosition first{
        std::min(pair.chest.x, pair.anvil.x), pair.chest.y - 1,
        std::min(pair.chest.z, pair.anvil.z)};
    const MapChestPosition last{
        std::max(pair.chest.x, pair.anvil.x), pair.chest.y - 1,
        std::max(pair.chest.z, pair.anvil.z)};
    return fillCommand(first, last, "minecraft:stone");
}

bool isAirName(std::string_view name) {
    return name == "minecraft:air" || name == "minecraft:cave_air" ||
           name == "minecraft:void_air";
}

MapPairPlacementVerification verifyBlockName(const NativeBlockInfo* block,
                                             std::string_view expected) {
    if (!block || block->name.empty()) {
        return MapPairPlacementVerification::PendingNativeReadback;
    }
    if (block->name == expected) return MapPairPlacementVerification::Confirmed;
    return isAirName(block->name)
        ? MapPairPlacementVerification::PendingNativeReadback
        : MapPairPlacementVerification::ConflictingWorldBlock;
}

}  // namespace

MapPairPlacementPreparation PrepareMapPairPlacementCommand(
        const BlockBounds& artwork_bounds, const MapChestAnvilPosition& pair,
        MapPairPlacementStep step, const MapPairPlacementEvidence& evidence,
        MapPairPlacementCommand* command) {
    if (!command || !isPlannedPair(artwork_bounds, pair) ||
        !(evidence.chest.position == pair.chest) ||
        !(evidence.anvil.position == pair.anvil)) {
        return MapPairPlacementPreparation::InvalidPlan;
    }
    if (step == MapPairPlacementStep::Anvil && !evidence.chest_ack_accepted) {
        return MapPairPlacementPreparation::ChestAckRequired;
    }
    if (hasUnknownCell(evidence.chest) || hasUnknownCell(evidence.anvil)) {
        return MapPairPlacementPreparation::Unavailable;
    }
    if (step == MapPairPlacementStep::Support) {
        if (!emptyAirFloorSite(evidence.chest) ||
            !emptyAirFloorSite(evidence.anvil)) {
            return MapPairPlacementPreparation::Unsafe;
        }
        *command = MapPairPlacementCommand{
            step, MapChestPosition{pair.chest.x, pair.chest.y - 1, pair.chest.z},
            supportFillCommand(pair), true};
        return MapPairPlacementPreparation::Ready;
    }
    if (evidence.require_exact_stone_floor) {
        if (evidence.chest_floor_identifier.empty() ||
            evidence.anvil_floor_identifier.empty()) {
            return MapPairPlacementPreparation::Unavailable;
        }
        if (evidence.chest_floor_identifier != "minecraft:stone" ||
            evidence.anvil_floor_identifier != "minecraft:stone") {
            return MapPairPlacementPreparation::Unsafe;
        }
    }
    if (step == MapPairPlacementStep::Chest) {
        if (!emptySupportedSite(evidence.chest) ||
            !emptySupportedSite(evidence.anvil)) {
            return MapPairPlacementPreparation::Unsafe;
        }
        *command = MapPairPlacementCommand{
            step, pair.chest,
            BuildSingleCellMapChestFillCommand(pair.chest), true};
        return MapPairPlacementPreparation::Ready;
    }
    if (step != MapPairPlacementStep::Anvil) {
        return MapPairPlacementPreparation::InvalidPlan;
    }
    if (evidence.chest_identifier.empty()) {
        return MapPairPlacementPreparation::Unavailable;
    }
    if (!exactPlacedChest(evidence.chest, evidence.chest_identifier)) {
        return MapPairPlacementPreparation::ChestNotConfirmed;
    }
    if (!safeAnvilBesidePlannedChest(evidence.anvil, pair)) {
        return MapPairPlacementPreparation::Unsafe;
    }
    *command = MapPairPlacementCommand{
        step, pair.anvil,
        fillCommand(pair.anvil, pair.anvil, "minecraft:anvil"), true};
    return MapPairPlacementPreparation::Ready;
}

MapPairPlacementPreparation PrepareMapPairPlacementCommandWithReader(
        const BlockBounds& artwork_bounds, const MapChestAnvilPosition& pair,
        MapPairPlacementStep step, bool chest_ack_accepted,
        NativeWorldReader* reader, MapPairPlacementCommand* command,
        bool require_exact_stone_floor) {
    if (!reader) return MapPairPlacementPreparation::Unavailable;
    MapPairPlacementEvidence evidence;
    evidence.chest = SurveyMapChestCandidateWithReader(reader, pair.chest);
    evidence.anvil = SurveyMapChestCandidateWithReader(reader, pair.anvil);
    evidence.chest_ack_accepted = chest_ack_accepted;
    evidence.require_exact_stone_floor = require_exact_stone_floor;
    if (require_exact_stone_floor && step != MapPairPlacementStep::Support) {
        NativeBlockInfo chest_floor;
        NativeBlockInfo anvil_floor;
        if (reader->getBlock(pair.chest.x, pair.chest.y - 1, pair.chest.z,
                             &chest_floor)) {
            evidence.chest_floor_identifier = std::move(chest_floor.name);
        }
        if (reader->getBlock(pair.anvil.x, pair.anvil.y - 1, pair.anvil.z,
                             &anvil_floor)) {
            evidence.anvil_floor_identifier = std::move(anvil_floor.name);
        }
    }
    if (step == MapPairPlacementStep::Anvil) {
        NativeBlockInfo block;
        if (reader->getBlock(pair.chest.x, pair.chest.y, pair.chest.z,
                             &block)) {
            evidence.chest_identifier = std::move(block.name);
        }
    }
    return PrepareMapPairPlacementCommand(
        artwork_bounds, pair, step, evidence, command);
}

MapPairPlacementVerification VerifyMapPairPlacementAfterAck(
        MapPairPlacementStep step, MapPairCommandAck ack,
        const NativeBlockInfo* chest, const NativeBlockInfo* anvil) {
    if (ack == MapPairCommandAck::Pending) {
        return MapPairPlacementVerification::PendingAck;
    }
    if (ack == MapPairCommandAck::Rejected) {
        return MapPairPlacementVerification::Rejected;
    }
    if (step == MapPairPlacementStep::Support) {
        const MapPairPlacementVerification chest_floor =
            verifyBlockName(chest, "minecraft:stone");
        const MapPairPlacementVerification anvil_floor =
            verifyBlockName(anvil, "minecraft:stone");
        if (chest_floor == MapPairPlacementVerification::ConflictingWorldBlock ||
            anvil_floor == MapPairPlacementVerification::ConflictingWorldBlock) {
            return MapPairPlacementVerification::ConflictingWorldBlock;
        }
        return chest_floor == MapPairPlacementVerification::Confirmed &&
                       anvil_floor == MapPairPlacementVerification::Confirmed
            ? MapPairPlacementVerification::Confirmed
            : MapPairPlacementVerification::PendingNativeReadback;
    }
    if (step != MapPairPlacementStep::Chest &&
        step != MapPairPlacementStep::Anvil) {
        return MapPairPlacementVerification::ConflictingWorldBlock;
    }
    const MapPairPlacementVerification chest_result =
        verifyBlockName(chest, "minecraft:chest");
    if (chest_result != MapPairPlacementVerification::Confirmed ||
        step == MapPairPlacementStep::Chest) return chest_result;
    return verifyBlockName(anvil, "minecraft:anvil");
}

MapPairPlacementVerification VerifyMapPairPlacementAfterAckWithReader(
        const MapChestAnvilPosition& pair, MapPairPlacementStep step,
        MapPairCommandAck ack, NativeWorldReader* reader) {
    if (ack != MapPairCommandAck::Accepted) {
        return VerifyMapPairPlacementAfterAck(step, ack, nullptr, nullptr);
    }
    if (!reader) {
        return MapPairPlacementVerification::PendingNativeReadback;
    }
    NativeBlockInfo chest;
    NativeBlockInfo anvil;
    const int32_t y = step == MapPairPlacementStep::Support
        ? pair.chest.y - 1 : pair.chest.y;
    const NativeBlockInfo* chest_read =
        reader->getBlock(pair.chest.x, y, pair.chest.z,
                         &chest) ? &chest : nullptr;
    const NativeBlockInfo* anvil_read =
        (step == MapPairPlacementStep::Anvil ||
         step == MapPairPlacementStep::Support) &&
        reader->getBlock(pair.anvil.x,
                         step == MapPairPlacementStep::Support
                             ? pair.anvil.y - 1 : pair.anvil.y,
                         pair.anvil.z,
                         &anvil) ? &anvil : nullptr;
    return VerifyMapPairPlacementAfterAck(step, ack, chest_read, anvil_read);
}

std::string BuildSingleCellMapChestFillCommand(
        const MapChestPosition& position) {
    return fillCommand(position, position, "minecraft:chest");
}

bool BuildMapPairSupportPlatformCells(
        const BlockBounds& artwork_bounds, const MapChestAnvilPosition& pair,
        uint8_t platform_side, std::array<MapChestPosition, 4>* cells) {
    if (!cells || platform_side < 1U || platform_side > 2U ||
        pair.chest.y != pair.anvil.y ||
        pair.chest.y == std::numeric_limits<int32_t>::min() ||
        pair.chest.y > std::numeric_limits<int32_t>::max() - 1) return false;
    const int64_t dx = static_cast<int64_t>(pair.anvil.x) - pair.chest.x;
    const int64_t dz = static_cast<int64_t>(pair.anvil.z) - pair.chest.z;
    if (std::abs(dx) + std::abs(dz) != 1) return false;
    const int64_t side = platform_side == 1U ? 1 : -1;
    const int64_t pad_dx = dz == 0 ? 0 : side;
    const int64_t pad_dz = dx == 0 ? 0 : side;
    const auto valid = [](int64_t value) {
        return value > std::numeric_limits<int32_t>::min() &&
               value < std::numeric_limits<int32_t>::max();
    };
    if (!valid(static_cast<int64_t>(pair.chest.x) + pad_dx) ||
        !valid(static_cast<int64_t>(pair.chest.z) + pad_dz) ||
        !valid(static_cast<int64_t>(pair.anvil.x) + pad_dx) ||
        !valid(static_cast<int64_t>(pair.anvil.z) + pad_dz)) return false;
    const int32_t y = pair.chest.y - 1;
    const std::array<MapChestPosition, 4> candidate{{
        {pair.chest.x, y, pair.chest.z},
        {pair.anvil.x, y, pair.anvil.z},
        {static_cast<int32_t>(pair.chest.x + pad_dx), y,
         static_cast<int32_t>(pair.chest.z + pad_dz)},
        {static_cast<int32_t>(pair.anvil.x + pad_dx), y,
         static_cast<int32_t>(pair.anvil.z + pad_dz)},
    }};
    for (const auto& cell : candidate) {
        if (!IsOutsideCompleteMapFootprint(artwork_bounds, cell)) return false;
    }
    *cells = candidate;
    return true;
}

bool BuildMapExtraSupportPlatformCells(
        const BlockBounds& artwork_bounds, const MapChestPosition& chest,
        uint8_t platform_corner, std::array<MapChestPosition, 4>* cells) {
    if (!cells || platform_corner < 1U || platform_corner > 4U ||
        chest.y == std::numeric_limits<int32_t>::min() ||
        chest.y > std::numeric_limits<int32_t>::max() - 1) return false;
    const int64_t dx = platform_corner <= 2U ? 1 : -1;
    const int64_t dz = (platform_corner & 1U) ? 1 : -1;
    const int64_t x = static_cast<int64_t>(chest.x) + dx;
    const int64_t z = static_cast<int64_t>(chest.z) + dz;
    if (x <= std::numeric_limits<int32_t>::min() ||
        x >= std::numeric_limits<int32_t>::max() ||
        z <= std::numeric_limits<int32_t>::min() ||
        z >= std::numeric_limits<int32_t>::max()) return false;
    const int32_t y = chest.y - 1;
    const std::array<MapChestPosition, 4> candidate{{
        {chest.x, y, chest.z},
        {static_cast<int32_t>(x), y, chest.z},
        {chest.x, y, static_cast<int32_t>(z)},
        {static_cast<int32_t>(x), y, static_cast<int32_t>(z)},
    }};
    for (const auto& cell : candidate) {
        if (!IsOutsideCompleteMapFootprint(artwork_bounds, cell)) return false;
    }
    *cells = candidate;
    return true;
}

bool MapPairPlatformStandingCell(
        const BlockBounds& artwork_bounds, const MapChestAnvilPosition& pair,
        uint8_t platform_side, MapChestPosition* floor) {
    std::array<MapChestPosition, 4> cells;
    if (!floor || !BuildMapPairSupportPlatformCells(
            artwork_bounds, pair, platform_side, &cells)) return false;
    *floor = cells[3];
    return true;
}

bool MapExtraPlatformStandingCell(
        const BlockBounds& artwork_bounds, const MapChestPosition& chest,
        uint8_t platform_corner, MapChestPosition* floor) {
    std::array<MapChestPosition, 4> cells;
    if (!floor || !BuildMapExtraSupportPlatformCells(
            artwork_bounds, chest, platform_corner, &cells)) return false;
    *floor = cells[3];
    return true;
}

bool FreshMapSupportPlatformAirWithReader(
        NativeWorldReader* reader,
        const std::array<MapChestPosition, 4>& cells) {
    if (!reader) return false;
    for (const auto& cell : cells) {
        NativeBlockInfo floor;
        NativeBlockInfo feet;
        NativeBlockInfo head;
        if (!reader->getBlock(cell.x, cell.y, cell.z, &floor) ||
            !reader->getBlock(cell.x, cell.y + 1, cell.z, &feet) ||
            !reader->getBlock(cell.x, cell.y + 2, cell.z, &head) ||
            !isAirName(floor.name) || !isAirName(feet.name) ||
            !isAirName(head.name)) return false;
    }
    return true;
}

bool ExactMapSupportPlatformStoneWithReader(
        NativeWorldReader* reader,
        const std::array<MapChestPosition, 4>& cells) {
    if (!reader) return false;
    for (const auto& cell : cells) {
        NativeBlockInfo block;
        if (!reader->getBlock(cell.x, cell.y, cell.z, &block) ||
            block.name != "minecraft:stone") return false;
    }
    return true;
}

bool SelectMapPairSupportPlatformSideWithReader(
        const BlockBounds& artwork_bounds, const MapChestAnvilPosition& pair,
        NativeWorldReader* reader, uint8_t* platform_side) {
    if (!reader || !platform_side) return false;
    for (uint8_t side = 1U; side <= 2U; ++side) {
        std::array<MapChestPosition, 4> cells;
        if (BuildMapPairSupportPlatformCells(
                artwork_bounds, pair, side, &cells) &&
            FreshMapSupportPlatformAirWithReader(reader, cells)) {
            *platform_side = side;
            return true;
        }
    }
    return false;
}

bool SelectMapExtraSupportPlatformCornerWithReader(
        const BlockBounds& artwork_bounds, const MapChestPosition& chest,
        NativeWorldReader* reader, uint8_t* platform_corner) {
    if (!reader || !platform_corner) return false;
    for (uint8_t corner = 1U; corner <= 4U; ++corner) {
        std::array<MapChestPosition, 4> cells;
        if (BuildMapExtraSupportPlatformCells(
                artwork_bounds, chest, corner, &cells) &&
            FreshMapSupportPlatformAirWithReader(reader, cells)) {
            *platform_corner = corner;
            return true;
        }
    }
    return false;
}

std::string BuildMapSupportPlatformFillCommand(
        const std::array<MapChestPosition, 4>& cells) {
    MapChestPosition first = cells[0];
    MapChestPosition last = cells[0];
    for (const auto& cell : cells) {
        first.x = std::min(first.x, cell.x);
        first.z = std::min(first.z, cell.z);
        last.x = std::max(last.x, cell.x);
        last.z = std::max(last.z, cell.z);
    }
    return fillCommand(first, last, "minecraft:stone");
}

}  // namespace build_import
