#ifndef INFINITE_TEXTURE_RUNTIME_ID_POOL_117_H
#define INFINITE_TEXTURE_RUNTIME_ID_POOL_117_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace build_import {

// BDX runtime IDs are tied to the exporter build, not to the target game's
// current runtime palette.  This table is the fixed NetEase 1.17 / 2.0.5 pool
// selected by the BDX UseRuntimeIDPool opcode with value 117.
struct RuntimeIdPool117Entry {
    uint16_t name_index = 0;
    uint16_t aux = 0;
};

namespace runtime_id_pool_117_detail {

// Generated mechanically from Happy2018new/bdump-docs:
// resources/blockRuntimeIDs/netease/runtimeIds_117.json.
#include "RuntimeIdPool117.inc"

}  // namespace runtime_id_pool_117_detail

inline bool lookupRuntimeIdPool117(uint32_t runtime_id, std::string_view* identifier,
                                   uint16_t* aux) {
    if (!identifier || !aux ||
        runtime_id >= runtime_id_pool_117_detail::kRuntimeIdPool117Entries.size()) {
        return false;
    }
    const RuntimeIdPool117Entry entry =
        runtime_id_pool_117_detail::kRuntimeIdPool117Entries[runtime_id];
    if (entry.name_index >= runtime_id_pool_117_detail::kRuntimeIdPool117Names.size()) {
        return false;
    }
    *identifier = runtime_id_pool_117_detail::kRuntimeIdPool117Names[entry.name_index];
    *aux = entry.aux;
    return true;
}

}  // namespace build_import

#endif  // INFINITE_TEXTURE_RUNTIME_ID_POOL_117_H
