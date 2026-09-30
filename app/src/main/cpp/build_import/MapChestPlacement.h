#ifndef INFINITE_TEXTURE_MAP_CHEST_PLACEMENT_H
#define INFINITE_TEXTURE_MAP_CHEST_PLACEMENT_H

#include "BuildImportTypes.h"

#include <cstdint>
#include <vector>

namespace build_import {

struct MapChestPosition {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const MapChestPosition& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

// The caller must classify a frozen, loaded-world snapshot before handing it
// to the selector. Unknown is deliberately not treated as air or as proof that
// no neighboring chest exists. "Chest" includes ordinary, trapped, and any
// other chest variant that should not neighbor the new storage chest.
enum class MapChestCell : uint8_t {
    Unknown,
    Air,
    SolidSupport,
    Chest,
    Other,
};

struct MapChestCandidateSurvey {
    MapChestPosition position;
    MapChestCell target = MapChestCell::Unknown;
    MapChestCell above = MapChestCell::Unknown;
    MapChestCell below = MapChestCell::Unknown;
    MapChestCell north = MapChestCell::Unknown;
    MapChestCell south = MapChestCell::Unknown;
    MapChestCell west = MapChestCell::Unknown;
    MapChestCell east = MapChestCell::Unknown;
};

struct MapChestAnvilPosition {
    MapChestPosition chest;
    MapChestPosition anvil;

    bool operator==(const MapChestAnvilPosition& other) const {
        return chest == other.chest && anvil == other.anvil;
    }
};

struct MapChestAnvilCandidateSurvey {
    MapChestCandidateSurvey chest;
    MapChestCandidateSurvey anvil;
};

// Generates a bounded, deterministic search order outside the complete
// level-zero map-tile footprint (not merely outside the nontransparent art).
// The nearest exterior side and positions along it near the artwork midpoint
// are tried first;
// the earlier tile-center search order remains as a fallback so persisted
// placement journals from older builds keep their original coordinates.
// Every position and its six orthogonal neighbors fit in int32_t. No game or
// filesystem state is read here. An empty result means the bounds are invalid
// or there is no representable candidate in the search band.
std::vector<MapChestPosition> EnumerateMapChestCandidates(
    const BlockBounds& artwork_bounds);

// Surveys must form a prefix of EnumerateMapChestCandidates() in that exact
// order. The caller may stop surveying as soon as this returns true. This
// function never mutates the world, dispatches commands, or trusts an unknown
// cell. On failure, *placement remains unchanged.
bool ChooseMapChestPlacement(const BlockBounds& artwork_bounds,
                             const std::vector<MapChestCandidateSurvey>& surveys,
                             MapChestPosition* placement);

// Optional adjacent anvil for map renaming. Both blocks remain outside every
// selected map tile. The anvil is one cardinal block from the chest, with its
// own air/headroom/support survey. Neither block is placed by these helpers.
std::vector<MapChestAnvilPosition> EnumerateMapChestAnvilCandidates(
    const BlockBounds& artwork_bounds);
bool ChooseMapChestAnvilPlacement(
    const BlockBounds& artwork_bounds,
    const std::vector<MapChestAnvilCandidateSurvey>& surveys,
    MapChestAnvilPosition* placement);

// For a newly created automatic pair, both floor cells must be known air.
// A single tracked fill can then add a two-block stone floor before placing
// either container. The legacy selector above still accepts existing solid
// support for saved/preplaced pair plans.
bool ChooseMapChestAnvilPlacementForAutoFill(
    const BlockBounds& artwork_bounds,
    const std::vector<MapChestAnvilCandidateSurvey>& surveys,
    MapChestAnvilPosition* placement);

// A support platform must stay outside every map tile, not only outside the
// nontransparent artwork. This also validates coordinate representability.
bool IsOutsideCompleteMapFootprint(const BlockBounds& artwork_bounds,
                                   const MapChestPosition& position);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_CHEST_PLACEMENT_H
