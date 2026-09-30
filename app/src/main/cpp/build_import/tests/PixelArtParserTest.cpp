#include "../PixelArtParser.h"
#include "RawSpoolTestReader.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <zlib.h>

using namespace build_import;

namespace {

namespace fs = std::filesystem;

struct RawDiskRecord {
  int32_t x;
  int32_t y;
  int32_t z;
  uint8_t aux;
  uint8_t flags;
  uint16_t name_length;
};

static_assert(sizeof(RawDiskRecord) == 16,
              "unexpected raw spool record layout");

struct RawBlock {
  RawDiskRecord record{};
  std::string name;
};

class ScopedTempDirectory {
public:
  ScopedTempDirectory() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt != 100; ++attempt) {
      path_ = fs::temp_directory_path() /
              ("build_import_pixel_parser_test_" + std::to_string(nonce) + "_" +
               std::to_string(attempt));
      std::error_code error;
      if (fs::create_directory(path_, error))
        return;
    }
    throw std::runtime_error("cannot create pixel parser test directory");
  }

  ~ScopedTempDirectory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }

  fs::path child(const std::string &name) const { return path_ / name; }

private:
  fs::path path_;
};

void appendBe32(std::vector<uint8_t> *output, uint32_t value) {
  output->push_back(static_cast<uint8_t>(value >> 24));
  output->push_back(static_cast<uint8_t>(value >> 16));
  output->push_back(static_cast<uint8_t>(value >> 8));
  output->push_back(static_cast<uint8_t>(value));
}

void appendChunk(std::vector<uint8_t> *png, const std::array<uint8_t, 4> &type,
                 const uint8_t *data, size_t size) {
  assert(size <= UINT32_MAX);
  appendBe32(png, static_cast<uint32_t>(size));
  png->insert(png->end(), type.begin(), type.end());
  if (size != 0)
    png->insert(png->end(), data, data + size);

  uLong crc = crc32(0, Z_NULL, 0);
  crc = crc32(crc, type.data(), static_cast<uInt>(type.size()));
  if (size != 0)
    crc = crc32(crc, data, static_cast<uInt>(size));
  appendBe32(png, static_cast<uint32_t>(crc));
}

void appendChunk(std::vector<uint8_t> *png, const std::array<uint8_t, 4> &type,
                 const std::vector<uint8_t> &data) {
  appendChunk(png, type, data.data(), data.size());
}

uint32_t channelCount(uint8_t color_type) {
  switch (color_type) {
  case 0:
    return 1;
  case 2:
    return 3;
  case 3:
    return 1;
  case 4:
    return 2;
  case 6:
    return 4;
  default:
    return 0;
  }
}

void writePng(const fs::path &path, uint32_t width, uint32_t height,
              uint8_t color_type, const std::vector<uint8_t> &pixels,
              const std::vector<uint8_t> &transparency = {},
              size_t idat_piece_size = 0,
              const std::vector<uint8_t> &trailing_compressed_bytes = {}) {
  const uint32_t channels = channelCount(color_type);
  assert(width != 0 && height != 0 && channels != 0);
  assert(pixels.size() == static_cast<uint64_t>(width) * height * channels);

  std::vector<uint8_t> scanlines;
  scanlines.reserve(static_cast<size_t>(height) *
                    (static_cast<size_t>(width) * channels + 1));
  const size_t row_size = static_cast<size_t>(width) * channels;
  for (uint32_t row = 0; row < height; ++row) {
    scanlines.push_back(0); // PNG filter: None.
    const auto begin =
        pixels.begin() + static_cast<std::ptrdiff_t>(row * row_size);
    scanlines.insert(scanlines.end(), begin,
                     begin + static_cast<std::ptrdiff_t>(row_size));
  }

  uLongf compressed_size = compressBound(static_cast<uLong>(scanlines.size()));
  std::vector<uint8_t> compressed(compressed_size);
  assert(compress2(compressed.data(), &compressed_size, scanlines.data(),
                   static_cast<uLong>(scanlines.size()),
                   Z_BEST_COMPRESSION) == Z_OK);
  compressed.resize(compressed_size);
  compressed.insert(compressed.end(), trailing_compressed_bytes.begin(),
                    trailing_compressed_bytes.end());

  std::vector<uint8_t> png{137, 80, 78, 71, 13, 10, 26, 10};
  std::vector<uint8_t> header;
  appendBe32(&header, width);
  appendBe32(&header, height);
  header.insert(header.end(), {8, color_type, 0, 0, 0});
  appendChunk(&png, {'I', 'H', 'D', 'R'}, header);
  if (!transparency.empty())
    appendChunk(&png, {'t', 'R', 'N', 'S'}, transparency);

  if (idat_piece_size == 0) {
    appendChunk(&png, {'I', 'D', 'A', 'T'}, compressed);
  } else {
    for (size_t offset = 0; offset < compressed.size();
         offset += idat_piece_size) {
      const size_t part = std::min(idat_piece_size, compressed.size() - offset);
      appendChunk(&png, {'I', 'D', 'A', 'T'}, compressed.data() + offset, part);
    }
  }
  appendChunk(&png, {'I', 'E', 'N', 'D'}, nullptr, 0);

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  assert(output);
  output.write(reinterpret_cast<const char *>(png.data()),
               static_cast<std::streamsize>(png.size()));
  assert(output);
}

void writeRawPng(const fs::path &path, uint32_t width, uint32_t height,
                 uint8_t bit_depth, uint8_t color_type, uint8_t interlace,
                 const std::vector<uint8_t> &scanlines,
                 const std::vector<uint8_t> &palette = {},
                 const std::vector<uint8_t> &transparency = {}) {
  uLongf compressed_size = compressBound(static_cast<uLong>(scanlines.size()));
  std::vector<uint8_t> compressed(compressed_size);
  assert(compress2(compressed.data(), &compressed_size, scanlines.data(),
                   static_cast<uLong>(scanlines.size()),
                   Z_BEST_COMPRESSION) == Z_OK);
  compressed.resize(compressed_size);

  std::vector<uint8_t> png{137, 80, 78, 71, 13, 10, 26, 10};
  std::vector<uint8_t> header;
  appendBe32(&header, width);
  appendBe32(&header, height);
  header.insert(header.end(), {bit_depth, color_type, 0, 0, interlace});
  appendChunk(&png, {'I', 'H', 'D', 'R'}, header);
  if (!palette.empty())
    appendChunk(&png, {'P', 'L', 'T', 'E'}, palette);
  if (!transparency.empty())
    appendChunk(&png, {'t', 'R', 'N', 'S'}, transparency);
  appendChunk(&png, {'I', 'D', 'A', 'T'}, compressed);
  appendChunk(&png, {'I', 'E', 'N', 'D'}, nullptr, 0);

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  assert(output);
  output.write(reinterpret_cast<const char *>(png.data()),
               static_cast<std::streamsize>(png.size()));
  assert(output);
}

std::vector<uint8_t> packIndexedRows(uint32_t width, uint32_t height,
                                     uint8_t bit_depth,
                                     const std::vector<uint8_t> &indices) {
  assert(indices.size() == static_cast<uint64_t>(width) * height);
  const size_t row_bytes = (static_cast<size_t>(width) * bit_depth + 7) / 8;
  std::vector<uint8_t> scanlines;
  scanlines.reserve(height * (row_bytes + 1));
  const uint8_t mask = static_cast<uint8_t>((1U << bit_depth) - 1U);
  for (uint32_t y = 0; y < height; ++y) {
    scanlines.push_back(0);
    const size_t row_start = scanlines.size();
    scanlines.resize(row_start + row_bytes, 0);
    for (uint32_t x = 0; x < width; ++x) {
      const size_t bit_offset = static_cast<size_t>(x) * bit_depth;
      const uint8_t shift = static_cast<uint8_t>(8 - bit_depth - (bit_offset & 7));
      scanlines[row_start + (bit_offset >> 3)] |=
          static_cast<uint8_t>((indices[static_cast<size_t>(y) * width + x] & mask)
                               << shift);
    }
  }
  return scanlines;
}

std::vector<uint8_t> makeAdam7RgbaScanlines(uint32_t width, uint32_t height,
                                            const std::vector<uint8_t> &rgba) {
  assert(rgba.size() == static_cast<uint64_t>(width) * height * 4);
  constexpr std::array<uint32_t, 7> start_x{{0, 4, 0, 2, 0, 1, 0}};
  constexpr std::array<uint32_t, 7> start_y{{0, 0, 4, 0, 2, 0, 1}};
  constexpr std::array<uint32_t, 7> step_x{{8, 8, 4, 4, 2, 2, 1}};
  constexpr std::array<uint32_t, 7> step_y{{8, 8, 8, 4, 4, 2, 2}};
  std::vector<uint8_t> scanlines;
  for (size_t pass = 0; pass < start_x.size(); ++pass) {
    if (width <= start_x[pass] || height <= start_y[pass])
      continue;
    for (uint32_t y = start_y[pass]; y < height; y += step_y[pass]) {
      scanlines.push_back(0);
      for (uint32_t x = start_x[pass]; x < width; x += step_x[pass]) {
        const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
        scanlines.insert(scanlines.end(), rgba.begin() + static_cast<std::ptrdiff_t>(offset),
                         rgba.begin() + static_cast<std::ptrdiff_t>(offset + 4));
      }
    }
  }
  return scanlines;
}

std::vector<uint8_t>
makeAdam7IndexedScanlines(uint32_t width, uint32_t height, uint8_t bit_depth,
                          const std::vector<uint8_t> &indices) {
  assert(indices.size() == static_cast<uint64_t>(width) * height);
  constexpr std::array<uint32_t, 7> start_x{{0, 4, 0, 2, 0, 1, 0}};
  constexpr std::array<uint32_t, 7> start_y{{0, 0, 4, 0, 2, 0, 1}};
  constexpr std::array<uint32_t, 7> step_x{{8, 8, 4, 4, 2, 2, 1}};
  constexpr std::array<uint32_t, 7> step_y{{8, 8, 8, 4, 4, 2, 2}};
  std::vector<uint8_t> scanlines;
  for (size_t pass = 0; pass < start_x.size(); ++pass) {
    if (width <= start_x[pass] || height <= start_y[pass])
      continue;
    const uint32_t pass_width =
        1 + (width - 1 - start_x[pass]) / step_x[pass];
    for (uint32_t y = start_y[pass]; y < height; y += step_y[pass]) {
      std::vector<uint8_t> pass_indices;
      pass_indices.reserve(pass_width);
      for (uint32_t x = start_x[pass]; x < width; x += step_x[pass])
        pass_indices.push_back(indices[static_cast<size_t>(y) * width + x]);
      const std::vector<uint8_t> packed =
          packIndexedRows(pass_width, 1, bit_depth, pass_indices);
      scanlines.insert(scanlines.end(), packed.begin(), packed.end());
    }
  }
  return scanlines;
}

bool parsePng(const fs::path &source, const fs::path &spool,
              int32_t target_width, SchematicParseResult *result,
              std::string *error,
              std::vector<SchematicParseProgress> *progress = nullptr) {
  PixelArtParseOptions options;
  options.source_path = source.string();
  options.spool_directory = spool.string();
  options.base_x = 100;
  options.base_y = 64;
  options.base_z = -40;
  options.target_width = target_width;
  options.chunk_size = 32;
  if (progress) {
    options.progress_callback = [progress](const SchematicParseProgress &value) {
      progress->push_back(value);
    };
  }
  return PixelArtParser().parse(options, result, error);
}

std::vector<RawBlock> readRawSpool(const std::string &path) {
  std::vector<RawBlock> blocks;
  for (build_import_test::DecodedRawRecord decoded :
       build_import_test::readRawSpool(path)) {
    RawBlock block;
    block.record = {decoded.x, decoded.y, decoded.z,
                    static_cast<uint8_t>(decoded.aux), decoded.flags,
                    static_cast<uint16_t>(decoded.name.size())};
    block.name = std::move(decoded.name);
    blocks.push_back(std::move(block));
  }
  return blocks;
}

std::vector<RawBlock> readAllStructureBlocks(const SchematicParseResult &result) {
  std::vector<RawBlock> blocks;
  for (const ChunkDescriptor &chunk : result.chunks) {
    const size_t phase = phaseIndex(ImportPhase::Structure);
    if (!chunk.has_phase[phase])
      continue;
    std::vector<RawBlock> chunk_blocks = readRawSpool(chunk.spool_paths[phase]);
    blocks.insert(blocks.end(), std::make_move_iterator(chunk_blocks.begin()),
                  std::make_move_iterator(chunk_blocks.end()));
  }
  return blocks;
}

std::vector<RawBlock> readAllBlocks(const SchematicParseResult &result) {
  std::vector<RawBlock> blocks;
  for (const ChunkDescriptor &chunk : result.chunks) {
    for (size_t phase = 0; phase < kImportPhaseCount; ++phase) {
      if (!chunk.has_phase[phase])
        continue;
      std::vector<RawBlock> chunk_blocks = readRawSpool(chunk.spool_paths[phase]);
      blocks.insert(blocks.end(), std::make_move_iterator(chunk_blocks.begin()),
                    std::make_move_iterator(chunk_blocks.end()));
    }
  }
  return blocks;
}

const RawBlock *findBlockAt(const std::vector<RawBlock> &blocks, int32_t x,
                            int32_t z) {
  const auto found =
      std::find_if(blocks.begin(), blocks.end(), [=](const RawBlock &block) {
        return block.record.x == x && block.record.y == 64 && block.record.z == z;
      });
  return found == blocks.end() ? nullptr : &*found;
}

bool containsRegularFile(const fs::path &directory) {
  std::error_code error;
  if (!fs::exists(directory, error))
    return false;
  for (fs::recursive_directory_iterator it(directory, error), end;
       it != end && !error; it.increment(error)) {
    if (it->is_regular_file(error))
      return true;
  }
  return false;
}

void testTransparentPixelsAreNotSpooled(const ScopedTempDirectory &temporary) {
  const fs::path source = temporary.child("mixed_alpha.png");
  const fs::path spool = temporary.child("mixed_alpha_spool");
  writePng(source, 2, 1, 6, {0, 0, 0, 0, 220, 220, 220, 255});

  SchematicParseResult result;
  std::string error;
  std::vector<SchematicParseProgress> progress;
  assert(parsePng(source, spool, 2, &result, &error, &progress));
  assert(error.empty());
  assert(!progress.empty());
  assert(progress.front().stage == SchematicParseStage::ReadingSource);
  assert(std::any_of(progress.begin(), progress.end(),
                     [](const SchematicParseProgress &value) {
                       return value.stage == SchematicParseStage::RoutingBlocks;
                     }));
  assert(progress.back().stage == SchematicParseStage::FinalizingSpools);
  assert(progress.back().completed == progress.back().total);
  assert(result.source_voxel_count == 2);
  assert(result.imported_block_count == 1);
  assert(result.skipped_block_count == 1);
  assert(result.chunks.size() == 1);
  const ChunkDescriptor &chunk = result.chunks.front();
  assert(chunk.has_phase[phaseIndex(ImportPhase::Structure)]);
  assert(chunk.imported_bounds.min_x == 100 &&
         chunk.imported_bounds.max_x == 101);
  const std::vector<RawBlock> blocks =
      readRawSpool(chunk.spool_paths[phaseIndex(ImportPhase::Structure)]);
  assert(blocks.size() == 1);
  assert(blocks.front().name == "minecraft:wool");

  const fs::path empty_source = temporary.child("fully_transparent.png");
  const fs::path empty_spool = temporary.child("fully_transparent_spool");
  writePng(empty_source, 1, 1, 6, {0, 0, 0, 0});
  result = {};
  error.clear();
  assert(parsePng(empty_source, empty_spool, 1, &result, &error));
  assert(error.empty());
  assert(result.imported_block_count == 0);
  assert(result.skipped_block_count == 1);
  assert(result.chunks.size() == 1);
  assert(result.chunks.front().imported_bounds.min_x == 100);
  assert(result.chunks.front().imported_bounds.max_x == 100);
  assert(std::none_of(result.chunks.front().has_phase.begin(),
                      result.chunks.front().has_phase.end(),
                      [](bool value) { return value; }));
  assert(!containsRegularFile(empty_spool));
}

void testBlockSinkBypassesChunkDescriptors(
    const ScopedTempDirectory &temporary) {
  const fs::path source = temporary.child("block_sink.png");
  const fs::path spool = temporary.child("block_sink_spool");
  writePng(source, 2, 1, 6, {0, 0, 0, 0, 220, 220, 220, 255});

  PixelArtParseOptions options;
  options.source_path = source.string();
  options.spool_directory = spool.string();
  options.base_x = 100;
  options.base_y = 64;
  options.base_z = -40;
  options.target_width = 2;
  options.chunk_size = 32;
  std::vector<ParsedBlock> blocks;
  options.block_sink = [&](const ParsedBlock &block, std::string *) {
    blocks.push_back(block);
    return true;
  };

  SchematicParseResult result;
  std::string error;
  assert(PixelArtParser().parse(options, &result, &error));
  assert(error.empty());
  assert(result.source_voxel_count == 2);
  assert(result.imported_block_count == 1 && result.skipped_block_count == 1);
  assert(result.chunks.empty() && blocks.size() == 1);
  assert(blocks.front().world_x == 101 && blocks.front().world_y == 64 &&
         blocks.front().world_z == -40);
  assert(blocks.front().spec.command_name == "minecraft:wool");
}

void testOptionalMapAlignmentPreservesImageBounds(
    const ScopedTempDirectory &temporary) {
  const fs::path source = temporary.child("map_alignment.png");
  writePng(source, 2, 1, 6,
           {255, 0, 0, 255, 0, 0, 255, 255});

  struct AlignmentCase {
    int32_t base_x;
    int32_t base_z;
    bool create_maps;
    int32_t expected_min_x;
    int32_t expected_min_z;
  };
  const std::array<AlignmentCase, 3> cases{{
      {100, 200, true, 64, 192},
      {-100, -200, true, -192, -320},
      {100, -200, false, 100, -200},
  }};

  for (size_t index = 0; index < cases.size(); ++index) {
    const AlignmentCase &test_case = cases[index];
    PixelArtParseOptions options;
    options.source_path = source.string();
    options.spool_directory =
        temporary.child("map_alignment_spool_" + std::to_string(index)).string();
    options.base_x = test_case.base_x;
    options.base_y = 64;
    options.base_z = test_case.base_z;
    options.target_width = 2;
    options.chunk_size = 32;
    options.create_maps_after_import = test_case.create_maps;
    std::vector<ParsedBlock> blocks;
    options.block_sink = [&](const ParsedBlock &block, std::string *) {
      blocks.push_back(block);
      return true;
    };

    SchematicParseResult result;
    std::string error;
    assert(PixelArtParser().parse(options, &result, &error));
    assert(error.empty());
    assert(result.imported_block_count == 2 && blocks.size() == 2);
    assert(result.source_volume_bounds.min_x == test_case.expected_min_x);
    assert(result.source_volume_bounds.max_x == test_case.expected_min_x + 1);
    assert(result.source_volume_bounds.min_y == 64);
    assert(result.source_volume_bounds.max_y == 64);
    assert(result.source_volume_bounds.min_z == test_case.expected_min_z);
    assert(result.source_volume_bounds.max_z == test_case.expected_min_z);
    assert(blocks[0].world_x == test_case.expected_min_x);
    assert(blocks[1].world_x == test_case.expected_min_x + 1);
    assert(blocks[0].world_y == 64 && blocks[1].world_y == 64);
    assert(blocks[0].world_z == test_case.expected_min_z &&
           blocks[1].world_z == test_case.expected_min_z);
  }
}

void testWaterUsesFluidPhase(const ScopedTempDirectory &temporary) {
  const fs::path source = temporary.child("water.png");
  const fs::path spool = temporary.child("water_spool");
  writePng(source, 1, 1, 2, {55, 55, 220});

  SchematicParseResult result;
  std::string error;
  assert(parsePng(source, spool, 1, &result, &error));
  assert(result.imported_block_count == 1 && result.chunks.size() == 1);
  const ChunkDescriptor &chunk = result.chunks.front();
  assert(chunk.has_phase[phaseIndex(ImportPhase::Fluid)]);
  assert(!chunk.has_phase[phaseIndex(ImportPhase::Structure)]);
  const std::vector<RawBlock> blocks =
      readRawSpool(chunk.spool_paths[phaseIndex(ImportPhase::Fluid)]);
  assert(blocks.size() == 1);
  assert(blocks.front().name == "minecraft:water");
  assert(blocks.front().record.aux == 0);
}

void testMultipleIdatChunks(const ScopedTempDirectory &temporary) {
  const fs::path source = temporary.child("many_idat.png");
  const fs::path spool = temporary.child("many_idat_spool");
  // One byte per IDAT exercises zlib headers, payload and checksum crossing
  // PNG chunk boundaries independently of scanline boundaries.
  writePng(source, 2, 2, 2,
           {220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220}, {}, 1);

  SchematicParseResult result;
  std::string error;
  assert(parsePng(source, spool, 2, &result, &error));
  assert(error.empty());
  assert(result.source_voxel_count == 4);
  assert(result.imported_block_count == 4);
}

void testTrailingCompressedDataIsRejected(
    const ScopedTempDirectory &temporary) {
  const fs::path source = temporary.child("trailing_zlib.png");
  const fs::path spool = temporary.child("trailing_zlib_spool");
  writePng(source, 1, 1, 2, {220, 220, 220}, {}, 0, {0x00, 0xff});

  SchematicParseResult result;
  std::string error;
  assert(!parsePng(source, spool, 1, &result, &error));
  assert(error == "PNG decompressed data length mismatch");
  assert(result.chunks.empty());
  assert(!containsRegularFile(spool));
}

void testTruecolourTrnsPixelsAreTransparent(
    const ScopedTempDirectory &temporary) {
  const fs::path source = temporary.child("truecolour_trns.png");
  const fs::path spool = temporary.child("truecolour_trns_spool");
  // The first pixel is the palette's exact water colour, but tRNS marks it
  // transparent. Only the second, opaque wool-colour pixel may be emitted.
  writePng(source, 2, 1, 2, {55, 55, 220, 220, 220, 220},
           {0, 55, 0, 55, 0, 220});

  SchematicParseResult result;
  std::string error;
  assert(parsePng(source, spool, 2, &result, &error));
  assert(result.imported_block_count == 1 && result.chunks.size() == 1);
  const ChunkDescriptor &chunk = result.chunks.front();
  assert(!chunk.has_phase[phaseIndex(ImportPhase::Fluid)]);
  assert(chunk.has_phase[phaseIndex(ImportPhase::Structure)]);
  const std::vector<RawBlock> blocks =
      readRawSpool(chunk.spool_paths[phaseIndex(ImportPhase::Structure)]);
  assert(blocks.size() == 1);
  assert(blocks.front().record.x == 101);
  assert(blocks.front().name == "minecraft:wool");
}

void testLowAlphaEdgesStayEmptyAndDoNotWhiten(
    const ScopedTempDirectory &temporary) {
  const fs::path source = temporary.child("low_alpha.png");
  const fs::path spool = temporary.child("low_alpha_spool");
  // Low-coverage antialias pixels are air. A mostly opaque black edge must
  // remain black instead of being composited onto white before palette lookup.
  writePng(source, 3, 1, 6,
           {21, 21, 21, 32, 21, 21, 21, 200, 255, 255, 255, 0});

  SchematicParseResult result;
  std::string error;
  assert(parsePng(source, spool, 3, &result, &error));
  assert(error.empty());
  assert(result.imported_block_count == 1);
  assert(result.skipped_block_count == 2);
  const std::vector<RawBlock> blocks = readAllStructureBlocks(result);
  assert(blocks.size() == 1);
  assert(blocks.front().record.x == 101);
  assert(blocks.front().name == "minecraft:wool");
  assert(blocks.front().record.aux == 15);
}

void testPackedPaletteDepths(const ScopedTempDirectory &temporary) {
  const std::vector<uint8_t> palette{
      21, 21, 21, 220, 220, 220, 220, 0, 0, 55, 55, 220};
  for (uint8_t depth : {uint8_t(1), uint8_t(2), uint8_t(4)}) {
    const uint32_t width = depth == 1 ? 2 : 4;
    std::vector<uint8_t> indices(width);
    for (uint32_t x = 0; x < width; ++x)
      indices[x] = static_cast<uint8_t>(x);
    const fs::path source = temporary.child("indexed_" + std::to_string(depth) + ".png");
    const fs::path spool = temporary.child("indexed_" + std::to_string(depth) + "_spool");
    const size_t palette_size = depth == 1 ? 6 : palette.size();
    writeRawPng(source, width, 1, depth, 3, 0,
                packIndexedRows(width, 1, depth, indices),
                std::vector<uint8_t>(palette.begin(), palette.begin() +
                                     static_cast<std::ptrdiff_t>(palette_size)),
                std::vector<uint8_t>(width, 255));

    SchematicParseResult result;
    std::string error;
    assert(parsePng(source, spool, static_cast<int32_t>(width), &result, &error));
    assert(error.empty());
    assert(result.source_voxel_count == width);
    assert(result.imported_block_count == width);
  }

  const fs::path transparent_source = temporary.child("indexed_transparent.png");
  const fs::path transparent_spool = temporary.child("indexed_transparent_spool");
  writeRawPng(transparent_source, 4, 1, 2, 3, 0,
              packIndexedRows(4, 1, 2, {0, 1, 2, 3}), palette,
              {0, 255, 255, 255});
  SchematicParseResult result;
  std::string error;
  assert(parsePng(transparent_source, transparent_spool, 4, &result, &error));
  assert(result.imported_block_count == 3);
  assert(result.skipped_block_count == 1);
  std::vector<RawBlock> blocks = readAllStructureBlocks(result);
  // Water is routed to the fluid phase, so only white and red are structure.
  assert(blocks.size() == 2);
  assert(std::none_of(blocks.begin(), blocks.end(), [](const RawBlock &block) {
    return block.record.x == 100;
  }));
}

void testAdam7Decoding(const ScopedTempDirectory &temporary) {
  constexpr uint32_t width = 5;
  constexpr uint32_t height = 5;
  std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
  for (size_t pixel = 0; pixel < static_cast<size_t>(width) * height; ++pixel) {
    rgba[pixel * 4] = 220;
    rgba[pixel * 4 + 1] = 220;
    rgba[pixel * 4 + 2] = 220;
    rgba[pixel * 4 + 3] = 255;
  }
  const size_t transparent = (2 * width + 2) * 4;
  rgba[transparent] = rgba[transparent + 1] = rgba[transparent + 2] = 0;
  rgba[transparent + 3] = 0;
  const fs::path source = temporary.child("adam7.png");
  const fs::path spool = temporary.child("adam7_spool");
  writeRawPng(source, width, height, 8, 6, 1,
              makeAdam7RgbaScanlines(width, height, rgba));

  SchematicParseResult result;
  std::string error;
  assert(parsePng(source, spool, width, &result, &error));
  assert(error.empty());
  assert(result.source_voxel_count == width * height);
  assert(result.imported_block_count == width * height - 1);
  assert(result.skipped_block_count == 1);
  const std::vector<RawBlock> blocks = readAllStructureBlocks(result);
  assert(blocks.size() == width * height - 1);
  assert(std::none_of(blocks.begin(), blocks.end(), [](const RawBlock &block) {
    return block.record.x == 102 && block.record.z == -38;
  }));
  assert(!fs::exists(spool / ".pixelart_rgba.tmp"));
  assert(!fs::exists(spool / ".pixelart_errors_a.tmp"));
  assert(!fs::exists(spool / ".pixelart_errors_b.tmp"));
}

void testAdam7PackedPaletteDepths(const ScopedTempDirectory &temporary) {
  constexpr uint32_t width = 9;
  constexpr uint32_t height = 9;
  const std::vector<uint8_t> palette{
      220, 220, 220, 21, 21, 21, 220, 0, 0, 55, 55, 220};

  for (uint8_t depth : {uint8_t(1), uint8_t(2), uint8_t(4)}) {
    const uint8_t palette_entries = depth == 1 ? 2 : 4;
    std::vector<uint8_t> indices(static_cast<size_t>(width) * height);
    uint64_t expected_blocks = 0;
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const uint8_t index = static_cast<uint8_t>((x + y * 2) % palette_entries);
        indices[static_cast<size_t>(y) * width + x] = index;
        if (index != 1)
          ++expected_blocks;
      }
    }

    const fs::path source =
        temporary.child("adam7_indexed_" + std::to_string(depth) + ".png");
    const fs::path spool = temporary.child("adam7_indexed_" +
                                            std::to_string(depth) + "_spool");
    const size_t palette_size = static_cast<size_t>(palette_entries) * 3;
    std::vector<uint8_t> transparency(palette_entries, 255);
    transparency[1] = 0;
    writeRawPng(source, width, height, depth, 3, 1,
                makeAdam7IndexedScanlines(width, height, depth, indices),
                std::vector<uint8_t>(palette.begin(),
                                     palette.begin() + palette_size),
                transparency);

    SchematicParseResult result;
    std::string error;
    assert(parsePng(source, spool, width, &result, &error));
    assert(error.empty());
    assert(result.source_voxel_count == width * height);
    assert(result.imported_block_count == expected_blocks);
    assert(result.skipped_block_count == width * height - expected_blocks);
    const std::vector<RawBlock> blocks = readAllBlocks(result);
    assert(blocks.size() == expected_blocks);

    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const uint8_t index = indices[static_cast<size_t>(y) * width + x];
        const RawBlock *block = findBlockAt(
            blocks, static_cast<int32_t>(100 + x), static_cast<int32_t>(-40 + y));
        if (index == 1) {
          assert(block == nullptr);
        } else if (index == 0) {
          assert(block && block->name == "minecraft:wool" &&
                 block->record.aux == 0);
        } else if (index == 2) {
          assert(block && block->name == "minecraft:redstone_block" &&
                 block->record.aux == 0);
        } else {
          assert(block && block->name == "minecraft:water" &&
                 block->record.aux == 0);
        }
      }
    }
    assert(!fs::exists(spool / ".pixelart_rgba.tmp"));
    assert(!fs::exists(spool / ".pixelart_errors_a.tmp"));
    assert(!fs::exists(spool / ".pixelart_errors_b.tmp"));
  }
}

void testQuantizationErrorCrossesStripeBoundary(
    const ScopedTempDirectory &temporary) {
  constexpr uint32_t width = 4100;
  std::vector<uint8_t> rgba(static_cast<size_t>(width) * 4, 0);
  const auto set_pixel = [&](uint32_t x, uint8_t r, uint8_t g, uint8_t b) {
    const size_t offset = static_cast<size_t>(x) * 4;
    rgba[offset] = r;
    rgba[offset + 1] = g;
    rgba[offset + 2] = b;
    rgba[offset + 3] = 255;
  };
  // Pixel 4095 maps to white wool and contributes -4.375 green error to the
  // next pixel. With that carried error pixel 4096 maps to quartz; if a stripe
  // resets horizontal diffusion it incorrectly remains white wool.
  set_pixel(4095, 220, 210, 220);
  set_pixel(4096, 220, 219, 216);

  const fs::path source = temporary.child("stripe_error.png");
  const fs::path spool = temporary.child("stripe_error_spool");
  writePng(source, width, 1, 6, rgba);

  SchematicParseResult result;
  std::string error;
  assert(parsePng(source, spool, width, &result, &error));
  assert(error.empty());
  assert(result.source_voxel_count == width);
  assert(result.imported_block_count == 2);
  const std::vector<RawBlock> blocks = readAllBlocks(result);
  assert(blocks.size() == 2);
  const RawBlock *before = findBlockAt(blocks, 100 + 4095, -40);
  const RawBlock *after = findBlockAt(blocks, 100 + 4096, -40);
  assert(before && before->name == "minecraft:wool" &&
         before->record.aux == 0);
  assert(after && after->name == "minecraft:quartz_block" &&
         after->record.aux == 0);
}

void testWideRowsCrossStripeBoundary(const ScopedTempDirectory &temporary) {
  constexpr uint32_t width = 4100;
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * 3, 220);
  const fs::path source = temporary.child("wide.png");
  const fs::path spool = temporary.child("wide_spool");
  writePng(source, width, 1, 2, rgb);

  SchematicParseResult result;
  std::string error;
  assert(parsePng(source, spool, width, &result, &error));
  assert(error.empty());
  assert(result.source_voxel_count == width);
  assert(result.imported_block_count == width);
  std::vector<RawBlock> blocks = readAllStructureBlocks(result);
  assert(blocks.size() == width);
  std::sort(blocks.begin(), blocks.end(), [](const RawBlock &left, const RawBlock &right) {
    return left.record.x < right.record.x;
  });
  for (uint32_t x = 0; x < width; ++x) {
    assert(blocks[x].record.x == static_cast<int32_t>(100 + x));
    assert(blocks[x].record.z == -40);
    assert(blocks[x].name == "minecraft:wool");
  }
  assert(!fs::exists(spool / ".pixelart_rgba.tmp"));
  assert(!fs::exists(spool / ".pixelart_errors_a.tmp"));
  assert(!fs::exists(spool / ".pixelart_errors_b.tmp"));
}

void testTargetWidthPreservesAspectRatio(const ScopedTempDirectory &temporary) {
  const fs::path source = temporary.child("aspect.png");
  const fs::path spool = temporary.child("aspect_spool");
  writePng(source, 4, 2, 2,
           {220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220,
            220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220, 220});

  SchematicParseResult result;
  std::string error;
  assert(parsePng(source, spool, 10, &result, &error));
  assert(error.empty());
  assert(result.source_voxel_count == 50);
  assert(result.imported_block_count == 50);
  assert(result.source_volume_bounds.min_x == 100);
  assert(result.source_volume_bounds.min_y == 64);
  assert(result.source_volume_bounds.min_z == -40);
  assert(result.source_volume_bounds.max_x == 109);
  assert(result.source_volume_bounds.max_y == 64);
  assert(result.source_volume_bounds.max_z == -36);
  std::vector<RawBlock> blocks = readAllStructureBlocks(result);
  assert(blocks.size() == 50);
  const auto max_x = std::max_element(blocks.begin(), blocks.end(),
      [](const RawBlock &left, const RawBlock &right) {
        return left.record.x < right.record.x;
      });
  const auto max_z = std::max_element(blocks.begin(), blocks.end(),
      [](const RawBlock &left, const RawBlock &right) {
        return left.record.z < right.record.z;
      });
  assert(max_x->record.x == 109);
  assert(max_z->record.z == -36);
}

void testCompactAndDiskScalersMatch(const ScopedTempDirectory &temporary) {
  constexpr uint32_t width = 4;
  constexpr uint32_t height = 3;
  std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
      rgba[offset] = static_cast<uint8_t>(20 + x * 53 + y * 11);
      rgba[offset + 1] = static_cast<uint8_t>(210 - x * 31 - y * 17);
      rgba[offset + 2] = static_cast<uint8_t>(35 + x * 23 + y * 61);
      rgba[offset + 3] = (x == 1 && y == 1) ? 0 : 255;
    }
  }
  const fs::path source = temporary.child("scaler_equivalence.png");
  writePng(source, width, height, 6, rgba);

  const auto parse_with_budget = [&](const fs::path &spool, size_t budget,
                                     std::vector<ParsedBlock> *blocks,
                                     SchematicParseResult *result) {
    PixelArtParseOptions options;
    options.source_path = source.string();
    options.spool_directory = spool.string();
    options.base_x = 100;
    options.base_y = 64;
    options.base_z = -40;
    options.target_width = 7;
    options.chunk_size = 32;
    options.streaming_memory_budget_bytes = budget;
    options.block_sink = [blocks](const ParsedBlock &block, std::string *) {
      blocks->push_back(block);
      return true;
    };
    std::string error;
    assert(PixelArtParser().parse(options, result, &error));
    assert(error.empty());
  };

  std::vector<ParsedBlock> compact_blocks;
  std::vector<ParsedBlock> disk_blocks;
  SchematicParseResult compact_result;
  SchematicParseResult disk_result;
  const fs::path compact_spool = temporary.child("compact_scaler_spool");
  const fs::path disk_spool = temporary.child("disk_scaler_spool");
  parse_with_budget(compact_spool, 8U * 1024U * 1024U,
                    &compact_blocks, &compact_result);
  parse_with_budget(disk_spool, 0, &disk_blocks, &disk_result);
  const auto order = [](const ParsedBlock &left, const ParsedBlock &right) {
    if (left.world_z != right.world_z) return left.world_z < right.world_z;
    return left.world_x < right.world_x;
  };
  std::sort(compact_blocks.begin(), compact_blocks.end(), order);
  std::sort(disk_blocks.begin(), disk_blocks.end(), order);
  assert(compact_result.source_voxel_count == disk_result.source_voxel_count);
  assert(compact_result.imported_block_count == disk_result.imported_block_count);
  assert(compact_result.skipped_block_count == disk_result.skipped_block_count);
  assert(compact_blocks.size() == disk_blocks.size());
  for (size_t index = 0; index < compact_blocks.size(); ++index) {
    const ParsedBlock &compact = compact_blocks[index];
    const ParsedBlock &disk = disk_blocks[index];
    assert(compact.world_x == disk.world_x && compact.world_y == disk.world_y &&
           compact.world_z == disk.world_z);
    assert(compact.spec.command_name == disk.spec.command_name);
    assert(compact.spec.aux == disk.spec.aux);
    assert(compact.spec.phase == disk.spec.phase);
  }
  assert(!fs::exists(disk_spool / ".pixelart_rgba.tmp"));
  assert(!fs::exists(disk_spool / ".pixelart_errors_a.tmp"));
  assert(!fs::exists(disk_spool / ".pixelart_errors_b.tmp"));
}

void testChunkDescriptorLimitIsForwarded(
    const ScopedTempDirectory &temporary) {
  const fs::path source = temporary.child("descriptor_limit.png");
  const fs::path spool = temporary.child("descriptor_limit_spool");
  writePng(source, 1, 1, 2, {220, 220, 220});

  PixelArtParseOptions options;
  options.source_path = source.string();
  options.spool_directory = spool.string();
  options.base_x = 100;
  options.base_y = 64;
  options.base_z = -40;
  options.target_width = 64;
  options.chunk_size = 32;
  options.maximum_chunk_descriptors = 1;
  SchematicParseResult result;
  std::string error;
  assert(!PixelArtParser().parse(options, &result, &error));
  assert(error == "source volume spans too many logical chunks");
  assert(result.chunks.empty());
}

} // namespace

int main() {
  const ScopedTempDirectory temporary;
  testTransparentPixelsAreNotSpooled(temporary);
  testBlockSinkBypassesChunkDescriptors(temporary);
  testOptionalMapAlignmentPreservesImageBounds(temporary);
  testWaterUsesFluidPhase(temporary);
  testMultipleIdatChunks(temporary);
  testTrailingCompressedDataIsRejected(temporary);
  testTruecolourTrnsPixelsAreTransparent(temporary);
  testLowAlphaEdgesStayEmptyAndDoNotWhiten(temporary);
  testPackedPaletteDepths(temporary);
  testAdam7Decoding(temporary);
  testAdam7PackedPaletteDepths(temporary);
  testQuantizationErrorCrossesStripeBoundary(temporary);
  testWideRowsCrossStripeBoundary(temporary);
  testTargetWidthPreservesAspectRatio(temporary);
  testCompactAndDiskScalersMatch(temporary);
  testChunkDescriptorLimitIsForwarded(temporary);
  return 0;
}
