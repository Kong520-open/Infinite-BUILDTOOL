#ifndef INFINITE_TEXTURE_COMMAND_MUSIC_PARSER_H
#define INFINITE_TEXTURE_COMMAND_MUSIC_PARSER_H

#include "SchematicParser.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace build_import {

// Bedrock's native note-block sound set. This writer deliberately accepts only
// this fixed enum so a malformed score field can never become command text.
enum class CommandMusicInstrument : uint8_t {
    Harp = 0,
    Bass,
    BassDrum,
    Snare,
    Hat,
    Guitar,
    Flute,
    Bell,
    Chime,
    Xylophone,
    IronXylophone,
    CowBell,
    Didgeridoo,
    Bit,
    Banjo,
    Pling,
    Count,
};

// One note onset supplied by an optional PCM-timeline client. The MIDI route
// uses CommandMusicTickEvent below so its native tick timeline remains exact.
struct CommandMusicPcmEvent {
    uint64_t start_frame = 0;
    double frequency_hz = 0.0;
    float amplitude = 1.0F;
    CommandMusicInstrument instrument = CommandMusicInstrument::Harp;
};

// A streaming source returns one event at a time in nondecreasing
// start_frame order.  has_event=false denotes a clean end of source; returning
// false denotes an error and should populate error when practical.
using CommandMusicEventSource = std::function<bool(CommandMusicPcmEvent* event,
                                                    bool* has_event,
                                                    std::string* error)>;

// A music event whose position is already expressed in Minecraft game ticks.
// This avoids forcing exact event sources (such as SMF/MIDI) through the PCM
// frame conversion path merely to reuse the command-block chain writer.
struct CommandMusicTickEvent {
    uint64_t tick = 0;
    double frequency_hz = 0.0;
    float amplitude = 1.0F;
    CommandMusicInstrument instrument = CommandMusicInstrument::Harp;
};

using CommandMusicTickEventSource = std::function<bool(CommandMusicTickEvent* event,
                                                        bool* has_event,
                                                        std::string* error)>;

struct CommandMusicChainPosition {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const CommandMusicChainPosition& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

// Packs the sequencer into the native 16x16 block chunk containing base_x/z.
// Layers are serpentine and alternate their Z travel direction, so the final
// block of one layer is directly below the first block of the next layer.  A
// single chain can therefore cross a layer boundary via an ordinary upward
// facing chain command block rather than an invalid diagonal jump.
class CommandMusicChainLayout {
public:
    static constexpr int32_t kNativeChunkSize = 16;
    // Current Bedrock dimensions use build coordinates -64 through 319. Keep
    // these defaults here rather than silently retaining the old 0..255 range:
    // a player standing on a modern high-altitude build must be able to import
    // the command-music chain at that Y coordinate.
    static constexpr int32_t kDefaultMinimumY = -64;
    static constexpr int32_t kDefaultMaximumY = 319;
    static constexpr uint64_t kBlocksPerLayer =
        static_cast<uint64_t>(kNativeChunkSize) * kNativeChunkSize;

    CommandMusicChainLayout(int32_t base_x, int32_t base_y, int32_t base_z,
                             int32_t minimum_y = kDefaultMinimumY,
                             int32_t maximum_y = kDefaultMaximumY);

    bool valid() const { return valid_; }
    const std::string& error() const { return error_; }
    uint64_t capacity() const { return capacity_; }
    int32_t chunkMinX() const { return chunk_min_x_; }
    int32_t chunkMinZ() const { return chunk_min_z_; }

    bool positionForIndex(uint64_t index, CommandMusicChainPosition* position,
                          std::string* error = nullptr) const;

    // Returns the native Bedrock facing auxiliary value (without the 0x08
    // conditional bit) for two face-adjacent command blocks.
    static bool facingAuxToward(const CommandMusicChainPosition& from,
                                const CommandMusicChainPosition& to,
                                uint16_t* aux, std::string* error = nullptr);

private:
    int32_t chunk_min_x_ = 0;
    int32_t chunk_min_z_ = 0;
    int32_t base_y_ = 0;
    int32_t minimum_y_ = 0;
    int32_t maximum_y_ = -1;
    uint64_t capacity_ = 0;
    bool valid_ = false;
    std::string error_;
};

// Converts a PCM frame offset to a nearest Minecraft game tick. This helper is
// public both for tests and for the streaming piano-score transcriber.
bool commandMusicTickForPcmFrame(uint64_t frame, uint32_t sample_rate_hz,
                                 uint32_t ticks_per_second, uint64_t* tick,
                                 std::string* error = nullptr);

// Native note-block samples are centred on F#4 at playsound pitch 1.0.  The
// returned value is clamped to Minecraft's practical note-block octave range
// [0.5, 2.0], preserving native note-block timbre instead of inventing custom
// resource-pack sounds.
double commandMusicPitchForFrequency(double frequency_hz);

// Returns the fixed Bedrock playsound identifier for an allowed instrument, or
// an empty view for an invalid enum value.
std::string_view commandMusicSoundForInstrument(CommandMusicInstrument instrument);

// A compact, command-safe `playsound` command generated solely from validated
// numeric audio features and the static instrument table above.
bool commandMusicPlaysoundCommand(const CommandMusicPcmEvent& event,
                                  std::string* command,
                                  std::string* error = nullptr);

struct CommandMusicParseOptions {
    // Reuse the ordinary parser's stream sinks, source/bounds settings,
    // cancellation callback and progress channel.  Runtime integration simply
    // copies BuildImportStartRequest::parse here.
    SchematicParseOptions output;

    uint32_t pcm_sample_rate_hz = 0;
    uint32_t ticks_per_second = 20;
    // Added to the first event's source tick.  It permits a generated
    // controller to reserve a short lead-in after the player powers its root
    // impulse command block.
    uint64_t initial_delay_ticks = 0;
    int32_t minimum_y = CommandMusicChainLayout::kDefaultMinimumY;
    int32_t maximum_y = CommandMusicChainLayout::kDefaultMaximumY;

    // Exactly one source form may be used. `events` is convenient for tests
    // and small PCM-timeline clients; event_source permits streaming input.
    std::vector<CommandMusicPcmEvent> events;
    CommandMusicEventSource event_source;
    uint64_t event_count_hint = 0;

    // Absolute Minecraft-tick input for score formats that already own their
    // timing model. This is mutually exclusive with the PCM forms above.
    std::vector<CommandMusicTickEvent> tick_events;
    CommandMusicTickEventSource tick_event_source;
    uint64_t tick_event_count_hint = 0;
};

// Builds a player-triggered sequencer: the first note is an impulse command
// block in redstone mode, every subsequent note is an always-active chain
// command block. Commands at the same audio tick receive tick_delay=0 and play
// as a chord; only the first command at a later tick carries the elapsed delay.
// Leading silence is intentionally removed: the first score note always has
// only initial_delay_ticks (normally zero), never an inherited source intro gap.
// Shell blocks are emitted through ParsedBlockSink and editable fields through
// ParsedCommandBlockSink without retaining the generated blueprint.
class CommandMusicParser {
public:
    static constexpr const char* kVersion = "command-music-sequencer-v3";

    bool parse(const CommandMusicParseOptions& options,
               SchematicParseResult* result,
               std::string* error = nullptr) const;

};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_COMMAND_MUSIC_PARSER_H
