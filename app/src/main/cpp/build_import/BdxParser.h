#ifndef INFINITE_TEXTURE_BDX_PARSER_H
#define INFINITE_TEXTURE_BDX_PARSER_H

#include "SchematicParser.h"

namespace build_import {

// Streams PhoenixBuilder / BDump BDX blueprints directly through the native
// chunk spool writer. BDX files use a Brotli-compressed command stream rather
// than an NBT volume, so no JSON or whole-building block array is created.
//
// Compact BDX chest inventories and command-block settings can be preserved as
// deferred records when their respective sinks are supplied.  They are never
// executed during parsing and are applied only after normal block placement
// has completed; arbitrary unsupported block-entity payloads remain skipped.
class BdxParser {
public:
    // Bump when BDX opcode handling or Bedrock-state conversion changes so an
    // interrupted job cannot replay spools built by a different parser.
    // v4 persists deferred command-block payloads. v5 additionally persists
    // compact opcode-40 chest inventories. v6 normalizes unqualified legacy
    // BDX names and skips non-portable editor permission markers. v7 also
    // normalizes historical camel-case/alias palette names before command
    // block NBT detection. v8 preserves the full uint16 legacy PlaceBlock
    // state/data value through the import spool and command plan. v9 delays
    // command-block sidecar delivery until final BDX stream order is known,
    // so a later placement cannot leave a stale update record behind. v10
    // accepts the legacy terminal command-block record that omits its final
    // need-redstone boolean immediately before the BDX End opcode. v11 adds
    // the fixed NetEase pool-117 runtime-id translation for opcodes 32-38.
    // v12 keeps the final command-block shell conditional state authoritative
    // when emitting the corresponding deferred editor-packet record. v13
    // recognizes legacy streams that omit needRedstone on every command-block
    // payload without consuming their following command opcode. v14 also
    // distinguishes that layout from historical non-canonical placeholder
    // bytes, preventing duplicated movement opcodes from spreading blocks.
    static constexpr const char* kVersion = "bdx-brotli-bedrock-v14";

    bool parse(const SchematicParseOptions& options,
               SchematicParseResult* result, std::string* error = nullptr) const;
    bool parse(const SchematicParseOptions& options, const BlockMapper& mapper,
               SchematicParseResult* result, std::string* error = nullptr) const;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BDX_PARSER_H
