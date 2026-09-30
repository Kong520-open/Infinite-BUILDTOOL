#include "../BuildProjectionOutline.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace build_import;

namespace {

struct TestBlock {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int32_t material = 0;
    bool full = true;
};

const TestBlock* findBlock(const std::vector<TestBlock>& blocks,
                           int32_t x, int32_t y, int32_t z) {
    for (const TestBlock& block : blocks) {
        if (block.x == x && block.y == y && block.z == z) return &block;
    }
    return nullptr;
}

OutlineCell sample(const std::vector<TestBlock>& blocks, const TestBlock& current,
                   const OutlineOffset& offset) {
    const TestBlock* neighbor = findBlock(
        blocks, current.x + offset.x, current.y + offset.y, current.z + offset.z);
    return {
        neighbor != nullptr && neighbor->full,
        neighbor != nullptr && neighbor->full && neighbor->material == current.material,
    };
}

uint16_t baseMaskFor(const std::vector<TestBlock>& blocks, const TestBlock& current) {
    if (!current.full) return kProjectionOutlineBaseMask;
    const OutlineCell current_cell{true, true};
    uint16_t mask = 0;
    for (std::size_t edge_index = 0; edge_index < kProjectionOutlineEdges.size();
         ++edge_index) {
        const OutlineEdgeNeighborhood& edge = kProjectionOutlineEdges[edge_index];
        const OutlineCell side_a = sample(blocks, current, edge.side_a);
        const OutlineCell side_b = sample(blocks, current, edge.side_b);
        const OutlineCell diagonal = sample(blocks, current, outlineDiagonalOffset(edge));
        if (outlineEdgeVisibleForOwner(
                edge.current_quadrant, current_cell, side_a, side_b, diagonal)) {
            mask |= static_cast<uint16_t>(1U << static_cast<uint8_t>(edge_index));
        }
    }
    return mask;
}

uint8_t cutMaskFor(const std::vector<TestBlock>& blocks, const TestBlock& current) {
    std::array<OutlineCell, 4> neighbors{};
    for (std::size_t index = 0; index < neighbors.size(); ++index) {
        neighbors[index] = sample(blocks, current, kProjectionHorizontalCutNeighbors[index]);
    }
    return outlineHorizontalCutMask(current.full, neighbors);
}

unsigned bitCount(uint16_t value) {
    unsigned count = 0;
    while (value != 0) {
        count += value & 1U;
        value = static_cast<uint16_t>(value >> 1U);
    }
    return count;
}

unsigned baseSegmentCount(const std::vector<TestBlock>& blocks) {
    unsigned count = 0;
    for (const TestBlock& block : blocks) count += bitCount(baseMaskFor(blocks, block));
    return count;
}

std::vector<TestBlock> horizontalRectangle(int32_t width, int32_t depth) {
    std::vector<TestBlock> blocks;
    for (int32_t z = 0; z < depth; ++z) {
        for (int32_t x = 0; x < width; ++x) blocks.push_back({x, 0, z, 1, true});
    }
    return blocks;
}

std::vector<TestBlock> thickLShape() {
    std::vector<TestBlock> blocks;
    for (int32_t y = 0; y < 3; ++y) {
        for (int32_t z = 0; z < 3; ++z) {
            for (int32_t x = 0; x < 3; ++x) {
                if (x != 2 || z != 2) blocks.push_back({x, y, z, 1, true});
            }
        }
    }
    return blocks;
}

void assertBitsClear(uint16_t mask, std::initializer_list<uint16_t> bits) {
    for (const uint16_t bit : bits) assert((mask & bit) == 0);
}

}  // namespace

int main() {
    static_assert(kProjectionOutlineEdges.size() == 12);
    static_assert(kProjectionHorizontalCutNeighbors.size() == 4);
    static_assert(packProjectionOutlineMask(0xffffU, 0xffU) == 0xffffU);
    static_assert(packProjectionOutlineMask(kOutlineEdgeXMinYMinZ, 0x05U) ==
                  static_cast<uint16_t>(kOutlineEdgeXMinYMinZ | kOutlineCutMinZ |
                                        kOutlineCutMinX));

    for (uint8_t occupied = 1; occupied < 16; ++occupied) {
        uint8_t owner_count = 0;
        uint8_t candidates = occupied;
        if (occupied == 0x07U) candidates = 0x06U;
        if (occupied == 0x0bU) candidates = 0x09U;
        if (occupied == 0x0dU) candidates = 0x09U;
        if (occupied == 0x0eU) candidates = 0x06U;
        uint8_t expected_owner = 0;
        while ((candidates & static_cast<uint8_t>(1U << expected_owner)) == 0) {
            ++expected_owner;
        }
        assert(outlineOwnerQuadrant(occupied) == expected_owner);
        for (uint8_t quadrant = 0; quadrant < 4; ++quadrant) {
            owner_count += ownsOutlineEdge(occupied, quadrant) ? 1U : 0U;
        }
        assert(owner_count == 1);
    }
    assert(outlineOwnerQuadrant(0) == kProjectionOutlineNoOwner);

    const std::vector<TestBlock> single{{0, 0, 0, 1, true}};
    assert(baseMaskFor(single, single.front()) == kProjectionOutlineBaseMask);
    assert(baseSegmentCount(single) == 12);

    const std::vector<TestBlock> same_pair{{0, 0, 0, 1, true},
                                            {1, 0, 0, 1, true}};
    assert(baseSegmentCount(same_pair) == 16);
    assertBitsClear(baseMaskFor(same_pair, same_pair[0]),
                    {kOutlineEdgeYMaxXMinZ, kOutlineEdgeYMaxXMaxZ,
                     kOutlineEdgeZMaxXMinY, kOutlineEdgeZMaxXMaxY});
    assertBitsClear(baseMaskFor(same_pair, same_pair[1]),
                    {kOutlineEdgeYMinXMinZ, kOutlineEdgeYMinXMaxZ,
                     kOutlineEdgeZMinXMinY, kOutlineEdgeZMinXMaxY});

    const std::vector<TestBlock> different_pair{{0, 0, 0, 1, true},
                                                 {1, 0, 0, 2, true}};
    assert(baseSegmentCount(different_pair) == 20);
    const uint16_t material_seam = static_cast<uint16_t>(
        baseMaskFor(different_pair, different_pair[0]) &
        (kOutlineEdgeYMaxXMinZ | kOutlineEdgeYMaxXMaxZ |
         kOutlineEdgeZMaxXMinY | kOutlineEdgeZMaxXMaxY));
    assert(bitCount(material_seam) == 4);

    const std::vector<TestBlock> square = horizontalRectangle(2, 2);
    assert(baseSegmentCount(square) == 20);
    const std::vector<TestBlock> large_square = horizontalRectangle(3, 3);
    assert(baseSegmentCount(large_square) == 28);

    const std::vector<TestBlock> l_shape{{0, 0, 0, 1, true},
                                         {1, 0, 0, 1, true},
                                         {0, 0, 1, 1, true}};
    assert(baseSegmentCount(l_shape) == 22);
    // The missing (1,0,1) cell leaves a concave vertical edge at (1,*,1).
    assert((baseMaskFor(l_shape, l_shape[0]) & kOutlineEdgeYMaxXMaxZ) == 0);
    assert((baseMaskFor(l_shape, l_shape[1]) & kOutlineEdgeYMinXMaxZ) != 0);

    const std::vector<TestBlock> thick_l_shape = thickLShape();
    const TestBlock* hidden_diagonal = findBlock(thick_l_shape, 1, 1, 1);
    const TestBlock* owner_side = findBlock(thick_l_shape, 2, 1, 1);
    const TestBlock* other_side = findBlock(thick_l_shape, 1, 1, 2);
    assert(hidden_diagonal != nullptr && owner_side != nullptr && other_side != nullptr);
    assert((baseMaskFor(thick_l_shape, *hidden_diagonal) & kOutlineEdgeYMaxXMaxZ) == 0);
    assert((baseMaskFor(thick_l_shape, *owner_side) & kOutlineEdgeYMinXMaxZ) != 0);
    assert((baseMaskFor(thick_l_shape, *other_side) & kOutlineEdgeYMaxXMinZ) == 0);

    const std::vector<TestBlock> vertical_pair{{0, 0, 0, 1, true},
                                                {0, 1, 0, 1, true}};
    assert(baseSegmentCount(vertical_pair) == 16);
    assertBitsClear(baseMaskFor(vertical_pair, vertical_pair[0]),
                    {kOutlineEdgeXMaxYMinZ, kOutlineEdgeXMaxYMaxZ,
                     kOutlineEdgeZMaxXMaxY, kOutlineEdgeZMinXMaxY});
    assertBitsClear(baseMaskFor(vertical_pair, vertical_pair[1]),
                    {kOutlineEdgeXMinYMinZ, kOutlineEdgeXMinYMaxZ,
                     kOutlineEdgeZMaxXMinY, kOutlineEdgeZMinXMinY});

    unsigned cut_segments = 0;
    for (const TestBlock& block : large_square) {
        cut_segments += bitCount(cutMaskFor(large_square, block));
    }
    assert(cut_segments == 12);
    assert(cutMaskFor(large_square, *findBlock(large_square, 1, 0, 1)) == 0);
    assert(bitCount(cutMaskFor(large_square, *findBlock(large_square, 0, 0, 0))) == 2);

    const std::vector<TestBlock> non_full_neighbor{{0, 0, 0, 1, true},
                                                    {1, 0, 0, 1, false}};
    assert(baseMaskFor(non_full_neighbor, non_full_neighbor[0]) ==
           kProjectionOutlineBaseMask);
    assert(baseMaskFor(non_full_neighbor, non_full_neighbor[1]) ==
           kProjectionOutlineBaseMask);
    assert(baseSegmentCount(non_full_neighbor) == 24);

    const OutlineCell full_current{true, true};
    const OutlineCell non_full_same{false, true};
    const OutlineCell empty{};
    assert(outlineEdgeIsFeature(full_current, non_full_same, empty, empty));
    assert(!outlineEdgeIsFeature(non_full_same, full_current, empty, empty));
    return 0;
}
