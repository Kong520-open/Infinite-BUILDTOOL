#ifndef INFINITE_TEXTURE_CONTAINER_CAPTURE_MAILBOX_H
#define INFINITE_TEXTURE_CONTAINER_CAPTURE_MAILBOX_H

#include "ItemExtraMapMetadata.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace build_import {

struct CapturedContainerItem {
    uint16_t slot = 0;
    int32_t numeric_id = 0;
    uint16_t count = 0;
    uint16_t aux = 0;
    // Optional wire evidence for future container transfers. A successful
    // capture does not imply either field is present or trustworthy by itself;
    // callers must also verify the runtime item kind and matching map UUID.
    bool has_network_stack_id = false;
    int32_t network_stack_id = 0;
    bool has_map_uuid = false;
    int64_t map_uuid = -1;
    // Candidate fields observed in the length-delimited ItemExtraData. The
    // target client's anvil naming path has not been confirmed, so a present
    // candidate alone must not be treated as proof of a completed rename.
    MapItemNameStatus name_status = MapItemNameStatus::Missing;
    MapItemNameSource name_source = MapItemNameSource::None;
    std::string name_candidate;
};

struct ContainerCaptureResult {
    uint64_t token = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint8_t container_id = 0;
    uint8_t container_type = 0;
    // Remains true even when a later InventoryContent packet is malformed.
    // The runtime must still close that server window before retrying.
    bool container_opened = false;
    // Set after a matching server ContainerClose packet. An opened-but-closed
    // window must not receive another client close while the capture retries.
    bool container_closed = false;
    uint32_t slot_count = 0;
    // InventoryContent's FullContainerName is independent of the window ID
    // above. Keep it separate so a future ItemStackRequest cannot mistake
    // the UI window number for its optional dynamic container identity.
    bool has_full_container_name = false;
    uint8_t full_container_name = 0;
    bool has_dynamic_container_id = false;
    uint32_t dynamic_container_id = 0;
    std::vector<CapturedContainerItem> items;
    std::string error;
};

enum class ContainerCapturePollState : uint8_t {
    Inactive = 0,
    WaitingForOpen = 1,
    WaitingForContent = 2,
    Ready = 3,
    Failed = 4,
};

constexpr uint32_t kMaximumCapturedContainerSlots = 256U;

// A cancelled request may still have one or more packets in the networking
// queue.  Keep that short-lived tail out of the game's UI before another
// automatic ClickBlock is allowed to reuse the same window ID.
struct ContainerCaptureQuarantine {
    uint64_t token = 0;
    uint8_t container_id = 0;
    uint8_t container_type = 0;
    bool from_visible_anvil = false;
    bool container_opened = false;
    bool container_closed = false;
    bool content_captured = false;
    bool close_sent = false;
};

// Only one container is captured at a time. The token prevents a packet that
// started parsing under a cancelled request from being committed after a
// retry, and lets the game tick poll without retaining packet-owned memory.
// A packet that first arrives after the server has reused the same window ID
// is indistinguishable on the wire; the runtime closes and settles windows
// between retries to keep that case out of the next request.
void ArmContainerCapture(uint64_t token, int32_t x, int32_t y, int32_t z);
// Atomic, exclusive hidden capture for an automated chest transfer. Unlike
// the legacy void arm, this refuses every active owner and unresolved packet
// quarantine. A legacy capture must not replace an already-claimed chest.
// The owner must cancel by the same token after its window is closed.
bool TryArmHiddenChestCapture(uint64_t token, int32_t x, int32_t y,
                               int32_t z) noexcept;
// Diagnostic/stock-client chest capture. The exact type-0 chest window is
// parsed as usual, but Open/Content/Slot/Close all reach the native client.
// The caller must close the visible screen before cancelling this token.
// A cancelled pre-Open request still quarantines its delayed window.
bool TryArmVisibleChestCapture(uint64_t token, int32_t x, int32_t y,
                               int32_t z) noexcept;
// Read-only gate for an automatic visible chest's still-open first window.
// This is packet/capture identity evidence, not proof that the corresponding
// native UI remains the top ScreenManager scene. Any later ContainerOpen or
// world-reset packet invalidates this gate for the current capture.
bool IsExactOpenVisibleChestCapture(uint64_t token, uint8_t window_id,
                                    uint8_t window_type, int32_t x, int32_t y,
                                    int32_t z) noexcept;
// An explicitly automated, world-verified anvil may need the stock client
// screen/controller to construct its live recipe and output stack identity.
// Unlike ArmContainerCapture, this records the exact type-5 anvil window but
// lets its Open/Content/Slot/Close packets reach the game. A caller must own
// the token, verify the world/session, close the window, then cancel capture.
// Returns false while another capture or an unresolved quarantine is active.
bool ArmVisibleAnvilCapture(uint64_t token, int32_t x, int32_t y,
                            int32_t z) noexcept;
void CancelContainerCapture(uint64_t token) noexcept;
ContainerCapturePollState PollContainerCapture(uint64_t token,
                                               ContainerCaptureResult* output);

// A visible anvil's Open reached the stock client, so cancelling the passive
// capture must not hide its later server Close. Retain only the exact opened
// type-5 window for a short time and report a matching inbound Close. This is
// evidence of server closure, not proof that the native screen was destroyed.
// A new capture, world reset, or timeout invalidates the receipt.
bool HasVisibleAnvilServerCloseReceipt(uint64_t token,
                                       uint8_t window_id) noexcept;
void ClearVisibleAnvilServerCloseReceipt() noexcept;

// Read-only gate for a newly placed, single 27-slot chest. This deliberately
// does not authorize a transfer: the target's ItemStackRequest container ABI
// and server response still have to be verified before any write is enabled.
bool SelectEmptySingleChestSlot(const ContainerCaptureResult& capture,
                                uint64_t expected_token, int32_t expected_x,
                                int32_t expected_y, int32_t expected_z,
                                uint8_t* slot, std::string* error = nullptr);

// Returns a recently cancelled incomplete request while it is still being
// quarantined.  This is polled only from the game thread, which may send one
// safe ContainerClose when a delayed ContainerOpen appears.  The raw receive
// hook continues to suppress matching late packets during the quarantine.
bool PollContainerCaptureQuarantine(ContainerCaptureQuarantine* output) noexcept;
void MarkContainerCaptureQuarantineCloseSent(uint8_t container_id,
                                             uint8_t container_type) noexcept;

// Called from the raw receive hook. Returns true only when a hidden automatic
// capture owns the packet and it must not reach the game's UI. An explicitly
// armed visible anvil or chest is captured by the same mailbox but always
// passed to the stock client, including later Slot and Close packets.
enum class VisibleAnvilCaptureEventKind : uint8_t {
    None = 0,
    Open = 1,
    Close = 2,
};

struct VisibleAnvilCaptureEvent {
    VisibleAnvilCaptureEventKind kind = VisibleAnvilCaptureEventKind::None;
    uint64_t token = 0;
    uint8_t window_id = 0;
};

enum class VisibleChestCaptureEventKind : uint8_t {
    None = 0,
    Open = 1,
    Close = 2,
};

struct VisibleChestCaptureEvent {
    VisibleChestCaptureEventKind kind = VisibleChestCaptureEventKind::None;
    uint64_t token = 0;
    uint8_t window_id = 0;
};

// An event is reported only for the first matching Open, or the matching
// Close, owned by an automatic visible-anvil capture. This lets the receive
// hook bind a local presentation correction to the exact client ingress.
bool ObserveContainerCapturePacket(
    std::string_view packet,
    VisibleAnvilCaptureEvent* visible_anvil_event = nullptr,
    VisibleChestCaptureEvent* visible_chest_event = nullptr) noexcept;

// Diagnostic-only observation of a player-opened ordinary single chest. This
// never owns a packet or changes the capture mailbox's suppression decision.
// The optional event output exists for deterministic parser tests; production
// callers may omit it. Logging is bounded to 128 events during the first
// 20 minutes after a manual chest open in this process.
enum class ManualChestTraceEventKind : uint8_t {
    None = 0,
    Open = 1,
    Content = 2,
    Slot = 3,
    Close = 4,
};

struct ManualChestTraceMapSample {
    uint16_t slot = 0;
    int64_t map_uuid = -1;
    MapItemNameStatus name_status = MapItemNameStatus::Missing;
    MapItemNameSource name_source = MapItemNameSource::None;
    uint16_t name_bytes = 0;
};

struct ManualChestTraceEvent {
    ManualChestTraceEventKind kind = ManualChestTraceEventKind::None;
    uint8_t container_id = 0;
    uint8_t container_type = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    bool parsed = false;
    uint32_t slot_count = 0;
    uint32_t item_count = 0;
    bool has_full_container_name = false;
    uint8_t full_container_name = 0;
    bool has_dynamic_container_id = false;
    uint32_t dynamic_container_id = 0;
    uint32_t map_uuid_count = 0;
    std::array<ManualChestTraceMapSample, 4U> map_samples{};
    uint8_t map_sample_count = 0;
    uint16_t changed_slot = 0;
    bool slot_occupied = false;
    bool slot_has_network_stack_id = false;
    int32_t slot_network_stack_id = 0;
    bool slot_has_map_uuid = false;
    int64_t slot_map_uuid = -1;
    MapItemNameStatus slot_name_status = MapItemNameStatus::Missing;
    MapItemNameSource slot_name_source = MapItemNameSource::None;
    uint16_t slot_name_bytes = 0;
    bool close_server_initiated = false;
};

bool ObserveManualChestTracePacket(std::string_view packet,
                                   ManualChestTraceEvent* event = nullptr) noexcept;

}  // namespace build_import

#endif  // INFINITE_TEXTURE_CONTAINER_CAPTURE_MAILBOX_H
