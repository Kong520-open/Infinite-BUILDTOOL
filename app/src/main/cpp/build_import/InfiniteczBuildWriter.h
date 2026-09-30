#ifndef INFINITE_TEXTURE_INFINITEcz_BUILD_WRITER_H
#define INFINITE_TEXTURE_INFINITEcz_BUILD_WRITER_H

#include "CommandBlockSpool.h"
#include "SchematicRawBlock.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace build_import {

// The canonical Infinitecz building format. Unlike Sponge/BDX, this request
// keeps the native Bedrock identity and auxiliary value alongside the
// portable palette view. All coordinates in raw_blocks/command_blocks are
// local to the exported volume.
struct InfiniteczBuildWriteRequest {
    std::string output_path;
    std::string display_name;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    int32_t origin_z = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t length = 0;
    std::vector<std::string> palette;
    std::vector<uint16_t> block_indices;
    // Optional native snapshots. A record is coordinate-unique and authoritative
    // even when the portable palette entry at that coordinate is air because no
    // Java/Sponge representation exists. state_json remains an opaque SDK JSON
    // snapshot; the v1 file stores it byte-for-byte alongside the full u16 aux.
    std::vector<SchematicRawBlock> raw_blocks;
    std::vector<CommandBlockRecord> command_blocks;
    std::function<bool()> cancellation_requested;
    std::shared_ptr<std::mutex> publish_mutex;
    std::atomic<bool>* publication_committed = nullptr;
};

class InfiniteczBuildWriter {
public:
    static constexpr const char* kExtension = ".infinity";
    static constexpr uint32_t kFormatVersion = 1;

    static bool write(const InfiniteczBuildWriteRequest& request,
                      std::string* error = nullptr);
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_INFINITEcz_BUILD_WRITER_H
