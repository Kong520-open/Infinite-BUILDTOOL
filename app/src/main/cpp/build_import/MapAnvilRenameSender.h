#ifndef INFINITE_TEXTURE_MAP_ANVIL_RENAME_SENDER_H
#define INFINITE_TEXTURE_MAP_ANVIL_RENAME_SENDER_H

#include "MapAnvilRecipeProbeState.h"
#include "ProjectionPrinterInventoryMailbox.h"

#include <cstdint>
#include <string>

namespace build_import {

// These structures describe two distinct moments. A map is held in the
// hotbar before input Place; the recipe getter can only be observed after the
// map has arrived in the anvil input. Do not require both states at once.
struct MapAnvilRenameTask {
    std::string expected_world_context;
    uintptr_t expected_dimension_token = 0;
    uint64_t expected_session_generation = 0;
    uint64_t window_token = 0;
    uint64_t recipe_ticket = 0;
    uint8_t expected_window_id = 0;
    int32_t anvil_x = 0;
    int32_t anvil_y = 0;
    int32_t anvil_z = 0;
    int32_t source_hotbar_slot = -1;
    int32_t source_network_stack_id = 0;
    int32_t expected_runtime_item_id = 0;
    int64_t expected_map_uuid = -1;
    uint64_t tile_cursor = 0;
    uint64_t columns = 0;
    uint64_t rows = 0;
};

struct MapAnvilRenameWorldEvidence {
    std::string world_context;
    uintptr_t dimension_token = 0;
    std::string block_identifier;
};

// A fresh, unsuppressed ContainerOpen + InventoryContent observation, paired
// with a native readback of the block at the packet's coordinates. The manual
// trace for this SO build established type 5 and a three-slot window. The
// InventoryContent FullContainerName for anvil remains unverified and is not
// inferred from the window ID.
struct MapAnvilRenameWindowEvidence {
    uint64_t session_generation = 0;
    uint64_t token = 0;
    uint8_t window_id = 0;
    uint8_t container_type = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    bool opened = false;
    bool closed = false;
    bool content_observed = false;
    uint32_t slot_count = 0;
    bool input_slot_empty = false;
};

struct MapAnvilHeldMapEvidence {
    int32_t selected_hotbar_slot = -1;
    int32_t network_stack_id = 0;
    int32_t runtime_item_id = 0;
    uint16_t count = 0;
    std::string item_identifier;
    bool has_map_uuid = false;
    int64_t map_uuid = -1;
};

// Semantic plan for the *observed* first request only: one Place of a held
// map from Hotbar(29) to anvil input Container(0), slot 1. This plan cannot
// itself send a packet. A future sender must use the verified native ABI,
// reserve a unique request ID, and journal before send.
struct MapAnvilInputDraft {
    uint8_t source_container = 29;
    uint8_t source_slot = 0;
    int32_t source_network_stack_id = 0;
    uint8_t destination_container = 0;
    uint8_t destination_slot = 1;
    uint8_t amount = 1;
    std::string expected_title;
};

bool PrepareMapAnvilInputDraft(const MapAnvilRenameTask& task,
                               const MapAnvilRenameWorldEvidence& world,
                               const MapAnvilRenameWindowEvidence& window,
                               const MapAnvilHeldMapEvidence& held,
                               MapAnvilInputDraft* draft,
                               std::string* error = nullptr);

using MapAnvilInputPreSend = bool (*)(int32_t request_id, void* context,
                                      std::string* error);

struct MapAnvilInputPlaceRequest {
    MapAnvilRenameTask task;
    // Must durably transition the matching Prepared journal to
    // InputDispatchArmed(request_id) before returning true. A failed or
    // throwing callback prevents sendToServer from being called.
    MapAnvilInputPreSend pre_send = nullptr;
    void* pre_send_context = nullptr;
};

struct MapAnvilInputSubmission {
    // A nonzero ID means the request was submitted, never that the server
    // accepted it or that the anvil input now contains the map.
    int32_t request_id = 0;
    uint64_t response_session_generation = 0;
    uint64_t response_generation_before_send = 0;
    // If sendToServer threw after entry, the outcome must be reconciled from
    // fresh server/native evidence. Never resend solely because of this bit.
    bool submission_outcome_uncertain = false;
};

// Host-testable final journal gate. The native sender reserves an independent
// negative odd request ID, then calls this synchronously before sendToServer.
bool ArmMapAnvilInputDispatchBeforeSend(
    const MapAnvilInputPlaceRequest& request, int32_t request_id,
    std::string* error = nullptr);

// One native Place: held Hotbar(29) -> already-open, captured anvil Container
// (0), slot 1. The game-thread sender obtains fresh world/window/ItemStack
// evidence itself, including the exact map UUID, and never opens a screen.
// No runtime path currently calls this. Its result is pending until the exact
// server ItemStackResponse is matched and the still-open anvil window is
// correlated with that same map. Closing an anvil may return its input item,
// so a close/reopen cycle is not an input verification step.
bool SendMapAnvilInputPlace(const MapAnvilInputPlaceRequest& request,
                            MapAnvilInputSubmission* submission,
                            std::string* error = nullptr);

// Confirms only server acceptance of the exact one-action Place, not that
// the map is still in the anvil. Keep the same window open; closing it can
// return the input map. Craft dispatch needs separately correlated live
// input evidence before it may consume this accepted response.
bool ValidateMapAnvilInputAcceptedResponse(
    const MapAnvilInputPlaceRequest& request,
    const MapAnvilInputSubmission& submission,
    const ProjectionPrinterInventoryResponse& response,
    std::string* error = nullptr);

struct MapAnvilCraftSubmission {
    int32_t request_id = 0;
    uint64_t response_session_generation = 0;
    uint64_t response_generation_before_send = 0;
    uint64_t window_token = 0;
    uint8_t window_id = 0;
    // The game's current result-click request, never a cached recipe sample.
    uint32_t dynamic_recipe_network_id = 0;
    int32_t consumed_input_network_stack_id = 0;
    // Taken from the native Place action (container 12), not assumed to be 0.
    int32_t destination_hotbar_slot = -1;
};

// Checks only whether the server accepted the exact result-collection request
// and reported the observed anvil input/output slot transition with the
// expected full UTF-8 tile title. The response has no map UUID or trustworthy
// final item identity; a separate native ItemStack readback must still
// confirm the exact UUID and DisplayName before the map may be stored.
bool ValidateMapAnvilCraftAcceptedResponse(
    const MapAnvilRenameTask& task,
    const MapAnvilCraftSubmission& submission,
    const ProjectionPrinterInventoryResponse& response,
    std::string* error = nullptr,
    int32_t* accepted_output_network_stack_id = nullptr);

struct MapAnvilInputMapEvidence {
    uint64_t session_generation = 0;
    uint64_t window_token = 0;
    uint8_t window_id = 0;
    uint8_t slot = 1;
    int32_t runtime_item_id = 0;
    // Live input-slot net ID may differ from the pre-send hotbar net ID.
    int32_t network_stack_id = 0;
    uint16_t count = 0;
    bool has_map_uuid = false;
    int64_t map_uuid = -1;
};

// The former map-items getter probe was shown not to run during manual anvil
// rename. Until the actual rename recipe source is identified, even an
// Observed result from that probe cannot authorize this draft. A manual
// sample ID (e.g. 3304) is never an acceptable default. Consume, temporary
// output SlotInfo, and packet serialization also remain unverified.
struct MapAnvilCraftDraft {
    uint32_t dynamic_recipe_network_id = 0;
    std::string expected_title;
};

bool PrepareMapAnvilCraftDraft(const MapAnvilRenameTask& task,
                               const MapAnvilRenameWorldEvidence& world,
                               const MapAnvilRenameWindowEvidence& window,
                               const MapAnvilInputMapEvidence& input,
                               MapAnvilRecipeProbeStatus probe_status,
                               const MapAnvilRecipeObservation& recipe,
                               MapAnvilCraftDraft* draft,
                               std::string* error = nullptr);

// Explicit fail-closed dispatch boundary. Until the native Consume action,
// temporary output SlotInfo/network ID and rename-cost behavior are proven,
// no automatic anvil request is emitted.
bool SendMapAnvilRename(const MapAnvilCraftDraft& draft,
                        std::string* error = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_ANVIL_RENAME_SENDER_H
