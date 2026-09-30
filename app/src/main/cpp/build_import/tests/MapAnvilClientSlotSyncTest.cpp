#include "MapAnvilClientSlotSync.h"
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
    le16(&result, 0U);       // anonymous root compound
    result.push_back(4);    // TAG_Long
    le16(&result, 8U);
    result.append("map_uuid");
    le64(&result, static_cast<uint64_t>(uuid));
    result.push_back(0);    // root TAG_End
    le32(&result, 0U);      // can_place_on count
    le32(&result, 0U);      // can_destroy count
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
    varInt(&result, 0);   // block runtime ID
    const std::string extra = mapExtra(uuid);
    varUInt(&result, static_cast<uint32_t>(extra.size()));
    result.append(extra);
    return result;
}

std::string slotPacket(uint32_t inventory_id, uint32_t slot,
                       const std::string& item) {
    std::string result;
    varUInt(&result, 0x32U);
    varUInt(&result, inventory_id);
    varUInt(&result, slot);
    result.push_back(0);  // FullContainerName: ContainerName
    result.push_back(0);  // no dynamic container ID
    result.push_back(0);  // empty storage ItemData
    result.append(item);
    return result;
}

build_import::MapAnvilClientSlotSyncRequest request() {
    build_import::MapAnvilClientSlotSyncRequest result;
    result.source_hotbar_slot = 4U;
    result.anvil_window_id = 77U;
    result.anvil_physical_slot = 2U;
    result.expected_runtime_item_id = 593;
    result.expected_source_network_stack_id = 127;
    result.expected_map_uuid = -123456789LL;
    result.confirmed_destination_network_stack_id = 300;
    return result;
}

void validInputProducesOnlyTwoNarrowChanges() {
    const auto binding = request();
    const std::string item = mapItem(593, 1U, 3U, 127,
                                     binding.expected_map_uuid);
    const std::string source = slotPacket(0U, 4U, item);
    const std::string destination = slotPacket(77U, 2U, item);
    build_import::MapAnvilClientSlotSyncPackets result;
    std::string error("stale error");
    assert(build_import::BuildMapAnvilClientInputSlotRefresh(
        binding, source, destination, &result, &error));
    assert(error.empty());
    assert(result.empty_source_packet == slotPacket(0U, 4U, std::string(1U, '\0')));
    assert(result.occupied_anvil_input_packet == slotPacket(
        77U, 2U, mapItem(593, 1U, 3U, 300, binding.expected_map_uuid)));

    // The native destination may be physical slot zero; its value is evidence
    // supplied by the caller, never baked into this byte converter.
    auto zero_slot = binding;
    zero_slot.anvil_physical_slot = 0U;
    zero_slot.confirmed_destination_network_stack_id = 127;
    assert(build_import::BuildMapAnvilClientInputSlotRefresh(
        zero_slot, source, slotPacket(77U, 0U, item), &result, &error));
    assert(result.occupied_anvil_input_packet == slotPacket(77U, 0U, item));
}

void rejectIncorrectEnvelopeAndMapIdentity() {
    const auto binding = request();
    const std::string item = mapItem(593, 1U, 3U, 127,
                                     binding.expected_map_uuid);
    const std::string source = slotPacket(0U, 4U, item);
    const std::string destination = slotPacket(77U, 2U, item);
    build_import::MapAnvilClientSlotSyncPackets result;
    std::string error;
    auto rejects = [&](const build_import::MapAnvilClientSlotSyncRequest& asked,
                       const std::string& src, const std::string& dst) {
        result.empty_source_packet = "stale";
        result.occupied_anvil_input_packet = "stale";
        assert(!build_import::BuildMapAnvilClientInputSlotRefresh(
            asked, src, dst, &result, &error));
        assert(!error.empty());
        assert(result.empty_source_packet.empty());
        assert(result.occupied_anvil_input_packet.empty());
    };
    rejects(binding, source, slotPacket(78U, 2U, item));
    rejects(binding, source, slotPacket(77U, 1U, item));
    rejects(binding, slotPacket(1U, 4U, item), destination);
    rejects(binding, slotPacket(0U, 3U, item), destination);
    rejects(binding, source, slotPacket(77U, 2U,
        mapItem(593, 1U, 3U, 127, binding.expected_map_uuid + 1)));
    rejects(binding, source, slotPacket(77U, 2U,
        mapItem(593, 2U, 3U, 127, binding.expected_map_uuid)));
    rejects(binding, source, slotPacket(77U, 2U,
        mapItem(593, 1U, 3U, 128, binding.expected_map_uuid)));
    rejects(binding, source, slotPacket(77U, 2U,
        mapItem(594, 1U, 3U, 127, binding.expected_map_uuid)));
    rejects(binding, source, slotPacket(77U, 2U,
        mapItem(593, 1U, 4U, 127, binding.expected_map_uuid)));
    std::string wrong_header = destination;
    wrong_header[0] = static_cast<char>(0x31U);
    rejects(binding, source, wrong_header);
    std::string trailing_byte = destination;
    trailing_byte.push_back('x');
    rejects(binding, source, trailing_byte);
    std::string truncated = destination;
    truncated.pop_back();
    rejects(binding, source, truncated);
    auto invalid = binding;
    invalid.anvil_window_id = 0U;
    rejects(invalid, source, destination);
    invalid = binding;
    invalid.confirmed_destination_network_stack_id = 0;
    rejects(invalid, source, destination);
}

}  // namespace

int main() {
    validInputProducesOnlyTwoNarrowChanges();
    rejectIncorrectEnvelopeAndMapIdentity();
}
