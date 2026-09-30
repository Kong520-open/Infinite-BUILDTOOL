#include "MapNativeAnvilNameBridge.h"

#include "MapNativeAnvilManagerProbe.h"
#include "MapTileNaming.h"

#include <cstring>
#include <limits>
#include <utility>

#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__ANDROID__) && defined(__aarch64__)
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

#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__ANDROID__) && defined(__aarch64__)
// libminecraftpe.so Build ID ccf9c31f3121a46e89d868dcedb239466b3ac726.
constexpr uint8_t kExpectedBuildId[20] = {
    0xCC,0xF9,0xC3,0x1F,0x31,0x21,0xA4,0x6E,0x89,0xD8,
    0x68,0xDC,0xED,0xB2,0x39,0x46,0x6B,0x3A,0xC7,0x26,
};
constexpr uintptr_t kRenameLambdaRva = 0x0603E534ULL;
constexpr uint8_t kRenameLambdaFingerprint[32] = {
    0xFF,0xC3,0x01,0xD1,0xFD,0x7B,0x04,0xA9,0xF6,0x57,0x05,0xA9,0xF4,0x4F,0x06,0xA9,
    0xFD,0x03,0x01,0x91,0x55,0xD0,0x3B,0xD5,0xF3,0x03,0x01,0xAA,0xA8,0x16,0x40,0xF9,
};

size_t align4(size_t value) noexcept {
    return (value + 3U) & ~size_t{3U};
}

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

struct RenameClosure {
    uintptr_t unused_vtable = 0;
    uintptr_t screen = 0;
};
static_assert(sizeof(RenameClosure) == 16U, "Unexpected UI lambda closure ABI");
static_assert(sizeof(std::string) == 24U, "Unexpected Android libc++ string ABI");
#endif

}  // namespace

bool ValidateMapNativeAnvilNameGate(const MapNativeAnvilNameRequest& request,
                                    MapVisibleAnvilWindowState state,
                                    const ContainerCaptureResult* capture,
                                    std::string* validated_title,
                                    std::string* error) {
    if (validated_title) validated_title->clear();
    if (error) error->clear();
    if (!request.ticket || request.expected_window_id == 0U ||
        request.expected_window_id == 0xFFU ||
        state != MapVisibleAnvilWindowState::Open || !capture ||
        capture->token != request.ticket || !capture->container_opened ||
        capture->container_closed || capture->container_type != 5U ||
        capture->container_id != request.expected_window_id ||
        capture->slot_count != 3U || capture->x != request.x ||
        capture->y != request.y || capture->z != request.z) {
        return reject(error, "native anvil window identity is not confirmed");
    }
    std::string title;
    if (!FormatMapTileName(request.tile_cursor, request.columns,
                           request.rows, &title) ||
        request.expected_title != title || title.empty() || title.size() > 22U) {
        return reject(error, "map tile name does not match the exact plan");
    }
    if (validated_title) *validated_title = std::move(title);
    return true;
}

bool SubmitMapNativeAnvilRenameText(uintptr_t minecraft_base,
                                    const MapNativeAnvilNameRequest& request,
                                    uint64_t now_ms,
                                    const MapVisibleAnvilWindowSession& session,
                                    std::string* error) {
    ContainerCaptureResult current;
    if (!session.verifiedLiveCapture(request.ticket, now_ms, &current, error))
        return false;
    std::string name;
    if (!ValidateMapNativeAnvilNameGate(request, session.state(),
                                        &current, &name, error)) return false;
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && defined(__ANDROID__) && defined(__aarch64__)
    if (!minecraft_base || !isExactBuild(minecraft_base)) {
        return reject(error, "native anvil build ID mismatch");
    }
    uintptr_t manager = 0;
    if (!BorrowMapNativeAnvilManagerForVerifiedWindow(minecraft_base,
            request.ticket, now_ms, session, &manager) || !manager) {
        return reject(error, "native anvil manager/window binding is unavailable");
    }
    uintptr_t screen = 0;
    if (!BorrowMapNativeAnvilScreenForCurrentThread(minecraft_base,
                                                     request.ticket, &screen)) {
        return reject(error, "native anvil screen is not live on this thread");
    }
    uintptr_t screen_manager = 0;
    if (!IsMemoryReadable(reinterpret_cast<const void*>(screen + 0xE40U),
                          sizeof(screen_manager))) {
        return reject(error, "native anvil screen manager is unreadable");
    }
    std::memcpy(&screen_manager,
                reinterpret_cast<const void*>(screen + 0xE40U),
                sizeof(screen_manager));
    if (screen_manager != manager) {
        return reject(error, "native anvil screen/window manager changed");
    }
    uintptr_t callback = 0;
    if (!ResolveMinecraftExecutableOffset(minecraft_base, kRenameLambdaRva,
                                          sizeof(kRenameLambdaFingerprint),
                                          &callback) ||
        !IsMemoryReadable(reinterpret_cast<const void*>(callback),
                          sizeof(kRenameLambdaFingerprint)) ||
        std::memcmp(reinterpret_cast<const void*>(callback),
                    kRenameLambdaFingerprint,
                    sizeof(kRenameLambdaFingerprint)) != 0) {
        return reject(error, "native anvil name callback fingerprint mismatch");
    }
    // Original AnvilScreenController proxy lambda: closure+8 is the owning
    // screen; x1 points to a libc++ std::string. It constructs the game's own
    // RichText twice and calls manager +0x20/+0x28/+0x40 in that order.
    const RenameClosure closure{0U, screen};
    using RenameCallback = void (*)(const RenameClosure*, const std::string*);
    reinterpret_cast<RenameCallback>(callback)(&closure, &name);
    return true;
#else
    (void)minecraft_base;
    return reject(error, "native anvil rename bridge is unavailable in this build");
#endif
}

}  // namespace build_import
