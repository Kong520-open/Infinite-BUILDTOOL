#include "../BuildProjectionRenderer.h"
#include "../InfiniteczBuildParser.h"

#include <algorithm>
#include <cmath>

namespace build_import {

bool resolveProjectionWorldLayerRange(int32_t relative_origin_y,
                                      const ProjectionLayerFilter& filter,
                                      int64_t* minimum_y,
                                      int64_t* maximum_y) noexcept {
    if (!minimum_y || !maximum_y) return false;
    if (filter.mode == ProjectionLayerMode::All) {
        *minimum_y = INT32_MIN;
        *maximum_y = INT32_MAX;
        return true;
    }
    int64_t lower = filter.minimum_y;
    int64_t upper = filter.maximum_y;
    switch (filter.mode) {
        case ProjectionLayerMode::AtOrAbove:
            upper = INT32_MAX;
            break;
        case ProjectionLayerMode::AtOrBelow:
            lower = INT32_MIN;
            break;
        case ProjectionLayerMode::Single:
            upper = lower;
            break;
        case ProjectionLayerMode::Range:
            if (lower > upper) std::swap(lower, upper);
            break;
        case ProjectionLayerMode::All:
            break;
    }
    if (filter.space == ProjectionLayerSpace::Relative) {
        if (lower != INT32_MIN) lower += relative_origin_y;
        if (upper != INT32_MAX) upper += relative_origin_y;
    }
    *minimum_y = lower;
    *maximum_y = upper;
    return lower <= upper;
}

bool makeProjectionDisplayScope(int32_t relative_origin_y, int32_t range_chunks,
                                const ProjectionLayerFilter& filter,
                                ProjectionDisplayScope* scope) noexcept {
    if (!scope) return false;
    ProjectionDisplayScope result;
    result.range_chunks = std::max(1, std::min(64, range_chunks));
    if (!resolveProjectionWorldLayerRange(relative_origin_y, filter,
                                          &result.minimum_world_y,
                                          &result.maximum_world_y)) {
        return false;
    }
    *scope = result;
    return true;
}

bool projectionDisplayScopeContainsBlock(const ProjectionDisplayScope& scope,
                                         float view_x, float view_z,
                                         int32_t block_x, int32_t block_y,
                                         int32_t block_z) noexcept {
    if (!std::isfinite(view_x) || !std::isfinite(view_z) ||
        block_y < scope.minimum_world_y || block_y > scope.maximum_world_y) {
        return false;
    }
    const double range = static_cast<double>(scope.range_chunks) * 16.0;
    const double minimum_x = static_cast<double>(block_x);
    const double maximum_x = minimum_x + 1.0;
    const double minimum_z = static_cast<double>(block_z);
    const double maximum_z = minimum_z + 1.0;
    const double x = static_cast<double>(view_x);
    const double z = static_cast<double>(view_z);
    const double dx = x < minimum_x ? minimum_x - x :
        (x > maximum_x ? x - maximum_x : 0.0);
    const double dz = z < minimum_z ? minimum_z - z :
        (z > maximum_z ? z - maximum_z : 0.0);
    return dx * dx + dz * dz <= range * range;
}

BuildProjectionRenderer& BuildProjectionRenderer::instance() {
    static BuildProjectionRenderer renderer;
    return renderer;
}

bool BuildProjectionRenderer::beginStreamingPlan(ProjectionStreamingOptions,
                                                  std::string*) {
    return false;
}

bool BuildProjectionRenderer::appendStreamingPartition(
        ProjectionPartitionInput, const CancelCheck&, std::string*) {
    return false;
}

bool BuildProjectionRenderer::commitStreamingPlan(const CancelCheck&, std::string*) {
    return false;
}

bool BuildProjectionRenderer::enqueueLazyPartition(
        uint64_t, ProjectionPartitionInput, const CancelCheck&, std::string*) {
    return false;
}

void BuildProjectionRenderer::abortStreamingPlan() noexcept {}
void BuildProjectionRenderer::clearPlan() noexcept {}
bool BuildProjectionRenderer::clearPlanAndWaitForRender(uint32_t) { return true; }
int32_t BuildProjectionRenderer::rangeChunks() const noexcept { return 12; }
ProjectionLayerFilter BuildProjectionRenderer::layerFilter() const {
    return {};
}
ProjectionDisplayScope BuildProjectionRenderer::displayScope(
        int32_t relative_origin_y) const noexcept {
    ProjectionDisplayScope scope;
    (void)makeProjectionDisplayScope(relative_origin_y, rangeChunks(), layerFilter(), &scope);
    return scope;
}
bool BuildProjectionRenderer::isEnabled() const noexcept { return true; }
uint64_t BuildProjectionRenderer::publishedPlanIdentity() const noexcept { return 0; }
std::string BuildProjectionRenderer::lazyBuildError(uint64_t) const { return {}; }
const char* BuildProjectionRenderer::renderDiagnostic() const noexcept {
    return "projection renderer diagnostics unavailable in host test stub";
}

bool GetLatestBuildRenderCameraPosition(float*, float*, float*) noexcept {
    return false;
}
}  // namespace build_import

namespace build_import {

bool InfiniteczBuildParser::parse(const SchematicParseOptions&,
                                  const BlockMapper&,
                                  SchematicParseResult*,
                                  std::string*) const {
    return false;
}

}  // namespace build_import
