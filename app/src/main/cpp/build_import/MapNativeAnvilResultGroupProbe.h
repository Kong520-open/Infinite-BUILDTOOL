#pragma once

#include "MapVisibleAnvilWindowSession.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace build_import {

struct MapNativeAnvilResultGroupObservation {
    std::string group;
    uint32_t nodes_scanned = 0;
    bool exact_match = false;
};

// The reader must copy exactly size bytes or return false. This injectable
// boundary lets host tests cover malformed and unreadable native memory.
using MapNativeAnvilMemoryReader = bool (*)(uintptr_t address, void* destination,
                                            size_t size, void* context) noexcept;

// Pure read-only decoder for this exact Android libc++ registry layout. It
// never calls the game's lookup/insert helper. True means key 2 exists exactly
// once and its string is exactly "anvil_result_items".
bool ProbeMapNativeAnvilResultGroupRegistry(
    uintptr_t registry, MapNativeAnvilMemoryReader reader, void* reader_context,
    MapNativeAnvilResultGroupObservation* observation,
    std::string* error = nullptr);

// Optional diagnostic only. Requires the exact libminecraftpe.so Build ID,
// the captured live type-5 anvil window, and the screen constructor's thread.
// This reads the registry but never clicks, closes, or alters the UI.
bool ProbeMapNativeAnvilResultGroup(
    uintptr_t minecraft_base, uint64_t ticket,
    const MapVisibleAnvilWindowSession& session,
    MapNativeAnvilResultGroupObservation* observation,
    std::string* error = nullptr);

}  // namespace build_import
