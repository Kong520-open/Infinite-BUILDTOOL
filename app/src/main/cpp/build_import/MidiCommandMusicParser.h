#ifndef INFINITE_TEXTURE_MIDI_COMMAND_MUSIC_PARSER_H
#define INFINITE_TEXTURE_MIDI_COMMAND_MUSIC_PARSER_H

#include "CommandMusicParser.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace build_import {

// A normalized Set Tempo meta event.  `tick` is an absolute SMF tick; the
// event becomes active at that tick for every following delta-time.
struct MidiTempoChange {
    uint64_t tick = 0;
    uint32_t microseconds_per_quarter_note = 500000;
};

// Channel voice state is kept separately from notes so callers can inspect
// the original score rather than reverse-engineering it from generated
// playsound commands.
struct MidiProgramChange {
    uint64_t tick = 0;
    uint8_t channel = 0;
    uint8_t program = 0;
};

struct MidiControlChange {
    uint64_t tick = 0;
    uint8_t channel = 0;
    uint8_t controller = 0;
    uint8_t value = 0;
};

// The signed 14-bit MIDI pitch-wheel value is in [-8192, 8191].
struct MidiPitchBend {
    uint64_t tick = 0;
    uint8_t channel = 0;
    int16_t value = 0;
};

// One fully paired note.  SMF does not identify overlapping equal-pitch notes;
// they are paired FIFO per channel/pitch, the conventional MIDI behaviour.
// `sustain_extended` means the key was released while CC64 was down, and
// `ended_by_end_of_file` flags a file that ended before a matching release.
struct MidiNoteEvent {
    uint64_t start_tick = 0;
    uint64_t key_release_tick = 0;
    uint64_t end_tick = 0;
    uint64_t start_microseconds = 0;
    uint64_t end_microseconds = 0;
    uint8_t channel = 0;
    uint8_t note = 0;
    uint8_t velocity = 0;
    uint8_t release_velocity = 0;
    uint8_t program = 0;
    int16_t pitch_bend = 0;
    // RPN 0,0 pitch-bend sensitivity captured at the note onset.  The MIDI
    // default is +/- two semitones, represented as 200 cents.
    uint16_t pitch_bend_range_cents = 200;
    bool percussion = false;
    bool has_key_release = false;
    bool sustain_extended = false;
    bool ended_by_sustain_release = false;
    bool ended_by_end_of_file = false;
};

struct MidiParseResult {
    uint16_t format = 0;
    uint16_t declared_track_count = 0;
    // The raw MThd division field is retained for diagnostics/export.
    uint16_t division = 0;
    bool uses_smpte_timing = false;
    uint16_t ticks_per_quarter_note = 0;
    int8_t smpte_frames_per_second_code = 0;
    uint8_t smpte_ticks_per_frame = 0;
    uint64_t end_tick = 0;
    uint64_t duration_microseconds = 0;
    std::vector<MidiTempoChange> tempo_changes;
    std::vector<MidiProgramChange> program_changes;
    std::vector<MidiControlChange> control_changes;
    std::vector<MidiPitchBend> pitch_bends;
    std::vector<MidiNoteEvent> notes;
};

struct MidiParseOptions {
    // Bound hostile/corrupt inputs before any event vectors are allocated.
    size_t maximum_file_bytes = 64U * 1024U * 1024U;
    uint32_t maximum_tracks = 256;
    uint64_t maximum_events = 1000000;
    // Parsing happens on the import planning worker, but large scores should
    // still honor the normal import cancellation path promptly.
    std::function<bool()> cancellation_requested;
};

// Options for lowering a normalized MIDI score to native note-block
// `playsound` command blocks. MIDI timing is quantized here, once, to the game
// tick; it is not represented as fake PCM frames.
struct MidiCommandMusicOptions {
    SchematicParseOptions output;
    uint32_t minecraft_ticks_per_second = 20;
    uint64_t initial_delay_ticks = 0;
    int32_t minimum_y = CommandMusicChainLayout::kDefaultMinimumY;
    int32_t maximum_y = CommandMusicChainLayout::kDefaultMaximumY;
    // Bedrock's native note-block samples have a useful two-octave pitch
    // range. Fold out-of-range melodic notes by octaves before generation,
    // while retaining their untouched original MIDI pitch in MidiNoteEvent.
    bool fold_notes_to_native_range = true;
    bool apply_pitch_bend = true;
    bool include_percussion = true;
};

// Map General MIDI program/percussion information onto the fixed native
// note-block sound table accepted by CommandMusicParser.
CommandMusicInstrument midiCommandMusicInstrumentForProgram(uint8_t program);
CommandMusicInstrument midiCommandMusicInstrumentForPercussionNote(uint8_t note);

// Frequency at A4=440Hz after the captured note-on pitch-wheel state.  The
// caller chooses whether the captured bend should be applied.
double midiCommandMusicFrequency(const MidiNoteEvent& note, bool apply_pitch_bend);

class MidiCommandMusicParser {
public:
    static constexpr const char* kVersion = "command-music-midi-v1";

    bool parseFile(const std::string& source_path,
                   const MidiParseOptions& options,
                   MidiParseResult* result,
                   std::string* error = nullptr) const;

    bool parseBytes(const uint8_t* data, size_t size,
                    const MidiParseOptions& options,
                    MidiParseResult* result,
                    std::string* error = nullptr) const;

    // Produces one absolute-tick event per MIDI note-on and delegates shell,
    // chain layout, deferred command payloads, cancellation and progress to
    // the existing CommandMusicParser pipeline. Same-tick notes are retained
    // in source order and therefore become zero-delay chord command blocks.
    bool buildCommandMusic(const MidiParseResult& midi,
                           const MidiCommandMusicOptions& options,
                           SchematicParseResult* result,
                           std::string* error = nullptr) const;

    bool parseFileToCommandMusic(const std::string& source_path,
                                 const MidiParseOptions& parse_options,
                                 const MidiCommandMusicOptions& command_options,
                                 MidiParseResult* midi_result,
                                 SchematicParseResult* result,
                                 std::string* error = nullptr) const;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MIDI_COMMAND_MUSIC_PARSER_H
