#pragma once

#include "MapAnvilRecipeProbeState.h"

#include <cstdint>
#include <string_view>

namespace build_import {

// Caller-supplied proof from the *same* live world/session. The probe verifies
// the ContainerOpen packet's window ID/type bytes, but cannot yet independently
// establish which numeric type means anvil in this NetEase build. The caller
// must confirm that semantic identity from a separate live anvil observation.
struct MapAnvilWindowEvidence {
    std::string_view container_open_packet;
    uint8_t window_id = 0;
    uint8_t observed_container_type = 0;
    bool anvil_block_and_window_confirmed = false;
};

// Bounded, read-only callback counters for a single armed Debug probe. These
// counts distinguish an unused getter from a failed map identity/copy-site
// correlation without logging stack contents or custom-name text. All counts
// saturate at 255; Release builds never collect them.
struct MapAnvilRecipeProbeDiagnostics {
    uint16_t getter_entries = 0;
    uint16_t getter_guard_rejected = 0;
    uint16_t first_uuid_read = 0;
    uint16_t second_uuid_read = 0;
    uint16_t first_uuid_match = 0;
    uint16_t second_uuid_match = 0;
    uint16_t getter_correlated = 0;
    uint16_t copy_entries = 0;
    uint16_t copy_without_getter = 0;
    uint16_t copy_storage_mismatch = 0;
    uint16_t copy_network_id_unreadable = 0;
    uint16_t copy_observed = 0;
};

// Research-only probe for the map-items getter. Two live manual anvil rename
// requests never entered this getter, so it is not an authorized source for
// an automatic rename recipe. Inert until explicitly called. On ARM64, Arm
// checks exact libminecraftpe.so fingerprints before installing read-only
// Dobby instrumentation. Tickets must increase monotonically. It never
// creates/calls an AnvilContainerScreenSimulation, sends an item request, or
// opens a UI. If no real native getter call occurs, Read stays Armed and the
// automatic rename must remain unavailable.
bool ArmMapAnvilRecipeProbe(uintptr_t minecraft_base, uint64_t ticket,
                            int64_t expected_map_uuid,
                            const MapAnvilWindowEvidence& window) noexcept;
void DisarmMapAnvilRecipeProbe(uint64_t ticket) noexcept;
MapAnvilRecipeProbeStatus ReadMapAnvilRecipeProbe(
    uint64_t ticket, int64_t expected_map_uuid,
    MapAnvilRecipeObservation* observation) noexcept;
bool ReadMapAnvilRecipeProbeDiagnostics(
    uint64_t ticket, MapAnvilRecipeProbeDiagnostics* diagnostics) noexcept;

// Even an Observed value from this getter does not prove the anvil rename
// recipe ID. The actual recipe source, temporary output slot/network ID,
// rename cost, and final server ACK need separate proof before sending.

}  // namespace build_import
