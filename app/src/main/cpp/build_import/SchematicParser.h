#ifndef INFINITE_TEXTURE_SCHEMATIC_PARSER_H
#define INFINITE_TEXTURE_SCHEMATIC_PARSER_H

#include "ChunkSpoolWriter.h"
#include "CommandBlockSpool.h"
#include "DeferredImportDataSpool.h"
#include "SchematicRawBlock.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace build_import {

enum class SchematicParseStage : uint8_t {
    ReadingSource,
    RoutingBlocks,
    FinalizingSpools,
};

struct SchematicParseProgress {
    SchematicParseStage stage = SchematicParseStage::ReadingSource;
    uint64_t completed = 0;
    uint64_t total = 0;
};

// Lets consumers stream mapped blocks into their own bounded storage. When a
// sink is present the parser does not create ChunkSpoolWriter descriptors;
// import callers that leave it empty retain the existing spool behavior.
using ParsedBlockSink = std::function<bool(const ParsedBlock& block,
                                            std::string* error)>;

// Command-block payloads are parsed separately from normal placement spools.
// A sink normally appends to CommandBlockSpoolWriter, keeping source command
// text off the /fill merge path until the structure shell is fully placed.
using ParsedCommandBlockSink = std::function<bool(const CommandBlockRecord& record,
                                                    std::string* error)>;

using ParsedContainerItemSink = std::function<bool(const ContainerItemRecord& record,
                                                    std::string* error)>;
using ParsedSignSink = std::function<bool(const SignRecord& record,
                                           std::string* error)>;
using ParsedEntitySink = std::function<bool(const EntityRecord& record,
                                            std::string* error)>;

// Optional lossless Bedrock-state records written by the Infinitecz
// schematic extension. Coordinates delivered to this sink are in world
// space, matching the other deferred payload sinks.
using ParsedRawBlockSink = std::function<bool(const SchematicRawBlock& record,
                                               std::string* error)>;

struct SchematicParseOptions {
    std::string source_path;
    std::string spool_directory;
    // The UI coordinates are the schematic paste origin. Sponge Offset or
    // MCEdit WEOffset is added to this origin before local voxel coordinates.
    int32_t base_x = 0;
    int32_t base_y = 0;
    int32_t base_z = 0;
    int32_t chunk_size = 32;
    // Bounds metadata for formats that may span many sparse regions. Zero is
    // unlimited. Import keeps the historical cap; streaming consumers may opt
    // out when they do not retain every chunk descriptor in memory.
    size_t maximum_chunk_descriptors = 65536;
    // Sponge palette data at or below this size is decoded from a bounded
    // memory buffer after the root compound has been validated. Larger arrays
    // keep the disk-backed fallback. Zero forces the fallback.
    size_t block_data_memory_budget_bytes = 32U * 1024U * 1024U;
    // Very large root-level Sponge v2 BlockData can appear before Width and
    // Offset. Once the root has been validated, re-open the gzip source and
    // route that array directly on a second pass instead of writing then
    // reading a huge raw temporary file. Zero disables this optimization.
    size_t replay_streaming_block_data_threshold_bytes = 96U * 1024U * 1024U;
    // Zero keeps the parser's historical unbounded behavior. Bounded callers
    // can stop before appending a block that exceeds their own work budget.
    uint64_t maximum_output_blocks = 0;
    // Import planning needs air-only source bounds for overwrite clearing.
    // Consumers that do not clear the source volume may disable this to avoid
    // materializing enormous sparse bounds into chunk descriptors.
    bool include_source_volume = true;
    // Optional source resolvers let non-import consumers retain source block
    // identity without inheriting BlockMapper's command-version restrictions.
    std::function<BlockMappingResult(uint16_t id, uint8_t data)> legacy_block_resolver;
    std::function<BlockMappingResult(std::string_view state)> state_block_resolver;
    ParsedBlockSink block_sink;
    // Optional deferred command-block sink.  A missing sink preserves the
    // historic block-only import behavior: command-block shells remain, while
    // their text is counted as omitted rather than implicitly executed.
    ParsedCommandBlockSink command_block_sink;
    // Normalized post-placement payloads. These never carry arbitrary source
    // NBT and are written only after regular placement / verification.
    ParsedContainerItemSink container_item_sink;
    ParsedSignSink sign_sink;
    ParsedEntitySink entity_sink;
    // A missing sink keeps records in SchematicParseResult::raw_blocks. When
    // supplied, records are streamed to the consumer and are not duplicated
    // in the result vector.
    ParsedRawBlockSink raw_block_sink;
    // Return true to stop parsing at the next cooperative cancellation point.
    std::function<bool()> cancellation_requested;
    // Called at bounded intervals from the parser worker. Consumers must not
    // block; the callback exists so very large files can distinguish NBT I/O,
    // voxel routing and staged-spool finalization.
    std::function<void(const SchematicParseProgress&)> progress_callback;
};

struct SchematicParseResult {
    std::vector<ChunkDescriptor> chunks;
    BlockBounds source_volume_bounds;
    uint64_t source_voxel_count = 0;
    uint64_t imported_block_count = 0;
    uint64_t skipped_block_count = 0;
    uint64_t unsupported_block_count = 0;
    uint64_t degraded_block_count = 0;
    // Source entities excluded because no deferred sink was supplied, their
    // safe basic fields were invalid, or a later region superseded their cell.
    uint64_t omitted_entity_count = 0;
    // Arbitrary/invalid block-entity payloads have no import channel, so
    // parsers keep their shell blocks and record what was intentionally left
    // out. Normalized container item records are counted separately below.
    uint64_t omitted_block_entity_count = 0;
    // Command-block payloads delivered to SchematicParseOptions::command_block_sink.
    // These remain deferred records; parsing never sends or executes commands.
    uint64_t command_block_payload_count = 0;
    // Command-block payloads are omitted only when no command_block_sink is
    // provided. Captured deferred payloads are counted separately above and
    // are never executed during parsing.
    uint64_t omitted_command_block_data_count = 0;
    uint64_t container_item_payload_count = 0;
    uint64_t sign_payload_count = 0;
    uint64_t entity_payload_count = 0;
    // Lossless Bedrock records from the optional Infinitecz schematic
    // extension. Coordinates are world-space when returned here.
    std::vector<SchematicRawBlock> raw_blocks;
    uint64_t raw_block_payload_count = 0;
    uint64_t omitted_raw_block_count = 0;
    // Invalid enchantments are removed one-by-one, leaving the owning item
    // eligible for import when its other data is valid.
    uint64_t filtered_enchantment_count = 0;
    std::string first_degradation_reason;
    // Litematic selections may contain multiple independent source volumes.
    // The current clear plan stores one cuboid per execution partition, so a
    // multi-region source must not use ClearImportedBounds or gaps would be
    // erased. Other source formats leave this at zero.
    uint32_t source_region_count = 0;
    int32_t source_offset_x = 0;
    int32_t source_offset_y = 0;
    int32_t source_offset_z = 0;
    bool sponge_format = false;
};

class SchematicParser {
public:
    bool parse(const SchematicParseOptions& options,
               SchematicParseResult* result, std::string* error = nullptr) const;
    bool parse(const SchematicParseOptions& options, const BlockMapper& mapper,
               SchematicParseResult* result, std::string* error = nullptr) const;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_SCHEMATIC_PARSER_H
