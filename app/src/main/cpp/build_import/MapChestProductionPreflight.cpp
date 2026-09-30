#include "MapChestProductionPreflight.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#if defined(__ANDROID__)
#include "NativeWorldAccess.h"
#include "../main.h"
#include "../native_auth.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include "../tp/MinecraftUpdateHook.h"
#endif

namespace build_import {
namespace {

bool fail(std::string* error, const char* reason) {
    if (error) *error = reason;
    return false;
}

bool matches(const MapChestPreflightExpectation& expected,
             const MapChestWindowRequest& request, std::string* error) {
    if (!expected.token || expected.world_context.empty() ||
        expected.world_context == "unknown" ||
        expected.max_distance_blocks < 1U ||
        expected.max_distance_blocks > 4U || expected.face < 0 ||
        expected.face > 5) {
        return fail(error, "map chest expectation is incomplete or unsafe");
    }
    if (request.token != expected.token || request.x != expected.x ||
        request.y != expected.y || request.z != expected.z ||
        request.face != expected.face) {
        return fail(error, "map chest request does not match its task identity");
    }
    return true;
}

bool sameWorld(const MapChestPreflightExpectation& expected,
               const MapChestPreflightObservation& observed,
               std::string* error) {
    if (!observed.authorized_game_tick || observed.world_context.empty() ||
        observed.world_context != expected.world_context) {
        return fail(error, "map chest world changed");
    }
    return true;
}

bool nearChest(const MapChestPreflightExpectation& expected,
               const MapChestPreflightObservation& observed,
               std::string* error) {
    if (!observed.player_position_available) {
        return fail(error, "fresh native player position is unavailable");
    }
    const int64_t radius = expected.max_distance_blocks;
    const int64_t dx = static_cast<int64_t>(observed.player_x) - expected.x;
    const int64_t dy = static_cast<int64_t>(observed.player_y) - expected.y;
    const int64_t dz = static_cast<int64_t>(observed.player_z) - expected.z;
    if (dx < -radius || dx > radius || dy < -radius || dy > radius ||
        dz < -radius || dz > radius ||
        dx * dx + dy * dy + dz * dz > radius * radius) {
        return fail(error, "player is too far from the map chest");
    }
    return true;
}

#if defined(__ANDROID__)
// Same version-specific LocalPlayer position getter and fingerprint as the
// anvil interaction preflight. Never use a Python/camera-position fallback.
constexpr uintptr_t kGetPlayerPositionRva = 0x0D8DFF5CULL;
constexpr std::array<uint8_t, 32U> kGetPlayerPositionFingerprint{{
    0x00U, 0x44U, 0x41U, 0xF9U, 0xC0U, 0x03U, 0x5FU, 0xD6U,
    0x08U, 0x4CU, 0x41U, 0xF9U, 0x20U, 0x00U, 0x40U, 0xFDU,
    0x00U, 0x05U, 0x00U, 0xFDU, 0xC0U, 0x03U, 0x5FU, 0xD6U,
    0x00U, 0x4CU, 0x41U, 0xF9U, 0xC0U, 0x03U, 0x5FU, 0xD6U,
}};

bool readPlayerPosition(int32_t* x, int32_t* y, int32_t* z,
                        std::string* error) {
    if (!x || !y || !z) return fail(error, "player position outputs are unavailable");
    const uintptr_t base = Main::getBaseAddress();
    uintptr_t getter = 0;
    if (!base || !ResolveMinecraftExecutableOffset(
            base, kGetPlayerPositionRva,
            kGetPlayerPositionFingerprint.size(), &getter) ||
        std::memcmp(reinterpret_cast<const void*>(getter),
                    kGetPlayerPositionFingerprint.data(),
                    kGetPlayerPositionFingerprint.size()) != 0) {
        return fail(error, "native player-position ABI fingerprint changed");
    }
    void* player = GetLocalPlayerPointer();
    if (!player || !IsMemoryReadable(player, sizeof(void*))) {
        return fail(error, "native local player is unavailable");
    }
    using GetPosition = const Vec3* (*)(void*);
    const Vec3* position = reinterpret_cast<GetPosition>(getter)(player);
    if (!position || !IsMemoryReadable(position, sizeof(Vec3)) ||
        !std::isfinite(position->x) || !std::isfinite(position->y) ||
        !std::isfinite(position->z)) {
        return fail(error, "native local-player position is not readable");
    }
    const double px = std::floor(static_cast<double>(position->x));
    const double py = std::floor(static_cast<double>(position->y));
    const double pz = std::floor(static_cast<double>(position->z));
    const double low = static_cast<double>(std::numeric_limits<int32_t>::min());
    const double high = static_cast<double>(std::numeric_limits<int32_t>::max());
    if (px < low || px > high || py < low || py > high ||
        pz < low || pz > high) {
        return fail(error, "native player position is outside block coordinates");
    }
    *x = static_cast<int32_t>(px);
    *y = static_cast<int32_t>(py);
    *z = static_cast<int32_t>(pz);
    return true;
}

bool readNativeObservation(void*, const MapChestWindowRequest& request,
                           bool include_block,
                           MapChestPreflightObservation* output,
                           std::string* error) {
    if (!output) return fail(error, "map chest observation output is unavailable");
    *output = {};
    if (!IsNativeSessionAuthorized() || !IsMinecraftUpdateGameThread()) {
        return fail(error, "map chest preflight requires an authorized game tick");
    }
    output->authorized_game_tick = true;
    if (!QueryWorldContextOnGameThread(&output->world_context, 1000)) {
        return fail(error, "live map chest world context is unavailable");
    }
    if (!include_block) {
        if (error) error->clear();
        return true;
    }
    if (!readPlayerPosition(&output->player_x, &output->player_y,
                            &output->player_z, error)) return false;
    output->player_position_available = true;
    NativeWorldReader reader;
    NativeBlockView block;
    if (!reader.open() ||
        !reader.getBlockView(request.x, request.y, request.z, &block) ||
        !block.name || !block.type_token) {
        return fail(error, "native chest Block cannot be read at the target");
    }
    output->block_available = true;
    output->block_identifier = *block.name;
    output->native_block = block.type_token;

    // A neighboring chest may have formed a double chest since the original
    // placement survey. All four neighbors must be readable and non-chests.
    constexpr std::array<std::array<int32_t, 2U>, 4U> offsets{{
        {{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}},
    }};
    for (const auto& offset : offsets) {
        const int64_t nx = static_cast<int64_t>(request.x) + offset[0];
        const int64_t nz = static_cast<int64_t>(request.z) + offset[1];
        if (nx < std::numeric_limits<int32_t>::min() ||
            nx > std::numeric_limits<int32_t>::max() ||
            nz < std::numeric_limits<int32_t>::min() ||
            nz > std::numeric_limits<int32_t>::max()) {
            return fail(error, "map chest neighbor coordinates overflow");
        }
        NativeBlockView neighbor;
        if (!reader.getBlockView(static_cast<int32_t>(nx), request.y,
                                 static_cast<int32_t>(nz), &neighbor) ||
            !neighbor.name || neighbor.name->find("chest") != std::string::npos) {
            return fail(error, "map chest is not an isolated single chest");
        }
    }
    output->isolated_single_chest = true;
    if (error) error->clear();
    return true;
}
#endif

}  // namespace

bool ReadVerifiedMapNativePlayerBlockPosition(
        int32_t* x, int32_t* y, int32_t* z, std::string* error) {
    if (error) error->clear();
    if (!x || !y || !z) {
        return fail(error, "native player position outputs are unavailable");
    }
    *x = 0;
    *y = 0;
    *z = 0;
#if defined(__ANDROID__)
    if (!IsNativeSessionAuthorized() || !IsMinecraftUpdateGameThread()) {
        return fail(error, "native player position requires an authorized game tick");
    }
    return readPlayerPosition(x, y, z, error);
#else
    return fail(error, "native player position is only available on Android");
#endif
}

bool VerifyMapChestProductionContext(
        void* context, const MapChestWindowRequest& request,
        std::string* error) {
    if (error) error->clear();
    const auto* strict = static_cast<MapChestStrictPreflightContext*>(context);
    if (!strict || !strict->read_observation ||
        !matches(strict->expected, request, error)) return false;
    MapChestPreflightObservation observed;
    std::string read_error;
    if (!strict->read_observation(strict->read_context, request, false,
                                  &observed, &read_error)) {
        if (error) *error = read_error.empty()
            ? "map chest world read failed" : read_error;
        return false;
    }
    return sameWorld(strict->expected, observed, error);
}

bool PrepareMapChestProductionOpen(
        void* context, const MapChestWindowRequest& request,
        const void** native_block, std::string* error) {
    if (native_block) *native_block = nullptr;
    if (error) error->clear();
    const auto* strict = static_cast<MapChestStrictPreflightContext*>(context);
    if (!native_block || !strict || !strict->read_observation ||
        !matches(strict->expected, request, error)) return false;
    MapChestPreflightObservation observed;
    std::string read_error;
    if (!strict->read_observation(strict->read_context, request, true,
                                  &observed, &read_error)) {
        if (error) *error = read_error.empty()
            ? "map chest block read failed" : read_error;
        return false;
    }
    if (!sameWorld(strict->expected, observed, error) ||
        !nearChest(strict->expected, observed, error)) return false;
    if (!observed.block_available || !observed.native_block ||
        !observed.isolated_single_chest ||
        observed.block_identifier != "minecraft:chest") {
        return fail(error, "target is not a readable isolated single chest");
    }
    *native_block = observed.native_block;
    if (error) error->clear();
    return true;
}

#if defined(__ANDROID__)
MapChestNativePreflight MakeNativeMapChestProductionPreflight(
        MapChestStrictPreflightContext* context) noexcept {
    if (!context) return {};
    context->read_context = nullptr;
    context->read_observation = &readNativeObservation;
    return {context, &PrepareMapChestProductionOpen};
}
#endif

}  // namespace build_import
