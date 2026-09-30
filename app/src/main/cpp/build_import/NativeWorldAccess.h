#ifndef INFINITE_TEXTURE_NATIVE_WORLD_ACCESS_H
#define INFINITE_TEXTURE_NATIVE_WORLD_ACCESS_H

#include "BuildImportTypes.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace build_import {

struct NativeBlockInfo {
    std::string name;
    uint16_t aux = 0;
    int32_t legacy_id = 0;
};

// Optional JSON snapshots obtained through the client ModSDK.  The current
// client SDK exposes legacy name/aux and block-entity data, but not the
// modern state map; state_json is therefore empty on builds without that
// server-side/native API.  Keeping the field here lets newer ABI profiles
// provide it without changing the export pipeline again.
struct NativeBlockSnapshot {
    bool client_block_available = false;
    std::string client_identifier;
    bool client_aux_available = false;
    uint16_t client_aux = 0;
    bool state_available = false;
    std::string state_json;
    bool entity_available = false;
    std::string entity_json;
};

// Non-owning block data for hot bulk scans. The name remains valid until the
// reader source changes or reset() is called.
struct NativeBlockView {
    const std::string* name = nullptr;
    const void* type_token = nullptr;
    uint16_t aux = 0;
    int32_t legacy_id = 0;
};

// Optional command-block entity data read through the client ModSDK.  The
// native block reader still remains the source of truth for shell identity;
// this payload only fills the editable fields that are not present in Block.
struct NativeCommandBlockData {
    bool available = false;
    bool shell_aux_available = false;
    uint16_t shell_aux = 0;
    uint16_t mode = 0;
    bool redstone_mode = true;
    bool conditional = false;
    std::string command;
    std::string last_output;
    std::string name;
    bool output_tracked = false;
    int32_t tick_delay = 0;
    bool executing_on_first_tick = false;
};

// A reusable reader for bulk scans on the local-player game thread. open()
// refreshes engine pointers but keeps immutable BlockLegacy metadata cached
// while the BlockSource remains unchanged.
class NativeWorldReader {
public:
    bool open();
    void reset();
    bool getBlock(int32_t x, int32_t y, int32_t z, NativeBlockInfo* output);
    bool getBlockView(int32_t x, int32_t y, int32_t z, NativeBlockView* output);
    uint64_t sourceGeneration() const { return source_generation_; }

private:
    // The current BlockSource virtual API takes a BlockPos reference rather
    // than three integer arguments.  Keep this POD layout explicit: passing
    // x/y/z as registers makes the engine interpret x as an address and
    // faults as soon as a teleported export starts scanning.
    struct NativeBlockPos {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
    };

    struct LegacyInfo {
        std::string name;
        int32_t id = 0;
    };

    using GetBlockFunction = void* (*)(void*, const NativeBlockPos*);
    void* region_ = nullptr;
    GetBlockFunction get_block_ = nullptr;
    std::unordered_map<void*, LegacyInfo> legacy_cache_;
    // Block instances are interned per block state, so their layout only has
    // to be probed once each. Probing is two pipe syscalls, which dominated
    // the scan cost when it ran for every block of every batch.
    std::unordered_set<const void*> validated_blocks_;
    uint64_t source_generation_ = 0;
};

class NativeWorldAccess {
public:
    // These methods must run on the local-player game thread.
    static bool getBlock(int32_t x, int32_t y, int32_t z, NativeBlockInfo* output);
    // Reads optional complete state/entity snapshots.  A true return means
    // the query itself completed; individual payloads may be unavailable on
    // client-only SDK builds.
    static bool getBlockSnapshot(int32_t x, int32_t y, int32_t z,
                                 NativeBlockSnapshot* output,
                                 bool include_container_items = false,
                                 int32_t dimension_id = 0);
    // Reads command-block shell aux plus editor data from the client ModSDK.
    // A true return can still have available == false when GetBlock() worked
    // but the block entity payload was unavailable; false means the complete
    // ModSDK query failed and the exporter should use its native fallback.
    static bool getCommandBlockData(int32_t x, int32_t y, int32_t z,
                                    NativeCommandBlockData* output);
    static bool isRegionReadable(const BlockBounds& bounds);
    static bool isChunkReadable(const ChunkCoord& chunk, int32_t chunk_size, int32_t sample_y);
    static uintptr_t dimensionToken();
    // Describes the most recent native world-reader failure.  This is intended
    // for the export UI/status and contains no engine-owned pointers.
    static const char* lastWorldReaderDiagnostic();
    // This reads engine-owned actor memory and therefore must run on the same
    // game thread as getBlock().
    static bool getLocalPlayerBlockPosition(int32_t* x, int32_t* y, int32_t* z);
    // A server-side /tp invalidates the short Python position cache. Call this
    // immediately after issuing a teleport so a subsequent arrival check does
    // not keep testing the location from before the move.
    static void invalidateLocalPlayerBlockPositionCache();
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_NATIVE_WORLD_ACCESS_H
