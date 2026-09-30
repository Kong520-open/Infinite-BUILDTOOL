#include "../MapAnvilWireDiagnostic.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <string>

using namespace build_import;

namespace {

constexpr int64_t kMapUuid = -532575944698LL;

void le16(std::string* out, uint16_t value) {
    out->push_back(static_cast<char>(value));
    out->push_back(static_cast<char>(value >> 8U));
}

void le32(std::string* out, uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        out->push_back(static_cast<char>(value >> shift));
    }
}

void le64(std::string* out, int64_t value) {
    const uint64_t bits = static_cast<uint64_t>(value);
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        out->push_back(static_cast<char>(bits >> shift));
    }
}

void varUInt(std::string* out, uint64_t value) {
    while (value >= 0x80U) {
        out->push_back(static_cast<char>((value & 0x7FU) | 0x80U));
        value >>= 7U;
    }
    out->push_back(static_cast<char>(value));
}

void varInt(std::string* out, int32_t value) {
    const auto encoded = (static_cast<uint32_t>(value) << 1U) ^
        static_cast<uint32_t>(value >> 31U);
    varUInt(out, encoded);
}

void varInt64(std::string* out, int64_t value) {
    const auto encoded = (static_cast<uint64_t>(value) << 1U) ^
        static_cast<uint64_t>(value >> 63U);
    varUInt(out, encoded);
}

std::string itemExtra(int64_t uuid, const std::string& title) {
    std::string out("\xFF\xFF\x01\x0A", 4U);
    le16(&out, 0U);
    out.push_back(4);
    le16(&out, 8U);
    out += "map_uuid";
    le64(&out, uuid);
    if (!title.empty()) {
        out.push_back(10);  // display compound
        le16(&out, 7U);
        out += "display";
        out.push_back(8);  // Name string
        le16(&out, 4U);
        out += "Name";
        le16(&out, static_cast<uint16_t>(title.size()));
        out += title;
        out.push_back(0);
    }
    out.push_back(0);
    le32(&out, 0U);
    le32(&out, 0U);
    return out;
}

std::string item(int64_t uuid, int32_t network_id,
                 const std::string& title) {
    std::string out;
    varInt(&out, 355);  // synthetic filled-map runtime ID
    le16(&out, 1U);
    varUInt(&out, 0U);
    out.push_back(1);
    varInt(&out, network_id);
    varInt(&out, 0);  // block runtime ID
    const std::string extra = itemExtra(uuid, title);
    varUInt(&out, extra.size());
    out += extra;
    return out;
}

std::string open(uint8_t window, uint8_t type,
                 int32_t x = 235, int32_t y = 22, int32_t z = 126) {
    std::string out;
    varUInt(&out, 0x2EU);
    out.push_back(static_cast<char>(window));
    out.push_back(static_cast<char>(type));
    varInt(&out, x);
    varUInt(&out, static_cast<uint32_t>(y));
    varInt(&out, z);
    varInt64(&out, -1);
    return out;
}

std::string content(uint32_t inventory_id, int64_t uuid,
                    const std::string& title) {
    std::string out;
    varUInt(&out, 0x31U);
    varUInt(&out, inventory_id);
    varUInt(&out, 3U);
    varInt(&out, 0);
    varInt(&out, 0);
    out += item(uuid, 76, title);
    return out;
}

std::string contentWithTail(uint32_t inventory_id, int64_t uuid,
                            uint8_t full_name, bool dynamic,
                            uint32_t dynamic_id) {
    std::string out = content(inventory_id, uuid, "");
    out.push_back(static_cast<char>(full_name));
    out.push_back(dynamic ? 1 : 0);
    if (dynamic) le32(&out, dynamic_id);
    varInt(&out, 0);  // empty storage item
    return out;
}

std::string slot(uint32_t inventory_id, uint32_t slot_number,
                 int64_t uuid, const std::string& title,
                 bool optional_context) {
    std::string out;
    varUInt(&out, 0x32U);
    varUInt(&out, inventory_id);
    varUInt(&out, slot_number);
    if (optional_context) {
        out.push_back(0);  // no FullContainerName
        out.push_back(0);  // no storage item
    } else {
        out.push_back(0);  // FullContainerName 0
        out.push_back(0);  // no dynamic ID
        varInt(&out, 0);   // empty storage item
    }
    out += item(uuid, 76, title);
    return out;
}

std::string close(uint8_t window, uint8_t type) {
    std::string out;
    varUInt(&out, 0x2FU);
    out.push_back(static_cast<char>(window));
    out.push_back(static_cast<char>(type));
    out.push_back(0);
    return out;
}

}  // namespace

int main() {
    MapAnvilMapIdentityIndex identity;
    int64_t resolved_uuid = 0;
    assert(identity.Resolve(71, &resolved_uuid) == MapAnvilIdentityMatch::Missing);
    assert(resolved_uuid == -1);
    identity.Observe(0, kMapUuid);
    identity.Observe(71, -1);
    assert(identity.Size() == 0U);
    identity.Observe(71, kMapUuid);  // negative UUIDs are valid
    identity.Observe(72, kMapUuid);
    identity.Observe(71, kMapUuid);
    assert(identity.Size() == 2U);
    assert(identity.Resolve(71, &resolved_uuid) == MapAnvilIdentityMatch::Unique);
    assert(resolved_uuid == kMapUuid);
    identity.Observe(71, 1234);
    assert(identity.Resolve(71, &resolved_uuid) == MapAnvilIdentityMatch::Ambiguous);
    assert(resolved_uuid == -1);
    assert(identity.Resolve(72, &resolved_uuid) == MapAnvilIdentityMatch::Unique);
    assert(resolved_uuid == kMapUuid);

    MapAnvilWireDiagnostic diagnostic;
    MapAnvilDiagnosticEvent event;
    auto start = std::chrono::steady_clock::time_point{};
    MapAnvilDiagnosticTarget target{7U, 235, 22, 126, kMapUuid, false};
    assert(!diagnostic.Arm(target, start));
    target.anvil_block_confirmed = true;
    assert(diagnostic.Arm(target, start));
    assert(!diagnostic.Arm(target, start));
    assert(!diagnostic.Observe(open(3U, 0U, 236), start, &event));
    assert(event.kind == MapAnvilDiagnosticEventKind::None);
    assert(!diagnostic.Observe(content(3U, kMapUuid, ""), start, &event));

    // The diagnostic reports the live type; it never assumes that 5 is anvil.
    assert(diagnostic.Observe(open(3U, 5U), start, &event));
    assert(event.kind == MapAnvilDiagnosticEventKind::Open &&
           event.window_id == 3U && event.container_type == 5U);
    assert(diagnostic.Status(7U, start) ==
           MapAnvilDiagnosticStatus::WindowObserved);
    assert(!diagnostic.Observe(content(4U, kMapUuid, ""), start, &event));

    MapAnvilWireDiagnostic debug_candidate;
    assert(debug_candidate.ArmDebugFirstType5Candidate());
    assert(!debug_candidate.ArmDebugFirstType5Candidate());
    assert(!debug_candidate.Observe(open(8U, 0U), start, &event));
    assert(debug_candidate.Observe(open(8U, 5U), start, &event));
    assert(event.kind == MapAnvilDiagnosticEventKind::Open &&
           event.x == 235 && event.y == 22 && event.z == 126);
    assert(debug_candidate.Observe(content(8U, kMapUuid, ""), start, &event));
    assert(event.matching_map_count == 1U &&
           event.matching_maps[0].map_uuid == kMapUuid);
    assert(debug_candidate.Status(1U, start + std::chrono::seconds(30)) ==
           MapAnvilDiagnosticStatus::WindowObserved);
    assert(debug_candidate.Status(1U, start + std::chrono::seconds(90)) ==
           MapAnvilDiagnosticStatus::Expired);
    debug_candidate.Disarm(1U);
    assert(!debug_candidate.ArmDebugFirstType5Candidate());

    const std::string title = "\xE5\x9C\xB0\xE5\x9B\xBE 1\xE8\xA1\x8C" "1\xE5\x88\x97";
    assert(diagnostic.Observe(content(3U, kMapUuid, title), start, &event));
    assert(event.kind == MapAnvilDiagnosticEventKind::Content &&
           event.inventory_id == 3U && event.slot_count == 3U &&
           event.matching_map_count == 1U);
    assert(!event.has_full_container_name);
    const auto& output = event.matching_maps[0];
    assert(output.slot == 2U && output.count == 1U &&
           output.has_network_stack_id && output.network_stack_id == 76 &&
           output.map_uuid == kMapUuid && output.name == title &&
           output.name_status == MapItemNameStatus::Present &&
           output.name_source == MapItemNameSource::DisplayName);

    assert(diagnostic.Observe(contentWithTail(3U, kMapUuid, 7U,
                                              true, 0x12345678U),
                              start, &event));
    assert(event.has_full_container_name && event.full_container_name == 7U &&
           event.has_dynamic_container_id &&
           event.dynamic_container_id == 0x12345678U);

    assert(diagnostic.Observe(slot(3U, 2U, kMapUuid, title, true),
                              start, &event));
    assert(event.kind == MapAnvilDiagnosticEventKind::Slot &&
           event.changed_slot == 2U && event.matching_map_count == 1U);
    assert(!diagnostic.Observe(slot(0U, 6U, 1234, title, false),
                               start, &event));
    assert(diagnostic.Observe(close(3U, 5U), start, &event));
    assert(event.kind == MapAnvilDiagnosticEventKind::Close);
    assert(diagnostic.Status(7U, start) == MapAnvilDiagnosticStatus::Closed);
    assert(!diagnostic.Observe(content(3U, kMapUuid, title), start, &event));
    assert(diagnostic.Observe(slot(0U, 6U, kMapUuid, title, false),
                              start, &event));
    assert(event.inventory_id == 0U && event.changed_slot == 6U &&
           event.matching_maps[0].name == title);

    assert(diagnostic.Status(7U, start + std::chrono::seconds(90)) ==
           MapAnvilDiagnosticStatus::Expired);
    assert(!diagnostic.Observe(slot(0U, 6U, kMapUuid, title, true),
                               start + std::chrono::seconds(90), &event));
    diagnostic.Disarm(7U);
    assert(!diagnostic.Arm(target, start));  // old callback ticket not reusable
    target.ticket = 8U;
    assert(diagnostic.Arm(target, start));
    assert(diagnostic.Observe(open(4U, 9U), start, &event));
    assert(event.container_type == 9U);  // no hard-coded type in parser
    for (int index = 0; index < 31; ++index) {
        assert(diagnostic.Observe(content(4U, kMapUuid, ""), start, &event));
    }
    assert(diagnostic.Status(8U, start) ==
           MapAnvilDiagnosticStatus::Exhausted);
    assert(!diagnostic.Observe(content(4U, kMapUuid, ""), start, &event));
}
