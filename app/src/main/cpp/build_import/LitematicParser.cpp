#include "LitematicParser.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <zlib.h>

namespace build_import {
namespace {

enum NbtTag : uint8_t {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    Long = 4,
    Float = 5,
    Double = 6,
    ByteArray = 7,
    String = 8,
    List = 9,
    Compound = 10,
    IntArray = 11,
    LongArray = 12,
};

constexpr size_t kStreamingBufferSize = 256U * 1024U;
// Direct BlockStates routing never materializes this payload: it is decoded
// from the gzip stream through a 256 KiB buffer.  Keep a finite compressed-NBT
// work limit, but do not reject a valid sparse structure merely because its
// packed air indices exceed the old raw-spool limit.
constexpr uint64_t kMaximumDecompressedNbtBytes =
    4ULL * 1024ULL * 1024ULL * 1024ULL + 64ULL * 1024ULL * 1024ULL;
constexpr uint32_t kMaximumNbtDepth = 64;
constexpr uint32_t kMaximumCompoundEntries = 4096;
constexpr uint32_t kMaximumSkippedListEntries = 1U << 24;
constexpr uint32_t kMaximumRegions = 4096;
constexpr size_t kMaximumNbtNameLength = 256;
constexpr uint32_t kMaximumPaletteEntries = 1U << 16;
constexpr size_t kMaximumPaletteIdentifierLength = 128;
constexpr size_t kMaximumPalettePropertyTokenLength = 64;
constexpr size_t kMaximumPaletteProperties = 128;
constexpr size_t kMaximumPaletteStateLength = 4096;
constexpr uint64_t kMaximumPaletteStringBytes = 16ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumBlockStatesSpoolBytes = 512ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumStreamingBlockStatesBytes =
    4ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr uint64_t kCancellationCheckInterval = 4096;
constexpr size_t kMaximumStateMappingCacheEntries = 1U << 16;
constexpr uint32_t kMaximumDeferredCommandBlocks = 1024U * 1024U;
constexpr uint64_t kMaximumDeferredCommandPayloadBytes = 64ULL * 1024ULL * 1024ULL;
constexpr size_t kMaximumCommandBlockTextBytes = CommandBlockSpoolWriter::kMaximumStringBytes;

using CancellationCallback = std::function<bool()>;

bool cancellationRequested(const CancellationCallback& callback, std::string* error) {
    if (!callback || !callback()) return false;
    if (error) *error = "import cancelled";
    return true;
}

void reportProgress(const SchematicParseOptions& options, SchematicParseStage stage,
                    uint64_t completed = 0, uint64_t total = 0) {
    if (options.progress_callback) options.progress_callback({stage, completed, total});
}

class Reader {
public:
    explicit Reader(const std::string& path)
        : stream_(gzopen(path.c_str(), "rb")), scratch_(kStreamingBufferSize) {
        if (stream_) gzbuffer(stream_, static_cast<unsigned>(kStreamingBufferSize));
    }

    ~Reader() {
        if (stream_) gzclose(stream_);
    }

    bool valid() const { return stream_ != nullptr; }

    bool read(void* data, size_t length) {
        if (length > kMaximumDecompressedNbtBytes - bytes_read_) return false;
        uint8_t* output = static_cast<uint8_t*>(data);
        while (length != 0) {
            const unsigned request = static_cast<unsigned>(
                std::min<size_t>(length, 1U << 20));
            const int received = gzread(stream_, output, request);
            if (received <= 0) return false;
            output += received;
            length -= static_cast<size_t>(received);
            bytes_read_ += static_cast<uint64_t>(received);
        }
        return true;
    }

    bool exhausted() {
        uint8_t byte = 0;
        const int received = gzread(stream_, &byte, 1);
        return received == 0 && gzeof(stream_) != 0;
    }

    uint64_t bytesRead() const { return bytes_read_; }
    uint8_t* scratchData() { return scratch_.data(); }
    size_t scratchSize() const { return scratch_.size(); }

    bool u8(uint8_t* value) { return read(value, 1); }

    bool be16(uint16_t* value) {
        uint8_t bytes[2];
        if (!read(bytes, sizeof(bytes))) return false;
        *value = (static_cast<uint16_t>(bytes[0]) << 8U) | bytes[1];
        return true;
    }

    bool be32(uint32_t* value) {
        uint8_t bytes[4];
        if (!read(bytes, sizeof(bytes))) return false;
        *value = (static_cast<uint32_t>(bytes[0]) << 24U) |
                 (static_cast<uint32_t>(bytes[1]) << 16U) |
                 (static_cast<uint32_t>(bytes[2]) << 8U) |
                 bytes[3];
        return true;
    }

    bool string(std::string* value, size_t maximum_length = kMaximumNbtNameLength) {
        uint16_t length = 0;
        if (!be16(&length)) return false;
        if (length > maximum_length) return false;
        value->assign(length, '\0');
        return length == 0 || read(&(*value)[0], length);
    }

private:
    gzFile stream_ = nullptr;
    std::vector<uint8_t> scratch_;
    uint64_t bytes_read_ = 0;
};

bool skipBytes(Reader& reader, uint64_t length,
               const CancellationCallback& cancellation_requested,
               std::string* error) {
    while (length != 0) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        const size_t amount = static_cast<size_t>(
            std::min<uint64_t>(length, reader.scratchSize()));
        if (!reader.read(reader.scratchData(), amount)) return false;
        length -= amount;
    }
    return true;
}

bool skipPayload(Reader& reader, uint8_t tag, uint32_t depth,
                  const CancellationCallback& cancellation_requested,
                  std::string* error, uint32_t* top_level_list_count = nullptr) {
    if (depth > kMaximumNbtDepth ||
        cancellationRequested(cancellation_requested, error)) return false;
    uint32_t count = 0;
    uint16_t length = 0;
    uint8_t list_tag = 0;
    switch (tag) {
        case Byte: return skipBytes(reader, 1, cancellation_requested, error);
        case Short: return skipBytes(reader, 2, cancellation_requested, error);
        case Int:
        case Float: return skipBytes(reader, 4, cancellation_requested, error);
        case Long:
        case Double: return skipBytes(reader, 8, cancellation_requested, error);
        case String:
            if (!reader.be16(&length)) return false;
            return skipBytes(reader, length, cancellation_requested, error);
        case ByteArray:
            if (!reader.be32(&count) || static_cast<int32_t>(count) < 0) return false;
            return skipBytes(reader, count, cancellation_requested, error);
        case IntArray:
            if (!reader.be32(&count) || static_cast<int32_t>(count) < 0) return false;
            return skipBytes(reader, static_cast<uint64_t>(count) * 4U,
                             cancellation_requested, error);
        case LongArray:
            if (!reader.be32(&count) || static_cast<int32_t>(count) < 0) return false;
            return skipBytes(reader, static_cast<uint64_t>(count) * 8U,
                             cancellation_requested, error);
        case List:
            if (!reader.u8(&list_tag) || !reader.be32(&count) ||
                static_cast<int32_t>(count) < 0 ||
                count > kMaximumSkippedListEntries) return false;
            if (top_level_list_count) *top_level_list_count = count;
            for (uint32_t index = 0; index < count; ++index) {
                if (!skipPayload(reader, list_tag, depth + 1,
                                 cancellation_requested, error)) return false;
            }
            return true;
        case Compound:
            count = 0;
            while (true) {
                if (cancellationRequested(cancellation_requested, error)) return false;
                uint8_t child = 0;
                std::string ignored_name;
                if (!reader.u8(&child)) return false;
                if (child == End) return true;
                if (++count > kMaximumCompoundEntries) return false;
                if (!reader.string(&ignored_name) ||
                    !skipPayload(reader, child, depth + 1,
                                 cancellation_requested, error)) return false;
            }
        default: return false;
    }
}

int createDirectory(const char* path) {
#if defined(_WIN32)
    return ::mkdir(path);
#else
    return ::mkdir(path, 0700);
#endif
}

bool ensureDirectory(const std::string& directory) {
    if (directory.empty()) return false;
    std::string partial;
    for (size_t index = 0; index <= directory.size(); ++index) {
        if (index != directory.size() && directory[index] != '/' &&
            directory[index] != '\\') {
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

bool readInt(Reader& reader, uint8_t tag, int32_t* value) {
    if (tag != Int) return false;
    uint32_t raw = 0;
    if (!reader.be32(&raw)) return false;
    *value = static_cast<int32_t>(raw);
    return true;
}

bool parseVectorCompound(Reader& reader, std::array<int32_t, 3>* vector,
                         const CancellationCallback& cancellation_requested,
                         std::string* error) {
    std::array<bool, 3> seen{};
    uint32_t field_count = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) break;
        if (++field_count > kMaximumCompoundEntries) return false;
        if (!reader.string(&name)) return false;
        const int index = name == "x" ? 0 : name == "y" ? 1 : name == "z" ? 2 : -1;
        if (index >= 0) {
            if (seen[static_cast<size_t>(index)] ||
                !readInt(reader, tag, &(*vector)[static_cast<size_t>(index)])) return false;
            seen[static_cast<size_t>(index)] = true;
        } else if (!skipPayload(reader, tag, 1, cancellation_requested, error)) {
            return false;
        }
    }
    return seen[0] && seen[1] && seen[2];
}

bool readNbtIntegral(Reader& reader, uint8_t tag, int64_t* value) {
    if (!value) return false;
    if (tag == Byte) {
        uint8_t raw = 0;
        if (!reader.u8(&raw)) return false;
        *value = static_cast<int8_t>(raw);
        return true;
    }
    if (tag == Short) {
        uint16_t raw = 0;
        if (!reader.be16(&raw)) return false;
        *value = static_cast<int16_t>(raw);
        return true;
    }
    if (tag == Int) {
        uint32_t raw = 0;
        if (!reader.be32(&raw)) return false;
        *value = static_cast<int32_t>(raw);
        return true;
    }
    if (tag == Long) {
        uint32_t high = 0;
        uint32_t low = 0;
        if (!reader.be32(&high) || !reader.be32(&low)) return false;
        const uint64_t raw = (static_cast<uint64_t>(high) << 32U) | low;
        *value = static_cast<int64_t>(raw);
        return true;
    }
    return false;
}

bool readIntVector3(Reader& reader, std::array<int32_t, 3>* value) {
    if (!value) return false;
    uint32_t count = 0;
    if (!reader.be32(&count) || count != value->size()) return false;
    for (int32_t& component : *value) {
        uint32_t raw = 0;
        if (!reader.be32(&raw)) return false;
        component = static_cast<int32_t>(raw);
    }
    return true;
}

bool readNbtFloating(Reader& reader, uint8_t tag, double* value) {
    if (!value) return false;
    if (tag == Float) {
        uint32_t raw = 0;
        float decoded = 0.0F;
        if (!reader.be32(&raw)) return false;
        std::memcpy(&decoded, &raw, sizeof(decoded));
        *value = decoded;
        return true;
    }
    if (tag == Double) {
        uint32_t high = 0, low = 0;
        if (!reader.be32(&high) || !reader.be32(&low)) return false;
        const uint64_t raw = (static_cast<uint64_t>(high) << 32U) | low;
        double decoded = 0.0;
        std::memcpy(&decoded, &raw, sizeof(decoded));
        *value = decoded;
        return true;
    }
    int64_t integral = 0;
    if (!readNbtIntegral(reader, tag, &integral)) return false;
    *value = static_cast<double>(integral);
    return true;
}

bool readNbtNumericList(Reader& reader, size_t expected_count,
                        std::vector<double>* output,
                        const CancellationCallback& cancellation_requested,
                        std::string* error) {
    if (!output) return false;
    uint8_t element_tag = 0;
    uint32_t count = 0;
    if (!reader.u8(&element_tag) || !reader.be32(&count) ||
        static_cast<int32_t>(count) < 0 || count != expected_count) return false;
    output->clear();
    output->reserve(expected_count);
    for (uint32_t index = 0; index < count; ++index) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        double value = 0.0;
        if (!readNbtFloating(reader, element_tag, &value) || !std::isfinite(value)) {
            return false;
        }
        output->push_back(value);
    }
    return true;
}

std::string legacyEnchantmentIdentifier(int64_t id) {
    switch (id) {
        case 0: return "minecraft:protection";
        case 1: return "minecraft:fire_protection";
        case 2: return "minecraft:feather_falling";
        case 3: return "minecraft:blast_protection";
        case 4: return "minecraft:projectile_protection";
        case 5: return "minecraft:respiration";
        case 6: return "minecraft:aqua_affinity";
        case 7: return "minecraft:thorns";
        case 8: return "minecraft:depth_strider";
        case 9: return "minecraft:frost_walker";
        case 10: return "minecraft:binding_curse";
        case 16: return "minecraft:sharpness";
        case 17: return "minecraft:smite";
        case 18: return "minecraft:bane_of_arthropods";
        case 19: return "minecraft:knockback";
        case 20: return "minecraft:fire_aspect";
        case 21: return "minecraft:looting";
        case 32: return "minecraft:efficiency";
        case 33: return "minecraft:silk_touch";
        case 34: return "minecraft:unbreaking";
        case 35: return "minecraft:fortune";
        case 48: return "minecraft:power";
        case 49: return "minecraft:punch";
        case 50: return "minecraft:flame";
        case 51: return "minecraft:infinity";
        case 61: return "minecraft:luck_of_the_sea";
        case 62: return "minecraft:lure";
        case 65: return "minecraft:loyalty";
        case 66: return "minecraft:impaling";
        case 67: return "minecraft:riptide";
        case 68: return "minecraft:channeling";
        case 70: return "minecraft:mending";
        case 71: return "minecraft:vanishing_curse";
        default: return {};
    }
}

bool parseDeferredEnchantmentCompound(Reader& reader,
                                       const CancellationCallback& cancellation_requested,
                                       std::vector<DeferredEnchantment>* output,
                                       uint64_t* filtered_count, std::string* error) {
    std::string identifier;
    int64_t level = 0;
    bool has_id = false;
    bool has_level = false;
    uint32_t fields = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) break;
        if (++fields > 128 || !reader.string(&name)) return false;
        if ((name == "id" || name == "Id") && tag == String) {
            if (!reader.string(&identifier, 128)) return false;
            has_id = true;
            continue;
        }
        if ((name == "id" || name == "Id") && tag != String) {
            int64_t numeric_id = 0;
            if (readNbtIntegral(reader, tag, &numeric_id)) {
                identifier = legacyEnchantmentIdentifier(numeric_id);
                has_id = !identifier.empty();
                continue;
            }
        }
        if (name == "lvl" || name == "Level" || name == "level") {
            if (readNbtIntegral(reader, tag, &level)) {
                has_level = true;
                continue;
            }
        }
        if (!skipPayload(reader, tag, 2, cancellation_requested, error)) return false;
    }
    DeferredEnchantment enchantment;
    if (!has_id || !has_level ||
        !normalizeDeferredEnchantment(&identifier, level, &enchantment)) {
        if (filtered_count) ++*filtered_count;
        return true;
    }
    if (output && output->size() < 32) output->push_back(std::move(enchantment));
    else if (filtered_count) ++*filtered_count;
    return true;
}

bool parseDeferredEnchantmentList(Reader& reader,
                                  const CancellationCallback& cancellation_requested,
                                  std::vector<DeferredEnchantment>* output,
                                  uint64_t* filtered_count, std::string* error) {
    uint8_t element_tag = 0;
    uint32_t count = 0;
    if (!reader.u8(&element_tag) || !reader.be32(&count) ||
        static_cast<int32_t>(count) < 0 || count > 256) return false;
    for (uint32_t index = 0; index < count; ++index) {
        if (element_tag == Compound) {
            if (!parseDeferredEnchantmentCompound(reader, cancellation_requested, output,
                                                  filtered_count, error)) return false;
        } else if (!skipPayload(reader, element_tag, 2, cancellation_requested, error)) {
            return false;
        }
    }
    return true;
}

bool parseDeferredEnchantLevelsCompound(Reader& reader,
                                         const CancellationCallback& cancellation_requested,
                                         std::vector<DeferredEnchantment>* output,
                                         uint64_t* filtered_count, std::string* error) {
    uint32_t fields = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string id;
        if (!reader.u8(&tag)) return false;
        if (tag == End) return true;
        if (++fields > 256 || !reader.string(&id, 128)) return false;
        int64_t level = 0;
        DeferredEnchantment enchantment;
        if (readNbtIntegral(reader, tag, &level) &&
            normalizeDeferredEnchantment(&id, level, &enchantment)) {
            if (output && output->size() < 32) output->push_back(std::move(enchantment));
            else if (filtered_count) ++*filtered_count;
            continue;
        }
        if (tag >= Byte && tag <= Long) {
            if (filtered_count) ++*filtered_count;
            continue;
        }
        if (!skipPayload(reader, tag, 2, cancellation_requested, error)) return false;
    }
}

bool parseDeferredItemTagCompound(Reader& reader,
                                  const CancellationCallback& cancellation_requested,
                                  std::vector<DeferredEnchantment>* enchantments,
                                  uint64_t* filtered_count, std::string* error) {
    uint32_t fields = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) return true;
        if (++fields > 1024 || !reader.string(&name)) return false;
        if ((name == "Enchantments" || name == "ench") && tag == List) {
            if (!parseDeferredEnchantmentList(reader, cancellation_requested, enchantments,
                                              filtered_count, error)) return false;
            continue;
        }
        if (!skipPayload(reader, tag, 3, cancellation_requested, error)) return false;
    }
}

bool parseDeferredItemComponentsCompound(Reader& reader,
                                         const CancellationCallback& cancellation_requested,
                                         std::vector<DeferredEnchantment>* enchantments,
                                         uint64_t* filtered_count, std::string* error) {
    uint32_t fields = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) return true;
        if (++fields > 1024 || !reader.string(&name)) return false;
        if (name == "minecraft:enchantments" && tag == Compound) {
            uint32_t inner_fields = 0;
            while (true) {
                uint8_t inner_tag = 0;
                std::string inner_name;
                if (!reader.u8(&inner_tag)) return false;
                if (inner_tag == End) break;
                if (++inner_fields > 128 || !reader.string(&inner_name)) return false;
                if (inner_name == "levels" && inner_tag == Compound) {
                    if (!parseDeferredEnchantLevelsCompound(reader, cancellation_requested,
                                                            enchantments, filtered_count,
                                                            error)) return false;
                } else if (!skipPayload(reader, inner_tag, 4,
                                        cancellation_requested, error)) {
                    return false;
                }
            }
            continue;
        }
        if (!skipPayload(reader, tag, 3, cancellation_requested, error)) return false;
    }
}

bool parseDeferredContainerItemCompound(Reader& reader,
                                        const CancellationCallback& cancellation_requested,
                                        ContainerItemRecord* output,
                                        uint64_t* filtered_count,
                                        std::string* error) {
    if (!output) return false;
    ContainerItemRecord record;
    bool has_id = false;
    bool has_count = false;
    bool has_slot = false;
    uint32_t fields = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) break;
        if (++fields > 2048 || !reader.string(&name)) return false;
        if ((name == "id" || name == "Id" || name == "Name") && tag == String) {
            if (!reader.string(&record.item_id, 256)) return false;
            has_id = normalizeDeferredItemIdentifier(&record.item_id);
            continue;
        }
        if (name == "Count" || name == "count") {
            int64_t value = 0;
            if (readNbtIntegral(reader, tag, &value)) {
                has_count = value > 0 && value <= 64;
                if (has_count) record.count = static_cast<uint16_t>(value);
                continue;
            }
        }
        if (name == "Slot" || name == "slot") {
            int64_t value = 0;
            if (readNbtIntegral(reader, tag, &value)) {
                has_slot = value >= 0 && value <= 255;
                if (has_slot) record.slot = static_cast<uint16_t>(value);
                continue;
            }
        }
        if (name == "Damage" || name == "damage" || name == "aux" || name == "Aux") {
            int64_t value = 0;
            if (readNbtIntegral(reader, tag, &value)) {
                if (value >= 0 && value <= UINT16_MAX) {
                    record.aux = static_cast<uint16_t>(value);
                }
                continue;
            }
        }
        if (name == "tag" && tag == Compound) {
            if (!parseDeferredItemTagCompound(reader, cancellation_requested,
                                              &record.enchantments, filtered_count,
                                              error)) return false;
            continue;
        }
        if (name == "components" && tag == Compound) {
            if (!parseDeferredItemComponentsCompound(reader, cancellation_requested,
                                                     &record.enchantments, filtered_count,
                                                     error)) return false;
            continue;
        }
        if (!skipPayload(reader, tag, 2, cancellation_requested, error)) return false;
    }
    if (!has_id || !has_count || !has_slot) return true;
    *output = std::move(record);
    return true;
}

bool parseDeferredContainerItemList(Reader& reader,
                                    const CancellationCallback& cancellation_requested,
                                    std::vector<ContainerItemRecord>* output,
                                    uint64_t* filtered_count, std::string* error) {
    uint8_t element_tag = 0;
    uint32_t count = 0;
    if (!reader.u8(&element_tag) || !reader.be32(&count) ||
        static_cast<int32_t>(count) < 0 || count > 256) return false;
    for (uint32_t index = 0; index < count; ++index) {
        if (element_tag != Compound) {
            if (!skipPayload(reader, element_tag, 2, cancellation_requested, error)) {
                return false;
            }
            continue;
        }
        ContainerItemRecord record;
        if (!parseDeferredContainerItemCompound(reader, cancellation_requested, &record,
                                                filtered_count, error)) return false;
        if (!record.item_id.empty() && output) {
            if (output->size() >= ContainerItemSpoolWriter::kMaximumRecords) {
                if (error) *error = "litematic container-item payload exceeds the safety limit";
                return false;
            }
            output->push_back(std::move(record));
        }
    }
    return true;
}

uint16_t commandBlockModeForIdentifier(std::string_view identifier) {
    const size_t separator = identifier.rfind(':');
    const std::string_view leaf = separator == std::string_view::npos
        ? identifier : identifier.substr(separator + 1);
    const auto equals_ignore_case = [](std::string_view left, std::string_view right) {
        if (left.size() != right.size()) return false;
        for (size_t index = 0; index < left.size(); ++index) {
            const auto lowercase = [](unsigned char value) {
                return value >= 'A' && value <= 'Z'
                    ? static_cast<unsigned char>(value - 'A' + 'a') : value;
            };
            if (lowercase(static_cast<unsigned char>(left[index])) !=
                lowercase(static_cast<unsigned char>(right[index]))) return false;
        }
        return true;
    };
    if (equals_ignore_case(leaf, "repeating_command_block") ||
        equals_ignore_case(leaf, "repeatingcommandblock")) return 1;
    if (equals_ignore_case(leaf, "chain_command_block") ||
        equals_ignore_case(leaf, "chaincommandblock")) return 2;
    return 0;
}

bool isCommandBlockEntityIdentifier(std::string_view identifier) {
    const size_t separator = identifier.rfind(':');
    const std::string_view leaf = separator == std::string_view::npos
        ? identifier : identifier.substr(separator + 1);
    const auto equals_ignore_case = [](std::string_view left, std::string_view right) {
        if (left.size() != right.size()) return false;
        for (size_t index = 0; index < left.size(); ++index) {
            const auto lowercase = [](unsigned char value) {
                return value >= 'A' && value <= 'Z'
                    ? static_cast<unsigned char>(value - 'A' + 'a') : value;
            };
            if (lowercase(static_cast<unsigned char>(left[index])) !=
                lowercase(static_cast<unsigned char>(right[index]))) return false;
        }
        return true;
    };
    return equals_ignore_case(leaf, "command_block") ||
           equals_ignore_case(leaf, "repeating_command_block") ||
           equals_ignore_case(leaf, "chain_command_block") ||
           equals_ignore_case(leaf, "commandblock") ||
           equals_ignore_case(leaf, "repeatingcommandblock") ||
           equals_ignore_case(leaf, "chaincommandblock") ||
           equals_ignore_case(leaf, "control");
}

bool accountPaletteString(size_t length, uint64_t* total, std::string* error) {
    if (length > kMaximumPaletteStringBytes - *total) {
        if (error) *error = "litematic palette string data exceeds safety limit";
        return false;
    }
    *total += length;
    return true;
}

bool parseProperties(Reader& reader,
                     std::vector<std::pair<std::string, std::string>>* properties,
                     uint64_t* palette_string_bytes,
                     const CancellationCallback& cancellation_requested,
                     std::string* error) {
    std::unordered_set<std::string> seen;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) return true;
        std::string value;
        if (!reader.string(&name, kMaximumPalettePropertyTokenLength) ||
            tag != String || name.empty() ||
            name.size() > kMaximumPalettePropertyTokenLength ||
            properties->size() >= kMaximumPaletteProperties ||
            !seen.insert(name).second ||
            !reader.string(&value, kMaximumPalettePropertyTokenLength) || value.empty() ||
            value.size() > kMaximumPalettePropertyTokenLength ||
            !accountPaletteString(name.size(), palette_string_bytes, error) ||
            !accountPaletteString(value.size(), palette_string_bytes, error)) return false;
        properties->emplace_back(std::move(name), std::move(value));
    }
}

bool parsePaletteEntry(Reader& reader, std::string* state,
                       uint64_t* palette_string_bytes,
                       const CancellationCallback& cancellation_requested,
                       std::string* error) {
    std::string identifier;
    std::vector<std::pair<std::string, std::string>> properties;
    bool has_name = false;
    bool has_properties = false;
    uint32_t field_count = 0;
    std::unordered_set<std::string> seen_names;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) break;
        if (++field_count > kMaximumCompoundEntries) return false;
        if (!reader.string(&name) || !seen_names.insert(name).second) return false;
        if (name == "Name") {
            if (tag != String || has_name ||
                !reader.string(&identifier, kMaximumPaletteIdentifierLength) ||
                identifier.empty() ||
                identifier.size() > kMaximumPaletteIdentifierLength ||
                !accountPaletteString(identifier.size(), palette_string_bytes,
                                      error)) return false;
            has_name = true;
        } else if (name == "Properties") {
            if (tag != Compound || has_properties ||
                !parseProperties(reader, &properties, palette_string_bytes,
                                 cancellation_requested, error)) return false;
            has_properties = true;
        } else if (!skipPayload(reader, tag, 2, cancellation_requested, error)) {
            return false;
        }
    }
    if (!has_name) return false;
    size_t state_length = identifier.size();
    if (!properties.empty()) {
        state_length += 2;
        for (const auto& property : properties) {
            state_length += property.first.size() + 1 + property.second.size();
        }
        state_length += properties.size() - 1;
    }
    if (state_length > kMaximumPaletteStateLength) {
        if (error) *error = "litematic palette state exceeds safety limit";
        return false;
    }
    *state = std::move(identifier);
    if (!properties.empty()) {
        state->push_back('[');
        for (size_t index = 0; index < properties.size(); ++index) {
            if (index != 0) state->push_back(',');
            *state += properties[index].first;
            state->push_back('=');
            *state += properties[index].second;
        }
        state->push_back(']');
    }
    return true;
}

bool parsePalette(Reader& reader, std::vector<std::string>* palette,
                  const CancellationCallback& cancellation_requested,
                  std::string* error) {
    uint8_t element_tag = 0;
    uint32_t count = 0;
    if (!reader.u8(&element_tag) || element_tag != Compound ||
        !reader.be32(&count) || static_cast<int32_t>(count) < 0 || count == 0 ||
        count > kMaximumPaletteEntries) return false;
    palette->reserve(count);
    uint64_t palette_string_bytes = 0;
    for (uint32_t index = 0; index < count; ++index) {
        if ((index & 0xFFU) == 0 &&
            cancellationRequested(cancellation_requested, error)) return false;
        std::string state;
        if (!parsePaletteEntry(reader, &state, &palette_string_bytes,
                               cancellation_requested, error)) return false;
        palette->push_back(std::move(state));
    }
    return true;
}

bool skipPalette(Reader& reader,
                 const CancellationCallback& cancellation_requested,
                 std::string* error) {
    uint8_t element_tag = 0;
    uint32_t count = 0;
    if (!reader.u8(&element_tag) || element_tag != Compound ||
        !reader.be32(&count) || static_cast<int32_t>(count) < 0 || count == 0 ||
        count > kMaximumPaletteEntries) return false;
    for (uint32_t index = 0; index < count; ++index) {
        if ((index & 0xFFU) == 0 &&
            cancellationRequested(cancellation_requested, error)) return false;
        if (!skipPayload(reader, Compound, 2, cancellation_requested, error)) return false;
    }
    return true;
}

bool readBlockStateLongCount(Reader& reader, uint64_t maximum_bytes,
                             uint32_t* count, std::string* error) {
    if (!reader.be32(count) || static_cast<int32_t>(*count) < 0) return false;
    const uint64_t byte_count = static_cast<uint64_t>(*count) * 8U;
    if (byte_count > maximum_bytes) {
        if (error) {
            *error = "litematic BlockStates exceeds " +
                std::to_string(maximum_bytes / (1024ULL * 1024ULL)) +
                " MiB safety limit";
        }
        return false;
    }
    return true;
}

bool copyLongArray(Reader& reader, const std::string& path, uint32_t* long_count,
                   const CancellationCallback& cancellation_requested,
                   std::string* error) {
    uint32_t count = 0;
    if (!readBlockStateLongCount(reader, kMaximumBlockStatesSpoolBytes,
                                 &count, error)) return false;
    const uint64_t byte_count = static_cast<uint64_t>(count) * 8U;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error) *error = "cannot create litematic BlockStates spool";
        return false;
    }
    uint64_t remaining = byte_count;
    while (remaining != 0) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        const size_t amount = static_cast<size_t>(
            std::min<uint64_t>(remaining, reader.scratchSize()));
        if (!reader.read(reader.scratchData(), amount)) return false;
        output.write(reinterpret_cast<const char*>(reader.scratchData()),
                     static_cast<std::streamsize>(amount));
        if (!output) return false;
        remaining -= amount;
    }
    output.flush();
    output.close();
    if (!output) return false;
    *long_count = count;
    return true;
}

bool skipBlockStateLongArray(Reader& reader, uint32_t* long_count,
                             const CancellationCallback& cancellation_requested,
                             std::string* error) {
    uint32_t count = 0;
    if (!readBlockStateLongCount(reader, kMaximumStreamingBlockStatesBytes,
                                 &count, error)) return false;
    const uint64_t byte_count = static_cast<uint64_t>(count) * 8U;
    if (!skipBytes(reader, byte_count, cancellation_requested, error)) return false;
    if (long_count) *long_count = count;
    return true;
}

struct RegionData {
    std::string name;
    std::array<int32_t, 3> position{};
    std::array<int32_t, 3> signed_size{};
    std::vector<std::string> palette;
    // TileEntities/BlockEntities are retained independently from the packed
    // palette stream.  The shell mode map is populated while routing blocks,
    // then applied to source records which omit an explicit mode.
    std::vector<CommandBlockRecord> command_block_records;
    std::vector<uint8_t> command_block_mode_explicit;
    std::unordered_map<uint64_t, CommandBlockShellState> command_block_modes;
    uint64_t command_block_text_bytes = 0;
    // Container records remain local until the packed palette has proved the
    // corresponding final shell.  This prevents stale TileEntities from
    // becoming /replaceitem writes after a later palette edit.
    std::vector<ContainerItemRecord> container_item_records;
    std::unordered_map<uint64_t, std::string> container_shells;
    // Litematica stores region-local entity positions.  Keep only normalized,
    // command-safe fields until this region's bounds are known and overlap
    // ownership has been checked.
    std::vector<EntityRecord> entity_records;
    uint64_t filtered_enchantment_count = 0;
    uint32_t block_state_longs = 0;
    uint32_t omitted_entity_count = 0;
    bool has_position = false;
    bool has_size = false;
    bool has_palette = false;
    bool has_block_states = false;
};

using DirectBlockStatesConsumer =
    std::function<bool(Reader& reader, RegionData* region, std::string* error)>;

bool appendRegionCommandBlock(RegionData* region, CommandBlockRecord&& record,
                              bool explicit_mode, std::string* error) {
    if (!region) return false;
    const uint64_t text_bytes = static_cast<uint64_t>(record.command.size()) +
        record.last_output.size() + record.name.size() + record.filtered_name.size();
    if (region->command_block_records.size() >= kMaximumDeferredCommandBlocks ||
        text_bytes > kMaximumDeferredCommandPayloadBytes ||
        region->command_block_text_bytes > kMaximumDeferredCommandPayloadBytes - text_bytes) {
        if (error) *error = "litematic command-block payload exceeds the 64 MiB safety limit";
        return false;
    }
    region->command_block_text_bytes += text_bytes;
    region->command_block_records.push_back(std::move(record));
    region->command_block_mode_explicit.push_back(explicit_mode ? 1U : 0U);
    return true;
}

bool parseLitematicBlockEntityCompound(
    Reader& reader, RegionData* region, bool capture_command_blocks,
    bool capture_container_items, const CancellationCallback& cancellation_requested,
    std::string* error) {
    if (!region) return false;
    CommandBlockRecord record;
    std::string identifier;
    bool explicit_mode = false;
    std::array<bool, 3> has_position{};
    std::vector<ContainerItemRecord> items;
    uint32_t field_count = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) break;
        if (++field_count > kMaximumCompoundEntries || !reader.string(&name)) return false;

        if ((name == "id" || name == "Id") && tag == String && capture_command_blocks) {
            if (!reader.string(&identifier, kMaximumCommandBlockTextBytes)) return false;
            continue;
        }
        if ((name == "Command" || name == "CustomName" || name == "LastOutput") &&
            tag == String && capture_command_blocks) {
            std::string* output = name == "Command" ? &record.command
                : name == "CustomName" ? &record.name : &record.last_output;
            if (!reader.string(output, kMaximumCommandBlockTextBytes)) return false;
            continue;
        }
        const int position_index = name == "x" ? 0 : name == "y" ? 1 : name == "z" ? 2 : -1;
        if (position_index >= 0) {
            int64_t value = 0;
            if (readNbtIntegral(reader, tag, &value)) {
                if (value >= std::numeric_limits<int32_t>::min() &&
                    value <= std::numeric_limits<int32_t>::max()) {
                    int32_t* coordinate = position_index == 0 ? &record.x
                        : position_index == 1 ? &record.y : &record.z;
                    *coordinate = static_cast<int32_t>(value);
                    has_position[static_cast<size_t>(position_index)] = true;
                }
                continue;
            }
        }
        if (name == "Pos" && tag == IntArray) {
            std::array<int32_t, 3> position{};
            if (!readIntVector3(reader, &position)) return false;
            record.x = position[0];
            record.y = position[1];
            record.z = position[2];
            has_position = {{true, true, true}};
            continue;
        }
        if ((name == "Items" || name == "items") && tag == List &&
            capture_container_items) {
            if (!parseDeferredContainerItemList(reader, cancellation_requested, &items,
                                                &region->filtered_enchantment_count,
                                                error)) return false;
            continue;
        }
        if (name == "TrackOutput" || name == "auto" || name == "conditionalMode" ||
            name == "ExecuteOnFirstTick" || name == "TickDelay" ||
            name == "LPCommandMode" || name == "CommandBlockMode") {
            int64_t value = 0;
            if (capture_command_blocks && readNbtIntegral(reader, tag, &value)) {
                if (name == "TrackOutput") record.output_tracked = value != 0;
                else if (name == "auto") record.redstone_mode = value == 0;
                else if (name == "conditionalMode") record.conditional = value != 0;
                else if (name == "ExecuteOnFirstTick") record.executing_on_first_tick = value != 0;
                else if (name == "TickDelay") {
                    if (value >= std::numeric_limits<int32_t>::min() &&
                        value <= std::numeric_limits<int32_t>::max()) {
                        record.tick_delay = static_cast<int32_t>(value);
                    }
                } else if (value >= 0 && value <= UINT16_MAX) {
                    record.mode = static_cast<uint16_t>(value);
                    explicit_mode = true;
                }
                continue;
            }
        }
        if (!skipPayload(reader, tag, 1, cancellation_requested, error)) return false;
    }

    if (capture_container_items && has_position[0] && has_position[1] && has_position[2]) {
        for (ContainerItemRecord& item : items) {
            item.x = record.x;
            item.y = record.y;
            item.z = record.z;
            if (region->container_item_records.size() >=
                ContainerItemSpoolWriter::kMaximumRecords) {
                if (error) *error = "litematic container-item payload exceeds the safety limit";
                return false;
            }
            region->container_item_records.push_back(std::move(item));
        }
    }

    // Empty/default command blocks can omit Command.  A matching command-block
    // shell is required later before this deferred data is emitted, so no
    // unrelated block entity can be turned into a packet write here.
    if (!capture_command_blocks || !has_position[0] || !has_position[1] ||
        !has_position[2] ||
        (!identifier.empty() && !isCommandBlockEntityIdentifier(identifier))) return true;
    if (!explicit_mode) record.mode = commandBlockModeForIdentifier(identifier);
    return appendRegionCommandBlock(region, std::move(record), explicit_mode, error);
}

bool parseLitematicBlockEntityList(
    Reader& reader, RegionData* region, bool capture_command_blocks,
    bool capture_container_items,
    const CancellationCallback& cancellation_requested, std::string* error) {
    uint8_t element_tag = 0;
    uint32_t count = 0;
    if (!reader.u8(&element_tag) || !reader.be32(&count) ||
        static_cast<int32_t>(count) < 0) return false;
    if (element_tag != Compound) {
        for (uint32_t index = 0; index < count; ++index) {
            if (!skipPayload(reader, element_tag, 1, cancellation_requested, error)) return false;
        }
        return true;
    }
    const uint64_t maximum_entities = capture_container_items
        ? ContainerItemSpoolWriter::kMaximumRecords
        : static_cast<uint64_t>(kMaximumDeferredCommandBlocks);
    if (count > maximum_entities) {
        if (error) *error = "litematic block-entity list exceeds the safety limit";
        return false;
    }
    for (uint32_t index = 0; index < count; ++index) {
        if (!parseLitematicBlockEntityCompound(reader, region,
                                               capture_command_blocks,
                                               capture_container_items,
                                               cancellation_requested, error)) {
            if (error && error->empty()) *error = "invalid litematic block-entity list";
            return false;
        }
    }
    return true;
}

bool parseLitematicEntityDataCompound(
    Reader& reader, EntityRecord* entity, bool* has_id, bool* has_position,
    bool* has_rotation, bool* has_name,
    const CancellationCallback& cancellation_requested, std::string* error) {
    if (!entity || !has_id || !has_position || !has_rotation || !has_name) return false;
    uint32_t fields = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) return true;
        if (++fields > kMaximumCompoundEntries || !reader.string(&name)) return false;
        if ((name == "id" || name == "Id") && tag == String) {
            std::string identifier;
            if (!reader.string(&identifier, 256)) return false;
            if (normalizeDeferredEntityIdentifier(&identifier)) {
                entity->entity_id = std::move(identifier);
                *has_id = true;
            }
            continue;
        }
        if (name == "Pos" && tag == List) {
            std::vector<double> position;
            if (!readNbtNumericList(reader, 3, &position, cancellation_requested, error)) {
                return false;
            }
            entity->x = position[0];
            entity->y = position[1];
            entity->z = position[2];
            *has_position = true;
            continue;
        }
        if (name == "Rotation" && tag == List) {
            std::vector<double> rotation;
            if (!readNbtNumericList(reader, 2, &rotation, cancellation_requested, error)) {
                return false;
            }
            entity->yaw = static_cast<float>(rotation[0]);
            entity->pitch = static_cast<float>(rotation[1]);
            *has_rotation = true;
            continue;
        }
        if ((name == "CustomName" || name == "custom_name") && tag == String) {
            std::string value;
            if (!reader.string(&value, 1024) || !normalizeDeferredEntityName(&value)) return false;
            entity->custom_name = std::move(value);
            *has_name = true;
            continue;
        }
        if (!skipPayload(reader, tag, 2, cancellation_requested, error)) return false;
    }
}

bool parseLitematicEntityCompound(Reader& reader, RegionData* region,
                                  const CancellationCallback& cancellation_requested,
                                  std::string* error) {
    if (!region) return false;
    EntityRecord entity;
    bool has_id = false;
    bool has_position = false;
    bool has_rotation = false;
    bool has_name = false;
    uint32_t fields = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) break;
        if (++fields > kMaximumCompoundEntries || !reader.string(&name)) return false;
        if ((name == "id" || name == "Id") && tag == String) {
            std::string identifier;
            if (!reader.string(&identifier, 256)) return false;
            if (normalizeDeferredEntityIdentifier(&identifier)) {
                entity.entity_id = std::move(identifier);
                has_id = true;
            }
            continue;
        }
        if (name == "Pos" && tag == List) {
            std::vector<double> position;
            if (!readNbtNumericList(reader, 3, &position, cancellation_requested, error)) {
                return false;
            }
            entity.x = position[0];
            entity.y = position[1];
            entity.z = position[2];
            has_position = true;
            continue;
        }
        if (name == "Rotation" && tag == List) {
            std::vector<double> rotation;
            if (!readNbtNumericList(reader, 2, &rotation, cancellation_requested, error)) {
                return false;
            }
            entity.yaw = static_cast<float>(rotation[0]);
            entity.pitch = static_cast<float>(rotation[1]);
            has_rotation = true;
            continue;
        }
        if ((name == "CustomName" || name == "custom_name") && tag == String) {
            if (!reader.string(&entity.custom_name, 1024) ||
                !normalizeDeferredEntityName(&entity.custom_name)) return false;
            has_name = true;
            continue;
        }
        if (name == "EntityData" && tag == Compound) {
            EntityRecord nested;
            bool nested_id = false;
            bool nested_position = false;
            bool nested_rotation = false;
            bool nested_name = false;
            if (!parseLitematicEntityDataCompound(reader, &nested, &nested_id,
                                                  &nested_position, &nested_rotation,
                                                  &nested_name,
                                                  cancellation_requested, error)) {
                return false;
            }
            // Litematica's outer Pos is the region-local canonical position.
            // EntityData is only a compatibility fallback for older exporters.
            if (!has_id && nested_id) {
                entity.entity_id = std::move(nested.entity_id);
                has_id = true;
            }
            if (!has_position && nested_position) {
                entity.x = nested.x;
                entity.y = nested.y;
                entity.z = nested.z;
                has_position = true;
            }
            if (!has_rotation && nested_rotation) {
                entity.yaw = nested.yaw;
                entity.pitch = nested.pitch;
                has_rotation = true;
            }
            if (!has_name && nested_name) {
                entity.custom_name = std::move(nested.custom_name);
                has_name = true;
            }
            continue;
        }
        if (!skipPayload(reader, tag, 1, cancellation_requested, error)) return false;
    }
    if (!has_id || !has_position || !std::isfinite(entity.x) ||
        !std::isfinite(entity.y) || !std::isfinite(entity.z) ||
        !std::isfinite(entity.yaw) || !std::isfinite(entity.pitch)) {
        ++region->omitted_entity_count;
        return true;
    }
    if (region->entity_records.size() >= EntitySpoolWriter::kMaximumRecords) {
        if (error) *error = "litematic entity payload exceeds the safety limit";
        return false;
    }
    region->entity_records.push_back(std::move(entity));
    return true;
}

bool parseLitematicEntityList(Reader& reader, RegionData* region,
                              const CancellationCallback& cancellation_requested,
                              std::string* error) {
    uint8_t element_tag = 0;
    uint32_t count = 0;
    if (!reader.u8(&element_tag) || !reader.be32(&count) ||
        static_cast<int32_t>(count) < 0 || count > EntitySpoolWriter::kMaximumRecords) {
        return false;
    }
    if (element_tag != Compound) {
        for (uint32_t index = 0; index < count; ++index) {
            if (!skipPayload(reader, element_tag, 1, cancellation_requested, error)) return false;
        }
        if (region) region->omitted_entity_count += count;
        return true;
    }
    for (uint32_t index = 0; index < count; ++index) {
        if (!parseLitematicEntityCompound(reader, region, cancellation_requested, error)) {
            if (error && error->empty()) *error = "invalid litematic entity list";
            return false;
        }
    }
    return true;
}

bool parseRegionPayload(Reader& reader, const SchematicParseOptions& options,
                        const std::string& block_states_path,
                         RegionData* region,
                         const DirectBlockStatesConsumer& direct_consumer,
                         bool* routed_directly,
                         bool* requires_replay,
                         const CancellationCallback& cancellation_requested,
                         std::string* error) {
    if (routed_directly) *routed_directly = false;
    if (requires_replay) *requires_replay = false;
    std::unordered_set<std::string> seen_names;
    uint32_t field_count = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) break;
        if (++field_count > kMaximumCompoundEntries) {
            if (error) *error = "litematic region contains too many tags";
            return false;
        }
        if (!reader.string(&name) || !seen_names.insert(name).second) return false;
        if (name == "Position") {
            if (tag != Compound || region->has_position ||
                !parseVectorCompound(reader, &region->position,
                                     cancellation_requested, error)) return false;
            region->has_position = true;
        } else if (name == "Size") {
            if (tag != Compound || region->has_size ||
                !parseVectorCompound(reader, &region->signed_size,
                                     cancellation_requested, error)) return false;
            region->has_size = true;
        } else if (name == "BlockStatePalette") {
            if (tag != List || region->has_palette ||
                !parsePalette(reader, &region->palette,
                              cancellation_requested, error)) return false;
            region->has_palette = true;
        } else if (name == "BlockStates") {
            if (tag != LongArray || region->has_block_states) return false;
            // Litematica normally writes geometry and the palette before the
            // packed states. Decode that common order directly from the gzip
            // stream. Unusual field orders are replayed after their metadata
            // has been read, so valid NBT remains order independent without a
            // giant raw BlockStates file.
            if (region->has_position && region->has_size && region->has_palette &&
                direct_consumer) {
                if (!direct_consumer(reader, region, error)) return false;
                if (routed_directly) *routed_directly = true;
            } else if (direct_consumer) {
                // BlockStates may precede Position, Size or the palette.  Do
                // not write a potentially multi-gigabyte raw copy merely to
                // recover from that legal NBT order.  The caller will replay
                // this region after its metadata has been parsed.
                if (!skipBlockStateLongArray(reader, &region->block_state_longs,
                                             cancellation_requested, error)) {
                    return false;
                }
                if (requires_replay) *requires_replay = true;
            } else if (!copyLongArray(reader, block_states_path,
                                       &region->block_state_longs,
                                      cancellation_requested, error)) {
                return false;
            }
            region->has_block_states = true;
        } else if ((name == "TileEntities" || name == "BlockEntities") &&
                    (options.command_block_sink || options.container_item_sink)) {
            if (tag != List || !parseLitematicBlockEntityList(
                    reader, region, options.command_block_sink != nullptr,
                    options.container_item_sink != nullptr,
                    cancellation_requested, error)) return false;
        } else if (name == "Entities" && options.entity_sink) {
            if (tag != List || !parseLitematicEntityList(
                    reader, region, cancellation_requested, error)) return false;
        } else if (name == "Entities") {
            if (tag != List || !skipPayload(reader, tag, 1, cancellation_requested,
                                             error, &region->omitted_entity_count)) return false;
        } else if (!skipPayload(reader, tag, 1, cancellation_requested, error)) {
            return false;
        }
    }
    return region->has_position && region->has_size && region->has_palette &&
           region->has_block_states;
}

// The first litematic pass only retains region geometry.  It still validates
// palette and BlockStates payloads, then the normal parser re-reads the source
// and routes one region at a time.  This keeps overlap handling bounded by the
// number of regions instead of the number of voxels.
bool scanRegionPayload(Reader& reader, RegionData* region,
                       const CancellationCallback& cancellation_requested,
                       std::string* error) {
    std::unordered_set<std::string> seen_names;
    uint32_t field_count = 0;
    while (true) {
        if (cancellationRequested(cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) break;
        if (++field_count > kMaximumCompoundEntries ||
            !reader.string(&name) || !seen_names.insert(name).second) return false;
        if (name == "Position") {
            if (tag != Compound || region->has_position ||
                !parseVectorCompound(reader, &region->position,
                                     cancellation_requested, error)) return false;
            region->has_position = true;
        } else if (name == "Size") {
            if (tag != Compound || region->has_size ||
                !parseVectorCompound(reader, &region->signed_size,
                                     cancellation_requested, error)) return false;
            region->has_size = true;
        } else if (name == "BlockStatePalette") {
            if (tag != List || region->has_palette ||
                !skipPalette(reader, cancellation_requested, error)) return false;
            region->has_palette = true;
        } else if (name == "BlockStates") {
            if (tag != LongArray || region->has_block_states ||
                !skipBlockStateLongArray(reader, nullptr,
                                         cancellation_requested, error)) return false;
            region->has_block_states = true;
        } else if (!skipPayload(reader, tag, 1, cancellation_requested, error)) {
            return false;
        }
    }
    return region->has_position && region->has_size && region->has_palette &&
           region->has_block_states;
}

uint32_t paletteBits(size_t palette_size) {
    uint32_t bits = 0;
    size_t capacity = 1;
    while (capacity < palette_size) {
        ++bits;
        if (capacity > std::numeric_limits<size_t>::max() / 2U) return 0;
        capacity <<= 1U;
    }
    return std::max<uint32_t>(2, bits);
}

bool checkedMultiply(uint64_t lhs, uint64_t rhs, uint64_t* value) {
    if (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs) return false;
    *value = lhs * rhs;
    return true;
}

bool absoluteDimension(int32_t signed_dimension, uint64_t* dimension) {
    if (signed_dimension == 0) return false;
    const int64_t value = signed_dimension;
    *dimension = static_cast<uint64_t>(value < 0 ? -value : value);
    return *dimension <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
}

bool regionAxisBounds(int32_t base, int32_t position, int32_t signed_size,
                      int32_t* minimum, int32_t* maximum) {
    const int64_t origin = static_cast<int64_t>(base) + position;
    const int64_t other = origin + (signed_size > 0
        ? static_cast<int64_t>(signed_size) - 1
        : static_cast<int64_t>(signed_size) + 1);
    const int64_t low = std::min(origin, other);
    const int64_t high = std::max(origin, other);
    if (low < std::numeric_limits<int32_t>::min() ||
        high > std::numeric_limits<int32_t>::max()) return false;
    *minimum = static_cast<int32_t>(low);
    *maximum = static_cast<int32_t>(high);
    return true;
}

bool regionBounds(const SchematicParseOptions& options, const RegionData& region,
                  BlockBounds* bounds, std::string* error) {
    std::array<uint64_t, 3> ignored_size{};
    for (size_t axis = 0; axis < ignored_size.size(); ++axis) {
        if (!absoluteDimension(region.signed_size[axis], &ignored_size[axis])) {
            if (error) *error = "litematic region has an invalid zero or oversized dimension";
            return false;
        }
    }
    if (!regionAxisBounds(options.base_x, region.position[0], region.signed_size[0],
                          &bounds->min_x, &bounds->max_x) ||
        !regionAxisBounds(options.base_y, region.position[1], region.signed_size[1],
                          &bounds->min_y, &bounds->max_y) ||
        !regionAxisBounds(options.base_z, region.position[2], region.signed_size[2],
                          &bounds->min_z, &bounds->max_z)) {
        if (error) *error = "litematic region coordinates exceed the supported world range";
        return false;
    }
    return true;
}

bool intersects(const BlockBounds& left, const BlockBounds& right) {
    return left.min_x <= right.max_x && left.max_x >= right.min_x &&
           left.min_y <= right.max_y && left.max_y >= right.min_y &&
           left.min_z <= right.max_z && left.max_z >= right.min_z;
}

bool containsPoint(const BlockBounds& bounds, int32_t x, int32_t y, int32_t z) {
    return x >= bounds.min_x && x <= bounds.max_x &&
           y >= bounds.min_y && y <= bounds.max_y &&
           z >= bounds.min_z && z <= bounds.max_z;
}

struct RegionOverlay {
    std::string name;
    BlockBounds bounds;
};

// A compact BVH lets the routing pass answer "does a later region own this
// voxel?" without retaining any block data.  Region count is capped at 4096,
// so this stays small even for very large litematics.
class RegionOverlayIndex {
public:
    void add(std::string name, const BlockBounds& bounds) {
        regions_.push_back({std::move(name), bounds});
    }

    bool build() {
        indices_.clear();
        nodes_.clear();
        if (regions_.empty()) return false;
        indices_.reserve(regions_.size());
        for (uint32_t index = 0; index < regions_.size(); ++index) {
            indices_.push_back(index);
        }
        nodes_.reserve(regions_.size() * 2U);
        root_node_ = buildNode(0, indices_.size());
        return true;
    }

    size_t size() const { return regions_.size(); }

    bool matches(size_t index, const std::string& name, const BlockBounds& bounds) const {
        if (index >= regions_.size()) return false;
        const RegionOverlay& expected = regions_[index];
        return expected.name == name && expected.bounds.min_x == bounds.min_x &&
               expected.bounds.min_y == bounds.min_y && expected.bounds.min_z == bounds.min_z &&
               expected.bounds.max_x == bounds.max_x && expected.bounds.max_y == bounds.max_y &&
               expected.bounds.max_z == bounds.max_z;
    }

    bool hasLaterOverlap(const BlockBounds& bounds, uint32_t current_region) const {
        return root_node_ != kNoNode && overlapsLater(root_node_, bounds, current_region);
    }

    bool isCoveredByLater(int32_t x, int32_t y, int32_t z,
                          uint32_t current_region) const {
        return root_node_ != kNoNode &&
               containsLater(root_node_, x, y, z, current_region);
    }

private:
    struct Node {
        BlockBounds bounds;
        size_t start = 0;
        size_t count = 0;
        uint32_t left = kNoNode;
        uint32_t right = kNoNode;
        uint32_t max_region_index = 0;
    };

    static constexpr uint32_t kNoNode = std::numeric_limits<uint32_t>::max();
    static constexpr size_t kLeafRegionCount = 8;

    static int64_t center(const BlockBounds& bounds, int axis) {
        switch (axis) {
            case 0: return static_cast<int64_t>(bounds.min_x) + bounds.max_x;
            case 1: return static_cast<int64_t>(bounds.min_y) + bounds.max_y;
            default: return static_cast<int64_t>(bounds.min_z) + bounds.max_z;
        }
    }

    uint32_t buildNode(size_t begin, size_t end) {
        const uint32_t node_index = static_cast<uint32_t>(nodes_.size());
        Node node;
        node.start = begin;
        node.count = end - begin;
        node.bounds = regions_[indices_[begin]].bounds;
        node.max_region_index = indices_[begin];
        for (size_t offset = begin + 1; offset < end; ++offset) {
            const RegionOverlay& region = regions_[indices_[offset]];
            node.bounds.min_x = std::min(node.bounds.min_x, region.bounds.min_x);
            node.bounds.min_y = std::min(node.bounds.min_y, region.bounds.min_y);
            node.bounds.min_z = std::min(node.bounds.min_z, region.bounds.min_z);
            node.bounds.max_x = std::max(node.bounds.max_x, region.bounds.max_x);
            node.bounds.max_y = std::max(node.bounds.max_y, region.bounds.max_y);
            node.bounds.max_z = std::max(node.bounds.max_z, region.bounds.max_z);
            node.max_region_index = std::max(node.max_region_index, indices_[offset]);
        }
        nodes_.push_back(node);
        if (node.count <= kLeafRegionCount) return node_index;

        const int64_t span_x = static_cast<int64_t>(node.bounds.max_x) - node.bounds.min_x;
        const int64_t span_y = static_cast<int64_t>(node.bounds.max_y) - node.bounds.min_y;
        const int64_t span_z = static_cast<int64_t>(node.bounds.max_z) - node.bounds.min_z;
        const int axis = span_y > span_x && span_y >= span_z ? 1 :
                         span_z > span_x && span_z > span_y ? 2 : 0;
        const size_t middle = begin + node.count / 2U;
        std::nth_element(indices_.begin() + static_cast<std::ptrdiff_t>(begin),
                         indices_.begin() + static_cast<std::ptrdiff_t>(middle),
                         indices_.begin() + static_cast<std::ptrdiff_t>(end),
                         [&](uint32_t left, uint32_t right) {
                             return center(regions_[left].bounds, axis) <
                                    center(regions_[right].bounds, axis);
                         });
        const uint32_t left = buildNode(begin, middle);
        const uint32_t right = buildNode(middle, end);
        nodes_[node_index].count = 0;
        nodes_[node_index].left = left;
        nodes_[node_index].right = right;
        return node_index;
    }

    bool overlapsLater(uint32_t node_index, const BlockBounds& bounds,
                       uint32_t current_region) const {
        const Node& node = nodes_[node_index];
        if (node.max_region_index <= current_region || !intersects(node.bounds, bounds)) {
            return false;
        }
        if (node.count != 0) {
            for (size_t offset = node.start; offset < node.start + node.count; ++offset) {
                const uint32_t region_index = indices_[offset];
                if (region_index > current_region &&
                    intersects(regions_[region_index].bounds, bounds)) return true;
            }
            return false;
        }
        return overlapsLater(node.left, bounds, current_region) ||
               overlapsLater(node.right, bounds, current_region);
    }

    bool containsLater(uint32_t node_index, int32_t x, int32_t y, int32_t z,
                       uint32_t current_region) const {
        const Node& node = nodes_[node_index];
        if (node.max_region_index <= current_region ||
            !containsPoint(node.bounds, x, y, z)) return false;
        if (node.count != 0) {
            for (size_t offset = node.start; offset < node.start + node.count; ++offset) {
                const uint32_t region_index = indices_[offset];
                if (region_index > current_region &&
                    containsPoint(regions_[region_index].bounds, x, y, z)) return true;
            }
            return false;
        }
        return containsLater(node.left, x, y, z, current_region) ||
               containsLater(node.right, x, y, z, current_region);
    }

    std::vector<RegionOverlay> regions_;
    std::vector<uint32_t> indices_;
    std::vector<Node> nodes_;
    uint32_t root_node_ = kNoNode;
};

bool checkedSourceOffset(int32_t minimum, int32_t base, int32_t* offset) {
    const int64_t value = static_cast<int64_t>(minimum) - base;
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) return false;
    *offset = static_cast<int32_t>(value);
    return true;
}

class LongWordReader {
public:
    explicit LongWordReader(const std::string& path) {
        stream_.open(path, std::ios::binary);
    }

    bool valid() const { return stream_.is_open(); }

    bool read(uint64_t* value) {
        uint8_t bytes[8];
        stream_.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
        if (stream_.gcount() != static_cast<std::streamsize>(sizeof(bytes))) return false;
        *value = (static_cast<uint64_t>(bytes[0]) << 56U) |
                 (static_cast<uint64_t>(bytes[1]) << 48U) |
                 (static_cast<uint64_t>(bytes[2]) << 40U) |
                 (static_cast<uint64_t>(bytes[3]) << 32U) |
                 (static_cast<uint64_t>(bytes[4]) << 24U) |
                 (static_cast<uint64_t>(bytes[5]) << 16U) |
                 (static_cast<uint64_t>(bytes[6]) << 8U) |
                 bytes[7];
        ++words_read_;
        return true;
    }

    uint32_t wordsRead() const { return words_read_; }

private:
    std::ifstream stream_;
    uint32_t words_read_ = 0;
};

class StreamLongWordReader {
public:
    explicit StreamLongWordReader(Reader* reader) : reader_(reader) {}

    bool valid() const { return reader_ != nullptr; }

    bool read(uint64_t* value) {
        uint8_t bytes[8];
        if (!reader_ || !reader_->read(bytes, sizeof(bytes))) return false;
        *value = (static_cast<uint64_t>(bytes[0]) << 56U) |
                 (static_cast<uint64_t>(bytes[1]) << 48U) |
                 (static_cast<uint64_t>(bytes[2]) << 40U) |
                 (static_cast<uint64_t>(bytes[3]) << 32U) |
                 (static_cast<uint64_t>(bytes[4]) << 24U) |
                 (static_cast<uint64_t>(bytes[5]) << 16U) |
                 (static_cast<uint64_t>(bytes[6]) << 8U) |
                 bytes[7];
        ++words_read_;
        return true;
    }

    uint32_t wordsRead() const { return words_read_; }

private:
    Reader* reader_ = nullptr;
    uint32_t words_read_ = 0;
};

template <typename WordReader>
class BasicPackedPaletteReader {
public:
    template <typename Source>
    BasicPackedPaletteReader(Source&& source, uint32_t bits)
        : words_(std::forward<Source>(source)), bits_(bits),
          mask_((uint64_t{1} << bits) - 1U) {}

    bool valid() const { return words_.valid() && bits_ > 0 && bits_ < 64; }

    bool read(uint32_t* value) {
        if (remaining_bits_ == 0) {
            if (!words_.read(&word_)) return false;
            remaining_bits_ = 64;
        }
        uint64_t decoded = 0;
        if (remaining_bits_ >= bits_) {
            decoded = word_ & mask_;
            word_ >>= bits_;
            remaining_bits_ -= bits_;
        } else {
            const uint32_t low_bits = remaining_bits_;
            decoded = word_;
            if (!words_.read(&word_)) return false;
            const uint32_t high_bits = bits_ - low_bits;
            decoded |= (word_ & ((uint64_t{1} << high_bits) - 1U)) << low_bits;
            word_ >>= high_bits;
            remaining_bits_ = 64 - high_bits;
        }
        *value = static_cast<uint32_t>(decoded);
        return true;
    }

    // Palette index zero is normally air.  When an entire packed word is
    // zero, decode its full values in one step instead of doing per-voxel
    // bit extraction and coordinate work.  The caller decides whether index
    // zero is actually skippable and caps the run at the region's remaining
    // voxel count.  This also preserves the bit state when a value crosses a
    // 64-bit word boundary.
    bool skipZeroValues(uint64_t maximum_values, uint64_t* skipped_values) {
        if (!skipped_values) return false;
        *skipped_values = 0;
        while (*skipped_values < maximum_values) {
            if (remaining_bits_ == 0) {
                if (!words_.read(&word_)) return false;
                remaining_bits_ = 64;
            }
            if (word_ != 0) return true;
            const uint32_t available_values = remaining_bits_ / bits_;
            // The pending value crosses into the next word.  Its upper bits
            // still need to be inspected by read().
            if (available_values == 0) return true;
            const uint32_t consumed_values = static_cast<uint32_t>(
                std::min<uint64_t>(available_values,
                                   maximum_values - *skipped_values));
            const uint32_t consumed_bits = consumed_values * bits_;
            if (consumed_bits == 64) word_ = 0;
            else word_ >>= consumed_bits;
            remaining_bits_ -= consumed_bits;
            *skipped_values += consumed_values;
            if (consumed_values != available_values || remaining_bits_ != 0) return true;
        }
        return true;
    }

    uint32_t wordsRead() const { return words_.wordsRead(); }
    bool hasZeroPadding() const { return remaining_bits_ == 0 || word_ == 0; }

private:
    WordReader words_;
    uint32_t bits_ = 0;
    uint32_t remaining_bits_ = 0;
    uint64_t mask_ = 0;
    uint64_t word_ = 0;
};

using PackedPaletteReader = BasicPackedPaletteReader<LongWordReader>;
using StreamPackedPaletteReader = BasicPackedPaletteReader<StreamLongWordReader>;
using StateMappingCache = std::unordered_map<std::string, BlockMappingResult>;

template <typename PackedReader>
bool routeRegionPacked(const SchematicParseOptions& options,
                       RegionData* region_ptr, PackedReader* packed,
                       StateMappingCache* mapping_cache,
                       ChunkSpoolWriter* writer, SchematicParseResult* result,
                       uint64_t completed_before,
                       const RegionOverlayIndex& overlays,
                       uint32_t region_index, std::string* error) {
    if (!region_ptr) {
        if (error) *error = "litematic region output is unavailable";
        return false;
    }
    const RegionData& region = *region_ptr;
    std::array<uint64_t, 3> size{};
    for (size_t axis = 0; axis < size.size(); ++axis) {
        if (!absoluteDimension(region.signed_size[axis], &size[axis])) {
            if (error) *error = "litematic region has an invalid zero or oversized dimension";
            return false;
        }
    }
    uint64_t layer_area = 0;
    uint64_t voxel_count = 0;
    if (!checkedMultiply(size[0], size[2], &layer_area) ||
        !checkedMultiply(layer_area, size[1], &voxel_count) || voxel_count == 0) {
        if (error) *error = "litematic region volume overflows";
        return false;
    }
    const uint32_t bits = paletteBits(region.palette.size());
    if (bits == 0 || bits >= 64 || voxel_count >
        (std::numeric_limits<uint64_t>::max() - 63U) / bits) {
        if (error) *error = "litematic palette bit width is invalid";
        return false;
    }
    const uint64_t expected_words = (voxel_count * bits + 63U) / 64U;
    if (expected_words != region.block_state_longs) {
        if (error) {
            *error = "litematic BlockStates length does not match region volume (" +
                region.name + ")";
        }
        return false;
    }

    BlockBounds bounds;
    if (!regionBounds(options, region, &bounds, error)) return false;
    if (!options.block_sink && options.include_source_volume &&
        (!writer || !writer->includeVolume(
            bounds, error, options.cancellation_requested))) return false;

    if (!result->source_volume_bounds.isValid()) result->source_volume_bounds = bounds;
    else {
        result->source_volume_bounds.min_x = std::min(result->source_volume_bounds.min_x, bounds.min_x);
        result->source_volume_bounds.min_y = std::min(result->source_volume_bounds.min_y, bounds.min_y);
        result->source_volume_bounds.min_z = std::min(result->source_volume_bounds.min_z, bounds.min_z);
        result->source_volume_bounds.max_x = std::max(result->source_volume_bounds.max_x, bounds.max_x);
        result->source_volume_bounds.max_y = std::max(result->source_volume_bounds.max_y, bounds.max_y);
        result->source_volume_bounds.max_z = std::max(result->source_volume_bounds.max_z, bounds.max_z);
    }
    if (result->source_voxel_count > std::numeric_limits<uint64_t>::max() - voxel_count) {
        if (error) *error = "litematic total voxel count overflows";
        return false;
    }
    result->source_voxel_count += voxel_count;

    std::vector<BlockMappingResult> mappings;
    mappings.reserve(region.palette.size());
    for (const std::string& state : region.palette) {
        if (mapping_cache) {
            const auto found = mapping_cache->find(state);
            if (found != mapping_cache->end()) {
                mappings.push_back(found->second);
                continue;
            }
        }
        BlockMappingResult mapping = options.state_block_resolver(state);
        if (mapping_cache &&
            mapping_cache->size() < kMaximumStateMappingCacheEntries) {
            mapping_cache->emplace(state, mapping);
        }
        mappings.push_back(std::move(mapping));
    }

    if (!packed || !packed->valid()) {
        if (error) *error = "cannot read litematic BlockStates data";
        return false;
    }
    const bool has_later_overlap = overlays.hasLaterOverlap(bounds, region_index);
    const bool can_skip_zero_runs = !has_later_overlap && !mappings.empty() &&
        mappings.front().status == BlockMappingStatus::Air;
    const BlockMappingResult* zero_mapping = can_skip_zero_runs ? &mappings.front() : nullptr;
    if (options.command_block_sink && !region_ptr->command_block_records.empty()) {
        region_ptr->command_block_modes.reserve(std::min<size_t>(
            region_ptr->command_block_records.size(), 4096U));
    }
    if (options.container_item_sink && !region_ptr->container_item_records.empty()) {
        region_ptr->container_shells.reserve(std::min<size_t>(
            region_ptr->container_item_records.size(), 4096U));
    }
    uint64_t last_reported_index = 0;
    const auto reportRouted = [&](uint64_t processed) {
        if (processed == voxel_count ||
            processed - last_reported_index >= kCancellationCheckInterval) {
            reportProgress(options, SchematicParseStage::RoutingBlocks,
                           completed_before + processed,
                           completed_before + voxel_count);
            last_reported_index = processed;
        }
    };
    reportProgress(options, SchematicParseStage::RoutingBlocks,
                   completed_before, completed_before + voxel_count);
    for (uint64_t index = 0; index < voxel_count;) {
        if ((index % kCancellationCheckInterval) == 0 &&
            cancellationRequested(options.cancellation_requested, error)) return false;
        if (can_skip_zero_runs) {
            uint64_t skipped_values = 0;
            if (!packed->skipZeroValues(voxel_count - index, &skipped_values)) {
                if (error) *error = "truncated litematic BlockStates data";
                return false;
            }
            if (skipped_values != 0) {
                result->skipped_block_count += skipped_values;
                if (!zero_mapping->reason.empty()) {
                    result->degraded_block_count += skipped_values;
                    if (result->first_degradation_reason.empty()) {
                        result->first_degradation_reason = region.palette.front() +
                            ": " + zero_mapping->reason;
                    }
                }
                index += skipped_values;
                reportRouted(index);
                continue;
            }
        }
        uint32_t palette_index = 0;
        if (!packed->read(&palette_index)) {
            if (error) *error = "truncated litematic BlockStates data";
            return false;
        }
        if (palette_index >= mappings.size()) {
            if (error) *error = "litematic BlockStates palette index is out of range";
            return false;
        }
        const uint64_t local_x = index % size[0];
        const uint64_t local_z = (index / size[0]) % size[2];
        const uint64_t local_y = index / layer_area;
        // Litematica stores the signed Size to preserve which selection
        // corner was first, but BlockStates are always indexed from the
        // region's minimum corner. Applying the sign to local coordinates
        // mirrors every negative axis, most visibly turning negative-Y
        // regions upside down.
        const int32_t world_x = static_cast<int32_t>(
            static_cast<int64_t>(bounds.min_x) + local_x);
        const int32_t world_y = static_cast<int32_t>(
            static_cast<int64_t>(bounds.min_y) + local_y);
        const int32_t world_z = static_cast<int32_t>(
            static_cast<int64_t>(bounds.min_z) + local_z);
        // The NBT compound preserves its region entry order.  A later region
        // owns every voxel in its box, including palette air, so an earlier
        // record must not reach the placement spool.
        if (has_later_overlap &&
            overlays.isCoveredByLater(world_x, world_y, world_z, region_index)) {
            ++result->skipped_block_count;
            ++index;
            reportRouted(index);
            continue;
        }
        const BlockMappingResult& mapping = mappings[palette_index];
        if (mapping.status == BlockMappingStatus::Air) {
            ++result->skipped_block_count;
            // Most air palette entries are intentional, but a mapper can also
            // safely skip a Java-only block which has no target equivalent.
            // Keep that loss visible in the import summary.
            if (!mapping.reason.empty()) {
                ++result->degraded_block_count;
                if (result->first_degradation_reason.empty()) {
                    result->first_degradation_reason = region.palette[palette_index] +
                        ": " + mapping.reason;
                }
            }
        } else if (mapping.status == BlockMappingStatus::Unsupported) {
            ++result->unsupported_block_count;
            if (error) {
                *error = "unsupported litematic block in region " + region.name +
                    " at local (" + std::to_string(local_x) + "," +
                    std::to_string(local_y) + "," + std::to_string(local_z) +
                    "): " + region.palette[palette_index];
                if (!mapping.reason.empty()) *error += " (" + mapping.reason + ")";
            }
            return false;
        } else {
            if (options.maximum_output_blocks != 0 &&
                result->imported_block_count >= options.maximum_output_blocks) {
                if (error) {
                    *error = "parsed block count exceeds configured limit of " +
                        std::to_string(options.maximum_output_blocks);
                }
                return false;
            }
            ParsedBlock block{
                world_x, world_y, world_z, mapping.spec,
            };
            if (options.block_sink) {
                if (!options.block_sink(block, error)) return false;
            } else if (!writer || !writer->append(block, error)) {
                if (error && error->empty()) {
                    *error = "litematic block output is unavailable";
                }
                return false;
            }
            if (!mapping.reason.empty()) {
                ++result->degraded_block_count;
                if (result->first_degradation_reason.empty()) {
                    result->first_degradation_reason = region.palette[palette_index] +
                        ": " + mapping.reason;
                }
            }
            if (options.command_block_sink &&
                (mapping.spec.command_name == "minecraft:command_block" ||
                 mapping.spec.command_name == "minecraft:repeating_command_block" ||
                 mapping.spec.command_name == "minecraft:chain_command_block")) {
                region_ptr->command_block_modes.emplace(
                    index, commandBlockShellState(commandBlockModeForIdentifier(
                               mapping.spec.command_name), mapping.spec.aux));
            }
            if (options.container_item_sink &&
                deferredContainerIdentifier(mapping.spec.command_name)) {
                region_ptr->container_shells.emplace(index, mapping.spec.command_name);
            }
            ++result->imported_block_count;
        }
        ++index;
        reportRouted(index);
    }
    if (packed->wordsRead() != region.block_state_longs) {
        if (error) *error = "litematic BlockStates contains trailing data";
        return false;
    }
    if (!packed->hasZeroPadding()) {
        if (error) *error = "litematic BlockStates has non-zero padding bits";
        return false;
    }
    reportProgress(options, SchematicParseStage::RoutingBlocks,
                   completed_before + voxel_count,
                   completed_before + voxel_count);
    return true;
}

bool routeRegion(const SchematicParseOptions& options,
                 const std::string& block_states_path, RegionData* region,
                 StateMappingCache* mapping_cache,
                 ChunkSpoolWriter* writer, SchematicParseResult* result,
                 uint64_t completed_before, const RegionOverlayIndex& overlays,
                 uint32_t region_index, std::string* error) {
    if (!region) return false;
    const uint32_t bits = paletteBits(region->palette.size());
    PackedPaletteReader packed(block_states_path, bits);
    return routeRegionPacked(options, region, &packed, mapping_cache, writer,
                              result, completed_before, overlays, region_index,
                              error);
}

bool routeRegionStream(Reader& reader, const SchematicParseOptions& options,
                       RegionData* region, StateMappingCache* mapping_cache,
                       ChunkSpoolWriter* writer, SchematicParseResult* result,
                       uint64_t completed_before,
                       const RegionOverlayIndex& overlays,
                       uint32_t region_index, std::string* error) {
    uint32_t word_count = 0;
    if (!readBlockStateLongCount(reader, kMaximumStreamingBlockStatesBytes,
                                 &word_count, error)) return false;
    region->block_state_longs = word_count;
    const uint32_t bits = paletteBits(region->palette.size());
    StreamPackedPaletteReader packed(&reader, bits);
    return routeRegionPacked(options, region, &packed, mapping_cache, writer,
                              result, completed_before, overlays, region_index,
                              error);
}

bool flushRegionCommandBlocks(const SchematicParseOptions& options,
                               RegionData* region, const BlockBounds& bounds,
                               const RegionOverlayIndex& overlays,
                               uint32_t region_index,
                               SchematicParseResult* result, std::string* error) {
    if (!region || !result) return false;
    if (!options.command_block_sink) return true;
    std::array<uint64_t, 3> size{};
    for (size_t axis = 0; axis < size.size(); ++axis) {
        if (!absoluteDimension(region->signed_size[axis], &size[axis])) {
            if (error) *error = "litematic command-block region has invalid dimensions";
            return false;
        }
    }
    uint64_t layer_area = 0;
    if (!checkedMultiply(size[0], size[2], &layer_area)) {
        if (error) *error = "litematic command-block region dimensions overflow";
        return false;
    }
    for (size_t record_index = 0; record_index < region->command_block_records.size(); ++record_index) {
        CommandBlockRecord& record = region->command_block_records[record_index];
        if (record.x < 0 || record.y < 0 || record.z < 0 ||
            static_cast<uint64_t>(record.x) >= size[0] ||
            static_cast<uint64_t>(record.y) >= size[1] ||
            static_cast<uint64_t>(record.z) >= size[2]) {
            // Tile entity coordinates outside a region are not command-block
            // payloads for that region.  Ignore them rather than making an
            // otherwise valid litematic fail because of unrelated NBT.
            continue;
        }
        const uint64_t local_index = static_cast<uint64_t>(record.x) +
            static_cast<uint64_t>(record.z) * size[0] +
            static_cast<uint64_t>(record.y) * layer_area;
        const auto shell = region->command_block_modes.find(local_index);
        // TileEntities can outlive a replaced shell in hand-edited files.  In
        // addition, a later overlapping region owns this coordinate even when
        // it contains palette air.  Never send an editor packet unless this
        // region still contributes a real command-block shell at the final
        // world coordinate.
        if (shell == region->command_block_modes.end()) {
            ++result->omitted_command_block_data_count;
            continue;
        }
        // Palette-backed BlockStates describe the final shell, including its
        // conditional bit.  Some litematic block-entity records omit that
        // field, so always use the shell values for the later update packet.
        applyCommandBlockShellState(&record, shell->second);
        if (!isValidCommandBlockMode(record.mode)) {
            if (error) *error = "litematic command-block mode is invalid";
            return false;
        }
        const int64_t world_x = static_cast<int64_t>(bounds.min_x) + record.x;
        const int64_t world_y = static_cast<int64_t>(bounds.min_y) + record.y;
        const int64_t world_z = static_cast<int64_t>(bounds.min_z) + record.z;
        if (world_x < std::numeric_limits<int32_t>::min() ||
            world_x > std::numeric_limits<int32_t>::max() ||
            world_y < std::numeric_limits<int32_t>::min() ||
            world_y > std::numeric_limits<int32_t>::max() ||
            world_z < std::numeric_limits<int32_t>::min() ||
            world_z > std::numeric_limits<int32_t>::max()) {
            if (error) *error = "litematic command-block coordinates exceed the supported world range";
            return false;
        }
        if (overlays.isCoveredByLater(static_cast<int32_t>(world_x),
                                      static_cast<int32_t>(world_y),
                                      static_cast<int32_t>(world_z),
                                      region_index)) {
            ++result->omitted_command_block_data_count;
            continue;
        }
        record.x = static_cast<int32_t>(world_x);
        record.y = static_cast<int32_t>(world_y);
        record.z = static_cast<int32_t>(world_z);
        if (!options.command_block_sink(record, error)) {
            if (error && error->empty()) *error = "litematic command-block sink rejected a payload";
            return false;
        }
        ++result->command_block_payload_count;
    }
    region->command_block_records.clear();
    region->command_block_mode_explicit.clear();
    region->command_block_modes.clear();
    region->command_block_text_bytes = 0;
    return true;
}

bool flushRegionContainerItems(const SchematicParseOptions& options,
                               RegionData* region, const BlockBounds& bounds,
                               const RegionOverlayIndex& overlays,
                               uint32_t region_index,
                               SchematicParseResult* result, std::string* error) {
    if (!region || !result) return false;
    if (!options.container_item_sink) return true;
    std::array<uint64_t, 3> size{};
    for (size_t axis = 0; axis < size.size(); ++axis) {
        if (!absoluteDimension(region->signed_size[axis], &size[axis])) {
            if (error) *error = "litematic container region has invalid dimensions";
            return false;
        }
    }
    uint64_t layer_area = 0;
    if (!checkedMultiply(size[0], size[2], &layer_area)) {
        if (error) *error = "litematic container region dimensions overflow";
        return false;
    }
    for (ContainerItemRecord& item : region->container_item_records) {
        if (item.x < 0 || item.y < 0 || item.z < 0 ||
            static_cast<uint64_t>(item.x) >= size[0] ||
            static_cast<uint64_t>(item.y) >= size[1] ||
            static_cast<uint64_t>(item.z) >= size[2]) {
            ++result->omitted_block_entity_count;
            continue;
        }
        const uint64_t local_index = static_cast<uint64_t>(item.x) +
            static_cast<uint64_t>(item.z) * size[0] +
            static_cast<uint64_t>(item.y) * layer_area;
        const auto shell = region->container_shells.find(local_index);
        // Do not turn a stale TileEntity (or item data left after a palette
        // replacement) into a write against an arbitrary block.
        if (shell == region->container_shells.end()) {
            ++result->omitted_block_entity_count;
            continue;
        }
        const int64_t world_x = static_cast<int64_t>(bounds.min_x) + item.x;
        const int64_t world_y = static_cast<int64_t>(bounds.min_y) + item.y;
        const int64_t world_z = static_cast<int64_t>(bounds.min_z) + item.z;
        if (world_x < std::numeric_limits<int32_t>::min() ||
            world_x > std::numeric_limits<int32_t>::max() ||
            world_y < std::numeric_limits<int32_t>::min() ||
            world_y > std::numeric_limits<int32_t>::max() ||
            world_z < std::numeric_limits<int32_t>::min() ||
            world_z > std::numeric_limits<int32_t>::max()) {
            if (error) *error = "litematic container coordinates exceed the supported world range";
            return false;
        }
        if (overlays.isCoveredByLater(static_cast<int32_t>(world_x),
                                      static_cast<int32_t>(world_y),
                                      static_cast<int32_t>(world_z),
                                      region_index)) {
            ++result->omitted_block_entity_count;
            continue;
        }
        item.x = static_cast<int32_t>(world_x);
        item.y = static_cast<int32_t>(world_y);
        item.z = static_cast<int32_t>(world_z);
        item.expected_container_id = shell->second;
        if (!options.container_item_sink(item, error)) {
            if (error && error->empty()) {
                *error = "litematic container-item sink rejected a payload";
            }
            return false;
        }
        ++result->container_item_payload_count;
    }
    region->container_item_records.clear();
    region->container_shells.clear();
    return true;
}

bool flushRegionEntities(const SchematicParseOptions& options, RegionData* region,
                         const BlockBounds& bounds,
                         const RegionOverlayIndex& overlays,
                         uint32_t region_index,
                         SchematicParseResult* result, std::string* error) {
    if (!region || !result) return false;
    if (!options.entity_sink) return true;
    std::array<uint64_t, 3> size{};
    for (size_t axis = 0; axis < size.size(); ++axis) {
        if (!absoluteDimension(region->signed_size[axis], &size[axis])) {
            if (error) *error = "litematic entity region has invalid dimensions";
            return false;
        }
    }
    constexpr double kMaximumWorldCoordinate = 30000000.0;
    for (EntityRecord& entity : region->entity_records) {
        // Entity positions in a litematic region are local, including when the
        // source Size is negative.  Packed blocks and deferred payloads both
        // begin at bounds.min, never at the signed selection corner.
        if (!std::isfinite(entity.x) || !std::isfinite(entity.y) ||
            !std::isfinite(entity.z) || !std::isfinite(entity.yaw) ||
            !std::isfinite(entity.pitch) || entity.x < 0.0 || entity.y < 0.0 ||
            entity.z < 0.0 || entity.x >= static_cast<double>(size[0]) ||
            entity.y >= static_cast<double>(size[1]) ||
            entity.z >= static_cast<double>(size[2])) {
            ++result->omitted_entity_count;
            continue;
        }
        entity.x += static_cast<double>(bounds.min_x);
        entity.y += static_cast<double>(bounds.min_y);
        entity.z += static_cast<double>(bounds.min_z);
        if (!std::isfinite(entity.x) || !std::isfinite(entity.y) ||
            !std::isfinite(entity.z) || std::abs(entity.x) > kMaximumWorldCoordinate ||
            std::abs(entity.y) > kMaximumWorldCoordinate ||
            std::abs(entity.z) > kMaximumWorldCoordinate) {
            ++result->omitted_entity_count;
            continue;
        }
        const double floor_x = std::floor(entity.x);
        const double floor_y = std::floor(entity.y);
        const double floor_z = std::floor(entity.z);
        if (floor_x < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
            floor_x > static_cast<double>(std::numeric_limits<int32_t>::max()) ||
            floor_y < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
            floor_y > static_cast<double>(std::numeric_limits<int32_t>::max()) ||
            floor_z < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
            floor_z > static_cast<double>(std::numeric_limits<int32_t>::max())) {
            ++result->omitted_entity_count;
            continue;
        }
        if (overlays.isCoveredByLater(static_cast<int32_t>(floor_x),
                                      static_cast<int32_t>(floor_y),
                                      static_cast<int32_t>(floor_z),
                                      region_index)) {
            ++result->omitted_entity_count;
            continue;
        }
        if (!options.entity_sink(entity, error)) {
            if (error && error->empty()) *error = "litematic entity sink rejected a payload";
            return false;
        }
        ++result->entity_payload_count;
    }
    region->entity_records.clear();
    return true;
}

// A handful of exporters write BlockStates before the region geometry and
// palette.  The normal streaming path cannot decode that order immediately,
// but retaining a giant raw array is worse than an extra sequential gzip pass.
// The main parse already validates the source to EOF, so this replay only needs
// to reach and decode the requested region.
bool replayDeferredRegionBlockStates(const SchematicParseOptions& options,
                                     uint32_t target_region_index,
                                     RegionData* target_region,
                                     StateMappingCache* mapping_cache,
                                     ChunkSpoolWriter* writer,
                                     SchematicParseResult* result,
                                     uint64_t completed_before,
                                     const RegionOverlayIndex& overlays,
                                     std::string* error) {
    if (!target_region) return false;
    Reader reader(options.source_path);
    if (!reader.valid()) {
        if (error) *error = "cannot reopen litematic for deferred BlockStates replay";
        return false;
    }
    uint8_t root_tag = 0;
    std::string root_name;
    if (!reader.u8(&root_tag) || root_tag != Compound || !reader.string(&root_name)) {
        if (error) *error = "litematic NBT root changed during deferred BlockStates replay";
        return false;
    }
    bool saw_regions = false;
    while (true) {
        if (cancellationRequested(options.cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) break;
        if (tag == End) break;
        if (!reader.string(&name)) break;
        if (name != "Regions") {
            if (!skipPayload(reader, tag, 1, options.cancellation_requested, error)) {
                return false;
            }
            continue;
        }
        if (tag != Compound || saw_regions) break;
        saw_regions = true;
        for (uint32_t region_index = 0;; ++region_index) {
            if (cancellationRequested(options.cancellation_requested, error)) return false;
            uint8_t region_tag = 0;
            std::string region_name;
            if (!reader.u8(&region_tag)) break;
            if (region_tag == End) break;
            if (region_index >= kMaximumRegions || !reader.string(&region_name) ||
                region_name.empty() || region_tag != Compound) break;
            if (region_index != target_region_index) {
                if (!skipPayload(reader, Compound, 1,
                                 options.cancellation_requested, error)) return false;
                continue;
            }
            if (region_name != target_region->name) {
                if (error) *error = "litematic regions changed during deferred BlockStates replay";
                return false;
            }
            bool routed = false;
            std::unordered_set<std::string> fields;
            while (true) {
                if (cancellationRequested(options.cancellation_requested, error)) return false;
                uint8_t field_tag = 0;
                std::string field_name;
                if (!reader.u8(&field_tag)) break;
                if (field_tag == End) break;
                if (!reader.string(&field_name) || !fields.insert(field_name).second) break;
                if (field_name != "BlockStates") {
                    if (!skipPayload(reader, field_tag, 1,
                                     options.cancellation_requested, error)) return false;
                    continue;
                }
                if (field_tag != LongArray || routed ||
                    !routeRegionStream(reader, options, target_region,
                                       mapping_cache, writer, result,
                                       completed_before, overlays,
                                       target_region_index, error)) {
                    if (error && error->empty()) {
                        *error = "invalid deferred litematic BlockStates payload";
                    }
                    return false;
                }
                routed = true;
            }
            if (!routed) {
                if (error) *error = "litematic region has no deferred BlockStates payload";
                return false;
            }
            return true;
        }
        break;
    }
    if (error && error->empty()) {
        *error = "litematic deferred BlockStates region was not found";
    }
    return false;
}

bool scanRegions(Reader& reader, const SchematicParseOptions& options,
                 RegionOverlayIndex* overlays, bool* has_regions,
                 std::string* error) {
    std::unordered_set<std::string> region_names;
    while (true) {
        if (cancellationRequested(options.cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) return true;
        if (region_names.size() >= kMaximumRegions) {
            if (error) *error = "litematic contains too many regions";
            return false;
        }
        if (!reader.string(&name) || name.empty() || tag != Compound ||
            !region_names.insert(name).second) return false;
        RegionData region;
        region.name = std::move(name);
        if (!scanRegionPayload(reader, &region,
                                options.cancellation_requested, error)) {
            if (error && error->empty()) *error = "invalid litematic region payload";
            return false;
        }
        BlockBounds bounds;
        if (!regionBounds(options, region, &bounds, error)) return false;
        overlays->add(std::move(region.name), bounds);
        *has_regions = true;
    }
}

bool parseRegions(Reader& reader, const SchematicParseOptions& options,
                   const std::string& block_states_path,
                   const RegionOverlayIndex& overlays, ChunkSpoolWriter* writer,
                   StateMappingCache* mapping_cache,
                   SchematicParseResult* result, std::string* error) {
    std::unordered_set<std::string> region_names;
    uint64_t routed_voxels = 0;
    uint32_t region_index = 0;
    while (true) {
        if (cancellationRequested(options.cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) return false;
        if (tag == End) {
            if (region_index != overlays.size()) {
                if (error) *error = "litematic regions changed while parsing";
                return false;
            }
            return true;
        }
        if (region_names.size() >= kMaximumRegions || region_index >= overlays.size()) {
            if (error) *error = "litematic regions changed while parsing";
            return false;
        }
        if (!reader.string(&name) || name.empty() || tag != Compound ||
            !region_names.insert(name).second) return false;
        RegionData region;
        region.name = std::move(name);
        bool routed_directly = false;
        bool requires_replay = false;
        DirectBlockStatesConsumer direct_consumer;
        direct_consumer = [&](Reader& block_state_reader,
                              RegionData* parsed_region,
                              std::string* route_error) {
            BlockBounds bounds;
            if (!regionBounds(options, *parsed_region, &bounds, route_error)) {
                return false;
            }
            if (!overlays.matches(region_index, parsed_region->name, bounds)) {
                if (route_error) {
                    *route_error = "litematic regions changed while parsing";
                }
                return false;
            }
            return routeRegionStream(
                block_state_reader, options, parsed_region, mapping_cache,
                writer, result, routed_voxels, overlays, region_index,
                route_error);
        };
        if (!parseRegionPayload(reader, options, block_states_path, &region,
                                 direct_consumer, &routed_directly,
                                 &requires_replay,
                                 options.cancellation_requested, error)) {
            if (error && error->empty()) *error = "invalid litematic region payload";
            return false;
        }
        BlockBounds bounds;
        if (!regionBounds(options, region, &bounds, error)) return false;
        if (!overlays.matches(region_index, region.name, bounds)) {
            if (error) *error = "litematic regions changed while parsing";
            return false;
        }
        if (!routed_directly) {
            if (requires_replay) {
                if (!replayDeferredRegionBlockStates(
                        options, region_index, &region, mapping_cache, writer,
                        result, routed_voxels, overlays, error)) return false;
            } else if (!routeRegion(options, block_states_path, &region, mapping_cache,
                                     writer, result, routed_voxels, overlays,
                                    region_index, error)) {
                return false;
            }
        }
        // Delay command records until this region's BlockStates have routed so
        // generic Java command-block entity ids can inherit the actual shell
        // mode (impulse/repeat/chain) from the palette-backed block state.
        if (!flushRegionCommandBlocks(options, &region, bounds, overlays,
                                      region_index, result, error)) {
            if (writer) writer->discard();
            return false;
        }
        if (!flushRegionContainerItems(options, &region, bounds, overlays,
                                       region_index, result, error)) {
            if (writer) writer->discard();
            return false;
        }
        if (!flushRegionEntities(options, &region, bounds, overlays,
                                 region_index, result, error)) {
            if (writer) writer->discard();
            return false;
        }
        result->filtered_enchantment_count += region.filtered_enchantment_count;
        result->omitted_entity_count += region.omitted_entity_count;
        ++result->source_region_count;
        routed_voxels = result->source_voxel_count;
        ++region_index;
    }
}

bool scanLitematicRegions(const SchematicParseOptions& options,
                          RegionOverlayIndex* overlays, std::string* error) {
    Reader reader(options.source_path);
    if (!reader.valid()) {
        if (error) *error = "cannot open litematic";
        return false;
    }
    uint8_t root_tag = 0;
    std::string root_name;
    if (!reader.u8(&root_tag) || root_tag != Compound || !reader.string(&root_name)) {
        if (error) *error = "litematic NBT root is not a compound";
        return false;
    }
    bool has_regions = false;
    bool saw_regions_tag = false;
    std::unordered_set<std::string> root_names;
    uint32_t root_field_count = 0;
    while (true) {
        if (cancellationRequested(options.cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) {
            if (error) *error = "unexpected end of litematic NBT";
            return false;
        }
        if (tag == End) break;
        if (++root_field_count > kMaximumCompoundEntries) {
            if (error) *error = "litematic root contains too many tags";
            return false;
        }
        if (!reader.string(&name) || !root_names.insert(name).second) {
            if (error) *error = "invalid or duplicate litematic root tag";
            return false;
        }
        if (name == "Regions") {
            if (tag != Compound || saw_regions_tag ||
                !scanRegions(reader, options, overlays, &has_regions, error)) {
                if (error && error->empty()) *error = "invalid litematic Regions compound";
                return false;
            }
            saw_regions_tag = true;
        } else if (!skipPayload(reader, tag, 1,
                                 options.cancellation_requested, error)) {
            if (error && error->empty()) *error = "invalid litematic root payload";
            return false;
        }
    }
    if (!saw_regions_tag || !has_regions) {
        if (error) *error = "litematic contains no regions";
        return false;
    }
    if (!reader.exhausted()) {
        if (error) *error = "trailing data after litematic NBT root";
        return false;
    }
    if (!overlays->build()) {
        if (error) *error = "litematic contains no regions";
        return false;
    }
    return true;
}

}  // namespace

bool LitematicParser::parse(const SchematicParseOptions& options,
                            const BlockMapper& mapper,
                            SchematicParseResult* result,
                            std::string* error) const {
    SchematicParseOptions resolved_options = options;
    if (!resolved_options.state_block_resolver) {
        resolved_options.state_block_resolver = [&mapper](std::string_view state) {
            return mapper.mapSpongeState(state);
        };
    }
    return parse(resolved_options, result, error);
}

bool LitematicParser::parse(const SchematicParseOptions& options,
                            SchematicParseResult* result,
                            std::string* error) const {
    if (error) error->clear();
    if (!result || options.source_path.empty() || options.spool_directory.empty() ||
        options.chunk_size <= 0) {
        if (error) *error = "invalid litematic parse options";
        return false;
    }
    *result = {};
    if (!options.state_block_resolver) {
        if (error) *error = "no palette-state block resolver was provided";
        return false;
    }
    if (cancellationRequested(options.cancellation_requested, error)) return false;
    if (!ensureDirectory(options.spool_directory)) {
        if (error) *error = "cannot create litematic spool directory";
        return false;
    }

    const std::string block_states_path =
        options.spool_directory + "/_litematic_blockstates.raw";
    struct RawCleanup {
        explicit RawCleanup(std::string path) : path(std::move(path)) {}
        ~RawCleanup() { std::remove(path.c_str()); }
        std::string path;
    } raw_cleanup(block_states_path);

    reportProgress(options, SchematicParseStage::ReadingSource);
    RegionOverlayIndex overlays;
    if (!scanLitematicRegions(options, &overlays, error)) return false;

    // Re-open the compressed source after the geometry-only pass.  The second
    // pass remains streaming and writes only the final, non-overridden voxels
    // into the existing chunk spools.
    reportProgress(options, SchematicParseStage::ReadingSource);
    Reader reader(options.source_path);
    if (!reader.valid()) {
        if (error) *error = "cannot open litematic";
        return false;
    }
    uint8_t root_tag = 0;
    std::string root_name;
    if (!reader.u8(&root_tag) || root_tag != Compound || !reader.string(&root_name)) {
        if (error) *error = "litematic NBT root is not a compound";
        return false;
    }

    std::unique_ptr<ChunkSpoolWriter> writer;
    if (!options.block_sink) {
        writer = std::make_unique<ChunkSpoolWriter>(
            options.spool_directory, options.chunk_size,
            options.maximum_chunk_descriptors);
    }
    StateMappingCache mapping_cache;
    mapping_cache.reserve(256);
    bool saw_regions_tag = false;
    std::unordered_set<std::string> root_names;
    uint32_t root_field_count = 0;
    while (true) {
        if (cancellationRequested(options.cancellation_requested, error)) return false;
        uint8_t tag = 0;
        std::string name;
        if (!reader.u8(&tag)) {
            if (error) *error = "unexpected end of litematic NBT";
            return false;
        }
        if (tag == End) break;
        if (++root_field_count > kMaximumCompoundEntries) {
            if (error) *error = "litematic root contains too many tags";
            return false;
        }
        if (!reader.string(&name) || !root_names.insert(name).second) {
            if (error) *error = "invalid or duplicate litematic root tag";
            return false;
        }
        if (name == "Regions") {
            if (tag != Compound || saw_regions_tag ||
                !parseRegions(reader, options, block_states_path,
                              overlays, writer.get(), &mapping_cache, result,
                              error)) {
                if (error && error->empty()) *error = "invalid litematic Regions compound";
                return false;
            }
            saw_regions_tag = true;
        } else if (!skipPayload(reader, tag, 1,
                                options.cancellation_requested, error)) {
            if (error && error->empty()) *error = "invalid litematic root payload";
            return false;
        }
    }
    if (!saw_regions_tag) {
        if (error) *error = "litematic contains no regions";
        return false;
    }
    if (!reader.exhausted()) {
        if (error) *error = "trailing data after litematic NBT root";
        return false;
    }

    // Regions may be disjoint. A full-source auxiliary layer (for example a
    // deny foundation) must still be scheduled across the global footprint,
    // rather than leaving gaps between separately routed region bounds.
    if (writer && options.include_source_volume && result->source_region_count > 1 &&
        !writer->includeMissingFootprint(result->source_volume_bounds, error,
                                         options.cancellation_requested)) {
        return false;
    }

    if (!checkedSourceOffset(result->source_volume_bounds.min_x, options.base_x,
                             &result->source_offset_x) ||
        !checkedSourceOffset(result->source_volume_bounds.min_y, options.base_y,
                             &result->source_offset_y) ||
        !checkedSourceOffset(result->source_volume_bounds.min_z, options.base_z,
                             &result->source_offset_z)) {
        if (error) *error = "litematic source offset exceeds the supported range";
        return false;
    }
    reportProgress(options, SchematicParseStage::FinalizingSpools, 0, 0);
    if (writer) {
        result->chunks = writer->finish(
            error, options.cancellation_requested,
            [&](uint64_t completed, uint64_t total) {
                reportProgress(options, SchematicParseStage::FinalizingSpools,
                               completed, total);
            });
    }
    if (error && !error->empty()) return false;
    if (cancellationRequested(options.cancellation_requested, error)) {
        if (writer) writer->discard();
        result->chunks.clear();
        return false;
    }
    reportProgress(options, SchematicParseStage::FinalizingSpools,
                   result->imported_block_count, result->imported_block_count);
    return options.block_sink ? true : !result->chunks.empty();
}

}  // namespace build_import
