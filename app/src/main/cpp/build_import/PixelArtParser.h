#ifndef INFINITE_TEXTURE_PIXEL_ART_PARSER_H
#define INFINITE_TEXTURE_PIXEL_ART_PARSER_H

#include "SchematicParser.h"

#include <cstddef>
#include <functional>

namespace build_import {

struct PixelArtParseOptions {
    std::string source_path;
    std::string spool_directory;
    int32_t base_x = 0;
    int32_t base_y = 0;
    int32_t base_z = 0;
    int32_t target_width = 64;
    // Align the wall to scale-zero map boundaries before placing pixels.
    bool create_maps_after_import = false;
    int32_t chunk_size = 32;
    // Keep descriptor metadata bounded even when an unlimited-width image is
    // mostly transparent. Zero explicitly opts out for disk-backed consumers.
    size_t maximum_chunk_descriptors = 65536;
    // Non-interlaced images use the faster single-pass scaler whenever its
    // row working set fits this budget. Zero forces the disk-backed fallback.
    size_t streaming_memory_budget_bytes = 64U * 1024U * 1024U;
    // Zero is unlimited. Bounded callers can reject an oversized scaled wall
    // as soon as its dimensions are known, before any block spool writes.
    uint64_t maximum_output_blocks = 0;
    // See SchematicParseOptions::include_source_volume. Pixel imports use the
    // wall bounds when overwrite clearing is enabled.
    bool include_source_volume = true;
    // See SchematicParseOptions::block_sink. This streams parsed pixels to a
    // caller without retaining the complete chunk plan in memory.
    ParsedBlockSink block_sink;
    // Return true to stop parsing at the next cooperative cancellation point.
    std::function<bool()> cancellation_requested;
    // Uses the shared parser stages so the runtime can expose one planning
    // progress channel for schematic and pixel-art sources.
    std::function<void(const SchematicParseProgress&)> progress_callback;
};

// Converts a PNG directly to chunk/phase spools. No JSON or whole blueprint
// representation is emitted. Ordinary images are decoded row-by-row; Adam7
// and ultra-wide output use reusable disk-backed rows and fixed-size stripes,
// so target width does not determine parser memory use.
class PixelArtParser {
public:
    static constexpr const char* kVersion = "pixel-art-v3";

    bool parse(const PixelArtParseOptions& options, SchematicParseResult* result,
               std::string* error = nullptr) const;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_PIXEL_ART_PARSER_H
