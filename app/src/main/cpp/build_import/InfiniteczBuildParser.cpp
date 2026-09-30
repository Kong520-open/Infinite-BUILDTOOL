#include "InfiniteczBuildParser.h"
#include "ContainerEntityCodec.h"
#include "InfiniteczBuildWriter.h"
#include "InfinityCompressionCodec.h"
#include "InfinityCryptoCodec.h"
#include "InfinityFormatKey.h"
#include "SignEntityCodec.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace build_import {
namespace {

constexpr size_t kHeaderSize = 96;
constexpr size_t kTrailerSize = 24;
constexpr uint64_t kMaximumBlockCount = 16ULL * 1024ULL * 1024ULL;
constexpr uint32_t kMaximumPaletteSize = 65'536U;
constexpr uint32_t kMaximumStringBytes = 4U * 1024U * 1024U;
constexpr uint64_t kMaximumRawPayloadBytes = 512ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumStagingFileBytes = 1ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr char kMagic[8] = {'I', 'C', 'B', 'U', 'I', 'L', 'D', 1};
constexpr char kTrailerMagic[8] = {'I', 'C', 'B', 'E', 'N', 'D', 1, 0};

class DecryptedFileGuard {
public:
    explicit DecryptedFileGuard(std::string path)
        : path_(std::move(path)), part_path_(path_ + ".part") {}
    ~DecryptedFileGuard() {
        std::remove(part_path_.c_str());
        std::remove(path_.c_str());
    }

    DecryptedFileGuard(const DecryptedFileGuard&) = delete;
    DecryptedFileGuard& operator=(const DecryptedFileGuard&) = delete;

private:
    std::string path_;
    std::string part_path_;
};

std::string uniqueDecryptedPath(const std::string& spool_directory) {
    static std::atomic<uint64_t> sequence{0};
    const uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const uint64_t suffix = sequence.fetch_add(1, std::memory_order_relaxed);
    const bool has_separator = !spool_directory.empty() &&
        (spool_directory.back() == '/' || spool_directory.back() == '\\');
    return spool_directory + (has_separator ? "" : "/") +
        ".infinity-decrypted-" + std::to_string(timestamp) + "-" +
        std::to_string(suffix) + ".IBuild";
}

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool isSignIdentifier(std::string_view identifier) {
    const size_t state = identifier.find('[');
    if (state != std::string_view::npos) identifier = identifier.substr(0, state);
    const size_t separator = identifier.rfind(':');
    const std::string_view leaf = separator == std::string_view::npos
        ? identifier : identifier.substr(separator + 1U);
    return leaf.find("sign") != std::string_view::npos;
}

enum class SignShellKind {
    None,
    Standing,
    Wall,
};

std::string_view identifierLeaf(std::string_view identifier) {
    const size_t state = identifier.find('[');
    if (state != std::string_view::npos) identifier = identifier.substr(0, state);
    const size_t separator = identifier.rfind(':');
    return separator == std::string_view::npos
        ? identifier : identifier.substr(separator + 1U);
}

bool hasSuffix(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
        value.substr(value.size() - suffix.size()) == suffix;
}

SignShellKind signShellKind(std::string_view identifier) {
    const std::string_view leaf = identifierLeaf(identifier);
    if (leaf == "standing_sign" || hasSuffix(leaf, "_standing_sign")) {
        return SignShellKind::Standing;
    }
    if (leaf == "wall_sign" ||
        (hasSuffix(leaf, "_wall_sign") && !hasSuffix(leaf, "_wall_hanging_sign"))) {
        return SignShellKind::Wall;
    }
    return SignShellKind::None;
}

bool isGenericLegacySign(std::string_view identifier) {
    const std::string_view leaf = identifierLeaf(identifier);
    return leaf == "standing_sign" || leaf == "wall_sign";
}

void reportProgress(const SchematicParseOptions& options, SchematicParseStage stage,
                    uint64_t completed = 0, uint64_t total = 0) {
    if (options.progress_callback) options.progress_callback({stage, completed, total});
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
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xffU] ^ (crc >> 8U);
    return crc;
}

uint16_t le16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
        static_cast<uint16_t>(data[1]) << 8U;
}

uint32_t le32(const uint8_t* data) {
    uint32_t result = 0;
    for (size_t i = 0; i < 4; ++i) result |= static_cast<uint32_t>(data[i]) << (i * 8U);
    return result;
}

uint64_t le64(const uint8_t* data) {
    uint64_t result = 0;
    for (size_t i = 0; i < 8; ++i) result |= static_cast<uint64_t>(data[i]) << (i * 8U);
    return result;
}

class PayloadReader {
public:
    PayloadReader(std::ifstream* input, uint64_t remaining)
        : input_(input), remaining_(remaining) {}

    bool bytes(void* output, size_t size) {
        if (!input_ || (!output && size != 0) || size > remaining_) return false;
        if (size != 0) {
            input_->read(static_cast<char*>(output), static_cast<std::streamsize>(size));
            if (!*input_) return false;
            crc_ = crc32Update(crc_, static_cast<const uint8_t*>(output), size);
            remaining_ -= size;
        }
        return true;
    }

    bool u8(uint8_t* value) { return value && bytes(value, 1); }

    bool u16(uint16_t* value) {
        uint8_t data[2]{};
        if (!value || !bytes(data, sizeof(data))) return false;
        *value = le16(data);
        return true;
    }

    bool u32(uint32_t* value) {
        uint8_t data[4]{};
        if (!value || !bytes(data, sizeof(data))) return false;
        *value = le32(data);
        return true;
    }

    bool i32(int32_t* value) {
        uint32_t raw = 0;
        if (!value || !u32(&raw)) return false;
        *value = static_cast<int32_t>(raw);
        return true;
    }

    bool string(std::string* value, uint32_t maximum, bool allow_empty = true) {
        uint32_t size = 0;
        if (!value || !u32(&size) || size > maximum || (!allow_empty && size == 0) ||
            size > remaining_) return false;
        try {
            value->assign(size, '\0');
        } catch (const std::bad_alloc&) {
            return false;
        }
        if (size != 0 && !bytes(&(*value)[0], size)) return false;
        return value->find('\0') == std::string::npos;
    }

    uint64_t remaining() const { return remaining_; }
    uint32_t checksum() const { return crc_ ^ 0xffffffffU; }

private:
    std::ifstream* input_ = nullptr;
    uint64_t remaining_ = 0;
    uint32_t crc_ = 0xffffffffU;
};

bool readFileBytes(std::ifstream* input, void* output, size_t size) {
    if (!input || (!output && size != 0)) return false;
    input->read(static_cast<char*>(output), static_cast<std::streamsize>(size));
    return static_cast<bool>(*input);
}

bool appendMappedBlock(const SchematicParseOptions& options,
                       ChunkSpoolWriter* writer, const ParsedBlock& block,
                       SchematicParseResult* result, std::string* error) {
    if (!result) return false;
    if (options.maximum_output_blocks != 0 &&
        result->imported_block_count >= options.maximum_output_blocks) {
        return fail(error, "parsed block count exceeds configured limit of " +
            std::to_string(options.maximum_output_blocks));
    }
    if (options.block_sink) {
        if (!options.block_sink(block, error)) return false;
    } else if (!writer || !writer->append(block, error)) {
        return fail(error, "Infinitecz block output is unavailable");
    }
    ++result->imported_block_count;
    return true;
}

}  // namespace

bool InfiniteczBuildParser::parse(const SchematicParseOptions& options,
                                  const BlockMapper& mapper,
                                  SchematicParseResult* result,
                                  std::string* error) const {
    if (error) error->clear();
    if (!result || options.source_path.empty() || options.spool_directory.empty() ||
        options.chunk_size <= 0) {
        return fail(error, "invalid Infinitecz parse options");
    }
    *result = {};
    if (options.cancellation_requested && options.cancellation_requested()) {
        return fail(error, "Infinitecz parse cancelled");
    }

    std::string payload_path = options.source_path;
    std::unique_ptr<DecryptedFileGuard> decrypted_guard;
    std::unique_ptr<DecryptedFileGuard> decompressed_guard;
    try {
        bool encrypted = false;
        if (!InfinityCryptoCodec::probeEncryptedFile(payload_path, &encrypted, error)) {
            return false;
        }
        if (encrypted) {
            const std::string plaintext_path = uniqueDecryptedPath(options.spool_directory);
            decrypted_guard = std::make_unique<DecryptedFileGuard>(plaintext_path);
            InfinityCryptoOptions crypto_options;
            crypto_options.cancellation_requested = options.cancellation_requested;
            crypto_options.maximum_plaintext_bytes = kMaximumStagingFileBytes;
            if (!InfinityCryptoCodec::decryptFileAtomic(payload_path, plaintext_path,
                                                         infinityFormatKeyV1(), crypto_options,
                                                         error)) {
                return false;
            }
            payload_path = plaintext_path;
        }

        bool compressed = false;
        if (!InfinityCompressionCodec::probeCompressedFile(payload_path, &compressed, error)) {
            return false;
        }
        if (compressed) {
            const std::string uncompressed_path = uniqueDecryptedPath(options.spool_directory);
            decompressed_guard = std::make_unique<DecryptedFileGuard>(uncompressed_path);
            InfinityCompressionOptions compression_options;
            compression_options.cancellation_requested = options.cancellation_requested;
            compression_options.maximum_uncompressed_bytes = kMaximumStagingFileBytes;
            compression_options.maximum_compressed_bytes =
                kMaximumStagingFileBytes - InfinityCompressionCodec::kHeaderSize;
            if (!InfinityCompressionCodec::decompressFile(payload_path, uncompressed_path,
                                                           compression_options, error)) {
                return false;
            }
            payload_path = uncompressed_path;
        }
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory to prepare Infinitecz building file");
    }

    std::ifstream input(payload_path, std::ios::binary);
    if (!input) return fail(error, "cannot open Infinitecz building file");
    input.seekg(0, std::ios::end);
    const std::streamoff file_size = input.tellg();
    if (file_size < static_cast<std::streamoff>(kHeaderSize + kTrailerSize)) {
        return fail(error, "Infinitecz file is truncated");
    }
    std::array<uint8_t, kTrailerSize> trailer{};
    input.seekg(file_size - static_cast<std::streamoff>(kTrailerSize), std::ios::beg);
    if (!readFileBytes(&input, trailer.data(), trailer.size()) ||
        !std::equal(std::begin(kTrailerMagic), std::end(kTrailerMagic), trailer.begin())) {
        return fail(error, "Infinitecz trailer is invalid");
    }
    const uint64_t payload_size = le64(trailer.data() + 8U);
    const uint32_t expected_crc = le32(trailer.data() + 16U);
    if (payload_size != static_cast<uint64_t>(file_size) - kHeaderSize - kTrailerSize) {
        return fail(error, "Infinitecz payload length does not match the file");
    }
    std::array<uint8_t, kHeaderSize> header{};
    input.seekg(0, std::ios::beg);
    if (!readFileBytes(&input, header.data(), header.size()) ||
        !std::equal(std::begin(kMagic), std::end(kMagic), header.begin())) {
        return fail(error, "Infinitecz header is invalid");
    }
    if (le32(header.data() + 8U) != InfiniteczBuildWriter::kFormatVersion ||
        le32(header.data() + 80U) != kHeaderSize || le32(header.data() + 84U) != kTrailerSize) {
        return fail(error, "unsupported Infinitecz format version");
    }
    const uint32_t flags = le32(header.data() + 12U);
    const int32_t width = static_cast<int32_t>(le32(header.data() + 16U));
    const int32_t height = static_cast<int32_t>(le32(header.data() + 20U));
    const int32_t length = static_cast<int32_t>(le32(header.data() + 24U));
    const int32_t source_origin_x = static_cast<int32_t>(le32(header.data() + 28U));
    const int32_t source_origin_y = static_cast<int32_t>(le32(header.data() + 32U));
    const int32_t source_origin_z = static_cast<int32_t>(le32(header.data() + 36U));
    const uint64_t volume = le64(header.data() + 40U);
    const uint32_t palette_count = le32(header.data() + 48U);
    const uint64_t block_count = le64(header.data() + 52U);
    const uint64_t raw_count = le64(header.data() + 60U);
    const uint64_t command_count = le64(header.data() + 68U);
    const uint32_t display_size = le32(header.data() + 76U);
    if (width <= 0 || height <= 0 || length <= 0 || palette_count == 0 ||
        palette_count > kMaximumPaletteSize || volume == 0 || volume > kMaximumBlockCount ||
        block_count != volume || display_size > 4096U || raw_count > volume ||
        command_count > volume || static_cast<uint64_t>(width) * height >
            std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(length)) {
        return fail(error, "Infinitecz header dimensions or counts are invalid");
    }
    const int64_t base_x = options.base_x;
    const int64_t base_y = options.base_y;
    const int64_t base_z = options.base_z;
    const auto fits = [](int64_t base, int32_t extent) {
        return base >= std::numeric_limits<int32_t>::min() &&
            base + static_cast<int64_t>(extent) - 1 <= std::numeric_limits<int32_t>::max();
    };
    if (!fits(base_x, width) || !fits(base_y, height) || !fits(base_z, length)) {
        return fail(error, "Infinitecz paste coordinates exceed the supported world range");
    }
    input.seekg(static_cast<std::streamoff>(kHeaderSize), std::ios::beg);
    PayloadReader payload(&input, payload_size);
    std::string display_name;
    if (!payload.string(&display_name, display_size) || display_name.size() != display_size) {
        return fail(error, "Infinitecz metadata is invalid");
    }
    std::vector<std::string> palette;
    try {
        palette.reserve(palette_count);
        for (uint32_t index = 0; index < palette_count; ++index) {
            std::string state;
            if (!payload.string(&state, kMaximumStringBytes, false)) {
                return fail(error, "Infinitecz palette is truncated or invalid");
            }
            palette.push_back(std::move(state));
        }
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory for Infinitecz palette");
    }
    if (palette.front() != "minecraft:air") return fail(error, "Infinitecz palette has no air entry");

    const uint64_t layer = static_cast<uint64_t>(width) * static_cast<uint64_t>(length);
    std::vector<SchematicRawBlock> raw_blocks;
    std::unordered_map<uint64_t, size_t> raw_lookup;
    try {
        raw_blocks.reserve(static_cast<size_t>(raw_count));
        raw_lookup.reserve(static_cast<size_t>(raw_count));
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory for Infinitecz raw-state records");
    }
    uint64_t raw_payload_bytes = 0;
    for (uint64_t index = 0; index < raw_count; ++index) {
        SchematicRawBlock record;
        if (!payload.i32(&record.x) || !payload.i32(&record.y) || !payload.i32(&record.z) ||
            !payload.u16(&record.aux) || !payload.i32(&record.legacy_id) ||
            !payload.string(&record.identifier, 1024U, false) ||
            !payload.string(&record.state_json, 4U * 1024U * 1024U) ||
            !payload.string(&record.entity_json, 64U * 1024U * 1024U)) {
            return fail(error, "Infinitecz raw-state section is truncated");
        }
        if (record.x < 0 || record.y < 0 || record.z < 0 || record.x >= width ||
            record.y >= height || record.z >= length) {
            return fail(error, "Infinitecz raw-state coordinate is outside the volume");
        }
        const uint64_t coordinate = static_cast<uint64_t>(record.x) +
            static_cast<uint64_t>(record.z) * static_cast<uint64_t>(width) +
            static_cast<uint64_t>(record.y) * layer;
        if (!raw_lookup.emplace(coordinate, raw_blocks.size()).second) {
            return fail(error, "Infinitecz raw-state section contains duplicate coordinates");
        }
        const uint64_t bytes = record.identifier.size() + record.state_json.size() +
            record.entity_json.size();
        if (bytes > kMaximumRawPayloadBytes || raw_payload_bytes > kMaximumRawPayloadBytes - bytes) {
            return fail(error, "Infinitecz raw-state section is too large");
        }
        raw_payload_bytes += bytes;
        raw_blocks.push_back(std::move(record));
    }

    std::vector<CommandBlockRecord> command_blocks;
    std::unordered_map<uint64_t, size_t> command_lookup;
    try {
        command_blocks.reserve(static_cast<size_t>(command_count));
        command_lookup.reserve(static_cast<size_t>(command_count));
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory for Infinitecz command records");
    }
    for (uint64_t index = 0; index < command_count; ++index) {
        CommandBlockRecord record;
        uint8_t flags_byte = 0;
        uint8_t reserved = 0;
        if (!payload.i32(&record.x) || !payload.i32(&record.y) || !payload.i32(&record.z) ||
            !payload.u16(&record.mode) || !payload.u8(&flags_byte) || !payload.u8(&reserved) ||
            !payload.i32(&record.tick_delay) ||
            !payload.string(&record.command, CommandBlockSpoolWriter::kMaximumStringBytes) ||
            !payload.string(&record.last_output, CommandBlockSpoolWriter::kMaximumStringBytes) ||
            !payload.string(&record.name, CommandBlockSpoolWriter::kMaximumStringBytes) ||
            !payload.string(&record.filtered_name, CommandBlockSpoolWriter::kMaximumStringBytes) ||
            !isValidCommandBlockMode(record.mode) || record.tick_delay < 0 || reserved != 0) {
            return fail(error, "Infinitecz command section is truncated or invalid");
        }
        if (record.x < 0 || record.y < 0 || record.z < 0 || record.x >= width ||
            record.y >= height || record.z >= length) {
            return fail(error, "Infinitecz command coordinate is outside the volume");
        }
        const uint64_t coordinate = static_cast<uint64_t>(record.x) +
            static_cast<uint64_t>(record.z) * static_cast<uint64_t>(width) +
            static_cast<uint64_t>(record.y) * layer;
        if (!command_lookup.emplace(coordinate, command_blocks.size()).second) {
            return fail(error, "Infinitecz command section contains duplicate coordinates");
        }
        record.redstone_mode = (flags_byte & 1U) != 0U;
        record.conditional = (flags_byte & 2U) != 0U;
        record.output_tracked = (flags_byte & 4U) != 0U;
        record.executing_on_first_tick = (flags_byte & 8U) != 0U;
        command_blocks.push_back(std::move(record));
    }

    result->source_voxel_count = volume;
    result->source_offset_x = 0;
    result->source_offset_y = 0;
    result->source_offset_z = 0;
    result->source_volume_bounds = {
        static_cast<int32_t>(base_x), static_cast<int32_t>(base_y), static_cast<int32_t>(base_z),
        static_cast<int32_t>(base_x + width - 1), static_cast<int32_t>(base_y + height - 1),
        static_cast<int32_t>(base_z + length - 1)};
    result->sponge_format = false;
    std::unique_ptr<ChunkSpoolWriter> writer;
    if (!options.block_sink) {
        writer = std::make_unique<ChunkSpoolWriter>(options.spool_directory, options.chunk_size,
                                                    options.maximum_chunk_descriptors);
        if (options.include_source_volume && !writer->includeVolume(
                result->source_volume_bounds, error, options.cancellation_requested)) return false;
    }
    if (!options.raw_block_sink) {
        try {
            result->raw_blocks.reserve(raw_blocks.size());
        } catch (const std::bad_alloc&) {
            return fail(error, "not enough memory for parsed Infinitecz raw-state records");
        }
    }
    std::vector<ContainerItemRecord> deferred_container_items;
    if (options.container_item_sink) {
        try {
            deferred_container_items.reserve(std::min<uint64_t>(
                raw_blocks.size(), static_cast<uint64_t>(1024U)));
        } catch (const std::bad_alloc&) {
            return fail(error, "not enough memory for Infinitecz container-item records");
        }
    }
    const auto map_portable_state = [&](std::string_view state) {
        if (!options.state_block_resolver) return mapper.mapInfiniteczState(state);
        BlockMappingResult mapping = options.state_block_resolver(state);
        return mapping.status == BlockMappingStatus::Unsupported
            ? mapper.mapInfiniteczState(state) : mapping;
    };
    for (SchematicRawBlock& record : raw_blocks) {
        record.x = static_cast<int32_t>(base_x + record.x);
        record.y = static_cast<int32_t>(base_y + record.y);
        record.z = static_cast<int32_t>(base_z + record.z);
        if (options.raw_block_sink) {
            if (!options.raw_block_sink(record, error)) return false;
        }
        ++result->raw_block_payload_count;
    }

    reportProgress(options, SchematicParseStage::RoutingBlocks, 0, volume);
    for (uint64_t index = 0; index < volume; ++index) {
        if ((index & 0x3ffU) == 0U) {
            if (options.cancellation_requested && options.cancellation_requested()) {
                return fail(error, "Infinitecz parse cancelled");
            }
            reportProgress(options, SchematicParseStage::RoutingBlocks, index, volume);
        }
        uint16_t palette_id = 0;
        if (!payload.u16(&palette_id) || palette_id >= palette.size()) {
            return fail(error, "Infinitecz block section is truncated or references an invalid palette");
        }
        const auto raw_it = raw_lookup.find(index);
        // The portable palette is only a compatibility view. A native record
        // remains authoritative when an exact Bedrock block (for example a
        // hanging sign with native aux/state) has no Java palette equivalent
        // and was therefore represented as air during export.
        if (palette_id == 0 && raw_it == raw_lookup.end()) {
            ++result->skipped_block_count;
            continue;
        }
        const int32_t local_x = static_cast<int32_t>(index % static_cast<uint64_t>(width));
        const int32_t local_z = static_cast<int32_t>((index / static_cast<uint64_t>(width)) %
                                                      static_cast<uint64_t>(length));
        const int32_t local_y = static_cast<int32_t>(index / layer);
        BlockMappingResult mapping;
        std::string source_state;
        if (raw_it != raw_lookup.end()) {
            const SchematicRawBlock& raw = raw_blocks[raw_it->second];
            mapping = mapper.mapInfiniteczState(raw.identifier, raw.aux, true, raw.state_json);
            source_state = raw.identifier;
            // A short-lived exporter trusted the optional client GetBlock()
            // name for raw sign records. On affected versions that API
            // flattened every wood family to standing_sign/wall_sign, while
            // the portable palette still retained the exact material. Recover
            // only that missing identity here; native aux remains authoritative
            // for orientation and other Bedrock placement state.
            if (mapping.isMapped() && isGenericLegacySign(raw.identifier) &&
                palette_id != 0) {
                BlockMappingResult palette_mapping = map_portable_state(palette[palette_id]);
                if (palette_mapping.isMapped() &&
                    signShellKind(mapping.spec.command_name) != SignShellKind::None &&
                    signShellKind(mapping.spec.command_name) ==
                        signShellKind(palette_mapping.spec.command_name)) {
                    mapping.spec.command_name =
                        std::move(palette_mapping.spec.command_name);
                }
            }
            if (mapping.status == BlockMappingStatus::Unsupported &&
                raw.legacy_id > 0 && raw.legacy_id <= UINT16_MAX && raw.aux <= UINT8_MAX) {
                // Modern native blocks frequently have no legacy numeric ID
                // and record zero here. Treating that sentinel as legacy air
                // would skip a valid portable palette fallback entirely.
                mapping = mapper.mapLegacy(static_cast<uint16_t>(raw.legacy_id),
                                           static_cast<uint8_t>(raw.aux));
            }
            if (mapping.status == BlockMappingStatus::Unsupported) {
                mapping = map_portable_state(palette[palette_id]);
                source_state = palette[palette_id];
            }
        } else {
            source_state = palette[palette_id];
            mapping = map_portable_state(source_state);
        }
        if (mapping.status == BlockMappingStatus::Air) {
            ++result->skipped_block_count;
            continue;
        }
        if (mapping.status == BlockMappingStatus::Unsupported) {
            ++result->unsupported_block_count;
            return fail(error, "unsupported Infinitecz block at local (" +
                std::to_string(local_x) + "," + std::to_string(local_y) + "," +
                std::to_string(local_z) + "): " + source_state);
        }
        const auto raw_record = raw_it == raw_lookup.end() ? nullptr
            : &raw_blocks[raw_it->second];
        if (raw_record && !raw_record->entity_json.empty() &&
            deferredContainerIdentifier(mapping.spec.command_name)) {
            if (!options.container_item_sink) {
                ++result->omitted_block_entity_count;
            } else {
                std::vector<ContainerItemRecord> parsed_items;
                std::string item_error;
                if (!parseContainerEntityJson(raw_record->entity_json, &parsed_items,
                                               &item_error)) {
                    return fail(error, item_error.empty()
                        ? "invalid Infinitecz container-item payload"
                        : "invalid Infinitecz container-item payload: " + item_error);
                } else {
                    std::string expected_container_id = mapping.spec.command_name;
                    if (!normalizeDeferredItemIdentifier(&expected_container_id) ||
                        !deferredContainerIdentifier(expected_container_id)) {
                        ++result->omitted_block_entity_count;
                    } else {
                        for (ContainerItemRecord& item : parsed_items) {
                            if (deferred_container_items.size() >=
                                ContainerItemSpoolWriter::kMaximumRecords) {
                                return fail(error,
                                            "Infinitecz container-item payload exceeds the safety limit");
                            }
                            item.x = static_cast<int32_t>(base_x + local_x);
                            item.y = static_cast<int32_t>(base_y + local_y);
                            item.z = static_cast<int32_t>(base_z + local_z);
                            item.expected_container_id = expected_container_id;
                            deferred_container_items.push_back(std::move(item));
                        }
                    }
                }
            }
        }
        if (raw_record && !raw_record->entity_json.empty() &&
            (isSignIdentifier(mapping.spec.command_name) ||
             isSignIdentifier(raw_record->identifier))) {
            if (!options.sign_sink) {
                ++result->omitted_block_entity_count;
            } else {
                SignRecord sign;
                std::string sign_error;
                if (!parseSignEntityJson(raw_record->entity_json, &sign, &sign_error)) {
                    return fail(error, sign_error.empty()
                        ? "invalid Infinitecz sign payload"
                        : "invalid Infinitecz sign payload: " + sign_error);
                }
                if (signRecordHasPayload(sign)) {
                    sign.x = static_cast<int32_t>(base_x + local_x);
                    sign.y = static_cast<int32_t>(base_y + local_y);
                    sign.z = static_cast<int32_t>(base_z + local_z);
                    sign.expected_sign_id = mapping.spec.command_name;
                    sign.has_expected_aux = true;
                    sign.expected_aux = mapping.spec.aux;
                    if (!options.sign_sink(sign, error)) {
                        return fail(error, error && !error->empty()
                            ? *error : "cannot persist Infinitecz sign payload");
                    }
                    ++result->sign_payload_count;
                }
            }
        }
        const auto command_it = command_lookup.find(index);
        if (command_it != command_lookup.end()) {
            const CommandBlockRecord& command = command_blocks[command_it->second];
            mapping.spec.aux = static_cast<uint16_t>((mapping.spec.aux & ~0x08U) |
                (command.conditional ? 0x08U : 0U));
        }
        ParsedBlock block{static_cast<int32_t>(base_x + local_x),
                          static_cast<int32_t>(base_y + local_y),
                          static_cast<int32_t>(base_z + local_z), mapping.spec};
        if (!appendMappedBlock(options, writer.get(), block, result, error)) return false;
        if (!mapping.reason.empty()) {
            ++result->degraded_block_count;
            if (result->first_degradation_reason.empty()) result->first_degradation_reason = mapping.reason;
        }
    }
    if (payload.remaining() != 0 || payload.checksum() != expected_crc) {
        return fail(error, "Infinitecz payload checksum or length mismatch");
    }
    if (options.container_item_sink) {
        for (const ContainerItemRecord& item : deferred_container_items) {
            if (!options.container_item_sink(item, error)) {
                if (error && error->empty()) {
                    *error = "Infinitecz container-item sink rejected a payload";
                }
                return false;
            }
            ++result->container_item_payload_count;
        }
    }
    if (!options.raw_block_sink) {
        for (SchematicRawBlock& record : raw_blocks) {
            result->raw_blocks.push_back(std::move(record));
        }
    }
    if (options.command_block_sink) {
        for (CommandBlockRecord& record : command_blocks) {
            record.x = static_cast<int32_t>(base_x + record.x);
            record.y = static_cast<int32_t>(base_y + record.y);
            record.z = static_cast<int32_t>(base_z + record.z);
            if (!options.command_block_sink(record, error)) return false;
            ++result->command_block_payload_count;
        }
    } else {
        result->omitted_command_block_data_count = command_blocks.size();
    }
    reportProgress(options, SchematicParseStage::RoutingBlocks, volume, volume);
    if (!options.block_sink) {
        reportProgress(options, SchematicParseStage::FinalizingSpools);
        result->chunks = writer->finish(error, options.cancellation_requested);
        if (result->chunks.empty() && result->imported_block_count != 0) {
            if (error && error->empty()) *error = "cannot finalize Infinitecz chunk spools";
            return false;
        }
    }
    (void)flags;
    (void)source_origin_x;
    (void)source_origin_y;
    (void)source_origin_z;
    (void)display_name;
    return true;
}

}  // namespace build_import
