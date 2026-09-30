#ifndef INFINITE_TEXTURE_PROJECTION_BLOCK_IDENTITY_H
#define INFINITE_TEXTURE_PROJECTION_BLOCK_IDENTITY_H

#include <cstdint>
#include <string>
#include <string_view>

namespace build_import {

// A compact, namespace-free identity shared by projection records, the live
// hotbar and the native world reader.  Legacy records deliberately retain
// their original name/data pair, so callers must normalize before comparing a
// flattened item such as `spruce_planks` with `planks + aux=1`.
struct ProjectionBlockIdentity {
    std::string name;
    uint16_t aux = 0;
};

// Removes a Minecraft namespace, serialized state suffix and harmless white
// space, then lower-cases a block identifier.  It applies only exact spelling
// aliases; it never performs a fuzzy/sub-string material match.
std::string ProjectionCanonicalBlockName(std::string_view value);

// Converts the projection source representation (which can retain legacy
// command names/data) into one flattened identity. Unknown names and
// auxiliary values intentionally remain exact so a new block type can never
// select a merely similar material by accident.
ProjectionBlockIdentity NormalizeProjectionBlockIdentity(std::string_view name,
                                                          uint16_t aux);

// Converts current inventory/world identifiers.  This is intentionally a
// distinct operation because `stone_slab` is an historical collision: in a
// legacy projection it means smooth stone, while the current flattened item
// identifier means the ordinary stone slab.  Comparisons below always treat
// their left input as a projection record and their right input as live data.
ProjectionBlockIdentity NormalizeLiveBlockIdentity(std::string_view name,
                                                    uint16_t aux);

// Exact projection-to-live identity. Unlike material matching, it preserves
// the physical upper/lower half of a slab and all non-placement state.
bool ProjectionBlockIdentityMatches(std::string_view left_name, uint16_t left_aux,
                                    std::string_view right_name, uint16_t right_aux);

// Inventory material identity.  A single slab item can create either half;
// stairs, axes and face-directed blocks likewise carry a material ItemStack,
// not their final placement state.  This comparison therefore ignores only
// those placement-derived bits after exact material normalization.
bool ProjectionBlockMaterialMatches(std::string_view left_name, uint16_t left_aux,
                                    std::string_view right_name, uint16_t right_aux);

// Equivalent material comparison when the left value is already a canonical
// inventory identity (for example a material-list entry), rather than a raw
// imported projection record.  Both sides are normalized as live identities so
// historical source aliases such as legacy `stone_stairs` are never applied a
// second time to a modern summary name.
bool ProjectionCanonicalMaterialMatchesLive(std::string_view material_name,
                                            uint16_t material_aux,
                                            std::string_view live_name,
                                            uint16_t live_aux);

constexpr uint16_t kProjectionSlabTopAux = UINT16_C(0x0008);
constexpr uint16_t kProjectionSlabDoubleAux = UINT16_C(0x0080);

bool IsProjectionSlabBlock(std::string_view name);
bool IsProjectionDoubleSlabBlock(std::string_view name);

// Decodes the placement half of a source projection record.  The helper
// accepts both legacy bit-3 halves and flattened 0/1 halves.  `is_double` is
// reported for named/encoded double slabs so callers can apply the printer's
// safe two-stage merge policy rather than mistaking it for a single half.
bool TryProjectionSlabPlacement(std::string_view name, uint16_t aux,
                                bool* top, bool* is_double);

enum class ProjectionBlockAxis : uint8_t {
    Y = 0,
    X = 1,
    Z = 2,
};

// Axis helpers cover legacy logs, target legacy pillars and flattened modern
// pillars.  A projection rotation swaps X/Z on odd quarter-turns.
bool TryProjectionBlockAxis(std::string_view name, uint16_t aux,
                            ProjectionBlockAxis* output);
bool ReplaceProjectionBlockAxis(std::string_view name, uint16_t source_aux,
                                ProjectionBlockAxis axis, uint16_t* output);
bool RotateProjectionBlockAxis(ProjectionBlockAxis source, uint8_t rotation_quarters,
                               ProjectionBlockAxis* output) noexcept;
bool RotateProjectionBlockAxisAux(std::string_view name, uint16_t source_aux,
                                  uint8_t rotation_quarters, uint16_t* output);

// This enum deliberately uses Bedrock's ItemUse face numeric layout.
enum class ProjectionBlockFace : uint8_t {
    Down = 0,
    Up = 1,
    North = 2,
    South = 3,
    West = 4,
    East = 5,
};

bool RotateProjectionBlockFace(ProjectionBlockFace source, uint8_t rotation_quarters,
                               ProjectionBlockFace* output) noexcept;

// End rods use a historical north/south-swapped value order while lightning
// rods use ordinary six-way facing.  These helpers hide that distinction from
// the placement policy and preserve non-facing state bits when replacing it.
bool TryProjectionBlockFace(std::string_view name, uint16_t aux,
                            ProjectionBlockFace* output);
bool ReplaceProjectionBlockFace(std::string_view name, uint16_t source_aux,
                                ProjectionBlockFace face, uint16_t* output);
bool RotateProjectionBlockFaceAux(std::string_view name, uint16_t source_aux,
                                  uint8_t rotation_quarters, uint16_t* output);

// A deliberately narrow allow-list of container shells whose contents are
// explicitly ignored by printer mode.  They keep their physical facing but
// do not restore block-actor inventory/NBT.  This does not make them safe
// support blocks; callers must still avoid clicking an existing container.
bool IsProjectionContentlessContainerShell(std::string_view name);

// Barrel/shulker boxes, droppers, dispensers and hoppers use the normal
// Bedrock six-way layout (down=0, up=1, north=2, south=3, west=4, east=5).
// The latter three families can contain transient bit 3 state; it is retained
// while rotating and deliberately ignored by identity comparison.  Observer
// shares this state layout, but is not a contentless container shell.
bool TryProjectionSixWayFacing(std::string_view name, uint16_t aux,
                               ProjectionBlockFace* output);
bool ReplaceProjectionSixWayFacing(std::string_view name, uint16_t source_aux,
                                   ProjectionBlockFace face, uint16_t* output);
bool RotateProjectionSixWayFacingAux(std::string_view name, uint16_t source_aux,
                                     uint8_t rotation_quarters, uint16_t* output);

// Chests and furnace-family shells use the same six-way encoding but only the
// four horizontal states are valid.  Their front is chosen from the opposite
// of the placing player's look direction, so the helper returns the silent
// yaw required to create a requested stored facing without moving the camera.
bool TryProjectionHorizontalSixWayFacing(std::string_view name, uint16_t aux,
                                         ProjectionBlockFace* output);
bool RotateProjectionHorizontalSixWayFacingAux(std::string_view name,
                                               uint16_t source_aux,
                                               uint8_t rotation_quarters,
                                               uint16_t* output);
bool TryProjectionHorizontalSixWaySilentYaw(std::string_view name, uint16_t aux,
                                            uint8_t rotation_quarters,
                                            float* output_yaw);

// Wall-mounted torches and buttons use a historical attachment encoding that
// looks similar to an ItemUse face but is not the same family as end rods or
// lightning rods: 1..4 are east/west/south/north, 5 is floor/up and (buttons
// only) 0 is ceiling/down.  The helpers retain only the physical attachment;
// a button's transient powered bit is deliberately ignored by identity
// comparison.
bool TryProjectionAttachmentFace(std::string_view name, uint16_t aux,
                                 ProjectionBlockFace* output);
bool ReplaceProjectionAttachmentFace(std::string_view name, uint16_t source_aux,
                                     ProjectionBlockFace face, uint16_t* output);
bool RotateProjectionAttachmentFaceAux(std::string_view name, uint16_t source_aux,
                                       uint8_t rotation_quarters, uint16_t* output);

// Levers have the same four wall attachments, plus two floor and two ceiling
// axis variants.  The powered bit is simulation state and is not recreated by
// the printer.  For floor/ceiling levers callers use the returned yaw so the
// original axis is selected without moving the visible camera.
bool TryProjectionLeverAttachmentFace(std::string_view name, uint16_t aux,
                                      ProjectionBlockFace* output);
bool RotateProjectionLeverAux(std::string_view name, uint16_t source_aux,
                              uint8_t rotation_quarters, uint16_t* output);
bool TryProjectionLeverSilentYaw(std::string_view name, uint16_t aux,
                                 uint8_t rotation_quarters, float* output_yaw);

// Horizontal blocks whose final direction is selected by player yaw.  Their
// compact direction order is south=0, west=1, north=2, east=3.  The family
// includes legacy anvils; their damage bits are preserved while a normalized
// identity converts the material into chipped/damaged_anvil when necessary.
bool IsProjectionYawDirectedBlock(std::string_view name);
bool TryProjectionHorizontalDirection(std::string_view name, uint16_t aux,
                                      uint8_t* output);
bool RotateProjectionHorizontalDirection(uint8_t source, uint8_t rotation_quarters,
                                         uint8_t* output) noexcept;
bool RotateProjectionHorizontalDirectionAux(std::string_view name, uint16_t source_aux,
                                             uint8_t rotation_quarters,
                                             uint16_t* output);

// Repeaters and comparators are placed with their horizontal facing, then
// adjusted only through their normal in-world use action.  Powered/locked
// state is supplied by the redstone graph and is intentionally not recreated.
bool TryProjectionRepeaterState(std::string_view name, uint16_t aux,
                                uint8_t* direction, uint8_t* delay);
bool RotateProjectionRepeaterAux(std::string_view name, uint16_t source_aux,
                                 uint8_t rotation_quarters, uint16_t* output);
bool TryProjectionComparatorState(std::string_view name, uint16_t aux,
                                  uint8_t* direction, bool* subtract_mode);
bool RotateProjectionComparatorAux(std::string_view name, uint16_t source_aux,
                                  uint8_t rotation_quarters, uint16_t* output);

// A daylight detector's power is derived from the world.  The normal/inverted
// shell is static and can be switched once through normal block use after a
// confirmed placement; both shells consume the same inventory material.
bool IsProjectionDaylightDetector(std::string_view name);
bool IsProjectionInvertedDaylightDetector(std::string_view name);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_PROJECTION_BLOCK_IDENTITY_H
