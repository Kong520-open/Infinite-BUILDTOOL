#include "ProjectionBlockIdentity.h"

#include <array>
#include <utility>

namespace build_import {
namespace {

constexpr std::string_view kMinecraftNamespace = "minecraft:";

bool endsWith(std::string_view value, std::string_view suffix) noexcept {
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool startsWith(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && value.compare(0U, prefix.size(), prefix) == 0;
}

bool contains(std::string_view value, std::string_view needle) noexcept {
    return value.find(needle) != std::string_view::npos;
}

ProjectionBlockIdentity identity(std::string_view name, uint16_t aux) {
    return {std::string(name), aux};
}

std::string withSuffix(std::string_view prefix, std::string_view suffix) {
    std::string result;
    result.reserve(prefix.size() + suffix.size());
    result.append(prefix.data(), prefix.size());
    result.append(suffix.data(), suffix.size());
    return result;
}

std::string canonicalAlias(std::string output) {
    // Only old/new spelling changes are listed here.  Material family changes
    // (for example log versus wood) are intentionally handled below where the
    // auxiliary data can preserve their distinct geometry.
    if (output == "grass") return "grass_block";
    if (output == "lit_furnace") return "furnace";
    if (output == "lit_blast_furnace") return "blast_furnace";
    if (output == "lit_smoker") return "smoker";
    if (output == "powered_repeater" || output == "unpowered_repeater") return "repeater";
    if (output == "powered_comparator" || output == "unpowered_comparator") return "comparator";
    if (output == "lit_redstone_ore") return "redstone_ore";
    if (output == "lit_redstone_lamp") return "redstone_lamp";
    if (output == "unlit_redstone_torch") return "redstone_torch";
    if (output == "redstone_wall_torch") return "redstone_torch";
    if (output == "wall_torch") return "torch";
    if (output == "soul_wall_torch") return "soul_torch";
    if (output == "wooden_button") return "oak_button";
    if (output == "flowing_water") return "water";
    if (output == "flowing_lava") return "lava";
    if (output == "brick_block") return "bricks";
    if (output == "web") return "cobweb";
    if (output == "melon_block") return "melon";
    if (output == "waterlily") return "lily_pad";
    if (output == "grass_path") return "dirt_path";
    if (output == "noteblock") return "note_block";
    if (output == "nether_quartz_ore") return "quartz_ore";
    if (output == "nether_brick") return "nether_bricks";
    if (output == "red_nether_brick") return "red_nether_bricks";
    if (output == "magma") return "magma_block";
    if (output == "end_bricks" || output == "end_stone_brick") return "end_stone_bricks";
    if (output == "prismarine_bricks") return "prismarine_brick";
    // Bedrock used "silver" for light gray before its colour-name cleanup.
    // Preserve the rest of the identifier verbatim so the rule covers wool,
    // terracotta, panes, carpet and glazed terracotta without fuzzy matching.
    constexpr std::string_view kSilverPrefix = "silver_";
    if (startsWith(output, kSilverPrefix)) {
        return "light_gray_" + output.substr(kSilverPrefix.size());
    }
    return output;
}

bool namedSingleSlab(std::string_view name) noexcept {
    if (!endsWith(name, "_slab")) return false;
    return !startsWith(name, "double_") && !contains(name, "_double_slab");
}

bool legacySlabFamily(std::string_view name) noexcept {
    return name == "wooden_slab" || name == "stone_slab" ||
        name == "stone_slab2" || name == "stone_slab3" ||
        name == "stone_slab4" || name == "stone_block_slab" ||
        name == "stone_block_slab2" || name == "stone_block_slab3" ||
        name == "stone_block_slab4";
}

bool legacyDoubleSlabFamily(std::string_view name) noexcept {
    return name == "double_wooden_slab" || name == "double_stone_slab" ||
        name == "double_stone_slab2" || name == "double_stone_slab3" ||
        name == "double_stone_slab4" || name == "double_stone_block_slab" ||
        name == "double_stone_block_slab2" || name == "double_stone_block_slab3" ||
        name == "double_stone_block_slab4";
}

// Current Bedrock keeps most complete slabs under a separate identifier
// instead of encoding a vertical half.  The printer and the projection
// matcher, however, both operate on the item that creates that block: its
// corresponding single-slab material.  Keep these modern spellings in the
// same logical representation as a source record carrying the synthetic
// double-state bit.
bool flattenedDoubleSlabIdentity(std::string_view name,
                                 ProjectionBlockIdentity* output) {
    if (!output) return false;

    std::string single_name;
    constexpr std::string_view kDoubleSuffix = "_double_slab";
    constexpr std::string_view kReversedDoubleSuffix = "_slab_double";
    if (endsWith(name, kDoubleSuffix)) {
        single_name.assign(name.substr(0U, name.size() - kDoubleSuffix.size()));
        single_name.append("_slab");
    } else if (endsWith(name, kReversedDoubleSuffix)) {
        single_name.assign(name.substr(0U, name.size() - std::string_view("_double").size()));
    } else {
        // Copper used a prefix/infix form in a few Bedrock versions.  These
        // are deliberately explicit: generic double_* handling would steal
        // legacy double_stone_slab / double_wooden_slab before their aux-based
        // material family can be resolved below.
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 8U>
            kCopperAliases{{
                {"double_cut_copper_slab", "cut_copper_slab"},
                {"exposed_double_cut_copper_slab", "exposed_cut_copper_slab"},
                {"weathered_double_cut_copper_slab", "weathered_cut_copper_slab"},
                {"oxidized_double_cut_copper_slab", "oxidized_cut_copper_slab"},
                {"waxed_double_cut_copper_slab", "waxed_cut_copper_slab"},
                {"waxed_exposed_double_cut_copper_slab", "waxed_exposed_cut_copper_slab"},
                {"waxed_weathered_cut_copper_slab", "waxed_weathered_cut_copper_slab"},
                {"waxed_oxidized_double_cut_copper_slab", "waxed_oxidized_cut_copper_slab"},
            }};
        for (const auto& alias : kCopperAliases) {
            if (name != alias.first) continue;
            single_name.assign(alias.second);
            break;
        }
    }

    if (!namedSingleSlab(single_name)) return false;
    *output = {std::move(single_name), kProjectionSlabDoubleAux};
    return true;
}

bool legacyLogName(std::string_view name) noexcept {
    return name == "log" || name == "log2";
}

bool modernAxisName(std::string_view name) noexcept {
    return endsWith(name, "_log") || endsWith(name, "_wood") ||
        endsWith(name, "_stem") || endsWith(name, "_hyphae") ||
        contains(name, "pillar") || name == "basalt" ||
        name == "polished_basalt" || name == "bamboo_block" ||
        name == "stripped_bamboo_block" || name == "chain" ||
        name == "deepslate";
}

bool oldPillarName(std::string_view name) noexcept {
    return name == "bone_block" || name == "hay_block";
}

bool isButtonAttachmentName(std::string_view name) noexcept {
    return name == "stone_button" || name == "oak_button" || endsWith(name, "_button");
}

bool isTorchAttachmentName(std::string_view name) noexcept {
    return name == "torch" || name == "soul_torch" || name == "redstone_torch";
}

bool isAttachmentName(std::string_view name) noexcept {
    return isButtonAttachmentName(name) || isTorchAttachmentName(name);
}

bool isLeverName(std::string_view name) noexcept {
    return name == "lever";
}

bool isAnvilName(std::string_view name) noexcept {
    return name == "anvil" || name == "chipped_anvil" || name == "damaged_anvil";
}

bool colorIdentity(std::string_view name, uint16_t aux,
                   ProjectionBlockIdentity* output) {
    if (!output || aux >= 16U) return false;
    static constexpr std::array<std::string_view, 16U> kColors{{
        "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
        "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
    }};
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 6U> kFamilies{{
        {"wool", "_wool"},
        {"stained_hardened_clay", "_terracotta"},
        {"stained_glass", "_stained_glass"},
        {"stained_glass_pane", "_stained_glass_pane"},
        {"carpet", "_carpet"},
        {"concrete", "_concrete"},
    }};
    for (const auto& family : kFamilies) {
        if (name != family.first) continue;
        *output = {withSuffix(kColors[aux], family.second), 0U};
        return true;
    }
    if (name == "concrete_powder") {
        *output = {withSuffix(kColors[aux], "_concrete_powder"), 0U};
        return true;
    }
    return false;
}

bool simpleVariantIdentity(std::string_view name, uint16_t aux,
                            ProjectionBlockIdentity* output) {
    if (!output) return false;
    struct Family {
        std::string_view name;
        const std::string_view* variants;
        size_t count;
    };
    static constexpr std::array<std::string_view, 7U> kStone{{
        "stone", "granite", "polished_granite", "diorite", "polished_diorite",
        "andesite", "polished_andesite",
    }};
    static constexpr std::array<std::string_view, 3U> kDirt{{
        "dirt", "coarse_dirt", "podzol",
    }};
    static constexpr std::array<std::string_view, 2U> kSand{{"sand", "red_sand"}};
    static constexpr std::array<std::string_view, 4U> kStoneBricks{{
        "stone_bricks", "mossy_stone_bricks", "cracked_stone_bricks",
        "chiseled_stone_bricks",
    }};
    static constexpr std::array<std::string_view, 3U> kPrismarine{{
        "prismarine", "prismarine_brick", "dark_prismarine",
    }};
    static constexpr std::array<Family, 5U> kFamilies{{
        {"stone", kStone.data(), kStone.size()},
        {"dirt", kDirt.data(), kDirt.size()},
        {"sand", kSand.data(), kSand.size()},
        {"stonebrick", kStoneBricks.data(), kStoneBricks.size()},
        {"prismarine", kPrismarine.data(), kPrismarine.size()},
    }};
    for (const Family& family : kFamilies) {
        if (name != family.name || aux >= family.count) continue;
        *output = identity(family.variants[aux], 0U);
        return true;
    }
    return false;
}

// These aliases are meaningful only on the imported side.  In particular,
// legacy `stone_stairs` is cobblestone while a current flattened
// `stone_stairs` is genuinely stone; applying this table to live data would
// silently select the wrong hotbar material.
bool projectionSourceAliasIdentity(std::string_view name, uint16_t aux,
                                   ProjectionBlockIdentity* output) {
    if (!output) return false;
    if (name == "normal_stone_stairs") {
        *output = {"stone_stairs", aux};
        return true;
    }
    if (name == "stone_stairs") {
        *output = {"cobblestone_stairs", aux};
        return true;
    }
    if (name == "end_brick_stairs") {
        *output = {"end_stone_brick_stairs", aux};
        return true;
    }
    if (name == "prismarine_bricks_stairs") {
        *output = {"prismarine_brick_stairs", aux};
        return true;
    }
    if (name == "pumpkin") {
        *output = {"carved_pumpkin", aux};
        return true;
    }
    if (name == "lit_pumpkin") {
        *output = {"jack_o_lantern", aux};
        return true;
    }
    if (name == "snow" && aux == 0U) {
        *output = {"snow_block", 0U};
        return true;
    }
    if (name == "slime" && aux == 0U) {
        *output = {"slime_block", 0U};
        return true;
    }
    return false;
}

// Families whose old generic name carried a material choice in the aux value,
// while a current item stack uses one identifier per material.  Keep this
// table intentionally limited to complete, verified legacy layouts: treating
// an unknown aux as a default item would make the printer consume a wrong
// material.
bool legacyVariantIdentity(std::string_view name, uint16_t aux,
                           ProjectionBlockIdentity* output) {
    if (!output) return false;
    const auto select = [&](const auto& variants) {
        if (aux >= variants.size()) return false;
        *output = identity(variants[aux], 0U);
        return true;
    };
    if (name == "sandstone") {
        static constexpr std::array<std::string_view, 3U> kVariants{{
            "sandstone", "chiseled_sandstone", "smooth_sandstone",
        }};
        return select(kVariants);
    }
    if (name == "red_sandstone") {
        static constexpr std::array<std::string_view, 3U> kVariants{{
            "red_sandstone", "chiseled_red_sandstone", "smooth_red_sandstone",
        }};
        return select(kVariants);
    }
    if (name == "sponge") {
        static constexpr std::array<std::string_view, 2U> kVariants{{
            "sponge", "wet_sponge",
        }};
        return select(kVariants);
    }
    if (name == "cobblestone_wall") {
        static constexpr std::array<std::string_view, 2U> kVariants{{
            "cobblestone_wall", "mossy_cobblestone_wall",
        }};
        return select(kVariants);
    }
    if (name == "monster_egg") {
        static constexpr std::array<std::string_view, 6U> kVariants{{
            "infested_stone", "infested_cobblestone", "infested_stone_bricks",
            "infested_mossy_stone_bricks", "infested_cracked_stone_bricks",
            "infested_chiseled_stone_bricks",
        }};
        return select(kVariants);
    }
    if (name == "anvil") {
        // Legacy anvils kept their wear stage in bits 2..3 and their horizontal
        // direction in bits 0..1.  There is no usable fourth damage stage.
        if ((aux & ~UINT16_C(0x000F)) != 0U ||
            (aux & UINT16_C(0x000C)) == UINT16_C(0x000C)) {
            return false;
        }
        const uint16_t damage = (aux >> 2U) & UINT16_C(0x0003);
        const std::string_view variant = damage == 0U ? "anvil" :
            damage == 1U ? "chipped_anvil" : "damaged_anvil";
        *output = {std::string(variant), static_cast<uint16_t>(aux & UINT16_C(0x0003))};
        return true;
    }
    if (name == "quartz_block") {
        // The pillar's axis shares the old quartz aux field.  Represent it
        // with the flattened pillar name and its compact y/x/z state so both
        // material selection and world comparison stay lossless.
        if (aux == 1U) {
            *output = {"chiseled_quartz_block", 0U};
            return true;
        }
        if (aux == 3U) {
            *output = {"smooth_quartz", 0U};
            return true;
        }
        if (aux == 2U || aux == 6U || aux == 10U) {
            *output = {"quartz_pillar", static_cast<uint16_t>(
                aux == 2U ? 0U : aux == 6U ? 1U : 2U)};
            return true;
        }
    }
    return false;
}

bool legacyWoodIdentity(std::string_view name, uint16_t aux,
                        ProjectionBlockIdentity* output) {
    if (!output) return false;
    static constexpr std::array<std::string_view, 6U> kWoods{{
        "oak", "spruce", "birch", "jungle", "acacia", "dark_oak",
    }};
    if (name == "fence" || name == "wooden_fence") {
        if (aux >= kWoods.size()) return false;
        *output = {withSuffix(kWoods[aux], "_fence"), 0U};
        return true;
    }
    if (name == "planks" || name == "wooden_planks") {
        if (aux >= kWoods.size()) return false;
        *output = {withSuffix(kWoods[aux], "_planks"), 0U};
        return true;
    }
    if (name == "sapling") {
        // The old generic sapling stores species in the low three bits and a
        // growth/update bit in bit 3.  The latter is not an item variant, so
        // retain only the species when matching a flattened hotbar item.
        if ((aux & ~UINT16_C(0x000F)) != 0U) return false;
        const uint16_t material = aux & UINT16_C(0x0007);
        if (material >= kWoods.size()) return false;
        *output = {withSuffix(kWoods[material], "_sapling"), 0U};
        return true;
    }
    if (name == "wooden_slab" || name == "double_wooden_slab") {
        const uint16_t material = aux & UINT16_C(0x0007);
        const uint16_t allowed = name == "wooden_slab" ? UINT16_C(0x000F) : UINT16_C(0x0007);
        if ((aux & ~allowed) != 0U || material >= kWoods.size()) return false;
        const uint16_t placement = name == "double_wooden_slab"
            ? kProjectionSlabDoubleAux
            : ((aux & kProjectionSlabTopAux) != 0U ? kProjectionSlabTopAux : 0U);
        *output = {withSuffix(kWoods[material], "_slab"), placement};
        return true;
    }
    if (name == "leaves" || name == "leaves2") {
        if ((aux & ~UINT16_C(0x000F)) != 0U) return false;
        const uint16_t material = aux & UINT16_C(0x0003);
        const size_t wood_index = name == "leaves" ? static_cast<size_t>(material)
                                                     : static_cast<size_t>(material + 4U);
        if (wood_index >= kWoods.size()) return false;
        // Legacy bits 2/3 are decay/persistence bookkeeping, not an item or
        // placement orientation. A fresh printer placement cannot faithfully
        // reproduce them, so material selection and exact visual hiding use
        // the stable flattened leaf identity instead.
        *output = {withSuffix(kWoods[wood_index], "_leaves"), 0U};
        return true;
    }
    if (name != "log" && name != "log2") return false;
    const uint16_t allowed = UINT16_C(0x000F);
    if ((aux & ~allowed) != 0U) return false;
    const uint16_t material = aux & UINT16_C(0x0003);
    const size_t wood_index = name == "log" ? static_cast<size_t>(material)
                                               : static_cast<size_t>(material + 4U);
    if (wood_index >= kWoods.size()) return false;
    const uint16_t axis_bits = aux & UINT16_C(0x000C);
    if (axis_bits == UINT16_C(0x000C)) {
        *output = {withSuffix(kWoods[wood_index], "_wood"), 0U};
    } else {
        const uint16_t axis = axis_bits == 0U ? 0U :
            axis_bits == UINT16_C(0x0004) ? 1U : 2U;
        *output = {withSuffix(kWoods[wood_index], "_log"), axis};
    }
    return true;
}

// Some builds retain an old generic-planks aux alongside the modern,
// per-species inventory name. Named planks have no placeable state in that
// field, so retaining it would make a genuine spruce stack look different from
// imported legacy `planks + 1`. Keep this list exact instead of matching every
// future *_planks identifier implicitly.
bool namedPlanksIdentity(std::string_view name, ProjectionBlockIdentity* output) {
    if (!output) return false;
    if (name != "oak_planks" && name != "spruce_planks" &&
        name != "birch_planks" && name != "jungle_planks" &&
        name != "acacia_planks" && name != "dark_oak_planks" &&
        name != "mangrove_planks" && name != "cherry_planks" &&
        name != "pale_oak_planks" && name != "bamboo_planks" &&
        name != "bamboo_mosaic_planks" && name != "crimson_planks" &&
        name != "warped_planks") {
        return false;
    }
    *output = {std::string(name), 0U};
    return true;
}

bool legacyStoneSlabIdentity(std::string_view name, uint16_t aux,
                              ProjectionBlockIdentity* output) {
    if (!output) return false;
    const bool is_double = legacyDoubleSlabFamily(name);
    const uint16_t allowed = is_double ? UINT16_C(0x0007) : UINT16_C(0x000F);
    if ((!legacySlabFamily(name) && !is_double) || (aux & ~allowed) != 0U) {
        return false;
    }
    const uint16_t material = aux & UINT16_C(0x0007);
    struct Family {
        std::string_view single_name;
        std::string_view double_name;
        const std::string_view* materials;
        size_t count;
    };
    static constexpr std::array<std::string_view, 8U> kFirst{{
        "smooth_stone_slab", "sandstone_slab", "petrified_oak_slab",
        "cobblestone_slab", "brick_slab", "stone_brick_slab", "quartz_slab",
        "nether_brick_slab",
    }};
    static constexpr std::array<std::string_view, 8U> kSecond{{
        "red_sandstone_slab", "purpur_slab", "prismarine_slab",
        "dark_prismarine_slab", "prismarine_brick_slab", "mossy_cobblestone_slab",
        "smooth_sandstone_slab", "red_nether_brick_slab",
    }};
    static constexpr std::array<std::string_view, 8U> kThird{{
        "end_stone_brick_slab", "smooth_red_sandstone_slab",
        "polished_andesite_slab", "andesite_slab", "diorite_slab",
        "polished_diorite_slab", "granite_slab", "polished_granite_slab",
    }};
    static constexpr std::array<std::string_view, 5U> kFourth{{
        "mossy_stone_brick_slab", "smooth_quartz_slab", "stone_slab",
        "cut_sandstone_slab", "cut_red_sandstone_slab",
    }};
    static constexpr std::array<Family, 4U> kFamilies{{
        {"stone_slab", "double_stone_slab", kFirst.data(), kFirst.size()},
        {"stone_slab2", "double_stone_slab2", kSecond.data(), kSecond.size()},
        {"stone_block_slab3", "double_stone_block_slab3", kThird.data(), kThird.size()},
        {"stone_block_slab4", "double_stone_block_slab4", kFourth.data(), kFourth.size()},
    }};
    const Family* selected = nullptr;
    if (name == "stone_slab" || name == "double_stone_slab" ||
        name == "stone_block_slab" || name == "double_stone_block_slab") {
        selected = &kFamilies[0];
    } else if (name == "stone_slab2" || name == "double_stone_slab2" ||
               name == "stone_block_slab2" || name == "double_stone_block_slab2") {
        selected = &kFamilies[1];
    } else if (name == "stone_slab3" || name == "double_stone_slab3" ||
               name == "stone_block_slab3" || name == "double_stone_block_slab3") {
        selected = &kFamilies[2];
    } else if (name == "stone_slab4" || name == "double_stone_slab4" ||
               name == "stone_block_slab4" || name == "double_stone_block_slab4") {
        selected = &kFamilies[3];
    }
    if (!selected || material >= selected->count) return false;
    const uint16_t placement = is_double ? kProjectionSlabDoubleAux :
        ((aux & kProjectionSlabTopAux) != 0U ? kProjectionSlabTopAux : 0U);
    *output = {std::string(selected->materials[material]), placement};
    return true;
}

bool isNamedShulkerBoxName(std::string_view name) noexcept {
    static constexpr std::array<std::string_view, 17U> kNames{{
        "undyed_shulker_box", "white_shulker_box", "orange_shulker_box",
        "magenta_shulker_box", "light_blue_shulker_box", "yellow_shulker_box",
        "lime_shulker_box", "pink_shulker_box", "gray_shulker_box",
        "light_gray_shulker_box", "cyan_shulker_box", "purple_shulker_box",
        "blue_shulker_box", "brown_shulker_box", "green_shulker_box",
        "red_shulker_box", "black_shulker_box",
    }};
    for (const std::string_view candidate : kNames) {
        if (name == candidate) return true;
    }
    return false;
}

bool isContentlessContainerShellName(std::string_view name) noexcept {
    return name == "chest" || name == "trapped_chest" || name == "ender_chest" ||
        name == "barrel" || name == "hopper" || name == "dispenser" ||
        name == "dropper" || name == "furnace" || name == "blast_furnace" ||
        name == "smoker" || isNamedShulkerBoxName(name);
}

bool isSixWayFacingName(std::string_view name) noexcept {
    return name == "barrel" || name == "hopper" || name == "dispenser" ||
        name == "dropper" || name == "observer" || isNamedShulkerBoxName(name);
}

bool sixWayHasTransientBit(std::string_view name) noexcept {
    return name == "hopper" || name == "dispenser" || name == "dropper" ||
        name == "observer";
}

bool isHorizontalSixWayFacingName(std::string_view name) noexcept {
    return name == "chest" || name == "trapped_chest" || name == "ender_chest" ||
        name == "furnace" || name == "blast_furnace" || name == "smoker";
}

bool isRepeaterName(std::string_view name) noexcept {
    return name == "repeater";
}

bool isComparatorName(std::string_view name) noexcept {
    return name == "comparator";
}

bool isDaylightDetectorName(std::string_view name) noexcept {
    return name == "daylight_detector" || name == "daylight_detector_inverted";
}

bool isInvertedDaylightDetectorName(std::string_view name) noexcept {
    return name == "daylight_detector_inverted";
}

bool isPlacementDerivedIdentity(std::string_view name) {
    if (IsProjectionSlabBlock(name) || contains(name, "stairs") ||
        name == "normal_stone_stairs") {
        return true;
    }
    ProjectionBlockAxis ignored_axis;
    if (TryProjectionBlockAxis(name, 0U, &ignored_axis) || modernAxisName(name) ||
        oldPillarName(name) || legacyLogName(name) || name == "quartz_block" ||
        name == "quartz_pillar") {
        return true;
    }
    ProjectionBlockFace ignored_attachment;
    if (TryProjectionAttachmentFace(name, 0U, &ignored_attachment) ||
        isAttachmentName(name) || TryProjectionLeverAttachmentFace(name, 0U,
                                                                     &ignored_attachment) ||
        isLeverName(name)) {
        return true;
    }
    return IsProjectionYawDirectedBlock(name) || name == "end_rod" ||
        contains(name, "lightning_rod") || name == "lantern" ||
        name == "soul_lantern" || isSixWayFacingName(name) ||
        isHorizontalSixWayFacingName(name) || isRepeaterName(name) ||
        isComparatorName(name) || isDaylightDetectorName(name);
}

}  // namespace

std::string ProjectionCanonicalBlockName(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (const unsigned char character : value) {
        if (character == '[' || character == '{' || character == '|') break;
        if (character >= 'A' && character <= 'Z') {
            output.push_back(static_cast<char>(character - 'A' + 'a'));
        } else if (character != ' ' && character != '\t' && character != '\r' &&
                   character != '\n') {
            output.push_back(static_cast<char>(character));
        }
    }
    if (output.compare(0U, kMinecraftNamespace.size(), kMinecraftNamespace) == 0) {
        output.erase(0U, kMinecraftNamespace.size());
    }
    return canonicalAlias(std::move(output));
}

bool IsProjectionDoubleSlabBlock(std::string_view raw_name) {
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    return legacyDoubleSlabFamily(name) || startsWith(name, "double_") ||
        contains(name, "_double_slab") || endsWith(name, "_slab_double");
}

bool IsProjectionSlabBlock(std::string_view raw_name) {
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    return legacySlabFamily(name) || legacyDoubleSlabFamily(name) || namedSingleSlab(name) ||
        IsProjectionDoubleSlabBlock(name);
}

bool TryProjectionSlabPlacement(std::string_view raw_name, uint16_t aux,
                                bool* top, bool* is_double) {
    if (!top || !is_double) return false;
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    if (!IsProjectionSlabBlock(name)) return false;
    *is_double = IsProjectionDoubleSlabBlock(name) ||
        (aux & kProjectionSlabDoubleAux) != 0U;
    if (*is_double) {
        *top = false;
        return true;
    }
    if (name == "wooden_slab" || legacySlabFamily(name)) {
        if ((aux & ~UINT16_C(0x000F)) != 0U) return false;
        *top = (aux & kProjectionSlabTopAux) != 0U;
        return true;
    }
    if (!namedSingleSlab(name)) return false;
    // Flattened targets store a physical half as 0/1; older command-space
    // records use bit 3. Both forms are accepted only when no unrelated state
    // bits are present.
    if (aux == 0U) {
        *top = false;
        return true;
    }
    if (aux == 1U || aux == kProjectionSlabTopAux ||
        aux == static_cast<uint16_t>(kProjectionSlabTopAux | 1U)) {
        *top = true;
        return true;
    }
    return false;
}

namespace {

// Imported projections are allowed to carry old numeric-data identifiers;
// current inventory/world data normally is not.  Keep the two representations
// separate here instead of guessing from a name that changed meaning during
// flattening (most notably `stone_slab`).
ProjectionBlockIdentity normalizeBlockIdentity(std::string_view raw_name, uint16_t aux,
                                               bool projection_source) {
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    if (name.empty()) return {name, aux};

    // These bits are driven by the world after placement. They are neither a
    // material variant nor a physical orientation, so a correct shell must not
    // turn yellow/red simply because the redstone graph currently differs from
    // the imported snapshot.  Keep the static parts exact: container-facing,
    // repeater delay and comparator mode remain visible and printable.
    uint16_t comparable_aux = aux;
    if (isButtonAttachmentName(name) || isLeverName(name) ||
        sixWayHasTransientBit(name)) {
        comparable_aux = static_cast<uint16_t>(aux & ~UINT16_C(0x0008));
    } else if (isSixWayFacingName(name) || isHorizontalSixWayFacingName(name)) {
        comparable_aux = static_cast<uint16_t>(aux & UINT16_C(0x0007));
    } else if (isRepeaterName(name)) {
        comparable_aux = static_cast<uint16_t>(aux & UINT16_C(0x000F));
    } else if (isComparatorName(name)) {
        comparable_aux = static_cast<uint16_t>(aux & UINT16_C(0x0007));
    } else if (isDaylightDetectorName(name)) {
        comparable_aux = 0U;
    }

    ProjectionBlockIdentity normalized;
    if ((projection_source &&
         projectionSourceAliasIdentity(name, comparable_aux, &normalized)) ||
        legacyWoodIdentity(name, comparable_aux, &normalized) ||
        namedPlanksIdentity(name, &normalized) ||
        // A live named double stone slab still has its material in aux, unlike
        // a live single stone_slab whose compact aux is its physical half.  It
        // is therefore safe (and necessary) to normalize the double family on
        // both sides of a projection-to-world comparison.
        ((projection_source || legacyDoubleSlabFamily(name)) &&
         legacyStoneSlabIdentity(name, comparable_aux, &normalized)) ||
        colorIdentity(name, comparable_aux, &normalized) ||
        legacyVariantIdentity(name, comparable_aux, &normalized) ||
        simpleVariantIdentity(name, comparable_aux, &normalized)) {
        return normalized;
    }
    if (flattenedDoubleSlabIdentity(name, &normalized)) return normalized;
    if (name == "hardened_clay" && comparable_aux == 0U) return {"terracotta", 0U};

    // Flattened leaves carry persistence/distance bookkeeping in the native
    // aux/state field, but it neither changes their item nor can it be chosen
    // by a placement click.  Treat every named leaf item as its material so a
    // hotbar snapshot such as `spruce_leaves + persistent` does not leave the
    // printer permanently waiting for a fictitious variant.  The legacy
    // leaves/leaves2 layout was normalized above by legacyWoodIdentity.
    if (endsWith(name, "_leaves") || name == "azalea_leaves_flowered") {
        return {name, 0U};
    }

    // A current flattened slab can be reported by the client with a compact
    // 0/1 half while an imported direct state uses legacy bit 3.  Normalize
    // only this one state bit; material variants in legacy slab families were
    // handled above and must never be mistaken for an upper half.
    if (namedSingleSlab(name) && (comparable_aux & kProjectionSlabDoubleAux) != 0U) {
        // A few decoded palette forms retain the single identifier while
        // storing type=double in an auxiliary bit. A double has no upper/lower
        // distinction, so fold every such form into the same full-block state.
        return {name, kProjectionSlabDoubleAux};
    }
    if (namedSingleSlab(name) && (!projection_source || !legacySlabFamily(name))) {
        // The current client represents a flattened slab half as 0/1, while
        // imported direct-state records can retain old bit 3.  Do not call
        // TryProjectionSlabPlacement here: for a *source* `stone_slab`, aux=1
        // is the old sandstone material, but for live `stone_slab`, aux=1 is
        // the ordinary stone slab's upper half.
        const bool compact_bottom = comparable_aux == 0U;
        const bool compact_top = comparable_aux == 1U ||
            comparable_aux == kProjectionSlabTopAux ||
            comparable_aux == static_cast<uint16_t>(kProjectionSlabTopAux | 1U);
        if (compact_bottom || compact_top) {
            const bool top = compact_top;
            return {name, static_cast<uint16_t>(top ? kProjectionSlabTopAux : 0U)};
        }
    }
    if (IsProjectionDoubleSlabBlock(name)) {
        return {name, static_cast<uint16_t>(comparable_aux | kProjectionSlabDoubleAux)};
    }
    return {name, comparable_aux};
}

}  // namespace

ProjectionBlockIdentity NormalizeProjectionBlockIdentity(std::string_view raw_name,
                                                          uint16_t aux) {
    return normalizeBlockIdentity(raw_name, aux, true);
}

ProjectionBlockIdentity NormalizeLiveBlockIdentity(std::string_view raw_name,
                                                    uint16_t aux) {
    return normalizeBlockIdentity(raw_name, aux, false);
}

bool ProjectionBlockIdentityMatches(std::string_view left_name, uint16_t left_aux,
                                    std::string_view right_name, uint16_t right_aux) {
    const ProjectionBlockIdentity left = NormalizeProjectionBlockIdentity(left_name, left_aux);
    const ProjectionBlockIdentity right = NormalizeLiveBlockIdentity(right_name, right_aux);
    return left.name == right.name && left.aux == right.aux;
}

bool ProjectionBlockMaterialMatches(std::string_view left_name, uint16_t left_aux,
                                     std::string_view right_name, uint16_t right_aux) {
    const ProjectionBlockIdentity left = NormalizeProjectionBlockIdentity(left_name, left_aux);
    const ProjectionBlockIdentity right = NormalizeLiveBlockIdentity(right_name, right_aux);
    if (left.name != right.name &&
        !(isDaylightDetectorName(left.name) && isDaylightDetectorName(right.name))) {
        return false;
    }
    if (isPlacementDerivedIdentity(left.name)) return true;
    return left.aux == right.aux;
}

bool ProjectionCanonicalMaterialMatchesLive(std::string_view material_name,
                                            uint16_t material_aux,
                                            std::string_view live_name,
                                            uint16_t live_aux) {
    // A material summary has already passed through NormalizeProjection...
    // while it was built. Treating it as a raw source again would reapply
    // legacy-only aliases (notably stone_stairs and legacy stone slabs).
    const ProjectionBlockIdentity material =
        NormalizeLiveBlockIdentity(material_name, material_aux);
    const ProjectionBlockIdentity live = NormalizeLiveBlockIdentity(live_name, live_aux);
    if (material.name != live.name &&
        !(isDaylightDetectorName(material.name) && isDaylightDetectorName(live.name))) {
        return false;
    }
    if (isPlacementDerivedIdentity(material.name)) return true;
    return material.aux == live.aux;
}

bool TryProjectionBlockAxis(std::string_view raw_name, uint16_t aux,
                            ProjectionBlockAxis* output) {
    if (!output) return false;
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    if (legacyLogName(name)) {
        if ((aux & ~UINT16_C(0x000F)) != 0U) return false;
        switch (aux & UINT16_C(0x000C)) {
            case 0U: *output = ProjectionBlockAxis::Y; return true;
            case 4U: *output = ProjectionBlockAxis::X; return true;
            case 8U: *output = ProjectionBlockAxis::Z; return true;
            default: return false;  // all-bark legacy wood, no physical axis
        }
    }
    if (name == "quartz_block" || name == "quartz_pillar") {
        if (aux == 2U) { *output = ProjectionBlockAxis::Y; return true; }
        if (aux == 6U) { *output = ProjectionBlockAxis::X; return true; }
        if (aux == 10U) { *output = ProjectionBlockAxis::Z; return true; }
        return false;
    }
    if (oldPillarName(name)) {
        if ((aux & ~UINT16_C(0x000C)) != 0U) return false;
        switch (aux & UINT16_C(0x000C)) {
            case 0U: *output = ProjectionBlockAxis::Y; return true;
            case 4U: *output = ProjectionBlockAxis::X; return true;
            case 8U: *output = ProjectionBlockAxis::Z; return true;
            default: return false;
        }
    }
    if (!modernAxisName(name) || (aux & ~UINT16_C(0x0003)) != 0U) return false;
    switch (aux & UINT16_C(0x0003)) {
        case 0U: *output = ProjectionBlockAxis::Y; return true;
        case 1U: *output = ProjectionBlockAxis::X; return true;
        case 2U: *output = ProjectionBlockAxis::Z; return true;
        default: return false;
    }
}

bool ReplaceProjectionBlockAxis(std::string_view raw_name, uint16_t source_aux,
                                ProjectionBlockAxis axis, uint16_t* output) {
    if (!output) return false;
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    if (legacyLogName(name)) {
        if ((source_aux & ~UINT16_C(0x000F)) != 0U) return false;
        const uint16_t encoded = axis == ProjectionBlockAxis::Y ? 0U :
            axis == ProjectionBlockAxis::X ? 4U : 8U;
        *output = static_cast<uint16_t>((source_aux & ~UINT16_C(0x000C)) | encoded);
        return true;
    }
    if (name == "quartz_block" || name == "quartz_pillar") {
        *output = axis == ProjectionBlockAxis::Y ? 2U :
            axis == ProjectionBlockAxis::X ? 6U : 10U;
        return true;
    }
    if (oldPillarName(name)) {
        if ((source_aux & ~UINT16_C(0x000C)) != 0U) return false;
        const uint16_t encoded = axis == ProjectionBlockAxis::Y ? 0U :
            axis == ProjectionBlockAxis::X ? 4U : 8U;
        *output = static_cast<uint16_t>((source_aux & ~UINT16_C(0x000C)) | encoded);
        return true;
    }
    if (!modernAxisName(name) || (source_aux & ~UINT16_C(0x0003)) != 0U) return false;
    *output = static_cast<uint16_t>((source_aux & ~UINT16_C(0x0003)) |
        static_cast<uint16_t>(axis));
    return true;
}

bool RotateProjectionBlockAxis(ProjectionBlockAxis source, uint8_t rotation_quarters,
                               ProjectionBlockAxis* output) noexcept {
    if (!output) return false;
    if ((rotation_quarters & 1U) == 0U || source == ProjectionBlockAxis::Y) {
        *output = source;
    } else {
        *output = source == ProjectionBlockAxis::X ? ProjectionBlockAxis::Z
                                                    : ProjectionBlockAxis::X;
    }
    return true;
}

bool RotateProjectionBlockAxisAux(std::string_view name, uint16_t source_aux,
                                  uint8_t rotation_quarters, uint16_t* output) {
    ProjectionBlockAxis source;
    ProjectionBlockAxis rotated;
    return TryProjectionBlockAxis(name, source_aux, &source) &&
        RotateProjectionBlockAxis(source, rotation_quarters, &rotated) &&
        ReplaceProjectionBlockAxis(name, source_aux, rotated, output);
}

bool RotateProjectionBlockFace(ProjectionBlockFace source, uint8_t rotation_quarters,
                               ProjectionBlockFace* output) noexcept {
    if (!output) return false;
    const uint8_t turns = rotation_quarters & 0x03U;
    ProjectionBlockFace value = source;
    for (uint8_t index = 0U; index < turns; ++index) {
        switch (value) {
            case ProjectionBlockFace::North: value = ProjectionBlockFace::East; break;
            case ProjectionBlockFace::East: value = ProjectionBlockFace::South; break;
            case ProjectionBlockFace::South: value = ProjectionBlockFace::West; break;
            case ProjectionBlockFace::West: value = ProjectionBlockFace::North; break;
            default: break;
        }
    }
    *output = value;
    return true;
}

bool TryProjectionBlockFace(std::string_view raw_name, uint16_t aux,
                            ProjectionBlockFace* output) {
    if (!output) return false;
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    if (name == "end_rod") {
        if ((aux & ~UINT16_C(0x0007)) != 0U) return false;
        switch (aux) {
            case 0U: *output = ProjectionBlockFace::Down; return true;
            case 1U: *output = ProjectionBlockFace::Up; return true;
            case 2U: *output = ProjectionBlockFace::South; return true;
            case 3U: *output = ProjectionBlockFace::North; return true;
            case 4U: *output = ProjectionBlockFace::East; return true;
            case 5U: *output = ProjectionBlockFace::West; return true;
            default: return false;
        }
    }
    if (name == "ladder") {
        if (aux < 2U || aux > 5U) return false;
        *output = static_cast<ProjectionBlockFace>(aux);
        return true;
    }
    if (!contains(name, "lightning_rod") || (aux & ~UINT16_C(0x000F)) != 0U ||
        (aux & UINT16_C(0x0007)) > 5U) {
        return false;
    }
    *output = static_cast<ProjectionBlockFace>(aux & UINT16_C(0x0007));
    return true;
}

bool ReplaceProjectionBlockFace(std::string_view raw_name, uint16_t source_aux,
                                ProjectionBlockFace face, uint16_t* output) {
    if (!output) return false;
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    if (name == "end_rod") {
        const uint16_t encoded = face == ProjectionBlockFace::Down ? 0U :
            face == ProjectionBlockFace::Up ? 1U :
            face == ProjectionBlockFace::South ? 2U :
            face == ProjectionBlockFace::North ? 3U :
            face == ProjectionBlockFace::East ? 4U : 5U;
        if ((source_aux & ~UINT16_C(0x0007)) != 0U) return false;
        *output = encoded;
        return true;
    }
    if (name == "ladder") {
        if (face != ProjectionBlockFace::North && face != ProjectionBlockFace::South &&
            face != ProjectionBlockFace::West && face != ProjectionBlockFace::East) {
            return false;
        }
        *output = static_cast<uint16_t>(face);
        return true;
    }
    if (!contains(name, "lightning_rod") || (source_aux & ~UINT16_C(0x000F)) != 0U) {
        return false;
    }
    *output = static_cast<uint16_t>((source_aux & ~UINT16_C(0x0007)) |
        static_cast<uint16_t>(face));
    return true;
}

bool RotateProjectionBlockFaceAux(std::string_view name, uint16_t source_aux,
                                  uint8_t rotation_quarters, uint16_t* output) {
    ProjectionBlockFace source;
    ProjectionBlockFace rotated;
    return TryProjectionBlockFace(name, source_aux, &source) &&
        RotateProjectionBlockFace(source, rotation_quarters, &rotated) &&
        ReplaceProjectionBlockFace(name, source_aux, rotated, output);
}

bool IsProjectionContentlessContainerShell(std::string_view raw_name) {
    return isContentlessContainerShellName(ProjectionCanonicalBlockName(raw_name));
}

bool TryProjectionSixWayFacing(std::string_view raw_name, uint16_t aux,
                               ProjectionBlockFace* output) {
    if (!output) return false;
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    if (!isSixWayFacingName(name)) return false;
    const uint16_t allowed = sixWayHasTransientBit(name) ? UINT16_C(0x000F)
                                                          : UINT16_C(0x0007);
    if ((aux & ~allowed) != 0U) return false;
    const uint16_t facing = aux & UINT16_C(0x0007);
    if (facing > static_cast<uint16_t>(ProjectionBlockFace::East) ||
        (name == "hopper" && facing == static_cast<uint16_t>(ProjectionBlockFace::Up))) {
        return false;
    }
    *output = static_cast<ProjectionBlockFace>(facing);
    return true;
}

bool ReplaceProjectionSixWayFacing(std::string_view raw_name, uint16_t source_aux,
                                   ProjectionBlockFace face, uint16_t* output) {
    if (!output) return false;
    ProjectionBlockFace ignored;
    if (!TryProjectionSixWayFacing(raw_name, source_aux, &ignored)) return false;
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    if (name == "hopper" && face == ProjectionBlockFace::Up) return false;
    *output = static_cast<uint16_t>((source_aux & ~UINT16_C(0x0007)) |
                                    static_cast<uint16_t>(face));
    return true;
}

bool RotateProjectionSixWayFacingAux(std::string_view name, uint16_t source_aux,
                                     uint8_t rotation_quarters, uint16_t* output) {
    ProjectionBlockFace source;
    ProjectionBlockFace rotated;
    return TryProjectionSixWayFacing(name, source_aux, &source) &&
        RotateProjectionBlockFace(source, rotation_quarters, &rotated) &&
        ReplaceProjectionSixWayFacing(name, source_aux, rotated, output);
}

bool TryProjectionHorizontalSixWayFacing(std::string_view raw_name, uint16_t aux,
                                         ProjectionBlockFace* output) {
    if (!output) return false;
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    if (!isHorizontalSixWayFacingName(name) || (aux & ~UINT16_C(0x0007)) != 0U ||
        aux < static_cast<uint16_t>(ProjectionBlockFace::North) ||
        aux > static_cast<uint16_t>(ProjectionBlockFace::East)) {
        return false;
    }
    *output = static_cast<ProjectionBlockFace>(aux);
    return true;
}

namespace {

bool replaceProjectionHorizontalSixWayFacing(std::string_view raw_name, uint16_t source_aux,
                                             ProjectionBlockFace face, uint16_t* output) {
    if (!output || (face != ProjectionBlockFace::North && face != ProjectionBlockFace::South &&
                    face != ProjectionBlockFace::West && face != ProjectionBlockFace::East)) {
        return false;
    }
    ProjectionBlockFace ignored;
    if (!TryProjectionHorizontalSixWayFacing(raw_name, source_aux, &ignored)) return false;
    *output = static_cast<uint16_t>(face);
    return true;
}

}  // namespace

bool RotateProjectionHorizontalSixWayFacingAux(std::string_view name,
                                               uint16_t source_aux,
                                               uint8_t rotation_quarters,
                                               uint16_t* output) {
    ProjectionBlockFace source;
    ProjectionBlockFace rotated;
    return TryProjectionHorizontalSixWayFacing(name, source_aux, &source) &&
        RotateProjectionBlockFace(source, rotation_quarters, &rotated) &&
        replaceProjectionHorizontalSixWayFacing(name, source_aux, rotated, output);
}

bool TryProjectionHorizontalSixWaySilentYaw(std::string_view name, uint16_t aux,
                                            uint8_t rotation_quarters,
                                            float* output_yaw) {
    if (!output_yaw) return false;
    uint16_t effective_aux = 0U;
    if (!RotateProjectionHorizontalSixWayFacingAux(name, aux, rotation_quarters,
                                                    &effective_aux)) {
        return false;
    }
    // Chest and furnace-family placement stores the direction opposite the
    // placing player's horizontal look. The packet hook applies this only to
    // the outgoing input packet, never to the visible camera.
    switch (static_cast<ProjectionBlockFace>(effective_aux)) {
        case ProjectionBlockFace::North: *output_yaw = 0.0F; return true;
        case ProjectionBlockFace::South: *output_yaw = 180.0F; return true;
        case ProjectionBlockFace::West: *output_yaw = -90.0F; return true;
        case ProjectionBlockFace::East: *output_yaw = 90.0F; return true;
        default: return false;
    }
}

bool TryProjectionAttachmentFace(std::string_view raw_name, uint16_t aux,
                                  ProjectionBlockFace* output) {
    if (!output) return false;
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    const bool button = isButtonAttachmentName(name);
    const bool torch = isTorchAttachmentName(name);
    if (!button && !torch) return false;
    // A button's bit 3 records only temporary redstone power. Torches use no
    // compatible high bit in the old layout, so reject one instead of making
    // a malformed source look like a legitimate attachment.
    const uint16_t allowed = button ? UINT16_C(0x000F) : UINT16_C(0x0007);
    if ((aux & ~allowed) != 0U) return false;
    const uint16_t facing = aux & UINT16_C(0x0007);
    switch (facing) {
        case 0U:
            if (!button) return false;
            *output = ProjectionBlockFace::Down;
            return true;
        case 1U: *output = ProjectionBlockFace::East; return true;
        case 2U: *output = ProjectionBlockFace::West; return true;
        case 3U: *output = ProjectionBlockFace::South; return true;
        case 4U: *output = ProjectionBlockFace::North; return true;
        case 5U: *output = ProjectionBlockFace::Up; return true;
        default: return false;
    }
}

bool ReplaceProjectionAttachmentFace(std::string_view raw_name, uint16_t source_aux,
                                     ProjectionBlockFace face, uint16_t* output) {
    if (!output) return false;
    ProjectionBlockFace ignored;
    if (!TryProjectionAttachmentFace(raw_name, source_aux, &ignored)) return false;
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    const bool button = isButtonAttachmentName(name);
    const uint16_t encoded = face == ProjectionBlockFace::Down ? 0U :
        face == ProjectionBlockFace::East ? 1U :
        face == ProjectionBlockFace::West ? 2U :
        face == ProjectionBlockFace::South ? 3U :
        face == ProjectionBlockFace::North ? 4U :
        face == ProjectionBlockFace::Up ? 5U : UINT16_C(0xFFFF);
    if (encoded == UINT16_C(0xFFFF) || (!button && encoded == 0U)) return false;
    *output = static_cast<uint16_t>((button ? (source_aux & UINT16_C(0x0008)) : 0U) |
                                    encoded);
    return true;
}

bool RotateProjectionAttachmentFaceAux(std::string_view name, uint16_t source_aux,
                                       uint8_t rotation_quarters, uint16_t* output) {
    ProjectionBlockFace source;
    ProjectionBlockFace rotated;
    return TryProjectionAttachmentFace(name, source_aux, &source) &&
        RotateProjectionBlockFace(source, rotation_quarters, &rotated) &&
        ReplaceProjectionAttachmentFace(name, source_aux, rotated, output);
}

bool TryProjectionLeverAttachmentFace(std::string_view raw_name, uint16_t aux,
                                      ProjectionBlockFace* output) {
    if (!output || !isLeverName(ProjectionCanonicalBlockName(raw_name)) ||
        (aux & ~UINT16_C(0x000F)) != 0U) {
        return false;
    }
    switch (aux & UINT16_C(0x0007)) {
        case 0U:
        case 7U: *output = ProjectionBlockFace::Down; return true;
        case 1U: *output = ProjectionBlockFace::East; return true;
        case 2U: *output = ProjectionBlockFace::West; return true;
        case 3U: *output = ProjectionBlockFace::South; return true;
        case 4U: *output = ProjectionBlockFace::North; return true;
        case 5U:
        case 6U: *output = ProjectionBlockFace::Up; return true;
        default: return false;
    }
}

bool RotateProjectionLeverAux(std::string_view raw_name, uint16_t source_aux,
                              uint8_t rotation_quarters, uint16_t* output) {
    if (!output) return false;
    ProjectionBlockFace ignored;
    if (!TryProjectionLeverAttachmentFace(raw_name, source_aux, &ignored)) return false;
    const uint16_t base = source_aux & UINT16_C(0x0007);
    uint16_t rotated = base;
    if ((rotation_quarters & 1U) != 0U) {
        if (base == 0U) rotated = 7U;
        else if (base == 7U) rotated = 0U;
        else if (base == 5U) rotated = 6U;
        else if (base == 6U) rotated = 5U;
        else {
            ProjectionBlockFace source_face;
            ProjectionBlockFace rotated_face;
            if (!TryProjectionLeverAttachmentFace(raw_name, source_aux, &source_face) ||
                !RotateProjectionBlockFace(source_face, rotation_quarters, &rotated_face)) {
                return false;
            }
            switch (rotated_face) {
                case ProjectionBlockFace::East: rotated = 1U; break;
                case ProjectionBlockFace::West: rotated = 2U; break;
                case ProjectionBlockFace::South: rotated = 3U; break;
                case ProjectionBlockFace::North: rotated = 4U; break;
                default: return false;
            }
        }
    }
    *output = static_cast<uint16_t>((source_aux & UINT16_C(0x0008)) | rotated);
    return true;
}

bool TryProjectionLeverSilentYaw(std::string_view raw_name, uint16_t aux,
                                 uint8_t rotation_quarters, float* output_yaw) {
    if (!output_yaw) return false;
    uint16_t effective_aux = 0U;
    if (!RotateProjectionLeverAux(raw_name, aux, rotation_quarters, &effective_aux)) {
        return false;
    }
    switch (effective_aux & UINT16_C(0x0007)) {
        // The two opposing yaw directions produce the same lever axis. Pick a
        // stable representative so the packet hook never visibly turns the
        // player's camera.
        case 0U:
        case 6U:
            *output_yaw = 90.0F;  // east/west axis
            return true;
        case 5U:
        case 7U:
            *output_yaw = 0.0F;   // north/south axis
            return true;
        default:
            return false;  // wall levers are selected entirely by click face
    }
}

bool IsProjectionYawDirectedBlock(std::string_view raw_name) {
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    return endsWith(name, "_glazed_terracotta") || name == "pumpkin" ||
        name == "lit_pumpkin" || name == "carved_pumpkin" ||
        name == "jack_o_lantern" || isAnvilName(name);
}

bool TryProjectionHorizontalDirection(std::string_view raw_name, uint16_t aux,
                                       uint8_t* output) {
    if (!output || !IsProjectionYawDirectedBlock(raw_name)) {
        return false;
    }
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    if (isAnvilName(name)) {
        if ((name == "anvil" &&
             ((aux & ~UINT16_C(0x000F)) != 0U ||
              (aux & UINT16_C(0x000C)) == UINT16_C(0x000C))) ||
            (name != "anvil" && (aux & ~UINT16_C(0x0003)) != 0U)) {
            return false;
        }
    } else if ((aux & ~UINT16_C(0x0003)) != 0U) {
        return false;
    }
    *output = static_cast<uint8_t>(aux & UINT16_C(0x0003));
    return true;
}

bool RotateProjectionHorizontalDirection(uint8_t source, uint8_t rotation_quarters,
                                         uint8_t* output) noexcept {
    if (!output || source > 3U) return false;
    *output = static_cast<uint8_t>((source + (rotation_quarters & 0x03U)) & 0x03U);
    return true;
}

bool RotateProjectionHorizontalDirectionAux(std::string_view name, uint16_t source_aux,
                                             uint8_t rotation_quarters,
                                             uint16_t* output) {
    if (!output) return false;
    uint8_t source = 0U;
    uint8_t rotated = 0U;
    if (!TryProjectionHorizontalDirection(name, source_aux, &source) ||
        !RotateProjectionHorizontalDirection(source, rotation_quarters, &rotated)) {
        return false;
    }
    *output = static_cast<uint16_t>((source_aux & ~UINT16_C(0x0003)) | rotated);
    return true;
}

bool TryProjectionRepeaterState(std::string_view raw_name, uint16_t aux,
                                uint8_t* direction, uint8_t* delay) {
    if (!direction || !delay || !isRepeaterName(ProjectionCanonicalBlockName(raw_name)) ||
        (aux & ~UINT16_C(0x001F)) != 0U) {
        return false;
    }
    *direction = static_cast<uint8_t>(aux & UINT16_C(0x0003));
    *delay = static_cast<uint8_t>(((aux >> 2U) & UINT16_C(0x0003)) + 1U);
    return true;
}

bool RotateProjectionRepeaterAux(std::string_view name, uint16_t source_aux,
                                 uint8_t rotation_quarters, uint16_t* output) {
    if (!output) return false;
    uint8_t source_direction = 0U;
    uint8_t ignored_delay = 0U;
    uint8_t rotated_direction = 0U;
    if (!TryProjectionRepeaterState(name, source_aux, &source_direction, &ignored_delay) ||
        !RotateProjectionHorizontalDirection(source_direction, rotation_quarters,
                                             &rotated_direction)) {
        return false;
    }
    *output = static_cast<uint16_t>((source_aux & ~UINT16_C(0x0003)) |
                                    rotated_direction);
    return true;
}

bool TryProjectionComparatorState(std::string_view raw_name, uint16_t aux,
                                  uint8_t* direction, bool* subtract_mode) {
    if (!direction || !subtract_mode ||
        !isComparatorName(ProjectionCanonicalBlockName(raw_name)) ||
        (aux & ~UINT16_C(0x000F)) != 0U) {
        return false;
    }
    *direction = static_cast<uint8_t>(aux & UINT16_C(0x0003));
    *subtract_mode = (aux & UINT16_C(0x0004)) != 0U;
    return true;
}

bool RotateProjectionComparatorAux(std::string_view name, uint16_t source_aux,
                                  uint8_t rotation_quarters, uint16_t* output) {
    if (!output) return false;
    uint8_t source_direction = 0U;
    uint8_t rotated_direction = 0U;
    bool ignored_subtract = false;
    if (!TryProjectionComparatorState(name, source_aux, &source_direction,
                                      &ignored_subtract) ||
        !RotateProjectionHorizontalDirection(source_direction, rotation_quarters,
                                             &rotated_direction)) {
        return false;
    }
    *output = static_cast<uint16_t>((source_aux & ~UINT16_C(0x0003)) |
                                    rotated_direction);
    return true;
}

bool IsProjectionDaylightDetector(std::string_view raw_name) {
    return isDaylightDetectorName(ProjectionCanonicalBlockName(raw_name));
}

bool IsProjectionInvertedDaylightDetector(std::string_view raw_name) {
    return isInvertedDaylightDetectorName(ProjectionCanonicalBlockName(raw_name));
}

}  // namespace build_import
