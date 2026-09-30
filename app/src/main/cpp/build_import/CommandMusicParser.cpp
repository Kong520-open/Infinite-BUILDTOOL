#include "CommandMusicParser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

namespace build_import {
namespace {

constexpr double kNativeNoteBlockReferenceFrequencyHz = 369.9944227116344;  // F#4.
constexpr double kMinimumNativeNoteBlockPitch = 0.5;
constexpr double kMaximumNativeNoteBlockPitch = 2.0;
constexpr uint32_t kDefaultTicksPerSecond = 20;

bool fail(std::string* error, const std::string& detail) {
    if (error) *error = detail;
    return false;
}

bool cancellationRequested(const SchematicParseOptions& options, std::string* error) {
    if (options.cancellation_requested && options.cancellation_requested()) {
        return fail(error, "music import cancelled");
    }
    return false;
}

void extendBounds(BlockBounds* bounds, const CommandMusicChainPosition& position) {
    if (!bounds) return;
    if (!bounds->isValid()) {
        bounds->min_x = bounds->max_x = position.x;
        bounds->min_y = bounds->max_y = position.y;
        bounds->min_z = bounds->max_z = position.z;
        return;
    }
    bounds->min_x = std::min(bounds->min_x, position.x);
    bounds->min_y = std::min(bounds->min_y, position.y);
    bounds->min_z = std::min(bounds->min_z, position.z);
    bounds->max_x = std::max(bounds->max_x, position.x);
    bounds->max_y = std::max(bounds->max_y, position.y);
    bounds->max_z = std::max(bounds->max_z, position.z);
}

std::string decimalCommandValue(double value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(3) << value;
    return stream.str();
}

struct PendingCommandBlock {
    CommandMusicChainPosition position;
    CommandBlockRecord record;
    bool is_root = false;
};

}  // namespace

CommandMusicChainLayout::CommandMusicChainLayout(int32_t base_x, int32_t base_y,
                                                  int32_t base_z, int32_t minimum_y,
                                                  int32_t maximum_y)
    : base_y_(base_y), minimum_y_(minimum_y), maximum_y_(maximum_y) {
    if (minimum_y_ > maximum_y_) {
        error_ = "music layout minimum Y is greater than maximum Y";
        return;
    }
    if (base_y_ < minimum_y_ || base_y_ > maximum_y_) {
        error_ = "music layout start Y is outside the configured world height";
        return;
    }

    const int64_t chunk_x = static_cast<int64_t>(floorDiv(base_x, kNativeChunkSize));
    const int64_t chunk_z = static_cast<int64_t>(floorDiv(base_z, kNativeChunkSize));
    const int64_t min_x = chunk_x * kNativeChunkSize;
    const int64_t min_z = chunk_z * kNativeChunkSize;
    if (min_x < std::numeric_limits<int32_t>::min() ||
        min_x > std::numeric_limits<int32_t>::max() ||
        min_z < std::numeric_limits<int32_t>::min() ||
        min_z > std::numeric_limits<int32_t>::max()) {
        error_ = "music layout chunk origin is outside the block-coordinate range";
        return;
    }

    const uint64_t layers = static_cast<uint64_t>(
        static_cast<int64_t>(maximum_y_) - static_cast<int64_t>(base_y_) + 1);
    if (layers == 0 || layers > std::numeric_limits<uint64_t>::max() / kBlocksPerLayer) {
        error_ = "music layout capacity overflows";
        return;
    }

    chunk_min_x_ = static_cast<int32_t>(min_x);
    chunk_min_z_ = static_cast<int32_t>(min_z);
    capacity_ = layers * kBlocksPerLayer;
    valid_ = true;
}

bool CommandMusicChainLayout::positionForIndex(uint64_t index,
                                                 CommandMusicChainPosition* position,
                                                 std::string* error) const {
    if (!position) return fail(error, "music layout position output is missing");
    if (!valid_) return fail(error, error_.empty() ? "music layout is invalid" : error_);
    if (index >= capacity_) {
        return fail(error,
                    "music sequence exceeds the available vertical capacity of its 16x16 chunk");
    }

    const uint64_t layer = index / kBlocksPerLayer;
    const uint64_t in_layer = index % kBlocksPerLayer;
    const uint64_t row = in_layer / static_cast<uint64_t>(kNativeChunkSize);
    const uint64_t column = in_layer % static_cast<uint64_t>(kNativeChunkSize);
    const int32_t x_offset = static_cast<int32_t>(
        (row & 1U) == 0U ? column : static_cast<uint64_t>(kNativeChunkSize - 1) - column);
    const int32_t z_offset = static_cast<int32_t>(
        (layer & 1U) == 0U ? row : static_cast<uint64_t>(kNativeChunkSize - 1) - row);
    const int64_t y = static_cast<int64_t>(base_y_) + static_cast<int64_t>(layer);
    if (y < minimum_y_ || y > maximum_y_) {
        return fail(error, "music layout calculated an out-of-range Y coordinate");
    }

    position->x = static_cast<int32_t>(static_cast<int64_t>(chunk_min_x_) + x_offset);
    position->y = static_cast<int32_t>(y);
    position->z = static_cast<int32_t>(static_cast<int64_t>(chunk_min_z_) + z_offset);
    return true;
}

bool CommandMusicChainLayout::facingAuxToward(const CommandMusicChainPosition& from,
                                                const CommandMusicChainPosition& to,
                                                uint16_t* aux, std::string* error) {
    if (!aux) return fail(error, "music chain facing output is missing");
    const int64_t dx = static_cast<int64_t>(to.x) - from.x;
    const int64_t dy = static_cast<int64_t>(to.y) - from.y;
    const int64_t dz = static_cast<int64_t>(to.z) - from.z;
    if (dx == 0 && dy == -1 && dz == 0) {
        *aux = 0;
        return true;
    }
    if (dx == 0 && dy == 1 && dz == 0) {
        *aux = 1;
        return true;
    }
    if (dx == 0 && dy == 0 && dz == -1) {
        *aux = 2;
        return true;
    }
    if (dx == 0 && dy == 0 && dz == 1) {
        *aux = 3;
        return true;
    }
    if (dx == -1 && dy == 0 && dz == 0) {
        *aux = 4;
        return true;
    }
    if (dx == 1 && dy == 0 && dz == 0) {
        *aux = 5;
        return true;
    }
    return fail(error, "music chain layout produced non-adjacent command blocks");
}

bool commandMusicTickForPcmFrame(uint64_t frame, uint32_t sample_rate_hz,
                                  uint32_t ticks_per_second, uint64_t* tick,
                                  std::string* error) {
    if (!tick) return fail(error, "music tick output is missing");
    if (sample_rate_hz == 0) return fail(error, "PCM sample rate must be positive");
    if (ticks_per_second == 0) return fail(error, "music tick rate must be positive");

    const long double scaled = static_cast<long double>(frame) *
        static_cast<long double>(ticks_per_second) / static_cast<long double>(sample_rate_hz);
    const long double rounded = std::floor(scaled + 0.5L);
    if (!std::isfinite(rounded) ||
        rounded > static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        return fail(error, "PCM event time exceeds the supported command-block timeline");
    }
    *tick = static_cast<uint64_t>(rounded);
    return true;
}

double commandMusicPitchForFrequency(double frequency_hz) {
    if (!std::isfinite(frequency_hz) || frequency_hz <= 0.0) return 1.0;
    const double raw = frequency_hz / kNativeNoteBlockReferenceFrequencyHz;
    return std::max(kMinimumNativeNoteBlockPitch, std::min(kMaximumNativeNoteBlockPitch, raw));
}

std::string_view commandMusicSoundForInstrument(CommandMusicInstrument instrument) {
    static constexpr std::array<std::string_view,
                                static_cast<size_t>(CommandMusicInstrument::Count)> kSounds{{
        "note.harp", "note.bass", "note.basedrum", "note.snare", "note.hat",
        "note.guitar", "note.flute", "note.bell", "note.chime", "note.xylophone",
        "note.iron_xylophone", "note.cow_bell", "note.didgeridoo", "note.bit",
        "note.banjo", "note.pling",
    }};
    const size_t index = static_cast<size_t>(instrument);
    return index < kSounds.size() ? kSounds[index] : std::string_view{};
}

bool commandMusicPlaysoundCommand(const CommandMusicPcmEvent& event,
                                   std::string* command, std::string* error) {
    if (!command) return fail(error, "music command output is missing");
    const std::string_view sound = commandMusicSoundForInstrument(event.instrument);
    if (sound.empty()) return fail(error, "music event uses an unsupported native note-block instrument");
    if (!std::isfinite(event.frequency_hz) || event.frequency_hz <= 0.0) {
        return fail(error, "music event has an invalid frequency");
    }
    if (!std::isfinite(event.amplitude) || event.amplitude < 0.0F) {
        return fail(error, "music event has an invalid amplitude");
    }

    const double volume = std::max(0.0, std::min(1.0, static_cast<double>(event.amplitude)));
    const double pitch = commandMusicPitchForFrequency(event.frequency_hz);
    *command = "playsound ";
    command->append(sound.data(), sound.size());
    command->append(" @a ~ ~ ~ ");
    command->append(decimalCommandValue(volume));
    command->push_back(' ');
    command->append(decimalCommandValue(pitch));
    return true;
}

bool CommandMusicParser::parse(const CommandMusicParseOptions& options,
                                SchematicParseResult* result,
                                std::string* error) const {
    if (!result) return fail(error, "music parse result is missing");
    *result = {};
    if (options.output.chunk_size <= 0) return fail(error, "music import chunk size must be positive");
    const bool has_pcm_vector = !options.events.empty();
    const bool has_pcm_source = static_cast<bool>(options.event_source);
    const bool has_tick_vector = !options.tick_events.empty();
    const bool has_tick_source = static_cast<bool>(options.tick_event_source);
    const uint32_t source_form_count = (has_pcm_vector ? 1U : 0U) +
        (has_pcm_source ? 1U : 0U) + (has_tick_vector ? 1U : 0U) +
        (has_tick_source ? 1U : 0U);
    if (source_form_count != 1U) {
        return fail(error, "music parser accepts exactly one PCM or absolute-tick event source");
    }
    const bool uses_absolute_ticks = has_tick_vector || has_tick_source;
    if (!uses_absolute_ticks && options.pcm_sample_rate_hz == 0) {
        return fail(error, "music PCM sample rate must be positive");
    }
    const uint32_t ticks_per_second = options.ticks_per_second == 0
        ? kDefaultTicksPerSecond : options.ticks_per_second;

    CommandMusicChainLayout layout(options.output.base_x, options.output.base_y,
                                   options.output.base_z, options.minimum_y,
                                   options.maximum_y);
    if (!layout.valid()) return fail(error, layout.error());

    std::unique_ptr<ChunkSpoolWriter> writer;
    if (!options.output.block_sink) {
        writer = std::make_unique<ChunkSpoolWriter>(options.output.spool_directory,
                                                    options.output.chunk_size,
                                                    options.output.maximum_chunk_descriptors);
    }

    const uint64_t progress_total = has_pcm_vector
        ? static_cast<uint64_t>(options.events.size())
        : has_tick_vector
            ? static_cast<uint64_t>(options.tick_events.size())
            : has_pcm_source ? options.event_count_hint : options.tick_event_count_hint;
    if (options.output.progress_callback) {
        options.output.progress_callback({SchematicParseStage::ReadingSource, 0, progress_total});
        options.output.progress_callback({SchematicParseStage::RoutingBlocks, 0, progress_total});
    }

    uint64_t pcm_vector_index = 0;
    uint64_t tick_vector_index = 0;
    const auto nextEvent = [&](CommandMusicPcmEvent* event, uint64_t* source_position,
                               bool* has_event) -> bool {
        if (!event || !source_position || !has_event) {
            return fail(error, "music event source output is missing");
        }
        *has_event = false;
        if (has_tick_source) {
            CommandMusicTickEvent tick_event;
            if (!options.tick_event_source(&tick_event, has_event, error)) {
                if (error && error->empty()) *error = "music absolute-tick event source failed";
                return false;
            }
            if (*has_event) {
                *event = {0, tick_event.frequency_hz, tick_event.amplitude, tick_event.instrument};
                *source_position = tick_event.tick;
            }
            return true;
        }
        if (has_tick_vector) {
            if (tick_vector_index >= options.tick_events.size()) return true;
            const CommandMusicTickEvent& tick_event = options.tick_events[tick_vector_index++];
            *event = {0, tick_event.frequency_hz, tick_event.amplitude, tick_event.instrument};
            *source_position = tick_event.tick;
            *has_event = true;
            return true;
        }
        if (has_pcm_source) {
            if (!options.event_source(event, has_event, error)) {
                if (error && error->empty()) *error = "music event source failed";
                return false;
            }
            if (*has_event) *source_position = event->start_frame;
            return true;
        }
        if (pcm_vector_index >= options.events.size()) return true;
        *event = options.events[pcm_vector_index++];
        *source_position = event->start_frame;
        *has_event = true;
        return true;
    };

    uint64_t allocated_blocks = 0;
    uint64_t emitted_blocks = 0;
    uint64_t consumed_events = 0;
    uint64_t previous_source_position = 0;
    uint64_t previous_tick = 0;
    bool has_previous_event = false;
    std::optional<PendingCommandBlock> pending;

    const auto emitPending = [&](const PendingCommandBlock& current,
                                 const CommandMusicChainPosition* successor) -> bool {
        if (cancellationRequested(options.output, error)) return false;
        if (options.output.maximum_output_blocks != 0 &&
            result->imported_block_count >= options.output.maximum_output_blocks) {
            return fail(error, "generated music command-block count exceeds the configured output limit of " +
                               std::to_string(options.output.maximum_output_blocks));
        }

        uint16_t aux = 5;
        if (successor && !CommandMusicChainLayout::facingAuxToward(current.position,
                                                                     *successor, &aux, error)) {
            return false;
        }

        ParsedBlock block;
        block.world_x = current.position.x;
        block.world_y = current.position.y;
        block.world_z = current.position.z;
        block.spec.command_name = current.is_root ? "minecraft:command_block"
                                                  : "minecraft:chain_command_block";
        block.spec.aux = aux;
        block.spec.phase = ImportPhase::Structure;
        block.spec.can_fill = false;
        block.spec.single_layer_only = false;
        block.spec.stateful = true;

        if (options.output.block_sink) {
            if (!options.output.block_sink(block, error)) {
                if (error && error->empty()) *error = "music block sink rejected a command-block shell";
                return false;
            }
        } else if (!writer || !writer->append(block, error)) {
            if (error && error->empty()) *error = "music block spool output is unavailable";
            return false;
        }

        if (options.output.command_block_sink) {
            if (!options.output.command_block_sink(current.record, error)) {
                if (error && error->empty()) *error = "music command-block sink rejected a payload";
                return false;
            }
            ++result->command_block_payload_count;
        } else {
            ++result->omitted_command_block_data_count;
        }

        extendBounds(&result->source_volume_bounds, current.position);
        if (result->source_voxel_count == std::numeric_limits<uint64_t>::max() ||
            result->imported_block_count == std::numeric_limits<uint64_t>::max() ||
            emitted_blocks == std::numeric_limits<uint64_t>::max()) {
            return fail(error, "music command-block count overflows");
        }
        ++result->source_voxel_count;
        ++result->imported_block_count;
        ++emitted_blocks;
        return true;
    };

    while (true) {
        if (cancellationRequested(options.output, error)) return false;
        CommandMusicPcmEvent event;
        uint64_t source_position = 0;
        bool has_event = false;
        if (!nextEvent(&event, &source_position, &has_event)) return false;
        if (!has_event) break;

        if (has_previous_event && source_position < previous_source_position) {
            return fail(error, uses_absolute_ticks
                ? "music event source is not ordered by absolute Minecraft tick"
                : "music event source is not ordered by PCM frame");
        }
        uint64_t event_tick = source_position;
        if (!uses_absolute_ticks &&
            !commandMusicTickForPcmFrame(source_position, options.pcm_sample_rate_hz,
                                         ticks_per_second, &event_tick, error)) {
            return false;
        }
        if (has_previous_event && event_tick < previous_tick) {
            return fail(error, "music event source is not ordered by Minecraft tick");
        }

        const uint64_t delay = has_previous_event ? event_tick - previous_tick
                                                   : options.initial_delay_ticks;
        if (delay > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
            return fail(error, "music note gap exceeds the command-block tick-delay limit");
        }

        std::string command;
        if (!commandMusicPlaysoundCommand(event, &command, error)) return false;
        CommandMusicChainPosition position;
        if (!layout.positionForIndex(allocated_blocks, &position, error)) return false;

        PendingCommandBlock next;
        next.position = position;
        next.is_root = !has_previous_event;
        next.record.x = position.x;
        next.record.y = position.y;
        next.record.z = position.z;
        next.record.mode = next.is_root ? kCommandBlockModeImpulse : kCommandBlockModeChain;
        next.record.redstone_mode = next.is_root;
        next.record.conditional = false;
        next.record.command = std::move(command);
        next.record.name = next.is_root ? "Music Start" : "Music Chain";
        next.record.output_tracked = false;
        next.record.tick_delay = static_cast<int32_t>(delay);
        next.record.executing_on_first_tick = false;

        if (allocated_blocks == std::numeric_limits<uint64_t>::max()) {
            return fail(error, "music command-block layout index overflows");
        }
        ++allocated_blocks;
        if (pending && !emitPending(*pending, &next.position)) return false;
        pending = std::move(next);
        previous_source_position = source_position;
        previous_tick = event_tick;
        has_previous_event = true;
        if (consumed_events == std::numeric_limits<uint64_t>::max()) {
            return fail(error, "music event count overflows");
        }
        ++consumed_events;
        if (options.output.progress_callback) {
            options.output.progress_callback(
                {SchematicParseStage::RoutingBlocks, consumed_events, progress_total});
        }
    }

    if (!pending) return fail(error, "music source contains no note-on events");
    if (!emitPending(*pending, nullptr)) return false;

    if (options.output.progress_callback) {
        options.output.progress_callback({SchematicParseStage::FinalizingSpools, 0, 1});
    }
    if (writer) {
        result->chunks = writer->finish(error, options.output.cancellation_requested,
                                        [&](uint64_t completed, uint64_t total) {
            if (options.output.progress_callback) {
                options.output.progress_callback(
                    {SchematicParseStage::FinalizingSpools, completed, total});
            }
        });
        if (error && !error->empty()) return false;
        if (result->chunks.empty()) return fail(error, "music parser produced no chunk spools");
    }
    result->source_region_count = 1;
    return true;
}

}  // namespace build_import
