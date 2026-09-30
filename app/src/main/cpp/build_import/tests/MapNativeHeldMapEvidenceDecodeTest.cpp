#include "../ProjectionPrinterInventoryClientSync.h"

#include <cassert>
#include <cstdint>
#include <string>

namespace {

void varUInt(std::string* out, uint32_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7FU);
        value >>= 7U;
        if (value != 0U) byte |= 0x80U;
        out->push_back(static_cast<char>(byte));
    } while (value != 0U);
}

void varInt(std::string* out, int32_t value) {
    const uint32_t mask = value < 0 ? 0xFFFFFFFFU : 0U;
    varUInt(out, (static_cast<uint32_t>(value) << 1U) ^ mask);
}

void le16(std::string* out, uint16_t value) {
    out->push_back(static_cast<char>(value & 0xFFU));
    out->push_back(static_cast<char>(value >> 8U));
}

void le32(std::string* out, uint32_t value) {
    for (unsigned i = 0; i < 4U; ++i) out->push_back(static_cast<char>(value >> (8U * i)));
}

void le64(std::string* out, int64_t signed_value) {
    const uint64_t value = static_cast<uint64_t>(signed_value);
    for (unsigned i = 0; i < 8U; ++i) out->push_back(static_cast<char>(value >> (8U * i)));
}

void named(std::string* out, uint8_t type, const std::string& name) {
    out->push_back(static_cast<char>(type));
    le16(out, static_cast<uint16_t>(name.size()));
    out->append(name);
}

std::string mapExtra(bool name, bool duplicate = false) {
    std::string out("\xFF\xFF\x01\x0A", 4U);
    le16(&out, 0U);
    named(&out, 4U, "map_uuid");
    le64(&out, -532575944698LL);
    if (name) {
        named(&out, 10U, "display");
        named(&out, 8U, "Name");
        const std::string title = "地图 1行1列";
        le16(&out, static_cast<uint16_t>(title.size()));
        out.append(title);
        if (duplicate) {
            named(&out, 8U, "Name");
            le16(&out, static_cast<uint16_t>(title.size()));
            out.append(title);
        }
        out.push_back(0);
    }
    out.push_back(0);
    le32(&out, 0U);
    le32(&out, 0U);
    return out;
}

std::string nativeSlot(uint8_t slot, uint16_t count, int32_t stack_id,
                       const std::string& extra, bool has_network_id = true) {
    std::string out;
    varUInt(&out, 0x32U);
    varUInt(&out, 0U);  // player inventory
    varUInt(&out, slot);
    out.push_back(0);   // full container name
    out.push_back(0);   // no dynamic ID
    varInt(&out, 0);    // empty storage item
    varInt(&out, 358);  // filled-map runtime ID in this fixture
    le16(&out, count);
    varUInt(&out, 0U);  // aux
    out.push_back(has_network_id ? 1 : 0);
    if (has_network_id) varInt(&out, stack_id);
    varInt(&out, 0);    // block runtime ID
    varUInt(&out, static_cast<uint32_t>(extra.size()));
    out.append(extra);
    return out;
}

void exactNativeMapMetadata() {
    const std::string packet = nativeSlot(5U, 1U, 71, mapExtra(true));
    build_import::ProjectionPrinterNativeSlotItem item;
    build_import::ItemExtraMapMetadata metadata;
    assert(build_import::DecodeProjectionPrinterNativeInventorySlotMapMetadata(
        packet, 5U, &item, &metadata));
    assert(item.occupied && item.runtime_item_id == 358 && item.count == 1U);
    assert(item.has_network_stack_id && item.network_stack_id == 71);
    assert(metadata.has_map_uuid && metadata.map_uuid == -532575944698LL);
    assert(metadata.name_status == build_import::MapItemNameStatus::Present);
    assert(metadata.name_source == build_import::MapItemNameSource::DisplayName);
    assert(metadata.display_name == "地图 1行1列");

    assert(!build_import::DecodeProjectionPrinterNativeInventorySlotMapMetadata(
        packet, 4U, &item, &metadata));
    assert(!item.occupied && !metadata.has_map_uuid);
    std::string trailing = packet;
    trailing.push_back('x');
    assert(!build_import::DecodeProjectionPrinterNativeInventorySlotMapMetadata(
        trailing, 5U, &item, &metadata));
    assert(!item.occupied && !metadata.has_map_uuid);
}

void nameCannotBeSynthesizedFromMissingOrAmbiguousNbt() {
    build_import::ProjectionPrinterNativeSlotItem item;
    build_import::ItemExtraMapMetadata metadata;
    assert(build_import::DecodeProjectionPrinterNativeInventorySlotMapMetadata(
        nativeSlot(5U, 1U, 71, mapExtra(false)), 5U, &item, &metadata));
    assert(metadata.has_map_uuid);
    assert(metadata.name_status == build_import::MapItemNameStatus::Missing);
    assert(metadata.display_name.empty());

    assert(build_import::DecodeProjectionPrinterNativeInventorySlotMapMetadata(
        nativeSlot(5U, 1U, 71, mapExtra(true, true)), 5U, &item, &metadata));
    assert(metadata.name_status == build_import::MapItemNameStatus::Unusable);
    assert(metadata.display_name.empty());

    assert(!build_import::DecodeProjectionPrinterNativeInventorySlotMapMetadata(
        nativeSlot(5U, 1U, 71, "cached-ui-title"), 5U, &item, &metadata));
    assert(!item.occupied && !metadata.has_map_uuid);
}

void previewItemInstanceDoesNotNeedNetworkStackIdentity() {
    const std::string packet = nativeSlot(0U, 1U, 0, mapExtra(true), false);
    build_import::ProjectionPrinterNativeSlotItem item;
    build_import::ItemExtraMapMetadata metadata;
    // The ordinary inventory reader must continue to reject an ItemInstance
    // preview without an owned ItemStack network ID.
    assert(!build_import::DecodeProjectionPrinterNativeInventorySlotMapMetadata(
        packet, 0U, &item, &metadata));
    assert(build_import::DecodeProjectionPrinterNativeInventorySlotPreviewMapMetadata(
        packet, 0U, &item, &metadata));
    assert(item.occupied && item.runtime_item_id == 358 && item.count == 1U);
    assert(!item.has_network_stack_id && item.network_stack_id == 0);
    assert(metadata.has_map_uuid && metadata.map_uuid == -532575944698LL);
    assert(metadata.name_status == build_import::MapItemNameStatus::Present &&
           metadata.name_source == build_import::MapItemNameSource::DisplayName &&
           metadata.display_name == "地图 1行1列");
    assert(!build_import::DecodeProjectionPrinterNativeInventorySlotPreviewMapMetadata(
        packet, 1U, &item, &metadata));
    assert(!item.occupied && !metadata.has_map_uuid);
    assert(!build_import::DecodeProjectionPrinterNativeInventorySlotPreviewMapMetadata(
        nativeSlot(0U, 1U, 0, "ui-only-name", false),
        0U, &item, &metadata));
}

}  // namespace

int main() {
    exactNativeMapMetadata();
    nameCannotBeSynthesizedFromMissingOrAmbiguousNbt();
    previewItemInstanceDoesNotNeedNetworkStackIdentity();
    return 0;
}
