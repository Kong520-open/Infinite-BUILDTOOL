#ifndef INFINITE_TEXTURE_PLAYER_INVENTORY_OPEN_PACKET_SENDER_H
#define INFINITE_TEXTURE_PLAYER_INVENTORY_OPEN_PACKET_SENDER_H

#include <string>

namespace build_import {

// Sends Interact(OpenInventory) through the current game's native packet ABI.
// It is deliberately limited to the local player and is called only from the
// verified LocalPlayer game tick, immediately after the silent-session mailbox
// has been armed.
class PlayerInventoryOpenPacketSender {
public:
    static bool send(std::string* error = nullptr);
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_PLAYER_INVENTORY_OPEN_PACKET_SENDER_H
