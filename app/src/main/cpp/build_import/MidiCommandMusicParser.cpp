#include "MidiCommandMusicParser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace build_import {
namespace {

constexpr uint32_t kDefaultTempoMicrosecondsPerQuarter = 500000;
constexpr uint16_t kDefaultPitchBendRangeCents = 200;
constexpr uint64_t kMicrosecondsPerSecond = 1000000;
constexpr double kA4FrequencyHz = 440.0;
constexpr double kNativeMinimumMidi = 54.0;  // F#3.
constexpr double kNativeMaximumMidi = 78.0;  // F#5.

bool fail(std::string* error, const std::string& detail) {
    if (error) *error = detail;
    return false;
}

bool cancellationRequested(const MidiParseOptions& options, std::string* error) {
    if (options.cancellation_requested && options.cancellation_requested()) {
        return fail(error, "MIDI import cancelled");
    }
    return false;
}

bool checkedAdd(uint64_t left, uint64_t right, uint64_t* output,
                std::string* error, const char* detail) {
    if (!output) return fail(error, "MIDI integer output is missing");
    if (right > std::numeric_limits<uint64_t>::max() - left) {
        return fail(error, detail);
    }
    *output = left + right;
    return true;
}

class ByteReader {
public:
    ByteReader(const uint8_t* begin, size_t size) : current_(begin), end_(begin + size) {}

    size_t remaining() const { return static_cast<size_t>(end_ - current_); }

    bool readByte(uint8_t* value, std::string* error) {
        if (!value) return fail(error, "MIDI byte output is missing");
        if (current_ == end_) return fail(error, "unexpected end of MIDI data");
        *value = *current_++;
        return true;
    }

    bool readBigEndian16(uint16_t* value, std::string* error) {
        if (!value) return fail(error, "MIDI 16-bit output is missing");
        uint8_t high = 0;
        uint8_t low = 0;
        if (!readByte(&high, error) || !readByte(&low, error)) return false;
        *value = static_cast<uint16_t>((static_cast<uint16_t>(high) << 8U) | low);
        return true;
    }

    bool readBigEndian32(uint32_t* value, std::string* error) {
        if (!value) return fail(error, "MIDI 32-bit output is missing");
        uint8_t bytes[4]{};
        for (uint8_t& byte : bytes) {
            if (!readByte(&byte, error)) return false;
        }
        *value = (static_cast<uint32_t>(bytes[0]) << 24U) |
                 (static_cast<uint32_t>(bytes[1]) << 16U) |
                 (static_cast<uint32_t>(bytes[2]) << 8U) |
                 static_cast<uint32_t>(bytes[3]);
        return true;
    }

    bool readBytes(size_t count, const uint8_t** bytes, std::string* error) {
        if (!bytes) return fail(error, "MIDI byte-range output is missing");
        if (count > remaining()) return fail(error, "truncated MIDI event payload");
        *bytes = current_;
        current_ += count;
        return true;
    }

    bool skip(size_t count, std::string* error) {
        const uint8_t* ignored = nullptr;
        return readBytes(count, &ignored, error);
    }

    bool readVariableLength(uint32_t* value, std::string* error) {
        if (!value) return fail(error, "MIDI variable-length output is missing");
        uint32_t decoded = 0;
        for (uint32_t index = 0; index < 4; ++index) {
            uint8_t byte = 0;
            if (!readByte(&byte, error)) return false;
            decoded = static_cast<uint32_t>((decoded << 7U) | (byte & 0x7FU));
            if ((byte & 0x80U) == 0U) {
                *value = decoded;
                return true;
            }
        }
        return fail(error, "SMF variable-length value exceeds four bytes");
    }

private:
    const uint8_t* current_ = nullptr;
    const uint8_t* end_ = nullptr;
};

enum class RawMidiEventKind : uint8_t {
    NoteOff,
    NoteOn,
    ProgramChange,
    ControlChange,
    PitchBend,
    Tempo,
};

struct RawMidiEvent {
    uint64_t tick = 0;
    uint64_t sequence = 0;
    uint16_t track = 0;
    RawMidiEventKind kind = RawMidiEventKind::NoteOn;
    uint8_t channel = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
    uint32_t value = 0;
};

uint8_t rawEventPriority(RawMidiEventKind kind) {
    // A note-off at the same tick as a re-articulation must close the old
    // voice first. Channel state changes are then visible to the new note-on.
    switch (kind) {
        case RawMidiEventKind::NoteOff: return 0;
        case RawMidiEventKind::ControlChange: return 1;
        case RawMidiEventKind::ProgramChange: return 2;
        case RawMidiEventKind::PitchBend: return 3;
        case RawMidiEventKind::NoteOn: return 4;
        case RawMidiEventKind::Tempo: return 5;
    }
    return 5;
}

bool appendRawEvent(std::vector<RawMidiEvent>* events, RawMidiEvent event,
                    uint64_t* sequence, std::string* error) {
    if (!events || !sequence) return fail(error, "MIDI event storage is missing");
    event.sequence = *sequence;
    if (*sequence == std::numeric_limits<uint64_t>::max()) {
        return fail(error, "MIDI event ordering counter overflows");
    }
    ++*sequence;
    events->push_back(event);
    return true;
}

bool countParsedEvent(uint64_t* count, const MidiParseOptions& options,
                      std::string* error) {
    if (!count) return fail(error, "MIDI event counter is missing");
    if (*count >= options.maximum_events) {
        return fail(error, "MIDI event count exceeds the configured safety limit");
    }
    ++*count;
    return true;
}

bool readChannelData(ByteReader* reader, bool has_first_data, uint8_t first_data,
                     uint8_t data_count, uint8_t* data1, uint8_t* data2,
                     std::string* error) {
    if (!reader || !data1 || !data2) return fail(error, "MIDI channel data output is missing");
    if (data_count != 1U && data_count != 2U) {
        return fail(error, "unsupported MIDI channel message length");
    }
    uint8_t first = first_data;
    if (!has_first_data && !reader->readByte(&first, error)) return false;
    if ((first & 0x80U) != 0U) return fail(error, "MIDI channel data byte has status bit set");
    *data1 = first;
    *data2 = 0;
    if (data_count == 2U) {
        uint8_t second = 0;
        if (!reader->readByte(&second, error)) return false;
        if ((second & 0x80U) != 0U) return fail(error, "MIDI channel data byte has status bit set");
        *data2 = second;
    }
    return true;
}

bool parseTrack(const uint8_t* data, size_t size, uint16_t track_index,
                const MidiParseOptions& options, std::vector<RawMidiEvent>* events,
                uint64_t* sequence, uint64_t* parsed_event_count,
                uint64_t* end_tick, std::string* error) {
    if (!data || !events || !sequence || !parsed_event_count || !end_tick) {
        return fail(error, "MIDI track parser output is missing");
    }
    ByteReader reader(data, size);
    uint64_t absolute_tick = 0;
    uint8_t running_status = 0;

    while (reader.remaining() != 0U) {
        if (cancellationRequested(options, error)) return false;
        uint32_t delta = 0;
        if (!reader.readVariableLength(&delta, error) ||
            !checkedAdd(absolute_tick, delta, &absolute_tick, error,
                        "MIDI absolute tick overflows")) {
            return false;
        }
        if (!countParsedEvent(parsed_event_count, options, error)) return false;

        uint8_t first = 0;
        if (!reader.readByte(&first, error)) return false;
        const bool has_first_data = (first & 0x80U) == 0U;
        uint8_t status = first;
        if (has_first_data) {
            if (running_status < 0x80U || running_status >= 0xF0U) {
                return fail(error, "MIDI running-status data has no channel status");
            }
            status = running_status;
        } else if (status < 0xF0U) {
            running_status = status;
        } else {
            // Meta, SysEx and system-common records do not carry SMF running
            // status into the following channel message.
            running_status = 0;
        }

        if (status == 0xFFU) {
            uint8_t meta_type = 0;
            uint32_t length = 0;
            const uint8_t* payload = nullptr;
            if (!reader.readByte(&meta_type, error) ||
                !reader.readVariableLength(&length, error) ||
                !reader.readBytes(static_cast<size_t>(length), &payload, error)) {
                return false;
            }
            if (meta_type == 0x51U) {
                if (length != 3U) return fail(error, "MIDI Set Tempo meta event must contain three bytes");
                const uint32_t tempo = (static_cast<uint32_t>(payload[0]) << 16U) |
                    (static_cast<uint32_t>(payload[1]) << 8U) |
                    static_cast<uint32_t>(payload[2]);
                if (tempo == 0U) return fail(error, "MIDI Set Tempo may not be zero");
                if (!appendRawEvent(events, {absolute_tick, 0, track_index,
                                             RawMidiEventKind::Tempo, 0, 0, 0, tempo},
                                    sequence, error)) {
                    return false;
                }
            }
            if (meta_type == 0x2FU) {
                if (length != 0U) return fail(error, "MIDI End-of-Track meta event must be empty");
                *end_tick = absolute_tick;
                return true;
            }
            continue;
        }

        if (status == 0xF0U || status == 0xF7U) {
            uint32_t length = 0;
            if (!reader.readVariableLength(&length, error) ||
                !reader.skip(static_cast<size_t>(length), error)) {
                return false;
            }
            continue;
        }

        if (status >= 0xF0U) {
            uint8_t data_count = 0;
            switch (status) {
                case 0xF1U: data_count = 1; break;
                case 0xF2U: data_count = 2; break;
                case 0xF3U: data_count = 1; break;
                case 0xF6U:
                case 0xF8U:
                case 0xF9U:
                case 0xFAU:
                case 0xFBU:
                case 0xFCU:
                case 0xFDU:
                case 0xFEU:
                    data_count = 0;
                    break;
                default:
                    return fail(error, "unsupported MIDI system status in SMF track");
            }
            if (data_count != 0U && !reader.skip(data_count, error)) return false;
            continue;
        }

        const uint8_t message = static_cast<uint8_t>(status & 0xF0U);
        const uint8_t channel = static_cast<uint8_t>(status & 0x0FU);
        const uint8_t data_count = (message == 0xC0U || message == 0xD0U) ? 1U : 2U;
        uint8_t data1 = 0;
        uint8_t data2 = 0;
        if (!readChannelData(&reader, has_first_data, first, data_count, &data1, &data2, error)) {
            return false;
        }

        RawMidiEvent event;
        event.tick = absolute_tick;
        event.track = track_index;
        event.channel = channel;
        event.data1 = data1;
        event.data2 = data2;
        switch (message) {
            case 0x80U:
                event.kind = RawMidiEventKind::NoteOff;
                break;
            case 0x90U:
                event.kind = data2 == 0U ? RawMidiEventKind::NoteOff : RawMidiEventKind::NoteOn;
                break;
            case 0xB0U:
                event.kind = RawMidiEventKind::ControlChange;
                break;
            case 0xC0U:
                event.kind = RawMidiEventKind::ProgramChange;
                break;
            case 0xE0U:
                event.kind = RawMidiEventKind::PitchBend;
                event.value = static_cast<uint32_t>((static_cast<uint32_t>(data2) << 7U) | data1);
                break;
            default:
                // Polyphonic/monophonic aftertouch and unrecognized channel
                // records are valid SMF but do not alter a one-shot playsound.
                continue;
        }
        if (!appendRawEvent(events, event, sequence, error)) return false;
    }

    *end_tick = absolute_tick;
    return true;
}

struct TempoSegment {
    uint64_t start_tick = 0;
    long double start_microseconds = 0.0L;
    uint32_t microseconds_per_quarter_note = kDefaultTempoMicrosecondsPerQuarter;
};

class MidiTimingConverter {
public:
    bool initialize(const std::vector<RawMidiEvent>& events, MidiParseResult* result,
                    std::string* error) {
        if (!result) return fail(error, "MIDI timing result is missing");
        result->tempo_changes.clear();
        std::vector<const RawMidiEvent*> tempos;
        tempos.reserve(events.size());
        for (const RawMidiEvent& event : events) {
            if (event.kind == RawMidiEventKind::Tempo) tempos.push_back(&event);
        }
        std::sort(tempos.begin(), tempos.end(), [](const RawMidiEvent* left,
                                                    const RawMidiEvent* right) {
            return left->tick == right->tick ? left->sequence < right->sequence
                                             : left->tick < right->tick;
        });
        for (const RawMidiEvent* tempo : tempos) {
            result->tempo_changes.push_back({tempo->tick, tempo->value});
        }

        smpte_ = result->uses_smpte_timing;
        if (smpte_) {
            const int32_t code = static_cast<int32_t>(result->smpte_frames_per_second_code);
            long double frames_per_second = 0.0L;
            switch (code) {
                case -24: frames_per_second = 24.0L; break;
                case -25: frames_per_second = 25.0L; break;
                case -29: frames_per_second = 30000.0L / 1001.0L; break;
                case -30: frames_per_second = 30.0L; break;
                default: return fail(error, "unsupported MIDI SMPTE frame-rate division");
            }
            if (result->smpte_ticks_per_frame == 0U) {
                return fail(error, "MIDI SMPTE division has zero ticks per frame");
            }
            microseconds_per_tick_ = kMicrosecondsPerSecond /
                (frames_per_second * static_cast<long double>(result->smpte_ticks_per_frame));
            if (!std::isfinite(microseconds_per_tick_) || microseconds_per_tick_ <= 0.0L) {
                return fail(error, "MIDI SMPTE timing is invalid");
            }
            return true;
        }

        if (result->ticks_per_quarter_note == 0U) {
            return fail(error, "MIDI division has zero ticks per quarter note");
        }
        segments_.clear();
        segments_.push_back({0, 0.0L, kDefaultTempoMicrosecondsPerQuarter});
        for (const RawMidiEvent* tempo : tempos) {
            TempoSegment& previous = segments_.back();
            if (tempo->tick < previous.start_tick) {
                return fail(error, "MIDI tempo events are not ordered");
            }
            const long double elapsed = static_cast<long double>(tempo->tick - previous.start_tick) *
                static_cast<long double>(previous.microseconds_per_quarter_note) /
                static_cast<long double>(result->ticks_per_quarter_note);
            const long double at_change = previous.start_microseconds + elapsed;
            if (!std::isfinite(at_change) ||
                at_change > static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
                return fail(error, "MIDI tempo timeline exceeds supported duration");
            }
            if (tempo->tick == previous.start_tick) {
                previous.microseconds_per_quarter_note = tempo->value;
            } else {
                segments_.push_back({tempo->tick, at_change, tempo->value});
            }
        }
        return true;
    }

    bool microsecondsAt(uint64_t tick, uint64_t* microseconds, std::string* error) const {
        if (!microseconds) return fail(error, "MIDI microsecond output is missing");
        long double value = 0.0L;
        if (smpte_) {
            value = static_cast<long double>(tick) * microseconds_per_tick_;
        } else {
            if (segments_.empty()) return fail(error, "MIDI PPQ timing was not initialized");
            const auto iterator = std::upper_bound(
                segments_.begin(), segments_.end(), tick,
                [](uint64_t value_tick, const TempoSegment& segment) {
                    return value_tick < segment.start_tick;
                });
            const TempoSegment& segment = iterator == segments_.begin()
                ? *segments_.begin() : *std::prev(iterator);
            value = segment.start_microseconds +
                static_cast<long double>(tick - segment.start_tick) *
                static_cast<long double>(segment.microseconds_per_quarter_note) /
                static_cast<long double>(ticks_per_quarter_note_);
        }
        if (!std::isfinite(value) || value < 0.0L ||
            value > static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
            return fail(error, "MIDI timeline exceeds supported duration");
        }
        *microseconds = static_cast<uint64_t>(std::floor(value + 0.5L));
        return true;
    }

    void setTicksPerQuarter(uint16_t ticks_per_quarter_note) {
        ticks_per_quarter_note_ = ticks_per_quarter_note;
    }

private:
    bool smpte_ = false;
    uint16_t ticks_per_quarter_note_ = 0;
    long double microseconds_per_tick_ = 0.0L;
    std::vector<TempoSegment> segments_;
};

enum class NoteFinishReason : uint8_t {
    Normal,
    SustainRelease,
    EndOfFile,
    Forced,
};

struct MidiChannelState {
    uint8_t program = 0;
    bool sustain_down = false;
    int16_t pitch_bend = 0;
    uint16_t pitch_bend_range_cents = kDefaultPitchBendRangeCents;
    uint8_t rpn_msb = 127;
    uint8_t rpn_lsb = 127;
    std::array<std::deque<size_t>, 128> held_notes;
    std::vector<size_t> sustained_notes;
};

bool finishNote(MidiNoteEvent* note, uint64_t tick, NoteFinishReason reason,
                const MidiTimingConverter& timing, std::string* error) {
    if (!note) return fail(error, "MIDI note output is missing");
    note->end_tick = std::max(note->start_tick, tick);
    note->ended_by_sustain_release = reason == NoteFinishReason::SustainRelease;
    note->ended_by_end_of_file = reason == NoteFinishReason::EndOfFile;
    return timing.microsecondsAt(note->end_tick, &note->end_microseconds, error);
}

bool releaseSustainedNotes(MidiChannelState* state, std::vector<MidiNoteEvent>* notes,
                           uint64_t tick, const MidiTimingConverter& timing,
                           std::string* error) {
    if (!state || !notes) return fail(error, "MIDI sustain state is missing");
    for (size_t index : state->sustained_notes) {
        if (index >= notes->size()) return fail(error, "MIDI sustained note index is invalid");
        if (!finishNote(&(*notes)[index], tick, NoteFinishReason::SustainRelease, timing, error)) {
            return false;
        }
    }
    state->sustained_notes.clear();
    return true;
}

bool releaseOneHeldNote(MidiChannelState* state, std::vector<MidiNoteEvent>* notes,
                        uint8_t note_number, uint8_t release_velocity, uint64_t tick,
                        const MidiTimingConverter& timing, std::string* error) {
    if (!state || !notes) return fail(error, "MIDI held-note state is missing");
    std::deque<size_t>& matching_notes = state->held_notes[note_number];
    if (matching_notes.empty()) return true;
    const size_t index = matching_notes.front();
    matching_notes.pop_front();
    if (index >= notes->size()) return fail(error, "MIDI held note index is invalid");
    MidiNoteEvent& note = (*notes)[index];
    note.has_key_release = true;
    note.key_release_tick = tick;
    note.release_velocity = release_velocity;
    if (state->sustain_down) {
        note.sustain_extended = true;
        state->sustained_notes.push_back(index);
        return true;
    }
    return finishNote(&note, tick, NoteFinishReason::Normal, timing, error);
}

bool releaseAllHeldNotes(MidiChannelState* state, std::vector<MidiNoteEvent>* notes,
                         uint64_t tick, const MidiTimingConverter& timing,
                         std::string* error) {
    if (!state || !notes) return fail(error, "MIDI held-note state is missing");
    for (std::deque<size_t>& matching_notes : state->held_notes) {
        while (!matching_notes.empty()) {
            const size_t index = matching_notes.front();
            matching_notes.pop_front();
            if (index >= notes->size()) return fail(error, "MIDI held note index is invalid");
            MidiNoteEvent& note = (*notes)[index];
            note.has_key_release = true;
            note.key_release_tick = tick;
            if (state->sustain_down) {
                note.sustain_extended = true;
                state->sustained_notes.push_back(index);
            } else if (!finishNote(&note, tick, NoteFinishReason::Normal, timing, error)) {
                return false;
            }
        }
    }
    return true;
}

bool forceEndAllNotes(MidiChannelState* state, std::vector<MidiNoteEvent>* notes,
                      uint64_t tick, NoteFinishReason reason,
                      const MidiTimingConverter& timing, std::string* error) {
    if (!state || !notes) return fail(error, "MIDI note state is missing");
    for (std::deque<size_t>& matching_notes : state->held_notes) {
        while (!matching_notes.empty()) {
            const size_t index = matching_notes.front();
            matching_notes.pop_front();
            if (index >= notes->size()) return fail(error, "MIDI held note index is invalid");
            if (!finishNote(&(*notes)[index], tick, reason, timing, error)) return false;
        }
    }
    for (size_t index : state->sustained_notes) {
        if (index >= notes->size()) return fail(error, "MIDI sustained note index is invalid");
        if (!finishNote(&(*notes)[index], tick, reason, timing, error)) return false;
    }
    state->sustained_notes.clear();
    return true;
}

bool applyControlChange(MidiChannelState* state, std::vector<MidiNoteEvent>* notes,
                        uint8_t controller, uint8_t value, uint64_t tick,
                        const MidiTimingConverter& timing, std::string* error) {
    if (!state || !notes) return fail(error, "MIDI control state is missing");
    switch (controller) {
        case 64: {
            const bool next_sustain_down = value >= 64U;
            if (state->sustain_down && !next_sustain_down &&
                !releaseSustainedNotes(state, notes, tick, timing, error)) {
                return false;
            }
            state->sustain_down = next_sustain_down;
            return true;
        }
        case 100:
            state->rpn_lsb = value;
            return true;
        case 101:
            state->rpn_msb = value;
            return true;
        case 6:
            if (state->rpn_msb == 0U && state->rpn_lsb == 0U) {
                state->pitch_bend_range_cents = static_cast<uint16_t>(value) * 100U +
                    static_cast<uint16_t>(state->pitch_bend_range_cents % 100U);
            }
            return true;
        case 38:
            if (state->rpn_msb == 0U && state->rpn_lsb == 0U) {
                state->pitch_bend_range_cents = static_cast<uint16_t>(
                    static_cast<uint16_t>(state->pitch_bend_range_cents / 100U) * 100U +
                    std::min<uint16_t>(value, 99U));
            }
            return true;
        case 120:  // All Sound Off.
            return forceEndAllNotes(state, notes, tick, NoteFinishReason::Forced, timing, error);
        case 121:  // Reset All Controllers.
            if (state->sustain_down && !releaseSustainedNotes(state, notes, tick, timing, error)) {
                return false;
            }
            state->sustain_down = false;
            state->pitch_bend = 0;
            state->pitch_bend_range_cents = kDefaultPitchBendRangeCents;
            state->rpn_msb = 127;
            state->rpn_lsb = 127;
            return true;
        case 123:  // All Notes Off.
            return releaseAllHeldNotes(state, notes, tick, timing, error);
        default:
            return true;
    }
}

bool quantizeMinecraftTick(uint64_t microseconds, uint32_t ticks_per_second,
                           uint64_t* tick, std::string* error) {
    if (!tick) return fail(error, "Minecraft tick output is missing");
    if (ticks_per_second == 0U) return fail(error, "Minecraft tick rate must be positive");
    if (microseconds > std::numeric_limits<uint64_t>::max() / ticks_per_second) {
        return fail(error, "MIDI duration exceeds Minecraft command timeline");
    }
    const uint64_t scaled = microseconds * static_cast<uint64_t>(ticks_per_second);
    *tick = scaled / kMicrosecondsPerSecond +
        (scaled % kMicrosecondsPerSecond >= kMicrosecondsPerSecond / 2U ? 1U : 0U);
    return true;
}

double commandFrequencyForMidiNote(const MidiNoteEvent& note, bool apply_pitch_bend,
                                   bool fold_notes_to_native_range) {
    double semitone = static_cast<double>(note.note);
    if (apply_pitch_bend) {
        semitone += static_cast<double>(note.pitch_bend) *
            static_cast<double>(note.pitch_bend_range_cents) / 8192.0 / 100.0;
    }
    if (fold_notes_to_native_range) {
        while (semitone < kNativeMinimumMidi) semitone += 12.0;
        while (semitone > kNativeMaximumMidi) semitone -= 12.0;
    }
    return kA4FrequencyHz * std::pow(2.0, (semitone - 69.0) / 12.0);
}

}  // namespace

CommandMusicInstrument midiCommandMusicInstrumentForProgram(uint8_t program) {
    // General MIDI program numbers are zero-based in MIDI bytes. This keeps a
    // deliberately small, deterministic mapping onto Bedrock's fixed native
    // note-block sound table rather than letting a source name enter command
    // text.
    if (program <= 7U) return CommandMusicInstrument::Harp;          // Piano.
    if (program <= 15U) return CommandMusicInstrument::Bell;         // Chromatic percussion.
    if (program <= 23U) return CommandMusicInstrument::Bit;          // Organ.
    if (program <= 31U) return CommandMusicInstrument::Guitar;       // Guitar.
    if (program <= 39U) return CommandMusicInstrument::Bass;         // Bass.
    if (program <= 47U) return CommandMusicInstrument::Flute;        // Strings.
    if (program <= 55U) return CommandMusicInstrument::Didgeridoo;   // Ensemble/brass.
    if (program <= 71U) return CommandMusicInstrument::Flute;        // Reed/pipe.
    if (program <= 87U) return CommandMusicInstrument::Pling;        // Synth lead/pad.
    if (program <= 103U) return CommandMusicInstrument::Bit;         // FX.
    if (program <= 111U) return CommandMusicInstrument::Banjo;       // Ethnic.
    if (program <= 119U) return CommandMusicInstrument::Xylophone;   // Percussive.
    return CommandMusicInstrument::Pling;                             // Sound effects.
}

CommandMusicInstrument midiCommandMusicInstrumentForPercussionNote(uint8_t note) {
    switch (note) {
        case 35:
        case 36:
            return CommandMusicInstrument::BassDrum;
        case 38:
        case 40:
            return CommandMusicInstrument::Snare;
        case 42:
        case 44:
        case 46:
        case 49:
        case 51:
        case 52:
        case 53:
        case 55:
        case 57:
        case 59:
            return CommandMusicInstrument::Hat;
        case 56:
            return CommandMusicInstrument::CowBell;
        case 60:
        case 61:
        case 62:
        case 63:
        case 64:
            return CommandMusicInstrument::Bell;
        default:
            return note < 38U ? CommandMusicInstrument::BassDrum
                              : CommandMusicInstrument::Snare;
    }
}

double midiCommandMusicFrequency(const MidiNoteEvent& note, bool apply_pitch_bend) {
    return commandFrequencyForMidiNote(note, apply_pitch_bend, false);
}

bool MidiCommandMusicParser::parseFile(const std::string& source_path,
                                        const MidiParseOptions& options,
                                        MidiParseResult* result,
                                        std::string* error) const {
    if (!result) return fail(error, "MIDI parse result is missing");
    *result = {};
    if (source_path.empty()) return fail(error, "MIDI source path is empty");
    if (cancellationRequested(options, error)) return false;
    if (options.maximum_file_bytes == 0U || options.maximum_tracks == 0U ||
        options.maximum_events == 0U) {
        return fail(error, "MIDI parser safety limits must be positive");
    }
    std::ifstream input(source_path, std::ios::binary | std::ios::ate);
    if (!input) return fail(error, "cannot open MIDI source file");
    const std::streamoff end_position = input.tellg();
    if (end_position < 0) return fail(error, "cannot read MIDI source size");
    const uint64_t byte_count = static_cast<uint64_t>(end_position);
    if (byte_count > options.maximum_file_bytes ||
        byte_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return fail(error, "MIDI source exceeds the configured file-size limit");
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(byte_count));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!input && !bytes.empty()) return fail(error, "cannot read MIDI source file");
    return parseBytes(bytes.data(), bytes.size(), options, result, error);
}

bool MidiCommandMusicParser::parseBytes(const uint8_t* data, size_t size,
                                         const MidiParseOptions& options,
                                         MidiParseResult* result,
                                         std::string* error) const {
    if (!result) return fail(error, "MIDI parse result is missing");
    *result = {};
    if (cancellationRequested(options, error)) return false;
    if (!data || size == 0U) return fail(error, "MIDI source is empty");
    if (size > options.maximum_file_bytes || options.maximum_file_bytes == 0U ||
        options.maximum_tracks == 0U || options.maximum_events == 0U) {
        return fail(error, "MIDI source exceeds configured parser safety limits");
    }

    ByteReader reader(data, size);
    const uint8_t* header_magic = nullptr;
    uint32_t header_length = 0;
    if (!reader.readBytes(4, &header_magic, error) ||
        header_magic[0] != 'M' || header_magic[1] != 'T' ||
        header_magic[2] != 'h' || header_magic[3] != 'd' ||
        !reader.readBigEndian32(&header_length, error)) {
        return fail(error, "MIDI source is missing a valid MThd header");
    }
    if (header_length < 6U || header_length > reader.remaining()) {
        return fail(error, "MIDI MThd header has an invalid length");
    }
    const uint8_t* header = nullptr;
    if (!reader.readBytes(static_cast<size_t>(header_length), &header, error)) return false;
    ByteReader header_reader(header, static_cast<size_t>(header_length));
    uint16_t format = 0;
    uint16_t track_count = 0;
    uint16_t division = 0;
    if (!header_reader.readBigEndian16(&format, error) ||
        !header_reader.readBigEndian16(&track_count, error) ||
        !header_reader.readBigEndian16(&division, error)) {
        return false;
    }
    if (format > 1U) return fail(error, "MIDI format 2 is not mergeable into one command sequence");
    if (track_count == 0U || track_count > options.maximum_tracks) {
        return fail(error, "MIDI track count is outside the configured safety limit");
    }
    if (format == 0U && track_count != 1U) {
        return fail(error, "MIDI format 0 must declare exactly one track");
    }

    result->format = format;
    result->declared_track_count = track_count;
    result->division = division;
    if ((division & 0x8000U) == 0U) {
        result->ticks_per_quarter_note = division;
        if (division == 0U) return fail(error, "MIDI PPQ division may not be zero");
    } else {
        result->uses_smpte_timing = true;
        result->smpte_frames_per_second_code = static_cast<int8_t>(division >> 8U);
        result->smpte_ticks_per_frame = static_cast<uint8_t>(division & 0xFFU);
        if (result->smpte_ticks_per_frame == 0U) {
            return fail(error, "MIDI SMPTE division may not have zero ticks per frame");
        }
    }

    std::vector<RawMidiEvent> events;
    events.reserve(static_cast<size_t>(std::min<uint64_t>(options.maximum_events, 4096U)));
    uint64_t sequence = 0;
    uint64_t parsed_event_count = 0;
    for (uint16_t track = 0; track < track_count; ++track) {
        if (cancellationRequested(options, error)) return false;
        const uint8_t* track_magic = nullptr;
        uint32_t track_length = 0;
        if (!reader.readBytes(4, &track_magic, error) ||
            track_magic[0] != 'M' || track_magic[1] != 'T' ||
            track_magic[2] != 'r' || track_magic[3] != 'k' ||
            !reader.readBigEndian32(&track_length, error)) {
            return fail(error, "MIDI source is missing an MTrk chunk");
        }
        const uint8_t* track_data = nullptr;
        if (!reader.readBytes(static_cast<size_t>(track_length), &track_data, error)) return false;
        uint64_t track_end_tick = 0;
        if (!parseTrack(track_data, static_cast<size_t>(track_length), track, options, &events,
                        &sequence, &parsed_event_count, &track_end_tick, error)) {
            return false;
        }
        result->end_tick = std::max(result->end_tick, track_end_tick);
    }

    MidiTimingConverter timing;
    timing.setTicksPerQuarter(result->ticks_per_quarter_note);
    if (!timing.initialize(events, result, error) ||
        !timing.microsecondsAt(result->end_tick, &result->duration_microseconds, error)) {
        return false;
    }

    std::stable_sort(events.begin(), events.end(), [](const RawMidiEvent& left,
                                                       const RawMidiEvent& right) {
        if (left.tick != right.tick) return left.tick < right.tick;
        const uint8_t left_priority = rawEventPriority(left.kind);
        const uint8_t right_priority = rawEventPriority(right.kind);
        return left_priority == right_priority ? left.sequence < right.sequence
                                               : left_priority < right_priority;
    });

    std::array<MidiChannelState, 16> channels;
    result->notes.reserve(events.size());
    for (size_t event_index = 0; event_index < events.size(); ++event_index) {
        if ((event_index & 0x3FFU) == 0U && cancellationRequested(options, error)) {
            return false;
        }
        const RawMidiEvent& event = events[event_index];
        if (event.kind == RawMidiEventKind::Tempo) continue;
        MidiChannelState& channel = channels[event.channel];
        switch (event.kind) {
            case RawMidiEventKind::NoteOff:
                if (!releaseOneHeldNote(&channel, &result->notes, event.data1, event.data2,
                                        event.tick, timing, error)) {
                    return false;
                }
                break;
            case RawMidiEventKind::ControlChange:
                result->control_changes.push_back({event.tick, event.channel, event.data1, event.data2});
                if (!applyControlChange(&channel, &result->notes, event.data1, event.data2,
                                        event.tick, timing, error)) {
                    return false;
                }
                break;
            case RawMidiEventKind::ProgramChange:
                channel.program = event.data1;
                result->program_changes.push_back({event.tick, event.channel, event.data1});
                break;
            case RawMidiEventKind::PitchBend: {
                const int32_t signed_bend = static_cast<int32_t>(event.value) - 8192;
                channel.pitch_bend = static_cast<int16_t>(signed_bend);
                result->pitch_bends.push_back({event.tick, event.channel, channel.pitch_bend});
                break;
            }
            case RawMidiEventKind::NoteOn: {
                MidiNoteEvent note;
                note.start_tick = event.tick;
                note.end_tick = event.tick;
                note.channel = event.channel;
                note.note = event.data1;
                note.velocity = event.data2;
                note.program = channel.program;
                note.pitch_bend = channel.pitch_bend;
                note.pitch_bend_range_cents = channel.pitch_bend_range_cents;
                note.percussion = event.channel == 9U;
                if (!timing.microsecondsAt(event.tick, &note.start_microseconds, error)) return false;
                note.end_microseconds = note.start_microseconds;
                if (result->notes.size() == std::numeric_limits<size_t>::max()) {
                    return fail(error, "MIDI note count overflows");
                }
                const size_t note_index = result->notes.size();
                result->notes.push_back(note);
                channel.held_notes[event.data1].push_back(note_index);
                break;
            }
            case RawMidiEventKind::Tempo:
                break;
        }
    }

    for (MidiChannelState& channel : channels) {
        if (!forceEndAllNotes(&channel, &result->notes, result->end_tick,
                              NoteFinishReason::EndOfFile, timing, error)) {
            return false;
        }
    }
    return true;
}

bool MidiCommandMusicParser::buildCommandMusic(const MidiParseResult& midi,
                                                const MidiCommandMusicOptions& options,
                                                SchematicParseResult* result,
                                                std::string* error) const {
    if (!result) return fail(error, "MIDI command-music result is missing");
    if (options.minecraft_ticks_per_second == 0U) {
        return fail(error, "Minecraft tick rate must be positive");
    }
    std::vector<CommandMusicTickEvent> events;
    events.reserve(midi.notes.size());
    for (size_t note_index = 0; note_index < midi.notes.size(); ++note_index) {
        if ((note_index & 0x3FFU) == 0U && options.output.cancellation_requested &&
            options.output.cancellation_requested()) {
            return fail(error, "MIDI import cancelled");
        }
        const MidiNoteEvent& note = midi.notes[note_index];
        if (note.velocity == 0U || (note.percussion && !options.include_percussion)) continue;
        uint64_t tick = 0;
        if (!quantizeMinecraftTick(note.start_microseconds, options.minecraft_ticks_per_second,
                                   &tick, error)) {
            return false;
        }
        const double frequency = commandFrequencyForMidiNote(
            note, options.apply_pitch_bend, options.fold_notes_to_native_range);
        if (!std::isfinite(frequency) || frequency <= 0.0) {
            return fail(error, "MIDI note produces an invalid playsound frequency");
        }
        const CommandMusicInstrument instrument = note.percussion
            ? midiCommandMusicInstrumentForPercussionNote(note.note)
            : midiCommandMusicInstrumentForProgram(note.program);
        events.push_back({tick, frequency,
                          static_cast<float>(static_cast<double>(note.velocity) / 127.0),
                          instrument});
    }
    if (events.empty()) return fail(error, "MIDI source contains no renderable note-on events");
    std::stable_sort(events.begin(), events.end(), [](const CommandMusicTickEvent& left,
                                                       const CommandMusicTickEvent& right) {
        return left.tick < right.tick;
    });

    CommandMusicParseOptions command_options;
    command_options.output = options.output;
    command_options.initial_delay_ticks = options.initial_delay_ticks;
    command_options.minimum_y = options.minimum_y;
    command_options.maximum_y = options.maximum_y;
    command_options.tick_events = std::move(events);
    return CommandMusicParser().parse(command_options, result, error);
}

bool MidiCommandMusicParser::parseFileToCommandMusic(
    const std::string& source_path, const MidiParseOptions& parse_options,
    const MidiCommandMusicOptions& command_options, MidiParseResult* midi_result,
    SchematicParseResult* result, std::string* error) const {
    MidiParseResult parsed;
    if (!parseFile(source_path, parse_options, &parsed, error)) return false;
    if (!buildCommandMusic(parsed, command_options, result, error)) return false;
    if (midi_result) *midi_result = std::move(parsed);
    return true;
}

}  // namespace build_import
