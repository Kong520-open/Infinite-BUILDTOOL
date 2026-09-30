#pragma once

#include <cstdint>
#include <string_view>

namespace build_import {

// Passive protocol-859 ClientboundMapItemData (packet 67) evidence. Receiving
// pixels proves only that the client was sent this texture data; it does not
// prove that the server persisted a filled map item.
struct MapTextureObservation {
    uint64_t session_generation = 0;
    uint64_t receive_sequence = 0;
    uint64_t texture_sequence = 0;
    int64_t map_id = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    int32_t origin_z = 0;
    uint8_t dimension = 0;
    uint8_t scale = 0;
    bool scale_known = false;
    bool locked = false;
    bool creation_seen = false;
    uint32_t texture_update_count = 0;
    int32_t last_x_offset = 0;
    int32_t last_y_offset = 0;
    int32_t last_width = 0;
    int32_t last_height = 0;
    // Union of pixel positions covered by validated texture rectangles.
    uint32_t covered_pixel_count = 0;
    // Most recent received colour at each covered position was nonzero.
    uint32_t nonzero_pixel_count = 0;
};

struct MapTextureRectangleCoverage {
    MapTextureObservation observation;
    uint32_t pixel_count = 0;
    uint32_t covered_pixel_count = 0;
    uint32_t nonzero_pixel_count = 0;
    bool fully_covered = false;
};

// Safe to call on the receive-hook thread. Malformed packets are ignored.
void ObserveMapTexturePacket(std::string_view packet) noexcept;

// Snapshots are copied under a mutex; callers never retain packet pointers.
bool GetMapTextureObservationById(int64_t map_id, MapTextureObservation* output) noexcept;
// Looks up only this map ID, then checks the union of validated texture
// updates inside a rectangle bounded by the map's 128x128 pixel grid. A true
// return means the map was observed, not that the rectangle is fully covered.
// The copied observation includes the session and texture sequence so callers
// can reject data predating their map-use operation. Pixel value zero is valid
// coverage; nonzero_pixel_count is diagnostic only.
bool GetMapTextureRectangleCoverageById(int64_t map_id, int32_t x_offset,
                                        int32_t y_offset, int32_t width,
                                        int32_t height,
                                        MapTextureRectangleCoverage* output) noexcept;
bool GetLatestMapTextureObservation(MapTextureObservation* output) noexcept;
bool GetMapTextureObservationAtOrigin(int32_t origin_x, int32_t origin_z,
                                      MapTextureObservation* output) noexcept;
uint64_t GetMapTextureReceiveSequence() noexcept;
void ClearMapTextureObservations() noexcept;

// While automatic map creation runs, the existing send hook records whether
// the client attempted MapInfoRequest (packet 68). This does not claim the
// server accepted the packet or that any pixel was saved.
void ArmMapTextureDiagnostics(bool armed) noexcept;
bool IsMapTextureDiagnosticsArmed() noexcept;
void RecordOutboundMapInfoRequestAttempt() noexcept;
uint64_t GetOutboundMapInfoRequestAttemptCount() noexcept;

}  // namespace build_import
