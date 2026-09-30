#ifndef INFINITE_TEXTURE_MAP_STORAGE_ANVIL_ADAPTER_H
#define INFINITE_TEXTURE_MAP_STORAGE_ANVIL_ADAPTER_H

#include "MapAnvilRenameSender.h"
#include "MapStoragePipelineDriver.h"

#include <cstdint>
#include <string>

namespace build_import {

struct MapStorageAnvilCraftPreview {
    bool native_readback = false;
    uint32_t dynamic_recipe_network_id = 0;
    int64_t input_map_uuid = -1;
    int64_t output_map_uuid = -1;
    std::string output_title;
};

// The runtime owns the visible anvil session. Every callback must obtain a
// fresh, same-world observation; no native window pointer may be cached here.
// A craft callback must durably arm the click before invoking the native
// result handler. That handler may enqueue its ItemStackRequest for a later
// game tick; the outbound hook durably arms the exact request before send.
struct MapStorageAnvilHooks {
    void* context = nullptr;
    // The production implementation must read the current-session mailbox.
    // Explicit callbacks also let host tests prove stale/rejected responses
    // cannot advance a durable journal. Missing callbacks fail closed.
    bool (*read_response_baseline)(void*, uint64_t* session_generation,
                                   uint64_t* response_generation,
                                   std::string*) = nullptr;
    bool (*read_exact_response)(void*, int32_t request_id,
                                ProjectionPrinterInventoryResponse*,
                                std::string*) = nullptr;
    bool (*read_world)(void*, const std::string&, int32_t,
                       int32_t, int32_t, int32_t,
                       MapAnvilRenameWorldEvidence*, std::string*) = nullptr;
    bool (*read_held_map)(void*, MapAnvilHeldMapEvidence*,
                          std::string*) = nullptr;
    bool (*ensure_visible_window)(void*, int32_t, int32_t, int32_t,
                                  MapAnvilRenameWindowEvidence*,
                                  std::string*) = nullptr;
    // Used after the one input Place has been armed. This must only inspect
    // the existing window; reopening can return the map from the input slot.
    bool (*read_visible_window)(void*, int32_t, int32_t, int32_t,
                                MapAnvilRenameWindowEvidence*,
                                std::string*) = nullptr;
    // This must inspect the native anvil input ItemStack, not infer it from
    // the response packet or an untrusted InventorySlot parse.
    bool (*read_native_input)(void*, const MapAnvilRenameWindowEvidence&,
                              MapAnvilInputProof*, std::string*) = nullptr;
    // Only for an already durably accepted Place whose native input is still
    // positively empty. May schedule one local client presentation refresh;
    // it never sends a second inventory request or counts as input proof.
    bool (*sync_accepted_client_input)(
        void*, const MapAnvilRenameRecord&,
        const MapAnvilRenameWindowEvidence&, std::string*) = nullptr;
    bool (*submit_input)(void*, const MapAnvilInputPlaceRequest&,
                         MapAnvilInputSubmission*, std::string*) = nullptr;
    // Original AnvilScreenController text callback. Success is only UI text
    // submission, not evidence that the map has been crafted or collected.
    bool (*submit_name_text)(void*, const MapAnvilRenameWindowEvidence&,
                             const MapAnvilRenameRecord&,
                             std::string*) = nullptr;
    bool (*read_native_craft_preview)(void*,
                                      const MapAnvilRenameWindowEvidence&,
                                      MapStorageAnvilCraftPreview*,
                                      std::string*) = nullptr;
    // Schedules the game's native result action after fsyncing ClickArmed.
    // The actual ItemStackRequest can be emitted asynchronously; this hook
    // must not retain pointers to this call's stack or resubmit a click.
    bool (*submit_craft_and_collect)(void*, const MapAnvilRenameRecord&,
                                      const MapAnvilRenameWindowEvidence&,
                                      const MapAnvilInputProof&,
                                      const MapStorageAnvilCraftPreview&,
                                      std::string*) = nullptr;
    // After collection, scan all 36 native player-inventory ItemStacks for
    // one unique UUID + exact DisplayName; the UI preview is not final proof.
    // Chest transfer separately requires that confirmed map in selected hotbar.
    bool (*read_native_output)(void*, MapAnvilOutputProof*,
                               std::string*) = nullptr;
};

struct MapStorageAnvilAdapter {
    std::string map_state_path;
    MapStorageAnvilHooks hooks;
};

bool IsMapStorageAnvilStep(MapStorageNextStep step) noexcept;

// These callbacks can be composed into MapStoragePipelineOps by dispatching
// only IsMapStorageAnvilStep() states here. The production storage callbacks
// continue to own chest placement, transfer and cursor commit.
MapStorageExternalResult RunMapStorageAnvilTransition(
    MapStorageAnvilAdapter* adapter, MapStorageNextStep step,
    const MapStorageCoordinatorInput& snapshot,
    const MapStorageCoordinatorDecision& decision,
    std::string* error = nullptr);
MapStorageExternalResult RunMapStorageAnvilReconcileNoResend(
    MapStorageAnvilAdapter* adapter, MapStorageNextStep step,
    const MapStorageCoordinatorInput& snapshot,
    const MapStorageCoordinatorDecision& decision,
    std::string* error = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_STORAGE_ANVIL_ADAPTER_H
