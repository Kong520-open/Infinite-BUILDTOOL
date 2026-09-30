#include "../MapChestPlacement.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

using namespace build_import;

namespace {

MapChestCandidateSurvey safeSurvey(MapChestPosition position) {
    MapChestCandidateSurvey survey;
    survey.position = position;
    survey.target = MapChestCell::Air;
    survey.above = MapChestCell::Air;
    survey.below = MapChestCell::SolidSupport;
    survey.north = MapChestCell::Air;
    survey.south = MapChestCell::Other;
    survey.west = MapChestCell::SolidSupport;
    survey.east = MapChestCell::Air;
    return survey;
}

void testCompleteMapFootprintAndPriority() {
    // X covers three map tiles, Z covers two. The source art is narrower than
    // the complete map footprint, so checking only artwork bounds is unsafe.
    const BlockBounds artwork{0, 64, 0, 300, 64, 130};
    const std::vector<MapChestPosition> candidates =
        EnumerateMapChestCandidates(artwork);
    assert(!candidates.empty());
    assert((candidates.front() == MapChestPosition{321, 65, 65}));
    // The former tile-center position must stay legal for old Armed journals.
    bool legacy_candidate_retained = false;
    for (const auto& candidate : candidates) {
        legacy_candidate_retained |= candidate == MapChestPosition{-66, 65, 0};
    }
    assert(legacy_candidate_retained);
    for (const MapChestPosition& position : candidates) {
        assert(position.x < -64 || position.x > 319 ||
               position.z < -64 || position.z > 191);
    }

    MapChestPosition chosen{111, 222, 333};
    assert(ChooseMapChestPlacement(
        artwork, {safeSurvey(candidates.front())}, &chosen));
    assert(chosen == candidates.front());

    auto blocked = safeSurvey(candidates[0]);
    blocked.target = MapChestCell::Other;
    chosen = {111, 222, 333};
    assert(ChooseMapChestPlacement(
        artwork, {blocked, safeSurvey(candidates[1])}, &chosen));
    assert(chosen == candidates[1]);
}

void testNoUnsafeCellCanBeAccepted() {
    const BlockBounds artwork{0, 64, 0, 0, 64, 0};
    const MapChestPosition candidate =
        EnumerateMapChestCandidates(artwork).front();
    const MapChestPosition sentinel{11, 22, 33};
    MapChestPosition chosen = sentinel;

    auto survey = safeSurvey(candidate);
    survey.target = MapChestCell::Unknown;
    assert(!ChooseMapChestPlacement(artwork, {survey}, &chosen));
    assert(chosen == sentinel);

    survey = safeSurvey(candidate);
    survey.above = MapChestCell::Other;
    assert(!ChooseMapChestPlacement(artwork, {survey}, &chosen));
    assert(chosen == sentinel);

    survey = safeSurvey(candidate);
    survey.below = MapChestCell::Air;
    assert(!ChooseMapChestPlacement(artwork, {survey}, &chosen));
    assert(chosen == sentinel);

    survey = safeSurvey(candidate);
    survey.below = MapChestCell::Chest;
    assert(!ChooseMapChestPlacement(artwork, {survey}, &chosen));
    assert(chosen == sentinel);

    for (int side = 0; side < 4; ++side) {
        survey = safeSurvey(candidate);
        MapChestCell* neighbor = side == 0 ? &survey.north :
            side == 1 ? &survey.south :
            side == 2 ? &survey.west : &survey.east;
        *neighbor = MapChestCell::Chest;
        assert(!ChooseMapChestPlacement(artwork, {survey}, &chosen));
        assert(chosen == sentinel);
        *neighbor = MapChestCell::Unknown;
        assert(!ChooseMapChestPlacement(artwork, {survey}, &chosen));
        assert(chosen == sentinel);
    }
}

void testSurveyMustFollowCandidateOrder() {
    const BlockBounds artwork{0, 64, 0, 0, 64, 0};
    const std::vector<MapChestPosition> candidates =
        EnumerateMapChestCandidates(artwork);
    MapChestPosition chosen{11, 22, 33};
    assert(!ChooseMapChestPlacement(artwork,
                                    {safeSurvey(candidates[1])}, &chosen));
    assert((chosen == MapChestPosition{11, 22, 33}));

    std::vector<MapChestCandidateSurvey> all_blocked;
    all_blocked.reserve(candidates.size());
    for (const MapChestPosition& candidate : candidates) {
        auto survey = safeSurvey(candidate);
        survey.target = MapChestCell::Other;
        all_blocked.push_back(survey);
    }
    assert(!ChooseMapChestPlacement(artwork, all_blocked, &chosen));
    assert((chosen == MapChestPosition{11, 22, 33}));
}

void testCoordinateBoundsAndInvalidInput() {
    const int32_t min = std::numeric_limits<int32_t>::min();
    const int32_t max = std::numeric_limits<int32_t>::max();
    assert(EnumerateMapChestCandidates(BlockBounds{}).empty());
    assert(EnumerateMapChestCandidates({0, max, 0, 0, max, 0}).empty());
    assert(EnumerateMapChestCandidates({max, 64, 0, max, 64, 0}).empty());

    const BlockBounds edge{min, 64, 0, min + 1, 64, 0};
    const std::vector<MapChestPosition> candidates =
        EnumerateMapChestCandidates(edge);
    assert(!candidates.empty());
    for (const MapChestPosition& candidate : candidates) {
        assert(static_cast<int64_t>(candidate.x) - 1 >= min);
        assert(static_cast<int64_t>(candidate.x) + 1 <= max);
        assert(static_cast<int64_t>(candidate.y) - 1 >= min);
        assert(static_cast<int64_t>(candidate.y) + 1 <= max);
        assert(static_cast<int64_t>(candidate.z) - 1 >= min);
        assert(static_cast<int64_t>(candidate.z) + 1 <= max);
        assert(static_cast<int64_t>(candidate.x) < static_cast<int64_t>(min) - 64 ||
               static_cast<int64_t>(candidate.x) > static_cast<int64_t>(min) + 63 ||
               candidate.z < -64 || candidate.z > 63);
    }
    MapChestPosition chosen{};
    assert(ChooseMapChestPlacement(edge, {safeSurvey(candidates.front())}, &chosen));
    assert(chosen == candidates.front());
    assert(!ChooseMapChestPlacement(edge,
                                    {safeSurvey(candidates.front())}, nullptr));
}

void testAdjacentAnvilPair() {
    const BlockBounds artwork{0, 64, 0, 300, 64, 130};
    const std::vector<MapChestAnvilPosition> candidates =
        EnumerateMapChestAnvilCandidates(artwork);
    assert(!candidates.empty());
    assert((candidates.front().chest == MapChestPosition{321, 65, 65}));
    assert((candidates.front().anvil == MapChestPosition{322, 65, 65}));
    for (const MapChestAnvilPosition& pair : candidates) {
        const auto outside = [](const MapChestPosition& site) {
            return site.x < -64 || site.x > 319 ||
                   site.z < -64 || site.z > 191;
        };
        assert(outside(pair.chest) && outside(pair.anvil));
        const int64_t dx = static_cast<int64_t>(pair.chest.x) - pair.anvil.x;
        const int64_t dz = static_cast<int64_t>(pair.chest.z) - pair.anvil.z;
        assert(pair.chest.y == pair.anvil.y);
        assert((dx == 0 && (dz == -1 || dz == 1)) ||
               (dz == 0 && (dx == -1 || dx == 1)));
    }

    const auto survey = [](const MapChestAnvilPosition& pair) {
        return MapChestAnvilCandidateSurvey{
            safeSurvey(pair.chest), safeSurvey(pair.anvil)};
    };
    MapChestAnvilPosition chosen{{11, 22, 33}, {44, 55, 66}};
    assert(ChooseMapChestAnvilPlacement(
        artwork, {survey(candidates.front())}, &chosen));
    assert(chosen == candidates.front());

    auto blocked = survey(candidates[0]);
    blocked.anvil.above = MapChestCell::Other;
    chosen = {{11, 22, 33}, {44, 55, 66}};
    assert(!ChooseMapChestAnvilPlacement(artwork, {blocked}, &chosen));
    assert((chosen == MapChestAnvilPosition{{11, 22, 33}, {44, 55, 66}}));
    assert(ChooseMapChestAnvilPlacement(
        artwork, {blocked, survey(candidates[1])}, &chosen));
    assert(chosen == candidates[1]);
    assert(!ChooseMapChestAnvilPlacement(
        artwork, {survey(candidates[1])}, &chosen));

    const int32_t min = std::numeric_limits<int32_t>::min();
    const std::vector<MapChestAnvilPosition> edge_candidates =
        EnumerateMapChestAnvilCandidates({min, 64, 0, min + 1, 64, 0});
    assert(!edge_candidates.empty());
    for (const MapChestAnvilPosition& pair : edge_candidates) {
        assert(static_cast<int64_t>(pair.anvil.x) - 1 >= min);
        assert(static_cast<int64_t>(pair.anvil.x) + 1 <=
               std::numeric_limits<int32_t>::max());
    }
}

void testArtworkAlignedOutsideMapFootprint() {
    // This artwork occupies one corner of a 128x128 map. The first chest is
    // now two blocks west of its edge and aligned with the art, rather than
    // 15 blocks beyond its Z edge at the map-tile center.
    const BlockBounds artwork{-704, -16, -576, -655, -16, -527};
    const auto pairs = EnumerateMapChestAnvilCandidates(artwork);
    assert(!pairs.empty());
    assert((pairs.front().chest == MapChestPosition{-706, -15, -552}));
    assert((pairs.front().anvil == MapChestPosition{-705, -15, -552}));
    bool previous_site_retained = false;
    for (const auto& pair : pairs) {
        previous_site_retained |=
            pair.chest == MapChestPosition{-706, -15, -512} &&
            pair.anvil == MapChestPosition{-705, -15, -512};
    }
    assert(previous_site_retained);
}

void testAutoFillPairNeedsTwoKnownAirFloorCells() {
    const BlockBounds artwork{0, 64, 0, 0, 64, 0};
    const auto candidates = EnumerateMapChestAnvilCandidates(artwork);
    assert(!candidates.empty());
    const auto first = candidates.front();
    auto survey = MapChestAnvilCandidateSurvey{
        safeSurvey(first.chest), safeSurvey(first.anvil)};
    survey.chest.below = MapChestCell::Air;
    survey.anvil.below = MapChestCell::Air;
    MapChestAnvilPosition chosen{{11, 22, 33}, {44, 55, 66}};
    assert(ChooseMapChestAnvilPlacementForAutoFill(
        artwork, {survey}, &chosen));
    assert(chosen == first);
    assert(!ChooseMapChestAnvilPlacement(artwork, {survey}, &chosen));

    chosen = {{11, 22, 33}, {44, 55, 66}};
    auto blocked = survey;
    blocked.chest.below = MapChestCell::SolidSupport;
    assert(!ChooseMapChestAnvilPlacementForAutoFill(
        artwork, {blocked}, &chosen));
    assert((chosen == MapChestAnvilPosition{{11, 22, 33}, {44, 55, 66}}));
    blocked = survey;
    blocked.anvil.below = MapChestCell::Unknown;
    assert(!ChooseMapChestAnvilPlacementForAutoFill(
        artwork, {blocked}, &chosen));
    blocked = survey;
    blocked.anvil.above = MapChestCell::Other;
    assert(!ChooseMapChestAnvilPlacementForAutoFill(
        artwork, {blocked}, &chosen));
    blocked = survey;
    blocked.chest.north = MapChestCell::Chest;
    assert(!ChooseMapChestAnvilPlacementForAutoFill(
        artwork, {blocked}, &chosen));
    assert(!ChooseMapChestAnvilPlacementForAutoFill(
        artwork, {}, &chosen));
    assert(!ChooseMapChestAnvilPlacementForAutoFill(
        artwork, {survey}, nullptr));
}

}  // namespace

int main() {
    testCompleteMapFootprintAndPriority();
    testNoUnsafeCellCanBeAccepted();
    testSurveyMustFollowCandidateOrder();
    testCoordinateBoundsAndInvalidInput();
    testAdjacentAnvilPair();
    testArtworkAlignedOutsideMapFootprint();
    testAutoFillPairNeedsTwoKnownAirFloorCells();
    return 0;
}
