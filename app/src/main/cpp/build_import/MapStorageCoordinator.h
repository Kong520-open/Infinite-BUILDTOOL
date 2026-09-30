#ifndef INFINITE_TEXTURE_MAP_STORAGE_COORDINATOR_H
#define INFINITE_TEXTURE_MAP_STORAGE_COORDINATOR_H

#include "MapAnvilRenameJournal.h"
#include "MapExtraChestPlacementJournal.h"

#include <cstdint>
#include <string>
#include <vector>

namespace build_import {

// A read-only description of the next proof or journal transition required
// for one automatic map tile. These values never authorize a packet send:
// callers must still perform each journal's fresh live preflight and arm it
// durably before dispatch. No runtime path invokes this classifier yet.
enum class MapStorageNextStep : uint8_t {
    Unsafe,
    AwaitPairJournal,
    SurveyPairSupport,
    ReconcilePairSupportNoResend,
    SurveyPairChest,
    ReconcilePairChestNoResend,
    SurveyPairAnvil,
    ReconcilePairAnvilNoResend,
    AwaitExtraChestJournal,
    SurveyExtraSupport,
    ReconcileExtraSupportNoResend,
    SurveyExtraChest,
    ReconcileExtraChestNoResend,
    AwaitRenameJournal,
    PreflightAnvilInput,
    ReconcileAnvilInputNoResend,
    PreflightAnvilCraft,
    ReconcileAnvilCraftNoResend,
    AwaitChestJournal,
    PreflightChestTransfer,
    ReopenChestNoResend,
    CommitTileCursor,
    ClearCommittedRenameJournal,
    ClearCommittedChestJournal,
    FinalizeCommittedMapUse,
    AwaitMapUseMarker,
    ClearExtraChestJournalsDescending,
    ClearPairJournal,
    Complete,
};

struct MapStorageCoordinatorInput {
    std::string world_id;
    int32_t dimension_id = 0;
    BlockBounds artwork_bounds;
    uint32_t columns = 0;
    uint32_t rows = 0;
    uint64_t tile_count = 0;
    uint64_t checkpoint_tile_cursor = 0;
    // Supplied only after loading and world-validating map-use.pending.
    // An active map has marker cursor == checkpoint; after chest commit the
    // old marker remains at checkpoint-1 until journal cleanup is finished.
    bool has_pending_map_use_marker = false;
    uint64_t pending_map_use_cursor = 0;
    // For a new send this comes from the freshly created map's native
    // identity. -1 is allowed after an Armed/ACK journal solely for no-resend
    // reconciliation, or after a confirmed chest transfer for durable cursor
    // commit/cleanup. The persisted UUID never authorizes a fresh Place.
    int64_t expected_map_uuid = -1;
    // Required for InputConfirmed -> fresh craft, especially after a process
    // restart when the map is no longer held. This must be a same-tick native
    // readback from the currently open anvil input, not a journal or packet
    // copy. The pure classifier checks every field against the durable record;
    // the sender must recheck it again immediately before dispatch.
    const MapAnvilInputProof* fresh_anvil_input = nullptr;
    const MapPairPlacementRecord* pair = nullptr;
    // Ordered, immutable sidecars for chest indices 1..N. This pure check
    // does not replace ValidateMapExtraChestPlanChain's live readbacks.
    std::vector<MapExtraChestPlacementRecord> extra_chests;
    const MapAnvilRenameRecord* rename = nullptr;
    const MapChestTransferRecord* chest = nullptr;
};

struct MapStorageCoordinatorDecision {
    MapStorageNextStep next = MapStorageNextStep::Unsafe;
    MapTileChestAddress chest_address;
    // Generated from tile index, not copied from a potentially stale item.
    std::string expected_title;
};

bool ClassifyMapStorageCoordinator(
    const MapStorageCoordinatorInput& input,
    MapStorageCoordinatorDecision* decision,
    std::string* error = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_STORAGE_COORDINATOR_H
