#ifndef INFINITE_TEXTURE_COMMAND_BLOCK_PACKET_ABI_PROFILE_H
#define INFINITE_TEXTURE_COMMAND_BLOCK_PACKET_ABI_PROFILE_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace build_import {

// A CommandBlockUpdatePacket can only be constructed and sent when every
// address below belongs to the same libminecraftpe.so build.  In particular,
// its constructor, vtable, packet layout and LoopbackPacketSender entry point
// must never be mixed between profiles.
struct CommandBlockPacketAbiProfile {
    const char* name = "";
    uintptr_t constructor_rva = 0;
    uintptr_t vtable_rva = 0;
    uintptr_t sender_rva = 0;
    size_t packet_size = 0;
    std::array<uint8_t, 32U> constructor_prologue{};
    std::array<uint8_t, 32U> sender_prologue{};
};

// Keep the original current-address-table profile and the recovered x19
// 3.8.15 profile together.  The caller identifies a profile by both real code
// fingerprints before it constructs a game-owned packet or installs a hook.
inline constexpr std::array<CommandBlockPacketAbiProfile, 2U>
    kCommandBlockPacketAbiProfiles{{
        {
            "x19-current-address-table",
            0x0A3CC4A0ULL,
            0x128E3EE8ULL,
            0x0A785BB0ULL,
            0xB8U,
            {{
                0xFDU, 0x7BU, 0xBEU, 0xA9U, 0xF4U, 0x4FU, 0x01U, 0xA9U,
                0xFDU, 0x03U, 0x00U, 0x91U, 0xF3U, 0x03U, 0x00U, 0xAAU,
                0x16U, 0xC0U, 0x0DU, 0x94U, 0x00U, 0xE4U, 0x00U, 0x6FU,
                0xA8U, 0x28U, 0x04U, 0xF0U, 0x08U, 0xA1U, 0x3BU, 0x91U,
            }},
            {{
                0xFDU, 0x7BU, 0xBBU, 0xA9U, 0xFCU, 0x0BU, 0x00U, 0xF9U,
                0xF8U, 0x5FU, 0x02U, 0xA9U, 0xF6U, 0x57U, 0x03U, 0xA9U,
                0xF4U, 0x4FU, 0x04U, 0xA9U, 0xFDU, 0x03U, 0x00U, 0x91U,
                0xFFU, 0xC3U, 0x08U, 0xD1U, 0x55U, 0xD0U, 0x3BU, 0xD5U,
            }},
        },
        {
            "x19-3.8.15",
            0x07EB6730ULL,
            0x0FBFFC10ULL,
            0x05D4AFB8ULL,
            0xB8U,
            {{
                0xFDU, 0x7BU, 0xBEU, 0xA9U, 0xF4U, 0x4FU, 0x01U, 0xA9U,
                0xFDU, 0x03U, 0x00U, 0x91U, 0xF3U, 0x03U, 0x00U, 0xAAU,
                0x2BU, 0x15U, 0x78U, 0x97U, 0x48U, 0xEAU, 0x03U, 0xB0U,
                0x34U, 0x00U, 0x80U, 0x52U, 0x00U, 0xE4U, 0x00U, 0x6FU,
            }},
            {{
                0xFDU, 0x7BU, 0xBBU, 0xA9U, 0xFCU, 0x0BU, 0x00U, 0xF9U,
                0xF8U, 0x5FU, 0x02U, 0xA9U, 0xF6U, 0x57U, 0x03U, 0xA9U,
                0xF4U, 0x4FU, 0x04U, 0xA9U, 0xFDU, 0x03U, 0x00U, 0x91U,
                0xFFU, 0xC3U, 0x08U, 0xD1U, 0x55U, 0xD0U, 0x3BU, 0xD5U,
            }},
        },
    }};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_COMMAND_BLOCK_PACKET_ABI_PROFILE_H
