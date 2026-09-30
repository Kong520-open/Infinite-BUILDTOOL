#include "ProjectionWorldMatchRuntime.h"

#include "BuildProjectionRenderer.h"
#include "BuildProjectionRuntime.h"
#include "NativeWorldAccess.h"
#include "ProjectionBlockIdentity.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace build_import {
namespace {

// A page is deliberately smaller than the world-read budget's long-term work
// queue.  It bounds both temporary string copies from BuildProjectionRuntime
// and a single tick's pressure on the local-player hook.
constexpr size_t kMaximumSourceTargetsPerPage = 256U;
constexpr size_t kMaximumWorldReadsPerTick = 128U;
constexpr size_t kMaximumPageRequestsPerTick = 4U;
// A normal frame can issue up to 1024 draw batches; a finite layer cut can
// contribute two disjoint batches for one rendered group. Keep every active
// renderer interest rather than silently dropping the far half of the view.
constexpr size_t kMaximumInterestRegions = 2048U;
constexpr int64_t kMaximumInterestRegionSpan = 32;
constexpr auto kPendingPageRetry = std::chrono::milliseconds(50);
constexpr auto kSourceUnavailableRetry = std::chrono::milliseconds(250);
constexpr auto kCompletedRegionRefresh = std::chrono::milliseconds(750);
constexpr auto kStatePublishInterval = std::chrono::milliseconds(125);
// A public client block-state query is comparatively expensive.  Modern
// stairs are the one non-ambiguous family whose Block::aux cannot be trusted
// on this build, so cache its semantic aux by the interned native Block state
// and spread first-time probes over successive game ticks.
constexpr size_t kMaximumCachedStairStateTokens = 128U;
constexpr size_t kMaximumStairStateSnapshotsPerTick = 2U;
constexpr auto kUnavailableStairStateRetry = std::chrono::milliseconds(750);

const std::shared_ptr<const ProjectionWorldMatchSnapshot> g_empty_snapshot =
    std::make_shared<const ProjectionWorldMatchSnapshot>();
std::shared_ptr<const ProjectionWorldMatchSnapshot> g_published_snapshot =
    g_empty_snapshot;

const std::shared_ptr<const ProjectionWorldMatchInterestSnapshot> g_empty_interest =
    std::make_shared<const ProjectionWorldMatchInterestSnapshot>();
std::shared_ptr<const ProjectionWorldMatchInterestSnapshot> g_published_interest =
    g_empty_interest;

struct PendingTarget {
    ProjectionWorldMatchPosition position;
    std::string expected_name;
    uint16_t expected_aux = 0;
    // A rotated projection has transformed source coordinates but does not
    // currently transform every Bedrock state encoding. Showing red/yellow or
    // hiding a directional/stateful block would therefore be misleading.
    bool state_comparison_ambiguous = false;
    // NativeBlockView deliberately exposes the block shell, not block-entity
    // NBT. A matching chest/sign/command block shell cannot prove its saved
    // payload is correct, so it must never hide the projected source block.
    bool entity_comparison_ambiguous = false;
    // Modern stairs encode their visible orientation in public client data or
    // GetBlockNew properties. Their native Block::aux can remain zero for
    // every facing, so a raw-aux comparison would falsely paint a correctly
    // placed stair yellow.
    bool requires_stair_state = false;
};

struct StairStateCacheEntry {
    bool has_aux = false;
    uint16_t aux = 0;
    // Failed public-state probes are retried later rather than treated as a
    // permanent result, but never once per rendered cell.
    std::chrono::steady_clock::time_point retry_after{};
};

struct InterestWork {
    ProjectionWorldMatchInterestRegion region;
    uint64_t cursor = 0;
    std::chrono::steady_clock::time_point retry_after{};
};

uint64_t mix64(uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

uint64_t rotateLeft(uint64_t value, unsigned int amount) noexcept {
    return (value << amount) | (value >> (64U - amount));
}

uint64_t groupCellHash(size_t cell_index, uint8_t state) noexcept {
    return mix64((static_cast<uint64_t>(cell_index) + 1U) ^
                 (static_cast<uint64_t>(state) << 48U));
}

void updateGroupFingerprint(ProjectionWorldMatchGroupState* group) noexcept {
    if (!group) return;
    if (group->non_default_count == 0U) {
        group->fingerprint = 0;
        return;
    }
    const uint64_t fingerprint = mix64(
        group->xor_hash ^ rotateLeft(group->additive_hash, 17U) ^
        (static_cast<uint64_t>(group->non_default_count) *
         UINT64_C(0xd6e8feb86659fd93)));
    // Zero is reserved for an all-default VBO. Preserve the distinction even
    // in the vanishingly unlikely event of a nonempty hash evaluating to zero.
    group->fingerprint = fingerprint == 0U ? 1U : fingerprint;
}

int32_t floorDivGroupCoordinate(int32_t value) noexcept {
    const int64_t wide = value;
    const int64_t span = kProjectionWorldMatchGroupSpan;
    const int64_t quotient = wide >= 0 ? wide / span : -(((-wide) + span - 1) / span);
    return static_cast<int32_t>(quotient);
}

uint8_t localGroupCoordinate(int32_t value, int32_t group_coordinate) noexcept {
    const int64_t local = static_cast<int64_t>(value) -
        static_cast<int64_t>(group_coordinate) * kProjectionWorldMatchGroupSpan;
    return local >= 0 && local < kProjectionWorldMatchGroupSpan
        ? static_cast<uint8_t>(local) : 0U;
}

size_t groupCellIndex(uint8_t local_x, uint8_t local_y, uint8_t local_z) noexcept {
    return static_cast<size_t>(local_x) |
        (static_cast<size_t>(local_z) << 4U) |
        (static_cast<size_t>(local_y) << 8U);
}

ProjectionWorldMatchGroupKey groupKeyForWorldPosition(
        int32_t x, int32_t y, int32_t z) noexcept {
    return {floorDivGroupCoordinate(x), floorDivGroupCoordinate(y),
            floorDivGroupCoordinate(z)};
}

ProjectionWorldMatchGroupKey groupKeyForOrigin(
        int32_t x, int32_t y, int32_t z) noexcept {
    // Renderer origins are always 16-aligned. floor division keeps this helper
    // safe and deterministic if a future caller supplies an unaligned value.
    return groupKeyForWorldPosition(x, y, z);
}

std::string canonicalName(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (const unsigned char character : value) {
        if (character == '[' || character == '{' || character == '|') break;
        if (character >= 'A' && character <= 'Z') {
            output.push_back(static_cast<char>(character - 'A' + 'a'));
        } else if (character != ' ' && character != '\t' && character != '\r' &&
                   character != '\n') {
            output.push_back(static_cast<char>(character));
        }
    }
    constexpr std::string_view kNamespace = "minecraft:";
    if (output.compare(0, kNamespace.size(), kNamespace) == 0) {
        output.erase(0, kNamespace.size());
    }
    // Keep these aliases aligned with the printer's source/world identity
    // comparison. They are legacy spelling changes, not fuzzy matching.
    if (output == "grass") return "grass_block";
    if (output == "lit_furnace") return "furnace";
    if (output == "unlit_redstone_torch") return "redstone_torch";
    if (output == "flowing_water") return "water";
    if (output == "flowing_lava") return "lava";
    return output;
}

bool containsToken(std::string_view value, std::string_view token) {
    return value.find(token) != std::string_view::npos;
}

bool isAir(std::string_view canonical_name) {
    return canonical_name.empty() || canonical_name == "air" ||
        canonical_name == "cave_air" || canonical_name == "void_air";
}

bool isStairName(std::string_view canonical_name) {
    return canonical_name.find("stairs") != std::string_view::npos ||
        canonical_name == "normal_stone_stairs";
}

bool isRotationSensitiveName(std::string_view canonical_name) {
    // Covers common legacy state encodings which rotate with a projected
    // structure. The source mapper also marks explicit stateful records, so
    // this stays conservative rather than guessing undocumented aux mappings.
    static constexpr std::string_view kTokens[] = {
        "stairs", "slab", "_log", "_wood", "_stem", "hyphae", "pillar",
        "glazed_terracotta", "chain", "lantern", "lightning_rod", "end_rod",
        "amethyst_cluster", "pointed_dripstone", "scaffolding", "ladder",
        "vine", "lichen", "torch", "rail", "repeater", "comparator",
        "redstone", "tripwire", "button", "lever", "door", "trapdoor",
        "fence_gate", "bed", "piston", "observer", "dispenser", "dropper",
        "hopper", "coral_fan", "hanging_roots", "cave_vines",
    };
    for (const std::string_view token : kTokens) {
        if (containsToken(canonical_name, token)) return true;
    }
    return canonical_name == "quartz_block" || canonical_name == "bone_block" ||
        canonical_name == "basalt" || canonical_name == "polished_basalt" ||
        canonical_name == "log" || canonical_name == "log2" ||
        canonical_name == "wood" || canonical_name == "pumpkin" ||
        canonical_name == "lit_pumpkin" || canonical_name == "hay_block" ||
        canonical_name == "bamboo_block" ||
        canonical_name == "stripped_bamboo_block" ||
        canonical_name == "end_portal_frame" ||
        canonical_name == "daylight_detector" ||
        canonical_name == "daylight_detector_inverted";
}

bool isLanternName(std::string_view raw_name) {
    const std::string name = ProjectionCanonicalBlockName(raw_name);
    return name == "lantern" || name == "soul_lantern";
}

bool hasComparablePlacementState(const ProjectionPrinterTarget& target) {
    const std::string name = ProjectionCanonicalBlockName(target.name);
    uint16_t ignored_aux = 0U;
    if (isStairName(name)) {
        return RotateProjectionStairAux(target.aux, target.rotation_quarters, &ignored_aux);
    }
    bool ignored_top = false;
    bool is_double = false;
    if (TryProjectionSlabPlacement(target.name, target.aux, &ignored_top, &is_double)) {
        // A complete slab has no horizontal orientation and is as comparable
        // as either single half. Its 0x80 identity is normalized centrally.
        return true;
    }
    if (IsProjectionDaylightDetector(target.name)) return target.aux <= UINT16_C(0x000F);
    if (IsProjectionContentlessContainerShell(target.name)) {
        ProjectionBlockFace ignored_face;
        if (TryProjectionSixWayFacing(target.name, target.aux, &ignored_face)) {
            return RotateProjectionSixWayFacingAux(target.name, target.aux,
                                                    target.rotation_quarters,
                                                    &ignored_aux);
        }
        if (TryProjectionHorizontalSixWayFacing(target.name, target.aux, &ignored_face)) {
            return RotateProjectionHorizontalSixWayFacingAux(target.name, target.aux,
                                                              target.rotation_quarters,
                                                              &ignored_aux);
        }
        return false;
    }
    uint8_t ignored_direction = 0U;
    uint8_t ignored_delay = 0U;
    bool ignored_subtract = false;
    if (TryProjectionRepeaterState(target.name, target.aux, &ignored_direction,
                                   &ignored_delay)) {
        return RotateProjectionRepeaterAux(target.name, target.aux,
                                           target.rotation_quarters, &ignored_aux);
    }
    if (TryProjectionComparatorState(target.name, target.aux, &ignored_direction,
                                    &ignored_subtract)) {
        return RotateProjectionComparatorAux(target.name, target.aux,
                                             target.rotation_quarters, &ignored_aux);
    }
    ProjectionBlockAxis ignored_axis;
    if (TryProjectionBlockAxis(target.name, target.aux, &ignored_axis)) {
        return RotateProjectionBlockAxisAux(target.name, target.aux,
                                             target.rotation_quarters, &ignored_aux);
    }
    ProjectionBlockFace ignored_face;
    if (TryProjectionBlockFace(target.name, target.aux, &ignored_face)) {
        return RotateProjectionBlockFaceAux(target.name, target.aux,
                                             target.rotation_quarters, &ignored_aux);
    }
    if (TryProjectionAttachmentFace(target.name, target.aux, &ignored_face)) {
        return RotateProjectionAttachmentFaceAux(target.name, target.aux,
                                                  target.rotation_quarters, &ignored_aux);
    }
    if (TryProjectionLeverAttachmentFace(target.name, target.aux, &ignored_face)) {
        return RotateProjectionLeverAux(target.name, target.aux,
                                        target.rotation_quarters, &ignored_aux);
    }
    if (isLanternName(target.name)) return target.aux <= 1U;
    ignored_direction = 0U;
    return TryProjectionHorizontalDirection(target.name, target.aux, &ignored_direction) &&
        RotateProjectionHorizontalDirectionAux(target.name, target.aux,
                                                target.rotation_quarters, &ignored_aux);
}

bool effectiveComparableAux(const ProjectionPrinterTarget& target, uint16_t* output) {
    if (!output) return false;
    const std::string name = ProjectionCanonicalBlockName(target.name);
    if (isStairName(name)) {
        return RotateProjectionStairAux(target.aux, target.rotation_quarters, output);
    }
    if (IsProjectionDaylightDetector(target.name)) {
        if (target.aux > UINT16_C(0x000F)) return false;
        *output = 0U;
        return true;
    }
    if (IsProjectionContentlessContainerShell(target.name)) {
        ProjectionBlockFace face;
        if (TryProjectionSixWayFacing(target.name, target.aux, &face)) {
            return RotateProjectionSixWayFacingAux(target.name, target.aux,
                                                    target.rotation_quarters, output);
        }
        if (TryProjectionHorizontalSixWayFacing(target.name, target.aux, &face)) {
            return RotateProjectionHorizontalSixWayFacingAux(target.name, target.aux,
                                                              target.rotation_quarters, output);
        }
        return false;
    }
    uint8_t direction = 0U;
    uint8_t delay = 0U;
    bool subtract_mode = false;
    if (TryProjectionRepeaterState(target.name, target.aux, &direction, &delay)) {
        return RotateProjectionRepeaterAux(target.name, target.aux,
                                           target.rotation_quarters, output);
    }
    if (TryProjectionComparatorState(target.name, target.aux, &direction, &subtract_mode)) {
        return RotateProjectionComparatorAux(target.name, target.aux,
                                             target.rotation_quarters, output);
    }
    ProjectionBlockAxis axis;
    if (TryProjectionBlockAxis(target.name, target.aux, &axis)) {
        return RotateProjectionBlockAxisAux(target.name, target.aux,
                                             target.rotation_quarters, output);
    }
    ProjectionBlockFace face;
    if (TryProjectionBlockFace(target.name, target.aux, &face)) {
        return RotateProjectionBlockFaceAux(target.name, target.aux,
                                             target.rotation_quarters, output);
    }
    if (TryProjectionAttachmentFace(target.name, target.aux, &face)) {
        return RotateProjectionAttachmentFaceAux(target.name, target.aux,
                                                  target.rotation_quarters, output);
    }
    if (TryProjectionLeverAttachmentFace(target.name, target.aux, &face)) {
        return RotateProjectionLeverAux(target.name, target.aux,
                                        target.rotation_quarters, output);
    }
    direction = 0U;
    if (TryProjectionHorizontalDirection(target.name, target.aux, &direction)) {
        return RotateProjectionHorizontalDirectionAux(target.name, target.aux,
                                                       target.rotation_quarters, output);
    }
    *output = target.aux;
    return true;
}

bool isKnownBlockEntityShell(std::string_view canonical_name) {
    static constexpr std::string_view kContainerOrInteractiveTokens[] = {
        "command_block", "chest", "barrel", "hopper", "dispenser", "dropper",
        "shulker", "furnace", "smoker", "blast_furnace", "brewing_stand",
        "lectern", "sign", "banner", "beacon", "spawner", "end_portal",
        "jigsaw", "structure_block",
    };
    for (const std::string_view token : kContainerOrInteractiveTokens) {
        if (containsToken(canonical_name, token)) return true;
    }
    static constexpr std::string_view kExactNames[] = {
        "flower_pot", "jukebox", "beehive", "bee_nest", "crafter",
        "chiseled_bookshelf", "decorated_pot", "end_gateway", "trial_spawner",
        "vault", "bell", "campfire", "soul_campfire", "conduit", "lodestone",
        "suspicious_sand", "suspicious_gravel", "sculk_sensor",
        "calibrated_sculk_sensor", "sculk_shrieker", "sculk_catalyst",
        "noteblock", "note_block", "cauldron", "moving_block",
        "piston_arm_collision", "chemistry_table", "element_constructor",
        "material_reducer", "lab_table", "crafting_table", "stonecutter",
        "loom", "cartography_table", "smithing_table", "grindstone",
        "enchanting_table", "respawn_anchor", "cake", "nether_reactor", "bed",
    };
    for (const std::string_view exact : kExactNames) {
        if (canonical_name == exact) return true;
    }
    return containsToken(canonical_name, "player_head") ||
        containsToken(canonical_name, "_head") ||
        containsToken(canonical_name, "skull");
}

bool isComparisonAmbiguous(const ProjectionPrinterTarget& target,
                           std::string_view expected_name) {
    // Stair orientation has a compact, verified transformation below. Keep
    // every other rotated directional/stateful family conservative until it
    // has an equally explicit state transform.
    // Stair state is fully represented by the verified aux transformation and
    // public client snapshot, even when the source record marks it stateful.
    // Do not leave rotated imported stairs permanently "unknown" merely
    // because their source carried an explicit facing/half state.
    if (hasComparablePlacementState(target)) return false;
    return target.rotation_quarters != 0U &&
        (target.stateful() || isRotationSensitiveName(expected_name));
}

ProjectionWorldMatchState compareTarget(const PendingTarget& target,
                                        const NativeBlockView& actual,
                                        uint16_t comparison_aux,
                                        bool stair_state_available) {
    if (!actual.name) return ProjectionWorldMatchState::Unknown;
    const ProjectionBlockIdentity actual_identity =
        NormalizeLiveBlockIdentity(*actual.name, comparison_aux);
    // Air has no user-placed block to annotate. Leave the ordinary projection
    // visible rather than reporting an empty target as a wrong existing block.
    if (isAir(actual_identity.name)) return ProjectionWorldMatchState::Unknown;
    // Classify material identity before state ambiguity. A rotated or stateful
    // source can still reliably show red when the actual block type differs.
    if (actual_identity.name != target.expected_name) return ProjectionWorldMatchState::WrongBlock;
    if (target.state_comparison_ambiguous || target.entity_comparison_ambiguous) {
        return ProjectionWorldMatchState::Unknown;
    }
    // Do not turn an unavailable client state into a false yellow result for
    // stairs. The raw native field is known to be zero for several distinct
    // modern stair facings, so it cannot prove either equal or unequal.
    if (target.requires_stair_state && !stair_state_available) {
        return ProjectionWorldMatchState::Unknown;
    }
    return actual_identity.aux == target.expected_aux
        ? ProjectionWorldMatchState::Exact
        : ProjectionWorldMatchState::WrongState;
}

bool validInterestRegion(const ProjectionWorldMatchInterestRegion& region) noexcept {
    if (!region.isValid()) return false;
    const int64_t span_x = static_cast<int64_t>(region.max_x) - region.min_x + 1;
    const int64_t span_y = static_cast<int64_t>(region.max_y) - region.min_y + 1;
    const int64_t span_z = static_cast<int64_t>(region.max_z) - region.min_z + 1;
    return span_x > 0 && span_y > 0 && span_z > 0 &&
        span_x <= kMaximumInterestRegionSpan &&
        span_y <= kMaximumInterestRegionSpan &&
        span_z <= kMaximumInterestRegionSpan;
}

bool sameInterestGeometry(const ProjectionWorldMatchInterestRegion& left,
                          const ProjectionWorldMatchInterestRegion& right) noexcept {
    return left.min_x == right.min_x && left.min_y == right.min_y &&
        left.min_z == right.min_z && left.max_x == right.max_x &&
        left.max_y == right.max_y && left.max_z == right.max_z;
}

bool interestGeometryLess(const ProjectionWorldMatchInterestRegion& left,
                         const ProjectionWorldMatchInterestRegion& right) noexcept {
    if (left.min_x != right.min_x) return left.min_x < right.min_x;
    if (left.min_y != right.min_y) return left.min_y < right.min_y;
    if (left.min_z != right.min_z) return left.min_z < right.min_z;
    if (left.max_x != right.max_x) return left.max_x < right.max_x;
    if (left.max_y != right.max_y) return left.max_y < right.max_y;
    if (left.max_z != right.max_z) return left.max_z < right.max_z;
    return left.priority < right.priority;
}

uint64_t interestFingerprint(const std::vector<ProjectionWorldMatchInterestRegion>& regions)
    noexcept {
    uint64_t hash = UINT64_C(0x14650fb0739d0383);
    const auto add = [&](uint64_t value) {
        hash ^= mix64(value + UINT64_C(0x9e3779b97f4a7c15));
        hash = rotateLeft(hash, 11U) * UINT64_C(0x100000001b3);
    };
    add(regions.size());
    for (const ProjectionWorldMatchInterestRegion& region : regions) {
        add(static_cast<uint32_t>(region.min_x));
        add(static_cast<uint32_t>(region.min_y));
        add(static_cast<uint32_t>(region.min_z));
        add(static_cast<uint32_t>(region.max_x));
        add(static_cast<uint32_t>(region.max_y));
        add(static_cast<uint32_t>(region.max_z));
        // Priority is assigned from the current camera/frustum pass. It is a
        // scheduling hint only, not part of the source geometry being matched.
        // Including it here made a tiny camera movement look like a brand-new
        // projection and briefly discarded all Exact results.
    }
    const uint64_t result = mix64(hash);
    return result == 0U ? UINT64_C(1) : result;
}

bool normalizeInterest(const ProjectionWorldMatchInterestSnapshot& source,
                       std::vector<ProjectionWorldMatchInterestRegion>* output,
                       uint64_t* fingerprint) {
    if (!output || !fingerprint) return false;
    output->clear();
    const size_t count = std::min(source.regions.size(), kMaximumInterestRegions);
    try {
        output->reserve(count);
        for (size_t index = 0; index < count; ++index) {
            const ProjectionWorldMatchInterestRegion& region = source.regions[index];
            if (validInterestRegion(region)) output->push_back(region);
        }
        std::sort(output->begin(), output->end(), interestGeometryLess);
        auto destination = output->begin();
        for (auto current = output->begin(); current != output->end(); ++current) {
            if (destination == output->begin() ||
                !sameInterestGeometry(*(destination - 1), *current)) {
                *destination++ = *current;
            } else if (current->priority < (destination - 1)->priority) {
                (destination - 1)->priority = current->priority;
            }
        }
        output->erase(destination, output->end());
        // Keep the canonical geometry order established above.  The renderer
        // is free to reprioritize its candidates every frame, but that must not
        // change the identity of an otherwise identical comparison region set.
    } catch (...) {
        output->clear();
        return false;
    }
    *fingerprint = interestFingerprint(*output);
    return true;
}

}  // namespace

bool RotateProjectionStairAux(uint16_t source_aux, uint8_t rotation_quarters,
                              uint16_t* output) noexcept {
    if (!output || (source_aux & ~UINT16_C(0x0007)) != 0U) return false;

    // weirdo_direction is ordered east, west, south, north rather than around
    // the compass. This permutation maps it to the clockwise E,S,W,N order;
    // it is also its own inverse.
    static constexpr std::array<uint8_t, 4U> kStairAuxToClockwise{{0U, 2U, 1U, 3U}};
    const uint8_t clockwise_direction =
        kStairAuxToClockwise[source_aux & UINT16_C(0x0003)];
    const uint8_t rotated_direction = static_cast<uint8_t>(
        (clockwise_direction + (rotation_quarters & 0x03U)) & 0x03U);
    *output = static_cast<uint16_t>(
        (source_aux & UINT16_C(0x0004)) |
        kStairAuxToClockwise[rotated_direction]);
    return true;
}

const ProjectionWorldMatchGroupState* FindProjectionWorldMatchGroupState(
        const ProjectionWorldMatchSnapshot* snapshot,
        int32_t group_origin_x, int32_t group_origin_y,
        int32_t group_origin_z) noexcept {
    if (!snapshot) return nullptr;
    const ProjectionWorldMatchGroupKey key = groupKeyForOrigin(
        group_origin_x, group_origin_y, group_origin_z);
    const auto entry = snapshot->groups.find(key);
    return entry == snapshot->groups.end() || !entry->second ? nullptr : entry->second.get();
}

uint64_t GetProjectionWorldMatchGroupFingerprint(
        const ProjectionWorldMatchSnapshot* snapshot,
        int32_t group_origin_x, int32_t group_origin_y,
        int32_t group_origin_z) noexcept {
    const ProjectionWorldMatchGroupState* group = FindProjectionWorldMatchGroupState(
        snapshot, group_origin_x, group_origin_y, group_origin_z);
    return group ? group->fingerprint : 0U;
}

ProjectionWorldMatchState GetProjectionWorldMatchGroupCellState(
        const ProjectionWorldMatchGroupState* group,
        uint8_t local_x, uint8_t local_y, uint8_t local_z) noexcept {
    if (!group || local_x >= kProjectionWorldMatchGroupSpan ||
        local_y >= kProjectionWorldMatchGroupSpan ||
        local_z >= kProjectionWorldMatchGroupSpan) {
        return ProjectionWorldMatchState::Unknown;
    }
    const uint8_t raw = group->cells[groupCellIndex(local_x, local_y, local_z)];
    switch (raw) {
        case static_cast<uint8_t>(ProjectionWorldMatchState::WrongBlock):
            return ProjectionWorldMatchState::WrongBlock;
        case static_cast<uint8_t>(ProjectionWorldMatchState::WrongState):
            return ProjectionWorldMatchState::WrongState;
        case static_cast<uint8_t>(ProjectionWorldMatchState::Exact):
            return ProjectionWorldMatchState::Exact;
        default:
            return ProjectionWorldMatchState::Unknown;
    }
}

ProjectionWorldMatchState GetProjectionWorldMatchStateAt(
        const ProjectionWorldMatchSnapshot* snapshot,
        int32_t world_x, int32_t world_y, int32_t world_z) noexcept {
    if (!snapshot) return ProjectionWorldMatchState::Unknown;
    const ProjectionWorldMatchGroupKey key = groupKeyForWorldPosition(
        world_x, world_y, world_z);
    const auto entry = snapshot->groups.find(key);
    if (entry == snapshot->groups.end() || !entry->second) {
        return ProjectionWorldMatchState::Unknown;
    }
    return GetProjectionWorldMatchGroupCellState(
        entry->second.get(), localGroupCoordinate(world_x, key.x),
        localGroupCoordinate(world_y, key.y), localGroupCoordinate(world_z, key.z));
}

std::shared_ptr<const ProjectionWorldMatchSnapshot>
GetProjectionWorldMatchSnapshot() noexcept {
    return std::atomic_load_explicit(&g_published_snapshot, std::memory_order_acquire);
}

void PublishProjectionWorldMatchInterest(
        std::shared_ptr<const ProjectionWorldMatchInterestSnapshot> snapshot) noexcept {
    if (!snapshot) {
        ClearProjectionWorldMatchInterest();
        return;
    }
    std::atomic_store_explicit(&g_published_interest, std::move(snapshot),
                               std::memory_order_release);
}

void ClearProjectionWorldMatchInterest() noexcept {
    std::atomic_store_explicit(&g_published_interest, g_empty_interest,
                               std::memory_order_release);
}

std::shared_ptr<const ProjectionWorldMatchInterestSnapshot>
GetProjectionWorldMatchInterestSnapshot() noexcept {
    return std::atomic_load_explicit(&g_published_interest, std::memory_order_acquire);
}

struct ProjectionWorldMatchRuntime::RuntimeState {
    NativeWorldReader reader;
    // Native Block objects are interned by complete block state. This cache is
    // therefore a bounded, per-BlockSource translation from a stair's native
    // state token to the command-compatible orientation aux.
    uint64_t stair_state_reader_generation = 0;
    size_t stair_state_snapshot_queries_remaining = 0;
    std::unordered_map<const void*, StairStateCacheEntry> stair_state_cache;
    uint64_t projection_generation = 0;
    uint64_t plan_identity = 0;
    uint64_t next_revision = 0;
    uintptr_t dimension_token = 0;
    std::chrono::steady_clock::time_point next_state_publish{};
    // True after a transient player/world-reader failure has deliberately
    // published an empty view. It avoids allocating an identical snapshot on
    // every teardown tick while still guaranteeing stale Exact is removed once.
    bool fail_open_published = false;

    std::shared_ptr<const ProjectionWorldMatchInterestSnapshot> active_interest;
    uint64_t interest_fingerprint = 0;
    std::vector<ProjectionWorldMatchInterestRegion> interest_regions;
    std::deque<InterestWork> work_queue;

    bool active_work_valid = false;
    InterestWork active_work;
    bool active_page_complete = false;
    std::vector<PendingTarget> pending_targets;
    size_t pending_index = 0;

    // Mutable owner pointers are never written while a published snapshot
    // shares them. setMatchStateLocked clones a single 4 KiB page first.
    std::unordered_map<ProjectionWorldMatchGroupKey,
                       std::shared_ptr<ProjectionWorldMatchGroupState>,
                       ProjectionWorldMatchGroupKeyHash> working_groups;
};

namespace {

bool findJsonPropertyValue(std::string_view json, std::string_view key,
                           size_t* output) noexcept {
    if (!output || key.empty()) return false;
    size_t cursor = 0U;
    while (cursor < json.size()) {
        const size_t quote = json.find('"', cursor);
        if (quote == std::string_view::npos ||
            quote + key.size() + 1U >= json.size()) {
            return false;
        }
        const size_t key_start = quote + 1U;
        const size_t closing_quote = key_start + key.size();
        if (json.compare(key_start, key.size(), key) == 0 &&
            json[closing_quote] == '"') {
            *output = closing_quote + 1U;
            return true;
        }
        cursor = quote + 1U;
    }
    return false;
}

bool parseJsonIntegerProperty(std::string_view json, std::string_view key,
                               int* output) noexcept {
    if (!output || key.empty()) return false;
    size_t cursor = 0U;
    if (!findJsonPropertyValue(json, key, &cursor)) return false;
    while (cursor < json.size() &&
           (json[cursor] == ' ' || json[cursor] == '\t' ||
            json[cursor] == '\r' || json[cursor] == '\n' ||
            json[cursor] == ':')) {
        ++cursor;
    }
    if (cursor >= json.size()) return false;
    bool negative = false;
    if (json[cursor] == '-') {
        negative = true;
        ++cursor;
    }
    if (cursor >= json.size() || json[cursor] < '0' || json[cursor] > '9') {
        return false;
    }
    int value = 0;
    while (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9') {
        const int digit = json[cursor] - '0';
        if (value > (std::numeric_limits<int>::max() - digit) / 10) return false;
        value = value * 10 + digit;
        ++cursor;
    }
    *output = negative ? -value : value;
    return true;
}

bool parseJsonBooleanProperty(std::string_view json, std::string_view key,
                               bool* output) noexcept {
    if (!output || key.empty()) return false;
    size_t cursor = 0U;
    if (!findJsonPropertyValue(json, key, &cursor)) return false;
    while (cursor < json.size() &&
           (json[cursor] == ' ' || json[cursor] == '\t' ||
            json[cursor] == '\r' || json[cursor] == '\n' ||
            json[cursor] == ':')) {
        ++cursor;
    }
    if (cursor + 4U <= json.size() && json.substr(cursor, 4U) == "true") {
        *output = true;
        return true;
    }
    if (cursor + 5U <= json.size() && json.substr(cursor, 5U) == "false") {
        *output = false;
        return true;
    }
    if (cursor < json.size() && (json[cursor] == '0' || json[cursor] == '1')) {
        *output = json[cursor] == '1';
        return true;
    }
    return false;
}

bool isStairSnapshotIdentifier(std::string_view identifier) noexcept {
    const size_t state_begin = identifier.find('[');
    if (state_begin != std::string_view::npos) identifier = identifier.substr(0U, state_begin);
    constexpr std::string_view kNamespace = "minecraft:";
    if (identifier.compare(0U, kNamespace.size(), kNamespace) == 0) {
        identifier.remove_prefix(kNamespace.size());
    }
    return identifier.find("stairs") != std::string_view::npos ||
        identifier == "normal_stone_stairs";
}

bool stairAuxFromSnapshot(const NativeBlockSnapshot& snapshot,
                           uint16_t* output) noexcept {
    if (!output) return false;
    // GetBlock(name, aux) is the public, command-compatible representation.
    // If a client builds it, it is preferable to ABI-dependent Block fields.
    if (snapshot.client_aux_available &&
        (!snapshot.client_block_available ||
          isStairSnapshotIdentifier(snapshot.client_identifier))) {
        *output = snapshot.client_aux;
        return true;
    }
    if (!snapshot.state_available || snapshot.state_json.empty()) return false;
    int direction = -1;
    if (!parseJsonIntegerProperty(snapshot.state_json, "weirdo_direction", &direction) ||
        direction < 0 || direction > 3) {
        return false;
    }
    bool upside_down = false;
    // A missing upside_down_bit represents the ordinary lower-half stair.
    parseJsonBooleanProperty(snapshot.state_json, "upside_down_bit", &upside_down);
    *output = static_cast<uint16_t>((direction & 3) | (upside_down ? 4 : 0));
    return true;
}

template <typename State>
bool resolveCachedStairAux(
        State* state,
        const ProjectionWorldMatchPosition& position,
        const NativeBlockView& actual, uint16_t* output) {
    if (!state || !output || !actual.type_token) return false;
    const auto now = std::chrono::steady_clock::now();
    auto cached = state->stair_state_cache.find(actual.type_token);
    if (cached != state->stair_state_cache.end()) {
        if (cached->second.has_aux) {
            *output = cached->second.aux;
            return true;
        }
        if (now < cached->second.retry_after) return false;
    }
    if (state->stair_state_snapshot_queries_remaining == 0U) return false;
    --state->stair_state_snapshot_queries_remaining;

    NativeBlockSnapshot snapshot;
    uint16_t resolved_aux = 0;
    const bool resolved = NativeWorldAccess::getBlockSnapshot(
        position.x, position.y, position.z, &snapshot, false, 0) &&
        TryResolveProjectionStairAux(snapshot, &resolved_aux);

    StairStateCacheEntry entry;
    entry.has_aux = resolved;
    entry.aux = resolved_aux;
    if (!resolved) entry.retry_after = now + kUnavailableStairStateRetry;
    if (cached != state->stair_state_cache.end()) {
        cached->second = entry;
    } else {
        if (state->stair_state_cache.size() >= kMaximumCachedStairStateTokens) {
            // The current BlockSource normally has only eight stair states.
            // Clearing this bounded presentation cache is safer than retaining
            // potentially stale state tokens across an unusual ABI change.
            state->stair_state_cache.clear();
        }
        state->stair_state_cache.emplace(actual.type_token, entry);
    }
    if (!resolved) return false;
    *output = resolved_aux;
    return true;
}

template <typename State>
void publishLocked(State* state) noexcept {
    if (!state) {
        std::atomic_store_explicit(&g_published_snapshot, g_empty_snapshot,
                                   std::memory_order_release);
        return;
    }
    try {
        auto snapshot = std::make_shared<ProjectionWorldMatchSnapshot>();
        snapshot->projection_generation = state->projection_generation;
        snapshot->plan_identity = state->plan_identity;
        snapshot->revision = ++state->next_revision;
        snapshot->groups.reserve(state->working_groups.size());
        for (const auto& entry : state->working_groups) {
            if (entry.second && entry.second->non_default_count != 0U) {
                snapshot->groups.emplace(entry.first,
                    std::shared_ptr<const ProjectionWorldMatchGroupState>(entry.second));
            }
        }
        std::shared_ptr<const ProjectionWorldMatchSnapshot> immutable = std::move(snapshot);
        std::atomic_store_explicit(&g_published_snapshot, std::move(immutable),
                                   std::memory_order_release);
        state->next_state_publish = std::chrono::steady_clock::now() +
            kStatePublishInterval;
    } catch (...) {
        // Leaving an old Exact snapshot resident could hide source blocks in a
        // world we can no longer prove. An allocation failure therefore fails
        // open to the normal projection view rather than retaining a stale map.
        std::atomic_store_explicit(&g_published_snapshot, g_empty_snapshot,
                                   std::memory_order_release);
    }
}

template <typename State>
void resetPageWorkLocked(State* state) {
    if (!state) return;
    state->work_queue.clear();
    state->active_work_valid = false;
    state->active_work = {};
    state->active_page_complete = false;
    state->pending_targets.clear();
    state->pending_index = 0;
    for (const ProjectionWorldMatchInterestRegion& region : state->interest_regions) {
        state->work_queue.push_back({region, 0, {}});
    }
}

template <typename State>
void resetComparisonLocked(State* state, uint64_t generation, uint64_t plan_identity,
                           bool reset_reader) {
    if (!state) return;
    if (reset_reader) state->reader.reset();
    state->projection_generation = generation;
    state->plan_identity = plan_identity;
    state->fail_open_published = false;
    state->working_groups.clear();
    resetPageWorkLocked(state);
}

template <typename State>
void withdrawComparisonLocked(State* state) {
    if (!state) return;
    state->reader.reset();
    state->projection_generation = 0;
    state->plan_identity = 0;
    state->dimension_token = 0;
    state->fail_open_published = false;
    state->active_interest.reset();
    state->interest_fingerprint = 0;
    state->interest_regions.clear();
    state->working_groups.clear();
    state->work_queue.clear();
    state->active_work_valid = false;
    state->active_work = {};
    state->active_page_complete = false;
    state->pending_targets.clear();
    state->pending_index = 0;
}

template <typename State>
bool installInterestLocked(
        State* state,
        std::shared_ptr<const ProjectionWorldMatchInterestSnapshot> interest,
        std::vector<ProjectionWorldMatchInterestRegion> regions,
        uint64_t fingerprint) {
    if (!state || !interest || interest->plan_identity == 0U) return false;
    state->active_interest = std::move(interest);
    state->interest_fingerprint = fingerprint;
    state->interest_regions = std::move(regions);
    state->dimension_token = 0;
    resetComparisonLocked(state, 0, state->active_interest->plan_identity, true);
    return true;
}

template <typename State>
std::shared_ptr<ProjectionWorldMatchGroupState> mutableGroupLocked(
        State* state, const ProjectionWorldMatchGroupKey& key, bool create) {
    if (!state) return nullptr;
    auto entry = state->working_groups.find(key);
    if (entry == state->working_groups.end()) {
        if (!create) return nullptr;
        auto group = std::make_shared<ProjectionWorldMatchGroupState>();
        state->working_groups.emplace(key, group);
        return group;
    }
    if (!entry->second) {
        if (!create) return nullptr;
        entry->second = std::make_shared<ProjectionWorldMatchGroupState>();
    } else if (entry->second.use_count() > 1L) {
        // The current immutable snapshot still owns this page. Copy only this
        // one group before changing it; all other snapshot pages stay shared.
        entry->second = std::make_shared<ProjectionWorldMatchGroupState>(*entry->second);
    }
    return entry->second;
}

template <typename State>
bool setMatchStateLocked(State* state, const ProjectionWorldMatchPosition& position,
                         ProjectionWorldMatchState value) {
    if (!state) return false;
    const uint8_t desired = static_cast<uint8_t>(value);
    const uint8_t normalized = desired <= static_cast<uint8_t>(ProjectionWorldMatchState::Exact)
        ? desired : static_cast<uint8_t>(ProjectionWorldMatchState::Unknown);
    const ProjectionWorldMatchGroupKey key = groupKeyForWorldPosition(
        position.x, position.y, position.z);
    const size_t cell_index = groupCellIndex(
        localGroupCoordinate(position.x, key.x),
        localGroupCoordinate(position.y, key.y),
        localGroupCoordinate(position.z, key.z));

    const auto existing = state->working_groups.find(key);
    if (existing == state->working_groups.end() && normalized == 0U) return false;
    if (existing != state->working_groups.end() && existing->second &&
        existing->second->cells[cell_index] == normalized) {
        return false;
    }

    std::shared_ptr<ProjectionWorldMatchGroupState> group = mutableGroupLocked(
        state, key, normalized != 0U);
    if (!group) return false;
    const uint8_t previous = group->cells[cell_index];
    if (previous == normalized) return false;
    if (previous != 0U) {
        const uint64_t hash = groupCellHash(cell_index, previous);
        group->xor_hash ^= hash;
        group->additive_hash -= hash * UINT64_C(0x9e3779b97f4a7c15);
        if (group->non_default_count != 0U) --group->non_default_count;
    }
    if (normalized != 0U) {
        const uint64_t hash = groupCellHash(cell_index, normalized);
        group->xor_hash ^= hash;
        group->additive_hash += hash * UINT64_C(0x9e3779b97f4a7c15);
        ++group->non_default_count;
    }
    group->cells[cell_index] = normalized;
    updateGroupFingerprint(group.get());
    if (group->non_default_count == 0U) {
        state->working_groups.erase(key);
    }
    return true;
}

template <typename State>
void finishActivePageLocked(State* state,
                            std::chrono::steady_clock::time_point now) {
    if (!state || !state->active_work_valid) return;
    InterestWork work = std::move(state->active_work);
    if (state->active_page_complete || work.cursor == 0U) {
        work.cursor = 0U;
        work.retry_after = now + kCompletedRegionRefresh;
    } else {
        work.retry_after = now;
    }
    state->work_queue.push_back(std::move(work));
    state->active_work_valid = false;
    state->active_work = {};
    state->active_page_complete = false;
    state->pending_targets.clear();
    state->pending_index = 0;
}

template <typename State>
bool populatePendingTargetsLocked(
        State* state, const ProjectionWorldMatchTargetPage& page) {
    if (!state) return false;
    try {
        state->pending_targets.clear();
        state->pending_index = 0;
        state->pending_targets.reserve(page.targets.size());
        for (const ProjectionPrinterTarget& target : page.targets) {
            uint16_t transformed_aux = 0U;
            const bool state_transform_valid =
                effectiveComparableAux(target, &transformed_aux);
            const ProjectionBlockIdentity expected_identity =
                NormalizeProjectionBlockIdentity(target.name, transformed_aux);
            const std::string expected_name = expected_identity.name;
            if (expected_name.empty()) continue;
            const bool is_stair = isStairName(expected_name);
            const bool state_comparison_ambiguous =
                !state_transform_valid || isComparisonAmbiguous(target, expected_name);
            const bool entity_comparison_ambiguous =
                isKnownBlockEntityShell(expected_name) &&
                !IsProjectionContentlessContainerShell(target.name);
            state->pending_targets.push_back({
                {target.x, target.y, target.z},
                expected_name,
                expected_identity.aux,
                state_comparison_ambiguous,
                entity_comparison_ambiguous,
                // Stairs use a public client state probe because their raw
                // native aux cannot distinguish every modern orientation.
                !state_comparison_ambiguous && !entity_comparison_ambiguous &&
                    is_stair,
            });
        }
    } catch (...) {
        state->pending_targets.clear();
        state->pending_index = 0;
        return false;
    }
    return true;
}

enum class PageAcquireResult : uint8_t {
    None,
    Ready,
    Reset,
};

template <typename State>
PageAcquireResult acquireNextPageLocked(
        State* state, std::chrono::steady_clock::time_point now) {
    if (!state || !state->active_interest || state->active_interest->plan_identity == 0U) {
        return PageAcquireResult::None;
    }
    for (size_t attempt = 0; attempt < kMaximumPageRequestsPerTick; ++attempt) {
        if (state->work_queue.empty()) return PageAcquireResult::None;
        InterestWork work = std::move(state->work_queue.front());
        state->work_queue.pop_front();
        if (work.retry_after > now) {
            state->work_queue.push_back(std::move(work));
            continue;
        }

        const ProjectionWorldMatchRegion region{
            work.region.min_x, work.region.min_y, work.region.min_z,
            work.region.max_x, work.region.max_y, work.region.max_z,
        };
        ProjectionWorldMatchTargetPage page;
        std::string ignored_error;
        const bool queried = BuildProjectionRuntime::instance().
            queryProjectionWorldMatchTargetPage(region, work.cursor,
                                                kMaximumSourceTargetsPerPage,
                                                &page, &ignored_error);
        const ProjectionWorldMatchTargetPageStatus status = page.status;
        if (!queried || status == ProjectionWorldMatchTargetPageStatus::SourceUnavailable) {
            // A source clear/read failure must never leave old Exact states in
            // the renderer. Keep the current interest and retry after a short
            // backoff so a still-loading source can recover.
            state->reader.reset();
            state->projection_generation = 0;
            state->working_groups.clear();
            resetPageWorkLocked(state);
            for (InterestWork& retry : state->work_queue) {
                retry.retry_after = now + kSourceUnavailableRetry;
            }
            publishLocked(state);
            return PageAcquireResult::Reset;
        }
        if (status == ProjectionWorldMatchTargetPageStatus::NoProjection) {
            withdrawComparisonLocked(state);
            publishLocked(state);
            return PageAcquireResult::Reset;
        }
        if (status == ProjectionWorldMatchTargetPageStatus::InvalidQuery) {
            // An invalid renderer region is isolated to this item. Dropping it
            // is safer than retrying a malformed request forever.
            continue;
        }
        if (status == ProjectionWorldMatchTargetPageStatus::Pending) {
            // A target-page Pending result means its compact raw partition is
            // still being decoded (or its short-lived cache lock is busy).
            // Keep this exact cursor at the head of the work queue so the
            // next query can consume the newly warm entry.  Rotating it behind
            // every visible region defeats the bounded 12-partition source
            // cache: a large view can evict the requested entry before this
            // cursor gets another chance to read it, leaving every page cold
            // forever.  The retry deadline keeps this priority wait out of
            // the hot game-tick path; Ready/Complete pages still flow through
            // finishActivePageLocked() and resume normal round-robin service.
            work.retry_after = now + kPendingPageRetry;
            state->work_queue.push_front(std::move(work));
            return PageAcquireResult::None;
        }
        if (status != ProjectionWorldMatchTargetPageStatus::Ready &&
            status != ProjectionWorldMatchTargetPageStatus::Complete) {
            work.retry_after = now + kPendingPageRetry;
            state->work_queue.push_back(std::move(work));
            continue;
        }
        if (page.plan_identity == 0U ||
            page.plan_identity != state->active_interest->plan_identity) {
            // The raw source changed under a new renderer plan. Restart from
            // the immutable interest rather than attaching page data to it.
            resetComparisonLocked(state, 0, state->active_interest->plan_identity, true);
            publishLocked(state);
            return PageAcquireResult::Reset;
        }
        if (state->projection_generation != 0U &&
            state->projection_generation != page.generation) {
            resetComparisonLocked(state, 0, state->active_interest->plan_identity, true);
            publishLocked(state);
            return PageAcquireResult::Reset;
        }
        state->projection_generation = page.generation;
        state->plan_identity = page.plan_identity;
        const uint64_t original_cursor = work.cursor;
        work.cursor = page.next_cursor;
        state->active_work = std::move(work);
        state->active_work_valid = true;
        state->active_page_complete =
            status == ProjectionWorldMatchTargetPageStatus::Complete ||
            page.next_cursor == 0U;
        if (!populatePendingTargetsLocked(state, page)) {
            // Preserve the page cursor when a local allocation fails; no source
            // records are skipped and the next tick can retry the same page.
            state->active_work.cursor = original_cursor;
            state->active_page_complete = false;
            state->active_work.retry_after = now + kPendingPageRetry;
            state->work_queue.push_front(std::move(state->active_work));
            state->active_work_valid = false;
            state->active_work = {};
            return PageAcquireResult::None;
        }
        if (state->pending_targets.empty()) {
            finishActivePageLocked(state, now);
            continue;
        }
        return PageAcquireResult::Ready;
    }
    return PageAcquireResult::None;
}

template <typename State>
bool stateHasLiveComparison(const State& state) {
    return state.plan_identity != 0U || !state.working_groups.empty() ||
        !state.work_queue.empty() || state.active_work_valid ||
        !state.pending_targets.empty() || state.active_interest != nullptr;
}

}  // namespace

bool TryResolveProjectionStairAux(const NativeBlockSnapshot& snapshot,
                                  uint16_t* output) noexcept {
    return stairAuxFromSnapshot(snapshot, output);
}

ProjectionWorldMatchRuntime& ProjectionWorldMatchRuntime::instance() {
    static ProjectionWorldMatchRuntime runtime;
    return runtime;
}

ProjectionWorldMatchRuntime::~ProjectionWorldMatchRuntime() = default;

void ProjectionWorldMatchRuntime::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runtime_) {
        std::atomic_store_explicit(&g_published_snapshot, g_empty_snapshot,
                                   std::memory_order_release);
        return;
    }
    RuntimeState& state = *runtime_;
    if (stateHasLiveComparison(state)) {
        withdrawComparisonLocked(&state);
        publishLocked(&state);
    } else {
        std::atomic_store_explicit(&g_published_snapshot, g_empty_snapshot,
                                   std::memory_order_release);
    }
}

void ProjectionWorldMatchRuntime::onGameTick() {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!runtime_) runtime_ = std::make_unique<RuntimeState>();
        RuntimeState& state = *runtime_;

        // A hidden projection must never keep autonomous comparison work alive.
        // This is also the fast lifecycle boundary for authorization revocation.
        if (!BuildProjectionRenderer::instance().isEnabled()) {
            if (stateHasLiveComparison(state)) {
                withdrawComparisonLocked(&state);
                publishLocked(&state);
            }
            return;
        }

        const std::shared_ptr<const ProjectionWorldMatchInterestSnapshot> interest =
            GetProjectionWorldMatchInterestSnapshot();
        if (!interest || interest->plan_identity == 0U || interest->regions.empty()) {
            if (stateHasLiveComparison(state)) {
                withdrawComparisonLocked(&state);
                publishLocked(&state);
            }
            return;
        }

        if (interest.get() != state.active_interest.get()) {
            std::vector<ProjectionWorldMatchInterestRegion> normalized_regions;
            uint64_t normalized_fingerprint = 0;
            if (!normalizeInterest(*interest, &normalized_regions, &normalized_fingerprint) ||
                normalized_regions.empty()) {
                if (stateHasLiveComparison(state)) {
                    withdrawComparisonLocked(&state);
                    publishLocked(&state);
                }
                return;
            }
            const bool changed = !state.active_interest ||
                state.plan_identity != interest->plan_identity ||
                state.interest_fingerprint != normalized_fingerprint;
            if (changed) {
                const bool is_new_plan = !state.active_interest ||
                    state.plan_identity != interest->plan_identity;
                if (is_new_plan) {
                    if (!installInterestLocked(&state, interest,
                                               std::move(normalized_regions),
                                               normalized_fingerprint)) {
                        withdrawComparisonLocked(&state);
                        publishLocked(&state);
                        return;
                    }
                    // A different projection may reuse the same group keys,
                    // so it must fail open until its own source is compared.
                    publishLocked(&state);
                } else {
                    // A projection render pass publishes only the currently
                    // visible regions.  Frustum edges and draw-budget changes
                    // therefore alter this list continually.  Keep previously
                    // verified results for the same immutable plan and simply
                    // requeue reads for the new visible set; clearing here made
                    // Exact blocks reappear and flicker every time the camera
                    // moved slightly.
                    state.active_interest = interest;
                    state.interest_fingerprint = normalized_fingerprint;
                    state.interest_regions = std::move(normalized_regions);
                    resetPageWorkLocked(&state);
                }
            } else {
                // Renderer may republish an equivalent snapshot object. Retain
                // progress rather than resetting the round-robin cursor.
                state.active_interest = interest;
            }
        }

        // Native world pointers are valid only on the local-player game tick.
        // Keep the existing player availability guard even though interest is
        // centered on the render camera rather than player interaction reach.
        int32_t ignored_player_x = 0;
        int32_t ignored_player_y = 0;
        int32_t ignored_player_z = 0;
        if (!NativeWorldAccess::getLocalPlayerBlockPosition(
                &ignored_player_x, &ignored_player_y, &ignored_player_z)) {
            // Player/level teardown can make the position helper fail for a
            // few ticks before the renderer is disabled. Do not retain an old
            // Exact page during that gap: fail open until a live local player
            // and BlockSource are available again.
            if (!state.fail_open_published) {
                state.reader.reset();
                state.working_groups.clear();
                resetPageWorkLocked(&state);
                publishLocked(&state);
                state.fail_open_published = true;
            }
            return;
        }
        state.fail_open_published = false;

        const uintptr_t current_dimension_token = NativeWorldAccess::dimensionToken();
        if (state.dimension_token != 0U &&
            state.dimension_token != current_dimension_token) {
            // Target classifications belong to a BlockSource. Even when the
            // projection source survives a world switch, old Exact pages must
            // disappear before the new world is read.
            state.dimension_token = current_dimension_token;
            resetComparisonLocked(&state, 0,
                                  state.active_interest
                                      ? state.active_interest->plan_identity : 0U,
                                  true);
            publishLocked(&state);
        } else if (state.dimension_token == 0U && current_dimension_token != 0U) {
            state.dimension_token = current_dimension_token;
        }

        const auto now = std::chrono::steady_clock::now();
        if (state.pending_index >= state.pending_targets.size()) {
            if (state.active_work_valid) finishActivePageLocked(&state, now);
            const PageAcquireResult acquired = acquireNextPageLocked(&state, now);
            if (acquired == PageAcquireResult::Reset ||
                state.pending_index >= state.pending_targets.size()) {
                return;
            }
        }

        if (!state.reader.open()) {
            // If the active source becomes unreadable, failing open is safer
            // than retaining Exact state which would hide visible projection.
            if (!state.fail_open_published) {
                state.working_groups.clear();
                publishLocked(&state);
                state.fail_open_published = true;
            }
            state.reader.reset();
            return;
        }
        state.fail_open_published = false;
        const uint64_t reader_generation = state.reader.sourceGeneration();
        if (state.stair_state_reader_generation != reader_generation) {
            // Native Block pointers belong to this BlockSource only. Never
            // reuse a cached stair orientation after a region/world switch.
            state.stair_state_cache.clear();
            state.stair_state_reader_generation = reader_generation;
        }
        state.stair_state_snapshot_queries_remaining =
            kMaximumStairStateSnapshotsPerTick;

        bool changed = false;
        size_t reads = 0;
        while (state.pending_index < state.pending_targets.size() &&
               reads < kMaximumWorldReadsPerTick) {
            const PendingTarget& target = state.pending_targets[state.pending_index++];
            ++reads;
            NativeBlockView actual;
            ProjectionWorldMatchState match = ProjectionWorldMatchState::Unknown;
            if (state.reader.getBlockView(target.position.x, target.position.y,
                                          target.position.z, &actual)) {
                uint16_t comparison_aux = actual.aux;
                bool stair_state_available = !target.requires_stair_state;
                // Material identity is cheap to establish from the native
                // reader. Query the public state API only for a same-material
                // stair whose raw aux cannot distinguish its orientation.
                if (target.requires_stair_state && actual.name &&
                    NormalizeLiveBlockIdentity(*actual.name, actual.aux).name ==
                        target.expected_name) {
                    stair_state_available = resolveCachedStairAux(
                        &state, target.position, actual, &comparison_aux);
                }
                match = compareTarget(target, actual, comparison_aux,
                                      stair_state_available);
            }
            // An unreadable coordinate deliberately clears an older result;
            // stale Exact must not survive chunk unloads or ABI read failures.
            if (setMatchStateLocked(&state, target.position, match)) changed = true;
        }

        const bool page_finished = state.pending_index >= state.pending_targets.size();
        if (page_finished) finishActivePageLocked(&state, now);
        if (changed && (page_finished || now >= state.next_state_publish)) {
            publishLocked(&state);
        }
    } catch (...) {
        // Hooks must not propagate allocation/ABI failures into the client.
        // Drop the public snapshot so any last verified Exact state cannot hide
        // projection geometry while the next game tick rebuilds its source page.
        // The lock from the try block has already unwound here, so also discard
        // private COW pages; otherwise a later successful tick could republish
        // stale Exact state after this fail-open publication.
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (runtime_) withdrawComparisonLocked(runtime_.get());
        } catch (...) {
        }
        std::atomic_store_explicit(&g_published_snapshot, g_empty_snapshot,
                                   std::memory_order_release);
    }
}

}  // namespace build_import
