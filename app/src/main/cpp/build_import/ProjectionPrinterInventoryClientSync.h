#ifndef INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_CLIENT_SYNC_H
#define INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_CLIENT_SYNC_H

#include "ItemExtraMapMetadata.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace build_import {

// A narrow, protocol-v859 bridge for the printer's local hotbar refresh.  It
// never sends a packet itself: the game-thread integration takes the queued
// bytes and feeds them through a separately verified client receive path.
// Keeping packet construction here independent from that ingress makes it
// possible to validate the exact captured InventoryContent before anything is
// shown to the client.
enum class ProjectionPrinterInventoryClientSyncTicketState : uint8_t {
    Unknown = 0,
    Pending = 1,
    Complete = 2,
    Cancelled = 3,
};

// Decoded from a live ItemStack by the game's own InventorySlot serializer.
// Unlike the passive server mailbox, this does not depend on a previously
// observed InventoryContent packet or on the Python item-component cache.
struct ProjectionPrinterNativeSlotItem {
    bool occupied = false;
    int32_t runtime_item_id = 0;
    uint16_t count = 0;
    uint16_t aux = 0;
    bool has_network_stack_id = false;
    int32_t network_stack_id = 0;
};

bool DecodeProjectionPrinterNativeInventorySlotItem(
    std::string_view packet, uint8_t expected_slot,
    ProjectionPrinterNativeSlotItem* output) noexcept;

// Decode the ItemExtraData emitted by the game's own InventorySlot writer.
// This never consults a packet mailbox or UI text cache. A malformed or
// unsupported NBT payload fails closed, including for an otherwise valid item.
bool DecodeProjectionPrinterNativeInventorySlotMapMetadata(
    std::string_view packet, uint8_t expected_slot,
    ProjectionPrinterNativeSlotItem* item,
    ItemExtraMapMetadata* metadata) noexcept;

// Preview-only decoder for an ItemInstance serialized by the game's own
// InventorySlot writer. An ItemInstance has no ItemStack network-ID variant;
// unlike the live-inventory decoder above, this accepts a missing wire net ID
// but still requires a complete, valid ItemExtra NBT with map_uuid. Never use
// its result as an owned inventory-stack identity or server ACK.
bool DecodeProjectionPrinterNativeInventorySlotPreviewMapMetadata(
    std::string_view packet, uint8_t expected_slot,
    ProjectionPrinterNativeSlotItem* item,
    ItemExtraMapMetadata* metadata) noexcept;

// Parse a complete game-written v859 InventorySlot packet for a specifically
// bound inventory/window and physical slot. Unlike the player-only helpers,
// this can inspect an opened container, but cannot infer or verify which
// physical slot belongs to an anvil: the caller must supply that evidence.
// Offsets refer to the ORIGINAL packet and permit narrow, queue-free edits.
struct ProjectionPrinterNativeMapSlotPacket {
    uint32_t wire_header = 0;
    uint32_t inventory_id = 0;
    uint32_t slot = 0;
    ProjectionPrinterNativeSlotItem item;
    ItemExtraMapMetadata metadata;
    size_t item_data_offset = 0;
    size_t item_data_length = 0;
    size_t network_stack_id_offset = 0;
    size_t network_stack_id_length = 0;
};

bool DecodeProjectionPrinterNativeMapSlotPacket(
    std::string_view packet, uint32_t expected_inventory_id,
    uint32_t expected_slot,
    ProjectionPrinterNativeMapSlotPacket* output) noexcept;

struct ProjectionPrinterInventoryClientSyncSnapshotInfo {
    bool ready = false;
    uint32_t inventory_id = 0;
    uint64_t revision = 0;
};

// The expected fields bind the request to the captured pre-move
// InventoryContent. The post-move fields replace only a captured
// ItemStackNetId zigzag-varint; they may be the old cache IDs for the supplied
// sorter-compatible immediate refresh, or response-authoritative IDs for a
// later correction. No other ItemData byte is changed.
struct ProjectionPrinterInventoryClientSyncMove {
    uint64_t snapshot_revision = 0;
    uint8_t source_inventory_slot = 0;      // ordinary inventory: 9..35
    uint8_t destination_hotbar_slot = 0;    // hotbar: 0..8
    int32_t expected_source_network_stack_id = 0;
    uint16_t expected_source_count = 0;
    bool expected_destination_occupied = false;
    int32_t expected_destination_network_stack_id = 0;
    int32_t confirmed_source_network_stack_id = 0;
    int32_t confirmed_destination_network_stack_id = 0;
};

// A prepared refresh holds the two raw InventorySlot packets built from the
// exact pre-move InventoryContent.  It is deliberately not visible to the
// receive FIFO until Commit... is called.  This lets the printer follow the
// reference inventory manager's ordering precisely: build the source->target
// update from the pre-move cache, submit the native Move/Swap, then immediately
// hand those two already-built updates to the client.  `id == 0` means no
// prepared refresh is held.
struct ProjectionPrinterInventoryClientSyncPreparedMove {
    uint64_t id = 0;
};

// `packet_index` is 0 for the original backpack slot and 1 for the destination
// hotbar slot.  They are deliberately queued in that order to mirror the
// reference inventory manager's source-then-destination update sequence.
struct ProjectionPrinterInventoryClientSyncQueuedPacket {
    uint64_t ticket = 0;
    uint8_t packet_index = 0;
    std::string bytes;
};

// Feed every raw incoming packet to this passive observer.  It records only a
// complete player InventoryContent (network inventory ID 0, exactly 36 slots),
// preserving every raw ItemData byte plus its v859 FullContainerName/storage
// tail.  Login/world reset packets discard state automatically.
void ObserveProjectionPrinterInventoryClientSyncPacket(std::string_view packet) noexcept;

// Drops captured inventory bytes and cancels every queued refresh ticket.
void ClearProjectionPrinterInventoryClientSync() noexcept;

bool GetProjectionPrinterInventoryClientSyncSnapshotInfo(
    ProjectionPrinterInventoryClientSyncSnapshotInfo* output) noexcept;

// Builds exactly two v859 InventorySlot (0x32) packets from the captured raw
// ItemData. It never synthesizes ItemData: it exchanges the original source /
// destination byte sequences and changes only an occupied post-move stack's
// captured ItemStackNetId zigzag-varint to the response-confirmed ID. `ticket`
// is nonzero only after both packets have been queued.
bool QueueProjectionPrinterInventoryClientSyncMove(
    const ProjectionPrinterInventoryClientSyncMove& move,
    uint64_t* ticket,
    std::string* error);

// Pre-builds an optimistic source->destination refresh while the complete
// InventoryContent pre-image is still current.  The packets retain the
// pre-move ItemStackNetIds, exactly like the supplied inventory sorter.  They
// are not injected unless Commit... succeeds after the native request has been
// submitted.  A later incoming InventoryContent does not invalidate this
// prepared pre-image; the caller may still enqueue it immediately after the
// matching Move/Swap call returns.
bool PrepareProjectionPrinterInventoryClientSyncMove(
    const ProjectionPrinterInventoryClientSyncMove& move,
    ProjectionPrinterInventoryClientSyncPreparedMove* prepared,
    std::string* error);

// Accepts the engine-serialized post-move source and destination InventorySlot
// packets. Unlike the captured-content path this needs only the two live
// ItemStacks, not a login InventoryContent baseline. Both packet identities,
// source count and post-move stack IDs are checked before holding the pair.
bool PrepareProjectionPrinterInventoryClientSyncNativeMove(
    const ProjectionPrinterInventoryClientSyncMove& move,
    std::string source_packet, std::string destination_packet,
    ProjectionPrinterInventoryClientSyncPreparedMove* prepared,
    std::string* error);

// Makes a successfully prepared source->destination refresh visible to the
// receive FIFO.  `ticket` is assigned only when both packets were enqueued.
// The prepared handle is consumed whether this succeeds or fails.
bool CommitProjectionPrinterInventoryClientSyncPreparedMove(
    ProjectionPrinterInventoryClientSyncPreparedMove* prepared,
    uint64_t* ticket,
    std::string* error);

// After an exactly matched server response, refresh the same two slots with
// response-assigned net IDs. Only the newest ticket retains its raw pre-image;
// this never requires InventoryContent and never changes other ItemData fields.
// Callers must first verify live slots still equal their immediate move result.
bool QueueProjectionPrinterInventoryClientSyncResponseCorrection(
    uint64_t original_ticket, int32_t confirmed_source_network_id,
    int32_t confirmed_destination_network_id, uint64_t* correction_ticket,
    std::string* error);

// Drops a prepared refresh after a native Move/Swap could not be submitted.
void DiscardProjectionPrinterInventoryClientSyncPreparedMove(
    ProjectionPrinterInventoryClientSyncPreparedMove* prepared) noexcept;

// Bind the exact receive connection which delivered the printer-owned
// ContainerOpen. A different non-null connection invalidates old prepared and
// queued updates before it is bound. Null explicitly clears the binding/state.
// Caller-owned receive scratch arguments must never be used as this identity.
void BindProjectionPrinterInventoryClientSyncIngress(const void* connection) noexcept;

// Removes one packet from the FIFO only for the non-null bound connection.
// Other receive callers must leave this queue untouched. The consumer must call Complete... only
// after it has actually handed this exact packet to the verified client ingress.
bool TakeProjectionPrinterInventoryClientSyncPacket(
    const void* connection,
    ProjectionPrinterInventoryClientSyncQueuedPacket* output);

bool CompleteProjectionPrinterInventoryClientSyncPacket(uint64_t ticket,
                                                         uint8_t packet_index) noexcept;
void CancelProjectionPrinterInventoryClientSyncTicket(uint64_t ticket) noexcept;
ProjectionPrinterInventoryClientSyncTicketState
GetProjectionPrinterInventoryClientSyncTicketState(uint64_t ticket) noexcept;

}  // namespace build_import

#endif  // INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_CLIENT_SYNC_H
