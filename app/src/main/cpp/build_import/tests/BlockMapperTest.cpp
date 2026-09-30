#include "../BlockMapper.h"

#include <cassert>
#include <utility>

using namespace build_import;

namespace {

BlockSpec mapped(BlockMappingResult result) {
    assert(result.status == BlockMappingStatus::Mapped);
    assert(result.reason.empty());
    return std::move(result.spec);
}

BlockSpec mappedWithWarning(BlockMappingResult result) {
    assert(result.status == BlockMappingStatus::Mapped);
    assert(!result.reason.empty());
    return std::move(result.spec);
}

}  // namespace

int main() {
    BlockMapper mapper;

    assert(mapper.mapLegacy(0, 0).status == BlockMappingStatus::Air);
    assert(mapper.mapLegacy(4095, 0).status == BlockMappingStatus::Unsupported);
    assert(mapper.mapLegacy(36, 0).status == BlockMappingStatus::Air);
    assert(mapped(mapper.mapLegacy(34, 0)).command_name == "minecraft:piston_arm_collision");
    assert(mapped(mapper.mapLegacy(37, 0)).command_name == "minecraft:yellow_flower");
    assert(mapped(mapper.mapLegacy(38, 2)).command_name == "minecraft:red_flower");
    assert(mapped(mapper.mapLegacy(90, 0)).command_name == "minecraft:portal");
    assert(mapped(mapper.mapLegacy(119, 0)).command_name == "minecraft:end_portal");
    assert(mapped(mapper.mapLegacy(208, 0)).command_name == "minecraft:grass_path");
    assert(mapped(mapper.mapLegacy(209, 0)).command_name == "minecraft:end_gateway");
    assert(mapped(mapper.mapLegacy(210, 0)).command_name == "minecraft:repeating_command_block");
    assert(mapped(mapper.mapLegacy(211, 0)).command_name == "minecraft:chain_command_block");
    const BlockSpec& legacy_conditional_chain_command = mapped(mapper.mapLegacy(211, 11));
    assert(legacy_conditional_chain_command.command_name == "minecraft:chain_command_block" &&
           legacy_conditional_chain_command.aux == 11 &&
           legacy_conditional_chain_command.stateful &&
           !legacy_conditional_chain_command.can_fill);
    const BlockSpec& legacy_potted_plant = mappedWithWarning(mapper.mapLegacy(140, 6));
    assert(legacy_potted_plant.command_name == "minecraft:flower_pot" &&
           legacy_potted_plant.aux == 0);
    const BlockSpec& legacy_beetroot = mapped(mapper.mapLegacy(207, 3));
    assert(legacy_beetroot.command_name == "minecraft:beetroot" &&
           legacy_beetroot.aux == 7);
    const BlockSpec& legacy_stone_slab_bottom = mapped(mapper.mapLegacy(44, 0));
    const BlockSpec& legacy_stone_slab_top = mapped(mapper.mapLegacy(44, 8));
    const BlockSpec& legacy_stone_brick_double_slab = mapped(mapper.mapLegacy(43, 5));
    const BlockSpec& legacy_wood_slab_top = mapped(mapper.mapLegacy(126, 8));
    const BlockSpec& legacy_red_sandstone_slab_top = mapped(mapper.mapLegacy(182, 8));
    const BlockSpec& legacy_purpur_slab_top = mapped(mapper.mapLegacy(205, 8));
    assert(legacy_stone_slab_bottom.command_name == "minecraft:stone_slab" &&
           legacy_stone_slab_bottom.aux == 0 && legacy_stone_slab_bottom.can_fill);
    assert(legacy_stone_slab_top.command_name == "minecraft:stone_slab" &&
           legacy_stone_slab_top.aux == 8 && legacy_stone_slab_top.can_fill);
    assert(legacy_stone_brick_double_slab.command_name == "minecraft:double_stone_slab" &&
           legacy_stone_brick_double_slab.aux == 5);
    assert(legacy_wood_slab_top.command_name == "minecraft:wooden_slab" &&
           legacy_wood_slab_top.aux == 8);
    assert(legacy_red_sandstone_slab_top.command_name == "minecraft:stone_slab2" &&
           legacy_red_sandstone_slab_top.aux == 8);
    assert(legacy_purpur_slab_top.command_name == "minecraft:purpur_slab" &&
           legacy_purpur_slab_top.aux == 8);

    const BlockSpec& legacy_bed_head = mapped(mapper.mapLegacy(26, 8));
    assert(legacy_bed_head.phase == ImportPhase::DependentAttachment);
    assert(!legacy_bed_head.can_fill && legacy_bed_head.single_layer_only &&
           legacy_bed_head.stateful);
    assert(mapped(mapper.mapLegacy(175, 0)).phase == ImportPhase::Attachment);
    assert(mapped(mapper.mapLegacy(175, 8)).phase == ImportPhase::DependentAttachment);

    assert(BlockMapper::isStableVerificationBlockName("minecraft:stone"));
    assert(BlockMapper::isStableVerificationBlockName("minecraft:fence_gate"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:wooden_button"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:wall_sign"));
    const auto assert_flat_identity = [](std::string_view source,
                                         std::string_view target,
                                         uint8_t aux) {
        const auto identity =
            BlockMapper::flatBlockIdentityForVerification(source);
        assert(identity);
        assert(identity->command_name == target);
        assert(identity->aux == aux);
    };
    assert_flat_identity("minecraft:cobweb", "minecraft:web", 0);
    assert_flat_identity("minecraft:snow_block", "minecraft:snow", 0);
    assert_flat_identity(
        "minecraft:infested_chiseled_stone_bricks", "minecraft:monster_egg", 5);
    assert_flat_identity("minecraft:prismarine_bricks", "minecraft:prismarine", 1);
    assert_flat_identity("minecraft:dark_prismarine", "minecraft:prismarine", 2);
    assert_flat_identity(
        "minecraft:nether_quartz_ore", "minecraft:quartz_ore", 0);
    assert_flat_identity(
        "minecraft:red_nether_bricks", "minecraft:red_nether_brick", 0);
    assert_flat_identity(
        "minecraft:end_stone_bricks", "minecraft:end_bricks", 0);
    assert_flat_identity("minecraft:magma_block", "minecraft:magma", 0);
    assert(!BlockMapper::flatBlockIdentityForVerification(
        "example:infested_chiseled_stone_bricks"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:wooden_door"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:torch"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:ladder"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:rail"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:carpet"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:oak_sapling"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:water"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:unpowered_repeater"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:powered_comparator"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:farmland"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:piston_arm_collision"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:sponge"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:grass_block"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:mycelium"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:portal"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:hopper"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:crafter"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:chest"));
    assert(!BlockMapper::isStableVerificationBlockName("minecraft:red_shulker_box"));

    const BlockSpec& dragon_egg = mapped(mapper.mapLegacy(122, 0));
    assert(dragon_egg.phase == ImportPhase::Gravity && dragon_egg.single_layer_only);

    const BlockSpec& wool = mapped(mapper.mapSpongeState("minecraft:light_blue_wool"));
    assert(wool.command_name == "minecraft:wool" && wool.aux == 3);
    assert(wool.phase == ImportPhase::Structure && wool.can_fill && !wool.stateful);

    const BlockSpec& stone_bricks = mapped(mapper.mapSpongeState("minecraft:stone_bricks"));
    assert(stone_bricks.command_name == "minecraft:stonebrick" && stone_bricks.aux == 0);
    assert(stone_bricks.phase == ImportPhase::Structure && stone_bricks.can_fill &&
           !stone_bricks.stateful);

    const BlockSpec& powder = mapped(mapper.mapSpongeState("minecraft:red_concrete_powder"));
    assert(powder.command_name == "minecraft:concrete_powder" && powder.aux == 14);
    assert(powder.phase == ImportPhase::Gravity);
    const BlockSpec& snowy_grass = mapped(
        mapper.mapSpongeState("minecraft:grass_block[snowy=true]"));
    assert(snowy_grass.command_name == "minecraft:grass_block" && snowy_grass.aux == 0);
    const BlockSpec& snowy_mycelium = mapped(
        mapper.mapSpongeState("minecraft:mycelium[snowy=false]"));
    assert(snowy_mycelium.command_name == "minecraft:mycelium" &&
           snowy_mycelium.aux == 0 && snowy_mycelium.can_fill);
    const BlockSpec& nether_portal = mappedWithWarning(
        mapper.mapSpongeState("minecraft:nether_portal[axis=z]"));
    assert(nether_portal.command_name == "minecraft:portal" &&
           nether_portal.aux == 0 && nether_portal.can_fill);
    assert(!mapper.mapSpongeState("minecraft:nether_portal[axis=y]").isMapped());
    const BlockSpec& jigsaw = mappedWithWarning(
        mapper.mapSpongeState("minecraft:jigsaw[orientation=up_west]"));
    assert(jigsaw.command_name == "minecraft:structure_block" &&
           jigsaw.aux == 0 && !jigsaw.can_fill && jigsaw.stateful);
    assert(!mapper.mapSpongeState("minecraft:jigsaw[orientation=sideways]").isMapped());

    const BlockSpec& log = mapped(mapper.mapSpongeState("minecraft:spruce_log[axis=x]"));
    assert(log.command_name == "minecraft:log" && log.aux == 5 && log.can_fill);
    const BlockSpec& bark = mapped(mapper.mapSpongeState("minecraft:spruce_wood[axis=z]"));
    assert(bark.command_name == "minecraft:log" && bark.aux == 13 && bark.can_fill);
    const BlockSpec& dandelion = mapped(mapper.mapSpongeState("minecraft:dandelion"));
    assert(dandelion.command_name == "minecraft:yellow_flower" && dandelion.aux == 0);
    const BlockSpec& pale_oak_wood = mappedWithWarning(
        mapper.mapSpongeState("minecraft:pale_oak_wood[axis=y]"));
    assert(pale_oak_wood.command_name == "minecraft:log" && pale_oak_wood.aux == 14);
    const BlockSpec& pale_oak_stairs = mappedWithWarning(mapper.mapSpongeState(
        "minecraft:pale_oak_stairs[facing=north,half=bottom,shape=straight,waterlogged=false]"));
    assert(pale_oak_stairs.command_name == "minecraft:birch_stairs");
    const BlockSpec& stripped_pale_oak = mappedWithWarning(
        mapper.mapSpongeState("minecraft:stripped_pale_oak_log[axis=x]"));
    assert(stripped_pale_oak.command_name == "minecraft:stripped_birch_log" &&
           stripped_pale_oak.aux == 1);
    const BlockSpec& wall = mapped(mapper.mapSpongeState(
        "minecraft:cobblestone_wall[east=low,north=none,south=tall,up=true,"
        "waterlogged=false,west=none]"));
    assert(wall.command_name == "minecraft:cobblestone_wall" && wall.aux == 0 &&
           wall.can_fill && !wall.stateful);
    const BlockSpec& bars = mapped(mapper.mapSpongeState(
        "minecraft:iron_bars[east=true,north=false,south=true,waterlogged=false,west=false]"));
    assert(bars.command_name == "minecraft:iron_bars" && bars.aux == 0 && bars.can_fill);
    const BlockSpec& nether_fence = mapped(mapper.mapSpongeState(
        "minecraft:nether_brick_fence[east=false,north=true,south=false,"
        "waterlogged=false,west=true]"));
    assert(nether_fence.command_name == "minecraft:nether_brick_fence" &&
           nether_fence.aux == 0 && nether_fence.can_fill);
    const BlockSpec& pumpkin = mapped(
        mapper.mapSpongeState("minecraft:carved_pumpkin[facing=east]"));
    assert(pumpkin.command_name == "minecraft:pumpkin" && pumpkin.aux == 3);
    const BlockSpec& hay_x = mapped(
        mapper.mapSpongeState("minecraft:hay_block[axis=x]"));
    const BlockSpec& hay_z = mapped(
        mapper.mapSpongeState("minecraft:hay_block[axis=z]"));
    assert(hay_x.command_name == "minecraft:hay_block" && hay_x.aux == 4);
    assert(hay_z.command_name == "minecraft:hay_block" && hay_z.aux == 8);
    const BlockSpec& quartz_ore = mapped(
        mapper.mapSpongeState("minecraft:nether_quartz_ore"));
    assert(quartz_ore.command_name == "minecraft:quartz_ore" && quartz_ore.aux == 0);

    const BlockSpec& stairs = mapped(mapper.mapSpongeState(
        "minecraft:oak_stairs[facing=north,half=top,shape=inner_left,waterlogged=false]"));
    assert(stairs.command_name == "minecraft:oak_stairs" && stairs.aux == 7);
    const BlockSpec& cobblestone_stairs = mapped(mapper.mapSpongeState(
        "minecraft:cobblestone_stairs[facing=east,half=bottom,shape=straight,waterlogged=false]"));
    assert(cobblestone_stairs.command_name == "minecraft:stone_stairs" &&
           cobblestone_stairs.aux == 0);
    const BlockSpec& normal_stone_stairs = mapped(mapper.mapSpongeState(
        "minecraft:stone_stairs[facing=west,half=top,shape=straight,waterlogged=false]"));
    assert(normal_stone_stairs.command_name == "minecraft:normal_stone_stairs" &&
           normal_stone_stairs.aux == 5);

    const BlockSpec& top_slab = mapped(
        mapper.mapSpongeState("minecraft:oak_slab[type=top,waterlogged=false]"));
    assert(top_slab.command_name == "minecraft:wooden_slab" && top_slab.aux == 8);
    const BlockSpec& double_slab = mapped(
        mapper.mapSpongeState("minecraft:stone_brick_slab[type=double,waterlogged=false]"));
    assert(double_slab.command_name == "minecraft:double_stone_slab" &&
           double_slab.aux == 5);
    const BlockSpec& normal_stone_slab = mapped(
        mapper.mapSpongeState("minecraft:stone_slab[type=bottom,waterlogged=false]"));
    assert(normal_stone_slab.command_name == "minecraft:stone_block_slab4" &&
           normal_stone_slab.aux == 2);
    const BlockSpec& smooth_stone_slab = mapped(
        mapper.mapSpongeState("minecraft:smooth_stone_slab[type=top,waterlogged=false]"));
    assert(smooth_stone_slab.command_name == "minecraft:stone_slab" &&
           smooth_stone_slab.aux == 8);
    const BlockSpec& old_half_top_slab = mapped(
        mapper.mapSpongeState("minecraft:oak_slab[half=top]"));
    assert(old_half_top_slab.command_name == "minecraft:wooden_slab" &&
           old_half_top_slab.aux == 8);
    const BlockSpec& old_variant_top_slab = mapped(mapper.mapSpongeState(
        "minecraft:stone_slab[variant=stone_brick,half=top]"));
    assert(old_variant_top_slab.command_name == "minecraft:stone_slab" &&
           old_variant_top_slab.aux == 13);
    const BlockSpec& old_prismarine_alias_slab = mapped(mapper.mapSpongeState(
        "minecraft:prismarine_bricks_slab[slab_type=top]"));
    assert(old_prismarine_alias_slab.command_name == "minecraft:stone_slab2" &&
           old_prismarine_alias_slab.aux == 12);
    const BlockSpec& tuff_top_slab = mappedWithWarning(mapper.mapSpongeState(
        "minecraft:tuff_slab[type=top,waterlogged=true]"));
    assert(tuff_top_slab.command_name == "minecraft:tuff_slab" &&
           tuff_top_slab.aux == 1 && tuff_top_slab.can_fill);
    const BlockSpec& tuff_brick_double_slab = mapped(mapper.mapSpongeState(
        "minecraft:tuff_brick_slab[type=double,waterlogged=false]"));
    assert(tuff_brick_double_slab.command_name == "minecraft:tuff_brick_double_slab" &&
           tuff_brick_double_slab.aux == 0 && tuff_brick_double_slab.can_fill);
    const BlockSpec& polished_tuff_top_slab = mapped(mapper.mapSpongeState(
        "minecraft:polished_tuff_slab[vertical_half=top]"));
    assert(polished_tuff_top_slab.command_name == "minecraft:polished_tuff_slab" &&
           polished_tuff_top_slab.aux == 1);
    const BlockSpec& direct_double_copper_slab = mapped(mapper.mapSpongeState(
        "minecraft:waxed_exposed_double_cut_copper_slab[vertical_half=bottom]"));
    assert(direct_double_copper_slab.command_name ==
           "minecraft:waxed_exposed_double_cut_copper_slab" &&
           direct_double_copper_slab.aux == 0);
    assert(mapper.mapSpongeState("minecraft:stone_slab[type=top,half=bottom]").status ==
           BlockMappingStatus::Unsupported);
    assert(mapper.mapSpongeState("minecraft:stone_slab[variant=unknown,half=top]").status ==
           BlockMappingStatus::Unsupported);
    // Verification identity: the target reports modern per-material slab names
    // with a 0/1 vertical-half aux, while the importer command space keeps the
    // legacy family with bit 3 (8) for the upper half. Both sides must land on
    // the same legacy identity for the final check.
    const auto flat = [&](const char* name, uint16_t aux) {
        return BlockMapper::flatBlockIdentityForVerification(name, aux);
    };
    {
        const auto identity = flat("minecraft:smooth_stone_slab", 0);
        assert(identity && identity->command_name == "minecraft:stone_slab" &&
               identity->aux == 0);
    }
    {
        const auto identity = flat("minecraft:smooth_stone_slab", 1);
        assert(identity && identity->command_name == "minecraft:stone_slab" &&
               identity->aux == 8);
    }
    {
        const auto identity = flat("minecraft:stone_brick_slab", 1);
        assert(identity && identity->command_name == "minecraft:stone_slab" &&
               identity->aux == 13);
    }
    {
        const auto identity = flat("minecraft:oak_slab", 0);
        assert(identity && identity->command_name == "minecraft:wooden_slab" &&
               identity->aux == 0);
    }
    {
        const auto identity = flat("minecraft:oak_slab", 1);
        assert(identity && identity->command_name == "minecraft:wooden_slab" &&
               identity->aux == 8);
    }
    {
        const auto identity = flat("minecraft:purpur_slab", 1);
        assert(identity && identity->command_name == "minecraft:stone_slab2" &&
               identity->aux == 9);
    }
    {
        const auto identity = flat("minecraft:purpur_slab", 8);
        assert(identity && identity->command_name == "minecraft:stone_slab2" &&
               identity->aux == 9);
    }
    {
        const auto identity = flat("minecraft:purpur_slab", 0);
        assert(identity && identity->command_name == "minecraft:stone_slab2" &&
               identity->aux == 1);
    }
    // Modern-only slabs and legacy family names have no reverse mapping.
    assert(!flat("minecraft:tuff_slab", 1));
    assert(!flat("minecraft:stone_slab", 13));
    assert(!flat("minecraft:wooden_slab", 8));
    const BlockSpec& smooth_stone = mapped(mapper.mapSpongeState("minecraft:smooth_stone"));
    assert(smooth_stone.command_name == "minecraft:smooth_stone" && smooth_stone.aux == 0);
    assert(mapped(mapper.mapSpongeState("minecraft:cut_sandstone")).command_name ==
           "minecraft:cut_sandstone");
    assert(mapped(mapper.mapSpongeState("minecraft:smooth_red_sandstone")).command_name ==
           "minecraft:smooth_red_sandstone");
    assert(mapped(mapper.mapSpongeState("minecraft:smooth_quartz")).command_name ==
           "minecraft:smooth_quartz");
    const BlockSpec& quartz_pillar_x = mapped(
        mapper.mapSpongeState("minecraft:quartz_pillar[axis=x]"));
    const BlockSpec& quartz_pillar_y = mapped(
        mapper.mapSpongeState("minecraft:quartz_pillar[axis=y]"));
    const BlockSpec& quartz_pillar_z = mapped(
        mapper.mapSpongeState("minecraft:quartz_pillar[axis=z]"));
    assert(quartz_pillar_x.command_name == "minecraft:quartz_block" &&
           quartz_pillar_x.aux == 6);
    assert(quartz_pillar_y.command_name == "minecraft:quartz_block" &&
           quartz_pillar_y.aux == 2);
    assert(quartz_pillar_z.command_name == "minecraft:quartz_block" &&
           quartz_pillar_z.aux == 10);

    const BlockSpec& bed_foot = mappedWithWarning(mapper.mapSpongeState(
        "minecraft:red_bed[facing=north,occupied=false,part=foot]"));
    assert(bed_foot.command_name == "minecraft:bed" && bed_foot.aux == 2 &&
           bed_foot.phase == ImportPhase::Attachment && bed_foot.stateful &&
           !bed_foot.can_fill);
    const BlockSpec& bed_head = mappedWithWarning(mapper.mapSpongeState(
        "minecraft:red_bed[facing=south,occupied=false,part=head]"));
    assert(bed_head.aux == 8 && bed_head.phase == ImportPhase::DependentAttachment);

    const BlockSpec& lower_door = mapped(mapper.mapSpongeState(
        "minecraft:oak_door[facing=north,half=lower,hinge=left,open=true,powered=false]"));
    assert(lower_door.command_name == "minecraft:wooden_door" && lower_door.aux == 7 &&
           lower_door.phase == ImportPhase::Attachment);
    const BlockSpec& upper_door = mapped(mapper.mapSpongeState(
        "minecraft:oak_door[facing=north,half=upper,hinge=right,open=true,powered=true]"));
    assert(upper_door.aux == 11 &&
           upper_door.phase == ImportPhase::DependentAttachment);

    const BlockSpec& trapdoor = mapped(mapper.mapSpongeState(
        "minecraft:oak_trapdoor[facing=east,half=top,open=true,powered=false,waterlogged=false]"));
    assert(trapdoor.command_name == "minecraft:trapdoor" && trapdoor.aux == 14 &&
           trapdoor.phase == ImportPhase::Attachment && trapdoor.stateful);

    const BlockSpec& lightning_rod = mapped(mapper.mapSpongeState(
        "minecraft:lightning_rod[facing=north,powered=true,waterlogged=false]"));
    assert(lightning_rod.command_name == "minecraft:lightning_rod" &&
           lightning_rod.aux == 10 && lightning_rod.phase == ImportPhase::Attachment &&
           lightning_rod.stateful && !lightning_rod.can_fill);
    const BlockSpec& oxidized_lightning_rod = mappedWithWarning(mapper.mapSpongeState(
        "minecraft:oxidized_lightning_rod[waterlogged=false,powered=false,facing=up]"));
    assert(oxidized_lightning_rod.command_name == "minecraft:lightning_rod" &&
           oxidized_lightning_rod.aux == 1 &&
           oxidized_lightning_rod.phase == ImportPhase::Attachment &&
           oxidized_lightning_rod.stateful && !oxidized_lightning_rod.can_fill);
    const BlockSpec& waxed_waterlogged_lightning_rod = mappedWithWarning(
        mapper.mapSpongeState(
            "minecraft:waxed_weathered_lightning_rod[facing=east,powered=false,waterlogged=true]"));
    assert(waxed_waterlogged_lightning_rod.command_name == "minecraft:lightning_rod" &&
           waxed_waterlogged_lightning_rod.aux == 5 &&
           waxed_waterlogged_lightning_rod.phase == ImportPhase::Attachment);

    const BlockSpec& rail = mapped(
        mapper.mapSpongeState("minecraft:rail[shape=north_east,waterlogged=false]"));
    assert(rail.command_name == "minecraft:rail" && rail.aux == 9 && rail.stateful);
    const BlockSpec& powered_rail = mapped(mapper.mapSpongeState(
        "minecraft:powered_rail[powered=true,shape=ascending_north,waterlogged=false]"));
    assert(powered_rail.command_name == "minecraft:golden_rail" && powered_rail.aux == 12);
    const BlockSpec& waterlogged_rail = mappedWithWarning(mapper.mapSpongeState(
        "minecraft:powered_rail[powered=true,shape=north_south,waterlogged=true]"));
    assert(waterlogged_rail.command_name == "minecraft:golden_rail" &&
           waterlogged_rail.aux == 8);
    const BlockSpec& wall_torch = mapped(
        mapper.mapSpongeState("minecraft:wall_torch[facing=north]"));
    assert(wall_torch.command_name == "minecraft:torch" && wall_torch.aux == 4 &&
           wall_torch.phase == ImportPhase::Attachment && !wall_torch.can_fill);
    const BlockSpec& standing_torch = mapped(
        mapper.mapSpongeState("minecraft:torch"));
    assert(standing_torch.command_name == "minecraft:torch" && standing_torch.aux == 5 &&
           standing_torch.phase == ImportPhase::Attachment && !standing_torch.can_fill);
    const BlockSpec& redstone_wall_torch = mapped(mapper.mapSpongeState(
        "minecraft:redstone_wall_torch[facing=west,lit=false]"));
    assert(redstone_wall_torch.command_name == "minecraft:unlit_redstone_torch" &&
           redstone_wall_torch.aux == 2 &&
           redstone_wall_torch.phase == ImportPhase::Attachment &&
           !redstone_wall_torch.can_fill);
    const BlockSpec& redstone_standing_torch = mapped(mapper.mapSpongeState(
        "minecraft:redstone_torch[lit=true]"));
    assert(redstone_standing_torch.command_name == "minecraft:redstone_torch" &&
           redstone_standing_torch.aux == 5 &&
           redstone_standing_torch.phase == ImportPhase::Attachment &&
           !redstone_standing_torch.can_fill);
    const BlockSpec& daylight = mapped(mapper.mapSpongeState(
        "minecraft:daylight_detector[inverted=false,power=7]"));
    assert(daylight.command_name == "minecraft:daylight_detector" && daylight.aux == 7 &&
           daylight.phase == ImportPhase::Attachment && daylight.stateful &&
           !daylight.can_fill);
    const BlockSpec& inverted_daylight = mapped(mapper.mapSpongeState(
        "minecraft:daylight_detector[inverted=true,power=12]"));
    assert(inverted_daylight.command_name == "minecraft:daylight_detector_inverted" &&
           inverted_daylight.aux == 12 && inverted_daylight.phase == ImportPhase::Attachment);
    const BlockSpec& bare_repeater = mappedWithWarning(
        mapper.mapSpongeState("minecraft:powered_repeater"));
    assert(bare_repeater.command_name == "minecraft:powered_repeater" &&
           bare_repeater.phase == ImportPhase::Attachment && !bare_repeater.can_fill);
    const BlockSpec& bare_comparator = mappedWithWarning(
        mapper.mapSpongeState("minecraft:unpowered_comparator"));
    assert(bare_comparator.command_name == "minecraft:unpowered_comparator" &&
           bare_comparator.phase == ImportPhase::Attachment && !bare_comparator.can_fill);
    const BlockSpec& bare_inverted_daylight = mappedWithWarning(
        mapper.mapSpongeState("minecraft:daylight_detector_inverted"));
    assert(bare_inverted_daylight.command_name == "minecraft:daylight_detector_inverted" &&
           bare_inverted_daylight.phase == ImportPhase::Attachment &&
           !bare_inverted_daylight.can_fill);
    assert(mapper.mapSpongeState("minecraft:piston_arm_collision").status ==
           BlockMappingStatus::Air);
    assert(mapper.mapSpongeState("minecraft:sticky_piston_arm_collision").status ==
           BlockMappingStatus::Air);
    const BlockSpec& ladder = mapped(
        mapper.mapSpongeState("minecraft:ladder[facing=east,waterlogged=false]"));
    assert(ladder.command_name == "minecraft:ladder" && ladder.aux == 5 &&
           ladder.phase == ImportPhase::Attachment && !ladder.can_fill);
    const BlockSpec& fire = mapped(mapper.mapSpongeState(
        "minecraft:fire[age=15,east=false,north=false,south=false,up=false,west=false]"));
    assert(fire.command_name == "minecraft:fire" && fire.aux == 15 &&
           fire.phase == ImportPhase::Attachment && !fire.can_fill);
    const BlockSpec& potted_cactus = mappedWithWarning(
        mapper.mapSpongeState("minecraft:potted_cactus"));
    assert(potted_cactus.command_name == "minecraft:flower_pot" &&
           potted_cactus.aux == 0 && potted_cactus.phase == ImportPhase::Attachment &&
           !potted_cactus.can_fill);
    const BlockSpec& note_block = mappedWithWarning(mapper.mapSpongeState(
        "minecraft:note_block[instrument=basedrum,note=1,powered=true]"));
    assert(note_block.command_name == "minecraft:noteblock" && note_block.stateful);

    const BlockSpec& flowing_water = mapped(
        mapper.mapSpongeState("minecraft:water[level=7]"));
    assert(flowing_water.command_name == "minecraft:water" && flowing_water.aux == 7 &&
           flowing_water.phase == ImportPhase::Fluid);
    const BlockSpec& dry_sea_pickle = mapped(mapper.mapSpongeState(
        "minecraft:sea_pickle[pickles=4,waterlogged=false]"));
    assert(dry_sea_pickle.command_name == "minecraft:sea_pickle" && dry_sea_pickle.aux == 7 &&
           dry_sea_pickle.phase == ImportPhase::Attachment && dry_sea_pickle.stateful &&
           !dry_sea_pickle.can_fill);
    const BlockSpec& waterlogged_sea_pickle = mapped(mapper.mapSpongeState(
        "minecraft:sea_pickle[pickles=2,waterlogged=true]"));
    assert(waterlogged_sea_pickle.command_name == "minecraft:sea_pickle" &&
           waterlogged_sea_pickle.aux == 1 &&
           waterlogged_sea_pickle.phase == ImportPhase::Attachment &&
           waterlogged_sea_pickle.stateful && !waterlogged_sea_pickle.can_fill);
    assert(mapper.mapSpongeState(
        "minecraft:sea_pickle[pickles=0,waterlogged=false]").status ==
           BlockMappingStatus::Unsupported);
    const BlockSpec& hopper = mapped(
        mapper.mapSpongeState("minecraft:hopper[enabled=false,facing=west]"));
    assert(hopper.command_name == "minecraft:hopper" && hopper.aux == 12 &&
           hopper.stateful && !hopper.can_fill);
    const BlockSpec& chest = mapped(mapper.mapSpongeState(
        "minecraft:chest[type=left,waterlogged=false,facing=south]"));
    assert(chest.command_name == "minecraft:chest" && chest.aux == 3 &&
           chest.stateful && !chest.can_fill);
    const BlockSpec& ender_chest = mapped(mapper.mapSpongeState(
        "minecraft:ender_chest[facing=east,waterlogged=false]"));
    assert(ender_chest.command_name == "minecraft:ender_chest" &&
           ender_chest.aux == 5 && ender_chest.stateful && !ender_chest.can_fill);
    const BlockSpec& red_shulker = mapped(mapper.mapSpongeState(
        "minecraft:red_shulker_box[facing=north]"));
    assert(red_shulker.command_name == "minecraft:red_shulker_box" &&
           red_shulker.aux == 2 && red_shulker.stateful && !red_shulker.can_fill);
    const BlockSpec& light_gray_shulker = mapped(mapper.mapSpongeState(
        "minecraft:light_gray_shulker_box[facing=up]"));
    assert(light_gray_shulker.command_name == "minecraft:silver_shulker_box" &&
           light_gray_shulker.aux == 1);
    const BlockSpec& furnace = mapped(mapper.mapSpongeState(
        "minecraft:furnace[facing=south,lit=false]"));
    assert(furnace.command_name == "minecraft:furnace" && furnace.aux == 3 &&
           furnace.stateful && !furnace.can_fill);
    const BlockSpec& lit_blast_furnace = mapped(mapper.mapSpongeState(
        "minecraft:blast_furnace[facing=west,lit=true]"));
    assert(lit_blast_furnace.command_name == "minecraft:lit_blast_furnace" &&
           lit_blast_furnace.aux == 4 && lit_blast_furnace.stateful &&
           !lit_blast_furnace.can_fill);
    const BlockSpec& powered_comparator = mapped(mapper.mapSpongeState(
        "minecraft:comparator[facing=east,mode=subtract,powered=true]"));
    assert(powered_comparator.command_name == "minecraft:powered_comparator" &&
           powered_comparator.aux == 15 &&
           powered_comparator.phase == ImportPhase::Attachment &&
           powered_comparator.stateful && !powered_comparator.can_fill);
    const BlockSpec& wire = mapped(mapper.mapSpongeState(
        "minecraft:redstone_wire[power=14,west=none,east=side,south=up,north=side]"));
    assert(wire.command_name == "minecraft:redstone_wire" && wire.aux == 14 &&
           wire.phase == ImportPhase::Attachment);
    const BlockSpec& bare_wire = mapped(mapper.mapSpongeState("minecraft:redstone_wire"));
    assert(bare_wire.command_name == "minecraft:redstone_wire" && bare_wire.aux == 0 &&
           bare_wire.phase == ImportPhase::Attachment);
    const BlockSpec& bare_sticky_piston = mappedWithWarning(
        mapper.mapSpongeState("minecraft:sticky_piston"));
    assert(bare_sticky_piston.command_name == "minecraft:sticky_piston" &&
           bare_sticky_piston.stateful && !bare_sticky_piston.can_fill);
    const BlockSpec& bare_hopper = mappedWithWarning(mapper.mapSpongeState("minecraft:hopper"));
    assert(bare_hopper.command_name == "minecraft:hopper" &&
           bare_hopper.stateful && !bare_hopper.can_fill);

    const BlockSpec& button = mapped(mapper.mapSpongeState(
        "minecraft:oak_button[face=wall,facing=west,powered=true]"));
    assert(button.command_name == "minecraft:wooden_button" && button.aux == 10 &&
           button.phase == ImportPhase::Attachment);
    const BlockSpec& lever = mapped(mapper.mapSpongeState(
        "minecraft:lever[face=ceiling,facing=east,powered=true]"));
    assert(lever.command_name == "minecraft:lever" && lever.aux == 8);

    const BlockSpec& sign = mapped(mapper.mapSpongeState(
        "minecraft:oak_sign[rotation=12,waterlogged=false]"));
    assert(sign.command_name == "minecraft:standing_sign" && sign.aux == 12);
    const BlockSpec& wall_sign = mapped(mapper.mapSpongeState(
        "minecraft:oak_wall_sign[facing=east,waterlogged=false]"));
    assert(wall_sign.command_name == "minecraft:wall_sign" && wall_sign.aux == 5);
    const BlockSpec& cherry_wall_sign = mapped(mapper.mapSpongeState(
        "minecraft:cherry_wall_sign[facing=west,waterlogged=false]"));
    assert(cherry_wall_sign.command_name == "minecraft:cherry_wall_sign" &&
           cherry_wall_sign.aux == 4);
    const BlockSpec& pale_oak_sign = mapped(mapper.mapSpongeState(
        "minecraft:pale_oak_sign[rotation=7,waterlogged=false]"));
    assert(pale_oak_sign.command_name == "minecraft:pale_oak_standing_sign" &&
           pale_oak_sign.aux == 7);
    const BlockSpec& pale_oak_wall_sign = mapped(mapper.mapSpongeState(
        "minecraft:pale_oak_wall_sign[facing=north,waterlogged=false]"));
    assert(pale_oak_wall_sign.command_name == "minecraft:pale_oak_wall_sign" &&
           pale_oak_wall_sign.aux == 2);
    const BlockSpec& pale_oak_hanging_sign = mapped(mapper.mapBedrockState(
        "minecraft:pale_oak_hanging_sign", "", 11, true));
    assert(pale_oak_hanging_sign.command_name == "minecraft:pale_oak_hanging_sign" &&
           pale_oak_hanging_sign.aux == 11);
    const BlockSpec& cherry_slab = mapped(mapper.mapSpongeState(
        "minecraft:cherry_slab[type=top,waterlogged=false]"));
    assert(cherry_slab.command_name == "minecraft:cherry_slab" && cherry_slab.aux == 1);

    const BlockSpec& powered_gate = mappedWithWarning(mapper.mapSpongeState(
        "minecraft:oak_fence_gate[facing=east,in_wall=false,open=true,powered=true]"));
    assert(powered_gate.command_name == "minecraft:fence_gate" && powered_gate.aux == 7 &&
           powered_gate.phase == ImportPhase::Structure && powered_gate.stateful &&
           !powered_gate.can_fill);
    const BlockSpec& wall_gate = mapped(mapper.mapSpongeState(
        "minecraft:oak_fence_gate[facing=east,in_wall=true,open=true,powered=false]"));
    assert(wall_gate.command_name == "minecraft:fence_gate" && wall_gate.aux == 15 &&
           wall_gate.phase == ImportPhase::Structure && wall_gate.stateful &&
           !wall_gate.can_fill);

    const BlockSpec& upper_plant = mapped(
        mapper.mapSpongeState("minecraft:sunflower[half=upper]"));
    assert(upper_plant.command_name == "minecraft:double_plant" && upper_plant.aux == 8 &&
           upper_plant.phase == ImportPhase::DependentAttachment);

    const BlockSpec& jack_o_lantern = mapped(
        mapper.mapSpongeState("minecraft:jack_o_lantern[facing=east]"));
    assert(jack_o_lantern.command_name == "minecraft:lit_pumpkin" &&
           jack_o_lantern.aux == 3);
    const BlockSpec& filled_cauldron = mappedWithWarning(
        mapper.mapSpongeState("minecraft:water_cauldron[level=3]"));
    assert(filled_cauldron.command_name == "minecraft:cauldron" &&
           filled_cauldron.aux == 0 && filled_cauldron.stateful &&
           !filled_cauldron.can_fill);
    const BlockSpec& extended_piston = mappedWithWarning(
        mapper.mapSpongeState("minecraft:piston[facing=up,extended=true]"));
    assert(extended_piston.command_name == "minecraft:piston" &&
           extended_piston.aux == 1 && extended_piston.stateful &&
           !extended_piston.can_fill);
    const BlockSpec& cave_vines_body = mappedWithWarning(
        mapper.mapSpongeState("minecraft:cave_vines_plant[berries=true]"));
    assert(cave_vines_body.command_name == "minecraft:cave_vines_body_with_berries" &&
           cave_vines_body.phase == ImportPhase::Attachment &&
           cave_vines_body.stateful && !cave_vines_body.can_fill);
    const BlockSpec& attached_melon_stem = mappedWithWarning(
        mapper.mapSpongeState("minecraft:attached_melon_stem[facing=north]"));
    assert(attached_melon_stem.command_name == "minecraft:melon_stem" &&
           attached_melon_stem.aux == 7 &&
           attached_melon_stem.phase == ImportPhase::Attachment &&
           attached_melon_stem.stateful && !attached_melon_stem.can_fill);
    const BlockSpec& double_deepslate_slab = mapped(
        mapper.mapSpongeState("minecraft:deepslate_brick_slab[type=double,waterlogged=false]"));
    assert(double_deepslate_slab.command_name == "minecraft:deepslate_brick_double_slab" &&
           double_deepslate_slab.aux == 0 && double_deepslate_slab.can_fill);
    const BlockSpec& wall_hanging_sign = mapped(mapper.mapSpongeState(
        "minecraft:oak_wall_hanging_sign[facing=east,waterlogged=false]"));
    assert(wall_hanging_sign.command_name == "minecraft:oak_hanging_sign" &&
           wall_hanging_sign.aux == 5 &&
           wall_hanging_sign.phase == ImportPhase::Attachment &&
           wall_hanging_sign.stateful && !wall_hanging_sign.can_fill);
    const BlockSpec& center_hanging_sign = mapped(mapper.mapSpongeState(
        "minecraft:pale_oak_hanging_sign[attached=true,rotation=11,waterlogged=false]"));
    assert(center_hanging_sign.command_name == "minecraft:pale_oak_hanging_sign" &&
           center_hanging_sign.aux == 0x01DAU &&
           center_hanging_sign.phase == ImportPhase::Attachment &&
           center_hanging_sign.stateful && !center_hanging_sign.can_fill);
    const BlockSpec& edge_hanging_sign = mapped(mapper.mapSpongeState(
        "minecraft:spruce_hanging_sign[attached=false,rotation=12,waterlogged=false]"));
    assert(edge_hanging_sign.command_name == "minecraft:spruce_hanging_sign" &&
           edge_hanging_sign.aux == 0x0085U &&
           edge_hanging_sign.phase == ImportPhase::Attachment &&
           edge_hanging_sign.stateful && !edge_hanging_sign.can_fill);
    for (uint32_t rotation = 0; rotation < 16; ++rotation) {
        const BlockSpec& rotated = mapped(mapper.mapSpongeState(
            "minecraft:oak_hanging_sign[attached=true,rotation=" +
            std::to_string(rotation) + ",waterlogged=false]"));
        assert(rotated.command_name == "minecraft:oak_hanging_sign" &&
               rotated.aux == static_cast<uint16_t>(0x0182U | (rotation << 3U)));
    }
    static constexpr uint32_t kEdgeRotations[] = {0, 4, 8, 12};
    static constexpr uint16_t kEdgeAux[] = {0x0083U, 0x0084U, 0x0082U, 0x0085U};
    for (size_t index = 0; index < 4; ++index) {
        const BlockSpec& edge = mapped(mapper.mapSpongeState(
            "minecraft:oak_hanging_sign[attached=false,rotation=" +
            std::to_string(kEdgeRotations[index]) + ",waterlogged=false]"));
        assert(edge.command_name == "minecraft:oak_hanging_sign" &&
               edge.aux == kEdgeAux[index]);
    }
    static constexpr const char* kWallFacings[] = {"north", "south", "west", "east"};
    static constexpr uint16_t kWallAux[] = {2, 3, 4, 5};
    for (size_t index = 0; index < 4; ++index) {
        const BlockSpec& wall_hanging = mapped(mapper.mapSpongeState(
            "minecraft:oak_wall_hanging_sign[facing=" +
            std::string(kWallFacings[index]) + ",waterlogged=false]"));
        assert(wall_hanging.command_name == "minecraft:oak_hanging_sign" &&
               wall_hanging.aux == kWallAux[index]);
    }
    const BlockSpec& wall_skull = mappedWithWarning(mapper.mapSpongeState(
        "minecraft:skeleton_wall_skull[facing=east,powered=false]"));
    assert(wall_skull.command_name == "minecraft:skull" && wall_skull.aux == 0 &&
           wall_skull.phase == ImportPhase::Attachment && wall_skull.stateful &&
           !wall_skull.can_fill);
    // Modern Sponge palettes commonly omit the legacy `powered` property on
    // player heads.  Keep this case covered because large Java schematics use
    // it for player_wall_head[facing=<direction>].
    const BlockSpec& player_wall_head = mappedWithWarning(mapper.mapSpongeState(
        "minecraft:player_wall_head[facing=west]"));
    assert(player_wall_head.command_name == "minecraft:skull" &&
           player_wall_head.aux == 3 &&
           player_wall_head.phase == ImportPhase::Attachment &&
           player_wall_head.stateful && !player_wall_head.can_fill);
    assert(mapper.mapSpongeState("minecraft:structure_void").status ==
           BlockMappingStatus::Air);
    assert(mapper.mapSpongeState(
        "minecraft:piston_head[facing=north,short=false,type=normal]").status ==
           BlockMappingStatus::Air);
    assert(mapper.mapSpongeState("minecraft:light[level=15,waterlogged=false]").status ==
           BlockMappingStatus::Air);
    assert(mapper.mapSpongeState("minecraft:cactus_flower").status ==
           BlockMappingStatus::Air);

    assert(mapper.mapSpongeState("minecraft:air").status == BlockMappingStatus::Air);
    assert(mapper.mapSpongeState("minecraft:void_air").status == BlockMappingStatus::Air);
    assert(mapper.mapSpongeState("minecraft:unknown_future_block").status ==
           BlockMappingStatus::Unsupported);
    assert(mapper.mapSpongeState("minecraft:stone[unsupported_property=value]").status ==
           BlockMappingStatus::Unsupported);
    const BlockSpec& native_stairs = mapped(mapper.mapSpongeState(
        "minecraft:oak_stairs[infinitecz_data=3]"));
    assert(native_stairs.command_name == "minecraft:oak_stairs" &&
           native_stairs.aux == 3);
    const BlockSpec& exported_coarse_dirt = mapped(mapper.mapSpongeState(
        "minecraft:coarse_dirt[infinitecz_data=0]"));
    assert(exported_coarse_dirt.command_name == "minecraft:dirt" &&
           exported_coarse_dirt.aux == 1);
    const BlockSpec& exported_red_sandstone_slab = mapped(mapper.mapSpongeState(
        "minecraft:red_sandstone_slab[infinitecz_data=0]"));
    assert(exported_red_sandstone_slab.command_name == "minecraft:stone_slab2" &&
           exported_red_sandstone_slab.aux == 0);
    assert(mapper.mapSpongeState(
        "minecraft:unknown_future_block[infinitecz_data=0]").status ==
           BlockMappingStatus::Unsupported);
    assert(mapper.mapSpongeState("minecraft:oak_stairs[infinitecz_data=16]").status ==
           BlockMappingStatus::Unsupported);
    assert(mapper.mapSpongeState(
        "minecraft:oak_stairs[infinitecz_data=3,facing=north]").status ==
           BlockMappingStatus::Unsupported);
    const BlockSpec& waterlogged_stairs = mappedWithWarning(mapper.mapSpongeState(
        "minecraft:oak_stairs[facing=north,half=top,shape=straight,waterlogged=true]"));
    assert(waterlogged_stairs.command_name == "minecraft:oak_stairs" &&
           waterlogged_stairs.aux == 7);
    assert(mapper.mapSpongeState("minecraft:stone[a=1,a=2]").status ==
           BlockMappingStatus::Unsupported);
    assert(mapper.mapSpongeState("minecraft:stone;op").status ==
           BlockMappingStatus::Unsupported);
    assert(mapper.mapSpongeState("example:stone_bricks").status ==
           BlockMappingStatus::Unsupported);
    assert(mapper.mapSpongeState("example:red_wool").status ==
           BlockMappingStatus::Unsupported);

    // BDX palette entries use a separate quoted Bedrock state array rather
    // than the Java/Sponge `name[key=value]` representation.
    const BlockSpec& bdx_stairs = mapped(mapper.mapBedrockState(
        "minecraft:quartz_stairs", "[\"upside_down_bit\"=true,\"weirdo_direction\"=1]"));
    assert(bdx_stairs.command_name == "minecraft:quartz_stairs" && bdx_stairs.aux == 5 &&
           bdx_stairs.can_fill && !bdx_stairs.stateful);
    const BlockSpec& bdx_door = mapped(mapper.mapBedrockState(
        "minecraft:birch_door", "[\"direction\"=0,\"door_hinge_bit\"=false,"
        "\"open_bit\"=true,\"upper_block_bit\"=false]"));
    assert(bdx_door.command_name == "minecraft:birch_door" && bdx_door.aux == 4 &&
           bdx_door.phase == ImportPhase::Attachment && bdx_door.stateful &&
           !bdx_door.can_fill);
    const BlockSpec& bdx_cardinal_door = mapped(mapper.mapBedrockState(
        "minecraft:spruce_door", "[\"door_hinge_bit\"=true,\"minecraft:cardinal_direction\"=\"west\","
        "\"open_bit\"=false,\"upper_block_bit\"=false]"));
    assert(bdx_cardinal_door.command_name == "minecraft:spruce_door" && bdx_cardinal_door.aux == 2 &&
           bdx_cardinal_door.phase == ImportPhase::Attachment && bdx_cardinal_door.stateful &&
           !bdx_cardinal_door.can_fill);
    const BlockSpec& bdx_trapdoor = mapped(mapper.mapBedrockState(
        "minecraft:birch_trapdoor", "[\"direction\"=3,\"open_bit\"=true,"
        "\"upside_down_bit\"=false]"));
    assert(bdx_trapdoor.command_name == "minecraft:birch_trapdoor" && bdx_trapdoor.aux == 11 &&
           bdx_trapdoor.phase == ImportPhase::Attachment && bdx_trapdoor.stateful &&
           !bdx_trapdoor.can_fill);
    const BlockSpec& bdx_slab = mapped(mapper.mapBedrockState(
        "minecraft:stone_block_slab", "[\"stone_slab_type\"=\"stone_brick\","
        "\"top_slot_bit\"=true]"));
    assert(bdx_slab.command_name == "minecraft:stone_slab" && bdx_slab.aux == 13 &&
           bdx_slab.can_fill);
    // Modern LevelDB palettes use `vertical_half` rather than the older
    // `top_slot_bit`, and may namespace the key. Both single/double aliases
    // need to reach the legacy target slab commands instead of stopping a
    // whole .mcworld import at its first slab.
    const BlockSpec& bdx_double_slab = mapped(mapper.mapBedrockState(
        "minecraft:double_stone_block_slab4",
        "[\"minecraft:vertical_half\"=\"bottom\",\"stone_slab_type_4\"=\"stone\"]"));
    assert(bdx_double_slab.command_name == "minecraft:double_stone_block_slab4" &&
           bdx_double_slab.aux == 2 && bdx_double_slab.can_fill);
    const BlockSpec& bdx_dark_prismarine_slab = mapped(mapper.mapBedrockState(
        "minecraft:dark_prismarine_slab", "[\"minecraft:vertical_half\"=\"top\"]"));
    assert(bdx_dark_prismarine_slab.command_name == "minecraft:stone_slab2" &&
           bdx_dark_prismarine_slab.aux == 11 && bdx_dark_prismarine_slab.can_fill);
    const BlockSpec& bdx_dark_prismarine_double_slab = mapped(mapper.mapBedrockState(
        "minecraft:dark_prismarine_double_slab", "[\"minecraft:vertical_half\"=\"bottom\"]"));
    assert(bdx_dark_prismarine_double_slab.command_name == "minecraft:double_stone_slab2" &&
           bdx_dark_prismarine_double_slab.aux == 3 && bdx_dark_prismarine_double_slab.can_fill);
    const BlockSpec& bdx_normal_stone_slab = mapped(mapper.mapBedrockState(
        "minecraft:normal_stone_slab", "[\"minecraft:vertical_half\"=\"bottom\"]"));
    assert(bdx_normal_stone_slab.command_name == "minecraft:stone_slab" &&
           bdx_normal_stone_slab.aux == 0 && bdx_normal_stone_slab.can_fill);
    const BlockSpec& bdx_tuff_top_slab = mapped(mapper.mapBedrockState(
        "minecraft:tuff_slab", "[\"minecraft:vertical_half\"=\"top\"]"));
    assert(bdx_tuff_top_slab.command_name == "minecraft:tuff_slab" &&
           bdx_tuff_top_slab.aux == 1 && bdx_tuff_top_slab.can_fill);
    const BlockSpec& bdx_tuff_double_slab = mapped(mapper.mapBedrockState(
        "minecraft:tuff_double_slab", "[\"top_slot_bit\"=false]"));
    assert(bdx_tuff_double_slab.command_name == "minecraft:tuff_double_slab" &&
           bdx_tuff_double_slab.aux == 0 && bdx_tuff_double_slab.can_fill);
    const BlockSpec& bdx_copper_double_slab = mapped(mapper.mapBedrockState(
        "minecraft:waxed_exposed_double_cut_copper_slab",
        "[\"minecraft:vertical_half\"=\"bottom\"]"));
    assert(bdx_copper_double_slab.command_name ==
           "minecraft:waxed_exposed_double_cut_copper_slab" &&
           bdx_copper_double_slab.aux == 0 && bdx_copper_double_slab.can_fill);
    const BlockSpec& bdx_legacy_zero_slab = mapped(mapper.mapBedrockState(
        "minecraft:stone_slab", {}, 0, true));
    const BlockSpec& bdx_legacy_top_slab = mapped(mapper.mapBedrockState(
        "minecraft:stone_slab", {}, 8, true));
    assert(bdx_legacy_zero_slab.command_name == "minecraft:stone_slab" &&
           bdx_legacy_zero_slab.aux == 0 && bdx_legacy_zero_slab.can_fill);
    assert(bdx_legacy_top_slab.command_name == "minecraft:stone_slab" &&
           bdx_legacy_top_slab.aux == 8 && bdx_legacy_top_slab.can_fill);
    // Native .infinity records carry the target identifier and full aux with
    // no textual state array. Attachment orientation/state must survive this
    // exact path and must never be merged into a /fill command.
    const BlockSpec& native_wall_torch = mapped(mapper.mapBedrockState(
        "minecraft:redstone_torch", {}, 2, true));
    assert(native_wall_torch.aux == 2 && native_wall_torch.stateful &&
           native_wall_torch.phase == ImportPhase::Attachment &&
           !native_wall_torch.can_fill);
    const BlockSpec& native_powered_lever = mapped(mapper.mapBedrockState(
        "minecraft:lever", {}, 11, true));
    assert(native_powered_lever.aux == 11 && native_powered_lever.stateful &&
           native_powered_lever.phase == ImportPhase::Attachment &&
           !native_powered_lever.can_fill);
    const BlockSpec& native_pressed_wall_button = mapped(mapper.mapBedrockState(
        "minecraft:stone_button", {}, 12, true));
    assert(native_pressed_wall_button.aux == 12 &&
           native_pressed_wall_button.stateful &&
           native_pressed_wall_button.phase == ImportPhase::Attachment &&
           !native_pressed_wall_button.can_fill);
    const BlockSpec& native_delayed_repeater = mapped(mapper.mapBedrockState(
        "minecraft:unpowered_repeater", {}, 14, true));
    assert(native_delayed_repeater.aux == 14 && native_delayed_repeater.stateful &&
           native_delayed_repeater.phase == ImportPhase::Attachment &&
           !native_delayed_repeater.can_fill);
    const BlockSpec& native_compare_comparator = mapped(mapper.mapBedrockState(
        "minecraft:unpowered_comparator", {}, 2, true));
    assert(native_compare_comparator.aux == 2 && native_compare_comparator.stateful &&
           native_compare_comparator.phase == ImportPhase::Attachment &&
           !native_compare_comparator.can_fill);
    const BlockSpec& native_subtract_comparator = mapped(mapper.mapBedrockState(
        "minecraft:powered_comparator", {}, 13, true));
    assert(native_subtract_comparator.aux == 13 && native_subtract_comparator.stateful &&
           native_subtract_comparator.phase == ImportPhase::Attachment &&
           !native_subtract_comparator.can_fill);
    // NetEase pool 117 retains these pre-flattening BDX families as a bare
    // name plus native aux. Their material/state must be decoded before the
    // generic target-name fallback.
    const BlockSpec& bdx_legacy_wood = mapped(mapper.mapBedrockState(
        "wood", {}, 17, true));
    assert(bdx_legacy_wood.command_name == "minecraft:log" &&
           bdx_legacy_wood.aux == 13 && bdx_legacy_wood.can_fill);
    const BlockSpec& bdx_legacy_dark_wood = mapped(mapper.mapBedrockState(
        "wood", {}, 45, true));
    assert(bdx_legacy_dark_wood.command_name == "minecraft:log2" &&
           bdx_legacy_dark_wood.aux == 13 && bdx_legacy_dark_wood.can_fill);
    const BlockSpec& bdx_legacy_slab3 = mapped(mapper.mapBedrockState(
        "stone_slab3", {}, 10, true));
    assert(bdx_legacy_slab3.command_name == "minecraft:stone_block_slab3" &&
           bdx_legacy_slab3.aux == 10 && bdx_legacy_slab3.can_fill);
    const BlockSpec& bdx_legacy_slab4 = mapped(mapper.mapBedrockState(
        "stone_slab4", {}, 4, true));
    assert(bdx_legacy_slab4.command_name == "minecraft:stone_block_slab4" &&
           bdx_legacy_slab4.aux == 4 && bdx_legacy_slab4.can_fill);
    const BlockSpec& bdx_legacy_double_slab4 = mapped(mapper.mapBedrockState(
        "double_stone_slab4", {}, 4, true));
    assert(bdx_legacy_double_slab4.command_name == "minecraft:double_stone_block_slab4" &&
           bdx_legacy_double_slab4.aux == 4 && bdx_legacy_double_slab4.can_fill);
    assert(mapper.mapBedrockState("double_stone_slab4", {}, 8, true).status ==
           BlockMappingStatus::Unsupported);
    const BlockSpec& bdx_legacy_shulker = mapped(mapper.mapBedrockState(
        "shulker_box", {}, 8, true));
    assert(bdx_legacy_shulker.command_name == "minecraft:silver_shulker_box" &&
           bdx_legacy_shulker.aux == 0 && bdx_legacy_shulker.stateful &&
           !bdx_legacy_shulker.can_fill);
    const BlockSpec& bdx_legacy_coral = mapped(mapper.mapBedrockState(
        "coral", {}, 11, true));
    assert(bdx_legacy_coral.command_name == "minecraft:dead_fire_coral" &&
           bdx_legacy_coral.aux == 0 && bdx_legacy_coral.stateful &&
           !bdx_legacy_coral.can_fill);
    const BlockSpec& bdx_legacy_coral_block = mapped(mapper.mapBedrockState(
        "coral_block", {}, 4, true));
    assert(bdx_legacy_coral_block.command_name == "minecraft:horn_coral_block" &&
           bdx_legacy_coral_block.aux == 0 && bdx_legacy_coral_block.stateful &&
           !bdx_legacy_coral_block.can_fill);
    const BlockSpec& bdx_legacy_dead_coral_block = mapped(mapper.mapBedrockState(
        "coral_block", {}, 11, true));
    assert(bdx_legacy_dead_coral_block.command_name == "minecraft:dead_fire_coral_block" &&
           bdx_legacy_dead_coral_block.aux == 0 && bdx_legacy_dead_coral_block.stateful &&
           !bdx_legacy_dead_coral_block.can_fill);
    assert(mapper.mapBedrockState("coral_block", {}, 5, true).status ==
           BlockMappingStatus::Unsupported);
    const BlockSpec& bdx_string_facing_observer = mapped(mapper.mapBedrockState(
        "minecraft:observer", "[\"minecraft:facing_direction\"=\"west\",\"powered_bit\"=false]"));
    assert(bdx_string_facing_observer.command_name == "minecraft:observer" &&
           bdx_string_facing_observer.aux == 4 && bdx_string_facing_observer.stateful &&
           !bdx_string_facing_observer.can_fill);
    const BlockSpec& bdx_log = mapped(mapper.mapBedrockState(
        "minecraft:log", "[\"old_log_type\"=\"spruce\",\"pillar_axis\"=\"z\"]"));
    assert(bdx_log.command_name == "minecraft:log" && bdx_log.aux == 9 && bdx_log.can_fill);
    const BlockSpec& bdx_dry_sea_pickle = mapped(mapper.mapBedrockState(
        "minecraft:sea_pickle", "[\"cluster_count\"=3,\"dead_bit\"=true]"));
    assert(bdx_dry_sea_pickle.command_name == "minecraft:sea_pickle" &&
           bdx_dry_sea_pickle.aux == 7 &&
           bdx_dry_sea_pickle.phase == ImportPhase::Attachment &&
           bdx_dry_sea_pickle.stateful && !bdx_dry_sea_pickle.can_fill);
    const BlockSpec& bdx_living_sea_pickle = mapped(mapper.mapBedrockState(
        "minecraft:sea_pickle", "[\"cluster_count\"=1,\"dead_bit\"=false]"));
    assert(bdx_living_sea_pickle.command_name == "minecraft:sea_pickle" &&
           bdx_living_sea_pickle.aux == 1 &&
           bdx_living_sea_pickle.phase == ImportPhase::Attachment &&
           bdx_living_sea_pickle.stateful && !bdx_living_sea_pickle.can_fill);
    assert(mapper.mapBedrockState(
        "minecraft:sea_pickle", "[\"cluster_count\"=4,\"dead_bit\"=true]").status ==
           BlockMappingStatus::Unsupported);
    const BlockSpec& bdx_observer = mapped(mapper.mapBedrockState(
        "minecraft:observer", "[\"minecraft:facing_direction\"=0,\"powered_bit\"=false]"));
    assert(bdx_observer.command_name == "minecraft:observer" && bdx_observer.aux == 0 &&
           bdx_observer.stateful && !bdx_observer.can_fill);
    const BlockSpec& bdx_command = mapped(mapper.mapBedrockState(
        "minecraft:repeating_command_block",
        "[\"conditional_bit\"=true,\"facing_direction\"=3]"));
    assert(bdx_command.command_name == "minecraft:repeating_command_block" &&
           bdx_command.aux == 11 && bdx_command.phase == ImportPhase::Structure &&
           bdx_command.stateful && !bdx_command.can_fill);
    const BlockSpec& bdx_runtime_command = mapped(mapper.mapBedrockState(
        "minecraft:chain_command_block", {}, 11, true));
    assert(bdx_runtime_command.command_name == "minecraft:chain_command_block" &&
           bdx_runtime_command.aux == 11 && bdx_runtime_command.stateful &&
           !bdx_runtime_command.can_fill);
    const BlockSpec& litematic_command = mapped(mapper.mapSpongeState(
        "minecraft:chain_command_block[facing=east,conditional=true]"));
    assert(litematic_command.command_name == "minecraft:chain_command_block" &&
           litematic_command.aux == 13 && litematic_command.phase == ImportPhase::Structure &&
           litematic_command.stateful && !litematic_command.can_fill);
    const BlockSpec& bdx_wall = mapped(mapper.mapBedrockState(
        "minecraft:cobblestone_wall", "[\"wall_block_type\"=\"stone_brick\","
        "\"wall_connection_type_east\"=\"tall\",\"wall_post_bit\"=true]"));
    assert(bdx_wall.command_name == "minecraft:stone_brick_wall" && bdx_wall.can_fill);
    const BlockSpec& bdx_darkoak_sign = mapped(mapper.mapBedrockState(
        "minecraft:darkoak_wall_sign", "[\"facing_direction\"=3]"));
    assert(bdx_darkoak_sign.command_name == "minecraft:darkoak_wall_sign" &&
           bdx_darkoak_sign.aux == 3 && bdx_darkoak_sign.stateful &&
           !bdx_darkoak_sign.can_fill);
    const BlockSpec& bdx_hanging_sign = mapped(mapper.mapBedrockState(
        "minecraft:pale_oak_hanging_sign",
        "[\"attached_bit\"=true,\"facing_direction\"=2,"
        "\"ground_sign_direction\"=11,\"hanging\"=true]"));
    assert(bdx_hanging_sign.command_name == "minecraft:pale_oak_hanging_sign" &&
           bdx_hanging_sign.aux == 0x01DAU && bdx_hanging_sign.stateful &&
           !bdx_hanging_sign.can_fill);
    const BlockSpec& bdx_legacy_log = mapped(
        mapper.mapBedrockState("minecraft:log", {}, 9));
    assert(bdx_legacy_log.command_name == "minecraft:log" && bdx_legacy_log.aux == 9 &&
           bdx_legacy_log.can_fill);
    // Some older BDX producers omit the minecraft namespace in their string
    // pool.  Safe bare leaves are normalized only by the BDX mapper, then
    // still have to pass the target block registry.
    const BlockSpec& bdx_bare_stone = mapped(
        mapper.mapBedrockState("stone", {}, 0, true));
    assert(bdx_bare_stone.command_name == "minecraft:stone" &&
           bdx_bare_stone.aux == 0 && bdx_bare_stone.can_fill);
    assert(mapper.mapBedrockState("example:stone", {}).status ==
           BlockMappingStatus::Unsupported);
    assert(mapper.mapBedrockState("stone;function", {}).status ==
           BlockMappingStatus::Unsupported);
    // Education/editor permission markers have no target command identity.
    // They are intentionally non-geometric and must not abort an otherwise
    // valid legacy BDX build.
    assert(mapper.mapBedrockState("deny", {}, 0, true).status ==
           BlockMappingStatus::Air);
    assert(mapper.mapBedrockState("allow", {}, 0, true).status ==
           BlockMappingStatus::Air);
    const BlockSpec& bdx_native_furnace_fallback = mappedWithWarning(mapper.mapBedrockState(
        "minecraft:furnace", "[\"facing_direction\"=0]"));
    assert(bdx_native_furnace_fallback.command_name == "minecraft:furnace" &&
           bdx_native_furnace_fallback.aux == 0 &&
           bdx_native_furnace_fallback.stateful && !bdx_native_furnace_fallback.can_fill);
    assert(mapper.mapBedrockState("minecraft:light_block", "[\"block_light_level\"=15]").status ==
           BlockMappingStatus::Air);
    assert(mapper.mapBedrockState("minecraft:command_block",
                                  "[\"facing_direction\"=3,\"text\"=\"/op test\"]").status ==
           BlockMappingStatus::Unsupported);
    assert(!mapper.resolveBedrockState("minecraft:light_block", "[\"block_light_level\"=15]").has_value());

    assert(!mapper.resolveLegacy(0, 0).has_value());
    assert(!mapper.resolveSpongeState("minecraft:unknown_future_block").has_value());
    return 0;
}
