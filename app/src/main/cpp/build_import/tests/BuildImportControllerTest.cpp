#include "../BuildImportController.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using namespace build_import;

namespace {

namespace fs = std::filesystem;

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path root = fs::temp_directory_path();
        for (int attempt = 0; attempt != 100; ++attempt) {
            path_ = root / ("build_import_controller_test_" + std::to_string(nonce) + "_" +
                            std::to_string(attempt));
            std::error_code error;
            if (fs::create_directory(path_, error)) return;
        }
        throw std::runtime_error("cannot create controller test directory");
    }

    ~ScopedTempDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    fs::path file(const std::string& name) const {
        return path_ / name;
    }

private:
    fs::path path_;
};

ChunkDescriptor makeChunk(
    int32_t x, int32_t z,
    std::initializer_list<ImportPhase> phases = {ImportPhase::Structure,
                                                  ImportPhase::Attachment}) {
    ChunkDescriptor descriptor;
    descriptor.coord = {x, z};
    descriptor.imported_bounds = {x * 32, 0, z * 32, x * 32 + 31, 20, z * 32 + 31};
    for (const ImportPhase phase : phases) {
        const size_t index = phaseIndex(phase);
        const std::string stem = std::to_string(x) + "_" + std::to_string(z) + "_" +
                                 std::to_string(static_cast<unsigned>(phase));
        descriptor.has_phase[index] = true;
        descriptor.spool_paths[index] = stem + ".spool";
        descriptor.command_paths[index] = stem + ".commands";
        descriptor.phase_block_counts[index] = phase == ImportPhase::Structure ? 100 : 10;
    }
    return descriptor;
}

ChunkDescriptor makeCompleteChunk(int32_t x, int32_t z) {
    return makeChunk(x, z, {ImportPhase::Structure, ImportPhase::Gravity,
                            ImportPhase::Attachment, ImportPhase::Fluid,
                            ImportPhase::DependentAttachment});
}

ImportIdentity makeIdentity(const std::string& suffix) {
    ImportIdentity identity;
    identity.job_id = "controller-test-" + suffix;
    identity.source_file = "fixture-" + suffix + ".schem";
    identity.source_hash = "source-hash-" + suffix;
    identity.options_hash = "options-hash-" + suffix;
    identity.mapper_version = "test-v1";
    return identity;
}

ImportConfig makeConfig(const fs::path& checkpoint_path, OverwritePolicy policy) {
    ImportConfig config;
    config.overwrite_policy = policy;
    config.checkpoint_path = checkpoint_path.string();
    return config;
}

void startImport(BuildImportController* controller, const ImportIdentity& identity,
                 const WorldContext& world, const ImportConfig& config,
                 const std::vector<ChunkDescriptor>& descriptors) {
    assert(controller->startPlanning(identity, world, config));
    for (const ChunkDescriptor& descriptor : descriptors) {
        assert(controller->addChunk(descriptor));
    }
    assert(controller->finishPlanning());
}

WorkUnit acquireExpected(BuildImportController* controller, ImportPhase phase, int32_t x,
                         int32_t z) {
    const std::optional<WorkUnit> unit = controller->acquireNextUnit();
    assert(unit);
    if (unit->phase != phase || unit->coord.x != x || unit->coord.z != z) {
        std::fprintf(stderr,
                     "unexpected unit: expected phase=%u coord=(%d,%d), "
                     "actual phase=%u coord=(%d,%d)\n",
                     static_cast<unsigned>(phase), x, z,
                     static_cast<unsigned>(unit->phase), unit->coord.x,
                     unit->coord.z);
    }
    assert(unit->phase == phase);
    assert(unit->coord.x == x);
    assert(unit->coord.z == z);
    return *unit;
}

void completeExpected(BuildImportController* controller, ImportPhase phase, int32_t x,
                      int32_t z) {
    const WorkUnit unit = acquireExpected(controller, phase, x, z);
    assert(controller->completeActiveUnit(unit.block_count));
}

void assertTerminalPosition(const CheckpointSnapshot& snapshot, size_t chunk_count) {
    assert(snapshot.phase_index == 0);
    assert(snapshot.chunk_index == chunk_count);
    assert(!snapshot.has_active_unit);
    assert(snapshot.active_spool_offset == 0);
}

void assertClearPlanCoverage(const std::vector<PlannedCommand>& plan,
                             const BlockBounds& expected_bounds,
                             uint64_t command_volume_limit) {
    assert(expected_bounds.isValid());
    assert(!plan.empty());

    const uint64_t width = static_cast<uint64_t>(
        static_cast<int64_t>(expected_bounds.max_x) - expected_bounds.min_x + 1);
    const uint64_t height = static_cast<uint64_t>(
        static_cast<int64_t>(expected_bounds.max_y) - expected_bounds.min_y + 1);
    const uint64_t depth = static_cast<uint64_t>(
        static_cast<int64_t>(expected_bounds.max_z) - expected_bounds.min_z + 1);
    const uint64_t expected_volume = width * height * depth;
    assert(expected_volume <= std::numeric_limits<size_t>::max());

    std::vector<uint8_t> coverage(static_cast<size_t>(expected_volume), 0);
    uint64_t planned_volume = 0;
    int32_t previous_max_y = expected_bounds.max_y;
    bool first = true;
    for (const PlannedCommand& command : plan) {
        const BlockBounds& bounds = command.bounds;
        assert(bounds.isValid());
        assert(bounds.min_x >= expected_bounds.min_x &&
               bounds.max_x <= expected_bounds.max_x);
        assert(bounds.min_y >= expected_bounds.min_y &&
               bounds.max_y <= expected_bounds.max_y);
        assert(bounds.min_z >= expected_bounds.min_z &&
               bounds.max_z <= expected_bounds.max_z);
        assert(command.name == "minecraft:air");
        assert(command.aux == 0);

        // Clear from the top down so gravity blocks cannot fall back into a
        // lower slab that was already cleared.
        if (first) {
            assert(bounds.max_y == expected_bounds.max_y);
            first = false;
        } else {
            assert(bounds.max_y <= previous_max_y);
        }
        previous_max_y = bounds.max_y;

        const uint64_t command_width = static_cast<uint64_t>(
            static_cast<int64_t>(bounds.max_x) - bounds.min_x + 1);
        const uint64_t command_height = static_cast<uint64_t>(
            static_cast<int64_t>(bounds.max_y) - bounds.min_y + 1);
        const uint64_t command_depth = static_cast<uint64_t>(
            static_cast<int64_t>(bounds.max_z) - bounds.min_z + 1);
        const uint64_t command_volume = command_width * command_height * command_depth;
        assert(command_volume != 0 && command_volume <= command_volume_limit);
        assert(command.block_count == command_volume);
        planned_volume += command_volume;

        for (int64_t y = bounds.min_y; y <= bounds.max_y; ++y) {
            for (int64_t z = bounds.min_z; z <= bounds.max_z; ++z) {
                for (int64_t x = bounds.min_x; x <= bounds.max_x; ++x) {
                    const uint64_t index =
                        static_cast<uint64_t>(y - expected_bounds.min_y) * depth * width +
                        static_cast<uint64_t>(z - expected_bounds.min_z) * width +
                        static_cast<uint64_t>(x - expected_bounds.min_x);
                    assert(index < coverage.size());
                    assert(coverage[static_cast<size_t>(index)] == 0);
                    coverage[static_cast<size_t>(index)] = 1;
                }
            }
        }
    }

    assert(planned_volume == expected_volume);
    assert(std::all_of(coverage.begin(), coverage.end(),
                       [](uint8_t visits) { return visits == 1; }));
}

WorkUnit acquirePhase(BuildImportController* controller, ImportPhase target) {
    for (size_t guard = 0; guard != 1024; ++guard) {
        const std::optional<WorkUnit> unit = controller->acquireNextUnit();
        assert(unit);
        if (unit->phase == target) return *unit;
        assert(controller->completeActiveUnit(unit->phase == ImportPhase::Clear
                                                  ? 0
                                                  : unit->block_count));
    }
    assert(false && "target import phase was not reached");
    return {};
}

void assertDynamicRollbackSnapshot(const CheckpointSnapshot& snapshot) {
    assert(snapshot.format_version == kBuildImportCheckpointCurrentVersion);
    assert(snapshot.state == ImportState::Paused);
    assert(snapshot.phase_index == phaseIndex(ImportPhase::Clear));
    assert(snapshot.chunk_index == 0);
    assert(!snapshot.has_active_unit);
    assert(snapshot.active_spool_offset == 0);
    assert(snapshot.completed_command_count == 0);
    assert(snapshot.completed_block_count == 0);
    assert(snapshot.verification_sample_index == 0);
}

void completeGlobalRollback(BuildImportController* controller,
                            const std::vector<int32_t>& sorted_x) {
    for (const int32_t x : sorted_x) {
        const WorkUnit clear = acquireExpected(controller, ImportPhase::Clear, x, 0);
        assert(!clear.clear_before_build);
        assert(controller->completeActiveUnit(0));
    }
    for (const ImportPhase phase : kPlacementPhaseOrder) {
        for (const int32_t x : sorted_x) {
            const WorkUnit unit = acquireExpected(controller, phase, x, 0);
            assert(controller->completeActiveUnit(unit.block_count));
        }
    }
}

void testCoordinateFloorDivision() {
    assert(floorDiv(-1, 32) == -1);
    assert(floorDiv(-32, 32) == -1);
    assert(floorDiv(-33, 32) == -2);
    const ChunkCoord negative_chunk = chunkForBlock(-1, -33, 32);
    assert(negative_chunk.x == -1);
    assert(negative_chunk.z == -2);
    const ChunkCoord negative_region = regionForChunk(ChunkCoord{-4, -1}, 3);
    assert(negative_region.x == -2);
    assert(negative_region.z == -1);
}

void testClearCommandSplitting() {
    assert(verificationStableSampleLimit(VerificationPrecision::Fast) == 8);
    assert(verificationAirSampleLimit(VerificationPrecision::Fast) == 2);
    assert(verificationStableSampleLimit(VerificationPrecision::Balanced) == 12);
    assert(verificationAirSampleLimit(VerificationPrecision::Balanced) == 3);
    assert(verificationStableSampleLimit(VerificationPrecision::Thorough) == 16);
    assert(verificationAirSampleLimit(VerificationPrecision::Thorough) == 4);

    const WorldContext world{"server-a", 0};
    WorkUnit tall_clear;
    tall_clear.phase = ImportPhase::Clear;
    // Irregular negative-origin bounds exercise both horizontal and vertical
    // tail fragments instead of only dimensions that divide the rate limits.
    tall_clear.imported_bounds = {-5, -3, 7, 26, 251, 37};
    tall_clear.load_bounds = {-64, 0, 96, 31, 255, 191};

    BuildImportController cautious_controller;
    ImportConfig cautious_config;
    cautious_config.overwrite_policy = OverwritePolicy::ClearImportedBounds;
    cautious_config.blocks_per_second = 20;
    assert(cautious_controller.startPlanning(makeIdentity("clear-cautious"), world,
                                              cautious_config));
    assert(cautious_controller.worldMatches(world));
    assert(!cautious_controller.worldMatches({"server-b", 0}));
    assert(cautious_controller.completedBlockCount() == 0);
    const BuildImportRuntimeMetadata runtime_metadata =
        cautious_controller.runtimeMetadata();
    assert(runtime_metadata.chunk_size == cautious_config.chunk_size);
    assert(runtime_metadata.simulation_chunk_range ==
           cautious_config.simulation_chunk_range);
    assert(runtime_metadata.region_span == cautious_config.simulation_chunk_range);
    assert(runtime_metadata.chunk_load_radius == cautious_config.chunk_load_radius);
    assert(runtime_metadata.chunk_wait_ticks == cautious_config.chunk_wait_ticks);
    assert(runtime_metadata.region_grid_origin == cautious_config.region_grid_origin);
    assert(runtime_metadata.ticking_area_min_y == cautious_config.ticking_area_min_y);
    assert(runtime_metadata.ticking_area_max_y == cautious_config.ticking_area_max_y);
    assert(runtime_metadata.overwrite_policy == cautious_config.overwrite_policy);
    assert(runtime_metadata.verify_after_import == cautious_config.verify_after_import);
    assert(runtime_metadata.verification_precision == cautious_config.verification_precision);
    assert(runtime_metadata.phase_index == 0);
    assert(runtime_metadata.verification_sample_index == 0);
    const std::vector<PlannedCommand> cautious_plan =
        cautious_controller.makeClearPlan(tall_clear);
    assertClearPlanCoverage(cautious_plan, tall_clear.imported_bounds,
                            fillBlockLimitForRate(cautious_config.blocks_per_second));
    assert(cautious_controller.makeClearCommands(tall_clear).size() == cautious_plan.size());
    assert(cautious_controller.makeClearCommand(tall_clear));

    BuildImportController fast_controller;
    ImportConfig fast_config;
    fast_config.overwrite_policy = OverwritePolicy::ClearImportedBounds;
    fast_config.blocks_per_second = 5000;
    assert(fast_controller.startPlanning(makeIdentity("clear-fast"), world, fast_config));
    const std::vector<PlannedCommand> fast_plan = fast_controller.makeClearPlan(tall_clear);
    assertClearPlanCoverage(fast_plan, tall_clear.imported_bounds,
                            fillBlockLimitForRate(fast_config.blocks_per_second));
    assert(fast_controller.makeClearCommands(tall_clear).size() == fast_plan.size());
    assert(fast_plan.size() < cautious_plan.size());

    BuildImportController turbo_controller;
    ImportConfig turbo_config;
    turbo_config.overwrite_policy = OverwritePolicy::ClearImportedBounds;
    turbo_config.blocks_per_second = kMaximumBlocksPerSecond;
    assert(turbo_controller.startPlanning(makeIdentity("clear-turbo"), world, turbo_config));
    const std::vector<PlannedCommand> turbo_plan = turbo_controller.makeClearPlan(tall_clear);
    assertClearPlanCoverage(turbo_plan, tall_clear.imported_bounds,
                            kMaximumScheduledFillBlockCount);
    assert(turbo_plan.size() < fast_plan.size());

    ImportConfig excessive_config = turbo_config;
    excessive_config.blocks_per_second = kMaximumBlocksPerSecond + 1;
    BuildImportController excessive_controller;
    assert(!excessive_controller.startPlanning(
        makeIdentity("clear-excessive"), world, excessive_config));

    ImportConfig invalid_verification_config = cautious_config;
    invalid_verification_config.verification_precision =
        static_cast<VerificationPrecision>(255);
    BuildImportController invalid_verification_controller;
    assert(!invalid_verification_controller.startPlanning(
        makeIdentity("invalid-verification"), world, invalid_verification_config));

    ImportConfig too_small_range = cautious_config;
    too_small_range.simulation_chunk_range =
        ImportConfig::kMinimumSimulationChunkRange - 1;
    BuildImportController too_small_range_controller;
    assert(!too_small_range_controller.startPlanning(
        makeIdentity("simulation-range-small"), world, too_small_range));

    ImportConfig too_large_range = cautious_config;
    too_large_range.simulation_chunk_range =
        ImportConfig::kMaximumSimulationChunkRange + 1;
    BuildImportController too_large_range_controller;
    assert(!too_large_range_controller.startPlanning(
        makeIdentity("simulation-range-large"), world, too_large_range));

    ImportConfig non_native_grid = cautious_config;
    non_native_grid.chunk_size = 32;
    BuildImportController non_native_grid_controller;
    assert(!non_native_grid_controller.startPlanning(
        makeIdentity("simulation-range-nonnative"), world, non_native_grid));

    const std::vector<std::string> prepare =
        cautious_controller.makePrepareCommands(tall_clear);
    assert(prepare.size() == 2);
    assert(prepare.front() ==
           "/tickingarea add -64 0 96 31 255 191 infinitecz_build true");
    assert(prepare[1] == "/tp @s -16 301 143");

    // A player close to the region centre needs no intermediate hops.
    const std::vector<std::string> near_prepare =
        cautious_controller.makePrepareCommands(tall_clear, true, -20, 150);
    assert(near_prepare == prepare);

    // A distant player approaches the centre through bounded /tp hops so a
    // server-side movement guard never sees one long-distance teleport. The
    // chain still ends with the exact centre teleport.
    const std::vector<std::string> far_prepare =
        cautious_controller.makePrepareCommands(tall_clear, true, -16, 5143);
    assert(far_prepare.size() > 3);
    assert(far_prepare.front() ==
           "/tickingarea add -64 0 96 31 255 191 infinitecz_build true");
    assert(far_prepare.back() == "/tp @s -16 301 143");
    long long previous_x = -16;
    long long previous_z = 5143;
    for (size_t index = 1; index < far_prepare.size(); ++index) {
        long long hop_x = 0;
        long long hop_y = 0;
        long long hop_z = 0;
        assert(std::sscanf(far_prepare[index].c_str(), "/tp @s %lld %lld %lld",
                           &hop_x, &hop_y, &hop_z) == 3);
        assert(hop_y == 301);
        const double step = std::hypot(static_cast<double>(hop_x - previous_x),
                                       static_cast<double>(hop_z - previous_z));
        assert(step <= 96.0 + 2.0);
        previous_x = hop_x;
        previous_z = hop_z;
    }
    assert(previous_x == -16 && previous_z == 143);
}

void testDenyFoundationPlanning(const ScopedTempDirectory& temp) {
    const WorldContext world{"deny-foundation-world", 0};
    ImportConfig config = makeConfig(temp.file("deny-foundation.checkpoint"),
                                     OverwritePolicy::PreserveExisting);
    config.blocks_per_second = 5000;
    config.place_deny_layer = true;

    BuildImportController controller;
    assert(controller.startPlanning(makeIdentity("deny-foundation"), world, config));
    const BlockBounds foundation{0, -1, 0, 127, -1, 63};
    assert(controller.setPlanningDenyLayerBounds(foundation));
    const CheckpointSnapshot planned = controller.checkpointSnapshot();
    assert(planned.config.place_deny_layer);
    assert(planned.config.deny_layer_bounds.min_y == -1);
    assert(planned.config.deny_layer_bounds.max_y == -1);

    // The second descriptor represents an air-only source-volume partition.
    // It must remain scheduled in preserve mode so the foundation has no gap.
    ChunkDescriptor source = makeChunk(0, 0, {ImportPhase::Structure});
    source.imported_bounds = {0, 0, 0, 15, 20, 15};
    // Put the retained empty descriptor in the next simulation-sized work
    // region. Production command planning aggregates each region before it
    // reaches the controller, so each unit owns one full 4x4-chunk foundation
    // slice even when its source bounds are only a small local cuboid.
    ChunkDescriptor empty = makeChunk(4, 0, {});
    empty.imported_bounds = {64, 0, 0, 79, 20, 15};
    assert(controller.addChunk(source));
    assert(controller.addChunk(empty));
    assert(controller.finishPlanning());

    const WorkUnit source_unit = acquireExpected(&controller, ImportPhase::Structure, 0, 0);
    assert(source_unit.place_deny_foundation);
    const std::vector<PlannedCommand> source_foundation =
        controller.makeDenyFoundationPlan(source_unit);
    assert(source_foundation.size() == 1);
    assert(source_foundation.front().name == "minecraft:deny");
    assert(source_foundation.front().bounds.min_x == 0);
    assert(source_foundation.front().bounds.max_x == 63);
    assert(source_foundation.front().bounds.min_y == -1);
    assert(source_foundation.front().bounds.max_y == -1);
    assert(source_foundation.front().bounds.min_z == 0);
    assert(source_foundation.front().bounds.max_z == 63);
    assert(source_foundation.front().block_count == 64U * 64U);
    assert(controller.completeActiveUnit(source_unit.block_count));

    const WorkUnit foundation_only = acquireExpected(&controller, ImportPhase::Clear, 4, 0);
    assert(foundation_only.place_deny_foundation);
    const std::vector<PlannedCommand> empty_foundation =
        controller.makeDenyFoundationPlan(foundation_only);
    assert(empty_foundation.size() == 1);
    assert(empty_foundation.front().name == "minecraft:deny");
    assert(empty_foundation.front().bounds.min_x == 64);
    assert(empty_foundation.front().bounds.max_x == 127);
    assert(empty_foundation.front().bounds.min_y == -1);
    assert(empty_foundation.front().bounds.max_y == -1);
    assert(empty_foundation.front().bounds.max_z == 63);
    assert(empty_foundation.front().block_count == 64U * 64U);
    assert(controller.completeActiveUnit(0));
    assert(controller.state() == ImportState::Verifying);

    std::string error;
    assert(!controller.setPlanningDenyLayerBounds(foundation, &error));
    assert(!error.empty());
}

void testSimulationChunkRangeGeometry() {
    const WorldContext world{"server-simulation-range", 0};
    assert(isValidSimulationChunkRange(4));
    assert(isValidSimulationChunkRange(8));
    assert(!isValidSimulationChunkRange(3));
    assert(!isValidSimulationChunkRange(9));

    ImportConfig four_chunk_config;
    four_chunk_config.simulation_chunk_range = 4;
    BuildImportController four_chunk_controller;
    startImport(&four_chunk_controller, makeIdentity("simulation-four"), world,
                four_chunk_config, {makeChunk(0, 0, {ImportPhase::Structure})});
    const WorkUnit four_chunk_unit =
        acquireExpected(&four_chunk_controller, ImportPhase::Structure, 0, 0);
    // Core: 4x4 native chunks = 64x64 blocks.  The load area keeps a one
    // native-chunk halo around it for neighboring updates.
    assert(four_chunk_unit.region_span == 4);
    assert(four_chunk_unit.load_bounds.min_x == -16);
    assert(four_chunk_unit.load_bounds.max_x == 79);
    assert(four_chunk_unit.load_bounds.min_z == -16);
    assert(four_chunk_unit.load_bounds.max_z == 79);

    ImportConfig eight_chunk_config;
    eight_chunk_config.simulation_chunk_range = 8;
    BuildImportController eight_chunk_controller;
    startImport(&eight_chunk_controller, makeIdentity("simulation-eight"), world,
                eight_chunk_config, {makeChunk(8, 8, {ImportPhase::Structure})});
    const WorkUnit eight_chunk_unit =
        acquireExpected(&eight_chunk_controller, ImportPhase::Structure, 8, 8);
    assert(eight_chunk_unit.region_span == 8);
    assert(eight_chunk_unit.region_coord == ChunkCoord({1, 1}));
    assert(eight_chunk_unit.load_bounds.min_x == 112);
    assert(eight_chunk_unit.load_bounds.max_x == 271);
    assert(eight_chunk_unit.load_bounds.min_z == 112);
    assert(eight_chunk_unit.load_bounds.max_z == 271);
}

void testWorkUnitCompletionInvariants(const ScopedTempDirectory& temp) {
    const WorldContext world{"server-invariants", 0};
    const std::vector<ChunkDescriptor> descriptors = {makeChunk(0, 0)};

    BuildImportController preserve;
    startImport(&preserve, makeIdentity("counts"), world,
                makeConfig(temp.file("counts.chk"), OverwritePolicy::PreserveExisting),
                descriptors);
    acquireExpected(&preserve, ImportPhase::Structure, 0, 0);
    std::string error;
    assert(!preserve.completeActiveUnit(99, &error));
    assert(!error.empty());
    assert(preserve.checkpointSnapshot().has_active_unit);
    assert(preserve.completeActiveUnit(100));

    BuildImportController clear;
    startImport(&clear, makeIdentity("clear-count"), world,
                makeConfig(temp.file("clear-count.chk"),
                           OverwritePolicy::ClearImportedBounds),
                descriptors);
    const WorkUnit clear_then_build =
        acquireExpected(&clear, ImportPhase::Structure, 0, 0);
    assert(clear_then_build.clear_before_build);
    error.clear();
    assert(!clear.completeActiveUnit(1, &error));
    assert(clear.checkpointSnapshot().has_active_unit);
    assert(clear.completeActiveUnit(100));

    CheckpointSnapshot overflow;
    overflow.identity = makeIdentity("overflow");
    overflow.world = world;
    overflow.config = makeConfig(temp.file("overflow.chk"),
                                 OverwritePolicy::PreserveExisting);
    overflow.state = ImportState::Running;
    overflow.phase_index = static_cast<uint32_t>(phaseIndex(ImportPhase::Attachment));
    overflow.chunk_index = 0;
    overflow.completed_block_count = std::numeric_limits<uint64_t>::max() - 5;
    BuildImportController restored;
    error.clear();
    assert(!restored.restoreFromCheckpoint(overflow, descriptors, &error));
    assert(!error.empty());
}

void testRestoreFailureIsTransactional(const ScopedTempDirectory& temp) {
    const WorldContext old_world{"server-old", 0};
    const std::vector<ChunkDescriptor> old_descriptors = {makeChunk(-1, 0)};
    BuildImportController controller;
    startImport(&controller, makeIdentity("old"), old_world,
                makeConfig(temp.file("old.chk"), OverwritePolicy::PreserveExisting),
                old_descriptors);
    acquireExpected(&controller, ImportPhase::Structure, -1, 0);
    const CheckpointSnapshot before = controller.checkpointSnapshot();

    CheckpointSnapshot candidate;
    candidate.identity = makeIdentity("candidate");
    candidate.world = {"server-candidate", 2};
    candidate.config = makeConfig(temp.file("missing-parent") / "candidate.chk",
                                  OverwritePolicy::PreserveExisting);
    candidate.state = ImportState::Running;
    candidate.phase_index = static_cast<uint32_t>(phaseIndex(ImportPhase::Structure));
    candidate.chunk_index = 0;
    std::string error;
    assert(!controller.restoreFromCheckpoint(candidate, {makeChunk(5, 5)}, &error));
    assert(!error.empty());

    const CheckpointSnapshot after = controller.checkpointSnapshot();
    assert(after.identity.job_id == before.identity.job_id);
    assert(after.world == before.world);
    assert(after.state == before.state);
    assert(after.phase_index == before.phase_index);
    assert(after.chunk_index == before.chunk_index);
    assert(after.has_active_unit == before.has_active_unit);
    assert(after.active_coord == before.active_coord);
    assert(after.active_phase == before.active_phase);
}

void testInterruptedUnitReplayAndCheckpointTransactions(const ScopedTempDirectory& temp) {
    const WorldContext world{"server-replay", 0};
    const std::vector<ChunkDescriptor> descriptors = {makeChunk(0, 0)};

    const fs::path replay_path = temp.file("replay.chk");
    BuildImportController replay;
    startImport(&replay, makeIdentity("replay"), world,
                makeConfig(replay_path, OverwritePolicy::PreserveExisting), descriptors);
    acquireExpected(&replay, ImportPhase::Structure, 0, 0);
    replay.recordActiveProgress(7);
    replay.pause();
    CheckpointSnapshot paused = replay.checkpointSnapshot();
    assert(paused.has_active_unit);
    assert(paused.active_spool_offset == 7);
    assert(paused.completed_command_count == 7);
    assert(replay.resume(world));
    const CheckpointSnapshot resumed = replay.checkpointSnapshot();
    assert(!resumed.has_active_unit);
    assert(resumed.active_spool_offset == 0);
    assert(resumed.completed_command_count == 0);
    const std::optional<CheckpointSnapshot> durable =
        BuildImportCheckpoint::load(replay_path.string());
    assert(durable);
    assert(!durable->has_active_unit);
    assert(durable->active_spool_offset == 0);
    assert(durable->completed_command_count == 0);

    const fs::path transaction_dir = temp.file("transaction");
    assert(fs::create_directory(transaction_dir));
    const fs::path transaction_path = transaction_dir / "state.chk";
    BuildImportController transaction;
    startImport(&transaction, makeIdentity("transaction"), world,
                makeConfig(transaction_path, OverwritePolicy::PreserveExisting), descriptors);
    acquireExpected(&transaction, ImportPhase::Structure, 0, 0);
    transaction.recordActiveProgress(3);
    transaction.pause();
    const CheckpointSnapshot before = transaction.checkpointSnapshot();
    std::error_code remove_error;
    fs::remove_all(transaction_dir, remove_error);
    assert(!remove_error);
    std::string error;
    assert(!transaction.resume(world, &error));
    assert(!error.empty());
    const CheckpointSnapshot after = transaction.checkpointSnapshot();
    assert(after.state == before.state);
    assert(after.has_active_unit == before.has_active_unit);
    assert(after.active_spool_offset == before.active_spool_offset);
    assert(after.completed_command_count == before.completed_command_count);
    assert(after.phase_index == before.phase_index);
    assert(after.chunk_index == before.chunk_index);
}

void testDynamicPauseRecovery(const ScopedTempDirectory& temp) {
    const WorldContext world{"server-dynamic-pause", 0};
    const std::vector<ChunkDescriptor> descriptors = {
        makeCompleteChunk(1, 0),
        makeCompleteChunk(-1, 0),
    };
    const std::vector<int32_t> sorted_x = {-1, 1};
    const std::array<ImportPhase, 4> dynamic_phases = {
        ImportPhase::Gravity,
        ImportPhase::Fluid,
        ImportPhase::Attachment,
        ImportPhase::DependentAttachment,
    };

    for (const ImportPhase interrupted_phase : dynamic_phases) {
        const std::string suffix =
            "dynamic-overwrite-" + std::to_string(phaseIndex(interrupted_phase));
        const fs::path checkpoint_path = temp.file(suffix + ".chk");
        BuildImportController controller;
        startImport(&controller, makeIdentity(suffix), world,
                    makeConfig(checkpoint_path,
                               OverwritePolicy::ClearImportedBounds),
                    descriptors);
        const WorkUnit interrupted = acquirePhase(&controller, interrupted_phase);
        assert(interrupted.coord.x == -1);
        controller.recordActiveProgress(3);
        assert(controller.pause());

        assertDynamicRollbackSnapshot(controller.checkpointSnapshot());
        const std::optional<CheckpointSnapshot> durable =
            BuildImportCheckpoint::load(checkpoint_path.string());
        assert(durable);
        assertDynamicRollbackSnapshot(*durable);

        assert(controller.resume(world));
        completeGlobalRollback(&controller, sorted_x);
        assert(controller.state() == ImportState::Verifying);
        const CheckpointSnapshot terminal = controller.checkpointSnapshot();
        assertTerminalPosition(terminal, descriptors.size());
        assert(terminal.completed_block_count == 280);
    }
}

void testPreserveDynamicPauseCannotResume(const ScopedTempDirectory& temp) {
    const WorldContext world{"server-dynamic-preserve", 0};
    const std::vector<ChunkDescriptor> descriptors = {makeCompleteChunk(0, 0)};
    const std::array<ImportPhase, 4> dynamic_phases = {
        ImportPhase::Gravity,
        ImportPhase::Fluid,
        ImportPhase::Attachment,
        ImportPhase::DependentAttachment,
    };

    for (const ImportPhase interrupted_phase : dynamic_phases) {
        const std::string suffix =
            "dynamic-preserve-" + std::to_string(phaseIndex(interrupted_phase));
        BuildImportController controller;
        startImport(&controller, makeIdentity(suffix), world,
                    makeConfig(temp.file(suffix + ".chk"),
                               OverwritePolicy::PreserveExisting),
                    descriptors);
        acquirePhase(&controller, interrupted_phase);
        controller.recordActiveProgress(2);
        assert(controller.pause());
        const CheckpointSnapshot paused = controller.checkpointSnapshot();
        assert(paused.state == ImportState::Paused);
        assert(paused.has_active_unit);
        assert(paused.active_phase == interrupted_phase);

        std::string error;
        assert(!controller.resume(world, &error));
        assert(!error.empty());
        const CheckpointSnapshot rejected = controller.checkpointSnapshot();
        assert(rejected.state == ImportState::Paused);
        assert(rejected.has_active_unit);
        assert(rejected.active_phase == interrupted_phase);
    }
}

void testCurrentDynamicCheckpointRestoration(const ScopedTempDirectory& temp) {
    const WorldContext world{"server-v6-dynamic", 0};
    const std::vector<ChunkDescriptor> descriptors = {makeCompleteChunk(0, 0)};
    const std::array<ImportPhase, 4> dynamic_phases = {
        ImportPhase::Gravity,
        ImportPhase::Fluid,
        ImportPhase::Attachment,
        ImportPhase::DependentAttachment,
    };

    for (const ImportPhase interrupted_phase : dynamic_phases) {
        const std::string suffix =
            "v6-overwrite-" + std::to_string(phaseIndex(interrupted_phase));
        const fs::path checkpoint_path = temp.file(suffix + ".chk");
        BuildImportController source;
        startImport(&source, makeIdentity(suffix), world,
                    makeConfig(checkpoint_path,
                               OverwritePolicy::ClearImportedBounds),
                    descriptors);
        acquirePhase(&source, interrupted_phase);
        source.recordActiveProgress(4);
        const CheckpointSnapshot active = source.checkpointSnapshot();
        assert(active.format_version == kBuildImportCheckpointCurrentVersion);
        assert(active.state == ImportState::Running);
        assert(active.has_active_unit);
        assert(active.active_phase == interrupted_phase);

        BuildImportController restored;
        std::string error;
        assert(restored.restoreFromCheckpoint(active, descriptors, &error));
        assertDynamicRollbackSnapshot(restored.checkpointSnapshot());
        const std::optional<CheckpointSnapshot> normalized =
            BuildImportCheckpoint::load(checkpoint_path.string(), &error);
        assert(normalized);
        assertDynamicRollbackSnapshot(*normalized);

        CheckpointSnapshot preserve_active = active;
        preserve_active.identity = makeIdentity(
            "v6-preserve-" + std::to_string(phaseIndex(interrupted_phase)));
        preserve_active.config = makeConfig(
            temp.file("v6-preserve-" +
                      std::to_string(phaseIndex(interrupted_phase)) + ".chk"),
            OverwritePolicy::PreserveExisting);
        BuildImportController rejected;
        error.clear();
        assert(!rejected.restoreFromCheckpoint(preserve_active, descriptors, &error));
        assert(!error.empty());
    }
}

void testDynamicUnitBoundaryRecovery(const ScopedTempDirectory& temp) {
    const WorldContext world{"server-dynamic-boundary", 0};
    const std::vector<ChunkDescriptor> descriptors = {
        makeCompleteChunk(-1, 0),
        makeCompleteChunk(0, 0),
    };
    const std::array<ImportPhase, 4> dynamic_phases = {
        ImportPhase::Gravity,
        ImportPhase::Attachment,
        ImportPhase::DependentAttachment,
        ImportPhase::Fluid,
    };

    for (const ImportPhase completed_phase : dynamic_phases) {
        const std::string phase_suffix = std::to_string(phaseIndex(completed_phase));
        const std::string overwrite_suffix = "dynamic-boundary-overwrite-" + phase_suffix;
        const fs::path overwrite_path = temp.file(overwrite_suffix + ".chk");
        BuildImportController overwrite;
        startImport(&overwrite, makeIdentity(overwrite_suffix), world,
                    makeConfig(overwrite_path,
                               OverwritePolicy::ClearImportedBounds),
                    descriptors);
        const WorkUnit completed = acquirePhase(&overwrite, completed_phase);
        assert(completed.coord.x == -1);
        overwrite.recordActiveProgress(2);
        assert(overwrite.completeActiveUnit(completed.block_count));
        const CheckpointSnapshot overwrite_boundary = overwrite.checkpointSnapshot();
        assert(overwrite_boundary.state == ImportState::Running);
        assert(overwrite_boundary.phase_index == phaseIndex(completed_phase));
        assert(overwrite_boundary.chunk_index == 1);
        assert(!overwrite_boundary.has_active_unit);
        assert(overwrite_boundary.completed_command_count != 0);

        assert(overwrite.pause());
        assertDynamicRollbackSnapshot(overwrite.checkpointSnapshot());
        const std::optional<CheckpointSnapshot> durable_overwrite =
            BuildImportCheckpoint::load(overwrite_path.string());
        assert(durable_overwrite);
        assertDynamicRollbackSnapshot(*durable_overwrite);

        const std::string preserve_suffix = "dynamic-boundary-preserve-" + phase_suffix;
        const fs::path preserve_path = temp.file(preserve_suffix + ".chk");
        BuildImportController preserve;
        startImport(&preserve, makeIdentity(preserve_suffix), world,
                    makeConfig(preserve_path, OverwritePolicy::PreserveExisting),
                    descriptors);
        const WorkUnit preserve_completed = acquirePhase(&preserve, completed_phase);
        assert(preserve_completed.coord.x == -1);
        preserve.recordActiveProgress(2);
        assert(preserve.completeActiveUnit(preserve_completed.block_count));
        assert(preserve.pause());
        const CheckpointSnapshot preserve_boundary = preserve.checkpointSnapshot();
        assert(preserve_boundary.state == ImportState::Paused);
        assert(preserve_boundary.phase_index == phaseIndex(completed_phase));
        assert(preserve_boundary.chunk_index == 1);
        assert(!preserve_boundary.has_active_unit);
        assert(preserve_boundary.completed_command_count != 0);

        BuildImportController rejected_restore;
        std::string error;
        assert(!rejected_restore.restoreFromCheckpoint(preserve_boundary,
                                                       descriptors, &error));
        assert(!error.empty());
        error.clear();
        assert(!preserve.resume(world, &error));
        assert(!error.empty());
        assert(preserve.state() == ImportState::Paused);
    }
}

void testCompletedGlobalClearCheckpointRestoration(const ScopedTempDirectory& temp) {
    const WorldContext world{"server-clear-boundary", 0};
    const std::vector<ChunkDescriptor> descriptors = {
        makeCompleteChunk(-1, 0),
        makeCompleteChunk(0, 0),
    };
    const fs::path checkpoint_path = temp.file("clear-boundary.chk");
    BuildImportController source;
    startImport(&source, makeIdentity("clear-boundary"), world,
                makeConfig(checkpoint_path, OverwritePolicy::ClearImportedBounds),
                descriptors);
    acquirePhase(&source, ImportPhase::Gravity);
    source.recordActiveProgress(1);
    assert(source.pause());
    assert(source.resume(world));

    acquireExpected(&source, ImportPhase::Clear, -1, 0);
    source.recordActiveProgress(1);
    assert(source.completeActiveUnit(0));
    const std::optional<CheckpointSnapshot> boundary = source.loadCheckpoint();
    assert(boundary);
    assert(boundary->state == ImportState::Running);
    assert(boundary->phase_index == phaseIndex(ImportPhase::Clear));
    assert(boundary->chunk_index == 1);
    assert(!boundary->has_active_unit);
    assert(boundary->active_spool_offset == 0);
    assert(boundary->completed_command_count != 0);
    assert(boundary->completed_block_count == 0);

    BuildImportController restored;
    std::string error;
    assert(restored.restoreFromCheckpoint(*boundary, descriptors, &error));
    const CheckpointSnapshot normalized = restored.checkpointSnapshot();
    assert(normalized.state == ImportState::Paused);
    assert(normalized.phase_index == phaseIndex(ImportPhase::Clear));
    assert(normalized.chunk_index == 1);
    assert(!normalized.has_active_unit);
    assert(normalized.completed_command_count == boundary->completed_command_count);
    assert(normalized.completed_block_count == 0);

    assert(restored.resume(world, &error));
    acquireExpected(&restored, ImportPhase::Clear, 0, 0);
    assert(restored.completeActiveUnit(0));
}

void testInterruptedGlobalClearRestoration(const ScopedTempDirectory& temp) {
    const WorldContext world{"server-global-clear", 0};
    const std::vector<ChunkDescriptor> descriptors = {
        makeCompleteChunk(-1, 0),
        makeCompleteChunk(0, 0),
    };
    const fs::path checkpoint_path = temp.file("global-clear.chk");
    BuildImportController source;
    startImport(&source, makeIdentity("global-clear"), world,
                makeConfig(checkpoint_path, OverwritePolicy::ClearImportedBounds),
                descriptors);
    acquirePhase(&source, ImportPhase::Gravity);
    source.recordActiveProgress(1);
    assert(source.pause());
    assert(source.resume(world));

    acquireExpected(&source, ImportPhase::Clear, -1, 0);
    assert(source.completeActiveUnit(0));
    acquireExpected(&source, ImportPhase::Clear, 0, 0);
    source.recordActiveProgress(3);
    assert(source.pause());
    const std::optional<CheckpointSnapshot> interrupted =
        BuildImportCheckpoint::load(checkpoint_path.string());
    assert(interrupted);
    assert(interrupted->state == ImportState::Paused);
    assert(interrupted->phase_index == phaseIndex(ImportPhase::Clear));
    assert(interrupted->chunk_index == 1);
    assert(interrupted->has_active_unit);
    assert(interrupted->active_phase == ImportPhase::Clear);
    assert(interrupted->active_spool_offset == 3);

    BuildImportController restored;
    std::string error;
    assert(restored.restoreFromCheckpoint(*interrupted, descriptors, &error));
    const CheckpointSnapshot normalized = restored.checkpointSnapshot();
    assert(normalized.state == ImportState::Paused);
    assert(normalized.phase_index == phaseIndex(ImportPhase::Clear));
    assert(normalized.chunk_index == 1);
    assert(!normalized.has_active_unit);
    assert(normalized.active_spool_offset == 0);
    assert(normalized.completed_command_count == 0);
    assert(normalized.completed_block_count == 0);

    assert(restored.resume(world, &error));
    const WorkUnit replayed =
        acquireExpected(&restored, ImportPhase::Clear, 0, 0);
    assert(restored.completeActiveUnit(0));
    for (const ImportPhase phase : kPlacementPhaseOrder) {
        for (const int32_t x : {-1, 0}) {
            const WorkUnit unit = acquireExpected(&restored, phase, x, 0);
            assert(restored.completeActiveUnit(unit.block_count));
        }
    }
    assert(restored.state() == ImportState::Verifying);
}

void testUnsafeAcquireDoesNotReturnUnitWhenCheckpointWriteFails(
        const ScopedTempDirectory& temp) {
    const WorldContext world{"server-acquire-transaction", 0};
    const fs::path checkpoint_dir = temp.file("acquire-transaction");
    assert(fs::create_directory(checkpoint_dir));
    const fs::path checkpoint_path = checkpoint_dir / "state.chk";
    BuildImportController controller;
    startImport(&controller, makeIdentity("acquire-transaction"), world,
                makeConfig(checkpoint_path, OverwritePolicy::PreserveExisting),
                {makeChunk(0, 0, {ImportPhase::Structure, ImportPhase::Gravity})});
    const WorkUnit structure =
        acquireExpected(&controller, ImportPhase::Structure, 0, 0);
    assert(controller.completeActiveUnit(structure.block_count));
    const CheckpointSnapshot before = controller.checkpointSnapshot();
    assert(!before.has_active_unit);

    std::error_code remove_error;
    fs::remove_all(checkpoint_dir, remove_error);
    assert(!remove_error);
    std::string error;
    assert(!controller.acquireNextUnit(&error));
    assert(!error.empty());

    const CheckpointSnapshot after = controller.checkpointSnapshot();
    assert(after.state == before.state);
    assert(after.phase_index == before.phase_index);
    assert(after.chunk_index == before.chunk_index);
    assert(after.completed_command_count == before.completed_command_count);
    assert(after.completed_block_count == before.completed_block_count);
    assert(!after.has_active_unit);
    assert(after.active_spool_offset == 0);
}

void testIdempotentAcquireUsesDurableCursor(const ScopedTempDirectory& temp) {
    const WorldContext world{"server-idempotent-acquire", 0};
    const fs::path checkpoint_path = temp.file("idempotent-acquire.chk");
    const ChunkDescriptor descriptor = makeChunk(0, 0, {ImportPhase::Structure});
    BuildImportController controller;
    startImport(&controller, makeIdentity("idempotent-acquire"), world,
                makeConfig(checkpoint_path, OverwritePolicy::PreserveExisting),
                {descriptor});

    const std::optional<CheckpointSnapshot> durable = controller.loadCheckpoint();
    assert(durable && !durable->has_active_unit);
    const WorkUnit active = acquireExpected(
        &controller, ImportPhase::Structure, descriptor.coord.x, descriptor.coord.z);
    assert(controller.checkpointSnapshot().has_active_unit);

    // The last durable cursor intentionally remains at the same idempotent
    // unit. A hard process exit therefore replays the full partition.
    CheckpointSnapshot crash_snapshot = *durable;
    crash_snapshot.config.checkpoint_path = temp.file("idempotent-restored.chk").string();
    BuildImportController restored;
    std::string error;
    assert(restored.restoreFromCheckpoint(crash_snapshot, {descriptor}, &error));
    assert(restored.resume(world, &error));
    const WorkUnit replayed = acquireExpected(
        &restored, ImportPhase::Structure, descriptor.coord.x, descriptor.coord.z);
    assert(replayed.block_count == active.block_count);
}

void testCheckpointRejectsTrailingData(const ScopedTempDirectory& temp) {
    const fs::path path = temp.file("trailing.chk");
    CheckpointSnapshot snapshot;
    snapshot.identity = makeIdentity("trailing");
    snapshot.world = {"server-trailing", 0};
    snapshot.config = makeConfig(path, OverwritePolicy::PreserveExisting);
    snapshot.config.verify_after_import = false;
    snapshot.config.verification_precision = VerificationPrecision::Balanced;
    snapshot.config.place_deny_layer = true;
    snapshot.config.deny_layer_bounds = {-16, -2, -32, 31, -2, 47};
    snapshot.degradation_notice =
        "6 blocks have unrepresentable Java states; first: waterlogged rail";
    snapshot.state = ImportState::Paused;
    assert(BuildImportCheckpoint::saveAtomically(path.string(), snapshot));
    const std::optional<CheckpointSnapshot> current =
        BuildImportCheckpoint::load(path.string());
    assert(current);
    assert(current->config.simulation_chunk_range ==
           snapshot.config.simulation_chunk_range);
    assert(!current->config.verify_after_import);
    assert(current->config.verification_precision == VerificationPrecision::Balanced);
    assert(current->config.place_deny_layer);
    assert(current->config.deny_layer_bounds.min_x == -16);
    assert(current->config.deny_layer_bounds.min_y == -2);
    assert(current->config.deny_layer_bounds.min_z == -32);
    assert(current->config.deny_layer_bounds.max_x == 31);
    assert(current->config.deny_layer_bounds.max_y == -2);
    assert(current->config.deny_layer_bounds.max_z == 47);
    assert(current->degradation_notice == snapshot.degradation_notice);
    assert(!current->config.create_maps_after_import);

    const fs::path pixel_map_path = temp.file("pixel-map.chk");
    CheckpointSnapshot pixel_map = snapshot;
    pixel_map.config.checkpoint_path = pixel_map_path.string();
    pixel_map.config.source_type = ImportSourceType::PixelArtPng;
    pixel_map.config.create_maps_after_import = true;
    assert(BuildImportCheckpoint::saveAtomically(pixel_map_path.string(), pixel_map));
    const auto restored_pixel_map = BuildImportCheckpoint::load(pixel_map_path.string());
    assert(restored_pixel_map);
    assert(restored_pixel_map->config.create_maps_after_import);

    const fs::path v12_path = temp.file("legacy-v12.chk");
    pixel_map.config.checkpoint_path = v12_path.string();
    assert(BuildImportCheckpoint::saveAtomically(v12_path.string(), pixel_map));
    {
        std::fstream legacy(v12_path, std::ios::binary | std::ios::in | std::ios::out);
        const uint32_t version = 12;
        legacy.seekp(sizeof(uint32_t), std::ios::beg);
        legacy.write(reinterpret_cast<const char*>(&version), sizeof(version));
        assert(legacy);
    }
    fs::resize_file(v12_path, fs::file_size(v12_path) - sizeof(uint8_t));
    const auto restored_v12 = BuildImportCheckpoint::load(v12_path.string());
    assert(restored_v12);
    assert(restored_v12->format_version == 12);
    assert(!restored_v12->config.create_maps_after_import);

    const fs::path invalid_map_path = temp.file("invalid-map-source.chk");
    pixel_map.config.checkpoint_path = invalid_map_path.string();
    pixel_map.config.source_type = ImportSourceType::Schematic;
    assert(BuildImportCheckpoint::saveAtomically(invalid_map_path.string(), pixel_map));
    std::string invalid_map_error;
    assert(!BuildImportCheckpoint::load(invalid_map_path.string(), &invalid_map_error));

    const fs::path invalid_deny_bounds_path = temp.file("invalid-deny-bounds.chk");
    CheckpointSnapshot invalid_deny_bounds = snapshot;
    invalid_deny_bounds.config.checkpoint_path = invalid_deny_bounds_path.string();
    invalid_deny_bounds.config.deny_layer_bounds = {};
    assert(BuildImportCheckpoint::saveAtomically(invalid_deny_bounds_path.string(),
                                                 invalid_deny_bounds));
    std::string invalid_deny_bounds_error;
    assert(!BuildImportCheckpoint::load(invalid_deny_bounds_path.string(),
                                        &invalid_deny_bounds_error));
    assert(!invalid_deny_bounds_error.empty());

    const fs::path planning_deny_bounds_path = temp.file("planning-deny-bounds.chk");
    CheckpointSnapshot planning_deny_bounds = snapshot;
    planning_deny_bounds.config.checkpoint_path = planning_deny_bounds_path.string();
    planning_deny_bounds.config.deny_layer_bounds = {};
    planning_deny_bounds.state = ImportState::Planning;
    assert(BuildImportCheckpoint::saveAtomically(planning_deny_bounds_path.string(),
                                                 planning_deny_bounds));
    const std::optional<CheckpointSnapshot> planning =
        BuildImportCheckpoint::load(planning_deny_bounds_path.string());
    assert(planning);
    assert(planning->config.place_deny_layer);
    assert(!planning->config.deny_layer_bounds.isValid());

    {
        std::ofstream stream(path, std::ios::binary | std::ios::app);
        stream.put('\x01');
        assert(stream);
    }
    std::string error;
    assert(!BuildImportCheckpoint::load(path.string(), &error));
    assert(!error.empty());

    const fs::path v10_path = temp.file("legacy-v10.chk");
    snapshot.config.checkpoint_path = v10_path.string();
    assert(BuildImportCheckpoint::saveAtomically(v10_path.string(), snapshot));
    {
        std::fstream legacy(v10_path, std::ios::binary | std::ios::in | std::ios::out);
        const uint32_t version = 10;
        legacy.seekp(sizeof(uint32_t), std::ios::beg);
        legacy.write(reinterpret_cast<const char*>(&version), sizeof(version));
        assert(legacy);
    }
    // The current v13 writer appends the v11 deny-layer fields, the v12
    // source/access fields and the v13 automatic-map flag.
    fs::resize_file(v10_path, fs::file_size(v10_path) - sizeof(uint8_t) * 4 -
        sizeof(int32_t) * 6);
    const std::optional<CheckpointSnapshot> v10 =
        BuildImportCheckpoint::load(v10_path.string(), &error);
    assert(v10);
    assert(v10->format_version == 10);
    assert(!v10->config.place_deny_layer);
    assert(!v10->config.deny_layer_bounds.isValid());

    const fs::path v7_path = temp.file("legacy-v7.chk");
    snapshot.config.checkpoint_path = v7_path.string();
    assert(BuildImportCheckpoint::saveAtomically(v7_path.string(), snapshot));
    {
        std::fstream legacy(v7_path, std::ios::binary | std::ios::in | std::ios::out);
        const uint32_t version = 7;
        legacy.seekp(sizeof(uint32_t), std::ios::beg);
        legacy.write(reinterpret_cast<const char*>(&version), sizeof(version));
        assert(legacy);
    }
    {
        std::ifstream current_stream(v7_path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(current_stream)),
                                std::istreambuf_iterator<char>());
        size_t cursor = sizeof(uint32_t) * 2;
        for (size_t string_index = 0; string_index < 6; ++string_index) {
            assert(cursor + sizeof(uint32_t) <= bytes.size());
            uint32_t length = 0;
            std::memcpy(&length, bytes.data() + cursor, sizeof(length));
            cursor += sizeof(length);
            assert(length <= bytes.size() - cursor);
            cursor += length;
        }
        // world dimension, chunk size, and legacy radius precede the v10
        // simulation-range field.  Strip it before treating this file as v7.
        cursor += sizeof(int32_t) * 3;
        assert(cursor + sizeof(int32_t) <= bytes.size());
        bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                    bytes.begin() + static_cast<std::ptrdiff_t>(
                        cursor + sizeof(int32_t)));
        std::ofstream legacy(v7_path, std::ios::binary | std::ios::trunc);
        legacy.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        assert(legacy);
    }
    // v7 predates verification options, degradation text, deny-layer state,
    // and the v12 source/access and v13 map-request fields.
    fs::resize_file(v7_path, fs::file_size(v7_path) - sizeof(uint8_t) * 6 -
        sizeof(int32_t) * 6 - sizeof(uint32_t) - snapshot.degradation_notice.size());
    const std::optional<CheckpointSnapshot> v7 =
        BuildImportCheckpoint::load(v7_path.string(), &error);
    assert(v7);
    assert(v7->format_version == 7);
    assert(v7->config.simulation_chunk_range == 0);
    assert(v7->config.verify_after_import);
    assert(v7->config.verification_precision == VerificationPrecision::Thorough);
    assert(!v7->config.place_deny_layer);
    assert(!v7->config.deny_layer_bounds.isValid());
    assert(v7->degradation_notice.empty());

    const fs::path legacy_path = temp.file("legacy-v4.chk");
    snapshot.config.checkpoint_path = legacy_path.string();
    snapshot.verification_sample_index = 99;
    assert(BuildImportCheckpoint::saveAtomically(legacy_path.string(), snapshot));
    {
        std::ifstream current_stream(legacy_path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(current_stream)),
                                std::istreambuf_iterator<char>());
        assert(bytes.size() >= sizeof(uint32_t) * 2);
        const uint32_t version = 4;
        std::memcpy(bytes.data() + sizeof(uint32_t), &version, sizeof(version));

        size_t cursor = sizeof(uint32_t) * 2;
        for (size_t string_index = 0; string_index < 6; ++string_index) {
            assert(cursor + sizeof(uint32_t) <= bytes.size());
            uint32_t length = 0;
            std::memcpy(&length, bytes.data() + cursor, sizeof(length));
            cursor += sizeof(length);
            assert(length <= bytes.size() - cursor);
            cursor += length;
        }
        cursor += sizeof(int32_t);      // world dimension
        cursor += sizeof(int32_t) * 2;  // chunk size and legacy radius
        // Remove v10's simulation range before laying the file out as v4.
        assert(cursor + sizeof(int32_t) <= bytes.size());
        bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                    bytes.begin() + static_cast<std::ptrdiff_t>(
                        cursor + sizeof(int32_t)));
        cursor += sizeof(int32_t) * 5;  // remaining v4 config fields
        assert(cursor + sizeof(int32_t) * 2 <= bytes.size());
        bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                    bytes.begin() + static_cast<std::ptrdiff_t>(
                        cursor + sizeof(int32_t) * 2));
        assert(bytes.size() >= sizeof(uint64_t) + sizeof(uint8_t) * 6 +
                                   sizeof(int32_t) * 6 + sizeof(uint32_t) +
                                   snapshot.degradation_notice.size());
        bytes.resize(bytes.size() - sizeof(uint64_t) - sizeof(uint8_t) * 6 -
                     sizeof(int32_t) * 6 - sizeof(uint32_t) -
                     snapshot.degradation_notice.size());

        std::ofstream legacy(legacy_path, std::ios::binary | std::ios::trunc);
        legacy.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        assert(legacy);
    }
    const std::optional<CheckpointSnapshot> legacy =
        BuildImportCheckpoint::load(legacy_path.string(), &error);
    assert(legacy);
    assert(legacy->format_version == 4);
    assert(legacy->config.simulation_chunk_range == 0);
    assert(legacy->verification_sample_index == 0);
    assert(legacy->config.verify_after_import);
    assert(legacy->config.verification_precision == VerificationPrecision::Thorough);
    assert(!legacy->config.place_deny_layer);
    assert(!legacy->config.deny_layer_bounds.isValid());
    assert(legacy->degradation_notice.empty());
}

void testPreserveLastUnitImmediatelyStartsVerification(const ScopedTempDirectory& temp) {
    const fs::path checkpoint_path = temp.file("preserve.chk");
    const ImportConfig config =
        makeConfig(checkpoint_path, OverwritePolicy::PreserveExisting);
    const ImportIdentity identity = makeIdentity("preserve");
    const WorldContext world{"server-preserve", -1};
    const std::vector<ChunkDescriptor> descriptors = {
        makeChunk(0, 0),
        makeChunk(-1, -1),
    };

    BuildImportController controller;
    startImport(&controller, identity, world, config, descriptors);

    // Preserve mode is phase-major. Negative coordinates sort before origin.
    completeExpected(&controller, ImportPhase::Structure, -1, -1);
    completeExpected(&controller, ImportPhase::Structure, 0, 0);
    completeExpected(&controller, ImportPhase::Attachment, -1, -1);
    const WorkUnit last = acquireExpected(&controller, ImportPhase::Attachment, 0, 0);
    assert(controller.completeActiveUnit(last.block_count));

    // Completing the final real spool must not require one more scheduler poll.
    assert(controller.state() == ImportState::Verifying);
    assert(!controller.acquireNextUnit());
    assertTerminalPosition(controller.checkpointSnapshot(), descriptors.size());

    const std::optional<CheckpointSnapshot> saved = controller.loadCheckpoint();
    assert(saved);
    assert(saved->state == ImportState::Verifying);
    assert(saved->completed_block_count == 220);
    assertTerminalPosition(*saved, descriptors.size());
}

void testPlanVersionOrdering(const ScopedTempDirectory& temp) {
    const WorldContext world{"server-order", 0};
    ImportConfig config = makeConfig(temp.file("order-v6.chk"),
                                     OverwritePolicy::PreserveExisting);
    config.simulation_chunk_range = 4;

    // A legacy plan is globally ordered by (z, x). Grouping these descriptors
    // by a shared simulation region would incorrectly move (0,1) before (3,0).
    ImportIdentity legacy = makeIdentity("order-v6");
    legacy.options_hash = "preserve-command-plan-v6";
    BuildImportController legacy_controller;
    startImport(&legacy_controller, legacy, world, config,
                {makeChunk(0, 1), makeChunk(3, 0)});
    completeExpected(&legacy_controller, ImportPhase::Structure, 3, 0);
    completeExpected(&legacy_controller, ImportPhase::Structure, 0, 1);

    // Region plans use a serpentine row walk to avoid a long teleport from
    // the right edge of one row back to the left edge of the next.
    config.checkpoint_path = temp.file("order-v7.chk").string();
    ImportIdentity region = makeIdentity("order-v7");
    region.options_hash = "preserve-region-command-plan-v7";
    BuildImportController region_controller;
    startImport(&region_controller, region, world, config,
                {makeChunk(0, 4), makeChunk(4, 0),
                 makeChunk(0, 0), makeChunk(4, 4)});
    for (const ChunkCoord expected : std::array<ChunkCoord, 4>{
             ChunkCoord{0, 0}, ChunkCoord{4, 0},
             ChunkCoord{4, 4}, ChunkCoord{0, 4}}) {
        completeExpected(&region_controller, ImportPhase::Structure,
                         expected.x, expected.z);
    }
}

void testAdaptiveRegionGridExecutionAndRestore(const ScopedTempDirectory& temp) {
    const WorldContext world{"server-adaptive-region", 0};
    ImportConfig config = makeConfig(temp.file("adaptive-region.chk"),
                                     OverwritePolicy::PreserveExisting);
    config.simulation_chunk_range = 4;

    ImportIdentity identity = makeIdentity("adaptive-region");
    identity.options_hash = "preserve-region-grid-v8";
    ChunkDescriptor descriptor = makeChunk(10, -7, {ImportPhase::Structure});
    descriptor.region_grid_origin = {10, -7};
    descriptor.imported_bounds = {160, 0, -112, 223, 20, -49};

    assert(regionForChunk({10, -7}, 4, descriptor.region_grid_origin) ==
           ChunkCoord({0, 0}));
    assert(regionForChunk({9, -8}, 4, descriptor.region_grid_origin) ==
           ChunkCoord({-1, -1}));

    BuildImportController controller;
    startImport(&controller, identity, world, config, {descriptor});
    const CheckpointSnapshot planned = controller.checkpointSnapshot();
    assert(planned.config.region_grid_origin == ChunkCoord({10, -7}));

    const WorkUnit active =
        acquireExpected(&controller, ImportPhase::Structure, 10, -7);
    assert(active.region_grid_origin == ChunkCoord({10, -7}));
    assert(active.region_coord == ChunkCoord({0, 0}));
    assert(active.region_span == 4);
    assert(active.load_bounds.min_x == 144);
    assert(active.load_bounds.max_x == 239);
    assert(active.load_bounds.min_z == -128);
    assert(active.load_bounds.max_z == -33);

    controller.recordActiveProgress(2);
    assert(controller.pause());
    const std::optional<CheckpointSnapshot> saved = controller.loadCheckpoint();
    assert(saved);
    assert(saved->config.region_grid_origin == ChunkCoord({10, -7}));

    // Restoration must use the checkpoint's grid origin rather than trusting
    // transient descriptor metadata rebuilt from the verification catalog.
    descriptor.region_grid_origin = {999, 999};
    BuildImportController restored;
    assert(restored.restoreFromCheckpoint(*saved, {descriptor}));
    assert(restored.resume(world));
    const WorkUnit replay =
        acquireExpected(&restored, ImportPhase::Structure, 10, -7);
    assert(replay.region_grid_origin == ChunkCoord({10, -7}));
    assert(replay.region_coord == ChunkCoord({0, 0}));
    assert(replay.load_bounds.min_x == 144);
    assert(replay.load_bounds.max_x == 239);
    assert(replay.load_bounds.min_z == -128);
    assert(replay.load_bounds.max_z == -33);
}

void testOverwriteRestoreAcrossNegativeRegions(const ScopedTempDirectory& temp) {
    const fs::path checkpoint_path = temp.file("overwrite.chk");
    ImportConfig config = makeConfig(checkpoint_path, OverwritePolicy::ClearImportedBounds);
    config.simulation_chunk_range = 4;  // Four native chunks per work-region axis.
    const ImportIdentity identity = makeIdentity("overwrite");
    const WorldContext world{"server-overwrite", 0};
    const std::vector<ChunkDescriptor> descriptors = {
        makeCompleteChunk(0, 0), makeCompleteChunk(-1, 0), makeCompleteChunk(-4, 0),
        makeCompleteChunk(-2, 0), makeCompleteChunk(-3, 0),
    };
    const std::array<int32_t, 5> sorted_x = {-4, -3, -2, -1, 0};

    BuildImportController controller;
    startImport(&controller, identity, world, config, descriptors);

    // Each partition is cleared immediately before its first placement unit;
    // there is no long-running global clear sweep.
    for (const int32_t x : {-4, -3}) {
        const WorkUnit unit = acquireExpected(&controller, ImportPhase::Structure, x, 0);
        assert(unit.clear_before_build);
        assert(controller.completeActiveUnit(unit.block_count));
    }
    const WorkUnit interrupted =
        acquireExpected(&controller, ImportPhase::Structure, -2, 0);
    assert(interrupted.clear_before_build);
    assert(interrupted.region_coord.x == -1);
    assert(interrupted.load_bounds.min_x == -80);
    assert(interrupted.load_bounds.max_x == 15);
    controller.recordActiveProgress(7);

    const CheckpointSnapshot in_memory = controller.checkpointSnapshot();
    assert(in_memory.phase_index == phaseIndex(ImportPhase::Structure));
    assert(in_memory.chunk_index == 2);
    assert(in_memory.has_active_unit);
    assert(in_memory.active_spool_offset == 7);

    // Structure acquisition is intentionally not written a second time. The
    // durable cursor already points at the same idempotent unit, so restoration
    // replays it atomically instead of resuming inside a /fill.
    const std::optional<CheckpointSnapshot> saved = controller.loadCheckpoint();
    assert(saved);
    assert(saved->state == ImportState::Running);
    assert(saved->phase_index == phaseIndex(ImportPhase::Structure));
    assert(saved->chunk_index == 2);
    assert(!saved->has_active_unit);
    assert(saved->active_spool_offset == 0);

    BuildImportController restored;
    assert(restored.restoreFromCheckpoint(*saved, descriptors));
    assert(restored.state() == ImportState::Paused);
    assert(restored.resume(world));

    // Resume replays the interrupted structure unit. Every remaining phase
    // then sweeps all regions before the controller advances to the next one.
    for (const int32_t x : {-2, -1, 0}) {
        const WorkUnit unit = acquireExpected(&restored, ImportPhase::Structure, x, 0);
        assert(unit.clear_before_build);
        assert(restored.completeActiveUnit(unit.block_count));
    }
    for (const ImportPhase phase : {ImportPhase::Gravity, ImportPhase::Attachment,
                                    ImportPhase::DependentAttachment,
                                    ImportPhase::Fluid}) {
        for (const int32_t x : sorted_x) {
            const WorkUnit unit = acquireExpected(&restored, phase, x, 0);
            assert(!unit.clear_before_build);
            assert(restored.completeActiveUnit(unit.block_count));
        }
    }
    assert(!restored.acquireNextUnit());
    assert(restored.state() == ImportState::Verifying);
    assertTerminalPosition(restored.checkpointSnapshot(), descriptors.size());
    assert(restored.checkpointSnapshot().completed_block_count == 700);
}

void testTerminalCheckpointRestoration(const ScopedTempDirectory& temp) {
    const std::vector<ChunkDescriptor> descriptors = {makeChunk(-1, 0), makeChunk(0, 0)};
    const WorldContext world{"server-terminal", 2};
    const std::array<ImportState, 3> terminal_states = {
        ImportState::Failed,
        ImportState::Verifying,
        ImportState::Paused,
    };

    for (size_t index = 0; index < terminal_states.size(); ++index) {
        const fs::path path = temp.file("terminal-" + std::to_string(index) + ".chk");
        CheckpointSnapshot terminal;
        terminal.identity = makeIdentity("terminal-" + std::to_string(index));
        terminal.world = world;
        terminal.config = makeConfig(path, OverwritePolicy::PreserveExisting);
        terminal.state = terminal_states[index];
        terminal.phase_index = 0;
        terminal.chunk_index = static_cast<uint32_t>(descriptors.size());
        terminal.completed_command_count = 123;
        terminal.completed_block_count = 220;
        terminal.verification_sample_index = 17;
        assert(BuildImportCheckpoint::saveAtomically(path.string(), terminal));

        const std::optional<CheckpointSnapshot> loaded =
            BuildImportCheckpoint::load(path.string());
        assert(loaded);
        assert(loaded->state == terminal_states[index]);
        assertTerminalPosition(*loaded, descriptors.size());

        BuildImportController restored;
        assert(restored.restoreFromCheckpoint(*loaded, descriptors));
        assert(restored.state() == ImportState::Paused);
        assertTerminalPosition(restored.checkpointSnapshot(), descriptors.size());
        assert(restored.resume(world));
        assert(restored.state() == ImportState::Verifying);
        assertTerminalPosition(restored.checkpointSnapshot(), descriptors.size());
        assert(restored.checkpointSnapshot().completed_command_count == 123);
        assert(restored.checkpointSnapshot().completed_block_count == 220);
        assert(restored.checkpointSnapshot().verification_sample_index == 17);
        assert(!restored.acquireNextUnit());

        const std::optional<CheckpointSnapshot> resumed =
            BuildImportCheckpoint::load(path.string());
        assert(resumed);
        assert(resumed->state == ImportState::Verifying);
        assert(resumed->completed_command_count == 123);
        assert(resumed->completed_block_count == 220);
        assert(resumed->verification_sample_index == 17);
        assertTerminalPosition(*resumed, descriptors.size());
    }
}

void testRestoreRejectsInconsistentMetadata(const ScopedTempDirectory& temp) {
    const WorldContext world{"server-strict", 0};
    const std::vector<ChunkDescriptor> descriptors = {makeChunk(0, 0)};
    CheckpointSnapshot snapshot;
    snapshot.identity = makeIdentity("strict");
    snapshot.world = world;
    snapshot.config = makeConfig(temp.file("strict.chk"),
                                 OverwritePolicy::ClearImportedBounds);
    snapshot.state = ImportState::Paused;
    snapshot.phase_index = phaseIndex(ImportPhase::Structure);
    snapshot.chunk_index = 0;

    snapshot.has_active_unit = true;
    snapshot.active_phase = ImportPhase::Structure;
    snapshot.active_coord = {1, 0};
    BuildImportController wrong_active;
    std::string error;
    assert(!wrong_active.restoreFromCheckpoint(snapshot, descriptors, &error));
    assert(!error.empty());

    snapshot.active_coord = {0, 0};
    snapshot.active_spool_offset = 2;
    snapshot.completed_command_count = 1;
    BuildImportController wrong_offset;
    error.clear();
    assert(!wrong_offset.restoreFromCheckpoint(snapshot, descriptors, &error));
    assert(!error.empty());

    snapshot.has_active_unit = false;
    snapshot.active_spool_offset = 0;
    snapshot.completed_command_count = 0;
    snapshot.state = ImportState::Verifying;
    BuildImportController wrong_state;
    error.clear();
    assert(!wrong_state.restoreFromCheckpoint(snapshot, descriptors, &error));
    assert(!error.empty());

    // Current rollback checkpoints may point at the global Clear phase, but
    // an active unit must still identify the descriptor under the cursor.
    snapshot.state = ImportState::Paused;
    snapshot.phase_index = phaseIndex(ImportPhase::Clear);
    snapshot.chunk_index = 0;
    snapshot.has_active_unit = true;
    snapshot.active_phase = ImportPhase::Clear;
    snapshot.active_coord = {1, 0};
    BuildImportController invalid_v6_clear;
    error.clear();
    assert(!invalid_v6_clear.restoreFromCheckpoint(snapshot, descriptors, &error));
    assert(!error.empty());

    // A v4 cursor is never interpreted with the current ordering. It is
    // conservatively migrated to the first real placement partition.
    snapshot.format_version = 4;
    snapshot.state = ImportState::Paused;
    snapshot.phase_index = phaseIndex(ImportPhase::Clear);
    snapshot.chunk_index = 0;
    snapshot.has_active_unit = true;
    snapshot.active_phase = ImportPhase::Clear;
    snapshot.active_coord = {0, 0};
    BuildImportController legacy_clear;
    error.clear();
    assert(legacy_clear.restoreFromCheckpoint(snapshot, descriptors, &error));
    assert(legacy_clear.resume(world));
    const WorkUnit migrated =
        acquireExpected(&legacy_clear, ImportPhase::Structure, 0, 0);
    assert(migrated.clear_before_build);

    // Persist a normalized current cursor even when the first available phase
    // is not Structure, so another process can restore before resume occurs.
    const std::vector<ChunkDescriptor> gravity_only = {
        makeChunk(2, -1, {ImportPhase::Gravity}),
    };
    snapshot.identity = makeIdentity("legacy-gravity-only");
    snapshot.config = makeConfig(temp.file("legacy-gravity-only.chk"),
                                 OverwritePolicy::ClearImportedBounds);
    snapshot.chunk_index = 99;
    snapshot.completed_command_count = 50;
    snapshot.completed_block_count = 999;
    snapshot.active_spool_offset = 7;
    snapshot.active_coord = {123, 456};
    BuildImportController legacy_gravity;
    error.clear();
    assert(legacy_gravity.restoreFromCheckpoint(snapshot, gravity_only, &error));
    const std::optional<CheckpointSnapshot> normalized = legacy_gravity.loadCheckpoint(&error);
    assert(normalized);
    assert(normalized->format_version == kBuildImportCheckpointCurrentVersion);
    assert(normalized->phase_index == phaseIndex(ImportPhase::Gravity));
    assert(normalized->chunk_index == 0);
    assert(normalized->completed_command_count == 0);
    assert(normalized->completed_block_count == 0);
    assert(!normalized->has_active_unit);

    BuildImportController restored_normalized;
    assert(restored_normalized.restoreFromCheckpoint(*normalized, gravity_only, &error));
    assert(restored_normalized.resume(world, &error));
    const WorkUnit gravity =
        acquireExpected(&restored_normalized, ImportPhase::Gravity, 2, -1);
    assert(gravity.clear_before_build);
}

void testLegacyV5CursorMigration(const ScopedTempDirectory& temp) {
    const WorldContext world{"server-v5", 0};
    const std::vector<ChunkDescriptor> descriptors = {
        makeChunk(-1, 0), makeChunk(0, 0),
    };

    CheckpointSnapshot interrupted;
    interrupted.format_version = 5;
    interrupted.identity = makeIdentity("v5-interrupted");
    interrupted.world = world;
    interrupted.config = makeConfig(temp.file("v5-interrupted.chk"),
                                    OverwritePolicy::PreserveExisting);
    interrupted.state = ImportState::Paused;
    interrupted.phase_index = phaseIndex(ImportPhase::Attachment);
    interrupted.chunk_index = 1;
    interrupted.has_active_unit = true;
    interrupted.active_coord = {0, 0};
    interrupted.active_phase = ImportPhase::Attachment;
    interrupted.active_spool_offset = 7;
    interrupted.completed_command_count = 21;
    interrupted.completed_block_count = 210;
    interrupted.verification_sample_index = 9;

    BuildImportController replayed;
    std::string error;
    assert(replayed.restoreFromCheckpoint(interrupted, descriptors, &error));
    const CheckpointSnapshot normalized = replayed.checkpointSnapshot();
    assert(normalized.format_version == kBuildImportCheckpointCurrentVersion);
    assert(normalized.phase_index == phaseIndex(ImportPhase::Structure));
    assert(normalized.chunk_index == 0);
    assert(normalized.completed_command_count == 0);
    assert(normalized.completed_block_count == 0);
    assert(normalized.verification_sample_index == 0);
    assert(!normalized.has_active_unit);
    assert(replayed.resume(world, &error));
    acquireExpected(&replayed, ImportPhase::Structure, -1, 0);

    CheckpointSnapshot terminal = interrupted;
    terminal.identity = makeIdentity("v5-terminal");
    terminal.config.checkpoint_path = temp.file("v5-terminal.chk").string();
    terminal.phase_index = 5;  // ImportPhase::Count in the v5 scheduler.
    terminal.chunk_index = 0;
    terminal.has_active_unit = false;
    terminal.active_spool_offset = 0;
    terminal.completed_command_count = 42;
    terminal.completed_block_count = 220;
    terminal.verification_sample_index = 9;

    BuildImportController migrated_terminal;
    assert(migrated_terminal.restoreFromCheckpoint(terminal, descriptors, &error));
    assertTerminalPosition(migrated_terminal.checkpointSnapshot(), descriptors.size());
    assert(migrated_terminal.checkpointSnapshot().verification_sample_index == 0);
    assert(migrated_terminal.resume(world, &error));
    assert(migrated_terminal.state() == ImportState::Verifying);
    assert(!migrated_terminal.acquireNextUnit());
}

void testContextChangeRequiresOriginalWorld() {
    ImportConfig config;
    config.overwrite_policy = OverwritePolicy::ClearImportedBounds;
    const WorldContext original_world{"server-context", 0};
    const std::vector<ChunkDescriptor> descriptors = {makeChunk(-1, 0), makeChunk(0, 0)};

    BuildImportController controller;
    startImport(&controller, makeIdentity("context"), original_world, config, descriptors);
    const WorkUnit interrupted = acquireExpected(&controller, ImportPhase::Structure, -1, 0);
    assert(interrupted.clear_before_build);
    controller.onWorldContextChanged(WorldContext{"server-context", 1});
    assert(controller.state() == ImportState::ClosedForContextChange);
    assert(!controller.resume(WorldContext{"other-server", 0}));
    assert(!controller.resume(WorldContext{"server-context", 1}));
    assert(controller.resume(original_world));

    const WorkUnit replayed = acquireExpected(&controller, ImportPhase::Structure, -1, 0);
    assert(replayed.coord.x == interrupted.coord.x);
    assert(replayed.coord.z == interrupted.coord.z);
    assert(replayed.phase == interrupted.phase);

    BuildImportController planning;
    assert(planning.startPlanning(makeIdentity("planning-context"), original_world, config));
    planning.onWorldContextChanged(WorldContext{"server-context", 1});
    assert(planning.state() == ImportState::ClosedForContextChange);
}

void testCrossPartitionDependencyAndEmptyOverwrite(const ScopedTempDirectory& temp) {
    const WorldContext world{"server-dependency", 0};
    const ImportConfig config = makeConfig(temp.file("dependency.chk"),
                                           OverwritePolicy::ClearImportedBounds);

    // The dependent half lives in the lexicographically earlier chunk. Global
    // phase ordering must still place the support half first.
    const ChunkDescriptor dependent =
        makeChunk(0, 0, {ImportPhase::DependentAttachment});
    const ChunkDescriptor support =
        makeChunk(1, 0, {ImportPhase::Attachment, ImportPhase::Fluid});
    BuildImportController dependency_controller;
    startImport(&dependency_controller, makeIdentity("dependency"), world, config,
                {dependent, support});
    WorkUnit support_unit =
        acquireExpected(&dependency_controller, ImportPhase::Attachment, 1, 0);
    assert(support_unit.clear_before_build);
    assert(dependency_controller.completeActiveUnit(support_unit.block_count));
    WorkUnit dependent_unit = acquireExpected(&dependency_controller,
        ImportPhase::DependentAttachment, 0, 0);
    assert(dependent_unit.clear_before_build);
    assert(dependency_controller.completeActiveUnit(dependent_unit.block_count));
    WorkUnit fluid_unit =
        acquireExpected(&dependency_controller, ImportPhase::Fluid, 1, 0);
    assert(!fluid_unit.clear_before_build);
    assert(dependency_controller.completeActiveUnit(fluid_unit.block_count));
    assert(dependency_controller.state() == ImportState::Verifying);

    ChunkDescriptor empty;
    empty.coord = {-1, 0};
    empty.imported_bounds = {-32, 10, 0, -1, 20, 31};
    const ChunkDescriptor built = makeChunk(0, 0, {ImportPhase::Structure});
    BuildImportController clear_controller;
    startImport(&clear_controller, makeIdentity("empty-overwrite"), world,
                makeConfig(temp.file("empty-overwrite.chk"),
                           OverwritePolicy::ClearImportedBounds),
                {empty, built});
    const WorkUnit clear =
        acquireExpected(&clear_controller, ImportPhase::Clear, -1, 0);
    assert(!clear.clear_before_build);
    assert(clear.block_count == 0);

    // Virtual Clear is idempotent, so its durable checkpoint remains at the
    // Structure cursor and a hard exit replays the same empty partition.
    const std::optional<CheckpointSnapshot> active_snapshot =
        clear_controller.loadCheckpoint();
    assert(active_snapshot && !active_snapshot->has_active_unit);
    assert(active_snapshot->phase_index == phaseIndex(ImportPhase::Structure));
    BuildImportController restored;
    assert(restored.restoreFromCheckpoint(*active_snapshot, {empty, built}));
    assert(restored.resume(world));
    const WorkUnit replayed = acquireExpected(&restored, ImportPhase::Clear, -1, 0);
    assert(restored.completeActiveUnit(replayed.block_count));
    const WorkUnit placement =
        acquireExpected(&restored, ImportPhase::Structure, 0, 0);
    assert(placement.clear_before_build);
}

void testDeferredCommitAndUnsafeBoundary(const ScopedTempDirectory& temp) {
    const fs::path checkpoint_path = temp.file("deferred-commit.chk");
    const WorldContext world{"server-deferred-commit", 0};
    const ChunkDescriptor first = makeChunk(0, 0, {ImportPhase::Structure});
    const ChunkDescriptor second = makeChunk(
        1, 0, {ImportPhase::Structure, ImportPhase::Gravity});
    BuildImportController controller;
    startImport(&controller, makeIdentity("deferred-commit"), world,
                makeConfig(checkpoint_path, OverwritePolicy::PreserveExisting),
                {first, second});

    const WorkUnit first_unit =
        acquireExpected(&controller, ImportPhase::Structure, 0, 0);
    assert(first_unit.sequence == 1);
    controller.recordActiveProgress(3);
    assert(controller.completeActiveUnitDeferred(first_unit.block_count));
    assert(controller.hasDeferredUnits());

    const std::optional<CheckpointSnapshot> still_durable =
        BuildImportCheckpoint::load(checkpoint_path.string());
    assert(still_durable);
    assert(still_durable->phase_index == phaseIndex(ImportPhase::Structure));
    assert(still_durable->chunk_index == 0);
    assert(still_durable->completed_command_count == 0);
    assert(still_durable->completed_block_count == 0);
    assert(!still_durable->has_active_unit);

    const std::optional<WorkUnit> first_peek = controller.peekNextUnit();
    const std::optional<WorkUnit> second_peek = controller.peekNextUnit();
    assert(first_peek && second_peek);
    assert(first_peek->coord == second.coord);
    assert(first_peek->phase == ImportPhase::Structure);
    assert(first_peek->sequence == 2);
    assert(second_peek->sequence == first_peek->sequence);
    const WorkUnit second_unit =
        acquireExpected(&controller, ImportPhase::Structure, 1, 0);
    assert(second_unit.sequence == first_peek->sequence);
    controller.recordActiveProgress(4);
    assert(controller.completeActiveUnitDeferred(second_unit.block_count));

    const std::optional<WorkUnit> dynamic_peek = controller.peekNextUnit();
    assert(dynamic_peek);
    assert(dynamic_peek->phase == ImportPhase::Gravity);
    assert(dynamic_peek->coord == second.coord);
    std::string acquire_error;
    assert(!controller.acquireNextUnit(&acquire_error));
    assert(!acquire_error.empty());
    assert(controller.hasDeferredUnits());

    assert(controller.commitDeferredUnits());
    assert(!controller.hasDeferredUnits());
    const std::optional<CheckpointSnapshot> committed =
        BuildImportCheckpoint::load(checkpoint_path.string());
    assert(committed);
    assert(committed->state == ImportState::Running);
    assert(committed->phase_index == phaseIndex(ImportPhase::Gravity));
    assert(committed->chunk_index == 1);
    assert(committed->completed_command_count == 7);
    assert(committed->completed_block_count == 200);
    assert(!committed->has_active_unit);

    const WorkUnit gravity = acquireExpected(&controller, ImportPhase::Gravity, 1, 0);
    assert(gravity.sequence == dynamic_peek->sequence);
    std::string deferred_error;
    assert(!controller.completeActiveUnitDeferred(gravity.block_count, &deferred_error));
    assert(!deferred_error.empty());
    assert(controller.completeActiveUnit(gravity.block_count));
}

void testDeferredCommitFailureRestoresBaseline(const ScopedTempDirectory& temp) {
    const fs::path checkpoint_dir = temp.file("deferred-commit-failure");
    assert(fs::create_directory(checkpoint_dir));
    const fs::path checkpoint_path = checkpoint_dir / "state.chk";
    const WorldContext world{"server-deferred-failure", 0};
    BuildImportController controller;
    startImport(&controller, makeIdentity("deferred-failure"), world,
                makeConfig(checkpoint_path, OverwritePolicy::PreserveExisting),
                {makeChunk(0, 0, {ImportPhase::Structure})});

    const WorkUnit active = acquireExpected(&controller, ImportPhase::Structure, 0, 0);
    controller.recordActiveProgress(9);
    assert(controller.completeActiveUnitDeferred(active.block_count));
    assert(controller.state() == ImportState::Verifying);
    assert(controller.hasDeferredUnits());

    std::error_code remove_error;
    fs::remove_all(checkpoint_dir, remove_error);
    assert(!remove_error);
    std::string commit_error;
    assert(!controller.commitDeferredUnits(&commit_error));
    assert(!commit_error.empty());
    assert(!controller.hasDeferredUnits());
    const CheckpointSnapshot rolled_back = controller.checkpointSnapshot();
    assert(rolled_back.state == ImportState::Running);
    assert(rolled_back.phase_index == phaseIndex(ImportPhase::Structure));
    assert(rolled_back.chunk_index == 0);
    assert(rolled_back.completed_command_count == 0);
    assert(rolled_back.completed_block_count == 0);
    assert(!rolled_back.has_active_unit);
    assert(rolled_back.active_spool_offset == 0);

    const WorkUnit replay = acquireExpected(&controller, ImportPhase::Structure, 0, 0);
    assert(replay.sequence == active.sequence);
}

void testDeferredPauseRollsBackBeforeCheckpoint(const ScopedTempDirectory& temp) {
    const fs::path checkpoint_path = temp.file("deferred-pause.chk");
    const WorldContext world{"server-deferred-pause", 0};
    BuildImportController controller;
    startImport(&controller, makeIdentity("deferred-pause"), world,
                makeConfig(checkpoint_path, OverwritePolicy::PreserveExisting),
                {makeChunk(0, 0, {ImportPhase::Structure}),
                 makeChunk(1, 0, {ImportPhase::Structure})});

    const WorkUnit first = acquireExpected(&controller, ImportPhase::Structure, 0, 0);
    controller.recordActiveProgress(5);
    assert(controller.completeActiveUnitDeferred(first.block_count));
    acquireExpected(&controller, ImportPhase::Structure, 1, 0);
    controller.recordActiveProgress(2);
    assert(controller.pause());
    assert(!controller.hasDeferredUnits());

    const std::optional<CheckpointSnapshot> paused =
        BuildImportCheckpoint::load(checkpoint_path.string());
    assert(paused);
    assert(paused->state == ImportState::Paused);
    assert(paused->phase_index == phaseIndex(ImportPhase::Structure));
    assert(paused->chunk_index == 0);
    assert(paused->completed_command_count == 0);
    assert(paused->completed_block_count == 0);
    assert(!paused->has_active_unit);
    assert(paused->active_spool_offset == 0);

    assert(controller.resume(world));
    const WorkUnit replay = acquireExpected(&controller, ImportPhase::Structure, 0, 0);
    assert(replay.sequence == first.sequence);
}

void testCancelCheckpointRetention(const ScopedTempDirectory& temp) {
    const fs::path checkpoint_path = temp.file("cancel.chk");
    const ImportConfig config =
        makeConfig(checkpoint_path, OverwritePolicy::PreserveExisting);
    const ImportIdentity identity = makeIdentity("cancel");
    const WorldContext world{"server-cancel", 0};

    BuildImportController controller;
    assert(controller.startPlanning(identity, world, config));
    assert(fs::exists(checkpoint_path));

    controller.cancel(false);
    assert(controller.state() == ImportState::Idle);
    assert(fs::exists(checkpoint_path));
    const std::optional<CheckpointSnapshot> retained =
        BuildImportCheckpoint::load(checkpoint_path.string());
    assert(retained);
    assert(retained->identity.job_id == identity.job_id);
    assert(retained->state == ImportState::Planning);

    controller.cancel(true);
    assert(controller.state() == ImportState::Idle);
    assert(!fs::exists(checkpoint_path));
}

}  // namespace

int main() {
    ScopedTempDirectory temp;
    testCoordinateFloorDivision();
    testClearCommandSplitting();
    testDenyFoundationPlanning(temp);
    testSimulationChunkRangeGeometry();
    testWorkUnitCompletionInvariants(temp);
    testRestoreFailureIsTransactional(temp);
    testInterruptedUnitReplayAndCheckpointTransactions(temp);
    testDynamicPauseRecovery(temp);
    testPreserveDynamicPauseCannotResume(temp);
    testCurrentDynamicCheckpointRestoration(temp);
    testDynamicUnitBoundaryRecovery(temp);
    testCompletedGlobalClearCheckpointRestoration(temp);
    testInterruptedGlobalClearRestoration(temp);
    testUnsafeAcquireDoesNotReturnUnitWhenCheckpointWriteFails(temp);
    testIdempotentAcquireUsesDurableCursor(temp);
    testCheckpointRejectsTrailingData(temp);
    testPreserveLastUnitImmediatelyStartsVerification(temp);
    testPlanVersionOrdering(temp);
    testAdaptiveRegionGridExecutionAndRestore(temp);
    testOverwriteRestoreAcrossNegativeRegions(temp);
    testTerminalCheckpointRestoration(temp);
    testRestoreRejectsInconsistentMetadata(temp);
    testLegacyV5CursorMigration(temp);
    testContextChangeRequiresOriginalWorld();
    testCrossPartitionDependencyAndEmptyOverwrite(temp);
    testDeferredCommitAndUnsafeBoundary(temp);
    testDeferredCommitFailureRestoresBaseline(temp);
    testDeferredPauseRollsBackBeforeCheckpoint(temp);
    testCancelCheckpointRetention(temp);
    return 0;
}
