#ifndef MINECRAFT_UPDATE_HOOK_H
#define MINECRAFT_UPDATE_HOOK_H

#include <cstdint>
#include <string>

// Installs the local-player tick hook used by building operations.
bool InitMinecraftUpdateHook(uintptr_t baseAddr);

// Run the client API world/dimension query on the local-player game thread.
// JNI callers may wait for at most timeout_ms; no engine API is invoked from
// their background thread.
bool QueryWorldContextOnGameThread(std::string* output, int timeout_ms);
uintptr_t GetCachedDimensionTokenForWorld(const std::string& world_context);

// Receives PyRpc feedback from the network hook. Returns true only for packets
// owned by the current export teleport, allowing its internal command feedback
// to be removed from the normal game UI.
bool ObserveBuildExportTeleportPacket(const std::string& packet);

// Starts an asynchronous server `/tp` to the requested active export-batch
// centre. The request succeeds only after the local player's native block
// position arrives at that centre; command feedback is deliberately ignored.
bool RequestBuildExportTeleport(float x, float y, float z);

// True while a coordinate-verified export TP is still waiting to be sent or
// for the local player position to reach its target. This is polled only from
// the local-player game tick by BuildExportRuntime.
bool IsBuildExportTeleportPending();

// Drops a queued teleport and any in-flight coordinate verification. Build
// export uses the shared slot while moving between batches or when cancelled.
void CancelBuildExportTeleport();

// Captured from Actor::normalTick and only consumed on that same game thread.
void* GetLocalPlayerPointer();

// True only on the thread currently executing the verified LocalPlayer
// Actor::normalTick hook. It is false before that hook records a game thread.
bool IsMinecraftUpdateGameThread() noexcept;

#endif // MINECRAFT_UPDATE_HOOK_H
