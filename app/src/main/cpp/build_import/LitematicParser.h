#ifndef INFINITE_TEXTURE_LITEMATIC_PARSER_H
#define INFINITE_TEXTURE_LITEMATIC_PARSER_H

#include "SchematicParser.h"

namespace build_import {

// Streams Java Litematica NBT regions directly into native chunk spools.
// Command blocks, normalized container items, and safe basic entity fields are
// deferred to their independent post-placement sinks.
class LitematicParser {
public:
    // Includes both coordinate-layout and block-mapping compatibility. Bump
    // this whenever either changes so checkpoints cannot replay stale spools.
    static constexpr const char* kVersion = "litematic-v3-deferred-data-v14";

    bool parse(const SchematicParseOptions& options,
               SchematicParseResult* result, std::string* error = nullptr) const;
    bool parse(const SchematicParseOptions& options, const BlockMapper& mapper,
               SchematicParseResult* result, std::string* error = nullptr) const;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_LITEMATIC_PARSER_H
