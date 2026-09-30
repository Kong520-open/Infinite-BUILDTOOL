#include "../MapVisibleAnvilProductionPreflight.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <string>

namespace {

using namespace build_import;

struct FakeReader {
    bool success = true;
    int world_reads = 0;
    int block_reads = 0;
    MapVisibleAnvilPreflightObservation observed;

    static bool read(void* context, const MapVisibleAnvilWindowRequest&,
                     bool include_block,
                     MapVisibleAnvilPreflightObservation* output,
                     std::string* error) {
        auto& fake = *static_cast<FakeReader*>(context);
        if (include_block) ++fake.block_reads;
        else ++fake.world_reads;
        if (!fake.success) {
            if (error) *error = "reader failed";
            return false;
        }
        if (output) *output = fake.observed;
        return true;
    }
};

MapVisibleAnvilWindowRequest request() {
    MapVisibleAnvilWindowRequest value;
    value.token = 73U;
    value.x = 12;
    value.y = 30;
    value.z = -9;
    value.face = 1;
    return value;
}

MapVisibleAnvilStrictPreflightContext context(FakeReader* fake) {
    MapVisibleAnvilStrictPreflightContext value;
    value.expected.token = 73U;
    value.expected.world_context = "stable:v1:test|0";
    value.expected.dimension_token = 0x1234U;
    value.expected.x = 12;
    value.expected.y = 30;
    value.expected.z = -9;
    value.expected.face = 1;
    value.expected.max_distance_blocks = 4U;
    value.read_context = fake;
    value.read_observation = &FakeReader::read;
    return value;
}

FakeReader reader() {
    FakeReader value;
    value.observed.authorized_game_tick = true;
    value.observed.world_context = "stable:v1:test|0";
    value.observed.dimension_token = 0x1234U;
    value.observed.player_position_available = true;
    value.observed.player_x = 13;
    value.observed.player_y = 31;
    value.observed.player_z = -8;
    value.observed.block_available = true;
    value.observed.block_identifier = "minecraft:anvil";
    value.observed.native_block = reinterpret_cast<const void*>(0x1234);
    return value;
}

void validSameTickBlockPointerAndWorld() {
    FakeReader fake = reader();
    auto strict = context(&fake);
    std::string error;
    const void* block = nullptr;
    assert(VerifyMapVisibleAnvilProductionContext(&strict, request(), &error));
    assert(error.empty() && fake.world_reads == 1 && fake.block_reads == 0);
    assert(PrepareMapVisibleAnvilProductionOpen(
        &strict, request(), &block, &error));
    assert(block == reinterpret_cast<const void*>(0x1234));
    assert(error.empty() && fake.block_reads == 1);
    fake.observed.block_identifier = "minecraft:chipped_anvil";
    assert(PrepareMapVisibleAnvilProductionOpen(
        &strict, request(), &block, &error));
    fake.observed.block_identifier = "minecraft:damaged_anvil";
    assert(PrepareMapVisibleAnvilProductionOpen(
        &strict, request(), &block, &error));
}

void mismatchCannotYieldBlockPointer() {
    FakeReader fake = reader();
    auto strict = context(&fake);
    std::string error;
    const void* block = reinterpret_cast<const void*>(0x9999);
    fake.observed.world_context = "stable:v1:other|0";
    assert(!VerifyMapVisibleAnvilProductionContext(&strict, request(), &error));
    assert(!PrepareMapVisibleAnvilProductionOpen(
        &strict, request(), &block, &error) && block == nullptr);
    fake.observed.world_context = strict.expected.world_context;
    // A transient native dimension pointer is not the world identity. Both a
    // changed pointer and an unavailable pointer must leave the stable world
    // and anvil preflight usable.
    fake.observed.dimension_token = 0x5678U;
    assert(VerifyMapVisibleAnvilProductionContext(&strict, request(), &error));
    assert(PrepareMapVisibleAnvilProductionOpen(
        &strict, request(), &block, &error));
    fake.observed.dimension_token = 0U;
    strict.expected.dimension_token = 0U;
    assert(VerifyMapVisibleAnvilProductionContext(&strict, request(), &error));
    assert(PrepareMapVisibleAnvilProductionOpen(
        &strict, request(), &block, &error));
    fake.observed.authorized_game_tick = false;
    assert(!VerifyMapVisibleAnvilProductionContext(&strict, request(), &error));
    fake.observed.authorized_game_tick = true;
    fake.success = false;
    assert(!PrepareMapVisibleAnvilProductionOpen(
        &strict, request(), &block, &error) && block == nullptr &&
           error == "reader failed");
}

void distanceIsStrictAndDoesNotBlockClose() {
    FakeReader fake = reader();
    auto strict = context(&fake);
    std::string error;
    const void* block = nullptr;
    fake.observed.player_x = 17;
    fake.observed.player_y = 30;
    fake.observed.player_z = -9;
    assert(!PrepareMapVisibleAnvilProductionOpen(
        &strict, request(), &block, &error));
    assert(block == nullptr);
    // Once the window is open, moving away is not a reason to refuse the
    // current world's numeric Close. verify_context checks world only.
    assert(VerifyMapVisibleAnvilProductionContext(&strict, request(), &error));
    fake.observed.player_position_available = false;
    assert(!PrepareMapVisibleAnvilProductionOpen(
        &strict, request(), &block, &error));
    assert(VerifyMapVisibleAnvilProductionContext(&strict, request(), &error));

    fake.observed.player_position_available = true;
    fake.observed.player_x = std::numeric_limits<int32_t>::max();
    fake.observed.player_y = std::numeric_limits<int32_t>::min();
    fake.observed.player_z = std::numeric_limits<int32_t>::max();
    assert(!PrepareMapVisibleAnvilProductionOpen(
        &strict, request(), &block, &error));
    assert(block == nullptr);
}

void requiresRealAnvilAndExactTaskIdentity() {
    FakeReader fake = reader();
    auto strict = context(&fake);
    std::string error;
    const void* block = nullptr;
    fake.observed.block_identifier = "minecraft:chest";
    assert(!PrepareMapVisibleAnvilProductionOpen(
        &strict, request(), &block, &error));
    fake.observed.block_identifier = "minecraft:anvil";
    fake.observed.native_block = nullptr;
    assert(!PrepareMapVisibleAnvilProductionOpen(
        &strict, request(), &block, &error));
    fake.observed.native_block = reinterpret_cast<const void*>(0x1234);
    auto wrong = request();
    ++wrong.x;
    assert(!VerifyMapVisibleAnvilProductionContext(&strict, wrong, &error));
    assert(!PrepareMapVisibleAnvilProductionOpen(
        &strict, wrong, &block, &error) && block == nullptr);
    wrong = request();
    ++wrong.token;
    assert(!VerifyMapVisibleAnvilProductionContext(&strict, wrong, &error));
    wrong = request();
    wrong.face = 2;
    assert(!VerifyMapVisibleAnvilProductionContext(&strict, wrong, &error));
    strict.expected.max_distance_blocks = 5U;
    assert(!VerifyMapVisibleAnvilProductionContext(&strict, request(), &error));
    strict.expected.max_distance_blocks = 4U;
    strict.expected.dimension_token = 0U;
    assert(VerifyMapVisibleAnvilProductionContext(&strict, request(), &error));
}

}  // namespace

int main() {
    validSameTickBlockPointerAndWorld();
    mismatchCannotYieldBlockPointer();
    distanceIsStrictAndDoesNotBlockClose();
    requiresRealAnvilAndExactTaskIdentity();
}
