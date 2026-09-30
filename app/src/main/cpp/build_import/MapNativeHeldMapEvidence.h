#ifndef INFINITE_TEXTURE_MAP_NATIVE_HELD_MAP_EVIDENCE_H
#define INFINITE_TEXTURE_MAP_NATIVE_HELD_MAP_EVIDENCE_H

#include "MapStorageProductionActions.h"

#include <string>

namespace build_import {

// Read-only evidence for MapStorageProductionHooks::read_held_map. Call only
// from the authorized LocalPlayer game tick. The name comes exclusively from
// the game's live ItemStack serializer, never from UI text or packet caches.
// Returns false unless every identity, count, NBT, build and selected-slot
// check succeeds. The output is reset on failure.
bool ReadNativeHeldFilledMapEvidence(MapStorageHeldMapEvidence* output,
                                     std::string* error = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_NATIVE_HELD_MAP_EVIDENCE_H
