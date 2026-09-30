#include "../MapNativeAnvilResultActionBridge.h"
#include "../MapTileNaming.h"

#include <cassert>
#include <string>

int main() {
    using namespace build_import;
    assert(!IsMapNativeAnvilAutomaticResultDispatchVerified());
    assert(BeforeMapNativeAnvilResultOutboundPacket(nullptr));
    assert(ClassifyMapNativeAnvilOutbound(false, false, false) ==
           MapNativeAnvilOutboundDisposition::PassUnrelated);
    assert(ClassifyMapNativeAnvilOutbound(false, true, true) ==
           MapNativeAnvilOutboundDisposition::PassUnrelated);
    assert(ClassifyMapNativeAnvilOutbound(true, false, false) ==
           MapNativeAnvilOutboundDisposition::PassUnrelated);
    assert(ClassifyMapNativeAnvilOutbound(true, true, true) ==
           MapNativeAnvilOutboundDisposition::SuppressScopedUnexpected);
    assert(ClassifyMapNativeAnvilOutbound(true, true, false) ==
           MapNativeAnvilOutboundDisposition::ExamineScopedRequest);
    MapAnvilRenameRecord record{};
    record.phase = MapAnvilRenamePhase::InputConfirmed;
    record.world_id = "world-1";
    record.dimension_id = 0;
    record.tile_cursor = 0;
    record.columns = 2;
    record.rows = 2;
    record.anvil_x = 10;
    record.anvil_y = 64;
    record.anvil_z = -4;
    record.map_runtime_item_id = 358;
    record.map_uuid = 4567;
    record.input_request_id = -3;
    record.input_response.session_generation = 2;
    record.input_response.window_token = 99;
    record.input_response.window_id = 7;
    assert(FormatMapTileName(record.tile_cursor, record.columns,
                             record.rows, &record.expected_title));

    MapAnvilInputProof proof{};
    proof.world_id = record.world_id;
    proof.dimension_id = record.dimension_id;
    proof.anvil_x = record.anvil_x;
    proof.anvil_y = record.anvil_y;
    proof.anvil_z = record.anvil_z;
    proof.fresh_window_token = record.input_response.window_token;
    proof.window_id = record.input_response.window_id;
    proof.input_slot = 1;
    proof.count = 1;
    proof.runtime_item_id = record.map_runtime_item_id;
    proof.network_stack_id = 7;
    proof.map_uuid = record.map_uuid;
    proof.native_readback = true;

    ContainerCaptureResult window{};
    window.token = proof.fresh_window_token;
    window.x = record.anvil_x;
    window.y = record.anvil_y;
    window.z = record.anvil_z;
    window.container_id = proof.window_id;
    window.container_type = 5;
    window.container_opened = true;
    window.slot_count = 3;

    ProjectionPrinterNativeAnvilInputMapSnapshot input{};
    input.runtime_item_id = record.map_runtime_item_id;
    input.network_stack_id = proof.network_stack_id;
    input.count = 1;
    input.map_uuid = record.map_uuid;
    input.item_identifier = "minecraft:filled_map";

    ProjectionPrinterNativeAnvilPreviewMapSnapshot preview{};
    preview.runtime_item_id = record.map_runtime_item_id;
    preview.count = 1;
    preview.map_uuid = record.map_uuid;
    preview.item_identifier = "minecraft:filled_map";
    preview.display_name = record.expected_title;

    std::string title, error;
    assert(ValidateMapNativeAnvilResultActionGate(
        record, proof, window, input, preview, &title, &error));
    assert(title == record.expected_title && error.empty());

    preview.display_name += " ";
    assert(!ValidateMapNativeAnvilResultActionGate(
        record, proof, window, input, preview, &title, &error));
    preview.display_name = record.expected_title;
    preview.map_uuid++;
    assert(!ValidateMapNativeAnvilResultActionGate(
        record, proof, window, input, preview, &title, &error));
    preview.map_uuid--;
    input.network_stack_id++;
    assert(!ValidateMapNativeAnvilResultActionGate(
        record, proof, window, input, preview, &title, &error));
    input.network_stack_id--;
    proof.window_id++;
    assert(!ValidateMapNativeAnvilResultActionGate(
        record, proof, window, input, preview, &title, &error));
    proof.window_id--;
    window.container_closed = true;
    assert(!ValidateMapNativeAnvilResultActionGate(
        record, proof, window, input, preview, &title, &error));
    window.container_closed = false;
    record.phase = MapAnvilRenamePhase::CraftDispatchArmed;
    assert(!ValidateMapNativeAnvilResultActionGate(
        record, proof, window, input, preview, &title, &error));
    return 0;
}
