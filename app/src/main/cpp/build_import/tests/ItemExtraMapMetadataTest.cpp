#include "../ItemExtraMapMetadata.h"

#include <cassert>
#include <cstdint>
#include <string>

using build_import::ItemExtraMapMetadata;
using build_import::MapItemNameSource;
using build_import::MapItemNameStatus;
using build_import::ParseItemExtraMapMetadata;

namespace {

void le16(std::string* out, uint16_t value) {
    out->push_back(static_cast<char>(value & 0xFFU));
    out->push_back(static_cast<char>(value >> 8U));
}

void le32(std::string* out, uint32_t value) {
    for (uint32_t index = 0; index < 4U; ++index) {
        out->push_back(static_cast<char>(value >> (index * 8U)));
    }
}

void le64(std::string* out, int64_t signed_value) {
    const uint64_t value = static_cast<uint64_t>(signed_value);
    for (uint32_t index = 0; index < 8U; ++index) {
        out->push_back(static_cast<char>(value >> (index * 8U)));
    }
}

void named(std::string* out, uint8_t type, const std::string& name) {
    out->push_back(static_cast<char>(type));
    le16(out, static_cast<uint16_t>(name.size()));
    out->append(name);
}

void stringTag(std::string* out, const std::string& name,
               const std::string& value) {
    named(out, 8U, name);
    le16(out, static_cast<uint16_t>(value.size()));
    out->append(value);
}

void uuidTag(std::string* out, int64_t uuid, const std::string& name = "map_uuid") {
    named(out, 4U, name);
    le64(out, uuid);
}

std::string start() {
    std::string out("\xFF\xFF\x01\x0A", 4);
    le16(&out, 0U);
    return out;
}

void finish(std::string* out) {
    out->push_back(0);  // root TAG_End
    le32(out, 0U);      // can_place_on
    le32(out, 0U);      // can_destroy
}

bool parse(const std::string& data, ItemExtraMapMetadata* result) {
    return ParseItemExtraMapMetadata(data, result);
}

void nameAndUuid() {
    constexpr int64_t uuid = -532575944698LL;
    const std::string expected =
        "\xE5\x9C\xB0\xE5\x9B\xBE 2\xE8\xA1\x8C" "3\xE5\x88\x97";
    std::string data = start();
    named(&data, 10U, "display");
    stringTag(&data, "Name", expected);
    data.push_back(0);
    uuidTag(&data, uuid);
    finish(&data);
    ItemExtraMapMetadata result;
    assert(parse(data, &result));
    assert(result.has_map_uuid && result.map_uuid == uuid);
    assert(result.name_status == MapItemNameStatus::Present);
    assert(result.name_source == MapItemNameSource::DisplayName);
    assert(result.display_name == expected);

    data = start();
    uuidTag(&data, uuid);
    stringTag(&data, "CustomName", expected);
    finish(&data);
    assert(parse(data, &result));
    assert(result.has_map_uuid && result.map_uuid == uuid);
    assert(result.name_status == MapItemNameStatus::Present);
    assert(result.name_source == MapItemNameSource::RootCustomName);
    assert(result.display_name == expected);

    data = start();
    uuidTag(&data, uuid);
    finish(&data);
    assert(parse(data, &result));
    assert(result.has_map_uuid && result.map_uuid == uuid);
    assert(result.name_status == MapItemNameStatus::Missing);
    assert(result.name_source == MapItemNameSource::None);
    assert(result.display_name.empty());
}

void duplicateAndUnusableNamesPreserveUuid() {
    constexpr int64_t uuid = 12345;
    ItemExtraMapMetadata result;
    std::string data = start();
    uuidTag(&data, uuid);
    stringTag(&data, "CustomName", "one");
    stringTag(&data, "CustomName", "two");
    finish(&data);
    assert(parse(data, &result));
    assert(result.has_map_uuid && result.map_uuid == uuid);
    assert(result.name_status == MapItemNameStatus::Unusable);
    assert(result.display_name.empty());

    data = start();
    stringTag(&data, "CustomName", "one");
    named(&data, 10U, "display");
    stringTag(&data, "Name", "two");
    data.push_back(0);
    uuidTag(&data, uuid);
    finish(&data);
    assert(parse(data, &result));
    assert(result.name_status == MapItemNameStatus::Unusable && result.has_map_uuid);

    data = start();
    named(&data, 10U, "display");
    stringTag(&data, "Name", "one");
    stringTag(&data, "Name", "two");
    data.push_back(0);
    uuidTag(&data, uuid);
    finish(&data);
    assert(parse(data, &result));
    assert(result.name_status == MapItemNameStatus::Unusable && result.has_map_uuid);

    for (const std::string& bad : {std::string(257U, 'x'),
                                   std::string("\xC0\xAF", 2),
                                   std::string("\xED\xA0\x80", 3),
                                   std::string("a\0b", 3)}) {
        data = start();
        stringTag(&data, "CustomName", bad);
        uuidTag(&data, uuid);
        finish(&data);
        assert(parse(data, &result));
        assert(result.has_map_uuid && result.map_uuid == uuid);
        assert(result.name_status == MapItemNameStatus::Unusable);
        assert(result.display_name.empty());
    }
    data = start();
    stringTag(&data, "CustomName", std::string(256U, 'x'));
    uuidTag(&data, uuid);
    finish(&data);
    assert(parse(data, &result));
    assert(result.name_status == MapItemNameStatus::Present &&
           result.display_name.size() == 256U);
}

void malformedDataFailsClosed() {
    ItemExtraMapMetadata result;
    std::string data = start();
    uuidTag(&data, 5);
    uuidTag(&data, 6);
    finish(&data);
    assert(!parse(data, &result));
    assert(!result.has_map_uuid && result.display_name.empty());

    data = start();
    stringTag(&data, "CustomName", std::string(4097U, 'x'));
    uuidTag(&data, 5);
    finish(&data);
    assert(!parse(data, &result));

    data = start();
    named(&data, 7U, "oversized_array");
    le32(&data, 0xFFFFFFFFU);
    finish(&data);
    assert(!parse(data, &result));

    data = start();
    named(&data, 9U, "oversized_list");
    data.push_back(1);
    le32(&data, 0xFFFFFFFFU);
    finish(&data);
    assert(!parse(data, &result));

    data = start();
    for (uint32_t index = 0; index < 17U; ++index) named(&data, 10U, "x");
    for (uint32_t index = 0; index < 17U; ++index) data.push_back(0);
    finish(&data);
    assert(!parse(data, &result));

    data = start();
    for (uint32_t index = 0; index < 16385U; ++index) {
        named(&data, 1U, "x");
        data.push_back(0);
    }
    finish(&data);
    assert(!parse(data, &result));

    data = start();
    named(&data, 10U, "nested");
    uuidTag(&data, 17);
    data.push_back(0);
    finish(&data);
    assert(parse(data, &result));
    assert(!result.has_map_uuid && result.name_status == MapItemNameStatus::Missing);

    data.push_back(0);
    assert(!parse(data, &result));
}

}  // namespace

int main() {
    nameAndUuid();
    duplicateAndUnusableNamesPreserveUuid();
    malformedDataFailsClosed();
    return 0;
}
