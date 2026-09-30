#include "MapNativeHeldMapEvidence.h"

#include "ItemRuntimeRegistry.h"
#include "ProjectionPrinterInventoryClientSync.h"
#include "ProjectionPrinterInventoryMover.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#if defined(__ANDROID__) && defined(__aarch64__)
#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"

#include <elf.h>
#include <link.h>
#endif

namespace build_import {
namespace {

bool reject(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

bool isFilledMapIdentifier(const std::string& value) {
    return value == "minecraft:map" || value == "map" ||
           value == "minecraft:filled_map" || value == "filled_map" ||
           value == "minecraft:locator_map" || value == "locator_map";
}

#if defined(__ANDROID__) && defined(__aarch64__)
// libminecraftpe.so from the verified Bedrock 1.21.120 / protocol-859 client.
constexpr uint8_t kExpectedBuildId[20] = {
    0xCC,0xF9,0xC3,0x1F,0x31,0x21,0xA4,0x6E,0x89,0xD8,
    0x68,0xDC,0xED,0xB2,0x39,0x46,0x6B,0x3A,0xC7,0x26,
};

size_t align4(size_t value) noexcept {
    return (value + 3U) & ~size_t{3U};
}

struct BuildIdQuery {
    uintptr_t base = 0;
    bool matched = false;
};

int inspectBuildId(dl_phdr_info* info, size_t, void* opaque) {
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
            query->base > std::numeric_limits<uintptr_t>::max() - segment.p_vaddr) {
            continue;
        }
        const auto* note = reinterpret_cast<const uint8_t*>(
            query->base + static_cast<uintptr_t>(segment.p_vaddr));
        const size_t size = static_cast<size_t>(segment.p_memsz);
        if (!IsMemoryReadable(note, size)) continue;
        size_t offset = 0;
        while (size - offset >= sizeof(Elf32_Nhdr)) {
            Elf32_Nhdr header{};
            std::memcpy(&header, note + offset, sizeof(header));
            offset += sizeof(header);
            const size_t name_size = align4(header.n_namesz);
            const size_t desc_size = align4(header.n_descsz);
            if (name_size > size - offset || desc_size > size - offset - name_size) break;
            const uint8_t* name = note + offset;
            const uint8_t* desc = note + offset + name_size;
            if (header.n_type == NT_GNU_BUILD_ID && header.n_namesz == 4U &&
                header.n_descsz == sizeof(kExpectedBuildId) &&
                std::memcmp(name, "GNU", 4U) == 0 &&
                std::memcmp(desc, kExpectedBuildId, sizeof(kExpectedBuildId)) == 0) {
                query->matched = true;
                return 1;
            }
            offset += name_size + desc_size;
        }
    }
    return 1;
}

bool isExactBuild(uintptr_t base) noexcept {
    BuildIdQuery query{base, false};
    dl_iterate_phdr(inspectBuildId, &query);
    return query.matched;
}
#endif

}  // namespace

bool ReadNativeHeldFilledMapEvidence(MapStorageHeldMapEvidence* output,
                                     std::string* error) {
    if (output) *output = {};
    if (error) error->clear();
    if (!output) return reject(error, "native held-map evidence output is unavailable");
#if !defined(__ANDROID__) || !defined(__aarch64__)
    return reject(error, "native held-map readback requires Android arm64-v8a");
#else
    try {
        const uintptr_t module_base = Main::getBaseAddress();
        if (!module_base || !isExactBuild(module_base)) {
            return reject(error, "held-map ABI does not match the verified game build");
        }
        if (!IsItemRuntimeRegistryReady()) {
            return reject(error, "held-map item registry is unavailable");
        }
        std::string packet;
        int32_t selected_slot = -1;
        if (!ReadProjectionPrinterNativeSelectedHotbarSlotPacket(
                &packet, &selected_slot, error)) return false;
        if (selected_slot < 0 || selected_slot > 8) {
            return reject(error, "held-map selected slot is invalid");
        }
        ProjectionPrinterNativeSlotItem item;
        ItemExtraMapMetadata metadata;
        if (!DecodeProjectionPrinterNativeInventorySlotMapMetadata(
                packet, static_cast<uint8_t>(selected_slot), &item, &metadata) ||
            !item.occupied || item.count != 1U || item.runtime_item_id <= 0 ||
            !item.has_network_stack_id || item.network_stack_id <= 0 ||
            !metadata.has_map_uuid || metadata.map_uuid == -1 ||
            metadata.name_status != MapItemNameStatus::Present ||
            metadata.name_source != MapItemNameSource::DisplayName ||
            metadata.display_name.empty()) {
            return reject(error, "held map lacks one-stack native UUID and DisplayName");
        }
        std::string identifier;
        if (ResolveItemRuntimeId(item.runtime_item_id, &identifier) !=
                ItemRuntimeResolveStatus::Ready ||
            !isFilledMapIdentifier(identifier)) {
            return reject(error, "held item is not a confirmed filled map");
        }
        int64_t native_uuid = -1;
        if (!ReadProjectionPrinterNativeMapUuid(selected_slot,
                                                item.network_stack_id,
                                                &native_uuid, error) ||
            native_uuid != metadata.map_uuid) {
            return reject(error, "held-map serialized UUID disagrees with native NBT");
        }
        std::string packet_after;
        int32_t selected_after = -1;
        if (!ReadProjectionPrinterNativeSelectedHotbarSlotPacket(
                &packet_after, &selected_after, error) ||
            selected_after != selected_slot || packet_after != packet) {
            return reject(error, "held map changed during native readback");
        }
        MapStorageHeldMapEvidence evidence;
        evidence.source.selected_hotbar_slot = selected_slot;
        evidence.source.network_stack_id = item.network_stack_id;
        evidence.source.runtime_item_id = item.runtime_item_id;
        evidence.source.count = item.count;
        evidence.source.item_identifier = std::move(identifier);
        evidence.source.has_map_uuid = true;
        evidence.source.map_uuid = native_uuid;
        evidence.name_status = metadata.name_status;
        evidence.name_source = metadata.name_source;
        evidence.name = std::move(metadata.display_name);
        evidence.native_readback = true;
        *output = std::move(evidence);
        return true;
    } catch (...) {
        *output = {};
        return reject(error, "native held-map readback failed");
    }
#endif
}

}  // namespace build_import
