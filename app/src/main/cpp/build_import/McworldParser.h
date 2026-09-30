#ifndef INFINITE_TEXTURE_MCWORLD_PARSER_H
#define INFINITE_TEXTURE_MCWORLD_PARSER_H

#include "BlockMapper.h"
#include "SchematicParser.h"

namespace build_import {

// Read-only parser for Bedrock .mcworld archives.  The format is a ZIP archive
// containing a LevelDB world database, not a schematic NBT tree. It imports
// overworld block states and can optionally retain only command-block payloads
// from 0x31 block-entity records; entities and inventories remain excluded.
class McworldParser {
public:
    // v5 adds deferred command-block extraction from Bedrock 0x31 records in
    // addition to v4's palette normalization. v6 also synchronizes a deferred
    // record's conditional state with the final palette shell. Existing spools
    // must therefore be rejected rather than resumed.
    static constexpr const char* kVersion = "mcworld-bedrock-leveldb-v6";

    bool parse(const SchematicParseOptions& options, SchematicParseResult* result,
               std::string* error) const;
    bool parse(const SchematicParseOptions& options, const BlockMapper& mapper,
               SchematicParseResult* result, std::string* error) const;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_MCWORLD_PARSER_H
