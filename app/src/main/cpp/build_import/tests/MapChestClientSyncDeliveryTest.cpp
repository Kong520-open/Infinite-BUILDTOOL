#include "MapChestClientSyncDelivery.h"

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
    varUInt(out, (static_cast<uint32_t>(value) << 1U) ^
                 (value < 0 ? 0xFFFFFFFFU : 0U));
}

void le16(std::string* out, uint16_t value) {
    out->push_back(static_cast<char>(value));
    out->push_back(static_cast<char>(value >> 8U));
}

void le32(std::string* out, uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U)
        out->push_back(static_cast<char>(value >> shift));
}

void le64(std::string* out, uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U)
        out->push_back(static_cast<char>(value >> shift));
}

std::string occupiedSourcePacket(int32_t network_id = 127) {
    std::string extra("\xFF\xFF\x01\x0A", 4U);
    le16(&extra, 0U);
    extra.push_back(4);
    le16(&extra, 8U);
    extra.append("map_uuid");
    le64(&extra, static_cast<uint64_t>(-123456789LL));
    extra.push_back(0);
    le32(&extra, 0U);
    le32(&extra, 0U);
    std::string packet;
    varUInt(&packet, 0x32U);
    varUInt(&packet, 0U);  // player inventory
    varUInt(&packet, 4U);  // original physical hotbar slot
    packet.push_back(0);  // ContainerName
    packet.push_back(0);  // no dynamic container ID
    packet.push_back(0);  // empty storage ItemData
    varInt(&packet, 593);
    le16(&packet, 1U);
    varUInt(&packet, 3U);
    packet.push_back(1);  // ItemStackNetId present
    varInt(&packet, network_id);
    varInt(&packet, 0);  // block runtime ID
    varUInt(&packet, static_cast<uint32_t>(extra.size()));
    packet.append(extra);
    return packet;
}

build_import::MapChestClientSyncCandidate candidate() {
    build_import::MapChestClientSyncCandidate result;
    result.request_id = -9;
    result.session_generation = 8;
    result.response_generation_before_send = 22;
    result.window_token = 91;
    result.window_id = 5;
    result.source_hotbar_slot = 4;
    result.destination_slot = 3;
    result.runtime_item_id = 593;
    result.source_network_stack_id = 127;
    result.map_uuid = -123456789LL;
    result.occupied_source_packet = occupiedSourcePacket();
    return result;
}

build_import::ProjectionPrinterInventoryResponse acceptedResponse() {
    using namespace build_import;
    ProjectionPrinterInventoryResponse response;
    response.valid = true;
    response.status = 0;
    response.layout = ProjectionPrinterInventoryResponseLayout::
        V859SlotHotbarSlotAmountNetworkId;
    response.request_id = -9;
    response.session_generation = 8;
    response.response_generation = 23;
    ProjectionPrinterInventoryResponseSlot source;
    source.container_id = 29;
    source.slot = 4;
    source.count = 0;
    source.network_stack_id = 0;
    ProjectionPrinterInventoryResponseSlot destination;
    destination.container_id = 7;
    destination.slot = 3;
    destination.count = 1;
    destination.network_stack_id = 181;
    response.slots = {source, destination};
    return response;
}

void prepareRequiresExactIngressAndSourceMap() {
    using namespace build_import;
    ClearMapChestClientSync();
    std::string error;
    auto source = candidate();
    assert(!PrepareMapChestClientSyncCandidate(source, &error));
    BindMapChestClientSyncIngress(reinterpret_cast<void*>(0x1234), 91, 6);
    assert(!PrepareMapChestClientSyncCandidate(source, &error));
    CancelMapChestClientSyncWindow(91);
    BindMapChestClientSyncIngress(reinterpret_cast<void*>(0x1234), 91, 5);
    source.occupied_source_packet = occupiedSourcePacket(126);
    assert(!PrepareMapChestClientSyncCandidate(source, &error));
    source = candidate();
    assert(PrepareMapChestClientSyncCandidate(source, &error));
    assert(error.empty());
    uint8_t slot = 0xFFU;
    assert(GetMapChestClientSyncSourceSlot(91, -9, &slot) && slot == 4U);
    assert(!GetMapChestClientSyncSourceSlot(92, -9, &slot));
    assert(!PrepareMapChestClientSyncCandidate(source, &error));
}

void oneAcceptedRequestQueuesOneClientPacket() {
    using namespace build_import;
    std::string error;
    auto response = acceptedResponse();
    uint64_t ticket = 7;
    assert(!QueueMapChestClientSyncAcceptedTransfer(
        91, 6, -9, response, &ticket, &error) && ticket == 0U);
    assert(!QueueMapChestClientSyncAcceptedTransfer(
        91, 5, -11, response, &ticket, &error));
    auto bad = response;
    bad.response_generation = 22;
    assert(!QueueMapChestClientSyncAcceptedTransfer(
        91, 5, -9, bad, &ticket, &error));
    bad = response;
    bad.slots[0].count = 1;
    assert(!QueueMapChestClientSyncAcceptedTransfer(
        91, 5, -9, bad, &ticket, &error));
    bad = response;
    bad.slots[1].container_id = 0;
    assert(!QueueMapChestClientSyncAcceptedTransfer(
        91, 5, -9, bad, &ticket, &error));
    bad = response;
    bad.slots.push_back(bad.slots[0]);
    assert(!QueueMapChestClientSyncAcceptedTransfer(
        91, 5, -9, bad, &ticket, &error));
    bad = response;
    bad.slots[1].has_dynamic_container_id = true;
    bad.slots[1].dynamic_container_id = 1U;
    assert(!QueueMapChestClientSyncAcceptedTransfer(
        91, 5, -9, bad, &ticket, &error));
    bad = response;
    bad.rejected = true;
    assert(!QueueMapChestClientSyncAcceptedTransfer(
        91, 5, -9, bad, &ticket, &error));
    // Match the existing production validator: an unrelated ordinary slot
    // or a nonzero empty-source net ID must not manufacture a rejection.
    response.slots[0].network_stack_id = 67;
    build_import::ProjectionPrinterInventoryResponseSlot unrelated;
    unrelated.container_id = 30;
    unrelated.slot = 12;
    response.slots.push_back(unrelated);
    assert(QueueMapChestClientSyncAcceptedTransfer(
        91, 5, -9, response, &ticket, &error));
    const uint64_t original_ticket = ticket;
    assert(ticket != 0U);
    assert(GetMapChestClientSyncTicketState(ticket) ==
           MapChestClientSyncTicketState::Pending);
    assert(QueueMapChestClientSyncAcceptedTransfer(
        91, 5, -9, response, &ticket, &error) && ticket == original_ticket);
    MapChestClientSyncQueuedPacket packet;
    assert(!TakeMapChestClientSyncPacket(
        reinterpret_cast<void*>(0x4321), &packet));
    assert(TakeMapChestClientSyncPacket(
        reinterpret_cast<void*>(0x1234), &packet));
    std::string empty_source = occupiedSourcePacket();
    // The original packet header ends immediately before its ItemData.
    empty_source.resize(6U);
    empty_source.push_back('\0');
    assert(packet.ticket == ticket && packet.bytes == empty_source);
    assert(!TakeMapChestClientSyncPacket(
        reinterpret_cast<void*>(0x1234), &packet));
    assert(CompleteMapChestClientSyncPacket(ticket));
    assert(!CompleteMapChestClientSyncPacket(ticket));
    assert(GetMapChestClientSyncTicketState(ticket) ==
           MapChestClientSyncTicketState::Complete);
    assert(!TakeMapChestClientSyncPacket(
        reinterpret_cast<void*>(0x1234), &packet));
    assert(QueueMapChestClientSyncAcceptedTransfer(
        91, 5, -9, response, &ticket, &error) && ticket == original_ticket);
    response.response_generation = 24;
    assert(!QueueMapChestClientSyncAcceptedTransfer(
        91, 5, -9, response, &ticket, &error));
}

void closeOrDifferentIngressCancelsPending() {
    using namespace build_import;
    ClearMapChestClientSync();
    BindMapChestClientSyncIngress(reinterpret_cast<void*>(0x1234), 91, 5);
    std::string error;
    assert(PrepareMapChestClientSyncCandidate(candidate(), &error));
    uint64_t ticket = 0;
    assert(QueueMapChestClientSyncAcceptedTransfer(
        91, 5, -9, acceptedResponse(), &ticket, &error));
    CancelMapChestClientSyncWindow(90);  // unrelated Close is ignored
    assert(GetMapChestClientSyncTicketState(ticket) ==
           MapChestClientSyncTicketState::Pending);
    CancelMapChestClientSyncWindow(91);
    assert(GetMapChestClientSyncTicketState(ticket) ==
           MapChestClientSyncTicketState::Cancelled);
    uint8_t slot = 0U;
    assert(!GetMapChestClientSyncSourceSlot(91, -9, &slot));
    MapChestClientSyncQueuedPacket packet;
    assert(!TakeMapChestClientSyncPacket(
        reinterpret_cast<void*>(0x1234), &packet));
    BindMapChestClientSyncIngress(reinterpret_cast<void*>(0x1234), 92, 5);
    assert(!PrepareMapChestClientSyncCandidate(candidate(), &error));
}

}  // namespace

int main() {
    prepareRequiresExactIngressAndSourceMap();
    oneAcceptedRequestQueuesOneClientPacket();
    closeOrDifferentIngressCancelsPending();
    build_import::ClearMapChestClientSync();
    return 0;
}
