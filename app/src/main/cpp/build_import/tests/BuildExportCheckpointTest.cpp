#include "BuildExportCheckpoint.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace build_import;

namespace {

template <typename T>
bool writeValue(std::FILE* file, const T& value) {
    return file && std::fwrite(&value, sizeof(T), 1, file) == 1;
}

bool writeString(std::FILE* file, const std::string& value) {
    const uint32_t size = static_cast<uint32_t>(value.size());
    return writeValue(file, size) &&
        (size == 0 || std::fwrite(value.data(), 1, size, file) == size);
}

bool writeCommandBlock(std::FILE* file, const CommandBlockRecord& record) {
    constexpr uint8_t redstone_mode = 1U << 0U;
    constexpr uint8_t conditional = 1U << 1U;
    constexpr uint8_t output_tracked = 1U << 2U;
    constexpr uint8_t executing_on_first_tick = 1U << 3U;
    uint8_t flags = 0;
    if (record.redstone_mode) flags |= redstone_mode;
    if (record.conditional) flags |= conditional;
    if (record.output_tracked) flags |= output_tracked;
    if (record.executing_on_first_tick) flags |= executing_on_first_tick;
    return writeValue(file, record.x) && writeValue(file, record.y) &&
        writeValue(file, record.z) && writeValue(file, record.mode) &&
        writeValue(file, flags) && writeValue(file, record.tick_delay) &&
        writeString(file, record.command) && writeString(file, record.last_output) &&
        writeString(file, record.name) && writeString(file, record.filtered_name);
}

bool writeRawBlock(std::FILE* file, const SchematicRawBlock& record) {
    return writeValue(file, record.x) && writeValue(file, record.y) &&
        writeValue(file, record.z) && writeValue(file, record.aux) &&
        writeValue(file, record.legacy_id) && writeString(file, record.identifier) &&
        writeString(file, record.state_json) && writeString(file, record.entity_json);
}

BuildExportCheckpointSnapshot makeSnapshot(const std::string& output_path) {
    BuildExportCheckpointSnapshot snapshot;
    snapshot.output_path = output_path;
    snapshot.world_id =
        "stable:v1:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef|0";
    snapshot.dimension_id = 0;
    snapshot.min_x = 10;
    snapshot.min_y = 20;
    snapshot.min_z = 30;
    snapshot.max_x = 12;
    snapshot.max_y = 20;
    snapshot.max_z = 30;
    snapshot.batch_size = 64;
    snapshot.total_blocks = 3;
    snapshot.journal_entries = 3;
    snapshot.exported_blocks = 2;
    snapshot.current_batch = 0;
    snapshot.batch_cursor = 3;
    snapshot.writing = false;
    snapshot.palette = {"minecraft:air", "minecraft:stone"};
    return snapshot;
}

bool writeLegacyV1Checkpoint(const std::string& output_path,
                             const BuildExportCheckpointSnapshot& snapshot) {
    std::FILE* file = std::fopen(
        BuildExportCheckpoint::checkpointPath(output_path).c_str(), "wb");
    if (!file) return false;
    constexpr uint32_t magic = 0x50584542;
    constexpr uint32_t version = 1;
    const uint8_t writing = snapshot.writing ? 1 : 0;
    const uint32_t palette_size = static_cast<uint32_t>(snapshot.palette.size());
    bool ok = writeValue(file, magic) && writeValue(file, version) &&
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
    for (const std::string& state : snapshot.palette) {
        if (ok) ok = writeString(file, state);
    }
    const bool closed = std::fclose(file) == 0;
    return ok && closed;
}

bool writeLegacyV2Checkpoint(const std::string& output_path,
                             const BuildExportCheckpointSnapshot& snapshot,
                             const CommandBlockRecord& record) {
    std::FILE* file = std::fopen(
        BuildExportCheckpoint::checkpointPath(output_path).c_str(), "wb");
    if (!file) return false;
    constexpr uint32_t magic = 0x50584542;
    constexpr uint32_t version = 2;
    const uint8_t writing = snapshot.writing ? 1 : 0;
    const uint32_t palette_size = static_cast<uint32_t>(snapshot.palette.size());
    bool ok = writeValue(file, magic) && writeValue(file, version) &&
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
    for (const std::string& state : snapshot.palette) {
        if (ok) ok = writeString(file, state);
    }
    const uint64_t count = 1;
    ok = ok && writeValue(file, count) && writeCommandBlock(file, record);
    const bool closed = std::fclose(file) == 0;
    return ok && closed;
}

bool writeLegacyV3Checkpoint(const std::string& output_path,
                             const BuildExportCheckpointSnapshot& snapshot,
                             const CommandBlockRecord& command,
                             const SchematicRawBlock& raw_block) {
    std::FILE* file = std::fopen(
        BuildExportCheckpoint::checkpointPath(output_path).c_str(), "wb");
    if (!file) return false;
    constexpr uint32_t magic = 0x50584542;
    constexpr uint32_t version = 3;
    const uint8_t writing = snapshot.writing ? 1 : 0;
    const uint32_t palette_size = static_cast<uint32_t>(snapshot.palette.size());
    bool ok = writeValue(file, magic) && writeValue(file, version) &&
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
    for (const std::string& state : snapshot.palette) {
        if (ok) ok = writeString(file, state);
    }
    constexpr uint64_t count = 1;
    ok = ok && writeValue(file, count) && writeCommandBlock(file, command) &&
        writeValue(file, count) && writeRawBlock(file, raw_block);
    const bool closed = std::fclose(file) == 0;
    return ok && closed;
}

bool writeLegacyV4Checkpoint(const std::string& output_path,
                             const BuildExportCheckpointSnapshot& snapshot,
                             const CommandBlockRecord& command,
                             const SchematicRawBlock& raw_block) {
    std::FILE* file = std::fopen(
        BuildExportCheckpoint::checkpointPath(output_path).c_str(), "wb");
    if (!file) return false;
    constexpr uint32_t magic = 0x50584542;
    constexpr uint32_t version = 4;
    const uint8_t writing = snapshot.writing ? 1 : 0;
    const uint32_t palette_size = static_cast<uint32_t>(snapshot.palette.size());
    const uint8_t capture_pending = snapshot.container_capture_pending ? 1U : 0U;
    bool ok = writeValue(file, magic) && writeValue(file, version) &&
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
    for (const std::string& state : snapshot.palette) {
        if (ok) ok = writeString(file, state);
    }
    constexpr uint64_t count = 1;
    ok = ok && writeValue(file, count) && writeCommandBlock(file, command) &&
        writeValue(file, count) && writeRawBlock(file, raw_block) &&
        writeValue(file, capture_pending) &&
        writeValue(file, snapshot.container_target_count) &&
        writeValue(file, snapshot.container_cursor);
    const bool closed = std::fclose(file) == 0;
    return ok && closed;
}

}  // namespace

int main() {
    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "infinitecz_export_checkpoint_test.schem";
    const std::string output_path = output.string();
    BuildExportCheckpoint::discard(output_path, nullptr);

    assert(BuildExportCheckpoint::createJournal(output_path));
    const uint16_t initial[] = {0, 65'535, 1};
    assert(BuildExportCheckpoint::appendJournal(output_path, initial, 3));
    BuildExportCheckpointSnapshot snapshot = makeSnapshot(output_path);
    CommandBlockRecord command_block;
    command_block.x = 1;
    command_block.mode = kCommandBlockModeChain;
    command_block.redstone_mode = false;
    command_block.conditional = true;
    command_block.command = "execute as @a run say checkpoint";
    command_block.last_output = "\xE5\xB7\xB2\xE6\x81\xA2\xE5\xA4\x8D";
    command_block.name = "checkpoint command";
    command_block.filtered_name = "filtered command";
    command_block.output_tracked = true;
    command_block.tick_delay = -12;
    command_block.executing_on_first_tick = true;
    snapshot.command_blocks.push_back(command_block);
    SchematicRawBlock raw_block;
    raw_block.x = 2;
    raw_block.y = 0;
    raw_block.z = 0;
    raw_block.identifier = "minecraft:redstone_torch";
    raw_block.aux = 4;
    raw_block.legacy_id = 76;
    raw_block.state_json = "{\"facing\":\"north\"}";
    raw_block.entity_json = "{\"id\":\"infinitecz:test\"}";
    snapshot.raw_blocks.push_back(raw_block);
    snapshot.container_capture_pending = true;
    snapshot.container_target_count = 1;
    snapshot.container_cursor = 1;
    snapshot.palette.resize(65'536, "minecraft:stone");
    for (size_t index = 2; index < snapshot.palette.size(); ++index) {
        snapshot.palette[index] = "infinitecz:test_" + std::to_string(index);
    }
    assert(BuildExportCheckpoint::saveAtomically(output_path, snapshot));
    assert(BuildExportCheckpoint::hasArtifacts(output_path));

    auto loaded = BuildExportCheckpoint::load(output_path);
    assert(loaded.has_value());
    assert(loaded->output_path == output_path);
    assert(loaded->world_id == snapshot.world_id);
    assert(loaded->format_version == kBuildExportCheckpointVersion);
    // 64 is the legacy/default four-chunk scan width. Existing checkpoints
    // must remain resumable after the selectable simulation-range change.
    assert(loaded->batch_size == 64);
    assert(loaded->journal_entries == 3);
    assert(loaded->palette.size() == 65'536);
    assert(loaded->palette.back() == "infinitecz:test_65535");
    assert(loaded->command_blocks.size() == 1);
    const CommandBlockRecord& restored_command = loaded->command_blocks.front();
    assert(restored_command.x == 1);
    assert(restored_command.mode == kCommandBlockModeChain);
    assert(!restored_command.redstone_mode);
    assert(restored_command.conditional);
    assert(restored_command.command == command_block.command);
    assert(restored_command.last_output == command_block.last_output);
    assert(restored_command.name == command_block.name);
    assert(restored_command.filtered_name == command_block.filtered_name);
    assert(restored_command.output_tracked);
    assert(restored_command.tick_delay == -12);
    assert(restored_command.executing_on_first_tick);
    assert(loaded->raw_blocks.size() == 1);
    const SchematicRawBlock& restored_raw = loaded->raw_blocks.front();
    assert(restored_raw.x == raw_block.x && restored_raw.y == raw_block.y &&
           restored_raw.z == raw_block.z);
    assert(restored_raw.identifier == raw_block.identifier);
    assert(restored_raw.aux == raw_block.aux && restored_raw.legacy_id == raw_block.legacy_id);
    assert(restored_raw.state_json == raw_block.state_json);
    assert(restored_raw.entity_json == raw_block.entity_json);
    assert(loaded->container_capture_pending);
    assert(loaded->export_container_items);
    assert(loaded->container_target_count == 1);
    assert(loaded->container_cursor == 1);

    // A process can die after appending entries but before publishing the next
    // checkpoint. The committed prefix remains loadable and the tail is safely
    // truncated during recovery.
    const uint16_t uncommitted_tail[] = {7, 8};
    assert(BuildExportCheckpoint::appendJournal(output_path, uncommitted_tail, 2));
    loaded = BuildExportCheckpoint::load(output_path);
    assert(loaded.has_value());
    assert(BuildExportCheckpoint::truncateJournal(output_path, loaded->journal_entries));
    assert(BuildExportCheckpoint::journalHasEntries(output_path, 3));
    assert(!BuildExportCheckpoint::journalHasEntries(output_path, 4));

    // A checkpoint may never authorize more entries than the durable journal.
    assert(BuildExportCheckpoint::truncateJournal(output_path, 2));
    assert(!BuildExportCheckpoint::load(output_path).has_value());

    assert(BuildExportCheckpoint::createJournal(output_path));
    assert(BuildExportCheckpoint::appendJournal(output_path, initial, 3));
    // New exports preserve their selected 8-chunk (128-block) batch width in
    // the existing on-disk batch_size field without changing the format.
    snapshot.batch_size = 128;
    assert(BuildExportCheckpoint::saveAtomically(output_path, snapshot));
    loaded = BuildExportCheckpoint::load(output_path);
    assert(loaded.has_value());
    assert(loaded->batch_size == 128);
    {
        std::ofstream corrupt(BuildExportCheckpoint::checkpointPath(output_path),
                              std::ios::binary | std::ios::app);
        corrupt.put('\x7f');
    }
    assert(!BuildExportCheckpoint::load(output_path).has_value());

    assert(BuildExportCheckpoint::discard(output_path));
    assert(BuildExportCheckpoint::createJournal(output_path));
    BuildExportCheckpointSnapshot maximum = makeSnapshot(output_path);
    maximum.min_x = 0;
    maximum.min_y = 0;
    maximum.min_z = 0;
    maximum.max_x = 4095;
    maximum.max_y = 255;
    maximum.max_z = 15;
    maximum.total_blocks = 16ULL * 1024ULL * 1024ULL;
    maximum.journal_entries = 0;
    maximum.exported_blocks = 0;
    maximum.current_batch = 0;
    maximum.batch_cursor = 0;
    maximum.palette = {"minecraft:air"};
    maximum.command_blocks.clear();
    maximum.raw_blocks.clear();
    assert(BuildExportCheckpoint::saveAtomically(output_path, maximum));
    loaded = BuildExportCheckpoint::load(output_path);
    assert(loaded.has_value());
    assert(loaded->total_blocks == 16ULL * 1024ULL * 1024ULL);

    // V5 persists the block-only option. A resumed export must not turn it
    // back on and start opening containers after the user disabled it.
    maximum.export_container_items = false;
    assert(BuildExportCheckpoint::saveAtomically(output_path, maximum));
    loaded = BuildExportCheckpoint::load(output_path);
    assert(loaded.has_value());
    assert(!loaded->export_container_items);

    maximum.max_z = 16;
    maximum.total_blocks += 4096ULL * 256ULL;
    assert(!BuildExportCheckpoint::saveAtomically(output_path, maximum));

    assert(BuildExportCheckpoint::discard(output_path));
    assert(!BuildExportCheckpoint::hasArtifacts(output_path));

    // V1 checkpoints did not carry command-block entity data. They remain
    // loadable so the runtime can keep their old extension-bearing sidecar key
    // while migrating subsequent commits to the current format.
    const std::filesystem::path legacy_output =
        std::filesystem::temp_directory_path() / "infinitecz_export_legacy.schem";
    const std::string legacy_path = legacy_output.string();
    BuildExportCheckpoint::discard(legacy_path, nullptr);
    assert(BuildExportCheckpoint::createJournal(legacy_path));
    assert(BuildExportCheckpoint::appendJournal(legacy_path, initial, 3));
    BuildExportCheckpointSnapshot legacy = makeSnapshot(legacy_path);
    legacy.format_version = 1;
    assert(writeLegacyV1Checkpoint(legacy_path, legacy));
    loaded = BuildExportCheckpoint::load(legacy_path);
    assert(loaded.has_value());
    assert(loaded->format_version == 1);
    assert(loaded->command_blocks.empty());
    assert(!loaded->container_capture_pending);
    assert(loaded->export_container_items);
    assert(loaded->container_target_count == 0);
    assert(loaded->container_cursor == 0);
    assert(BuildExportCheckpoint::discard(legacy_path));

    const std::filesystem::path legacy_v2_output =
        std::filesystem::temp_directory_path() / "infinitecz_export_legacy_v2.schem";
    const std::string legacy_v2_path = legacy_v2_output.string();
    BuildExportCheckpoint::discard(legacy_v2_path, nullptr);
    assert(BuildExportCheckpoint::createJournal(legacy_v2_path));
    assert(BuildExportCheckpoint::appendJournal(legacy_v2_path, initial, 3));
    BuildExportCheckpointSnapshot legacy_v2 = makeSnapshot(legacy_v2_path);
    CommandBlockRecord legacy_v2_command;
    legacy_v2_command.x = 1;
    legacy_v2_command.mode = kCommandBlockModeRepeat;
    legacy_v2_command.command = "say legacy-v2";
    legacy_v2_command.name = "legacy-v2";
    assert(writeLegacyV2Checkpoint(legacy_v2_path, legacy_v2, legacy_v2_command));
    loaded = BuildExportCheckpoint::load(legacy_v2_path);
    assert(loaded.has_value());
    assert(loaded->format_version == 2);
    assert(loaded->raw_blocks.empty());
    assert(loaded->command_blocks.size() == 1);
    assert(loaded->command_blocks.front().command == legacy_v2_command.command);
    assert(loaded->export_container_items);
    assert(!loaded->container_capture_pending);
    assert(loaded->container_target_count == 0);
    assert(loaded->container_cursor == 0);
    assert(BuildExportCheckpoint::discard(legacy_v2_path));

    const std::filesystem::path legacy_v3_output =
        std::filesystem::temp_directory_path() / "infinitecz_export_legacy_v3.schem";
    const std::string legacy_v3_path = legacy_v3_output.string();
    BuildExportCheckpoint::discard(legacy_v3_path, nullptr);
    assert(BuildExportCheckpoint::createJournal(legacy_v3_path));
    assert(BuildExportCheckpoint::appendJournal(legacy_v3_path, initial, 3));
    BuildExportCheckpointSnapshot legacy_v3 = makeSnapshot(legacy_v3_path);
    assert(writeLegacyV3Checkpoint(legacy_v3_path, legacy_v3,
                                   legacy_v2_command, raw_block));
    loaded = BuildExportCheckpoint::load(legacy_v3_path);
    assert(loaded.has_value());
    assert(loaded->format_version == 3);
    assert(loaded->command_blocks.size() == 1);
    assert(loaded->raw_blocks.size() == 1);
    assert(loaded->export_container_items);
    assert(!loaded->container_capture_pending);
    assert(loaded->container_target_count == 0);
    assert(loaded->container_cursor == 0);
    assert(BuildExportCheckpoint::discard(legacy_v3_path));

    // V4 already persisted in-progress container capture state but did not
    // have a content-policy byte. It must retain the historical "include"
    // behavior when read by V5.
    const std::filesystem::path legacy_v4_output =
        std::filesystem::temp_directory_path() / "infinitecz_export_legacy_v4.schem";
    const std::string legacy_v4_path = legacy_v4_output.string();
    BuildExportCheckpoint::discard(legacy_v4_path, nullptr);
    assert(BuildExportCheckpoint::createJournal(legacy_v4_path));
    assert(BuildExportCheckpoint::appendJournal(legacy_v4_path, initial, 3));
    BuildExportCheckpointSnapshot legacy_v4 = makeSnapshot(legacy_v4_path);
    legacy_v4.container_capture_pending = true;
    legacy_v4.container_target_count = 1;
    legacy_v4.container_cursor = 0;
    assert(writeLegacyV4Checkpoint(legacy_v4_path, legacy_v4,
                                   legacy_v2_command, raw_block));
    loaded = BuildExportCheckpoint::load(legacy_v4_path);
    assert(loaded.has_value());
    assert(loaded->format_version == 4);
    assert(loaded->export_container_items);
    assert(loaded->container_capture_pending);
    assert(loaded->container_target_count == 1);
    assert(loaded->container_cursor == 0);
    assert(BuildExportCheckpoint::discard(legacy_v4_path));

    assert(BuildExportCheckpoint::createJournal(output_path));
    BuildExportCheckpointSnapshot invalid_container = makeSnapshot(output_path);
    invalid_container.raw_blocks.push_back(raw_block);
    invalid_container.container_capture_pending = true;
    invalid_container.container_target_count = 1;
    invalid_container.container_cursor = 2;
    assert(!BuildExportCheckpoint::saveAtomically(output_path, invalid_container));
    invalid_container.container_cursor = 0;
    invalid_container.container_target_count = 0;
    assert(!BuildExportCheckpoint::saveAtomically(output_path, invalid_container));
    invalid_container.container_capture_pending = false;
    invalid_container.container_target_count = 1;
    assert(!BuildExportCheckpoint::saveAtomically(output_path, invalid_container));
    invalid_container.container_target_count = 0;
    invalid_container.export_container_items = false;
    invalid_container.container_capture_pending = true;
    assert(!BuildExportCheckpoint::saveAtomically(output_path, invalid_container));

    BuildExportCheckpointSnapshot invalid_raw = makeSnapshot(output_path);
    SchematicRawBlock invalid_record;
    invalid_record.x = 99;
    invalid_record.identifier = "minecraft:stone";
    invalid_raw.raw_blocks.push_back(invalid_record);
    assert(!BuildExportCheckpoint::saveAtomically(output_path, invalid_raw));
    invalid_raw.raw_blocks.front().x = 0;
    invalid_raw.raw_blocks.front().state_json.assign(4U * 1024U * 1024U + 1U, 'x');
    assert(!BuildExportCheckpoint::saveAtomically(output_path, invalid_raw));
    assert(BuildExportCheckpoint::discard(output_path));
    return 0;
}
