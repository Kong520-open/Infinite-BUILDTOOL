#include "ContainerCaptureMailbox.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace build_import {
namespace {

constexpr uint32_t kContainerOpenPacketId = 0x2EU;
constexpr uint32_t kContainerClosePacketId = 0x2FU;
constexpr uint32_t kInventoryContentPacketId = 0x31U;
constexpr uint32_t kInventorySlotPacketId = 0x32U;
constexpr uint32_t kDisconnectPacketId = 0x05U;
constexpr uint32_t kStartGamePacketId = 0x0BU;
constexpr uint32_t kChangeDimensionPacketId = 0x3DU;
// Bedrock's client-side codec permits up to 5 MiB of NBT on one item. We do
// not retain this payload, but still need to skip it to reach the next slot.
constexpr uint32_t kMaximumItemUserDataBytes = 5U * 1024U * 1024U;
constexpr size_t kMaximumInventoryContentPacketBytes = 10U * 1024U * 1024U;
constexpr size_t kMaximumCaptureErrorBytes = 255U;
constexpr auto kIncompleteCaptureQuarantineDuration = std::chrono::seconds(1);
constexpr auto kCompletedCaptureQuarantineDuration = std::chrono::milliseconds(500);
constexpr auto kVisibleAnvilCloseReceiptDuration = std::chrono::seconds(8);
constexpr uint8_t kChestWindowType = 0U;
constexpr uint8_t kAnvilWindowType = 5U;
constexpr uint8_t kChestRequestContainerName = 7U;
constexpr uint32_t kSingleChestSlotCount = 27U;
constexpr auto kManualTraceDuration = std::chrono::minutes(20);
constexpr uint32_t kManualTraceMaximumEvents = 128U;

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
        uint32_t value = 0;
        for (uint32_t index = 0; index < 4U; ++index) {
            value |= static_cast<uint32_t>(static_cast<uint8_t>(input_[cursor_ + index]))
                << (index * 8U);
        }
        cursor_ += 4U;
        *output = value;
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

    bool take(size_t count, std::string_view* output) {
        if (!output || count > input_.size() - cursor_) return false;
        *output = input_.substr(cursor_, count);
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
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

struct ParsedInventory {
    uint32_t inventory_id = 0;
    bool inventory_id_seen = false;
    uint32_t slot_count = 0;
    struct Item {
        uint16_t slot = 0;
        int32_t numeric_id = 0;
        uint16_t count = 0;
        uint16_t aux = 0;
        bool has_network_stack_id = false;
        int32_t network_stack_id = 0;
        bool has_map_uuid = false;
        int64_t map_uuid = -1;
        MapItemNameStatus name_status = MapItemNameStatus::Missing;
        MapItemNameSource name_source = MapItemNameSource::None;
        std::string name_candidate;
    };
    std::array<Item, kMaximumCapturedContainerSlots> items{};
    uint32_t item_count = 0;
    bool has_full_container_name = false;
    uint8_t full_container_name = 0;
    bool has_dynamic_container_id = false;
    uint32_t dynamic_container_id = 0;
};

struct ParsedInventorySlot {
    uint32_t inventory_id = 0;
    uint32_t slot = 0;
    ParsedInventory::Item item;
};

struct MailboxItem {
    uint16_t slot = 0;
    int32_t numeric_id = 0;
    uint16_t count = 0;
    uint16_t aux = 0;
    bool has_network_stack_id = false;
    int32_t network_stack_id = 0;
    bool has_map_uuid = false;
    int64_t map_uuid = -1;
    MapItemNameStatus name_status = MapItemNameStatus::Missing;
    MapItemNameSource name_source = MapItemNameSource::None;
    std::string name_candidate;
};

struct Mailbox {
    bool active = false;
    bool visible_anvil = false;
    bool visible_chest = false;
    bool visible_chest_interrupted = false;
    bool exclusive_hidden_chest = false;
    uint64_t token = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    bool open_seen = false;
    bool close_seen = false;
    uint8_t container_id = 0;
    uint8_t container_type = 0;
    bool content_seen = false;
    uint32_t slot_count = 0;
    bool has_full_container_name = false;
    uint8_t full_container_name = 0;
    bool has_dynamic_container_id = false;
    uint32_t dynamic_container_id = 0;
    uint32_t item_count = 0;
    std::array<MailboxItem, kMaximumCapturedContainerSlots> items{};
    uint16_t error_length = 0;
    std::array<char, kMaximumCaptureErrorBytes + 1U> error{};
};

struct QuarantinedMailbox {
    bool active = false;
    uint64_t token = 0;
    bool from_visible_anvil = false;
    bool from_visible_chest = false;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    bool open_seen = false;
    bool close_seen = false;
    bool content_seen = false;
    bool close_sent = false;
    uint8_t container_id = 0;
    uint8_t container_type = 0;
    std::chrono::steady_clock::time_point expires_at{};
};

std::mutex g_mutex;
Mailbox g_mailbox;
QuarantinedMailbox g_quarantine;

struct VisibleAnvilCloseReceipt {
    uint64_t token = 0;
    uint8_t window_id = 0;
    uint8_t window_type = 0;
    bool received = false;
    std::chrono::steady_clock::time_point expires_at{};
};

VisibleAnvilCloseReceipt g_visible_anvil_close_receipt;

struct ManualChestTraceState {
    bool enabled = false;
    bool open = false;
    bool confirmed_single_chest = false;
    uint64_t generation = 0;
    uint8_t container_id = 0;
    uint8_t container_type = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint32_t logged_events = 0;
    std::chrono::steady_clock::time_point started_at{};
};

ManualChestTraceState g_manual_trace;

void discardExpiredQuarantineLocked() {
    if (!g_quarantine.active) return;
    const auto now = std::chrono::steady_clock::now();
    if (now >= g_quarantine.expires_at) g_quarantine = {};
}

void discardExpiredVisibleAnvilCloseReceiptLocked() {
    if (g_visible_anvil_close_receipt.token != 0U &&
        std::chrono::steady_clock::now() >=
            g_visible_anvil_close_receipt.expires_at) {
        g_visible_anvil_close_receipt = {};
    }
}

void quarantineMailboxLocked() {
    if (!g_mailbox.active) return;
    // Once the stock client has seen a visible Open, its Slot and Close must
    // also reach the stock client. The owner closes that window before
    // cancelling; never turn it into a hidden quarantine at this point.
    if ((g_mailbox.visible_anvil || g_mailbox.visible_chest) &&
        g_mailbox.open_seen) return;
    g_quarantine = {};
    g_quarantine.active = true;
    g_quarantine.token = g_mailbox.token;
    g_quarantine.from_visible_anvil = g_mailbox.visible_anvil;
    g_quarantine.from_visible_chest = g_mailbox.visible_chest;
    g_quarantine.x = g_mailbox.x;
    g_quarantine.y = g_mailbox.y;
    g_quarantine.z = g_mailbox.z;
    g_quarantine.open_seen = g_mailbox.open_seen;
    g_quarantine.close_seen = g_mailbox.close_seen;
    g_quarantine.content_seen = g_mailbox.content_seen;
    g_quarantine.container_id = g_mailbox.container_id;
    g_quarantine.container_type = g_mailbox.container_type;
    // A completed request has already sent its client close, but we must
    // still suppress a late server close because the game's UI never saw the
    // corresponding ContainerOpen.  Incomplete requests retain the longer
    // window so a delayed open can be closed safely on the game thread.
    g_quarantine.expires_at = std::chrono::steady_clock::now() +
        (g_mailbox.content_seen ? kCompletedCaptureQuarantineDuration
                                : kIncompleteCaptureQuarantineDuration);
}

bool readWireHeader(Reader* reader, uint32_t expected_packet_id) {
    uint32_t header = 0;
    return reader && reader->varUInt(&header) && (header & 0x3FFU) == expected_packet_id;
}

bool parseOpen(std::string_view packet, ParsedOpen* output) {
    if (!output) return false;
    Reader reader(packet);
    uint32_t encoded_y = 0;
    int64_t ignored_entity_unique_id = 0;
    return readWireHeader(&reader, kContainerOpenPacketId) &&
        reader.byte(&output->container_id) && reader.byte(&output->container_type) &&
        reader.varInt(&output->x) && reader.varUInt(&encoded_y) &&
        (output->y = static_cast<int32_t>(encoded_y), true) && reader.varInt(&output->z) &&
        reader.varInt64(&ignored_entity_unique_id);
}

struct ParsedClose {
    uint8_t container_id = 0;
    uint8_t container_type = 0;
    bool server_initiated = false;
};

bool parseClose(std::string_view packet, ParsedClose* output) {
    if (!output) return false;
    Reader reader(packet);
    uint8_t server_initiated = 0;
    if (!readWireHeader(&reader, kContainerClosePacketId) ||
        !reader.byte(&output->container_id) ||
        !reader.byte(&output->container_type) ||
        !reader.byte(&server_initiated) || server_initiated > 1U) {
        return false;
    }
    output->server_initiated = server_initiated != 0U;
    return true;
}

void setParseError(const char** error, const char* message) {
    if (error) *error = message;
}

void setMailboxErrorLocked(const char* message) {
    g_mailbox.error.fill('\0');
    g_mailbox.error_length = 0;
    if (!message) return;
    const size_t length = std::min(std::strlen(message), kMaximumCaptureErrorBytes);
    std::memcpy(g_mailbox.error.data(), message, length);
    g_mailbox.error_length = static_cast<uint16_t>(length);
}

bool parseInventoryItem(Reader* reader, ParsedInventory::Item* output) {
    if (!reader || !output) return false;
    *output = {};
    if (!reader->varInt(&output->numeric_id)) return false;
    if (output->numeric_id == 0) return true;

    uint32_t damage = 0;
    uint8_t has_network_id = 0;
    int32_t ignored_block_runtime_id = 0;
    uint32_t user_data_bytes = 0;
    std::string_view user_data;
    if (!reader->littleEndian16(&output->count) || output->count == 0U ||
        !reader->varUInt(&damage) || damage > std::numeric_limits<uint16_t>::max() ||
        !reader->byte(&has_network_id) || has_network_id > 1U ||
        (has_network_id != 0U && !reader->varInt(&output->network_stack_id)) ||
        !reader->varInt(&ignored_block_runtime_id) ||
        !reader->varUInt(&user_data_bytes) ||
        user_data_bytes > kMaximumItemUserDataBytes ||
        !reader->take(user_data_bytes, &user_data)) return false;
    output->aux = static_cast<uint16_t>(damage);
    output->has_network_stack_id = has_network_id != 0U;
    ItemExtraMapMetadata metadata;
    if (ParseItemExtraMapMetadata(user_data, &metadata)) {
        output->has_map_uuid = metadata.has_map_uuid;
        output->map_uuid = metadata.map_uuid;
        if (metadata.has_map_uuid) {
            output->name_status = metadata.name_status;
            output->name_source = metadata.name_source;
            output->name_candidate = std::move(metadata.display_name);
        }
    }
    return true;
}

bool parseSlotContainerName(Reader* reader) {
    uint8_t ignored_name = 0;
    uint8_t has_dynamic_id = 0;
    uint32_t ignored_dynamic_id = 0;
    return reader && reader->byte(&ignored_name) &&
        reader->byte(&has_dynamic_id) && has_dynamic_id <= 1U &&
        (has_dynamic_id == 0U || reader->littleEndian32(&ignored_dynamic_id));
}

bool parseSlotOptionalContext(Reader* reader) {
    uint8_t has_name = 0;
    uint8_t has_storage_item = 0;
    ParsedInventory::Item ignored_storage_item;
    return reader && reader->byte(&has_name) && has_name <= 1U &&
        (has_name == 0U || parseSlotContainerName(reader)) &&
        reader->byte(&has_storage_item) && has_storage_item <= 1U &&
        (has_storage_item == 0U ||
         parseInventoryItem(reader, &ignored_storage_item));
}

bool parseInventorySlotVariant(std::string_view packet, bool optional_context,
                               ParsedInventorySlot* output) {
    if (!output || packet.size() > kMaximumInventoryContentPacketBytes) return false;
    Reader reader(packet);
    ParsedInventorySlot parsed;
    if (!readWireHeader(&reader, kInventorySlotPacketId) ||
        !reader.varUInt(&parsed.inventory_id) || !reader.varUInt(&parsed.slot) ||
        parsed.slot >= kSingleChestSlotCount) return false;
    ParsedInventory::Item ignored_storage_item;
    if (optional_context ? !parseSlotOptionalContext(&reader)
                         : (!parseSlotContainerName(&reader) ||
                            !parseInventoryItem(&reader, &ignored_storage_item))) {
        return false;
    }
    if (!parseInventoryItem(&reader, &parsed.item) || !reader.atEnd()) return false;
    parsed.item.slot = static_cast<uint16_t>(parsed.slot);
    *output = parsed;
    return true;
}

bool parseInventorySlot(std::string_view packet, ParsedInventorySlot* output) {
    return parseInventorySlotVariant(packet, true, output) ||
        parseInventorySlotVariant(packet, false, output);
}

// The post-v748 InventoryContent tail contains a FullContainerName followed
// by a storage item. It is optional evidence for transfer preparation, not a
// new requirement for the existing read-only export capture.
void parseInventoryTail(Reader* reader, ParsedInventory* output) {
    if (!reader || !output) return;
    uint8_t name = 0;
    uint8_t has_dynamic_id = 0;
    uint32_t dynamic_id = 0;
    ParsedInventory::Item storage_item;
    if (!reader->byte(&name) || !reader->byte(&has_dynamic_id) ||
        has_dynamic_id > 1U ||
        (has_dynamic_id != 0U && !reader->littleEndian32(&dynamic_id)) ||
        !parseInventoryItem(reader, &storage_item) || !reader->atEnd()) return;
    output->has_full_container_name = true;
    output->full_container_name = name;
    output->has_dynamic_container_id = has_dynamic_id != 0U;
    output->dynamic_container_id = dynamic_id;
}

bool parseInventory(std::string_view packet, ParsedInventory* output,
                    const char** error) {
    if (!output) return false;
    *output = {};
    Reader reader(packet);
    uint32_t slot_count = 0;
    if (!readWireHeader(&reader, kInventoryContentPacketId) ||
        !reader.varUInt(&output->inventory_id)) {
        setParseError(error, "invalid InventoryContent header or inventory ID");
        return false;
    }
    output->inventory_id_seen = true;
    if (packet.size() > kMaximumInventoryContentPacketBytes) {
        setParseError(error, "InventoryContent packet exceeds the safety limit");
        return false;
    }
    if (!reader.varUInt(&slot_count) || slot_count > kMaximumCapturedContainerSlots) {
        setParseError(error, "invalid InventoryContent header or slot count");
        return false;
    }

    output->slot_count = slot_count;
    for (uint32_t slot = 0; slot < slot_count; ++slot) {
        ParsedInventory::Item item;
        if (!parseInventoryItem(&reader, &item)) {
            setParseError(error, "invalid or truncated InventoryContent item payload");
            return false;
        }
        if (item.numeric_id == 0) continue;

        if (output->item_count >= output->items.size()) {
            setParseError(error, "InventoryContent has too many non-empty items");
            return false;
        }
        item.slot = static_cast<uint16_t>(slot);
        output->items[output->item_count++] = item;
    }
    parseInventoryTail(&reader, output);
    return true;
}

bool manualTraceAutomaticWindowLocked(uint8_t container_id) {
    discardExpiredQuarantineLocked();
    return (g_mailbox.active && g_mailbox.open_seen &&
            !g_mailbox.close_seen && g_mailbox.container_id == container_id) ||
        (g_quarantine.active && g_quarantine.open_seen &&
         !g_quarantine.close_seen && g_quarantine.container_id == container_id);
}

bool manualTraceCanLogLocked() {
    if (!g_manual_trace.enabled ||
        std::chrono::steady_clock::now() - g_manual_trace.started_at >=
            kManualTraceDuration ||
        g_manual_trace.logged_events >= kManualTraceMaximumEvents) {
        g_manual_trace.open = false;
        return false;
    }
    ++g_manual_trace.logged_events;
    return true;
}

void logManualChestTraceEvent(const ManualChestTraceEvent& event) {
#if defined(__ANDROID__)
    constexpr const char* kTag = "Infinitecz_MapManualTrace";
    const auto window = static_cast<unsigned int>(event.container_id);
    switch (event.kind) {
        case ManualChestTraceEventKind::Open:
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "chest_open window=%u type=%u pos=%d,%d,%d",
                window, static_cast<unsigned int>(event.container_type),
                event.x, event.y, event.z);
            break;
        case ManualChestTraceEventKind::Content:
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "chest_content window=%u parsed=%d slots=%u items=%u "
                "full_name_present=%d full_name=%u dynamic=%d dynamic_id=%u "
                "map_uuid_count=%u sample_count=%u sample=%u:%lld,%u:%lld,%u:%lld,%u:%lld "
                "first_map_name_status=%u source=%u bytes=%u",
                window, event.parsed ? 1 : 0,
                static_cast<unsigned int>(event.slot_count),
                static_cast<unsigned int>(event.item_count),
                event.has_full_container_name ? 1 : 0,
                static_cast<unsigned int>(event.full_container_name),
                event.has_dynamic_container_id ? 1 : 0,
                static_cast<unsigned int>(event.dynamic_container_id),
                static_cast<unsigned int>(event.map_uuid_count),
                static_cast<unsigned int>(event.map_sample_count),
                static_cast<unsigned int>(event.map_samples[0].slot),
                static_cast<long long>(event.map_samples[0].map_uuid),
                static_cast<unsigned int>(event.map_samples[1].slot),
                static_cast<long long>(event.map_samples[1].map_uuid),
                static_cast<unsigned int>(event.map_samples[2].slot),
                static_cast<long long>(event.map_samples[2].map_uuid),
                static_cast<unsigned int>(event.map_samples[3].slot),
                static_cast<long long>(event.map_samples[3].map_uuid),
                static_cast<unsigned int>(event.map_samples[0].name_status),
                static_cast<unsigned int>(event.map_samples[0].name_source),
                static_cast<unsigned int>(event.map_samples[0].name_bytes));
            break;
        case ManualChestTraceEventKind::Slot:
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "chest_slot window=%u slot=%u occupied=%d net_id_present=%d "
                "net_id=%d map_uuid_present=%d map_uuid=%lld "
                "name_status=%u name_source=%u name_bytes=%u",
                window, static_cast<unsigned int>(event.changed_slot),
                event.slot_occupied ? 1 : 0,
                event.slot_has_network_stack_id ? 1 : 0,
                event.slot_network_stack_id,
                event.slot_has_map_uuid ? 1 : 0,
                static_cast<long long>(event.slot_map_uuid),
                static_cast<unsigned int>(event.slot_name_status),
                static_cast<unsigned int>(event.slot_name_source),
                static_cast<unsigned int>(event.slot_name_bytes));
            break;
        case ManualChestTraceEventKind::Close:
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "chest_close window=%u type=%u server_initiated=%d",
                window, static_cast<unsigned int>(event.container_type),
                event.close_server_initiated ? 1 : 0);
            break;
        case ManualChestTraceEventKind::None:
            break;
    }
#else
    (void)event;
#endif
}

void populateResultLocked(ContainerCaptureResult* output) {
    if (!output) return;
    output->token = g_mailbox.token;
    output->x = g_mailbox.x;
    output->y = g_mailbox.y;
    output->z = g_mailbox.z;
    output->container_id = g_mailbox.container_id;
    output->container_type = g_mailbox.container_type;
    output->container_opened = g_mailbox.open_seen;
    output->container_closed = g_mailbox.close_seen;
    output->slot_count = g_mailbox.slot_count;
    output->has_full_container_name = g_mailbox.has_full_container_name;
    output->full_container_name = g_mailbox.full_container_name;
    output->has_dynamic_container_id = g_mailbox.has_dynamic_container_id;
    output->dynamic_container_id = g_mailbox.dynamic_container_id;
    output->items.clear();
    output->items.reserve(g_mailbox.item_count);
    for (uint32_t index = 0; index < g_mailbox.item_count; ++index) {
        const MailboxItem& source = g_mailbox.items[index];
        CapturedContainerItem item;
        item.slot = source.slot;
        item.numeric_id = source.numeric_id;
        item.count = source.count;
        item.aux = source.aux;
        item.has_network_stack_id = source.has_network_stack_id;
        item.network_stack_id = source.network_stack_id;
        item.has_map_uuid = source.has_map_uuid;
        item.map_uuid = source.map_uuid;
        item.name_status = source.name_status;
        item.name_source = source.name_source;
        item.name_candidate = source.name_candidate;
        output->items.push_back(std::move(item));
    }
    output->error.assign(g_mailbox.error.data(), g_mailbox.error_length);
}

}  // namespace

void ArmContainerCapture(uint64_t token, int32_t x, int32_t y, int32_t z) {
    std::lock_guard<std::mutex> lock(g_mutex);
    discardExpiredQuarantineLocked();
    // Legacy hidden captures are void-returning and may replace their own
    // retries. They must not silently displace a visible anvil whose stock
    // client still owns the opened window and incoming slot/close traffic.
    if (g_mailbox.active &&
        (g_mailbox.visible_anvil || g_mailbox.visible_chest ||
         g_mailbox.exclusive_hidden_chest)) return;
    g_visible_anvil_close_receipt = {};
    g_mailbox = {};
    g_mailbox.active = token != 0U;
    g_mailbox.token = token;
    g_mailbox.x = x;
    g_mailbox.y = y;
    g_mailbox.z = z;
}

bool TryArmHiddenChestCapture(uint64_t token, int32_t x, int32_t y,
                               int32_t z) noexcept {
    try {
        if (token == 0U) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        discardExpiredQuarantineLocked();
        if (g_mailbox.active || g_quarantine.active) return false;
        g_visible_anvil_close_receipt = {};
        g_mailbox = {};
        g_mailbox.active = true;
        g_mailbox.exclusive_hidden_chest = true;
        g_mailbox.token = token;
        g_mailbox.x = x;
        g_mailbox.y = y;
        g_mailbox.z = z;
        return true;
    } catch (...) {
        return false;
    }
}

bool TryArmVisibleChestCapture(uint64_t token, int32_t x, int32_t y,
                               int32_t z) noexcept {
    try {
        if (token == 0U) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        discardExpiredQuarantineLocked();
        if (g_mailbox.active || g_quarantine.active) return false;
        g_visible_anvil_close_receipt = {};
        g_mailbox = {};
        g_mailbox.active = true;
        g_mailbox.visible_chest = true;
        g_mailbox.token = token;
        g_mailbox.x = x;
        g_mailbox.y = y;
        g_mailbox.z = z;
        return true;
    } catch (...) {
        return false;
    }
}

bool IsExactOpenVisibleChestCapture(uint64_t token, uint8_t window_id,
                                    uint8_t window_type, int32_t x, int32_t y,
                                    int32_t z) noexcept {
    try {
        if (token == 0U || window_id == 0U || window_id == 0xFFU ||
            window_type != kChestWindowType) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_mailbox.active && g_mailbox.visible_chest &&
            !g_mailbox.visible_chest_interrupted &&
            g_mailbox.token == token && g_mailbox.x == x &&
            g_mailbox.y == y && g_mailbox.z == z &&
            g_mailbox.open_seen && !g_mailbox.close_seen &&
            g_mailbox.container_id == window_id &&
            g_mailbox.container_type == window_type &&
            g_mailbox.content_seen &&
            g_mailbox.slot_count == kSingleChestSlotCount &&
            g_mailbox.has_full_container_name &&
            g_mailbox.full_container_name == 0U &&
            !g_mailbox.has_dynamic_container_id &&
            g_mailbox.dynamic_container_id == 0U &&
            g_mailbox.error_length == 0U;
    } catch (...) {
        return false;
    }
}

bool ArmVisibleAnvilCapture(uint64_t token, int32_t x, int32_t y,
                            int32_t z) noexcept {
    try {
        if (token == 0U) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        discardExpiredQuarantineLocked();
        if (g_mailbox.active ||
            (g_quarantine.active && !g_quarantine.close_seen)) return false;
        g_visible_anvil_close_receipt = {};
        g_mailbox = {};
        g_mailbox.active = true;
        g_mailbox.visible_anvil = true;
        g_mailbox.token = token;
        g_mailbox.x = x;
        g_mailbox.y = y;
        g_mailbox.z = z;
        return true;
    } catch (...) {
        return false;
    }
}

void CancelContainerCapture(uint64_t token) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        discardExpiredQuarantineLocked();
        if (g_mailbox.active && g_mailbox.token == token) {
            if (g_mailbox.visible_anvil && g_mailbox.open_seen &&
                g_mailbox.container_type == kAnvilWindowType &&
                g_mailbox.container_id != 0U &&
                g_mailbox.container_id != 0xFFU) {
                g_visible_anvil_close_receipt = {
                    token, g_mailbox.container_id, kAnvilWindowType,
                    g_mailbox.close_seen,
                    std::chrono::steady_clock::now() +
                        kVisibleAnvilCloseReceiptDuration};
            }
            quarantineMailboxLocked();
            g_mailbox = {};
        }
    } catch (...) {
    }
}

bool HasVisibleAnvilServerCloseReceipt(uint64_t token,
                                       uint8_t window_id) noexcept {
    try {
        if (token == 0U || window_id == 0U || window_id == 0xFFU) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        discardExpiredVisibleAnvilCloseReceiptLocked();
        return g_visible_anvil_close_receipt.received &&
            g_visible_anvil_close_receipt.token == token &&
            g_visible_anvil_close_receipt.window_id == window_id &&
            g_visible_anvil_close_receipt.window_type == kAnvilWindowType;
    } catch (...) {
        return false;
    }
}

void ClearVisibleAnvilServerCloseReceipt() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_visible_anvil_close_receipt = {};
    } catch (...) {
    }
}

ContainerCapturePollState PollContainerCapture(uint64_t token,
                                               ContainerCaptureResult* output) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_mailbox.active || token == 0U || g_mailbox.token != token) {
        if (output) *output = {};
        return ContainerCapturePollState::Inactive;
    }
    if (output) {
        *output = {};
        output->token = g_mailbox.token;
        output->x = g_mailbox.x;
        output->y = g_mailbox.y;
        output->z = g_mailbox.z;
        output->container_id = g_mailbox.container_id;
        output->container_type = g_mailbox.container_type;
        output->container_opened = g_mailbox.open_seen;
        output->container_closed = g_mailbox.close_seen;
        output->slot_count = g_mailbox.slot_count;
        output->has_full_container_name = g_mailbox.has_full_container_name;
        output->full_container_name = g_mailbox.full_container_name;
        output->has_dynamic_container_id = g_mailbox.has_dynamic_container_id;
        output->dynamic_container_id = g_mailbox.dynamic_container_id;
        output->error.assign(g_mailbox.error.data(), g_mailbox.error_length);
    }
    if (g_mailbox.error_length != 0U) return ContainerCapturePollState::Failed;
    if (!g_mailbox.open_seen) return ContainerCapturePollState::WaitingForOpen;
    if (!g_mailbox.content_seen) return ContainerCapturePollState::WaitingForContent;
    populateResultLocked(output);
    return ContainerCapturePollState::Ready;
}

bool SelectEmptySingleChestSlot(const ContainerCaptureResult& capture,
                                uint64_t expected_token, int32_t expected_x,
                                int32_t expected_y, int32_t expected_z,
                                uint8_t* slot, std::string* error) {
    if (slot) *slot = 0;
    const auto refuse = [error](const char* reason) {
        if (error) *error = reason;
        return false;
    };
    if (!slot || expected_token == 0U || capture.token != expected_token ||
        capture.x != expected_x || capture.y != expected_y ||
        capture.z != expected_z || !capture.error.empty()) {
        return refuse("chest capture does not match the pending storage request");
    }
    if (!capture.container_opened || capture.container_closed ||
        capture.container_id == 0U || capture.container_id == 0xFFU ||
        capture.container_type != kChestWindowType ||
        capture.slot_count != kSingleChestSlotCount) {
        return refuse("storage target is not one open 27-slot chest window");
    }
    if (!capture.has_full_container_name ||
        capture.full_container_name != 0U ||
        capture.has_dynamic_container_id || capture.dynamic_container_id != 0U) {
        // This game's observed single-chest InventoryContent tail names the
        // content as 0, while a manual ItemStackRequest Place targets slot
        // type 7. They are different fields and must not be equated. A block
        // chest has no item-backed dynamic container ID; ContainerOpen binds
        // the actual window to the expected block position.
        return refuse("ordinary chest content identity is missing or dynamic");
    }
    std::array<bool, kSingleChestSlotCount> occupied{};
    for (const auto& item : capture.items) {
        if (item.slot >= occupied.size() || occupied[item.slot] ||
            item.numeric_id == 0 || item.count == 0U) {
            return refuse("chest slot snapshot contains an invalid occupied slot");
        }
        occupied[item.slot] = true;
    }
    for (uint8_t index = 0; index < occupied.size(); ++index) {
        if (!occupied[index]) {
            *slot = index;
            if (error) error->clear();
            return true;
        }
    }
    return refuse("the designated single chest has no free slot");
}

bool PollContainerCaptureQuarantine(ContainerCaptureQuarantine* output) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        discardExpiredQuarantineLocked();
        if (output) *output = {};
        if (!g_quarantine.active) return false;
        if (output) {
            output->token = g_quarantine.token;
            output->container_id = g_quarantine.container_id;
            output->container_type = g_quarantine.container_type;
            output->from_visible_anvil = g_quarantine.from_visible_anvil;
            output->container_opened = g_quarantine.open_seen;
            output->container_closed = g_quarantine.close_seen;
            output->content_captured = g_quarantine.content_seen;
            output->close_sent = g_quarantine.close_sent;
        }
        return true;
    } catch (...) {
        if (output) *output = {};
        return false;
    }
}

void MarkContainerCaptureQuarantineCloseSent(uint8_t container_id,
                                             uint8_t container_type) noexcept {
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

bool ObserveContainerCapturePacket(
        std::string_view packet,
        VisibleAnvilCaptureEvent* visible_anvil_event,
        VisibleChestCaptureEvent* visible_chest_event) noexcept {
    bool suppress_packet = false;
    try {
        if (visible_anvil_event) *visible_anvil_event = {};
        if (visible_chest_event) *visible_chest_event = {};
        if (packet.empty()) return false;
        Reader header_reader(packet);
        uint32_t wire_header = 0;
        if (!header_reader.varUInt(&wire_header)) return false;
        const uint32_t packet_id = wire_header & 0x3FFU;

        if (packet_id == kDisconnectPacketId ||
            packet_id == kStartGamePacketId ||
            packet_id == kChangeDimensionPacketId) {
            // The receive hook also passes world-reset packets through this
            // observer. Do not carry an old world's numeric window ID into
            // a new world, even before the task runtime notices the reset.
            ClearVisibleAnvilServerCloseReceipt();
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_mailbox.active && g_mailbox.visible_chest) {
                    g_mailbox.visible_chest_interrupted = true;
                    setMailboxErrorLocked("visible chest capture lost its world");
                }
            }
            return false;
        }

        if (packet_id == kContainerOpenPacketId) {
            ParsedOpen parsed;
            if (!parseOpen(packet, &parsed)) return false;
            std::lock_guard<std::mutex> lock(g_mutex);
            discardExpiredQuarantineLocked();
            if (g_mailbox.active && g_mailbox.visible_chest &&
                g_mailbox.open_seen && !g_mailbox.close_seen) {
                // A second window may have displaced the visible chest UI,
                // even if it reuses the same numeric ID or block position.
                g_mailbox.visible_chest_interrupted = true;
            }
            if (!g_mailbox.active || parsed.x != g_mailbox.x ||
                parsed.y != g_mailbox.y || parsed.z != g_mailbox.z ||
                g_mailbox.close_seen) {
                if (!g_quarantine.active || g_quarantine.close_seen ||
                    parsed.x != g_quarantine.x || parsed.y != g_quarantine.y ||
                    parsed.z != g_quarantine.z ||
                    (g_quarantine.from_visible_anvil &&
                     parsed.container_type != kAnvilWindowType) ||
                    (g_quarantine.from_visible_chest &&
                     parsed.container_type != kChestWindowType)) {
                    return false;
                }
                if (g_quarantine.open_seen) {
                    return parsed.container_id == g_quarantine.container_id &&
                        parsed.container_type == g_quarantine.container_type;
                }
                g_quarantine.open_seen = true;
                g_quarantine.container_id = parsed.container_id;
                g_quarantine.container_type = parsed.container_type;
#if defined(__ANDROID__)
                if (!g_quarantine.from_visible_anvil) {
                    __android_log_print(ANDROID_LOG_INFO,
                        "Infinitecz_MapChestWindow",
                        "open owner=quarantine id=%u type=%u",
                        static_cast<unsigned int>(parsed.container_id),
                        static_cast<unsigned int>(parsed.container_type));
                }
#endif
                return true;
            }
            if ((g_mailbox.visible_anvil &&
                 parsed.container_type != kAnvilWindowType) ||
                (g_mailbox.visible_chest &&
                 parsed.container_type != kChestWindowType)) {
                setMailboxErrorLocked(g_mailbox.visible_anvil
                    ? "opened container is not an anvil"
                    : "opened container is not a chest");
                return false;
            }
            if (g_mailbox.open_seen) {
                return !g_mailbox.visible_anvil && !g_mailbox.visible_chest &&
                    parsed.container_id == g_mailbox.container_id;
            }
            g_mailbox.open_seen = true;
            g_mailbox.close_seen = false;
            g_mailbox.container_id = parsed.container_id;
            g_mailbox.container_type = parsed.container_type;
            g_mailbox.content_seen = false;
            g_mailbox.slot_count = 0;
            g_mailbox.has_full_container_name = false;
            g_mailbox.full_container_name = 0;
            g_mailbox.has_dynamic_container_id = false;
            g_mailbox.dynamic_container_id = 0;
            g_mailbox.item_count = 0;
            setMailboxErrorLocked(nullptr);
#if defined(__ANDROID__)
            if (!g_mailbox.visible_anvil) {
                __android_log_print(ANDROID_LOG_INFO,
                    "Infinitecz_MapChestWindow",
                    "open owner=active id=%u type=%u",
                    static_cast<unsigned int>(parsed.container_id),
                    static_cast<unsigned int>(parsed.container_type));
            }
#endif
            if (g_mailbox.visible_anvil && visible_anvil_event) {
                visible_anvil_event->kind = VisibleAnvilCaptureEventKind::Open;
                visible_anvil_event->token = g_mailbox.token;
                visible_anvil_event->window_id = parsed.container_id;
            }
            if (g_mailbox.visible_chest &&
                !g_mailbox.visible_chest_interrupted &&
                parsed.container_id != 0U && parsed.container_id != 0xFFU &&
                visible_chest_event) {
                visible_chest_event->kind = VisibleChestCaptureEventKind::Open;
                visible_chest_event->token = g_mailbox.token;
                visible_chest_event->window_id = parsed.container_id;
            }
            return !g_mailbox.visible_anvil && !g_mailbox.visible_chest;
        }

        if (packet_id == kContainerClosePacketId) {
            ParsedClose parsed;
            if (!parseClose(packet, &parsed)) return false;
            std::lock_guard<std::mutex> lock(g_mutex);
            discardExpiredQuarantineLocked();
            if (g_mailbox.active && g_mailbox.open_seen && !g_mailbox.close_seen &&
                g_mailbox.container_id == parsed.container_id &&
                g_mailbox.container_type == parsed.container_type) {
                g_mailbox.close_seen = true;
                if (!g_mailbox.content_seen) {
                    setMailboxErrorLocked(
                        "container closed before its inventory was captured");
                }
#if defined(__ANDROID__)
                if (!g_mailbox.visible_anvil) {
                    __android_log_print(ANDROID_LOG_INFO,
                        "Infinitecz_MapChestWindow",
                        "close owner=active id=%u type=%u server=%d",
                        static_cast<unsigned int>(parsed.container_id),
                        static_cast<unsigned int>(parsed.container_type),
                        parsed.server_initiated ? 1 : 0);
                }
#endif
                if (g_mailbox.visible_anvil && visible_anvil_event) {
                    visible_anvil_event->kind = VisibleAnvilCaptureEventKind::Close;
                    visible_anvil_event->token = g_mailbox.token;
                    visible_anvil_event->window_id = parsed.container_id;
                }
                if (g_mailbox.visible_chest && visible_chest_event) {
                    visible_chest_event->kind = VisibleChestCaptureEventKind::Close;
                    visible_chest_event->token = g_mailbox.token;
                    visible_chest_event->window_id = parsed.container_id;
                }
                return !g_mailbox.visible_anvil && !g_mailbox.visible_chest;
            }
            discardExpiredVisibleAnvilCloseReceiptLocked();
            if (g_visible_anvil_close_receipt.token != 0U &&
                g_visible_anvil_close_receipt.window_id == parsed.container_id &&
                g_visible_anvil_close_receipt.window_type == parsed.container_type) {
                g_visible_anvil_close_receipt.received = true;
                if (visible_anvil_event) {
                    visible_anvil_event->kind = VisibleAnvilCaptureEventKind::Close;
                    visible_anvil_event->token =
                        g_visible_anvil_close_receipt.token;
                    visible_anvil_event->window_id = parsed.container_id;
                }
                // This Open was visible to the stock client. Its Close must
                // also reach the game, irrespective of our receipt state.
                return false;
            }
            if (g_quarantine.active && g_quarantine.open_seen &&
                !g_quarantine.close_seen &&
                g_quarantine.container_id == parsed.container_id &&
                g_quarantine.container_type == parsed.container_type) {
                g_quarantine.close_seen = true;
#if defined(__ANDROID__)
                if (!g_quarantine.from_visible_anvil) {
                    __android_log_print(ANDROID_LOG_INFO,
                        "Infinitecz_MapChestWindow",
                        "close owner=quarantine id=%u type=%u server=%d",
                        static_cast<unsigned int>(parsed.container_id),
                        static_cast<unsigned int>(parsed.container_type),
                        parsed.server_initiated ? 1 : 0);
                }
#endif
                return true;
            }
            return false;
        }

        if (packet_id == kInventorySlotPacketId) {
            // A packet-only chest has no client-side screen context. Once the
            // initial Content was captured, server Slot updates for this same
            // hidden window must stay hidden too; a fresh open is used for
            // authoritative post-transfer verification.
            Reader identity_reader(packet);
            uint32_t inventory_id = 0;
            if (!readWireHeader(&identity_reader, kInventorySlotPacketId) ||
                !identity_reader.varUInt(&inventory_id)) return false;
            std::lock_guard<std::mutex> lock(g_mutex);
            discardExpiredQuarantineLocked();
            if (g_mailbox.active && g_mailbox.open_seen &&
                !g_mailbox.close_seen &&
                inventory_id == g_mailbox.container_id) {
                return !g_mailbox.visible_anvil && !g_mailbox.visible_chest;
            }
            return g_quarantine.active && g_quarantine.open_seen &&
                !g_quarantine.close_seen &&
                inventory_id == g_quarantine.container_id;
        }

        if (packet_id != kInventoryContentPacketId) return false;
        Reader identity_reader(packet);
        uint32_t inventory_id = 0;
        if (!readWireHeader(&identity_reader, kInventoryContentPacketId) ||
            !identity_reader.varUInt(&inventory_id)) {
            return false;
        }
        uint64_t request_token = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            discardExpiredQuarantineLocked();
            if (!g_mailbox.active || !g_mailbox.open_seen || g_mailbox.close_seen ||
                inventory_id != g_mailbox.container_id) {
                return g_quarantine.active && g_quarantine.open_seen &&
                    !g_quarantine.close_seen &&
                    inventory_id == g_quarantine.container_id;
            }
            request_token = g_mailbox.token;
            suppress_packet = !g_mailbox.visible_anvil &&
                !g_mailbox.visible_chest;
        }

        ParsedInventory parsed;
        const char* parse_error = nullptr;
        const bool parsed_ok = parseInventory(packet, &parsed, &parse_error);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_mailbox.active || g_mailbox.token != request_token ||
            !g_mailbox.open_seen ||
            !parsed.inventory_id_seen ||
            parsed.inventory_id != g_mailbox.container_id) {
            return suppress_packet;
        }
        if (!parsed_ok) {
            setMailboxErrorLocked(parse_error
                ? parse_error : "unable to parse container inventory");
            return suppress_packet;
        }
#if defined(__ANDROID__)
        if (!g_mailbox.content_seen && g_mailbox.container_type == kChestWindowType &&
            parsed.slot_count == kSingleChestSlotCount) {
            uint32_t maps_with_uuid = 0;
            for (uint32_t index = 0; index < parsed.item_count; ++index) {
                if (parsed.items[index].has_map_uuid) ++maps_with_uuid;
            }
            __android_log_print(ANDROID_LOG_INFO, "Infinitecz_MapChest",
                "capture window=%u type=%u slots=%u items=%u uuid_tags=%u full_name=%u dynamic=%d dynamic_id=%u",
                static_cast<unsigned int>(g_mailbox.container_id),
                static_cast<unsigned int>(g_mailbox.container_type),
                static_cast<unsigned int>(parsed.slot_count),
                static_cast<unsigned int>(parsed.item_count),
                static_cast<unsigned int>(maps_with_uuid),
                static_cast<unsigned int>(parsed.full_container_name),
                parsed.has_dynamic_container_id ? 1 : 0,
                static_cast<unsigned int>(parsed.dynamic_container_id));
        }
#endif
        g_mailbox.content_seen = true;
        g_mailbox.slot_count = parsed.slot_count;
        g_mailbox.has_full_container_name = parsed.has_full_container_name;
        g_mailbox.full_container_name = parsed.full_container_name;
        g_mailbox.has_dynamic_container_id = parsed.has_dynamic_container_id;
        g_mailbox.dynamic_container_id = parsed.dynamic_container_id;
        g_mailbox.item_count = parsed.item_count;
        for (uint32_t index = 0; index < parsed.item_count; ++index) {
            const ParsedInventory::Item& source = parsed.items[index];
            MailboxItem& destination = g_mailbox.items[index];
            destination = {};
            destination.slot = source.slot;
            destination.numeric_id = source.numeric_id;
            destination.count = source.count;
            destination.aux = source.aux;
            destination.has_network_stack_id = source.has_network_stack_id;
            destination.network_stack_id = source.network_stack_id;
            destination.has_map_uuid = source.has_map_uuid;
            destination.map_uuid = source.map_uuid;
            destination.name_status = source.name_status;
            destination.name_source = source.name_source;
            destination.name_candidate = source.name_candidate;
        }
        return suppress_packet;
    } catch (...) {
        try {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_mailbox.active) {
                setMailboxErrorLocked("container packet capture failed");
            }
        } catch (...) {
        }
        return suppress_packet;
    }
}

bool ObserveManualChestTracePacket(std::string_view packet,
                                   ManualChestTraceEvent* event) noexcept {
    if (event) *event = {};
    try {
        if (packet.empty()) return false;
        Reader header_reader(packet);
        uint32_t wire_header = 0;
        if (!header_reader.varUInt(&wire_header)) return false;
        const uint32_t packet_id = wire_header & 0x3FFU;
        ManualChestTraceEvent observed;

        if (packet_id == kContainerOpenPacketId) {
            ParsedOpen parsed;
            if (!parseOpen(packet, &parsed) ||
                parsed.container_type != kChestWindowType ||
                parsed.container_id == 0U || parsed.container_id == 0xFFU) return false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                discardExpiredQuarantineLocked();
                // Automatic export owns exactly these coordinates; its open
                // packet must not be represented as a player-opened chest.
                if ((g_mailbox.active && !g_mailbox.close_seen &&
                     parsed.x == g_mailbox.x && parsed.y == g_mailbox.y &&
                     parsed.z == g_mailbox.z) ||
                    (g_quarantine.active && !g_quarantine.close_seen &&
                     parsed.x == g_quarantine.x &&
                     parsed.y == g_quarantine.y &&
                     parsed.z == g_quarantine.z)) {
                    if (g_manual_trace.open &&
                        g_manual_trace.container_id == parsed.container_id) {
                        g_manual_trace.open = false;
                        g_manual_trace.confirmed_single_chest = false;
                        ++g_manual_trace.generation;
                    }
                    return false;
                }
                if (!g_manual_trace.enabled) {
                    g_manual_trace.enabled = true;
                    g_manual_trace.started_at = std::chrono::steady_clock::now();
                }
                g_manual_trace.open = true;
                g_manual_trace.confirmed_single_chest = false;
                ++g_manual_trace.generation;
                g_manual_trace.container_id = parsed.container_id;
                g_manual_trace.container_type = parsed.container_type;
                g_manual_trace.x = parsed.x;
                g_manual_trace.y = parsed.y;
                g_manual_trace.z = parsed.z;
                if (!manualTraceCanLogLocked()) return false;
                observed.kind = ManualChestTraceEventKind::Open;
                observed.container_id = parsed.container_id;
                observed.container_type = parsed.container_type;
                observed.x = parsed.x;
                observed.y = parsed.y;
                observed.z = parsed.z;
                observed.parsed = true;
            }
        } else if (packet_id == kInventoryContentPacketId ||
                   packet_id == kInventorySlotPacketId) {
            Reader identity_reader(packet);
            uint32_t inventory_id = 0;
            if (!readWireHeader(&identity_reader, packet_id) ||
                !identity_reader.varUInt(&inventory_id)) return false;
            uint64_t generation = 0;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!g_manual_trace.open ||
                    inventory_id != g_manual_trace.container_id ||
                    manualTraceAutomaticWindowLocked(
                        static_cast<uint8_t>(inventory_id)) ||
                    (packet_id == kInventorySlotPacketId &&
                     !g_manual_trace.confirmed_single_chest)) return false;
                generation = g_manual_trace.generation;
            }

            ParsedInventory parsed_content;
            ParsedInventorySlot parsed_slot;
            const char* ignored_error = nullptr;
            const bool parsed_ok = packet_id == kInventoryContentPacketId
                ? parseInventory(packet, &parsed_content, &ignored_error)
                : parseInventorySlot(packet, &parsed_slot);
            if (packet_id == kInventorySlotPacketId &&
                (!parsed_ok || parsed_slot.inventory_id != inventory_id)) return false;
            if (packet_id == kInventoryContentPacketId && parsed_ok &&
                (parsed_content.inventory_id != inventory_id ||
                 parsed_content.slot_count != kSingleChestSlotCount)) return false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!g_manual_trace.open ||
                    g_manual_trace.generation != generation ||
                    inventory_id != g_manual_trace.container_id ||
                    manualTraceAutomaticWindowLocked(
                        static_cast<uint8_t>(inventory_id)) ||
                    !manualTraceCanLogLocked()) return false;
                observed.container_id = g_manual_trace.container_id;
                observed.container_type = g_manual_trace.container_type;
                observed.x = g_manual_trace.x;
                observed.y = g_manual_trace.y;
                observed.z = g_manual_trace.z;
                observed.parsed = parsed_ok;
                if (packet_id == kInventoryContentPacketId) {
                    observed.kind = ManualChestTraceEventKind::Content;
                    if (parsed_ok) {
                        observed.slot_count = parsed_content.slot_count;
                        observed.item_count = parsed_content.item_count;
                        observed.has_full_container_name =
                            parsed_content.has_full_container_name;
                        observed.full_container_name =
                            parsed_content.full_container_name;
                        observed.has_dynamic_container_id =
                            parsed_content.has_dynamic_container_id;
                        observed.dynamic_container_id =
                            parsed_content.dynamic_container_id;
                        g_manual_trace.confirmed_single_chest =
                            parsed_content.has_full_container_name &&
                            (parsed_content.full_container_name == 0U ||
                             parsed_content.full_container_name ==
                                 kChestRequestContainerName) &&
                            !parsed_content.has_dynamic_container_id;
                        for (uint32_t index = 0;
                             index < parsed_content.item_count; ++index) {
                            const auto& item = parsed_content.items[index];
                            if (!item.has_map_uuid) continue;
                            ++observed.map_uuid_count;
                            if (observed.map_sample_count <
                                observed.map_samples.size()) {
                                auto& sample = observed.map_samples[
                                    observed.map_sample_count++];
                                sample.slot = item.slot;
                                sample.map_uuid = item.map_uuid;
                                sample.name_status = item.name_status;
                                sample.name_source = item.name_source;
                                sample.name_bytes = static_cast<uint16_t>(
                                    item.name_candidate.size());
                            }
                        }
                    }
                } else {
                    observed.kind = ManualChestTraceEventKind::Slot;
                    observed.changed_slot =
                        static_cast<uint16_t>(parsed_slot.slot);
                    observed.slot_occupied = parsed_slot.item.numeric_id != 0;
                    observed.slot_has_network_stack_id =
                        parsed_slot.item.has_network_stack_id;
                    observed.slot_network_stack_id =
                        parsed_slot.item.network_stack_id;
                    observed.slot_has_map_uuid = parsed_slot.item.has_map_uuid;
                    observed.slot_map_uuid = parsed_slot.item.map_uuid;
                    observed.slot_name_status = parsed_slot.item.name_status;
                    observed.slot_name_source = parsed_slot.item.name_source;
                    observed.slot_name_bytes = static_cast<uint16_t>(
                        parsed_slot.item.name_candidate.size());
                }
            }
        } else if (packet_id == kContainerClosePacketId) {
            ParsedClose parsed;
            if (!parseClose(packet, &parsed)) return false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!g_manual_trace.open ||
                    parsed.container_id != g_manual_trace.container_id ||
                    parsed.container_type != g_manual_trace.container_type ||
                    manualTraceAutomaticWindowLocked(parsed.container_id)) {
                    return false;
                }
                g_manual_trace.open = false;
                g_manual_trace.confirmed_single_chest = false;
                ++g_manual_trace.generation;
                if (!manualTraceCanLogLocked()) return false;
                observed.kind = ManualChestTraceEventKind::Close;
                observed.container_id = parsed.container_id;
                observed.container_type = parsed.container_type;
                observed.x = g_manual_trace.x;
                observed.y = g_manual_trace.y;
                observed.z = g_manual_trace.z;
                observed.parsed = true;
                observed.close_server_initiated = parsed.server_initiated;
            }
        } else {
            return false;
        }
        if (event) *event = observed;
        logManualChestTraceEvent(observed);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace build_import
