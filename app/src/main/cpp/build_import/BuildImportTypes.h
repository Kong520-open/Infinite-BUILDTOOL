#ifndef INFINITE_TEXTURE_BUILD_IMPORT_TYPES_H
#define INFINITE_TEXTURE_BUILD_IMPORT_TYPES_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace build_import {

enum class ImportPhase : uint8_t {
    Clear = 0,
    Structure,
    Gravity,
    Attachment,
    Fluid,
    // Keep this phase after every legacy phase so existing phase numbers and
    // command-spool names remain readable. The second half of multi-block
    // attachments (bed heads, upper doors, and similar blocks) is emitted here
    // only after all support halves have been placed across every partition.
    DependentAttachment,
    Count,
};

enum class ImportState : uint8_t {
    Idle,
    Planning,
    Running,
    Paused,
    ClosedForContextChange,
    Completed,
    Failed,
    Verifying,
};

enum class OverwritePolicy : uint8_t {
    PreserveExisting,
    ClearImportedBounds,
};

enum class VerificationPrecision : uint8_t {
    Fast = 0,
    Balanced,
    Thorough,
};

// The source format is persisted with a checkpoint so a restricted building
// import cannot be resumed through a spool created for a different importer.
enum class ImportSourceType : uint8_t {
    Schematic = 0,
    PixelArtPng,
    // Keep the old on-disk checkpoint value reserved.  MP3 command-music
    // imports are intentionally no longer accepted, and restore explicitly
    // rejects this marker rather than mistaking an interrupted MP3 job for a
    // MIDI score.
    DeprecatedCommandMusicMp3,
    Litematic,
    Bdx,
    Mcworld,
    InfiniteczBuild,
    // Appended instead of reusing the deprecated MP3 value because this enum
    // is persisted in build-import checkpoints.
    CommandMusicMidi,
};

// Full access is granted by a live signed capability ticket.  The limited
// profile is deliberately narrow and is the only profile available before a
// building-tools account has been verified.
enum class BuildToolsAccessProfile : uint8_t {
    Full = 0,
    LimitedImport = 1,
};

inline bool isLimitedBuildToolsImport(BuildToolsAccessProfile profile) {
    return profile == BuildToolsAccessProfile::LimitedImport;
}

constexpr size_t kImportPhaseCount = static_cast<size_t>(ImportPhase::Count);

// Raw parser output is temporary, but keeping an explicit versioned layout
// lets the planner read older interrupted jobs while avoiding one block-name
// string per voxel in newly parsed jobs. All fields are native little-endian;
// parser and planner always run in the same process/ABI.
constexpr uint32_t kRawSpoolMagic = 0x32505342U;       // "BSP2"
// Version 3 widens the temporary block auxiliary value to the full native
// Bedrock uint16_t range.  Older BDX writers store packed block states in the
// legacy PlaceBlock data field, and valid values such as 5399 must not be
// truncated while a plan is being built.
constexpr uint32_t kRawSpoolVersion = 3;
constexpr uint32_t kRawSpoolKindBlocks = 1;
constexpr uint32_t kRawSpoolKindStaged = 2;
constexpr uint32_t kRawPaletteMagic = 0x324C5042U;     // "BPL2"
constexpr uint32_t kRawPaletteVersion = 2;
constexpr const char* kRawPaletteFileName = "_raw_block_palette.bpal";

struct RawSpoolHeaderV2 {
    uint32_t magic = kRawSpoolMagic;
    uint32_t version = kRawSpoolVersion;
    uint32_t record_size = 0;
    uint32_t kind = 0;
};

struct RawSpoolRecordV2 {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint32_t name_id = 0;
    uint8_t aux = 0;
    uint8_t flags = 0;
    uint16_t reserved = 0;
};

// Keep the v2 declaration above so the planner can explicitly recognize an
// interrupted pre-v3 spool instead of treating its byte layout as a v3 one.
// The v3 layout intentionally remains 20 bytes, but moves one reserved byte
// to the end to make room for the complete auxiliary value.
struct RawSpoolRecordV3 {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint32_t name_id = 0;
    uint16_t aux = 0;
    uint8_t flags = 0;
    uint8_t reserved = 0;
};

struct StagedRawSpoolRecordV2 {
    RawSpoolRecordV2 block;
    uint8_t phase = 0;
    uint8_t reserved[3]{};
};

struct StagedRawSpoolRecordV3 {
    RawSpoolRecordV3 block;
    uint8_t phase = 0;
    uint8_t reserved[3]{};
};

struct RawPaletteHeaderV2 {
    uint32_t magic = kRawPaletteMagic;
    uint32_t version = kRawPaletteVersion;
    uint32_t entry_count = 0;
    uint32_t reserved = 0;
};

static_assert(sizeof(RawSpoolHeaderV2) == 16, "unexpected raw spool header layout");
static_assert(sizeof(RawSpoolRecordV2) == 20, "unexpected raw spool record layout");
static_assert(sizeof(RawSpoolRecordV3) == 20, "unexpected wide raw spool record layout");
static_assert(sizeof(StagedRawSpoolRecordV2) == 24,
              "unexpected staged raw spool record layout");
static_assert(sizeof(StagedRawSpoolRecordV3) == 24,
              "unexpected staged wide raw spool record layout");
static_assert(sizeof(RawPaletteHeaderV2) == 16, "unexpected raw palette header layout");

constexpr int32_t kMaximumBlocksPerSecond = 200000;
constexpr uint32_t kMaximumFillBlockCount = 32768;
// Static structure commands may use the complete Bedrock /fill allowance.
// Dynamic phases stay on their smaller cap below because they can trigger
// block updates, gravity and fluid simulation immediately.
constexpr uint32_t kMaximumScheduledFillBlockCount = kMaximumFillBlockCount;
constexpr uint32_t kMaximumDynamicFillBlockCount = 2048;
static_assert(kMaximumScheduledFillBlockCount <= kMaximumFillBlockCount,
               "scheduled fills must respect the Bedrock command limit");
static_assert(kMaximumDynamicFillBlockCount <= kMaximumScheduledFillBlockCount,
              "dynamic fills must not exceed the static planning limit");

// Average throughput is already enforced by the runtime token bucket, which
// debits every command by its full block count. These caps therefore only
// bound the single-command server-tick spike, so they may be far larger than
// one dispatch interval's budget: a static /fill may spend up to two seconds
// of the configured rate at once, a dynamic one (gravity/fluid updates run
// immediately) up to a quarter second. Larger caps directly reduce the total
// command count, which is the real dispatch bottleneck at 20 Hz.
inline uint32_t fillBlockLimitForRate(int32_t blocks_per_second) {
    const int64_t rate = std::max<int64_t>(1, blocks_per_second);
    return static_cast<uint32_t>(std::max<int64_t>(
        64, std::min<int64_t>(kMaximumScheduledFillBlockCount, rate * 2)));
}

inline uint32_t dynamicFillBlockLimitForRate(int32_t blocks_per_second) {
    const int64_t rate = std::max<int64_t>(1, blocks_per_second);
    return static_cast<uint32_t>(std::max<int64_t>(
        32, std::min<int64_t>(kMaximumDynamicFillBlockCount, rate / 4)));
}

inline bool isValidVerificationPrecision(VerificationPrecision precision) {
    return precision >= VerificationPrecision::Fast &&
           precision <= VerificationPrecision::Thorough;
}

inline uint32_t verificationStableSampleLimit(VerificationPrecision precision) {
    switch (precision) {
        case VerificationPrecision::Fast: return 8;
        case VerificationPrecision::Balanced: return 12;
        case VerificationPrecision::Thorough: return 16;
    }
    return 16;
}

inline uint32_t verificationAirSampleLimit(VerificationPrecision precision) {
    switch (precision) {
        case VerificationPrecision::Fast: return 2;
        case VerificationPrecision::Balanced: return 3;
        case VerificationPrecision::Thorough: return 4;
    }
    return 4;
}

// Phase IDs are persistent file/checkpoint values, so the new dependency
// phase is ID 5 even though it executes before the legacy fluid ID 4.
constexpr std::array<ImportPhase, kImportPhaseCount - 1> kPlacementPhaseOrder{{
    ImportPhase::Structure,
    ImportPhase::Gravity,
    ImportPhase::Attachment,
    ImportPhase::DependentAttachment,
    ImportPhase::Fluid,
}};

inline size_t phaseIndex(ImportPhase phase) {
    return static_cast<size_t>(phase);
}

inline size_t placementOrderIndex(ImportPhase phase) {
    for (size_t index = 0; index < kPlacementPhaseOrder.size(); ++index) {
        if (kPlacementPhaseOrder[index] == phase) return index;
    }
    return kPlacementPhaseOrder.size();
}

inline ImportPhase nextPlacementPhase(ImportPhase phase) {
    const size_t index = placementOrderIndex(phase);
    return index < kPlacementPhaseOrder.size() && index + 1 < kPlacementPhaseOrder.size()
        ? kPlacementPhaseOrder[index + 1] : ImportPhase::Count;
}

struct ChunkCoord {
    int32_t x = 0;
    int32_t z = 0;

    bool operator==(const ChunkCoord& other) const {
        return x == other.x && z == other.z;
    }

    bool operator<(const ChunkCoord& other) const {
        return z == other.z ? x < other.x : z < other.z;
    }
};

struct BlockBounds {
    int32_t min_x = 0;
    int32_t min_y = 0;
    int32_t min_z = 0;
    int32_t max_x = -1;
    int32_t max_y = -1;
    int32_t max_z = -1;

    bool isValid() const {
        return min_x <= max_x && min_y <= max_y && min_z <= max_z;
    }
};

struct WorldContext {
    std::string world_id;
    int32_t dimension_id = 0;

    bool operator==(const WorldContext& other) const {
        return world_id == other.world_id && dimension_id == other.dimension_id;
    }
};

struct ImportConfig {
    // All new imports are partitioned on native (16x16 block) chunks.  Keeping
    // this explicit prevents a UI value expressed in simulation chunks from
    // silently becoming a larger 32-block logical chunk.
    static constexpr int32_t kVanillaChunkSize = 16;
    static constexpr int32_t kMinimumSimulationChunkRange = 4;
    static constexpr int32_t kMaximumSimulationChunkRange = 8;
    static constexpr int32_t kDefaultSimulationChunkRange =
        kMinimumSimulationChunkRange;

    int32_t chunk_size = kVanillaChunkSize;
    // Side length of one core import work unit, measured in native Minecraft
    // chunks (16x16 blocks).  Valid new-import values are 4..8, so an entered
    // value maps exactly to an NxN core region rather than a radius-derived
    // number of larger logical chunks.
    int32_t simulation_chunk_range = kDefaultSimulationChunkRange;
    // Persisted only to restore checkpoints written before v10.  New imports
    // must use simulation_chunk_range; this value no longer controls region
    // aggregation for them.
    int32_t chunk_load_radius = 1;
    // Large builds run on asynchronous chunk pipelines; three seconds gives a
    // newly added tickingarea time to become usable before placement begins.
    int32_t chunk_wait_ticks = 60;
    int32_t blocks_per_second = 20;
    // These fields are checkpointed.  In particular, a checkpoint created
    // with full capability must never be silently resumed as an unverified
    // limited import after the capability expires.
    ImportSourceType source_type = ImportSourceType::Schematic;
    BuildToolsAccessProfile access_profile = BuildToolsAccessProfile::Full;
    int32_t ticking_area_min_y = 0;
    int32_t ticking_area_max_y = 255;
    int32_t teleport_y_offset = 50;
    // Region command spools use a grid of region_span logical chunks.  The
    // origin is selected from the parsed content so a build that starts at an
    // arbitrary chunk boundary does not get split by the world-origin grid.
    // It is persisted with the checkpoint because loading, verification and
    // repair must all calculate exactly the same region membership.
    ChunkCoord region_grid_origin;
    OverwritePolicy overwrite_policy = OverwritePolicy::PreserveExisting;
    bool verify_after_import = true;
    VerificationPrecision verification_precision = VerificationPrecision::Thorough;
    // Pixel-art-only post-import task. Persist the user's choice in the main
    // checkpoint so a missing map cursor sidecar cannot silently skip it.
    bool create_maps_after_import = false;
    // When enabled, the runtime places a single minecraft:deny layer below the
    // complete imported source volume before the structure commands run.  The
    // bounds are filled after parsing, while the controller is still planning.
    bool place_deny_layer = false;
    BlockBounds deny_layer_bounds;
    // When false the runtime skips the sendcommandfeedback gamerule entirely,
    // leaving the game's default feedback behaviour intact.  Defaults to true
    // so imports that do not pass this field stay suppressed as before.
    bool suppress_command_feedback = true;
    std::string checkpoint_path;
};

inline bool isValidSimulationChunkRange(int32_t range) {
    return range >= ImportConfig::kMinimumSimulationChunkRange &&
           range <= ImportConfig::kMaximumSimulationChunkRange;
}

inline int32_t legacyRegionSpanForChunkLoadRadius(int32_t radius) {
    return std::max<int32_t>(1, radius * 2 + 1);
}

// A zero simulation range is an explicit marker for a checkpoint written by
// an older scheduler.  It preserves that checkpoint's old logical-chunk
// geometry while all fresh imports use the exact native-chunk range above.
inline int32_t regionSpanForImportConfig(const ImportConfig& config) {
    if (config.simulation_chunk_range <= 0) {
        return legacyRegionSpanForChunkLoadRadius(config.chunk_load_radius);
    }
    const int64_t region_blocks = static_cast<int64_t>(config.simulation_chunk_range) *
        ImportConfig::kVanillaChunkSize;
    const int32_t logical_chunk_size = std::max<int32_t>(1, config.chunk_size);
    // Fresh configurations require a 16-block logical chunk, but use a ceiling
    // here so a corrupt checkpoint can never produce a work region smaller than
    // the native simulation range it recorded.
    return static_cast<int32_t>(std::max<int64_t>(
        1, (region_blocks + logical_chunk_size - 1) / logical_chunk_size));
}

struct ImportIdentity {
    std::string job_id;
    std::string source_file;
    std::string source_hash;
    std::string options_hash;
    std::string mapper_version;
};

// The parser owns the spool contents. The controller keeps only this small
// descriptor so that command scheduling never stores a full blueprint in RAM.
struct ChunkDescriptor {
    ChunkCoord coord;
    // Set by CommandSpoolBuilder after it aggregates logical chunk spools into
    // a region command spool.  Pre-aggregation descriptors deliberately keep
    // the default origin; the controller adopts the common value at planning
    // completion.
    ChunkCoord region_grid_origin;
    BlockBounds imported_bounds;
    std::array<bool, kImportPhaseCount> has_phase{};
    std::array<std::string, kImportPhaseCount> spool_paths{};
    std::array<std::string, kImportPhaseCount> command_paths{};
    std::array<uint64_t, kImportPhaseCount> phase_block_counts{};
    // BDX exporters occasionally emit duplicate coordinates; skipped entries
    // are counted here rather than aborting the entire import.
    uint64_t duplicate_block_count = 0;
};

struct WorkUnit {
    uint64_t sequence = 0;
    ChunkCoord coord;
    ChunkCoord region_coord;
    ChunkCoord region_grid_origin;
    int32_t region_span = 1;
    ImportPhase phase = ImportPhase::Structure;
    BlockBounds imported_bounds;
    BlockBounds load_bounds;
    int32_t wait_ticks = 40;
    std::string spool_path;
    uint64_t block_count = 0;
    // Overwrite imports clear a logical partition immediately before its
    // first placement phase. This avoids leaving a very large build empty
    // during a separate global clear sweep.
    bool clear_before_build = false;
    // Auxiliary only: this unit owns the horizontal slice of the optional
    // minecraft:deny foundation. It is intentionally not represented in a
    // source command spool or in block-progress accounting.
    bool place_deny_foundation = false;
};

inline bool hasSpoolForPhase(const ChunkDescriptor& descriptor, ImportPhase phase) {
    const size_t index = phaseIndex(phase);
    return descriptor.has_phase[index] &&
           (!descriptor.command_paths[index].empty() || !descriptor.spool_paths[index].empty());
}

// C++ integer division truncates toward zero. Chunk lookup must use floor
// division so coordinates immediately below zero stay in negative chunks.
inline int32_t floorDiv(int32_t value, int32_t divisor) {
    if (divisor <= 0) return 0;
    int32_t quotient = value / divisor;
    const int32_t remainder = value % divisor;
    if (remainder != 0 && value < 0) --quotient;
    return quotient;
}

inline ChunkCoord chunkForBlock(int32_t block_x, int32_t block_z, int32_t chunk_size) {
    return {floorDiv(block_x, chunk_size), floorDiv(block_z, chunk_size)};
}

inline int64_t floorDiv64(int64_t value, int32_t divisor) {
    if (divisor <= 0) return 0;
    int64_t quotient = value / divisor;
    const int64_t remainder = value % divisor;
    if (remainder != 0 && value < 0) --quotient;
    return quotient;
}

inline ChunkCoord regionForChunk(const ChunkCoord& chunk, int32_t region_span,
                                 const ChunkCoord& origin = {}) {
    const int32_t span = std::max<int32_t>(1, region_span);
    // A span of one has no aggregation benefit and preserving the historical
    // direct mapping avoids unnecessary relative-coordinate edge cases.
    if (span == 1) return chunk;
    return {
        static_cast<int32_t>(floorDiv64(
            static_cast<int64_t>(chunk.x) - origin.x, span)),
        static_cast<int32_t>(floorDiv64(
            static_cast<int64_t>(chunk.z) - origin.z, span)),
    };
}

inline int64_t regionMinChunkCoordinate(int32_t region_coordinate, int32_t region_span,
                                        int32_t origin_coordinate) {
    return static_cast<int64_t>(origin_coordinate) +
           static_cast<int64_t>(region_coordinate) *
               std::max<int32_t>(1, region_span);
}

inline ChunkCoord selectRegionGridOrigin(const std::vector<ChunkDescriptor>& chunks,
                                         int32_t region_span) {
    if (chunks.empty() || region_span <= 1) return {};
    ChunkCoord origin = chunks.front().coord;
    for (const ChunkDescriptor& descriptor : chunks) {
        origin.x = std::min(origin.x, descriptor.coord.x);
        origin.z = std::min(origin.z, descriptor.coord.z);
    }
    return origin;
}

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BUILD_IMPORT_TYPES_H
