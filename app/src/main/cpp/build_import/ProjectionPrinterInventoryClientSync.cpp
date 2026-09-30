#include "ProjectionPrinterInventoryClientSync.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace build_import {
namespace {

constexpr uint32_t kDisconnectPacketId = 0x05U;
constexpr uint32_t kStartGamePacketId = 0x0BU;
constexpr uint32_t kChangeDimensionPacketId = 0x3DU;
constexpr uint32_t kInventoryContentPacketId = 0x31U;
constexpr uint32_t kInventorySlotPacketId = 0x32U;
constexpr uint32_t kPacketIdMask = 0x3FFU;
constexpr uint32_t kPlayerInventoryNetworkId = 0U;
constexpr uint32_t kPlayerInventorySlotCount = 36U;
constexpr size_t kMaximumInventoryPacketBytes = 10U * 1024U * 1024U;
constexpr uint32_t kMaximumItemUserDataBytes = 5U * 1024U * 1024U;
constexpr size_t kMaximumQueuedPackets = 16U;
constexpr size_t kMaximumQueuedPacketBytes = 20U * 1024U * 1024U;
constexpr size_t kMaximumTicketHistory = 64U;

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

    size_t position() const { return cursor_; }
    bool atEnd() const { return cursor_ == input_.size(); }

private:
    std::string_view input_;
    size_t cursor_ = 0;
};

struct RawItem {
    std::string bytes;
    size_t packet_offset = 0;
    bool occupied = false;
    int32_t runtime_item_id = 0;
    bool has_network_stack_id = false;
    int32_t network_stack_id = 0;
    uint16_t count = 0;
    uint16_t aux = 0;
    size_t network_stack_id_offset = 0;
    size_t network_stack_id_length = 0;
    size_t user_data_offset = 0;
    size_t user_data_length = 0;
};

struct Snapshot {
    bool ready = false;
    uint32_t wire_header = 0;
    uint32_t inventory_id = 0;
    uint64_t revision = 0;
    std::array<RawItem, kPlayerInventorySlotCount> slots{};
    // Exact v859 FullContainerName followed by storage ItemData. This is the
    // context required between the outer InventorySlot fields and its ItemData.
    std::string slot_context;
};

struct Ticket {
    uint64_t id = 0;
    ProjectionPrinterInventoryClientSyncTicketState state =
        ProjectionPrinterInventoryClientSyncTicketState::Pending;
    uint8_t taken_mask = 0;
    uint8_t complete_mask = 0;
    uint8_t source_slot = 0;
    uint8_t destination_slot = 0;
    std::string source_packet;
    std::string destination_packet;
};

// A prepared pair is intentionally separate from the receive FIFO.  The
// printer captures it from the pre-move InventoryContent, submits the native
// Move/Swap, then commits this exact pair immediately afterwards.  Keeping the
// bytes here means a fast server InventoryContent/ItemStackResponse cannot
// invalidate the old pre-image in the small interval between those two calls.
struct PreparedMove {
    uint64_t id = 0;
    uint8_t source_slot = 0;
    uint8_t destination_slot = 0;
    std::string source_packet;
    std::string destination_packet;
};

struct State {
    const void* ingress_connection = nullptr;
    Snapshot snapshot;
    uint64_t next_snapshot_revision = 1U;
    uint64_t next_ticket = 1U;
    uint64_t next_prepared_move = 1U;
    std::deque<ProjectionPrinterInventoryClientSyncQueuedPacket> packets;
    std::deque<Ticket> tickets;
    std::deque<PreparedMove> prepared_moves;
    size_t queued_packet_bytes = 0;
};

std::mutex g_mutex;
State g_state;

bool readPacketHeader(Reader* reader, uint32_t expected_packet_id,
                      uint32_t* wire_header) {
    uint32_t header = 0;
    if (!reader || !reader->varUInt(&header) ||
        (header & kPacketIdMask) != expected_packet_id) {
        return false;
    }
    if (wire_header) *wire_header = header;
    return true;
}

bool readPacketId(std::string_view packet, uint32_t* packet_id) {
    Reader reader(packet);
    uint32_t header = 0;
    if (!packet_id || !reader.varUInt(&header)) return false;
    *packet_id = header & kPacketIdMask;
    return true;
}

bool parseRawItem(Reader* reader, std::string_view packet, RawItem* output) {
    if (!reader || !output) return false;
    RawItem item;
    const size_t start = reader->position();
    int32_t runtime_item_id = 0;
    if (!reader->varInt(&runtime_item_id)) return false;
    if (runtime_item_id != 0) {
        uint16_t count = 0;
        uint32_t aux = 0;
        uint8_t wire_has_network_stack_id = 0;
        int32_t network_stack_id = 0;
        size_t network_stack_id_start = 0;
        size_t network_stack_id_end = 0;
        int32_t ignored_block_runtime_id = 0;
        uint32_t user_data_bytes = 0;
        if (!reader->littleEndian16(&count) || count == 0U ||
            !reader->varUInt(&aux) ||
            aux > std::numeric_limits<uint16_t>::max() ||
            !reader->byte(&wire_has_network_stack_id) ||
            wire_has_network_stack_id > 1U) {
            return false;
        }
        if (wire_has_network_stack_id != 0U) {
            network_stack_id_start = reader->position();
            if (!reader->varInt(&network_stack_id)) return false;
            network_stack_id_end = reader->position();
        }
        if (!reader->varInt(&ignored_block_runtime_id) ||
            !reader->varUInt(&user_data_bytes) ||
            user_data_bytes > kMaximumItemUserDataBytes) {
            return false;
        }
        item.user_data_offset = reader->position() - start;
        item.user_data_length = user_data_bytes;
        if (!reader->skip(user_data_bytes)) return false;
        item.occupied = true;
        item.runtime_item_id = runtime_item_id;
        item.count = count;
        item.aux = static_cast<uint16_t>(aux);
        item.has_network_stack_id = wire_has_network_stack_id != 0U;
        item.network_stack_id = network_stack_id;
        if (item.has_network_stack_id) {
            item.network_stack_id_offset = network_stack_id_start - start;
            item.network_stack_id_length = network_stack_id_end - network_stack_id_start;
        }
    }
    const size_t end = reader->position();
    if (end < start || end > packet.size()) return false;
    item.bytes.assign(packet.data() + start, end - start);
    item.packet_offset = start;
    *output = std::move(item);
    return true;
}

bool parseFullContainerName(Reader* reader) {
    uint8_t ignored_container_id = 0;
    uint8_t has_dynamic_id = 0;
    uint32_t ignored_dynamic_id = 0;
    return reader && reader->byte(&ignored_container_id) && reader->byte(&has_dynamic_id) &&
        has_dynamic_id <= 1U &&
        (has_dynamic_id == 0U || reader->littleEndian32(&ignored_dynamic_id));
}

bool parseInventoryContent(std::string_view packet, Snapshot* output) {
    if (!output || packet.size() > kMaximumInventoryPacketBytes) return false;
    Snapshot parsed;
    Reader reader(packet);
    uint32_t slot_count = 0;
    if (!readPacketHeader(&reader, kInventoryContentPacketId, &parsed.wire_header) ||
        !reader.varUInt(&parsed.inventory_id) || !reader.varUInt(&slot_count) ||
        parsed.inventory_id != kPlayerInventoryNetworkId ||
        slot_count != kPlayerInventorySlotCount) {
        return false;
    }
    for (uint32_t index = 0; index < slot_count; ++index) {
        if (!parseRawItem(&reader, packet, &parsed.slots[index])) return false;
    }
    const size_t context_start = reader.position();
    RawItem ignored_storage_item;
    if (!parseFullContainerName(&reader) ||
        !parseRawItem(&reader, packet, &ignored_storage_item) || !reader.atEnd()) {
        return false;
    }
    parsed.slot_context.assign(packet.data() + context_start,
                               packet.size() - context_start);
    parsed.ready = true;
    *output = std::move(parsed);
    return true;
}

bool parseInventorySlotItemForInventory(
        std::string_view packet, uint32_t expected_inventory_id,
        uint32_t expected_slot, bool optional_context, RawItem* item,
        uint32_t* wire_header = nullptr) {
    if (!item || packet.empty() || packet.size() > kMaximumInventoryPacketBytes) {
        return false;
    }
    Reader reader(packet);
    uint32_t inventory_id = 0;
    uint32_t slot = 0;
    if (!readPacketHeader(&reader, kInventorySlotPacketId, wire_header) ||
        !reader.varUInt(&inventory_id) || inventory_id != expected_inventory_id ||
        !reader.varUInt(&slot) || slot != expected_slot) return false;
    RawItem ignored_storage_item;
    if (optional_context) {
        uint8_t has_container = 0;
        uint8_t has_storage = 0;
        if (!reader.byte(&has_container) || has_container > 1U ||
            (has_container != 0U && !parseFullContainerName(&reader)) ||
            !reader.byte(&has_storage) || has_storage > 1U ||
            (has_storage != 0U &&
             !parseRawItem(&reader, packet, &ignored_storage_item))) return false;
    } else if (!parseFullContainerName(&reader) ||
               !parseRawItem(&reader, packet, &ignored_storage_item)) {
        return false;
    }
    return parseRawItem(&reader, packet, item) && reader.atEnd();
}

bool parseInventorySlotItem(std::string_view packet, uint8_t expected_slot,
                            bool optional_context, RawItem* item) {
    return parseInventorySlotItemForInventory(
        packet, kPlayerInventoryNetworkId, expected_slot, optional_context,
        item);
}

bool parseNativeInventorySlotItem(std::string_view packet, uint8_t expected_slot,
                                  RawItem* item) {
    // The verified writer emits the v859 context. Also accept the optional
    // context variant already supported by the passive inventory mailbox.
    return parseInventorySlotItem(packet, expected_slot, false, item) ||
        parseInventorySlotItem(packet, expected_slot, true, item);
}

void appendVarUInt(std::string* output, uint32_t value) {
    if (!output) return;
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7FU);
        value >>= 7U;
        if (value != 0U) byte |= 0x80U;
        output->push_back(static_cast<char>(byte));
    } while (value != 0U);
}

void appendVarInt(std::string* output, int32_t value) {
    const uint32_t sign_mask = value < 0 ? std::numeric_limits<uint32_t>::max() : 0U;
    const uint32_t zig_zag = (static_cast<uint32_t>(value) << 1U) ^ sign_mask;
    appendVarUInt(output, zig_zag);
}

bool replaceCapturedNetworkStackId(const RawItem& captured, int32_t confirmed_network_id,
                                   RawItem* output) {
    if (!output) return false;
    if (!captured.occupied) {
        // A full Move leaves the source as the original air ItemData. It has
        // no ItemStackNetId field and must be reused without synthesizing one.
        *output = captured;
        return true;
    }
    if (!captured.has_network_stack_id || captured.network_stack_id_length == 0U ||
        captured.network_stack_id_offset > captured.bytes.size() ||
        captured.network_stack_id_length >
            captured.bytes.size() - captured.network_stack_id_offset) {
        return false;
    }
    RawItem replaced = captured;
    std::string encoded_network_id;
    appendVarInt(&encoded_network_id, confirmed_network_id);
    replaced.bytes.replace(replaced.network_stack_id_offset,
                           replaced.network_stack_id_length, encoded_network_id);
    replaced.network_stack_id = confirmed_network_id;
    replaced.network_stack_id_length = encoded_network_id.size();
    *output = std::move(replaced);
    return true;
}

bool buildInventorySlotPacket(const Snapshot& snapshot, uint8_t slot,
                              const RawItem& item, std::string* output) {
    if (!output || !snapshot.ready || snapshot.inventory_id != kPlayerInventoryNetworkId ||
        slot >= kPlayerInventorySlotCount || item.bytes.empty()) {
        return false;
    }
    std::string packet;
    const uint32_t slot_header = (snapshot.wire_header & ~kPacketIdMask) |
        kInventorySlotPacketId;
    appendVarUInt(&packet, slot_header);
    appendVarUInt(&packet, snapshot.inventory_id);
    appendVarUInt(&packet, slot);
    packet.append(snapshot.slot_context);
    packet.append(item.bytes);
    if (packet.size() > kMaximumInventoryPacketBytes) return false;
    *output = std::move(packet);
    return true;
}

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

Ticket* findTicketLocked(uint64_t ticket) {
    for (Ticket& candidate : g_state.tickets) {
        if (candidate.id == ticket) return &candidate;
    }
    return nullptr;
}

const Ticket* findTicketConstLocked(uint64_t ticket) {
    for (const Ticket& candidate : g_state.tickets) {
        if (candidate.id == ticket) return &candidate;
    }
    return nullptr;
}

void pruneTicketHistoryLocked() {
    while (g_state.tickets.size() > kMaximumTicketHistory) {
        const auto iterator = std::find_if(
            g_state.tickets.begin(), g_state.tickets.end(),
            [](const Ticket& ticket) {
                return ticket.state != ProjectionPrinterInventoryClientSyncTicketState::Pending;
            });
        if (iterator == g_state.tickets.end()) return;
        g_state.tickets.erase(iterator);
    }
}

void cancelTicketLocked(uint64_t ticket) {
    Ticket* record = findTicketLocked(ticket);
    if (!record || record->state != ProjectionPrinterInventoryClientSyncTicketState::Pending) {
        return;
    }
    record->state = ProjectionPrinterInventoryClientSyncTicketState::Cancelled;
    std::string{}.swap(record->source_packet);
    std::string{}.swap(record->destination_packet);
    for (auto iterator = g_state.packets.begin(); iterator != g_state.packets.end();) {
        if (iterator->ticket != ticket) {
            ++iterator;
            continue;
        }
        if (iterator->bytes.size() <= g_state.queued_packet_bytes) {
            g_state.queued_packet_bytes -= iterator->bytes.size();
        } else {
            g_state.queued_packet_bytes = 0U;
        }
        iterator = g_state.packets.erase(iterator);
    }
}

void clearLocked() {
    g_state.ingress_connection = nullptr;
    g_state.snapshot = {};
    g_state.packets.clear();
    g_state.prepared_moves.clear();
    g_state.queued_packet_bytes = 0U;
    for (Ticket& ticket : g_state.tickets) {
        std::string{}.swap(ticket.source_packet);
        std::string{}.swap(ticket.destination_packet);
        if (ticket.state == ProjectionPrinterInventoryClientSyncTicketState::Pending) {
            ticket.state = ProjectionPrinterInventoryClientSyncTicketState::Cancelled;
        }
    }
    pruneTicketHistoryLocked();
}

uint64_t takeNextNonzero(uint64_t* counter) {
    if (!counter) return 0U;
    uint64_t result = *counter;
    ++(*counter);
    if (*counter == 0U) *counter = 1U;
    if (result == 0U) {
        result = *counter;
        ++(*counter);
        if (*counter == 0U) *counter = 1U;
    }
    return result;
}

bool buildMovePacketsLocked(const ProjectionPrinterInventoryClientSyncMove& move,
                            PreparedMove* prepared, std::string* error) {
    if (!prepared) return fail(error, "client refresh prepared output is null");
    const Snapshot& snapshot = g_state.snapshot;
    if (!snapshot.ready) return fail(error, "player InventoryContent is not ready");
    if (move.snapshot_revision == 0U || move.snapshot_revision != snapshot.revision) {
        return fail(error, "player InventoryContent changed before client refresh");
    }
    if (move.source_inventory_slot < 9U ||
        move.source_inventory_slot >= kPlayerInventorySlotCount ||
        move.destination_hotbar_slot >= 9U) {
        return fail(error, "client refresh slot is outside backpack-to-hotbar range");
    }
    const RawItem& source = snapshot.slots[move.source_inventory_slot];
    const RawItem& destination = snapshot.slots[move.destination_hotbar_slot];
    if (!source.occupied || !source.has_network_stack_id ||
        source.network_stack_id != move.expected_source_network_stack_id) {
        return fail(error, "client refresh source no longer matches expected stack id");
    }
    if (destination.occupied != move.expected_destination_occupied) {
        return fail(error, "client refresh destination occupancy changed");
    }
    if (destination.occupied &&
        (!destination.has_network_stack_id ||
         destination.network_stack_id != move.expected_destination_network_stack_id)) {
        return fail(error, "client refresh destination no longer matches expected stack id");
    }
    if (!destination.occupied && move.expected_destination_network_stack_id != 0) {
        return fail(error, "client refresh empty destination has a stack id");
    }

    // Move source is the captured destination ItemData (usually air), while
    // the hotbar destination receives the captured source ItemData. The caller
    // may deliberately pass the pre-move IDs here for the reference sorter
    // sequence, or response IDs when it needs an optional later correction.
    RawItem source_after;
    RawItem destination_after;
    if (!replaceCapturedNetworkStackId(destination,
                                       move.confirmed_source_network_stack_id,
                                       &source_after) ||
        !replaceCapturedNetworkStackId(source,
                                       move.confirmed_destination_network_stack_id,
                                       &destination_after)) {
        return fail(error, "captured ItemData cannot accept requested stack id");
    }
    std::string source_packet;
    std::string destination_packet;
    if (!buildInventorySlotPacket(snapshot, move.source_inventory_slot, source_after,
                                  &source_packet) ||
        !buildInventorySlotPacket(snapshot, move.destination_hotbar_slot,
                                  destination_after, &destination_packet)) {
        return fail(error, "could not build v859 client InventorySlot refresh");
    }
    if (source_packet.size() > kMaximumQueuedPacketBytes ||
        destination_packet.size() > kMaximumQueuedPacketBytes ||
        source_packet.size() > kMaximumQueuedPacketBytes - destination_packet.size()) {
        return fail(error, "client refresh packet exceeds byte limit");
    }
    prepared->id = 0U;
    prepared->source_slot = move.source_inventory_slot;
    prepared->destination_slot = move.destination_hotbar_slot;
    prepared->source_packet = std::move(source_packet);
    prepared->destination_packet = std::move(destination_packet);
    return true;
}

bool enqueuePreparedMoveLocked(PreparedMove&& prepared, uint64_t* ticket,
                               std::string* error) {
    if (ticket) *ticket = 0U;
    if (prepared.source_packet.empty() || prepared.destination_packet.empty()) {
        return fail(error, "prepared client refresh is empty");
    }
    if (g_state.packets.size() > kMaximumQueuedPackets - 2U) {
        return fail(error, "client refresh queue is full");
    }
    const size_t source_size = prepared.source_packet.size();
    const size_t destination_size = prepared.destination_packet.size();
    if (source_size > kMaximumQueuedPacketBytes ||
        destination_size > kMaximumQueuedPacketBytes ||
        source_size > kMaximumQueuedPacketBytes - destination_size ||
        g_state.queued_packet_bytes >
            kMaximumQueuedPacketBytes - source_size - destination_size) {
        return fail(error, "client refresh queue byte limit reached");
    }

    const uint64_t assigned_ticket = takeNextNonzero(&g_state.next_ticket);
    Ticket record;
    record.id = assigned_ticket;
    record.source_slot = prepared.source_slot;
    record.destination_slot = prepared.destination_slot;
    record.source_packet = prepared.source_packet;
    record.destination_packet = prepared.destination_packet;
    // Only one printer material move is active. Retain bytes just for the
    // newest pair so response correction has a pre-image without keeping up
    // to 64 large NBT payload pairs alive in the ticket status history.
    for (Ticket& older : g_state.tickets) {
        std::string{}.swap(older.source_packet);
        std::string{}.swap(older.destination_packet);
    }
    g_state.tickets.push_back(std::move(record));
    ProjectionPrinterInventoryClientSyncQueuedPacket source_update;
    source_update.ticket = assigned_ticket;
    source_update.packet_index = 0U;
    source_update.bytes = std::move(prepared.source_packet);
    ProjectionPrinterInventoryClientSyncQueuedPacket destination_update;
    destination_update.ticket = assigned_ticket;
    destination_update.packet_index = 1U;
    destination_update.bytes = std::move(prepared.destination_packet);
    g_state.queued_packet_bytes += source_update.bytes.size() + destination_update.bytes.size();
    g_state.packets.push_back(std::move(source_update));
    g_state.packets.push_back(std::move(destination_update));
    pruneTicketHistoryLocked();
    if (ticket) *ticket = assigned_ticket;
    return true;
}

auto findPreparedMoveLocked(uint64_t id) {
    return std::find_if(g_state.prepared_moves.begin(), g_state.prepared_moves.end(),
                        [id](const PreparedMove& candidate) {
                            return candidate.id == id;
                        });
}

}  // namespace

bool DecodeProjectionPrinterNativeInventorySlotItem(
    std::string_view packet, uint8_t expected_slot,
    ProjectionPrinterNativeSlotItem* output) noexcept {
    if (!output) return false;
    *output = {};
    try {
        RawItem parsed;
        if (!parseNativeInventorySlotItem(packet, expected_slot, &parsed)) return false;
        output->occupied = parsed.occupied;
        output->runtime_item_id = parsed.runtime_item_id;
        output->count = parsed.count;
        output->aux = parsed.aux;
        output->has_network_stack_id = parsed.has_network_stack_id;
        output->network_stack_id = parsed.network_stack_id;
        return true;
    } catch (...) {
        return false;
    }
}

bool DecodeProjectionPrinterNativeInventorySlotMapMetadata(
    std::string_view packet, uint8_t expected_slot,
    ProjectionPrinterNativeSlotItem* item,
    ItemExtraMapMetadata* metadata) noexcept {
    if (item) *item = {};
    if (metadata) *metadata = {};
    if (!item || !metadata) return false;
    try {
        RawItem parsed;
        if (!parseNativeInventorySlotItem(packet, expected_slot, &parsed) ||
            !parsed.occupied || parsed.runtime_item_id <= 0 || parsed.count == 0U ||
            !parsed.has_network_stack_id || parsed.network_stack_id <= 0 ||
            parsed.user_data_length == 0U ||
            parsed.user_data_offset > parsed.bytes.size() ||
            parsed.user_data_length > parsed.bytes.size() - parsed.user_data_offset) {
            return false;
        }
        ItemExtraMapMetadata parsed_metadata;
        if (!ParseItemExtraMapMetadata(
                std::string_view(parsed.bytes).substr(parsed.user_data_offset,
                                                       parsed.user_data_length),
                &parsed_metadata) || !parsed_metadata.has_map_uuid) return false;
        item->occupied = true;
        item->runtime_item_id = parsed.runtime_item_id;
        item->count = parsed.count;
        item->aux = parsed.aux;
        item->has_network_stack_id = true;
        item->network_stack_id = parsed.network_stack_id;
        *metadata = std::move(parsed_metadata);
        return true;
    } catch (...) {
        *item = {};
        *metadata = {};
        return false;
    }
}

bool DecodeProjectionPrinterNativeInventorySlotPreviewMapMetadata(
    std::string_view packet, uint8_t expected_slot,
    ProjectionPrinterNativeSlotItem* item,
    ItemExtraMapMetadata* metadata) noexcept {
    if (item) *item = {};
    if (metadata) *metadata = {};
    if (!item || !metadata) return false;
    try {
        RawItem parsed;
        if (!parseNativeInventorySlotItem(packet, expected_slot, &parsed) ||
            !parsed.occupied || parsed.runtime_item_id <= 0 ||
            parsed.count == 0U ||
            (parsed.has_network_stack_id && parsed.network_stack_id < 0) ||
            parsed.user_data_length == 0U ||
            parsed.user_data_offset > parsed.bytes.size() ||
            parsed.user_data_length > parsed.bytes.size() - parsed.user_data_offset) {
            return false;
        }
        ItemExtraMapMetadata parsed_metadata;
        if (!ParseItemExtraMapMetadata(
                std::string_view(parsed.bytes).substr(parsed.user_data_offset,
                                                       parsed.user_data_length),
                &parsed_metadata) || !parsed_metadata.has_map_uuid) return false;
        item->occupied = true;
        item->runtime_item_id = parsed.runtime_item_id;
        item->count = parsed.count;
        item->aux = parsed.aux;
        // Do not report a wire net ID for a preview ItemInstance. It has no
        // ItemStack network identity even if the serializer supplied one in
        // a temporary NetworkItemStackDescriptor.
        item->has_network_stack_id = false;
        item->network_stack_id = 0;
        *metadata = std::move(parsed_metadata);
        return true;
    } catch (...) {
        *item = {};
        *metadata = {};
        return false;
    }
}

bool DecodeProjectionPrinterNativeMapSlotPacket(
    std::string_view packet, uint32_t expected_inventory_id,
    uint32_t expected_slot,
    ProjectionPrinterNativeMapSlotPacket* output) noexcept {
    if (!output) return false;
    *output = {};
    try {
        // The game writer used by the anvil capture emits the complete v859
        // FullContainerName/storage tail, not the optional legacy variant.
        RawItem parsed;
        uint32_t wire_header = 0;
        if (!parseInventorySlotItemForInventory(
                packet, expected_inventory_id, expected_slot, false,
                &parsed, &wire_header) ||
            !parsed.occupied || parsed.runtime_item_id <= 0 ||
            parsed.count == 0U || !parsed.has_network_stack_id ||
            parsed.network_stack_id <= 0 || parsed.user_data_length == 0U ||
            parsed.user_data_offset > parsed.bytes.size() ||
            parsed.user_data_length >
                parsed.bytes.size() - parsed.user_data_offset ||
            parsed.network_stack_id_length == 0U ||
            parsed.network_stack_id_offset > parsed.bytes.size() ||
            parsed.network_stack_id_length >
                parsed.bytes.size() - parsed.network_stack_id_offset) {
            return false;
        }
        ItemExtraMapMetadata metadata;
        if (!ParseItemExtraMapMetadata(
                std::string_view(parsed.bytes).substr(parsed.user_data_offset,
                                                     parsed.user_data_length),
                &metadata) || !metadata.has_map_uuid) return false;

        ProjectionPrinterNativeMapSlotPacket decoded;
        decoded.wire_header = wire_header;
        decoded.inventory_id = expected_inventory_id;
        decoded.slot = expected_slot;
        decoded.item.occupied = true;
        decoded.item.runtime_item_id = parsed.runtime_item_id;
        decoded.item.count = parsed.count;
        decoded.item.aux = parsed.aux;
        decoded.item.has_network_stack_id = true;
        decoded.item.network_stack_id = parsed.network_stack_id;
        decoded.metadata = std::move(metadata);
        decoded.item_data_offset = parsed.packet_offset;
        decoded.item_data_length = parsed.bytes.size();
        decoded.network_stack_id_offset = parsed.packet_offset +
            parsed.network_stack_id_offset;
        decoded.network_stack_id_length = parsed.network_stack_id_length;
        *output = std::move(decoded);
        return true;
    } catch (...) {
        *output = {};
        return false;
    }
}

void ObserveProjectionPrinterInventoryClientSyncPacket(std::string_view packet) noexcept {
    try {
        if (packet.empty()) return;
        uint32_t packet_id = 0;
        if (!readPacketId(packet, &packet_id)) return;
        if (packet_id == kDisconnectPacketId || packet_id == kStartGamePacketId ||
            packet_id == kChangeDimensionPacketId) {
            ClearProjectionPrinterInventoryClientSync();
            return;
        }
        if (packet_id != kInventoryContentPacketId) return;
        Snapshot parsed;
        if (!parseInventoryContent(packet, &parsed)) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        parsed.revision = takeNextNonzero(&g_state.next_snapshot_revision);
        g_state.snapshot = std::move(parsed);
    } catch (...) {
        // This runs in a networking hook. Malformed content must preserve the
        // last complete snapshot and must never unwind into the game client.
    }
}

void ClearProjectionPrinterInventoryClientSync() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        clearLocked();
    } catch (...) {
    }
}

bool GetProjectionPrinterInventoryClientSyncSnapshotInfo(
    ProjectionPrinterInventoryClientSyncSnapshotInfo* output) noexcept {
    try {
        if (!output) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        output->ready = g_state.snapshot.ready;
        output->inventory_id = g_state.snapshot.inventory_id;
        output->revision = g_state.snapshot.revision;
        return output->ready;
    } catch (...) {
        if (output) *output = {};
        return false;
    }
}

bool QueueProjectionPrinterInventoryClientSyncMove(
    const ProjectionPrinterInventoryClientSyncMove& move, uint64_t* ticket,
    std::string* error) {
    if (ticket) *ticket = 0U;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        PreparedMove prepared;
        return buildMovePacketsLocked(move, &prepared, error) &&
            enqueuePreparedMoveLocked(std::move(prepared), ticket, error);
    } catch (...) {
        return fail(error, "client refresh allocation failed");
    }
}

bool PrepareProjectionPrinterInventoryClientSyncMove(
    const ProjectionPrinterInventoryClientSyncMove& move,
    ProjectionPrinterInventoryClientSyncPreparedMove* prepared,
    std::string* error) {
    if (prepared) *prepared = {};
    try {
        if (!prepared) return fail(error, "client refresh prepared handle is null");
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state.prepared_moves.size() >= kMaximumQueuedPackets / 2U) {
            return fail(error, "client refresh prepared queue is full");
        }
        PreparedMove entry;
        if (!buildMovePacketsLocked(move, &entry, error)) return false;
        entry.id = takeNextNonzero(&g_state.next_prepared_move);
        prepared->id = entry.id;
        g_state.prepared_moves.push_back(std::move(entry));
        return true;
    } catch (...) {
        return fail(error, "client refresh prepare allocation failed");
    }
}

bool PrepareProjectionPrinterInventoryClientSyncNativeMove(
    const ProjectionPrinterInventoryClientSyncMove& move,
    std::string source_packet, std::string destination_packet,
    ProjectionPrinterInventoryClientSyncPreparedMove* prepared,
    std::string* error) {
    if (prepared) *prepared = {};
    try {
        if (!prepared || move.source_inventory_slot < 9U ||
            move.source_inventory_slot >= kPlayerInventorySlotCount ||
            move.destination_hotbar_slot >= 9U || move.expected_source_count == 0U ||
            move.expected_source_network_stack_id <= 0) {
            return fail(error, "native client refresh request is invalid");
        }
        RawItem source_after;
        RawItem destination_after;
        if (!parseNativeInventorySlotItem(source_packet, move.source_inventory_slot,
                                           &source_after) ||
            !parseNativeInventorySlotItem(destination_packet,
                                           move.destination_hotbar_slot,
                                           &destination_after)) {
            return fail(error, "native client InventorySlot serialization is invalid");
        }
        if (!destination_after.occupied || !destination_after.has_network_stack_id ||
            destination_after.network_stack_id != move.expected_source_network_stack_id ||
            destination_after.count != move.expected_source_count) {
            return fail(error, "native client refresh source stack or count changed");
        }
        if (source_after.occupied != move.expected_destination_occupied ||
            (source_after.occupied && (!source_after.has_network_stack_id ||
                source_after.network_stack_id != move.expected_destination_network_stack_id)) ||
            (!source_after.occupied && move.expected_destination_network_stack_id != 0)) {
            return fail(error, "native client refresh destination stack changed");
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state.prepared_moves.size() >= kMaximumQueuedPackets / 2U) {
            return fail(error, "client refresh prepared queue is full");
        }
        PreparedMove entry;
        entry.id = takeNextNonzero(&g_state.next_prepared_move);
        entry.source_slot = move.source_inventory_slot;
        entry.destination_slot = move.destination_hotbar_slot;
        entry.source_packet = std::move(source_packet);
        entry.destination_packet = std::move(destination_packet);
        const uint64_t assigned_id = entry.id;
        g_state.prepared_moves.push_back(std::move(entry));
        prepared->id = assigned_id;
        if (error) error->clear();
        return true;
    } catch (...) {
        return fail(error, "native client refresh preparation failed");
    }
}

bool CommitProjectionPrinterInventoryClientSyncPreparedMove(
    ProjectionPrinterInventoryClientSyncPreparedMove* prepared, uint64_t* ticket,
    std::string* error) {
    if (ticket) *ticket = 0U;
    try {
        if (!prepared || prepared->id == 0U) {
            return fail(error, "client refresh prepared handle is invalid");
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto iterator = findPreparedMoveLocked(prepared->id);
        prepared->id = 0U;
        if (iterator == g_state.prepared_moves.end()) {
            return fail(error, "client refresh prepared handle has expired");
        }
        PreparedMove entry = std::move(*iterator);
        g_state.prepared_moves.erase(iterator);
        return enqueuePreparedMoveLocked(std::move(entry), ticket, error);
    } catch (...) {
        if (prepared) prepared->id = 0U;
        return fail(error, "client refresh commit allocation failed");
    }
}

bool QueueProjectionPrinterInventoryClientSyncResponseCorrection(
    uint64_t original_ticket, int32_t confirmed_source_network_id,
    int32_t confirmed_destination_network_id, uint64_t* correction_ticket,
    std::string* error) {
    if (correction_ticket) *correction_ticket = 0U;
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        const Ticket* original = findTicketConstLocked(original_ticket);
        if (!original || original->state !=
                ProjectionPrinterInventoryClientSyncTicketState::Complete ||
            original->source_packet.empty() || original->destination_packet.empty()) {
            return fail(error, "completed client refresh pre-image is unavailable");
        }
        RawItem source;
        RawItem destination;
        if (!parseNativeInventorySlotItem(original->source_packet, original->source_slot,
                                           &source) ||
            !parseNativeInventorySlotItem(original->destination_packet,
                                           original->destination_slot, &destination) ||
            !destination.occupied || confirmed_destination_network_id <= 0 ||
            (source.occupied ? confirmed_source_network_id <= 0 :
                               confirmed_source_network_id != 0)) {
            return fail(error, "response correction does not match the moved stacks");
        }
        RawItem source_corrected;
        RawItem destination_corrected;
        if (!replaceCapturedNetworkStackId(source, confirmed_source_network_id,
                                            &source_corrected) ||
            !replaceCapturedNetworkStackId(destination, confirmed_destination_network_id,
                                            &destination_corrected)) {
            return fail(error, "response correction could not replace stack IDs");
        }
        PreparedMove correction;
        correction.source_slot = original->source_slot;
        correction.destination_slot = original->destination_slot;
        correction.source_packet = original->source_packet;
        correction.destination_packet = original->destination_packet;
        correction.source_packet.replace(source.packet_offset, source.bytes.size(),
                                           source_corrected.bytes);
        correction.destination_packet.replace(destination.packet_offset,
                                                destination.bytes.size(),
                                                destination_corrected.bytes);
        return enqueuePreparedMoveLocked(std::move(correction), correction_ticket, error);
    } catch (...) {
        return fail(error, "response correction allocation failed");
    }
}

void DiscardProjectionPrinterInventoryClientSyncPreparedMove(
    ProjectionPrinterInventoryClientSyncPreparedMove* prepared) noexcept {
    try {
        if (!prepared || prepared->id == 0U) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto iterator = findPreparedMoveLocked(prepared->id);
        if (iterator != g_state.prepared_moves.end()) {
            g_state.prepared_moves.erase(iterator);
        }
        prepared->id = 0U;
    } catch (...) {
        if (prepared) prepared->id = 0U;
    }
}

void BindProjectionPrinterInventoryClientSyncIngress(const void* connection) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!connection || (g_state.ingress_connection &&
                            g_state.ingress_connection != connection)) {
            clearLocked();
        }
        g_state.ingress_connection = connection;
    } catch (...) {
        // Never unwind through the native network receiver.
    }
}

bool TakeProjectionPrinterInventoryClientSyncPacket(
    const void* connection, ProjectionPrinterInventoryClientSyncQueuedPacket* output) {
    if (!connection || !output) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state.ingress_connection != connection) return false;
    if (g_state.packets.empty()) return false;
    const ProjectionPrinterInventoryClientSyncQueuedPacket& front = g_state.packets.front();
    Ticket* record = findTicketLocked(front.ticket);
    if (!record || record->state != ProjectionPrinterInventoryClientSyncTicketState::Pending ||
        front.packet_index > 1U) {
        return false;
    }
    *output = front;
    record->taken_mask |= static_cast<uint8_t>(1U << front.packet_index);
    if (front.bytes.size() <= g_state.queued_packet_bytes) {
        g_state.queued_packet_bytes -= front.bytes.size();
    } else {
        g_state.queued_packet_bytes = 0U;
    }
    g_state.packets.pop_front();
    return true;
}

bool CompleteProjectionPrinterInventoryClientSyncPacket(uint64_t ticket,
                                                         uint8_t packet_index) noexcept {
    try {
        if (ticket == 0U || packet_index > 1U) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        Ticket* record = findTicketLocked(ticket);
        if (!record || record->state != ProjectionPrinterInventoryClientSyncTicketState::Pending) {
            return false;
        }
        const uint8_t mask = static_cast<uint8_t>(1U << packet_index);
        if ((record->taken_mask & mask) == 0U) return false;
        record->complete_mask |= mask;
        if (record->complete_mask == 0x03U) {
            record->state = ProjectionPrinterInventoryClientSyncTicketState::Complete;
            pruneTicketHistoryLocked();
        }
        return true;
    } catch (...) {
        return false;
    }
}

void CancelProjectionPrinterInventoryClientSyncTicket(uint64_t ticket) noexcept {
    try {
        if (ticket == 0U) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        cancelTicketLocked(ticket);
        pruneTicketHistoryLocked();
    } catch (...) {
    }
}

ProjectionPrinterInventoryClientSyncTicketState
GetProjectionPrinterInventoryClientSyncTicketState(uint64_t ticket) noexcept {
    try {
        if (ticket == 0U) return ProjectionPrinterInventoryClientSyncTicketState::Unknown;
        std::lock_guard<std::mutex> lock(g_mutex);
        const Ticket* record = findTicketConstLocked(ticket);
        return record ? record->state : ProjectionPrinterInventoryClientSyncTicketState::Unknown;
    } catch (...) {
        return ProjectionPrinterInventoryClientSyncTicketState::Unknown;
    }
}

}  // namespace build_import
