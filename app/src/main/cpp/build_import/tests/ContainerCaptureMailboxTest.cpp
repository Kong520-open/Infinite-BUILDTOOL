#include "../ContainerCaptureMailbox.h"
#include "../MapChestUiCloseBridge.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

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

void le16(std::string* output, uint16_t value) {
    output->push_back(static_cast<char>(value & 0xFFU));
    output->push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void le32(std::string* output, uint32_t value) {
    for (unsigned int index = 0; index < 4U; ++index) {
        output->push_back(static_cast<char>((value >> (index * 8U)) & 0xFFU));
    }
}

void le64(std::string* output, uint64_t value) {
    for (unsigned int index = 0; index < 8U; ++index) {
        output->push_back(static_cast<char>((value >> (index * 8U)) & 0xFFU));
    }
}

std::string openPacket(uint8_t window, int32_t x, int32_t y, int32_t z,
                       uint8_t type = 0U) {
    std::string packet;
    varUInt(&packet, 0x2EU);
    packet.push_back(static_cast<char>(window));
    packet.push_back(static_cast<char>(type));
    varInt(&packet, x);
    varUInt(&packet, static_cast<uint32_t>(y));
    varInt(&packet, z);
    varInt(&packet, -1);
    return packet;
}

std::string closePacket(uint8_t window, uint8_t type = 0U) {
    std::string packet;
    varUInt(&packet, 0x2FU);
    packet.push_back(static_cast<char>(window));
    packet.push_back(static_cast<char>(type));
    packet.push_back(1);
    return packet;
}

void emptyItem(std::string* output) {
    varInt(output, 0);
}

void item(std::string* output, int32_t numeric_id, uint16_t count,
          uint32_t damage, bool has_network_id,
          size_t user_data_bytes = 3U,
          const std::string* explicit_user_data = nullptr) {
    varInt(output, numeric_id);
    output->push_back(static_cast<char>(count & 0xFFU));
    output->push_back(static_cast<char>((count >> 8U) & 0xFFU));
    varUInt(output, damage);
    output->push_back(has_network_id ? 1 : 0);
    if (has_network_id) varInt(output, 91);
    varInt(output, 0);
    if (explicit_user_data) {
        varUInt(output, static_cast<uint32_t>(explicit_user_data->size()));
        output->append(*explicit_user_data);
    } else {
        varUInt(output, static_cast<uint32_t>(user_data_bytes));
        output->append(user_data_bytes, 'n');
    }
}

std::string mapUserData(int64_t uuid, const std::string* custom_name = nullptr) {
    std::string output;
    le16(&output, 0xFFFFU);
    output.push_back(1); // network little-endian NBT codec
    output.push_back(10); // TAG_Compound root
    le16(&output, 0U); // unnamed root
    output.push_back(4); // TAG_Long
    le16(&output, 8U);
    output.append("map_uuid");
    le64(&output, static_cast<uint64_t>(uuid));
    if (custom_name) {
        output.push_back(8); // candidate root CustomName TAG_String
        le16(&output, 10U);
        output.append("CustomName");
        le16(&output, static_cast<uint16_t>(custom_name->size()));
        output.append(*custom_name);
    }
    output.push_back(0); // TAG_End
    le32(&output, 0U); // can_place_on
    le32(&output, 0U); // can_destroy
    return output;
}

std::string singleChestPacket(uint32_t window, bool dynamic_id,
                              int64_t uuid = -12345,
                              uint8_t full_container_name = 7U,
                              const std::string* custom_name = nullptr) {
    std::string packet;
    varUInt(&packet, 0x31U);
    varUInt(&packet, window);
    varUInt(&packet, 27U);
    emptyItem(&packet);
    const std::string nbt = mapUserData(uuid, custom_name);
    item(&packet, 358, 1, 0, true, 0U, &nbt);
    for (int index = 2; index < 27; ++index) emptyItem(&packet);
    // FullContainerName 0 is observed on manually opened chests.
    packet.push_back(static_cast<char>(full_container_name));
    packet.push_back(dynamic_id ? 1 : 0);
    if (dynamic_id) le32(&packet, 9001U);
    emptyItem(&packet); // storage item
    return packet;
}

std::string singleChestSlotPacket(uint32_t window, uint32_t slot,
                                  int64_t uuid, bool optional_context,
                                  const std::string* custom_name = nullptr) {
    std::string packet;
    varUInt(&packet, 0x32U);
    varUInt(&packet, window);
    varUInt(&packet, slot);
    if (optional_context) {
        packet.push_back(0); // no optional FullContainerName
        packet.push_back(0); // no optional storage item
    } else {
        packet.push_back(7); // chest request container name
        packet.push_back(0); // no dynamic container ID
        emptyItem(&packet); // storage item
    }
    const std::string nbt = mapUserData(uuid, custom_name);
    item(&packet, 358, 1, 0, true, 0U, &nbt);
    return packet;
}

void currentInventoryTail(std::string* output) {
    // v748+, including protocol v859 (1.21.120): FullContainerName followed
    // by a storage ItemData. Use a null dynamic ID and an empty storage item.
    varUInt(output, 0U);
    output->push_back(0);
    emptyItem(output);
}

std::string inventoryPacket(uint32_t window, bool empty = false,
                            size_t first_item_user_data_bytes = 3U) {
    std::string packet;
    varUInt(&packet, 0x31U);
    varUInt(&packet, window);
    varUInt(&packet, empty ? 2U : 3U);
    emptyItem(&packet);
    if (empty) {
        emptyItem(&packet);
    } else {
        item(&packet, 5, 64, 7, true, first_item_user_data_bytes);
        item(&packet, -17, 2, 65535, false);
    }
    currentInventoryTail(&packet);
    return packet;
}

std::string emptyAnvilPacket(uint32_t window) {
    std::string packet;
    varUInt(&packet, 0x31U);
    varUInt(&packet, window);
    varUInt(&packet, 3U);
    for (int slot = 0; slot < 3; ++slot) emptyItem(&packet);
    currentInventoryTail(&packet);
    return packet;
}

}  // namespace

int main() {
    using namespace build_import;

    ContainerCaptureResult result;
    VisibleChestCaptureEvent chest_event;
    // Diagnostic visible-chest mode still captures the exact server window,
    // but every packet for that window reaches the native client. A legacy
    // hidden capture cannot steal its exclusive token.
    assert(!TryArmVisibleChestCapture(0U, 80, 70, 81));
    assert(TryArmVisibleChestCapture(801U, 80, 70, 81));
    assert(!TryArmVisibleChestCapture(802U, 80, 70, 81));
    assert(!TryArmHiddenChestCapture(803U, 80, 70, 81));
    ArmContainerCapture(804U, 80, 70, 81);
    assert(PollContainerCapture(804U, &result) ==
           ContainerCapturePollState::Inactive);
    assert(!ObserveContainerCapturePacket(
        openPacket(80, 81, 70, 81), nullptr, &chest_event));
    assert(chest_event.kind == VisibleChestCaptureEventKind::None);
    assert(PollContainerCapture(801U, &result) ==
           ContainerCapturePollState::WaitingForOpen);
    assert(!ObserveContainerCapturePacket(
        openPacket(81, 80, 70, 81), nullptr, &chest_event));
    assert(chest_event.kind == VisibleChestCaptureEventKind::Open &&
           chest_event.token == 801U && chest_event.window_id == 81U);
    assert(PollContainerCapture(801U, &result) ==
           ContainerCapturePollState::WaitingForContent &&
           result.container_opened && result.container_id == 81U &&
           result.container_type == 0U);
    assert(!ObserveContainerCapturePacket(singleChestPacket(
        82, false, 123456, 0U)));
    assert(!ObserveContainerCapturePacket(singleChestPacket(
        81, false, 123456, 0U)));
    assert(PollContainerCapture(801U, &result) ==
           ContainerCapturePollState::Ready && result.slot_count == 27U &&
           result.items.size() == 1U && result.items[0].has_map_uuid &&
           result.items[0].map_uuid == 123456);
    assert(IsExactOpenVisibleChestCapture(801U, 81U, 0U, 80, 70, 81));
    assert(!IsExactOpenVisibleChestCapture(801U, 82U, 0U, 80, 70, 81));
    assert(!IsExactOpenVisibleChestCapture(801U, 81U, 5U, 80, 70, 81));
    assert(!IsExactOpenVisibleChestCapture(801U, 81U, 0U, 80, 70, 82));
    assert(QueueMapChestUiClose(801U, 81U, 0U, 80, 70, 81));
    assert(!QueueMapChestUiClose(801U, 81U, 0U, 80, 70, 81));
    assert(TakePendingMapChestUiCloseRequest() == 801U);
    assert(TakePendingMapChestUiCloseRequest() == 0U);
    assert(IsMapChestUiCloseStillSafe(801U));
    MarkMapChestUiCloseDispatched(801U);
    assert(WasMapChestUiCloseDispatched(801U));
    assert(!ObserveContainerCapturePacket(singleChestSlotPacket(
        81, 0U, 123456, true)));
    assert(!ObserveContainerCapturePacket(
        closePacket(81, 5U), nullptr, &chest_event));
    assert(chest_event.kind == VisibleChestCaptureEventKind::None);
    assert(PollContainerCapture(801U, &result) ==
           ContainerCapturePollState::Ready && !result.container_closed);
    assert(!ObserveContainerCapturePacket(
        closePacket(81), nullptr, &chest_event));
    assert(chest_event.kind == VisibleChestCaptureEventKind::Close &&
           chest_event.token == 801U && chest_event.window_id == 81U);
    assert(!ObserveContainerCapturePacket(
        closePacket(81), nullptr, &chest_event));
    assert(chest_event.kind == VisibleChestCaptureEventKind::None);
    assert(PollContainerCapture(801U, &result) ==
           ContainerCapturePollState::Ready && result.container_closed);
    assert(!IsExactOpenVisibleChestCapture(801U, 81U, 0U, 80, 70, 81));
    CancelMapChestUiClose(801U);
    assert(!WasMapChestUiCloseDispatched(801U));
    CancelContainerCapture(801U);
    ContainerCaptureQuarantine visible_chest_quarantine;
    assert(!PollContainerCaptureQuarantine(&visible_chest_quarantine));
    assert(!ObserveContainerCapturePacket(singleChestSlotPacket(
        81, 1U, 123456, true)));

    // Even a later Open that reuses the same coordinates is ambiguous: it
    // may have replaced the tool's visible screen before Java sends Back.
    assert(TryArmVisibleChestCapture(805U, 80, 70, 81));
    assert(!ObserveContainerCapturePacket(openPacket(82, 80, 70, 81)));
    assert(!ObserveContainerCapturePacket(singleChestPacket(
        82, false, 123456, 0U)));
    assert(IsExactOpenVisibleChestCapture(805U, 82U, 0U, 80, 70, 81));
    assert(!ObserveContainerCapturePacket(
        openPacket(83, 80, 70, 81), nullptr, &chest_event));
    assert(chest_event.kind == VisibleChestCaptureEventKind::None);
    assert(!IsExactOpenVisibleChestCapture(805U, 82U, 0U, 80, 70, 81));
    assert(!QueueMapChestUiClose(805U, 82U, 0U, 80, 70, 81));
    assert(!ObserveContainerCapturePacket(closePacket(82)));
    CancelContainerCapture(805U);

    // A server Close after queue but before Java claims the ticket also
    // prevents an accidental Back on the next screen.
    assert(TryArmVisibleChestCapture(806U, 80, 70, 81));
    assert(!ObserveContainerCapturePacket(openPacket(84, 80, 70, 81)));
    assert(!ObserveContainerCapturePacket(singleChestPacket(
        84, false, 123456, 0U)));
    assert(QueueMapChestUiClose(806U, 84U, 0U, 80, 70, 81));
    assert(!ObserveContainerCapturePacket(closePacket(84)));
    assert(TakePendingMapChestUiCloseRequest() == 0U);
    CancelContainerCapture(806U);

    // A deliberately armed automatic anvil must be observed without hiding
    // any packet from the stock client, which owns the live recipe and output
    // NetIdVariant. An unrelated window/type must not join that capture.
    assert(!ArmVisibleAnvilCapture(0U, 90, 70, 80));
    assert(ArmVisibleAnvilCapture(901U, 90, 70, 80));
    assert(!ArmVisibleAnvilCapture(902U, 90, 70, 80));
    VisibleAnvilCaptureEvent visible_event;
    assert(!ObserveContainerCapturePacket(
        openPacket(71, 91, 70, 80, 5U), &visible_event));
    assert(visible_event.kind == VisibleAnvilCaptureEventKind::None);
    assert(PollContainerCapture(901U, &result) ==
           ContainerCapturePollState::WaitingForOpen);
    assert(!ObserveContainerCapturePacket(openPacket(72, 90, 70, 80, 0U)));
    assert(PollContainerCapture(901U, &result) ==
           ContainerCapturePollState::Failed);
    assert(!ObserveContainerCapturePacket(
        openPacket(73, 90, 70, 80, 5U), &visible_event));
    assert(visible_event.kind == VisibleAnvilCaptureEventKind::Open &&
           visible_event.token == 901U && visible_event.window_id == 73U);
    assert(!ObserveContainerCapturePacket(
        openPacket(73, 90, 70, 80, 5U), &visible_event));
    assert(visible_event.kind == VisibleAnvilCaptureEventKind::None);
    assert(PollContainerCapture(901U, &result) ==
           ContainerCapturePollState::WaitingForContent);
    assert(result.container_opened && result.container_id == 73U &&
           result.container_type == 5U && !result.container_closed);
    assert(!ObserveContainerCapturePacket(inventoryPacket(74, true)));
    assert(!ObserveContainerCapturePacket(emptyAnvilPacket(73)));
    assert(PollContainerCapture(901U, &result) ==
           ContainerCapturePollState::Ready);
    assert(result.slot_count == 3U && result.items.empty());
    // A concurrent hidden chest arm cannot evict the active visible window.
    ArmContainerCapture(999U, 1, 2, 3);
    assert(PollContainerCapture(901U, &result) ==
           ContainerCapturePollState::Ready);
    assert(PollContainerCapture(999U, nullptr) ==
           ContainerCapturePollState::Inactive);
    assert(!ObserveContainerCapturePacket(
        singleChestSlotPacket(73, 0U, 123456, true)));
    assert(!ObserveContainerCapturePacket(closePacket(73, 0U), &visible_event));
    assert(visible_event.kind == VisibleAnvilCaptureEventKind::None);
    assert(PollContainerCapture(901U, &result) ==
           ContainerCapturePollState::Ready && !result.container_closed);
    assert(!ObserveContainerCapturePacket(closePacket(73, 5U), &visible_event));
    assert(visible_event.kind == VisibleAnvilCaptureEventKind::Close &&
           visible_event.token == 901U && visible_event.window_id == 73U);
    assert(PollContainerCapture(901U, &result) ==
           ContainerCapturePollState::Ready && result.container_closed);
    CancelContainerCapture(901U);
    assert(HasVisibleAnvilServerCloseReceipt(901U, 73U));
    assert(!HasVisibleAnvilServerCloseReceipt(901U, 74U));
    ContainerCaptureQuarantine visible_quarantine;
    assert(!PollContainerCaptureQuarantine(&visible_quarantine));
    assert(!ObserveContainerCapturePacket(inventoryPacket(73, true)));

    // The caller may cancel after sending its client Close but before a
    // server Close arrives. Once Open reached the stock client, the late
    // packets are never hidden by mailbox quarantine.
    assert(ArmVisibleAnvilCapture(902U, 93, 70, 80));
    assert(!ObserveContainerCapturePacket(openPacket(74, 93, 70, 80, 5U)));
    assert(!ObserveContainerCapturePacket(emptyAnvilPacket(74)));
    CancelContainerCapture(902U);
    assert(!HasVisibleAnvilServerCloseReceipt(902U, 74U));
    assert(!PollContainerCaptureQuarantine(&visible_quarantine));
    assert(!ObserveContainerCapturePacket(
        singleChestSlotPacket(74, 0U, 123456, true)));
    assert(!ObserveContainerCapturePacket(closePacket(75, 5U)));
    assert(!ObserveContainerCapturePacket(closePacket(74, 0U)));
    std::string truncated_visible_close;
    varUInt(&truncated_visible_close, 0x2FU);
    truncated_visible_close.push_back(74);
    assert(!ObserveContainerCapturePacket(truncated_visible_close));
    assert(!HasVisibleAnvilServerCloseReceipt(902U, 74U));
    assert(!ObserveContainerCapturePacket(closePacket(74, 5U), &visible_event));
    assert(visible_event.kind == VisibleAnvilCaptureEventKind::Close &&
           visible_event.token == 902U && visible_event.window_id == 74U);
    assert(HasVisibleAnvilServerCloseReceipt(902U, 74U));
    assert(!HasVisibleAnvilServerCloseReceipt(901U, 74U));

    // A malformed Content still reaches the stock client in visible mode;
    // only our passive capture fails, and the owner can close the window.
    assert(ArmVisibleAnvilCapture(904U, 94, 70, 80));
    assert(!HasVisibleAnvilServerCloseReceipt(902U, 74U));
    assert(!ObserveContainerCapturePacket(openPacket(78, 94, 70, 80, 5U)));
    std::string malformed_anvil_content;
    varUInt(&malformed_anvil_content, 0x31U);
    varUInt(&malformed_anvil_content, 78U);
    varUInt(&malformed_anvil_content, 3U);
    assert(!ObserveContainerCapturePacket(malformed_anvil_content));
    assert(PollContainerCapture(904U, &result) ==
           ContainerCapturePollState::Failed && !result.error.empty());
    assert(!ObserveContainerCapturePacket(closePacket(78, 5U)));
    CancelContainerCapture(904U);

    // A world reset clears a pending close receipt. A numerically identical
    // Close in the next world cannot become evidence for the old ticket.
    assert(ArmVisibleAnvilCapture(905U, 95, 70, 80));
    assert(!ObserveContainerCapturePacket(openPacket(79, 95, 70, 80, 5U)));
    CancelContainerCapture(905U);
    assert(!HasVisibleAnvilServerCloseReceipt(905U, 79U));
    std::string start_game;
    varUInt(&start_game, 0x0BU);
    assert(!ObserveContainerCapturePacket(start_game));
    assert(!ObserveContainerCapturePacket(closePacket(79, 5U)));
    assert(!HasVisibleAnvilServerCloseReceipt(905U, 79U));

    // Cancellation before Open is different: suppress the delayed exact
    // type-5 Open and its tail, since no automatic operation will own its UI.
    assert(ArmVisibleAnvilCapture(903U, 92, 70, 80));
    CancelContainerCapture(903U);
    assert(!ObserveContainerCapturePacket(openPacket(75, 92, 70, 80, 0U)));
    assert(ObserveContainerCapturePacket(openPacket(76, 92, 70, 80, 5U)));
    assert(PollContainerCaptureQuarantine(&visible_quarantine) &&
           visible_quarantine.token == 903U &&
           visible_quarantine.from_visible_anvil &&
           visible_quarantine.container_opened &&
           visible_quarantine.container_type == 5U);
    assert(ObserveContainerCapturePacket(inventoryPacket(76, true)));
    assert(ObserveContainerCapturePacket(closePacket(76, 5U)));
    assert(!ObserveContainerCapturePacket(inventoryPacket(76, true)));

    assert(!ObserveContainerCapturePacket(openPacket(1, 1, 2, 3)));
    assert(!ObserveContainerCapturePacket(inventoryPacket(1)));
    assert(!ObserveContainerCapturePacket(closePacket(1)));

    ArmContainerCapture(11, -4, -12, 9);
    assert(!ObserveContainerCapturePacket(inventoryPacket(2)));
    assert(!ObserveContainerCapturePacket(closePacket(2)));
    assert(!ObserveContainerCapturePacket(openPacket(2, -4, -11, 9)));
    assert(PollContainerCapture(11, &result) ==
           ContainerCapturePollState::WaitingForOpen);

    assert(ObserveContainerCapturePacket(openPacket(2, -4, -12, 9)));
    assert(PollContainerCapture(11, &result) ==
           ContainerCapturePollState::WaitingForContent);
    assert(result.container_id == 2 && result.container_opened &&
           !result.container_closed);
    assert(result.container_opened);

    assert(!ObserveContainerCapturePacket(inventoryPacket(3)));
    assert(PollContainerCapture(11, &result) ==
           ContainerCapturePollState::WaitingForContent);

    assert(ObserveContainerCapturePacket(inventoryPacket(2)));
    assert(PollContainerCapture(11, &result) == ContainerCapturePollState::Ready);
    assert(result.slot_count == 3 && result.items.size() == 2);
    assert(result.items[0].slot == 1 && result.items[0].numeric_id == 5 &&
           result.items[0].count == 64 && result.items[0].aux == 7 &&
           result.items[0].has_network_stack_id &&
           result.items[0].network_stack_id == 91 &&
           !result.items[0].has_map_uuid);
    assert(result.items[1].slot == 2 && result.items[1].numeric_id == -17 &&
           result.items[1].count == 2 && result.items[1].aux == 65535 &&
           !result.items[1].has_network_stack_id);
    assert(result.has_full_container_name && result.full_container_name == 0U &&
           !result.has_dynamic_container_id);
    // Slot updates to a hidden packet-only window must not reach a missing
    // client-side container UI. Ordinary inventory IDs remain untouched.
    assert(ObserveContainerCapturePacket(
        singleChestSlotPacket(2, 0, 123456, true)));
    assert(!ObserveContainerCapturePacket(
        singleChestSlotPacket(3, 0, 123456, true)));

    CancelContainerCapture(10);
    assert(PollContainerCapture(11, nullptr) == ContainerCapturePollState::Ready);
    CancelContainerCapture(11);
    assert(PollContainerCapture(11, &result) == ContainerCapturePollState::Inactive);
    assert(ObserveContainerCapturePacket(
        singleChestSlotPacket(2, 0, 123456, true)));
    assert(ObserveContainerCapturePacket(closePacket(2)));
    assert(!ObserveContainerCapturePacket(
        singleChestSlotPacket(2, 0, 123456, true)));

    // A timeout can race a delayed server ContainerOpen.  The old request is
    // quarantined so its packets never reach a UI that did not receive the
    // original opening packet, and the game tick can close that late window.
    ArmContainerCapture(111, -4, -12, 9);
    CancelContainerCapture(111);
    assert(ObserveContainerCapturePacket(openPacket(22, -4, -12, 9)));
    ContainerCaptureQuarantine quarantine;
    assert(PollContainerCaptureQuarantine(&quarantine));
    assert(quarantine.container_opened && !quarantine.container_closed &&
           !quarantine.content_captured && !quarantine.close_sent &&
           quarantine.container_id == 22U);
    MarkContainerCaptureQuarantineCloseSent(22U, 0U);
    assert(PollContainerCaptureQuarantine(&quarantine));
    assert(quarantine.close_sent);
    assert(ObserveContainerCapturePacket(inventoryPacket(22, true)));
    assert(ObserveContainerCapturePacket(closePacket(22)));
    assert(PollContainerCaptureQuarantine(&quarantine));
    assert(quarantine.container_closed);

    // A normal completed capture also needs a small filter window: the game
    // did not receive its ContainerOpen packet, so a late server close must
    // not be delivered to the UI after the exporter has sent its own close.
    ArmContainerCapture(112, 11, 12, 13);
    assert(ObserveContainerCapturePacket(openPacket(23, 11, 12, 13)));
    assert(ObserveContainerCapturePacket(inventoryPacket(23, true)));
    assert(PollContainerCapture(112, &result) == ContainerCapturePollState::Ready);
    CancelContainerCapture(112);
    assert(PollContainerCaptureQuarantine(&quarantine));
    assert(quarantine.container_opened && quarantine.content_captured &&
           !quarantine.close_sent && quarantine.container_id == 23U);
    // A same-numbered Close for another container type must not retire the
    // hidden chest, even after its capture moved into quarantine.
    assert(!ObserveContainerCapturePacket(closePacket(23, 5U)));
    assert(PollContainerCaptureQuarantine(&quarantine));
    assert(!quarantine.container_closed);
    assert(ObserveContainerCapturePacket(closePacket(23)));
    assert(PollContainerCaptureQuarantine(&quarantine));
    assert(quarantine.container_closed);

    ArmContainerCapture(12, 1, 2, 3);
    assert(ObserveContainerCapturePacket(openPacket(7, 1, 2, 3)));
    assert(!ObserveContainerCapturePacket(closePacket(6)));
    assert(!ObserveContainerCapturePacket(closePacket(7, 5U)));
    assert(PollContainerCapture(12, &result) ==
           ContainerCapturePollState::WaitingForContent);
    assert(!result.container_closed);
    assert(ObserveContainerCapturePacket(closePacket(7)));
    assert(!ObserveContainerCapturePacket(inventoryPacket(7)));
    assert(!ObserveContainerCapturePacket(closePacket(7)));
    assert(PollContainerCapture(12, &result) == ContainerCapturePollState::Failed);
    assert(result.container_opened && result.container_closed);

    ArmContainerCapture(13, 1, 2, 3);
    assert(ObserveContainerCapturePacket(openPacket(8, 1, 2, 3)));
    std::string truncated;
    varUInt(&truncated, 0x31U);
    varUInt(&truncated, 8);
    varUInt(&truncated, 1);
    varInt(&truncated, 5);
    assert(ObserveContainerCapturePacket(truncated));
    assert(PollContainerCapture(13, &result) == ContainerCapturePollState::Failed);
    assert(result.container_opened && result.container_id == 8);
    assert(!result.container_closed);

    // The v685+ close tail is required. A legacy one-byte close must not make
    // the runtime believe the current server window has already closed.
    std::string legacy_close;
    varUInt(&legacy_close, 0x2FU);
    legacy_close.push_back(8);
    assert(!ObserveContainerCapturePacket(legacy_close));
    assert(PollContainerCapture(13, &result) == ContainerCapturePollState::Failed);
    assert(!result.container_closed);
    assert(ObserveContainerCapturePacket(closePacket(8)));
    assert(PollContainerCapture(13, &result) == ContainerCapturePollState::Failed);
    assert(result.container_closed);

    ArmContainerCapture(14, 7, 8, 9);
    assert(ObserveContainerCapturePacket(openPacket(9, 7, 8, 9)));
    assert(ObserveContainerCapturePacket(inventoryPacket(9, true)));
    assert(PollContainerCapture(14, &result) == ContainerCapturePollState::Ready);
    assert(result.slot_count == 2 && result.items.empty());
    assert(ObserveContainerCapturePacket(closePacket(9)));
    assert(PollContainerCapture(14, &result) == ContainerCapturePollState::Ready);
    assert(result.container_opened && result.container_closed);
    CancelContainerCapture(14);

    // ContainerOpen includes entityUniqueId in current protocol versions.
    // Do not arm a window from a packet truncated immediately after BlockPos.
    ArmContainerCapture(15, 4, 5, 6);
    std::string truncated_open = openPacket(10, 4, 5, 6);
    truncated_open.pop_back();
    assert(!ObserveContainerCapturePacket(truncated_open));
    assert(PollContainerCapture(15, &result) ==
           ContainerCapturePollState::WaitingForOpen);
    CancelContainerCapture(15);

    // The parser discards item NBT, but it must accept the same 5 MiB item
    // bound as the current Bedrock codec rather than rejecting valid chests.
    ArmContainerCapture(16, 10, 11, 12);
    assert(ObserveContainerCapturePacket(openPacket(11, 10, 11, 12)));
    assert(ObserveContainerCapturePacket(
        inventoryPacket(11, false, 65537U)));
    assert(PollContainerCapture(16, &result) == ContainerCapturePollState::Ready);
    assert(result.items.size() == 2U);
    CancelContainerCapture(16);

    ArmContainerCapture(17, 10, 11, 12);
    assert(ObserveContainerCapturePacket(openPacket(12, 10, 11, 12)));
    assert(ObserveContainerCapturePacket(
        inventoryPacket(12, false, 5U * 1024U * 1024U)));
    assert(PollContainerCapture(17, &result) == ContainerCapturePollState::Ready);
    assert(result.items.size() == 2U);
    CancelContainerCapture(17);

    ArmContainerCapture(18, 10, 11, 12);
    assert(ObserveContainerCapturePacket(openPacket(13, 10, 11, 12)));
    assert(ObserveContainerCapturePacket(
        inventoryPacket(13, false, 5U * 1024U * 1024U + 1U)));
    assert(PollContainerCapture(18, &result) == ContainerCapturePollState::Failed);
    assert(result.error.find("item payload") != std::string::npos);
    CancelContainerCapture(18);

    ArmContainerCapture(19, 10, 11, 12);
    assert(ObserveContainerCapturePacket(openPacket(14, 10, 11, 12)));
    std::string oversized_packet = inventoryPacket(14, true);
    oversized_packet.resize(10U * 1024U * 1024U + 1U, '\0');
    assert(ObserveContainerCapturePacket(oversized_packet));
    assert(PollContainerCapture(19, &result) == ContainerCapturePollState::Failed);
    assert(result.error.find("safety limit") != std::string::npos);
    CancelContainerCapture(19);

    // A real single-chest Content packet uses FullContainerName 0 with no
    // dynamic ID. That is not the request target's container-slot type 7.
    // The exact open window, position and token still have to match.
    ArmContainerCapture(20, 30, 70, 40);
    assert(ObserveContainerCapturePacket(openPacket(40, 30, 70, 40)));
    assert(ObserveContainerCapturePacket(singleChestPacket(40, false, -12345, 0U)));
    assert(PollContainerCapture(20, &result) == ContainerCapturePollState::Ready);
    assert(result.has_full_container_name && result.full_container_name == 0U &&
           !result.has_dynamic_container_id && result.dynamic_container_id == 0U);
    assert(result.items.size() == 1U && result.items[0].slot == 1U &&
           result.items[0].has_network_stack_id &&
           result.items[0].network_stack_id == 91 &&
           result.items[0].has_map_uuid && result.items[0].map_uuid == -12345 &&
           result.items[0].name_status == MapItemNameStatus::Missing &&
           result.items[0].name_candidate.empty());
    uint8_t empty_slot = 255U;
    std::string selection_error;
    assert(SelectEmptySingleChestSlot(result, 20, 30, 70, 40,
                                      &empty_slot, &selection_error));
    assert(empty_slot == 0U && selection_error.empty());
    assert(!SelectEmptySingleChestSlot(result, 21, 30, 70, 40,
                                       &empty_slot, &selection_error));
    assert(!selection_error.empty());
    ContainerCaptureResult invalid = result;
    invalid.slot_count = 54U;
    assert(!SelectEmptySingleChestSlot(invalid, 20, 30, 70, 40,
                                       &empty_slot, nullptr));
    invalid = result;
    invalid.full_container_name = 7U;
    assert(!SelectEmptySingleChestSlot(invalid, 20, 30, 70, 40,
                                       &empty_slot, nullptr));
    invalid = result;
    invalid.has_dynamic_container_id = true;
    invalid.dynamic_container_id = 9001U;
    assert(!SelectEmptySingleChestSlot(invalid, 20, 30, 70, 40,
                                       &empty_slot, nullptr));
    CancelContainerCapture(20);

    // Export still captures a dynamic item-backed container, but it is not
    // mistaken for the new ordinary block chest storage target.
    ArmContainerCapture(21, 31, 70, 40);
    assert(ObserveContainerCapturePacket(openPacket(41, 31, 70, 40)));
    assert(ObserveContainerCapturePacket(singleChestPacket(41, true, -12345, 0U)));
    assert(PollContainerCapture(21, &result) == ContainerCapturePollState::Ready);
    assert(result.items.size() == 1U && result.items[0].has_map_uuid &&
           result.has_dynamic_container_id && result.dynamic_container_id == 9001U);
    assert(!SelectEmptySingleChestSlot(result, 21, 31, 70, 40,
                                       &empty_slot, nullptr));
    CancelContainerCapture(21);

    // A map name is read only from the already length-delimited ItemExtraData
    // field. The root CustomName location is a candidate, not proof of the
    // target client's anvil naming ABI. Negative UUIDs remain valid evidence.
    const std::string candidate_name = "Map R1 C1";
    ArmContainerCapture(23, 70, 80, 90);
    assert(ObserveContainerCapturePacket(openPacket(60, 70, 80, 90)));
    assert(ObserveContainerCapturePacket(singleChestPacket(
        60, false, -532575944698LL, 0U, &candidate_name)));
    assert(PollContainerCapture(23, &result) == ContainerCapturePollState::Ready);
    assert(result.items.size() == 1U && result.items[0].has_map_uuid &&
           result.items[0].map_uuid == -532575944698LL &&
           result.items[0].name_status == MapItemNameStatus::Present &&
           result.items[0].name_source == MapItemNameSource::RootCustomName &&
           result.items[0].name_candidate == candidate_name);
    CancelContainerCapture(23);

    const std::string invalid_name(1U, static_cast<char>(0xFF));
    ArmContainerCapture(24, 71, 80, 90);
    assert(ObserveContainerCapturePacket(openPacket(61, 71, 80, 90)));
    assert(ObserveContainerCapturePacket(singleChestPacket(
        61, false, -532575944698LL, 0U, &invalid_name)));
    assert(PollContainerCapture(24, &result) == ContainerCapturePollState::Ready);
    assert(result.items.size() == 1U && result.items[0].has_map_uuid &&
           result.items[0].map_uuid == -532575944698LL &&
           result.items[0].name_status == MapItemNameStatus::Unusable &&
           result.items[0].name_candidate.empty());
    CancelContainerCapture(24);

    // The manual trace is read-only. A player-opened chest still reaches the
    // stock UI, and content/slot changes are decoded without arming capture.
    const std::string manual_open = openPacket(51, 50, 70, 40);
    const std::string manual_content = singleChestPacket(51, false, 123456);
    ManualChestTraceEvent trace;
    assert(ObserveManualChestTracePacket(manual_open, &trace));
    assert(trace.kind == ManualChestTraceEventKind::Open &&
           trace.container_id == 51 && trace.container_type == 0 &&
           trace.x == 50 && trace.y == 70 && trace.z == 40);
    assert(!ObserveContainerCapturePacket(manual_open));
    assert(ObserveManualChestTracePacket(manual_content, &trace));
    assert(trace.kind == ManualChestTraceEventKind::Content && trace.parsed &&
           trace.slot_count == 27 && trace.item_count == 1 &&
           trace.has_full_container_name && trace.full_container_name == 7 &&
           !trace.has_dynamic_container_id && trace.map_uuid_count == 1 &&
           trace.map_sample_count == 1 && trace.map_samples[0].slot == 1 &&
           trace.map_samples[0].map_uuid == 123456);
    assert(!ObserveContainerCapturePacket(manual_content));
    const std::string named_manual_content =
        singleChestPacket(51, false, -532575944698LL, 0U, &candidate_name);
    assert(ObserveManualChestTracePacket(named_manual_content, &trace));
    assert(trace.kind == ManualChestTraceEventKind::Content && trace.parsed &&
           trace.map_uuid_count == 1U && trace.map_sample_count == 1U &&
           trace.map_samples[0].map_uuid == -532575944698LL &&
           trace.map_samples[0].name_status == MapItemNameStatus::Present &&
           trace.map_samples[0].name_source == MapItemNameSource::RootCustomName &&
           trace.map_samples[0].name_bytes == candidate_name.size());
    const std::string manual_slot =
        singleChestSlotPacket(51, 2, 123456, true);
    assert(ObserveManualChestTracePacket(manual_slot, &trace));
    assert(trace.kind == ManualChestTraceEventKind::Slot && trace.parsed &&
           trace.changed_slot == 2 && trace.slot_occupied &&
           trace.slot_has_network_stack_id && trace.slot_network_stack_id == 91 &&
           trace.slot_has_map_uuid && trace.slot_map_uuid == 123456 &&
           trace.slot_name_status == MapItemNameStatus::Missing);
    const std::string named_manual_slot =
        singleChestSlotPacket(51, 2, -532575944698LL, true, &candidate_name);
    assert(ObserveManualChestTracePacket(named_manual_slot, &trace));
    assert(trace.kind == ManualChestTraceEventKind::Slot && trace.parsed &&
           trace.slot_map_uuid == -532575944698LL &&
           trace.slot_name_status == MapItemNameStatus::Present &&
           trace.slot_name_source == MapItemNameSource::RootCustomName &&
           trace.slot_name_bytes == candidate_name.size());
    const std::string manual_slot_v859 =
        singleChestSlotPacket(51, 3, 123456, false);
    assert(ObserveManualChestTracePacket(manual_slot_v859, &trace));
    assert(trace.kind == ManualChestTraceEventKind::Slot &&
           trace.changed_slot == 3 && trace.slot_map_uuid == 123456);

    // The user's real manual chest sends FullContainerName=0, not 7. It is
    // safe for passive observation after a matching type-0 Open + 27 slots;
    // this does not relax the separate automatic-transfer identity gate.
    const std::string manual_content_name0 =
        singleChestPacket(51, false, 654321, 0U);
    assert(ObserveManualChestTracePacket(manual_content_name0, &trace));
    assert(trace.kind == ManualChestTraceEventKind::Content && trace.parsed &&
           trace.slot_count == 27 && trace.has_full_container_name &&
           trace.full_container_name == 0 && !trace.has_dynamic_container_id &&
           trace.map_uuid_count == 1 && trace.map_samples[0].map_uuid == 654321);
    const std::string manual_slot_after_name0 =
        singleChestSlotPacket(51, 4, 654321, true);
    assert(ObserveManualChestTracePacket(manual_slot_after_name0, &trace));
    assert(trace.kind == ManualChestTraceEventKind::Slot &&
           trace.changed_slot == 4 && trace.slot_has_map_uuid &&
           trace.slot_map_uuid == 654321);

    // Client-side manual Close need not yield an incoming Close packet. The
    // server may reuse the same window number and block position on reopen.
    assert(ObserveManualChestTracePacket(manual_open, &trace));
    assert(trace.kind == ManualChestTraceEventKind::Open &&
           trace.container_id == 51 && trace.x == 50);
    assert(ObserveManualChestTracePacket(manual_content_name0, &trace));
    assert(trace.kind == ManualChestTraceEventKind::Content && trace.parsed &&
           trace.map_uuid_count == 1 && trace.map_samples[0].map_uuid == 654321);
    assert(!ObserveManualChestTracePacket(inventoryPacket(52), &trace));
    assert(!ObserveManualChestTracePacket(
        singleChestSlotPacket(52, 2, 123456, true), &trace));
    assert(ObserveManualChestTracePacket(closePacket(51), &trace));
    assert(trace.kind == ManualChestTraceEventKind::Close &&
           trace.close_server_initiated);
    assert(!ObserveContainerCapturePacket(closePacket(51)));
    assert(!ObserveManualChestTracePacket(manual_slot, &trace));

    ArmContainerCapture(22, 60, 70, 40);
    const std::string automatic_open = openPacket(52, 60, 70, 40);
    assert(!ObserveManualChestTracePacket(automatic_open, &trace));
    assert(ObserveContainerCapturePacket(automatic_open));
    assert(!ObserveManualChestTracePacket(singleChestPacket(52, false), &trace));
    CancelContainerCapture(22);

    // A map-storage chest claim is atomic and cannot be stolen by a legacy
    // export capture. The ordinary hidden export behavior above is unchanged.
    assert(!TryArmHiddenChestCapture(0U, 9, 10, 11));
    // A just-cancelled export may still have a packet tail in quarantine.
    // Existing tests have no virtual clock, so first settle it by observing
    // the matching server close if a window was actually opened.
    assert(ObserveContainerCapturePacket(closePacket(52)));
    // The quarantine itself remains briefly active; a new exclusive claim
    // must fail rather than risk a reused numeric window ID.
    assert(!TryArmHiddenChestCapture(1001U, 9, 10, 11));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    assert(TryArmHiddenChestCapture(1001U, 9, 10, 11));
    assert(!TryArmHiddenChestCapture(1002U, 9, 10, 11));
    ArmContainerCapture(1003U, 7, 8, 9);
    assert(PollContainerCapture(1001U, &result) ==
           ContainerCapturePollState::WaitingForOpen);
    assert(PollContainerCapture(1003U, &result) ==
           ContainerCapturePollState::Inactive);
    assert(ObserveContainerCapturePacket(openPacket(64, 9, 10, 11)));
    assert(ObserveContainerCapturePacket(
        singleChestPacket(64, false, 123456, 0U)));
    assert(PollContainerCapture(1001U, &result) ==
           ContainerCapturePollState::Ready);
    CancelContainerCapture(1001U);

    // If the diagnostic capture is cancelled before its Open arrives, only
    // that delayed type-0 window is quarantined; an unrelated type-5 window
    // remains visible and cannot be mistaken for the cancelled chest.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    assert(TryArmVisibleChestCapture(805U, 85, 70, 86));
    CancelContainerCapture(805U);
    assert(!ObserveContainerCapturePacket(openPacket(84, 85, 70, 86, 5U)));
    assert(ObserveContainerCapturePacket(openPacket(85, 85, 70, 86)));
    assert(PollContainerCaptureQuarantine(&visible_chest_quarantine) &&
           visible_chest_quarantine.token == 805U &&
           visible_chest_quarantine.container_opened &&
           visible_chest_quarantine.container_type == 0U);
    assert(ObserveContainerCapturePacket(singleChestPacket(
        85, false, 123456, 0U)));
    assert(ObserveContainerCapturePacket(singleChestSlotPacket(
        85, 0U, 123456, true)));
    assert(ObserveContainerCapturePacket(closePacket(85)));
    // A pending visible chest Open from an old world cannot bind a client
    // refresh ingress after its numeric window ID is reused in the new world.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    assert(TryArmVisibleChestCapture(807U, 85, 70, 86));
    std::string chest_world_reset;
    varUInt(&chest_world_reset, 0x0BU);
    assert(!ObserveContainerCapturePacket(
        chest_world_reset, nullptr, &chest_event));
    assert(chest_event.kind == VisibleChestCaptureEventKind::None);
    assert(!ObserveContainerCapturePacket(
        openPacket(86, 85, 70, 86), nullptr, &chest_event));
    assert(chest_event.kind == VisibleChestCaptureEventKind::None);
    CancelContainerCapture(807U);
    return 0;
}
