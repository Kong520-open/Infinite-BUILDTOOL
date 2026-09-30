#ifndef INFINITE_TEXTURE_ITEM_RUNTIME_REGISTRY_H
#define INFINITE_TEXTURE_ITEM_RUNTIME_REGISTRY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace build_import {

enum class ItemRuntimeResolveStatus : uint8_t {
    Ready = 0,
    RegistryUnavailable = 1,
    UnknownRuntimeId = 2,
};

// Observes StartGame, Disconnect, and the current ItemRegistry packet (0xA2).
// A registry is published only after the complete bounded packet parses.
void ObserveItemRuntimeRegistryPacket(std::string_view packet) noexcept;
void ClearItemRuntimeRegistry() noexcept;

ItemRuntimeResolveStatus ResolveItemRuntimeId(int32_t runtime_id,
                                              std::string* identifier);
bool IsItemRuntimeRegistryReady() noexcept;
size_t ItemRuntimeRegistrySize() noexcept;

}  // namespace build_import

#endif  // INFINITE_TEXTURE_ITEM_RUNTIME_REGISTRY_H
