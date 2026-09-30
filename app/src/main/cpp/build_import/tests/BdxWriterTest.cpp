#include "../BdxParser.h"
#include "../BdxWriter.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace build_import;

namespace {

struct TemporaryDirectory {
    TemporaryDirectory() {
        path = fs::temp_directory_path() /
            ("build_import_bdx_writer_" + std::to_string(
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(this))));
        fs::create_directories(path);
    }
    ~TemporaryDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
    fs::path path;
};

void testRoundTripWithCommandBlock() {
    TemporaryDirectory temporary;
    const fs::path output = temporary.path / "round_trip.bdx";

    BdxWriteRequest request;
    request.output_path = output.string();
    request.width = 4;
    request.height = 1;
    request.length = 1;
    request.palette = {
        {"minecraft:air", {}, 0},
        {"minecraft:stone", {}, 0},
        {"minecraft:command_block",
         "minecraft:command_block[conditional=true,facing=east]", 0},
        {"minecraft:repeating_command_block",
         "minecraft:repeating_command_block[conditional=false,facing=up]", 0},
        {"minecraft:chain_command_block",
         "minecraft:chain_command_block[conditional=true,facing=south]", 0},
    };
    request.block_indices = {2, 3, 4, 1};

    CommandBlockRecord impulse;
    impulse.x = 0;
    impulse.command = "say round-trip | impulse";
    impulse.name = "writer impulse";
    impulse.last_output = "output one";
    impulse.conditional = true;
    impulse.redstone_mode = true;
    impulse.output_tracked = true;
    impulse.tick_delay = 7;
    request.command_blocks.push_back(impulse);

    CommandBlockRecord repeating;
    repeating.x = 1;
    repeating.mode = kCommandBlockModeRepeat;
    repeating.command = "execute as @a run say repeating";
    repeating.name = "repeat name";
    repeating.last_output = "output two";
    repeating.redstone_mode = false;
    repeating.executing_on_first_tick = true;
    repeating.tick_delay = -12;
    request.command_blocks.push_back(repeating);

    CommandBlockRecord chain;
    chain.x = 2;
    chain.mode = kCommandBlockModeChain;
    chain.command = "say \xE4\xB8\xAD\xE6\x96\x87";
    chain.name = "chain \xE5\x90\x8D\xE7\xA7\xB0";
    chain.conditional = true;
    chain.redstone_mode = true;
    chain.output_tracked = true;
    chain.executing_on_first_tick = true;
    chain.tick_delay = 123456;
    request.command_blocks.push_back(chain);

    std::string error;
    assert(BdxWriter::write(request, &error));
    assert(error.empty());
    assert(fs::exists(output));

    SchematicParseOptions options;
    options.source_path = output.string();
    options.base_x = 10;
    options.base_y = 20;
    options.base_z = 30;
    std::vector<ParsedBlock> blocks;
    std::vector<CommandBlockRecord> commands;
    options.block_sink = [&blocks](const ParsedBlock& block, std::string*) {
        blocks.push_back(block);
        return true;
    };
    options.command_block_sink = [&commands](const CommandBlockRecord& record, std::string*) {
        commands.push_back(record);
        return true;
    };
    SchematicParseResult result;
    assert(BdxParser().parse(options, BlockMapper(), &result, &error));
    assert(error.empty());
    assert(result.imported_block_count == 4U);
    assert(blocks.size() == 4U);
    assert(blocks[0].spec.command_name == "minecraft:command_block");
    assert(blocks[0].spec.aux == 13U);  // east (5) + conditional bit (8)
    assert(blocks[1].spec.command_name == "minecraft:repeating_command_block");
    assert(blocks[1].spec.aux == 1U);   // up
    assert(blocks[2].spec.command_name == "minecraft:chain_command_block");
    assert(blocks[2].spec.aux == 11U);  // south (3) + conditional bit (8)

    assert(commands.size() == 3U);
    assert(commands[0].x == 10 && commands[0].y == 20 && commands[0].z == 30);
    assert(commands[0].mode == kCommandBlockModeImpulse);
    assert(commands[0].command == impulse.command);
    assert(commands[0].name == impulse.name);
    assert(commands[0].last_output == impulse.last_output);
    assert(commands[0].conditional);
    assert(commands[0].redstone_mode);
    assert(commands[0].output_tracked);
    assert(commands[0].tick_delay == impulse.tick_delay);

    assert(commands[1].x == 11 && commands[1].mode == kCommandBlockModeRepeat);
    assert(commands[1].command == repeating.command);
    assert(commands[1].name == repeating.name);
    assert(commands[1].last_output == repeating.last_output);
    assert(!commands[1].conditional);
    assert(!commands[1].redstone_mode);
    assert(commands[1].executing_on_first_tick);
    assert(commands[1].tick_delay == repeating.tick_delay);

    assert(commands[2].x == 12 && commands[2].mode == kCommandBlockModeChain);
    assert(commands[2].command == chain.command);
    assert(commands[2].name == chain.name);
    assert(commands[2].conditional);
    assert(commands[2].redstone_mode);
    assert(commands[2].output_tracked);
    assert(commands[2].executing_on_first_tick);
    assert(commands[2].tick_delay == chain.tick_delay);
}

}  // namespace

int main() {
    testRoundTripWithCommandBlock();
    std::cout << "BdxWriterTest passed\n";
    return 0;
}
