#include "McworldParser.h"

#include "ChunkSpoolWriter.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace build_import {
namespace {

// The importer deliberately implements a small, read-only subset of the
// mcworld container and Mojang's LevelDB variant. It never extracts packs or
// opens the database for writing: only db/* table/WAL entries and the
// SubChunkPrefix records are considered. Behaviour packs, scripts, entities
// and inventories remain outside the import path; command-block settings are
// the sole block-entity exception and are written later through the native
// command-block update packet after their shell blocks are in place.
constexpr uint32_t kZipLocalHeaderSignature = 0x04034B50U;
constexpr uint32_t kZipCentralHeaderSignature = 0x02014B50U;
constexpr uint32_t kZipEndOfCentralDirectorySignature = 0x06054B50U;
constexpr uint64_t kLevelDbTableMagic = 0xDB4775248B80FB57ULL;
constexpr size_t kLevelDbFooterBytes = 48;
constexpr size_t kLevelDbBlockTrailerBytes = 5;
constexpr size_t kLevelDbLogBlockBytes = 32768;
// Classic ZIP's EOCD count is uint16_t. Keep a lower importer cap so a
// pathological archive cannot force tens of thousands of unrelated entries
// through the central-directory scanner (and avoid treating ZIP64's 0xffff
// sentinel as an ordinary count).
constexpr size_t kMaximumArchiveEntries = 32768;
constexpr uint64_t kMaximumZipEntryBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr size_t kMaximumTableBlockBytes = 64U * 1024U * 1024U;
constexpr size_t kMaximumPaletteEntries = 65536;
constexpr size_t kMaximumNbtDepth = 64;
constexpr uint64_t kMaximumDatabaseRecords = 8ULL * 1024ULL * 1024ULL;
constexpr uint64_t kCancellationInterval = 1024;

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool cancellationRequested(const SchematicParseOptions& options, std::string* error) {
    if (options.cancellation_requested && options.cancellation_requested()) {
        return fail(error, "import cancelled");
    }
    return false;
}

void reportProgress(const SchematicParseOptions& options, SchematicParseStage stage,
                    uint64_t completed, uint64_t total) {
    if (options.progress_callback) options.progress_callback({stage, completed, total});
}

uint16_t readLe16(const uint8_t* value) {
    return static_cast<uint16_t>(value[0]) |
           (static_cast<uint16_t>(value[1]) << 8U);
}

uint32_t readLe32(const uint8_t* value) {
    return static_cast<uint32_t>(value[0]) |
           (static_cast<uint32_t>(value[1]) << 8U) |
           (static_cast<uint32_t>(value[2]) << 16U) |
           (static_cast<uint32_t>(value[3]) << 24U);
}

uint64_t readLe64(const uint8_t* value) {
    uint64_t result = 0;
    for (size_t index = 0; index < 8; ++index) {
        result |= static_cast<uint64_t>(value[index]) << (index * 8U);
    }
    return result;
}

int32_t readSignedLe32(const uint8_t* value) {
    return static_cast<int32_t>(readLe32(value));
}

int32_t readSignedByte(uint8_t value) {
    return value < 0x80U ? static_cast<int32_t>(value)
                          : static_cast<int32_t>(value) - 0x100;
}

const std::array<uint32_t, 256>& crc32cTable() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t index = 0; index < result.size(); ++index) {
            uint32_t value = index;
            for (uint32_t bit = 0; bit < 8; ++bit) {
                value = (value & 1U) != 0U ? (value >> 1U) ^ 0x82F63B78U : value >> 1U;
            }
            result[index] = value;
        }
        return result;
    }();
    return table;
}

uint32_t crc32cExtend(uint32_t crc, const uint8_t* data, size_t size) {
    const auto& table = crc32cTable();
    for (size_t index = 0; index < size; ++index) {
        crc = table[(crc ^ data[index]) & 0xFFU] ^ (crc >> 8U);
    }
    return crc;
}

uint32_t unmaskLevelDbCrc32c(uint32_t value) {
    const uint32_t rotated = value - 0xA282EAD8U;
    return (rotated >> 17U) | (rotated << 15U);
}

bool verifyLevelDbRecordCrc(uint32_t masked_crc, uint8_t type,
                            const uint8_t* data, size_t size,
                            std::string_view record_name, std::string* error) {
    if (!data && size != 0) return fail(error, "mcworld LevelDB checksum input is unavailable");
    uint32_t crc = crc32cExtend(0xFFFFFFFFU, &type, 1);
    crc = crc32cExtend(crc, data, size);
    if (unmaskLevelDbCrc32c(masked_crc) == ~crc) return true;
    return fail(error, "mcworld LevelDB " + std::string(record_name) + " CRC mismatch");
}

bool checkedAdd(int32_t base, int64_t offset, int32_t* output, std::string* error) {
    const int64_t value = static_cast<int64_t>(base) + offset;
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
        return fail(error, "mcworld coordinates exceed the supported world range");
    }
    *output = static_cast<int32_t>(value);
    return true;
}

bool readVarint(const uint8_t* data, size_t size, size_t* cursor, uint64_t* value) {
    if (!data || !cursor || !value) return false;
    uint64_t result = 0;
    for (uint32_t shift = 0; shift < 64; shift += 7) {
        if (*cursor >= size) return false;
        const uint8_t byte = data[(*cursor)++];
        if (shift == 63 && (byte & 0xFEU) != 0) return false;
        result |= static_cast<uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0) {
            *value = result;
            return true;
        }
    }
    return false;
}

bool readVarint32(const uint8_t* data, size_t size, size_t* cursor, uint32_t* value) {
    uint64_t wide = 0;
    if (!readVarint(data, size, cursor, &wide) || wide > UINT32_MAX) return false;
    *value = static_cast<uint32_t>(wide);
    return true;
}

bool inflateRaw(const uint8_t* input, size_t input_size, size_t maximum_output,
                std::vector<uint8_t>* output, std::string* error,
                int window_bits = -MAX_WBITS) {
    if (!output) return fail(error, "mcworld deflate output is unavailable");
    output->clear();
    z_stream stream{};
    if (inflateInit2(&stream, window_bits) != Z_OK) {
        return fail(error, "cannot initialize mcworld deflate decoder");
    }
    std::array<uint8_t, 64 * 1024> buffer{};
    size_t supplied = 0;
    bool finished = false;
    while (!finished) {
        if (stream.avail_in == 0 && supplied < input_size) {
            const size_t take = std::min(input_size - supplied,
                                         static_cast<size_t>(std::numeric_limits<uInt>::max()));
            stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input + supplied));
            stream.avail_in = static_cast<uInt>(take);
            supplied += take;
        }
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        const int status = inflate(&stream, Z_NO_FLUSH);
        const size_t produced = buffer.size() - stream.avail_out;
        if (produced != 0) {
            if (produced > maximum_output || output->size() > maximum_output - produced) {
                inflateEnd(&stream);
                return fail(error, "mcworld compressed block exceeds the safety limit");
            }
            output->insert(output->end(), buffer.data(), buffer.data() + produced);
        }
        if (status == Z_STREAM_END) {
            finished = true;
        } else if (status != Z_OK) {
            inflateEnd(&stream);
            return fail(error, "cannot decode mcworld deflate block");
        } else if (produced == 0 && stream.avail_in == 0 && supplied == input_size) {
            inflateEnd(&stream);
            return fail(error, "truncated mcworld deflate block");
        }
    }
    inflateEnd(&stream);
    return true;
}

// LevelDB type 1 is raw Snappy (not the framed transport format).  Bedrock
// worlds normally use type 4 raw-deflate, but accepting Snappy keeps older
// exports readable without adding another native dependency.
bool decodeSnappy(const uint8_t* input, size_t input_size, size_t maximum_output,
                  std::vector<uint8_t>* output, std::string* error) {
    if (!output) return fail(error, "mcworld Snappy output is unavailable");
    size_t cursor = 0;
    uint64_t expected = 0;
    if (!readVarint(input, input_size, &cursor, &expected) || expected > maximum_output ||
        expected > std::numeric_limits<size_t>::max()) {
        return fail(error, "invalid mcworld Snappy block length");
    }
    output->clear();
    output->reserve(static_cast<size_t>(expected));
    const auto appendCopy = [&](size_t offset, size_t length) -> bool {
        if (offset == 0 || offset > output->size() || length > expected - output->size()) {
            return false;
        }
        const size_t source = output->size() - offset;
        for (size_t index = 0; index < length; ++index) output->push_back((*output)[source + index]);
        return true;
    };
    while (cursor < input_size && output->size() < expected) {
        const uint8_t tag = input[cursor++];
        const uint8_t type = tag & 0x03U;
        if (type == 0) {
            uint64_t length = tag >> 2U;
            if (length < 60) {
                ++length;
            } else {
                const uint32_t bytes = static_cast<uint32_t>(length - 59);
                if (bytes == 0 || bytes > 4 || cursor + bytes > input_size) {
                    return fail(error, "invalid mcworld Snappy literal");
                }
                length = 0;
                for (uint32_t index = 0; index < bytes; ++index) {
                    length |= static_cast<uint64_t>(input[cursor++]) << (index * 8U);
                }
                ++length;
            }
            if (length > expected - output->size() || length > input_size - cursor) {
                return fail(error, "truncated mcworld Snappy literal");
            }
            output->insert(output->end(), input + cursor, input + cursor + static_cast<size_t>(length));
            cursor += static_cast<size_t>(length);
            continue;
        }
        size_t length = 0;
        size_t offset = 0;
        if (type == 1) {
            if (cursor >= input_size) return fail(error, "truncated mcworld Snappy copy");
            length = 4 + ((tag >> 2U) & 0x07U);
            offset = (static_cast<size_t>(tag & 0xE0U) << 3U) | input[cursor++];
        } else if (type == 2) {
            if (cursor + 2 > input_size) return fail(error, "truncated mcworld Snappy copy");
            length = 1 + (tag >> 2U);
            offset = static_cast<size_t>(input[cursor]) |
                     (static_cast<size_t>(input[cursor + 1]) << 8U);
            cursor += 2;
        } else {
            if (cursor + 4 > input_size) return fail(error, "truncated mcworld Snappy copy");
            length = 1 + (tag >> 2U);
            offset = static_cast<size_t>(input[cursor]) |
                     (static_cast<size_t>(input[cursor + 1]) << 8U) |
                     (static_cast<size_t>(input[cursor + 2]) << 16U) |
                     (static_cast<size_t>(input[cursor + 3]) << 24U);
            cursor += 4;
        }
        if (!appendCopy(offset, length)) return fail(error, "invalid mcworld Snappy copy");
    }
    if (cursor != input_size || output->size() != expected) {
        return fail(error, "mcworld Snappy block length mismatch");
    }
    return true;
}

struct ZipEntry {
    std::string name;
    uint16_t flags = 0;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint64_t compressed_size = 0;
    uint64_t uncompressed_size = 0;
    uint64_t local_header_offset = 0;
};

class ZipArchive {
public:
    bool open(const std::string& path, std::string* error) {
        path_ = path;
        input_.open(path, std::ios::binary);
        if (!input_) return fail(error, "cannot open mcworld archive");
        input_.seekg(0, std::ios::end);
        const std::streamoff end = input_.tellg();
        if (end < 22) return fail(error, "mcworld archive is truncated");
        if (end < 0) return fail(error, "cannot determine mcworld archive size");
        file_size_ = static_cast<uint64_t>(end);
        const size_t tail_size = static_cast<size_t>(std::min<uint64_t>(file_size_, 0xFFFFU + 22U));
        std::vector<uint8_t> tail(tail_size);
        input_.seekg(static_cast<std::streamoff>(file_size_ - tail_size), std::ios::beg);
        input_.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
        if (input_.gcount() != static_cast<std::streamsize>(tail.size())) {
            return fail(error, "cannot read mcworld archive directory");
        }
        size_t end_offset = tail.size() - 22;
        bool found = false;
        do {
            if (readLe32(tail.data() + end_offset) == kZipEndOfCentralDirectorySignature) {
                found = true;
                break;
            }
        } while (end_offset-- != 0);
        if (!found) return fail(error, "mcworld archive has no ZIP central directory");
        const uint16_t disk = readLe16(tail.data() + end_offset + 4);
        const uint16_t directory_disk = readLe16(tail.data() + end_offset + 6);
        const uint16_t entry_count = readLe16(tail.data() + end_offset + 10);
        const uint32_t directory_size = readLe32(tail.data() + end_offset + 12);
        const uint32_t directory_offset = readLe32(tail.data() + end_offset + 16);
        if (disk != 0 || directory_disk != 0 || entry_count == UINT16_MAX ||
            directory_size == UINT32_MAX || directory_offset == UINT32_MAX) {
            return fail(error, "ZIP64 or multi-disk mcworld archives are not supported");
        }
        if (entry_count > kMaximumArchiveEntries ||
            static_cast<uint64_t>(directory_offset) + directory_size > file_size_) {
            return fail(error, "invalid mcworld ZIP central directory");
        }
        std::vector<uint8_t> directory(directory_size);
        input_.seekg(directory_offset, std::ios::beg);
        input_.read(reinterpret_cast<char*>(directory.data()), static_cast<std::streamsize>(directory.size()));
        if (input_.gcount() != static_cast<std::streamsize>(directory.size())) {
            return fail(error, "cannot read mcworld ZIP central directory");
        }
        entries_.clear();
        size_t cursor = 0;
        for (uint16_t index = 0; index < entry_count; ++index) {
            if (cursor + 46 > directory.size() ||
                readLe32(directory.data() + cursor) != kZipCentralHeaderSignature) {
                return fail(error, "invalid mcworld ZIP entry header");
            }
            const uint16_t flags = readLe16(directory.data() + cursor + 8);
            const uint16_t method = readLe16(directory.data() + cursor + 10);
            const uint32_t crc = readLe32(directory.data() + cursor + 16);
            const uint32_t compressed = readLe32(directory.data() + cursor + 20);
            const uint32_t uncompressed = readLe32(directory.data() + cursor + 24);
            const uint16_t name_length = readLe16(directory.data() + cursor + 28);
            const uint16_t extra_length = readLe16(directory.data() + cursor + 30);
            const uint16_t comment_length = readLe16(directory.data() + cursor + 32);
            const uint32_t local_offset = readLe32(directory.data() + cursor + 42);
            const uint64_t next = static_cast<uint64_t>(cursor) + 46U + name_length +
                                  extra_length + comment_length;
            if (next > directory.size() || compressed == UINT32_MAX ||
                uncompressed == UINT32_MAX || local_offset == UINT32_MAX) {
                return fail(error, "ZIP64 mcworld entry is not supported");
            }
            ZipEntry entry;
            entry.name.assign(reinterpret_cast<const char*>(directory.data() + cursor + 46), name_length);
            entry.flags = flags;
            entry.method = method;
            entry.crc = crc;
            entry.compressed_size = compressed;
            entry.uncompressed_size = uncompressed;
            entry.local_header_offset = local_offset;
            entries_.push_back(std::move(entry));
            cursor = static_cast<size_t>(next);
        }
        return true;
    }

    const std::vector<ZipEntry>& entries() const { return entries_; }

    bool read(const ZipEntry& entry, std::vector<uint8_t>* output,
              std::string* error) const {
        if (!output) return fail(error, "mcworld ZIP output is unavailable");
        if ((entry.flags & 0x0001U) != 0) return fail(error, "encrypted mcworld ZIP entries are not supported");
        if (entry.compressed_size > kMaximumZipEntryBytes ||
            entry.uncompressed_size > kMaximumZipEntryBytes ||
            entry.local_header_offset + 30 > file_size_) {
            return fail(error, "mcworld ZIP entry exceeds the safety limit");
        }
        input_.clear();
        input_.seekg(static_cast<std::streamoff>(entry.local_header_offset), std::ios::beg);
        std::array<uint8_t, 30> local{};
        input_.read(reinterpret_cast<char*>(local.data()), static_cast<std::streamsize>(local.size()));
        if (input_.gcount() != static_cast<std::streamsize>(local.size()) ||
            readLe32(local.data()) != kZipLocalHeaderSignature) {
            return fail(error, "invalid mcworld ZIP local entry header");
        }
        const uint16_t name_length = readLe16(local.data() + 26);
        const uint16_t extra_length = readLe16(local.data() + 28);
        const uint64_t data_offset = entry.local_header_offset + 30U + name_length + extra_length;
        if (data_offset > file_size_ || entry.compressed_size > file_size_ - data_offset ||
            entry.compressed_size > std::numeric_limits<size_t>::max()) {
            return fail(error, "truncated mcworld ZIP entry");
        }
        std::vector<uint8_t> compressed(static_cast<size_t>(entry.compressed_size));
        input_.seekg(static_cast<std::streamoff>(data_offset), std::ios::beg);
        input_.read(reinterpret_cast<char*>(compressed.data()),
                    static_cast<std::streamsize>(compressed.size()));
        if (input_.gcount() != static_cast<std::streamsize>(compressed.size())) {
            return fail(error, "cannot read mcworld ZIP entry");
        }
        if (entry.method == 0) {
            if (entry.compressed_size != entry.uncompressed_size) {
                return fail(error, "stored mcworld ZIP entry has mismatched sizes");
            }
            *output = std::move(compressed);
        } else if (entry.method == 8) {
            if (!inflateRaw(compressed.data(), compressed.size(),
                            static_cast<size_t>(entry.uncompressed_size), output, error)) return false;
            if (output->size() != entry.uncompressed_size) {
                return fail(error, "mcworld ZIP entry length mismatch");
            }
        } else {
            return fail(error, "mcworld ZIP entry uses an unsupported compression method");
        }
        const uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(output->data()),
                                static_cast<uInt>(std::min<size_t>(output->size(),
                                    static_cast<size_t>(std::numeric_limits<uInt>::max()))));
        // zlib's crc32 accepts uInt-sized increments.  Re-run the remainder for
        // unusually large entries rather than silently skipping integrity checks.
        uLong rolling = crc;
        size_t checked = std::min<size_t>(output->size(), static_cast<size_t>(std::numeric_limits<uInt>::max()));
        while (checked < output->size()) {
            const size_t take = std::min(output->size() - checked,
                                         static_cast<size_t>(std::numeric_limits<uInt>::max()));
            rolling = crc32(rolling, reinterpret_cast<const Bytef*>(output->data() + checked),
                            static_cast<uInt>(take));
            checked += take;
        }
        if (static_cast<uint32_t>(rolling) != entry.crc) {
            return fail(error, "mcworld ZIP entry CRC mismatch");
        }
        return true;
    }

private:
    std::string path_;
    mutable std::ifstream input_;
    uint64_t file_size_ = 0;
    std::vector<ZipEntry> entries_;
};

struct StoredRecord {
    uint64_t sequence = 0;
    uint64_t offset = 0;
    uint32_t length = 0;
    size_t memory_index = 0;
    bool deleted = false;
};

class RecordStore {
public:
    ~RecordStore() {
        close();
    }

    bool open(const SchematicParseOptions& options, std::string* error) {
        // Streaming tests and source-only consumers can legitimately route
        // through block_sink without a spool directory. Keep that mode wholly
        // in memory; command imports always get the disk-backed bounded path.
        memory_mode_ = options.block_sink && options.spool_directory.empty();
        if (memory_mode_) return true;
        if (options.spool_directory.empty()) {
            return fail(error, "mcworld parser requires spool_directory when block_sink is not set");
        }
        static std::atomic<uint64_t> sequence{0};
        const uint64_t token = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) ^ ++sequence;
        path_ = options.spool_directory;
        if (!path_.empty() && path_.back() != '/' && path_.back() != '\\') path_.push_back('/');
        path_ += "_mcworld_records_" + std::to_string(token) + ".bin";
        output_.open(path_, std::ios::binary | std::ios::trunc);
        if (!output_) return fail(error, "cannot create mcworld temporary record file");
        return true;
    }

    bool consider(const std::string& key, uint64_t sequence, uint8_t type,
                  const uint8_t* value, size_t value_size, std::string* error) {
        if (type != 0 && type != 1) return true;
        auto found = records_.find(key);
        if (found != records_.end() && found->second.sequence > sequence) return true;
        StoredRecord record;
        record.sequence = sequence;
        record.deleted = type == 0;
        if (!record.deleted) {
            if (value_size > UINT32_MAX || (!value && value_size != 0)) {
                return fail(error, "mcworld subchunk record is too large");
            }
            record.length = static_cast<uint32_t>(value_size);
            if (memory_mode_) {
                record.memory_index = memory_values_.size();
                memory_values_.emplace_back(value, value + value_size);
            } else {
                output_.seekp(0, std::ios::end);
                const std::streamoff position = output_.tellp();
                if (position < 0) return fail(error, "cannot position mcworld temporary record file");
                record.offset = static_cast<uint64_t>(position);
                output_.write(reinterpret_cast<const char*>(value),
                              static_cast<std::streamsize>(value_size));
                if (!output_) return fail(error, "cannot write mcworld temporary record file");
            }
        }
        records_[key] = record;
        return true;
    }

    bool finalize(std::string* error) {
        if (memory_mode_) return true;
        output_.flush();
        if (!output_) return fail(error, "cannot finalize mcworld temporary record file");
        output_.close();
        input_.open(path_, std::ios::binary);
        if (!input_) return fail(error, "cannot reopen mcworld temporary record file");
        return true;
    }

    const std::unordered_map<std::string, StoredRecord>& records() const { return records_; }

    bool read(const StoredRecord& record, std::vector<uint8_t>* value, std::string* error) {
        if (!value) return fail(error, "mcworld record output is unavailable");
        if (record.deleted) return fail(error, "attempted to read a deleted mcworld record");
        if (memory_mode_) {
            if (record.memory_index >= memory_values_.size()) {
                return fail(error, "invalid in-memory mcworld record reference");
            }
            *value = memory_values_[record.memory_index];
            return true;
        }
        input_.clear();
        input_.seekg(static_cast<std::streamoff>(record.offset), std::ios::beg);
        value->resize(record.length);
        input_.read(reinterpret_cast<char*>(value->data()), static_cast<std::streamsize>(value->size()));
        if (input_.gcount() != static_cast<std::streamsize>(value->size())) {
            return fail(error, "cannot read mcworld temporary record file");
        }
        return true;
    }

private:
    void close() {
        if (output_.is_open()) output_.close();
        if (input_.is_open()) input_.close();
        if (!path_.empty()) std::remove(path_.c_str());
    }

    bool memory_mode_ = false;
    std::string path_;
    std::ofstream output_;
    std::ifstream input_;
    std::unordered_map<std::string, StoredRecord> records_;
    std::vector<std::vector<uint8_t>> memory_values_;
};

struct SubchunkAddress {
    int32_t chunk_x = 0;
    int32_t chunk_z = 0;
    int32_t dimension = 0;
    int32_t section_y = 0;
};

enum class SubchunkKeyKind : uint8_t {
    Other,
    Overworld,
    OtherDimension,
};

struct BlockEntityAddress {
    int32_t chunk_x = 0;
    int32_t chunk_z = 0;
    int32_t dimension = 0;
};

enum class BlockEntityKeyKind : uint8_t {
    Other,
    Overworld,
    OtherDimension,
};

SubchunkKeyKind parseSubchunkKey(std::string_view key, SubchunkAddress* address) {
    if (!address) return SubchunkKeyKind::Other;
    const auto* bytes = reinterpret_cast<const uint8_t*>(key.data());
    if (key.size() == 10 && bytes[8] == 0x2FU) {
        address->chunk_x = readSignedLe32(bytes);
        address->chunk_z = readSignedLe32(bytes + 4);
        address->dimension = 0;
        address->section_y = readSignedByte(bytes[9]);
        return SubchunkKeyKind::Overworld;
    }
    if (key.size() == 14 && bytes[12] == 0x2FU) {
        address->chunk_x = readSignedLe32(bytes);
        address->chunk_z = readSignedLe32(bytes + 4);
        address->dimension = readSignedLe32(bytes + 8);
        address->section_y = readSignedByte(bytes[13]);
        return address->dimension == 0 ? SubchunkKeyKind::Overworld
                                       : SubchunkKeyKind::OtherDimension;
    }
    return SubchunkKeyKind::Other;
}

BlockEntityKeyKind parseBlockEntityKey(std::string_view key, BlockEntityAddress* address) {
    if (!address) return BlockEntityKeyKind::Other;
    const auto* bytes = reinterpret_cast<const uint8_t*>(key.data());
    if (key.size() == 9 && bytes[8] == 0x31U) {
        address->chunk_x = readSignedLe32(bytes);
        address->chunk_z = readSignedLe32(bytes + 4);
        address->dimension = 0;
        return BlockEntityKeyKind::Overworld;
    }
    if (key.size() == 13 && bytes[12] == 0x31U) {
        address->chunk_x = readSignedLe32(bytes);
        address->chunk_z = readSignedLe32(bytes + 4);
        address->dimension = readSignedLe32(bytes + 8);
        return address->dimension == 0 ? BlockEntityKeyKind::Overworld
                                        : BlockEntityKeyKind::OtherDimension;
    }
    return BlockEntityKeyKind::Other;
}

bool isDbEntryName(std::string_view name, std::string_view suffix) {
    return name.size() > suffix.size() && name.compare(0, 3, "db/") == 0 &&
           name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool decodeLevelDbBlock(const std::vector<uint8_t>& table, uint64_t offset, uint64_t size,
                        std::vector<uint8_t>* output, std::string* error) {
    if (!output || offset > table.size() || size > table.size() - offset ||
        size > table.size() - offset - (table.size() - offset >= kLevelDbBlockTrailerBytes
            ? kLevelDbBlockTrailerBytes : 0)) {
        return fail(error, "invalid mcworld LevelDB block handle");
    }
    if (offset + size + kLevelDbBlockTrailerBytes > table.size()) {
        return fail(error, "truncated mcworld LevelDB block");
    }
    const uint8_t compression = table[static_cast<size_t>(offset + size)];
    const uint8_t* compressed = table.data() + static_cast<size_t>(offset);
    const size_t compressed_size = static_cast<size_t>(size);
    // Bedrock's modified LevelDB table writer is not consistent about the
    // standard LevelDB CRC32C trailer (real exported worlds may leave a
    // non-standard value here).  Length/handle bounds and decompression are
    // still verified below; authoritative MANIFEST and WAL log records do
    // receive CRC32C validation because those streams are consistently framed.
    if (compression == 0) {
        if (compressed_size > kMaximumTableBlockBytes) {
            return fail(error, "mcworld LevelDB block exceeds the safety limit");
        }
        output->assign(compressed, compressed + compressed_size);
        return true;
    }
    if (compression == 1) return decodeSnappy(compressed, compressed_size,
                                               kMaximumTableBlockBytes, output, error);
    // Older MCPE/Bedrock LevelDB tables use zlib-wrapped DEFLATE (type 2),
    // while current worlds commonly write raw DEFLATE (type 4).  ZIP itself
    // also remains raw, so keep the two window modes explicit.
    if (compression == 2) return inflateRaw(compressed, compressed_size,
                                             kMaximumTableBlockBytes, output, error,
                                             MAX_WBITS);
    if (compression == 4) return inflateRaw(compressed, compressed_size,
                                             kMaximumTableBlockBytes, output, error);
    return fail(error, "mcworld LevelDB block uses an unsupported compression type " +
                       std::to_string(compression));
}

template <typename Callback>
bool forEachRestartBlockEntry(const std::vector<uint8_t>& block, Callback&& callback,
                              std::string* error) {
    if (block.size() < 4) return fail(error, "truncated mcworld LevelDB restart block");
    const uint32_t restart_count = readLe32(block.data() + block.size() - 4);
    const uint64_t restart_bytes = (static_cast<uint64_t>(restart_count) + 1U) * 4U;
    if (restart_bytes > block.size()) return fail(error, "invalid mcworld LevelDB restart table");
    const size_t end = block.size() - static_cast<size_t>(restart_bytes);
    std::string previous_key;
    size_t cursor = 0;
    while (cursor < end) {
        uint64_t shared = 0, unshared = 0, value_length = 0;
        if (!readVarint(block.data(), end, &cursor, &shared) ||
            !readVarint(block.data(), end, &cursor, &unshared) ||
            !readVarint(block.data(), end, &cursor, &value_length) ||
            shared > previous_key.size() || unshared > end - cursor ||
            value_length > end - cursor - static_cast<size_t>(unshared)) {
            return fail(error, "invalid mcworld LevelDB restart entry");
        }
        std::string key = previous_key.substr(0, static_cast<size_t>(shared));
        key.append(reinterpret_cast<const char*>(block.data() + cursor),
                   static_cast<size_t>(unshared));
        cursor += static_cast<size_t>(unshared);
        const std::string_view value(reinterpret_cast<const char*>(block.data() + cursor),
                                     static_cast<size_t>(value_length));
        cursor += static_cast<size_t>(value_length);
        if (!callback(std::string_view(key), value)) return false;
        previous_key.swap(key);
    }
    return cursor == end || fail(error, "invalid mcworld LevelDB restart block tail");
}

bool readBlockHandle(std::string_view bytes, uint64_t* offset, uint64_t* size,
                     std::string* error) {
    size_t cursor = 0;
    if (!offset || !size ||
        !readVarint(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &cursor, offset) ||
        !readVarint(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &cursor, size)) {
        return fail(error, "invalid mcworld LevelDB block handle");
    }
    return true;
}

template <typename Callback>
bool forEachSstableEntry(const std::vector<uint8_t>& table, Callback&& callback,
                         std::string* error) {
    if (table.size() < kLevelDbFooterBytes ||
        readLe64(table.data() + table.size() - 8) != kLevelDbTableMagic) {
        return fail(error, "invalid mcworld LevelDB table footer");
    }
    const uint8_t* footer = table.data() + table.size() - kLevelDbFooterBytes;
    size_t footer_cursor = 0;
    uint64_t ignored_meta_offset = 0, ignored_meta_size = 0, index_offset = 0, index_size = 0;
    if (!readVarint(footer, 40, &footer_cursor, &ignored_meta_offset) ||
        !readVarint(footer, 40, &footer_cursor, &ignored_meta_size) ||
        !readVarint(footer, 40, &footer_cursor, &index_offset) ||
        !readVarint(footer, 40, &footer_cursor, &index_size)) {
        return fail(error, "invalid mcworld LevelDB table footer handles");
    }
    std::vector<uint8_t> index_block;
    if (!decodeLevelDbBlock(table, index_offset, index_size, &index_block, error)) return false;
    return forEachRestartBlockEntry(index_block,
        [&](std::string_view, std::string_view handle) -> bool {
            uint64_t data_offset = 0, data_size = 0;
            if (!readBlockHandle(handle, &data_offset, &data_size, error)) return false;
            std::vector<uint8_t> data_block;
            if (!decodeLevelDbBlock(table, data_offset, data_size, &data_block, error)) return false;
            return forEachRestartBlockEntry(data_block, callback, error);
        }, error);
}

bool collectLevelDbEntry(std::string_view internal_key, std::string_view value,
                         RecordStore* store, uint64_t* processed_records,
                         uint64_t* skipped_dimension_records, std::string* error) {
    if (!store || internal_key.size() < 8) return true;
    if (processed_records && ++*processed_records > kMaximumDatabaseRecords) {
        return fail(error, "mcworld LevelDB contains too many records");
    }
    const size_t user_length = internal_key.size() - 8;
    const auto* trailer = reinterpret_cast<const uint8_t*>(internal_key.data() + user_length);
    const uint64_t tag = readLe64(trailer);
    const uint8_t type = static_cast<uint8_t>(tag & 0xFFU);
    if (type != 0 && type != 1) return true;
    const std::string_view user_key = internal_key.substr(0, user_length);
    SubchunkAddress ignored_subchunk;
    BlockEntityAddress ignored_block_entity;
    const SubchunkKeyKind subchunk_kind = parseSubchunkKey(user_key, &ignored_subchunk);
    const BlockEntityKeyKind block_entity_kind =
        parseBlockEntityKey(user_key, &ignored_block_entity);
    if (subchunk_kind == SubchunkKeyKind::OtherDimension ||
        block_entity_kind == BlockEntityKeyKind::OtherDimension) {
        if (skipped_dimension_records) ++*skipped_dimension_records;
        return true;
    }
    if (subchunk_kind != SubchunkKeyKind::Overworld &&
        block_entity_kind != BlockEntityKeyKind::Overworld) return true;
    return store->consider(std::string(user_key), tag >> 8U, type,
                           reinterpret_cast<const uint8_t*>(value.data()), value.size(), error);
}

bool collectWriteBatch(const uint8_t* batch, size_t size, RecordStore* store,
                       uint64_t* processed_records, uint64_t* skipped_dimension_records,
                       std::string* error) {
    if (!batch || size < 12) return fail(error, "truncated mcworld LevelDB write batch");
    const uint64_t sequence = readLe64(batch);
    const uint32_t count = readLe32(batch + 8);
    size_t cursor = 12;
    for (uint32_t index = 0; index < count; ++index) {
        if (cursor >= size || sequence > UINT64_MAX - index) {
            return fail(error, "invalid mcworld LevelDB write batch");
        }
        const uint8_t type = batch[cursor++];
        uint32_t key_length = 0;
        if (!readVarint32(batch, size, &cursor, &key_length) || key_length > size - cursor) {
            return fail(error, "truncated mcworld LevelDB write-batch key");
        }
        const std::string_view key(reinterpret_cast<const char*>(batch + cursor), key_length);
        cursor += key_length;
        std::string_view value;
        if (type == 1) {
            uint32_t value_length = 0;
            if (!readVarint32(batch, size, &cursor, &value_length) || value_length > size - cursor) {
                return fail(error, "truncated mcworld LevelDB write-batch value");
            }
            value = std::string_view(reinterpret_cast<const char*>(batch + cursor), value_length);
            cursor += value_length;
        } else if (type != 0) {
            return fail(error, "unsupported mcworld LevelDB write-batch record type");
        }
        SubchunkAddress ignored_subchunk;
        BlockEntityAddress ignored_block_entity;
        const SubchunkKeyKind subchunk_kind = parseSubchunkKey(key, &ignored_subchunk);
        const BlockEntityKeyKind block_entity_kind =
            parseBlockEntityKey(key, &ignored_block_entity);
        if (subchunk_kind == SubchunkKeyKind::OtherDimension ||
            block_entity_kind == BlockEntityKeyKind::OtherDimension) {
            if (skipped_dimension_records) ++*skipped_dimension_records;
            continue;
        }
        if ((subchunk_kind == SubchunkKeyKind::Overworld ||
             block_entity_kind == BlockEntityKeyKind::Overworld) &&
            !store->consider(std::string(key), sequence + index, type,
                              reinterpret_cast<const uint8_t*>(value.data()), value.size(), error)) {
            return false;
        }
        if (processed_records && ++*processed_records > kMaximumDatabaseRecords) {
            return fail(error, "mcworld LevelDB contains too many records");
        }
    }
    return cursor == size || fail(error, "mcworld LevelDB write batch has trailing data");
}

bool collectWalRecords(const std::vector<uint8_t>& log, RecordStore* store,
                       uint64_t* processed_records, uint64_t* skipped_dimension_records,
                       const SchematicParseOptions& options, std::string* error) {
    std::vector<uint8_t> fragment;
    size_t cursor = 0;
    uint64_t physical_records = 0;
    while (cursor < log.size()) {
        const size_t block_remaining = kLevelDbLogBlockBytes - (cursor % kLevelDbLogBlockBytes);
        if (block_remaining < 7) {
            cursor += block_remaining;
            continue;
        }
        if (cursor + 7 > log.size()) return fail(error, "truncated mcworld LevelDB WAL header");
        const uint32_t masked_crc = readLe32(log.data() + cursor);
        const uint16_t length = readLe16(log.data() + cursor + 4);
        const uint8_t type = log[cursor + 6];
        cursor += 7;
        if (length > block_remaining - 7 || length > log.size() - cursor) {
            return fail(error, "invalid mcworld LevelDB WAL record length");
        }
        const uint8_t* data = log.data() + cursor;
        cursor += length;
        if ((++physical_records % kCancellationInterval) == 0 &&
            cancellationRequested(options, error)) return false;
        if (type == 0 && length == 0) continue;
        if (!verifyLevelDbRecordCrc(masked_crc, type, data, length, "WAL record", error)) {
            return false;
        }
        if (type == 1) {
            if (!fragment.empty()) return fail(error, "invalid fragmented mcworld LevelDB WAL record");
            if (!collectWriteBatch(data, length, store, processed_records,
                                   skipped_dimension_records, error)) return false;
        } else if (type == 2) {
            fragment.assign(data, data + length);
        } else if (type == 3) {
            if (fragment.empty() || fragment.size() > kMaximumZipEntryBytes - length) {
                return fail(error, "invalid fragmented mcworld LevelDB WAL record");
            }
            fragment.insert(fragment.end(), data, data + length);
        } else if (type == 4) {
            if (fragment.empty() || fragment.size() > kMaximumZipEntryBytes - length) {
                return fail(error, "invalid fragmented mcworld LevelDB WAL record");
            }
            fragment.insert(fragment.end(), data, data + length);
            if (!collectWriteBatch(fragment.data(), fragment.size(), store, processed_records,
                                   skipped_dimension_records, error)) return false;
            fragment.clear();
        } else {
            return fail(error, "unsupported mcworld LevelDB WAL record type");
        }
    }
    return fragment.empty() || fail(error, "truncated fragmented mcworld LevelDB WAL record");
}

bool skipLengthPrefixedSlice(const uint8_t* data, size_t size, size_t* cursor) {
    if (!data || !cursor || *cursor > size) return false;
    uint32_t length = 0;
    if (!readVarint32(data, size, cursor, &length) || length > size - *cursor) return false;
    *cursor += length;
    return true;
}

struct ManifestTable {
    uint32_t level = 0;
    uint64_t file_size = 0;
};

struct ManifestState {
    std::unordered_map<uint64_t, ManifestTable> live_tables;
    bool has_log_number = false;
    uint64_t log_number = 0;
    bool has_prev_log_number = false;
    uint64_t prev_log_number = 0;
    bool has_next_file_number = false;
    uint64_t next_file_number = 0;
    bool has_last_sequence = false;
    uint64_t last_sequence = 0;
};

// VersionEdit tags are part of LevelDB's stable on-disk protocol.  A Bedrock
// mcworld is an archive of a live LevelDB directory, so selecting every .ldb
// file is not enough: the manifest tells us which tables have survived the
// latest compaction and which WALs are eligible for recovery.
bool applyManifestEdit(const uint8_t* edit, size_t size, ManifestState* state,
                       std::string* error) {
    if (!edit || !state) return fail(error, "mcworld manifest edit is unavailable");
    size_t cursor = 0;
    std::vector<uint64_t> deleted_tables;
    std::vector<std::pair<uint64_t, ManifestTable>> added_tables;
    while (cursor < size) {
        uint32_t tag = 0;
        if (!readVarint32(edit, size, &cursor, &tag)) {
            return fail(error, "truncated mcworld LevelDB manifest tag");
        }
        uint64_t number = 0;
        uint32_t level = 0;
        switch (tag) {
            case 1:  // comparator name
                if (!skipLengthPrefixedSlice(edit, size, &cursor)) {
                    return fail(error, "invalid mcworld manifest comparator");
                }
                break;
            case 2:  // log number
                if (!readVarint(edit, size, &cursor, &state->log_number)) {
                    return fail(error, "invalid mcworld manifest log number");
                }
                state->has_log_number = true;
                break;
            case 3:  // next file number
                if (!readVarint(edit, size, &cursor, &state->next_file_number)) {
                    return fail(error, "invalid mcworld manifest next file number");
                }
                state->has_next_file_number = true;
                break;
            case 4:  // last sequence number
                if (!readVarint(edit, size, &cursor, &state->last_sequence)) {
                    return fail(error, "invalid mcworld manifest last sequence number");
                }
                state->has_last_sequence = true;
                break;
            case 5:  // compact pointer: level + internal key
                if (!readVarint32(edit, size, &cursor, &level) ||
                    !skipLengthPrefixedSlice(edit, size, &cursor)) {
                    return fail(error, "invalid mcworld manifest compact pointer");
                }
                break;
            case 6:  // deleted file: level + file number
                if (!readVarint32(edit, size, &cursor, &level) ||
                    !readVarint(edit, size, &cursor, &number)) {
                    return fail(error, "invalid mcworld manifest deleted file");
                }
                deleted_tables.push_back(number);
                break;
            case 7: {  // new file: level + number + size + smallest/largest keys
                uint64_t file_size = 0;
                if (!readVarint32(edit, size, &cursor, &level) ||
                    !readVarint(edit, size, &cursor, &number) ||
                    !readVarint(edit, size, &cursor, &file_size) ||
                    !skipLengthPrefixedSlice(edit, size, &cursor) ||
                    !skipLengthPrefixedSlice(edit, size, &cursor)) {
                    return fail(error, "invalid mcworld manifest new-file entry");
                }
                added_tables.emplace_back(number, ManifestTable{level, file_size});
                break;
            }
            case 9:  // previous log number
                if (!readVarint(edit, size, &cursor, &state->prev_log_number)) {
                    return fail(error, "invalid mcworld manifest previous log number");
                }
                state->has_prev_log_number = true;
                break;
            default:
                // Unknown VersionEdit fields cannot be skipped safely because
                // their payload encoding is tag-specific.  Failing here avoids
                // importing a stale or incomplete world view.
                return fail(error, "unsupported mcworld LevelDB manifest tag " +
                                   std::to_string(tag));
        }
    }
    // VersionSet::Builder applies all DeletedFile entries before all NewFile
    // entries in an edit. Preserve that semantic even if a third-party writer
    // serializes the individual tags in a different order.
    for (const uint64_t number : deleted_tables) state->live_tables.erase(number);
    for (const auto& added : added_tables) {
        if (state->live_tables.find(added.first) == state->live_tables.end() &&
            state->live_tables.size() >= kMaximumArchiveEntries) {
            return fail(error, "mcworld manifest contains too many live tables");
        }
        state->live_tables[added.first] = added.second;
    }
    return true;
}

bool collectManifestRecords(const std::vector<uint8_t>& manifest, ManifestState* state,
                            const SchematicParseOptions& options, std::string* error) {
    if (!state) return fail(error, "mcworld manifest state is unavailable");
    std::vector<uint8_t> fragment;
    size_t cursor = 0;
    uint64_t physical_records = 0;
    const auto apply = [&](const uint8_t* data, size_t length) {
        return applyManifestEdit(data, length, state, error);
    };
    while (cursor < manifest.size()) {
        const size_t block_remaining = kLevelDbLogBlockBytes - (cursor % kLevelDbLogBlockBytes);
        if (block_remaining < 7) {
            cursor += block_remaining;
            continue;
        }
        if (cursor + 7 > manifest.size()) {
            return fail(error, "truncated mcworld LevelDB manifest header");
        }
        const uint32_t masked_crc = readLe32(manifest.data() + cursor);
        const uint16_t length = readLe16(manifest.data() + cursor + 4);
        const uint8_t type = manifest[cursor + 6];
        cursor += 7;
        if (length > block_remaining - 7 || length > manifest.size() - cursor) {
            return fail(error, "invalid mcworld LevelDB manifest record length");
        }
        const uint8_t* data = manifest.data() + cursor;
        cursor += length;
        if ((++physical_records % kCancellationInterval) == 0 &&
            cancellationRequested(options, error)) return false;
        if (type == 0 && length == 0) continue;
        if (!verifyLevelDbRecordCrc(masked_crc, type, data, length, "manifest record", error)) {
            return false;
        }
        if (type == 1) {
            if (!fragment.empty() || !apply(data, length)) {
                return fragment.empty() ? false
                    : fail(error, "invalid fragmented mcworld LevelDB manifest record");
            }
        } else if (type == 2) {
            if (!fragment.empty()) return fail(error, "invalid fragmented mcworld LevelDB manifest record");
            fragment.assign(data, data + length);
        } else if (type == 3) {
            if (fragment.empty() || fragment.size() > kMaximumZipEntryBytes - length) {
                return fail(error, "invalid fragmented mcworld LevelDB manifest record");
            }
            fragment.insert(fragment.end(), data, data + length);
        } else if (type == 4) {
            if (fragment.empty() || fragment.size() > kMaximumZipEntryBytes - length) {
                return fail(error, "invalid fragmented mcworld LevelDB manifest record");
            }
            fragment.insert(fragment.end(), data, data + length);
            if (!apply(fragment.data(), fragment.size())) return false;
            fragment.clear();
        } else {
            return fail(error, "unsupported mcworld LevelDB manifest record type");
        }
    }
    return fragment.empty() || fail(error, "truncated fragmented mcworld LevelDB manifest record");
}

bool parseDbNumberedName(std::string_view name, std::string_view suffix, uint64_t* number) {
    if (!number || !isDbEntryName(name, suffix)) return false;
    const size_t digits_begin = 3;
    const size_t digits_length = name.size() - digits_begin - suffix.size();
    if (digits_length == 0 || digits_length > 20) return false;
    uint64_t parsed = 0;
    for (size_t index = 0; index < digits_length; ++index) {
        const unsigned char digit = static_cast<unsigned char>(name[digits_begin + index]);
        if (digit < '0' || digit > '9' ||
            parsed > (UINT64_MAX - static_cast<uint64_t>(digit - '0')) / 10U) {
            return false;
        }
        parsed = parsed * 10U + static_cast<uint64_t>(digit - '0');
    }
    *number = parsed;
    return true;
}

bool loadManifestState(const ZipArchive& archive, const SchematicParseOptions& options,
                       ManifestState* state, bool* found, std::string* error) {
    if (!state || !found) return fail(error, "mcworld manifest output is unavailable");
    *state = {};
    *found = false;
    const ZipEntry* current = nullptr;
    for (const ZipEntry& entry : archive.entries()) {
        if (entry.name != "db/CURRENT") continue;
        if (current) return fail(error, "mcworld archive contains duplicate db/CURRENT entries");
        current = &entry;
    }
    // Some lightweight exporters omit CURRENT/MANIFEST altogether.  Preserve
    // the old best-effort scan for those packages, but never ignore a present
    // manifest because it is authoritative for a normal LevelDB directory.
    if (!current) return true;
    std::vector<uint8_t> current_bytes;
    if (!archive.read(*current, &current_bytes, error) || current_bytes.empty() ||
        current_bytes.size() > 256) {
        return error && !error->empty() ? false : fail(error, "invalid mcworld db/CURRENT file");
    }
    std::string manifest_name(current_bytes.begin(), current_bytes.end());
    while (!manifest_name.empty() &&
           (manifest_name.back() == '\n' || manifest_name.back() == '\r')) {
        manifest_name.pop_back();
    }
    constexpr std::string_view kManifestPrefix = "MANIFEST-";
    if (manifest_name.size() <= kManifestPrefix.size() ||
        manifest_name.compare(0, kManifestPrefix.size(), kManifestPrefix) != 0) {
        return fail(error, "invalid mcworld db/CURRENT manifest name");
    }
    for (size_t index = kManifestPrefix.size(); index < manifest_name.size(); ++index) {
        if (manifest_name[index] < '0' || manifest_name[index] > '9') {
            return fail(error, "invalid mcworld db/CURRENT manifest name");
        }
    }
    const std::string archive_name = "db/" + manifest_name;
    const ZipEntry* manifest_entry = nullptr;
    for (const ZipEntry& entry : archive.entries()) {
        if (entry.name != archive_name) continue;
        if (manifest_entry) return fail(error, "mcworld archive contains duplicate manifest entries");
        manifest_entry = &entry;
    }
    if (!manifest_entry) return fail(error, "mcworld db/CURRENT references a missing manifest");
    std::vector<uint8_t> manifest;
    if (!archive.read(*manifest_entry, &manifest, error) ||
        !collectManifestRecords(manifest, state, options, error)) return false;
    if (!state->has_log_number || !state->has_next_file_number || !state->has_last_sequence) {
        return fail(error, "mcworld LevelDB manifest is missing required recovery metadata");
    }
    *found = true;
    return true;
}

bool collectArchiveRecords(const ZipArchive& archive, RecordStore* store,
                           const SchematicParseOptions& options,
                           uint64_t* skipped_dimension_records, std::string* error) {
    if (!store) return fail(error, "mcworld record store is unavailable");
    ManifestState manifest;
    bool has_manifest = false;
    if (!loadManifestState(archive, options, &manifest, &has_manifest, error)) return false;
    std::vector<const ZipEntry*> table_candidates;
    std::vector<const ZipEntry*> log_candidates;
    for (const ZipEntry& entry : archive.entries()) {
        const bool table = isDbEntryName(entry.name, ".ldb") ||
                           isDbEntryName(entry.name, ".sst");
        if (table && entry.uncompressed_size != 0) {
            table_candidates.push_back(&entry);
        } else if (isDbEntryName(entry.name, ".log") && entry.uncompressed_size != 0) {
            log_candidates.push_back(&entry);
        }
    }

    std::vector<const ZipEntry*> tables;
    std::vector<const ZipEntry*> logs;
    if (has_manifest) {
        std::unordered_set<uint64_t> selected_table_numbers;
        for (const ZipEntry* entry : table_candidates) {
            uint64_t number = 0;
            const std::string_view suffix = isDbEntryName(entry->name, ".ldb") ? ".ldb" : ".sst";
            if (!parseDbNumberedName(entry->name, suffix, &number)) {
                return fail(error, "invalid numbered mcworld LevelDB table name");
            }
            const auto live = manifest.live_tables.find(number);
            if (live == manifest.live_tables.end()) continue;
            if (entry->uncompressed_size != live->second.file_size) {
                return fail(error, "mcworld LevelDB table size does not match its manifest metadata");
            }
            if (!selected_table_numbers.insert(number).second) {
                return fail(error, "mcworld manifest selects duplicate LevelDB table numbers");
            }
            tables.push_back(entry);
        }
        for (const auto& live : manifest.live_tables) {
            if (selected_table_numbers.find(live.first) == selected_table_numbers.end()) {
                return fail(error, "mcworld manifest references a missing LevelDB table");
            }
        }
        for (const ZipEntry* entry : log_candidates) {
            uint64_t number = 0;
            if (!parseDbNumberedName(entry->name, ".log", &number)) {
                return fail(error, "invalid numbered mcworld LevelDB WAL name");
            }
            if (!manifest.has_log_number || number >= manifest.log_number ||
                (manifest.has_prev_log_number && number == manifest.prev_log_number)) {
                logs.push_back(entry);
            }
        }
    } else {
        tables = std::move(table_candidates);
        logs = std::move(log_candidates);
    }

    uint64_t total_bytes = 0;
    for (const ZipEntry* entry : tables) total_bytes += entry->uncompressed_size;
    for (const ZipEntry* entry : logs) total_bytes += entry->uncompressed_size;
    if (tables.empty() && logs.empty()) {
        return fail(error, has_manifest
            ? "mcworld manifest selects no readable LevelDB table or WAL entries"
            : "mcworld archive contains no LevelDB table or WAL entries");
    }
    const auto numbered_before = [](const ZipEntry* left, const ZipEntry* right) {
        const auto parse = [](const ZipEntry* entry, uint64_t* number) {
            if (isDbEntryName(entry->name, ".log")) {
                return parseDbNumberedName(entry->name, ".log", number);
            }
            if (isDbEntryName(entry->name, ".ldb")) {
                return parseDbNumberedName(entry->name, ".ldb", number);
            }
            return parseDbNumberedName(entry->name, ".sst", number);
        };
        uint64_t left_number = 0;
        uint64_t right_number = 0;
        const bool left_numbered = parse(left, &left_number);
        const bool right_numbered = parse(right, &right_number);
        if (left_numbered && right_numbered && left_number != right_number) {
            return left_number < right_number;
        }
        if (left_numbered != right_numbered) return left_numbered;
        return left->name < right->name;
    };
    std::sort(tables.begin(), tables.end(), numbered_before);
    std::sort(logs.begin(), logs.end(), numbered_before);
    uint64_t completed_bytes = 0;
    uint64_t processed_records = 0;
    reportProgress(options, SchematicParseStage::ReadingSource, 0, total_bytes);
    const auto read_table = [&](const ZipEntry& entry) -> bool {
        if (cancellationRequested(options, error)) return false;
        std::vector<uint8_t> table;
        if (!archive.read(entry, &table, error)) return false;
        uint64_t entries = 0;
        if (!forEachSstableEntry(table,
            [&](std::string_view key, std::string_view value) -> bool {
                if ((++entries % kCancellationInterval) == 0 &&
                    cancellationRequested(options, error)) return false;
                return collectLevelDbEntry(key, value, store, &processed_records,
                                           skipped_dimension_records, error);
            }, error)) return false;
        completed_bytes += entry.uncompressed_size;
        reportProgress(options, SchematicParseStage::ReadingSource, completed_bytes, total_bytes);
        return true;
    };
    const auto read_log = [&](const ZipEntry& entry) -> bool {
        if (cancellationRequested(options, error)) return false;
        std::vector<uint8_t> log;
        if (!archive.read(entry, &log, error) ||
            !collectWalRecords(log, store, &processed_records, skipped_dimension_records,
                               options, error)) return false;
        completed_bytes += entry.uncompressed_size;
        reportProgress(options, SchematicParseStage::ReadingSource, completed_bytes, total_bytes);
        return true;
    };
    // LevelDB sequence numbers determine visibility, not archive ordering. Read
    // both sources and let RecordStore retain the highest internal sequence;
    // logs are deliberately processed too because a freshly exported world may
    // have all current chunks only in its WAL.
    for (const ZipEntry* entry : tables) if (!read_table(*entry)) return false;
    for (const ZipEntry* entry : logs) if (!read_log(*entry)) return false;
    return true;
}

bool isSafeBedrockToken(std::string_view value) {
    if (value.empty() || value.size() > 512) return false;
    for (const unsigned char byte : value) {
        if (!(std::isalnum(byte) || byte == '_' || byte == '-' || byte == '.' ||
              byte == ':' || byte == '/')) return false;
    }
    return true;
}

struct BedrockPaletteState {
    std::string identifier;
    std::string properties;
    std::string combined;
};

uint16_t commandBlockModeForIdentifier(std::string_view identifier) {
    const size_t separator = identifier.rfind(':');
    const std::string_view leaf = separator == std::string_view::npos
        ? identifier : identifier.substr(separator + 1);
    const auto equals_ignore_case = [](std::string_view left, std::string_view right) {
        if (left.size() != right.size()) return false;
        for (size_t index = 0; index < left.size(); ++index) {
            const auto lowercase = [](unsigned char value) {
                return value >= 'A' && value <= 'Z'
                    ? static_cast<unsigned char>(value - 'A' + 'a') : value;
            };
            if (lowercase(static_cast<unsigned char>(left[index])) !=
                lowercase(static_cast<unsigned char>(right[index]))) return false;
        }
        return true;
    };
    if (equals_ignore_case(leaf, "repeating_command_block") ||
        equals_ignore_case(leaf, "repeatingcommandblock")) return 1;
    if (equals_ignore_case(leaf, "chain_command_block") ||
        equals_ignore_case(leaf, "chaincommandblock")) return 2;
    return 0;
}

bool isCommandBlockEntityIdentifier(std::string_view identifier) {
    const size_t separator = identifier.rfind(':');
    const std::string_view leaf = separator == std::string_view::npos
        ? identifier : identifier.substr(separator + 1);
    const auto equals_ignore_case = [](std::string_view left, std::string_view right) {
        if (left.size() != right.size()) return false;
        for (size_t index = 0; index < left.size(); ++index) {
            const auto lowercase = [](unsigned char value) {
                return value >= 'A' && value <= 'Z'
                    ? static_cast<unsigned char>(value - 'A' + 'a') : value;
            };
            if (lowercase(static_cast<unsigned char>(left[index])) !=
                lowercase(static_cast<unsigned char>(right[index]))) return false;
        }
        return true;
    };
    return equals_ignore_case(leaf, "command_block") ||
           equals_ignore_case(leaf, "repeating_command_block") ||
           equals_ignore_case(leaf, "chain_command_block") ||
           equals_ignore_case(leaf, "commandblock") ||
           equals_ignore_case(leaf, "repeatingcommandblock") ||
           equals_ignore_case(leaf, "chaincommandblock") ||
           equals_ignore_case(leaf, "control");
}

class LittleNbtReader {
public:
    LittleNbtReader(const uint8_t* data, size_t size, size_t* cursor)
        : data_(data), size_(size), cursor_(cursor) {}

    bool paletteEntry(BedrockPaletteState* state, std::string* error) {
        if (!state) return fail(error, "mcworld palette output is unavailable");
        uint8_t type = 0;
        std::string root_name;
        if (!u8(&type) || type != 10 || !string(&root_name, 4096)) {
            return fail(error, "invalid mcworld palette NBT root");
        }
        std::string identifier;
        std::map<std::string, std::string> properties;
        bool saw_states = false;
        while (true) {
            uint8_t child_type = 0;
            if (!u8(&child_type)) return fail(error, "truncated mcworld palette NBT");
            if (child_type == 0) break;
            std::string name;
            if (!string(&name, 4096)) return fail(error, "invalid mcworld palette NBT name");
            if (name == "name" && child_type == 8) {
                if (!string(&identifier, 512)) return fail(error, "invalid mcworld block name");
            } else if (name == "states" && child_type == 10) {
                if (saw_states || !stateCompound(&properties, error)) return false;
                saw_states = true;
            } else if (!skipPayload(child_type, 0, error)) {
                return false;
            }
        }
        if (!isSafeBedrockToken(identifier)) {
            return fail(error, "mcworld palette has an unsafe block identifier");
        }
        if (identifier.find(':') == std::string::npos) identifier = "minecraft:" + identifier;
        // Some Bedrock saves serialize facing_direction as a cardinal string,
        // whereas the target's Bedrock-state mapper uses the native 0..5 enum.
        // Normalize only the exact six values; numeric states remain untouched.
        for (auto& property : properties) {
            const size_t separator = property.first.rfind(':');
            const std::string_view key = separator == std::string::npos
                ? std::string_view(property.first)
                : std::string_view(property.first).substr(separator + 1);
            if (key != "facing_direction") continue;
            const std::string& value = property.second;
            if (value == "\"down\"") property.second = "0";
            else if (value == "\"up\"") property.second = "1";
            else if (value == "\"north\"") property.second = "2";
            else if (value == "\"south\"") property.second = "3";
            else if (value == "\"west\"") property.second = "4";
            else if (value == "\"east\"") property.second = "5";
        }
        // Old Bedrock worlds use normal_stone_slab for smooth stone. Rewriting
        // it to stone_block_slab would select the modern `stone_slab` material
        // instead, so retain the canonical smooth-stone route used by the
        // mapper. The double form has no separate target identifier in the
        // flattened path; make its physical type explicit before mapping.
        if (identifier == "minecraft:normal_stone_slab") {
            identifier = "minecraft:smooth_stone_slab";
        } else if (identifier == "minecraft:normal_stone_double_slab" ||
                   identifier == "minecraft:normal_stone_slab_double") {
            identifier = "minecraft:smooth_stone_slab";
            const auto [type, inserted] = properties.emplace("type", "\"double\"");
            if (!inserted && type->second != "\"double\"") {
                return fail(error, "invalid mcworld normal-stone-double-slab palette state");
            }
        }
        constexpr std::string_view kLightPrefix = "minecraft:light_block_";
        if (identifier.compare(0, kLightPrefix.size(), kLightPrefix) == 0) {
            const std::string_view level = std::string_view(identifier).substr(kLightPrefix.size());
            uint32_t parsed_level = 0;
            bool valid_level = !level.empty();
            for (const unsigned char digit : level) {
                if (digit < '0' || digit > '9') { valid_level = false; break; }
                parsed_level = parsed_level * 10U + (digit - '0');
            }
            if (!valid_level || parsed_level > 15 ||
                !properties.emplace("block_light_level", std::to_string(parsed_level)).second) {
                return fail(error, "invalid mcworld light-block palette identifier");
            }
            identifier = "minecraft:light_block";
        }
        state->identifier = std::move(identifier);
        state->properties.clear();
        for (const auto& entry : properties) {
            if (!state->properties.empty()) state->properties.push_back(',');
            state->properties += '"';
            state->properties += entry.first;
            state->properties += "\"=";
            state->properties += entry.second;
        }
        if (!state->properties.empty()) {
            state->properties.insert(state->properties.begin(), '[');
            state->properties.push_back(']');
        }
        state->combined = state->identifier + state->properties;
        return true;
    }

    // Parses one little-endian block-entity root.  This is intentionally a
    // narrow whitelist: arbitrary inventories, signs and behavior-pack NBT
    // remain skipped even when they share the chunk's 0x31 record.
    bool commandBlockEntry(CommandBlockRecord* record, bool* candidate,
                           bool* explicit_mode, bool* has_identifier,
                           bool* declared_command_block,
                           std::string* error) {
        if (!record || !candidate || !explicit_mode || !has_identifier ||
            !declared_command_block) {
            return fail(error, "mcworld command-block NBT output is unavailable");
        }
        *record = {};
        *candidate = false;
        *explicit_mode = false;
        *has_identifier = false;
        *declared_command_block = false;
        uint8_t root_type = 0;
        std::string root_name;
        if (!u8(&root_type) || root_type != 10 || !string(&root_name, 4096)) {
            return false;
        }
        std::string identifier;
        std::array<bool, 3> has_position{};
        uint32_t fields = 0;
        const auto read_integral = [&](uint8_t type, int64_t* value) -> bool {
            if (type == 1) {
                uint8_t raw = 0;
                if (!u8(&raw)) return false;
                *value = static_cast<int8_t>(raw);
                return true;
            }
            if (type == 2) {
                uint16_t raw = 0;
                if (!le16(&raw)) return false;
                *value = static_cast<int16_t>(raw);
                return true;
            }
            if (type == 3) {
                uint32_t raw = 0;
                if (!le32(&raw)) return false;
                *value = static_cast<int32_t>(raw);
                return true;
            }
            if (type == 4) {
                uint64_t raw = 0;
                if (!le64(&raw)) return false;
                *value = static_cast<int64_t>(raw);
                return true;
            }
            return false;
        };
        while (true) {
            uint8_t type = 0;
            std::string name;
            if (!u8(&type)) return false;
            if (type == 0) break;
            if (++fields > 4096 || !string(&name, 4096)) return false;
            if ((name == "id" || name == "Id") && type == 8) {
                if (!string(&identifier, CommandBlockSpoolWriter::kMaximumStringBytes)) return false;
                continue;
            }
            if ((name == "Command" || name == "CustomName" || name == "LastOutput") &&
                type == 8) {
                std::string* output = name == "Command" ? &record->command
                    : name == "CustomName" ? &record->name : &record->last_output;
                if (!string(output, CommandBlockSpoolWriter::kMaximumStringBytes)) return false;
                continue;
            }
            const int position_index = name == "x" ? 0 : name == "y" ? 1 : name == "z" ? 2 : -1;
            if (position_index >= 0) {
                int64_t value = 0;
                if (read_integral(type, &value)) {
                    if (value >= std::numeric_limits<int32_t>::min() &&
                        value <= std::numeric_limits<int32_t>::max()) {
                        int32_t* coordinate = position_index == 0 ? &record->x
                            : position_index == 1 ? &record->y : &record->z;
                        *coordinate = static_cast<int32_t>(value);
                        has_position[static_cast<size_t>(position_index)] = true;
                    }
                    continue;
                }
            }
            if (name == "TrackOutput" || name == "auto" || name == "conditionalMode" ||
                name == "ExecuteOnFirstTick" || name == "TickDelay" ||
                name == "LPCommandMode" || name == "CommandBlockMode") {
                int64_t value = 0;
                if (read_integral(type, &value)) {
                    if (name == "TrackOutput") record->output_tracked = value != 0;
                    else if (name == "auto") record->redstone_mode = value == 0;
                    else if (name == "conditionalMode") record->conditional = value != 0;
                    else if (name == "ExecuteOnFirstTick") record->executing_on_first_tick = value != 0;
                    else if (name == "TickDelay") {
                        if (value >= std::numeric_limits<int32_t>::min() &&
                            value <= std::numeric_limits<int32_t>::max()) {
                            record->tick_delay = static_cast<int32_t>(value);
                        }
                    } else if (value >= 0 && value <= UINT16_MAX) {
                        record->mode = static_cast<uint16_t>(value);
                        *explicit_mode = true;
                    }
                    continue;
                }
            }
            if (!skipPayload(type, 0, error)) return false;
        }
        *has_identifier = !identifier.empty();
        *declared_command_block = isCommandBlockEntityIdentifier(identifier);
        if (!*explicit_mode) record->mode = commandBlockModeForIdentifier(identifier);
        // Empty/default command blocks sometimes omit the Command tag.  The
        // caller independently verifies that the final world data has a
        // command-block shell at this position before emitting any packet.
        *candidate = has_position[0] && has_position[1] && has_position[2];
        return true;
    }

private:
    bool available(size_t count) const {
        return cursor_ && *cursor_ <= size_ && count <= size_ - *cursor_;
    }

    bool u8(uint8_t* value) {
        if (!value || !available(1)) return false;
        *value = data_[(*cursor_)++];
        return true;
    }

    bool le16(uint16_t* value) {
        if (!value || !available(2)) return false;
        *value = readLe16(data_ + *cursor_);
        *cursor_ += 2;
        return true;
    }

    bool le32(uint32_t* value) {
        if (!value || !available(4)) return false;
        *value = readLe32(data_ + *cursor_);
        *cursor_ += 4;
        return true;
    }

    bool le64(uint64_t* value) {
        if (!value || !available(8)) return false;
        *value = readLe64(data_ + *cursor_);
        *cursor_ += 8;
        return true;
    }

    bool string(std::string* value, size_t maximum) {
        uint16_t length = 0;
        if (!value || !le16(&length) || length > maximum || !available(length)) return false;
        value->assign(reinterpret_cast<const char*>(data_ + *cursor_), length);
        *cursor_ += length;
        return true;
    }

    bool count(uint32_t* value) {
        uint32_t raw = 0;
        if (!le32(&raw) || raw > kMaximumPaletteEntries) return false;
        *value = raw;
        return true;
    }

    bool stateValue(uint8_t type, std::string_view property_name,
                    std::string* value, std::string* error) {
        if (!value) return fail(error, "mcworld state output is unavailable");
        if (type == 1) {
            uint8_t raw = 0;
            if (!u8(&raw)) return fail(error, "truncated mcworld byte state");
            const bool boolean_property = property_name.size() >= 4 &&
                property_name.compare(property_name.size() - 4, 4, "_bit") == 0;
            *value = boolean_property && raw <= 1 ? (raw == 0 ? "false" : "true")
                                                  : std::to_string(readSignedByte(raw));
            return true;
        }
        if (type == 2) {
            uint16_t raw = 0;
            if (!le16(&raw)) return fail(error, "truncated mcworld short state");
            *value = std::to_string(static_cast<int16_t>(raw));
            return true;
        }
        if (type == 3) {
            uint32_t raw = 0;
            if (!le32(&raw)) return fail(error, "truncated mcworld integer state");
            *value = std::to_string(static_cast<int32_t>(raw));
            return true;
        }
        if (type == 4) {
            uint64_t raw = 0;
            if (!le64(&raw)) return fail(error, "truncated mcworld long state");
            *value = std::to_string(static_cast<int64_t>(raw));
            return true;
        }
        if (type == 8) {
            std::string raw;
            if (!string(&raw, 512) || !isSafeBedrockToken(raw)) {
                return fail(error, "invalid mcworld string state");
            }
            *value = '"' + raw + '"';
            return true;
        }
        return fail(error, "unsupported mcworld block-state NBT type");
    }

    bool stateCompound(std::map<std::string, std::string>* output, std::string* error) {
        if (!output) return fail(error, "mcworld state compound output is unavailable");
        while (true) {
            uint8_t type = 0;
            if (!u8(&type)) return fail(error, "truncated mcworld state compound");
            if (type == 0) return true;
            std::string name;
            std::string value;
            if (!string(&name, 256) || !isSafeBedrockToken(name) ||
                !stateValue(type, name, &value, error)) return false;
            if (!output->emplace(std::move(name), std::move(value)).second) {
                return fail(error, "duplicate mcworld block-state property");
            }
        }
    }

    bool skipPayload(uint8_t type, size_t depth, std::string* error) {
        if (depth > kMaximumNbtDepth) return fail(error, "mcworld NBT nesting is too deep");
        switch (type) {
            case 1: return available(1) ? (*cursor_ += 1, true) : fail(error, "truncated mcworld NBT byte");
            case 2: return available(2) ? (*cursor_ += 2, true) : fail(error, "truncated mcworld NBT short");
            case 3: return available(4) ? (*cursor_ += 4, true) : fail(error, "truncated mcworld NBT integer");
            case 4: return available(8) ? (*cursor_ += 8, true) : fail(error, "truncated mcworld NBT long");
            case 5: return available(4) ? (*cursor_ += 4, true) : fail(error, "truncated mcworld NBT float");
            case 6: return available(8) ? (*cursor_ += 8, true) : fail(error, "truncated mcworld NBT double");
            case 7: {
                uint32_t length = 0;
                if (!count(&length) || !available(length)) return fail(error, "invalid mcworld NBT byte array");
                *cursor_ += length;
                return true;
            }
            case 8: {
                std::string ignored;
                return string(&ignored, kMaximumTableBlockBytes) || fail(error, "invalid mcworld NBT string");
            }
            case 9: {
                uint8_t child_type = 0;
                uint32_t length = 0;
                if (!u8(&child_type) || !count(&length)) return fail(error, "invalid mcworld NBT list");
                for (uint32_t index = 0; index < length; ++index) {
                    if (!skipPayload(child_type, depth + 1, error)) return false;
                }
                return true;
            }
            case 10: {
                while (true) {
                    uint8_t child_type = 0;
                    if (!u8(&child_type)) return fail(error, "truncated mcworld NBT compound");
                    if (child_type == 0) return true;
                    std::string ignored;
                    if (!string(&ignored, 4096) || !skipPayload(child_type, depth + 1, error)) return false;
                }
            }
            case 11: {
                uint32_t length = 0;
                if (!count(&length) || length > (size_ - *cursor_) / 4U) {
                    return fail(error, "invalid mcworld NBT integer array");
                }
                *cursor_ += static_cast<size_t>(length) * 4U;
                return true;
            }
            case 12: {
                uint32_t length = 0;
                if (!count(&length) || length > (size_ - *cursor_) / 8U) {
                    return fail(error, "invalid mcworld NBT long array");
                }
                *cursor_ += static_cast<size_t>(length) * 8U;
                return true;
            }
            default:
                return fail(error, "invalid mcworld NBT tag type");
        }
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t* cursor_ = nullptr;
};

struct ResolvedPaletteEntry {
    BlockMappingResult mapping;
    std::string source;
};

struct PalettedStorage {
    uint32_t bits_per_block = 0;
    std::vector<uint32_t> words;
    std::vector<ResolvedPaletteEntry> palette;
};

class MappingContext {
public:
    MappingContext(const SchematicParseOptions& options, const BlockMapper& mapper)
        : options_(options), mapper_(mapper) {}

    const BlockMappingResult& bedrock(const BedrockPaletteState& state) {
        const auto found = bedrock_cache_.find(state.combined);
        if (found != bedrock_cache_.end()) return found->second;
        BlockMappingResult mapped;
        // Custom resolvers can retain source identities, including blocks
        // that the older command registry cannot place. Import has no resolver
        // and therefore uses the conservative target-specific fallbacks below.
        if (options_.state_block_resolver) {
            mapped = options_.state_block_resolver(state.combined);
        } else if (state.identifier == "minecraft:border_block" ||
                   state.identifier == "minecraft:coral_fan_hang") {
            mapped.status = BlockMappingStatus::Air;
            mapped.reason = state.identifier == "minecraft:border_block"
                ? "Bedrock border block has no portable target and was skipped"
                : "Bedrock hanging coral fan has no target-version mapping and was skipped";
        } else if (state.identifier == "minecraft:jigsaw") {
            // A Bedrock jigsaw's pool/name/final-state live in the block-entity
            // record we intentionally never read. Retain an inert shell rather
            // than executing or copying that payload.
            mapped.status = BlockMappingStatus::Mapped;
            mapped.spec.command_name = "minecraft:structure_block";
            mapped.spec.phase = ImportPhase::Structure;
            mapped.spec.can_fill = false;
            mapped.spec.stateful = true;
            mapped.reason = "jigsaw block-entity data was omitted; imported as an inert structure-block shell";
        } else {
            mapped = mapper_.mapBedrockState(state.identifier, state.properties);
        }
        return bedrock_cache_.emplace(state.combined, std::move(mapped)).first->second;
    }

    const BlockMappingResult& legacy(uint16_t id, uint8_t aux) {
        const uint32_t key = (static_cast<uint32_t>(id) << 8U) | aux;
        const auto found = legacy_cache_.find(key);
        if (found != legacy_cache_.end()) return found->second;
        BlockMappingResult mapped = options_.legacy_block_resolver
            ? options_.legacy_block_resolver(id, aux) : mapper_.mapLegacy(id, aux);
        return legacy_cache_.emplace(key, std::move(mapped)).first->second;
    }

private:
    const SchematicParseOptions& options_;
    const BlockMapper& mapper_;
    std::unordered_map<std::string, BlockMappingResult> bedrock_cache_;
    std::unordered_map<uint32_t, BlockMappingResult> legacy_cache_;
};

bool paletteIndex(const PalettedStorage& storage, uint32_t index, uint32_t* output,
                  std::string* error) {
    if (!output || storage.bits_per_block == 0 || storage.bits_per_block > 16) {
        return fail(error, "invalid mcworld paletted storage");
    }
    const uint32_t entries_per_word = 32U / storage.bits_per_block;
    const uint32_t word_index = index / entries_per_word;
    const uint32_t bit_offset = (index % entries_per_word) * storage.bits_per_block;
    if (word_index >= storage.words.size()) return fail(error, "truncated mcworld paletted storage");
    const uint32_t mask = storage.bits_per_block == 16 ? 0xFFFFU
                                                       : ((1U << storage.bits_per_block) - 1U);
    *output = (storage.words[word_index] >> bit_offset) & mask;
    if (*output >= storage.palette.size()) {
        return fail(error, "mcworld block palette index is out of range");
    }
    return true;
}

template <typename Callback>
bool visitLegacySubchunk(const std::vector<uint8_t>& payload, const SubchunkAddress& address,
                          MappingContext* mappings, const SchematicParseOptions& options,
                          Callback&& callback, std::string* error) {
    constexpr size_t kLegacyBlockDataBytes = 1U + 4096U + 2048U;
    constexpr size_t kLegacyLightArrayBytes = 2048U;
    // Versions 0 and 2..7 store the legacy ID/data arrays.  The trailing
    // skylight and blocklight nibble arrays are optional and have no bearing
    // on a block-only import, so retain the placement data and deliberately
    // ignore either or both light arrays.
    const size_t size = payload.size();
    if (!mappings || (size != kLegacyBlockDataBytes &&
                      size != kLegacyBlockDataBytes + kLegacyLightArrayBytes &&
                      size != kLegacyBlockDataBytes + kLegacyLightArrayBytes * 2U)) {
        return fail(error, "invalid legacy mcworld subchunk length");
    }
    for (uint32_t index = 0; index < 4096; ++index) {
        if ((index % kCancellationInterval) == 0 && cancellationRequested(options, error)) return false;
        const uint16_t id = payload[1U + index];
        const uint8_t packed_aux = payload[1U + 4096U + index / 2U];
        const uint8_t aux = static_cast<uint8_t>((packed_aux >> ((index & 1U) * 4U)) & 0x0FU);
        const BlockMappingResult& mapping = mappings->legacy(id, aux);
        if (mapping.status == BlockMappingStatus::Air) continue;
        // Bedrock's persistent subchunk arrays use YZX order: Y increments
        // first, then Z, then X.  This differs from the Java/Sponge XZY
        // layouts used elsewhere in the importer.
        const int64_t x = static_cast<int64_t>(address.chunk_x) * 16 + ((index >> 8U) & 0x0FU);
        const int64_t z = static_cast<int64_t>(address.chunk_z) * 16 + ((index >> 4U) & 0x0FU);
        const int64_t y = static_cast<int64_t>(address.section_y) * 16 + (index & 0x0FU);
        if (!callback(x, y, z, mapping, std::string_view{})) return false;
    }
    return true;
}

template <typename Callback>
bool visitPalettedSubchunk(const std::vector<uint8_t>& payload, const SubchunkAddress& address,
                           MappingContext* mappings, const SchematicParseOptions& options,
                           Callback&& callback, std::string* error) {
    if (!mappings || payload.empty()) return fail(error, "truncated mcworld paletted subchunk");
    const uint8_t version = payload[0];
    if (version != 1 && version != 8 && version != 9) {
        return fail(error, "unsupported mcworld subchunk version " + std::to_string(version));
    }
    // Version 1 is the first portable palette format.  It stores precisely
    // one block-storage record and omits the later storage-count byte.
    uint8_t storage_count = 1;
    size_t cursor = 1;
    if (version != 1) {
        if (payload.size() < 2) return fail(error, "truncated mcworld paletted subchunk");
        storage_count = payload[1];
        cursor = version == 9 ? 3 : 2;
    }
    if (version == 9 && payload.size() < 3) return fail(error, "truncated mcworld v9 subchunk");
    if (version == 9 && readSignedByte(payload[2]) != address.section_y) {
        return fail(error, "mcworld v9 subchunk section does not match its LevelDB key");
    }
    if (storage_count == 0) {
        return cursor == payload.size() || fail(error, "trailing data after empty mcworld subchunk");
    }
    if (storage_count > 8) return fail(error, "mcworld subchunk has too many storage layers");
    std::vector<PalettedStorage> layers;
    layers.reserve(storage_count);
    for (uint8_t layer = 0; layer < storage_count; ++layer) {
        if (cursor >= payload.size()) return fail(error, "truncated mcworld paletted storage header");
        const uint8_t header = payload[cursor++];
        if ((header & 1U) != 0) {
            return fail(error, "runtime-id mcworld subchunks are not portable");
        }
        PalettedStorage storage;
        storage.bits_per_block = header >> 1U;
        if (storage.bits_per_block == 0 || storage.bits_per_block > 16) {
            return fail(error, "invalid mcworld bits-per-block value");
        }
        const uint32_t entries_per_word = 32U / storage.bits_per_block;
        const uint32_t word_count = (4096U + entries_per_word - 1U) / entries_per_word;
        if (word_count > (payload.size() - cursor) / 4U) {
            return fail(error, "truncated mcworld paletted storage words");
        }
        storage.words.reserve(word_count);
        for (uint32_t word = 0; word < word_count; ++word) {
            storage.words.push_back(readLe32(payload.data() + cursor));
            cursor += 4;
        }
        if (cursor > payload.size() || payload.size() - cursor < 4) {
            return fail(error, "truncated mcworld block palette size");
        }
        const uint32_t palette_count = readLe32(payload.data() + cursor);
        cursor += 4;
        if (palette_count == 0 || palette_count > kMaximumPaletteEntries) {
            return fail(error, "invalid mcworld block palette size");
        }
        storage.palette.reserve(static_cast<size_t>(palette_count));
        for (uint32_t entry = 0; entry < palette_count; ++entry) {
            BedrockPaletteState state;
            LittleNbtReader reader(payload.data(), payload.size(), &cursor);
            if (!reader.paletteEntry(&state, error)) return false;
            ResolvedPaletteEntry resolved;
            resolved.mapping = mappings->bedrock(state);
            resolved.source = std::move(state.combined);
            storage.palette.push_back(std::move(resolved));
        }
        layers.push_back(std::move(storage));
    }
    if (cursor != payload.size()) return fail(error, "trailing data in mcworld paletted subchunk");
    for (uint32_t index = 0; index < 4096; ++index) {
        if ((index % kCancellationInterval) == 0 && cancellationRequested(options, error)) return false;
        const ResolvedPaletteEntry* selected = nullptr;
        for (const PalettedStorage& layer : layers) {
            uint32_t entry_index = 0;
            if (!paletteIndex(layer, index, &entry_index, error)) return false;
            const ResolvedPaletteEntry& entry = layer.palette[entry_index];
            if (entry.mapping.status == BlockMappingStatus::Air) continue;
            selected = &entry;
            break;
        }
        if (!selected) continue;
        // See the legacy reader above: paletted storages use the same YZX
        // ordering, even though their palette and bit packing differ.
        const int64_t x = static_cast<int64_t>(address.chunk_x) * 16 + ((index >> 8U) & 0x0FU);
        const int64_t z = static_cast<int64_t>(address.chunk_z) * 16 + ((index >> 4U) & 0x0FU);
        const int64_t y = static_cast<int64_t>(address.section_y) * 16 + (index & 0x0FU);
        if (!callback(x, y, z, selected->mapping, selected->source)) return false;
    }
    return true;
}

template <typename Callback>
bool visitSubchunk(const std::vector<uint8_t>& payload, const SubchunkAddress& address,
                   MappingContext* mappings, const SchematicParseOptions& options,
                   Callback&& callback, std::string* error) {
    if (payload.empty()) return fail(error, "empty mcworld subchunk record");
    if (payload[0] == 0 || (payload[0] >= 2 && payload[0] <= 7)) {
        return visitLegacySubchunk(payload, address, mappings, options,
                                   std::forward<Callback>(callback), error);
    }
    return visitPalettedSubchunk(payload, address, mappings, options,
                                 std::forward<Callback>(callback), error);
}

struct RawBounds {
    bool valid = false;
    int64_t min_x = 0, min_y = 0, min_z = 0;
    int64_t max_x = 0, max_y = 0, max_z = 0;

    void include(int64_t x, int64_t y, int64_t z) {
        if (!valid) {
            valid = true;
            min_x = max_x = x;
            min_y = max_y = y;
            min_z = max_z = z;
            return;
        }
        min_x = std::min(min_x, x); max_x = std::max(max_x, x);
        min_y = std::min(min_y, y); max_y = std::max(max_y, y);
        min_z = std::min(min_z, z); max_z = std::max(max_z, z);
    }
};

struct RawBlockCoord {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const RawBlockCoord& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct RawBlockCoordHash {
    size_t operator()(const RawBlockCoord& value) const {
        uint64_t hash = static_cast<uint32_t>(value.x);
        hash = (hash * 0x9E3779B185EBCA87ULL) ^ static_cast<uint32_t>(value.y);
        hash = (hash * 0xC2B2AE3D27D4EB4FULL) ^ static_cast<uint32_t>(value.z);
        return static_cast<size_t>(hash ^ (hash >> 32U));
    }
};

uint64_t saturatedVolume(const BlockBounds& bounds) {
    if (!bounds.isValid()) return 0;
    const uint64_t width = static_cast<uint64_t>(static_cast<int64_t>(bounds.max_x) - bounds.min_x + 1);
    const uint64_t height = static_cast<uint64_t>(static_cast<int64_t>(bounds.max_y) - bounds.min_y + 1);
    const uint64_t length = static_cast<uint64_t>(static_cast<int64_t>(bounds.max_z) - bounds.min_z + 1);
    if (width != 0 && height > UINT64_MAX / width) return UINT64_MAX;
    const uint64_t area = width * height;
    return area != 0 && length > UINT64_MAX / area ? UINT64_MAX : area * length;
}

}  // namespace

bool McworldParser::parse(const SchematicParseOptions& options,
                          SchematicParseResult* result, std::string* error) const {
    BlockMapper mapper;
    return parse(options, mapper, result, error);
}

bool McworldParser::parse(const SchematicParseOptions& options, const BlockMapper& mapper,
                          SchematicParseResult* result, std::string* error) const {
    if (!result) return fail(error, "mcworld parse result is unavailable");
    *result = {};
    if (options.source_path.empty()) return fail(error, "mcworld source path is empty");
    if (options.chunk_size <= 0) return fail(error, "mcworld chunk size is invalid");
    if (cancellationRequested(options, error)) return false;

    ZipArchive archive;
    if (!archive.open(options.source_path, error)) return false;
    RecordStore records;
    if (!records.open(options, error)) return false;
    uint64_t skipped_dimension_records = 0;
    if (!collectArchiveRecords(archive, &records, options, &skipped_dimension_records, error) ||
        !records.finalize(error)) return false;

    struct SubchunkRecord {
        SubchunkAddress address;
        StoredRecord record;
    };
    struct BlockEntityRecord {
        BlockEntityAddress address;
        StoredRecord record;
    };
    std::vector<SubchunkRecord> subchunks;
    std::vector<BlockEntityRecord> block_entities;
    subchunks.reserve(records.records().size());
    block_entities.reserve(records.records().size() / 16U + 1U);
    for (const auto& pair : records.records()) {
        if (pair.second.deleted) continue;
        SubchunkAddress address;
        if (parseSubchunkKey(pair.first, &address) == SubchunkKeyKind::Overworld) {
            subchunks.push_back({address, pair.second});
            continue;
        }
        BlockEntityAddress block_entity_address;
        if (parseBlockEntityKey(pair.first, &block_entity_address) ==
            BlockEntityKeyKind::Overworld) {
            block_entities.push_back({block_entity_address, pair.second});
        }
    }
    std::sort(subchunks.begin(), subchunks.end(), [](const SubchunkRecord& left,
                                                      const SubchunkRecord& right) {
        if (left.address.chunk_x != right.address.chunk_x) {
            return left.address.chunk_x < right.address.chunk_x;
        }
        if (left.address.chunk_z != right.address.chunk_z) {
            return left.address.chunk_z < right.address.chunk_z;
        }
        return left.address.section_y < right.address.section_y;
    });
    std::sort(block_entities.begin(), block_entities.end(),
              [](const BlockEntityRecord& left, const BlockEntityRecord& right) {
        if (left.address.chunk_x != right.address.chunk_x) {
            return left.address.chunk_x < right.address.chunk_x;
        }
        return left.address.chunk_z < right.address.chunk_z;
    });
    if (subchunks.empty()) return fail(error, "mcworld archive contains no overworld subchunks");

    MappingContext mappings(options, mapper);
    RawBounds raw_bounds;
    std::vector<uint8_t> payload;
    reportProgress(options, SchematicParseStage::RoutingBlocks, 0, subchunks.size());
    for (size_t index = 0; index < subchunks.size(); ++index) {
        if ((index % 8U) == 0 && cancellationRequested(options, error)) return false;
        if (!records.read(subchunks[index].record, &payload, error)) return false;
        if (!visitSubchunk(payload, subchunks[index].address, &mappings, options,
            [&](int64_t x, int64_t y, int64_t z, const BlockMappingResult& mapping,
                std::string_view source) -> bool {
                if (mapping.status == BlockMappingStatus::Unsupported) {
                    std::string message = "unsupported mcworld block at (" + std::to_string(x) + "," +
                        std::to_string(y) + "," + std::to_string(z) + ")";
                    if (!source.empty()) message += ": " + std::string(source);
                    if (!mapping.reason.empty()) message += " (" + mapping.reason + ")";
                    return fail(error, std::move(message));
                }
                raw_bounds.include(x, y, z);
                return true;
            }, error)) return false;
        reportProgress(options, SchematicParseStage::RoutingBlocks, index + 1, subchunks.size());
    }
    if (!raw_bounds.valid) return fail(error, "mcworld contains no importable overworld blocks");
    const auto in_int32 = [](int64_t value) {
        return value >= std::numeric_limits<int32_t>::min() &&
               value <= std::numeric_limits<int32_t>::max();
    };
    if (!in_int32(raw_bounds.min_x) || !in_int32(raw_bounds.min_y) || !in_int32(raw_bounds.min_z)) {
        return fail(error, "mcworld source coordinates exceed the supported world range");
    }
    int32_t max_x = 0, max_y = 0, max_z = 0;
    if (!checkedAdd(options.base_x, raw_bounds.max_x - raw_bounds.min_x, &max_x, error) ||
        !checkedAdd(options.base_y, raw_bounds.max_y - raw_bounds.min_y, &max_y, error) ||
        !checkedAdd(options.base_z, raw_bounds.max_z - raw_bounds.min_z, &max_z, error)) return false;
    result->source_offset_x = static_cast<int32_t>(raw_bounds.min_x);
    result->source_offset_y = static_cast<int32_t>(raw_bounds.min_y);
    result->source_offset_z = static_cast<int32_t>(raw_bounds.min_z);
    result->source_volume_bounds = {options.base_x, options.base_y, options.base_z,
                                    max_x, max_y, max_z};
    result->source_voxel_count = saturatedVolume(result->source_volume_bounds);
    result->source_region_count = 1;
    if (skipped_dimension_records != 0) {
        result->first_degradation_reason = "non-overworld mcworld subchunk records were skipped";
    }

    std::unique_ptr<ChunkSpoolWriter> writer;
    if (!options.block_sink) {
        writer = std::make_unique<ChunkSpoolWriter>(options.spool_directory, options.chunk_size,
                                                    options.maximum_chunk_descriptors);
        if (options.include_source_volume &&
            !writer->includeVolume(result->source_volume_bounds, error,
                                   options.cancellation_requested)) return false;
    }

    std::unordered_map<RawBlockCoord, CommandBlockShellState, RawBlockCoordHash>
        command_block_shells;
    if (options.command_block_sink) command_block_shells.reserve(block_entities.size() * 2U + 16U);
    reportProgress(options, SchematicParseStage::RoutingBlocks, 0, subchunks.size());
    for (size_t index = 0; index < subchunks.size(); ++index) {
        if ((index % 8U) == 0 && cancellationRequested(options, error)) {
            if (writer) writer->discard();
            return false;
        }
        if (!records.read(subchunks[index].record, &payload, error) ||
            !visitSubchunk(payload, subchunks[index].address, &mappings, options,
                [&](int64_t raw_x, int64_t raw_y, int64_t raw_z,
                    const BlockMappingResult& mapping, std::string_view source) -> bool {
                    if (mapping.status == BlockMappingStatus::Unsupported) {
                        return fail(error, "unsupported mcworld block changed during routing");
                    }
                    if (options.maximum_output_blocks != 0 &&
                        result->imported_block_count >= options.maximum_output_blocks) {
                        return fail(error, "parsed mcworld block count exceeds configured limit of " +
                                           std::to_string(options.maximum_output_blocks));
                    }
                    ParsedBlock block;
                    if (!checkedAdd(options.base_x, raw_x - raw_bounds.min_x, &block.world_x, error) ||
                        !checkedAdd(options.base_y, raw_y - raw_bounds.min_y, &block.world_y, error) ||
                        !checkedAdd(options.base_z, raw_z - raw_bounds.min_z, &block.world_z, error)) {
                        return false;
                    }
                    block.spec = mapping.spec;
                    if (options.command_block_sink &&
                        (mapping.spec.command_name == "minecraft:command_block" ||
                         mapping.spec.command_name == "minecraft:repeating_command_block" ||
                         mapping.spec.command_name == "minecraft:chain_command_block") &&
                        raw_x >= std::numeric_limits<int32_t>::min() &&
                        raw_x <= std::numeric_limits<int32_t>::max() &&
                        raw_y >= std::numeric_limits<int32_t>::min() &&
                        raw_y <= std::numeric_limits<int32_t>::max() &&
                        raw_z >= std::numeric_limits<int32_t>::min() &&
                        raw_z <= std::numeric_limits<int32_t>::max()) {
                        command_block_shells.emplace(
                            RawBlockCoord{static_cast<int32_t>(raw_x),
                                          static_cast<int32_t>(raw_y),
                                          static_cast<int32_t>(raw_z)},
                            commandBlockShellState(commandBlockModeForIdentifier(
                                mapping.spec.command_name), mapping.spec.aux));
                    }
                    if (options.block_sink) {
                        if (!options.block_sink(block, error)) return false;
                    } else if (!writer || !writer->append(block, error)) {
                        return fail(error, "mcworld block spool output is unavailable");
                    }
                    if (!mapping.reason.empty()) {
                        ++result->degraded_block_count;
                        if (result->first_degradation_reason.empty()) {
                            result->first_degradation_reason = std::string(source) + ": " + mapping.reason;
                        }
                    }
                    ++result->imported_block_count;
                    return true;
                }, error)) {
            if (writer) writer->discard();
            return false;
        }
        reportProgress(options, SchematicParseStage::RoutingBlocks, index + 1, subchunks.size());
    }
    if (options.command_block_sink) {
        for (const BlockEntityRecord& block_entity : block_entities) {
            if (cancellationRequested(options, error)) {
                if (writer) writer->discard();
                return false;
            }
            if (!records.read(block_entity.record, &payload, error)) {
                if (writer) writer->discard();
                return false;
            }
            size_t cursor = 0;
            while (cursor < payload.size()) {
                const size_t record_start = cursor;
                LittleNbtReader reader(payload.data(), payload.size(), &cursor);
                CommandBlockRecord command_block;
                bool candidate = false;
                bool explicit_mode = false;
                bool has_identifier = false;
                bool declared_command_block = false;
                // A 0x31 value can contain unrelated or exporter-specific
                // block-entity NBT.  Invalid/unknown entries are skipped as a
                // whole, preserving the historic container omission behavior.
                if (!reader.commandBlockEntry(&command_block, &candidate,
                                               &explicit_mode, &has_identifier,
                                               &declared_command_block,
                                               nullptr) || cursor <= record_start) {
                    break;
                }
                // An explicit non-command id must never configure a command
                // block merely because a stale/malformed 0x31 record happens
                // to use the same coordinate.  Keep id-less legacy NBT: the
                // actual shell check below remains the final authority there.
                if (!candidate || (has_identifier && !declared_command_block)) continue;
                const RawBlockCoord source_position{
                    command_block.x, command_block.y, command_block.z};
                const auto shell = command_block_shells.find(source_position);
                // NBT can remain after its block was replaced (or can be a
                // malformed mod payload that merely claims an `id`). Require
                // a shell parsed from the final subchunk data before emitting
                // an editor update; otherwise a packet could target a chest,
                // air, or a stale coordinate.
                if (shell == command_block_shells.end()) {
                    ++result->omitted_command_block_data_count;
                    continue;
                }
                // The final palette shell is authoritative for both its type
                // and conditional bit.  A LevelDB block-entity record can be
                // stale or omit conditionalMode, and must not reset the shell
                // while its editor packet is being applied.
                applyCommandBlockShellState(&command_block, shell->second);
                if (!isValidCommandBlockMode(command_block.mode)) {
                    if (writer) writer->discard();
                    return fail(error, "mcworld command-block mode is invalid");
                }
                if (!checkedAdd(options.base_x,
                                static_cast<int64_t>(command_block.x) - raw_bounds.min_x,
                                &command_block.x, error) ||
                    !checkedAdd(options.base_y,
                                static_cast<int64_t>(command_block.y) - raw_bounds.min_y,
                                &command_block.y, error) ||
                    !checkedAdd(options.base_z,
                                static_cast<int64_t>(command_block.z) - raw_bounds.min_z,
                                &command_block.z, error)) {
                    if (writer) writer->discard();
                    return false;
                }
                if (!options.command_block_sink(command_block, error)) {
                    if (writer) writer->discard();
                    if (error && error->empty()) {
                        *error = "mcworld command-block sink rejected a payload";
                    }
                    return false;
                }
                ++result->command_block_payload_count;
            }
        }
    }
    reportProgress(options, SchematicParseStage::FinalizingSpools, 0,
                   result->imported_block_count);
    if (writer) {
        result->chunks = writer->finish(error, options.cancellation_requested,
            [&](uint64_t completed, uint64_t total) {
                reportProgress(options, SchematicParseStage::FinalizingSpools, completed, total);
            });
        if (result->chunks.empty() && result->imported_block_count != 0) {
            return fail(error, "cannot finalize mcworld chunk spools");
        }
    }
    if (cancellationRequested(options, error)) {
        if (writer) writer->discard();
        result->chunks.clear();
        return false;
    }
    reportProgress(options, SchematicParseStage::FinalizingSpools,
                   result->imported_block_count, result->imported_block_count);
    return options.block_sink ? result->imported_block_count != 0 : !result->chunks.empty();
}

}  // namespace build_import
