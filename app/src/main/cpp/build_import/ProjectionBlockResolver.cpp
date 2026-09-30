#include "ProjectionBlockResolver.h"

#include <array>
#include <string>

namespace build_import {
namespace {

struct LegacyEntry {
    uint16_t id;
    const char* name;
};

// MCEdit stores only numeric IDs, so retaining source identity requires a
// local legacy registry. Unknown IDs are still emitted with a stable synthetic
// name instead of aborting the entire projection.
constexpr LegacyEntry kLegacyEntries[] = {
    {1, "minecraft:stone"}, {2, "minecraft:grass_block"},
    {3, "minecraft:dirt"}, {4, "minecraft:cobblestone"},
    {5, "minecraft:planks"}, {6, "minecraft:sapling"},
    {7, "minecraft:bedrock"}, {8, "minecraft:flowing_water"},
    {9, "minecraft:water"}, {10, "minecraft:flowing_lava"},
    {11, "minecraft:lava"}, {12, "minecraft:sand"},
    {13, "minecraft:gravel"}, {14, "minecraft:gold_ore"},
    {15, "minecraft:iron_ore"}, {16, "minecraft:coal_ore"},
    {17, "minecraft:log"}, {18, "minecraft:leaves"},
    {19, "minecraft:sponge"}, {20, "minecraft:glass"},
    {21, "minecraft:lapis_ore"}, {22, "minecraft:lapis_block"},
    {23, "minecraft:dispenser"}, {24, "minecraft:sandstone"},
    {25, "minecraft:noteblock"}, {26, "minecraft:bed"},
    {27, "minecraft:golden_rail"}, {28, "minecraft:detector_rail"},
    {29, "minecraft:sticky_piston"}, {30, "minecraft:web"},
    {31, "minecraft:tallgrass"}, {32, "minecraft:deadbush"},
    {33, "minecraft:piston"}, {34, "minecraft:piston_arm_collision"},
    {35, "minecraft:wool"}, {37, "minecraft:yellow_flower"},
    {38, "minecraft:red_flower"}, {39, "minecraft:brown_mushroom"},
    {40, "minecraft:red_mushroom"}, {41, "minecraft:gold_block"},
    {42, "minecraft:iron_block"}, {43, "minecraft:double_stone_slab"},
    {44, "minecraft:stone_slab"}, {45, "minecraft:brick_block"},
    {46, "minecraft:tnt"}, {47, "minecraft:bookshelf"},
    {48, "minecraft:mossy_cobblestone"}, {49, "minecraft:obsidian"},
    {50, "minecraft:torch"}, {51, "minecraft:fire"},
    {52, "minecraft:mob_spawner"}, {53, "minecraft:oak_stairs"},
    {54, "minecraft:chest"}, {55, "minecraft:redstone_wire"},
    {56, "minecraft:diamond_ore"}, {57, "minecraft:diamond_block"},
    {58, "minecraft:crafting_table"}, {59, "minecraft:wheat"},
    {60, "minecraft:farmland"}, {61, "minecraft:furnace"},
    {62, "minecraft:lit_furnace"}, {63, "minecraft:standing_sign"},
    {64, "minecraft:wooden_door"}, {65, "minecraft:ladder"},
    {66, "minecraft:rail"}, {67, "minecraft:stone_stairs"},
    {68, "minecraft:wall_sign"}, {69, "minecraft:lever"},
    {70, "minecraft:stone_pressure_plate"}, {71, "minecraft:iron_door"},
    {72, "minecraft:wooden_pressure_plate"}, {73, "minecraft:redstone_ore"},
    {74, "minecraft:lit_redstone_ore"}, {75, "minecraft:unlit_redstone_torch"},
    {76, "minecraft:redstone_torch"}, {77, "minecraft:stone_button"},
    {78, "minecraft:snow_layer"}, {79, "minecraft:ice"},
    {80, "minecraft:snow"}, {81, "minecraft:cactus"},
    {82, "minecraft:clay"}, {83, "minecraft:reeds"},
    {84, "minecraft:jukebox"}, {85, "minecraft:fence"},
    {86, "minecraft:pumpkin"}, {87, "minecraft:netherrack"},
    {88, "minecraft:soul_sand"}, {89, "minecraft:glowstone"},
    {90, "minecraft:portal"}, {91, "minecraft:lit_pumpkin"},
    {92, "minecraft:cake"}, {93, "minecraft:unpowered_repeater"},
    {94, "minecraft:powered_repeater"}, {95, "minecraft:stained_glass"},
    {96, "minecraft:trapdoor"}, {97, "minecraft:monster_egg"},
    {98, "minecraft:stonebrick"}, {99, "minecraft:brown_mushroom_block"},
    {100, "minecraft:red_mushroom_block"}, {101, "minecraft:iron_bars"},
    {102, "minecraft:glass_pane"}, {103, "minecraft:melon_block"},
    {104, "minecraft:pumpkin_stem"}, {105, "minecraft:melon_stem"},
    {106, "minecraft:vine"}, {107, "minecraft:fence_gate"},
    {108, "minecraft:brick_stairs"}, {109, "minecraft:stone_brick_stairs"},
    {110, "minecraft:mycelium"}, {111, "minecraft:waterlily"},
    {112, "minecraft:nether_brick"}, {113, "minecraft:nether_brick_fence"},
    {114, "minecraft:nether_brick_stairs"}, {115, "minecraft:nether_wart"},
    {116, "minecraft:enchanting_table"}, {117, "minecraft:brewing_stand"},
    {118, "minecraft:cauldron"}, {119, "minecraft:end_portal"},
    {120, "minecraft:end_portal_frame"}, {121, "minecraft:end_stone"},
    {122, "minecraft:dragon_egg"}, {123, "minecraft:redstone_lamp"},
    {124, "minecraft:lit_redstone_lamp"}, {125, "minecraft:double_wooden_slab"},
    {126, "minecraft:wooden_slab"}, {127, "minecraft:cocoa"},
    {128, "minecraft:sandstone_stairs"}, {129, "minecraft:emerald_ore"},
    {130, "minecraft:ender_chest"}, {131, "minecraft:tripwire_hook"},
    {132, "minecraft:tripwire"}, {133, "minecraft:emerald_block"},
    {134, "minecraft:spruce_stairs"}, {135, "minecraft:birch_stairs"},
    {136, "minecraft:jungle_stairs"}, {137, "minecraft:command_block"},
    {138, "minecraft:beacon"}, {139, "minecraft:cobblestone_wall"},
    {140, "minecraft:flower_pot"}, {141, "minecraft:carrots"},
    {142, "minecraft:potatoes"}, {143, "minecraft:wooden_button"},
    {144, "minecraft:skull"}, {145, "minecraft:anvil"},
    {146, "minecraft:trapped_chest"}, {147, "minecraft:light_weighted_pressure_plate"},
    {148, "minecraft:heavy_weighted_pressure_plate"},
    {149, "minecraft:unpowered_comparator"}, {150, "minecraft:powered_comparator"},
    {151, "minecraft:daylight_detector"}, {152, "minecraft:redstone_block"},
    {153, "minecraft:quartz_ore"}, {154, "minecraft:hopper"},
    {155, "minecraft:quartz_block"}, {156, "minecraft:quartz_stairs"},
    {157, "minecraft:activator_rail"}, {158, "minecraft:dropper"},
    {159, "minecraft:stained_hardened_clay"}, {160, "minecraft:stained_glass_pane"},
    {161, "minecraft:leaves2"}, {162, "minecraft:log2"},
    {163, "minecraft:acacia_stairs"}, {164, "minecraft:dark_oak_stairs"},
    {165, "minecraft:slime"}, {166, "minecraft:barrier"},
    {167, "minecraft:iron_trapdoor"}, {168, "minecraft:prismarine"},
    {169, "minecraft:sea_lantern"}, {170, "minecraft:hay_block"},
    {171, "minecraft:carpet"}, {172, "minecraft:hardened_clay"},
    {173, "minecraft:coal_block"}, {174, "minecraft:packed_ice"},
    {175, "minecraft:double_plant"}, {176, "minecraft:standing_banner"},
    {177, "minecraft:wall_banner"}, {178, "minecraft:daylight_detector_inverted"},
    {179, "minecraft:red_sandstone"}, {180, "minecraft:red_sandstone_stairs"},
    {181, "minecraft:double_stone_slab2"}, {182, "minecraft:stone_slab2"},
    {183, "minecraft:spruce_fence_gate"}, {184, "minecraft:birch_fence_gate"},
    {185, "minecraft:jungle_fence_gate"}, {186, "minecraft:dark_oak_fence_gate"},
    {187, "minecraft:acacia_fence_gate"}, {188, "minecraft:spruce_fence"},
    {189, "minecraft:birch_fence"}, {190, "minecraft:jungle_fence"},
    {191, "minecraft:dark_oak_fence"}, {192, "minecraft:acacia_fence"},
    {193, "minecraft:spruce_door"}, {194, "minecraft:birch_door"},
    {195, "minecraft:jungle_door"}, {196, "minecraft:acacia_door"},
    {197, "minecraft:dark_oak_door"}, {198, "minecraft:end_rod"},
    {199, "minecraft:chorus_plant"}, {200, "minecraft:chorus_flower"},
    {201, "minecraft:purpur_block"}, {202, "minecraft:purpur_pillar"},
    {203, "minecraft:purpur_stairs"}, {204, "minecraft:purpur_double_slab"},
    {205, "minecraft:purpur_slab"}, {206, "minecraft:end_bricks"},
    {207, "minecraft:beetroot"}, {208, "minecraft:grass_path"},
    {209, "minecraft:end_gateway"}, {210, "minecraft:repeating_command_block"},
    {211, "minecraft:chain_command_block"}, {212, "minecraft:frosted_ice"},
    {213, "minecraft:magma"}, {214, "minecraft:nether_wart_block"},
    {215, "minecraft:red_nether_brick"}, {216, "minecraft:bone_block"},
    {218, "minecraft:observer"}, {219, "minecraft:white_shulker_box"},
    {220, "minecraft:orange_shulker_box"}, {221, "minecraft:magenta_shulker_box"},
    {222, "minecraft:light_blue_shulker_box"}, {223, "minecraft:yellow_shulker_box"},
    {224, "minecraft:lime_shulker_box"}, {225, "minecraft:pink_shulker_box"},
    {226, "minecraft:gray_shulker_box"}, {227, "minecraft:silver_shulker_box"},
    {228, "minecraft:cyan_shulker_box"}, {229, "minecraft:purple_shulker_box"},
    {230, "minecraft:blue_shulker_box"}, {231, "minecraft:brown_shulker_box"},
    {232, "minecraft:green_shulker_box"}, {233, "minecraft:red_shulker_box"},
    {234, "minecraft:black_shulker_box"}, {235, "minecraft:white_glazed_terracotta"},
    {236, "minecraft:orange_glazed_terracotta"},
    {237, "minecraft:magenta_glazed_terracotta"},
    {238, "minecraft:light_blue_glazed_terracotta"},
    {239, "minecraft:yellow_glazed_terracotta"},
    {240, "minecraft:lime_glazed_terracotta"},
    {241, "minecraft:pink_glazed_terracotta"},
    {242, "minecraft:gray_glazed_terracotta"},
    {243, "minecraft:silver_glazed_terracotta"},
    {244, "minecraft:cyan_glazed_terracotta"},
    {245, "minecraft:purple_glazed_terracotta"},
    {246, "minecraft:blue_glazed_terracotta"},
    {247, "minecraft:brown_glazed_terracotta"},
    {248, "minecraft:green_glazed_terracotta"},
    {249, "minecraft:red_glazed_terracotta"},
    {250, "minecraft:black_glazed_terracotta"}, {251, "minecraft:concrete"},
    {252, "minecraft:concrete_powder"}, {255, "minecraft:structure_block"},
};

const char* legacyName(uint16_t id) {
    static const auto lookup = [] {
        std::array<const char*, 4096> names{};
        for (const LegacyEntry& entry : kLegacyEntries) names[entry.id] = entry.name;
        return names;
    }();
    return id < lookup.size() ? lookup[id] : nullptr;
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
        value.substr(value.size() - suffix.size()) == suffix;
}

std::string_view stateIdentifier(std::string_view state) {
    const size_t bracket = state.find('[');
    return state.substr(0, bracket);
}

std::string_view leafName(std::string_view identifier) {
    const size_t separator = identifier.rfind(':');
    return separator == std::string_view::npos ? identifier :
        identifier.substr(separator + 1);
}

std::string_view propertyValue(std::string_view state, std::string_view key) {
    const size_t bracket = state.find('[');
    if (bracket == std::string_view::npos || state.empty() || state.back() != ']') return {};
    std::string_view properties = state.substr(bracket + 1, state.size() - bracket - 2);
    while (!properties.empty()) {
        const size_t comma = properties.find(',');
        const std::string_view pair = properties.substr(0, comma);
        const size_t equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == key) {
            return pair.substr(equals + 1);
        }
        if (comma == std::string_view::npos) break;
        properties.remove_prefix(comma + 1);
    }
    return {};
}

int parseUnsigned(std::string_view value, int maximum) {
    if (value.empty()) return -1;
    int result = 0;
    for (char character : value) {
        if (character < '0' || character > '9') return -1;
        result = result * 10 + (character - '0');
        if (result > maximum) return -1;
    }
    return result;
}

int horizontalIndex(std::string_view facing) {
    if (facing == "south") return 0;
    if (facing == "west") return 1;
    if (facing == "north") return 2;
    if (facing == "east") return 3;
    return -1;
}

int stairIndex(std::string_view facing) {
    if (facing == "east") return 0;
    if (facing == "west") return 1;
    if (facing == "south") return 2;
    if (facing == "north") return 3;
    return -1;
}

int trapdoorIndex(std::string_view facing) {
    if (facing == "south") return 0;
    if (facing == "north") return 1;
    if (facing == "east") return 2;
    if (facing == "west") return 3;
    return -1;
}

uint8_t visualAux(std::string_view state, std::string_view leaf) {
    uint8_t aux = 0;
    const std::string_view facing = propertyValue(state, "facing");
    const std::string_view half = propertyValue(state, "half");

    if (leaf.find("stairs") != std::string_view::npos) {
        const int direction = stairIndex(facing);
        if (direction >= 0) aux = static_cast<uint8_t>(direction);
        if (half == "top") aux |= 4U;
        return aux;
    }
    if (endsWith(leaf, "_trapdoor") || leaf == "trapdoor") {
        const int direction = trapdoorIndex(facing);
        if (direction >= 0) aux = static_cast<uint8_t>(direction);
        if (propertyValue(state, "open") == "true") aux |= 4U;
        if (half == "top") aux |= 8U;
        return aux;
    }
    if (endsWith(leaf, "_door") && !endsWith(leaf, "_trapdoor")) {
        if (half == "upper") {
            aux = 8U;
            if (propertyValue(state, "hinge") == "right") aux |= 1U;
            if (propertyValue(state, "powered") == "true") aux |= 2U;
        } else {
            const int direction = horizontalIndex(facing);
            if (direction >= 0) aux = static_cast<uint8_t>((direction + 1) & 3);
            if (propertyValue(state, "open") == "true") aux |= 4U;
        }
        return aux;
    }
    if (leaf == "bed" || endsWith(leaf, "_bed")) {
        const int direction = horizontalIndex(facing);
        if (direction >= 0) aux = static_cast<uint8_t>(direction);
        if (propertyValue(state, "part") == "head") aux |= 8U;
        return aux;
    }
    if (leaf.find("slab") != std::string_view::npos) {
        if (propertyValue(state, "type") == "top") aux |= 8U;
        return aux;
    }
    if (leaf == "snow" || leaf == "snow_layer") {
        const int layers = parseUnsigned(propertyValue(state, "layers"), 8);
        if (layers > 0) aux = static_cast<uint8_t>(layers - 1);
        return aux;
    }
    if (leaf == "fence_gate" || endsWith(leaf, "_fence_gate")) {
        const int direction = horizontalIndex(facing);
        if (direction >= 0) aux = static_cast<uint8_t>(direction);
        if (propertyValue(state, "open") == "true") aux |= 4U;
        if (propertyValue(state, "powered") == "true") aux |= 8U;
        return aux;
    }
    const int rotation = parseUnsigned(propertyValue(state, "rotation"), 15);
    if (rotation >= 0) return static_cast<uint8_t>(rotation);
    const int level = parseUnsigned(propertyValue(state, "level"), 15);
    if (level >= 0) return static_cast<uint8_t>(level);
    const int direction = horizontalIndex(facing);
    return direction >= 0 ? static_cast<uint8_t>(direction) : 0;
}

}  // namespace

BlockMappingResult resolveProjectionLegacyBlock(uint16_t id, uint8_t data) {
    if (id == 0) return {BlockMappingStatus::Air, {}, {}};
    BlockSpec spec;
    const char* name = legacyName(id);
    spec.command_name = name ? name : "legacy:unknown_" + std::to_string(id);
    spec.aux = data & 0x0FU;
    return {BlockMappingStatus::Mapped, std::move(spec), {}};
}

BlockMappingResult resolveProjectionPaletteState(std::string_view state) {
    const std::string_view identifier = stateIdentifier(state);
    const std::string_view leaf = leafName(identifier);
    if (leaf == "air" || leaf == "cave_air" || leaf == "void_air" ||
        leaf == "structure_void") {
        return {BlockMappingStatus::Air, {}, {}};
    }

    BlockSpec spec;
    spec.command_name = identifier.empty() ? "projection:unknown_block" :
        std::string(identifier);
    spec.aux = visualAux(state, leaf);
    spec.stateful = state.find('[') != std::string_view::npos;
    spec.can_fill = !spec.stateful;
    return {BlockMappingStatus::Mapped, std::move(spec), {}};
}

}  // namespace build_import
