#pragma once

namespace build_import {

// Verify and cache this game's packet/action ABI before any diagnostic hook
// instruments a constructor whose original bytes are part of that profile.
bool PrimeMapManualOutboundTraceProfile() noexcept;

// Diagnostic-only observation of the already-constructed native packet.
// Never modifies a packet, sender, slot, or request counter.
void ObserveMapManualOutboundPacket(const void* packet) noexcept;

}  // namespace build_import
