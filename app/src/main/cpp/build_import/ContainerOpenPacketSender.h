#ifndef INFINITE_TEXTURE_CONTAINER_OPEN_PACKET_SENDER_H
#define INFINITE_TEXTURE_CONTAINER_OPEN_PACKET_SENDER_H

#include <cstdint>
#include <string>

namespace build_import {

// Local hit coordinates on the support block for a ClickBlock ItemUse.  Normal
// callers keep the game's face-centre default; state-aware placements such as
// stairs use this to deliberately choose the upper or lower half of a side.
struct ItemUseClickPosition {
    float x = 0.5F;
    float y = 0.5F;
    float z = 0.5F;
};

// Sends the client-side InventoryTransaction/ItemUse/ClickBlock request that
// makes the server open a container. This never invokes a local UI, GameMode,
// or LocalPlayer interaction method. Call it only on the local-player game
// tick thread with a Block pointer returned by NativeWorldReader.
class ContainerOpenPacketSender {
public:
    static bool send(int32_t x, int32_t y, int32_t z,
                      const void* native_block, int face = 1,
                      std::string* error = nullptr,
                      int32_t expected_hotbar_slot = -1,
                      bool has_expected_network_stack_id = false,
                      int32_t expected_network_stack_id = 0);
    static bool sendWithClick(int32_t x, int32_t y, int32_t z,
                              const void* native_block, int face,
                              const ItemUseClickPosition& click_position,
                              std::string* error = nullptr,
                              int32_t expected_hotbar_slot = -1,
                              bool has_expected_network_stack_id = false,
                              int32_t expected_network_stack_id = 0);
    // The map creator passes both identities so a user hotbar switch cannot
    // turn an otherwise valid map-use request into use of an unrelated item.
    static bool useSelectedItem(std::string* error = nullptr,
                                int32_t expected_hotbar_slot = -1,
                                bool has_expected_network_stack_id = false,
                                int32_t expected_network_stack_id = 0);
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_CONTAINER_OPEN_PACKET_SENDER_H
