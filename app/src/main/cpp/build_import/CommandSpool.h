#ifndef INFINITE_TEXTURE_COMMAND_SPOOL_H
#define INFINITE_TEXTURE_COMMAND_SPOOL_H

#include "BuildImportTypes.h"

#include <cstdint>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace build_import {

enum class PlannedCommandFlag : uint8_t {
    HasMergeMetadata = 0x01,
    Fillable = 0x02,
    SingleLayer = 0x04,
};

struct PlannedCommand {
    BlockBounds bounds;
    std::string name;
    uint16_t aux = 0;
    uint32_t block_count = 0;
    // Version 1 command spools have no merge metadata and leave this at zero.
    // They remain executable, while region stitching applies its historical
    // conservative singleton rule to those records.
    uint8_t flags = 0;
};

inline bool hasPlannedCommandFlag(const PlannedCommand& command,
                                  PlannedCommandFlag flag) {
    return (command.flags & static_cast<uint8_t>(flag)) != 0;
}

struct VerificationPlanSample {
    ChunkCoord chunk;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    std::string name;
    uint16_t aux = 0;
    uint8_t flags = 0;
};

enum class VerificationSampleFlag : uint8_t {
    ExpectedAir = 0x01,
    IgnoreAux = 0x02,
};

inline bool hasVerificationSampleFlag(const VerificationPlanSample& sample,
                                      VerificationSampleFlag flag) {
    return (sample.flags & static_cast<uint8_t>(flag)) != 0;
}

struct VerificationChunkPlan {
    ChunkCoord coord;
    BlockBounds imported_bounds;
    uint64_t total_block_count = 0;
    uint64_t eligible_count = 0;
    std::vector<VerificationPlanSample> samples;
};

enum class CommandSpoolBuildStage : uint8_t {
    OptimizingChunks = 0,
    WritingVerificationPlan,
    AggregatingRegions,
};

struct CommandSpoolBuildProgress {
    CommandSpoolBuildStage stage = CommandSpoolBuildStage::OptimizingChunks;
    uint64_t completed = 0;
    uint64_t total = 0;
};

class CommandSpoolBuilder {
public:
    using CancelCheck = std::function<bool()>;
    using ProgressCallback = std::function<void(const CommandSpoolBuildProgress&)>;

    static constexpr const char* kVerificationPlanName = "verification.plan";

    // Converts raw per-block spools into compact, already-merged command
    // streams. This runs on the parser worker, never on the game tick.
    static bool build(const std::string& directory, int32_t blocks_per_second,
                      OverwritePolicy overwrite_policy,
                      std::vector<ChunkDescriptor>* chunks, std::string* error = nullptr,
                      const CancelCheck& cancel_check = {});

    // Aggregates finalized per-chunk command streams into region-sized work
    // units. Verification metadata remains per chunk so repair retains its
    // original precision. When align_region_grid_to_content is set, the grid
    // begins at the minimum parsed logical chunk instead of world chunk 0,0.
    // The selected origin is retained in each resulting descriptor for the
    // controller to checkpoint with the import plan. Disabling verification
    // samples still writes per-chunk bounds and counts required by restore.
    static bool build(const std::string& directory, int32_t blocks_per_second,
                       OverwritePolicy overwrite_policy, int32_t region_span,
                       std::vector<ChunkDescriptor>* chunks, std::string* error = nullptr,
                       const CancelCheck& cancel_check = {},
                       bool align_region_grid_to_content = false,
                        const ProgressCallback& progress_callback = {},
                        bool include_verification_samples = true,
                       // Preserve-existing imports normally discard empty
                       // source partitions. Auxiliary plans that cover the
                       // full source footprint can retain them without
                       // turning their air into verification expectations.
                        bool retain_empty_descriptors = false,
                        // A non-zero cap is used by the limited building-tools
                        // profile to prevent a single /fill from consuming more
                        // than its entire one-second block allowance.
                        uint32_t maximum_merged_command_blocks = 0);

    // Compatibility overload for callers that do not perform destructive
    // overwrite. New code should pass the configured policy explicitly.
    static bool build(const std::string& directory, int32_t blocks_per_second,
                      std::vector<ChunkDescriptor>* chunks, std::string* error = nullptr,
                      const CancelCheck& cancel_check = {});

    static bool loadVerificationPlan(const std::string& path,
                                     std::vector<VerificationChunkPlan>* chunks,
                                     std::string* error = nullptr);

    // Compatibility overload. It deliberately flattens only after the v2
    // chunk metadata and all counts have been validated.
    static bool loadVerificationPlan(const std::string& path,
                                     std::vector<VerificationPlanSample>* samples,
                                     std::string* error = nullptr);
};

class CommandSpoolReader {
public:
    explicit CommandSpoolReader(const std::string& path);

    bool valid() const { return valid_; }
    bool failed() const { return failed_; }
    uint64_t totalBlockCount() const { return total_block_count_; }
    uint64_t commandCount() const { return command_count_; }
    uint64_t offset() const { return offset_; }
    uint64_t commandsRead() const { return commands_read_; }
    bool exhausted() const {
        return valid_ && !failed_ && commands_read_ == command_count_;
    }
    bool seek(uint64_t offset);
    std::optional<PlannedCommand> next();

private:
    std::string path_;
    std::vector<char> stream_buffer_;
    std::ifstream stream_;
    uint64_t data_offset_ = 0;
    uint64_t file_size_ = 0;
    uint64_t offset_ = 0;
    uint64_t command_count_ = 0;
    uint64_t total_block_count_ = 0;
    uint64_t commands_read_ = 0;
    uint64_t blocks_read_ = 0;
    uint32_t version_ = 0;
    bool valid_ = false;
    bool failed_ = false;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_COMMAND_SPOOL_H
