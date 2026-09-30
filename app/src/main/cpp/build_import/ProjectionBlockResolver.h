#ifndef INFINITE_TEXTURE_PROJECTION_BLOCK_RESOLVER_H
#define INFINITE_TEXTURE_PROJECTION_BLOCK_RESOLVER_H

#include "BlockMapper.h"

#include <cstdint>
#include <string_view>

namespace build_import {

// Resolves source blocks for visualization, not command execution. Unlike
// BlockMapper it accepts future/modded identifiers and retains the source
// material name so the projection can still assign a distinct appearance.
BlockMappingResult resolveProjectionLegacyBlock(uint16_t id, uint8_t data);
BlockMappingResult resolveProjectionPaletteState(std::string_view state);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_PROJECTION_BLOCK_RESOLVER_H
