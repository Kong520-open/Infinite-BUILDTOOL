#ifndef INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_MAILBOX_H
#define INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_MAILBOX_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace build_import {

// The printer only ever moves items from the player's ordinary inventory.  Do
// not reuse ContainerCaptureMailbox for this: that mailbox deliberately owns
// and suppresses packets belonging to an automatic container export.
constexpr uint32_t kProjectionPrinterPlayerInventorySlotCount = 36U;
// A response is correlated by its signed ItemStackRequest ID. Keep a bounded
// recent history rather than merging an unrelated successful entry with a
// later rejected entry from the same or another response packet.
constexpr uint32_t kProjectionPrinterInventoryResponseHistoryCapacity = 16U;

struct ProjectionPrinterInventorySlot {
    uint8_t slot = 0;
    bool occupied = false;
    int32_t runtime_item_id = 0;
    bool has_network_stack_id = false;
    int32_t network_stack_id = 0;
    uint16_t count = 0;
    uint16_t aux = 0;
    // Resolved lazily from ItemRuntimeRegistry. It is empty while the current
    // session has not received a complete registry or for an unknown item ID.
    std::string name;
};

struct ProjectionPrinterInventorySnapshot {
    // A true value means this came from one complete player InventoryContent
    // packet. InventorySlot packets by themselves never establish a snapshot.
    bool ready = false;
    uint32_t inventory_id = 0;
    uint64_t session_generation = 0;
    uint64_t revision = 0;
    std::array<ProjectionPrinterInventorySlot,
               kProjectionPrinterPlayerInventorySlotCount> slots{};
};

// ItemStackResponse uses request-container enums, which are intentionally
// retained verbatim. They must not be assumed to be the same numeric IDs as
// InventoryContent's network inventory ID. A sender that creates an
// ItemStackRequest can match this response to its own request ID and container
// plan before using it.
struct ProjectionPrinterInventoryResponseSlot {
    uint8_t container_id = 0;
    bool has_dynamic_container_id = false;
    uint32_t dynamic_container_id = 0;
    // v859 writes both physical slot fields. `slot` is the changed slot in the
    // named container, while `hotbar_slot` is the protocol's second response
    // byte (it often equals slot, but is not a client-requested slot).
    bool has_hotbar_slot = false;
    uint8_t hotbar_slot = 0;
    // Retained only for source compatibility with the previous speculative
    // parser. v859 never fills this field: it is not the response HotbarSlot.
    bool has_requested_slot = false;
    uint8_t requested_slot = 0;
    uint8_t slot = 0;
    uint8_t count = 0;
    int32_t network_stack_id = 0;
    std::string custom_name;
    std::string filter_custom_name;
    int32_t durability_correction = 0;
};

// Kept with every decoded response so future request code can refuse a packet
// layout which has not been validated for the active game build. None of these
// values authorizes an ItemStackRequest by itself.
enum class ProjectionPrinterInventoryResponseLayout : uint8_t {
    None = 0,
    SlotAmountNetworkId = 1,
    RequestedSlotThenSlotAmountNetworkId = 2,
    SlotThenRequestedSlotAmountNetworkId = 3,
    // Minecraft Bedrock 1.21.120 / protocol v859:
    // Slot, HotbarSlot, Count, ItemStackNetIdVariant, names, durability.
    V859SlotHotbarSlotAmountNetworkId = 4,
};

struct ProjectionPrinterInventoryResponse {
    // This structure now always describes one complete response entry. `valid`
    // means that entry succeeded; it is never combined with rejection fields
    // from a different request.
    bool valid = false;
    // True when this same entry was rejected by the server. The protocol-native
    // status is retained because later builds may expose more failure values.
    bool rejected = false;
    uint8_t status = 0;
    uint8_t rejection_status = 0;
    int32_t rejection_request_id = 0;
    uint64_t rejection_observed_inventory_revision = 0;
    // Increments for every fully decoded ItemStackResponse packet in this
    // session, including a rejection with no container payload. A caller can
    // snapshot it immediately before sending a request and avoid attributing
    // an older rejection to that later request.
    uint64_t response_generation = 0;
    // Packet-level metadata for the packet that carried this entry.
    uint32_t entry_count = 0;
    uint32_t successful_entry_count = 0;
    ProjectionPrinterInventoryResponseLayout layout =
        ProjectionPrinterInventoryResponseLayout::None;
    uint64_t session_generation = 0;
    uint64_t observed_inventory_revision = 0;
    // ItemStackRequest IDs are ZigZag-encoded signed int32 values on v859.
    int32_t request_id = 0;
    std::vector<ProjectionPrinterInventoryResponseSlot> slots;
};

// Called from the raw receive hook after the packet has been decoded by the
// client. This observer never suppresses or mutates the game's packet stream.
void ObserveProjectionPrinterInventoryPacket(std::string_view packet) noexcept;

// Enables one process-lifetime, read-only ItemStackResponse diagnostic window.
// It is off by default, expires after 20 minutes or 128 log lines, and repeat
// calls cannot extend or restart it. Only numeric request/container/slot
// metadata and custom-name byte lengths are logged; the response itself and
// custom-name text are never logged. The receive path remains observational.
void BeginMapManualTraceResponseSession() noexcept;
void EndMapManualTraceResponseSession() noexcept;

// Clear cached ItemStack network IDs when the client changes session/world or
// native authorization is revoked. Safe to call from any hook path.
void ClearProjectionPrinterInventoryMailbox() noexcept;

// Copies the current complete player inventory. Item names are resolved after
// the mailbox lock is released, so the receive hook never depends on registry
// allocation or name lookup latency.
bool GetProjectionPrinterInventorySnapshot(
    ProjectionPrinterInventorySnapshot* output) noexcept;

// Returns the latest complete ItemStackResponse entry retained by the mailbox.
// It preserves the legacy API shape, but never returns a successful entry
// merged with rejection metadata belonging to a different request.
bool GetProjectionPrinterInventoryResponse(
    ProjectionPrinterInventoryResponse* output) noexcept;

// Looks up the most recent complete response entry for one exact signed
// ItemStackRequest ID in the bounded current-session history. A true result
// means the returned success/rejection status and slots all belong to this
// request ID. Callers must use this API, rather than arrival ordering, to
// attribute a server acceptance or rejection to an inventory transaction.
bool GetProjectionPrinterInventoryResponseByRequestId(
    int32_t request_id, ProjectionPrinterInventoryResponse* output) noexcept;

// Returns the current client/world generation even before a complete
// InventoryContent or ItemStackResponse has arrived. It lets a native-only
// inventory move bind a later response to this game session without assuming
// that the embedded item component has refreshed.
uint64_t GetProjectionPrinterInventoryMailboxSessionGeneration() noexcept;

}  // namespace build_import

#endif  // INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_MAILBOX_H
