#pragma once

#include <cstdint>
#include <string_view>

namespace build_import {

// Debug-build-only, one-process-lifetime observation of the first player-
// opened type-5 candidate window. These functions are no-ops in release.
// They never open a UI, mutate a packet, issue a command, or send a request.
void InitMapAnvilDebugBridge() noexcept;
void ObserveMapAnvilDebugInbound(std::string_view packet) noexcept;
// Called only after the manual outbound observer has verified a single Place
// from hotbar container 29 or main-inventory container 30 into anvil input
// container 0, slot 1. Read-only.
void ObserveMapAnvilDebugOutboundInput(int32_t request_id,
                                       int32_t source_network_stack_id) noexcept;
void ObserveMapAnvilDebugOutboundRename(int32_t request_id,
                                        uint32_t observed_recipe_id,
                                        uint32_t custom_name_count) noexcept;
// The manual outbound parser has verified a Place from CreatedOutput 61:50.
// The three scalar words are evidence only; no ID is synthesized from them.
void ObserveMapAnvilDebugOutboundCreatedOutputVariant(
    int32_t request_id, int32_t network_id, int32_t secondary_id,
    int32_t tag) noexcept;
// The only world read and native getter-probe arm happen on the player tick.
void TickMapAnvilDebugBridge() noexcept;
void ClearMapAnvilDebugBridge() noexcept;

}  // namespace build_import
