#ifndef INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_MOVER_H
#define INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_MOVER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace build_import {

struct ProjectionPrinterInventoryClientSyncPreparedMove;

constexpr size_t kProjectionPrinterLiveInventorySlotCount = 36U;

// A point-in-time view of the live player inventory's ItemStack network
// identities.  It is read on the local-player game tick before the Python
// item-component snapshot, then the mover reads the chosen stacks again just
// before submitting an action.  That ordering makes it a safe fallback when
// the receive hook was installed after the login InventoryContent packet.
struct ProjectionPrinterLiveInventorySlot {
    bool occupied = false;
    bool has_network_stack_id = false;
    int32_t network_stack_id = 0;
};

struct ProjectionPrinterLiveInventorySnapshot {
    bool ready = false;
    std::array<ProjectionPrinterLiveInventorySlot,
               kProjectionPrinterLiveInventorySlotCount> slots{};
};

struct ProjectionPrinterNativeInventoryItemSlot {
    bool occupied = false;
    int32_t runtime_item_id = 0;
    uint16_t count = 0;
    uint16_t aux = 0;
    bool has_network_stack_id = false;
    int32_t network_stack_id = 0;
};

struct ProjectionPrinterNativeInventorySnapshot {
    bool ready = false;
    int32_t selected_hotbar_slot = -1;
    std::array<ProjectionPrinterNativeInventoryItemSlot,
               kProjectionPrinterLiveInventorySlotCount> slots{};
};

// Native input slot of an already-bound anvil UI manager. This is deliberately
// only item evidence: the caller must independently prove that the borrowed
// manager belongs to the exact live server window/world and that the matching
// input ItemStackResponse was accepted. The source hotbar net ID is not reused
// here because the server may allocate a new ID after Place.
struct ProjectionPrinterNativeAnvilInputMapSnapshot {
    int32_t runtime_item_id = 0;
    int32_t network_stack_id = 0;
    uint16_t count = 0;
    int64_t map_uuid = -1;
    std::string item_identifier;
};

struct ProjectionPrinterNativeAnvilPreviewMapSnapshot {
    int32_t runtime_item_id = 0;
    uint16_t count = 0;
    int64_t map_uuid = -1;
    uint32_t dynamic_recipe_network_id = 0;
    std::string item_identifier;
    std::string display_name;
};

// The renamed output need not return to the selected hotbar slot. A read-only
// scan reports it only when the full 36-slot native inventory has one and
// only one stack with the expected UUID and exact DisplayName.
struct ProjectionPrinterNativeInventoryFilledMapMatch {
    int32_t inventory_slot = -1;
    int32_t runtime_item_id = 0;
    int32_t network_stack_id = 0;
    uint16_t count = 0;
    int64_t map_uuid = -1;
    std::string item_identifier;
    std::string display_name;
};

struct ProjectionPrinterNativeSelectedFilledMapSnapshot {
    int32_t selected_hotbar_slot = -1;
    int32_t runtime_item_id = 0;
    int32_t network_stack_id = 0;
    uint16_t count = 0;
    int64_t map_uuid = -1;
    std::string item_identifier;
};

// Immutable description captured from either a complete passive
// InventoryContent baseline or the live snapshot above, paired with the
// client item-component snapshot.  The mover reads the native ItemStacks
// again and refuses to send when any network-stack identity has changed.
struct ProjectionPrinterInventoryMoveRequest {
    int32_t source_inventory_slot = -1;       // ordinary inventory: 9..35
    int32_t destination_hotbar_slot = -1;     // hotbar: 0..8
    int32_t expected_source_network_stack_id = 0;
    bool expected_destination_occupied = false;
    int32_t expected_destination_network_stack_id = 0;
    uint16_t expected_source_count = 0;
};

// Constructs one native RequestData -> Batch -> ItemStackRequestPacket and
// sends it directly, as the reference sorter does. It reserves a unique ID
// from the game's counter without registering a client-screen prediction
// scope. The caller must run it on the local-player game tick and serialize
// it with printer disable/clear/revocation. On success it returns the exact
// decoded signed ItemStackRequest ID that the server will echo
// in ItemStackResponse; callers must use that ID rather than packet ordering
// to attribute a later response.
bool MoveProjectionPrinterBackpackItemToHotbar(
    const ProjectionPrinterInventoryMoveRequest& request,
    std::string* error = nullptr, int32_t* request_id = nullptr);

// Reads all 36 player-inventory ItemStack network IDs without sending a packet
// or mutating inventory.  This is intentionally available separately from
// moving so callers can take it before their semantic (Python) inventory read.
bool ReadProjectionPrinterLiveInventorySnapshot(
    ProjectionPrinterLiveInventorySnapshot* output,
    std::string* error = nullptr);

// Serializes each occupied live player ItemStack through the game's verified
// InventorySlot writer, then decodes its item runtime ID/count/aux/net ID.
// Read-only; callers should resolve runtime IDs with the current registry.
bool ReadProjectionPrinterNativeInventorySnapshot(
    ProjectionPrinterNativeInventorySnapshot* output,
    std::string* error = nullptr);

// Serializes only the currently selected live hotbar ItemStack through the
// verified game InventorySlot writer. The selected slot and network identity
// are checked before and after serialization. Read-only, local-player tick.
bool ReadProjectionPrinterNativeSelectedHotbarSlotPacket(
    std::string* packet, int32_t* selected_slot,
    std::string* error = nullptr);

// Call only after the current item runtime ID was resolved as a filled map.
// Rechecks the exact live stack ID before reading its native map_uuid NBT.
bool ReadProjectionPrinterNativeMapUuid(
    int32_t inventory_slot, int32_t expected_network_stack_id,
    int64_t* map_uuid, std::string* error = nullptr);

// Read-only capture for an already server-accepted anvil Place. Serializes
// the same verified live source map twice with the game's InventorySlot
// writer: once as the player hotbar slot and once as the specified visible
// anvil window slot. It does not enqueue either packet or change inventory.
// The caller must independently bind the exact response/window before any
// local client presentation update is allowed.
bool CaptureMapAnvilLocalInputSlotPackets(
    int32_t source_hotbar_slot, int32_t expected_network_stack_id,
    int32_t expected_runtime_item_id, int64_t expected_map_uuid,
    uint8_t anvil_window_id, uint8_t anvil_inventory_slot,
    std::string* source_occupied_packet,
    std::string* anvil_occupied_packet,
    std::string* error = nullptr);

// Reads the real input ItemStack (manager slot index 0) of a synchronously
// borrowed, independently verified anvil manager. Calls only the game's
// fingerprinted read-only getter/serializer/NBT accessors on the owning UI
// thread; rejects an empty slot, wrong runtime item/UUID, unstable net ID or
// changing bytes. Never retain manager beyond the verified borrow call.
// A successful result is NOT server acceptance or a window-binding proof.
bool ReadProjectionPrinterNativeAnvilInputMap(
    uintptr_t verified_anvil_manager, int32_t expected_runtime_item_id,
    int64_t expected_map_uuid,
    ProjectionPrinterNativeAnvilInputMapSnapshot* output,
    std::string* error = nullptr);

// Reads manager+0x130, an ItemInstance preview ending at manager+0x218.
// It has NO ItemStack network-ID variant; this function never reads +0xE8
// from the preview. It double-serializes the preview via the game's
// fingerprinted ItemInstance-base copy inside InventorySlot, validates the
// exact UUID and full DisplayName in emitted NBT and cross-checks native NBT.
// An optional diagnostic recipe ID comes from manager+0x218, verified as the
// 4-byte field stock result-click passes to CraftRecipeOptional. It is read
// twice around serialization, but it must NOT authorize a craft on its own:
// the actual stock result-click action still has to prove the current ID.
// A matching preview is still client-predicted and cannot replace a server
// craft ACK or the final inventory ItemStack readback.
bool ReadProjectionPrinterNativeAnvilPreviewMap(
    uintptr_t verified_anvil_manager, int32_t expected_runtime_item_id,
    int64_t expected_map_uuid, const std::string& expected_full_display_name,
    ProjectionPrinterNativeAnvilPreviewMapSnapshot* output,
    std::string* error = nullptr);

// Reads all ordinary player inventory slots twice through the game's native
// InventorySlot serializer, requiring identical packets and network variants
// throughout. Accepts only a unique one-map stack whose serialized UUID and
// full DisplayName match the expected output and whose UUID agrees with the
// live CompoundTag accessor. The returned actual slot/net ID may differ from
// the pre-anvil source. No packet is sent and no slot is changed. This is not
// proof of server craft acceptance; the caller must match its ACK separately.
bool ReadProjectionPrinterNativeInventoryFilledMapMatch(
    int32_t expected_runtime_item_id, int64_t expected_map_uuid,
    const std::string& expected_full_display_name,
    ProjectionPrinterNativeInventoryFilledMapMatch* output,
    std::string* error = nullptr);

// Positively proves that an exact filled-map UUID is absent from every one of
// the 36 live player slots. Two complete native InventorySlot snapshots must
// agree, and each filled-map stack's serialized UUID must agree with the
// read-only native CompoundTag accessor. A present map returns true with
// *absent == false; any unreadable/unstable slot returns false and leaves
// *absent == false. Call only on the local-player Android game tick.
bool ReadProjectionPrinterNativeInventoryMapUuidAbsent(
    int32_t expected_runtime_item_id, int64_t expected_map_uuid,
    bool* absent, std::string* error = nullptr);

// Strict initial-map readback before opening an anvil. Requires the selected
// hotbar stack to be exactly one filled map with the expected UUID; it does
// not require a custom name. `expected_runtime_item_id == 0` allows the first
// read before the caller has recorded the runtime ID, but still requires a
// positive ID resolved by the live item registry to a filled-map identifier.
// A positive expected ID additionally requires exact equality.
bool ReadProjectionPrinterNativeSelectedFilledMap(
    int32_t expected_runtime_item_id, int64_t expected_map_uuid,
    ProjectionPrinterNativeSelectedFilledMapSnapshot* output,
    std::string* error = nullptr);

// Copies just the two live ItemStacks into native InventorySlot packets and
// uses the game's own BinaryStream writer. Holds source->hotbar updates for
// Commit after Move/Swap; no full InventoryContent cache is required.
bool PrepareProjectionPrinterLiveInventoryClientSyncMove(
    const ProjectionPrinterInventoryMoveRequest& request,
    ProjectionPrinterInventoryClientSyncPreparedMove* prepared,
    std::string* error = nullptr);

// Verifies that the item currently held by the local player still has the
// exact network-stack identity the printer selected before it constructs its
// ItemUse transaction.  This closes the user-switches-slot / inventory-update
// race between material selection and placement.
bool VerifyProjectionPrinterSelectedHotbarItem(
    int32_t expected_hotbar_slot, int32_t expected_network_stack_id,
    std::string* error = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_MOVER_H
