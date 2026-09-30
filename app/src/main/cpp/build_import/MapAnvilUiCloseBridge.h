#pragma once

#include <cstdint>

namespace build_import {

// The server-side ContainerClose does not dismiss the game's visible anvil
// screen. Queue one stock Android BACK action only for the exact live native
// anvil screen whose rename has already been confirmed.
bool QueueMapAnvilUiClose(uint64_t ticket) noexcept;
uint64_t TakePendingMapAnvilUiCloseRequest() noexcept;
bool IsMapAnvilUiCloseStillSafe(uint64_t ticket) noexcept;
void MarkMapAnvilUiCloseDispatched(uint64_t ticket) noexcept;
bool WasMapAnvilUiCloseDispatched(uint64_t ticket) noexcept;
void CancelMapAnvilUiClose(uint64_t ticket) noexcept;

}  // namespace build_import
