#include "CommandSpool.h"
#include "BlockMapper.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <string_view>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace build_import {
namespace {

constexpr uint32_t kCommandMagic = 0x444D4342;       // "BCMD"
constexpr uint32_t kLegacyCommandVersion = 1;
constexpr uint32_t kMergeMetadataCommandVersion = 2;
constexpr uint32_t kCommandVersion = 3;
constexpr uint32_t kVerificationMagic = 0x50564942;  // "BIVP"
constexpr uint32_t kVerificationVersion = 3;
constexpr uint32_t kStableSamplesPerChunk = 16;
constexpr uint32_t kAirSamplesPerChunk = 4;
constexpr uint32_t kMinimumFluidSamples = 4;
constexpr uint64_t kMaximumAirProbeCount = 65536;
constexpr uint32_t kLegacyMaximumAirSamplesPerChunk = 8;
constexpr uint16_t kMaxBlockNameLength = 256;
constexpr uint8_t kKnownVerificationFlags =
    static_cast<uint8_t>(VerificationSampleFlag::ExpectedAir) |
    static_cast<uint8_t>(VerificationSampleFlag::IgnoreAux);
constexpr uint8_t kKnownPlannedCommandFlags =
    static_cast<uint8_t>(PlannedCommandFlag::HasMergeMetadata) |
    static_cast<uint8_t>(PlannedCommandFlag::Fillable) |
    static_cast<uint8_t>(PlannedCommandFlag::SingleLayer);
constexpr uint64_t kCommandHeaderSize = sizeof(uint32_t) * 2 + sizeof(uint64_t) * 2;
constexpr uint64_t kLegacyCommandRecordFixedSize = sizeof(int32_t) * 6 + sizeof(uint32_t) +
                                                    sizeof(uint8_t) * 2 + sizeof(uint16_t);
constexpr uint64_t kCommandRecordFixedSize = sizeof(int32_t) * 6 + sizeof(uint32_t) +
                                              sizeof(uint16_t) * 2 + sizeof(uint8_t);
constexpr uint64_t kVerificationHeaderSize = sizeof(uint32_t) * 2 + sizeof(uint64_t) * 2;
constexpr uint64_t kVerificationChunkFixedSize = sizeof(int32_t) * 8 + sizeof(uint64_t) * 2 +
                                                 sizeof(uint32_t) * 2;
constexpr uint64_t kLegacyVerificationSampleFixedSize = sizeof(int32_t) * 3 +
    sizeof(uint8_t) * 2 + sizeof(uint16_t);
constexpr uint64_t kVerificationSampleFixedSize = sizeof(int32_t) * 3 +
    sizeof(uint16_t) * 2 + sizeof(uint8_t);
// Stitching keeps multiple candidate vectors alive. Extremely fragmented 3D
// plans get little benefit from another cross-chunk pass but can otherwise
// consume tens of megabytes on Android during planning.
constexpr uint64_t kMaximumStitchCommandCount = 262144;
constexpr uint64_t kMaximumDualCandidateStitchCommandCount = 65536;
constexpr uint64_t kMaximumPlanarRemergeBlockCount = 131072;
constexpr uint64_t kMaximumThreeDimensionalRemergeBlockCount = 32768;
// The adaptive cover already evaluates every axis order for each seed. The
// additional whole-phase covers are a command-count refinement, not a
// correctness requirement. Bounding them keeps exceptionally tall or dense
// chunk phases from spending seconds repeating the same full-record walk.
constexpr size_t kMaximumExhaustiveMergeRecordCount = 131072;
// Dense material groups use one bit per position plus one immutable template
// for cheap cover resets. Keep the optimization local to a chunk phase and
// bounded so a sparse palette cannot amplify memory use.
constexpr size_t kMinimumDenseMergeGroupRecords = 64;
constexpr uint64_t kMaximumDenseMergeBitsPerRecord = 64;
constexpr size_t kMaximumDenseMergeWords =
    (4ULL * 1024ULL * 1024ULL) / sizeof(uint64_t);
// Each successful stitch pass reduces the command count, but pathological
// alternating shapes can expose only a small number of joins per pass. A
// bounded refinement retains an exact cover even when it stops early.
constexpr size_t kMaximumStitchPassPairs = 8;
constexpr size_t kCommandReadBufferSize = 64 * 1024;
constexpr size_t kRawSpoolReadBufferSize = 64 * 1024;

struct RawDiskRecord {
    int32_t x, y, z;
    uint8_t aux, flags;
    uint16_t name_length;
};

struct RawRecord {
    int32_t x = 0, y = 0, z = 0;
    uint16_t aux = 0;
    uint8_t flags = 0;
    uint32_t name_index = 0;
};

struct RawBatch {
    std::vector<RawRecord> records;
    std::vector<std::string> names;
    const std::vector<std::string>* shared_names = nullptr;
    bool coordinate_ordered = false;

    const std::vector<std::string>& nameTable() const {
        return shared_names ? *shared_names : names;
    }
};

bool isBed(std::string_view name) {
    const size_t separator = name.rfind(':');
    const std::string_view leaf = separator == std::string_view::npos
                                      ? name
                                      : name.substr(separator + 1);
    return leaf == "bed" ||
           (leaf.size() > 4 && leaf.substr(leaf.size() - 4) == "_bed");
}

struct Position {
    int32_t x = 0, y = 0, z = 0;
    bool operator==(const Position& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct PositionHash {
    size_t operator()(const Position& value) const {
        return static_cast<size_t>(uint32_t(value.x) * 73856093u) ^
               static_cast<size_t>(uint32_t(value.y) * 19349663u) ^
               static_cast<size_t>(uint32_t(value.z) * 83492791u);
    }
};

struct SampleBucket {
    uint64_t seen = 0;
    std::vector<VerificationPlanSample> samples;
};

struct CommandRecordHeader {
    BlockBounds bounds;
    uint32_t block_count = 0;
    uint16_t aux = 0;
    uint8_t reserved = 0;
    uint16_t name_length = 0;
};

struct LegacyCommandRecordHeader {
    BlockBounds bounds;
    uint32_t block_count = 0;
    uint8_t aux = 0;
    uint8_t reserved = 0;
    uint16_t name_length = 0;
};

template <typename T>
bool writeValue(std::ofstream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(stream);
}

template <typename T>
bool readValue(std::ifstream& stream, T* value) {
    stream.read(reinterpret_cast<char*>(value), sizeof(*value));
    return static_cast<bool>(stream);
}

bool streamSize(std::ifstream& stream, uint64_t* size) {
    stream.clear();
    stream.seekg(0, std::ios::end);
    const std::streampos end = stream.tellg();
    if (end < 0) return false;
    *size = static_cast<uint64_t>(end);
    stream.seekg(0, std::ios::beg);
    return static_cast<bool>(stream);
}

bool addWithoutOverflow(uint64_t left, uint64_t right, uint64_t* result) {
    if (right > std::numeric_limits<uint64_t>::max() - left) return false;
    *result = left + right;
    return true;
}

bool boundsVolume(const BlockBounds& bounds, uint64_t* volume) {
    if (!bounds.isValid()) return false;
    const uint64_t width = static_cast<uint64_t>(
        static_cast<int64_t>(bounds.max_x) - bounds.min_x + 1);
    const uint64_t height = static_cast<uint64_t>(
        static_cast<int64_t>(bounds.max_y) - bounds.min_y + 1);
    const uint64_t depth = static_cast<uint64_t>(
        static_cast<int64_t>(bounds.max_z) - bounds.min_z + 1);
    if (width != 0 && height > std::numeric_limits<uint64_t>::max() / width) return false;
    const uint64_t plane = width * height;
    if (depth != 0 && plane > std::numeric_limits<uint64_t>::max() / depth) return false;
    *volume = plane * depth;
    return *volume != 0;
}

class RecordIndex {
public:
    explicit RecordIndex(const std::vector<RawRecord>& records) {
        if (records.empty() || records.size() > std::numeric_limits<uint32_t>::max()) return;
        bounds_ = {records.front().x, records.front().y, records.front().z,
                   records.front().x, records.front().y, records.front().z};
        for (const RawRecord& record : records) {
            bounds_.min_x = std::min(bounds_.min_x, record.x);
            bounds_.min_y = std::min(bounds_.min_y, record.y);
            bounds_.min_z = std::min(bounds_.min_z, record.z);
            bounds_.max_x = std::max(bounds_.max_x, record.x);
            bounds_.max_y = std::max(bounds_.max_y, record.y);
            bounds_.max_z = std::max(bounds_.max_z, record.z);
        }
        uint64_t volume = 0;
        const uint64_t proportional_limit = records.size() <=
                std::numeric_limits<uint64_t>::max() / 8
            ? static_cast<uint64_t>(records.size()) * 8 :
              std::numeric_limits<uint64_t>::max();
        constexpr uint64_t kMaximumDenseEntries = 4ULL * 1024ULL * 1024ULL;
        if (boundsVolume(bounds_, &volume) && volume <= kMaximumDenseEntries &&
            (volume <= proportional_limit || volume <= 262144)) {
            dense_.assign(static_cast<size_t>(volume), kMissing);
            for (size_t index = 0; index < records.size(); ++index) {
                uint32_t& slot = dense_[denseOffset(
                    records[index].x, records[index].y, records[index].z)];
                if (slot != kMissing) {
                    duplicate_position_ = true;
                    dense_.clear();
                    return;
                }
                slot = static_cast<uint32_t>(index);
            }
        } else {
            sparse_.reserve(records.size());
            for (size_t index = 0; index < records.size(); ++index) {
                const auto inserted = sparse_.emplace(
                    Position{records[index].x, records[index].y, records[index].z}, index);
                if (!inserted.second) {
                    duplicate_position_ = true;
                    sparse_.clear();
                    return;
                }
            }
        }
        valid_ = true;
    }

    std::optional<size_t> find(int32_t x, int32_t y, int32_t z) const {
        if (!valid_) return std::nullopt;
        if (!dense_.empty()) {
            if (x < bounds_.min_x || x > bounds_.max_x ||
                y < bounds_.min_y || y > bounds_.max_y ||
                z < bounds_.min_z || z > bounds_.max_z) return std::nullopt;
            const uint32_t index = dense_[denseOffset(x, y, z)];
            return index == kMissing ? std::nullopt : std::optional<size_t>(index);
        }
        const auto found = sparse_.find({x, y, z});
        return found == sparse_.end() ? std::nullopt :
               std::optional<size_t>(found->second);
    }

    bool valid() const { return valid_; }
    bool duplicatePosition() const { return duplicate_position_; }

private:
    size_t denseOffset(int32_t x, int32_t y, int32_t z) const {
        const uint64_t width = static_cast<uint64_t>(
            static_cast<int64_t>(bounds_.max_x) - bounds_.min_x + 1);
        const uint64_t depth = static_cast<uint64_t>(
            static_cast<int64_t>(bounds_.max_z) - bounds_.min_z + 1);
        const uint64_t x_offset = static_cast<uint64_t>(
            static_cast<int64_t>(x) - bounds_.min_x);
        const uint64_t y_offset = static_cast<uint64_t>(
            static_cast<int64_t>(y) - bounds_.min_y);
        const uint64_t z_offset = static_cast<uint64_t>(
            static_cast<int64_t>(z) - bounds_.min_z);
        return static_cast<size_t>((y_offset * depth + z_offset) * width + x_offset);
    }

    static constexpr uint32_t kMissing = std::numeric_limits<uint32_t>::max();
    BlockBounds bounds_;
    std::vector<uint32_t> dense_;
    std::unordered_map<Position, size_t, PositionHash> sparse_;
    bool valid_ = false;
    bool duplicate_position_ = false;
};

class PositionFlags {
public:
    explicit PositionFlags(const BlockBounds& bounds) : bounds_(bounds) {
        uint64_t volume = 0;
        constexpr uint64_t kMaximumDenseEntries = 8ULL * 1024ULL * 1024ULL;
        if (boundsVolume(bounds_, &volume) && volume <= kMaximumDenseEntries) {
            dense_.assign(static_cast<size_t>(volume), 0);
        }
    }

    bool insertOccupied(const Position& position) {
        uint8_t& flags = valueFor(position);
        if ((flags & kOccupied) != 0) return false;
        flags |= kOccupied;
        ++occupied_count_;
        return true;
    }

    void markUnstable(const Position& position) {
        valueFor(position) |= kUnstable;
    }

    bool occupied(const Position& position) const {
        return (valueAt(position) & kOccupied) != 0;
    }

    bool unstable(const Position& position) const {
        return (valueAt(position) & kUnstable) != 0;
    }

    size_t occupiedCount() const { return occupied_count_; }

private:
    bool containsPosition(const Position& position) const {
        return bounds_.isValid() && position.x >= bounds_.min_x &&
            position.x <= bounds_.max_x && position.y >= bounds_.min_y &&
            position.y <= bounds_.max_y && position.z >= bounds_.min_z &&
            position.z <= bounds_.max_z;
    }

    size_t denseOffset(const Position& position) const {
        const uint64_t width = static_cast<uint64_t>(
            static_cast<int64_t>(bounds_.max_x) - bounds_.min_x + 1);
        const uint64_t depth = static_cast<uint64_t>(
            static_cast<int64_t>(bounds_.max_z) - bounds_.min_z + 1);
        const uint64_t x = static_cast<uint64_t>(
            static_cast<int64_t>(position.x) - bounds_.min_x);
        const uint64_t y = static_cast<uint64_t>(
            static_cast<int64_t>(position.y) - bounds_.min_y);
        const uint64_t z = static_cast<uint64_t>(
            static_cast<int64_t>(position.z) - bounds_.min_z);
        return static_cast<size_t>((y * depth + z) * width + x);
    }

    uint8_t& valueFor(const Position& position) {
        if (!dense_.empty() && containsPosition(position)) {
            return dense_[denseOffset(position)];
        }
        return sparse_[position];
    }

    uint8_t valueAt(const Position& position) const {
        if (!dense_.empty() && containsPosition(position)) {
            return dense_[denseOffset(position)];
        }
        const auto found = sparse_.find(position);
        return found == sparse_.end() ? 0 : found->second;
    }

    static constexpr uint8_t kOccupied = 0x01;
    static constexpr uint8_t kUnstable = 0x02;
    BlockBounds bounds_;
    std::vector<uint8_t> dense_;
    std::unordered_map<Position, uint8_t, PositionHash> sparse_;
    size_t occupied_count_ = 0;
};

bool contains(const BlockBounds& bounds, int32_t x, int32_t y, int32_t z) {
    return bounds.isValid() && x >= bounds.min_x && x <= bounds.max_x &&
           y >= bounds.min_y && y <= bounds.max_y &&
           z >= bounds.min_z && z <= bounds.max_z;
}

uint64_t commandRecordFixedSizeForVersion(uint32_t version) {
    return version == kLegacyCommandVersion || version == kMergeMetadataCommandVersion
        ? kLegacyCommandRecordFixedSize : kCommandRecordFixedSize;
}

uint64_t verificationSampleFixedSizeForVersion(uint32_t version) {
    return version == 2 ? kLegacyVerificationSampleFixedSize
                        : kVerificationSampleFixedSize;
}

bool readCommandHeader(std::ifstream& stream, uint32_t version,
                       CommandRecordHeader* header) {
    if (!header) return false;
    if (version == kLegacyCommandVersion || version == kMergeMetadataCommandVersion) {
        LegacyCommandRecordHeader legacy{};
        if (!readValue(stream, &legacy.bounds.min_x) ||
            !readValue(stream, &legacy.bounds.min_y) ||
            !readValue(stream, &legacy.bounds.min_z) ||
            !readValue(stream, &legacy.bounds.max_x) ||
            !readValue(stream, &legacy.bounds.max_y) ||
            !readValue(stream, &legacy.bounds.max_z) ||
            !readValue(stream, &legacy.block_count) ||
            !readValue(stream, &legacy.aux) || !readValue(stream, &legacy.reserved) ||
            !readValue(stream, &legacy.name_length)) {
            return false;
        }
        header->bounds = legacy.bounds;
        header->block_count = legacy.block_count;
        header->aux = legacy.aux;
        header->reserved = legacy.reserved;
        header->name_length = legacy.name_length;
        return true;
    }
    return readValue(stream, &header->bounds.min_x) &&
           readValue(stream, &header->bounds.min_y) &&
           readValue(stream, &header->bounds.min_z) &&
           readValue(stream, &header->bounds.max_x) &&
           readValue(stream, &header->bounds.max_y) &&
           readValue(stream, &header->bounds.max_z) &&
           readValue(stream, &header->block_count) &&
           readValue(stream, &header->aux) && readValue(stream, &header->reserved) &&
           readValue(stream, &header->name_length);
}

bool validPlannedCommandFlags(uint8_t flags) {
    if ((flags & ~kKnownPlannedCommandFlags) != 0) return false;
    const bool has_metadata =
        (flags & static_cast<uint8_t>(PlannedCommandFlag::HasMergeMetadata)) != 0;
    return has_metadata || flags == 0;
}

bool validCommandHeader(const CommandRecordHeader& header, uint32_t version) {
    uint64_t volume = 0;
    const bool valid_flags = version == kLegacyCommandVersion
        ? header.reserved == 0 : validPlannedCommandFlags(header.reserved);
    return valid_flags && header.name_length != 0 &&
           header.name_length <= kMaxBlockNameLength && header.block_count != 0 &&
           boundsVolume(header.bounds, &volume) && volume == header.block_count;
}

bool cancellationRequested(const CommandSpoolBuilder::CancelCheck& cancel_check,
                           std::string* error) {
    if (!cancel_check || !cancel_check()) return false;
    if (error) *error = "import cancelled";
    return true;
}

void reportProgress(const CommandSpoolBuilder::ProgressCallback& callback,
                    CommandSpoolBuildStage stage, uint64_t completed,
                    uint64_t total) {
    if (callback) callback({stage, completed, total});
}

bool replaceFileAtomically(const std::string& source, const std::string& destination) {
#if defined(_WIN32)
    return MoveFileExA(source.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    return std::rename(source.c_str(), destination.c_str()) == 0;
#endif
}

class TemporaryOutputFile {
public:
    explicit TemporaryOutputFile(std::string path)
        : path_(std::move(path)),
          stream_(path_, std::ios::binary | std::ios::trunc) {}

    ~TemporaryOutputFile() {
        if (stream_.is_open()) stream_.close();
        if (!committed_) std::remove(path_.c_str());
    }

    std::ofstream& stream() { return stream_; }

    bool commit(const std::string& destination) {
        stream_.flush();
        stream_.close();
        if (!stream_ || !replaceFileAtomically(path_, destination)) return false;
        committed_ = true;
        return true;
    }

private:
    std::string path_;
    std::ofstream stream_;
    bool committed_ = false;
};

bool loadRawPalette(const std::string& path, std::vector<std::string>* names,
                    bool* found, std::string* error) {
    names->clear();
    *found = false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return true;
    uint64_t file_size = 0;
    RawPaletteHeaderV2 header{};
    if (!streamSize(stream, &file_size) || file_size < sizeof(header) ||
        !readValue(stream, &header) || header.magic != kRawPaletteMagic ||
        header.version != kRawPaletteVersion || header.reserved != 0 ||
        header.entry_count > (file_size - sizeof(header)) /
                                 (sizeof(uint16_t) + 1)) {
        if (error) *error = "invalid raw block palette";
        return false;
    }
    names->reserve(header.entry_count);
    std::set<std::string> unique_names;
    for (uint32_t index = 0; index < header.entry_count; ++index) {
        uint16_t length = 0;
        if (!readValue(stream, &length) || length == 0 ||
            length > kMaxBlockNameLength) {
            if (error) *error = "invalid raw block palette entry";
            return false;
        }
        std::string name(length, '\0');
        stream.read(&name[0], length);
        if (!stream || !unique_names.insert(name).second) {
            if (error) *error = "invalid raw block palette entry";
            return false;
        }
        names->push_back(std::move(name));
    }
    if (stream.peek() != std::char_traits<char>::eof()) {
        if (error) *error = "raw block palette has trailing data";
        return false;
    }
    *found = true;
    return true;
}

bool coordinatePrecedes(const RawRecord& left, const RawRecord& right) {
    if (left.y != right.y) return left.y < right.y;
    if (left.z != right.z) return left.z < right.z;
    return left.x < right.x;
}

void appendRawRecord(const RawRecord& record, RawBatch* output) {
    if (!output->records.empty() &&
        coordinatePrecedes(record, output->records.back())) {
        output->coordinate_ordered = false;
    }
    output->records.push_back(record);
}

bool readRawRecords(const std::string& path,
                    const std::vector<std::string>* shared_palette,
                    RawBatch* output,
                    const CommandSpoolBuilder::CancelCheck& cancel_check,
                    std::string* error) {
    std::vector<char> stream_buffer(kRawSpoolReadBufferSize);
    std::ifstream stream;
    stream.rdbuf()->pubsetbuf(stream_buffer.data(), stream_buffer.size());
    stream.open(path, std::ios::binary);
    if (!stream) {
        if (error) *error = "cannot open raw chunk spool";
        return false;
    }
    output->records.clear();
    output->names.clear();
    output->shared_names = nullptr;
    output->coordinate_ordered = true;

    uint32_t possible_magic = 0;
    stream.read(reinterpret_cast<char*>(&possible_magic), sizeof(possible_magic));
    const std::streamsize prefix_bytes = stream.gcount();
    stream.clear();
    stream.seekg(0, std::ios::beg);
    if (!stream) {
        if (error) *error = "cannot inspect raw chunk spool";
        return false;
    }
    if (prefix_bytes == static_cast<std::streamsize>(sizeof(possible_magic)) &&
        possible_magic == kRawSpoolMagic) {
        RawSpoolHeaderV2 header{};
        const bool is_v3 = readValue(stream, &header) &&
            header.version == kRawSpoolVersion &&
            header.kind == kRawSpoolKindBlocks &&
            header.record_size == sizeof(RawSpoolRecordV3);
        const bool is_v2 = header.version == 2 &&
            header.kind == kRawSpoolKindBlocks &&
            header.record_size == sizeof(RawSpoolRecordV2);
        if ((!is_v3 && !is_v2) || !shared_palette) {
            if (error) *error = shared_palette
                ? "invalid raw chunk spool header"
                : "raw chunk spool is missing its block palette";
            return false;
        }
        output->shared_names = shared_palette;
        if (is_v3) {
            RawSpoolRecordV3 disk{};
            for (;;) {
                if ((output->records.size() & 0x3FF) == 0 &&
                    cancellationRequested(cancel_check, error)) return false;
                stream.read(reinterpret_cast<char*>(&disk), sizeof(disk));
                const std::streamsize bytes = stream.gcount();
                if (bytes == 0 && stream.eof()) break;
                if (bytes != static_cast<std::streamsize>(sizeof(disk))) {
                    if (error) *error = "truncated raw chunk spool";
                    return false;
                }
                if (disk.reserved != 0 || disk.name_id >= shared_palette->size()) {
                    if (error) *error = "corrupt raw chunk spool";
                    return false;
                }
                appendRawRecord({disk.x, disk.y, disk.z, disk.aux, disk.flags,
                                 disk.name_id}, output);
            }
        } else {
            // Interrupted plans written before the wide-aux migration used the
            // same header/palette protocol but retained an 8-bit auxiliary
            // value.  Widen it explicitly instead of interpreting the v2
            // reserved bytes as the high half of a v3 state.
            RawSpoolRecordV2 disk{};
            for (;;) {
                if ((output->records.size() & 0x3FF) == 0 &&
                    cancellationRequested(cancel_check, error)) return false;
                stream.read(reinterpret_cast<char*>(&disk), sizeof(disk));
                const std::streamsize bytes = stream.gcount();
                if (bytes == 0 && stream.eof()) break;
                if (bytes != static_cast<std::streamsize>(sizeof(disk))) {
                    if (error) *error = "truncated raw chunk spool";
                    return false;
                }
                if (disk.reserved != 0 || disk.name_id >= shared_palette->size()) {
                    if (error) *error = "corrupt raw chunk spool";
                    return false;
                }
                appendRawRecord({disk.x, disk.y, disk.z, disk.aux, disk.flags,
                                 disk.name_id}, output);
            }
        }
        if (!stream.eof() && stream.fail()) {
            if (error) *error = "cannot read raw chunk spool";
            return false;
        }
        return true;
    }

    std::unordered_map<std::string, uint32_t> name_indices;
    RawDiskRecord disk{};
    for (;;) {
        if ((output->records.size() & 0x3FF) == 0 &&
            cancellationRequested(cancel_check, error)) return false;
        stream.read(reinterpret_cast<char*>(&disk), sizeof(disk));
        const std::streamsize header_bytes = stream.gcount();
        if (header_bytes == 0 && stream.eof()) break;
        if (header_bytes != static_cast<std::streamsize>(sizeof(disk))) {
            if (error) *error = "truncated raw chunk spool";
            return false;
        }
        if (disk.name_length == 0 || disk.name_length > kMaxBlockNameLength) {
            if (error) *error = "corrupt raw chunk spool";
            return false;
        }
        RawRecord record{disk.x, disk.y, disk.z, disk.aux, disk.flags, 0};
        std::string name(disk.name_length, '\0');
        stream.read(&name[0], disk.name_length);
        if (!stream) {
            if (error) *error = "truncated raw chunk spool";
            return false;
        }
        auto existing = name_indices.find(name);
        if (existing == name_indices.end()) {
            if (output->names.size() >= std::numeric_limits<uint32_t>::max()) {
                if (error) *error = "raw chunk spool contains too many block names";
                return false;
            }
            record.name_index = static_cast<uint32_t>(output->names.size());
            output->names.push_back(name);
            name_indices.emplace(std::move(name), record.name_index);
        } else {
            record.name_index = existing->second;
        }
        appendRawRecord(record, output);
    }
    if (!stream.eof() && stream.fail()) {
        if (error) *error = "cannot read raw chunk spool";
        return false;
    }
    return true;
}

bool sameBlock(const RawRecord& left, const RawRecord& right) {
    return left.aux == right.aux && left.flags == right.flags &&
           left.name_index == right.name_index;
}

bool isSafeAttachmentFillName(std::string_view name) {
    const size_t separator = name.rfind(':');
    const std::string_view leaf = separator == std::string_view::npos
        ? name : name.substr(separator + 1);
    static constexpr std::array<std::string_view, 22> kSafeNames{{
        "bamboo", "bamboo_sapling", "beetroot", "brown_mushroom", "cactus",
        "carrots", "crimson_roots", "deadbush", "melon_stem", "nether_wart",
        "potatoes", "pumpkin_stem", "red_flower", "red_mushroom", "sapling",
        "short_grass", "tallgrass", "torchflower", "warped_roots", "wheat",
        "wither_rose", "yellow_flower",
    }};
    return std::find(kSafeNames.begin(), kSafeNames.end(), leaf) != kSafeNames.end();
}

bool canFill(const RawRecord& value, ImportPhase phase,
             const std::vector<std::string>& names) {
    if (phase == ImportPhase::DependentAttachment) return false;
    if (phase == ImportPhase::Attachment) {
        return (value.flags & 0x04) != 0 && (value.flags & 0x08) == 0 &&
               value.name_index < names.size() &&
               isSafeAttachmentFillName(names[value.name_index]);
    }
    const bool mapped_fillable = value.flags == 0 ||
        ((value.flags & 0x04) != 0 && (value.flags & 0x01) != 0 &&
         (value.flags & 0x08) == 0);
    return mapped_fillable;
}

bool singleLayer(const RawRecord& value, ImportPhase phase) {
    return phase == ImportPhase::Gravity || phase == ImportPhase::Fluid ||
           phase == ImportPhase::Attachment ||
           ((value.flags & 0x04) != 0 && (value.flags & 0x02) != 0);
}

uint64_t mix64(uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

void considerVerificationSample(const RawRecord& record, std::string_view name,
                                 ImportPhase phase,
                                 const ChunkCoord& chunk, uint32_t capacity,
                                 SampleBucket* bucket) {
    // Gravity, support-dependent blocks, and naturally changing blocks can
    // legitimately move or disappear after placement. Fluid Aux levels are
    // dynamic, but retaining a presence sample is necessary to detect a whole
    // omitted fluid partition. Callers keep source and flowing reservoirs
    // separate so stable sources are always preferred when available.
    const bool fluid = phase == ImportPhase::Fluid;
    if (phase == ImportPhase::Gravity || phase == ImportPhase::Attachment ||
        phase == ImportPhase::DependentAttachment ||
        (phase == ImportPhase::Structure &&
         !BlockMapper::isStableVerificationBlockName(name))) {
        return;
    }
    ++bucket->seen;
    if (capacity == 0) return;
    size_t destination = bucket->samples.size();
    if (destination >= capacity) {
        const uint64_t seed = bucket->seen ^ (uint64_t(uint32_t(chunk.x)) << 32) ^
                              uint32_t(chunk.z);
        const uint64_t slot = mix64(seed) % bucket->seen;
        if (slot >= capacity) return;
        destination = static_cast<size_t>(slot);
    }
    const uint8_t sample_flags = (record.flags & 0x08) != 0 || fluid
        ? static_cast<uint8_t>(VerificationSampleFlag::IgnoreAux) : 0;
    VerificationPlanSample sample{chunk, record.x, record.y, record.z,
                                  std::string(name), record.aux, sample_flags};
    if (destination == bucket->samples.size()) {
        bucket->samples.push_back(std::move(sample));
    } else {
        bucket->samples[destination] = std::move(sample);
    }
}

struct MergeGroupKey {
    uint32_t name_index = 0;
    int32_t y = 0;
    uint16_t aux = 0;
    uint8_t flags = 0;
    bool layer_only = false;

    bool operator<(const MergeGroupKey& other) const {
        if (name_index != other.name_index) return name_index < other.name_index;
        if (aux != other.aux) return aux < other.aux;
        if (flags != other.flags) return flags < other.flags;
        if (layer_only != other.layer_only) return layer_only < other.layer_only;
        return layer_only && y < other.y;
    }
};

class MergeCoverState {
public:
    MergeCoverState(const std::vector<RawRecord>& records,
                    const std::vector<std::string>& names,
                    const RecordIndex& positions, ImportPhase phase)
        : records_(records), positions_(positions), sparse_used_(records.size(), false),
          dense_group_for_record_(records.size(), kSparseGroup) {
        struct GroupCandidate {
            MergeGroupKey key;
            BlockBounds bounds;
            uint64_t count = 0;
            size_t word_count = 0;
            uint32_t dense_group = kSparseGroup;
        };

        std::map<MergeGroupKey, uint32_t> candidate_indices;
        std::vector<GroupCandidate> candidates;
        std::vector<uint32_t> candidate_for_record(records.size(), kSparseGroup);
        for (size_t index = 0; index < records.size(); ++index) {
            const RawRecord& record = records[index];
            if (!canFill(record, phase, names)) continue;
            const bool layer_only = singleLayer(record, phase);
            const MergeGroupKey key{record.name_index, layer_only ? record.y : 0,
                                    record.aux, record.flags, layer_only};
            const auto inserted = candidate_indices.emplace(
                key, static_cast<uint32_t>(candidates.size()));
            if (inserted.second) {
                const uint32_t candidate_index = static_cast<uint32_t>(candidates.size());
                candidates.push_back({key,
                                      {record.x, record.y, record.z,
                                       record.x, record.y, record.z},
                                      0, 0, kSparseGroup});
                (void)candidate_index;
            }
            GroupCandidate& candidate = candidates[inserted.first->second];
            candidate.bounds.min_x = std::min(candidate.bounds.min_x, record.x);
            candidate.bounds.min_y = std::min(candidate.bounds.min_y, record.y);
            candidate.bounds.min_z = std::min(candidate.bounds.min_z, record.z);
            candidate.bounds.max_x = std::max(candidate.bounds.max_x, record.x);
            candidate.bounds.max_y = std::max(candidate.bounds.max_y, record.y);
            candidate.bounds.max_z = std::max(candidate.bounds.max_z, record.z);
            ++candidate.count;
            candidate_for_record[index] = inserted.first->second;
        }

        std::vector<size_t> dense_candidates;
        dense_candidates.reserve(candidates.size());
        for (size_t index = 0; index < candidates.size(); ++index) {
            GroupCandidate& candidate = candidates[index];
            if (candidate.count < kMinimumDenseMergeGroupRecords) continue;
            uint64_t volume = 0;
            if (!boundsVolume(candidate.bounds, &volume) ||
                volume > std::numeric_limits<uint64_t>::max() - 63) {
                continue;
            }
            const uint64_t density_limit = candidate.count <=
                    std::numeric_limits<uint64_t>::max() /
                        kMaximumDenseMergeBitsPerRecord
                ? candidate.count * kMaximumDenseMergeBitsPerRecord
                : std::numeric_limits<uint64_t>::max();
            if (volume > density_limit) continue;
            const uint64_t word_count = (volume + 63) / 64;
            if (word_count == 0 || word_count > kMaximumDenseMergeWords) continue;
            candidate.word_count = static_cast<size_t>(word_count);
            dense_candidates.push_back(index);
        }
        std::sort(dense_candidates.begin(), dense_candidates.end(),
                  [&](size_t left, size_t right) {
                      if (candidates[left].count != candidates[right].count) {
                          return candidates[left].count > candidates[right].count;
                      }
                      return candidates[left].key < candidates[right].key;
                  });

        size_t allocated_words = 0;
        for (const size_t candidate_index : dense_candidates) {
            GroupCandidate& candidate = candidates[candidate_index];
            if (candidate.word_count > kMaximumDenseMergeWords - allocated_words) continue;
            const uint64_t width = static_cast<uint64_t>(
                static_cast<int64_t>(candidate.bounds.max_x) -
                candidate.bounds.min_x + 1);
            const uint64_t depth = static_cast<uint64_t>(
                static_cast<int64_t>(candidate.bounds.max_z) -
                candidate.bounds.min_z + 1);
            candidate.dense_group = static_cast<uint32_t>(dense_groups_.size());
            dense_groups_.push_back({candidate.bounds,
                                     static_cast<size_t>(width),
                                     static_cast<size_t>(depth),
                                     allocated_words, candidate.word_count});
            allocated_words += candidate.word_count;
        }
        initial_words_.resize(allocated_words);
        for (size_t index = 0; index < records.size(); ++index) {
            const uint32_t candidate_index = candidate_for_record[index];
            if (candidate_index == kSparseGroup) continue;
            dense_group_for_record_[index] = candidates[candidate_index].dense_group;
            const uint32_t group = dense_group_for_record_[index];
            if (group == kSparseGroup) continue;
            const std::optional<size_t> offset = localOffset(
                group, records[index].x, records[index].y, records[index].z);
            if (!offset) continue;
            const DenseGroup& layout = dense_groups_[group];
            initial_words_[layout.word_offset + (*offset >> 6)] |=
                uint64_t{1} << (*offset & 63);
        }
        remaining_words_ = initial_words_;
    }

    void reset() {
        std::fill(sparse_used_.begin(), sparse_used_.end(), false);
        remaining_words_ = initial_words_;
    }

    bool dense(size_t record_index) const {
        return dense_group_for_record_[record_index] != kSparseGroup;
    }

    bool available(size_t record_index) const {
        if (!dense(record_index)) return !sparse_used_[record_index];
        const RawRecord& record = records_[record_index];
        return allSetXRange(record_index, record.x, record.x, record.y, record.z);
    }

    bool matches(size_t seed_index, int32_t x, int32_t y, int32_t z) const {
        if (dense(seed_index)) return allSetXRange(seed_index, x, x, y, z);
        const std::optional<size_t> found = positions_.find(x, y, z);
        return found && !sparse_used_[*found] &&
               sameBlock(records_[*found], records_[seed_index]);
    }

    bool allSetXRange(size_t seed_index, int32_t min_x, int32_t max_x,
                      int32_t y, int32_t z) const {
        const uint32_t group = dense_group_for_record_[seed_index];
        if (group == kSparseGroup || min_x > max_x) return false;
        const std::optional<size_t> begin = localOffset(group, min_x, y, z);
        const std::optional<size_t> end = localOffset(group, max_x, y, z);
        if (!begin || !end || *end < *begin) return false;
        return allSetLocalRange(dense_groups_[group], *begin, *end - *begin + 1);
    }

    void clearXRange(size_t seed_index, int32_t min_x, int32_t max_x,
                     int32_t y, int32_t z) {
        const uint32_t group = dense_group_for_record_[seed_index];
        const std::optional<size_t> begin = localOffset(group, min_x, y, z);
        const std::optional<size_t> end = localOffset(group, max_x, y, z);
        if (!begin || !end || *end < *begin) return;
        clearLocalRange(dense_groups_[group], *begin, *end - *begin + 1);
    }

    void markSparseUsed(size_t record_index) {
        sparse_used_[record_index] = true;
    }

private:
    struct DenseGroup {
        BlockBounds bounds;
        size_t width = 0;
        size_t depth = 0;
        size_t word_offset = 0;
        size_t word_count = 0;
    };

    static uint64_t rangeMask(size_t bit_offset, size_t bit_count) {
        if (bit_count == 64) return std::numeric_limits<uint64_t>::max();
        return ((uint64_t{1} << bit_count) - 1) << bit_offset;
    }

    std::optional<size_t> localOffset(uint32_t group, int32_t x, int32_t y,
                                      int32_t z) const {
        if (group == kSparseGroup || group >= dense_groups_.size()) return std::nullopt;
        const DenseGroup& layout = dense_groups_[group];
        if (x < layout.bounds.min_x || x > layout.bounds.max_x ||
            y < layout.bounds.min_y || y > layout.bounds.max_y ||
            z < layout.bounds.min_z || z > layout.bounds.max_z) {
            return std::nullopt;
        }
        const uint64_t local_x = static_cast<uint64_t>(
            static_cast<int64_t>(x) - layout.bounds.min_x);
        const uint64_t local_y = static_cast<uint64_t>(
            static_cast<int64_t>(y) - layout.bounds.min_y);
        const uint64_t local_z = static_cast<uint64_t>(
            static_cast<int64_t>(z) - layout.bounds.min_z);
        return static_cast<size_t>((local_y * layout.depth + local_z) *
                                   layout.width + local_x);
    }

    bool allSetLocalRange(const DenseGroup& layout, size_t begin,
                          size_t count) const {
        while (count != 0) {
            const size_t word = begin >> 6;
            const size_t bit = begin & 63;
            const size_t take = std::min<size_t>(count, 64 - bit);
            if (word >= layout.word_count) return false;
            const uint64_t mask = rangeMask(bit, take);
            if ((remaining_words_[layout.word_offset + word] & mask) != mask) {
                return false;
            }
            begin += take;
            count -= take;
        }
        return true;
    }

    void clearLocalRange(const DenseGroup& layout, size_t begin, size_t count) {
        while (count != 0) {
            const size_t word = begin >> 6;
            const size_t bit = begin & 63;
            const size_t take = std::min<size_t>(count, 64 - bit);
            const uint64_t mask = rangeMask(bit, take);
            remaining_words_[layout.word_offset + word] &= ~mask;
            begin += take;
            count -= take;
        }
    }

    static constexpr uint32_t kSparseGroup = std::numeric_limits<uint32_t>::max();
    const std::vector<RawRecord>& records_;
    const RecordIndex& positions_;
    std::vector<bool> sparse_used_;
    std::vector<uint32_t> dense_group_for_record_;
    std::vector<DenseGroup> dense_groups_;
    std::vector<uint64_t> initial_words_;
    std::vector<uint64_t> remaining_words_;
};

bool appendExpectedAirSamples(const VerificationChunkPlan& metadata,
                              const PositionFlags& positions,
                              bool has_dynamic_blocks,
                              std::vector<VerificationPlanSample>* samples,
                              const CommandSpoolBuilder::CancelCheck& cancel_check,
                              std::string* error) {
    uint64_t volume = 0;
    if (!boundsVolume(metadata.imported_bounds, &volume)) {
        if (error) *error = "verification bounds are too large";
        return false;
    }
    if (static_cast<uint64_t>(positions.occupiedCount()) >= volume || has_dynamic_blocks) {
        // Gravity and liquid blocks can legitimately move into cleared air.
        // Omitting air probes for those partitions avoids false repair loops;
        // stable placed blocks are still sampled normally.
        return true;
    }

    const uint64_t width = static_cast<uint64_t>(
        static_cast<int64_t>(metadata.imported_bounds.max_x) - metadata.imported_bounds.min_x + 1);
    const uint64_t depth = static_cast<uint64_t>(
        static_cast<int64_t>(metadata.imported_bounds.max_z) - metadata.imported_bounds.min_z + 1);
    const uint64_t plane = width * depth;
    uint64_t cursor = mix64((uint64_t(uint32_t(metadata.coord.x)) << 32) |
                            uint32_t(metadata.coord.z)) % volume;
    uint64_t step = mix64(cursor ^ volume ^ metadata.total_block_count) % volume;
    if (step == 0) step = 1;
    while (std::gcd(step, volume) != 1) {
        if (++step == volume) step = 1;
    }

    const uint8_t air_flags = static_cast<uint8_t>(VerificationSampleFlag::ExpectedAir) |
                              static_cast<uint8_t>(VerificationSampleFlag::IgnoreAux);
    const uint64_t probe_count = std::min(volume, kMaximumAirProbeCount);
    uint32_t selected = 0;
    for (uint64_t visited = 0; visited < probe_count && selected < kAirSamplesPerChunk;
         ++visited) {
        if ((visited & 0xFFF) == 0 && cancellationRequested(cancel_check, error)) return false;
        const uint64_t x_offset = cursor % width;
        const uint64_t yz = cursor / width;
        const uint64_t z_offset = yz % depth;
        const uint64_t y_offset = cursor / plane;
        const Position position{
            static_cast<int32_t>(static_cast<int64_t>(metadata.imported_bounds.min_x) +
                                 static_cast<int64_t>(x_offset)),
            static_cast<int32_t>(static_cast<int64_t>(metadata.imported_bounds.min_y) +
                                 static_cast<int64_t>(y_offset)),
            static_cast<int32_t>(static_cast<int64_t>(metadata.imported_bounds.min_z) +
                                 static_cast<int64_t>(z_offset)),
        };
        const bool stable_air = !positions.occupied(position) &&
                                !positions.unstable(position);
        if (stable_air) {
            samples->push_back({metadata.coord, position.x, position.y, position.z,
                                "minecraft:air", 0, air_flags});
            ++selected;
        }
        cursor = cursor >= volume - step ? cursor - (volume - step) : cursor + step;
    }
    return true;
}

bool mergeRecords(RawBatch batch, ImportPhase phase,
                   uint32_t configured_limit,
                   const CommandSpoolBuilder::CancelCheck& cancel_check,
                   std::vector<PlannedCommand>* output, std::string* error) {
    if (cancellationRequested(cancel_check, error)) return false;
    std::vector<RawRecord>& records = batch.records;
    const std::vector<std::string>& names = batch.nameTable();
    std::vector<uint8_t> bed_names;
    if (phase == ImportPhase::Attachment) {
        bed_names.reserve(names.size());
        for (const std::string& name : names) {
            bed_names.push_back(isBed(name) ? 1U : 0U);
        }
    }
    const auto priorityFor = [&](const RawRecord& record) {
        if (phase != ImportPhase::Attachment || record.name_index >= bed_names.size() ||
            bed_names[record.name_index] == 0) return 2;
        return (record.aux & 0x08) == 0 ? 0 : 1;
    };
    struct SortCancelled {};
    uint64_t comparisons = 0;
    const auto recordPrecedes = [&](const RawRecord& left, const RawRecord& right) {
        const int left_priority = priorityFor(left);
        const int right_priority = priorityFor(right);
        if (left_priority != right_priority) return left_priority < right_priority;
        if (left.y != right.y) return left.y < right.y;
        if (left.z != right.z) return left.z < right.z;
        if (left.x != right.x) return left.x < right.x;
        if (left.name_index != right.name_index) return left.name_index < right.name_index;
        return left.aux < right.aux;
    };
    bool already_ordered = batch.coordinate_ordered && phase != ImportPhase::Attachment;
    if (phase == ImportPhase::Attachment) {
        already_ordered = true;
        for (size_t index = 1; index < records.size(); ++index) {
            if (recordPrecedes(records[index], records[index - 1])) {
                already_ordered = false;
                break;
            }
        }
    }
    if (!already_ordered) {
        try {
            std::sort(records.begin(), records.end(), [&](const RawRecord& left,
                                                           const RawRecord& right) {
                if ((++comparisons & 0xFFF) == 0 &&
                    cancellationRequested(cancel_check, error)) {
                    throw SortCancelled{};
                }
                return recordPrecedes(left, right);
            });
        } catch (const SortCancelled&) {
            return false;
        }
    }
    if (cancellationRequested(cancel_check, error)) return false;

    RecordIndex positions(records);
    if (!positions.valid()) {
        if (error) {
            *error = positions.duplicatePosition()
                ? "raw chunk phase contains duplicate block position"
                : "raw chunk phase is too large to index";
        }
        return false;
    }
    MergeCoverState cover_state(records, names, positions, phase);

    struct MergeBounds {
        std::array<int32_t, 3> low{};
        std::array<int32_t, 3> high{};
        uint64_t volume = 1;
    };
    const auto volumeOf = [](const MergeBounds& bounds) {
        return uint64_t(static_cast<int64_t>(bounds.high[0]) - bounds.low[0] + 1) *
               uint64_t(static_cast<int64_t>(bounds.high[1]) - bounds.low[1] + 1) *
               uint64_t(static_cast<int64_t>(bounds.high[2]) - bounds.low[2] + 1);
    };
    static constexpr int kAxisOrders[6][3] = {
        {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0},
    };
    static constexpr int kSingleAxisOrders[3][1] = {{0}, {1}, {2}};
    uint32_t merge_work = 0;
    const auto cancelledPeriodically = [&]() {
        return (++merge_work & 0x3FF) == 0 &&
               cancellationRequested(cancel_check, error);
    };
    // A locally largest cuboid is not always the best cover for a whole
    // phase. For example, two one-block-offset horizontal stripes become
    // three commands if the first seed consumes their 2x2 overlap, but only
    // two commands when X runs are completed before Z merging. Build the
    // adaptive cover as the no-regression baseline, then compare whole-phase
    // fixed full-axis and single-axis covers and retain the one with the
    // fewest RPC commands.
    const auto buildCover = [&](const int* fixed_order,
                                size_t fixed_axis_count,
                                std::vector<PlannedCommand>* commands) {
        cover_state.reset();
        bool merge_cancelled = false;
        const auto completeFace = [&](const MergeBounds& bounds, int axis,
                                      size_t seed_index) {
            const bool dense = cover_state.dense(seed_index);
            if (axis == 0) {
                for (int32_t y = bounds.low[1]; ; ++y) {
                    for (int32_t z = bounds.low[2]; ; ++z) {
                        if (cancelledPeriodically()) {
                            merge_cancelled = true;
                            return false;
                        }
                        if (!cover_state.matches(seed_index, bounds.high[0], y, z)) {
                            return false;
                        }
                        if (z == bounds.high[2]) break;
                    }
                    if (y == bounds.high[1]) break;
                }
                return true;
            }
            if (axis == 1) {
                for (int32_t z = bounds.low[2]; ; ++z) {
                    if (cancelledPeriodically()) {
                        merge_cancelled = true;
                        return false;
                    }
                    if (dense) {
                        if (!cover_state.allSetXRange(
                                seed_index, bounds.low[0], bounds.high[0],
                                bounds.high[1], z)) {
                            return false;
                        }
                    } else {
                        for (int32_t x = bounds.low[0]; ; ++x) {
                            if (cancelledPeriodically()) {
                                merge_cancelled = true;
                                return false;
                            }
                            if (!cover_state.matches(
                                    seed_index, x, bounds.high[1], z)) {
                                return false;
                            }
                            if (x == bounds.high[0]) break;
                        }
                    }
                    if (z == bounds.high[2]) break;
                }
                return true;
            }
            for (int32_t y = bounds.low[1]; ; ++y) {
                if (cancelledPeriodically()) {
                    merge_cancelled = true;
                    return false;
                }
                if (dense) {
                    if (!cover_state.allSetXRange(
                            seed_index, bounds.low[0], bounds.high[0], y,
                            bounds.high[2])) {
                        return false;
                    }
                } else {
                    for (int32_t x = bounds.low[0]; ; ++x) {
                        if (cancelledPeriodically()) {
                            merge_cancelled = true;
                            return false;
                        }
                        if (!cover_state.matches(
                                seed_index, x, y, bounds.high[2])) {
                            return false;
                        }
                        if (x == bounds.high[0]) break;
                    }
                }
                if (y == bounds.high[1]) break;
            }
            return true;
        };

        commands->clear();
        commands->reserve(std::min<size_t>(records.size(), 4096));
        for (size_t index = 0; index < records.size(); ++index) {
            if ((index & 0x3F) == 0 && cancellationRequested(cancel_check, error)) {
                return false;
            }
            if (!cover_state.available(index)) continue;
            const RawRecord& seed = records[index];
            const bool layer_only = singleLayer(seed, phase);
            const uint64_t limit = configured_limit;
            MergeBounds best{{seed.x, seed.y, seed.z},
                             {seed.x, seed.y, seed.z}, 1};
            if (canFill(seed, phase, names)) {
                // Single-layer phases have only two distinct axis orders.
                // Avoid evaluating the four duplicates created by skipping Y.
                static constexpr int kLayerAxisOrderIndices[2] = {0, 4};
                const size_t order_count = fixed_order ? 1 : (layer_only ? 2 : 6);
                for (size_t candidate_index = 0; candidate_index < order_count;
                     ++candidate_index) {
                    const int* order = fixed_order ? fixed_order :
                        kAxisOrders[layer_only
                            ? kLayerAxisOrderIndices[candidate_index]
                            : candidate_index];
                    MergeBounds candidate{{seed.x, seed.y, seed.z},
                                          {seed.x, seed.y, seed.z}, 1};
                    const size_t axis_count = fixed_order ? fixed_axis_count : 3;
                    for (size_t order_index = 0; order_index < axis_count; ++order_index) {
                        const int axis = order[order_index];
                        if (layer_only && axis == 1) continue;
                        for (;;) {
                            if (cancelledPeriodically()) return false;
                            if (candidate.high[axis] ==
                                std::numeric_limits<int32_t>::max()) break;
                            MergeBounds expanded = candidate;
                            ++expanded.high[axis];
                            if (volumeOf(expanded) > limit) break;
                            if (!completeFace(expanded, axis, index)) {
                                if (merge_cancelled) return false;
                                break;
                            }
                            candidate = expanded;
                        }
                    }
                    candidate.volume = volumeOf(candidate);
                    if (candidate.volume > best.volume) best = candidate;
                    if (best.volume == limit) break;
                }
            }
            if (cover_state.dense(index)) {
                for (int32_t y = best.low[1]; ; ) {
                    for (int32_t z = best.low[2]; ; ) {
                        if (cancelledPeriodically()) return false;
                        cover_state.clearXRange(
                            index, best.low[0], best.high[0], y, z);
                        if (z == best.high[2]) break;
                        ++z;
                    }
                    if (y == best.high[1]) break;
                    ++y;
                }
            } else {
                for (int32_t y = best.low[1]; ; ) {
                    for (int32_t z = best.low[2]; ; ) {
                        for (int32_t x = best.low[0]; ; ) {
                            if (cancelledPeriodically()) return false;
                            const std::optional<size_t> found = positions.find(x, y, z);
                            if (found) cover_state.markSparseUsed(*found);
                            if (x == best.high[0]) break;
                            ++x;
                        }
                        if (z == best.high[2]) break;
                        ++z;
                    }
                    if (y == best.high[1]) break;
                    ++y;
                }
            }
            commands->push_back({{best.low[0], best.low[1], best.low[2],
                                  best.high[0], best.high[1], best.high[2]},
                                 names[seed.name_index], seed.aux,
                                 static_cast<uint32_t>(best.volume),
                                 static_cast<uint8_t>(
                                     static_cast<uint8_t>(
                                         PlannedCommandFlag::HasMergeMetadata) |
                                      (canFill(seed, phase, names)
                                          ? static_cast<uint8_t>(
                                                PlannedCommandFlag::Fillable)
                                          : 0U) |
                                     (layer_only
                                          ? static_cast<uint8_t>(
                                                PlannedCommandFlag::SingleLayer)
                                          : 0U))});
        }
        return true;
    };

    std::vector<PlannedCommand> best_commands;
    if (!buildCover(nullptr, 0, &best_commands)) return false;

    uint64_t fillable_count = 0;
    uint64_t non_fillable_count = 0;
    std::map<MergeGroupKey, uint64_t> merge_group_counts;
    for (size_t index = 0; index < records.size(); ++index) {
        if ((index & 0xFFF) == 0 && cancellationRequested(cancel_check, error)) {
            return false;
        }
        const RawRecord& record = records[index];
        if (!canFill(record, phase, names)) {
            ++non_fillable_count;
            continue;
        }
        ++fillable_count;
        const bool layer_only = singleLayer(record, phase);
        ++merge_group_counts[{record.name_index, layer_only ? record.y : 0,
                              record.aux, record.flags, layer_only}];
    }
    uint64_t command_lower_bound = non_fillable_count;
    for (const auto& group : merge_group_counts) {
        command_lower_bound +=
            (group.second + configured_limit - 1) / configured_limit;
    }
    if (records.size() <= kMaximumExhaustiveMergeRecordCount && fillable_count > 1 &&
        best_commands.size() > command_lower_bound) {
        std::vector<PlannedCommand> candidate_commands;
        // Gravity and fluid phases cannot merge across Y, leaving only X/Z
        // and Z/X as distinct whole-phase strategies.
        static constexpr int kLayerPlanOrderIndices[2] = {0, 4};
        const bool phase_is_single_layer = phase == ImportPhase::Gravity ||
                                           phase == ImportPhase::Fluid;
        const size_t plan_count = phase_is_single_layer ? 2 : 6;
        for (size_t plan_index = 0; plan_index < plan_count; ++plan_index) {
            const int order_index = phase_is_single_layer
                ? kLayerPlanOrderIndices[plan_index]
                : static_cast<int>(plan_index);
            if (!buildCover(kAxisOrders[order_index], 3,
                            &candidate_commands)) return false;
            if (candidate_commands.size() < best_commands.size()) {
                best_commands.swap(candidate_commands);
                if (best_commands.size() == command_lower_bound) break;
            }
        }
        // Expanding across a second axis can consume an overlap that would
        // otherwise allow a longer run later in the scan. Pure-axis covers
        // handle those small stair-step and offset-edge counterexamples.
        static constexpr int kLayerSingleAxisIndices[2] = {0, 2};
        const size_t single_plan_count = phase_is_single_layer ? 2 : 3;
        for (size_t plan_index = 0;
             plan_index < single_plan_count &&
                 best_commands.size() > command_lower_bound;
             ++plan_index) {
            const int axis_index = phase_is_single_layer
                ? kLayerSingleAxisIndices[plan_index]
                : static_cast<int>(plan_index);
            if (!buildCover(kSingleAxisOrders[axis_index], 1,
                            &candidate_commands)) return false;
            if (candidate_commands.size() < best_commands.size()) {
                best_commands.swap(candidate_commands);
            }
        }
    }
    *output = std::move(best_commands);
    return true;
}

bool writeCommandSpool(const std::string& path, const std::vector<PlannedCommand>& commands,
                       uint64_t block_count,
                       const CommandSpoolBuilder::CancelCheck& cancel_check,
                       std::string* error) {
    uint64_t command_blocks = 0;
    for (size_t index = 0; index < commands.size(); ++index) {
        if ((index & 0x3FF) == 0 && cancellationRequested(cancel_check, error)) return false;
        const PlannedCommand& command = commands[index];
        uint64_t volume = 0;
        if (command.name.empty() || command.name.size() > kMaxBlockNameLength ||
            !boundsVolume(command.bounds, &volume) || volume != command.block_count ||
            command.block_count == 0 || !validPlannedCommandFlags(command.flags) ||
            !addWithoutOverflow(command_blocks, command.block_count, &command_blocks)) {
            if (error) *error = "invalid merged command";
            return false;
        }
    }
    if (command_blocks != block_count || (commands.empty() != (block_count == 0))) {
        if (error) *error = "merged command block count mismatch";
        return false;
    }
    const std::string temporary = path + ".tmp";
    TemporaryOutputFile output_file(temporary);
    std::ofstream& stream = output_file.stream();
    const uint64_t command_count = commands.size();
    if (!stream || !writeValue(stream, kCommandMagic) || !writeValue(stream, kCommandVersion) ||
        !writeValue(stream, command_count) || !writeValue(stream, block_count)) {
        if (error) *error = "cannot create merged command spool";
        return false;
    }
    for (size_t index = 0; index < commands.size(); ++index) {
        if ((index & 0x3FF) == 0 && cancellationRequested(cancel_check, error)) {
            return false;
        }
        const PlannedCommand& command = commands[index];
        const uint16_t name_length = static_cast<uint16_t>(command.name.size());
        if (!writeValue(stream, command.bounds.min_x) || !writeValue(stream, command.bounds.min_y) ||
            !writeValue(stream, command.bounds.min_z) || !writeValue(stream, command.bounds.max_x) ||
            !writeValue(stream, command.bounds.max_y) || !writeValue(stream, command.bounds.max_z) ||
            !writeValue(stream, command.block_count) || !writeValue(stream, command.aux) ||
            !writeValue(stream, command.flags) || !writeValue(stream, name_length)) {
            if (error) *error = "cannot write merged command spool";
            return false;
        }
        stream.write(command.name.data(), name_length);
        if (!stream) {
            if (error) *error = "cannot write merged command spool";
            return false;
        }
    }
    if (!output_file.commit(path)) {
        if (error) *error = "cannot finalize merged command spool";
        return false;
    }
    return true;
}

struct CommandSpoolSource {
    std::string path;
    uint64_t block_count = 0;
};

bool inspectCommandSpools(const std::vector<CommandSpoolSource>& sources,
                          uint64_t expected_block_count,
                          uint64_t* command_count,
                          const CommandSpoolBuilder::CancelCheck& cancel_check,
                          std::string* error) {
    uint64_t commands = 0;
    uint64_t blocks = 0;
    for (size_t index = 0; index < sources.size(); ++index) {
        if ((index & 0x3F) == 0 && cancellationRequested(cancel_check, error)) return false;
        CommandSpoolReader reader(sources[index].path);
        uint64_t next_commands = 0;
        uint64_t next_blocks = 0;
        if (!reader.valid() || reader.totalBlockCount() != sources[index].block_count ||
            !addWithoutOverflow(commands, reader.commandCount(), &next_commands) ||
            !addWithoutOverflow(blocks, reader.totalBlockCount(), &next_blocks)) {
            if (error) *error = "invalid chunk command spool";
            return false;
        }
        commands = next_commands;
        blocks = next_blocks;
    }
    if (sources.empty() || commands == 0 || blocks != expected_block_count) {
        if (error) *error = "region command spool count mismatch";
        return false;
    }
    *command_count = commands;
    return true;
}

bool writeCommandRecord(std::ofstream& stream, const PlannedCommand& command,
                        std::string* error) {
    uint64_t volume = 0;
    if (command.name.empty() || command.name.size() > kMaxBlockNameLength ||
        command.block_count == 0 || !boundsVolume(command.bounds, &volume) ||
        volume != command.block_count || !validPlannedCommandFlags(command.flags)) {
        if (error) *error = "invalid merged command";
        return false;
    }
    const uint16_t name_length = static_cast<uint16_t>(command.name.size());
    if (!writeValue(stream, command.bounds.min_x) ||
        !writeValue(stream, command.bounds.min_y) ||
        !writeValue(stream, command.bounds.min_z) ||
        !writeValue(stream, command.bounds.max_x) ||
        !writeValue(stream, command.bounds.max_y) ||
        !writeValue(stream, command.bounds.max_z) ||
        !writeValue(stream, command.block_count) ||
        !writeValue(stream, command.aux) || !writeValue(stream, command.flags) ||
        !writeValue(stream, name_length)) {
        if (error) *error = "cannot write merged command spool";
        return false;
    }
    stream.write(command.name.data(), name_length);
    if (!stream) {
        if (error) *error = "cannot write merged command spool";
        return false;
    }
    return true;
}

bool writeConcatenatedCommandSpool(
        const std::string& path, const std::vector<CommandSpoolSource>& sources,
        uint64_t command_count, uint64_t block_count,
        const CommandSpoolBuilder::CancelCheck& cancel_check, std::string* error) {
    const std::string temporary = path + ".tmp";
    TemporaryOutputFile output_file(temporary);
    std::ofstream& stream = output_file.stream();
    if (!stream || !writeValue(stream, kCommandMagic) ||
        !writeValue(stream, kCommandVersion) || !writeValue(stream, command_count) ||
        !writeValue(stream, block_count)) {
        if (error) *error = "cannot create merged command spool";
        return false;
    }

    uint64_t written_commands = 0;
    uint64_t written_blocks = 0;
    for (const CommandSpoolSource& source : sources) {
        CommandSpoolReader reader(source.path);
        if (!reader.valid() || reader.totalBlockCount() != source.block_count) {
            if (error) *error = "invalid chunk command spool";
            return false;
        }
        while (const std::optional<PlannedCommand> command = reader.next()) {
            if ((written_commands & 0x3FF) == 0 &&
                cancellationRequested(cancel_check, error)) return false;
            uint64_t next_blocks = 0;
            if (!addWithoutOverflow(written_blocks, command->block_count, &next_blocks) ||
                !writeCommandRecord(stream, *command, error)) return false;
            written_blocks = next_blocks;
            ++written_commands;
        }
        if (reader.failed() || !reader.exhausted()) {
            if (error) *error = "invalid chunk command spool";
            return false;
        }
    }
    if (written_commands != command_count || written_blocks != block_count) {
        if (error) *error = "region command spool count mismatch";
        return false;
    }
    if (!output_file.commit(path)) {
        if (error) *error = "cannot finalize merged command spool";
        return false;
    }
    return true;
}

bool readCommandSpools(const std::vector<CommandSpoolSource>& sources,
                       uint64_t command_count,
                       const CommandSpoolBuilder::CancelCheck& cancel_check,
                       std::vector<PlannedCommand>* commands, std::string* error) {
    if (command_count > kMaximumPlanarRemergeBlockCount ||
        command_count > std::numeric_limits<size_t>::max()) {
        if (error) *error = "region command spool is too large to optimize";
        return false;
    }
    commands->clear();
    commands->reserve(static_cast<size_t>(command_count));
    for (const CommandSpoolSource& source : sources) {
        CommandSpoolReader reader(source.path);
        if (!reader.valid() || reader.totalBlockCount() != source.block_count) {
            if (error) *error = "invalid chunk command spool";
            return false;
        }
        while (const std::optional<PlannedCommand> command = reader.next()) {
            if ((commands->size() & 0x3FF) == 0 &&
                cancellationRequested(cancel_check, error)) return false;
            commands->push_back(std::move(*command));
        }
        if (reader.failed() || !reader.exhausted()) {
            if (error) *error = "invalid chunk command spool";
            return false;
        }
    }
    if (commands->size() != command_count) {
        if (error) *error = "region command spool count mismatch";
        return false;
    }
    return true;
}

int compareForStitchAxis(const PlannedCommand& left,
                         const PlannedCommand& right, int axis) {
    if (left.name != right.name) return left.name < right.name ? -1 : 1;
    if (left.aux != right.aux) return left.aux < right.aux ? -1 : 1;
    if (left.flags != right.flags) return left.flags < right.flags ? -1 : 1;
    const BlockBounds& a = left.bounds;
    const BlockBounds& b = right.bounds;
    const auto compare = [](int32_t first, int32_t second) {
        return first < second ? -1 : (first > second ? 1 : 0);
    };
    const auto minimum = [](const BlockBounds& bounds, int selected_axis) {
        if (selected_axis == 0) return bounds.min_x;
        if (selected_axis == 1) return bounds.min_y;
        return bounds.min_z;
    };
    const auto maximum = [](const BlockBounds& bounds, int selected_axis) {
        if (selected_axis == 0) return bounds.max_x;
        if (selected_axis == 1) return bounds.max_y;
        return bounds.max_z;
    };
    for (int selected_axis = 0; selected_axis < 3; ++selected_axis) {
        if (selected_axis == axis) continue;
        int order = compare(minimum(a, selected_axis), minimum(b, selected_axis));
        if (order != 0) return order;
        order = compare(maximum(a, selected_axis), maximum(b, selected_axis));
        if (order != 0) return order;
    }
    int order = compare(minimum(a, axis), minimum(b, axis));
    if (order != 0) return order;
    order = compare(maximum(a, axis), maximum(b, axis));
    if (order != 0) return order;
    if (left.block_count != right.block_count) {
        return left.block_count < right.block_count ? -1 : 1;
    }
    return 0;
}

bool canStitch(const PlannedCommand& left, const PlannedCommand& right,
               int axis, uint32_t merge_limit) {
    if (axis < 0 || axis > 2 || left.name != right.name ||
        left.aux != right.aux || left.flags != right.flags ||
        static_cast<uint64_t>(left.block_count) + right.block_count > merge_limit) {
        return false;
    }
    const bool has_metadata = hasPlannedCommandFlag(
        left, PlannedCommandFlag::HasMergeMetadata);
    if (has_metadata) {
        if (!hasPlannedCommandFlag(left, PlannedCommandFlag::Fillable)) return false;
    } else if (left.block_count <= 1 || right.block_count <= 1) {
        // Version 1 spools did not retain fill eligibility. Preserve their
        // conservative behavior: a multi-block command proves /fill was safe,
        // but a singleton may have originated from a stateful block.
        return false;
    }
    if (axis == 1) {
        // Version 1 has no SingleLayer metadata, so vertical stitching cannot
        // prove that a legacy gravity/fluid command is safe to extend.
        if (!has_metadata || hasPlannedCommandFlag(
                left, PlannedCommandFlag::SingleLayer)) return false;
    }
    const auto minimum = [](const BlockBounds& bounds, int selected_axis) {
        if (selected_axis == 0) return bounds.min_x;
        if (selected_axis == 1) return bounds.min_y;
        return bounds.min_z;
    };
    const auto maximum = [](const BlockBounds& bounds, int selected_axis) {
        if (selected_axis == 0) return bounds.max_x;
        if (selected_axis == 1) return bounds.max_y;
        return bounds.max_z;
    };
    for (int selected_axis = 0; selected_axis < 3; ++selected_axis) {
        if (selected_axis == axis) continue;
        if (minimum(left.bounds, selected_axis) !=
                minimum(right.bounds, selected_axis) ||
            maximum(left.bounds, selected_axis) !=
                maximum(right.bounds, selected_axis)) {
            return false;
        }
    }
    const int32_t left_maximum = maximum(left.bounds, axis);
    return left_maximum != std::numeric_limits<int32_t>::max() &&
           left_maximum + 1 == minimum(right.bounds, axis);
}

struct StitchSortCancelled {};

bool stitchAxis(std::vector<PlannedCommand>* commands, int axis,
                uint32_t merge_limit,
                const CommandSpoolBuilder::CancelCheck& cancel_check,
                bool* changed, std::string* error) {
    uint64_t comparisons = 0;
    try {
        std::sort(commands->begin(), commands->end(),
                  [&](const PlannedCommand& left, const PlannedCommand& right) {
            if ((++comparisons & 0xFFF) == 0 &&
                cancellationRequested(cancel_check, error)) {
                throw StitchSortCancelled{};
            }
            return compareForStitchAxis(left, right, axis) < 0;
        });
    } catch (const StitchSortCancelled&) {
        return false;
    }

    size_t output_index = 0;
    for (size_t index = 0; index < commands->size(); ++index) {
        if ((index & 0x3FF) == 0 && cancellationRequested(cancel_check, error)) return false;
        PlannedCommand& command = (*commands)[index];
        if (output_index != 0 &&
            canStitch((*commands)[output_index - 1], command, axis, merge_limit)) {
            PlannedCommand& previous = (*commands)[output_index - 1];
            if (axis == 0) previous.bounds.max_x = command.bounds.max_x;
            else if (axis == 1) previous.bounds.max_y = command.bounds.max_y;
            else previous.bounds.max_z = command.bounds.max_z;
            previous.block_count += command.block_count;
            *changed = true;
        } else {
            if (output_index != index) {
                (*commands)[output_index] = std::move(command);
            }
            ++output_index;
        }
    }
    commands->resize(output_index);
    return true;
}

bool stitchInOrder(std::vector<PlannedCommand>* commands,
                   const std::array<int, 3>& axes, uint32_t merge_limit,
                   const CommandSpoolBuilder::CancelCheck& cancel_check,
                   std::string* error) {
    for (size_t pass = 0; pass < kMaximumStitchPassPairs; ++pass) {
        bool pass_changed = false;
        for (int axis : axes) {
            bool axis_changed = false;
            if (!stitchAxis(commands, axis, merge_limit, cancel_check,
                            &axis_changed, error)) return false;
            pass_changed = pass_changed || axis_changed;
        }
        if (!pass_changed) return true;
    }
    return true;
}

bool sortCommandsForExecution(std::vector<PlannedCommand>* commands,
                              const CommandSpoolBuilder::CancelCheck& cancel_check,
                              std::string* error) {
    uint64_t comparisons = 0;
    try {
        std::sort(commands->begin(), commands->end(),
                  [&](const PlannedCommand& left, const PlannedCommand& right) {
            if ((++comparisons & 0xFFF) == 0 &&
                cancellationRequested(cancel_check, error)) {
                throw StitchSortCancelled{};
            }
            const BlockBounds& a = left.bounds;
            const BlockBounds& b = right.bounds;
            if (a.min_y != b.min_y) return a.min_y < b.min_y;
            if (a.min_z != b.min_z) return a.min_z < b.min_z;
            if (a.min_x != b.min_x) return a.min_x < b.min_x;
            if (a.max_y != b.max_y) return a.max_y < b.max_y;
            if (a.max_z != b.max_z) return a.max_z < b.max_z;
            if (a.max_x != b.max_x) return a.max_x < b.max_x;
            if (left.name != right.name) return left.name < right.name;
            if (left.aux != right.aux) return left.aux < right.aux;
            return left.flags < right.flags;
        });
    } catch (const StitchSortCancelled&) {
        return false;
    }
    return !cancellationRequested(cancel_check, error);
}

bool stitchRegionCommands(std::vector<PlannedCommand>* commands,
                          uint32_t merge_limit,
                          const CommandSpoolBuilder::CancelCheck& cancel_check,
                          std::string* error) {
    if (commands->size() > kMaximumDualCandidateStitchCommandCount) {
        if (!stitchInOrder(commands, {{0, 2, 1}}, merge_limit,
                           cancel_check, error)) return false;
        return sortCommandsForExecution(commands, cancel_check, error);
    }
    std::vector<PlannedCommand> x_then_z = *commands;
    std::vector<PlannedCommand> z_then_x = std::move(*commands);
    if (!stitchInOrder(&x_then_z, {{0, 2, 1}}, merge_limit, cancel_check, error) ||
        !stitchInOrder(&z_then_x, {{2, 0, 1}}, merge_limit, cancel_check, error)) {
        return false;
    }
    *commands = x_then_z.size() <= z_then_x.size()
        ? std::move(x_then_z) : std::move(z_then_x);
    return sortCommandsForExecution(commands, cancel_check, error);
}

struct RegionRemergeGroupKey {
    std::string name;
    int32_t y = 0;
    uint16_t aux = 0;
    uint8_t flags = 0;

    bool operator<(const RegionRemergeGroupKey& other) const {
        if (name != other.name) return name < other.name;
        if (aux != other.aux) return aux < other.aux;
        if (flags != other.flags) return flags < other.flags;
        return y < other.y;
    }
};

bool remergeRegionCommands(std::vector<PlannedCommand>* commands,
                           ImportPhase phase, uint32_t merge_limit,
                           uint64_t expected_block_count,
                           const CommandSpoolBuilder::CancelCheck& cancel_check,
                           std::string* error) {
    if (commands->size() < 2 ||
        (phase != ImportPhase::Structure && phase != ImportPhase::Gravity &&
         phase != ImportPhase::Fluid)) {
        return true;
    }
    if (cancellationRequested(cancel_check, error)) return false;
    bool all_single_layer = true;
    int32_t minimum_y = commands->front().bounds.min_y;
    int32_t maximum_y = commands->front().bounds.max_y;
    uint64_t total_block_count = 0;
    std::map<RegionRemergeGroupKey, uint64_t> group_counts;
    for (size_t index = 0; index < commands->size(); ++index) {
        if ((index & 0x3FF) == 0 && cancellationRequested(cancel_check, error)) {
            return false;
        }
        const PlannedCommand& command = (*commands)[index];
        if (!hasPlannedCommandFlag(
                command, PlannedCommandFlag::HasMergeMetadata)) {
            // Old command spools remain executable, but lack enough metadata
            // to reconstruct fill eligibility without changing semantics.
            return true;
        }
        const bool fillable = hasPlannedCommandFlag(
            command, PlannedCommandFlag::Fillable);
        const bool single_layer = hasPlannedCommandFlag(
            command, PlannedCommandFlag::SingleLayer);
        if ((!fillable && command.block_count != 1) ||
            (single_layer && command.bounds.min_y != command.bounds.max_y) ||
            !addWithoutOverflow(total_block_count, command.block_count,
                                &total_block_count)) {
            if (error) *error = "invalid region command merge metadata";
            return false;
        }
        all_single_layer = all_single_layer && single_layer;
        minimum_y = std::min(minimum_y, command.bounds.min_y);
        maximum_y = std::max(maximum_y, command.bounds.max_y);
        RegionRemergeGroupKey key{
            command.name, single_layer ? command.bounds.min_y : 0,
            command.aux, command.flags};
        uint64_t& count = group_counts[key];
        if (!addWithoutOverflow(count, command.block_count, &count)) {
            if (error) *error = "region merge group block count overflow";
            return false;
        }
    }
    if (total_block_count != expected_block_count) {
        if (error) *error = "region command block count mismatch";
        return false;
    }

    const bool planar = minimum_y == maximum_y;
    const uint64_t remerge_limit = planar || all_single_layer
        ? kMaximumPlanarRemergeBlockCount
        : kMaximumThreeDimensionalRemergeBlockCount;
    if (total_block_count > remerge_limit ||
        total_block_count > std::numeric_limits<size_t>::max()) {
        return true;
    }

    uint64_t command_lower_bound = 0;
    for (const auto& group : group_counts) {
        const bool fillable =
            (group.first.flags &
             static_cast<uint8_t>(PlannedCommandFlag::Fillable)) != 0;
        const uint64_t group_lower_bound = fillable
            ? (group.second + merge_limit - 1) / merge_limit
            : group.second;
        if (!addWithoutOverflow(command_lower_bound, group_lower_bound,
                                &command_lower_bound)) {
            if (error) *error = "region command lower bound overflow";
            return false;
        }
    }
    if (commands->size() <= command_lower_bound) return true;

    RawBatch batch;
    batch.records.reserve(static_cast<size_t>(total_block_count));
    std::unordered_map<std::string, uint32_t> name_indices;
    name_indices.reserve(group_counts.size());
    for (size_t command_index = 0; command_index < commands->size(); ++command_index) {
        const PlannedCommand& command = (*commands)[command_index];
        auto inserted = name_indices.emplace(
            command.name, static_cast<uint32_t>(batch.names.size()));
        if (inserted.second) batch.names.push_back(command.name);
        const uint32_t name_index = inserted.first->second;
        const uint8_t normalized_flags = static_cast<uint8_t>(
            0x04U |
            (hasPlannedCommandFlag(command, PlannedCommandFlag::Fillable)
                 ? 0x01U : 0U) |
            (hasPlannedCommandFlag(command, PlannedCommandFlag::SingleLayer)
                 ? 0x02U : 0U));
        for (int32_t y = command.bounds.min_y; ; ++y) {
            for (int32_t z = command.bounds.min_z; ; ++z) {
                for (int32_t x = command.bounds.min_x; ; ++x) {
                    if ((batch.records.size() & 0xFFF) == 0 &&
                        cancellationRequested(cancel_check, error)) {
                        return false;
                    }
                    batch.records.push_back(
                        {x, y, z, command.aux, normalized_flags, name_index});
                    if (x == command.bounds.max_x) break;
                }
                if (z == command.bounds.max_z) break;
            }
            if (y == command.bounds.max_y) break;
        }
    }
    if (batch.records.size() != total_block_count) {
        if (error) *error = "region command expansion count mismatch";
        return false;
    }

    std::vector<PlannedCommand> candidate;
    if (!mergeRecords(std::move(batch), phase, merge_limit, cancel_check,
                      &candidate, error)) {
        return false;
    }
    uint64_t candidate_block_count = 0;
    for (const PlannedCommand& command : candidate) {
        if (!addWithoutOverflow(candidate_block_count, command.block_count,
                                &candidate_block_count)) {
            if (error) *error = "region remerge block count overflow";
            return false;
        }
    }
    if (candidate_block_count != total_block_count) {
        if (error) *error = "region remerge block count mismatch";
        return false;
    }
    if (candidate.size() < commands->size()) commands->swap(candidate);
    return true;
}

void unionBounds(BlockBounds* destination, const BlockBounds& source) {
    destination->min_x = std::min(destination->min_x, source.min_x);
    destination->min_y = std::min(destination->min_y, source.min_y);
    destination->min_z = std::min(destination->min_z, source.min_z);
    destination->max_x = std::max(destination->max_x, source.max_x);
    destination->max_y = std::max(destination->max_y, source.max_y);
    destination->max_z = std::max(destination->max_z, source.max_z);
}

bool aggregateRegionCommandSpools(
        const std::string& directory, int32_t region_span,
        const ChunkCoord& region_grid_origin,
        uint32_t static_merge_limit, uint32_t dynamic_merge_limit,
        std::vector<ChunkDescriptor>* chunks,
        const CommandSpoolBuilder::CancelCheck& cancel_check,
        const CommandSpoolBuilder::ProgressCallback& progress_callback,
        std::string* error) {
    std::map<ChunkCoord, std::vector<size_t>> grouped_indices;
    for (size_t index = 0; index < chunks->size(); ++index) {
        grouped_indices[regionForChunk((*chunks)[index].coord, region_span,
                                       region_grid_origin)].push_back(index);
    }

    std::vector<ChunkDescriptor> regions;
    regions.reserve(grouped_indices.size());
    reportProgress(progress_callback, CommandSpoolBuildStage::AggregatingRegions,
                   0, grouped_indices.size());
    size_t completed_regions = 0;
    for (auto& group : grouped_indices) {
        if (cancellationRequested(cancel_check, error)) return false;
        std::vector<size_t>& indices = group.second;
        std::sort(indices.begin(), indices.end(), [&](size_t left, size_t right) {
            return (*chunks)[left].coord < (*chunks)[right].coord;
        });
        ChunkDescriptor region;
        region.coord = (*chunks)[indices.front()].coord;
        region.region_grid_origin = region_grid_origin;
        region.imported_bounds = (*chunks)[indices.front()].imported_bounds;
        for (size_t index = 1; index < indices.size(); ++index) {
            unionBounds(&region.imported_bounds, (*chunks)[indices[index]].imported_bounds);
        }

        for (size_t phase_index = phaseIndex(ImportPhase::Structure);
             phase_index < kImportPhaseCount; ++phase_index) {
            std::vector<CommandSpoolSource> sources;
            sources.reserve(indices.size());
            uint64_t block_count = 0;
            for (const size_t index : indices) {
                const ChunkDescriptor& chunk = (*chunks)[index];
                if (!chunk.has_phase[phase_index]) continue;
                uint64_t next_block_count = 0;
                if (chunk.command_paths[phase_index].empty() ||
                    !addWithoutOverflow(block_count,
                                        chunk.phase_block_counts[phase_index],
                                        &next_block_count)) {
                    if (error) *error = "invalid chunk command descriptor";
                    return false;
                }
                block_count = next_block_count;
                sources.push_back({chunk.command_paths[phase_index],
                                   chunk.phase_block_counts[phase_index]});
            }
            if (sources.empty()) continue;

            uint64_t command_count = 0;
            if (!inspectCommandSpools(sources, block_count, &command_count,
                                      cancel_check, error)) return false;
            const std::string command_path = directory + "/region_" +
                std::to_string(group.first.x) + "_" +
                std::to_string(group.first.z) + "_phase_" +
                std::to_string(phase_index) + ".bsp.cmd";
            const ImportPhase phase = static_cast<ImportPhase>(phase_index);
            const bool stitchable_phase = phase == ImportPhase::Structure ||
                phase == ImportPhase::Gravity || phase == ImportPhase::Fluid;
            const uint32_t phase_merge_limit = phase == ImportPhase::Structure
                ? static_merge_limit : dynamic_merge_limit;
            // readCommandSpools materializes at most
            // kMaximumPlanarRemergeBlockCount commands, so stitching must not
            // admit more than it can read: a region in the gap between the
            // two caps falls through to plain concatenation instead of
            // failing the whole build with "too large to optimize".
            const bool can_stitch = stitchable_phase &&
                command_count <= std::min<uint64_t>(
                    kMaximumStitchCommandCount, kMaximumPlanarRemergeBlockCount);
            const bool can_remerge = stitchable_phase &&
                block_count <= kMaximumPlanarRemergeBlockCount &&
                command_count <= kMaximumPlanarRemergeBlockCount;
            if (can_stitch || can_remerge) {
                std::vector<PlannedCommand> commands;
                if (!readCommandSpools(sources, command_count, cancel_check,
                                       &commands, error)) {
                    return false;
                }
                if (can_stitch &&
                    !stitchRegionCommands(&commands, phase_merge_limit,
                                          cancel_check, error)) {
                    return false;
                }
                if (can_remerge &&
                    !remergeRegionCommands(&commands, phase, phase_merge_limit,
                                           block_count, cancel_check, error)) {
                    return false;
                }
                if (!writeCommandSpool(command_path, commands, block_count,
                                       cancel_check, error)) {
                    return false;
                }
            } else if (!writeConcatenatedCommandSpool(
                           command_path, sources, command_count, block_count,
                           cancel_check, error)) {
                return false;
            }
            for (const CommandSpoolSource& source : sources) {
                if (std::remove(source.path.c_str()) != 0) {
                    if (error) *error = "cannot remove chunk command spool";
                    return false;
                }
            }
            region.has_phase[phase_index] = true;
            region.command_paths[phase_index] = command_path;
            region.phase_block_counts[phase_index] = block_count;
        }
        regions.push_back(std::move(region));
        reportProgress(progress_callback, CommandSpoolBuildStage::AggregatingRegions,
                       ++completed_regions, grouped_indices.size());
    }
    *chunks = std::move(regions);
    return true;
}

bool optimizeChunkDescriptor(
        ChunkDescriptor* descriptor, OverwritePolicy overwrite_policy,
        uint32_t static_merge_limit, uint32_t dynamic_merge_limit,
        const std::vector<std::string>* raw_palette,
        bool include_verification_samples,
        bool allow_empty_descriptor,
        const CommandSpoolBuilder::CancelCheck& cancel_check,
        VerificationChunkPlan* output, std::string* error) {
    VerificationChunkPlan verification;
    verification.coord = descriptor->coord;
    verification.imported_bounds = descriptor->imported_bounds;
    bool has_raw_placement = false;
    for (size_t phase_index = phaseIndex(ImportPhase::Structure);
         phase_index < kImportPhaseCount; ++phase_index) {
        if (descriptor->has_phase[phase_index]) {
            has_raw_placement = true;
            break;
        }
    }
    if (!has_raw_placement && allow_empty_descriptor &&
        overwrite_policy == OverwritePolicy::PreserveExisting) {
        // A retained descriptor exists solely to drive an auxiliary plan over
        // a sparse source footprint. It deliberately contributes neither
        // source commands nor "expected air" verification samples.
        *output = std::move(verification);
        return true;
    }
    SampleBucket structure_samples;
    SampleBucket fluid_source_samples;
    SampleBucket flowing_fluid_samples;
    PositionFlags positions(descriptor->imported_bounds);
    bool has_dynamic_blocks = false;
    for (size_t phase_index = phaseIndex(ImportPhase::Structure);
         phase_index < kImportPhaseCount; ++phase_index) {
        if (cancellationRequested(cancel_check, error)) return false;
        if (!descriptor->has_phase[phase_index]) continue;
        RawBatch batch;
        if (!readRawRecords(descriptor->spool_paths[phase_index], raw_palette,
                            &batch, cancel_check, error)) return false;
        if (batch.records.empty()) {
            if (error) *error = "raw chunk phase spool is empty";
            return false;
        }
        const ImportPhase phase = static_cast<ImportPhase>(phase_index);
        const std::vector<std::string>& names = batch.nameTable();
        // First pass: validate records and identify duplicate coordinates.
        // Some BDX exporters emit duplicate coordinates; keep the first entry
        // and collect indices of subsequent duplicates so they can be removed
        // before mergeRecords builds the command spool.  Counts and the
        // verification plan must only reflect the deduplicated set.
        std::vector<size_t> duplicate_indices;
        for (size_t record_index = 0; record_index < batch.records.size(); ++record_index) {
            if ((record_index & 0xFFF) == 0 &&
                cancellationRequested(cancel_check, error)) return false;
            const RawRecord& record = batch.records[record_index];
            if (record.name_index >= names.size()) {
                if (error) *error = "raw chunk spool contains an invalid block-name index";
                return false;
            }
            const Position position{record.x, record.y, record.z};
            if (!contains(descriptor->imported_bounds, record.x, record.y, record.z) ||
                (record.flags & ~uint8_t(0x0F)) != 0) {
                if (error) *error = "raw chunk spool contains invalid or out-of-bounds blocks";
                return false;
            }
            if (!positions.insertOccupied(position)) {
                duplicate_indices.push_back(record_index);
                ++descriptor->duplicate_block_count;
                continue;
            }
            if (overwrite_policy == OverwritePolicy::ClearImportedBounds &&
                (phase == ImportPhase::Gravity || phase == ImportPhase::Fluid ||
                 (record.flags & 0x08) != 0)) {
                positions.markUnstable(position);
                has_dynamic_blocks = true;
            }
            SampleBucket* sample_bucket = &structure_samples;
            if (phase == ImportPhase::Fluid) {
                sample_bucket = record.aux == 0
                    ? &fluid_source_samples : &flowing_fluid_samples;
            }
            considerVerificationSample(record, names[record.name_index], phase,
                                        descriptor->coord,
                                        include_verification_samples
                                            ? kStableSamplesPerChunk : 0,
                                        sample_bucket);
        }
        // Remove duplicates from the batch (in reverse index order to preserve
        // indices) so mergeRecords only sees unique positions.
        for (auto it = duplicate_indices.rbegin(); it != duplicate_indices.rend(); ++it) {
            batch.records.erase(batch.records.begin() + static_cast<std::ptrdiff_t>(*it));
        }
        const size_t dedup_count = batch.records.size();
        descriptor->phase_block_counts[phase_index] = dedup_count;
        if (!addWithoutOverflow(verification.total_block_count, dedup_count,
                                &verification.total_block_count)) {
            if (error) *error = "chunk block count overflow";
            return false;
        }
        std::vector<PlannedCommand> commands;
        const uint32_t phase_merge_limit = phase == ImportPhase::Structure
            ? static_merge_limit : dynamic_merge_limit;
        if (!mergeRecords(std::move(batch), phase, phase_merge_limit,
                          cancel_check, &commands, error)) return false;
        const std::string command_path = descriptor->spool_paths[phase_index] + ".cmd";
        if (!writeCommandSpool(command_path, commands,
                               descriptor->phase_block_counts[phase_index],
                               cancel_check, error)) return false;
        descriptor->command_paths[phase_index] = command_path;
        if (std::remove(descriptor->spool_paths[phase_index].c_str()) == 0) {
            descriptor->spool_paths[phase_index].clear();
        }
    }
    if (verification.total_block_count == 0 &&
        overwrite_policy != OverwritePolicy::ClearImportedBounds) {
        if (error) *error = "chunk descriptor has no block records";
        return false;
    }
    uint64_t fluid_seen = 0;
    if (!addWithoutOverflow(fluid_source_samples.seen, flowing_fluid_samples.seen,
                            &fluid_seen) ||
        !addWithoutOverflow(structure_samples.seen, fluid_seen,
                            &verification.eligible_count)) {
        if (error) *error = "verification eligible count overflow";
        return false;
    }
    const size_t available_fluid_samples =
        fluid_source_samples.samples.size() + flowing_fluid_samples.samples.size();
    const size_t minimum_fluid = structure_samples.seen == 0
        ? std::min<size_t>(kStableSamplesPerChunk, available_fluid_samples)
        : std::min<size_t>(kMinimumFluidSamples, available_fluid_samples);
    const size_t structure_count = std::min<size_t>(
        structure_samples.samples.size(), kStableSamplesPerChunk - minimum_fluid);
    const size_t fluid_capacity = kStableSamplesPerChunk - structure_count;
    const size_t fluid_source_count = std::min<size_t>(
        fluid_source_samples.samples.size(), fluid_capacity);
    const size_t flowing_fluid_count = std::min<size_t>(
        flowing_fluid_samples.samples.size(), fluid_capacity - fluid_source_count);
    verification.samples.reserve(structure_count + fluid_source_count +
                                 flowing_fluid_count + kAirSamplesPerChunk);
    verification.samples.insert(
        verification.samples.end(),
        std::make_move_iterator(structure_samples.samples.begin()),
        std::make_move_iterator(structure_samples.samples.begin() + structure_count));
    verification.samples.insert(
        verification.samples.end(),
        std::make_move_iterator(fluid_source_samples.samples.begin()),
        std::make_move_iterator(
            fluid_source_samples.samples.begin() + fluid_source_count));
    verification.samples.insert(
        verification.samples.end(),
        std::make_move_iterator(flowing_fluid_samples.samples.begin()),
        std::make_move_iterator(
            flowing_fluid_samples.samples.begin() + flowing_fluid_count));
    if (include_verification_samples &&
        overwrite_policy == OverwritePolicy::ClearImportedBounds &&
        !appendExpectedAirSamples(verification, positions, has_dynamic_blocks,
                                  &verification.samples,
                                  cancel_check, error)) return false;
    *output = std::move(verification);
    return true;
}

bool writeVerificationPlan(const std::string& path,
                           const std::vector<VerificationChunkPlan>& chunks,
                           const CommandSpoolBuilder::CancelCheck& cancel_check,
                           std::string* error) {
    uint64_t sample_count = 0;
    std::set<ChunkCoord> seen_chunks;
    for (size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index) {
        if ((chunk_index & 0xFF) == 0 && cancellationRequested(cancel_check, error)) return false;
        const VerificationChunkPlan& chunk = chunks[chunk_index];
        uint32_t stable_samples = 0;
        uint32_t air_samples = 0;
        std::set<std::array<int32_t, 3>> seen_positions;
        if (!chunk.imported_bounds.isValid() ||
            chunk.eligible_count > chunk.total_block_count ||
            !seen_chunks.insert(chunk.coord).second ||
            chunk.samples.size() > kStableSamplesPerChunk + kAirSamplesPerChunk ||
            !addWithoutOverflow(sample_count, chunk.samples.size(), &sample_count)) {
            if (error) *error = "invalid verification chunk metadata";
            return false;
        }
        for (const VerificationPlanSample& sample : chunk.samples) {
            const bool expected_air = hasVerificationSampleFlag(
                sample, VerificationSampleFlag::ExpectedAir);
            if (!(sample.chunk == chunk.coord) || sample.name.empty() ||
                sample.name.size() > kMaxBlockNameLength ||
                (sample.flags & ~kKnownVerificationFlags) != 0 ||
                !contains(chunk.imported_bounds, sample.x, sample.y, sample.z) ||
                !seen_positions.insert({sample.x, sample.y, sample.z}).second ||
                (expected_air &&
                 (sample.name != "minecraft:air" || sample.aux != 0 ||
                  !hasVerificationSampleFlag(sample, VerificationSampleFlag::IgnoreAux)))) {
                if (error) *error = "invalid verification sample";
                return false;
            }
            if (chunk.total_block_count == 0 && !expected_air) {
                if (error) *error = "empty verification chunks may only contain air samples";
                return false;
            }
            if (expected_air) ++air_samples;
            else ++stable_samples;
        }
        if (stable_samples > kStableSamplesPerChunk || stable_samples > chunk.eligible_count ||
            air_samples > kAirSamplesPerChunk) {
            if (error) *error = "invalid verification sample counts";
            return false;
        }
    }
    const std::string temporary = path + ".tmp";
    TemporaryOutputFile output_file(temporary);
    std::ofstream& stream = output_file.stream();
    const uint64_t chunk_count = chunks.size();
    if (!stream || !writeValue(stream, kVerificationMagic) ||
        !writeValue(stream, kVerificationVersion) || !writeValue(stream, chunk_count) ||
        !writeValue(stream, sample_count)) {
        if (error) *error = "cannot create verification plan";
        return false;
    }
    for (size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index) {
        if ((chunk_index & 0xFF) == 0 && cancellationRequested(cancel_check, error)) {
            return false;
        }
        const VerificationChunkPlan& chunk = chunks[chunk_index];
        const uint32_t chunk_sample_count = static_cast<uint32_t>(chunk.samples.size());
        const uint32_t reserved = 0;
        if (!writeValue(stream, chunk.coord.x) || !writeValue(stream, chunk.coord.z) ||
            !writeValue(stream, chunk.imported_bounds.min_x) ||
            !writeValue(stream, chunk.imported_bounds.min_y) ||
            !writeValue(stream, chunk.imported_bounds.min_z) ||
            !writeValue(stream, chunk.imported_bounds.max_x) ||
            !writeValue(stream, chunk.imported_bounds.max_y) ||
            !writeValue(stream, chunk.imported_bounds.max_z) ||
            !writeValue(stream, chunk.total_block_count) ||
            !writeValue(stream, chunk.eligible_count) ||
            !writeValue(stream, chunk_sample_count) || !writeValue(stream, reserved)) {
            if (error) *error = "cannot write verification chunk metadata";
            return false;
        }
        for (const VerificationPlanSample& sample : chunk.samples) {
            const uint16_t name_length = static_cast<uint16_t>(sample.name.size());
            if (!writeValue(stream, sample.x) || !writeValue(stream, sample.y) ||
                !writeValue(stream, sample.z) || !writeValue(stream, sample.aux) ||
                !writeValue(stream, sample.flags) ||
                !writeValue(stream, name_length)) {
                if (error) *error = "cannot write verification plan";
                return false;
            }
            stream.write(sample.name.data(), name_length);
            if (!stream) {
                if (error) *error = "cannot write verification plan";
                return false;
            }
        }
    }
    if (!output_file.commit(path)) {
        if (error) *error = "cannot finalize verification plan";
        return false;
    }
    return true;
}

}  // namespace

bool CommandSpoolBuilder::build(const std::string& directory, int32_t blocks_per_second,
                                 OverwritePolicy overwrite_policy, int32_t region_span,
                                 std::vector<ChunkDescriptor>* chunks, std::string* error,
                                 const CancelCheck& cancel_check,
                                  bool align_region_grid_to_content,
                                  const ProgressCallback& progress_callback,
                                  bool include_verification_samples,
                                  bool retain_empty_descriptors,
                                  uint32_t maximum_merged_command_blocks) {
    if (!chunks || chunks->empty() || directory.empty() || blocks_per_second < 1 ||
        region_span < 1 ||
        (overwrite_policy != OverwritePolicy::PreserveExisting &&
         overwrite_policy != OverwritePolicy::ClearImportedBounds)) {
        if (error) *error = "invalid command spool options";
        return false;
    }
    if (cancellationRequested(cancel_check, error)) return false;
    std::vector<std::string> raw_palette;
    bool has_raw_palette = false;
    const std::string raw_palette_path = directory + "/" + kRawPaletteFileName;
    if (!loadRawPalette(raw_palette_path, &raw_palette, &has_raw_palette, error)) {
        return false;
    }
    const auto has_raw_placement = [](const ChunkDescriptor& descriptor) {
        for (size_t phase = phaseIndex(ImportPhase::Structure);
             phase < kImportPhaseCount; ++phase) {
            if (descriptor.has_phase[phase]) return true;
        }
        return false;
    };
    if (overwrite_policy == OverwritePolicy::PreserveExisting &&
        !retain_empty_descriptors) {
        chunks->erase(std::remove_if(chunks->begin(), chunks->end(),
            [&](const ChunkDescriptor& descriptor) {
                return !has_raw_placement(descriptor);
            }), chunks->end());
        if (chunks->empty()) {
            if (error) *error = "schematic contains no non-air blocks";
            return false;
        }
    }
    const uint32_t static_merge_limit = maximum_merged_command_blocks == 0
        ? fillBlockLimitForRate(blocks_per_second)
        : std::min(fillBlockLimitForRate(blocks_per_second),
                   maximum_merged_command_blocks);
    const uint32_t dynamic_merge_limit = maximum_merged_command_blocks == 0
        ? dynamicFillBlockLimitForRate(blocks_per_second)
        : std::min(dynamicFillBlockLimitForRate(blocks_per_second),
                   maximum_merged_command_blocks);
    std::set<ChunkCoord> seen_chunks;
    for (const ChunkDescriptor& descriptor : *chunks) {
        if (!descriptor.imported_bounds.isValid() ||
            !seen_chunks.insert(descriptor.coord).second) {
            if (error) *error = "invalid or duplicate chunk descriptor";
            return false;
        }
    }

    std::vector<VerificationChunkPlan> verification_chunks(chunks->size());
    std::vector<std::string> worker_errors(chunks->size());
    std::atomic<size_t> next_chunk{0};
    std::atomic<size_t> completed_chunks{0};
    std::atomic<bool> stop_workers{false};
    std::mutex callback_mutex;
    const CancelCheck guarded_cancel = [&]() {
        if (stop_workers.load(std::memory_order_relaxed)) return true;
        std::lock_guard<std::mutex> lock(callback_mutex);
        return cancel_check && cancel_check();
    };
    reportProgress(progress_callback, CommandSpoolBuildStage::OptimizingChunks,
                   0, chunks->size());
    const auto worker = [&]() {
        while (!stop_workers.load(std::memory_order_relaxed)) {
            const size_t index = next_chunk.fetch_add(1, std::memory_order_relaxed);
            if (index >= chunks->size()) break;
            if (!optimizeChunkDescriptor(
                    &(*chunks)[index], overwrite_policy, static_merge_limit,
                    dynamic_merge_limit, has_raw_palette ? &raw_palette : nullptr,
                    include_verification_samples, retain_empty_descriptors,
                    guarded_cancel,
                    &verification_chunks[index], &worker_errors[index])) {
                stop_workers.store(true, std::memory_order_relaxed);
                break;
            }
            std::lock_guard<std::mutex> lock(callback_mutex);
            const size_t completed =
                completed_chunks.fetch_add(1, std::memory_order_relaxed) + 1;
            reportProgress(progress_callback, CommandSpoolBuildStage::OptimizingChunks,
                           completed, chunks->size());
        }
    };
    // Chunk optimization is CPU-bound and embarrassingly parallel over the
    // shared cursor. Scale with the device's cores, but keep the worker count
    // small: each in-flight chunk can hold several MiB of merge bitmaps.
    const size_t hardware_workers = std::max<size_t>(
        1, static_cast<size_t>(std::thread::hardware_concurrency()) / 2);
    const size_t background_worker_count = chunks->size() > 1
        ? std::min<size_t>({hardware_workers, chunks->size(), 4}) - 1
        : 0;
    std::vector<std::thread> background_workers;
    background_workers.reserve(background_worker_count);
    for (size_t index = 0; index < background_worker_count; ++index) {
        try {
            background_workers.emplace_back(worker);
        } catch (const std::system_error&) {
            // Resource-constrained devices retain the remaining serial share.
            break;
        }
    }
    worker();
    for (std::thread& background_worker : background_workers) {
        if (background_worker.joinable()) background_worker.join();
    }
    if (completed_chunks.load(std::memory_order_relaxed) != chunks->size()) {
        for (const std::string& worker_error : worker_errors) {
            if (!worker_error.empty() && worker_error != "import cancelled") {
                if (error) *error = worker_error;
                return false;
            }
        }
        for (const std::string& worker_error : worker_errors) {
            if (!worker_error.empty()) {
                if (error) *error = worker_error;
                return false;
            }
        }
        if (error) *error = "import cancelled";
        return false;
    }
    if (has_raw_palette && std::remove(raw_palette_path.c_str()) != 0) {
        if (error) *error = "cannot remove raw block palette";
        return false;
    }
    reportProgress(progress_callback, CommandSpoolBuildStage::WritingVerificationPlan,
                   0, verification_chunks.size());
    if (cancellationRequested(cancel_check, error) ||
        !writeVerificationPlan(directory + "/" + kVerificationPlanName,
                               verification_chunks, cancel_check, error)) {
        return false;
    }
    reportProgress(progress_callback, CommandSpoolBuildStage::WritingVerificationPlan,
                   verification_chunks.size(), verification_chunks.size());
    const ChunkCoord region_grid_origin = align_region_grid_to_content && region_span > 1
        ? selectRegionGridOrigin(*chunks, region_span) : ChunkCoord{};
    return region_span == 1 ||
           aggregateRegionCommandSpools(directory, region_span, region_grid_origin,
                                         static_merge_limit,
                                         dynamic_merge_limit, chunks,
                                         cancel_check, progress_callback, error);
}

bool CommandSpoolBuilder::build(const std::string& directory, int32_t blocks_per_second,
                                OverwritePolicy overwrite_policy,
                                std::vector<ChunkDescriptor>* chunks, std::string* error,
                                const CancelCheck& cancel_check) {
    return build(directory, blocks_per_second, overwrite_policy, 1,
                 chunks, error, cancel_check, false, {});
}

bool CommandSpoolBuilder::build(const std::string& directory, int32_t blocks_per_second,
                                std::vector<ChunkDescriptor>* chunks, std::string* error,
                                const CancelCheck& cancel_check) {
    return build(directory, blocks_per_second, OverwritePolicy::PreserveExisting,
                 chunks, error, cancel_check);
}

bool CommandSpoolBuilder::loadVerificationPlan(const std::string& path,
                                               std::vector<VerificationChunkPlan>* chunks,
                                               std::string* error) {
    if (!chunks) {
        if (error) *error = "missing verification plan output";
        return false;
    }
    chunks->clear();
    std::ifstream stream(path, std::ios::binary);
    uint64_t file_size = 0;
    uint32_t magic = 0, version = 0;
    uint64_t chunk_count = 0, sample_count = 0;
    if (!stream || !streamSize(stream, &file_size) || file_size < kVerificationHeaderSize ||
        !readValue(stream, &magic) || !readValue(stream, &version) ||
        !readValue(stream, &chunk_count) || !readValue(stream, &sample_count) ||
        magic != kVerificationMagic ||
        (version != 2 && version != kVerificationVersion) || chunk_count == 0) {
        if (error) *error = "verification plan is missing, corrupt, or unsupported";
        return false;
    }
    const uint64_t sample_fixed_size = verificationSampleFixedSizeForVersion(version);
    const uint32_t maximum_air_samples =
        version == 2 ? kLegacyMaximumAirSamplesPerChunk : kAirSamplesPerChunk;
    const uint32_t maximum_samples_per_chunk =
        kStableSamplesPerChunk + maximum_air_samples;
    const uint64_t payload_size = file_size - kVerificationHeaderSize;
    if (chunk_count > payload_size / kVerificationChunkFixedSize ||
        chunk_count > std::numeric_limits<size_t>::max()) {
        if (error) *error = "verification plan counts exceed file size";
        return false;
    }
    const uint64_t chunk_metadata_size = chunk_count * kVerificationChunkFixedSize;
    const uint64_t remaining_payload_size = payload_size - chunk_metadata_size;
    const uint64_t minimum_sample_size = sample_fixed_size + 1;
    const uint64_t minimum_chunks_for_samples =
        sample_count / maximum_samples_per_chunk +
        (sample_count % maximum_samples_per_chunk == 0 ? 0 : 1);
    if (sample_count > remaining_payload_size / minimum_sample_size ||
        minimum_chunks_for_samples > chunk_count) {
        if (error) *error = "verification plan counts exceed file size";
        return false;
    }

    std::set<ChunkCoord> seen_chunks;
    uint64_t parsed_samples = 0;
    for (uint64_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        VerificationChunkPlan chunk;
        uint32_t chunk_sample_count = 0, reserved = 0;
        if (!readValue(stream, &chunk.coord.x) || !readValue(stream, &chunk.coord.z) ||
            !readValue(stream, &chunk.imported_bounds.min_x) ||
            !readValue(stream, &chunk.imported_bounds.min_y) ||
            !readValue(stream, &chunk.imported_bounds.min_z) ||
            !readValue(stream, &chunk.imported_bounds.max_x) ||
            !readValue(stream, &chunk.imported_bounds.max_y) ||
            !readValue(stream, &chunk.imported_bounds.max_z) ||
            !readValue(stream, &chunk.total_block_count) ||
            !readValue(stream, &chunk.eligible_count) ||
            !readValue(stream, &chunk_sample_count) || !readValue(stream, &reserved)) {
            if (error) *error = "truncated verification chunk metadata";
            chunks->clear();
            return false;
        }
        uint64_t volume = 0;
        if (reserved != 0 || !boundsVolume(chunk.imported_bounds, &volume) ||
            chunk.total_block_count > volume ||
            chunk.eligible_count > chunk.total_block_count ||
            chunk_sample_count > maximum_samples_per_chunk ||
            !seen_chunks.insert(chunk.coord).second ||
            !addWithoutOverflow(parsed_samples, chunk_sample_count, &parsed_samples) ||
            parsed_samples > sample_count) {
            if (error) *error = "corrupt verification chunk metadata";
            chunks->clear();
            return false;
        }
        chunk.samples.reserve(chunk_sample_count);
        std::set<std::array<int32_t, 3>> seen_positions;
        uint32_t stable_samples = 0, air_samples = 0;
        for (uint32_t sample_index = 0; sample_index < chunk_sample_count; ++sample_index) {
            VerificationPlanSample sample;
            sample.chunk = chunk.coord;
            uint16_t name_length = 0;
            if (!readValue(stream, &sample.x) || !readValue(stream, &sample.y) ||
                !readValue(stream, &sample.z)) {
                if (error) *error = "truncated verification sample";
                chunks->clear();
                return false;
            }
            if (version == 2) {
                uint8_t legacy_aux = 0;
                if (!readValue(stream, &legacy_aux)) {
                    if (error) *error = "truncated verification sample";
                    chunks->clear();
                    return false;
                }
                sample.aux = legacy_aux;
            } else if (!readValue(stream, &sample.aux)) {
                if (error) *error = "truncated verification sample";
                chunks->clear();
                return false;
            }
            if (!readValue(stream, &sample.flags) || !readValue(stream, &name_length) ||
                name_length == 0 || name_length > kMaxBlockNameLength ||
                (sample.flags & ~kKnownVerificationFlags) != 0 ||
                !contains(chunk.imported_bounds, sample.x, sample.y, sample.z) ||
                !seen_positions.insert({sample.x, sample.y, sample.z}).second) {
                if (error) *error = "corrupt verification sample";
                chunks->clear();
                return false;
            }
            sample.name.assign(name_length, '\0');
            stream.read(&sample.name[0], name_length);
            if (!stream) {
                if (error) *error = "truncated verification sample";
                chunks->clear();
                return false;
            }
            const bool expected_air = hasVerificationSampleFlag(
                sample, VerificationSampleFlag::ExpectedAir);
            if (expected_air) {
                if (sample.name != "minecraft:air" || sample.aux != 0 ||
                    !hasVerificationSampleFlag(sample, VerificationSampleFlag::IgnoreAux)) {
                    if (error) *error = "invalid expected-air verification sample";
                    chunks->clear();
                    return false;
                }
                ++air_samples;
            } else {
                ++stable_samples;
            }
            chunk.samples.push_back(std::move(sample));
        }
        if ((chunk.total_block_count == 0 && stable_samples != 0) ||
            stable_samples > kStableSamplesPerChunk ||
            stable_samples > chunk.eligible_count ||
            air_samples > maximum_air_samples) {
            if (error) *error = "verification sample counts do not match metadata";
            chunks->clear();
            return false;
        }
        chunks->push_back(std::move(chunk));
    }
    const std::streampos end = stream.tellg();
    if (parsed_samples != sample_count || end < 0 ||
        static_cast<uint64_t>(end) != file_size) {
        if (error) *error = "verification plan count or file size mismatch";
        chunks->clear();
        return false;
    }
    return true;
}

bool CommandSpoolBuilder::loadVerificationPlan(const std::string& path,
                                               std::vector<VerificationPlanSample>* samples,
                                               std::string* error) {
    if (!samples) {
        if (error) *error = "missing verification sample output";
        return false;
    }
    samples->clear();
    std::vector<VerificationChunkPlan> chunks;
    if (!loadVerificationPlan(path, &chunks, error)) return false;
    uint64_t sample_count = 0;
    for (const VerificationChunkPlan& chunk : chunks) {
        if (!addWithoutOverflow(sample_count, chunk.samples.size(), &sample_count) ||
            sample_count > std::numeric_limits<size_t>::max()) {
            if (error) *error = "verification sample count overflow";
            return false;
        }
    }
    samples->reserve(static_cast<size_t>(sample_count));
    for (VerificationChunkPlan& chunk : chunks) {
        for (VerificationPlanSample& sample : chunk.samples) {
            samples->push_back(std::move(sample));
        }
    }
    return true;
}

CommandSpoolReader::CommandSpoolReader(const std::string& path)
    : path_(path), stream_buffer_(kCommandReadBufferSize) {
    stream_.rdbuf()->pubsetbuf(stream_buffer_.data(),
                              static_cast<std::streamsize>(stream_buffer_.size()));
    stream_.open(path, std::ios::binary);
    uint32_t magic = 0, version = 0;
    if (!stream_ || !streamSize(stream_, &file_size_) || file_size_ < kCommandHeaderSize ||
        !readValue(stream_, &magic) || !readValue(stream_, &version) ||
        !readValue(stream_, &command_count_) || !readValue(stream_, &total_block_count_) ||
        magic != kCommandMagic ||
        (version != kLegacyCommandVersion &&
         version != kMergeMetadataCommandVersion && version != kCommandVersion) ||
        (command_count_ == 0 && (total_block_count_ != 0 || file_size_ != kCommandHeaderSize)) ||
        (command_count_ != 0 &&
         (total_block_count_ < command_count_ ||
          command_count_ > (file_size_ - kCommandHeaderSize) /
              (commandRecordFixedSizeForVersion(version) + 1)))) {
        failed_ = true;
        return;
    }
    version_ = version;
    data_offset_ = static_cast<uint64_t>(stream_.tellg());
    if (data_offset_ != kCommandHeaderSize) {
        failed_ = true;
        return;
    }
    offset_ = data_offset_;
    valid_ = true;
}

bool CommandSpoolReader::seek(uint64_t offset) {
    if (!valid_ || failed_ || offset < data_offset_ || offset > file_size_) return false;
    if (offset == offset_) return true;
    if (offset == data_offset_) {
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(data_offset_), std::ios::beg);
        if (!stream_) return false;
        offset_ = data_offset_;
        commands_read_ = 0;
        blocks_read_ = 0;
        return true;
    }

    std::vector<char> probe_buffer(kCommandReadBufferSize);
    std::ifstream probe;
    probe.rdbuf()->pubsetbuf(probe_buffer.data(),
                            static_cast<std::streamsize>(probe_buffer.size()));
    probe.open(path_, std::ios::binary);
    if (!probe) return false;
    probe.seekg(static_cast<std::streamoff>(data_offset_), std::ios::beg);
    if (!probe) return false;
    uint64_t probe_offset = data_offset_;
    uint64_t probe_commands = 0, probe_blocks = 0;
    uint64_t target_commands = 0, target_blocks = 0;
    bool found_target = false;
    const uint64_t record_fixed_size = commandRecordFixedSizeForVersion(version_);
    while (probe_offset < file_size_) {
        if (probe_offset == offset) {
            target_commands = probe_commands;
            target_blocks = probe_blocks;
            found_target = true;
        }
        if (probe_commands >= command_count_ ||
            file_size_ - probe_offset < record_fixed_size + 1) return false;
        CommandRecordHeader header;
        if (!readCommandHeader(probe, version_, &header) ||
            !validCommandHeader(header, version_)) return false;
        const uint64_t record_size = record_fixed_size + header.name_length;
        if (record_size > file_size_ - probe_offset) return false;
        probe.ignore(header.name_length);
        if (!probe || !addWithoutOverflow(probe_blocks, header.block_count, &probe_blocks)) {
            return false;
        }
        probe_offset += record_size;
        ++probe_commands;
    }
    if (probe_offset == offset) {
        target_commands = probe_commands;
        target_blocks = probe_blocks;
        found_target = true;
    }
    if (!found_target || probe_offset != file_size_ ||
        probe_commands != command_count_ || probe_blocks != total_block_count_) {
        return false;
    }
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) return false;
    offset_ = offset;
    commands_read_ = target_commands;
    blocks_read_ = target_blocks;
    return true;
}

std::optional<PlannedCommand> CommandSpoolReader::next() {
    if (!valid_ || failed_ || commands_read_ >= command_count_) return std::nullopt;
    const uint64_t record_fixed_size = commandRecordFixedSizeForVersion(version_);
    if (file_size_ - offset_ < record_fixed_size + 1) {
        failed_ = true;
        return std::nullopt;
    }
    CommandRecordHeader header;
    if (!readCommandHeader(stream_, version_, &header) ||
        !validCommandHeader(header, version_) ||
        header.name_length > file_size_ - offset_ - record_fixed_size) {
        failed_ = true;
        return std::nullopt;
    }
    PlannedCommand command;
    command.bounds = header.bounds;
    command.block_count = header.block_count;
    command.aux = header.aux;
    command.flags = header.reserved;
    command.name.assign(header.name_length, '\0');
    stream_.read(&command.name[0], header.name_length);
    if (!stream_) {
        failed_ = true;
        return std::nullopt;
    }
    uint64_t new_blocks_read = 0;
    if (!addWithoutOverflow(blocks_read_, command.block_count, &new_blocks_read)) {
        failed_ = true;
        return std::nullopt;
    }
    uint64_t new_offset = 0;
    const uint64_t record_size = record_fixed_size + header.name_length;
    if (!addWithoutOverflow(offset_, record_size, &new_offset) ||
        new_offset > file_size_) {
        failed_ = true;
        return std::nullopt;
    }
    ++commands_read_;
    blocks_read_ = new_blocks_read;
    offset_ = new_offset;
    const uint64_t remaining_commands = command_count_ - commands_read_;
    if ((commands_read_ == command_count_ &&
         (offset_ != file_size_ || blocks_read_ != total_block_count_)) ||
        (remaining_commands != 0 &&
          remaining_commands > (file_size_ - offset_) / (record_fixed_size + 1))) {
        failed_ = true;
        return std::nullopt;
    }
    return command;
}

}  // namespace build_import
