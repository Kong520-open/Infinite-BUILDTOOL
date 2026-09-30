#include "SignEntityCodec.h"

#include "../Json/cJSON.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace build_import {
namespace {

constexpr size_t kMaximumEntityJsonBytes = 64U * 1024U * 1024U;

struct JsonDeleter {
    void operator()(cJSON* value) const noexcept {
        if (value) cJSON_Delete(value);
    }
};

using JsonPtr = std::unique_ptr<cJSON, JsonDeleter>;

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

bool hasDecodedNulEscape(std::string_view json) {
    for (size_t index = 0; index + 5U < json.size(); ++index) {
        if (json[index] != '\\' || json[index + 1U] != 'u' ||
            json.compare(index + 2U, 4U, "0000") != 0) continue;
        size_t slash_count = 1U;
        size_t cursor = index;
        while (cursor > 0U && json[cursor - 1U] == '\\') {
            ++slash_count;
            --cursor;
        }
        if ((slash_count & 1U) != 0U) return true;
    }
    return false;
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

bool readText(cJSON* value, std::string* output, std::string* error) {
    if (!value || !output) return fail(error, "sign Text field is missing");
    if (cJSON_IsString(value) && value->valuestring) {
        const std::string_view text(value->valuestring);
        if (text.size() > SignSpoolWriter::kMaximumTextBytes || !validUtf8(text)) {
            return fail(error, "sign Text field is oversized or invalid UTF-8");
        }
        output->assign(text);
        return true;
    }
    if (!cJSON_IsObject(value)) {
        return fail(error, "sign Text field is not a string or bytes wrapper");
    }

    cJSON* type = nullptr;
    cJSON* hex = nullptr;
    size_t field_count = 0;
    for (cJSON* field = value->child; field; field = field->next) {
        ++field_count;
        if (!field->string) {
            return fail(error, "sign Text bytes wrapper has an invalid field");
        }
        if (std::strcmp(field->string, "__type__") == 0) {
            if (type) return fail(error, "sign Text bytes wrapper repeats __type__");
            type = field;
        } else if (std::strcmp(field->string, "hex") == 0) {
            if (hex) return fail(error, "sign Text bytes wrapper repeats hex");
            hex = field;
        } else {
            return fail(error, "sign Text object is not a bytes wrapper");
        }
    }
    if (field_count != 2U || !cJSON_IsString(type) || !type->valuestring ||
        std::strcmp(type->valuestring, "bytes") != 0 || !cJSON_IsString(hex) ||
        !hex->valuestring) {
        return fail(error, "sign Text object is not a bytes wrapper");
    }

    const std::string_view encoded(hex->valuestring);
    if ((encoded.size() & 1U) != 0U ||
        encoded.size() > SignSpoolWriter::kMaximumTextBytes * 2U) {
        return fail(error, "sign Text bytes wrapper has invalid hex length");
    }
    const auto nibble = [](char character, uint8_t* result) {
        if (character >= '0' && character <= '9') {
            *result = static_cast<uint8_t>(character - '0');
        } else if (character >= 'a' && character <= 'f') {
            *result = static_cast<uint8_t>(character - 'a' + 10);
        } else if (character >= 'A' && character <= 'F') {
            *result = static_cast<uint8_t>(character - 'A' + 10);
        } else {
            return false;
        }
        return true;
    };
    output->clear();
    output->reserve(encoded.size() / 2U);
    for (size_t index = 0; index < encoded.size(); index += 2U) {
        uint8_t high = 0;
        uint8_t low = 0;
        if (!nibble(encoded[index], &high) || !nibble(encoded[index + 1U], &low)) {
            output->clear();
            return fail(error, "sign Text bytes wrapper contains invalid hex");
        }
        output->push_back(static_cast<char>((high << 4U) | low));
    }
    if (!validUtf8(*output)) {
        output->clear();
        return fail(error, "sign Text bytes wrapper is invalid UTF-8 or contains NUL");
    }
    return true;
}

JsonPtr parseJson(std::string_view entity_json, std::string* error) {
    if (entity_json.empty() || entity_json.size() > kMaximumEntityJsonBytes ||
        entity_json.find('\0') != std::string_view::npos ||
        hasDecodedNulEscape(entity_json)) {
        if (error) *error = "sign entity JSON is empty, oversized, or contains NUL";
        return {};
    }
    std::string input(entity_json);
    const char* parse_end = nullptr;
    JsonPtr root(cJSON_ParseWithLengthOpts(input.c_str(), input.size() + 1U,
                                           &parse_end, 1));
    if (!root || !cJSON_IsObject(root.get()) ||
        (parse_end && *parse_end != '\0')) {
        if (error) *error = "sign entity JSON is not an object";
        return {};
    }
    return root;
}

bool readBoolean(cJSON* value, bool* output) {
    if (!value || !output) return false;
    if (cJSON_IsBool(value)) {
        *output = cJSON_IsTrue(value);
        return true;
    }
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        (value->valuedouble != 0.0 && value->valuedouble != 1.0)) return false;
    *output = value->valuedouble == 1.0;
    return true;
}

bool readInteger(cJSON* value, int32_t* output) {
    if (!value || !output || !cJSON_IsNumber(value) ||
        !std::isfinite(value->valuedouble) ||
        std::floor(value->valuedouble) != value->valuedouble ||
        value->valuedouble < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
        value->valuedouble > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    *output = static_cast<int32_t>(value->valuedouble);
    return true;
}

bool readOptionalBoolean(cJSON* object, const char* name, bool* present,
                         bool* output, std::string* error) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!value) return true;
    if (!readBoolean(value, output)) {
        if (error) *error = std::string("sign ") + name + " field is not boolean";
        return false;
    }
    *present = true;
    return true;
}

bool readOptionalColor(cJSON* object, bool* present, int32_t* output,
                       std::string* error) {
    static constexpr const char* kNames[] = {"SignTextColor", "TextColor"};
    bool found = false;
    int32_t parsed = 0;
    for (const char* name : kNames) {
        cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
        if (!value) continue;
        int32_t candidate = 0;
        if (!readInteger(value, &candidate)) {
            if (error) *error = std::string("sign ") + name + " field is not int32";
            return false;
        }
        if (found && parsed != candidate) {
            return fail(error, "sign text color aliases disagree");
        }
        found = true;
        parsed = candidate;
    }
    *present = found;
    if (found) *output = parsed;
    return true;
}

bool hasFaceField(cJSON* object) {
    static constexpr const char* kNames[] = {
        "Text", "SignTextColor", "TextColor", "IgnoreLighting",
        "PersistFormatting", "HideGlowOutline"};
    for (const char* name : kNames) {
        if (cJSON_GetObjectItemCaseSensitive(object, name)) return true;
    }
    return false;
}

bool readFace(cJSON* object, SignTextRecord* output, std::string* error) {
    if (!object || !output || !cJSON_IsObject(object)) {
        return fail(error, "sign text face is not an object");
    }
    SignTextRecord parsed;
    parsed.present = true;
    cJSON* text = cJSON_GetObjectItemCaseSensitive(object, "Text");
    if (text) {
        if (!readText(text, &parsed.text, error)) return false;
        parsed.has_text = true;
    }
    if (!readOptionalColor(object, &parsed.has_text_color, &parsed.text_color, error) ||
        !readOptionalBoolean(object, "IgnoreLighting", &parsed.has_ignore_lighting,
                             &parsed.ignore_lighting, error) ||
        !readOptionalBoolean(object, "PersistFormatting",
                             &parsed.has_persist_formatting,
                             &parsed.persist_formatting, error) ||
        !readOptionalBoolean(object, "HideGlowOutline",
                             &parsed.has_hide_glow_outline,
                             &parsed.hide_glow_outline, error)) {
        return false;
    }
    *output = std::move(parsed);
    return true;
}

bool validFaceForEncoding(const SignTextRecord& face) {
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
        (face.has_hide_glow_outline || !face.hide_glow_outline);
}

cJSON* encodeFace(const SignTextRecord& face) {
    cJSON* object = cJSON_CreateObject();
    if (!object) return nullptr;
    if ((face.has_text &&
         !cJSON_AddStringToObject(object, "Text", face.text.c_str())) ||
        (face.has_text_color &&
         !cJSON_AddNumberToObject(object, "SignTextColor", face.text_color)) ||
        (face.has_ignore_lighting &&
         !cJSON_AddBoolToObject(object, "IgnoreLighting", face.ignore_lighting)) ||
        (face.has_persist_formatting &&
         !cJSON_AddBoolToObject(object, "PersistFormatting", face.persist_formatting)) ||
        (face.has_hide_glow_outline &&
         !cJSON_AddBoolToObject(object, "HideGlowOutline", face.hide_glow_outline))) {
        cJSON_Delete(object);
        return nullptr;
    }
    return object;
}

}  // namespace

bool parseSignEntityJson(std::string_view entity_json, SignRecord* output,
                         std::string* error) {
    if (error) error->clear();
    if (!output) return fail(error, "sign record output is missing");
    *output = {};
    try {
        JsonPtr root = parseJson(entity_json, error);
        if (!root) return false;
        SignRecord parsed;
        cJSON* front = cJSON_GetObjectItemCaseSensitive(root.get(), "FrontText");
        cJSON* back = cJSON_GetObjectItemCaseSensitive(root.get(), "BackText");
        if (front || back) {
            if ((front && !readFace(front, &parsed.front, error)) ||
                (back && !readFace(back, &parsed.back, error))) return false;
        } else if (hasFaceField(root.get()) &&
                   !readFace(root.get(), &parsed.front, error)) {
            return false;
        }
        cJSON* waxed = cJSON_GetObjectItemCaseSensitive(root.get(), "IsWaxed");
        if (waxed) {
            if (!readBoolean(waxed, &parsed.is_waxed)) {
                return fail(error, "sign IsWaxed field is not boolean");
            }
            parsed.has_is_waxed = true;
        }
        *output = std::move(parsed);
    } catch (const std::bad_alloc&) {
        *output = {};
        return fail(error, "not enough memory to parse sign entity JSON");
    }
    return true;
}

bool normalizeSignEntityJson(std::string_view entity_json,
                             std::string* normalized_json,
                             std::string* error) {
    if (error) error->clear();
    if (!normalized_json) return fail(error, "normalized sign JSON output is missing");
    normalized_json->clear();
    SignRecord record;
    if (!parseSignEntityJson(entity_json, &record, error)) return false;
    return encodeSignEntityJson(record, normalized_json, error);
}

bool encodeSignEntityJson(const SignRecord& record, std::string* entity_json,
                          std::string* error) {
    if (error) error->clear();
    if (!entity_json) return fail(error, "sign entity JSON output is missing");
    entity_json->clear();
    if (!validFaceForEncoding(record.front) || !validFaceForEncoding(record.back) ||
        (!record.has_is_waxed && record.is_waxed)) {
        return fail(error, "sign record contains invalid persistent fields");
    }
    JsonPtr root(cJSON_CreateObject());
    if (!root) return fail(error, "cannot create normalized sign JSON");
    if (record.front.present) {
        cJSON* front = encodeFace(record.front);
        if (!front || !cJSON_AddItemToObject(root.get(), "FrontText", front)) {
            if (front) cJSON_Delete(front);
            return fail(error, "cannot create normalized sign JSON");
        }
    }
    if (record.back.present) {
        cJSON* back = encodeFace(record.back);
        if (!back || !cJSON_AddItemToObject(root.get(), "BackText", back)) {
            if (back) cJSON_Delete(back);
            return fail(error, "cannot create normalized sign JSON");
        }
    }
    if (record.has_is_waxed &&
        !cJSON_AddBoolToObject(root.get(), "IsWaxed", record.is_waxed)) {
        return fail(error, "cannot create normalized sign JSON");
    }
    char* printed = cJSON_PrintUnformatted(root.get());
    if (!printed) return fail(error, "cannot serialize normalized sign JSON");
    try {
        entity_json->assign(printed);
    } catch (...) {
        cJSON_free(printed);
        return fail(error, "not enough memory for normalized sign JSON");
    }
    cJSON_free(printed);
    return true;
}

bool signRecordHasPayload(const SignRecord& record) {
    return record.front.present || record.back.present || record.has_is_waxed;
}

bool signRecordMatches(const SignRecord& expected, const SignRecord& observed,
                       std::string* mismatch) {
    if (mismatch) mismatch->clear();
    const auto compare_face = [mismatch](const SignTextRecord& wanted,
                                         const SignTextRecord& actual,
                                         const char* side) {
        const auto fail_field = [mismatch, side](const char* field) {
            if (mismatch) *mismatch = std::string(side) + " sign " + field + " differs";
            return false;
        };
        if (!wanted.present) return true;
        if (!actual.present) return fail_field("face");
        if (wanted.has_text) {
            // Bedrock may omit Text entirely for a blank face while another
            // snapshot represents the same face as an explicit empty string.
            // Treat only that empty representation as equivalent; non-empty
            // text still requires an exact readback.
            if ((!actual.has_text && !wanted.text.empty()) ||
                (actual.has_text && wanted.text != actual.text)) {
                return fail_field("text");
            }
        }
        if (wanted.has_text_color &&
            (!actual.has_text_color || wanted.text_color != actual.text_color)) {
            return fail_field("text color");
        }
        if (wanted.has_ignore_lighting &&
            (!actual.has_ignore_lighting ||
             wanted.ignore_lighting != actual.ignore_lighting)) {
            return fail_field("lighting state");
        }
        if (wanted.has_persist_formatting &&
            (!actual.has_persist_formatting ||
             wanted.persist_formatting != actual.persist_formatting)) {
            return fail_field("formatting state");
        }
        if (wanted.has_hide_glow_outline &&
            (!actual.has_hide_glow_outline ||
             wanted.hide_glow_outline != actual.hide_glow_outline)) {
            return fail_field("glow state");
        }
        return true;
    };
    if (!compare_face(expected.front, observed.front, "front") ||
        !compare_face(expected.back, observed.back, "back")) {
        return false;
    }
    if (expected.has_is_waxed &&
        (!observed.has_is_waxed || expected.is_waxed != observed.is_waxed)) {
        if (mismatch) *mismatch = "sign wax state differs";
        return false;
    }
    return true;
}

bool signRecordMatchesWithLightingNormalization(
        const SignRecord& expected, const SignRecord& observed,
        std::string* mismatch, bool* normalized) {
    if (normalized) *normalized = false;

    std::string local_mismatch;
    std::string* strict_mismatch = mismatch ? mismatch : &local_mismatch;
    if (signRecordMatches(expected, observed, strict_mismatch)) return true;
    if (*strict_mismatch != "front sign lighting state differs" &&
        *strict_mismatch != "back sign lighting state differs") {
        return false;
    }

    SignRecord relaxed = expected;
    relaxed.front.has_ignore_lighting = false;
    relaxed.front.ignore_lighting = false;
    relaxed.back.has_ignore_lighting = false;
    relaxed.back.ignore_lighting = false;
    std::string relaxed_mismatch;
    if (!signRecordMatches(relaxed, observed, &relaxed_mismatch)) {
        if (mismatch) *mismatch = std::move(relaxed_mismatch);
        return false;
    }
    if (normalized) *normalized = true;
    return true;
}

}  // namespace build_import
