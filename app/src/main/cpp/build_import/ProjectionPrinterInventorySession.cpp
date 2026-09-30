#include "ProjectionPrinterInventorySession.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>

namespace build_import {
namespace {

constexpr uint32_t kContainerOpenPacketId = 0x2EU;
constexpr uint32_t kContainerClosePacketId = 0x2FU;
constexpr uint32_t kInventoryContentPacketId = 0x31U;
constexpr uint32_t kDisconnectPacketId = 0x05U;
constexpr uint32_t kStartGamePacketId = 0x0BU;
constexpr uint32_t kChangeDimensionPacketId = 0x3DU;
constexpr size_t kMaximumInventoryPacketBytes = 10U * 1024U * 1024U;
constexpr uint32_t kMaximumItemUserDataBytes = 5U * 1024U * 1024U;
constexpr size_t kMaximumErrorBytes = 191U;
constexpr auto kIncompleteQuarantineDuration = std::chrono::seconds(1);
constexpr auto kCompletedQuarantineDuration = std::chrono::milliseconds(500);

class Reader {
public:
    explicit Reader(std::string_view input) : input_(input) {}

    bool byte(uint8_t* output) {
        if (!output || cursor_ >= input_.size()) return false;
        *output = static_cast<uint8_t>(input_[cursor_++]);
        return true;
    }

    bool littleEndian16(uint16_t* output) {
        if (!output || input_.size() - cursor_ < 2U) return false;
        *output = static_cast<uint16_t>(static_cast<uint8_t>(input_[cursor_])) |
            static_cast<uint16_t>(static_cast<uint8_t>(input_[cursor_ + 1U]) << 8U);
        cursor_ += 2U;
        return true;
    }

    bool littleEndian32(uint32_t* output) {
        if (!output || input_.size() - cursor_ < 4U) return false;
        *output = static_cast<uint32_t>(static_cast<uint8_t>(input_[cursor_])) |
            static_cast<uint32_t>(static_cast<uint8_t>(input_[cursor_ + 1U]) << 8U) |
            static_cast<uint32_t>(static_cast<uint8_t>(input_[cursor_ + 2U]) << 16U) |
            static_cast<uint32_t>(static_cast<uint8_t>(input_[cursor_ + 3U]) << 24U);
        cursor_ += 4U;
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

    bool varUInt64(uint64_t* output) {
        if (!output) return false;
        uint64_t value = 0;
        for (uint32_t index = 0; index < 10U && cursor_ < input_.size(); ++index) {
            const uint8_t current = static_cast<uint8_t>(input_[cursor_++]);
            if (index == 9U && (current & 0xFEU) != 0U) return false;
            value |= static_cast<uint64_t>(current & 0x7FU) << (index * 7U);
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
        *output = static_cast<int32_t>((encoded >> 1U) ^ (0U - (encoded & 1U)));
        return true;
    }

    bool varInt64(int64_t* output) {
        uint64_t encoded = 0;
        if (!output || !varUInt64(&encoded)) return false;
        *output = static_cast<int64_t>((encoded >> 1U) ^
                                      (0ULL - (encoded & 1ULL)));
        return true;
    }

    bool skip(size_t count) {
        if (count > input_.size() - cursor_) return false;
        cursor_ += count;
        return true;
    }

    bool atEnd() const { return cursor_ == input_.size(); }

private:
    std::string_view input_;
    size_t cursor_ = 0;
};

struct ParsedOpen {
    uint8_t container_id = 0;
    uint8_t container_type = 0;
};

struct ParsedClose {
    uint8_t container_id = 0;
    uint8_t container_type = 0;
    bool server_initiated = false;
};

struct Mailbox {
    bool active = false;
    uint64_t token = 0;
    bool open_seen = false;
    bool content_seen = false;
    bool close_sent = false;
    bool close_seen = false;
    uint8_t container_id = 0;
    uint8_t container_type = 0;
    uint16_t error_length = 0;
    std::array<char, kMaximumErrorBytes + 1U> error{};
};

struct Quarantine {
    bool active = false;
    bool open_seen = false;
    bool content_seen = false;
    bool close_sent = false;
    bool close_seen = false;
    uint8_t container_id = 0;
    uint8_t container_type = 0;
    std::chrono::steady_clock::time_point expires_at{};
};

std::mutex g_mutex;
Mailbox g_mailbox;
Quarantine g_quarantine;

bool readWireHeader(Reader* reader, uint32_t expected_packet_id) {
    uint32_t header = 0;
    return reader && reader->varUInt(&header) &&
        (header & 0x3FFU) == expected_packet_id;
}

bool readPacketId(std::string_view packet, uint32_t* packet_id) {
    Reader reader(packet);
    uint32_t header = 0;
    if (!packet_id || !reader.varUInt(&header)) return false;
    *packet_id = header & 0x3FFU;
    return true;
}

bool parsePlayerInventoryOpen(std::string_view packet, ParsedOpen* output) {
    if (!output) return false;
    Reader reader(packet);
    int32_t ignored_x = 0;
    uint32_t ignored_y = 0;
    int32_t ignored_z = 0;
    int64_t entity_unique_id = 0;
    if (!readWireHeader(&reader, kContainerOpenPacketId) ||
        !reader.byte(&output->container_id) ||
        !reader.byte(&output->container_type) ||
        output->container_type != kProjectionPrinterPlayerInventoryContainerType ||
        !reader.varInt(&ignored_x) || !reader.varUInt(&ignored_y) ||
        !reader.varInt(&ignored_z) || !reader.varInt64(&entity_unique_id) ||
        // The local-player inventory window has no backing entity. Requiring
        // the protocol's -1 sentinel prevents an armed printer from claiming
        // an unrelated entity-backed type=255 window.
        entity_unique_id != -1) {
        return false;
    }
    return true;
}

bool parseClose(std::string_view packet, ParsedClose* output) {
    if (!output) return false;
    Reader reader(packet);
    uint8_t server_initiated = 0;
    if (!readWireHeader(&reader, kContainerClosePacketId) ||
        !reader.byte(&output->container_id) || !reader.byte(&output->container_type) ||
        !reader.byte(&server_initiated) || server_initiated > 1U) {
        return false;
    }
    output->server_initiated = server_initiated != 0U;
    return true;
}

bool matchesPlayerInventoryContentIdentity(std::string_view packet) {
    Reader reader(packet);
    uint32_t inventory_id = 0;
    uint32_t slot_count = 0;
    return readWireHeader(&reader, kInventoryContentPacketId) &&
        reader.varUInt(&inventory_id) && reader.varUInt(&slot_count) &&
        inventory_id == kProjectionPrinterPlayerInventoryNetworkId &&
        slot_count == kProjectionPrinterPlayerInventorySessionSlotCount;
}

bool skipItem(Reader* reader) {
    if (!reader) return false;
    int32_t runtime_item_id = 0;
    if (!reader->varInt(&runtime_item_id)) return false;
    if (runtime_item_id == 0) return true;

    uint16_t ignored_count = 0;
    uint32_t aux = 0;
    uint8_t has_network_stack_id = 0;
    int32_t ignored_network_stack_id = 0;
    int32_t ignored_block_runtime_id = 0;
    uint32_t user_data_bytes = 0;
    return reader->littleEndian16(&ignored_count) && ignored_count != 0U &&
        reader->varUInt(&aux) && aux <= std::numeric_limits<uint16_t>::max() &&
        reader->byte(&has_network_stack_id) && has_network_stack_id <= 1U &&
        (has_network_stack_id == 0U || reader->varInt(&ignored_network_stack_id)) &&
        reader->varInt(&ignored_block_runtime_id) && reader->varUInt(&user_data_bytes) &&
        user_data_bytes <= kMaximumItemUserDataBytes && reader->skip(user_data_bytes);
}

bool parseFullContainerName(Reader* reader) {
    if (!reader) return false;
    uint8_t ignored_container_id = 0;
    uint8_t has_dynamic_id = 0;
    uint32_t ignored_dynamic_id = 0;
    return reader->byte(&ignored_container_id) && reader->byte(&has_dynamic_id) &&
        has_dynamic_id <= 1U &&
        (has_dynamic_id == 0U || reader->littleEndian32(&ignored_dynamic_id));
}

bool parsePlayerInventoryContent(std::string_view packet) {
    if (packet.size() > kMaximumInventoryPacketBytes) return false;
    Reader reader(packet);
    uint32_t inventory_id = 0;
    uint32_t slot_count = 0;
    if (!readWireHeader(&reader, kInventoryContentPacketId) ||
        !reader.varUInt(&inventory_id) || !reader.varUInt(&slot_count) ||
        inventory_id != kProjectionPrinterPlayerInventoryNetworkId ||
        slot_count != kProjectionPrinterPlayerInventorySessionSlotCount) {
        return false;
    }
    for (uint32_t index = 0; index < slot_count; ++index) {
        if (!skipItem(&reader)) return false;
    }
    return parseFullContainerName(&reader) && skipItem(&reader) && reader.atEnd();
}

bool needsClientClose(bool opened, bool closed, uint8_t container_id) {
    // Keep the local-player inventory (container id 0) aligned with the
    // supplied backpack-manager flow: OpenInventory creates the server-side
    // inventory context, but its silent session is retired locally and does
    // not emit ContainerClose(0).  The reference implementation only sends a
    // close for a real non-zero window id.  Closing id 0 made the stock client
    // tear down the hotbar/UI state that the immediately queued slot updates
    // are supposed to refresh.
    return opened && !closed && container_id != 0U;
}

void setMailboxErrorLocked(const char* message) {
    g_mailbox.error.fill('\0');
    g_mailbox.error_length = 0;
    if (!message) return;
    const size_t length = std::min(std::strlen(message), kMaximumErrorBytes);
    std::memcpy(g_mailbox.error.data(), message, length);
    g_mailbox.error_length = static_cast<uint16_t>(length);
}

void discardExpiredQuarantineLocked() {
    if (g_quarantine.active && std::chrono::steady_clock::now() >=
                                   g_quarantine.expires_at) {
        g_quarantine = {};
    }
}

void quarantineMailboxLocked() {
    if (!g_mailbox.active) return;
    g_quarantine = {};
    g_quarantine.active = true;
    g_quarantine.open_seen = g_mailbox.open_seen;
    g_quarantine.content_seen = g_mailbox.content_seen;
    g_quarantine.close_sent = g_mailbox.close_sent;
    g_quarantine.close_seen = g_mailbox.close_seen;
    g_quarantine.container_id = g_mailbox.container_id;
    g_quarantine.container_type = g_mailbox.container_type;
    g_quarantine.expires_at = std::chrono::steady_clock::now() +
        (g_mailbox.content_seen ? kCompletedQuarantineDuration
                                : kIncompleteQuarantineDuration);
}

void populateResultLocked(ProjectionPrinterInventorySessionResult* output) {
    if (!output) return;
    output->token = g_mailbox.token;
    output->container_id = g_mailbox.container_id;
    output->container_type = g_mailbox.container_type;
    output->container_opened = g_mailbox.open_seen;
    output->inventory_content_received = g_mailbox.content_seen;
    output->client_close_sent = g_mailbox.close_sent;
    output->container_closed = g_mailbox.close_seen;
    output->requires_client_close = needsClientClose(
        g_mailbox.open_seen, g_mailbox.close_seen, g_mailbox.container_id);
    output->error.assign(g_mailbox.error.data(), g_mailbox.error_length);
}

ProjectionPrinterInventorySessionReceiveDisposition observeOpenLocked(const ParsedOpen& parsed) {
    discardExpiredQuarantineLocked();
    if (g_mailbox.active) {
        if (!g_mailbox.open_seen && !g_mailbox.close_seen) {
            g_mailbox.open_seen = true;
            g_mailbox.content_seen = false;
            g_mailbox.close_sent = false;
            g_mailbox.close_seen = false;
            g_mailbox.container_id = parsed.container_id;
            g_mailbox.container_type = parsed.container_type;
            setMailboxErrorLocked(nullptr);
            return ProjectionPrinterInventorySessionReceiveDisposition::ActiveOpen;
        }
        const bool matches_active_open = g_mailbox.open_seen &&
            g_mailbox.container_id == parsed.container_id &&
            g_mailbox.container_type == parsed.container_type;
        // A duplicate active open has already received its one presentation
        // decision. Passing it preserves stock container ownership; when the
        // first handoff failed, keep every repeat hidden until runtime closes
        // the failed session.
        if (!matches_active_open) {
            return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
        }
        return g_mailbox.error_length == 0U
            ? ProjectionPrinterInventorySessionReceiveDisposition::Pass
            : ProjectionPrinterInventorySessionReceiveDisposition::QuarantinedOpen;
    }
    if (!g_quarantine.active) return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
    if (!g_quarantine.open_seen) {
        g_quarantine.open_seen = true;
        g_quarantine.content_seen = false;
        g_quarantine.close_sent = false;
        g_quarantine.close_seen = false;
        g_quarantine.container_id = parsed.container_id;
        g_quarantine.container_type = parsed.container_type;
        return ProjectionPrinterInventorySessionReceiveDisposition::QuarantinedOpen;
    }
    return g_quarantine.container_id == parsed.container_id &&
            g_quarantine.container_type == parsed.container_type
        ? ProjectionPrinterInventorySessionReceiveDisposition::QuarantinedOpen
        : ProjectionPrinterInventorySessionReceiveDisposition::Pass;
}

bool activeOwnsContentLocked() {
    return g_mailbox.active && g_mailbox.open_seen && !g_mailbox.close_seen;
}

bool quarantineOwnsContentLocked() {
    return g_quarantine.active && g_quarantine.open_seen && !g_quarantine.close_seen;
}

}  // namespace

bool ArmProjectionPrinterInventorySession(uint64_t token) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        discardExpiredQuarantineLocked();
        if (token == 0U || g_mailbox.active || g_quarantine.active) return false;
        g_mailbox = {};
        g_mailbox.active = true;
        g_mailbox.token = token;
        return true;
    } catch (...) {
        return false;
    }
}

void CancelProjectionPrinterInventorySession(uint64_t token) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        discardExpiredQuarantineLocked();
        if (token != 0U && g_mailbox.active && g_mailbox.token == token) {
            quarantineMailboxLocked();
            g_mailbox = {};
        }
    } catch (...) {
    }
}

void ClearProjectionPrinterInventorySession() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_mailbox = {};
        g_quarantine = {};
    } catch (...) {
    }
}

ProjectionPrinterInventorySessionPollState PollProjectionPrinterInventorySession(
    uint64_t token, ProjectionPrinterInventorySessionResult* output) {
    std::lock_guard<std::mutex> lock(g_mutex);
    discardExpiredQuarantineLocked();
    if (!g_mailbox.active || token == 0U || token != g_mailbox.token) {
        if (output) *output = {};
        return ProjectionPrinterInventorySessionPollState::Inactive;
    }
    if (output) {
        *output = {};
        populateResultLocked(output);
    }
    if (g_mailbox.error_length != 0U) {
        return ProjectionPrinterInventorySessionPollState::Failed;
    }
    if (!g_mailbox.open_seen) {
        return ProjectionPrinterInventorySessionPollState::WaitingForOpen;
    }
    if (g_mailbox.close_seen) {
        return ProjectionPrinterInventorySessionPollState::Closed;
    }
    if (!g_mailbox.content_seen) {
        return ProjectionPrinterInventorySessionPollState::WaitingForContent;
    }
    if (g_mailbox.close_sent) {
        return ProjectionPrinterInventorySessionPollState::WaitingForClose;
    }
    return ProjectionPrinterInventorySessionPollState::Ready;
}

bool GetProjectionPrinterInventorySessionCloseRequest(
    uint64_t token, ProjectionPrinterInventorySessionCloseRequest* output) noexcept {
    try {
        if (!output) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        discardExpiredQuarantineLocked();
        *output = {};
        if (!g_mailbox.active || token == 0U || g_mailbox.token != token ||
            !g_mailbox.open_seen || g_mailbox.close_seen) {
            return false;
        }
        output->token = token;
        output->container_id = g_mailbox.container_id;
        output->container_type = g_mailbox.container_type;
        output->requires_client_close = needsClientClose(
            g_mailbox.open_seen, g_mailbox.close_seen, g_mailbox.container_id);
        return true;
    } catch (...) {
        if (output) *output = {};
        return false;
    }
}

void MarkProjectionPrinterInventorySessionCloseSent(uint64_t token,
                                                     uint8_t container_id,
                                                     uint8_t container_type) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        discardExpiredQuarantineLocked();
        if (g_mailbox.active && token != 0U && g_mailbox.token == token &&
            g_mailbox.open_seen && !g_mailbox.close_seen &&
            g_mailbox.container_id == container_id &&
            g_mailbox.container_type == container_type) {
            g_mailbox.close_sent = true;
        }
    } catch (...) {
    }
}

void FailProjectionPrinterInventorySessionPresentationGate() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        discardExpiredQuarantineLocked();
        if (g_mailbox.active && g_mailbox.open_seen && !g_mailbox.close_seen) {
            setMailboxErrorLocked(
                "silent inventory presentation gate was unavailable for ContainerOpen");
        }
    } catch (...) {
    }
}

bool PollProjectionPrinterInventorySessionQuarantine(
    ProjectionPrinterInventorySessionQuarantine* output) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        discardExpiredQuarantineLocked();
        if (output) *output = {};
        if (!g_quarantine.active) return false;
        if (output) {
            output->container_id = g_quarantine.container_id;
            output->container_type = g_quarantine.container_type;
            output->container_opened = g_quarantine.open_seen;
            output->inventory_content_received = g_quarantine.content_seen;
            output->client_close_sent = g_quarantine.close_sent;
            output->container_closed = g_quarantine.close_seen;
            output->requires_client_close = needsClientClose(
                g_quarantine.open_seen, g_quarantine.close_seen,
                g_quarantine.container_id);
        }
        return true;
    } catch (...) {
        if (output) *output = {};
        return false;
    }
}

void MarkProjectionPrinterInventorySessionQuarantineCloseSent(
    uint8_t container_id, uint8_t container_type) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        discardExpiredQuarantineLocked();
        if (g_quarantine.active && g_quarantine.open_seen &&
            !g_quarantine.close_seen &&
            g_quarantine.container_id == container_id &&
            g_quarantine.container_type == container_type) {
            g_quarantine.close_sent = true;
        }
    } catch (...) {
    }
}

ProjectionPrinterInventorySessionReceiveDisposition
ObserveProjectionPrinterInventorySessionPacket(std::string_view packet) noexcept {
    try {
        if (packet.empty()) return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
        uint32_t packet_id = 0;
        if (!readPacketId(packet, &packet_id)) {
            return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
        }
        // A player-inventory window cannot cross a login, world, or dimension
        // boundary. Clear this state before the same observer sees new-world
        // InventoryContent, otherwise a delayed old request could be mistaken
        // for a valid material exchange in the new session.
        if (packet_id == kDisconnectPacketId || packet_id == kStartGamePacketId ||
            packet_id == kChangeDimensionPacketId) {
            ClearProjectionPrinterInventorySession();
            return ProjectionPrinterInventorySessionReceiveDisposition::WorldReset;
        }

        if (packet_id == kContainerOpenPacketId) {
            ParsedOpen parsed;
            if (!parsePlayerInventoryOpen(packet, &parsed)) {
                return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            // The presentation gate may later route an active open through the
            // stock slow factory path so its ItemStackResponse/HUD mapping is
            // established while the final screen handoff remains hidden. This
            // mailbox only identifies the exact printer window; it does not
            // itself choose packet delivery.
            return observeOpenLocked(parsed);
        }

        if (packet_id == kInventoryContentPacketId) {
            if (!matchesPlayerInventoryContentIdentity(packet)) {
                return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
            }
            uint64_t token = 0;
            bool quarantine = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                discardExpiredQuarantineLocked();
                if (activeOwnsContentLocked()) {
                    token = g_mailbox.token;
                } else if (quarantineOwnsContentLocked()) {
                    quarantine = true;
                } else {
                    return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
                }
            }

            const bool parsed = parsePlayerInventoryContent(packet);
            std::lock_guard<std::mutex> lock(g_mutex);
            discardExpiredQuarantineLocked();
            if (!quarantine) {
                if (!g_mailbox.active || g_mailbox.token != token ||
                    !activeOwnsContentLocked()) {
                    return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
                }
                if (!parsed) {
                    setMailboxErrorLocked(
                        "invalid player InventoryContent after silent inventory open");
                } else {
                    g_mailbox.content_seen = true;
                }
                // Unlike ContainerOpen, this packet must be delivered to the
                // stock client.  The native ItemStackNetManagerClient uses the
                // player inventory content to establish its live network-stack
                // state; suppressing it makes a later ScopeBegin request stale.
                return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
            }
            if (quarantineOwnsContentLocked()) {
                g_quarantine.content_seen = parsed;
            }
            return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
        }

        if (packet_id != kContainerClosePacketId) {
            return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
        }
        ParsedClose parsed;
        if (!parseClose(packet, &parsed)) {
            return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        discardExpiredQuarantineLocked();
        if (g_mailbox.active && g_mailbox.open_seen && !g_mailbox.close_seen &&
            g_mailbox.container_id == parsed.container_id &&
            g_mailbox.container_type == parsed.container_type) {
            g_mailbox.close_seen = true;
            if (!g_mailbox.content_seen) {
                setMailboxErrorLocked(
                    "player inventory closed before its content was captured");
            }
            // The raw hook owns only the matching ContainerOpen; this close
            // must still reach stock code to finalize any container state.
            return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
        }
        if (g_quarantine.active && g_quarantine.open_seen &&
            !g_quarantine.close_seen &&
            g_quarantine.container_id == parsed.container_id &&
            g_quarantine.container_type == parsed.container_type) {
            g_quarantine.close_seen = true;
            return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
        }
        return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
    } catch (...) {
        try {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_mailbox.active) {
                setMailboxErrorLocked("silent player inventory session packet handling failed");
            }
        } catch (...) {
        }
        return ProjectionPrinterInventorySessionReceiveDisposition::Pass;
    }
}

}  // namespace build_import
