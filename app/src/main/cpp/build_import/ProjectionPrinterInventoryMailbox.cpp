#include "ProjectionPrinterInventoryMailbox.h"

#include "ItemRuntimeRegistry.h"
#include "ProjectionPrinterInventoryDiagnostics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace build_import {
namespace {

constexpr uint32_t kDisconnectPacketId = 0x05U;
constexpr uint32_t kStartGamePacketId = 0x0BU;
constexpr uint32_t kChangeDimensionPacketId = 0x3DU;
constexpr uint32_t kInventoryContentPacketId = 0x31U;
constexpr uint32_t kInventorySlotPacketId = 0x32U;
constexpr uint32_t kItemStackResponsePacketId = 0x94U;

// InventoryContent uses the network inventory ID, not the ItemStackRequest
// FullContainerName enum. In this protocol the ordinary player inventory is
// inventory 0 and has exactly the 36 slots exposed by the game client.
constexpr uint32_t kPlayerInventoryNetworkId = 0U;
constexpr size_t kMaximumInventoryPacketBytes = 10U * 1024U * 1024U;
constexpr uint32_t kMaximumItemUserDataBytes = 5U * 1024U * 1024U;
constexpr uint32_t kMaximumResponseEntries = 64U;
constexpr uint32_t kMaximumResponseContainers = 64U;
constexpr uint32_t kMaximumResponseSlotsPerContainer = 256U;
constexpr size_t kMaximumStoredResponseSlots = 128U;
constexpr uint32_t kMaximumResponseCustomNameBytes = 256U;
constexpr uint32_t kMapManualTraceMaximumLines = 128U;
constexpr auto kMapManualTraceMaximumDuration = std::chrono::minutes(20);

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

    bool varInt(int32_t* output) {
        uint32_t encoded = 0;
        if (!output || !varUInt(&encoded)) return false;
        *output = static_cast<int32_t>((encoded >> 1U) ^ (0U - (encoded & 1U)));
        return true;
    }

    bool skip(size_t count) {
        if (count > input_.size() - cursor_) return false;
        cursor_ += count;
        return true;
    }

    bool string(std::string* output, uint32_t maximum_bytes) {
        uint32_t length = 0;
        if (!varUInt(&length) || length > maximum_bytes ||
            length > input_.size() - cursor_) {
            return false;
        }
        if (output) output->assign(input_.data() + cursor_, length);
        cursor_ += length;
        return true;
    }

    bool atEnd() const { return cursor_ == input_.size(); }

private:
    std::string_view input_;
    size_t cursor_ = 0;
};

struct ParsedItem {
    bool occupied = false;
    int32_t runtime_item_id = 0;
    bool has_network_stack_id = false;
    int32_t network_stack_id = 0;
    uint16_t count = 0;
    uint16_t aux = 0;
};

struct ParsedInventoryContent {
    uint32_t inventory_id = 0;
    uint32_t slot_count = 0;
    std::array<ParsedItem, kProjectionPrinterPlayerInventorySlotCount> slots{};
};

struct ParsedInventorySlot {
    uint32_t inventory_id = 0;
    uint32_t slot = 0;
    ParsedItem item;
};

struct ParsedResponseSlot {
    uint8_t container_id = 0;
    bool has_dynamic_container_id = false;
    uint32_t dynamic_container_id = 0;
    bool has_hotbar_slot = false;
    uint8_t hotbar_slot = 0;
    bool has_requested_slot = false;
    uint8_t requested_slot = 0;
    uint8_t slot = 0;
    uint8_t count = 0;
    int32_t network_stack_id = 0;
    std::string custom_name;
    std::string filter_custom_name;
    int32_t durability_correction = 0;
};

struct ParsedResponseContainer {
    uint8_t container_id = 0;
    bool has_dynamic_container_id = false;
    uint32_t dynamic_container_id = 0;
    uint32_t declared_slot_count = 0;
    size_t first_slot_index = 0;
};

struct ParsedResponseEntry {
    // A retained entry is complete only when every response slot of this
    // single wire entry fit in the bounded entry representation. Entries that
    // exceed the slot cap are still fully parsed for packet alignment, but are
    // deliberately not exposed as an incomplete transaction result.
    bool complete = true;
    uint8_t status = 0;
    int32_t request_id = 0;
    bool succeeded = false;
    ProjectionPrinterInventoryResponseLayout layout =
        ProjectionPrinterInventoryResponseLayout::None;
    std::vector<ParsedResponseContainer> containers;
    std::vector<ParsedResponseSlot> slots;
};

struct ParsedResponsePacket {
    uint32_t entry_count = 0;
    uint32_t successful_entry_count = 0;
    // Only the newest bounded subset is retained. Earlier entries are still
    // decoded to validate the packet but will naturally already be stale by
    // the time a caller issues a lookup.
    std::vector<ParsedResponseEntry> entries;
};

struct StoredResponseEntry {
    bool occupied = false;
    uint8_t status = 0;
    int32_t request_id = 0;
    ProjectionPrinterInventoryResponseLayout layout =
        ProjectionPrinterInventoryResponseLayout::None;
    uint64_t response_generation = 0;
    uint64_t observed_inventory_revision = 0;
    uint32_t packet_entry_count = 0;
    uint32_t packet_successful_entry_count = 0;
    std::vector<ParsedResponseSlot> slots;
};

struct StoredSlot {
    bool occupied = false;
    int32_t runtime_item_id = 0;
    bool has_network_stack_id = false;
    int32_t network_stack_id = 0;
    uint16_t count = 0;
    uint16_t aux = 0;
};

struct Mailbox {
    bool ready = false;
    uint32_t inventory_id = 0;
    uint64_t session_generation = 1U;
    uint64_t revision = 0;
    std::array<StoredSlot, kProjectionPrinterPlayerInventorySlotCount> slots{};
    uint64_t response_generation = 0;
    std::array<StoredResponseEntry,
               static_cast<size_t>(kProjectionPrinterInventoryResponseHistoryCapacity)>
        response_history{};
    size_t response_history_next = 0;
    size_t response_history_count = 0;
    bool has_latest_response_entry = false;
    size_t latest_response_entry_index = 0;
};

enum class MapManualTraceState : uint8_t {
    Off = 0,
    Active,
    Finished,
};

struct MapManualTraceSession {
    MapManualTraceState state = MapManualTraceState::Off;
    std::chrono::steady_clock::time_point deadline{};
    uint32_t remaining_lines = 0;
};

std::mutex g_mutex;
Mailbox g_mailbox;
MapManualTraceSession g_map_manual_trace;

void logMapManualTraceLineLocked(const char* format, ...) {
    if (g_map_manual_trace.state != MapManualTraceState::Active) return;
    if (g_map_manual_trace.remaining_lines == 0U ||
        std::chrono::steady_clock::now() >= g_map_manual_trace.deadline) {
        g_map_manual_trace.state = MapManualTraceState::Finished;
        return;
    }
    --g_map_manual_trace.remaining_lines;
#if defined(__ANDROID__)
    va_list arguments;
    va_start(arguments, format);
    __android_log_vprint(ANDROID_LOG_INFO, "Infinitecz_MapManualTrace",
                         format, arguments);
    va_end(arguments);
#else
    (void)format;
#endif
    if (g_map_manual_trace.remaining_lines == 0U) {
        g_map_manual_trace.state = MapManualTraceState::Finished;
    }
}

void logMapManualResponseLocked(const ParsedResponsePacket& parsed,
                                uint64_t response_generation) {
    if (g_map_manual_trace.state != MapManualTraceState::Active) return;
    for (const ParsedResponseEntry& entry : parsed.entries) {
        logMapManualTraceLineLocked(
            "response request=%d status=%u session=%llu generation=%llu "
            "packet_entries=%u containers=%zu slots=%zu",
            entry.request_id, static_cast<unsigned>(entry.status),
            static_cast<unsigned long long>(g_mailbox.session_generation),
            static_cast<unsigned long long>(response_generation),
            parsed.entry_count, entry.containers.size(), entry.slots.size());
        for (size_t container_index = 0; container_index < entry.containers.size();
             ++container_index) {
            const ParsedResponseContainer& container =
                entry.containers[container_index];
            logMapManualTraceLineLocked(
                "response_container request=%d index=%zu name=%u dynamic=%u "
                "dynamic_id=%u slots=%u",
                entry.request_id, container_index,
                static_cast<unsigned>(container.container_id),
                container.has_dynamic_container_id ? 1U : 0U,
                container.dynamic_container_id, container.declared_slot_count);
            const size_t end = container.first_slot_index +
                container.declared_slot_count;
            for (size_t slot_index = container.first_slot_index;
                 slot_index < end && slot_index < entry.slots.size(); ++slot_index) {
                const ParsedResponseSlot& slot = entry.slots[slot_index];
                logMapManualTraceLineLocked(
                    "response_slot request=%d container_index=%zu slot=%u "
                    "hotbar_slot=%u count=%u network_id=%d custom_name_bytes=%zu "
                    "filter_name_bytes=%zu durability=%d",
                    entry.request_id, container_index,
                    static_cast<unsigned>(slot.slot),
                    static_cast<unsigned>(slot.hotbar_slot),
                    static_cast<unsigned>(slot.count), slot.network_stack_id,
                    slot.custom_name.size(), slot.filter_custom_name.size(),
                    slot.durability_correction);
            }
        }
    }
}

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

bool parseItem(Reader* reader, ParsedItem* output) {
    if (!reader || !output) return false;
    *output = {};
    int32_t runtime_item_id = 0;
    if (!reader->varInt(&runtime_item_id)) return false;
    if (runtime_item_id == 0) return true;

    uint16_t count = 0;
    uint32_t aux = 0;
    uint8_t has_network_stack_id = 0;
    int32_t network_stack_id = 0;
    int32_t ignored_block_runtime_id = 0;
    uint32_t user_data_bytes = 0;
    if (!reader->littleEndian16(&count) || count == 0U || !reader->varUInt(&aux) ||
        aux > std::numeric_limits<uint16_t>::max() ||
        !reader->byte(&has_network_stack_id) || has_network_stack_id > 1U ||
        (has_network_stack_id != 0U && !reader->varInt(&network_stack_id)) ||
        !reader->varInt(&ignored_block_runtime_id) || !reader->varUInt(&user_data_bytes) ||
        user_data_bytes > kMaximumItemUserDataBytes || !reader->skip(user_data_bytes)) {
        return false;
    }
    output->occupied = true;
    output->runtime_item_id = runtime_item_id;
    output->has_network_stack_id = has_network_stack_id != 0U;
    output->network_stack_id = network_stack_id;
    output->count = count;
    output->aux = static_cast<uint16_t>(aux);
    return true;
}

// Since protocol v748 InventoryContent ends with FullContainerName plus a
// storage ItemData. The current project already validates this exact v859
// tail in ContainerPacketPipelineTest; requiring it prevents a truncated
// packet from becoming a trusted player snapshot.
bool parseFullContainerName(Reader* reader, uint8_t* container_id,
                            bool* has_dynamic_id_output, uint32_t* dynamic_id) {
    uint8_t wire_has_dynamic_id = 0;
    uint32_t parsed_dynamic_id = 0;
    if (!reader || !reader->byte(container_id) || !reader->byte(&wire_has_dynamic_id) ||
        wire_has_dynamic_id > 1U ||
        (wire_has_dynamic_id != 0U && !reader->littleEndian32(&parsed_dynamic_id))) {
        return false;
    }
    if (wire_has_dynamic_id != 0U) {
        if (has_dynamic_id_output) *has_dynamic_id_output = true;
        if (dynamic_id) *dynamic_id = parsed_dynamic_id;
    } else {
        if (has_dynamic_id_output) *has_dynamic_id_output = false;
        if (dynamic_id) *dynamic_id = 0;
    }
    return true;
}

bool parseOptionalFullContainerName(Reader* reader, uint8_t* container_id,
                                    bool* has_dynamic_id, uint32_t* dynamic_id) {
    uint8_t present = 0;
    if (!reader || !reader->byte(&present) || present > 1U) return false;
    if (present == 0U) {
        if (container_id) *container_id = 0;
        if (has_dynamic_id) *has_dynamic_id = false;
        if (dynamic_id) *dynamic_id = 0;
        return true;
    }
    return parseFullContainerName(reader, container_id, has_dynamic_id, dynamic_id);
}

bool parseOptionalItem(Reader* reader, ParsedItem* output) {
    uint8_t present = 0;
    if (!reader || !reader->byte(&present) || present > 1U) return false;
    if (present == 0U) {
        if (output) *output = {};
        return true;
    }
    return parseItem(reader, output);
}

bool parseInventoryContentTail(Reader* reader) {
    uint8_t ignored_full_container_name = 0;
    bool ignored_has_dynamic_id = false;
    uint32_t ignored_dynamic_id = 0;
    ParsedItem ignored_storage_item;
    return parseFullContainerName(reader, &ignored_full_container_name,
                                  &ignored_has_dynamic_id, &ignored_dynamic_id) &&
        parseItem(reader, &ignored_storage_item) && reader->atEnd();
}

bool parseInventoryContent(std::string_view packet, ParsedInventoryContent* output) {
    if (!output || packet.size() > kMaximumInventoryPacketBytes) return false;
    *output = {};
    Reader reader(packet);
    uint32_t slot_count = 0;
    if (!readWireHeader(&reader, kInventoryContentPacketId) ||
        !reader.varUInt(&output->inventory_id) || !reader.varUInt(&slot_count) ||
        output->inventory_id != kPlayerInventoryNetworkId ||
        slot_count != kProjectionPrinterPlayerInventorySlotCount) {
        return false;
    }
    output->slot_count = slot_count;
    for (uint32_t index = 0; index < slot_count; ++index) {
        if (!parseItem(&reader, &output->slots[index])) return false;
    }
    return parseInventoryContentTail(&reader);
}

bool parseInventorySlotCurrent(Reader* reader, ParsedInventorySlot* output) {
    if (!reader || !output) return false;
    uint8_t ignored_full_container_name = 0;
    bool ignored_has_dynamic_id = false;
    uint32_t ignored_dynamic_id = 0;
    ParsedItem ignored_storage_item;
    return readWireHeader(reader, kInventorySlotPacketId) &&
        reader->varUInt(&output->inventory_id) && reader->varUInt(&output->slot) &&
        output->slot < kProjectionPrinterPlayerInventorySlotCount &&
        parseOptionalFullContainerName(reader, &ignored_full_container_name,
                                       &ignored_has_dynamic_id, &ignored_dynamic_id) &&
        parseOptionalItem(reader, &ignored_storage_item) &&
        parseItem(reader, &output->item) && reader->atEnd();
}

bool parseInventorySlotV859(Reader* reader, ParsedInventorySlot* output) {
    if (!reader || !output) return false;
    uint8_t ignored_full_container_name = 0;
    bool ignored_has_dynamic_id = false;
    uint32_t ignored_dynamic_id = 0;
    ParsedItem ignored_storage_item;
    return readWireHeader(reader, kInventorySlotPacketId) &&
        reader->varUInt(&output->inventory_id) && reader->varUInt(&output->slot) &&
        output->slot < kProjectionPrinterPlayerInventorySlotCount &&
        parseFullContainerName(reader, &ignored_full_container_name,
                               &ignored_has_dynamic_id, &ignored_dynamic_id) &&
        parseItem(reader, &ignored_storage_item) && parseItem(reader, &output->item) &&
        reader->atEnd();
}

bool parseInventorySlot(std::string_view packet, ParsedInventorySlot* output) {
    if (!output || packet.size() > kMaximumInventoryPacketBytes) return false;
    ParsedInventorySlot parsed;
    Reader current_reader(packet);
    if (parseInventorySlotCurrent(&current_reader, &parsed)) {
        *output = parsed;
        return true;
    }
    Reader v859_reader(packet);
    if (!parseInventorySlotV859(&v859_reader, &parsed)) return false;
    *output = parsed;
    return true;
}

// v859 ItemStackResponse is deliberately decoded as one fixed wire layout:
// Response { status:u8, request_id:zigzag32, containers[] }, followed on a
// success path by ResponseSlot { Slot:u8, HotbarSlot:u8, Count:u8,
// ItemStackNetId:zigzag32, CustomName:string, FilteredCustomName:string,
// DurabilityCorrection:zigzag32 }.  In particular, HotbarSlot is not an
// optional RequestedSlot field.  Treating it as one made a two-byte slot
// prefix parse differently depending on the bytes in a real response.
bool parseItemStackResponse(std::string_view packet, ParsedResponsePacket* output) {
    if (!output || packet.size() > kMaximumInventoryPacketBytes) return false;
    *output = {};
    Reader reader(packet);
    uint32_t response_count = 0;
    if (!readWireHeader(&reader, kItemStackResponsePacketId) ||
        !reader.varUInt(&response_count) || response_count > kMaximumResponseEntries) {
        return false;
    }

    constexpr uint32_t kHistoryCapacity =
        kProjectionPrinterInventoryResponseHistoryCapacity;
    const uint32_t first_retained_response = response_count > kHistoryCapacity
        ? response_count - kHistoryCapacity
        : 0U;
    output->entry_count = response_count;
    output->entries.reserve(std::min<size_t>(
        static_cast<size_t>(response_count), static_cast<size_t>(kHistoryCapacity)));

    for (uint32_t response_index = 0; response_index < response_count;
         ++response_index) {
        uint8_t status = 0;
        int32_t request_id = 0;
        if (!reader.byte(&status) || !reader.varInt(&request_id)) return false;

        const bool retain = response_index >= first_retained_response;
        ParsedResponseEntry entry;
        entry.status = status;
        entry.request_id = request_id;
        entry.succeeded = status == 0U;

        // Failed responses have no container payload.  Preserve the raw status
        // and signed request id as a complete, standalone transaction result.
        if (status != 0U) {
            if (retain) output->entries.push_back(std::move(entry));
            continue;
        }

        ++output->successful_entry_count;
        entry.layout = ProjectionPrinterInventoryResponseLayout::
            V859SlotHotbarSlotAmountNetworkId;
        uint32_t container_count = 0;
        if (!reader.varUInt(&container_count) ||
            container_count > kMaximumResponseContainers) {
            return false;
        }
        for (uint32_t container_index = 0; container_index < container_count;
             ++container_index) {
            uint8_t container_id = 0;
            bool has_dynamic_container_id = false;
            uint32_t dynamic_container_id = 0;
            uint32_t slot_count = 0;
            if (!parseFullContainerName(&reader, &container_id,
                                        &has_dynamic_container_id,
                                        &dynamic_container_id) ||
                !reader.varUInt(&slot_count) ||
                slot_count > kMaximumResponseSlotsPerContainer) {
                return false;
            }
            if (retain && entry.complete) {
                ParsedResponseContainer container;
                container.container_id = container_id;
                container.has_dynamic_container_id = has_dynamic_container_id;
                container.dynamic_container_id = dynamic_container_id;
                container.declared_slot_count = slot_count;
                container.first_slot_index = entry.slots.size();
                entry.containers.push_back(container);
            }
            for (uint32_t slot_index = 0; slot_index < slot_count; ++slot_index) {
                ParsedResponseSlot parsed;
                const bool store_slot = retain && entry.complete &&
                    entry.slots.size() < kMaximumStoredResponseSlots;
                if (!reader.byte(&parsed.slot) || !reader.byte(&parsed.hotbar_slot) ||
                    !reader.byte(&parsed.count) ||
                    !reader.varInt(&parsed.network_stack_id) ||
                    !reader.string(store_slot ? &parsed.custom_name : nullptr,
                                   kMaximumResponseCustomNameBytes) ||
                    !reader.string(store_slot ? &parsed.filter_custom_name : nullptr,
                                   kMaximumResponseCustomNameBytes) ||
                    !reader.varInt(&parsed.durability_correction)) {
                    return false;
                }
                parsed.has_hotbar_slot = true;
                parsed.container_id = container_id;
                parsed.has_dynamic_container_id = has_dynamic_container_id;
                parsed.dynamic_container_id = dynamic_container_id;
                if (store_slot) {
                    entry.slots.push_back(std::move(parsed));
                } else if (retain) {
                    // We must not expose a partially retained success entry as
                    // authoritative to a caller matching a native transaction.
                    entry.complete = false;
                }
            }
        }
        if (retain && entry.complete) output->entries.push_back(std::move(entry));
    }
    return reader.atEnd();
}

void storeResponseEntryLocked(ParsedResponseEntry&& source,
                              uint32_t packet_entry_count,
                              uint32_t packet_successful_entry_count,
                              uint64_t response_generation,
                              uint64_t observed_inventory_revision) {
    constexpr size_t kHistoryCapacity = static_cast<size_t>(
        kProjectionPrinterInventoryResponseHistoryCapacity);
    static_assert(kHistoryCapacity > 0U, "response history must have capacity");

    StoredResponseEntry stored;
    stored.occupied = true;
    stored.status = source.status;
    stored.request_id = source.request_id;
    stored.layout = source.layout;
    stored.response_generation = response_generation;
    stored.observed_inventory_revision = observed_inventory_revision;
    stored.packet_entry_count = packet_entry_count;
    stored.packet_successful_entry_count = packet_successful_entry_count;
    stored.slots = std::move(source.slots);

    const size_t index = g_mailbox.response_history_next;
    g_mailbox.response_history[index] = std::move(stored);
    g_mailbox.latest_response_entry_index = index;
    g_mailbox.has_latest_response_entry = true;
    g_mailbox.response_history_next = (index + 1U) % kHistoryCapacity;
    if (g_mailbox.response_history_count < kHistoryCapacity) {
        ++g_mailbox.response_history_count;
    }
}

bool copyResponseEntryLocked(const StoredResponseEntry& source,
                             uint64_t session_generation,
                             ProjectionPrinterInventoryResponse* output) {
    if (!output || !source.occupied) return false;
    ProjectionPrinterInventoryResponse response;
    response.status = source.status;
    response.valid = source.status == 0U;
    response.rejected = source.status != 0U;
    response.rejection_status = response.rejected ? source.status : 0U;
    response.rejection_request_id = response.rejected ? source.request_id : 0;
    response.rejection_observed_inventory_revision = response.rejected
        ? source.observed_inventory_revision
        : 0U;
    response.response_generation = source.response_generation;
    response.entry_count = source.packet_entry_count;
    response.successful_entry_count = source.packet_successful_entry_count;
    response.layout = source.layout;
    response.session_generation = session_generation;
    response.observed_inventory_revision = source.observed_inventory_revision;
    response.request_id = source.request_id;
    response.slots.reserve(source.slots.size());
    for (const ParsedResponseSlot& source_slot : source.slots) {
        ProjectionPrinterInventoryResponseSlot destination;
        destination.container_id = source_slot.container_id;
        destination.has_dynamic_container_id = source_slot.has_dynamic_container_id;
        destination.dynamic_container_id = source_slot.dynamic_container_id;
        destination.has_hotbar_slot = source_slot.has_hotbar_slot;
        destination.hotbar_slot = source_slot.hotbar_slot;
        destination.has_requested_slot = source_slot.has_requested_slot;
        destination.requested_slot = source_slot.requested_slot;
        destination.slot = source_slot.slot;
        destination.count = source_slot.count;
        destination.network_stack_id = source_slot.network_stack_id;
        destination.custom_name = source_slot.custom_name;
        destination.filter_custom_name = source_slot.filter_custom_name;
        destination.durability_correction = source_slot.durability_correction;
        response.slots.push_back(std::move(destination));
    }
    *output = std::move(response);
    return true;
}

void clearMailboxLocked() {
    const uint64_t next_generation = g_mailbox.session_generation ==
        std::numeric_limits<uint64_t>::max()
        ? 1U : g_mailbox.session_generation + 1U;
    g_mailbox = {};
    g_mailbox.session_generation = next_generation;
}

void writeSlot(StoredSlot* destination, const ParsedItem& source) {
    if (!destination) return;
    destination->occupied = source.occupied;
    destination->runtime_item_id = source.runtime_item_id;
    destination->has_network_stack_id = source.has_network_stack_id;
    destination->network_stack_id = source.network_stack_id;
    destination->count = source.count;
    destination->aux = source.aux;
}

}  // namespace

void BeginMapManualTraceResponseSession() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_map_manual_trace.state != MapManualTraceState::Off) return;
        g_map_manual_trace.state = MapManualTraceState::Active;
        g_map_manual_trace.deadline = std::chrono::steady_clock::now() +
            kMapManualTraceMaximumDuration;
        g_map_manual_trace.remaining_lines = kMapManualTraceMaximumLines;
    } catch (...) {
    }
}

void EndMapManualTraceResponseSession() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_map_manual_trace.state = MapManualTraceState::Finished;
        g_map_manual_trace.remaining_lines = 0U;
    } catch (...) {
    }
}

void ClearProjectionPrinterInventoryMailbox() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        clearMailboxLocked();
    } catch (...) {
    }
}

void ObserveProjectionPrinterInventoryPacket(std::string_view packet) noexcept {
    try {
        if (packet.empty()) return;
        uint32_t packet_id = 0;
        if (!readPacketId(packet, &packet_id)) return;
        if (packet_id == kStartGamePacketId || packet_id == kDisconnectPacketId ||
            packet_id == kChangeDimensionPacketId) {
            ClearProjectionPrinterInventoryMailbox();
            return;
        }
        if (packet_id == kInventoryContentPacketId) {
            ParsedInventoryContent parsed;
            if (!parseInventoryContent(packet, &parsed)) return;
            std::lock_guard<std::mutex> lock(g_mutex);
            const uint64_t generation = g_mailbox.session_generation;
            g_mailbox.ready = true;
            g_mailbox.inventory_id = parsed.inventory_id;
            ++g_mailbox.revision;
            if (g_mailbox.revision == 0U) ++g_mailbox.revision;
            for (uint32_t index = 0; index < parsed.slot_count; ++index) {
                writeSlot(&g_mailbox.slots[index], parsed.slots[index]);
            }
            // Preserve a nonzero generation even if a malformed caller reset
            // the internal state before this content packet arrived.
            g_mailbox.session_generation = generation == 0U ? 1U : generation;
            return;
        }
        if (packet_id == kInventorySlotPacketId) {
            ParsedInventorySlot parsed;
            if (!parseInventorySlot(packet, &parsed)) return;
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_mailbox.ready || parsed.inventory_id != g_mailbox.inventory_id) return;
            writeSlot(&g_mailbox.slots[parsed.slot], parsed.item);
            ++g_mailbox.revision;
            if (g_mailbox.revision == 0U) ++g_mailbox.revision;
            return;
        }
        if (packet_id != kItemStackResponsePacketId) return;
        ParsedResponsePacket parsed;
        if (!parseItemStackResponse(packet, &parsed)) {
            std::lock_guard<std::mutex> lock(g_mutex);
            logMapManualTraceLineLocked("response_parse_failed bytes=%zu",
                                        packet.size());
            return;
        }
        if (parsed.entry_count == 0U) {
            std::lock_guard<std::mutex> lock(g_mutex);
            logMapManualTraceLineLocked("response_empty bytes=%zu", packet.size());
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_mailbox.response_generation;
        if (g_mailbox.response_generation == 0U) ++g_mailbox.response_generation;
        logMapManualResponseLocked(parsed, g_mailbox.response_generation);
        for (ParsedResponseEntry& entry : parsed.entries) {
            LogProjectionPrinterInventoryDiagnostic(
                "response_receive request=%d status=%u session=%llu generation=%llu "
                "inventory_revision=%llu slots=%zu packet_entries=%u",
                entry.request_id, static_cast<unsigned>(entry.status),
                static_cast<unsigned long long>(g_mailbox.session_generation),
                static_cast<unsigned long long>(g_mailbox.response_generation),
                static_cast<unsigned long long>(g_mailbox.revision), entry.slots.size(),
                parsed.entry_count);
            storeResponseEntryLocked(std::move(entry), parsed.entry_count,
                                     parsed.successful_entry_count,
                                     g_mailbox.response_generation,
                                     g_mailbox.revision);
        }
    } catch (...) {
        // The receive hook must never unwind through the client networking
        // thread. Retain the last complete snapshot on malformed input.
    }
}

bool GetProjectionPrinterInventorySnapshot(
    ProjectionPrinterInventorySnapshot* output) noexcept {
    try {
        if (!output) return false;
        std::array<StoredSlot, kProjectionPrinterPlayerInventorySlotCount> stored{};
        uint64_t generation = 0;
        uint64_t revision = 0;
        uint32_t inventory_id = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_mailbox.ready) {
                *output = {};
                return false;
            }
            stored = g_mailbox.slots;
            generation = g_mailbox.session_generation;
            revision = g_mailbox.revision;
            inventory_id = g_mailbox.inventory_id;
        }
        ProjectionPrinterInventorySnapshot snapshot;
        snapshot.ready = true;
        snapshot.inventory_id = inventory_id;
        snapshot.session_generation = generation;
        snapshot.revision = revision;
        for (size_t index = 0; index < stored.size(); ++index) {
            const StoredSlot& source = stored[index];
            ProjectionPrinterInventorySlot& destination = snapshot.slots[index];
            destination.slot = static_cast<uint8_t>(index);
            destination.occupied = source.occupied;
            destination.runtime_item_id = source.runtime_item_id;
            destination.has_network_stack_id = source.has_network_stack_id;
            destination.network_stack_id = source.network_stack_id;
            destination.count = source.count;
            destination.aux = source.aux;
            if (source.occupied) {
                (void)ResolveItemRuntimeId(source.runtime_item_id, &destination.name);
            }
        }
        *output = std::move(snapshot);
        return true;
    } catch (...) {
        if (output) *output = {};
        return false;
    }
}

bool GetProjectionPrinterInventoryResponse(
    ProjectionPrinterInventoryResponse* output) noexcept {
    try {
        if (!output) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_mailbox.has_latest_response_entry) {
            *output = {};
            return false;
        }
        return copyResponseEntryLocked(
            g_mailbox.response_history[g_mailbox.latest_response_entry_index],
            g_mailbox.session_generation, output);
    } catch (...) {
        if (output) *output = {};
        return false;
    }
}

bool GetProjectionPrinterInventoryResponseByRequestId(
    int32_t request_id, ProjectionPrinterInventoryResponse* output) noexcept {
    try {
        if (!output) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        constexpr size_t kHistoryCapacity = static_cast<size_t>(
            kProjectionPrinterInventoryResponseHistoryCapacity);
        for (size_t offset = 0; offset < g_mailbox.response_history_count; ++offset) {
            const size_t index = (g_mailbox.response_history_next + kHistoryCapacity -
                                  1U - offset) % kHistoryCapacity;
            const StoredResponseEntry& entry = g_mailbox.response_history[index];
            if (entry.occupied && entry.request_id == request_id) {
                return copyResponseEntryLocked(entry, g_mailbox.session_generation,
                                               output);
            }
        }
        *output = {};
        return false;
    } catch (...) {
        if (output) *output = {};
        return false;
    }
}

uint64_t GetProjectionPrinterInventoryMailboxSessionGeneration() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_mailbox.session_generation;
    } catch (...) {
        return 0U;
    }
}

}  // namespace build_import
