#include "../SignEditSessionMailbox.h"

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
    const uint32_t encoded = (static_cast<uint32_t>(value) << 1U) ^
        static_cast<uint32_t>(value >> 31);
    varUInt(output, encoded);
}

std::string openSignPacket(int32_t x, int32_t y, int32_t z, bool front,
                           uint32_t sender_subclient = 0U) {
    std::string packet;
    varUInt(&packet, 303U | (sender_subclient << 10U));
    varInt(&packet, x);
    varUInt(&packet, static_cast<uint32_t>(y));
    varInt(&packet, z);
    packet.push_back(front ? 1 : 0);
    return packet;
}

}  // namespace

int main() {
    using namespace build_import;

    SignEditSessionResult result;
    assert(!ObserveSignEditSessionPacket(openSignPacket(1, 2, 3, true)));

    ArmSignEditSession(11, -17, -60, 29);
    assert(!ObserveSignEditSessionPacket(openSignPacket(-16, -60, 29, true)));
    assert(PollSignEditSession(11, &result) ==
           SignEditSessionPollState::WaitingForOpen);
    assert(result.token == 11 && result.x == -17 && result.y == -60 &&
           result.z == 29);

    assert(ObserveSignEditSessionPacket(openSignPacket(-17, -60, 29, false, 2)));
    assert(PollSignEditSession(11, &result) == SignEditSessionPollState::Ready);
    assert(!result.front_side && result.error.empty());
    // A duplicate matching packet remains suppressed and cannot change the
    // face selected by the first server acknowledgement.
    assert(ObserveSignEditSessionPacket(openSignPacket(-17, -60, 29, true)));
    assert(PollSignEditSession(11, &result) == SignEditSessionPollState::Ready);
    assert(!result.front_side);

    CancelSignEditSession(10);
    assert(PollSignEditSession(11, nullptr) == SignEditSessionPollState::Ready);
    CancelSignEditSession(11);
    assert(PollSignEditSession(11, &result) == SignEditSessionPollState::Inactive);

    ArmSignEditSession(12, 4, 5, 6);
    std::string wrong_id = openSignPacket(4, 5, 6, true);
    wrong_id[0] = 1;
    assert(!ObserveSignEditSessionPacket(wrong_id));
    assert(PollSignEditSession(12, &result) ==
           SignEditSessionPollState::WaitingForOpen);

    std::string truncated = openSignPacket(4, 5, 6, true);
    truncated.pop_back();
    assert(ObserveSignEditSessionPacket(truncated));
    assert(PollSignEditSession(12, &result) == SignEditSessionPollState::Failed);
    assert(result.error.find("v859") != std::string::npos);
    CancelSignEditSession(12);

    ArmSignEditSession(13, 4, 5, 6);
    std::string invalid_boolean = openSignPacket(4, 5, 6, true);
    invalid_boolean.back() = 2;
    assert(ObserveSignEditSessionPacket(invalid_boolean));
    assert(PollSignEditSession(13, &result) == SignEditSessionPollState::Failed);
    CancelSignEditSession(13);

    ArmSignEditSession(14, 4, 5, 6);
    std::string trailing = openSignPacket(4, 5, 6, true);
    trailing.push_back('\0');
    assert(ObserveSignEditSessionPacket(trailing));
    assert(PollSignEditSession(14, &result) == SignEditSessionPollState::Failed);
    CancelSignEditSession(14);

    ArmSignEditSession(15, 7, 8, 9);
    std::string truncated_position;
    varUInt(&truncated_position, 303U);
    varInt(&truncated_position, 7);
    assert(!ObserveSignEditSessionPacket(truncated_position));
    assert(PollSignEditSession(15, &result) ==
           SignEditSessionPollState::WaitingForOpen);
    CancelSignEditSession(15);
    return 0;
}
