#ifndef INFINITE_TEXTURE_MAP_STORAGE_PRODUCTION_ACTIONS_H
#define INFINITE_TEXTURE_MAP_STORAGE_PRODUCTION_ACTIONS_H

#include "MapChestStorageSender.h"
#include "MapStorageAnvilAdapter.h"
#include "MapStoragePipelineDriver.h"
#include "NativeWorldAccess.h"

#include <cstdint>
#include <string>

namespace build_import {

struct MapStorageTrackedRpcReceipt {
    // Must be the UUID returned by the tracked RPC poll, not a copy of the
    // requested UUID. Pending is not an accepted command.
    std::string returned_uuid;
    MapPairCommandAck ack = MapPairCommandAck::Pending;
};

struct MapStorageHeldMapEvidence {
    FilledMapChestSourceEvidence source;
    uintptr_t dimension_token = 0;
    MapItemNameStatus name_status = MapItemNameStatus::Missing;
    MapItemNameSource name_source = MapItemNameSource::None;
    std::string name;
    bool native_readback = false;
};

// All hooks run synchronously on an authorized live game tick. A false result
// from dispatch_tracked_rpc AFTER its journal has been Armed is ambiguous and
// must not cause a retry. `capture_chest(..., false, ...)` must retain the
// same open capture across Prepared -> send when it remains live; otherwise
// the Prepared journal is refreshed and the next tick rechecks it.
// `capture_chest(..., true, ...)` closes the transfer window without opening
// a second chest; it succeeds only when the close is complete. The accepted
// Place response plus original-window settlement are the proof. In a live
// session settlement includes accepted client source-slot sync and close;
// process recovery requires the durable response and no surviving window.
// This never opens a second chest.
struct MapStorageProductionHooks {
    void* context = nullptr;
    bool (*require_live_world)(void*, const std::string&, int32_t,
                               std::string*) = nullptr;
    bool (*new_command_uuid)(void*, std::string*, std::string*) = nullptr;
    // Must approve only the exact surveyed, single-purpose placement `/fill`
    // commands supported by this game fork. This is an additional command
    // syntax gate, not a replacement for the journal or native preflight.
    bool (*approve_placement_command)(void*, const std::string&,
                                       std::string*) = nullptr;
    bool (*dispatch_tracked_rpc)(void*, const std::string&,
                                 const std::string&, std::string*) = nullptr;
    bool (*poll_tracked_rpc)(void*, const std::string&,
                             MapStorageTrackedRpcReceipt*,
                             std::string*) = nullptr;
    // True only after the runtime has confirmed that the local player is
    // near this exact journaled support cell and its target chunk has had a
    // settling interval. A missing hook forbids support retries.
    bool (*allow_support_retry)(void*, const MapChestPosition&,
                                std::string*) = nullptr;
    bool (*capture_chest)(void*, const MapChestPosition&, bool,
                          ContainerCaptureResult*, std::string*) = nullptr;
    bool (*read_held_map)(void*, MapStorageHeldMapEvidence*,
                          std::string*) = nullptr;
    // Production implementation delegates to SendFilledMapToSingleChest.
    bool (*submit_chest_place)(void*, const FilledMapToSingleChestRequest&,
                               FilledMapChestSubmission*,
                               std::string*) = nullptr;
    // Must atomically persist cursor n+1 (file fsync + parent-directory fsync)
    // before returning true. Does NOT remove map-use.pending yet.
    bool (*commit_cursor_fsynced)(void*, uint64_t, uint64_t,
                                  const MapChestTransferRecord&,
                                  std::string*) = nullptr;
    // Called only after both rename/chest journals have been cleared. Must
    // durably remove the old map-use marker and reset old map runtime fields.
    bool (*finalize_committed_map_use)(void*, uint64_t, uint64_t,
                                       std::string*) = nullptr;
};

struct MapStorageProductionActions {
    std::string map_state_path;
    NativeWorldReader* reader = nullptr;
    MapStorageProductionHooks hooks;
    // Optional, separately verified anvil implementation. Production still
    // refuses every rename step when this is null or a required hook is
    // missing; the runtime's global storage gate remains independent.
    MapStorageAnvilAdapter* anvil_adapter = nullptr;
    // Legacy preplaced-pair test: the runtime re-reads both user-placed blocks
    // in this exact world/tick. Never treat a missing production pair journal
    // as confirmed unless this bounded, ephemeral proof matches the snapshot.
    bool debug_preexisting_pair_verified = false;
    MapChestAnvilPosition debug_preexisting_pair{};
};

// The returned callback table is inert until a MapStoragePipelineDriver calls
// it. Anvil steps are routed only to an explicitly supplied adapter; all
// chest placement, transfer and cursor commits remain owned here.
MapStoragePipelineOps MakeMapStorageProductionOps(
    MapStorageProductionActions* actions) noexcept;

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_STORAGE_PRODUCTION_ACTIONS_H
