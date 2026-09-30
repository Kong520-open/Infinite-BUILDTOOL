#include "MapAnvilClientSyncDelivery.h"

#include "MapAnvilClientSlotSync.h"
#include "MapAnvilRenameSender.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <utility>

namespace build_import {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kCandidateLifetime = std::chrono::seconds(20);
constexpr auto kQueuedLifetime = std::chrono::seconds(5);

struct DeliveryState {
    std::mutex mutex;
    const void* connection = nullptr;
    uint64_t window_token = 0;
    uint8_t window_id = 0;
    std::optional<MapAnvilClientSyncCandidate> candidate;
    Clock::time_point candidate_deadline{};
    uint64_t next_ticket = 1;
    uint64_t ticket = 0;
    MapAnvilClientSyncTicketState ticket_state =
        MapAnvilClientSyncTicketState::Unknown;
    uint64_t accepted_response_generation = 0;
    int32_t accepted_destination_network_stack_id = 0;
    Clock::time_point queued_deadline{};
    uint8_t next_packet_index = 0;
    bool packet_in_flight = false;
    MapAnvilClientSlotSyncPackets packets;
};

DeliveryState& state() {
    static DeliveryState instance;
    return instance;
}

bool fail(std::string* error, const char* reason) {
    if (error) *error = reason;
    return false;
}

void clearPayload(DeliveryState* delivery) {
    delivery->packets = {};
    delivery->packet_in_flight = false;
    delivery->next_packet_index = 0;
}

void cancelPending(DeliveryState* delivery) {
    if (delivery->ticket_state == MapAnvilClientSyncTicketState::Pending) {
        delivery->ticket_state = MapAnvilClientSyncTicketState::Cancelled;
    }
    clearPayload(delivery);
}

void expire(DeliveryState* delivery, Clock::time_point now) {
    if (delivery->candidate && now > delivery->candidate_deadline &&
        delivery->ticket_state == MapAnvilClientSyncTicketState::Unknown) {
        delivery->candidate.reset();
    }
    if (delivery->ticket_state == MapAnvilClientSyncTicketState::Pending &&
        now > delivery->queued_deadline) {
        cancelPending(delivery);
    }
}

bool validCandidate(const MapAnvilClientSyncCandidate& candidate) {
    return candidate.request_id < 0 &&
        (static_cast<uint32_t>(candidate.request_id) & 1U) != 0U &&
        candidate.session_generation != 0 && candidate.window_token != 0 &&
        candidate.window_id != 0 && candidate.window_id != 0xFFU &&
        candidate.source_hotbar_slot <= 8U &&
        candidate.anvil_physical_slot_explicit &&
        candidate.anvil_physical_slot <= 2U &&
        candidate.runtime_item_id > 0 &&
        candidate.source_network_stack_id > 0 &&
        candidate.map_uuid != -1 &&
        !candidate.occupied_source_packet.empty() &&
        !candidate.occupied_anvil_packet.empty();
}

MapAnvilClientSlotSyncRequest slotRequest(
    const MapAnvilClientSyncCandidate& candidate, int32_t destination_id) {
    MapAnvilClientSlotSyncRequest request;
    request.source_hotbar_slot = candidate.source_hotbar_slot;
    request.anvil_window_id = candidate.window_id;
    request.anvil_physical_slot = candidate.anvil_physical_slot;
    request.expected_runtime_item_id = candidate.runtime_item_id;
    request.expected_source_network_stack_id =
        candidate.source_network_stack_id;
    request.expected_map_uuid = candidate.map_uuid;
    request.confirmed_destination_network_stack_id = destination_id;
    return request;
}

bool validateAcceptedResponse(const MapAnvilClientSyncCandidate& candidate,
                              const ProjectionPrinterInventoryResponse& response,
                              int32_t* destination_id, std::string* error) {
    MapAnvilInputPlaceRequest request;
    request.task.expected_session_generation = candidate.session_generation;
    request.task.source_hotbar_slot = candidate.source_hotbar_slot;
    request.task.source_network_stack_id =
        candidate.source_network_stack_id;
    request.task.expected_map_uuid = candidate.map_uuid;
    MapAnvilInputSubmission submission;
    submission.request_id = candidate.request_id;
    submission.response_session_generation = candidate.session_generation;
    submission.response_generation_before_send =
        candidate.response_generation_before_send;
    if (!ValidateMapAnvilInputAcceptedResponse(
            request, submission, response, error)) {
        return false;
    }
    for (const auto& slot : response.slots) {
        // This is the server's ItemStackResponse namespace, NOT a physical
        // client InventorySlot index. The latter is independently verified.
        if (slot.container_id == 0U && slot.slot == 1U) {
            *destination_id = slot.network_stack_id;
            return true;
        }
    }
    return fail(error, "accepted anvil response lacks its input slot");
}

}  // namespace

void BindMapAnvilClientSyncIngress(
    const void* connection, uint64_t window_token, uint8_t window_id) noexcept {
    if (!connection || window_token == 0 || window_id == 0 ||
        window_id == 0xFFU) {
        return;
    }
    try {
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        expire(&delivery, Clock::now());
        if (delivery.connection) {
            if (delivery.connection == connection &&
                delivery.window_token == window_token &&
                delivery.window_id == window_id) {
                return;
            }
            // A second ingress cannot redirect an accepted request to an
            // unrelated receive path, even when the numeric window ID repeats.
            cancelPending(&delivery);
            delivery.candidate.reset();
            delivery.connection = nullptr;
            delivery.window_token = 0;
            delivery.window_id = 0;
            return;
        }
        delivery.connection = connection;
        delivery.window_token = window_token;
        delivery.window_id = window_id;
    } catch (...) {
        // A receive hook must never throw into the game.
    }
}

bool PrepareMapAnvilClientSyncCandidate(
    MapAnvilClientSyncCandidate candidate, std::string* error) {
    if (!validCandidate(candidate)) {
        return fail(error, "anvil client sync candidate lacks an explicit physical slot or map identity");
    }
    try {
        // Check serializer output before retaining it. At this stage the
        // destination still carries the source net ID, so this is read-only.
        MapAnvilClientSlotSyncPackets checked;
        if (!BuildMapAnvilClientInputSlotRefresh(
                slotRequest(candidate, candidate.source_network_stack_id),
                candidate.occupied_source_packet,
                candidate.occupied_anvil_packet, &checked, error)) {
            return false;
        }
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        const auto now = Clock::now();
        expire(&delivery, now);
        if (!delivery.connection ||
            delivery.window_token != candidate.window_token ||
            delivery.window_id != candidate.window_id) {
            return fail(error, "anvil client sync has no matching receive ingress");
        }
        if (delivery.ticket_state == MapAnvilClientSyncTicketState::Pending ||
            delivery.candidate) {
            return fail(error, "anvil client sync already has an active candidate");
        }
        delivery.candidate = std::move(candidate);
        delivery.candidate_deadline = now + kCandidateLifetime;
        delivery.ticket = 0;
        delivery.ticket_state = MapAnvilClientSyncTicketState::Unknown;
        delivery.accepted_response_generation = 0;
        delivery.accepted_destination_network_stack_id = 0;
        clearPayload(&delivery);
        if (error) error->clear();
        return true;
    } catch (...) {
        return fail(error, "anvil client sync candidate allocation failed");
    }
}

bool QueueMapAnvilClientSyncAcceptedInput(
    uint64_t window_token, uint8_t window_id, int32_t request_id,
    const ProjectionPrinterInventoryResponse& response,
    uint64_t* ticket, std::string* error) {
    if (ticket) *ticket = 0;
    if (!ticket || window_token == 0 || window_id == 0 ||
        window_id == 0xFFU || request_id >= 0) {
        return fail(error, "anvil client sync queue identity is invalid");
    }
    try {
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        const auto now = Clock::now();
        expire(&delivery, now);
        if (!delivery.connection || !delivery.candidate ||
            delivery.window_token != window_token ||
            delivery.window_id != window_id ||
            delivery.candidate->window_token != window_token ||
            delivery.candidate->window_id != window_id ||
            delivery.candidate->request_id != request_id) {
            return fail(error, "anvil client sync window or request changed");
        }
        int32_t destination_id = 0;
        if (!validateAcceptedResponse(*delivery.candidate, response,
                                      &destination_id, error)) {
            return false;
        }
        if (delivery.ticket != 0) {
            if (delivery.ticket_state ==
                    MapAnvilClientSyncTicketState::Cancelled ||
                delivery.accepted_response_generation !=
                    response.response_generation ||
                delivery.accepted_destination_network_stack_id !=
                    destination_id) {
                return fail(error, "anvil client sync ticket cannot be replayed");
            }
            *ticket = delivery.ticket;
            if (error) error->clear();
            return true;
        }
        MapAnvilClientSlotSyncPackets built;
        if (!BuildMapAnvilClientInputSlotRefresh(
                slotRequest(*delivery.candidate, destination_id),
                delivery.candidate->occupied_source_packet,
                delivery.candidate->occupied_anvil_packet, &built, error)) {
            return false;
        }
        delivery.ticket = delivery.next_ticket++;
        if (delivery.next_ticket == 0) delivery.next_ticket = 1;
        delivery.ticket_state = MapAnvilClientSyncTicketState::Pending;
        delivery.accepted_response_generation = response.response_generation;
        delivery.accepted_destination_network_stack_id = destination_id;
        delivery.queued_deadline = now + kQueuedLifetime;
        delivery.next_packet_index = 0;
        delivery.packet_in_flight = false;
        delivery.packets = std::move(built);
        *ticket = delivery.ticket;
        if (error) error->clear();
        return true;
    } catch (...) {
        return fail(error, "anvil client sync queue allocation failed");
    }
}

bool TakeMapAnvilClientSyncPacket(
    const void* connection, MapAnvilClientSyncQueuedPacket* output) noexcept {
    if (!connection || !output) return false;
    try {
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        expire(&delivery, Clock::now());
        if (delivery.connection != connection ||
            delivery.ticket_state != MapAnvilClientSyncTicketState::Pending ||
            delivery.packet_in_flight || delivery.next_packet_index > 1U) {
            return false;
        }
        const std::string& bytes = delivery.next_packet_index == 0U ?
            delivery.packets.empty_source_packet :
            delivery.packets.occupied_anvil_input_packet;
        if (bytes.empty()) return false;
        MapAnvilClientSyncQueuedPacket next;
        next.ticket = delivery.ticket;
        next.packet_index = delivery.next_packet_index;
        next.bytes = bytes;
        *output = std::move(next);
        delivery.packet_in_flight = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool CompleteMapAnvilClientSyncPacket(
    uint64_t ticket, uint8_t packet_index) noexcept {
    if (ticket == 0 || packet_index > 1U) return false;
    try {
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        expire(&delivery, Clock::now());
        if (delivery.ticket != ticket ||
            delivery.ticket_state != MapAnvilClientSyncTicketState::Pending ||
            !delivery.packet_in_flight ||
            delivery.next_packet_index != packet_index) {
            return false;
        }
        delivery.packet_in_flight = false;
        if (packet_index == 0U) {
            delivery.packets.empty_source_packet.clear();
            delivery.next_packet_index = 1U;
        } else {
            delivery.ticket_state = MapAnvilClientSyncTicketState::Complete;
            clearPayload(&delivery);
            // The raw map packet is no longer needed. Keep only identity so
            // repeated polling returns the same completed ticket, not a send.
            if (delivery.candidate) {
                delivery.candidate->occupied_source_packet.clear();
                delivery.candidate->occupied_anvil_packet.clear();
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

MapAnvilClientSyncTicketState GetMapAnvilClientSyncTicketState(
    uint64_t ticket) noexcept {
    if (ticket == 0) return MapAnvilClientSyncTicketState::Unknown;
    try {
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        expire(&delivery, Clock::now());
        return delivery.ticket == ticket ? delivery.ticket_state :
            MapAnvilClientSyncTicketState::Unknown;
    } catch (...) {
        return MapAnvilClientSyncTicketState::Unknown;
    }
}

void CancelMapAnvilClientSyncWindow(uint64_t window_token) noexcept {
    if (window_token == 0) return;
    try {
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        if (delivery.window_token != window_token &&
            (!delivery.candidate ||
             delivery.candidate->window_token != window_token)) {
            return;
        }
        cancelPending(&delivery);
        delivery.candidate.reset();
        delivery.connection = nullptr;
        delivery.window_token = 0;
        delivery.window_id = 0;
    } catch (...) {
    }
}

void ClearMapAnvilClientSync() noexcept {
    try {
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        cancelPending(&delivery);
        delivery.candidate.reset();
        delivery.connection = nullptr;
        delivery.window_token = 0;
        delivery.window_id = 0;
    } catch (...) {
    }
}

}  // namespace build_import
