#pragma once

#include "CommandBlockSpool.h"

#include <cstdint>
#include <limits>

namespace build_import {

// The server validates command-block edits using the live player position,
// while the client-side position cache is block-aligned.  Keep a full two
// blocks of margin below the observed eight-block server limit so a block
// centre / fractional-player-position difference cannot make an edge record
// fail after the local guard has accepted it.
constexpr int32_t kCommandBlockEditorSafeRadiusBlocks = 6;

struct CommandBlockWriteTeleportTarget {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

inline int32_t commandBlockWriteTeleportY(int32_t block_y) {
    constexpr int32_t kVerticalOffset = 2;
    if (block_y > std::numeric_limits<int32_t>::max() - kVerticalOffset) {
        return std::numeric_limits<int32_t>::max();
    }
    return block_y + kVerticalOffset;
}

// Always anchor a command-block write burst on the actual pending record.
// Grid cells are only a batching/loading optimization; using their geometric
// centre as the player target can put sparse edge records beyond the server's
// strict editor range.
inline CommandBlockWriteTeleportTarget commandBlockWriteTeleportTarget(
        const CommandBlockRecord& record) {
    return {record.x, commandBlockWriteTeleportY(record.y), record.z};
}

inline bool isWithinCommandBlockEditorSafeRadius(int32_t player_x, int32_t player_y,
                                                 int32_t player_z,
                                                 const CommandBlockRecord& record) {
    const int64_t dx = static_cast<int64_t>(player_x) - record.x;
    const int64_t dy = static_cast<int64_t>(player_y) - record.y;
    const int64_t dz = static_cast<int64_t>(player_z) - record.z;
    constexpr int64_t radius = kCommandBlockEditorSafeRadiusBlocks;
    // Bound each term before squaring so deliberately malformed/extreme
    // coordinates cannot overflow signed int64_t arithmetic.
    if (dx < -radius || dx > radius || dy < -radius || dy > radius ||
        dz < -radius || dz > radius) {
        return false;
    }
    return dx * dx + dy * dy + dz * dz <= radius * radius;
}

}  // namespace build_import
