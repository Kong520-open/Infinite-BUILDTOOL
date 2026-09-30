#pragma once

#include <cstdint>

namespace build_import {

class MapVisibleAnvilWindowSession;

// Exact-build layout observed in the anvil screen constructor and its nested
// coordinate controller. This is only the structural part of the proof: the
// caller must separately verify the live world/dimension and packet capture.
struct MapNativeAnvilWindowFields {
    int32_t capture_x = 0, capture_y = 0, capture_z = 0;
    uint8_t capture_id = 0, capture_type = 0;
    int32_t screen_x = 0, screen_y = 0, screen_z = 0;
    uint32_t screen_target_kind = 0;
    int32_t controller_x = 0, controller_y = 0, controller_z = 0;
    uint8_t controller_id = 0, controller_type = 0;
};

constexpr bool MatchMapNativeAnvilWindowFields(
    const MapNativeAnvilWindowFields& fields) noexcept {
    return fields.capture_id != 0U && fields.capture_id != 0xFFU &&
        fields.capture_type == 5U && fields.screen_target_kind == 1U &&
        fields.controller_id == fields.capture_id &&
        fields.controller_type == 5U &&
        fields.screen_x == fields.capture_x &&
        fields.screen_y == fields.capture_y &&
        fields.screen_z == fields.capture_z &&
        fields.controller_x == fields.capture_x &&
        fields.controller_y == fields.capture_y &&
        fields.controller_z == fields.capture_z;
}

// Read-only, one-window-at-a-time diagnostic. A constructor hit does not prove
// that the manager belongs to the intended window; the caller must separately
// match the live type-5 ContainerOpen/Content capture before interpreting it.
struct MapNativeAnvilManagerSnapshot {
    uint64_t ticket = 0;
    uint32_t constructor_hits = 0;
    uint32_t destructor_hits = 0;
    uint32_t screen_constructor_hits = 0;
    uint32_t screen_destructor_hits = 0;
    bool live = false;
    bool screen_live = false;
    bool ambiguous = false;
    bool screen_ambiguous = false;
    bool expired = false;
    bool vtable_matches = false;
    bool screen_vtable_matches = false;
    bool screen_manager_matches = false;
};

// A previous screen is retired only after the exact object's complete C++
// destructor reached its final base-destructor epilogue. The deleting wrapper
// is not used by every ownership path, so its completion is diagnostic only.
// Observing the destructor entry or letting the lease expire is insufficient.
struct MapNativeAnvilRearmEvidence {
    bool explicitly_disarmed = false;
    bool unowned_constructor_observed = false;
    bool manager_vtable_matches = false;
    bool screen_vtable_matches = false;
    bool screen_manager_matches = false;
    bool ambiguous = false;
    uint32_t manager_constructors = 0;
    uint32_t screen_constructors = 0;
    uint32_t manager_destructors = 0;
    uint32_t screen_destructors = 0;
    uint32_t manager_complete_destructor_epilogues = 0;
    uint32_t screen_complete_destructor_epilogues = 0;
    uint32_t manager_delete_completions = 0;
    uint32_t screen_delete_completions = 0;
    uint32_t callbacks_in_flight = 0;
};

constexpr bool CanSequentiallyRearmMapNativeAnvilProbe(
    const MapNativeAnvilRearmEvidence& evidence) noexcept {
    return evidence.explicitly_disarmed &&
        !evidence.unowned_constructor_observed && !evidence.ambiguous &&
        evidence.manager_vtable_matches &&
        evidence.screen_vtable_matches &&
        evidence.screen_manager_matches &&
        evidence.manager_constructors == 1U &&
        evidence.screen_constructors == 1U &&
        evidence.manager_destructors == 1U &&
        evidence.screen_destructors == 1U &&
        evidence.manager_complete_destructor_epilogues == 1U &&
        evidence.screen_complete_destructor_epilogues == 1U &&
        evidence.manager_delete_completions <= 1U &&
        evidence.screen_delete_completions <= 1U &&
        evidence.callbacks_in_flight == 0U;
}

// A strictly increasing ticket prevents a delayed caller using an old ticket
// from reading a newer session's screen snapshot.
constexpr bool IsFreshMapNativeAnvilProbeTicket(uint64_t last_ticket,
                                                 uint64_t next_ticket) noexcept {
    return next_ticket != 0U && next_ticket > last_ticket;
}

// Installs exact-build ARM64 lifecycle instrumentation before an automated
// native anvil window is opened. It can be armed again only after explicit
// disarm and the complete-destructor epilogues of both tracked objects have
// been observed. This never calls a manager method, sends a packet, or writes
// to the game object. Hooks remain installed after disarm.
bool ArmMapNativeAnvilManagerProbe(uintptr_t minecraft_base,
                                   uint64_t ticket) noexcept;
void DisarmMapNativeAnvilManagerProbe(uint64_t ticket) noexcept;
bool ReadMapNativeAnvilManagerProbe(uintptr_t minecraft_base,
                                    uint64_t ticket,
                                    MapNativeAnvilManagerSnapshot* output) noexcept;
bool ReadMapNativeAnvilProbeRetirement(uintptr_t minecraft_base,
                                      uint64_t last_ticket,
                                      MapNativeAnvilRearmEvidence* output) noexcept;

// Returns the captured screen only to code already running on the same native
// thread that constructed both screen and manager. The pointer must not be
// retained or accessed after that synchronous call returns. This is an
// internal bridge primitive, not proof of a matching server window by itself.
bool BorrowMapNativeAnvilScreenForCurrentThread(uintptr_t minecraft_base,
                                                uint64_t ticket,
                                                uintptr_t* screen) noexcept;

// Borrow the exact-build anvil manager only after a fresh, type-5, three-slot
// numeric ContainerOpen/Content capture, world/dimension preflight, and native
// screen/controller coordinate + window-ID match. Callers must use the pointer
// synchronously on this constructor thread and must not retain it across a
// tick or request. This may fail if the stock client has not populated its
// nested controller's ID; 0xFF is never guessed from a packet.
bool BorrowMapNativeAnvilManagerForVerifiedWindow(
    uintptr_t minecraft_base, uint64_t ticket, uint64_t now_ms,
    const MapVisibleAnvilWindowSession& session,
    uintptr_t* manager) noexcept;

}  // namespace build_import
