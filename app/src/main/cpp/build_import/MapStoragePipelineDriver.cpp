#include "MapStoragePipelineDriver.h"

#include <string>

namespace build_import {
namespace {

bool isReconcileOnly(MapStorageNextStep step) noexcept {
    switch (step) {
        case MapStorageNextStep::ReconcilePairSupportNoResend:
        case MapStorageNextStep::ReconcilePairChestNoResend:
        case MapStorageNextStep::ReconcilePairAnvilNoResend:
        case MapStorageNextStep::ReconcileExtraSupportNoResend:
        case MapStorageNextStep::ReconcileExtraChestNoResend:
        case MapStorageNextStep::ReconcileAnvilInputNoResend:
        case MapStorageNextStep::ReconcileAnvilCraftNoResend:
        case MapStorageNextStep::ReopenChestNoResend:
            return true;
        default:
            return false;
    }
}

// An exact in-process latch, not a substitute for the persisted Armed marker.
// The step and tile identify a single transaction phase. A fresh journal
// snapshot changes the classified step before the next transition is allowed.
std::string transitionKey(const MapStorageCoordinatorInput& snapshot,
                          const MapStorageCoordinatorDecision& decision) {
    return std::to_string(snapshot.world_id.size()) + ":" +
        snapshot.world_id + "|" +
        std::to_string(snapshot.dimension_id) + "|" +
        std::to_string(snapshot.tile_count) + "|" +
        std::to_string(snapshot.checkpoint_tile_cursor) + "|" +
        std::to_string(snapshot.has_pending_map_use_marker ? 1 : 0) + "|" +
        std::to_string(snapshot.pending_map_use_cursor) + "|" +
        std::to_string(snapshot.expected_map_uuid) + "|" +
        std::to_string(snapshot.artwork_bounds.min_x) + "|" +
        std::to_string(snapshot.artwork_bounds.min_y) + "|" +
        std::to_string(snapshot.artwork_bounds.min_z) + "|" +
        std::to_string(snapshot.artwork_bounds.max_x) + "|" +
        std::to_string(snapshot.artwork_bounds.max_y) + "|" +
        std::to_string(snapshot.artwork_bounds.max_z) + "|" +
        std::to_string(snapshot.extra_chests.size()) + "|" +
        std::to_string(static_cast<unsigned>(decision.next)) + "|" +
        std::to_string(decision.chest_address.chest_index) + "|" +
        std::to_string(decision.chest_address.slot) + "|" +
        std::to_string(decision.expected_title.size()) + ":" +
        decision.expected_title + "|" +
        // Prepared chest refresh is a durable, non-dispatching update that
        // keeps the same classified step. Its new live capture/source permit
        // one new preflight. Other request IDs intentionally do not unlock
        // a transition latch after an ambiguous send.
        (decision.next == MapStorageNextStep::PreflightChestTransfer &&
         snapshot.chest ?
             std::to_string(snapshot.chest->source_network_stack_id) + "|" +
             std::to_string(snapshot.chest->pre_send_capture_token)
             : std::string());
}

}  // namespace

MapStorageDriveResult MapStoragePipelineDriver::tick(
        const MapStorageCoordinatorInput& snapshot,
        const MapStoragePipelineOps& ops) {
    MapStorageDriveResult result;
    if (!ClassifyMapStorageCoordinator(snapshot, &result.decision,
                                       &result.error)) {
        result.status = MapStorageDriveStatus::Unsafe;
        return result;
    }
    const MapStorageNextStep step = result.decision.next;
    if (step == MapStorageNextStep::Complete) {
        result.status = MapStorageDriveStatus::Complete;
        return result;
    }
    if (step == MapStorageNextStep::AwaitMapUseMarker) {
        // Map creation and its marker are owned by the outer import runtime,
        // not by storage callbacks. Never reuse the previous map UUID here.
        result.status = MapStorageDriveStatus::WaitingForEvidence;
        return result;
    }
    if (step == MapStorageNextStep::Unsafe) {
        result.status = MapStorageDriveStatus::Unsafe;
        result.error = "map storage classifier returned an unsafe step";
        return result;
    }

    const bool reconcile = isReconcileOnly(step);
    const MapStorageStepCallback callback = reconcile
        ? ops.reconcile_without_resend : ops.transition;
    if (!callback) {
        result.status = MapStorageDriveStatus::WaitingForHandler;
        return result;
    }
    const std::string key = transitionKey(snapshot, result.decision);
    if (!reconcile && key == last_transition_key_) {
        result.status = MapStorageDriveStatus::AwaitingJournalRefresh;
        return result;
    }

    MapStorageExternalResult external;
    try {
        external = callback(ops.context, step, snapshot,
                            result.decision, &result.error);
    } catch (...) {
        // A transition callback could have entered sendToServer before it
        // threw. Treat it as ambiguous, never as a retryable preflight error.
        if (!reconcile) last_transition_key_ = key;
        result.status = reconcile ? MapStorageDriveStatus::Unsafe
                                  : MapStorageDriveStatus::OneTransitionAttempted;
        result.error = reconcile
            ? "map storage reconciliation callback threw"
            : "map storage transition outcome is unknown; reload journal and reconcile";
        return result;
    }
    switch (external) {
        case MapStorageExternalResult::NoChange:
            result.status = MapStorageDriveStatus::WaitingForEvidence;
            break;
        case MapStorageExternalResult::DurableTransition:
        case MapStorageExternalResult::DispatchOutcomeUnknown:
            // Reconcile callback never dispatches, but both statuses still
            // require the caller to reload the journal before proceeding.
            if (!reconcile) last_transition_key_ = key;
            result.status = MapStorageDriveStatus::OneTransitionAttempted;
            break;
        case MapStorageExternalResult::ErrorBeforeDispatch:
            result.status = MapStorageDriveStatus::FailedBeforeDispatch;
            if (result.error.empty()) {
                result.error = "map storage callback failed before dispatch";
            }
            break;
    }
    return result;
}

}  // namespace build_import
