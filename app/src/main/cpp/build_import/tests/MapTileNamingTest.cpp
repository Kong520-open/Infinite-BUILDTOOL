#include "../MapTileNaming.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <string>

using build_import::FormatMapTileName;

int main() {
    std::string name = "unchanged";
    assert(FormatMapTileName(0, 1, 1, &name));
    assert(name == "地图 1行1列");
    assert(FormatMapTileName(5, 3, 2, &name));
    assert(name == "地图 2行3列");
    assert(FormatMapTileName(65535, 65536, 1, &name));
    assert(name == "地图 1行65536列");
    assert(name.size() <= 24U);
    assert(!FormatMapTileName(6, 3, 2, &name));
    assert(!FormatMapTileName(0, 0, 1, &name));
    assert(!FormatMapTileName(0, std::numeric_limits<uint64_t>::max(), 2, &name));
    assert(!FormatMapTileName(0, 1, 1, nullptr));
    assert(name == "地图 1行65536列");
    return 0;
}
