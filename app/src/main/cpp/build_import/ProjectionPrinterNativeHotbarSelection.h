#pragma once

#include <cstdint>
#include <string>

namespace build_import {

// Installs the version-fingerprinted hooks used to retain the current HUD
// ClientInstanceScreenModel while it is alive.  A failure is non-fatal: the
// printer simply waits instead of calling an unverified game function.
bool InitProjectionPrinterNativeHotbarSelection(uintptr_t minecraft_base) noexcept;

// Drops the current HUD/model pair on session revocation.  The hooks remain
// installed for the process lifetime and will capture a later HUD instance.
void ClearProjectionPrinterNativeHotbarSelection() noexcept;

// Runs the game's complete original hotbar-selection path.  It updates the
// native inventory listener/HUD and submits the corresponding normal client
// selection update; it never fabricates an inventory slot or network packet.
// This may only be called from the verified LocalPlayer game tick.
bool RequestProjectionPrinterNativeHotbarSelection(int32_t slot,
                                                   std::string* error = nullptr);

}  // namespace build_import
