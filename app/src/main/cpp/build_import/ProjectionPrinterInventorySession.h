#ifndef INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_SESSION_H
#define INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_SESSION_H

#include <cstdint>
#include <string>
#include <string_view>

namespace build_import {

// This is the ContainerOpen type emitted by the server in response to an
// Interact::OpenInventory request.  It is deliberately checked together with
// InventoryContent's network inventory ID (0) before a packet is attributed to
// this session.
// Other ContainerOpen packets, including chests and block containers, are
// never claimed by this mailbox.
constexpr uint8_t kProjectionPrinterPlayerInventoryContainerType = 0xFFU;
constexpr uint32_t kProjectionPrinterPlayerInventoryNetworkId = 0U;
constexpr uint32_t kProjectionPrinterPlayerInventorySessionSlotCount = 36U;

enum class ProjectionPrinterInventorySessionPollState : uint8_t {
    Inactive = 0,
    WaitingForOpen = 1,
    WaitingForContent = 2,
    Ready = 3,
    WaitingForClose = 4,
    Closed = 5,
    Failed = 6,
};

// Delivery decision for the raw receive hook. The exact active player-inventory
// ContainerOpen reaches stock dispatch with its UI call separately cancelled;
// a late packet from a cancelled request must stay out of the game UI.
enum class ProjectionPrinterInventorySessionReceiveDisposition : uint8_t {
    Pass = 0,
    ActiveOpen = 1,
    QuarantinedOpen = 2,
    WorldReset = 3,
};

struct ProjectionPrinterInventorySessionResult {
    uint64_t token = 0;
    uint8_t container_id = 0;
    uint8_t container_type = 0;
    bool container_opened = false;
    bool inventory_content_received = false;
    bool client_close_sent = false;
    bool container_closed = false;
    // The reference client only sends ContainerClose for a nonzero player
    // window ID.  A zero ID denotes the ordinary local player inventory and
    // should be finalized locally instead of sending a synthetic close.
    bool requires_client_close = false;
    std::string error;
};

// A CloseRequest only reports a server window that this mailbox has already
// claimed.  The component intentionally does not send it: the caller must use
// the version-checked native ContainerClosePacket sender from the game thread.
struct ProjectionPrinterInventorySessionCloseRequest {
    uint64_t token = 0;
    uint8_t container_id = 0;
    uint8_t container_type = 0;
    bool requires_client_close = false;
};

// A cancelled Interact::OpenInventory can still receive a late ContainerOpen
// from the server.  The receive hook must keep that short tail out of the game
// UI; the game thread may use this result to send one safe close for a nonzero
// late window ID.
struct ProjectionPrinterInventorySessionQuarantine {
    uint8_t container_id = 0;
    uint8_t container_type = 0;
    bool container_opened = false;
    bool inventory_content_received = false;
    bool client_close_sent = false;
    bool container_closed = false;
    bool requires_client_close = false;
};

// Arms a single silent player-inventory session.  Call this immediately before
// sending the native Interact(OpenInventory) packet.  It returns false while a
// previous cancelled request is still quarantining delayed packets; callers
// must wait rather than risk attributing that old response to a new request.
bool ArmProjectionPrinterInventorySession(uint64_t token) noexcept;

// Cancels an active session.  Any packet tail that can still open a silent
// player inventory is quarantined and its ContainerOpen remains suppressed
// briefly so a cancelled request cannot reopen a screen.
void CancelProjectionPrinterInventorySession(uint64_t token) noexcept;

// Clears both active and quarantined state on world/session teardown.
void ClearProjectionPrinterInventorySession() noexcept;

ProjectionPrinterInventorySessionPollState PollProjectionPrinterInventorySession(
    uint64_t token, ProjectionPrinterInventorySessionResult* output);

// Returns the exact close parameters after a matching player ContainerOpen.
// This is intentionally non-mutating so an unavailable native sender can be
// retried without generating duplicate requests.  Call Mark...CloseSent only
// after the native sender accepted the packet.
bool GetProjectionPrinterInventorySessionCloseRequest(
    uint64_t token, ProjectionPrinterInventorySessionCloseRequest* output) noexcept;
void MarkProjectionPrinterInventorySessionCloseSent(uint64_t token,
                                                     uint8_t container_id,
                                                     uint8_t container_type) noexcept;

// Called only when a printer-owned ContainerOpen has already been identified
// by the receive hook but its version-gated UI cancellation cannot be armed.
// The receive hook suppresses that open and this marker stops the move from
// proceeding without a confirmed silent session.
void FailProjectionPrinterInventorySessionPresentationGate() noexcept;

bool PollProjectionPrinterInventorySessionQuarantine(
    ProjectionPrinterInventorySessionQuarantine* output) noexcept;
void MarkProjectionPrinterInventorySessionQuarantineCloseSent(
    uint8_t container_id, uint8_t container_type) noexcept;

// Called from the raw receive hook after the passive player-inventory mailbox
// has observed the packet.  It records the armed Interact OpenInventory flow:
// ContainerOpen(type=Inventory), its player InventoryContent(id=0, 36 slots),
// and a matching ContainerClose. The receive hook routes ActiveOpen through
// the version-gated UI cancellation hook while retaining stock packet handling;
// it raw-suppresses QuarantinedOpen. All other
// packets, including InventoryContent, InventorySlot, ItemStackResponse and
// ContainerClose, return Pass and must reach the game. WorldReset clears only
// this session; the receive hook releases the separate presentation gate.
ProjectionPrinterInventorySessionReceiveDisposition
ObserveProjectionPrinterInventorySessionPacket(std::string_view packet) noexcept;

}  // namespace build_import

#endif  // INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_SESSION_H
