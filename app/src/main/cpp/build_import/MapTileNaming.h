#ifndef INFINITE_TEXTURE_MAP_TILE_NAMING_H
#define INFINITE_TEXTURE_MAP_TILE_NAMING_H

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace build_import {

// Row and column are one-based in the item name. The compact form stays below
// 24 UTF-8 bytes for every currently supported (<= 65536 tile) map plan.
inline bool FormatMapTileName(uint64_t tile_cursor, uint64_t columns,
                              uint64_t rows, std::string* name) {
    if (!name || columns == 0 || rows == 0 ||
        columns > 65536U || rows > 65536U ||
        columns > std::numeric_limits<uint64_t>::max() / rows ||
        columns * rows > 65536U || tile_cursor >= columns * rows) {
        return false;
    }
    const uint64_t row = tile_cursor / columns + 1U;
    const uint64_t column = tile_cursor % columns + 1U;
    std::string formatted = "地图 " + std::to_string(row) + "行" +
                            std::to_string(column) + "列";
    if (formatted.size() > 24U) return false;
    *name = std::move(formatted);
    return true;
}

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_TILE_NAMING_H
