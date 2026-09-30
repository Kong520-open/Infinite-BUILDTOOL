#ifndef INFINITE_TEXTURE_CONTAINER_CLOSE_PACKET_SENDER_H
#define INFINITE_TEXTURE_CONTAINER_CLOSE_PACKET_SENDER_H

#include <cstdint>
#include <string>

namespace build_import {

// Sends a client-initiated close for the currently open container through the
// game's native ContainerClosePacket path. Call only from the game thread.
class ContainerClosePacketSender {
public:
    static bool send(uint8_t container_id, uint8_t container_type,
                     std::string* error = nullptr);
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_CONTAINER_CLOSE_PACKET_SENDER_H
