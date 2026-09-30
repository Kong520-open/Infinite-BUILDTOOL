#include "../MapNativeAnvilResultGroupProbe.h"

#include <array>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Region {
    uintptr_t address;
    size_t size;
};

struct Memory {
    std::vector<Region> regions;

    template <size_t N>
    void add(const std::array<uint8_t, N>& bytes) {
        regions.push_back({reinterpret_cast<uintptr_t>(bytes.data()), bytes.size()});
    }
};

bool readMemory(uintptr_t address, void* destination, size_t size,
                void* opaque) noexcept {
    auto* memory = static_cast<Memory*>(opaque);
    if (!memory || !destination || !size) return false;
    for (const Region& region : memory->regions) {
        if (address >= region.address) {
            const uintptr_t offset = address - region.address;
            if (offset <= region.size && size <= region.size - offset) {
                std::memcpy(destination, reinterpret_cast<const void*>(address), size);
                return true;
            }
        }
    }
    return false;
}

template <size_t N>
void putPointer(std::array<uint8_t, N>* bytes, size_t offset, uintptr_t value) {
    assert(bytes && offset <= bytes->size() - sizeof(value));
    std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

using Node = std::array<uint8_t, 0x18U + 24U>;

void shortString(Node* node, const std::string& value) {
    assert(node && value.size() <= 22U);
    (*node)[0x18U] = static_cast<uint8_t>(value.size() << 1U);
    std::memcpy(node->data() + 0x19U, value.data(), value.size());
    (*node)[0x19U + value.size()] = 0;
}

void longString(Node* node, const char* data, uint64_t size) {
    assert(node);
    (*node)[0x18U] = 1U;
    std::memcpy(node->data() + 0x20U, &size, sizeof(size));
    putPointer(node, 0x28U, reinterpret_cast<uintptr_t>(data));
}

bool probe(uintptr_t registry, Memory* memory,
           build_import::MapNativeAnvilResultGroupObservation* observation,
           std::string* error) {
    return build_import::ProbeMapNativeAnvilResultGroupRegistry(
        registry, readMemory, memory, observation, error);
}

}  // namespace

int main() {
    using build_import::MapNativeAnvilResultGroupObservation;

    alignas(8) std::array<uint8_t, 0x20U> registry{};
    alignas(8) Node result{};
    alignas(8) Node other{};
    result[0x10U] = 2U;
    other[0x10U] = 1U;
    shortString(&result, "anvil_result_items");
    shortString(&other, "other");
    putPointer(&registry, 0x10U, reinterpret_cast<uintptr_t>(&other));
    putPointer(&other, 0U, reinterpret_cast<uintptr_t>(&result));
    Memory memory;
    memory.add(registry);
    memory.add(result);
    memory.add(other);
    MapNativeAnvilResultGroupObservation observation{};
    std::string error;

    assert(probe(reinterpret_cast<uintptr_t>(&registry), &memory,
                 &observation, &error));
    assert(observation.exact_match && observation.group == "anvil_result_items");
    assert(observation.nodes_scanned == 2U && error.empty());

    shortString(&result, "wrong_group");
    assert(!probe(reinterpret_cast<uintptr_t>(&registry), &memory,
                  &observation, &error));
    assert(!observation.exact_match && observation.group == "wrong_group");
    shortString(&result, "anvil_result_items");

    result[0x10U] = 3U;
    assert(!probe(reinterpret_cast<uintptr_t>(&registry), &memory,
                  &observation, &error));
    assert(!observation.exact_match);
    result[0x10U] = 2U;

    putPointer(&result, 0U, reinterpret_cast<uintptr_t>(&other));
    assert(!probe(reinterpret_cast<uintptr_t>(&registry), &memory,
                  &observation, &error));
    assert(error.find("cycle") != std::string::npos);
    putPointer(&result, 0U, 0U);

    other[0x10U] = 2U;
    assert(!probe(reinterpret_cast<uintptr_t>(&registry), &memory,
                  &observation, &error));
    assert(error.find("duplicate") != std::string::npos);
    other[0x10U] = 1U;

    const std::string long_name = "anvil_result_items_extra";
    longString(&result, long_name.c_str(), long_name.size());
    memory.regions.push_back({reinterpret_cast<uintptr_t>(long_name.c_str()),
                              long_name.size() + 1U});
    assert(!probe(reinterpret_cast<uintptr_t>(&registry), &memory,
                  &observation, &error));
    assert(observation.group == long_name && !observation.exact_match);
    memory.regions.pop_back();
    assert(!probe(reinterpret_cast<uintptr_t>(&registry), &memory,
                  &observation, &error));
    assert(error.find("invalid") != std::string::npos);
    longString(&result, long_name.c_str(), 65U);
    assert(!probe(reinterpret_cast<uintptr_t>(&registry), &memory,
                  &observation, &error));
    shortString(&result, "anvil_result_items");

    Memory missing{};
    missing.add(registry);
    assert(!probe(reinterpret_cast<uintptr_t>(&registry), &missing,
                  &observation, &error));
    assert(error.find("unreadable") != std::string::npos);

    alignas(8) std::array<Node, 129U> many{};
    for (size_t i = 0; i < many.size(); ++i) {
        many[i][0x10U] = 1U;
        if (i + 1U < many.size()) {
            putPointer(&many[i], 0U, reinterpret_cast<uintptr_t>(&many[i + 1U]));
        }
    }
    putPointer(&registry, 0x10U, reinterpret_cast<uintptr_t>(&many[0]));
    Memory bounded{};
    bounded.add(registry);
    bounded.regions.push_back({reinterpret_cast<uintptr_t>(many.data()),
                               sizeof(many)});
    assert(!probe(reinterpret_cast<uintptr_t>(&registry), &bounded,
                  &observation, &error));
    assert(observation.nodes_scanned == 128U);
    assert(error.find("limit") != std::string::npos);

    assert(!probe(0U, &memory, &observation, &error));
    assert(!observation.exact_match);

    build_import::MapVisibleAnvilWindowSession inactive_session{};
    assert(!build_import::ProbeMapNativeAnvilResultGroup(
        0U, 0U, inactive_session, &observation, &error));
    assert(!observation.exact_match);
    assert(error.find("unavailable") != std::string::npos);
    return 0;
}
