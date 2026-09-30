#ifndef INFINITE_TEXTURE_RENDER_CAMERA_TRACKER_H
#define INFINITE_TEXTURE_RENDER_CAMERA_TRACKER_H

#include <cstdint>

namespace build_import {

// Tracks the render camera as a fallback for export position queries when the
// embedded Python bridge is temporarily unavailable after a game update.
bool InitRenderCameraTrackerHook(uintptr_t base_address);
bool GetLatestRenderCameraPosition(float* x, float* y, float* z) noexcept;

}  // namespace build_import

#endif  // INFINITE_TEXTURE_RENDER_CAMERA_TRACKER_H
