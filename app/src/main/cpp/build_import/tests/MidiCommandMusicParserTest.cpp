#include "../MidiCommandMusicParser.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* detail) {
    if (!condition) {
        std::cerr << "MidiCommandMusicParserTest: " << detail << '\n';
        std::exit(1);
    }
}

void appendU16(std::vector<uint8_t>* bytes, uint16_t value) {
    bytes->push_back(static_cast<uint8_t>(value >> 8U));
    bytes->push_back(static_cast<uint8_t>(value));
}

void appendU32(std::vector<uint8_t>* bytes, uint32_t value) {
    bytes->push_back(static_cast<uint8_t>(value >> 24U));
    bytes->push_back(static_cast<uint8_t>(value >> 16U));
    bytes->push_back(static_cast<uint8_t>(value >> 8U));
    bytes->push_back(static_cast<uint8_t>(value));
}

void appendTrack(std::vector<uint8_t>* bytes, const std::vector<uint8_t>& track) {
    bytes->insert(bytes->end(), {'M', 'T', 'r', 'k'});
    appendU32(bytes, static_cast<uint32_t>(track.size()));
    bytes->insert(bytes->end(), track.begin(), track.end());
}

std::vector<uint8_t> makeFormatOneScore() {
    std::vector<uint8_t> bytes{'M', 'T', 'h', 'd'};
    appendU32(&bytes, 6);
    appendU16(&bytes, 1);   // Format 1: timing and note tracks are merged.
    appendU16(&bytes, 2);
    appendU16(&bytes, 96);

    // 120 BPM for one quarter note, then 240 BPM. The final end marker gives
    // a convenient duration check across the tempo boundary.
    const std::vector<uint8_t> tempo_track{
        0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,
        0x60, 0xFF, 0x51, 0x03, 0x03, 0xD0, 0x90,
        0x60, 0xFF, 0x2F, 0x00,
    };
    appendTrack(&bytes, tempo_track);

    // Channel 1 uses guitar, channel 10 emits a bass drum in the same game
    // tick. C4 is released while CC64 is held, then ends when sustain lifts.
    const std::vector<uint8_t> note_track{
        0x00, 0xB0, 0x40, 0x7F,
        0x00, 0xC0, 0x18,
        0x00, 0x90, 0x3C, 0x64,
        0x00, 0x99, 0x24, 0x78,
        0x30, 0x80, 0x3C, 0x40,
        0x30, 0xB0, 0x40, 0x00,
        0x00, 0x89, 0x24, 0x40,
        0x60, 0xFF, 0x2F, 0x00,
    };
    appendTrack(&bytes, note_track);
    return bytes;
}

std::vector<uint8_t> makeFormatZeroScore() {
    std::vector<uint8_t> bytes{'M', 'T', 'h', 'd'};
    appendU32(&bytes, 6);
    appendU16(&bytes, 0);
    appendU16(&bytes, 1);
    appendU16(&bytes, 96);
    const std::vector<uint8_t> track{
        0x00, 0xC0, 0x00,
        0x00, 0x90, 0x45, 0x7F,
        0x60, 0x80, 0x45, 0x20,
        0x00, 0xFF, 0x2F, 0x00,
    };
    appendTrack(&bytes, track);
    return bytes;
}

void testFormatOneMergesTempoProgramSustainAndPercussion() {
    const std::vector<uint8_t> bytes = makeFormatOneScore();
    build_import::MidiCommandMusicParser parser;
    build_import::MidiParseResult midi;
    std::string error;
    require(parser.parseBytes(bytes.data(), bytes.size(), {}, &midi, &error),
            "format-1 score should parse");
    require(midi.format == 1 && midi.declared_track_count == 2,
            "format and track count should be retained");
    require(midi.ticks_per_quarter_note == 96 && !midi.uses_smpte_timing,
            "PPQ division should be retained");
    require(midi.tempo_changes.size() == 2 &&
                midi.tempo_changes[0].microseconds_per_quarter_note == 500000 &&
                midi.tempo_changes[1].microseconds_per_quarter_note == 250000,
            "tempo changes should be parsed in their original timeline");
    require(midi.duration_microseconds == 750000,
            "tempo map should convert the merged track duration accurately");
    require(midi.program_changes.size() == 1 && midi.program_changes[0].program == 24,
            "program changes should be retained");
    require(midi.control_changes.size() == 2 && midi.control_changes[0].controller == 64,
            "CC64 sustain records should be retained");
    require(midi.notes.size() == 2, "same-tick multichannel notes should both survive");
    require(midi.notes[0].channel == 0 && midi.notes[0].program == 24 &&
                midi.notes[0].sustain_extended && midi.notes[0].end_tick == 96 &&
                midi.notes[0].end_microseconds == 500000,
            "sustained melodic note should retain its program and actual release time");
    require(midi.notes[1].channel == 9 && midi.notes[1].percussion,
            "channel 10 should be marked as percussion");

    std::vector<build_import::CommandBlockRecord> commands;
    build_import::MidiCommandMusicOptions command_options;
    command_options.output.chunk_size = 16;
    command_options.output.base_y = 64;
    command_options.output.block_sink = [](const build_import::ParsedBlock&, std::string*) {
        return true;
    };
    command_options.output.command_block_sink = [&commands](
        const build_import::CommandBlockRecord& command, std::string*) {
        commands.push_back(command);
        return true;
    };
    build_import::SchematicParseResult result;
    require(parser.buildCommandMusic(midi, command_options, &result, &error),
            "MIDI score should lower to command blocks");
    require(commands.size() == 2 && commands[0].tick_delay == 0 && commands[1].tick_delay == 0,
            "same-tick MIDI notes should lower as a zero-delay chord");
    require(commands[0].command.find("playsound note.guitar") == 0,
            "GM guitar program should choose the native guitar sound");
    require(commands[1].command.find("playsound note.basedrum") == 0,
            "channel-10 kick should choose the native bass-drum sound");
}

void testFormatZeroParses() {
    const std::vector<uint8_t> bytes = makeFormatZeroScore();
    build_import::MidiCommandMusicParser parser;
    build_import::MidiParseResult midi;
    std::string error;
    require(parser.parseBytes(bytes.data(), bytes.size(), {}, &midi, &error),
            "format-0 score should parse");
    require(midi.format == 0 && midi.notes.size() == 1 && midi.notes[0].note == 69,
            "format-0 note should be preserved");
}

}  // namespace

int main() {
    testFormatOneMergesTempoProgramSustainAndPercussion();
    testFormatZeroParses();
    return 0;
}
