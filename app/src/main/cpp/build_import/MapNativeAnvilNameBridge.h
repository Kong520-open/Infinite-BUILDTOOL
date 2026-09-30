#pragma once

#include "MapVisibleAnvilWindowSession.h"

#include <cstdint>
#include <string>

namespace build_import {

struct MapNativeAnvilNameRequest {
    uint64_t ticket = 0;
    uint8_t expected_window_id = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint64_t tile_cursor = 0;
    uint64_t columns = 0;
    uint64_t rows = 0;
    std::string expected_title;
};

// Pure, host-testable gate. A successful result only proves that this is the
// expected open type-5 anvil window and its expected tile name. It does not
// prove the input map, recipe or server-side rename.
bool ValidateMapNativeAnvilNameGate(const MapNativeAnvilNameRequest& request,
                                    MapVisibleAnvilWindowState state,
                                    const ContainerCaptureResult* capture,
                                    std::string* validated_title,
                                    std::string* error = nullptr);

// Disabled unless explicitly called by a higher-level, verified pipeline.
// Submits the game's original anvil text-change callback on its owning native
// thread. now_ms must be the current monotonic game-tick time; the live
// mailbox, world, numeric window ID and native manager are rechecked first.
// A true return means only that the callback ran, not that crafting or output
// collection succeeded. This never moves items or closes the UI.
bool SubmitMapNativeAnvilRenameText(uintptr_t minecraft_base,
                                    const MapNativeAnvilNameRequest& request,
                                    uint64_t now_ms,
                                    const MapVisibleAnvilWindowSession& session,
                                    std::string* error = nullptr);

}  // namespace build_import
