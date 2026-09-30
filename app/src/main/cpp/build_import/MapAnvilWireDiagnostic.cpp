#include "MapAnvilWireDiagnostic.h"

#include <cstddef>
#include <cstring>
#include <limits>

namespace build_import {
namespace {

constexpr uint32_t kOpenId = 0x2EU;
constexpr uint32_t kCloseId = 0x2FU;
constexpr uint32_t kContentId = 0x31U;
constexpr uint32_t kSlotId = 0x32U;
constexpr size_t kMaximumPacketBytes = 10U * 1024U * 1024U;
constexpr uint32_t kMaximumSlots = 256U;
constexpr auto kLifetime = std::chrono::seconds(90);
constexpr uint8_t kMaximumEvents = 32U;

class Reader final {
public:
    explicit Reader(std::string_view input) : input_(input) {}

    bool byte(uint8_t* output) {
        if (!output || cursor_ >= input_.size()) return false;
        *output = static_cast<uint8_t>(input_[cursor_++]);
        return true;
    }

    bool le16(uint16_t* output) {
        if (!output || input_.size() - cursor_ < 2U) return false;
        const auto* data = reinterpret_cast<const uint8_t*>(input_.data() + cursor_);
        *output = static_cast<uint16_t>(data[0]) |
                  static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8U);
        cursor_ += 2U;
        return true;
    }

    bool le32(uint32_t* output) {
        if (!output || input_.size() - cursor_ < 4U) return false;
        uint32_t value = 0;
        for (uint32_t index = 0; index < 4U; ++index) {
            value |= static_cast<uint32_t>(
                static_cast<uint8_t>(input_[cursor_ + index])) << (8U * index);
        }
        cursor_ += 4U;
        *output = value;
        return true;
    }

    bool varUInt(uint32_t* output) {
        if (!output) return false;
        uint32_t value = 0;
        for (uint32_t index = 0; index < 5U && cursor_ < input_.size(); ++index) {
            const uint8_t next = static_cast<uint8_t>(input_[cursor_++]);
            if (index == 4U && (next & 0xF0U) != 0U) return false;
            value |= static_cast<uint32_t>(next & 0x7FU) << (7U * index);
            if ((next & 0x80U) == 0U) {
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

    bool varInt64(int64_t* output) {
        if (!output) return false;
        uint64_t encoded = 0;
        for (uint32_t index = 0; index < 10U && cursor_ < input_.size(); ++index) {
            const uint8_t next = static_cast<uint8_t>(input_[cursor_++]);
            if (index == 9U && (next & 0xFEU) != 0U) return false;
            encoded |= static_cast<uint64_t>(next & 0x7FU) << (7U * index);
            if ((next & 0x80U) == 0U) {
                const uint64_t decoded = (encoded >> 1U) ^
                    (0ULL - (encoded & 1ULL));
                std::memcpy(output, &decoded, sizeof(decoded));
                return true;
            }
        }
        return false;
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

bool header(Reader* reader, uint32_t packet_id) {
    uint32_t wire_header = 0;
    return reader && reader->varUInt(&wire_header) &&
           (wire_header & 0x3FFU) == packet_id;
}

struct Open {
    uint8_t window = 0;
    uint8_t type = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

bool parseOpen(std::string_view packet, Open* output) {
    if (!output) return false;
    Reader reader(packet);
    uint32_t y = 0;
    int64_t ignored_entity = 0;
    if (!header(&reader, kOpenId) || !reader.byte(&output->window) ||
        !reader.byte(&output->type) || !reader.varInt(&output->x) ||
        !reader.varUInt(&y) || y > INT32_MAX ||
        !reader.varInt(&output->z) || !reader.varInt64(&ignored_entity)) return false;
    output->y = static_cast<int32_t>(y);
    return true;  // protocol extensions after the entity ID are tolerated
}

bool parseClose(std::string_view packet, uint8_t* window, uint8_t* type) {
    Reader reader(packet);
    uint8_t server_initiated = 0;
    return header(&reader, kCloseId) && reader.byte(window) &&
           reader.byte(type) && reader.byte(&server_initiated) &&
           server_initiated <= 1U;
}

bool parseItem(Reader* reader, uint16_t slot, int64_t expected_map_uuid,
               MapAnvilDiagnosticMapItem* output, bool* matching) {
    if (!reader || !output || !matching) return false;
    *output = {};
    *matching = false;
    int32_t runtime_id = 0;
    if (!reader->varInt(&runtime_id)) return false;
    if (runtime_id == 0) return true;
    uint16_t count = 0;
    uint32_t aux = 0;
    uint8_t has_network_id = 0;
    int32_t network_id = 0;
    int32_t ignored_block_id = 0;
    uint32_t extra_length = 0;
    std::string_view extra;
    if (!reader->le16(&count) || count == 0U ||
        !reader->varUInt(&aux) || aux > UINT16_MAX ||
        !reader->byte(&has_network_id) || has_network_id > 1U ||
        (has_network_id && !reader->varInt(&network_id)) ||
        !reader->varInt(&ignored_block_id) ||
        !reader->varUInt(&extra_length) || extra_length > 5U * 1024U * 1024U ||
        !reader->take(extra_length, &extra)) return false;
    ItemExtraMapMetadata metadata;
    if (!ParseItemExtraMapMetadata(extra, &metadata) ||
        !metadata.has_map_uuid ||
        (expected_map_uuid != -1 && metadata.map_uuid != expected_map_uuid)) return true;
    output->slot = slot;
    output->runtime_item_id = runtime_id;
    output->count = count;
    output->has_network_stack_id = has_network_id != 0U;
    output->network_stack_id = network_id;
    output->map_uuid = metadata.map_uuid;
    output->name_status = metadata.name_status;
    output->name_source = metadata.name_source;
    output->name = std::move(metadata.display_name);
    *matching = true;
    return true;
}

bool parseContent(std::string_view packet, uint32_t inventory_id,
                  int64_t expected_uuid, MapAnvilDiagnosticEvent* event) {
    if (!event || packet.size() > kMaximumPacketBytes) return false;
    Reader reader(packet);
    uint32_t received_id = 0;
    uint32_t count = 0;
    if (!header(&reader, kContentId) || !reader.varUInt(&received_id) ||
        received_id != inventory_id || !reader.varUInt(&count) ||
        count > kMaximumSlots) return false;
    event->inventory_id = received_id;
    event->slot_count = count;
    for (uint32_t slot = 0; slot < count; ++slot) {
        MapAnvilDiagnosticMapItem item;
        bool matching = false;
        if (!parseItem(&reader, static_cast<uint16_t>(slot), expected_uuid,
                       &item, &matching)) return false;
        if (matching && event->matching_map_count < event->matching_maps.size()) {
            event->matching_maps[event->matching_map_count++] = std::move(item);
        }
    }
    // Post-v748 InventoryContent can append FullContainerName, optional
    // dynamic ID and a storage item. Decode only if the entire known tail is
    // well-formed; older/extended tails leave this evidence unavailable.
    if (!reader.atEnd()) {
        uint8_t name = 0;
        uint8_t has_dynamic = 0;
        uint32_t dynamic_id = 0;
        MapAnvilDiagnosticMapItem ignored_storage;
        bool ignored_matching = false;
        if (reader.byte(&name) && reader.byte(&has_dynamic) &&
            has_dynamic <= 1U &&
            (!has_dynamic || reader.le32(&dynamic_id)) &&
            parseItem(&reader, 0U, expected_uuid,
                      &ignored_storage, &ignored_matching) && reader.atEnd()) {
            event->has_full_container_name = true;
            event->full_container_name = name;
            event->has_dynamic_container_id = has_dynamic != 0U;
            event->dynamic_container_id = dynamic_id;
        }
    }
    return true;
}

bool parseFullContainerName(Reader* reader) {
    uint8_t ignored_name = 0;
    uint8_t has_dynamic = 0;
    uint32_t ignored_dynamic_id = 0;
    return reader && reader->byte(&ignored_name) &&
           reader->byte(&has_dynamic) && has_dynamic <= 1U &&
           (!has_dynamic || reader->le32(&ignored_dynamic_id));
}

bool parseSlotVariant(std::string_view packet, bool optional_context,
                      uint32_t inventory_id, int64_t expected_uuid,
                      MapAnvilDiagnosticEvent* event) {
    Reader reader(packet);
    uint32_t received_id = 0;
    uint32_t slot = 0;
    if (!header(&reader, kSlotId) || !reader.varUInt(&received_id) ||
        received_id != inventory_id || !reader.varUInt(&slot) ||
        slot >= kMaximumSlots) return false;
    MapAnvilDiagnosticMapItem ignored_storage;
    bool ignored_matching = false;
    if (optional_context) {
        uint8_t has_name = 0;
        uint8_t has_storage = 0;
        if (!reader.byte(&has_name) || has_name > 1U ||
            (has_name && !parseFullContainerName(&reader)) ||
            !reader.byte(&has_storage) || has_storage > 1U ||
            (has_storage && !parseItem(&reader, 0U, expected_uuid,
                                       &ignored_storage, &ignored_matching))) return false;
    } else if (!parseFullContainerName(&reader) ||
               !parseItem(&reader, 0U, expected_uuid,
                          &ignored_storage, &ignored_matching)) {
        return false;
    }
    MapAnvilDiagnosticMapItem item;
    bool matching = false;
    if (!parseItem(&reader, static_cast<uint16_t>(slot), expected_uuid,
                   &item, &matching) || !reader.atEnd()) return false;
    event->inventory_id = inventory_id;
    event->changed_slot = static_cast<uint16_t>(slot);
    if (matching) {
        event->matching_maps[0] = std::move(item);
        event->matching_map_count = 1U;
    }
    return true;
}

bool parseSlot(std::string_view packet, uint32_t inventory_id,
               int64_t expected_uuid, MapAnvilDiagnosticEvent* event) {
    if (!event || packet.size() > kMaximumPacketBytes) return false;
    return parseSlotVariant(packet, true, inventory_id, expected_uuid, event) ||
           parseSlotVariant(packet, false, inventory_id, expected_uuid, event);
}

}  // namespace

void MapAnvilMapIdentityIndex::Observe(int32_t network_stack_id,
                                       int64_t map_uuid) noexcept {
    if (network_stack_id <= 0 || map_uuid == -1) return;
    for (size_t index = 0; index < count_; ++index) {
        Entry& entry = entries_[index];
        if (entry.network_stack_id != network_stack_id) continue;
        if (entry.map_uuid != map_uuid) entry.ambiguous = true;
        return;
    }
    if (count_ == entries_.size()) {
        overflowed_ = true;
        return;
    }
    entries_[count_++] = {network_stack_id, map_uuid, false};
}

MapAnvilIdentityMatch MapAnvilMapIdentityIndex::Resolve(
    int32_t network_stack_id, int64_t* map_uuid) const noexcept {
    if (map_uuid) *map_uuid = -1;
    if (network_stack_id <= 0) return MapAnvilIdentityMatch::Missing;
    // Once capacity is exceeded, an unrecorded conflicting observation is
    // possible; no source can be safely identified from this index.
    if (overflowed_) return MapAnvilIdentityMatch::Ambiguous;
    for (size_t index = 0; index < count_; ++index) {
        const Entry& entry = entries_[index];
        if (entry.network_stack_id != network_stack_id) continue;
        if (entry.ambiguous) return MapAnvilIdentityMatch::Ambiguous;
        if (map_uuid) *map_uuid = entry.map_uuid;
        return MapAnvilIdentityMatch::Unique;
    }
    return MapAnvilIdentityMatch::Missing;
}

bool MapAnvilWireDiagnostic::ArmDebugFirstType5Candidate() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (debug_candidate_consumed_ || status_ != MapAnvilDiagnosticStatus::Off)
            return false;
        debug_candidate_consumed_ = true;
        debug_candidate_mode_ = true;
        target_ = {};
        target_.ticket = ++last_ticket_;
        target_.expected_map_uuid = -1;
        status_ = MapAnvilDiagnosticStatus::AwaitingOpen;
        deadline_ = std::chrono::steady_clock::time_point::max();
        window_id_ = 0;
        window_type_ = 0;
        event_count_ = 0;
        return true;
    } catch (...) {
        return false;
    }
}

bool MapAnvilWireDiagnostic::Arm(
    const MapAnvilDiagnosticTarget& target,
    std::chrono::steady_clock::time_point now) noexcept {
    if (!target.ticket || target.expected_map_uuid == -1 ||
        !target.anvil_block_confirmed) return false;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (target.ticket <= last_ticket_ ||
            status_ == MapAnvilDiagnosticStatus::AwaitingOpen ||
            status_ == MapAnvilDiagnosticStatus::WindowObserved) return false;
        target_ = target;
        last_ticket_ = target.ticket;
        deadline_ = now + kLifetime;
        status_ = MapAnvilDiagnosticStatus::AwaitingOpen;
        debug_candidate_mode_ = false;
        window_id_ = 0;
        window_type_ = 0;
        event_count_ = 0;
        return true;
    } catch (...) {
        return false;
    }
}

void MapAnvilWireDiagnostic::Disarm(uint64_t ticket) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ticket || target_.ticket != ticket) return;
        target_ = {};
        deadline_ = {};
        status_ = MapAnvilDiagnosticStatus::Off;
        window_id_ = 0;
        window_type_ = 0;
        event_count_ = 0;
    } catch (...) {
    }
}

void MapAnvilWireDiagnostic::expireLocked(
    std::chrono::steady_clock::time_point now) noexcept {
    if (status_ != MapAnvilDiagnosticStatus::Off &&
        status_ != MapAnvilDiagnosticStatus::Expired &&
        status_ != MapAnvilDiagnosticStatus::Exhausted && now >= deadline_) {
        status_ = MapAnvilDiagnosticStatus::Expired;
    }
}

MapAnvilDiagnosticStatus MapAnvilWireDiagnostic::Status(
    uint64_t ticket, std::chrono::steady_clock::time_point now) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        expireLocked(now);
        return ticket && ticket == target_.ticket
            ? status_ : MapAnvilDiagnosticStatus::Off;
    } catch (...) {
        return MapAnvilDiagnosticStatus::Off;
    }
}

bool MapAnvilWireDiagnostic::Observe(
    std::string_view packet, std::chrono::steady_clock::time_point now,
    MapAnvilDiagnosticEvent* event) noexcept {
    if (event) *event = {};
    if (packet.empty() || packet.size() > kMaximumPacketBytes) return false;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        expireLocked(now);
        if (status_ != MapAnvilDiagnosticStatus::AwaitingOpen &&
            status_ != MapAnvilDiagnosticStatus::WindowObserved &&
            status_ != MapAnvilDiagnosticStatus::Closed) return false;
        Reader reader(packet);
        uint32_t wire_header = 0;
        if (!reader.varUInt(&wire_header)) return false;
        const uint32_t packet_id = wire_header & 0x3FFU;
        MapAnvilDiagnosticEvent parsed;
        parsed.ticket = target_.ticket;
        parsed.window_id = window_id_;
        parsed.container_type = window_type_;
        if (packet_id == kOpenId &&
            status_ == MapAnvilDiagnosticStatus::AwaitingOpen) {
            Open opened;
            if (!parseOpen(packet, &opened) || opened.window == 0U ||
                opened.window == 0xFFU ||
                (debug_candidate_mode_
                    ? opened.type != 5U
                    : (opened.x != target_.x || opened.y != target_.y ||
                       opened.z != target_.z))) return false;
            if (debug_candidate_mode_) {
                target_.x = opened.x;
                target_.y = opened.y;
                target_.z = opened.z;
                deadline_ = now + kLifetime;
            }
            window_id_ = opened.window;
            window_type_ = opened.type;
            status_ = MapAnvilDiagnosticStatus::WindowObserved;
            parsed.kind = MapAnvilDiagnosticEventKind::Open;
            parsed.window_id = opened.window;
            parsed.container_type = opened.type;
            parsed.x = opened.x;
            parsed.y = opened.y;
            parsed.z = opened.z;
        } else if ((packet_id == kContentId || packet_id == kSlotId) &&
                   status_ != MapAnvilDiagnosticStatus::AwaitingOpen) {
            uint32_t inventory_id = 0;
            if (!reader.varUInt(&inventory_id) ||
                (inventory_id != 0U &&
                 (status_ != MapAnvilDiagnosticStatus::WindowObserved ||
                  inventory_id != window_id_))) return false;
            const bool content = packet_id == kContentId;
            if (!(content ? parseContent(packet, inventory_id,
                                          target_.expected_map_uuid, &parsed)
                          : parseSlot(packet, inventory_id,
                                      target_.expected_map_uuid, &parsed))) return false;
            // Player inventory ID 0 can update frequently. Record only this
            // exact map UUID; anvil-window content also records slot count.
            if (inventory_id == 0U && parsed.matching_map_count == 0U) return false;
            if (debug_candidate_mode_ && inventory_id == window_id_ &&
                target_.expected_map_uuid == -1 &&
                parsed.matching_map_count == 1U) {
                target_.expected_map_uuid = parsed.matching_maps[0].map_uuid;
            }
            parsed.kind = content ? MapAnvilDiagnosticEventKind::Content
                                  : MapAnvilDiagnosticEventKind::Slot;
        } else if (packet_id == kCloseId &&
                   status_ == MapAnvilDiagnosticStatus::WindowObserved) {
            uint8_t window = 0, type = 0;
            if (!parseClose(packet, &window, &type) ||
                window != window_id_ || type != window_type_) return false;
            status_ = MapAnvilDiagnosticStatus::Closed;
            parsed.kind = MapAnvilDiagnosticEventKind::Close;
        } else {
            return false;
        }
        ++event_count_;
        if (event_count_ >= kMaximumEvents) {
            status_ = MapAnvilDiagnosticStatus::Exhausted;
        }
        if (event) *event = std::move(parsed);
        return true;
    } catch (...) {
        if (event) *event = {};
        return false;
    }
}

}  // namespace build_import
