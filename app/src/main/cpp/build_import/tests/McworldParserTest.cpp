#include "../McworldParser.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

using namespace build_import;

namespace {

namespace fs = std::filesystem;

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt != 100; ++attempt) {
            path_ = fs::temp_directory_path() /
                    ("build_import_mcworld_parser_test_" + std::to_string(nonce) + "_" +
                     std::to_string(attempt));
            std::error_code error;
            if (fs::create_directory(path_, error)) return;
        }
        throw std::runtime_error("cannot create mcworld parser test directory");
    }

    ~ScopedTempDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    fs::path child(const std::string& name) const { return path_ / name; }

private:
    fs::path path_;
};

struct ZipEntry {
    std::string name;
    std::vector<uint8_t> bytes;
};

struct ParsedFixture {
    SchematicParseResult result;
    std::vector<ParsedBlock> blocks;
    std::vector<CommandBlockRecord> command_blocks;
    std::string error;
};

void appendU8(std::vector<uint8_t>* output, uint8_t value) {
    output->push_back(value);
}

void appendLe16(std::vector<uint8_t>* output, uint16_t value) {
    output->push_back(static_cast<uint8_t>(value));
    output->push_back(static_cast<uint8_t>(value >> 8));
}

void appendLe32(std::vector<uint8_t>* output, uint32_t value) {
    for (int shift = 0; shift != 32; shift += 8) {
        output->push_back(static_cast<uint8_t>(value >> shift));
    }
}

void appendLe64(std::vector<uint8_t>* output, uint64_t value) {
    for (int shift = 0; shift != 64; shift += 8) {
        output->push_back(static_cast<uint8_t>(value >> shift));
    }
}

void appendNbtString(std::vector<uint8_t>* output, const std::string& value) {
    assert(value.size() <= UINT16_MAX);
    appendLe16(output, static_cast<uint16_t>(value.size()));
    output->insert(output->end(), value.begin(), value.end());
}

void appendNbtNamedString(std::vector<uint8_t>* output, const std::string& name,
                          const std::string& value) {
    appendU8(output, 8U);
    appendNbtString(output, name);
    appendNbtString(output, value);
}

void appendNbtNamedInt(std::vector<uint8_t>* output, const std::string& name,
                       int32_t value) {
    appendU8(output, 3U);
    appendNbtString(output, name);
    appendLe32(output, static_cast<uint32_t>(value));
}

void appendNbtNamedByte(std::vector<uint8_t>* output, const std::string& name,
                        uint8_t value) {
    appendU8(output, 1U);
    appendNbtString(output, name);
    appendU8(output, value);
}

std::vector<uint8_t> commandBlockEntityNbt(int32_t x, int32_t y, int32_t z) {
    std::vector<uint8_t> output;
    appendU8(&output, 10U);  // Compound root
    appendNbtString(&output, "");
    // Use Bedrock's legacy no-underscore id to prove the parser derives its
    // final mode from the mapped command-block shell, not arbitrary text.
    appendNbtNamedString(&output, "id", "CommandBlock");
    appendNbtNamedInt(&output, "x", x);
    appendNbtNamedInt(&output, "y", y);
    appendNbtNamedInt(&output, "z", z);
    appendNbtNamedString(&output, "Command", "say mcworld command");
    appendNbtNamedString(&output, "CustomName", "mcworld command");
    appendNbtNamedByte(&output, "TrackOutput", 1U);
    appendNbtNamedByte(&output, "auto", 0U);
    appendNbtNamedInt(&output, "TickDelay", 3);
    appendU8(&output, 0U);
    return output;
}

void appendVarint(std::vector<uint8_t>* output, uint64_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7fU);
        value >>= 7U;
        if (value != 0U) byte = static_cast<uint8_t>(byte | 0x80U);
        output->push_back(byte);
    } while (value != 0U);
}

uint32_t crc32c(const uint8_t* bytes, size_t length) {
    uint32_t crc = 0xffffffffU;
    for (size_t index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (int bit = 0; bit != 8; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0x82f63b78U : crc >> 1U;
        }
    }
    return ~crc;
}

uint32_t maskLevelDbCrc(uint32_t crc) {
    return ((crc >> 15U) | (crc << 17U)) + 0xa282ead8U;
}

std::vector<uint8_t> rawDeflate(const std::vector<uint8_t>& source) {
    z_stream stream{};
    assert(deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                        Z_DEFAULT_STRATEGY) == Z_OK);
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(source.data()));
    stream.avail_in = static_cast<uInt>(source.size());

    std::vector<uint8_t> compressed;
    std::array<uint8_t, 4096> buffer{};
    int status = Z_OK;
    do {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        status = deflate(&stream, Z_FINISH);
        assert(status == Z_OK || status == Z_STREAM_END);
        const size_t written = buffer.size() - stream.avail_out;
        compressed.insert(compressed.end(), buffer.begin(), buffer.begin() +
                                                        static_cast<std::ptrdiff_t>(written));
    } while (status != Z_STREAM_END);
    assert(deflateEnd(&stream) == Z_OK);
    return compressed;
}

void appendZipArchive(std::vector<uint8_t>* output, const std::vector<ZipEntry>& entries,
                      bool deflate_entries) {
    struct CentralEntry {
        std::string name;
        uint32_t crc = 0;
        uint32_t compressed_size = 0;
        uint32_t uncompressed_size = 0;
        uint32_t local_offset = 0;
        uint16_t method = 0;
    };
    std::vector<CentralEntry> central_entries;
    central_entries.reserve(entries.size());

    for (const ZipEntry& entry : entries) {
        const std::vector<uint8_t> payload = deflate_entries ? rawDeflate(entry.bytes) : entry.bytes;
        assert(entry.name.size() <= UINT16_MAX);
        assert(payload.size() <= UINT32_MAX);
        assert(entry.bytes.size() <= UINT32_MAX);
        const uint32_t crc = static_cast<uint32_t>(
            ::crc32(0L, reinterpret_cast<const Bytef*>(entry.bytes.data()),
                    static_cast<uInt>(entry.bytes.size())));
        const uint32_t local_offset = static_cast<uint32_t>(output->size());
        const uint16_t method = deflate_entries ? 8U : 0U;

        appendLe32(output, 0x04034b50U);
        appendLe16(output, 20U);
        appendLe16(output, 0U);
        appendLe16(output, method);
        appendLe16(output, 0U);
        appendLe16(output, 0U);
        appendLe32(output, crc);
        appendLe32(output, static_cast<uint32_t>(payload.size()));
        appendLe32(output, static_cast<uint32_t>(entry.bytes.size()));
        appendLe16(output, static_cast<uint16_t>(entry.name.size()));
        appendLe16(output, 0U);
        output->insert(output->end(), entry.name.begin(), entry.name.end());
        output->insert(output->end(), payload.begin(), payload.end());

        central_entries.push_back({entry.name, crc, static_cast<uint32_t>(payload.size()),
                                   static_cast<uint32_t>(entry.bytes.size()), local_offset, method});
    }

    const uint32_t central_offset = static_cast<uint32_t>(output->size());
    for (const CentralEntry& entry : central_entries) {
        appendLe32(output, 0x02014b50U);
        appendLe16(output, 20U);
        appendLe16(output, 20U);
        appendLe16(output, 0U);
        appendLe16(output, entry.method);
        appendLe16(output, 0U);
        appendLe16(output, 0U);
        appendLe32(output, entry.crc);
        appendLe32(output, entry.compressed_size);
        appendLe32(output, entry.uncompressed_size);
        appendLe16(output, static_cast<uint16_t>(entry.name.size()));
        appendLe16(output, 0U);
        appendLe16(output, 0U);
        appendLe16(output, 0U);
        appendLe16(output, 0U);
        appendLe32(output, 0U);
        appendLe32(output, entry.local_offset);
        output->insert(output->end(), entry.name.begin(), entry.name.end());
    }
    const uint32_t central_size = static_cast<uint32_t>(output->size()) - central_offset;
    assert(central_entries.size() <= UINT16_MAX);
    appendLe32(output, 0x06054b50U);
    appendLe16(output, 0U);
    appendLe16(output, 0U);
    appendLe16(output, static_cast<uint16_t>(central_entries.size()));
    appendLe16(output, static_cast<uint16_t>(central_entries.size()));
    appendLe32(output, central_size);
    appendLe32(output, central_offset);
    appendLe16(output, 0U);
}

void writeMcworld(const fs::path& path, const std::vector<ZipEntry>& entries,
                  bool deflate_entries = false) {
    std::vector<uint8_t> archive;
    appendZipArchive(&archive, entries, deflate_entries);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(archive.data()),
                 static_cast<std::streamsize>(archive.size()));
    assert(output);
}

void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    assert(output);
}

std::vector<uint8_t> overworldSubchunkKey(int32_t chunk_x, int32_t chunk_z, int8_t subchunk_y) {
    std::vector<uint8_t> key;
    appendLe32(&key, static_cast<uint32_t>(chunk_x));
    appendLe32(&key, static_cast<uint32_t>(chunk_z));
    appendU8(&key, 0x2fU);
    appendU8(&key, static_cast<uint8_t>(subchunk_y));
    return key;
}

std::vector<uint8_t> overworldBlockEntityKey(int32_t chunk_x, int32_t chunk_z) {
    std::vector<uint8_t> key;
    appendLe32(&key, static_cast<uint32_t>(chunk_x));
    appendLe32(&key, static_cast<uint32_t>(chunk_z));
    appendU8(&key, 0x31U);
    return key;
}

size_t blockIndex(int local_x, int local_y, int local_z) {
    assert(local_x >= 0 && local_x < 16);
    assert(local_y >= 0 && local_y < 16);
    assert(local_z >= 0 && local_z < 16);
    // Bedrock persistent subchunks are YZX, unlike the XZY arrays used by
    // Java schematic formats: Y is the low nibble, Z the middle nibble and X
    // the high nibble. Keep this fixture-side encoding explicit so an X/Y
    // swap cannot be hidden by symmetric test coordinates.
    return static_cast<size_t>((local_x << 8) | (local_z << 4) | local_y);
}

std::vector<uint8_t> legacyV0Subchunk() {
    std::vector<uint8_t> result(1U + 4096U + 2048U, 0U);
    result[0] = 0U;
    result[1U + blockIndex(1, 2, 3)] = 1U;   // stone
    result[1U + blockIndex(5, 2, 4)] = 35U;  // red wool via aux 14
    const size_t wool_index = blockIndex(5, 2, 4);
    const size_t aux_offset = 1U + 4096U + wool_index / 2U;
    result[aux_offset] = static_cast<uint8_t>(
        result[aux_offset] | static_cast<uint8_t>(14U << ((wool_index & 1U) * 4U)));
    return result;
}

std::vector<uint8_t> legacyV2SubchunkWithLightArrays() {
    constexpr size_t kBlocksOffset = 1U;
    constexpr size_t kAuxOffset = kBlocksOffset + 4096U;
    constexpr size_t kLightOffset = kAuxOffset + 2048U;
    std::vector<uint8_t> result(kLightOffset + 4096U, 0U);
    result[0] = 2U;
    // This intentionally has different X/Y values. With the historical XZY
    // interpretation it would become a different world coordinate.
    const size_t gold_index = blockIndex(8, 9, 2);
    result[kBlocksOffset + gold_index] = 41U;  // gold block
    std::fill(result.begin() + static_cast<std::ptrdiff_t>(kLightOffset),
              result.begin() + static_cast<std::ptrdiff_t>(kLightOffset + 2048U), 0xa5U);
    std::fill(result.begin() + static_cast<std::ptrdiff_t>(kLightOffset + 2048U), result.end(),
              0x5aU);
    return result;
}

void appendLittleEndianNbtString(std::vector<uint8_t>* output, const std::string& key,
                                 const std::string& value) {
    appendU8(output, 8U);  // TAG_String
    appendLe16(output, static_cast<uint16_t>(key.size()));
    output->insert(output->end(), key.begin(), key.end());
    appendLe16(output, static_cast<uint16_t>(value.size()));
    output->insert(output->end(), value.begin(), value.end());
}

void appendLittleEndianNbtByte(std::vector<uint8_t>* output, const std::string& key,
                               uint8_t value) {
    appendU8(output, 1U);  // TAG_Byte
    appendLe16(output, static_cast<uint16_t>(key.size()));
    output->insert(output->end(), key.begin(), key.end());
    appendU8(output, value);
}

void appendPaletteEntry(
    std::vector<uint8_t>* output, const std::string& name,
    const std::vector<std::pair<std::string, uint8_t>>& byte_states = {}) {
    appendU8(output, 10U);  // unnamed TAG_Compound
    appendLe16(output, 0U);
    appendLittleEndianNbtString(output, "name", name);
    appendU8(output, 10U);  // TAG_Compound states
    appendLe16(output, 6U);
    constexpr std::array<char, 6> kStates = {'s', 't', 'a', 't', 'e', 's'};
    output->insert(output->end(), kStates.begin(), kStates.end());
    for (const auto& state : byte_states) {
        appendLittleEndianNbtByte(output, state.first, state.second);
    }
    appendU8(output, 0U);  // states end
    appendU8(output, 0U);  // root end
}

std::vector<uint8_t> paletteV9Subchunk() {
    constexpr int kBitsPerBlock = 2;
    constexpr int kEntriesPerWord = 32 / kBitsPerBlock;
    std::array<uint8_t, 4096> indices{};
    indices[blockIndex(2, 0, 1)] = 1U;  // stone
    indices[blockIndex(3, 0, 1)] = 2U;  // command-block shell
    indices[blockIndex(4, 0, 1)] = 3U;  // container shell

    std::vector<uint8_t> result;
    appendU8(&result, 9U);
    appendU8(&result, 1U);
    appendU8(&result, 1U);
    appendU8(&result, static_cast<uint8_t>(kBitsPerBlock << 1));
    for (size_t word_index = 0; word_index < indices.size() / kEntriesPerWord; ++word_index) {
        uint32_t word = 0U;
        for (int entry = 0; entry < kEntriesPerWord; ++entry) {
            const size_t index = word_index * static_cast<size_t>(kEntriesPerWord) +
                                 static_cast<size_t>(entry);
            word |= static_cast<uint32_t>(indices[index]) << (entry * kBitsPerBlock);
        }
        appendLe32(&result, word);
    }
    appendLe32(&result, 4U);
    appendPaletteEntry(&result, "minecraft:air");
    appendPaletteEntry(&result, "minecraft:stone");
    appendPaletteEntry(&result, "minecraft:command_block");
    appendPaletteEntry(&result, "minecraft:chest");
    return result;
}

std::vector<uint8_t> paletteV1Subchunk() {
    const size_t iron_index = blockIndex(13, 6, 9);
    std::vector<uint8_t> result;
    appendU8(&result, 1U);
    appendU8(&result, 2U);  // one-bit persistent palette, no storage-count byte in v1
    for (size_t word_index = 0; word_index < 128U; ++word_index) {
        const size_t first_index = word_index * 32U;
        const uint32_t word = iron_index >= first_index && iron_index < first_index + 32U
            ? 1U << static_cast<uint32_t>(iron_index - first_index) : 0U;
        appendLe32(&result, word);
    }
    appendLe32(&result, 2U);
    appendPaletteEntry(&result, "minecraft:air");
    appendPaletteEntry(&result, "minecraft:iron_block");
    return result;
}

std::vector<uint8_t> paletteV9StairSubchunk() {
    const size_t stair_index = blockIndex(6, 0, 1);
    std::vector<uint8_t> result;
    appendU8(&result, 9U);
    appendU8(&result, 1U);
    appendU8(&result, 2U);
    appendU8(&result, 2U);  // one bit per block, persistent palette
    for (size_t word_index = 0; word_index < 128U; ++word_index) {
        const size_t first_index = word_index * 32U;
        const uint32_t word = stair_index >= first_index && stair_index < first_index + 32U
            ? 1U << static_cast<uint32_t>(stair_index - first_index) : 0U;
        appendLe32(&result, word);
    }
    appendLe32(&result, 2U);
    appendPaletteEntry(&result, "minecraft:air");
    // Both values use TAG_Byte=1.  Only the *_bit state is boolean; the
    // direction must remain numeric or the imported stair faces east instead
    // of west. This guards the LevelDB palette NBT conversion path.
    appendPaletteEntry(&result, "minecraft:oak_stairs",
                       {{"weirdo_direction", 1U}, {"upside_down_bit", 1U}});
    return result;
}

std::vector<uint8_t> paletteV9NormalStoneSlabSubchunk() {
    constexpr int kBitsPerBlock = 2;
    constexpr int kEntriesPerWord = 32 / kBitsPerBlock;
    std::array<uint8_t, 4096> indices{};
    indices[blockIndex(2, 0, 1)] = 1U;  // upper smooth-stone slab
    indices[blockIndex(3, 0, 1)] = 2U;  // double smooth-stone slab

    std::vector<uint8_t> result;
    appendU8(&result, 9U);
    appendU8(&result, 1U);
    appendU8(&result, 0U);
    appendU8(&result, static_cast<uint8_t>(kBitsPerBlock << 1));
    for (size_t word_index = 0; word_index < indices.size() / kEntriesPerWord; ++word_index) {
        uint32_t word = 0U;
        for (int entry = 0; entry < kEntriesPerWord; ++entry) {
            const size_t index = word_index * static_cast<size_t>(kEntriesPerWord) +
                                 static_cast<size_t>(entry);
            word |= static_cast<uint32_t>(indices[index]) << (entry * kBitsPerBlock);
        }
        appendLe32(&result, word);
    }
    appendLe32(&result, 3U);
    appendPaletteEntry(&result, "minecraft:air");
    appendPaletteEntry(&result, "minecraft:normal_stone_slab", {{"top_slot_bit", 1U}});
    appendPaletteEntry(&result, "minecraft:normal_stone_double_slab", {{"top_slot_bit", 0U}});
    return result;
}

std::vector<uint8_t> paletteV9ThreeBitSubchunk() {
    constexpr int kBitsPerBlock = 3;
    constexpr size_t kEntriesPerWord = 32U / static_cast<size_t>(kBitsPerBlock);
    const size_t gold_index = blockIndex(7, 0, 1);
    std::vector<uint8_t> result;
    appendU8(&result, 9U);
    appendU8(&result, 1U);
    appendU8(&result, 3U);
    appendU8(&result, static_cast<uint8_t>(kBitsPerBlock << 1));
    const size_t word_count = (4096U + kEntriesPerWord - 1U) / kEntriesPerWord;
    for (size_t word_index = 0; word_index < word_count; ++word_index) {
        const size_t first_index = word_index * kEntriesPerWord;
        const uint32_t word = gold_index >= first_index && gold_index < first_index + kEntriesPerWord
            ? 4U << static_cast<uint32_t>((gold_index - first_index) * kBitsPerBlock) : 0U;
        appendLe32(&result, word);
    }
    appendLe32(&result, 5U);
    appendPaletteEntry(&result, "minecraft:air");
    appendPaletteEntry(&result, "minecraft:stone");
    appendPaletteEntry(&result, "minecraft:dirt");
    appendPaletteEntry(&result, "minecraft:grass_block");
    appendPaletteEntry(&result, "minecraft:gold_block");
    return result;
}

void appendPut(std::vector<uint8_t>* batch, const std::vector<uint8_t>& key,
               const std::vector<uint8_t>& value) {
    appendU8(batch, 1U);
    appendVarint(batch, key.size());
    batch->insert(batch->end(), key.begin(), key.end());
    appendVarint(batch, value.size());
    batch->insert(batch->end(), value.begin(), value.end());
}

void appendDelete(std::vector<uint8_t>* batch, const std::vector<uint8_t>& key) {
    appendU8(batch, 0U);  // LevelDB deletion record
    appendVarint(batch, key.size());
    batch->insert(batch->end(), key.begin(), key.end());
}

std::vector<uint8_t> levelDbFullRecord(const std::vector<uint8_t>& payload) {
    assert(payload.size() <= UINT16_MAX);
    std::vector<uint8_t> checksum_input;
    checksum_input.reserve(payload.size() + 1U);
    appendU8(&checksum_input, 1U);  // LevelDB FULL record
    checksum_input.insert(checksum_input.end(), payload.begin(), payload.end());

    std::vector<uint8_t> record;
    appendLe32(&record, maskLevelDbCrc(crc32c(checksum_input.data(), checksum_input.size())));
    appendLe16(&record, static_cast<uint16_t>(payload.size()));
    appendU8(&record, 1U);  // LevelDB FULL record
    record.insert(record.end(), payload.begin(), payload.end());
    return record;
}

struct LevelDbMutation {
    std::vector<uint8_t> key;
    std::vector<uint8_t> value;
    bool deleted = false;
};

std::vector<uint8_t> levelDbWal(uint64_t sequence,
                                const std::vector<LevelDbMutation>& mutations) {
    assert(mutations.size() <= UINT32_MAX);
    std::vector<uint8_t> batch;
    appendLe64(&batch, sequence);
    appendLe32(&batch, static_cast<uint32_t>(mutations.size()));
    for (const LevelDbMutation& mutation : mutations) {
        if (mutation.deleted) appendDelete(&batch, mutation.key);
        else appendPut(&batch, mutation.key, mutation.value);
    }
    return levelDbFullRecord(batch);
}

std::vector<uint8_t> levelDbWal() {
    std::vector<uint8_t> batch;
    appendLe64(&batch, 1U);
    appendLe32(&batch, 7U);
    appendPut(&batch, overworldSubchunkKey(-2, 3, 0), legacyV0Subchunk());
    appendPut(&batch, overworldSubchunkKey(-1, 3, 0), legacyV2SubchunkWithLightArrays());
    appendPut(&batch, overworldSubchunkKey(-2, 3, 1), paletteV9Subchunk());
    appendPut(&batch, overworldSubchunkKey(-2, 3, 2), paletteV9StairSubchunk());
    appendPut(&batch, overworldSubchunkKey(-2, 3, 3), paletteV9ThreeBitSubchunk());
    appendPut(&batch, overworldSubchunkKey(-2, 4, 0), paletteV1Subchunk());
    // The 0x31 record contains raw little-endian Bedrock NBT. The parser must
    // retain only the command block's editor settings; it must not treat the
    // command text as a normal block-placement command during parsing.
    appendPut(&batch, overworldBlockEntityKey(-2, 3),
              commandBlockEntityNbt(-29, 16, 49));

    return levelDbFullRecord(batch);
}

std::vector<uint8_t> levelDbRestartBlock(const std::vector<uint8_t>& key,
                                         const std::vector<uint8_t>& value) {
    std::vector<uint8_t> block;
    appendVarint(&block, 0U);  // no shared prefix in this single-entry block
    appendVarint(&block, key.size());
    appendVarint(&block, value.size());
    block.insert(block.end(), key.begin(), key.end());
    block.insert(block.end(), value.begin(), value.end());
    appendLe32(&block, 0U);  // first restart offset
    appendLe32(&block, 1U);  // restart count
    return block;
}

std::vector<uint8_t> zlibWrappedDeflate(const std::vector<uint8_t>& source) {
    assert(source.size() <= static_cast<size_t>(std::numeric_limits<uLong>::max()));
    uLongf compressed_size = compressBound(static_cast<uLong>(source.size()));
    std::vector<uint8_t> compressed(static_cast<size_t>(compressed_size));
    const int status = compress2(reinterpret_cast<Bytef*>(compressed.data()), &compressed_size,
                                 reinterpret_cast<const Bytef*>(source.data()),
                                 static_cast<uLong>(source.size()), Z_BEST_COMPRESSION);
    assert(status == Z_OK);
    compressed.resize(static_cast<size_t>(compressed_size));
    return compressed;
}

std::vector<uint8_t> zlibCompressedSstable(const std::vector<uint8_t>& user_key,
                                            const std::vector<uint8_t>& value) {
    std::vector<uint8_t> internal_key = user_key;
    appendLe64(&internal_key, (7ULL << 8U) | 1ULL);  // sequence 7, LevelDB value record
    const std::vector<uint8_t> data_uncompressed = levelDbRestartBlock(internal_key, value);
    const std::vector<uint8_t> data_compressed = zlibWrappedDeflate(data_uncompressed);

    // LevelDB stores the masked CRC32C of [compression type | block bytes] in
    // every block trailer.  Keeping the synthetic fixture valid exercises the
    // same corruption checks as a real exported Bedrock database.
    const auto appendBlock = [](std::vector<uint8_t>* table, const std::vector<uint8_t>& bytes,
                                uint8_t compression) {
        table->insert(table->end(), bytes.begin(), bytes.end());
        appendU8(table, compression);
        std::vector<uint8_t> checksum_input;
        checksum_input.reserve(bytes.size() + 1U);
        appendU8(&checksum_input, compression);
        checksum_input.insert(checksum_input.end(), bytes.begin(), bytes.end());
        appendLe32(table, maskLevelDbCrc(crc32c(checksum_input.data(), checksum_input.size())));
    };

    std::vector<uint8_t> table;
    appendBlock(&table, data_compressed, 2U);  // zlib-wrapped DEFLATE

    const uint64_t index_offset = table.size();
    std::vector<uint8_t> data_handle;
    appendVarint(&data_handle, 0U);
    appendVarint(&data_handle, data_compressed.size());
    const std::vector<uint8_t> index_block = levelDbRestartBlock(
        std::vector<uint8_t>{static_cast<uint8_t>('i')}, data_handle);
    appendBlock(&table, index_block, 0U);  // uncompressed index block

    std::vector<uint8_t> footer;
    appendVarint(&footer, 0U);  // ignored metaindex handle offset
    appendVarint(&footer, 0U);  // ignored metaindex handle size
    appendVarint(&footer, index_offset);
    appendVarint(&footer, index_block.size());
    footer.resize(40U, 0U);
    appendLe64(&footer, 0xDB4775248B80FB57ULL);
    assert(footer.size() == 48U);
    table.insert(table.end(), footer.begin(), footer.end());
    return table;
}

void appendManifestNewFile(std::vector<uint8_t>* edit, uint32_t level, uint64_t number,
                           uint64_t file_size) {
    appendVarint(edit, 7U);  // VersionEdit::kNewFile
    appendVarint(edit, level);
    appendVarint(edit, number);
    appendVarint(edit, file_size);
    appendVarint(edit, 0U);  // smallest internal key (not needed for selection)
    appendVarint(edit, 0U);  // largest internal key
}

void appendManifestDeleteFile(std::vector<uint8_t>* edit, uint32_t level, uint64_t number) {
    appendVarint(edit, 6U);  // VersionEdit::kDeletedFile
    appendVarint(edit, level);
    appendVarint(edit, number);
}

std::vector<uint8_t> manifestWithStaleEntries(uint64_t stale_size, uint64_t live_size,
                                              uint64_t second_live_size) {
    // Two VersionEdits make the selection rule explicit: table 5 is first
    // live, then compacted away; table 7 is introduced by the later edit. A
    // real CURRENT/MANIFEST must also retain the recovery metadata fields.
    std::vector<uint8_t> first_edit;
    appendVarint(&first_edit, 2U);  // log number
    appendVarint(&first_edit, 12U);
    appendVarint(&first_edit, 3U);  // next file number
    appendVarint(&first_edit, 15U);
    appendVarint(&first_edit, 4U);  // last sequence
    appendVarint(&first_edit, 64U);
    appendManifestNewFile(&first_edit, 0U, 5U, stale_size);
    appendManifestNewFile(&first_edit, 0U, 6U, live_size);

    std::vector<uint8_t> second_edit;
    appendManifestDeleteFile(&second_edit, 0U, 5U);
    appendManifestNewFile(&second_edit, 0U, 7U, second_live_size);
    appendVarint(&second_edit, 9U);  // previous log number
    appendVarint(&second_edit, 12U);
    appendVarint(&second_edit, 2U);  // current log number
    appendVarint(&second_edit, 13U);

    std::vector<uint8_t> manifest = levelDbFullRecord(first_edit);
    const std::vector<uint8_t> second_record = levelDbFullRecord(second_edit);
    manifest.insert(manifest.end(), second_record.begin(), second_record.end());
    return manifest;
}

bool parseFile(const fs::path& source, ParsedFixture* fixture) {
    SchematicParseOptions options;
    options.source_path = source.string();
    options.base_x = 100;
    options.base_y = 64;
    options.base_z = -50;
    options.chunk_size = 32;
    options.block_sink = [&fixture](const ParsedBlock& block, std::string*) {
        fixture->blocks.push_back(block);
        return true;
    };
    options.command_block_sink = [&fixture](const CommandBlockRecord& record, std::string*) {
        fixture->command_blocks.push_back(record);
        return true;
    };
    return McworldParser().parse(options, BlockMapper(), &fixture->result, &fixture->error);
}

const ParsedBlock& blockAt(const std::vector<ParsedBlock>& blocks, int32_t x, int32_t y,
                           int32_t z) {
    const auto found = std::find_if(blocks.begin(), blocks.end(), [=](const ParsedBlock& block) {
        return block.world_x == x && block.world_y == y && block.world_z == z;
    });
    assert(found != blocks.end());
    return *found;
}

void assertMappedFixture(const ParsedFixture& fixture) {
    assert(fixture.error.empty());
    assert(fixture.result.imported_block_count == 9U);
    assert(fixture.result.unsupported_block_count == 0U);
    assert(fixture.result.command_block_payload_count == 1U);
    assert(fixture.result.omitted_command_block_data_count == 0U);
    assert(fixture.blocks.size() == 9U);
    assert(fixture.command_blocks.size() == 1U);
    assert(fixture.result.chunks.empty());

    // .mcworld contains absolute world chunk positions. Import treats the
    // lowest occupied source coordinate as the requested paste origin, so a
    // copied world never shifts by its original negative chunk coordinates.
    const ParsedBlock& legacy_stone = blockAt(fixture.blocks, 100, 64, -48);
    assert(legacy_stone.spec.command_name == "minecraft:stone");
    assert(legacy_stone.spec.aux == 0U);
    const ParsedBlock& legacy_wool = blockAt(fixture.blocks, 104, 64, -47);
    assert(legacy_wool.spec.command_name == "minecraft:wool");
    assert(legacy_wool.spec.aux == 14U);
    const ParsedBlock& legacy_v2_gold = blockAt(fixture.blocks, 123, 71, -49);
    assert(legacy_v2_gold.spec.command_name == "minecraft:gold_block");
    const ParsedBlock& palette_v1_iron = blockAt(fixture.blocks, 112, 68, -26);
    assert(palette_v1_iron.spec.command_name == "minecraft:iron_block");
    const ParsedBlock& palette_stone = blockAt(fixture.blocks, 101, 78, -50);
    assert(palette_stone.spec.command_name == "minecraft:stone");
    const ParsedBlock& command_shell = blockAt(fixture.blocks, 102, 78, -50);
    assert(command_shell.spec.command_name == "minecraft:command_block");
    assert(command_shell.spec.stateful);
    assert(!command_shell.spec.can_fill);
    const CommandBlockRecord& command_data = fixture.command_blocks.front();
    assert(command_data.x == 102 && command_data.y == 78 && command_data.z == -50);
    assert(command_data.mode == kCommandBlockModeImpulse);
    assert(command_data.redstone_mode && command_data.output_tracked);
    assert(command_data.tick_delay == 3);
    assert(command_data.command == "say mcworld command");
    assert(command_data.name == "mcworld command");
    const ParsedBlock& container_shell = blockAt(fixture.blocks, 103, 78, -50);
    assert(container_shell.spec.command_name == "minecraft:chest");
    const ParsedBlock& modern_stair = blockAt(fixture.blocks, 105, 94, -50);
    assert(modern_stair.spec.command_name == "minecraft:oak_stairs");
    assert(modern_stair.spec.aux == 5U);
    assert(modern_stair.spec.phase == ImportPhase::Structure);
    const ParsedBlock& three_bit_gold = blockAt(fixture.blocks, 106, 110, -50);
    assert(three_bit_gold.spec.command_name == "minecraft:gold_block");

    for (const ParsedBlock& block : fixture.blocks) {
        assert(block.spec.command_name.find("mcworld command") == std::string::npos);
    }

    const BlockBounds& bounds = fixture.result.source_volume_bounds;
    assert(bounds.min_x == 100 && bounds.min_y == 64 && bounds.min_z == -50);
    assert(bounds.max_x == 123 && bounds.max_y == 110 && bounds.max_z == -26);
}

void testStoredAndDeflatedZipAndLevelDbWal(const ScopedTempDirectory& temporary) {
    // Explicitly pin a non-symmetric coordinate. A Java/Sponge XZY encoder
    // would produce 0x321 here; Bedrock must remain YZX (0x132).
    assert(blockIndex(1, 2, 3) == 0x132U);
    const std::vector<ZipEntry> entries = {
        {"db/000001.log", levelDbWal()},
        {"levelname.txt", {'m', 'c', 'w', 'o', 'r', 'l', 'd'}},
    };

    const fs::path stored_source = temporary.child("stored.mcworld");
    writeMcworld(stored_source, entries, false);
    ParsedFixture stored;
    assert(parseFile(stored_source, &stored));
    assertMappedFixture(stored);

    const fs::path deflated_source = temporary.child("deflated.mcworld");
    writeMcworld(deflated_source, entries, true);
    ParsedFixture deflated;
    assert(parseFile(deflated_source, &deflated));
    assertMappedFixture(deflated);
}

void testZlibCompressedSstableAndVersionOnePalette(const ScopedTempDirectory& temporary) {
    const fs::path source = temporary.child("zlib_table_v1.mcworld");
    const std::vector<uint8_t> user_key = overworldSubchunkKey(-3, 5, 0);
    writeMcworld(source, {{"db/000001.ldb", zlibCompressedSstable(user_key, paletteV1Subchunk())}},
                 false);

    ParsedFixture fixture;
    assert(parseFile(source, &fixture));
    assert(fixture.error.empty());
    assert(fixture.result.imported_block_count == 1U);
    assert(fixture.blocks.size() == 1U);
    const ParsedBlock& iron = blockAt(fixture.blocks, 100, 64, -50);
    assert(iron.spec.command_name == "minecraft:iron_block");
    assert(fixture.result.source_offset_x == -35);
    assert(fixture.result.source_offset_y == 6);
    assert(fixture.result.source_offset_z == 89);
}

void testNormalStoneSlabsUseCanonicalSmoothStoneMapping(const ScopedTempDirectory& temporary) {
    const fs::path source = temporary.child("normal_stone_slabs.mcworld");
    writeMcworld(source, {{"db/000001.log", levelDbWal(
        1U, {{overworldSubchunkKey(-2, 3, 0), paletteV9NormalStoneSlabSubchunk(), false}})}},
        false);

    ParsedFixture fixture;
    assert(parseFile(source, &fixture));
    assert(fixture.error.empty());
    assert(fixture.result.imported_block_count == 2U);
    assert(fixture.blocks.size() == 2U);

    const ParsedBlock& upper = blockAt(fixture.blocks, 100, 64, -50);
    assert(upper.spec.command_name == "minecraft:stone_slab");
    assert(upper.spec.aux == 8U);
    assert(upper.spec.can_fill);

    const ParsedBlock& doubled = blockAt(fixture.blocks, 101, 64, -50);
    assert(doubled.spec.command_name == "minecraft:double_stone_slab");
    assert(doubled.spec.aux == 0U);
    assert(doubled.spec.can_fill);
}

void testManifestChoosesOnlyLiveTablesAndEligibleWals(const ScopedTempDirectory& temporary) {
    // The archive deliberately contains stale LevelDB files. Reading every
    // .ldb/.log ZIP member would resurrect removed chunks or move the import
    // origin; only CURRENT -> MANIFEST-selected tables and eligible WALs may
    // contribute records.
    const std::vector<uint8_t> stale_table = zlibCompressedSstable(
        overworldSubchunkKey(-5, 0, 0), paletteV1Subchunk());
    const std::vector<uint8_t> tombstoned_live_table = zlibCompressedSstable(
        overworldSubchunkKey(0, 0, 0), paletteV1Subchunk());
    const std::vector<uint8_t> second_live_table = zlibCompressedSstable(
        overworldSubchunkKey(1, 0, 0), paletteV1Subchunk());
    const std::vector<uint8_t> unlisted_table = zlibCompressedSstable(
        overworldSubchunkKey(2, 0, 0), paletteV1Subchunk());

    const std::vector<uint8_t> stale_wal = levelDbWal(
        100U, {{overworldSubchunkKey(3, 0, 0), paletteV1Subchunk(), false}});
    const std::vector<uint8_t> previous_wal = levelDbWal(
        200U, {{overworldSubchunkKey(4, 0, 0), paletteV1Subchunk(), false}});
    const std::vector<uint8_t> current_wal = levelDbWal(
        300U, {{overworldSubchunkKey(0, 0, 0), {}, true}});
    const std::vector<uint8_t> newer_wal = levelDbWal(
        400U, {{overworldSubchunkKey(5, 0, 0), paletteV1Subchunk(), false}});
    const std::vector<uint8_t> manifest = manifestWithStaleEntries(
        stale_table.size(), tombstoned_live_table.size(), second_live_table.size());
    const std::string current = "MANIFEST-000001\n";

    const fs::path source = temporary.child("manifest_selection.mcworld");
    writeMcworld(source, {
        {"db/CURRENT", std::vector<uint8_t>(current.begin(), current.end())},
        {"db/MANIFEST-000001", manifest},
        {"db/000005.ldb", stale_table},
        {"db/000006.ldb", tombstoned_live_table},
        {"db/000007.sst", second_live_table},
        {"db/000008.ldb", unlisted_table},
        {"db/000011.log", stale_wal},
        {"db/000012.log", previous_wal},
        {"db/000013.log", current_wal},
        {"db/000014.log", newer_wal},
    }, false);

    ParsedFixture fixture;
    assert(parseFile(source, &fixture));
    assert(fixture.error.empty());
    // Exactly three records survive: live table 7, previous log 12 and newer
    // log 14. Table 6 is selected but its key is correctly removed by log 13.
    assert(fixture.result.imported_block_count == 3U);
    assert(fixture.blocks.size() == 3U);
    assert(fixture.result.source_offset_x == 29);
    assert(fixture.result.source_offset_y == 6);
    assert(fixture.result.source_offset_z == 9);
    for (const int32_t x : {100, 148, 164}) {
        const ParsedBlock& block = blockAt(fixture.blocks, x, 64, -50);
        assert(block.spec.command_name == "minecraft:iron_block");
    }
}

void testInvalidArchiveAndTruncatedWalAreRejected(const ScopedTempDirectory& temporary) {
    const fs::path bad_archive = temporary.child("not_a_zip.mcworld");
    writeBytes(bad_archive, {0x50U, 0x4bU, 0x03U, 0x04U, 0x00U});
    ParsedFixture invalid_archive;
    assert(!parseFile(bad_archive, &invalid_archive));
    assert(!invalid_archive.error.empty());

    std::vector<uint8_t> truncated_wal = levelDbWal();
    assert(truncated_wal.size() > 32U);
    truncated_wal.resize(truncated_wal.size() - 17U);
    const fs::path truncated_source = temporary.child("truncated_wal.mcworld");
    writeMcworld(truncated_source, {{"db/000001.log", std::move(truncated_wal)}}, false);
    ParsedFixture truncated;
    assert(!parseFile(truncated_source, &truncated));
    assert(!truncated.error.empty());
}

int parseAndReportExternalFile(const char* path) {
    SchematicParseOptions options;
    options.source_path = path;
    options.chunk_size = 32;
    options.block_sink = [](const ParsedBlock&, std::string*) { return true; };

    SchematicParseResult result;
    std::string error;
    const bool parsed = McworldParser().parse(options, BlockMapper(), &result, &error);
    const BlockBounds& bounds = result.source_volume_bounds;
    std::printf(
        "%s|ok=%d|voxels=%llu|blocks=%llu|air=%llu|unsupported=%llu|degraded=%llu|"
        "bounds=%d,%d,%d:%d,%d,%d|raw_offset=%d,%d,%d|error=%s\n",
        path, parsed ? 1 : 0, static_cast<unsigned long long>(result.source_voxel_count),
        static_cast<unsigned long long>(result.imported_block_count),
        static_cast<unsigned long long>(result.skipped_block_count),
        static_cast<unsigned long long>(result.unsupported_block_count),
        static_cast<unsigned long long>(result.degraded_block_count), bounds.min_x, bounds.min_y,
        bounds.min_z, bounds.max_x, bounds.max_y, bounds.max_z, result.source_offset_x,
        result.source_offset_y, result.source_offset_z, error.c_str());
    return parsed ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2) return parseAndReportExternalFile(argv[1]);
    if (argc != 1) {
        std::fprintf(stderr, "usage: McworldParserTest [path-to-file.mcworld]\n");
        return 2;
    }
    const ScopedTempDirectory temporary;
    testStoredAndDeflatedZipAndLevelDbWal(temporary);
    testZlibCompressedSstableAndVersionOnePalette(temporary);
    testNormalStoneSlabsUseCanonicalSmoothStoneMapping(temporary);
    testManifestChoosesOnlyLiveTablesAndEligibleWals(temporary);
    testInvalidArchiveAndTruncatedWalAreRejected(temporary);
    return 0;
}
