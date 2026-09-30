#ifndef INFINITE_TEXTURE_MAP_PAIR_PLACEMENT_JOURNAL_H
#define INFINITE_TEXTURE_MAP_PAIR_PLACEMENT_JOURNAL_H

#include "MapChestPlacement.h"
#include "MapChestWorldPlacement.h"

#include <cstdint>
#include <string>

namespace build_import {

// This file belongs to one automatic-map job and records its initial
// chest/anvil pair; tile_count is the whole job size (up to 65,536), not the
// capacity of that one chest. Later chests need their own placement plan.
// A selected pair is never reselected after a restart: a dispatched but
// unacknowledged fill/setblock is ambiguous and must not be silently resent.
// Values 1..5 belong to older journals and retain their original meaning.
enum class MapPairPlacementPhase : uint32_t {
    Selected = 1,
    ChestDispatchArmed = 2,
    ChestConfirmed = 3,
    AnvilDispatchArmed = 4,
    PairConfirmed = 5,
    SupportDispatchArmed = 6,
    SupportConfirmed = 7,
    SupportSelected = 8,
};

struct MapPairPlacementRecord {
    std::string world_id;
    int32_t dimension_id = 0;
    uint64_t tile_count = 0;
    BlockBounds artwork_bounds;
    MapChestAnvilPosition pair;
    MapPairPlacementPhase phase = MapPairPlacementPhase::Selected;
    // Old sidecars encode zero in a reserved byte. New auto-placement keeps
    // this marker through PairConfirmed so support stone can be reverified.
    bool support_created = false;
    // One idempotent support-stone retry may be consumed after a missing ACK.
    // The marker also blocks retry after an explicit rejection. Stored in a
    // formerly reserved byte of the version-2 sidecar (old journals read 0).
    bool support_retry_used = false;
    // Zero preserves the legacy two-cell support command. New plans persist
    // side 1/2 before arming the four-cell 2x2 support platform.
    uint8_t platform_side = 0;
    // Present only while one /fill or /setblock is armed. It is persisted before send
    // so a late ACK can never be mistaken for the other placement command.
    std::string active_command_uuid;
};

enum class MapPairPlacementLoad : uint8_t { Missing, Loaded, Unsafe };
enum class MapPairPlacementResume : uint8_t {
    Unsafe,
    SurveySelectedPair,
    AmbiguousChestDispatch,
    VerifyChestBeforeAnvil,
    AmbiguousAnvilDispatch,
    VerifyPairBeforeStorage,
    SurveySelectedSupport,
    AmbiguousSupportDispatch,
    VerifySupportBeforeChest,
};

std::string MapPairPlacementJournalPath(const std::string& map_state_path);

// Create-only; a journal or leftover temporary file must never be replaced.
bool BeginMapPairPlacementJournal(const std::string& map_state_path,
                                  const MapPairPlacementRecord& selected,
                                  std::string* error = nullptr);
MapPairPlacementLoad LoadMapPairPlacementJournal(
    const std::string& map_state_path, MapPairPlacementRecord* output,
    std::string* error = nullptr);

// Persist the ambiguity marker synchronously BEFORE dispatching a command.
// New auto pair: SupportSelected -> SupportDispatchArmed -> SupportConfirmed
// -> ChestDispatchArmed -> ChestConfirmed -> AnvilDispatchArmed -> PairConfirmed.
// Older Selected journals retain their original direct chest path.
bool ArmMapPairPlacementDispatch(const std::string& map_state_path,
                                 const MapPairPlacementRecord& expected,
                                 MapPairPlacementStep step,
                                 const std::string& command_uuid,
                                 std::string* error = nullptr);

// Only an unarmed legacy SupportSelected record may opt into a platform.
// An already-Armed UUID always retains its original two-cell geometry.
bool UpgradeMapPairSupportSelectedPlatform(
    const std::string& map_state_path,
    const MapPairPlacementRecord& expected_selected,
    uint8_t platform_side, std::string* error = nullptr);

// This exception applies ONLY to the two stone floor cells while support is
// Armed. Both must be freshly read as air in the original live world after a
// confirmed near-site settle. Persist the consumed retry and replacement UUID
// atomically before dispatch. Never call this for chest/anvil commands.
bool RearmMapPairSupportDispatchOnce(
    const std::string& map_state_path,
    const MapPairPlacementRecord& expected_armed,
    const std::string& replacement_command_uuid,
    const NativeBlockInfo* chest_floor,
    const NativeBlockInfo* anvil_floor,
    std::string* error = nullptr,
    const NativeBlockInfo* pad_first = nullptr,
    const NativeBlockInfo* pad_second = nullptr);

// An explicit rejected/mismatched receipt permanently consumes retry rights
// for this Armed support command, including across a process restart.
bool BlockMapPairSupportRetry(
    const std::string& map_state_path,
    const MapPairPlacementRecord& expected_armed,
    std::string* error = nullptr);

// The ACK UUID must come from the actual RPC result, never copied from the
// request merely to pass this check. Accepted RPC ACK and native block
// readback are both mandatory. For Support, chest/anvil arguments are the
// floor cells directly below the planned chest/anvil. This method does not
// send commands or infer ownership from a block found after a crash.
bool ConfirmMapPairPlacementStep(const std::string& map_state_path,
                                 const MapPairPlacementRecord& expected_armed,
                                 MapPairPlacementStep step,
                                 const std::string& ack_command_uuid,
                                 MapPairCommandAck ack,
                                 const NativeBlockInfo* chest,
                                 const NativeBlockInfo* anvil,
                                 std::string* error = nullptr,
                                 const NativeBlockInfo* pad_first = nullptr,
                                 const NativeBlockInfo* pad_second = nullptr);

// A dispatched /fill may have no readable RPC receipt on this game build.
// This separate recovery path confirms only the exact desired block state
// from a fresh same-world native readback of an already-Armed journal. It
// never treats a missing receipt as an accepted ACK or resends the command.
bool ConfirmMapPairPlacementStepByNativeReadback(
    const std::string& map_state_path,
    const MapPairPlacementRecord& expected_armed,
    MapPairPlacementStep step,
    const NativeBlockInfo* chest,
    const NativeBlockInfo* anvil,
    std::string* error = nullptr,
    const NativeBlockInfo* pad_first = nullptr,
    const NativeBlockInfo* pad_second = nullptr);

MapPairPlacementResume ClassifyMapPairPlacementResume(
    const MapPairPlacementRecord& record, const std::string& world_id,
    int32_t dimension_id, uint64_t tile_count,
    const BlockBounds& artwork_bounds) noexcept;

// Only after the final map cursor is durably committed. The world blocks are
// deliberately retained; this removes only the job's sidecar.
bool ClearCompletedMapPairPlacementJournal(
    const std::string& map_state_path,
    const MapPairPlacementRecord& expected_confirmed,
    uint64_t checkpoint_tile_cursor, std::string* error = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_PAIR_PLACEMENT_JOURNAL_H
