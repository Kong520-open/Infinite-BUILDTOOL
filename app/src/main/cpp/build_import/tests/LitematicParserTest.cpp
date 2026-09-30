#include "../LitematicParser.h"
#include "../CommandSpool.h"
#include "RawSpoolTestReader.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <zlib.h>

using namespace build_import;

namespace {

enum Tag : uint8_t {
    End = 0,
    Byte = 1,
    Double = 6,
    Int = 3,
    String = 8,
    List = 9,
    Compound = 10,
    LongArray = 12,
};

struct RawDiskRecord {
    int32_t x;
    int32_t y;
    int32_t z;
    uint16_t aux;
    uint8_t flags;
    uint16_t name_length;
};

struct PaletteEntry {
    std::string name;
    std::vector<std::pair<std::string, std::string>> properties;
};

void u8(std::vector<uint8_t>* output, uint8_t value) { output->push_back(value); }

void be16(std::vector<uint8_t>* output, uint16_t value) {
    output->push_back(static_cast<uint8_t>(value >> 8U));
    output->push_back(static_cast<uint8_t>(value));
}

void be32(std::vector<uint8_t>* output, uint32_t value) {
    output->push_back(static_cast<uint8_t>(value >> 24U));
    output->push_back(static_cast<uint8_t>(value >> 16U));
    output->push_back(static_cast<uint8_t>(value >> 8U));
    output->push_back(static_cast<uint8_t>(value));
}

void be64(std::vector<uint8_t>* output, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output->push_back(static_cast<uint8_t>(value >> shift));
    }
}

void nbtString(std::vector<uint8_t>* output, const std::string& value) {
    assert(value.size() <= UINT16_MAX);
    be16(output, static_cast<uint16_t>(value.size()));
    output->insert(output->end(), value.begin(), value.end());
}

void namedTag(std::vector<uint8_t>* output, Tag tag, const std::string& name) {
    u8(output, static_cast<uint8_t>(tag));
    nbtString(output, name);
}

void intTag(std::vector<uint8_t>* output, const std::string& name, int32_t value) {
    namedTag(output, Int, name);
    be32(output, static_cast<uint32_t>(value));
}

void stringTag(std::vector<uint8_t>* output, const std::string& name,
               const std::string& value) {
    namedTag(output, String, name);
    nbtString(output, value);
}

void doubleListTag(std::vector<uint8_t>* output, const std::string& name,
                   std::initializer_list<double> values) {
    namedTag(output, List, name);
    u8(output, Double);
    be32(output, static_cast<uint32_t>(values.size()));
    for (const double value : values) {
        uint64_t raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));
        be64(output, raw);
    }
}

void vectorTag(std::vector<uint8_t>* output, const std::string& name,
               int32_t x, int32_t y, int32_t z) {
    namedTag(output, Compound, name);
    intTag(output, "x", x);
    intTag(output, "y", y);
    intTag(output, "z", z);
    u8(output, End);
}

uint32_t paletteBits(size_t palette_size) {
    uint32_t bits = 0;
    size_t capacity = 1;
    while (capacity < palette_size) {
        ++bits;
        capacity <<= 1U;
    }
    return bits < 2 ? 2 : bits;
}

std::vector<uint64_t> pack(const std::vector<uint32_t>& values, size_t palette_size) {
    const uint32_t bits = paletteBits(palette_size);
    std::vector<uint64_t> words((static_cast<uint64_t>(values.size()) * bits + 63U) / 64U);
    for (size_t index = 0; index < values.size(); ++index) {
        const uint64_t bit_index = static_cast<uint64_t>(index) * bits;
        const size_t word = static_cast<size_t>(bit_index / 64U);
        const uint32_t offset = static_cast<uint32_t>(bit_index % 64U);
        words[word] |= static_cast<uint64_t>(values[index]) << offset;
        if (offset + bits > 64U) {
            words[word + 1] |= static_cast<uint64_t>(values[index]) >> (64U - offset);
        }
    }
    return words;
}

void paletteTag(std::vector<uint8_t>* output,
                const std::vector<PaletteEntry>& palette) {
    namedTag(output, List, "BlockStatePalette");
    u8(output, Compound);
    be32(output, static_cast<uint32_t>(palette.size()));
    for (const PaletteEntry& entry : palette) {
        stringTag(output, "Name", entry.name);
        if (!entry.properties.empty()) {
            namedTag(output, Compound, "Properties");
            for (const auto& property : entry.properties) {
                stringTag(output, property.first, property.second);
            }
            u8(output, End);
        }
        u8(output, End);
    }
}

void blockStatesTag(std::vector<uint8_t>* output,
                    const std::vector<uint64_t>& words,
                    int32_t forced_count = -1) {
    namedTag(output, LongArray, "BlockStates");
    const uint32_t count = forced_count < 0 ? static_cast<uint32_t>(words.size())
                                             : static_cast<uint32_t>(forced_count);
    be32(output, count);
    for (uint32_t index = 0; index < count && index < words.size(); ++index) {
        be64(output, words[index]);
    }
}

void ignoredContainerData(std::vector<uint8_t>* output) {
    namedTag(output, List, "TileEntities");
    u8(output, Compound);
    be32(output, 1);
    stringTag(output, "id", "minecraft:red_shulker_box");
    namedTag(output, List, "Items");
    u8(output, Compound);
    be32(output, 1);
    stringTag(output, "id", "minecraft:diamond");
    namedTag(output, Byte, "Count");
    u8(output, 64);
    u8(output, End);
    u8(output, End);
}

void ignoredEntityAndTickData(std::vector<uint8_t>* output) {
    namedTag(output, List, "Entities");
    u8(output, Compound);
    be32(output, 1);
    stringTag(output, "id", "minecraft:item");
    stringTag(output, "Item", "minecraft:diamond");
    u8(output, End);

    namedTag(output, List, "PendingBlockTicks");
    u8(output, Compound);
    be32(output, 1);
    stringTag(output, "Block", "minecraft:stone");
    intTag(output, "Time", 1);
    u8(output, End);

    namedTag(output, List, "PendingFluidTicks");
    u8(output, Compound);
    be32(output, 1);
    stringTag(output, "Fluid", "minecraft:water");
    intTag(output, "Time", 1);
    u8(output, End);
}

void commandBlockEntityData(std::vector<uint8_t>* output, int32_t x, int32_t y,
                            int32_t z, const std::string& command) {
    namedTag(output, List, "TileEntities");
    u8(output, Compound);
    be32(output, 1);
    // Deliberately use Bedrock's legacy no-underscore id: the parser must
    // still obtain chain mode from the actual palette-backed shell here.
    stringTag(output, "id", "CommandBlock");
    intTag(output, "x", x);
    intTag(output, "y", y);
    intTag(output, "z", z);
    stringTag(output, "Command", command);
    stringTag(output, "CustomName", "litematic command");
    namedTag(output, Byte, "TrackOutput");
    u8(output, 1);
    namedTag(output, Byte, "auto");
    u8(output, 0);
    intTag(output, "TickDelay", 6);
    u8(output, End);
}

void deferredContainerAndEntityData(std::vector<uint8_t>* output,
                                    int32_t container_x, int32_t container_y,
                                    int32_t container_z) {
    namedTag(output, List, "TileEntities");
    u8(output, Compound);
    be32(output, 1);
    stringTag(output, "id", "minecraft:red_shulker_box");
    intTag(output, "x", container_x);
    intTag(output, "y", container_y);
    intTag(output, "z", container_z);
    namedTag(output, List, "Items");
    u8(output, Compound);
    be32(output, 1);
    stringTag(output, "id", "minecraft:diamond_sword");
    namedTag(output, Byte, "Count");
    u8(output, 1);
    namedTag(output, Byte, "Slot");
    u8(output, 4);
    namedTag(output, Compound, "tag");
    namedTag(output, List, "Enchantments");
    u8(output, Compound);
    be32(output, 2);
    stringTag(output, "id", "minecraft:sharpness");
    namedTag(output, Byte, "lvl");
    u8(output, 5);
    u8(output, End);
    stringTag(output, "id", "minecraft:sharpness");
    namedTag(output, Byte, "lvl");
    u8(output, 6);  // Illegal: Sharpness is capped at V.
    u8(output, End);
    u8(output, End);  // tag
    u8(output, End);  // item
    u8(output, End);  // TileEntity

    namedTag(output, List, "Entities");
    u8(output, Compound);
    be32(output, 2);
    stringTag(output, "id", "minecraft:sheep");
    doubleListTag(output, "Pos", {0.5, 0.0, 0.5});
    doubleListTag(output, "Rotation", {45.0, 10.0});
    stringTag(output, "CustomName", "region sheep");
    u8(output, End);
    // Item entities intentionally have no recoverable /summon payload.
    stringTag(output, "id", "minecraft:item");
    doubleListTag(output, "Pos", {1.5, 0.0, 1.5});
    u8(output, End);
}

void region(std::vector<uint8_t>* output, const std::string& name,
            int32_t px, int32_t py, int32_t pz,
            int32_t sx, int32_t sy, int32_t sz,
            const std::vector<PaletteEntry>& palette,
             const std::vector<uint32_t>& values,
             bool include_container_data = false,
             int32_t forced_long_count = -1,
             bool block_states_first = true) {
    namedTag(output, Compound, name);
    if (block_states_first) {
        // Deliberately put BlockStates before dimensions and palette to
        // exercise the parser's deferred streaming-replay path.
        blockStatesTag(output, pack(values, palette.size()), forced_long_count);
    }
    if (include_container_data) {
        ignoredContainerData(output);
        ignoredEntityAndTickData(output);
    }
    vectorTag(output, "Position", px, py, pz);
    vectorTag(output, "Size", sx, sy, sz);
    paletteTag(output, palette);
    if (!block_states_first) {
        blockStatesTag(output, pack(values, palette.size()), forced_long_count);
    }
    u8(output, End);
}

std::vector<uint8_t> rootBegin() {
    std::vector<uint8_t> output;
    namedTag(&output, Compound, "Litematic");
    intTag(&output, "Version", 6);
    namedTag(&output, Compound, "Regions");
    return output;
}

void rootEnd(std::vector<uint8_t>* output) {
    u8(output, End);  // Regions
    u8(output, End);  // Root
}

void writeGzip(const std::filesystem::path& path, const std::vector<uint8_t>& nbt) {
    gzFile file = gzopen(path.string().c_str(), "wb9");
    assert(file != nullptr);
    assert(gzwrite(file, nbt.data(), static_cast<unsigned>(nbt.size())) ==
           static_cast<int>(nbt.size()));
    assert(gzclose(file) == Z_OK);
}

bool parseFile(const std::filesystem::path& source,
               const std::filesystem::path& spool,
               SchematicParseResult* result, std::string* error,
               int32_t base_x = -30, int32_t base_y = 64, int32_t base_z = 30) {
    SchematicParseOptions options;
    options.source_path = source.string();
    options.spool_directory = spool.string();
    options.base_x = base_x;
    options.base_y = base_y;
    options.base_z = base_z;
    options.chunk_size = 32;
    return LitematicParser().parse(options, BlockMapper(), result, error);
}

std::vector<RawDiskRecord> readRecords(const SchematicParseResult& result) {
    std::vector<RawDiskRecord> records;
    for (const ChunkDescriptor& chunk : result.chunks) {
        for (size_t phase = 0; phase < kImportPhaseCount; ++phase) {
            if (chunk.spool_paths[phase].empty()) continue;
            for (const build_import_test::DecodedRawRecord& decoded :
                 build_import_test::readRawSpool(chunk.spool_paths[phase])) {
                records.push_back({decoded.x, decoded.y, decoded.z,
                                   decoded.aux, decoded.flags,
                                   static_cast<uint16_t>(decoded.name.size())});
            }
        }
    }
    return records;
}

bool hasRecord(const SchematicParseResult& result, int32_t x, int32_t y, int32_t z,
               const std::string& expected_name, uint16_t expected_aux) {
    for (const ChunkDescriptor& chunk : result.chunks) {
        for (size_t phase = 0; phase < kImportPhaseCount; ++phase) {
            if (chunk.spool_paths[phase].empty()) continue;
            for (const build_import_test::DecodedRawRecord& record :
                 build_import_test::readRawSpool(chunk.spool_paths[phase])) {
                if (record.x == x && record.y == y && record.z == z &&
                    record.aux == expected_aux && record.name == expected_name) return true;
            }
        }
    }
    return false;
}

bool containsChunkSpool(const std::filesystem::path& directory) {
    if (!std::filesystem::exists(directory)) return false;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().find(".bsp") != std::string::npos) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        bool success = true;
        for (int argument = 1; argument < argc; ++argument) {
            const std::filesystem::path source(argv[argument]);
            const std::filesystem::path spool =
                std::filesystem::temp_directory_path() /
                ("litematic_probe_" + std::to_string(argument));
            std::filesystem::remove_all(spool);
            SchematicParseOptions options;
            options.source_path = source.string();
            options.spool_directory = spool.string();
            options.chunk_size = 32;
            SchematicParseResult probe;
            std::string probe_error;
            const bool parsed = LitematicParser().parse(
                options, BlockMapper(), &probe, &probe_error);
            const size_t parsed_chunks = probe.chunks.size();
            const bool optimized = parsed && CommandSpoolBuilder::build(
                spool.string(), 10240, OverwritePolicy::PreserveExisting, 3,
                &probe.chunks, &probe_error, {}, false, {}, true);
            std::printf(
                "%s|ok=%d|optimized=%d|voxels=%llu|blocks=%llu|air=%llu|unsupported=%llu|"
                "degraded=%llu|omitted_entities=%llu|source_regions=%u|parsed_chunks=%zu|optimized_units=%zu|"
                "bounds=%d,%d,%d:%d,%d,%d|error=%s\n",
                source.string().c_str(), parsed ? 1 : 0, optimized ? 1 : 0,
                static_cast<unsigned long long>(probe.source_voxel_count),
                static_cast<unsigned long long>(probe.imported_block_count),
                static_cast<unsigned long long>(probe.skipped_block_count),
                static_cast<unsigned long long>(probe.unsupported_block_count),
                static_cast<unsigned long long>(probe.degraded_block_count),
                static_cast<unsigned long long>(probe.omitted_entity_count),
                probe.source_region_count, parsed_chunks, probe.chunks.size(),
                probe.source_volume_bounds.min_x,
                probe.source_volume_bounds.min_y, probe.source_volume_bounds.min_z,
                probe.source_volume_bounds.max_x, probe.source_volume_bounds.max_y,
                probe.source_volume_bounds.max_z, probe_error.c_str());
            success = success && parsed && optimized;
            std::filesystem::remove_all(spool);
        }
        return success ? 0 : 2;
    }
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "build_import_litematic_parser_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    const std::vector<PaletteEntry> palette{
        {"minecraft:air", {}},
        {"minecraft:stone", {}},
        {"minecraft:oak_stairs", {{"facing", "north"}, {"half", "top"},
                                    {"shape", "straight"}, {"waterlogged", "false"}}},
        {"minecraft:dirt", {}},
        {"minecraft:spruce_planks", {}},
        {"minecraft:chest", {{"facing", "north"}, {"type", "single"},
                              {"waterlogged", "false"}}},
        {"minecraft:powered_rail", {{"powered", "true"},
                                     {"shape", "north_south"},
                                     {"waterlogged", "true"}}},
        {"minecraft:red_shulker_box", {{"facing", "north"}}},
    };
    std::vector<uint32_t> values(40, 1);
    values[0] = 0;
    values[10] = 5;
    values[11] = 6;
    values[12] = 7;
    values[21] = 4;  // Starts at bit 63 and crosses a Long at three bits per value.
    std::vector<uint8_t> basic = rootBegin();
    region(&basic, "main", 0, 0, 0, 40, 1, 1, palette, values, true);
    rootEnd(&basic);
    const auto basic_path = directory / "basic.litematic";
    writeGzip(basic_path, basic);

    std::string error;
    SchematicParseResult result;
    const auto basic_spool = directory / "basic_spool";
    const bool basic_parsed = parseFile(basic_path, basic_spool, &result, &error);
    if (!basic_parsed) std::fprintf(stderr, "basic parse failed: %s\n", error.c_str());
    assert(basic_parsed);
    assert(result.source_voxel_count == 40);
    assert(result.imported_block_count == 39);
    assert(result.skipped_block_count == 1);
    assert(result.unsupported_block_count == 0);
    assert(result.degraded_block_count == 1);
    assert(result.omitted_entity_count == 1);
    assert(result.first_degradation_reason.find("waterlogged rail") != std::string::npos);
    assert(result.source_region_count == 1);

    assert(result.chunks.size() == 2);
    assert(result.source_volume_bounds.min_x == -30);
    assert(result.source_volume_bounds.max_x == 9);
    assert(hasRecord(result, -9, 64, 30, "minecraft:planks", 1));
    assert(hasRecord(result, -20, 64, 30, "minecraft:chest", 2));
    assert(hasRecord(result, -18, 64, 30, "minecraft:red_shulker_box", 2));
    // No deferred sink was supplied, so this legacy block-only parse does not
    // retain the red shulker's item data in the normal placement spools.
    assert(readRecords(result).size() == 39);
    assert(!std::filesystem::exists(basic_spool / "_litematic_blockstates.raw"));

    // Normal Litematica field order lets packed states route directly from
    // the NBT stream. Keep this separate from the fallback-order fixture
    // above so both paths stay covered.
    std::vector<uint8_t> direct = rootBegin();
    region(&direct, "direct", 0, 0, 0, 40, 1, 1, palette, values,
           false, -1, false);
    rootEnd(&direct);
    const auto direct_path = directory / "direct.litematic";
    const auto direct_spool = directory / "direct_spool";
    writeGzip(direct_path, direct);
    error.clear();
    assert(parseFile(direct_path, direct_spool, &result, &error));
    assert(error.empty());
    assert(result.source_voxel_count == 40 && result.imported_block_count == 39);
    assert(result.skipped_block_count == 1 && result.chunks.size() == 2);
    assert(hasRecord(result, -9, 64, 30, "minecraft:planks", 1));
    assert(!std::filesystem::exists(
        direct_spool / "_litematic_blockstates.raw"));

    SchematicParseOptions sink_options;
    sink_options.source_path = basic_path.string();
    sink_options.spool_directory = (directory / "basic_sink_spool").string();
    sink_options.base_x = -30;
    sink_options.base_y = 64;
    sink_options.base_z = 30;
    sink_options.chunk_size = 32;
    std::vector<ParsedBlock> sink_blocks;
    sink_options.block_sink = [&](const ParsedBlock& block, std::string*) {
        sink_blocks.push_back(block);
        return true;
    };
    error.clear();
    assert(LitematicParser().parse(
        sink_options, BlockMapper(), &result, &error));
    assert(error.empty());
    assert(result.source_voxel_count == 40 && result.imported_block_count == 39);
    assert(result.skipped_block_count == 1);
    assert(result.chunks.empty() && sink_blocks.size() == 39);
    assert(std::all_of(sink_blocks.begin(), sink_blocks.end(),
                       [](const ParsedBlock& block) {
                             return !block.spec.command_name.empty();
                       }));

    // Command-block data stays out of normal block output, then is retained
    // at the final world coordinate with mode derived from its chain shell.
    std::vector<uint8_t> command_litematic = rootBegin();
    namedTag(&command_litematic, Compound, "command_region");
    vectorTag(&command_litematic, "Position", 2, 3, 4);
    vectorTag(&command_litematic, "Size", 1, 1, 1);
    // The tile entity intentionally omits conditionalMode. The parser must
    // retain the conditional state from this Java block-state shell instead.
    paletteTag(&command_litematic, {{"minecraft:chain_command_block",
                                     {{"facing", "east"}, {"conditional", "true"}}}});
    blockStatesTag(&command_litematic, pack({0}, 1));
    commandBlockEntityData(&command_litematic, 0, 0, 0, "say litematic command");
    u8(&command_litematic, End);
    rootEnd(&command_litematic);
    const auto command_litematic_path = directory / "command_block.litematic";
    writeGzip(command_litematic_path, command_litematic);
    SchematicParseOptions command_litematic_options;
    command_litematic_options.source_path = command_litematic_path.string();
    command_litematic_options.spool_directory = (directory / "command_block_spool").string();
    command_litematic_options.base_x = -30;
    command_litematic_options.base_y = 64;
    command_litematic_options.base_z = 30;
    command_litematic_options.chunk_size = 32;
    std::vector<CommandBlockRecord> litematic_commands;
    command_litematic_options.command_block_sink =
        [&](const CommandBlockRecord& record, std::string*) {
            litematic_commands.push_back(record);
            return true;
        };
    error.clear();
    assert(LitematicParser().parse(command_litematic_options, BlockMapper(), &result, &error));
    assert(error.empty());
    assert(result.command_block_payload_count == 1U);
    assert(result.omitted_command_block_data_count == 0U);
    assert(litematic_commands.size() == 1U);
    assert(litematic_commands[0].x == -28 && litematic_commands[0].y == 67 &&
           litematic_commands[0].z == 34);
    assert(litematic_commands[0].mode == kCommandBlockModeChain);
    assert(litematic_commands[0].redstone_mode && litematic_commands[0].output_tracked);
    assert(litematic_commands[0].conditional);
    assert(litematic_commands[0].tick_delay == 6);
    assert(litematic_commands[0].command == "say litematic command");

    // Container items and safe basic entities are deferred after the regular
    // block stream.  Negative Size uses bounds.min + local coordinates for all
    // three payload kinds; it must not mirror them around the selection corner.
    std::vector<uint8_t> deferred_data = rootBegin();
    namedTag(&deferred_data, Compound, "negative_deferred");
    vectorTag(&deferred_data, "Position", 4, 5, 6);
    vectorTag(&deferred_data, "Size", -3, 1, -2);
    // A shulker box has a six-way facing state.  Keep this generated fixture
    // semantically valid instead of relying on an omitted palette property
    // that no real Litematica exporter writes.
    paletteTag(&deferred_data, {{"minecraft:stone", {}},
                                {"minecraft:red_shulker_box",
                                 {{"facing", "north"}}}});
    blockStatesTag(&deferred_data, pack({0, 0, 0, 0, 1, 0}, 2));
    deferredContainerAndEntityData(&deferred_data, 1, 0, 1);
    u8(&deferred_data, End);
    rootEnd(&deferred_data);
    const auto deferred_data_path = directory / "deferred_data.litematic";
    writeGzip(deferred_data_path, deferred_data);
    SchematicParseOptions deferred_data_options;
    deferred_data_options.source_path = deferred_data_path.string();
    deferred_data_options.spool_directory = (directory / "deferred_data_spool").string();
    deferred_data_options.base_x = -30;
    deferred_data_options.base_y = 64;
    deferred_data_options.base_z = 30;
    deferred_data_options.chunk_size = 32;
    std::vector<ContainerItemRecord> litematic_container_items;
    std::vector<EntityRecord> litematic_entities;
    deferred_data_options.container_item_sink =
        [&](const ContainerItemRecord& record, std::string*) {
            litematic_container_items.push_back(record);
            return true;
        };
    deferred_data_options.entity_sink = [&](const EntityRecord& record, std::string*) {
        litematic_entities.push_back(record);
        return true;
    };
    error.clear();
    const bool deferred_data_parsed =
        LitematicParser().parse(deferred_data_options, BlockMapper(), &result, &error);
    if (!deferred_data_parsed) {
        std::fprintf(stderr, "deferred-data parse failed: %s\n", error.c_str());
    }
    assert(deferred_data_parsed);
    assert(error.empty());
    assert(result.container_item_payload_count == 1U);
    assert(result.entity_payload_count == 1U);
    assert(result.omitted_entity_count == 1U);
    assert(result.filtered_enchantment_count == 1U);
    assert(litematic_container_items.size() == 1U);
    const ContainerItemRecord& deferred_item = litematic_container_items.front();
    assert(deferred_item.x == -27 && deferred_item.y == 69 && deferred_item.z == 36);
    assert(deferred_item.expected_container_id == "minecraft:red_shulker_box");
    assert(deferred_item.item_id == "minecraft:diamond_sword" &&
           deferred_item.slot == 4 && deferred_item.count == 1);
    assert(deferred_item.enchantments.size() == 1U &&
           deferred_item.enchantments.front().id == "minecraft:sharpness" &&
           deferred_item.enchantments.front().level == 5);
    assert(litematic_entities.size() == 1U);
    assert(litematic_entities.front().entity_id == "minecraft:sheep");
    assert(litematic_entities.front().x == -27.5 && litematic_entities.front().y == 69.0 &&
           litematic_entities.front().z == 35.5);
    assert(litematic_entities.front().yaw == 45.0F && litematic_entities.front().pitch == 10.0F);
    assert(litematic_entities.front().custom_name == "region sheep");

    // A later overlapping region owns the destination even if it replaces the
    // command shell. Its stale TileEntity data must not become a packet write.
    std::vector<uint8_t> overridden_command = rootBegin();
    namedTag(&overridden_command, Compound, "first_command");
    vectorTag(&overridden_command, "Position", 0, 0, 0);
    vectorTag(&overridden_command, "Size", 1, 1, 1);
    paletteTag(&overridden_command, {{"minecraft:command_block", {}}});
    blockStatesTag(&overridden_command, pack({0}, 1));
    commandBlockEntityData(&overridden_command, 0, 0, 0, "say stale command");
    u8(&overridden_command, End);
    region(&overridden_command, "replacement", 0, 0, 0, 1, 1, 1,
           {{"minecraft:dirt", {}}}, {0}, false, -1, false);
    rootEnd(&overridden_command);
    const auto overridden_command_path = directory / "overridden_command.litematic";
    writeGzip(overridden_command_path, overridden_command);
    command_litematic_options.source_path = overridden_command_path.string();
    command_litematic_options.spool_directory = (directory / "overridden_command_spool").string();
    litematic_commands.clear();
    error.clear();
    assert(LitematicParser().parse(command_litematic_options, BlockMapper(), &result, &error));
    assert(error.empty());
    assert(result.command_block_payload_count == 0U);
    assert(result.omitted_command_block_data_count == 1U);
    assert(litematic_commands.empty());

    // The same ownership rule applies to inventories: a later region that
    // replaces the shell must suppress stale TileEntity Items from the older
    // region rather than writing them into whatever now occupies that block.
    std::vector<uint8_t> overridden_container = rootBegin();
    namedTag(&overridden_container, Compound, "first_container");
    vectorTag(&overridden_container, "Position", 0, 0, 0);
    vectorTag(&overridden_container, "Size", 1, 1, 1);
    paletteTag(&overridden_container, {{"minecraft:red_shulker_box", {}}});
    blockStatesTag(&overridden_container, pack({0}, 1));
    deferredContainerAndEntityData(&overridden_container, 0, 0, 0);
    u8(&overridden_container, End);
    region(&overridden_container, "replacement", 0, 0, 0, 1, 1, 1,
           {{"minecraft:stone", {}}}, {0}, false, -1, false);
    rootEnd(&overridden_container);
    const auto overridden_container_path = directory / "overridden_container.litematic";
    writeGzip(overridden_container_path, overridden_container);
    SchematicParseOptions overridden_container_options;
    overridden_container_options.source_path = overridden_container_path.string();
    overridden_container_options.spool_directory =
        (directory / "overridden_container_spool").string();
    overridden_container_options.base_x = -30;
    overridden_container_options.base_y = 64;
    overridden_container_options.base_z = 30;
    overridden_container_options.chunk_size = 32;
    litematic_container_items.clear();
    overridden_container_options.container_item_sink =
        [&](const ContainerItemRecord& record, std::string*) {
            litematic_container_items.push_back(record);
            return true;
        };
    error.clear();
    assert(LitematicParser().parse(overridden_container_options, BlockMapper(), &result, &error));
    assert(error.empty());
    assert(result.container_item_payload_count == 0U);
    assert(result.omitted_block_entity_count == 1U);
    assert(litematic_container_items.empty());

    // Sparse regions routinely use palette index zero for air.  This fixture
    // uses a three-bit palette so zero runs end on non-word-aligned values;
    // the BlockStates-first layout also requires deferred source replay.
    std::vector<uint32_t> sparse_values(256, 0);
    sparse_values[21] = 1;
    sparse_values[64] = 2;
    sparse_values[201] = 3;
    std::vector<uint8_t> sparse_zero = rootBegin();
    region(&sparse_zero, "sparse_zero", 0, 0, 0, 256, 1, 1,
           {{"minecraft:air", {}}, {"minecraft:stone", {}},
            {"minecraft:dirt", {}}, {"minecraft:spruce_planks", {}},
            {"minecraft:cobblestone", {}}},
           sparse_values);
    rootEnd(&sparse_zero);
    const auto sparse_zero_path = directory / "sparse_zero.litematic";
    const auto sparse_zero_spool = directory / "sparse_zero_spool";
    writeGzip(sparse_zero_path, sparse_zero);
    error.clear();
    assert(parseFile(sparse_zero_path, sparse_zero_spool, &result, &error));
    assert(error.empty());
    assert(result.source_voxel_count == 256 && result.imported_block_count == 3);
    assert(result.skipped_block_count == 253);
    assert(hasRecord(result, -9, 64, 30, "minecraft:stone", 0));
    assert(hasRecord(result, 34, 64, 30, "minecraft:dirt", 0));
    assert(hasRecord(result, 171, 64, 30, "minecraft:planks", 1));
    assert(!std::filesystem::exists(
        sparse_zero_spool / "_litematic_blockstates.raw"));

    // Java's flattened stone_bricks name must map to the target-version
    // stonebrick command with its base auxiliary value.
    std::vector<uint8_t> stone_bricks_file = rootBegin();
    region(&stone_bricks_file, "Unnamed", 0, 0, 0, 1, 1, 1,
           {{"minecraft:stone_bricks", {}}}, {0});
    rootEnd(&stone_bricks_file);
    const auto stone_bricks_path = directory / "stone_bricks.litematic";
    const auto stone_bricks_spool = directory / "stone_bricks_spool";
    writeGzip(stone_bricks_path, stone_bricks_file);
    error.clear();
    assert(parseFile(stone_bricks_path, stone_bricks_spool, &result, &error));
    assert(error.empty());
    assert(result.unsupported_block_count == 0 && result.imported_block_count == 1);
    assert(hasRecord(result, -30, 64, 30, "minecraft:stonebrick", 0));

    std::vector<uint8_t> excessive_chunks = rootBegin();
    namedTag(&excessive_chunks, Compound, "wide");
    blockStatesTag(&excessive_chunks, std::vector<uint64_t>(65537, 0));
    vectorTag(&excessive_chunks, "Position", 0, 0, 0);
    vectorTag(&excessive_chunks, "Size", 32 * 65536 + 1, 1, 1);
    paletteTag(&excessive_chunks, {{"minecraft:air", {}}});
    u8(&excessive_chunks, End);
    rootEnd(&excessive_chunks);
    const auto excessive_path = directory / "excessive_chunks.litematic";
    writeGzip(excessive_path, excessive_chunks);
    const auto excessive_spool = directory / "excessive_chunk_spool";
    error.clear();
    assert(!parseFile(excessive_path, excessive_spool, &result, &error));
    assert(error.find("too many logical chunks") != std::string::npos);
    assert(!std::filesystem::exists(
        excessive_spool / "_litematic_blockstates.raw"));
    assert(!containsChunkSpool(excessive_spool));

    std::vector<uint8_t> negative = rootBegin();
    std::vector<uint32_t> negative_values(12, 0);
    negative_values[1] = 1;
    negative_values[3] = 2;
    negative_values[6] = 3;
    negative_values.back() = 4;
    region(&negative, "negative", 1, 2, -1, -3, 2, -2,
           {{"minecraft:stone", {}}, {"minecraft:gold_block", {}},
            {"minecraft:diamond_block", {}}, {"minecraft:emerald_block", {}},
            {"minecraft:iron_block", {}}}, negative_values);
    region(&negative, "second", 35, 0, 35, 1, 1, 1,
           {{"minecraft:dirt", {}}}, {0});
    region(&negative, "negative_y", 20, 10, 20, 1, -2, 1,
           {{"minecraft:stone", {}}, {"minecraft:gold_block", {}}}, {0, 1});
    rootEnd(&negative);
    const auto negative_path = directory / "negative.litematic";
    writeGzip(negative_path, negative);
    assert(parseFile(negative_path, directory / "negative_spool", &result, &error));
    assert(result.source_voxel_count == 15 && result.imported_block_count == 15);
    assert(result.source_region_count == 3);
    assert(result.source_volume_bounds.min_x == -31);
    assert(result.source_volume_bounds.max_x == 5);
    assert(result.source_volume_bounds.min_y == 64);
    assert(result.source_volume_bounds.max_y == 74);
    assert(result.source_volume_bounds.min_z == 28);
    assert(result.source_volume_bounds.max_z == 65);
    assert(result.source_offset_x == -1);
    assert(result.source_offset_y == 0);
    assert(result.source_offset_z == -2);
    bool saw_negative_corner = false;
    for (const RawDiskRecord& record : readRecords(result)) {
        if (record.x == -31 && record.y == 67 && record.z == 28) {
            saw_negative_corner = true;
        }
    }
    assert(saw_negative_corner);
    // Signed region sizes select the opposite corner, but the packed block
    // array itself always starts at the minimum X/Y/Z corner.
    assert(hasRecord(result, -31, 66, 28, "minecraft:stone", 0));
    assert(hasRecord(result, -30, 66, 28, "minecraft:gold_block", 0));
    assert(hasRecord(result, -31, 66, 29, "minecraft:diamond_block", 0));
    assert(hasRecord(result, -31, 67, 28, "minecraft:emerald_block", 0));
    assert(hasRecord(result, -29, 67, 29, "minecraft:iron_block", 0));
    assert(hasRecord(result, -10, 73, 50, "minecraft:stone", 0));
    assert(hasRecord(result, -10, 74, 50, "minecraft:gold_block", 0));

    std::vector<uint8_t> bad_index = rootBegin();
    region(&bad_index, "bad", 0, 0, 0, 2, 1, 1,
           {{"minecraft:air", {}}, {"minecraft:stone", {}}}, {1, 3});
    rootEnd(&bad_index);
    const auto bad_index_path = directory / "bad_index.litematic";
    writeGzip(bad_index_path, bad_index);
    const auto bad_index_spool = directory / "bad_index_spool";
    assert(!parseFile(bad_index_path, bad_index_spool, &result, &error));
    assert(error.find("palette index") != std::string::npos);
    assert(!std::filesystem::exists(
        bad_index_spool / "_litematic_blockstates.raw"));
    assert(!containsChunkSpool(bad_index_spool));

    std::vector<uint8_t> truncated = rootBegin();
    region(&truncated, "truncated", 0, 0, 0, 64, 1, 1,
           {{"minecraft:stone", {}}}, std::vector<uint32_t>(64, 0), false, 1);
    rootEnd(&truncated);
    const auto truncated_path = directory / "truncated.litematic";
    writeGzip(truncated_path, truncated);
    const auto truncated_spool = directory / "truncated_spool";
    assert(!parseFile(truncated_path, truncated_spool, &result, &error));
    assert(error.find("length") != std::string::npos);
    assert(!std::filesystem::exists(
        truncated_spool / "_litematic_blockstates.raw"));

    std::vector<uint8_t> nonzero_padding = rootBegin();
    namedTag(&nonzero_padding, Compound, "padding");
    blockStatesTag(&nonzero_padding, {uint64_t{1} << 63U});
    vectorTag(&nonzero_padding, "Position", 0, 0, 0);
    vectorTag(&nonzero_padding, "Size", 1, 1, 1);
    paletteTag(&nonzero_padding, {{"minecraft:air", {}}});
    u8(&nonzero_padding, End);
    rootEnd(&nonzero_padding);
    const auto padding_path = directory / "nonzero_padding.litematic";
    writeGzip(padding_path, nonzero_padding);
    const auto padding_spool = directory / "nonzero_padding_spool";
    assert(!parseFile(padding_path, padding_spool, &result, &error));
    assert(error.find("padding") != std::string::npos);
    assert(!std::filesystem::exists(
        padding_spool / "_litematic_blockstates.raw"));
    assert(!containsChunkSpool(padding_spool));

    std::vector<uint8_t> oversized_states = rootBegin();
    region(&oversized_states, "oversized", 0, 0, 0, 1, 1, 1,
           {{"minecraft:stone", {}}}, {0}, false, 536870913);
    rootEnd(&oversized_states);
    const auto oversized_path = directory / "oversized_states.litematic";
    writeGzip(oversized_path, oversized_states);
    const auto oversized_spool = directory / "oversized_states_spool";
    assert(!parseFile(oversized_path, oversized_spool, &result, &error));
    assert(error.find("4096 MiB") != std::string::npos);
    assert(!std::filesystem::exists(
        oversized_spool / "_litematic_blockstates.raw"));

    std::vector<uint8_t> offset_overflow = rootBegin();
    region(&offset_overflow, "offset", std::numeric_limits<int32_t>::min(), 0, 0,
           -2, 1, 1, {{"minecraft:stone", {}}}, {0, 0});
    rootEnd(&offset_overflow);
    const auto offset_path = directory / "offset_overflow.litematic";
    writeGzip(offset_path, offset_overflow);
    const auto offset_spool = directory / "offset_overflow_spool";
    assert(!parseFile(offset_path, offset_spool, &result, &error,
                      std::numeric_limits<int32_t>::max(), 64, 30));
    assert(error.find("source offset") != std::string::npos);
    assert(!std::filesystem::exists(
        offset_spool / "_litematic_blockstates.raw"));
    assert(!containsChunkSpool(offset_spool));

    std::vector<uint8_t> overlapping = rootBegin();
    region(&overlapping, "first", 0, 0, 0, 1, 1, 1,
           {{"minecraft:stone", {}}}, {0});
    region(&overlapping, "second", 0, 0, 0, 1, 1, 1,
           {{"minecraft:dirt", {}}}, {0});
    rootEnd(&overlapping);
    const auto overlapping_path = directory / "overlapping.litematic";
    writeGzip(overlapping_path, overlapping);
    const auto overlapping_spool = directory / "overlapping_spool";
    error.clear();
    assert(parseFile(overlapping_path, overlapping_spool, &result, &error));
    assert(error.empty());
    assert(result.source_voxel_count == 2 && result.source_region_count == 2);
    assert(result.imported_block_count == 1 && result.skipped_block_count == 1);
    assert(readRecords(result).size() == 1);
    assert(hasRecord(result, -30, 64, 30, "minecraft:dirt", 0));
    assert(!hasRecord(result, -30, 64, 30, "minecraft:stone", 0));
    assert(!std::filesystem::exists(
        overlapping_spool / "_litematic_blockstates.raw"));

    SchematicParseOptions overlap_sink_options;
    overlap_sink_options.source_path = overlapping_path.string();
    overlap_sink_options.spool_directory = (directory / "overlap_sink_spool").string();
    overlap_sink_options.base_x = -30;
    overlap_sink_options.base_y = 64;
    overlap_sink_options.base_z = 30;
    overlap_sink_options.chunk_size = 32;
    std::vector<ParsedBlock> overlap_sink_blocks;
    overlap_sink_options.block_sink = [&](const ParsedBlock& block, std::string*) {
        overlap_sink_blocks.push_back(block);
        return true;
    };
    error.clear();
    assert(LitematicParser().parse(
        overlap_sink_options, BlockMapper(), &result, &error));
    assert(error.empty() && overlap_sink_blocks.size() == 1);
    assert(overlap_sink_blocks[0].world_x == -30 &&
           overlap_sink_blocks[0].world_y == 64 &&
           overlap_sink_blocks[0].world_z == 30 &&
           overlap_sink_blocks[0].spec.command_name == "minecraft:dirt");

    // Every cell in the later region owns the final composition.  Its air
    // therefore removes the earlier record instead of leaving a duplicate for
    // CommandSpoolBuilder to reject.
    std::vector<uint8_t> overlap_air = rootBegin();
    region(&overlap_air, "first", 0, 0, 0, 1, 1, 1,
           {{"minecraft:stone", {}}}, {0});
    region(&overlap_air, "second", 0, 0, 0, 1, 1, 1,
           {{"minecraft:air", {}}}, {0});
    rootEnd(&overlap_air);
    const auto overlap_air_path = directory / "overlap_air.litematic";
    const auto overlap_air_spool = directory / "overlap_air_spool";
    writeGzip(overlap_air_path, overlap_air);
    error.clear();
    assert(parseFile(overlap_air_path, overlap_air_spool, &result, &error));
    assert(error.empty());
    assert(result.source_voxel_count == 2 && result.source_region_count == 2);
    assert(result.imported_block_count == 0 && result.skipped_block_count == 2);
    assert(result.degraded_block_count == 0);
    assert(readRecords(result).empty());
    assert(!std::filesystem::exists(
        overlap_air_spool / "_litematic_blockstates.raw"));

    // A Java-only state may be safely skipped, but it must remain visible in
    // the parse result instead of looking like an intentional palette-air.
    std::vector<uint8_t> degraded_air = rootBegin();
    region(&degraded_air, "cactus_flower", 0, 0, 0, 1, 1, 1,
           {{"minecraft:cactus_flower", {}}}, {0});
    rootEnd(&degraded_air);
    const auto degraded_air_path = directory / "degraded_air.litematic";
    const auto degraded_air_spool = directory / "degraded_air_spool";
    writeGzip(degraded_air_path, degraded_air);
    error.clear();
    assert(parseFile(degraded_air_path, degraded_air_spool, &result, &error));
    assert(error.empty());
    assert(result.imported_block_count == 0 && result.skipped_block_count == 1);
    assert(result.degraded_block_count == 1);
    assert(result.first_degradation_reason.find("cactus flower") != std::string::npos);

    // Overlap is per voxel rather than per region: the untouched part of the
    // earlier region remains in the output and only the intersecting cell is
    // replaced by the later region.
    std::vector<uint8_t> partial_overlap = rootBegin();
    region(&partial_overlap, "base", 0, 0, 0, 2, 1, 1,
           {{"minecraft:stone", {}}, {"minecraft:gold_block", {}}}, {0, 1});
    region(&partial_overlap, "replacement", 1, 0, 0, 1, 1, 1,
           {{"minecraft:dirt", {}}}, {0});
    rootEnd(&partial_overlap);
    const auto partial_overlap_path = directory / "partial_overlap.litematic";
    const auto partial_overlap_spool = directory / "partial_overlap_spool";
    writeGzip(partial_overlap_path, partial_overlap);
    error.clear();
    assert(parseFile(partial_overlap_path, partial_overlap_spool, &result, &error));
    assert(error.empty());
    assert(result.imported_block_count == 2 && result.skipped_block_count == 1);
    assert(readRecords(result).size() == 2);
    assert(hasRecord(result, -30, 64, 30, "minecraft:stone", 0));
    assert(hasRecord(result, -29, 64, 30, "minecraft:dirt", 0));
    assert(!hasRecord(result, -29, 64, 30, "minecraft:gold_block", 0));

    std::filesystem::remove_all(directory);
    return 0;
}
