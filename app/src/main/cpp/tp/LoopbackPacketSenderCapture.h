#ifndef LOOPBACK_PACKET_SENDER_CAPTURE_H
#define LOOPBACK_PACKET_SENDER_CAPTURE_H

#include <cstdint>
#include <cstddef>
// Captures the sender and inspects packets required by building tools.
// Returns false when the hook cannot be installed yet so callers may retry.
bool InitLoopbackPacketSenderCapture(uintptr_t baseAddr);

// The sender instance observed by the existing sendToServer hook.  It is used
// by gameplay modules on the game thread only.
void* GetCapturedLoopbackPacketSender();
// Dobby rewrites the sendToServer entry point after this capture hook is
// installed.  Native packet producers can use this immutable pre-hook
// snapshot to verify an ABI fingerprint without incorrectly treating our own
// hook veneer as a game-version mismatch.
bool CapturedLoopbackPacketSenderOriginalPrologueMatches(const uint8_t* expected,
                                                         size_t size);
bool IsMemoryReadable(const void* ptr, size_t size);

#endif // LOOPBACK_PACKET_SENDER_CAPTURE_H
