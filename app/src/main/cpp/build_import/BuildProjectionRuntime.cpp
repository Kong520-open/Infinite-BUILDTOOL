#include "BuildProjectionRuntime.h"

#include "BdxParser.h"
#include "BlockMapper.h"
#include "BuildProjectionRenderer.h"
#include "LitematicParser.h"
#include "McworldParser.h"
#include "InfiniteczBuildParser.h"
#include "PixelArtParser.h"
#include "ProjectionBlockIdentity.h"
#include "SchematicParser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <map>
#include <new>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <thread>

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace build_import {
namespace {

constexpr int32_t kProjectionChunkSize = 32;
constexpr int32_t kProjectionPartitionSize = 32;
constexpr uint64_t kStatusBlockInterval = 16384;
constexpr size_t kSpoolReadBufferSize = 64 * 1024;
constexpr uint32_t kProjectionRetirementWaitMs = 1000;
constexpr size_t kMaximumBufferedPartitions = 64;
constexpr size_t kPartitionRecordsPerChunk = 64;
constexpr size_t kMaximumPartitionMaterialBytes = 16U * 1024U * 1024U;
constexpr size_t kMaximumPartitionCells =
    static_cast<size_t>(kProjectionPartitionSize + 2) *
        (kProjectionPartitionSize + 2) * (kProjectionPartitionSize + 2);
constexpr uint8_t kProjectionCanFillFlag = 0x01U;
constexpr uint8_t kProjectionSingleLayerFlag = 0x02U;
constexpr uint8_t kProjectionRecordFlag = 0x04U;
constexpr uint8_t kProjectionStatefulFlag = 0x08U;
constexpr uint8_t kProjectionCoreFlag = 0x10U;
constexpr uint32_t kProjectionChunkMagic = 0x50524a31U;
constexpr uint64_t kInvalidProjectionChunkOffset = UINT64_MAX;

struct DiskRecord {
    int32_t x;
    int32_t y;
    int32_t z;
    uint16_t aux;
    uint8_t flags;
    uint8_t phase;
    uint16_t name_index;
    uint16_t reserved;
};

static_assert(sizeof(DiskRecord) == 20, "raw chunk spool record ABI changed");

struct PartitionChunkHeader {
    uint64_t previous_offset;
    int32_t partition_x;
    int32_t partition_y;
    int32_t partition_z;
    uint32_t record_count;
    uint32_t magic;
};

static_assert(sizeof(PartitionChunkHeader) == 32,
              "projection partition chunk header ABI changed");

enum class ProjectionSourceType : uint8_t {
    Schematic,
    Litematic,
    Bdx,
    Mcworld,
    InfiniteczBuild,
    PixelArtPng,
};

int createDirectory(const char* path) {
#if defined(_WIN32)
    return ::_mkdir(path);
#else
    return ::mkdir(path, 0700);
#endif
}

int removeDirectory(const char* path) {
#if defined(_WIN32)
    return ::_rmdir(path);
#else
    return ::rmdir(path);
#endif
}

bool ensureDirectory(const std::string& directory) {
    if (directory.empty()) return false;
    std::string partial;
    for (size_t index = 0; index <= directory.size(); ++index) {
        if (index != directory.size() && directory[index] != '/' &&
            directory[index] != '\\') {
            partial.push_back(directory[index]);
            continue;
        }
        if (!partial.empty() && partial != "." && partial != "/" &&
            createDirectory(partial.c_str()) != 0 && errno != EEXIST) {
            return false;
        }
        if (index != directory.size()) {
            if (partial.empty() && directory[index] == '/') partial = "/";
            else if (!partial.empty() && partial.back() != '/') partial.push_back('/');
        }
    }
    return true;
}

std::string joinPath(const std::string& directory, const std::string& name) {
    if (directory.empty() || directory.back() == '/' || directory.back() == '\\') {
        return directory + name;
    }
    return directory + "/" + name;
}

bool createUniqueSpoolDirectory(const std::string& work_directory, uint64_t generation,
                                std::string* directory, std::string* error) {
    if (!ensureDirectory(work_directory)) {
        if (error) *error = "cannot create projection work directory";
        return false;
    }
    const uint64_t timestamp = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (uint32_t attempt = 0; attempt < 32; ++attempt) {
        const std::string candidate = joinPath(
            work_directory, "projection_spool_" + std::to_string(timestamp) + "_" +
                std::to_string(generation) + "_" + std::to_string(attempt));
        if (createDirectory(candidate.c_str()) == 0) {
            *directory = candidate;
            return true;
        }
        if (errno != EEXIST) break;
    }
    if (error) *error = "cannot create a private projection spool directory";
    return false;
}

void cleanupOwnedSpoolDirectory(const std::string& directory) noexcept {
    if (directory.empty()) return;
    DIR* raw_directory = nullptr;
    try {
        raw_directory = opendir(directory.c_str());
        if (raw_directory) {
            while (dirent* entry = readdir(raw_directory)) {
                const std::string name = entry->d_name;
                if (name == "." || name == "..") continue;
                std::remove(joinPath(directory, name).c_str());
            }
            closedir(raw_directory);
            raw_directory = nullptr;
        }
    } catch (...) {
        if (raw_directory) closedir(raw_directory);
    }
    removeDirectory(directory.c_str());
}

// A forced process stop does not run the ProjectionPlan destructor, so its
// private spool directory remains under files/build_projection. These are
// generated caches, never user-selected source files; remove only this exact
// name family after the prior render plan has been acknowledged as retired.
void cleanupAbandonedProjectionSpoolDirectories(const std::string& work_directory) noexcept {
    if (work_directory.empty()) return;
    DIR* raw_directory = opendir(work_directory.c_str());
    if (!raw_directory) return;
    try {
        while (dirent* entry = readdir(raw_directory)) {
            const std::string name = entry->d_name;
            if (name.rfind("projection_spool_", 0) != 0) continue;
            const std::string candidate = joinPath(work_directory, name);
            struct stat metadata {};
            if (stat(candidate.c_str(), &metadata) != 0 || !S_ISDIR(metadata.st_mode)) {
                continue;
            }
            cleanupOwnedSpoolDirectory(candidate);
        }
    } catch (...) {
        // Best effort only. A subsequent spool creation still reports a
        // normal I/O failure if the app-private directory is unavailable.
    }
    closedir(raw_directory);
}

class ScopedSpoolDirectory {
public:
    explicit ScopedSpoolDirectory(std::string directory)
        : directory_(std::move(directory)) {}

    ~ScopedSpoolDirectory() noexcept { cleanup(); }

    void cleanup() noexcept {
        if (directory_.empty()) return;
        cleanupOwnedSpoolDirectory(directory_);
        directory_.clear();
    }

    // A committed streaming plan owns the directory because its optimized
    // group spools remain disk-backed for the lifetime of the projection.
    void release() noexcept { directory_.clear(); }

private:
    std::string directory_;
};

struct PartitionCoord {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const PartitionCoord& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct PartitionCoordHash {
    size_t operator()(const PartitionCoord& coord) const {
        uint64_t hash = 1469598103934665603ULL;
        const auto add = [&](uint32_t value) {
            hash ^= value;
            hash *= 1099511628211ULL;
        };
        add(static_cast<uint32_t>(coord.x));
        add(static_cast<uint32_t>(coord.y));
        add(static_cast<uint32_t>(coord.z));
        return static_cast<size_t>(hash ^ (hash >> 32U));
    }
};

struct BlockPosition {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const BlockPosition& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct BlockPositionHash {
    size_t operator()(const BlockPosition& position) const {
        return PartitionCoordHash{}({position.x, position.y, position.z});
    }
};

class PartitionSpoolWriter {
public:
    explicit PartitionSpoolWriter(std::string directory)
        : container_path_(joinPath(std::move(directory), "projection_partitions.bin")) {}

    ~PartitionSpoolWriter() {
        if (container_writer_.is_open()) container_writer_.close();
    }

    struct PartitionInfo {
        uint64_t tail_offset = kInvalidProjectionChunkOffset;
        uint64_t total_records = 0;
        uint64_t core_records = 0;
    };

    bool internName(const std::string& name, uint16_t* index, std::string* error) {
        if (finalized_) {
            if (error) *error = "projection partition storage is already finalized";
            return false;
        }
        if (!index || name.empty()) {
            if (error) *error = "projection material name is empty";
            return false;
        }
        const auto existing = name_indices_.find(name);
        if (existing != name_indices_.end()) {
            *index = existing->second;
            return true;
        }
        if (material_names_.size() > UINT16_MAX ||
            material_name_bytes_ > kMaximumPartitionMaterialBytes ||
            name.size() > kMaximumPartitionMaterialBytes - material_name_bytes_) {
            if (error) *error = "projection material dictionary exceeds its memory budget";
            return false;
        }
        const uint16_t new_index = static_cast<uint16_t>(material_names_.size());
        const auto inserted = name_indices_.emplace(name, new_index);
        material_names_.emplace_back(inserted.first->first);
        material_name_bytes_ += name.size();
        *index = new_index;
        return true;
    }

    const std::vector<std::string_view>& materialNames() const { return material_names_; }
    const std::string& containerPath() const { return container_path_; }
    const std::unordered_map<PartitionCoord, PartitionInfo, PartitionCoordHash>&
    partitions() const { return partitions_; }

    bool append(const PartitionCoord& coord, DiskRecord record,
                bool core, std::string* error) {
        if (finalized_) {
            if (error) *error = "projection partition storage is already finalized";
            return false;
        }
        OpenBuffer* buffer = openBuffer(coord, error);
        if (!buffer) return false;
        record.flags = core
            ? static_cast<uint8_t>(record.flags | kProjectionCoreFlag)
            : static_cast<uint8_t>(record.flags & ~kProjectionCoreFlag);
        PartitionInfo& info = partitions_[coord];
        if (info.total_records == UINT64_MAX || (core && info.core_records == UINT64_MAX)) {
            if (error) *error = "projection partition record count exceeds the supported range";
            return false;
        }
        ++info.total_records;
        if (core) ++info.core_records;
        buffer->records[buffer->record_count++] = record;
        buffer->last_access = ++access_serial_;
        if (buffer->record_count == buffer->records.size() &&
            !flushBuffer(coord, buffer, error)) return false;
        return true;
    }

    bool finish(std::string* error) {
        if (finalized_) {
            if (!finalize_succeeded_ && error && error->empty()) {
                *error = "projection spatial partitions were not finalized";
            }
            return finalize_succeeded_;
        }
        finalized_ = true;
        bool success = true;
        for (auto& entry : open_buffers_) {
            if (!flushBuffer(entry.first, &entry.second, error)) success = false;
        }
        open_buffers_.clear();
        if (container_writer_.is_open()) {
            container_writer_.flush();
            container_writer_.close();
            if (!container_writer_) success = false;
        }
        if (!success && error && error->empty()) {
            *error = "cannot finalize projection spatial partitions";
        }
        finalize_succeeded_ = success;
        return success;
    }

private:
    struct OpenBuffer {
        std::array<DiskRecord, kPartitionRecordsPerChunk> records{};
        size_t record_count = 0;
        uint64_t last_access = 0;
    };

    bool ensureWriter(std::string* error) {
        if (container_writer_.is_open()) return true;
        container_writer_.open(container_path_, std::ios::binary | std::ios::trunc);
        if (container_writer_) return true;
        if (error) *error = "cannot create projection partition storage";
        return false;
    }

    bool flushBuffer(const PartitionCoord& coord, OpenBuffer* buffer,
                     std::string* error) {
        if (!buffer || buffer->record_count == 0) return true;
        if (!ensureWriter(error)) return false;
        const auto info = partitions_.find(coord);
        if (info == partitions_.end()) {
            if (error) *error = "projection partition index is inconsistent";
            return false;
        }
        const std::streamoff position = static_cast<std::streamoff>(
            container_writer_.tellp());
        if (position < 0) {
            if (error) *error = "cannot determine projection partition storage position";
            return false;
        }
        const PartitionChunkHeader header{
            info->second.tail_offset, coord.x, coord.y, coord.z,
            static_cast<uint32_t>(buffer->record_count), kProjectionChunkMagic,
        };
        container_writer_.write(reinterpret_cast<const char*>(&header), sizeof(header));
        container_writer_.write(
            reinterpret_cast<const char*>(buffer->records.data()),
            static_cast<std::streamsize>(buffer->record_count * sizeof(DiskRecord)));
        if (!container_writer_) {
            if (error) *error = "cannot write projection partition storage";
            return false;
        }
        info->second.tail_offset = static_cast<uint64_t>(position);
        buffer->record_count = 0;
        return true;
    }

    OpenBuffer* openBuffer(const PartitionCoord& coord, std::string* error) {
        auto existing = open_buffers_.find(coord);
        if (existing != open_buffers_.end()) return &existing->second;
        if (open_buffers_.size() >= kMaximumBufferedPartitions) {
            auto oldest = open_buffers_.begin();
            for (auto it = std::next(open_buffers_.begin()); it != open_buffers_.end(); ++it) {
                if (it->second.last_access < oldest->second.last_access) oldest = it;
            }
            if (!flushBuffer(oldest->first, &oldest->second, error)) return nullptr;
            open_buffers_.erase(oldest);
        }
        OpenBuffer opened;
        opened.last_access = ++access_serial_;
        return &open_buffers_.emplace(coord, std::move(opened)).first->second;
    }

    std::string container_path_;
    std::ofstream container_writer_;
    std::unordered_map<PartitionCoord, OpenBuffer, PartitionCoordHash> open_buffers_;
    std::unordered_map<PartitionCoord, PartitionInfo, PartitionCoordHash> partitions_;
    std::unordered_map<std::string, uint16_t> name_indices_;
    std::vector<std::string_view> material_names_;
    size_t material_name_bytes_ = 0;
    uint64_t access_serial_ = 0;
    bool finalized_ = false;
    bool finalize_succeeded_ = false;
};

class ScopedStreamingPlan {
public:
    explicit ScopedStreamingPlan(BuildProjectionRenderer* renderer) : renderer_(renderer) {}
    ~ScopedStreamingPlan() {
        if (renderer_) renderer_->abortStreamingPlan();
    }
    void release() noexcept { renderer_ = nullptr; }

private:
    BuildProjectionRenderer* renderer_;
};

bool hasCaseInsensitiveSuffix(const std::string& value, const char* suffix) {
    const size_t suffix_length = std::strlen(suffix);
    if (value.size() < suffix_length) return false;
    const size_t offset = value.size() - suffix_length;
    for (size_t index = 0; index < suffix_length; ++index) {
        const unsigned char left = static_cast<unsigned char>(value[offset + index]);
        const unsigned char right = static_cast<unsigned char>(suffix[index]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

bool sourceTypeForPath(const std::string& path, ProjectionSourceType* type) {
    if (hasCaseInsensitiveSuffix(path, ".schem") ||
        hasCaseInsensitiveSuffix(path, ".schematic")) {
        *type = ProjectionSourceType::Schematic;
        return true;
    }
    if (hasCaseInsensitiveSuffix(path, ".litematic")) {
        *type = ProjectionSourceType::Litematic;
        return true;
    }
    if (hasCaseInsensitiveSuffix(path, ".bdx")) {
        *type = ProjectionSourceType::Bdx;
        return true;
    }
    if (hasCaseInsensitiveSuffix(path, ".mcworld")) {
        *type = ProjectionSourceType::Mcworld;
        return true;
    }
    if (hasCaseInsensitiveSuffix(path, ".infinity") ||
        hasCaseInsensitiveSuffix(path, ".IBuild")) {
        *type = ProjectionSourceType::InfiniteczBuild;
        return true;
    }
    if (hasCaseInsensitiveSuffix(path, ".png")) {
        *type = ProjectionSourceType::PixelArtPng;
        return true;
    }
    return false;
}

const char* sourceLabel(ProjectionSourceType type) {
    switch (type) {
        case ProjectionSourceType::Schematic: return "schematic";
        case ProjectionSourceType::Litematic: return "litematic";
        case ProjectionSourceType::Bdx: return "BDX";
        case ProjectionSourceType::Mcworld: return "mcworld";
        case ProjectionSourceType::InfiniteczBuild: return ".infinity";
        case ProjectionSourceType::PixelArtPng: return "pixel art";
    }
    return "blueprint";
}

std::string canonicalProjectionBlockName(std::string_view source_state) {
    const size_t properties = source_state.find('[');
    std::string_view identifier = source_state.substr(0, properties);
    while (!identifier.empty() && std::isspace(static_cast<unsigned char>(identifier.front()))) {
        identifier.remove_prefix(1);
    }
    while (!identifier.empty() && std::isspace(static_cast<unsigned char>(identifier.back()))) {
        identifier.remove_suffix(1);
    }
    if (identifier.empty()) return {};

    std::string result;
    result.reserve(identifier.size() + 10U);
    if (identifier.find(':') == std::string_view::npos) result = "minecraft:";
    for (unsigned char character : identifier) {
        const char lower = static_cast<char>(std::tolower(character));
        const bool valid = (lower >= 'a' && lower <= 'z') ||
            (lower >= '0' && lower <= '9') || lower == '_' || lower == ':' ||
            lower == '-' || lower == '/' || lower == '.';
        if (!valid) return {};
        result.push_back(lower);
    }
    return result;
}

std::string_view projectionLeaf(std::string_view name) {
    const size_t separator = name.find(':');
    return separator == std::string_view::npos ? name : name.substr(separator + 1U);
}

std::string_view trimProjectionStateToken(std::string_view token) {
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
        token.remove_prefix(1U);
    }
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
        token.remove_suffix(1U);
    }
    return token;
}

// BDX and LevelDB palettes often wrap both sides of a property in quotes
// (for example ["wood_type"="spruce"]).  Strip only one balanced outer
// pair: property values are compared as tokens below, so interpreting escape
// sequences here would make a malformed palette silently mean something else.
std::string_view stripBalancedProjectionStateQuotes(std::string_view token) {
    token = trimProjectionStateToken(token);
    if (token.size() < 2U || token.front() != '"' || token.back() != '"') return token;

    bool escaped = false;
    for (size_t index = 1U; index < token.size(); ++index) {
        const char character = token[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '"') {
            // An unescaped quote before the final character means this was
            // not one balanced outer pair after all.
            if (index + 1U != token.size()) return token;
            return token.substr(1U, token.size() - 2U);
        }
    }
    return token;
}

bool projectionStateKeyMatches(std::string_view candidate, std::string_view key) {
    if (candidate == key) return true;
    constexpr std::string_view kMinecraftPrefix = "minecraft:";
    return candidate.size() > kMinecraftPrefix.size() &&
        candidate.substr(0U, kMinecraftPrefix.size()) == kMinecraftPrefix &&
        candidate.substr(kMinecraftPrefix.size()) == key;
}

std::string_view stateProperty(std::string_view state, std::string_view key) {
    const size_t open = state.find('[');
    if (open == std::string_view::npos) return {};

    size_t close = std::string_view::npos;
    bool quoted = false;
    bool escaped = false;
    for (size_t index = open + 1U; index < state.size(); ++index) {
        const char character = state[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\' && quoted) {
            escaped = true;
            continue;
        }
        if (character == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && character == ']') {
            close = index;
            break;
        }
    }
    if (close == std::string_view::npos || quoted) return {};

    size_t start = open + 1U;
    while (start < close) {
        size_t end = start;
        size_t equals = std::string_view::npos;
        quoted = false;
        escaped = false;
        for (; end < close; ++end) {
            const char character = state[end];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (character == '\\' && quoted) {
                escaped = true;
                continue;
            }
            if (character == '"') {
                quoted = !quoted;
                continue;
            }
            if (!quoted && character == '=' && equals == std::string_view::npos) {
                equals = end;
                continue;
            }
            if (!quoted && character == ',') break;
        }
        if (!quoted && equals != std::string_view::npos && equals < end) {
            const std::string_view candidate = stripBalancedProjectionStateQuotes(
                state.substr(start, equals - start));
            if (projectionStateKeyMatches(candidate, key)) {
                return stripBalancedProjectionStateQuotes(
                    state.substr(equals + 1U, end - equals - 1U));
            }
        }
        start = end + 1U;
    }
    return {};
}

std::string_view firstStateProperty(std::string_view state, std::string_view primary,
                                    std::string_view fallback) {
    const std::string_view value = stateProperty(state, primary);
    return value.empty() ? stateProperty(state, fallback) : value;
}

bool projectionNameEndsWith(std::string_view name, std::string_view suffix) {
    return name.size() >= suffix.size() &&
        name.substr(name.size() - suffix.size()) == suffix;
}

int projectionWoodTypeAux(std::string_view wood_type) {
    static constexpr std::array<std::string_view, 6> kWoodTypes{{
        "oak", "spruce", "birch", "jungle", "acacia", "dark_oak",
    }};
    for (size_t index = 0; index < kWoodTypes.size(); ++index) {
        if (wood_type == kWoodTypes[index]) return static_cast<int>(index);
    }
    return -1;
}

int projectionLegacyLogTypeAux(std::string_view leaf, std::string_view type) {
    if (leaf == "log") {
        if (type == "oak" || type == "oka") return 0;
        if (type == "spruce") return 1;
        if (type == "birch") return 2;
        if (type == "jungle") return 3;
    } else if (leaf == "log2") {
        if (type == "acacia") return 0;
        if (type == "dark_oak") return 1;
    }
    return -1;
}

int projectionModernAxisAux(std::string_view axis) {
    if (axis == "y") return 0;
    if (axis == "x") return 1;
    if (axis == "z") return 2;
    return -1;
}

int projectionLegacyPillarAxisAux(std::string_view axis) {
    if (axis == "y") return 0;
    if (axis == "x") return 4;
    if (axis == "z") return 8;
    return -1;
}

int projectionQuartzPillarAxisAux(std::string_view axis) {
    if (axis == "y") return 2;
    if (axis == "x") return 6;
    if (axis == "z") return 10;
    return -1;
}

int projectionSixWayFacingAux(std::string_view facing) {
    if (facing == "down" || facing == "0") return 0;
    if (facing == "up" || facing == "1") return 1;
    if (facing == "north" || facing == "2") return 2;
    if (facing == "south" || facing == "3") return 3;
    if (facing == "west" || facing == "4") return 4;
    if (facing == "east" || facing == "5") return 5;
    return -1;
}

int projectionHorizontalFacingAux(std::string_view facing) {
    if (facing == "south" || facing == "3") return 0;
    if (facing == "west" || facing == "4") return 1;
    if (facing == "north" || facing == "2") return 2;
    if (facing == "east" || facing == "5") return 3;
    return -1;
}

int projectionWallAttachmentAux(std::string_view facing) {
    // Torch/button/lever wall attachment uses the old mounting layout, not
    // the normal six-way ItemUse layout: east=1, west=2, south=3, north=4.
    if (facing == "east" || facing == "5") return 1;
    if (facing == "west" || facing == "4") return 2;
    if (facing == "south" || facing == "3") return 3;
    if (facing == "north" || facing == "2") return 4;
    return -1;
}

int projectionEndRodFacingAux(std::string_view facing) {
    if (facing == "down" || facing == "0") return 0;
    if (facing == "up" || facing == "1") return 1;
    if (facing == "south" || facing == "3") return 2;
    if (facing == "north" || facing == "2") return 3;
    if (facing == "east" || facing == "5") return 4;
    if (facing == "west" || facing == "4") return 5;
    return -1;
}

bool projectionModernAxisName(std::string_view leaf) {
    return projectionNameEndsWith(leaf, "_log") ||
        projectionNameEndsWith(leaf, "_wood") ||
        projectionNameEndsWith(leaf, "_stem") ||
        projectionNameEndsWith(leaf, "_hyphae") ||
        leaf == "bamboo_block" || leaf == "stripped_bamboo_block" ||
        leaf == "basalt" || leaf == "polished_basalt" || leaf == "deepslate" ||
        leaf == "purpur_pillar";
}

bool projectionHasUnsupportedWoodType(std::string_view name, std::string_view state) {
    const std::string_view leaf = projectionLeaf(name);
    if (leaf == "sapling") {
        const std::string_view wood_type = firstStateProperty(state, "sapling_type", "wood_type");
        return !wood_type.empty() && projectionWoodTypeAux(wood_type) < 0;
    }
    if (leaf == "fence") {
        // Older Bedrock palettes named the property old_log_type while newer
        // ones use wood_type.  Both describe the same six material variants.
        const std::string_view wood_type = firstStateProperty(state, "wood_type", "old_log_type");
        return !wood_type.empty() && projectionWoodTypeAux(wood_type) < 0;
    }
    if (leaf != "planks" && leaf != "wooden_slab" && leaf != "double_wooden_slab") {
        return false;
    }
    const std::string_view wood_type = stateProperty(state, "wood_type");
    return !wood_type.empty() && projectionWoodTypeAux(wood_type) < 0;
}

bool projectionHasUnsupportedLegacyLogType(std::string_view name, std::string_view state) {
    const std::string_view leaf = projectionLeaf(name);
    if (leaf != "log" && leaf != "log2") return false;
    const std::string_view type = leaf == "log"
        ? firstStateProperty(state, "old_log_type", "wood_type")
        : firstStateProperty(state, "new_log_type", "wood_type");
    return !type.empty() && projectionLegacyLogTypeAux(leaf, type) < 0;
}

int dyeIndex(std::string_view color) {
    static constexpr std::array<std::string_view, 16> kDyeOrder{{
        "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
        "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
    }};
    for (size_t index = 0; index < kDyeOrder.size(); ++index) {
        if (color == kDyeOrder[index]) return static_cast<int>(index);
    }
    return -1;
}

int projectionStateSmallUnsigned(std::string_view value, uint8_t maximum) {
    if (value.empty()) return -1;
    uint32_t parsed = 0U;
    for (const unsigned char character : value) {
        if (character < '0' || character > '9') return -1;
        parsed = parsed * 10U + static_cast<uint32_t>(character - '0');
        if (parsed > maximum) return -1;
    }
    return static_cast<int>(parsed);
}

uint8_t projectionAuxForState(std::string_view name, std::string_view state) {
    const std::string_view leaf = projectionLeaf(name);
    const auto contains = [&](std::string_view token) {
        return leaf.find(token) != std::string_view::npos;
    };
    const std::string_view facing = firstStateProperty(state, "facing", "facing_direction");
    const std::string_view half = firstStateProperty(state, "half", "vertical_half");
    const std::string_view type = firstStateProperty(state, "type", "slab_type");
    const std::string_view axis = firstStateProperty(state, "axis", "pillar_axis");
    uint8_t aux = 0;

    if (contains("stairs")) {
        // Bedrock's historical weirdo_direction order differs from its
        // generic facing_direction order: east=0, west=1, south=2, north=3.
        const std::string_view stair_facing = firstStateProperty(
            state, "facing", "weirdo_direction");
        if (stair_facing == "east" || stair_facing == "0") aux = 0;
        else if (stair_facing == "west" || stair_facing == "1") aux = 1;
        else if (stair_facing == "south" || stair_facing == "2") aux = 2;
        else if (stair_facing == "north" || stair_facing == "3") aux = 3;
        if (half == "top" || stateProperty(state, "upside_down_bit") == "true") {
            aux |= 0x04U;
        }
    } else if (contains("trapdoor")) {
        if (facing == "south") aux = 0;
        else if (facing == "north") aux = 1;
        else if (facing == "east") aux = 2;
        else if (facing == "west") aux = 3;
        const std::string_view open = firstStateProperty(state, "open", "open_bit");
        // Target trapdoor metadata is facing | top(0x04) | open(0x08).
        // Keep the source state accurate even though printer placement remains
        // intentionally paused until its click-height/multi-state policy is
        // implemented as one verified unit.
        if (half == "top") aux |= 0x04U;
        if (open == "true") aux |= 0x08U;
    } else if (contains("slab")) {
        // Legacy wooden slabs encode species in the low three bits while the
        // upper-half bit remains meaningful to the renderer/printer.
        if (leaf == "wooden_slab" || leaf == "double_wooden_slab") {
            const int wood = projectionWoodTypeAux(stateProperty(state, "wood_type"));
            if (wood >= 0) aux = static_cast<uint8_t>(wood);
        }
        const std::string_view top_slot_bit = stateProperty(state, "top_slot_bit");
        constexpr std::string_view kReversedDoubleSuffix = "_slab_double";
        const bool named_double = contains("double_slab") ||
            (leaf.size() >= kReversedDoubleSuffix.size() &&
             leaf.compare(leaf.size() - kReversedDoubleSuffix.size(),
                          kReversedDoubleSuffix.size(), kReversedDoubleSuffix) == 0);
        const bool is_double = type == "double" || named_double;
        const bool is_top = type == "top" || type == "upper" || half == "top" ||
            half == "upper" || top_slot_bit == "true" || top_slot_bit == "1";
        if (is_top && !is_double) aux |= 0x08U;
        if (is_double) aux |= 0x80U;
    } else if (leaf == "planks") {
        // A generic Bedrock planks identifier carries its species only in
        // wood_type.  Do not coerce an unrecognised value to a non-default
        // species: leaving aux untouched is safer than inventing a material.
        const int wood = projectionWoodTypeAux(stateProperty(state, "wood_type"));
        if (wood >= 0) aux = static_cast<uint8_t>(wood);
    } else if (leaf == "sapling") {
        const int wood = projectionWoodTypeAux(
            firstStateProperty(state, "sapling_type", "wood_type"));
        if (wood >= 0) aux = static_cast<uint8_t>(wood);
    } else if (leaf == "fence") {
        const int wood = projectionWoodTypeAux(
            firstStateProperty(state, "wood_type", "old_log_type"));
        if (wood >= 0) aux = static_cast<uint8_t>(wood);
    } else if (leaf == "log" || leaf == "log2") {
        const std::string_view log_type = leaf == "log"
            ? firstStateProperty(state, "old_log_type", "wood_type")
            : firstStateProperty(state, "new_log_type", "wood_type");
        const int material = projectionLegacyLogTypeAux(leaf, log_type);
        if (material >= 0) aux = static_cast<uint8_t>(material);
        const int pillar_axis = projectionLegacyPillarAxisAux(axis);
        if (pillar_axis >= 0) aux = static_cast<uint8_t>(aux | pillar_axis);
    } else if (leaf == "leaves" || leaf == "leaves2") {
        const std::string_view leaf_type = leaf == "leaves"
            ? firstStateProperty(state, "old_leaf_type", "wood_type")
            : firstStateProperty(state, "new_leaf_type", "wood_type");
        const int material = projectionLegacyLogTypeAux(
            leaf == "leaves" ? "log" : "log2", leaf_type);
        if (material >= 0) aux = static_cast<uint8_t>(material);
        if (firstStateProperty(state, "persistent", "persistent_bit") == "true") {
            aux |= 0x04U;
        }
    } else if (leaf == "quartz_pillar" || leaf == "quartz_block") {
        const std::string_view chisel = firstStateProperty(state, "chisel_type", "variant");
        if (leaf == "quartz_block" && (chisel == "chiseled" ||
            chisel == "chiseled_quartz_block")) {
            aux = 1U;
        } else if (leaf == "quartz_block" && (chisel == "smooth" ||
                   chisel == "smooth_quartz")) {
            aux = 3U;
        } else if (leaf == "quartz_pillar" || chisel == "lines" ||
                   chisel == "quartz_pillar") {
            // A missing pillar axis is the state default (vertical/y), not an
            // unknown direction. The historic quartz representation uses 2
            // for that default rather than zero.
            const int pillar_axis = axis.empty() ? 2 : projectionQuartzPillarAxisAux(axis);
            if (pillar_axis >= 0) aux = static_cast<uint8_t>(pillar_axis);
        }
    } else if (leaf == "bone_block" || leaf == "hay_block") {
        // These retained the old pillar axis bit layout even in current
        // palettes: y=0, x=4, z=8.
        const int pillar_axis = projectionLegacyPillarAxisAux(axis);
        if (pillar_axis >= 0) aux = static_cast<uint8_t>(pillar_axis);
    } else if (leaf == "chain") {
        const int chain_axis = projectionModernAxisAux(axis);
        if (chain_axis >= 0) aux = static_cast<uint8_t>(chain_axis);
    } else if (projectionModernAxisName(leaf)) {
        // Flattened logs, all-bark wood, stems/hyphae and modern pillars use
        // y=0, x=1, z=2 in the target's direct block-state aux convention.
        const int modern_axis = projectionModernAxisAux(axis);
        if (modern_axis >= 0) aux = static_cast<uint8_t>(modern_axis);
    } else if (leaf == "torch" || leaf == "soul_torch" ||
               leaf == "redstone_torch" || leaf == "unlit_redstone_torch" ||
               leaf == "wall_torch" || leaf == "soul_wall_torch" ||
               leaf == "redstone_wall_torch") {
        const bool wall = leaf == "wall_torch" || leaf == "soul_wall_torch" ||
            leaf == "redstone_wall_torch";
        if (wall) {
            const int direction = projectionWallAttachmentAux(facing);
            if (direction >= 0) aux = static_cast<uint8_t>(direction);
        } else {
            // Historical metadata uses 5 for a torch standing on the top face
            // of a support block.  Do not encode it as generic six-way up.
            aux = 5U;
        }
    } else if (projectionNameEndsWith(leaf, "_button") || leaf == "stone_button" ||
               leaf == "wooden_button") {
        const std::string_view mount_face = stateProperty(state, "face");
        int direction = -1;
        if (mount_face == "wall") direction = projectionWallAttachmentAux(facing);
        else if (mount_face == "floor" && projectionHorizontalFacingAux(facing) >= 0) {
            direction = 5;
        } else if (mount_face == "ceiling" && projectionHorizontalFacingAux(facing) >= 0) {
            direction = 0;
        }
        if (direction >= 0) aux = static_cast<uint8_t>(direction);
        if (firstStateProperty(state, "powered", "powered_bit") == "true") aux |= 0x08U;
    } else if (leaf == "lever") {
        const std::string_view mount_face = stateProperty(state, "face");
        int direction = -1;
        if (mount_face == "wall") direction = projectionWallAttachmentAux(facing);
        else if (mount_face == "floor") {
            if (facing == "east" || facing == "west") direction = 6;
            else if (facing == "north" || facing == "south") direction = 5;
        } else if (mount_face == "ceiling") {
            if (facing == "east" || facing == "west") direction = 0;
            else if (facing == "north" || facing == "south") direction = 7;
        }
        if (direction >= 0) aux = static_cast<uint8_t>(direction);
        if (firstStateProperty(state, "powered", "powered_bit") == "true") aux |= 0x08U;
    } else if (leaf == "anvil" || leaf == "chipped_anvil" || leaf == "damaged_anvil") {
        const int direction = projectionHorizontalFacingAux(facing);
        if (direction >= 0) aux = static_cast<uint8_t>(direction);
        // Some legacy direct palettes retain a generic anvil name plus a
        // damage property. Modern palettes use chipped/damaged_anvil names;
        // both forms are preserved for the source-to-live identity layer.
        const std::string_view damage = firstStateProperty(state, "damage", "damage_state");
        if (leaf == "anvil") {
            if (damage == "chipped" || damage == "slightly_damaged") aux |= 0x04U;
            else if (damage == "damaged" || damage == "very_damaged") aux |= 0x08U;
        }
    } else if (leaf == "barrel" || leaf == "hopper" || leaf == "dispenser" ||
               leaf == "dropper" || leaf == "observer" || leaf == "shulker_box" ||
               projectionNameEndsWith(leaf, "_shulker_box")) {
        // These all expose Bedrock's ordinary six-way layout.  Triggered,
        // powered, open and enabled are simulation/runtime values, so a fresh
        // projection stores only the physical face which can be recreated by
        // an ItemUse click.
        const int direction = projectionSixWayFacingAux(facing);
        if (direction >= 0 && !(leaf == "hopper" && direction == 1)) {
            aux = static_cast<uint8_t>(direction);
        }
    } else if (leaf == "chest" || leaf == "trapped_chest" || leaf == "ender_chest" ||
               leaf == "furnace" || leaf == "lit_furnace" ||
               leaf == "blast_furnace" || leaf == "lit_blast_furnace" ||
               leaf == "smoker" || leaf == "lit_smoker") {
        const int direction = projectionSixWayFacingAux(facing);
        if (direction >= 2) aux = static_cast<uint8_t>(direction);
    } else if (leaf == "repeater") {
        const int direction = projectionHorizontalFacingAux(facing);
        const int delay = projectionStateSmallUnsigned(stateProperty(state, "delay"), 4U);
        if (direction >= 0 && delay >= 1) {
            aux = static_cast<uint8_t>(direction | ((delay - 1) << 2));
        }
    } else if (leaf == "comparator") {
        const int direction = projectionHorizontalFacingAux(facing);
        if (direction >= 0) {
            aux = static_cast<uint8_t>(direction |
                (stateProperty(state, "mode") == "subtract" ? 0x04U : 0U));
        }
    } else if (leaf == "daylight_detector" || leaf == "daylight_detector_inverted") {
        // Light level/power changes with the world. The shell's normal/inverted
        // mode is represented by the target name and is restored later through
        // one ordinary block-use when required.
        aux = 0U;
    } else if (leaf == "end_rod") {
        const int direction = projectionEndRodFacingAux(facing);
        if (direction >= 0) aux = static_cast<uint8_t>(direction);
    } else if (leaf == "lightning_rod" || projectionNameEndsWith(leaf, "_lightning_rod")) {
        const int direction = projectionSixWayFacingAux(facing);
        if (direction >= 0) aux = static_cast<uint8_t>(direction);
        // Powered is transient, so only retain it when the source explicitly
        // supplied the state instead of assuming it from a missing property.
        const std::string_view powered = firstStateProperty(state, "powered", "powered_bit");
        if (powered == "true") aux |= 0x08U;
    } else if (leaf == "lantern" || leaf == "soul_lantern") {
        const std::string_view hanging = firstStateProperty(state, "hanging", "hanging_bit");
        if (hanging == "true") aux = 1;
    } else if (contains("ladder") || contains("wall_sign")) {
        const int direction = projectionSixWayFacingAux(facing);
        if (direction >= 0) aux = static_cast<uint8_t>(direction);
    } else if (contains("door")) {
        if (facing == "south") aux = 0;
        else if (facing == "west") aux = 1;
        else if (facing == "north") aux = 2;
        else if (facing == "east") aux = 3;
    } else if (projectionNameEndsWith(leaf, "_glazed_terracotta") ||
                leaf == "pumpkin" || leaf == "carved_pumpkin" || leaf == "jack_o_lantern" ||
               leaf == "lit_pumpkin") {
        const int direction = projectionHorizontalFacingAux(facing);
        if (direction >= 0) aux = static_cast<uint8_t>(direction);
    }

    const bool generic_dyeable = leaf == "wool" || leaf == "concrete" ||
        leaf == "concrete_powder" || leaf == "stained_glass" ||
        leaf == "stained_glass_pane" || leaf == "carpet" ||
        leaf == "stained_hardened_clay";
    if (generic_dyeable) {
        const int color = dyeIndex(stateProperty(state, "color"));
        if (color >= 0) aux = static_cast<uint8_t>((aux & 0xf0U) | color);
    }

    if (leaf == "stone") {
        const std::string_view variant = firstStateProperty(state, "stone_type", "variant");
        if (variant == "granite") aux = 1;
        else if (variant == "granite_smooth" || variant == "polished_granite") aux = 2;
        else if (variant == "diorite") aux = 3;
        else if (variant == "diorite_smooth" || variant == "polished_diorite") aux = 4;
        else if (variant == "andesite") aux = 5;
        else if (variant == "andesite_smooth" || variant == "polished_andesite") aux = 6;
    } else if (leaf == "dirt") {
        const std::string_view variant = firstStateProperty(state, "dirt_type", "variant");
        if (variant == "coarse" || variant == "coarse_dirt") aux = 1U;
        else if (variant == "podzol") aux = 2U;
    } else if (leaf == "sand") {
        const std::string_view variant = firstStateProperty(state, "sand_type", "variant");
        if (variant == "red" || variant == "red_sand") aux = 1U;
    } else if (leaf == "sandstone" || leaf == "red_sandstone") {
        const std::string_view variant = firstStateProperty(state, "sand_stone_type", "variant");
        if (variant == "chiseled" || variant == "heiroglyphs" ||
            variant == "chiseled_sandstone" || variant == "chiseled_red_sandstone") {
            aux = 1U;
        } else if (variant == "smooth" || variant == "smooth_sandstone" ||
                   variant == "smooth_red_sandstone") {
            aux = 2U;
        }
    }
    return aux;
}

bool projectionAirName(std::string_view name) {
    const std::string_view leaf = projectionLeaf(name);
    return leaf == "air" || leaf == "cave_air" || leaf == "void_air";
}

BlockMappingResult projectionStateMapping(std::string_view state) {
    BlockMappingResult result;
    const std::string name = canonicalProjectionBlockName(state);
    if (name.empty()) {
        result.status = BlockMappingStatus::Unsupported;
        result.reason = "projection source state has an invalid identifier";
        return result;
    }
    if (projectionAirName(name)) {
        result.status = BlockMappingStatus::Air;
        return result;
    }
    // Generic legacy names rely on wood_type for their material identity.  A
    // future/invalid value must not silently become oak (aux 0), because that
    // would make the printer consume and place a demonstrably wrong block.
    if (projectionHasUnsupportedWoodType(name, state) ||
        projectionHasUnsupportedLegacyLogType(name, state)) {
        result.status = BlockMappingStatus::Unsupported;
        result.reason = "projection source has an unsupported wood_type";
        return result;
    }
    const std::string_view leaf = projectionLeaf(name);
    uint16_t source_aux = projectionAuxForState(name, state);
    std::string stored_name = name;
    // Bedrock can use one generic shulker identifier plus a color property,
    // whereas the target inventory exposes one item identifier per color.
    // Preserve that material distinction before printer material matching sees
    // the record; an omitted color is the target's uncoloured shulker item.
    if (leaf == "shulker_box") {
        std::string_view color = stateProperty(state, "color");
        if (color == "silver") color = "light_gray";
        if (!color.empty() && dyeIndex(color) < 0) {
            result.status = BlockMappingStatus::Unsupported;
            result.reason = "projection source shulker box has an invalid color";
            return result;
        }
        if (color.empty()) {
            stored_name = "minecraft:undyed_shulker_box";
        } else {
            stored_name = "minecraft:" + std::string(color) + "_shulker_box";
        }
    }
    if (leaf == "daylight_detector" && stateProperty(state, "inverted") == "true") {
        stored_name = "minecraft:daylight_detector_inverted";
    }
    // The target preserves the historic stair namespace: `stone_stairs` is
    // its cobblestone identifier, while a flattened source `stone_stairs`
    // means actual stone stairs. Store the latter under the target's explicit
    // normal_stone_stairs spelling so identity matching can distinguish both.
    if (leaf == "stone_stairs") stored_name = "minecraft:normal_stone_stairs";
    // The target's quartz-pillar direction is carried by the legacy
    // quartz_block aux (2/6/10). Retaining a flattened quartz_pillar name
    // alongside that aux would make source-to-live equality compare two
    // different state encodings, so preserve the canonical legacy carrier.
    if (leaf == "quartz_pillar") stored_name = "minecraft:quartz_block";
    // Java/flattened palettes use `stone_slab[type=bottom/top]` for ordinary
    // stone. The target's same spelling is an old smooth-stone family, so
    // retain an explicit legacy-family material slot (4th family, variant 2)
    // before the shared identity layer sees it. Old palettes with a
    // stone_slab_type property retain their original family semantics.
    const std::string_view legacy_slab_type = stateProperty(state, "stone_slab_type");
    const std::string_view slab_type = firstStateProperty(state, "type", "slab_type");
    const std::string_view slab_half = firstStateProperty(state, "half", "vertical_half");
    const bool modern_stone_slab = leaf == "stone_slab" && legacy_slab_type.empty() &&
        (slab_type == "bottom" || slab_type == "top" || slab_type == "double" ||
         slab_half == "bottom" || slab_half == "top");
    if (modern_stone_slab) {
        const bool is_double = slab_type == "double";
        const bool top = !is_double && (slab_type == "top" || slab_half == "top");
        stored_name = is_double ? "minecraft:double_stone_block_slab4"
                                : "minecraft:stone_block_slab4";
        source_aux = static_cast<uint16_t>(2U | (top ? UINT16_C(0x0008) : 0U));
    }
    // The old generic sandstone name has only default/chiseled/smooth aux
    // variants.  A cut state is a distinct current block item, so retain the
    // explicit identifier instead of silently selecting ordinary sandstone.
    if (leaf == "sandstone" || leaf == "red_sandstone") {
        const std::string_view variant = firstStateProperty(state, "sand_stone_type", "variant");
        if (variant == "cut" || variant == "cut_sandstone" ||
            variant == "cut_red_sandstone") {
            stored_name = leaf == "red_sandstone" ? "minecraft:cut_red_sandstone"
                                                   : "minecraft:cut_sandstone";
            source_aux = 0U;
        }
    }
    result.status = BlockMappingStatus::Mapped;
    result.spec.command_name = std::move(stored_name);
    result.spec.aux = source_aux;
    // These blocks have a deliberately narrower placement policy than an
    // ordinary structural block.  In particular an anvil must land on the
    // upper face of a support block: allowing the generic side/ceiling search
    // can make the client try an impossible floating placement.  Attachment
    // blocks are routed through the printer's face-aware policy instead of
    // being treated as generic fill candidates.
    const bool directional_attachment =
        leaf == "torch" || leaf == "soul_torch" ||
        leaf == "redstone_torch" || leaf == "unlit_redstone_torch" ||
        leaf == "wall_torch" || leaf == "soul_wall_torch" ||
        leaf == "redstone_wall_torch" || leaf == "lever" ||
        leaf == "stone_button" || leaf == "wooden_button" ||
        projectionNameEndsWith(leaf, "_button");
    const bool contentless_container_shell =
        leaf == "chest" || leaf == "trapped_chest" || leaf == "ender_chest" ||
        leaf == "barrel" || leaf == "hopper" || leaf == "dispenser" ||
        leaf == "dropper" || leaf == "furnace" || leaf == "lit_furnace" ||
        leaf == "blast_furnace" || leaf == "lit_blast_furnace" ||
        leaf == "smoker" || leaf == "lit_smoker" || leaf == "shulker_box" ||
        leaf == "undyed_shulker_box" ||
        projectionNameEndsWith(leaf, "_shulker_box");
    const bool adjustable_redstone = leaf == "repeater" || leaf == "comparator" ||
        leaf == "daylight_detector" || leaf == "daylight_detector_inverted";
    if (contentless_container_shell) {
        // The printer restores only the shell's physical direction. It never
        // imports a block actor's items, custom name or other NBT.
        result.spec.phase = ImportPhase::Structure;
        result.spec.can_fill = false;
        result.spec.stateful = true;
    } else if (adjustable_redstone) {
        // These are first placed safely, then their normal in-world use action
        // restores delay, subtract mode or the daylight detector mode after
        // the world confirms the shell.
        result.spec.phase = ImportPhase::Attachment;
        result.spec.can_fill = false;
        result.spec.stateful = true;
    } else if (leaf == "observer") {
        // The source state is preserved for rendering/comparison, but observer
        // placement is deliberately not automated until its six-way ItemUse
        // semantics (including vertical pitch) are verified on this ABI.
        result.spec.phase = ImportPhase::Attachment;
        result.spec.can_fill = false;
        result.spec.stateful = true;
    } else if (leaf == "anvil" || leaf == "chipped_anvil" || leaf == "damaged_anvil") {
        result.spec.phase = ImportPhase::Gravity;
        result.spec.can_fill = true;
    } else if (directional_attachment) {
        result.spec.phase = ImportPhase::Attachment;
        result.spec.can_fill = false;
        // Powered is a transient bit in the live block state, but keeping this
        // marked stateful preserves the source's mounting/orientation state in
        // rendering and comparison.
        result.spec.stateful = leaf == "lever" || leaf == "stone_button" ||
            leaf == "wooden_button" || projectionNameEndsWith(leaf, "_button");
    } else {
        result.spec.phase = ImportPhase::Structure;
        result.spec.can_fill = true;
    }
    return result;
}

BlockMappingResult projectionLegacyMapping(const BlockMapper& mapper,
                                           uint16_t id, uint8_t data) {
    BlockMappingResult result = mapper.mapLegacy(id, data);
    if (result.status != BlockMappingStatus::Unsupported) return result;

    // Old MCEdit numeric IDs do not contain a namespace or a source palette.
    // Preserve a visible placeholder rather than rejecting the entire
    // projection because the import command mapper lacks one legacy entry.
    result.status = BlockMappingStatus::Mapped;
    result.spec.command_name = "minecraft:legacy_block_" + std::to_string(id);
    result.spec.aux = data;
    result.spec.phase = ImportPhase::Structure;
    result.spec.can_fill = true;
    result.reason = "legacy block is rendered with a generic projection material";
    return result;
}

bool checkedCoordinate(int64_t value, int32_t* output) {
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    *output = static_cast<int32_t>(value);
    return true;
}

bool checkedFloorCoordinate(float value, int32_t* output) {
    if (!output || !std::isfinite(value)) return false;
    const double floored = std::floor(static_cast<double>(value));
    if (floored < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        floored > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    *output = static_cast<int32_t>(floored);
    return true;
}

bool rotatePosition(const DiskRecord& record, const BuildProjectionLoadRequest& request,
                    int32_t* x, int32_t* z) {
    const int64_t relative_x = static_cast<int64_t>(record.x) - request.base_x;
    const int64_t relative_z = static_cast<int64_t>(record.z) - request.base_z;
    int64_t rotated_x = record.x;
    int64_t rotated_z = record.z;
    switch (request.rotation_degrees) {
        case 0:
            break;
        case 90:
            rotated_x = static_cast<int64_t>(request.base_x) - relative_z;
            rotated_z = static_cast<int64_t>(request.base_z) + relative_x;
            break;
        case 180:
            rotated_x = static_cast<int64_t>(request.base_x) - relative_x;
            rotated_z = static_cast<int64_t>(request.base_z) - relative_z;
            break;
        case 270:
            rotated_x = static_cast<int64_t>(request.base_x) + relative_z;
            rotated_z = static_cast<int64_t>(request.base_z) - relative_x;
            break;
        default:
            return false;
    }
    return checkedCoordinate(rotated_x, x) && checkedCoordinate(rotated_z, z);
}

PartitionCoord partitionForBlock(int32_t x, int32_t y, int32_t z) {
    return {
        floorDiv(x, kProjectionPartitionSize),
        floorDiv(y, kProjectionPartitionSize),
        floorDiv(z, kProjectionPartitionSize),
    };
}

bool boundsForPartition(const PartitionCoord& coord, BlockBounds* bounds) {
    if (!bounds) return false;
    const int64_t min_x = static_cast<int64_t>(coord.x) * kProjectionPartitionSize;
    const int64_t min_y = static_cast<int64_t>(coord.y) * kProjectionPartitionSize;
    const int64_t min_z = static_cast<int64_t>(coord.z) * kProjectionPartitionSize;
    const int64_t max_x = min_x + kProjectionPartitionSize - 1;
    const int64_t max_y = min_y + kProjectionPartitionSize - 1;
    const int64_t max_z = min_z + kProjectionPartitionSize - 1;
    if (min_x < INT32_MIN || max_x > INT32_MAX ||
        min_y < INT32_MIN || max_y > INT32_MAX ||
        min_z < INT32_MIN || max_z > INT32_MAX) {
        return false;
    }
    *bounds = {
        static_cast<int32_t>(min_x), static_cast<int32_t>(min_y),
        static_cast<int32_t>(min_z), static_cast<int32_t>(max_x),
        static_cast<int32_t>(max_y), static_cast<int32_t>(max_z),
    };
    return true;
}

bool appendPartitionRecordWithHalo(PartitionSpoolWriter* writer,
                                   DiskRecord record, const std::string& name,
                                   std::string* error) {
    if (!writer || !writer->internName(name, &record.name_index, error)) return false;
    const PartitionCoord core = partitionForBlock(record.x, record.y, record.z);
    if (!writer->append(core, record, true, error)) return false;

    const int64_t origin_x = static_cast<int64_t>(core.x) * kProjectionPartitionSize;
    const int64_t origin_y = static_cast<int64_t>(core.y) * kProjectionPartitionSize;
    const int64_t origin_z = static_cast<int64_t>(core.z) * kProjectionPartitionSize;

    std::array<int32_t, 2> x_offsets{{0, 0}};
    std::array<int32_t, 2> y_offsets{{0, 0}};
    std::array<int32_t, 2> z_offsets{{0, 0}};
    size_t x_offset_count = 1;
    size_t y_offset_count = 1;
    size_t z_offset_count = 1;
    if (record.x == origin_x && record.x != INT32_MIN) {
        x_offsets[x_offset_count++] = -1;
    } else if (record.x == origin_x + kProjectionPartitionSize - 1 &&
               record.x != INT32_MAX) {
        x_offsets[x_offset_count++] = 1;
    }
    if (record.y == origin_y && record.y != INT32_MIN) {
        y_offsets[y_offset_count++] = -1;
    } else if (record.y == origin_y + kProjectionPartitionSize - 1 &&
               record.y != INT32_MAX) {
        y_offsets[y_offset_count++] = 1;
    }
    if (record.z == origin_z && record.z != INT32_MIN) {
        z_offsets[z_offset_count++] = -1;
    } else if (record.z == origin_z + kProjectionPartitionSize - 1 &&
               record.z != INT32_MAX) {
        z_offsets[z_offset_count++] = 1;
    }

    // A renderer edge consults both face neighbours and their diagonal. Taking
    // the Cartesian product gives every target partition a complete one-voxel
    // Chebyshev halo while only duplicating boundary records that can be used.
    for (size_t x_index = 0; x_index < x_offset_count; ++x_index) {
        for (size_t y_index = 0; y_index < y_offset_count; ++y_index) {
            for (size_t z_index = 0; z_index < z_offset_count; ++z_index) {
                const int32_t x_offset = x_offsets[x_index];
                const int32_t y_offset = y_offsets[y_index];
                const int32_t z_offset = z_offsets[z_index];
                if (x_offset == 0 && y_offset == 0 && z_offset == 0) continue;
                const PartitionCoord neighbor{
                    core.x + x_offset, core.y + y_offset, core.z + z_offset,
                };
                if (!writer->append(neighbor, record, false, error)) return false;
            }
        }
    }
    return true;
}

bool coordinateInBounds(const BlockPosition& position, const BlockBounds& bounds) {
    return position.x >= bounds.min_x && position.x <= bounds.max_x &&
        position.y >= bounds.min_y && position.y <= bounds.max_y &&
        position.z >= bounds.min_z && position.z <= bounds.max_z;
}

bool coordinateInChebyshevHalo(const BlockPosition& position,
                               const BlockBounds& bounds) {
    const int64_t x = position.x, y = position.y, z = position.z;
    const int64_t min_x = bounds.min_x, min_y = bounds.min_y, min_z = bounds.min_z;
    const int64_t max_x = bounds.max_x, max_y = bounds.max_y, max_z = bounds.max_z;
    return x >= min_x - 1 && x <= max_x + 1 &&
        y >= min_y - 1 && y <= max_y + 1 &&
        z >= min_z - 1 && z <= max_z + 1;
}

struct PendingProjectionBlock {
    ProjectionBlock block;
    uint16_t material_name_index = 0;
};

struct CachedProjectionPrinterTarget {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint16_t aux = 0;
    uint16_t name_index = 0;
    uint8_t flags = 0;
    uint8_t phase = 0;
};

struct ProjectionPrinterPartitionCacheEntry {
    std::vector<CachedProjectionPrinterTarget> targets;
    uint64_t last_access = 0;
};

struct ProjectionDisplayScopeCountKey {
    uint64_t generation = 0;
    uint64_t plan_identity = 0;
    // The partition fields make cache ownership/debugging clear, while the
    // precise view block keeps a count accurate at range boundaries.
    int32_t player_partition_x = 0;
    int32_t player_partition_z = 0;
    int32_t view_block_x = 0;
    int32_t view_block_z = 0;
    int32_t range_chunks = 0;
    int64_t minimum_world_y = INT32_MIN;
    int64_t maximum_world_y = INT32_MAX;

    bool operator==(const ProjectionDisplayScopeCountKey& other) const {
        return generation == other.generation && plan_identity == other.plan_identity &&
            player_partition_x == other.player_partition_x &&
            player_partition_z == other.player_partition_z &&
            view_block_x == other.view_block_x && view_block_z == other.view_block_z &&
            range_chunks == other.range_chunks &&
            minimum_world_y == other.minimum_world_y &&
            maximum_world_y == other.maximum_world_y;
    }
};

struct ProjectionDisplayScopeCountKeyHash {
    size_t operator()(const ProjectionDisplayScopeCountKey& key) const {
        uint64_t hash = 1469598103934665603ULL;
        const auto add = [&](uint64_t value) {
            hash ^= value;
            hash *= 1099511628211ULL;
        };
        add(key.generation);
        add(key.plan_identity);
        add(static_cast<uint32_t>(key.player_partition_x));
        add(static_cast<uint32_t>(key.player_partition_z));
        add(static_cast<uint32_t>(key.view_block_x));
        add(static_cast<uint32_t>(key.view_block_z));
        add(static_cast<uint32_t>(key.range_chunks));
        add(static_cast<uint64_t>(key.minimum_world_y));
        add(static_cast<uint64_t>(key.maximum_world_y));
        return static_cast<size_t>(hash ^ (hash >> 32U));
    }
};

struct ProjectionDisplayScopeCountEntry {
    uint64_t block_count = 0;
    uint64_t last_access = 0;
    bool known = false;
    bool pending = false;
    // A malformed/inaccessible source must not cause every printer tick to
    // immediately enqueue the same full-scope scan again.  The next
    // projection publication creates a fresh source/cache and retries there.
    bool failed = false;
};

struct ProjectionDisplayScopeCountRequest {
    ProjectionDisplayScopeCountKey key;
    ProjectionDisplayScope scope;
    float view_x = 0.f;
    float view_z = 0.f;
};

bool readPartitionSpool(
        std::ifstream* stream, uint64_t container_size,
        const PartitionCoord& coord,
        const PartitionSpoolWriter::PartitionInfo& partition_info,
        const BuildProjectionLoadRequest& request,
        const std::vector<std::string_view>& material_names,
        const std::function<bool()>& cancelled,
        ProjectionPartitionInput* partition,
        uint64_t maximum_core_records,
        uint64_t* core_record_count,
        size_t* core_cell_count,
        std::string* error) {
    if (!stream || !*stream || !partition || !core_record_count || !core_cell_count ||
        !boundsForPartition(coord, &partition->core_bounds)) {
        if (error) *error = "projection partition has invalid coordinate bounds";
        return false;
    }
    if (partition_info.total_records == 0 ||
        partition_info.tail_offset == kInvalidProjectionChunkOffset ||
        partition_info.core_records > maximum_core_records) {
        if (error) *error = "projection partition index is inconsistent";
        return false;
    }

    std::unordered_map<BlockPosition, PendingProjectionBlock, BlockPositionHash> cells;
    cells.reserve(static_cast<size_t>(std::min<uint64_t>(
        partition_info.total_records, kMaximumPartitionCells)));
    uint64_t record_index = 0;
    uint64_t visited_records = 0;
    uint64_t chunk_offset = partition_info.tail_offset;
    *core_record_count = 0;
    *core_cell_count = 0;
    while (chunk_offset != kInvalidProjectionChunkOffset) {
        if (chunk_offset > container_size ||
            sizeof(PartitionChunkHeader) > container_size - chunk_offset) {
            if (error) *error = "projection partition chunk offset is outside its storage";
            return false;
        }
        stream->clear();
        stream->seekg(static_cast<std::streamoff>(chunk_offset), std::ios::beg);
        PartitionChunkHeader header{};
        stream->read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!*stream || stream->gcount() != static_cast<std::streamsize>(sizeof(header)) ||
            header.magic != kProjectionChunkMagic ||
            header.partition_x != coord.x || header.partition_y != coord.y ||
            header.partition_z != coord.z || header.record_count == 0 ||
            header.record_count > kPartitionRecordsPerChunk ||
            (header.previous_offset != kInvalidProjectionChunkOffset &&
             header.previous_offset >= chunk_offset)) {
            if (error) *error = "projection partition contains an invalid chunk";
            return false;
        }
        const size_t chunk_record_bytes =
            static_cast<size_t>(header.record_count) * sizeof(DiskRecord);
        const uint64_t records_offset = chunk_offset + sizeof(PartitionChunkHeader);
        if (records_offset > container_size ||
            chunk_record_bytes > container_size - records_offset ||
            visited_records > partition_info.total_records ||
            header.record_count > partition_info.total_records - visited_records) {
            if (error) *error = "projection partition contains a truncated chunk";
            return false;
        }
        std::array<DiskRecord, kPartitionRecordsPerChunk> records{};
        stream->read(reinterpret_cast<char*>(records.data()),
                     static_cast<std::streamsize>(chunk_record_bytes));
        if (!*stream || stream->gcount() != static_cast<std::streamsize>(chunk_record_bytes)) {
            if (error) *error = "projection partition contains a truncated chunk";
            return false;
        }
        visited_records += header.record_count;
        chunk_offset = header.previous_offset;

        // Chunks form a newest-to-oldest chain. Walking each chunk backwards
        // and keeping the first coordinate preserves last-write-wins semantics
        // without first materializing or sorting every record in the partition.
        for (size_t reverse = header.record_count; reverse != 0; --reverse) {
            if ((record_index++ & 0x3ffU) == 0 && cancelled()) {
                if (error) *error = "projection load cancelled";
                return false;
            }
            const DiskRecord& record = records[reverse - 1U];
            if ((record.flags & kProjectionRecordFlag) == 0 ||
                record.name_index >= material_names.size() ||
                material_names[record.name_index].empty()) {
                if (error) *error = "projection spatial partition contains an invalid record";
                return false;
            }
            const BlockPosition position{record.x, record.y, record.z};
            const bool in_core = coordinateInBounds(position, partition->core_bounds);
            if (!in_core &&
                !coordinateInChebyshevHalo(position, partition->core_bounds)) {
                if (error) *error = "projection spatial partition contains an out-of-range block";
                return false;
            }
            if (((record.flags & kProjectionCoreFlag) != 0) != in_core) {
                if (error) *error = "projection spatial partition has an invalid core marker";
                return false;
            }
            if (in_core) {
                if (*core_record_count >= maximum_core_records) {
                    if (error) *error = "projection partitions contain more blocks than parsed";
                    return false;
                }
                ++(*core_record_count);
            }

            PendingProjectionBlock pending;
            pending.block.x = record.x;
            pending.block.y = record.y;
            pending.block.z = record.z;
            pending.block.aux = static_cast<uint8_t>(record.aux & 0xFFU);
            pending.block.rotation_quarters =
                static_cast<uint8_t>(request.rotation_degrees / 90);
            pending.block.source_aux = record.aux;
            pending.block.source_name_index = record.name_index;
            pending.block.source_flags = static_cast<uint8_t>(
                record.flags & (kProjectionCanFillFlag | kProjectionSingleLayerFlag |
                                kProjectionStatefulFlag));
            pending.block.source_phase = record.phase;
            pending.material_name_index = record.name_index;
            cells.emplace(position, std::move(pending));
        }
    }
    if (visited_records != partition_info.total_records ||
        *core_record_count != partition_info.core_records) {
        if (error) *error = "projection partition record index does not match its storage";
        return false;
    }

    ProjectionBlueprint& blueprint = partition->blueprint;
    blueprint.blocks.reserve(cells.size());
    blueprint.names.reserve(std::min<size_t>(cells.size(), 1024U));
    std::unordered_map<uint16_t, uint32_t> name_indices;
    name_indices.reserve(std::min<size_t>(cells.size(), 1024U));
    size_t retained_material_bytes = 0;
    for (auto& entry : cells) {
        PendingProjectionBlock& pending = entry.second;
        auto existing_name = name_indices.find(pending.material_name_index);
        if (existing_name == name_indices.end()) {
            if (blueprint.names.size() >= std::numeric_limits<uint32_t>::max()) {
                if (error) *error = "projection partition has too many materials";
                return false;
            }
            const std::string_view name = material_names[pending.material_name_index];
            if (retained_material_bytes > kMaximumPartitionMaterialBytes ||
                name.size() > kMaximumPartitionMaterialBytes - retained_material_bytes) {
                if (error) *error = "projection partition has too much material metadata";
                return false;
            }
            const uint32_t index = static_cast<uint32_t>(blueprint.names.size());
            blueprint.names.emplace_back(name);
            name_indices.emplace(pending.material_name_index, index);
            retained_material_bytes += name.size();
            pending.block.name_index = index;
        } else {
            pending.block.name_index = existing_name->second;
        }
        if (coordinateInBounds(entry.first, partition->core_bounds)) ++(*core_cell_count);
        blueprint.blocks.push_back(pending.block);
    }
    return true;
}

[[maybe_unused]] bool streamPartitionContainer(
        const PartitionSpoolWriter& writer,
        const BuildProjectionLoadRequest& request,
        uint64_t expected_block_count,
        const std::function<bool()>& cancelled,
        const std::function<void(uint64_t)>& progress,
        std::string* error) {
    std::array<char, kSpoolReadBufferSize> stream_buffer{};
    std::ifstream stream;
    stream.rdbuf()->pubsetbuf(stream_buffer.data(),
                             static_cast<std::streamsize>(stream_buffer.size()));
    stream.open(writer.containerPath(), std::ios::binary | std::ios::ate);
    if (!stream) {
        if (error) *error = "cannot open projection partition storage";
        return false;
    }
    const std::streamoff end = static_cast<std::streamoff>(stream.tellg());
    if (end <= 0) {
        if (error) *error = "projection partition storage is empty";
        return false;
    }
    const uint64_t container_size = static_cast<uint64_t>(end);
    uint64_t consumed_core_records = 0;
    bool success = true;
    // Partitions live in a hash map, so replaying them in map order seeks all
    // over the container. Visiting them by descending tail offset walks the
    // chunk chains close to backwards-linear instead, which matters on flash
    // storage for large blueprints. Partitions are independent, so ordering
    // only changes I/O locality.
    std::vector<std::pair<PartitionCoord, PartitionSpoolWriter::PartitionInfo>>
        ordered_partitions;
    try {
        ordered_partitions.reserve(writer.partitions().size());
        for (const auto& entry : writer.partitions()) {
            ordered_partitions.push_back(entry);
        }
    } catch (const std::bad_alloc&) {
        if (error) *error = "not enough memory to order projection partitions";
        return false;
    }
    std::sort(ordered_partitions.begin(), ordered_partitions.end(),
              [](const auto& left, const auto& right) {
                  return left.second.tail_offset > right.second.tail_offset;
              });
    for (const auto& entry : ordered_partitions) {
        if (cancelled()) {
            if (error) *error = "projection load cancelled";
            success = false;
            break;
        }
        const PartitionCoord& coord = entry.first;
        const PartitionSpoolWriter::PartitionInfo& partition_info = entry.second;
        // A halo-only target has no cubes of its own and therefore cannot emit
        // a render group. Its records exist only to cull faces for a core that
        // never arrived, so skip the chain entirely.
        if (partition_info.core_records == 0) continue;
        ProjectionPartitionInput partition;
        uint64_t core_records = 0;
        size_t core_cells = 0;
        if (consumed_core_records > expected_block_count) {
            if (error) *error = "projection partitions contain more blocks than parsed";
            success = false;
            break;
        }
        if (!readPartitionSpool(&stream, container_size, coord, partition_info,
                                request, writer.materialNames(), cancelled, &partition,
                                expected_block_count - consumed_core_records,
                                &core_records, &core_cells, error)) {
            success = false;
            break;
        }
        if (consumed_core_records > expected_block_count ||
            core_records > expected_block_count - consumed_core_records) {
            if (error) *error = "projection partitions contain more blocks than parsed";
            success = false;
            break;
        }
        consumed_core_records += core_records;
        if (core_cells != 0 &&
            !BuildProjectionRenderer::instance().appendStreamingPartition(
                std::move(partition), cancelled, error)) {
            success = false;
            break;
        }
        if (progress && (consumed_core_records == expected_block_count ||
                         (consumed_core_records % kStatusBlockInterval) < core_records)) {
            progress(consumed_core_records);
        }
    }
    if (!success) return false;
    if (consumed_core_records != expected_block_count) {
        if (error) *error = "projection partition count does not match the parsed blueprint";
        return false;
    }
    stream.close();
    if (std::remove(writer.containerPath().c_str()) != 0) {
        if (error) *error = "cannot release consumed projection partition storage";
        return false;
    }
    return true;
}

std::string progressText(ProjectionSourceType type,
                         const SchematicParseProgress& progress) {
    std::string text;
    switch (progress.stage) {
        case SchematicParseStage::ReadingSource:
            text = std::string("reading ") + sourceLabel(type);
            break;
        case SchematicParseStage::RoutingBlocks:
            text = std::string("decoding ") + sourceLabel(type) + " blocks";
            break;
        case SchematicParseStage::FinalizingSpools:
            text = "finalizing projection block data";
            break;
    }
    if (progress.total != 0) {
        const uint64_t completed = std::min(progress.completed, progress.total);
        const uint64_t percent = static_cast<uint64_t>(
            static_cast<long double>(completed) * 100.0L / progress.total);
        text += " (" + std::to_string(percent) + "%)";
    }
    return text;
}

}  // namespace

struct BuildProjectionRuntime::LazyProjectionSource {
    uint64_t generation = 0;
    uint64_t plan_identity = 0;
    uint64_t source_block_count = 0;
    uint64_t raw_spool_size = 0;
    int32_t relative_origin_y = 0;
    std::string raw_spool_path;
    std::string ready_status;
    BuildProjectionLoadRequest request;
    std::unordered_map<PartitionCoord, PartitionSpoolWriter::PartitionInfo,
                       PartitionCoordHash> partitions;
    // A 32-block X/Z column is indexed independently from Y so tall buildings
    // start at the player height instead of eagerly constructing every floor.
    std::unordered_map<uint64_t, std::vector<PartitionCoord>> columns;
    std::vector<std::string> material_names;
    std::vector<std::string_view> material_name_views;
    std::unordered_set<PartitionCoord, PartitionCoordHash> built_partitions;
    std::unordered_map<uint64_t, size_t> built_partitions_per_column;
    uint64_t total_core_partitions = 0;
    uint64_t queued_core_records = 0;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> raw_spool_released{false};
    // Raw records stay available for the entire published projection lifetime.
    // The lazy renderer mutates only its built_* accounting; the fields read
    // by the printer below are immutable once load() publishes this source.
    std::atomic<bool> printer_active{true};
    std::mutex printer_io_mutex;
    std::unordered_map<PartitionCoord, ProjectionPrinterPartitionCacheEntry,
                       PartitionCoordHash> printer_partition_cache;
    uint64_t printer_partition_access_serial = 0;
    // Printer tick calls only inspect this memory cache.  Cold partitions are
    // decoded by a dedicated worker so seeking the raw spool never steals a
    // game tick.
    std::mutex printer_target_request_mutex;
    std::condition_variable printer_target_request_cv;
    std::deque<PartitionCoord> printer_target_requests;
    std::unordered_set<PartitionCoord, PartitionCoordHash> printer_target_pending;
    std::unordered_set<PartitionCoord, PartitionCoordHash> printer_target_failed;
    std::atomic<bool> printer_target_stop{false};
    std::atomic<bool> printer_target_load_failed{false};
    std::mutex display_scope_count_mutex;
    std::condition_variable display_scope_count_cv;
    std::deque<ProjectionDisplayScopeCountRequest> display_scope_count_requests;
    std::unordered_map<ProjectionDisplayScopeCountKey,
                       ProjectionDisplayScopeCountEntry,
                       ProjectionDisplayScopeCountKeyHash> display_scope_counts;
    uint64_t display_scope_count_access_serial = 0;
    std::atomic<bool> display_scope_count_stop{false};
    // Unlike the display-scope count cache, this is exactly one immutable
    // whole-projection summary.  It is built from raw core cells on its own
    // worker so UI callers can page it without seeking the spool themselves.
    std::mutex material_summary_mutex;
    std::vector<ProjectionMaterialSummaryEntry> material_summary_entries;
    uint64_t material_summary_block_count = 0;
    bool material_summary_ready = false;
    bool material_summary_failed = false;
    std::atomic<bool> material_summary_stop{false};

    ~LazyProjectionSource() { releaseRawSpool(); }

    void releaseRawSpool() noexcept {
        bool expected = false;
        if (!raw_spool_released.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
        if (!raw_spool_path.empty()) std::remove(raw_spool_path.c_str());
    }
};

namespace {

constexpr size_t kMaximumCachedPrinterPartitions = 12U;
constexpr size_t kMaximumDisplayScopeCountEntries = 16U;
constexpr size_t kMaximumQueuedDisplayScopeCounts = 2U;
constexpr int32_t kMaximumWorldMatchRegionSpan = 32;
constexpr size_t kMaximumWorldMatchTargetPageTargets = 256U;
constexpr uint64_t kWorldMatchCursorIndexMask = UINT64_C(0xffffffff);

bool validWorldMatchRegion(const ProjectionWorldMatchRegion& region) {
    if (region.min_x > region.max_x || region.min_y > region.max_y ||
        region.min_z > region.max_z) {
        return false;
    }
    const int64_t span_x = static_cast<int64_t>(region.max_x) - region.min_x + 1;
    const int64_t span_y = static_cast<int64_t>(region.max_y) - region.min_y + 1;
    const int64_t span_z = static_cast<int64_t>(region.max_z) - region.min_z + 1;
    return span_x > 0 && span_x <= kMaximumWorldMatchRegionSpan &&
        span_y > 0 && span_y <= kMaximumWorldMatchRegionSpan &&
        span_z > 0 && span_z <= kMaximumWorldMatchRegionSpan;
}

bool decodeWorldMatchCursor(uint64_t cursor, size_t* partition_ordinal,
                            size_t* target_index) {
    if (!partition_ordinal || !target_index) return false;
    if (cursor == 0) {
        *partition_ordinal = 0;
        *target_index = 0;
        return true;
    }
    const uint64_t encoded_ordinal = cursor >> 32U;
    if (encoded_ordinal == 0 ||
        encoded_ordinal > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        (cursor & kWorldMatchCursorIndexMask) >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    *partition_ordinal = static_cast<size_t>(encoded_ordinal - 1U);
    *target_index = static_cast<size_t>(cursor & kWorldMatchCursorIndexMask);
    return true;
}

uint64_t encodeWorldMatchCursor(size_t partition_ordinal, size_t target_index) {
    if (partition_ordinal >= static_cast<size_t>(UINT32_MAX) ||
        target_index > static_cast<size_t>(UINT32_MAX)) {
        return 0;
    }
    return (static_cast<uint64_t>(partition_ordinal + 1U) << 32U) |
        static_cast<uint64_t>(target_index);
}

bool cachedProjectionPrinterTargetLess(const CachedProjectionPrinterTarget& left,
                                       const CachedProjectionPrinterTarget& right) {
    // The page cursor addresses this vector directly, so its ordering must not
    // depend on the unordered source-cell map used while decoding a partition.
    if (left.z != right.z) return left.z < right.z;
    if (left.y != right.y) return left.y < right.y;
    if (left.x != right.x) return left.x < right.x;
    if (left.name_index != right.name_index) return left.name_index < right.name_index;
    if (left.aux != right.aux) return left.aux < right.aux;
    if (left.phase != right.phase) return left.phase < right.phase;
    return left.flags < right.flags;
}

bool targetWithinWorldMatchRegion(const CachedProjectionPrinterTarget& target,
                                  const ProjectionWorldMatchRegion& region) {
    return target.x >= region.min_x && target.x <= region.max_x &&
        target.y >= region.min_y && target.y <= region.max_y &&
        target.z >= region.min_z && target.z <= region.max_z;
}

bool projectionPrinterSourceActive(
        const std::shared_ptr<BuildProjectionRuntime::LazyProjectionSource>& source) {
    return source && source->printer_active.load(std::memory_order_acquire) &&
        !source->raw_spool_released.load(std::memory_order_acquire);
}

// Converts an imported source cell into the identity of the ItemStack needed
// to create it.  Keep this separate from exact world matching: a material
// list must not split one item merely because its final block state has a
// facing, a slab half, or a dynamic redstone value.  Unknown aux variants are
// deliberately retained rather than guessed into a different material.
ProjectionBlockIdentity materialSummaryIdentity(std::string_view source_name,
                                                uint16_t source_aux,
                                                uint8_t source_flags) {
    ProjectionBlockIdentity identity =
        NormalizeProjectionBlockIdentity(source_name, source_aux);
    const std::string canonical_name = ProjectionCanonicalBlockName(source_name);
    const bool placement_state =
        (source_flags & kProjectionStatefulFlag) != 0U ||
        IsProjectionSlabBlock(canonical_name) ||
        canonical_name.find("stairs") != std::string::npos ||
        ProjectionBlockMaterialMatches(source_name, source_aux, source_name, 0U);
    if (placement_state) identity.aux = 0U;

    // Inverted daylight detectors are switched in-world but use the exact
    // same inventory material as the normal detector.
    if (IsProjectionDaylightDetector(identity.name)) {
        identity.name = "daylight_detector";
        identity.aux = 0U;
    }
    return identity;
}

bool buildProjectionMaterialSummary(
        const std::shared_ptr<BuildProjectionRuntime::LazyProjectionSource>& source,
        std::vector<ProjectionMaterialSummaryEntry>* output,
        uint64_t* total_block_count, std::string* error) {
    if (!source || !output || !total_block_count || !projectionPrinterSourceActive(source)) {
        if (error) *error = "projection material source is unavailable";
        return false;
    }
    std::ifstream stream(source->raw_spool_path, std::ios::binary);
    if (!stream) {
        if (error) *error = "cannot open projection material source storage";
        return false;
    }

    using MaterialKey = std::pair<std::string, uint16_t>;
    std::map<MaterialKey, uint64_t> counts;
    uint64_t final_core_cells = 0;
    const auto cancelled = [&]() {
        return !projectionPrinterSourceActive(source) ||
            source->material_summary_stop.load(std::memory_order_acquire);
    };

    // Every original cell belongs to exactly one core partition.  The same
    // record can occur in several neighbouring partition halos, but we only
    // inspect core_bounds here. readPartitionSpool also resolves duplicate
    // writes within that core newest-to-oldest before exposing a cell.
    for (const auto& entry : source->partitions) {
        if (cancelled()) {
            if (error) *error = "projection material summary was cancelled";
            return false;
        }
        if (entry.second.core_records == 0) continue;

        ProjectionPartitionInput partition;
        uint64_t core_records = 0;
        size_t core_cells = 0;
        if (!readPartitionSpool(&stream, source->raw_spool_size, entry.first,
                                entry.second, source->request,
                                source->material_name_views, cancelled, &partition,
                                entry.second.core_records, &core_records, &core_cells,
                                error)) {
            return false;
        }
        for (const ProjectionBlock& block : partition.blueprint.blocks) {
            const BlockPosition position{block.x, block.y, block.z};
            if (!coordinateInBounds(position, partition.core_bounds)) continue;
            if (block.source_name_index >= source->material_names.size() ||
                source->material_names[block.source_name_index].empty() ||
                block.source_phase <= static_cast<uint8_t>(ImportPhase::Clear) ||
                block.source_phase >= static_cast<uint8_t>(ImportPhase::Count)) {
                if (error) *error = "projection material source contains an invalid core block";
                return false;
            }
            const ProjectionBlockIdentity identity = materialSummaryIdentity(
                source->material_names[block.source_name_index], block.source_aux,
                block.source_flags);
            if (identity.name.empty()) {
                if (error) *error = "projection material source has an empty item identity";
                return false;
            }
            if (final_core_cells == std::numeric_limits<uint64_t>::max()) {
                if (error) *error = "projection material block count overflows";
                return false;
            }
            ++final_core_cells;
            const MaterialKey key{identity.name, identity.aux};
            auto material = counts.find(key);
            if (material == counts.end()) {
                counts.emplace(key, 1U);
            } else {
                if (material->second == std::numeric_limits<uint64_t>::max()) {
                    if (error) *error = "projection material count overflows";
                    return false;
                }
                ++material->second;
            }
        }
    }

    output->clear();
    output->reserve(counts.size());
    for (const auto& material : counts) {
        output->push_back({material.first.first, material.first.second, material.second});
    }
    // The preview is primarily a shopping/collection list: surface the largest
    // requirements first.  Keep the lexical identity order as a deterministic
    // tie-breaker so entries never reshuffle between pages for the same plan.
    std::sort(output->begin(), output->end(),
              [](const ProjectionMaterialSummaryEntry& left,
                 const ProjectionMaterialSummaryEntry& right) {
                  if (left.count != right.count) return left.count > right.count;
                  if (left.item_name != right.item_name) {
                      return left.item_name < right.item_name;
                  }
                  return left.item_aux < right.item_aux;
              });
    *total_block_count = final_core_cells;
    return true;
}

bool loadPrinterPartition(
        const std::shared_ptr<BuildProjectionRuntime::LazyProjectionSource>& source,
        const PartitionCoord& coord, std::string* error) {
    if (!projectionPrinterSourceActive(source)) {
        if (error) *error = "projection printer source is unavailable";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(source->printer_io_mutex);
        const auto cached = source->printer_partition_cache.find(coord);
        if (cached != source->printer_partition_cache.end()) {
            cached->second.last_access = ++source->printer_partition_access_serial;
            return true;
        }
    }
    const auto info = source->partitions.find(coord);
    if (info == source->partitions.end() || info->second.core_records == 0) return true;

    std::ifstream stream(source->raw_spool_path, std::ios::binary);
    if (!stream) {
        if (error) *error = "cannot open projection printer source storage";
        return false;
    }
    ProjectionPartitionInput partition;
    uint64_t core_records = 0;
    size_t core_cells = 0;
    const auto cancelled = [&]() {
        return !projectionPrinterSourceActive(source);
    };
    if (!readPartitionSpool(&stream, source->raw_spool_size, coord, info->second,
                            source->request, source->material_name_views, cancelled,
                            &partition, info->second.core_records, &core_records,
                            &core_cells, error)) {
        return false;
    }
    if (!projectionPrinterSourceActive(source)) {
        if (error) *error = "projection printer source was cleared";
        return false;
    }

    ProjectionPrinterPartitionCacheEntry entry;
    entry.targets.reserve(core_cells);
    for (const ProjectionBlock& block : partition.blueprint.blocks) {
        const BlockPosition position{block.x, block.y, block.z};
        if (!coordinateInBounds(position, partition.core_bounds)) continue;
        if (block.source_name_index >= source->material_names.size() ||
            source->material_names[block.source_name_index].empty() ||
            block.source_phase <= static_cast<uint8_t>(ImportPhase::Clear) ||
            block.source_phase >= static_cast<uint8_t>(ImportPhase::Count)) {
            if (error) *error = "projection printer source contains an invalid core target";
            return false;
        }
        entry.targets.push_back({block.x, block.y, block.z, block.source_aux,
                                 block.source_name_index, block.source_flags,
                                 block.source_phase});
    }
    if (entry.targets.size() != core_cells) {
        if (error) *error = "projection printer core target count is inconsistent";
        return false;
    }
    std::sort(entry.targets.begin(), entry.targets.end(), cachedProjectionPrinterTargetLess);
    {
        std::lock_guard<std::mutex> lock(source->printer_io_mutex);
        if (!projectionPrinterSourceActive(source)) {
            if (error) *error = "projection printer source was cleared";
            return false;
        }
        const auto existing = source->printer_partition_cache.find(coord);
        if (existing != source->printer_partition_cache.end()) {
            existing->second.last_access = ++source->printer_partition_access_serial;
            return true;
        }
        entry.last_access = ++source->printer_partition_access_serial;
        if (source->printer_partition_cache.size() >= kMaximumCachedPrinterPartitions) {
            auto oldest = source->printer_partition_cache.end();
            for (auto it = source->printer_partition_cache.begin();
                 it != source->printer_partition_cache.end(); ++it) {
                if (oldest == source->printer_partition_cache.end() ||
                    it->second.last_access < oldest->second.last_access) {
                    oldest = it;
                }
            }
            if (oldest != source->printer_partition_cache.end()) {
                source->printer_partition_cache.erase(oldest);
            }
        }
        source->printer_partition_cache.emplace(coord, std::move(entry));
    }
    return true;
}

void schedulePrinterPartitionLoad(
        const std::shared_ptr<BuildProjectionRuntime::LazyProjectionSource>& source,
        const PartitionCoord& coord) {
    if (!projectionPrinterSourceActive(source)) return;
    const auto info = source->partitions.find(coord);
    if (info == source->partitions.end() || info->second.core_records == 0) return;
    // This helper is used from the local-player tick.  A deferred request is
    // harmless because the next tick retries it, whereas waiting behind the
    // worker would turn a cold source-cache lookup into a hitch.
    std::unique_lock<std::mutex> lock(source->printer_target_request_mutex,
                                      std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (source->printer_target_stop.load(std::memory_order_acquire) ||
        source->printer_target_failed.find(coord) != source->printer_target_failed.end() ||
        !source->printer_target_pending.emplace(coord).second) {
        return;
    }
    constexpr size_t kMaximumQueuedPrinterPartitions = 24U;
    while (source->printer_target_requests.size() >= kMaximumQueuedPrinterPartitions) {
        const PartitionCoord dropped = source->printer_target_requests.front();
        source->printer_target_requests.pop_front();
        source->printer_target_pending.erase(dropped);
    }
    source->printer_target_requests.push_back(coord);
    source->printer_target_request_cv.notify_one();
}

ProjectionDisplayScopeCountKey makeDisplayScopeCountKey(
        const BuildProjectionRuntime::LazyProjectionSource& source,
        const ProjectionDisplayScope& scope, int32_t view_x, int32_t view_z) {
    return {
        source.generation,
        source.plan_identity,
        floorDiv(view_x, kProjectionPartitionSize),
        floorDiv(view_z, kProjectionPartitionSize),
        view_x,
        view_z,
        scope.range_chunks,
        scope.minimum_world_y,
        scope.maximum_world_y,
    };
}

bool scopeCouldContainPartition(const ProjectionDisplayScope& scope,
                                float view_x, float view_z,
                                const BlockBounds& bounds) {
    if (!bounds.isValid() || bounds.max_y < scope.minimum_world_y ||
        bounds.min_y > scope.maximum_world_y) {
        return false;
    }
    const double range = static_cast<double>(scope.range_chunks) * 16.0;
    const double x = static_cast<double>(view_x);
    const double z = static_cast<double>(view_z);
    const double minimum_x = static_cast<double>(bounds.min_x);
    const double maximum_x = static_cast<double>(bounds.max_x) + 1.0;
    const double minimum_z = static_cast<double>(bounds.min_z);
    const double maximum_z = static_cast<double>(bounds.max_z) + 1.0;
    const double dx = x < minimum_x ? minimum_x - x : (x > maximum_x ? x - maximum_x : 0.0);
    const double dz = z < minimum_z ? minimum_z - z : (z > maximum_z ? z - maximum_z : 0.0);
    return dx * dx + dz * dz <= range * range;
}

bool countDisplayScopeBlocks(
        const std::shared_ptr<BuildProjectionRuntime::LazyProjectionSource>& source,
        const ProjectionDisplayScopeCountRequest& request, uint64_t* output,
        std::string* error) {
    if (!output || !projectionPrinterSourceActive(source)) {
        if (error) *error = "projection printer source is unavailable";
        return false;
    }
    std::ifstream stream(source->raw_spool_path, std::ios::binary);
    if (!stream) {
        if (error) *error = "cannot open projection display-scope storage";
        return false;
    }
    uint64_t count = 0;
    const auto cancelled = [&]() {
        return !projectionPrinterSourceActive(source) ||
            source->display_scope_count_stop.load(std::memory_order_acquire);
    };
    for (const auto& item : source->partitions) {
        if (cancelled()) {
            if (error) *error = "projection display-scope count was cancelled";
            return false;
        }
        if (item.second.core_records == 0) continue;
        BlockBounds bounds;
        if (!boundsForPartition(item.first, &bounds) ||
            !scopeCouldContainPartition(request.scope, request.view_x, request.view_z, bounds)) {
            continue;
        }
        ProjectionPartitionInput partition;
        uint64_t core_records = 0;
        size_t core_cells = 0;
        if (!readPartitionSpool(&stream, source->raw_spool_size, item.first, item.second,
                                source->request, source->material_name_views, cancelled,
                                &partition, item.second.core_records, &core_records,
                                &core_cells, error)) {
            return false;
        }
        for (const ProjectionBlock& block : partition.blueprint.blocks) {
            const BlockPosition position{block.x, block.y, block.z};
            if (!coordinateInBounds(position, partition.core_bounds) ||
                !projectionDisplayScopeContainsBlock(request.scope, request.view_x,
                                                     request.view_z, block.x, block.y,
                                                     block.z)) {
                continue;
            }
            if (count == std::numeric_limits<uint64_t>::max()) {
                if (error) *error = "projection display-scope block count overflows";
                return false;
            }
            ++count;
        }
    }
    *output = count;
    return true;
}

void scheduleDisplayScopeCount(
        const std::shared_ptr<BuildProjectionRuntime::LazyProjectionSource>& source,
        const ProjectionDisplayScopeCountRequest& request) {
    if (!projectionPrinterSourceActive(source)) return;
    std::lock_guard<std::mutex> lock(source->display_scope_count_mutex);
    if (source->display_scope_count_stop.load(std::memory_order_acquire)) return;
    auto existing = source->display_scope_counts.find(request.key);
    if (existing != source->display_scope_counts.end()) {
        existing->second.last_access = ++source->display_scope_count_access_serial;
        if (existing->second.known || existing->second.pending ||
            existing->second.failed) {
            return;
        }
        existing->second.pending = true;
    } else {
        if (source->display_scope_counts.size() >= kMaximumDisplayScopeCountEntries) {
            auto oldest = source->display_scope_counts.end();
            for (auto it = source->display_scope_counts.begin();
                 it != source->display_scope_counts.end(); ++it) {
                if (it->second.pending) continue;
                if (oldest == source->display_scope_counts.end() ||
                    it->second.last_access < oldest->second.last_access) {
                    oldest = it;
                }
            }
            if (oldest != source->display_scope_counts.end()) {
                source->display_scope_counts.erase(oldest);
            }
        }
        if (source->display_scope_counts.size() >= kMaximumDisplayScopeCountEntries) return;
        ProjectionDisplayScopeCountEntry entry;
        entry.pending = true;
        entry.last_access = ++source->display_scope_count_access_serial;
        existing = source->display_scope_counts.emplace(request.key, entry).first;
    }
    while (source->display_scope_count_requests.size() >= kMaximumQueuedDisplayScopeCounts) {
        const ProjectionDisplayScopeCountRequest dropped =
            source->display_scope_count_requests.front();
        source->display_scope_count_requests.pop_front();
        const auto dropped_entry = source->display_scope_counts.find(dropped.key);
        if (dropped_entry != source->display_scope_counts.end()) {
            dropped_entry->second.pending = false;
        }
    }
    source->display_scope_count_requests.push_back(request);
    source->display_scope_count_cv.notify_one();
}

bool lookupDisplayScopeCount(
        const std::shared_ptr<BuildProjectionRuntime::LazyProjectionSource>& source,
        const ProjectionDisplayScopeCountKey& key, uint64_t* count) {
    if (!source || !count) return false;
    std::lock_guard<std::mutex> lock(source->display_scope_count_mutex);
    const auto existing = source->display_scope_counts.find(key);
    if (existing == source->display_scope_counts.end() || !existing->second.known) return false;
    existing->second.last_access = ++source->display_scope_count_access_serial;
    *count = existing->second.block_count;
    return true;
}

}  // namespace

BuildProjectionRuntime& BuildProjectionRuntime::instance() {
    static BuildProjectionRuntime runtime;
    return runtime;
}

BuildProjectionRuntime::~BuildProjectionRuntime() {
    generation_.fetch_add(1, std::memory_order_acq_rel);
    stopLazySurfaceWorker();
    clearPrinterSource();
}

void BuildProjectionRuntime::stopLazySurfaceWorker() noexcept {
    std::thread worker;
    std::shared_ptr<LazyProjectionSource> source;
    {
        std::lock_guard<std::mutex> lock(lazy_worker_mutex_);
        source = std::move(lazy_source_);
        if (source) source->stop_requested.store(true, std::memory_order_release);
        worker = std::move(lazy_worker_);
    }
    if (worker.joinable()) {
        try {
            worker.join();
        } catch (...) {
            // Joining a standard thread should not throw in this state.  Keep
            // the process safe if an unusual runtime reports an error instead.
            try {
                worker.detach();
            } catch (...) {
            }
        }
    }
}

void BuildProjectionRuntime::clearPrinterSource() noexcept {
    std::thread target_worker;
    std::thread scope_worker;
    std::thread material_summary_worker;
    std::shared_ptr<LazyProjectionSource> source;
    {
        std::lock_guard<std::mutex> lock(printer_source_mutex_);
        source = std::move(printer_source_);
        if (source) {
            // Publish unavailability before releasing the raw spool.  A query
            // which already has a shared_ptr rechecks this state while holding
            // printer_io_mutex, so it cannot start a new source read below.
            source->printer_active.store(false, std::memory_order_release);
            source->printer_target_stop.store(true, std::memory_order_release);
            source->display_scope_count_stop.store(true, std::memory_order_release);
            source->material_summary_stop.store(true, std::memory_order_release);
            source->printer_target_request_cv.notify_all();
            source->display_scope_count_cv.notify_all();
        }
        target_worker = std::move(printer_target_worker_);
        scope_worker = std::move(printer_scope_worker_);
        material_summary_worker = std::move(printer_material_summary_worker_);
    }
    const auto join_worker = [](std::thread* worker) {
        if (!worker || !worker->joinable()) return;
        try {
            if (worker->get_id() == std::this_thread::get_id()) {
                // This is not expected in normal lifecycle paths, but avoids
                // terminating if a future worker error handler clears itself.
                worker->detach();
            } else {
                worker->join();
            }
        } catch (...) {
            try {
                worker->detach();
            } catch (...) {
            }
        }
    };
    join_worker(&target_worker);
    join_worker(&scope_worker);
    join_worker(&material_summary_worker);
    if (source) {
        // Wait out an in-flight nearby query before this function permits the
        // last source owner to remove its backing spool.
        try {
            std::lock_guard<std::mutex> lock(source->printer_io_mutex);
            source->printer_partition_cache.clear();
        } catch (...) {
        }
    }
}

void BuildProjectionRuntime::publishPrinterSource(
        std::shared_ptr<LazyProjectionSource> source) {
    if (!source) {
        throw std::runtime_error("projection printer source is missing");
    }
    std::lock_guard<std::mutex> lock(printer_source_mutex_);
    if (printer_source_ || printer_target_worker_.joinable() ||
        printer_scope_worker_.joinable() || printer_material_summary_worker_.joinable()) {
        throw std::runtime_error("previous projection printer source is still active");
    }
    source->printer_active.store(true, std::memory_order_release);
    source->printer_target_stop.store(false, std::memory_order_release);
    source->display_scope_count_stop.store(false, std::memory_order_release);
    source->material_summary_stop.store(false, std::memory_order_release);
    printer_source_ = source;
    try {
        printer_target_worker_ = std::thread(&BuildProjectionRuntime::runPrinterTargetWorker,
                                              this, source);
        printer_scope_worker_ = std::thread(&BuildProjectionRuntime::runPrinterScopeWorker,
                                             this, std::move(source));
        printer_material_summary_worker_ =
            std::thread(&BuildProjectionRuntime::runPrinterMaterialSummaryWorker,
                        this, printer_source_);
    } catch (...) {
        printer_source_->printer_active.store(false, std::memory_order_release);
        printer_source_->printer_target_stop.store(true, std::memory_order_release);
        printer_source_->display_scope_count_stop.store(true, std::memory_order_release);
        printer_source_->material_summary_stop.store(true, std::memory_order_release);
        printer_source_->printer_target_request_cv.notify_all();
        printer_source_->display_scope_count_cv.notify_all();
        std::thread target_worker = std::move(printer_target_worker_);
        std::thread scope_worker = std::move(printer_scope_worker_);
        std::thread material_summary_worker = std::move(printer_material_summary_worker_);
        printer_source_.reset();
        try {
            if (target_worker.joinable()) target_worker.join();
        } catch (...) {
            try {
                target_worker.detach();
            } catch (...) {
            }
        }
        try {
            if (scope_worker.joinable()) scope_worker.join();
        } catch (...) {
            try {
                scope_worker.detach();
            } catch (...) {
            }
        }
        try {
            if (material_summary_worker.joinable()) material_summary_worker.join();
        } catch (...) {
            try {
                material_summary_worker.detach();
            } catch (...) {
            }
        }
        throw;
    }
}

void BuildProjectionRuntime::runPrinterTargetWorker(
        std::shared_ptr<LazyProjectionSource> source) noexcept {
    if (!source) return;
    try {
        while (projectionPrinterSourceActive(source) &&
               !source->printer_target_stop.load(std::memory_order_acquire)) {
            PartitionCoord coord;
            bool have_request = false;
            {
                std::unique_lock<std::mutex> lock(source->printer_target_request_mutex);
                source->printer_target_request_cv.wait(lock, [&]() {
                    return source->printer_target_stop.load(std::memory_order_acquire) ||
                        !source->printer_target_requests.empty();
                });
                if (source->printer_target_stop.load(std::memory_order_acquire)) return;
                if (!source->printer_target_requests.empty()) {
                    coord = source->printer_target_requests.front();
                    source->printer_target_requests.pop_front();
                    source->printer_target_pending.erase(coord);
                    have_request = true;
                }
            }
            if (!have_request) continue;
            std::string detail;
            if (!loadPrinterPartition(source, coord, &detail) &&
                projectionPrinterSourceActive(source) &&
                !source->printer_target_stop.load(std::memory_order_acquire)) {
                source->printer_target_load_failed.store(true, std::memory_order_release);
                std::lock_guard<std::mutex> lock(source->printer_target_request_mutex);
                source->printer_target_failed.emplace(coord);
            }
        }
    } catch (...) {
        source->printer_target_load_failed.store(true, std::memory_order_release);
    }
}

void BuildProjectionRuntime::runPrinterScopeWorker(
        std::shared_ptr<LazyProjectionSource> source) noexcept {
    if (!source) return;
    try {
        while (projectionPrinterSourceActive(source) &&
               !source->display_scope_count_stop.load(std::memory_order_acquire)) {
            ProjectionDisplayScopeCountRequest request;
            bool have_request = false;
            {
                std::unique_lock<std::mutex> lock(source->display_scope_count_mutex);
                source->display_scope_count_cv.wait(lock, [&]() {
                    return source->display_scope_count_stop.load(std::memory_order_acquire) ||
                        !source->display_scope_count_requests.empty();
                });
                if (source->display_scope_count_stop.load(std::memory_order_acquire)) {
                    return;
                }
                if (!source->display_scope_count_requests.empty()) {
                    request = source->display_scope_count_requests.front();
                    source->display_scope_count_requests.pop_front();
                    have_request = true;
                }
            }
            if (!have_request) continue;

            uint64_t count = 0;
            std::string detail;
            const bool counted = countDisplayScopeBlocks(source, request, &count, &detail);
            std::lock_guard<std::mutex> lock(source->display_scope_count_mutex);
            const auto entry = source->display_scope_counts.find(request.key);
            if (entry == source->display_scope_counts.end()) continue;
            entry->second.pending = false;
            entry->second.last_access = ++source->display_scope_count_access_serial;
            if (counted && projectionPrinterSourceActive(source) &&
                !source->display_scope_count_stop.load(std::memory_order_acquire)) {
                entry->second.block_count = count;
                entry->second.known = true;
            } else if (!source->display_scope_count_stop.load(std::memory_order_acquire)) {
                entry->second.failed = true;
            }
        }
    } catch (...) {
        // The cache is advisory only.  Keep source queries usable if a
        // background allocation or I/O implementation reports an exception.
    }
}

void BuildProjectionRuntime::runPrinterMaterialSummaryWorker(
        std::shared_ptr<LazyProjectionSource> source) noexcept {
    if (!source) return;
    try {
        std::vector<ProjectionMaterialSummaryEntry> entries;
        uint64_t total_block_count = 0;
        std::string detail;
        const bool built = buildProjectionMaterialSummary(
            source, &entries, &total_block_count, &detail);

        // clearPrinterSource() publishes stop/unavailability before joining
        // this worker. Do not publish a result into a source that was retired
        // while its file stream was being read.
        if (!projectionPrinterSourceActive(source) ||
            source->material_summary_stop.load(std::memory_order_acquire)) {
            return;
        }
        std::lock_guard<std::mutex> lock(source->material_summary_mutex);
        if (!projectionPrinterSourceActive(source) ||
            source->material_summary_stop.load(std::memory_order_acquire)) {
            return;
        }
        if (built) {
            source->material_summary_entries = std::move(entries);
            source->material_summary_block_count = total_block_count;
            source->material_summary_ready = true;
            source->material_summary_failed = false;
        } else {
            // A bad preview cache must never take down a working projection or
            // printer. Queries expose SourceUnavailable until the next load.
            source->material_summary_entries.clear();
            source->material_summary_block_count = 0;
            source->material_summary_ready = false;
            source->material_summary_failed = true;
        }
    } catch (...) {
        if (!projectionPrinterSourceActive(source) ||
            source->material_summary_stop.load(std::memory_order_acquire)) {
            return;
        }
        try {
            std::lock_guard<std::mutex> lock(source->material_summary_mutex);
            if (projectionPrinterSourceActive(source) &&
                !source->material_summary_stop.load(std::memory_order_acquire)) {
                source->material_summary_entries.clear();
                source->material_summary_block_count = 0;
                source->material_summary_ready = false;
                source->material_summary_failed = true;
            }
        } catch (...) {
        }
    }
}

void BuildProjectionRuntime::startLazySurfaceWorker(
        std::shared_ptr<LazyProjectionSource> source) {
    if (!source) return;
    std::lock_guard<std::mutex> lock(lazy_worker_mutex_);
    if (lazy_worker_.joinable()) {
        throw std::runtime_error("previous lazy projection worker is still running");
    }
    source->stop_requested.store(false, std::memory_order_release);
    lazy_source_ = source;
    try {
        lazy_worker_ = std::thread(&BuildProjectionRuntime::runLazySurfaceWorker,
                                   this, std::move(source));
    } catch (...) {
        lazy_source_.reset();
        throw;
    }
}

void BuildProjectionRuntime::runLazySurfaceWorker(
        std::shared_ptr<LazyProjectionSource> source) noexcept {
    if (!source) return;
    const auto cancelled = [&]() {
        return source->stop_requested.load(std::memory_order_acquire) ||
            source->generation != generation_.load(std::memory_order_acquire);
    };
    const auto detachCompletedSource = [&]() {
        std::lock_guard<std::mutex> lock(lazy_worker_mutex_);
        if (lazy_source_.get() == source.get()) lazy_source_.reset();
    };
    const auto fail = [&](std::string detail) {
        if (cancelled()) return;
        BuildProjectionRenderer::instance().clearPlan();
        clearPrinterSource();
        source->releaseRawSpool();
        try {
            publishStatus(source->generation, BuildProjectionState::Failed,
                          "projection nearby-surface streaming failed: " + detail,
                          0, source->request.source_path);
        } catch (...) {
        }
        detachCompletedSource();
    };

    try {
        BuildProjectionRenderer& renderer = BuildProjectionRenderer::instance();
        std::array<char, kSpoolReadBufferSize> stream_buffer{};
        std::ifstream stream;
        bool stream_open = false;
        int32_t cached_radius = -1;
        std::vector<std::pair<int32_t, int32_t>> column_offsets;
        uint64_t last_status_records = 0;

        const auto columnKey = [](int32_t x, int32_t z) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32U) |
                static_cast<uint32_t>(z);
        };
        const auto blockCoordinate = [](float coordinate, int32_t* output) {
            if (!output || !std::isfinite(coordinate)) return false;
            const double value = std::floor(static_cast<double>(coordinate));
            if (value < static_cast<double>(INT32_MIN) ||
                value > static_cast<double>(INT32_MAX)) {
                return false;
            }
            *output = static_cast<int32_t>(value);
            return true;
        };
        const auto targetPartitionY = [&](int32_t camera_y) {
            const ProjectionLayerFilter filter = renderer.layerFilter();
            int64_t lower = filter.minimum_y;
            int64_t upper = filter.maximum_y;
            if (filter.mode == ProjectionLayerMode::All) {
                return floorDiv(camera_y, kProjectionPartitionSize);
            }
            if (filter.mode == ProjectionLayerMode::AtOrAbove) {
                upper = INT32_MAX;
            } else if (filter.mode == ProjectionLayerMode::AtOrBelow) {
                lower = INT32_MIN;
            } else if (filter.mode == ProjectionLayerMode::Single) {
                upper = lower;
            } else if (filter.mode == ProjectionLayerMode::Range && lower > upper) {
                std::swap(lower, upper);
            }
            if (filter.space == ProjectionLayerSpace::Relative) {
                if (lower != INT32_MIN) lower += source->relative_origin_y;
                if (upper != INT32_MAX) upper += source->relative_origin_y;
            }
            int64_t target = camera_y;
            if (target < lower) target = lower;
            if (target > upper) target = upper;
            target = std::max<int64_t>(INT32_MIN,
                                       std::min<int64_t>(INT32_MAX, target));
            return floorDiv(static_cast<int32_t>(target), kProjectionPartitionSize);
        };
        const auto selectNearbyPartition = [&](int32_t camera_x, int32_t camera_y,
                                                int32_t camera_z,
                                                PartitionCoord* output) {
            if (!output) return false;
            const int32_t range_chunks = renderer.rangeChunks();
            // Render groups are 16 blocks but raw source partitions are 32.
            // The additional ring prevents a visible edge from waiting until
            // the player crosses a partition boundary.
            const int32_t radius = std::max(
                1, (std::max(1, range_chunks) + 2) / 2 + 1);
            if (radius != cached_radius) {
                column_offsets.clear();
                const size_t diameter = static_cast<size_t>(radius) * 2U + 1U;
                column_offsets.reserve(diameter * diameter);
                for (int32_t z = -radius; z <= radius; ++z) {
                    for (int32_t x = -radius; x <= radius; ++x) {
                        if (static_cast<int64_t>(x) * x +
                                static_cast<int64_t>(z) * z >
                            static_cast<int64_t>(radius) * radius) {
                            continue;
                        }
                        column_offsets.emplace_back(x, z);
                    }
                }
                std::sort(column_offsets.begin(), column_offsets.end(),
                          [](const auto& left, const auto& right) {
                              const int64_t left_distance =
                                  static_cast<int64_t>(left.first) * left.first +
                                  static_cast<int64_t>(left.second) * left.second;
                              const int64_t right_distance =
                                  static_cast<int64_t>(right.first) * right.first +
                                  static_cast<int64_t>(right.second) * right.second;
                              if (left_distance != right_distance) {
                                  return left_distance < right_distance;
                              }
                              return left.second != right.second
                                  ? left.second < right.second : left.first < right.first;
                          });
                cached_radius = radius;
            }

            const int32_t partition_x = floorDiv(camera_x, kProjectionPartitionSize);
            const int32_t partition_y = targetPartitionY(camera_y);
            const int32_t partition_z = floorDiv(camera_z, kProjectionPartitionSize);
            bool found = false;
            int64_t best_score = std::numeric_limits<int64_t>::max();
            PartitionCoord best{};
            for (const auto& offset : column_offsets) {
                const int64_t column_x64 = static_cast<int64_t>(partition_x) + offset.first;
                const int64_t column_z64 = static_cast<int64_t>(partition_z) + offset.second;
                if (column_x64 < INT32_MIN || column_x64 > INT32_MAX ||
                    column_z64 < INT32_MIN || column_z64 > INT32_MAX) {
                    continue;
                }
                const uint64_t key = columnKey(static_cast<int32_t>(column_x64),
                                               static_cast<int32_t>(column_z64));
                const auto column = source->columns.find(key);
                if (column == source->columns.end() || column->second.empty()) continue;
                const auto built_count = source->built_partitions_per_column.find(key);
                if (built_count != source->built_partitions_per_column.end() &&
                    built_count->second >= column->second.size()) {
                    continue;
                }
                const std::vector<PartitionCoord>& candidates = column->second;
                auto right = std::lower_bound(
                    candidates.begin(), candidates.end(), partition_y,
                    [](const PartitionCoord& coord, int32_t y) { return coord.y < y; });
                auto left = right;
                while (left != candidates.begin() || right != candidates.end()) {
                    const PartitionCoord* candidate = nullptr;
                    if (left == candidates.begin()) {
                        candidate = &*right++;
                    } else if (right == candidates.end()) {
                        candidate = &*--left;
                    } else {
                        const PartitionCoord& lower = *(left - 1);
                        const PartitionCoord& upper = *right;
                        const int64_t lower_distance = static_cast<int64_t>(partition_y) -
                            lower.y;
                        const int64_t upper_distance = static_cast<int64_t>(upper.y) -
                            partition_y;
                        if (lower_distance <= upper_distance) {
                            candidate = &*--left;
                        } else {
                            candidate = &*right++;
                        }
                    }
                    if (source->built_partitions.find(*candidate) !=
                        source->built_partitions.end()) {
                        continue;
                    }
                    const int64_t horizontal_distance =
                        static_cast<int64_t>(offset.first) * offset.first +
                        static_cast<int64_t>(offset.second) * offset.second;
                    const int64_t vertical_distance = std::llabs(
                        static_cast<int64_t>(candidate->y) - partition_y);
                    // Favor a contiguous horizontal area, while still pulling
                    // nearby vertical floors in before distant columns.
                    const int64_t score = horizontal_distance * 8 + vertical_distance;
                    if (!found || score < best_score ||
                        (score == best_score && candidate->y < best.y)) {
                        found = true;
                        best_score = score;
                        best = *candidate;
                    }
                    break;
                }
            }
            if (!found) return false;
            *output = best;
            return true;
        };

        while (!cancelled()) {
            const std::string renderer_error =
                renderer.lazyBuildError(source->plan_identity);
            if (!renderer_error.empty()) {
                if (!cancelled()) fail(renderer_error);
                return;
            }

            float camera_x = 0.f;
            float camera_y = 0.f;
            float camera_z = 0.f;
            int32_t block_x = 0;
            int32_t block_y = 0;
            int32_t block_z = 0;
            if (!GetLatestBuildRenderCameraPosition(&camera_x, &camera_y, &camera_z) ||
                !blockCoordinate(camera_x, &block_x) ||
                !blockCoordinate(camera_y, &block_y) ||
                !blockCoordinate(camera_z, &block_z)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            PartitionCoord coord;
            if (!selectNearbyPartition(block_x, block_y, block_z, &coord)) {
                if (source->built_partitions.size() >= source->total_core_partitions) {
                    if (source->queued_core_records != source->source_block_count) {
                        fail("projection partition count does not match the parsed blueprint");
                        return;
                    }
                    if (stream_open) stream.close();
                    // The renderer is finished with raw records here, but the
                    // printer owns an independent immutable source for the
                    // full projection lifetime.
                    publishStatus(source->generation, BuildProjectionState::Ready,
                                  source->ready_status +
                                      "; nearby surface extraction queued",
                                  source->source_block_count, source->request.source_path);
                    detachCompletedSource();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            const auto partition_info = source->partitions.find(coord);
            if (partition_info == source->partitions.end() ||
                partition_info->second.core_records == 0) {
                fail("projection nearby partition index is inconsistent");
                return;
            }
            if (!stream_open) {
                stream.rdbuf()->pubsetbuf(stream_buffer.data(),
                                           static_cast<std::streamsize>(stream_buffer.size()));
                stream.open(source->raw_spool_path, std::ios::binary);
                if (!stream) {
                    fail("cannot open projection source partition storage");
                    return;
                }
                stream_open = true;
            }

            ProjectionPartitionInput partition;
            uint64_t core_records = 0;
            size_t core_cells = 0;
            std::string detail;
            const uint64_t remaining_records =
                source->queued_core_records <= source->source_block_count
                ? source->source_block_count - source->queued_core_records : 0;
            if (remaining_records == 0 || !readPartitionSpool(
                    &stream, source->raw_spool_size, coord, partition_info->second,
                    source->request, source->material_name_views, cancelled, &partition,
                    remaining_records, &core_records, &core_cells, &detail)) {
                if (!cancelled()) {
                    fail(detail.empty() ? "cannot read nearby projection partition" : detail);
                }
                return;
            }
            if (core_cells != 0 && !renderer.enqueueLazyPartition(
                    source->plan_identity, std::move(partition), cancelled, &detail)) {
                if (!cancelled()) {
                    fail(detail.empty() ? "cannot queue nearby projection surfaces" : detail);
                }
                return;
            }
            if (core_records == 0 || core_records >
                source->source_block_count - source->queued_core_records ||
                !source->built_partitions.insert(coord).second) {
                fail("projection nearby partition accounting is inconsistent");
                return;
            }
            source->queued_core_records += core_records;
            ++source->built_partitions_per_column[columnKey(coord.x, coord.z)];
            if (source->queued_core_records == source->source_block_count ||
                source->queued_core_records - last_status_records >= kStatusBlockInterval) {
                const uint64_t percent = static_cast<uint64_t>(
                    static_cast<long double>(source->queued_core_records) * 100.0L /
                    source->source_block_count);
                publishStatus(source->generation, BuildProjectionState::Ready,
                              source->ready_status + "; streaming nearby surfaces (" +
                                  std::to_string(std::min<uint64_t>(100, percent)) + "%)",
                              source->source_block_count, source->request.source_path);
                last_status_records = source->queued_core_records;
            }
        }
    } catch (const std::bad_alloc&) {
        fail("not enough memory for nearby projection surfaces");
    } catch (...) {
        fail("unexpected nearby projection surface streaming error");
    }
    detachCompletedSource();
}

bool BuildProjectionRuntime::load(BuildProjectionLoadRequest request, std::string* error) {
    if (error) error->clear();
    const uint64_t generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::unique_lock<std::mutex> operation_lock(operation_mutex_);
    // Stop and join the prior source-reader before its raw partition spool is
    // removed or a new plan takes its place.  Doing this after acquiring the
    // operation lock also closes the narrow race where an older load publishes
    // a worker just as a newer request is queued.
    stopLazySurfaceWorker();
    clearPrinterSource();
    const auto cancelled = [&]() {
        return generation != generation_.load(std::memory_order_acquire);
    };
    try {
    if (cancelled()) {
        if (error) *error = "projection load superseded";
        return false;
    }
    // A load attempt replaces the current standalone projection. Do not begin
    // the next parser/texture lifecycle until the world render thread has
    // released the prior plan's GLES objects; this avoids a second load
    // overlapping a retired plan inside the injected game renderer.
    BuildProjectionRenderer& renderer = BuildProjectionRenderer::instance();
    if (!renderer.clearPlanAndWaitForRender(kProjectionRetirementWaitMs)) {
        const std::string detail =
            "previous projection is still being released; return to the loaded world and retry";
        publishStatus(generation, BuildProjectionState::Failed, detail);
        if (error) *error = detail;
        return false;
    }
    cleanupAbandonedProjectionSpoolDirectories(request.work_directory);

    ProjectionSourceType source_type;
    if (request.source_path.empty() || request.work_directory.empty()) {
        const std::string detail = "projection source and work directory are required";
        publishStatus(generation, BuildProjectionState::Failed, detail);
        if (error) *error = detail;
        return false;
    }
    if (!sourceTypeForPath(request.source_path, &source_type)) {
        const std::string detail =
            "unsupported projection file; use .infinity, .schem, .schematic, .litematic, .bdx, .mcworld, or .png";
        publishStatus(generation, BuildProjectionState::Failed, detail);
        if (error) *error = detail;
        return false;
    }
    if (request.rotation_degrees != 0 && request.rotation_degrees != 90 &&
        request.rotation_degrees != 180 && request.rotation_degrees != 270) {
        const std::string detail = "projection rotation must be 0, 90, 180, or 270 degrees";
        publishStatus(generation, BuildProjectionState::Failed, detail);
        if (error) *error = detail;
        return false;
    }
    if (source_type == ProjectionSourceType::PixelArtPng && request.pixel_art_width < 1) {
        const std::string detail = "pixel-art projection width must be positive";
        publishStatus(generation, BuildProjectionState::Failed, detail);
        if (error) *error = detail;
        return false;
    }

    publishStatus(generation, BuildProjectionState::Loading,
                  std::string("preparing ") + sourceLabel(source_type) + " projection",
                  0, request.source_path);

    std::string spool_directory;
    std::string detail;
    if (!createUniqueSpoolDirectory(request.work_directory, generation,
                                    &spool_directory, &detail)) {
        publishStatus(generation, BuildProjectionState::Failed, detail);
        if (error) *error = detail;
        return false;
    }
    ScopedSpoolDirectory spool_cleanup(spool_directory);

    PartitionSpoolWriter partition_writer(spool_directory);
    uint64_t partitioned_block_count = 0;
    int32_t relative_origin_y = request.base_y;
    bool found_imported_bounds = false;
    std::vector<ProjectionMaterialRequest> material_requests;
    std::unordered_map<std::string, uint16_t> material_ids;
    std::string last_material_name;
    uint8_t last_material_aux = 0;
    uint8_t last_material_rotation = 0;
    bool has_last_material = false;
    const auto registerMaterial = [&](const std::string& name, uint8_t aux,
                                      std::string* sink_error) {
        const uint8_t rotation = static_cast<uint8_t>(request.rotation_degrees / 90);
        if (has_last_material && last_material_aux == aux &&
            last_material_rotation == rotation && last_material_name == name) {
            return true;
        }
        std::string key;
        key.reserve(name.size() + 3U);
        key.append(name);
        key.push_back('\0');
        key.push_back(static_cast<char>(aux));
        key.push_back(static_cast<char>(rotation));
        const auto existing = material_ids.find(key);
        if (existing == material_ids.end()) {
            if (material_requests.size() >= static_cast<size_t>(UINT16_MAX)) {
                if (sink_error) {
                    *sink_error = "projection material combinations exceed the supported range";
                }
                return false;
            }
            const uint16_t material_id = static_cast<uint16_t>(
                material_requests.size() + 1U);
            material_requests.push_back({material_id, name, aux, rotation});
            material_ids.emplace(std::move(key), material_id);
        }
        last_material_name = name;
        last_material_aux = aux;
        last_material_rotation = rotation;
        has_last_material = true;
        return true;
    };
    const ParsedBlockSink block_sink = [&](const ParsedBlock& block,
                                            std::string* sink_error) {
        if (cancelled()) {
            if (sink_error) *sink_error = "projection load cancelled";
            return false;
        }
        const uint8_t phase = static_cast<uint8_t>(block.spec.phase);
        if (phase <= static_cast<uint8_t>(ImportPhase::Clear) ||
            phase >= static_cast<uint8_t>(ImportPhase::Count) ||
            block.spec.command_name.empty() ||
            block.spec.command_name.size() > UINT16_MAX) {
            if (sink_error) *sink_error = "projection parser emitted an invalid block";
            return false;
        }
        // Projection geometry predates the importer's 16-bit native Bedrock
        // state support and deliberately uses only the visual low byte for
        // rendering.  Keep the complete value in the raw partition record so
        // the printer can match a real inventory item without losing BDX
        // state variants.
        const uint8_t visual_aux = static_cast<uint8_t>(block.spec.aux & 0xFFU);
        if (!registerMaterial(block.spec.command_name, visual_aux, sink_error)) {
            return false;
        }
        if (partitioned_block_count == std::numeric_limits<uint64_t>::max()) {
            if (sink_error) *sink_error = "projection block count exceeds the supported range";
            return false;
        }

        DiskRecord record{
            block.world_x,
            block.world_y,
            block.world_z,
            block.spec.aux,
            static_cast<uint8_t>(
                kProjectionRecordFlag |
                (block.spec.can_fill ? kProjectionCanFillFlag : 0x00U) |
                (block.spec.single_layer_only ? kProjectionSingleLayerFlag : 0x00U) |
                (block.spec.stateful ? kProjectionStatefulFlag : 0x00U)),
            static_cast<uint8_t>(block.spec.phase),
            0,
            0,
        };
        int32_t rotated_x = 0;
        int32_t rotated_z = 0;
        if (!rotatePosition(record, request, &rotated_x, &rotated_z)) {
            if (sink_error) {
                *sink_error = "rotated projection exceeds the supported coordinate range";
            }
            return false;
        }
        record.x = rotated_x;
        record.z = rotated_z;
        if (!appendPartitionRecordWithHalo(
                &partition_writer, record, block.spec.command_name, sink_error)) {
            return false;
        }
        if (!found_imported_bounds || block.world_y < relative_origin_y) {
            relative_origin_y = block.world_y;
            found_imported_bounds = true;
        }
        ++partitioned_block_count;
        return true;
    };

    SchematicParseResult result;
    const auto progress_callback = [&](const SchematicParseProgress& progress) {
        if (!cancelled()) {
            publishStatus(generation, BuildProjectionState::Loading,
                          progressText(source_type, progress), 0, request.source_path);
        }
    };

    bool parsed = false;
    try {
        if (source_type == ProjectionSourceType::PixelArtPng) {
            PixelArtParseOptions options;
            options.source_path = request.source_path;
            options.spool_directory = spool_directory;
            options.base_x = request.base_x;
            options.base_y = request.base_y;
            options.base_z = request.base_z;
            options.target_width = request.pixel_art_width;
            options.chunk_size = kProjectionChunkSize;
            options.maximum_output_blocks = 0;
            options.include_source_volume = false;
            options.block_sink = block_sink;
            options.cancellation_requested = cancelled;
            options.progress_callback = progress_callback;
            PixelArtParser parser;
            parsed = parser.parse(options, &result, &detail);
        } else {
            SchematicParseOptions options;
            options.source_path = request.source_path;
            options.spool_directory = spool_directory;
            options.base_x = request.base_x;
            options.base_y = request.base_y;
            options.base_z = request.base_z;
            options.chunk_size = kProjectionChunkSize;
            options.maximum_output_blocks = 0;
            options.maximum_chunk_descriptors = 0;
            options.include_source_volume = false;
            options.block_sink = block_sink;
            options.cancellation_requested = cancelled;
            options.progress_callback = progress_callback;
            BlockMapper mapper;
            options.legacy_block_resolver = [&mapper](uint16_t id, uint8_t data) {
                return projectionLegacyMapping(mapper, id, data);
            };
            options.state_block_resolver = [](std::string_view state) {
                return projectionStateMapping(state);
            };
            if (source_type == ProjectionSourceType::Litematic) {
                LitematicParser parser;
                parsed = parser.parse(options, &result, &detail);
            } else if (source_type == ProjectionSourceType::Bdx) {
                // BDX uses its own Brotli-compressed command stream, but emits
                // the same streamed ParsedBlock records as the other formats.
                // This keeps large BDX projections partitioned and independent
                // from the import command pipeline.
                BdxParser parser;
                parsed = parser.parse(options, &result, &detail);
            } else if (source_type == ProjectionSourceType::Mcworld) {
                McworldParser parser;
                // The no-mapper overload follows the same projection contract as
                // Litematic: it consumes the source-state resolver above so the
                // renderer can retain valid Bedrock identities which have no
                // command-version mapping. Import uses the mapper overload.
                parsed = parser.parse(options, &result, &detail);
            } else if (source_type == ProjectionSourceType::InfiniteczBuild) {
                InfiniteczBuildParser parser;
                parsed = parser.parse(options, mapper, &result, &detail);
            } else {
                SchematicParser parser;
                parsed = parser.parse(options, &result, &detail);
            }
        }
    } catch (const std::bad_alloc&) {
        detail = "projection parsing needs more memory than available";
    } catch (...) {
        detail = "unexpected failure while parsing projection source";
    }

    if (cancelled()) {
        if (error) *error = "projection load cancelled";
        return false;
    }
    if (!parsed) {
        if (detail.empty()) detail = "cannot parse projection source";
        publishStatus(generation, BuildProjectionState::Failed, detail);
        if (error) *error = detail;
        return false;
    }
    if (result.imported_block_count == 0) {
        detail = "projection contains no visible blocks";
        publishStatus(generation, BuildProjectionState::Failed, detail);
        if (error) *error = detail;
        return false;
    }
    if (!found_imported_bounds ||
        partitioned_block_count != result.imported_block_count) {
        detail = "projection partition count does not match the parsed blueprint";
        publishStatus(generation, BuildProjectionState::Failed, detail);
        if (error) *error = detail;
        return false;
    }
    publishStatus(generation, BuildProjectionState::Loading,
                  "finalizing projection partitions", 0, request.source_path);
    if (!partition_writer.finish(&detail)) {
        publishStatus(generation, BuildProjectionState::Failed, detail);
        if (error) *error = detail;
        return false;
    }
    if (cancelled()) {
        if (error) *error = "projection load cancelled";
        return false;
    }

    publishStatus(generation, BuildProjectionState::Loading,
                  "indexing nearby projection surfaces", 0, request.source_path);
    std::shared_ptr<LazyProjectionSource> lazy_source;
    try {
        if (material_requests.empty()) {
            detail = "projection contains no renderable materials";
            publishStatus(generation, BuildProjectionState::Failed, detail);
            if (error) *error = detail;
            return false;
        }
        lazy_source = std::make_shared<LazyProjectionSource>();
        lazy_source->generation = generation;
        lazy_source->source_block_count = result.imported_block_count;
        lazy_source->relative_origin_y = relative_origin_y;
        lazy_source->raw_spool_path = partition_writer.containerPath();
        lazy_source->request = request;
        lazy_source->partitions = partition_writer.partitions();
        lazy_source->material_names.reserve(partition_writer.materialNames().size());
        for (std::string_view name : partition_writer.materialNames()) {
            lazy_source->material_names.emplace_back(name);
        }
        lazy_source->material_name_views.reserve(lazy_source->material_names.size());
        for (const std::string& name : lazy_source->material_names) {
            lazy_source->material_name_views.emplace_back(name);
        }
        const auto partitionColumnKey = [](int32_t x, int32_t z) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32U) |
                static_cast<uint32_t>(z);
        };
        lazy_source->columns.reserve(lazy_source->partitions.size());
        for (const auto& entry : lazy_source->partitions) {
            if (entry.second.core_records == 0) continue;
            lazy_source->columns[partitionColumnKey(entry.first.x, entry.first.z)]
                .push_back(entry.first);
            ++lazy_source->total_core_partitions;
        }
        if (lazy_source->total_core_partitions == 0) {
            detail = "projection contains no source partitions";
            publishStatus(generation, BuildProjectionState::Failed, detail);
            if (error) *error = detail;
            return false;
        }
        for (auto& entry : lazy_source->columns) {
            std::sort(entry.second.begin(), entry.second.end(),
                      [](const PartitionCoord& left, const PartitionCoord& right) {
                          return left.y != right.y ? left.y < right.y :
                              (left.x != right.x ? left.x < right.x : left.z < right.z);
                      });
        }
        std::ifstream size_stream(lazy_source->raw_spool_path,
                                  std::ios::binary | std::ios::ate);
        if (!size_stream || size_stream.tellg() <= 0) {
            detail = "projection partition storage is empty";
            publishStatus(generation, BuildProjectionState::Failed, detail);
            if (error) *error = detail;
            return false;
        }
        lazy_source->raw_spool_size = static_cast<uint64_t>(size_stream.tellg());
        size_stream.close();

        ProjectionStreamingOptions streaming_options;
        streaming_options.source_block_count = result.imported_block_count;
        streaming_options.relative_origin_y = relative_origin_y;
        streaming_options.cache_directory = spool_directory;
        // Zero removes a logical total-size ceiling. Disk exhaustion is still
        // reported by the renderer while the raw source and pending handoff
        // queues remain separately bounded.
        streaming_options.maximum_spool_bytes = 0;
        streaming_options.material_requests = std::move(material_requests);
        streaming_options.lazy_surface_streaming = true;
        if (!renderer.beginStreamingPlan(std::move(streaming_options), &detail) ||
            !renderer.commitStreamingPlan(cancelled, &detail)) {
            renderer.abortStreamingPlan();
            if (cancelled()) {
                if (error) *error = "projection load cancelled";
                return false;
            }
            if (detail.empty()) detail = "cannot initialize nearby projection render storage";
            publishStatus(generation, BuildProjectionState::Failed, detail);
            if (error) *error = detail;
            return false;
        }
        lazy_source->plan_identity = renderer.publishedPlanIdentity();
        if (lazy_source->plan_identity == 0) {
            detail = "cannot publish nearby projection render plan";
            lazy_source->releaseRawSpool();
            renderer.clearPlan();
            publishStatus(generation, BuildProjectionState::Failed, detail);
            if (error) *error = detail;
            return false;
        }
        spool_cleanup.release();
    } catch (const std::bad_alloc&) {
        if (lazy_source) lazy_source->releaseRawSpool();
        BuildProjectionRenderer::instance().clearPlan();
        detail = "projection nearby surface index needs more memory than available";
        publishStatus(generation, BuildProjectionState::Failed, detail);
        if (error) *error = detail;
        return false;
    } catch (...) {
        if (lazy_source) lazy_source->releaseRawSpool();
        BuildProjectionRenderer::instance().clearPlan();
        detail = "unexpected failure while indexing nearby projection surfaces";
        publishStatus(generation, BuildProjectionState::Failed, detail);
        if (error) *error = detail;
        return false;
    }
    if (cancelled()) {
        lazy_source->releaseRawSpool();
        BuildProjectionRenderer::instance().clearPlan();
        if (error) *error = "projection load cancelled";
        return false;
    }

    const uint64_t loaded_block_count = result.imported_block_count;
    std::string ready_status = "projection ready: " +
        std::to_string(loaded_block_count) + " blocks";
    if (result.unsupported_block_count != 0) {
        ready_status += "; " + std::to_string(result.unsupported_block_count) +
            " unsupported blocks omitted";
    }
    lazy_source->ready_status = ready_status;
    if (!publishStatus(generation, BuildProjectionState::Ready,
                       ready_status + "; streaming nearby surfaces",
                       loaded_block_count, request.source_path)) {
        lazy_source->releaseRawSpool();
        BuildProjectionRenderer::instance().clearPlan();
        if (error) *error = "projection load superseded";
        return false;
    }
    try {
        // Publish the immutable raw source before starting lazy surface
        // extraction.  The renderer and printer may now consume independent
        // file streams without either one owning/deleting the other's data.
        publishPrinterSource(lazy_source);
        startLazySurfaceWorker(std::move(lazy_source));
    } catch (const std::bad_alloc&) {
        clearPrinterSource();
        BuildProjectionRenderer::instance().clearPlan();
        if (error) *error = "not enough memory to start nearby projection surface worker";
        return false;
    } catch (...) {
        clearPrinterSource();
        BuildProjectionRenderer::instance().clearPlan();
        if (error) *error = "cannot start nearby projection surface worker";
        return false;
    }
    return true;
    } catch (const std::bad_alloc&) {
        BuildProjectionRenderer::instance().clearPlan();
        constexpr const char* detail =
            "projection loading needs more memory than available";
        if (generation == generation_.load(std::memory_order_acquire)) {
            try {
                publishStatus(generation, BuildProjectionState::Failed, detail);
            } catch (...) {
            }
        }
        if (error) {
            try {
                *error = detail;
            } catch (...) {
            }
        }
        return false;
    } catch (...) {
        BuildProjectionRenderer::instance().clearPlan();
        constexpr const char* detail = "unexpected failure while loading projection";
        if (generation == generation_.load(std::memory_order_acquire)) {
            try {
                publishStatus(generation, BuildProjectionState::Failed, detail);
            } catch (...) {
            }
        }
        if (error) {
            try {
                *error = detail;
            } catch (...) {
            }
        }
        return false;
    }
}

void BuildProjectionRuntime::clear() {
    generation_.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    stopLazySurfaceWorker();
    clearPrinterSource();
    // UI clear runs on a background thread and immediately re-enables its
    // controls afterwards.  Wait for the render thread to hand off the old
    // plan before reporting completion, so a rapid next load cannot overlap
    // old group VBOs/textures during a world renderer recreation.
    BuildProjectionRenderer& renderer = BuildProjectionRenderer::instance();
    if (!renderer.clearPlanAndWaitForRender(kProjectionRetirementWaitMs)) {
        // This is a bounded best-effort wait.  The plan is already unpublished
        // and will be retired by the next render frame; load() repeats the
        // same handoff check before creating a new projection.
        std::fprintf(stderr,
                     "BuildProjectionRuntime: clear timed out waiting for render retirement\n");
    }
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    state_ = BuildProjectionState::Idle;
    status_ = "no projection loaded";
    block_count_ = 0;
    source_path_.clear();
}

bool BuildProjectionRuntime::queryNearbyPrinterTargets(
        const ProjectionPrinterQuery& query, ProjectionPrinterQueryResult* result,
        std::string* error) const {
    if (error) error->clear();
    if (!result) {
        if (error) *error = "projection printer query result is required";
        return false;
    }
    *result = ProjectionPrinterQueryResult{};
    constexpr int32_t kMaximumPrinterHorizontalRadius = 16;
    constexpr int32_t kMaximumPrinterVerticalRadius = 16;
    constexpr size_t kMaximumPrinterTargets = 1024U;
    if (query.horizontal_radius_blocks < 0 ||
        query.horizontal_radius_blocks > kMaximumPrinterHorizontalRadius ||
        query.vertical_radius_blocks < 0 ||
        query.vertical_radius_blocks > kMaximumPrinterVerticalRadius ||
        query.maximum_targets == 0 || query.maximum_targets > kMaximumPrinterTargets) {
        result->status = ProjectionPrinterQueryStatus::InvalidQuery;
        if (error) {
            *error = "projection printer query radius must be 0..16 and target limit 1..1024";
        }
        return false;
    }

    std::shared_ptr<LazyProjectionSource> source;
    {
        std::lock_guard<std::mutex> lock(printer_source_mutex_);
        source = printer_source_;
    }
    if (!source) {
        result->status = ProjectionPrinterQueryStatus::NoProjection;
        return true;
    }
    if (!projectionPrinterSourceActive(source)) {
        result->status = ProjectionPrinterQueryStatus::SourceUnavailable;
        if (error) *error = "projection printer source is being released";
        return false;
    }
    if (source->printer_target_load_failed.load(std::memory_order_acquire)) {
        result->status = ProjectionPrinterQueryStatus::SourceUnavailable;
        if (error) *error = "projection printer source could not decode a nearby partition";
        return false;
    }

    result->generation = source->generation;
    result->plan_identity = source->plan_identity;
    const BuildProjectionRenderer& renderer = BuildProjectionRenderer::instance();
    // The renderer's range is centered on its latest Level::_render camera,
    // not necessarily the LocalPlayer (third-person, spectator and free-camera
    // modes may differ). Keep interaction reach based on the real player below,
    // but use the exact render center when deciding which raw projection blocks
    // belong to the user-visible range. A camera-less first game tick safely
    // falls back to the player position.
    int32_t display_center_x = query.player_x;
    int32_t display_center_z = query.player_z;
    float display_view_x = static_cast<float>(query.player_x);
    float display_view_z = static_cast<float>(query.player_z);
    float render_camera_x = 0.0F;
    float ignored_render_camera_y = 0.0F;
    float render_camera_z = 0.0F;
    if (GetLatestBuildRenderCameraPosition(&render_camera_x, &ignored_render_camera_y,
                                           &render_camera_z)) {
        int32_t camera_block_x = 0;
        int32_t camera_block_z = 0;
        if (checkedFloorCoordinate(render_camera_x, &camera_block_x) &&
            checkedFloorCoordinate(render_camera_z, &camera_block_z)) {
            display_center_x = camera_block_x;
            display_center_z = camera_block_z;
            display_view_x = render_camera_x;
            display_view_z = render_camera_z;
        }
    }
    const ProjectionDisplayScope scope = renderer.displayScope(source->relative_origin_y);
    const ProjectionDisplayScopeCountKey scope_key =
        makeDisplayScopeCountKey(*source, scope, display_center_x, display_center_z);
    uint64_t scope_count = 0;
    if (lookupDisplayScopeCount(source, scope_key, &scope_count)) {
        result->display_scope_block_count = scope_count;
        result->display_scope_block_count_known = true;
    } else {
        scheduleDisplayScopeCount(source, {
            scope_key,
            scope,
            display_view_x,
            display_view_z,
        });
    }

    int32_t minimum_x = 0;
    int32_t maximum_x = 0;
    int32_t minimum_y = 0;
    int32_t maximum_y = 0;
    int32_t minimum_z = 0;
    int32_t maximum_z = 0;
    if (!checkedCoordinate(static_cast<int64_t>(query.player_x) -
                           query.horizontal_radius_blocks, &minimum_x) ||
        !checkedCoordinate(static_cast<int64_t>(query.player_x) +
                           query.horizontal_radius_blocks, &maximum_x) ||
        !checkedCoordinate(static_cast<int64_t>(query.player_y) -
                           query.vertical_radius_blocks, &minimum_y) ||
        !checkedCoordinate(static_cast<int64_t>(query.player_y) +
                           query.vertical_radius_blocks, &maximum_y) ||
        !checkedCoordinate(static_cast<int64_t>(query.player_z) -
                           query.horizontal_radius_blocks, &minimum_z) ||
        !checkedCoordinate(static_cast<int64_t>(query.player_z) +
                           query.horizontal_radius_blocks, &maximum_z)) {
        result->status = ProjectionPrinterQueryStatus::InvalidQuery;
        if (error) *error = "projection printer query exceeds supported world coordinates";
        return false;
    }

    std::vector<PartitionCoord> nearby_partitions;
    try {
        const int32_t start_x = floorDiv(minimum_x, kProjectionPartitionSize);
        const int32_t end_x = floorDiv(maximum_x, kProjectionPartitionSize);
        const int32_t start_y = floorDiv(minimum_y, kProjectionPartitionSize);
        const int32_t end_y = floorDiv(maximum_y, kProjectionPartitionSize);
        const int32_t start_z = floorDiv(minimum_z, kProjectionPartitionSize);
        const int32_t end_z = floorDiv(maximum_z, kProjectionPartitionSize);
        nearby_partitions.reserve(static_cast<size_t>(end_x - start_x + 1) *
                                  static_cast<size_t>(end_y - start_y + 1) *
                                  static_cast<size_t>(end_z - start_z + 1));
        for (int32_t partition_z = start_z; partition_z <= end_z; ++partition_z) {
            for (int32_t partition_y = start_y; partition_y <= end_y; ++partition_y) {
                for (int32_t partition_x = start_x; partition_x <= end_x; ++partition_x) {
                    const PartitionCoord coord{partition_x, partition_y, partition_z};
                    const auto info = source->partitions.find(coord);
                    if (info == source->partitions.end() || info->second.core_records == 0) {
                        continue;
                    }
                    nearby_partitions.push_back(coord);
                    schedulePrinterPartitionLoad(source, coord);
                }
            }
        }
    } catch (const std::bad_alloc&) {
        result->status = ProjectionPrinterQueryStatus::SourceUnavailable;
        if (error) *error = "not enough memory to inspect nearby projection partitions";
        return false;
    }

    std::vector<CachedProjectionPrinterTarget> nearby_targets;
    bool cold_partition_pending = false;
    // Never wait for a cache writer in a game tick.  A cold query simply
    // schedules its partitions above and returns an empty Ready snapshot until
    // the target worker has decoded them.
    std::unique_lock<std::mutex> cache_lock(source->printer_io_mutex, std::try_to_lock);
    if (cache_lock.owns_lock()) {
        try {
            for (const PartitionCoord& coord : nearby_partitions) {
                const auto cached = source->printer_partition_cache.find(coord);
                if (cached == source->printer_partition_cache.end()) {
                    cold_partition_pending = true;
                    continue;
                }
                cached->second.last_access = ++source->printer_partition_access_serial;
                for (const CachedProjectionPrinterTarget& target : cached->second.targets) {
                    if (target.x < minimum_x || target.x > maximum_x ||
                        target.y < minimum_y || target.y > maximum_y ||
                        target.z < minimum_z || target.z > maximum_z) {
                        continue;
                    }
                    if (query.restrict_to_display_scope &&
                        !projectionDisplayScopeContainsBlock(
                             scope, display_view_x, display_view_z, target.x, target.y,
                            target.z)) {
                        continue;
                    }
                    nearby_targets.push_back(target);
                }
            }
        } catch (const std::bad_alloc&) {
            result->status = ProjectionPrinterQueryStatus::SourceUnavailable;
            if (error) *error = "not enough memory to collect nearby projection targets";
            return false;
        }
    } else if (!nearby_partitions.empty()) {
        cold_partition_pending = true;
    }
    if (cache_lock.owns_lock()) cache_lock.unlock();
    if (!projectionPrinterSourceActive(source)) {
        result->status = ProjectionPrinterQueryStatus::SourceUnavailable;
        if (error) *error = "projection printer source was cleared";
        return false;
    }

    const auto score = [&](const CachedProjectionPrinterTarget& target) {
        const int64_t dx = static_cast<int64_t>(target.x) - query.player_x;
        const int64_t dy = static_cast<int64_t>(target.y) - query.player_y;
        const int64_t dz = static_cast<int64_t>(target.z) - query.player_z;
        return dx * dx + dy * dy + dz * dz;
    };
    std::sort(nearby_targets.begin(), nearby_targets.end(),
              [&](const CachedProjectionPrinterTarget& left,
                  const CachedProjectionPrinterTarget& right) {
                  const int64_t left_score = score(left);
                  const int64_t right_score = score(right);
                  if (left_score != right_score) return left_score < right_score;
                  if (left.y != right.y) return left.y < right.y;
                  if (left.x != right.x) return left.x < right.x;
                  if (left.z != right.z) return left.z < right.z;
                  if (left.name_index != right.name_index) {
                      return left.name_index < right.name_index;
                  }
                  return left.aux < right.aux;
              });
    if (nearby_targets.size() > query.maximum_targets) {
        nearby_targets.resize(query.maximum_targets);
        result->truncated = true;
    }
    try {
        result->targets.reserve(nearby_targets.size());
        for (const CachedProjectionPrinterTarget& target : nearby_targets) {
            if (target.name_index >= source->material_names.size()) {
                result->status = ProjectionPrinterQueryStatus::SourceUnavailable;
                if (error) *error = "projection printer target material is invalid";
                return false;
            }
            result->targets.push_back({
                target.x,
                target.y,
                target.z,
                source->material_names[target.name_index],
                target.aux,
                static_cast<ImportPhase>(target.phase),
                target.flags,
                static_cast<uint8_t>(source->request.rotation_degrees / 90),
            });
        }
    } catch (const std::bad_alloc&) {
        result->targets.clear();
        result->status = ProjectionPrinterQueryStatus::SourceUnavailable;
        if (error) *error = "not enough memory to copy nearby projection targets";
        return false;
    }
    result->truncated = result->truncated || cold_partition_pending;
    result->status = result->targets.empty() && !cold_partition_pending
        ? ProjectionPrinterQueryStatus::Empty
        : ProjectionPrinterQueryStatus::Ready;
    return true;
}

bool BuildProjectionRuntime::queryProjectionWorldMatchTargetPage(
        const ProjectionWorldMatchRegion& region, uint64_t cursor,
        size_t maximum_targets, ProjectionWorldMatchTargetPage* page,
        std::string* error) const {
    if (error) error->clear();
    if (!page) {
        if (error) *error = "projection world-match page is required";
        return false;
    }
    *page = ProjectionWorldMatchTargetPage{};
    if (!validWorldMatchRegion(region) || maximum_targets == 0 ||
        maximum_targets > kMaximumWorldMatchTargetPageTargets) {
        page->status = ProjectionWorldMatchTargetPageStatus::InvalidQuery;
        if (error) {
            *error = "projection world-match region must be ordered, span at most 32 blocks "
                "per axis, and request 1..256 targets";
        }
        return false;
    }

    size_t partition_ordinal = 0;
    size_t target_index = 0;
    if (!decodeWorldMatchCursor(cursor, &partition_ordinal, &target_index)) {
        page->status = ProjectionWorldMatchTargetPageStatus::InvalidQuery;
        if (error) *error = "projection world-match cursor is invalid";
        return false;
    }

    std::shared_ptr<LazyProjectionSource> source;
    {
        // clear()/publication briefly owns this mutex.  Returning Pending is
        // preferable to stalling a game tick until that lifecycle operation
        // completes; the caller will retry the same cursor next tick.
        std::unique_lock<std::mutex> lock(printer_source_mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            page->status = ProjectionWorldMatchTargetPageStatus::Pending;
            page->next_cursor = cursor;
            return true;
        }
        source = printer_source_;
    }
    if (!source) {
        page->status = ProjectionWorldMatchTargetPageStatus::NoProjection;
        return true;
    }
    if (!projectionPrinterSourceActive(source)) {
        page->status = ProjectionWorldMatchTargetPageStatus::SourceUnavailable;
        if (error) *error = "projection world-match source is being released";
        return false;
    }
    if (source->printer_target_load_failed.load(std::memory_order_acquire)) {
        page->status = ProjectionWorldMatchTargetPageStatus::SourceUnavailable;
        if (error) *error = "projection world-match source could not decode a partition";
        return false;
    }

    page->generation = source->generation;
    page->plan_identity = source->plan_identity;

    std::vector<PartitionCoord> partitions;
    try {
        const int32_t start_x = floorDiv(region.min_x, kProjectionPartitionSize);
        const int32_t end_x = floorDiv(region.max_x, kProjectionPartitionSize);
        const int32_t start_y = floorDiv(region.min_y, kProjectionPartitionSize);
        const int32_t end_y = floorDiv(region.max_y, kProjectionPartitionSize);
        const int32_t start_z = floorDiv(region.min_z, kProjectionPartitionSize);
        const int32_t end_z = floorDiv(region.max_z, kProjectionPartitionSize);
        const size_t partition_count =
            static_cast<size_t>(end_x - start_x + 1) *
            static_cast<size_t>(end_y - start_y + 1) *
            static_cast<size_t>(end_z - start_z + 1);
        partitions.reserve(partition_count);
        // This z/y/x traversal is the stable partition ordinal encoded into
        // the page cursor. It must not depend on unordered_map iteration.
        for (int32_t partition_z = start_z; partition_z <= end_z; ++partition_z) {
            for (int32_t partition_y = start_y; partition_y <= end_y; ++partition_y) {
                for (int32_t partition_x = start_x; partition_x <= end_x; ++partition_x) {
                    const PartitionCoord coord{partition_x, partition_y, partition_z};
                    const auto info = source->partitions.find(coord);
                    if (info == source->partitions.end() || info->second.core_records == 0) {
                        continue;
                    }
                    partitions.push_back(coord);
                }
            }
        }
    } catch (const std::bad_alloc&) {
        page->status = ProjectionWorldMatchTargetPageStatus::SourceUnavailable;
        if (error) *error = "not enough memory to enumerate world-match source partitions";
        return false;
    }

    if (partitions.empty()) {
        if (cursor != 0) {
            page->status = ProjectionWorldMatchTargetPageStatus::InvalidQuery;
            if (error) *error = "projection world-match cursor has no source partition";
            return false;
        }
        page->status = ProjectionWorldMatchTargetPageStatus::Complete;
        return true;
    }
    if (partition_ordinal >= partitions.size()) {
        page->status = ProjectionWorldMatchTargetPageStatus::InvalidQuery;
        if (error) *error = "projection world-match cursor partition is outside the region";
        return false;
    }

    // Schedule every tiny-region partition before inspecting the cache. The
    // target worker owns all spool I/O; this game-tick query only retries after
    // those entries become warm and never waits for either worker mutex.
    for (const PartitionCoord& coord : partitions) {
        schedulePrinterPartitionLoad(source, coord);
    }

    std::unique_lock<std::mutex> cache_lock(source->printer_io_mutex, std::try_to_lock);
    if (!cache_lock.owns_lock()) {
        page->status = ProjectionWorldMatchTargetPageStatus::Pending;
        page->next_cursor = cursor;
        return true;
    }
    if (!projectionPrinterSourceActive(source)) {
        page->status = ProjectionWorldMatchTargetPageStatus::SourceUnavailable;
        if (error) *error = "projection world-match source was cleared";
        return false;
    }
    // Page semantics stay simple and lossless: wait until the whole compact
    // region is warm instead of returning a partial prefix followed by a cold
    // partition. At most eight source partitions can overlap a valid region.
    for (const PartitionCoord& coord : partitions) {
        if (source->printer_partition_cache.find(coord) ==
            source->printer_partition_cache.end()) {
            page->status = ProjectionWorldMatchTargetPageStatus::Pending;
            page->next_cursor = cursor;
            return true;
        }
    }

    const auto cursor_cached = source->printer_partition_cache.find(
        partitions[partition_ordinal]);
    if (cursor_cached == source->printer_partition_cache.end() ||
        target_index > cursor_cached->second.targets.size()) {
        page->status = ProjectionWorldMatchTargetPageStatus::InvalidQuery;
        if (error) *error = "projection world-match cursor target index is outside the partition";
        return false;
    }

    try {
        page->targets.reserve(maximum_targets);
        size_t current_partition = partition_ordinal;
        size_t current_target = target_index;
        while (current_partition < partitions.size()) {
            const auto cached = source->printer_partition_cache.find(
                partitions[current_partition]);
            // All entries were proven warm above while this lock is held.
            if (cached == source->printer_partition_cache.end()) {
                page->targets.clear();
                page->status = ProjectionWorldMatchTargetPageStatus::Pending;
                page->next_cursor = cursor;
                return true;
            }
            cached->second.last_access = ++source->printer_partition_access_serial;
            const std::vector<CachedProjectionPrinterTarget>& targets = cached->second.targets;
            for (; current_target < targets.size(); ++current_target) {
                const CachedProjectionPrinterTarget& target = targets[current_target];
                if (!targetWithinWorldMatchRegion(target, region)) continue;
                if (target.name_index >= source->material_names.size() ||
                    source->material_names[target.name_index].empty()) {
                    page->targets.clear();
                    page->status = ProjectionWorldMatchTargetPageStatus::SourceUnavailable;
                    if (error) *error = "projection world-match target material is invalid";
                    return false;
                }
                page->targets.push_back({
                    target.x,
                    target.y,
                    target.z,
                    source->material_names[target.name_index],
                    target.aux,
                    static_cast<ImportPhase>(target.phase),
                    target.flags,
                    static_cast<uint8_t>(source->request.rotation_degrees / 90),
                });
                if (page->targets.size() == maximum_targets) {
                    ++current_target;
                    const uint64_t next_cursor = encodeWorldMatchCursor(
                        current_partition, current_target);
                    if (next_cursor == 0) {
                        page->targets.clear();
                        page->status = ProjectionWorldMatchTargetPageStatus::SourceUnavailable;
                        if (error) *error = "projection world-match cursor exceeds its encoding";
                        return false;
                    }
                    page->next_cursor = next_cursor;
                    page->status = ProjectionWorldMatchTargetPageStatus::Ready;
                    return true;
                }
            }
            ++current_partition;
            current_target = 0;
        }
    } catch (const std::bad_alloc&) {
        page->targets.clear();
        page->status = ProjectionWorldMatchTargetPageStatus::SourceUnavailable;
        if (error) *error = "not enough memory to copy projection world-match targets";
        return false;
    }

    page->next_cursor = 0;
    page->status = ProjectionWorldMatchTargetPageStatus::Complete;
    return true;
}

bool BuildProjectionRuntime::queryProjectionMaterialSummaryPage(
        uint64_t cursor, size_t maximum_entries,
        ProjectionMaterialSummaryPage* page, std::string* error) const {
    if (error) error->clear();
    if (!page) {
        if (error) *error = "projection material summary page is required";
        return false;
    }
    *page = ProjectionMaterialSummaryPage{};
    constexpr size_t kMaximumMaterialSummaryPageEntries = 256U;
    if (maximum_entries == 0 || maximum_entries > kMaximumMaterialSummaryPageEntries) {
        page->status = ProjectionMaterialSummaryPageStatus::InvalidQuery;
        if (error) {
            *error = "projection material summary page limit must be 1..256";
        }
        return false;
    }

    std::shared_ptr<LazyProjectionSource> source;
    {
        // Material paging may come from an injected UI thread rather than a
        // game tick, but preserving the same short source-lock discipline as
        // printer queries makes clear/publication safe without keeping a raw
        // source pointer beyond this scope.
        std::lock_guard<std::mutex> lock(printer_source_mutex_);
        source = printer_source_;
    }
    if (!source) {
        page->status = ProjectionMaterialSummaryPageStatus::NoProjection;
        return true;
    }
    if (!projectionPrinterSourceActive(source)) {
        page->status = ProjectionMaterialSummaryPageStatus::SourceUnavailable;
        if (error) *error = "projection material source is being released";
        return false;
    }

    page->generation = source->generation;
    page->plan_identity = source->plan_identity;
    std::lock_guard<std::mutex> lock(source->material_summary_mutex);
    if (!projectionPrinterSourceActive(source)) {
        page->status = ProjectionMaterialSummaryPageStatus::SourceUnavailable;
        if (error) *error = "projection material source was cleared";
        return false;
    }
    if (source->material_summary_failed) {
        page->status = ProjectionMaterialSummaryPageStatus::SourceUnavailable;
        if (error) *error = "projection material summary could not read its source";
        return false;
    }
    if (!source->material_summary_ready) {
        page->status = ProjectionMaterialSummaryPageStatus::Pending;
        page->next_cursor = cursor;
        return true;
    }

    const size_t entry_count = source->material_summary_entries.size();
    if (cursor > static_cast<uint64_t>(entry_count)) {
        page->status = ProjectionMaterialSummaryPageStatus::InvalidQuery;
        if (error) *error = "projection material summary cursor is outside the list";
        return false;
    }
    const size_t start = static_cast<size_t>(cursor);
    const size_t end = std::min(entry_count, start + maximum_entries);
    page->total_block_count = source->material_summary_block_count;
    page->total_material_count = static_cast<uint64_t>(entry_count);
    try {
        page->entries.reserve(end - start);
        for (size_t index = start; index < end; ++index) {
            page->entries.push_back(source->material_summary_entries[index]);
        }
    } catch (const std::bad_alloc&) {
        page->entries.clear();
        page->status = ProjectionMaterialSummaryPageStatus::SourceUnavailable;
        if (error) *error = "not enough memory to copy projection material summary page";
        return false;
    }
    if (end == entry_count) {
        page->next_cursor = 0;
        page->status = ProjectionMaterialSummaryPageStatus::Complete;
    } else {
        page->next_cursor = static_cast<uint64_t>(end);
        page->status = ProjectionMaterialSummaryPageStatus::Ready;
    }
    return true;
}

BuildProjectionState BuildProjectionRuntime::state() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

std::string BuildProjectionRuntime::status() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    std::string result = status_;
    if (state_ == BuildProjectionState::Ready) {
        const char* diagnostic = BuildProjectionRenderer::instance().renderDiagnostic();
        if (diagnostic && std::strcmp(diagnostic, "projection renderer ready") != 0) {
            result += " (";
            result += diagnostic;
            result += ")";
        }
    }
    return result;
}

uint64_t BuildProjectionRuntime::blockCount() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return block_count_;
}

std::string BuildProjectionRuntime::sourcePath() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return source_path_;
}

bool BuildProjectionRuntime::publishStatus(uint64_t generation,
                                           BuildProjectionState state,
                                           std::string status,
                                           uint64_t block_count,
                                           std::string source_path) {
    if (generation != generation_.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (generation != generation_.load(std::memory_order_relaxed)) return false;
    state_ = state;
    status_ = std::move(status);
    block_count_ = block_count;
    if (!source_path.empty() || state == BuildProjectionState::Idle ||
        state == BuildProjectionState::Failed) {
        source_path_ = std::move(source_path);
    }
    return true;
}

#if defined(BUILD_IMPORT_PROJECTION_TESTS)
bool RunBuildProjectionPartitionSpoolSelfTest(const std::string& work_directory,
                                              std::string* error) {
    const auto fail = [&](const std::string& detail) {
        if (error) *error = detail;
        return false;
    };
    // The printer and renderer must agree on this boundary: relative layers
    // are resolved once to world Y, while horizontal range includes a block
    // whose AABB touches the circular edge.
    ProjectionLayerFilter scope_filter;
    scope_filter.mode = ProjectionLayerMode::Range;
    scope_filter.space = ProjectionLayerSpace::Relative;
    scope_filter.minimum_y = 1;
    scope_filter.maximum_y = 3;
    ProjectionDisplayScope display_scope;
    if (!makeProjectionDisplayScope(64, 2, scope_filter, &display_scope) ||
        display_scope.range_chunks != 2 || display_scope.minimum_world_y != 65 ||
        display_scope.maximum_world_y != 67 ||
        !projectionDisplayScopeContainsBlock(display_scope, 0.f, 0.f, 32, 65, 0) ||
        projectionDisplayScopeContainsBlock(display_scope, 0.f, 0.f, 33, 65, 0) ||
        projectionDisplayScopeContainsBlock(display_scope, 0.f, 0.f, 0, 64, 0)) {
        return fail("projection display scope does not match range/layer semantics");
    }
    std::string directory;
    std::string detail;
    if (!createUniqueSpoolDirectory(work_directory, 0, &directory, &detail)) {
        return fail(detail.empty() ? "cannot create projection spool test directory" : detail);
    }
    ScopedSpoolDirectory cleanup(directory);
    PartitionSpoolWriter writer(directory);
    uint16_t stone = 0;
    uint16_t dirt = 0;
    if (!writer.internName("minecraft:stone", &stone, &detail) ||
        !writer.internName("minecraft:dirt", &dirt, &detail)) {
        return fail(detail);
    }

    const PartitionCoord primary{0, 0, 0};
    for (uint32_t index = 0; index < 130; ++index) {
        DiskRecord record{
            1, 1, 1, static_cast<uint16_t>(0x8100U + index),
            static_cast<uint8_t>(
                kProjectionRecordFlag |
                (index == 129 ? (kProjectionCanFillFlag | kProjectionStatefulFlag) : 0U)),
            static_cast<uint8_t>(index == 129 ? ImportPhase::Gravity : ImportPhase::Structure),
            static_cast<uint16_t>(index == 129 ? dirt : stone), 0,
        };
        if (!writer.append(primary, record, true, &detail)) return fail(detail);
    }
    // More than 64 simultaneously active partitions forces bounded-buffer LRU
    // flushes while every partition still shares one sequential container.
    for (int32_t x = 1; x <= 96; ++x) {
        DiskRecord record{
            x * kProjectionPartitionSize, 0, 0, 0, kProjectionRecordFlag,
            static_cast<uint8_t>(ImportPhase::Structure), stone, 0,
        };
        if (!writer.append({x, 0, 0}, record, true, &detail)) return fail(detail);
    }
    DiskRecord halo_record{1, kProjectionPartitionSize - 1, 1, 0,
                           kProjectionRecordFlag,
                           static_cast<uint8_t>(ImportPhase::Structure), stone, 0};
    const PartitionCoord halo_only{0, 1, 0};
    if (!writer.append(halo_only, halo_record, false, &detail) ||
        !writer.finish(&detail)) {
        return fail(detail);
    }
    if (!writer.finish(&detail)) return fail("projection partition finish is not idempotent");

    const auto primary_info = writer.partitions().find(primary);
    const auto halo_info = writer.partitions().find(halo_only);
    if (primary_info == writer.partitions().end() ||
        primary_info->second.total_records != 130 ||
        primary_info->second.core_records != 130 ||
        halo_info == writer.partitions().end() ||
        halo_info->second.total_records != 1 || halo_info->second.core_records != 0 ||
        writer.partitions().size() != 98) {
        return fail("projection partition index does not match the stress fixture");
    }

    std::ifstream size_stream(writer.containerPath(), std::ios::binary | std::ios::ate);
    if (!size_stream || size_stream.tellg() <= 0) {
        return fail("projection partition test container is empty");
    }
    const uint64_t container_size = static_cast<uint64_t>(size_stream.tellg());
    size_stream.close();
    const uint64_t size_before_rejected_append = container_size;
    DiskRecord rejected_record{0, 0, 0, 0, kProjectionRecordFlag,
                               static_cast<uint8_t>(ImportPhase::Structure), stone, 0};
    detail.clear();
    if (writer.append(primary, rejected_record, true, &detail) || detail.empty()) {
        return fail("projection partition accepted a record after finish");
    }
    std::ifstream unchanged_stream(writer.containerPath(), std::ios::binary | std::ios::ate);
    if (!unchanged_stream ||
        static_cast<uint64_t>(unchanged_stream.tellg()) != size_before_rejected_append) {
        return fail("rejected post-finish append changed the partition container");
    }
    unchanged_stream.close();

    DIR* raw_directory = opendir(directory.c_str());
    if (!raw_directory) return fail("cannot enumerate projection partition test directory");
    size_t regular_entry_count = 0;
    while (dirent* entry = readdir(raw_directory)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") ++regular_entry_count;
    }
    closedir(raw_directory);
    if (regular_entry_count != 1) {
        return fail("projection partition writer created more than one storage file");
    }

    BuildProjectionLoadRequest request;
    const auto never_cancel = [] { return false; };
    for (const auto& entry : writer.partitions()) {
        if (entry.second.core_records == 0) continue;
        std::ifstream stream(writer.containerPath(), std::ios::binary);
        ProjectionPartitionInput partition;
        uint64_t core_records = 0;
        size_t core_cells = 0;
        detail.clear();
        if (!readPartitionSpool(&stream, container_size, entry.first, entry.second,
                                request, writer.materialNames(), never_cancel,
                                &partition, entry.second.core_records, &core_records,
                                &core_cells, &detail)) {
            return fail(detail);
        }
        const size_t expected_cells = 1;
        if (core_records != entry.second.core_records || core_cells != expected_cells ||
            partition.blueprint.blocks.size() != expected_cells) {
            return fail("projection partition reader lost records during LRU stress");
        }
        if (entry.first == primary) {
            const ProjectionBlock& block = partition.blueprint.blocks.front();
            if (block.aux != static_cast<uint8_t>(129) ||
                block.source_aux != static_cast<uint16_t>(0x8181U) ||
                block.source_flags != static_cast<uint8_t>(
                    kProjectionCanFillFlag | kProjectionStatefulFlag) ||
                block.source_phase != static_cast<uint8_t>(ImportPhase::Gravity) ||
                block.rotation_quarters != 0 ||
                block.name_index >= partition.blueprint.names.size() ||
                partition.blueprint.names[block.name_index] != "minecraft:dirt") {
                return fail("projection partition lost printer metadata or last-write-wins order");
            }
        }
    }

    const auto read_header = [&](uint64_t offset, PartitionChunkHeader* header) {
        if (!header) return false;
        std::ifstream stream(writer.containerPath(), std::ios::binary);
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        stream.read(reinterpret_cast<char*>(header), sizeof(*header));
        return stream && stream.gcount() == static_cast<std::streamsize>(sizeof(*header));
    };
    const auto write_header = [&](uint64_t offset, const PartitionChunkHeader& header) {
        std::fstream stream(writer.containerPath(),
                            std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
        stream.flush();
        return static_cast<bool>(stream);
    };
    const auto expect_primary_failure = [&](const char* label) {
        std::ifstream stream(writer.containerPath(), std::ios::binary);
        ProjectionPartitionInput partition;
        uint64_t core_records = 0;
        size_t core_cells = 0;
        std::string read_error;
        const bool accepted = readPartitionSpool(
            &stream, container_size, primary, primary_info->second, request,
            writer.materialNames(), never_cancel, &partition,
            primary_info->second.core_records, &core_records, &core_cells, &read_error);
        if (!accepted && !read_error.empty()) return true;
        if (error) *error = std::string("projection partition accepted ") + label;
        return false;
    };

    PartitionChunkHeader original_header{};
    if (!read_header(primary_info->second.tail_offset, &original_header)) {
        return fail("cannot read projection partition test chunk header");
    }
    PartitionChunkHeader corrupted = original_header;
    corrupted.magic ^= 0x1U;
    if (!write_header(primary_info->second.tail_offset, corrupted) ||
        !expect_primary_failure("an invalid chunk magic") ||
        !write_header(primary_info->second.tail_offset, original_header)) {
        return false;
    }
    corrupted = original_header;
    corrupted.previous_offset = primary_info->second.tail_offset;
    if (!write_header(primary_info->second.tail_offset, corrupted) ||
        !expect_primary_failure("a self-referential chunk chain") ||
        !write_header(primary_info->second.tail_offset, original_header)) {
        return false;
    }

    const auto final_chunk = std::max_element(
        writer.partitions().begin(), writer.partitions().end(),
        [](const auto& left, const auto& right) {
            return left.second.tail_offset < right.second.tail_offset;
        });
    if (final_chunk == writer.partitions().end() || container_size == 0) {
        return fail("projection partition test cannot locate its final chunk");
    }
    std::ifstream truncated_stream(writer.containerPath(), std::ios::binary);
    ProjectionPartitionInput truncated_partition;
    uint64_t truncated_core_records = 0;
    size_t truncated_core_cells = 0;
    detail.clear();
    if (readPartitionSpool(
            &truncated_stream, container_size - 1U, final_chunk->first,
            final_chunk->second, request, writer.materialNames(), never_cancel,
            &truncated_partition, final_chunk->second.core_records,
            &truncated_core_records, &truncated_core_cells, &detail) || detail.empty()) {
        return fail("projection partition accepted a truncated final chunk");
    }

    std::string halo_directory;
    detail.clear();
    if (!createUniqueSpoolDirectory(work_directory, 1, &halo_directory, &detail)) {
        return fail(detail.empty() ? "cannot create projection halo test directory" : detail);
    }
    ScopedSpoolDirectory halo_cleanup(halo_directory);
    PartitionSpoolWriter halo_writer(halo_directory);
    const std::array<DiskRecord, 4> halo_fixture{{
        {31, 31, 1, 0, kProjectionRecordFlag,
         static_cast<uint8_t>(ImportPhase::Structure), 0, 0},
        {32, 31, 1, 0, kProjectionRecordFlag,
         static_cast<uint8_t>(ImportPhase::Structure), 0, 0},
        {32, 32, 1, 0, kProjectionRecordFlag,
         static_cast<uint8_t>(ImportPhase::Structure), 0, 0},
        {32, 32, 32, 0, kProjectionRecordFlag,
         static_cast<uint8_t>(ImportPhase::Structure), 0, 0},
    }};
    for (const DiskRecord& record : halo_fixture) {
        if (!appendPartitionRecordWithHalo(
                &halo_writer, record, "minecraft:stone", &detail)) {
            return fail(detail);
        }
    }
    if (!halo_writer.finish(&detail)) return fail(detail);

    const auto target_info = halo_writer.partitions().find(primary);
    if (target_info == halo_writer.partitions().end() ||
        target_info->second.total_records != halo_fixture.size() ||
        target_info->second.core_records != 1) {
        return fail("projection Chebyshev halo did not reach the diagonal target partition");
    }
    std::ifstream halo_size_stream(
        halo_writer.containerPath(), std::ios::binary | std::ios::ate);
    if (!halo_size_stream || halo_size_stream.tellg() <= 0) {
        return fail("projection halo test container is empty");
    }
    const uint64_t halo_container_size =
        static_cast<uint64_t>(halo_size_stream.tellg());
    halo_size_stream.close();

    std::ifstream halo_stream(halo_writer.containerPath(), std::ios::binary);
    ProjectionPartitionInput halo_partition;
    uint64_t halo_core_records = 0;
    size_t halo_core_cells = 0;
    detail.clear();
    if (!readPartitionSpool(
            &halo_stream, halo_container_size, primary, target_info->second,
            request, halo_writer.materialNames(), never_cancel, &halo_partition,
            target_info->second.core_records, &halo_core_records, &halo_core_cells,
            &detail)) {
        return fail(detail);
    }
    if (halo_core_records != 1 || halo_core_cells != 1 ||
        halo_partition.blueprint.blocks.size() != halo_fixture.size() ||
        halo_partition.blueprint.names.size() != 1 ||
        halo_partition.blueprint.names.front() != "minecraft:stone") {
        return fail("projection Chebyshev halo reader lost L-shaped boundary data");
    }
    for (const DiskRecord& expected : halo_fixture) {
        const auto found = std::find_if(
            halo_partition.blueprint.blocks.begin(), halo_partition.blueprint.blocks.end(),
            [&](const ProjectionBlock& block) {
                return block.x == expected.x && block.y == expected.y &&
                    block.z == expected.z;
            });
        if (found == halo_partition.blueprint.blocks.end()) {
            return fail("projection Chebyshev halo omitted an edge or corner neighbour");
        }
    }
    return true;
}
#endif

}  // namespace build_import
