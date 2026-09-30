#include "../MapChestStorageVerification.h"

#include <cassert>

using namespace build_import;

int main() {
    ContainerCaptureResult capture;
    capture.token = 42;
    capture.x = 235;
    capture.y = 22;
    capture.z = 126;
    capture.container_id = 3;
    capture.container_type = 0;
    capture.container_opened = true;
    capture.slot_count = 27;
    capture.has_full_container_name = true;
    capture.full_container_name = 0;
    CapturedContainerItem map;
    map.slot = 0;
    map.numeric_id = 358;
    map.count = 1;
    map.has_network_stack_id = true;
    map.network_stack_id = 76;
    map.has_map_uuid = true;
    map.map_uuid = -532575944698LL;
    capture.items.push_back(map);

    assert(VerifyFilledMapInReopenedChest(capture, 42, 235, 22, 126, 0,
                                           map.map_uuid, 358) ==
           FilledMapChestReopenResult::Confirmed);
    const std::string title = "地图 1行1列";
    assert(VerifyNamedFilledMapInReopenedChest(
               capture, 42, 235, 22, 126, 0, map.map_uuid, 358,
               title, MapItemNameSource::DisplayName) ==
           NamedFilledMapChestReopenResult::NameUnconfirmed);
    capture.items[0].name_status = MapItemNameStatus::Present;
    capture.items[0].name_source = MapItemNameSource::DisplayName;
    capture.items[0].name_candidate = title;
    assert(VerifyNamedFilledMapInReopenedChest(
               capture, 42, 235, 22, 126, 0, map.map_uuid, 358,
               title, MapItemNameSource::DisplayName) ==
           NamedFilledMapChestReopenResult::Confirmed);
    assert(VerifyNamedFilledMapInReopenedChest(
               capture, 42, 235, 22, 126, 0, map.map_uuid, 358,
               "地图 1行2列", MapItemNameSource::DisplayName) ==
           NamedFilledMapChestReopenResult::NameUnconfirmed);
    assert(VerifyNamedFilledMapInReopenedChest(
               capture, 42, 235, 22, 126, 0, map.map_uuid, 358,
               title, MapItemNameSource::RootCustomName) ==
           NamedFilledMapChestReopenResult::NameUnconfirmed);
    assert(VerifyFilledMapInReopenedChest(capture, 43, 235, 22, 126, 0,
                                           map.map_uuid, 358) ==
           FilledMapChestReopenResult::InvalidCapture);
    assert(VerifyFilledMapInReopenedChest(capture, 42, 235, 22, 126, 1,
                                           map.map_uuid, 358) ==
           FilledMapChestReopenResult::MissingOrDifferentItem);
    capture.container_closed = true;
    assert(VerifyFilledMapInReopenedChest(capture, 42, 235, 22, 126, 0,
                                           map.map_uuid, 358) ==
           FilledMapChestReopenResult::InvalidCapture);
    capture.container_closed = false;
    capture.items.push_back(map);
    capture.items.back().slot = 1;
    assert(VerifyFilledMapInReopenedChest(capture, 42, 235, 22, 126, 0,
                                           map.map_uuid, 358) ==
           FilledMapChestReopenResult::MissingOrDifferentItem);
    capture.items.pop_back();
    capture.items[0].numeric_id = 359;
    assert(VerifyFilledMapInReopenedChest(capture, 42, 235, 22, 126, 0,
                                           map.map_uuid, 358) ==
           FilledMapChestReopenResult::MissingOrDifferentItem);
    capture.items[0].numeric_id = 358;
    capture.items[0].has_map_uuid = false;
    assert(VerifyFilledMapInReopenedChest(capture, 42, 235, 22, 126, 0,
                                           map.map_uuid, 358) ==
           FilledMapChestReopenResult::MissingOrDifferentItem);
    return 0;
}
