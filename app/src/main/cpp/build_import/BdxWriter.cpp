#include "BdxWriter.h"

#include <brotli/encode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <charconv>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace build_import {
namespace {

bool hasBdxExtension(const std::string& path) {
    constexpr std::string_view extension = ".bdx";
    if (path.size() < extension.size()) return false;
    const size_t start = path.size() - extension.size();
    for (size_t index = 0; index < extension.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(path[start + index]);
        if (static_cast<char>(std::tolower(character)) != extension[index]) return false;
    }
    return true;
}

bool isCommandBlock(std::string_view name) {
    const size_t bracket = name.find('[');
    if (bracket != std::string_view::npos) name = name.substr(0, bracket);
    const size_t separator = name.rfind(':');
    const std::string_view leaf = separator == std::string_view::npos
        ? name : name.substr(separator + 1);
    return leaf == "command_block" || leaf == "repeating_command_block" ||
        leaf == "chain_command_block";
}

bool isSafeIdentifier(std::string_view value) {
    if (value.empty() || value.size() > 128U) return false;
    const size_t separator = value.find(':');
    if (separator != std::string_view::npos &&
        (separator == 0 || value.find(':', separator + 1U) != std::string_view::npos ||
         value.substr(0, separator) != "minecraft")) {
        return false;
    }
    for (const char character : value) {
        const unsigned char ch = static_cast<unsigned char>(character);
        if (!(std::islower(ch) || std::isdigit(ch) || character == ':' ||
              character == '_' || character == '-')) {
            return false;
        }
    }
    return true;
}

std::string identifierFromState(const std::string& value) {
    const size_t bracket = value.find('[');
    return value.substr(0, bracket == std::string::npos ? value.size() : bracket);
}

std::string stateFromEntry(const BdxPaletteEntry& entry, const std::string& identifier) {
    if (entry.state.empty()) return {};
    const size_t bracket = entry.state.find('[');
    if (bracket != std::string::npos) return entry.state.substr(bracket);
    // Callers sometimes pass a complete state string in both fields. Treat
    // the identifier-only form as an absent state so it uses opcode 7 rather
    // than feeding a block name to the Bedrock property parser.
    if (entry.state == identifier) return {};
    return entry.state;
}

std::string_view stateProperty(std::string_view state, std::string_view key) {
    const size_t opening = state.find('[');
    if (opening == std::string_view::npos) return {};
    size_t cursor = opening + 1U;
    while (cursor < state.size()) {
        while (cursor < state.size() && (state[cursor] == ',' || state[cursor] == ' ')) ++cursor;
        const size_t equals = state.find('=', cursor);
        if (equals == std::string_view::npos) return {};
        const size_t end = state.find_first_of(",]", equals + 1U);
        if (end == std::string_view::npos) return {};
        if (state.substr(cursor, equals - cursor) == key) {
            return state.substr(equals + 1U, end - equals - 1U);
        }
        cursor = end + (state[end] == ',' ? 1U : 0U);
        if (state[end] == ']') break;
    }
    return {};
}

bool parseUnsignedProperty(std::string_view value, uint32_t maximum, uint32_t* output) {
    if (!output || value.empty()) return false;
    uint32_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size() ||
        parsed > maximum) {
        return false;
    }
    *output = parsed;
    return true;
}

uint16_t commandBlockDataFromEntry(const BdxPaletteEntry& entry) {
    uint16_t data = entry.data;
    const std::string_view state(entry.state);
    std::string_view facing = stateProperty(state, "facing");
    if (facing.empty()) facing = stateProperty(state, "facing_direction");
    if (!facing.empty()) {
        uint32_t direction = 0;
        if (parseUnsignedProperty(facing, 5U, &direction)) {
            data = static_cast<uint16_t>((data & ~0x07U) | direction);
        } else {
            static constexpr std::array<std::string_view, 6> kFacing{{
                "down", "up", "north", "south", "west", "east",
            }};
            for (uint16_t index = 0; index < kFacing.size(); ++index) {
                if (facing == kFacing[index]) {
                    data = static_cast<uint16_t>((data & ~0x07U) | index);
                    break;
                }
            }
        }
    }
    std::string_view conditional = stateProperty(state, "conditional");
    if (conditional.empty()) conditional = stateProperty(state, "conditional_bit");
    if (conditional == "true") data = static_cast<uint16_t>(data | 0x08U);
    else if (conditional == "false") data = static_cast<uint16_t>(data & ~0x08U);
    return data;
}

void appendBe16(std::vector<uint8_t>* output, uint16_t value) {
    output->push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    output->push_back(static_cast<uint8_t>(value & 0xffU));
}

void appendBe32(std::vector<uint8_t>* output, uint32_t value) {
    output->push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
    output->push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    output->push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    output->push_back(static_cast<uint8_t>(value & 0xffU));
}

bool appendCString(std::vector<uint8_t>* output, const std::string& value) {
    if (value.find('\0') != std::string::npos) return false;
    output->insert(output->end(), value.begin(), value.end());
    output->push_back(0);
    return true;
}

bool appendSignedDelta(std::vector<uint8_t>* output, uint8_t opcode, int64_t delta) {
    if (delta == 0) return true;
    if (delta < std::numeric_limits<int32_t>::min() ||
        delta > std::numeric_limits<int32_t>::max()) return false;
    output->push_back(opcode);
    appendBe32(output, static_cast<uint32_t>(static_cast<int32_t>(delta)));
    return true;
}

bool appendCommandPayload(std::vector<uint8_t>* output, const CommandBlockRecord& record) {
    if (!isValidCommandBlockMode(record.mode) ||
        record.command.size() > CommandBlockSpoolWriter::kMaximumStringBytes ||
        record.name.size() > CommandBlockSpoolWriter::kMaximumStringBytes ||
        record.last_output.size() > CommandBlockSpoolWriter::kMaximumStringBytes ||
        record.command.find('\0') != std::string::npos ||
        record.name.find('\0') != std::string::npos ||
        record.last_output.find('\0') != std::string::npos) {
        return false;
    }
    appendBe32(output, record.mode);
    if (!appendCString(output, record.command) || !appendCString(output, record.name) ||
        !appendCString(output, record.last_output)) return false;
    appendBe32(output, static_cast<uint32_t>(record.tick_delay));
    output->push_back(record.executing_on_first_tick ? 1U : 0U);
    output->push_back(record.output_tracked ? 1U : 0U);
    output->push_back(record.conditional ? 1U : 0U);
    output->push_back(record.redstone_mode ? 1U : 0U);
    return true;
}

bool syncFile(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "r+b");
    if (!file) return false;
#if defined(_WIN32)
    const bool synced = _commit(_fileno(file)) == 0;
#else
    const bool synced = fsync(fileno(file)) == 0;
#endif
    const bool closed = std::fclose(file) == 0;
    return synced && closed;
}

void syncParentDirectory(const std::string& path) {
#if !defined(_WIN32)
    const size_t separator = path.find_last_of('/');
    const std::string parent = separator == std::string::npos ? "." :
        (separator == 0 ? "/" : path.substr(0, separator));
    const int descriptor = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor >= 0) {
        fsync(descriptor);
        close(descriptor);
    }
#else
    (void)path;
#endif
}

bool publishFile(const std::string& temporary, const std::string& destination) {
#if defined(_WIN32)
    return MoveFileExA(temporary.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    return std::rename(temporary.c_str(), destination.c_str()) == 0;
#endif
}

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

}  // namespace

bool BdxWriter::write(const BdxWriteRequest& request, std::string* error) {
    if (request.output_path.empty() || !hasBdxExtension(request.output_path)) {
        return fail(error, "output path must end with .bdx");
    }
    if (request.width <= 0 || request.height <= 0 || request.length <= 0) {
        return fail(error, "BDX dimensions must be positive");
    }
    if (request.palette.empty() || request.palette.size() > 65'536U ||
        request.block_indices.size() != static_cast<size_t>(
            static_cast<uint64_t>(request.width) * static_cast<uint64_t>(request.height) *
            static_cast<uint64_t>(request.length))) {
        return fail(error, "BDX palette or block array does not match its dimensions");
    }

    std::unordered_map<uint64_t, const CommandBlockRecord*> command_by_index;
    command_by_index.reserve(request.command_blocks.size());
    const uint64_t width = static_cast<uint64_t>(request.width);
    const uint64_t length = static_cast<uint64_t>(request.length);
    const uint64_t layer = width * length;
    for (const CommandBlockRecord& record : request.command_blocks) {
        if (record.x < 0 || record.y < 0 || record.z < 0 || record.x >= request.width ||
            record.y >= request.height || record.z >= request.length) {
            return fail(error, "BDX command-block coordinate is outside the export volume");
        }
        const uint64_t index = static_cast<uint64_t>(record.x) +
            static_cast<uint64_t>(record.z) * width + static_cast<uint64_t>(record.y) * layer;
        // A retried export can observe the same coordinate more than once.
        // Keep the newest payload; the compact block journal remains the
        // authority for whether that coordinate is still a command block.
        command_by_index[index] = &record;
    }

    std::vector<uint8_t> commands;
    try {
        commands.reserve(std::min<size_t>(request.block_indices.size() * 8U, 64U * 1024U));
        // String pool entries are emitted up front. Keep entry 0 aligned with
        // the palette so an index can be used directly by the placement op.
        for (const BdxPaletteEntry& entry : request.palette) {
            const std::string identifier = entry.identifier.empty()
                ? identifierFromState(entry.state) : entry.identifier;
            if (!isSafeIdentifier(identifier)) {
                return fail(error, "BDX palette contains an invalid identifier");
            }
            commands.push_back(1U);
            if (!appendCString(&commands, identifier)) {
                return fail(error, "BDX palette contains an invalid identifier");
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory for BDX command stream");
    }

    std::vector<uint8_t> stream;
    try {
        stream.reserve(commands.size() + request.block_indices.size() * 8U + 96U);
        // A BDX Brotli payload starts with the decoded command-stream header,
        // followed by the author C string. The outer BD@ bytes are added
        // below around the compressed payload.
        stream.push_back('B');
        stream.push_back('D');
        stream.push_back('X');
        stream.push_back(0U);
        if (!appendCString(&stream, request.display_name.empty()
                                      ? std::string("Infinitecz BuildExport")
                                      : request.display_name)) {
            return fail(error, "BDX author metadata contains NUL");
        }
        stream.insert(stream.end(), commands.begin(), commands.end());
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory for BDX command stream");
    }

    int64_t current_x = 0;
    int64_t current_y = 0;
    int64_t current_z = 0;
    bool wrote_block = false;
    for (uint64_t index = 0; index < request.block_indices.size(); ++index) {
        if ((index & 0x3ffU) == 0 && request.cancellation_requested &&
            request.cancellation_requested()) {
            return fail(error, "BDX write cancelled");
        }
        const uint16_t palette_id = request.block_indices[static_cast<size_t>(index)];
        if (palette_id >= request.palette.size()) return fail(error, "BDX block references an invalid palette id");
        if (palette_id == 0) continue;
        wrote_block = true;
        const int64_t x = static_cast<int64_t>(index % width);
        const int64_t z = static_cast<int64_t>((index / width) % length);
        const int64_t y = static_cast<int64_t>(index / layer);
        if (!appendSignedDelta(&stream, 21U, x - current_x) ||
            !appendSignedDelta(&stream, 23U, y - current_y) ||
            !appendSignedDelta(&stream, 25U, z - current_z)) {
            return fail(error, "BDX coordinate delta exceeds the supported range");
        }
        current_x = x;
        current_y = y;
        current_z = z;
        const BdxPaletteEntry& entry = request.palette[palette_id];
        const std::string identifier = entry.identifier.empty()
            ? identifierFromState(entry.state) : entry.identifier;
        const auto command = command_by_index.find(index);
        const bool command_block = isCommandBlock(identifier);
        if (command_block) {
            stream.push_back(27U);
            // Opcode 27 is PlaceBlockWithCommandBlockData and carries the
            // same palette index + legacy data prefix as opcode 7 before the
            // command payload. Omitting the index shifts the mode field and
            // makes the resulting stream unreadable by BdxParser.
            appendBe16(&stream, palette_id);
            uint16_t shell_data = commandBlockDataFromEntry(entry);
            CommandBlockRecord fallback;
            if (command == command_by_index.end()) {
                fallback.x = static_cast<int32_t>(x);
                fallback.y = static_cast<int32_t>(y);
                fallback.z = static_cast<int32_t>(z);
                fallback.conditional = (shell_data & 0x08U) != 0U;
                if (identifier.find("repeating_command_block") != std::string::npos) {
                    fallback.mode = kCommandBlockModeRepeat;
                } else if (identifier.find("chain_command_block") != std::string::npos) {
                    fallback.mode = kCommandBlockModeChain;
                }
                appendBe16(&stream, shell_data);
                if (!appendCommandPayload(&stream, fallback)) {
                    return fail(error, "BDX command-block payload is invalid");
                }
            } else {
                shell_data = command->second->conditional
                    ? static_cast<uint16_t>(shell_data | 0x08U)
                    : static_cast<uint16_t>(shell_data & ~0x08U);
                appendBe16(&stream, shell_data);
                if (!appendCommandPayload(&stream, *command->second)) {
                    return fail(error, "BDX command-block payload is invalid");
                }
            }
        } else {
            const std::string state = stateFromEntry(entry, identifier);
            if (state.empty()) {
                // No inline state means the legacy opcode is the only valid
                // representation. Passing a block identifier as the state
                // string makes the parser reject it as malformed properties.
                stream.push_back(7U);
                appendBe16(&stream, palette_id);
                appendBe16(&stream, entry.data);
            } else {
                if (state.size() > 4096U) {
                    return fail(error, "BDX block state exceeds the supported length limit");
                }
                stream.push_back(13U);
                appendBe16(&stream, palette_id);
                if (!appendCString(&stream, state)) {
                    return fail(error, "BDX block state contains NUL");
                }
            }
        }
    }
    for (auto command = command_by_index.begin(); command != command_by_index.end();) {
        const uint16_t palette_id = request.block_indices[static_cast<size_t>(command->first)];
        if (palette_id == 0 || palette_id >= request.palette.size()) {
            command = command_by_index.erase(command);
            continue;
        }
        const BdxPaletteEntry& entry = request.palette[palette_id];
        const std::string identifier = entry.identifier.empty()
            ? identifierFromState(entry.state) : entry.identifier;
        if (!isCommandBlock(identifier)) {
            command = command_by_index.erase(command);
            continue;
        }
        ++command;
    }
    if (!wrote_block) return fail(error, "BDX export contains no block-placement commands");
    stream.push_back(88U);

    if (request.cancellation_requested && request.cancellation_requested()) {
        return fail(error, "BDX write cancelled");
    }
    size_t compressed_capacity = BrotliEncoderMaxCompressedSize(stream.size());
    if (compressed_capacity == 0) return fail(error, "cannot allocate BDX Brotli buffer");
    std::vector<uint8_t> compressed;
    try {
        compressed.resize(compressed_capacity);
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory for BDX Brotli buffer");
    }
    size_t compressed_size = compressed.size();
    if (!BrotliEncoderCompress(5, 22, BROTLI_MODE_GENERIC, stream.size(), stream.data(),
                               &compressed_size, compressed.data())) {
        return fail(error, "cannot Brotli-compress BDX command stream");
    }

    const std::string part_path = request.output_path + ".part";
    std::remove(part_path.c_str());
    {
        std::ofstream output(part_path, std::ios::binary | std::ios::trunc);
        if (!output) return fail(error, "cannot create BDX temporary file");
        output.write("BD@", 3);
        output.write(reinterpret_cast<const char*>(compressed.data()),
                     static_cast<std::streamsize>(compressed_size));
        output.flush();
        if (!output) {
            output.close();
            std::remove(part_path.c_str());
            return fail(error, "cannot finish BDX temporary file");
        }
    }
    if (!syncFile(part_path)) {
        std::remove(part_path.c_str());
        return fail(error, "cannot sync BDX temporary file");
    }
    std::unique_lock<std::mutex> publish_lock;
    if (request.publish_mutex) publish_lock = std::unique_lock<std::mutex>(*request.publish_mutex);
    if (request.cancellation_requested && request.cancellation_requested()) {
        std::remove(part_path.c_str());
        return fail(error, "BDX write cancelled");
    }
    if (!publishFile(part_path, request.output_path)) {
        const int publish_error = errno;
        std::remove(part_path.c_str());
        return fail(error, "cannot publish BDX: " + std::string(std::strerror(publish_error)));
    }
    syncParentDirectory(request.output_path);
    if (request.publication_committed) {
        request.publication_committed->store(true, std::memory_order_release);
    }
    if (error) error->clear();
    return true;
}

}  // namespace build_import
