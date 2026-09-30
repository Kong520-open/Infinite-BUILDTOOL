#ifndef INFINITE_TEXTURE_MAP_CHEST_STORAGE_SENDER_H
#define INFINITE_TEXTURE_MAP_CHEST_STORAGE_SENDER_H

#include "ContainerCaptureMailbox.h"
#include "ProjectionPrinterInventoryMailbox.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace build_import {

using FilledMapChestPreSend = bool (*)(int32_t request_id, void* context,
                                        std::string* error);

// The caller must retain this identity from the moment its newly placed chest
// is opened. The request is intentionally limited to one already-held,
// unstacked filled map and one empty slot in that exact single-chest window.
struct FilledMapToSingleChestRequest {
    std::string expected_world_context;
    uintptr_t expected_dimension_token = 0;
    uint64_t capture_token = 0;
    int32_t chest_x = 0;
    int32_t chest_y = 0;
    int32_t chest_z = 0;
    uint8_t expected_window_id = 0;
    uint8_t destination_slot = 0;
    int32_t source_hotbar_slot = -1;
    int32_t source_network_stack_id = 0;
    int32_t expected_runtime_item_id = 0;
    int64_t expected_map_uuid = -1;
    // Required synchronous durability barrier. The callback must persist
    // DispatchArmed(request_id) before returning true. False or an exception
    // prevents sendToServer from being called.
    FilledMapChestPreSend pre_send = nullptr;
    void* pre_send_context = nullptr;
};

// Pure evidence for a host-testable preflight. The native sender obtains each
// field afresh on the local-player game tick; callers cannot supply it to
// bypass the live checks.
struct FilledMapChestSourceEvidence {
    int32_t selected_hotbar_slot = -1;
    int32_t network_stack_id = 0;
    int32_t runtime_item_id = 0;
    uint16_t count = 0;
    std::string item_identifier;
    bool has_map_uuid = false;
    int64_t map_uuid = -1;
};

struct FilledMapChestWorldEvidence {
    std::string world_context;
    uintptr_t dimension_token = 0;
    std::string block_identifier;
};

bool ValidateFilledMapSingleChestPreflight(
    const FilledMapToSingleChestRequest& request,
    const ContainerCaptureResult& capture,
    const FilledMapChestSourceEvidence& source,
    const FilledMapChestWorldEvidence& world,
    std::string* error = nullptr);

// Host-testable final journal gate. Called only after the native Place packet
// has been constructed and validated, immediately before sendToServer.
bool ArmFilledMapChestDispatchBeforeSend(
    const FilledMapToSingleChestRequest& request, int32_t request_id,
    std::string* error = nullptr);

struct FilledMapChestSubmission {
    // A nonzero value means one native ItemStackRequest was submitted, not
    // that the server accepted it or that the chest contains the map.
    int32_t request_id = 0;
    uint64_t response_session_generation = 0;
    // Match responses by request ID AND current session generation, and
    // require response_generation strictly greater than this baseline.
    uint64_t response_generation_before_send = 0;
    // True only if sendToServer was entered but its return was interrupted.
    // The caller must reconcile the chest and source before any retry.
    bool submission_outcome_uncertain = false;
};

// Runs only on the live game's LocalPlayer tick. It creates exactly one
// independent native Place action with source Hotbar(29) and destination
// Container(7), dynamic ID absent, matching the observed manual transaction.
// It never retries and never reports storage success; the caller must match
// the ItemStackResponse and then re-open the chest to verify map_uuid.
bool SendFilledMapToSingleChest(
    const FilledMapToSingleChestRequest& request,
    FilledMapChestSubmission* submission,
    std::string* error = nullptr);

// Validate one already-correlated response entry. `expected_item_name` is the
// exact UTF-8 map title from the anvil step; production rename+storage must
// pass a nonempty value. This is still not storage confirmation: the caller
// must close and reopen the chest to verify its ItemStack and map UUID.
bool ValidateFilledMapChestAcceptedResponse(
    const FilledMapToSingleChestRequest& request,
    const FilledMapChestSubmission& submission,
    const ProjectionPrinterInventoryResponse& response,
    std::string_view expected_item_name,
    std::string* error = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_CHEST_STORAGE_SENDER_H
