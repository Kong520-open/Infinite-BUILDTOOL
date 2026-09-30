#include "BdxParser.h"

#include "RuntimeIdPool117.h"

#include <brotli/decode.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace build_import {
namespace {

constexpr size_t kCompressedBufferBytes = 64U * 1024U;
constexpr size_t kDecodedBufferBytes = 64U * 1024U;
constexpr uint64_t kMaximumDecodedBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr size_t kMaximumPaletteEntries = static_cast<size_t>(UINT16_MAX) + 1U;
constexpr size_t kMaximumPaletteStringBytes = 16U * 1024U;
constexpr size_t kMaximumMetadataStringBytes = 1024U * 1024U;
constexpr size_t kMaximumNbtRecordBytes = 64U * 1024U * 1024U;
constexpr uint32_t kMaximumNbtCollectionEntries = 1024U * 1024U;
constexpr uint32_t kMaximumNbtByteArrayBytes = 64U * 1024U * 1024U;
constexpr uint32_t kMaximumNbtDepth = 64;
constexpr uint64_t kCancellationCheckInterval = 1024;
constexpr uint64_t kProgressInterval = 2048;
constexpr uint8_t kBdxEndOpcode = 88U;

// The original BDX command-block payload has four trailing boolean fields.
// Some legacy exporters omit the final field entirely.  A different exporter
// family writes an opaque, non-boolean placeholder at that location; it must
// be physically consumed to keep the command stream aligned, but its value is
// not safe to trust as an actual redstone setting.  The compatibility form is
// evaluated at each payload because some writers omit its final placeholder
// immediately before End.  The chosen dialect still prevents ordinary streams
// from replaying a movement opcode as a coordinate change.
enum class BdxNeedRedstoneLayout : uint8_t {
    StandardOrUnknown,
    Standard,
    LegacyOmitted,
    LegacyCompatibilityPlaceholder,
};

bool isBdxCommandOpcode(uint8_t value) {
    // These are the command opcodes accepted by the stream switch below.
    // 10 and 11 are deliberately excluded because they are not supported BDX
    // opcodes, not valid evidence that a boolean was omitted.
    return (value >= 1U && value <= 9U) ||
        (value >= 12U && value <= 41U) || value == kBdxEndOpcode;
}

bool isSingleStepCoordinateOpcode(uint8_t value) {
    return value >= 14U && value <= 19U;
}

bool isCompatibilityNeedRedstonePlaceholder(uint8_t first, uint8_t second) {
    // A known legacy writer stores a non-canonical byte copied from the next
    // unit-coordinate opcode.  The wire tail therefore reads e.g. 0E 0E 07:
    // the first 0E is the unusable placeholder, the second is AddXValue0 and
    // 07 places the next block.  Treating both bytes as opcodes doubles every
    // coordinate step and visibly spreads the imported construction apart.
    return first == second && isSingleStepCoordinateOpcode(first);
}

bool defaultRedstoneModeForMissingBdxFlag(uint16_t mode) {
    // An incomplete impulse payload must stay redstone-controlled: turning an
    // unknown impulse block into always-active could execute an arbitrary
    // command during import.  Chain/repeating blocks, by contrast, require
    // their normal always-active state to function as a command sequence.
    return mode != kCommandBlockModeRepeat && mode != kCommandBlockModeChain;
}

bool fail(std::string* error, const std::string& detail) {
    if (error) *error = detail;
    return false;
}

class BrotliByteReader {
public:
    explicit BrotliByteReader(const std::string& path, std::string* error) {
        if (path.empty()) {
            fail(error, "BDX source path is empty");
            return;
        }
        input_.open(path, std::ios::binary);
        if (!input_) {
            fail(error, "cannot open BDX file");
            return;
        }
        input_.seekg(0, std::ios::end);
        const std::streamoff end = input_.tellg();
        if (end < 3) {
            fail(error, "BDX file is truncated before its Brotli header");
            return;
        }
        compressed_size_ = static_cast<uint64_t>(end);
        input_.seekg(0, std::ios::beg);
        std::array<uint8_t, 3> header{};
        input_.read(reinterpret_cast<char*>(header.data()),
                    static_cast<std::streamsize>(header.size()));
        if (!input_ || header[0] != 'B' || header[1] != 'D' || header[2] != '@') {
            fail(error, "not a Brotli-compressed BDX file (expected BD@ header)");
            return;
        }
        compressed_bytes_read_ = header.size();
        decoder_ = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
        if (!decoder_) {
            fail(error, "cannot create Brotli decoder for BDX file");
            return;
        }
        valid_ = true;
    }

    ~BrotliByteReader() {
        if (decoder_) BrotliDecoderDestroyInstance(decoder_);
    }

    BrotliByteReader(const BrotliByteReader&) = delete;
    BrotliByteReader& operator=(const BrotliByteReader&) = delete;

    bool valid() const { return valid_; }
    uint64_t compressedSize() const { return compressed_size_; }
    uint64_t compressedBytesRead() const { return compressed_bytes_read_; }
    uint64_t bytesConsumed() const { return bytes_consumed_; }

    // Keep the current BDX opcode with the stream reader so a malformed legacy
    // payload reports the actual record boundary instead of the unhelpful
    // generic "unexpected end" produced by a later primitive read.  The
    // context is diagnostic-only and never influences parsing.
    void setCommandContext(uint8_t opcode, uint64_t ordinal, uint64_t offset) {
        command_context_ = "opcode " + std::to_string(opcode) +
            " (#" + std::to_string(ordinal) + ", decoded byte " +
            std::to_string(offset) + ")";
    }

    bool readByte(uint8_t* value, std::string* error) {
        if (!value) return fail(error, "BDX byte reader received a null output");
        if (!lookahead_.empty()) {
            *value = lookahead_.front();
            lookahead_.pop_front();
            ++bytes_consumed_;
            return true;
        }
        if (decoded_position_ == decoded_size_ && !refill(error)) {
            setUnexpectedEndError(error);
            return false;
        }
        *value = decoded_[decoded_position_++];
        ++bytes_consumed_;
        return true;
    }

    // Inspect decoded bytes without changing the logical stream position.
    // The small replay deque makes look-ahead safe even across Brotli output
    // buffer boundaries, where peeking directly into decoded_ would fail.
    bool peekByte(uint8_t* value, std::string* error) {
        return peekBytes(value, 1U, error);
    }

    bool peekBytes(uint8_t* destination, size_t count, std::string* error) {
        if (count != 0 && !destination) {
            return fail(error, "BDX byte reader received a null buffer");
        }
        size_t read_count = 0;
        for (; read_count < count; ++read_count) {
            if (!readByte(destination + read_count, error)) {
                // Parsing stops after a failed look-ahead, but restore the
                // logical position as well so the diagnostic stays accurate.
                while (read_count != 0) {
                    --read_count;
                    lookahead_.push_front(destination[read_count]);
                    --bytes_consumed_;
                }
                return false;
            }
        }
        while (count != 0) {
            --count;
            lookahead_.push_front(destination[count]);
            --bytes_consumed_;
        }
        return true;
    }

    bool readBytes(uint8_t* destination, size_t count, std::string* error) {
        if (count != 0 && !destination) return fail(error, "BDX byte reader received a null buffer");
        while (count != 0 && !lookahead_.empty()) {
            *destination++ = lookahead_.front();
            lookahead_.pop_front();
            --count;
            ++bytes_consumed_;
        }
        while (count != 0) {
            if (decoded_position_ == decoded_size_ && !refill(error)) {
                setUnexpectedEndError(error);
                return false;
            }
            const size_t available = decoded_size_ - decoded_position_;
            const size_t take = std::min(available, count);
            std::copy_n(decoded_.data() + decoded_position_, take, destination);
            decoded_position_ += take;
            destination += take;
            count -= take;
            bytes_consumed_ += take;
        }
        return true;
    }

    bool skip(size_t count, std::string* error) {
        while (count != 0 && !lookahead_.empty()) {
            lookahead_.pop_front();
            --count;
            ++bytes_consumed_;
        }
        while (count != 0) {
            if (decoded_position_ == decoded_size_ && !refill(error)) {
                setUnexpectedEndError(error);
                return false;
            }
            const size_t available = decoded_size_ - decoded_position_;
            const size_t take = std::min(available, count);
            decoded_position_ += take;
            count -= take;
            bytes_consumed_ += take;
        }
        return true;
    }

    bool readBE16(uint16_t* value, std::string* error) {
        std::array<uint8_t, 2> bytes{};
        if (!readBytes(bytes.data(), bytes.size(), error)) return false;
        *value = static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8U) | bytes[1]);
        return true;
    }

    bool readBE32(uint32_t* value, std::string* error) {
        std::array<uint8_t, 4> bytes{};
        if (!readBytes(bytes.data(), bytes.size(), error)) return false;
        *value = (static_cast<uint32_t>(bytes[0]) << 24U) |
                 (static_cast<uint32_t>(bytes[1]) << 16U) |
                 (static_cast<uint32_t>(bytes[2]) << 8U) |
                 bytes[3];
        return true;
    }

    bool readLE16(uint16_t* value, std::string* error) {
        std::array<uint8_t, 2> bytes{};
        if (!readBytes(bytes.data(), bytes.size(), error)) return false;
        *value = static_cast<uint16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8U));
        return true;
    }

    bool readLE32(uint32_t* value, std::string* error) {
        std::array<uint8_t, 4> bytes{};
        if (!readBytes(bytes.data(), bytes.size(), error)) return false;
        *value = static_cast<uint32_t>(bytes[0]) |
                 (static_cast<uint32_t>(bytes[1]) << 8U) |
                 (static_cast<uint32_t>(bytes[2]) << 16U) |
                 (static_cast<uint32_t>(bytes[3]) << 24U);
        return true;
    }

    bool readCString(std::string* value, size_t maximum_length, std::string* error) {
        if (value) value->clear();
        for (size_t length = 0; length <= maximum_length; ++length) {
            uint8_t byte = 0;
            if (!readByte(&byte, error)) return false;
            if (byte == 0) return true;
            if (length == maximum_length) {
                return fail(error, "BDX string exceeds the supported length limit");
            }
            if (value) value->push_back(static_cast<char>(byte));
        }
        return fail(error, "BDX string exceeds the supported length limit");
    }

private:
    void setUnexpectedEndError(std::string* error) const {
        if (!error || !error->empty()) return;
        *error = "unexpected end of BDX command stream at decoded byte " +
            std::to_string(bytes_consumed_);
        if (!command_context_.empty()) *error += " while reading " + command_context_;
    }

    bool refillInput(std::string* error) {
        if (input_available_ != 0 || input_eof_) return input_available_ != 0;
        input_.read(reinterpret_cast<char*>(compressed_.data()),
                    static_cast<std::streamsize>(compressed_.size()));
        const std::streamsize read = input_.gcount();
        if (read < 0) return fail(error, "cannot read BDX Brotli stream");
        if (read == 0) {
            if (input_.bad()) return fail(error, "cannot read BDX Brotli stream");
            input_eof_ = true;
            return false;
        }
        input_next_ = compressed_.data();
        input_available_ = static_cast<size_t>(read);
        compressed_bytes_read_ += static_cast<uint64_t>(read);
        return true;
    }

    bool refill(std::string* error) {
        if (decoded_position_ != decoded_size_) return true;
        decoded_position_ = 0;
        decoded_size_ = 0;
        while (true) {
            if (decoder_finished_) return false;
            if (input_available_ == 0 && !input_eof_ && !refillInput(error)) {
                if (error && !error->empty()) return false;
            }
            size_t output_available = decoded_.size();
            uint8_t* output_next = decoded_.data();
            const BrotliDecoderResult result = BrotliDecoderDecompressStream(
                decoder_, &input_available_, &input_next_, &output_available, &output_next, nullptr);
            decoded_size_ = decoded_.size() - output_available;
            if (decoded_size_ != 0) {
                decoded_bytes_produced_ += decoded_size_;
                if (decoded_bytes_produced_ > kMaximumDecodedBytes) {
                    return fail(error, "BDX Brotli stream exceeds the 1 GiB decoded safety limit");
                }
                decoder_finished_ = result == BROTLI_DECODER_RESULT_SUCCESS;
                return true;
            }
            if (result == BROTLI_DECODER_RESULT_SUCCESS) {
                decoder_finished_ = true;
                return false;
            }
            if (result == BROTLI_DECODER_RESULT_ERROR) {
                const char* detail = BrotliDecoderErrorString(BrotliDecoderGetErrorCode(decoder_));
                return fail(error, std::string("cannot decode BDX Brotli stream") +
                                   (detail ? std::string(": ") + detail : std::string()));
            }
            if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT &&
                input_available_ == 0 && input_eof_) {
                return fail(error, "BDX Brotli stream ends before decompression is complete");
            }
            if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
                return fail(error, "BDX Brotli decoder made no output progress");
            }
        }
    }

    std::ifstream input_;
    BrotliDecoderState* decoder_ = nullptr;
    std::array<uint8_t, kCompressedBufferBytes> compressed_{};
    std::array<uint8_t, kDecodedBufferBytes> decoded_{};
    std::deque<uint8_t> lookahead_;
    const uint8_t* input_next_ = nullptr;
    size_t input_available_ = 0;
    size_t decoded_position_ = 0;
    size_t decoded_size_ = 0;
    uint64_t compressed_size_ = 0;
    uint64_t compressed_bytes_read_ = 0;
    uint64_t decoded_bytes_produced_ = 0;
    uint64_t bytes_consumed_ = 0;
    std::string command_context_;
    bool input_eof_ = false;
    bool decoder_finished_ = false;
    bool valid_ = false;
};

bool readSignedBE16(BrotliByteReader* reader, int32_t* value, std::string* error) {
    uint16_t raw = 0;
    if (!reader->readBE16(&raw, error)) return false;
    *value = raw <= 0x7FFFU ? static_cast<int32_t>(raw)
                            : static_cast<int32_t>(raw) - 0x10000;
    return true;
}

bool readSignedBE32(BrotliByteReader* reader, int64_t* value, std::string* error) {
    uint32_t raw = 0;
    if (!reader->readBE32(&raw, error)) return false;
    *value = raw <= 0x7FFFFFFFU ? static_cast<int64_t>(raw)
                                : static_cast<int64_t>(raw) - 0x100000000LL;
    return true;
}

bool addCoordinate(int32_t* coordinate, int64_t offset, const char* axis, std::string* error) {
    const int64_t next = static_cast<int64_t>(*coordinate) + offset;
    if (next < std::numeric_limits<int32_t>::min() ||
        next > std::numeric_limits<int32_t>::max()) {
        return fail(error, std::string("BDX ") + axis + " coordinate overflows the supported range");
    }
    *coordinate = static_cast<int32_t>(next);
    return true;
}

bool addWorldCoordinate(int32_t base, int32_t local, int32_t* output, std::string* error) {
    const int64_t next = static_cast<int64_t>(base) + local;
    if (next < std::numeric_limits<int32_t>::min() ||
        next > std::numeric_limits<int32_t>::max()) {
        return fail(error, "BDX placement coordinate overflows the supported world range");
    }
    *output = static_cast<int32_t>(next);
    return true;
}

void extendBounds(BlockBounds* bounds, int32_t x, int32_t y, int32_t z) {
    if (!bounds->isValid()) {
        bounds->min_x = bounds->max_x = x;
        bounds->min_y = bounds->max_y = y;
        bounds->min_z = bounds->max_z = z;
        return;
    }
    bounds->min_x = std::min(bounds->min_x, x);
    bounds->min_y = std::min(bounds->min_y, y);
    bounds->min_z = std::min(bounds->min_z, z);
    bounds->max_x = std::max(bounds->max_x, x);
    bounds->max_y = std::max(bounds->max_y, y);
    bounds->max_z = std::max(bounds->max_z, z);
}

bool addBoundsBase(const BlockBounds& local, const SchematicParseOptions& options,
                   BlockBounds* world, std::string* error) {
    if (!local.isValid()) return true;
    return addWorldCoordinate(options.base_x, local.min_x, &world->min_x, error) &&
           addWorldCoordinate(options.base_y, local.min_y, &world->min_y, error) &&
           addWorldCoordinate(options.base_z, local.min_z, &world->min_z, error) &&
           addWorldCoordinate(options.base_x, local.max_x, &world->max_x, error) &&
           addWorldCoordinate(options.base_y, local.max_y, &world->max_y, error) &&
           addWorldCoordinate(options.base_z, local.max_z, &world->max_z, error);
}

uint64_t saturatedVolume(const BlockBounds& bounds) {
    if (!bounds.isValid()) return 0;
    const uint64_t width = static_cast<uint64_t>(static_cast<int64_t>(bounds.max_x) - bounds.min_x + 1);
    const uint64_t height = static_cast<uint64_t>(static_cast<int64_t>(bounds.max_y) - bounds.min_y + 1);
    const uint64_t length = static_cast<uint64_t>(static_cast<int64_t>(bounds.max_z) - bounds.min_z + 1);
    if (width != 0 && height > std::numeric_limits<uint64_t>::max() / width) {
        return std::numeric_limits<uint64_t>::max();
    }
    const uint64_t area = width * height;
    return length != 0 && area > std::numeric_limits<uint64_t>::max() / length
        ? std::numeric_limits<uint64_t>::max() : area * length;
}

bool isCommandBlockName(std::string_view name) {
    const size_t separator = name.rfind(':');
    const std::string_view leaf = separator == std::string_view::npos
        ? name : name.substr(separator + 1);
    return leaf == "command_block" || leaf == "repeating_command_block" ||
           leaf == "chain_command_block";
}

// Older BDX writers use several historical spellings in their string pool:
// they may omit the default namespace (`stone`), retain Bedrock's camel-case
// IDs (`seaLantern`), or keep a handful of old all-lowercase aliases
// (`glowingobsidian`).  Normalize only this BDX palette boundary, never a
// command payload.  The result must still pass BlockMapper's target-registry
// gate before it can be used in a game command.
//
// The input language remains deliberately narrow: ASCII letters, digits,
// underscores and hyphens in the leaf; one safe namespace separator at most.
// In particular, spaces, brackets, quotes, slashes, semicolons and control
// characters cannot become part of an emitted identifier.
bool appendNormalizedLegacyBdxLeaf(std::string_view source, std::string* normalized) {
    if (!normalized || source.empty() || source.size() > 128) return false;

    normalized->clear();
    normalized->reserve(source.size() + 8);
    for (size_t index = 0; index < source.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(source[index]);
        const bool lower = character >= 'a' && character <= 'z';
        const bool upper = character >= 'A' && character <= 'Z';
        const bool digit = character >= '0' && character <= '9';
        if (lower || digit || character == '_' || character == '-') {
            normalized->push_back(static_cast<char>(character));
            continue;
        }
        if (!upper) return false;

        // Preserve acronym boundaries too: `redSTONEBrick` becomes
        // `red_stone_brick`, while a regular `seaLantern` becomes
        // `sea_lantern`.  Existing separators are never doubled.
        const bool previous_lower_or_digit = index != 0 &&
            ((source[index - 1] >= 'a' && source[index - 1] <= 'z') ||
             (source[index - 1] >= '0' && source[index - 1] <= '9'));
        const bool previous_upper = index != 0 &&
            source[index - 1] >= 'A' && source[index - 1] <= 'Z';
        const bool next_lower = index + 1 < source.size() &&
            source[index + 1] >= 'a' && source[index + 1] <= 'z';
        if (!normalized->empty() && normalized->back() != '_' &&
            normalized->back() != '-' &&
            (previous_lower_or_digit || (previous_upper && next_lower))) {
            normalized->push_back('_');
        }
        normalized->push_back(static_cast<char>(character - 'A' + 'a'));
    }

    // These names predate the usual camel-case convention, so it is not
    // possible to infer their word boundaries mechanically.  Keep this short
    // and evidence-based rather than treating arbitrary text as a block ID.
    if (*normalized == "glowingobsidian") {
        *normalized = "glowing_obsidian";
    } else if (*normalized == "netherreactor") {
        *normalized = "nether_reactor";
    }
    return true;
}

bool normalizeLegacyBdxNamespace(std::string_view source, std::string* normalized) {
    if (!normalized || source.empty() || source.size() > 64) return false;
    normalized->clear();
    normalized->reserve(source.size());
    for (const unsigned char character : source) {
        if (character >= 'a' && character <= 'z') {
            normalized->push_back(static_cast<char>(character));
        } else if (character >= 'A' && character <= 'Z') {
            normalized->push_back(static_cast<char>(character - 'A' + 'a'));
        } else if ((character >= '0' && character <= '9') || character == '_' ||
                   character == '-' || character == '.') {
            normalized->push_back(static_cast<char>(character));
        } else {
            return false;
        }
    }
    return true;
}

bool normalizeLegacyBdxBlockIdentifier(std::string_view source, std::string* normalized) {
    if (!normalized || source.empty() || source.size() > 128) return false;
    const size_t separator = source.find(':');
    if (separator != std::string_view::npos &&
        source.find(':', separator + 1) != std::string_view::npos) {
        return false;
    }

    std::string leaf;
    if (separator == std::string_view::npos) {
        if (!appendNormalizedLegacyBdxLeaf(source, &leaf)) return false;
        *normalized = "minecraft:" + leaf;
        return true;
    }

    std::string name_space;
    if (separator == 0 || separator + 1 == source.size() ||
        !normalizeLegacyBdxNamespace(source.substr(0, separator), &name_space) ||
        !appendNormalizedLegacyBdxLeaf(source.substr(separator + 1), &leaf)) {
        return false;
    }
    // Preserve a non-Minecraft namespace as a safe, explicit value.
    // BlockMapper will reject it with its normal target-version diagnostic.
    *normalized = std::move(name_space);
    normalized->push_back(':');
    normalized->append(leaf);
    return true;
}

bool skipNbtString(BrotliByteReader* reader, std::string* error) {
    uint16_t length = 0;
    return reader->readLE16(&length, error) && reader->skip(length, error);
}

bool readNbtCount(BrotliByteReader* reader, uint32_t maximum, uint32_t* count,
                  std::string* error) {
    uint32_t raw = 0;
    if (!reader->readLE32(&raw, error)) return false;
    if (raw > 0x7FFFFFFFU || raw > maximum) {
        return fail(error, "BDX block-entity NBT has an oversized collection");
    }
    *count = raw;
    return true;
}

bool skipNbtPayload(BrotliByteReader* reader, uint8_t type, uint32_t depth,
                    std::string* error) {
    if (depth > kMaximumNbtDepth) {
        return fail(error, "BDX block-entity NBT exceeds the nesting limit");
    }
    switch (type) {
        case 1: return reader->skip(1, error);                 // byte
        case 2: return reader->skip(2, error);                 // short
        case 3: return reader->skip(4, error);                 // int
        case 4: return reader->skip(8, error);                 // long
        case 5: return reader->skip(4, error);                 // float
        case 6: return reader->skip(8, error);                 // double
        case 7: {                                               // byte array
            uint32_t count = 0;
            return readNbtCount(reader, kMaximumNbtByteArrayBytes, &count, error) &&
                   reader->skip(count, error);
        }
        case 8:                                                 // string
            return skipNbtString(reader, error);
        case 9: {                                               // list
            uint8_t element_type = 0;
            uint32_t count = 0;
            if (!reader->readByte(&element_type, error) ||
                !readNbtCount(reader, kMaximumNbtCollectionEntries, &count, error)) {
                return false;
            }
            if (element_type == 0 && count != 0) {
                return fail(error, "BDX block-entity NBT has a non-empty end-tag list");
            }
            if (element_type > 12) return fail(error, "BDX block-entity NBT has an unknown list type");
            for (uint32_t index = 0; index < count; ++index) {
                if (!skipNbtPayload(reader, element_type, depth + 1, error)) return false;
            }
            return true;
        }
        case 10: {                                              // compound
            while (true) {
                uint8_t child_type = 0;
                if (!reader->readByte(&child_type, error)) return false;
                if (child_type == 0) return true;
                if (child_type > 12 || !skipNbtString(reader, error) ||
                    !skipNbtPayload(reader, child_type, depth + 1, error)) {
                    if (child_type > 12 && error && error->empty()) {
                        *error = "BDX block-entity NBT has an unknown compound type";
                    }
                    return false;
                }
            }
        }
        case 11: {                                              // int array
            uint32_t count = 0;
            if (!readNbtCount(reader, kMaximumNbtCollectionEntries, &count, error)) return false;
            return reader->skip(static_cast<size_t>(count) * 4U, error);
        }
        case 12: {                                              // long array
            uint32_t count = 0;
            if (!readNbtCount(reader, kMaximumNbtCollectionEntries, &count, error)) return false;
            return reader->skip(static_cast<size_t>(count) * 8U, error);
        }
        default:
            return fail(error, "BDX block-entity NBT has an unknown tag type");
    }
}

bool skipNbtRoot(BrotliByteReader* reader, std::string* error) {
    const uint64_t start = reader->bytesConsumed();
    uint8_t root_type = 0;
    if (!reader->readByte(&root_type, error)) return false;
    if (root_type == 0 || root_type > 12) {
        return fail(error, "BDX block-entity NBT has an invalid root type");
    }
    if (!skipNbtString(reader, error) || !skipNbtPayload(reader, root_type, 0, error)) {
        return false;
    }
    if (reader->bytesConsumed() - start > kMaximumNbtRecordBytes) {
        return fail(error, "BDX block-entity NBT exceeds the 64 MiB safety limit");
    }
    return true;
}

bool readNbtString(BrotliByteReader* reader, std::string* value, std::string* error) {
    uint16_t length = 0;
    if (!reader->readLE16(&length, error)) return false;
    if (value) {
        value->assign(length, '\0');
        if (length != 0 && !reader->readBytes(
                reinterpret_cast<uint8_t*>(&(*value)[0]), length, error)) return false;
        return true;
    }
    return reader->skip(length, error);
}

bool readNbtIntegral(BrotliByteReader* reader, uint8_t type, int64_t* value,
                     std::string* error) {
    if (!value) return false;
    switch (type) {
        case 1: {
            uint8_t raw = 0;
            if (!reader->readByte(&raw, error)) return false;
            *value = static_cast<int8_t>(raw);
            return true;
        }
        case 2: {
            uint16_t raw = 0;
            if (!reader->readLE16(&raw, error)) return false;
            *value = static_cast<int16_t>(raw);
            return true;
        }
        case 3: {
            uint32_t raw = 0;
            if (!reader->readLE32(&raw, error)) return false;
            *value = static_cast<int32_t>(raw);
            return true;
        }
        case 4: {
            uint32_t low = 0;
            uint32_t high = 0;
            if (!reader->readLE32(&low, error) || !reader->readLE32(&high, error)) return false;
            const uint64_t raw = static_cast<uint64_t>(low) |
                                 (static_cast<uint64_t>(high) << 32U);
            *value = static_cast<int64_t>(raw);
            return true;
        }
        default:
            return false;
    }
}

uint16_t commandBlockModeForName(std::string_view name) {
    const size_t separator = name.rfind(':');
    const std::string_view leaf = separator == std::string_view::npos
        ? name : name.substr(separator + 1);
    if (leaf == "repeating_command_block") return 1;
    if (leaf == "chain_command_block") return 2;
    return 0;
}

int32_t signedInt32FromBits(uint32_t value) {
    constexpr uint32_t kSignBit = uint32_t{1} << 31U;
    if (value < kSignBit) return static_cast<int32_t>(value);
    // Do not cast an out-of-range uint32_t to int32_t: that conversion is
    // implementation-defined. Reconstruct the two's-complement value with
    // values that are representable at every intermediate step instead.
    return std::numeric_limits<int32_t>::min() +
        static_cast<int32_t>(value - kSignBit);
}

bool readCommandBlockNbtRoot(BrotliByteReader* reader, CommandBlockRecord* record,
                             std::string* error) {
    if (!reader || !record) return fail(error, "BDX command-block NBT output is unavailable");
    const uint64_t start = reader->bytesConsumed();
    uint8_t root_type = 0;
    if (!reader->readByte(&root_type, error) || root_type != 10 ||
        !skipNbtString(reader, error)) {
        if (error && error->empty()) *error = "BDX command-block NBT root is not a compound";
        return false;
    }
    uint32_t field_count = 0;
    while (true) {
        uint8_t type = 0;
        if (!reader->readByte(&type, error)) return false;
        if (type == 0) break;
        if (type > 12 || ++field_count > kMaximumNbtCollectionEntries) {
            return fail(error, "BDX command-block NBT has too many or invalid fields");
        }
        std::string field;
        if (!readNbtString(reader, &field, error)) return false;
        if (field == "Command" || field == "CustomName" || field == "LastOutput") {
            if (type != 8) {
                if (!skipNbtPayload(reader, type, 1, error)) return false;
                continue;
            }
            std::string* output = field == "Command" ? &record->command
                : field == "CustomName" ? &record->name : &record->last_output;
            if (!readNbtString(reader, output, error)) return false;
            continue;
        }
        if (field == "TrackOutput" || field == "auto" || field == "conditionalMode" ||
            field == "ExecuteOnFirstTick" || field == "TickDelay" ||
            field == "LPCommandMode" || field == "CommandBlockMode") {
            int64_t value = 0;
            if (readNbtIntegral(reader, type, &value, error)) {
                if (field == "TrackOutput") record->output_tracked = value != 0;
                else if (field == "auto") record->redstone_mode = value == 0;
                else if (field == "conditionalMode") record->conditional = value != 0;
                else if (field == "ExecuteOnFirstTick") record->executing_on_first_tick = value != 0;
                else if (field == "TickDelay") {
                    if (value >= std::numeric_limits<int32_t>::min() &&
                        value <= std::numeric_limits<int32_t>::max()) {
                        record->tick_delay = static_cast<int32_t>(value);
                    }
                } else if (value >= 0 && value <= UINT16_MAX) {
                    record->mode = static_cast<uint16_t>(value);
                }
                continue;
            }
            if (type >= 1 && type <= 4) return false;
        }
        if (!skipNbtPayload(reader, type, 1, error)) return false;
    }
    if (reader->bytesConsumed() - start > kMaximumNbtRecordBytes) {
        return fail(error, "BDX command-block NBT exceeds the 64 MiB safety limit");
    }
    return true;
}

bool readCommandBlockData(BrotliByteReader* reader, CommandBlockRecord* record,
                          BdxNeedRedstoneLayout* need_redstone_layout,
                          std::string* error) {
    if (!reader || !need_redstone_layout) {
        return fail(error, "BDX command-block payload parser is unavailable");
    }
    uint32_t raw_mode = 0;
    uint32_t raw_tick_delay = 0;
    std::array<uint8_t, 3> flags{};
    uint8_t redstone_mode = 1U;
    // BDX command payload order is mode, command, custom name, last output,
    // tick delay, then execute-first/track-output/conditional/redstone flags.
    // Preserve the payload as a deferred record; the parser never submits it
    // to the game and callers without a sink can pass nullptr to stream-skip.
    if (!reader->readBE32(&raw_mode, error) || raw_mode > UINT16_MAX ||
        !reader->readCString(record ? &record->command : nullptr,
                             kMaximumMetadataStringBytes, error) ||
        !reader->readCString(record ? &record->name : nullptr,
                             kMaximumMetadataStringBytes, error) ||
         !reader->readCString(record ? &record->last_output : nullptr,
                              kMaximumMetadataStringBytes, error) ||
         !reader->readBE32(&raw_tick_delay, error) ||
         !reader->readBytes(flags.data(), flags.size(), error)) {
        return false;
    }
    const uint16_t mode = static_cast<uint16_t>(raw_mode);
    if (*need_redstone_layout == BdxNeedRedstoneLayout::LegacyOmitted) {
        redstone_mode = defaultRedstoneModeForMissingBdxFlag(mode) ? 1U : 0U;
    } else if (*need_redstone_layout == BdxNeedRedstoneLayout::LegacyCompatibilityPlaceholder) {
        uint8_t next_byte = 0;
        if (!reader->peekByte(&next_byte, error)) return false;
        bool consume_placeholder = !isBdxCommandOpcode(next_byte);
        if (isSingleStepCoordinateOpcode(next_byte)) {
            std::array<uint8_t, 2> probe{};
            if (!reader->peekBytes(probe.data(), probe.size(), error)) return false;
            consume_placeholder = isCompatibilityNeedRedstonePlaceholder(probe[0], probe[1]);
        }
        if (consume_placeholder) {
            uint8_t ignored_placeholder = 0;
            if (!reader->readByte(&ignored_placeholder, error)) return false;
        }
        redstone_mode = defaultRedstoneModeForMissingBdxFlag(mode) ? 1U : 0U;
    } else if (*need_redstone_layout == BdxNeedRedstoneLayout::Standard) {
        if (!reader->readByte(&redstone_mode, error)) return false;
        if (redstone_mode != 0U && redstone_mode != 1U) {
            return fail(error, "BDX command-block needRedstone flag is not a boolean");
        }
    } else {
        uint8_t next_byte = 0;
        if (!reader->peekByte(&next_byte, error)) return false;
        if (next_byte == 0U) {
            if (!reader->readByte(&redstone_mode, error)) return false;
            *need_redstone_layout = BdxNeedRedstoneLayout::Standard;
        } else if (next_byte == 1U) {
            // Opcode 1 (CreateConstantString) collides with the canonical
            // boolean value true.  Its following byte is normally text (or a
            // NUL terminator), neither of which can begin a valid BDX opcode.
            // In that case the fourth field is genuinely omitted and the
            // opcode must remain available to the outer stream loop.
            std::array<uint8_t, 2> probe{};
            if (!reader->peekBytes(probe.data(), probe.size(), error)) return false;
            if (probe[1] == 0U || !isBdxCommandOpcode(probe[1])) {
                *need_redstone_layout = BdxNeedRedstoneLayout::LegacyOmitted;
                redstone_mode = defaultRedstoneModeForMissingBdxFlag(mode) ? 1U : 0U;
            } else {
                if (!reader->readByte(&redstone_mode, error)) return false;
                *need_redstone_layout = BdxNeedRedstoneLayout::Standard;
            }
        } else if (isSingleStepCoordinateOpcode(next_byte)) {
            std::array<uint8_t, 2> probe{};
            if (!reader->peekBytes(probe.data(), probe.size(), error)) return false;
            if (isCompatibilityNeedRedstonePlaceholder(probe[0], probe[1])) {
                uint8_t ignored_placeholder = 0;
                if (!reader->readByte(&ignored_placeholder, error)) return false;
                *need_redstone_layout = BdxNeedRedstoneLayout::LegacyCompatibilityPlaceholder;
                redstone_mode = defaultRedstoneModeForMissingBdxFlag(mode) ? 1U : 0U;
            } else {
                *need_redstone_layout = BdxNeedRedstoneLayout::LegacyOmitted;
                redstone_mode = defaultRedstoneModeForMissingBdxFlag(mode) ? 1U : 0U;
            }
        } else if (isBdxCommandOpcode(next_byte)) {
            // The next byte is already a complete command opcode rather than
            // the missing boolean.  Do not consume it: the outer loop must
            // process it as usual, and all following command payloads share
            // this old exporter layout.
            *need_redstone_layout = BdxNeedRedstoneLayout::LegacyOmitted;
            redstone_mode = defaultRedstoneModeForMissingBdxFlag(mode) ? 1U : 0U;
        } else {
            // Historical writers occasionally leave an opaque nonzero byte in
            // this slot.  It is a physical field, so consume it to preserve
            // coordinates, but use the conservative no-field redstone default
            // rather than treating arbitrary data as an always-active flag.
            uint8_t ignored_placeholder = 0;
            if (!reader->readByte(&ignored_placeholder, error)) return false;
            *need_redstone_layout = BdxNeedRedstoneLayout::LegacyCompatibilityPlaceholder;
            redstone_mode = defaultRedstoneModeForMissingBdxFlag(mode) ? 1U : 0U;
        }
    }
    if (record) {
        record->mode = mode;
        record->tick_delay = signedInt32FromBits(raw_tick_delay);
        record->executing_on_first_tick = flags[0] != 0;
        record->output_tracked = flags[1] != 0;
        record->conditional = flags[2] != 0;
        record->redstone_mode = redstone_mode != 0;
    }
    return true;
}

// Opcode 40 has a compact inventory form rather than generic NBT.  Keep the
// raw values separate from the import-side record so parsing can first prove
// that the corresponding block placement was accepted and is still a target
// container before anything is persisted for the delayed item-write phase.
struct BdxChestItem {
    std::string item_id;
    uint8_t count = 0;
    uint16_t aux = 0;
    uint8_t slot = 0;
};

bool readChestData(BrotliByteReader* reader, std::vector<BdxChestItem>* items,
                   std::string* error) {
    uint8_t slots = 0;
    if (!reader->readByte(&slots, error)) return false;
    if (items) {
        items->clear();
        items->reserve(slots);
    }
    for (uint8_t index = 0; index < slots; ++index) {
        BdxChestItem item;
        if (!reader->readCString(items ? &item.item_id : nullptr, kMaximumPaletteStringBytes,
                                 error) ||
            !reader->readByte(&item.count, error) ||
            !reader->readBE16(&item.aux, error) ||
            !reader->readByte(&item.slot, error)) return false;
        if (items) items->push_back(std::move(item));
    }
    return true;
}

struct LocalPosition {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const LocalPosition& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct LocalPositionHash {
    size_t operator()(const LocalPosition& position) const {
        uint64_t value = (static_cast<uint64_t>(static_cast<uint32_t>(position.x)) << 32U) |
            static_cast<uint32_t>(position.z);
        value ^= static_cast<uint64_t>(static_cast<uint32_t>(position.y)) *
            0x9e3779b97f4a7c15ULL;
        value ^= value >> 30U;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27U;
        return static_cast<size_t>(value);
    }
};

// BDX is a command stream, so a later placement may overwrite a chest from an
// earlier opcode.  Keep only the newest normalized payload for each coordinate
// and send it to the caller after the stream has reached its final state.
struct PendingContainerPayload {
    std::vector<ContainerItemRecord> items;
    bool omitted_payload = false;
};

// Command-block settings can arrive in a separate BDX opcode from their shell.
// Buffer the newest setting for each local coordinate until the command stream
// ends: a subsequent placement can replace that shell with air or another
// block, in which case an already-emitted sidecar record would be stale.
struct PendingCommandBlockPayload {
    CommandBlockRecord record;
    LocalPosition local_position;
    // unordered_map makes coordinate lookup cheap while streaming, but not
    // iteration deterministic.  Preserve the order in which the surviving
    // payloads were last observed so the deferred writer naturally stays near
    // neighbouring BDX records instead of needlessly hopping between cells.
    uint64_t sequence = 0;
};

}  // namespace

bool BdxParser::parse(const SchematicParseOptions& options,
                      SchematicParseResult* result, std::string* error) const {
    BlockMapper mapper;
    return parse(options, mapper, result, error);
}

bool BdxParser::parse(const SchematicParseOptions& options, const BlockMapper& mapper,
                      SchematicParseResult* result, std::string* error) const {
    if (!result) return fail(error, "BDX parse result is missing");
    *result = {};
    if (options.chunk_size <= 0) return fail(error, "BDX chunk size must be positive");

    BrotliByteReader reader(options.source_path, error);
    if (!reader.valid()) return false;
    if (options.progress_callback) {
        options.progress_callback({SchematicParseStage::ReadingSource, 0, reader.compressedSize()});
    }

    std::array<uint8_t, 4> magic{};
    if (!reader.readBytes(magic.data(), magic.size(), error) ||
        magic[0] != 'B' || magic[1] != 'D' || magic[2] != 'X' || magic[3] != 0) {
        return fail(error, "BDX Brotli stream does not contain a BDX command header");
    }
    // Author metadata is intentionally not surfaced or trusted. It has no
    // effect on imported blocks and is never sent to the game.
    if (!reader.readCString(nullptr, kMaximumPaletteStringBytes, error)) return false;

    std::unique_ptr<ChunkSpoolWriter> writer;
    if (!options.block_sink) {
        writer = std::make_unique<ChunkSpoolWriter>(options.spool_directory, options.chunk_size,
                                                    options.maximum_chunk_descriptors);
    }

    std::vector<std::string> strings;
    strings.reserve(256);
    std::unordered_map<std::string, BlockMappingResult> mapping_cache;
    // Runtime IDs are not portable across game versions.  Cache only the
    // already version-pinned pool 117 translations so a large legacy BDX does
    // not repeatedly normalize/map the same runtime state for every voxel.
    std::unordered_map<uint32_t, BlockMappingResult> runtime_mapping_cache;
    runtime_mapping_cache.reserve(1024);
    uint8_t active_runtime_pool = 0;
    bool has_active_runtime_pool = false;
    LocalPosition position;
    // BDX can split shell placement and SetCommandBlockData into separate
    // opcodes. Track only the command-block shells (rather than every block)
    // so a later payload can be tied to a real final block at that coordinate.
    std::unordered_map<LocalPosition, CommandBlockShellState, LocalPositionHash>
        command_block_shells;
    std::unordered_map<LocalPosition, PendingCommandBlockPayload, LocalPositionHash>
        pending_command_block_payloads;
    // Container inventory writes are intentionally delayed until the BDX
    // stream ends.  Unlike ordinary block spools, a /replaceitem sidecar has
    // no way to retract an earlier chest payload if a later opcode overwrites
    // the same coordinate.
    std::unordered_map<LocalPosition, PendingContainerPayload, LocalPositionHash>
        pending_container_payloads;
    uint64_t pending_container_item_count = 0;
    uint64_t pending_command_block_sequence = 0;
    BdxNeedRedstoneLayout need_redstone_layout = BdxNeedRedstoneLayout::StandardOrUnknown;
    BlockBounds local_bounds;
    bool saw_placement = false;
    uint64_t command_count = 0;

    const auto paletteString = [&](uint16_t index, const char* role,
                                   const std::string** value) -> bool {
        if (index >= strings.size()) {
            return fail(error, std::string("BDX ") + role + " string index " +
                               std::to_string(index) + " is outside the string pool");
        }
        *value = &strings[index];
        return true;
    };
    const auto mapBlock = [&](std::string_view name, std::string_view state,
                                 uint16_t legacy_data,
                                bool has_legacy_aux) -> BlockMappingResult {
        std::string normalized_name;
        if (!normalizeLegacyBdxBlockIdentifier(name, &normalized_name)) {
            return {BlockMappingStatus::Unsupported, {},
                    "unsafe BDX block identifier"};
        }
        std::string cache_key;
        cache_key.reserve(normalized_name.size() + state.size() + 6);
        // Cache the normalized form.  This makes `stone` and
        // `minecraft:stone` share one mapping result while retaining the
        // distinction between a legacy zero aux and an absent aux below.
        cache_key.append(normalized_name);
        cache_key.push_back('\0');
        cache_key.append(state.data(), state.size());
        cache_key.push_back('\0');
        // The legacy PlaceBlock data field is big-endian uint16_t in BDX.
        // Include both bytes in the cache key; using only the low byte would
        // alias distinct packed Bedrock states such as 0x0517 and 0x1517.
        cache_key.push_back(static_cast<char>(legacy_data & 0xFFU));
        cache_key.push_back(static_cast<char>((legacy_data >> 8U) & 0xFFU));
        // A zero aux from the old PlaceBlock opcodes still means "preserve the
        // native command identity". It is distinct from a stateful BDX
        // placement whose absent legacy aux merely defaults to zero.
        cache_key.push_back(has_legacy_aux ? '\1' : '\0');
        const auto found = mapping_cache.find(cache_key);
        if (found != mapping_cache.end()) return found->second;
        BlockMappingResult mapped = mapper.mapBedrockState(normalized_name, state, legacy_data,
                                                             has_legacy_aux);
        mapping_cache.emplace(std::move(cache_key), mapped);
        return mapped;
    };
    const auto emitMappedBlock = [&](const BlockMappingResult& mapped,
                                     std::string_view name,
                                     std::string_view state) -> bool {
        saw_placement = true;
        extendBounds(&local_bounds, position.x, position.y, position.z);
        const auto stale = pending_container_payloads.find(position);
        if (stale != pending_container_payloads.end()) {
            pending_container_item_count -= stale->second.items.size();
            pending_container_payloads.erase(stale);
        }
        // Any placement at this coordinate supersedes a preceding command
        // payload, including an air placement. A later command-data opcode can
        // still attach a fresh payload to the shell tracked below.
        pending_command_block_payloads.erase(position);
        if (mapped.status == BlockMappingStatus::Air) {
            command_block_shells.erase(position);
            ++result->skipped_block_count;
            return true;
        }
        if (!mapped.isMapped()) {
            ++result->unsupported_block_count;
            return fail(error, "unsupported BDX block at local (" + std::to_string(position.x) +
                               "," + std::to_string(position.y) + "," +
                               std::to_string(position.z) + "): " + std::string(name) +
                               std::string(state) +
                               (mapped.reason.empty() ? std::string() : " (" + mapped.reason + ")"));
        }
        if (options.maximum_output_blocks != 0 &&
            result->imported_block_count >= options.maximum_output_blocks) {
            return fail(error, "parsed BDX block count exceeds configured limit of " +
                               std::to_string(options.maximum_output_blocks));
        }
        ParsedBlock block;
        if (!addWorldCoordinate(options.base_x, position.x, &block.world_x, error) ||
            !addWorldCoordinate(options.base_y, position.y, &block.world_y, error) ||
            !addWorldCoordinate(options.base_z, position.z, &block.world_z, error)) return false;
        block.spec = mapped.spec;
        if (options.block_sink) {
            if (!options.block_sink(block, error)) return false;
        } else if (!writer || !writer->append(block, error)) {
            if (error && error->empty()) *error = "BDX block spool output is unavailable";
            return false;
        }
        {
            const LocalPosition key = position;
            if (isCommandBlockName(block.spec.command_name)) {
                command_block_shells[key] = commandBlockShellState(
                    commandBlockModeForName(block.spec.command_name), block.spec.aux);
            } else {
                // Keep shell state current even when no sink was requested, so
                // a malformed payload is consistently counted as omitted.
                command_block_shells.erase(key);
            }
        }
        if (!mapped.reason.empty()) {
            ++result->degraded_block_count;
            if (result->first_degradation_reason.empty()) {
                result->first_degradation_reason = std::string(name) + std::string(state) +
                    ": " + mapped.reason;
            }
        }
        ++result->imported_block_count;
        return true;
    };
    const auto emitBlock = [&](std::string_view name, std::string_view state,
                               uint16_t legacy_data, bool has_legacy_aux) -> bool {
        return emitMappedBlock(mapBlock(name, state, legacy_data, has_legacy_aux), name, state);
    };
    const auto emitLegacy = [&](std::string_view name, uint16_t data) -> bool {
        // BDX PlaceBlock defines data as uint16_t.  Some older Bedrock/NetEase
        // exporters use all 16 bits for packed block states, so preserve the
        // exact value rather than rejecting or silently narrowing it.
        return emitBlock(name, {}, data, true);
    };
    const auto resolveRuntimeBlock = [&](uint32_t runtime_id, uint8_t opcode,
                                         std::string_view* name, uint16_t* data) -> bool {
        if (!has_active_runtime_pool) {
            return fail(error, "BDX runtime-id placement before UseRuntimeIDPool at local (" +
                               std::to_string(position.x) + "," +
                               std::to_string(position.y) + "," +
                               std::to_string(position.z) + ")");
        }
        if (active_runtime_pool != 117U) {
            return fail(error, "unsupported BDX runtime-id pool " +
                               std::to_string(active_runtime_pool) + " for opcode " +
                               std::to_string(opcode) + " at local (" +
                               std::to_string(position.x) + "," +
                               std::to_string(position.y) + "," +
                               std::to_string(position.z) + ")");
        }
        if (!lookupRuntimeIdPool117(runtime_id, name, data)) {
            return fail(error, "BDX runtime-id " + std::to_string(runtime_id) +
                               " is outside NetEase pool 117 for opcode " +
                               std::to_string(opcode) + " at local (" +
                               std::to_string(position.x) + "," +
                               std::to_string(position.y) + "," +
                               std::to_string(position.z) + ")");
        }
        return true;
    };
    const auto emitRuntimeBlock = [&](uint32_t runtime_id, uint8_t opcode) -> bool {
        std::string_view name;
        uint16_t data = 0;
        if (!resolveRuntimeBlock(runtime_id, opcode, &name, &data)) return false;
        const auto found = runtime_mapping_cache.find(runtime_id);
        if (found != runtime_mapping_cache.end()) {
            return emitMappedBlock(found->second, name, {});
        }
        // The source table uses old unqualified Bedrock names.  Route its
        // exact aux value through the same normalization/mapping path used by
        // legacy palette records, once per runtime ID.
        const BlockMappingResult mapped = mapBlock(name, {}, data, true);
        const auto inserted = runtime_mapping_cache.emplace(runtime_id, mapped);
        return emitMappedBlock(inserted.first->second, name, {});
    };
    const auto emitContainerBlock = [&](std::string_view name, uint16_t data,
                                        const std::vector<BdxChestItem>& chest_items) -> bool {
        const LocalPosition source_position = position;
        const BlockMappingResult mapped = mapBlock(name, {}, data, true);
        if (!emitLegacy(name, data)) return false;

        const bool can_emit_items = options.container_item_sink && mapped.isMapped() &&
            deferredContainerIdentifier(mapped.spec.command_name);
        PendingContainerPayload payload;
        bool had_nonempty_source_item = false;
        if (can_emit_items) {
            for (const BdxChestItem& source_item : chest_items) {
                // BDX inventories have no enchantment payload at this opcode.
                // Their item data is normalized before it reaches the shared
                // sidecar, so no invalid enchantment can be emitted here.
                if (source_item.count == 0) continue;
                had_nonempty_source_item = true;
                std::string item_id = source_item.item_id;
                if (source_item.count > 64 || !normalizeDeferredItemIdentifier(&item_id)) {
                    continue;
                }
                ContainerItemRecord record;
                if (!addWorldCoordinate(options.base_x, source_position.x, &record.x, error) ||
                    !addWorldCoordinate(options.base_y, source_position.y, &record.y, error) ||
                    !addWorldCoordinate(options.base_z, source_position.z, &record.z, error)) {
                    return false;
                }
                record.slot = source_item.slot;
                record.count = source_item.count;
                record.aux = source_item.aux;
                record.expected_container_id = mapped.spec.command_name;
                record.item_id = std::move(item_id);
                payload.items.push_back(std::move(record));
            }
        }
        // The compact BDX chest format holds only inventory entries. An empty,
        // supported container is faithfully represented by its shell; otherwise
        // record its lost payload once at block-entity granularity.
        payload.omitted_payload =
            !can_emit_items || (had_nonempty_source_item && payload.items.empty());
        if (!payload.items.empty()) {
            if (payload.items.size() >
                ContainerItemSpoolWriter::kMaximumRecords - pending_container_item_count) {
                return fail(error, "BDX deferred container-item count exceeds the safety limit");
            }
            pending_container_item_count += payload.items.size();
        }
        if (!payload.items.empty() || payload.omitted_payload) {
            pending_container_payloads.emplace(source_position, std::move(payload));
        }
        return true;
    };
    const auto emitCommandBlockPayload = [&](CommandBlockRecord* payload) -> bool {
        // Parsing without a deferred sink is still a supported inspection/test
        // mode.  readCommandBlockData has already consumed the wire payload;
        // record that it was deliberately omitted instead of treating the
        // null output object as malformed input.
        if (!options.command_block_sink) {
            ++result->omitted_command_block_data_count;
            return true;
        }
        if (!payload) return fail(error, "BDX command-block payload is unavailable");
        const auto shell = command_block_shells.find(position);
        if (shell == command_block_shells.end()) {
            // A malformed/stale SetCommandBlockData opcode must not turn into
            // a native update packet for whichever block happens to be there.
            ++result->omitted_command_block_data_count;
            return true;
        }
        // Packet data has to agree with the shell.  The source block entity is
        // allowed to be stale or omit conditionalMode, whereas the final
        // routed shell is exactly what the normal import pipeline placed.
        // Applying both fields here prevents the update packet from resetting
        // a conditional command block to its default unconditional state.
        applyCommandBlockShellState(payload, shell->second);
        if (!addWorldCoordinate(options.base_x, position.x, &payload->x, error) ||
            !addWorldCoordinate(options.base_y, position.y, &payload->y, error) ||
            !addWorldCoordinate(options.base_z, position.z, &payload->z, error)) {
            return false;
        }
        // Last payload wins. It is delivered only after the stream proves that
        // this coordinate still ends as the expected command-block shell.
        if (pending_command_block_sequence == std::numeric_limits<uint64_t>::max()) {
            return fail(error, "BDX command-block payload sequence overflows");
        }
        pending_command_block_payloads[position] = {
            std::move(*payload), position, pending_command_block_sequence++};
        return true;
    };
    const auto moveWithReset = [&](int axis, int64_t offset) -> bool {
        int32_t* coordinate = axis == 0 ? &position.x : axis == 1 ? &position.y : &position.z;
        const char* name = axis == 0 ? "X" : axis == 1 ? "Y" : "Z";
        if (!addCoordinate(coordinate, offset, name, error)) return false;
        if (axis != 0) position.x = 0;
        if (axis != 1) position.y = 0;
        if (axis != 2) position.z = 0;
        return true;
    };

    bool ended = false;
    while (!ended) {
        if ((command_count % kCancellationCheckInterval) == 0 &&
            options.cancellation_requested && options.cancellation_requested()) {
            return fail(error, "import cancelled");
        }
        uint8_t opcode = 0;
        if (!reader.readByte(&opcode, error)) return false;
        ++command_count;
        reader.setCommandContext(opcode, command_count, reader.bytesConsumed() - 1U);
        if ((command_count % kProgressInterval) == 0 && options.progress_callback) {
            options.progress_callback({SchematicParseStage::ReadingSource,
                                       std::min(reader.compressedBytesRead(), reader.compressedSize()),
                                       reader.compressedSize()});
        }
        switch (opcode) {
            case 1: {  // CreateConstantString
                if (strings.size() >= kMaximumPaletteEntries) {
                    return fail(error, "BDX string pool exceeds the 16-bit index limit");
                }
                std::string value;
                if (!reader.readCString(&value, kMaximumPaletteStringBytes, error)) return false;
                strings.push_back(std::move(value));
                break;
            }
            case 2: {  // Legacy AddInt16XValue0
                uint16_t offset = 0;
                if (!reader.readBE16(&offset, error) || !moveWithReset(0, offset)) return false;
                break;
            }
            case 3:    // Legacy AddXValue0
                if (!moveWithReset(0, 1)) return false;
                break;
            case 4: {  // Legacy AddInt16YValue0
                uint16_t offset = 0;
                if (!reader.readBE16(&offset, error) || !moveWithReset(1, offset)) return false;
                break;
            }
            case 5: {  // PlaceBlockWithBlockStates
                uint16_t block_index = 0;
                uint16_t state_index = 0;
                const std::string* name = nullptr;
                const std::string* state = nullptr;
                if (!reader.readBE16(&block_index, error) || !reader.readBE16(&state_index, error) ||
                    !paletteString(block_index, "block", &name) ||
                    !paletteString(state_index, "state", &state) ||
                    !emitBlock(*name, *state, 0, false)) return false;
                break;
            }
            case 6: {  // Legacy AddInt16ZValue0
                uint16_t offset = 0;
                if (!reader.readBE16(&offset, error) || !moveWithReset(2, offset)) return false;
                break;
            }
            case 7: {  // PlaceBlock
                uint16_t block_index = 0;
                uint16_t data = 0;
                const std::string* name = nullptr;
                if (!reader.readBE16(&block_index, error) || !reader.readBE16(&data, error) ||
                    !paletteString(block_index, "block", &name) || !emitLegacy(*name, data)) return false;
                break;
            }
            case 8:    // Legacy AddZValue0
                if (!moveWithReset(2, 1)) return false;
                break;
            case 9:    // NOP
                break;
            case 12: { // Legacy AddInt32ZValue0
                uint32_t offset = 0;
                if (!reader.readBE32(&offset, error) || !moveWithReset(2, offset)) return false;
                break;
            }
            case 13: { // PlaceBlockWithInlineBlockStates
                uint16_t block_index = 0;
                const std::string* name = nullptr;
                std::string state;
                if (!reader.readBE16(&block_index, error) || !paletteString(block_index, "block", &name) ||
                    !reader.readCString(&state, kMaximumPaletteStringBytes, error) ||
                    !emitBlock(*name, state, 0, false)) return false;
                break;
            }
            case 14: if (!addCoordinate(&position.x, 1, "X", error)) return false; break;
            case 15: if (!addCoordinate(&position.x, -1, "X", error)) return false; break;
            case 16: if (!addCoordinate(&position.y, 1, "Y", error)) return false; break;
            case 17: if (!addCoordinate(&position.y, -1, "Y", error)) return false; break;
            case 18: if (!addCoordinate(&position.z, 1, "Z", error)) return false; break;
            case 19: if (!addCoordinate(&position.z, -1, "Z", error)) return false; break;
            case 20: {
                int32_t offset = 0;
                if (!readSignedBE16(&reader, &offset, error) ||
                    !addCoordinate(&position.x, offset, "X", error)) return false;
                break;
            }
            case 21: {
                int64_t offset = 0;
                if (!readSignedBE32(&reader, &offset, error) ||
                    !addCoordinate(&position.x, offset, "X", error)) return false;
                break;
            }
            case 22: {
                int32_t offset = 0;
                if (!readSignedBE16(&reader, &offset, error) ||
                    !addCoordinate(&position.y, offset, "Y", error)) return false;
                break;
            }
            case 23: {
                int64_t offset = 0;
                if (!readSignedBE32(&reader, &offset, error) ||
                    !addCoordinate(&position.y, offset, "Y", error)) return false;
                break;
            }
            case 24: {
                int32_t offset = 0;
                if (!readSignedBE16(&reader, &offset, error) ||
                    !addCoordinate(&position.z, offset, "Z", error)) return false;
                break;
            }
            case 25: {
                int64_t offset = 0;
                if (!readSignedBE32(&reader, &offset, error) ||
                    !addCoordinate(&position.z, offset, "Z", error)) return false;
                break;
            }
            case 26: { // SetCommandBlockData for the current position
                CommandBlockRecord payload;
                if (!readCommandBlockData(&reader,
                                           options.command_block_sink ? &payload : nullptr,
                                           &need_redstone_layout, error) ||
                    !emitCommandBlockPayload(options.command_block_sink ? &payload : nullptr)) {
                    return false;
                }
                break;
            }
            case 27: { // PlaceBlockWithCommandBlockData
                uint16_t block_index = 0;
                uint16_t data = 0;
                const std::string* name = nullptr;
                CommandBlockRecord payload;
                if (!reader.readBE16(&block_index, error) || !reader.readBE16(&data, error) ||
                    !paletteString(block_index, "block", &name) ||
                    !readCommandBlockData(&reader,
                                          options.command_block_sink ? &payload : nullptr,
                                          &need_redstone_layout, error) ||
                    !emitLegacy(*name, data) ||
                    !emitCommandBlockPayload(options.command_block_sink ? &payload : nullptr)) return false;
                break;
            }
            case 28:
            case 29:
            case 30: {
                uint8_t raw = 0;
                if (!reader.readByte(&raw, error)) return false;
                const int32_t offset = raw <= 0x7FU ? static_cast<int32_t>(raw)
                                                    : static_cast<int32_t>(raw) - 0x100;
                int32_t* target = opcode == 28 ? &position.x : opcode == 29 ? &position.y : &position.z;
                const char* axis = opcode == 28 ? "X" : opcode == 29 ? "Y" : "Z";
                if (!addCoordinate(target, offset, axis, error)) return false;
                break;
            }
            case 31: { // UseRuntimeIDPool; runtime IDs are exporter-version-specific.
                if (!reader.readByte(&active_runtime_pool, error)) return false;
                has_active_runtime_pool = true;
                break;
            }
            case 36: { // PlaceCommandBlockWithData: implied command-block shell plus payload.
                uint16_t data = 0;
                CommandBlockRecord payload;
                if (!reader.readBE16(&data, error) ||
                    !readCommandBlockData(&reader,
                                          options.command_block_sink ? &payload : nullptr,
                                          &need_redstone_layout, error) ||
                    !emitLegacy("minecraft:command_block", data) ||
                    !emitCommandBlockPayload(options.command_block_sink ? &payload : nullptr)) return false;
                break;
            }
            case 32: { // PlaceRuntimeBlock (uint16 runtime ID)
                uint16_t runtime_id = 0;
                if (!reader.readBE16(&runtime_id, error) || !emitRuntimeBlock(runtime_id, opcode)) {
                    return false;
                }
                break;
            }
            case 33: { // PlaceBlockWithRuntimeId (uint32 runtime ID)
                uint32_t runtime_id = 0;
                if (!reader.readBE32(&runtime_id, error) || !emitRuntimeBlock(runtime_id, opcode)) {
                    return false;
                }
                break;
            }
            case 34: { // Runtime command-block shell plus deferred settings (uint16 ID).
                uint16_t runtime_id = 0;
                CommandBlockRecord payload;
                if (!reader.readBE16(&runtime_id, error) || !emitRuntimeBlock(runtime_id, opcode) ||
                    !readCommandBlockData(&reader,
                                          options.command_block_sink ? &payload : nullptr,
                                          &need_redstone_layout, error) ||
                    !emitCommandBlockPayload(options.command_block_sink ? &payload : nullptr)) {
                    return false;
                }
                break;
            }
            case 35: { // Runtime command-block shell plus deferred settings (uint32 ID).
                uint32_t runtime_id = 0;
                CommandBlockRecord payload;
                if (!reader.readBE32(&runtime_id, error) || !emitRuntimeBlock(runtime_id, opcode) ||
                    !readCommandBlockData(&reader,
                                          options.command_block_sink ? &payload : nullptr,
                                          &need_redstone_layout, error) ||
                    !emitCommandBlockPayload(options.command_block_sink ? &payload : nullptr)) {
                    return false;
                }
                break;
            }
            case 37: { // Runtime container shell plus compact inventory entries (uint16 ID).
                uint16_t runtime_id = 0;
                std::string_view name;
                uint16_t data = 0;
                std::vector<BdxChestItem> chest_items;
                if (!reader.readBE16(&runtime_id, error) ||
                    !resolveRuntimeBlock(runtime_id, opcode, &name, &data) ||
                    !readChestData(&reader,
                                   options.container_item_sink ? &chest_items : nullptr,
                                   error) ||
                    !emitContainerBlock(name, data, chest_items)) {
                    return false;
                }
                break;
            }
            case 38: { // Runtime container shell plus compact inventory entries (uint32 ID).
                uint32_t runtime_id = 0;
                std::string_view name;
                uint16_t data = 0;
                std::vector<BdxChestItem> chest_items;
                if (!reader.readBE32(&runtime_id, error) ||
                    !resolveRuntimeBlock(runtime_id, opcode, &name, &data) ||
                    !readChestData(&reader,
                                   options.container_item_sink ? &chest_items : nullptr,
                                   error) ||
                    !emitContainerBlock(name, data, chest_items)) {
                    return false;
                }
                break;
            }
            case 39: { // AssignDebugData
                uint32_t length = 0;
                if (!reader.readBE32(&length, error) || length > kMaximumMetadataStringBytes ||
                    !reader.skip(length, error)) {
                    if (length > kMaximumMetadataStringBytes && error && error->empty()) {
                        *error = "BDX debug payload exceeds the supported length limit";
                    }
                    return false;
                }
                break;
            }
            case 40: { // PlaceBlockWithChestData
                uint16_t block_index = 0;
                uint16_t data = 0;
                const std::string* name = nullptr;
                std::vector<BdxChestItem> chest_items;
                if (!reader.readBE16(&block_index, error) || !reader.readBE16(&data, error) ||
                    !paletteString(block_index, "block", &name) ||
                    !readChestData(&reader,
                                   options.container_item_sink ? &chest_items : nullptr,
                                   error)) return false;

                if (!emitContainerBlock(*name, data, chest_items)) return false;
                break;
            }
            case 41: { // PlaceBlockWithNBTData
                uint16_t block_index = 0;
                uint16_t state_index = 0;
                uint16_t ignored_duplicate_state_index = 0;
                const std::string* name = nullptr;
                const std::string* state = nullptr;
                CommandBlockRecord payload;
                if (!reader.readBE16(&block_index, error) || !reader.readBE16(&state_index, error) ||
                    !reader.readBE16(&ignored_duplicate_state_index, error) ||
                    !paletteString(block_index, "block", &name) ||
                    !paletteString(state_index, "state", &state)) return false;
                // Opcode 41 carries an NBT root immediately after the palette
                // names.  Use the same legacy-name normalization as the later
                // block mapper, otherwise an old `commandBlock` spelling would
                // be treated as generic NBT and its command payload lost.
                std::string normalized_command_name;
                const bool command_block =
                    normalizeLegacyBdxBlockIdentifier(*name, &normalized_command_name) &&
                    isCommandBlockName(normalized_command_name);
                if (command_block && options.command_block_sink) {
                    payload.mode = commandBlockModeForName(normalized_command_name);
                    if (!readCommandBlockNbtRoot(&reader, &payload, error)) return false;
                } else if (!skipNbtRoot(&reader, error)) {
                    return false;
                }
                if (!emitBlock(*name, *state, 0, false)) return false;
                // Generic block-entity NBT remains unsupported. Command-block
                // data has its own retained/omitted counter, so it must not
                // also produce a misleading generic block-entity warning.
                if (!command_block) {
                    ++result->omitted_block_entity_count;
                }
                if (command_block &&
                    !emitCommandBlockPayload(options.command_block_sink ? &payload : nullptr)) return false;
                break;
            }
            case kBdxEndOpcode: // A signed BDX may have data after this marker.
                ended = true;
                break;
            default:
                return fail(error, "BDX command stream contains an unsupported opcode " +
                                   std::to_string(opcode));
        }
    }

    if (!saw_placement) return fail(error, "BDX file contains no block-placement commands");
    std::vector<const PendingCommandBlockPayload*> ordered_command_block_payloads;
    ordered_command_block_payloads.reserve(pending_command_block_payloads.size());
    for (const auto& entry : pending_command_block_payloads) {
        ordered_command_block_payloads.push_back(&entry.second);
    }
    std::sort(ordered_command_block_payloads.begin(), ordered_command_block_payloads.end(),
              [](const PendingCommandBlockPayload* left, const PendingCommandBlockPayload* right) {
                  return left->sequence < right->sequence;
              });
    for (const PendingCommandBlockPayload* pending : ordered_command_block_payloads) {
        if (options.cancellation_requested && options.cancellation_requested()) {
            return fail(error, "import cancelled");
        }
        const auto shell = command_block_shells.find(pending->local_position);
        if (shell == command_block_shells.end()) {
            // This should only be reachable after a malformed BDX sequence; a
            // normal later placement erases the pending record above. Keep it
            // fail-safe rather than targeting a non-command block.
            ++result->omitted_command_block_data_count;
            continue;
        }
        if (options.command_block_sink) {
            CommandBlockRecord record = pending->record;
            applyCommandBlockShellState(&record, shell->second);
            if (!options.command_block_sink(record, error)) {
                if (error && error->empty()) {
                    *error = "BDX command-block sink rejected a payload";
                }
                return false;
            }
            ++result->command_block_payload_count;
        } else {
            ++result->omitted_command_block_data_count;
        }
    }
    for (const auto& entry : pending_container_payloads) {
        if (entry.second.omitted_payload) ++result->omitted_block_entity_count;
        if (options.container_item_sink) {
            if (options.cancellation_requested && options.cancellation_requested()) {
                return fail(error, "import cancelled");
            }
            for (const ContainerItemRecord& record : entry.second.items) {
                if (!options.container_item_sink(record, error)) {
                    if (error && error->empty()) {
                        *error = "BDX container-item sink rejected a payload";
                    }
                    return false;
                }
                ++result->container_item_payload_count;
            }
        }
    }
    if (!addBoundsBase(local_bounds, options, &result->source_volume_bounds, error)) return false;
    result->source_voxel_count = saturatedVolume(result->source_volume_bounds);
    result->source_region_count = 1;
    if (options.progress_callback) {
        options.progress_callback({SchematicParseStage::RoutingBlocks,
                                   result->imported_block_count + result->skipped_block_count, 0});
    }
    if (writer && options.include_source_volume &&
        !writer->includeVolume(result->source_volume_bounds, error, options.cancellation_requested)) {
        return false;
    }
    if (writer) {
        if (options.progress_callback) {
            options.progress_callback({SchematicParseStage::FinalizingSpools, 0, 1});
        }
        result->chunks = writer->finish(error, options.cancellation_requested,
            [&](uint64_t completed, uint64_t total) {
                if (options.progress_callback) {
                    options.progress_callback({SchematicParseStage::FinalizingSpools, completed, total});
                }
            });
        if (result->chunks.empty() && result->imported_block_count != 0) {
            if (error && error->empty()) *error = "cannot finalize BDX chunk spools";
            return false;
        }
    }
    return true;
}

}  // namespace build_import
