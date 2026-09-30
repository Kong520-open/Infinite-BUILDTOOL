#include "MapTextureObservation.h"

#if defined(__ANDROID__)
#define LOG_TAG "Infinitecz_MapTexture"
#include "../log_control.h"
#else
#define LOGI(...) std::printf(__VA_ARGS__)
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string_view>

namespace build_import {
namespace {

// The 1.21.120 / protocol 859 map header matches minecraft-data's Bedrock
// schema, but the target game's pixel payload has a different encoding.
constexpr uint32_t kMapPacketId = 0x43U;
constexpr uint32_t kStartGamePacketId = 0x0BU;
constexpr uint32_t kDisconnectPacketId = 0x05U;
constexpr uint32_t kChangeDimensionPacketId = 0x3DU;
constexpr uint32_t kTextureFlag = 0x02U;
constexpr uint32_t kDecorationFlag = 0x04U;
constexpr uint32_t kCreationFlag = 0x08U;
constexpr uint32_t kKnownFlags = 0x0FU;
constexpr size_t kMapSide = 128U;
constexpr size_t kPixelCount = kMapSide * kMapSide;
constexpr size_t kBitsetWords = kPixelCount / 64U;
constexpr size_t kHistoryCapacity = 32U;
constexpr size_t kMaxPacketBytes = 512U * 1024U;
constexpr uint32_t kMaxIncludedMapIds = 1024U;
constexpr uint32_t kMaxDecorations = 1024U;
constexpr uint32_t kMaxDecorationLabelBytes = 4096U;

class Reader {
public:
    explicit Reader(std::string_view input) : input_(input) {}

    bool byte(uint8_t* out) noexcept {
        if (!out || cursor_ >= input_.size()) return false;
        *out = static_cast<uint8_t>(input_[cursor_++]);
        return true;
    }

    bool littleEndian32(uint32_t* out) noexcept {
        if (!out || input_.size() - cursor_ < 4U) return false;
        *out = static_cast<uint32_t>(static_cast<uint8_t>(input_[cursor_])) |
               (static_cast<uint32_t>(static_cast<uint8_t>(input_[cursor_ + 1U])) << 8U) |
               (static_cast<uint32_t>(static_cast<uint8_t>(input_[cursor_ + 2U])) << 16U) |
               (static_cast<uint32_t>(static_cast<uint8_t>(input_[cursor_ + 3U])) << 24U);
        cursor_ += 4U;
        return true;
    }

    bool littleEndian16(uint16_t* out) noexcept {
        if (!out || input_.size() - cursor_ < 2U) return false;
        *out = static_cast<uint16_t>(static_cast<uint8_t>(input_[cursor_])) |
               static_cast<uint16_t>(static_cast<uint8_t>(input_[cursor_ + 1U])) << 8U;
        cursor_ += 2U;
        return true;
    }

    bool varUInt32(uint32_t* out) noexcept {
        if (!out) return false;
        uint32_t value = 0;
        for (uint32_t index = 0; index < 5U; ++index) {
            uint8_t byte_value = 0;
            if (!byte(&byte_value) || (index == 4U && (byte_value & 0xF0U) != 0U)) {
                return false;
            }
            value |= static_cast<uint32_t>(byte_value & 0x7FU) << (index * 7U);
            if ((byte_value & 0x80U) == 0U) {
                *out = value;
                return true;
            }
        }
        return false;
    }

    bool varUInt64(uint64_t* out) noexcept {
        if (!out) return false;
        uint64_t value = 0;
        for (uint32_t index = 0; index < 10U; ++index) {
            uint8_t byte_value = 0;
            if (!byte(&byte_value) || (index == 9U && (byte_value & 0xFEU) != 0U)) {
                return false;
            }
            value |= static_cast<uint64_t>(byte_value & 0x7FU) << (index * 7U);
            if ((byte_value & 0x80U) == 0U) {
                *out = value;
                return true;
            }
        }
        return false;
    }

    bool zigZag32(int32_t* out) noexcept {
        uint32_t raw = 0;
        if (!out || !varUInt32(&raw)) return false;
        const uint32_t magnitude = raw >> 1U;
        if ((raw & 1U) == 0U) {
            *out = static_cast<int32_t>(magnitude);
        } else if (magnitude == static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
            *out = std::numeric_limits<int32_t>::min();
        } else {
            *out = -static_cast<int32_t>(magnitude + 1U);
        }
        return true;
    }

    bool zigZag64(int64_t* out) noexcept {
        uint64_t raw = 0;
        if (!out || !varUInt64(&raw)) return false;
        const uint64_t magnitude = raw >> 1U;
        if ((raw & 1U) == 0U) {
            *out = static_cast<int64_t>(magnitude);
        } else if (magnitude == static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            *out = std::numeric_limits<int64_t>::min();
        } else {
            *out = -static_cast<int64_t>(magnitude + 1U);
        }
        return true;
    }

    bool skipString(uint32_t maximum_bytes) noexcept {
        uint32_t length = 0;
        if (!varUInt32(&length) || length > maximum_bytes ||
            length > input_.size() - cursor_) {
            return false;
        }
        cursor_ += length;
        return true;
    }

    bool atEnd() const noexcept { return cursor_ == input_.size(); }
    size_t position() const noexcept { return cursor_; }

private:
    std::string_view input_;
    size_t cursor_ = 0;
};

struct ParsedMapPacket {
    MapTextureObservation snapshot;
    bool has_texture = false;
    bool has_creation = false;
    std::array<uint8_t, kPixelCount> pixels_nonzero{};
};

// The target game's ClientboundMapItemDataPacket reader (libminecraftpe.so,
// build-id ccf9c31f3121a46e89d868dcedb239466b3ac726) reads a zero-fill
// boolean and then, when false, one of three pixel encodings. Palette entries
// map a little-endian uint32 color to an 8- or 16-bit pixel index. Only whether a color
// is zero is needed here; coverage still requires the entire payload.
bool readTexturePixels(Reader* reader, uint32_t pixel_count,
                       std::array<uint8_t, kPixelCount>* pixels_nonzero) noexcept {
    if (!reader || !pixels_nonzero || pixel_count == 0U || pixel_count > kPixelCount) {
        return false;
    }
    uint8_t zero_filled = 0;
    if (!reader->byte(&zero_filled)) return false;
    if (zero_filled != 0U) return true;

    uint8_t encoding = 0;
    uint32_t count = 0;
    if (!reader->byte(&encoding) || encoding < 1U || encoding > 3U ||
        !reader->varUInt32(&count) || count != pixel_count) {
        return false;
    }
    if (encoding == 3U) {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t color = 0;
            if (!reader->littleEndian32(&color)) return false;
            (*pixels_nonzero)[i] = color != 0U ? 1U : 0U;
        }
        return true;
    }

    // A palette index is stored for every pixel. The network palette then
    // contains (color, index) pairs, so a sparse bitset is enough to resolve
    // nonzero pixels without allocating a 64K-entry color table in the hook.
    std::array<uint16_t, kPixelCount> indices{};
    for (uint32_t i = 0; i < count; ++i) {
        if (encoding == 1U) {
            uint8_t index = 0;
            if (!reader->byte(&index)) return false;
            indices[i] = index;
        } else if (!reader->littleEndian16(&indices[i])) {
            return false;
        }
    }
    uint32_t palette_count = 0;
    constexpr size_t kPaletteIndexCount = 1U << 16U;
    if (!reader->varUInt32(&palette_count) ||
        palette_count > (encoding == 1U ? 256U : kPaletteIndexCount)) {
        return false;
    }
    std::array<uint64_t, kPaletteIndexCount / 64U> nonzero_palette{};
    for (uint32_t i = 0; i < palette_count; ++i) {
        uint32_t color = 0;
        uint16_t index = 0;
        if (!reader->littleEndian32(&color)) return false;
        if (encoding == 1U) {
            uint8_t narrow_index = 0;
            if (!reader->byte(&narrow_index)) return false;
            index = narrow_index;
        } else if (!reader->littleEndian16(&index)) {
            return false;
        }
        const uint64_t bit = uint64_t{1} << (index % 64U);
        if (color != 0U) nonzero_palette[index / 64U] |= bit;
        else nonzero_palette[index / 64U] &= ~bit;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t index = indices[i];
        (*pixels_nonzero)[i] =
            (nonzero_palette[index / 64U] >> (index % 64U)) & 1U;
    }
    return true;
}

struct StoredMap {
    bool occupied = false;
    MapTextureObservation snapshot;
    std::array<uint64_t, kBitsetWords> covered{};
    std::array<uint64_t, kBitsetWords> nonzero{};
};

std::mutex g_mutex;
std::array<StoredMap, kHistoryCapacity> g_maps{};
uint64_t g_session_generation = 1U;
uint64_t g_receive_sequence = 0U;
std::atomic<uint64_t> g_map_packet_attempts{0U};
std::atomic<uint64_t> g_map_packet_rejections{0U};
std::atomic<uint64_t> g_outbound_map_info_attempts{0U};
std::atomic<bool> g_map_diagnostics_armed{false};

bool readBlockCoordinates(Reader* reader) noexcept {
    int32_t x = 0;
    uint32_t y = 0;
    int32_t z = 0;
    return reader && reader->zigZag32(&x) && reader->varUInt32(&y) &&
           reader->zigZag32(&z);
}

bool skipDecorations(Reader* reader) noexcept {
    uint32_t tracked_count = 0;
    if (!reader || !reader->varUInt32(&tracked_count) || tracked_count > kMaxDecorations) {
        return false;
    }
    for (uint32_t i = 0; i < tracked_count; ++i) {
        uint32_t type = 0;
        if (!reader->littleEndian32(&type)) return false;
        if (type == 0U) {
            int64_t ignored_id = 0;
            if (!reader->zigZag64(&ignored_id)) return false;
        } else if (type == 1U) {
            if (!readBlockCoordinates(reader)) return false;
        } else if (type != 2U) {
            // Type 2 has no payload in the Mojang enum; unknown values may
            // carry new fields, so fail closed rather than lose alignment.
            return false;
        }
    }
    uint32_t decoration_count = 0;
    if (!reader->varUInt32(&decoration_count) || decoration_count > kMaxDecorations) {
        return false;
    }
    for (uint32_t i = 0; i < decoration_count; ++i) {
        uint8_t ignored = 0;
        uint32_t ignored_color = 0;
        if (!reader->byte(&ignored) || !reader->byte(&ignored) ||
            !reader->byte(&ignored) || !reader->byte(&ignored) ||
            !reader->skipString(kMaxDecorationLabelBytes) ||
            !reader->varUInt32(&ignored_color)) {
            return false;
        }
    }
    return true;
}

bool parseMapPacket(Reader* reader, ParsedMapPacket* out) noexcept {
    if (!reader || !out) return false;
    uint32_t flags = 0;
    uint8_t locked = 0;
    if (!reader->zigZag64(&out->snapshot.map_id) ||
        !reader->varUInt32(&flags) || (flags & ~kKnownFlags) != 0U ||
        !reader->byte(&out->snapshot.dimension) ||
        !reader->byte(&locked) || locked > 1U ||
        !reader->zigZag32(&out->snapshot.origin_x) ||
        !reader->zigZag32(&out->snapshot.origin_y) ||
        !reader->zigZag32(&out->snapshot.origin_z)) {
        return false;
    }
    out->snapshot.locked = locked != 0U;
    out->has_creation = (flags & kCreationFlag) != 0U;
    out->has_texture = (flags & kTextureFlag) != 0U;
    if (out->has_creation) {
        uint32_t count = 0;
        if (!reader->varUInt32(&count) || count > kMaxIncludedMapIds) return false;
        for (uint32_t i = 0; i < count; ++i) {
            int64_t ignored_id = 0;
            if (!reader->zigZag64(&ignored_id)) return false;
        }
    }
    if ((flags & (kCreationFlag | kDecorationFlag | kTextureFlag)) != 0U) {
        if (!reader->byte(&out->snapshot.scale)) return false;
        out->snapshot.scale_known = true;
    }
    if ((flags & kDecorationFlag) != 0U && !skipDecorations(reader)) return false;
    if (out->has_texture) {
        auto& snapshot = out->snapshot;
        if (!reader->zigZag32(&snapshot.last_width) ||
            !reader->zigZag32(&snapshot.last_height) ||
            !reader->zigZag32(&snapshot.last_x_offset) ||
            !reader->zigZag32(&snapshot.last_y_offset) ||
            snapshot.last_width < 1 || snapshot.last_height < 1 ||
            snapshot.last_x_offset < 0 || snapshot.last_y_offset < 0 ||
            snapshot.last_width > static_cast<int32_t>(kMapSide) ||
            snapshot.last_height > static_cast<int32_t>(kMapSide) ||
            snapshot.last_x_offset > static_cast<int32_t>(kMapSide) - snapshot.last_width ||
            snapshot.last_y_offset > static_cast<int32_t>(kMapSide) - snapshot.last_height) {
            return false;
        }
        if (!readTexturePixels(reader,
                static_cast<uint32_t>(snapshot.last_width * snapshot.last_height),
                &out->pixels_nonzero)) return false;
    }
    return reader->atEnd();
}

StoredMap* findSlot(int64_t map_id) noexcept {
    StoredMap* replacement = &g_maps[0];
    StoredMap* first_empty = nullptr;
    for (StoredMap& entry : g_maps) {
        if (entry.occupied && entry.snapshot.map_id == map_id) return &entry;
        if (!entry.occupied && !first_empty) first_empty = &entry;
        if (entry.occupied &&
            entry.snapshot.receive_sequence < replacement->snapshot.receive_sequence) {
            replacement = &entry;
        }
    }
    return first_empty ? first_empty : replacement;
}

void applyParsed(const ParsedMapPacket& parsed) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    StoredMap* entry = findSlot(parsed.snapshot.map_id);
    const bool reset = !entry->occupied ||
        entry->snapshot.map_id != parsed.snapshot.map_id ||
        entry->snapshot.dimension != parsed.snapshot.dimension ||
        entry->snapshot.origin_x != parsed.snapshot.origin_x ||
        entry->snapshot.origin_z != parsed.snapshot.origin_z;
    if (reset) *entry = {};
    entry->occupied = true;
    auto& state = entry->snapshot;
    state.session_generation = g_session_generation;
    state.receive_sequence = ++g_receive_sequence;
    state.map_id = parsed.snapshot.map_id;
    state.origin_x = parsed.snapshot.origin_x;
    state.origin_y = parsed.snapshot.origin_y;
    state.origin_z = parsed.snapshot.origin_z;
    state.dimension = parsed.snapshot.dimension;
    state.locked = parsed.snapshot.locked;
    if (parsed.snapshot.scale_known) {
        state.scale = parsed.snapshot.scale;
        state.scale_known = true;
    }
    state.creation_seen = state.creation_seen || parsed.has_creation;
    if (!parsed.has_texture) return;

    state.texture_sequence = state.receive_sequence;
    state.last_x_offset = parsed.snapshot.last_x_offset;
    state.last_y_offset = parsed.snapshot.last_y_offset;
    state.last_width = parsed.snapshot.last_width;
    state.last_height = parsed.snapshot.last_height;
    ++state.texture_update_count;
    const size_t width = static_cast<size_t>(state.last_width);
    const size_t height = static_cast<size_t>(state.last_height);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const size_t index = (y + static_cast<size_t>(state.last_y_offset)) * kMapSide +
                                 x + static_cast<size_t>(state.last_x_offset);
            const size_t word = index / 64U;
            const uint64_t bit = uint64_t{1} << (index % 64U);
            if ((entry->covered[word] & bit) == 0U) {
                entry->covered[word] |= bit;
                ++state.covered_pixel_count;
            }
            const bool was_nonzero = (entry->nonzero[word] & bit) != 0U;
            const bool is_nonzero = parsed.pixels_nonzero[y * width + x] != 0U;
            if (was_nonzero != is_nonzero) {
                if (is_nonzero) {
                    entry->nonzero[word] |= bit;
                    ++state.nonzero_pixel_count;
                } else {
                    entry->nonzero[word] &= ~bit;
                    --state.nonzero_pixel_count;
                }
            }
        }
    }
}

}  // namespace

void ObserveMapTexturePacket(std::string_view packet) noexcept {
    if (packet.empty() || packet.size() > kMaxPacketBytes) return;
    Reader reader(packet);
    uint32_t header = 0;
    if (!reader.varUInt32(&header)) return;
    const uint32_t packet_id = header & 0x3FFU;
    if (packet_id == kStartGamePacketId || packet_id == kDisconnectPacketId ||
        packet_id == kChangeDimensionPacketId) {
        ClearMapTextureObservations();
        return;
    }
    if (packet_id != kMapPacketId) return;
    const uint64_t attempt = g_map_packet_attempts.fetch_add(
        1U, std::memory_order_relaxed) + 1U;
    const bool sample = attempt <= 24U || (attempt & 31U) == 0U;
    ParsedMapPacket parsed;
    if (parseMapPacket(&reader, &parsed)) {
        applyParsed(parsed);
        if (sample) {
            LOGI("[map-packet] accepted #%llu bytes=%zu id=%lld origin=(%d,%d,%d) "
                 "dimension=%u scale_known=%d scale=%u texture=%d rect=(%d,%d,%d,%d)",
                 static_cast<unsigned long long>(attempt), packet.size(),
                 static_cast<long long>(parsed.snapshot.map_id),
                 parsed.snapshot.origin_x, parsed.snapshot.origin_y,
                 parsed.snapshot.origin_z,
                 static_cast<unsigned>(parsed.snapshot.dimension),
                 parsed.snapshot.scale_known ? 1 : 0,
                 static_cast<unsigned>(parsed.snapshot.scale),
                 parsed.has_texture ? 1 : 0,
                 parsed.snapshot.last_x_offset, parsed.snapshot.last_y_offset,
                 parsed.snapshot.last_width, parsed.snapshot.last_height);
        }
    } else if (const uint64_t rejected = g_map_packet_rejections.fetch_add(
                   1U, std::memory_order_relaxed) + 1U;
               sample || rejected <= 64U ||
                   (IsMapTextureDiagnosticsArmed() && rejected <= 512U)) {
        // A failed map packet can contain both decorations and pixels. Keep
        // the entire short packet so a parser offset can be matched to the
        // actual wire field without requiring another map creation attempt.
        char prefix[769]{};
        const size_t count = std::min<size_t>(packet.size(), 256U);
        for (size_t index = 0; index < count; ++index) {
            std::snprintf(prefix + index * 3U, sizeof(prefix) - index * 3U,
                          "%02X ", static_cast<unsigned>(
                              static_cast<uint8_t>(packet[index])));
        }
        LOGI("[map-packet] rejected #%llu rejected=%llu bytes=%zu offset=%zu id_so_far=%lld "
             "origin_so_far=(%d,%d,%d) texture=%d rect=(%d,%d,%d,%d) hex=%s",
             static_cast<unsigned long long>(attempt),
             static_cast<unsigned long long>(rejected), packet.size(),
             reader.position(), static_cast<long long>(parsed.snapshot.map_id),
             parsed.snapshot.origin_x, parsed.snapshot.origin_y,
             parsed.snapshot.origin_z, parsed.has_texture ? 1 : 0,
             parsed.snapshot.last_x_offset, parsed.snapshot.last_y_offset,
             parsed.snapshot.last_width, parsed.snapshot.last_height, prefix);
    }
}

bool GetMapTextureObservationById(int64_t map_id, MapTextureObservation* output) noexcept {
    if (!output) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const StoredMap& entry : g_maps) {
        if (entry.occupied && entry.snapshot.map_id == map_id) {
            *output = entry.snapshot;
            return true;
        }
    }
    return false;
}

bool GetMapTextureRectangleCoverageById(int64_t map_id, int32_t x_offset,
                                        int32_t y_offset, int32_t width,
                                        int32_t height,
                                        MapTextureRectangleCoverage* output) noexcept {
    if (!output || map_id == -1 || x_offset < 0 || y_offset < 0 ||
        width < 1 || height < 1 ||
        width > static_cast<int32_t>(kMapSide) ||
        height > static_cast<int32_t>(kMapSide) ||
        x_offset > static_cast<int32_t>(kMapSide) - width ||
        y_offset > static_cast<int32_t>(kMapSide) - height) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    for (const StoredMap& entry : g_maps) {
        if (!entry.occupied || entry.snapshot.map_id != map_id) continue;

        MapTextureRectangleCoverage result;
        result.observation = entry.snapshot;
        result.pixel_count = static_cast<uint32_t>(width * height);
        for (int32_t y = y_offset; y < y_offset + height; ++y) {
            for (int32_t x = x_offset; x < x_offset + width; ++x) {
                const size_t index = static_cast<size_t>(y) * kMapSide +
                                     static_cast<size_t>(x);
                const size_t word = index / 64U;
                const uint64_t bit = uint64_t{1} << (index % 64U);
                if ((entry.covered[word] & bit) != 0U) {
                    ++result.covered_pixel_count;
                    if ((entry.nonzero[word] & bit) != 0U) {
                        ++result.nonzero_pixel_count;
                    }
                }
            }
        }
        result.fully_covered = result.covered_pixel_count == result.pixel_count;
        *output = result;
        return true;
    }
    return false;
}

bool GetLatestMapTextureObservation(MapTextureObservation* output) noexcept {
    if (!output) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    const StoredMap* latest = nullptr;
    for (const StoredMap& entry : g_maps) {
        if (entry.occupied &&
            (!latest || entry.snapshot.receive_sequence > latest->snapshot.receive_sequence)) {
            latest = &entry;
        }
    }
    if (!latest) return false;
    *output = latest->snapshot;
    return true;
}

bool GetMapTextureObservationAtOrigin(int32_t origin_x, int32_t origin_z,
                                      MapTextureObservation* output) noexcept {
    if (!output) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    const StoredMap* latest = nullptr;
    for (const StoredMap& entry : g_maps) {
        if (entry.occupied && entry.snapshot.origin_x == origin_x &&
            entry.snapshot.origin_z == origin_z &&
            (!latest || entry.snapshot.receive_sequence > latest->snapshot.receive_sequence)) {
            latest = &entry;
        }
    }
    if (!latest) return false;
    *output = latest->snapshot;
    return true;
}

uint64_t GetMapTextureReceiveSequence() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_receive_sequence;
}

void ClearMapTextureObservations() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (StoredMap& entry : g_maps) entry = {};
    ++g_session_generation;
}

void ArmMapTextureDiagnostics(bool armed) noexcept {
    if (armed && !g_map_diagnostics_armed.exchange(true, std::memory_order_acq_rel)) {
        g_outbound_map_info_attempts.store(0U, std::memory_order_release);
    } else if (!armed) {
        g_map_diagnostics_armed.store(false, std::memory_order_release);
    }
}

bool IsMapTextureDiagnosticsArmed() noexcept {
    return g_map_diagnostics_armed.load(std::memory_order_acquire);
}

void RecordOutboundMapInfoRequestAttempt() noexcept {
    if (!IsMapTextureDiagnosticsArmed()) return;
    const uint64_t attempt = g_outbound_map_info_attempts.fetch_add(
        1U, std::memory_order_relaxed) + 1U;
    if (attempt <= 12U || (attempt & 31U) == 0U) {
        LOGI("[map-info-out] attempt #%llu",
             static_cast<unsigned long long>(attempt));
    }
}

uint64_t GetOutboundMapInfoRequestAttemptCount() noexcept {
    return g_outbound_map_info_attempts.load(std::memory_order_acquire);
}

}  // namespace build_import
