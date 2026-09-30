#ifndef INFINITE_TEXTURE_MAP_VISIBLE_ANVIL_PRODUCTION_PREFLIGHT_H
#define INFINITE_TEXTURE_MAP_VISIBLE_ANVIL_PRODUCTION_PREFLIGHT_H

#include "MapVisibleAnvilWindowSession.h"

#include <cstdint>
#include <string>

namespace build_import {

// The owning map task must fill every expected identity. In particular, do
// not pass an inferred/default world name. A range
// of 1..4 block-coordinate units is intentionally stricter than the game's
// version-dependent maximum reach. The tool can move the player closer.
struct MapVisibleAnvilPreflightExpectation {
    uint64_t token = 0;
    std::string world_context;
    // Retained for callers that still populate it; this preflight does not
    // require or compare native dimension pointers.
    uintptr_t dimension_token = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int face = 1;
    uint8_t max_distance_blocks = 0;
};

struct MapVisibleAnvilPreflightObservation {
    bool authorized_game_tick = false;
    std::string world_context;
    // Compatibility only; the native reader leaves this unset.
    uintptr_t dimension_token = 0;
    bool player_position_available = false;
    int32_t player_x = 0;
    int32_t player_y = 0;
    int32_t player_z = 0;
    bool block_available = false;
    std::string block_identifier;
    // A NativeWorldReader::getBlockView type_token from this exact coordinate.
    // It is passed to ClickBlock immediately on the same game tick, never
    // stored in the window request or retained by this adapter.
    const void* native_block = nullptr;
};

using MapVisibleAnvilReadObservation = bool (*)(
    void*, const MapVisibleAnvilWindowRequest&, bool include_block,
    MapVisibleAnvilPreflightObservation*, std::string*);

struct MapVisibleAnvilStrictPreflightContext {
    MapVisibleAnvilPreflightExpectation expected;
    void* read_context = nullptr;
    MapVisibleAnvilReadObservation read_observation = nullptr;
};

// Host-testable callbacks for MapVisibleAnvilNativePreflight. verify_context
// checks the full world context but deliberately does not enforce reach: if the
// player moves after opening, the current world's window can still be closed.
bool VerifyMapVisibleAnvilProductionContext(
    void* context, const MapVisibleAnvilWindowRequest& request,
    std::string* error = nullptr);
bool PrepareMapVisibleAnvilProductionOpen(
    void* context, const MapVisibleAnvilWindowRequest& request,
    const void** native_block, std::string* error = nullptr);

#if defined(__ANDROID__)
// Fills the observation callback with the current authorized local-player
// tick reader. The returned adapter must remain alive alongside its Context
// until the window session has closed or aborted.
MapVisibleAnvilNativePreflight MakeNativeMapVisibleAnvilProductionPreflight(
    MapVisibleAnvilStrictPreflightContext* context) noexcept;
#endif

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_VISIBLE_ANVIL_PRODUCTION_PREFLIGHT_H
