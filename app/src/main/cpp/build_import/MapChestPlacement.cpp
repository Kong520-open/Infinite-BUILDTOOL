#include "MapChestPlacement.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace build_import {
namespace {

constexpr int64_t kMapSpan = 128;
constexpr int64_t kMapHalfSpan = kMapSpan / 2;
constexpr std::array<int64_t, 3> kExteriorDepths{{2, 3, 4}};
constexpr std::array<int64_t, 17> kLateralOffsets{{
    0, -1, 1, -2, 2, -3, 3, -4, 4, -5, 5, -6, 6, -7, 7, -8, 8,
}};
constexpr std::array<int64_t, 17> kVerticalOffsets{{
    0, -1, 1, -2, 2, -3, 3, -4, 4, -5, 5, -6, 6, -7, 7, -8, 8,
}};

int64_t floorDiv128(int64_t value) {
    int64_t quotient = value / kMapSpan;
    if (value % kMapSpan < 0) --quotient;
    return quotient;
}

int64_t mapCenter(int32_t coordinate) {
    return floorDiv128(static_cast<int64_t>(coordinate) + kMapHalfSpan) * kMapSpan;
}

bool inInt32(int64_t value) {
    return value >= std::numeric_limits<int32_t>::min() &&
           value <= std::numeric_limits<int32_t>::max();
}

// The survey includes all six neighbors, so no candidate may sit at an
// int32 boundary even if its own position would be representable.
bool neighborsInInt32(int64_t value) {
    return value > std::numeric_limits<int32_t>::min() &&
           value < std::numeric_limits<int32_t>::max();
}

void appendColumn(std::vector<MapChestPosition>* candidates,
                  int64_t x, int64_t z, int64_t preferred_y) {
    if (!neighborsInInt32(x) || !neighborsInInt32(z)) return;
    for (const int64_t offset : kVerticalOffsets) {
        const int64_t y = preferred_y + offset;
        if (!neighborsInInt32(y)) continue;
        candidates->push_back(MapChestPosition{
            static_cast<int32_t>(x), static_cast<int32_t>(y),
            static_cast<int32_t>(z)});
    }
}

bool safeSurveyWithFloor(const MapChestCandidateSurvey& survey,
                         MapChestCell required_floor) {
    if (survey.target != MapChestCell::Air ||
        survey.above != MapChestCell::Air ||
        survey.below != required_floor) {
        return false;
    }
    const std::array<MapChestCell, 4> neighbors{{
        survey.north, survey.south, survey.west, survey.east,
    }};
    for (const MapChestCell cell : neighbors) {
        if (cell == MapChestCell::Unknown || cell == MapChestCell::Chest) return false;
    }
    return true;
}

bool safeSurvey(const MapChestCandidateSurvey& survey) {
    return safeSurveyWithFloor(survey, MapChestCell::SolidSupport);
}

bool outsideCompleteMapFootprint(const BlockBounds& bounds,
                                 int64_t x, int64_t z) {
    if (!bounds.isValid()) return false;
    const int64_t first_x = mapCenter(bounds.min_x);
    const int64_t last_x = mapCenter(bounds.max_x);
    const int64_t first_z = mapCenter(bounds.min_z);
    const int64_t last_z = mapCenter(bounds.max_z);
    if (!inInt32(first_x) || !inInt32(last_x) ||
        !inInt32(first_z) || !inInt32(last_z)) return false;
    return x < first_x - kMapHalfSpan ||
           x > last_x + kMapHalfSpan - 1 ||
           z < first_z - kMapHalfSpan ||
           z > last_z + kMapHalfSpan - 1;
}

}  // namespace

std::vector<MapChestPosition> EnumerateMapChestCandidates(
        const BlockBounds& artwork_bounds) {
    std::vector<MapChestPosition> candidates;
    if (!artwork_bounds.isValid() ||
        artwork_bounds.max_y == std::numeric_limits<int32_t>::max()) {
        return candidates;
    }

    const int64_t first_x = mapCenter(artwork_bounds.min_x);
    const int64_t last_x = mapCenter(artwork_bounds.max_x);
    const int64_t first_z = mapCenter(artwork_bounds.min_z);
    const int64_t last_z = mapCenter(artwork_bounds.max_z);
    // Match the map-creation planner: an unrepresentable tile center is not
    // silently clamped to a different map. The footprint itself may extend
    // beyond int32 at an edge; individual candidates are checked below.
    if (!inInt32(first_x) || !inInt32(last_x) ||
        !inInt32(first_z) || !inInt32(last_z)) {
        return candidates;
    }

    const int64_t west_edge = first_x - kMapHalfSpan;
    const int64_t east_edge = last_x + kMapHalfSpan - 1;
    const int64_t north_edge = first_z - kMapHalfSpan;
    const int64_t south_edge = last_z + kMapHalfSpan - 1;
    const int64_t art_mid_x = static_cast<int64_t>(artwork_bounds.min_x) +
        (static_cast<int64_t>(artwork_bounds.max_x) - artwork_bounds.min_x) / 2;
    const int64_t art_mid_z = static_cast<int64_t>(artwork_bounds.min_z) +
        (static_cast<int64_t>(artwork_bounds.max_z) - artwork_bounds.min_z) / 2;
    const int64_t preferred_y = static_cast<int64_t>(artwork_bounds.max_y) + 1;
    candidates.reserve(kExteriorDepths.size() * 8U *
                       kLateralOffsets.size() * kVerticalOffsets.size());

    // Side priority follows the artwork, not a hard-coded west/north/east/
    // south order. All four gaps grow by the same exterior depth, so sorting
    // once is enough; the side index preserves the old order for ties.
    std::array<std::pair<int64_t, uint8_t>, 4> sides{{
        {static_cast<int64_t>(artwork_bounds.min_x) - west_edge, 0},
        {static_cast<int64_t>(artwork_bounds.min_z) - north_edge, 1},
        {east_edge - static_cast<int64_t>(artwork_bounds.max_x), 2},
        {south_edge - static_cast<int64_t>(artwork_bounds.max_z), 3},
    }};
    std::sort(sides.begin(), sides.end());

    // Prefer the artwork's own midpoint along each exterior edge. The old
    // tile-center columns remain below as fallbacks and, more importantly,
    // keep already-persisted placement journals valid after an upgrade.
    // Every candidate still lies outside the complete selected map footprint
    // so the storage blocks cannot be drawn onto the finished map.
    for (const int64_t depth : kExteriorDepths) {
        for (const int64_t offset : kLateralOffsets) {
            for (const auto& side : sides) {
                switch (side.second) {
                    case 0: appendColumn(&candidates, west_edge - depth,
                                         art_mid_z + offset, preferred_y); break;
                    case 1: appendColumn(&candidates, art_mid_x + offset,
                                         north_edge - depth, preferred_y); break;
                    case 2: appendColumn(&candidates, east_edge + depth,
                                         art_mid_z + offset, preferred_y); break;
                    case 3: appendColumn(&candidates, art_mid_x + offset,
                                         south_edge + depth, preferred_y); break;
                    default: break;
                }
            }
        }
    }
    // Legacy tile-center search order is retained as an exact suffix: a
    // paused Armed journal must resume at its original position, not reselect.
    for (const int64_t depth : kExteriorDepths) {
        for (const int64_t offset : kLateralOffsets) {
            appendColumn(&candidates, west_edge - depth,
                         first_z + offset, preferred_y);
            appendColumn(&candidates, first_x + offset,
                         north_edge - depth, preferred_y);
            appendColumn(&candidates, east_edge + depth,
                         first_z + offset, preferred_y);
            appendColumn(&candidates, first_x + offset,
                         south_edge + depth, preferred_y);
        }
    }
    return candidates;
}

bool ChooseMapChestPlacement(const BlockBounds& artwork_bounds,
                             const std::vector<MapChestCandidateSurvey>& surveys,
                             MapChestPosition* placement) {
    if (!placement || surveys.empty()) return false;
    const std::vector<MapChestPosition> candidates =
        EnumerateMapChestCandidates(artwork_bounds);
    if (surveys.size() > candidates.size()) return false;
    // A complete prefix preserves deterministic priority and prevents a
    // caller from injecting an arbitrary, unplanned site into the selector.
    for (size_t index = 0; index < surveys.size(); ++index) {
        if (!(surveys[index].position == candidates[index])) return false;
    }
    for (const MapChestCandidateSurvey& survey : surveys) {
        if (safeSurvey(survey)) {
            *placement = survey.position;
            return true;
        }
    }
    return false;
}

std::vector<MapChestAnvilPosition> EnumerateMapChestAnvilCandidates(
        const BlockBounds& artwork_bounds) {
    std::vector<MapChestAnvilPosition> pairs;
    const std::vector<MapChestPosition> chest_candidates =
        EnumerateMapChestCandidates(artwork_bounds);
    pairs.reserve(chest_candidates.size() * 4U);
    static constexpr std::array<std::array<int64_t, 2>, 4> kAdjacentOffsets{{
        {{1, 0}}, {{0, 1}}, {{-1, 0}}, {{0, -1}},
    }};
    for (const MapChestPosition& chest : chest_candidates) {
        for (const auto& offset : kAdjacentOffsets) {
            const int64_t anvil_x = static_cast<int64_t>(chest.x) + offset[0];
            const int64_t anvil_z = static_cast<int64_t>(chest.z) + offset[1];
            if (!neighborsInInt32(anvil_x) || !neighborsInInt32(anvil_z) ||
                !outsideCompleteMapFootprint(
                    artwork_bounds, anvil_x, anvil_z)) continue;
            pairs.push_back(MapChestAnvilPosition{
                chest,
                MapChestPosition{static_cast<int32_t>(anvil_x), chest.y,
                                 static_cast<int32_t>(anvil_z)}});
        }
    }
    return pairs;
}

static bool chooseMapChestAnvilPlacementWithFloor(
        const BlockBounds& artwork_bounds,
        const std::vector<MapChestAnvilCandidateSurvey>& surveys,
        MapChestCell required_floor, MapChestAnvilPosition* placement) {
    if (!placement || surveys.empty()) return false;
    const std::vector<MapChestAnvilPosition> candidates =
        EnumerateMapChestAnvilCandidates(artwork_bounds);
    if (surveys.size() > candidates.size()) return false;
    for (size_t index = 0; index < surveys.size(); ++index) {
        if (!(surveys[index].chest.position == candidates[index].chest) ||
            !(surveys[index].anvil.position == candidates[index].anvil)) {
            return false;
        }
    }
    for (const MapChestAnvilCandidateSurvey& survey : surveys) {
        if (safeSurveyWithFloor(survey.chest, required_floor) &&
            safeSurveyWithFloor(survey.anvil, required_floor)) {
            *placement = MapChestAnvilPosition{
                survey.chest.position, survey.anvil.position};
            return true;
        }
    }
    return false;
}

bool ChooseMapChestAnvilPlacement(
        const BlockBounds& artwork_bounds,
        const std::vector<MapChestAnvilCandidateSurvey>& surveys,
        MapChestAnvilPosition* placement) {
    return chooseMapChestAnvilPlacementWithFloor(
        artwork_bounds, surveys, MapChestCell::SolidSupport, placement);
}

bool ChooseMapChestAnvilPlacementForAutoFill(
        const BlockBounds& artwork_bounds,
        const std::vector<MapChestAnvilCandidateSurvey>& surveys,
        MapChestAnvilPosition* placement) {
    return chooseMapChestAnvilPlacementWithFloor(
        artwork_bounds, surveys, MapChestCell::Air, placement);
}

bool IsOutsideCompleteMapFootprint(const BlockBounds& artwork_bounds,
                                   const MapChestPosition& position) {
    return outsideCompleteMapFootprint(artwork_bounds, position.x, position.z);
}

}  // namespace build_import
