#include "MapNativeAnvilCraftPacket.h"

#include <cstring>
#include <limits>
#include <utility>

#if defined(__ANDROID__) && defined(__aarch64__)
#include "MapManualOutboundTrace.h"
#include "../tp/LoopbackPacketSenderCapture.h"

#include <elf.h>
#include <link.h>
#endif

namespace build_import {

bool IsMapNativeAnvilCraftPacketCandidate(
    const MapNativeAnvilCraftPacketShape& shape,
    const std::string& expected_title,
    int32_t expected_input_network_id) noexcept {
    return !expected_title.empty() && expected_title.size() <= 22U &&
        expected_input_network_id > 0 && shape.request_count == 1U &&
        shape.request_id < 0 &&
        (static_cast<uint32_t>(shape.request_id) & 1U) != 0U &&
        shape.action_count == 3U && shape.name_count == 1U &&
        shape.custom_name == expected_title &&
        shape.first_action_type == 15U && shape.recipe_network_id != 0U &&
        shape.filtered_string_index == 0 &&
        shape.second_action_type == 5U && shape.consumed_count == 1U &&
        shape.consumed_container == 0U && shape.consumed_slot == 1U &&
        shape.consumed_network_id == expected_input_network_id &&
        shape.third_action_type == 1U && shape.placed_count == 1U &&
        shape.created_container == 61U && shape.created_slot == 50U &&
        shape.created_predicted_sequence == shape.request_id &&
        shape.created_variant_tag == 1 &&
        shape.destination_container == 12U &&
        shape.destination_slot <= 8U && shape.destination_network_id == 0;
}

#if defined(__ANDROID__) && defined(__aarch64__)
namespace {

// libminecraftpe.so Build ID ccf9c31f3121a46e89d868dcedb239466b3ac726.
constexpr uintptr_t kPacketVtableRva = 0x12951140ULL;
constexpr uintptr_t kRequestVtableRva = 0x12AAA500ULL;
constexpr uintptr_t kCraftOptionalVtableRva = 0x12AA4978ULL;
constexpr uintptr_t kConsumeVtableRva = 0x12AA8250ULL;
constexpr uintptr_t kPlaceVtableRva = 0x12AA8130ULL;
constexpr size_t kPacketBatchOffset = 0x30U;
constexpr uint8_t kExpectedBuildId[20] = {
    0xCC,0xF9,0xC3,0x1F,0x31,0x21,0xA4,0x6E,0x89,0xD8,
    0x68,0xDC,0xED,0xB2,0x39,0x46,0x6B,0x3A,0xC7,0x26,
};

size_t align4(size_t value) noexcept { return (value + 3U) & ~size_t{3U}; }

struct BuildIdQuery { uintptr_t base; bool matched = false; };

int checkBuildId(dl_phdr_info* info, size_t, void* opaque) noexcept {
    auto* query = static_cast<BuildIdQuery*>(opaque);
    if (!info || !query || static_cast<uintptr_t>(info->dlpi_addr) != query->base ||
        !info->dlpi_name) return 0;
    const char* leaf = std::strrchr(info->dlpi_name, '/');
    leaf = leaf ? leaf + 1 : info->dlpi_name;
    if (std::strcmp(leaf, "libminecraftpe.so") != 0) return 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const auto& segment = info->dlpi_phdr[i];
        if (segment.p_type != PT_NOTE || segment.p_memsz == 0U ||
            segment.p_memsz > 4096U ||
            query->base > std::numeric_limits<uintptr_t>::max() -
                              segment.p_vaddr) continue;
        const uintptr_t note = query->base + segment.p_vaddr;
        const size_t size = static_cast<size_t>(segment.p_memsz);
        if (!IsMemoryReadable(reinterpret_cast<const void*>(note), size)) continue;
        size_t offset = 0;
        while (size - offset >= sizeof(Elf32_Nhdr)) {
            Elf32_Nhdr header{};
            std::memcpy(&header, reinterpret_cast<const void*>(note + offset),
                        sizeof(header));
            offset += sizeof(header);
            const size_t name_size = align4(header.n_namesz);
            const size_t desc_size = align4(header.n_descsz);
            if (name_size > size - offset ||
                desc_size > size - offset - name_size) break;
            const uint8_t* name = reinterpret_cast<const uint8_t*>(note + offset);
            const uint8_t* desc = name + name_size;
            if (header.n_type == NT_GNU_BUILD_ID && header.n_namesz == 4U &&
                header.n_descsz == sizeof(kExpectedBuildId) &&
                std::memcmp(name, "GNU", 4U) == 0 &&
                std::memcmp(desc, kExpectedBuildId,
                            sizeof(kExpectedBuildId)) == 0) {
                query->matched = true;
                return 1;
            }
            offset += name_size + desc_size;
        }
    }
    return 1;
}

bool exactBuild(uintptr_t base) noexcept {
    BuildIdQuery query{base};
    dl_iterate_phdr(checkBuildId, &query);
    return query.matched;
}

template <typename T>
bool copyAt(uintptr_t address, size_t offset, T* output) noexcept {
    if (!address || !output ||
        address > std::numeric_limits<uintptr_t>::max() - offset) return false;
    const void* source = reinterpret_cast<const void*>(address + offset);
    if (!IsMemoryReadable(source, sizeof(T))) return false;
    std::memcpy(output, source, sizeof(T));
    return true;
}

bool vectorOf(uintptr_t owner, size_t offset, size_t stride,
              size_t expected, uintptr_t* begin) noexcept {
    struct Vector { uintptr_t first, last, cap; } vector{};
    if (!copyAt(owner, offset, &vector) || !stride ||
        !vector.first || vector.first > vector.last ||
        vector.last > vector.cap ||
        vector.last - vector.first != expected * stride ||
        !IsMemoryReadable(reinterpret_cast<const void*>(vector.first),
                          expected * stride)) return false;
    *begin = vector.first;
    return true;
}

bool readName(uintptr_t address, std::string* output) noexcept {
    if (!output) return false;
    uint8_t raw[24]{};
    if (!copyAt(address, 0U, &raw)) return false;
    size_t length = 0;
    if ((raw[0] & 1U) == 0U) {
        length = raw[0] >> 1U;
        if (length > 22U || raw[1U + length] != 0U) return false;
        output->assign(reinterpret_cast<const char*>(raw + 1U), length);
        return true;
    }
    uintptr_t data = 0;
    std::memcpy(&length, raw + 8U, sizeof(length));
    std::memcpy(&data, raw + 16U, sizeof(data));
    if (!data || length > 22U ||
        !IsMemoryReadable(reinterpret_cast<const void*>(data), length + 1U))
        return false;
    const char* chars = reinterpret_cast<const char*>(data);
    if (chars[length] != '\0') return false;
    output->assign(chars, length);
    return true;
}

bool readSlot(uintptr_t action, size_t offset, uint8_t* container,
              uint8_t* slot, int32_t* network_id) noexcept {
    uint32_t dynamic_id = 0;
    uint8_t dynamic = 0;
    return copyAt(action, offset, container) &&
        copyAt(action, offset + 4U, &dynamic_id) &&
        copyAt(action, offset + 8U, &dynamic) &&
        copyAt(action, offset + 0xCU, slot) &&
        copyAt(action, offset + 0x10U, network_id) &&
        !dynamic && !dynamic_id;
}

bool actionAt(uintptr_t actions, size_t index, uintptr_t expected_vtable,
              uint8_t expected_type, uintptr_t* output) noexcept {
    uintptr_t action = 0, vtable = 0;
    uint8_t type = 0;
    if (!copyAt(actions, index * sizeof(uintptr_t), &action) || !action ||
        !copyAt(action, 0U, &vtable) || vtable != expected_vtable ||
        !copyAt(action, 8U, &type) || type != expected_type) return false;
    *output = action;
    return true;
}

}  // namespace
#endif

bool DecodeMapNativeAnvilCraftPacketCandidate(
    uintptr_t minecraft_base, const void* packet,
    MapNativeAnvilCraftPacketShape* output) noexcept {
    if (output) *output = {};
#if defined(__ANDROID__) && defined(__aarch64__)
    try {
    if (!output || !minecraft_base || !packet ||
        minecraft_base > std::numeric_limits<uintptr_t>::max() -
                             kPacketVtableRva ||
        !exactBuild(minecraft_base) ||
        !PrimeMapManualOutboundTraceProfile()) return false;
    uintptr_t packet_vtable = 0, batch = 0, requests = 0, request = 0;
    uintptr_t request_vtable = 0, actions = 0, names = 0;
    if (!copyAt(reinterpret_cast<uintptr_t>(packet), 0U, &packet_vtable) ||
        packet_vtable != minecraft_base + kPacketVtableRva ||
        !copyAt(reinterpret_cast<uintptr_t>(packet), kPacketBatchOffset,
                &batch) || !batch ||
        !vectorOf(batch, 0U, sizeof(uintptr_t), 1U, &requests) ||
        !copyAt(requests, 0U, &request) || !request ||
        !copyAt(request, 0U, &request_vtable) ||
        request_vtable != minecraft_base + kRequestVtableRva ||
        !vectorOf(request, 0x30U, sizeof(uintptr_t), 3U, &actions) ||
        !vectorOf(request, 0x10U, 24U, 1U, &names)) return false;
    MapNativeAnvilCraftPacketShape parsed{};
    parsed.request_count = 1U;
    parsed.action_count = 3U;
    parsed.name_count = 1U;
    if (!copyAt(request, 0x08U, &parsed.request_id) ||
        !readName(names, &parsed.custom_name)) return false;
    uintptr_t craft = 0, consume = 0, place = 0;
    if (!actionAt(actions, 0U, minecraft_base + kCraftOptionalVtableRva,
                  15U, &craft) ||
        !actionAt(actions, 1U, minecraft_base + kConsumeVtableRva,
                  5U, &consume) ||
        !actionAt(actions, 2U, minecraft_base + kPlaceVtableRva,
                  1U, &place)) return false;
    parsed.first_action_type = 15U;
    parsed.second_action_type = 5U;
    parsed.third_action_type = 1U;
    uint32_t dynamic_id = 0;
    uint8_t dynamic = 0;
    uintptr_t predicted_vtable = 0;
    if (!copyAt(craft, 0x0CU, &parsed.recipe_network_id) ||
        !copyAt(craft, 0x10U, &parsed.filtered_string_index) ||
        !copyAt(consume, 0x0BU, &parsed.consumed_count) ||
        !readSlot(consume, 0x10U, &parsed.consumed_container,
                  &parsed.consumed_slot, &parsed.consumed_network_id) ||
        !copyAt(place, 0x0BU, &parsed.placed_count) ||
        !copyAt(place, 0x10U, &parsed.created_container) ||
        !copyAt(place, 0x14U, &dynamic_id) ||
        !copyAt(place, 0x18U, &dynamic) ||
        !copyAt(place, 0x1CU, &parsed.created_slot) ||
        dynamic || dynamic_id ||
        !copyAt(place, 0x20U, &predicted_vtable) ||
        predicted_vtable != minecraft_base + kRequestVtableRva ||
        !copyAt(place, 0x28U, &parsed.created_predicted_sequence) ||
        !copyAt(place, 0x30U, &parsed.created_variant_tag) ||
        !readSlot(place, 0x38U, &parsed.destination_container,
                  &parsed.destination_slot,
                  &parsed.destination_network_id)) return false;
    *output = std::move(parsed);
    return true;
    } catch (...) {
        if (output) *output = {};
        return false;
    }
#else
    (void)minecraft_base;
    (void)packet;
    return false;
#endif
}

}  // namespace build_import
