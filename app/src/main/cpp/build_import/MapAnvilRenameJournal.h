#ifndef INFINITE_TEXTURE_MAP_ANVIL_RENAME_JOURNAL_H
#define INFINITE_TEXTURE_MAP_ANVIL_RENAME_JOURNAL_H

#include "ItemExtraMapMetadata.h"
#include "MapChestTransferJournal.h"

#include <cstdint>
#include <string>

namespace build_import {

// One map, two non-idempotent ItemStackRequests. This sidecar records intent
// only; it never opens a window or sends a packet. An Armed phase is ambiguous
// after a crash, even if the corresponding ACK was not observed.
enum class MapAnvilRenamePhase : uint32_t {
    Prepared = 1,
    InputDispatchArmed = 2,
    InputResponseAccepted = 3,
    InputConfirmed = 4,
    CraftDispatchArmed = 5,
    CraftResponseAccepted = 6,
    RenamedMapConfirmed = 7,
    // Original result click may enqueue its packet after the native handler
    // returns. Durable before the click, so recovery cannot click twice.
    CraftClickArmed = 8,
};

// Captured and fsynced before the matching non-idempotent request is sent.
// A request ID alone is not enough: it can be reused after a client restart,
// while an old mailbox response may still be visible. The window token is a
// same-process lease; after restart it deliberately cannot authorize replay.
struct MapAnvilResponseCorrelation {
    uint64_t session_generation = 0;
    uint64_t response_generation_before_send = 0;
    uint64_t window_token = 0;
    uint8_t window_id = 0;
    int32_t source_hotbar_slot = -1;  // input Place only; craft uses -1.
};

struct MapAnvilRenameRecord {
    std::string world_id;
    int32_t dimension_id = 0;
    uint64_t tile_cursor = 0;
    uint64_t tile_count = 0;
    uint32_t columns = 0;
    uint32_t rows = 0;
    int32_t anvil_x = 0;
    int32_t anvil_y = 0;
    int32_t anvil_z = 0;
    int32_t map_runtime_item_id = 0;
    int64_t map_uuid = -1;
    // Pre-send hotbar identity only. The server may assign the anvil input a
    // different positive network stack ID after Place.
    int32_t input_source_network_stack_id = 0;
    std::string expected_title;
    int32_t input_request_id = 0;
    int32_t craft_request_id = 0;
    // Original native input identity captured before the result click.
    int32_t craft_input_network_stack_id = 0;
    // Exact destination from the game's resulting ItemStackRequest; -1 until
    // the craft packet is journaled. Never assume hotbar slot zero.
    int32_t craft_destination_hotbar_slot = -1;
    // Exact output net ID returned by the accepted craft response. This is
    // persisted before native readback and must match the native item.
    int32_t craft_accepted_output_network_stack_id = 0;
    int32_t renamed_network_stack_id = 0;
    MapAnvilResponseCorrelation input_response;
    MapAnvilResponseCorrelation craft_response;
    MapAnvilRenamePhase phase = MapAnvilRenamePhase::Prepared;
};

enum class MapAnvilRenameLoad : uint8_t { Missing, Loaded, Unsafe };
enum class MapAnvilRenameRecovery : uint8_t {
    Unsafe,
    FreshInputPreflight,
    ReconcileInputNoResend,
    FreshCraftPreflight,
    ReconcileCraftClickNoResend,
    ReconcileCraftNoResend,
    ProceedToChestStorage,
    ClearAfterStorageCommit,
};

// The production caller must obtain each proof from the current world on the
// game thread and correlate it with the current server window. These fields
// are deliberately insufficient to initiate a packet by themselves.
struct MapAnvilInputProof {
    std::string world_id;
    int32_t dimension_id = 0;
    int32_t anvil_x = 0;
    int32_t anvil_y = 0;
    int32_t anvil_z = 0;
    uint64_t fresh_window_token = 0;
    uint8_t window_id = 0;
    uint8_t input_slot = 1;
    uint16_t count = 0;
    int32_t runtime_item_id = 0;
    // Fresh destination-slot net ID; it need not equal the hotbar source ID.
    int32_t network_stack_id = 0;
    int64_t map_uuid = -1;
    // Current packet-only hidden anvil capture does not provide this after
    // Place. A matching ItemStackResponse is useful but is NOT a native
    // readback; callers must not set this merely to advance the journal.
    // Closing/reopening the anvil can return its input map to the player.
    bool native_readback = false;
};

struct MapAnvilOutputProof {
    std::string world_id;
    int32_t dimension_id = 0;
    uint16_t count = 0;
    int32_t runtime_item_id = 0;
    int32_t network_stack_id = 0;
    int64_t map_uuid = -1;
    MapItemNameSource name_source = MapItemNameSource::None;
    std::string name;
    bool native_readback = false;
};

std::string MapAnvilRenameJournalPath(const std::string& map_state_path);
bool BeginMapAnvilRenameJournal(const std::string& map_state_path,
                                const MapAnvilRenameRecord& prepared,
                                std::string* error = nullptr);
MapAnvilRenameLoad LoadMapAnvilRenameJournal(
    const std::string& map_state_path, MapAnvilRenameRecord* output,
    std::string* error = nullptr);

// Call each Arm synchronously after reserving the native negative odd request
// ID and BEFORE sendToServer. Failure forbids the send. Never retry an Armed
// request automatically. Both the exact accepted server response and fresh
// native evidence are required before confirming either anvil mutation.
bool ArmMapAnvilInputDispatch(const std::string& map_state_path,
                               const MapAnvilRenameRecord& expected_prepared,
                               int32_t request_id,
                               const MapAnvilResponseCorrelation& correlation,
                               std::string* error = nullptr);
bool NoteMapAnvilInputAccepted(const std::string& map_state_path,
                               const MapAnvilRenameRecord& expected_armed,
                               int32_t request_id, std::string* error = nullptr);
bool ConfirmMapAnvilInput(const std::string& map_state_path,
                          const MapAnvilRenameRecord& expected,
                          const MapAnvilInputProof& proof,
                          std::string* error = nullptr);
// Must be fsynced before calling the original native result-click handler.
// A ClickArmed state forbids a second click even if no packet is observed.
bool ArmMapAnvilCraftClick(const std::string& map_state_path,
                            const MapAnvilRenameRecord& expected_input_confirmed,
                            const MapAnvilInputProof& pre_click_input,
                            std::string* error = nullptr);
bool ArmMapAnvilCraftDispatch(const std::string& map_state_path,
                               const MapAnvilRenameRecord& expected_click_armed,
                               const MapAnvilInputProof& pre_click_input,
                               int32_t request_id,
                               const MapAnvilResponseCorrelation& correlation,
                               uint8_t destination_hotbar_slot,
                               std::string* error = nullptr);
bool NoteMapAnvilCraftAccepted(const std::string& map_state_path,
                               const MapAnvilRenameRecord& expected_armed,
                               int32_t request_id,
                               int32_t accepted_output_network_stack_id,
                               std::string* error = nullptr);
bool ConfirmMapAnvilRenamedOutput(const std::string& map_state_path,
                                  const MapAnvilRenameRecord& expected,
                                  const MapAnvilOutputProof& proof,
                                  std::string* error = nullptr);

MapAnvilRenameRecovery ClassifyMapAnvilRenameRecovery(
    const MapAnvilRenameRecord& journal, const std::string& world_id,
    int32_t dimension_id, uint64_t checkpoint_tile_cursor,
    uint64_t tile_count) noexcept;

// Only after the separate chest journal reached legacy ReopenConfirmed,
// InventoryConfirmed, or AcceptedAndClosed (durably accepted Place and the
// original chest window settled or closed), and the map cursor was durably
// advanced. This does not clear the chest journal.
bool ClearCommittedMapAnvilRenameJournal(
    const std::string& map_state_path,
    const MapAnvilRenameRecord& expected_confirmed,
    const MapChestTransferRecord& confirmed_chest,
    uint64_t checkpoint_tile_cursor,
    std::string* error = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_ANVIL_RENAME_JOURNAL_H
