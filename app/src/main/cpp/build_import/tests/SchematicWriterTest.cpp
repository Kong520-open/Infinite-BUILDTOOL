#include "SchematicWriter.h"
#include "BlockMapper.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

#include <zlib.h>

using namespace build_import;

int main() {
    static_assert(std::is_same_v<
        decltype(SchematicWriteRequest{}.block_indices), std::vector<uint16_t>>);
    assert(SchematicWriter::blockStateForExport("minecraft:oak_stairs", 3) ==
           "minecraft:oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:normal_stone_slab", 0) ==
           "minecraft:stone_slab[type=bottom,waterlogged=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:normal_stone_slab", 1) ==
           "minecraft:stone_slab[type=top,waterlogged=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:normal_stone_double_slab", 0) ==
           "minecraft:stone_slab[type=double,waterlogged=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:wool", 14) ==
           "minecraft:red_wool");
    assert(SchematicWriter::blockStateForExport("tile.stone.name", 0) ==
           "minecraft:stone");
    assert(SchematicWriter::blockStateForExport("minecraft:redstone_wire", 12) ==
           "minecraft:redstone_wire[east=none,north=none,power=12,south=none,west=none]");
    // A center ceiling sign stores its four-bit rotation in bits 3..6;
    // an edge ceiling sign stores only a cardinal facing in bits 0..2.
    for (uint16_t rotation = 0; rotation < 16; ++rotation) {
        const uint16_t aux = static_cast<uint16_t>(0x0182U | (rotation << 3U));
        const std::string expected =
            "minecraft:oak_hanging_sign[attached=true,rotation=" +
            std::to_string(rotation) + ",waterlogged=false]";
        assert(SchematicWriter::blockStateForExport("minecraft:oak_hanging_sign", aux) ==
               expected);
    }
    assert(SchematicWriter::blockStateForExport("minecraft:oak_hanging_sign", 0x0083) ==
           "minecraft:oak_hanging_sign[attached=false,rotation=0,waterlogged=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:oak_hanging_sign", 0x0084) ==
           "minecraft:oak_hanging_sign[attached=false,rotation=4,waterlogged=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:oak_hanging_sign", 0x0082) ==
           "minecraft:oak_hanging_sign[attached=false,rotation=8,waterlogged=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:oak_hanging_sign", 0x0085) ==
           "minecraft:oak_hanging_sign[attached=false,rotation=12,waterlogged=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:oak_hanging_sign", 2) ==
           "minecraft:oak_wall_hanging_sign[facing=north,waterlogged=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:spruce_hanging_sign", 3) ==
           "minecraft:spruce_wall_hanging_sign[facing=south,waterlogged=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:dark_oak_hanging_sign", 4) ==
           "minecraft:dark_oak_wall_hanging_sign[facing=west,waterlogged=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:pale_oak_hanging_sign", 5) ==
           "minecraft:pale_oak_wall_hanging_sign[facing=east,waterlogged=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:standing_sign", 6) ==
           "minecraft:oak_sign[rotation=6,waterlogged=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:darkoak_standing_sign", 7) ==
           "minecraft:dark_oak_sign[rotation=7,waterlogged=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:pale_oak_standing_sign", 8) ==
           "minecraft:pale_oak_sign[rotation=8,waterlogged=false]");

    // Fence-gate aux uses the same south/west/north/east order as the other
    // horizontal Bedrock blocks. Bits 2 and 3 must remain open/in_wall.
    assert(SchematicWriter::blockStateForExport("minecraft:fence_gate", 0) ==
           "minecraft:oak_fence_gate[facing=south,in_wall=false,open=false,powered=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:fence_gate", 1) ==
           "minecraft:oak_fence_gate[facing=west,in_wall=false,open=false,powered=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:fence_gate", 2) ==
           "minecraft:oak_fence_gate[facing=north,in_wall=false,open=false,powered=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:fence_gate", 3) ==
           "minecraft:oak_fence_gate[facing=east,in_wall=false,open=false,powered=false]");
    assert(SchematicWriter::blockStateForExport("minecraft:dark_oak_fence_gate", 12) ==
           "minecraft:dark_oak_fence_gate[facing=south,in_wall=true,open=true,powered=false]");

    // Every supported wood family must survive the Java palette round-trip;
    // only the target's historical oak/dark-oak aliases are normalized.
    struct SignMaterial {
        const char* source;
        const char* java_leaf;
        const char* target_leaf;
    };
    const SignMaterial sign_materials[] = {
        {"oak_standing_sign", "oak_sign", "standing_sign"},
        {"spruce_standing_sign", "spruce_sign", "spruce_standing_sign"},
        {"birch_standing_sign", "birch_sign", "birch_standing_sign"},
        {"jungle_standing_sign", "jungle_sign", "jungle_standing_sign"},
        {"acacia_standing_sign", "acacia_sign", "acacia_standing_sign"},
        {"darkoak_standing_sign", "dark_oak_sign", "darkoak_standing_sign"},
        {"mangrove_standing_sign", "mangrove_sign", "mangrove_standing_sign"},
        {"bamboo_standing_sign", "bamboo_sign", "bamboo_standing_sign"},
        {"crimson_standing_sign", "crimson_sign", "crimson_standing_sign"},
        {"warped_standing_sign", "warped_sign", "warped_standing_sign"},
        {"cherry_standing_sign", "cherry_sign", "cherry_standing_sign"},
        {"pale_oak_standing_sign", "pale_oak_sign", "pale_oak_standing_sign"},
    };
    for (const SignMaterial& material : sign_materials) {
        const std::string state = SchematicWriter::blockStateForExport(
            std::string("minecraft:") + material.source, 7);
        assert(state == std::string("minecraft:") + material.java_leaf +
               "[rotation=7,waterlogged=false]");
        const BlockMappingResult mapped = BlockMapper{}.mapSpongeState(state);
        assert(mapped.status == BlockMappingStatus::Mapped);
        assert(mapped.spec.command_name == std::string("minecraft:") + material.target_leaf);
        assert(mapped.spec.aux == 7 && mapped.spec.phase == ImportPhase::Attachment &&
               mapped.spec.stateful && !mapped.spec.can_fill);
    }
    const SignMaterial wall_materials[] = {
        {"oak_wall_sign", "oak_wall_sign", "wall_sign"},
        {"spruce_wall_sign", "spruce_wall_sign", "spruce_wall_sign"},
        {"birch_wall_sign", "birch_wall_sign", "birch_wall_sign"},
        {"jungle_wall_sign", "jungle_wall_sign", "jungle_wall_sign"},
        {"acacia_wall_sign", "acacia_wall_sign", "acacia_wall_sign"},
        {"dark_oak_wall_sign", "dark_oak_wall_sign", "darkoak_wall_sign"},
        {"mangrove_wall_sign", "mangrove_wall_sign", "mangrove_wall_sign"},
        {"bamboo_wall_sign", "bamboo_wall_sign", "bamboo_wall_sign"},
        {"crimson_wall_sign", "crimson_wall_sign", "crimson_wall_sign"},
        {"warped_wall_sign", "warped_wall_sign", "warped_wall_sign"},
        {"cherry_wall_sign", "cherry_wall_sign", "cherry_wall_sign"},
        {"pale_oak_wall_sign", "pale_oak_wall_sign", "pale_oak_wall_sign"},
    };
    for (const SignMaterial& material : wall_materials) {
        const std::string state = SchematicWriter::blockStateForExport(
            std::string("minecraft:") + material.source, 5);
        assert(state == std::string("minecraft:") + material.java_leaf +
               "[facing=east,waterlogged=false]");
        const BlockMappingResult mapped = BlockMapper{}.mapSpongeState(state);
        assert(mapped.status == BlockMappingStatus::Mapped);
        assert(mapped.spec.command_name == std::string("minecraft:") + material.target_leaf);
        assert(mapped.spec.aux == 5 && mapped.spec.phase == ImportPhase::Attachment &&
               mapped.spec.stateful && !mapped.spec.can_fill);
    }

    // A newly exported palette must be accepted by the strict import mapper.
    // These cases include every stateful family from the reported failing
    // schematic, plus its target-only normal_stone aliases.
    struct ExportCase {
        const char* name;
        uint16_t aux;
    };
    const ExportCase export_cases[] = {
        {"minecraft:acacia_door", 0},
        {"minecraft:acacia_leaves", 0},
        {"minecraft:acacia_log", 0},
        {"minecraft:acacia_planks", 0},
        {"minecraft:acacia_stairs", 0},
        {"minecraft:acacia_stairs", 1},
        {"minecraft:acacia_trapdoor", 0},
        {"minecraft:andesite_wall", 0},
        {"minecraft:anvil", 0},
        {"minecraft:azalea", 0},
        {"minecraft:azalea_leaves_flowered", 0},
        {"minecraft:barrel", 0},
        {"minecraft:beehive", 0},
        {"minecraft:birch_door", 0},
        {"minecraft:birch_trapdoor", 0},
        {"minecraft:black_concrete", 0},
        {"minecraft:blue_shulker_box", 0},
        {"minecraft:bookshelf", 0},
        {"minecraft:brown_carpet", 0},
        {"minecraft:cartography_table", 0},
        {"minecraft:cave_vines", 0},
        {"minecraft:cave_vines_body_with_berries", 0},
        {"minecraft:cave_vines_head_with_berries", 0},
        {"minecraft:coal_block", 0},
        {"minecraft:crafting_table", 0},
        {"minecraft:cut_red_sandstone_slab", 0},
        {"minecraft:deepslate_tile_wall", 0},
        {"minecraft:dirt", 0},
        {"minecraft:end_rod", 0},
        {"minecraft:fletching_table", 0},
        {"minecraft:flower_pot", 0},
        {"minecraft:furnace", 0},
        {"minecraft:glass", 0},
        {"minecraft:glass_pane", 0},
        {"minecraft:green_shulker_box", 0},
        {"minecraft:iron_bars", 0},
        {"minecraft:iron_trapdoor", 0},
        {"minecraft:ladder", 0},
        {"minecraft:light_blue_concrete", 0},
        {"minecraft:light_blue_wool", 0},
        {"minecraft:light_gray_concrete", 0},
        {"minecraft:lime_carpet", 0},
        {"minecraft:lime_wool", 0},
        {"minecraft:lodestone", 0},
        {"minecraft:moss_block", 0},
        {"minecraft:mossy_stone_brick_wall", 0},
        {"minecraft:normal_stone_double_slab", 0},
        {"minecraft:normal_stone_slab", 0},
        {"minecraft:normal_stone_stairs", 0},
        {"minecraft:normal_stone_stairs", 1},
        {"minecraft:oak_double_slab", 0},
        {"minecraft:oak_fence", 0},
        {"minecraft:oak_leaves", 0},
        {"minecraft:oak_planks", 0},
        {"minecraft:oak_stairs", 0},
        {"minecraft:orange_wool", 0},
        {"minecraft:peony", 0},
        {"minecraft:polished_deepslate_double_slab", 0},
        {"minecraft:polished_deepslate_slab", 0},
        {"minecraft:polished_diorite", 0},
        {"minecraft:purple_wool", 0},
        {"minecraft:quartz_double_slab", 0},
        {"minecraft:quartz_pillar", 0},
        {"minecraft:quartz_slab", 0},
        {"minecraft:quartz_stairs", 0},
        {"minecraft:rail", 0},
        {"minecraft:red_wool", 0},
        {"minecraft:sandstone_slab", 0},
        {"minecraft:scaffolding", 0},
        {"minecraft:sea_lantern", 0},
        {"minecraft:short_grass", 0},
        {"minecraft:skeleton_skull", 0},
        {"minecraft:smooth_red_sandstone", 0},
        {"minecraft:smooth_red_sandstone_slab", 0},
        {"minecraft:smooth_stone", 0},
        {"minecraft:smooth_stone_double_slab", 0},
        {"minecraft:smooth_stone_slab", 0},
        {"minecraft:spruce_door", 0},
        {"minecraft:spruce_leaves", 0},
        {"minecraft:spruce_log", 0},
        {"minecraft:spruce_slab", 0},
        {"minecraft:spruce_trapdoor", 0},
        {"minecraft:stone", 0},
        {"minecraft:stone_brick_wall", 0},
        {"minecraft:trapped_chest", 0},
        {"minecraft:wall_sign", 0},
        {"minecraft:water", 0},
        {"minecraft:waxed_oxidized_cut_copper_slab", 1},
        {"minecraft:waxed_oxidized_cut_copper_stairs", 0},
        {"minecraft:white_concrete", 0},
        {"minecraft:yellow_concrete", 0},
        {"minecraft:yellow_terracotta", 0},
        {"minecraft:yellow_wool", 0},
    };
    const BlockMapper mapper;

    // Exported state must survive the strict Sponge mapper with the same
    // target command and auxiliary value.  Checking only Mapped here would
    // let a default-facing repeater or standing torch pass unnoticed.
    struct StatefulRoundTripCase {
        const char* source_name;
        uint16_t source_aux;
        const char* expected_state;
        const char* expected_command;
        uint16_t expected_aux;
    };
    const StatefulRoundTripCase stateful_cases[] = {
        {"minecraft:torch", 1,
         "minecraft:wall_torch[facing=east]", "minecraft:torch", 1},
        {"minecraft:torch", 5,
         "minecraft:torch", "minecraft:torch", 5},
        {"minecraft:soul_torch", 2,
         "minecraft:soul_wall_torch[facing=west]", "minecraft:soul_torch", 2},
        {"minecraft:redstone_torch", 4,
         "minecraft:redstone_wall_torch[facing=north,lit=true]",
         "minecraft:redstone_torch", 4},
        {"minecraft:unlit_redstone_torch", 2,
         "minecraft:redstone_wall_torch[facing=west,lit=false]",
         "minecraft:unlit_redstone_torch", 2},
        {"minecraft:unlit_redstone_torch", 5,
         "minecraft:redstone_torch[lit=false]", "minecraft:unlit_redstone_torch", 5},
        {"minecraft:lever", 12,
         "minecraft:lever[face=wall,facing=north,powered=true]",
         "minecraft:lever", 12},
        {"minecraft:lever", 0,
         "minecraft:lever[face=ceiling,facing=east,powered=false]",
         "minecraft:lever", 0},
        {"minecraft:wooden_button", 10,
         "minecraft:oak_button[face=wall,facing=west,powered=true]",
         "minecraft:wooden_button", 10},
        {"minecraft:stone_button", 5,
         "minecraft:stone_button[face=floor,facing=north,powered=false]",
         "minecraft:stone_button", 5},
        {"minecraft:unpowered_repeater", 10,
         "minecraft:repeater[delay=3,facing=north,locked=false,powered=false]",
         "minecraft:unpowered_repeater", 10},
        {"minecraft:powered_repeater", 13,
         "minecraft:repeater[delay=4,facing=west,locked=false,powered=true]",
         "minecraft:powered_repeater", 13},
        {"minecraft:unpowered_comparator", 6,
         "minecraft:comparator[facing=north,mode=subtract,powered=false]",
         "minecraft:unpowered_comparator", 6},
        {"minecraft:powered_comparator", 15,
         "minecraft:comparator[facing=east,mode=subtract,powered=true]",
         "minecraft:powered_comparator", 15},
        {"minecraft:piston", 5,
         "minecraft:piston[extended=false,facing=east]", "minecraft:piston", 5},
        {"minecraft:sticky_piston", 1,
         "minecraft:sticky_piston[extended=false,facing=up]", "minecraft:sticky_piston", 1},
        {"minecraft:dropper", 13,
         "minecraft:dropper[facing=east,triggered=true]", "minecraft:dropper", 13},
        {"minecraft:dispenser", 2,
         "minecraft:dispenser[facing=north,triggered=false]", "minecraft:dispenser", 2},
        {"minecraft:hopper", 12,
         "minecraft:hopper[enabled=false,facing=west]", "minecraft:hopper", 12},
        {"minecraft:observer", 11,
         "minecraft:observer[facing=south,powered=true]", "minecraft:observer", 11},
        {"minecraft:redstone_lamp", 0,
         "minecraft:redstone_lamp[lit=false]", "minecraft:redstone_lamp", 0},
        {"minecraft:lit_redstone_lamp", 0,
         "minecraft:redstone_lamp[lit=true]", "minecraft:lit_redstone_lamp", 0},
        {"minecraft:daylight_detector", 7,
         "minecraft:daylight_detector[inverted=false,power=7]",
         "minecraft:daylight_detector", 7},
        {"minecraft:daylight_detector_inverted", 12,
         "minecraft:daylight_detector[inverted=true,power=12]",
         "minecraft:daylight_detector_inverted", 12},
        {"minecraft:oak_hanging_sign", 0x0083,
         "minecraft:oak_hanging_sign[attached=false,rotation=0,waterlogged=false]",
         "minecraft:oak_hanging_sign", 0x0083},
        {"minecraft:oak_hanging_sign", 0x0084,
         "minecraft:oak_hanging_sign[attached=false,rotation=4,waterlogged=false]",
         "minecraft:oak_hanging_sign", 0x0084},
        {"minecraft:oak_hanging_sign", 0x0082,
         "minecraft:oak_hanging_sign[attached=false,rotation=8,waterlogged=false]",
         "minecraft:oak_hanging_sign", 0x0082},
        {"minecraft:oak_hanging_sign", 0x0085,
         "minecraft:oak_hanging_sign[attached=false,rotation=12,waterlogged=false]",
         "minecraft:oak_hanging_sign", 0x0085},
        {"minecraft:dark_oak_hanging_sign", 0x01fa,
         "minecraft:dark_oak_hanging_sign[attached=true,rotation=15,waterlogged=false]",
         "minecraft:dark_oak_hanging_sign", 0x01fa},
        {"minecraft:oak_sign", 6,
         "minecraft:oak_sign[rotation=6,waterlogged=false]",
         "minecraft:standing_sign", 6},
        {"minecraft:fence_gate", 0,
         "minecraft:oak_fence_gate[facing=south,in_wall=false,open=false,powered=false]",
         "minecraft:fence_gate", 0},
        {"minecraft:fence_gate", 15,
         "minecraft:oak_fence_gate[facing=east,in_wall=true,open=true,powered=false]",
         "minecraft:fence_gate", 15},
        {"minecraft:spruce_fence_gate", 6,
         "minecraft:spruce_fence_gate[facing=north,in_wall=false,open=true,powered=false]",
         "minecraft:spruce_fence_gate", 6},
        {"minecraft:mangrove_fence_gate", 9,
         "minecraft:mangrove_fence_gate[facing=west,in_wall=true,open=false,powered=false]",
         "minecraft:mangrove_fence_gate", 9},
    };
    for (const StatefulRoundTripCase& item : stateful_cases) {
        const std::string state = SchematicWriter::blockStateForExport(
            item.source_name, item.source_aux);
        assert(state == item.expected_state);
        const BlockMappingResult mapped_state = mapper.mapSpongeState(state);
        if (mapped_state.status != BlockMappingStatus::Mapped ||
            mapped_state.spec.command_name != item.expected_command ||
            mapped_state.spec.aux != item.expected_aux) {
            std::fprintf(stderr, "stateful export did not round-trip: %s/%u -> %s\n",
                         item.source_name, static_cast<unsigned>(item.source_aux), state.c_str());
            return 1;
        }
    }
    assert(SchematicWriter::blockStateForExport("minecraft:piston_arm_collision", 3) ==
           "minecraft:piston_head[facing=south,short=false,type=normal]");
    assert(SchematicWriter::blockStateForExport("minecraft:sticky_piston_arm_collision", 4) ==
           "minecraft:piston_head[facing=west,short=false,type=sticky]");
    assert(SchematicWriter::blockStateForExport("minecraft:piston", 9) ==
           "minecraft:piston[extended=true,facing=up]");

    const BlockMappingResult normal_slab = mapper.mapSpongeState(
        SchematicWriter::blockStateForExport("minecraft:normal_stone_slab", 0));
    assert(normal_slab.status == BlockMappingStatus::Mapped);
    assert(normal_slab.spec.command_name == "minecraft:stone_block_slab4");
    assert(normal_slab.spec.aux == 2);
    const BlockMappingResult normal_double_slab = mapper.mapSpongeState(
        SchematicWriter::blockStateForExport("minecraft:normal_stone_double_slab", 0));
    assert(normal_double_slab.status == BlockMappingStatus::Mapped);
    assert(normal_double_slab.spec.command_name == "minecraft:double_stone_block_slab4");
    assert(normal_double_slab.spec.aux == 2);
    for (const ExportCase& item : export_cases) {
        const std::string state = SchematicWriter::blockStateForExport(item.name, item.aux);
        assert(state.find("infinitecz_data") == std::string::npos);
        const BlockMappingResult mapped = mapper.mapSpongeState(state);
        if (mapped.status != BlockMappingStatus::Mapped) {
            std::fprintf(stderr, "exported state did not re-import: %s -> %s (%s)\n",
                         item.name, state.c_str(), mapped.reason.c_str());
            return 1;
        }
    }

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "infinitecz_schematic_writer_test.schem";
    std::remove(path.string().c_str());
    SchematicWriteRequest request;
    request.output_path = path.string();
    request.display_name = "writer-test";
    request.width = 2;
    request.height = 1;
    request.length = 2;
    request.palette = {"minecraft:air", "minecraft:stone"};
    request.block_indices = {0, 1, 1, 0};
    assert(SchematicWriter::write(request));

    gzFile input = gzopen(path.string().c_str(), "rb");
    assert(input != nullptr);
    std::vector<unsigned char> bytes;
    unsigned char buffer[256];
    int read = 0;
    while ((read = gzread(input, buffer, sizeof(buffer))) > 0) {
        bytes.insert(bytes.end(), buffer, buffer + read);
    }
    assert(read == 0);
    assert(gzclose(input) == Z_OK);
    const std::string nbt(bytes.begin(), bytes.end());
    assert(nbt.find("Schematic") != std::string::npos);
    const size_t small_block_data_name = nbt.find("BlockData");
    assert(small_block_data_name != std::string::npos);
    const size_t small_length_offset =
        small_block_data_name + std::string("BlockData").size();
    assert(small_length_offset + 8 <= bytes.size());
    assert(bytes[small_length_offset] == 0 && bytes[small_length_offset + 1] == 0 &&
           bytes[small_length_offset + 2] == 0 && bytes[small_length_offset + 3] == 4);
    assert(bytes[small_length_offset + 4] == 0 && bytes[small_length_offset + 5] == 1 &&
           bytes[small_length_offset + 6] == 1 && bytes[small_length_offset + 7] == 0);
    assert(nbt.find("Palette") != std::string::npos);
    std::remove(path.string().c_str());

    const std::filesystem::path invalid_path =
        std::filesystem::temp_directory_path() / "infinitecz_schematic_writer_invalid.schem";
    request.output_path = invalid_path.string();
    request.width = 1;
    request.length = 1;
    request.palette = {"minecraft:air", "minecraft:stone"};
    request.block_indices = {2};
    assert(!SchematicWriter::write(request));

    const std::filesystem::path maximum_path =
        std::filesystem::temp_directory_path() / "infinitecz_schematic_writer_max_id.schem";
    request.output_path = maximum_path.string();
    request.palette.resize(65'536);
    request.palette[0] = "minecraft:air";
    for (size_t index = 1; index < request.palette.size(); ++index) {
        request.palette[index] = "infinitecz:test_" + std::to_string(index);
    }
    request.block_indices = {65'535};
    assert(SchematicWriter::write(request));
    input = gzopen(maximum_path.string().c_str(), "rb");
    assert(input != nullptr);
    bytes.clear();
    while ((read = gzread(input, buffer, sizeof(buffer))) > 0) {
        bytes.insert(bytes.end(), buffer, buffer + read);
    }
    assert(read == 0);
    assert(gzclose(input) == Z_OK);
    const std::string maximum_nbt(bytes.begin(), bytes.end());
    const size_t block_data_name = maximum_nbt.find("BlockData");
    assert(block_data_name != std::string::npos);
    const size_t length_offset = block_data_name + std::string("BlockData").size();
    assert(length_offset + 7 <= bytes.size());
    assert(bytes[length_offset] == 0 && bytes[length_offset + 1] == 0 &&
           bytes[length_offset + 2] == 0 && bytes[length_offset + 3] == 3);
    assert(bytes[length_offset + 4] == 0xff && bytes[length_offset + 5] == 0xff &&
           bytes[length_offset + 6] == 0x03);
    std::remove(maximum_path.string().c_str());
    return 0;
}
