#include "MapVisibleAnvilProductionPreflight.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(__ANDROID__)
#include "NativeWorldAccess.h"
#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include "../tp/MinecraftUpdateHook.h"
#endif

namespace build_import {
namespace {

bool fail(std::string* error, const char* reason) {
    if (error) *error = reason;
    return false;
}

bool requestMatches(const MapVisibleAnvilPreflightExpectation& expected,
                    const MapVisibleAnvilWindowRequest& request,
                    std::string* error) {
    if (expected.token == 0U || expected.world_context.empty() ||
        expected.world_context == "unknown" ||
        expected.max_distance_blocks < 1U ||
        expected.max_distance_blocks > 4U || expected.face < 0 ||
        expected.face > 5) {
        return fail(error, "visible anvil expectation is incomplete or unsafe");
    }
    if (request.token != expected.token || request.x != expected.x ||
        request.y != expected.y || request.z != expected.z ||
        request.face != expected.face) {
        return fail(error, "visible anvil request does not match its task identity");
    }
    return true;
}

bool sameAuthorizedWorld(const MapVisibleAnvilPreflightExpectation& expected,
                         const MapVisibleAnvilPreflightObservation& observed,
                         std::string* error) {
    if (!observed.authorized_game_tick || observed.world_context.empty() ||
        observed.world_context == "unknown" ||
        observed.world_context != expected.world_context) {
        return fail(error, "visible anvil world changed");
    }
    return true;
}

bool nearAnvil(const MapVisibleAnvilPreflightExpectation& expected,
               const MapVisibleAnvilPreflightObservation& observed,
               std::string* error) {
    if (!observed.player_position_available) {
        return fail(error, "fresh player position is unavailable");
    }
    const int64_t radius = expected.max_distance_blocks;
    const int64_t dx = static_cast<int64_t>(observed.player_x) - expected.x;
    const int64_t dy = static_cast<int64_t>(observed.player_y) - expected.y;
    const int64_t dz = static_cast<int64_t>(observed.player_z) - expected.z;
    // Reject per-axis first; raw int32 coordinate deltas can square beyond
    // int64 range at world extremes, whereas bounded deltas never can.
    if (dx < -radius || dx > radius || dy < -radius || dy > radius ||
        dz < -radius || dz > radius ||
        dx * dx + dy * dy + dz * dz > radius * radius) {
        return fail(error, "player is too far from the anvil to interact");
    }
    return true;
}

bool isAnvilIdentifier(const std::string& identifier) {
    return identifier == "minecraft:anvil" || identifier == "anvil" ||
        identifier == "minecraft:chipped_anvil" ||
        identifier == "chipped_anvil" ||
        identifier == "minecraft:damaged_anvil" ||
        identifier == "damaged_anvil";
}

#if defined(__ANDROID__)
// The same v859 LocalPlayer getter and 32-byte fingerprint already used by
// ContainerOpenPacketSender when building its ItemUse position. Keep this
// local check independent of NativeWorldAccess's Python/camera fallback:
// third-person camera position is not proof of player interaction range.
constexpr uintptr_t kGetPlayerPositionRva = 0x0D8DFF5CULL;
constexpr std::array<uint8_t, 32U> kGetPlayerPositionFingerprint{{
    0x00U, 0x44U, 0x41U, 0xF9U, 0xC0U, 0x03U, 0x5FU, 0xD6U,
    0x08U, 0x4CU, 0x41U, 0xF9U, 0x20U, 0x00U, 0x40U, 0xFDU,
    0x00U, 0x05U, 0x00U, 0xFDU, 0xC0U, 0x03U, 0x5FU, 0xD6U,
    0x00U, 0x4CU, 0x41U, 0xF9U, 0xC0U, 0x03U, 0x5FU, 0xD6U,
}};

bool readNativePlayerBlockPosition(int32_t* x, int32_t* y, int32_t* z,
                                   std::string* error) {
    if (!x || !y || !z) return fail(error, "player position outputs are unavailable");
    const uintptr_t base = Main::getBaseAddress();
    uintptr_t getter_address = 0;
    if (!base || !ResolveMinecraftExecutableOffset(
            base, kGetPlayerPositionRva,
            kGetPlayerPositionFingerprint.size(), &getter_address) ||
        std::memcmp(reinterpret_cast<const void*>(getter_address),
                    kGetPlayerPositionFingerprint.data(),
                    kGetPlayerPositionFingerprint.size()) != 0) {
        return fail(error, "native player-position ABI fingerprint changed");
    }
    void* player = GetLocalPlayerPointer();
    if (!player || !IsMemoryReadable(player, sizeof(void*))) {
        return fail(error, "native local player is unavailable");
    }
    using GetPosition = const Vec3* (*)(void*);
    const Vec3* position = reinterpret_cast<GetPosition>(getter_address)(player);
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
        return fail(error, "native local-player position is outside block coordinates");
    }
    *x = static_cast<int32_t>(px);
    *y = static_cast<int32_t>(py);
    *z = static_cast<int32_t>(pz);
    return true;
}

bool readNativeObservation(
        void*, const MapVisibleAnvilWindowRequest& request,
        bool include_block, MapVisibleAnvilPreflightObservation* output,
        std::string* error) {
    if (!output) return fail(error, "anvil preflight observation output is unavailable");
    *output = {};
    if (!IsMinecraftUpdateGameThread()) {
        return fail(error, "anvil preflight requires an authorized game tick");
    }
    output->authorized_game_tick = true;
    if (!QueryWorldContextOnGameThread(&output->world_context, 1000)) {
        return fail(error, "live anvil world context could not be read");
    }
    if (!include_block) {
        if (error) error->clear();
        return true;
    }

    if (!readNativePlayerBlockPosition(&output->player_x, &output->player_y,
                                       &output->player_z, error)) {
        return false;
    }
    output->player_position_available = true;

    NativeWorldReader reader;
    NativeBlockView block;
    if (!reader.open() ||
        !reader.getBlockView(request.x, request.y, request.z, &block) ||
        !block.name || !block.type_token) {
        return fail(error, "native anvil Block cannot be read at the target");
    }
    output->block_available = true;
    output->block_identifier = *block.name;
    output->native_block = block.type_token;
    if (error) error->clear();
    return true;
}
#endif

}  // namespace

bool VerifyMapVisibleAnvilProductionContext(
        void* context, const MapVisibleAnvilWindowRequest& request,
        std::string* error) {
    if (error) error->clear();
    const auto* strict = static_cast<MapVisibleAnvilStrictPreflightContext*>(context);
    if (!strict || !strict->read_observation ||
        !requestMatches(strict->expected, request, error)) {
        return false;
    }
    MapVisibleAnvilPreflightObservation observed;
    std::string read_error;
    if (!strict->read_observation(strict->read_context, request, false,
                                  &observed, &read_error)) {
        if (error) *error = read_error.empty()
            ? "visible anvil world read failed" : read_error;
        return false;
    }
    return sameAuthorizedWorld(strict->expected, observed, error);
}

bool PrepareMapVisibleAnvilProductionOpen(
        void* context, const MapVisibleAnvilWindowRequest& request,
        const void** native_block, std::string* error) {
    if (native_block) *native_block = nullptr;
    if (error) error->clear();
    const auto* strict = static_cast<MapVisibleAnvilStrictPreflightContext*>(context);
    if (!native_block || !strict || !strict->read_observation ||
        !requestMatches(strict->expected, request, error)) {
        return false;
    }
    MapVisibleAnvilPreflightObservation observed;
    std::string read_error;
    if (!strict->read_observation(strict->read_context, request, true,
                                  &observed, &read_error)) {
        if (error) *error = read_error.empty()
            ? "visible anvil block read failed" : read_error;
        return false;
    }
    if (!sameAuthorizedWorld(strict->expected, observed, error) ||
        !nearAnvil(strict->expected, observed, error)) {
        return false;
    }
    if (!observed.block_available || !observed.native_block ||
        !isAnvilIdentifier(observed.block_identifier)) {
        return fail(error, "target is not a readable native anvil Block");
    }
    *native_block = observed.native_block;
    if (error) error->clear();
    return true;
}

#if defined(__ANDROID__)
MapVisibleAnvilNativePreflight MakeNativeMapVisibleAnvilProductionPreflight(
        MapVisibleAnvilStrictPreflightContext* context) noexcept {
    if (!context) return {};
    context->read_context = nullptr;
    context->read_observation = &readNativeObservation;
    return {context, &VerifyMapVisibleAnvilProductionContext,
            &PrepareMapVisibleAnvilProductionOpen};
}
#endif

}  // namespace build_import
