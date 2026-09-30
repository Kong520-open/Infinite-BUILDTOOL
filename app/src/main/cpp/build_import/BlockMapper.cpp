#include "BlockMapper.h"

#include "TargetBlockRegistry.h"

#include "../Json/cJSON.h"

#include <array>
#include <cctype>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace build_import {
namespace {

struct LegacyEntry {
    uint16_t id;
    const char* name;
};

// Core legacy IDs used by classic MCEdit schematics. Metadata is preserved as
// aux; target-version-specific exceptions can be added without changing the
// parser or spool format.
constexpr LegacyEntry kLegacyEntries[] = {
    {1, "minecraft:stone"},
    // Bedrock's current command registry uses grass_block.  Keeping the
    // legacy id but emitting the canonical name prevents an old "grass"
    // command from resolving to air on newer clients.
    {2, "minecraft:grass_block"}, {3, "minecraft:dirt"},
    {4, "minecraft:cobblestone"}, {5, "minecraft:planks"}, {7, "minecraft:bedrock"},
    {8, "minecraft:flowing_water"}, {9, "minecraft:water"}, {10, "minecraft:flowing_lava"},
    {11, "minecraft:lava"}, {12, "minecraft:sand"}, {13, "minecraft:gravel"},
    {14, "minecraft:gold_ore"}, {15, "minecraft:iron_ore"}, {16, "minecraft:coal_ore"},
    {17, "minecraft:log"}, {18, "minecraft:leaves"}, {19, "minecraft:sponge"},
    {20, "minecraft:glass"}, {21, "minecraft:lapis_ore"}, {22, "minecraft:lapis_block"},
    {24, "minecraft:sandstone"}, {25, "minecraft:noteblock"}, {35, "minecraft:wool"},
    {37, "minecraft:yellow_flower"}, {38, "minecraft:red_flower"}, {41, "minecraft:gold_block"},
    {42, "minecraft:iron_block"}, {43, "minecraft:double_stone_slab"}, {44, "minecraft:stone_slab"},
    {45, "minecraft:brick_block"}, {46, "minecraft:tnt"}, {47, "minecraft:bookshelf"},
    {48, "minecraft:mossy_cobblestone"}, {49, "minecraft:obsidian"}, {50, "minecraft:torch"},
    {53, "minecraft:oak_stairs"}, {54, "minecraft:chest"}, {56, "minecraft:diamond_ore"},
    {57, "minecraft:diamond_block"}, {58, "minecraft:crafting_table"}, {60, "minecraft:farmland"},
    {61, "minecraft:furnace"}, {65, "minecraft:ladder"}, {67, "minecraft:stone_stairs"},
    {69, "minecraft:lever"}, {73, "minecraft:redstone_ore"}, {76, "minecraft:redstone_torch"},
    {78, "minecraft:snow_layer"}, {79, "minecraft:ice"}, {80, "minecraft:snow"},
    {81, "minecraft:cactus"}, {82, "minecraft:clay"}, {84, "minecraft:jukebox"},
    {85, "minecraft:fence"}, {87, "minecraft:netherrack"}, {88, "minecraft:soul_sand"},
    {89, "minecraft:glowstone"}, {91, "minecraft:lit_pumpkin"}, {95, "minecraft:stained_glass"},
    {98, "minecraft:stonebrick"}, {102, "minecraft:glass_pane"}, {103, "minecraft:melon_block"},
    {112, "minecraft:nether_brick"}, {121, "minecraft:end_stone"}, {129, "minecraft:emerald_ore"},
    {133, "minecraft:emerald_block"}, {152, "minecraft:redstone_block"}, {155, "minecraft:quartz_block"},
    {159, "minecraft:stained_hardened_clay"}, {165, "minecraft:slime"}, {168, "minecraft:prismarine"},
    {6, "minecraft:sapling"}, {23, "minecraft:dispenser"}, {25, "minecraft:noteblock"},
    {26, "minecraft:bed"}, {27, "minecraft:golden_rail"}, {28, "minecraft:detector_rail"},
    {29, "minecraft:sticky_piston"}, {30, "minecraft:web"}, {31, "minecraft:tallgrass"},
    {32, "minecraft:deadbush"}, {33, "minecraft:piston"},
    {34, "minecraft:piston_arm_collision"}, {35, "minecraft:wool"},
    {39, "minecraft:brown_mushroom"}, {40, "minecraft:red_mushroom"}, {51, "minecraft:fire"},
    {52, "minecraft:mob_spawner"}, {55, "minecraft:redstone_wire"}, {59, "minecraft:wheat"},
    {62, "minecraft:lit_furnace"}, {63, "minecraft:standing_sign"}, {64, "minecraft:wooden_door"},
    {66, "minecraft:rail"}, {68, "minecraft:wall_sign"}, {70, "minecraft:stone_pressure_plate"},
    {71, "minecraft:iron_door"}, {72, "minecraft:wooden_pressure_plate"},
    {74, "minecraft:lit_redstone_ore"}, {75, "minecraft:unlit_redstone_torch"},
    {77, "minecraft:stone_button"}, {83, "minecraft:reeds"}, {86, "minecraft:pumpkin"},
    {90, "minecraft:portal"},
    {92, "minecraft:cake"}, {93, "minecraft:unpowered_repeater"}, {94, "minecraft:powered_repeater"},
    {96, "minecraft:trapdoor"}, {97, "minecraft:monster_egg"}, {99, "minecraft:brown_mushroom_block"},
    {100, "minecraft:red_mushroom_block"}, {101, "minecraft:iron_bars"}, {104, "minecraft:pumpkin_stem"},
    {105, "minecraft:melon_stem"}, {106, "minecraft:vine"}, {107, "minecraft:fence_gate"},
    {108, "minecraft:brick_stairs"}, {109, "minecraft:stone_brick_stairs"}, {110, "minecraft:mycelium"},
    {111, "minecraft:waterlily"}, {113, "minecraft:nether_brick_fence"}, {114, "minecraft:nether_brick_stairs"},
    {115, "minecraft:nether_wart"}, {116, "minecraft:enchanting_table"}, {117, "minecraft:brewing_stand"},
    {118, "minecraft:cauldron"}, {119, "minecraft:end_portal"},
    {120, "minecraft:end_portal_frame"}, {122, "minecraft:dragon_egg"},
    {123, "minecraft:redstone_lamp"}, {124, "minecraft:lit_redstone_lamp"},
    {125, "minecraft:double_wooden_slab"}, {126, "minecraft:wooden_slab"}, {127, "minecraft:cocoa"},
    {128, "minecraft:sandstone_stairs"}, {130, "minecraft:ender_chest"}, {131, "minecraft:tripwire_hook"},
    {132, "minecraft:tripwire"}, {134, "minecraft:spruce_stairs"}, {135, "minecraft:birch_stairs"},
    {136, "minecraft:jungle_stairs"}, {137, "minecraft:command_block"}, {138, "minecraft:beacon"},
    {139, "minecraft:cobblestone_wall"}, {140, "minecraft:flower_pot"}, {141, "minecraft:carrots"},
    {142, "minecraft:potatoes"}, {143, "minecraft:wooden_button"}, {144, "minecraft:skull"},
    {145, "minecraft:anvil"}, {146, "minecraft:trapped_chest"},
    {147, "minecraft:light_weighted_pressure_plate"}, {148, "minecraft:heavy_weighted_pressure_plate"},
    {149, "minecraft:unpowered_comparator"}, {150, "minecraft:powered_comparator"},
    {151, "minecraft:daylight_detector"}, {153, "minecraft:quartz_ore"}, {154, "minecraft:hopper"},
    {156, "minecraft:quartz_stairs"}, {157, "minecraft:activator_rail"}, {158, "minecraft:dropper"},
    {160, "minecraft:stained_glass_pane"}, {161, "minecraft:leaves2"}, {162, "minecraft:log2"},
    {163, "minecraft:acacia_stairs"}, {164, "minecraft:dark_oak_stairs"}, {166, "minecraft:barrier"},
    {167, "minecraft:iron_trapdoor"}, {169, "minecraft:sea_lantern"}, {170, "minecraft:hay_block"},
    {171, "minecraft:carpet"}, {172, "minecraft:hardened_clay"}, {173, "minecraft:coal_block"},
    {174, "minecraft:packed_ice"}, {175, "minecraft:double_plant"}, {176, "minecraft:standing_banner"},
    {177, "minecraft:wall_banner"}, {178, "minecraft:daylight_detector_inverted"},
    {179, "minecraft:red_sandstone"}, {180, "minecraft:red_sandstone_stairs"},
    {181, "minecraft:double_stone_slab2"}, {182, "minecraft:stone_slab2"},
    {183, "minecraft:spruce_fence_gate"}, {184, "minecraft:birch_fence_gate"},
    {185, "minecraft:jungle_fence_gate"}, {186, "minecraft:dark_oak_fence_gate"},
    {187, "minecraft:acacia_fence_gate"}, {188, "minecraft:spruce_fence"},
    {189, "minecraft:birch_fence"}, {190, "minecraft:jungle_fence"},
    {191, "minecraft:dark_oak_fence"}, {192, "minecraft:acacia_fence"},
    {193, "minecraft:spruce_door"}, {194, "minecraft:birch_door"}, {195, "minecraft:jungle_door"},
    {196, "minecraft:acacia_door"}, {197, "minecraft:dark_oak_door"},
    {198, "minecraft:end_rod"}, {199, "minecraft:chorus_plant"}, {200, "minecraft:chorus_flower"},
    {201, "minecraft:purpur_block"}, {202, "minecraft:purpur_pillar"}, {203, "minecraft:purpur_stairs"},
    {204, "minecraft:purpur_double_slab"}, {205, "minecraft:purpur_slab"}, {206, "minecraft:end_bricks"},
    {207, "minecraft:beetroot"}, {208, "minecraft:grass_path"},
    {209, "minecraft:end_gateway"}, {210, "minecraft:repeating_command_block"},
    {211, "minecraft:chain_command_block"}, {212, "minecraft:frosted_ice"},
    {213, "minecraft:magma"},
    {214, "minecraft:nether_wart_block"}, {215, "minecraft:red_nether_brick"}, {216, "minecraft:bone_block"},
    {218, "minecraft:observer"}, {219, "minecraft:white_shulker_box"}, {220, "minecraft:orange_shulker_box"},
    {221, "minecraft:magenta_shulker_box"}, {222, "minecraft:light_blue_shulker_box"},
    {223, "minecraft:yellow_shulker_box"}, {224, "minecraft:lime_shulker_box"},
    {225, "minecraft:pink_shulker_box"}, {226, "minecraft:gray_shulker_box"},
    {227, "minecraft:silver_shulker_box"}, {228, "minecraft:cyan_shulker_box"},
    {229, "minecraft:purple_shulker_box"}, {230, "minecraft:blue_shulker_box"},
    {231, "minecraft:brown_shulker_box"}, {232, "minecraft:green_shulker_box"},
    {233, "minecraft:red_shulker_box"}, {234, "minecraft:black_shulker_box"},
    {235, "minecraft:white_glazed_terracotta"}, {236, "minecraft:orange_glazed_terracotta"},
    {237, "minecraft:magenta_glazed_terracotta"}, {238, "minecraft:light_blue_glazed_terracotta"},
    {239, "minecraft:yellow_glazed_terracotta"}, {240, "minecraft:lime_glazed_terracotta"},
    {241, "minecraft:pink_glazed_terracotta"}, {242, "minecraft:gray_glazed_terracotta"},
    {243, "minecraft:silver_glazed_terracotta"}, {244, "minecraft:cyan_glazed_terracotta"},
    {245, "minecraft:purple_glazed_terracotta"}, {246, "minecraft:blue_glazed_terracotta"},
    {247, "minecraft:brown_glazed_terracotta"}, {248, "minecraft:green_glazed_terracotta"},
    {249, "minecraft:red_glazed_terracotta"}, {250, "minecraft:black_glazed_terracotta"},
    {251, "minecraft:concrete"}, {252, "minecraft:concrete_powder"}, {255, "minecraft:structure_block"},
};

const LegacyEntry* findLegacy(uint16_t id) {
    // Legacy IDs are sparse but bounded by the 12-bit AddBlocks extension.
    // Build the direct lookup once so large schematics do not linearly scan
    // the entire mapping table for every voxel.
    static const auto lookup = [] {
        std::array<const LegacyEntry*, 4096> table{};
        for (const LegacyEntry& entry : kLegacyEntries) {
            if (entry.id < table.size() && table[entry.id] == nullptr) table[entry.id] = &entry;
        }
        return table;
    }();
    return id < lookup.size() ? lookup[id] : nullptr;
}

using Properties = std::unordered_map<std::string, std::string>;

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool replacePrefix(std::string* value, std::string_view prefix, std::string_view replacement) {
    if (!startsWith(*value, prefix)) return false;
    value->replace(0, prefix.size(), replacement.data(), replacement.size());
    return true;
}

void appendWarning(std::string* warning, std::string_view message) {
    if (!warning->empty()) warning->append("; ");
    warning->append(message.data(), message.size());
}

bool isSafePropertyToken(std::string_view value) {
    if (value.empty() || value.size() > 64) return false;
    for (const char character : value) {
        const unsigned char ch = static_cast<unsigned char>(character);
        if (!(std::islower(ch) || std::isdigit(ch) || character == '_' || character == '-')) {
            return false;
        }
    }
    return true;
}

bool parseProperties(std::string_view state, size_t bracket, Properties* properties) {
    if (bracket == std::string_view::npos) return true;
    if (state.empty() || state.back() != ']' || bracket + 1 >= state.size()) return false;
    std::string_view remaining = state.substr(bracket + 1, state.size() - bracket - 2);
    while (!remaining.empty()) {
        const size_t comma = remaining.find(',');
        const std::string_view pair = remaining.substr(0, comma);
        const size_t equals = pair.find('=');
        if (equals == std::string_view::npos || equals == 0 || equals + 1 >= pair.size()) return false;
        std::string key(pair.substr(0, equals));
        std::string value(pair.substr(equals + 1));
        if (!isSafePropertyToken(key) || !isSafePropertyToken(value)) return false;
        if (!properties->emplace(std::move(key), std::move(value)).second) return false;
        if (comma == std::string_view::npos) break;
        remaining.remove_prefix(comma + 1);
    }
    return true;
}

struct CJsonDeleter {
    void operator()(cJSON* value) const noexcept {
        if (value) cJSON_Delete(value);
    }
};

bool isSafeBedrockStateToken(std::string_view value) {
    if (value.empty() || value.size() > 96) return false;
    for (const char character : value) {
        const unsigned char ch = static_cast<unsigned char>(character);
        if (!(std::islower(ch) || std::isdigit(ch) || character == '_' ||
              character == '-' || character == ':')) {
            return false;
        }
    }
    return true;
}

// Converts the raw native state-JSON snapshot (captured verbatim from the
// running game at export time, e.g. {"weirdo_direction":3,"upside_down_bit":
// false}) into a BDX-style bracketed property string
// (`["weirdo_direction"=3,"upside_down_bit"=false]`) that mapBedrockState's
// existing per-block decoders already understand.  This is the only place
// that must stay in sync with new Bedrock block-state keys; individual block
// mapping branches (doors, stairs, trapdoors, copper variants, ...) need no
// per-block changes because they all read through parseBedrockProperties.
// Unsupported JSON shapes are simply dropped: an empty result falls back to
// the caller's legacy-aux path rather than fabricating a state.
std::string nativeStateJsonToBedrockProperties(std::string_view state_json) {
    if (state_json.empty() || state_json.size() > 4096) return {};
    std::unique_ptr<cJSON, CJsonDeleter> root(
        cJSON_ParseWithLength(state_json.data(), state_json.size()));
    if (!root || !cJSON_IsObject(root.get())) return {};

    std::string result;
    result.push_back('[');
    bool first = true;
    for (cJSON* entry = root->child; entry != nullptr; entry = entry->next) {
        if (!entry->string) continue;
        std::string_view key(entry->string);
        // Bedrock occasionally namespace-qualifies a state key (notably
        // minecraft:facing_direction). Strip the namespace the same way
        // parseBedrockProperties does before validating the token.
        constexpr std::string_view kMinecraftPrefix = "minecraft:";
        if (key.size() > kMinecraftPrefix.size() &&
            key.compare(0, kMinecraftPrefix.size(), kMinecraftPrefix) == 0) {
            key.remove_prefix(kMinecraftPrefix.size());
        }
        if (!isSafeBedrockStateToken(key)) continue;

        std::string value;
        if (cJSON_IsTrue(entry)) {
            value = "true";
        } else if (cJSON_IsFalse(entry)) {
            value = "false";
        } else if (cJSON_IsNumber(entry)) {
            if (!std::isfinite(entry->valuedouble) ||
                std::floor(entry->valuedouble) != entry->valuedouble) continue;
            const int64_t number = static_cast<int64_t>(entry->valuedouble);
            if (number < 0 || number > 0xffff) continue;
            value = std::to_string(number);
        } else if (cJSON_IsString(entry) && entry->valuestring) {
            value.assign(entry->valuestring);
        } else {
            continue;
        }
        if (!isSafeBedrockStateToken(value)) continue;

        if (!first) result.push_back(',');
        first = false;
        result.push_back('"');
        result.append(key.data(), key.size());
        result.append("\"=\"");
        result.append(value);
        result.push_back('"');
    }
    result.push_back(']');
    return first ? std::string() : result;
}

// BDX keeps Bedrock block states as a separate, quoted array, e.g.
// `["facing_direction"=3,"conditional_bit"=false]`.  Exporters are not
// consistent about the separator: many write the JSON-ish colon form
// `["color":"white"]` instead.  Both are accepted, but a colon may only
// separate a *quoted* key, because an unquoted key is allowed to contain a
// namespace colon (`minecraft:vertical_half=top`) and would otherwise be
// ambiguous.  This parser is kept deliberately narrow: palette data must
// never be able to turn into command text or arbitrary target identifiers.
std::string_view trimStateWhitespace(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

bool readBedrockStateToken(std::string_view state, size_t* cursor,
                           std::string_view* token, bool* quoted = nullptr) {
    if (quoted) *quoted = false;
    while (*cursor < state.size() && (state[*cursor] == ' ' || state[*cursor] == '\t' ||
                                      state[*cursor] == '\r' || state[*cursor] == '\n')) {
        ++*cursor;
    }
    if (*cursor >= state.size()) return false;
    if (state[*cursor] == '\"') {
        if (quoted) *quoted = true;
        const size_t begin = ++*cursor;
        while (*cursor < state.size() && state[*cursor] != '\"') {
            // Escapes and non-token characters are rejected below.  BDX block
            // states are plain scalar values, so accepting escaped strings
            // would only broaden the parser attack surface.
            ++*cursor;
        }
        if (*cursor >= state.size() || state[*cursor] != '\"') return false;
        *token = state.substr(begin, *cursor - begin);
        ++*cursor;
    } else {
        const size_t begin = *cursor;
        while (*cursor < state.size() && state[*cursor] != '=' && state[*cursor] != ',' &&
               state[*cursor] != ']') {
            ++*cursor;
        }
        *token = trimStateWhitespace(state.substr(begin, *cursor - begin));
    }
    return isSafeBedrockStateToken(*token);
}

bool parseBedrockProperties(std::string_view state, Properties* properties) {
    state = trimStateWhitespace(state);
    if (state.empty()) return true;
    if (state.size() < 2 || state.front() != '[' || state.back() != ']') return false;

    size_t cursor = 1;
    while (true) {
        while (cursor < state.size() && (state[cursor] == ' ' || state[cursor] == '\t' ||
                                         state[cursor] == '\r' || state[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor == state.size() - 1) return true;

        std::string_view key;
        bool quoted_key = false;
        if (!readBedrockStateToken(state, &cursor, &key, &quoted_key)) return false;
        while (cursor < state.size() && (state[cursor] == ' ' || state[cursor] == '\t' ||
                                         state[cursor] == '\r' || state[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor >= state.size()) return false;
        // An unquoted key swallows everything up to `=`, so a colon there is
        // part of the key's namespace rather than a separator.  Only a quoted
        // key has an unambiguous end and may therefore use the colon form.
        if (state[cursor] != '=' && !(quoted_key && state[cursor] == ':')) return false;
        ++cursor;

        std::string_view value;
        if (!readBedrockStateToken(state, &cursor, &value)) return false;
        while (cursor < state.size() && (state[cursor] == ' ' || state[cursor] == '\t' ||
                                         state[cursor] == '\r' || state[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor >= state.size()) return false;

        // Bedrock occasionally namespace-qualifies a state key (notably
        // minecraft:facing_direction).  The namespace is semantic noise for
        // a BDX palette and is removed only after it passed the safe-token
        // check above.
        constexpr std::string_view kMinecraftPrefix = "minecraft:";
        if (startsWith(key, kMinecraftPrefix)) key.remove_prefix(kMinecraftPrefix.size());
        if (!isSafePropertyToken(key)) return false;
        if (!properties->emplace(std::string(key), std::string(value)).second) return false;

        if (state[cursor] == ']') return cursor == state.size() - 1;
        if (state[cursor] != ',') return false;
        ++cursor;
    }
}

const char* bedrockSixWayFacing(uint32_t direction) {
    static constexpr const char* kFacing[] = {"down", "up", "north", "south", "west", "east"};
    return direction < 6 ? kFacing[direction] : nullptr;
}

const char* bedrockDoorFacing(uint32_t direction) {
    // Bedrock's door direction property is the legacy lower-half metadata:
    // 0=east, 1=south, 2=west, 3=north.
    static constexpr const char* kFacing[] = {"east", "south", "west", "north"};
    return direction < 4 ? kFacing[direction] : nullptr;
}

const char* bedrockStairFacing(uint32_t direction) {
    // Unlike doors, Bedrock's historical "weirdo" stair direction uses
    // west as value 1 and south as value 2.
    static constexpr const char* kFacing[] = {"east", "west", "south", "north"};
    return direction < 4 ? kFacing[direction] : nullptr;
}

std::string makeCanonicalState(std::string_view identifier, std::string_view properties) {
    std::string result(identifier);
    if (!properties.empty()) {
        result.push_back('[');
        result.append(properties.data(), properties.size());
        result.push_back(']');
    }
    return result;
}

std::string_view bedrockWoodType(std::string_view value) {
    static constexpr std::string_view kWood[] = {
        "oak", "spruce", "birch", "jungle", "acacia", "dark_oak",
    };
    for (const std::string_view wood : kWood) {
        if (value == wood) return wood;
    }
    return {};
}

std::string_view bedrockStoneSlabSource(std::string_view family, std::string_view type) {
    struct Entry {
        std::string_view family;
        std::string_view type;
        std::string_view source;
    };
    static constexpr Entry kEntries[] = {
        {"stone_block_slab", "stone", "stone_slab"},
        {"stone_block_slab", "smooth_stone", "smooth_stone_slab"},
        // The historical `wood` material of this family is the petrified oak
        // slab, not a wooden slab.  It only appears in pre-flattening spellings
        // of the family, so it is easy to miss when reading modern palettes.
        {"stone_block_slab", "wood", "petrified_oak_slab"},
        {"stone_block_slab", "sandstone", "sandstone_slab"},
        {"stone_block_slab", "cobblestone", "cobblestone_slab"},
        {"stone_block_slab", "brick", "brick_slab"},
        {"stone_block_slab", "stone_brick", "stone_brick_slab"},
        {"stone_block_slab", "quartz", "quartz_slab"},
        {"stone_block_slab", "nether_brick", "nether_brick_slab"},
        {"stone_block_slab2", "red_sandstone", "red_sandstone_slab"},
        {"stone_block_slab2", "purpur", "purpur_slab"},
        {"stone_block_slab2", "prismarine_rough", "prismarine_slab"},
        {"stone_block_slab2", "prismarine_dark", "dark_prismarine_slab"},
        {"stone_block_slab2", "prismarine_brick", "prismarine_brick_slab"},
        {"stone_block_slab2", "mossy_cobblestone", "mossy_cobblestone_slab"},
        {"stone_block_slab2", "smooth_sandstone", "smooth_sandstone_slab"},
        {"stone_block_slab2", "red_nether_brick", "red_nether_brick_slab"},
        {"stone_block_slab3", "end_stone_brick", "end_stone_brick_slab"},
        {"stone_block_slab3", "smooth_red_sandstone", "smooth_red_sandstone_slab"},
        {"stone_block_slab3", "polished_andesite", "polished_andesite_slab"},
        {"stone_block_slab3", "andesite", "andesite_slab"},
        {"stone_block_slab3", "diorite", "diorite_slab"},
        {"stone_block_slab3", "polished_diorite", "polished_diorite_slab"},
        {"stone_block_slab3", "granite", "granite_slab"},
        {"stone_block_slab3", "polished_granite", "polished_granite_slab"},
        {"stone_block_slab4", "mossy_cobblestone", "mossy_cobblestone_slab"},
        {"stone_block_slab4", "smooth_quartz", "smooth_quartz_slab"},
        {"stone_block_slab4", "stone", "stone_slab"},
        {"stone_block_slab4", "cut_sandstone", "cut_sandstone_slab"},
        {"stone_block_slab4", "cut_red_sandstone", "cut_red_sandstone_slab"},
    };
    for (const Entry& entry : kEntries) {
        if (entry.family == family && entry.type == type) return entry.source;
    }
    return {};
}

bool isBedrockFallbackStateful(std::string_view leaf) {
    return leaf == "barrel" || leaf == "beehive" || leaf == "beacon" ||
        leaf == "brewing_stand" || leaf == "cauldron" || leaf == "chest" ||
        leaf == "trapped_chest" || leaf == "composter" || leaf == "crafter" ||
        leaf == "dispenser" || leaf == "dropper" || leaf == "furnace" ||
        leaf == "blast_furnace" || leaf == "smoker" || leaf == "hopper" ||
        leaf == "jukebox" || leaf == "lectern" || leaf == "noteblock" ||
        leaf == "observer" || leaf == "shulker_box" || leaf == "structure_block" ||
        leaf == "command_block" || leaf == "repeating_command_block" ||
        leaf == "chain_command_block" || leaf == "bed" || leaf == "double_plant" ||
        leaf == "trapdoor" || endsWith(leaf, "_door") ||
        endsWith(leaf, "_trapdoor") || endsWith(leaf, "_button") ||
        endsWith(leaf, "_sign") || endsWith(leaf, "_banner") ||
        leaf == "fence_gate" || endsWith(leaf, "_fence_gate") ||
        leaf == "piston" || leaf == "sticky_piston" || leaf == "end_rod" ||
        leaf == "sea_pickle" ||
        leaf == "redstone_wire" || leaf == "repeater" ||
        leaf == "powered_repeater" || leaf == "unpowered_repeater" ||
        leaf == "comparator" || leaf == "powered_comparator" ||
        leaf == "unpowered_comparator" || leaf == "daylight_detector" ||
        leaf == "daylight_detector_inverted" || leaf == "redstone_torch" ||
        leaf == "unlit_redstone_torch" || leaf == "ladder" || leaf == "torch" ||
        leaf == "soul_torch" ||
        leaf.find("rail") != std::string_view::npos || leaf == "lever";
}

const std::string* property(const Properties& properties, std::string_view name) {
    const auto found = properties.find(std::string(name));
    return found == properties.end() ? nullptr : &found->second;
}

bool onlyProperties(const Properties& properties,
                    std::initializer_list<std::string_view> allowed) {
    for (const auto& entry : properties) {
        bool known = false;
        for (const std::string_view name : allowed) {
            if (entry.first == name) {
                known = true;
                break;
            }
        }
        if (!known) return false;
    }
    return true;
}

bool onlyKnownJavaStateProperties(const Properties& properties) {
    static constexpr std::string_view kKnown[] = {
        "age", "attached", "attachment", "axis", "berries", "bites", "bloom",
        "bottom", "can_summon", "candles", "charges", "conditional", "cracked",
        "crafting", "delay", "disarmed", "distance", "down", "drag", "dusted",
        "east", "eggs", "enabled", "extended", "eye", "face", "facing",
        "flower_amount", "half", "hanging", "has_book", "has_bottle_0",
        "has_bottle_1", "has_bottle_2", "has_record", "hatch", "honey_level",
        "in_wall", "instrument", "inverted", "layers", "leaves", "level", "lit",
        "locked", "mode", "moisture", "north", "note", "occupied", "ominous",
        "open", "orientation", "part", "persistent", "pickles", "power", "powered",
        "rotation", "sculk_sensor_phase", "shape", "short", "shrieking",
        "signal_fire", "slot_0_occupied", "slot_1_occupied", "slot_2_occupied",
        "slot_3_occupied", "slot_4_occupied", "slot_5_occupied", "snowy", "south",
        "stage", "thickness", "tilt", "top_slot_bit", "trial_spawner_state", "triggered",
        "type", "slab_type", "unstable", "up", "variant", "vertical_direction",
        "vertical_half", "waterlogged", "west",
    };
    for (const auto& entry : properties) {
        bool known = false;
        for (const std::string_view candidate : kKnown) {
            if (entry.first == candidate) {
                known = true;
                break;
            }
        }
        if (!known) return false;
    }
    return true;
}

bool validBoolean(const std::string* value) {
    return !value || *value == "true" || *value == "false";
}

// Java uses `type=bottom/top/double`, while older exporters and Bedrock
// palettes also use half/vertical_half/top_slot_bit. Keep this conversion in
// one place so every source form preserves the physical half of a slab.
enum class SlabPlacement : uint8_t {
    Bottom,
    Top,
    Double,
};

bool parseSlabPlacementToken(std::string_view value, SlabPlacement* output) {
    if (!output) return false;
    if (value == "bottom" || value == "lower") {
        *output = SlabPlacement::Bottom;
        return true;
    }
    if (value == "top" || value == "upper") {
        *output = SlabPlacement::Top;
        return true;
    }
    if (value == "double") {
        *output = SlabPlacement::Double;
        return true;
    }
    return false;
}

bool parseSlabPlacement(const Properties& properties, bool accept_type,
                        bool accept_slab_type, SlabPlacement* output) {
    if (!output) return false;

    bool has_half = false;
    bool has_double = false;
    SlabPlacement half = SlabPlacement::Bottom;
    const auto add = [&](const std::string* value) {
        if (!value) return true;
        SlabPlacement candidate = SlabPlacement::Bottom;
        if (!parseSlabPlacementToken(*value, &candidate)) return false;
        if (candidate == SlabPlacement::Double) {
            has_double = true;
            return true;
        }
        if (!has_half) {
            half = candidate;
            has_half = true;
            return true;
        }
        return half == candidate;
    };

    if (accept_type && !add(property(properties, "type"))) return false;
    if (accept_slab_type && !add(property(properties, "slab_type"))) return false;
    if (!add(property(properties, "half")) ||
        !add(property(properties, "vertical_half"))) {
        return false;
    }
    const std::string* top_slot_bit = property(properties, "top_slot_bit");
    if (top_slot_bit) {
        if (*top_slot_bit != "true" && *top_slot_bit != "false") return false;
        if (!has_double) {
            const SlabPlacement candidate = *top_slot_bit == "true"
                ? SlabPlacement::Top : SlabPlacement::Bottom;
            if (!has_half) {
                half = candidate;
                has_half = true;
            } else if (half != candidate) {
                return false;
            }
        }
    }
    *output = has_double ? SlabPlacement::Double :
        has_half ? half : SlabPlacement::Bottom;
    return true;
}

bool falseOrMissing(const std::string* value) {
    return !value || *value == "false";
}

bool parseUnsigned(const std::string* value, uint32_t maximum, uint32_t* output) {
    if (!value || value->empty()) return false;
    uint32_t parsed = 0;
    for (const char character : *value) {
        if (character < '0' || character > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(character - '0');
        if (digit > maximum || parsed > (maximum - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    *output = parsed;
    return true;
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

int wallFacingIndex(std::string_view facing) {
    if (facing == "north") return 2;
    if (facing == "south") return 3;
    if (facing == "west") return 4;
    if (facing == "east") return 5;
    return -1;
}

int trapdoorFacingIndex(std::string_view facing) {
    if (facing == "south") return 0;
    if (facing == "north") return 1;
    if (facing == "east") return 2;
    if (facing == "west") return 3;
    return -1;
}

int wallAttachmentIndex(std::string_view facing) {
    if (facing == "east") return 1;
    if (facing == "west") return 2;
    if (facing == "south") return 3;
    if (facing == "north") return 4;
    return -1;
}

int sixWayFacingIndex(std::string_view facing) {
    if (facing == "down") return 0;
    if (facing == "up") return 1;
    if (facing == "north") return 2;
    if (facing == "south") return 3;
    if (facing == "west") return 4;
    if (facing == "east") return 5;
    return -1;
}

int endRodFacingIndex(std::string_view facing) {
    if (facing == "down") return 0;
    if (facing == "up") return 1;
    if (facing == "south") return 2;
    if (facing == "north") return 3;
    if (facing == "east") return 4;
    if (facing == "west") return 5;
    return -1;
}

int crafterOrientationIndex(std::string_view orientation) {
    static constexpr std::array<std::string_view, 12> kOrientations{{
        "down_east", "down_north", "down_south", "down_west",
        "up_east", "up_north", "up_south", "up_west",
        "west_up", "east_up", "north_up", "south_up",
    }};
    for (size_t index = 0; index < kOrientations.size(); ++index) {
        if (orientation == kOrientations[index]) return static_cast<int>(index);
    }
    return -1;
}

int colorAux(std::string_view color) {
    static constexpr std::array<std::string_view, 16> kColors{{
        "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
        "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
    }};
    if (color == "silver") return 8;
    for (size_t index = 0; index < kColors.size(); ++index) {
        if (kColors[index] == color) return static_cast<int>(index);
    }
    return -1;
}

bool coloredShulkerBoxCommand(std::string_view leaf, std::string* command_name) {
    constexpr std::string_view suffix = "_shulker_box";
    if (!endsWith(leaf, suffix) || leaf.size() <= suffix.size()) return false;
    const std::string_view color = leaf.substr(0, leaf.size() - suffix.size());
    if (colorAux(color) < 0) return false;
    command_name->assign("minecraft:");
    if (color == "light_gray") command_name->append("silver");
    else command_name->append(color.data(), color.size());
    command_name->append(suffix.data(), suffix.size());
    return true;
}

bool splitColoredBlock(std::string_view leaf, std::string* command_name, uint16_t* aux) {
    struct Family { std::string_view suffix; std::string_view target; };
    static constexpr Family kFamilies[] = {
        {"_concrete_powder", "concrete_powder"}, {"_stained_glass_pane", "stained_glass_pane"},
        {"_stained_glass", "stained_glass"}, {"_terracotta", "stained_hardened_clay"},
        {"_concrete", "concrete"}, {"_carpet", "carpet"}, {"_wool", "wool"},
    };
    if (endsWith(leaf, "_glazed_terracotta")) return false;
    for (const Family& family : kFamilies) {
        if (leaf.size() <= family.suffix.size() ||
            leaf.substr(leaf.size() - family.suffix.size()) != family.suffix) continue;
        const int color = colorAux(leaf.substr(0, leaf.size() - family.suffix.size()));
        if (color < 0) return false;
        *command_name = "minecraft:" + std::string(family.target);
        *aux = static_cast<uint8_t>(color);
        return true;
    }
    return false;
}

enum class WoodVariantKind {
    None,
    Planks,
    Log,
    Wood,
    Slab,
    Leaves,
    ModernLog,
    ModernWood,
    ModernSlab,
    ModernLeaves,
};

WoodVariantKind mapWoodVariant(std::string_view leaf, std::string* command_name, uint16_t* aux) {
    static constexpr std::array<std::string_view, 12> kWoods{{
        "oak", "spruce", "birch", "jungle", "acacia", "dark_oak", "cherry",
        "mangrove", "bamboo", "crimson", "warped", "pale_oak",
    }};
    for (size_t index = 0; index < kWoods.size(); ++index) {
        const std::string prefix(kWoods[index]);
        // pale_oak is now in the target registry; no substitution needed.
        const size_t mapped_index = index;
        const bool legacy_variant = mapped_index < 6;
        if (leaf == prefix + "_planks") {
            if (legacy_variant) {
                *command_name = "minecraft:planks";
                *aux = static_cast<uint8_t>(mapped_index);
            } else {
                *command_name = "minecraft:" + std::string(leaf);
                *aux = 0;
            }
            return WoodVariantKind::Planks;
        }
        if (leaf == prefix + "_log") {
            if (legacy_variant) {
                *command_name = mapped_index < 4 ? "minecraft:log" : "minecraft:log2";
                *aux = static_cast<uint8_t>(mapped_index < 4 ? mapped_index : mapped_index - 4);
            } else {
                *command_name = "minecraft:" + std::string(leaf);
                *aux = 0;
            }
            return legacy_variant ? WoodVariantKind::Log : WoodVariantKind::ModernLog;
        }
        if (leaf == prefix + "_wood") {
            if (legacy_variant) {
                *command_name = mapped_index < 4 ? "minecraft:log" : "minecraft:log2";
                *aux = static_cast<uint8_t>((mapped_index < 4 ? mapped_index : mapped_index - 4) | 12);
            } else {
                *command_name = "minecraft:" + std::string(leaf);
                *aux = 0;
            }
            return legacy_variant ? WoodVariantKind::Wood : WoodVariantKind::ModernWood;
        }
        if (leaf == prefix + "_slab") {
            if (legacy_variant) {
                *command_name = "minecraft:wooden_slab";
                *aux = static_cast<uint8_t>(mapped_index);
            } else {
                *command_name = "minecraft:" + std::string(leaf);
                *aux = 0;
            }
            return legacy_variant ? WoodVariantKind::Slab : WoodVariantKind::ModernSlab;
        }
        if (leaf == prefix + "_leaves") {
            if (legacy_variant) {
                *command_name = mapped_index < 4 ? "minecraft:leaves" : "minecraft:leaves2";
                *aux = static_cast<uint8_t>(mapped_index < 4 ? mapped_index : mapped_index - 4);
            } else {
                *command_name = "minecraft:" + std::string(leaf);
                *aux = 0;
            }
            return legacy_variant ? WoodVariantKind::Leaves : WoodVariantKind::ModernLeaves;
        }
    }
    return WoodVariantKind::None;
}

bool mapModernWoodDoubleSlab(std::string_view leaf, std::string* command_name) {
    struct Slab { std::string_view source; std::string_view target; };
    static constexpr Slab kSlabs[] = {
        {"cherry_slab", "cherry_double_slab"},
        {"mangrove_slab", "mangrove_double_slab"},
        {"bamboo_slab", "bamboo_double_slab"},
        {"crimson_slab", "crimson_double_slab"},
        {"warped_slab", "warped_double_slab"},
        {"pale_oak_slab", "pale_oak_double_slab"},
    };
    for (const Slab& slab : kSlabs) {
        if (leaf != slab.source) continue;
        *command_name = "minecraft:" + std::string(slab.target);
        return true;
    }
    return false;
}

bool mapWallHead(std::string_view leaf, uint16_t* aux) {
    struct Head { std::string_view source; uint8_t data; };
    static constexpr Head kHeads[] = {
        {"skeleton_wall_skull", 0},
        {"wither_skeleton_wall_skull", 1},
        {"zombie_wall_head", 2},
        {"player_wall_head", 3},
        {"creeper_wall_head", 4},
        {"dragon_wall_head", 5},
    };
    for (const Head& head : kHeads) {
        if (leaf != head.source) continue;
        *aux = head.data;
        return true;
    }
    return false;
}

bool mapStoneSlab(std::string_view leaf,
                  std::string* command_name,
                  std::string* double_command_name,
                  uint8_t* top_bit,
                  uint16_t* aux) {
    struct Slab {
        std::string_view source;
        std::string_view target;
        std::string_view double_target;
        uint8_t data;
        uint8_t upper_bit;
    };
    static constexpr Slab kSlabs[] = {
        {"smooth_stone_slab", "stone_slab", "double_stone_slab", 0, 8},
        {"sandstone_slab", "stone_slab", "double_stone_slab", 1, 8},
        {"petrified_oak_slab", "stone_slab", "double_stone_slab", 2, 8},
        {"cobblestone_slab", "stone_slab", "double_stone_slab", 3, 8},
        {"brick_slab", "stone_slab", "double_stone_slab", 4, 8},
        {"stone_brick_slab", "stone_slab", "double_stone_slab", 5, 8},
        {"quartz_slab", "stone_slab", "double_stone_slab", 6, 8},
        {"nether_brick_slab", "stone_slab", "double_stone_slab", 7, 8},
        {"red_sandstone_slab", "stone_slab2", "double_stone_slab2", 0, 8},
        {"purpur_slab", "stone_slab2", "double_stone_slab2", 1, 8},
        {"prismarine_slab", "stone_slab2", "double_stone_slab2", 2, 8},
        {"dark_prismarine_slab", "stone_slab2", "double_stone_slab2", 3, 8},
        {"prismarine_brick_slab", "stone_slab2", "double_stone_slab2", 4, 8},
        {"mossy_cobblestone_slab", "stone_slab2", "double_stone_slab2", 5, 8},
        {"smooth_sandstone_slab", "stone_slab2", "double_stone_slab2", 6, 8},
        {"red_nether_brick_slab", "stone_slab2", "double_stone_slab2", 7, 8},
        {"end_stone_brick_slab", "stone_block_slab3", "double_stone_block_slab3", 0, 8},
        {"smooth_red_sandstone_slab", "stone_block_slab3", "double_stone_block_slab3", 1, 8},
        {"polished_andesite_slab", "stone_block_slab3", "double_stone_block_slab3", 2, 8},
        {"andesite_slab", "stone_block_slab3", "double_stone_block_slab3", 3, 8},
        {"diorite_slab", "stone_block_slab3", "double_stone_block_slab3", 4, 8},
        {"polished_diorite_slab", "stone_block_slab3", "double_stone_block_slab3", 5, 8},
        {"granite_slab", "stone_block_slab3", "double_stone_block_slab3", 6, 8},
        {"polished_granite_slab", "stone_block_slab3", "double_stone_block_slab3", 7, 8},
        {"mossy_stone_brick_slab", "stone_block_slab4", "double_stone_block_slab4", 0, 8},
        {"smooth_quartz_slab", "stone_block_slab4", "double_stone_block_slab4", 1, 8},
        {"stone_slab", "stone_block_slab4", "double_stone_block_slab4", 2, 8},
        {"cut_sandstone_slab", "stone_block_slab4", "double_stone_block_slab4", 3, 8},
        {"cut_red_sandstone_slab", "stone_block_slab4", "double_stone_block_slab4", 4, 8},
        {"mangrove_slab", "mangrove_slab", "mangrove_double_slab", 0, 1},
        {"bamboo_slab", "bamboo_slab", "bamboo_double_slab", 0, 1},
        {"bamboo_mosaic_slab", "bamboo_mosaic_slab", "bamboo_mosaic_double_slab", 0, 1},
        {"crimson_slab", "crimson_slab", "crimson_double_slab", 0, 1},
        {"warped_slab", "warped_slab", "warped_double_slab", 0, 1},
        {"blackstone_slab", "blackstone_slab", "blackstone_double_slab", 0, 1},
        {"polished_blackstone_slab", "polished_blackstone_slab", "polished_blackstone_double_slab", 0, 1},
        {"polished_blackstone_brick_slab", "polished_blackstone_brick_slab", "polished_blackstone_brick_double_slab", 0, 1},
        {"cobbled_deepslate_slab", "cobbled_deepslate_slab", "cobbled_deepslate_double_slab", 0, 1},
        {"polished_deepslate_slab", "polished_deepslate_slab", "polished_deepslate_double_slab", 0, 1},
        {"deepslate_brick_slab", "deepslate_brick_slab", "deepslate_brick_double_slab", 0, 1},
        {"deepslate_tile_slab", "deepslate_tile_slab", "deepslate_tile_double_slab", 0, 1},
        {"mud_brick_slab", "mud_brick_slab", "mud_brick_double_slab", 0, 1},
        {"tuff_slab", "tuff_slab", "tuff_double_slab", 0, 1},
        {"tuff_brick_slab", "tuff_brick_slab", "tuff_brick_double_slab", 0, 1},
        {"polished_tuff_slab", "polished_tuff_slab", "polished_tuff_double_slab", 0, 1},
        {"cut_copper_slab", "cut_copper_slab", "double_cut_copper_slab", 0, 1},
        {"exposed_cut_copper_slab", "exposed_cut_copper_slab", "exposed_double_cut_copper_slab", 0, 1},
        {"weathered_cut_copper_slab", "weathered_cut_copper_slab", "weathered_double_cut_copper_slab", 0, 1},
        {"oxidized_cut_copper_slab", "oxidized_cut_copper_slab", "oxidized_double_cut_copper_slab", 0, 1},
        {"waxed_cut_copper_slab", "waxed_cut_copper_slab", "waxed_double_cut_copper_slab", 0, 1},
        {"waxed_exposed_cut_copper_slab", "waxed_exposed_cut_copper_slab", "waxed_exposed_double_cut_copper_slab", 0, 1},
        {"waxed_weathered_cut_copper_slab", "waxed_weathered_cut_copper_slab", "waxed_weathered_double_cut_copper_slab", 0, 1},
        {"waxed_oxidized_cut_copper_slab", "waxed_oxidized_cut_copper_slab", "waxed_oxidized_double_cut_copper_slab", 0, 1},
    };
    for (const Slab& slab : kSlabs) {
        if (leaf != slab.source) continue;
        *command_name = "minecraft:" + std::string(slab.target);
        *double_command_name = "minecraft:" + std::string(slab.double_target);
        *top_bit = slab.upper_bit;
        *aux = slab.data;
        return true;
    }
    return false;
}

// Flattened Bedrock calls its double copper slabs both `*_double_slab` and
// `double_*_slab`, depending on the block family/version. Normalize both
// spellings to the Java single-slab identifier before selecting type=double.
bool flattenedDoubleSlabSingleLeaf(std::string_view leaf, std::string* single_leaf) {
    if (!single_leaf) return false;
    constexpr std::string_view kDoubleSlabSuffix = "_double_slab";
    if (endsWith(leaf, kDoubleSlabSuffix)) {
        single_leaf->assign(leaf.substr(0, leaf.size() - kDoubleSlabSuffix.size()));
        single_leaf->append("_slab");
        return true;
    }
    static constexpr std::pair<std::string_view, std::string_view> kAliases[] = {
        {"double_cut_copper_slab", "cut_copper_slab"},
        {"exposed_double_cut_copper_slab", "exposed_cut_copper_slab"},
        {"weathered_double_cut_copper_slab", "weathered_cut_copper_slab"},
        {"oxidized_double_cut_copper_slab", "oxidized_cut_copper_slab"},
        {"waxed_double_cut_copper_slab", "waxed_cut_copper_slab"},
        {"waxed_exposed_double_cut_copper_slab", "waxed_exposed_cut_copper_slab"},
        {"waxed_weathered_double_cut_copper_slab", "waxed_weathered_cut_copper_slab"},
        {"waxed_oxidized_double_cut_copper_slab", "waxed_oxidized_cut_copper_slab"},
    };
    for (const auto& alias : kAliases) {
        if (leaf != alias.first) continue;
        single_leaf->assign(alias.second);
        return true;
    }
    return false;
}

// Pre-flattening Sponge schematics describe the material separately from the
// slab half, for example stone_slab[variant=stone_brick,half=top]. Map that
// material back to a canonical single-slab name before the normal state path
// selects target data. Unknown variants stay unsupported rather than silently
// becoming the wrong material.
std::string_view legacySlabVariantSource(std::string_view family,
                                         std::string_view variant) {
    struct Variant {
        std::string_view family;
        std::string_view value;
        std::string_view source;
    };
    static constexpr Variant kVariants[] = {
        {"stone_slab", "stone", "smooth_stone_slab"},
        {"stone_slab", "smooth_stone", "smooth_stone_slab"},
        {"stone_slab", "sandstone", "sandstone_slab"},
        {"stone_slab", "wood_old", "petrified_oak_slab"},
        {"stone_slab", "petrified_oak", "petrified_oak_slab"},
        {"stone_slab", "cobblestone", "cobblestone_slab"},
        {"stone_slab", "brick", "brick_slab"},
        {"stone_slab", "stone_brick", "stone_brick_slab"},
        {"stone_slab", "nether_brick", "nether_brick_slab"},
        {"stone_slab", "quartz", "quartz_slab"},
        {"stone_slab2", "red_sandstone", "red_sandstone_slab"},
        {"stone_slab2", "purpur", "purpur_slab"},
        {"stone_slab2", "prismarine_rough", "prismarine_slab"},
        {"stone_slab2", "prismarine_dark", "dark_prismarine_slab"},
        {"stone_slab2", "prismarine_brick", "prismarine_brick_slab"},
        {"stone_slab2", "mossy_cobblestone", "mossy_cobblestone_slab"},
        {"stone_slab2", "smooth_sandstone", "smooth_sandstone_slab"},
        {"stone_slab2", "red_nether_brick", "red_nether_brick_slab"},
        {"stone_slab3", "end_stone_brick", "end_stone_brick_slab"},
        {"stone_slab3", "smooth_red_sandstone", "smooth_red_sandstone_slab"},
        {"stone_slab3", "polished_andesite", "polished_andesite_slab"},
        {"stone_slab3", "andesite", "andesite_slab"},
        {"stone_slab3", "diorite", "diorite_slab"},
        {"stone_slab3", "polished_diorite", "polished_diorite_slab"},
        {"stone_slab3", "granite", "granite_slab"},
        {"stone_slab3", "polished_granite", "polished_granite_slab"},
        {"stone_slab4", "mossy_stone_brick", "mossy_stone_brick_slab"},
        {"stone_slab4", "smooth_quartz", "smooth_quartz_slab"},
        {"stone_slab4", "stone", "stone_slab"},
        {"stone_slab4", "cut_sandstone", "cut_sandstone_slab"},
        {"stone_slab4", "cut_red_sandstone", "cut_red_sandstone_slab"},
        {"wooden_slab", "oak", "oak_slab"},
        {"wooden_slab", "spruce", "spruce_slab"},
        {"wooden_slab", "birch", "birch_slab"},
        {"wooden_slab", "jungle", "jungle_slab"},
        {"wooden_slab", "acacia", "acacia_slab"},
        {"wooden_slab", "dark_oak", "dark_oak_slab"},
    };
    for (const Variant& entry : kVariants) {
        if (entry.family == family && entry.value == variant) return entry.source;
    }
    return {};
}

bool isTargetStair(std::string_view leaf) {
    static constexpr std::string_view kStairs[] = {
        "oak_stairs", "spruce_stairs", "birch_stairs", "jungle_stairs",
        "acacia_stairs", "dark_oak_stairs", "mangrove_stairs", "cherry_stairs",
        "bamboo_stairs", "bamboo_mosaic_stairs", "crimson_stairs", "warped_stairs",
        "pale_oak_stairs", "stone_stairs", "normal_stone_stairs", "granite_stairs",
        "polished_granite_stairs", "diorite_stairs", "polished_diorite_stairs",
        "andesite_stairs", "polished_andesite_stairs", "mossy_cobblestone_stairs",
        "stone_brick_stairs", "mossy_stone_brick_stairs", "brick_stairs",
        "end_brick_stairs", "nether_brick_stairs", "red_nether_brick_stairs",
        "sandstone_stairs", "smooth_sandstone_stairs", "red_sandstone_stairs",
        "smooth_red_sandstone_stairs", "quartz_stairs", "smooth_quartz_stairs",
        "purpur_stairs", "prismarine_stairs", "prismarine_bricks_stairs",
        "dark_prismarine_stairs", "blackstone_stairs", "polished_blackstone_stairs",
        "polished_blackstone_brick_stairs", "cut_copper_stairs",
        "exposed_cut_copper_stairs", "weathered_cut_copper_stairs",
        "oxidized_cut_copper_stairs", "waxed_cut_copper_stairs",
        "waxed_exposed_cut_copper_stairs", "waxed_weathered_cut_copper_stairs",
        "waxed_oxidized_cut_copper_stairs", "cobbled_deepslate_stairs",
        "polished_deepslate_stairs", "deepslate_brick_stairs",
        "deepslate_tile_stairs", "mud_brick_stairs", "tuff_stairs",
        "polished_tuff_stairs", "tuff_brick_stairs", "resin_brick_stairs",
    };
    for (const std::string_view name : kStairs) {
        if (leaf == name) return true;
    }
    return false;
}

bool mapStairName(std::string_view leaf, std::string* command_name) {
    struct StairAlias { std::string_view source; std::string_view target; };
    static constexpr StairAlias kAliases[] = {
        {"cobblestone_stairs", "stone_stairs"},
        {"stone_stairs", "normal_stone_stairs"},
        {"end_stone_brick_stairs", "end_brick_stairs"},
        {"prismarine_brick_stairs", "prismarine_bricks_stairs"},
    };
    for (const StairAlias& alias : kAliases) {
        if (leaf != alias.source) continue;
        *command_name = "minecraft:" + std::string(alias.target);
        return true;
    }
    *command_name = "minecraft:" + std::string(leaf);
    return isTargetStair(leaf);
}

bool flatSlabIdentityForVerification(std::string_view leaf, uint16_t aux,
                                     std::string* command_name, uint16_t* command_aux) {
    struct SlabFamily {
        std::string_view legacy_family;
        std::array<std::string_view, 8> materials;
    };
    static constexpr std::array<SlabFamily, 5> kFamilies{{
        {"stone_slab", {"smooth_stone_slab", "sandstone_slab", "petrified_oak_slab",
                        "cobblestone_slab", "brick_slab", "stone_brick_slab",
                        "quartz_slab", "nether_brick_slab"}},
        {"stone_slab2", {"red_sandstone_slab", "purpur_slab", "prismarine_slab",
                         "dark_prismarine_slab", "prismarine_brick_slab",
                         "mossy_cobblestone_slab", "smooth_sandstone_slab",
                         "red_nether_brick_slab"}},
        {"stone_block_slab3", {"end_stone_brick_slab", "smooth_red_sandstone_slab",
                               "polished_andesite_slab", "andesite_slab", "diorite_slab",
                               "polished_diorite_slab", "granite_slab",
                               "polished_granite_slab"}},
        {"stone_block_slab4", {"mossy_stone_brick_slab", "smooth_quartz_slab",
                               "normal_stone_slab", "cut_sandstone_slab",
                               "cut_red_sandstone_slab"}},
        {"wooden_slab", {"oak_slab", "spruce_slab", "birch_slab", "jungle_slab",
                         "acacia_slab", "dark_oak_slab"}},
    }};
    for (const SlabFamily& family : kFamilies) {
        for (size_t index = 0; index < family.materials.size(); ++index) {
            if (leaf != family.materials[index]) continue;
            if (!command_name || !command_aux) return false;
            const bool upper = (aux & 0x08U) != 0U || (aux & 0x01U) != 0U;
            *command_name = "minecraft:" + std::string(family.legacy_family);
            *command_aux = static_cast<uint16_t>(index | (upper ? 0x08U : 0U));
            return true;
        }
    }
    // Historical alias accepted by older Java palettes and NetEase exports.
    if (leaf == "prismarine_bricks_slab") {
        if (!command_name || !command_aux) return false;
        const bool upper = (aux & 0x08U) != 0U || (aux & 0x01U) != 0U;
        *command_name = "minecraft:stone_slab2";
        *command_aux = static_cast<uint16_t>(4 | (upper ? 0x08U : 0U));
        return true;
    }
    return false;
}

bool mapFlatBlock(std::string_view leaf, std::string* command_name, uint16_t* aux) {
    struct FlatBlock { std::string_view source; std::string_view target; uint8_t data; };
    static constexpr FlatBlock kFlatBlocks[] = {
        {"granite", "stone", 1}, {"polished_granite", "stone", 2},
        {"diorite", "stone", 3}, {"polished_diorite", "stone", 4},
        {"andesite", "stone", 5}, {"polished_andesite", "stone", 6},
        {"coarse_dirt", "dirt", 1}, {"podzol", "dirt", 2},
        {"red_sand", "sand", 1},
        {"stone_bricks", "stonebrick", 0},
        {"mossy_stone_bricks", "stonebrick", 1},
        {"cracked_stone_bricks", "stonebrick", 2},
        {"chiseled_stone_bricks", "stonebrick", 3},
        {"cobweb", "web", 0}, {"grass_block", "grass_block", 0},
        {"oak_fence", "fence", 0}, {"bricks", "brick_block", 0},
        {"melon", "melon_block", 0}, {"lily_pad", "waterlily", 0},
        {"snow_block", "snow", 0}, {"terracotta", "hardened_clay", 0},
        {"infested_stone", "monster_egg", 0},
        {"infested_cobblestone", "monster_egg", 1},
        {"infested_stone_bricks", "monster_egg", 2},
        {"infested_mossy_stone_bricks", "monster_egg", 3},
        {"infested_cracked_stone_bricks", "monster_egg", 4},
        {"infested_chiseled_stone_bricks", "monster_egg", 5},
        {"prismarine_bricks", "prismarine", 1}, {"dark_prismarine", "prismarine", 2},
        {"chiseled_quartz_block", "quartz_block", 1},
        {"quartz_pillar", "quartz_block", 2},
        {"nether_bricks", "nether_brick", 0},
        {"nether_quartz_ore", "quartz_ore", 0},
        {"red_nether_bricks", "red_nether_brick", 0},
        {"end_stone_bricks", "end_bricks", 0}, {"magma_block", "magma", 0},
    };
    for (const FlatBlock& entry : kFlatBlocks) {
        if (leaf != entry.source) continue;
        *command_name = "minecraft:" + std::string(entry.target);
        *aux = entry.data;
        return true;
    }
    return false;
}

bool isKnownLegacyCommand(std::string_view command_name) {
    for (const LegacyEntry& entry : kLegacyEntries) {
        if (command_name == entry.name) return true;
    }
    return false;
}

bool isKnownTargetCommand(std::string_view command_name) {
    if (isKnownLegacyCommand(command_name)) return true;
    constexpr std::string_view prefix = "minecraft:";
    if (command_name.size() <= prefix.size() || command_name.substr(0, prefix.size()) != prefix) {
        return false;
    }
    const std::string_view leaf = command_name.substr(prefix.size());
    return target_registry::contains(leaf);
}

bool mapSignName(std::string_view leaf, bool wall, std::string* command_name) {
    if (wall && leaf == "wall_sign") {
        *command_name = "minecraft:wall_sign";
        return true;
    }
    if (!wall && leaf == "standing_sign") {
        *command_name = "minecraft:standing_sign";
        return true;
    }
    const std::string_view suffix = wall ? "_wall_sign" : "_sign";
    if (!endsWith(leaf, suffix) || endsWith(leaf, "_hanging_sign") ||
        endsWith(leaf, "_wall_hanging_sign")) return false;
    const std::string_view wood = leaf.substr(0, leaf.size() - suffix.size());
    if (wood == "oak") {
        *command_name = wall ? "minecraft:wall_sign" : "minecraft:standing_sign";
        return true;
    }
    if (wood == "spruce" || wood == "birch" || wood == "jungle" || wood == "acacia" ||
        wood == "mangrove" || wood == "bamboo" || wood == "crimson" ||
        wood == "warped" || wood == "pale_oak") {
        *command_name = "minecraft:" + std::string(wood) +
                        (wall ? "_wall_sign" : "_standing_sign");
        return true;
    }
    // The target command registry kept the historical `darkoak_*` sign
    // identifiers even though the rest of the dark-oak family uses
    // `dark_oak_*`.  Accept both source spellings here and always emit the
    // one that this target can actually place.
    if (wood == "dark_oak" || wood == "darkoak") {
        *command_name = wall ? "minecraft:darkoak_wall_sign" :
                              "minecraft:darkoak_standing_sign";
        return true;
    }
    if (wood == "cherry") {
        *command_name = "minecraft:cherry" + std::string(wall ? "_wall_sign" : "_standing_sign");
        return true;
    }
    return false;
}

bool mapHangingSignName(std::string_view leaf, bool wall,
                        std::string* command_name) {
    const std::string_view suffix = wall ? "_wall_hanging_sign" : "_hanging_sign";
    if (!endsWith(leaf, suffix)) return false;
    const std::string_view wood = leaf.substr(0, leaf.size() - suffix.size());
    static constexpr std::string_view kWoods[] = {
        "acacia", "bamboo", "birch", "cherry", "crimson", "dark_oak",
        "jungle", "mangrove", "oak", "pale_oak", "spruce", "warped",
    };
    for (const std::string_view supported : kWoods) {
        if (wood != supported) continue;
        *command_name = "minecraft:" + std::string(wood) + "_hanging_sign";
        return true;
    }
    return false;
}

int hangingSignFacingFromRotation(uint32_t rotation) {
    // Java rotation increases south -> west -> north -> east. Bedrock's
    // ceiling-edge hanging sign stores only the nearest cardinal direction.
    static constexpr int kFacing[] = {3, 4, 2, 5};
    return kFacing[((rotation + 2U) & 0x0fU) / 4U];
}

bool mapDoublePlant(std::string_view leaf, uint16_t* aux) {
    struct Plant { std::string_view name; uint8_t data; };
    static constexpr Plant kPlants[] = {
        {"sunflower", 0}, {"lilac", 1}, {"tall_grass", 2},
        {"large_fern", 3}, {"rose_bush", 4}, {"peony", 5},
    };
    for (const Plant& plant : kPlants) {
        if (leaf == plant.name) {
            *aux = plant.data;
            return true;
        }
    }
    return false;
}

bool mapConnectedPane(std::string_view leaf, std::string* command_name, uint16_t* aux) {
    if (leaf == "glass_pane") {
        *command_name = "minecraft:glass_pane";
        *aux = 0;
        return true;
    }
    constexpr std::string_view suffix = "_stained_glass_pane";
    if (!endsWith(leaf, suffix) || leaf.size() <= suffix.size()) return false;
    const int color = colorAux(leaf.substr(0, leaf.size() - suffix.size()));
    if (color < 0) return false;
    *command_name = "minecraft:stained_glass_pane";
    *aux = static_cast<uint8_t>(color);
    return true;
}

bool mapWoodFence(std::string_view leaf, std::string* command_name) {
    struct Fence { std::string_view source; std::string_view target; };
    static constexpr Fence kFences[] = {
        {"oak_fence", "fence"}, {"spruce_fence", "spruce_fence"},
        {"birch_fence", "birch_fence"}, {"jungle_fence", "jungle_fence"},
        {"acacia_fence", "acacia_fence"}, {"dark_oak_fence", "dark_oak_fence"},
        {"mangrove_fence", "mangrove_fence"}, {"cherry_fence", "cherry_fence"},
        {"crimson_fence", "crimson_fence"}, {"warped_fence", "warped_fence"},
        {"bamboo_fence", "bamboo_fence"}, {"pale_oak_fence", "pale_oak_fence"},
    };
    for (const Fence& fence : kFences) {
        if (leaf != fence.source) continue;
        *command_name = "minecraft:" + std::string(fence.target);
        return true;
    }
    return false;
}

bool mapWall(std::string_view leaf, std::string* command_name) {
    static constexpr std::string_view kWalls[] = {
        "cobblestone_wall", "stone_brick_wall", "mossy_cobblestone_wall",
        "andesite_wall", "blackstone_wall", "diorite_wall", "granite_wall",
        "mud_brick_wall", "brick_wall", "polished_blackstone_wall",
        "deepslate_brick_wall", "cobbled_deepslate_wall", "mossy_stone_brick_wall",
        "polished_blackstone_brick_wall", "nether_brick_wall", "deepslate_tile_wall",
        "sandstone_wall", "polished_deepslate_wall", "red_nether_brick_wall",
        "prismarine_wall", "red_sandstone_wall", "tuff_wall", "end_stone_brick_wall",
    };
    for (const std::string_view name : kWalls) {
        if (leaf != name) continue;
        *command_name = "minecraft:" + std::string(leaf);
        return true;
    }
    return false;
}

bool mapSmallFlower(std::string_view leaf, std::string* command_name, uint16_t* aux) {
    struct Flower { std::string_view source; std::string_view target; uint8_t data; };
    static constexpr Flower kFlowers[] = {
        {"dandelion", "yellow_flower", 0}, {"poppy", "poppy", 0},
        {"blue_orchid", "blue_orchid", 0}, {"allium", "allium", 0},
        {"azure_bluet", "azure_bluet", 0}, {"red_tulip", "red_tulip", 0},
        {"orange_tulip", "orange_tulip", 0}, {"white_tulip", "white_tulip", 0},
        {"pink_tulip", "pink_tulip", 0}, {"oxeye_daisy", "oxeye_daisy", 0},
        {"cornflower", "cornflower", 0},
        {"lily_of_the_valley", "lily_of_the_valley", 0},
        {"dead_bush", "deadbush", 0},
    };
    for (const Flower& flower : kFlowers) {
        if (leaf != flower.source) continue;
        *command_name = "minecraft:" + std::string(flower.target);
        *aux = flower.data;
        return true;
    }
    return false;
}

bool mapPressurePlate(std::string_view leaf, std::string* command_name) {
    struct Plate { std::string_view source; std::string_view target; bool is_weighted; };
    static constexpr Plate kPlates[] = {
        {"oak_pressure_plate", "wooden_pressure_plate", false},
        {"stone_pressure_plate", "stone_pressure_plate", false},
        {"spruce_pressure_plate", "spruce_pressure_plate", false},
        {"birch_pressure_plate", "birch_pressure_plate", false},
        {"jungle_pressure_plate", "jungle_pressure_plate", false},
        {"acacia_pressure_plate", "acacia_pressure_plate", false},
        {"dark_oak_pressure_plate", "dark_oak_pressure_plate", false},
        {"crimson_pressure_plate", "crimson_pressure_plate", false},
        {"warped_pressure_plate", "warped_pressure_plate", false},
        {"mangrove_pressure_plate", "mangrove_pressure_plate", false},
        {"cherry_pressure_plate", "cherry_pressure_plate", false},
        {"bamboo_pressure_plate", "bamboo_pressure_plate", false},
        {"pale_oak_pressure_plate", "pale_oak_pressure_plate", false},
        {"polished_blackstone_pressure_plate", "polished_blackstone_pressure_plate", false},
        {"light_weighted_pressure_plate", "light_weighted_pressure_plate", true},
        {"heavy_weighted_pressure_plate", "heavy_weighted_pressure_plate", true},
    };
    for (const Plate& plate : kPlates) {
        if (leaf != plate.source) continue;
        *command_name = "minecraft:" + std::string(plate.target);
        return true;
    }
    return false;
}

bool mapCrop(std::string_view leaf, std::string* command_name, uint32_t* maximum_age) {
    if (leaf == "wheat" || leaf == "carrots" || leaf == "potatoes" ||
        leaf == "pumpkin_stem" || leaf == "melon_stem") {
        *command_name = "minecraft:" + std::string(leaf);
        *maximum_age = 7;
        return true;
    }
    if (leaf == "sugar_cane") {
        *command_name = "minecraft:reeds";
        *maximum_age = 15;
        return true;
    }
    if (leaf == "cactus") {
        *command_name = "minecraft:cactus";
        *maximum_age = 15;
        return true;
    }
    if (leaf == "nether_wart") {
        *command_name = "minecraft:nether_wart";
        *maximum_age = 3;
        return true;
    }
    if (leaf == "beetroots") {
        *command_name = "minecraft:beetroot";
        *maximum_age = 3;
        return true;
    }
    return false;
}

bool isCrop(std::string_view leaf) {
    return leaf == "wheat" || leaf == "carrots" || leaf == "potatoes" ||
        leaf == "pumpkin_stem" || leaf == "melon_stem" || leaf == "sugar_cane" ||
        leaf == "cactus" || leaf == "nether_wart" || leaf == "beetroots";
}

bool isSupportedStrippedPillar(std::string_view leaf) {
    static constexpr std::string_view kNames[] = {
        "stripped_oak_log", "stripped_spruce_log", "stripped_birch_log",
        "stripped_jungle_log", "stripped_acacia_log", "stripped_dark_oak_log",
        "stripped_oak_wood", "stripped_spruce_wood", "stripped_birch_wood",
        "stripped_jungle_wood", "stripped_acacia_wood", "stripped_dark_oak_wood",
        "stripped_cherry_log", "stripped_cherry_wood",
        "stripped_mangrove_log", "stripped_mangrove_wood", "stripped_bamboo_block",
        "stripped_crimson_stem", "stripped_crimson_hyphae",
        "stripped_warped_stem", "stripped_warped_hyphae",
        "stripped_pale_oak_log", "stripped_pale_oak_wood",
    };
    for (const std::string_view name : kNames) {
        if (leaf == name) return true;
    }
    return false;
}

bool mushroomFace(const Properties& properties, std::string_view face) {
    const std::string* value = property(properties, face);
    return value && *value == "true";
}

uint8_t mushroomBlockAux(std::string_view leaf,
                         const Properties& properties,
                         bool* degraded) {
    const bool down = mushroomFace(properties, "down");
    const bool east = mushroomFace(properties, "east");
    const bool north = mushroomFace(properties, "north");
    const bool south = mushroomFace(properties, "south");
    const bool up = mushroomFace(properties, "up");
    const bool west = mushroomFace(properties, "west");
    const unsigned count = static_cast<unsigned>(down) + static_cast<unsigned>(east) +
        static_cast<unsigned>(north) + static_cast<unsigned>(south) +
        static_cast<unsigned>(up) + static_cast<unsigned>(west);
    if (leaf == "mushroom_stem") return count == 0 ? 0 : 15;
    if (count == 0) return 0;
    if (!down && up) {
        if (west && north && !east && !south) return 1;
        if (!west && north && !east && !south) return 2;
        if (!west && north && east && !south) return 3;
        if (west && !north && !east && !south) return 4;
        if (!west && !north && !east && !south) return 5;
        if (!west && !north && east && !south) return 6;
        if (west && !north && !east && south) return 7;
        if (!west && !north && !east && south) return 8;
        if (!west && !north && east && south) return 9;
    }
    *degraded = count != 6;
    return 14;
}

bool mapRailShape(std::string_view shape, bool corners_allowed, uint16_t* aux) {
    struct Shape { std::string_view name; uint8_t data; };
    static constexpr Shape kShapes[] = {
        {"north_south", 0}, {"east_west", 1}, {"ascending_east", 2},
        {"ascending_west", 3}, {"ascending_north", 4}, {"ascending_south", 5},
        {"south_east", 6}, {"south_west", 7}, {"north_west", 8}, {"north_east", 9},
    };
    for (const Shape& entry : kShapes) {
        if (shape != entry.name || (!corners_allowed && entry.data > 5)) continue;
        *aux = entry.data;
        return true;
    }
    return false;
}

bool isDoor(std::string_view leaf) {
    return endsWith(leaf, "_door") && !endsWith(leaf, "_trapdoor");
}

bool isBed(std::string_view leaf) {
    return leaf == "bed" || endsWith(leaf, "_bed");
}

bool isCommandBlock(std::string_view leaf) {
    return leaf == "command_block" || leaf == "repeating_command_block" ||
           leaf == "chain_command_block";
}

}  // namespace

BlockMappingResult BlockMapper::mapLegacy(uint16_t id, uint8_t data) const {
    if (id == 0) return {BlockMappingStatus::Air, {}, {}};
    if (id == 36) {
        // MCEdit schematics store the transient piston head/extension as id 36.
        // The target has no standalone block for it; the piston base carries the
        // persistent structure, so retaining this entry would create a false
        // unsupported-block failure (or an invalid visible block).
        return {BlockMappingStatus::Air, {},
                "legacy piston head/extension is transient and was skipped"};
    }
    const LegacyEntry* entry = findLegacy(id);
    if (!entry) {
        return {BlockMappingStatus::Unsupported, {},
                "unsupported legacy block id " + std::to_string(id)};
    }
    BlockSpec result;
    result.command_name = entry->name;
    result.aux = data & 0x0F;
    std::string mapping_warning;
    if (id == 140) {
        // Legacy flower-pot metadata points at the Java block-entity plant.
        // The import format deliberately excludes block entities, so retain
        // only the empty container rather than materializing the wrong plant.
        if (result.aux != 0) {
            mapping_warning = "legacy flower pot contents require block-entity data and were omitted";
        }
        result.aux = 0;
    } else if (id == 207) {
        // Java beetroot ages 0..3 map to the sparse Bedrock growth states.
        static constexpr uint8_t kBeetrootGrowth[] = {0, 3, 4, 7};
        result.aux = kBeetrootGrowth[result.aux & 0x03U];
    }
    result.phase = phaseFor(result.command_name);
    const size_t separator = result.command_name.rfind(':');
    const std::string_view leaf = std::string_view(result.command_name).substr(separator + 1);
    if ((isBed(leaf) || isDoor(leaf) || leaf == "double_plant") &&
        (result.aux & 0x08) != 0) {
        result.phase = ImportPhase::DependentAttachment;
    }
    result.single_layer_only = result.phase != ImportPhase::Structure;
    // Command blocks always receive a second, per-block native update after
    // their shell is placed.  Do not merge legacy shells into /fill commands:
    // preserving each placement keeps its facing/conditional aux state aligned
    // with that later update's redstone and conditional settings.
    result.stateful = isCommandBlock(leaf) || isBed(leaf) || isDoor(leaf) ||
                      endsWith(leaf, "trapdoor") ||
                      leaf.find("rail") != std::string_view::npos ||
                      leaf.find("button") != std::string_view::npos || leaf == "lever" ||
                      leaf.find("sign") != std::string_view::npos ||
                      endsWith(leaf, "fence_gate") || leaf == "fence_gate" ||
                      leaf == "double_plant";
    result.can_fill = result.phase != ImportPhase::Attachment &&
                      result.phase != ImportPhase::DependentAttachment && !result.stateful;
    return {BlockMappingStatus::Mapped, std::move(result), std::move(mapping_warning)};
}

std::optional<FlatBlockIdentity> BlockMapper::flatBlockIdentityForVerification(
        std::string_view block_name, uint16_t source_aux) {
    const size_t state = block_name.find('[');
    if (state != std::string_view::npos) block_name = block_name.substr(0, state);
    const size_t separator = block_name.rfind(':');
    if (separator != std::string_view::npos &&
        block_name.substr(0, separator) != "minecraft") {
        return std::nullopt;
    }
    const std::string_view leaf = separator == std::string_view::npos
        ? block_name : block_name.substr(separator + 1);
    std::string command_name;
    uint16_t aux = 0;
    // Slabs carry their vertical half in the aux.  Match the modern per-material
    // leaves before the generic alias table so the half survives normalization.
    if (flatSlabIdentityForVerification(leaf, source_aux, &command_name, &aux)) {
        return FlatBlockIdentity{std::move(command_name), aux};
    }
    if (!mapFlatBlock(leaf, &command_name, &aux)) return std::nullopt;
    return FlatBlockIdentity{std::move(command_name), aux};
}

BlockMappingResult BlockMapper::mapSpongeState(std::string_view state) const {
    const auto unsupported = [&](std::string reason) {
        return BlockMappingResult{BlockMappingStatus::Unsupported, {}, std::move(reason)};
    };
    if (state.empty() || state.size() > 4096) return unsupported("invalid Sponge block state");
    const size_t bracket = state.find('[');
    const std::string_view identifier = state.substr(0, bracket);
    Properties properties;
    if (!parseProperties(state, bracket, &properties)) {
        return unsupported("malformed Sponge block state properties");
    }
    const bool air = identifier == "minecraft:air" || identifier == "minecraft:cave_air" ||
                     identifier == "minecraft:void_air" || identifier == "air";
    if (air) {
        if (!properties.empty()) return unsupported("air block has unsupported state properties");
        return {BlockMappingStatus::Air, {}, {}};
    }
    if (!isSafeIdentifier(identifier)) {
        return unsupported("unsafe or unqualified Sponge block identifier");
    }
    if (identifier.substr(0, identifier.find(':')) != "minecraft") {
        return unsupported("non-minecraft block namespace has no target-version mapping");
    }

    const std::string* native_data = property(properties, "infinitecz_data");
    if (native_data) {
        uint32_t aux = 0;
        if (properties.size() != 1 || !parseUnsigned(native_data, 15, &aux)) {
            return unsupported("invalid Infinitecz native block data");
        }
        if (isKnownTargetCommand(identifier)) {
            BlockSpec native_result;
            native_result.command_name.assign(identifier.data(), identifier.size());
            native_result.aux = static_cast<uint16_t>(aux);
            native_result.phase = phaseFor(native_result.command_name);
            const size_t native_separator = native_result.command_name.rfind(':');
            const std::string_view native_leaf = std::string_view(native_result.command_name).substr(
                native_separator == std::string::npos ? 0 : native_separator + 1);
            if ((isBed(native_leaf) || isDoor(native_leaf) || native_leaf == "double_plant") &&
                (native_result.aux & 0x08U) != 0) {
                native_result.phase = ImportPhase::DependentAttachment;
            }
            native_result.single_layer_only = native_result.phase != ImportPhase::Structure;
            native_result.stateful = isBed(native_leaf) || isDoor(native_leaf) ||
                endsWith(native_leaf, "trapdoor") ||
                native_leaf.find("rail") != std::string_view::npos ||
                native_leaf.find("button") != std::string_view::npos || native_leaf == "lever" ||
                native_leaf.find("sign") != std::string_view::npos ||
                endsWith(native_leaf, "fence_gate") || native_leaf == "fence_gate" ||
                native_leaf == "double_plant";
            native_result.can_fill = !native_result.stateful &&
                native_result.phase != ImportPhase::Attachment &&
                native_result.phase != ImportPhase::DependentAttachment;
            return {BlockMappingStatus::Mapped, std::move(native_result), {}};
        }

        // Exported native names can already be flattened Java aliases such as
        // coarse_dirt. They are not target commands themselves, so validate and
        // discard the private marker before applying the normal alias mapping.
        properties.clear();
    }

    // Older target-side exporters wrote only the target command name for
    // stateful blocks (for example `minecraft:sticky_piston` or
    // `minecraft:hopper`) instead of a complete Java state.  Those names are
    // still safe and unambiguous, so retain the block with its target default
    // state rather than entering a Java-specific branch that requires a
    // missing facing/power property. Redstone wire keeps its dedicated path
    // below so a bare wire receives the unpowered default without a warning.
    const size_t propertyless_separator = identifier.rfind(':');
    const std::string_view propertyless_leaf = identifier.substr(
        propertyless_separator == std::string_view::npos ? 0 : propertyless_separator + 1);
    if (properties.empty() && propertyless_leaf != "redstone_wire" &&
        propertyless_leaf != "torch" && propertyless_leaf != "soul_torch" &&
        isBedrockFallbackStateful(propertyless_leaf) && isKnownTargetCommand(identifier)) {
        BlockSpec fallback;
        fallback.command_name.assign(identifier.data(), identifier.size());
        fallback.phase = phaseFor(fallback.command_name);
        fallback.single_layer_only = fallback.phase != ImportPhase::Structure;
        fallback.stateful = true;
        fallback.can_fill = false;
        return {BlockMappingStatus::Mapped, std::move(fallback),
                "target block state properties were omitted; the target default state was used"};
    }

    BlockSpec result;
    const size_t separator = identifier.rfind(':');
    const std::string_view leaf = identifier.substr(separator + 1);
    bool stateful = false;
    bool force_no_fill = false;
    std::string mapping_warning;
    std::optional<ImportPhase> forced_phase;

    // These Java-only blocks describe a transient simulation state or an
    // invisible editor marker. The target has no persistent counterpart, so
    // skipping them is safer than materialising an arbitrary visible block.
    if (leaf == "structure_void") {
        if (!properties.empty()) return unsupported("unsupported structure-void state properties");
        return {BlockMappingStatus::Air, {},
                "Java structure void is an editor marker and was skipped"};
    } else if (leaf == "piston_arm_collision" ||
               leaf == "sticky_piston_arm_collision") {
        if (!properties.empty()) {
            return unsupported("unsupported transient piston collision state properties");
        }
        return {BlockMappingStatus::Air, {},
                "target piston arm collision is transient and was skipped"};
    } else if (leaf == "moving_piston" || leaf == "piston_head") {
        if (!onlyProperties(properties, leaf == "moving_piston"
                                             ? std::initializer_list<std::string_view>{"facing", "type"}
                                             : std::initializer_list<std::string_view>{"facing", "short", "type"}) ||
            !validBoolean(property(properties, "short"))) {
            return unsupported("unsupported transient piston state properties");
        }
        const std::string* facing = property(properties, "facing");
        const std::string* type = property(properties, "type");
        if (!facing || sixWayFacingIndex(*facing) < 0 || !type ||
            (*type != "normal" && *type != "sticky")) {
            return unsupported("transient piston state is invalid");
        }
        return {BlockMappingStatus::Air, {},
                "transient Java piston head/moving block was skipped"};
    } else if (leaf == "light") {
        if (!onlyProperties(properties, {"level", "waterlogged"}) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported Java light-block state properties");
        }
        uint32_t level = 0;
        if (!parseUnsigned(property(properties, "level"), 15, &level)) {
            return unsupported("Java light block has an invalid level");
        }
        return {BlockMappingStatus::Air, {},
                "Java invisible light block has no target equivalent and was skipped"};
    } else if (leaf == "water" || leaf == "flowing_water" ||
        leaf == "lava" || leaf == "flowing_lava") {
        if (!onlyProperties(properties, {"level"})) {
            return unsupported("unsupported fluid state properties");
        }
        uint32_t level = 0;
        if (property(properties, "level") &&
            !parseUnsigned(property(properties, "level"), 15, &level)) {
            return unsupported("fluid state has an invalid level");
        }
        result.command_name = (leaf == "lava" || leaf == "flowing_lava")
            ? "minecraft:lava" : "minecraft:water";
        result.aux = static_cast<uint8_t>(level);
        forced_phase = ImportPhase::Fluid;
    } else if (leaf == "sea_pickle") {
        // Bedrock encodes the pickle count in bits 0-1 and whether the pickle
        // is dry/dead in bit 2. Dropping the Java waterlogged state makes dry
        // decorative sea pickles live, which creates water around them.
        if (!onlyProperties(properties, {"pickles", "waterlogged"}) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported sea-pickle state properties");
        }
        uint32_t pickles = 0;
        if (!parseUnsigned(property(properties, "pickles"), 4, &pickles) || pickles == 0) {
            return unsupported("sea pickle has an invalid pickle count");
        }
        const bool dry = property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "false";
        result.command_name = "minecraft:sea_pickle";
        result.aux = static_cast<uint8_t>((pickles - 1U) | (dry ? 0x04U : 0U));
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (mapConnectedPane(leaf, &result.command_name, &result.aux)) {
        if (!onlyProperties(properties, {"waterlogged", "north", "south", "east", "west"}) ||
            !validBoolean(property(properties, "waterlogged")) ||
            !validBoolean(property(properties, "north")) ||
            !validBoolean(property(properties, "south")) ||
            !validBoolean(property(properties, "east")) ||
            !validBoolean(property(properties, "west"))) {
            return unsupported("unsupported glass-pane state properties");
        }
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged glass pane loses its contained fluid";
        }
    } else if (mapWall(leaf, &result.command_name)) {
        if (!onlyProperties(properties, {"waterlogged", "north", "south",
                                         "east", "west", "up"}) ||
            !validBoolean(property(properties, "waterlogged")) ||
            !validBoolean(property(properties, "up"))) {
            return unsupported("unsupported wall state properties");
        }
        for (const char* direction : {"north", "south", "east", "west"}) {
            const std::string* connection = property(properties, direction);
            if (connection && *connection != "none" && *connection != "low" &&
                *connection != "tall") {
                return unsupported("wall has an invalid connection state");
            }
        }
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged wall loses its contained fluid";
        }
    } else if (leaf == "iron_bars" || leaf == "nether_brick_fence") {
        if (!onlyProperties(properties, {"waterlogged", "north", "south",
                                         "east", "west"}) ||
            !validBoolean(property(properties, "waterlogged")) ||
            !validBoolean(property(properties, "north")) ||
            !validBoolean(property(properties, "south")) ||
            !validBoolean(property(properties, "east")) ||
            !validBoolean(property(properties, "west"))) {
            return unsupported("unsupported connected-block state properties");
        }
        result.command_name.assign(identifier.data(), identifier.size());
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged connected block loses its contained fluid";
        }
    } else if (mapWoodFence(leaf, &result.command_name)) {
        if (!onlyProperties(properties, {"waterlogged", "north", "south", "east", "west"}) ||
            !validBoolean(property(properties, "waterlogged")) ||
            !validBoolean(property(properties, "north")) ||
            !validBoolean(property(properties, "south")) ||
            !validBoolean(property(properties, "east")) ||
            !validBoolean(property(properties, "west"))) {
            return unsupported("unsupported wooden fence state properties");
        }
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged fence loses its contained fluid";
        }
    } else if (leaf == "mushroom_stem" || leaf == "brown_mushroom_block" ||
               leaf == "red_mushroom_block") {
        if (!onlyProperties(properties, {"down", "east", "north", "south", "up", "west"}) ||
            !validBoolean(property(properties, "down")) ||
            !validBoolean(property(properties, "east")) ||
            !validBoolean(property(properties, "north")) ||
            !validBoolean(property(properties, "south")) ||
            !validBoolean(property(properties, "up")) ||
            !validBoolean(property(properties, "west"))) {
            return unsupported("unsupported mushroom block face state");
        }
        bool degraded_faces = false;
        result.command_name = leaf == "mushroom_stem"
            ? "minecraft:red_mushroom_block" : "minecraft:" + std::string(leaf);
        result.aux = mushroomBlockAux(leaf, properties, &degraded_faces);
        if (degraded_faces) {
            mapping_warning = "mushroom face combination is approximated by the target legacy state";
        }
    } else if (leaf == "wall_torch") {
        if (!onlyProperties(properties, {"facing"})) {
            return unsupported("unsupported wall torch state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? wallAttachmentIndex(*facing) : -1;
        if (direction < 0) return unsupported("wall torch has an invalid facing");
        result.command_name = "minecraft:torch";
        result.aux = static_cast<uint8_t>(direction);
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "soul_wall_torch") {
        if (!onlyProperties(properties, {"facing"})) {
            return unsupported("unsupported soul wall torch state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? wallAttachmentIndex(*facing) : -1;
        if (direction < 0) return unsupported("soul wall torch has an invalid facing");
        result.command_name = "minecraft:soul_torch";
        result.aux = static_cast<uint8_t>(direction);
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "torch" || leaf == "soul_torch") {
        if (!properties.empty()) {
            return unsupported("unsupported standing torch state properties");
        }
        result.command_name.assign(identifier.data(), identifier.size());
        result.aux = 5;
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "redstone_wall_torch") {
        if (!onlyProperties(properties, {"facing", "lit"}) ||
            !validBoolean(property(properties, "lit"))) {
            return unsupported("unsupported redstone wall torch state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? wallAttachmentIndex(*facing) : -1;
        if (direction < 0) return unsupported("redstone wall torch has an invalid facing");
        result.command_name = property(properties, "lit") &&
                *property(properties, "lit") == "false"
            ? "minecraft:unlit_redstone_torch" : "minecraft:redstone_torch";
        result.aux = static_cast<uint8_t>(direction);
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "redstone_torch") {
        if (!onlyProperties(properties, {"lit"}) ||
            !validBoolean(property(properties, "lit"))) {
            return unsupported("unsupported standing redstone torch state properties");
        }
        result.command_name = property(properties, "lit") &&
                *property(properties, "lit") == "false"
            ? "minecraft:unlit_redstone_torch" : "minecraft:redstone_torch";
        result.aux = 5;
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "end_rod") {
        if (!onlyProperties(properties, {"facing"})) {
            return unsupported("unsupported end rod state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? endRodFacingIndex(*facing) : -1;
        if (direction < 0) return unsupported("end rod has an invalid facing");
        result.command_name = "minecraft:end_rod";
        result.aux = static_cast<uint8_t>(direction);
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "lantern" || leaf == "soul_lantern") {
        if (!onlyProperties(properties, {"hanging", "waterlogged"}) ||
            !validBoolean(property(properties, "hanging")) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported lantern state properties");
        }
        result.command_name.assign(identifier.data(), identifier.size());
        result.aux = property(properties, "hanging") &&
            *property(properties, "hanging") == "true" ? 1 : 0;
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged lantern loses its contained fluid";
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "chain") {
        if (!onlyProperties(properties, {"axis", "waterlogged"}) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported chain state properties");
        }
        const std::string* axis = property(properties, "axis");
        if (!axis || (*axis != "x" && *axis != "y" && *axis != "z")) {
            return unsupported("chain has an invalid axis");
        }
        result.command_name = "minecraft:chain";
        result.aux = static_cast<uint8_t>(*axis == "y" ? 0 : *axis == "x" ? 1 : 2);
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged chain loses its contained fluid";
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "campfire" || leaf == "soul_campfire") {
        if (!onlyProperties(properties, {"facing", "lit", "signal_fire", "waterlogged"}) ||
            !validBoolean(property(properties, "lit")) ||
            !validBoolean(property(properties, "signal_fire")) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported campfire state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? horizontalIndex(*facing) : -1;
        if (direction < 0) return unsupported("campfire has an invalid facing");
        result.command_name.assign(identifier.data(), identifier.size());
        result.aux = static_cast<uint8_t>(direction |
            (property(properties, "lit") && *property(properties, "lit") == "false" ? 4 : 0));
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged campfire loses its contained fluid";
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "ladder") {
        if (!onlyProperties(properties, {"facing", "waterlogged"}) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported ladder state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? wallFacingIndex(*facing) : -1;
        if (direction < 0) return unsupported("ladder has an invalid facing");
        result.command_name = "minecraft:ladder";
        result.aux = static_cast<uint8_t>(direction);
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged ladder loses its contained fluid";
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "carved_pumpkin" || leaf == "jack_o_lantern") {
        if (!onlyProperties(properties, {"facing"})) {
            return unsupported("unsupported carved pumpkin state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? horizontalIndex(*facing) : -1;
        if (direction < 0) return unsupported("carved pumpkin has an invalid facing");
        result.command_name = leaf == "jack_o_lantern" ? "minecraft:lit_pumpkin" :
                                                       "minecraft:pumpkin";
        result.aux = static_cast<uint8_t>(direction);
    } else if (leaf == "hay_block") {
        if (!onlyProperties(properties, {"axis"})) {
            return unsupported("unsupported hay block state properties");
        }
        const std::string* axis = property(properties, "axis");
        if (!axis || (*axis != "x" && *axis != "y" && *axis != "z")) {
            return unsupported("hay block has an invalid axis");
        }
        result.command_name = "minecraft:hay_block";
        result.aux = static_cast<uint8_t>(*axis == "x" ? 4 : *axis == "z" ? 8 : 0);
    } else if (isSupportedStrippedPillar(leaf)) {
        if (!onlyProperties(properties, {"axis"})) {
            return unsupported("unsupported stripped pillar state properties");
        }
        const std::string* axis = property(properties, "axis");
        if (!axis || (*axis != "x" && *axis != "y" && *axis != "z")) {
            return unsupported("stripped pillar has an invalid axis");
        }
        result.command_name.assign(identifier.data(), identifier.size());
        result.aux = static_cast<uint8_t>(*axis == "y" ? 0 : *axis == "x" ? 1 : 2);
    } else if (leaf == "fire" || leaf == "soul_fire") {
        if (!onlyProperties(properties, {"age", "north", "south",
                                         "east", "west", "up"}) ||
            !validBoolean(property(properties, "north")) ||
            !validBoolean(property(properties, "south")) ||
            !validBoolean(property(properties, "east")) ||
            !validBoolean(property(properties, "west")) ||
            !validBoolean(property(properties, "up"))) {
            return unsupported("unsupported fire state properties");
        }
        uint32_t age = 0;
        if (property(properties, "age") &&
            !parseUnsigned(property(properties, "age"), 15, &age)) {
            return unsupported("fire has an invalid age");
        }
        result.command_name = "minecraft:fire";
        result.aux = static_cast<uint8_t>(age);
        if (leaf == "soul_fire") {
            mapping_warning = "soul fire is imported as ordinary fire because the target has no soul-fire block";
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf.size() > 7 && leaf.substr(0, 7) == "potted_") {
        if (!properties.empty()) return unsupported("unsupported potted plant state properties");
        result.command_name = "minecraft:flower_pot";
        result.aux = 0;
        mapping_warning = "potted plant type requires block-entity data and is imported as an empty pot";
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "water_cauldron" || leaf == "lava_cauldron" ||
               leaf == "powder_snow_cauldron") {
        if (!onlyProperties(properties, {"level"})) {
            return unsupported("unsupported filled-cauldron state properties");
        }
        uint32_t level = 0;
        if (property(properties, "level") &&
            !parseUnsigned(property(properties, "level"), 3, &level)) {
            return unsupported("filled cauldron has an invalid level");
        }
        result.command_name = "minecraft:cauldron";
        result.aux = 0;
        mapping_warning = "Java filled cauldron contents are omitted because the target uses a different state format";
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "spawner") {
        if (!properties.empty()) return unsupported("unsupported Java spawner state properties");
        result.command_name = "minecraft:mob_spawner";
        mapping_warning = "Java spawner entity configuration is omitted";
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "brewing_stand") {
        if (!onlyProperties(properties, {"has_bottle_0", "has_bottle_1", "has_bottle_2"}) ||
            !validBoolean(property(properties, "has_bottle_0")) ||
            !validBoolean(property(properties, "has_bottle_1")) ||
            !validBoolean(property(properties, "has_bottle_2"))) {
            return unsupported("unsupported brewing stand state properties");
        }
        result.command_name = "minecraft:brewing_stand";
        result.aux = 0;
        if ((property(properties, "has_bottle_0") &&
             *property(properties, "has_bottle_0") == "true") ||
            (property(properties, "has_bottle_1") &&
             *property(properties, "has_bottle_1") == "true") ||
            (property(properties, "has_bottle_2") &&
             *property(properties, "has_bottle_2") == "true")) {
            mapping_warning = "brewing stand bottle display is omitted with container contents";
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "hopper") {
        if (!onlyProperties(properties, {"enabled", "facing"}) ||
            !validBoolean(property(properties, "enabled"))) {
            return unsupported("unsupported hopper state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? sixWayFacingIndex(*facing) : -1;
        if (direction < 0 || direction == 1) {
            return unsupported("hopper has an invalid facing");
        }
        result.command_name = "minecraft:hopper";
        result.aux = static_cast<uint8_t>(direction);
        if (property(properties, "enabled") &&
            *property(properties, "enabled") == "false") result.aux |= 8;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "dropper" || leaf == "dispenser") {
        if (!onlyProperties(properties, {"facing", "triggered"}) ||
            !validBoolean(property(properties, "triggered"))) {
            return unsupported("unsupported dispenser/dropper state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? sixWayFacingIndex(*facing) : -1;
        if (direction < 0) return unsupported("dispenser/dropper has an invalid facing");
        result.command_name.assign(identifier.data(), identifier.size());
        result.aux = static_cast<uint8_t>(direction);
        if (property(properties, "triggered") &&
            *property(properties, "triggered") == "true") result.aux |= 8;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "furnace" || leaf == "blast_furnace" || leaf == "smoker") {
        if (!onlyProperties(properties, {"facing", "lit"}) ||
            !validBoolean(property(properties, "lit"))) {
            return unsupported("unsupported furnace state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? sixWayFacingIndex(*facing) : -1;
        if (direction < 2) return unsupported("furnace has an invalid facing");
        const bool lit = property(properties, "lit") &&
            *property(properties, "lit") == "true";
        if (lit) {
            result.command_name = leaf == "furnace" ? "minecraft:lit_furnace" :
                                  leaf == "blast_furnace" ? "minecraft:lit_blast_furnace" :
                                  "minecraft:lit_smoker";
        } else {
            result.command_name.assign(identifier.data(), identifier.size());
        }
        result.aux = static_cast<uint8_t>(direction);
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "observer") {
        if (!onlyProperties(properties, {"facing", "powered"}) ||
            !validBoolean(property(properties, "powered"))) {
            return unsupported("unsupported observer state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? sixWayFacingIndex(*facing) : -1;
        if (direction < 0) return unsupported("observer has an invalid facing");
        result.command_name = "minecraft:observer";
        result.aux = static_cast<uint8_t>(direction |
            (property(properties, "powered") && *property(properties, "powered") == "true" ? 8 : 0));
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "piston" || leaf == "sticky_piston") {
        if (!onlyProperties(properties, {"facing", "extended"}) ||
            !validBoolean(property(properties, "extended"))) {
            return unsupported("unsupported piston state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? sixWayFacingIndex(*facing) : -1;
        if (direction < 0) return unsupported("piston has an invalid facing");
        result.command_name.assign(identifier.data(), identifier.size());
        result.aux = static_cast<uint8_t>(direction);
        if (property(properties, "extended") &&
            *property(properties, "extended") == "true") {
            mapping_warning = "extended Java piston is imported retracted; transient piston head is omitted";
        }
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "chest" || leaf == "trapped_chest") {
        if (!onlyProperties(properties, {"facing", "type", "waterlogged"}) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported chest state properties");
        }
        const std::string* facing = property(properties, "facing");
        const std::string* type = property(properties, "type");
        const int direction = facing ? sixWayFacingIndex(*facing) : -1;
        if (direction < 2 || !type ||
            (*type != "single" && *type != "left" && *type != "right")) {
            return unsupported("chest has an invalid facing or type");
        }
        result.command_name.assign(identifier.data(), identifier.size());
        result.aux = static_cast<uint8_t>(direction);
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged chest loses its contained fluid";
        }
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "ender_chest") {
        if (!onlyProperties(properties, {"facing", "waterlogged"}) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported ender chest state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? sixWayFacingIndex(*facing) : -1;
        if (direction < 2) return unsupported("ender chest has an invalid facing");
        result.command_name = "minecraft:ender_chest";
        result.aux = static_cast<uint8_t>(direction);
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged ender chest loses its contained fluid";
        }
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "repeater") {
        if (!onlyProperties(properties, {"delay", "facing", "locked", "powered"}) ||
            !validBoolean(property(properties, "locked")) ||
            !validBoolean(property(properties, "powered"))) {
            return unsupported("unsupported repeater state properties");
        }
        uint32_t delay = 0;
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? horizontalIndex(*facing) : -1;
        if (direction < 0 || !parseUnsigned(property(properties, "delay"), 4, &delay) ||
            delay == 0) return unsupported("repeater has an invalid facing or delay");
        result.command_name = property(properties, "powered") &&
                *property(properties, "powered") == "true"
            ? "minecraft:powered_repeater" : "minecraft:unpowered_repeater";
        result.aux = static_cast<uint8_t>(direction | ((delay - 1U) << 2U));
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "comparator") {
        if (!onlyProperties(properties, {"facing", "mode", "powered"}) ||
            !validBoolean(property(properties, "powered"))) {
            return unsupported("unsupported comparator state properties");
        }
        const std::string* facing = property(properties, "facing");
        const std::string* mode = property(properties, "mode");
        const int direction = facing ? horizontalIndex(*facing) : -1;
        if (direction < 0 || !mode || (*mode != "compare" && *mode != "subtract")) {
            return unsupported("comparator has an invalid facing or mode");
        }
        result.command_name = property(properties, "powered") &&
                *property(properties, "powered") == "true"
            ? "minecraft:powered_comparator" : "minecraft:unpowered_comparator";
        result.aux = static_cast<uint8_t>(direction | (*mode == "subtract" ? 4 : 0) |
            (property(properties, "powered") && *property(properties, "powered") == "true" ? 8 : 0));
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "redstone_wire") {
        if (!onlyProperties(properties, {"power", "north", "south", "east", "west"})) {
            return unsupported("unsupported redstone wire state properties");
        }
        uint32_t power = 0;
        const std::string* power_property = property(properties, "power");
        // Some Sponge/WorldEdit exports use the valid shorthand
        // `minecraft:redstone_wire` and omit all derived connection state.
        // Missing power is the unpowered default; validate it when present.
        if (power_property && !parseUnsigned(power_property, 15, &power)) {
            return unsupported("redstone wire has an invalid power level");
        }
        for (const char* direction : {"north", "south", "east", "west"}) {
            const std::string* connection = property(properties, direction);
            if (connection && *connection != "none" && *connection != "side" &&
                *connection != "up") return unsupported("redstone wire has an invalid connection");
        }
        result.command_name = "minecraft:redstone_wire";
        result.aux = static_cast<uint8_t>(power);
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "crafter") {
        if (!onlyProperties(properties, {"crafting", "triggered", "orientation"}) ||
            !validBoolean(property(properties, "crafting")) ||
            !validBoolean(property(properties, "triggered"))) {
            return unsupported("unsupported crafter state properties");
        }
        const std::string* orientation = property(properties, "orientation");
        const int direction = orientation ? crafterOrientationIndex(*orientation) : -1;
        if (direction < 0) return unsupported("crafter has an invalid orientation");
        result.command_name = "minecraft:crafter";
        result.aux = static_cast<uint8_t>(direction);
        if (property(properties, "triggered") &&
            *property(properties, "triggered") == "true") result.aux |= 16;
        if (property(properties, "crafting") &&
            *property(properties, "crafting") == "true") result.aux |= 32;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "note_block") {
        if (!onlyProperties(properties, {"instrument", "note", "powered"}) ||
            !validBoolean(property(properties, "powered"))) {
            return unsupported("unsupported note block state properties");
        }
        uint32_t note = 0;
        if (!parseUnsigned(property(properties, "note"), 24, &note)) {
            return unsupported("note block has an invalid note");
        }
        result.command_name = "minecraft:noteblock";
        mapping_warning = "note block instrument, note and powered state cannot be encoded "
                          "by the target command format";
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "barrel") {
        if (!onlyProperties(properties, {"facing", "open"}) ||
            !validBoolean(property(properties, "open"))) {
            return unsupported("unsupported barrel state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? sixWayFacingIndex(*facing) : -1;
        if (direction < 0) return unsupported("barrel has an invalid facing");
        result.command_name = "minecraft:barrel";
        result.aux = static_cast<uint8_t>(direction);
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "shulker_box" ||
               coloredShulkerBoxCommand(leaf, &result.command_name)) {
        if (!onlyProperties(properties, {"facing"})) {
            return unsupported("unsupported shulker box state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? sixWayFacingIndex(*facing) : -1;
        if (direction < 0) return unsupported("shulker box has an invalid facing");
        if (leaf == "shulker_box") result.command_name = "minecraft:undyed_shulker_box";
        result.aux = static_cast<uint8_t>(direction);
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "composter") {
        if (!onlyProperties(properties, {"level"})) {
            return unsupported("unsupported composter state properties");
        }
        uint32_t level = 0;
        if (!parseUnsigned(property(properties, "level"), 8, &level)) {
            return unsupported("composter has an invalid level");
        }
        result.command_name = "minecraft:composter";
        result.aux = static_cast<uint8_t>(level);
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "lightning_rod" || endsWith(leaf, "_lightning_rod")) {
        if (!onlyProperties(properties, {"facing", "powered", "waterlogged"}) ||
            !validBoolean(property(properties, "powered")) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported lightning rod state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? sixWayFacingIndex(*facing) : -1;
        if (direction < 0) return unsupported("lightning rod has an invalid facing");
        // The target registry exposes one lightning-rod identifier. Some Java
        // Java sources encode copper weathering/waxing in the block name, so
        // preserve its orientation and powered state while dropping only the
        // unavailable cosmetic variant.
        result.command_name = "minecraft:lightning_rod";
        if (leaf != "lightning_rod") {
            appendWarning(&mapping_warning,
                          "lightning rod weathering/waxing is unavailable in the target registry and was reset");
        }
        result.aux = static_cast<uint8_t>(direction |
            (property(properties, "powered") && *property(properties, "powered") == "true" ? 8 : 0));
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            appendWarning(&mapping_warning,
                          "waterlogged lightning rod loses its contained fluid");
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "scaffolding") {
        if (!onlyProperties(properties, {"bottom", "distance", "waterlogged"}) ||
            !validBoolean(property(properties, "bottom")) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported scaffolding state properties");
        }
        uint32_t distance = 0;
        if (!parseUnsigned(property(properties, "distance"), 7, &distance)) {
            return unsupported("scaffolding has an invalid distance");
        }
        result.command_name = "minecraft:scaffolding";
        result.aux = static_cast<uint8_t>(distance);
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged scaffolding loses its contained fluid";
        }
        forced_phase = ImportPhase::Gravity;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "deepslate") {
        if (!onlyProperties(properties, {"axis"})) {
            return unsupported("unsupported deepslate state properties");
        }
        const std::string* axis = property(properties, "axis");
        if (axis && *axis != "x" && *axis != "y" && *axis != "z") {
            return unsupported("deepslate has an invalid axis");
        }
        result.command_name = "minecraft:deepslate";
        result.aux = static_cast<uint8_t>(!axis || *axis == "y" ? 0 : *axis == "x" ? 1 : 2);
    } else if (leaf == "bone_block") {
        if (!onlyProperties(properties, {"axis"})) {
            return unsupported("unsupported bone block state properties");
        }
        const std::string* axis = property(properties, "axis");
        if (!axis || (*axis != "x" && *axis != "y" && *axis != "z")) {
            return unsupported("bone block has an invalid axis");
        }
        result.command_name = "minecraft:bone_block";
        // Bone blocks retained the legacy pillar-axis bit layout.
        result.aux = static_cast<uint8_t>(*axis == "x" ? 4 : *axis == "z" ? 8 : 0);
    } else if (leaf == "basalt" || leaf == "polished_basalt" ||
               leaf == "bamboo_block") {
        if (!onlyProperties(properties, {"axis"})) {
            return unsupported("unsupported modern pillar state properties");
        }
        const std::string* axis = property(properties, "axis");
        if (!axis || (*axis != "x" && *axis != "y" && *axis != "z")) {
            return unsupported("modern pillar has an invalid axis");
        }
        result.command_name.assign(identifier.data(), identifier.size());
        result.aux = static_cast<uint8_t>(*axis == "y" ? 0 : *axis == "x" ? 1 : 2);
    } else if (leaf == "anvil" || leaf == "chipped_anvil" || leaf == "damaged_anvil") {
        if (!onlyProperties(properties, {"facing"})) {
            return unsupported("unsupported anvil state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? horizontalIndex(*facing) : -1;
        if (direction < 0) return unsupported("anvil has an invalid facing");
        result.command_name = "minecraft:anvil";
        result.aux = static_cast<uint8_t>(direction |
            (leaf == "chipped_anvil" ? 4 : leaf == "damaged_anvil" ? 8 : 0));
        forced_phase = ImportPhase::Gravity;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "redstone_lamp") {
        if (!onlyProperties(properties, {"lit"}) ||
            !validBoolean(property(properties, "lit"))) {
            return unsupported("unsupported redstone lamp state properties");
        }
        result.command_name = property(properties, "lit") &&
                *property(properties, "lit") == "true"
            ? "minecraft:lit_redstone_lamp" : "minecraft:redstone_lamp";
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "daylight_detector") {
        if (!onlyProperties(properties, {"inverted", "power"}) ||
            !validBoolean(property(properties, "inverted"))) {
            return unsupported("unsupported daylight detector state properties");
        }
        uint32_t power = 0;
        if (property(properties, "power") &&
            !parseUnsigned(property(properties, "power"), 15, &power)) {
            return unsupported("daylight detector has an invalid power state");
        }
        result.command_name = property(properties, "inverted") &&
                *property(properties, "inverted") == "true"
            ? "minecraft:daylight_detector_inverted" : "minecraft:daylight_detector";
        result.aux = static_cast<uint8_t>(power);
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (mapPressurePlate(leaf, &result.command_name)) {
        const bool weighted_plate = leaf == "light_weighted_pressure_plate" ||
                                    leaf == "heavy_weighted_pressure_plate";
        if (weighted_plate) {
            uint32_t power = 0;
            if (!onlyProperties(properties, {"power"}) ||
                !parseUnsigned(property(properties, "power"), 15, &power)) {
                return unsupported("weighted pressure plate has an invalid power state");
            }
            result.aux = static_cast<uint8_t>(power);
        } else if (!onlyProperties(properties, {"powered"}) ||
                   !validBoolean(property(properties, "powered"))) {
            return unsupported("pressure plate has an invalid powered state");
        } else {
            result.aux = property(properties, "powered") &&
                *property(properties, "powered") == "true" ? 15 : 0;
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "farmland") {
            uint32_t moisture = 0;
            if (!onlyProperties(properties, {"moisture"}) ||
                !parseUnsigned(property(properties, "moisture"), 7, &moisture)) {
                return unsupported("farmland has an invalid moisture state");
            }
            result.command_name = "minecraft:farmland";
            result.aux = static_cast<uint8_t>(moisture);
            stateful = true;
            force_no_fill = true;
        } else if (leaf == "snow") {
            uint32_t layers = 0;
            if (!onlyProperties(properties, {"layers"}) ||
                !parseUnsigned(property(properties, "layers"), 8, &layers) || layers == 0) {
                return unsupported("snow layer has an invalid layer count");
            }
            result.command_name = "minecraft:snow_layer";
            result.aux = static_cast<uint8_t>(layers - 1);
            forced_phase = ImportPhase::Attachment;
            stateful = true;
            force_no_fill = true;
    } else if (leaf == "vine") {
        if (!onlyProperties(properties, {"east", "north", "south", "up", "west"}) ||
            !validBoolean(property(properties, "east")) ||
                !validBoolean(property(properties, "north")) ||
                !validBoolean(property(properties, "south")) ||
                !validBoolean(property(properties, "up")) ||
                !validBoolean(property(properties, "west"))) {
                return unsupported("vine has an invalid attachment state");
            }
            result.command_name = "minecraft:vine";
            result.aux = static_cast<uint8_t>(
                (property(properties, "south") && *property(properties, "south") == "true" ? 1 : 0) |
                (property(properties, "west") && *property(properties, "west") == "true" ? 2 : 0) |
                (property(properties, "north") && *property(properties, "north") == "true" ? 4 : 0) |
                (property(properties, "east") && *property(properties, "east") == "true" ? 8 : 0));
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "cave_vines" || leaf == "cave_vines_plant") {
        if (!onlyProperties(properties, {"age", "berries"}) ||
            !validBoolean(property(properties, "berries"))) {
            return unsupported("unsupported cave-vines state properties");
        }
        uint32_t age = 0;
        if (property(properties, "age") &&
            !parseUnsigned(property(properties, "age"), 25, &age)) {
            return unsupported("cave vines have an invalid age");
        }
        const bool berries = property(properties, "berries") &&
            *property(properties, "berries") == "true";
        if (leaf == "cave_vines_plant" && berries) {
            result.command_name = "minecraft:cave_vines_body_with_berries";
        } else if (leaf == "cave_vines" && berries) {
            result.command_name = "minecraft:cave_vines_head_with_berries";
        } else {
            result.command_name = "minecraft:cave_vines";
        }
        if (leaf == "cave_vines_plant" || property(properties, "age")) {
            mapping_warning = "Java cave-vine body/age state is approximated by the target vine block";
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "weeping_vines" || leaf == "weeping_vines_plant" ||
               leaf == "twisting_vines" || leaf == "twisting_vines_plant") {
        if (!onlyProperties(properties, {"age"})) {
            return unsupported("unsupported nether-vines state properties");
        }
        uint32_t age = 0;
        if (property(properties, "age") &&
            !parseUnsigned(property(properties, "age"), 25, &age)) {
            return unsupported("nether vines have an invalid age");
        }
        result.command_name = leaf.find("weeping") != std::string_view::npos
            ? "minecraft:weeping_vines" : "minecraft:twisting_vines";
        if (endsWith(leaf, "_plant") || property(properties, "age")) {
            mapping_warning = "Java vine body/age state is approximated by the target vine block";
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "kelp" || leaf == "kelp_plant") {
        if (!onlyProperties(properties, {"age"})) {
            return unsupported("unsupported kelp state properties");
        }
        uint32_t age = 0;
        if (property(properties, "age") &&
            !parseUnsigned(property(properties, "age"), 25, &age)) {
            return unsupported("kelp has an invalid age");
        }
        result.command_name = "minecraft:kelp";
        if (leaf == "kelp_plant" || property(properties, "age")) {
            mapping_warning = "Java kelp body/age state is approximated by the target kelp block";
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "tall_seagrass") {
        if (!onlyProperties(properties, {"half"})) {
            return unsupported("unsupported tall-seagrass state properties");
        }
        const std::string* half = property(properties, "half");
        if (!half || (*half != "lower" && *half != "upper")) {
            return unsupported("tall seagrass is missing a valid half");
        }
        result.command_name = "minecraft:seagrass";
        mapping_warning = "two-block Java seagrass is approximated by target seagrass";
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "bubble_column") {
        if (!onlyProperties(properties, {"drag"}) ||
            !validBoolean(property(properties, "drag"))) {
            return unsupported("unsupported bubble-column state properties");
        }
        result.command_name = "minecraft:water";
        result.aux = 0;
        mapping_warning = "Java bubble column is imported as still water; bubble force is omitted";
        forced_phase = ImportPhase::Fluid;
    } else if (leaf == "big_dripleaf_stem") {
        if (!onlyProperties(properties, {"facing", "waterlogged"}) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported big-dripleaf stem state properties");
        }
        const std::string* facing = property(properties, "facing");
        if (!facing || horizontalIndex(*facing) < 0) {
            return unsupported("big dripleaf stem is missing a valid facing");
        }
        result.command_name = "minecraft:big_dripleaf";
        mapping_warning = "Java big-dripleaf stem is approximated by the target big dripleaf block";
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "small_dripleaf") {
        if (!onlyProperties(properties, {"facing", "half", "waterlogged"}) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported small-dripleaf state properties");
        }
        const std::string* facing = property(properties, "facing");
        const std::string* half = property(properties, "half");
        if (!facing || horizontalIndex(*facing) < 0 || !half ||
            (*half != "lower" && *half != "upper")) {
            return unsupported("small dripleaf is missing a valid facing or half");
        }
        if (*half == "upper") {
            return {BlockMappingStatus::Air, {},
                    "upper half of Java small dripleaf was skipped during target approximation"};
        }
        result.command_name = "minecraft:big_dripleaf";
        mapping_warning = "Java small dripleaf is approximated by the target big dripleaf block";
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "dirt_path") {
        if (!properties.empty()) return unsupported("unsupported dirt path state properties");
        result.command_name = "minecraft:grass_path";
    } else if (leaf == "rooted_dirt") {
        if (!properties.empty()) return unsupported("unsupported rooted-dirt state properties");
        result.command_name = "minecraft:dirt";
        mapping_warning = "Java rooted dirt is imported as target dirt";
    } else if (leaf == "slime_block") {
        if (!properties.empty()) return unsupported("unsupported slime-block state properties");
        result.command_name = "minecraft:slime";
    } else if (leaf == "podzol") {
            if (!onlyProperties(properties, {"snowy"}) ||
                !validBoolean(property(properties, "snowy"))) {
                return unsupported("unsupported podzol state properties");
            }
            result.command_name = "minecraft:dirt";
            result.aux = 2;
        } else if (leaf == "cake") {
            uint32_t bites = 0;
            if (!onlyProperties(properties, {"bites"}) ||
                !parseUnsigned(property(properties, "bites"), 6, &bites)) {
                return unsupported("cake has an invalid bite count");
            }
            result.command_name = "minecraft:cake";
            result.aux = static_cast<uint8_t>(bites);
            forced_phase = ImportPhase::Attachment;
            stateful = true;
            force_no_fill = true;
        } else if (leaf == "jukebox") {
            if (!onlyProperties(properties, {"has_record"}) ||
                !validBoolean(property(properties, "has_record"))) {
                return unsupported("unsupported jukebox state properties");
            }
            result.command_name = "minecraft:jukebox";
            if (property(properties, "has_record") &&
                *property(properties, "has_record") == "true") {
                mapping_warning = "jukebox record is omitted with container contents";
            }
            stateful = true;
            force_no_fill = true;
    } else if (mapSmallFlower(leaf, &result.command_name, &result.aux)) {
        if (!properties.empty()) return unsupported("unsupported small flower state properties");
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "bush") {
        if (!properties.empty()) return unsupported("unsupported Java bush state properties");
        result.command_name = "minecraft:tallgrass";
        result.aux = 1;
        mapping_warning = "Java bush is approximated by target tall grass";
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "cactus_flower") {
        if (!properties.empty()) return unsupported("unsupported cactus-flower state properties");
        // The target has no cactus-flower equivalent. Do not replace it with a
        // ground flower, which would immediately break when placed on cactus.
        return {BlockMappingStatus::Air, {},
                "Java cactus flower has no target equivalent and was skipped"};
    } else if (leaf == "attached_melon_stem" || leaf == "attached_pumpkin_stem") {
        if (!onlyProperties(properties, {"facing"})) {
            return unsupported("unsupported attached-stem state properties");
        }
        const std::string* facing = property(properties, "facing");
        if (!facing || horizontalIndex(*facing) < 0) {
            return unsupported("attached stem is missing a valid facing");
        }
        result.command_name = leaf == "attached_melon_stem"
            ? "minecraft:melon_stem" : "minecraft:pumpkin_stem";
        result.aux = 7;
        mapping_warning = "attached Java stem is imported as a mature target stem; fruit connection is recomputed";
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (isCrop(leaf)) {
            uint32_t maximum_age = 0;
            if (!mapCrop(leaf, &result.command_name, &maximum_age)) {
                return unsupported("unsupported crop type");
            }
            uint32_t age = 0;
            if (!onlyProperties(properties, {"age"}) ||
                !parseUnsigned(property(properties, "age"), maximum_age, &age)) {
                return unsupported("crop has an invalid age state");
            }
            result.aux = static_cast<uint8_t>(age);
            if (leaf == "beetroots") {
                static constexpr uint8_t kBeetrootGrowth[] = {0, 3, 4, 7};
                result.aux = kBeetrootGrowth[age];
            }
            forced_phase = ImportPhase::Attachment;
            stateful = true;
            force_no_fill = true;
            } else if (leaf == "fern" || leaf == "grass" || leaf == "short_grass") {
                if (!properties.empty()) return unsupported("unsupported short-plant state properties");
                result.command_name = "minecraft:tallgrass";
                result.aux = leaf == "fern" ? 2 : 1;
                forced_phase = ImportPhase::Attachment;
                stateful = true;
                force_no_fill = true;
            } else if (isBed(leaf)) {
                if (!onlyProperties(properties, {"facing", "part", "occupied", "waterlogged"}) ||
                    !validBoolean(property(properties, "occupied")) ||
                    !falseOrMissing(property(properties, "waterlogged"))) {
                    return unsupported("unsupported bed state properties");
                }
                const auto facing = properties.find("facing");
                const std::string* part = property(properties, "part");
                const int direction = facing == properties.end() ? -1 : horizontalIndex(facing->second);
                if (direction < 0 || !part || (*part != "foot" && *part != "head")) {
                    return unsupported("bed state is missing a valid facing or part");
                }
                if (leaf != "bed") {
                    const std::string_view color = leaf.substr(0, leaf.size() - 4);
                    if (colorAux(color) < 0) return unsupported("unsupported bed color");
                }
                result.command_name = "minecraft:bed";
                if (leaf != "bed") {
                    mapping_warning = "bed color cannot be retained without its Java block entity";
                }
                result.aux = static_cast<uint8_t>(direction | (*part == "head" ? 8 : 0));
                forced_phase = *part == "head" ? ImportPhase::DependentAttachment :
                                                  ImportPhase::Attachment;
                stateful = true;
                force_no_fill = true;
    } else if (isDoor(leaf)) {
        if (!onlyProperties(properties, {"facing", "half", "hinge", "open", "powered"}) ||
            !validBoolean(property(properties, "open")) ||
            !validBoolean(property(properties, "powered"))) {
            return unsupported("unsupported door state properties");
        }
        const std::string* facing = property(properties, "facing");
        const std::string* half = property(properties, "half");
        const std::string* hinge = property(properties, "hinge");
        const int direction = facing ? horizontalIndex(*facing) : -1;
        if (direction < 0 || !half || (*half != "lower" && *half != "upper") ||
            !hinge || (*hinge != "left" && *hinge != "right")) {
            return unsupported("door state is missing a valid facing, half, or hinge");
        }
        const std::string_view material = leaf.substr(0, leaf.size() - 5);
        if (material == "oak" || material == "wooden") result.command_name = "minecraft:wooden_door";
        else if (material == "iron" || material == "spruce" || material == "birch" ||
                 material == "jungle" || material == "acacia" || material == "dark_oak" ||
                 material == "mangrove" || material == "cherry" || material == "bamboo" ||
                 material == "crimson" || material == "warped" || material == "pale_oak" ||
                 material == "copper" || material == "exposed_copper" ||
                 material == "weathered_copper" || material == "oxidized_copper" ||
                 material == "waxed_copper" || material == "waxed_exposed_copper" ||
                 material == "waxed_weathered_copper" || material == "waxed_oxidized_copper") {
            result.command_name.assign(identifier.data(), identifier.size());
        } else {
            return unsupported("unsupported door material");
        }
        if (*half == "upper") {
            result.aux = static_cast<uint8_t>(8 | (*hinge == "right" ? 1 : 0) |
                (property(properties, "powered") && *property(properties, "powered") == "true" ? 2 : 0));
            forced_phase = ImportPhase::DependentAttachment;
        } else {
            result.aux = static_cast<uint16_t>((direction + 1) & 3);
            if (property(properties, "open") && *property(properties, "open") == "true") {
                result.aux |= 4;
            }
            // Bedrock derives the lower half's *visual* hinge from the upper
            // block at runtime, so the legacy lower-half aux layout (bits
            // 0-1 facing, bit 2 open) never reserved a hinge bit. That loses
            // the Java palette's lower-half hinge value on re-export/replace,
            // and BlockCommandFormatter cannot recover it from the aux alone.
            // Bit 4 is unused by every other lower-half door consumer (the
            // DependentAttachment check only tests bit 3), so stash the
            // parsed hinge there for the placement formatter to read back.
            if (*hinge == "right") {
                result.aux |= 0x10U;
            }
            forced_phase = ImportPhase::Attachment;
        }
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "trapdoor" || endsWith(leaf, "_trapdoor")) {
        if (!onlyProperties(properties, {"facing", "half", "open", "powered", "waterlogged"}) ||
            !validBoolean(property(properties, "open")) ||
            !validBoolean(property(properties, "powered")) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported trapdoor state properties");
        }
        const std::string* facing = property(properties, "facing");
        const std::string* half = property(properties, "half");
        const int direction = facing ? trapdoorFacingIndex(*facing) : -1;
        if (direction < 0 || !half || (*half != "bottom" && *half != "top")) {
            return unsupported("trapdoor state is missing a valid facing or half");
        }
        const std::string_view material = leaf == "trapdoor" ? std::string_view("oak") :
                                          leaf.substr(0, leaf.size() - 9);
        if (material == "oak") result.command_name = "minecraft:trapdoor";
        else if (material == "iron" || material == "spruce" || material == "birch" ||
                 material == "jungle" || material == "acacia" || material == "dark_oak" ||
                 material == "mangrove" || material == "cherry" || material == "bamboo" ||
                 material == "crimson" || material == "warped" || material == "pale_oak" ||
                 material == "copper" || material == "exposed_copper" ||
                 material == "weathered_copper" || material == "oxidized_copper" ||
                 material == "waxed_copper" || material == "waxed_exposed_copper" ||
                 material == "waxed_weathered_copper" || material == "waxed_oxidized_copper") {
            result.command_name.assign(identifier.data(), identifier.size());
        } else {
            return unsupported("unsupported trapdoor material");
        }
        result.aux = static_cast<uint8_t>(direction | (*half == "top" ? 4 : 0));
        if (property(properties, "open") && *property(properties, "open") == "true") {
            result.aux |= 8;
        }
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged trapdoor loses its contained fluid";
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "rail" || leaf == "powered_rail" || leaf == "golden_rail" ||
               leaf == "detector_rail" || leaf == "activator_rail") {
        if (!onlyProperties(properties, {"shape", "powered", "waterlogged"}) ||
            !validBoolean(property(properties, "powered")) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported rail state properties");
        }
        const std::string* shape = property(properties, "shape");
        const bool ordinary = leaf == "rail";
        if (!shape || !mapRailShape(*shape, ordinary, &result.aux)) {
            return unsupported("rail state has an unsupported shape");
        }
        result.command_name = leaf == "powered_rail" ? "minecraft:golden_rail" :
                              "minecraft:" + std::string(leaf);
        if (!ordinary && property(properties, "powered") &&
            *property(properties, "powered") == "true") result.aux |= 8;
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged rail loses its contained fluid";
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (endsWith(leaf, "_button") || leaf == "stone_button" ||
               leaf == "wooden_button") {
        if (!onlyProperties(properties, {"face", "facing", "powered"}) ||
            !validBoolean(property(properties, "powered"))) {
            return unsupported("unsupported button state properties");
        }
        const std::string* face = property(properties, "face");
        const std::string* facing = property(properties, "facing");
        if (!face || !facing) return unsupported("button state is missing face or facing");
        int direction = -1;
        if (*face == "wall") direction = wallAttachmentIndex(*facing);
        else if (*face == "floor" && horizontalIndex(*facing) >= 0) direction = 5;
        else if (*face == "ceiling" && horizontalIndex(*facing) >= 0) direction = 0;
        if (direction < 0) return unsupported("button state has an invalid face or facing");
        if (leaf == "oak_button") result.command_name = "minecraft:wooden_button";
        else if (leaf == "stone_button" || leaf == "wooden_button" ||
                 leaf == "spruce_button" || leaf == "birch_button" ||
                 leaf == "jungle_button" || leaf == "acacia_button" ||
                 leaf == "dark_oak_button" || leaf == "mangrove_button" ||
                 leaf == "cherry_button" || leaf == "bamboo_button" ||
                 leaf == "crimson_button" || leaf == "warped_button" ||
                 leaf == "pale_oak_button" || leaf == "polished_blackstone_button") {
            result.command_name.assign(identifier.data(), identifier.size());
        } else {
            return unsupported("unsupported button material");
        }
        result.aux = static_cast<uint8_t>(direction |
            (property(properties, "powered") && *property(properties, "powered") == "true" ? 8 : 0));
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "lever") {
        if (!onlyProperties(properties, {"face", "facing", "powered"}) ||
            !validBoolean(property(properties, "powered"))) {
            return unsupported("unsupported lever state properties");
        }
        const std::string* face = property(properties, "face");
        const std::string* facing = property(properties, "facing");
        if (!face || !facing) return unsupported("lever state is missing face or facing");
        int direction = -1;
        if (*face == "wall") direction = wallAttachmentIndex(*facing);
        else if (*face == "floor") {
            direction = (*facing == "east" || *facing == "west") ? 6 :
                        (*facing == "north" || *facing == "south") ? 5 : -1;
        } else if (*face == "ceiling") {
            direction = (*facing == "east" || *facing == "west") ? 0 :
                        (*facing == "north" || *facing == "south") ? 7 : -1;
        }
        if (direction < 0) return unsupported("lever state has an invalid face or facing");
        result.command_name = "minecraft:lever";
        result.aux = static_cast<uint8_t>(direction |
            (property(properties, "powered") && *property(properties, "powered") == "true" ? 8 : 0));
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (mapWallHead(leaf, &result.aux)) {
        if (!onlyProperties(properties, {"facing", "powered"}) ||
            !validBoolean(property(properties, "powered"))) {
            return unsupported("unsupported wall-head state properties");
        }
        const std::string* facing = property(properties, "facing");
        if (!facing || wallFacingIndex(*facing) < 0) {
            return unsupported("wall head is missing a valid facing");
        }
        result.command_name = "minecraft:skull";
        mapping_warning = "Java wall-head orientation and block-entity data are omitted";
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "standing_banner" || leaf == "wall_banner" ||
               endsWith(leaf, "_banner") || endsWith(leaf, "_wall_banner")) {
        const bool wall = leaf == "wall_banner" || endsWith(leaf, "_wall_banner");
        if (wall) {
            if (!onlyProperties(properties, {"facing"})) {
                return unsupported("unsupported wall banner state");
            }
            const std::string* facing = property(properties, "facing");
            const int direction = facing ? wallFacingIndex(*facing) : -1;
            if (direction < 0) return unsupported("wall banner has an invalid facing");
            result.command_name = "minecraft:wall_banner";
            result.aux = static_cast<uint8_t>(direction);
        } else {
            if (!onlyProperties(properties, {"rotation"})) {
                return unsupported("unsupported standing banner state");
            }
            uint32_t rotation = 0;
            if (!parseUnsigned(property(properties, "rotation"), 15, &rotation)) {
                return unsupported("standing banner has an invalid rotation");
            }
            result.command_name = "minecraft:standing_banner";
            result.aux = static_cast<uint8_t>(rotation);
        }
        mapping_warning = "banner color and pattern block-entity data are omitted";
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (endsWith(leaf, "_wall_hanging_sign")) {
        if (!onlyProperties(properties, {"facing", "waterlogged"}) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported wall hanging-sign state");
        }
        if (!mapHangingSignName(leaf, true, &result.command_name)) {
            return unsupported("unsupported wall hanging-sign material");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? wallFacingIndex(*facing) : -1;
        if (direction < 0) return unsupported("wall hanging sign is missing a valid facing");
        result.aux = static_cast<uint8_t>(direction);
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged wall hanging sign loses its contained fluid";
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (endsWith(leaf, "_hanging_sign")) {
        if (!onlyProperties(properties, {"attached", "rotation", "waterlogged"}) ||
            !validBoolean(property(properties, "attached")) ||
            !validBoolean(property(properties, "waterlogged")) ||
            !mapHangingSignName(leaf, false, &result.command_name)) {
            return unsupported("unsupported ceiling hanging-sign state");
        }
        const std::string* attached_property = property(properties, "attached");
        uint32_t rotation = 0;
        if (!attached_property ||
            !parseUnsigned(property(properties, "rotation"), 15, &rotation)) {
            return unsupported("ceiling hanging sign is missing attached or rotation state");
        }
        const bool attached = *attached_property == "true";
        if (attached) {
            // Bedrock center signs use ground_sign_direction, keep a dummy
            // north-facing value, and set both hanging and attached bits.
            result.aux = static_cast<uint16_t>(0x0182U | (rotation << 3U));
        } else {
            result.aux = static_cast<uint16_t>(
                0x0080U | hangingSignFacingFromRotation(rotation));
            if ((rotation & 0x03U) != 0U) {
                mapping_warning =
                    "ceiling-edge hanging-sign rotation was rounded to a target cardinal direction";
            }
        }
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            appendWarning(&mapping_warning,
                          "waterlogged ceiling hanging sign loses its contained fluid");
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "wall_sign" || endsWith(leaf, "_wall_sign")) {
        if (!onlyProperties(properties, {"facing", "waterlogged"}) ||
            !validBoolean(property(properties, "waterlogged")) ||
            !mapSignName(leaf, true, &result.command_name)) {
            return unsupported("unsupported wall sign state");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? wallFacingIndex(*facing) : -1;
        if (direction < 0) return unsupported("wall sign state is missing a valid facing");
        result.aux = static_cast<uint8_t>(direction);
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged wall sign loses its contained fluid";
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "standing_sign" ||
               (endsWith(leaf, "_sign") && !endsWith(leaf, "_hanging_sign") &&
                !endsWith(leaf, "_wall_hanging_sign"))) {
        if (!onlyProperties(properties, {"rotation", "waterlogged"}) ||
            !validBoolean(property(properties, "waterlogged")) ||
            !mapSignName(leaf, false, &result.command_name)) {
            return unsupported("unsupported standing sign state");
        }
        uint32_t rotation = 0;
        if (!parseUnsigned(property(properties, "rotation"), 15, &rotation)) {
            return unsupported("standing sign state is missing a valid rotation");
        }
        result.aux = static_cast<uint8_t>(rotation);
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged standing sign loses its contained fluid";
        }
        forced_phase = ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf == "fence_gate" || endsWith(leaf, "_fence_gate")) {
        if (!onlyProperties(properties, {"facing", "in_wall", "open", "powered"}) ||
            !validBoolean(property(properties, "in_wall")) ||
            !validBoolean(property(properties, "open")) ||
            !validBoolean(property(properties, "powered"))) {
            return unsupported("unsupported fence gate state properties");
        }
        const std::string* facing = property(properties, "facing");
        const int direction = facing ? horizontalIndex(*facing) : -1;
        if (direction < 0) return unsupported("fence gate state is missing a valid facing");
        if (leaf == "oak_fence_gate") result.command_name = "minecraft:fence_gate";
        else if (leaf == "fence_gate" || leaf == "spruce_fence_gate" ||
                  leaf == "birch_fence_gate" || leaf == "jungle_fence_gate" ||
                  leaf == "acacia_fence_gate" || leaf == "dark_oak_fence_gate" ||
                  leaf == "mangrove_fence_gate" || leaf == "cherry_fence_gate" ||
                  leaf == "bamboo_fence_gate" || leaf == "crimson_fence_gate" ||
                  leaf == "warped_fence_gate" || leaf == "pale_oak_fence_gate") {
            result.command_name.assign(identifier.data(), identifier.size());
        } else {
            return unsupported("unsupported fence gate material");
        }
        result.aux = static_cast<uint8_t>(direction |
            (property(properties, "open") && *property(properties, "open") == "true" ? 4 : 0) |
            (property(properties, "in_wall") && *property(properties, "in_wall") == "true" ? 8 : 0));
        if (property(properties, "powered") && *property(properties, "powered") == "true") {
            mapping_warning = "fence gate powered state is recomputed by the target redstone network";
        }
        stateful = true;
        force_no_fill = true;
    } else if (mapDoublePlant(leaf, &result.aux)) {
        if (!onlyProperties(properties, {"half"})) {
            return unsupported("unsupported double-plant state properties");
        }
        const std::string* half = property(properties, "half");
        if (!half || (*half != "lower" && *half != "upper")) {
            return unsupported("double-plant state is missing a valid half");
        }
        result.command_name = "minecraft:double_plant";
        if (*half == "upper") result.aux = 8;
        forced_phase = *half == "upper" ? ImportPhase::DependentAttachment :
                                          ImportPhase::Attachment;
        stateful = true;
        force_no_fill = true;
    } else if (leaf.find("stairs") != std::string_view::npos) {
        if (!onlyProperties(properties, {"facing", "half", "shape", "waterlogged"}) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported stair state properties");
        }
        const std::string* facing = property(properties, "facing");
        const std::string* half = property(properties, "half");
        const std::string* shape = property(properties, "shape");
        const int direction = facing ? stairIndex(*facing) : -1;
        const bool valid_shape = !shape || *shape == "straight" || *shape == "inner_left" ||
                                 *shape == "inner_right" || *shape == "outer_left" ||
                                 *shape == "outer_right";
        if (direction < 0 || !half || (*half != "bottom" && *half != "top") || !valid_shape) {
            return unsupported("stair state has an invalid facing, half, or shape");
        }
        if (!mapStairName(leaf, &result.command_name)) return unsupported("unsupported stair type");
        result.aux = static_cast<uint8_t>(direction | (*half == "top" ? 4 : 0));
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged stair loses its contained fluid";
        }
    } else if (leaf.find("slab") != std::string_view::npos) {
        if (!onlyProperties(properties, {"type", "half", "slab_type", "vertical_half",
                                         "top_slot_bit", "variant", "waterlogged"}) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return unsupported("unsupported slab state properties");
        }
        SlabPlacement placement = SlabPlacement::Bottom;
        if (!parseSlabPlacement(properties, true, true, &placement)) {
            return unsupported("slab state has conflicting or invalid half properties");
        }
        std::string normalized_slab_leaf;
        const bool named_double_slab = flattenedDoubleSlabSingleLeaf(leaf, &normalized_slab_leaf);
        std::string_view slab_leaf = named_double_slab ? std::string_view(normalized_slab_leaf) : leaf;
        if (slab_leaf == "prismarine_bricks_slab") slab_leaf = "prismarine_brick_slab";
        if (const std::string* variant = property(properties, "variant")) {
            const std::string_view variant_source = legacySlabVariantSource(slab_leaf, *variant);
            if (variant_source.empty()) return unsupported("unsupported legacy slab variant");
            slab_leaf = variant_source;
        }
        if (named_double_slab) placement = SlabPlacement::Double;

        std::string double_slab_command;
        uint8_t top_bit = 8;
        const WoodVariantKind wood = mapWoodVariant(slab_leaf, &result.command_name, &result.aux);
        if (wood == WoodVariantKind::ModernSlab) top_bit = 1;
        if (wood != WoodVariantKind::Slab && wood != WoodVariantKind::ModernSlab &&
            !mapStoneSlab(slab_leaf, &result.command_name, &double_slab_command,
                           &top_bit, &result.aux)) {
            result.command_name.assign(identifier.data(), identifier.size());
            if (!isKnownLegacyCommand(result.command_name)) return unsupported("unsupported slab type");
        }
        if (placement == SlabPlacement::Top) result.aux |= top_bit;
        else if (placement == SlabPlacement::Double) {
            if (!double_slab_command.empty()) {
                result.command_name = std::move(double_slab_command);
            } else if (wood == WoodVariantKind::ModernSlab &&
                       mapModernWoodDoubleSlab(slab_leaf, &result.command_name)) {
                result.aux = 0;
            } else if (result.command_name == "minecraft:wooden_slab") {
                result.command_name = "minecraft:double_wooden_slab";
            } else if (result.command_name == "minecraft:cherry_slab") {
                result.command_name = "minecraft:cherry_double_slab";
            } else if (result.command_name == "minecraft:stone_slab2") {
                result.command_name = "minecraft:double_stone_slab2";
            } else if (result.command_name == "minecraft:purpur_slab") {
                result.command_name = "minecraft:purpur_double_slab";
            } else {
                return unsupported("unsupported double slab type");
            }
        }
        if (property(properties, "waterlogged") &&
            *property(properties, "waterlogged") == "true") {
            mapping_warning = "waterlogged slab loses its contained fluid";
        }
    } else {
        const WoodVariantKind wood = mapWoodVariant(leaf, &result.command_name, &result.aux);
        if (wood == WoodVariantKind::Log || wood == WoodVariantKind::Wood ||
            wood == WoodVariantKind::ModernLog || wood == WoodVariantKind::ModernWood) {
            if (!onlyProperties(properties, {"axis"})) {
                return unsupported("unsupported log state properties");
            }
            const std::string* axis = property(properties, "axis");
            if (!axis || (*axis != "x" && *axis != "y" && *axis != "z")) {
                return unsupported("log state is missing a valid axis");
            }
            if (wood == WoodVariantKind::Log) {
                if (*axis == "x") result.aux |= 4;
                else if (*axis == "z") result.aux |= 8;
            } else if (wood == WoodVariantKind::ModernLog ||
                       wood == WoodVariantKind::ModernWood) {
                result.aux = static_cast<uint8_t>(*axis == "y" ? 0 : *axis == "x" ? 1 : 2);
            }
        } else if (wood == WoodVariantKind::Leaves || wood == WoodVariantKind::ModernLeaves ||
                   leaf == "mangrove_leaves" || leaf == "azalea_leaves" ||
                   leaf == "flowering_azalea_leaves") {
            if (!onlyProperties(properties, {"distance", "persistent", "waterlogged"}) ||
                !validBoolean(property(properties, "persistent")) ||
                !validBoolean(property(properties, "waterlogged"))) {
                return unsupported("unsupported leaves state properties");
            }
            uint32_t distance = 0;
            if (property(properties, "distance") &&
                (!parseUnsigned(property(properties, "distance"), 7, &distance) || distance == 0)) {
                return unsupported("leaves state has an invalid distance");
            }
            const bool modern_leaves = wood == WoodVariantKind::ModernLeaves ||
                leaf == "mangrove_leaves" || leaf == "azalea_leaves" ||
                leaf == "flowering_azalea_leaves";
            if (modern_leaves) {
                result.command_name = leaf == "flowering_azalea_leaves"
                    ? "minecraft:azalea_leaves_flowered" : "minecraft:" + std::string(leaf);
                result.aux = property(properties, "persistent") &&
                    *property(properties, "persistent") == "true" ? 2 : 0;
            } else if (property(properties, "persistent") &&
                       *property(properties, "persistent") == "true") {
                result.aux |= 4;
            }
            if (property(properties, "waterlogged") &&
                *property(properties, "waterlogged") == "true") {
                mapping_warning = "waterlogged leaves lose their contained fluid";
            }
        } else if (wood == WoodVariantKind::Planks) {
            if (!properties.empty()) return unsupported("unsupported planks state properties");
        } else if (splitColoredBlock(leaf, &result.command_name, &result.aux)) {
            if (!properties.empty()) return unsupported("unsupported colored-block state properties");
        } else if (leaf == "wool" || leaf == "concrete" || leaf == "concrete_powder" ||
                   leaf == "stained_hardened_clay" || leaf == "stained_glass" ||
                   leaf == "stained_glass_pane" || leaf == "carpet") {
            if (!onlyProperties(properties, {"color"})) {
                return unsupported("unsupported legacy colored-block state properties");
            }
            const std::string* color = property(properties, "color");
            const int color_data = color ? colorAux(*color) : 0;
            if (color_data < 0) return unsupported("colored block is missing a valid color");
            result.command_name.assign(identifier.data(), identifier.size());
            result.aux = static_cast<uint8_t>(color_data);
        } else if (endsWith(leaf, "_glazed_terracotta")) {
            if (!onlyProperties(properties, {"facing"})) {
                return unsupported("unsupported glazed-terracotta state properties");
            }
            const std::string* facing = property(properties, "facing");
            const int direction = facing ? horizontalIndex(*facing) : -1;
            if (direction < 0) return unsupported("glazed terracotta is missing a valid facing");
            result.command_name.assign(identifier.data(), identifier.size());
            if (leaf == "light_gray_glazed_terracotta") {
                result.command_name = "minecraft:silver_glazed_terracotta";
            }
            if (!isKnownLegacyCommand(result.command_name)) {
                return unsupported("unsupported glazed-terracotta color");
            }
            result.aux = static_cast<uint8_t>(direction);
        } else if (leaf == "mycelium") {
            if (!onlyProperties(properties, {"snowy"}) ||
                !validBoolean(property(properties, "snowy"))) {
                return unsupported("unsupported mycelium state properties");
            }
            result.command_name = "minecraft:mycelium";
        } else if (identifier == "minecraft:grass" || leaf == "grass_block") {
            if (!onlyProperties(properties, {"snowy"}) ||
                !validBoolean(property(properties, "snowy"))) {
                return unsupported("unsupported grass state properties");
            }
            result.command_name = "minecraft:grass_block";
        } else if (leaf == "nether_portal") {
            if (!onlyProperties(properties, {"axis"})) {
                return unsupported("unsupported nether portal state properties");
            }
            const std::string* axis = property(properties, "axis");
            if (axis && *axis != "x" && *axis != "z") {
                return unsupported("nether portal has an invalid axis");
            }
            // The target exposes the legacy portal block but has no verified
            // command data encoding for the Java x/z axis.  Preserve the
            // portal shell rather than aborting the complete schematic; the
            // game reconstructs the visible portal plane from adjacent blocks.
            result.command_name = "minecraft:portal";
            mapping_warning = "Java nether portal axis was reset to the target portal default";
        } else if (leaf == "jigsaw") {
            if (!onlyProperties(properties, {"orientation"})) {
                return unsupported("unsupported jigsaw state properties");
            }
            const std::string* orientation = property(properties, "orientation");
            static constexpr std::string_view kOrientations[] = {
                "down_east", "down_north", "down_south", "down_west",
                "up_east", "up_north", "up_south", "up_west",
                "west_up", "east_up", "north_up", "south_up",
            };
            if (orientation && std::find(std::begin(kOrientations),
                                         std::end(kOrientations),
                                         std::string_view(*orientation)) ==
                                    std::end(kOrientations)) {
                return unsupported("jigsaw has an invalid orientation");
            }
            // This target does not expose a jigsaw command identifier.  A
            // structure block is the closest safe, placeable counterpart and
            // retains the building geometry without executing source content.
            result.command_name = "minecraft:structure_block";
            stateful = true;
            force_no_fill = true;
            mapping_warning =
                "Java jigsaw was approximated with a structure block; orientation and generated content were ignored";
        } else if (leaf == "command_block" || leaf == "repeating_command_block" ||
                   leaf == "chain_command_block") {
            // Java/Litematic stores the command-block condition in its block
            // state, while the editable command data is emitted later as a
            // separate block entity.  Preserve the target's native six-way
            // facing plus conditional bit here so that final packet update can
            // use the same state rather than silently reset it.
            if (!onlyProperties(properties, {"facing", "conditional"}) ||
                !validBoolean(property(properties, "conditional"))) {
                return unsupported("unsupported command-block state properties");
            }
            const std::string* facing = property(properties, "facing");
            const int direction = facing ? sixWayFacingIndex(*facing) : 2;
            if (direction < 0) return unsupported("command block has an invalid facing");
            const bool conditional = property(properties, "conditional") &&
                *property(properties, "conditional") == "true";
            result.command_name.assign(identifier.data(), identifier.size());
            result.aux = static_cast<uint8_t>(direction | (conditional ? 0x08 : 0x00));
            stateful = true;
            force_no_fill = true;
        } else if (leaf == "quartz_pillar") {
            if (!onlyProperties(properties, {"axis"})) {
                return unsupported("unsupported quartz pillar state properties");
            }
            const std::string* axis = property(properties, "axis");
            if (axis && *axis != "x" && *axis != "y" && *axis != "z") {
                return unsupported("quartz pillar has an invalid axis");
            }
            result.command_name = "minecraft:quartz_block";
            // Bedrock packs chisel_type into the low two bits and pillar_axis
            // above it: lines/y=2, lines/x=6, and lines/z=10. Values 3/4 are
            // smooth/y and default/x rather than horizontal quartz pillars.
            result.aux = static_cast<uint8_t>(!axis || *axis == "y" ? 2 : *axis == "x" ? 6 : 10);
        } else if (mapFlatBlock(leaf, &result.command_name, &result.aux)) {
            if (!properties.empty()) return unsupported("unsupported flattened-block state properties");
        } else if (leaf == "smooth_stone" || leaf == "chiseled_sandstone" ||
                   leaf == "cut_sandstone" || leaf == "smooth_sandstone" ||
                   leaf == "chiseled_red_sandstone" ||
                   leaf == "cut_red_sandstone" || leaf == "smooth_red_sandstone" ||
                   leaf == "smooth_quartz" || leaf == "smooth_basalt" ||
                   leaf == "cobbled_deepslate" ||
                   leaf == "tuff" || leaf == "moss_block" || leaf == "calcite" ||
                   leaf == "packed_ice") {
            if (!properties.empty()) return unsupported("unsupported modern block state properties");
            result.command_name.assign(identifier.data(), identifier.size());
        } else {
            result.command_name.assign(identifier.data(), identifier.size());
            if (!properties.empty()) {
                if (!onlyKnownJavaStateProperties(properties)) {
                    return unsupported("unsupported state properties for block");
                }
                // The target registry knows this block, but the Java and
                // Bedrock state schemas are not identical. Keep the block
                // instead of aborting the entire structure, reset only the
                // untranslatable state, and surface the loss in parse stats.
                // Orientation-sensitive blocks are issued as individual
                // attachment commands so support ordering remains valid.
                const bool placement_sensitive =
                    property(properties, "facing") || property(properties, "face") ||
                    property(properties, "attachment") || property(properties, "half") ||
                    property(properties, "part") || property(properties, "rotation") ||
                    property(properties, "hanging") ||
                    endsWith(leaf, "_hanging_sign") ||
                    endsWith(leaf, "_wall_hanging_sign");
                if (placement_sensitive) {
                    forced_phase = ImportPhase::Attachment;
                    stateful = true;
                    force_no_fill = true;
                }
                mapping_warning =
                    "Java block-state properties are not available in the target registry and were reset";
            }
        }
    }

    const bool pale_oak_source = startsWith(leaf, "pale_oak_") ||
                                 startsWith(leaf, "stripped_pale_oak_");
    const bool exact_pale_oak_target =
        (startsWith(result.command_name, "minecraft:pale_oak_") ||
         startsWith(result.command_name, "minecraft:stripped_pale_oak_")) &&
        isKnownTargetCommand(result.command_name);
    if (!exact_pale_oak_target) {
        replacePrefix(&result.command_name, "minecraft:pale_oak_", "minecraft:birch_");
        replacePrefix(&result.command_name, "minecraft:stripped_pale_oak_",
                      "minecraft:stripped_birch_");
    }
    if (pale_oak_source && !exact_pale_oak_target) {
        appendWarning(&mapping_warning,
                      "pale oak is unavailable in the target registry and was approximated with birch");
    }
    // Every specialized mapping path must pass the same target-version gate.
    // This prevents a helper from silently emitting a Java-only identifier.
    if (!isKnownTargetCommand(result.command_name)) {
        return unsupported("block identifier has no target-version mapping");
    }

    result.phase = forced_phase ? *forced_phase : phaseFor(result.command_name);
    result.single_layer_only = result.phase != ImportPhase::Structure;
    result.stateful = stateful;
    result.can_fill = !force_no_fill && !stateful &&
                      result.phase != ImportPhase::Attachment &&
                      result.phase != ImportPhase::DependentAttachment;
    return {BlockMappingStatus::Mapped, std::move(result), std::move(mapping_warning)};
}

BlockMappingResult BlockMapper::mapBedrockState(std::string_view identifier,
                                                  std::string_view state,
                                                  uint16_t legacy_data,
                                                  bool has_legacy_aux) const {
    const auto unsupported = [&](std::string reason) {
        return BlockMappingResult{BlockMappingStatus::Unsupported, {}, std::move(reason)};
    };
    // Old BDX writers frequently store a bare Bedrock block leaf (for
    // example `stone` or `bedrock`) instead of the modern
    // `minecraft:stone` form.  BDX is the only input route that reaches this
    // mapper, and the value still has to pass the exact target-registry gate
    // below before it can become a command token.  Prefix only a syntactically
    // safe bare leaf; a foreign namespace or any punctuation/control content
    // remains rejected rather than being reinterpreted as a command name.
    std::string normalized_identifier;
    if (identifier.find(':') == std::string_view::npos) {
        if (identifier.empty() || identifier.size() > 128) {
            return unsupported("unsafe or non-minecraft BDX block identifier");
        }
        for (const char character : identifier) {
            const unsigned char ch = static_cast<unsigned char>(character);
            if (!(std::islower(ch) || std::isdigit(ch) || character == '_' ||
                  character == '-')) {
                return unsupported("unsafe or non-minecraft BDX block identifier");
            }
        }
        normalized_identifier = "minecraft:" + std::string(identifier);
        identifier = normalized_identifier;
    }
    if (!isSafeIdentifier(identifier) ||
        identifier.substr(0, identifier.find(':')) != "minecraft") {
        return unsupported("unsafe or non-minecraft BDX block identifier");
    }

    // Older Bedrock exporters spell the dark-oak family as `darkoak_*`,
    // whereas current Bedrock uses `dark_oak_*`.  Normalize before every
    // specialized mapping path, except for the target's two historical sign
    // IDs, which deliberately remain `darkoak_*` in its command registry.
    constexpr std::string_view kDarkOakPrefix = "minecraft:darkoak_";
    if (startsWith(identifier, kDarkOakPrefix) &&
        identifier != "minecraft:darkoak_wall_sign" &&
        identifier != "minecraft:darkoak_standing_sign") {
        normalized_identifier = "minecraft:dark_oak_" +
            std::string(identifier.substr(kDarkOakPrefix.size()));
        identifier = normalized_identifier;
    } else if (identifier == "minecraft:dark_oak_wall_sign") {
        normalized_identifier = "minecraft:darkoak_wall_sign";
        identifier = normalized_identifier;
    } else if (identifier == "minecraft:dark_oak_standing_sign") {
        normalized_identifier = "minecraft:darkoak_standing_sign";
        identifier = normalized_identifier;
    }

    Properties properties;
    if (state.size() > 4096 || !parseBedrockProperties(state, &properties)) {
        return unsupported("malformed BDX Bedrock block state properties");
    }

    const size_t separator = identifier.rfind(':');
    const std::string_view leaf = identifier.substr(separator + 1);
    const bool air = leaf == "air" || leaf == "cave_air" || leaf == "void_air";
    if (air) {
        if (!properties.empty()) return unsupported("BDX air block has state properties");
        return {BlockMappingStatus::Air, {}, {}};
    }

    const auto direct = [&](std::string command_name, uint16_t aux, bool stateful,
                            std::optional<ImportPhase> forced_phase = std::nullopt,
                            std::string warning = {}) {
        if (!isKnownTargetCommand(command_name)) {
            return unsupported("block identifier has no target-version mapping");
        }
        BlockSpec result;
        result.command_name = std::move(command_name);
        result.aux = aux;
        result.phase = forced_phase ? *forced_phase : phaseFor(result.command_name);
        result.single_layer_only = result.phase != ImportPhase::Structure;
        result.stateful = stateful;
        result.can_fill = !stateful && result.phase != ImportPhase::Attachment &&
            result.phase != ImportPhase::DependentAttachment;
        return BlockMappingResult{BlockMappingStatus::Mapped, std::move(result), std::move(warning)};
    };
    const auto map_canonical = [&](std::string_view canonical_identifier,
                                   std::string_view canonical_properties) {
        return mapSpongeState(makeCanonicalState(canonical_identifier, canonical_properties));
    };

    // NetEase 1.17 BDX pool 117 still contains several pre-flattening block
    // families as a bare name plus a native aux value.  Resolve those values
    // before the generic target-name fallback, which cannot infer their
    // material or half/state from the historical family identifier alone.
    if (properties.empty() && has_legacy_aux && leaf == "wood") {
        const uint16_t wood_type = legacy_data & 0x07U;
        const uint16_t facing_direction = legacy_data >> 3U;
        if (wood_type > 5U || facing_direction > 5U) {
            return unsupported("legacy BDX wood has an invalid type or facing direction");
        }
        // `wood` is the old all-bark family. The target encodes its six legacy
        // species through log/log2 and the all-bark bit (0x0c); the original
        // directional face is not visible once represented as a wood block.
        const bool old_log = wood_type < 4U;
        const uint16_t aux = static_cast<uint16_t>((old_log ? wood_type : wood_type - 4U) | 0x0CU);
        return direct(old_log ? "minecraft:log" : "minecraft:log2", aux, false);
    }

    if (properties.empty() && has_legacy_aux &&
        (leaf == "stone_slab3" || leaf == "stone_slab4")) {
        static constexpr std::array<std::string_view, 8> kSlab3Materials{{
            "end_stone_brick_slab", "smooth_red_sandstone_slab",
            "polished_andesite_slab", "andesite_slab", "diorite_slab",
            "polished_diorite_slab", "granite_slab", "polished_granite_slab",
        }};
        static constexpr std::array<std::string_view, 5> kSlab4Materials{{
            "mossy_stone_brick_slab", "smooth_quartz_slab", "stone_slab",
            "cut_sandstone_slab", "cut_red_sandstone_slab",
        }};
        const uint16_t material = legacy_data & 0x07U;
        if ((legacy_data & ~0x0FU) != 0U ||
            (leaf == "stone_slab3" && material >= kSlab3Materials.size()) ||
            (leaf == "stone_slab4" && material >= kSlab4Materials.size())) {
            return unsupported("legacy BDX stone slab has an invalid material or half");
        }
        const std::string_view target = leaf == "stone_slab3"
            ? kSlab3Materials[material] : kSlab4Materials[material];
        return map_canonical("minecraft:" + std::string(target),
                             std::string("type=") +
                              ((legacy_data & 0x08U) != 0U ? "top" : "bottom") +
                                  ",waterlogged=false");
    }

    if (properties.empty() && has_legacy_aux && leaf == "double_stone_slab4") {
        // Pre-flattening Bedrock used this name for a complete two-half slab.
        // Unlike stone_slab4, its data has no top-half bit: the target's
        // double_stone_block_slab4 only carries the material in bits 0..2.
        static constexpr std::array<std::string_view, 5> kSlab4Materials{{
            "mossy_stone_brick_slab", "smooth_quartz_slab", "stone_slab",
            "cut_sandstone_slab", "cut_red_sandstone_slab",
        }};
        const uint16_t material = legacy_data & 0x07U;
        if ((legacy_data & ~0x07U) != 0U || material >= kSlab4Materials.size()) {
            return unsupported("legacy BDX double stone slab has an invalid material or state");
        }
        return map_canonical("minecraft:" + std::string(kSlab4Materials[material]),
                             "type=double,waterlogged=false");
    }

    if (properties.empty() && has_legacy_aux && leaf == "shulker_box") {
        static constexpr std::array<std::string_view, 16> kShulkerCommands{{
            "minecraft:white_shulker_box", "minecraft:orange_shulker_box",
            "minecraft:magenta_shulker_box", "minecraft:light_blue_shulker_box",
            "minecraft:yellow_shulker_box", "minecraft:lime_shulker_box",
            "minecraft:pink_shulker_box", "minecraft:gray_shulker_box",
            "minecraft:silver_shulker_box", "minecraft:cyan_shulker_box",
            "minecraft:purple_shulker_box", "minecraft:blue_shulker_box",
            "minecraft:brown_shulker_box", "minecraft:green_shulker_box",
            "minecraft:red_shulker_box", "minecraft:black_shulker_box",
        }};
        if (legacy_data >= kShulkerCommands.size()) {
            return unsupported("legacy BDX shulker box has an invalid color");
        }
        // Old generic shulker_box data carries only its color. Its missing
        // facing state defaults to the same native down-facing value used by
        // the target command registry.
        return direct(std::string(kShulkerCommands[legacy_data]), 0, true);
    }

    if (properties.empty() && has_legacy_aux && leaf == "coral") {
        static constexpr std::array<std::string_view, 5> kCoralKinds{{
            "tube", "brain", "bubble", "fire", "horn",
        }};
        const uint16_t kind = legacy_data & 0x07U;
        if ((legacy_data & ~0x0FU) != 0U || kind >= kCoralKinds.size()) {
            return unsupported("legacy BDX coral has an invalid type or dead flag");
        }
        const bool dead = (legacy_data & 0x08U) != 0U;
        return direct("minecraft:" + std::string(dead ? "dead_" : "") +
                          std::string(kCoralKinds[kind]) + "_coral",
                       0, true);
    }

    if (properties.empty() && has_legacy_aux && leaf == "coral_block") {
        static constexpr std::array<std::string_view, 5> kCoralKinds{{
            "tube", "brain", "bubble", "fire", "horn",
        }};
        const uint16_t kind = legacy_data & 0x07U;
        if ((legacy_data & ~0x0FU) != 0U || kind >= kCoralKinds.size()) {
            return unsupported("legacy BDX coral block has an invalid type or dead flag");
        }
        const bool dead = (legacy_data & 0x08U) != 0U;
        // The old `coral_block` family is distinct from `coral` (the latter
        // maps to the non-block coral form). Keep that geometry distinction
        // while retaining the native dead-bit meaning.
        return direct("minecraft:" + std::string(dead ? "dead_" : "") +
                          std::string(kCoralKinds[kind]) + "_coral_block",
                      0, true);
    }

    // These two blocks existed only in very old Bedrock editions and still
    // appear in historical BDX exports.  The target registry has no exact
    // Nether Reactor Core and no glowing-obsidian ID.  Preserve the closest
    // safe visible material for the latter and skip the non-portable reactor
    // marker instead of aborting an otherwise importable command-block build.
    if (leaf == "glowing_obsidian") {
        return direct("minecraft:crying_obsidian", 0, false, std::nullopt,
                      "legacy glowing obsidian was approximated with crying obsidian");
    }
    if (leaf == "nether_reactor") {
        return {BlockMappingStatus::Air, {},
                "legacy Nether Reactor Core has no portable target equivalent and was skipped"};
    }

    const auto map_flattened_slab = [&](std::string_view source_leaf)
            -> std::optional<BlockMappingResult> {
        constexpr std::string_view kSlabSuffix = "_slab";
        if (!endsWith(source_leaf, kSlabSuffix) ||
            !onlyProperties(properties, {"type", "half", "slab_type", "vertical_half",
                                         "top_slot_bit", "waterlogged"}) ||
            !validBoolean(property(properties, "waterlogged"))) {
            return std::nullopt;
        }
        SlabPlacement placement = SlabPlacement::Bottom;
        if (!parseSlabPlacement(properties, true, true, &placement)) return std::nullopt;

        std::string single_leaf;
        if (flattenedDoubleSlabSingleLeaf(source_leaf, &single_leaf)) {
            placement = SlabPlacement::Double;
        } else {
            single_leaf.assign(source_leaf);
        }
        const char* type = placement == SlabPlacement::Double ? "double" :
            placement == SlabPlacement::Top ? "top" : "bottom";
        BlockMappingResult mapped = map_canonical("minecraft:" + single_leaf,
            std::string("type=") + type + ",waterlogged=false");
        if (!mapped.isMapped()) return std::nullopt;
        return mapped;
    };
    const auto number = [&](std::string_view name, uint32_t maximum, uint32_t* output) {
        const std::string* value = property(properties, name);
        if (parseUnsigned(value, maximum, output)) return true;
        // Modern Bedrock world palettes commonly encode the six-way facing as
        // a string (`north`, `west`, …), while older BDX streams use its
        // numeric representation.  Accept both at the common conversion
        // boundary so observers, dispensers, pistons and command blocks do
        // not reject an entire mcworld merely because of that serialization
        // difference.
        if ((name != "facing_direction" && name != "cardinal_direction") ||
            !value || !output) return false;
        if (name == "facing_direction") {
            struct Facing { std::string_view name; uint32_t value; };
            static constexpr Facing kFacing[] = {
                {"down", 0}, {"up", 1}, {"north", 2},
                {"south", 3}, {"west", 4}, {"east", 5},
            };
            for (const Facing& facing : kFacing) {
                if (*value != facing.name) continue;
                if (facing.value > maximum) return false;
                *output = facing.value;
                return true;
            }
        }
        if (name == "cardinal_direction") {
            struct Cardinal { std::string_view name; uint32_t value; };
            // Bedrock door/trapdoor cardinal_direction uses the same numeric
            // order as the old `direction` property: east, south, west, north.
            static constexpr Cardinal kCardinal[] = {
                {"east", 0}, {"south", 1}, {"west", 2}, {"north", 3},
            };
            for (const Cardinal& cardinal : kCardinal) {
                if (*value != cardinal.name) continue;
                if (cardinal.value > maximum) return false;
                *output = cardinal.value;
                return true;
            }
        }
        return false;
    };
    const auto boolean = [&](std::string_view name, bool default_value, bool* output) {
        const std::string* value = property(properties, name);
        if (!value) {
            *output = default_value;
            return true;
        }
        if (*value == "true") {
            *output = true;
            return true;
        }
        if (*value == "false") {
            *output = false;
            return true;
        }
        return false;
    };
    const auto horizontalDirection = [&](uint32_t* output) {
        const bool has_legacy = property(properties, "direction") != nullptr;
        const bool has_cardinal = property(properties, "cardinal_direction") != nullptr;
        if (has_legacy == has_cardinal) return false;
        return has_legacy ? number("direction", 3, output)
                          : number("cardinal_direction", 3, output);
    };

    if (endsWith(leaf, "_hanging_sign") && !properties.empty()) {
        if (!onlyProperties(properties, {"attached_bit", "facing_direction",
                                         "ground_sign_direction", "hanging"})) {
            return unsupported("unsupported Bedrock hanging-sign state properties");
        }
        uint32_t facing = legacy_data & 0x07U;
        uint32_t rotation = (legacy_data >> 3U) & 0x0fU;
        bool hanging = (legacy_data & 0x0080U) != 0U;
        bool attached = (legacy_data & 0x0100U) != 0U;
        if ((property(properties, "facing_direction") &&
             !number("facing_direction", 5, &facing)) ||
            (property(properties, "ground_sign_direction") &&
             !number("ground_sign_direction", 15, &rotation)) ||
            !boolean("hanging", hanging, &hanging) ||
            !boolean("attached_bit", attached, &attached)) {
            return unsupported("Bedrock hanging sign has an invalid state value");
        }
        const uint16_t aux = static_cast<uint16_t>(
            facing | (rotation << 3U) | (hanging ? 0x0080U : 0U) |
            (attached ? 0x0100U : 0U));
        return direct(std::string(identifier), aux, true);
    }

    // BDX records command text separately from palette states. This mapper
    // preserves only the command-block shell and its orientation; the parser
    // routes the matching command payload into a dedicated deferred sidecar.
    // Command blocks must never be greedily merged by /fill because adjacent
    // shells may carry different data that is written after placement.
    if (leaf == "command_block" || leaf == "repeating_command_block" ||
        leaf == "chain_command_block") {
        if (!onlyProperties(properties, {"facing_direction", "conditional_bit"})) {
            return unsupported("unsupported BDX command-block state properties");
        }
        uint32_t facing = legacy_data & 0x07U;
        if (property(properties, "facing_direction") && !number("facing_direction", 5, &facing)) {
            return unsupported("BDX command block has an invalid facing direction");
        }
        // Runtime-ID BDX palettes store command-block orientation and its
        // conditional bit in legacy_data, without a textual state array.
        // An explicit Bedrock state wins when present; otherwise preserve bit
        // 0x08 instead of silently turning a conditional shell into a normal
        // one during mapping.
        bool conditional = has_legacy_aux && (legacy_data & 0x08U) != 0U;
        if (!boolean("conditional_bit", conditional, &conditional)) {
            return unsupported("BDX command block has an invalid conditional flag");
        }
        return direct(std::string(identifier), static_cast<uint8_t>(facing | (conditional ? 8 : 0)),
                      true);
    }

    // This old target command registry has no stable invisible-light block.
    // Skipping it is preferable to emitting a visible placeholder.
    if (leaf == "light_block" || startsWith(leaf, "light_block_")) {
        uint32_t ignored = 0;
        if (!onlyProperties(properties, {"block_light_level"}) ||
            (property(properties, "block_light_level") &&
             !number("block_light_level", 15, &ignored))) {
            return unsupported("BDX light block has an invalid light level");
        }
        return {BlockMappingStatus::Air, {},
                "BDX invisible light block has no target equivalent and was skipped"};
    }

    // `structure_void` is deliberately invisible in the source world.  It
    // carries no visible geometry and the legacy target has no safe command
    // identity for it, so omit it rather than aborting a whole mcworld import.
    if (leaf == "structure_void") {
        return {BlockMappingStatus::Air, {},
                "Bedrock structure void has no target equivalent and was skipped"};
    }

    // Bedrock saved worlds and older BDX writers can retain editor permission
    // markers (`allow`, `deny`, and `border_block`).  The target command
    // registry deliberately has no equivalent.  These do not contribute any
    // portable build geometry, so treating them as air is safer than rejecting
    // an otherwise portable import or substituting an arbitrary visible block.
    if (leaf == "allow" || leaf == "deny" || leaf == "border_block") {
        return {BlockMappingStatus::Air, {},
                "Bedrock editor permission block has no target equivalent and was skipped"};
    }

    // A saved Bedrock world stores a jigsaw's direction and rotation as native
    // numeric properties, rather than Java's single orientation string.  The
    // importer has no block-entity/NBT write channel, so retain only a
    // stateful, non-mergeable structure-block shell.  This intentionally
    // discards generated-pool data and cannot execute source content.
    if (leaf == "jigsaw") {
        if (!onlyProperties(properties, {"facing_direction", "rotation", "joint", "orientation"})) {
            return unsupported("unsupported Bedrock jigsaw state properties");
        }
        uint32_t ignored = 0;
        if ((property(properties, "facing_direction") &&
             !number("facing_direction", 5, &ignored)) ||
            (property(properties, "rotation") && !number("rotation", 3, &ignored))) {
            return unsupported("Bedrock jigsaw has invalid numeric state");
        }
        return direct("minecraft:structure_block", 0, true, ImportPhase::Structure,
                      "Bedrock jigsaw was approximated with a structure block; "
                      "orientation and generated content were ignored");
    }

    // Bedrock uses nether_portal with an axis state while this target exposes
    // the legacy portal identifier.  It is kept as an unmergeable shell so
    // the directional state cannot turn a greedy /fill into a wrong portal.
    if (leaf == "nether_portal") {
        if (!onlyProperties(properties, {"axis"})) {
            return unsupported("unsupported Bedrock nether-portal state properties");
        }
        const std::string* axis = property(properties, "axis");
        if (axis && *axis != "x" && *axis != "z") {
            return unsupported("Bedrock nether portal has an invalid axis");
        }
        return direct("minecraft:portal", 0, true, std::nullopt,
                      "Bedrock nether-portal axis was reset for the target registry");
    }

    // A legacy BDX placement carries an auxiliary value even when it is zero.
    // It must take precedence over the flattened-name converter below: a
    // native stone_slab data=0 is the target's smooth-stone lower slab, not
    // Java's flattened stone_slab material.
    if (properties.empty() && (has_legacy_aux || legacy_data != 0) &&
        isKnownTargetCommand(identifier)) {
        std::optional<ImportPhase> forced_phase;
        if ((isBed(leaf) || isDoor(leaf) || leaf == "double_plant") &&
            (legacy_data & 0x08U) != 0) {
            forced_phase = ImportPhase::DependentAttachment;
        }
        return direct(std::string(identifier), legacy_data,
                      isBedrockFallbackStateful(leaf), forced_phase);
    }

    // Flattened Bedrock palettes may omit a default half state entirely, and
    // double slabs can be named `*_double_slab`. Try the normalized form
    // before the generic state-less fallback loses the target's slab metadata.
    if (const std::optional<BlockMappingResult> slab = map_flattened_slab(leaf)) {
        return *slab;
    }

    if (properties.empty()) {
        // Legacy BDX placement opcodes carry the native auxiliary value beside
        // a state-less palette name. The explicit legacy path above has
        // already preserved such records, including data=0 lower slabs.
        BlockMappingResult plain = mapSpongeState(identifier);
        if (plain.isMapped() || plain.status == BlockMappingStatus::Air) return plain;
        if (isKnownTargetCommand(identifier)) {
            return direct(std::string(identifier), legacy_data,
                          isBedrockFallbackStateful(leaf));
        }
        return plain;
    }

    if (leaf == "sea_pickle") {
        if (!onlyProperties(properties, {"cluster_count", "dead_bit"}) ||
            !property(properties, "cluster_count")) {
            return unsupported("unsupported Bedrock sea-pickle state properties");
        }
        uint32_t cluster_count = 0;
        bool dead = false;
        if (!number("cluster_count", 3, &cluster_count) ||
            !boolean("dead_bit", false, &dead)) {
            return unsupported("Bedrock sea pickle has an invalid state");
        }
        return direct("minecraft:sea_pickle",
                      static_cast<uint8_t>(cluster_count | (dead ? 0x04U : 0U)),
                      true, ImportPhase::Attachment);
    }

    // Bedrock uses one shulker identifier plus a color state; the target uses
    // color-specific identifiers.  Its container contents are intentionally
    // not part of this mapper or BDX import path.
    if (leaf == "shulker_box") {
        if (!onlyProperties(properties, {"color", "facing_direction"})) {
            return unsupported("unsupported BDX shulker-box state properties");
        }
        const std::string* color = property(properties, "color");
        std::string command_name = "minecraft:undyed_shulker_box";
        if (color) {
            if (colorAux(*color) < 0) return unsupported("BDX shulker box has an invalid color");
            const std::string target_color = *color == "light_gray" ? "silver" : *color;
            command_name = "minecraft:" + std::string(target_color) + "_shulker_box";
        }
        uint32_t facing = legacy_data & 0x07U;
        if (property(properties, "facing_direction") && !number("facing_direction", 5, &facing)) {
            return unsupported("BDX shulker box has an invalid facing direction");
        }
        return direct(std::move(command_name), static_cast<uint8_t>(facing), true);
    }

    if (leaf == "wool" || leaf == "concrete" || leaf == "concrete_powder" ||
        leaf == "stained_hardened_clay" || leaf == "stained_glass" ||
        leaf == "stained_glass_pane" || leaf == "carpet") {
        if (!onlyProperties(properties, {"color"}) || !property(properties, "color") ||
            colorAux(*property(properties, "color")) < 0) {
            return unsupported("BDX colored block has an invalid color state");
        }
        return map_canonical(identifier, "color=" + *property(properties, "color"));
    }

    if (endsWith(leaf, "_stairs") || leaf == "normal_stone_stairs") {
        if (!onlyProperties(properties, {"upside_down_bit", "weirdo_direction"})) {
            return unsupported("unsupported BDX stair state properties");
        }
        uint32_t direction = 0;
        bool upside_down = false;
        if (!number("weirdo_direction", 3, &direction) ||
            !boolean("upside_down_bit", false, &upside_down)) {
            return unsupported("BDX stair has an invalid orientation");
        }
        const char* facing = bedrockStairFacing(direction);
        return map_canonical(identifier, std::string("facing=") + facing +
            ",half=" + (upside_down ? "top" : "bottom") +
            ",shape=straight,waterlogged=false");
    }

    if (isDoor(leaf)) {
        if (!onlyProperties(properties, {"direction", "cardinal_direction", "door_hinge_bit",
                                         "open_bit", "upper_block_bit"})) {
            return unsupported("unsupported BDX door state properties");
        }
        uint32_t direction = 0;
        bool hinge = false;
        bool open = false;
        bool upper = false;
        if (!horizontalDirection(&direction) || !boolean("door_hinge_bit", false, &hinge) ||
            !boolean("open_bit", false, &open) || !boolean("upper_block_bit", false, &upper)) {
            return unsupported("BDX door has an invalid state");
        }
        const char* facing = bedrockDoorFacing(direction);
        return map_canonical(identifier, std::string("facing=") + facing +
            ",half=" + (upper ? "upper" : "lower") +
            ",hinge=" + (hinge ? "right" : "left") +
            ",open=" + (open ? "true" : "false") + ",powered=false");
    }

    if (leaf == "trapdoor" || endsWith(leaf, "_trapdoor")) {
        if (!onlyProperties(properties, {"direction", "cardinal_direction", "open_bit",
                                         "upside_down_bit"})) {
            return unsupported("unsupported BDX trapdoor state properties");
        }
        uint32_t direction = 0;
        bool open = false;
        bool upside_down = false;
        if (!horizontalDirection(&direction) || !boolean("open_bit", false, &open) ||
            !boolean("upside_down_bit", false, &upside_down)) {
            return unsupported("BDX trapdoor has an invalid state");
        }
        std::string command_name = leaf == "oak_trapdoor" ? "minecraft:trapdoor" :
                                                             std::string(identifier);
        const uint8_t aux = static_cast<uint8_t>(direction | (upside_down ? 4 : 0) |
                                                 (open ? 8 : 0));
        return direct(std::move(command_name), aux, true, ImportPhase::Attachment);
    }

    if (leaf == "double_plant") {
        if (!onlyProperties(properties, {"double_plant_type", "upper_block_bit"}) ||
            !property(properties, "double_plant_type")) {
            return unsupported("unsupported BDX double-plant state properties");
        }
        struct Plant { std::string_view name; uint8_t data; };
        static constexpr Plant kPlants[] = {
            {"sunflower", 0}, {"syringa", 1}, {"grass", 2},
            {"fern", 3}, {"rose", 4}, {"paeonia", 5},
        };
        uint8_t data = 0;
        bool found = false;
        for (const Plant& plant : kPlants) {
            if (*property(properties, "double_plant_type") == plant.name) {
                data = plant.data;
                found = true;
                break;
            }
        }
        bool upper = false;
        if (!found || !boolean("upper_block_bit", false, &upper)) {
            return unsupported("BDX double plant has an invalid state");
        }
        if (upper) data |= 8;
        return direct("minecraft:double_plant", data, true,
                      upper ? ImportPhase::DependentAttachment : ImportPhase::Attachment);
    }

    // Bedrock has used both a boolean `top_slot_bit` and the newer textual
    // `vertical_half` for the same slab half.  Saved mcworld palettes in the
    // wild contain both forms, so normalize them before mapping to the target
    // command's type metadata.
    const auto slabTop = [&](bool* top) {
        if (!top) return false;
        SlabPlacement placement = SlabPlacement::Bottom;
        if (!parseSlabPlacement(properties, false, false, &placement) ||
            placement == SlabPlacement::Double) {
            return false;
        }
        *top = placement == SlabPlacement::Top;
        return true;
    };

    // Bedrock 1.20+ may serialize the smooth-stone family as
    // `normal_stone_slab` instead of a generic stone_block_slab plus a type
    // property.  The old target exposes that material as stone_slab aux 0,
    // which is already covered by the canonical smooth-stone conversion.
    const bool normal_stone_double_slab = leaf == "normal_stone_double_slab" ||
        leaf == "normal_stone_slab_double";
    if (leaf == "normal_stone_slab" || normal_stone_double_slab) {
        if (!onlyProperties(properties, {"half", "top_slot_bit", "vertical_half"})) {
            return unsupported("unsupported Bedrock normal-stone slab state properties");
        }
        bool top = false;
        if (!slabTop(&top)) return unsupported("Bedrock normal-stone slab has an invalid half");
        return map_canonical("minecraft:smooth_stone_slab", std::string("type=") +
            (normal_stone_double_slab ? "double" : top ? "top" : "bottom") +
            ",waterlogged=false");
    }

    // Pre-flattening Bedrock spelled these families `stone_slab` /
    // `double_stone_slab` (with an optional 2..4 suffix); current Bedrock uses
    // `stone_block_slab`.  Accept the historical spelling so a modern state
    // array keeps its material and half.  Without this the name only matches
    // the legacy command table, and the generic shell fallback silently resets
    // a cobblestone slab to the family's first material.  Only the state-array
    // form is remapped: the bare-name-plus-aux spellings carry no properties
    // and are decoded by the legacy branches above.
    const bool double_slab = startsWith(leaf, "double_stone_block_slab") ||
        (!properties.empty() && startsWith(leaf, "double_stone_slab"));
    std::string_view slab_family = double_slab ? leaf.substr(7) : leaf;
    std::string normalized_slab_family;
    constexpr std::string_view kLegacySlabFamily = "stone_slab";
    if (!properties.empty() && startsWith(slab_family, kLegacySlabFamily)) {
        normalized_slab_family = "stone_block_slab";
        normalized_slab_family.append(slab_family.substr(kLegacySlabFamily.size()));
        slab_family = normalized_slab_family;
    }
    if (slab_family == "stone_block_slab" || slab_family == "stone_block_slab2" ||
        slab_family == "stone_block_slab3" || slab_family == "stone_block_slab4") {
        const std::string_view type_key = slab_family == "stone_block_slab" ? "stone_slab_type" :
            slab_family == "stone_block_slab2" ? "stone_slab_type_2" :
            slab_family == "stone_block_slab3" ? "stone_slab_type_3" : "stone_slab_type_4";
        if (!onlyProperties(properties, {type_key, "half", "top_slot_bit", "vertical_half"}) ||
            !property(properties, type_key)) {
            return unsupported("unsupported BDX stone-slab state properties");
        }
        const std::string_view source = bedrockStoneSlabSource(slab_family, *property(properties, type_key));
        bool top = false;
        if (source.empty() || !slabTop(&top)) {
            return unsupported("BDX stone slab has an invalid type or half");
        }
        const std::string source_identifier = "minecraft:" + std::string(source);
        return map_canonical(source_identifier, std::string("type=") +
            (double_slab ? "double" : top ? "top" : "bottom") + ",waterlogged=false");
    }

    const bool double_wood_slab = leaf == "double_wooden_slab";
    if (leaf == "wooden_slab" || double_wood_slab) {
        if (!onlyProperties(properties, {"wood_type", "half", "top_slot_bit", "vertical_half"}) ||
            !property(properties, "wood_type")) {
            return unsupported("unsupported BDX wooden-slab state properties");
        }
        const std::string_view wood = bedrockWoodType(*property(properties, "wood_type"));
        bool top = false;
        if (wood.empty() || !slabTop(&top)) {
            return unsupported("BDX wooden slab has an invalid type or half");
        }
        const std::string source_identifier = "minecraft:" + std::string(wood) + "_slab";
        return map_canonical(source_identifier, std::string("type=") +
            (double_wood_slab ? "double" : top ? "top" : "bottom") + ",waterlogged=false");
    }

    if (endsWith(leaf, "_slab") &&
        (property(properties, "type") || property(properties, "half") ||
         property(properties, "slab_type") || property(properties, "top_slot_bit") ||
         property(properties, "vertical_half"))) {
        return unsupported("BDX slab has an unsupported material or half state");
    }

    if (leaf == "dirt") {
        if (!onlyProperties(properties, {"dirt_type"}) || !property(properties, "dirt_type")) {
            return unsupported("unsupported BDX dirt state properties");
        }
        const std::string& type = *property(properties, "dirt_type");
        if (type == "normal") return mapSpongeState("minecraft:dirt");
        if (type == "coarse") return mapSpongeState("minecraft:coarse_dirt");
        if (type == "podzol") return mapSpongeState("minecraft:podzol");
        return unsupported("BDX dirt has an unsupported dirt type");
    }

    if (leaf == "stone") {
        if (!onlyProperties(properties, {"stone_type"}) || !property(properties, "stone_type")) {
            return unsupported("unsupported BDX stone state properties");
        }
        const std::string& type = *property(properties, "stone_type");
        static constexpr std::pair<std::string_view, std::string_view> kStone[] = {
            {"stone", "stone"}, {"granite", "granite"}, {"granite_smooth", "polished_granite"},
            {"diorite", "diorite"}, {"diorite_smooth", "polished_diorite"},
            {"andesite", "andesite"}, {"andesite_smooth", "polished_andesite"},
        };
        for (const auto& entry : kStone) {
            if (type == entry.first) return mapSpongeState("minecraft:" + std::string(entry.second));
        }
        return unsupported("BDX stone has an unsupported stone type");
    }

    if (leaf == "sand") {
        if (!onlyProperties(properties, {"sand_type"}) || !property(properties, "sand_type")) {
            return unsupported("unsupported BDX sand state properties");
        }
        if (*property(properties, "sand_type") == "normal") return mapSpongeState("minecraft:sand");
        if (*property(properties, "sand_type") == "red") return mapSpongeState("minecraft:red_sand");
        return unsupported("BDX sand has an unsupported sand type");
    }

    if (leaf == "sandstone" || leaf == "red_sandstone") {
        if (!onlyProperties(properties, {"sand_stone_type"}) || !property(properties, "sand_stone_type")) {
            return unsupported("unsupported BDX sandstone state properties");
        }
        const std::string& type = *property(properties, "sand_stone_type");
        const std::string prefix = leaf == "sandstone" ? "" : "red_";
        if (type == "default") return mapSpongeState("minecraft:" + prefix + "sandstone");
        if (type == "smooth") return mapSpongeState("minecraft:smooth_" + prefix + "sandstone");
        if (type == "cut") return mapSpongeState("minecraft:cut_" + prefix + "sandstone");
        if (type == "heiroglyphs" || type == "chiseled") {
            return mapSpongeState("minecraft:chiseled_" + prefix + "sandstone");
        }
        return unsupported("BDX sandstone has an unsupported sandstone type");
    }

    // The all-bark `wood` family carries its species in a property rather than
    // in the name.  Its bare-name-plus-aux form is decoded further above; this
    // is the modern state-array form.  `wood` is four characters, so it never
    // matches the `_wood` pillar rule below, and the name is in neither the
    // legacy command table nor the target registry — without this branch the
    // whole import fails on an ordinary acacia/spruce wood block.
    if (leaf == "wood") {
        if (!onlyProperties(properties, {"wood_type", "pillar_axis", "stripped_bit"}) ||
            !property(properties, "wood_type")) {
            return unsupported("unsupported BDX wood state properties");
        }
        const std::string_view wood = bedrockWoodType(*property(properties, "wood_type"));
        if (wood.empty()) return unsupported("BDX wood variant is unsupported");
        bool stripped = false;
        if (!boolean("stripped_bit", false, &stripped)) {
            return unsupported("BDX wood has an invalid stripped flag");
        }
        std::string axis = "y";
        if (const std::string* pillar_axis = property(properties, "pillar_axis")) {
            if (*pillar_axis != "x" && *pillar_axis != "y" && *pillar_axis != "z") {
                return unsupported("BDX wood has an invalid pillar axis");
            }
            axis = *pillar_axis;
        }
        const std::string identifier_name = std::string("minecraft:") +
            (stripped ? "stripped_" : "") + std::string(wood) + "_wood";
        return map_canonical(identifier_name, "axis=" + axis);
    }

    if (leaf == "planks" || leaf == "fence") {
        // planks always uses "wood_type".  fence used "old_log_type" in older
        // Bedrock exporters, but modern BDX writers send "wood_type" instead.
        // Accept both so a normal oak or spruce fence does not abort the import.
        const bool fence_has_wood_type =
            leaf == "fence" && property(properties, "wood_type") != nullptr;
        const std::string_view key =
            (leaf == "planks" || fence_has_wood_type) ? "wood_type" : "old_log_type";
        if (!onlyProperties(properties, {"wood_type", "old_log_type"}) ||
            !property(properties, key)) {
            return unsupported("unsupported BDX wood-variant state properties");
        }
        const std::string_view wood = bedrockWoodType(*property(properties, key));
        if (wood.empty()) return unsupported("BDX wood variant is unsupported");
        return mapSpongeState("minecraft:" + std::string(wood) +
                              (leaf == "planks" ? "_planks" : "_fence"));
    }

    if (leaf == "log" || leaf == "log2") {
        const std::string_view type_key = leaf == "log" ? "old_log_type" : "new_log_type";
        if (!onlyProperties(properties, {type_key, "pillar_axis"}) || !property(properties, type_key)) {
            return unsupported("unsupported BDX log state properties");
        }
        uint8_t data = 0;
        const std::string& type = *property(properties, type_key);
        if (leaf == "log") {
            if (type == "oak" || type == "oka") data = 0;
            else if (type == "spruce") data = 1;
            else if (type == "birch") data = 2;
            else if (type == "jungle") data = 3;
            else return unsupported("BDX old log has an unsupported wood type");
        } else {
            if (type == "acacia") data = 0;
            else if (type == "dark_oak") data = 1;
            else return unsupported("BDX new log has an unsupported wood type");
        }
        if (const std::string* axis = property(properties, "pillar_axis")) {
            if (*axis == "x") data |= 4;
            else if (*axis == "z") data |= 8;
            else if (*axis != "y") return unsupported("BDX log has an invalid pillar axis");
        }
        return direct("minecraft:" + std::string(leaf), data, false);
    }

    if (endsWith(leaf, "_log") || endsWith(leaf, "_wood") || leaf == "chain" ||
        leaf == "deepslate") {
        if (onlyProperties(properties, {"pillar_axis"}) && property(properties, "pillar_axis")) {
            const std::string& axis = *property(properties, "pillar_axis");
            if (axis != "x" && axis != "y" && axis != "z") {
                return unsupported("BDX pillar has an invalid axis");
            }
            return map_canonical(identifier, "axis=" + axis);
        }
    }

    if (leaf == "leaves" || leaf == "leaves2") {
        const std::string_view type_key = leaf == "leaves" ? "old_leaf_type" : "new_leaf_type";
        if (!onlyProperties(properties, {type_key, "persistent_bit", "update_bit"}) ||
            !property(properties, type_key)) {
            return unsupported("unsupported BDX leaves state properties");
        }
        uint8_t data = 0;
        const std::string& type = *property(properties, type_key);
        if (leaf == "leaves") {
            if (type == "oak" || type == "oka") data = 0;
            else if (type == "spruce") data = 1;
            else if (type == "birch") data = 2;
            else if (type == "jungle") data = 3;
            else return unsupported("BDX old leaves have an unsupported type");
        } else {
            if (type == "acacia") data = 0;
            else if (type == "dark_oak") data = 1;
            else return unsupported("BDX new leaves have an unsupported type");
        }
        bool persistent = false;
        bool update = false;
        if (!boolean("persistent_bit", false, &persistent) ||
            !boolean("update_bit", false, &update)) {
            return unsupported("BDX leaves have an invalid persistence state");
        }
        // `update_bit` is a simulation hint; it does not change the target
        // visual state.
        (void)update;
        if (persistent) data |= 4;
        return direct("minecraft:" + std::string(leaf), data, false);
    }

    if (leaf == "quartz_block") {
        if (!onlyProperties(properties, {"chisel_type", "pillar_axis"}) ||
            !property(properties, "chisel_type")) {
            return unsupported("unsupported BDX quartz state properties");
        }
        const std::string& type = *property(properties, "chisel_type");
        uint8_t data = 0;
        if (type == "default") data = 0;
        else if (type == "chiseled") data = 1;
        else if (type == "smooth") data = 3;
        else if (type == "lines") {
            const std::string* axis = property(properties, "pillar_axis");
            if (!axis || *axis == "y") data = 2;
            else if (*axis == "x") data = 6;
            else if (*axis == "z") data = 10;
            else return unsupported("BDX quartz pillar has an invalid axis");
        } else {
            return unsupported("BDX quartz has an unsupported chisel type");
        }
        return direct("minecraft:quartz_block", data, false);
    }

    if (leaf == "cobblestone_wall" && property(properties, "wall_block_type")) {
        static constexpr std::pair<std::string_view, std::string_view> kWalls[] = {
            {"cobblestone", "cobblestone_wall"}, {"mossy_cobblestone", "mossy_cobblestone_wall"},
            {"stone_brick", "stone_brick_wall"}, {"mossy_stone_brick", "mossy_stone_brick_wall"},
            {"andesite", "andesite_wall"}, {"diorite", "diorite_wall"},
            {"granite", "granite_wall"}, {"sandstone", "sandstone_wall"},
            {"red_sandstone", "red_sandstone_wall"}, {"nether_brick", "nether_brick_wall"},
            // Older Bedrock palettes store every wall material under the
            // generic cobblestone_wall identifier.  Keep the material family
            // rather than treating the modern connection properties as an
            // unsupported block and aborting an otherwise valid mcworld.
            {"brick", "brick_wall"}, {"mud_brick", "mud_brick_wall"},
            {"blackstone", "blackstone_wall"},
            {"polished_blackstone", "polished_blackstone_wall"},
            {"polished_blackstone_brick", "polished_blackstone_brick_wall"},
            {"cobbled_deepslate", "cobbled_deepslate_wall"},
            {"deepslate_brick", "deepslate_brick_wall"},
            {"deepslate_tile", "deepslate_tile_wall"},
            {"polished_deepslate", "polished_deepslate_wall"},
            {"red_nether_brick", "red_nether_brick_wall"},
            {"prismarine", "prismarine_wall"}, {"tuff", "tuff_wall"},
            {"end_stone_brick", "end_stone_brick_wall"},
        };
        for (const auto& entry : kWalls) {
            if (*property(properties, "wall_block_type") == entry.first) {
                return mapSpongeState("minecraft:" + std::string(entry.second));
            }
        }
        return unsupported("BDX wall has an unsupported wall type");
    }

    if (leaf == "red_flower") {
        if (!onlyProperties(properties, {"flower_type"}) || !property(properties, "flower_type")) {
            return unsupported("unsupported BDX flower state properties");
        }
        static constexpr std::string_view kFlowers[] = {
            "poppy", "blue_orchid", "allium", "houstonia", "red_tulip", "orange_tulip",
            "white_tulip", "pink_tulip", "oxeye_daisy", "cornflower", "lily_of_the_valley",
        };
        for (const std::string_view flower : kFlowers) {
            if (*property(properties, "flower_type") == flower) {
                const std::string_view source = flower == "houstonia" ? "azure_bluet" : flower;
                return mapSpongeState("minecraft:" + std::string(source));
            }
        }
        return unsupported("BDX flower has an unsupported flower type");
    }

    if (leaf == "tallgrass") {
        if (!onlyProperties(properties, {"tall_grass_type"}) || !property(properties, "tall_grass_type")) {
            return unsupported("unsupported BDX tallgrass state properties");
        }
        if (*property(properties, "tall_grass_type") == "tall") return mapSpongeState("minecraft:grass");
        if (*property(properties, "tall_grass_type") == "fern") return mapSpongeState("minecraft:fern");
        return unsupported("BDX tallgrass has an unsupported type");
    }

    if (leaf == "water" || leaf == "flowing_water" || leaf == "lava" || leaf == "flowing_lava") {
        if (!onlyProperties(properties, {"liquid_depth"}) || !property(properties, "liquid_depth")) {
            return unsupported("unsupported BDX fluid state properties");
        }
        uint32_t depth = 0;
        if (!number("liquid_depth", 15, &depth)) return unsupported("BDX fluid has an invalid depth");
        return map_canonical(identifier, "level=" + std::to_string(depth));
    }

    if (leaf == "composter") {
        if (!onlyProperties(properties, {"composter_fill_level"}) ||
            !property(properties, "composter_fill_level")) {
            return unsupported("unsupported BDX composter state properties");
        }
        uint32_t level = 0;
        if (!number("composter_fill_level", 8, &level)) {
            return unsupported("BDX composter has an invalid fill level");
        }
        return map_canonical(identifier, "level=" + std::to_string(level));
    }

    if (property(properties, "facing_direction")) {
        uint32_t direction = 0;
        if (!number("facing_direction", 5, &direction)) {
            return unsupported("BDX block has an invalid facing direction");
        }
        const char* facing = bedrockSixWayFacing(direction);
        const std::string canonical_facing = std::string("facing=") + facing;
        if (leaf == "end_rod") return map_canonical(identifier, canonical_facing);
        if (leaf == "observer") {
            bool powered = false;
            if (!onlyProperties(properties, {"facing_direction", "powered_bit"}) ||
                !boolean("powered_bit", false, &powered)) {
                return unsupported("unsupported BDX observer state properties");
            }
            return map_canonical(identifier, canonical_facing + ",powered=" +
                                 (powered ? "true" : "false"));
        }
        if (leaf == "hopper") {
            bool disabled = false;
            if (!onlyProperties(properties, {"facing_direction", "toggle_bit"}) ||
                !boolean("toggle_bit", false, &disabled)) {
                return unsupported("unsupported BDX hopper state properties");
            }
            return map_canonical(identifier, canonical_facing + ",enabled=" +
                                 (disabled ? "false" : "true"));
        }
        if (leaf == "barrel") {
            bool open = false;
            if (!onlyProperties(properties, {"facing_direction", "open_bit"}) ||
                !boolean("open_bit", false, &open)) {
                return unsupported("unsupported BDX barrel state properties");
            }
            return map_canonical(identifier, canonical_facing + ",open=" +
                                 (open ? "true" : "false"));
        }
        if (leaf == "chest" || leaf == "trapped_chest") {
            if (!onlyProperties(properties, {"facing_direction"})) {
                return unsupported("unsupported BDX chest state properties");
            }
            return map_canonical(identifier, canonical_facing + ",type=single,waterlogged=false");
        }
        if (leaf == "ender_chest") {
            if (!onlyProperties(properties, {"facing_direction"})) {
                return unsupported("unsupported BDX ender-chest state properties");
            }
            return map_canonical(identifier, canonical_facing + ",waterlogged=false");
        }
        if (leaf == "furnace" || leaf == "blast_furnace" || leaf == "smoker") {
            if (!onlyProperties(properties, {"facing_direction", "lit_bit"})) {
                return unsupported("unsupported BDX furnace state properties");
            }
            bool lit = false;
            if (!boolean("lit_bit", false, &lit)) return unsupported("BDX furnace has an invalid lit flag");
            // Some exported Bedrock palettes use 0/1 for furnace facings.
            // Those values do not have a Java-facing counterpart accepted by
            // mapSpongeState, but they are valid target aux values.  Preserve
            // the native data and make the placement unmergeable instead of
            // rejecting the whole BDX file.
            if (direction < 2) {
                return direct(std::string(identifier), static_cast<uint8_t>(direction), true,
                              std::nullopt,
                              "BDX furnace used a native non-Java facing direction");
            }
            return map_canonical(identifier, canonical_facing + ",lit=" + (lit ? "true" : "false"));
        }
        if (leaf == "wall_sign" || endsWith(leaf, "_wall_sign")) {
            if (!onlyProperties(properties, {"facing_direction"})) {
                return unsupported("unsupported BDX wall-sign state properties");
            }
            return map_canonical(identifier, canonical_facing + ",waterlogged=false");
        }
        if (leaf == "wall_banner") {
            if (!onlyProperties(properties, {"facing_direction"})) {
                return unsupported("unsupported BDX wall-banner state properties");
            }
            return map_canonical(identifier, canonical_facing);
        }
        if (endsWith(leaf, "_glazed_terracotta")) {
            if (!onlyProperties(properties, {"facing_direction"})) {
                return unsupported("unsupported BDX glazed-terracotta state properties");
            }
            return map_canonical(identifier, canonical_facing);
        }
        if (leaf == "ladder") {
            if (!onlyProperties(properties, {"facing_direction"})) {
                return unsupported("unsupported BDX ladder state properties");
            }
            return map_canonical(identifier, canonical_facing + ",waterlogged=false");
        }
        // Many native Bedrock blocks expose a facing_direction whose numeric
        // representation is exactly the target aux value.  Keep it rather
        // than dropping a valid BDX palette entry just because its complete
        // modern state schema is outside the legacy command registry.
        return direct(std::string(identifier), static_cast<uint8_t>(direction), true,
                      std::nullopt, "BDX block state was reduced to its facing direction");
    }

    // State arrays that do not have an exact target encoding are still useful
    // as a block shell.  Preserve known identifiers, prevent /fill merging,
    // and record the intentional state loss rather than silently producing a
    // hole in the imported structure.
    if (isKnownTargetCommand(identifier)) {
        return direct(std::string(identifier), legacy_data, true, std::nullopt,
                      "BDX block-state properties are unavailable in the target command registry and were reset");
    }
    return unsupported("BDX block identifier has no target-version mapping");
}

BlockMappingResult BlockMapper::mapInfiniteczState(std::string_view state,
                                                    uint16_t native_aux,
                                                    bool has_native_aux,
                                                    std::string_view state_json) const {
    const size_t bracket = state.find('[');
    const std::string_view identifier = state.substr(0, bracket);
    constexpr std::string_view kMinecraftPrefix = "minecraft:";
    const bool safe_native_identifier =
        bracket == std::string_view::npos && isSafeIdentifier(identifier) &&
        startsWith(identifier, kMinecraftPrefix) &&
        identifier.size() > kMinecraftPrefix.size() &&
        identifier.find(':', kMinecraftPrefix.size()) == std::string_view::npos;

    // The native state-JSON snapshot is the authoritative source for every
    // stateful block's orientation (doors, trapdoors, stairs, copper
    // variants, ...): it is captured verbatim from the running game and
    // therefore always reflects the current Bedrock state schema, unlike
    // native_aux which can be a stale/zeroed legacy field for any block whose
    // state moved to a modern property (see rawSnapshotAuxForExport). When a
    // state JSON snapshot is available, convert it to the same bracketed
    // property syntax BDX uses and let mapBedrockState's existing per-block
    // decoders (already exercised by the BDX import path) do the work, so no
    // per-block-type branch is needed here or when new blocks are added.
    if (!state_json.empty()) {
        const std::string synthesized_properties =
            nativeStateJsonToBedrockProperties(state_json);
        if (!synthesized_properties.empty()) {
            BlockMappingResult mapped = mapBedrockState(
                identifier, synthesized_properties,
                has_native_aux ? native_aux : 0, has_native_aux);
            if (mapped.isMapped()) return mapped;
        }
    }

    // Registered names still use the established mapper so historical
    // aliases, slab families and phase rules retain their exact behavior.
    // Only a native identifier absent from the frozen registry takes the
    // forward-compatible path.
    if (!safe_native_identifier || isKnownTargetCommand(identifier)) {
        return has_native_aux
            ? mapBedrockState(state, {}, native_aux, true)
            : mapSpongeState(state);
    }

    BlockSpec result;
    result.command_name.assign(identifier.data(), identifier.size());
    result.aux = has_native_aux ? native_aux : 0;
    result.phase = phaseFor(result.command_name);
    const std::string_view leaf = identifier.substr(kMinecraftPrefix.size());
    if ((isBed(leaf) || isDoor(leaf) || leaf == "double_plant") &&
        (result.aux & 0x08U) != 0U) {
        result.phase = ImportPhase::DependentAttachment;
    }
    result.single_layer_only = result.phase != ImportPhase::Structure;
    result.stateful = isBedrockFallbackStateful(leaf);
    result.can_fill = !result.stateful && result.phase != ImportPhase::Attachment &&
        result.phase != ImportPhase::DependentAttachment;
    return {BlockMappingStatus::Mapped, std::move(result), {}};
}

std::optional<BlockSpec> BlockMapper::resolveLegacy(uint16_t id, uint8_t data) const {
    BlockMappingResult result = mapLegacy(id, data);
    if (!result.isMapped()) return std::nullopt;
    return std::move(result.spec);
}

std::optional<BlockSpec> BlockMapper::resolveSpongeState(std::string_view state) const {
    BlockMappingResult result = mapSpongeState(state);
    if (!result.isMapped()) return std::nullopt;
    return std::move(result.spec);
}

std::optional<BlockSpec> BlockMapper::resolveBedrockState(std::string_view identifier,
                                                            std::string_view state,
                                                            uint16_t legacy_data,
                                                            bool has_legacy_aux) const {
    BlockMappingResult result = mapBedrockState(identifier, state, legacy_data, has_legacy_aux);
    if (!result.isMapped()) return std::nullopt;
    return std::move(result.spec);
}

bool BlockMapper::isStableVerificationBlockName(std::string_view block_name) {
    if (phaseFor(block_name) != ImportPhase::Structure) return false;
    const size_t state = block_name.find('[');
    if (state != std::string_view::npos) block_name = block_name.substr(0, state);
    const size_t separator = block_name.rfind(':');
    const std::string_view leaf = separator == std::string_view::npos
        ? block_name : block_name.substr(separator + 1);
    const auto ends_with = [&](std::string_view suffix) {
        return leaf.size() >= suffix.size() &&
            leaf.substr(leaf.size() - suffix.size()) == suffix;
    };
    static constexpr std::string_view kUnstableNames[] = {
        "air", "bamboo", "beetroot", "beetroots", "brown_mushroom",
        "barrel", "cactus", "carpet", "carrots", "chest", "chorus_flower",
        "chorus_plant", "composter", "cocoa", "crafter", "dead_bush", "deadbush",
        "daylight_detector", "dispenser", "dropper",
        "daylight_detector_inverted", "end_gateway", "end_portal", "farmland", "fire",
        "frosted_ice", "grass", "grass_block", "grass_path",
        "hopper", "ice", "kelp", "leaves", "leaves2", "lit_redstone_lamp",
        "melon_stem", "mycelium", "nether_portal", "nether_wart", "portal", "potatoes",
        "pumpkin_stem",
        "noteblock", "observer", "piston", "piston_arm_collision", "powered_comparator",
        "powered_repeater",
        "red_flower", "red_mushroom", "redstone_lamp", "sapling", "seagrass",
        "snow_layer", "tall_grass", "tallgrass", "unpowered_comparator",
        "shulker_box", "sponge", "sticky_piston", "trapped_chest",
        "undyed_shulker_box", "unpowered_repeater", "wet_sponge", "wheat",
        "yellow_flower",
    };
    for (const std::string_view unstable : kUnstableNames) {
        if (leaf == unstable) return false;
    }
    return !ends_with("_sapling") && !ends_with("_leaves") &&
        !ends_with("_carpet") && !ends_with("_flower") &&
        !ends_with("_mushroom") && !ends_with("_stem") &&
        !ends_with("_shulker_box") &&
        leaf.find("coral") == std::string_view::npos;
}

ImportPhase BlockMapper::phaseFor(std::string_view block_name) {
    const size_t bracket = block_name.find('[');
    if (bracket != std::string_view::npos) block_name = block_name.substr(0, bracket);
    const size_t separator = block_name.rfind(':');
    const std::string_view leaf = separator == std::string_view::npos ? block_name : block_name.substr(separator + 1);
    if (leaf == "water" || leaf == "flowing_water" || leaf == "lava" || leaf == "flowing_lava" ||
        leaf == "bubble_column" || leaf == "powder_snow") {
        return ImportPhase::Fluid;
    }
    const auto endsWith = [&](std::string_view suffix) {
        return leaf.size() >= suffix.size() && leaf.substr(leaf.size() - suffix.size()) == suffix;
    };
    if (leaf == "sand" || leaf == "red_sand" || leaf == "gravel" || leaf == "dragon_egg" ||
        leaf == "scaffolding" || leaf == "pointed_dripstone" || leaf == "concretepowder" ||
        leaf == "concrete_powder" ||
        leaf == "suspicious_sand" || leaf == "suspicious_gravel" ||
        endsWith("_concrete_powder") || endsWith("_anvil") || leaf == "anvil") return ImportPhase::Gravity;
    if (leaf == "waterlily" || leaf == "lily_pad" || leaf == "vine" || leaf == "bed" ||
        leaf == "standing_sign" || leaf == "wall_sign" || leaf == "standing_banner" ||
        leaf == "wall_banner" || leaf == "redstone_wire" || leaf == "tripwire" ||
        leaf == "tripwire_hook" || leaf == "reeds" || leaf == "cake" ||
        leaf == "flower_pot" || leaf == "skull" || leaf == "end_rod" ||
        leaf == "double_plant" || leaf == "bamboo" || leaf == "bamboo_sapling" ||
        leaf == "cave_vines" || leaf == "cave_vines_plant" || leaf == "big_dripleaf" ||
        leaf == "big_dripleaf_stem" || leaf == "kelp" || leaf == "seagrass" ||
        leaf == "sea_pickle" ||
        leaf == "amethyst_cluster" || leaf == "small_amethyst_bud" ||
        leaf == "medium_amethyst_bud" || leaf == "large_amethyst_bud" ||
        leaf == "candle" || endsWith("_candle") || leaf == "torchflower" ||
        leaf == "wither_rose" || leaf == "spore_blossom" ||
        endsWith("_sapling") || endsWith("_flower") || endsWith("_coral_fan") ||
        endsWith("_coral_wall_fan") || endsWith("_roots") ||
        endsWith("_bed") || endsWith("_sign") || endsWith("_wall_sign") ||
        endsWith("_banner") || endsWith("_wall_banner") || endsWith("_pressure_plate") ||
        block_name.find("torch") != std::string_view::npos ||
        block_name.find("rail") != std::string_view::npos ||
        block_name.find("button") != std::string_view::npos ||
        block_name.find("lever") != std::string_view::npos ||
        leaf == "powered_repeater" || leaf == "unpowered_repeater" ||
        leaf == "powered_comparator" || leaf == "unpowered_comparator" ||
        leaf == "daylight_detector" || leaf == "daylight_detector_inverted" ||
        block_name.find("ladder") != std::string_view::npos ||
        block_name.find("door") != std::string_view::npos) return ImportPhase::Attachment;
    return ImportPhase::Structure;
}

bool BlockMapper::isSafeIdentifier(std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    for (const char character : value) {
        const unsigned char ch = static_cast<unsigned char>(character);
        if (!(std::islower(ch) || std::isdigit(ch) || character == ':' || character == '_' || character == '-')) {
            return false;
        }
    }
    return value.find(':') != std::string_view::npos;
}

}  // namespace build_import
