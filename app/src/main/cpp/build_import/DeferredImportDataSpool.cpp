#include "DeferredImportDataSpool.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <utility>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace build_import {
namespace {

constexpr uint32_t kContainerMagic = 0x31534344U;  // "DCS1"
constexpr uint32_t kEntityMagic = 0x31534544U;     // "DES1"
constexpr uint32_t kSignMagic = 0x31535344U;       // "DSS1"
constexpr uint16_t kMaximumIdentifierBytes = 256;
constexpr uint16_t kMaximumEntityNameBytes = 128;
constexpr uint8_t kMaximumEnchantmentsPerItem = 32;

#pragma pack(push, 1)
struct FileHeader {
    uint32_t magic = 0;
    uint32_t version = 1;
    uint64_t record_count = 0;
    uint64_t data_bytes = 0;
};

struct ContainerRecordHeader {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint16_t slot = 0;
    uint16_t count = 0;
    uint16_t aux = 0;
    uint16_t container_id_bytes = 0;
    uint16_t item_id_bytes = 0;
    uint8_t enchantment_count = 0;
    uint8_t reserved[3]{};
};

struct EntityRecordHeader {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    float yaw = 0.0F;
    float pitch = 0.0F;
    uint16_t entity_id_bytes = 0;
    uint16_t name_bytes = 0;
};

struct SignRecordHeader {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint32_t flags = 0;
    int32_t front_text_color = 0;
    int32_t back_text_color = 0;
    uint32_t front_text_bytes = 0;
    uint32_t back_text_bytes = 0;
    uint16_t sign_id_bytes = 0;
    uint16_t expected_aux = 0;
};
#pragma pack(pop)

static_assert(sizeof(FileHeader) == 24, "unexpected deferred spool file header");
static_assert(sizeof(ContainerRecordHeader) == 26,
              "unexpected deferred container record header");
static_assert(sizeof(EntityRecordHeader) == 36,
               "unexpected deferred entity record header");
static_assert(sizeof(SignRecordHeader) == 36,
              "unexpected deferred sign record header");

constexpr uint32_t kSignFrontPresent = 1U << 0U;
constexpr uint32_t kSignFrontHasText = 1U << 1U;
constexpr uint32_t kSignFrontHasColor = 1U << 2U;
constexpr uint32_t kSignFrontHasIgnoreLighting = 1U << 3U;
constexpr uint32_t kSignFrontIgnoreLighting = 1U << 4U;
constexpr uint32_t kSignFrontHasPersistFormatting = 1U << 5U;
constexpr uint32_t kSignFrontPersistFormatting = 1U << 6U;
constexpr uint32_t kSignFrontHasHideGlowOutline = 1U << 7U;
constexpr uint32_t kSignFrontHideGlowOutline = 1U << 8U;
constexpr uint32_t kSignFrontHasLegacyResolved = 1U << 9U;
constexpr uint32_t kSignFrontLegacyResolved = 1U << 10U;
constexpr uint32_t kSignBackPresent = 1U << 11U;
constexpr uint32_t kSignBackHasText = 1U << 12U;
constexpr uint32_t kSignBackHasColor = 1U << 13U;
constexpr uint32_t kSignBackHasIgnoreLighting = 1U << 14U;
constexpr uint32_t kSignBackIgnoreLighting = 1U << 15U;
constexpr uint32_t kSignBackHasPersistFormatting = 1U << 16U;
constexpr uint32_t kSignBackPersistFormatting = 1U << 17U;
constexpr uint32_t kSignBackHasHideGlowOutline = 1U << 18U;
constexpr uint32_t kSignBackHideGlowOutline = 1U << 19U;
constexpr uint32_t kSignBackHasLegacyResolved = 1U << 20U;
constexpr uint32_t kSignBackLegacyResolved = 1U << 21U;
constexpr uint32_t kSignHasIsWaxed = 1U << 22U;
constexpr uint32_t kSignIsWaxed = 1U << 23U;
constexpr uint32_t kSignHasExpectedAux = 1U << 24U;
constexpr uint32_t kKnownSignFlags = (1U << 25U) - 1U;

bool validFileHeader(const FileHeader& header, uint32_t expected_magic,
                     uint64_t maximum_records, uint64_t maximum_data_bytes,
                     uint64_t minimum_record_bytes) {
    if (header.magic != expected_magic || header.version != 1 ||
        header.record_count > maximum_records || header.data_bytes > maximum_data_bytes ||
        minimum_record_bytes == 0) {
        return false;
    }
    if (header.record_count == 0) return header.data_bytes == 0;
    // Divide instead of multiplying so a hostile record count can never wrap
    // while we check that there is room for every fixed record header.
    return header.data_bytes / minimum_record_bytes >= header.record_count;
}

bool fail(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}

int createDirectory(const char* path) {
#if defined(_WIN32)
    return ::_mkdir(path);
#else
    return ::mkdir(path, 0700);
#endif
}

bool ensureDirectory(const std::string& directory) {
    if (directory.empty()) return false;
    std::string partial;
    for (size_t index = 0; index <= directory.size(); ++index) {
        if (index < directory.size() && directory[index] != '/' && directory[index] != '\\') {
            partial.push_back(directory[index]);
            continue;
        }
        if (!partial.empty() && partial != "." &&
            createDirectory(partial.c_str()) != 0 && errno != EEXIST) {
            return false;
        }
        if (index != directory.size()) {
            const char separator = directory[index];
            if (partial.empty() && (separator == '/' || separator == '\\')) {
                partial.push_back(separator);
            } else if (!partial.empty() && partial.back() != '/' && partial.back() != '\\') {
                partial.push_back(separator);
            }
        }
    }
    return true;
}

std::string joinPath(const std::string& directory, const std::string& file_name) {
    if (directory.empty()) return file_name;
    if (directory.back() == '/' || directory.back() == '\\') return directory + file_name;
    return directory + "/" + file_name;
}

bool replaceFile(const std::string& temporary, const std::string& destination) {
#if defined(_WIN32)
    std::remove(destination.c_str());
#endif
    return std::rename(temporary.c_str(), destination.c_str()) == 0;
}

bool safeNamespaceToken(const std::string& value, size_t maximum) {
    if (value.empty() || value.size() > maximum) return false;
    for (const unsigned char character : value) {
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '_' ||
              character == ':' || character == '-' || character == '.')) {
            return false;
        }
    }
    return true;
}

void lowercaseAscii(std::string* value) {
    if (!value) return;
    for (char& character : *value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte >= 'A' && byte <= 'Z') character = static_cast<char>(byte - 'A' + 'a');
    }
}

bool normalizeMinecraftIdentifier(std::string* identifier, size_t maximum) {
    if (!identifier) return false;
    lowercaseAscii(identifier);
    if (identifier->find(':') == std::string::npos) identifier->insert(0, "minecraft:");
    if (identifier->rfind("minecraft:", 0) != 0 ||
        identifier->find(':', std::string("minecraft:").size()) != std::string::npos) return false;
    return safeNamespaceToken(*identifier, maximum);
}

std::string leafOf(std::string_view identifier) {
    const size_t separator = identifier.rfind(':');
    return std::string(separator == std::string_view::npos ? identifier : identifier.substr(separator + 1));
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool validUtf8(std::string_view value) {
    size_t cursor = 0;
    while (cursor < value.size()) {
        const uint8_t first = static_cast<uint8_t>(value[cursor]);
        if (first == 0U) return false;
        if (first <= 0x7FU) {
            ++cursor;
            continue;
        }
        auto continuation = [&value](size_t index) {
            return index < value.size() &&
                (static_cast<uint8_t>(value[index]) & 0xC0U) == 0x80U;
        };
        if (first >= 0xC2U && first <= 0xDFU) {
            if (!continuation(cursor + 1U)) return false;
            cursor += 2U;
            continue;
        }
        if (first >= 0xE0U && first <= 0xEFU) {
            if (!continuation(cursor + 1U) || !continuation(cursor + 2U)) return false;
            const uint8_t second = static_cast<uint8_t>(value[cursor + 1U]);
            if ((first == 0xE0U && second < 0xA0U) ||
                (first == 0xEDU && second >= 0xA0U)) return false;
            cursor += 3U;
            continue;
        }
        if (first >= 0xF0U && first <= 0xF4U) {
            if (!continuation(cursor + 1U) || !continuation(cursor + 2U) ||
                !continuation(cursor + 3U)) return false;
            const uint8_t second = static_cast<uint8_t>(value[cursor + 1U]);
            if ((first == 0xF0U && second < 0x90U) ||
                (first == 0xF4U && second >= 0x90U)) return false;
            cursor += 4U;
            continue;
        }
        return false;
    }
    return true;
}

const std::map<std::string, uint8_t>& enchantmentLevels() {
    static const std::map<std::string, uint8_t> values{
        {"minecraft:aqua_affinity", 1}, {"minecraft:bane_of_arthropods", 5},
        {"minecraft:binding", 1}, {"minecraft:binding_curse", 1},
        {"minecraft:blast_protection", 4}, {"minecraft:breach", 4},
        {"minecraft:channeling", 1}, {"minecraft:density", 5},
        {"minecraft:depth_strider", 3}, {"minecraft:efficiency", 5},
        {"minecraft:feather_falling", 4}, {"minecraft:fire_aspect", 2},
        {"minecraft:fire_protection", 4}, {"minecraft:flame", 1},
        {"minecraft:fortune", 3}, {"minecraft:frost_walker", 2},
        {"minecraft:impaling", 5}, {"minecraft:infinity", 1},
        {"minecraft:knockback", 2}, {"minecraft:looting", 3},
        {"minecraft:loyalty", 3}, {"minecraft:luck_of_the_sea", 3},
        {"minecraft:lure", 3}, {"minecraft:mending", 1},
        {"minecraft:multishot", 1}, {"minecraft:piercing", 4},
        {"minecraft:power", 5}, {"minecraft:projectile_protection", 4},
        {"minecraft:protection", 4}, {"minecraft:punch", 2},
        {"minecraft:quick_charge", 3}, {"minecraft:respiration", 3},
        {"minecraft:riptide", 3}, {"minecraft:sharpness", 5},
        {"minecraft:silk_touch", 1}, {"minecraft:smite", 5},
        {"minecraft:soul_speed", 3}, {"minecraft:swift_sneak", 3},
        {"minecraft:thorns", 3}, {"minecraft:unbreaking", 3},
        {"minecraft:vanishing", 1}, {"minecraft:vanishing_curse", 1},
        {"minecraft:wind_burst", 3},
    };
    return values;
}

bool validContainerRecord(const ContainerItemRecord& record, std::string* error) {
    std::string container_id = record.expected_container_id;
    std::string item_id = record.item_id;
    if (record.slot > 255 || record.count == 0 || record.count > 64 ||
        record.enchantments.size() > kMaximumEnchantmentsPerItem ||
        !normalizeDeferredItemIdentifier(&container_id) ||
        container_id != record.expected_container_id ||
        !normalizeDeferredItemToken(&item_id) || item_id != record.item_id) {
        return fail(error, "invalid deferred container record");
    }
    for (const DeferredEnchantment& enchantment : record.enchantments) {
        DeferredEnchantment normalized;
        std::string id = enchantment.id;
        if (!normalizeDeferredEnchantment(&id, enchantment.level, &normalized) ||
            normalized.id != enchantment.id || normalized.level != enchantment.level) {
            return fail(error, "invalid deferred enchantment record");
        }
    }
    return true;
}

bool validEntityRecord(const EntityRecord& record, std::string* error) {
    std::string id = record.entity_id;
    std::string name = record.custom_name;
    if (!normalizeDeferredEntityIdentifier(&id) || id != record.entity_id ||
        !normalizeDeferredEntityName(&name) || name != record.custom_name ||
        !std::isfinite(record.x) || !std::isfinite(record.y) || !std::isfinite(record.z) ||
        !std::isfinite(record.yaw) || !std::isfinite(record.pitch)) {
        return fail(error, "invalid deferred entity record");
    }
    return true;
}

bool validSignFace(const SignTextRecord& face) {
    if (!face.present) {
        return !face.has_text && face.text.empty() && !face.has_text_color &&
            face.text_color == 0 && !face.has_ignore_lighting &&
            !face.ignore_lighting && !face.has_persist_formatting &&
            !face.persist_formatting && !face.has_hide_glow_outline &&
            !face.hide_glow_outline &&
            !face.has_text_ignore_legacy_bug_resolved &&
            !face.text_ignore_legacy_bug_resolved;
    }
    return (face.has_text || face.text.empty()) &&
        (!face.has_text || (face.text.size() <= SignSpoolWriter::kMaximumTextBytes &&
                            validUtf8(face.text))) &&
        (face.has_text_color || face.text_color == 0) &&
        (face.has_ignore_lighting || !face.ignore_lighting) &&
        (face.has_persist_formatting || !face.persist_formatting) &&
        (face.has_hide_glow_outline || !face.hide_glow_outline) &&
        (face.has_text_ignore_legacy_bug_resolved ||
         !face.text_ignore_legacy_bug_resolved);
}

bool validSignRecord(const SignRecord& record, std::string* error) {
    std::string sign_id = record.expected_sign_id;
    if (!normalizeDeferredItemIdentifier(&sign_id) ||
        sign_id != record.expected_sign_id || !deferredSignIdentifier(sign_id) ||
        !validSignFace(record.front) || !validSignFace(record.back) ||
        (!record.has_expected_aux && record.expected_aux != 0U) ||
        (!record.has_is_waxed && record.is_waxed) ||
        (!record.front.present && !record.back.present && !record.has_is_waxed)) {
        return fail(error, "invalid deferred sign record");
    }
    return true;
}

uint32_t signRecordFlags(const SignRecord& record) {
    const SignTextRecord& front = record.front;
    const SignTextRecord& back = record.back;
    uint32_t flags = 0;
    if (front.present) flags |= kSignFrontPresent;
    if (front.has_text) flags |= kSignFrontHasText;
    if (front.has_text_color) flags |= kSignFrontHasColor;
    if (front.has_ignore_lighting) flags |= kSignFrontHasIgnoreLighting;
    if (front.ignore_lighting) flags |= kSignFrontIgnoreLighting;
    if (front.has_persist_formatting) flags |= kSignFrontHasPersistFormatting;
    if (front.persist_formatting) flags |= kSignFrontPersistFormatting;
    if (front.has_hide_glow_outline) flags |= kSignFrontHasHideGlowOutline;
    if (front.hide_glow_outline) flags |= kSignFrontHideGlowOutline;
    if (front.has_text_ignore_legacy_bug_resolved) flags |= kSignFrontHasLegacyResolved;
    if (front.text_ignore_legacy_bug_resolved) flags |= kSignFrontLegacyResolved;
    if (back.present) flags |= kSignBackPresent;
    if (back.has_text) flags |= kSignBackHasText;
    if (back.has_text_color) flags |= kSignBackHasColor;
    if (back.has_ignore_lighting) flags |= kSignBackHasIgnoreLighting;
    if (back.ignore_lighting) flags |= kSignBackIgnoreLighting;
    if (back.has_persist_formatting) flags |= kSignBackHasPersistFormatting;
    if (back.persist_formatting) flags |= kSignBackPersistFormatting;
    if (back.has_hide_glow_outline) flags |= kSignBackHasHideGlowOutline;
    if (back.hide_glow_outline) flags |= kSignBackHideGlowOutline;
    if (back.has_text_ignore_legacy_bug_resolved) flags |= kSignBackHasLegacyResolved;
    if (back.text_ignore_legacy_bug_resolved) flags |= kSignBackLegacyResolved;
    if (record.has_is_waxed) flags |= kSignHasIsWaxed;
    if (record.is_waxed) flags |= kSignIsWaxed;
    if (record.has_expected_aux) flags |= kSignHasExpectedAux;
    return flags;
}

void applySignRecordFlags(uint32_t flags, SignRecord* record) {
    SignTextRecord& front = record->front;
    SignTextRecord& back = record->back;
    front.present = (flags & kSignFrontPresent) != 0U;
    front.has_text = (flags & kSignFrontHasText) != 0U;
    front.has_text_color = (flags & kSignFrontHasColor) != 0U;
    front.has_ignore_lighting = (flags & kSignFrontHasIgnoreLighting) != 0U;
    front.ignore_lighting = (flags & kSignFrontIgnoreLighting) != 0U;
    front.has_persist_formatting = (flags & kSignFrontHasPersistFormatting) != 0U;
    front.persist_formatting = (flags & kSignFrontPersistFormatting) != 0U;
    front.has_hide_glow_outline = (flags & kSignFrontHasHideGlowOutline) != 0U;
    front.hide_glow_outline = (flags & kSignFrontHideGlowOutline) != 0U;
    front.has_text_ignore_legacy_bug_resolved =
        (flags & kSignFrontHasLegacyResolved) != 0U;
    front.text_ignore_legacy_bug_resolved =
        (flags & kSignFrontLegacyResolved) != 0U;
    back.present = (flags & kSignBackPresent) != 0U;
    back.has_text = (flags & kSignBackHasText) != 0U;
    back.has_text_color = (flags & kSignBackHasColor) != 0U;
    back.has_ignore_lighting = (flags & kSignBackHasIgnoreLighting) != 0U;
    back.ignore_lighting = (flags & kSignBackIgnoreLighting) != 0U;
    back.has_persist_formatting = (flags & kSignBackHasPersistFormatting) != 0U;
    back.persist_formatting = (flags & kSignBackPersistFormatting) != 0U;
    back.has_hide_glow_outline = (flags & kSignBackHasHideGlowOutline) != 0U;
    back.hide_glow_outline = (flags & kSignBackHideGlowOutline) != 0U;
    back.has_text_ignore_legacy_bug_resolved =
        (flags & kSignBackHasLegacyResolved) != 0U;
    back.text_ignore_legacy_bug_resolved =
        (flags & kSignBackLegacyResolved) != 0U;
    record->has_is_waxed = (flags & kSignHasIsWaxed) != 0U;
    record->is_waxed = (flags & kSignIsWaxed) != 0U;
    record->has_expected_aux = (flags & kSignHasExpectedAux) != 0U;
}

}  // namespace

bool normalizeDeferredItemIdentifier(std::string* identifier) {
    if (!identifier) return false;
    lowercaseAscii(identifier);
    if (identifier->find(':') == std::string::npos) {
        identifier->insert(0, "minecraft:");
    }
    if (identifier->empty() || identifier->size() > kMaximumIdentifierBytes) return false;
    const size_t separator = identifier->find(':');
    if (separator == 0U || separator + 1U >= identifier->size() ||
        identifier->find(':', separator + 1U) != std::string::npos) {
        return false;
    }
    for (size_t index = 0; index < identifier->size(); ++index) {
        if (index == separator) continue;
        const unsigned char character = static_cast<unsigned char>((*identifier)[index]);
        const bool common = (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '_' ||
            character == '-' || character == '.';
        if (!common && !(index > separator && character == '/')) return false;
    }
    return true;
}

bool parseDeferredNumericItemIdentifier(std::string_view identifier,
                                        int32_t* numeric_id) {
    if (identifier.empty()) return false;
    size_t cursor = 0;
    bool negative = false;
    if (identifier.front() == '-') {
        negative = true;
        cursor = 1;
    }
    if (cursor == identifier.size()) return false;

    uint64_t magnitude = 0;
    const uint64_t limit = negative
        ? static_cast<uint64_t>(
              -(static_cast<int64_t>(std::numeric_limits<int32_t>::min())))
        : static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    for (; cursor < identifier.size(); ++cursor) {
        const char value = identifier[cursor];
        if (value < '0' || value > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(value - '0');
        if (magnitude > (limit - digit) / 10U) return false;
        magnitude = magnitude * 10U + digit;
    }
    if (magnitude == 0U) return false;

    const int64_t signed_value = negative
        ? -static_cast<int64_t>(magnitude)
        : static_cast<int64_t>(magnitude);
    if (numeric_id) *numeric_id = static_cast<int32_t>(signed_value);
    return true;
}

bool normalizeDeferredItemToken(std::string* identifier) {
    int32_t numeric_id = 0;
    if (!identifier || parseDeferredNumericItemIdentifier(*identifier, &numeric_id)) {
        return false;
    }
    return normalizeDeferredItemIdentifier(identifier);
}

bool normalizeDeferredEntityIdentifier(std::string* identifier) {
    if (!normalizeMinecraftIdentifier(identifier, kMaximumIdentifierBytes)) return false;
    const std::string leaf = leafOf(*identifier);
    // These are data carriers or players rather than recoverable blueprint
    // actors.  Never turn their NBT into a summon command.
    return leaf != "player" && leaf != "item" && leaf != "experience_orb" &&
           leaf != "area_effect_cloud" && leaf != "lightning_bolt" && leaf != "tnt";
}

bool normalizeDeferredEnchantmentIdentifier(std::string* identifier) {
    if (!normalizeMinecraftIdentifier(identifier, 64)) return false;
    return enchantmentLevels().find(*identifier) != enchantmentLevels().end();
}

bool normalizeDeferredEntityName(std::string* value) {
    if (!value) return false;
    std::string clean;
    clean.reserve(std::min<size_t>(value->size(), kMaximumEntityNameBytes));
    for (const unsigned char byte : *value) {
        if (byte < 0x20 || byte == 0x7F) continue;
        if (clean.size() >= kMaximumEntityNameBytes) break;
        clean.push_back(static_cast<char>(byte));
    }
    *value = std::move(clean);
    return true;
}

bool normalizeDeferredEnchantment(std::string* identifier, int64_t level,
                                  DeferredEnchantment* output) {
    if (!identifier || !output || level <= 0 || level > 255 ||
        !normalizeDeferredEnchantmentIdentifier(identifier)) return false;
    const auto found = enchantmentLevels().find(*identifier);
    if (found == enchantmentLevels().end() || level > found->second) return false;
    output->id = *identifier;
    output->level = static_cast<uint8_t>(level);
    return true;
}

bool deferredContainerIdentifier(std::string_view identifier) {
    const std::string leaf = leafOf(identifier);
    return leaf == "chest" || leaf == "trapped_chest" || leaf == "barrel" ||
           leaf == "hopper" || leaf == "dispenser" || leaf == "dropper" ||
           leaf == "furnace" || leaf == "lit_furnace" ||
           leaf == "blast_furnace" || leaf == "lit_blast_furnace" ||
           leaf == "smoker" || leaf == "lit_smoker" ||
           leaf == "brewing_stand" || leaf == "shulker_box" ||
           (leaf.size() > 12 && leaf.compare(leaf.size() - 12, 12, "_shulker_box") == 0) ||
           leaf == "decorated_pot";
}

bool packetCapturableContainerIdentifier(std::string_view identifier) {
    const std::string leaf = leafOf(identifier);
    // A decorated pot stores an item, but interacting with it does not open a
    // ContainerOpen window and may insert the held item into the source build.
    return leaf != "decorated_pot" && deferredContainerIdentifier(identifier);
}

bool deferredSignIdentifier(std::string_view identifier) {
    const size_t state = identifier.find('[');
    if (state != std::string_view::npos) identifier = identifier.substr(0, state);
    const std::string leaf = leafOf(identifier);
    return leaf == "sign" || leaf == "standing_sign" || leaf == "wall_sign" ||
        endsWith(leaf, "_sign");
}

bool deferredHangingSignIdentifier(std::string_view identifier) {
    const size_t state = identifier.find('[');
    if (state != std::string_view::npos) identifier = identifier.substr(0, state);
    const std::string leaf = leafOf(identifier);
    return leaf == "hanging_sign" || endsWith(leaf, "_hanging_sign");
}

// Return the material encoded by a sign shell when the identifier carries
// one. Legacy generic IDs (standing_sign/wall_sign) intentionally return an
// empty material because the client may expose those for any wood family.
bool signShellIdentity(std::string_view identifier, bool* hanging,
                       std::string* material) {
    if (!hanging || !material) return false;
    *hanging = false;
    material->clear();
    const size_t state = identifier.find('[');
    if (state != std::string_view::npos) identifier = identifier.substr(0, state);
    const size_t separator = identifier.rfind(':');
    std::string leaf = std::string(separator == std::string_view::npos
        ? identifier : identifier.substr(separator + 1));
    if (leaf.empty()) return false;
    for (char& character : leaf) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    const auto removeSuffix = [&](std::string_view suffix, bool is_hanging) {
        if (leaf.size() < suffix.size() ||
            leaf.compare(leaf.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return false;
        }
        leaf.resize(leaf.size() - suffix.size());
        *hanging = is_hanging;
        if (leaf == "darkoak") leaf = "dark_oak";
        *material = std::move(leaf);
        return true;
    };
    if (leaf == "sign" || leaf == "standing_sign" || leaf == "wall_sign" ||
        leaf == "hanging_sign" || leaf == "wall_hanging_sign") {
        *hanging = leaf == "hanging_sign" || leaf == "wall_hanging_sign";
        return true;
    }
    // Check the longer hanging suffixes first: they also end in `_sign`.
    if (removeSuffix("_wall_hanging_sign", true) ||
        removeSuffix("_hanging_sign", true) ||
        removeSuffix("_standing_sign", false) ||
        removeSuffix("_wall_sign", false) ||
        removeSuffix("_sign", false)) {
        return true;
    }
    return false;
}

bool deferredSignShellMatches(std::string_view expected_identifier,
                              std::string_view actual_identifier) {
    bool expected_hanging = false;
    bool actual_hanging = false;
    std::string expected_material;
    std::string actual_material;
    if (!signShellIdentity(expected_identifier, &expected_hanging, &expected_material) ||
        !signShellIdentity(actual_identifier, &actual_hanging, &actual_material) ||
        expected_hanging != actual_hanging) {
        return false;
    }
    // If both sides identify a concrete wood family, reject a material swap.
    // A generic legacy shell remains acceptable because it does not expose
    // enough information to distinguish oak from the other sign families.
    return expected_material.empty() || actual_material.empty() ||
        expected_material == actual_material;
}

SignSpoolWriter::SignSpoolWriter(std::string directory, std::string file_name)
    : directory_(std::move(directory)),
      spool_path_(joinPath(directory_, file_name.empty() ? kFileName : file_name)),
      temporary_path_(spool_path_ + ".tmp") {}

SignSpoolWriter::~SignSpoolWriter() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!finished_) discardLocked(false);
}

bool SignSpoolWriter::ensureOpenLocked(std::string* error) {
    if (opened_) return !failed_;
    if (failed_ || finished_) return fail(error, "sign spool writer is closed");
    if (!ensureDirectory(directory_)) {
        failed_ = true;
        return fail(error, "cannot create sign spool directory");
    }
    std::remove(temporary_path_.c_str());
    output_.open(temporary_path_, std::ios::binary | std::ios::trunc);
    if (!output_) {
        failed_ = true;
        return fail(error, "cannot create sign spool");
    }
    const FileHeader header{kSignMagic, kVersion, 0, 0};
    output_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!output_) {
        failed_ = true;
        return fail(error, "cannot initialize sign spool");
    }
    opened_ = true;
    return true;
}

bool SignSpoolWriter::append(const SignRecord& record, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!validSignRecord(record, error) || !ensureOpenLocked(error)) return false;
    const uint64_t size = sizeof(SignRecordHeader) + record.expected_sign_id.size() +
        record.front.text.size() + record.back.text.size();
    if (record_count_ >= kMaximumRecords || size > kMaximumDataBytes - data_bytes_) {
        failed_ = true;
        return fail(error, "sign sidecar exceeds the safety limit");
    }
    SignRecordHeader header;
    header.x = record.x;
    header.y = record.y;
    header.z = record.z;
    header.flags = signRecordFlags(record);
    header.front_text_color = record.front.text_color;
    header.back_text_color = record.back.text_color;
    header.front_text_bytes = static_cast<uint32_t>(record.front.text.size());
    header.back_text_bytes = static_cast<uint32_t>(record.back.text.size());
    header.sign_id_bytes = static_cast<uint16_t>(record.expected_sign_id.size());
    header.expected_aux = record.expected_aux;
    output_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output_.write(record.expected_sign_id.data(),
                  static_cast<std::streamsize>(record.expected_sign_id.size()));
    output_.write(record.front.text.data(),
                  static_cast<std::streamsize>(record.front.text.size()));
    output_.write(record.back.text.data(),
                  static_cast<std::streamsize>(record.back.text.size()));
    if (!output_) {
        failed_ = true;
        return fail(error, "cannot append sign spool record");
    }
    ++record_count_;
    data_bytes_ += size;
    return true;
}

bool SignSpoolWriter::finish(std::string* spool_path, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (spool_path) spool_path->clear();
    if (failed_) return fail(error, "sign spool writer failed");
    if (finished_) {
        if (spool_path) *spool_path = spool_path_;
        return true;
    }
    if (!ensureOpenLocked(error)) return false;
    const FileHeader header{kSignMagic, kVersion, record_count_, data_bytes_};
    output_.seekp(0, std::ios::beg);
    output_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output_.flush();
    output_.close();
    if (!output_ || !replaceFile(temporary_path_, spool_path_)) {
        failed_ = true;
        std::remove(temporary_path_.c_str());
        return fail(error, "cannot finalize sign spool");
    }
    finished_ = true;
    if (spool_path) *spool_path = spool_path_;
    return true;
}

void SignSpoolWriter::discardLocked(bool remove_final) {
    if (output_.is_open()) output_.close();
    std::remove(temporary_path_.c_str());
    if (remove_final) std::remove(spool_path_.c_str());
}

void SignSpoolWriter::discard() {
    std::lock_guard<std::mutex> lock(mutex_);
    discardLocked(true);
    opened_ = false;
    finished_ = false;
    failed_ = false;
    record_count_ = 0;
    data_bytes_ = 0;
}

uint64_t SignSpoolWriter::recordCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return record_count_;
}

SignSpoolReader::SignSpoolReader(std::string path) : path_(std::move(path)) {
    std::lock_guard<std::mutex> lock(mutex_);
    input_.open(path_, std::ios::binary);
    if (!input_ || !readHeaderLocked()) failLocked();
}

void SignSpoolReader::failLocked() {
    failed_ = true;
    valid_ = false;
}

bool SignSpoolReader::readHeaderLocked() {
    FileHeader header;
    input_.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!input_ || !validFileHeader(header, kSignMagic,
                                    SignSpoolWriter::kMaximumRecords,
                                    SignSpoolWriter::kMaximumDataBytes,
                                    sizeof(SignRecordHeader)) ||
        header.data_bytes >
            static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    record_count_ = header.record_count;
    data_bytes_ = header.data_bytes;
    data_start_ = input_.tellg();
    if (data_start_ < std::streampos(0)) return false;
    input_.seekg(0, std::ios::end);
    const std::streampos end = input_.tellg();
    if (end < data_start_ ||
        static_cast<uint64_t>(end - data_start_) != data_bytes_) return false;
    input_.seekg(data_start_, std::ios::beg);
    consumed_bytes_ = 0;
    cursor_ = 0;
    valid_ = static_cast<bool>(input_);
    return valid_;
}

bool SignSpoolReader::readBytesLocked(void* output, size_t count) {
    if (count > data_bytes_ - consumed_bytes_ ||
        count > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        failLocked();
        return false;
    }
    input_.read(reinterpret_cast<char*>(output), static_cast<std::streamsize>(count));
    if (!input_) {
        failLocked();
        return false;
    }
    consumed_bytes_ += count;
    return true;
}

bool SignSpoolReader::readRecordLocked(SignRecord* record) {
    if (!record || cursor_ >= record_count_) return false;
    SignRecordHeader header;
    if (!readBytesLocked(&header, sizeof(header))) return false;
    if ((header.flags & ~kKnownSignFlags) != 0U ||
        ((header.flags & kSignHasExpectedAux) == 0U && header.expected_aux != 0U) ||
        header.sign_id_bytes == 0U ||
        header.sign_id_bytes > kMaximumIdentifierBytes ||
        header.front_text_bytes > SignSpoolWriter::kMaximumTextBytes ||
        header.back_text_bytes > SignSpoolWriter::kMaximumTextBytes) {
        failLocked();
        return false;
    }
    SignRecord parsed;
    parsed.x = header.x;
    parsed.y = header.y;
    parsed.z = header.z;
    parsed.front.text_color = header.front_text_color;
    parsed.back.text_color = header.back_text_color;
    applySignRecordFlags(header.flags, &parsed);
    parsed.expected_aux = header.expected_aux;
    try {
        parsed.expected_sign_id.assign(header.sign_id_bytes, '\0');
        parsed.front.text.assign(header.front_text_bytes, '\0');
        parsed.back.text.assign(header.back_text_bytes, '\0');
    } catch (const std::bad_alloc&) {
        failLocked();
        return false;
    }
    if (!readBytesLocked(&parsed.expected_sign_id[0], parsed.expected_sign_id.size()) ||
        (!parsed.front.text.empty() &&
         !readBytesLocked(&parsed.front.text[0], parsed.front.text.size())) ||
        (!parsed.back.text.empty() &&
         !readBytesLocked(&parsed.back.text[0], parsed.back.text.size())) ||
        !validSignRecord(parsed, nullptr)) {
        failLocked();
        return false;
    }
    ++cursor_;
    if (cursor_ == record_count_ && consumed_bytes_ != data_bytes_) {
        failLocked();
        return false;
    }
    *record = std::move(parsed);
    return true;
}

bool SignSpoolReader::skipRecordLocked() {
    SignRecord ignored;
    return readRecordLocked(&ignored);
}

bool SignSpoolReader::resetLocked() {
    if (!input_.is_open()) {
        failLocked();
        return false;
    }
    input_.clear();
    input_.seekg(data_start_, std::ios::beg);
    if (!input_) {
        failLocked();
        return false;
    }
    consumed_bytes_ = 0;
    cursor_ = 0;
    valid_ = true;
    failed_ = false;
    return true;
}

bool SignSpoolReader::seekRecord(uint64_t record_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_ || record_index > record_count_ || !resetLocked()) return false;
    while (cursor_ < record_index && skipRecordLocked()) {}
    return valid_ && !failed_ && cursor_ == record_index;
}

std::optional<SignRecord> SignSpoolReader::next() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_ || failed_ || cursor_ >= record_count_) return std::nullopt;
    SignRecord record;
    return readRecordLocked(&record)
        ? std::optional<SignRecord>(std::move(record))
        : std::nullopt;
}

EntitySpoolWriter::EntitySpoolWriter(std::string directory, std::string file_name)
    : directory_(std::move(directory)),
      spool_path_(joinPath(directory_, file_name.empty() ? kFileName : file_name)),
      temporary_path_(spool_path_ + ".tmp") {}

EntitySpoolWriter::~EntitySpoolWriter() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!finished_) discardLocked(false);
}

bool EntitySpoolWriter::ensureOpenLocked(std::string* error) {
    if (opened_) return !failed_;
    if (failed_ || finished_) return fail(error, "entity spool writer is closed");
    if (!ensureDirectory(directory_)) { failed_ = true; return fail(error, "cannot create entity spool directory"); }
    std::remove(temporary_path_.c_str());
    output_.open(temporary_path_, std::ios::binary | std::ios::trunc);
    if (!output_) { failed_ = true; return fail(error, "cannot create entity spool"); }
    const FileHeader header{kEntityMagic, kVersion, 0, 0};
    output_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!output_) { failed_ = true; return fail(error, "cannot initialize entity spool"); }
    opened_ = true;
    return true;
}

bool EntitySpoolWriter::append(const EntityRecord& record, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!validEntityRecord(record, error) || !ensureOpenLocked(error)) return false;
    const uint64_t size = sizeof(EntityRecordHeader) + record.entity_id.size() + record.custom_name.size();
    if (record_count_ >= kMaximumRecords || size > kMaximumDataBytes - data_bytes_) {
        failed_ = true;
        return fail(error, "entity sidecar exceeds the safety limit");
    }
    EntityRecordHeader header;
    header.x = record.x; header.y = record.y; header.z = record.z;
    header.yaw = record.yaw; header.pitch = record.pitch;
    header.entity_id_bytes = static_cast<uint16_t>(record.entity_id.size());
    header.name_bytes = static_cast<uint16_t>(record.custom_name.size());
    output_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output_.write(record.entity_id.data(), static_cast<std::streamsize>(record.entity_id.size()));
    output_.write(record.custom_name.data(), static_cast<std::streamsize>(record.custom_name.size()));
    if (!output_) { failed_ = true; return fail(error, "cannot append entity spool record"); }
    ++record_count_;
    data_bytes_ += size;
    return true;
}

bool EntitySpoolWriter::finish(std::string* spool_path, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (spool_path) spool_path->clear();
    if (failed_) return fail(error, "entity spool writer failed");
    if (finished_) { if (spool_path) *spool_path = spool_path_; return true; }
    if (!ensureOpenLocked(error)) return false;
    const FileHeader header{kEntityMagic, kVersion, record_count_, data_bytes_};
    output_.seekp(0, std::ios::beg);
    output_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output_.flush(); output_.close();
    if (!output_ || !replaceFile(temporary_path_, spool_path_)) {
        failed_ = true; std::remove(temporary_path_.c_str());
        return fail(error, "cannot finalize entity spool");
    }
    finished_ = true;
    if (spool_path) *spool_path = spool_path_;
    return true;
}

void EntitySpoolWriter::discardLocked(bool remove_final) {
    if (output_.is_open()) output_.close();
    std::remove(temporary_path_.c_str());
    if (remove_final) std::remove(spool_path_.c_str());
}

void EntitySpoolWriter::discard() {
    std::lock_guard<std::mutex> lock(mutex_);
    discardLocked(true);
    opened_ = false; finished_ = false; failed_ = false; record_count_ = 0; data_bytes_ = 0;
}

uint64_t EntitySpoolWriter::recordCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return record_count_;
}

EntitySpoolReader::EntitySpoolReader(std::string path) : path_(std::move(path)) {
    std::lock_guard<std::mutex> lock(mutex_);
    input_.open(path_, std::ios::binary);
    if (!input_ || !readHeaderLocked()) failLocked();
}

void EntitySpoolReader::failLocked() { failed_ = true; valid_ = false; }

bool EntitySpoolReader::readHeaderLocked() {
    FileHeader header;
    input_.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!input_ || !validFileHeader(header, kEntityMagic,
                                    EntitySpoolWriter::kMaximumRecords,
                                    EntitySpoolWriter::kMaximumDataBytes,
                                    sizeof(EntityRecordHeader)) ||
        header.data_bytes > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    record_count_ = header.record_count; data_bytes_ = header.data_bytes; data_start_ = input_.tellg();
    if (data_start_ < std::streampos(0)) return false;
    input_.seekg(0, std::ios::end); const std::streampos end = input_.tellg();
    if (end < data_start_ || static_cast<uint64_t>(end - data_start_) != data_bytes_) return false;
    input_.seekg(data_start_, std::ios::beg); consumed_bytes_ = 0; cursor_ = 0;
    valid_ = static_cast<bool>(input_); return valid_;
}

bool EntitySpoolReader::readBytesLocked(void* output, size_t count) {
    if (count > data_bytes_ - consumed_bytes_ ||
        count > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        failLocked();
        return false;
    }
    input_.read(reinterpret_cast<char*>(output), static_cast<std::streamsize>(count));
    if (!input_) { failLocked(); return false; }
    consumed_bytes_ += count; return true;
}

bool EntitySpoolReader::skipBytesLocked(uint64_t count) {
    if (count > data_bytes_ - consumed_bytes_ || count > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        failLocked(); return false;
    }
    input_.seekg(static_cast<std::streamoff>(count), std::ios::cur);
    if (!input_) { failLocked(); return false; }
    consumed_bytes_ += count; return true;
}

bool EntitySpoolReader::readRecordLocked(EntityRecord* record) {
    if (!record || cursor_ >= record_count_) return false;
    EntityRecordHeader header;
    if (!readBytesLocked(&header, sizeof(header))) return false;
    if (header.entity_id_bytes == 0 || header.entity_id_bytes > kMaximumIdentifierBytes ||
        header.name_bytes > kMaximumEntityNameBytes) {
        failLocked();
        return false;
    }
    EntityRecord parsed;
    parsed.x = header.x; parsed.y = header.y; parsed.z = header.z;
    parsed.yaw = header.yaw; parsed.pitch = header.pitch;
    parsed.entity_id.assign(header.entity_id_bytes, '\0');
    parsed.custom_name.assign(header.name_bytes, '\0');
    if (!readBytesLocked(&parsed.entity_id[0], parsed.entity_id.size()) ||
        (!parsed.custom_name.empty() && !readBytesLocked(&parsed.custom_name[0], parsed.custom_name.size())) ||
        !validEntityRecord(parsed, nullptr)) { failLocked(); return false; }
    ++cursor_;
    if (cursor_ == record_count_ && consumed_bytes_ != data_bytes_) {
        failLocked();
        return false;
    }
    *record = std::move(parsed);
    return true;
}

bool EntitySpoolReader::skipRecordLocked() { EntityRecord ignored; return readRecordLocked(&ignored); }

bool EntitySpoolReader::resetLocked() {
    if (!input_.is_open()) { failLocked(); return false; }
    input_.clear(); input_.seekg(data_start_, std::ios::beg);
    if (!input_) { failLocked(); return false; }
    consumed_bytes_ = 0;
    cursor_ = 0;
    valid_ = true;
    failed_ = false;
    return true;
}

bool EntitySpoolReader::seekRecord(uint64_t record_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_ || record_index > record_count_ || !resetLocked()) return false;
    while (cursor_ < record_index && skipRecordLocked()) {}
    return valid_ && !failed_ && cursor_ == record_index;
}

std::optional<EntityRecord> EntitySpoolReader::next() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_ || failed_ || cursor_ >= record_count_) return std::nullopt;
    EntityRecord record;
    return readRecordLocked(&record) ? std::optional<EntityRecord>(std::move(record)) : std::nullopt;
}
ContainerItemSpoolWriter::ContainerItemSpoolWriter(std::string directory, std::string file_name)
    : directory_(std::move(directory)),
      spool_path_(joinPath(directory_, file_name.empty() ? kFileName : file_name)),
      temporary_path_(spool_path_ + ".tmp") {}
ContainerItemSpoolWriter::~ContainerItemSpoolWriter() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!finished_) discardLocked(false);
}
bool ContainerItemSpoolWriter::ensureOpenLocked(std::string* error) {
    if (opened_) return !failed_;
    if (failed_ || finished_) return fail(error, "container spool writer is closed");
    if (!ensureDirectory(directory_)) { failed_ = true; return fail(error, "cannot create container spool directory"); }
    std::remove(temporary_path_.c_str());
    output_.open(temporary_path_, std::ios::binary | std::ios::trunc);
    if (!output_) { failed_ = true; return fail(error, "cannot create container spool"); }
    const FileHeader header{kContainerMagic, kVersion, 0, 0};
    output_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!output_) { failed_ = true; return fail(error, "cannot initialize container spool"); }
    opened_ = true;
    return true;
}
bool ContainerItemSpoolWriter::append(const ContainerItemRecord& record, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!validContainerRecord(record, error) || !ensureOpenLocked(error)) return false;
    uint64_t enchantment_bytes = 0;
    for (const DeferredEnchantment& enchantment : record.enchantments) {
        enchantment_bytes += sizeof(uint16_t) + enchantment.id.size() + sizeof(uint8_t);
    }
    const uint64_t size = sizeof(ContainerRecordHeader) + record.expected_container_id.size() +
        record.item_id.size() + enchantment_bytes;
    if (record_count_ >= kMaximumRecords || size > kMaximumDataBytes - data_bytes_) {
        failed_ = true;
        return fail(error, "container sidecar exceeds the safety limit");
    }
    ContainerRecordHeader header;
    header.x = record.x; header.y = record.y; header.z = record.z;
    header.slot = record.slot; header.count = record.count; header.aux = record.aux;
    header.container_id_bytes = static_cast<uint16_t>(record.expected_container_id.size());
    header.item_id_bytes = static_cast<uint16_t>(record.item_id.size());
    header.enchantment_count = static_cast<uint8_t>(record.enchantments.size());
    output_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output_.write(record.expected_container_id.data(),
                  static_cast<std::streamsize>(record.expected_container_id.size()));
    output_.write(record.item_id.data(), static_cast<std::streamsize>(record.item_id.size()));
    for (const DeferredEnchantment& enchantment : record.enchantments) {
        const uint16_t id_bytes = static_cast<uint16_t>(enchantment.id.size());
        output_.write(reinterpret_cast<const char*>(&id_bytes), sizeof(id_bytes));
        output_.write(enchantment.id.data(), static_cast<std::streamsize>(enchantment.id.size()));
        output_.write(reinterpret_cast<const char*>(&enchantment.level), sizeof(enchantment.level));
    }
    if (!output_) { failed_ = true; return fail(error, "cannot append container spool record"); }
    ++record_count_;
    data_bytes_ += size;
    return true;
}
bool ContainerItemSpoolWriter::finish(std::string* spool_path, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (spool_path) spool_path->clear();
    if (failed_) return fail(error, "container spool writer failed");
    if (finished_) { if (spool_path) *spool_path = spool_path_; return true; }
    if (!ensureOpenLocked(error)) return false;
    const FileHeader header{kContainerMagic, kVersion, record_count_, data_bytes_};
    output_.seekp(0, std::ios::beg);
    output_.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output_.flush(); output_.close();
    if (!output_ || !replaceFile(temporary_path_, spool_path_)) {
        failed_ = true; std::remove(temporary_path_.c_str());
        return fail(error, "cannot finalize container spool");
    }
    finished_ = true;
    if (spool_path) *spool_path = spool_path_;
    return true;
}
void ContainerItemSpoolWriter::discardLocked(bool remove_final) {
    if (output_.is_open()) output_.close();
    std::remove(temporary_path_.c_str());
    if (remove_final) std::remove(spool_path_.c_str());
}
void ContainerItemSpoolWriter::discard() {
    std::lock_guard<std::mutex> lock(mutex_);
    discardLocked(true);
    opened_ = false; finished_ = false; failed_ = false; record_count_ = 0; data_bytes_ = 0;
}
uint64_t ContainerItemSpoolWriter::recordCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return record_count_;
}
ContainerItemSpoolReader::ContainerItemSpoolReader(std::string path) : path_(std::move(path)) {
    std::lock_guard<std::mutex> lock(mutex_);
    input_.open(path_, std::ios::binary);
    if (!input_ || !readHeaderLocked()) failLocked();
}
void ContainerItemSpoolReader::failLocked() { failed_ = true; valid_ = false; }
bool ContainerItemSpoolReader::readHeaderLocked() {
    FileHeader header;
    input_.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!input_ || !validFileHeader(header, kContainerMagic,
                                    ContainerItemSpoolWriter::kMaximumRecords,
                                    ContainerItemSpoolWriter::kMaximumDataBytes,
                                    sizeof(ContainerRecordHeader)) ||
        header.data_bytes > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    record_count_ = header.record_count; data_bytes_ = header.data_bytes; data_start_ = input_.tellg();
    if (data_start_ < std::streampos(0)) return false;
    input_.seekg(0, std::ios::end); const std::streampos end = input_.tellg();
    if (end < data_start_ || static_cast<uint64_t>(end - data_start_) != data_bytes_) return false;
    input_.seekg(data_start_, std::ios::beg); consumed_bytes_ = 0; cursor_ = 0;
    valid_ = static_cast<bool>(input_); return valid_;
}
bool ContainerItemSpoolReader::readBytesLocked(void* output, size_t count) {
    if (count > data_bytes_ - consumed_bytes_ ||
        count > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        failLocked();
        return false;
    }
    input_.read(reinterpret_cast<char*>(output), static_cast<std::streamsize>(count));
    if (!input_) { failLocked(); return false; }
    consumed_bytes_ += count; return true;
}
bool ContainerItemSpoolReader::skipBytesLocked(uint64_t count) {
    if (count > data_bytes_ - consumed_bytes_ || count > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        failLocked(); return false;
    }
    input_.seekg(static_cast<std::streamoff>(count), std::ios::cur);
    if (!input_) { failLocked(); return false; }
    consumed_bytes_ += count; return true;
}
bool ContainerItemSpoolReader::readRecordLocked(ContainerItemRecord* record) {
    if (!record || cursor_ >= record_count_) return false;
    ContainerRecordHeader header;
    if (!readBytesLocked(&header, sizeof(header))) return false;
    if (header.container_id_bytes == 0 || header.container_id_bytes > kMaximumIdentifierBytes ||
        header.item_id_bytes == 0 || header.item_id_bytes > kMaximumIdentifierBytes ||
        header.enchantment_count > kMaximumEnchantmentsPerItem) {
        failLocked();
        return false;
    }
    ContainerItemRecord parsed;
    parsed.x = header.x; parsed.y = header.y; parsed.z = header.z;
    parsed.slot = header.slot; parsed.count = header.count; parsed.aux = header.aux;
    parsed.expected_container_id.assign(header.container_id_bytes, '\0');
    parsed.item_id.assign(header.item_id_bytes, '\0');
    if (!readBytesLocked(&parsed.expected_container_id[0], parsed.expected_container_id.size()) ||
        !readBytesLocked(&parsed.item_id[0], parsed.item_id.size())) {
        failLocked();
        return false;
    }
    parsed.enchantments.reserve(header.enchantment_count);
    for (uint8_t index = 0; index < header.enchantment_count; ++index) {
        uint16_t id_bytes = 0;
        if (!readBytesLocked(&id_bytes, sizeof(id_bytes)) || id_bytes == 0 ||
            id_bytes > 64) {
            failLocked();
            return false;
        }
        DeferredEnchantment enchantment;
        enchantment.id.assign(id_bytes, '\0');
        if (!readBytesLocked(&enchantment.id[0], enchantment.id.size()) ||
            !readBytesLocked(&enchantment.level, sizeof(enchantment.level))) {
            failLocked();
            return false;
        }
        parsed.enchantments.push_back(std::move(enchantment));
    }
    if (!validContainerRecord(parsed, nullptr)) { failLocked(); return false; }
    ++cursor_;
    if (cursor_ == record_count_ && consumed_bytes_ != data_bytes_) {
        failLocked();
        return false;
    }
    *record = std::move(parsed);
    return true;
}
bool ContainerItemSpoolReader::skipRecordLocked() { ContainerItemRecord ignored; return readRecordLocked(&ignored); }
bool ContainerItemSpoolReader::resetLocked() {
    if (!input_.is_open()) { failLocked(); return false; }
    input_.clear(); input_.seekg(data_start_, std::ios::beg);
    if (!input_) { failLocked(); return false; }
    consumed_bytes_ = 0;
    cursor_ = 0;
    valid_ = true;
    failed_ = false;
    return true;
}
bool ContainerItemSpoolReader::seekRecord(uint64_t record_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_ || record_index > record_count_ || !resetLocked()) return false;
    while (cursor_ < record_index && skipRecordLocked()) {}
    return valid_ && !failed_ && cursor_ == record_index;
}
std::optional<ContainerItemRecord> ContainerItemSpoolReader::next() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid_ || failed_ || cursor_ >= record_count_) return std::nullopt;
    ContainerItemRecord record;
    return readRecordLocked(&record) ? std::optional<ContainerItemRecord>(std::move(record)) : std::nullopt;
}
}  // namespace build_import
