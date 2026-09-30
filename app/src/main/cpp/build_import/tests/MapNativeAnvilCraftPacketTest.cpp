#include "../MapNativeAnvilCraftPacket.h"

#include <cassert>
#include <string>

int main() {
    using namespace build_import;
    MapNativeAnvilCraftPacketShape shape{};
    shape.request_count = 1;
    shape.request_id = -9;
    shape.action_count = 3;
    shape.name_count = 1;
    shape.custom_name = "地图 1行1列";
    shape.first_action_type = 15;
    shape.recipe_network_id = 3304;
    shape.filtered_string_index = 0;
    shape.second_action_type = 5;
    shape.consumed_count = 1;
    shape.consumed_container = 0;
    shape.consumed_slot = 1;
    shape.consumed_network_id = 21;
    shape.third_action_type = 1;
    shape.placed_count = 1;
    shape.created_container = 61;
    shape.created_slot = 50;
    shape.created_predicted_sequence = shape.request_id;
    shape.created_variant_tag = 1;
    shape.destination_container = 12;
    shape.destination_slot = 0;
    shape.destination_network_id = 0;
    const std::string title = shape.custom_name;
    assert(IsMapNativeAnvilCraftPacketCandidate(shape, title, 21));
    shape.destination_slot = 1;
    assert(IsMapNativeAnvilCraftPacketCandidate(shape, title, 21));
    shape.destination_slot = 8;
    assert(IsMapNativeAnvilCraftPacketCandidate(shape, title, 21));
    shape.destination_slot = 9;
    assert(!IsMapNativeAnvilCraftPacketCandidate(shape, title, 21));
    shape.destination_slot = 0;
    assert(!IsMapNativeAnvilCraftPacketCandidate(shape, title + " ", 21));
    assert(!IsMapNativeAnvilCraftPacketCandidate(shape, title, 22));
    shape.recipe_network_id = 0;
    assert(!IsMapNativeAnvilCraftPacketCandidate(shape, title, 21));
    shape.recipe_network_id = 3304;
    shape.created_predicted_sequence = -11;
    assert(!IsMapNativeAnvilCraftPacketCandidate(shape, title, 21));
    shape.created_predicted_sequence = -9;
    shape.destination_container = 29;
    assert(!IsMapNativeAnvilCraftPacketCandidate(shape, title, 21));
    shape.destination_container = 12;
    shape.action_count = 4;
    assert(!IsMapNativeAnvilCraftPacketCandidate(shape, title, 21));
    shape.action_count = 3;
    shape.request_id = -10;
    assert(!IsMapNativeAnvilCraftPacketCandidate(shape, title, 21));
    assert(!DecodeMapNativeAnvilCraftPacketCandidate(0, nullptr, &shape));
    return 0;
}
