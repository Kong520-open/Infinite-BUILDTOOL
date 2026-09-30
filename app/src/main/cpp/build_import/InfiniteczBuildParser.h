#ifndef INFINITE_TEXTURE_INFINITEcz_BUILD_PARSER_H
#define INFINITE_TEXTURE_INFINITEcz_BUILD_PARSER_H

#include "SchematicParser.h"

namespace build_import {

// Reads the versioned lossless Infinitecz building format and presents it to
// the same bounded chunk/command sinks used by the legacy import pipeline.
class InfiniteczBuildParser {
public:
    // This identifies the generated command plan, not the on-disk file
    // format. v2 makes raw Bedrock records authoritative and invalidates old
    // checkpoints that approximated pale-oak and hanging-sign states through
    // the portable Java palette. v3 lets safe native identifiers exported by
    // a newer game bypass the bundled registry snapshot.
    static constexpr const char* kVersion = "infinitecz-build-v3";

    bool parse(const SchematicParseOptions& options, const BlockMapper& mapper,
               SchematicParseResult* result, std::string* error = nullptr) const;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_INFINITEcz_BUILD_PARSER_H
