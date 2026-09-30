#pragma once

#include <array>
#include <cstdint>

namespace build_import {

// Debug-only, one-window native observation. No click, recipe, or packet is
// submitted. The result event values are copied only while the stock game's
// callback is executing on its owning thread.
struct MapAnvilNativeActionTraceSnapshot {
    uint32_t callback_calls = 0;
    uint32_t recipe_lookup_calls = 0;
    bool result_click_seen = false;
    bool result_click_on_arm_thread = false;
    int32_t result_w1 = -1;
    int32_t result_w3 = -1;
    std::array<uint32_t, 8> recipe_ids{};
    uint8_t recipe_count = 0;
};

// Caller must already have confirmed one live type-5 anvil window, its world
// block, and the source map UUID. Instrumentation is gated by the exact game
// Build ID and instruction fingerprints, and expires after 90 seconds.
bool ArmMapAnvilNativeActionTrace(uintptr_t minecraft_base, uint64_t ticket,
                                  uint8_t window_id, int64_t map_uuid) noexcept;
// Debug-only fallback for a verified manual Place into anvil input when this
// process did not observe ContainerOpen. No window or map identity is inferred:
// only the exact-Build-ID, fingerprinted, live anvil-screen result callback is
// recorded. Never use this weaker evidence to authorize production actions.
bool ArmMapAnvilUnboundNativeActionTrace(uintptr_t minecraft_base,
                                         uint64_t ticket) noexcept;
bool ReadMapAnvilNativeActionTrace(uint64_t ticket,
                                   MapAnvilNativeActionTraceSnapshot* output) noexcept;
void DisarmMapAnvilNativeActionTrace(uint64_t ticket) noexcept;

}  // namespace build_import
