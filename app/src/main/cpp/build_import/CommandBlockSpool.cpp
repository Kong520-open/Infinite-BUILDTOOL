#include "CommandBlockSpool.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace build_import {
namespace {

constexpr uint32_t kSpoolMagic = 0x31534243U;     // "CBS1" in little endian
constexpr uint32_t kManifestMagic = 0x314D4243U;  // "CBM1" in little endian
constexpr uint32_t kHeaderVersion = CommandBlockSpoolWriter::kVersion;
constexpr uint32_t kLegacyHeaderVersion = 1;
constexpr uint8_t kFlagRedstoneMode = 0x01U;
constexpr uint8_t kFlagConditional = 0x02U;
constexpr uint8_t kFlagOutputTracked = 0x04U;
constexpr uint8_t kFlagExecutingOnFirstTick = 0x08U;
constexpr uint8_t kKnownFlags = kFlagRedstoneMode | kFlagConditional |
    kFlagOutputTracked | kFlagExecutingOnFirstTick;

// Spools are only consumed by the same native importer ABI, but fixed structs
// make accidental format drift visible in host tests and allow a reader to
// reject partial files before it attempts to parse variable-size text.
struct FileHeaderV1 {
    uint32_t magic = kSpoolMagic;
    uint32_t version = kHeaderVersion;
    uint64_t record_count = 0;
    uint64_t data_bytes = 0;
};

struct RecordHeaderV1 {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint16_t mode = 0;
    uint8_t flags = 0;
    uint8_t reserved = 0;
    uint32_t command_bytes = 0;
    uint32_t last_output_bytes = 0;
    uint32_t name_bytes = 0;
    uint32_t filtered_name_bytes = 0;
};

// V1 accidentally omitted tick_delay.  Keep its wire layout above for resume
// compatibility, but write the corrected V2 layout for every new import.
struct RecordHeaderV2 {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint16_t mode = 0;
    uint8_t flags = 0;
    uint8_t reserved = 0;
    int32_t tick_delay = 0;
    uint32_t command_bytes = 0;
    uint32_t last_output_bytes = 0;
    uint32_t name_bytes = 0;
    uint32_t filtered_name_bytes = 0;
};

struct ManifestV1 {
    uint32_t magic = kManifestMagic;
    uint32_t version = kHeaderVersion;
    uint64_t record_count = 0;
    uint64_t data_bytes = 0;
    int32_t min_x = 0;
    int32_t min_y = 0;
    int32_t min_z = 0;
    int32_t max_x = -1;
    int32_t max_y = -1;
    int32_t max_z = -1;
};

static_assert(sizeof(FileHeaderV1) == 24, "unexpected command-block spool header layout");
static_assert(sizeof(RecordHeaderV1) == 32, "unexpected command-block spool record layout");
static_assert(sizeof(RecordHeaderV2) == 36, "unexpected V2 command-block spool record layout");
static_assert(sizeof(ManifestV1) == 48, "unexpected command-block manifest layout");

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

int createDirectory(const char* path) {
#if defined(_WIN32)
    return ::_mkdir(path);
#else
    return ::mkdir(path, 0700);
#endif
}

bool ensureDirectory(const std::string& directory) {
    if (directory.empty()) return false;
    std::string partial;
    for (size_t index = 0; index <= directory.size(); ++index) {
        if (index < directory.size() && directory[index] != '/' && directory[index] != '\\') {
            partial.push_back(directory[index]);
            continue;
        }
        if (!partial.empty() && partial != "." &&
            createDirectory(partial.c_str()) != 0 && errno != EEXIST) {
            return false;
        }
        if (index != directory.size()) {
            const char separator = directory[index];
            if (partial.empty() && (separator == '/' || separator == '\\')) {
                partial.push_back(separator);
            } else if (!partial.empty() && partial.back() != '/' && partial.back() != '\\') {
                partial.push_back(separator);
            }
        }
    }
    return true;
}

std::string joinPath(const std::string& directory, const std::string& file_name) {
    if (directory.empty()) return file_name;
    if (directory.back() == '/' || directory.back() == '\\') return directory + file_name;
    return directory + "/" + file_name;
}

bool replaceFile(const std::string& temporary, const std::string& destination) {
    // Android/POSIX rename replaces an existing file atomically.  Do not first
    // remove the final sidecar there: an app/process interruption between the
    // two calls would otherwise lose a resumable import.  MSVCRT does not offer
    // overwrite semantics, so retain the host-test fallback only on Windows.
#if defined(_WIN32)
    std::remove(destination.c_str());
#endif
    return std::rename(temporary.c_str(), destination.c_str()) == 0;
}

bool checkedRecordLengths(const CommandBlockRecord& record, uint64_t* total,
                          std::string* error) {
    const std::array<size_t, 4> lengths{{record.command.size(), record.last_output.size(),
                                         record.name.size(), record.filtered_name.size()}};
    uint64_t bytes = sizeof(RecordHeaderV2);
    for (const size_t length : lengths) {
        if (length > CommandBlockSpoolWriter::kMaximumStringBytes) {
            return fail(error, "command-block text exceeds the 1 MiB spool safety limit");
        }
        if (length > std::numeric_limits<uint32_t>::max() ||
            bytes > CommandBlockSpoolWriter::kMaximumRecordBytes - length) {
            return fail(error, "command-block spool record exceeds the safety limit");
        }
        bytes += length;
    }
    *total = bytes;
    return true;
}

void extendBounds(BlockBounds* bounds, const CommandBlockRecord& record) {
    if (!bounds->isValid()) {
        *bounds = {record.x, record.y, record.z, record.x, record.y, record.z};
        return;
    }
    bounds->min_x = std::min(bounds->min_x, record.x);
    bounds->min_y = std::min(bounds->min_y, record.y);
    bounds->min_z = std::min(bounds->min_z, record.z);
    bounds->max_x = std::max(bounds->max_x, record.x);
    bounds->max_y = std::max(bounds->max_y, record.y);
    bounds->max_z = std::max(bounds->max_z, record.z);
}

}  // namespace

CommandBlockSpoolWriter::CommandBlockSpoolWriter(std::string directory,
                                                   std::string file_name)
    : directory_(std::move(directory)),
      spool_path_(joinPath(directory_, file_name.empty() ? kFileName : file_name)),
      temporary_path_(spool_path_ + ".tmp"),
      manifest_path_(spool_path_ + ".manifest"),
      temporary_manifest_path_(manifest_path_ + ".tmp") {}

CommandBlockSpoolWriter::~CommandBlockSpoolWriter() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!finished_) discardLocked(false);
}

bool CommandBlockSpoolWriter::ensureOpenLocked(std::string* error) {
    if (opened_) return !failed_;
    if (failed_ || finished_) return fail(error, "command-block spool writer is closed");
    if (!ensureDirectory(directory_)) {
        failed_ = true;
        return fail(error, "cannot create command-block spool directory");
    }
    std::remove(temporary_path_.c_str());
    output_.open(temporary_path_, std::ios::binary | std::ios::trunc);
    if (!output_) {
        failed_ = true;
        return fail(error, "cannot create command-block spool");
    }
    const FileHeaderV1 header;
    output_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!output_) {
        output_.close();
        failed_ = true;
        return fail(error, "cannot initialize command-block spool");
    }
    opened_ = true;
    return true;
}

bool CommandBlockSpoolWriter::append(const CommandBlockRecord& record, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t serialized_size = 0;
    if (!isValidCommandBlockMode(record.mode)) {
        return fail(error, "command-block mode must be impulse, repeat, or chain");
    }
    if (!checkedRecordLengths(record, &serialized_size, error) || !ensureOpenLocked(error)) {
        return false;
    }
    if (record_count_ == std::numeric_limits<uint64_t>::max() ||
        data_bytes_ > std::numeric_limits<uint64_t>::max() - serialized_size) {
        failed_ = true;
        return fail(error, "command-block spool size overflows");
    }
    RecordHeaderV2 header;
    header.x = record.x;
    header.y = record.y;
    header.z = record.z;
    header.mode = record.mode;
    header.flags = (record.redstone_mode ? kFlagRedstoneMode : 0U) |
                   (record.conditional ? kFlagConditional : 0U) |
                    (record.output_tracked ? kFlagOutputTracked : 0U) |
                    (record.executing_on_first_tick ? kFlagExecutingOnFirstTick : 0U);
    header.tick_delay = record.tick_delay;
    header.command_bytes = static_cast<uint32_t>(record.command.size());
    header.last_output_bytes = static_cast<uint32_t>(record.last_output.size());
    header.name_bytes = static_cast<uint32_t>(record.name.size());
    header.filtered_name_bytes = static_cast<uint32_t>(record.filtered_name.size());
    output_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    const auto write_text = [&](const std::string& value) {
        if (!value.empty()) output_.write(value.data(), static_cast<std::streamsize>(value.size()));
    };
    write_text(record.command);
    write_text(record.last_output);
    write_text(record.name);
    write_text(record.filtered_name);
    if (!output_) {
        failed_ = true;
        return fail(error, "cannot append command-block spool record");
    }
    ++record_count_;
    data_bytes_ += serialized_size;
    extendBounds(&bounds, record);
    return true;
}

bool CommandBlockSpoolWriter::writeManifestLocked(const CommandBlockSpoolManifest& manifest,
                                                    std::string* error) {
    std::remove(temporary_manifest_path_.c_str());
    std::ofstream manifest_output(temporary_manifest_path_, std::ios::binary | std::ios::trunc);
    if (!manifest_output) return fail(error, "cannot create command-block spool manifest");
    const ManifestV1 disk{
        kManifestMagic, kHeaderVersion, manifest.record_count, manifest.data_bytes,
        manifest.bounds.min_x, manifest.bounds.min_y, manifest.bounds.min_z,
        manifest.bounds.max_x, manifest.bounds.max_y, manifest.bounds.max_z,
    };
    manifest_output.write(reinterpret_cast<const char*>(&disk), sizeof(disk));
    manifest_output.flush();
    manifest_output.close();
    if (!manifest_output || !replaceFile(temporary_manifest_path_, manifest_path_)) {
        std::remove(temporary_manifest_path_.c_str());
        return fail(error, "cannot finalize command-block spool manifest");
    }
    return true;
}

bool CommandBlockSpoolWriter::finish(CommandBlockSpoolManifest* manifest, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!manifest) return fail(error, "command-block spool manifest output is unavailable");
    *manifest = {};
    if (failed_) return fail(error, "command-block spool writer failed");
    if (finished_) {
        manifest->spool_path = spool_path_;
        manifest->manifest_path = manifest_path_;
        manifest->record_count = record_count_;
        manifest->data_bytes = data_bytes_;
        manifest->bounds = bounds;
        return true;
    }
    if (!ensureOpenLocked(error)) return false;
    const FileHeaderV1 header{kSpoolMagic, kHeaderVersion, record_count_, data_bytes_};
    output_.seekp(0, std::ios::beg);
    output_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output_.flush();
    output_.close();
    if (!output_ || !replaceFile(temporary_path_, spool_path_)) {
        failed_ = true;
        std::remove(temporary_path_.c_str());
        return fail(error, "cannot finalize command-block spool");
    }
    CommandBlockSpoolManifest completed;
    completed.spool_path = spool_path_;
    completed.manifest_path = manifest_path_;
    completed.record_count = record_count_;
    completed.data_bytes = data_bytes_;
    completed.bounds = bounds;
    if (!writeManifestLocked(completed, error)) {
        failed_ = true;
        return false;
    }
    finished_ = true;
    *manifest = std::move(completed);
    return true;
}

void CommandBlockSpoolWriter::discardLocked(bool remove_final) {
    if (output_.is_open()) output_.close();
    std::remove(temporary_path_.c_str());
    std::remove(temporary_manifest_path_.c_str());
    if (remove_final) {
        std::remove(spool_path_.c_str());
        std::remove(manifest_path_.c_str());
    }
}

void CommandBlockSpoolWriter::discard() {
    std::lock_guard<std::mutex> lock(mutex_);
    discardLocked(true);
    opened_ = false;
    finished_ = false;
    record_count_ = 0;
    data_bytes_ = 0;
    bounds = {};
}

bool CommandBlockSpoolWriter::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

uint64_t CommandBlockSpoolWriter::recordCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return record_count_;
}

bool CommandBlockSpoolWriter::readManifest(const std::string& manifest_path,
                                           CommandBlockSpoolManifest* manifest,
                                           std::string* error) {
    if (!manifest) return fail(error, "command-block spool manifest output is unavailable");
    *manifest = {};
    std::ifstream input(manifest_path, std::ios::binary);
    ManifestV1 disk;
    input.read(reinterpret_cast<char*>(&disk), sizeof(disk));
    if (!input || disk.magic != kManifestMagic ||
        (disk.version != kLegacyHeaderVersion && disk.version != kHeaderVersion)) {
        return fail(error, "invalid command-block spool manifest");
    }
    char trailing = 0;
    if (input.read(&trailing, 1)) return fail(error, "command-block spool manifest has trailing data");
    manifest->manifest_path = manifest_path;
    const size_t separator = manifest_path.rfind(".manifest");
    manifest->spool_path = separator == std::string::npos
        ? manifest_path : manifest_path.substr(0, separator);
    manifest->record_count = disk.record_count;
    manifest->data_bytes = disk.data_bytes;
    manifest->bounds = {disk.min_x, disk.min_y, disk.min_z,
                        disk.max_x, disk.max_y, disk.max_z};
    return true;
}

CommandBlockSpoolReader::CommandBlockSpoolReader(std::string path) : path_(std::move(path)) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (path_.empty()) {
        failed_ = true;
        return;
    }
    input_.open(path_, std::ios::binary);
    if (!input_ || !readHeaderLocked()) {
        failed_ = true;
        valid_ = false;
    }
}

void CommandBlockSpoolReader::failLocked() {
    failed_ = true;
    valid_ = false;
}

bool CommandBlockSpoolReader::readHeaderLocked() {
    FileHeaderV1 header;
    input_.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!input_ || header.magic != kSpoolMagic ||
        (header.version != kLegacyHeaderVersion && header.version != kHeaderVersion)) {
        return false;
    }
    version_ = header.version;
    record_count_ = header.record_count;
    data_bytes_ = header.data_bytes;
    data_start_ = input_.tellg();
    if (data_start_ < std::streampos(0)) return false;
    // A finite cap protects resume paths from a corrupt header claiming an
    // impossible stream length.  The parser side already applies a per-record
    // cap, so this only needs to reject signed/seek overflows.
    if (data_bytes_ > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    // Do not trust a header from a truncated spool, nor silently accept a
    // stale manifest paired with a newer body that happens to have the same
    // record count.  Checking the exact physical body length also handles the
    // zero-record case where no call to next() would otherwise validate it.
    input_.seekg(0, std::ios::end);
    const std::streampos file_end = input_.tellg();
    if (file_end < data_start_ ||
        static_cast<uint64_t>(file_end - data_start_) != data_bytes_) {
        return false;
    }
    input_.seekg(data_start_, std::ios::beg);
    if (!input_) return false;
    consumed_bytes_ = 0;
    cursor_ = 0;
    valid_ = true;
    return true;
}

bool CommandBlockSpoolReader::readBytesLocked(void* destination, size_t count) {
    if (count > data_bytes_ - consumed_bytes_) {
        failLocked();
        return false;
    }
    input_.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(count));
    if (!input_) {
        failLocked();
        return false;
    }
    consumed_bytes_ += count;
    return true;
}

bool CommandBlockSpoolReader::skipBytesLocked(uint64_t count) {
    if (count > data_bytes_ - consumed_bytes_ ||
        count > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        failLocked();
        return false;
    }
    input_.seekg(static_cast<std::streamoff>(count), std::ios::cur);
    if (!input_) {
        failLocked();
        return false;
    }
    consumed_bytes_ += count;
    return true;
}

bool CommandBlockSpoolReader::readRecordLocked(CommandBlockRecord* record) {
    if (!record || cursor_ >= record_count_) return false;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int32_t tick_delay = 0;
    uint16_t mode = 0;
    uint8_t flags = 0;
    uint8_t reserved = 0;
    std::array<uint32_t, 4> lengths{};
    if (version_ == kLegacyHeaderVersion) {
        RecordHeaderV1 header;
        if (!readBytesLocked(&header, sizeof(header))) return false;
        x = header.x;
        y = header.y;
        z = header.z;
        mode = header.mode;
        flags = header.flags;
        reserved = header.reserved;
        lengths = {{header.command_bytes, header.last_output_bytes,
                    header.name_bytes, header.filtered_name_bytes}};
    } else if (version_ == kHeaderVersion) {
        RecordHeaderV2 header;
        if (!readBytesLocked(&header, sizeof(header))) return false;
        x = header.x;
        y = header.y;
        z = header.z;
        mode = header.mode;
        flags = header.flags;
        reserved = header.reserved;
        tick_delay = header.tick_delay;
        lengths = {{header.command_bytes, header.last_output_bytes,
                    header.name_bytes, header.filtered_name_bytes}};
    } else {
        failLocked();
        return false;
    }
    if (reserved != 0 || !isValidCommandBlockMode(mode) || (flags & ~kKnownFlags) != 0) {
        failLocked();
        return false;
    }
    uint64_t strings_bytes = 0;
    for (const uint32_t length : lengths) {
        if (length > CommandBlockSpoolWriter::kMaximumStringBytes ||
            strings_bytes > CommandBlockSpoolWriter::kMaximumRecordBytes - length) {
            failLocked();
            return false;
        }
        strings_bytes += length;
    }
    if (strings_bytes > data_bytes_ - consumed_bytes_) {
        failLocked();
        return false;
    }
    record->x = x;
    record->y = y;
    record->z = z;
    record->mode = mode;
    record->redstone_mode = (flags & kFlagRedstoneMode) != 0;
    record->conditional = (flags & kFlagConditional) != 0;
    record->output_tracked = (flags & kFlagOutputTracked) != 0;
    record->tick_delay = tick_delay;
    record->executing_on_first_tick = (flags & kFlagExecutingOnFirstTick) != 0;
    const auto read_text = [&](uint32_t length, std::string* output) -> bool {
        output->assign(length, '\0');
        return length == 0 || readBytesLocked(&(*output)[0], length);
    };
    if (!read_text(lengths[0], &record->command) ||
        !read_text(lengths[1], &record->last_output) ||
        !read_text(lengths[2], &record->name) ||
        !read_text(lengths[3], &record->filtered_name)) {
        return false;
    }
    ++cursor_;
    if (cursor_ == record_count_ && consumed_bytes_ != data_bytes_) {
        failLocked();
        return false;
    }
    return true;
}

bool CommandBlockSpoolReader::skipRecordLocked() {
    if (cursor_ >= record_count_) return false;
    uint16_t mode = 0;
    uint8_t flags = 0;
    uint8_t reserved = 0;
    std::array<uint32_t, 4> lengths{};
    if (version_ == kLegacyHeaderVersion) {
        RecordHeaderV1 header;
        if (!readBytesLocked(&header, sizeof(header))) return false;
        mode = header.mode;
        flags = header.flags;
        reserved = header.reserved;
        lengths = {{header.command_bytes, header.last_output_bytes,
                    header.name_bytes, header.filtered_name_bytes}};
    } else if (version_ == kHeaderVersion) {
        RecordHeaderV2 header;
        if (!readBytesLocked(&header, sizeof(header))) return false;
        mode = header.mode;
        flags = header.flags;
        reserved = header.reserved;
        lengths = {{header.command_bytes, header.last_output_bytes,
                    header.name_bytes, header.filtered_name_bytes}};
    } else {
        failLocked();
        return false;
    }
    if (reserved != 0 || !isValidCommandBlockMode(mode) || (flags & ~kKnownFlags) != 0) {
        failLocked();
        return false;
    }
    uint64_t strings_bytes = 0;
    for (const uint32_t length : lengths) {
        if (length > CommandBlockSpoolWriter::kMaximumStringBytes ||
            strings_bytes > CommandBlockSpoolWriter::kMaximumRecordBytes - length) {
            failLocked();
            return false;
        }
        strings_bytes += length;
    }
    if (!skipBytesLocked(strings_bytes)) return false;
    ++cursor_;
    if (cursor_ == record_count_ && consumed_bytes_ != data_bytes_) {
        failLocked();
        return false;
    }
    return true;
}

bool CommandBlockSpoolReader::resetLocked() {
    if (!input_.is_open()) return false;
    input_.clear();
    input_.seekg(data_start_, std::ios::beg);
    if (!input_) return false;
    cursor_ = 0;
    consumed_bytes_ = 0;
    valid_ = true;
    failed_ = false;
    return true;
}

bool CommandBlockSpoolReader::valid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return valid_ && !failed_;
}

bool CommandBlockSpoolReader::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

uint64_t CommandBlockSpoolReader::recordCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return record_count_;
}

uint64_t CommandBlockSpoolReader::dataBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_bytes_;
}

uint64_t CommandBlockSpoolReader::cursor() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cursor_;
}

bool CommandBlockSpoolReader::seekRecord(uint64_t record_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_ || failed_ || record_index > record_count_ || !resetLocked()) return false;
    while (cursor_ < record_index) {
        if (!skipRecordLocked()) return false;
    }
    return true;
}

std::optional<CommandBlockRecord> CommandBlockSpoolReader::next() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_ || failed_ || cursor_ >= record_count_) return std::nullopt;
    CommandBlockRecord record;
    if (!readRecordLocked(&record)) return std::nullopt;
    return record;
}

}  // namespace build_import
