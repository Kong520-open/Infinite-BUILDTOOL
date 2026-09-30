#include "SignEditSessionMailbox.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <mutex>

namespace build_import {
namespace {

// Bedrock protocol v859 (1.21.120).
constexpr uint32_t kOpenSignPacketId = 303U;
constexpr size_t kMaximumErrorBytes = 191U;

class Reader {
public:
    explicit Reader(std::string_view input) : input_(input) {}

    bool byte(uint8_t* output) {
        if (!output || cursor_ >= input_.size()) return false;
        *output = static_cast<uint8_t>(input_[cursor_++]);
        return true;
    }

    bool varUInt(uint32_t* output) {
        if (!output) return false;
        uint32_t value = 0;
        for (uint32_t index = 0; index < 5U && cursor_ < input_.size(); ++index) {
            const uint8_t current = static_cast<uint8_t>(input_[cursor_++]);
            if (index == 4U && (current & 0xF0U) != 0U) return false;
            value |= static_cast<uint32_t>(current & 0x7FU) << (index * 7U);
            if ((current & 0x80U) == 0U) {
                *output = value;
                return true;
            }
        }
        return false;
    }

    bool varInt(int32_t* output) {
        uint32_t encoded = 0;
        if (!output || !varUInt(&encoded)) return false;
        *output = static_cast<int32_t>((encoded >> 1U) ^
                                      (0U - (encoded & 1U)));
        return true;
    }

    bool atEnd() const { return cursor_ == input_.size(); }

private:
    std::string_view input_;
    size_t cursor_ = 0;
};

struct Mailbox {
    bool active = false;
    uint64_t token = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    bool open_seen = false;
    bool front_side = true;
    uint16_t error_length = 0;
    std::array<char, kMaximumErrorBytes + 1U> error{};
};

std::mutex g_mutex;
Mailbox g_mailbox;

void setErrorLocked(const char* message) {
    g_mailbox.error.fill('\0');
    g_mailbox.error_length = 0;
    if (!message) return;
    const size_t length = std::min(std::strlen(message), kMaximumErrorBytes);
    std::memcpy(g_mailbox.error.data(), message, length);
    g_mailbox.error_length = static_cast<uint16_t>(length);
}

void populateResultLocked(SignEditSessionResult* output) {
    if (!output) return;
    *output = {};
    output->token = g_mailbox.token;
    output->x = g_mailbox.x;
    output->y = g_mailbox.y;
    output->z = g_mailbox.z;
    output->front_side = g_mailbox.front_side;
    output->error.assign(g_mailbox.error.data(), g_mailbox.error_length);
}

}  // namespace

void ArmSignEditSession(uint64_t token, int32_t x, int32_t y, int32_t z) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mailbox = {};
    g_mailbox.active = token != 0U;
    g_mailbox.token = token;
    g_mailbox.x = x;
    g_mailbox.y = y;
    g_mailbox.z = z;
}

void CancelSignEditSession(uint64_t token) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_mailbox.active && g_mailbox.token == token) g_mailbox = {};
    } catch (...) {
    }
}

SignEditSessionPollState PollSignEditSession(uint64_t token,
                                             SignEditSessionResult* output) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_mailbox.active || token == 0U || g_mailbox.token != token) {
        if (output) *output = {};
        return SignEditSessionPollState::Inactive;
    }
    populateResultLocked(output);
    if (g_mailbox.error_length != 0U) return SignEditSessionPollState::Failed;
    if (!g_mailbox.open_seen) return SignEditSessionPollState::WaitingForOpen;
    return SignEditSessionPollState::Ready;
}

bool ObserveSignEditSessionPacket(std::string_view packet) noexcept {
    try {
        if (packet.empty()) return false;
        Reader reader(packet);
        uint32_t wire_header = 0;
        if (!reader.varUInt(&wire_header) ||
            (wire_header & 0x3FFU) != kOpenSignPacketId) {
            return false;
        }

        int32_t x = 0;
        int32_t z = 0;
        uint32_t encoded_y = 0;
        if (!reader.varInt(&x) || !reader.varUInt(&encoded_y) ||
            !reader.varInt(&z)) {
            return false;
        }
        const int32_t y = static_cast<int32_t>(encoded_y);

        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_mailbox.active || x != g_mailbox.x || y != g_mailbox.y ||
            z != g_mailbox.z) {
            return false;
        }

        uint8_t front_side = 0;
        if (!reader.byte(&front_side) || front_side > 1U || !reader.atEnd()) {
            setErrorLocked(
                "matching OpenSign packet does not match the protocol v859 layout");
            return true;
        }
        if (!g_mailbox.open_seen) {
            g_mailbox.open_seen = true;
            g_mailbox.front_side = front_side != 0U;
            setErrorLocked(nullptr);
        }
        return true;
    } catch (...) {
        try {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_mailbox.active) setErrorLocked("sign edit packet capture failed");
        } catch (...) {
        }
        return false;
    }
}

}  // namespace build_import
