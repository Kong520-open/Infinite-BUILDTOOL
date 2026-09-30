#ifndef INFINITE_TEXTURE_MAP_INVENTORY_TRANSFER_H
#define INFINITE_TEXTURE_MAP_INVENTORY_TRANSFER_H

#include "ProjectionPrinterInventoryMover.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace build_import {

struct MapInventorySlot {
    bool occupied = false;
    bool native_occupied = false;
    bool empty_map = false;
    bool filled_map = false;
    uint16_t count = 0;
    bool has_network_stack_id = false;
    int32_t network_stack_id = 0;
    bool has_map_uuid = false;
    int64_t map_uuid = -1;
};

struct MapInventorySnapshot {
    bool ready = false;
    bool network_ready = false;
    int32_t selected_hotbar_slot = -1;
    std::array<MapInventorySlot, kProjectionPrinterLiveInventorySlotCount> slots{};
};

// Reads the game's live ItemStacks through its native InventorySlot serializer,
// resolves their runtime IDs, and reads map_uuid only for confirmed filled
// maps. Only call on the LocalPlayer tick; no packet is sent or consumed.
bool ReadMapInventorySnapshot(MapInventorySnapshot* output,
                              std::string* error = nullptr);

class MapInventoryTransfer {
public:
    enum class Kind : uint8_t { Empty, Filled };
    enum class Result : uint8_t { Waiting, Ready, Missing, Failed };
    struct ReadyItem {
        int32_t hotbar_slot = -1;
        int32_t network_stack_id = 0;
        uint16_t count = 0;
    };
    struct NamedFilledMapIdentity {
        int32_t runtime_item_id = 0;
        int64_t map_uuid = -1;
        std::string title;
    };

    // exact_network_stack_id is zero when any blank-map stack is acceptable.
    // The freshly generated filled map must pass its pre-move network ID here
    // so an older map is never selected as texture-update evidence.
    Result tick(Kind kind, int32_t exact_network_stack_id,
                ReadyItem* output, std::string* error,
                const NamedFilledMapIdentity* named_map = nullptr);
    void cancel() noexcept;

private:
    enum class Stage : uint8_t { Idle, Opening, MovePending, Closing, PostMoveReady };
    void finishSession() noexcept;
    bool requestSelection(int32_t slot) const;
    Result fail(std::string* error, const std::string& detail);

    Stage stage_ = Stage::Idle;
    Kind target_kind_ = Kind::Empty;
    uint64_t session_token_ = 0;
    uint64_t presentation_generation_before_open_ = 0;
    uint64_t presentation_generation_ = 0;
    bool presentation_settled_ = false;
    uint8_t close_ticks_remaining_ = 0;
    std::chrono::steady_clock::time_point opened_at_{};
    std::chrono::steady_clock::time_point next_action_at_{};
    std::chrono::steady_clock::time_point move_sent_at_{};
    std::chrono::steady_clock::time_point post_move_ready_at_{};
    ProjectionPrinterInventoryMoveRequest move_{};
    int32_t request_id_ = 0;
    uint64_t response_session_generation_ = 0;
    uint64_t response_generation_before_send_ = 0;
    uint64_t remote_revision_before_send_ = 0;
    uint64_t local_refresh_ticket_ = 0;
    uint64_t correction_ticket_ = 0;
    uint16_t destination_count_before_ = 0;
    int32_t confirmed_destination_network_id_ = 0;
    bool named_map_active_ = false;
    NamedFilledMapIdentity named_map_{};
};

}  // namespace build_import

#endif
