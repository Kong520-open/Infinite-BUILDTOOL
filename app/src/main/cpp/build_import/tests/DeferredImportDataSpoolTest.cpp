#include "../DeferredImportDataSpool.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

using namespace build_import;

namespace {
namespace fs = std::filesystem;

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            path_ = fs::temp_directory_path() /
                ("deferred_import_data_spool_test_" + std::to_string(token) + "_" +
                 std::to_string(attempt));
            std::error_code error;
            if (fs::create_directory(path_, error)) return;
        }
        assert(false && "cannot create deferred import data spool test directory");
    }

    ~ScopedTempDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    std::string string() const { return path_.string(); }

private:
    fs::path path_;
};

#pragma pack(push, 1)
struct TestFileHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t record_count;
    uint64_t data_bytes;
};
#pragma pack(pop)

static_assert(sizeof(TestFileHeader) == 24U, "unexpected deferred spool test header layout");

ContainerItemRecord sampleContainerRecord() {
    ContainerItemRecord record;
    record.x = -42;
    record.y = 72;
    record.z = 19;
    record.slot = 5;
    record.count = 3;
    record.aux = 2;
    record.expected_container_id = "minecraft:chest";
    record.item_id = "minecraft:diamond_sword";
    record.enchantments = {{"minecraft:sharpness", 5}, {"minecraft:unbreaking", 3}};
    return record;
}

EntityRecord sampleEntityRecord() {
    EntityRecord record;
    record.entity_id = "minecraft:armor_stand";
    record.x = -42.25;
    record.y = 72.5;
    record.z = 19.75;
    record.yaw = 90.0F;
    record.pitch = -15.5F;
    record.custom_name = "Deferred display name";
    return record;
}

SignRecord sampleSignRecord() {
    SignRecord record;
    record.x = 123;
    record.y = -32;
    record.z = -456;
    record.expected_sign_id = "minecraft:oak_hanging_sign";
    record.has_expected_aux = true;
    record.expected_aux = 0x01DAU;
    record.front.present = true;
    record.front.has_text = true;
    record.front.text = std::string("front\n") + u8"\u4f60\u597d";
    record.front.has_text_color = true;
    record.front.text_color = -16711936;
    record.front.has_ignore_lighting = true;
    record.front.ignore_lighting = true;
    record.front.has_persist_formatting = true;
    record.front.persist_formatting = false;
    record.front.has_hide_glow_outline = true;
    record.front.hide_glow_outline = true;
    record.front.has_text_ignore_legacy_bug_resolved = true;
    record.front.text_ignore_legacy_bug_resolved = true;
    record.back.present = true;
    record.back.has_text = true;
    record.back.text = "back";
    record.back.has_text_color = true;
    record.back.text_color = -2;
    record.back.has_ignore_lighting = true;
    record.back.ignore_lighting = false;
    record.has_is_waxed = true;
    record.is_waxed = true;
    return record;
}

void testNormalizers() {
    std::string item = "Diamond_Sword";
    assert(normalizeDeferredItemIdentifier(&item));
    assert(item == "minecraft:diamond_sword");

    std::string invalid_item = "minecraft:diamond:sword";
    assert(!normalizeDeferredItemIdentifier(&invalid_item));

    std::string custom_item = "Example:Tools/Hammer";
    assert(normalizeDeferredItemIdentifier(&custom_item));
    assert(custom_item == "example:tools/hammer");

    std::string numeric_item = "276";
    assert(!normalizeDeferredItemToken(&numeric_item));

    std::string name = "name";
    name.push_back('\x01');
    name += "tag";
    assert(normalizeDeferredEntityName(&name));
    assert(name == "nametag");

    DeferredEnchantment enchantment;
    std::string enchantment_id = "Sharpness";
    assert(normalizeDeferredEnchantment(&enchantment_id, 5, &enchantment));
    assert(enchantment.id == "minecraft:sharpness" && enchantment.level == 5);
    enchantment_id = "sharpness";
    assert(!normalizeDeferredEnchantment(&enchantment_id, 6, &enchantment));

    assert(deferredContainerIdentifier("minecraft:decorated_pot"));
    assert(!packetCapturableContainerIdentifier("minecraft:decorated_pot"));
    assert(packetCapturableContainerIdentifier("minecraft:chest"));
    assert(packetCapturableContainerIdentifier("barrel"));
    assert(packetCapturableContainerIdentifier("minecraft:lit_furnace"));
    assert(packetCapturableContainerIdentifier("minecraft:lit_blast_furnace"));
    assert(packetCapturableContainerIdentifier("minecraft:lit_smoker"));
    assert(!packetCapturableContainerIdentifier("minecraft:ender_chest"));

    assert(deferredSignIdentifier("minecraft:standing_sign"));
    assert(deferredSignIdentifier("oak_wall_sign"));
    assert(deferredSignIdentifier("minecraft:dark_oak_hanging_sign"));
    assert(!deferredSignIdentifier("minecraft:chest"));
}

void testContainerRoundTripAndSeek() {
    const ScopedTempDirectory temporary;
    ContainerItemSpoolWriter writer(temporary.string());
    const ContainerItemRecord expected = sampleContainerRecord();
    assert(writer.append(expected));

    std::string spool_path;
    assert(writer.finish(&spool_path));
    assert(!spool_path.empty());

    ContainerItemSpoolReader reader(spool_path);
    assert(reader.valid());
    assert(reader.recordCount() == 1U);
    const auto first = reader.next();
    assert(first.has_value());
    assert(first->x == expected.x && first->y == expected.y && first->z == expected.z);
    assert(first->slot == expected.slot && first->count == expected.count && first->aux == expected.aux);
    assert(first->expected_container_id == expected.expected_container_id);
    assert(first->item_id == expected.item_id);
    assert(first->enchantments.size() == expected.enchantments.size());
    assert(first->enchantments[0].id == "minecraft:sharpness" &&
           first->enchantments[0].level == 5);
    assert(!reader.next().has_value());
    assert(!reader.failed());

    assert(reader.seekRecord(0));
    const auto replay = reader.next();
    assert(replay.has_value());
    assert(replay->item_id == expected.item_id);

    ContainerItemSpoolWriter empty_writer(temporary.string(), "empty_container.dis");
    std::string empty_path;
    assert(empty_writer.finish(&empty_path));
    ContainerItemSpoolReader empty_reader(empty_path);
    assert(empty_reader.valid());
    assert(empty_reader.recordCount() == 0U);
    assert(!empty_reader.next().has_value());
    assert(!empty_reader.failed());
}

void testEntityRoundTrip() {
    const ScopedTempDirectory temporary;
    EntitySpoolWriter writer(temporary.string());
    const EntityRecord expected = sampleEntityRecord();
    assert(writer.append(expected));

    std::string spool_path;
    assert(writer.finish(&spool_path));
    EntitySpoolReader reader(spool_path);
    assert(reader.valid());
    const auto record = reader.next();
    assert(record.has_value());
    assert(record->entity_id == expected.entity_id);
    assert(record->x == expected.x && record->y == expected.y && record->z == expected.z);
    assert(record->yaw == expected.yaw && record->pitch == expected.pitch);
    assert(record->custom_name == expected.custom_name);
    assert(reader.seekRecord(0));
    assert(reader.next().has_value());

    assert(!reader.failed());

    EntitySpoolWriter invalid_writer(temporary.string(), "invalid_entity.dis");
    EntityRecord invalid = expected;
    invalid.x = std::numeric_limits<double>::quiet_NaN();
    assert(!invalid_writer.append(invalid));
}

void testSignRoundTripAndSeek() {
    const ScopedTempDirectory temporary;
    SignSpoolWriter writer(temporary.string());
    const SignRecord expected = sampleSignRecord();
    assert(writer.append(expected));

    std::string spool_path;
    assert(writer.finish(&spool_path));
    SignSpoolReader reader(spool_path);
    assert(reader.valid());
    assert(reader.recordCount() == 1U);
    const auto record = reader.next();
    assert(record.has_value());
    assert(record->x == expected.x && record->y == expected.y &&
           record->z == expected.z);
    assert(record->expected_sign_id == expected.expected_sign_id);
    assert(record->has_expected_aux &&
           record->expected_aux == expected.expected_aux);
    assert(record->front.present && record->front.has_text &&
           record->front.text == expected.front.text);
    assert(record->front.has_text_color &&
           record->front.text_color == expected.front.text_color);
    assert(record->front.has_ignore_lighting &&
           record->front.ignore_lighting);
    assert(record->front.has_persist_formatting &&
           !record->front.persist_formatting);
    assert(record->front.has_hide_glow_outline &&
           record->front.hide_glow_outline);
    assert(record->front.has_text_ignore_legacy_bug_resolved &&
           record->front.text_ignore_legacy_bug_resolved);
    assert(record->back.present && record->back.has_text &&
           record->back.text == expected.back.text);
    assert(record->back.has_ignore_lighting &&
           !record->back.ignore_lighting);
    assert(record->has_is_waxed && record->is_waxed);
    assert(!reader.next().has_value());
    assert(!reader.failed());
    assert(reader.seekRecord(0));
    assert(reader.next().has_value());

    SignRecord legacy = expected;
    legacy.has_expected_aux = false;
    legacy.expected_aux = 0;
    SignSpoolWriter legacy_writer(temporary.string(), "legacy_sign.dis");
    assert(legacy_writer.append(legacy));
    std::string legacy_path;
    assert(legacy_writer.finish(&legacy_path));
    SignSpoolReader legacy_reader(legacy_path);
    const auto legacy_record = legacy_reader.next();
    assert(legacy_record.has_value() && !legacy_record->has_expected_aux &&
           legacy_record->expected_aux == 0);

    SignSpoolWriter empty_writer(temporary.string(), "empty_sign.dis");
    std::string empty_path;
    assert(empty_writer.finish(&empty_path));
    SignSpoolReader empty_reader(empty_path);
    assert(empty_reader.valid());
    assert(empty_reader.recordCount() == 0U);
    assert(!empty_reader.next().has_value());

    SignSpoolWriter invalid_writer(temporary.string(), "invalid_sign.dis");
    SignRecord invalid = expected;
    invalid.expected_sign_id = "minecraft:chest";
    assert(!invalid_writer.append(invalid));

    SignSpoolWriter invalid_aux_writer(temporary.string(), "invalid_aux_sign.dis");
    invalid = expected;
    invalid.has_expected_aux = false;
    assert(!invalid_aux_writer.append(invalid));

    SignSpoolWriter hidden_writer(temporary.string(), "hidden_sign.dis");
    invalid = expected;
    invalid.front.has_ignore_lighting = false;
    invalid.front.ignore_lighting = true;
    assert(!hidden_writer.append(invalid));
}

void testSignCorruptionFailsClosed() {
    const ScopedTempDirectory temporary;
    SignSpoolWriter writer(temporary.string(), "corrupt_sign.dis");
    assert(writer.append(sampleSignRecord()));
    std::string spool_path;
    assert(writer.finish(&spool_path));

    std::fstream file(spool_path, std::ios::binary | std::ios::in | std::ios::out);
    assert(file);
    const std::streamoff flags_offset = static_cast<std::streamoff>(
        sizeof(TestFileHeader) + sizeof(int32_t) * 3U);
    file.seekp(flags_offset, std::ios::beg);
    const uint32_t unknown_flag = 0x80000000U;
    file.write(reinterpret_cast<const char*>(&unknown_flag), sizeof(unknown_flag));
    file.close();

    SignSpoolReader reader(spool_path);
    assert(reader.valid());
    assert(!reader.next().has_value());
    assert(reader.failed());
}

void testCorruptionIsNotReportedAsEndOfFile() {
    const ScopedTempDirectory temporary;
    ContainerItemSpoolWriter writer(temporary.string());
    assert(writer.append(sampleContainerRecord()));
    std::string spool_path;
    assert(writer.finish(&spool_path));

    // The first variable-length field begins after three coordinates and the
    // slot/count/aux fields. Preserve the physical file size so next() must
    // reject the malformed record rather than mistaking it for a clean EOF.
    std::fstream file(spool_path, std::ios::binary | std::ios::in | std::ios::out);
    assert(file);
    const std::streamoff container_id_length_offset =
        static_cast<std::streamoff>(sizeof(TestFileHeader) + sizeof(int32_t) * 3U +
                                    sizeof(uint16_t) * 3U);
    file.seekp(container_id_length_offset, std::ios::beg);
    const uint16_t zero = 0;
    file.write(reinterpret_cast<const char*>(&zero), sizeof(zero));
    file.close();

    ContainerItemSpoolReader reader(spool_path);
    assert(reader.valid());
    assert(!reader.next().has_value());
    assert(reader.failed());

    EntitySpoolWriter entity_writer(temporary.string(), "trailing_entity.dis");
    assert(entity_writer.append(sampleEntityRecord()));
    std::string entity_path;
    assert(entity_writer.finish(&entity_path));
    std::ofstream trailing(entity_path, std::ios::binary | std::ios::app);
    assert(trailing);
    const char extra = '\x7F';
    trailing.write(&extra, 1);
    trailing.close();
    EntitySpoolReader trailing_reader(entity_path);
    assert(!trailing_reader.valid());
    assert(trailing_reader.failed());
}

}  // namespace

int main() {
    testNormalizers();
    testContainerRoundTripAndSeek();
    testEntityRoundTrip();
    testSignRoundTripAndSeek();
    testSignCorruptionFailsClosed();
    testCorruptionIsNotReportedAsEndOfFile();
    return 0;
}
