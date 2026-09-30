#ifndef INFINITE_TEXTURE_MAP_ANVIL_CLIENT_SYNC_DELIVERY_H
#define INFINITE_TEXTURE_MAP_ANVIL_CLIENT_SYNC_DELIVERY_H

#include "ProjectionPrinterInventoryMailbox.h"

#include <cstdint>
#include <string>

namespace build_import {

// A process-local presentation correction for one already-accepted anvil
// input Place. It is never a server packet and never authorizes a second
// ItemStackRequest. All IDs belong to the same verified visible window.
struct MapAnvilClientSyncCandidate {
    int32_t request_id = 0;
    uint64_t session_generation = 0;
    uint64_t response_generation_before_send = 0;
    uint64_t window_token = 0;
    uint8_t window_id = 0;
    uint8_t source_hotbar_slot = 0;
    // Explicitly selected by the caller from the target client's anvil
    // layout; never copied from ItemStackResponse's separate slot namespace.
    bool anvil_physical_slot_explicit = false;
    uint8_t anvil_physical_slot = 0;
    int32_t runtime_item_id = 0;
    int32_t source_network_stack_id = 0;
    int64_t map_uuid = -1;
    std::string occupied_source_packet;
    std::string occupied_anvil_packet;
};

enum class MapAnvilClientSyncTicketState : uint8_t {
    Unknown = 0,
    Pending = 1,
    Complete = 2,
    Cancelled = 3,
};

struct MapAnvilClientSyncQueuedPacket {
    uint64_t ticket = 0;
    uint8_t packet_index = 0;
    std::string bytes;
};

// Bound only by the exact first ContainerOpen packet owned by an automatic
// visible anvil capture. The network receive hook supplies its client-bound
// connection pointer; neither a manually opened anvil nor an arbitrary
// ItemStackResponse may change the binding.
void BindMapAnvilClientSyncIngress(
    const void* connection, uint64_t window_token, uint8_t window_id) noexcept;

bool PrepareMapAnvilClientSyncCandidate(
    MapAnvilClientSyncCandidate candidate, std::string* error = nullptr);

// The caller has durably recorded InputResponseAccepted and freshly verified
// the same visible window, original world and still-empty native input. This
// function rechecks the exact v859 response and prepares two client-only
// InventorySlot packets, source empty first, destination map second.
bool QueueMapAnvilClientSyncAcceptedInput(
    uint64_t window_token, uint8_t window_id, int32_t request_id,
    const ProjectionPrinterInventoryResponse& response,
    uint64_t* ticket, std::string* error = nullptr);

bool TakeMapAnvilClientSyncPacket(
    const void* connection, MapAnvilClientSyncQueuedPacket* output) noexcept;
bool CompleteMapAnvilClientSyncPacket(
    uint64_t ticket, uint8_t packet_index) noexcept;
MapAnvilClientSyncTicketState GetMapAnvilClientSyncTicketState(
    uint64_t ticket) noexcept;
void CancelMapAnvilClientSyncWindow(uint64_t window_token) noexcept;
void ClearMapAnvilClientSync() noexcept;

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_ANVIL_CLIENT_SYNC_DELIVERY_H
