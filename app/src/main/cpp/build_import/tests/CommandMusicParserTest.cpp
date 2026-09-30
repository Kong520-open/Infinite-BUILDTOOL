#include "../CommandMusicParser.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* detail) {
    if (!condition) {
        std::cerr << "CommandMusicParserTest: " << detail << '\n';
        std::exit(1);
    }
}

bool almostEqual(double left, double right) {
    return std::fabs(left - right) < 0.000001;
}

void testLayoutTurnsUpAtLayerBoundary() {
    build_import::CommandMusicChainLayout layout(31, 64, -1);
    require(layout.valid(), "layout should accept a normal base position");
    require(layout.capacity() == 65536,
            "layout capacity should use all modern build layers through Y=319");

    build_import::CommandMusicChainPosition first;
    build_import::CommandMusicChainPosition last_first_layer;
    build_import::CommandMusicChainPosition first_second_layer;
    std::string error;
    require(layout.positionForIndex(0, &first, &error), "first position should exist");
    require(layout.positionForIndex(255, &last_first_layer, &error),
            "last first-layer position should exist");
    require(layout.positionForIndex(256, &first_second_layer, &error),
            "first second-layer position should exist");
    require(first.x == 16 && first.y == 64 && first.z == -16,
            "layout should select the input coordinate's native chunk");
    require(last_first_layer.x == 16 && last_first_layer.y == 64 && last_first_layer.z == -1,
            "first layer should finish on the far Z edge");
    require(first_second_layer.x == 16 && first_second_layer.y == 65 && first_second_layer.z == -1,
            "next layer should begin directly above the prior end");

    uint16_t aux = 0;
    require(build_import::CommandMusicChainLayout::facingAuxToward(
                last_first_layer, first_second_layer, &aux, &error),
            "layer boundary should be a valid command-chain edge");
    require(aux == 1, "layer boundary must face upward");

    build_import::CommandMusicChainLayout high_altitude_layout(0, 300, 0);
    require(high_altitude_layout.valid(),
            "modern high-altitude player coordinates must be valid music starts");
    require(high_altitude_layout.capacity() == 5120,
            "a Y=300 start should retain the remaining twenty build layers");
}

void testPitchAndTickHelpers() {
    constexpr double f_sharp_4 = 369.9944227116344;
    require(almostEqual(build_import::commandMusicPitchForFrequency(f_sharp_4), 1.0),
            "F#4 must map to native pitch 1.0");
    require(almostEqual(build_import::commandMusicPitchForFrequency(f_sharp_4 * 2.0), 2.0),
            "one octave above must map to pitch 2.0");
    require(almostEqual(build_import::commandMusicPitchForFrequency(f_sharp_4 * 0.25), 0.5),
            "low frequencies must clamp to the native note-block range");

    uint64_t tick = 0;
    std::string error;
    require(build_import::commandMusicTickForPcmFrame(48000, 48000, 20, &tick, &error),
            "one second of PCM should convert to ticks");
    require(tick == 20, "one second should be twenty ticks");
    require(build_import::commandMusicTickForPcmFrame(1200, 48000, 20, &tick, &error),
            "half tick PCM position should convert");
    require(tick == 1, "half ticks should quantize upward");
}

void testParserEmitsImpulseThenChainPayloads() {
    using namespace build_import;
    std::vector<ParsedBlock> blocks;
    std::vector<CommandBlockRecord> records;
    CommandMusicParseOptions options;
    options.output.chunk_size = 16;
    options.output.base_x = 31;
    options.output.base_y = 64;
    options.output.base_z = -1;
    options.output.block_sink = [&blocks](const ParsedBlock& block, std::string*) {
        blocks.push_back(block);
        return true;
    };
    options.output.command_block_sink = [&records](const CommandBlockRecord& record, std::string*) {
        records.push_back(record);
        return true;
    };
    options.pcm_sample_rate_hz = 20;
    options.events = {
        {0, 369.9944227116344, 1.0F, CommandMusicInstrument::Harp},
        {1, 369.9944227116344, 0.5F, CommandMusicInstrument::Bell},
        {1, 739.9888454232688, 1.0F, CommandMusicInstrument::Bass},
    };

    CommandMusicParser parser;
    SchematicParseResult result;
    std::string error;
    require(parser.parse(options, &result, &error), "parser should accept ordered PCM events");
    require(blocks.size() == 3 && records.size() == 3, "every note should emit one shell and one payload");
    require(result.imported_block_count == 3 && result.command_block_payload_count == 3,
            "result counters should include the generated command blocks");
    require(blocks[0].spec.command_name == "minecraft:command_block",
            "first note must use a manually powered impulse root");
    require(blocks[1].spec.command_name == "minecraft:chain_command_block" &&
                blocks[2].spec.command_name == "minecraft:chain_command_block",
            "later notes must use chain command blocks");
    require(blocks[0].spec.aux == 5 && blocks[1].spec.aux == 5,
            "command shells should face their next snake node");
    require(!blocks[0].spec.can_fill && blocks[0].spec.stateful,
            "command shells must bypass /fill merging");
    require(records[0].mode == kCommandBlockModeImpulse && records[0].redstone_mode &&
                records[0].tick_delay == 0,
            "root payload should be an immediate redstone-controlled impulse block");
    require(records[1].mode == kCommandBlockModeChain && !records[1].redstone_mode &&
                records[1].tick_delay == 1,
            "later tick should use an always-active chain delay");
    require(records[2].mode == kCommandBlockModeChain && !records[2].redstone_mode &&
                records[2].tick_delay == 0,
            "same-tick chord member should use zero delay");
    require(records[0].command == "playsound note.harp @a ~ ~ ~ 1.000 1.000",
            "root command should be a native note-block playsound");
    require(records[1].command == "playsound note.bell @a ~ ~ ~ 0.500 1.000",
            "instrument and volume should reach the playsound command");
    require(records[2].command == "playsound note.bass @a ~ ~ ~ 1.000 2.000",
            "frequency should reach the native playsound pitch");
}

void testParserNormalizesLeadingTimelineGapFromRootDelay() {
    using namespace build_import;
    std::vector<CommandBlockRecord> records;
    CommandMusicParseOptions options;
    options.output.chunk_size = 16;
    options.output.base_x = 0;
    options.output.base_y = 64;
    options.output.base_z = 0;
    options.output.block_sink = [](const ParsedBlock&, std::string*) { return true; };
    options.output.command_block_sink = [&records](const CommandBlockRecord& record, std::string*) {
        records.push_back(record);
        return true;
    };
    // At 400 Hz, the first accepted note is ten seconds (200 ticks) into the
    // source. The redstone-powered root must still start immediately; only
    // subsequent score-note gaps belong in command-block tick delays.
    options.pcm_sample_rate_hz = 400;
    options.initial_delay_ticks = 0;
    options.events = {
        {4000, 369.9944227116344, 1.0F, CommandMusicInstrument::Harp},
        {4040, 415.3046975799451, 1.0F, CommandMusicInstrument::Harp},
    };

    CommandMusicParser parser;
    SchematicParseResult result;
    std::string error;
    require(parser.parse(options, &result, &error),
            "parser should normalize leading silence from a piano score");
    require(records.size() == 2, "leading-silence test should emit two command records");
    require(records[0].tick_delay == 0,
            "the root must not inherit the source's 200-tick leading silence");
    require(records[1].tick_delay == 2,
            "the second note must retain its two-tick score gap");

    records.clear();
    options.initial_delay_ticks = 3;
    require(parser.parse(options, &result, &error),
            "parser should also accept an explicit piano-score lead-in");
    require(records.size() == 2 && records[0].tick_delay == 3,
            "configured lead-in must stay independent of discarded source silence");
}

}  // namespace

int main() {
    testLayoutTurnsUpAtLayerBoundary();
    testPitchAndTickHelpers();
    testParserEmitsImpulseThenChainPayloads();
    testParserNormalizesLeadingTimelineGapFromRootDelay();
    return 0;
}
