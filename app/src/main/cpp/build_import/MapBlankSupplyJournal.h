#ifndef INFINITE_TEXTURE_MAP_BLANK_SUPPLY_JOURNAL_H
#define INFINITE_TEXTURE_MAP_BLANK_SUPPLY_JOURNAL_H

#include <cstdint>
#include <string>

namespace build_import {

// /give is not idempotent. Arm this intent durably before dispatch, and never
// send another /give for an unresolved intent after a pause or process crash.
struct MapBlankSupplyIntent {
    std::string world_id;
    int32_t dimension_id = 0;
    uint64_t tile_cursor = 0;
    uint64_t tile_count = 0;
    uint32_t empty_maps_before = 0;
    std::string rpc_uuid;
};

enum class MapBlankSupplyLoad : uint8_t { Missing, Loaded, Unsafe };

std::string MapBlankSupplyJournalPath(const std::string& map_state_path);
bool ArmMapBlankSupply(const std::string& map_state_path,
                       const MapBlankSupplyIntent& intent,
                       std::string* error = nullptr);
MapBlankSupplyLoad LoadMapBlankSupply(
    const std::string& map_state_path, MapBlankSupplyIntent* intent,
    std::string* error = nullptr);
bool MapBlankSupplyMatches(const MapBlankSupplyIntent& intent,
                           const std::string& world_id, int32_t dimension_id,
                           uint64_t tile_cursor, uint64_t tile_count) noexcept;
bool ClearMapBlankSupply(const std::string& map_state_path,
                         const MapBlankSupplyIntent& expected,
                         std::string* error = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_BLANK_SUPPLY_JOURNAL_H
