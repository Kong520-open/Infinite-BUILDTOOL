#include "PixelArtParser.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>
#include <zlib.h>

namespace build_import {
namespace {

struct Rgba { uint8_t r, g, b, a = 255; };
struct PaletteBlock {
    uint8_t r, g, b, aux;
    const char* name;
    ImportPhase phase = ImportPhase::Structure;
    bool can_fill = true;
    bool single_layer_only = false;
};

bool cancellationRequested(const std::function<bool()>& callback, std::string* error) {
    if (!callback || !callback()) return false;
    if (error) *error = "import cancelled";
    return true;
}

void reportProgress(const PixelArtParseOptions& options, SchematicParseStage stage,
                    uint64_t completed = 0, uint64_t total = 0) {
    if (options.progress_callback) {
        options.progress_callback({stage, completed, total});
    }
}

// Same base palette as the reference Python converter: wool, stained clay,
// then its single-colour Nether/deepslate additions.
constexpr PaletteBlock kPalette[] = {
    {220,220,220,0,"minecraft:wool"},{186,109,44,1,"minecraft:wool"},{153,65,186,2,"minecraft:wool"},{88,132,186,3,"minecraft:wool"},
    {197,197,44,4,"minecraft:wool"},{109,176,21,5,"minecraft:wool"},{208,109,142,6,"minecraft:wool"},{65,65,65,7,"minecraft:wool"},
    {132,132,132,8,"minecraft:wool"},{65,109,132,9,"minecraft:wool"},{109,54,153,10,"minecraft:wool"},{44,65,153,11,"minecraft:wool"},
    {88,65,44,12,"minecraft:wool"},{88,109,44,13,"minecraft:wool"},{132,44,44,14,"minecraft:wool"},{21,21,21,15,"minecraft:wool"},
    {180,152,138,0,"minecraft:stained_hardened_clay"},{137,70,31,1,"minecraft:stained_hardened_clay"},{128,75,93,2,"minecraft:stained_hardened_clay"},{96,93,119,3,"minecraft:stained_hardened_clay"},
    {160,114,31,4,"minecraft:stained_hardened_clay"},{88,100,45,5,"minecraft:stained_hardened_clay"},{138,66,67,6,"minecraft:stained_hardened_clay"},{49,35,30,7,"minecraft:stained_hardened_clay"},
    {116,92,84,8,"minecraft:stained_hardened_clay"},{75,79,79,9,"minecraft:stained_hardened_clay"},{105,62,75,10,"minecraft:stained_hardened_clay"},{65,53,79,11,"minecraft:stained_hardened_clay"},
    {65,43,30,12,"minecraft:stained_hardened_clay"},{65,70,36,13,"minecraft:stained_hardened_clay"},{122,51,39,14,"minecraft:stained_hardened_clay"},{31,18,13,15,"minecraft:stained_hardened_clay"},
    {109,153,48,0,"minecraft:grass_block"},{213,201,140,2,"minecraft:planks"},{130,94,66,3,"minecraft:planks"},{123,102,62,0,"minecraft:planks"},{111,74,42,1,"minecraft:planks"},
    {220,0,0,0,"minecraft:redstone_block"},{144,144,144,0,"minecraft:iron_block"},{215,205,66,0,"minecraft:gold_block"},{79,188,183,0,"minecraft:diamond_block"},{63,110,220,0,"minecraft:lapis_block"},
    {0,187,50,0,"minecraft:emerald_block"},{220,217,211,0,"minecraft:quartz_block"},{96,96,96,0,"minecraft:cobblestone"},{96,1,0,0,"minecraft:netherrack"},{141,144,158,0,"minecraft:clay"},
    {138,138,220,0,"minecraft:ice"},{0,106,0,0,"minecraft:leaves"},{55,55,220,0,"minecraft:water",ImportPhase::Fluid,true,false},{163,41,42,0,"minecraft:crimson_nylium"},{127,54,83,0,"minecraft:crimson_planks"},
    {79,21,25,0,"minecraft:crimson_hyphae"},{18,108,115,0,"minecraft:warped_nylium"},{50,122,120,0,"minecraft:warped_planks"},{74,37,53,0,"minecraft:warped_hyphae"},
    {17,155,114,0,"minecraft:warped_wart_block"},{86,86,86,0,"minecraft:cobbled_deepslate"},{186,150,126,0,"minecraft:raw_iron_block"},{109,144,129,63,"minecraft:glow_lichen",ImportPhase::Attachment,false,true},
};
constexpr size_t kPaletteCount = sizeof(kPalette) / sizeof(kPalette[0]);

class ExactPaletteSearch {
public:
    ExactPaletteSearch() {
        std::iota(indices_.begin(), indices_.end(), uint16_t{0});
        root_ = build(0, indices_.size());
    }

    const PaletteBlock& closest(float r, float g, float b) const {
        size_t best_index = 0;
        float best_distance = distanceSquared(kPalette[0], r, g, b);
        nearest(root_, r, g, b, &best_index, &best_distance);
        return kPalette[best_index];
    }

private:
    struct Node {
        uint16_t palette_index = 0;
        int16_t left = -1;
        int16_t right = -1;
        uint8_t axis = 0;
    };

    static float component(const PaletteBlock& block, uint8_t axis) {
        return axis == 0 ? block.r : axis == 1 ? block.g : block.b;
    }

    static float queryComponent(float r, float g, float b, uint8_t axis) {
        return axis == 0 ? r : axis == 1 ? g : b;
    }

    static float distanceSquared(const PaletteBlock& block,
                                 float r, float g, float b) {
        const float dr = r - block.r;
        const float dg = g - block.g;
        const float db = b - block.b;
        return dr * dr + dg * dg + db * db;
    }

    int16_t build(size_t begin, size_t end) {
        if (begin == end) return -1;
        uint8_t axis = 0;
        std::array<int32_t, 3> minimum{{255, 255, 255}};
        std::array<int32_t, 3> maximum{{0, 0, 0}};
        for (size_t offset = begin; offset < end; ++offset) {
            const PaletteBlock& block = kPalette[indices_[offset]];
            const std::array<int32_t, 3> value{{block.r, block.g, block.b}};
            for (size_t candidate = 0; candidate < value.size(); ++candidate) {
                minimum[candidate] = std::min(minimum[candidate], value[candidate]);
                maximum[candidate] = std::max(maximum[candidate], value[candidate]);
            }
        }
        if (maximum[1] - minimum[1] > maximum[axis] - minimum[axis]) axis = 1;
        if (maximum[2] - minimum[2] > maximum[axis] - minimum[axis]) axis = 2;
        const size_t middle = begin + (end - begin) / 2U;
        std::nth_element(indices_.begin() + static_cast<std::ptrdiff_t>(begin),
                         indices_.begin() + static_cast<std::ptrdiff_t>(middle),
                         indices_.begin() + static_cast<std::ptrdiff_t>(end),
                         [&](uint16_t left, uint16_t right) {
                             const float left_value = component(kPalette[left], axis);
                             const float right_value = component(kPalette[right], axis);
                             return left_value == right_value ? left < right
                                                              : left_value < right_value;
                         });
        const int16_t node_index = static_cast<int16_t>(node_count_++);
        nodes_[static_cast<size_t>(node_index)].palette_index = indices_[middle];
        nodes_[static_cast<size_t>(node_index)].axis = axis;
        nodes_[static_cast<size_t>(node_index)].left = build(begin, middle);
        nodes_[static_cast<size_t>(node_index)].right = build(middle + 1U, end);
        return node_index;
    }

    void nearest(int16_t node_index, float r, float g, float b,
                 size_t* best_index, float* best_distance) const {
        if (node_index < 0) return;
        const Node& node = nodes_[static_cast<size_t>(node_index)];
        const PaletteBlock& candidate = kPalette[node.palette_index];
        const float candidate_distance = distanceSquared(candidate, r, g, b);
        if (candidate_distance < *best_distance ||
            (candidate_distance == *best_distance && node.palette_index < *best_index)) {
            *best_index = node.palette_index;
            *best_distance = candidate_distance;
        }
        const float difference = queryComponent(r, g, b, node.axis) -
                                 component(candidate, node.axis);
        const int16_t near_child = difference < 0 ? node.left : node.right;
        const int16_t far_child = difference < 0 ? node.right : node.left;
        nearest(near_child, r, g, b, best_index, best_distance);
        if (difference * difference <= *best_distance) {
            nearest(far_child, r, g, b, best_index, best_distance);
        }
    }

    std::array<uint16_t, kPaletteCount> indices_{};
    std::array<Node, kPaletteCount> nodes_{};
    size_t node_count_ = 0;
    int16_t root_ = -1;
};

uint16_t be16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
bool checkedMultiply(uint64_t left, uint64_t right, uint64_t* result) {
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) return false;
    *result = left * right;
    return true;
}
bool checkedAdd(uint64_t left, uint64_t right, uint64_t* result) {
    if (right > std::numeric_limits<uint64_t>::max() - left) return false;
    *result = left + right;
    return true;
}
float clamp(float value) { return std::max(0.0f, std::min(255.0f, value)); }
float clampUnit(float value) { return std::max(0.0f, std::min(1.0f, value)); }
float clampError(float value) { return std::max(-255.0f, std::min(255.0f, value)); }
const PaletteBlock& closestBlock(float r, float g, float b);

// Target rows wider than this are processed as independent horizontal
// stripes.  The stripe width is deliberately unrelated to target_width so a
// caller can request a very wide mural without growing parser RSS.
constexpr uint32_t kTargetStripeWidth = 4096;
constexpr uint64_t kMaxPackedPngRowBytes = 32ULL * 1024 * 1024;
constexpr float kVisibleAlphaThreshold = 0.5f;

struct MappedInterval {
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t denominator = 1;
    uint32_t first = 0;
    uint32_t last = 0;
};

// Maps one source cell into the destination coordinate system without using
// long double. PNG dimensions are uint32 and target dimensions are capped to
// int32, so every product stays within uint64.
MappedInterval mappedInterval(uint32_t index, uint32_t source_count,
                              uint32_t destination_count) {
    const uint64_t start = static_cast<uint64_t>(index) * destination_count;
    const uint64_t end = (static_cast<uint64_t>(index) + 1U) * destination_count;
    const uint64_t denominator = source_count;
    const uint64_t first = start / denominator;
    const uint64_t last = (end + denominator - 1U) / denominator - 1U;
    return {start, end, denominator, static_cast<uint32_t>(first),
            static_cast<uint32_t>(std::min<uint64_t>(destination_count - 1U, last))};
}

double mappedOverlap(const MappedInterval& interval, uint32_t destination_index) {
    const uint64_t cell_start = static_cast<uint64_t>(destination_index) *
                                interval.denominator;
    const uint64_t cell_end = (static_cast<uint64_t>(destination_index) + 1U) *
                              interval.denominator;
    const uint64_t overlap_start = std::max(interval.start, cell_start);
    const uint64_t overlap_end = std::min(interval.end, cell_end);
    return overlap_end > overlap_start
        ? static_cast<double>(overlap_end - overlap_start) /
              static_cast<double>(interval.denominator)
        : 0.0;
}

uint32_t mappedFloor(uint32_t index, uint32_t source_count,
                     uint32_t destination_count) {
    return static_cast<uint32_t>(
        static_cast<uint64_t>(index) * destination_count / source_count);
}

uint32_t mappedCeil(uint32_t index, uint32_t source_count,
                    uint32_t destination_count) {
    const uint64_t numerator = static_cast<uint64_t>(index) * destination_count;
    return static_cast<uint32_t>((numerator + source_count - 1U) / source_count);
}

constexpr std::array<uint8_t, 8> kPngSignature{{137,80,78,71,13,10,26,10}};

struct PngMetadata {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t bit_depth = 0;
    uint8_t color_type = 0;
    uint8_t interlace = 0;
    std::vector<uint8_t> palette;
    std::vector<uint8_t> transparency;
};

bool inspectPng(const std::string& path, const std::function<bool()>& cancel,
                PngMetadata* metadata, std::string* error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const std::streampos end_position = input ? input.tellg() : std::streampos(-1);
    if (!input || end_position < static_cast<std::streamoff>(kPngSignature.size())) {
        if (error) *error = "cannot open PNG";
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(end_position);
    input.seekg(0, std::ios::beg);
    std::array<uint8_t, 8> signature{};
    if (!input.read(reinterpret_cast<char*>(signature.data()), signature.size()) ||
        signature != kPngSignature) {
        if (error) *error = "not a PNG file";
        return false;
    }

    uint64_t position = signature.size();
    bool has_header = false, has_palette = false, has_transparency = false;
    bool has_idat = false, idat_closed = false, has_end = false;
    while (position < file_size) {
        if (cancellationRequested(cancel, error)) return false;
        if (file_size - position < 12) {
            if (error) *error = "truncated PNG chunk";
            return false;
        }
        std::array<uint8_t, 4> length_bytes{}, type{};
        if (!input.read(reinterpret_cast<char*>(length_bytes.data()), length_bytes.size()) ||
            !input.read(reinterpret_cast<char*>(type.data()), type.size())) return false;
        const uint32_t length = be32(length_bytes.data());
        position += 8;
        if (uint64_t(length) + 4 > file_size - position) {
            if (error) *error = "truncated PNG chunk";
            return false;
        }
        const bool ihdr = std::equal(type.begin(), type.end(), reinterpret_cast<const uint8_t*>("IHDR"));
        const bool plte = std::equal(type.begin(), type.end(), reinterpret_cast<const uint8_t*>("PLTE"));
        const bool trns = std::equal(type.begin(), type.end(), reinterpret_cast<const uint8_t*>("tRNS"));
        const bool idat = std::equal(type.begin(), type.end(), reinterpret_cast<const uint8_t*>("IDAT"));
        const bool iend = std::equal(type.begin(), type.end(), reinterpret_cast<const uint8_t*>("IEND"));
        if (ihdr) {
            std::array<uint8_t, 13> header{};
            if (has_header || position != 16 || length != header.size() ||
                !input.read(reinterpret_cast<char*>(header.data()), header.size())) {
                if (error) *error = "invalid PNG header";
                return false;
            }
            metadata->width = be32(header.data());
            metadata->height = be32(header.data() + 4);
            metadata->bit_depth = header[8];
            metadata->color_type = header[9];
            metadata->interlace = header[12];
            if (header[10] != 0 || header[11] != 0) {
                if (error) *error = "unsupported PNG compression or filter method";
                return false;
            }
            has_header = true;
        } else if (plte || trns) {
            if (!has_header || has_idat || length > 768 ||
                (plte ? has_palette : has_transparency)) {
                if (error) *error = "invalid PNG palette";
                return false;
            }
            std::vector<uint8_t>& target = plte ? metadata->palette : metadata->transparency;
            target.resize(length);
            if (length != 0 && !input.read(reinterpret_cast<char*>(target.data()), length)) return false;
            if (plte) has_palette = true; else has_transparency = true;
        } else if (idat) {
            if (!has_header || idat_closed) {
                if (error) *error = "invalid PNG IDAT order";
                return false;
            }
            has_idat = true;
            input.seekg(length, std::ios::cur);
        } else {
            if (has_idat && !iend) idat_closed = true;
            if (iend && length != 0) {
                if (error) *error = "invalid PNG end chunk";
                return false;
            }
            input.seekg(length, std::ios::cur);
        }
        if (!input) return false;
        input.seekg(4, std::ios::cur);  // CRC is preserved in the source but not needed for decoding.
        if (!input) return false;
        position += uint64_t(length) + 4;
        if (iend) {
            has_end = true;
            break;
        }
    }
    if (!has_header || !has_idat || !has_end || position != file_size) {
        if (error) *error = "PNG is missing IHDR, IDAT or IEND, or has trailing data";
        return false;
    }
    return true;
}

class PngIdatReader {
public:
    explicit PngIdatReader(const std::string& path) : input_(path, std::ios::binary) {
        std::array<uint8_t, 8> signature{};
        valid_ = input_.read(reinterpret_cast<char*>(signature.data()), signature.size()) &&
                 signature == kPngSignature;
    }

    bool read(uint8_t* output, size_t capacity, size_t* received) {
        *received = 0;
        while (valid_ && !finished_) {
            if (remaining_ != 0) {
                const size_t part = std::min<uint64_t>(remaining_, capacity);
                if (!input_.read(reinterpret_cast<char*>(output), part)) return false;
                remaining_ -= part;
                *received = part;
                return true;
            }
            if (inside_idat_) {
                input_.seekg(4, std::ios::cur);  // Previous IDAT CRC.
                if (!input_) return false;
                inside_idat_ = false;
            }
            std::array<uint8_t, 4> length_bytes{}, type{};
            if (!input_.read(reinterpret_cast<char*>(length_bytes.data()), length_bytes.size()) ||
                !input_.read(reinterpret_cast<char*>(type.data()), type.size())) return false;
            const uint32_t length = be32(length_bytes.data());
            const bool idat = std::equal(type.begin(), type.end(), reinterpret_cast<const uint8_t*>("IDAT"));
            if (idat) {
                seen_idat_ = true;
                inside_idat_ = true;
                remaining_ = length;
                if (remaining_ == 0) continue;
            } else if (seen_idat_) {
                finished_ = true;
                return true;
            } else {
                input_.seekg(static_cast<std::streamoff>(length) + 4, std::ios::cur);
                if (!input_) return false;
            }
        }
        return valid_;
    }

private:
    std::ifstream input_;
    uint64_t remaining_ = 0;
    bool valid_ = false;
    bool seen_idat_ = false;
    bool inside_idat_ = false;
    bool finished_ = false;
};

uint32_t pngChannelCount(uint8_t color_type) {
    switch (color_type) {
        case 0: return 1;
        case 2: return 3;
        case 3: return 1;
        case 4: return 2;
        case 6: return 4;
        default: return 0;
    }
}

bool validatePngMetadata(const PngMetadata& metadata, std::string* error) {
    const uint8_t depth = metadata.bit_depth;
    bool valid_depth = false;
    if (metadata.color_type == 0) valid_depth = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    else if (metadata.color_type == 3) valid_depth = depth == 1 || depth == 2 || depth == 4 || depth == 8;
    else valid_depth = depth == 8 || depth == 16;
    const size_t palette_entries = metadata.palette.size() / 3;
    if (metadata.width == 0 || metadata.height == 0 || metadata.interlace > 1 ||
        pngChannelCount(metadata.color_type) == 0 || !valid_depth ||
        (metadata.color_type == 3 &&
         (metadata.palette.empty() || metadata.palette.size() % 3 != 0 ||
          palette_entries > (size_t(1) << depth) ||
          metadata.transparency.size() > palette_entries)) ||
        (metadata.color_type == 0 && !metadata.transparency.empty() && metadata.transparency.size() != 2) ||
        (metadata.color_type == 2 && !metadata.transparency.empty() && metadata.transparency.size() != 6) ||
        ((metadata.color_type == 4 || metadata.color_type == 6) && !metadata.transparency.empty())) {
        if (error) *error = "unsupported or malformed PNG colour format";
        return false;
    }
    return true;
}

uint16_t packedSample(const uint8_t* row, uint64_t sample_index, uint8_t depth) {
    if (depth == 16) return be16(row + sample_index * 2);
    if (depth == 8) return row[sample_index];
    const uint64_t bit_offset = sample_index * depth;
    const uint8_t shift = static_cast<uint8_t>(8 - depth - (bit_offset & 7));
    return static_cast<uint16_t>((row[bit_offset >> 3] >> shift) & ((1U << depth) - 1U));
}

uint8_t sampleToByte(uint16_t value, uint8_t depth) {
    if (depth == 8) return static_cast<uint8_t>(value);
    if (depth == 16) return static_cast<uint8_t>((uint32_t(value) * 255U + 32767U) / 65535U);
    const uint32_t maximum = (1U << depth) - 1U;
    return static_cast<uint8_t>((uint32_t(value) * 255U + maximum / 2U) / maximum);
}

bool decodePngPixel(const PngMetadata& metadata, const uint8_t* row,
                    uint32_t pixel_index, Rgba* output, std::string* error) {
    const uint64_t sample = uint64_t(pixel_index) * pngChannelCount(metadata.color_type);
    const uint8_t depth = metadata.bit_depth;
    if (metadata.color_type == 0) {
        const uint16_t gray = packedSample(row, sample, depth);
        const uint8_t value = sampleToByte(gray, depth);
        const bool transparent = metadata.transparency.size() == 2 &&
                                 gray == be16(metadata.transparency.data());
        *output = {value, value, value, transparent ? uint8_t(0) : uint8_t(255)};
        return true;
    }
    if (metadata.color_type == 2) {
        const uint16_t red = packedSample(row, sample, depth);
        const uint16_t green = packedSample(row, sample + 1, depth);
        const uint16_t blue = packedSample(row, sample + 2, depth);
        const bool transparent = metadata.transparency.size() == 6 &&
            red == be16(metadata.transparency.data()) &&
            green == be16(metadata.transparency.data() + 2) &&
            blue == be16(metadata.transparency.data() + 4);
        *output = {sampleToByte(red, depth), sampleToByte(green, depth),
                   sampleToByte(blue, depth), transparent ? uint8_t(0) : uint8_t(255)};
        return true;
    }
    if (metadata.color_type == 3) {
        const size_t index = packedSample(row, sample, depth);
        if (index >= metadata.palette.size() / 3) {
            if (error) *error = "PNG palette index is out of range";
            return false;
        }
        *output = {metadata.palette[index * 3], metadata.palette[index * 3 + 1],
                   metadata.palette[index * 3 + 2],
                   index < metadata.transparency.size() ? metadata.transparency[index] : uint8_t(255)};
        return true;
    }
    if (metadata.color_type == 4) {
        const uint16_t gray = packedSample(row, sample, depth);
        const uint16_t alpha = packedSample(row, sample + 1, depth);
        const uint8_t value = sampleToByte(gray, depth);
        *output = {value, value, value, sampleToByte(alpha, depth)};
        return true;
    }
    *output = {sampleToByte(packedSample(row, sample, depth), depth),
               sampleToByte(packedSample(row, sample + 1, depth), depth),
               sampleToByte(packedSample(row, sample + 2, depth), depth),
               sampleToByte(packedSample(row, sample + 3, depth), depth)};
    return true;
}

bool packedRowSize(uint32_t width, const PngMetadata& metadata, size_t* size,
                   size_t* filter_bytes_per_pixel, std::string* error) {
    const uint64_t bits_per_pixel = uint64_t(pngChannelCount(metadata.color_type)) * metadata.bit_depth;
    uint64_t bits = 0;
    if (!checkedMultiply(width, bits_per_pixel, &bits) || bits > (SIZE_MAX - 7) ||
        (bits + 7) / 8 > kMaxPackedPngRowBytes) {
        if (error) *error = "PNG scanline is too wide to decode safely";
        return false;
    }
    *size = static_cast<size_t>((bits + 7) / 8);
    *filter_bytes_per_pixel = static_cast<size_t>(std::max<uint64_t>(1, (bits_per_pixel + 7) / 8));
    return true;
}

bool unfilterPngRow(uint8_t filter, size_t bytes_per_pixel,
                    const std::vector<uint8_t>& previous,
                    std::vector<uint8_t>* current, std::string* error) {
    for (size_t i = 0; i < current->size(); ++i) {
        const uint8_t left = i >= bytes_per_pixel ? (*current)[i - bytes_per_pixel] : 0;
        const uint8_t above = previous.empty() ? 0 : previous[i];
        const uint8_t upper_left = previous.empty() || i < bytes_per_pixel
            ? 0 : previous[i - bytes_per_pixel];
        if (filter == 1) (*current)[i] = static_cast<uint8_t>((*current)[i] + left);
        else if (filter == 2) (*current)[i] = static_cast<uint8_t>((*current)[i] + above);
        else if (filter == 3) (*current)[i] = static_cast<uint8_t>((*current)[i] + (uint16_t(left) + above) / 2);
        else if (filter == 4) {
            const int prediction = int(left) + above - upper_left;
            const int left_distance = std::abs(prediction - int(left));
            const int above_distance = std::abs(prediction - int(above));
            const int diagonal_distance = std::abs(prediction - int(upper_left));
            const uint8_t predictor = left_distance <= above_distance && left_distance <= diagonal_distance
                ? left : (above_distance <= diagonal_distance ? above : upper_left);
            (*current)[i] = static_cast<uint8_t>((*current)[i] + predictor);
        } else if (filter != 0) {
            if (error) *error = "unsupported PNG row filter";
            return false;
        }
    }
    return true;
}

class PngInflater {
public:
    PngInflater(const std::string& path, const std::function<bool()>& cancel,
                std::string* error)
        : reader_(path), cancel_(cancel), error_(error) {
        valid_ = inflateInit(&stream_) == Z_OK;
        if (!valid_ && error_) *error_ = "cannot initialize PNG inflater";
    }
    ~PngInflater() { if (valid_) inflateEnd(&stream_); }

    bool valid() const { return valid_; }

    bool readExact(uint8_t* output, size_t length) {
        size_t produced = 0;
        while (produced < length) {
            if (cancellationRequested(cancel_, error_)) return false;
            if (finished_) return false;
            if (stream_.avail_in == 0 && !supplyInput()) return false;
            const size_t capacity = std::min<size_t>(length - produced, buffer_.size());
            stream_.next_out = output + produced;
            stream_.avail_out = static_cast<uInt>(capacity);
            const uInt input_before = stream_.avail_in;
            const int status = inflate(&stream_, Z_NO_FLUSH);
            const size_t made = capacity - stream_.avail_out;
            const size_t consumed = input_before - stream_.avail_in;
            produced += made;
            if (status == Z_STREAM_END) finished_ = true;
            else if (status != Z_OK) return false;
            if ((made == 0 && consumed == 0) || (finished_ && produced < length)) return false;
        }
        return true;
    }

    bool finish() {
        uint8_t extra = 0;
        while (!finished_) {
            if (cancellationRequested(cancel_, error_)) return false;
            if (stream_.avail_in == 0 && !supplyInput()) return false;
            stream_.next_out = &extra;
            stream_.avail_out = 1;
            const uInt input_before = stream_.avail_in;
            const int status = inflate(&stream_, Z_NO_FLUSH);
            if (stream_.avail_out == 0) return false;  // Extra decompressed byte.
            if (status == Z_STREAM_END) finished_ = true;
            else if (status != Z_OK || input_before == stream_.avail_in) return false;
        }
        if (stream_.avail_in != 0) return false;
        size_t trailing_size = 0;
        if (!reader_.read(&extra, 1, &trailing_size) || trailing_size != 0) return false;
        return true;
    }

private:
    bool supplyInput() {
        size_t received = 0;
        if (!reader_.read(buffer_.data(), buffer_.size(), &received) || received == 0) return false;
        stream_.next_in = buffer_.data();
        stream_.avail_in = static_cast<uInt>(received);
        return true;
    }

    z_stream stream_{};
    PngIdatReader reader_;
    std::array<uint8_t, 64 * 1024> buffer_{};
    const std::function<bool()>& cancel_;
    std::string* error_ = nullptr;
    bool valid_ = false;
    bool finished_ = false;
};

struct PixelAccum {
    double r = 0;
    double g = 0;
    double b = 0;
    double alpha = 0;
    double weight = 0;
};

struct ErrorPixel {
    float r = 0;
    float g = 0;
    float b = 0;
};
static_assert(sizeof(ErrorPixel) == 12, "unexpected error-row layout");
static_assert(sizeof(Rgba) == 4, "unexpected RGBA layout");

bool compactScalerFitsBudget(const PngMetadata& metadata, uint32_t target_width,
                             size_t budget, std::string* error) {
    if (metadata.interlace != 0 || budget == 0) return false;
    size_t packed_row_size = 0;
    size_t filter_bytes = 0;
    if (!packedRowSize(metadata.width, metadata, &packed_row_size,
                       &filter_bytes, error)) return false;
    (void)filter_bytes;
    uint64_t source_rows = 0;
    uint64_t target_rows = 0;
    uint64_t working_set = 0;
    constexpr uint64_t kTargetBytesPerColumn =
        2U * sizeof(PixelAccum) + 7U * sizeof(float);
    if (!checkedMultiply(packed_row_size, 2U, &source_rows) ||
        !checkedMultiply(target_width, kTargetBytesPerColumn, &target_rows) ||
        !checkedAdd(source_rows, target_rows, &working_set)) return false;
    // Leave room for zlib, palettes, ChunkSpoolWriter buffers and allocator
    // alignment instead of consuming the caller's entire memory allowance.
    constexpr uint64_t kFixedHeadroom = 2U * 1024U * 1024U;
    return working_set <= budget && kFixedHeadroom <= budget - working_set;
}

void addError(ErrorPixel* destination, float r, float g, float b) {
    destination->r = clampError(destination->r + r);
    destination->g = clampError(destination->g + g);
    destination->b = clampError(destination->b + b);
}

int createDirectory(const char* path) {
#if defined(_WIN32)
    return ::mkdir(path);
#else
    return ::mkdir(path, 0700);
#endif
}

bool ensurePixelDirectory(const std::string& directory) {
    if (directory.empty()) return false;
    std::string partial;
    for (size_t index = 0; index <= directory.size(); ++index) {
        if (index != directory.size() && directory[index] != '/' && directory[index] != '\\') {
            partial += directory[index];
            continue;
        }
        if (!partial.empty() && partial != "." &&
            createDirectory(partial.c_str()) != 0 && errno != EEXIST) return false;
        if (index != directory.size()) {
            if (partial.empty() && directory[index] == '/') partial = "/";
            else if (!partial.empty() && partial.back() != '/') partial += '/';
        }
    }
    return true;
}

class PixelTemporaryFiles {
public:
    explicit PixelTemporaryFiles(const std::string& directory)
        : rgba(directory + "/.pixelart_rgba.tmp"),
          errors_a(directory + "/.pixelart_errors_a.tmp"),
          errors_b(directory + "/.pixelart_errors_b.tmp") {
        discard();
    }
    ~PixelTemporaryFiles() { discard(); }
    void discard() const {
        std::remove(rgba.c_str());
        std::remove(errors_a.c_str());
        std::remove(errors_b.c_str());
    }
    std::string rgba;
    std::string errors_a;
    std::string errors_b;
};

bool alignMapMinimum(int32_t coordinate, int32_t* output) {
    if (!output) return false;
    constexpr int64_t kMapSpan = 128;
    const int64_t shifted = static_cast<int64_t>(coordinate) + kMapSpan / 2;
    const int64_t quotient = shifted >= 0
        ? shifted / kMapSpan
        : -((-shifted + kMapSpan - 1) / kMapSpan);
    const int64_t minimum = quotient * kMapSpan - kMapSpan / 2;
    if (minimum < std::numeric_limits<int32_t>::min() ||
        minimum > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    *output = static_cast<int32_t>(minimum);
    return true;
}

bool scaledGeometry(const PngMetadata& metadata, const PixelArtParseOptions& options,
                    uint32_t* target_width, uint32_t* target_height,
                    std::string* error) {
    *target_width = static_cast<uint32_t>(options.target_width);
    const uint64_t scaled_height_numerator =
        static_cast<uint64_t>(metadata.height) * *target_width;
    const uint64_t height = std::max<uint64_t>(
        1, (scaled_height_numerator + metadata.width / 2U) / metadata.width);
    if (height > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        if (error) *error = "scaled pixel-art height exceeds coordinate range";
        return false;
    }
    *target_height = static_cast<uint32_t>(height);
    const uint64_t maximum_pixels =
        static_cast<uint64_t>(*target_width) * *target_height;
    if (options.maximum_output_blocks != 0 &&
        maximum_pixels > options.maximum_output_blocks) {
        if (error) {
            *error = "scaled pixel-art block count exceeds configured limit of " +
                std::to_string(options.maximum_output_blocks);
        }
        return false;
    }
    if (static_cast<int64_t>(options.base_x) + *target_width - 1 > std::numeric_limits<int32_t>::max() ||
        static_cast<int64_t>(options.base_z) + *target_height - 1 > std::numeric_limits<int32_t>::max()) {
        if (error) *error = "scaled pixel-art coordinates exceed the supported world range";
        return false;
    }
    return true;
}

bool appendPaletteBlock(uint32_t x, uint32_t y, const PaletteBlock& block,
                        const PixelArtParseOptions& options, ChunkSpoolWriter* writer,
                        SchematicParseResult* result, std::string* error) {
    if (options.maximum_output_blocks != 0 &&
        result->imported_block_count >= options.maximum_output_blocks) {
        if (error) {
            *error = "pixel-art block count exceeds configured limit of " +
                std::to_string(options.maximum_output_blocks);
        }
        return false;
    }
    ParsedBlock parsed{
        static_cast<int32_t>(static_cast<int64_t>(options.base_x) + x),
        options.base_y,
        static_cast<int32_t>(static_cast<int64_t>(options.base_z) + y),
        {block.name, block.aux, block.phase, block.can_fill, block.single_layer_only}};
    if (options.block_sink) {
        if (!options.block_sink(parsed, error)) return false;
    } else if (!writer || !writer->append(parsed, error)) {
        if (error && error->empty()) *error = "pixel-art block output is unavailable";
        return false;
    }
    ++result->imported_block_count;
    return true;
}

// Fast path for ordinary non-interlaced images.  target_width is capped by
// kTargetStripeWidth before this function is selected, so every allocation is
// bounded even though the decoder remains a single streaming pass.
bool streamCompactPngToSpool(const std::string& path, const PngMetadata& metadata,
                             uint32_t target_width, uint32_t target_height,
                             const PixelArtParseOptions& options,
                             ChunkSpoolWriter* writer, SchematicParseResult* result,
                             std::string* error) {
    size_t row_size = 0, filter_bpp = 0;
    if (!packedRowSize(metadata.width, metadata, &row_size, &filter_bpp, error)) return false;
    std::vector<uint8_t> current(row_size), previous(row_size);
    std::vector<PixelAccum> horizontal(target_width), vertical(target_width);
    std::vector<float> row_r(target_width), row_g(target_width), row_b(target_width), row_alpha(target_width);
    std::vector<float> error_r(target_width), error_g(target_width), error_b(target_width);
    const std::vector<uint8_t> empty_previous;
    PngInflater inflater(path, options.cancellation_requested, error);
    if (!inflater.valid()) return false;

    auto emitRow = [&](uint32_t y) -> bool {
        for (uint32_t x = 0; x < target_width; ++x) {
            if ((x & 0xFF) == 0 && cancellationRequested(options.cancellation_requested, error)) return false;
            const PixelAccum& value = vertical[x];
            const float alpha = value.weight > 0 ? clampUnit(float(value.alpha / value.weight)) : 0.0f;
            row_alpha[x] = alpha;
            row_r[x] = value.alpha > 1e-12 ? float(value.r / value.alpha) + error_r[x] : 0.0f;
            row_g[x] = value.alpha > 1e-12 ? float(value.g / value.alpha) + error_g[x] : 0.0f;
            row_b[x] = value.alpha > 1e-12 ? float(value.b / value.alpha) + error_b[x] : 0.0f;
            error_r[x] = error_g[x] = error_b[x] = 0.0f;
        }
        for (uint32_t x = 0; x < target_width; ++x) {
            if (row_alpha[x] < kVisibleAlphaThreshold) {
                ++result->skipped_block_count;
                continue;
            }
            const float adjusted_r = clamp(row_r[x]);
            const float adjusted_g = clamp(row_g[x]);
            const float adjusted_b = clamp(row_b[x]);
            const PaletteBlock& block = closestBlock(adjusted_r, adjusted_g, adjusted_b);
            const float er = adjusted_r - block.r;
            const float eg = adjusted_g - block.g;
            const float eb = adjusted_b - block.b;
            if (x + 1 < target_width) {
                row_r[x + 1] += er * 7.0f / 16.0f;
                row_g[x + 1] += eg * 7.0f / 16.0f;
                row_b[x + 1] += eb * 7.0f / 16.0f;
            }
            if (x > 0) {
                error_r[x - 1] = clampError(error_r[x - 1] + er * 3.0f / 16.0f);
                error_g[x - 1] = clampError(error_g[x - 1] + eg * 3.0f / 16.0f);
                error_b[x - 1] = clampError(error_b[x - 1] + eb * 3.0f / 16.0f);
            }
            error_r[x] = clampError(error_r[x] + er * 5.0f / 16.0f);
            error_g[x] = clampError(error_g[x] + eg * 5.0f / 16.0f);
            error_b[x] = clampError(error_b[x] + eb * 5.0f / 16.0f);
            if (x + 1 < target_width) {
                error_r[x + 1] = clampError(error_r[x + 1] + er * 1.0f / 16.0f);
                error_g[x + 1] = clampError(error_g[x + 1] + eg * 1.0f / 16.0f);
                error_b[x + 1] = clampError(error_b[x + 1] + eb * 1.0f / 16.0f);
            }
            if (!appendPaletteBlock(x, y, block, options, writer, result, error)) return false;
        }
        if ((y & 0x0FU) == 0 || y + 1U == target_height) {
            reportProgress(options, SchematicParseStage::RoutingBlocks,
                           static_cast<uint64_t>(y) + 1U, target_height);
        }
        return true;
    };

    uint32_t active_target_row = 0;
    for (uint32_t sy = 0; sy < metadata.height; ++sy) {
        if (cancellationRequested(options.cancellation_requested, error)) return false;
        uint8_t filter = 0;
        if (!inflater.readExact(&filter, 1) || !inflater.readExact(current.data(), current.size())) {
            if (error && error->empty()) *error = "cannot decompress PNG scanline";
            return false;
        }
        if (!unfilterPngRow(filter, filter_bpp, sy == 0 ? empty_previous : previous,
                            &current, error)) return false;
        std::fill(horizontal.begin(), horizontal.end(), PixelAccum{});
        for (uint32_t sx = 0; sx < metadata.width; ++sx) {
            if ((sx & 0x3FF) == 0 && cancellationRequested(options.cancellation_requested, error)) return false;
            Rgba pixel{};
            if (!decodePngPixel(metadata, current.data(), sx, &pixel, error)) return false;
            const double alpha = double(pixel.a) / 255.0;
            const MappedInterval horizontal_span =
                mappedInterval(sx, metadata.width, target_width);
            for (uint32_t tx = horizontal_span.first; tx <= horizontal_span.last; ++tx) {
                const double weight = mappedOverlap(horizontal_span, tx);
                if (weight <= 0) continue;
                horizontal[tx].r += pixel.r * alpha * weight;
                horizontal[tx].g += pixel.g * alpha * weight;
                horizontal[tx].b += pixel.b * alpha * weight;
                horizontal[tx].alpha += alpha * weight;
                horizontal[tx].weight += weight;
            }
        }
        const MappedInterval vertical_span =
            mappedInterval(sy, metadata.height, target_height);
        for (uint32_t ty = vertical_span.first; ty <= vertical_span.last; ++ty) {
            if (ty < active_target_row) continue;
            if (ty != active_target_row) {
                if (error) *error = "PNG resize row scheduling failed";
                return false;
            }
            const double weight_y = mappedOverlap(vertical_span, ty);
            for (uint32_t x = 0; x < target_width; ++x) {
                vertical[x].r += horizontal[x].r * weight_y;
                vertical[x].g += horizontal[x].g * weight_y;
                vertical[x].b += horizontal[x].b * weight_y;
                vertical[x].alpha += horizontal[x].alpha * weight_y;
                vertical[x].weight += horizontal[x].weight * weight_y;
            }
            if (vertical_span.end >=
                (static_cast<uint64_t>(ty) + 1U) * vertical_span.denominator) {
                if (!emitRow(active_target_row)) return false;
                ++active_target_row;
                std::fill(vertical.begin(), vertical.end(), PixelAccum{});
            }
        }
        std::swap(current, previous);
    }
    if (!inflater.finish()) {
        if (error && error->empty()) *error = "PNG decompressed data length mismatch";
        return false;
    }
    while (active_target_row < target_height) {
        if (!emitRow(active_target_row++)) return false;
        std::fill(vertical.begin(), vertical.end(), PixelAccum{});
    }
    result->source_voxel_count = uint64_t(target_width) * target_height;
    return true;
}

struct Adam7Pass {
    uint32_t start_x;
    uint32_t start_y;
    uint32_t step_x;
    uint32_t step_y;
};

uint32_t passExtent(uint32_t full, uint32_t start, uint32_t step) {
    return full <= start ? 0 : 1 + (full - 1 - start) / step;
}

bool writeDecodedPassRow(std::fstream* rgba_file, const PngMetadata& metadata,
                         const std::vector<uint8_t>& row, const Adam7Pass& pass,
                         uint32_t pass_width, uint32_t global_y,
                         const std::function<bool()>& cancel, std::string* error) {
    constexpr uint32_t kRgbaTilePixels = 4096;
    if (pass.step_x == 1) {
        std::vector<Rgba> converted(std::min<uint32_t>(pass_width, kRgbaTilePixels));
        for (uint32_t begin = 0; begin < pass_width; begin += kRgbaTilePixels) {
            if (cancellationRequested(cancel, error)) return false;
            const uint32_t count = std::min<uint32_t>(kRgbaTilePixels, pass_width - begin);
            for (uint32_t i = 0; i < count; ++i) {
                if (!decodePngPixel(metadata, row.data(), begin + i, &converted[i], error)) return false;
            }
            const uint64_t offset = (uint64_t(global_y) * metadata.width + pass.start_x + begin) * sizeof(Rgba);
            rgba_file->clear();
            rgba_file->seekp(static_cast<std::streamoff>(offset));
            rgba_file->write(reinterpret_cast<const char*>(converted.data()),
                             static_cast<std::streamsize>(count * sizeof(Rgba)));
            if (!*rgba_file) { if (error) *error = "cannot write pixel-art work file"; return false; }
        }
        return true;
    }

    std::vector<Rgba> tile(kRgbaTilePixels);
    for (uint32_t tile_begin = 0; tile_begin < metadata.width; tile_begin += kRgbaTilePixels) {
        const uint32_t tile_count = std::min<uint32_t>(kRgbaTilePixels, metadata.width - tile_begin);
        const uint32_t first = tile_begin <= pass.start_x ? 0 :
            (tile_begin - pass.start_x + pass.step_x - 1) / pass.step_x;
        if (first >= pass_width || pass.start_x + first * pass.step_x >= tile_begin + tile_count) continue;
        if (cancellationRequested(cancel, error)) return false;
        const uint64_t offset = (uint64_t(global_y) * metadata.width + tile_begin) * sizeof(Rgba);
        rgba_file->clear();
        rgba_file->seekg(static_cast<std::streamoff>(offset));
        rgba_file->read(reinterpret_cast<char*>(tile.data()),
                        static_cast<std::streamsize>(tile_count * sizeof(Rgba)));
        if (rgba_file->gcount() != static_cast<std::streamsize>(tile_count * sizeof(Rgba))) {
            if (error) *error = "cannot read pixel-art work file";
            return false;
        }
        for (uint32_t px = first; px < pass_width; ++px) {
            const uint32_t global_x = pass.start_x + px * pass.step_x;
            if (global_x >= tile_begin + tile_count) break;
            if (!decodePngPixel(metadata, row.data(), px, &tile[global_x - tile_begin], error)) return false;
        }
        rgba_file->clear();
        rgba_file->seekp(static_cast<std::streamoff>(offset));
        rgba_file->write(reinterpret_cast<const char*>(tile.data()),
                         static_cast<std::streamsize>(tile_count * sizeof(Rgba)));
        if (!*rgba_file) { if (error) *error = "cannot write pixel-art work file"; return false; }
    }
    return true;
}

bool decodePngToRgbaFile(const std::string& source_path, const std::string& rgba_path,
                         const PngMetadata& metadata,
                         const std::function<bool()>& cancel, std::string* error) {
    uint64_t pixel_count = 0, byte_count = 0;
    if (!checkedMultiply(metadata.width, metadata.height, &pixel_count) ||
        !checkedMultiply(pixel_count, sizeof(Rgba), &byte_count) || byte_count == 0 ||
        byte_count > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        if (error) *error = "PNG dimensions exceed work-file range";
        return false;
    }
    std::fstream output(rgba_path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!output) { if (error) *error = "cannot create pixel-art work file"; return false; }
    output.seekp(static_cast<std::streamoff>(byte_count - 1));
    output.put('\0');
    output.flush();
    if (!output) { if (error) *error = "cannot size pixel-art work file"; return false; }

    static constexpr std::array<Adam7Pass, 7> kAdam7{{
        {0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
        {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}}};
    const Adam7Pass plain{0, 0, 1, 1};
    PngInflater inflater(source_path, cancel, error);
    if (!inflater.valid()) return false;
    const size_t pass_count = metadata.interlace == 0 ? 1 : kAdam7.size();
    for (size_t pass_index = 0; pass_index < pass_count; ++pass_index) {
        const Adam7Pass& pass = metadata.interlace == 0 ? plain : kAdam7[pass_index];
        const uint32_t width = passExtent(metadata.width, pass.start_x, pass.step_x);
        const uint32_t height = passExtent(metadata.height, pass.start_y, pass.step_y);
        if (width == 0 || height == 0) continue;
        size_t row_size = 0, filter_bpp = 0;
        if (!packedRowSize(width, metadata, &row_size, &filter_bpp, error)) return false;
        std::vector<uint8_t> current(row_size), previous(row_size);
        const std::vector<uint8_t> empty_previous;
        for (uint32_t row_index = 0; row_index < height; ++row_index) {
            if (cancellationRequested(cancel, error)) return false;
            uint8_t filter = 0;
            if (!inflater.readExact(&filter, 1) || !inflater.readExact(current.data(), current.size())) {
                if (error && error->empty()) *error = "cannot decompress PNG pass row";
                return false;
            }
            if (!unfilterPngRow(filter, filter_bpp,
                                row_index == 0 ? empty_previous : previous,
                                &current, error)) return false;
            const uint32_t global_y = pass.start_y + row_index * pass.step_y;
            if (!writeDecodedPassRow(&output, metadata, current, pass, width, global_y, cancel, error)) return false;
            std::swap(current, previous);
        }
    }
    if (!inflater.finish()) {
        if (error && error->empty()) *error = "PNG decompressed data length mismatch";
        return false;
    }
    output.flush();
    output.close();
    if (!output) { if (error) *error = "cannot finalize pixel-art work file"; return false; }
    return true;
}

bool scaleRgbaFileToSpool(const std::string& rgba_path,
                          const std::string& errors_a_path,
                          const std::string& errors_b_path,
                          const PngMetadata& metadata,
                          uint32_t target_width, uint32_t target_height,
                          const PixelArtParseOptions& options,
                          ChunkSpoolWriter* writer, SchematicParseResult* result,
                          std::string* error) {
    std::ifstream rgba_file(rgba_path, std::ios::binary);
    if (!rgba_file) { if (error) *error = "cannot open pixel-art work file"; return false; }
    std::string current_error_path;
    std::string next_error_path = errors_a_path;
    std::vector<Rgba> source_tile(kTargetStripeWidth);
    std::vector<Rgba> cached_source_rows;
    uint32_t cached_source_y_begin = 0;
    uint32_t cached_source_y_end = 0;
    bool has_cached_source_rows = false;
    const uint64_t source_row_bytes =
        static_cast<uint64_t>(metadata.width) * sizeof(Rgba);
    const uint64_t source_cache_budget = std::min<uint64_t>(
        32U * 1024U * 1024U, options.streaming_memory_budget_bytes / 2U);

    for (uint32_t y = 0; y < target_height; ++y) {
        if (cancellationRequested(options.cancellation_requested, error)) return false;
        std::ifstream current_errors;
        if (!current_error_path.empty()) {
            current_errors.open(current_error_path, std::ios::binary);
            if (!current_errors) { if (error) *error = "cannot open pixel-art error row"; return false; }
        }
        std::ofstream next_errors(next_error_path, std::ios::binary | std::ios::trunc);
        if (!next_errors) { if (error) *error = "cannot create pixel-art error row"; return false; }
        ErrorPixel pending_left{};
        ErrorPixel seed_current{};
        ErrorPixel right_error{};

        const MappedInterval vertical_span =
            mappedInterval(y, target_height, metadata.height);
        const uint32_t source_y_begin = vertical_span.first;
        const uint32_t source_y_end = vertical_span.last;
        const uint64_t source_row_count =
            static_cast<uint64_t>(source_y_end) - source_y_begin + 1U;
        uint64_t source_window_bytes = 0;
        const bool cache_source_window = source_cache_budget != 0 &&
            checkedMultiply(source_row_count, source_row_bytes,
                            &source_window_bytes) &&
            source_window_bytes <= source_cache_budget &&
            source_row_count <= std::numeric_limits<size_t>::max() / metadata.width;
        if (cache_source_window &&
            (!has_cached_source_rows || cached_source_y_begin != source_y_begin ||
             cached_source_y_end != source_y_end)) {
            cached_source_rows.resize(
                static_cast<size_t>(source_row_count) * metadata.width);
            const uint64_t offset =
                static_cast<uint64_t>(source_y_begin) * source_row_bytes;
            rgba_file.clear();
            rgba_file.seekg(static_cast<std::streamoff>(offset));
            rgba_file.read(reinterpret_cast<char*>(cached_source_rows.data()),
                           static_cast<std::streamsize>(source_window_bytes));
            if (rgba_file.gcount() != static_cast<std::streamsize>(source_window_bytes)) {
                if (error) *error = "cannot read pixel-art work file";
                return false;
            }
            cached_source_y_begin = source_y_begin;
            cached_source_y_end = source_y_end;
            has_cached_source_rows = true;
        } else if (!cache_source_window) {
            cached_source_rows.clear();
            has_cached_source_rows = false;
        }

        for (uint32_t x_begin = 0; x_begin < target_width; x_begin += kTargetStripeWidth) {
            const uint32_t count = std::min<uint32_t>(kTargetStripeWidth, target_width - x_begin);
            const uint32_t x_end = x_begin + count;
            std::vector<PixelAccum> accum(count);
            const uint32_t source_x_begin =
                mappedFloor(x_begin, target_width, metadata.width);
            const uint32_t source_x_end = std::min<uint32_t>(
                metadata.width - 1U,
                mappedCeil(x_end, target_width, metadata.width) - 1U);

            for (uint32_t source_y = source_y_begin; source_y <= source_y_end; ++source_y) {
                const double weight_y = mappedOverlap(vertical_span, source_y);
                for (uint32_t tile_begin = source_x_begin; tile_begin <= source_x_end;) {
                    const uint32_t tile_count = std::min<uint32_t>(kTargetStripeWidth,
                        source_x_end - tile_begin + 1);
                    const Rgba* tile_pixels = nullptr;
                    if (has_cached_source_rows) {
                        const size_t row_offset =
                            static_cast<size_t>(source_y - cached_source_y_begin) *
                            metadata.width;
                        tile_pixels = cached_source_rows.data() + row_offset + tile_begin;
                    } else {
                        const uint64_t offset =
                            (uint64_t(source_y) * metadata.width + tile_begin) *
                            sizeof(Rgba);
                        rgba_file.clear();
                        rgba_file.seekg(static_cast<std::streamoff>(offset));
                        rgba_file.read(reinterpret_cast<char*>(source_tile.data()),
                                       static_cast<std::streamsize>(tile_count * sizeof(Rgba)));
                        if (rgba_file.gcount() !=
                            static_cast<std::streamsize>(tile_count * sizeof(Rgba))) {
                            if (error) *error = "cannot read pixel-art work file";
                            return false;
                        }
                        tile_pixels = source_tile.data();
                    }
                    for (uint32_t i = 0; i < tile_count; ++i) {
                        const uint32_t source_x = tile_begin + i;
                        const Rgba& pixel = tile_pixels[i];
                        const double alpha = double(pixel.a) / 255.0;
                        const MappedInterval horizontal_span =
                            mappedInterval(source_x, metadata.width, target_width);
                        const uint32_t target_begin = std::max<uint32_t>(x_begin,
                            horizontal_span.first);
                        const uint32_t target_end = std::min<uint32_t>(x_end - 1,
                            horizontal_span.last);
                        if (target_begin > target_end) continue;
                        for (uint32_t target_x = target_begin; target_x <= target_end; ++target_x) {
                            const double weight_x = mappedOverlap(horizontal_span, target_x);
                            const double weight = weight_x * weight_y;
                            PixelAccum& value = accum[target_x - x_begin];
                            value.r += pixel.r * alpha * weight;
                            value.g += pixel.g * alpha * weight;
                            value.b += pixel.b * alpha * weight;
                            value.alpha += alpha * weight;
                            value.weight += weight;
                        }
                    }
                    tile_begin += tile_count;
                }
            }

            std::vector<ErrorPixel> current(count);
            if (current_errors.is_open()) {
                current_errors.read(reinterpret_cast<char*>(current.data()),
                                    static_cast<std::streamsize>(count * sizeof(ErrorPixel)));
                if (current_errors.gcount() != static_cast<std::streamsize>(count * sizeof(ErrorPixel))) {
                    if (error) *error = "cannot read pixel-art error row";
                    return false;
                }
            }
            std::vector<ErrorPixel> next(count + 1);
            next[0] = seed_current;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t x = x_begin + i;
                const PixelAccum& value = accum[i];
                const float alpha = value.weight > 0 ? clampUnit(float(value.alpha / value.weight)) : 0.0f;
                if (alpha < kVisibleAlphaThreshold || value.alpha <= 1e-12) {
                    ++result->skipped_block_count;
                    right_error = {};
                    continue;
                }
                const float adjusted_r = clamp(float(value.r / value.alpha) + current[i].r + right_error.r);
                const float adjusted_g = clamp(float(value.g / value.alpha) + current[i].g + right_error.g);
                const float adjusted_b = clamp(float(value.b / value.alpha) + current[i].b + right_error.b);
                const PaletteBlock& block = closestBlock(adjusted_r, adjusted_g, adjusted_b);
                const float er = adjusted_r - block.r;
                const float eg = adjusted_g - block.g;
                const float eb = adjusted_b - block.b;
                right_error = {er * 7.0f / 16.0f, eg * 7.0f / 16.0f, eb * 7.0f / 16.0f};
                if (x > 0) {
                    if (i == 0) addError(&pending_left, er * 3.0f / 16.0f,
                                        eg * 3.0f / 16.0f, eb * 3.0f / 16.0f);
                    else addError(&next[i - 1], er * 3.0f / 16.0f,
                                  eg * 3.0f / 16.0f, eb * 3.0f / 16.0f);
                }
                addError(&next[i], er * 5.0f / 16.0f,
                         eg * 5.0f / 16.0f, eb * 5.0f / 16.0f);
                if (x + 1 < target_width) {
                    addError(&next[i + 1], er * 1.0f / 16.0f,
                             eg * 1.0f / 16.0f, eb * 1.0f / 16.0f);
                }
                if (!appendPaletteBlock(x, y, block, options, writer, result, error)) return false;
            }
            if (x_begin > 0) {
                next_errors.write(reinterpret_cast<const char*>(&pending_left), sizeof(pending_left));
            }
            if (count > 1) {
                next_errors.write(reinterpret_cast<const char*>(next.data()),
                                  static_cast<std::streamsize>((count - 1) * sizeof(ErrorPixel)));
            }
            if (!next_errors) { if (error) *error = "cannot write pixel-art error row"; return false; }
            pending_left = next[count - 1];
            seed_current = next[count];
        }
        next_errors.write(reinterpret_cast<const char*>(&pending_left), sizeof(pending_left));
        next_errors.flush();
        next_errors.close();
        if (!next_errors) { if (error) *error = "cannot finalize pixel-art error row"; return false; }
        current_errors.close();
        current_error_path = next_error_path;
        next_error_path = current_error_path == errors_a_path ? errors_b_path : errors_a_path;
        if ((y & 0x0FU) == 0 || y + 1U == target_height) {
            reportProgress(options, SchematicParseStage::RoutingBlocks,
                           static_cast<uint64_t>(y) + 1U, target_height);
        }
    }
    result->source_voxel_count = uint64_t(target_width) * target_height;
    return true;
}

bool streamPngScaledToSpool(const std::string& path, const PixelArtParseOptions& options,
                            ChunkSpoolWriter* writer, SchematicParseResult* result,
                            std::string* error) {
    if (cancellationRequested(options.cancellation_requested, error)) return false;
    PngMetadata metadata;
    if (!inspectPng(path, options.cancellation_requested, &metadata, error) ||
        !validatePngMetadata(metadata, error)) return false;
    uint32_t target_width = 0, target_height = 0;
    if (!scaledGeometry(metadata, options, &target_width, &target_height, error)) return false;
    reportProgress(options, SchematicParseStage::RoutingBlocks, 0, target_height);
    const BlockBounds full_wall{
        options.base_x, options.base_y, options.base_z,
        static_cast<int32_t>(static_cast<int64_t>(options.base_x) + target_width - 1),
        options.base_y,
        static_cast<int32_t>(static_cast<int64_t>(options.base_z) + target_height - 1)};
    // Keep the complete scaled footprint even when transparent pixels leave
    // individual chunk spools empty. Auxiliary import features, such as the
    // optional deny foundation, operate on this source-volume boundary.
    result->source_volume_bounds = full_wall;
    if (!options.block_sink && options.include_source_volume &&
        (!writer || !writer->includeVolume(
            full_wall, error, options.cancellation_requested))) return false;
    if (compactScalerFitsBudget(metadata, target_width,
                                options.streaming_memory_budget_bytes, error)) {
        return streamCompactPngToSpool(path, metadata, target_width, target_height,
                                       options, writer, result, error);
    }
    if (!ensurePixelDirectory(options.spool_directory)) {
        if (error) *error = "cannot create pixel-art work directory";
        return false;
    }
    PixelTemporaryFiles temporary(options.spool_directory);
    if (!decodePngToRgbaFile(path, temporary.rgba, metadata,
                             options.cancellation_requested, error)) return false;
    return scaleRgbaFileToSpool(temporary.rgba, temporary.errors_a, temporary.errors_b,
                                metadata, target_width, target_height, options,
                                writer, result, error);
}

const PaletteBlock& closestBlock(float r, float g, float b) {
    static const ExactPaletteSearch search;
    return search.closest(r, g, b);
}

}  // namespace

bool PixelArtParser::parse(const PixelArtParseOptions& options, SchematicParseResult* result, std::string* error) const {
    if (error) error->clear();
    if (!result || options.source_path.empty() || options.spool_directory.empty() || options.target_width < 1 || options.chunk_size <= 0) {
        if (error) *error = "invalid pixel-art options";
        return false;
    }
    PixelArtParseOptions effective_options = options;
    if (effective_options.create_maps_after_import &&
        (!alignMapMinimum(options.base_x, &effective_options.base_x) ||
         !alignMapMinimum(options.base_z, &effective_options.base_z))) {
        if (error) *error = "pixel-art map alignment exceeds the supported world range";
        return false;
    }
    *result = {};
    if (cancellationRequested(effective_options.cancellation_requested, error)) return false;
    reportProgress(effective_options, SchematicParseStage::ReadingSource);
    std::unique_ptr<ChunkSpoolWriter> writer;
    if (!effective_options.block_sink) {
        writer = std::make_unique<ChunkSpoolWriter>(
            effective_options.spool_directory, effective_options.chunk_size,
            effective_options.maximum_chunk_descriptors);
    }
    try {
        if (!streamPngScaledToSpool(
                effective_options.source_path, effective_options, writer.get(), result, error)) {
            if (writer) writer->discard();
            return false;
        }
    } catch (const std::bad_alloc&) {
        if (writer) writer->discard();
        if (error) *error = "pixel-art decoding needs more memory than available";
        return false;
    }
    if (cancellationRequested(effective_options.cancellation_requested, error)) {
        if (writer) writer->discard();
        return false;
    }
    reportProgress(effective_options, SchematicParseStage::FinalizingSpools, 0,
                   result->imported_block_count);
    if (writer) {
        result->chunks = writer->finish(
            error, effective_options.cancellation_requested,
            [&](uint64_t completed, uint64_t total) {
                reportProgress(effective_options, SchematicParseStage::FinalizingSpools,
                               completed, total);
            });
    }
    if (cancellationRequested(effective_options.cancellation_requested, error)) {
        if (writer) writer->discard();
        result->chunks.clear();
        return false;
    }
    reportProgress(effective_options, SchematicParseStage::FinalizingSpools,
                   result->imported_block_count, result->imported_block_count);
    return effective_options.block_sink ? true : !result->chunks.empty();
}

}  // namespace build_import
