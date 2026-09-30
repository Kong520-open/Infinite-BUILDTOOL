#include "ItemExtraMapMetadata.h"

#include <cstddef>
#include <cstring>
#include <utility>

namespace build_import {
namespace {

constexpr size_t kMaximumItemExtraBytes = 5U * 1024U * 1024U;
constexpr uint32_t kMaximumDepth = 16U;
constexpr uint32_t kMaximumTags = 16384U;
constexpr uint16_t kMaximumNbtStringBytes = 4096U;
constexpr size_t kMaximumUsableNameBytes = 256U;

bool validDisplayUtf8(std::string_view text) {
    for (size_t cursor = 0; cursor < text.size();) {
        const uint8_t first = static_cast<uint8_t>(text[cursor]);
        if (first < 0x80U) {
            if (first < 0x20U || first == 0x7FU) return false;
            ++cursor;
            continue;
        }
        uint32_t length = 0;
        if (first >= 0xC2U && first <= 0xDFU) length = 2U;
        else if (first >= 0xE0U && first <= 0xEFU) length = 3U;
        else if (first >= 0xF0U && first <= 0xF4U) length = 4U;
        else return false;
        if (length > text.size() - cursor) return false;
        const uint8_t second = static_cast<uint8_t>(text[cursor + 1U]);
        if ((second & 0xC0U) != 0x80U ||
            (first == 0xE0U && second < 0xA0U) ||
            (first == 0xEDU && second >= 0xA0U) ||
            (first == 0xF0U && second < 0x90U) ||
            (first == 0xF4U && second >= 0x90U)) return false;
        for (uint32_t index = 2U; index < length; ++index) {
            if ((static_cast<uint8_t>(text[cursor + index]) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        cursor += length;
    }
    return true;
}

class Reader {
public:
    explicit Reader(std::string_view input) : input_(input) {}

    bool parse(ItemExtraMapMetadata* output) {
        if (!output || input_.size() > kMaximumItemExtraBytes ||
            input_.size() < 6U ||
            static_cast<uint8_t>(input_[0]) != 0xFFU ||
            static_cast<uint8_t>(input_[1]) != 0xFFU ||
            static_cast<uint8_t>(input_[2]) != 1U) return false;
        cursor_ = 3U;
        uint8_t root_type = 0;
        std::string_view root_name;
        if (!byte(&root_type) || root_type != 10U || !name(&root_name) ||
            !compound(0U, Scope::Root) || !stringList() || !stringList() ||
            cursor_ != input_.size()) return false;

        output->has_map_uuid = map_uuid_seen_ && map_uuid_ != -1;
        output->map_uuid = output->has_map_uuid ? map_uuid_ : -1;
        if (name_unusable_) {
            output->name_status = MapItemNameStatus::Unusable;
        } else if (name_seen_ && !name_candidate_.empty()) {
            output->name_status = MapItemNameStatus::Present;
            output->name_source = name_source_;
            output->display_name.assign(name_candidate_.data(), name_candidate_.size());
        }
        return true;
    }

private:
    enum class Scope : uint8_t { Root, Display, Other };

    bool available(size_t count) const {
        return cursor_ <= input_.size() && count <= input_.size() - cursor_;
    }

    bool skip(size_t count) {
        if (!available(count)) return false;
        cursor_ += count;
        return true;
    }

    bool byte(uint8_t* output) {
        if (!output || !available(1U)) return false;
        *output = static_cast<uint8_t>(input_[cursor_++]);
        return true;
    }

    bool le16(uint16_t* output) {
        if (!output || !available(2U)) return false;
        *output = static_cast<uint16_t>(static_cast<uint8_t>(input_[cursor_])) |
                  static_cast<uint16_t>(static_cast<uint8_t>(input_[cursor_ + 1U]) << 8U);
        cursor_ += 2U;
        return true;
    }

    bool le32(uint32_t* output) {
        if (!output || !available(4U)) return false;
        uint32_t value = 0;
        for (uint32_t index = 0; index < 4U; ++index) {
            value |= static_cast<uint32_t>(static_cast<uint8_t>(input_[cursor_ + index]))
                << (index * 8U);
        }
        cursor_ += 4U;
        *output = value;
        return true;
    }

    bool le64(int64_t* output) {
        if (!output || !available(8U)) return false;
        uint64_t value = 0;
        for (uint32_t index = 0; index < 8U; ++index) {
            value |= static_cast<uint64_t>(static_cast<uint8_t>(input_[cursor_ + index]))
                << (index * 8U);
        }
        cursor_ += 8U;
        std::memcpy(output, &value, sizeof(value));
        return true;
    }

    bool name(std::string_view* output) {
        uint16_t length = 0;
        if (!output || !le16(&length) || length > kMaximumNbtStringBytes ||
            !available(length)) return false;
        *output = input_.substr(cursor_, length);
        cursor_ += length;
        return true;
    }

    bool stringList() {
        uint32_t count = 0;
        if (!le32(&count) || count > (input_.size() - cursor_) / 2U) return false;
        for (uint32_t index = 0; index < count; ++index) {
            std::string_view ignored;
            if (!name(&ignored)) return false;
        }
        return true;
    }

    void observeName(MapItemNameSource source, std::string_view candidate) {
        if (name_seen_) name_unusable_ = true;
        name_seen_ = true;
        if (candidate.size() > kMaximumUsableNameBytes ||
            !validDisplayUtf8(candidate)) name_unusable_ = true;
        if (!name_unusable_) {
            name_source_ = source;
            name_candidate_ = candidate;
        }
    }

    bool payload(uint8_t type, uint32_t depth) {
        if (depth > kMaximumDepth) return false;
        uint32_t length = 0;
        switch (type) {
            case 1U: return skip(1U);
            case 2U: return skip(2U);
            case 3U: case 5U: return skip(4U);
            case 4U: case 6U: return skip(8U);
            case 7U: case 11U: case 12U: {
                if (!le32(&length) || length > input_.size()) return false;
                const size_t width = type == 7U ? 1U : type == 11U ? 4U : 8U;
                return length <= (input_.size() - cursor_) / width &&
                    skip(static_cast<size_t>(length) * width);
            }
            case 8U: {
                std::string_view ignored;
                return name(&ignored);
            }
            case 9U: {
                uint8_t element_type = 0;
                if (!byte(&element_type) || !le32(&length) ||
                    length > input_.size() || (length != 0U &&
                    (element_type == 0U || element_type > 12U))) return false;
                for (uint32_t index = 0; index < length; ++index) {
                    if (++tag_count_ > kMaximumTags ||
                        !payload(element_type, depth + 1U)) return false;
                }
                return true;
            }
            case 10U: return compound(depth + 1U, Scope::Other);
            default: return false;
        }
    }

    bool compound(uint32_t depth, Scope scope) {
        if (depth > kMaximumDepth) return false;
        for (;;) {
            uint8_t type = 0;
            if (!byte(&type)) return false;
            if (type == 0U) return true;
            std::string_view field;
            if (type > 12U || !name(&field) || ++tag_count_ > kMaximumTags) {
                return false;
            }
            if (scope == Scope::Root && field == "map_uuid") {
                if (type != 4U || map_uuid_seen_ || !le64(&map_uuid_)) return false;
                map_uuid_seen_ = true;
            } else if ((scope == Scope::Root && field == "CustomName") ||
                       (scope == Scope::Display && field == "Name")) {
                const MapItemNameSource source = scope == Scope::Root
                    ? MapItemNameSource::RootCustomName : MapItemNameSource::DisplayName;
                if (type == 8U) {
                    std::string_view value;
                    if (!name(&value)) return false;
                    observeName(source, value);
                } else {
                    name_unusable_ = true;
                    if (!payload(type, depth)) return false;
                }
            } else if (scope == Scope::Root && field == "display") {
                if (display_seen_) name_unusable_ = true;
                display_seen_ = true;
                if (type == 10U) {
                    if (!compound(depth + 1U, Scope::Display)) return false;
                } else {
                    name_unusable_ = true;
                    if (!payload(type, depth)) return false;
                }
            } else if (!payload(type, depth)) {
                return false;
            }
        }
    }

    std::string_view input_;
    size_t cursor_ = 0;
    uint32_t tag_count_ = 0;
    bool map_uuid_seen_ = false;
    int64_t map_uuid_ = -1;
    bool display_seen_ = false;
    bool name_seen_ = false;
    bool name_unusable_ = false;
    MapItemNameSource name_source_ = MapItemNameSource::None;
    std::string_view name_candidate_;
};

}  // namespace

bool ParseItemExtraMapMetadata(std::string_view item_extra,
                               ItemExtraMapMetadata* output) {
    if (!output) return false;
    *output = {};
    ItemExtraMapMetadata parsed;
    if (!Reader(item_extra).parse(&parsed)) return false;
    *output = std::move(parsed);
    return true;
}

}  // namespace build_import
