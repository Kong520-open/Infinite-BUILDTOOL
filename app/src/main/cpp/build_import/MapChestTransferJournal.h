#ifndef INFINITE_TEXTURE_MAP_CHEST_TRANSFER_JOURNAL_H
#define INFINITE_TEXTURE_MAP_CHEST_TRANSFER_JOURNAL_H

#include "ContainerCaptureMailbox.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace build_import {

// Each tool-placed chest holds at most 27 map tiles. This sidecar lives
// beside map_creation.state and protects ONE non-idempotent ItemStackRequest.
// It is deliberately independent of BuildImportRuntime and sends no packets.
enum class MapChestTransferPhase : uint32_t {
    Prepared = 1,        // No send is allowed yet; recheck source/chest on resume.
    DispatchArmed = 2,   // Persisted BEFORE send; send may have happened.
    ResponseAccepted = 3,
    ReopenConfirmed = 4, // Matching UUID and exact title observed in a NEW chest capture.
    InventoryConfirmed = 5, // Accepted Place and a complete stable native player-inventory absence scan.
    AcceptedAndClosed = 6, // Accepted Place and original chest window settled/closed.
};

// The first-window close/settlement signal must refer to the same transfer as
// the durably recorded server-accepted Place response. On a live run this
// means the exact original chest window closed. After process restart it may
// mean the old window cannot survive and the resumed process has no open
// chest window. The full transfer identity is checked against the journal.
// This proof does not require a second chest opening or an inventory scan.
struct MapChestAcceptedCloseProof {
    int32_t accepted_request_id = 0;
    bool original_chest_window_settled = false;
};

// Supplied only after a complete native scan of all 36 ordinary player slots.
// The producer must fail closed if even one slot or its map UUID cannot be
// read, or if the inventory changes across the two scans. Absence alone is
// never proof that the server accepted the chest transfer.
struct MapChestInventoryAbsenceProof {
    int32_t accepted_request_id = 0;
    int64_t absent_map_uuid = -1;
    bool complete_native_inventory_scan = false;
    bool expected_uuid_absent = false;
};

struct MapChestTransferRecord {
    std::string world_id;
    int32_t dimension_id = 0;
    uint64_t tile_cursor = 0;
    uint64_t tile_count = 0;
    int32_t chest_x = 0;
    int32_t chest_y = 0;
    int32_t chest_z = 0;
    // Deterministic multi-chest layout: tile_cursor / 27 selects the chest,
    // tile_cursor % 27 selects its slot. The corresponding chest position
    // must be checked against a separately persisted placement plan.
    uint16_t chest_index = 0;
    uint8_t chest_slot = 0;
    int32_t source_runtime_item_id = 0;
    int32_t source_network_stack_id = 0;
    int64_t source_map_uuid = -1;
    // Token of the empty-chest capture used by the send preflight. Retained
    // to bind the exact Place response to its original transfer window.
    uint64_t pre_send_capture_token = 0;
    int32_t request_id = 0;
    MapChestTransferPhase phase = MapChestTransferPhase::Prepared;
};

enum class MapChestJournalLoad : uint8_t {
    Missing = 0,
    Loaded = 1,
    Unsafe = 2, // Corrupt, leftover temporary file, or inaccessible.
};

enum class MapChestRecoveryAction : uint8_t {
    Unsafe = 0,
    FreshPreflightRequired = 1, // Prepared: source/chest must be read again.
    ReopenChestNoResend = 2,     // Legacy name: reconcile ACK/inventory, never resend.
    CommitTileCursorNoResend = 3,
    ClearJournalAfterCursorCommit = 4,
};

std::string MapChestTransferJournalPath(const std::string& map_state_path);

// Create only when no prior sidecar (including .tmp) exists. A map-use
// sidecar must remain until the map is stored and its tile cursor committed.
bool BeginMapChestTransferJournal(const std::string& map_state_path,
                                  const MapChestTransferRecord& prepared,
                                  std::string* error = nullptr);

MapChestJournalLoad LoadMapChestTransferJournal(
    const std::string& map_state_path, MapChestTransferRecord* output,
    std::string* error = nullptr);

// Safe only while phase=Prepared. Call after a fresh, live read proves the
// SAME map UUID is still in the source and the target chest slot is empty.
// This changes only the source net ID and capture token, never the target.
bool RefreshPreparedMapChestTransferJournal(
    const std::string& map_state_path,
    const MapChestTransferRecord& expected_prepared,
    int32_t fresh_source_network_stack_id, uint64_t fresh_empty_capture_token,
    std::string* error = nullptr);

// MUST be called synchronously from the sender's pre_send callback, after
// request_id is reserved but BEFORE the underlying sendToServer call. If this
// returns false the caller MUST NOT send. Once it succeeds, a crash or failed
// send is ambiguous and automatic resend is forever forbidden for this tile.
bool ArmMapChestTransferDispatch(const std::string& map_state_path,
                                 const MapChestTransferRecord& expected_prepared,
                                 int32_t request_id, std::string* error = nullptr);

bool NoteMapChestTransferAccepted(const std::string& map_state_path,
                                  const MapChestTransferRecord& expected_armed,
                                  int32_t request_id, std::string* error = nullptr);

// An ACK is useful but not sufficient. A new 27-slot single-chest capture
// must contain the original UUID AND the exact row/column title in the exact
// slot. `expected_name_source` must have been established from a live manual
// rename on this game build; neither NBT candidate is trusted by default.
// This also permits recovery when the ACK was lost, without resending.
bool ConfirmMapChestTransferReopen(const std::string& map_state_path,
                                   const MapChestTransferRecord& expected,
                                   const ContainerCaptureResult& reopen,
                                   std::string_view expected_name,
                                   MapItemNameSource expected_name_source,
                                   std::string* error = nullptr);

// New no-reopen confirmation path. The accepted server response must first
// have been durably recorded with NoteMapChestTransferAccepted. A caller's
// complete, stable native player-inventory scan must then find no map with
// this transfer's UUID. This never authorizes a second Place.
bool ConfirmMapChestTransferInventoryAbsent(
    const std::string& map_state_path,
    const MapChestTransferRecord& expected_accepted,
    const MapChestInventoryAbsenceProof& proof,
    std::string* error = nullptr);

// Record completion only after the exact Place response was durably accepted
// and the original chest window has settled/closed. A missing settlement
// signal must remain pending; neither reopening nor a second Place is allowed.
bool ConfirmMapChestTransferAcceptedClose(
    const std::string& map_state_path,
    const MapChestTransferRecord& expected_accepted,
    const MapChestAcceptedCloseProof& proof,
    std::string* error = nullptr);

// Compare the saved journal to the CURRENT checkpoint and world context.
// No result authorizes resending an already-armed request.
MapChestRecoveryAction ClassifyMapChestTransferRecovery(
    const MapChestTransferRecord& journal, const std::string& world_id,
    int32_t dimension_id, uint64_t checkpoint_tile_cursor,
    uint64_t expected_tile_count, int32_t chest_x, int32_t chest_y,
    int32_t chest_z, uint8_t expected_slot) noexcept;

// Remove only after the separate map_creation.state cursor was DURABLY
// advanced by exactly one. The path remains untouched on a failed check.
bool ClearCommittedMapChestTransferJournal(
    const std::string& map_state_path,
    const MapChestTransferRecord& expected_confirmed,
    uint64_t checkpoint_tile_cursor, std::string* error = nullptr);

} // namespace build_import

#endif // INFINITE_TEXTURE_MAP_CHEST_TRANSFER_JOURNAL_H
