#include "MapChestStorageVerification.h"

#include <array>

namespace build_import {

FilledMapChestReopenResult VerifyFilledMapInReopenedChest(
        const ContainerCaptureResult& capture, uint64_t reopen_token,
        int32_t chest_x, int32_t chest_y, int32_t chest_z,
        uint8_t submitted_slot, int64_t expected_map_uuid,
        int32_t expected_runtime_item_id) {
    if (reopen_token == 0U || capture.token != reopen_token ||
        capture.x != chest_x || capture.y != chest_y || capture.z != chest_z ||
        !capture.error.empty() || !capture.container_opened ||
        capture.container_closed || capture.container_id == 0U ||
        capture.container_id == 0xFFU || capture.container_type != 0U ||
        capture.slot_count != 27U || !capture.has_full_container_name ||
        capture.full_container_name != 0U || capture.has_dynamic_container_id ||
        capture.dynamic_container_id != 0U || submitted_slot >= 27U ||
        expected_map_uuid == -1 || expected_runtime_item_id <= 0) {
        return FilledMapChestReopenResult::InvalidCapture;
    }

    const CapturedContainerItem* submitted_item = nullptr;
    uint32_t matching_uuid_count = 0U;
    std::array<bool, 27U> occupied{};
    for (const CapturedContainerItem& item : capture.items) {
        if (item.slot >= occupied.size() || occupied[item.slot] ||
            item.numeric_id == 0 || item.count == 0U) {
            return FilledMapChestReopenResult::InvalidCapture;
        }
        occupied[item.slot] = true;
        if (item.slot == submitted_slot) {
            if (submitted_item) return FilledMapChestReopenResult::InvalidCapture;
            submitted_item = &item;
        }
        if (item.has_map_uuid && item.map_uuid == expected_map_uuid) {
            ++matching_uuid_count;
        }
    }
    if (matching_uuid_count != 1U || !submitted_item ||
        !submitted_item->has_map_uuid ||
        submitted_item->map_uuid != expected_map_uuid ||
        submitted_item->numeric_id != expected_runtime_item_id ||
        submitted_item->count != 1U ||
        !submitted_item->has_network_stack_id ||
        submitted_item->network_stack_id <= 0) {
        return FilledMapChestReopenResult::MissingOrDifferentItem;
    }
    return FilledMapChestReopenResult::Confirmed;
}

NamedFilledMapChestReopenResult VerifyNamedFilledMapInReopenedChest(
        const ContainerCaptureResult& capture, uint64_t reopen_token,
        int32_t chest_x, int32_t chest_y, int32_t chest_z,
        uint8_t submitted_slot, int64_t expected_map_uuid,
        int32_t expected_runtime_item_id, std::string_view expected_name,
        MapItemNameSource expected_name_source) {
    const FilledMapChestReopenResult base = VerifyFilledMapInReopenedChest(
        capture, reopen_token, chest_x, chest_y, chest_z, submitted_slot,
        expected_map_uuid, expected_runtime_item_id);
    if (base == FilledMapChestReopenResult::InvalidCapture) {
        return NamedFilledMapChestReopenResult::InvalidCapture;
    }
    if (base != FilledMapChestReopenResult::Confirmed) {
        return NamedFilledMapChestReopenResult::MissingOrDifferentItem;
    }
    if (expected_name.empty() || expected_name.size() > 256U ||
        expected_name_source == MapItemNameSource::None) {
        return NamedFilledMapChestReopenResult::NameUnconfirmed;
    }
    for (const CapturedContainerItem& item : capture.items) {
        if (item.slot != submitted_slot) continue;
        return item.name_status == MapItemNameStatus::Present &&
               item.name_source == expected_name_source &&
               item.name_candidate == expected_name
            ? NamedFilledMapChestReopenResult::Confirmed
            : NamedFilledMapChestReopenResult::NameUnconfirmed;
    }
    return NamedFilledMapChestReopenResult::MissingOrDifferentItem;
}

}  // namespace build_import
