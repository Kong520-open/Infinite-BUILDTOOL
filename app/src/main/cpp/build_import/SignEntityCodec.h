#ifndef INFINITE_TEXTURE_SIGN_ENTITY_CODEC_H
#define INFINITE_TEXTURE_SIGN_ENTITY_CODEC_H

#include "DeferredImportDataSpool.h"

#include <string>
#include <string_view>

namespace build_import {

// Extracts only persistent sign state. Coordinates, block-actor IDs, editing
// owners, locks and server-filtered text are deliberately ignored.
bool parseSignEntityJson(std::string_view entity_json,
                         SignRecord* output,
                         std::string* error = nullptr);

// Produces the canonical modern Bedrock representation used inside .infinity.
// Legacy top-level Text fields are represented as FrontText. Field presence is
// retained so absent values are not replaced with guessed defaults.
bool normalizeSignEntityJson(std::string_view entity_json,
                             std::string* normalized_json,
                             std::string* error = nullptr);

// Serializes the persistent fields of an already normalized record. Position
// and expected shell identity/Aux are import metadata and are never included
// in JSON.
bool encodeSignEntityJson(const SignRecord& record,
                          std::string* entity_json,
                          std::string* error = nullptr);

bool signRecordHasPayload(const SignRecord& record);

// Compares only fields that were present in the exported record. The server
// may materialize additional defaults, but it may not change exported text or
// persistent styling without making verification fail.
bool signRecordMatches(const SignRecord& expected, const SignRecord& observed,
                       std::string* mismatch = nullptr);

// Bedrock may omit IgnoreLighting or replace it with the sign shell's default
// after accepting otherwise complete block-actor data. Accept that server-side
// normalization only when every other exported field still matches exactly.
bool signRecordMatchesWithLightingNormalization(
    const SignRecord& expected, const SignRecord& observed,
    std::string* mismatch = nullptr, bool* normalized = nullptr);

}  // namespace build_import

#endif  // INFINITE_TEXTURE_SIGN_ENTITY_CODEC_H
