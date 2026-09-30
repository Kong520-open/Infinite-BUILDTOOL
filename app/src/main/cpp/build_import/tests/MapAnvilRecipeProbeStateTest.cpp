#include "MapAnvilRecipeProbeState.h"

#include <cassert>
#include <chrono>

using build_import::MapAnvilRecipeObservation;
using build_import::MapAnvilRecipeProbeState;
using build_import::MapAnvilRecipeProbeStatus;

int main() {
    MapAnvilRecipeProbeState state;
    MapAnvilRecipeObservation observation;
    const auto start = std::chrono::steady_clock::time_point{};

    assert(state.Read(7, 42, start, &observation) ==
           MapAnvilRecipeProbeStatus::Unavailable);
    assert(!state.Arm(0, 3, 42, start));
    assert(!state.Arm(7, 3, -1, start));
    assert(state.Arm(7, 3, 42, start));
    assert(!state.Arm(8, 4, 43, start));
    assert(state.Read(7, 42, start, &observation) ==
           MapAnvilRecipeProbeStatus::Armed);
    assert(observation.recipe_network_id == 0);

    state.Observe(8, 42, 2411, start);   // wrong ticket
    state.Observe(7, 43, 2411, start);   // wrong map
    state.Observe(7, 42, 0, start);      // invalid ID
    assert(state.Read(7, 42, start, &observation) ==
           MapAnvilRecipeProbeStatus::Armed);
    state.Observe(7, 42, 2411, start);
    assert(state.Read(7, 42, start, &observation) ==
           MapAnvilRecipeProbeStatus::Observed);
    assert(observation.ticket == 7 && observation.window_id == 3 &&
           observation.map_uuid == 42 && observation.recipe_network_id == 2411);
    state.Observe(7, 42, 2411, start);   // same lookup is harmless
    assert(state.Read(7, 42, start, &observation) ==
           MapAnvilRecipeProbeStatus::Observed);
    state.Observe(7, 42, 2412, start);   // conflicting recipe closes the gate
    assert(state.Read(7, 42, start, &observation) ==
           MapAnvilRecipeProbeStatus::Ambiguous);
    assert(observation.recipe_network_id == 0);
    state.Disarm(8);                      // wrong ticket cannot clear it
    assert(state.Read(7, 42, start, nullptr) ==
           MapAnvilRecipeProbeStatus::Ambiguous);
    state.Disarm(7);
    assert(state.Read(7, 42, start, nullptr) ==
           MapAnvilRecipeProbeStatus::Unavailable);
    assert(!state.Arm(7, 3, 42, start)); // never reuse a stale callback ticket

    assert(state.Arm(9, 5, 99, start));
    assert(state.Expects(9, 99, start));
    assert(!state.Expects(9, 98, start));
    const auto expired = start + std::chrono::seconds(30);
    assert(!state.Expects(9, 99, expired));
    state.Observe(9, 99, 100, expired);
    assert(state.Read(9, 99, expired, &observation) ==
           MapAnvilRecipeProbeStatus::Expired);
    assert(observation.recipe_network_id == 0);
    assert(state.Arm(10, 6, 100, expired));
    state.Observe(10, 100, 123, expired);
    assert(state.Read(10, 100, expired, &observation) ==
           MapAnvilRecipeProbeStatus::Observed);
    assert(state.Read(10, 100, expired + std::chrono::seconds(30),
                      &observation) == MapAnvilRecipeProbeStatus::Expired);
    assert(observation.recipe_network_id == 0);

    state.Disarm(10);
    constexpr int64_t negative_map_uuid = -532575944698LL;
    assert(state.Arm(11, 7, negative_map_uuid, expired));
    assert(state.Expects(11, negative_map_uuid, expired));
    state.Observe(11, negative_map_uuid, 731, expired);
    assert(state.Read(11, negative_map_uuid, expired, &observation) ==
           MapAnvilRecipeProbeStatus::Observed);
    assert(observation.map_uuid == negative_map_uuid &&
           observation.recipe_network_id == 731);
}
