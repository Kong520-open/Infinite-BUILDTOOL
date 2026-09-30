#ifndef INFINITE_TEXTURE_SIGN_EDIT_SESSION_MAILBOX_H
#define INFINITE_TEXTURE_SIGN_EDIT_SESSION_MAILBOX_H

#include <cstdint>
#include <string>
#include <string_view>

namespace build_import {

struct SignEditSessionResult {
    uint64_t token = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    bool front_side = true;
    std::string error;
};

enum class SignEditSessionPollState : uint8_t {
    Inactive = 0,
    WaitingForOpen = 1,
    Ready = 2,
    Failed = 3,
};

// Arms one packet-only sign interaction. The server's matching OpenSign packet
// is consumed by the raw receive hook, recording which face the server opened
// without allowing the local sign editor UI to appear.
void ArmSignEditSession(uint64_t token, int32_t x, int32_t y, int32_t z);
void CancelSignEditSession(uint64_t token) noexcept;
SignEditSessionPollState PollSignEditSession(uint64_t token,
                                             SignEditSessionResult* output);

// Returns true only for an OpenSign packet at the coordinates of the currently
// armed importer request. A matching malformed packet is also suppressed and
// reported as Failed so incompatible wire layouts cannot reach the game UI.
bool ObserveSignEditSessionPacket(std::string_view packet) noexcept;

}  // namespace build_import

#endif  // INFINITE_TEXTURE_SIGN_EDIT_SESSION_MAILBOX_H
