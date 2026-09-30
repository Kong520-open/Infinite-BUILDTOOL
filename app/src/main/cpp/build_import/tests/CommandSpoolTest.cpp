#include "../CommandSpool.h"
#include "../ChunkSpoolWriter.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace build_import;

namespace {

struct RawDiskRecord {
    int32_t x, y, z;
    uint8_t aux, flags;
    uint16_t name_length;
};

void appendRaw(std::ofstream& stream, int32_t x, int32_t y, int32_t z,
               const std::string& name, uint8_t flags = 0x05, uint8_t aux = 0) {
    const RawDiskRecord record{x, y, z, aux, flags,
                               static_cast<uint16_t>(name.size())};
    stream.write(reinterpret_cast<const char*>(&record), sizeof(record));
    stream.write(name.data(), static_cast<std::streamsize>(name.size()));
}

void copyPlan(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::filesystem::copy_file(source, target,
                               std::filesystem::copy_options::overwrite_existing);
}

using BlockPosition = std::array<int32_t, 3>;
struct ExpectedBlock {
    std::string name;
    uint8_t aux = 0;
    uint8_t flags = 0;
};
using ExpectedBlockMap = std::map<BlockPosition, ExpectedBlock>;

bool expectedCanFill(uint8_t flags, ImportPhase phase) {
    if (phase == ImportPhase::Attachment ||
        phase == ImportPhase::DependentAttachment) return false;
    return flags == 0 ||
           ((flags & 0x04) != 0 && (flags & 0x01) != 0 &&
            (flags & 0x08) == 0);
}

bool expectedSingleLayer(uint8_t flags, ImportPhase phase) {
    return phase == ImportPhase::Gravity || phase == ImportPhase::Fluid ||
           ((flags & 0x04) != 0 && (flags & 0x02) != 0);
}

std::vector<uint32_t> assertExactCommandCover(const std::string& path,
                                              const ExpectedBlockMap& expected,
                                              uint32_t command_limit,
                                              ImportPhase phase) {
    CommandSpoolReader reader(path);
    assert(reader.valid());
    assert(reader.totalBlockCount() == expected.size());
    std::set<BlockPosition> seen;
    std::vector<uint32_t> command_sizes;
    uint64_t emitted_blocks = 0;
    while (const auto command = reader.next()) {
        const uint64_t width = static_cast<uint64_t>(
            static_cast<int64_t>(command->bounds.max_x) -
            command->bounds.min_x + 1);
        const uint64_t height = static_cast<uint64_t>(
            static_cast<int64_t>(command->bounds.max_y) -
            command->bounds.min_y + 1);
        const uint64_t depth = static_cast<uint64_t>(
            static_cast<int64_t>(command->bounds.max_z) -
            command->bounds.min_z + 1);
        const uint64_t volume = width * height * depth;
        assert(volume == command->block_count);
        assert(command->block_count <= command_limit);
        command_sizes.push_back(command->block_count);
        emitted_blocks += command->block_count;
        std::optional<uint8_t> command_flags;
        for (int32_t y = command->bounds.min_y; ; ++y) {
            for (int32_t z = command->bounds.min_z; ; ++z) {
                for (int32_t x = command->bounds.min_x; ; ++x) {
                    const BlockPosition position{x, y, z};
                    const auto found = expected.find(position);
                    assert(found != expected.end());
                    assert(found->second.name == command->name);
                    assert(found->second.aux == command->aux);
                    if (!command_flags) {
                        command_flags = found->second.flags;
                    } else {
                        // Merge metadata is normalized in the compact command,
                        // so exact-cover validation must still prove that the
                        // optimizer never combined distinct raw flag domains.
                        assert(*command_flags == found->second.flags);
                    }
                    assert(seen.insert(position).second);
                    if (x == command->bounds.max_x) break;
                }
                if (z == command->bounds.max_z) break;
            }
            if (y == command->bounds.max_y) break;
        }
        assert(command_flags.has_value());
        assert(hasPlannedCommandFlag(
            *command, PlannedCommandFlag::HasMergeMetadata));
        assert(hasPlannedCommandFlag(*command, PlannedCommandFlag::Fillable) ==
               expectedCanFill(*command_flags, phase));
        assert(hasPlannedCommandFlag(*command, PlannedCommandFlag::SingleLayer) ==
               expectedSingleLayer(*command_flags, phase));
        if (!expectedCanFill(*command_flags, phase)) {
            assert(command->block_count == 1);
        }
        if (expectedSingleLayer(*command_flags, phase)) {
            assert(command->bounds.min_y == command->bounds.max_y);
        }
    }
    assert(!reader.failed() && reader.exhausted());
    assert(emitted_blocks == expected.size());
    assert(seen.size() == expected.size());
    return command_sizes;
}

void assertRectangularCoverCase(const std::filesystem::path& root,
                                const std::string& case_name,
                                int32_t blocks_per_second,
                                uint32_t expected_limit,
                                int32_t width, int32_t height, int32_t depth) {
    assert(fillBlockLimitForRate(blocks_per_second) == expected_limit);
    const std::filesystem::path case_directory = root / case_name;
    std::filesystem::create_directories(case_directory);
    const std::filesystem::path raw_path = case_directory / "rectangle.bsp";
    constexpr int32_t kMinX = -4097;
    constexpr int32_t kMinY = 41;
    constexpr int32_t kMinZ = -2051;
    ExpectedBlockMap expected;
    {
        std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
        for (int32_t y = kMinY; y < kMinY + height; ++y) {
            for (int32_t z = kMinZ; z < kMinZ + depth; ++z) {
                for (int32_t x = kMinX; x < kMinX + width; ++x) {
                    appendRaw(raw, x, y, z, "minecraft:stained_hardened_clay",
                              0x05, 11);
                    expected[{x, y, z}] =
                        {"minecraft:stained_hardened_clay", 11, 0x05};
                }
            }
        }
        assert(raw);
    }

    ChunkDescriptor descriptor;
    descriptor.coord = {-129, -65};
    descriptor.imported_bounds = {
        kMinX, kMinY, kMinZ,
        kMinX + width - 1, kMinY + height - 1, kMinZ + depth - 1};
    descriptor.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    descriptor.spool_paths[phaseIndex(ImportPhase::Structure)] = raw_path.string();
    std::vector<ChunkDescriptor> chunks{descriptor};
    std::string error;
    assert(CommandSpoolBuilder::build(
        case_directory.string(), blocks_per_second,
        OverwritePolicy::PreserveExisting, &chunks, &error));

    const std::vector<uint32_t> command_sizes = assertExactCommandCover(
        chunks[0].command_paths[phaseIndex(ImportPhase::Structure)],
        expected, expected_limit, ImportPhase::Structure);
    const uint64_t total = expected.size();
    const uint32_t tail = static_cast<uint32_t>(total % expected_limit);
    assert(tail != 0);
    assert(command_sizes.size() ==
           static_cast<size_t>((total + expected_limit - 1) / expected_limit));
    assert(static_cast<size_t>(std::count(command_sizes.begin(), command_sizes.end(),
                                          expected_limit)) ==
           static_cast<size_t>(total / expected_limit));
    assert(std::count(command_sizes.begin(), command_sizes.end(), tail) == 1);
}

void assertMixedFlagPropertyCase(const std::filesystem::path& root,
                                 uint32_t seed,
                                 int32_t blocks_per_second,
                                 uint32_t expected_limit) {
    struct PaletteBlock {
        const char* name;
        uint8_t aux;
        uint8_t flags;
    };
    const std::array<PaletteBlock, 6> palette{{
        {"minecraft:stone", 0, 0x05},
        {"minecraft:stone", 0, 0x07},
        {"minecraft:stone", 0, 0x08},
        {"minecraft:wool", 14, 0x05},
        {"minecraft:wool", 14, 0x07},
        {"minecraft:torch", 0, 0x08},
    }};
    assert(fillBlockLimitForRate(blocks_per_second) == expected_limit);
    const std::filesystem::path case_directory =
        root / ("mixed_" + std::to_string(seed) + "_" +
                std::to_string(expected_limit));
    std::filesystem::create_directories(case_directory);
    const std::filesystem::path raw_path = case_directory / "mixed.bsp";
    ExpectedBlockMap expected;
    uint32_t state = seed;
    const auto nextRandom = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state;
    };
    bool saw_fillable = false;
    bool saw_single_layer = false;
    bool saw_non_fillable = false;
    constexpr int32_t kMinX = -47;
    constexpr int32_t kMinY = 54;
    constexpr int32_t kMinZ = -39;
    constexpr int32_t kWidth = 11;
    constexpr int32_t kHeight = 5;
    constexpr int32_t kDepth = 9;
    {
        std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
        for (int32_t y = kMinY; y < kMinY + kHeight; ++y) {
            for (int32_t z = kMinZ; z < kMinZ + kDepth; ++z) {
                for (int32_t x = kMinX; x < kMinX + kWidth; ++x) {
                    if (nextRandom() % 7 == 0) continue;
                    const PaletteBlock& block = palette[nextRandom() % palette.size()];
                    appendRaw(raw, x, y, z, block.name, block.flags, block.aux);
                    expected[{x, y, z}] = {block.name, block.aux, block.flags};
                    saw_fillable = saw_fillable ||
                        expectedCanFill(block.flags, ImportPhase::Structure);
                    saw_single_layer = saw_single_layer ||
                        expectedSingleLayer(block.flags, ImportPhase::Structure);
                    saw_non_fillable = saw_non_fillable ||
                        !expectedCanFill(block.flags, ImportPhase::Structure);
                }
            }
        }
        assert(raw);
    }
    assert(!expected.empty());
    assert(saw_fillable && saw_single_layer && saw_non_fillable);

    ChunkDescriptor descriptor;
    descriptor.coord = {-2, -2};
    descriptor.imported_bounds = {
        kMinX, kMinY, kMinZ,
        kMinX + kWidth - 1, kMinY + kHeight - 1, kMinZ + kDepth - 1};
    descriptor.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    descriptor.spool_paths[phaseIndex(ImportPhase::Structure)] = raw_path.string();
    std::vector<ChunkDescriptor> chunks{descriptor};
    std::string error;
    assert(CommandSpoolBuilder::build(
        case_directory.string(), blocks_per_second,
        OverwritePolicy::PreserveExisting, &chunks, &error));
    assertExactCommandCover(
        chunks[0].command_paths[phaseIndex(ImportPhase::Structure)],
        expected, expected_limit, ImportPhase::Structure);
}

std::string buildStructureCase(const std::filesystem::path& root,
                               const std::string& case_name,
                               const ExpectedBlockMap& expected,
                               int32_t blocks_per_second) {
    assert(!expected.empty());
    const std::filesystem::path case_directory = root / case_name;
    std::filesystem::create_directories(case_directory);
    const std::filesystem::path raw_path = case_directory / "structure.bsp";
    BlockBounds bounds;
    bool first = true;
    {
        std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
        for (const auto& entry : expected) {
            const BlockPosition& position = entry.first;
            const ExpectedBlock& block = entry.second;
            appendRaw(raw, position[0], position[1], position[2],
                      block.name, block.flags, block.aux);
            if (first) {
                bounds = {position[0], position[1], position[2],
                          position[0], position[1], position[2]};
                first = false;
            } else {
                bounds.min_x = std::min(bounds.min_x, position[0]);
                bounds.min_y = std::min(bounds.min_y, position[1]);
                bounds.min_z = std::min(bounds.min_z, position[2]);
                bounds.max_x = std::max(bounds.max_x, position[0]);
                bounds.max_y = std::max(bounds.max_y, position[1]);
                bounds.max_z = std::max(bounds.max_z, position[2]);
            }
        }
        assert(raw);
    }

    ChunkDescriptor descriptor;
    descriptor.coord = {0, 0};
    descriptor.imported_bounds = bounds;
    descriptor.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    descriptor.spool_paths[phaseIndex(ImportPhase::Structure)] = raw_path.string();
    std::vector<ChunkDescriptor> chunks{descriptor};
    std::string error;
    assert(CommandSpoolBuilder::build(
        case_directory.string(), blocks_per_second,
        OverwritePolicy::PreserveExisting, 1, &chunks, &error,
        {}, false, {}, false));
    assert(chunks.size() == 1);
    const std::string command_path =
        chunks[0].command_paths[phaseIndex(ImportPhase::Structure)];
    assert(!command_path.empty());
    return command_path;
}

void assertBitmapMaterialAndAuxIsolation(const std::filesystem::path& root) {
    ExpectedBlockMap expected;
    const auto addPair = [&](int32_t min_x, const char* name, uint8_t aux) {
        for (int32_t x = min_x; x < min_x + 2; ++x) {
            expected[{x, -12, -7}] = {name, aux, 0x05};
        }
    };
    addPair(-8, "minecraft:stone", 0);
    addPair(-6, "minecraft:stone", 1);
    addPair(-4, "minecraft:wool", 1);
    addPair(-2, "minecraft:wool", 14);

    const std::string command_path = buildStructureCase(
        root, "bitmap_material_aux_isolation", expected, 5000);
    const std::vector<uint32_t> sizes = assertExactCommandCover(
        command_path, expected, fillBlockLimitForRate(5000),
        ImportPhase::Structure);
    assert(sizes.size() == 4);
    assert(std::all_of(sizes.begin(), sizes.end(),
                       [](uint32_t size) { return size == 2; }));
}

void assertNegativeCoordinateBitmapCover(const std::filesystem::path& root) {
    ExpectedBlockMap expected;
    for (int32_t y = -4; y <= -3; ++y) {
        for (int32_t z = -9; z <= -7; ++z) {
            for (int32_t x = -13; x <= -10; ++x) {
                expected[{x, y, z}] = {"minecraft:stone", 0, 0x05};
            }
        }
    }
    const std::string command_path = buildStructureCase(
        root, "bitmap_negative_coordinates", expected, 5000);
    assert(assertExactCommandCover(
               command_path, expected, fillBlockLimitForRate(5000),
               ImportPhase::Structure) == std::vector<uint32_t>({24}));

    CommandSpoolReader reader(command_path);
    const std::optional<PlannedCommand> command = reader.next();
    assert(command.has_value());
    assert(command->bounds.min_x == -13 && command->bounds.max_x == -10);
    assert(command->bounds.min_y == -4 && command->bounds.max_y == -3);
    assert(command->bounds.min_z == -9 && command->bounds.max_z == -7);
    assert(reader.exhausted());
}

void assertSparseIndexFallbackCover(const std::filesystem::path& root) {
    // The multi-trillion-cell bounding volume is intentionally much larger
    // than the dense index threshold. Only these six occupied cells may be
    // emitted when RecordIndex and PositionFlags use their sparse fallback.
    ExpectedBlockMap expected;
    expected[{-1000000, 64, -1000000}] = {"minecraft:stone", 0, 0x05};
    expected[{-999999, 64, -1000000}] = {"minecraft:stone", 0, 0x05};
    expected[{1000000, 65, 1000000}] = {"minecraft:wool", 4, 0x05};
    expected[{1000000, 66, 1000000}] = {"minecraft:wool", 4, 0x05};
    expected[{-500000, 65, 500000}] = {"minecraft:concrete", 7, 0x05};
    expected[{-500000, 65, 500001}] = {"minecraft:concrete", 7, 0x05};

    const std::string command_path = buildStructureCase(
        root, "bitmap_sparse_index_fallback", expected, 5000);
    std::vector<uint32_t> sizes = assertExactCommandCover(
        command_path, expected, fillBlockLimitForRate(5000),
        ImportPhase::Structure);
    std::sort(sizes.begin(), sizes.end());
    assert(sizes == std::vector<uint32_t>({2, 2, 2}));
}

void assertSixAxisCoverDoesNotRegress(const std::filesystem::path& root) {
    // This offset 3D shape has a three-command locally-largest cover, while a
    // fixed whole-plan axis order covers it with two 3x2x1 cuboids. Rotate the
    // shape through every axis permutation so all six candidates must retain
    // the better result instead of falling back to the local cover.
    constexpr std::array<std::array<int, 3>, 6> permutations{{
        {{0, 1, 2}}, {{0, 2, 1}}, {{1, 0, 2}},
        {{1, 2, 0}}, {{2, 0, 1}}, {{2, 1, 0}},
    }};
    for (size_t permutation_index = 0;
         permutation_index < permutations.size(); ++permutation_index) {
        ExpectedBlockMap expected;
        const auto add = [&](int32_t a, int32_t b, int32_t c) {
            std::array<int32_t, 3> position{{-20, 40, -11}};
            position[permutations[permutation_index][0]] += a;
            position[permutations[permutation_index][1]] += b;
            position[permutations[permutation_index][2]] += c;
            expected[position] = {"minecraft:wool", 7, 0x05};
        };
        for (int32_t b = 0; b <= 1; ++b) {
            for (int32_t a = 1; a <= 3; ++a) add(a, b, 0);
            for (int32_t a = 0; a <= 2; ++a) add(a, b, 1);
        }

        const std::string command_path = buildStructureCase(
            root, "bitmap_six_axis_" + std::to_string(permutation_index),
            expected, 5000);
        std::vector<uint32_t> sizes = assertExactCommandCover(
            command_path, expected, fillBlockLimitForRate(5000),
            ImportPhase::Structure);
        std::sort(sizes.begin(), sizes.end());
        assert(sizes == std::vector<uint32_t>({6, 6}));
    }
}

void assertDuplicatePositionsRejected(const std::filesystem::path& root) {
    const std::filesystem::path case_directory = root / "duplicate_positions";
    std::filesystem::create_directories(case_directory);
    const std::filesystem::path raw_path = case_directory / "duplicates.bsp";
    {
        std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
        appendRaw(raw, -17, 64, 33, "minecraft:stone", 0x05, 0);
        appendRaw(raw, -17, 64, 33, "minecraft:wool", 0x05, 14);
        assert(raw);
    }

    ChunkDescriptor descriptor;
    descriptor.coord = {-1, 1};
    descriptor.imported_bounds = {-17, 64, 33, -17, 64, 33};
    descriptor.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    descriptor.spool_paths[phaseIndex(ImportPhase::Structure)] = raw_path.string();
    std::vector<ChunkDescriptor> chunks{descriptor};
    std::string error;
    assert(!CommandSpoolBuilder::build(
        case_directory.string(), 5000, OverwritePolicy::PreserveExisting,
        &chunks, &error));
    assert(error == "raw chunk spool contains invalid or duplicate blocks");
}

template <typename T>
void appendBinary(std::ofstream* stream, const T& value) {
    stream->write(reinterpret_cast<const char*>(&value), sizeof(value));
    assert(*stream);
}

void writeLegacyV1CommandSpool(const std::filesystem::path& path) {
    constexpr uint32_t kCommandMagic = 0x444D4342;
    constexpr uint32_t kLegacyCommandVersion = 1;
    constexpr uint64_t kCommandCount = 1;
    constexpr uint64_t kBlockCount = 1;
    constexpr BlockBounds kBounds{7, 64, -3, 7, 64, -3};
    constexpr uint32_t kRecordBlockCount = 1;
    constexpr uint8_t kAux = 0;
    constexpr uint8_t kReserved = 0;
    const std::string name = "minecraft:stone";
    const uint16_t name_length = static_cast<uint16_t>(name.size());

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    assert(stream);
    appendBinary(&stream, kCommandMagic);
    appendBinary(&stream, kLegacyCommandVersion);
    appendBinary(&stream, kCommandCount);
    appendBinary(&stream, kBlockCount);
    appendBinary(&stream, kBounds.min_x);
    appendBinary(&stream, kBounds.min_y);
    appendBinary(&stream, kBounds.min_z);
    appendBinary(&stream, kBounds.max_x);
    appendBinary(&stream, kBounds.max_y);
    appendBinary(&stream, kBounds.max_z);
    appendBinary(&stream, kRecordBlockCount);
    appendBinary(&stream, kAux);
    appendBinary(&stream, kReserved);
    appendBinary(&stream, name_length);
    stream.write(name.data(), static_cast<std::streamsize>(name.size()));
    assert(stream);
}

void writeLegacyV2PlanWithEightAirSamples(const std::filesystem::path& path) {
    constexpr uint32_t kVerificationMagic = 0x50564942;
    constexpr uint32_t kVerificationVersion = 2;
    const uint64_t chunk_count = 1;
    const uint64_t sample_count = 8;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    assert(stream);
    appendBinary(&stream, kVerificationMagic);
    appendBinary(&stream, kVerificationVersion);
    appendBinary(&stream, chunk_count);
    appendBinary(&stream, sample_count);

    const ChunkCoord coord{7, -3};
    const BlockBounds bounds{0, 64, 0, 8, 64, 0};
    const uint64_t total_block_count = 1;
    const uint64_t eligible_count = 0;
    const uint32_t chunk_sample_count = 8;
    const uint32_t reserved = 0;
    appendBinary(&stream, coord.x);
    appendBinary(&stream, coord.z);
    appendBinary(&stream, bounds.min_x);
    appendBinary(&stream, bounds.min_y);
    appendBinary(&stream, bounds.min_z);
    appendBinary(&stream, bounds.max_x);
    appendBinary(&stream, bounds.max_y);
    appendBinary(&stream, bounds.max_z);
    appendBinary(&stream, total_block_count);
    appendBinary(&stream, eligible_count);
    appendBinary(&stream, chunk_sample_count);
    appendBinary(&stream, reserved);

    const std::string name = "minecraft:air";
    const uint8_t aux = 0;
    const uint8_t flags = static_cast<uint8_t>(VerificationSampleFlag::ExpectedAir) |
                          static_cast<uint8_t>(VerificationSampleFlag::IgnoreAux);
    const uint16_t name_length = static_cast<uint16_t>(name.size());
    for (int32_t x = 0; x < 8; ++x) {
        const int32_t y = 64;
        const int32_t z = 0;
        appendBinary(&stream, x);
        appendBinary(&stream, y);
        appendBinary(&stream, z);
        appendBinary(&stream, aux);
        appendBinary(&stream, flags);
        appendBinary(&stream, name_length);
        stream.write(name.data(), static_cast<std::streamsize>(name.size()));
        assert(stream);
    }
}

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "build_import_command_spool_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const std::filesystem::path legacy_command_path =
        directory / "legacy_v1.bsp.cmd";
    writeLegacyV1CommandSpool(legacy_command_path);
    {
        CommandSpoolReader legacy_command_reader(legacy_command_path.string());
        assert(legacy_command_reader.valid());
        assert(legacy_command_reader.commandCount() == 1);
        assert(legacy_command_reader.totalBlockCount() == 1);
        const std::optional<PlannedCommand> legacy_command = legacy_command_reader.next();
        assert(legacy_command.has_value());
        assert(legacy_command->bounds.min_x == 7 && legacy_command->bounds.min_y == 64 &&
               legacy_command->bounds.min_z == -3);
        assert(legacy_command->name == "minecraft:stone" && legacy_command->aux == 0);
        assert(legacy_command->flags == 0);
        assert(!hasPlannedCommandFlag(
            *legacy_command, PlannedCommandFlag::HasMergeMetadata));
        assert(legacy_command_reader.exhausted());
    }
    {

    const std::filesystem::path structure_path = directory / "chunk_-1_0_phase_1.bsp";
    {
        std::ofstream raw(structure_path, std::ios::binary | std::ios::trunc);
        for (int32_t x = -32; x < 0; ++x) {
            appendRaw(raw, x, 64, 0, "minecraft:stone");
        }
        // Extends the imported bounds to a second row while leaving 31 cells
        // empty so destructive overwrite gets expected-air samples.
        appendRaw(raw, -32, 64, 1, "minecraft:stone");
        assert(raw);
    }
    const std::filesystem::path gravity_path = directory / "chunk_0_0_phase_2.bsp";
    {
        std::ofstream raw(gravity_path, std::ios::binary | std::ios::trunc);
        appendRaw(raw, 0, 64, 0, "minecraft:sand", 0x07);
        assert(raw);
    }
    const std::filesystem::path fluid_path = directory / "chunk_2_0_phase_4.bsp";
    {
        std::ofstream raw(fluid_path, std::ios::binary | std::ios::trunc);
        appendRaw(raw, 64, 64, 0, "minecraft:water", 0x07);
        appendRaw(raw, 65, 64, 0, "minecraft:flowing_water", 0x07, 5);
        assert(raw);
    }
    const std::filesystem::path shell_path = directory / "chunk_1_0_phase_1.bsp";
    {
        std::ofstream raw(shell_path, std::ios::binary | std::ios::trunc);
        for (int32_t y = 64; y <= 66; ++y) {
            for (int32_t z = 0; z <= 2; ++z) {
                for (int32_t x = 32; x <= 34; ++x) {
                    if (x == 33 && y == 65 && z == 1) continue;
                    appendRaw(raw, x, y, z, "minecraft:stone");
                }
            }
        }
        assert(raw);
    }

    ChunkDescriptor structure;
    structure.coord = {-1, 0};
    structure.imported_bounds = {-32, 64, 0, -1, 64, 1};
    structure.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    structure.spool_paths[phaseIndex(ImportPhase::Structure)] = structure_path.string();
    ChunkDescriptor gravity;
    gravity.coord = {0, 0};
    gravity.imported_bounds = {0, 64, 0, 0, 64, 0};
    gravity.has_phase[phaseIndex(ImportPhase::Gravity)] = true;
    gravity.spool_paths[phaseIndex(ImportPhase::Gravity)] = gravity_path.string();
    ChunkDescriptor shell;
    shell.coord = {1, 0};
    shell.imported_bounds = {32, 64, 0, 34, 66, 2};
    shell.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    shell.spool_paths[phaseIndex(ImportPhase::Structure)] = shell_path.string();
    ChunkDescriptor fluid;
    fluid.coord = {2, 0};
    fluid.imported_bounds = {64, 64, 0, 65, 64, 0};
    fluid.has_phase[phaseIndex(ImportPhase::Fluid)] = true;
    fluid.spool_paths[phaseIndex(ImportPhase::Fluid)] = fluid_path.string();

    std::vector<ChunkDescriptor> chunks{structure, gravity, shell, fluid};
    std::string error;
    std::vector<ChunkDescriptor> cancelled_chunks = chunks;
    assert(!CommandSpoolBuilder::build(
        directory.string(), 20, OverwritePolicy::ClearImportedBounds,
        &cancelled_chunks, &error, [] { return true; }));
    assert(error == "import cancelled");
    error.clear();
    assert(CommandSpoolBuilder::build(directory.string(), 20,
                                      OverwritePolicy::ClearImportedBounds,
                                      &chunks, &error));
    // Raw per-block files are released phase-by-phase once the compact command
    // stream has been finalized.
    assert(!std::filesystem::exists(structure_path));
    assert(!std::filesystem::exists(gravity_path));
    assert(!std::filesystem::exists(shell_path));
    assert(!std::filesystem::exists(fluid_path));

    CommandSpoolReader reader(
        chunks[0].command_paths[phaseIndex(ImportPhase::Structure)]);
    assert(reader.valid());
    assert(!reader.failed());
    assert(!reader.exhausted());
    assert(reader.commandsRead() == 0);
    assert(reader.totalBlockCount() == 33);
    const uint64_t data_offset = reader.offset();
    uint64_t emitted = 0;
    std::vector<uint64_t> record_boundaries;
    while (const auto command = reader.next()) {
        assert(command->block_count > 0);
        assert(command->block_count <= 64);  // 20 bps uses the 64-block merge floor.
        emitted += command->block_count;
        record_boundaries.push_back(reader.offset());
    }
    assert(!reader.failed());
    assert(reader.exhausted());  // Known immediately after the last record, without another read.
    assert(reader.commandsRead() == reader.commandCount());
    assert(emitted == 33);
    assert(record_boundaries.size() == reader.commandCount());
    assert(!reader.next().has_value());
    assert(reader.exhausted());

    const std::filesystem::path corrupt_command = directory / "corrupt_payload.bsp.cmd";
    copyPlan(chunks[0].command_paths[phaseIndex(ImportPhase::Structure)], corrupt_command);
    std::filesystem::resize_file(corrupt_command,
                                 std::filesystem::file_size(corrupt_command) - 1);
    CommandSpoolReader corrupt_reader(corrupt_command.string());
    assert(corrupt_reader.valid());  // The header alone remains syntactically valid.
    assert(corrupt_reader.seek(corrupt_reader.offset()));
    while (corrupt_reader.next()) {}
    assert(corrupt_reader.failed());
    assert(!corrupt_reader.exhausted());

    const std::filesystem::path invalid_flags_command =
        directory / "invalid_command_flags.bsp.cmd";
    copyPlan(chunks[0].command_paths[phaseIndex(ImportPhase::Structure)],
             invalid_flags_command);
    {
        std::fstream invalid_flags(
            invalid_flags_command,
            std::ios::binary | std::ios::in | std::ios::out);
        // v3 command records carry a uint16 auxiliary value before their
        // one-byte merge flags.  Keep this corruption test aimed at the flag
        // byte rather than the high half of Aux.
        const std::streamoff flags_offset = static_cast<std::streamoff>(
            data_offset + sizeof(int32_t) * 6 + sizeof(uint32_t) + sizeof(uint16_t));
        invalid_flags.seekp(flags_offset, std::ios::beg);
        const uint8_t unknown_flag = 0x80;
        invalid_flags.write(reinterpret_cast<const char*>(&unknown_flag),
                            sizeof(unknown_flag));
        assert(invalid_flags);
    }
    CommandSpoolReader invalid_flags_reader(invalid_flags_command.string());
    assert(invalid_flags_reader.valid());
    assert(invalid_flags_reader.seek(invalid_flags_reader.offset()));
    assert(!invalid_flags_reader.next().has_value());
    assert(invalid_flags_reader.failed());

    CommandSpoolReader seek_reader(
        chunks[0].command_paths[phaseIndex(ImportPhase::Structure)]);
    assert(seek_reader.seek(record_boundaries.front()));
    assert(seek_reader.commandsRead() == 1);
    const uint64_t saved_offset = seek_reader.offset();
    assert(seek_reader.seek(saved_offset));
    assert(seek_reader.commandsRead() == 1);
    assert(!seek_reader.seek(saved_offset - 1));  // A byte inside a record is not resumable.
    assert(seek_reader.offset() == saved_offset);
    assert(seek_reader.commandsRead() == 1);
    assert(seek_reader.next().has_value());
    assert(seek_reader.commandsRead() == 2);
    assert(seek_reader.seek(data_offset));
    assert(seek_reader.commandsRead() == 0);
    assert(!seek_reader.exhausted());
    assert(seek_reader.seek(record_boundaries.back()));
    assert(seek_reader.commandsRead() == seek_reader.commandCount());
    assert(seek_reader.exhausted());

    const std::filesystem::path plan_path =
        directory / CommandSpoolBuilder::kVerificationPlanName;
    std::vector<VerificationChunkPlan> plans;
    assert(CommandSpoolBuilder::loadVerificationPlan(plan_path.string(), &plans, &error));
    assert(plans.size() == 4);
    assert(plans[0].coord == structure.coord);
    assert(plans[0].total_block_count == 33);
    assert(plans[0].eligible_count == 33);
    assert(plans[0].samples.size() == 20);
    uint32_t stable_samples = 0;
    uint32_t air_samples = 0;
    std::set<std::array<int32_t, 3>> positions;
    for (const VerificationPlanSample& sample : plans[0].samples) {
        assert(positions.insert({sample.x, sample.y, sample.z}).second);
        if (hasVerificationSampleFlag(sample, VerificationSampleFlag::ExpectedAir)) {
            ++air_samples;
            assert(sample.name == "minecraft:air");
            assert(sample.aux == 0);
            assert(hasVerificationSampleFlag(sample, VerificationSampleFlag::IgnoreAux));
            assert(sample.y == 64 && sample.z == 1 && sample.x > -32);
        } else {
            ++stable_samples;
            assert(sample.name == "minecraft:stone");
        }
    }
    assert(stable_samples == 16);
    assert(air_samples == 4);

    // Gravity chunks remain in the plan so callers can report the verification
    // coverage gap, but moving gravity blocks are not stable enough to sample.
    assert(plans[1].coord == gravity.coord);
    assert(plans[1].total_block_count == 1);
    assert(plans[1].eligible_count == 0);
    assert(plans[1].samples.empty());

    // Only a fully enclosed cavity is safe to assert as expected air.
    assert(plans[2].coord == shell.coord);
    assert(plans[2].total_block_count == 26);
    assert(plans[2].eligible_count == 26);
    assert(plans[2].samples.size() == 17);
    uint32_t shell_stable_samples = 0;
    uint32_t shell_air_samples = 0;
    for (const VerificationPlanSample& sample : plans[2].samples) {
        if (hasVerificationSampleFlag(sample, VerificationSampleFlag::ExpectedAir)) {
            ++shell_air_samples;
            assert(sample.x == 33 && sample.y == 65 && sample.z == 1);
            assert(sample.name == "minecraft:air");
            assert(hasVerificationSampleFlag(sample, VerificationSampleFlag::IgnoreAux));
        } else {
            ++shell_stable_samples;
        }
    }
    assert(shell_stable_samples == 16);
    assert(shell_air_samples == 1);

    assert(plans[3].coord == fluid.coord);
    assert(plans[3].total_block_count == 2);
    assert(plans[3].eligible_count == 2);
    assert(plans[3].samples.size() == 2);
    assert(plans[3].samples[0].name == "minecraft:water");
    assert(plans[3].samples[0].aux == 0);
    assert(hasVerificationSampleFlag(
        plans[3].samples[0], VerificationSampleFlag::IgnoreAux));
    assert(plans[3].samples[1].name == "minecraft:flowing_water");
    assert(plans[3].samples[1].aux == 5);
    assert(hasVerificationSampleFlag(
        plans[3].samples[1], VerificationSampleFlag::IgnoreAux));

    std::vector<VerificationPlanSample> flat_samples;
    assert(CommandSpoolBuilder::loadVerificationPlan(
        plan_path.string(), &flat_samples, &error));
    assert(flat_samples.size() == 39);

    const std::filesystem::path legacy_air_plan = directory / "legacy_v2_eight_air.plan";
    writeLegacyV2PlanWithEightAirSamples(legacy_air_plan);
    std::vector<VerificationChunkPlan> legacy_air_chunks;
    assert(CommandSpoolBuilder::loadVerificationPlan(
        legacy_air_plan.string(), &legacy_air_chunks, &error));
    assert(legacy_air_chunks.size() == 1);
    assert(legacy_air_chunks[0].samples.size() == 8);
    for (const VerificationPlanSample& sample : legacy_air_chunks[0].samples) {
        assert(hasVerificationSampleFlag(sample, VerificationSampleFlag::ExpectedAir));
        assert(hasVerificationSampleFlag(sample, VerificationSampleFlag::IgnoreAux));
        assert(sample.name == "minecraft:air" && sample.aux == 0);
    }

    const std::filesystem::path trailing_plan = directory / "trailing.plan";
    copyPlan(plan_path, trailing_plan);
    {
        std::ofstream trailing(trailing_plan, std::ios::binary | std::ios::app);
        trailing.put('\0');
    }
    plans.clear();
    assert(!CommandSpoolBuilder::loadVerificationPlan(
        trailing_plan.string(), &plans, &error));
    assert(plans.empty());

    const std::filesystem::path forged_count_plan = directory / "forged_count.plan";
    copyPlan(plan_path, forged_count_plan);
    {
        std::fstream forged(forged_count_plan, std::ios::binary | std::ios::in | std::ios::out);
        const uint64_t impossible_count = std::numeric_limits<uint64_t>::max();
        forged.seekp(8, std::ios::beg);
        forged.write(reinterpret_cast<const char*>(&impossible_count),
                     sizeof(impossible_count));
    }
    assert(!CommandSpoolBuilder::loadVerificationPlan(
        forged_count_plan.string(), &plans, &error));
    assert(plans.empty());

    const std::filesystem::path invalid_bounds_plan = directory / "invalid_bounds.plan";
    copyPlan(plan_path, invalid_bounds_plan);
    {
        std::fstream invalid(invalid_bounds_plan, std::ios::binary | std::ios::in | std::ios::out);
        const int32_t invalid_max_x = -33;
        invalid.seekp(44, std::ios::beg);  // v2 header + coord + min bounds.
        invalid.write(reinterpret_cast<const char*>(&invalid_max_x), sizeof(invalid_max_x));
    }
    assert(!CommandSpoolBuilder::loadVerificationPlan(
        invalid_bounds_plan.string(), &plans, &error));
    assert(plans.empty());

    const std::filesystem::path truncated_raw = directory / "truncated_raw.bsp";
    {
        std::ofstream raw(truncated_raw, std::ios::binary | std::ios::trunc);
        appendRaw(raw, 32, 64, 0, "minecraft:stone");
        const uint32_t partial_header = 0x12345678;
        raw.write(reinterpret_cast<const char*>(&partial_header), sizeof(partial_header));
        assert(raw);
    }
    ChunkDescriptor truncated;
    truncated.coord = {1, 0};
    truncated.imported_bounds = {32, 64, 0, 32, 64, 0};
    truncated.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    truncated.spool_paths[phaseIndex(ImportPhase::Structure)] = truncated_raw.string();
    std::vector<ChunkDescriptor> truncated_chunks{truncated};
    error.clear();
    assert(!CommandSpoolBuilder::build(directory.string(), 20,
                                       OverwritePolicy::PreserveExisting,
                                       &truncated_chunks, &error));
    assert(error == "truncated raw chunk spool");

    const std::filesystem::path edge_directory = directory / "edge";
    std::filesystem::create_directories(edge_directory);
    const std::filesystem::path edge_raw = edge_directory / "edge.bsp";
    {
        std::ofstream raw(edge_raw, std::ios::binary | std::ios::trunc);
        appendRaw(raw, std::numeric_limits<int32_t>::max() - 1, 64, 0,
                  "minecraft:stone");
        appendRaw(raw, std::numeric_limits<int32_t>::max(), 64, 0,
                  "minecraft:stone");
    }
    ChunkDescriptor edge;
    edge.coord = {std::numeric_limits<int32_t>::max() / 32, 0};
    edge.imported_bounds = {std::numeric_limits<int32_t>::max() - 1, 64, 0,
                            std::numeric_limits<int32_t>::max(), 64, 0};
    edge.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    edge.spool_paths[phaseIndex(ImportPhase::Structure)] = edge_raw.string();
    std::vector<ChunkDescriptor> edge_chunks{edge};
    assert(CommandSpoolBuilder::build(edge_directory.string(), 5000,
                                      OverwritePolicy::PreserveExisting,
                                      &edge_chunks, &error));
    CommandSpoolReader edge_reader(
        edge_chunks[0].command_paths[phaseIndex(ImportPhase::Structure)]);
    const auto edge_command = edge_reader.next();
    assert(edge_command.has_value());
    assert(edge_command->block_count == 2);
    assert(edge_command->bounds.max_x == std::numeric_limits<int32_t>::max());
    assert(edge_reader.exhausted());

    const std::filesystem::path full_3d_directory = directory / "full_3d";
    std::filesystem::create_directories(full_3d_directory);
    const std::filesystem::path full_3d_raw = full_3d_directory / "full_3d.bsp";
    {
        std::ofstream raw(full_3d_raw, std::ios::binary | std::ios::trunc);
        for (int32_t y = 64; y <= 66; ++y) {
            for (int32_t z = 0; z <= 1; ++z) {
                for (int32_t x = 0; x <= 3; ++x) {
                    appendRaw(raw, x, y, z, "minecraft:stone");
                }
            }
        }
        assert(raw);
    }
    ChunkDescriptor full_3d;
    full_3d.coord = {0, 0};
    full_3d.imported_bounds = {0, 64, 0, 3, 66, 1};
    full_3d.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    full_3d.spool_paths[phaseIndex(ImportPhase::Structure)] = full_3d_raw.string();
    std::vector<ChunkDescriptor> full_3d_chunks{full_3d};
    assert(CommandSpoolBuilder::build(full_3d_directory.string(), 5000,
                                      OverwritePolicy::PreserveExisting,
                                      &full_3d_chunks, &error));
    CommandSpoolReader full_3d_reader(
        full_3d_chunks[0].command_paths[phaseIndex(ImportPhase::Structure)]);
    assert(full_3d_reader.valid());
    assert(full_3d_reader.commandCount() == 1);
    const auto full_3d_command = full_3d_reader.next();
    assert(full_3d_command.has_value());
    assert(full_3d_command->bounds.min_x == 0 && full_3d_command->bounds.max_x == 3);
    assert(full_3d_command->bounds.min_y == 64 && full_3d_command->bounds.max_y == 66);
    assert(full_3d_command->bounds.min_z == 0 && full_3d_command->bounds.max_z == 1);
    assert(full_3d_command->block_count == 24);
    assert(full_3d_reader.exhausted());

    const std::filesystem::path offset_stripes_directory =
        directory / "offset_stripes";
    std::filesystem::create_directories(offset_stripes_directory);
    const std::filesystem::path offset_stripes_raw =
        offset_stripes_directory / "offset_stripes.bsp";
    {
        std::ofstream raw(offset_stripes_raw, std::ios::binary | std::ios::trunc);
        // A seed-local largest-cuboid choice consumes the 2x2 overlap and
        // leaves both ends as singletons (three commands). Whole-plan X-first
        // merging recognizes the two offset runs and emits two commands.
        for (int32_t x = 1; x <= 3; ++x) {
            appendRaw(raw, x, 64, 0, "minecraft:wool", 0x05, 3);
        }
        for (int32_t x = 0; x <= 2; ++x) {
            appendRaw(raw, x, 64, 1, "minecraft:wool", 0x05, 3);
        }
        assert(raw);
    }
    ChunkDescriptor offset_stripes;
    offset_stripes.coord = {0, 0};
    offset_stripes.imported_bounds = {0, 64, 0, 3, 64, 1};
    offset_stripes.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    offset_stripes.spool_paths[phaseIndex(ImportPhase::Structure)] =
        offset_stripes_raw.string();
    std::vector<ChunkDescriptor> offset_stripes_chunks{offset_stripes};
    assert(CommandSpoolBuilder::build(
        offset_stripes_directory.string(), 20,
        OverwritePolicy::PreserveExisting, &offset_stripes_chunks, &error));
    CommandSpoolReader offset_stripes_reader(
        offset_stripes_chunks[0].command_paths[phaseIndex(ImportPhase::Structure)]);
    assert(offset_stripes_reader.valid());
    assert(offset_stripes_reader.commandCount() == 2);
    std::set<int32_t> offset_stripe_rows;
    while (const auto command = offset_stripes_reader.next()) {
        assert(command->name == "minecraft:wool" && command->aux == 3);
        assert(command->bounds.min_y == 64 && command->bounds.max_y == 64);
        assert(command->bounds.min_z == command->bounds.max_z);
        assert(command->block_count == 3);
        offset_stripe_rows.insert(command->bounds.min_z);
    }
    assert(offset_stripes_reader.exhausted());
    assert(offset_stripe_rows == std::set<int32_t>({0, 1}));

    const std::filesystem::path single_axis_directory =
        directory / "single_axis_counterexample";
    std::filesystem::create_directories(single_axis_directory);
    const std::filesystem::path single_axis_raw =
        single_axis_directory / "single_axis.bsp";
    {
        std::ofstream raw(single_axis_raw, std::ios::binary | std::ios::trunc);
        // 0100
        // 1110
        // Any cover that greedily expands into the second row consumes the
        // overlap and leaves two singletons. Pure X runs need only two commands.
        appendRaw(raw, 1, 64, 0, "minecraft:wool", 0x05, 5);
        for (int32_t x = 0; x <= 2; ++x) {
            appendRaw(raw, x, 64, 1, "minecraft:wool", 0x05, 5);
        }
        assert(raw);
    }
    ChunkDescriptor single_axis;
    single_axis.coord = {0, 0};
    single_axis.imported_bounds = {0, 64, 0, 3, 64, 1};
    single_axis.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    single_axis.spool_paths[phaseIndex(ImportPhase::Structure)] =
        single_axis_raw.string();
    std::vector<ChunkDescriptor> single_axis_chunks{single_axis};
    assert(CommandSpoolBuilder::build(
        single_axis_directory.string(), 20,
        OverwritePolicy::PreserveExisting, &single_axis_chunks, &error));
    CommandSpoolReader single_axis_reader(
        single_axis_chunks[0].command_paths[phaseIndex(ImportPhase::Structure)]);
    assert(single_axis_reader.valid());
    assert(single_axis_reader.commandCount() == 2);
    std::set<uint32_t> single_axis_sizes;
    while (const auto command = single_axis_reader.next()) {
        assert(command->name == "minecraft:wool" && command->aux == 5);
        assert(command->bounds.min_y == 64 && command->bounds.max_y == 64);
        assert(command->bounds.min_z == command->bounds.max_z);
        single_axis_sizes.insert(command->block_count);
    }
    assert(single_axis_reader.exhausted());
    assert(single_axis_sizes == std::set<uint32_t>({1, 3}));

    const std::filesystem::path layered_gravity_directory =
        directory / "layered_gravity";
    std::filesystem::create_directories(layered_gravity_directory);
    const std::filesystem::path layered_gravity_raw =
        layered_gravity_directory / "layered_gravity.bsp";
    {
        std::ofstream raw(layered_gravity_raw, std::ios::binary | std::ios::trunc);
        for (int32_t y = 64; y <= 65; ++y) {
            for (int32_t x = 0; x <= 1; ++x) {
                appendRaw(raw, x, y, 0, "minecraft:sand", 0x07);
            }
        }
        assert(raw);
    }
    ChunkDescriptor layered_gravity;
    layered_gravity.coord = {0, 0};
    layered_gravity.imported_bounds = {0, 64, 0, 1, 65, 0};
    layered_gravity.has_phase[phaseIndex(ImportPhase::Gravity)] = true;
    layered_gravity.spool_paths[phaseIndex(ImportPhase::Gravity)] =
        layered_gravity_raw.string();
    std::vector<ChunkDescriptor> layered_gravity_chunks{layered_gravity};
    assert(CommandSpoolBuilder::build(
        layered_gravity_directory.string(), 50000,
        OverwritePolicy::PreserveExisting, &layered_gravity_chunks, &error));
    CommandSpoolReader layered_gravity_reader(
        layered_gravity_chunks[0].command_paths[phaseIndex(ImportPhase::Gravity)]);
    assert(layered_gravity_reader.valid());
    assert(layered_gravity_reader.commandCount() == 2);
    std::set<int32_t> gravity_layers;
    while (const auto command = layered_gravity_reader.next()) {
        assert(command->bounds.min_y == command->bounds.max_y);
        assert(command->block_count == 2);
        gravity_layers.insert(command->bounds.min_y);
    }
    assert(layered_gravity_reader.exhausted());
    assert(gravity_layers == std::set<int32_t>({64, 65}));

    assertBitmapMaterialAndAuxIsolation(directory);
    assertNegativeCoordinateBitmapCover(directory);
    assertSparseIndexFallbackCover(directory);
    assertSixAxisCoverDoesNotRegress(directory);
    assertDuplicatePositionsRejected(directory);

    // Dense cuboids must remain an exact cover at every scheduler merge cap.
    // The X lengths deliberately leave one non-divisible 3D tail command.
    assertRectangularCoverCase(directory, "rectangle_limit_64",
                               20, 64, 17, 2, 2);
    assertRectangularCoverCase(directory, "rectangle_limit_1250",
                               625, 1250, 76, 2, 5);
    assertRectangularCoverCase(directory, "rectangle_limit_12500",
                               6250, 12500, 129, 4, 8);
    assertRectangularCoverCase(directory, "rectangle_fill_hard_limit",
                               kMaximumBlocksPerSecond,
                               kMaximumFillBlockCount, 32769, 1, 1);

    const std::array<uint32_t, 4> property_seeds{{
        0x00000001u, 0x6D2B79F5u, 0xC001D00Du, 0xFFFFFFFFu,
    }};
    const std::array<std::pair<int32_t, uint32_t>, 3> property_rates{{
        {20, 64}, {625, 1250}, {6250, 12500},
    }};
    for (const uint32_t seed : property_seeds) {
        for (const auto& rate : property_rates) {
            assertMixedFlagPropertyCase(directory, seed, rate.first, rate.second);
        }
    }

    struct RandomPaletteBlock {
        const char* name;
        uint8_t aux;
    };
    uint32_t random_state = 0x6D2B79F5u;
    const auto nextRandom = [&]() {
        random_state = random_state * 1664525u + 1013904223u;
        return random_state;
    };

    const std::filesystem::path random_structure_directory =
        directory / "random_structure_cover";
    std::filesystem::create_directories(random_structure_directory);
    const std::filesystem::path random_structure_raw =
        random_structure_directory / "random_structure.bsp";
    ExpectedBlockMap random_structure_expected;
    const std::array<RandomPaletteBlock, 3> structure_palette{{
        {"minecraft:stone", 0},
        {"minecraft:wool", 14},
        {"minecraft:stained_hardened_clay", 3},
    }};
    {
        std::ofstream raw(random_structure_raw, std::ios::binary | std::ios::trunc);
        for (int32_t y = 64; y < 68; ++y) {
            for (int32_t z = 0; z < 7; ++z) {
                for (int32_t x = 0; x < 8; ++x) {
                    if (nextRandom() % 5 == 0) continue;
                    const RandomPaletteBlock& block =
                        structure_palette[nextRandom() % structure_palette.size()];
                    appendRaw(raw, x, y, z, block.name, 0x05, block.aux);
                    random_structure_expected[{x, y, z}] = {block.name, block.aux, 0x05};
                }
            }
        }
        assert(raw);
    }
    assert(!random_structure_expected.empty());
    ChunkDescriptor random_structure;
    random_structure.coord = {0, 0};
    random_structure.imported_bounds = {0, 64, 0, 7, 67, 6};
    random_structure.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    random_structure.spool_paths[phaseIndex(ImportPhase::Structure)] =
        random_structure_raw.string();
    std::vector<ChunkDescriptor> random_structure_chunks{random_structure};
    assert(CommandSpoolBuilder::build(
        random_structure_directory.string(), 50000,
        OverwritePolicy::PreserveExisting, &random_structure_chunks, &error));
    assertExactCommandCover(
        random_structure_chunks[0].command_paths[phaseIndex(ImportPhase::Structure)],
        random_structure_expected, fillBlockLimitForRate(50000),
        ImportPhase::Structure);

    const std::filesystem::path random_gravity_directory =
        directory / "random_gravity_cover";
    std::filesystem::create_directories(random_gravity_directory);
    const std::filesystem::path random_gravity_raw =
        random_gravity_directory / "random_gravity.bsp";
    ExpectedBlockMap random_gravity_expected;
    const std::array<RandomPaletteBlock, 3> gravity_palette{{
        {"minecraft:sand", 0},
        {"minecraft:gravel", 0},
        {"minecraft:concrete_powder", 5},
    }};
    {
        std::ofstream raw(random_gravity_raw, std::ios::binary | std::ios::trunc);
        for (int32_t y = 70; y < 73; ++y) {
            for (int32_t z = 0; z < 6; ++z) {
                for (int32_t x = 0; x < 7; ++x) {
                    if (nextRandom() % 4 == 0) continue;
                    const RandomPaletteBlock& block =
                        gravity_palette[nextRandom() % gravity_palette.size()];
                    appendRaw(raw, x, y, z, block.name, 0x07, block.aux);
                    random_gravity_expected[{x, y, z}] = {block.name, block.aux, 0x07};
                }
            }
        }
        assert(raw);
    }
    assert(!random_gravity_expected.empty());
    ChunkDescriptor random_gravity;
    random_gravity.coord = {0, 0};
    random_gravity.imported_bounds = {0, 70, 0, 6, 72, 5};
    random_gravity.has_phase[phaseIndex(ImportPhase::Gravity)] = true;
    random_gravity.spool_paths[phaseIndex(ImportPhase::Gravity)] =
        random_gravity_raw.string();
    std::vector<ChunkDescriptor> random_gravity_chunks{random_gravity};
    assert(CommandSpoolBuilder::build(
        random_gravity_directory.string(), 50000,
        OverwritePolicy::PreserveExisting, &random_gravity_chunks, &error));
    assertExactCommandCover(
        random_gravity_chunks[0].command_paths[phaseIndex(ImportPhase::Gravity)],
        random_gravity_expected, fillBlockLimitForRate(50000),
        ImportPhase::Gravity);

    const std::filesystem::path high_rate_directory = directory / "high_rate";
    std::filesystem::create_directories(high_rate_directory);
    const std::filesystem::path high_rate_raw = high_rate_directory / "high_rate.bsp";
    {
        std::ofstream raw(high_rate_raw, std::ios::binary | std::ios::trunc);
        for (int32_t y = 0; y < 50; ++y) {
            for (int32_t z = 0; z < 30; ++z) {
                for (int32_t x = 0; x < 17; ++x) {
                    appendRaw(raw, x, y, z, "minecraft:stone");
                }
            }
        }
        assert(raw);
    }
    ChunkDescriptor high_rate;
    high_rate.coord = {0, 0};
    high_rate.imported_bounds = {0, 0, 0, 16, 49, 29};
    high_rate.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    high_rate.spool_paths[phaseIndex(ImportPhase::Structure)] = high_rate_raw.string();
    std::vector<ChunkDescriptor> high_rate_chunks{high_rate};
    assert(CommandSpoolBuilder::build(
        high_rate_directory.string(), 50000,
        OverwritePolicy::PreserveExisting, &high_rate_chunks, &error));
    CommandSpoolReader high_rate_reader(
        high_rate_chunks[0].command_paths[phaseIndex(ImportPhase::Structure)]);
    assert(high_rate_reader.valid());
    assert(high_rate_reader.commandCount() < 13);
    uint64_t high_rate_blocks = 0;
    uint32_t high_rate_largest_command = 0;
    while (const auto command = high_rate_reader.next()) {
        assert(command->block_count <= fillBlockLimitForRate(50000));
        high_rate_blocks += command->block_count;
        high_rate_largest_command =
            std::max(high_rate_largest_command, command->block_count);
    }
    assert(high_rate_reader.exhausted());
    assert(high_rate_blocks == 25500);
    // Static structure fills intentionally use a larger scheduler cap than
    // update-heavy gravity and fluid phases to reduce RPC volume.
    assert(high_rate_largest_command > 2048 &&
           high_rate_largest_command <= fillBlockLimitForRate(50000));

    const std::filesystem::path holed_directory = directory / "holed_3d";
    std::filesystem::create_directories(holed_directory);
    const std::filesystem::path holed_raw = holed_directory / "holed_3d.bsp";
    {
        std::ofstream raw(holed_raw, std::ios::binary | std::ios::trunc);
        for (int32_t y = 64; y <= 66; ++y) {
            for (int32_t z = 0; z <= 2; ++z) {
                for (int32_t x = 0; x <= 2; ++x) {
                    if (x == 1 && y == 65 && z == 1) continue;
                    appendRaw(raw, x, y, z, "minecraft:stone");
                }
            }
        }
        assert(raw);
    }
    ChunkDescriptor holed;
    holed.coord = {0, 0};
    holed.imported_bounds = {0, 64, 0, 2, 66, 2};
    holed.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    holed.spool_paths[phaseIndex(ImportPhase::Structure)] = holed_raw.string();
    std::vector<ChunkDescriptor> holed_chunks{holed};
    assert(CommandSpoolBuilder::build(holed_directory.string(), 5000,
                                      OverwritePolicy::PreserveExisting,
                                      &holed_chunks, &error));
    CommandSpoolReader holed_reader(
        holed_chunks[0].command_paths[phaseIndex(ImportPhase::Structure)]);
    assert(holed_reader.valid());
    assert(holed_reader.commandCount() > 1 && holed_reader.commandCount() < 26);
    uint64_t holed_blocks = 0;
    std::set<std::array<int32_t, 3>> holed_positions;
    while (const auto command = holed_reader.next()) {
        const uint64_t width = static_cast<uint64_t>(
            static_cast<int64_t>(command->bounds.max_x) - command->bounds.min_x + 1);
        const uint64_t height = static_cast<uint64_t>(
            static_cast<int64_t>(command->bounds.max_y) - command->bounds.min_y + 1);
        const uint64_t depth = static_cast<uint64_t>(
            static_cast<int64_t>(command->bounds.max_z) - command->bounds.min_z + 1);
        assert(command->block_count == width * height * depth);
        holed_blocks += command->block_count;
        for (int32_t y = command->bounds.min_y; y <= command->bounds.max_y; ++y) {
            for (int32_t z = command->bounds.min_z; z <= command->bounds.max_z; ++z) {
                for (int32_t x = command->bounds.min_x; x <= command->bounds.max_x; ++x) {
                    assert(!(x == 1 && y == 65 && z == 1));
                    assert(holed_positions.insert({x, y, z}).second);
                }
            }
        }
    }
    assert(!holed_reader.failed() && holed_reader.exhausted());
    assert(holed_blocks == 26 && holed_positions.size() == 26);
    for (int32_t y = 64; y <= 66; ++y) {
        for (int32_t z = 0; z <= 2; ++z) {
            for (int32_t x = 0; x <= 2; ++x) {
                const bool is_hole = x == 1 && y == 65 && z == 1;
                assert(holed_positions.count({x, y, z}) == (is_hole ? 0 : 1));
            }
        }
    }

    const std::filesystem::path bed_directory = directory / "bed";
    std::filesystem::create_directories(bed_directory);
    const std::filesystem::path bed_raw = bed_directory / "bed.bsp";
    {
        std::ofstream raw(bed_raw, std::ios::binary | std::ios::trunc);
        // Coordinate order would put the head first without dependency sorting.
        appendRaw(raw, 0, 64, 0, "minecraft:bed", 0x06, 8);
        appendRaw(raw, 1, 64, 0, "minecraft:bed", 0x06, 0);
    }
    ChunkDescriptor bed;
    bed.coord = {0, 0};
    bed.imported_bounds = {0, 64, 0, 1, 64, 0};
    bed.has_phase[phaseIndex(ImportPhase::Attachment)] = true;
    bed.spool_paths[phaseIndex(ImportPhase::Attachment)] = bed_raw.string();
    std::vector<ChunkDescriptor> bed_chunks{bed};
    assert(CommandSpoolBuilder::build(bed_directory.string(), 20,
                                      OverwritePolicy::PreserveExisting,
                                      &bed_chunks, &error));
    CommandSpoolReader bed_reader(
        bed_chunks[0].command_paths[phaseIndex(ImportPhase::Attachment)]);
    const auto foot = bed_reader.next();
    const auto head = bed_reader.next();
    assert(foot.has_value() && head.has_value());
    assert(foot->aux == 0 && head->aux == 8);
    assert(bed_reader.exhausted());

    std::vector<VerificationChunkPlan> bed_plans;
    assert(CommandSpoolBuilder::loadVerificationPlan(
        (bed_directory / CommandSpoolBuilder::kVerificationPlanName).string(),
        &bed_plans, &error));
    assert(bed_plans.size() == 1);
    assert(bed_plans[0].coord == bed.coord);
    assert(bed_plans[0].total_block_count == 2);
    assert(bed_plans[0].eligible_count == 0);
    assert(bed_plans[0].samples.empty());

    const std::filesystem::path attachment_fill_directory =
        directory / "attachment_fill_whitelist";
    std::filesystem::create_directories(attachment_fill_directory);
    std::vector<ChunkDescriptor> attachment_fill_chunks;
    {
        ChunkSpoolWriter writer(attachment_fill_directory.string(), 32);
        std::string writer_error;
        // Write the upper row first to exercise the raw-order fallback. The
        // planner must restore y order and keep every /fill one layer tall.
        for (int32_t y : {65, 64}) {
            for (int32_t x = 0; x < 8; ++x) {
                assert(writer.append(
                    {x, y, 0,
                     {"minecraft:yellow_flower", 0, ImportPhase::Attachment,
                      false, true, false}},
                    &writer_error));
            }
        }
        for (int32_t x = 8; x < 10; ++x) {
            assert(writer.append(
                {x, 64, 0,
                 {"minecraft:torch", 0, ImportPhase::Attachment,
                  false, true, false}},
                &writer_error));
        }
        for (int32_t x = 10; x < 12; ++x) {
            assert(writer.append(
                {x, 64, 0,
                 {"minecraft:red_flower", 0, ImportPhase::Attachment,
                  false, true, true}},
                &writer_error));
        }
        attachment_fill_chunks = writer.finish(&writer_error);
        assert(writer_error.empty() && attachment_fill_chunks.size() == 1);
    }
    assert(CommandSpoolBuilder::build(
        attachment_fill_directory.string(), 5000,
        OverwritePolicy::PreserveExisting, &attachment_fill_chunks, &error));
    CommandSpoolReader attachment_fill_reader(
        attachment_fill_chunks[0].command_paths[
            phaseIndex(ImportPhase::Attachment)]);
    assert(attachment_fill_reader.valid());
    size_t flower_fills = 0;
    size_t protected_singletons = 0;
    int32_t previous_y = std::numeric_limits<int32_t>::min();
    while (const std::optional<PlannedCommand> command =
               attachment_fill_reader.next()) {
        assert(command->bounds.min_y >= previous_y);
        previous_y = command->bounds.min_y;
        assert(command->bounds.min_y == command->bounds.max_y);
        assert(hasPlannedCommandFlag(
            *command, PlannedCommandFlag::SingleLayer));
        if (command->name == "minecraft:yellow_flower") {
            assert(command->block_count == 8);
            assert(hasPlannedCommandFlag(
                *command, PlannedCommandFlag::Fillable));
            ++flower_fills;
        } else {
            assert((command->name == "minecraft:torch" ||
                    command->name == "minecraft:red_flower") &&
                   command->block_count == 1);
            assert(!hasPlannedCommandFlag(
                *command, PlannedCommandFlag::Fillable));
            ++protected_singletons;
        }
    }
    assert(!attachment_fill_reader.failed() &&
           attachment_fill_reader.exhausted());
    assert(flower_fills == 2 && protected_singletons == 4);

    const std::filesystem::path dependent_directory =
        directory / "dependent_attachment";
    std::filesystem::create_directories(dependent_directory);
    const std::filesystem::path dependent_raw =
        dependent_directory / "dependent.bsp";
    {
        std::ofstream raw(dependent_raw, std::ios::binary | std::ios::trunc);
        // Legacy raw spools may not carry the v2 flag marker. The phase still
        // makes dependent attachment blocks ineligible for /fill merging.
        appendRaw(raw, 0, 65, 0, "minecraft:bed", 0, 8);
        appendRaw(raw, 1, 65, 0, "minecraft:bed", 0, 8);
        assert(raw);
    }
    ChunkDescriptor dependent;
    dependent.coord = {0, 0};
    dependent.imported_bounds = {0, 65, 0, 1, 65, 0};
    dependent.has_phase[phaseIndex(ImportPhase::DependentAttachment)] = true;
    dependent.spool_paths[phaseIndex(ImportPhase::DependentAttachment)] =
        dependent_raw.string();
    std::vector<ChunkDescriptor> dependent_chunks{dependent};
    assert(CommandSpoolBuilder::build(
        dependent_directory.string(), 5000, OverwritePolicy::PreserveExisting,
        &dependent_chunks, &error));
    CommandSpoolReader dependent_reader(
        dependent_chunks[0].command_paths[
            phaseIndex(ImportPhase::DependentAttachment)]);
    assert(dependent_reader.valid());
    assert(dependent_reader.commandCount() == 2);
    assert(dependent_reader.next().has_value());
    assert(dependent_reader.next().has_value());
    assert(dependent_reader.exhausted());

    const std::filesystem::path reserved_directory = directory / "attachment_reservation";
    std::filesystem::create_directories(reserved_directory);
    const std::filesystem::path reserved_structure_raw =
        reserved_directory / "structure.bsp";
    const std::filesystem::path reserved_attachment_raw =
        reserved_directory / "attachment.bsp";
    const std::filesystem::path reserved_fluid_raw =
        reserved_directory / "fluid.bsp";
    {
        std::ofstream raw(reserved_structure_raw, std::ios::binary | std::ios::trunc);
        for (int32_t x = 0; x < 20; ++x) {
            appendRaw(raw, x, 64, 0, "minecraft:stone");
        }
        assert(raw);
    }
    {
        std::ofstream raw(reserved_attachment_raw, std::ios::binary | std::ios::trunc);
        for (int32_t x = 20; x < 26; ++x) {
            appendRaw(raw, x, 64, 0, "minecraft:torch", 0x06,
                      static_cast<uint8_t>(x - 20));
        }
        assert(raw);
    }
    {
        std::ofstream raw(reserved_fluid_raw, std::ios::binary | std::ios::trunc);
        appendRaw(raw, 26, 64, 0, "minecraft:water", 0x07, 0);
        appendRaw(raw, 27, 64, 0, "minecraft:water", 0x07, 0);
        for (int32_t x = 28; x < 32; ++x) {
            appendRaw(raw, x, 64, 0, "minecraft:flowing_water", 0x07,
                      static_cast<uint8_t>(x - 25));
        }
        assert(raw);
    }
    ChunkDescriptor reserved;
    reserved.coord = {0, 0};
    reserved.imported_bounds = {0, 64, 0, 31, 64, 0};
    reserved.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    reserved.spool_paths[phaseIndex(ImportPhase::Structure)] =
        reserved_structure_raw.string();
    reserved.has_phase[phaseIndex(ImportPhase::Attachment)] = true;
    reserved.spool_paths[phaseIndex(ImportPhase::Attachment)] =
        reserved_attachment_raw.string();
    reserved.has_phase[phaseIndex(ImportPhase::Fluid)] = true;
    reserved.spool_paths[phaseIndex(ImportPhase::Fluid)] = reserved_fluid_raw.string();
    std::vector<ChunkDescriptor> reserved_chunks{reserved};
    assert(CommandSpoolBuilder::build(reserved_directory.string(), 5000,
                                      OverwritePolicy::PreserveExisting,
                                      &reserved_chunks, &error));
    std::vector<VerificationChunkPlan> reserved_plans;
    assert(CommandSpoolBuilder::loadVerificationPlan(
        (reserved_directory / CommandSpoolBuilder::kVerificationPlanName).string(),
        &reserved_plans, &error));
    assert(reserved_plans.size() == 1);
    assert(reserved_plans[0].total_block_count == 32);
    assert(reserved_plans[0].eligible_count == 26);
    assert(reserved_plans[0].samples.size() == 16);
    size_t reserved_structure_samples = 0;
    size_t reserved_source_samples = 0;
    size_t reserved_flowing_samples = 0;
    for (const VerificationPlanSample& sample : reserved_plans[0].samples) {
        assert(!hasVerificationSampleFlag(sample, VerificationSampleFlag::ExpectedAir));
        if (sample.name == "minecraft:stone") {
            ++reserved_structure_samples;
            assert(!hasVerificationSampleFlag(sample, VerificationSampleFlag::IgnoreAux));
        } else if (sample.name == "minecraft:water") {
            ++reserved_source_samples;
            assert(hasVerificationSampleFlag(sample, VerificationSampleFlag::IgnoreAux));
        } else {
            ++reserved_flowing_samples;
            assert(sample.name == "minecraft:flowing_water");
            assert(hasVerificationSampleFlag(sample, VerificationSampleFlag::IgnoreAux));
        }
    }
    assert(reserved_structure_samples == 12);
    assert(reserved_source_samples == 2);
    assert(reserved_flowing_samples == 2);

    const std::filesystem::path region_stitch_directory =
        directory / "region_stitch";
    std::filesystem::create_directories(region_stitch_directory);
    std::vector<ChunkDescriptor> region_stitch_chunks;
    ExpectedBlockMap region_stitch_expected;
    std::vector<std::filesystem::path> region_stitch_chunk_commands;
    for (int32_t chunk_x = 0; chunk_x < 3; ++chunk_x) {
        const std::filesystem::path raw_path = region_stitch_directory /
            ("chunk_" + std::to_string(chunk_x) + "_0_phase_1.bsp");
        {
            std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
            for (int32_t z = 0; z < 32; ++z) {
                for (int32_t x = chunk_x * 32; x < (chunk_x + 1) * 32; ++x) {
                    appendRaw(raw, x, 64, z, "minecraft:stone", 0x05);
                    region_stitch_expected[{x, 64, z}] =
                        {"minecraft:stone", 0, 0x05};
                }
            }
            assert(raw);
        }
        ChunkDescriptor chunk;
        chunk.coord = {chunk_x, 0};
        chunk.imported_bounds = {chunk_x * 32, 64, 0,
                                 (chunk_x + 1) * 32 - 1, 64, 31};
        chunk.has_phase[phaseIndex(ImportPhase::Structure)] = true;
        chunk.spool_paths[phaseIndex(ImportPhase::Structure)] = raw_path.string();
        region_stitch_chunks.push_back(std::move(chunk));
        region_stitch_chunk_commands.push_back(raw_path.string() + ".cmd");
    }
    std::reverse(region_stitch_chunks.begin(), region_stitch_chunks.end());
    std::vector<CommandSpoolBuildProgress> region_stitch_progress;
    assert(CommandSpoolBuilder::build(
        region_stitch_directory.string(), 50000,
        OverwritePolicy::PreserveExisting, 3, &region_stitch_chunks, &error,
        {}, false, [&](const CommandSpoolBuildProgress& progress) {
            region_stitch_progress.push_back(progress);
        }));
    assert(!region_stitch_progress.empty());
    assert(region_stitch_progress.front().stage ==
           CommandSpoolBuildStage::OptimizingChunks);
    assert(region_stitch_progress.front().completed == 0);
    assert(region_stitch_progress.front().total == 3);
    assert(std::any_of(region_stitch_progress.begin(), region_stitch_progress.end(),
                       [](const CommandSpoolBuildProgress& progress) {
                           return progress.stage ==
                                      CommandSpoolBuildStage::WritingVerificationPlan &&
                                  progress.completed == progress.total;
                       }));
    assert(region_stitch_progress.back().stage ==
           CommandSpoolBuildStage::AggregatingRegions);
    assert(region_stitch_progress.back().completed == 1);
    assert(region_stitch_progress.back().total == 1);
    assert(region_stitch_chunks.size() == 1);
    assert(region_stitch_chunks[0].coord == ChunkCoord({0, 0}));
    assert(region_stitch_chunks[0].imported_bounds.min_x == 0);
    assert(region_stitch_chunks[0].imported_bounds.max_x == 95);
    assert(region_stitch_chunks[0].phase_block_counts[
               phaseIndex(ImportPhase::Structure)] == 3072);
    const std::filesystem::path stitched_path = region_stitch_directory /
        "region_0_0_phase_1.bsp.cmd";
    assert(std::filesystem::path(region_stitch_chunks[0].command_paths[
               phaseIndex(ImportPhase::Structure)]).filename() ==
           stitched_path.filename());
    const std::vector<uint32_t> stitched_sizes = assertExactCommandCover(
        stitched_path.string(), region_stitch_expected,
        fillBlockLimitForRate(50000),
        ImportPhase::Structure);
    assert(stitched_sizes == std::vector<uint32_t>({3072}));
    for (const std::filesystem::path& chunk_command : region_stitch_chunk_commands) {
        assert(!std::filesystem::exists(chunk_command));
    }
    std::vector<VerificationChunkPlan> region_stitch_plans;
    assert(CommandSpoolBuilder::loadVerificationPlan(
        (region_stitch_directory /
         CommandSpoolBuilder::kVerificationPlanName).string(),
        &region_stitch_plans, &error));
    assert(region_stitch_plans.size() == 3);
    std::set<ChunkCoord> region_stitch_plan_coords;
    for (const VerificationChunkPlan& plan : region_stitch_plans) {
        assert(region_stitch_plan_coords.insert(plan.coord).second);
    }
    assert(region_stitch_plan_coords ==
           std::set<ChunkCoord>({{0, 0}, {1, 0}, {2, 0}}));

    const std::filesystem::path region_singleton_directory =
        directory / "region_singletons";
    std::filesystem::create_directories(region_singleton_directory);
    std::vector<ChunkDescriptor> region_singleton_chunks;
    ExpectedBlockMap region_singleton_expected;
    for (int32_t chunk_x = 0; chunk_x < 2; ++chunk_x) {
        const int32_t x = 31 + chunk_x;
        const std::filesystem::path raw_path = region_singleton_directory /
            ("chunk_" + std::to_string(chunk_x) + "_0_phase_1.bsp");
        {
            std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
            appendRaw(raw, x, 70, 0, "minecraft:stone", 0x08);
            assert(raw);
        }
        region_singleton_expected[{x, 70, 0}] =
            {"minecraft:stone", 0, 0x08};
        ChunkDescriptor chunk;
        chunk.coord = {chunk_x, 0};
        chunk.imported_bounds = {x, 70, 0, x, 70, 0};
        chunk.has_phase[phaseIndex(ImportPhase::Structure)] = true;
        chunk.spool_paths[phaseIndex(ImportPhase::Structure)] = raw_path.string();
        region_singleton_chunks.push_back(std::move(chunk));
    }
    assert(CommandSpoolBuilder::build(
        region_singleton_directory.string(), 50000,
        OverwritePolicy::PreserveExisting, 2, &region_singleton_chunks, &error));
    assert(region_singleton_chunks.size() == 1);
    const std::vector<uint32_t> singleton_sizes = assertExactCommandCover(
        region_singleton_chunks[0].command_paths[
            phaseIndex(ImportPhase::Structure)],
        region_singleton_expected, 2048, ImportPhase::Structure);
    assert(singleton_sizes == std::vector<uint32_t>({1, 1}));

    const std::filesystem::path fillable_singleton_directory =
        directory / "region_fillable_singletons";
    std::filesystem::create_directories(fillable_singleton_directory);
    std::vector<ChunkDescriptor> fillable_singleton_chunks;
    ExpectedBlockMap fillable_singleton_expected;
    for (int32_t chunk_x = 0; chunk_x < 2; ++chunk_x) {
        const int32_t x = 31 + chunk_x;
        const std::filesystem::path raw_path = fillable_singleton_directory /
            ("chunk_" + std::to_string(chunk_x) + "_0_phase_1.bsp");
        {
            std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
            appendRaw(raw, x, 70, 0, "minecraft:stone", 0x05);
            assert(raw);
        }
        fillable_singleton_expected[{x, 70, 0}] =
            {"minecraft:stone", 0, 0x05};
        ChunkDescriptor chunk;
        chunk.coord = {chunk_x, 0};
        chunk.imported_bounds = {x, 70, 0, x, 70, 0};
        chunk.has_phase[phaseIndex(ImportPhase::Structure)] = true;
        chunk.spool_paths[phaseIndex(ImportPhase::Structure)] = raw_path.string();
        fillable_singleton_chunks.push_back(std::move(chunk));
    }
    assert(CommandSpoolBuilder::build(
        fillable_singleton_directory.string(), 50000,
        OverwritePolicy::PreserveExisting, 2, &fillable_singleton_chunks, &error));
    assert(fillable_singleton_chunks.size() == 1);
    assert(assertExactCommandCover(
               fillable_singleton_chunks[0].command_paths[
                   phaseIndex(ImportPhase::Structure)],
               fillable_singleton_expected, 2048, ImportPhase::Structure) ==
           std::vector<uint32_t>({2}));

    const std::filesystem::path region_remerge_directory =
        directory / "region_exact_remerge";
    std::filesystem::create_directories(region_remerge_directory);
    std::vector<ChunkDescriptor> region_remerge_chunks;
    ExpectedBlockMap region_remerge_expected;
    for (int32_t chunk_x = 0; chunk_x < 2; ++chunk_x) {
        const std::filesystem::path raw_path = region_remerge_directory /
            ("chunk_" + std::to_string(chunk_x) + "_0_phase_1.bsp");
        std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
        if (chunk_x == 0) {
            appendRaw(raw, 31, 64, 0, "minecraft:wool", 0x05, 3);
            appendRaw(raw, 30, 64, 1, "minecraft:wool", 0x05, 3);
            appendRaw(raw, 31, 64, 1, "minecraft:wool", 0x05, 3);
        } else {
            appendRaw(raw, 32, 64, 0, "minecraft:wool", 0x05, 3);
            appendRaw(raw, 33, 64, 0, "minecraft:wool", 0x05, 3);
            appendRaw(raw, 32, 64, 1, "minecraft:wool", 0x05, 3);
        }
        assert(raw);
        ChunkDescriptor chunk;
        chunk.coord = {chunk_x, 0};
        chunk.imported_bounds = {chunk_x == 0 ? 30 : 32, 64, 0,
                                 chunk_x == 0 ? 31 : 33, 64, 1};
        chunk.has_phase[phaseIndex(ImportPhase::Structure)] = true;
        chunk.spool_paths[phaseIndex(ImportPhase::Structure)] = raw_path.string();
        region_remerge_chunks.push_back(std::move(chunk));
    }
    for (int32_t x = 31; x <= 33; ++x) {
        region_remerge_expected[{x, 64, 0}] =
            {"minecraft:wool", 3, 0x05};
    }
    for (int32_t x = 30; x <= 32; ++x) {
        region_remerge_expected[{x, 64, 1}] =
            {"minecraft:wool", 3, 0x05};
    }
    assert(CommandSpoolBuilder::build(
        region_remerge_directory.string(), 20,
        OverwritePolicy::PreserveExisting, 2, &region_remerge_chunks, &error));
    assert(region_remerge_chunks.size() == 1);
    const std::vector<uint32_t> remerged_sizes = assertExactCommandCover(
        region_remerge_chunks[0].command_paths[phaseIndex(ImportPhase::Structure)],
        region_remerge_expected, fillBlockLimitForRate(20), ImportPhase::Structure);
    assert(remerged_sizes.size() == 2);
    assert(std::count(remerged_sizes.begin(), remerged_sizes.end(), 3) == 2);

    const std::filesystem::path region_no_gain_directory =
        directory / "region_remerge_no_gain";
    std::filesystem::create_directories(region_no_gain_directory);
    std::vector<ChunkDescriptor> region_no_gain_chunks;
    ExpectedBlockMap region_no_gain_expected;
    for (int32_t chunk_x = 0; chunk_x < 2; ++chunk_x) {
        const int32_t min_x = chunk_x == 0 ? 28 : 32;
        const int32_t max_x = chunk_x == 0 ? 31 : 35;
        const std::filesystem::path raw_path = region_no_gain_directory /
            ("chunk_" + std::to_string(chunk_x) + "_0_phase_1.bsp");
        std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
        for (int32_t z = 0; z < 8; ++z) {
            for (int32_t x = min_x; x <= max_x; ++x) {
                const bool stone = ((x + z) & 1) == 0;
                const char* name = stone ? "minecraft:stone" : "minecraft:wool";
                const uint8_t aux = stone ? 0 : 14;
                appendRaw(raw, x, 72, z, name, 0x05, aux);
                region_no_gain_expected[{x, 72, z}] = {name, aux, 0x05};
            }
        }
        assert(raw);
        ChunkDescriptor chunk;
        chunk.coord = {chunk_x, 0};
        chunk.imported_bounds = {min_x, 72, 0, max_x, 72, 7};
        chunk.has_phase[phaseIndex(ImportPhase::Structure)] = true;
        chunk.spool_paths[phaseIndex(ImportPhase::Structure)] = raw_path.string();
        region_no_gain_chunks.push_back(std::move(chunk));
    }
    assert(CommandSpoolBuilder::build(
        region_no_gain_directory.string(), 50000,
        OverwritePolicy::PreserveExisting, 2, &region_no_gain_chunks, &error));
    assert(region_no_gain_chunks.size() == 1);
    const std::vector<uint32_t> no_gain_sizes = assertExactCommandCover(
        region_no_gain_chunks[0].command_paths[phaseIndex(ImportPhase::Structure)],
        region_no_gain_expected, fillBlockLimitForRate(50000),
        ImportPhase::Structure);
    assert(no_gain_sizes.size() == region_no_gain_expected.size());
    assert(std::all_of(no_gain_sizes.begin(), no_gain_sizes.end(),
                       [](uint32_t count) { return count == 1; }));

    const std::filesystem::path region_remerge_limit_directory =
        directory / "region_remerge_3d_limit";
    std::filesystem::create_directories(region_remerge_limit_directory);
    std::vector<ChunkDescriptor> region_remerge_limit_chunks;
    ExpectedBlockMap region_remerge_limit_expected;
    constexpr int32_t kRemergeLimitHeight = 5462;
    static_assert(uint64_t{kRemergeLimitHeight} * 6 > 32768,
                  "threshold case must exceed the 3D remerge limit");
    for (int32_t chunk_x = 0; chunk_x < 2; ++chunk_x) {
        const std::filesystem::path raw_path = region_remerge_limit_directory /
            ("chunk_" + std::to_string(chunk_x) + "_0_phase_1.bsp");
        std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
        for (int32_t y = 0; y < kRemergeLimitHeight; ++y) {
            if (chunk_x == 0) {
                appendRaw(raw, 31, y, 0, "minecraft:stone", 0x05);
                appendRaw(raw, 30, y, 1, "minecraft:stone", 0x05);
                appendRaw(raw, 31, y, 1, "minecraft:stone", 0x05);
            } else {
                appendRaw(raw, 32, y, 0, "minecraft:stone", 0x05);
                appendRaw(raw, 33, y, 0, "minecraft:stone", 0x05);
                appendRaw(raw, 32, y, 1, "minecraft:stone", 0x05);
            }
        }
        assert(raw);
        ChunkDescriptor chunk;
        chunk.coord = {chunk_x, 0};
        chunk.imported_bounds = {chunk_x == 0 ? 30 : 32, 0, 0,
                                 chunk_x == 0 ? 31 : 33,
                                 kRemergeLimitHeight - 1, 1};
        chunk.has_phase[phaseIndex(ImportPhase::Structure)] = true;
        chunk.spool_paths[phaseIndex(ImportPhase::Structure)] = raw_path.string();
        region_remerge_limit_chunks.push_back(std::move(chunk));
    }
    for (int32_t y = 0; y < kRemergeLimitHeight; ++y) {
        for (int32_t x = 31; x <= 33; ++x) {
            region_remerge_limit_expected[{x, y, 0}] =
                {"minecraft:stone", 0, 0x05};
        }
        for (int32_t x = 30; x <= 32; ++x) {
            region_remerge_limit_expected[{x, y, 1}] =
                {"minecraft:stone", 0, 0x05};
        }
    }
    assert(CommandSpoolBuilder::build(
        region_remerge_limit_directory.string(), kMaximumBlocksPerSecond,
        OverwritePolicy::PreserveExisting, 2,
        &region_remerge_limit_chunks, &error));
    assert(region_remerge_limit_chunks.size() == 1);
    const std::vector<uint32_t> remerge_limit_sizes = assertExactCommandCover(
        region_remerge_limit_chunks[0].command_paths[
            phaseIndex(ImportPhase::Structure)],
        region_remerge_limit_expected,
        fillBlockLimitForRate(kMaximumBlocksPerSecond), ImportPhase::Structure);
    assert(remerge_limit_sizes.size() == 4);

    const std::filesystem::path aligned_region_directory =
        directory / "content_aligned_region_grid";
    std::filesystem::create_directories(aligned_region_directory);
    std::vector<ChunkDescriptor> aligned_region_chunks;
    ExpectedBlockMap aligned_region_expected;
    for (int32_t chunk_x = 10; chunk_x <= 12; ++chunk_x) {
        for (int32_t chunk_z = -7; chunk_z <= -5; ++chunk_z) {
            const int32_t min_x = chunk_x * 32;
            const int32_t min_z = chunk_z * 32;
            const std::filesystem::path raw_path = aligned_region_directory /
                ("chunk_" + std::to_string(chunk_x) + "_" +
                 std::to_string(chunk_z) + "_phase_1.bsp");
            std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
            for (int32_t z = min_z; z < min_z + 32; ++z) {
                for (int32_t x = min_x; x < min_x + 32; ++x) {
                    appendRaw(raw, x, 80, z, "minecraft:stone", 0x05);
                    aligned_region_expected[{x, 80, z}] =
                        {"minecraft:stone", 0, 0x05};
                }
            }
            assert(raw);
            ChunkDescriptor chunk;
            chunk.coord = {chunk_x, chunk_z};
            chunk.imported_bounds = {min_x, 80, min_z,
                                     min_x + 31, 80, min_z + 31};
            chunk.has_phase[phaseIndex(ImportPhase::Structure)] = true;
            chunk.spool_paths[phaseIndex(ImportPhase::Structure)] =
                raw_path.string();
            aligned_region_chunks.push_back(std::move(chunk));
        }
    }
    assert(CommandSpoolBuilder::build(
        aligned_region_directory.string(), 50000,
        OverwritePolicy::PreserveExisting, 3, &aligned_region_chunks, &error,
        {}, true));
    assert(aligned_region_chunks.size() == 1);
    assert(aligned_region_chunks[0].coord == ChunkCoord({10, -7}));
    for (const ChunkDescriptor& descriptor : aligned_region_chunks) {
        assert(descriptor.region_grid_origin == ChunkCoord({10, -7}));
    }
    assert(std::filesystem::path(aligned_region_chunks[0].command_paths[
               phaseIndex(ImportPhase::Structure)]).filename() ==
           "region_0_0_phase_1.bsp.cmd");
    assert(assertExactCommandCover(
               aligned_region_chunks[0].command_paths[
                   phaseIndex(ImportPhase::Structure)],
               aligned_region_expected, fillBlockLimitForRate(50000),
               ImportPhase::Structure) == std::vector<uint32_t>({9216}));

    const std::filesystem::path region_edges_directory =
        directory / "region_edges";
    std::filesystem::create_directories(region_edges_directory);
    std::vector<ChunkDescriptor> region_edge_chunks;
    ExpectedBlockMap negative_region_expected;
    ExpectedBlockMap tail_region_expected;
    for (int32_t chunk_x = -3; chunk_x <= -1; ++chunk_x) {
        const int32_t min_x = chunk_x * 32;
        const std::filesystem::path raw_path = region_edges_directory /
            ("chunk_" + std::to_string(chunk_x) + "_-1_phase_1.bsp");
        {
            std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
            for (int32_t x = min_x; x < min_x + 32; ++x) {
                appendRaw(raw, x, 64, -32, "minecraft:stone", 0x05);
                negative_region_expected[{x, 64, -32}] =
                    {"minecraft:stone", 0, 0x05};
            }
            assert(raw);
        }
        ChunkDescriptor chunk;
        chunk.coord = {chunk_x, -1};
        chunk.imported_bounds = {min_x, 64, -32, min_x + 31, 64, -32};
        chunk.has_phase[phaseIndex(ImportPhase::Structure)] = true;
        chunk.spool_paths[phaseIndex(ImportPhase::Structure)] = raw_path.string();
        region_edge_chunks.push_back(std::move(chunk));
    }
    // Only two of the three possible X chunks exist in this positive region.
    // Region aggregation must emit that incomplete tail instead of dropping it.
    for (int32_t chunk_x = 3; chunk_x <= 4; ++chunk_x) {
        const int32_t min_x = chunk_x * 32;
        const std::filesystem::path raw_path = region_edges_directory /
            ("chunk_" + std::to_string(chunk_x) + "_0_phase_1.bsp");
        {
            std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
            for (int32_t x = min_x; x < min_x + 32; ++x) {
                appendRaw(raw, x, 64, 0, "minecraft:wool", 0x05, 4);
                tail_region_expected[{x, 64, 0}] =
                    {"minecraft:wool", 4, 0x05};
            }
            assert(raw);
        }
        ChunkDescriptor chunk;
        chunk.coord = {chunk_x, 0};
        chunk.imported_bounds = {min_x, 64, 0, min_x + 31, 64, 0};
        chunk.has_phase[phaseIndex(ImportPhase::Structure)] = true;
        chunk.spool_paths[phaseIndex(ImportPhase::Structure)] = raw_path.string();
        region_edge_chunks.push_back(std::move(chunk));
    }
    std::reverse(region_edge_chunks.begin(), region_edge_chunks.end());
    assert(CommandSpoolBuilder::build(
        region_edges_directory.string(), 50000,
        OverwritePolicy::PreserveExisting, 3, &region_edge_chunks, &error));
    assert(region_edge_chunks.size() == 2);
    const ChunkDescriptor* negative_region = nullptr;
    const ChunkDescriptor* tail_region = nullptr;
    for (const ChunkDescriptor& region : region_edge_chunks) {
        const std::string filename = std::filesystem::path(
            region.command_paths[phaseIndex(ImportPhase::Structure)]).filename().string();
        if (filename == "region_-1_-1_phase_1.bsp.cmd") {
            negative_region = &region;
        } else if (filename == "region_1_0_phase_1.bsp.cmd") {
            tail_region = &region;
        } else {
            assert(false);
        }
    }
    assert(negative_region != nullptr && tail_region != nullptr);
    assert(negative_region->coord == ChunkCoord({-3, -1}));
    assert(negative_region->imported_bounds.min_x == -96);
    assert(negative_region->imported_bounds.max_x == -1);
    assert(negative_region->phase_block_counts[
               phaseIndex(ImportPhase::Structure)] == 96);
    assert(assertExactCommandCover(
               negative_region->command_paths[phaseIndex(ImportPhase::Structure)],
               negative_region_expected, 2048, ImportPhase::Structure) ==
           std::vector<uint32_t>({96}));
    assert(tail_region->coord == ChunkCoord({3, 0}));
    assert(tail_region->imported_bounds.min_x == 96);
    assert(tail_region->imported_bounds.max_x == 159);
    assert(tail_region->phase_block_counts[
               phaseIndex(ImportPhase::Structure)] == 64);
    assert(assertExactCommandCover(
               tail_region->command_paths[phaseIndex(ImportPhase::Structure)],
               tail_region_expected, 2048, ImportPhase::Structure) ==
           std::vector<uint32_t>({64}));

    const std::filesystem::path region_phases_directory =
        directory / "region_multiple_phases";
    std::filesystem::create_directories(region_phases_directory);
    std::vector<ChunkDescriptor> region_phase_chunks;
    ExpectedBlockMap region_structure_expected;
    ExpectedBlockMap region_gravity_expected;
    for (int32_t chunk_x = 0; chunk_x <= 1; ++chunk_x) {
        const int32_t min_x = chunk_x == 0 ? 30 : 32;
        const std::filesystem::path structure_raw = region_phases_directory /
            ("chunk_" + std::to_string(chunk_x) + "_0_phase_1.bsp");
        const std::filesystem::path gravity_raw = region_phases_directory /
            ("chunk_" + std::to_string(chunk_x) + "_0_phase_2.bsp");
        {
            std::ofstream raw(structure_raw, std::ios::binary | std::ios::trunc);
            for (int32_t x = min_x; x < min_x + 2; ++x) {
                appendRaw(raw, x, 64, 0, "minecraft:stone", 0x05);
                region_structure_expected[{x, 64, 0}] =
                    {"minecraft:stone", 0, 0x05};
            }
            assert(raw);
        }
        {
            std::ofstream raw(gravity_raw, std::ios::binary | std::ios::trunc);
            for (int32_t x = min_x; x < min_x + 2; ++x) {
                appendRaw(raw, x, 65, 0, "minecraft:sand", 0x07);
                region_gravity_expected[{x, 65, 0}] =
                    {"minecraft:sand", 0, 0x07};
            }
            assert(raw);
        }
        ChunkDescriptor chunk;
        chunk.coord = {chunk_x, 0};
        chunk.imported_bounds = {min_x, 64, 0, min_x + 1, 65, 0};
        chunk.has_phase[phaseIndex(ImportPhase::Structure)] = true;
        chunk.spool_paths[phaseIndex(ImportPhase::Structure)] = structure_raw.string();
        chunk.has_phase[phaseIndex(ImportPhase::Gravity)] = true;
        chunk.spool_paths[phaseIndex(ImportPhase::Gravity)] = gravity_raw.string();
        region_phase_chunks.push_back(std::move(chunk));
    }
    assert(CommandSpoolBuilder::build(
        region_phases_directory.string(), 50000,
        OverwritePolicy::PreserveExisting, 2, &region_phase_chunks, &error));
    assert(region_phase_chunks.size() == 1);
    const ChunkDescriptor& phase_region = region_phase_chunks[0];
    assert(phase_region.coord == ChunkCoord({0, 0}));
    assert(phase_region.has_phase[phaseIndex(ImportPhase::Structure)]);
    assert(phase_region.has_phase[phaseIndex(ImportPhase::Gravity)]);
    assert(phase_region.phase_block_counts[phaseIndex(ImportPhase::Structure)] == 4);
    assert(phase_region.phase_block_counts[phaseIndex(ImportPhase::Gravity)] == 4);
    assert(std::filesystem::path(phase_region.command_paths[
               phaseIndex(ImportPhase::Structure)]).filename() ==
           "region_0_0_phase_1.bsp.cmd");
    assert(std::filesystem::path(phase_region.command_paths[
               phaseIndex(ImportPhase::Gravity)]).filename() ==
           "region_0_0_phase_2.bsp.cmd");
    assert(assertExactCommandCover(
               phase_region.command_paths[phaseIndex(ImportPhase::Structure)],
               region_structure_expected, fillBlockLimitForRate(50000),
               ImportPhase::Structure) ==
           std::vector<uint32_t>({4}));
    assert(assertExactCommandCover(
               phase_region.command_paths[phaseIndex(ImportPhase::Gravity)],
               region_gravity_expected, dynamicFillBlockLimitForRate(50000),
               ImportPhase::Gravity) ==
           std::vector<uint32_t>({4}));

    const std::filesystem::path region_clear_directory =
        directory / "region_clear_only";
    std::filesystem::create_directories(region_clear_directory);
    const std::filesystem::path clear_structure_raw =
        region_clear_directory / "chunk_0_0_phase_1.bsp";
    {
        std::ofstream raw(clear_structure_raw, std::ios::binary | std::ios::trunc);
        appendRaw(raw, 0, 40, 0, "minecraft:stone", 0x05);
        assert(raw);
    }
    std::vector<ChunkDescriptor> region_clear_chunks;
    for (const int32_t chunk_x : std::array<int32_t, 4>{{0, 1, 4, 5}}) {
        ChunkDescriptor chunk;
        chunk.coord = {chunk_x, 0};
        chunk.imported_bounds = {chunk_x * 32, 40, 0,
                                 chunk_x * 32 + 31, 40, 31};
        if (chunk_x == 0) {
            chunk.has_phase[phaseIndex(ImportPhase::Structure)] = true;
            chunk.spool_paths[phaseIndex(ImportPhase::Structure)] =
                clear_structure_raw.string();
        }
        region_clear_chunks.push_back(std::move(chunk));
    }
    assert(CommandSpoolBuilder::build(
        region_clear_directory.string(), 50000,
        OverwritePolicy::ClearImportedBounds, 2, &region_clear_chunks, &error));
    assert(region_clear_chunks.size() == 2);
    const ChunkDescriptor* mixed_clear_region = nullptr;
    const ChunkDescriptor* clear_only_region = nullptr;
    for (const ChunkDescriptor& region : region_clear_chunks) {
        if (region.coord == ChunkCoord({0, 0})) {
            mixed_clear_region = &region;
        } else if (region.coord == ChunkCoord({4, 0})) {
            clear_only_region = &region;
        } else {
            assert(false);
        }
    }
    assert(mixed_clear_region != nullptr && clear_only_region != nullptr);
    assert(mixed_clear_region->imported_bounds.min_x == 0);
    assert(mixed_clear_region->imported_bounds.max_x == 63);
    assert(mixed_clear_region->has_phase[phaseIndex(ImportPhase::Structure)]);
    assert(mixed_clear_region->phase_block_counts[
               phaseIndex(ImportPhase::Structure)] == 1);
    assert(std::filesystem::path(mixed_clear_region->command_paths[
               phaseIndex(ImportPhase::Structure)]).filename() ==
           "region_0_0_phase_1.bsp.cmd");
    assert(clear_only_region->imported_bounds.min_x == 128);
    assert(clear_only_region->imported_bounds.max_x == 191);
    for (size_t phase = phaseIndex(ImportPhase::Structure);
         phase < kImportPhaseCount; ++phase) {
        assert(!clear_only_region->has_phase[phase]);
        assert(clear_only_region->command_paths[phase].empty());
    }
    std::vector<VerificationChunkPlan> region_clear_plans;
    assert(CommandSpoolBuilder::loadVerificationPlan(
        (region_clear_directory /
         CommandSpoolBuilder::kVerificationPlanName).string(),
        &region_clear_plans, &error));
    assert(region_clear_plans.size() == 4);
    const std::set<ChunkCoord> expected_empty_coords{{1, 0}, {4, 0}, {5, 0}};
    std::set<ChunkCoord> seen_empty_coords;
    for (const VerificationChunkPlan& plan : region_clear_plans) {
        if (plan.total_block_count != 0) {
            assert(plan.coord == ChunkCoord({0, 0}));
            assert(plan.total_block_count == 1);
            continue;
        }
        assert(plan.eligible_count == 0);
        assert(plan.samples.size() == 4);
        assert(expected_empty_coords.count(plan.coord) == 1);
        assert(seen_empty_coords.insert(plan.coord).second);
        for (const VerificationPlanSample& sample : plan.samples) {
            assert(hasVerificationSampleFlag(
                sample, VerificationSampleFlag::ExpectedAir));
            assert(sample.name == "minecraft:air");
        }
    }
    assert(seen_empty_coords == expected_empty_coords);

    const std::filesystem::path empty_directory = directory / "empty_overwrite";
    std::filesystem::create_directories(empty_directory);
    ChunkDescriptor empty;
    empty.coord = {-2, 3};
    empty.imported_bounds = {-64, 40, 96, -33, 42, 127};
    std::vector<ChunkDescriptor> empty_chunks{empty};
    assert(CommandSpoolBuilder::build(empty_directory.string(), 20,
                                      OverwritePolicy::ClearImportedBounds,
                                      &empty_chunks, &error));
    assert(empty_chunks.size() == 1);
    for (size_t phase = phaseIndex(ImportPhase::Structure);
         phase < kImportPhaseCount; ++phase) {
        assert(!empty_chunks[0].has_phase[phase]);
        assert(empty_chunks[0].command_paths[phase].empty());
    }
    std::vector<VerificationChunkPlan> empty_plans;
    assert(CommandSpoolBuilder::loadVerificationPlan(
        (empty_directory / CommandSpoolBuilder::kVerificationPlanName).string(),
        &empty_plans, &error));
    assert(empty_plans.size() == 1);
    assert(empty_plans[0].coord == empty.coord);
    assert(empty_plans[0].total_block_count == 0);
    assert(empty_plans[0].eligible_count == 0);
    assert(empty_plans[0].samples.size() == 4);
    for (const VerificationPlanSample& sample : empty_plans[0].samples) {
        assert(hasVerificationSampleFlag(sample, VerificationSampleFlag::ExpectedAir));
        assert(sample.name == "minecraft:air");
    }

    std::vector<ChunkDescriptor> preserved_empty{empty};
    error.clear();
    assert(!CommandSpoolBuilder::build(empty_directory.string(), 20,
                                       OverwritePolicy::PreserveExisting,
                                       &preserved_empty, &error));
    assert(error == "schematic contains no non-air blocks");

    // The optional deny foundation needs empty source-volume partitions in
    // preserve mode, but those partitions must not become expected-air
    // verification samples or source placement commands.
    const std::filesystem::path retained_empty_directory =
        directory / "empty_preserve_foundation";
    std::filesystem::create_directories(retained_empty_directory);
    std::vector<ChunkDescriptor> retained_empty{empty};
    error.clear();
    assert(CommandSpoolBuilder::build(
        retained_empty_directory.string(), 20,
        OverwritePolicy::PreserveExisting, 1, &retained_empty, &error,
        {}, false, {}, true, true));
    assert(retained_empty.size() == 1);
    for (size_t phase = phaseIndex(ImportPhase::Structure);
         phase < kImportPhaseCount; ++phase) {
        assert(!retained_empty[0].has_phase[phase]);
        assert(retained_empty[0].command_paths[phase].empty());
    }
    std::vector<VerificationChunkPlan> retained_empty_plans;
    assert(CommandSpoolBuilder::loadVerificationPlan(
        (retained_empty_directory / CommandSpoolBuilder::kVerificationPlanName).string(),
        &retained_empty_plans, &error));
    assert(retained_empty_plans.size() == 1);
    assert(retained_empty_plans[0].total_block_count == 0);
    assert(retained_empty_plans[0].eligible_count == 0);
    assert(retained_empty_plans[0].samples.empty());

    const std::filesystem::path metadata_directory = directory / "metadata_only";
    std::filesystem::create_directories(metadata_directory);
    const std::filesystem::path metadata_raw =
        metadata_directory / "chunk_0_0_phase_1.bsp";
    {
        std::ofstream raw(metadata_raw, std::ios::binary | std::ios::trunc);
        appendRaw(raw, 0, 64, 0, "minecraft:stone");
        appendRaw(raw, 1, 64, 0, "minecraft:dirt");
        assert(raw);
    }
    ChunkDescriptor metadata_descriptor;
    metadata_descriptor.coord = {0, 0};
    metadata_descriptor.imported_bounds = {0, 64, 0, 1, 64, 0};
    metadata_descriptor.has_phase[phaseIndex(ImportPhase::Structure)] = true;
    metadata_descriptor.spool_paths[phaseIndex(ImportPhase::Structure)] =
        metadata_raw.string();
    std::vector<ChunkDescriptor> metadata_chunks{metadata_descriptor};
    assert(CommandSpoolBuilder::build(
        metadata_directory.string(), 20, OverwritePolicy::ClearImportedBounds,
        1, &metadata_chunks, &error, {}, false, {}, false));
    std::vector<VerificationChunkPlan> metadata_plans;
    assert(CommandSpoolBuilder::loadVerificationPlan(
        (metadata_directory / CommandSpoolBuilder::kVerificationPlanName).string(),
        &metadata_plans, &error));
    assert(metadata_plans.size() == 1);
    assert(metadata_plans[0].coord == metadata_descriptor.coord);
    assert(metadata_plans[0].imported_bounds.min_x == 0);
    assert(metadata_plans[0].imported_bounds.max_x == 1);
    assert(metadata_plans[0].total_block_count == 2);
    assert(metadata_plans[0].eligible_count == 2);
    assert(metadata_plans[0].samples.empty());
    CommandSpoolReader metadata_reader(
        metadata_chunks[0].command_paths[phaseIndex(ImportPhase::Structure)]);
    assert(metadata_reader.valid());
    assert(metadata_reader.totalBlockCount() == 2);
    }

    std::filesystem::remove_all(directory);
    return 0;
}
