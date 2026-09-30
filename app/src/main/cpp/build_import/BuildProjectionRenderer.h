#ifndef INFINITE_TEXTURE_BUILD_PROJECTION_RENDERER_H
#define INFINITE_TEXTURE_BUILD_PROJECTION_RENDERER_H

#include "BuildImportTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace build_import {

// A projection always works from the parsed blueprint voxels, never from the
// command spools used by the importer. Names live in a compact shared table;
// repeating a material millions of times must not allocate millions of strings.
struct ProjectionBlock {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    // Visual aux is intentionally low-byte only because the projection models
    // predate full Bedrock state values.  The source fields below preserve the
    // untruncated parser record for non-render consumers such as the printer.
    uint8_t aux = 0;
    uint8_t rotation_quarters = 0;
    uint32_t name_index = 0;
    uint16_t source_aux = 0;
    uint16_t source_name_index = 0;
    uint8_t source_flags = 0;
    uint8_t source_phase = 0;
};

struct ProjectionBlueprint {
    std::vector<ProjectionBlock> blocks;
    std::vector<std::string> names;
};

enum class ProjectionLayerMode : uint8_t {
    All = 0,
    AtOrAbove,
    AtOrBelow,
    Single,
    Range,
};

enum class ProjectionLayerSpace : uint8_t {
    // Zero is the lowest layer of the projected building. Native UI may show
    // this as layer 1 while keeping the renderer's coordinate arithmetic zero-based.
    Relative = 0,
    World,
};

struct ProjectionLayerFilter {
    ProjectionLayerMode mode = ProjectionLayerMode::All;
    ProjectionLayerSpace space = ProjectionLayerSpace::Relative;
    int32_t minimum_y = 0;
    int32_t maximum_y = 0;
};

// A stable, pointer-free snapshot of the projection controls that determine
// which source blocks belong to the user's current display scope.  Rendering
// has additional per-frame concerns (frustum, GPU budgets, and surface
// extraction), so gameplay features should use this scope rather than a
// transient draw list.
struct ProjectionDisplayScope {
    int32_t range_chunks = 12;
    int64_t minimum_world_y = INT32_MIN;
    int64_t maximum_world_y = INT32_MAX;
};

// A game-tick produced position which is currently safe for the projection
// printer to attempt: it is inside the active display scope and interaction
// range, its target cell is empty, and a valid support/click face exists.  It
// deliberately carries no engine pointer or item information so the renderer
// can consume it asynchronously without touching world state.
struct ProjectionReachabilityPreviewPosition {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

// Resolves a layer UI selection into the same world-space interval used by
// the renderer.  Relative layers are zero-based and anchored at the lowest
// Y of the loaded projection.
bool resolveProjectionWorldLayerRange(int32_t relative_origin_y,
                                      const ProjectionLayerFilter& filter,
                                      int64_t* minimum_y,
                                      int64_t* maximum_y) noexcept;

// Builds a complete snapshot for non-render consumers such as the projection
// printer.  range_chunks is normalized to the renderer's supported range.
bool makeProjectionDisplayScope(int32_t relative_origin_y, int32_t range_chunks,
                                const ProjectionLayerFilter& filter,
                                ProjectionDisplayScope* scope) noexcept;

// Tests the raw source block against the stable display range.  Horizontal
// range uses the nearest point of the block AABB, mirroring the renderer's
// group-bound distance rule while remaining independent of render frames.
bool projectionDisplayScopeContainsBlock(const ProjectionDisplayScope& scope,
                                         float view_x, float view_z,
                                         int32_t block_x, int32_t block_y,
                                         int32_t block_z) noexcept;

struct ProjectionMaterialRequest {
    uint16_t material_id = 0;
    std::string name;
    uint8_t aux = 0;
    uint8_t rotation_quarters = 0;
};

struct ProjectionStreamingOptions {
    uint64_t source_block_count = 0;
    int32_t relative_origin_y = 0;
    std::string cache_directory;
    // These are resource budgets rather than total-building block limits. A
    // caller can tune them for the device without changing projection semantics.
    size_t maximum_partition_blocks = 256U * 1024U;
    // Zero means no logical total-size cap; actual storage exhaustion is still
    // reported cleanly by appendStreamingPartition.
    size_t maximum_spool_bytes = 0;
    size_t maximum_cpu_cache_bytes = 24U * 1024U * 1024U;
    // Lazy projection builds publish their complete material registry before
    // the first spatial partition is converted into render instances.  The UI
    // can therefore decode/install every vanilla texture once, while later
    // nearby partitions retain stable material IDs.
    std::vector<ProjectionMaterialRequest> material_requests;
    // Keep the render spool open after commit and allow worker-produced groups
    // to be adopted by the world-render thread in a bounded per-frame budget.
    // This is intentionally opt-in: normal streaming plans retain their
    // existing all-at-once publication semantics.
    bool lazy_surface_streaming = false;
};

struct ProjectionPartitionInput {
    ProjectionBlueprint blueprint;
    // Only blocks in core_bounds produce instances. The blueprint should also
    // contain a one-block halo so faces shared across partitions are culled.
    BlockBounds core_bounds;
};

class BuildProjectionRenderer {
public:
    using CancelCheck = std::function<bool()>;

    static BuildProjectionRenderer& instance();

    // Large projections are built one bounded spatial partition at a time.
    // appendStreamingPartition writes finished instance groups directly to a
    // renderer-owned disk spool, so neither source blocks nor all instances
    // become permanently resident in memory.
    bool beginStreamingPlan(ProjectionStreamingOptions options,
                            std::string* error = nullptr);
    bool appendStreamingPartition(ProjectionPartitionInput partition,
                                  const CancelCheck& cancel_check = {},
                                  std::string* error = nullptr);
    bool commitStreamingPlan(const CancelCheck& cancel_check = {},
                             std::string* error = nullptr);
    // Performs expensive surface extraction on the caller's worker thread,
    // then queues the compact groups.  Only render() adopts those groups into
    // the published plan, so GPU/LRU state remains render-thread owned.
    bool enqueueLazyPartition(uint64_t expected_plan_identity,
                              ProjectionPartitionInput partition,
                              const CancelCheck& cancel_check = {},
                              std::string* error = nullptr);
    void abortStreamingPlan() noexcept;
    void clearPlan() noexcept;
    // Clears the published plan and, when one was active, waits for the world
    // render thread to relinquish its GLES objects before another plan is
    // built. This is intended for the background loader, never the render
    // hook itself.
    bool clearPlanAndWaitForRender(uint32_t timeout_ms);

    void setEnabled(bool enabled);
    // Lightweight atomic snapshot for non-render gameplay features.  A hidden
    // projection must not leave an autonomous printer active in the world.
    bool isEnabled() const noexcept;
    bool setOutlineEnabled(bool enabled) noexcept;
    void setFillAlpha(float alpha);
    void setRangeChunks(int32_t range_chunks);
    int32_t rangeChunks() const noexcept;
    void setLayerFilter(ProjectionLayerFilter filter);
    ProjectionLayerFilter layerFilter() const;
    ProjectionDisplayScope displayScope(int32_t relative_origin_y) const noexcept;
    // Reachability presentation is separate from printer enablement.  The
    // printer publishes a compact immutable set from the verified game tick;
    // this render-side toggle only controls whether that set is highlighted.
    void setReachabilityPreviewEnabled(bool enabled) noexcept;
    bool reachabilityPreviewEnabled() const noexcept;
    void publishReachabilityPreview(
        uint64_t expected_plan_identity,
        std::vector<ProjectionReachabilityPreviewPosition> positions);
    void clearReachabilityPreview() noexcept;
    // Snapshot of the compact palette referenced by per-instance material_id.
    // ID zero is always the programmatic-color fallback.
    std::vector<ProjectionMaterialRequest> textureMaterialRequests() const;
    // Publishes decoded vanilla texture layers for the currently published
    // projection plan. material_keys use "name\taux\trotationQuarter" and each
    // material contributes six face-layer entries in -Z,+Z,-X,+X,-Y,+Y order.
    // Face layer zero (or -1 at this API boundary) keeps the programmatic-color
    // fallback for that face; decoded texture layers start at one.
    bool installTexturePack(std::vector<std::string> material_keys,
                            std::vector<int16_t> face_layers,
                            int32_t tile_size,
                            std::vector<uint8_t> rgba_layers,
                            std::string* error = nullptr);
    uint64_t publishedPlanIdentity() const noexcept;
    // Nonempty only when the render thread was unable to commit queued lazy
    // groups (for example, because its spool storage became unavailable).
    std::string lazyBuildError(uint64_t expected_plan_identity) const;

    // A compact, pointer-free reason for why the latest render frame could
    // not acquire a camera.  Exposed through the projection status UI to make
    // a game-version ABI mismatch diagnosable without logcat.
    const char* renderDiagnostic() const noexcept;
    void render();

private:
    BuildProjectionRenderer() = default;
};

// Installs the one world-render hook required by the projection renderer.
bool InitBuildProjectionHook(uintptr_t base_address);

// Returns the most recent world-space render-camera origin published by the
// Level::_render hook.  It is deliberately pointer-free so non-render code
// can use it as a safe native fallback when the embedded Python bridge is not
// available after a game update.
bool GetLatestBuildRenderCameraPosition(float* x, float* y, float* z) noexcept;

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BUILD_PROJECTION_RENDERER_H
