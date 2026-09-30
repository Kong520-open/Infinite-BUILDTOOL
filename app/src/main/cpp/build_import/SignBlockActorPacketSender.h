#ifndef INFINITE_TEXTURE_SIGN_BLOCK_ACTOR_PACKET_SENDER_H
#define INFINITE_TEXTURE_SIGN_BLOCK_ACTOR_PACKET_SENDER_H

#include "DeferredImportDataSpool.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace build_import {

// Synchronously restores one already-placed sign through the game's native
// BlockActorDataPacket path. Call only from the local-player/game thread after
// the target chunk and sign shell have been verified by the import runtime.
class SignBlockActorPacketSender {
public:
    enum class Face : uint8_t {
        Front = 0,
        Back = 1,
    };

    static constexpr size_t kMaximumFaceTextBytes = 64U * 1024U;
    static constexpr size_t kMaximumTotalTextBytes = 96U * 1024U;

    // Performs the same bounded, fail-closed input checks used by send(). This
    // is exposed so the parser/runtime can reject a bad spool record before it
    // reaches any game ABI call.
    static bool validate(const SignRecord& record,
                         std::string* error = nullptr);

    // Returns false without sending on unsupported CPUs, unknown game builds,
    // invalid sign data, or when no live LoopbackPacketSender is available.
    static bool send(const SignRecord& record,
                     std::string* error = nullptr);

    // Sends only the face selected by the server's OpenSign packet. Bedrock's
    // edit session accepts data for one face at a time; including both faces in
    // one BlockActorDataPacket can silently discard the non-session face.
    // Wax state is optional so a two-face restore can defer IsWaxed until the
    // final edit session instead of locking the second face prematurely.
    static bool sendFace(const SignRecord& record, Face face,
                         bool include_waxed,
                         std::string* error = nullptr);
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_SIGN_BLOCK_ACTOR_PACKET_SENDER_H
