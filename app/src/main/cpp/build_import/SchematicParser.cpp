#include "SchematicParser.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <zlib.h>

namespace build_import {
namespace {

enum NbtTag : uint8_t { End = 0, Byte = 1, Short = 2, Int = 3, Long = 4, Float = 5,
                        Double = 6, ByteArray = 7, String = 8, List = 9, Compound = 10,
                        IntArray = 11, LongArray = 12 };

using CancellationCallback = std::function<bool()>;
using ArrayProgressCallback = std::function<void(uint64_t, uint64_t)>;

constexpr size_t kStreamingBufferSize = 256U * 1024U;
constexpr uint32_t kMaximumDeferredCommandBlocks = 1024U * 1024U;
constexpr uint64_t kMaximumDeferredCommandPayloadBytes = 64ULL * 1024ULL * 1024ULL;
constexpr size_t kMaximumCommandBlockTextBytes = CommandBlockSpoolWriter::kMaximumStringBytes;
// These limits mirror SchematicWriter's validation. Raw records are an
// extension, so a malformed third-party file must not trigger unbounded
// allocation even when the ordinary BlockData is small.
constexpr uint32_t kMaximumRawBlocks = 1024U * 1024U;
constexpr uint64_t kMaximumRawPayloadBytes = 256ULL * 1024ULL * 1024ULL;
constexpr size_t kMaximumRawIdentifierBytes = 1024U;
constexpr size_t kMaximumRawStateBytes = 64U * 1024U;
constexpr size_t kMaximumRawEntityBytes = 64U * 1024U * 1024U;

bool cancellationRequested(const CancellationCallback& callback, std::string* error) {
    if (!callback || !callback()) return false;
    if (error) *error = "import cancelled";
    return true;
}

void reportProgress(const SchematicParseOptions& options, SchematicParseStage stage,
                    uint64_t completed = 0, uint64_t total = 0) {
    if (options.progress_callback) {
        options.progress_callback({stage, completed, total});
    }
}

class Reader {
public:
    explicit Reader(const std::string& path)
        : source_path_(path), stream_(gzopen(path.c_str(), "rb")),
          scratch_(kStreamingBufferSize) {
        if (stream_) gzbuffer(stream_, static_cast<unsigned>(kStreamingBufferSize));
    }
    ~Reader() { if (stream_) gzclose(stream_); }
    bool valid() const { return stream_ != nullptr; }
    const std::string& failure() const { return failure_; }
    uint64_t bytesRead() const { return bytes_read_; }
    bool read(void* data, size_t length) {
        if (!stream_) {
            failure_ = "cannot open gzip-compressed schematic stream";
            return false;
        }
        uint8_t* output = static_cast<uint8_t*>(data);
        while (length != 0) {
            const unsigned request = static_cast<unsigned>(std::min<size_t>(length, 1U << 20));
            const int received = gzread(stream_, output, request);
            if (received <= 0) {
                recordGzipFailure();
                return false;
            }
            crc_ = crc32(crc_, output, static_cast<uInt>(received));
            bytes_read_ += static_cast<uint64_t>(received);
            output += received;
            length -= static_cast<size_t>(received);
        }
        return true;
    }
    uint8_t* scratchData() { return scratch_.data(); }
    size_t scratchSize() const { return scratch_.size(); }
    bool u8(uint8_t* value) { return read(value, 1); }
    bool be16(uint16_t* output) { uint8_t b[2]; if (!read(b, 2)) return false; *output = (uint16_t(b[0]) << 8) | b[1]; return true; }
    bool be32(uint32_t* output) { uint8_t b[4]; if (!read(b, 4)) return false; *output = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3]; return true; }
    bool string(std::string* output, size_t maximum_length = 4096) {
        uint16_t n = 0;
        if (!be16(&n) || n > maximum_length) return false;
        output->assign(n, '\0');
        return n == 0 || read(&(*output)[0], n);
    }
    bool exhausted() {
        uint8_t trailing = 0;
        const int received = gzread(stream_, &trailing, 1);
        // A truncated gzip trailer can report a zero-byte read. Consult the
        // zlib status before treating that as a clean logical EOF; otherwise
        // a file with valid NBT followed by a cut-off compressed stream could
        // be accepted as complete.
        if (received <= 0) recordGzipFailure();
        if (received != 0 || !failure_.empty() || gzeof(stream_) == 0) return false;
        if (!validateGzipTrailer()) return false;
        const int close_status = gzclose(stream_);
        stream_ = nullptr;
        if (close_status != Z_OK) {
            failure_ = "gzip-compressed schematic data is corrupt";
            return false;
        }
        return true;
    }
private:
    bool validateGzipTrailer() {
        // gzread() deliberately permits a source that ends before the gzip
        // trailer.  Check the single-member trailer ourselves against the
        // bytes already emitted by zlib so a valid NBT prefix cannot mask a
        // truncated or checksum-corrupted schematic.  gzopen also accepts
        // plain NBT streams, for which no gzip trailer is expected.
        std::ifstream source(source_path_, std::ios::binary | std::ios::ate);
        if (!source) {
            failure_ = "cannot validate gzip-compressed schematic data";
            return false;
        }
        const std::streamoff source_size = source.tellg();
        if (source_size < 2) {
            failure_ = "gzip-compressed schematic data is corrupt";
            return false;
        }
        source.seekg(0, std::ios::beg);
        uint8_t magic[2]{};
        source.read(reinterpret_cast<char*>(magic), sizeof(magic));
        if (!source) {
            failure_ = "cannot validate gzip-compressed schematic data";
            return false;
        }
        if (magic[0] != 0x1F || magic[1] != 0x8B) return true;
        if (source_size < 18) {
            failure_ = "gzip-compressed schematic data is corrupt";
            return false;
        }
        source.seekg(-8, std::ios::end);
        uint8_t trailer[8]{};
        source.read(reinterpret_cast<char*>(trailer), sizeof(trailer));
        if (!source) {
            failure_ = "cannot validate gzip-compressed schematic data";
            return false;
        }
        const uint32_t expected_crc = uint32_t(trailer[0]) |
            (uint32_t(trailer[1]) << 8) | (uint32_t(trailer[2]) << 16) |
            (uint32_t(trailer[3]) << 24);
        const uint32_t expected_size = uint32_t(trailer[4]) |
            (uint32_t(trailer[5]) << 8) | (uint32_t(trailer[6]) << 16) |
            (uint32_t(trailer[7]) << 24);
        if (expected_crc != static_cast<uint32_t>(crc_) ||
            expected_size != static_cast<uint32_t>(bytes_read_)) {
            failure_ = "gzip-compressed schematic data is corrupt";
            return false;
        }
        return true;
    }

    void recordGzipFailure() {
        if (!stream_ || !failure_.empty()) return;
        int code = Z_OK;
        const char* description = gzerror(stream_, &code);
        if (code == Z_OK && gzeof(stream_) != 0) return;
        if (code == Z_DATA_ERROR || code == Z_STREAM_ERROR || code == Z_BUF_ERROR) {
            failure_ = "gzip-compressed schematic data is corrupt";
        } else if (code == Z_ERRNO) {
            failure_ = "cannot read gzip-compressed schematic data";
        } else {
            failure_ = "cannot read gzip-compressed schematic data";
        }
        if (description && *description) {
            failure_ += ": ";
            failure_ += description;
        }
    }

    std::string source_path_;
    gzFile stream_ = nullptr;
    std::vector<uint8_t> scratch_;
    std::string failure_;
    uint64_t bytes_read_ = 0;
    uLong crc_ = crc32(0L, Z_NULL, 0);
};

bool skipBytes(Reader& reader, uint64_t length, const CancellationCallback& cancellation_requested,
               std::string* error) {
    while (length != 0) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        const size_t chunk = std::min<uint64_t>(length, reader.scratchSize());
        if (!reader.read(reader.scratchData(), chunk)) return false;
        length -= chunk;
    }
    return true;
}

bool copyByteArray(Reader& reader, const std::string& path,
                   std::string_view array_name,
                   uint32_t* copied_length,
                   const CancellationCallback& cancellation_requested,
                   const ArrayProgressCallback& progress_callback,
                   std::string* error) {
    if (cancellationRequested(cancellation_requested, error)) return false;
    uint32_t signed_length = 0;
    if (!reader.be32(&signed_length) || static_cast<int32_t>(signed_length) < 0) {
        if (error) {
            *error = !reader.failure().empty() ? reader.failure()
                : "invalid " + std::string(array_name) + " array length";
        }
        return false;
    }
    if (copied_length) *copied_length = signed_length;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) { if (error) *error = "cannot create raw NBT spool"; return false; }
    uint64_t remaining = signed_length;
    const uint64_t payload_start = reader.bytesRead();
    if (progress_callback) progress_callback(0, signed_length);
    while (remaining != 0) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        const size_t chunk = std::min<uint64_t>(remaining, reader.scratchSize());
        if (!reader.read(reader.scratchData(), chunk)) {
            if (error) {
                *error = !reader.failure().empty() ? reader.failure()
                    : "truncated " + std::string(array_name) + " array (expected " +
                        std::to_string(signed_length) + " bytes, received " +
                        std::to_string(reader.bytesRead() - payload_start) + ")";
            }
            return false;
        }
        output.write(reinterpret_cast<const char*>(reader.scratchData()),
                     static_cast<std::streamsize>(chunk));
        if (!output) return false;
        remaining -= chunk;
        if (progress_callback) {
            progress_callback(static_cast<uint64_t>(signed_length) - remaining,
                              signed_length);
        }
    }
    output.flush();
    output.close();
    if (!output) {
        if (error) *error = "cannot finalize raw NBT spool";
        return false;
    }
    return true;
}

struct ByteArrayStorage {
    explicit ByteArrayStorage(std::string spool_path)
        : spool_path(std::move(spool_path)) {}

    std::string spool_path;
    std::vector<uint8_t> memory;
    uint32_t length = 0;
    bool in_memory = false;
};

bool readBoundedByteArrayPayload(Reader& reader, uint32_t signed_length,
                                  ByteArrayStorage* storage,
                                  size_t memory_budget_bytes,
                                  const CancellationCallback& cancellation_requested,
                                  const ArrayProgressCallback& progress_callback,
                                  std::string* error) {
    if (!storage || cancellationRequested(cancellation_requested, error)) return false;
    storage->length = signed_length;
    storage->memory.clear();
    storage->in_memory = false;
    if (signed_length <= memory_budget_bytes) {
        try {
            storage->memory.resize(signed_length);
            storage->in_memory = true;
        } catch (const std::bad_alloc&) {
            storage->memory.clear();
        }
    }

    std::ofstream output;
    if (!storage->in_memory) {
        output.open(storage->spool_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error) *error = "cannot create raw NBT spool";
            return false;
        }
    }
    if (progress_callback) progress_callback(0, signed_length);
    uint64_t position = 0;
    const uint64_t payload_start = reader.bytesRead();
    while (position < signed_length) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        const size_t amount = static_cast<size_t>(std::min<uint64_t>(
            static_cast<uint64_t>(signed_length) - position,
            reader.scratchSize()));
        if (storage->in_memory) {
            if (!reader.read(storage->memory.data() + static_cast<size_t>(position),
                             amount)) {
                if (error) *error = !reader.failure().empty() ? reader.failure()
                    : "truncated NBT byte array (expected " +
                        std::to_string(signed_length) + " bytes, received " +
                        std::to_string(reader.bytesRead() - payload_start) + ")";
                return false;
            }
        } else {
            if (!reader.read(reader.scratchData(), amount)) {
                if (error) *error = !reader.failure().empty() ? reader.failure()
                    : "truncated NBT byte array (expected " +
                        std::to_string(signed_length) + " bytes, received " +
                        std::to_string(reader.bytesRead() - payload_start) + ")";
                return false;
            }
            output.write(reinterpret_cast<const char*>(reader.scratchData()),
                         static_cast<std::streamsize>(amount));
            if (!output) return false;
        }
        position += amount;
        if (progress_callback) progress_callback(position, signed_length);
    }
    if (output.is_open()) {
        output.flush();
        output.close();
        if (!output) {
            if (error) *error = "cannot finalize raw NBT spool";
            return false;
        }
    }
    return true;
}

bool readBoundedByteArray(Reader& reader, ByteArrayStorage* storage,
                          size_t memory_budget_bytes,
                          const CancellationCallback& cancellation_requested,
                          const ArrayProgressCallback& progress_callback,
                          std::string* error) {
    if (!storage || cancellationRequested(cancellation_requested, error)) return false;
    uint32_t signed_length = 0;
    if (!reader.be32(&signed_length) || static_cast<int32_t>(signed_length) < 0) {
        if (error) *error = !reader.failure().empty() ? reader.failure()
            : "invalid NBT byte-array length";
        return false;
    }
    return readBoundedByteArrayPayload(reader, signed_length, storage,
                                       memory_budget_bytes,
                                       cancellation_requested,
                                       progress_callback, error);
}

bool skipByteArrayPayload(Reader& reader, uint32_t signed_length,
                          std::string_view array_name,
                          const CancellationCallback& cancellation_requested,
                          const ArrayProgressCallback& progress_callback,
                          std::string* error) {
    if (cancellationRequested(cancellation_requested, error)) return false;
    const uint64_t payload_start = reader.bytesRead();
    uint64_t remaining = signed_length;
    if (progress_callback) progress_callback(0, signed_length);
    while (remaining != 0) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        const size_t chunk = std::min<uint64_t>(remaining, reader.scratchSize());
        if (!reader.read(reader.scratchData(), chunk)) {
            if (error) {
                *error = !reader.failure().empty() ? reader.failure()
                    : "truncated " + std::string(array_name) + " array (expected " +
                        std::to_string(signed_length) + " bytes, received " +
                        std::to_string(reader.bytesRead() - payload_start) + ")";
            }
            return false;
        }
        remaining -= chunk;
        if (progress_callback) {
            progress_callback(static_cast<uint64_t>(signed_length) - remaining,
                              signed_length);
        }
    }
    return true;
}

bool skipPayload(Reader& reader, uint8_t tag, uint32_t depth,
                 const CancellationCallback& cancellation_requested, std::string* error) {
    if (cancellationRequested(cancellation_requested, error)) return false;
    if (depth > 64) return false;
    uint32_t count = 0; uint16_t string_size = 0; uint8_t list_tag = 0;
    switch (tag) {
        case Byte: return skipBytes(reader, 1, cancellation_requested, error);
        case Short: return skipBytes(reader, 2, cancellation_requested, error);
        case Int: case Float: return skipBytes(reader, 4, cancellation_requested, error);
        case Long: case Double: return skipBytes(reader, 8, cancellation_requested, error);
        case String:
            if (!reader.be16(&string_size)) return false;
            return skipBytes(reader, string_size, cancellation_requested, error);
        case ByteArray:
            if (!reader.be32(&count) || static_cast<int32_t>(count) < 0) return false;
            return skipBytes(reader, count, cancellation_requested, error);
        case IntArray:
            if (!reader.be32(&count) || static_cast<int32_t>(count) < 0) return false;
            return skipBytes(reader, uint64_t(count) * 4, cancellation_requested, error);
        case LongArray:
            if (!reader.be32(&count) || static_cast<int32_t>(count) < 0) return false;
            return skipBytes(reader, uint64_t(count) * 8, cancellation_requested, error);
        case List:
            if (!reader.u8(&list_tag) || !reader.be32(&count) || static_cast<int32_t>(count) < 0) return false;
            for (uint32_t i = 0; i < count; ++i) {
                if (!skipPayload(reader, list_tag, depth + 1, cancellation_requested, error)) return false;
            }
            return true;
        case Compound:
            while (true) {
                if (cancellationRequested(cancellation_requested, error)) return false;
                uint8_t child = 0; std::string ignored;
                if (!reader.u8(&child)) return false;
                if (child == End) return true;
                if (!reader.string(&ignored) ||
                    !skipPayload(reader, child, depth + 1, cancellation_requested, error)) return false;
            }
        default: return false;
    }
}

bool readNumber(Reader& reader, uint8_t tag, int32_t* value) {
    if (tag == Short) { uint16_t raw = 0; if (!reader.be16(&raw)) return false; *value = static_cast<int16_t>(raw); return true; }
    if (tag == Int) { uint32_t raw = 0; if (!reader.be32(&raw)) return false; *value = static_cast<int32_t>(raw); return true; }
    if (tag == Byte) { uint8_t raw = 0; if (!reader.u8(&raw)) return false; *value = static_cast<int8_t>(raw); return true; }
    return false;
}

bool readIntVector3(Reader& reader, std::array<int32_t, 3>* value) {
    uint32_t count = 0;
    if (!reader.be32(&count) || count != value->size()) return false;
    for (int32_t& component : *value) {
        uint32_t raw = 0;
        if (!reader.be32(&raw)) return false;
        component = static_cast<int32_t>(raw);
    }
    return true;
}

// Reads a root-extension byte array directly into a bounded string. Unlike
// the ordinary BlockData arrays, raw entity payloads are delivered to a
// consumer and must not be left in a temporary spool after parsing.
bool readRawByteArrayString(Reader& reader, std::string* output, size_t maximum,
                            const CancellationCallback& cancellation_requested,
                            std::string* error) {
    if (!output) return false;
    uint32_t length = 0;
    if (!reader.be32(&length) || static_cast<int32_t>(length) < 0) {
        if (error) *error = !reader.failure().empty() ? reader.failure()
            : "invalid Infinitecz raw byte-array length";
        return false;
    }
    if (static_cast<uint64_t>(length) > maximum) {
        if (error) *error = "Infinitecz raw byte-array exceeds the safety limit";
        return false;
    }
    try {
        output->assign(length, '\0');
    } catch (const std::bad_alloc&) {
        if (error) *error = "not enough memory for Infinitecz raw byte-array";
        return false;
    }
    size_t offset = 0;
    while (offset < output->size()) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        const size_t amount = std::min(output->size() - offset, reader.scratchSize());
        if (!reader.read(&(*output)[offset], amount)) {
            if (error) *error = !reader.failure().empty() ? reader.failure()
                : "truncated Infinitecz raw byte-array";
            return false;
        }
        offset += amount;
    }
    return true;
}

bool readNbtIntegral(Reader& reader, uint8_t tag, int64_t* value) {
    if (!value) return false;
    if (tag == Byte) {
        uint8_t raw = 0;
        if (!reader.u8(&raw)) return false;
        *value = static_cast<int8_t>(raw);
        return true;
    }
    if (tag == Short) {
        uint16_t raw = 0;
        if (!reader.be16(&raw)) return false;
        *value = static_cast<int16_t>(raw);
        return true;
    }
    if (tag == Int) {
        uint32_t raw = 0;
        if (!reader.be32(&raw)) return false;
        *value = static_cast<int32_t>(raw);
        return true;
    }
    if (tag == Long) {
        uint32_t high = 0;
        uint32_t low = 0;
        if (!reader.be32(&high) || !reader.be32(&low)) return false;
        const uint64_t raw = (static_cast<uint64_t>(high) << 32U) | low;
        *value = static_cast<int64_t>(raw);
        return true;
    }
    return false;
}

bool parseInfiniteczRawBlock(Reader& reader,
                             const CancellationCallback& cancellation_requested,
                             SchematicRawBlock* output, std::string* error) {
    if (!output) return false;
    *output = SchematicRawBlock{};
    bool has_x = false;
    bool has_y = false;
    bool has_z = false;
    bool has_identifier = false;
    bool has_aux = false;
    bool has_legacy_id = false;
    std::unordered_set<std::string> field_names;
    uint32_t field_count = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) break;
        if (++field_count > 64U || !reader.string(&name) ||
            !field_names.insert(name).second) {
            if (error) *error = "invalid or duplicate Infinitecz raw-state field";
            return false;
        }
        if (name == "x" || name == "y" || name == "z" || name == "LegacyId") {
            int64_t value = 0;
            if (!readNbtIntegral(reader, tag, &value) ||
                value < std::numeric_limits<int32_t>::min() ||
                value > std::numeric_limits<int32_t>::max()) {
                if (error) *error = "invalid Infinitecz raw-state integer";
                return false;
            }
            const int32_t converted = static_cast<int32_t>(value);
            if (name == "x") { output->x = converted; has_x = true; }
            else if (name == "y") { output->y = converted; has_y = true; }
            else if (name == "z") { output->z = converted; has_z = true; }
            else { output->legacy_id = converted; has_legacy_id = true; }
            continue;
        }
        if (name == "Aux") {
            // SchematicWriter emits Aux as a signed Short because NBT has no
            // unsigned scalar type. Read the bit pattern as unsigned here so
            // values 0x8000..0xffff survive the round trip unchanged.
            uint64_t value = 0;
            if (tag == Byte) {
                uint8_t raw = 0;
                if (!reader.u8(&raw)) value = UINT64_MAX;
                else value = raw;
            } else if (tag == Short) {
                uint16_t raw = 0;
                if (!reader.be16(&raw)) value = UINT64_MAX;
                else value = raw;
            } else if (tag == Int) {
                uint32_t raw = 0;
                if (!reader.be32(&raw)) value = UINT64_MAX;
                else value = raw;
            } else {
                value = UINT64_MAX;
            }
            if (value > UINT16_MAX) {
                if (error) *error = "invalid Infinitecz raw-state auxiliary value";
                return false;
            }
            output->aux = static_cast<uint16_t>(value);
            has_aux = true;
            continue;
        }
        if (name == "Identifier") {
            if (tag != String || !reader.string(&output->identifier,
                                                 kMaximumRawIdentifierBytes) ||
                output->identifier.empty() ||
                output->identifier.find('\0') != std::string::npos) {
                if (error) *error = "invalid Infinitecz raw-state identifier";
                return false;
            }
            has_identifier = true;
            continue;
        }
        if (name == "StateJson") {
            if (tag == String) {
                if (!reader.string(&output->state_json, kMaximumRawStateBytes)) {
                    if (error) *error = "invalid Infinitecz StateJson";
                    return false;
                }
            } else if (tag == ByteArray) {
                if (!readRawByteArrayString(reader, &output->state_json,
                                            kMaximumRawStateBytes,
                                            cancellation_requested, error)) return false;
            } else {
                if (error) *error = "invalid Infinitecz StateJson tag";
                return false;
            }
            continue;
        }
        if (name == "EntityJson") {
            if (tag == String) {
                if (!reader.string(&output->entity_json, kMaximumRawEntityBytes)) {
                    if (error) *error = "invalid Infinitecz EntityJson";
                    return false;
                }
            } else if (tag == ByteArray) {
                if (!readRawByteArrayString(reader, &output->entity_json,
                                            kMaximumRawEntityBytes,
                                            cancellation_requested, error)) return false;
            } else {
                if (error) *error = "invalid Infinitecz EntityJson tag";
                return false;
            }
            continue;
        }
        if (!skipPayload(reader, tag, 1, cancellation_requested, error)) {
            if (error && error->empty()) *error = "invalid Infinitecz raw-state payload";
            return false;
        }
    }
    if (!has_x || !has_y || !has_z || !has_identifier || !has_aux || !has_legacy_id) {
        if (error) *error = "Infinitecz raw-state record is missing a required field";
        return false;
    }
    return true;
}

bool parseInfiniteczRawBlockList(Reader& reader,
                                 const CancellationCallback& cancellation_requested,
                                 std::vector<SchematicRawBlock>* output,
                                 uint64_t* payload_bytes, std::string* error) {
    if (!output || !payload_bytes) return false;
    uint8_t element_tag = 0;
    uint32_t count = 0;
    if (!reader.u8(&element_tag) || !reader.be32(&count) ||
        static_cast<int32_t>(count) < 0 || element_tag != Compound ||
        count > kMaximumRawBlocks) {
        if (error) *error = "invalid Infinitecz RawBlocks list";
        return false;
    }
    try {
        output->reserve(output->size() + count);
    } catch (const std::bad_alloc&) {
        if (error) *error = "not enough memory for Infinitecz raw-state records";
        return false;
    }
    for (uint32_t index = 0; index < count; ++index) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        SchematicRawBlock record;
        if (!parseInfiniteczRawBlock(reader, cancellation_requested, &record, error)) return false;
        const uint64_t record_bytes = static_cast<uint64_t>(record.identifier.size()) +
            record.state_json.size() + record.entity_json.size();
        if (record_bytes > kMaximumRawPayloadBytes ||
            *payload_bytes > kMaximumRawPayloadBytes - record_bytes) {
            if (error) *error = "Infinitecz raw-state payload exceeds the safety limit";
            return false;
        }
        *payload_bytes += record_bytes;
        output->push_back(std::move(record));
    }
    return true;
}

bool parseInfiniteczCompound(Reader& reader,
                             const CancellationCallback& cancellation_requested,
                             std::vector<SchematicRawBlock>* raw_blocks,
                             uint64_t* payload_bytes, std::string* error) {
    if (!raw_blocks || !payload_bytes) return false;
    int64_t format_version = -1;
    bool has_format_version = false;
    bool has_raw_blocks = false;
    std::unordered_set<std::string> field_names;
    uint32_t field_count = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) break;
        if (++field_count > 16U || !reader.string(&name) ||
            !field_names.insert(name).second) {
            if (error) *error = "invalid or duplicate Infinitecz extension field";
            return false;
        }
        if (name == "FormatVersion") {
            if (!readNbtIntegral(reader, tag, &format_version) || format_version < 0) {
                if (error) *error = "invalid Infinitecz extension format version";
                return false;
            }
            has_format_version = true;
            continue;
        }
        if (name == "RawBlocks") {
            if (tag != List || has_raw_blocks ||
                !parseInfiniteczRawBlockList(reader, cancellation_requested, raw_blocks,
                                              payload_bytes, error)) return false;
            has_raw_blocks = true;
            continue;
        }
        if (!skipPayload(reader, tag, 1, cancellation_requested, error)) {
            if (error && error->empty()) *error = "invalid Infinitecz extension payload";
            return false;
        }
    }
    if (!has_format_version || format_version != 1) {
        if (error) *error = "unsupported Infinitecz extension format version";
        return false;
    }
    return true;
}

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

struct DeferredCommandBlocks {
    std::vector<CommandBlockRecord> records;
    // A generic legacy "Control" id has no reliable shell mode.  Keep the
    // distinction so the actual parsed block state can supply repeat/chain
    // mode after the normal voxel stream has routed.
    std::vector<uint8_t> explicit_modes;
    uint64_t text_bytes = 0;
};

bool appendDeferredCommandBlock(DeferredCommandBlocks* deferred,
                                 CommandBlockRecord&& record,
                                 bool explicit_mode,
                                 std::string* error) {
    if (!deferred) return false;
    const uint64_t text_bytes = static_cast<uint64_t>(record.command.size()) +
        record.last_output.size() + record.name.size() + record.filtered_name.size();
    if (deferred->records.size() >= kMaximumDeferredCommandBlocks ||
        text_bytes > kMaximumDeferredCommandPayloadBytes ||
        deferred->text_bytes > kMaximumDeferredCommandPayloadBytes - text_bytes) {
        if (error) *error = "schematic command-block payload exceeds the 64 MiB safety limit";
        return false;
    }
    deferred->text_bytes += text_bytes;
    deferred->records.push_back(std::move(record));
    deferred->explicit_modes.push_back(explicit_mode ? 1U : 0U);
    return true;
}

bool readNbtFloating(Reader& reader, uint8_t tag, double* value) {
    if (!value) return false;
    if (tag == Float) {
        uint32_t raw = 0;
        float decoded = 0.0F;
        if (!reader.be32(&raw)) return false;
        std::memcpy(&decoded, &raw, sizeof(decoded));
        *value = decoded;
        return true;
    }
    if (tag == Double) {
        uint32_t high = 0, low = 0;
        uint64_t raw = 0;
        double decoded = 0.0;
        if (!reader.be32(&high) || !reader.be32(&low)) return false;
        raw = (static_cast<uint64_t>(high) << 32U) | low;
        std::memcpy(&decoded, &raw, sizeof(decoded));
        *value = decoded;
        return true;
    }
    int64_t integral = 0;
    if (readNbtIntegral(reader, tag, &integral)) {
        *value = static_cast<double>(integral);
        return true;
    }
    return false;
}

bool readNbtNumericList(Reader& reader, size_t expected_count, std::vector<double>* output,
                        const CancellationCallback& cancellation_requested,
                        std::string* error) {
    if (!output) return false;
    uint8_t type = 0;
    uint32_t count = 0;
    if (!reader.u8(&type) || !reader.be32(&count) ||
        static_cast<int32_t>(count) < 0 || count != expected_count) return false;
    output->clear();
    output->reserve(expected_count);
    for (uint32_t index = 0; index < count; ++index) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        double value = 0.0;
        if (!readNbtFloating(reader, type, &value) || !std::isfinite(value)) return false;
        output->push_back(value);
    }
    return true;
}

std::string legacyEnchantmentIdentifier(int64_t id) {
    switch (id) {
        case 0: return "minecraft:protection";
        case 1: return "minecraft:fire_protection";
        case 2: return "minecraft:feather_falling";
        case 3: return "minecraft:blast_protection";
        case 4: return "minecraft:projectile_protection";
        case 5: return "minecraft:respiration";
        case 6: return "minecraft:aqua_affinity";
        case 7: return "minecraft:thorns";
        case 8: return "minecraft:depth_strider";
        case 9: return "minecraft:frost_walker";
        case 10: return "minecraft:binding_curse";
        case 16: return "minecraft:sharpness";
        case 17: return "minecraft:smite";
        case 18: return "minecraft:bane_of_arthropods";
        case 19: return "minecraft:knockback";
        case 20: return "minecraft:fire_aspect";
        case 21: return "minecraft:looting";
        case 32: return "minecraft:efficiency";
        case 33: return "minecraft:silk_touch";
        case 34: return "minecraft:unbreaking";
        case 35: return "minecraft:fortune";
        case 48: return "minecraft:power";
        case 49: return "minecraft:punch";
        case 50: return "minecraft:flame";
        case 51: return "minecraft:infinity";
        case 61: return "minecraft:luck_of_the_sea";
        case 62: return "minecraft:lure";
        case 65: return "minecraft:loyalty";
        case 66: return "minecraft:impaling";
        case 67: return "minecraft:riptide";
        case 68: return "minecraft:channeling";
        case 70: return "minecraft:mending";
        case 71: return "minecraft:vanishing_curse";
        default: return {};
    }
}

bool parseDeferredEnchantmentCompound(Reader& reader,
                                      const CancellationCallback& cancellation_requested,
                                      std::vector<DeferredEnchantment>* output,
                                      uint64_t* filtered_count, std::string* error) {
    std::string identifier;
    int64_t level = 0;
    bool has_id = false;
    bool has_level = false;
    uint32_t fields = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) break;
        if (++fields > 128 || !reader.string(&name)) return false;
        if ((name == "id" || name == "Id") && tag == String) {
            if (!reader.string(&identifier, 128)) return false;
            has_id = true;
            continue;
        }
        if ((name == "id" || name == "Id") && tag != String) {
            int64_t numeric_id = 0;
            if (readNbtIntegral(reader, tag, &numeric_id)) {
                identifier = legacyEnchantmentIdentifier(numeric_id);
                has_id = !identifier.empty();
                continue;
            }
        }
        if (name == "lvl" || name == "Level" || name == "level") {
            if (readNbtIntegral(reader, tag, &level)) {
                has_level = true;
                continue;
            }
        }
        if (!skipPayload(reader, tag, 2, cancellation_requested, error)) return false;
    }
    DeferredEnchantment enchantment;
    if (!has_id || !has_level || !normalizeDeferredEnchantment(&identifier, level, &enchantment)) {
        if (filtered_count) ++*filtered_count;
        return true;
    }
    if (output && output->size() < 32) output->push_back(std::move(enchantment));
    else if (filtered_count) ++*filtered_count;
    return true;
}

bool parseDeferredEnchantmentList(Reader& reader,
                                  const CancellationCallback& cancellation_requested,
                                  std::vector<DeferredEnchantment>* output,
                                  uint64_t* filtered_count, std::string* error) {
    uint8_t element_tag = 0;
    uint32_t count = 0;
    if (!reader.u8(&element_tag) || !reader.be32(&count) || static_cast<int32_t>(count) < 0 ||
        count > 256) return false;
    for (uint32_t index = 0; index < count; ++index) {
        if (element_tag == Compound) {
            if (!parseDeferredEnchantmentCompound(reader, cancellation_requested, output,
                                                  filtered_count, error)) return false;
        } else if (!skipPayload(reader, element_tag, 2, cancellation_requested, error)) {
            return false;
        }
    }
    return true;
}

bool parseDeferredEnchantLevelsCompound(Reader& reader,
                                         const CancellationCallback& cancellation_requested,
                                         std::vector<DeferredEnchantment>* output,
                                         uint64_t* filtered_count, std::string* error) {
    uint32_t fields = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string id;
        if (!reader.u8(&tag)) return false;
        if (tag == End) return true;
        if (++fields > 256 || !reader.string(&id, 128)) return false;
        int64_t level = 0;
        DeferredEnchantment enchantment;
        if (readNbtIntegral(reader, tag, &level) &&
            normalizeDeferredEnchantment(&id, level, &enchantment)) {
            if (output && output->size() < 32) output->push_back(std::move(enchantment));
            else if (filtered_count) ++*filtered_count;
            continue;
        }
        if (tag >= Byte && tag <= Long) {
            if (filtered_count) ++*filtered_count;
            continue;
        }
        if (!skipPayload(reader, tag, 2, cancellation_requested, error)) return false;
    }
}

bool parseDeferredItemTagCompound(Reader& reader,
                                  const CancellationCallback& cancellation_requested,
                                  std::vector<DeferredEnchantment>* enchantments,
                                  uint64_t* filtered_count, std::string* error) {
    uint32_t fields = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) return true;
        if (++fields > 1024 || !reader.string(&name)) return false;
        if ((name == "Enchantments" || name == "ench") && tag == List) {
            if (!parseDeferredEnchantmentList(reader, cancellation_requested, enchantments,
                                              filtered_count, error)) return false;
            continue;
        }
        if (!skipPayload(reader, tag, 3, cancellation_requested, error)) return false;
    }
}

bool parseDeferredItemComponentsCompound(Reader& reader,
                                         const CancellationCallback& cancellation_requested,
                                         std::vector<DeferredEnchantment>* enchantments,
                                         uint64_t* filtered_count, std::string* error) {
    uint32_t fields = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) return true;
        if (++fields > 1024 || !reader.string(&name)) return false;
        if (name == "minecraft:enchantments" && tag == Compound) {
            uint32_t inner_fields = 0;
            while (true) {
                uint8_t inner_tag = 0;
                std::string inner_name;
                if (!reader.u8(&inner_tag)) return false;
                if (inner_tag == End) break;
                if (++inner_fields > 128 || !reader.string(&inner_name)) return false;
                if (inner_name == "levels" && inner_tag == Compound) {
                    if (!parseDeferredEnchantLevelsCompound(reader, cancellation_requested,
                                                            enchantments, filtered_count, error)) return false;
                } else if (!skipPayload(reader, inner_tag, 4, cancellation_requested, error)) {
                    return false;
                }
            }
            continue;
        }
        if (!skipPayload(reader, tag, 3, cancellation_requested, error)) return false;
    }
}

bool parseDeferredContainerItemCompound(Reader& reader,
                                        const CancellationCallback& cancellation_requested,
                                        ContainerItemRecord* output,
                                        uint64_t* filtered_count,
                                        std::string* error) {
    if (!output) return false;
    ContainerItemRecord record;
    bool has_id = false, has_count = false, has_slot = false;
    uint32_t fields = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) break;
        if (++fields > 2048 || !reader.string(&name)) return false;
        if ((name == "id" || name == "Id" || name == "Name") && tag == String) {
            if (!reader.string(&record.item_id, 256)) return false;
            has_id = normalizeDeferredItemIdentifier(&record.item_id);
            continue;
        }
        if (name == "Count" || name == "count") {
            int64_t value = 0;
            if (readNbtIntegral(reader, tag, &value)) {
                has_count = value > 0 && value <= 64;
                if (has_count) record.count = static_cast<uint16_t>(value);
                continue;
            }
        }
        if (name == "Slot" || name == "slot") {
            int64_t value = 0;
            if (readNbtIntegral(reader, tag, &value)) {
                has_slot = value >= 0 && value <= 255;
                if (has_slot) record.slot = static_cast<uint16_t>(value);
                continue;
            }
        }
        if (name == "Damage" || name == "damage" || name == "aux" || name == "Aux") {
            int64_t value = 0;
            if (readNbtIntegral(reader, tag, &value)) {
                if (value >= 0 && value <= UINT16_MAX) record.aux = static_cast<uint16_t>(value);
                continue;
            }
        }
        if (name == "tag" && tag == Compound) {
            if (!parseDeferredItemTagCompound(reader, cancellation_requested, &record.enchantments,
                                              filtered_count, error)) return false;
            continue;
        }
        if (name == "components" && tag == Compound) {
            if (!parseDeferredItemComponentsCompound(reader, cancellation_requested,
                                                    &record.enchantments, filtered_count, error)) return false;
            continue;
        }
        if (!skipPayload(reader, tag, 2, cancellation_requested, error)) return false;
    }
    if (!has_id || !has_count || !has_slot) return true;
    *output = std::move(record);
    return true;
}

bool parseDeferredContainerItemList(Reader& reader,
                                    const CancellationCallback& cancellation_requested,
                                    std::vector<ContainerItemRecord>* output,
                                    uint64_t* filtered_count, std::string* error) {
    uint8_t element_tag = 0;
    uint32_t count = 0;
    if (!reader.u8(&element_tag) || !reader.be32(&count) || static_cast<int32_t>(count) < 0 ||
        count > 256) return false;
    for (uint32_t index = 0; index < count; ++index) {
        if (element_tag != Compound) {
            if (!skipPayload(reader, element_tag, 2, cancellation_requested, error)) return false;
            continue;
        }
        ContainerItemRecord record;
        if (!parseDeferredContainerItemCompound(reader, cancellation_requested, &record,
                                                filtered_count, error)) return false;
        if (!record.item_id.empty() && output) {
            if (output->size() >= ContainerItemSpoolWriter::kMaximumRecords) {
                if (error) *error = "schematic container-item payload exceeds the safety limit";
                return false;
            }
            output->push_back(std::move(record));
        }
    }
    return true;
}

bool parseCommandBlockEntityCompound(Reader& reader,
                                      const CancellationCallback& cancellation_requested,
                                      DeferredCommandBlocks* deferred,
                                      std::vector<ContainerItemRecord>* containers,
                                      uint64_t* filtered_enchantments,
                                      std::string* error) {
    CommandBlockRecord record;
    std::string identifier;
    bool has_mode = false;
    std::array<bool, 3> has_position{};
    std::vector<ContainerItemRecord> items;
    uint32_t field_count = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) break;
        if (++field_count > 4096 || !reader.string(&name)) return false;

        if ((name == "id" || name == "Id") && tag == String) {
            if (!reader.string(&identifier, kMaximumCommandBlockTextBytes)) return false;
            continue;
        }
        if ((name == "Command" || name == "CustomName" || name == "LastOutput") &&
            tag == String) {
            std::string* output = name == "Command" ? &record.command
                : name == "CustomName" ? &record.name : &record.last_output;
            if (!reader.string(output, kMaximumCommandBlockTextBytes)) return false;
            continue;
        }
        const int position_index = name == "x" ? 0 : name == "y" ? 1 : name == "z" ? 2 : -1;
        if (position_index >= 0) {
            int64_t value = 0;
            if (readNbtIntegral(reader, tag, &value)) {
                if (value >= std::numeric_limits<int32_t>::min() &&
                    value <= std::numeric_limits<int32_t>::max()) {
                    int32_t* coordinate = position_index == 0 ? &record.x
                        : position_index == 1 ? &record.y : &record.z;
                    *coordinate = static_cast<int32_t>(value);
                    has_position[static_cast<size_t>(position_index)] = true;
                }
                continue;
            }
        }
        if (name == "Pos" && tag == IntArray) {
            std::array<int32_t, 3> position{};
            if (!readIntVector3(reader, &position)) return false;
            record.x = position[0];
            record.y = position[1];
            record.z = position[2];
            has_position = {{true, true, true}};
            continue;
        }
        if ((name == "Items" || name == "items") && tag == List) {
            if (containers) {
                if (!parseDeferredContainerItemList(reader, cancellation_requested, &items,
                                                    filtered_enchantments, error)) return false;
            } else if (!skipPayload(reader, tag, 1, cancellation_requested, error)) {
                return false;
            }
            continue;
        }
        if (name == "TrackOutput" || name == "auto" || name == "conditionalMode" ||
            name == "ExecuteOnFirstTick" || name == "TickDelay" ||
            name == "LPCommandMode" || name == "CommandBlockMode") {
            int64_t value = 0;
            if (readNbtIntegral(reader, tag, &value)) {
                if (name == "TrackOutput") record.output_tracked = value != 0;
                else if (name == "auto") record.redstone_mode = value == 0;
                else if (name == "conditionalMode") record.conditional = value != 0;
                else if (name == "ExecuteOnFirstTick") record.executing_on_first_tick = value != 0;
                else if (name == "TickDelay") {
                    if (value >= std::numeric_limits<int32_t>::min() &&
                        value <= std::numeric_limits<int32_t>::max()) {
                        record.tick_delay = static_cast<int32_t>(value);
                    }
                } else if (value >= 0 && value <= UINT16_MAX) {
                    record.mode = static_cast<uint16_t>(value);
                    has_mode = true;
                }
                continue;
            }
        }
        if (!skipPayload(reader, tag, 1, cancellation_requested, error)) return false;
    }

    // A default/empty command block can legally omit its Command string.  The
    // final shell lookup below is the authority that prevents arbitrary block
    // entity NBT from becoming an editor packet, so retain a positioned
    // command-block candidate even when its command text is empty.
    if (has_position[0] && has_position[1] && has_position[2] && containers) {
        for (ContainerItemRecord& item : items) {
            item.x = record.x;
            item.y = record.y;
            item.z = record.z;
            if (containers->size() >= ContainerItemSpoolWriter::kMaximumRecords) {
                if (error) *error = "schematic container-item payload exceeds the safety limit";
                return false;
            }
            containers->push_back(std::move(item));
        }
    }
    if (!has_position[0] || !has_position[1] || !has_position[2] ||
        (!identifier.empty() && !isCommandBlockEntityIdentifier(identifier))) return true;
    if (!has_mode) record.mode = commandBlockModeForIdentifier(identifier);
    return appendDeferredCommandBlock(deferred, std::move(record), has_mode, error);
}

bool parseCommandBlockEntityList(Reader& reader,
                                  const CancellationCallback& cancellation_requested,
                                  DeferredCommandBlocks* deferred,
                                  std::vector<ContainerItemRecord>* containers,
                                  uint64_t* filtered_enchantments,
                                  std::string* error) {
    uint8_t element_tag = 0;
    uint32_t count = 0;
    if (!reader.u8(&element_tag) || !reader.be32(&count) ||
        static_cast<int32_t>(count) < 0) return false;
    // A list that is not compounds cannot express a block entity.  Consume it
    // normally instead of rejecting a schematic merely for extra metadata.
    if (element_tag != Compound) {
        for (uint32_t index = 0; index < count; ++index) {
            if (!skipPayload(reader, element_tag, 1, cancellation_requested, error)) return false;
        }
        return true;
    }
    for (uint32_t index = 0; index < count; ++index) {
        if (index >= kMaximumDeferredCommandBlocks ||
            !parseCommandBlockEntityCompound(reader, cancellation_requested, deferred, containers,
                                             filtered_enchantments, error)) {
            if (error && error->empty()) *error = "schematic command-block entity list exceeds safety limits";
            return false;
        }
    }
    return true;
}

bool flushDeferredCommandBlocks(const SchematicParseOptions& options,
                                 const std::array<int64_t, 3>& origin,
                                 int32_t width, int32_t height, int32_t length,
                                 const std::unordered_map<uint64_t, CommandBlockShellState>& command_block_modes,
                                 DeferredCommandBlocks* deferred,
                                 SchematicParseResult* result,
                                 std::string* error) {
    if (!deferred || !result) return false;
    if (!options.command_block_sink) return true;
    if (width <= 0 || height <= 0 || length <= 0) {
        if (error) *error = "schematic command-block dimensions are invalid";
        return false;
    }
    const uint64_t layer_area = static_cast<uint64_t>(width) *
        static_cast<uint64_t>(length);
    for (size_t record_index = 0; record_index < deferred->records.size(); ++record_index) {
        CommandBlockRecord& record = deferred->records[record_index];
        // Source block-entity lists can contain stale records after an editor
        // replaces a command block.  Only emit an update when the routed shell
        // at the same local position is definitely a command block.  This is
        // especially important for generic legacy `Control` entries, whose NBT
        // alone cannot prove which block now occupies the coordinate.
        if (record.x < 0 || record.y < 0 || record.z < 0 ||
            record.x >= width || record.y >= height || record.z >= length) {
            ++result->omitted_command_block_data_count;
            continue;
        }
        const uint64_t local_index = static_cast<uint64_t>(record.x) +
            static_cast<uint64_t>(record.z) * static_cast<uint64_t>(width) +
            static_cast<uint64_t>(record.y) * layer_area;
        const auto shell = command_block_modes.find(local_index);
        if (shell == command_block_modes.end()) {
            ++result->omitted_command_block_data_count;
            continue;
        }
        // A block entity can be stale or omit conditionalMode, but the routed
        // shell is what was actually placed.  Keep both packet-facing shell
        // properties synchronized so the later editor update cannot turn a
        // conditional block into an unconditional one.
        applyCommandBlockShellState(&record, shell->second);
        if (!isValidCommandBlockMode(record.mode)) {
            if (error) *error = "schematic command-block mode is invalid";
            return false;
        }
        const std::array<int64_t, 3> world{{
            origin[0] + record.x, origin[1] + record.y, origin[2] + record.z}};
        if (world[0] < std::numeric_limits<int32_t>::min() ||
            world[0] > std::numeric_limits<int32_t>::max() ||
            world[1] < std::numeric_limits<int32_t>::min() ||
            world[1] > std::numeric_limits<int32_t>::max() ||
            world[2] < std::numeric_limits<int32_t>::min() ||
            world[2] > std::numeric_limits<int32_t>::max()) {
            if (error) *error = "schematic command-block coordinates exceed the supported world range";
            return false;
        }
        record.x = static_cast<int32_t>(world[0]);
        record.y = static_cast<int32_t>(world[1]);
        record.z = static_cast<int32_t>(world[2]);
        if (!options.command_block_sink(record, error)) {
            if (error && error->empty()) *error = "schematic command-block sink rejected a payload";
            return false;
        }
        ++result->command_block_payload_count;
    }
    deferred->records.clear();
    deferred->explicit_modes.clear();
    deferred->text_bytes = 0;
    return true;
}

bool flushDeferredContainerItems(
    const SchematicParseOptions& options, const std::array<int64_t, 3>& origin,
    int32_t width, int32_t height, int32_t length,
    const std::unordered_map<uint64_t, std::string>& container_shells,
    std::vector<ContainerItemRecord>* deferred, SchematicParseResult* result,
    std::string* error) {
    if (!deferred || !result) return false;
    if (!options.container_item_sink) return true;
    const uint64_t layer_area = static_cast<uint64_t>(width) * static_cast<uint64_t>(length);
    for (ContainerItemRecord& item : *deferred) {
        if (item.x < 0 || item.y < 0 || item.z < 0 || item.x >= width || item.y >= height ||
            item.z >= length) {
            ++result->omitted_block_entity_count;
            continue;
        }
        const uint64_t local = static_cast<uint64_t>(item.x) +
            static_cast<uint64_t>(item.z) * static_cast<uint64_t>(width) +
            static_cast<uint64_t>(item.y) * layer_area;
        const auto shell = container_shells.find(local);
        if (shell == container_shells.end()) {
            ++result->omitted_block_entity_count;
            continue;
        }
        const std::array<int64_t, 3> world{{origin[0] + item.x, origin[1] + item.y,
                                              origin[2] + item.z}};
        if (world[0] < std::numeric_limits<int32_t>::min() ||
            world[0] > std::numeric_limits<int32_t>::max() ||
            world[1] < std::numeric_limits<int32_t>::min() ||
            world[1] > std::numeric_limits<int32_t>::max() ||
            world[2] < std::numeric_limits<int32_t>::min() ||
            world[2] > std::numeric_limits<int32_t>::max()) {
            if (error) *error = "schematic container coordinates exceed the supported world range";
            return false;
        }
        item.x = static_cast<int32_t>(world[0]);
        item.y = static_cast<int32_t>(world[1]);
        item.z = static_cast<int32_t>(world[2]);
        item.expected_container_id = shell->second;
        if (!options.container_item_sink(item, error)) {
            if (error && error->empty()) *error = "schematic container-item sink rejected a payload";
            return false;
        }
        ++result->container_item_payload_count;
    }
    deferred->clear();
    return true;
}

bool parseDeferredEntityCompound(Reader& reader,
                                 const CancellationCallback& cancellation_requested,
                                 std::vector<EntityRecord>* output,
                                 uint64_t* omitted_count, std::string* error) {
    EntityRecord entity;
    bool has_id = false;
    bool has_position = false;
    uint32_t fields = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) break;
        if (++fields > 4096 || !reader.string(&name)) return false;
        if ((name == "id" || name == "Id") && tag == String) {
            if (!reader.string(&entity.entity_id, 256)) return false;
            has_id = normalizeDeferredEntityIdentifier(&entity.entity_id);
            continue;
        }
        if (name == "Pos" && tag == List) {
            std::vector<double> position;
            if (!readNbtNumericList(reader, 3, &position, cancellation_requested, error)) return false;
            entity.x = position[0]; entity.y = position[1]; entity.z = position[2];
            has_position = true;
            continue;
        }
        if (name == "Rotation" && tag == List) {
            std::vector<double> rotation;
            if (!readNbtNumericList(reader, 2, &rotation, cancellation_requested, error)) return false;
            entity.yaw = static_cast<float>(rotation[0]);
            entity.pitch = static_cast<float>(rotation[1]);
            continue;
        }
        if ((name == "CustomName" || name == "custom_name") && tag == String) {
            if (!reader.string(&entity.custom_name, 1024) ||
                !normalizeDeferredEntityName(&entity.custom_name)) return false;
            continue;
        }
        if (!skipPayload(reader, tag, 1, cancellation_requested, error)) return false;
    }
    if (!has_id || !has_position || !std::isfinite(entity.x) || !std::isfinite(entity.y) ||
        !std::isfinite(entity.z) || !std::isfinite(entity.yaw) || !std::isfinite(entity.pitch)) {
        if (omitted_count) ++*omitted_count;
        return true;
    }
    if (output) {
        if (output->size() >= EntitySpoolWriter::kMaximumRecords) {
            if (error) *error = "schematic entity payload exceeds the safety limit";
            return false;
        }
        output->push_back(std::move(entity));
    }
    return true;
}

bool parseDeferredEntityList(Reader& reader,
                             const CancellationCallback& cancellation_requested,
                             std::vector<EntityRecord>* output,
                             uint64_t* omitted_count, std::string* error) {
    uint8_t element_tag = 0;
    uint32_t count = 0;
    if (!reader.u8(&element_tag) || !reader.be32(&count) || static_cast<int32_t>(count) < 0 ||
        count > EntitySpoolWriter::kMaximumRecords) return false;
    for (uint32_t index = 0; index < count; ++index) {
        if (element_tag == Compound) {
            if (!parseDeferredEntityCompound(reader, cancellation_requested, output,
                                             omitted_count, error)) return false;
        } else if (!skipPayload(reader, element_tag, 1, cancellation_requested, error)) {
            return false;
        }
    }
    return true;
}

bool flushDeferredEntities(const SchematicParseOptions& options,
                           const std::array<int64_t, 3>& origin,
                           std::vector<EntityRecord>* deferred,
                           SchematicParseResult* result, std::string* error) {
    if (!deferred || !result) return false;
    if (!options.entity_sink) return true;
    constexpr double kMaximumWorldCoordinate = 30000000.0;
    for (EntityRecord& entity : *deferred) {
        entity.x += static_cast<double>(origin[0]);
        entity.y += static_cast<double>(origin[1]);
        entity.z += static_cast<double>(origin[2]);
        if (!std::isfinite(entity.x) || !std::isfinite(entity.y) || !std::isfinite(entity.z) ||
            std::abs(entity.x) > kMaximumWorldCoordinate ||
            std::abs(entity.y) > kMaximumWorldCoordinate ||
            std::abs(entity.z) > kMaximumWorldCoordinate) {
            ++result->omitted_entity_count;
            continue;
        }
        if (!options.entity_sink(entity, error)) {
            if (error && error->empty()) *error = "schematic entity sink rejected a payload";
            return false;
        }
        ++result->entity_payload_count;
    }
    deferred->clear();
    return true;
}

bool flushInfiniteczRawBlocks(const SchematicParseOptions& options,
                              const std::array<int64_t, 3>& origin,
                              int32_t width, int32_t height, int32_t length,
                              std::vector<SchematicRawBlock>* deferred,
                              uint64_t payload_bytes,
                              SchematicParseResult* result, std::string* error) {
    if (!deferred || !result) return false;
    if (deferred->size() > kMaximumRawBlocks || payload_bytes > kMaximumRawPayloadBytes) {
        if (error) *error = "Infinitecz raw-state payload exceeds the safety limit";
        return false;
    }
    const uint64_t layer_area = static_cast<uint64_t>(width) *
        static_cast<uint64_t>(length);
    std::unordered_set<uint64_t> coordinates;
    try {
        coordinates.reserve(deferred->size());
    } catch (const std::bad_alloc&) {
        if (error) *error = "not enough memory to validate Infinitecz raw-state coordinates";
        return false;
    }
    // Validate the complete set before invoking a sink. This prevents a
    // duplicate or out-of-range record near the end from leaving a partially
    // applied raw-state stream.
    for (const SchematicRawBlock& record : *deferred) {
        if (record.x < 0 || record.x >= width || record.y < 0 || record.y >= height ||
            record.z < 0 || record.z >= length || record.identifier.empty() ||
            record.identifier.size() > kMaximumRawIdentifierBytes ||
            record.identifier.find('\0') != std::string::npos) {
            if (error) *error = "Infinitecz raw-state coordinate or identifier is invalid";
            return false;
        }
        const uint64_t local = static_cast<uint64_t>(record.x) +
            static_cast<uint64_t>(record.z) * static_cast<uint64_t>(width) +
            static_cast<uint64_t>(record.y) * layer_area;
        if (!coordinates.emplace(local).second) {
            if (error) *error = "Infinitecz raw-state records contain duplicate coordinates";
            return false;
        }
        const std::array<int64_t, 3> world{{origin[0] + record.x,
                                             origin[1] + record.y,
                                             origin[2] + record.z}};
        if (world[0] < std::numeric_limits<int32_t>::min() ||
            world[0] > std::numeric_limits<int32_t>::max() ||
            world[1] < std::numeric_limits<int32_t>::min() ||
            world[1] > std::numeric_limits<int32_t>::max() ||
            world[2] < std::numeric_limits<int32_t>::min() ||
            world[2] > std::numeric_limits<int32_t>::max()) {
            if (error) *error = "Infinitecz raw-state coordinates exceed the supported world range";
            return false;
        }
    }
    if (!options.raw_block_sink) {
        try {
            result->raw_blocks.reserve(result->raw_blocks.size() + deferred->size());
        } catch (const std::bad_alloc&) {
            if (error) *error = "not enough memory for parsed Infinitecz raw-state records";
            return false;
        }
    }
    for (SchematicRawBlock& record : *deferred) {
        record.x = static_cast<int32_t>(origin[0] + record.x);
        record.y = static_cast<int32_t>(origin[1] + record.y);
        record.z = static_cast<int32_t>(origin[2] + record.z);
        if (options.raw_block_sink) {
            if (!options.raw_block_sink(record, error)) {
                if (error && error->empty()) *error = "Infinitecz raw-state sink rejected a payload";
                return false;
            }
        } else {
            result->raw_blocks.push_back(std::move(record));
        }
        ++result->raw_block_payload_count;
    }
    deferred->clear();
    return true;
}

bool parsePalette(Reader& reader, std::vector<std::string>* palette,
                   const CancellationCallback& cancellation_requested, std::string* error) {
    std::vector<bool> occupied;
    std::unordered_set<std::string> state_names;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0; std::string state; int32_t id = 0;
        if (!reader.u8(&tag)) return false;
        if (tag == End) return true;
        if (!reader.string(&state) || state.empty() || !state_names.insert(state).second) return false;
        if (!readNumber(reader, tag, &id) || id < 0 || id > 65535) return false;
        if (occupied.size() <= static_cast<size_t>(id)) occupied.resize(static_cast<size_t>(id) + 1);
        if (occupied[id]) return false;
        occupied[id] = true;
        if (palette->size() <= static_cast<size_t>(id)) palette->resize(static_cast<size_t>(id) + 1);
        (*palette)[id] = std::move(state);
    }
}

bool parseSpongeBlocks(Reader& reader, std::vector<std::string>* palette,
                       ByteArrayStorage* block_data, bool* has_palette,
                       bool* has_block_data, size_t memory_budget_bytes,
                       const CancellationCallback& cancellation_requested,
                       const ArrayProgressCallback& progress_callback,
                       std::string* error) {
    std::unordered_set<std::string> seen_names;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) return true;
        if (!reader.string(&name) || !seen_names.insert(name).second) return false;
        if (name == "Palette") {
            if (tag != Compound || *has_palette) return false;
            if (!parsePalette(reader, palette, cancellation_requested, error)) return false;
            *has_palette = true;
            continue;
        }
        if (name == "Data") {
            if (tag != ByteArray || *has_block_data ||
                !readBoundedByteArray(reader, block_data, memory_budget_bytes,
                                      cancellation_requested, progress_callback,
                                      error)) return false;
            *has_block_data = true;
            continue;
        }
        if (!skipPayload(reader, tag, 1, cancellation_requested, error)) return false;
    }
}

class BufferedByteReader {
public:
    explicit BufferedByteReader(const std::string& path)
        : buffer_(kStreamingBufferSize) {
        if (!path.empty()) stream_.open(path, std::ios::binary);
    }

    explicit BufferedByteReader(const ByteArrayStorage& storage)
        : buffer_(kStreamingBufferSize) {
        if (storage.in_memory) memory_ = &storage.memory;
        else if (!storage.spool_path.empty()) {
            stream_.open(storage.spool_path, std::ios::binary);
        }
    }

    bool valid() const { return memory_ != nullptr || stream_.is_open(); }

    bool read(uint8_t* output) {
        if (memory_) {
            if (memory_position_ >= memory_->size()) return false;
            *output = (*memory_)[memory_position_++];
            return true;
        }
        if (position_ == available_ && !refill()) return false;
        *output = buffer_[position_++];
        return true;
    }

    bool failed() const { return failed_; }

private:
    bool refill() {
        if (at_end_ || failed_) return false;
        stream_.read(reinterpret_cast<char*>(buffer_.data()),
                     static_cast<std::streamsize>(buffer_.size()));
        const std::streamsize count = stream_.gcount();
        position_ = 0;
        available_ = count > 0 ? static_cast<size_t>(count) : 0;
        if (available_ != 0) return true;
        at_end_ = stream_.eof();
        failed_ = stream_.bad() || (!at_end_ && stream_.fail());
        return false;
    }

    std::ifstream stream_;
    const std::vector<uint8_t>* memory_ = nullptr;
    size_t memory_position_ = 0;
    std::vector<uint8_t> buffer_;
    size_t position_ = 0;
    size_t available_ = 0;
    bool at_end_ = false;
    bool failed_ = false;
};

// Presents one root-level NBT byte-array payload as the same one-byte reader
// interface as BufferedByteReader. It lets a replayed gzip stream feed the
// Sponge VarInt decoder without materialising `_blockdata.raw` first.
class BoundedByteReader {
public:
    BoundedByteReader(Reader* reader, uint64_t remaining)
        : reader_(reader), remaining_(remaining) {}

    bool read(uint8_t* output) {
        if (position_ == available_) {
            if (remaining_ == 0) return false;
            if (!reader_) {
                failed_ = true;
                return false;
            }
            const size_t amount = static_cast<size_t>(std::min<uint64_t>(
                remaining_, reader_->scratchSize()));
            if (!reader_->read(reader_->scratchData(), amount)) {
                failed_ = true;
                return false;
            }
            position_ = 0;
            available_ = amount;
        }
        *output = reader_->scratchData()[position_++];
        --remaining_;
        return true;
    }

    bool failed() const { return failed_; }
    bool exhausted() const { return remaining_ == 0; }

private:
    Reader* reader_ = nullptr;
    uint64_t remaining_ = 0;
    size_t position_ = 0;
    size_t available_ = 0;
    bool failed_ = false;
};

int createDirectory(const char* path) {
#if defined(_WIN32)
    return ::mkdir(path);
#else
    return ::mkdir(path, 0700);
#endif
}

bool ensureDirectory(const std::string& directory) {
    if (directory.empty()) return false;
    std::string partial;
    for (size_t index = 0; index <= directory.size(); ++index) {
        if (index != directory.size() && directory[index] != '/' && directory[index] != '\\') {
            partial += directory[index];
            continue;
        }
        if (!partial.empty() && partial != "." &&
            createDirectory(partial.c_str()) != 0 && errno != EEXIST) return false;
        if (index != directory.size()) {
            if (partial.empty() && directory[index] == '/') partial = "/";
            else if (!partial.empty() && partial.back() != '/') partial += '/';
        }
    }
    return true;
}

}  // namespace

bool SchematicParser::parse(const SchematicParseOptions& options,
                            const BlockMapper& mapper,
                            SchematicParseResult* result,
                            std::string* error) const {
    SchematicParseOptions resolved_options = options;
    if (!resolved_options.legacy_block_resolver) {
        resolved_options.legacy_block_resolver = [&mapper](uint16_t id, uint8_t data) {
            return mapper.mapLegacy(id, data);
        };
    }
    if (!resolved_options.state_block_resolver) {
        resolved_options.state_block_resolver = [&mapper](std::string_view state) {
            return mapper.mapSpongeState(state);
        };
    }
    return parse(resolved_options, result, error);
}

bool SchematicParser::parse(const SchematicParseOptions& options,
                            SchematicParseResult* result, std::string* error) const {
    if (error) error->clear();
    if (!result || options.source_path.empty() || options.spool_directory.empty() || options.chunk_size <= 0) { if (error) *error = "invalid parse options"; return false; }
    *result = {};
    if (cancellationRequested(options.cancellation_requested, error)) return false;
    reportProgress(options, SchematicParseStage::ReadingSource);
    if (!ensureDirectory(options.spool_directory)) { if (error) *error = "cannot create spool directory"; return false; }
    Reader reader(options.source_path);
    if (!reader.valid()) { if (error) *error = "cannot open schematic"; return false; }
    const ArrayProgressCallback reading_progress = [&](uint64_t completed, uint64_t total) {
        reportProgress(options, SchematicParseStage::ReadingSource, completed, total);
    };
    uint8_t root = 0;
    std::string root_name;
    if (!reader.u8(&root)) {
        if (error) *error = !reader.failure().empty()
            ? reader.failure() : "truncated NBT root";
        return false;
    }
    if (root != Compound) {
        if (error) *error = "NBT root is not a compound";
        return false;
    }
    if (!reader.string(&root_name)) {
        if (error) *error = !reader.failure().empty()
            ? reader.failure() : "truncated NBT root name";
        return false;
    }
    int32_t width = -1, height = -1, length = -1;
    int32_t palette_max = -1;
    std::array<int32_t, 3> sponge_offset{};
    std::array<int32_t, 3> we_offset{};
    std::array<bool, 3> has_we_offset{};
    bool has_sponge_offset = false;
    std::vector<std::string> palette;
    const std::string blocks_path = options.spool_directory + "/_blocks.raw";
    const std::string data_path = options.spool_directory + "/_data.raw";
    const std::string add_path = options.spool_directory + "/_add.raw";
    const std::string block_data_path = options.spool_directory + "/_blockdata.raw";
    ByteArrayStorage block_data(block_data_path);
    struct RawArrayCleanup {
        std::array<std::string, 4> paths;
        ~RawArrayCleanup() {
            for (const std::string& path : paths) std::remove(path.c_str());
        }
    } raw_cleanup{{blocks_path, data_path, add_path, block_data_path}};
    bool has_width = false, has_height = false, has_length = false;
    bool has_palette_max = false;
    bool has_blocks = false, has_data = false, has_add = false;
    bool has_palette = false, has_block_data = false, has_sponge_blocks = false;
    bool deferred_sponge_block_data = false;
    uint32_t blocks_length = 0, data_length = 0, add_length = 0, block_data_length = 0;
    DeferredCommandBlocks deferred_command_blocks;
    std::vector<ContainerItemRecord> deferred_container_items;
    std::vector<EntityRecord> deferred_entities;
    std::vector<SchematicRawBlock> deferred_raw_blocks;
    uint64_t raw_payload_bytes = 0;
    bool has_infinitecz_extension = false;
    std::unordered_set<std::string> root_names;
    while (true) {
        if (cancellationRequested(options.cancellation_requested, error)) return false;
        uint8_t tag = 0; std::string name;
        if (!reader.u8(&tag)) {
            if (error) *error = !reader.failure().empty()
                ? reader.failure() : "unexpected NBT end";
            return false;
        }
        if (tag == End) break;
        if (!reader.string(&name) || !root_names.insert(name).second) {
            if (error) *error = !reader.failure().empty()
                ? reader.failure() : "invalid or duplicate root NBT tag";
            return false;
        }
        int32_t* dimension = name == "Width" ? &width : name == "Height" ? &height : name == "Length" ? &length : nullptr;
        if (dimension) {
            bool* seen = name == "Width" ? &has_width : name == "Height" ? &has_height : &has_length;
            if (*seen || !readNumber(reader, tag, dimension)) {
                if (error) *error = "invalid or duplicate schematic dimension";
                return false;
            }
            *seen = true;
            continue;
        }
        if (name == "Offset") {
            if (tag != IntArray || has_sponge_offset || !readIntVector3(reader, &sponge_offset)) {
                if (error) *error = "invalid or duplicate Sponge Offset";
                return false;
            }
            has_sponge_offset = true;
            continue;
        }
        const int offset_index = name == "WEOffsetX" ? 0 :
                                 name == "WEOffsetY" ? 1 :
                                 name == "WEOffsetZ" ? 2 : -1;
        if (offset_index >= 0) {
            if (has_we_offset[static_cast<size_t>(offset_index)] ||
                !readNumber(reader, tag, &we_offset[static_cast<size_t>(offset_index)])) {
                if (error) *error = "invalid or duplicate MCEdit WEOffset";
                return false;
            }
            has_we_offset[static_cast<size_t>(offset_index)] = true;
            continue;
        }
        if (name == "PaletteMax") {
            if (has_palette_max || !readNumber(reader, tag, &palette_max) ||
                palette_max <= 0) {
                if (error) *error = "invalid or duplicate schematic PaletteMax";
                return false;
            }
            has_palette_max = true;
            continue;
        }
        if (name == "Palette") {
            if (tag != Compound || has_palette ||
                !parsePalette(reader, &palette, options.cancellation_requested, error)) {
                if (error && error->empty()) *error = "invalid or duplicate Sponge palette";
                return false;
            }
            has_palette = true;
            continue;
        }
        if (name == "Infinitecz") {
            if (tag != Compound || has_infinitecz_extension ||
                !parseInfiniteczCompound(reader, options.cancellation_requested,
                                         &deferred_raw_blocks, &raw_payload_bytes, error)) {
                if (error && error->empty()) *error = "invalid Infinitecz schematic extension";
                return false;
            }
            has_infinitecz_extension = true;
            continue;
        }
        if (name == "Blocks" && tag == Compound) {
            if (has_blocks || has_sponge_blocks ||
                !parseSpongeBlocks(reader, &palette, &block_data, &has_palette,
                                   &has_block_data,
                                   options.block_data_memory_budget_bytes,
                                   options.cancellation_requested, reading_progress,
                                   error)) {
                if (error && error->empty()) *error = "invalid Sponge v3 Blocks compound";
                return false;
            }
            has_sponge_blocks = true;
            block_data_length = block_data.length;
            continue;
        }
        if (name == "Blocks") {
            if (tag != ByteArray || has_blocks || has_sponge_blocks ||
                !copyByteArray(reader, blocks_path, "MCEdit Blocks", &blocks_length,
                               options.cancellation_requested, reading_progress,
                               error)) {
                if (error && error->empty()) *error = "invalid or duplicate MCEdit Blocks array";
                return false;
            }
            has_blocks = true;
            continue;
        }
        if (name == "BlockData") {
            if (tag != ByteArray || has_block_data) {
                if (error && error->empty()) *error = "invalid or duplicate schematic byte array";
                return false;
            }
            uint32_t declared_length = 0;
            if (!reader.be32(&declared_length) ||
                static_cast<int32_t>(declared_length) < 0) {
                if (error) *error = !reader.failure().empty() ? reader.failure()
                    : "invalid Sponge BlockData array length";
                return false;
            }
            const bool defer_to_replay =
                options.replay_streaming_block_data_threshold_bytes != 0 &&
                static_cast<uint64_t>(declared_length) >=
                    options.replay_streaming_block_data_threshold_bytes;
            const bool read = defer_to_replay
                ? skipByteArrayPayload(reader, declared_length, "Sponge BlockData",
                                       options.cancellation_requested,
                                       reading_progress, error)
                : readBoundedByteArrayPayload(reader, declared_length, &block_data,
                                              options.block_data_memory_budget_bytes,
                                              options.cancellation_requested,
                                              reading_progress, error);
            if (!read) {
                if (error && error->empty()) *error = "invalid or duplicate schematic byte array";
                return false;
            }
            if (defer_to_replay) {
                block_data.length = declared_length;
                block_data.memory.clear();
                block_data.in_memory = false;
                deferred_sponge_block_data = true;
            }
            block_data_length = declared_length;
            has_block_data = true;
            continue;
        }
        if (name == "Data" || name == "AddBlocks") {
            bool* seen = name == "Data" ? &has_data : &has_add;
            uint32_t* length = name == "Data" ? &data_length : &add_length;
            const std::string& path = name == "Data" ? data_path : add_path;
            if (tag != ByteArray || *seen ||
                !copyByteArray(reader, path, name, length,
                               options.cancellation_requested,
                               reading_progress, error)) {
                if (error && error->empty()) *error = "invalid or duplicate schematic byte array";
                return false;
            }
            *seen = true;
            continue;
        }
        if ((name == "TileEntities" || name == "BlockEntities") && tag == List &&
            (options.command_block_sink || options.container_item_sink)) {
            if (!parseCommandBlockEntityList(reader, options.cancellation_requested,
                                              &deferred_command_blocks,
                                              options.container_item_sink ? &deferred_container_items : nullptr,
                                              &result->filtered_enchantment_count, error)) {
                if (error && error->empty()) *error = "invalid schematic block-entity list";
                return false;
            }
            continue;
        }
        if (name == "Entities" && tag == List && options.entity_sink) {
            if (!parseDeferredEntityList(reader, options.cancellation_requested,
                                         &deferred_entities, &result->omitted_entity_count,
                                         error)) {
                if (error && error->empty()) *error = "invalid schematic entity list";
                return false;
            }
            continue;
        }
        if (!skipPayload(reader, tag, 0, options.cancellation_requested, error)) {
            if (error && error->empty()) *error = "unsupported or corrupt NBT payload";
            return false;
        }
    }
    if (!reader.exhausted()) {
        if (error) *error = !reader.failure().empty()
            ? reader.failure() : "trailing or corrupt data after NBT root";
        return false;
    }
    if (!has_width || !has_height || !has_length || width <= 0 || height <= 0 || length <= 0) {
        if (error) *error = "invalid schematic dimensions";
        return false;
    }
    const int64_t layer_area = static_cast<int64_t>(width) * length;
    if (layer_area <= 0 || layer_area > std::numeric_limits<int64_t>::max() / height) {
        if (error) *error = "invalid schematic dimensions";
        return false;
    }
    const int64_t count = layer_area * height;
    if (count > std::numeric_limits<int32_t>::max()) {
        if (error) *error = "schematic exceeds the NBT byte-array address range";
        return false;
    }
    result->source_voxel_count = count;
    // Some WorldEdit exports carry a modern Palette together with legacy-named
    // Blocks/Data arrays. In that bridge dialect, every byte in Blocks is a
    // direct palette index and Data is stale legacy metadata. Require the
    // matching PaletteMax signature so a genuinely mixed or corrupt file is
    // not silently interpreted as palette-backed.
    const bool palette_indexed_blocks =
        has_blocks && has_data && has_palette && has_palette_max &&
        palette_max == static_cast<int32_t>(palette.size()) &&
        !has_sponge_blocks && !has_block_data && !has_add;
    const bool mcedit_format = has_blocks && !palette_indexed_blocks;
    const bool sponge_format = has_palette && has_block_data;
    const bool palette_format = sponge_format || palette_indexed_blocks;
    if (mcedit_format && (has_sponge_blocks || has_palette || has_block_data)) {
        if (error) *error = "schematic mixes MCEdit and Sponge block storage";
        return false;
    }
    if (sponge_format && (has_blocks || has_data || has_add)) {
        if (error) *error = "schematic mixes Sponge and MCEdit block arrays";
        return false;
    }
    if (!mcedit_format && !palette_format) {
        if (error) *error = "unsupported schematic: expected MCEdit or Sponge block data";
        return false;
    }
    if (mcedit_format &&
        (blocks_length != static_cast<uint64_t>(count) ||
         (has_data && data_length != static_cast<uint64_t>(count)) ||
         (has_add && add_length != (static_cast<uint64_t>(count) + 1) / 2))) {
        if (error) *error = "MCEdit block array length mismatch";
        return false;
    }
    if (palette_indexed_blocks &&
        (blocks_length != static_cast<uint64_t>(count) ||
         (has_data && data_length != static_cast<uint64_t>(count)))) {
        if (error) *error = "palette-indexed Blocks array length mismatch";
        return false;
    }
    if (sponge_format &&
        (block_data_length < static_cast<uint64_t>(count) ||
         block_data_length > static_cast<uint64_t>(count) * 5)) {
        if (error) *error = "Sponge BlockData length is impossible for the dimensions";
        return false;
    }

    const std::array<int32_t, 3>& format_offset = sponge_format ? sponge_offset : we_offset;
    const std::array<int64_t, 3> origin{{
        static_cast<int64_t>(options.base_x) + format_offset[0],
        static_cast<int64_t>(options.base_y) + format_offset[1],
        static_cast<int64_t>(options.base_z) + format_offset[2],
    }};
    const auto coordinateRangeFits = [](int64_t base, int32_t extent) {
        return base >= std::numeric_limits<int32_t>::min() &&
               base + static_cast<int64_t>(extent) - 1 <=
                   std::numeric_limits<int32_t>::max();
    };
    if (!coordinateRangeFits(origin[0], width) ||
        !coordinateRangeFits(origin[1], height) ||
        !coordinateRangeFits(origin[2], length)) {
        if (error) *error = "schematic offset coordinates exceed the supported world range";
        return false;
    }
    result->source_offset_x = format_offset[0];
    result->source_offset_y = format_offset[1];
    result->source_offset_z = format_offset[2];
    result->source_volume_bounds = {
        static_cast<int32_t>(origin[0]), static_cast<int32_t>(origin[1]),
        static_cast<int32_t>(origin[2]),
        static_cast<int32_t>(origin[0] + width - 1),
        static_cast<int32_t>(origin[1] + height - 1),
        static_cast<int32_t>(origin[2] + length - 1),
    };
    std::unique_ptr<ChunkSpoolWriter> writer;
    if (!options.block_sink) {
        writer = std::make_unique<ChunkSpoolWriter>(
            options.spool_directory, options.chunk_size,
            options.maximum_chunk_descriptors);
        if (options.include_source_volume &&
            !writer->includeVolume(result->source_volume_bounds, error,
                                   options.cancellation_requested)) return false;
    }
    reportProgress(options, SchematicParseStage::RoutingBlocks, 0,
                   static_cast<uint64_t>(count));
    // Keep only a compact local-index -> mode table.  TileEntities are read
    // before the block arrays in many valid schematics, so command payloads
    // must be held until the actual shell blocks have been resolved.
    std::unordered_map<uint64_t, CommandBlockShellState> command_block_modes;
    std::unordered_map<uint64_t, std::string> container_shells;
    if (options.command_block_sink) {
        command_block_modes.reserve(std::min<uint64_t>(
            static_cast<uint64_t>(deferred_command_blocks.records.size()), 4096U));
    }
    if (options.container_item_sink) {
        container_shells.reserve(std::min<uint64_t>(
            static_cast<uint64_t>(deferred_container_items.size()), 4096U));
    }
    auto append = [&](int64_t index, const BlockMappingResult& mapping,
                       std::string_view source_state) -> bool {
        const int32_t x = static_cast<int32_t>(index % width);
        const int32_t z = static_cast<int32_t>((index / width) % length);
        const int32_t y = static_cast<int32_t>(index / layer_area);
        if (mapping.status == BlockMappingStatus::Air) {
            ++result->skipped_block_count;
            return true;
        }
        if (mapping.status == BlockMappingStatus::Unsupported) {
            // Skip unrecognised legacy blocks (e.g. mod-only IDs from Schematica
            // exports) rather than aborting the whole import.  The vanilla portions
            // of the structure still import correctly; the mod blocks are omitted.
            ++result->unsupported_block_count;
            return true;
        }
        if (options.maximum_output_blocks != 0 &&
            result->imported_block_count >= options.maximum_output_blocks) {
            if (error) {
                *error = "parsed block count exceeds configured limit of " +
                    std::to_string(options.maximum_output_blocks);
            }
            return false;
        }
        ParsedBlock block{
            static_cast<int32_t>(origin[0] + x),
            static_cast<int32_t>(origin[1] + y),
            static_cast<int32_t>(origin[2] + z),
            mapping.spec,
        };
        if (options.block_sink) {
            if (!options.block_sink(block, error)) return false;
        } else if (!writer || !writer->append(block, error)) {
            if (error && error->empty()) *error = "schematic block output is unavailable";
            return false;
        }
        if (!mapping.reason.empty()) {
            ++result->degraded_block_count;
            if (result->first_degradation_reason.empty()) {
                result->first_degradation_reason = source_state.empty()
                    ? mapping.reason
                    : std::string(source_state) + ": " + mapping.reason;
            }
        }
        if (options.command_block_sink &&
            isCommandBlockEntityIdentifier(mapping.spec.command_name)) {
            command_block_modes.emplace(
                static_cast<uint64_t>(index),
                commandBlockShellState(commandBlockModeForIdentifier(mapping.spec.command_name),
                                       mapping.spec.aux));
        }
        if (options.container_item_sink &&
            deferredContainerIdentifier(mapping.spec.command_name)) {
            container_shells.emplace(static_cast<uint64_t>(index), mapping.spec.command_name);
        }
        ++result->imported_block_count;
        return true;
    };
    if (mcedit_format) {
        if (!options.legacy_block_resolver) {
            if (error) *error = "no legacy block resolver was provided";
            return false;
        }
        BufferedByteReader blocks(blocks_path);
        BufferedByteReader data(has_data ? data_path : std::string());
        BufferedByteReader add(has_add ? add_path : std::string());
        if (!blocks.valid() || (has_data && !data.valid()) ||
            (has_add && !add.valid())) {
            if (error) *error = "missing MCEdit raw arrays";
            return false;
        }
        uint8_t add_value = 0;
        // MCEdit's AddBlocks extension has two incompatible nibble orders in
        // the wild.  WorldEdit's reader/writer pair keeps the even index in the
        // low nibble, while Schematica keeps it in the high nibble.  Choosing
        // the wrong order does not merely produce unknown ids: it can also
        // yield a *valid* legacy id and silently place the wrong block, which
        // is worse than refusing the file.  `SchematicaMapping` is written only
        // by Schematica, so its presence selects that order and its absence
        // keeps the WorldEdit one.
        const bool schematica_add_nibbles = root_names.count("SchematicaMapping") != 0;
        std::unordered_map<uint32_t, BlockMappingResult> resolved_legacy;
        resolved_legacy.reserve(256);
        for (int64_t index = 0; index < count; ++index) {
            if ((index & 0x3FF) == 0 &&
                cancellationRequested(options.cancellation_requested, error)) return false;
            if ((index & 0x3FFF) == 0) {
                reportProgress(options, SchematicParseStage::RoutingBlocks,
                               static_cast<uint64_t>(index),
                               static_cast<uint64_t>(count));
            }
            uint8_t id = 0, meta = 0;
            if (!blocks.read(&id) || (has_data && !data.read(&meta))) {
                if (error) *error = "truncated MCEdit block array";
                return false;
            }
            uint16_t full_id = id;
            if (has_add) {
                if ((index & 1) == 0 && !add.read(&add_value)) {
                    if (error) *error = "truncated AddBlocks array";
                    return false;
                }
                const unsigned shift = schematica_add_nibbles
                    ? 4U * (1U - static_cast<unsigned>(index & 1))
                    : 4U * static_cast<unsigned>(index & 1);
                full_id |= uint16_t((add_value >> shift) & 0x0F) << 8;
            }
            const uint32_t mapping_key = (static_cast<uint32_t>(full_id) << 4U) |
                                         (meta & 0x0FU);
            auto mapping = resolved_legacy.find(mapping_key);
            if (mapping == resolved_legacy.end()) {
                mapping = resolved_legacy.emplace(
                    mapping_key, options.legacy_block_resolver(full_id, meta)).first;
            }
            if (!append(index, mapping->second, {})) return false;
        }
        uint8_t trailing = 0;
        if (blocks.read(&trailing) || (has_data && data.read(&trailing)) ||
            (has_add && add.read(&trailing))) {
            if (error) *error = "trailing MCEdit block array data";
            return false;
        }
        if (blocks.failed() || (has_data && data.failed()) || (has_add && add.failed())) {
            if (error) *error = "cannot finish reading MCEdit block arrays";
            return false;
        }
    } else if (palette_format) {
        if (!options.state_block_resolver) {
            if (error) *error = "no palette-state block resolver was provided";
            return false;
        }
        result->sponge_format = true;
        // Palette states repeat across most schematics. Resolve each state
        // once instead of reparsing identifiers and properties per voxel.
        std::vector<BlockMappingResult> resolved_palette;
        resolved_palette.reserve(palette.size());
        for (size_t palette_index = 0; palette_index < palette.size(); ++palette_index) {
            if ((palette_index & 0xFF) == 0 && cancellationRequested(options.cancellation_requested, error)) return false;
            resolved_palette.push_back(options.state_block_resolver(palette[palette_index]));
        }
        const auto route_sponge_data = [&](auto* data) -> bool {
            uint32_t value = 0;
            uint32_t shift = 0;
            int64_t index = 0;
            uint64_t byte_index = 0;
            uint8_t byte = 0;
            while (data->read(&byte)) {
                if ((byte_index++ & 0x3FF) == 0 &&
                    cancellationRequested(options.cancellation_requested, error)) return false;
                if (shift == 28 && (byte & 0xF0) != 0) {
                    if (error) *error = "invalid Sponge VarInt";
                    return false;
                }
                value |= uint32_t(byte & 0x7F) << shift;
                if (byte & 0x80) {
                    shift += 7;
                    if (shift > 28) {
                        if (error) *error = "invalid Sponge VarInt";
                        return false;
                    }
                    continue;
                }
                if (index >= count || value >= palette.size() || palette[value].empty()) {
                    if (error) *error = "Sponge palette index out of range or references a palette hole";
                    return false;
                }
                if (!append(index++, resolved_palette[value], palette[value])) return false;
                if ((index & 0x3FFF) == 0) {
                    reportProgress(options, SchematicParseStage::RoutingBlocks,
                                   static_cast<uint64_t>(index),
                                   static_cast<uint64_t>(count));
                }
                value = 0;
                shift = 0;
            }
            if (data->failed()) {
                if (error) *error = "cannot finish reading Sponge BlockData";
                return false;
            }
            if (index != count || shift != 0) {
                if (error) *error = "Sponge BlockData length mismatch";
                return false;
            }
            return true;
        };
        if (palette_indexed_blocks) {
            BufferedByteReader blocks(blocks_path);
            if (!blocks.valid()) {
                if (error) *error = "missing palette-indexed Blocks array";
                return false;
            }
            int64_t index = 0;
            uint8_t palette_index = 0;
            while (blocks.read(&palette_index)) {
                if ((index & 0x3FF) == 0 &&
                    cancellationRequested(options.cancellation_requested, error)) return false;
                if (index >= count || palette_index >= palette.size() ||
                    palette[palette_index].empty()) {
                    if (error) {
                        *error = "palette-indexed Blocks array references a palette hole or "
                                 "out-of-range index";
                    }
                    return false;
                }
                if (!append(index++, resolved_palette[palette_index],
                            palette[palette_index])) return false;
                if ((index & 0x3FFF) == 0) {
                    reportProgress(options, SchematicParseStage::RoutingBlocks,
                                   static_cast<uint64_t>(index),
                                   static_cast<uint64_t>(count));
                }
            }
            if (blocks.failed()) {
                if (error) *error = "cannot finish reading palette-indexed Blocks array";
                return false;
            }
            if (index != count) {
                if (error) *error = "palette-indexed Blocks array length mismatch";
                return false;
            }
        } else if (!deferred_sponge_block_data) {
            BufferedByteReader data(block_data);
            if (!data.valid()) {
                if (error) *error = "missing Sponge BlockData";
                return false;
            }
            if (!route_sponge_data(&data)) return false;
        } else {
            // Width and Offset may occur after BlockData in otherwise valid
            // Sponge v2 files. The first pass has now validated them, so a
            // second gzip pass can decode the payload straight into chunk
            // spools and avoid the large `_blockdata.raw` write/read pair.
            reportProgress(options, SchematicParseStage::ReadingSource, 0,
                           block_data_length);
            Reader replay(options.source_path);
            uint8_t replay_root = 0;
            std::string replay_root_name;
            if (!replay.valid() || !replay.u8(&replay_root) ||
                replay_root != Compound || !replay.string(&replay_root_name)) {
                if (error) *error = !replay.failure().empty() ? replay.failure()
                    : "cannot replay Sponge BlockData source";
                return false;
            }
            bool replayed = false;
            while (true) {
                if (cancellationRequested(options.cancellation_requested, error)) return false;
                uint8_t tag = 0;
                std::string name;
                if (!replay.u8(&tag)) {
                    if (error) *error = !replay.failure().empty() ? replay.failure()
                        : "unexpected end while replaying Sponge BlockData";
                    return false;
                }
                if (tag == End) break;
                if (!replay.string(&name)) {
                    if (error) *error = !replay.failure().empty() ? replay.failure()
                        : "invalid NBT tag while replaying Sponge BlockData";
                    return false;
                }
                if (name != "BlockData") {
                    if (!skipPayload(replay, tag, 0,
                                     options.cancellation_requested, error)) {
                        if (error && error->empty()) {
                            *error = !replay.failure().empty() ? replay.failure()
                                : "cannot skip NBT tag while replaying Sponge BlockData";
                        }
                        return false;
                    }
                    continue;
                }
                if (tag != ByteArray) {
                    if (error) *error = "replayed Sponge BlockData is not a byte array";
                    return false;
                }
                uint32_t replay_length = 0;
                if (!replay.be32(&replay_length) ||
                    replay_length != block_data_length) {
                    if (error) *error = !replay.failure().empty() ? replay.failure()
                        : "replayed Sponge BlockData length changed";
                    return false;
                }
                BoundedByteReader data(&replay, replay_length);
                if (!route_sponge_data(&data)) return false;
                replayed = true;
                break;
            }
            if (!replayed) {
                if (error) *error = "replayed source has no Sponge BlockData";
                return false;
            }
        }
    }
    if (cancellationRequested(options.cancellation_requested, error)) return false;
    reportProgress(options, SchematicParseStage::RoutingBlocks,
                   static_cast<uint64_t>(count), static_cast<uint64_t>(count));
    if (!flushInfiniteczRawBlocks(options, origin, width, height, length,
                                  &deferred_raw_blocks, raw_payload_bytes,
                                  result, error)) {
        if (writer) writer->discard();
        return false;
    }
    // Command payloads are written only after the ordinary block arrays have
    // routed successfully.  The sink therefore never receives a command for a
    // schematic whose command-block shell failed to parse or map.
    if (!flushDeferredCommandBlocks(options, origin, width, height, length,
                                    command_block_modes, &deferred_command_blocks,
                                    result, error)) {
        if (writer) writer->discard();
        return false;
    }
    if (!flushDeferredContainerItems(options, origin, width, height, length,
                                     container_shells, &deferred_container_items,
                                     result, error)) {
        if (writer) writer->discard();
        return false;
    }
    if (!flushDeferredEntities(options, origin, &deferred_entities, result, error)) {
        if (writer) writer->discard();
        return false;
    }
    reportProgress(options, SchematicParseStage::FinalizingSpools, 0,
                    result->imported_block_count);
    if (writer) {
        result->chunks = writer->finish(
            error, options.cancellation_requested,
            [&](uint64_t completed, uint64_t total) {
                reportProgress(options, SchematicParseStage::FinalizingSpools,
                               completed, total);
            });
    }
    if (cancellationRequested(options.cancellation_requested, error)) {
        if (writer) writer->discard();
        result->chunks.clear();
        return false;
    }
    reportProgress(options, SchematicParseStage::FinalizingSpools,
                   result->imported_block_count, result->imported_block_count);
    return options.block_sink ? true : !result->chunks.empty();
}

}  // namespace build_import
