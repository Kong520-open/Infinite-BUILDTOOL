#include "../SignEntityCodec.h"

#include <cassert>
#include <string>

using namespace build_import;

namespace {

void testModernFieldsAndWhitelist() {
    const std::string source =
        R"({"id":"Sign","x":12,"y":64,"z":-8,"TextOwner":987654321,)"
        R"("LockedForEditing":1,"FilteredText":"top-level filtered",)"
        R"("FrontText":{"Text":"front\n\u4f60\u597d","SignTextColor":-16711936,)"
        R"("IgnoreLighting":1,"PersistFormatting":false,"HideGlowOutline":1,)"
        R"("TextIgnoreLegacyBugResolved":true,"TextOwner":42,)"
        R"("FilteredText":"filtered front","Unknown":{"unsafe":true}},)"
        R"("BackText":{"Text":"back","TextColor":-2,"IgnoreLighting":false,)"
        R"("LockedForEditing":true},"IsWaxed":1,"UnknownRoot":"ignored"})";

    SignRecord record;
    std::string error;
    assert(parseSignEntityJson(source, &record, &error));
    assert(error.empty());
    assert(record.x == 0 && record.y == 0 && record.z == 0);
    assert(record.expected_sign_id.empty());
    assert(record.front.present && record.front.has_text);
    assert(record.front.text == std::string("front\n") + u8"\u4f60\u597d");
    assert(record.front.has_text_color && record.front.text_color == -16711936);
    assert(record.front.has_ignore_lighting && record.front.ignore_lighting);
    assert(record.front.has_persist_formatting && !record.front.persist_formatting);
    assert(record.front.has_hide_glow_outline && record.front.hide_glow_outline);
    assert(!record.front.has_text_ignore_legacy_bug_resolved &&
           !record.front.text_ignore_legacy_bug_resolved);
    assert(record.back.present && record.back.has_text && record.back.text == "back");
    assert(record.back.has_text_color && record.back.text_color == -2);
    assert(record.back.has_ignore_lighting && !record.back.ignore_lighting);
    assert(record.has_is_waxed && record.is_waxed);

    std::string normalized;
    assert(normalizeSignEntityJson(source, &normalized, &error));
    const std::string expected =
        std::string(R"({"FrontText":{"Text":"front\n)") + u8"\u4f60\u597d" +
        R"(","SignTextColor":-16711936,"IgnoreLighting":true,"PersistFormatting":false,"HideGlowOutline":true},"BackText":{"Text":"back","SignTextColor":-2,"IgnoreLighting":false},"IsWaxed":true})";
    assert(normalized == expected);
    assert(normalized.find("TextOwner") == std::string::npos);
    assert(normalized.find("LockedForEditing") == std::string::npos);
    assert(normalized.find("FilteredText") == std::string::npos);
    assert(normalized.find("\"x\"") == std::string::npos);
    assert(normalized.find("Unknown") == std::string::npos);
    assert(normalized.find("TextIgnoreLegacyBugResolved") == std::string::npos);
}

void testLegacyAndPresence() {
    std::string normalized;
    std::string error;
    assert(normalizeSignEntityJson(
        R"({"Text":"line 1\nline 2","TextColor":-16777216,"IgnoreLighting":0,"PersistFormatting":1,"HideGlowOutline":0,"TextOwner":55,"x":1,"y":2,"z":3})",
        &normalized, &error));
    assert(normalized ==
           R"({"FrontText":{"Text":"line 1\nline 2","SignTextColor":-16777216,"IgnoreLighting":false,"PersistFormatting":true,"HideGlowOutline":false}})");

    SignRecord record;
    assert(parseSignEntityJson(
        R"({"FrontText":{"Text":""},"BackText":{},"IsWaxed":0})",
        &record, &error));
    assert(record.front.present && record.front.has_text && record.front.text.empty());
    assert(record.back.present && !record.back.has_text);
    assert(record.has_is_waxed && !record.is_waxed);
    assert(signRecordHasPayload(record));
    assert(encodeSignEntityJson(record, &normalized, &error));
    assert(normalized ==
           R"({"FrontText":{"Text":""},"BackText":{},"IsWaxed":false})");

    assert(parseSignEntityJson(
        R"({"id":"Sign","x":5,"TextOwner":1,"LockedForEditing":1,"FilteredText":"discard"})",
        &record, &error));
    assert(!signRecordHasPayload(record));
    assert(encodeSignEntityJson(record, &normalized, &error));
    assert(normalized == "{}");

    assert(parseSignEntityJson(
        R"({"TextIgnoreLegacyBugResolved":true,"TextOwner":7})",
        &record, &error));
    assert(!signRecordHasPayload(record));
}

void testPythonBytesTextWrapper() {
    const std::string source =
        R"({"FrontText":{"Text":{"__type__":"bytes","hex":"48656C6C6F0A7369676E"}},)"
        R"("BackText":{"Text":{"hex":"e4bda0e5a5bd","__type__":"bytes"}}})";
    SignRecord record;
    std::string error;
    assert(parseSignEntityJson(source, &record, &error));
    assert(error.empty());
    assert(record.front.present && record.front.has_text);
    assert(record.front.text == "Hello\nsign");
    assert(record.back.present && record.back.has_text);
    assert(record.back.text == u8"\u4f60\u597d");

    std::string normalized;
    assert(normalizeSignEntityJson(source, &normalized, &error));
    const std::string expected =
        std::string(R"({"FrontText":{"Text":"Hello\nsign"},"BackText":{"Text":")") +
        u8"\u4f60\u597d" + R"("}})";
    assert(normalized == expected);

    static constexpr const char* kInvalidWrappers[] = {
        R"({"FrontText":{"Text":{"__type__":"bytes","hex":"1"}}})",
        R"({"FrontText":{"Text":{"__type__":"bytes","hex":"0g"}}})",
        R"({"FrontText":{"Text":{"__type__":"bytes","hex":"610062"}}})",
        R"({"FrontText":{"Text":{"__type__":"bytes","hex":"c0af"}}})",
        R"({"FrontText":{"Text":{"__type__":"repr","hex":"61"}}})",
        R"({"FrontText":{"Text":{"__type__":"bytes","hex":61}}})",
        R"({"FrontText":{"Text":{"__type__":"bytes","hex":"61","extra":true}}})",
        R"({"FrontText":{"Text":{"__type__":"bytes","__type__":"bytes","hex":"61"}}})",
        R"({"FrontText":{"Text":{"hex":"61"}}})",
    };
    for (const char* invalid : kInvalidWrappers) {
        assert(!parseSignEntityJson(invalid, &record, &error));
        assert(!error.empty());
    }

    const std::string oversized_hex(
        (SignSpoolWriter::kMaximumTextBytes + 1U) * 2U, '1');
    const std::string oversized =
        R"({"FrontText":{"Text":{"__type__":"bytes","hex":")" +
        oversized_hex + R"("}}})";
    assert(!parseSignEntityJson(oversized, &record, &error));
    assert(!error.empty());
}

void testStrictKnownFields() {
    SignRecord record;
    std::string normalized;
    std::string error;
    assert(!parseSignEntityJson("[]", &record, &error));
    assert(!parseSignEntityJson(R"({"FrontText":"not an object"})", &record,
                                &error));
    assert(!parseSignEntityJson(
        R"({"FrontText":{"IgnoreLighting":2}})", &record, &error));
    assert(!parseSignEntityJson(
        R"({"FrontText":{"SignTextColor":1,"TextColor":2}})", &record,
        &error));
    assert(!parseSignEntityJson(R"({"IsWaxed":"yes"})", &record, &error));
    assert(!parseSignEntityJson(R"({"Text":"bad\u0000tail"})", &record,
                                &error));

    record = {};
    record.front.present = true;
    record.front.has_text = true;
    record.front.text.assign(SignSpoolWriter::kMaximumTextBytes + 1U, 'x');
    assert(!encodeSignEntityJson(record, &normalized, &error));

    record = {};
    record.x = 100;
    record.y = 20;
    record.z = -100;
    record.expected_sign_id = "minecraft:oak_sign";
    record.front.present = true;
    record.front.has_text = true;
    record.front.text = "coordinates stay out";
    assert(encodeSignEntityJson(record, &normalized, &error));
    assert(normalized == R"({"FrontText":{"Text":"coordinates stay out"}})");
}

void testVerificationUsesExportedPresence() {
    SignRecord expected;
    expected.front.present = true;
    expected.front.has_text = true;
    expected.front.text = "kept";
    expected.has_is_waxed = true;
    expected.is_waxed = false;

    SignRecord observed = expected;
    observed.front.has_text_color = true;
    observed.front.text_color = -16777216;
    observed.back.present = true;
    observed.back.has_text = true;
    observed.back.text = "server default";
    std::string mismatch;
    assert(signRecordMatches(expected, observed, &mismatch));
    assert(mismatch.empty());

    observed.front.text = "filtered";
    assert(!signRecordMatches(expected, observed, &mismatch));
    assert(mismatch == "front sign text differs");

    expected.front.text.clear();
    observed.front.has_text = false;
    observed.front.text.clear();
    mismatch.clear();
    assert(signRecordMatches(expected, observed, &mismatch));
    assert(mismatch.empty());
}

void testLightingNormalizationDoesNotHideOtherMismatches() {
    SignRecord expected;
    expected.front.present = true;
    expected.front.has_text = true;
    expected.front.text = "front";
    expected.front.has_text_color = true;
    expected.front.text_color = -16711936;
    expected.front.has_ignore_lighting = true;
    expected.front.ignore_lighting = true;
    expected.front.has_persist_formatting = true;
    expected.front.persist_formatting = true;
    expected.back.present = true;
    expected.back.has_text = true;
    expected.back.text = "back";
    expected.back.has_ignore_lighting = true;
    expected.back.ignore_lighting = false;
    expected.has_is_waxed = true;
    expected.is_waxed = true;

    SignRecord observed = expected;
    observed.front.has_ignore_lighting = false;
    observed.front.ignore_lighting = false;
    std::string mismatch;
    bool normalized = false;
    assert(signRecordMatchesWithLightingNormalization(
        expected, observed, &mismatch, &normalized));
    assert(normalized);
    assert(mismatch == "front sign lighting state differs");

    observed.front.text = "changed";
    assert(!signRecordMatchesWithLightingNormalization(
        expected, observed, &mismatch, &normalized));
    assert(!normalized);
    assert(mismatch == "front sign text differs");

    observed.front.text = expected.front.text;
    observed.front.persist_formatting = false;
    assert(!signRecordMatchesWithLightingNormalization(
        expected, observed, &mismatch, &normalized));
    assert(!normalized);
    assert(mismatch == "front sign formatting state differs");

    observed = expected;
    observed.back.ignore_lighting = true;
    assert(signRecordMatchesWithLightingNormalization(
        expected, observed, nullptr, &normalized));
    assert(normalized);

    observed.is_waxed = false;
    assert(!signRecordMatchesWithLightingNormalization(
        expected, observed, &mismatch, &normalized));
    assert(!normalized);
    assert(mismatch == "sign wax state differs");
}

}  // namespace

int main() {
    testModernFieldsAndWhitelist();
    testLegacyAndPresence();
    testPythonBytesTextWrapper();
    testStrictKnownFields();
    testVerificationUsesExportedPresence();
    testLightingNormalizationDoesNotHideOtherMismatches();
    return 0;
}
