#pragma once

#include <cstdint>
#include <string>

namespace build_import {

// A decoded, read-only candidate from one game-created ItemStackRequest.
// This describes the narrow result-take pattern inferred from the native
// constructors; it is NOT yet a verified wire profile or permission to send.
struct MapNativeAnvilCraftPacketShape {
    uint32_t request_count = 0;
    int32_t request_id = 0;
    uint32_t action_count = 0;
    uint32_t name_count = 0;
    std::string custom_name;
    uint8_t first_action_type = 0;
    uint32_t recipe_network_id = 0;
    int32_t filtered_string_index = -1;
    uint8_t second_action_type = 0;
    uint8_t consumed_count = 0;
    uint8_t consumed_container = 0;
    uint8_t consumed_slot = 0;
    int32_t consumed_network_id = 0;
    uint8_t third_action_type = 0;
    uint8_t placed_count = 0;
    uint8_t created_container = 0;
    uint8_t created_slot = 0;
    int32_t created_predicted_sequence = 0;
    int32_t created_variant_tag = 0;
    uint8_t destination_container = 0;
    uint8_t destination_slot = 0;
    int32_t destination_network_id = 0;
};

// Host-testable, deliberately restrictive candidate check. Even true is
// diagnostic only until a real outbound action sequence confirms every field.
bool IsMapNativeAnvilCraftPacketCandidate(
    const MapNativeAnvilCraftPacketShape& shape,
    const std::string& expected_title,
    int32_t expected_input_network_id) noexcept;

// Reads only the exact SO's native packet/request/action ABI after checking
// executable fingerprints and object vtables. Returns false on any malformed
// field and never mutates or sends the packet.
bool DecodeMapNativeAnvilCraftPacketCandidate(
    uintptr_t minecraft_base, const void* packet,
    MapNativeAnvilCraftPacketShape* output) noexcept;

}  // namespace build_import
