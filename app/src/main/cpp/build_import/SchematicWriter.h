#ifndef INFINITE_TEXTURE_SCHEMATIC_WRITER_H
#define INFINITE_TEXTURE_SCHEMATIC_WRITER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "SchematicRawBlock.h"

namespace build_import {

struct SchematicWriteRequest {
    std::string output_path;
    std::string display_name;
    int32_t width = 0;
    int32_t height = 0;
    int32_t length = 0;
    std::vector<std::string> palette;
    // Sponge palette IDs are limited to 0..65,535. Keeping them compact lets
    // a 16M-block export use the same 32 MiB index budget as the old 8M limit.
    std::vector<uint16_t> block_indices;
    // Optional lossless Bedrock snapshots.  Sponge readers can ignore the
    // Infinitecz extension while the native importer can use the original
    // identifier/aux without trying to reverse a Java state mapping.
    std::vector<SchematicRawBlock> raw_blocks;
    std::function<bool()> cancellation_requested;
    // Optional lock shared with the runtime's cancel path. It is held across
    // the final cancellation check and rename so cancellation linearizes with
    // publication of the destination file.
    std::shared_ptr<std::mutex> publish_mutex;
    // Set while publish_mutex is held immediately after the destination rename.
    // The pointed-to atomic must outlive write().
    std::atomic<bool>* publication_committed = nullptr;
};

class SchematicWriter {
public:
    static constexpr int32_t kDataVersion = 3953;
    static constexpr size_t kMaximumPaletteSize = 65'536;

    // Creates a gzip-compressed Sponge v2 schematic through a same-directory
    // .part file. The destination is replaced only after a complete close.
    static bool write(const SchematicWriteRequest& request, std::string* error = nullptr);

    // Converts the target Bedrock name/aux pair to a standard Java/Sponge
    // palette state. Target-only aliases and aux bits are expanded into normal
    // state properties so exported files remain portable and re-importable.
    static std::string blockStateForExport(std::string raw_name, uint16_t aux,
                                           int32_t legacy_id = 0);
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_SCHEMATIC_WRITER_H
