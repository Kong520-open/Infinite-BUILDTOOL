#include "../ProjectionBlockIdentity.h"

#include <cassert>
#include <cstdint>

using namespace build_import;

namespace {

void expectIdentity(const ProjectionBlockIdentity& identity, const char* name,
                    uint16_t aux) {
    assert(identity.name == name);
    assert(identity.aux == aux);
}

}  // namespace

int main() {
    // Canonicalization must remain exact and namespace/state-insensitive.  The
    // printer receives names from both legacy projection records and current
    // inventory snapshots, so casing and serialized state suffixes must not
    // cause a material lookup to wait forever.
    assert(ProjectionCanonicalBlockName(" Minecraft:PLANKS[wood_type=spruce] ") ==
           "planks");
    assert(ProjectionCanonicalBlockName("minecraft:spruce_planks") ==
           "spruce_planks");

    // Legacy lit container and redstone identifiers have been flattened in
    // current Bedrock snapshots.  Canonicalization must retain their static
    // aux while collapsing only the transient powered spelling.
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:lit_blast_furnace", 5U),
                   "blast_furnace", 5U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:lit_smoker", 2U),
                   "smoker", 2U);
    assert(ProjectionBlockIdentityMatches("minecraft:lit_blast_furnace", 5U,
                                          "minecraft:blast_furnace", 5U));
    assert(ProjectionBlockIdentityMatches("minecraft:lit_smoker", 2U,
                                          "minecraft:smoker", 2U));

    // Powered/unpowered repeater and comparator names represent the same
    // physical shell.  Their dynamic powered bit is normalized away, whereas
    // delay and subtract-mode remain part of the exact placed identity.
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:powered_repeater", 29U),
                   "repeater", 13U);
    expectIdentity(NormalizeLiveBlockIdentity("minecraft:unpowered_repeater", 13U),
                   "repeater", 13U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:powered_comparator", 13U),
                   "comparator", 5U);
    expectIdentity(NormalizeLiveBlockIdentity("minecraft:unpowered_comparator", 5U),
                   "comparator", 5U);

    // Legacy planks store the species in aux while current inventory reports a
    // per-species item name.  This is the concrete spruce-planks regression:
    // legacy projection aux=1 must select minecraft:spruce_planks aux=0.
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:planks", 0U),
                   "oak_planks", 0U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:planks", 1U),
                   "spruce_planks", 0U);
    expectIdentity(NormalizeLiveBlockIdentity("minecraft:spruce_planks", 0U),
                   "spruce_planks", 0U);
    // A current client build can retain the old generic plank aux after it
    // has already flattened the item name. It is not a spruce-planks state.
    expectIdentity(NormalizeLiveBlockIdentity("minecraft:spruce_planks", 1U),
                   "spruce_planks", 0U);
    assert(ProjectionBlockMaterialMatches("minecraft:planks", 1U,
                                          "minecraft:spruce_planks", 0U));
    assert(ProjectionBlockMaterialMatches("minecraft:planks", 1U,
                                          "minecraft:spruce_planks", 1U));
    assert(ProjectionBlockMaterialMatches("minecraft:spruce_planks", 0U,
                                          "minecraft:spruce_planks", 1U));
    assert(ProjectionBlockIdentityMatches("minecraft:planks", 1U,
                                          "minecraft:spruce_planks", 0U));
    assert(!ProjectionBlockMaterialMatches("minecraft:planks", 1U,
                                           "minecraft:oak_planks", 0U));
    assert(!ProjectionBlockIdentityMatches("minecraft:planks", 1U,
                                            "minecraft:oak_planks", 0U));
    assert(!ProjectionBlockMaterialMatches("minecraft:spruce_planks", 0U,
                                           "minecraft:oak_planks", 1U));

    // The generic legacy sapling uses low bits for species and bit 3 only as
    // a growth/update marker. Both records must select the same flattened
    // inventory item rather than stalling on a fictitious aux variant.
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:sapling", 1U),
                    "spruce_sapling", 0U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:sapling", 9U),
                    "spruce_sapling", 0U);
    assert(ProjectionBlockMaterialMatches("minecraft:sapling", 9U,
                                           "minecraft:spruce_sapling", 0U));
    assert(ProjectionBlockIdentityMatches("minecraft:sapling", 9U,
                                           "minecraft:spruce_sapling", 0U));
    assert(!ProjectionBlockMaterialMatches("minecraft:sapling", 6U,
                                            "minecraft:oak_sapling", 0U));

    // Legacy wooden_slab uses the low bits for wood species and bit 0x8 for
    // vertical half.  Normalization may change the material name, but must
    // preserve the top-bit so world-state equality distinguishes both halves.
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:wooden_slab", 1U),
                   "spruce_slab", 0U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:wooden_slab", 9U),
                   "spruce_slab", 8U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:spruce_slab", 0U),
                   "spruce_slab", 0U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:spruce_slab", 8U),
                   "spruce_slab", 8U);

    // A hotbar slab item is valid for either placement half, while exact world
    // comparison must reject a block already placed on the opposite half.
    assert(ProjectionBlockMaterialMatches("minecraft:wooden_slab", 1U,
                                          "minecraft:spruce_slab", 0U));
    assert(ProjectionBlockMaterialMatches("minecraft:wooden_slab", 9U,
                                          "minecraft:spruce_slab", 0U));
    assert(ProjectionBlockMaterialMatches("minecraft:spruce_slab", 0U,
                                          "minecraft:spruce_slab", 8U));
    assert(ProjectionBlockIdentityMatches("minecraft:wooden_slab", 1U,
                                          "minecraft:spruce_slab", 0U));
    assert(ProjectionBlockIdentityMatches("minecraft:wooden_slab", 9U,
                                          "minecraft:spruce_slab", 8U));
    assert(!ProjectionBlockIdentityMatches("minecraft:wooden_slab", 1U,
                                           "minecraft:spruce_slab", 8U));
    assert(!ProjectionBlockIdentityMatches("minecraft:wooden_slab", 9U,
                                           "minecraft:spruce_slab", 0U));
    assert(!ProjectionBlockMaterialMatches("minecraft:wooden_slab", 1U,
                                           "minecraft:birch_slab", 0U));

    // Legacy stone slab families encode both material and top-bit in aux.
    // The flattened inventory names must resolve to the same identity without
    // losing the physical placement half.
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:stone_slab", 13U),
                   "stone_brick_slab", 8U);
    assert(ProjectionBlockIdentityMatches("minecraft:stone_slab", 13U,
                                          "minecraft:stone_brick_slab", 8U));
    assert(!ProjectionBlockIdentityMatches("minecraft:stone_slab", 13U,
                                           "minecraft:stone_brick_slab", 0U));
    assert(ProjectionBlockMaterialMatches("minecraft:stone_slab", 13U,
                                          "minecraft:stone_brick_slab", 0U));
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:stone_block_slab4", 2U),
                   "stone_slab", 0U);
    expectIdentity(NormalizeLiveBlockIdentity("minecraft:stone_slab", 0U),
                   "stone_slab", 0U);
    expectIdentity(NormalizeLiveBlockIdentity("minecraft:stone_slab", 1U),
                   "stone_slab", 8U);
    assert(ProjectionBlockIdentityMatches("minecraft:stone_block_slab4", 2U,
                                          "minecraft:stone_slab", 0U));

    // A complete slab is still made from the ordinary slab item. Normalize
    // every legacy/current double spelling to that material plus the explicit
    // full-block marker, while preserving the single-half distinction.
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:double_stone_block_slab4", 2U),
                   "stone_slab", kProjectionSlabDoubleAux);
    expectIdentity(NormalizeLiveBlockIdentity("minecraft:double_stone_block_slab4", 2U),
                   "stone_slab", kProjectionSlabDoubleAux);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:oak_double_slab", 0U),
                   "oak_slab", kProjectionSlabDoubleAux);
    expectIdentity(NormalizeLiveBlockIdentity("minecraft:polished_deepslate_double_slab", 0U),
                   "polished_deepslate_slab", kProjectionSlabDoubleAux);
    expectIdentity(NormalizeLiveBlockIdentity("minecraft:double_cut_copper_slab", 0U),
                   "cut_copper_slab", kProjectionSlabDoubleAux);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:oak_slab",
                                                     kProjectionSlabDoubleAux),
                   "oak_slab", kProjectionSlabDoubleAux);
    assert(ProjectionBlockIdentityMatches("minecraft:oak_double_slab", 0U,
                                          "minecraft:oak_slab",
                                          kProjectionSlabDoubleAux));
    assert(!ProjectionBlockIdentityMatches("minecraft:oak_double_slab", 0U,
                                           "minecraft:oak_slab", 0U));
    assert(ProjectionBlockMaterialMatches("minecraft:oak_double_slab", 0U,
                                          "minecraft:oak_slab", 0U));
    // A compact live aux of one remains an upper half, never a complete slab.
    expectIdentity(NormalizeLiveBlockIdentity("minecraft:stone_slab", 1U),
                   "stone_slab", kProjectionSlabTopAux);

    // More legacy source aliases which otherwise make the printer remain in
    // “waiting for material” even though the exact flattened block exists in
    // the player's live inventory.  Each comparison is deliberately ordered
    // source projection first, live data second.
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:sandstone", 1U),
                   "chiseled_sandstone", 0U);
    expectIdentity(NormalizeLiveBlockIdentity("minecraft:chiseled_sandstone", 0U),
                   "chiseled_sandstone", 0U);
    assert(ProjectionBlockIdentityMatches("minecraft:sandstone", 1U,
                                          "minecraft:chiseled_sandstone", 0U));
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:sandstone", 2U),
                   "smooth_sandstone", 0U);
    assert(ProjectionBlockIdentityMatches("minecraft:sandstone", 2U,
                                          "minecraft:smooth_sandstone", 0U));

    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:red_sandstone", 1U),
                   "chiseled_red_sandstone", 0U);
    assert(ProjectionBlockIdentityMatches("minecraft:red_sandstone", 1U,
                                          "minecraft:chiseled_red_sandstone", 0U));
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:red_sandstone", 2U),
                   "smooth_red_sandstone", 0U);
    assert(ProjectionBlockIdentityMatches("minecraft:red_sandstone", 2U,
                                          "minecraft:smooth_red_sandstone", 0U));

    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:quartz_block", 1U),
                   "chiseled_quartz_block", 0U);
    assert(ProjectionBlockIdentityMatches("minecraft:quartz_block", 1U,
                                          "minecraft:chiseled_quartz_block", 0U));
    // Old quartz pillar uses 2/6/10 for y/x/z, while current live blocks use
    // the compact modern pillar axis 0/1/2.  The axis must not be discarded
    // by source-to-live equality.
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:quartz_block", 2U),
                   "quartz_pillar", 0U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:quartz_block", 6U),
                   "quartz_pillar", 1U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:quartz_block", 10U),
                   "quartz_pillar", 2U);
    assert(ProjectionBlockIdentityMatches("minecraft:quartz_block", 2U,
                                          "minecraft:quartz_pillar", 0U));
    assert(ProjectionBlockIdentityMatches("minecraft:quartz_block", 6U,
                                          "minecraft:quartz_pillar", 1U));
    assert(ProjectionBlockIdentityMatches("minecraft:quartz_block", 10U,
                                          "minecraft:quartz_pillar", 2U));

    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:sponge", 1U),
                   "wet_sponge", 0U);
    assert(ProjectionBlockIdentityMatches("minecraft:sponge", 1U,
                                          "minecraft:wet_sponge", 0U));
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:cobblestone_wall", 1U),
                   "mossy_cobblestone_wall", 0U);
    assert(ProjectionBlockIdentityMatches("minecraft:cobblestone_wall", 1U,
                                          "minecraft:mossy_cobblestone_wall", 0U));
    expectIdentity(NormalizeProjectionBlockIdentity(
                       "minecraft:silver_glazed_terracotta", 0U),
                   "light_gray_glazed_terracotta", 0U);
    assert(ProjectionBlockIdentityMatches("minecraft:silver_glazed_terracotta", 0U,
                                          "minecraft:light_gray_glazed_terracotta", 0U));

    // Several historical stair names collide with, or were later split into,
    // flattened item names.  Source aliases must be applied only on the left
    // projection side and must retain every orientation/top-half aux bit.
    expectIdentity(NormalizeProjectionBlockIdentity(
                       "minecraft:normal_stone_stairs", 5U),
                   "stone_stairs", 5U);
    assert(ProjectionBlockIdentityMatches("minecraft:normal_stone_stairs", 5U,
                                           "minecraft:stone_stairs", 5U));

    // Material-summary records have already been flattened once.  Comparing a
    // canonical material to a live inventory item must not apply source-only
    // aliases a second time (old stone_stairs would otherwise become
    // cobblestone_stairs and falsely make the material list/printer wait).
    assert(ProjectionCanonicalMaterialMatchesLive("minecraft:stone_stairs", 0U,
                                                  "minecraft:stone_stairs", 3U));
    assert(!ProjectionCanonicalMaterialMatchesLive("minecraft:stone_stairs", 0U,
                                                   "minecraft:cobblestone_stairs", 3U));
    // Inventory materials intentionally ignore placement-only state such as a
    // slab half; one slab item can place both halves.
    assert(ProjectionCanonicalMaterialMatchesLive("minecraft:stone_slab", 0U,
                                                  "minecraft:stone_slab", 1U));
    assert(!ProjectionCanonicalMaterialMatchesLive("minecraft:stone_slab", 0U,
                                                   "minecraft:smooth_stone_slab", 0U));
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:stone_stairs", 6U),
                   "cobblestone_stairs", 6U);
    assert(ProjectionBlockIdentityMatches("minecraft:stone_stairs", 6U,
                                          "minecraft:cobblestone_stairs", 6U));
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:end_brick_stairs", 3U),
                   "end_stone_brick_stairs", 3U);
    assert(ProjectionBlockIdentityMatches("minecraft:end_brick_stairs", 3U,
                                          "minecraft:end_stone_brick_stairs", 3U));
    expectIdentity(NormalizeProjectionBlockIdentity(
                       "minecraft:prismarine_bricks_stairs", 7U),
                   "prismarine_brick_stairs", 7U);
    assert(ProjectionBlockIdentityMatches("minecraft:prismarine_bricks_stairs", 7U,
                                          "minecraft:prismarine_brick_stairs", 7U));

    // Pumpkins are yaw-directed but their old source names still flatten to
    // current carved-pumpkin/jack-o-lantern materials without discarding yaw.
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:pumpkin", 3U),
                   "carved_pumpkin", 3U);
    assert(ProjectionBlockIdentityMatches("minecraft:pumpkin", 3U,
                                          "minecraft:carved_pumpkin", 3U));
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:lit_pumpkin", 1U),
                   "jack_o_lantern", 1U);
    assert(ProjectionBlockIdentityMatches("minecraft:lit_pumpkin", 1U,
                                          "minecraft:jack_o_lantern", 1U));

    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:snow", 0U),
                   "snow_block", 0U);
    assert(ProjectionBlockIdentityMatches("minecraft:snow", 0U,
                                          "minecraft:snow_block", 0U));
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:slime", 0U),
                   "slime_block", 0U);
    assert(ProjectionBlockIdentityMatches("minecraft:slime", 0U,
                                          "minecraft:slime_block", 0U));
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:quartz_block", 3U),
                   "smooth_quartz", 0U);
    assert(ProjectionBlockIdentityMatches("minecraft:quartz_block", 3U,
                                          "minecraft:smooth_quartz", 0U));

    // The legacy decay/persistence flags in bits 2/3 do not describe a leaf
    // material or a placement orientation.  All old leaf species must still
    // select the matching flattened item even with both flags present.
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:leaves", 12U),
                   "oak_leaves", 0U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:leaves", 13U),
                   "spruce_leaves", 0U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:leaves", 14U),
                   "birch_leaves", 0U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:leaves", 15U),
                   "jungle_leaves", 0U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:leaves2", 12U),
                   "acacia_leaves", 0U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:leaves2", 13U),
                   "dark_oak_leaves", 0U);
    assert(ProjectionBlockMaterialMatches("minecraft:leaves", 12U,
                                          "minecraft:oak_leaves", 0U));
    assert(ProjectionBlockMaterialMatches("minecraft:leaves", 13U,
                                          "minecraft:spruce_leaves", 0U));
    assert(ProjectionBlockMaterialMatches("minecraft:leaves", 14U,
                                          "minecraft:birch_leaves", 0U));
    assert(ProjectionBlockMaterialMatches("minecraft:leaves", 15U,
                                          "minecraft:jungle_leaves", 0U));
    assert(ProjectionBlockMaterialMatches("minecraft:leaves2", 12U,
                                          "minecraft:acacia_leaves", 0U));
    assert(ProjectionBlockMaterialMatches("minecraft:leaves2", 13U,
                                           "minecraft:dark_oak_leaves", 0U));
    // Flattened leaves can report persistence/distance in their native aux.
    // That state is not an inventory variant and must not block material
    // selection or make an otherwise correct placed leaf turn yellow.
    expectIdentity(NormalizeLiveBlockIdentity("minecraft:spruce_leaves", 2U),
                    "spruce_leaves", 0U);
    assert(ProjectionBlockMaterialMatches("minecraft:spruce_leaves", 0U,
                                           "minecraft:spruce_leaves", 2U));
    assert(ProjectionBlockIdentityMatches("minecraft:spruce_leaves", 0U,
                                           "minecraft:spruce_leaves", 2U));
    assert(ProjectionBlockMaterialMatches("minecraft:nether_quartz_ore", 0U,
                                           "minecraft:quartz_ore", 0U));

    // Vertical half is invariant under every horizontal projection rotation.
    // The placement helper therefore has no rotation argument: it decodes the
    // physical half which the printer must preserve while only horizontal
    // directions are transformed elsewhere.
    bool top = false;
    bool is_double = false;
    assert(TryProjectionSlabPlacement("minecraft:wooden_slab", 1U, &top,
                                      &is_double) &&
           !top && !is_double);
    assert(TryProjectionSlabPlacement("minecraft:wooden_slab", 9U, &top,
                                      &is_double) &&
           top && !is_double);
    assert(TryProjectionSlabPlacement("minecraft:spruce_slab", 0U, &top,
                                      &is_double) &&
           !top && !is_double);
    assert(TryProjectionSlabPlacement("minecraft:spruce_slab", 1U, &top,
                                      &is_double) &&
           top && !is_double);
    assert(TryProjectionSlabPlacement("minecraft:spruce_slab", 8U, &top,
                                      &is_double) &&
           top && !is_double);
    assert(TryProjectionSlabPlacement("minecraft:double_wooden_slab", 1U, &top,
                                      &is_double) &&
           !top && is_double);
    assert(!TryProjectionSlabPlacement("minecraft:spruce_slab", 2U, &top,
                                       &is_double));

    // Axis blocks rotate only in the horizontal plane.  Legacy log aux packs
    // wood material in bits 0..1 and axis in bits 2..3, so X/Z changes must
    // leave the spruce material (low bits = 1) untouched.
    ProjectionBlockAxis axis = ProjectionBlockAxis::Y;
    uint16_t rotated_aux = 0U;
    assert(TryProjectionBlockAxis("minecraft:log", 5U, &axis) &&
           axis == ProjectionBlockAxis::X);
    assert(RotateProjectionBlockAxisAux("minecraft:log", 5U, 1U,
                                        &rotated_aux) &&
           rotated_aux == 9U);
    assert(TryProjectionBlockAxis("minecraft:log", rotated_aux, &axis) &&
           axis == ProjectionBlockAxis::Z);
    assert(RotateProjectionBlockAxisAux("minecraft:log", 9U, 1U,
                                        &rotated_aux) &&
           rotated_aux == 5U);
    assert(RotateProjectionBlockAxisAux("minecraft:log", 5U, 2U,
                                        &rotated_aux) &&
           rotated_aux == 5U);
    assert(RotateProjectionBlockAxisAux("minecraft:oak_log", 1U, 1U,
                                        &rotated_aux) &&
           rotated_aux == 2U);
    assert(RotateProjectionBlockAxisAux("minecraft:oak_log", 2U, 3U,
                                        &rotated_aux) &&
           rotated_aux == 1U);
    assert(RotateProjectionBlockAxisAux("minecraft:oak_log", 0U, 1U,
                                        &rotated_aux) &&
           rotated_aux == 0U);

    // Ordinary six-way blocks use the ItemUse face order directly.  Preserve
    // unrelated state bit 3 while rotating the low facing bits, and do not
    // rotate the vertical faces.
    ProjectionBlockFace face = ProjectionBlockFace::Down;
    assert(TryProjectionBlockFace("minecraft:lightning_rod", 2U, &face) &&
           face == ProjectionBlockFace::North);
    assert(RotateProjectionBlockFaceAux("minecraft:lightning_rod", 2U, 1U,
                                        &rotated_aux) &&
           rotated_aux == 5U);
    assert(RotateProjectionBlockFaceAux("minecraft:lightning_rod", 10U, 1U,
                                        &rotated_aux) &&
           rotated_aux == 13U);
    assert(RotateProjectionBlockFaceAux("minecraft:lightning_rod", 0U, 3U,
                                        &rotated_aux) &&
           rotated_aux == 0U);
    assert(RotateProjectionBlockFaceAux("minecraft:lightning_rod", 1U, 2U,
                                        &rotated_aux) &&
           rotated_aux == 1U);

    // Contentless container shells are deliberately allow-listed: their
    // physical facing is restored, while block-actor inventory data is not.
    // Six-way barrel/hopper/dispenser state uses the ordinary face layout.
    assert(IsProjectionContentlessContainerShell("minecraft:hopper"));
    assert(IsProjectionContentlessContainerShell("minecraft:lit_smoker"));
    assert(!IsProjectionContentlessContainerShell("minecraft:observer"));
    assert(TryProjectionSixWayFacing("minecraft:barrel", 2U, &face) &&
           face == ProjectionBlockFace::North);
    assert(RotateProjectionSixWayFacingAux("minecraft:barrel", 2U, 1U,
                                           &rotated_aux) &&
           rotated_aux == 5U);  // north -> east

    // Hopper has no upward outlet.  Its enabled/disabled state occupies bit
    // 3 and therefore must survive a rotation yet not make a correct shell
    // appear mismatched after the redstone graph changes it.
    assert(TryProjectionSixWayFacing("minecraft:hopper", 10U, &face) &&
           face == ProjectionBlockFace::North);
    assert(RotateProjectionSixWayFacingAux("minecraft:hopper", 10U, 1U,
                                           &rotated_aux) &&
           rotated_aux == 13U);
    assert(!TryProjectionSixWayFacing("minecraft:hopper", 1U, &face));
    assert(!ReplaceProjectionSixWayFacing("minecraft:hopper", 0U,
                                          ProjectionBlockFace::Up, &rotated_aux));
    assert(ProjectionBlockIdentityMatches("minecraft:hopper", 2U,
                                          "minecraft:hopper", 10U));
    assert(!ProjectionBlockIdentityMatches("minecraft:hopper", 2U,
                                           "minecraft:hopper", 5U));
    assert(ProjectionBlockMaterialMatches("minecraft:hopper", 2U,
                                          "minecraft:hopper", 5U));

    // Chests and furnace-family shells can only face horizontally.  Their
    // silent player yaw is the opposite of the requested stored face so their
    // placement does not visibly rotate the local camera.
    assert(TryProjectionHorizontalSixWayFacing("minecraft:lit_blast_furnace", 2U,
                                               &face) &&
           face == ProjectionBlockFace::North);
    assert(RotateProjectionHorizontalSixWayFacingAux("minecraft:chest", 3U, 1U,
                                                      &rotated_aux) &&
           rotated_aux == 4U);  // south -> west
    float container_yaw = 0.0F;
    assert(TryProjectionHorizontalSixWaySilentYaw("minecraft:lit_smoker", 2U, 1U,
                                                  &container_yaw) &&
           container_yaw == 90.0F);  // rotated north -> east
    assert(TryProjectionHorizontalSixWaySilentYaw("minecraft:chest", 3U, 1U,
                                                  &container_yaw) &&
           container_yaw == -90.0F);  // rotated south -> west
    assert(!TryProjectionHorizontalSixWayFacing("minecraft:chest", 1U, &face));
    assert(!ProjectionBlockIdentityMatches("minecraft:chest", 2U,
                                           "minecraft:chest", 5U));
    assert(ProjectionBlockMaterialMatches("minecraft:chest", 2U,
                                          "minecraft:chest", 5U));

    // Ladders share the ordinary horizontal face numbers but deliberately
    // reject vertical faces; a north-facing source must become east-facing
    // after one clockwise projected quarter turn.
    assert(TryProjectionBlockFace("minecraft:ladder", 2U, &face) &&
           face == ProjectionBlockFace::North);
    assert(RotateProjectionBlockFaceAux("minecraft:ladder", 2U, 1U,
                                        &rotated_aux) &&
           rotated_aux == 5U);
    assert(!RotateProjectionBlockFaceAux("minecraft:ladder", 0U, 1U,
                                         &rotated_aux));

    // End rods encode north/south differently from ordinary six-way blocks.
    // The API must account for that historical layout rather than treating
    // their raw aux as a PlayerAction face number.
    assert(TryProjectionBlockFace("minecraft:end_rod", 3U, &face) &&
           face == ProjectionBlockFace::North);
    assert(RotateProjectionBlockFaceAux("minecraft:end_rod", 3U, 1U,
                                        &rotated_aux) &&
           rotated_aux == 4U);  // north -> east
    assert(RotateProjectionBlockFaceAux("minecraft:end_rod", 2U, 1U,
                                        &rotated_aux) &&
           rotated_aux == 5U);  // south -> west
    assert(!RotateProjectionBlockFaceAux("minecraft:end_rod", 6U, 1U,
                                         &rotated_aux));

    // Torch attachment aux is not ItemUse face order: 1..4 encode
    // east/west/south/north and 5 is the floor attachment.  Verify all five
    // records and the complete clockwise wall cycle.
    assert(TryProjectionAttachmentFace("minecraft:torch", 1U, &face) &&
           face == ProjectionBlockFace::East);
    assert(TryProjectionAttachmentFace("minecraft:torch", 2U, &face) &&
           face == ProjectionBlockFace::West);
    assert(TryProjectionAttachmentFace("minecraft:torch", 3U, &face) &&
           face == ProjectionBlockFace::South);
    assert(TryProjectionAttachmentFace("minecraft:torch", 4U, &face) &&
           face == ProjectionBlockFace::North);
    assert(TryProjectionAttachmentFace("minecraft:torch", 5U, &face) &&
           face == ProjectionBlockFace::Up);
    assert(RotateProjectionAttachmentFaceAux("minecraft:torch", 1U, 1U,
                                              &rotated_aux) &&
           rotated_aux == 3U);  // east -> south
    assert(RotateProjectionAttachmentFaceAux("minecraft:torch", 3U, 1U,
                                              &rotated_aux) &&
           rotated_aux == 2U);  // south -> west
    assert(RotateProjectionAttachmentFaceAux("minecraft:torch", 2U, 1U,
                                              &rotated_aux) &&
           rotated_aux == 4U);  // west -> north
    assert(RotateProjectionAttachmentFaceAux("minecraft:torch", 4U, 1U,
                                              &rotated_aux) &&
           rotated_aux == 1U);  // north -> east
    assert(RotateProjectionAttachmentFaceAux("minecraft:torch", 5U, 3U,
                                              &rotated_aux) &&
           rotated_aux == 5U);

    // Buttons add a ceiling attachment at 0 and transient powered bit 0x8.
    // Power is not part of a physical placement identity, but must survive a
    // direction replacement packet so a live record is not needlessly red.
    assert(TryProjectionAttachmentFace("minecraft:stone_button", 0U, &face) &&
           face == ProjectionBlockFace::Down);
    assert(TryProjectionAttachmentFace("minecraft:stone_button", 1U, &face) &&
           face == ProjectionBlockFace::East);
    assert(TryProjectionAttachmentFace("minecraft:stone_button", 2U, &face) &&
           face == ProjectionBlockFace::West);
    assert(TryProjectionAttachmentFace("minecraft:stone_button", 3U, &face) &&
           face == ProjectionBlockFace::South);
    assert(TryProjectionAttachmentFace("minecraft:stone_button", 4U, &face) &&
           face == ProjectionBlockFace::North);
    assert(TryProjectionAttachmentFace("minecraft:stone_button", 5U, &face) &&
           face == ProjectionBlockFace::Up);
    assert(RotateProjectionAttachmentFaceAux("minecraft:stone_button", 1U, 1U,
                                              &rotated_aux) &&
           rotated_aux == 3U);
    assert(RotateProjectionAttachmentFaceAux("minecraft:stone_button", 9U, 1U,
                                              &rotated_aux) &&
           rotated_aux == 11U);
    assert(RotateProjectionAttachmentFaceAux("minecraft:stone_button", 0U, 1U,
                                              &rotated_aux) &&
           rotated_aux == 0U);
    assert(RotateProjectionAttachmentFaceAux("minecraft:stone_button", 5U, 3U,
                                              &rotated_aux) &&
           rotated_aux == 5U);
    assert(ProjectionBlockIdentityMatches("minecraft:stone_button", 1U,
                                          "minecraft:stone_button", 9U));

    // Lever 0..7 represents two ceiling axes, four wall faces and two floor
    // axes.  A horizontal projection swaps each floor/ceiling axis pair on an
    // odd turn, rotates wall faces normally, and retains the powered bit.
    assert(TryProjectionLeverAttachmentFace("minecraft:lever", 0U, &face) &&
           face == ProjectionBlockFace::Down);
    assert(TryProjectionLeverAttachmentFace("minecraft:lever", 1U, &face) &&
           face == ProjectionBlockFace::East);
    assert(TryProjectionLeverAttachmentFace("minecraft:lever", 2U, &face) &&
           face == ProjectionBlockFace::West);
    assert(TryProjectionLeverAttachmentFace("minecraft:lever", 3U, &face) &&
           face == ProjectionBlockFace::South);
    assert(TryProjectionLeverAttachmentFace("minecraft:lever", 4U, &face) &&
           face == ProjectionBlockFace::North);
    assert(TryProjectionLeverAttachmentFace("minecraft:lever", 5U, &face) &&
           face == ProjectionBlockFace::Up);
    assert(TryProjectionLeverAttachmentFace("minecraft:lever", 6U, &face) &&
           face == ProjectionBlockFace::Up);
    assert(TryProjectionLeverAttachmentFace("minecraft:lever", 7U, &face) &&
           face == ProjectionBlockFace::Down);
    assert(RotateProjectionLeverAux("minecraft:lever", 0U, 1U, &rotated_aux) &&
           rotated_aux == 7U);
    assert(RotateProjectionLeverAux("minecraft:lever", 7U, 1U, &rotated_aux) &&
           rotated_aux == 0U);
    assert(RotateProjectionLeverAux("minecraft:lever", 5U, 1U, &rotated_aux) &&
           rotated_aux == 6U);
    assert(RotateProjectionLeverAux("minecraft:lever", 6U, 1U, &rotated_aux) &&
           rotated_aux == 5U);
    assert(RotateProjectionLeverAux("minecraft:lever", 1U, 1U, &rotated_aux) &&
           rotated_aux == 3U);  // east wall -> south wall
    assert(RotateProjectionLeverAux("minecraft:lever", 9U, 1U, &rotated_aux) &&
           rotated_aux == 11U); // powered east wall -> powered south wall
    assert(ProjectionBlockIdentityMatches("minecraft:lever", 3U,
                                          "minecraft:lever", 11U));
    float lever_yaw = 0.0F;
    assert(TryProjectionLeverSilentYaw("minecraft:lever", 0U, 0U, &lever_yaw) &&
           lever_yaw == 90.0F);
    assert(TryProjectionLeverSilentYaw("minecraft:lever", 5U, 0U, &lever_yaw) &&
           lever_yaw == 0.0F);
    assert(TryProjectionLeverSilentYaw("minecraft:lever", 5U, 1U, &lever_yaw) &&
           lever_yaw == 90.0F);

    // Legacy anvil packs wear in bits 2..3 and its yaw direction in 0..1.
    // The source identity flattens the material to chipped/damaged_anvil but
    // keeps the compact live direction, and rotation preserves wear bits.
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:anvil", 4U),
                   "chipped_anvil", 0U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:anvil", 7U),
                   "chipped_anvil", 3U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:anvil", 8U),
                   "damaged_anvil", 0U);
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:anvil", 11U),
                   "damaged_anvil", 3U);
    assert(ProjectionBlockIdentityMatches("minecraft:anvil", 4U,
                                          "minecraft:chipped_anvil", 0U));
    assert(ProjectionBlockIdentityMatches("minecraft:anvil", 8U,
                                          "minecraft:damaged_anvil", 0U));
    uint8_t anvil_direction = 0U;
    assert(TryProjectionHorizontalDirection("minecraft:anvil", 5U,
                                            &anvil_direction) &&
           anvil_direction == 1U);
    assert(RotateProjectionHorizontalDirectionAux("minecraft:anvil", 5U, 1U,
                                                   &rotated_aux) &&
           rotated_aux == 6U);
    assert(RotateProjectionHorizontalDirectionAux("minecraft:anvil", 11U, 1U,
                                                   &rotated_aux) &&
           rotated_aux == 8U);
    assert(ProjectionBlockMaterialMatches("minecraft:anvil", 5U,
                                          "minecraft:chipped_anvil", 2U));

    // Player-yaw-directed blocks use south, west, north, east as compact
    // 0..3 directions.  A projected clockwise quarter turn advances exactly
    // one slot and a full revolution is an identity operation.
    uint8_t direction = 0U;
    assert(TryProjectionHorizontalDirection("minecraft:blue_glazed_terracotta", 0U,
                                            &direction) &&
           direction == 0U);
    assert(RotateProjectionHorizontalDirectionAux(
               "minecraft:blue_glazed_terracotta", 0U, 1U, &rotated_aux) &&
           rotated_aux == 1U);
    assert(RotateProjectionHorizontalDirectionAux(
               "minecraft:blue_glazed_terracotta", 1U, 3U, &rotated_aux) &&
           rotated_aux == 0U);
    assert(RotateProjectionHorizontalDirectionAux("minecraft:pumpkin", 2U, 4U,
                                                  &rotated_aux) &&
           rotated_aux == 2U);

    // A repeater keeps its static delay through rotation while the powered
    // graph bit (0x10) is ignored for world equality.  Hotbar matching is
    // deliberately broader because a single repeater item creates every
    // direction/delay state.
    uint8_t repeater_direction = 0U;
    uint8_t repeater_delay = 0U;
    assert(TryProjectionRepeaterState("minecraft:powered_repeater", 29U,
                                      &repeater_direction, &repeater_delay) &&
           repeater_direction == 1U && repeater_delay == 4U);
    assert(RotateProjectionRepeaterAux("minecraft:powered_repeater", 29U, 1U,
                                       &rotated_aux) &&
           rotated_aux == 30U);
    assert(ProjectionBlockIdentityMatches("minecraft:powered_repeater", 29U,
                                          "minecraft:unpowered_repeater", 13U));
    assert(!ProjectionBlockIdentityMatches("minecraft:repeater", 13U,
                                           "minecraft:repeater", 12U));
    assert(!ProjectionBlockIdentityMatches("minecraft:repeater", 13U,
                                           "minecraft:repeater", 9U));
    assert(ProjectionBlockMaterialMatches("minecraft:powered_repeater", 29U,
                                          "minecraft:unpowered_repeater", 0U));

    // Comparator bit 2 is its static subtract-mode and bit 3 is transient
    // power.  Rotation changes only the low facing bits, preserving both in
    // the packet value but ignoring power during exact world comparison.
    uint8_t comparator_direction = 0U;
    bool comparator_subtract = false;
    assert(TryProjectionComparatorState("minecraft:powered_comparator", 13U,
                                        &comparator_direction, &comparator_subtract) &&
           comparator_direction == 1U && comparator_subtract);
    assert(RotateProjectionComparatorAux("minecraft:powered_comparator", 13U, 1U,
                                         &rotated_aux) &&
           rotated_aux == 14U);
    assert(ProjectionBlockIdentityMatches("minecraft:powered_comparator", 13U,
                                          "minecraft:unpowered_comparator", 5U));
    assert(!ProjectionBlockIdentityMatches("minecraft:comparator", 5U,
                                           "minecraft:comparator", 6U));
    assert(!ProjectionBlockIdentityMatches("minecraft:comparator", 5U,
                                           "minecraft:comparator", 1U));
    assert(ProjectionBlockMaterialMatches("minecraft:powered_comparator", 13U,
                                          "minecraft:unpowered_comparator", 0U));

    // Daylight-detector power is world-derived.  Normal and inverted shells
    // are exact-distinct placement states but share one inventory material.
    expectIdentity(NormalizeProjectionBlockIdentity("minecraft:daylight_detector", 15U),
                   "daylight_detector", 0U);
    expectIdentity(NormalizeLiveBlockIdentity("minecraft:daylight_detector_inverted", 7U),
                   "daylight_detector_inverted", 0U);
    assert(IsProjectionDaylightDetector("minecraft:daylight_detector"));
    assert(IsProjectionDaylightDetector("minecraft:daylight_detector_inverted"));
    assert(IsProjectionInvertedDaylightDetector("minecraft:daylight_detector_inverted"));
    assert(!IsProjectionInvertedDaylightDetector("minecraft:daylight_detector"));
    assert(ProjectionBlockIdentityMatches("minecraft:daylight_detector", 0U,
                                          "minecraft:daylight_detector", 15U));
    assert(!ProjectionBlockIdentityMatches("minecraft:daylight_detector", 0U,
                                           "minecraft:daylight_detector_inverted", 0U));
    assert(ProjectionBlockMaterialMatches("minecraft:daylight_detector", 15U,
                                          "minecraft:daylight_detector_inverted", 0U));

    return 0;
}
