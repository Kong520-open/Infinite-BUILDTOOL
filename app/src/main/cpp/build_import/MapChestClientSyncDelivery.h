#pragma once

#include "ProjectionPrinterInventoryMailbox.h"

#include <cstdint>
#include <string>

namespace build_import {

// One client-only InventorySlot refresh for the source hotbar slot of an
// accepted, tool-owned, visible single-chest Place. This never sends another
// ItemStackRequest and never changes the server or a chest slot.
struct MapChestClientSyncCandidate {
    int32_t request_id = 0;
    uint64_t session_generation = 0;
    uint64_t response_generation_before_send = 0;
    uint64_t window_token = 0;
    uint8_t window_id = 0;
    uint8_t source_hotbar_slot = 0;
    uint8_t destination_slot = 0;
    int32_t runtime_item_id = 0;
    int32_t source_network_stack_id = 0;
    int64_t map_uuid = -1;
    std::string occupied_source_packet;
};

enum class MapChestClientSyncTicketState : uint8_t {
    Unknown = 0,
    Pending,
    Complete,
    Cancelled,
};

struct MapChestClientSyncQueuedPacket {
    uint64_t ticket = 0;
    std::string bytes;
};

// The receive connection must be the one which delivered the exact visible
// chest ContainerOpen. A hidden proof chest must never bind this FIFO.
void BindMapChestClientSyncIngress(
    const void* connection, uint64_t window_token, uint8_t window_id) noexcept;

// Prepare before sending Place, while the live source map is still present.
// The native source pre-image is transformed and validated here, but no local
// packet is queued until the matching server response has been accepted.
bool PrepareMapChestClientSyncCandidate(
    MapChestClientSyncCandidate candidate, std::string* error = nullptr);

bool QueueMapChestClientSyncAcceptedTransfer(
    uint64_t window_token, uint8_t window_id, int32_t request_id,
    const ProjectionPrinterInventoryResponse& response,
    uint64_t* ticket, std::string* error = nullptr);

bool TakeMapChestClientSyncPacket(
    const void* connection, MapChestClientSyncQueuedPacket* output) noexcept;
bool CompleteMapChestClientSyncPacket(uint64_t ticket) noexcept;
MapChestClientSyncTicketState GetMapChestClientSyncTicketState(
    uint64_t ticket) noexcept;

// This is the physical player inventory slot captured before Place. It is
// available to the runtime for exact native empty-slot readback and is never
// guessed from the current selection. Cancellation invalidates it.
bool GetMapChestClientSyncSourceSlot(
    uint64_t window_token, int32_t request_id, uint8_t* slot) noexcept;

void CancelMapChestClientSyncWindow(uint64_t window_token) noexcept;
void ClearMapChestClientSync() noexcept;

}  // namespace build_import
