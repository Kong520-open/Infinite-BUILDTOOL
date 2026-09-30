#ifndef INFINITE_TEXTURE_BUILD_IMPORT_UNDO_STORE_H
#define INFINITE_TEXTURE_BUILD_IMPORT_UNDO_STORE_H

#include "BuildImportTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace build_import {

constexpr uint32_t kBuildImportUndoStoreCurrentVersion = 2;

enum class BuildImportUndoRecordState : uint32_t {
    Available = 0,
    Claimed = 1,
    Consumed = 2,
};

enum class BuildImportUndoTransitionResult {
    Updated,
    MissingOrInvalid,
    Mismatch,
    WriteFailed,
};

struct BuildImportUndoRegion {
    ChunkCoord coord;
    BlockBounds bounds;
};

struct BuildImportUndoManifest {
    WorldContext world;
    int32_t chunk_size = ImportConfig::kVanillaChunkSize;
    int32_t simulation_chunk_range = ImportConfig::kDefaultSimulationChunkRange;
    int32_t blocks_per_second = 20;
    std::vector<BuildImportUndoRegion> regions;
    std::string record_id;
    BuildImportUndoRecordState state = BuildImportUndoRecordState::Available;
    std::string claim_job_id;
};

using BuildImportUndoSnapshot = BuildImportUndoManifest;

class BuildImportUndoStore {
public:
    using Region = BuildImportUndoRegion;
    using Manifest = BuildImportUndoManifest;
    using Snapshot = BuildImportUndoManifest;

    static constexpr const char* kFileName = "last_import.undo";
    static constexpr size_t kMaximumWorldIdLength = 16U * 1024U;
    static constexpr size_t kMaximumRecordIdLength = 256U;
    static constexpr size_t kMaximumClaimJobIdLength = 1024U;
    static constexpr uint32_t kMaximumRegionCount = 65'536U;

    static std::string filePath(const std::string& directory);
    static bool buildNormalizedRegions(
        const std::vector<BlockBounds>& imported_bounds,
        const std::optional<BlockBounds>& deny_foundation,
        std::vector<BuildImportUndoRegion>* regions,
        std::string* error = nullptr);
    static bool saveAtomically(const std::string& directory,
                               const BuildImportUndoManifest& manifest,
                               std::string* error = nullptr);
    static std::optional<BuildImportUndoManifest> load(const std::string& directory,
                                                       std::string* error = nullptr);
    static BuildImportUndoTransitionResult transitionState(
        const std::string& directory, const std::string& record_id,
        BuildImportUndoRecordState expected_state,
        const std::string& expected_claim_job_id,
        BuildImportUndoRecordState next_state,
        const std::string& next_claim_job_id,
        std::string* error = nullptr);
    static bool remove(const std::string& directory, std::string* error = nullptr);
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BUILD_IMPORT_UNDO_STORE_H
