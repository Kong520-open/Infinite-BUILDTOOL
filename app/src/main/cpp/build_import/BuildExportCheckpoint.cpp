#include "BuildExportCheckpoint.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace build_import {
namespace {

constexpr uint32_t kMagic = 0x50584542;  // "BEXP" in little-endian form.
constexpr uint32_t kMaximumStringLength = 16 * 1024;
constexpr uint64_t kMaximumPaletteBytes = 32ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumCommandDataBytes = 256ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumRawDataBytes = 256ULL * 1024ULL * 1024ULL;
constexpr uint32_t kMaximumPaletteSize = 65'536;
constexpr uint64_t kMaximumBlockCount = 16ULL * 1024ULL * 1024ULL;
constexpr uint8_t kCommandRedstoneMode = 1U << 0U;
constexpr uint8_t kCommandConditional = 1U << 1U;
constexpr uint8_t kCommandOutputTracked = 1U << 2U;
constexpr uint8_t kCommandExecutingOnFirstTick = 1U << 3U;
constexpr uint8_t kKnownCommandFlags = kCommandRedstoneMode | kCommandConditional |
    kCommandOutputTracked | kCommandExecutingOnFirstTick;

template <typename T>
bool writeValue(std::FILE* file, const T& value) {
    return file && std::fwrite(&value, sizeof(T), 1, file) == 1;
}

template <typename T>
bool readValue(std::FILE* file, T* value) {
    return file && value && std::fread(value, sizeof(T), 1, file) == 1;
}

bool writeString(std::FILE* file, const std::string& value) {
    if (value.size() > std::numeric_limits<uint32_t>::max()) return false;
    const uint32_t size = static_cast<uint32_t>(value.size());
    return writeValue(file, size) &&
        (size == 0 || std::fwrite(value.data(), 1, size, file) == size);
}

bool readString(std::FILE* file, std::string* value, uint64_t* total_bytes,
                uint32_t maximum_length = kMaximumStringLength,
                uint64_t maximum_total = kMaximumPaletteBytes) {
    uint32_t size = 0;
    if (!readValue(file, &size) || size > maximum_length) return false;
    if (total_bytes) {
        if (size > maximum_total || *total_bytes > maximum_total - size) return false;
        *total_bytes += size;
    }
    value->assign(size, '\0');
    return size == 0 || std::fread(&(*value)[0], 1, size, file) == size;
}

bool writeCommandBlock(std::FILE* file, const CommandBlockRecord& record) {
    uint8_t flags = 0;
    if (record.redstone_mode) flags |= kCommandRedstoneMode;
    if (record.conditional) flags |= kCommandConditional;
    if (record.output_tracked) flags |= kCommandOutputTracked;
    if (record.executing_on_first_tick) flags |= kCommandExecutingOnFirstTick;
    return writeValue(file, record.x) && writeValue(file, record.y) &&
        writeValue(file, record.z) && writeValue(file, record.mode) &&
        writeValue(file, flags) && writeValue(file, record.tick_delay) &&
        writeString(file, record.command) && writeString(file, record.last_output) &&
        writeString(file, record.name) && writeString(file, record.filtered_name);
}

bool readCommandBlock(std::FILE* file, CommandBlockRecord* record,
                      uint64_t* total_string_bytes) {
    if (!file || !record) return false;
    uint8_t flags = 0;
    if (!readValue(file, &record->x) || !readValue(file, &record->y) ||
        !readValue(file, &record->z) || !readValue(file, &record->mode) ||
        !readValue(file, &flags) || (flags & ~kKnownCommandFlags) != 0U ||
        !readValue(file, &record->tick_delay) ||
        !readString(file, &record->command, total_string_bytes,
                    CommandBlockSpoolWriter::kMaximumStringBytes,
                    kMaximumCommandDataBytes) ||
        !readString(file, &record->last_output, total_string_bytes,
                    CommandBlockSpoolWriter::kMaximumStringBytes,
                    kMaximumCommandDataBytes) ||
        !readString(file, &record->name, total_string_bytes,
                    CommandBlockSpoolWriter::kMaximumStringBytes,
                    kMaximumCommandDataBytes) ||
        !readString(file, &record->filtered_name, total_string_bytes,
                    CommandBlockSpoolWriter::kMaximumStringBytes,
                    kMaximumCommandDataBytes)) {
        return false;
    }
    record->redstone_mode = (flags & kCommandRedstoneMode) != 0U;
    record->conditional = (flags & kCommandConditional) != 0U;
    record->output_tracked = (flags & kCommandOutputTracked) != 0U;
    record->executing_on_first_tick =
        (flags & kCommandExecutingOnFirstTick) != 0U;
    return true;
}

bool writeRawBlock(std::FILE* file, const SchematicRawBlock& record) {
    return writeValue(file, record.x) && writeValue(file, record.y) &&
        writeValue(file, record.z) && writeValue(file, record.aux) &&
        writeValue(file, record.legacy_id) && writeString(file, record.identifier) &&
        writeString(file, record.state_json) && writeString(file, record.entity_json);
}

bool readRawBlock(std::FILE* file, SchematicRawBlock* record,
                  uint64_t* total_bytes) {
    if (!file || !record || !readValue(file, &record->x) ||
        !readValue(file, &record->y) || !readValue(file, &record->z) ||
        !readValue(file, &record->aux) || !readValue(file, &record->legacy_id) ||
        !readString(file, &record->identifier, total_bytes, 1024U,
                    kMaximumRawDataBytes) ||
        !readString(file, &record->state_json, total_bytes, 4U * 1024U * 1024U,
                    kMaximumRawDataBytes) ||
        !readString(file, &record->entity_json, total_bytes, 64U * 1024U * 1024U,
                    kMaximumRawDataBytes)) {
        return false;
    }
    return !record->identifier.empty() && record->identifier.find('\0') == std::string::npos;
}

bool flushAndSync(std::FILE* file) {
    if (!file || std::fflush(file) != 0) return false;
#if defined(_WIN32)
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

bool replaceFileAtomically(const std::string& source, const std::string& destination) {
#if defined(_WIN32)
    return MoveFileExA(source.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    return std::rename(source.c_str(), destination.c_str()) == 0;
#endif
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

bool fileExists(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
}

bool removeIfPresent(const std::string& path) {
    if (std::remove(path.c_str()) == 0) return true;
    return errno == ENOENT;
}

bool checkedVolume(const BuildExportCheckpointSnapshot& snapshot, uint64_t* volume) {
    if (snapshot.min_x > snapshot.max_x || snapshot.min_y > snapshot.max_y ||
        snapshot.min_z > snapshot.max_z) {
        return false;
    }
    const uint64_t width = static_cast<uint64_t>(
        static_cast<int64_t>(snapshot.max_x) - snapshot.min_x + 1);
    const uint64_t height = static_cast<uint64_t>(
        static_cast<int64_t>(snapshot.max_y) - snapshot.min_y + 1);
    const uint64_t length = static_cast<uint64_t>(
        static_cast<int64_t>(snapshot.max_z) - snapshot.min_z + 1);
    if (width > static_cast<uint64_t>(std::numeric_limits<int16_t>::max()) ||
        height > static_cast<uint64_t>(std::numeric_limits<int16_t>::max()) ||
        length > static_cast<uint64_t>(std::numeric_limits<int16_t>::max()) ||
        height > std::numeric_limits<uint64_t>::max() / width) {
        return false;
    }
    const uint64_t layer = width * height;
    if (length > std::numeric_limits<uint64_t>::max() / layer) return false;
    *volume = layer * length;
    return *volume != 0 && *volume <= kMaximumBlockCount;
}

bool validSnapshot(const BuildExportCheckpointSnapshot& snapshot,
                   const std::vector<std::string>& palette,
                   const std::vector<CommandBlockRecord>& command_blocks,
                   const std::vector<SchematicRawBlock>& raw_blocks) {
    uint64_t volume = 0;
    if (snapshot.format_version < kMinimumBuildExportCheckpointVersion ||
        snapshot.format_version > kBuildExportCheckpointVersion ||
        !checkedVolume(snapshot, &volume) || snapshot.output_path.empty() ||
        snapshot.world_id.empty() || snapshot.batch_size <= 0 ||
        snapshot.traversal_version != kBuildExportTraversalVersion ||
        snapshot.total_blocks != volume || snapshot.journal_entries > volume ||
        snapshot.exported_blocks > snapshot.journal_entries ||
        command_blocks.size() > snapshot.exported_blocks ||
        raw_blocks.size() > snapshot.exported_blocks ||
        (snapshot.format_version == 1 && !command_blocks.empty()) ||
        (snapshot.format_version < 3 && !raw_blocks.empty()) ||
        (snapshot.format_version < 4 &&
         (snapshot.container_capture_pending || snapshot.container_target_count != 0 ||
          snapshot.container_cursor != 0)) ||
        (snapshot.format_version < 5 && !snapshot.export_container_items) ||
        (!snapshot.export_container_items &&
         (snapshot.container_capture_pending || snapshot.container_target_count != 0 ||
          snapshot.container_cursor != 0)) ||
        snapshot.container_cursor > snapshot.container_target_count ||
        snapshot.container_target_count > raw_blocks.size() ||
        (snapshot.container_capture_pending && snapshot.container_target_count == 0) ||
        (!snapshot.container_capture_pending &&
         (snapshot.container_target_count != 0 || snapshot.container_cursor != 0)) ||
        palette.empty() || palette.size() > kMaximumPaletteSize ||
        palette.front() != "minecraft:air" ||
        (snapshot.writing &&
         (snapshot.journal_entries != volume || snapshot.container_capture_pending))) {
        return false;
    }
    uint64_t palette_bytes = 0;
    for (const std::string& state : palette) {
        if (state.empty() || state.size() > kMaximumStringLength ||
            palette_bytes > kMaximumPaletteBytes - state.size()) {
            return false;
        }
        palette_bytes += state.size();
    }
    const int64_t width = static_cast<int64_t>(snapshot.max_x) - snapshot.min_x + 1;
    const int64_t height = static_cast<int64_t>(snapshot.max_y) - snapshot.min_y + 1;
    const int64_t length = static_cast<int64_t>(snapshot.max_z) - snapshot.min_z + 1;
    uint64_t command_bytes = 0;
    for (const CommandBlockRecord& record : command_blocks) {
        if (record.x < 0 || record.x >= width || record.y < 0 || record.y >= height ||
            record.z < 0 || record.z >= length || !isValidCommandBlockMode(record.mode)) {
            return false;
        }
        for (const std::string* value : {&record.command, &record.last_output,
                                         &record.name, &record.filtered_name}) {
            if (value->size() > CommandBlockSpoolWriter::kMaximumStringBytes ||
                command_bytes > kMaximumCommandDataBytes - value->size()) {
                return false;
            }
            command_bytes += value->size();
        }
    }
    std::unordered_set<uint64_t> raw_coordinates;
    try {
        raw_coordinates.reserve(raw_blocks.size());
    } catch (const std::bad_alloc&) {
        return false;
    }
    uint64_t raw_bytes = 0;
    for (const SchematicRawBlock& record : raw_blocks) {
        if (record.x < 0 || record.x >= width || record.y < 0 || record.y >= height ||
            record.z < 0 || record.z >= length || record.identifier.empty() ||
            record.identifier.size() > 1024U || record.identifier.find('\0') != std::string::npos ||
            record.state_json.size() > 4U * 1024U * 1024U ||
            record.entity_json.size() > 64U * 1024U * 1024U) {
            return false;
        }
        const uint64_t coordinate = static_cast<uint64_t>(record.x) +
            static_cast<uint64_t>(record.z) * static_cast<uint64_t>(width) +
            static_cast<uint64_t>(record.y) * static_cast<uint64_t>(width) *
                static_cast<uint64_t>(length);
        if (!raw_coordinates.emplace(coordinate).second) return false;
        const uint64_t bytes = static_cast<uint64_t>(record.identifier.size()) +
            record.state_json.size() + record.entity_json.size();
        if (bytes > kMaximumRawDataBytes || raw_bytes > kMaximumRawDataBytes - bytes) {
            return false;
        }
        raw_bytes += bytes;
    }
    return true;
}

}  // namespace

std::string BuildExportCheckpoint::checkpointPath(const std::string& output_path) {
    return output_path + ".export.checkpoint";
}

std::string BuildExportCheckpoint::journalPath(const std::string& output_path) {
    return output_path + ".export.blocks";
}

bool BuildExportCheckpoint::hasArtifacts(const std::string& output_path) {
    if (output_path.empty()) return false;
    return fileExists(checkpointPath(output_path)) || fileExists(journalPath(output_path));
}

bool BuildExportCheckpoint::createJournal(const std::string& output_path, std::string* error) {
    const std::string path = journalPath(output_path);
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        if (error) *error = "cannot create export block journal";
        return false;
    }
    const bool ok = flushAndSync(file);
    const bool closed = std::fclose(file) == 0;
    if (!ok || !closed) {
        std::remove(path.c_str());
        if (error) *error = "cannot initialize export block journal";
        return false;
    }
    return true;
}

bool BuildExportCheckpoint::appendJournal(const std::string& output_path,
                                          const uint16_t* values, size_t count,
                                          std::string* error) {
    if (count == 0) return true;
    if (!values) {
        if (error) *error = "invalid export block journal buffer";
        return false;
    }
    std::FILE* file = std::fopen(journalPath(output_path).c_str(), "ab");
    if (!file) {
        if (error) *error = "cannot open export block journal";
        return false;
    }
    const bool written = std::fwrite(values, sizeof(uint16_t), count, file) == count;
    const bool flushed = std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !flushed || !closed) {
        if (error) *error = "cannot append export block journal";
        return false;
    }
    return true;
}

bool BuildExportCheckpoint::syncJournal(const std::string& output_path, std::string* error) {
    std::FILE* file = std::fopen(journalPath(output_path).c_str(), "rb");
    if (!file) {
        if (error) *error = "cannot open export block journal for sync";
        return false;
    }
#if defined(_WIN32)
    const bool synced = _commit(_fileno(file)) == 0;
#else
    const bool synced = fsync(fileno(file)) == 0;
#endif
    const bool closed = std::fclose(file) == 0;
    if (!synced || !closed) {
        if (error) *error = "cannot sync export block journal";
        return false;
    }
    return true;
}

bool BuildExportCheckpoint::truncateJournal(const std::string& output_path,
                                            uint64_t entry_count,
                                            std::string* error) {
    if (entry_count > std::numeric_limits<uint64_t>::max() / sizeof(uint16_t)) {
        if (error) *error = "export block journal length overflows";
        return false;
    }
    std::FILE* file = std::fopen(journalPath(output_path).c_str(), "r+b");
    if (!file) {
        if (error) *error = "cannot open export block journal for recovery";
        return false;
    }
    const uint64_t bytes = entry_count * sizeof(uint16_t);
#if defined(_WIN32)
    const bool truncated = _chsize_s(_fileno(file), bytes) == 0;
#else
    const bool truncated = bytes <= static_cast<uint64_t>(std::numeric_limits<off_t>::max()) &&
        ftruncate(fileno(file), static_cast<off_t>(bytes)) == 0;
#endif
    const bool synced = truncated && flushAndSync(file);
    const bool closed = std::fclose(file) == 0;
    if (!truncated || !synced || !closed) {
        if (error) *error = "cannot truncate export block journal";
        return false;
    }
    return true;
}

bool BuildExportCheckpoint::journalHasEntries(const std::string& output_path,
                                              uint64_t entry_count,
                                              std::string* error) {
    std::FILE* file = std::fopen(journalPath(output_path).c_str(), "rb");
    if (!file) {
        if (error) *error = "export block journal is missing";
        return false;
    }
#if defined(_WIN32)
    const int descriptor = _fileno(file);
    const __int64 size = descriptor >= 0 ? _filelengthi64(descriptor) : -1;
    const bool valid = size >= 0 && static_cast<uint64_t>(size) >=
        entry_count * sizeof(uint16_t);
#else
    struct stat info {};
    const bool valid = fstat(fileno(file), &info) == 0 && info.st_size >= 0 &&
        static_cast<uint64_t>(info.st_size) >= entry_count * sizeof(uint16_t);
#endif
    std::fclose(file);
    if (!valid && error) *error = "export block journal is truncated";
    return valid;
}

bool BuildExportCheckpoint::saveAtomically(
        const std::string& output_path,
        const BuildExportCheckpointSnapshot& snapshot,
        std::string* error) {
    return saveAtomically(output_path, snapshot, snapshot.palette,
                          snapshot.command_blocks, snapshot.raw_blocks, error);
}

bool BuildExportCheckpoint::saveAtomically(
        const std::string& output_path,
        const BuildExportCheckpointSnapshot& snapshot,
        const std::vector<std::string>& palette,
        std::string* error) {
    return saveAtomically(output_path, snapshot, palette, snapshot.command_blocks,
                          snapshot.raw_blocks, error);
}

bool BuildExportCheckpoint::saveAtomically(
        const std::string& output_path,
        const BuildExportCheckpointSnapshot& snapshot,
        const std::vector<std::string>& palette,
        const std::vector<CommandBlockRecord>& command_blocks,
        std::string* error) {
    return saveAtomically(output_path, snapshot, palette, command_blocks,
                          snapshot.raw_blocks, error);
}

bool BuildExportCheckpoint::saveAtomically(
        const std::string& output_path,
        const BuildExportCheckpointSnapshot& snapshot,
        const std::vector<std::string>& palette,
        const std::vector<CommandBlockRecord>& command_blocks,
        const std::vector<SchematicRawBlock>& raw_blocks,
        std::string* error) {
    if (snapshot.format_version != kBuildExportCheckpointVersion ||
        !validSnapshot(snapshot, palette, command_blocks, raw_blocks) ||
        snapshot.output_path != output_path) {
        if (error) *error = "invalid export checkpoint snapshot";
        return false;
    }
    const std::string path = checkpointPath(output_path);
    const std::string temporary_path = path + ".tmp";
    std::FILE* file = std::fopen(temporary_path.c_str(), "wb");
    if (!file) {
        if (error) *error = "cannot create export checkpoint temporary file";
        return false;
    }
    const uint8_t writing = snapshot.writing ? 1 : 0;
    const uint32_t palette_size = static_cast<uint32_t>(palette.size());
    const uint64_t command_block_count = static_cast<uint64_t>(command_blocks.size());
    bool ok = writeValue(file, kMagic) &&
        writeValue(file, kBuildExportCheckpointVersion) &&
        writeString(file, snapshot.output_path) && writeString(file, snapshot.world_id) &&
        writeValue(file, snapshot.dimension_id) &&
        writeValue(file, snapshot.min_x) && writeValue(file, snapshot.min_y) &&
        writeValue(file, snapshot.min_z) && writeValue(file, snapshot.max_x) &&
        writeValue(file, snapshot.max_y) && writeValue(file, snapshot.max_z) &&
        writeValue(file, snapshot.batch_size) && writeValue(file, snapshot.traversal_version) &&
        writeValue(file, snapshot.total_blocks) && writeValue(file, snapshot.journal_entries) &&
        writeValue(file, snapshot.exported_blocks) && writeValue(file, snapshot.current_batch) &&
        writeValue(file, snapshot.batch_cursor) && writeValue(file, writing) &&
        writeValue(file, palette_size);
    for (const std::string& state : palette) {
        if (ok) ok = writeString(file, state);
    }
    if (ok) ok = writeValue(file, command_block_count);
    for (const CommandBlockRecord& record : command_blocks) {
        if (ok) ok = writeCommandBlock(file, record);
    }
    const uint64_t raw_block_count = static_cast<uint64_t>(raw_blocks.size());
    if (ok) ok = writeValue(file, raw_block_count);
    for (const SchematicRawBlock& record : raw_blocks) {
        if (ok) ok = writeRawBlock(file, record);
    }
    const uint8_t container_capture_pending =
        snapshot.container_capture_pending ? 1U : 0U;
    const uint8_t export_container_items =
        snapshot.export_container_items ? 1U : 0U;
    if (ok) {
        ok = writeValue(file, export_container_items) &&
            writeValue(file, container_capture_pending) &&
            writeValue(file, snapshot.container_target_count) &&
            writeValue(file, snapshot.container_cursor);
    }
    const bool synced = ok && flushAndSync(file);
    const bool closed = std::fclose(file) == 0;
    if (!ok || !synced || !closed) {
        std::remove(temporary_path.c_str());
        if (error) *error = "cannot write export checkpoint";
        return false;
    }
    if (!replaceFileAtomically(temporary_path, path)) {
        std::remove(temporary_path.c_str());
        if (error) *error = "cannot atomically replace export checkpoint";
        return false;
    }
    syncParentDirectory(path);
    return true;
}

std::optional<BuildExportCheckpointSnapshot> BuildExportCheckpoint::load(
        const std::string& output_path, std::string* error) {
    try {
        const std::string path = checkpointPath(output_path);
        std::unique_ptr<std::FILE, decltype(&std::fclose)> file(
            std::fopen(path.c_str(), "rb"), &std::fclose);
        if (!file) {
            if (error) *error = "export checkpoint is missing";
            return std::nullopt;
        }
        uint32_t magic = 0;
        uint32_t version = 0;
        uint8_t writing = 0;
        uint32_t palette_size = 0;
        uint64_t palette_bytes = 0;
        uint64_t command_block_count = 0;
        uint64_t command_string_bytes = 0;
        uint64_t raw_block_count = 0;
        uint64_t raw_string_bytes = 0;
        uint8_t export_container_items = 1;
        uint8_t container_capture_pending = 0;
        BuildExportCheckpointSnapshot snapshot;
        bool ok = readValue(file.get(), &magic) && readValue(file.get(), &version) &&
            magic == kMagic && version >= kMinimumBuildExportCheckpointVersion &&
            version <= kBuildExportCheckpointVersion &&
            readString(file.get(), &snapshot.output_path, nullptr) &&
            readString(file.get(), &snapshot.world_id, nullptr) &&
            readValue(file.get(), &snapshot.dimension_id) &&
            readValue(file.get(), &snapshot.min_x) && readValue(file.get(), &snapshot.min_y) &&
            readValue(file.get(), &snapshot.min_z) && readValue(file.get(), &snapshot.max_x) &&
            readValue(file.get(), &snapshot.max_y) && readValue(file.get(), &snapshot.max_z) &&
            readValue(file.get(), &snapshot.batch_size) &&
            readValue(file.get(), &snapshot.traversal_version) &&
            readValue(file.get(), &snapshot.total_blocks) &&
            readValue(file.get(), &snapshot.journal_entries) &&
            readValue(file.get(), &snapshot.exported_blocks) &&
            readValue(file.get(), &snapshot.current_batch) &&
            readValue(file.get(), &snapshot.batch_cursor) && readValue(file.get(), &writing) &&
            readValue(file.get(), &palette_size) && writing <= 1 && palette_size > 0 &&
            palette_size <= kMaximumPaletteSize;
        if (ok) {
            snapshot.palette.resize(palette_size);
        }
        for (uint32_t index = 0; ok && index < palette_size; ++index) {
            ok = readString(file.get(), &snapshot.palette[index], &palette_bytes);
        }
        if (ok && version >= 2) {
            ok = readValue(file.get(), &command_block_count) &&
                command_block_count <= snapshot.exported_blocks &&
                command_block_count <= static_cast<uint64_t>(
                    std::numeric_limits<size_t>::max());
            if (ok) snapshot.command_blocks.reserve(
                static_cast<size_t>(command_block_count));
            for (uint64_t index = 0; ok && index < command_block_count; ++index) {
                CommandBlockRecord record;
                ok = readCommandBlock(file.get(), &record, &command_string_bytes);
                if (ok) snapshot.command_blocks.push_back(std::move(record));
            }
        }
        if (ok && version >= 3) {
            ok = readValue(file.get(), &raw_block_count) &&
                raw_block_count <= snapshot.exported_blocks &&
                raw_block_count <= static_cast<uint64_t>(std::numeric_limits<size_t>::max());
            if (ok) snapshot.raw_blocks.reserve(static_cast<size_t>(raw_block_count));
            for (uint64_t index = 0; ok && index < raw_block_count; ++index) {
                SchematicRawBlock record;
                ok = readRawBlock(file.get(), &record, &raw_string_bytes);
                if (ok) snapshot.raw_blocks.push_back(std::move(record));
            }
        }
        if (ok && version >= 5) {
            ok = readValue(file.get(), &export_container_items) &&
                export_container_items <= 1U;
            snapshot.export_container_items = export_container_items != 0;
        }
        if (ok && version >= 4) {
            ok = readValue(file.get(), &container_capture_pending) &&
                container_capture_pending <= 1U &&
                readValue(file.get(), &snapshot.container_target_count) &&
                readValue(file.get(), &snapshot.container_cursor);
            snapshot.container_capture_pending = container_capture_pending != 0;
        }
        const int trailing = ok ? std::fgetc(file.get()) : EOF;
        snapshot.format_version = version;
        snapshot.writing = writing != 0;
        if (!ok || trailing != EOF || snapshot.output_path != output_path ||
            !validSnapshot(snapshot, snapshot.palette, snapshot.command_blocks,
                           snapshot.raw_blocks) ||
            !journalHasEntries(output_path, snapshot.journal_entries, nullptr)) {
            if (error) *error = "export checkpoint is corrupt, incomplete, or unsupported";
            return std::nullopt;
        }
        return snapshot;
    } catch (const std::bad_alloc&) {
        if (error) *error = "not enough memory to load export checkpoint";
        return std::nullopt;
    } catch (...) {
        if (error) *error = "cannot load export checkpoint";
        return std::nullopt;
    }
}

bool BuildExportCheckpoint::discard(const std::string& output_path, std::string* error) {
    if (output_path.empty()) {
        if (error) *error = "invalid export checkpoint path";
        return false;
    }
    const std::string checkpoint = checkpointPath(output_path);
    const bool ok = removeIfPresent(checkpoint + ".tmp") &&
        removeIfPresent(checkpoint) && removeIfPresent(journalPath(output_path)) &&
        removeIfPresent(output_path + ".part") &&
        removeIfPresent(output_path + ".schem.part") &&
        removeIfPresent(output_path + ".bdx.part") &&
        removeIfPresent(output_path + ".infinity.part") &&
        removeIfPresent(output_path + ".infinity.plaintext.part") &&
        removeIfPresent(output_path + ".infinity.compressed.part") &&
        removeIfPresent(output_path + ".IBuild.part") &&
        removeIfPresent(output_path + ".ibuild.part") &&
        removeIfPresent(output_path + ".plaintext.part") &&
        removeIfPresent(output_path + ".compressed.part");
    if (!ok && error) {
        *error = "cannot delete one or more export checkpoint files: " +
            std::string(std::strerror(errno));
    }
    return ok;
}

}  // namespace build_import
