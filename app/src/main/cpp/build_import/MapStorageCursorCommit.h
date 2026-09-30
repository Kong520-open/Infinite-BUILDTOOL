#ifndef INFINITE_TEXTURE_MAP_STORAGE_CURSOR_COMMIT_H
#define INFINITE_TEXTURE_MAP_STORAGE_CURSOR_COMMIT_H

#include <cstdint>
#include <string>

namespace build_import {

// Advances the existing map_creation.state DDS1 cursor by exactly one after
// the corresponding chest journal has reached a committable transfer phase
// (legacy ReopenConfirmed/InventoryConfirmed or AcceptedAndClosed). The caller
// must verify that journal and the current world first. This file operation
// cannot itself prove a map is in the chest.
//
// A false result can mean the atomic replace happened but the final directory
// sync failed. Never infer the cursor from the return value alone; reload the
// state and its journals before deciding what to do next.
bool CommitVerifiedMapTileCursor(const std::string& state_path,
                                 uint64_t tile_count,
                                 uint64_t expected_cursor,
                                 std::string* error = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MAP_STORAGE_CURSOR_COMMIT_H
