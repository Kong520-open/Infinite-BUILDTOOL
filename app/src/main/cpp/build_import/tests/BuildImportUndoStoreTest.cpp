#include "BuildImportUndoStore.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace build_import;

namespace {

BuildImportUndoManifest makeManifest() {
    BuildImportUndoManifest manifest;
    manifest.world.world_id =
        "stable:v1:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef|0";
    manifest.world.dimension_id = 0;
    manifest.chunk_size = ImportConfig::kVanillaChunkSize;
    manifest.simulation_chunk_range = 6;
    manifest.blocks_per_second = 12'345;
    manifest.record_id = "record-0123456789abcdef";
    manifest.regions = {
        {{2, -1}, {32, -64, -16, 47, 319, -1}},
        {{-1, 0}, {-16, 5, 0, -1, 72, 15}},
    };
    return manifest;
}

bool sameBounds(const BlockBounds& left, const BlockBounds& right) {
    return left.min_x == right.min_x && left.min_y == right.min_y &&
        left.min_z == right.min_z && left.max_x == right.max_x &&
        left.max_y == right.max_y && left.max_z == right.max_z;
}

void assertSameManifest(const BuildImportUndoManifest& actual,
                        const BuildImportUndoManifest& expected) {
    assert(actual.world == expected.world);
    assert(actual.chunk_size == expected.chunk_size);
    assert(actual.simulation_chunk_range == expected.simulation_chunk_range);
    assert(actual.blocks_per_second == expected.blocks_per_second);
    assert(actual.record_id == expected.record_id);
    assert(actual.state == expected.state);
    assert(actual.claim_job_id == expected.claim_job_id);
    assert(actual.regions.size() == expected.regions.size());
    for (size_t index = 0; index < actual.regions.size(); ++index) {
        assert(actual.regions[index].coord == expected.regions[index].coord);
        assert(sameBounds(actual.regions[index].bounds, expected.regions[index].bounds));
    }
}

std::vector<char> readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    assert(stream);
    const std::streamoff size = stream.tellg();
    assert(size >= 0);
    std::vector<char> bytes(static_cast<size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    assert(stream);
    return bytes;
}

void writeFile(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    assert(stream);
    if (!bytes.empty()) {
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    assert(stream);
}

void putUint32(std::vector<char>* bytes, size_t offset, uint32_t value) {
    assert(bytes && offset <= bytes->size() && bytes->size() - offset >= 4);
    (*bytes)[offset] = static_cast<char>(value & 0xffU);
    (*bytes)[offset + 1] = static_cast<char>((value >> 8U) & 0xffU);
    (*bytes)[offset + 2] = static_cast<char>((value >> 16U) & 0xffU);
    (*bytes)[offset + 3] = static_cast<char>((value >> 24U) & 0xffU);
}

size_t regionCountOffset(const BuildImportUndoManifest& manifest) {
    // magic, version, string length, string, dimension and three config fields.
    return 8U + 4U + manifest.world.world_id.size() + 4U + 12U;
}

size_t firstRegionOffset(const BuildImportUndoManifest& manifest) {
    return regionCountOffset(manifest) + 4U;
}

void restoreValidFile(const std::string& directory,
                      const BuildImportUndoManifest& manifest) {
    std::string error;
    assert(BuildImportUndoStore::saveAtomically(directory, manifest, &error));
    assert(error.empty());
}

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "build_import_undo_store_test";
    std::error_code filesystem_error;
    std::filesystem::remove_all(directory, filesystem_error);
    filesystem_error.clear();
    assert(std::filesystem::create_directories(directory, filesystem_error));
    assert(!filesystem_error);

    const std::string directory_string = directory.string();
    const std::filesystem::path undo_path =
        BuildImportUndoStore::filePath(directory_string);
    assert(undo_path.filename() == BuildImportUndoStore::kFileName);
    assert(BuildImportUndoStore::remove(directory_string));

    const BuildImportUndoManifest original = makeManifest();
    restoreValidFile(directory_string, original);
    auto loaded = BuildImportUndoStore::load(directory_string);
    assert(loaded.has_value());
    assertSameManifest(*loaded, original);

    // Imported bounds from legacy 32-block plans are split onto the current
    // 16x16 grid. A matching deny foundation is folded into the adjacent
    // bottom slice so undo clears both the building footprint and deny layer.
    std::vector<BuildImportUndoRegion> normalized;
    std::string normalize_error;
    const BlockBounds legacy_bounds{8, 10, -8, 31, 20, 7};
    const BlockBounds foundation{8, 9, -8, 31, 9, 7};
    assert(BuildImportUndoStore::buildNormalizedRegions(
        {legacy_bounds}, foundation, &normalized, &normalize_error));
    assert(normalize_error.empty());
    assert(normalized.size() == 4U);
    for (const BuildImportUndoRegion& region : normalized) {
        assert(region.bounds.min_y == 9);
        assert(region.bounds.max_y == 20);
        assert(chunkForBlock(region.bounds.min_x, region.bounds.min_z,
                             ImportConfig::kVanillaChunkSize) == region.coord);
        assert(chunkForBlock(region.bounds.max_x, region.bounds.max_z,
                             ImportConfig::kVanillaChunkSize) == region.coord);
    }
    normalized.clear();
    assert(!BuildImportUndoStore::buildNormalizedRegions(
        {legacy_bounds}, BlockBounds{7, 9, -8, 31, 9, 7},
        &normalized, &normalize_error));
    assert(normalize_error ==
           "deny foundation does not match the completed import footprint");

    // Publishing a newer successful import replaces the prior manifest and
    // does not expose the temporary file as a second candidate.
    BuildImportUndoManifest replacement = original;
    replacement.world.world_id = "another-world";
    replacement.world.dimension_id = 2;
    replacement.simulation_chunk_range = 8;
    replacement.blocks_per_second = kMaximumBlocksPerSecond;
    replacement.record_id = "replacement-record";
    replacement.regions = {{{0, 0}, {3, 10, 7, 12, 20, 15}}};
    restoreValidFile(directory_string, replacement);
    loaded = BuildImportUndoStore::load(directory_string);
    assert(loaded.has_value());
    assertSameManifest(*loaded, replacement);
    assert(!std::filesystem::exists(undo_path.string() + ".tmp"));

    // Saving malformed in-memory data never replaces the last valid record.
    BuildImportUndoManifest invalid = original;
    invalid.regions.push_back(invalid.regions.front());
    assert(!BuildImportUndoStore::saveAtomically(directory_string, invalid));
    invalid = original;
    invalid.regions.front().coord.x += 1;
    assert(!BuildImportUndoStore::saveAtomically(directory_string, invalid));
    invalid = original;
    invalid.regions.front().bounds.max_y = invalid.regions.front().bounds.min_y - 1;
    assert(!BuildImportUndoStore::saveAtomically(directory_string, invalid));
    invalid = original;
    invalid.chunk_size = 32;
    assert(!BuildImportUndoStore::saveAtomically(directory_string, invalid));
    invalid = original;
    invalid.simulation_chunk_range =
        ImportConfig::kMaximumSimulationChunkRange + 1;
    assert(!BuildImportUndoStore::saveAtomically(directory_string, invalid));
    invalid = original;
    invalid.blocks_per_second = 0;
    assert(!BuildImportUndoStore::saveAtomically(directory_string, invalid));
    invalid = original;
    invalid.world.world_id.assign(BuildImportUndoStore::kMaximumWorldIdLength + 1U, 'x');
    assert(!BuildImportUndoStore::saveAtomically(directory_string, invalid));
    invalid = original;
    invalid.regions.clear();
    assert(!BuildImportUndoStore::saveAtomically(directory_string, invalid));
    invalid = original;
    invalid.record_id.clear();
    assert(!BuildImportUndoStore::saveAtomically(directory_string, invalid));
    invalid = original;
    invalid.state = BuildImportUndoRecordState::Available;
    invalid.claim_job_id = "unexpected-claim";
    assert(!BuildImportUndoStore::saveAtomically(directory_string, invalid));
    invalid = original;
    invalid.state = BuildImportUndoRecordState::Claimed;
    assert(!BuildImportUndoStore::saveAtomically(directory_string, invalid));
    loaded = BuildImportUndoStore::load(directory_string);
    assert(loaded.has_value());
    assertSameManifest(*loaded, replacement);

    // Magic and exact version are fail-closed.
    restoreValidFile(directory_string, original);
    std::vector<char> valid_bytes = readFile(undo_path);
    // Version 1 records remain readable and receive a deterministic legacy
    // identity. Claiming them republishes the record in the bound v2 format.
    std::vector<char> legacy = valid_bytes;
    putUint32(&legacy, 4, 1U);
    constexpr size_t kRegionRecordSize = 8U + 24U;
    legacy.resize(firstRegionOffset(original) +
                  original.regions.size() * kRegionRecordSize);
    writeFile(undo_path, legacy);
    loaded = BuildImportUndoStore::load(directory_string);
    assert(loaded.has_value());
    assert(loaded->record_id.rfind("legacy-", 0) == 0);
    assert(loaded->state == BuildImportUndoRecordState::Available);
    assert(loaded->claim_job_id.empty());
    restoreValidFile(directory_string, original);
    valid_bytes = readFile(undo_path);
    std::vector<char> damaged = valid_bytes;
    putUint32(&damaged, 0, 0);
    writeFile(undo_path, damaged);
    assert(!BuildImportUndoStore::load(directory_string).has_value());
    damaged = valid_bytes;
    putUint32(&damaged, 4, kBuildImportUndoStoreCurrentVersion + 1U);
    writeFile(undo_path, damaged);
    assert(!BuildImportUndoStore::load(directory_string).has_value());

    // Length and count fields are checked before allocating their payloads.
    damaged = valid_bytes;
    putUint32(&damaged, 8,
              static_cast<uint32_t>(BuildImportUndoStore::kMaximumWorldIdLength + 1U));
    writeFile(undo_path, damaged);
    assert(!BuildImportUndoStore::load(directory_string).has_value());
    damaged = valid_bytes;
    putUint32(&damaged, regionCountOffset(original),
              BuildImportUndoStore::kMaximumRegionCount + 1U);
    writeFile(undo_path, damaged);
    assert(!BuildImportUndoStore::load(directory_string).has_value());

    // Truncation and otherwise valid payloads with appended bytes are rejected.
    damaged = valid_bytes;
    assert(!damaged.empty());
    damaged.pop_back();
    writeFile(undo_path, damaged);
    assert(!BuildImportUndoStore::load(directory_string).has_value());
    damaged = valid_bytes;
    damaged.push_back(static_cast<char>(0x7f));
    writeFile(undo_path, damaged);
    assert(!BuildImportUndoStore::load(directory_string).has_value());

    // Duplicate records and a coordinate that disagrees with its 16x16 block
    // bounds are rejected even when the rest of the file is well-formed.
    damaged = valid_bytes;
    const size_t first_region = firstRegionOffset(original);
    assert(damaged.size() >= first_region + 2U * kRegionRecordSize);
    for (size_t index = 0; index < kRegionRecordSize; ++index) {
        damaged[first_region + kRegionRecordSize + index] = damaged[first_region + index];
    }
    writeFile(undo_path, damaged);
    assert(!BuildImportUndoStore::load(directory_string).has_value());
    damaged = valid_bytes;
    putUint32(&damaged, first_region, 3U);
    writeFile(undo_path, damaged);
    assert(!BuildImportUndoStore::load(directory_string).has_value());

    // Invalid on-disk configuration and inverted bounds are not recoverable.
    damaged = valid_bytes;
    const size_t chunk_size_offset = 8U + 4U + original.world.world_id.size() + 4U;
    putUint32(&damaged, chunk_size_offset, 32U);
    writeFile(undo_path, damaged);
    assert(!BuildImportUndoStore::load(directory_string).has_value());
    damaged = valid_bytes;
    const size_t maximum_y_offset = first_region + 8U + 16U;
    putUint32(&damaged, maximum_y_offset,
              static_cast<uint32_t>(original.regions.front().bounds.min_y - 1));
    writeFile(undo_path, damaged);
    assert(!BuildImportUndoStore::load(directory_string).has_value());

    restoreValidFile(directory_string, original);
    std::string transition_error;
    assert(BuildImportUndoStore::transitionState(
               directory_string, original.record_id,
               BuildImportUndoRecordState::Available, {},
               BuildImportUndoRecordState::Claimed, "undo-job", &transition_error) ==
           BuildImportUndoTransitionResult::Updated);
    assert(transition_error.empty());
    loaded = BuildImportUndoStore::load(directory_string);
    assert(loaded.has_value());
    assert(loaded->state == BuildImportUndoRecordState::Claimed);
    assert(loaded->claim_job_id == "undo-job");
    assert(BuildImportUndoStore::transitionState(
               directory_string, "different-record",
               BuildImportUndoRecordState::Claimed, "undo-job",
               BuildImportUndoRecordState::Consumed, "undo-job") ==
           BuildImportUndoTransitionResult::Mismatch);
    assert(BuildImportUndoStore::transitionState(
               directory_string, original.record_id,
               BuildImportUndoRecordState::Claimed, "undo-job",
               BuildImportUndoRecordState::Available, {}) ==
           BuildImportUndoTransitionResult::Updated);
    assert(BuildImportUndoStore::transitionState(
               directory_string, original.record_id,
               BuildImportUndoRecordState::Available, {},
               BuildImportUndoRecordState::Consumed, {}) ==
           BuildImportUndoTransitionResult::Updated);
    loaded = BuildImportUndoStore::load(directory_string);
    assert(loaded.has_value());
    assert(loaded->state == BuildImportUndoRecordState::Consumed);

    restoreValidFile(directory_string, original);
    {
        std::ofstream stale(undo_path.string() + ".tmp", std::ios::binary);
        assert(stale);
        stale.put('x');
    }
    assert(BuildImportUndoStore::remove(directory_string));
    assert(!std::filesystem::exists(undo_path));
    assert(!std::filesystem::exists(undo_path.string() + ".tmp"));
    assert(BuildImportUndoStore::remove(directory_string));
    assert(!BuildImportUndoStore::load(directory_string).has_value());

    filesystem_error.clear();
    std::filesystem::remove_all(directory, filesystem_error);
    assert(!filesystem_error);
    return 0;
}
