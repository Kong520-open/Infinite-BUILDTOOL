#include "MapAnvilClientSyncDelivery.h"

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

std::string mapItem(int32_t network_id) {
    std::string extra("\xFF\xFF\x01\x0A", 4U);
    le16(&extra, 0U);
    extra.push_back(4);
    le16(&extra, 8U);
    extra.append("map_uuid");
    le64(&extra, static_cast<uint64_t>(-123456789LL));
    extra.push_back(0);
    le32(&extra, 0U);
    le32(&extra, 0U);

    std::string item;
    varInt(&item, 593);
    le16(&item, 1U);
    varUInt(&item, 3U);
    item.push_back(1);  // ItemStackNetId present
    varInt(&item, network_id);
    varInt(&item, 0);   // block runtime ID
    varUInt(&item, static_cast<uint32_t>(extra.size()));
    item.append(extra);
    return item;
}

std::string slotPacket(uint32_t inventory_id, uint32_t slot,
                       const std::string& item) {
    std::string result;
    varUInt(&result, 0x32U);
    varUInt(&result, inventory_id);
    varUInt(&result, slot);
    result.push_back(0);  // ContainerName
    result.push_back(0);  // no dynamic container ID
    result.push_back(0);  // empty storage ItemData
    result.append(item);
    return result;
}

build_import::MapAnvilClientSyncCandidate candidate() {
    build_import::MapAnvilClientSyncCandidate result;
    result.request_id = -3;
    result.session_generation = 9;
    result.response_generation_before_send = 10;
    result.window_token = 71;
    result.window_id = 7;
    result.source_hotbar_slot = 4;
    result.anvil_physical_slot_explicit = true;
    result.anvil_physical_slot = 0;
    result.runtime_item_id = 593;
    result.source_network_stack_id = 127;
    result.map_uuid = -123456789LL;
    result.occupied_source_packet = slotPacket(0U, 4U, mapItem(127));
    result.occupied_anvil_packet = slotPacket(7U, 0U, mapItem(127));
    return result;
}

build_import::ProjectionPrinterInventoryResponse acceptedResponse() {
    using namespace build_import;
    ProjectionPrinterInventoryResponse response;
    response.valid = true;
    response.status = 0;
    response.layout = ProjectionPrinterInventoryResponseLayout::
        V859SlotHotbarSlotAmountNetworkId;
    response.entry_count = 1;
    response.successful_entry_count = 1;
    response.request_id = -3;
    response.session_generation = 9;
    response.response_generation = 11;
    ProjectionPrinterInventoryResponseSlot source;
    source.container_id = 29;
    source.slot = 4;
    source.count = 0;
    source.network_stack_id = 0;
    response.slots.push_back(source);
    ProjectionPrinterInventoryResponseSlot destination;
    destination.container_id = 0;
    destination.slot = 1;  // server response namespace, not physical slot
    destination.count = 1;
    destination.network_stack_id = 300;
    response.slots.push_back(destination);
    return response;
}

void prepareRequiresExactIngressAndVerifiedPhysicalSlot() {
    using namespace build_import;
    ClearMapAnvilClientSync();
    std::string error;
    auto prepared = candidate();
    prepared.anvil_physical_slot_explicit = false;
    assert(!PrepareMapAnvilClientSyncCandidate(prepared, &error));
    assert(!error.empty());

    prepared = candidate();
    assert(!PrepareMapAnvilClientSyncCandidate(prepared, &error));
    assert(!error.empty());
    BindMapAnvilClientSyncIngress(reinterpret_cast<void*>(0x1234), 71, 8);
    assert(!PrepareMapAnvilClientSyncCandidate(prepared, &error));
    CancelMapAnvilClientSyncWindow(71);
    BindMapAnvilClientSyncIngress(reinterpret_cast<void*>(0x1234), 71, 7);
    assert(PrepareMapAnvilClientSyncCandidate(prepared, &error));
    assert(error.empty());
    assert(!PrepareMapAnvilClientSyncCandidate(prepared, &error));
}

void acceptedResponseQueuesExactlyOnceAndInOrder() {
    using namespace build_import;
    auto response = acceptedResponse();
    std::string error;
    uint64_t ticket = 999;
    assert(!QueueMapAnvilClientSyncAcceptedInput(
        71, 8, -3, response, &ticket, &error));
    assert(ticket == 0);
    assert(!QueueMapAnvilClientSyncAcceptedInput(
        71, 7, -5, response, &ticket, &error));
    auto stale = response;
    stale.response_generation = 10;
    assert(!QueueMapAnvilClientSyncAcceptedInput(
        71, 7, -3, stale, &ticket, &error));
    assert(QueueMapAnvilClientSyncAcceptedInput(
        71, 7, -3, response, &ticket, &error));
    assert(ticket != 0);
    const uint64_t same_ticket = ticket;
    assert(GetMapAnvilClientSyncTicketState(ticket) ==
           MapAnvilClientSyncTicketState::Pending);
    assert(QueueMapAnvilClientSyncAcceptedInput(
        71, 7, -3, response, &ticket, &error));
    assert(ticket == same_ticket);
    MapAnvilClientSyncQueuedPacket packet;
    assert(!TakeMapAnvilClientSyncPacket(
        reinterpret_cast<void*>(0x4321), &packet));
    assert(TakeMapAnvilClientSyncPacket(
        reinterpret_cast<void*>(0x1234), &packet));
    assert(packet.ticket == ticket && packet.packet_index == 0);
    assert(packet.bytes == slotPacket(0U, 4U, std::string(1U, '\0')));
    assert(!TakeMapAnvilClientSyncPacket(
        reinterpret_cast<void*>(0x1234), &packet));
    assert(!CompleteMapAnvilClientSyncPacket(ticket, 1));
    assert(CompleteMapAnvilClientSyncPacket(ticket, 0));
    assert(!CompleteMapAnvilClientSyncPacket(ticket, 0));
    assert(TakeMapAnvilClientSyncPacket(
        reinterpret_cast<void*>(0x1234), &packet));
    assert(packet.ticket == ticket && packet.packet_index == 1);
    assert(packet.bytes == slotPacket(7U, 0U, mapItem(300)));
    assert(CompleteMapAnvilClientSyncPacket(ticket, 1));
    assert(GetMapAnvilClientSyncTicketState(ticket) ==
           MapAnvilClientSyncTicketState::Complete);
    assert(!TakeMapAnvilClientSyncPacket(
        reinterpret_cast<void*>(0x1234), &packet));
    assert(QueueMapAnvilClientSyncAcceptedInput(
        71, 7, -3, response, &ticket, &error));
    assert(ticket == same_ticket);
    assert(!TakeMapAnvilClientSyncPacket(
        reinterpret_cast<void*>(0x1234), &packet));
    response.response_generation = 12;
    assert(!QueueMapAnvilClientSyncAcceptedInput(
        71, 7, -3, response, &ticket, &error));
    assert(ticket == 0);
}

void closeOrDifferentIngressCancelsPendingDelivery() {
    using namespace build_import;
    ClearMapAnvilClientSync();
    BindMapAnvilClientSyncIngress(reinterpret_cast<void*>(0x1234), 71, 7);
    std::string error;
    assert(PrepareMapAnvilClientSyncCandidate(candidate(), &error));
    uint64_t ticket = 0;
    assert(QueueMapAnvilClientSyncAcceptedInput(
        71, 7, -3, acceptedResponse(), &ticket, &error));
    CancelMapAnvilClientSyncWindow(72);  // unrelated close is ignored
    assert(GetMapAnvilClientSyncTicketState(ticket) ==
           MapAnvilClientSyncTicketState::Pending);
    BindMapAnvilClientSyncIngress(reinterpret_cast<void*>(0x4321), 71, 7);
    assert(GetMapAnvilClientSyncTicketState(ticket) ==
           MapAnvilClientSyncTicketState::Cancelled);
    MapAnvilClientSyncQueuedPacket packet;
    assert(!TakeMapAnvilClientSyncPacket(
        reinterpret_cast<void*>(0x1234), &packet));
    assert(!TakeMapAnvilClientSyncPacket(
        reinterpret_cast<void*>(0x4321), &packet));
    assert(!QueueMapAnvilClientSyncAcceptedInput(
        71, 7, -3, acceptedResponse(), &ticket, &error));

    ClearMapAnvilClientSync();
    BindMapAnvilClientSyncIngress(reinterpret_cast<void*>(0x1234), 71, 7);
    assert(PrepareMapAnvilClientSyncCandidate(candidate(), &error));
    assert(QueueMapAnvilClientSyncAcceptedInput(
        71, 7, -3, acceptedResponse(), &ticket, &error));
    CancelMapAnvilClientSyncWindow(71);
    assert(GetMapAnvilClientSyncTicketState(ticket) ==
           MapAnvilClientSyncTicketState::Cancelled);
    assert(!TakeMapAnvilClientSyncPacket(
        reinterpret_cast<void*>(0x1234), &packet));
}

}  // namespace

int main() {
    prepareRequiresExactIngressAndVerifiedPhysicalSlot();
    acceptedResponseQueuesExactlyOnceAndInOrder();
    closeOrDifferentIngressCancelsPendingDelivery();
}
