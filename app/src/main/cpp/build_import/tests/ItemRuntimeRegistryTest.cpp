#include "ItemRuntimeRegistry.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
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
    varUInt(output, (static_cast<uint32_t>(value) << 1U) ^
                    static_cast<uint32_t>(value >> 31));
}

void varLong(std::string* output, int64_t value) {
    varUInt64(output, (static_cast<uint64_t>(value) << 1U) ^
                      static_cast<uint64_t>(value >> 63));
}

void networkString(std::string* output, const std::string& value) {
    varUInt(output, static_cast<uint32_t>(value.size()));
    output->append(value);
}

void namedTag(std::string* output, uint8_t type, const std::string& name) {
    output->push_back(static_cast<char>(type));
    networkString(output, name);
}

void emptyCompound(std::string* output) {
    output->push_back(10);
    networkString(output, {});
    output->push_back(0);
}

void complexCompound(std::string* output) {
    output->push_back(10);
    networkString(output, {});

    namedTag(output, 3, "version");
    varInt(output, -4);

    namedTag(output, 9, "labels");
    output->push_back(8);
    varInt(output, 2);
    networkString(output, "one");
    networkString(output, "two");

    namedTag(output, 7, "bytes");
    varInt(output, 3);
    output->append("abc", 3);

    namedTag(output, 11, "ints");
    varInt(output, 2);
    varInt(output, 7);
    varInt(output, -8);

    namedTag(output, 12, "longs");
    varInt(output, 1);
    varLong(output, -9);

    namedTag(output, 10, "nested");
    namedTag(output, 1, "flag");
    output->push_back(1);
    output->push_back(0);

    output->push_back(0);
}

void emptyArraysCompound(std::string* output) {
    output->push_back(10);
    networkString(output, {});

    namedTag(output, 7, "bytes");
    varInt(output, 0);

    namedTag(output, 9, "list");
    output->push_back(1);
    varInt(output, 0);

    namedTag(output, 11, "ints");
    varInt(output, 0);

    namedTag(output, 12, "longs");
    varInt(output, 0);

    output->push_back(0);
}

struct Entry {
    std::string name;
    int16_t runtime_id = 0;
    bool complex_nbt = false;
    int32_t version = 2;
    bool empty_arrays_nbt = false;
};

std::string registryPacket(const std::vector<Entry>& entries) {
    std::string packet;
    varUInt(&packet, 0xA2U);
    varUInt(&packet, static_cast<uint32_t>(entries.size()));
    for (const Entry& entry : entries) {
        networkString(&packet, entry.name);
        const uint16_t encoded = static_cast<uint16_t>(entry.runtime_id);
        packet.push_back(static_cast<char>(encoded & 0xFFU));
        packet.push_back(static_cast<char>((encoded >> 8U) & 0xFFU));
        packet.push_back(1);
        varInt(&packet, entry.version);
        if (entry.complex_nbt) complexCompound(&packet);
        else if (entry.empty_arrays_nbt) emptyArraysCompound(&packet);
        else emptyCompound(&packet);
    }
    return packet;
}

std::string packetWithId(uint32_t id) {
    std::string packet;
    varUInt(&packet, id);
    return packet;
}

}  // namespace

int main() {
    using namespace build_import;

    ClearItemRuntimeRegistry();
    assert(!IsItemRuntimeRegistryReady());
    std::string identifier;
    assert(ResolveItemRuntimeId(276, &identifier) ==
           ItemRuntimeResolveStatus::RegistryUnavailable);

    ObserveItemRuntimeRegistryPacket(registryPacket({
        {"minecraft:air", 0, false},
        {"minecraft:diamond_sword", 276, true},
        {"example:tools/hammer", -17, false},
    }));
    assert(IsItemRuntimeRegistryReady());
    assert(ItemRuntimeRegistrySize() == 3U);
    assert(ResolveItemRuntimeId(276, &identifier) == ItemRuntimeResolveStatus::Ready);
    assert(identifier == "minecraft:diamond_sword");
    assert(ResolveItemRuntimeId(-17, &identifier) == ItemRuntimeResolveStatus::Ready);
    assert(identifier == "example:tools/hammer");
    assert(ResolveItemRuntimeId(999, &identifier) ==
           ItemRuntimeResolveStatus::UnknownRuntimeId);
    assert(identifier.empty());

    ObserveItemRuntimeRegistryPacket(packetWithId(0x31U));
    assert(IsItemRuntimeRegistryReady());

    std::string truncated = registryPacket({{"minecraft:stone", 5, false}});
    truncated.pop_back();
    ObserveItemRuntimeRegistryPacket(truncated);
    assert(IsItemRuntimeRegistryReady());
    assert(ItemRuntimeRegistrySize() == 3U);
    assert(ResolveItemRuntimeId(276, &identifier) == ItemRuntimeResolveStatus::Ready);
    assert(identifier == "minecraft:diamond_sword");

    ObserveItemRuntimeRegistryPacket(registryPacket({
        {"minecraft:stone", 5, false},
        {"minecraft:dirt", 5, false},
    }));
    assert(IsItemRuntimeRegistryReady());
    assert(ItemRuntimeRegistrySize() == 3U);

    ObserveItemRuntimeRegistryPacket(registryPacket({
        {"minecraft:stone", 5, false, 3},
    }));
    assert(IsItemRuntimeRegistryReady());
    assert(ItemRuntimeRegistrySize() == 3U);

    ObserveItemRuntimeRegistryPacket(registryPacket({
        {"minecraft:stone", 5, false},
        {"not command safe", 6, false},
        {"minecraft:dirt", 7, false, 2, true},
    }));
    assert(IsItemRuntimeRegistryReady());
    assert(ItemRuntimeRegistrySize() == 2U);
    assert(ResolveItemRuntimeId(5, &identifier) == ItemRuntimeResolveStatus::Ready);
    assert(identifier == "minecraft:stone");
    assert(ResolveItemRuntimeId(6, &identifier) ==
           ItemRuntimeResolveStatus::UnknownRuntimeId);
    assert(ResolveItemRuntimeId(7, &identifier) == ItemRuntimeResolveStatus::Ready);
    assert(identifier == "minecraft:dirt");

    ObserveItemRuntimeRegistryPacket(registryPacket({}));
    assert(IsItemRuntimeRegistryReady());
    assert(ItemRuntimeRegistrySize() == 0U);
    assert(ResolveItemRuntimeId(5, &identifier) ==
           ItemRuntimeResolveStatus::UnknownRuntimeId);

    ObserveItemRuntimeRegistryPacket(
        registryPacket({{"minecraft:diamond_sword", 276, false}}));
    assert(IsItemRuntimeRegistryReady());
    ObserveItemRuntimeRegistryPacket(packetWithId(0x0BU));
    assert(!IsItemRuntimeRegistryReady());

    ObserveItemRuntimeRegistryPacket(
        registryPacket({{"minecraft:diamond_sword", 276, false}}));
    ObserveItemRuntimeRegistryPacket(packetWithId(0x05U));
    assert(!IsItemRuntimeRegistryReady());
    return 0;
}
