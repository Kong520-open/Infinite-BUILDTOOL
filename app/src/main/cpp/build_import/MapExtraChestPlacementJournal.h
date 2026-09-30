#ifndef INFINITE_TEXTURE_MAP_EXTRA_CHEST_PLACEMENT_JOURNAL_H
#define INFINITE_TEXTURE_MAP_EXTRA_CHEST_PLACEMENT_JOURNAL_H

#include "MapPairPlacementJournal.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace build_import {

constexpr uint64_t kMapChestSlotCount = 27U;
constexpr uint64_t kMaximumMapTileCount = 65536U;

struct MapTileChestAddress {
    uint16_t chest_index = 0;
    uint8_t slot = 0;
};

// Chest zero is the chest beside the tool-placed anvil. Each later chest has
// its own immutable, create-only placement sidecar. No caller may silently
// fall back to a different chest when a recorded site is unavailable.
bool ResolveMapTileChestAddress(uint64_t tile_cursor, uint64_t tile_count,
                                MapTileChestAddress* address) noexcept;

enum class MapExtraChestPlacementPhase : uint32_t {
    Selected = 1,
    DispatchArmed = 2,
    Confirmed = 3,
    // Append-only: phases 1..3 retain the meaning of older sidecars.
    SupportSelected = 4,
    SupportDispatchArmed = 5,
    SupportConfirmed = 6,
};

struct MapExtraChestPlacementRecord {
    std::string world_id;
    int32_t dimension_id = 0;
    uint64_t tile_count = 0;
    BlockBounds artwork_bounds;
    uint16_t chest_index = 0;  // Must be >= 1 and contain at least one tile.
    MapChestPosition chest;
    MapExtraChestPlacementPhase phase = MapExtraChestPlacementPhase::Selected;
    // Persisted in an old reserved byte (zero for legacy sidecars). Retained
    // through Confirmed to distinguish auto-created stone support.
    bool support_created = false;
    // Version-1 sidecar reserved byte; zero in existing journals. Consumed
    // before one support-stone retry or after an explicit rejected ACK.
    bool support_retry_used = false;
    // Zero is the legacy one-cell support; 1..4 identify one fixed 2x2
    // platform quadrant before any support command is armed.
    uint8_t platform_corner = 0;
    // Persisted BEFORE /fill is dispatched. A confirmed support UUID is
    // replaced by a distinct chest UUID when that later command is armed.
    // The final chest UUID is retained to detect duplicates in the chain.
    std::string command_uuid;
};

enum class MapExtraChestPlacementLoad : uint8_t { Missing, Loaded, Unsafe };
enum class MapExtraChestPlacementResume : uint8_t {
    Unsafe,
    SurveySelectedSite,
    AmbiguousDispatchNoResend,
    VerifyConfirmedChest,
    SurveySelectedSupport,
    AmbiguousSupportDispatch,
    VerifySupportBeforeChest,
};

// A caller must obtain each readback by querying the exact position on the
// current world's game thread. Passing a stale cache is not valid evidence.
struct MapExtraChestReadback {
    MapChestPosition position;
    bool available = false;
    NativeBlockInfo block;
};

std::string MapExtraChestPlacementJournalPath(
    const std::string& map_state_path, uint16_t chest_index);

bool BeginMapExtraChestPlacementJournal(
    const std::string& map_state_path,
    const MapExtraChestPlacementRecord& selected,
    std::string* error = nullptr);
MapExtraChestPlacementLoad LoadMapExtraChestPlacementJournal(
    const std::string& map_state_path, uint16_t chest_index,
    MapExtraChestPlacementRecord* output, std::string* error = nullptr);

// New auto-placement path. Persist the ambiguity marker BEFORE the support
// /fill. Once armed, a crash or transport failure may not be resent blindly.
bool ArmMapExtraChestSupportDispatch(
    const std::string& map_state_path,
    const MapExtraChestPlacementRecord& expected_selected,
    const std::string& unique_command_uuid, std::string* error = nullptr);

bool UpgradeMapExtraChestSupportSelectedPlatform(
    const std::string& map_state_path,
    const MapExtraChestPlacementRecord& expected_selected,
    uint8_t platform_corner, std::string* error = nullptr);

bool RearmMapExtraChestSupportDispatchOnce(
    const std::string& map_state_path,
    const MapExtraChestPlacementRecord& expected_armed,
    const std::string& replacement_command_uuid,
    const MapExtraChestReadback& support_readback,
    std::string* error = nullptr,
    const std::array<MapExtraChestReadback, 3>* pad_readbacks = nullptr);

bool BlockMapExtraChestSupportRetry(
    const std::string& map_state_path,
    const MapExtraChestPlacementRecord& expected_armed,
    std::string* error = nullptr);

// The exact support cell below the selected chest must be freshly read as
// minecraft:stone after a matching accepted tracked RPC ACK.
bool ConfirmMapExtraChestSupport(
    const std::string& map_state_path,
    const MapExtraChestPlacementRecord& expected_armed,
    const std::string& ack_command_uuid, MapPairCommandAck ack,
    const MapExtraChestReadback& support_readback,
    std::string* error = nullptr,
    const std::array<MapExtraChestReadback, 3>* pad_readbacks = nullptr);

// No-ACK recovery is restricted to an already-Armed support /fill and an
// exact fresh native stone readback at the immutable journaled coordinate.
bool ConfirmMapExtraChestSupportByNativeReadback(
    const std::string& map_state_path,
    const MapExtraChestPlacementRecord& expected_armed,
    const MapExtraChestReadback& support_readback,
    std::string* error = nullptr,
    const std::array<MapExtraChestReadback, 3>* pad_readbacks = nullptr);

// Call synchronously before sending the chest command. Old Selected sidecars
// may arm directly; new SupportConfirmed sidecars require a different UUID.
// Once armed, a crash or transport failure is ambiguous and no re-send occurs.
bool ArmMapExtraChestPlacementDispatch(
    const std::string& map_state_path,
    const MapExtraChestPlacementRecord& expected_selected,
    const std::string& unique_command_uuid, std::string* error = nullptr);

// Requires the exact armed UUID, accepted tracked RPC ACK, and a fresh native
// read of minecraft:chest at the recorded coordinates. Never sends a command.
bool ConfirmMapExtraChestPlacement(
    const std::string& map_state_path,
    const MapExtraChestPlacementRecord& expected_armed,
    const std::string& ack_command_uuid, MapPairCommandAck ack,
    const MapExtraChestReadback& chest_readback,
    std::string* error = nullptr);

bool ConfirmMapExtraChestPlacementByNativeReadback(
    const std::string& map_state_path,
    const MapExtraChestPlacementRecord& expected_armed,
    const MapExtraChestReadback& chest_readback,
    std::string* error = nullptr);

MapExtraChestPlacementResume ClassifyMapExtraChestPlacementResume(
    const MapExtraChestPlacementRecord& record, const std::string& world_id,
    int32_t dimension_id, uint64_t tile_count,
    const BlockBounds& artwork_bounds, uint16_t chest_index) noexcept;

// Before selecting or USING target chest N, load the confirmed pair and
// every extra sidecar 1..N-1. Read every previous chest from the live native
// world and supply ordered evidence: pair chest first, then extras in index
// order. This rejects gaps, stale worlds, missing/wrong blocks, duplicates,
// and face-adjacent chest coordinates (including the proposed target).
bool ValidateMapExtraChestPlanChain(
    const MapPairPlacementRecord& confirmed_pair,
    const std::vector<MapExtraChestPlacementRecord>& prior_extras,
    const std::vector<MapExtraChestReadback>& ordered_readbacks,
    const MapExtraChestPlacementRecord& target,
    std::string* error = nullptr);

// Only after the final map cursor is durably committed. World chests remain.
// For multiple sidecars, the orchestrator must delete in DESCENDING chest
// index order. After a crash during cleanup, a missing suffix is acceptable
// only when the final cursor is durable; a missing interior/prefix journal
// during an unfinished map job is unsafe and must not trigger reselection.
bool ClearCompletedMapExtraChestPlacementJournal(
    const std::string& map_state_path,
    const MapExtraChestPlacementRecord& expected_confirmed,
    uint64_t checkpoint_tile_cursor, std::string* error = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_EXTRA_CHEST_PLACEMENT_JOURNAL_H
