#ifndef INFINITE_TEXTURE_BDX_WRITER_H
#define INFINITE_TEXTURE_BDX_WRITER_H

#include "CommandBlockSpool.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace build_import {

// A BDX palette entry keeps both the Bedrock identifier and the optional
// block-state string.  Legacy opcode 7 is used when no state string is
// available; opcode 13 carries the state inline for modern blocks.
struct BdxPaletteEntry {
    std::string identifier;
    std::string state;
    uint16_t data = 0;
};

struct BdxWriteRequest {
    std::string output_path;
    std::string display_name;
    int32_t width = 0;
    int32_t height = 0;
    int32_t length = 0;
    std::vector<BdxPaletteEntry> palette;
    // The index order matches SchematicWriter: x is fastest, then z, then y.
    std::vector<uint16_t> block_indices;
    // Coordinates are local to the exported volume (0 <= x < width, etc.).
    std::vector<CommandBlockRecord> command_blocks;
    std::function<bool()> cancellation_requested;
    std::shared_ptr<std::mutex> publish_mutex;
    std::atomic<bool>* publication_committed = nullptr;
};

class BdxWriter {
public:
    static bool write(const BdxWriteRequest& request, std::string* error = nullptr);
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BDX_WRITER_H
