#include "ContainerEntityCodec.h"
#include "InfiniteczBuildParser.h"
#include "InfiniteczBuildWriter.h"
#include "InfinityCompressionCodec.h"
#include "InfinityCryptoCodec.h"
#include "InfinityFormatKey.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace build_import;

namespace {

std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

bool containsText(const std::vector<uint8_t>& bytes, const std::string& text) {
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) != bytes.end();
}

void writeBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    assert(output);
}

bool hasDecryptedStagingFile(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(".infinity-decrypted-", 0) == 0) return true;
    }
    return false;
}

void testForwardCompatibleNativeBlocks(const std::filesystem::path& directory) {
    const std::filesystem::path output = directory / "forward_blocks.infinity";
    InfiniteczBuildWriteRequest request;
    request.output_path = output.string();
    request.display_name = "forward-blocks";
    request.width = 3;
    request.height = 1;
    request.length = 1;
    request.palette = {"minecraft:air", "minecraft:pale_oak_planks",
                       "minecraft:cherry_planks"};
    request.block_indices = {1, 0, 2};
    const std::string native_state = R"({"weirdo_direction":1,"upside_down_bit":true})";
    request.raw_blocks.push_back({1, 0, 0, "minecraft:pale_oak_stairs", 5, 0,
                                  native_state, {}});

    std::string error;
    assert(InfiniteczBuildWriter::write(request, &error));
    BlockMapper mapper;
    SchematicParseOptions options;
    options.source_path = output.string();
    options.spool_directory = directory.string();
    std::vector<ParsedBlock> blocks;
    std::vector<SchematicRawBlock> raw_blocks;
    options.block_sink = [&blocks](const ParsedBlock& block, std::string*) {
        blocks.push_back(block);
        return true;
    };
    options.raw_block_sink = [&raw_blocks](const SchematicRawBlock& block, std::string*) {
        raw_blocks.push_back(block);
        return true;
    };
    SchematicParseResult result;
    assert(InfiniteczBuildParser().parse(options, mapper, &result, &error));
    assert(error.empty() && result.imported_block_count == 3 && blocks.size() == 3);
    assert(blocks[0].spec.command_name == "minecraft:pale_oak_planks");
    assert(blocks[1].spec.command_name == "minecraft:pale_oak_stairs" &&
           blocks[1].spec.aux == 5);
    assert(blocks[2].spec.command_name == "minecraft:cherry_planks");
    assert(raw_blocks.size() == 1 && raw_blocks[0].state_json == native_state);
}

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "infinitecz_build_format_test";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    const std::filesystem::path output = directory / "round_trip.infinity";

    InfiniteczBuildWriteRequest request;
    request.output_path = output.string();
    request.display_name = "round-trip";
    request.origin_x = 100;
    request.origin_y = 64;
    request.origin_z = -20;
    request.width = 4;
    request.height = 2;
    request.length = 3;
    request.palette = {"minecraft:air", "minecraft:stone", "minecraft:lever",
                       "minecraft:command_block[facing=north]",
                       "minecraft:redstone_torch[lit=true]", "minecraft:stone_button",
                       "minecraft:powered_comparator",
                       "minecraft:spruce_sign[rotation=4,waterlogged=false]",
                       "minecraft:birch_wall_hanging_sign[facing=north,waterlogged=false]",
                       "minecraft:jungle_wall_sign[facing=east,waterlogged=false]",
                       "minecraft:birch_fence_gate[facing=west,in_wall=true,open=true,powered=false]"};
    request.block_indices = {
        1, 2, 5, 7, 3, 4, 6, 0, 8, 8, 8, 9,
        10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    // Early native builds accidentally JSON-quoted the SDK identifier. The
    // parser must still use this record's exact legacy identity and aux.
    request.raw_blocks.push_back({1, 0, 0, "\"minecraft:lever\"", 3, 69,
                                  "{\"facing\":\"west\"}", {}});
    CommandBlockRecord command;
    command.x = 0;
    command.y = 0;
    command.z = 1;
    command.mode = kCommandBlockModeImpulse;
    command.command = "say preserved";
    command.name = "test";
    command.tick_delay = 7;
    command.output_tracked = true;
    request.raw_blocks.push_back({0, 0, 1, "minecraft:command_block", 4, 137,
                                  {}, "{\"Command\":\"say preserved\"}"});
    request.raw_blocks.push_back({1, 0, 1, "minecraft:redstone_torch", 2, 76,
                                  {}, {}});
    request.raw_blocks.push_back({2, 0, 0, "minecraft:stone_button", 12, 77,
                                  {}, {}});
    request.raw_blocks.push_back({2, 0, 1, "minecraft:powered_comparator", 13, 150,
                                  {}, {}});
    request.raw_blocks.push_back({
        3, 0, 0, "minecraft:standing_sign", 4, 63, {},
        R"({"FrontText":{"Text":"front\n\u4f60\u597d","SignTextColor":-16711936,"IgnoreLighting":true,"PersistFormatting":false,"HideGlowOutline":false},"BackText":{"Text":"back","SignTextColor":-65536,"IgnoreLighting":false,"PersistFormatting":true,"HideGlowOutline":true},"IsWaxed":true,"x":999,"y":999,"z":999,"TextOwner":123,"LockedForEditing":true,"FilteredText":"not restored"})"});
    request.raw_blocks.push_back({0, 0, 0, "minecraft:chest", 0, 54,
                                   {}, R"({"Items":[{"Slot":3,"Name":"STONE","Count":2,"Damage":7,"tag":{"ignored":true}},{"Slot":4,"Name":"minecraft:oak_planks","Count":64,"Damage":7},{"Slot":10,"Name":"minecraft:diamond_sword","Count":1,"Damage":23}]})"});
    // Some native Bedrock states have no portable Java/Sponge palette entry.
    // Their palette voxel is air, but the coordinate-unique raw record remains
    // authoritative and must retain both its complete u16 aux and opaque SDK
    // state snapshot through encrypted and legacy-plaintext v1 files.
    const std::string hanging_sign_state =
        R"({"attached_bit":true,"ground_sign_direction":11})";
    request.raw_blocks.push_back({3, 0, 1, "minecraft:pale_oak_hanging_sign",
                                  0x01DAU, 0, hanging_sign_state, {}});
    // The portable palette can contain only one approximation while the raw
    // native records retain distinct material and wall-facing values.
    request.raw_blocks.push_back({0, 0, 2, "minecraft:pale_oak_hanging_sign",
                                  5, 0, {}, {}});
    request.raw_blocks.push_back({1, 0, 2, "minecraft:pale_oak_hanging_sign",
                                  3, 0, {}, {}});
    request.raw_blocks.push_back({2, 0, 2, "minecraft:pale_oak_hanging_sign",
                                  4, 0, {}, {}});
    // Old exports could flatten the raw SDK name to a generic sign even
    // though their portable palette retained the exact wood family. The raw
    // aux must still win for direction while the palette repairs material.
    request.raw_blocks.push_back({3, 0, 2, "minecraft:wall_sign", 5, 68,
                                  {}, {}});
    // A modern raw block with legacy_id=0 must not be reinterpreted as air.
    // When its exact target name is unavailable, retain the palette's safe
    // material approximation and all of its directional/state bits.
    request.raw_blocks.push_back({0, 1, 0, "minecraft:pale_oak_fence_gate",
                                  13, 0, {}, {}});
    request.command_blocks.push_back(command);
    std::string error;
    InfiniteczBuildWriteRequest invalid_request = request;
    invalid_request.output_path = (directory / "wrong.icbuild").string();
    assert(!InfiniteczBuildWriter::write(invalid_request, &error));
    assert(error.find(".infinity") != std::string::npos);
    error.clear();
    InfiniteczBuildWriteRequest uppercase_request = request;
    uppercase_request.output_path = (directory / "uppercase.INFINITY").string();
    assert(!InfiniteczBuildWriter::write(uppercase_request, &error));
    assert(error.find("lowercase .infinity") != std::string::npos);
    error.clear();
    InfiniteczBuildWriteRequest legacy_request = request;
    legacy_request.output_path = (directory / "legacy.IBuild").string();
    assert(!InfiniteczBuildWriter::write(legacy_request, &error));
    assert(error.find(".infinity") != std::string::npos);
    error.clear();
    assert(InfiniteczBuildWriter::write(request, &error));
    assert(error.empty());
    assert(std::filesystem::is_regular_file(output));
    const std::vector<uint8_t> encrypted_bytes = readBytes(output);
    assert(encrypted_bytes.size() > InfinityCryptoCodec::kHeaderSize +
           InfinityCryptoCodec::kTagSize);
    assert(!containsText(encrypted_bytes, "round-trip"));
    assert(!containsText(encrypted_bytes, "minecraft:redstone_torch"));
    assert(!containsText(encrypted_bytes, "say preserved"));
    assert(!containsText(encrypted_bytes, "minecraft:oak_planks"));
    assert(!containsText(encrypted_bytes, "minecraft:diamond_sword"));
    bool encrypted = false;
    assert(InfinityCryptoCodec::probeEncryptedFile(output.string(), &encrypted, &error));
    assert(encrypted);
    assert(!std::filesystem::exists(output.string() + ".plaintext.part"));
    assert(!std::filesystem::exists(output.string() + ".compressed.part"));

    BlockMapper mapper;
    SchematicParseOptions options;
    options.source_path = output.string();
    options.spool_directory = directory.string();
    options.base_x = 10;
    options.base_y = 20;
    options.base_z = 30;
    std::vector<ParsedBlock> blocks;
    std::vector<SchematicRawBlock> raw;
    std::vector<CommandBlockRecord> commands;
    std::vector<ContainerItemRecord> container_items;
    std::vector<SignRecord> signs;
    options.block_sink = [&blocks](const ParsedBlock& block, std::string*) {
        blocks.push_back(block);
        return true;
    };
    options.raw_block_sink = [&raw](const SchematicRawBlock& record, std::string*) {
        raw.push_back(record);
        return true;
    };
    options.command_block_sink = [&commands](const CommandBlockRecord& record, std::string*) {
        commands.push_back(record);
        return true;
    };
    options.container_item_sink = [&container_items](const ContainerItemRecord& record,
                                                      std::string*) {
        container_items.push_back(record);
        return true;
    };
    options.sign_sink = [&signs](const SignRecord& record, std::string*) {
        signs.push_back(record);
        return true;
    };
    SchematicParseResult result;
    InfiniteczBuildParser parser;
    assert(parser.parse(options, mapper, &result, &error));
    assert(error.empty());
    assert(result.imported_block_count == 13);
    assert(blocks.size() == 13);
    assert(raw.size() == 13);
    const auto raw_hanging_sign = std::find_if(
        raw.begin(), raw.end(), [](const SchematicRawBlock& record) {
            return record.identifier == "minecraft:pale_oak_hanging_sign";
        });
    assert(raw_hanging_sign != raw.end());
    assert(raw_hanging_sign->x == 13 && raw_hanging_sign->y == 20 &&
           raw_hanging_sign->z == 31 && raw_hanging_sign->aux == 0x01DAU &&
           raw_hanging_sign->state_json == hanging_sign_state);
    assert(result.sign_payload_count == 1);
    assert(signs.size() == 1);
    const SignRecord& sign = signs.front();
    assert(sign.x == 13 && sign.y == 20 && sign.z == 30);
    assert(sign.expected_sign_id == "minecraft:spruce_standing_sign");
    assert(sign.has_expected_aux && sign.expected_aux == 4);
    assert(sign.front.present && sign.front.has_text &&
           sign.front.text == std::string("front\n") + u8"\u4f60\u597d");
    assert(sign.front.has_text_color && sign.front.text_color == -16711936);
    assert(sign.front.has_ignore_lighting && sign.front.ignore_lighting);
    assert(sign.front.has_persist_formatting && !sign.front.persist_formatting);
    assert(sign.front.has_hide_glow_outline && !sign.front.hide_glow_outline);
    assert(sign.back.present && sign.back.has_text && sign.back.text == "back");
    assert(sign.back.has_text_color && sign.back.text_color == -65536);
    assert(sign.back.has_ignore_lighting && !sign.back.ignore_lighting);
    assert(sign.back.has_persist_formatting && sign.back.persist_formatting);
    assert(sign.back.has_hide_glow_outline && sign.back.hide_glow_outline);
    assert(sign.has_is_waxed && sign.is_waxed);
    assert(result.container_item_payload_count == 3);
    assert(container_items.size() == 3 && container_items[0].x == 10 &&
           container_items[0].y == 20 && container_items[0].z == 30 &&
           container_items[0].slot == 3 && container_items[0].count == 2 &&
           container_items[0].aux == 7 && container_items[0].item_id == "minecraft:stone" &&
           container_items[0].expected_container_id == "minecraft:chest");
    assert(container_items[1].x == 10 && container_items[1].y == 20 &&
           container_items[1].z == 30 && container_items[1].slot == 4 &&
           container_items[1].count == 64 && container_items[1].aux == 7 &&
           container_items[1].item_id == "minecraft:oak_planks" &&
           container_items[1].expected_container_id == "minecraft:chest");
    assert(container_items[2].x == 10 && container_items[2].y == 20 &&
           container_items[2].z == 30 && container_items[2].slot == 10 &&
           container_items[2].count == 1 && container_items[2].aux == 23 &&
           container_items[2].item_id == "minecraft:diamond_sword" &&
           container_items[2].expected_container_id == "minecraft:chest");
    assert(formatContainerReplaceItemCommand(container_items[1]) ==
           "/replaceitem block 10 20 30 slot.container 4 minecraft:oak_planks 64 7");
    assert(formatContainerReplaceItemCommand(container_items[2]) ==
           "/replaceitem block 10 20 30 slot.container 10 minecraft:diamond_sword 1 23");
    bool found_lever = false;
    bool found_wall_torch = false;
    bool found_pressed_wall_button = false;
    bool found_subtract_comparator = false;
    bool found_sign_block = false;
    bool found_native_hanging_sign = false;
    bool found_native_east_wall_hanging_sign = false;
    bool found_native_south_wall_hanging_sign = false;
    bool found_native_west_wall_hanging_sign = false;
    bool found_recovered_jungle_wall_sign = false;
    bool found_modern_gate_palette_fallback = false;
    for (const ParsedBlock& block : blocks) {
        if (block.world_x == 11 && block.world_y == 20 && block.world_z == 30) {
            found_lever = block.spec.command_name == "minecraft:lever" &&
                block.spec.aux == 3 && block.spec.phase == ImportPhase::Attachment &&
                block.spec.stateful && !block.spec.can_fill;
        }
        if (block.world_x == 11 && block.world_y == 20 && block.world_z == 31) {
            found_wall_torch = block.spec.command_name == "minecraft:redstone_torch" &&
                block.spec.aux == 2 && block.spec.phase == ImportPhase::Attachment &&
                block.spec.stateful && !block.spec.can_fill;
        }
        if (block.world_x == 12 && block.world_y == 20 && block.world_z == 30) {
            found_pressed_wall_button =
                block.spec.command_name == "minecraft:stone_button" &&
                block.spec.aux == 12 && block.spec.phase == ImportPhase::Attachment &&
                block.spec.stateful && !block.spec.can_fill;
        }
        if (block.world_x == 12 && block.world_y == 20 && block.world_z == 31) {
            found_subtract_comparator =
                block.spec.command_name == "minecraft:powered_comparator" &&
                block.spec.aux == 13 && block.spec.phase == ImportPhase::Attachment &&
                block.spec.stateful && !block.spec.can_fill;
        }
        if (block.world_x == 13 && block.world_y == 20 && block.world_z == 30) {
            found_sign_block = block.spec.command_name == "minecraft:spruce_standing_sign" &&
                block.spec.aux == 4 && block.spec.phase == ImportPhase::Attachment &&
                block.spec.stateful && !block.spec.can_fill;
        }
        if (block.world_x == 13 && block.world_y == 20 && block.world_z == 31) {
            found_native_hanging_sign =
                block.spec.command_name == "minecraft:pale_oak_hanging_sign" &&
                block.spec.aux == 0x01DAU &&
                block.spec.phase == ImportPhase::Attachment &&
                block.spec.stateful && !block.spec.can_fill;
        }
        if (block.world_y == 20 && block.world_z == 32 &&
            block.spec.command_name == "minecraft:pale_oak_hanging_sign" &&
            block.spec.phase == ImportPhase::Attachment &&
            block.spec.stateful && !block.spec.can_fill) {
            if (block.world_x == 10 && block.spec.aux == 5) {
                found_native_east_wall_hanging_sign = true;
            } else if (block.world_x == 11 && block.spec.aux == 3) {
                found_native_south_wall_hanging_sign = true;
            } else if (block.world_x == 12 && block.spec.aux == 4) {
                found_native_west_wall_hanging_sign = true;
            }
        }
        if (block.world_x == 13 && block.world_y == 20 && block.world_z == 32) {
            found_recovered_jungle_wall_sign =
                block.spec.command_name == "minecraft:jungle_wall_sign" &&
                block.spec.aux == 5 && block.spec.phase == ImportPhase::Attachment &&
                block.spec.stateful && !block.spec.can_fill;
        }
        if (block.world_x == 10 && block.world_y == 21 && block.world_z == 30) {
            found_modern_gate_palette_fallback =
                block.spec.command_name == "minecraft:birch_fence_gate" &&
                block.spec.aux == 13 && block.spec.phase == ImportPhase::Structure &&
                block.spec.stateful && !block.spec.can_fill;
        }
    }
    assert(found_lever);
    assert(found_wall_torch);
    assert(found_pressed_wall_button);
    assert(found_subtract_comparator);
    assert(found_sign_block);
    assert(found_native_hanging_sign);
    assert(found_native_east_wall_hanging_sign);
    assert(found_native_south_wall_hanging_sign);
    assert(found_native_west_wall_hanging_sign);
    assert(found_recovered_jungle_wall_sign);
    assert(found_modern_gate_palette_fallback);
    assert(commands.size() == 1);
    assert(commands[0].x == 10 && commands[0].y == 20 && commands[0].z == 31);
    assert(commands[0].command == "say preserved");
    assert(commands[0].tick_delay == 7);
    assert(!hasDecryptedStagingFile(directory));

    // Authentication must fail before the inner building parser is allowed to
    // consume a modified payload, and no decrypted staging file may remain.
    const std::filesystem::path tampered_path = directory / "tampered.infinity";
    std::vector<uint8_t> tampered_bytes = encrypted_bytes;
    tampered_bytes[InfinityCryptoCodec::kHeaderSize + 17U] ^= 0x40U;
    writeBytes(tampered_path, tampered_bytes);
    options.source_path = tampered_path.string();
    SchematicParseResult tampered_result;
    error.clear();
    assert(!parser.parse(options, mapper, &tampered_result, &error));
    assert(error.find("authentication failed") != std::string::npos);
    assert(!hasDecryptedStagingFile(directory));

    // The authenticated plaintext is now an explicitly versioned Brotli
    // wrapper. Its declared expanded size is bounded before decoding.
    const std::filesystem::path compressed_payload = directory / "compressed.payload";
    const std::filesystem::path legacy_plaintext = directory / "legacy.IBuild";
    error.clear();
    assert(InfinityCryptoCodec::decryptFileAtomic(output.string(), compressed_payload.string(),
                                                   infinityFormatKeyV1(), {}, &error));
    bool compressed = false;
    assert(InfinityCompressionCodec::probeCompressedFile(compressed_payload.string(),
                                                          &compressed, &error));
    assert(compressed);
    InfinityCompressionOptions too_small;
    too_small.maximum_uncompressed_bytes = 1;
    const std::filesystem::path rejected_expansion = directory / "rejected.IBuild";
    assert(!InfinityCompressionCodec::decompressFile(compressed_payload.string(),
                                                      rejected_expansion.string(),
                                                      too_small, &error));
    assert(error.find("configured size limit") != std::string::npos);
    assert(!std::filesystem::exists(rejected_expansion));
    error.clear();
    InfinityCompressionOptions compression_options;
    compression_options.maximum_uncompressed_bytes = 1ULL * 1024ULL * 1024ULL * 1024ULL;
    compression_options.maximum_compressed_bytes = 1ULL * 1024ULL * 1024ULL * 1024ULL;
    assert(InfinityCompressionCodec::decompressFile(compressed_payload.string(),
                                                     legacy_plaintext.string(),
                                                     compression_options, &error));
    const std::vector<uint8_t> legacy_bytes = readBytes(legacy_plaintext);
    assert(containsText(legacy_bytes, "minecraft:redstone_torch"));
    assert(std::filesystem::file_size(compressed_payload) < legacy_bytes.size());

    // Plain version-one files from the short-lived pre-encryption release
    // remain readable.
    blocks.clear();
    raw.clear();
    commands.clear();
    container_items.clear();
    signs.clear();
    options.source_path = legacy_plaintext.string();
    SchematicParseResult legacy_result;
    assert(parser.parse(options, mapper, &legacy_result, &error));
    assert(error.empty());
    assert(legacy_result.imported_block_count == 13);
    assert(blocks.size() == 13);
    assert(raw.size() == 13);
    const auto legacy_raw_hanging_sign = std::find_if(
        raw.begin(), raw.end(), [](const SchematicRawBlock& record) {
            return record.identifier == "minecraft:pale_oak_hanging_sign";
        });
    assert(legacy_raw_hanging_sign != raw.end());
    assert(legacy_raw_hanging_sign->aux == 0x01DAU &&
           legacy_raw_hanging_sign->state_json == hanging_sign_state);
    assert(commands.size() == 1 && commands[0].command == "say preserved");
    assert(container_items.size() == 3 &&
           container_items[0].item_id == "minecraft:stone" &&
           container_items[0].aux == 7 &&
           container_items[1].item_id == "minecraft:oak_planks" &&
           container_items[1].count == 64 && container_items[1].aux == 7 &&
           container_items[2].item_id == "minecraft:diamond_sword" &&
           container_items[2].count == 1 &&
           container_items[2].aux == 23);
    assert(legacy_result.sign_payload_count == 1);
    assert(signs.size() == 1 && signs[0].x == 13 && signs[0].y == 20 &&
           signs[0].z == 30 &&
           signs[0].expected_sign_id == "minecraft:spruce_standing_sign" &&
           signs[0].front.text == std::string("front\n") + u8"\u4f60\u597d" &&
           signs[0].back.text == "back" && signs[0].has_is_waxed &&
           signs[0].is_waxed);
    assert(!hasDecryptedStagingFile(directory));

    // Compatibility: older encrypted .infinity files contain the raw ICBUILD
    // bytes directly, without the compression wrapper.
    const std::filesystem::path legacy_encrypted = directory / "legacy_encrypted.infinity";
    error.clear();
    assert(InfinityCryptoCodec::encryptFileAtomic(legacy_plaintext.string(),
                                                   legacy_encrypted.string(),
                                                   infinityFormatKeyV1(), {}, &error));
    blocks.clear();
    raw.clear();
    commands.clear();
    container_items.clear();
    signs.clear();
    options.source_path = legacy_encrypted.string();
    SchematicParseResult legacy_encrypted_result;
    assert(parser.parse(options, mapper, &legacy_encrypted_result, &error));
    assert(error.empty());
    assert(legacy_encrypted_result.imported_block_count == 13);
    assert(raw.size() == 13 && commands.size() == 1 && container_items.size() == 3 &&
           signs.size() == 1);
    assert(!hasDecryptedStagingFile(directory));

    testForwardCompatibleNativeBlocks(directory);

    std::filesystem::remove_all(directory, ignored);
    return 0;
}
