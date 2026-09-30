#pragma once

#include <cstdint>

namespace build_import {

// A packet-visible, tool-owned 27-slot chest is the only eligible window.
// The bridge does not send ContainerClose or a key event; the Java caller
// delivers one stock Back down/up only after IsMapChestUiCloseStillSafe.
// Packet identity cannot prove that the chest is the current top native UI,
// so callers must also check the live, focused game Activity and promptly
// cancel on any task/world/window transition. The claim expires in 1 second.
bool QueueMapChestUiClose(uint64_t token, uint8_t window_id,
                          uint8_t window_type, int32_t x, int32_t y,
                          int32_t z) noexcept;
uint64_t TakePendingMapChestUiCloseRequest() noexcept;
bool IsMapChestUiCloseStillSafe(uint64_t token) noexcept;
void MarkMapChestUiCloseDispatched(uint64_t token) noexcept;
bool WasMapChestUiCloseDispatched(uint64_t token) noexcept;
// Called by the existing LoopbackPacketSender hook AFTER its original send
// returns. This records only an exact close for the tool-owned visible chest;
// the observer never sends a packet or claims that the server accepted it.
bool HasPendingMapChestUiCloseWindow() noexcept;
void ObserveMapChestOutboundClose(uint8_t window_id,
                                  uint8_t window_type) noexcept;
bool WasMapChestOutboundCloseObserved(uint64_t token) noexcept;
void CancelMapChestUiClose(uint64_t token) noexcept;

}  // namespace build_import
