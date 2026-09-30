#include "SchematicWriter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <zlib.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace build_import {
namespace {

enum NbtTag : uint8_t {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    ByteArray = 7,
    String = 8,
    List = 9,
    Compound = 10,
    IntArray = 11,
};

bool cancelled(const SchematicWriteRequest& request) {
    return request.cancellation_requested && request.cancellation_requested();
}

bool syncFile(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "r+b");
    if (!file) return false;
#if defined(_WIN32)
    const bool synced = _commit(_fileno(file)) == 0;
#else
    const bool synced = fsync(fileno(file)) == 0;
#endif
    const bool closed = std::fclose(file) == 0;
    return synced && closed;
}

bool replaceFileAtomically(const std::string& source, const std::string& destination) {
#if defined(_WIN32)
    return MoveFileExA(source.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    return std::rename(source.c_str(), destination.c_str()) == 0;
#endif
}

void syncParentDirectory(const std::string& path) {
#if !defined(_WIN32)
    const size_t separator = path.find_last_of('/');
    const std::string parent = separator == std::string::npos ? "." :
        (separator == 0 ? "/" : path.substr(0, separator));
    const int descriptor = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor >= 0) {
        fsync(descriptor);
        close(descriptor);
    }
#else
    (void)path;
#endif
}

class GzipNbtWriter {
public:
    explicit GzipNbtWriter(const std::string& path) : file_(gzopen(path.c_str(), "wb6")) {}
    ~GzipNbtWriter() {
        if (file_) gzclose(file_);
    }

    bool valid() const { return file_ != nullptr; }
    bool failed() const { return failed_; }

    bool close() {
        if (!file_) return !failed_;
        const int result = gzclose(file_);
        file_ = nullptr;
        failed_ = failed_ || result != Z_OK;
        return !failed_;
    }

    bool raw(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        while (size != 0 && !failed_) {
            const unsigned chunk = static_cast<unsigned>(std::min<size_t>(
                size, static_cast<size_t>(std::numeric_limits<int>::max())));
            if (gzwrite(file_, bytes, chunk) != static_cast<int>(chunk)) {
                failed_ = true;
                break;
            }
            bytes += chunk;
            size -= chunk;
        }
        return !failed_;
    }

    bool u8(uint8_t value) { return raw(&value, sizeof(value)); }

    bool i16(int16_t value) {
        const uint16_t bits = static_cast<uint16_t>(value);
        const uint8_t bytes[] = {
            static_cast<uint8_t>((bits >> 8U) & 0xffU),
            static_cast<uint8_t>(bits & 0xffU),
        };
        return raw(bytes, sizeof(bytes));
    }

    bool i32(int32_t value) {
        const uint32_t bits = static_cast<uint32_t>(value);
        const uint8_t bytes[] = {
            static_cast<uint8_t>((bits >> 24U) & 0xffU),
            static_cast<uint8_t>((bits >> 16U) & 0xffU),
            static_cast<uint8_t>((bits >> 8U) & 0xffU),
            static_cast<uint8_t>(bits & 0xffU),
        };
        return raw(bytes, sizeof(bytes));
    }

    bool utf(std::string_view value) {
        if (value.size() > std::numeric_limits<uint16_t>::max()) {
            failed_ = true;
            return false;
        }
        return i16(static_cast<int16_t>(value.size())) && raw(value.data(), value.size());
    }

    bool tag(uint8_t type, std::string_view name) {
        return u8(type) && utf(name);
    }

    bool taggedInt(std::string_view name, int32_t value) {
        return tag(Int, name) && i32(value);
    }

    bool taggedShort(std::string_view name, int16_t value) {
        return tag(Short, name) && i16(value);
    }

    bool taggedString(std::string_view name, std::string_view value) {
        return tag(String, name) && utf(value);
    }

    bool taggedByte(std::string_view name, uint8_t value) {
        return tag(Byte, name) && u8(value);
    }

    bool taggedByteArray(std::string_view name, std::string_view value) {
        return tag(ByteArray, name) &&
            value.size() <= static_cast<size_t>(std::numeric_limits<int32_t>::max()) &&
            i32(static_cast<int32_t>(value.size())) && raw(value.data(), value.size());
    }

private:
    gzFile file_ = nullptr;
    bool failed_ = false;
};

size_t varIntSize(uint32_t value) {
    size_t size = 1;
    while (value >= 0x80U) {
        value >>= 7U;
        ++size;
    }
    return size;
}

void appendVarInt(std::vector<uint8_t>* output, uint32_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7fU);
        value >>= 7U;
        if (value != 0) byte |= 0x80U;
        output->push_back(byte);
    } while (value != 0);
}

bool hasSchemExtension(const std::string& path) {
    constexpr std::string_view extension = ".schem";
    if (path.size() < extension.size()) return false;
    const size_t start = path.size() - extension.size();
    for (size_t index = 0; index < extension.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(path[start + index]);
        if (static_cast<char>(std::tolower(character)) != extension[index]) return false;
    }
    return true;
}

std::string stripLegacyDecoration(std::string value) {
    if (value.rfind("tile.", 0) == 0) value.erase(0, 5);
    constexpr std::string_view suffix = ".name";
    if (value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
        value.resize(value.size() - suffix.size());
    }
    if (value.find(':') == std::string::npos) value.insert(0, "minecraft:");
    return value;
}

std::string_view leafOf(const std::string& name) {
    const size_t separator = name.rfind(':');
    return separator == std::string::npos ? std::string_view(name)
                                          : std::string_view(name).substr(separator + 1);
}

bool isAir(std::string_view name) {
    return name == "air" || name == "minecraft:air" ||
           name == "cave_air" || name == "minecraft:cave_air" ||
           name == "void_air" || name == "minecraft:void_air";
}

const char* colorName(uint16_t aux) {
    static constexpr const char* colors[] = {
        "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
        "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
    };
    return colors[aux & 0x0fU];
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

std::string javaState(std::string_view leaf, std::string_view properties = {}) {
    std::string result = "minecraft:";
    result.append(leaf.data(), leaf.size());
    if (!properties.empty()) {
        result.push_back('[');
        result.append(properties.data(), properties.size());
        result.push_back(']');
    }
    return result;
}

const char* horizontalFacing(uint16_t data) {
    // BlockMapper::horizontalIndex(): south=0, west=1, north=2, east=3.
    static constexpr std::array<const char*, 4> kFacing{{"south", "west", "north", "east"}};
    return kFacing[data & 0x03U];
}

const char* doorFacing(uint16_t data) {
    // Native lower door data stores (horizontalIndex(facing) + 1) & 3.
    static constexpr std::array<const char*, 4> kFacing{{"east", "south", "west", "north"}};
    return kFacing[data & 0x03U];
}

const char* stairFacing(uint16_t data) {
    // BlockMapper::stairIndex(): east=0, west=1, south=2, north=3.
    static constexpr std::array<const char*, 4> kFacing{{"east", "west", "south", "north"}};
    return kFacing[data & 0x03U];
}

const char* trapdoorFacing(uint16_t data) {
    // BlockMapper::trapdoorFacingIndex(): south=0, north=1, east=2, west=3.
    static constexpr std::array<const char*, 4> kFacing{{"south", "north", "east", "west"}};
    return kFacing[data & 0x03U];
}

const char* wallFacing(uint16_t data) {
    switch (data & 0x07U) {
        case 2: return "north";
        case 3: return "south";
        case 4: return "west";
        case 5: return "east";
        default: return "north";
    }
}

const char* hangingSignEdgeRotation(uint16_t data) {
    // Edge-mounted ceiling signs store only facing_direction in bits 0..2.
    // Java's ceiling-edge variant uses the equivalent cardinal rotations.
    switch (data & 0x07U) {
        case 3: return "0";   // south
        case 4: return "4";   // west
        case 2: return "8";   // north
        case 5: return "12";  // east
        default: return "0";
    }
}

const char* wallAttachmentFacing(uint16_t data) {
    // BlockMapper::wallAttachmentIndex(): east=1, west=2, south=3, north=4.
    switch (data & 0x07U) {
        case 1: return "east";
        case 2: return "west";
        case 3: return "south";
        case 4: return "north";
        default: return "north";
    }
}

const char* sixWayFacing(uint16_t data) {
    static constexpr std::array<const char*, 6> kFacing{{
        "down", "up", "north", "south", "west", "east",
    }};
    return kFacing[(data < kFacing.size()) ? data : 2U];
}

const char* endRodFacing(uint16_t data) {
    static constexpr std::array<const char*, 6> kFacing{{
        "down", "up", "south", "north", "east", "west",
    }};
    return kFacing[(data < kFacing.size()) ? data : 1U];
}

const char* axisForFlattenedPillar(uint16_t data) {
    // Old target states use 4/8 while flattened states use 1/2.  Accept both
    // representations because the world-access hook can expose either form.
    if ((data & 0x0cU) == 4U || (data & 0x03U) == 1U) return "x";
    if ((data & 0x0cU) == 8U || (data & 0x03U) == 2U) return "z";
    return "y";
}

std::string standardStairLeaf(std::string_view leaf) {
    if (leaf == "normal_stone_stairs") return "stone_stairs";
    if (leaf == "stone_stairs") return "cobblestone_stairs";
    if (leaf == "end_brick_stairs") return "end_stone_brick_stairs";
    if (leaf == "prismarine_bricks_stairs") return "prismarine_brick_stairs";
    return std::string(leaf);
}

std::string standardLeaf(std::string_view leaf) {
    if (startsWith(leaf, "darkoak_")) {
        return "dark_oak_" + std::string(leaf.substr(std::string_view("darkoak_").size()));
    }
    if (leaf == "azalea_leaves_flowered") return "flowering_azalea_leaves";
    return std::string(leaf);
}

bool legacySlabState(std::string_view leaf, uint16_t data, std::string* output) {
    static constexpr std::array<const char*, 8> kStoneSlabs{{
        "smooth_stone_slab", "sandstone_slab", "petrified_oak_slab",
        "cobblestone_slab", "brick_slab", "stone_brick_slab", "quartz_slab",
        "nether_brick_slab",
    }};
    static constexpr std::array<const char*, 8> kStoneSlabs2{{
        "red_sandstone_slab", "purpur_slab", "prismarine_slab",
        "dark_prismarine_slab", "prismarine_brick_slab", "mossy_cobblestone_slab",
        "smooth_sandstone_slab", "red_nether_brick_slab",
    }};
    static constexpr std::array<const char*, 8> kStoneSlabs3{{
        "end_stone_brick_slab", "smooth_red_sandstone_slab", "polished_andesite_slab",
        "andesite_slab", "diorite_slab", "polished_diorite_slab", "granite_slab",
        "polished_granite_slab",
    }};
    static constexpr std::array<const char*, 5> kStoneSlabs4{{
        "mossy_stone_brick_slab", "smooth_quartz_slab", "stone_slab",
        "cut_sandstone_slab", "cut_red_sandstone_slab",
    }};
    static constexpr std::array<const char*, 6> kWoodSlabs{{
        "oak_slab", "spruce_slab", "birch_slab", "jungle_slab", "acacia_slab",
        "dark_oak_slab",
    }};
    struct Group {
        std::string_view single;
        std::string_view doubled;
        const char* const* variants;
        size_t count;
    };
    static constexpr std::array<Group, 5> kGroups{{
        {"stone_slab", "double_stone_slab", kStoneSlabs.data(), kStoneSlabs.size()},
        {"stone_slab2", "double_stone_slab2", kStoneSlabs2.data(), kStoneSlabs2.size()},
        {"stone_block_slab3", "double_stone_block_slab3", kStoneSlabs3.data(), kStoneSlabs3.size()},
        {"stone_block_slab4", "double_stone_block_slab4", kStoneSlabs4.data(), kStoneSlabs4.size()},
        {"wooden_slab", "double_wooden_slab", kWoodSlabs.data(), kWoodSlabs.size()},
    }};
    for (const Group& group : kGroups) {
        const bool doubled = leaf == group.doubled;
        if (!doubled && leaf != group.single) continue;
        const size_t variant = data & 0x07U;
        if (variant >= group.count) return false;
        const char* const type = doubled ? "double" : ((data & 0x08U) != 0 ? "top" : "bottom");
        *output = javaState(group.variants[variant],
                            std::string("type=") + type + ",waterlogged=false");
        return true;
    }
    return false;
}

std::string slabState(std::string_view raw_leaf, uint16_t data) {
    std::string legacy;
    if (legacySlabState(raw_leaf, data, &legacy)) return legacy;

    std::string leaf = standardLeaf(raw_leaf);
    bool doubled = false;
    constexpr std::string_view kDoubleSuffix = "_double_slab";
    constexpr std::string_view kDoubleCopperSuffix = "_double_cut_copper_slab";
    if (startsWith(leaf, "double_")) {
        leaf.erase(0, std::string_view("double_").size());
        doubled = true;
    } else if (endsWith(leaf, kDoubleCopperSuffix)) {
        leaf.resize(leaf.size() - kDoubleCopperSuffix.size());
        leaf += "_cut_copper_slab";
        doubled = true;
    } else if (endsWith(leaf, kDoubleSuffix)) {
        leaf.resize(leaf.size() - kDoubleSuffix.size());
        leaf += "_slab";
        doubled = true;
    }
    if (leaf == "normal_stone_slab") leaf = "stone_slab";
    if (leaf == "prismarine_bricks_slab") leaf = "prismarine_brick_slab";
    if (!endsWith(leaf, "_slab")) return javaState(leaf);

    const char* const type = doubled ? "double" :
        (((data & 0x09U) != 0) ? "top" : "bottom");
    return javaState(leaf, std::string("type=") + type + ",waterlogged=false");
}

}  // namespace

std::string SchematicWriter::blockStateForExport(std::string raw_name, uint16_t aux,
                                                   int32_t legacy_id) {
    std::string identifier = stripLegacyDecoration(std::move(raw_name));
    std::string leaf(leafOf(identifier));
    if (identifier.empty() || isAir(identifier)) return "minecraft:air";

    if (leaf.find("slab") == std::string::npos) {
        if (legacy_id == 44) leaf = "stone_slab";
        else if (legacy_id == 43) leaf = "double_stone_slab";
        else if (legacy_id == 126) leaf = "wooden_slab";
        else if (legacy_id == 125) leaf = "double_wooden_slab";
        else if (legacy_id == 182) leaf = "stone_slab2";
        else if (legacy_id == 181) leaf = "double_stone_slab2";
        if (leaf != leafOf(identifier)) identifier = "minecraft:" + leaf;
    }

    if (leaf == "wool" || leaf == "carpet" || leaf == "concrete" ||
        leaf == "stained_glass" || leaf == "stained_glass_pane") {
        return "minecraft:" + std::string(colorName(aux)) + "_" + leaf;
    }
    if (leaf == "concretePowder" || leaf == "concrete_powder") {
        return "minecraft:" + std::string(colorName(aux)) + "_concrete_powder";
    }
    if (leaf == "stained_hardened_clay") {
        return "minecraft:" + std::string(colorName(aux)) + "_terracotta";
    }

    static constexpr const char* woods[] = {
        "oak", "spruce", "birch", "jungle", "acacia", "dark_oak",
    };
    const uint16_t data = aux & 0x0fU;
    if (leaf == "command_block" || leaf == "repeating_command_block" ||
        leaf == "chain_command_block") {
        // Keep the command-block shell state in the palette.  The editable
        // command text is emitted separately by BDX, but preserving these two
        // fields lets a resumed export reconstruct the same shell instead of
        // silently resetting its direction or conditional bit.
        return javaState(leaf, std::string("conditional=") +
                                     ((aux & 0x08U) != 0U ? "true" : "false") +
                                     ",facing=" + sixWayFacing(aux & 0x07U));
    }
    if (leaf == "planks" && data < 6) {
        return "minecraft:" + std::string(woods[data]) + "_planks";
    }
    if (leaf == "log") {
        std::string state = "minecraft:" + std::string(woods[data & 3U]) +
            ((data & 12U) == 12U ? "_wood" : "_log");
        state += "[axis=";
        state += (data & 12U) == 4U ? "x" : (data & 12U) == 8U ? "z" : "y";
        return state + "]";
    }
    if (leaf == "log2") {
        std::string state = "minecraft:" + std::string(woods[(data & 1U) ? 5 : 4]) +
            ((data & 12U) == 12U ? "_wood" : "_log");
        state += "[axis=";
        state += (data & 12U) == 4U ? "x" : (data & 12U) == 8U ? "z" : "y";
        return state + "]";
    }
    if (leaf == "stone") {
        static constexpr const char* variants[] = {
            "stone", "granite", "polished_granite", "diorite",
            "polished_diorite", "andesite", "polished_andesite",
        };
        if (data < 7) return "minecraft:" + std::string(variants[data]);
    }
    if (leaf == "dirt") {
        if (data == 1) return "minecraft:coarse_dirt";
        if (data == 2) return "minecraft:podzol";
    }
    if (leaf == "sand" && data == 1) return "minecraft:red_sand";

    // The world hook exposes target-version names and auxiliary values, whereas
    // Sponge palettes require Java block-state names.  Do not put target aux in
    // a private property: downstream applications then see a non-standard
    // palette and strict imports fail.  Serialize the inverse of the mapper's
    // stateful paths instead, falling back only to a normal property-free Java
    // identifier for blocks that have no state in the target registry.
    std::string java_leaf = standardLeaf(leaf);
    if (java_leaf == "standing_sign") {
        java_leaf = "oak_sign";
    } else if (endsWith(java_leaf, "_standing_sign")) {
        java_leaf.resize(java_leaf.size() - std::string_view("_standing_sign").size());
        java_leaf += "_sign";
    }

    if (leaf == "torch" || leaf == "soul_torch") {
        if (data >= 1U && data <= 4U) {
            return javaState(leaf == "soul_torch" ? "soul_wall_torch" : "wall_torch",
                             std::string("facing=") + wallAttachmentFacing(data));
        }
        return javaState(leaf);
    }
    if (leaf == "redstone_torch" || leaf == "unlit_redstone_torch") {
        const bool lit = leaf == "redstone_torch";
        if (data >= 1U && data <= 4U) {
            return javaState("redstone_wall_torch",
                             std::string("facing=") + wallAttachmentFacing(data) +
                                 ",lit=" + (lit ? "true" : "false"));
        }
        return javaState("redstone_torch", std::string("lit=") +
                                               (lit ? "true" : "false"));
    }
    if (leaf == "lever") {
        const uint16_t direction = data & 0x07U;
        const char* face = "wall";
        const char* facing = wallAttachmentFacing(direction);
        if (direction == 0U || direction == 7U) {
            face = "ceiling";
            facing = direction == 0U ? "east" : "north";
        } else if (direction == 5U || direction == 6U) {
            face = "floor";
            facing = direction == 6U ? "east" : "north";
        }
        return javaState("lever", std::string("face=") + face + ",facing=" + facing +
                                      ",powered=" +
                                      ((data & 0x08U) != 0U ? "true" : "false"));
    }
    if (leaf == "wooden_button" || leaf == "stone_button" ||
        endsWith(leaf, "_button")) {
        const uint16_t direction = data & 0x07U;
        const char* face = "wall";
        const char* facing = wallAttachmentFacing(direction);
        if (direction == 0U) {
            face = "ceiling";
            facing = "north";
        } else if (direction == 5U) {
            face = "floor";
            facing = "north";
        }
        if (java_leaf == "wooden_button") java_leaf = "oak_button";
        return javaState(java_leaf, std::string("face=") + face + ",facing=" + facing +
                                        ",powered=" +
                                        ((data & 0x08U) != 0U ? "true" : "false"));
    }
    if (leaf == "unpowered_repeater" || leaf == "powered_repeater") {
        return javaState("repeater",
                         "delay=" + std::to_string(((data >> 2U) & 0x03U) + 1U) +
                             ",facing=" + horizontalFacing(data) + ",locked=false,powered=" +
                             (leaf == "powered_repeater" ? "true" : "false"));
    }
    if (leaf == "unpowered_comparator" || leaf == "powered_comparator") {
        return javaState("comparator", std::string("facing=") + horizontalFacing(data) +
                                           ",mode=" +
                                           ((data & 0x04U) != 0U ? "subtract" : "compare") +
                                           ",powered=" +
                                           (leaf == "powered_comparator" ? "true" : "false"));
    }
    if (leaf == "piston" || leaf == "sticky_piston") {
        return javaState(leaf, std::string("extended=") +
                                   ((data & 0x08U) != 0U ? "true" : "false") +
                                   ",facing=" + sixWayFacing(data & 0x07U));
    }
    if (leaf == "piston_arm_collision" || leaf == "sticky_piston_arm_collision") {
        return javaState("piston_head", std::string("facing=") +
                                            sixWayFacing(data & 0x07U) +
                                            ",short=false,type=" +
                                            (leaf == "sticky_piston_arm_collision"
                                                 ? "sticky" : "normal"));
    }
    if (leaf == "dropper" || leaf == "dispenser") {
        return javaState(leaf, std::string("facing=") + sixWayFacing(data & 0x07U) +
                                   ",triggered=" +
                                   ((data & 0x08U) != 0U ? "true" : "false"));
    }
    if (leaf == "hopper") {
        return javaState(leaf, std::string("enabled=") +
                                   ((data & 0x08U) != 0U ? "false" : "true") +
                                   ",facing=" + sixWayFacing(data & 0x07U));
    }
    if (leaf == "observer") {
        return javaState(leaf, std::string("facing=") + sixWayFacing(data & 0x07U) +
                                   ",powered=" +
                                   ((data & 0x08U) != 0U ? "true" : "false"));
    }
    if (leaf == "redstone_lamp" || leaf == "lit_redstone_lamp") {
        return javaState("redstone_lamp", std::string("lit=") +
                                              (leaf == "lit_redstone_lamp"
                                                   ? "true" : "false"));
    }
    if (leaf == "daylight_detector" || leaf == "daylight_detector_inverted") {
        return javaState("daylight_detector", std::string("inverted=") +
                                                (leaf == "daylight_detector_inverted"
                                                     ? "true" : "false") +
                                                ",power=" +
                                                std::to_string(data & 0x0fU));
    }

    if (java_leaf.find("slab") != std::string::npos) {
        return slabState(leaf, data);
    }
    if (java_leaf.find("stairs") != std::string::npos) {
        const std::string state_leaf = standardStairLeaf(java_leaf);
        return javaState(state_leaf, std::string("facing=") + stairFacing(data) +
                                     ",half=" + ((data & 0x04U) != 0 ? "top" : "bottom") +
                                     ",shape=straight,waterlogged=false");
    }
    if (endsWith(java_leaf, "_door") && !endsWith(java_leaf, "_trapdoor")) {
        if (java_leaf == "wooden_door") java_leaf = "oak_door";
        const bool upper = (data & 0x08U) != 0;
        return javaState(java_leaf, std::string("facing=") + doorFacing(data) +
                                    ",half=" + (upper ? "upper" : "lower") +
                                    ",hinge=" + ((data & 0x01U) != 0 ? "right" : "left") +
                                    ",open=" + ((data & 0x04U) != 0 ? "true" : "false") +
                                    ",powered=" + ((data & 0x02U) != 0 ? "true" : "false"));
    }
    if (java_leaf == "trapdoor" || endsWith(java_leaf, "_trapdoor")) {
        if (java_leaf == "trapdoor") java_leaf = "oak_trapdoor";
        return javaState(java_leaf, std::string("facing=") + trapdoorFacing(data) +
                                    ",half=" + ((data & 0x04U) != 0 ? "top" : "bottom") +
                                    ",open=" + ((data & 0x08U) != 0 ? "true" : "false") +
                                    ",powered=false,waterlogged=false");
    }
    if (java_leaf == "fence_gate" || endsWith(java_leaf, "_fence_gate")) {
        // Legacy Bedrock gate data stores horizontal facing in bits 0..1,
        // open in bit 2, and the in-wall placement flag in bit 3.  Emit all
        // three Java properties so a gate is not silently reset to the
        // default south-facing, closed state by the Sponge importer.
        if (java_leaf == "fence_gate") java_leaf = "oak_fence_gate";
        return javaState(java_leaf, std::string("facing=") + horizontalFacing(data) +
                                    ",in_wall=" + ((data & 0x08U) != 0 ? "true" : "false") +
                                    ",open=" + ((data & 0x04U) != 0 ? "true" : "false") +
                                    ",powered=false");
    }
    if (endsWith(java_leaf, "_log") || endsWith(java_leaf, "_wood")) {
        return javaState(java_leaf, std::string("axis=") + axisForFlattenedPillar(data));
    }
    if (endsWith(java_leaf, "_leaves")) {
        return javaState(java_leaf,
                         std::string("distance=7,persistent=") +
                             (((data & 0x06U) != 0) ? "true" : "false") +
                             ",waterlogged=false");
    }
    if (endsWith(java_leaf, "_wall")) {
        return javaState(java_leaf,
                         "east=none,north=none,south=none,up=false,waterlogged=false,west=none");
    }
    if (java_leaf == "glass_pane" || endsWith(java_leaf, "_stained_glass_pane") ||
        java_leaf == "iron_bars" || java_leaf == "nether_brick_fence" ||
        endsWith(java_leaf, "_fence")) {
        return javaState(java_leaf,
                         "east=false,north=false,south=false,waterlogged=false,west=false");
    }
    if (java_leaf == "redstone_wire") {
        // Sponge palettes require a Java block state.  A bare
        // `minecraft:redstone_wire` is accepted on import, but emitting the
        // explicit power/default connections keeps exported files portable and
        // preserves the target's auxiliary power value.
        return javaState(java_leaf,
                         "east=none,north=none,power=" +
                             std::to_string(data & 0x0fU) +
                             ",south=none,west=none");
    }
    if (java_leaf == "water" || java_leaf == "flowing_water" ||
        java_leaf == "lava" || java_leaf == "flowing_lava") {
        return javaState(java_leaf, "level=" + std::to_string(data));
    }
    if (java_leaf == "anvil" || java_leaf == "chipped_anvil" ||
        java_leaf == "damaged_anvil") {
        return javaState(java_leaf, std::string("facing=") + horizontalFacing(data));
    }
    if (java_leaf == "barrel") {
        return javaState(java_leaf, std::string("facing=") + sixWayFacing(data) + ",open=false");
    }
    if (java_leaf == "shulker_box" || endsWith(java_leaf, "_shulker_box")) {
        return javaState(java_leaf, std::string("facing=") + sixWayFacing(data));
    }
    if (java_leaf == "end_rod") {
        return javaState(java_leaf, std::string("facing=") + endRodFacing(data));
    }
    if (java_leaf == "ladder") {
        return javaState(java_leaf, std::string("facing=") + wallFacing(data) +
                                    ",waterlogged=false");
    }
    if (endsWith(java_leaf, "_hanging_sign") &&
        !endsWith(java_leaf, "_wall_hanging_sign")) {
        // Bedrock packs facing_direction into bits 0..2,
        // ground_sign_direction into bits 3..6, hanging into bit 7, and
        // attached_bit into bit 8. Java represents wall and ceiling variants
        // as different block identifiers.
        if ((aux & 0x0080U) != 0U) {
            const bool attached = (aux & 0x0100U) != 0U;
            const std::string rotation = attached
                ? std::to_string((aux >> 3U) & 0x0fU)
                : hangingSignEdgeRotation(aux);
            return javaState(java_leaf,
                             std::string("attached=") + (attached ? "true" : "false") +
                                 ",rotation=" + rotation +
                                 ",waterlogged=false");
        }
        constexpr std::string_view kHangingSignSuffix = "_hanging_sign";
        std::string wall_leaf =
            java_leaf.substr(0, java_leaf.size() - kHangingSignSuffix.size());
        wall_leaf += "_wall_hanging_sign";
        return javaState(wall_leaf, std::string("facing=") + wallFacing(aux) +
                                        ",waterlogged=false");
    }
    if (java_leaf == "wall_sign" || endsWith(java_leaf, "_wall_sign")) {
        return javaState(java_leaf, std::string("facing=") + wallFacing(data) +
                                    ",waterlogged=false");
    }
    if (java_leaf == "standing_sign" ||
        (endsWith(java_leaf, "_sign") && !endsWith(java_leaf, "_hanging_sign"))) {
        return javaState(java_leaf, "rotation=" + std::to_string(data & 0x0fU) +
                                    ",waterlogged=false");
    }
    if (java_leaf == "rail" || java_leaf == "golden_rail" ||
        java_leaf == "powered_rail" || java_leaf == "detector_rail" ||
        java_leaf == "activator_rail") {
        static constexpr std::array<const char*, 10> kRailShapes{{
            "north_south", "east_west", "ascending_east", "ascending_west",
            "ascending_north", "ascending_south", "south_east", "south_west",
            "north_west", "north_east",
        }};
        const bool ordinary = java_leaf == "rail";
        uint16_t shape = data & 0x07U;
        if (shape >= kRailShapes.size() || (!ordinary && shape > 5U)) shape = 0;
        if (java_leaf == "golden_rail") java_leaf = "powered_rail";
        return javaState(java_leaf, std::string("shape=") + kRailShapes[shape] +
                                    ",powered=" + ((data & 0x08U) != 0 ? "true" : "false") +
                                    ",waterlogged=false");
    }
    if (java_leaf == "furnace" || java_leaf == "blast_furnace" || java_leaf == "smoker" ||
        java_leaf == "lit_furnace" || java_leaf == "lit_blast_furnace" ||
        java_leaf == "lit_smoker") {
        bool lit = false;
        if (startsWith(java_leaf, "lit_")) {
            java_leaf.erase(0, std::string_view("lit_").size());
            lit = true;
        }
        const char* const facing = data >= 2U && data <= 5U ? sixWayFacing(data) : "north";
        return javaState(java_leaf, std::string("facing=") + facing +
                                    ",lit=" + (lit ? "true" : "false"));
    }
    if (java_leaf == "trapped_chest" || java_leaf == "chest") {
        const char* const facing = data >= 2U && data <= 5U ? sixWayFacing(data) : "north";
        return javaState(java_leaf, std::string("facing=") + facing +
                                    ",type=single,waterlogged=false");
    }
    if (java_leaf == "quartz_pillar") {
        return javaState(java_leaf, std::string("axis=") + axisForFlattenedPillar(data));
    }
    if (java_leaf == "scaffolding") {
        return javaState(java_leaf, "bottom=false,distance=" + std::to_string(data & 0x07U) +
                                    ",waterlogged=false");
    }
    if (java_leaf == "cave_vines_body_with_berries") {
        return javaState("cave_vines_plant", "berries=true");
    }
    if (java_leaf == "cave_vines_head_with_berries") {
        return javaState("cave_vines", "age=0,berries=true");
    }
    if (java_leaf == "cave_vines") {
        return javaState("cave_vines", "age=0,berries=false");
    }
    if (java_leaf == "sunflower" || java_leaf == "lilac" ||
        java_leaf == "tall_grass" || java_leaf == "large_fern" ||
        java_leaf == "rose_bush" || java_leaf == "peony") {
        return javaState(java_leaf, std::string("half=") +
                                    ((data & 0x08U) != 0 ? "upper" : "lower"));
    }
    if (java_leaf == "skeleton_skull" || java_leaf == "wither_skeleton_skull" ||
        java_leaf == "zombie_head" || java_leaf == "creeper_head" ||
        java_leaf == "dragon_head") {
        return javaState(java_leaf, "rotation=" + std::to_string(data & 0x0fU));
    }

    return javaState(java_leaf);
}

bool SchematicWriter::write(const SchematicWriteRequest& request, std::string* error) {
    const auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (request.output_path.empty() || !hasSchemExtension(request.output_path)) {
        return fail("output path must end with .schem");
    }
    if (request.width <= 0 || request.height <= 0 || request.length <= 0 ||
        request.width > std::numeric_limits<int16_t>::max() ||
        request.height > std::numeric_limits<int16_t>::max() ||
        request.length > std::numeric_limits<int16_t>::max()) {
        return fail("schematic dimensions exceed Sponge v2 limits");
    }
    const uint64_t volume = static_cast<uint64_t>(request.width) *
        static_cast<uint64_t>(request.height) * static_cast<uint64_t>(request.length);
    if (volume > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
        request.block_indices.size() != volume) {
        return fail("schematic block array does not match its dimensions");
    }
    if (request.palette.empty() || request.palette.size() > kMaximumPaletteSize ||
        request.palette.front() != "minecraft:air") {
        return fail("schematic palette is invalid");
    }

    // Validate the optional native-state extension before opening the output
    // file.  Coordinates are local to the Sponge volume and must be unique;
    // duplicate entries would make a later importer choose an arbitrary
    // state.  Keep the limits bounded even when a malformed checkpoint is
    // supplied by an older build.
    if (request.raw_blocks.size() > volume) {
        return fail("schematic raw-state record count exceeds the volume");
    }
    std::unordered_set<uint64_t> raw_coordinates;
    try {
        raw_coordinates.reserve(request.raw_blocks.size());
    } catch (const std::bad_alloc&) {
        return fail("not enough memory to validate schematic raw-state records");
    }
    uint64_t raw_payload_bytes = 0;
    for (const SchematicRawBlock& raw_block : request.raw_blocks) {
        if (raw_block.x < 0 || raw_block.x >= request.width ||
            raw_block.y < 0 || raw_block.y >= request.height ||
            raw_block.z < 0 || raw_block.z >= request.length ||
            raw_block.identifier.empty() || raw_block.identifier.size() > 1024U ||
            raw_block.identifier.find('\0') != std::string::npos ||
            raw_block.state_json.size() > 4U * 1024U * 1024U ||
            raw_block.entity_json.size() > 64U * 1024U * 1024U) {
            return fail("invalid schematic raw-state record");
        }
        const uint64_t coordinate = static_cast<uint64_t>(raw_block.x) +
            static_cast<uint64_t>(raw_block.z) * static_cast<uint64_t>(request.width) +
            static_cast<uint64_t>(raw_block.y) * static_cast<uint64_t>(request.width) *
                static_cast<uint64_t>(request.length);
        if (!raw_coordinates.emplace(coordinate).second) {
            return fail("schematic raw-state records contain duplicate coordinates");
        }
        const uint64_t record_bytes = static_cast<uint64_t>(raw_block.identifier.size()) +
            raw_block.state_json.size() + raw_block.entity_json.size();
        if (record_bytes > 256U * 1024U * 1024U ||
            raw_payload_bytes > 256U * 1024U * 1024U - record_bytes) {
            return fail("schematic raw-state payload is too large");
        }
        raw_payload_bytes += record_bytes;
    }

    uint64_t block_data_size = 0;
    for (size_t index = 0; index < request.block_indices.size(); ++index) {
        if ((index & 0xfffU) == 0 && cancelled(request)) return fail("schematic write cancelled");
        const uint32_t palette_id = request.block_indices[index];
        if (palette_id >= request.palette.size()) return fail("block references an invalid palette id");
        block_data_size += varIntSize(palette_id);
        if (block_data_size > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
            return fail("Sponge BlockData exceeds the NBT byte-array limit");
        }
    }

    const std::string part_path = request.output_path + ".part";
    std::remove(part_path.c_str());
    GzipNbtWriter writer(part_path);
    if (!writer.valid()) return fail("cannot create schematic temporary file");

    bool ok = writer.tag(Compound, "Schematic") &&
              writer.taggedInt("Version", 2) &&
              writer.taggedInt("DataVersion", kDataVersion) &&
              writer.taggedShort("Width", static_cast<int16_t>(request.width)) &&
              writer.taggedShort("Height", static_cast<int16_t>(request.height)) &&
              writer.taggedShort("Length", static_cast<int16_t>(request.length)) &&
              writer.tag(Compound, "Palette");
    for (size_t index = 0; ok && index < request.palette.size(); ++index) {
        if ((index & 0xffU) == 0 && cancelled(request)) ok = false;
        if (request.palette[index].empty()) ok = false;
        if (ok) ok = writer.taggedInt(request.palette[index], static_cast<int32_t>(index));
    }
    ok = ok && writer.u8(End) &&
         writer.taggedInt("PaletteMax", static_cast<int32_t>(request.palette.size())) &&
         writer.tag(ByteArray, "BlockData") && writer.i32(static_cast<int32_t>(block_data_size));

    std::vector<uint8_t> encoded;
    encoded.reserve(64 * 1024);
    for (size_t index = 0; ok && index < request.block_indices.size(); ++index) {
        if ((index & 0xfffU) == 0 && cancelled(request)) {
            ok = false;
            break;
        }
        appendVarInt(&encoded, request.block_indices[index]);
        if (encoded.size() >= 60 * 1024) {
            ok = writer.raw(encoded.data(), encoded.size());
            encoded.clear();
        }
    }
    if (ok && !encoded.empty()) ok = writer.raw(encoded.data(), encoded.size());

    const int32_t offset[] = {0, 0, 0};
    ok = ok && writer.tag(IntArray, "Offset") && writer.i32(3) &&
         writer.i32(offset[0]) && writer.i32(offset[1]) && writer.i32(offset[2]) &&
         writer.tag(List, "BlockEntities") && writer.u8(Compound) && writer.i32(0) &&
         writer.tag(List, "Entities") && writer.u8(Compound) && writer.i32(0) &&
         writer.tag(Compound, "Metadata") &&
         writer.taggedString("Name", request.display_name.empty() ? "export" : request.display_name) &&
         writer.taggedString("Generator", "Infinitecz BuildExport") &&
         writer.taggedInt("WEOffsetX", 0) && writer.taggedInt("WEOffsetY", 0) &&
         writer.taggedInt("WEOffsetZ", 0) && writer.u8(End);
    if (ok && !request.raw_blocks.empty()) {
        ok = writer.tag(Compound, "Infinitecz") &&
             writer.taggedInt("FormatVersion", 1) &&
             writer.tag(List, "RawBlocks") && writer.u8(Compound) &&
             writer.i32(static_cast<int32_t>(request.raw_blocks.size()));
        for (const SchematicRawBlock& raw_block : request.raw_blocks) {
            if (!ok) break;
            ok = writer.taggedInt("x", raw_block.x) &&
                 writer.taggedInt("y", raw_block.y) &&
                 writer.taggedInt("z", raw_block.z) &&
                 writer.taggedString("Identifier", raw_block.identifier) &&
                 writer.taggedShort("Aux", static_cast<int16_t>(raw_block.aux)) &&
                 writer.taggedInt("LegacyId", raw_block.legacy_id);
            if (ok && !raw_block.state_json.empty()) {
                ok = writer.taggedString("StateJson", raw_block.state_json);
            }
            if (ok && !raw_block.entity_json.empty()) {
                ok = writer.taggedByteArray("EntityJson", raw_block.entity_json);
            }
            if (ok) ok = writer.u8(End);
        }
        if (ok) ok = writer.u8(End);  // Infinitecz compound
    }
    ok = ok && writer.u8(End);  // Schematic root

    if (cancelled(request)) ok = false;
    const bool closed = writer.close();
    if (!ok || writer.failed() || !closed) {
        std::remove(part_path.c_str());
        return fail(cancelled(request) ? "schematic write cancelled" : "cannot finish gzip schematic");
    }
    if (!syncFile(part_path)) {
        std::remove(part_path.c_str());
        return fail("cannot sync schematic temporary file");
    }
    std::unique_lock<std::mutex> publish_lock;
    if (request.publish_mutex) publish_lock = std::unique_lock<std::mutex>(*request.publish_mutex);
    if (cancelled(request)) {
        std::remove(part_path.c_str());
        return fail("schematic write cancelled");
    }
    if (!replaceFileAtomically(part_path, request.output_path)) {
#if defined(_WIN32)
        const unsigned long publish_error = GetLastError();
#else
        const int rename_error = errno;
#endif
        std::remove(part_path.c_str());
#if defined(_WIN32)
        return fail("cannot publish schematic: Windows error " +
                    std::to_string(publish_error));
#else
        return fail("cannot publish schematic: " + std::string(std::strerror(rename_error)));
#endif
    }
    syncParentDirectory(request.output_path);
    if (request.publication_committed) {
        request.publication_committed->store(true, std::memory_order_release);
    }
    if (error) error->clear();
    return true;
}

}  // namespace build_import
