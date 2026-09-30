#ifndef INFINITE_TEXTURE_COMMAND_BLOCK_SPOOL_H
#define INFINITE_TEXTURE_COMMAND_BLOCK_SPOOL_H

#include "BuildImportTypes.h"

#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

namespace build_import {

constexpr uint16_t kCommandBlockModeImpulse = 0;
constexpr uint16_t kCommandBlockModeRepeat = 1;
constexpr uint16_t kCommandBlockModeChain = 2;

inline bool isValidCommandBlockMode(uint16_t mode) {
    return mode <= kCommandBlockModeChain;
}

// The command-block shell is placed by the normal block-import pipeline while
// its editable data follows later through CommandBlockUpdatePacket.  Keep the
// two pieces of source state together so the packet cannot accidentally reset
// a conditional shell when a legacy block-entity NBT omits conditionalMode.
struct CommandBlockShellState {
    uint16_t mode = kCommandBlockModeImpulse;
    bool conditional = false;
};

inline CommandBlockShellState commandBlockShellState(uint16_t mode, uint16_t aux) {
    return {mode, (aux & 0x08U) != 0U};
}

// A deferred command-block update.  Parsers only preserve this source data;
// applying it to a loaded world is intentionally owned by the import runtime.
// Keeping it independent from RawSpoolRecordV2 prevents arbitrary block-entity
// NBT from entering the normal /fill command pipeline.
struct CommandBlockRecord {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint16_t mode = 0;
    bool redstone_mode = true;
    bool conditional = false;
    std::string command;
    std::string last_output;
    std::string name;
    // The current CommandBlockUpdatePacket has a second filtered-name string.
    // Blueprint formats do not normally contain it, so parsers leave it empty.
    std::string filtered_name;
    bool output_tracked = false;
    int32_t tick_delay = 0;
    bool executing_on_first_tick = false;
};

inline void applyCommandBlockShellState(CommandBlockRecord* record,
                                        const CommandBlockShellState& shell) {
    if (!record) return;
    record->mode = shell.mode;
    record->conditional = shell.conditional;
}

// The manifest is deliberately small and serializable.  A checkpoint/runtime
// can retain only this descriptor plus a record cursor without holding every
// source command in RAM.
struct CommandBlockSpoolManifest {
    std::string spool_path;
    std::string manifest_path;
    uint64_t record_count = 0;
    uint64_t data_bytes = 0;
    BlockBounds bounds;

    bool isValid() const { return !spool_path.empty() && !manifest_path.empty(); }
};

class CommandBlockSpoolWriter {
public:
    static constexpr const char* kFileName = "command_blocks.cbs";
    static constexpr const char* kManifestFileName = "command_blocks.cbs.manifest";
    // V2 adds the command block's tick delay to each deferred record.  The
    // reader still accepts V1 so a paused import created by an earlier build
    // can safely resume (with V1's historical/default zero delay).
    static constexpr uint32_t kVersion = 2;
    static constexpr uint32_t kMaximumStringBytes = 1024U * 1024U;
    static constexpr uint64_t kMaximumRecordBytes =
        static_cast<uint64_t>(kMaximumStringBytes) * 4U + 36U;

    explicit CommandBlockSpoolWriter(std::string directory,
                                     std::string file_name = kFileName);
    ~CommandBlockSpoolWriter();

    CommandBlockSpoolWriter(const CommandBlockSpoolWriter&) = delete;
    CommandBlockSpoolWriter& operator=(const CommandBlockSpoolWriter&) = delete;

    // Safe to call from parser callbacks on multiple threads.  The writer
    // preserves append order under its mutex.
    bool append(const CommandBlockRecord& record, std::string* error = nullptr);
    bool finish(CommandBlockSpoolManifest* manifest, std::string* error = nullptr);
    void discard();

    bool failed() const;
    uint64_t recordCount() const;

    static bool readManifest(const std::string& manifest_path,
                             CommandBlockSpoolManifest* manifest,
                             std::string* error = nullptr);

private:
    bool ensureOpenLocked(std::string* error);
    bool writeManifestLocked(const CommandBlockSpoolManifest& manifest,
                             std::string* error);
    void discardLocked(bool remove_final);

    mutable std::mutex mutex_;
    std::string directory_;
    std::string spool_path_;
    std::string temporary_path_;
    std::string manifest_path_;
    std::string temporary_manifest_path_;
    std::ofstream output_;
    uint64_t record_count_ = 0;
    uint64_t data_bytes_ = 0;
    BlockBounds bounds;
    bool opened_ = false;
    bool finished_ = false;
    bool failed_ = false;
};

class CommandBlockSpoolReader {
public:
    static constexpr const char* kFileName = CommandBlockSpoolWriter::kFileName;

    explicit CommandBlockSpoolReader(std::string path);
    ~CommandBlockSpoolReader() = default;

    CommandBlockSpoolReader(const CommandBlockSpoolReader&) = delete;
    CommandBlockSpoolReader& operator=(const CommandBlockSpoolReader&) = delete;

    bool valid() const;
    bool failed() const;
    uint64_t recordCount() const;
    uint64_t dataBytes() const;
    uint64_t cursor() const;
    // Variable-size records make seeking a bounded linear scan.  It is meant
    // for resume (once per job), not random-access iteration.
    bool seekRecord(uint64_t record_index);
    std::optional<CommandBlockRecord> next();

private:
    bool readHeaderLocked();
    bool readRecordLocked(CommandBlockRecord* record);
    bool skipRecordLocked();
    bool readBytesLocked(void* destination, size_t count);
    bool skipBytesLocked(uint64_t count);
    bool resetLocked();
    void failLocked();

    mutable std::mutex mutex_;
    std::string path_;
    std::ifstream input_;
    uint64_t record_count_ = 0;
    uint64_t data_bytes_ = 0;
    uint64_t consumed_bytes_ = 0;
    uint64_t cursor_ = 0;
    std::streampos data_start_{};
    uint32_t version_ = 0;
    bool valid_ = false;
    bool failed_ = false;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_COMMAND_BLOCK_SPOOL_H
