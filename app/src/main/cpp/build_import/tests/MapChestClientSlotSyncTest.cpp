#include "MapChestClientSlotSync.h"
#include "ProjectionPrinterInventoryClientSync.h"

#include <cassert>
#include <cstdint>
#include <string>

namespace {

void varUInt(std::string* output, uint32_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7FU);
        value >>= 7U;
        if (value != 0U) byte |= 0x80U;
        output->push_back(static_cast<char>(byte));
    } while (value != 0U);
}

void varInt(std::string* output, int32_t value) {
    varUInt(output, (static_cast<uint32_t>(value) << 1U) ^
                    (value < 0 ? 0xFFFFFFFFU : 0U));
}

void le16(std::string* output, uint16_t value) {
    output->push_back(static_cast<char>(value));
    output->push_back(static_cast<char>(value >> 8U));
}

void le32(std::string* output, uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U)
        output->push_back(static_cast<char>(value >> shift));
}

void le64(std::string* output, uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U)
        output->push_back(static_cast<char>(value >> shift));
}

std::string mapExtra(int64_t uuid) {
    std::string result("\xFF\xFF\x01\x0A", 4U);
    le16(&result, 0U);
    result.push_back(4);  // TAG_Long
    le16(&result, 8U);
    result.append("map_uuid");
    le64(&result, static_cast<uint64_t>(uuid));
    result.push_back(0);  // TAG_End
    le32(&result, 0U);  // can_place_on count
    le32(&result, 0U);  // can_destroy count
    return result;
}

std::string mapItem(int32_t runtime_id, uint16_t count, uint32_t aux,
                    int32_t network_id, int64_t uuid) {
    std::string result;
    varInt(&result, runtime_id);
    le16(&result, count);
    varUInt(&result, aux);
    result.push_back(1);  // has ItemStackNetId
    varInt(&result, network_id);
    varInt(&result, 0);  // block runtime ID
    const std::string extra = mapExtra(uuid);
    varUInt(&result, static_cast<uint32_t>(extra.size()));
    result.append(extra);
    return result;
}

std::string slotPacket(uint32_t inventory_id, uint32_t slot,
                       const std::string& item) {
    std::string result;
    varUInt(&result, 0x32U);  // InventorySlot packet
    varUInt(&result, inventory_id);
    varUInt(&result, slot);
    result.push_back(0);  // FullContainerName: ContainerName
    result.push_back(0);  // no dynamic container ID
    result.push_back(0);  // empty storage ItemData
    result.append(item);
    return result;
}

build_import::MapChestClientSlotSyncRequest request() {
    build_import::MapChestClientSlotSyncRequest result;
    result.source_hotbar_slot = 4U;
    result.expected_runtime_item_id = 593;
    result.expected_source_network_stack_id = 127;
    result.expected_map_uuid = -123456789LL;
    return result;
}

void transformsOnlyOccupiedItemData() {
    const auto binding = request();
    const std::string input = slotPacket(0U, 4U,
        mapItem(593, 1U, 3U, 127, binding.expected_map_uuid));
    std::string output;
    std::string error("stale");
    assert(build_import::BuildMapChestClientSourceSlotRefresh(
        binding, input, &output, &error));
    assert(error.empty());
    assert(output == slotPacket(0U, 4U, std::string(1U, '\0')));
    build_import::ProjectionPrinterNativeSlotItem parsed;
    assert(build_import::DecodeProjectionPrinterNativeInventorySlotItem(
        output, 4U, &parsed));
    assert(!parsed.occupied);

    // Changing only the input aux should not alter any envelope/context byte.
    const std::string variant = slotPacket(0U, 4U,
        mapItem(593, 1U, 257U, 127, binding.expected_map_uuid));
    assert(build_import::BuildMapChestClientSourceSlotRefresh(
        binding, variant, &output, &error));
    assert(output == slotPacket(0U, 4U, std::string(1U, '\0')));
}

void rejectsWrongEnvelopeOrIdentity() {
    const auto binding = request();
    const std::string valid_item = mapItem(593, 1U, 3U, 127,
                                           binding.expected_map_uuid);
    const std::string input = slotPacket(0U, 4U, valid_item);
    std::string output;
    std::string error;
    auto rejects = [&](const build_import::MapChestClientSlotSyncRequest& asked,
                       const std::string& source) {
        output = "stale";
        error.clear();
        assert(!build_import::BuildMapChestClientSourceSlotRefresh(
            asked, source, &output, &error));
        assert(output.empty());
        assert(!error.empty());
    };
    rejects(binding, slotPacket(1U, 4U, valid_item));
    rejects(binding, slotPacket(0U, 5U, valid_item));
    rejects(binding, slotPacket(0U, 4U,
        mapItem(594, 1U, 3U, 127, binding.expected_map_uuid)));
    rejects(binding, slotPacket(0U, 4U,
        mapItem(593, 2U, 3U, 127, binding.expected_map_uuid)));
    rejects(binding, slotPacket(0U, 4U,
        mapItem(593, 1U, 3U, 128, binding.expected_map_uuid)));
    rejects(binding, slotPacket(0U, 4U,
        mapItem(593, 1U, 3U, 127, binding.expected_map_uuid + 1)));
    rejects(binding, slotPacket(0U, 4U, std::string(1U, '\0')));
    std::string wrong_header = input;
    wrong_header[0] = static_cast<char>(0x31U);
    rejects(binding, wrong_header);
    std::string trailing = input;
    trailing.push_back('x');
    rejects(binding, trailing);
    std::string truncated = input;
    truncated.pop_back();
    rejects(binding, truncated);
    auto invalid = binding;
    invalid.source_hotbar_slot = 9U;
    rejects(invalid, input);
    invalid = binding;
    invalid.expected_runtime_item_id = 0;
    rejects(invalid, input);
    invalid = binding;
    invalid.expected_source_network_stack_id = 0;
    rejects(invalid, input);
    invalid = binding;
    invalid.expected_map_uuid = -1;
    rejects(invalid, input);
    error.clear();
    assert(!build_import::BuildMapChestClientSourceSlotRefresh(
        binding, input, nullptr, &error));
    assert(!error.empty());
}

}  // namespace

int main() {
    transformsOnlyOccupiedItemData();
    rejectsWrongEnvelopeOrIdentity();
}
