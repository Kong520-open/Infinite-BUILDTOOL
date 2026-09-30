#include "../MapTextureObservation.h"

#include <cassert>
#include <cstdint>
#include <string>

namespace {

void varUInt(std::string* bytes, uint64_t value) {
    do {
        uint8_t next = static_cast<uint8_t>(value & 0x7FU);
        value >>= 7U;
        if (value != 0U) next |= 0x80U;
        bytes->push_back(static_cast<char>(next));
    } while (value != 0U);
}

void zigZag32(std::string* bytes, int32_t value) {
    const uint32_t raw = value >= 0 ? static_cast<uint32_t>(value) * 2U :
        static_cast<uint32_t>(-static_cast<int64_t>(value) * 2 - 1);
    varUInt(bytes, raw);
}

void zigZag64(std::string* bytes, int64_t value) {
    assert(value >= 0);
    varUInt(bytes, static_cast<uint64_t>(value) * 2U);
}

void byte(std::string* bytes, uint8_t value) {
    bytes->push_back(static_cast<char>(value));
}

void littleEndian32(std::string* bytes, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        byte(bytes, static_cast<uint8_t>(value >> shift));
    }
}

std::string texturePacket(int64_t map_id, int32_t origin_x, int32_t origin_z,
                          int32_t x_offset, int32_t y_offset,
                          int32_t width, int32_t height,
                          uint32_t first_color, uint32_t other_color,
                          bool creation = false, bool decoration = false,
                          uint8_t encoding = 3U, bool zero_filled = false) {
    std::string bytes;
    // Bedrock packet header includes a packet ID in the low ten bits, with
    // sub-client routing bits above it. The receive hook sees this full header.
    varUInt(&bytes, 0x43U | (2U << 10U));
    zigZag64(&bytes, map_id);
    varUInt(&bytes, 0x02U | (creation ? 0x08U : 0U) |
                    (decoration ? 0x04U : 0U));
    byte(&bytes, 0U);  // Overworld.
    byte(&bytes, 0U);  // Not locked.
    zigZag32(&bytes, origin_x);
    zigZag32(&bytes, 70);
    zigZag32(&bytes, origin_z);
    if (creation) {
        varUInt(&bytes, 1U);
        zigZag64(&bytes, map_id);
    }
    byte(&bytes, 0U);  // Scale 0.
    if (decoration) {
        varUInt(&bytes, 1U);  // One tracked entity.
        littleEndian32(&bytes, 0U);
        zigZag64(&bytes, 77);
        varUInt(&bytes, 1U);  // One decoration.
        byte(&bytes, 0U);   // Type.
        byte(&bytes, 0U);   // Rotation.
        byte(&bytes, 1U);   // X.
        byte(&bytes, 2U);   // Y.
        varUInt(&bytes, 1U);
        byte(&bytes, static_cast<uint8_t>('x'));
        varUInt(&bytes, 0xFF000000U);
    }
    zigZag32(&bytes, width);
    zigZag32(&bytes, height);
    zigZag32(&bytes, x_offset);
    zigZag32(&bytes, y_offset);
    // This game's map reader has a zero-fill byte and a pixel encoding byte
    // before the payload; the generic Bedrock varint-color list lacks both.
    byte(&bytes, zero_filled ? 1U : 0U);
    if (zero_filled) return bytes;
    byte(&bytes, encoding);
    const uint32_t count = static_cast<uint32_t>(width * height);
    varUInt(&bytes, count);
    if (encoding == 3U) {
        for (uint32_t i = 0; i < count; ++i) {
            littleEndian32(&bytes, i == 0U ? first_color : other_color);
        }
    } else if (encoding == 1U || encoding == 2U) {
        for (uint32_t i = 0; i < count; ++i) {
            byte(&bytes, i == 0U ? 1U : 0U);
            if (encoding == 2U) byte(&bytes, 0U);
        }
        varUInt(&bytes, 2U);
        littleEndian32(&bytes, first_color);
        byte(&bytes, 1U);
        if (encoding == 2U) byte(&bytes, 0U);
        littleEndian32(&bytes, other_color);
        byte(&bytes, 0U);
        if (encoding == 2U) byte(&bytes, 0U);
    }
    return bytes;
}

}  // namespace

int main() {
    using namespace build_import;
    ClearMapTextureObservations();
    const uint64_t before = GetMapTextureReceiveSequence();
    MapTextureObservation snapshot;
    MapTextureRectangleCoverage coverage;
    assert(!GetLatestMapTextureObservation(&snapshot));
    assert(!GetMapTextureRectangleCoverageById(42, 0, 0, 1, 1, &coverage));
    assert(!GetMapTextureRectangleCoverageById(42, 0, 0, 1, 1, nullptr));
    assert(!GetMapTextureRectangleCoverageById(-1, 0, 0, 1, 1, &coverage));
    assert(!GetMapTextureRectangleCoverageById(42, -1, 0, 1, 1, &coverage));
    assert(!GetMapTextureRectangleCoverageById(42, 0, -1, 1, 1, &coverage));
    assert(!GetMapTextureRectangleCoverageById(42, 0, 0, 0, 1, &coverage));
    assert(!GetMapTextureRectangleCoverageById(42, 0, 0, 1, 0, &coverage));
    assert(!GetMapTextureRectangleCoverageById(42, 127, 0, 2, 1, &coverage));
    assert(!GetMapTextureRectangleCoverageById(42, 0, 127, 1, 2, &coverage));

    const std::string first = texturePacket(42, 64, -64, 0, 0, 2, 1,
                                            0xFF000000U, 0U, true, true);
    ObserveMapTexturePacket(first);
    assert(GetMapTextureObservationById(42, &snapshot));
    assert(snapshot.receive_sequence > before);
    assert(snapshot.texture_sequence == snapshot.receive_sequence);
    assert(snapshot.map_id == 42 && snapshot.origin_x == 64 && snapshot.origin_z == -64);
    assert(snapshot.creation_seen && snapshot.scale_known && snapshot.scale == 0);
    assert(snapshot.last_x_offset == 0 && snapshot.last_y_offset == 0);
    assert(snapshot.last_width == 2 && snapshot.last_height == 1);
    assert(snapshot.covered_pixel_count == 2 && snapshot.nonzero_pixel_count == 1);
    assert(GetMapTextureRectangleCoverageById(42, 0, 0, 2, 1, &coverage));
    assert(coverage.pixel_count == 2U && coverage.covered_pixel_count == 2U);
    assert(coverage.nonzero_pixel_count == 1U && coverage.fully_covered);
    assert(coverage.observation.map_id == 42 &&
           coverage.observation.texture_sequence == snapshot.texture_sequence);
    assert(GetMapTextureRectangleCoverageById(42, 0, 0, 3, 1, &coverage));
    assert(coverage.pixel_count == 3U && coverage.covered_pixel_count == 2U);
    assert(!coverage.fully_covered);

    ObserveMapTexturePacket(texturePacket(45, 0, 0, 0, 0, 2, 1,
                                           0xFF000000U, 0U, false, false, 1U));
    assert(GetMapTextureRectangleCoverageById(45, 0, 0, 2, 1, &coverage));
    assert(coverage.fully_covered && coverage.nonzero_pixel_count == 1U);
    ObserveMapTexturePacket(texturePacket(46, 0, 0, 0, 0, 2, 1,
                                           0xFF000000U, 0U, false, false, 2U));
    assert(GetMapTextureRectangleCoverageById(46, 0, 0, 2, 1, &coverage));
    assert(coverage.fully_covered && coverage.nonzero_pixel_count == 1U);
    ObserveMapTexturePacket(texturePacket(47, 0, 0, 0, 0, 2, 1,
                                           0U, 0U, false, false, 3U, true));
    assert(GetMapTextureRectangleCoverageById(47, 0, 0, 2, 1, &coverage));
    assert(coverage.fully_covered && coverage.nonzero_pixel_count == 0U);

    const uint64_t sequence = GetMapTextureReceiveSequence();
    std::string truncated = first;
    truncated.pop_back();
    ObserveMapTexturePacket(truncated);
    assert(GetMapTextureReceiveSequence() == sequence);
    std::string trailing = first;
    trailing.push_back('\0');
    ObserveMapTexturePacket(trailing);
    assert(GetMapTextureReceiveSequence() == sequence);
    ObserveMapTexturePacket(texturePacket(42, 64, -64, 127, 0, 2, 1, 1U, 1U));
    assert(GetMapTextureReceiveSequence() == sequence);
    std::string wrong_pixel_count = texturePacket(42, 64, -64, 0, 0, 2, 1,
                                                   1U, 1U);
    // The raw-color count follows the zero-fill and encoding bytes.
    wrong_pixel_count[wrong_pixel_count.size() - 9U] = '\x03';
    ObserveMapTexturePacket(wrong_pixel_count);
    assert(GetMapTextureReceiveSequence() == sequence);
    ObserveMapTexturePacket(std::string(1, static_cast<char>(0x44)));
    assert(GetMapTextureReceiveSequence() == sequence);

    ObserveMapTexturePacket(texturePacket(43, 192, -64, 0, 0, 1, 1, 1U, 1U));
    ObserveMapTexturePacket(texturePacket(42, 64, -64, 1, 0, 1, 1, 1U, 1U));
    assert(GetMapTextureObservationById(42, &snapshot));
    assert(snapshot.texture_update_count == 2U);
    assert(snapshot.covered_pixel_count == 2U && snapshot.nonzero_pixel_count == 2U);
    assert(GetMapTextureObservationAtOrigin(192, -64, &snapshot));
    assert(snapshot.map_id == 43);
    assert(GetMapTextureRectangleCoverageById(43, 0, 0, 2, 1, &coverage));
    assert(coverage.covered_pixel_count == 1U && !coverage.fully_covered);
    assert(GetMapTextureRectangleCoverageById(42, 0, 0, 2, 1, &coverage));
    assert(coverage.covered_pixel_count == 2U && coverage.nonzero_pixel_count == 2U);

    ObserveMapTexturePacket(texturePacket(42, 64, -64, 0, 0, 64, 128, 1U, 1U));
    assert(GetMapTextureObservationById(42, &snapshot));
    assert(snapshot.covered_pixel_count == 64U * 128U);
    assert(GetMapTextureRectangleCoverageById(42, 0, 0, 64, 128, &coverage));
    assert(coverage.fully_covered && coverage.covered_pixel_count == 64U * 128U);
    assert(GetMapTextureRectangleCoverageById(42, 0, 0, 128, 128, &coverage));
    assert(!coverage.fully_covered && coverage.covered_pixel_count == 64U * 128U);
    ObserveMapTexturePacket(texturePacket(42, 64, -64, 64, 0, 64, 128, 1U, 1U));
    assert(GetMapTextureObservationById(42, &snapshot));
    assert(snapshot.covered_pixel_count == 128U * 128U);
    assert(snapshot.nonzero_pixel_count == 128U * 128U);
    assert(GetMapTextureRectangleCoverageById(42, 0, 0, 128, 128, &coverage));
    assert(coverage.fully_covered && coverage.pixel_count == 128U * 128U);
    assert(coverage.nonzero_pixel_count == 128U * 128U);
    ObserveMapTexturePacket(texturePacket(42, 64, -64, 127, 127, 1, 1, 0U, 0U));
    assert(GetMapTextureRectangleCoverageById(42, 127, 127, 1, 1, &coverage));
    assert(coverage.fully_covered && coverage.nonzero_pixel_count == 0U);
    assert(GetMapTextureRectangleCoverageById(42, 0, 0, 128, 128, &coverage));
    assert(coverage.fully_covered && coverage.nonzero_pixel_count ==
           128U * 128U - 1U);

    const uint64_t generation = snapshot.session_generation;
    std::string start_game;
    varUInt(&start_game, 0x0BU);
    ObserveMapTexturePacket(start_game);
    assert(!GetMapTextureObservationById(42, &snapshot));
    assert(!GetMapTextureRectangleCoverageById(42, 0, 0, 128, 128, &coverage));
    ObserveMapTexturePacket(texturePacket(44, 64, 64, 0, 0, 1, 1, 1U, 1U));
    assert(GetLatestMapTextureObservation(&snapshot));
    assert(snapshot.session_generation > generation);
}
