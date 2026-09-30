#include "MapChestClientSyncDelivery.h"

#include "MapChestClientSlotSync.h"

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
    std::optional<MapChestClientSyncCandidate> candidate;
    Clock::time_point candidate_deadline{};
    uint64_t next_ticket = 1;
    uint64_t ticket = 0;
    MapChestClientSyncTicketState ticket_state =
        MapChestClientSyncTicketState::Unknown;
    uint64_t accepted_response_generation = 0;
    Clock::time_point queued_deadline{};
    bool packet_in_flight = false;
    std::string empty_source_packet;
};

DeliveryState& state() {
    static DeliveryState instance;
    return instance;
}

bool fail(std::string* error, const char* reason) {
    if (error) *error = reason;
    return false;
}

void discardPacket(DeliveryState* delivery) {
    std::string{}.swap(delivery->empty_source_packet);
    delivery->packet_in_flight = false;
}

void cancel(DeliveryState* delivery) {
    if (delivery->ticket_state == MapChestClientSyncTicketState::Pending) {
        delivery->ticket_state = MapChestClientSyncTicketState::Cancelled;
    }
    delivery->candidate.reset();
    discardPacket(delivery);
}

void expire(DeliveryState* delivery, Clock::time_point now) {
    if (delivery->candidate && now > delivery->candidate_deadline &&
        delivery->ticket_state == MapChestClientSyncTicketState::Unknown) {
        delivery->candidate.reset();
        discardPacket(delivery);
    }
    if (delivery->ticket_state == MapChestClientSyncTicketState::Pending &&
        now > delivery->queued_deadline) {
        cancel(delivery);
    }
}

bool validCandidate(const MapChestClientSyncCandidate& candidate) {
    return candidate.request_id < 0 &&
        (static_cast<uint32_t>(candidate.request_id) & 1U) != 0U &&
        candidate.session_generation != 0U &&
        candidate.window_token != 0U &&
        candidate.window_id != 0U && candidate.window_id != 0xFFU &&
        candidate.source_hotbar_slot <= 8U &&
        candidate.destination_slot < 27U &&
        candidate.runtime_item_id > 0 &&
        candidate.source_network_stack_id > 0 &&
        candidate.map_uuid != -1 &&
        !candidate.occupied_source_packet.empty();
}

bool acceptedResponse(const MapChestClientSyncCandidate& candidate,
                      const ProjectionPrinterInventoryResponse& response,
                      std::string* error) {
    if (!response.valid || response.rejected || response.status != 0U ||
        response.layout !=
            ProjectionPrinterInventoryResponseLayout::
                V859SlotHotbarSlotAmountNetworkId ||
        response.request_id != candidate.request_id ||
        response.session_generation != candidate.session_generation ||
        response.response_generation <=
            candidate.response_generation_before_send) {
        return fail(error, "chest local sync lacks one exact accepted Place response");
    }
    const ProjectionPrinterInventoryResponseSlot* source = nullptr;
    const ProjectionPrinterInventoryResponseSlot* destination = nullptr;
    for (const auto& slot : response.slots) {
        if (slot.has_dynamic_container_id || slot.dynamic_container_id != 0U) {
            return fail(error, "chest local sync response contains a dynamic container");
        }
        if (slot.container_id == 29U &&
            slot.slot == candidate.source_hotbar_slot) {
            if (source) return fail(error, "chest local sync response repeats its source slot");
            source = &slot;
        } else if (slot.container_id == 7U &&
                   slot.slot == candidate.destination_slot) {
            if (destination) return fail(error, "chest local sync response repeats its chest slot");
            destination = &slot;
        }
    }
    if (!source || !destination || source->count != 0U ||
        destination->count != 1U ||
        destination->network_stack_id <= 0) {
        return fail(error, "chest local sync response did not move one map to the chest");
    }
    if (error) error->clear();
    return true;
}

}  // namespace

void BindMapChestClientSyncIngress(
    const void* connection, uint64_t window_token, uint8_t window_id) noexcept {
    if (!connection || window_token == 0U || window_id == 0U ||
        window_id == 0xFFU) return;
    try {
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        expire(&delivery, Clock::now());
        if (delivery.connection == connection &&
            delivery.window_token == window_token &&
            delivery.window_id == window_id) return;
        // A different Open must never inherit an earlier candidate or packet.
        cancel(&delivery);
        delivery.connection = connection;
        delivery.window_token = window_token;
        delivery.window_id = window_id;
        delivery.ticket = 0U;
        delivery.ticket_state = MapChestClientSyncTicketState::Unknown;
        delivery.accepted_response_generation = 0U;
    } catch (...) {
        // Receive hooks cannot unwind into the client.
    }
}

bool PrepareMapChestClientSyncCandidate(
    MapChestClientSyncCandidate candidate, std::string* error) {
    if (!validCandidate(candidate)) {
        return fail(error, "chest local sync candidate identity is invalid");
    }
    try {
        MapChestClientSlotSyncRequest request;
        request.source_hotbar_slot = candidate.source_hotbar_slot;
        request.expected_runtime_item_id = candidate.runtime_item_id;
        request.expected_source_network_stack_id =
            candidate.source_network_stack_id;
        request.expected_map_uuid = candidate.map_uuid;
        std::string empty_source_packet;
        if (!BuildMapChestClientSourceSlotRefresh(
                request, candidate.occupied_source_packet,
                &empty_source_packet, error)) {
            return false;
        }
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        const auto now = Clock::now();
        expire(&delivery, now);
        if (!delivery.connection ||
            delivery.window_token != candidate.window_token ||
            delivery.window_id != candidate.window_id) {
            return fail(error, "chest local sync has no matching visible receive ingress");
        }
        if (delivery.candidate ||
            delivery.ticket_state == MapChestClientSyncTicketState::Pending) {
            return fail(error, "chest local sync already owns an active Place");
        }
        candidate.occupied_source_packet.clear();
        delivery.candidate = std::move(candidate);
        delivery.candidate_deadline = now + kCandidateLifetime;
        delivery.ticket = 0U;
        delivery.ticket_state = MapChestClientSyncTicketState::Unknown;
        delivery.accepted_response_generation = 0U;
        delivery.empty_source_packet = std::move(empty_source_packet);
        delivery.packet_in_flight = false;
        if (error) error->clear();
        return true;
    } catch (...) {
        return fail(error, "chest local sync candidate allocation failed");
    }
}

bool QueueMapChestClientSyncAcceptedTransfer(
    uint64_t window_token, uint8_t window_id, int32_t request_id,
    const ProjectionPrinterInventoryResponse& response,
    uint64_t* ticket, std::string* error) {
    if (ticket) *ticket = 0U;
    if (!ticket || window_token == 0U || window_id == 0U ||
        window_id == 0xFFU || request_id >= 0) {
        return fail(error, "chest local sync queue identity is invalid");
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
            return fail(error, "chest local sync window or request changed");
        }
        if (!acceptedResponse(*delivery.candidate, response, error)) {
            return false;
        }
        if (delivery.ticket != 0U) {
            if (delivery.ticket_state == MapChestClientSyncTicketState::Cancelled ||
                delivery.accepted_response_generation !=
                    response.response_generation) {
                return fail(error, "chest local sync ticket cannot be replayed");
            }
            *ticket = delivery.ticket;
            if (error) error->clear();
            return true;
        }
        if (delivery.empty_source_packet.empty()) {
            return fail(error, "chest local sync source refresh is unavailable");
        }
        delivery.ticket = delivery.next_ticket++;
        if (delivery.next_ticket == 0U) delivery.next_ticket = 1U;
        delivery.ticket_state = MapChestClientSyncTicketState::Pending;
        delivery.accepted_response_generation = response.response_generation;
        delivery.queued_deadline = now + kQueuedLifetime;
        delivery.packet_in_flight = false;
        *ticket = delivery.ticket;
        if (error) error->clear();
        return true;
    } catch (...) {
        return fail(error, "chest local sync queue allocation failed");
    }
}

bool TakeMapChestClientSyncPacket(
    const void* connection, MapChestClientSyncQueuedPacket* output) noexcept {
    if (!connection || !output) return false;
    try {
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        expire(&delivery, Clock::now());
        if (delivery.connection != connection || !delivery.candidate ||
            delivery.ticket_state != MapChestClientSyncTicketState::Pending ||
            delivery.packet_in_flight || delivery.empty_source_packet.empty()) {
            return false;
        }
        MapChestClientSyncQueuedPacket next;
        next.ticket = delivery.ticket;
        next.bytes = delivery.empty_source_packet;
        *output = std::move(next);
        delivery.packet_in_flight = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool CompleteMapChestClientSyncPacket(uint64_t ticket) noexcept {
    if (ticket == 0U) return false;
    try {
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        expire(&delivery, Clock::now());
        if (delivery.ticket != ticket ||
            delivery.ticket_state != MapChestClientSyncTicketState::Pending ||
            !delivery.packet_in_flight) return false;
        delivery.ticket_state = MapChestClientSyncTicketState::Complete;
        discardPacket(&delivery);
        return true;
    } catch (...) {
        return false;
    }
}

MapChestClientSyncTicketState GetMapChestClientSyncTicketState(
    uint64_t ticket) noexcept {
    if (ticket == 0U) return MapChestClientSyncTicketState::Unknown;
    try {
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        expire(&delivery, Clock::now());
        return delivery.ticket == ticket ? delivery.ticket_state :
            MapChestClientSyncTicketState::Unknown;
    } catch (...) {
        return MapChestClientSyncTicketState::Unknown;
    }
}

bool GetMapChestClientSyncSourceSlot(
    uint64_t window_token, int32_t request_id, uint8_t* slot) noexcept {
    if (slot) *slot = 0xFFU;
    if (!slot || window_token == 0U || request_id >= 0) return false;
    try {
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        expire(&delivery, Clock::now());
        if (!delivery.connection || !delivery.candidate ||
            delivery.window_token != window_token ||
            delivery.candidate->window_token != window_token ||
            delivery.candidate->request_id != request_id ||
            delivery.ticket_state == MapChestClientSyncTicketState::Cancelled) {
            return false;
        }
        *slot = delivery.candidate->source_hotbar_slot;
        return true;
    } catch (...) {
        return false;
    }
}

void CancelMapChestClientSyncWindow(uint64_t window_token) noexcept {
    if (window_token == 0U) return;
    try {
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        if (delivery.window_token != window_token) return;
        cancel(&delivery);
        delivery.connection = nullptr;
        delivery.window_token = 0U;
        delivery.window_id = 0U;
    } catch (...) {
    }
}

void ClearMapChestClientSync() noexcept {
    try {
        DeliveryState& delivery = state();
        std::lock_guard<std::mutex> lock(delivery.mutex);
        cancel(&delivery);
        delivery.connection = nullptr;
        delivery.window_token = 0U;
        delivery.window_id = 0U;
    } catch (...) {
    }
}

}  // namespace build_import
