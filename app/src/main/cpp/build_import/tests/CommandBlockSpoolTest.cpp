#include "../CommandBlockSpool.h"
#include "../CommandBlockWriteGeometry.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace build_import;

namespace {
namespace fs = std::filesystem;

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            path_ = fs::temp_directory_path() /
                ("command_block_spool_test_" + std::to_string(token) + "_" +
                 std::to_string(attempt));
            std::error_code error;
            if (fs::create_directory(path_, error)) return;
        }
        assert(false && "cannot create command-block spool test directory");
    }

    ~ScopedTempDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    std::string string() const { return path_.string(); }

private:
    fs::path path_;
};

CommandBlockRecord recordAt(int32_t x, int32_t y, int32_t z, uint16_t mode,
                             std::string command) {
    CommandBlockRecord record;
    record.x = x;
    record.y = y;
    record.z = z;
    record.mode = mode;
    record.redstone_mode = false;
    record.conditional = true;
    record.command = std::move(command);
    record.last_output = "last output";
    record.name = "name";
    record.filtered_name = "filtered";
    record.output_tracked = true;
    record.tick_delay = 7;
    record.executing_on_first_tick = true;
    return record;
}

// V1 is reproduced only for this compatibility test. The production reader
// accepts it so a paused import from the earlier sidecar format can resume.
struct LegacyFileHeader {
    uint32_t magic = 0x31534243U;
    uint32_t version = 1U;
    uint64_t record_count = 0U;
    uint64_t data_bytes = 0U;
};

struct LegacyRecordHeader {
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

static_assert(sizeof(LegacyFileHeader) == 24, "unexpected V1 spool header layout");
static_assert(sizeof(LegacyRecordHeader) == 32, "unexpected V1 spool record layout");

void writeLegacyV1Record(const fs::path& path) {
    const std::string command = "say legacy";
    const std::string last_output = "legacy output";
    const std::string name = "legacy name";
    const std::string filtered_name = "legacy filtered";
    const LegacyRecordHeader record{
        -12, 70, 34, kCommandBlockModeRepeat,
        static_cast<uint8_t>(0x02U | 0x04U), 0U,
        static_cast<uint32_t>(command.size()),
        static_cast<uint32_t>(last_output.size()),
        static_cast<uint32_t>(name.size()),
        static_cast<uint32_t>(filtered_name.size()),
    };
    const uint64_t data_bytes = sizeof(record) + command.size() + last_output.size() +
        name.size() + filtered_name.size();
    const LegacyFileHeader header{0x31534243U, 1U, 1U, data_bytes};
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&record), sizeof(record));
    output.write(command.data(), static_cast<std::streamsize>(command.size()));
    output.write(last_output.data(), static_cast<std::streamsize>(last_output.size()));
    output.write(name.data(), static_cast<std::streamsize>(name.size()));
    output.write(filtered_name.data(), static_cast<std::streamsize>(filtered_name.size()));
    assert(output);
}

void testRoundTripAndSeek() {
    const ScopedTempDirectory temporary;
    CommandBlockSpoolWriter writer(temporary.string());
    assert(writer.append(recordAt(10, 64, -3, 0, "say first")));
    assert(writer.append(recordAt(-7, 80, 11, 2, "say second")));
    CommandBlockSpoolManifest manifest;
    assert(writer.finish(&manifest));
    assert(manifest.isValid());
    assert(manifest.record_count == 2);
    assert(manifest.bounds.min_x == -7 && manifest.bounds.max_x == 10);
    assert(manifest.bounds.min_y == 64 && manifest.bounds.max_y == 80);
    assert(manifest.bounds.min_z == -3 && manifest.bounds.max_z == 11);

    CommandBlockSpoolManifest reread_manifest;
    assert(CommandBlockSpoolWriter::readManifest(manifest.manifest_path, &reread_manifest));
    assert(reread_manifest.record_count == 2);
    assert(reread_manifest.data_bytes == manifest.data_bytes);

    CommandBlockSpoolReader reader(manifest.spool_path);
    assert(reader.valid());
    assert(reader.recordCount() == 2);
    const auto first = reader.next();
    assert(first.has_value());
    assert(first->x == 10 && first->y == 64 && first->z == -3);
    assert(first->mode == 0 && first->command == "say first");
    assert(first->last_output == "last output" && first->name == "name");
    assert(first->filtered_name == "filtered");
    assert(!first->redstone_mode && first->conditional && first->output_tracked);
    assert(first->tick_delay == 7 && first->executing_on_first_tick);
    assert(reader.seekRecord(1));
    const auto second = reader.next();
    assert(second.has_value());
    assert(second->x == -7 && second->y == 80 && second->z == 11);
    assert(second->mode == 2 && second->command == "say second");
    assert(!reader.next().has_value());
    assert(!reader.failed());
}

void testConcurrentAppend() {
    const ScopedTempDirectory temporary;
    CommandBlockSpoolWriter writer(temporary.string());
    constexpr int kThreads = 4;
    constexpr int kRecordsPerThread = 32;
    std::vector<std::thread> threads;
    for (int thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([&writer, thread] {
            for (int index = 0; index < kRecordsPerThread; ++index) {
                std::string error;
                assert(writer.append(recordAt(thread * 100 + index, 64, thread,
                                              static_cast<uint16_t>(index % 3),
                                              "say threaded"), &error));
                assert(error.empty());
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    CommandBlockSpoolManifest manifest;
    assert(writer.finish(&manifest));
    assert(manifest.record_count == static_cast<uint64_t>(kThreads * kRecordsPerThread));
    CommandBlockSpoolReader reader(manifest.spool_path);
    assert(reader.valid());
    uint64_t count = 0;
    while (reader.next().has_value()) ++count;
    assert(count == manifest.record_count);
    assert(!reader.failed());
}

void testLegacyV1DefaultsTickDelay() {
    const ScopedTempDirectory temporary;
    const fs::path path = fs::path(temporary.string()) / "legacy.cbs";
    writeLegacyV1Record(path);

    CommandBlockSpoolReader reader(path.string());
    assert(reader.valid());
    assert(reader.recordCount() == 1U);
    const auto record = reader.next();
    assert(record.has_value());
    assert(record->x == -12 && record->y == 70 && record->z == 34);
    assert(record->mode == kCommandBlockModeRepeat);
    assert(!record->redstone_mode && record->conditional && record->output_tracked);
    assert(record->tick_delay == 0);
    assert(record->command == "say legacy");
    assert(!reader.next().has_value());
    assert(!reader.failed());
}

void testShellStateKeepsRedstoneAndSynchronizesConditional() {
    CommandBlockRecord record;
    // `false` is the packet representation of Bedrock's auto/keep-on mode.
    // The shell helper must not touch it while it restores the type and the
    // conditional bit from the block that was actually placed.
    record.redstone_mode = false;
    record.conditional = false;
    applyCommandBlockShellState(&record,
                                commandBlockShellState(kCommandBlockModeChain, 0x0BU));
    assert(record.mode == kCommandBlockModeChain);
    assert(record.conditional);
    assert(!record.redstone_mode);

    record.redstone_mode = true;
    applyCommandBlockShellState(&record,
                                commandBlockShellState(kCommandBlockModeRepeat, 0x03U));
    assert(record.mode == kCommandBlockModeRepeat);
    assert(!record.conditional);
    assert(record.redstone_mode);
}

void testWriteTeleportAnchorAndSafeRadius() {
    const CommandBlockRecord record = recordAt(-17, 70, 29, kCommandBlockModeChain,
                                                "say anchored");
    const CommandBlockWriteTeleportTarget target = commandBlockWriteTeleportTarget(record);
    // The write phase must target the actual command block, not the centre of
    // the containing grid cell. That keeps sparse/edge records editable.
    assert(target.x == -17);
    assert(target.y == 72);
    assert(target.z == 29);

    assert(isWithinCommandBlockEditorSafeRadius(-17, 72, 29, record));
    assert(isWithinCommandBlockEditorSafeRadius(-11, 70, 29, record));
    assert(!isWithinCommandBlockEditorSafeRadius(-10, 70, 29, record));
    assert(!isWithinCommandBlockEditorSafeRadius(
        std::numeric_limits<int32_t>::max(), 70, 29, record));

    CommandBlockRecord high = record;
    high.y = std::numeric_limits<int32_t>::max();
    assert(commandBlockWriteTeleportTarget(high).y == std::numeric_limits<int32_t>::max());
}

void testCorruptRecordHeaderFailsReader() {
    const ScopedTempDirectory temporary;
    CommandBlockSpoolWriter writer(temporary.string());
    assert(writer.append(recordAt(2, 65, 3, kCommandBlockModeImpulse, "say valid")));
    CommandBlockSpoolManifest manifest;
    assert(writer.finish(&manifest));

    // The mode is after the fixed file header and three int32 coordinates.
    // Preserve the file length so this exercises record validation itself.
    std::fstream file(manifest.spool_path, std::ios::binary | std::ios::in | std::ios::out);
    assert(file);
    const std::streamoff mode_offset = static_cast<std::streamoff>(sizeof(LegacyFileHeader) +
        sizeof(int32_t) * 3U);
    file.seekp(mode_offset, std::ios::beg);
    const uint16_t invalid_mode = 9U;
    file.write(reinterpret_cast<const char*>(&invalid_mode), sizeof(invalid_mode));
    file.close();

    CommandBlockSpoolReader reader(manifest.spool_path);
    assert(reader.valid());
    assert(!reader.next().has_value());
    assert(reader.failed());
}

}  // namespace

int main() {
    testRoundTripAndSeek();
    testConcurrentAppend();
    testLegacyV1DefaultsTickDelay();
    testShellStateKeepsRedstoneAndSynchronizesConditional();
    testWriteTeleportAnchorAndSafeRadius();
    testCorruptRecordHeaderFailsReader();
    return 0;
}
