#include "../MapChestProductionPreflight.h"

#include <cassert>
#include <string>

using namespace build_import;

namespace {

struct FakeReader {
    MapChestPreflightObservation observation;
    bool readable = true;
};

bool readFake(void* context, const MapChestWindowRequest&, bool include_block,
              MapChestPreflightObservation* output, std::string*) {
    auto* fake = static_cast<FakeReader*>(context);
    if (!fake || !fake->readable || !output) return false;
    *output = fake->observation;
    if (!include_block) {
        output->block_available = false;
        output->native_block = nullptr;
    }
    return true;
}

}  // namespace

int main() {
    int block = 7;
    MapChestWindowRequest request{123U, 20, 64, 30, 1};
    FakeReader reader;
    reader.observation.authorized_game_tick = true;
    reader.observation.world_context = "world-one";
    reader.observation.dimension_token = 0x1100U;
    reader.observation.player_position_available = true;
    reader.observation.player_x = 21;
    reader.observation.player_y = 64;
    reader.observation.player_z = 30;
    reader.observation.block_available = true;
    reader.observation.block_identifier = "minecraft:chest";
    reader.observation.isolated_single_chest = true;
    reader.observation.native_block = &block;
    MapChestStrictPreflightContext strict;
    strict.expected = {123U, "world-one", 0x1100U, 20, 64, 30, 1, 4};
    strict.read_context = &reader;
    strict.read_observation = &readFake;

    std::string error;
    const void* native_block = nullptr;
    assert(VerifyMapChestProductionContext(&strict, request, &error));
    assert(PrepareMapChestProductionOpen(&strict, request, &native_block,
                                         &error));
    assert(native_block == &block);

    reader.observation.player_x = 25;
    assert(!PrepareMapChestProductionOpen(&strict, request, &native_block,
                                          &error));
    assert(native_block == nullptr);
    // Closing an already-open window does not demand reach, only the same
    // authorized world/session.
    assert(VerifyMapChestProductionContext(&strict, request, &error));
    reader.observation.player_x = 21;

    reader.observation.isolated_single_chest = false;
    assert(!PrepareMapChestProductionOpen(&strict, request, &native_block,
                                          &error));
    reader.observation.isolated_single_chest = true;
    reader.observation.block_identifier = "minecraft:trapped_chest";
    assert(!PrepareMapChestProductionOpen(&strict, request, &native_block,
                                          &error));
    reader.observation.block_identifier = "minecraft:chest";

    // A missing or changed native dimension pointer must not block a valid
    // world/session: this value can be temporarily unavailable in-game.
    reader.observation.dimension_token = 0U;
    assert(VerifyMapChestProductionContext(&strict, request, &error));
    assert(PrepareMapChestProductionOpen(&strict, request, &native_block,
                                         &error));
    reader.observation.dimension_token = 0x2200U;
    assert(VerifyMapChestProductionContext(&strict, request, &error));
    assert(PrepareMapChestProductionOpen(&strict, request, &native_block,
                                         &error));
    strict.expected.dimension_token = 0U;
    assert(VerifyMapChestProductionContext(&strict, request, &error));
    assert(PrepareMapChestProductionOpen(&strict, request, &native_block,
                                         &error));

    reader.observation.world_context = "world-two";
    assert(!VerifyMapChestProductionContext(&strict, request, &error));
    assert(!PrepareMapChestProductionOpen(&strict, request, &native_block,
                                          &error));
    reader.observation.world_context = "world-one";
    reader.observation.authorized_game_tick = false;
    assert(!VerifyMapChestProductionContext(&strict, request, &error));
    assert(!PrepareMapChestProductionOpen(&strict, request, &native_block,
                                          &error));
    reader.observation.authorized_game_tick = true;

    request.token = 124U;
    assert(!VerifyMapChestProductionContext(&strict, request, &error));
    request.token = 123U;
    reader.readable = false;
    assert(!PrepareMapChestProductionOpen(&strict, request, &native_block,
                                          &error));
    return 0;
}
