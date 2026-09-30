#include "ContainerCaptureMailbox.h"
#include "ContainerEntityCodec.h"
#include "ItemRuntimeRegistry.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

void varUInt(std::string* output, uint32_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7FU);
        value >>= 7U;
        if (value != 0U) byte |= 0x80U;
        output->push_back(static_cast<char>(byte));
    } while (value != 0U);
}

void varUInt64(std::string* output, uint64_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7FU);
        value >>= 7U;
        if (value != 0U) byte |= 0x80U;
        output->push_back(static_cast<char>(byte));
    } while (value != 0U);
}

void varInt(std::string* output, int32_t value) {
    const uint32_t encoded = (static_cast<uint32_t>(value) << 1U) ^
        static_cast<uint32_t>(value >> 31);
    varUInt(output, encoded);
}

void varInt64(std::string* output, int64_t value) {
    const uint64_t encoded = (static_cast<uint64_t>(value) << 1U) ^
        static_cast<uint64_t>(value >> 63);
    varUInt64(output, encoded);
}

std::string openPacket(uint8_t window, uint8_t type,
                       int32_t x, int32_t y, int32_t z) {
    std::string packet;
    varUInt(&packet, 0x2EU);
    packet.push_back(static_cast<char>(window));
    packet.push_back(static_cast<char>(type));
    varInt(&packet, x);
    varUInt(&packet, static_cast<uint32_t>(y));
    varInt(&packet, z);
    varInt64(&packet, -1);
    return packet;
}

void emptyItem(std::string* output) {
    varInt(output, 0);
}

void item(std::string* output, int32_t numeric_id, uint16_t count,
          uint32_t damage, bool has_network_id, int32_t block_runtime_id,
          const std::string& user_data) {
    varInt(output, numeric_id);
    output->push_back(static_cast<char>(count & 0xFFU));
    output->push_back(static_cast<char>((count >> 8U) & 0xFFU));
    varUInt(output, damage);
    output->push_back(has_network_id ? 1 : 0);
    if (has_network_id) varInt(output, 91);
    varInt(output, block_runtime_id);
    varUInt(output, static_cast<uint32_t>(user_data.size()));
    output->append(user_data);
}

void currentInventoryTail(std::string* output) {
    // Protocol v859 inherits the v748 InventoryContent tail:
    // FullContainerName plus a storage ItemData.
    varUInt(output, 0U);
    output->push_back(0);
    emptyItem(output);
}

std::string inventoryPacket(uint32_t window) {
    std::string packet;
    varUInt(&packet, 0x31U);
    varUInt(&packet, window);
    varUInt(&packet, 27U);
    for (uint32_t slot = 0; slot < 27U; ++slot) {
        if (slot == 0U) {
            item(&packet, 5, 64, 7, true, 0, "nbt-ignored");
        } else if (slot == 10U) {
            item(&packet, 276, 1, 23, false, -1, {});
        } else {
            emptyItem(&packet);
        }
    }
    currentInventoryTail(&packet);
    return packet;
}

void registryEntry(std::string* output, const std::string& name,
                   int16_t runtime_id) {
    varUInt(output, static_cast<uint32_t>(name.size()));
    output->append(name);
    const uint16_t encoded = static_cast<uint16_t>(runtime_id);
    output->push_back(static_cast<char>(encoded & 0xFFU));
    output->push_back(static_cast<char>((encoded >> 8U) & 0xFFU));
    output->push_back(0);
    varInt(output, 2);
    output->push_back(10);
    varUInt(output, 0);
    output->push_back(0);
}

std::string itemRegistryPacket() {
    std::string packet;
    varUInt(&packet, 0xA2U);
    varUInt(&packet, 2U);
    registryEntry(&packet, "minecraft:oak_planks", 5);
    registryEntry(&packet, "minecraft:diamond_sword", 276);
    return packet;
}

}  // namespace

int main() {
    using namespace build_import;

    constexpr uint64_t kToken = 805U;
    constexpr int32_t kX = -189;
    constexpr int32_t kY = -60;
    constexpr int32_t kZ = 61;
    ContainerCaptureResult capture;

    ClearItemRuntimeRegistry();
    ObserveItemRuntimeRegistryPacket(itemRegistryPacket());
    assert(IsItemRuntimeRegistryReady());

    ArmContainerCapture(kToken, kX, kY, kZ);
    assert(!ObserveContainerCapturePacket(openPacket(2, 0, kX + 1, kY, kZ)));
    assert(PollContainerCapture(kToken, &capture) ==
           ContainerCapturePollState::WaitingForOpen);

    assert(ObserveContainerCapturePacket(openPacket(2, 0, kX, kY, kZ)));
    assert(PollContainerCapture(kToken, &capture) ==
           ContainerCapturePollState::WaitingForContent);
    assert(capture.container_id == 2 && capture.container_type == 0 &&
           capture.container_opened && !capture.container_closed);

    assert(!ObserveContainerCapturePacket(inventoryPacket(3)));
    assert(PollContainerCapture(kToken, &capture) ==
           ContainerCapturePollState::WaitingForContent);
    assert(ObserveContainerCapturePacket(inventoryPacket(2)));
    assert(PollContainerCapture(kToken, &capture) ==
           ContainerCapturePollState::Ready);
    assert(capture.slot_count == 27U && capture.items.size() == 2U);

    std::string entity_json;
    std::string error;
    assert(encodeCapturedContainerItemsJson(capture, &entity_json, &error));
    assert(error.empty());
    assert(entity_json ==
           R"({"Items":[{"Slot":0,"Name":"minecraft:oak_planks","Count":64,"Damage":7},{"Slot":10,"Name":"minecraft:diamond_sword","Count":1,"Damage":23}]})");

    std::vector<ContainerItemRecord> restored;
    assert(parseContainerEntityJson(entity_json, &restored, &error));
    assert(restored.size() == 2U);
    assert(restored[0].slot == 0U && restored[0].item_id == "minecraft:oak_planks" &&
           restored[0].count == 64U && restored[0].aux == 7U);
    assert(restored[1].slot == 10U && restored[1].item_id == "minecraft:diamond_sword" &&
           restored[1].count == 1U && restored[1].aux == 23U);
    for (ContainerItemRecord& record : restored) {
        record.x = kX;
        record.y = kY;
        record.z = kZ;
    }
    assert(formatContainerReplaceItemCommand(restored[0]) ==
           "/replaceitem block -189 -60 61 slot.container 0 minecraft:oak_planks 64 7");
    assert(formatContainerReplaceItemCommand(restored[1]) ==
           "/replaceitem block -189 -60 61 slot.container 10 minecraft:diamond_sword 1 23");

    capture.slot_count = 54U;
    assert(!encodeCapturedContainerItemsJson(capture, &entity_json, &error));
    assert(error.find("double-chest") != std::string::npos);
    capture.slot_count = 27U;
    ClearItemRuntimeRegistry();
    assert(!encodeCapturedContainerItemsJson(capture, &entity_json, &error));
    assert(error.find("re-enter the world") != std::string::npos);
    CancelContainerCapture(kToken);
    return 0;
}
