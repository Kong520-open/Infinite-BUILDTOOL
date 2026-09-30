#include "../MapChestNativeSurvey.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <tuple>

using namespace build_import;

namespace {

using Key = std::tuple<int32_t, int32_t, int32_t>;
std::map<Key, std::string> g_native_blocks;

void put(int32_t x, int32_t y, int32_t z, const char* name) {
    g_native_blocks[Key{x, y, z}] = name;
}

void goodSite(const MapChestPosition& position) {
    g_native_blocks.clear();
    put(position.x, position.y, position.z, "minecraft:air");
    put(position.x, position.y + 1, position.z, "minecraft:air");
    put(position.x, position.y - 1, position.z, "minecraft:stone");
    put(position.x, position.y, position.z - 1, "minecraft:air");
    put(position.x, position.y, position.z + 1, "minecraft:water");
    put(position.x - 1, position.y, position.z, "minecraft:air");
    put(position.x + 1, position.y, position.z, "minecraft:air");
}

}  // namespace

namespace build_import {

bool NativeWorldAccess::getBlock(int32_t x, int32_t y, int32_t z,
                                 NativeBlockInfo* output) {
    if (!output) return false;
    const auto found = g_native_blocks.find(Key{x, y, z});
    if (found == g_native_blocks.end()) return false;
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
    const auto classify = [](const char* name) {
        NativeBlockInfo block;
        block.name = name;
        return ClassifyMapChestNativeBlock(block);
    };
    assert(classify("minecraft:air") == MapChestCell::Air);
    assert(classify("cave_air") == MapChestCell::Air);
    assert(classify("minecraft:grass_block") == MapChestCell::SolidSupport);
    assert(classify("minecraft:stone") == MapChestCell::SolidSupport);
    assert(classify("minecraft:spruce_planks") == MapChestCell::SolidSupport);
    assert(classify("minecraft:oak_stairs") == MapChestCell::Other);
    assert(classify("minecraft:water") == MapChestCell::Other);
    assert(classify("minecraft:chest") == MapChestCell::Chest);
    assert(classify("minecraft:trapped_chest") == MapChestCell::Chest);
    assert(classify("minecraft:ender_chest") == MapChestCell::Chest);
    assert(classify("minecraft:waxed_oxidized_copper_chest") == MapChestCell::Chest);
    assert(classify("addon:glowing_chest") == MapChestCell::Chest);
    assert(classify("addon:solid_block") == MapChestCell::Unknown);
    assert(classify("minecraft:future_block") == MapChestCell::Unknown);
    assert(classify("Minecraft:air") == MapChestCell::Unknown);
    assert(classify("") == MapChestCell::Unknown);

    const auto is_placeholder = [](const char* name) {
        NativeBlockInfo block;
        block.name = name;
        return IsMapStorageNativeReadPlaceholder(block);
    };
    assert(is_placeholder("minecraft:client_request_placeholder_block"));
    assert(!is_placeholder("minecraft:air"));
    assert(!is_placeholder("minecraft:stone"));
    assert(!is_placeholder("minecraft:future_block"));
    assert(!is_placeholder("addon:client_request_placeholder_block"));

    // The destination chunk can briefly report all air immediately after a
    // teleport, even though the previously filled platform remains in the
    // world. Do not mistake that specific snapshot for a changed standing cell.
    assert(ShouldRetryMapStorageStandingSurvey(
        classify("minecraft:air"), classify("cave_air"),
        classify("minecraft:void_air")));
    assert(!ShouldRetryMapStorageStandingSurvey(
        classify("minecraft:stone"), classify("minecraft:air"),
        classify("minecraft:air")));
    assert(!ShouldRetryMapStorageStandingSurvey(
        classify("minecraft:stone"), classify("minecraft:air"),
        classify("minecraft:stone")));

    const BlockBounds artwork{0, 64, 0, 0, 64, 0};
    const MapChestPosition position = EnumerateMapChestCandidates(artwork).front();
    goodSite(position);
    const MapChestCandidateSurvey ready =
        SurveyMapChestCandidateFromNativeWorld(position);
    assert(ready.position == position);
    assert(ready.target == MapChestCell::Air);
    assert(ready.above == MapChestCell::Air);
    assert(ready.below == MapChestCell::SolidSupport);
    assert(ready.south == MapChestCell::Other);
    MapChestPosition selected{};
    assert(ChooseMapChestPlacement(artwork, {ready}, &selected));
    assert(selected == position);

    NativeWorldReader reader;
    const MapChestCandidateSurvey reused =
        SurveyMapChestCandidateWithReader(&reader, position);
    assert(reused.target == ready.target && reused.below == ready.below);
    const MapChestCandidateSurvey missing_reader =
        SurveyMapChestCandidateWithReader(nullptr, position);
    assert(missing_reader.target == MapChestCell::Unknown);
    assert(!ChooseMapChestPlacement(artwork, {missing_reader}, &selected));

    g_native_blocks.erase(Key{position.x, position.y + 1, position.z});
    const MapChestCandidateSurvey unreadable =
        SurveyMapChestCandidateFromNativeWorld(position);
    assert(unreadable.above == MapChestCell::Unknown);
    assert(!ChooseMapChestPlacement(artwork, {unreadable}, &selected));

    goodSite(position);
    put(position.x + 1, position.y, position.z,
        "minecraft:waxed_exposed_copper_chest");
    const MapChestCandidateSurvey adjacent_chest =
        SurveyMapChestCandidateFromNativeWorld(position);
    assert(adjacent_chest.east == MapChestCell::Chest);
    assert(!ChooseMapChestPlacement(artwork, {adjacent_chest}, &selected));

    // An arbitrary caller-provided boundary position must not overflow when
    // the survey probes x +/- 1, y +/- 1, or z +/- 1.
    const MapChestPosition boundary{
        std::numeric_limits<int32_t>::max(), 70, 20};
    const MapChestCandidateSurvey invalid =
        SurveyMapChestCandidateFromNativeWorld(boundary);
    assert(invalid.target == MapChestCell::Unknown);
    assert(invalid.east == MapChestCell::Unknown);
    return 0;
}
