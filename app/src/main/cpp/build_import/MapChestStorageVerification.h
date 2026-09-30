#ifndef INFINITE_TEXTURE_MAP_CHEST_STORAGE_VERIFICATION_H
#define INFINITE_TEXTURE_MAP_CHEST_STORAGE_VERIFICATION_H

#include "ContainerCaptureMailbox.h"

#include <cstdint>
#include <string_view>

namespace build_import {

enum class FilledMapChestReopenResult : uint8_t {
    InvalidCapture,
    MissingOrDifferentItem,
    Confirmed,
};

enum class NamedFilledMapChestReopenResult : uint8_t {
    InvalidCapture,
    MissingOrDifferentItem,
    NameUnconfirmed,
    Confirmed,
};

// Reopening creates a different server window. Verify the fresh capture token,
// exact block coordinates, single-chest shape, and the same item runtime ID
// plus map_uuid in the submitted slot. A successful ItemStackResponse alone is
// not confirmation.
FilledMapChestReopenResult VerifyFilledMapInReopenedChest(
    const ContainerCaptureResult& capture, uint64_t reopen_token,
    int32_t chest_x, int32_t chest_y, int32_t chest_z,
    uint8_t submitted_slot, int64_t expected_map_uuid,
    int32_t expected_runtime_item_id);

// Stronger completion gate for a renamed tile. `expected_name_source` must
// first be confirmed against a live manually renamed map on this game build;
// ItemExtra's two possible NBT paths are only candidates until then. A map
// UUID match alone cannot establish that the row/column title survived.
NamedFilledMapChestReopenResult VerifyNamedFilledMapInReopenedChest(
    const ContainerCaptureResult& capture, uint64_t reopen_token,
    int32_t chest_x, int32_t chest_y, int32_t chest_z,
    uint8_t submitted_slot, int64_t expected_map_uuid,
    int32_t expected_runtime_item_id, std::string_view expected_name,
    MapItemNameSource expected_name_source);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_CHEST_STORAGE_VERIFICATION_H
