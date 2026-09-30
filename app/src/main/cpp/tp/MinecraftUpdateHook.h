#ifndef MINECRAFT_UPDATE_HOOK_H
#define MINECRAFT_UPDATE_HOOK_H

#include <cstdint>
#include <string>

// 初始化 Minecraft::update hook
bool InitMinecraftUpdateHook(uintptr_t baseAddr);

// 提交待执行的 Python 代码（从 JNI 线程调用）
void SubmitPythonCode(const std::string& code);

// Run the client API world/dimension query on the local-player game thread.
// JNI callers may wait for at most timeout_ms; no engine API is invoked from
// their background thread.
bool QueryWorldContextOnGameThread(std::string* output, int timeout_ms);
uintptr_t GetCachedDimensionTokenForWorld(const std::string& world_context);

// Reads clientlevel.get_player_permissions() on the client game thread. A
// successful query returns the exact integer permission level through output;
// callers must require level 2 before enabling operator-only behavior.
bool QueryPlayerPermissionOnGameThread(int* output, int timeout_ms);

// Legacy acknowledgement-based probe. Build export now verifies permission by
// issuing a real TP to the active batch centre and observing local coordinates.
bool ProbeTeleportPermissionOnGameThread(std::string* detail, int timeout_ms);

// Receives PyRpc feedback from the network hook. Returns true only for packets
// owned by the current teleport probe, allowing its internal command feedback
// to be removed from the normal game UI.
bool ObserveTeleportPermissionProbePacket(const std::string& packet);

// The capability is scoped to the live Dimension instance and is cleared by a
// failed or new probe.
bool HasVerifiedTeleportCommandPermission();
void InvalidateTeleportCommandPermission() noexcept;

// Starts an asynchronous server `/tp` to the requested active export-batch
// centre. The request succeeds only after the local player's native block
// position arrives at that centre; command feedback is deliberately ignored.
bool RequestTeleport(float x, float y, float z);

// True while a coordinate-verified export TP is still waiting to be sent or
// for the local player position to reach its target. This is polled only from
// the local-player game tick by BuildExportRuntime.
bool IsTeleportPermissionProbePending();

// Drops a queued teleport and any in-flight coordinate verification. Build
// export uses the shared slot while moving between batches or when cancelled.
void CancelPendingTeleportRequest();

// Captured from Actor::normalTick and only consumed on that same game thread.
void* GetLocalPlayerPointer();

// True only on the thread currently executing the verified LocalPlayer
// Actor::normalTick hook. It is false before that hook records a game thread.
bool IsMinecraftUpdateGameThread() noexcept;

#endif // MINECRAFT_UPDATE_HOOK_H
