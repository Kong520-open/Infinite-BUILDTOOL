#ifndef INFINITE_TEXTURE_PROJECTION_WORLD_MATCH_RUNTIME_H
#define INFINITE_TEXTURE_PROJECTION_WORLD_MATCH_RUNTIME_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace build_import {

struct NativeBlockSnapshot;

// Modern Bedrock stairs use the non-clockwise weirdo_direction encoding
// (east=0, west=1, south=2, north=3) plus bit 0x04 for the upper half.
// Projection position rotation is clockwise in the X/Z plane, so callers
// must transform this state explicitly rather than adding to the raw aux.
// Returns false for an aux value that is not a representable stair state.
bool RotateProjectionStairAux(uint16_t source_aux, uint8_t rotation_quarters,
                              uint16_t* output) noexcept;

// Resolves the command-compatible stair aux from the public client snapshot.
// It deliberately never falls back to the raw native Block::aux because that
// field is zero for several distinct modern stair states on the supported
// client build.
bool TryResolveProjectionStairAux(const NativeBlockSnapshot& snapshot,
                                  uint16_t* output) noexcept;

// The visual state for one projected source block after the local game-thread
// world reader has inspected its current target coordinate.  Unknown is
// deliberately the default: it keeps the ordinary projection appearance for
// air, unreadable chunks, cold source partitions and source states whose
// expected rotation cannot be proven from the current ABI.
enum class ProjectionWorldMatchState : uint8_t {
    Unknown = 0,
    WrongBlock = 1,
    WrongState = 2,
    Exact = 3,
};

// Projection instance groups use a fixed 16 x 16 x 16 world-space grid.  Keep
// comparison data in that same grid: a large active projection then needs one
// small immutable state page per rendered group rather than a global hash map
// which every visible group would have to rescan.
constexpr int32_t kProjectionWorldMatchGroupSpan = 16;
constexpr size_t kProjectionWorldMatchGroupCellCount =
    static_cast<size_t>(kProjectionWorldMatchGroupSpan) *
    kProjectionWorldMatchGroupSpan * kProjectionWorldMatchGroupSpan;

// Pointer-free world coordinate key retained for source/page work on the game
// tick.  It is deliberately not used by the render snapshot.
struct ProjectionWorldMatchPosition {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const ProjectionWorldMatchPosition& other) const noexcept {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct ProjectionWorldMatchPositionHash {
    size_t operator()(const ProjectionWorldMatchPosition& value) const noexcept {
        const uint64_t x = static_cast<uint32_t>(value.x);
        const uint64_t y = static_cast<uint32_t>(value.y);
        const uint64_t z = static_cast<uint32_t>(value.z);
        return static_cast<size_t>((x * 0x9E3779B185EBCA87ULL) ^
                                   (y * 0xC2B2AE3D27D4EB4FULL) ^
                                   (z * 0x165667B19E3779F9ULL));
    }
};

struct ProjectionWorldMatchGroupKey {
    // These are floor-divided group coordinates, not block coordinates.
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const ProjectionWorldMatchGroupKey& other) const noexcept {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct ProjectionWorldMatchGroupKeyHash {
    size_t operator()(const ProjectionWorldMatchGroupKey& value) const noexcept {
        const uint64_t x = static_cast<uint32_t>(value.x);
        const uint64_t y = static_cast<uint32_t>(value.y);
        const uint64_t z = static_cast<uint32_t>(value.z);
        return static_cast<size_t>((x * 0x9E3779B185EBCA87ULL) ^
                                   (y * 0xC2B2AE3D27D4EB4FULL) ^
                                   (z * 0x165667B19E3779F9ULL));
    }
};

// Once published, this page is never modified.  `cells` uses x as the low
// nibble, z as the next nibble and y as the high nibble.  `fingerprint` is
// zero for an entirely default page and otherwise lets a resident VBO decide
// in O(1) whether it needs rebuilding.
struct ProjectionWorldMatchGroupState {
    std::array<uint8_t, kProjectionWorldMatchGroupCellCount> cells{};
    uint64_t fingerprint = 0;
    uint64_t xor_hash = 0;
    uint64_t additive_hash = 0;
    uint16_t non_default_count = 0;
};

// Published by the verified local-player game tick and never mutated after
// publication.  Missing group pages/cells render with their normal projection
// color.  The outer map is intentionally small (active 16-cube groups); page
// pointers are shared copy-on-write between adjacent snapshots.
struct ProjectionWorldMatchSnapshot {
    uint64_t projection_generation = 0;
    uint64_t plan_identity = 0;
    uint64_t revision = 0;
    std::unordered_map<ProjectionWorldMatchGroupKey,
                       std::shared_ptr<const ProjectionWorldMatchGroupState>,
                       ProjectionWorldMatchGroupKeyHash> groups;
};

// Render-thread consumer.  The returned shared snapshot is immutable and can
// be retained for the full frame without touching world-owned memory.
std::shared_ptr<const ProjectionWorldMatchSnapshot>
GetProjectionWorldMatchSnapshot() noexcept;

// O(1) renderer helpers.  `group_origin_*` must be the 16-aligned origin of a
// renderer group.  World-position lookup is provided for tests and any caller
// that has not already decoded an instance's local grid coordinate.
const ProjectionWorldMatchGroupState* FindProjectionWorldMatchGroupState(
    const ProjectionWorldMatchSnapshot* snapshot,
    int32_t group_origin_x, int32_t group_origin_y,
    int32_t group_origin_z) noexcept;
uint64_t GetProjectionWorldMatchGroupFingerprint(
    const ProjectionWorldMatchSnapshot* snapshot,
    int32_t group_origin_x, int32_t group_origin_y,
    int32_t group_origin_z) noexcept;
ProjectionWorldMatchState GetProjectionWorldMatchStateAt(
    const ProjectionWorldMatchSnapshot* snapshot,
    int32_t world_x, int32_t world_y, int32_t world_z) noexcept;
ProjectionWorldMatchState GetProjectionWorldMatchGroupCellState(
    const ProjectionWorldMatchGroupState* group,
    uint8_t local_x, uint8_t local_y, uint8_t local_z) noexcept;

// Render thread -> local-player tick work description.  The renderer should
// publish only groups/layer batches which it actually chose to draw in the
// current view, ordered with a smaller priority first.  Bounds are inclusive,
// must span no more than 32 blocks per axis, and are pointer-free so the tick
// never retains renderer-owned plan memory.
struct ProjectionWorldMatchInterestRegion {
    int32_t min_x = 0;
    int32_t min_y = 0;
    int32_t min_z = 0;
    int32_t max_x = -1;
    int32_t max_y = -1;
    int32_t max_z = -1;
    uint32_t priority = 0;

    bool isValid() const noexcept {
        return min_x <= max_x && min_y <= max_y && min_z <= max_z;
    }
};

struct ProjectionWorldMatchInterestSnapshot {
    uint64_t plan_identity = 0;
    // Increment only when the region list changes.  The consumer also hashes
    // normalized region content defensively so an accidental per-frame bump
    // cannot starve the round-robin scanner.
    uint64_t revision = 0;
    std::vector<ProjectionWorldMatchInterestRegion> regions;
};

// The render thread calls Publish... after it has finalized its visible draw
// groups.  Ownership is immutable and atomically exchanged; this function
// never reads the world.  Passing null/zero or Clear... withdraws work.
void PublishProjectionWorldMatchInterest(
    std::shared_ptr<const ProjectionWorldMatchInterestSnapshot> snapshot) noexcept;
void ClearProjectionWorldMatchInterest() noexcept;
std::shared_ptr<const ProjectionWorldMatchInterestSnapshot>
GetProjectionWorldMatchInterestSnapshot() noexcept;

// Bounded local-world comparison service. NativeWorldReader is used only from
// onGameTick(), which must be called by the verified LocalPlayer tick.
class ProjectionWorldMatchRuntime {
public:
    static ProjectionWorldMatchRuntime& instance();

    void onGameTick();
    // Invalidates stale statuses immediately when a projection is hidden,
    // cleared, reloaded, or authorization is revoked.
    void clear();

private:
    ProjectionWorldMatchRuntime() = default;
    ~ProjectionWorldMatchRuntime();
    ProjectionWorldMatchRuntime(const ProjectionWorldMatchRuntime&) = delete;
    ProjectionWorldMatchRuntime& operator=(const ProjectionWorldMatchRuntime&) = delete;

    struct RuntimeState;
    std::mutex mutex_;
    std::unique_ptr<RuntimeState> runtime_;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_PROJECTION_WORLD_MATCH_RUNTIME_H
