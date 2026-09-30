#include "../ItemRuntimeRegistry.h"
#include "../ProjectionPrinterInventoryMailbox.h"

#include <cassert>
#include <cctype>
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
    const uint32_t encoded = (static_cast<uint32_t>(value) << 1U) ^
        static_cast<uint32_t>(value >> 31);
    varUInt(output, encoded);
}

void string(std::string* output, const std::string& value) {
    varUInt(output, static_cast<uint32_t>(value.size()));
    output->append(value);
}

void emptyItem(std::string* output) {
    varInt(output, 0);
}

void item(std::string* output, int32_t runtime_id, uint16_t count, uint16_t aux,
          bool has_network_stack_id, int32_t network_stack_id) {
    varInt(output, runtime_id);
    output->push_back(static_cast<char>(count & 0xFFU));
    output->push_back(static_cast<char>((count >> 8U) & 0xFFU));
    varUInt(output, aux);
    output->push_back(has_network_stack_id ? 1 : 0);
    if (has_network_stack_id) varInt(output, network_stack_id);
    varInt(output, 0);  // block runtime ID
    varUInt(output, 0); // item user data
}

std::string playerInventoryContent() {
    std::string packet;
    varUInt(&packet, 0x31U);
    varUInt(&packet, 0U);  // player InventoryContent network ID
    varUInt(&packet, 36U);
    for (uint32_t slot = 0; slot < 36U; ++slot) {
        if (slot == 2U) {
            item(&packet, 5, 64, 7, true, 91);
        } else {
            emptyItem(&packet);
        }
    }
    // v748/v859 InventoryContent tail: FullContainerName + storage ItemData.
    varUInt(&packet, 0U);
    packet.push_back(0); // no dynamic container ID
    emptyItem(&packet);
    return packet;
}

std::string playerSlotUpdate() {
    std::string packet;
    varUInt(&packet, 0x32U);
    varUInt(&packet, 0U);
    varUInt(&packet, 2U);
    packet.push_back(0); // no optional FullContainerName
    packet.push_back(0); // no optional storage item
    item(&packet, 5, 13, 9, true, 123);
    return packet;
}

std::string playerSlotUpdateV859Tail() {
    std::string packet;
    varUInt(&packet, 0x32U);
    varUInt(&packet, 0U);
    varUInt(&packet, 2U);
    // Older v859 layout: non-optional FullContainerName + storage Item.
    packet.push_back(0);
    packet.push_back(0);
    emptyItem(&packet);
    item(&packet, 5, 12, 9, true, 124);
    return packet;
}

void successfulItemStackResponseEntry(std::string* packet, int32_t request_id,
                                      uint8_t container_id, uint8_t slot,
                                      uint8_t hotbar_slot, uint8_t count,
                                      int32_t network_stack_id) {
    packet->push_back(0); // ItemStackResponseStatus::Ok
    varInt(packet, request_id);
    varUInt(packet, 1U);
    packet->push_back(static_cast<char>(container_id)); // FullContainerName ID
    packet->push_back(0);                               // no dynamic container ID
    varUInt(packet, 1U);
    packet->push_back(static_cast<char>(slot));
    packet->push_back(static_cast<char>(hotbar_slot));
    packet->push_back(static_cast<char>(count));
    varInt(packet, network_stack_id);
    string(packet, "test");
    string(packet, "filter");
    varInt(packet, 0);
}

void rejectedItemStackResponseEntry(std::string* packet, uint8_t status,
                                    int32_t request_id) {
    packet->push_back(static_cast<char>(status));
    varInt(packet, request_id);
    // Failure responses carry no response-container payload.
}

std::string itemStackResponse() {
    std::string packet;
    varUInt(&packet, 0x94U);
    varUInt(&packet, 1U);
    successfulItemStackResponseEntry(&packet, 41, 29U, 2U, 8U, 13U, 123);
    return packet;
}

std::string rejectedItemStackResponse(uint8_t status, int32_t request_id) {
    std::string packet;
    varUInt(&packet, 0x94U);
    varUInt(&packet, 1U);
    rejectedItemStackResponseEntry(&packet, status, request_id);
    return packet;
}

std::string mixedItemStackResponseBatch(bool rejection_last) {
    std::string packet;
    varUInt(&packet, 0x94U);
    varUInt(&packet, 2U);
    if (rejection_last) {
        successfulItemStackResponseEntry(&packet, 101, 29U, 5U, 1U, 32U, 321);
        rejectedItemStackResponseEntry(&packet, 2U, -17);
    } else {
        rejectedItemStackResponseEntry(&packet, 3U, -19);
        successfulItemStackResponseEntry(&packet, 103, 29U, 6U, 3U, 16U, 654);
    }
    return packet;
}

std::string registryPacket() {
    std::string packet;
    varUInt(&packet, 0xA2U);
    varUInt(&packet, 1U);
    string(&packet, "minecraft:stone");
    packet.push_back(5);
    packet.push_back(0);
    packet.push_back(0); // not component based
    varInt(&packet, 2);
    packet.push_back(10); // empty network compound
    varUInt(&packet, 0U);
    packet.push_back(0);
    return packet;
}

}  // namespace

namespace build_import {

// ItemRuntimeRegistry normally receives this from DeferredImportDataSpool. The
// mailbox test only needs the valid registry identifier used above.
bool normalizeDeferredItemIdentifier(std::string* identifier) {
    if (!identifier || identifier->empty()) return false;
    for (char& character : *identifier) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return identifier->find(':') != std::string::npos;
}

}  // namespace build_import

int main() {
    using namespace build_import;

    ClearProjectionPrinterInventoryMailbox();
    ClearItemRuntimeRegistry();
    ObserveItemRuntimeRegistryPacket(registryPacket());
    ObserveProjectionPrinterInventoryPacket(playerInventoryContent());

    ProjectionPrinterInventorySnapshot snapshot;
    assert(GetProjectionPrinterInventorySnapshot(&snapshot));
    assert(snapshot.ready && snapshot.inventory_id == 0U && snapshot.revision != 0U);
    assert(!snapshot.slots[0].occupied);
    assert(snapshot.slots[2].occupied && snapshot.slots[2].runtime_item_id == 5 &&
           snapshot.slots[2].has_network_stack_id &&
           snapshot.slots[2].network_stack_id == 91 && snapshot.slots[2].count == 64 &&
           snapshot.slots[2].aux == 7 && snapshot.slots[2].name == "minecraft:stone");

    const uint64_t content_revision = snapshot.revision;
    ObserveProjectionPrinterInventoryPacket(playerSlotUpdate());
    assert(GetProjectionPrinterInventorySnapshot(&snapshot));
    assert(snapshot.revision > content_revision && snapshot.slots[2].count == 13 &&
           snapshot.slots[2].aux == 9 && snapshot.slots[2].network_stack_id == 123);

    ObserveProjectionPrinterInventoryPacket(playerSlotUpdateV859Tail());
    assert(GetProjectionPrinterInventorySnapshot(&snapshot));
    assert(snapshot.slots[2].count == 12 && snapshot.slots[2].network_stack_id == 124);

    ObserveProjectionPrinterInventoryPacket(itemStackResponse());
    ProjectionPrinterInventoryResponse response;
    assert(GetProjectionPrinterInventoryResponse(&response));
    assert(response.valid && response.layout ==
               ProjectionPrinterInventoryResponseLayout::
                   V859SlotHotbarSlotAmountNetworkId &&
           response.request_id == 41 && response.slots.size() == 1U);
    assert(response.slots[0].container_id == 29U && response.slots[0].slot == 2U &&
            response.slots[0].has_hotbar_slot && response.slots[0].hotbar_slot == 8U &&
            !response.slots[0].has_requested_slot &&
            !response.slots[0].has_dynamic_container_id && response.slots[0].count == 13U &&
            response.slots[0].network_stack_id == 123 &&
            response.slots[0].custom_name == "test" &&
            response.slots[0].filter_custom_name == "filter");
    const uint64_t successful_response_generation = response.response_generation;

    // A server failure is a standalone entry, not fields merged into a stale
    // success. The signed negative ID also verifies ZigZag request decoding.
    ObserveProjectionPrinterInventoryPacket(rejectedItemStackResponse(2U, -77));
    assert(GetProjectionPrinterInventoryResponse(&response));
    assert(!response.valid && response.rejected && response.status == 2U &&
            response.rejection_status == 2U && response.request_id == -77 &&
            response.rejection_request_id == -77 && response.slots.empty() &&
            response.rejection_observed_inventory_revision == snapshot.revision &&
            response.response_generation > successful_response_generation);
    assert(GetProjectionPrinterInventoryResponseByRequestId(41, &response));
    assert(response.valid && !response.rejected && response.request_id == 41 &&
           response.status == 0U && response.slots.size() == 1U);
    assert(GetProjectionPrinterInventoryResponseByRequestId(-77, &response));
    assert(!response.valid && response.rejected && response.request_id == -77 &&
           response.status == 2U);

    // A batched success followed by an unrelated rejection must not be
    // attributed to the success by the legacy "latest" accessor. Both remain
    // independently queryable by their exact request IDs.
    ObserveProjectionPrinterInventoryPacket(mixedItemStackResponseBatch(true));
    assert(GetProjectionPrinterInventoryResponse(&response));
    assert(!response.valid && response.rejected && response.request_id == -17 &&
           response.status == 2U && response.slots.empty());
    assert(GetProjectionPrinterInventoryResponseByRequestId(101, &response));
    assert(response.valid && !response.rejected && response.request_id == 101 &&
           response.slots.size() == 1U && response.slots[0].slot == 5U &&
           response.slots[0].hotbar_slot == 1U && response.slots[0].count == 32U);
    assert(GetProjectionPrinterInventoryResponseByRequestId(-17, &response));
    assert(!response.valid && response.rejected && response.request_id == -17 &&
           response.status == 2U);

    // The reverse order verifies that a later success does not overwrite the
    // previous failure in the ID-indexed history either.
    ObserveProjectionPrinterInventoryPacket(mixedItemStackResponseBatch(false));
    assert(GetProjectionPrinterInventoryResponse(&response));
    assert(response.valid && !response.rejected && response.request_id == 103 &&
           response.slots.size() == 1U && response.slots[0].slot == 6U &&
           response.slots[0].hotbar_slot == 3U);
    assert(GetProjectionPrinterInventoryResponseByRequestId(-19, &response));
    assert(!response.valid && response.rejected && response.request_id == -19 &&
           response.status == 3U);

    // A truncated Content packet must not replace a known good snapshot.
    std::string truncated = playerInventoryContent();
    truncated.pop_back();
    ObserveProjectionPrinterInventoryPacket(truncated);
    assert(GetProjectionPrinterInventorySnapshot(&snapshot));
    assert(snapshot.slots[2].network_stack_id == 124);

    std::string start_game;
    varUInt(&start_game, 0x0BU);
    ObserveProjectionPrinterInventoryPacket(start_game);
    assert(!GetProjectionPrinterInventorySnapshot(&snapshot));
    assert(!GetProjectionPrinterInventoryResponse(&response));
    return 0;
}
