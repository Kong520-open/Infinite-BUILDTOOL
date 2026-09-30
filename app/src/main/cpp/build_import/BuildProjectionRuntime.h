#ifndef INFINITE_TEXTURE_BUILD_PROJECTION_RUNTIME_H
#define INFINITE_TEXTURE_BUILD_PROJECTION_RUNTIME_H

#include "BuildImportTypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace build_import {

struct BuildProjectionLoadRequest {
    std::string source_path;
    std::string work_directory;
    int32_t base_x = 0;
    int32_t base_y = 0;
    int32_t base_z = 0;
    // Clockwise in the top-down X/Z view. Only quarter turns are accepted.
    int32_t rotation_degrees = 0;
    int32_t pixel_art_width = 64;
};

enum class BuildProjectionState : uint8_t {
    Idle,
    Loading,
    Ready,
    Failed,
};

// Raw source flags retained for the printer.  These deliberately mirror the
// parser's compact projection spool flags, except for the private validity
// and core markers which are not exposed to callers.
enum ProjectionPrinterTargetFlag : uint8_t {
    ProjectionPrinterCanFill = 0x01U,
    ProjectionPrinterSingleLayerOnly = 0x02U,
    ProjectionPrinterStateful = 0x08U,
};

// A fully resolved source block for the projection printer.  Unlike a render
// instance this preserves the exact 16-bit auxiliary value and import phase,
// so gameplay placement can make its own material and safety decisions.
struct ProjectionPrinterTarget {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    std::string name;
    uint16_t aux = 0;
    ImportPhase phase = ImportPhase::Structure;
    uint8_t flags = 0;
    uint8_t rotation_quarters = 0;

    bool canFill() const { return (flags & ProjectionPrinterCanFill) != 0; }
    bool singleLayerOnly() const {
        return (flags & ProjectionPrinterSingleLayerOnly) != 0;
    }
    bool stateful() const { return (flags & ProjectionPrinterStateful) != 0; }
};

// The query is intentionally bounded around the player.  The printer can
// only interact with nearby blocks, so this avoids scanning a large rendered
// distance on every game tick.
struct ProjectionPrinterQuery {
    int32_t player_x = 0;
    int32_t player_y = 0;
    int32_t player_z = 0;
    int32_t horizontal_radius_blocks = 8;
    int32_t vertical_radius_blocks = 8;
    size_t maximum_targets = 256;
    bool restrict_to_display_scope = true;
};

enum class ProjectionPrinterQueryStatus : uint8_t {
    NoProjection,
    Ready,
    Empty,
    InvalidQuery,
    SourceUnavailable,
};

struct ProjectionPrinterQueryResult {
    ProjectionPrinterQueryStatus status = ProjectionPrinterQueryStatus::NoProjection;
    uint64_t generation = 0;
    uint64_t plan_identity = 0;
    // The complete current display scope can span far beyond the interaction
    // radius.  A nearby tick query returns this only when a matching cached
    // count is available; callers must not treat an unknown value as zero.
    uint64_t display_scope_block_count = 0;
    bool display_scope_block_count_known = false;
    // True means this is not a complete nearby snapshot: either it exceeded
    // maximum_targets or one or more cold source partitions are still being
    // decoded by the background cache worker.  Callers should retry on the
    // next game tick before deciding that a Ready/empty result is complete.
    bool truncated = false;
    std::vector<ProjectionPrinterTarget> targets;
};

// A compact inclusive world-space region requested by the renderer-interest
// matcher. The target-page API bounds every axis to one 32-block source
// partition span, so game-tick callers never enumerate a full display range.
struct ProjectionWorldMatchRegion {
    int32_t min_x = 0;
    int32_t min_y = 0;
    int32_t min_z = 0;
    int32_t max_x = 0;
    int32_t max_y = 0;
    int32_t max_z = 0;
};

enum class ProjectionWorldMatchTargetPageStatus : uint8_t {
    NoProjection,
    // A non-final, fully cached page. Continue with next_cursor.
    Ready,
    // One or more source partitions are cold or busy. The current cursor stays
    // valid; retry from it on a later game tick after the cache worker runs.
    Pending,
    // This is the final page; next_cursor is zero. It may carry targets.
    Complete,
    InvalidQuery,
    SourceUnavailable,
};

// A pointer-free source page for a compact renderer-interest region. Cursor
// zero starts a region (and also marks completion); nonzero cursors encode the
// deterministic partition ordinal plus the next sorted cached target index.
// Callers must restart at zero if generation or plan_identity changes.
struct ProjectionWorldMatchTargetPage {
    ProjectionWorldMatchTargetPageStatus status =
        ProjectionWorldMatchTargetPageStatus::NoProjection;
    uint64_t generation = 0;
    uint64_t plan_identity = 0;
    uint64_t next_cursor = 0;
    std::vector<ProjectionPrinterTarget> targets;
};

// One item-material entry for the complete loaded projection.  item_name is a
// namespace-free, normalized Bedrock item/block identifier (for example
// "spruce_planks").  item_aux is normally zero because placement-only state
// such as direction, slab half and redstone power is folded into one item;
// it is retained only for a material variant which cannot be safely flattened
// into a distinct name.
struct ProjectionMaterialSummaryEntry {
    std::string item_name;
    uint16_t item_aux = 0;
    uint64_t count = 0;
};

enum class ProjectionMaterialSummaryPageStatus : uint8_t {
    NoProjection,
    // The immutable whole-projection source is still being reduced on its
    // background worker. Retry the same cursor later; no partial summary is
    // exposed so callers never mix two pages from different reductions.
    Pending,
    // A non-final page. Continue with next_cursor.
    Ready,
    // The final page; next_cursor is zero. It may contain no entries only for
    // a source which contains no final core cells.
    Complete,
    InvalidQuery,
    SourceUnavailable,
};

// A deterministic, pointer-free page of the material list for the entire
// loaded projection.  It is intentionally independent from render distance,
// layer filters and printer display scope. Cursor zero starts a fresh read;
// a nonzero cursor is the next entry index returned by the preceding Ready
// page. Restart at zero if generation or plan_identity changes.
struct ProjectionMaterialSummaryPage {
    ProjectionMaterialSummaryPageStatus status =
        ProjectionMaterialSummaryPageStatus::NoProjection;
    uint64_t generation = 0;
    uint64_t plan_identity = 0;
    uint64_t next_cursor = 0;
    // Number of final, unique source cells represented by the summary. This
    // excludes partition halo duplicates and overwritten records.
    uint64_t total_block_count = 0;
    uint64_t total_material_count = 0;
    std::vector<ProjectionMaterialSummaryEntry> entries;
};

// Owns blueprint parsing and projection publication independently from the
// building importer. load() is synchronous and may be called from a worker;
// status queries and clear() are safe from the native UI thread.
class BuildProjectionRuntime {
public:
    // Implementation-only storage type.  The declaration is public only so
    // translation-unit helpers can keep shared ownership opaque; callers
    // cannot construct or inspect the incomplete type.
    struct LazyProjectionSource;

    static BuildProjectionRuntime& instance();

    bool load(BuildProjectionLoadRequest request, std::string* error = nullptr);
    void clear();

    BuildProjectionState state() const;
    std::string status() const;
    uint64_t blockCount() const;
    std::string sourcePath() const;

    // Returns a bounded snapshot of core source blocks near the player.  The
    // active display range/layer controls are applied when requested, but
    // frustum and per-frame renderer budgets are deliberately excluded so a
    // printer queue does not change as the player turns the camera.
    bool queryNearbyPrinterTargets(const ProjectionPrinterQuery& query,
                                   ProjectionPrinterQueryResult* result,
                                   std::string* error = nullptr) const;

    // Returns one nonblocking page of raw source blocks in region. It only
    // consults the existing printer target cache: a cold partition is queued
    // for its worker and reported as Pending, never decoded synchronously on
    // the local-player game tick. Regions are inclusive and limited to 32
    // blocks on every axis; maximum_targets is limited to 256.
    bool queryProjectionWorldMatchTargetPage(
        const ProjectionWorldMatchRegion& region, uint64_t cursor,
        size_t maximum_targets, ProjectionWorldMatchTargetPage* page,
        std::string* error = nullptr) const;

    // Returns a page of the complete projection material summary. The source
    // is reduced once on a background worker after publication, so this API
    // never synchronously scans raw spool storage on a UI/game-tick caller.
    // maximum_entries is limited to 1..256.
    bool queryProjectionMaterialSummaryPage(
        uint64_t cursor, size_t maximum_entries,
        ProjectionMaterialSummaryPage* page,
        std::string* error = nullptr) const;

private:
    BuildProjectionRuntime() = default;
    ~BuildProjectionRuntime();

    void stopLazySurfaceWorker() noexcept;
    void clearPrinterSource() noexcept;
    void publishPrinterSource(std::shared_ptr<LazyProjectionSource> source);
    void runPrinterTargetWorker(std::shared_ptr<LazyProjectionSource> source) noexcept;
    void runPrinterScopeWorker(std::shared_ptr<LazyProjectionSource> source) noexcept;
    void runPrinterMaterialSummaryWorker(
        std::shared_ptr<LazyProjectionSource> source) noexcept;
    void startLazySurfaceWorker(std::shared_ptr<LazyProjectionSource> source);
    void runLazySurfaceWorker(std::shared_ptr<LazyProjectionSource> source) noexcept;

    bool publishStatus(uint64_t generation, BuildProjectionState state,
                       std::string status, uint64_t block_count = 0,
                       std::string source_path = {});

    // Serializes parse/publication operations while generation_ lets clear or
    // a newer load cooperatively cancel the operation currently holding it.
    mutable std::mutex operation_mutex_;
    mutable std::mutex state_mutex_;
    mutable std::mutex lazy_worker_mutex_;
    mutable std::mutex printer_source_mutex_;
    std::thread lazy_worker_;
    std::thread printer_target_worker_;
    std::thread printer_scope_worker_;
    std::thread printer_material_summary_worker_;
    std::shared_ptr<LazyProjectionSource> lazy_source_;
    std::shared_ptr<LazyProjectionSource> printer_source_;
    std::atomic<uint64_t> generation_{0};
    BuildProjectionState state_ = BuildProjectionState::Idle;
    std::string status_ = "no projection loaded";
    uint64_t block_count_ = 0;
    std::string source_path_;
};

#if defined(BUILD_IMPORT_PROJECTION_TESTS)
bool RunBuildProjectionPartitionSpoolSelfTest(const std::string& work_directory,
                                              std::string* error = nullptr);
#endif

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BUILD_PROJECTION_RUNTIME_H
