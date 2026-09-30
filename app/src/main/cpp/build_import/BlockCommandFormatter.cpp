#include "BlockCommandFormatter.h"

#include <algorithm>
#include <cctype>

namespace build_import {
namespace {

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
        value.substr(value.size() - suffix.size()) == suffix;
}

std::string normalizedLeaf(std::string_view name) {
    const size_t separator = name.rfind(':');
    std::string leaf(name.substr(separator == std::string_view::npos ? 0 : separator + 1));
    std::transform(leaf.begin(), leaf.end(), leaf.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return leaf;
}

const char* booleanState(bool value) {
    return value ? "true" : "false";
}

std::string explicitStateArgument(std::string_view name, uint16_t aux) {
    const std::string leaf = normalizedLeaf(name);
    std::string result(name);

    // These target families expose modern Bedrock states whose command
    // interpretation is not reliably equivalent to the packed numeric Aux.
    if (endsWith(leaf, "_hanging_sign")) {
        const uint16_t facing = aux & 0x0007U;
        if (facing > 5U) return {};
        result += " [\"attached_bit\"=";
        result += booleanState((aux & 0x0100U) != 0U);
        result += ",\"facing_direction\"=" + std::to_string(facing);
        result += ",\"ground_sign_direction\"=" +
            std::to_string((aux >> 3U) & 0x000fU);
        result += ",\"hanging\"=";
        result += booleanState((aux & 0x0080U) != 0U);
        result += "]";
        return result;
    }

    if (leaf == "standing_sign" || endsWith(leaf, "_standing_sign")) {
        result += " [\"ground_sign_direction\"=" +
            std::to_string(aux & 0x000fU) + "]";
        return result;
    }

    if (leaf == "wall_sign" || endsWith(leaf, "_wall_sign")) {
        const uint16_t facing = aux & 0x0007U;
        if (facing > 5U) return {};
        result += " [\"facing_direction\"=" + std::to_string(facing) + "]";
        return result;
    }

    if (leaf == "fence_gate" || endsWith(leaf, "_fence_gate")) {
        static constexpr const char* kCardinalDirections[] = {
            "south", "west", "north", "east",
        };
        result += " [\"in_wall_bit\"=";
        result += booleanState((aux & 0x0008U) != 0U);
        result += ",\"minecraft:cardinal_direction\"=\"";
        result += kCardinalDirections[aux & 0x0003U];
        result += "\",\"open_bit\"=";
        result += booleanState((aux & 0x0004U) != 0U);
        result += "]";
        return result;
    }

    // Stairs, doors and trapdoors added after Bedrock's block-state
    // flattening (pale_oak, resin_brick, cherry, mangrove, bamboo,
    // crimson/warped and every copper-family variant, among others) have no
    // legacy numeric-Aux compatibility mapping registered by the engine: a
    // trailing plain integer on /setblock is silently accepted but produces
    // the block's default orientation instead of the requested one.
    // Materials that predate flattening (oak, iron, stone/cobblestone
    // stairs, ...) happen to still work with the legacy numeric form because
    // the engine keeps a translation table for them, but emitting explicit
    // block-state brackets is correct and safe for both old and new
    // materials, matching the treatment already given to signs and fence
    // gates above.
    if (endsWith(leaf, "_stairs")) {
        // BlockMapper::stairIndex()/bedrockStairFacing(): east=0, west=1,
        // south=2, north=3 - the same ordering as the native
        // weirdo_direction property, so bits 0-1 pass through unchanged.
        result += " [\"upside_down_bit\"=";
        result += booleanState((aux & 0x0004U) != 0U);
        result += ",\"weirdo_direction\"=" + std::to_string(aux & 0x0003U);
        result += "]";
        return result;
    }

    if (endsWith(leaf, "_door") && !endsWith(leaf, "_trapdoor")) {
        // BlockMapper's Sponge-state door encoding stores, for the lower
        // half, (horizontalIndex(facing)+1)&3 in bits 0-1 and open in bit 2;
        // that numbering already matches the native cardinal_direction
        // property (east=0, south=1, west=2, north=3), so it passes through
        // unchanged. The upper half instead stores upper_block_bit in bit 3
        // and hinge in bit 0, with no facing of its own - Bedrock derives the
        // upper half's visual orientation from the lower half at runtime.
        // The lower half's own hinge (needed so a *closed* door swings to the
        // correct side instead of defaulting to left) is stashed in bit 4 by
        // BlockMapper's door decoder, since bits 0-2 are already spoken for.
        const bool upper = (aux & 0x0008U) != 0U;
        result += " [\"door_hinge_bit\"=";
        result += booleanState(upper ? (aux & 0x0001U) != 0U : (aux & 0x0010U) != 0U);
        if (!upper) {
            result += ",\"minecraft:cardinal_direction\"=\"";
            static constexpr const char* kCardinalDirections[] = {
                "east", "south", "west", "north",
            };
            result += kCardinalDirections[aux & 0x0003U];
            result += "\"";
        }
        result += ",\"open_bit\"=";
        result += booleanState(!upper && (aux & 0x0004U) != 0U);
        result += ",\"upper_block_bit\"=";
        result += booleanState(upper);
        result += "]";
        return result;
    }

    if (leaf == "trapdoor" || endsWith(leaf, "_trapdoor")) {
        // BlockMapper's Sponge-state trapdoor branch encodes facing with
        // trapdoorFacingIndex() (south=0, north=1, east=2, west=3), a
        // different ordering than the native cardinal_direction property
        // (east=0, south=1, west=2, north=3), so it must be remapped rather
        // than passed through directly.
        static constexpr const char* kCardinalDirections[] = {
            "south", "north", "east", "west",
        };
        result += " [\"minecraft:cardinal_direction\"=\"";
        result += kCardinalDirections[aux & 0x0003U];
        result += "\",\"open_bit\"=";
        result += booleanState((aux & 0x0008U) != 0U);
        result += ",\"upside_down_bit\"=";
        result += booleanState((aux & 0x0004U) != 0U);
        result += "]";
        return result;
    }

    return {};
}

}  // namespace

std::string formatPlacementBlockArgument(std::string_view name, uint16_t aux) {
    if (std::string explicit_state = explicitStateArgument(name, aux);
        !explicit_state.empty()) {
        return explicit_state;
    }
    std::string result(name);
    if (aux != 0U) result += " " + std::to_string(aux);
    return result;
}

std::string formatVerificationBlockArgument(std::string_view name, uint16_t aux,
                                            bool ignore_aux) {
    if (ignore_aux) return std::string(name) + " -1";
    return formatPlacementBlockArgument(name, aux);
}

}  // namespace build_import
