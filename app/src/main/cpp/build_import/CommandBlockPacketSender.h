#ifndef INFINITE_TEXTURE_COMMAND_BLOCK_PACKET_SENDER_H
#define INFINITE_TEXTURE_COMMAND_BLOCK_PACKET_SENDER_H

#include "CommandBlockSpool.h"

#include <string>

namespace build_import {

// Sends one already-placed command block's settings through the game's native
// CommandBlockUpdatePacket path.  This is intentionally a synchronous,
// game-thread-only primitive: callers must invoke it from the local-player
// tick after the command-block shell has been placed and its chunk is loaded.
class CommandBlockPacketSender {
public:
    // Returns false without sending if this is not the ABI/library revision
    // this packet layout was recovered from, or when no live loopback sender
    // has been captured yet.  `error` is optional and never contains engine
    // pointers or raw packet data.
    static bool send(const CommandBlockRecord& record, std::string* error = nullptr);
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_COMMAND_BLOCK_PACKET_SENDER_H
