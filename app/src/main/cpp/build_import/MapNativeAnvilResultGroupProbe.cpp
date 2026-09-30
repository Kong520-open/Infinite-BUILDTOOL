#include "MapNativeAnvilResultGroupProbe.h"

#include <array>
#include <cstring>
#include <limits>

#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__ANDROID__) && defined(__aarch64__)
#include "MapNativeAnvilManagerProbe.h"
#include "../tp/LoopbackPacketSenderCapture.h"

#include <elf.h>
#include <link.h>
#endif

namespace build_import {
namespace {

constexpr uintptr_t kRegistryHeadOffset = 0x10U;
constexpr uintptr_t kNodeKeyOffset = 0x10U;
constexpr uintptr_t kNodeStringOffset = 0x18U;
constexpr uint8_t kAnvilResultKey = 2U;
constexpr size_t kMaxNodes = 128U;
constexpr size_t kMaxGroupBytes = 64U;
constexpr char kExpectedGroup[] = "anvil_result_items";
static_assert(sizeof(uintptr_t) == 8U, "Native registry decoding requires 64-bit pointers");

bool reject(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

bool addOffset(uintptr_t address, uintptr_t offset, uintptr_t* result) noexcept {
    if (!result || address > std::numeric_limits<uintptr_t>::max() - offset)
        return false;
    *result = address + offset;
    return true;
}

bool readAt(uintptr_t address, void* destination, size_t size,
            MapNativeAnvilMemoryReader reader, void* context) noexcept {
    return address && reader && destination && size &&
           address <= std::numeric_limits<uintptr_t>::max() - (size - 1U) &&
           reader(address, destination, size, context);
}

bool decodeNativeString(uintptr_t address, MapNativeAnvilMemoryReader reader,
                        void* context, std::string* result) {
    std::array<uint8_t, 24> raw{};
    if (!result || !readAt(address, raw.data(), raw.size(), reader, context))
        return false;
    const bool long_form = (raw[0] & 1U) != 0U;
    if (!long_form) {
        const size_t length = raw[0] >> 1U;
        if (length > 22U || raw[1U + length] != 0U) return false;
        result->assign(reinterpret_cast<const char*>(raw.data() + 1U), length);
        return true;
    }
    uint64_t length = 0;
    uintptr_t data = 0;
    std::memcpy(&length, raw.data() + 8U, sizeof(length));
    std::memcpy(&data, raw.data() + 16U, sizeof(data));
    if (!data || length > kMaxGroupBytes) return false;
    std::array<char, kMaxGroupBytes + 1U> bytes{};
    if (!readAt(data, bytes.data(), static_cast<size_t>(length) + 1U,
                reader, context) || bytes[static_cast<size_t>(length)] != '\0')
        return false;
    result->assign(bytes.data(), static_cast<size_t>(length));
    return true;
}

#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__ANDROID__) && defined(__aarch64__)
// libminecraftpe.so Build ID ccf9c31f3121a46e89d868dcedb239466b3ac726.
constexpr uint8_t kExpectedBuildId[20] = {
    0xCC,0xF9,0xC3,0x1F,0x31,0x21,0xA4,0x6E,0x89,0xD8,
    0x68,0xDC,0xED,0xB2,0x39,0x46,0x6B,0x3A,0xC7,0x26,
};
constexpr uintptr_t kRegistryPointerRva = 0x12DA2080ULL;
static_assert(sizeof(std::string) == 24U, "Unexpected Android libc++ string ABI");

size_t align4(size_t value) noexcept { return (value + 3U) & ~size_t{3U}; }

struct BuildIdQuery {
    uintptr_t base;
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
            query->base > std::numeric_limits<uintptr_t>::max() - segment.p_vaddr)
            continue;
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
            if (name_size > size - offset || desc_size > size - offset - name_size)
                break;
            const uint8_t* name = note + offset;
            const uint8_t* desc = note + offset + name_size;
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

bool isExactBuild(uintptr_t base) noexcept {
    BuildIdQuery query{base, false};
    dl_iterate_phdr(inspectBuildId, &query);
    return query.matched;
}

bool readNativeMemory(uintptr_t address, void* destination, size_t size,
                      void*) noexcept {
    const void* source = reinterpret_cast<const void*>(address);
    if (!IsMemoryReadable(source, size)) return false;
    std::memcpy(destination, source, size);
    return true;
}
#endif

}  // namespace

bool ProbeMapNativeAnvilResultGroupRegistry(
    uintptr_t registry, MapNativeAnvilMemoryReader reader, void* reader_context,
    MapNativeAnvilResultGroupObservation* observation, std::string* error) {
    if (observation) *observation = {};
    if (error) error->clear();
    if (!observation || !reader || !registry || (registry & 7U) != 0U)
        return reject(error, "native result-group registry is unavailable");
    uintptr_t head_address = 0;
    if (!addOffset(registry, kRegistryHeadOffset, &head_address))
        return reject(error, "native result-group registry address overflow");
    uintptr_t node = 0;
    if (!readAt(head_address, &node, sizeof(node), reader, reader_context))
        return reject(error, "native result-group registry head is unreadable");
    std::array<uintptr_t, kMaxNodes> visited{};
    bool found = false;
    while (node) {
        if ((node & 7U) != 0U)
            return reject(error, "native result-group node is unaligned");
        if (observation->nodes_scanned >= kMaxNodes)
            return reject(error, "native result-group node limit exceeded");
        for (uint32_t i = 0; i < observation->nodes_scanned; ++i) {
            if (visited[i] == node)
                return reject(error, "native result-group registry cycle detected");
        }
        visited[observation->nodes_scanned++] = node;
        std::array<uint8_t, kNodeStringOffset> header{};
        if (!readAt(node, header.data(), header.size(), reader, reader_context))
            return reject(error, "native result-group node is unreadable");
        uintptr_t next = 0;
        std::memcpy(&next, header.data(), sizeof(next));
        if (header[kNodeKeyOffset] == kAnvilResultKey) {
            if (found)
                return reject(error, "duplicate native anvil result-group key");
            uintptr_t string_address = 0;
            if (!addOffset(node, kNodeStringOffset, &string_address) ||
                !decodeNativeString(string_address, reader, reader_context,
                                    &observation->group))
                return reject(error, "native anvil result-group string is invalid");
            found = true;
        }
        node = next;
    }
    if (!found)
        return reject(error, "native anvil result-group key is absent");
    if (observation->group != kExpectedGroup)
        return reject(error, "native anvil result-group name differs");
    observation->exact_match = true;
    return true;
}

bool ProbeMapNativeAnvilResultGroup(
    uintptr_t minecraft_base, uint64_t ticket,
    const MapVisibleAnvilWindowSession& session,
    MapNativeAnvilResultGroupObservation* observation, std::string* error) {
    if (observation) *observation = {};
    if (error) error->clear();
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__ANDROID__) && defined(__aarch64__)
    const ContainerCaptureResult* capture = session.capture();
    if (!observation || !ticket || session.state() != MapVisibleAnvilWindowState::Open ||
        !capture || capture->token != ticket || !capture->container_opened ||
        capture->container_closed || capture->container_type != 5U ||
        capture->slot_count != 3U)
        return reject(error, "native anvil window identity is not confirmed");
    if (!minecraft_base || !isExactBuild(minecraft_base))
        return reject(error, "native anvil build ID mismatch");
    uintptr_t screen = 0;
    if (!BorrowMapNativeAnvilScreenForCurrentThread(minecraft_base, ticket,
                                                     &screen) || !screen)
        return reject(error, "native anvil screen is not live on this thread");
    uintptr_t registry_address = 0;
    if (!addOffset(minecraft_base, kRegistryPointerRva, &registry_address))
        return reject(error, "native result-group registry address overflow");
    uintptr_t registry = 0;
    if (!readNativeMemory(registry_address, &registry, sizeof(registry), nullptr))
        return reject(error, "native result-group registry pointer is unreadable");
    return ProbeMapNativeAnvilResultGroupRegistry(
        registry, readNativeMemory, nullptr, observation, error);
#else
    (void)minecraft_base;
    (void)ticket;
    (void)session;
    return reject(error, "native anvil result-group probe is unavailable in this build");
#endif
}

}  // namespace build_import
