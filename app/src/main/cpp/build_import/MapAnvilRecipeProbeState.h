#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

namespace build_import {

// This is evidence from a native recipe lookup, not permission to craft. In
// particular it does not identify the temporary output SlotInfo/net ID.
struct MapAnvilRecipeObservation {
    uint64_t ticket = 0;
    uint8_t window_id = 0;
    int64_t map_uuid = -1;
    uint32_t recipe_network_id = 0;
};

enum class MapAnvilRecipeProbeStatus : uint8_t {
    Unavailable,
    Armed,
    Observed,
    Ambiguous,
    Expired,
};

// A bounded, fail-closed evidence latch. Native hook callbacks only call
// Observe after the input ItemStack's map_uuid has been read and matched.
class MapAnvilRecipeProbeState final {
public:
    bool Arm(uint64_t ticket, uint8_t window_id, int64_t map_uuid,
             std::chrono::steady_clock::time_point now) noexcept;
    void Disarm(uint64_t ticket) noexcept;
    bool Expects(uint64_t ticket, int64_t map_uuid,
                 std::chrono::steady_clock::time_point now) noexcept;
    void Observe(uint64_t ticket, int64_t map_uuid, uint32_t recipe_network_id,
                 std::chrono::steady_clock::time_point now) noexcept;
    MapAnvilRecipeProbeStatus Read(uint64_t ticket, int64_t map_uuid,
                                  std::chrono::steady_clock::time_point now,
                                  MapAnvilRecipeObservation* output) noexcept;

private:
    void expireLocked(std::chrono::steady_clock::time_point now) noexcept;

    std::mutex mutex_;
    MapAnvilRecipeObservation observation_;
    std::chrono::steady_clock::time_point deadline_{};
    MapAnvilRecipeProbeStatus status_ = MapAnvilRecipeProbeStatus::Unavailable;
    uint64_t last_ticket_ = 0;
};

}  // namespace build_import
