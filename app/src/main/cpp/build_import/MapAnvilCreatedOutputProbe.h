#pragma once

#include "MapAnvilRecipeProbe.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace build_import {

// The native record is observed, never retained. No ItemStack/NBT bytes or
// pointers are copied into this structure.
struct MapAnvilCreatedOutputObservation {
    uint64_t ticket = 0;
    uint32_t site = 0; // 1: constructed record, 2: record before consumption.
    uint8_t callback_index = 0; // Stable 0..31 slot, even if callbacks finish out of order.
    uint32_t kind = 0;
    uint8_t container_name = 0;
    uint32_t dynamic_id = 0;
    uint8_t has_dynamic_id = 0;
    uint8_t slot = 0;
    uint32_t count = 0;
    int32_t network_id = 0;
    int32_t secondary_id = 0;
    int32_t tag = 0;
};

struct MapAnvilCreatedOutputSnapshot {
    struct EntrySample {
        bool installed = false;
        uint32_t calls = 0;
        bool first_ready = false;
        uintptr_t first_receiver = 0;
        uintptr_t first_caller_pc = 0;
    };
    struct HeaderSample {
        bool seen = false;
        uint32_t kind = 0;
        uint8_t container_name = 0;
        uint8_t slot = 0;
    };
    uint32_t generic_attempts = 0;
    uint32_t anvil_attempts = 0;
    uint32_t generic_header_hits = 0;
    uint32_t anvil_header_hits = 0;
    HeaderSample generic_first_header{};
    HeaderSample anvil_first_header{};
    uint32_t callback_count = 0;
    uint32_t unreadable_count = 0;
    uint32_t captured_count = 0;
    std::array<MapAnvilCreatedOutputObservation, 32> records{};
    // Optional entry hooks are independently verified and never gate the
    // existing record probe. A zero count with installed=true is meaningful.
    EntrySample anvil_simulation_entry{};
    EntrySample anvil_type2_entry{};
    EntrySample craft_optional_constructor{};
    uint32_t craft_constructor_recorded = 0;
};

// Testable decoding of only two 24-byte scalar regions of a CreatedOutput
// record: header at +0 and ItemStackNetIdVariant at +0x100. Padding, names,
// ItemStack, NBT and native pointers are intentionally not inspected.
bool DecodeMapAnvilCreatedOutputScalars(
    std::string_view header, std::string_view variant,
    MapAnvilCreatedOutputObservation* output) noexcept;
bool MapAnvilCreatedOutputIsTarget(
    const MapAnvilCreatedOutputObservation& record) noexcept;
bool MapAnvilCreatedOutputStorageIndex(
    uint32_t site, uint32_t site_ordinal, uint8_t* output) noexcept;

// Pure helper used by the bridge to avoid losing a late-ready lower-index
// callback when the snapshot has already contained a higher-index callback.
uint32_t MapAnvilCreatedOutputNewRecordBits(
    const MapAnvilCreatedOutputSnapshot& snapshot,
    uint32_t logged_mask) noexcept;

// Debug/arm64 only. Exact SO fingerprints are checked before Dobby hooks are
// installed. The caller must have observed the same live type-5 window, read
// its anvil block on game tick, and established a unique filled-map UUID.
// Hooks are read-only; they do not send packets, touch the UI, or log.
bool ArmMapAnvilCreatedOutputProbe(
    uintptr_t minecraft_base, uint64_t ticket, int64_t expected_map_uuid,
    const MapAnvilWindowEvidence& window) noexcept;
void DisarmMapAnvilCreatedOutputProbe(uint64_t ticket) noexcept;
bool ReadMapAnvilCreatedOutputProbe(
    uint64_t ticket, MapAnvilCreatedOutputSnapshot* output) noexcept;
// Matches a subsequently observed type-15 action against a constructor's
// original x0. Only the caller PC is returned; no action memory is inspected.
// A match proves this action was constructed at the captured call site, not
// that any particular recipe ID or output identity is valid.
bool LookupMapAnvilCraftCtorCaller(
    uintptr_t action_address, uintptr_t* caller_pc) noexcept;

} // namespace build_import
