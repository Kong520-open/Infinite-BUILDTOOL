#include "../SchematicParser.h"
#include "RawSpoolTestReader.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <zlib.h>

using namespace build_import;

namespace {

struct RawDiskRecord {
    int32_t x;
    int32_t y;
    int32_t z;
    uint32_t name_id;
    // RawSpoolRecordV3 preserves the full target auxiliary value. Keep this
    // host-side inspection mirror in the same v3 layout so a valid packed
    // Bedrock state cannot be narrowed in this test.
    uint16_t aux;
    uint8_t flags;
    uint8_t reserved;
};

static_assert(sizeof(RawDiskRecord) == 20, "unexpected raw spool record layout");

struct RawBlockRecord {
    RawDiskRecord block{};
    std::string name;
};

enum Tag : uint8_t {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    ByteArray = 7,
    String = 8,
    List = 9,
    Compound = 10,
    IntArray = 11,
};

void u8(std::vector<uint8_t>* output, uint8_t value) { output->push_back(value); }
void be16(std::vector<uint8_t>* output, uint16_t value) {
    output->push_back(static_cast<uint8_t>(value >> 8));
    output->push_back(static_cast<uint8_t>(value));
}
void be32(std::vector<uint8_t>* output, uint32_t value) {
    output->push_back(static_cast<uint8_t>(value >> 24));
    output->push_back(static_cast<uint8_t>(value >> 16));
    output->push_back(static_cast<uint8_t>(value >> 8));
    output->push_back(static_cast<uint8_t>(value));
}
void name(std::vector<uint8_t>* output, const std::string& value) {
    be16(output, static_cast<uint16_t>(value.size()));
    output->insert(output->end(), value.begin(), value.end());
}
void root(std::vector<uint8_t>* output) {
    u8(output, Compound);
    name(output, "Schematic");
}
void shortTag(std::vector<uint8_t>* output, const std::string& key, int16_t value) {
    u8(output, Short);
    name(output, key);
    be16(output, static_cast<uint16_t>(value));
}
void intTag(std::vector<uint8_t>* output, const std::string& key, int32_t value) {
    u8(output, Int);
    name(output, key);
    be32(output, static_cast<uint32_t>(value));
}
void byteTag(std::vector<uint8_t>* output, const std::string& key, uint8_t value) {
    u8(output, Byte);
    name(output, key);
    u8(output, value);
}
void stringTag(std::vector<uint8_t>* output, const std::string& key,
               const std::string& value) {
    u8(output, String);
    name(output, key);
    name(output, value);
}
void compoundListTag(std::vector<uint8_t>* output, const std::string& key,
                     uint32_t count) {
    u8(output, List);
    name(output, key);
    u8(output, Compound);
    be32(output, count);
}
void compoundTag(std::vector<uint8_t>* output, const std::string& key) {
    u8(output, Compound);
    name(output, key);
}
void byteArrayTag(std::vector<uint8_t>* output, const std::string& key,
                  const std::vector<uint8_t>& value) {
    u8(output, ByteArray);
    name(output, key);
    be32(output, static_cast<uint32_t>(value.size()));
    output->insert(output->end(), value.begin(), value.end());
}
void intArrayTag(std::vector<uint8_t>* output, const std::string& key,
                 const std::vector<int32_t>& value) {
    u8(output, IntArray);
    name(output, key);
    be32(output, static_cast<uint32_t>(value.size()));
    for (const int32_t component : value) be32(output, static_cast<uint32_t>(component));
}
void end(std::vector<uint8_t>* output) { u8(output, End); }

void writeGzip(const std::filesystem::path& path, const std::vector<uint8_t>& nbt) {
    gzFile file = gzopen(path.string().c_str(), "wb9");
    assert(file != nullptr);
    assert(gzwrite(file, nbt.data(), static_cast<unsigned>(nbt.size())) ==
           static_cast<int>(nbt.size()));
    assert(gzclose(file) == Z_OK);
}

bool parseFile(const std::filesystem::path& source,
               const std::filesystem::path& spool,
               SchematicParseResult* result, std::string* error) {
    SchematicParseOptions options;
    options.source_path = source.string();
    options.spool_directory = spool.string();
    options.base_x = -33;
    options.base_y = 64;
    options.base_z = 31;
    options.chunk_size = 32;
    return SchematicParser().parse(options, BlockMapper(), result, error);
}

std::vector<uint8_t> spongeV3(bool palette_hole = false,
                              bool duplicate_palette_id = false) {
    std::vector<uint8_t> nbt;
    root(&nbt);
    // Blocks deliberately precedes dimensions to verify order-independent
    // streaming of raw arrays.
    compoundTag(&nbt, "Blocks");
    compoundTag(&nbt, "Palette");
    intTag(&nbt, palette_hole ? "minecraft:stone" : "minecraft:wool", palette_hole ? 1 : 0);
    if (duplicate_palette_id) intTag(&nbt, "minecraft:dirt", palette_hole ? 1 : 0);
    if (!palette_hole) intTag(&nbt, "minecraft:red_concrete_powder", 1);
    end(&nbt);
    byteArrayTag(&nbt, "Data", palette_hole ? std::vector<uint8_t>{0}
                                             : std::vector<uint8_t>{0, 1, 0, 1});
    end(&nbt);
    shortTag(&nbt, "Width", palette_hole ? 1 : 2);
    shortTag(&nbt, "Height", 1);
    shortTag(&nbt, "Length", palette_hole ? 1 : 2);
    end(&nbt);
    return nbt;
}

std::vector<uint8_t> spongeSingle(const std::string& state,
                                   const std::vector<int32_t>* offset = nullptr) {
    std::vector<uint8_t> nbt;
    root(&nbt);
    shortTag(&nbt, "Width", 1);
    shortTag(&nbt, "Height", 1);
    shortTag(&nbt, "Length", 1);
    if (offset) intArrayTag(&nbt, "Offset", *offset);
    compoundTag(&nbt, "Blocks");
    compoundTag(&nbt, "Palette");
    intTag(&nbt, state, 0);
    end(&nbt);
    byteArrayTag(&nbt, "Data", {0});
    end(&nbt);
    end(&nbt);
    return nbt;
}

std::vector<uint8_t> spongeWithInfiniteczRawBlocks(bool duplicate = false,
                                                   uint16_t aux = 12) {
    std::vector<uint8_t> nbt;
    root(&nbt);
    shortTag(&nbt, "Width", 2);
    shortTag(&nbt, "Height", 1);
    shortTag(&nbt, "Length", 1);
    compoundTag(&nbt, "Palette");
    intTag(&nbt, "minecraft:air", 0);
    intTag(&nbt, "minecraft:stone", 1);
    end(&nbt);
    byteArrayTag(&nbt, "BlockData", {0, 1});
    compoundTag(&nbt, "Infinitecz");
    intTag(&nbt, "FormatVersion", 1);
    compoundListTag(&nbt, "RawBlocks", duplicate ? 2U : 1U);
    const auto append_raw = [&](int32_t x, const std::string& state_json) {
        intTag(&nbt, "x", x);
        intTag(&nbt, "y", 0);
        intTag(&nbt, "z", 0);
        stringTag(&nbt, "Identifier", "minecraft:lever");
        shortTag(&nbt, "Aux", static_cast<int16_t>(aux));
        intTag(&nbt, "LegacyId", 69);
        stringTag(&nbt, "StateJson", state_json);
        byteArrayTag(&nbt, "EntityJson", {'{', '}', '\n'});
        end(&nbt);
    };
    append_raw(1, "{\"face\":\"wall\",\"facing\":\"north\"}");
    if (duplicate) append_raw(1, "{\"duplicate\":true}");
    end(&nbt);  // Infinitecz
    end(&nbt);  // Schematic
    return nbt;
}

// Sponge v2 permits BlockData before Width/Offset. This exercises the large
// source replay path without creating a large test fixture.
std::vector<uint8_t> spongeV2BlockDataBeforeDimensions() {
    std::vector<uint8_t> nbt;
    root(&nbt);
    compoundTag(&nbt, "Palette");
    intTag(&nbt, "minecraft:stone", 0);
    end(&nbt);
    byteArrayTag(&nbt, "BlockData", {0, 0, 0, 0});
    shortTag(&nbt, "Width", 2);
    shortTag(&nbt, "Height", 1);
    shortTag(&nbt, "Length", 2);
    end(&nbt);
    return nbt;
}

std::vector<uint8_t> legacyCommandBlockSchematic(bool command_shell = true,
                                                  bool always_active = false,
                                                  bool conditional = false) {
    std::vector<uint8_t> nbt;
    root(&nbt);
    shortTag(&nbt, "Width", 1);
    shortTag(&nbt, "Height", 1);
    shortTag(&nbt, "Length", 1);
    byteArrayTag(&nbt, "Blocks", {command_shell ? uint8_t(137) : uint8_t(1)});
    byteArrayTag(&nbt, "Data", {conditional ? uint8_t(8) : uint8_t(0)});
    compoundListTag(&nbt, "TileEntities", 1);
    // Bedrock exporters frequently use the legacy no-underscore entity id.
    stringTag(&nbt, "id", "CommandBlock");
    intTag(&nbt, "x", 0);
    intTag(&nbt, "y", 0);
    intTag(&nbt, "z", 0);
    stringTag(&nbt, "Command", "say schematic command");
    stringTag(&nbt, "CustomName", "test command");
    byteTag(&nbt, "TrackOutput", 1);
    byteTag(&nbt, "auto", always_active ? 1 : 0);
    byteTag(&nbt, "conditionalMode", conditional ? 1 : 0);
    intTag(&nbt, "TickDelay", 4);
    end(&nbt);
    end(&nbt);
    return nbt;
}

std::vector<uint8_t> paletteIndexedLegacyBlocks(bool include_palette_max = true,
                                                bool invalid_index = false,
                                                bool include_we_offset = false) {
    std::vector<uint8_t> nbt;
    root(&nbt);
    intTag(&nbt, "Version", 2);
    shortTag(&nbt, "Width", 2);
    shortTag(&nbt, "Height", 1);
    shortTag(&nbt, "Length", 2);
    if (include_we_offset) {
        intTag(&nbt, "WEOffsetX", -5);
        intTag(&nbt, "WEOffsetY", 2);
        intTag(&nbt, "WEOffsetZ", 33);
    }
    if (include_palette_max) intTag(&nbt, "PaletteMax", 3);
    compoundTag(&nbt, "Palette");
    intTag(&nbt, "minecraft:air", 0);
    intTag(&nbt, "minecraft:wool", 1);
    intTag(&nbt, "minecraft:red_concrete_powder", 2);
    end(&nbt);
    // WorldEdit's palette bridge retains this legacy field, but modern block
    // state is entirely described by Palette and Blocks.
    byteArrayTag(&nbt, "Blocks", {0, 1, invalid_index ? uint8_t(3) : uint8_t(2), 1});
    byteArrayTag(&nbt, "Data", {0, 15, 7, 4});
    end(&nbt);
    return nbt;
}

std::vector<uint8_t> spongeV2LargeBlockDataBeforeDimensions() {
    constexpr int16_t kWidth = 300;
    constexpr int16_t kLength = 1000;
    std::vector<uint8_t> nbt;
    root(&nbt);
    compoundTag(&nbt, "Palette");
    intTag(&nbt, "minecraft:stone", 0);
    end(&nbt);
    // Every palette index is one byte, but this payload crosses the parser's
    // streaming-buffer boundary during the replay pass.
    byteArrayTag(&nbt, "BlockData",
                 std::vector<uint8_t>(kWidth * kLength, 0));
    shortTag(&nbt, "Width", kWidth);
    shortTag(&nbt, "Height", 1);
    shortTag(&nbt, "Length", kLength);
    end(&nbt);
    return nbt;
}

uint64_t mixRouteKeyForTest(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

uint32_t routeBucketForTest(const ChunkCoord& coord, ImportPhase phase, uint32_t depth) {
    uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(coord.x)) << 32) |
                   static_cast<uint32_t>(coord.z);
    key ^= static_cast<uint64_t>(static_cast<uint8_t>(phase)) * 0xd6e8feb86659fd93ULL;
    key = mixRouteKeyForTest(
        key ^ (static_cast<uint64_t>(depth) * 0xa0761d6478bd642fULL));
    return static_cast<uint32_t>(key % 32);
}

std::vector<RawBlockRecord> readRawSpool(const std::string& path) {
    std::vector<RawBlockRecord> records;
    for (build_import_test::DecodedRawRecord decoded :
         build_import_test::readRawSpool(path)) {
        RawBlockRecord record;
        record.block = {decoded.x, decoded.y, decoded.z, 0U, decoded.aux,
                        decoded.flags, 0U};
        record.name = std::move(decoded.name);
        records.push_back(std::move(record));
    }
    return records;
}

bool containsRouteTemporaryFile(const std::filesystem::path& directory) {
    if (!std::filesystem::exists(directory)) return false;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.rfind("_route_", 0) == 0 &&
            entry.path().extension() == ".tmp") return true;
    }
    return false;
}

std::vector<ChunkCoord> appendRecursiveRouteFixture(ChunkSpoolWriter* writer,
                                                    std::string* error) {
    constexpr int32_t kChunkSize = 32;
    constexpr int32_t kDirectRouteCount = 193;
    constexpr int32_t kRevisitedRouteCount = 128;
    constexpr size_t kCollidingRouteCount = 97;
    for (int32_t chunk_x = 0; chunk_x < kDirectRouteCount; ++chunk_x) {
        const ParsedBlock block{
            chunk_x * kChunkSize, 64, 0,
            {"minecraft:stone", 0, ImportPhase::Structure, true, false, false}};
        assert(writer->append(block, error));
    }
    assert(!writer->usedStagedRouting());
    for (int32_t chunk_x = 0; chunk_x < kRevisitedRouteCount; ++chunk_x) {
        const ParsedBlock block{
            chunk_x * kChunkSize, 65, 0,
            {"minecraft:stone", 0, ImportPhase::Structure, true, false, false}};
        assert(writer->append(block, error));
    }
    assert(writer->usedStagedRouting());
    assert(writer->reopenedSpoolCount() == kRevisitedRouteCount);

    std::vector<ChunkCoord> colliding;
    colliding.reserve(kCollidingRouteCount);
    for (int32_t chunk_x = -1; colliding.size() < kCollidingRouteCount; --chunk_x) {
        assert(chunk_x > -1000000);
        const ChunkCoord coord{chunk_x, -7};
        if (routeBucketForTest(coord, ImportPhase::Structure, 0) != 0) continue;
        const ParsedBlock block{
            chunk_x * kChunkSize, 70, coord.z * kChunkSize,
            {"minecraft:stone", 0, ImportPhase::Structure, true, false, false}};
        assert(writer->append(block, error));
        colliding.push_back(coord);
    }
    return colliding;
}

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "build_import_schematic_parser_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    std::string error;
    SchematicParseResult result;

    const auto v3_path = directory / "v3.schem";
    writeGzip(v3_path, spongeV3());
    assert(parseFile(v3_path, directory / "v3_spool", &result, &error));
    assert(result.sponge_format);
    assert(result.source_voxel_count == 4 && result.imported_block_count == 4);
    assert(result.chunks.size() == 4);  // X and Z both cross chunk boundaries.
    assert(result.source_volume_bounds.min_x == -33 && result.source_volume_bounds.max_x == -32);
    assert(result.source_volume_bounds.min_z == 31 && result.source_volume_bounds.max_z == 32);
    assert(result.source_offset_x == 0 && result.source_offset_y == 0 &&
           result.source_offset_z == 0);

    // Infinitecz raw-state records are kept losslessly and converted from
    // schematic-local coordinates to the same world space as ordinary blocks.
    const auto raw_extension_path = directory / "infinitecz_raw.schem";
    writeGzip(raw_extension_path, spongeWithInfiniteczRawBlocks());
    error.clear();
    assert(parseFile(raw_extension_path, directory / "infinitecz_raw_spool",
                     &result, &error));
    assert(error.empty());
    assert(result.raw_block_payload_count == 1U && result.raw_blocks.size() == 1U);
    assert(result.omitted_raw_block_count == 0U);
    assert(result.raw_blocks[0].x == -32 && result.raw_blocks[0].y == 64 &&
           result.raw_blocks[0].z == 31);
    assert(result.raw_blocks[0].identifier == "minecraft:lever" &&
           result.raw_blocks[0].aux == 12U && result.raw_blocks[0].legacy_id == 69);
    assert(result.raw_blocks[0].state_json ==
           "{\"face\":\"wall\",\"facing\":\"north\"}");
    assert(result.raw_blocks[0].entity_json == std::string("{}\n"));

    const auto raw_high_aux_path = directory / "infinitecz_raw_high_aux.schem";
    writeGzip(raw_high_aux_path, spongeWithInfiniteczRawBlocks(false, 0xffff));
    error.clear();
    assert(parseFile(raw_high_aux_path, directory / "infinitecz_raw_high_aux_spool",
                     &result, &error));
    assert(error.empty() && result.raw_blocks.size() == 1U &&
           result.raw_blocks[0].aux == 0xffffU);

    std::vector<SchematicRawBlock> streamed_raw_records;
    SchematicParseOptions raw_sink_options;
    raw_sink_options.source_path = raw_extension_path.string();
    raw_sink_options.spool_directory = (directory / "infinitecz_raw_sink_spool").string();
    raw_sink_options.base_x = -33;
    raw_sink_options.base_y = 64;
    raw_sink_options.base_z = 31;
    raw_sink_options.raw_block_sink = [&](const SchematicRawBlock& record, std::string*) {
        streamed_raw_records.push_back(record);
        return true;
    };
    error.clear();
    assert(SchematicParser().parse(raw_sink_options, BlockMapper(), &result, &error));
    assert(error.empty() && result.raw_block_payload_count == 1U);
    assert(result.raw_blocks.empty() && streamed_raw_records.size() == 1U);
    assert(streamed_raw_records[0].x == -32 && streamed_raw_records[0].aux == 12U);

    const auto duplicate_raw_path = directory / "infinitecz_raw_duplicate.schem";
    writeGzip(duplicate_raw_path, spongeWithInfiniteczRawBlocks(true));
    error.clear();
    assert(!parseFile(duplicate_raw_path, directory / "infinitecz_raw_duplicate_spool",
                      &result, &error));
    assert(error == "Infinitecz raw-state records contain duplicate coordinates");

    const auto command_schematic_path = directory / "command_tile_entity.schematic";
    writeGzip(command_schematic_path, legacyCommandBlockSchematic());
    SchematicParseOptions command_options;
    command_options.source_path = command_schematic_path.string();
    command_options.spool_directory = (directory / "command_tile_spool").string();
    command_options.base_x = -33;
    command_options.base_y = 64;
    command_options.base_z = 31;
    command_options.chunk_size = 32;
    std::vector<CommandBlockRecord> command_records;
    command_options.command_block_sink = [&](const CommandBlockRecord& record, std::string*) {
        command_records.push_back(record);
        return true;
    };
    error.clear();
    assert(SchematicParser().parse(command_options, BlockMapper(), &result, &error));
    assert(error.empty());
    assert(result.command_block_payload_count == 1);
    assert(command_records.size() == 1);
    assert(command_records[0].x == -33 && command_records[0].y == 64 &&
           command_records[0].z == 31);
    assert(command_records[0].mode == 0 && command_records[0].redstone_mode);
    assert(command_records[0].output_tracked && command_records[0].tick_delay == 4);
    assert(command_records[0].command == "say schematic command");
    assert(command_records[0].name == "test command");

    // Keep-on and conditional are independent packet settings.  The shell's
    // aux bit is the final authority for the physical conditional state while
    // NBT auto=1 must remain the always-active (non-redstone) setting.
    const auto always_active_path = directory / "always_active_conditional.schematic";
    writeGzip(always_active_path, legacyCommandBlockSchematic(true, true, true));
    command_options.source_path = always_active_path.string();
    command_options.spool_directory = (directory / "always_active_conditional_spool").string();
    command_records.clear();
    error.clear();
    assert(SchematicParser().parse(command_options, BlockMapper(), &result, &error));
    assert(error.empty());
    assert(result.command_block_payload_count == 1);
    assert(command_records.size() == 1);
    assert(!command_records[0].redstone_mode && command_records[0].conditional);

    // A stale legacy Control tile entity must not be sent to the post-import
    // writer when its local block array now contains ordinary stone.
    const auto stale_command_schematic_path = directory / "stale_command_tile_entity.schematic";
    writeGzip(stale_command_schematic_path, legacyCommandBlockSchematic(false));
    command_options.source_path = stale_command_schematic_path.string();
    command_options.spool_directory = (directory / "stale_command_tile_spool").string();
    command_records.clear();
    error.clear();
    assert(SchematicParser().parse(command_options, BlockMapper(), &result, &error));
    assert(error.empty());
    assert(result.command_block_payload_count == 0U);
    assert(result.omitted_command_block_data_count == 1U);
    assert(command_records.empty());

    SchematicParseOptions descriptor_limit_options;
    descriptor_limit_options.source_path = v3_path.string();
    descriptor_limit_options.spool_directory =
        (directory / "descriptor_limit_spool").string();
    descriptor_limit_options.base_x = -33;
    descriptor_limit_options.base_y = 64;
    descriptor_limit_options.base_z = 31;
    descriptor_limit_options.chunk_size = 32;
    descriptor_limit_options.maximum_chunk_descriptors = 3;
    error.clear();
    assert(!SchematicParser().parse(
        descriptor_limit_options, BlockMapper(), &result, &error));
    assert(error == "source volume spans too many logical chunks");

    const auto sink_spool = directory / "sink_spool";
    SchematicParseOptions sink_options;
    sink_options.source_path = v3_path.string();
    sink_options.spool_directory = sink_spool.string();
    sink_options.base_x = -33;
    sink_options.base_y = 64;
    sink_options.base_z = 31;
    sink_options.chunk_size = 32;
    std::vector<ParsedBlock> sink_blocks;
    bool memory_path_created_raw_file = false;
    sink_options.block_sink = [&](const ParsedBlock& block, std::string*) {
        memory_path_created_raw_file = memory_path_created_raw_file ||
            std::filesystem::exists(sink_spool / "_blockdata.raw");
        sink_blocks.push_back(block);
        return true;
    };
    error.clear();
    assert(SchematicParser().parse(
        sink_options, BlockMapper(), &result, &error));
    assert(error.empty());
    assert(result.source_voxel_count == 4 && result.imported_block_count == 4);
    assert(result.chunks.empty() && sink_blocks.size() == 4);
    assert(!memory_path_created_raw_file);
    assert(std::all_of(sink_blocks.begin(), sink_blocks.end(),
                       [](const ParsedBlock& block) {
                           return !block.spec.command_name.empty();
                       }));

    const auto disk_fallback_spool = directory / "disk_fallback_spool";
    SchematicParseOptions disk_fallback_options = sink_options;
    disk_fallback_options.spool_directory = disk_fallback_spool.string();
    disk_fallback_options.block_data_memory_budget_bytes = 0;
    std::vector<ParsedBlock> disk_fallback_blocks;
    bool disk_fallback_created_raw_file = false;
    disk_fallback_options.block_sink =
        [&](const ParsedBlock& block, std::string*) {
            disk_fallback_created_raw_file = disk_fallback_created_raw_file ||
                std::filesystem::exists(
                    disk_fallback_spool / "_blockdata.raw");
            disk_fallback_blocks.push_back(block);
            return true;
        };
    error.clear();
    assert(SchematicParser().parse(
        disk_fallback_options, BlockMapper(), &result, &error));
    assert(error.empty());
    assert(disk_fallback_created_raw_file);
    assert(disk_fallback_blocks.size() == sink_blocks.size());
    for (size_t index = 0; index < sink_blocks.size(); ++index) {
        assert(disk_fallback_blocks[index].world_x ==
               sink_blocks[index].world_x);
        assert(disk_fallback_blocks[index].world_y ==
               sink_blocks[index].world_y);
        assert(disk_fallback_blocks[index].world_z ==
               sink_blocks[index].world_z);
        assert(disk_fallback_blocks[index].spec.command_name ==
               sink_blocks[index].spec.command_name);
        assert(disk_fallback_blocks[index].spec.aux ==
               sink_blocks[index].spec.aux);
    }
    assert(!std::filesystem::exists(
        disk_fallback_spool / "_blockdata.raw"));

    const auto replay_path = directory / "v2_replay.schem";
    writeGzip(replay_path, spongeV2BlockDataBeforeDimensions());
    const auto replay_spool = directory / "v2_replay_spool";
    SchematicParseOptions replay_options = sink_options;
    replay_options.source_path = replay_path.string();
    replay_options.spool_directory = replay_spool.string();
    replay_options.block_data_memory_budget_bytes = 0;
    replay_options.replay_streaming_block_data_threshold_bytes = 1;
    std::vector<ParsedBlock> replay_blocks;
    bool replay_created_raw_file = false;
    replay_options.block_sink = [&](const ParsedBlock& block, std::string*) {
        replay_created_raw_file = replay_created_raw_file ||
            std::filesystem::exists(replay_spool / "_blockdata.raw");
        replay_blocks.push_back(block);
        return true;
    };
    error.clear();
    assert(SchematicParser().parse(replay_options, BlockMapper(), &result, &error));
    assert(error.empty());
    assert(result.sponge_format && result.source_voxel_count == 4);
    assert(replay_blocks.size() == 4 && !replay_created_raw_file);
    assert(replay_blocks.front().world_x == -33 && replay_blocks.front().world_z == 31);
    assert(replay_blocks.back().world_x == -32 && replay_blocks.back().world_z == 32);
    assert(!std::filesystem::exists(replay_spool / "_blockdata.raw"));

    const auto replay_multibuffer_path = directory / "v2_replay_multibuffer.schem";
    writeGzip(replay_multibuffer_path, spongeV2LargeBlockDataBeforeDimensions());
    const auto replay_multibuffer_spool = directory / "v2_replay_multibuffer_spool";
    SchematicParseOptions replay_multibuffer_options = replay_options;
    replay_multibuffer_options.source_path = replay_multibuffer_path.string();
    replay_multibuffer_options.spool_directory = replay_multibuffer_spool.string();
    uint64_t replay_multibuffer_count = 0;
    bool replay_multibuffer_created_raw_file = false;
    replay_multibuffer_options.block_sink = [&](const ParsedBlock&, std::string*) {
        if ((replay_multibuffer_count & 0x3FFF) == 0) {
            replay_multibuffer_created_raw_file = replay_multibuffer_created_raw_file ||
                std::filesystem::exists(replay_multibuffer_spool / "_blockdata.raw");
        }
        ++replay_multibuffer_count;
        return true;
    };
    error.clear();
    assert(SchematicParser().parse(replay_multibuffer_options, BlockMapper(),
                                   &result, &error));
    assert(error.empty());
    assert(replay_multibuffer_count == 300000 &&
           result.imported_block_count == replay_multibuffer_count);
    assert(!replay_multibuffer_created_raw_file);

    std::vector<uint8_t> mcedit;
    root(&mcedit);
    shortTag(&mcedit, "Width", 2);
    shortTag(&mcedit, "Height", 1);
    shortTag(&mcedit, "Length", 1);
    byteArrayTag(&mcedit, "Blocks", {1, 2});
    end(&mcedit);
    const auto mcedit_path = directory / "mcedit.schematic";
    writeGzip(mcedit_path, mcedit);
    assert(parseFile(mcedit_path, directory / "mcedit_spool", &result, &error));
    assert(!result.sponge_format);
    assert(result.source_voxel_count == 2 && result.imported_block_count == 2);

    const auto palette_indexed_path = directory / "palette_indexed_legacy.schematic";
    writeGzip(palette_indexed_path, paletteIndexedLegacyBlocks());
    error.clear();
    assert(parseFile(palette_indexed_path, directory / "palette_indexed_legacy_spool",
                     &result, &error));
    assert(error.empty());
    assert(result.sponge_format);
    assert(result.source_voxel_count == 4 && result.imported_block_count == 3 &&
           result.skipped_block_count == 1);

    SchematicParseOptions palette_indexed_sink_options;
    palette_indexed_sink_options.source_path = palette_indexed_path.string();
    palette_indexed_sink_options.spool_directory =
        (directory / "palette_indexed_legacy_sink_spool").string();
    std::vector<ParsedBlock> palette_indexed_sink_blocks;
    palette_indexed_sink_options.state_block_resolver = [](std::string_view state) {
        BlockMappingResult mapping;
        if (state == "minecraft:air") {
            mapping.status = BlockMappingStatus::Air;
            return mapping;
        }
        mapping.status = BlockMappingStatus::Mapped;
        mapping.spec.command_name = std::string(state);
        return mapping;
    };
    palette_indexed_sink_options.block_sink =
        [&](const ParsedBlock& block, std::string*) {
            palette_indexed_sink_blocks.push_back(block);
            return true;
        };
    error.clear();
    assert(SchematicParser().parse(palette_indexed_sink_options, &result, &error));
    assert(error.empty() && palette_indexed_sink_blocks.size() == 3);
    assert(palette_indexed_sink_blocks[0].spec.command_name == "minecraft:wool");
    assert(palette_indexed_sink_blocks[1].spec.command_name ==
           "minecraft:red_concrete_powder");
    assert(palette_indexed_sink_blocks[2].spec.command_name == "minecraft:wool");

    const auto palette_indexed_offset_path =
        directory / "palette_indexed_legacy_offset.schematic";
    writeGzip(palette_indexed_offset_path,
              paletteIndexedLegacyBlocks(true, false, true));
    error.clear();
    assert(parseFile(palette_indexed_offset_path,
                     directory / "palette_indexed_legacy_offset_spool",
                     &result, &error));
    assert(error.empty());
    assert(result.source_offset_x == -5 && result.source_offset_y == 2 &&
           result.source_offset_z == 33);
    assert(result.source_volume_bounds.min_x == -38 &&
           result.source_volume_bounds.min_y == 66 &&
           result.source_volume_bounds.min_z == 64);

    const auto palette_indexed_invalid_path =
        directory / "palette_indexed_legacy_invalid.schematic";
    writeGzip(palette_indexed_invalid_path, paletteIndexedLegacyBlocks(true, true));
    error.clear();
    assert(!parseFile(palette_indexed_invalid_path,
                      directory / "palette_indexed_legacy_invalid_spool",
                      &result, &error));
    assert(error.find("palette-indexed Blocks array references") != std::string::npos);

    const auto palette_indexed_ambiguous_path =
        directory / "palette_indexed_legacy_ambiguous.schematic";
    writeGzip(palette_indexed_ambiguous_path, paletteIndexedLegacyBlocks(false));
    error.clear();
    assert(!parseFile(palette_indexed_ambiguous_path,
                      directory / "palette_indexed_legacy_ambiguous_spool",
                      &result, &error));
    assert(error == "schematic mixes MCEdit and Sponge block storage");

    std::vector<uint8_t> truncated_mcedit;
    root(&truncated_mcedit);
    shortTag(&truncated_mcedit, "Width", 2);
    shortTag(&truncated_mcedit, "Height", 1);
    shortTag(&truncated_mcedit, "Length", 1);
    u8(&truncated_mcedit, ByteArray);
    name(&truncated_mcedit, "Blocks");
    be32(&truncated_mcedit, 2);
    u8(&truncated_mcedit, 1);
    const auto truncated_mcedit_path = directory / "truncated_mcedit.schematic";
    writeGzip(truncated_mcedit_path, truncated_mcedit);
    error.clear();
    assert(!parseFile(truncated_mcedit_path, directory / "truncated_mcedit_spool",
                      &result, &error));
    assert(error == "truncated MCEdit Blocks array (expected 2 bytes, received 1)");

    const auto corrupt_gzip_path = directory / "corrupt_checksum.schem";
    writeGzip(corrupt_gzip_path, spongeV3());
    {
        std::fstream corrupt_file(corrupt_gzip_path,
                                  std::ios::in | std::ios::out | std::ios::binary);
        assert(corrupt_file);
        corrupt_file.seekg(-1, std::ios::end);
        char byte = 0;
        corrupt_file.read(&byte, 1);
        assert(corrupt_file);
        corrupt_file.seekp(-1, std::ios::end);
        corrupt_file.put(static_cast<char>(byte ^ 0x5A));
        corrupt_file.flush();
        assert(corrupt_file);
    }
    error.clear();
    assert(!parseFile(corrupt_gzip_path, directory / "corrupt_checksum_spool",
                      &result, &error));
    assert(error.rfind("gzip-compressed schematic data is corrupt", 0) == 0);

    // A complete NBT document is still invalid when its gzip trailer is cut
    // off.  gzread() can return zero for that condition, so it must not be
    // mistaken for a clean end-of-stream.
    const auto truncated_gzip_path = directory / "truncated_gzip_trailer.schem";
    writeGzip(truncated_gzip_path, spongeV3());
    const uintmax_t gzip_size = std::filesystem::file_size(truncated_gzip_path);
    assert(gzip_size > 4);
    std::filesystem::resize_file(truncated_gzip_path, gzip_size - 4);
    error.clear();
    assert(!parseFile(truncated_gzip_path, directory / "truncated_gzip_spool",
                      &result, &error));
    assert(error.rfind("gzip-compressed schematic data is corrupt", 0) == 0);

    std::vector<uint8_t> all_air;
    root(&all_air);
    shortTag(&all_air, "Width", 2);
    shortTag(&all_air, "Height", 2);
    shortTag(&all_air, "Length", 1);
    byteArrayTag(&all_air, "Blocks", {0, 0, 0, 0});
    end(&all_air);
    const auto all_air_path = directory / "all_air.schematic";
    writeGzip(all_air_path, all_air);
    assert(parseFile(all_air_path, directory / "all_air_spool", &result, &error));
    assert(result.imported_block_count == 0 && result.skipped_block_count == 4);
    assert(result.unsupported_block_count == 0 && result.chunks.size() == 2);
    for (const ChunkDescriptor& chunk : result.chunks) {
        assert(chunk.imported_bounds.min_y == 64 && chunk.imported_bounds.max_y == 65);
        for (const bool has_phase : chunk.has_phase) assert(!has_phase);
    }

    std::vector<uint8_t> mcedit_offset;
    root(&mcedit_offset);
    shortTag(&mcedit_offset, "Width", 2);
    shortTag(&mcedit_offset, "Height", 1);
    shortTag(&mcedit_offset, "Length", 1);
    intTag(&mcedit_offset, "WEOffsetX", -5);
    intTag(&mcedit_offset, "WEOffsetY", 2);
    intTag(&mcedit_offset, "WEOffsetZ", 33);
    byteArrayTag(&mcedit_offset, "Blocks", {1, 1});
    end(&mcedit_offset);
    const auto mcedit_offset_path = directory / "mcedit_offset.schematic";
    writeGzip(mcedit_offset_path, mcedit_offset);
    assert(parseFile(mcedit_offset_path, directory / "mcedit_offset_spool", &result, &error));
    assert(result.source_offset_x == -5 && result.source_offset_y == 2 &&
           result.source_offset_z == 33);
    assert(result.source_volume_bounds.min_x == -38 && result.source_volume_bounds.max_x == -37);
    assert(result.source_volume_bounds.min_y == 66 && result.source_volume_bounds.max_y == 66);
    assert(result.source_volume_bounds.min_z == 64 && result.source_volume_bounds.max_z == 64);

    const std::vector<int32_t> sponge_offset{5, -2, -31};
    const auto sponge_offset_path = directory / "sponge_offset.schem";
    writeGzip(sponge_offset_path, spongeSingle("minecraft:stone", &sponge_offset));
    assert(parseFile(sponge_offset_path, directory / "sponge_offset_spool", &result, &error));
    assert(result.source_offset_x == 5 && result.source_offset_y == -2 &&
           result.source_offset_z == -31);
    assert(result.source_volume_bounds.min_x == -28 && result.source_volume_bounds.min_y == 62 &&
           result.source_volume_bounds.min_z == 0);

    std::vector<uint8_t> unsupported_legacy;
    root(&unsupported_legacy);
    shortTag(&unsupported_legacy, "Width", 1);
    shortTag(&unsupported_legacy, "Height", 1);
    shortTag(&unsupported_legacy, "Length", 1);
    byteArrayTag(&unsupported_legacy, "Blocks", {36});
    end(&unsupported_legacy);
    const auto unsupported_legacy_path = directory / "unsupported_legacy.schematic";
    writeGzip(unsupported_legacy_path, unsupported_legacy);
    assert(parseFile(unsupported_legacy_path, directory / "unsupported_legacy_spool",
                     &result, &error));
    assert(result.unsupported_block_count == 0 && result.skipped_block_count == 1);

    const auto unsupported_sponge_path = directory / "unsupported_sponge.schem";
    writeGzip(unsupported_sponge_path, spongeSingle("minecraft:future_unknown_block"));
    assert(!parseFile(unsupported_sponge_path, directory / "unsupported_sponge_spool",
                      &result, &error));
    assert(result.unsupported_block_count == 1);
    assert(error.find("minecraft:future_unknown_block") != std::string::npos);

    const auto sponge_air_path = directory / "sponge_air.schem";
    writeGzip(sponge_air_path, spongeSingle("minecraft:air"));
    assert(parseFile(sponge_air_path, directory / "sponge_air_spool", &result, &error));
    assert(result.imported_block_count == 0 && result.skipped_block_count == 1 &&
           result.chunks.size() == 1);

    constexpr int16_t kWideWidth = 3104;
    std::vector<uint8_t> wide;
    root(&wide);
    shortTag(&wide, "Width", kWideWidth);
    shortTag(&wide, "Height", 1);
    shortTag(&wide, "Length", 3);
    byteArrayTag(&wide, "Blocks", std::vector<uint8_t>(kWideWidth * 3, 1));
    end(&wide);
    const auto wide_path = directory / "wide.schematic";
    const auto wide_spool = directory / "wide_spool";
    writeGzip(wide_path, wide);
    std::vector<SchematicParseProgress> wide_progress;
    SchematicParseOptions wide_options;
    wide_options.source_path = wide_path.string();
    wide_options.spool_directory = wide_spool.string();
    wide_options.base_x = -33;
    wide_options.base_y = 64;
    wide_options.base_z = 31;
    wide_options.chunk_size = 32;
    wide_options.progress_callback = [&](const SchematicParseProgress& progress) {
        wide_progress.push_back(progress);
    };
    assert(SchematicParser().parse(wide_options, BlockMapper(), &result, &error));
    assert(!wide_progress.empty());
    assert(wide_progress.front().stage == SchematicParseStage::ReadingSource);
    assert(std::any_of(wide_progress.begin(), wide_progress.end(),
                       [](const SchematicParseProgress& progress) {
                           return progress.stage == SchematicParseStage::RoutingBlocks;
                       }));
    assert(wide_progress.back().stage == SchematicParseStage::FinalizingSpools);
    assert(wide_progress.back().completed == result.imported_block_count);
    assert(wide_progress.back().total == result.imported_block_count);
    assert(result.chunks.size() > 96 && result.imported_block_count == kWideWidth * 3);
    uint64_t routed_records = 0;
    for (const ChunkDescriptor& chunk : result.chunks) {
        const std::string& path = chunk.spool_paths[phaseIndex(ImportPhase::Structure)];
        assert(chunk.has_phase[phaseIndex(ImportPhase::Structure)] && !path.empty());
        assert(std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) > 0);
        routed_records += readRawSpool(path).size();
    }
    assert(routed_records == static_cast<uint64_t>(kWideWidth) * 3);
    for (const auto& entry : std::filesystem::directory_iterator(wide_spool)) {
        assert(entry.path().filename().string().find("_route_") == std::string::npos);
    }

    // Merely crossing the descriptor-cache size must stay on the direct LRU
    // path when every spool is written once in chunk-major order.
    const auto sequential_spool = directory / "sequential_spool";
    {
        ChunkSpoolWriter writer(sequential_spool.string(), 32);
        std::string route_error;
        for (int32_t chunk_x = 0; chunk_x < 256; ++chunk_x) {
            assert(writer.append(
                {chunk_x * 32, 64, 0,
                 {"minecraft:stone", 0, ImportPhase::Structure,
                  true, false, false}},
                &route_error));
        }
        assert(writer.reopenedSpoolCount() == 0);
        assert(!writer.usedStagedRouting());
        const std::vector<ChunkDescriptor> chunks = writer.finish(&route_error);
        assert(route_error.empty() && chunks.size() == 256);
        assert(!writer.usedStagedRouting());
    }

    // Force more than the open-spool limit into one top-level bucket. This
    // covers recursive staged routing and negative chunk coordinates without
    // creating thousands of final files.
    const auto recursive_route_spool = directory / "recursive_route_spool";
    {
        ChunkSpoolWriter writer(recursive_route_spool.string(), 32);
        std::string route_error;
        const std::vector<ChunkCoord> colliding =
            appendRecursiveRouteFixture(&writer, &route_error);
        const std::vector<ChunkDescriptor> chunks = writer.finish(&route_error);
        assert(route_error.empty());
        assert(chunks.size() == 193 + colliding.size());
        size_t negative_chunks = 0;
        for (const ChunkDescriptor& chunk : chunks) {
            if (chunk.coord.z == -7) ++negative_chunks;
            const std::string& path =
                chunk.spool_paths[phaseIndex(ImportPhase::Structure)];
            assert(chunk.has_phase[phaseIndex(ImportPhase::Structure)]);
            assert(std::filesystem::is_regular_file(path));
            assert(readRawSpool(path).size() >= 1);
        }
        assert(negative_chunks == colliding.size());
    }
    for (const auto& entry : std::filesystem::directory_iterator(recursive_route_spool)) {
        assert(entry.path().filename().string().find("_route_") == std::string::npos);
    }

    // Sustained LRU reopen churn switches to staged routing. A later Fluid
    // block then targets the first direct spool; final routing must append,
    // not truncate, and preserve every compact raw field.
    const auto staged_append_spool = directory / "staged_append_spool";
    {
        ChunkSpoolWriter writer(staged_append_spool.string(), 32);
        std::string route_error;
        for (int32_t chunk_x = 0; chunk_x < 193; ++chunk_x) {
            const bool target = chunk_x == 0;
            const ParsedBlock block{
                chunk_x * 32, target ? 41 : 64, 0,
                target
                    ? BlockSpec{"minecraft:water", 3, ImportPhase::Fluid,
                                false, false, true}
                    : BlockSpec{"minecraft:stone", 0, ImportPhase::Structure,
                                true, false, false},
            };
            assert(writer.append(block, &route_error));
        }
        assert(!writer.usedStagedRouting());
        for (int32_t chunk_x = 1; chunk_x <= 128; ++chunk_x) {
            assert(writer.append(
                {chunk_x * 32, 65, 0,
                 {"minecraft:dirt", 1, ImportPhase::Structure,
                  true, false, false}},
                &route_error));
        }
        assert(writer.usedStagedRouting());
        assert(writer.append(
            {31, 42, 31,
             {"minecraft:flowing_water", 7, ImportPhase::Fluid, true, true, false}},
            &route_error));
        assert(containsRouteTemporaryFile(staged_append_spool));

        const std::vector<ChunkDescriptor> chunks = writer.finish(&route_error);
        assert(route_error.empty());
        const auto target = std::find_if(
            chunks.begin(), chunks.end(),
            [](const ChunkDescriptor& chunk) { return chunk.coord == ChunkCoord{0, 0}; });
        assert(target != chunks.end());
        for (size_t phase = 0; phase < kImportPhaseCount; ++phase) {
            assert(target->has_phase[phase] == (phase == phaseIndex(ImportPhase::Fluid)));
        }
        const std::string& target_path =
            target->spool_paths[phaseIndex(ImportPhase::Fluid)];
        assert(!target_path.empty());
        const std::vector<RawBlockRecord> records = readRawSpool(target_path);
        assert(records.size() == 2);
        assert(records[0].block.x == 0 && records[0].block.y == 41 &&
               records[0].block.z == 0 && records[0].block.aux == 3 &&
               records[0].block.flags == 0x0c && records[0].name == "minecraft:water");
        assert(records[1].block.x == 31 && records[1].block.y == 42 &&
               records[1].block.z == 31 && records[1].block.aux == 7 &&
               records[1].block.flags == 0x07 &&
               records[1].name == "minecraft:flowing_water");
        assert(!containsRouteTemporaryFile(staged_append_spool));
    }

    std::vector<uint8_t> wrong_length;
    root(&wrong_length);
    shortTag(&wrong_length, "Width", 2);
    shortTag(&wrong_length, "Height", 1);
    shortTag(&wrong_length, "Length", 1);
    byteArrayTag(&wrong_length, "Blocks", {1});
    end(&wrong_length);
    const auto wrong_length_path = directory / "wrong_length.schematic";
    writeGzip(wrong_length_path, wrong_length);
    assert(!parseFile(wrong_length_path, directory / "wrong_length_spool", &result, &error));
    assert(error == "MCEdit block array length mismatch");

    std::vector<uint8_t> duplicate_dimension;
    root(&duplicate_dimension);
    shortTag(&duplicate_dimension, "Width", 1);
    shortTag(&duplicate_dimension, "Width", 1);
    shortTag(&duplicate_dimension, "Height", 1);
    shortTag(&duplicate_dimension, "Length", 1);
    byteArrayTag(&duplicate_dimension, "Blocks", {1});
    end(&duplicate_dimension);
    const auto duplicate_dimension_path = directory / "duplicate_dimension.schematic";
    writeGzip(duplicate_dimension_path, duplicate_dimension);
    assert(!parseFile(duplicate_dimension_path, directory / "duplicate_spool", &result, &error));

    const auto hole_path = directory / "palette_hole.schem";
    writeGzip(hole_path, spongeV3(true, false));
    assert(!parseFile(hole_path, directory / "hole_spool", &result, &error));
    assert(error.find("palette hole") != std::string::npos);

    const auto duplicate_palette_path = directory / "duplicate_palette.schem";
    writeGzip(duplicate_palette_path, spongeV3(false, true));
    assert(!parseFile(duplicate_palette_path, directory / "duplicate_palette_spool",
                      &result, &error));

    std::vector<uint8_t> trailing = spongeV3();
    trailing.push_back(0x7f);
    const auto trailing_path = directory / "trailing.schem";
    writeGzip(trailing_path, trailing);
    assert(!parseFile(trailing_path, directory / "trailing_spool", &result, &error));
    assert(error == "trailing or corrupt data after NBT root");

    const auto invalid_phase_spool = directory / "invalid_phase_spool";
    {
        ChunkSpoolWriter writer(invalid_phase_spool.string(), 32);
        ParsedBlock block{0, 64, 0,
                          {"minecraft:stone", 0, ImportPhase::Clear, true, false, false}};
        error.clear();
        assert(!writer.append(block, &error));
        assert(error == "invalid block or closed spool writer");
        block.spec.phase = ImportPhase::Count;
        assert(!writer.append(block, &error));
        block.spec.phase = ImportPhase::Structure;
        assert(writer.append(block, &error));
        const std::vector<ChunkDescriptor> chunks = writer.finish(&error);
        assert(chunks.size() == 1);
        assert(chunks[0].has_phase[phaseIndex(ImportPhase::Structure)]);
    }

    const auto cancelled_route_spool = directory / "cancelled_route_spool";
    {
        ChunkSpoolWriter writer(cancelled_route_spool.string(), 32);
        std::string route_error;
        for (int32_t chunk_x = 0; chunk_x < 193; ++chunk_x) {
            ParsedBlock block{chunk_x * 32, 64, 0,
                              {"minecraft:stone", 0, ImportPhase::Structure,
                               true, false, false}};
            assert(writer.append(block, &route_error));
        }
        for (int32_t chunk_x = 0; chunk_x < 128; ++chunk_x) {
            ParsedBlock block{chunk_x * 32, 65, 0,
                              {"minecraft:stone", 0, ImportPhase::Structure,
                               true, false, false}};
            assert(writer.append(block, &route_error));
        }
        assert(writer.usedStagedRouting());
        uint32_t cancellation_checks = 0;
        const std::vector<ChunkDescriptor> chunks = writer.finish(
            &route_error, [&]() { return ++cancellation_checks >= 2; });
        assert(chunks.empty());
        assert(route_error == "import cancelled");
    }
    if (std::filesystem::exists(cancelled_route_spool)) {
        for (const auto& entry : std::filesystem::directory_iterator(cancelled_route_spool)) {
            assert(entry.path().filename().string().find(".bsp") == std::string::npos);
            assert(entry.path().filename().string().find("_route_") == std::string::npos);
        }
    }

    const auto cancelled_recursive_spool = directory / "cancelled_recursive_spool";
    {
        ChunkSpoolWriter writer(cancelled_recursive_spool.string(), 32);
        std::string route_error;
        appendRecursiveRouteFixture(&writer, &route_error);
        uint32_t cancellation_checks = 0;
        const std::vector<ChunkDescriptor> chunks = writer.finish(
            &route_error, [&]() { return ++cancellation_checks >= 3; });
        assert(chunks.empty());
        assert(route_error == "import cancelled");
    }
    if (std::filesystem::exists(cancelled_recursive_spool)) {
        for (const auto& entry : std::filesystem::directory_iterator(cancelled_recursive_spool)) {
            assert(entry.path().filename().string().find(".bsp") == std::string::npos);
            assert(entry.path().filename().string().find("_route_") == std::string::npos);
        }
    }

    std::filesystem::remove_all(directory);
    return 0;
}
