#include "ProjectionPrinterInventoryClientSync.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <string>

namespace {

constexpr uint32_t kContentHeader = (3U << 10U) | 0x31U;
char g_ingress;
char g_other_ingress;

void resetBoundInventoryClientSync() {
    build_import::ClearProjectionPrinterInventoryClientSync();
    build_import::BindProjectionPrinterInventoryClientSyncIngress(&g_ingress);
}

void appendVarUInt(std::string* output, uint32_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7FU);
        value >>= 7U;
        if (value != 0U) byte |= 0x80U;
        output->push_back(static_cast<char>(byte));
    } while (value != 0U);
}

void appendVarInt(std::string* output, int32_t value) {
    const uint32_t sign_mask = value < 0 ? 0xFFFFFFFFU : 0U;
    appendVarUInt(output, (static_cast<uint32_t>(value) << 1U) ^ sign_mask);
}

void appendLittleEndian16(std::string* output, uint16_t value) {
    output->push_back(static_cast<char>(value & 0xFFU));
    output->push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

std::string makeAir() {
    std::string output;
    appendVarInt(&output, 0);
    return output;
}

std::string makeItem(int32_t runtime_id, uint16_t count, uint32_t aux,
                     int32_t network_stack_id, int32_t block_runtime_id,
                     const std::string& user_data) {
    std::string output;
    appendVarInt(&output, runtime_id);
    appendLittleEndian16(&output, count);
    appendVarUInt(&output, aux);
    output.push_back(static_cast<char>(1));
    appendVarInt(&output, network_stack_id);
    appendVarInt(&output, block_runtime_id);
    appendVarUInt(&output, static_cast<uint32_t>(user_data.size()));
    output.append(user_data);
    return output;
}

void appendV859Context(std::string* output) {
    output->push_back(static_cast<char>(0));  // FullContainerName.ContainerName
    output->push_back(static_cast<char>(0));  // no dynamic container id
    output->append(makeAir());                // storage ItemData
}

std::string makeContent(const std::array<std::string, 36U>& slots) {
    std::string output;
    appendVarUInt(&output, kContentHeader);
    appendVarUInt(&output, 0U);
    appendVarUInt(&output, 36U);
    for (const std::string& slot : slots) output.append(slot);
    appendV859Context(&output);
    return output;
}

std::string makeSlotPacket(uint8_t slot, const std::string& item) {
    std::string output;
    appendVarUInt(&output, (kContentHeader & ~0x3FFU) | 0x32U);
    appendVarUInt(&output, 0U);
    appendVarUInt(&output, slot);
    appendV859Context(&output);
    output.append(item);
    return output;
}

build_import::ProjectionPrinterInventoryClientSyncMove makeMove(
    uint64_t revision, uint8_t source, uint8_t destination, int32_t expected_source,
    bool expected_destination_occupied, int32_t expected_destination,
    int32_t confirmed_source, int32_t confirmed_destination) {
    build_import::ProjectionPrinterInventoryClientSyncMove move;
    move.snapshot_revision = revision;
    move.source_inventory_slot = source;
    move.destination_hotbar_slot = destination;
    move.expected_source_network_stack_id = expected_source;
    move.expected_destination_occupied = expected_destination_occupied;
    move.expected_destination_network_stack_id = expected_destination;
    move.confirmed_source_network_stack_id = confirmed_source;
    move.confirmed_destination_network_stack_id = confirmed_destination;
    return move;
}

void testMoveRefreshUsesRawItemsAndConfirmedIds() {
    resetBoundInventoryClientSync();
    std::array<std::string, 36U> slots{};
    for (std::string& slot : slots) slot = makeAir();
    const std::string source = makeItem(4, 12U, 7U, 31, 93, "source-nbt");
    slots[11] = source;
    const std::string content = makeContent(slots);
    build_import::ObserveProjectionPrinterInventoryClientSyncPacket(content);

    build_import::ProjectionPrinterInventoryClientSyncSnapshotInfo info;
    assert(build_import::GetProjectionPrinterInventoryClientSyncSnapshotInfo(&info));
    assert(info.ready);
    assert(info.revision != 0U);

    const auto move = makeMove(info.revision, 11U, 4U, 31, false, 0, 0, 501);
    uint64_t ticket = 0;
    std::string error;
    assert(build_import::QueueProjectionPrinterInventoryClientSyncMove(move, &ticket, &error));
    assert(ticket != 0U);

    build_import::ProjectionPrinterInventoryClientSyncQueuedPacket first;
    build_import::ProjectionPrinterInventoryClientSyncQueuedPacket second;
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &first));
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &second));
    assert(first.ticket == ticket && first.packet_index == 0U);
    assert(second.ticket == ticket && second.packet_index == 1U);
    assert(first.bytes == makeSlotPacket(11U, makeAir()));
    assert(second.bytes == makeSlotPacket(
        4U, makeItem(4, 12U, 7U, 501, 93, "source-nbt")));
    assert(build_import::CompleteProjectionPrinterInventoryClientSyncPacket(ticket, 0U));
    assert(build_import::GetProjectionPrinterInventoryClientSyncTicketState(ticket) ==
           build_import::ProjectionPrinterInventoryClientSyncTicketState::Pending);
    assert(build_import::CompleteProjectionPrinterInventoryClientSyncPacket(ticket, 1U));
    assert(build_import::GetProjectionPrinterInventoryClientSyncTicketState(ticket) ==
           build_import::ProjectionPrinterInventoryClientSyncTicketState::Complete);
}

void testSwapRefreshUsesResponseIdsOnBothRawItems() {
    resetBoundInventoryClientSync();
    std::array<std::string, 36U> slots{};
    for (std::string& slot : slots) slot = makeAir();
    slots[17] = makeItem(6, 4U, 1U, 41, 99, "backpack-nbt");
    slots[2] = makeItem(8, 1U, 2U, 73, 101, "hotbar-nbt");
    build_import::ObserveProjectionPrinterInventoryClientSyncPacket(makeContent(slots));

    build_import::ProjectionPrinterInventoryClientSyncSnapshotInfo info;
    assert(build_import::GetProjectionPrinterInventoryClientSyncSnapshotInfo(&info));
    const auto move = makeMove(info.revision, 17U, 2U, 41, true, 73, 901, 1001);
    uint64_t ticket = 0;
    assert(build_import::QueueProjectionPrinterInventoryClientSyncMove(move, &ticket, nullptr));

    build_import::ProjectionPrinterInventoryClientSyncQueuedPacket first;
    build_import::ProjectionPrinterInventoryClientSyncQueuedPacket second;
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &first));
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &second));
    assert(first.bytes == makeSlotPacket(17U, makeItem(8, 1U, 2U, 901, 101,
                                                       "hotbar-nbt")));
    assert(second.bytes == makeSlotPacket(2U, makeItem(6, 4U, 1U, 1001, 99,
                                                        "backpack-nbt")));
    build_import::CancelProjectionPrinterInventoryClientSyncTicket(ticket);
    assert(build_import::GetProjectionPrinterInventoryClientSyncTicketState(ticket) ==
           build_import::ProjectionPrinterInventoryClientSyncTicketState::Cancelled);
}

void testStaleSnapshotAndWorldResetAreRejected() {
    resetBoundInventoryClientSync();
    std::array<std::string, 36U> slots{};
    for (std::string& slot : slots) slot = makeAir();
    slots[9] = makeItem(2, 1U, 0U, 7, 4, "x");
    build_import::ObserveProjectionPrinterInventoryClientSyncPacket(makeContent(slots));
    build_import::ProjectionPrinterInventoryClientSyncSnapshotInfo info;
    assert(build_import::GetProjectionPrinterInventoryClientSyncSnapshotInfo(&info));
    uint64_t ticket = 0;
    const auto stale = makeMove(info.revision + 1U, 9U, 0U, 7, false, 0, 0, 8);
    assert(!build_import::QueueProjectionPrinterInventoryClientSyncMove(stale, &ticket, nullptr));
    assert(ticket == 0U);

    const std::string start_game(1U, static_cast<char>(0x0BU));
    build_import::ObserveProjectionPrinterInventoryClientSyncPacket(start_game);
    assert(!build_import::GetProjectionPrinterInventoryClientSyncSnapshotInfo(&info));
}

void testPreparedRefreshCommitsPreMovePacketsAfterSnapshotAdvances() {
    resetBoundInventoryClientSync();
    std::array<std::string, 36U> slots{};
    for (std::string& slot : slots) slot = makeAir();
    slots[14] = makeItem(19, 6U, 3U, 121, 77, "pre-move-nbt");
    build_import::ObserveProjectionPrinterInventoryClientSyncPacket(makeContent(slots));

    build_import::ProjectionPrinterInventoryClientSyncSnapshotInfo info;
    assert(build_import::GetProjectionPrinterInventoryClientSyncSnapshotInfo(&info));
    const auto optimistic = makeMove(info.revision, 14U, 5U, 121, false, 0, 0, 121);
    build_import::ProjectionPrinterInventoryClientSyncPreparedMove prepared;
    assert(build_import::PrepareProjectionPrinterInventoryClientSyncMove(
        optimistic, &prepared, nullptr));
    assert(prepared.id != 0U);

    // A fast server response can update the passive content snapshot before the
    // game thread returns from the native Move. The prepared source->hotbar
    // pair must still be committed from the exact pre-move cache.
    slots[14] = makeAir();
    slots[5] = makeItem(19, 6U, 3U, 401, 77, "pre-move-nbt");
    build_import::ObserveProjectionPrinterInventoryClientSyncPacket(makeContent(slots));

    uint64_t ticket = 0U;
    assert(build_import::CommitProjectionPrinterInventoryClientSyncPreparedMove(
        &prepared, &ticket, nullptr));
    assert(prepared.id == 0U && ticket != 0U);
    build_import::ProjectionPrinterInventoryClientSyncQueuedPacket first;
    build_import::ProjectionPrinterInventoryClientSyncQueuedPacket second;
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &first));
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &second));
    assert(first.bytes == makeSlotPacket(14U, makeAir()));
    assert(second.bytes == makeSlotPacket(
        5U, makeItem(19, 6U, 3U, 121, 77, "pre-move-nbt")));
    build_import::CancelProjectionPrinterInventoryClientSyncTicket(ticket);
}

void testNativeRefreshWithoutContentPreservesMetadataAndResponseCorrection() {
    resetBoundInventoryClientSync();
    build_import::ProjectionPrinterInventoryClientSyncSnapshotInfo info;
    assert(!build_import::GetProjectionPrinterInventoryClientSyncSnapshotInfo(&info));
    auto move = makeMove(0U, 18U, 6U, 127, true, 31, 31, 127);
    move.expected_source_count = 43U;
    const auto source_after = makeSlotPacket(
        18U, makeItem(11, 3U, 8U, 31, 551, "original-hotbar-custom-nbt"));
    const auto destination_after = makeSlotPacket(
        6U, makeItem(9, 43U, 4U, 127, 991, "original-backpack-custom-nbt"));
    build_import::ProjectionPrinterInventoryClientSyncPreparedMove prepared;
    assert(build_import::PrepareProjectionPrinterInventoryClientSyncNativeMove(
        move, source_after, destination_after, &prepared, nullptr));
    build_import::ProjectionPrinterInventoryClientSyncQueuedPacket packet;
    assert(!build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
    uint64_t ticket = 0;
    assert(build_import::CommitProjectionPrinterInventoryClientSyncPreparedMove(
        &prepared, &ticket, nullptr));
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
    assert(packet.bytes == source_after && packet.packet_index == 0U);
    assert(build_import::CompleteProjectionPrinterInventoryClientSyncPacket(ticket, 0U));
    uint64_t correction_ticket = 0;
    assert(!build_import::QueueProjectionPrinterInventoryClientSyncResponseCorrection(
        ticket, 8192, 1, &correction_ticket, nullptr));
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
    assert(packet.bytes == destination_after && packet.packet_index == 1U);
    assert(build_import::CompleteProjectionPrinterInventoryClientSyncPacket(ticket, 1U));
    // Larger and smaller response varints must preserve NBT/count/aux byte-for-byte.
    assert(build_import::QueueProjectionPrinterInventoryClientSyncResponseCorrection(
        ticket, 8192, 1, &correction_ticket, nullptr));
    assert(correction_ticket != 0U && correction_ticket != ticket);
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
    assert(packet.bytes == makeSlotPacket(
        18U, makeItem(11, 3U, 8U, 8192, 551, "original-hotbar-custom-nbt")));
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
    assert(packet.bytes == makeSlotPacket(
        6U, makeItem(9, 43U, 4U, 1, 991, "original-backpack-custom-nbt")));
    assert(!build_import::GetProjectionPrinterInventoryClientSyncSnapshotInfo(&info));
    resetBoundInventoryClientSync();
}

void testNativeRefreshRejectsChangedStacksCountsAndMalformedPackets() {
    resetBoundInventoryClientSync();
    auto move = makeMove(0U, 20U, 1U, 555, false, 0, 0, 555);
    move.expected_source_count = 12U;
    const auto source_after = makeSlotPacket(20U, makeAir());
    const auto destination_after = makeSlotPacket(
        1U, makeItem(9, 12U, 1U, 555, 91, "custom-nbt"));
    build_import::ProjectionPrinterInventoryClientSyncPreparedMove prepared;
    assert(!build_import::PrepareProjectionPrinterInventoryClientSyncNativeMove(
        move, source_after, makeSlotPacket(
            1U, makeItem(9, 11U, 1U, 555, 91, "custom-nbt")), &prepared, nullptr));
    assert(!build_import::PrepareProjectionPrinterInventoryClientSyncNativeMove(
        move, source_after, makeSlotPacket(
            1U, makeItem(9, 12U, 1U, 556, 91, "custom-nbt")), &prepared, nullptr));
    assert(!build_import::PrepareProjectionPrinterInventoryClientSyncNativeMove(
        move, makeSlotPacket(21U, makeAir()), destination_after, &prepared, nullptr));
    assert(!build_import::PrepareProjectionPrinterInventoryClientSyncNativeMove(
        move, source_after, destination_after + "unexpected", &prepared, nullptr));
    assert(prepared.id == 0U);
    assert(build_import::PrepareProjectionPrinterInventoryClientSyncNativeMove(
        move, source_after, destination_after, &prepared, nullptr));
    uint64_t ticket = 0;
    assert(build_import::CommitProjectionPrinterInventoryClientSyncPreparedMove(
        &prepared, &ticket, nullptr));
    build_import::ProjectionPrinterInventoryClientSyncQueuedPacket packet;
    for (uint8_t index = 0; index < 2U; ++index) {
        assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
        assert(build_import::CompleteProjectionPrinterInventoryClientSyncPacket(ticket, index));
    }
    // A whole-stack move leaves air in the source. A response cannot inject
    // a new occupied source merely by supplying an arbitrary network ID.
    assert(!build_import::QueueProjectionPrinterInventoryClientSyncResponseCorrection(
        ticket, 7, 600, nullptr, nullptr));
    assert(build_import::QueueProjectionPrinterInventoryClientSyncResponseCorrection(
        ticket, 0, 600, nullptr, nullptr));
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
    assert(packet.bytes == source_after);
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
    assert(packet.bytes == makeSlotPacket(1U, makeItem(9, 12U, 1U, 600, 91, "custom-nbt")));
    resetBoundInventoryClientSync();
}

build_import::ProjectionPrinterInventoryClientSyncPreparedMove prepareIngressTestMove() {
    auto move = makeMove(0U, 19U, 3U, 43, false, 0, 0, 43);
    move.expected_source_count = 8U;
    build_import::ProjectionPrinterInventoryClientSyncPreparedMove prepared;
    assert(build_import::PrepareProjectionPrinterInventoryClientSyncNativeMove(
        move, makeSlotPacket(19U, makeAir()),
        makeSlotPacket(3U, makeItem(9, 8U, 1U, 43, 91, "bounded-test-data")),
        &prepared, nullptr));
    return prepared;
}

uint64_t queueIngressTestMove() {
    auto prepared = prepareIngressTestMove();
    uint64_t ticket = 0U;
    assert(build_import::CommitProjectionPrinterInventoryClientSyncPreparedMove(
        &prepared, &ticket, nullptr));
    assert(ticket != 0U);
    return ticket;
}

void testOnlyBoundIngressCanDrainInSourceDestinationOrder() {
    build_import::ClearProjectionPrinterInventoryClientSync();
    const uint64_t ticket = queueIngressTestMove();
    build_import::ProjectionPrinterInventoryClientSyncQueuedPacket packet;
    packet.bytes = "untouched";
    assert(!build_import::TakeProjectionPrinterInventoryClientSyncPacket(nullptr, &packet));
    assert(!build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
    assert(packet.bytes == "untouched");
    build_import::BindProjectionPrinterInventoryClientSyncIngress(&g_ingress);
    assert(!build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_other_ingress, &packet));
    assert(packet.bytes == "untouched");
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
    assert(packet.ticket == ticket && packet.packet_index == 0U);
    assert(packet.bytes == makeSlotPacket(19U, makeAir()));
    assert(build_import::CompleteProjectionPrinterInventoryClientSyncPacket(ticket, 0U));
    // Re-observing the same game connection must not clear the destination half.
    build_import::BindProjectionPrinterInventoryClientSyncIngress(&g_ingress);
    assert(!build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_other_ingress, &packet));
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
    assert(packet.ticket == ticket && packet.packet_index == 1U);
    assert(packet.bytes == makeSlotPacket(
        3U, makeItem(9, 8U, 1U, 43, 91, "bounded-test-data")));
    assert(build_import::CompleteProjectionPrinterInventoryClientSyncPacket(ticket, 1U));
    assert(build_import::GetProjectionPrinterInventoryClientSyncTicketState(ticket) ==
           build_import::ProjectionPrinterInventoryClientSyncTicketState::Complete);
    assert(!build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
}

void testClearCancelsOldUpdatesAndRequiresRebinding() {
    resetBoundInventoryClientSync();
    const uint64_t ticket = queueIngressTestMove();
    auto prepared = prepareIngressTestMove();
    build_import::ClearProjectionPrinterInventoryClientSync();
    assert(build_import::GetProjectionPrinterInventoryClientSyncTicketState(ticket) ==
           build_import::ProjectionPrinterInventoryClientSyncTicketState::Cancelled);
    uint64_t rejected_ticket = 0U;
    assert(!build_import::CommitProjectionPrinterInventoryClientSyncPreparedMove(
        &prepared, &rejected_ticket, nullptr));
    const uint64_t new_ticket = queueIngressTestMove();
    assert(new_ticket != ticket);
    build_import::ProjectionPrinterInventoryClientSyncQueuedPacket packet;
    assert(!build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
    build_import::BindProjectionPrinterInventoryClientSyncIngress(&g_ingress);
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
    assert(packet.ticket == new_ticket && packet.packet_index == 0U);
    build_import::BindProjectionPrinterInventoryClientSyncIngress(nullptr);
    assert(build_import::GetProjectionPrinterInventoryClientSyncTicketState(new_ticket) ==
           build_import::ProjectionPrinterInventoryClientSyncTicketState::Cancelled);
    assert(!build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
}

void testConnectionChangeCancelsQueuedAndPreparedOldWorldUpdates() {
    resetBoundInventoryClientSync();
    const uint64_t ticket = queueIngressTestMove();
    auto prepared = prepareIngressTestMove();
    build_import::ProjectionPrinterInventoryClientSyncQueuedPacket packet;
    assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
    assert(packet.packet_index == 0U);
    build_import::BindProjectionPrinterInventoryClientSyncIngress(&g_other_ingress);
    assert(build_import::GetProjectionPrinterInventoryClientSyncTicketState(ticket) ==
           build_import::ProjectionPrinterInventoryClientSyncTicketState::Cancelled);
    assert(!build_import::CompleteProjectionPrinterInventoryClientSyncPacket(ticket, 0U));
    assert(!build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
    assert(!build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_other_ingress, &packet));
    uint64_t rejected_ticket = 0U;
    assert(!build_import::CommitProjectionPrinterInventoryClientSyncPreparedMove(
        &prepared, &rejected_ticket, nullptr));
    const uint64_t new_ticket = queueIngressTestMove();
    assert(new_ticket != ticket);
    for (uint8_t index = 0U; index < 2U; ++index) {
        assert(!build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_ingress, &packet));
        assert(build_import::TakeProjectionPrinterInventoryClientSyncPacket(&g_other_ingress, &packet));
        assert(packet.ticket == new_ticket && packet.packet_index == index);
        assert(build_import::CompleteProjectionPrinterInventoryClientSyncPacket(new_ticket, index));
    }
    build_import::ClearProjectionPrinterInventoryClientSync();
}

void testNativeSlotDecodeKeepsItemTypeCountAndNetworkIdentity() {
    build_import::ProjectionPrinterNativeSlotItem decoded;
    const std::string blank_map = makeSlotPacket(
        3U, makeItem(41, 5U, 2U, 301, 0, "blank-map-nbt"));
    assert(build_import::DecodeProjectionPrinterNativeInventorySlotItem(
        blank_map, 3U, &decoded));
    assert(decoded.occupied && decoded.runtime_item_id == 41 &&
           decoded.count == 5U && decoded.aux == 2U &&
           decoded.has_network_stack_id && decoded.network_stack_id == 301);

    const std::string filled_map = makeSlotPacket(
        3U, makeItem(42, 1U, 0U, 302, 0, "filled-map-nbt"));
    assert(build_import::DecodeProjectionPrinterNativeInventorySlotItem(
        filled_map, 3U, &decoded));
    assert(decoded.occupied && decoded.runtime_item_id == 42 &&
           decoded.count == 1U && decoded.network_stack_id == 302);
    assert(!build_import::DecodeProjectionPrinterNativeInventorySlotItem(
        filled_map, 4U, &decoded));

    assert(build_import::DecodeProjectionPrinterNativeInventorySlotItem(
        makeSlotPacket(3U, makeAir()), 3U, &decoded));
    assert(!decoded.occupied && decoded.runtime_item_id == 0 && decoded.count == 0U);
    std::string malformed = blank_map;
    malformed.push_back('\0');
    assert(!build_import::DecodeProjectionPrinterNativeInventorySlotItem(
        malformed, 3U, &decoded));
}

}  // namespace

int main() {
    testMoveRefreshUsesRawItemsAndConfirmedIds();
    testSwapRefreshUsesResponseIdsOnBothRawItems();
    testStaleSnapshotAndWorldResetAreRejected();
    testPreparedRefreshCommitsPreMovePacketsAfterSnapshotAdvances();
    testNativeRefreshWithoutContentPreservesMetadataAndResponseCorrection();
    testNativeRefreshRejectsChangedStacksCountsAndMalformedPackets();
    testOnlyBoundIngressCanDrainInSourceDestinationOrder();
    testClearCancelsOldUpdatesAndRequiresRebinding();
    testConnectionChangeCancelsQueuedAndPreparedOldWorldUpdates();
    testNativeSlotDecodeKeepsItemTypeCountAndNetworkIdentity();
    return 0;
}
