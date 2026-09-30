#include "../BdxParser.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace build_import;

namespace {

namespace fs = std::filesystem;

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt != 100; ++attempt) {
            path_ = fs::temp_directory_path() /
                    ("build_import_bdx_parser_test_" + std::to_string(nonce) + "_" +
                     std::to_string(attempt));
            std::error_code error;
            if (fs::create_directory(path_, error)) return;
        }
        throw std::runtime_error("cannot create BDX parser test directory");
    }

    ~ScopedTempDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    fs::path child(const std::string& name) const { return path_ / name; }

private:
    fs::path path_;
};

// A complete BDX stream generated once with Node's Brotli encoder and kept as
// bytes rather than depending on an encoder in the native test target.  Its
// decoded command stream contains the following sequence:
//   concrete(red) at (0,0,0), +X,
//   repeating command block with an NBT Command payload at (1,0,0), +X,
//   stone at (2,0,0), End.
// The parser must retain the inert command-block shell, skip the NBT payload
// (including its command text), and continue to the later normal block.
constexpr std::array<uint8_t, 156> kFixtureBdx = {
    66, 68, 64, 27, 212, 0, 0, 196, 138, 181, 237, 11, 162, 145, 204, 92,
    195, 18, 205, 164, 4, 252, 94, 68, 221, 224, 192, 225, 116, 192, 1, 183,
    151, 129, 6, 157, 112, 222, 214, 119, 27, 59, 207, 82, 176, 102, 72, 219,
    237, 241, 68, 25, 185, 197, 166, 48, 243, 2, 194, 76, 29, 244, 76, 181,
    138, 157, 30, 71, 122, 102, 22, 38, 132, 15, 241, 52, 116, 164, 204, 52,
    232, 7, 132, 222, 209, 141, 108, 214, 227, 48, 84, 35, 131, 101, 160,
    142, 163, 8, 29, 175, 6, 172, 185, 5, 133, 153, 200, 154, 172, 151, 253,
    18, 159, 188, 84, 58, 110, 219, 146, 15, 127, 8, 159, 31, 58, 0, 132,
    233, 2, 34, 36, 72, 67, 160, 223, 195, 157, 201, 204, 144, 171, 237,
    220, 55, 229, 40, 46, 120, 249, 2, 166, 29, 200, 232, 60, 1,
};

// The first placement uses a state-palette opcode with an explicitly empty
// state array. The second uses legacy PlaceBlock with the same identifier and
// aux=0. They must not share a mapping-cache entry: the latter carries an
// explicit native aux even though its numeric value is zero.
constexpr std::array<uint8_t, 55> kLegacyZeroAuxFixtureBdx = {
    66, 68, 64, 139, 23, 128, 66, 68, 88, 0, 102, 105, 120, 116,
    117, 114, 101, 0, 1, 109, 105, 110, 101, 99, 114, 97, 102, 116,
    58, 115, 116, 111, 110, 101, 95, 115, 108, 97, 98, 0, 1, 0,
    5, 0, 0, 0, 1, 14, 7, 0, 0, 0, 0, 88, 3,
};

// Decoded stream: a legacy, unqualified `seaLantern` palette identifier is
// placed with the old PlaceBlock opcode.  Older BDX writers use this camel
// case spelling; it must normalize to the safe target identifier
// minecraft:sea_lantern rather than being rejected as an unsafe name.
constexpr std::array<uint8_t, 35> kLegacyCamelCaseFixtureBdx = {
    66, 68, 64, 33, 104, 0, 4, 66, 68, 88, 0, 116, 101, 115, 116, 0,
    1, 115, 101, 97, 76, 97, 110, 116, 101, 114, 110, 0, 7, 0, 0, 0,
    0, 88, 3,
};

// Decoded stream: an older BDX writer omits the default namespace entirely
// and emits `stone` in the palette.  This is the exact compatibility class
// exercised by legacy BDX exports such as 雪球菜单及玩家互传.bdx.
// It must become the normal target command identifier minecraft:stone.
constexpr std::array<uint8_t, 30> kLegacyUnqualifiedStoneFixtureBdx = {
    66, 68, 64, 33, 84, 0, 4, 66, 68, 88, 0, 116, 101, 115, 116, 0,
    1, 115, 116, 111, 110, 101, 0, 7, 0, 0, 0, 0, 88, 3,
};

// Decoded stream: PlaceBlockWithChestData for a chest at (0,0,0).  It has two
// valid items and one invalid identifier, which proves that compact BDX chest
// data reaches the deferred item sink while malformed entries are left out.
constexpr std::array<uint8_t, 93> kChestFixtureBdx = {
    66, 68, 64, 27, 87, 0, 96, 29, 9, 54, 78, 114, 51, 215, 216, 195,
    8, 149, 14, 79, 222, 254, 109, 72, 65, 169, 162, 41, 65, 51, 56, 83,
    8, 220, 223, 126, 6, 160, 0, 85, 88, 26, 108, 192, 129, 14, 232, 219,
    24, 224, 78, 179, 156, 160, 17, 44, 67, 167, 212, 35, 166, 204, 252,
    235, 119, 5, 251, 145, 126, 235, 212, 208, 151, 60, 37, 0, 115, 55,
    229, 53, 6, 96, 128, 83, 137, 107, 155, 113, 69, 226, 9, 151, 1,
};

// A legacy BDX exporter variant omits the final `needRedstone` byte from its
// terminal SetCommandBlockData (opcode 26) record, then writes the normal End
// opcode immediately afterwards.  The parser must preserve the command block
// with the documented redstone-mode default and still consume the End marker.
// A genuinely truncated record remains rejected by testTruncatedStreamIsRejected.
constexpr std::array<uint8_t, 67> kTerminalThreeFlagCommandFixtureBdx = {
    66, 68, 64,
    31, 60, 0, 0, 4, 34, 111, 247, 73, 232, 146, 148, 77, 9, 223, 236,
    135, 96, 243, 4, 10, 112, 200, 137, 248, 119, 75, 226, 128, 52, 216, 24,
    195, 33, 56, 102, 227, 117, 228, 162, 188, 113, 31, 210, 142, 187, 237, 94,
    135, 150, 0, 16, 205, 181, 159, 238, 4, 157, 219, 22, 232, 131, 16, 20,
};

// Decoded stream: two legacy command-block records omit their fourth
// needRedstone flag, and each is immediately followed by AddXValue0 (opcode
// 14).  The first omitted field proves the parser detects the old dialect;
// the second proves it stays in that dialect rather than swallowing another
// movement opcode.  The final stone must therefore land at X=2.
constexpr std::array<uint8_t, 91> kLegacyMissingNeedRedstoneFixtureBdx = {
    66, 68, 64, 27, 146, 0, 24, 134, 231, 105, 231, 251, 73, 214, 121, 20,
    72, 34, 187, 9, 100, 221, 104, 55, 153, 140, 166, 39, 235, 66, 173, 158,
    32, 24, 33, 153, 16, 181, 36, 86, 122, 178, 36, 41, 10, 147, 181, 103,
    73, 99, 242, 231, 56, 151, 145, 204, 197, 20, 150, 111, 186, 110, 88,
    210, 62, 159, 247, 206, 124, 250, 169, 208, 81, 190, 235, 31, 192, 56,
    152, 149, 180, 42, 5, 5, 15, 46, 64, 3, 233, 194, 2,
};

// Decoded stream: two command-block payloads contain a non-canonical byte 14
// in the needRedstone slot, immediately followed by the real AddXValue0
// opcode 14.  This is the exact byte-shape used by mm_起风了.bdx: the first
// byte must be consumed solely to retain stream alignment, while command-block
// redstone behavior must still use the conservative missing-field fallback.
// The two shells therefore land at X=0 and X=1, never X=0 and X=2.
constexpr std::array<uint8_t, 70> kNonCanonicalNeedRedstonePlaceholderFixtureBdx = {
    66, 68, 64, 27, 91, 0, 248, 135, 160, 185, 157, 201, 54, 186, 201, 146,
    141, 98, 9, 107, 50, 100, 130, 193, 13, 244, 65, 132, 13, 56, 16, 13,
    185, 192, 121, 151, 147, 5, 47, 116, 153, 214, 45, 139, 250, 134, 225, 190,
    90, 61, 158, 114, 198, 242, 183, 119, 98, 132, 9, 222, 242, 207, 189, 85,
    0, 82, 194, 248, 186, 25,
};

// Decoded stream: a three-flag command payload is immediately followed by
// CreateConstantString (opcode 1).  Opcode 1 collides with boolean true, so
// this fixture proves the parser keeps that opcode when the following byte is
// plainly string data and does not desynchronize the palette or coordinates.
constexpr std::array<uint8_t, 75> kLegacyMissingNeedRedstoneBeforeStringFixtureBdx = {
    66, 68, 64, 27, 88, 0, 96, 7, 34, 111, 39, 125, 179, 46, 69, 209,
    148, 240, 205, 126, 198, 38, 16, 248, 194, 41, 103, 16, 80, 0, 28, 6,
    210, 242, 8, 14, 2, 135, 13, 56, 178, 160, 225, 249, 87, 169, 138, 143,
    231, 132, 189, 206, 232, 215, 166, 231, 45, 221, 114, 24, 1, 72, 227, 217,
    191, 99, 0, 224, 8, 234, 45, 68, 129, 130, 2,
};

// Pool 117 is the fixed NetEase 1.17 runtime-id palette. Each fixture places
// stone, a conditional chain command block (keep-on, no redstone), then a
// chest with one item. Together they cover every runtime placement opcode:
// 32/34/37 use uint16 IDs and 33/35/38 use uint32 IDs.
constexpr std::array<uint8_t, 78> kRuntimeIdFixtureBdx = {
    66, 68, 64, 27, 76, 0, 248, 197, 255, 220, 246, 254, 110, 121, 25, 243,
    18, 160, 113, 148, 176, 138, 136, 193, 68, 14, 28, 2, 105, 1, 235, 193,
    159, 14, 172, 64, 203, 33, 205, 36, 174, 250, 188, 96, 223, 142, 174, 8,
    143, 71, 0, 216, 153, 255, 206, 113, 47, 87, 63, 215, 128, 133, 64, 16,
    33, 79, 104, 238, 151, 186, 60, 242, 230, 138, 159, 96, 197, 37,
};

constexpr std::array<uint8_t, 75> kRuntimeId32FixtureBdx = {
    66, 68, 64, 27, 82, 0, 248, 7, 2, 217, 214, 33, 153, 172, 82, 2,
    52, 70, 9, 84, 215, 151, 86, 36, 146, 141, 76, 146, 14, 148, 32, 203,
    148, 229, 5, 79, 123, 63, 56, 175, 7, 134, 170, 4, 32, 199, 128, 120,
    151, 127, 247, 122, 183, 103, 92, 91, 4, 0, 4, 4, 148, 24, 228, 84,
    88, 199, 173, 173, 175, 178, 123, 146, 41, 98, 20,
};

void writeBytes(const fs::path& path, const uint8_t* bytes, size_t length) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(length));
    assert(output);
}

bool parseWithSink(const fs::path& source, SchematicParseResult* result,
                   std::vector<ParsedBlock>* blocks,
                   std::vector<CommandBlockRecord>* command_blocks,
                   std::string* error,
                   std::vector<ContainerItemRecord>* container_items = nullptr) {
    SchematicParseOptions options;
    options.source_path = source.string();
    options.base_x = 100;
    options.base_y = 64;
    options.base_z = -40;
    options.chunk_size = 32;
    options.block_sink = [blocks](const ParsedBlock& block, std::string*) {
        blocks->push_back(block);
        return true;
    };
    if (command_blocks) {
        options.command_block_sink = [command_blocks](const CommandBlockRecord& record,
                                                       std::string*) {
            command_blocks->push_back(record);
            return true;
        };
    }
    if (container_items) {
        options.container_item_sink = [container_items](const ContainerItemRecord& record,
                                                         std::string*) {
            container_items->push_back(record);
            return true;
        };
    }
    return BdxParser().parse(options, BlockMapper(), result, error);
}

void testBedrockStatesNbtSkippingAndCoordinates(const ScopedTempDirectory& temporary) {
    const fs::path source = temporary.child("payload_and_followup.bdx");
    writeBytes(source, kFixtureBdx.data(), kFixtureBdx.size());

    SchematicParseResult result;
    std::vector<ParsedBlock> blocks;
    std::vector<CommandBlockRecord> command_blocks;
    std::string error;
    assert(parseWithSink(source, &result, &blocks, &command_blocks, &error));
    assert(error.empty());

    assert(result.imported_block_count == 3);
    assert(result.skipped_block_count == 0);
    assert(result.unsupported_block_count == 0);
    assert(result.omitted_block_entity_count == 0);
    assert(result.command_block_payload_count == 1);
    assert(result.omitted_command_block_data_count == 0);
    assert(result.source_region_count == 1);
    assert(result.source_voxel_count == 3);
    assert(result.source_volume_bounds.min_x == 100 && result.source_volume_bounds.max_x == 102);
    assert(result.source_volume_bounds.min_y == 64 && result.source_volume_bounds.max_y == 64);
    assert(result.source_volume_bounds.min_z == -40 && result.source_volume_bounds.max_z == -40);
    assert(result.chunks.empty());  // The block sink intentionally bypasses spool creation.

    assert(blocks.size() == 3);
    assert(blocks[0].world_x == 100 && blocks[0].world_y == 64 && blocks[0].world_z == -40);
    assert(blocks[0].spec.command_name == "minecraft:concrete");
    assert(blocks[1].world_x == 101 && blocks[1].world_y == 64 && blocks[1].world_z == -40);
    assert(blocks[1].spec.command_name == "minecraft:repeating_command_block");
    assert(blocks[1].spec.stateful);
    assert(!blocks[1].spec.can_fill);
    assert(blocks[2].world_x == 102 && blocks[2].world_y == 64 && blocks[2].world_z == -40);
    assert(blocks[2].spec.command_name == "minecraft:stone");
    assert(command_blocks.size() == 1);
    assert(command_blocks[0].x == 101 && command_blocks[0].y == 64 &&
           command_blocks[0].z == -40);
    assert(command_blocks[0].command.find("should_not_execute") != std::string::npos);
    for (const ParsedBlock& block : blocks) {
        // The literal is deliberately present only in the fixture's skipped
        // NBT. A block specification must never contain command payload text.
        assert(block.spec.command_name.find("should_not_execute") == std::string::npos);
    }
}

// Inspection and compatibility callers are allowed to omit the deferred
// command-block sink.  The parser must still consume the BDX payload and keep
// routing the normal block stream; only the post-import metadata is omitted.
void testCommandBlockPayloadCanBeDeliberatelyOmitted(const ScopedTempDirectory& temporary) {
    const fs::path source = temporary.child("payload_without_command_sink.bdx");
    writeBytes(source, kFixtureBdx.data(), kFixtureBdx.size());

    SchematicParseResult result;
    std::vector<ParsedBlock> blocks;
    std::string error;
    assert(parseWithSink(source, &result, &blocks, nullptr, &error));
    assert(error.empty());
    assert(result.imported_block_count == 3U);
    assert(result.command_block_payload_count == 0U);
    assert(result.omitted_command_block_data_count == 1U);
    assert(blocks.size() == 3U);
    assert(blocks[1].spec.command_name == "minecraft:repeating_command_block");
}

void testLegacyZeroAuxUsesItsOwnMappingCacheEntry(const ScopedTempDirectory& temporary) {
    const fs::path source = temporary.child("legacy_zero_aux.bdx");
    writeBytes(source, kLegacyZeroAuxFixtureBdx.data(), kLegacyZeroAuxFixtureBdx.size());

    SchematicParseResult result;
    std::vector<ParsedBlock> blocks;
    std::string error;
    assert(parseWithSink(source, &result, &blocks, nullptr, &error));
    assert(error.empty());
    assert(result.imported_block_count == 2U);
    assert(blocks.size() == 2U);

    // A state-palette entry without legacy aux remains the normal flattened
    // stone-slab material. The legacy opcode must retain the exact native
    // command identity and its explicit zero aux instead.
    assert(blocks[0].world_x == 100 && blocks[0].world_y == 64 && blocks[0].world_z == -40);
    assert(blocks[0].spec.command_name == "minecraft:stone_block_slab4");
    assert(blocks[0].spec.aux == 2U);
    assert(blocks[1].world_x == 101 && blocks[1].world_y == 64 && blocks[1].world_z == -40);
    assert(blocks[1].spec.command_name == "minecraft:stone_slab");
    assert(blocks[1].spec.aux == 0U);
}

void testLegacyCamelCaseIdentifierNormalizes(const ScopedTempDirectory& temporary) {
    const fs::path source = temporary.child("legacy_camel_case.bdx");
    writeBytes(source, kLegacyCamelCaseFixtureBdx.data(), kLegacyCamelCaseFixtureBdx.size());

    SchematicParseResult result;
    std::vector<ParsedBlock> blocks;
    std::string error;
    assert(parseWithSink(source, &result, &blocks, nullptr, &error));
    assert(error.empty());
    assert(result.imported_block_count == 1U);
    assert(result.unsupported_block_count == 0U);
    assert(blocks.size() == 1U);
    assert(blocks[0].world_x == 100 && blocks[0].world_y == 64 && blocks[0].world_z == -40);
    assert(blocks[0].spec.command_name == "minecraft:sea_lantern");
}

void testLegacyUnqualifiedStoneIdentifierNormalizes(const ScopedTempDirectory& temporary) {
    const fs::path source = temporary.child("legacy_unqualified_stone.bdx");
    writeBytes(source, kLegacyUnqualifiedStoneFixtureBdx.data(),
               kLegacyUnqualifiedStoneFixtureBdx.size());

    SchematicParseResult result;
    std::vector<ParsedBlock> blocks;
    std::string error;
    assert(parseWithSink(source, &result, &blocks, nullptr, &error));
    assert(error.empty());
    assert(result.imported_block_count == 1U);
    assert(result.unsupported_block_count == 0U);
    assert(blocks.size() == 1U);
    assert(blocks[0].world_x == 100 && blocks[0].world_y == 64 && blocks[0].world_z == -40);
    assert(blocks[0].spec.command_name == "minecraft:stone");
}

void testChestItemsAreDeferredAndInvalidIdentifiersAreFiltered(
    const ScopedTempDirectory& temporary) {
    const fs::path source = temporary.child("chest_items.bdx");
    writeBytes(source, kChestFixtureBdx.data(), kChestFixtureBdx.size());

    SchematicParseResult result;
    std::vector<ParsedBlock> blocks;
    std::vector<ContainerItemRecord> items;
    std::string error;
    assert(parseWithSink(source, &result, &blocks, nullptr, &error, &items));
    assert(error.empty());
    assert(result.imported_block_count == 1U);
    assert(result.container_item_payload_count == 2U);
    // The invalid source item is intentionally omitted, but it must not cause
    // a whole-chest failure, an omitted-container warning, or prevent valid
    // contents from being retained.
    assert(result.omitted_block_entity_count == 0U);
    assert(blocks.size() == 1U);
    assert(blocks[0].spec.command_name == "minecraft:chest");
    assert(items.size() == 2U);
    assert(items[0].x == 100 && items[0].y == 64 && items[0].z == -40);
    assert(items[0].expected_container_id == "minecraft:chest");
    assert(items[0].item_id == "minecraft:diamond_sword");
    assert(items[0].slot == 3U && items[0].count == 1U && items[0].aux == 0U);
    assert(items[1].item_id == "minecraft:stone");
    assert(items[1].slot == 4U && items[1].count == 64U && items[1].aux == 0U);
    assert(items[0].enchantments.empty() && items[1].enchantments.empty());
}

void testTerminalThreeFlagCommandBlockPayloadUsesSafeImpulseFallback(
    const ScopedTempDirectory& temporary) {
    const fs::path source = temporary.child("terminal_three_flag_command_block.bdx");
    writeBytes(source, kTerminalThreeFlagCommandFixtureBdx.data(),
               kTerminalThreeFlagCommandFixtureBdx.size());

    SchematicParseResult result;
    std::vector<ParsedBlock> blocks;
    std::vector<CommandBlockRecord> command_blocks;
    std::string error;
    assert(parseWithSink(source, &result, &blocks, &command_blocks, &error));
    assert(error.empty());
    assert(result.imported_block_count == 1U);
    assert(result.command_block_payload_count == 1U);
    assert(result.omitted_command_block_data_count == 0U);
    assert(blocks.size() == 1U);
    assert(blocks[0].spec.command_name == "minecraft:command_block");
    assert(command_blocks.size() == 1U);
    assert(command_blocks[0].x == 100 && command_blocks[0].y == 64 &&
           command_blocks[0].z == -40);
    assert(command_blocks[0].mode == kCommandBlockModeImpulse);
    assert(command_blocks[0].command == "say terminal");
    assert(command_blocks[0].name == "name");
    // The missing field cannot safely make an arbitrary impulse command
    // always-active, so pulse/impulse blocks retain the redstone-controlled
    // fallback. Chain/repeating blocks use the separate legacy test below.
    assert(command_blocks[0].redstone_mode);
    assert(!command_blocks[0].executing_on_first_tick);
    assert(command_blocks[0].output_tracked);
    assert(!command_blocks[0].conditional);
    assert(command_blocks[0].tick_delay == 0);
    assert(command_blocks[0].last_output.empty());
}

void testLegacyMissingNeedRedstoneKeepsChainAndRepeatingBlocksActive(
    const ScopedTempDirectory& temporary) {
    const fs::path source = temporary.child("legacy_missing_need_redstone.bdx");
    writeBytes(source, kLegacyMissingNeedRedstoneFixtureBdx.data(),
               kLegacyMissingNeedRedstoneFixtureBdx.size());

    SchematicParseResult result;
    std::vector<ParsedBlock> blocks;
    std::vector<CommandBlockRecord> command_blocks;
    std::string error;
    assert(parseWithSink(source, &result, &blocks, &command_blocks, &error));
    assert(error.empty());

    assert(result.imported_block_count == 3U);
    assert(result.command_block_payload_count == 2U);
    assert(blocks.size() == 3U);
    assert(blocks[0].world_x == 100 &&
           blocks[0].spec.command_name == "minecraft:chain_command_block");
    assert(blocks[1].world_x == 101 &&
           blocks[1].spec.command_name == "minecraft:repeating_command_block");
    // Both omitted fields were followed by opcode 14. Reaching X=102 proves
    // neither movement opcode was consumed as a redstone boolean.
    assert(blocks[2].world_x == 102 && blocks[2].world_y == 64 &&
           blocks[2].world_z == -40);
    assert(blocks[2].spec.command_name == "minecraft:stone");

    assert(command_blocks.size() == 2U);
    assert(command_blocks[0].x == 100 && command_blocks[0].mode == kCommandBlockModeChain);
    assert(command_blocks[0].command == "say chain");
    assert(!command_blocks[0].redstone_mode);
    assert(command_blocks[1].x == 101 && command_blocks[1].mode == kCommandBlockModeRepeat);
    assert(command_blocks[1].command == "say repeat");
    assert(!command_blocks[1].redstone_mode);
}

void testNonCanonicalNeedRedstonePlaceholderDoesNotDuplicateMovement(
    const ScopedTempDirectory& temporary) {
    const fs::path source = temporary.child("noncanonical_need_redstone_placeholder.bdx");
    writeBytes(source, kNonCanonicalNeedRedstonePlaceholderFixtureBdx.data(),
               kNonCanonicalNeedRedstonePlaceholderFixtureBdx.size());

    SchematicParseResult result;
    std::vector<ParsedBlock> blocks;
    std::vector<CommandBlockRecord> command_blocks;
    std::string error;
    assert(parseWithSink(source, &result, &blocks, &command_blocks, &error));
    assert(error.empty());
    assert(result.imported_block_count == 2U);
    assert(result.command_block_payload_count == 2U);
    assert(blocks.size() == 2U);
    assert(blocks[0].world_x == 100 && blocks[1].world_x == 101);
    assert(command_blocks.size() == 2U);
    assert(command_blocks[0].x == 100 && command_blocks[1].x == 101);
    assert(command_blocks[0].command == "say one");
    assert(command_blocks[1].command == "say two");
    // Byte 14 is only an opaque compatibility placeholder, not a trusted
    // always-active setting for an arbitrary impulse command block.
    assert(command_blocks[0].redstone_mode && command_blocks[1].redstone_mode);
}

void testLegacyMissingNeedRedstoneBeforeCreateConstantString(
    const ScopedTempDirectory& temporary) {
    const fs::path source = temporary.child("legacy_missing_need_redstone_before_string.bdx");
    writeBytes(source, kLegacyMissingNeedRedstoneBeforeStringFixtureBdx.data(),
               kLegacyMissingNeedRedstoneBeforeStringFixtureBdx.size());

    SchematicParseResult result;
    std::vector<ParsedBlock> blocks;
    std::vector<CommandBlockRecord> command_blocks;
    std::string error;
    assert(parseWithSink(source, &result, &blocks, &command_blocks, &error));
    assert(error.empty());
    assert(result.imported_block_count == 2U);
    assert(blocks.size() == 2U);
    assert(blocks[0].world_x == 100 &&
           blocks[0].spec.command_name == "minecraft:command_block");
    assert(blocks[1].world_x == 101 &&
           blocks[1].spec.command_name == "minecraft:stone");
    assert(command_blocks.size() == 1U);
    assert(command_blocks[0].command == "say missing");
    assert(command_blocks[0].redstone_mode);
}

void assertRuntimeCommandBlockFixture(const ScopedTempDirectory& temporary,
                                      const char* source_name,
                                      const uint8_t* bytes,
                                      size_t byte_count) {
        const fs::path source = temporary.child(source_name);
        writeBytes(source, bytes, byte_count);
        SchematicParseResult result;
        std::vector<ParsedBlock> blocks;
        std::vector<CommandBlockRecord> command_blocks;
        std::vector<ContainerItemRecord> items;
        std::string error;
        assert(parseWithSink(source, &result, &blocks, &command_blocks, &error, &items));
        assert(error.empty());

        // A placement follows each command-block payload.  This proves the
        // uint16/uint32 runtime-id decoder consumed the complete payload and
        // did not shift the rest of the opcode stream.
        assert(result.imported_block_count == 3U);
        assert(result.command_block_payload_count == 1U);
        assert(result.container_item_payload_count == 1U);
        assert(blocks.size() == 3U);
        assert(blocks[0].world_x == 100 && blocks[0].world_y == 64 &&
               blocks[0].world_z == -40);
        assert(blocks[0].spec.command_name == "minecraft:stone");
        assert(blocks[1].world_x == 101 && blocks[1].world_y == 64 &&
               blocks[1].world_z == -40);
        assert(blocks[1].spec.command_name == "minecraft:chain_command_block");
        assert(blocks[2].world_x == 102 && blocks[2].world_y == 64 &&
               blocks[2].world_z == -40);
        assert(blocks[2].spec.command_name == "minecraft:chest");

        assert(command_blocks.size() == 1U);
        const CommandBlockRecord& record = command_blocks.front();
        assert(record.x == 101 && record.y == 64 && record.z == -40);
        // BDX payload `needRedstone=false` means the keep-on state, while
        // `conditional=true` selects the conditional command-block state.
        assert(record.mode == kCommandBlockModeChain);
        assert(!record.redstone_mode);
        assert(record.conditional);
        assert(record.executing_on_first_tick);
        assert(!record.output_tracked);
        assert(record.tick_delay == 0);
        assert(record.command == "say runtime");
        assert(record.name == "runtime");
        assert(record.last_output.empty());

        assert(items.size() == 1U);
        assert(items[0].x == 102 && items[0].y == 64 && items[0].z == -40);
        assert(items[0].expected_container_id == "minecraft:chest");
        assert(items[0].item_id == "minecraft:stone");
        assert(items[0].slot == 4U && items[0].count == 2U && items[0].aux == 0U);
}

void testRuntimeId16CommandBlockAndContainerPayloads(const ScopedTempDirectory& temporary) {
    assertRuntimeCommandBlockFixture(temporary, "runtime_id_16.bdx",
                                     kRuntimeIdFixtureBdx.data(),
                                     kRuntimeIdFixtureBdx.size());
}

void testRuntimeId32CommandBlockAndContainerPayloads(const ScopedTempDirectory& temporary) {
    assertRuntimeCommandBlockFixture(temporary, "runtime_id_32.bdx",
                                     kRuntimeId32FixtureBdx.data(),
                                     kRuntimeId32FixtureBdx.size());
}

void testTruncatedStreamIsRejected(const ScopedTempDirectory& temporary) {
    const fs::path source = temporary.child("truncated.bdx");
    writeBytes(source, kFixtureBdx.data(), kFixtureBdx.size() - 7);

    SchematicParseResult result;
    std::vector<ParsedBlock> blocks;
    std::string error;
    assert(!parseWithSink(source, &result, &blocks, nullptr, &error));
    assert(!error.empty());
}

void testBadContainerHeaderIsRejected(const ScopedTempDirectory& temporary) {
    const fs::path source = temporary.child("not_bdx.bdx");
    constexpr std::array<uint8_t, 4> kNotBdx = {66, 68, 88, 0};
    writeBytes(source, kNotBdx.data(), kNotBdx.size());

    SchematicParseResult result;
    std::vector<ParsedBlock> blocks;
    std::string error;
    assert(!parseWithSink(source, &result, &blocks, nullptr, &error));
    assert(error == "not a Brotli-compressed BDX file (expected BD@ header)");
}

int parseAndReportExternalFile(const char* path) {
    SchematicParseOptions options;
    options.source_path = path;
    options.chunk_size = 32;
    options.block_sink = [](const ParsedBlock&, std::string*) { return true; };
    // The command body is intentionally not printed or executed by this host
    // parser test.  Retaining records here exercises the exact deferred-sink
    // path used by BuildImportRuntime and proves that a successful parse did
    // not merely skip command-block payloads.
    std::vector<CommandBlockRecord> command_blocks;
    options.command_block_sink = [&command_blocks](const CommandBlockRecord& record,
                                                   std::string*) {
        command_blocks.push_back(record);
        return true;
    };

    SchematicParseResult result;
    std::string error;
    if (!BdxParser().parse(options, BlockMapper(), &result, &error)) {
        std::fprintf(stderr, "BDX parse failed: %s\n", error.c_str());
        return 1;
    }
    const BlockBounds& bounds = result.source_volume_bounds;
    std::printf(
        "BDX parsed: imported=%llu skipped=%llu unsupported=%llu degraded=%llu "
        "omitted_block_entities=%llu command_payloads=%llu retained_command_payloads=%zu "
        "bounds=(%d,%d,%d)-(%d,%d,%d)\n",
        static_cast<unsigned long long>(result.imported_block_count),
        static_cast<unsigned long long>(result.skipped_block_count),
        static_cast<unsigned long long>(result.unsupported_block_count),
        static_cast<unsigned long long>(result.degraded_block_count),
        static_cast<unsigned long long>(result.omitted_block_entity_count),
        static_cast<unsigned long long>(result.command_block_payload_count),
        command_blocks.size(),
        bounds.min_x, bounds.min_y, bounds.min_z, bounds.max_x, bounds.max_y, bounds.max_z);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) return parseAndReportExternalFile(argv[1]);
    if (argc != 1) {
        std::fprintf(stderr, "usage: BdxParserTest [path-to-file.bdx]\n");
        return 2;
    }

    const ScopedTempDirectory temporary;
    testBedrockStatesNbtSkippingAndCoordinates(temporary);
    testCommandBlockPayloadCanBeDeliberatelyOmitted(temporary);
    testLegacyZeroAuxUsesItsOwnMappingCacheEntry(temporary);
    testLegacyCamelCaseIdentifierNormalizes(temporary);
    testLegacyUnqualifiedStoneIdentifierNormalizes(temporary);
    testChestItemsAreDeferredAndInvalidIdentifiersAreFiltered(temporary);
    testTerminalThreeFlagCommandBlockPayloadUsesSafeImpulseFallback(temporary);
    testLegacyMissingNeedRedstoneKeepsChainAndRepeatingBlocksActive(temporary);
    testNonCanonicalNeedRedstonePlaceholderDoesNotDuplicateMovement(temporary);
    testLegacyMissingNeedRedstoneBeforeCreateConstantString(temporary);
    testRuntimeId16CommandBlockAndContainerPayloads(temporary);
    testRuntimeId32CommandBlockAndContainerPayloads(temporary);
    testTruncatedStreamIsRejected(temporary);
    testBadContainerHeaderIsRejected(temporary);
    return 0;
}
