#pragma once

#include "MapAnvilRenameJournal.h"
#include "MapAnvilRenameSender.h"
#include "MapVisibleAnvilWindowSession.h"
#include "ProjectionPrinterInventoryMover.h"

#include <cstdint>
#include <string>

namespace build_import {

struct MapNativeAnvilResultActionRequest {
    const MapAnvilRenameRecord* confirmed = nullptr;
    const MapAnvilInputProof* input = nullptr;
    // The bridge fsyncs CraftClickArmed before invoking the original UI
    // handler and fsyncs CraftDispatchArmed at the later outbound send hook.
    std::string map_state_path;
    uint64_t now_ms = 0;
};

struct MapNativeAnvilResultActionOutcome {
    MapAnvilCraftSubmission submission{};
    uint32_t dynamic_recipe_network_id = 0;
    // True only when sendToServer was entered synchronously. The original
    // handler can queue its packet for a later tick; in that case Submit may
    // succeed with this still false, and the journal is authoritative.
    bool send_invoked = false;
    bool outcome_uncertain = false;
};

enum class MapNativeAnvilOutboundDisposition : uint8_t {
    PassUnrelated,
    SuppressScopedUnexpected,
    ExamineScopedRequest,
};

// Pure policy: absent intent never affects unrelated traffic. While a click
// is armed, non-ItemStackRequest traffic passes; another ItemStackRequest is
// examined once or suppressed after the one dispatch attempt.
MapNativeAnvilOutboundDisposition ClassifyMapNativeAnvilOutbound(
    bool intent_active, bool is_item_stack_request,
    bool request_already_seen) noexcept;

// Runtime can query this before moving a map into the anvil input. At present
// it deliberately returns false even for Debug builds.
bool IsMapNativeAnvilAutomaticResultDispatchVerified() noexcept;

// Pure gate over already-captured server window and double-read native item
// evidence. The preview is client-predicted and cannot substitute for ACK.
bool ValidateMapNativeAnvilResultActionGate(
    const MapAnvilRenameRecord& record,
    const MapAnvilInputProof& proof,
    const ContainerCaptureResult& live_window,
    const ProjectionPrinterNativeAnvilInputMapSnapshot& input,
    const ProjectionPrinterNativeAnvilPreviewMapSnapshot& preview,
    std::string* title, std::string* error = nullptr);

// Bridge to the game's own AnvilScreenController result action. No synthetic
// Craft/Consume/Place packet is emitted. A true result means its one-shot click
// was durably armed and the original handler invoked; packet dispatch can be
// asynchronous, and ACK/final map UUID must still be reconciled separately.
bool SubmitMapNativeAnvilResultAction(
    uintptr_t minecraft_base,
    const MapNativeAnvilResultActionRequest& request,
    const MapVisibleAnvilWindowSession& session,
    MapNativeAnvilResultActionOutcome* outcome,
    std::string* error = nullptr);

// Called by the existing sender-only and legacy sender hooks before forwarding
// to the original function. It durably arms one exact asynchronous result
// request before sendToServer, or suppresses an ambiguous request.
bool BeforeMapNativeAnvilResultOutboundPacket(const void* packet) noexcept;

}  // namespace build_import
