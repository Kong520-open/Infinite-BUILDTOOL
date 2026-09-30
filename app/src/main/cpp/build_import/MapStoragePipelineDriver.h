#ifndef INFINITE_TEXTURE_MAP_STORAGE_PIPELINE_DRIVER_H
#define INFINITE_TEXTURE_MAP_STORAGE_PIPELINE_DRIVER_H

#include "MapStorageCoordinator.h"

#include <cstdint>
#include <string>

namespace build_import {

// NoChange and ErrorBeforeDispatch both require that no durable state changed
// and no packet send was invoked. Once Armed was persisted, the callback must
// return DurableTransition or DispatchOutcomeUnknown, even if the native send
// function reported failure. It must persist Armed BEFORE invoking send.
enum class MapStorageExternalResult : uint8_t {
    NoChange,
    DurableTransition,
    DispatchOutcomeUnknown,
    ErrorBeforeDispatch,
};

using MapStorageStepCallback = MapStorageExternalResult (*)(
    void* context, MapStorageNextStep step,
    const MapStorageCoordinatorInput& snapshot,
    const MapStorageCoordinatorDecision& decision,
    std::string* error);

struct MapStoragePipelineOps {
    void* context = nullptr;
    // For Await/Survey/Preflight/Commit/Clear. Survey and Preflight callbacks
    // may dispatch only after their phase-specific durable Armed write.
    MapStorageStepCallback transition = nullptr;
    // For Armed/ACK and chest-reopen states only. This callback may obtain
    // fresh evidence and advance a journal, but MUST NOT resend the original
    // placement, ItemStackRequest, or other non-idempotent packet.
    MapStorageStepCallback reconcile_without_resend = nullptr;
};

enum class MapStorageDriveStatus : uint8_t {
    Unsafe,
    WaitingForHandler,
    WaitingForEvidence,
    AwaitingJournalRefresh,
    OneTransitionAttempted,
    FailedBeforeDispatch,
    Complete,
};

struct MapStorageDriveResult {
    MapStorageDriveStatus status = MapStorageDriveStatus::Unsafe;
    MapStorageCoordinatorDecision decision;
    std::string error;
};

// Inert until a production runtime creates and ticks this object. It neither
// loads journals nor sends packets. The caller supplies a freshly loaded
// snapshot on every tick. Exactly one external callback is called per tick.
// After any transition callback reports a durable/ambiguous action, an
// identical snapshot cannot trigger another transition on this instance.
class MapStoragePipelineDriver final {
public:
    MapStorageDriveResult tick(const MapStorageCoordinatorInput& snapshot,
                               const MapStoragePipelineOps& ops);

private:
    std::string last_transition_key_;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_STORAGE_PIPELINE_DRIVER_H
