#ifndef INFINITE_TEXTURE_BLOCK_MAPPER_H
#define INFINITE_TEXTURE_BLOCK_MAPPER_H

#include "BuildImportTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace build_import {

struct BlockSpec {
    std::string command_name;
    // Bedrock's native auxiliary/state value is 16-bit.  Most schematic
    // formats use only the low byte, but legacy BDX PlaceBlock records can
    // carry a packed 16-bit value.
    uint16_t aux = 0;
    ImportPhase phase = ImportPhase::Structure;
    bool can_fill = true;
    bool single_layer_only = false;
    bool stateful = false;
};

struct FlatBlockIdentity {
    std::string command_name;
    uint16_t aux = 0;
};

enum class BlockMappingStatus : uint8_t {
    Air,
    Mapped,
    Unsupported,
};

// Air and unsupported input are deliberately distinct. Parsers may skip Air,
// but must surface Unsupported instead of silently creating a hole.
struct BlockMappingResult {
    BlockMappingStatus status = BlockMappingStatus::Unsupported;
    BlockSpec spec;
    std::string reason;

    bool isMapped() const { return status == BlockMappingStatus::Mapped; }
};

// Maps the legacy MCEdit numeric ID/data pair to a command-safe block name.
// The mapper is intentionally versioned; unsupported entries are rejected
// rather than emitted as arbitrary command text.
class BlockMapper {
public:
    // v13 invalidates plans made before native hanging-sign states and the
    // target's pale-oak sign identifiers were mapped losslessly.
    static constexpr const char* kVersion = "bedrock-legacy-v13";

    BlockMappingResult mapLegacy(uint16_t id, uint8_t data) const;
    BlockMappingResult mapSpongeState(std::string_view state) const;

    // Maps the Bedrock palette form used by BDX files.  BDX keeps the block
    // identifier and its quoted state array separate, for example
    // `minecraft:oak_stairs` plus `["upside_down_bit"=false,
    // "weirdo_direction"=3]`.  The optional legacy data is used only by
    // state-less/native fallbacks; explicit BDX state always wins.
    BlockMappingResult mapBedrockState(std::string_view identifier,
                                       std::string_view state,
                                       uint16_t legacy_data = 0,
                                       bool has_legacy_aux = false) const;

    // Maps identifiers written by this app's native .infinity exporter. The
    // bundled target registry is only a snapshot, while an exported native
    // name proves that the running game already exposes that block. Unknown
    // but command-safe minecraft identifiers may therefore pass through on
    // this route only; external Sponge/BDX inputs keep the strict registry
    // gate above. A portable palette fallback is accepted only when it has no
    // state properties, because silently dropping an unknown state is lossy.
    BlockMappingResult mapInfiniteczState(std::string_view state,
                                          uint16_t native_aux = 0,
                                          bool has_native_aux = false,
                                          std::string_view state_json = {}) const;

    // Compatibility helpers for callers that do not need to distinguish Air
    // from Unsupported. SchematicParser uses the detailed methods above.
    std::optional<BlockSpec> resolveLegacy(uint16_t id, uint8_t data) const;
    std::optional<BlockSpec> resolveSpongeState(std::string_view state) const;
    std::optional<BlockSpec> resolveBedrockState(std::string_view identifier,
                                                  std::string_view state,
                                                  uint16_t legacy_data = 0,
                                                  bool has_legacy_aux = false) const;

    // Final verification must avoid blocks that can detach, decay, grow, melt,
    // or otherwise change without an importer failure.
    static bool isStableVerificationBlockName(std::string_view block_name);

    // Converts a flattened/native block name back to the exact legacy command
    // identity used by the importer. Keeping this beside mapFlatBlock prevents
    // final verification from maintaining a second, incomplete alias table.
    // The auxiliary value is required for slabs: the target reports modern
    // names with a 0/1 vertical-half aux, while the importer's command space
    // keeps the legacy family and bit 3 (8) for the upper half.
    static std::optional<FlatBlockIdentity> flatBlockIdentityForVerification(
        std::string_view block_name, uint16_t aux = 0);

private:
    static ImportPhase phaseFor(std::string_view block_name);
    static bool isSafeIdentifier(std::string_view value);
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_BLOCK_MAPPER_H
