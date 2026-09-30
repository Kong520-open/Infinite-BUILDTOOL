#include "MapBlankSupplyJournal.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace build_import;

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("map_blank_supply_test_" + std::to_string(stamp));
    assert(std::filesystem::create_directory(directory));
    const std::string state_path = (directory / "map_creation.state").string();

    MapBlankSupplyIntent record;
    record.world_id = "stable:v1:test|0";
    record.dimension_id = 0;
    record.tile_cursor = 2;
    record.tile_count = 8;
    record.rpc_uuid = "map-give-test-1";

    MapBlankSupplyIntent loaded;
    std::string error;
    assert(LoadMapBlankSupply(state_path, &loaded, &error) ==
           MapBlankSupplyLoad::Missing);
    assert(ArmMapBlankSupply(state_path, record, &error));
    assert(!ArmMapBlankSupply(state_path, record, &error));
    assert(LoadMapBlankSupply(state_path, &loaded, &error) ==
           MapBlankSupplyLoad::Loaded);
    assert(MapBlankSupplyMatches(loaded, record.world_id, 0, 2, 8));
    assert(!MapBlankSupplyMatches(loaded, record.world_id, 1, 2, 8));
    assert(!MapBlankSupplyMatches(loaded, record.world_id, 0, 3, 8));
    assert(!ClearMapBlankSupply(state_path,
            MapBlankSupplyIntent{record.world_id, 0, 2, 8, 0, "wrong-rpc"},
            &error));
    assert(ClearMapBlankSupply(state_path, record, &error));
    assert(LoadMapBlankSupply(state_path, &loaded, &error) ==
           MapBlankSupplyLoad::Missing);

    // A partial pre-rename file is not silently overwritten and cannot
    // authorize another give after an interrupted attempt.
    const std::string temporary = MapBlankSupplyJournalPath(state_path) + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary);
        out << "partial";
    }
    assert(LoadMapBlankSupply(state_path, &loaded, &error) ==
           MapBlankSupplyLoad::Unsafe);
    assert(!ArmMapBlankSupply(state_path, record, &error));
    assert(std::filesystem::remove(temporary));

    assert(ArmMapBlankSupply(state_path, record, &error));
    {
        std::fstream file(MapBlankSupplyJournalPath(state_path),
                          std::ios::in | std::ios::out | std::ios::binary);
        assert(file);
        file.seekp(24);
        file.put('!');
    }
    assert(LoadMapBlankSupply(state_path, &loaded, &error) ==
           MapBlankSupplyLoad::Unsafe);

    assert(std::filesystem::remove(MapBlankSupplyJournalPath(state_path)));
    assert(std::filesystem::remove(directory));
}
