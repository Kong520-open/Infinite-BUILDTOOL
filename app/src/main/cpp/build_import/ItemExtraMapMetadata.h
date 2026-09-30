#ifndef INFINITE_TEXTURE_ITEM_EXTRA_MAP_METADATA_H
#define INFINITE_TEXTURE_ITEM_EXTRA_MAP_METADATA_H

#include <cstdint>
#include <string>
#include <string_view>

namespace build_import {

enum class MapItemNameStatus : uint8_t {
    Missing,
    Present,
    // A candidate field existed, but was duplicate, ambiguous, invalid UTF-8,
    // or too long. Never use it as evidence that an item was renamed.
    Unusable,
};

enum class MapItemNameSource : uint8_t {
    None,
    RootCustomName,
    DisplayName,
};

struct ItemExtraMapMetadata {
    bool has_map_uuid = false;
    int64_t map_uuid = -1;
    MapItemNameStatus name_status = MapItemNameStatus::Missing;
    MapItemNameSource name_source = MapItemNameSource::None;
    std::string display_name;
};

// Read-only parser for the complete Bedrock ItemExtraData field: 0xffff,
// little-endian NBT codec 1, root TAG_Compound, then two short-string lists.
// Only root map_uuid and two *candidate* custom-name locations are retained.
// Neither name location has yet been confirmed for the target NetEase client.
// A false result means malformed or unsupported data; output is reset.
// An unusable name does not invalidate an otherwise valid root map_uuid.
bool ParseItemExtraMapMetadata(std::string_view item_extra,
                               ItemExtraMapMetadata* output);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_ITEM_EXTRA_MAP_METADATA_H
