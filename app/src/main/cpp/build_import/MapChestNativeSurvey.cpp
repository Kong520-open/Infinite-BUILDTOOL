#include "MapChestNativeSurvey.h"

#include <array>
#include <limits>
#include <string_view>

namespace build_import {
namespace {

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

bool validIdentifierPart(std::string_view part) {
    if (part.empty()) return false;
    for (const char character : part) {
        if ((character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') ||
            character == '_' || character == '-' || character == '.' ||
            character == '/') {
            continue;
        }
        return false;
    }
    return true;
}

bool isKnownFullCube(std::string_view leaf) {
    static constexpr std::array<std::string_view, 38> kExact{{
        "bedrock", "stone", "cobblestone", "mossy_cobblestone",
        "deepslate", "cobbled_deepslate", "grass_block", "dirt",
        "coarse_dirt", "rooted_dirt", "podzol", "mycelium",
        "sand", "red_sand", "sandstone", "red_sandstone",
        "gravel", "clay", "netherrack", "end_stone",
        "obsidian", "crying_obsidian", "blackstone", "basalt",
        "polished_basalt", "calcite", "tuff", "dripstone_block",
        "mud", "packed_mud", "bricks", "brick_block",
        "stonebrick", "stone_bricks", "planks", "wood",
        "log", "log2",
    }};
    for (const std::string_view exact : kExact) {
        if (leaf == exact) return true;
    }
    static constexpr std::array<std::string_view, 7> kFullCubeSuffixes{{
        "_planks", "_concrete", "_wool", "_terracotta",
        "_bricks", "_log", "_wood",
    }};
    for (const std::string_view suffix : kFullCubeSuffixes) {
        if (endsWith(leaf, suffix)) return true;
    }
    return false;
}

bool isKnownNonSupport(std::string_view leaf) {
    static constexpr std::array<std::string_view, 16> kExact{{
        "water", "flowing_water", "lava", "flowing_lava",
        "fire", "soul_fire", "snow_layer", "snow",
        "tallgrass", "tall_grass", "short_grass", "fern",
        "torch", "soul_torch", "vine", "scaffolding",
    }};
    for (const std::string_view exact : kExact) {
        if (leaf == exact) return true;
    }
    return endsWith(leaf, "_stairs") || endsWith(leaf, "_slab") ||
           endsWith(leaf, "_fence") || endsWith(leaf, "_wall") ||
           endsWith(leaf, "_trapdoor") || endsWith(leaf, "_door");
}

template <typename ReadBlock>
MapChestCandidateSurvey surveyCandidate(const MapChestPosition& position,
                                        ReadBlock&& read_block) {
    MapChestCandidateSurvey survey;
    survey.position = position;
    const auto minimum = std::numeric_limits<int32_t>::min();
    const auto maximum = std::numeric_limits<int32_t>::max();
    if (position.x <= minimum || position.x >= maximum ||
        position.y <= minimum || position.y >= maximum ||
        position.z <= minimum || position.z >= maximum) {
        return survey;
    }
    const auto read_cell = [&](int32_t x, int32_t y, int32_t z) {
        NativeBlockInfo block;
        return read_block(x, y, z, &block)
            ? ClassifyMapChestNativeBlock(block) : MapChestCell::Unknown;
    };
    survey.target = read_cell(position.x, position.y, position.z);
    survey.above = read_cell(position.x, position.y + 1, position.z);
    survey.below = read_cell(position.x, position.y - 1, position.z);
    survey.north = read_cell(position.x, position.y, position.z - 1);
    survey.south = read_cell(position.x, position.y, position.z + 1);
    survey.west = read_cell(position.x - 1, position.y, position.z);
    survey.east = read_cell(position.x + 1, position.y, position.z);
    return survey;
}

}  // namespace

MapChestCell ClassifyMapChestNativeBlock(const NativeBlockInfo& block) {
    std::string_view name(block.name);
    if (name.empty()) return MapChestCell::Unknown;
    std::string_view name_space("minecraft");
    const size_t colon = name.find(':');
    if (colon != std::string_view::npos) {
        name_space = name.substr(0, colon);
        name.remove_prefix(colon + 1);
        if (name.find(':') != std::string_view::npos) return MapChestCell::Unknown;
    }
    if (!validIdentifierPart(name_space) || !validIdentifierPart(name)) {
        return MapChestCell::Unknown;
    }
    // Prefer a conservative chest match even in a custom namespace or a
    // future vanilla variant; adjacency must never create a double chest.
    if (name.find("chest") != std::string_view::npos) return MapChestCell::Chest;
    if (name_space != "minecraft") return MapChestCell::Unknown;
    if (name == "air" || name == "cave_air" || name == "void_air") {
        return MapChestCell::Air;
    }
    if (isKnownFullCube(name)) return MapChestCell::SolidSupport;
    if (isKnownNonSupport(name)) return MapChestCell::Other;
    return MapChestCell::Unknown;
}

bool IsMapStorageNativeReadPlaceholder(const NativeBlockInfo& block) {
    return block.name == "minecraft:client_request_placeholder_block";
}

bool ShouldRetryMapStorageStandingSurvey(MapChestCell floor,
                                         MapChestCell feet,
                                         MapChestCell head) {
    return floor == MapChestCell::Air &&
           feet == MapChestCell::Air &&
           head == MapChestCell::Air;
}

MapChestCandidateSurvey SurveyMapChestCandidateFromNativeWorld(
        const MapChestPosition& position) {
    return surveyCandidate(position, [](int32_t x, int32_t y, int32_t z,
                                        NativeBlockInfo* output) {
        return NativeWorldAccess::getBlock(x, y, z, output);
    });
}

MapChestCandidateSurvey SurveyMapChestCandidateWithReader(
        NativeWorldReader* reader, const MapChestPosition& position) {
    return surveyCandidate(position, [reader](int32_t x, int32_t y, int32_t z,
                                              NativeBlockInfo* output) {
        return reader && reader->getBlock(x, y, z, output);
    });
}

}  // namespace build_import
