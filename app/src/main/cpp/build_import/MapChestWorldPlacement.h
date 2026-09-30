#ifndef INFINITE_TEXTURE_MAP_CHEST_WORLD_PLACEMENT_H
#define INFINITE_TEXTURE_MAP_CHEST_WORLD_PLACEMENT_H

#include "MapChestNativeSurvey.h"

#include <array>
#include <string>

namespace build_import {

// Preserve the numeric values of Chest and Anvil for existing saved journals.
enum class MapPairPlacementStep : uint8_t { Chest, Anvil, Support };
enum class MapPairPlacementPreparation : uint8_t {
    Ready,
    InvalidPlan,
    Unavailable,
    Unsafe,
    ChestAckRequired,
    ChestNotConfirmed,
};
enum class MapPairCommandAck : uint8_t { Pending, Accepted, Rejected };
enum class MapPairPlacementVerification : uint8_t {
    PendingAck,
    Rejected,
    PendingNativeReadback,
    ConflictingWorldBlock,
    Confirmed,
};

struct MapPairPlacementEvidence {
    MapChestCandidateSurvey chest;
    MapChestCandidateSurvey anvil;
    // Only relevant to the anvil step. NativeWorldAccess returns namespaced
    // identifiers, and only the exact chest placed by this plan is accepted.
    std::string chest_identifier;
    bool chest_ack_accepted = false;
    // New auto-created pairs must retain both exact stone floor cells after
    // their support fill. Legacy saved pairs may stand on another known solid.
    bool require_exact_stone_floor = false;
    std::string chest_floor_identifier;
    std::string anvil_floor_identifier;
};

struct MapPairPlacementCommand {
    MapPairPlacementStep step = MapPairPlacementStep::Chest;
    MapChestPosition position;
    std::string text;
    // The external scheduler must use a tracked ACK and native readback
    // before trusting any world mutation.
    bool requires_live_syntax_validation = true;
};

// Pure validation/command construction. Does not mutate *command on failure.
// Surveys must be obtained immediately before dispatch. The Support step
// requires air below both blocks and fills those adjacent cells with stone;
// Chest and Anvil require the resulting solid floor. The commands use /fill
// without a replace mode, so a fresh native preflight is mandatory, but a
// server-side race remains possible until a no-overwrite mode is validated.
MapPairPlacementPreparation PrepareMapPairPlacementCommand(
    const BlockBounds& artwork_bounds, const MapChestAnvilPosition& pair,
    MapPairPlacementStep step, const MapPairPlacementEvidence& evidence,
    MapPairPlacementCommand* command);

// Read-only game-thread adapter. A NativeWorldReader opened in the correct
// dimension may be reused across multiple candidates. It never sends the
// returned command. The caller owns RPC UUID assignment and durable ACK state.
MapPairPlacementPreparation PrepareMapPairPlacementCommandWithReader(
    const BlockBounds& artwork_bounds, const MapChestAnvilPosition& pair,
    MapPairPlacementStep step, bool chest_ack_accepted,
    NativeWorldReader* reader, MapPairPlacementCommand* command,
    bool require_exact_stone_floor = false);

// Pure ACK + block-identity validation. For the anvil step, both blocks must
// remain present. A nullptr means the native read failed; air after an ACK is
// treated as not-yet-synchronized, never as confirmation.
MapPairPlacementVerification VerifyMapPairPlacementAfterAck(
    MapPairPlacementStep step, MapPairCommandAck ack,
    const NativeBlockInfo* chest, const NativeBlockInfo* anvil);
MapPairPlacementVerification VerifyMapPairPlacementAfterAckWithReader(
    const MapChestAnvilPosition& pair, MapPairPlacementStep step,
    MapPairCommandAck ack, NativeWorldReader* reader);

// Used by the extra-chest pipeline, which performs its own fresh native
// survey and tracked ACK/readback. No arbitrary block name is accepted.
std::string BuildSingleCellMapChestFillCommand(
    const MapChestPosition& position);

// Durable platform markers: zero is the old two-cell pair / one-cell extra
// support command. Pair sides 1/2 extend perpendicular to the pair; extra
// corners 1..4 extend by one block in each horizontal axis. A platform has
// exactly four stone cells. The standing-cell helpers return the stone floor,
// so a player teleport targets floor.y + 1 after all four cells are confirmed.
bool BuildMapPairSupportPlatformCells(
    const BlockBounds& artwork_bounds, const MapChestAnvilPosition& pair,
    uint8_t platform_side, std::array<MapChestPosition, 4>* cells);
bool BuildMapExtraSupportPlatformCells(
    const BlockBounds& artwork_bounds, const MapChestPosition& chest,
    uint8_t platform_corner, std::array<MapChestPosition, 4>* cells);
bool MapPairPlatformStandingCell(
    const BlockBounds& artwork_bounds, const MapChestAnvilPosition& pair,
    uint8_t platform_side, MapChestPosition* floor);
bool MapExtraPlatformStandingCell(
    const BlockBounds& artwork_bounds, const MapChestPosition& chest,
    uint8_t platform_corner, MapChestPosition* floor);
bool SelectMapPairSupportPlatformSideWithReader(
    const BlockBounds& artwork_bounds, const MapChestAnvilPosition& pair,
    NativeWorldReader* reader, uint8_t* platform_side);
bool SelectMapExtraSupportPlatformCornerWithReader(
    const BlockBounds& artwork_bounds, const MapChestPosition& chest,
    NativeWorldReader* reader, uint8_t* platform_corner);
bool FreshMapSupportPlatformAirWithReader(
    NativeWorldReader* reader, const std::array<MapChestPosition, 4>& cells);
bool ExactMapSupportPlatformStoneWithReader(
    NativeWorldReader* reader, const std::array<MapChestPosition, 4>& cells);
std::string BuildMapSupportPlatformFillCommand(
    const std::array<MapChestPosition, 4>& cells);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_CHEST_WORLD_PLACEMENT_H
