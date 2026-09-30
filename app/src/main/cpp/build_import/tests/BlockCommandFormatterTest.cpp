#include "../BlockCommandFormatter.h"

#include <cassert>

using namespace build_import;

int main() {
    assert(formatPlacementBlockArgument("minecraft:stone", 0) == "minecraft:stone");
    assert(formatPlacementBlockArgument("minecraft:stone", 4) == "minecraft:stone 4");

    assert(formatPlacementBlockArgument("minecraft:pale_oak_standing_sign", 14) ==
           "minecraft:pale_oak_standing_sign [\"ground_sign_direction\"=14]");
    assert(formatPlacementBlockArgument("minecraft:standing_sign", 0) ==
           "minecraft:standing_sign [\"ground_sign_direction\"=0]");
    assert(formatPlacementBlockArgument("minecraft:standing_sign", 5) ==
           "minecraft:standing_sign [\"ground_sign_direction\"=5]");
    assert(formatPlacementBlockArgument("minecraft:wall_sign", 0) ==
           "minecraft:wall_sign [\"facing_direction\"=0]");
    assert(formatPlacementBlockArgument("minecraft:wall_sign", 3) ==
           "minecraft:wall_sign [\"facing_direction\"=3]");

    assert(formatPlacementBlockArgument("minecraft:oak_hanging_sign", 2) ==
           "minecraft:oak_hanging_sign [\"attached_bit\"=false,\"facing_direction\"=2,"
           "\"ground_sign_direction\"=0,\"hanging\"=false]");
    assert(formatPlacementBlockArgument("minecraft:oak_hanging_sign", 7) ==
           "minecraft:oak_hanging_sign 7");
    assert(formatPlacementBlockArgument("minecraft:pale_oak_hanging_sign", 0x01DAU) ==
           "minecraft:pale_oak_hanging_sign [\"attached_bit\"=true,"
           "\"facing_direction\"=2,\"ground_sign_direction\"=11,"
           "\"hanging\"=true]");

    assert(formatPlacementBlockArgument("minecraft:bamboo_fence_gate", 1) ==
           "minecraft:bamboo_fence_gate [\"in_wall_bit\"=false,"
           "\"minecraft:cardinal_direction\"=\"west\",\"open_bit\"=false]");
    assert(formatPlacementBlockArgument("minecraft:fence_gate", 0) ==
           "minecraft:fence_gate [\"in_wall_bit\"=false,"
           "\"minecraft:cardinal_direction\"=\"south\",\"open_bit\"=false]");
    assert(formatPlacementBlockArgument("minecraft:bamboo_fence_gate", 2) ==
           "minecraft:bamboo_fence_gate [\"in_wall_bit\"=false,"
           "\"minecraft:cardinal_direction\"=\"north\",\"open_bit\"=false]");
    assert(formatPlacementBlockArgument("minecraft:fence_gate", 12) ==
           "minecraft:fence_gate [\"in_wall_bit\"=true,"
           "\"minecraft:cardinal_direction\"=\"south\",\"open_bit\"=true]");
    assert(formatPlacementBlockArgument("minecraft:fence_gate", 15) ==
           "minecraft:fence_gate [\"in_wall_bit\"=true,"
           "\"minecraft:cardinal_direction\"=\"east\",\"open_bit\"=true]");

    assert(formatVerificationBlockArgument("minecraft:wall_sign", 3, false) ==
           "minecraft:wall_sign [\"facing_direction\"=3]");
    assert(formatVerificationBlockArgument("minecraft:wall_sign", 3, true) ==
           "minecraft:wall_sign -1");
    return 0;
}
