#include "InfiniteczBuildWriter.h"
#include "InfinityCompressionCodec.h"
#include "InfinityFormatKey.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace build_import {
namespace {

constexpr size_t kHeaderSize = 96;
constexpr size_t kTrailerSize = 24;
constexpr uint32_t kFlagRawBlocks = 1U << 0U;
constexpr uint32_t kFlagCommands = 1U << 1U;
constexpr uint32_t kFlagStateJson = 1U << 2U;
constexpr uint32_t kFlagEntityJson = 1U << 3U;
constexpr uint64_t kMaximumBlockCount = 16ULL * 1024ULL * 1024ULL;
constexpr uint32_t kMaximumPaletteSize = 65'536U;
constexpr uint32_t kMaximumStringBytes = 4U * 1024U * 1024U;
constexpr uint64_t kMaximumRawPayloadBytes = 512ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumStagingFileBytes = 1ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr char kMagic[8] = {'I', 'C', 'B', 'U', 'I', 'L', 'D', 1};
constexpr char kTrailerMagic[8] = {'I', 'C', 'B', 'E', 'N', 'D', 1, 0};

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

void appendLe32(std::array<uint8_t, kHeaderSize>* bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        (*bytes)[offset + i] = static_cast<uint8_t>((value >> (i * 8U)) & 0xffU);
    }
}

void appendLe64(std::array<uint8_t, kHeaderSize>* bytes, size_t offset, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        (*bytes)[offset + i] = static_cast<uint8_t>((value >> (i * 8U)) & 0xffU);
    }
}

bool validString(std::string_view value, size_t maximum, bool allow_empty = true) {
    return (allow_empty || !value.empty()) && value.size() <= maximum &&
        value.find('\0') == std::string_view::npos;
}

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t size) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t i = 0; i < result.size(); ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) ? (value >> 1U) ^ 0xedb88320U : value >> 1U;
            }
            result[i] = value;
        }
        return result;
    }();
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xffU] ^ (crc >> 8U);
    }
    return crc;
}

class PayloadWriter {
public:
    explicit PayloadWriter(std::ofstream* output) : output_(output) {}

    bool bytes(const void* data, size_t size) {
        if (!output_ || (!data && size != 0)) return false;
        if (size != 0) {
            output_->write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            if (!*output_) return false;
            crc_ = crc32Update(crc_, static_cast<const uint8_t*>(data), size);
            if (payload_size_ > std::numeric_limits<uint64_t>::max() - size) return false;
            payload_size_ += size;
        }
        return true;
    }

    bool u8(uint8_t value) { return bytes(&value, sizeof(value)); }

    bool u16(uint16_t value) {
        uint8_t data[2] = {static_cast<uint8_t>(value & 0xffU),
                           static_cast<uint8_t>((value >> 8U) & 0xffU)};
        return bytes(data, sizeof(data));
    }

    bool u32(uint32_t value) {
        uint8_t data[4]{};
        for (size_t i = 0; i < 4; ++i) data[i] = static_cast<uint8_t>((value >> (i * 8U)) & 0xffU);
        return bytes(data, sizeof(data));
    }

    bool i32(int32_t value) { return u32(static_cast<uint32_t>(value)); }

    bool u64(uint64_t value) {
        uint8_t data[8]{};
        for (size_t i = 0; i < 8; ++i) data[i] = static_cast<uint8_t>((value >> (i * 8U)) & 0xffU);
        return bytes(data, sizeof(data));
    }

    bool string(std::string_view value) {
        if (value.size() > std::numeric_limits<uint32_t>::max() ||
            !u32(static_cast<uint32_t>(value.size()))) return false;
        return bytes(value.data(), value.size());
    }

    uint64_t payloadSize() const { return payload_size_; }
    uint32_t checksum() const { return crc_ ^ 0xffffffffU; }

private:
    std::ofstream* output_ = nullptr;
    uint32_t crc_ = 0xffffffffU;
    uint64_t payload_size_ = 0;
};

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

class PlaintextFileGuard {
public:
    explicit PlaintextFileGuard(std::string path) : path_(std::move(path)) {}
    ~PlaintextFileGuard() { std::remove(path_.c_str()); }

    PlaintextFileGuard(const PlaintextFileGuard&) = delete;
    PlaintextFileGuard& operator=(const PlaintextFileGuard&) = delete;

private:
    std::string path_;
};

bool writeTrailer(std::ofstream* output, uint64_t payload_size, uint32_t checksum) {
    if (!output) return false;
    output->write(kTrailerMagic, sizeof(kTrailerMagic));
    uint8_t data[12]{};
    for (size_t i = 0; i < 8; ++i) data[i] = static_cast<uint8_t>((payload_size >> (i * 8U)) & 0xffU);
    for (size_t i = 0; i < 4; ++i) data[8U + i] = static_cast<uint8_t>((checksum >> (i * 8U)) & 0xffU);
    output->write(reinterpret_cast<const char*>(data), sizeof(data));
    const uint32_t reserved = 0;
    output->write(reinterpret_cast<const char*>(&reserved), sizeof(reserved));
    return static_cast<bool>(*output);
}

bool writeCommand(PayloadWriter* writer, const CommandBlockRecord& record) {
    if (!writer || !isValidCommandBlockMode(record.mode) || record.tick_delay < 0 ||
        !validString(record.command, CommandBlockSpoolWriter::kMaximumStringBytes) ||
        !validString(record.last_output, CommandBlockSpoolWriter::kMaximumStringBytes) ||
        !validString(record.name, CommandBlockSpoolWriter::kMaximumStringBytes) ||
        !validString(record.filtered_name, CommandBlockSpoolWriter::kMaximumStringBytes)) {
        return false;
    }
    uint8_t flags = 0;
    if (record.redstone_mode) flags |= 1U;
    if (record.conditional) flags |= 2U;
    if (record.output_tracked) flags |= 4U;
    if (record.executing_on_first_tick) flags |= 8U;
    return writer->i32(record.x) && writer->i32(record.y) && writer->i32(record.z) &&
        writer->u16(record.mode) && writer->u8(flags) && writer->u8(0) &&
        writer->i32(record.tick_delay) && writer->string(record.command) &&
        writer->string(record.last_output) && writer->string(record.name) &&
        writer->string(record.filtered_name);
}

}  // namespace

bool InfiniteczBuildWriter::write(const InfiniteczBuildWriteRequest& request,
                                  std::string* error) {
    if (error) error->clear();
    const std::string_view output_path(request.output_path);
    const std::string_view extension(kExtension);
    const bool has_extension = output_path.size() >= extension.size() &&
        output_path.compare(output_path.size() - extension.size(),
                            extension.size(), extension) == 0;
    if (request.output_path.empty() || !has_extension) {
        return fail(error, "Infinitecz output path must end with lowercase .infinity");
    }
    const std::string part_path = request.output_path + ".plaintext.part";
    const std::string compressed_path = request.output_path + ".compressed.part";
    // Clear plaintext left by a process interruption before doing any request
    // validation that could return early.
    std::remove(part_path.c_str());
    std::remove(compressed_path.c_str());
    if (request.width <= 0 || request.height <= 0 || request.length <= 0) {
        return fail(error, "Infinitecz dimensions must be positive");
    }
    const uint64_t width = static_cast<uint64_t>(request.width);
    const uint64_t height = static_cast<uint64_t>(request.height);
    const uint64_t length = static_cast<uint64_t>(request.length);
    if (height > std::numeric_limits<uint64_t>::max() / width ||
        width * height > std::numeric_limits<uint64_t>::max() / length) {
        return fail(error, "Infinitecz dimensions overflow");
    }
    const uint64_t volume = width * height * length;
    if (volume == 0 || volume > kMaximumBlockCount || request.block_indices.size() != volume) {
        return fail(error, "Infinitecz block array does not match its dimensions");
    }
    if (request.palette.empty() || request.palette.size() > kMaximumPaletteSize ||
        request.palette.front() != "minecraft:air") {
        return fail(error, "Infinitecz palette is invalid");
    }
    if (!validString(request.display_name, 4096U)) {
        return fail(error, "Infinitecz display name is invalid");
    }
    for (const std::string& state : request.palette) {
        if (!validString(state, kMaximumStringBytes, false)) {
            return fail(error, "Infinitecz palette contains an invalid state");
        }
    }
    for (const uint16_t id : request.block_indices) {
        if (id >= request.palette.size()) return fail(error, "Infinitecz block references invalid palette");
    }
    const int64_t w = request.width;
    const int64_t h = request.height;
    const int64_t l = request.length;
    std::unordered_set<uint64_t> raw_coordinates;
    std::unordered_set<uint64_t> command_coordinates;
    try {
        raw_coordinates.reserve(request.raw_blocks.size());
        command_coordinates.reserve(request.command_blocks.size());
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory to validate Infinitecz records");
    }
    uint64_t raw_bytes = 0;
    uint32_t flags = 0;
    for (const SchematicRawBlock& record : request.raw_blocks) {
        if (record.x < 0 || record.y < 0 || record.z < 0 || record.x >= w ||
            record.y >= h || record.z >= l || !validString(record.identifier, 1024U, false) ||
            record.state_json.size() > 4U * 1024U * 1024U ||
            record.entity_json.size() > 64U * 1024U * 1024U ||
            record.state_json.find('\0') != std::string::npos ||
            record.entity_json.find('\0') != std::string::npos) {
            return fail(error, "invalid Infinitecz raw-state record");
        }
        const uint64_t coordinate = static_cast<uint64_t>(record.x) +
            static_cast<uint64_t>(record.z) * width +
            static_cast<uint64_t>(record.y) * width * length;
        if (!raw_coordinates.emplace(coordinate).second) {
            return fail(error, "Infinitecz raw-state records contain duplicate coordinates");
        }
        const uint64_t bytes = record.identifier.size() + record.state_json.size() +
            record.entity_json.size();
        if (bytes > kMaximumRawPayloadBytes || raw_bytes > kMaximumRawPayloadBytes - bytes) {
            return fail(error, "Infinitecz raw-state payload is too large");
        }
        raw_bytes += bytes;
        flags |= kFlagRawBlocks;
        if (!record.state_json.empty()) flags |= kFlagStateJson;
        if (!record.entity_json.empty()) flags |= kFlagEntityJson;
    }
    for (const CommandBlockRecord& record : request.command_blocks) {
        if (record.x < 0 || record.y < 0 || record.z < 0 || record.x >= w ||
            record.y >= h || record.z >= l || !isValidCommandBlockMode(record.mode) ||
            record.tick_delay < 0) {
            return fail(error, "invalid Infinitecz command-block record");
        }
        const uint64_t coordinate = static_cast<uint64_t>(record.x) +
            static_cast<uint64_t>(record.z) * width +
            static_cast<uint64_t>(record.y) * width * length;
        if (!command_coordinates.emplace(coordinate).second) {
            return fail(error, "Infinitecz command-block records contain duplicate coordinates");
        }
        for (const std::string* value : {&record.command, &record.last_output,
                                         &record.name, &record.filtered_name}) {
            if (!validString(*value, CommandBlockSpoolWriter::kMaximumStringBytes)) {
                return fail(error, "invalid Infinitecz command-block text");
            }
        }
        flags |= kFlagCommands;
    }

    PlaintextFileGuard plaintext_guard(part_path);
    PlaintextFileGuard compressed_guard(compressed_path);
    std::ofstream output(part_path, std::ios::binary | std::ios::trunc);
    if (!output) return fail(error, "cannot create Infinitecz plaintext staging file");
    std::array<uint8_t, kHeaderSize> header{};
    std::copy(std::begin(kMagic), std::end(kMagic), header.begin());
    appendLe32(&header, 8, kFormatVersion);
    appendLe32(&header, 12, flags);
    appendLe32(&header, 16, static_cast<uint32_t>(request.width));
    appendLe32(&header, 20, static_cast<uint32_t>(request.height));
    appendLe32(&header, 24, static_cast<uint32_t>(request.length));
    appendLe32(&header, 28, static_cast<uint32_t>(request.origin_x));
    appendLe32(&header, 32, static_cast<uint32_t>(request.origin_y));
    appendLe32(&header, 36, static_cast<uint32_t>(request.origin_z));
    appendLe64(&header, 40, volume);
    appendLe32(&header, 48, static_cast<uint32_t>(request.palette.size()));
    appendLe64(&header, 52, static_cast<uint64_t>(request.block_indices.size()));
    appendLe64(&header, 60, static_cast<uint64_t>(request.raw_blocks.size()));
    appendLe64(&header, 68, static_cast<uint64_t>(request.command_blocks.size()));
    appendLe32(&header, 76, static_cast<uint32_t>(request.display_name.size()));
    appendLe32(&header, 80, static_cast<uint32_t>(kHeaderSize));
    appendLe32(&header, 84, static_cast<uint32_t>(kTrailerSize));
    output.write(reinterpret_cast<const char*>(header.data()), header.size());
    if (!output) {
        output.close();
        std::remove(part_path.c_str());
        return fail(error, "cannot write Infinitecz header");
    }
    PayloadWriter writer(&output);
    if (!writer.string(request.display_name)) {
        output.close();
        std::remove(part_path.c_str());
        return fail(error, "cannot write Infinitecz metadata");
    }
    for (const std::string& state : request.palette) {
        if (!writer.string(state)) {
            output.close();
            std::remove(part_path.c_str());
            return fail(error, "cannot write Infinitecz palette");
        }
    }
    for (const SchematicRawBlock& record : request.raw_blocks) {
        if (!writer.i32(record.x) || !writer.i32(record.y) || !writer.i32(record.z) ||
            !writer.u16(record.aux) || !writer.i32(record.legacy_id) ||
            !writer.string(record.identifier) || !writer.string(record.state_json) ||
            !writer.string(record.entity_json)) {
            output.close();
            std::remove(part_path.c_str());
            return fail(error, "cannot write Infinitecz raw-state data");
        }
    }
    for (const CommandBlockRecord& record : request.command_blocks) {
        if (!writeCommand(&writer, record)) {
            output.close();
            std::remove(part_path.c_str());
            return fail(error, "cannot write Infinitecz command data");
        }
    }
    for (size_t index = 0; index < request.block_indices.size(); ++index) {
        if ((index & 0x3fffU) == 0U && request.cancellation_requested &&
            request.cancellation_requested()) {
            output.close();
            std::remove(part_path.c_str());
            return fail(error, "Infinitecz write cancelled");
        }
        if (!writer.u16(request.block_indices[index])) {
            output.close();
            std::remove(part_path.c_str());
            return fail(error, "cannot write Infinitecz block data");
        }
    }
    if (request.cancellation_requested && request.cancellation_requested()) {
        output.close();
        std::remove(part_path.c_str());
        return fail(error, "Infinitecz write cancelled");
    }
    if (!writeTrailer(&output, writer.payloadSize(), writer.checksum())) {
        output.close();
        std::remove(part_path.c_str());
        return fail(error, "cannot write Infinitecz trailer");
    }
    output.flush();
    output.close();
    if (!output || !syncFile(part_path)) {
        std::remove(part_path.c_str());
        return fail(error, "cannot finish Infinitecz plaintext staging file");
    }
    InfinityCompressionOptions compression_options;
    compression_options.cancellation_requested = request.cancellation_requested;
    compression_options.maximum_uncompressed_bytes = kMaximumStagingFileBytes;
    compression_options.maximum_compressed_bytes =
        kMaximumStagingFileBytes - InfinityCompressionCodec::kHeaderSize;
    if (!InfinityCompressionCodec::compressFile(part_path, compressed_path,
                                                 compression_options, error)) {
        return false;
    }
    InfinityCryptoOptions crypto_options;
    crypto_options.cancellation_requested = request.cancellation_requested;
    crypto_options.publish_mutex = request.publish_mutex;
    crypto_options.publication_committed = request.publication_committed;
    crypto_options.maximum_plaintext_bytes = kMaximumStagingFileBytes;
    if (!InfinityCryptoCodec::encryptFileAtomic(compressed_path, request.output_path,
                                                 infinityFormatKeyV1(), crypto_options, error)) {
        return false;
    }
    if (error) error->clear();
    return true;
}

}  // namespace build_import
