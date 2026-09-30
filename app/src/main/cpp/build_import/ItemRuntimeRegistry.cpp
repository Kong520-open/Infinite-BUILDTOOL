#include "ItemRuntimeRegistry.h"

#include "DeferredImportDataSpool.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>

namespace build_import {
namespace {

constexpr uint32_t kDisconnectPacketId = 0x05U;
constexpr uint32_t kStartGamePacketId = 0x0BU;
constexpr uint32_t kItemRegistryPacketId = 0xA2U;
constexpr size_t kMaximumPacketBytes = 64U * 1024U * 1024U;
constexpr uint32_t kMaximumRegistryEntries = 65'536U;
constexpr uint32_t kMaximumIdentifierBytes = 256U;
constexpr uint32_t kMaximumNbtStringBytes = 16U * 1024U * 1024U;
constexpr uint32_t kMaximumNbtNodes = 262'144U;
constexpr uint32_t kMaximumNbtDepth = 64U;
constexpr int32_t kMaximumItemVersion = 2;

class Reader {
public:
    explicit Reader(std::string_view input) : input_(input) {}

    bool byte(uint8_t* output) {
        if (!output || cursor_ >= input_.size()) return false;
        *output = static_cast<uint8_t>(input_[cursor_++]);
        return true;
    }

    bool littleEndian16(uint16_t* output) {
        if (!output || input_.size() - cursor_ < 2U) return false;
        *output = static_cast<uint16_t>(static_cast<uint8_t>(input_[cursor_])) |
            static_cast<uint16_t>(static_cast<uint8_t>(input_[cursor_ + 1U]) << 8U);
        cursor_ += 2U;
        return true;
    }

    bool varUInt(uint32_t* output) {
        if (!output) return false;
        uint32_t value = 0;
        for (uint32_t index = 0; index < 5U && cursor_ < input_.size(); ++index) {
            const uint8_t current = static_cast<uint8_t>(input_[cursor_++]);
            if (index == 4U && (current & 0xF0U) != 0U) return false;
            value |= static_cast<uint32_t>(current & 0x7FU) << (index * 7U);
            if ((current & 0x80U) == 0U) {
                *output = value;
                return true;
            }
        }
        return false;
    }

    bool varUInt64(uint64_t* output) {
        if (!output) return false;
        uint64_t value = 0;
        for (uint32_t index = 0; index < 10U && cursor_ < input_.size(); ++index) {
            const uint8_t current = static_cast<uint8_t>(input_[cursor_++]);
            if (index == 9U && (current & 0xFEU) != 0U) return false;
            value |= static_cast<uint64_t>(current & 0x7FU) << (index * 7U);
            if ((current & 0x80U) == 0U) {
                *output = value;
                return true;
            }
        }
        return false;
    }

    bool varInt(int32_t* output) {
        uint32_t encoded = 0;
        if (!output || !varUInt(&encoded)) return false;
        *output = static_cast<int32_t>((encoded >> 1U) ^ (0U - (encoded & 1U)));
        return true;
    }

    bool varLong(int64_t* output) {
        uint64_t encoded = 0;
        if (!output || !varUInt64(&encoded)) return false;
        *output = static_cast<int64_t>((encoded >> 1U) ^ (0ULL - (encoded & 1ULL)));
        return true;
    }

    bool skip(size_t count) {
        if (count > input_.size() - cursor_) return false;
        cursor_ += count;
        return true;
    }

    bool string(std::string* output, uint32_t maximum_bytes) {
        uint32_t length = 0;
        if (!output || !varUInt(&length) || length > maximum_bytes ||
            length > input_.size() - cursor_) {
            return false;
        }
        output->assign(input_.data() + cursor_, length);
        cursor_ += length;
        return true;
    }

    bool skipString(uint32_t maximum_bytes) {
        uint32_t length = 0;
        return varUInt(&length) && length <= maximum_bytes && skip(length);
    }

    bool atEnd() const { return cursor_ == input_.size(); }

private:
    std::string_view input_;
    size_t cursor_ = 0;
};

bool skipNbtPayload(Reader* reader, uint8_t type, uint32_t depth,
                    uint32_t* node_count) {
    if (!reader || !node_count || type > 12U ||
        ++(*node_count) > kMaximumNbtNodes) {
        return false;
    }
    switch (type) {
        case 0: return true;
        case 1: return reader->skip(1U);
        case 2: return reader->skip(2U);
        case 3: {
            int32_t ignored = 0;
            return reader->varInt(&ignored);
        }
        case 4: {
            int64_t ignored = 0;
            return reader->varLong(&ignored);
        }
        case 5: return reader->skip(4U);
        case 6: return reader->skip(8U);
        case 7: {
            int32_t length = 0;
            return reader->varInt(&length) && length >= 0 &&
                reader->skip(static_cast<size_t>(length));
        }
        case 8:
            return reader->skipString(kMaximumNbtStringBytes);
        case 9: {
            if (depth == 0U) return false;
            uint8_t element_type = 0;
            int32_t length = 0;
            if (!reader->byte(&element_type) || element_type > 12U ||
                !reader->varInt(&length) || length < 0 ||
                static_cast<uint32_t>(length) > kMaximumNbtNodes - *node_count ||
                (element_type == 0U && length != 0)) {
                return false;
            }
            for (int32_t index = 0; index < length; ++index) {
                if (!skipNbtPayload(reader, element_type, depth - 1U, node_count)) {
                    return false;
                }
            }
            return true;
        }
        case 10: {
            if (depth == 0U) return false;
            for (;;) {
                uint8_t child_type = 0;
                if (!reader->byte(&child_type) || child_type > 12U) return false;
                if (child_type == 0U) return true;
                if (!reader->skipString(kMaximumNbtStringBytes) ||
                    !skipNbtPayload(reader, child_type, depth - 1U, node_count)) {
                    return false;
                }
            }
        }
        case 11: {
            int32_t length = 0;
            if (!reader->varInt(&length) || length < 0 ||
                static_cast<uint32_t>(length) > kMaximumNbtNodes - *node_count) {
                return false;
            }
            for (int32_t index = 0; index < length; ++index) {
                int32_t ignored = 0;
                if (!reader->varInt(&ignored)) return false;
            }
            *node_count += static_cast<uint32_t>(length);
            return true;
        }
        case 12: {
            int32_t length = 0;
            if (!reader->varInt(&length) || length < 0 ||
                static_cast<uint32_t>(length) > kMaximumNbtNodes - *node_count) {
                return false;
            }
            for (int32_t index = 0; index < length; ++index) {
                int64_t ignored = 0;
                if (!reader->varLong(&ignored)) return false;
            }
            *node_count += static_cast<uint32_t>(length);
            return true;
        }
        default:
            return false;
    }
}

bool skipNetworkCompound(Reader* reader) {
    uint8_t root_type = 0;
    uint32_t node_count = 0;
    return reader && reader->byte(&root_type) && root_type == 10U &&
        reader->skipString(kMaximumNbtStringBytes) &&
        skipNbtPayload(reader, root_type, kMaximumNbtDepth, &node_count);
}

bool readPacketId(std::string_view packet, uint32_t* packet_id) {
    Reader reader(packet);
    uint32_t header = 0;
    if (!packet_id || !reader.varUInt(&header)) return false;
    *packet_id = header & 0x3FFU;
    return true;
}

bool parseRegistryPacket(std::string_view packet,
                         std::unordered_map<int32_t, std::string>* output) {
    if (!output || packet.empty() || packet.size() > kMaximumPacketBytes) return false;
    output->clear();
    Reader reader(packet);
    uint32_t header = 0;
    uint32_t count = 0;
    if (!reader.varUInt(&header) || (header & 0x3FFU) != kItemRegistryPacketId ||
        !reader.varUInt(&count) || count > kMaximumRegistryEntries) {
        return false;
    }
    output->reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        std::string identifier;
        uint16_t encoded_id = 0;
        uint8_t component_based = 0;
        int32_t item_version = 0;
        if (!reader.string(&identifier, kMaximumIdentifierBytes) ||
            !reader.littleEndian16(&encoded_id) || !reader.byte(&component_based) ||
            component_based > 1U || !reader.varInt(&item_version) || item_version < 0 ||
            item_version > kMaximumItemVersion ||
            !skipNetworkCompound(&reader)) {
            return false;
        }
        std::string normalized = identifier;
        if (!normalizeDeferredItemIdentifier(&normalized) ||
            normalized != identifier) continue;
        const int32_t runtime_id = static_cast<int16_t>(encoded_id);
        if (!output->emplace(runtime_id, std::move(identifier)).second) {
            return false;
        }
    }
    return reader.atEnd();
}

std::mutex g_registry_mutex;
bool g_registry_ready = false;
std::unordered_map<int32_t, std::string> g_runtime_names;

}  // namespace

void ClearItemRuntimeRegistry() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        g_registry_ready = false;
        g_runtime_names.clear();
    } catch (...) {
    }
}

void ObserveItemRuntimeRegistryPacket(std::string_view packet) noexcept {
    try {
        uint32_t packet_id = 0;
        if (!readPacketId(packet, &packet_id)) return;
        if (packet_id == kStartGamePacketId || packet_id == kDisconnectPacketId) {
            ClearItemRuntimeRegistry();
            return;
        }
        if (packet_id != kItemRegistryPacketId) return;

        std::unordered_map<int32_t, std::string> parsed;
        if (!parseRegistryPacket(packet, &parsed)) {
            return;
        }
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        g_runtime_names.swap(parsed);
        g_registry_ready = true;
    } catch (...) {
        // Keep the last complete snapshot. StartGame and Disconnect are the
        // only events allowed to invalidate it without a replacement.
    }
}

ItemRuntimeResolveStatus ResolveItemRuntimeId(int32_t runtime_id,
                                              std::string* identifier) {
    if (identifier) identifier->clear();
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    if (!g_registry_ready) return ItemRuntimeResolveStatus::RegistryUnavailable;
    const auto found = g_runtime_names.find(runtime_id);
    if (found == g_runtime_names.end()) return ItemRuntimeResolveStatus::UnknownRuntimeId;
    if (identifier) *identifier = found->second;
    return ItemRuntimeResolveStatus::Ready;
}

bool IsItemRuntimeRegistryReady() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        return g_registry_ready;
    } catch (...) {
        return false;
    }
}

size_t ItemRuntimeRegistrySize() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        return g_registry_ready ? g_runtime_names.size() : 0U;
    } catch (...) {
        return 0U;
    }
}

}  // namespace build_import
