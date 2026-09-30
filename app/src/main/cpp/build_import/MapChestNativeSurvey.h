#ifndef INFINITE_TEXTURE_MAP_CHEST_NATIVE_SURVEY_H
#define INFINITE_TEXTURE_MAP_CHEST_NATIVE_SURVEY_H

#include "MapChestPlacement.h"
#include "NativeWorldAccess.h"

namespace build_import {

// Conservative name-only classification. NativeBlockInfo does not expose a
// reliable collision/solid-face flag, so only known full-cube families count
// as support. Unrecognized identifiers remain Unknown, never "probably air".
MapChestCell ClassifyMapChestNativeBlock(const NativeBlockInfo& block);

// Bedrock can return this synthetic block while a newly teleported-to chunk
// is still loading. It is not evidence of air or of a real obstruction.
bool IsMapStorageNativeReadPlaceholder(const NativeBlockInfo& block);

// A dedicated platform's stone floor should exist after its first successful
// fill. If a later native survey reports air at floor, feet, and head, treat
// that snapshot as not yet confirmed (for example while the destination chunk
// catches up after teleport), not as evidence that the standing cell changed.
// A known floor or a solid obstruction must be handled by the caller's normal
// validation path.
bool ShouldRetryMapStorageStandingSurvey(MapChestCell floor,
                                         MapChestCell feet,
                                         MapChestCell head);

// Read-only game-thread adapters. The reader overload allows a caller to open
// NativeWorldReader once and survey an ordered candidate sequence cheaply.
// Every failed native read leaves its cell Unknown. These functions never send
// a packet, issue a command, or place a block.
MapChestCandidateSurvey SurveyMapChestCandidateFromNativeWorld(
    const MapChestPosition& position);
MapChestCandidateSurvey SurveyMapChestCandidateWithReader(
    NativeWorldReader* reader, const MapChestPosition& position);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_CHEST_NATIVE_SURVEY_H
