#ifndef INFINITE_TEXTURE_BUILD_EXPORT_CHECKPOINT_H
#define INFINITE_TEXTURE_BUILD_EXPORT_CHECKPOINT_H

#include "CommandBlockSpool.h"
#include "SchematicRawBlock.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace build_import {

constexpr uint32_t kBuildExportCheckpointVersion = 5;
constexpr uint32_t kMinimumBuildExportCheckpointVersion = 1;
constexpr uint32_t kBuildExportTraversalVersion = 1;

struct BuildExportCheckpointSnapshot {
    uint32_t format_version = kBuildExportCheckpointVersion;
    std::string output_path;
    std::string world_id;
    int32_t dimension_id = 0;
    int32_t min_x = 0;
    int32_t min_y = 0;
    int32_t min_z = 0;
    int32_t max_x = -1;
    int32_t max_y = -1;
    int32_t max_z = -1;
    // Horizontal side of one scan batch in blocks. Legacy checkpoints store
    // 64 here (four 16x16 vanilla chunks); newer exports store 64..128.
    int32_t batch_size = 0;
    uint32_t traversal_version = kBuildExportTraversalVersion;
    uint64_t total_blocks = 0;
    uint64_t journal_entries = 0;
    uint64_t exported_blocks = 0;
    uint64_t current_batch = 0;
    uint64_t batch_cursor = 0;
    bool writing = false;
    std::vector<std::string> palette;
    // V2 persists the editable command-block payload alongside the palette.
    // V1 checkpoints leave this empty and are still accepted for migration.
    std::vector<CommandBlockRecord> command_blocks;
    // V3 persists the lossless Bedrock snapshots used by the schematic
    // Infinitecz extension. Older checkpoints simply restore without them.
    std::vector<SchematicRawBlock> raw_blocks;
    // V4 keeps a completed voxel batch resumable while its coordinate-specific
    // container inventories are still being captured. V5 records whether this
    // export includes container inventories at all. The target list itself is
    // rebuilt deterministically from raw_blocks; only its verified size and
    // completed prefix need to be durable.
    bool export_container_items = true;
    bool container_capture_pending = false;
    uint64_t container_target_count = 0;
    uint64_t container_cursor = 0;
};

class BuildExportCheckpoint {
public:
    static std::string checkpointPath(const std::string& output_path);
    static std::string journalPath(const std::string& output_path);

    // Returns true for incomplete or corrupt artifacts too, allowing the UI to
    // offer an explicit restart/delete choice instead of silently overwriting.
    static bool hasArtifacts(const std::string& output_path);
    static bool createJournal(const std::string& output_path, std::string* error = nullptr);
    static bool appendJournal(const std::string& output_path, const uint16_t* values,
                              size_t count, std::string* error = nullptr);
    static bool syncJournal(const std::string& output_path, std::string* error = nullptr);
    static bool truncateJournal(const std::string& output_path, uint64_t entry_count,
                                std::string* error = nullptr);
    static bool journalHasEntries(const std::string& output_path, uint64_t entry_count,
                                  std::string* error = nullptr);

    static bool saveAtomically(const std::string& output_path,
                               const BuildExportCheckpointSnapshot& snapshot,
                               std::string* error = nullptr);
    // Runtime commits can borrow the live palette and avoid copying up to
    // 32 MiB of strings merely to serialize a checkpoint.
    static bool saveAtomically(const std::string& output_path,
                               const BuildExportCheckpointSnapshot& snapshot,
                               const std::vector<std::string>& palette,
                               std::string* error = nullptr);
    static bool saveAtomically(const std::string& output_path,
                               const BuildExportCheckpointSnapshot& snapshot,
                               const std::vector<std::string>& palette,
                               const std::vector<CommandBlockRecord>& command_blocks,
                               std::string* error = nullptr);
    static bool saveAtomically(const std::string& output_path,
                               const BuildExportCheckpointSnapshot& snapshot,
                               const std::vector<std::string>& palette,
                               const std::vector<CommandBlockRecord>& command_blocks,
                               const std::vector<SchematicRawBlock>& raw_blocks,
                               std::string* error = nullptr);
    static std::optional<BuildExportCheckpointSnapshot> load(
        const std::string& output_path, std::string* error = nullptr);

    // Removes only resumable-export sidecars and unfinished writer .part files;
    // completed publications are never removed.
    static bool discard(const std::string& output_path, std::string* error = nullptr);
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BUILD_EXPORT_CHECKPOINT_H
