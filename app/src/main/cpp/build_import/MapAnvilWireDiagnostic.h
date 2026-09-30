#ifndef INFINITE_TEXTURE_MAP_ANVIL_WIRE_DIAGNOSTIC_H
#define INFINITE_TEXTURE_MAP_ANVIL_WIRE_DIAGNOSTIC_H

#include "ItemExtraMapMetadata.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace build_import {

// Read-only, explicitly armed diagnostics for one player-opened anvil. A
// caller must first verify the block at these coordinates is an anvil in the
// current world. The observer neither opens a screen nor suppresses packets.
struct MapAnvilDiagnosticTarget {
    uint64_t ticket = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int64_t expected_map_uuid = -1;
    bool anvil_block_confirmed = false;
};

enum class MapAnvilDiagnosticEventKind : uint8_t {
    None, Open, Content, Slot, Close,
};

struct MapAnvilDiagnosticMapItem {
    uint16_t slot = 0;
    int32_t runtime_item_id = 0;
    uint16_t count = 0;
    bool has_network_stack_id = false;
    int32_t network_stack_id = 0;
    int64_t map_uuid = -1;
    MapItemNameStatus name_status = MapItemNameStatus::Missing;
    MapItemNameSource name_source = MapItemNameSource::None;
    std::string name;
};

struct MapAnvilDiagnosticEvent {
    MapAnvilDiagnosticEventKind kind = MapAnvilDiagnosticEventKind::None;
    uint64_t ticket = 0;
    uint8_t window_id = 0;
    uint8_t container_type = 0;  // observed, never assumed to mean anvil
    int32_t x = 0, y = 0, z = 0;  // only for Open
    uint32_t inventory_id = 0;   // window ID or player inventory ID 0
    uint32_t slot_count = 0;
    uint16_t changed_slot = 0;   // only for Slot
    bool has_full_container_name = false;  // parsed InventoryContent tail
    uint8_t full_container_name = 0;
    bool has_dynamic_container_id = false;
    uint32_t dynamic_container_id = 0;
    std::array<MapAnvilDiagnosticMapItem, 8> matching_maps{};
    uint8_t matching_map_count = 0;
};

// Correlates a native inventory/verified wire map UUID with the network stack
// ID in the observed manual input Place. Slot number alone is not identity:
// the item can move between a chest, hotbar and anvil before the rename.
enum class MapAnvilIdentityMatch : uint8_t {
    Missing, Unique, Ambiguous,
};

class MapAnvilMapIdentityIndex final {
public:
    void Observe(int32_t network_stack_id, int64_t map_uuid) noexcept;
    MapAnvilIdentityMatch Resolve(int32_t network_stack_id,
                                  int64_t* map_uuid) const noexcept;
    size_t Size() const noexcept { return count_; }

private:
    struct Entry {
        int32_t network_stack_id = 0;
        int64_t map_uuid = -1;
        bool ambiguous = false;
    };
    std::array<Entry, 64> entries_{};
    size_t count_ = 0;
    bool overflowed_ = false;
};

enum class MapAnvilDiagnosticStatus : uint8_t {
    Off, AwaitingOpen, WindowObserved, Closed, Expired, Exhausted,
};

// `Observe` may be called at the existing receive-hook observation point. It
// returns true only for an in-scope event. The caller may log its fields, but
// must always pass the original packet on to the stock client. The 30-second
// and 32-event bounds prevent an old manual window from becoming evidence for
// a later automatic rename. Do not arm this for automatic workflow dispatch.
class MapAnvilWireDiagnostic final {
public:
    // Debug-only one-shot candidate. The first type-5 ContainerOpen is only
    // evidence of a candidate window, NOT proof that the world block is an
    // anvil. The owner must verify the block on the game tick before arming
    // any native recipe probe. This method never opens or hides a window.
    bool ArmDebugFirstType5Candidate() noexcept;
    bool Arm(const MapAnvilDiagnosticTarget& target,
             std::chrono::steady_clock::time_point now) noexcept;
    void Disarm(uint64_t ticket) noexcept;
    MapAnvilDiagnosticStatus Status(
        uint64_t ticket, std::chrono::steady_clock::time_point now) noexcept;
    bool Observe(std::string_view packet,
                 std::chrono::steady_clock::time_point now,
                 MapAnvilDiagnosticEvent* event) noexcept;

private:
    void expireLocked(std::chrono::steady_clock::time_point now) noexcept;

    std::mutex mutex_;
    MapAnvilDiagnosticTarget target_{};
    uint64_t last_ticket_ = 0;
    std::chrono::steady_clock::time_point deadline_{};
    MapAnvilDiagnosticStatus status_ = MapAnvilDiagnosticStatus::Off;
    uint8_t window_id_ = 0;
    uint8_t window_type_ = 0;
    uint8_t event_count_ = 0;
    bool debug_candidate_mode_ = false;
    bool debug_candidate_consumed_ = false;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_ANVIL_WIRE_DIAGNOSTIC_H
