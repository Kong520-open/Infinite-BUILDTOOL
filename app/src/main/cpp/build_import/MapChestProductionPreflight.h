#ifndef INFINITE_TEXTURE_MAP_CHEST_PRODUCTION_PREFLIGHT_H
#define INFINITE_TEXTURE_MAP_CHEST_PRODUCTION_PREFLIGHT_H

#include "MapChestWindowSession.h"

#include <cstdint>
#include <string>

namespace build_import {

struct MapChestPreflightExpectation {
    uint64_t token = 0;
    std::string world_context;
    // Kept for existing aggregate callers; native dimension pointers are not
    // stable enough to gate this operation and are intentionally ignored.
    uintptr_t dimension_token = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int face = 1;
    uint8_t max_distance_blocks = 0;
};

struct MapChestPreflightObservation {
    bool authorized_game_tick = false;
    std::string world_context;
    // Compatibility field only; native observation leaves this unset.
    uintptr_t dimension_token = 0;
    bool player_position_available = false;
    int32_t player_x = 0;
    int32_t player_y = 0;
    int32_t player_z = 0;
    bool block_available = false;
    std::string block_identifier;
    bool isolated_single_chest = false;
    const void* native_block = nullptr;
};

using MapChestReadPreflightObservation = bool (*)(
    void*, const MapChestWindowRequest&, bool include_block,
    MapChestPreflightObservation*, std::string*);

struct MapChestStrictPreflightContext {
    MapChestPreflightExpectation expected;
    void* read_context = nullptr;
    MapChestReadPreflightObservation read_observation = nullptr;
};

// Reads the actual local player's block coordinates on an authorized game
// tick. Unlike NativeWorldAccess::getLocalPlayerBlockPosition, this never
// falls back to the render camera when the native position is unavailable.
bool ReadVerifiedMapNativePlayerBlockPosition(
    int32_t* x, int32_t* y, int32_t* z, std::string* error = nullptr);

// Also called immediately before sending a numeric window close. This only
// validates the original world/session; reach is checked at open time.
bool VerifyMapChestProductionContext(
    void* context, const MapChestWindowRequest& request,
    std::string* error = nullptr);

// Must be called on the authorized LocalPlayer tick. The returned Block
// pointer is consumed by ClickBlock immediately, never retained by a session.
bool PrepareMapChestProductionOpen(
    void* context, const MapChestWindowRequest& request,
    const void** native_block, std::string* error = nullptr);

#if defined(__ANDROID__)
MapChestNativePreflight MakeNativeMapChestProductionPreflight(
    MapChestStrictPreflightContext* context) noexcept;
#endif

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_CHEST_PRODUCTION_PREFLIGHT_H
