#ifndef INFINITE_TEXTURE_BUILD_PROJECTION_OUTLINE_H
#define INFINITE_TEXTURE_BUILD_PROJECTION_OUTLINE_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace build_import {

constexpr uint8_t kProjectionOutlineEdgeCount = 12;
constexpr uint16_t kProjectionOutlineBaseMask = 0x0fffU;
constexpr uint16_t kProjectionOutlineCutMask = 0xf000U;
constexpr uint8_t kProjectionOutlineNoOwner = 0xffU;

// These bits follow the line-index order used by the projection cube mesh.
enum ProjectionOutlineEdgeBit : uint16_t {
    kOutlineEdgeXMinYMinZ = 1U << 0U,
    kOutlineEdgeYMaxXMinZ = 1U << 1U,
    kOutlineEdgeXMaxYMinZ = 1U << 2U,
    kOutlineEdgeYMinXMinZ = 1U << 3U,
    kOutlineEdgeXMinYMaxZ = 1U << 4U,
    kOutlineEdgeYMaxXMaxZ = 1U << 5U,
    kOutlineEdgeXMaxYMaxZ = 1U << 6U,
    kOutlineEdgeYMinXMaxZ = 1U << 7U,
    kOutlineEdgeZMinXMinY = 1U << 8U,
    kOutlineEdgeZMaxXMinY = 1U << 9U,
    kOutlineEdgeZMaxXMaxY = 1U << 10U,
    kOutlineEdgeZMinXMaxY = 1U << 11U,
};

enum ProjectionOutlineCutBit : uint16_t {
    kOutlineCutMinZ = 1U << 12U,
    kOutlineCutMaxZ = 1U << 13U,
    kOutlineCutMinX = 1U << 14U,
    kOutlineCutMaxX = 1U << 15U,
};

struct OutlineCell {
    bool full = false;
    // This is only consulted when full is true and is relative to the current cell.
    bool same_material = false;
};

struct OutlineOffset {
    int8_t x = 0;
    int8_t y = 0;
    int8_t z = 0;
};

struct OutlineEdgeNeighborhood {
    OutlineOffset side_a;
    OutlineOffset side_b;
    // Quadrants use bit 0 for the positive side_a axis and bit 1 for the
    // positive side_b axis. The other cells are current^1, current^2, current^3.
    uint8_t current_quadrant = 0;
};

// Neighbor offsets point out of the current cube at each local mesh edge.
inline constexpr std::array<OutlineEdgeNeighborhood, kProjectionOutlineEdgeCount>
    kProjectionOutlineEdges{{
        {{0, -1, 0}, {0, 0, -1}, 3},  // X, min Y, min Z
        {{1, 0, 0}, {0, 0, -1}, 2},   // Y, max X, min Z
        {{0, 1, 0}, {0, 0, -1}, 2},   // X, max Y, min Z
        {{-1, 0, 0}, {0, 0, -1}, 3},  // Y, min X, min Z
        {{0, -1, 0}, {0, 0, 1}, 1},   // X, min Y, max Z
        {{1, 0, 0}, {0, 0, 1}, 0},    // Y, max X, max Z
        {{0, 1, 0}, {0, 0, 1}, 0},    // X, max Y, max Z
        {{-1, 0, 0}, {0, 0, 1}, 1},   // Y, min X, max Z
        {{-1, 0, 0}, {0, -1, 0}, 3},  // Z, min X, min Y
        {{1, 0, 0}, {0, -1, 0}, 2},   // Z, max X, min Y
        {{1, 0, 0}, {0, 1, 0}, 0},    // Z, max X, max Y
        {{-1, 0, 0}, {0, 1, 0}, 1},   // Z, min X, max Y
    }};

inline constexpr std::array<OutlineOffset, 4> kProjectionHorizontalCutNeighbors{{
    {0, 0, -1},
    {0, 0, 1},
    {-1, 0, 0},
    {1, 0, 0},
}};

// Three-cell concave cases exclude the cell diagonal to the gap. It has no
// exposed face beside the edge and can be stored in an interior-only batch.
inline constexpr std::array<uint8_t, 16> kProjectionOutlineOwnerByOccupancy{{
    kProjectionOutlineNoOwner, 0, 1, 0, 2, 0, 1, 1,
    3,                         0, 1, 0, 2, 0, 1, 0,
}};

constexpr OutlineOffset outlineDiagonalOffset(
    const OutlineEdgeNeighborhood& edge) noexcept {
    return {
        static_cast<int8_t>(edge.side_a.x + edge.side_b.x),
        static_cast<int8_t>(edge.side_a.y + edge.side_b.y),
        static_cast<int8_t>(edge.side_a.z + edge.side_b.z),
    };
}

// A material region produces a line for convex, concave, and diagonal-contact
// topology. Two adjacent cells form a flat surface, so only a material boundary
// remains visible there. Four full cells completely surround the edge.
constexpr bool outlineEdgeIsFeature(const OutlineCell& current,
                                    const OutlineCell& side_a,
                                    const OutlineCell& side_b,
                                    const OutlineCell& diagonal) noexcept {
    if (!current.full) return false;
    const uint8_t full_count = static_cast<uint8_t>(1U +
        static_cast<uint8_t>(side_a.full) + static_cast<uint8_t>(side_b.full) +
        static_cast<uint8_t>(diagonal.full));
    if (full_count == 1U || full_count == 3U) return true;
    if (full_count == 4U) return false;
    if (diagonal.full) return true;
    const OutlineCell& adjacent = side_a.full ? side_a : side_b;
    return !adjacent.same_material;
}

constexpr uint8_t outlineOccupancyMask(uint8_t current_quadrant,
                                       const OutlineCell& current,
                                       const OutlineCell& side_a,
                                       const OutlineCell& side_b,
                                       const OutlineCell& diagonal) noexcept {
    if (current_quadrant > 3U) return 0;
    uint8_t mask = 0;
    if (current.full) mask |= static_cast<uint8_t>(1U << current_quadrant);
    if (side_a.full) mask |= static_cast<uint8_t>(1U << (current_quadrant ^ 1U));
    if (side_b.full) mask |= static_cast<uint8_t>(1U << (current_quadrant ^ 2U));
    if (diagonal.full) mask |= static_cast<uint8_t>(1U << (current_quadrant ^ 3U));
    return mask;
}

constexpr uint8_t outlineOwnerQuadrant(uint8_t occupied_quadrants) noexcept {
    return kProjectionOutlineOwnerByOccupancy[occupied_quadrants & 0x0fU];
}

constexpr bool ownsOutlineEdge(uint8_t occupied_quadrants,
                               uint8_t current_quadrant) noexcept {
    return current_quadrant < 4U &&
        outlineOwnerQuadrant(occupied_quadrants) == current_quadrant;
}

constexpr bool outlineEdgeVisibleForOwner(uint8_t current_quadrant,
                                          const OutlineCell& current,
                                          const OutlineCell& side_a,
                                          const OutlineCell& side_b,
                                          const OutlineCell& diagonal) noexcept {
    return outlineEdgeIsFeature(current, side_a, side_b, diagonal) &&
        ownsOutlineEdge(outlineOccupancyMask(
                            current_quadrant, current, side_a, side_b, diagonal),
                        current_quadrant);
}

// Returns bits 0..3 in -Z,+Z,-X,+X order. Callers store them in bits 12..15.
constexpr uint8_t outlineHorizontalCutMask(
    bool current_full, const std::array<OutlineCell, 4>& neighbors) noexcept {
    if (!current_full) return 0;
    uint8_t mask = 0;
    for (std::size_t side = 0; side < neighbors.size(); ++side) {
        const OutlineCell& neighbor = neighbors[side];
        if (!neighbor.full || !neighbor.same_material) {
            mask |= static_cast<uint8_t>(1U << static_cast<uint8_t>(side));
        }
    }
    return mask;
}

constexpr uint16_t packProjectionOutlineMask(uint16_t base_edges,
                                             uint8_t horizontal_cut_edges) noexcept {
    return static_cast<uint16_t>((base_edges & kProjectionOutlineBaseMask) |
                                 (static_cast<uint16_t>(horizontal_cut_edges & 0x0fU)
                                  << 12U));
}

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BUILD_PROJECTION_OUTLINE_H
