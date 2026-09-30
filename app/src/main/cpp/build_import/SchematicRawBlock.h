#ifndef INFINITE_TEXTURE_SCHEMATIC_RAW_BLOCK_H
#define INFINITE_TEXTURE_SCHEMATIC_RAW_BLOCK_H

#include <cstdint>
#include <string>

namespace build_import {

// A lossless snapshot of the target Bedrock block representation.  The
// ordinary Sponge palette remains the portable view; these records retain the
// source identity for blocks whose legacy aux/state cannot be represented by
// a Java palette state.  state_json/entity_json are UTF-8 JSON snapshots
// produced by the in-game API and are intentionally opaque to the legacy
// importer until a target-side write protocol is available.
struct SchematicRawBlock {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    std::string identifier;
    uint16_t aux = 0;
    int32_t legacy_id = 0;
    std::string state_json;
    std::string entity_json;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_SCHEMATIC_RAW_BLOCK_H
