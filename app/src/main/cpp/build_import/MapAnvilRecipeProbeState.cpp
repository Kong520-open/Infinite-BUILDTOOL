#include "MapAnvilRecipeProbeState.h"

namespace build_import {
namespace {
constexpr auto kObservationLifetime = std::chrono::seconds(30);
}

bool MapAnvilRecipeProbeState::Arm(
    uint64_t ticket, uint8_t window_id, int64_t map_uuid,
    std::chrono::steady_clock::time_point now) noexcept {
    if (ticket == 0 || map_uuid == -1) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    // A later session may not reuse a ticket: a delayed native callback from
    // the old getter must never become evidence for a newly armed session.
    if (ticket <= last_ticket_) return false;
    if (status_ == MapAnvilRecipeProbeStatus::Armed ||
        status_ == MapAnvilRecipeProbeStatus::Observed) return false;
    last_ticket_ = ticket;
    observation_ = {ticket, window_id, map_uuid, 0};
    deadline_ = now + kObservationLifetime;
    status_ = MapAnvilRecipeProbeStatus::Armed;
    return true;
}

void MapAnvilRecipeProbeState::Disarm(uint64_t ticket) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ticket == 0 || observation_.ticket != ticket) return;
    observation_ = {};
    deadline_ = {};
    status_ = MapAnvilRecipeProbeStatus::Unavailable;
}

void MapAnvilRecipeProbeState::expireLocked(
    std::chrono::steady_clock::time_point now) noexcept {
    if ((status_ == MapAnvilRecipeProbeStatus::Armed ||
         status_ == MapAnvilRecipeProbeStatus::Observed) && now >= deadline_) {
        observation_.recipe_network_id = 0;
        status_ = MapAnvilRecipeProbeStatus::Expired;
    }
}

bool MapAnvilRecipeProbeState::Expects(
    uint64_t ticket, int64_t map_uuid,
    std::chrono::steady_clock::time_point now) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    expireLocked(now);
    return ticket != 0 && observation_.ticket == ticket &&
           observation_.map_uuid == map_uuid &&
           (status_ == MapAnvilRecipeProbeStatus::Armed ||
            status_ == MapAnvilRecipeProbeStatus::Observed);
}

void MapAnvilRecipeProbeState::Observe(
    uint64_t ticket, int64_t map_uuid, uint32_t recipe_network_id,
    std::chrono::steady_clock::time_point now) noexcept {
    if (recipe_network_id == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    expireLocked(now);
    if (ticket == 0 || observation_.ticket != ticket ||
        observation_.map_uuid != map_uuid) return;
    if (status_ == MapAnvilRecipeProbeStatus::Armed) {
        observation_.recipe_network_id = recipe_network_id;
        status_ = MapAnvilRecipeProbeStatus::Observed;
    } else if (status_ == MapAnvilRecipeProbeStatus::Observed &&
               observation_.recipe_network_id != recipe_network_id) {
        observation_.recipe_network_id = 0;
        status_ = MapAnvilRecipeProbeStatus::Ambiguous;
    }
}

MapAnvilRecipeProbeStatus MapAnvilRecipeProbeState::Read(
    uint64_t ticket, int64_t map_uuid,
    std::chrono::steady_clock::time_point now,
    MapAnvilRecipeObservation* output) noexcept {
    if (output) *output = {};
    std::lock_guard<std::mutex> lock(mutex_);
    expireLocked(now);
    if (ticket == 0 || observation_.ticket != ticket ||
        observation_.map_uuid != map_uuid) {
        return MapAnvilRecipeProbeStatus::Unavailable;
    }
    if (output && status_ == MapAnvilRecipeProbeStatus::Observed) {
        *output = observation_;
    }
    return status_;
}

}  // namespace build_import
