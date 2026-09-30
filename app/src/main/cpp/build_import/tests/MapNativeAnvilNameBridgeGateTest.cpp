#include "../MapNativeAnvilNameBridge.h"
#include "../MapTileNaming.h"

#include <cassert>
#include <string>

int main() {
    using namespace build_import;

    MapNativeAnvilNameRequest request{};
    request.ticket = 37;
    request.expected_window_id = 7;
    request.x = 12;
    request.y = 64;
    request.z = -5;
    request.tile_cursor = 4;
    request.columns = 3;
    request.rows = 2;
    assert(FormatMapTileName(request.tile_cursor, request.columns,
                             request.rows, &request.expected_title));
    assert(request.expected_title == "地图 2行2列");

    ContainerCaptureResult capture{};
    capture.token = request.ticket;
    capture.x = request.x;
    capture.y = request.y;
    capture.z = request.z;
    capture.container_id = 7;
    capture.container_type = 5;
    capture.container_opened = true;
    capture.slot_count = 3;
    std::string validated;
    std::string error;
    assert(ValidateMapNativeAnvilNameGate(
        request, MapVisibleAnvilWindowState::Open,
        &capture, &validated, &error));
    assert(validated == request.expected_title && error.empty());

    capture.container_id++;
    assert(!ValidateMapNativeAnvilNameGate(
        request, MapVisibleAnvilWindowState::Open,
        &capture, &validated, &error));
    capture.container_id--;

    capture.token++;
    assert(!ValidateMapNativeAnvilNameGate(
        request, MapVisibleAnvilWindowState::Open,
        &capture, &validated, &error));
    capture.token--;
    capture.container_type = 0;
    assert(!ValidateMapNativeAnvilNameGate(
        request, MapVisibleAnvilWindowState::Open,
        &capture, &validated, &error));
    capture.container_type = 5;
    capture.slot_count = 2;
    assert(!ValidateMapNativeAnvilNameGate(
        request, MapVisibleAnvilWindowState::Open,
        &capture, &validated, &error));
    capture.slot_count = 3;
    capture.container_closed = true;
    assert(!ValidateMapNativeAnvilNameGate(
        request, MapVisibleAnvilWindowState::Open,
        &capture, &validated, &error));
    capture.container_closed = false;
    assert(!ValidateMapNativeAnvilNameGate(
        request, MapVisibleAnvilWindowState::WaitingForContent,
        &capture, &validated, &error));

    request.expected_title += " ";
    assert(!ValidateMapNativeAnvilNameGate(
        request, MapVisibleAnvilWindowState::Open,
        &capture, &validated, &error));
    return 0;
}
