#include "../ProjectionPrinterInventorySession.h"

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

void varUInt64(std::string* output, uint64_t value) {
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

void varInt64(std::string* output, int64_t value) {
    const uint64_t encoded = (static_cast<uint64_t>(value) << 1U) ^
        static_cast<uint64_t>(value >> 63);
    varUInt64(output, encoded);
}

void emptyItem(std::string* output) {
    varInt(output, 0);
}

void item(std::string* output) {
    varInt(output, 5);
    output->push_back(64);
    output->push_back(0);
    varUInt(output, 7);
    output->push_back(1);
    varInt(output, 91);
    varInt(output, 0);
    varUInt(output, 0);
}

std::string playerOpen(uint8_t window_id) {
    std::string packet;
    varUInt(&packet, 0x2EU);
    packet.push_back(static_cast<char>(window_id));
    packet.push_back(static_cast<char>(0xFFU));
    varInt(&packet, 0);
    varUInt(&packet, 0U);
    varInt(&packet, 0);
    varInt64(&packet, -1);
    return packet;
}

std::string nonPlayerOpen(uint8_t window_id) {
    std::string packet = playerOpen(window_id);
    packet[2] = 0;  // packet ID + window ID use one byte in this fixture.
    return packet;
}

std::string closePacket(uint8_t window_id, uint8_t container_type = 0xFFU) {
    std::string packet;
    varUInt(&packet, 0x2FU);
    packet.push_back(static_cast<char>(window_id));
    packet.push_back(static_cast<char>(container_type));
    packet.push_back(0);
    return packet;
}

std::string controlPacket(uint32_t packet_id) {
    std::string packet;
    varUInt(&packet, packet_id);
    return packet;
}

std::string playerInventoryContent() {
    std::string packet;
    varUInt(&packet, 0x31U);
    varUInt(&packet, 0U);
    varUInt(&packet, 36U);
    for (uint32_t slot = 0; slot < 36U; ++slot) {
        if (slot == 2U) {
            item(&packet);
        } else {
            emptyItem(&packet);
        }
    }
    packet.push_back(0); // FullContainerName container ID
    packet.push_back(0); // no dynamic ID
    emptyItem(&packet);  // storage item
    return packet;
}

}  // namespace

int main() {
    using namespace build_import;

    ClearProjectionPrinterInventorySession();
    ProjectionPrinterInventorySessionResult result;
    assert(ObserveProjectionPrinterInventorySessionPacket(playerOpen(3)) ==
           ProjectionPrinterInventorySessionReceiveDisposition::Pass);
    assert(ObserveProjectionPrinterInventorySessionPacket(playerInventoryContent()) ==
           ProjectionPrinterInventorySessionReceiveDisposition::Pass);
    assert(PollProjectionPrinterInventorySession(1, &result) ==
           ProjectionPrinterInventorySessionPollState::Inactive);

    assert(ArmProjectionPrinterInventorySession(11));
    assert(!ArmProjectionPrinterInventorySession(12));
    assert(PollProjectionPrinterInventorySession(11, &result) ==
           ProjectionPrinterInventorySessionPollState::WaitingForOpen);
    assert(ObserveProjectionPrinterInventorySessionPacket(nonPlayerOpen(9)) ==
           ProjectionPrinterInventorySessionReceiveDisposition::Pass);
    assert(PollProjectionPrinterInventorySession(11, &result) ==
           ProjectionPrinterInventorySessionPollState::WaitingForOpen);

    assert(ObserveProjectionPrinterInventorySessionPacket(playerOpen(17)) ==
           ProjectionPrinterInventorySessionReceiveDisposition::ActiveOpen);
    assert(PollProjectionPrinterInventorySession(11, &result) ==
           ProjectionPrinterInventorySessionPollState::WaitingForContent);
    assert(result.container_opened && result.container_id == 17U &&
           result.container_type == kProjectionPrinterPlayerInventoryContainerType);
    assert(ObserveProjectionPrinterInventorySessionPacket(closePacket(18)) ==
           ProjectionPrinterInventorySessionReceiveDisposition::Pass);
    assert(ObserveProjectionPrinterInventorySessionPacket(playerInventoryContent()) ==
           ProjectionPrinterInventorySessionReceiveDisposition::Pass);
    assert(PollProjectionPrinterInventorySession(11, &result) ==
           ProjectionPrinterInventorySessionPollState::Ready);
    assert(result.inventory_content_received && result.requires_client_close);

    ProjectionPrinterInventorySessionCloseRequest close;
    assert(GetProjectionPrinterInventorySessionCloseRequest(11, &close));
    assert(close.container_id == 17U && close.requires_client_close);
    MarkProjectionPrinterInventorySessionCloseSent(11, close.container_id,
                                                   close.container_type);
    assert(PollProjectionPrinterInventorySession(11, &result) ==
           ProjectionPrinterInventorySessionPollState::WaitingForClose);
    assert(ObserveProjectionPrinterInventorySessionPacket(closePacket(17)) ==
           ProjectionPrinterInventorySessionReceiveDisposition::Pass);
    assert(PollProjectionPrinterInventorySession(11, &result) ==
           ProjectionPrinterInventorySessionPollState::Closed);
    assert(result.container_closed);
    CancelProjectionPrinterInventorySession(11);

    // A login/world/dimension boundary must not let a silent player inventory
    // session inherit InventoryContent from the next world.
    ClearProjectionPrinterInventorySession();
    assert(ArmProjectionPrinterInventorySession(19));
    assert(ObserveProjectionPrinterInventorySessionPacket(playerOpen(5)) ==
           ProjectionPrinterInventorySessionReceiveDisposition::ActiveOpen);
    assert(ObserveProjectionPrinterInventorySessionPacket(controlPacket(0x0BU)) ==
           ProjectionPrinterInventorySessionReceiveDisposition::WorldReset);
    assert(PollProjectionPrinterInventorySession(19, &result) ==
           ProjectionPrinterInventorySessionPollState::Inactive);

    // A malformed player content packet after a captured player open is still
    // delivered to the game, but produces a deterministic session failure.
    ClearProjectionPrinterInventorySession();
    assert(ArmProjectionPrinterInventorySession(21));
    assert(ObserveProjectionPrinterInventorySessionPacket(playerOpen(0)) ==
           ProjectionPrinterInventorySessionReceiveDisposition::ActiveOpen);
    std::string truncated = playerInventoryContent();
    truncated.pop_back();
    assert(ObserveProjectionPrinterInventorySessionPacket(truncated) ==
           ProjectionPrinterInventorySessionReceiveDisposition::Pass);
    assert(PollProjectionPrinterInventorySession(21, &result) ==
           ProjectionPrinterInventorySessionPollState::Failed);
    assert(result.error.find("InventoryContent") != std::string::npos);
    ProjectionPrinterInventorySessionCloseRequest zero_close;
    assert(GetProjectionPrinterInventorySessionCloseRequest(21, &zero_close));
    assert(!zero_close.requires_client_close);
    CancelProjectionPrinterInventorySession(21);

    // A timeout before the server answers still suppresses the delayed player
    // window from the quarantined tail and exposes its nonzero ID for one
    // best-effort close.
    ClearProjectionPrinterInventorySession();
    assert(ArmProjectionPrinterInventorySession(31));
    CancelProjectionPrinterInventorySession(31);
    assert(!ArmProjectionPrinterInventorySession(32));
    assert(ObserveProjectionPrinterInventorySessionPacket(playerOpen(23)) ==
           ProjectionPrinterInventorySessionReceiveDisposition::QuarantinedOpen);
    ProjectionPrinterInventorySessionQuarantine quarantine;
    assert(PollProjectionPrinterInventorySessionQuarantine(&quarantine));
    assert(quarantine.container_opened && quarantine.container_id == 23U &&
           quarantine.requires_client_close && !quarantine.client_close_sent);
    assert(ObserveProjectionPrinterInventorySessionPacket(playerInventoryContent()) ==
           ProjectionPrinterInventorySessionReceiveDisposition::Pass);
    assert(PollProjectionPrinterInventorySessionQuarantine(&quarantine));
    assert(quarantine.inventory_content_received);
    MarkProjectionPrinterInventorySessionQuarantineCloseSent(23U, 0xFFU);
    assert(PollProjectionPrinterInventorySessionQuarantine(&quarantine));
    assert(quarantine.client_close_sent);
    assert(ObserveProjectionPrinterInventorySessionPacket(closePacket(23)) ==
           ProjectionPrinterInventorySessionReceiveDisposition::Pass);
    assert(PollProjectionPrinterInventorySessionQuarantine(&quarantine));
    assert(quarantine.container_closed);

    ClearProjectionPrinterInventorySession();
    assert(!PollProjectionPrinterInventorySessionQuarantine(&quarantine));
    assert(ArmProjectionPrinterInventorySession(41));
    CancelProjectionPrinterInventorySession(41);
    ClearProjectionPrinterInventorySession();
    return 0;
}
