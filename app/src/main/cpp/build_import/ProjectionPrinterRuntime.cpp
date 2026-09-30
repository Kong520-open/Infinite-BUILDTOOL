#include "ProjectionPrinterRuntime.h"

#include "BuildProjectionRuntime.h"
#include "BuildProjectionRenderer.h"
#include "ContainerClosePacketSender.h"
#include "ContainerOpenPacketSender.h"
#include "NativeWorldAccess.h"
#include "PlayerInventoryOpenPacketSender.h"
#include "ProjectionPrinterInventoryMailbox.h"
#include "ProjectionPrinterInventoryClientSync.h"
#include "ProjectionPrinterInventoryDiagnostics.h"
#include "ProjectionPrinterInventoryMover.h"
#include "ProjectionPrinterInventoryMoveEvidence.h"
#include "ProjectionPrinterNativeHotbarSelection.h"
#include "ProjectionPrinterInventoryPresentationGate.h"
#include "ProjectionPrinterInventorySession.h"
#include "ProjectionPrinterSilentRotation.h"
#include "ProjectionBlockIdentity.h"
#include "ProjectionWorldMatchRuntime.h"
#include "../tp/PythonUtils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace build_import {
namespace {

constexpr int32_t kMinimumPrinterRate = 1;
constexpr int32_t kMaximumPrinterRate = 20;
constexpr int32_t kDefaultPrinterRate = 4;
constexpr int32_t kInteractionHorizontalRadius = 6;
constexpr int32_t kInteractionVerticalRadius = 6;
constexpr size_t kMaximumCandidateTargets = 160U;
constexpr uint8_t kProjectionRecordStateful = 0x08U;
constexpr auto kPlacementConfirmationTimeout = std::chrono::milliseconds(1200);
constexpr auto kTransientFailureRetry = std::chrono::milliseconds(300);
constexpr auto kInventoryRetryDelay = std::chrono::milliseconds(150);
constexpr auto kConfirmationRetryDelay = std::chrono::milliseconds(600);
constexpr auto kSilentRotationConfirmationTimeout = std::chrono::milliseconds(750);
constexpr auto kInventoryMoveConfirmationTimeout = std::chrono::milliseconds(2500);
constexpr auto kInventoryMoveFailureDelay = std::chrono::milliseconds(900);
constexpr auto kInventoryOpenConfirmationTimeout = std::chrono::milliseconds(2500);
// The material overlay polls from a Kotlin worker, but item components may be
// read only from this local-player tick.  Keep that bridge deliberately slow
// enough to remain invisible to the printer's normal dispatch work.
constexpr auto kMaterialInventoryRefreshInterval = std::chrono::milliseconds(650);
constexpr auto kMaterialInventoryMaximumAge = std::chrono::milliseconds(1800);
// The preview reads only the same nearby bounded source query and native block
// views that the printer already uses. Keep it responsive without turning an
// ordinary game tick into a repeated full interaction scan.
constexpr auto kReachabilityPreviewRefreshInterval = std::chrono::milliseconds(125);
// A response-confirmed material is used at most for the first automatic
// placement after a silent backpack -> hotbar exchange. Its native network
// stack ID is rechecked immediately before ItemUse, so this short bridge never
// trusts the embedded Python inventory component after it has gone stale.
constexpr auto kVerifiedHotbarOverrideLifetime = std::chrono::seconds(3);
// A successful ItemStackResponse updates the native container before every
// presentation/listener path has necessarily committed it to the hotbar UI.
// Keep the silent server window alive for three subsequent LocalPlayer ticks
// (~150ms at the normal client rate). This matches the reference inventory
// close settle interval and gives the source->hotbar local slot updates one
// receive poll plus one stock HUD commit before ContainerClose.
constexpr uint8_t kInventoryPostMoveUiCommitTicks = 3U;
constexpr uint8_t kProjectionPrinterHotbarRequestContainer = 29U;
constexpr uint8_t kProjectionPrinterInventoryRequestContainer = 30U;
constexpr uint8_t kMaximumPlacementConfirmationAttempts = 3U;
// State interactions (repeater delay, comparator mode and daylight inversion)
// are ordinary ItemUse packets too.  Bound their failed sends independently so
// a stale selected slot or an unavailable loopback sender can never turn one
// placed block into an endless packet loop.
constexpr uint8_t kMaximumStateAdjustmentSendFailures = 3U;

std::chrono::milliseconds printerDispatchInterval(int32_t configured_rate) {
    const int32_t safe_rate = std::max(kMinimumPrinterRate, configured_rate);
    return std::chrono::milliseconds(std::max<int64_t>(1, 1000LL / safe_rate));
}

std::chrono::milliseconds stateAdjustmentDispatchInterval(int32_t configured_rate) {
    // A state click must not exceed the user's printer rate.  The small retry
    // floor additionally gives the native world snapshot time to settle.
    return std::max(kTransientFailureRetry, printerDispatchInterval(configured_rate));
}

struct PrinterPosition {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    bool operator==(const PrinterPosition& other) const noexcept {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct PrinterPositionHash {
    size_t operator()(const PrinterPosition& value) const noexcept {
        const uint64_t x = static_cast<uint32_t>(value.x);
        const uint64_t y = static_cast<uint32_t>(value.y);
        const uint64_t z = static_cast<uint32_t>(value.z);
        return static_cast<size_t>((x * 0x9E3779B185EBCA87ULL) ^
                                   (y * 0xC2B2AE3D27D4EB4FULL) ^
                                   (z * 0x165667B19E3779F9ULL));
    }
};

enum class InventoryVerificationSource : uint8_t {
    None,
    Mailbox,
    LiveNative,
};

struct InventorySlot {
    int32_t slot = -1;
    std::string name;
    uint16_t aux = 0;
    int32_t count = 0;
    // The embedded-Python component gives us the user-facing material name.
    // A matching passive packet entry or a preceding read-only native snapshot
    // additionally gives us an authoritative stack-network ID.  The native
    // mover reads it once more at the write boundary, so this is never merely
    // a UI-visible inventory value.
    bool network_verified = false;
    int32_t runtime_item_id = 0;
    int32_t network_stack_id = 0;
};

struct NetworkInventorySlot {
    bool occupied = false;
    int32_t runtime_item_id = 0;
    bool has_network_stack_id = false;
    int32_t network_stack_id = 0;
    uint16_t count = 0;
    uint16_t aux = 0;
};

struct InventorySnapshot {
    bool available = false;
    int32_t selected_hotbar_slot = -1;
    std::vector<InventorySlot> slots;
    // Complete packet baseline, when it was observed. It remains the
    // preferred source because it carries semantic fields as well as IDs.
    bool mailbox_ready = false;
    uint64_t mailbox_session_generation = 0;
    uint64_t mailbox_revision = 0;
    // Keep the remote evidence even when selection prefers the live-native
    // identities. A local InventorySlot refresh cannot overwrite this copy.
    ProjectionPrinterInventorySnapshot remote_inventory;
    // When the receive hook attached after the login InventoryContent packet,
    // the read-only native snapshot captured before the Python semantic read
    // supplies the same identity guard. The mover rechecks it before sending.
    bool network_ready = false;
    InventoryVerificationSource network_source = InventoryVerificationSource::None;
    std::array<NetworkInventorySlot,
               kProjectionPrinterPlayerInventorySlotCount> network_slots{};
};

enum class MaterialSelection : uint8_t {
    Ready,
    SwitchingHotbar,
    InBackpack,
    Missing,
    Failed,
};

struct PlacementSupport {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int face = 1;
    const void* native_block = nullptr;
    bool has_click_override = false;
    ItemUseClickPosition click_override{};
    // The packet hook rewrites only the outgoing PlayerAuthInput yaw.  It
    // never changes the local camera, but the following ItemUse will still be
    // interpreted by the server with this target-facing orientation.
    bool requires_silent_yaw = false;
    float silent_yaw = 0.0F;
};

struct Candidate {
    ProjectionPrinterTarget target;
    PrinterPosition position;
    PlacementSupport support;
    double squared_distance = 0.0;
};

struct PendingPlacement {
    ProjectionPrinterTarget target;
    PrinterPosition position;
    std::chrono::steady_clock::time_point sent_at{};
    int32_t selected_hotbar_slot = -1;
    // Repeater/comparator adjustment sends exactly one normal block-use, then
    // waits for a fresh native world state before another action.  Never batch
    // clicks: a delayed server response could otherwise cycle past the desired
    // delay/mode.
    bool state_adjustment_pending = false;
    uint8_t state_adjustment_count = 0U;
    uint8_t state_adjustment_send_failures = 0U;
    uint16_t state_before_adjustment = 0U;
    // A redstone click is accepted only after the native world reaches this
    // exact static state.  Dynamic powered/locked bits are intentionally
    // stripped before this value is stored.
    uint16_t state_expected_after_adjustment = 0U;
};

bool samePendingStateAdjustmentSnapshot(const PendingPlacement& current,
                                        const PendingPlacement& observed) noexcept {
    return current.position == observed.position &&
        current.sent_at == observed.sent_at &&
        current.selected_hotbar_slot == observed.selected_hotbar_slot &&
        current.state_adjustment_pending == observed.state_adjustment_pending &&
        current.state_adjustment_count == observed.state_adjustment_count &&
        current.state_adjustment_send_failures == observed.state_adjustment_send_failures &&
        current.state_before_adjustment == observed.state_before_adjustment &&
        current.state_expected_after_adjustment == observed.state_expected_after_adjustment;
}

// A stair is armed only after its material and support have both been chosen.
// The associated PlayerAuthInputPacket is sent before the later ItemUse, so
// the server receives the desired yaw without moving the local camera.
struct PendingSilentRotation {
    PrinterPosition position;
    ProjectionPrinterSilentRotation::Ticket ticket = 0U;
    std::chrono::steady_clock::time_point armed_at{};
};

// Packet-backed moves are authorized from either the passive inventory mailbox
// or a live-native ID snapshot.  A successful ItemStackRequest is normally
// acknowledged with ItemStackResponse, though, and that response is allowed
// to update the client container without a follow-up InventorySlot packet.
// Consequently a mailbox baseline is not, by itself, a valid *confirmation*
// source: when available, confirmation must prefer a fresh live-native
// snapshot paired with the item-component semantics.  Both paths inspect the
// destination material before placing anything.
struct PendingInventoryMove {
    uint64_t inventory_session_token = 0;
    int32_t source_inventory_slot = -1;
    int32_t destination_hotbar_slot = -1;
    std::string expected_name;
    uint16_t expected_aux = 0;
    uint16_t expected_source_count = 0;
    int32_t expected_source_runtime_item_id = 0;
    int32_t expected_source_network_stack_id = 0;
    bool expected_destination_occupied = false;
    std::string expected_destination_name;
    int32_t expected_destination_runtime_item_id = 0;
    uint16_t expected_destination_aux = 0;
    uint16_t expected_destination_count = 0;
    int32_t expected_destination_network_stack_id = 0;
    uint64_t mailbox_session_generation = 0;
    uint64_t response_session_generation = 0;
    uint64_t mailbox_revision_before_send = 0;
    // The supplied backpack sorter updates the client in the same tick as its
    // native Move/Swap: source backpack slot first, then target hotbar slot.
    // This records whether that exact pre-move pair reached our local receive
    // FIFO. ItemStackResponse remains placement authorization only.
    bool client_slot_refresh_queued = false;
    uint64_t client_slot_refresh_ticket = 0;
    bool response_client_refresh_queued = false;
    uint64_t response_client_refresh_ticket = 0;
    uint64_t response_generation_before_send = 0;
    // Captured by the native ItemStackNetManagerClient immediately after
    // ScopeBegin.  This is the only identity the server echoes in an
    // ItemStackResponse; session and arrival generation are merely diagnostic.
    int32_t request_id = 0;
    InventoryVerificationSource verification_source = InventoryVerificationSource::None;
    std::chrono::steady_clock::time_point sent_at{};
};

// A timed-out request may already have moved this stack on the server. Keep
// its source identity after retiring the in-flight wait so stale client state
// cannot submit the same move again. This has no time-based expiry.
struct InventoryMoveRetryGuard {
    PendingInventoryMove submitted;
    bool remote_source_known = false;
    ProjectionPrinterInventorySlot remote_source_before_send;
};

// A one-shot, response-backed material selection. It exists only to bridge the
// interval where ItemStackResponse has changed the native player container but
// the embedded Python item component has not rebuilt its slot cache.
struct VerifiedHotbarOverride {
    int32_t hotbar_slot = -1;
    int32_t network_stack_id = 0;
    std::string name;
    uint16_t aux = 0;
    uint16_t count = 0;
    std::chrono::steady_clock::time_point expires_at{};
};

enum class ResponseMoveProof : uint8_t {
    NotObserved,
    Confirmed,
    ClientRefreshRequired,
    Mismatch,
};

void logInventoryMoveNativeState(const char* stage, const PendingInventoryMove& pending) {
    ProjectionPrinterLiveInventorySnapshot live;
    const bool ready = ReadProjectionPrinterLiveInventorySnapshot(&live, nullptr) && live.ready;
    const auto* source = ready && pending.source_inventory_slot >= 9 &&
            pending.source_inventory_slot <= 35
        ? &live.slots[static_cast<size_t>(pending.source_inventory_slot)] : nullptr;
    const auto* destination = ready && pending.destination_hotbar_slot >= 0 &&
            pending.destination_hotbar_slot <= 8
        ? &live.slots[static_cast<size_t>(pending.destination_hotbar_slot)] : nullptr;
    LogProjectionPrinterInventoryDiagnostic(
        "move_live stage=%s request=%d ready=%d source=%d occupied=%d net=%d "
        "destination=%d occupied=%d net=%d local_ticket=%llu local_state=%u",
        stage, pending.request_id, ready ? 1 : 0, pending.source_inventory_slot,
        source && source->occupied ? 1 : 0, source ? source->network_stack_id : 0,
        pending.destination_hotbar_slot, destination && destination->occupied ? 1 : 0,
        destination ? destination->network_stack_id : 0,
        static_cast<unsigned long long>(pending.client_slot_refresh_ticket),
        static_cast<unsigned>(GetProjectionPrinterInventoryClientSyncTicketState(
            pending.client_slot_refresh_ticket)));
}

bool parseBoundedInt(std::string_view text, int32_t minimum, int32_t maximum,
                     int32_t* output) {
    if (!output || text.empty() || text.size() > 16U) return false;
    char buffer[17]{};
    std::copy(text.begin(), text.end(), buffer);
    char* end = nullptr;
    const long parsed = std::strtol(buffer, &end, 10);
    if (end != buffer + text.size() || parsed < minimum || parsed > maximum) return false;
    *output = static_cast<int32_t>(parsed);
    return true;
}

int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool decodeHex(std::string_view encoded, std::string* output) {
    if (!output || (encoded.size() & 1U) != 0U || encoded.size() > 1024U) return false;
    std::string decoded;
    decoded.reserve(encoded.size() / 2U);
    for (size_t index = 0; index < encoded.size(); index += 2U) {
        const int high = hexValue(encoded[index]);
        const int low = hexValue(encoded[index + 1U]);
        if (high < 0 || low < 0) return false;
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    *output = std::move(decoded);
    return true;
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
    // The client has used both spellings around the legacy-to-modern block
    // mapper transition. These aliases are identity-preserving, unlike broad
    // substring matching which could select an incorrect building material.
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

bool isAirOrReplaceable(std::string_view raw_name) {
    const std::string name = canonicalName(raw_name);
    return name.empty() || name == "air" || name == "cave_air" || name == "void_air" ||
           name == "tallgrass" || name == "tall_grass" || name == "short_grass" ||
           name == "fern" || name == "large_fern" || name == "deadbush" ||
           name == "vine" || name == "fire" || name == "snow_layer" ||
           name == "waterlily" || name == "lily_pad";
}

// The printer is deliberately more conservative than a player placing a
// block by hand: the requested policy is to leave every existing *wrong*
// block untouched.  Foliage and fire may normally be replaceable in vanilla,
// but they are still an existing block at a projected target and therefore
// must not be overwritten by this automation.
bool isEmptyPlacementTarget(std::string_view raw_name) {
    const std::string name = canonicalName(raw_name);
    return name.empty() || name == "air" || name == "cave_air" ||
           name == "void_air";
}

bool isFluid(std::string_view raw_name) {
    const std::string name = canonicalName(raw_name);
    return name == "water" || name == "lava" || containsToken(name, "bubble_column") ||
           containsToken(name, "flowing_water") || containsToken(name, "flowing_lava");
}

bool isCommandContainerOrSign(std::string_view raw_name) {
    const std::string name = canonicalName(raw_name);
    return containsToken(name, "command_block") || containsToken(name, "chest") ||
           containsToken(name, "barrel") || containsToken(name, "hopper") ||
           containsToken(name, "dispenser") || containsToken(name, "dropper") ||
           containsToken(name, "shulker") || containsToken(name, "furnace") ||
           containsToken(name, "smoker") || containsToken(name, "blast_furnace") ||
           containsToken(name, "brewing_stand") || containsToken(name, "lectern") ||
           containsToken(name, "sign") || containsToken(name, "banner") ||
           containsToken(name, "beacon") || containsToken(name, "spawner") ||
           containsToken(name, "end_portal") || containsToken(name, "jigsaw") ||
            containsToken(name, "structure_block");
}

// A projection has no safe block-actor/NBT restore path in printer mode.  Do
// not infer that a shell is harmless merely because its name is not a chest or
// sign: many current Bedrock block actors are decorative or redstone-related.
// This list is intentionally shared by target and support checks, since
// clicking an interactive support block can open a screen instead of placing.
bool isKnownBlockEntityOrInteractive(std::string_view raw_name) {
    const std::string name = canonicalName(raw_name);
    if (isCommandContainerOrSign(name)) return true;
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
        if (name == exact) return true;
    }
    return containsToken(name, "player_head") || containsToken(name, "_head") ||
           containsToken(name, "skull") || containsToken(name, "end_portal");
}

// These blocks derive their final state from click face, player yaw/pitch or
// a neighbor graph. The printer intentionally does not rotate the player or
// mutate its camera, so sending a normal ItemUse here could create a *wrong*
// block and then permanently skip it. Keep them paused until an explicit
// state-aware placement policy exists.
bool isPlacementSensitiveTarget(std::string_view raw_name) {
    const std::string name = canonicalName(raw_name);
    if (name.rfind("legacy_block_", 0U) == 0U) return true;
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
        if (containsToken(name, token)) return true;
    }
    return name == "quartz_block" || name == "bone_block" || name == "basalt" ||
           name == "polished_basalt" || name == "log" || name == "log2" ||
           name == "wood" || name == "pumpkin" || name == "lit_pumpkin" ||
           name == "hay_block" || name == "bamboo_block" ||
           name == "stripped_bamboo_block" || name == "end_portal_frame" ||
           name == "daylight_detector" || name == "daylight_detector_inverted";
}

bool isStairTarget(std::string_view raw_name) {
    const std::string name = canonicalName(raw_name);
    return containsToken(name, "stairs") || name == "normal_stone_stairs";
}

bool isSlabTarget(std::string_view raw_name) {
    return IsProjectionSlabBlock(raw_name);
}

bool isLanternTarget(std::string_view raw_name) {
    const std::string name = canonicalName(raw_name);
    return name == "lantern" || name == "soul_lantern";
}

bool hasExplicitPlacementPolicy(const ProjectionPrinterTarget& target) {
    uint16_t ignored_aux = 0U;
    if (isStairTarget(target.name)) {
        return RotateProjectionStairAux(target.aux, target.rotation_quarters, &ignored_aux);
    }
    if (isSlabTarget(target.name)) {
        bool ignored_top = false;
        bool is_double = false;
        return TryProjectionSlabPlacement(target.name, target.aux, &ignored_top, &is_double);
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
    if (isLanternTarget(target.name)) return target.aux <= 1U;
    ignored_direction = 0U;
    return TryProjectionHorizontalDirection(target.name, target.aux, &ignored_direction) &&
        RotateProjectionHorizontalDirectionAux(target.name, target.aux,
                                                target.rotation_quarters, &ignored_aux);
}

bool stairTargetYaw(const ProjectionPrinterTarget& target, float* output_yaw) {
    if (!output_yaw || !isStairTarget(target.name)) return false;
    uint16_t effective_aux = 0U;
    if (!RotateProjectionStairAux(target.aux, target.rotation_quarters, &effective_aux)) {
        return false;
    }
    static constexpr std::array<float, 4U> kYawForFacing{{
        -90.0F, 90.0F, 0.0F, 180.0F,
    }};
    *output_yaw = kYawForFacing[effective_aux & 0x03U];
    return true;
}

bool yawDirectedTargetYaw(const ProjectionPrinterTarget& target, float* output_yaw) {
    if (!output_yaw) return false;
    uint16_t effective_aux = 0U;
    if (!RotateProjectionHorizontalDirectionAux(target.name, target.aux,
                                                 target.rotation_quarters,
                                                 &effective_aux)) {
        return false;
    }
    // Bedrock's historical horizontal direction layout used by pumpkins and
    // glazed terracotta is south=0, west=1, north=2, east=3.
    static constexpr std::array<float, 4U> kYawForDirection{{
        0.0F, 90.0F, 180.0F, -90.0F,
    }};
    *output_yaw = kYawForDirection[effective_aux & 0x03U];
    return true;
}

bool horizontalContainerTargetYaw(const ProjectionPrinterTarget& target, float* output_yaw) {
    return IsProjectionContentlessContainerShell(target.name) &&
        TryProjectionHorizontalSixWaySilentYaw(target.name, target.aux,
                                                target.rotation_quarters, output_yaw);
}

bool adjustableRedstoneTargetYaw(const ProjectionPrinterTarget& target, float* output_yaw) {
    if (!output_yaw) return false;
    uint16_t effective_aux = 0U;
    uint8_t direction = 0U;
    uint8_t delay = 0U;
    bool subtract_mode = false;
    if (TryProjectionRepeaterState(target.name, target.aux, &direction, &delay)) {
        if (!RotateProjectionRepeaterAux(target.name, target.aux,
                                         target.rotation_quarters, &effective_aux)) {
            return false;
        }
    } else if (TryProjectionComparatorState(target.name, target.aux, &direction,
                                            &subtract_mode)) {
        if (!RotateProjectionComparatorAux(target.name, target.aux,
                                           target.rotation_quarters, &effective_aux)) {
            return false;
        }
    } else {
        return false;
    }
    // Repeater/comparator direction has the same south/west/north/east
    // ordering as the other yaw-selected blocks, and their arrows point in
    // the placing player's look direction.
    static constexpr std::array<float, 4U> kYawForDirection{{
        0.0F, 90.0F, 180.0F, -90.0F,
    }};
    *output_yaw = kYawForDirection[effective_aux & UINT16_C(0x0003)];
    return true;
}

bool materialMatchesTarget(const InventorySlot& slot,
                            const ProjectionPrinterTarget& target) {
    return slot.count > 0 && ProjectionBlockMaterialMatches(
        target.name, target.aux, slot.name, slot.aux);
}

bool isUnsafeSupport(std::string_view raw_name) {
    const std::string name = canonicalName(raw_name);
    // Anvils are safe printer targets with an explicit yaw policy, but using
    // one as the clicked neighbour opens its UI on many clients. Keep that
    // distinction local to support selection rather than re-blocking anvils
    // as projection targets.
    if (name == "anvil" || name == "chipped_anvil" || name == "damaged_anvil") {
        return true;
    }
    if (isAirOrReplaceable(name) || isFluid(name) ||
        isKnownBlockEntityOrInteractive(name)) {
        return true;
    }
    static constexpr std::array<std::string_view, 29U> kUnsupported{
        "torch", "rail", "redstone_wire", "tripwire", "string", "ladder", "vine",
        "button", "lever", "pressure_plate", "carpet", "flower", "sapling", "mushroom",
        "seagrass", "kelp", "coral_fan", "candle", "chain", "lantern", "cobweb",
        "web", "bed", "door", "trapdoor", "banner", "skull", "scaffolding",
        "powder_snow",
    };
    for (const std::string_view token : kUnsupported) {
        if (containsToken(name, token)) return true;
    }
    return name == "daylight_detector" || name == "daylight_detector_inverted";
}

bool isSupportedTarget(const ProjectionPrinterTarget& target) {
    const bool explicit_policy = hasExplicitPlacementPolicy(target);
    const bool allowed_phase = target.phase == ImportPhase::Structure ||
        target.phase == ImportPhase::Gravity ||
        (explicit_policy && target.phase == ImportPhase::Attachment);
    if (target.name.empty() ||
        !allowed_phase ||
        ((target.flags & kProjectionRecordStateful) != 0U && !explicit_policy)) {
        return false;
    }
    const std::string name = canonicalName(target.name);
    const bool contentless_container = IsProjectionContentlessContainerShell(target.name);
    return !isAirOrReplaceable(name) && !isFluid(name) &&
            (!isKnownBlockEntityOrInteractive(name) || contentless_container) &&
            (explicit_policy || !isPlacementSensitiveTarget(name));
}

bool effectivePlacementAux(const ProjectionPrinterTarget& target, uint16_t* output) {
    if (!output) return false;
    if (isStairTarget(target.name)) {
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

bool targetMatchesBlock(const ProjectionPrinterTarget& target,
                        const NativeBlockView& actual,
                        bool resolve_stair_state = false,
                        bool* stair_state_unavailable = nullptr) {
    if (stair_state_unavailable) *stair_state_unavailable = false;
    if (!actual.name || !ProjectionBlockMaterialMatches(target.name, target.aux,
                                                         *actual.name, actual.aux)) {
        return false;
    }
    if (isStairTarget(target.name)) {
        if (!resolve_stair_state) return false;
        uint16_t expected_aux = 0U;
        NativeBlockSnapshot snapshot;
        uint16_t actual_aux = 0U;
        if (!RotateProjectionStairAux(target.aux, target.rotation_quarters, &expected_aux) ||
            !NativeWorldAccess::getBlockSnapshot(target.x, target.y, target.z, &snapshot,
                                                  false, 0) ||
            !TryResolveProjectionStairAux(snapshot, &actual_aux)) {
            if (stair_state_unavailable) *stair_state_unavailable = true;
            return false;
        }
        return expected_aux == actual_aux;
    }
    uint16_t expected_aux = 0U;
    return effectivePlacementAux(target, &expected_aux) &&
        ProjectionBlockIdentityMatches(target.name, expected_aux, *actual.name, actual.aux);
}

bool isDoubleSlabTarget(const ProjectionPrinterTarget& target) {
    bool ignored_top = false;
    bool is_double = false;
    return TryProjectionSlabPlacement(target.name, target.aux, &ignored_top, &is_double) &&
        is_double;
}

// A double slab is built in two accepted ItemUse actions. This predicate is
// intentionally stricter than ordinary material matching: it permits only a
// same-material *single* slab already occupying the target coordinate, never
// a different existing block that the printer might overwrite.
bool isPartialDoubleSlabTarget(const ProjectionPrinterTarget& target,
                               const NativeBlockView& actual) {
    if (!actual.name || !actual.type_token || !isDoubleSlabTarget(target) ||
        isEmptyPlacementTarget(*actual.name) || isFluid(*actual.name) ||
        !ProjectionBlockMaterialMatches(target.name, target.aux,
                                        *actual.name, actual.aux)) {
        return false;
    }
    bool actual_top = false;
    bool actual_is_double = false;
    return TryProjectionSlabPlacement(*actual.name, actual.aux,
                                      &actual_top, &actual_is_double) &&
        !actual_is_double;
}

bool targetWithinReach(const ProjectionPrinterTarget& target,
                       int32_t player_x, int32_t player_y, int32_t player_z,
                       double* squared_distance) {
    const double dx = (static_cast<double>(target.x) + 0.5) -
        (static_cast<double>(player_x) + 0.5);
    const double dy = (static_cast<double>(target.y) + 0.5) -
        (static_cast<double>(player_y) + 1.55);
    const double dz = (static_cast<double>(target.z) + 0.5) -
        (static_cast<double>(player_z) + 0.5);
    const double squared = dx * dx + dy * dy + dz * dz;
    // Keep one half block below the normal five-block interaction distance:
    // the engine's live Vec3 remains authoritative when it sends ItemUse.
    if (squared > 20.25) return false;
    if (squared_distance) *squared_distance = squared;
    return true;
}

bool findPlacementSupport(NativeWorldReader* reader,
                           const ProjectionPrinterTarget& target,
                           PlacementSupport* output) {
    if (!reader || !output) return false;
    struct FaceProbe {
        int32_t dx;
        int32_t dy;
        int32_t dz;
        int face;
    };
    // A face belongs to the support block, not the empty target coordinate.
    static constexpr std::array<FaceProbe, 6U> kFaces{{
        {0, -1, 0, 1}, {0, 1, 0, 0}, {0, 0, -1, 3},
        {0, 0, 1, 2}, {-1, 0, 0, 5}, {1, 0, 0, 4},
    }};
    // A gravity block that has only a side/upper neighbor would immediately
    // fall away from the projection coordinate. It must be placed on the top
    // face of a real block underneath; normal structural blocks can use any
    // safe adjacent face.
    const size_t face_count = target.phase == ImportPhase::Gravity ? 1U : kFaces.size();
    for (size_t face_index = 0; face_index < face_count; ++face_index) {
        const FaceProbe& probe = kFaces[face_index];
        NativeBlockView support;
        if (!reader->getBlockView(target.x + probe.dx, target.y + probe.dy,
                                  target.z + probe.dz, &support) ||
            !support.name || !support.type_token || isUnsafeSupport(*support.name)) {
            continue;
        }
        output->x = target.x + probe.dx;
        output->y = target.y + probe.dy;
        output->z = target.z + probe.dz;
        output->face = probe.face;
        output->native_block = support.type_token;
        return true;
    }
    return false;
}

ItemUseClickPosition stairClickPositionForFace(int face, bool upper_half) {
    const float side_y = upper_half ? 0.75F : 0.25F;
    switch (face) {
        case 0: return {0.5F, 0.0F, 0.5F};
        case 1: return {0.5F, 1.0F, 0.5F};
        case 2: return {0.5F, side_y, 0.0F};
        case 3: return {0.5F, side_y, 1.0F};
        case 4: return {0.0F, side_y, 0.5F};
        case 5: return {1.0F, side_y, 0.5F};
        default: return {0.5F, side_y, 0.5F};
    }
}

bool findStairPlacementSupport(NativeWorldReader* reader,
                               const ProjectionPrinterTarget& target,
                               PlacementSupport* output) {
    if (!reader || !output) return false;
    uint16_t effective_aux = 0U;
    if (!RotateProjectionStairAux(target.aux, target.rotation_quarters, &effective_aux)) {
        return false;
    }
    const bool upper_half = (effective_aux & 0x04U) != 0U;
    struct FaceProbe {
        int32_t dx;
        int32_t dy;
        int32_t dz;
        int face;
    };
    // A top stair must be placed against an overhead block's DOWN face (or a
    // high side click); a bottom stair must be placed against the floor's UP
    // face (or a low side click).  Using the generic face-centre sequence here
    // would force every top stair into the lower half.
    static constexpr std::array<FaceProbe, 4U> kSideFaces{{
        {0, 0, -1, 3}, {0, 0, 1, 2}, {-1, 0, 0, 5}, {1, 0, 0, 4},
    }};
    const FaceProbe preferred = upper_half
        ? FaceProbe{0, 1, 0, 0}
        : FaceProbe{0, -1, 0, 1};
    const auto try_probe = [&](const FaceProbe& probe) {
        NativeBlockView support;
        if (!reader->getBlockView(target.x + probe.dx, target.y + probe.dy,
                                  target.z + probe.dz, &support) ||
            !support.name || !support.type_token || isUnsafeSupport(*support.name)) {
            return false;
        }
        output->x = target.x + probe.dx;
        output->y = target.y + probe.dy;
        output->z = target.z + probe.dz;
        output->face = probe.face;
        output->native_block = support.type_token;
        output->has_click_override = true;
        output->click_override = stairClickPositionForFace(probe.face, upper_half);
        return true;
    };
    if (try_probe(preferred)) return true;
    for (const FaceProbe& probe : kSideFaces) {
        if (try_probe(probe)) return true;
    }
    return false;
}

struct DirectedSupportProbe {
    int32_t dx = 0;
    int32_t dy = 0;
    int32_t dz = 0;
    int face = 1;
};

bool selectDirectedSupport(NativeWorldReader* reader,
                           const ProjectionPrinterTarget& target,
                           const DirectedSupportProbe& probe,
                           bool has_click_override,
                           const ItemUseClickPosition& click_override,
                           PlacementSupport* output) {
    if (!reader || !output) return false;
    NativeBlockView support;
    if (!reader->getBlockView(target.x + probe.dx, target.y + probe.dy,
                              target.z + probe.dz, &support) ||
        !support.name || !support.type_token || isUnsafeSupport(*support.name)) {
        return false;
    }
    output->x = target.x + probe.dx;
    output->y = target.y + probe.dy;
    output->z = target.z + probe.dz;
    output->face = probe.face;
    output->native_block = support.type_token;
    output->has_click_override = has_click_override;
    output->click_override = click_override;
    return true;
}

bool findSlabPlacementSupport(NativeWorldReader* reader,
                              const ProjectionPrinterTarget& target,
                              PlacementSupport* output) {
    bool top = false;
    bool is_double = false;
    if (!TryProjectionSlabPlacement(target.name, target.aux, &top, &is_double)) {
        return false;
    }

    if (is_double) {
        NativeBlockView existing;
        if (!reader->getBlockView(target.x, target.y, target.z, &existing) || !existing.name) {
            return false;
        }
        if (!isEmptyPlacementTarget(*existing.name) && !isFluid(*existing.name)) {
            // The first half was confirmed. Click the open side of that exact
            // slab rather than a neighbour, so the target game's normal slab
            // merge rule creates a double slab. The second packet is only ever
            // reached after a fresh native world read; no speculative double
            // ItemUse packets are queued together.
            if (!isPartialDoubleSlabTarget(target, existing)) return false;
            bool existing_top = false;
            bool ignored_existing_double = false;
            if (!TryProjectionSlabPlacement(*existing.name, existing.aux,
                                            &existing_top, &ignored_existing_double)) {
                return false;
            }
            const int merge_face = existing_top ? 0 : 1;
            output->x = target.x;
            output->y = target.y;
            output->z = target.z;
            output->face = merge_face;
            output->native_block = existing.type_token;
            output->has_click_override = true;
            output->click_override = stairClickPositionForFace(merge_face, existing_top);
            return true;
        }
        // An empty double-slab target always starts with a bottom half. Once
        // that half is visible, the branch above performs the merge on a later
        // game tick.
        top = false;
    }
    static constexpr std::array<DirectedSupportProbe, 4U> kSideFaces{{
        {0, 0, -1, 3}, {0, 0, 1, 2}, {-1, 0, 0, 5}, {1, 0, 0, 4},
    }};
    const DirectedSupportProbe preferred = top
        ? DirectedSupportProbe{0, 1, 0, 0}
        : DirectedSupportProbe{0, -1, 0, 1};
    const auto try_probe = [&](const DirectedSupportProbe& probe) {
        return selectDirectedSupport(reader, target, probe, true,
                                     stairClickPositionForFace(probe.face, top), output);
    };
    if (try_probe(preferred)) return true;
    for (const DirectedSupportProbe& probe : kSideFaces) {
        if (try_probe(probe)) return true;
    }
    return false;
}

bool findAxisPlacementSupport(NativeWorldReader* reader,
                              const ProjectionPrinterTarget& target,
                              PlacementSupport* output) {
    ProjectionBlockAxis source_axis;
    ProjectionBlockAxis effective_axis;
    if (!TryProjectionBlockAxis(target.name, target.aux, &source_axis) ||
        !RotateProjectionBlockAxis(source_axis, target.rotation_quarters, &effective_axis)) {
        return false;
    }
    static constexpr std::array<DirectedSupportProbe, 2U> kVertical{{
        {0, -1, 0, 1}, {0, 1, 0, 0},
    }};
    static constexpr std::array<DirectedSupportProbe, 2U> kXAxis{{
        {-1, 0, 0, 5}, {1, 0, 0, 4},
    }};
    static constexpr std::array<DirectedSupportProbe, 2U> kZAxis{{
        {0, 0, -1, 3}, {0, 0, 1, 2},
    }};
    const auto try_probes = [&](const auto& probes) {
        for (const DirectedSupportProbe& probe : probes) {
            if (selectDirectedSupport(reader, target, probe, false, {}, output)) return true;
        }
        return false;
    };
    switch (effective_axis) {
        case ProjectionBlockAxis::Y: return try_probes(kVertical);
        case ProjectionBlockAxis::X: return try_probes(kXAxis);
        case ProjectionBlockAxis::Z: return try_probes(kZAxis);
    }
    return false;
}

bool findPlacementSupportForFace(NativeWorldReader* reader,
                                 const ProjectionPrinterTarget& target,
                                 ProjectionBlockFace effective_face,
                                 PlacementSupport* output) {
    DirectedSupportProbe probe;
    switch (effective_face) {
        case ProjectionBlockFace::Down: probe = {0, 1, 0, 0}; break;
        case ProjectionBlockFace::Up: probe = {0, -1, 0, 1}; break;
        case ProjectionBlockFace::North: probe = {0, 0, 1, 2}; break;
        case ProjectionBlockFace::South: probe = {0, 0, -1, 3}; break;
        case ProjectionBlockFace::West: probe = {1, 0, 0, 4}; break;
        case ProjectionBlockFace::East: probe = {-1, 0, 0, 5}; break;
    }
    return selectDirectedSupport(reader, target, probe, false, {}, output);
}

bool findFacePlacementSupport(NativeWorldReader* reader,
                               const ProjectionPrinterTarget& target,
                               PlacementSupport* output) {
    ProjectionBlockFace source_face;
    ProjectionBlockFace effective_face;
    if (!TryProjectionBlockFace(target.name, target.aux, &source_face) ||
        !RotateProjectionBlockFace(source_face, target.rotation_quarters, &effective_face)) {
        return false;
    }
    return findPlacementSupportForFace(reader, target, effective_face, output);
}

bool oppositeBlockFace(ProjectionBlockFace source, ProjectionBlockFace* output) {
    if (!output) return false;
    switch (source) {
        case ProjectionBlockFace::Down: *output = ProjectionBlockFace::Up; return true;
        case ProjectionBlockFace::Up: *output = ProjectionBlockFace::Down; return true;
        case ProjectionBlockFace::North: *output = ProjectionBlockFace::South; return true;
        case ProjectionBlockFace::South: *output = ProjectionBlockFace::North; return true;
        case ProjectionBlockFace::West: *output = ProjectionBlockFace::East; return true;
        case ProjectionBlockFace::East: *output = ProjectionBlockFace::West; return true;
    }
    return false;
}

bool findSixWayContainerPlacementSupport(NativeWorldReader* reader,
                                         const ProjectionPrinterTarget& target,
                                         PlacementSupport* output) {
    ProjectionBlockFace source_face;
    ProjectionBlockFace effective_face;
    if (!TryProjectionSixWayFacing(target.name, target.aux, &source_face) ||
        !RotateProjectionBlockFace(source_face, target.rotation_quarters, &effective_face)) {
        return false;
    }
    // A hopper's outlet is the inverse of its clicked face. The rest of the
    // validated container shells keep the normal ItemUse face direction.
    if (canonicalName(target.name) == "hopper") {
        ProjectionBlockFace click_face;
        return oppositeBlockFace(effective_face, &click_face) &&
            findPlacementSupportForFace(reader, target, click_face, output);
    }
    return findPlacementSupportForFace(reader, target, effective_face, output);
}

bool findHorizontalContainerPlacementSupport(NativeWorldReader* reader,
                                             const ProjectionPrinterTarget& target,
                                             PlacementSupport* output) {
    if (!findPlacementSupport(reader, target, output) ||
        !horizontalContainerTargetYaw(target, &output->silent_yaw)) {
        return false;
    }
    output->requires_silent_yaw = true;
    return true;
}

bool findAdjustableRedstonePlacementSupport(NativeWorldReader* reader,
                                            const ProjectionPrinterTarget& target,
                                            PlacementSupport* output) {
    static constexpr DirectedSupportProbe kFloor{0, -1, 0, 1};
    if (!selectDirectedSupport(reader, target, kFloor, false, {}, output) ||
        !adjustableRedstoneTargetYaw(target, &output->silent_yaw)) {
        return false;
    }
    output->requires_silent_yaw = true;
    return true;
}

bool findDaylightDetectorPlacementSupport(NativeWorldReader* reader,
                                          const ProjectionPrinterTarget& target,
                                          PlacementSupport* output) {
    static constexpr DirectedSupportProbe kFloor{0, -1, 0, 1};
    return IsProjectionDaylightDetector(target.name) && target.aux <= UINT16_C(0x000F) &&
        selectDirectedSupport(reader, target, kFloor, false, {}, output);
}

bool findAttachmentPlacementSupport(NativeWorldReader* reader,
                                    const ProjectionPrinterTarget& target,
                                   PlacementSupport* output) {
    ProjectionBlockFace source_face;
    ProjectionBlockFace effective_face;
    if (!TryProjectionAttachmentFace(target.name, target.aux, &source_face) ||
        !RotateProjectionBlockFace(source_face, target.rotation_quarters, &effective_face)) {
        return false;
    }
    return findPlacementSupportForFace(reader, target, effective_face, output);
}

bool findLeverPlacementSupport(NativeWorldReader* reader,
                               const ProjectionPrinterTarget& target,
                               PlacementSupport* output) {
    uint16_t effective_aux = 0U;
    ProjectionBlockFace effective_face;
    if (!RotateProjectionLeverAux(target.name, target.aux, target.rotation_quarters,
                                  &effective_aux) ||
        !TryProjectionLeverAttachmentFace(target.name, effective_aux, &effective_face) ||
        !findPlacementSupportForFace(reader, target, effective_face, output)) {
        return false;
    }
    float yaw = 0.0F;
    if (TryProjectionLeverSilentYaw(target.name, target.aux, target.rotation_quarters, &yaw)) {
        output->requires_silent_yaw = true;
        output->silent_yaw = yaw;
    }
    return true;
}

bool findLanternPlacementSupport(NativeWorldReader* reader,
                                 const ProjectionPrinterTarget& target,
                                 PlacementSupport* output) {
    if (!isLanternTarget(target.name) || target.aux > 1U) return false;
    const DirectedSupportProbe probe = target.aux == 1U
        ? DirectedSupportProbe{0, 1, 0, 0}
        : DirectedSupportProbe{0, -1, 0, 1};
    return selectDirectedSupport(reader, target, probe, false, {}, output);
}

bool findTargetPlacementSupport(NativeWorldReader* reader,
                                const ProjectionPrinterTarget& target,
                                PlacementSupport* output) {
    if (!reader || !output) return false;
    *output = PlacementSupport{};
    if (isStairTarget(target.name)) {
        if (!findStairPlacementSupport(reader, target, output) ||
            !stairTargetYaw(target, &output->silent_yaw)) {
            return false;
        }
        output->requires_silent_yaw = true;
        return true;
    }
    if (isSlabTarget(target.name)) return findSlabPlacementSupport(reader, target, output);
    if (IsProjectionDaylightDetector(target.name)) {
        return findDaylightDetectorPlacementSupport(reader, target, output);
    }
    if (IsProjectionContentlessContainerShell(target.name)) {
        ProjectionBlockFace face;
        if (TryProjectionSixWayFacing(target.name, target.aux, &face)) {
            return findSixWayContainerPlacementSupport(reader, target, output);
        }
        if (TryProjectionHorizontalSixWayFacing(target.name, target.aux, &face)) {
            return findHorizontalContainerPlacementSupport(reader, target, output);
        }
        return false;
    }
    uint8_t direction = 0U;
    uint8_t delay = 0U;
    bool subtract_mode = false;
    if (TryProjectionRepeaterState(target.name, target.aux, &direction, &delay) ||
        TryProjectionComparatorState(target.name, target.aux, &direction, &subtract_mode)) {
        return findAdjustableRedstonePlacementSupport(reader, target, output);
    }
    ProjectionBlockAxis ignored_axis;
    if (TryProjectionBlockAxis(target.name, target.aux, &ignored_axis)) {
        return findAxisPlacementSupport(reader, target, output);
    }
    ProjectionBlockFace ignored_face;
    if (TryProjectionBlockFace(target.name, target.aux, &ignored_face)) {
        return findFacePlacementSupport(reader, target, output);
    }
    if (TryProjectionAttachmentFace(target.name, target.aux, &ignored_face)) {
        return findAttachmentPlacementSupport(reader, target, output);
    }
    if (TryProjectionLeverAttachmentFace(target.name, target.aux, &ignored_face)) {
        return findLeverPlacementSupport(reader, target, output);
    }
    if (isLanternTarget(target.name)) return findLanternPlacementSupport(reader, target, output);
    uint8_t ignored_direction = 0U;
    if (TryProjectionHorizontalDirection(target.name, target.aux, &ignored_direction)) {
        if (!findPlacementSupport(reader, target, output) ||
            !yawDirectedTargetYaw(target, &output->silent_yaw)) {
            return false;
        }
        output->requires_silent_yaw = true;
        return true;
    }
    return findPlacementSupport(reader, target, output);
}

void publishReachabilityPreviewForQuery(const ProjectionPrinterQueryResult& query_result,
                                        int32_t player_x, int32_t player_y,
                                        int32_t player_z) {
    BuildProjectionRenderer& renderer = BuildProjectionRenderer::instance();
    if (query_result.plan_identity == 0U ||
        query_result.status == ProjectionPrinterQueryStatus::NoProjection ||
        query_result.status == ProjectionPrinterQueryStatus::SourceUnavailable ||
        query_result.status == ProjectionPrinterQueryStatus::InvalidQuery) {
        renderer.clearReachabilityPreview();
        return;
    }
    NativeWorldReader reader;
    if (!reader.open()) {
        // A world-reader gap must fail open instead of retaining a stale
        // green target after a dimension/chunk transition.
        renderer.clearReachabilityPreview();
        return;
    }
    std::vector<ProjectionReachabilityPreviewPosition> positions;
    positions.reserve(query_result.targets.size());
    for (const ProjectionPrinterTarget& target : query_result.targets) {
        if (!isSupportedTarget(target)) continue;
        if (!targetWithinReach(target, player_x, player_y, player_z, nullptr)) continue;
        NativeBlockView actual;
        if (!reader.getBlockView(target.x, target.y, target.z, &actual) || !actual.name) {
            continue;
        }
        bool stair_state_unavailable = false;
        const bool partial_double_slab = isPartialDoubleSlabTarget(target, actual);
        if (targetMatchesBlock(target, actual, isStairTarget(target.name),
                               &stair_state_unavailable) || stair_state_unavailable ||
            (!isEmptyPlacementTarget(*actual.name) && !partial_double_slab) ||
            isFluid(*actual.name)) {
            continue;
        }
        PlacementSupport support;
        if (!findTargetPlacementSupport(&reader, target, &support)) continue;
        positions.push_back({target.x, target.y, target.z});
    }
    renderer.publishReachabilityPreview(query_result.plan_identity, std::move(positions));
}

bool parseInventorySnapshot(std::string_view payload, InventorySnapshot* output) {
    if (!output || payload.size() > 32U * 1024U) return false;
    *output = InventorySnapshot{};
    size_t line_start = 0;
    bool selected_seen = false;
    while (line_start <= payload.size()) {
        const size_t line_end = payload.find('\n', line_start);
        const std::string_view line = payload.substr(
            line_start, line_end == std::string_view::npos ? std::string_view::npos :
                line_end - line_start);
        line_start = line_end == std::string_view::npos ? payload.size() + 1U : line_end + 1U;
        if (line.empty()) continue;
        if (line.compare(0, 9U, "selected=") == 0) {
            int32_t selected = -1;
            if (!parseBoundedInt(line.substr(9U), -1, 8, &selected)) return false;
            output->selected_hotbar_slot = selected;
            selected_seen = true;
            continue;
        }
        std::array<std::string_view, 4U> fields{};
        size_t field_start = 0;
        for (size_t index = 0; index < fields.size(); ++index) {
            const size_t separator = line.find('\t', field_start);
            if (separator == std::string_view::npos) {
                if (index + 1U != fields.size()) return false;
                fields[index] = line.substr(field_start);
                field_start = line.size();
                break;
            }
            fields[index] = line.substr(field_start, separator - field_start);
            field_start = separator + 1U;
        }
        if (field_start != line.size()) return false;
        InventorySlot slot;
        int32_t aux = 0;
        if (!parseBoundedInt(fields[0], 0, 35, &slot.slot) ||
            !parseBoundedInt(fields[1], 0, 65535, &aux) ||
            !parseBoundedInt(fields[2], 1, 65535, &slot.count) ||
            !decodeHex(fields[3], &slot.name) || slot.name.empty()) {
            return false;
        }
        slot.aux = static_cast<uint16_t>(aux);
        output->slots.push_back(std::move(slot));
    }
    output->available = selected_seen;
    return output->available;
}

void applyLiveInventoryNetworkSnapshot(
    const ProjectionPrinterLiveInventorySnapshot& live,
    InventorySnapshot* output) {
    if (!output || !live.ready) return;
    output->network_ready = true;
    output->network_source = InventoryVerificationSource::LiveNative;
    for (size_t index = 0; index < output->network_slots.size() &&
                           index < live.slots.size(); ++index) {
        const ProjectionPrinterLiveInventorySlot& source = live.slots[index];
        NetworkInventorySlot& destination = output->network_slots[index];
        destination.occupied = source.occupied;
        destination.has_network_stack_id = source.has_network_stack_id;
        destination.network_stack_id = source.network_stack_id;
    }
    for (InventorySlot& slot : output->slots) {
        if (slot.slot < 0 || static_cast<size_t>(slot.slot) >= live.slots.size()) {
            continue;
        }
        const ProjectionPrinterLiveInventorySlot& network =
            live.slots[static_cast<size_t>(slot.slot)];
        if (!network.occupied || !network.has_network_stack_id) continue;
        slot.network_verified = true;
        slot.network_stack_id = network.network_stack_id;
    }
}

bool readInventorySnapshot(InventorySnapshot* output,
                           bool prefer_live_network = false) {
    if (!output) return false;
    // Capture native identities before the Python component scan. If a user or
    // network update changes a slot during that scan, the mover's second read
    // below will see a different ID and refuse the action rather than moving a
    // newly inserted item under stale semantic metadata.
    ProjectionPrinterLiveInventorySnapshot live;
    std::string ignored_live_error;
    const bool live_ready = ReadProjectionPrinterLiveInventorySnapshot(
        &live, &ignored_live_error) && live.ready;
    static const std::string kStatements =
        "import binascii\n"
        "import mod.client.extraClientApi as _infinitecz_printer_api\n"
        "def _infinitecz_printer_int(_value, _default=0):\n"
        "  try: return int(_value)\n"
        "  except BaseException: return _default\n"
        "def _infinitecz_printer_first(_item, _keys, _default=None):\n"
        "  for _key in _keys:\n"
        "    try:\n"
        "      _value = _item.get(_key)\n"
        "      if _value is not None: return _value\n"
        "    except BaseException: pass\n"
        "  return _default\n"
        "def _infinitecz_printer_snapshot():\n"
        "  try:\n"
        "    _player = _infinitecz_printer_api.GetLocalPlayerId()\n"
        "    if _player is None or str(_player) in ('', '-1'): return ''\n"
        "    _factory = _infinitecz_printer_api.GetEngineCompFactory()\n"
        "    _items = _factory.CreateItem(_player)\n"
        "    _position = _infinitecz_printer_api.GetMinecraftEnum().ItemPosType.INVENTORY\n"
        "    _selected = _infinitecz_printer_int(_items.GetSlotId(), -1)\n"
        "    if _selected < 0 or _selected > 8: return ''\n"
        "    _rows = ['selected=%d' % _selected]\n"
        "    for _slot in range(36):\n"
        "      try: _item = _items.GetPlayerItem(_position, _slot)\n"
        "      except BaseException: continue\n"
        "      if not isinstance(_item, dict): continue\n"
        "      _name = _infinitecz_printer_first(_item, ('newItemName','itemName','name'))\n"
        "      if _name is None: continue\n"
        "      try:\n"
        "        if isinstance(_name, unicode): _encoded = _name.encode('utf-8')\n"
        "        else: _encoded = str(_name).encode('utf-8')\n"
        "      except NameError:\n"
        "        _encoded = str(_name).encode('utf-8')\n"
        "      _count = _infinitecz_printer_int(_infinitecz_printer_first(_item, ('count','itemCount','amount'), 1), 1)\n"
        "      if _count <= 0: continue\n"
        "      _aux = _infinitecz_printer_int(_infinitecz_printer_first(_item, ('auxValue','aux','newAuxValue','itemAux','damage'), 0), 0)\n"
        "      if _aux < 0 or _aux > 65535: continue\n"
        "      _rows.append('%d\\t%d\\t%d\\t%s' % (_slot, _aux, _count, binascii.hexlify(_encoded).decode('ascii')))\n"
        "    return '\\n'.join(_rows)\n"
        "  except BaseException:\n"
        "    return ''\n"
        "_infinitecz_printer_inventory = _infinitecz_printer_snapshot()\n";
    std::string response;
    if (!PythonUtils::PyEvalUtf8(kStatements, "_infinitecz_printer_inventory", &response)) {
        return false;
    }
    if (!parseInventorySnapshot(response, output)) return false;

    const bool mailbox_ready = GetProjectionPrinterInventorySnapshot(
        &output->remote_inventory) && output->remote_inventory.ready &&
        output->remote_inventory.session_generation != 0U;
    if (mailbox_ready) {
        output->mailbox_ready = true;
        output->mailbox_session_generation = output->remote_inventory.session_generation;
        output->mailbox_revision = output->remote_inventory.revision;
    }
    if (prefer_live_network && live_ready) {
        applyLiveInventoryNetworkSnapshot(live, output);
        return true;
    }

    // Prefer a complete passive baseline when it exists: unlike the native
    // identity view it also independently carries count/aux/runtime metadata.
    // It is often unavailable when the tool is injected after the game's login
    // inventory packet, in which case the safe live fallback below is used.
    if (!mailbox_ready) {
        if (live_ready) applyLiveInventoryNetworkSnapshot(live, output);
        return true;
    }
    const ProjectionPrinterInventorySnapshot& mailbox = output->remote_inventory;
    output->network_ready = true;
    output->network_source = InventoryVerificationSource::Mailbox;
    for (size_t index = 0; index < mailbox.slots.size(); ++index) {
        const ProjectionPrinterInventorySlot& source = mailbox.slots[index];
        NetworkInventorySlot& destination = output->network_slots[index];
        destination.occupied = source.occupied;
        destination.runtime_item_id = source.runtime_item_id;
        destination.has_network_stack_id = source.has_network_stack_id;
        destination.network_stack_id = source.network_stack_id;
        destination.count = source.count;
        destination.aux = source.aux;
    }

    // Pair an API-visible stack with a packet-visible stack only when every
    // independently available property agrees.  The packet's resolved name
    // may legitimately be empty before ItemRuntimeRegistry finishes, so it is
    // used as an additional check when present rather than a hard requirement.
    for (InventorySlot& slot : output->slots) {
        if (slot.slot < 0 ||
            static_cast<size_t>(slot.slot) >= mailbox.slots.size()) {
            continue;
        }
        const ProjectionPrinterInventorySlot& network =
            mailbox.slots[static_cast<size_t>(slot.slot)];
        if (!network.occupied || !network.has_network_stack_id ||
            network.count != static_cast<uint16_t>(slot.count) ||
            network.aux != slot.aux ||
            (!network.name.empty() &&
             canonicalName(network.name) != canonicalName(slot.name))) {
            continue;
        }
        slot.network_verified = true;
        slot.runtime_item_id = network.runtime_item_id;
        slot.network_stack_id = network.network_stack_id;
    }
    return true;
}

const InventorySlot* findInventorySlot(const InventorySnapshot& inventory,
                                       int32_t slot_number) {
    for (const InventorySlot& slot : inventory.slots) {
        if (slot.slot == slot_number) return &slot;
    }
    return nullptr;
}

bool materialSlotMatches(const InventorySlot* slot, std::string_view expected_name,
                         uint16_t expected_aux) {
    return slot && slot->count > 0 && slot->aux == expected_aux &&
        canonicalName(slot->name) == canonicalName(expected_name);
}

// Pick a predictable, least-disruptive hotbar destination.  Prefer an empty
// non-selected slot, then an occupied non-selected slot.  The printer never
// rewrites the currently held slot while preparing material: the player may be
// using it manually.  An occupied destination must have a current native
// stack ID so the native request can prove it is swapping exactly what the
// verified snapshot observed. Never overwrite an occupied hotbar slot whose
// semantic item could not also be read through the client item component.
bool chooseBackpackMoveDestination(const InventorySnapshot& inventory,
                                   int32_t* output_slot) {
    if (output_slot) *output_slot = -1;
    if (!inventory.network_ready) return false;
    int32_t best_slot = -1;
    int best_score = std::numeric_limits<int>::max();
    for (int32_t slot = 0; slot <= 8; ++slot) {
        if (slot == inventory.selected_hotbar_slot) continue;
        const NetworkInventorySlot& candidate =
            inventory.network_slots[static_cast<size_t>(slot)];
        if (candidate.occupied && !candidate.has_network_stack_id) continue;
        if (candidate.occupied && !findInventorySlot(inventory, slot)) continue;
        const int score = (candidate.occupied ? 10 : 0) + slot;
        if (score < best_score) {
            best_score = score;
            best_slot = slot;
        }
    }
    if (best_slot < 0) return false;
    if (output_slot) *output_slot = best_slot;
    return true;
}

projection_inventory_evidence::SubmittedMove inventoryMoveEvidence(
    const PendingInventoryMove& pending) {
    projection_inventory_evidence::SubmittedMove evidence;
    evidence.source_slot = pending.source_inventory_slot;
    evidence.destination_slot = pending.destination_hotbar_slot;
    evidence.session_generation = pending.response_session_generation;
    evidence.remote_revision_before_send = pending.mailbox_revision_before_send;
    evidence.source_before.occupied = true;
    evidence.source_before.has_network_stack_id = true;
    evidence.source_before.network_stack_id = pending.expected_source_network_stack_id;
    evidence.source_before.runtime_item_id = pending.expected_source_runtime_item_id;
    evidence.source_before.name = pending.expected_name;
    evidence.source_before.aux = pending.expected_aux;
    evidence.source_before.count = pending.expected_source_count;
    evidence.destination_before.occupied = pending.expected_destination_occupied;
    evidence.destination_before.has_network_stack_id = pending.expected_destination_occupied;
    evidence.destination_before.network_stack_id = pending.expected_destination_network_stack_id;
    evidence.destination_before.runtime_item_id = pending.expected_destination_runtime_item_id;
    evidence.destination_before.name = pending.expected_destination_name;
    evidence.destination_before.aux = pending.expected_destination_aux;
    evidence.destination_before.count = pending.expected_destination_count;
    return evidence;
}

bool inventoryMoveRetrySourceChanged(const InventoryMoveRetryGuard& guard,
                                     const InventorySnapshot& inventory) {
    const PendingInventoryMove& submitted = guard.submitted;
    const size_t source_slot = static_cast<size_t>(submitted.source_inventory_slot);
    if (source_slot >= inventory.network_slots.size()) return false;
    auto evidence = inventoryMoveEvidence(submitted);
    evidence.remote_source_known = guard.remote_source_known;
    evidence.remote_source_before_send = guard.remote_source_before_send;
    projection_inventory_evidence::LiveSource live;
    if (inventory.network_ready &&
        inventory.network_source == InventoryVerificationSource::LiveNative) {
        const NetworkInventorySlot& source = inventory.network_slots[source_slot];
        live.ready = true;
        live.slot.occupied = source.occupied;
        live.slot.has_network_stack_id = source.has_network_stack_id;
        live.slot.network_stack_id = source.network_stack_id;
        const InventorySlot* const material = findInventorySlot(
            inventory, submitted.source_inventory_slot);
        if (material && material->network_verified &&
            material->network_stack_id == submitted.expected_source_network_stack_id) {
            live.count_known = true;
            live.slot.count = static_cast<uint16_t>(material->count);
        }
    }
    return projection_inventory_evidence::sourceChangedSinceSubmission(
        evidence, live, inventory.remote_inventory);
}

bool inventoryMoveConfirmedFromMailbox(const PendingInventoryMove& pending,
                                        const InventorySnapshot& inventory) {
    if (!inventory.mailbox_ready) return false;
    return projection_inventory_evidence::remoteInventoryConfirmsMove(
        inventoryMoveEvidence(pending), inventory.remote_inventory,
        [](const std::string& actual, const std::string& expected) {
            return canonicalName(actual) == canonicalName(expected);
        });
}

bool inventoryMoveConfirmedFromLiveSnapshot(const PendingInventoryMove& pending,
                                            const InventorySnapshot& inventory) {
    if (!inventory.network_ready ||
        inventory.network_source != InventoryVerificationSource::LiveNative ||
        pending.destination_hotbar_slot < 0 || pending.destination_hotbar_slot > 8 ||
        pending.source_inventory_slot < 9 || pending.source_inventory_slot > 35) {
        return false;
    }
    const InventorySlot* const destination = findInventorySlot(
        inventory, pending.destination_hotbar_slot);
    if (!materialSlotMatches(destination, pending.expected_name, pending.expected_aux) ||
        !destination->network_verified ||
        destination->count != static_cast<int32_t>(pending.expected_source_count)) {
        return false;
    }

    const InventorySlot* const source = findInventorySlot(
        inventory, pending.source_inventory_slot);
    if (!pending.expected_destination_occupied) {
        return source == nullptr;
    }
    return source && source->network_verified &&
        source->count == static_cast<int32_t>(pending.expected_destination_count) &&
        source->aux == pending.expected_destination_aux &&
        canonicalName(source->name) == canonicalName(pending.expected_destination_name);
}

bool inventoryMoveConfirmed(const PendingInventoryMove& pending,
                            const InventorySnapshot& inventory) {
    // The sorter-compatible local InventorySlot pair updates this same live
    // container optimistically. Its contents can no longer prove a server
    // commit. Only the remote-only mailbox (or the exact response path above)
    // is independent evidence after a local refresh has been queued.
    if (pending.client_slot_refresh_queued) {
        return inventoryMoveConfirmedFromMailbox(pending, inventory);
    }
    // A native snapshot observes the actual client container after the
    // ItemStackResponse has been applied.  Do not make a request authorized
    // by an older InventoryContent packet wait forever for a separate
    // InventorySlot echo that some servers never send for ItemStackRequest.
    if (inventory.network_ready &&
        inventory.network_source == InventoryVerificationSource::LiveNative) {
        return inventoryMoveConfirmedFromLiveSnapshot(pending, inventory);
    }
    switch (pending.verification_source) {
        case InventoryVerificationSource::Mailbox:
            return inventoryMoveConfirmedFromMailbox(pending, inventory);
        case InventoryVerificationSource::LiveNative:
            return inventoryMoveConfirmedFromLiveSnapshot(pending, inventory);
        case InventoryVerificationSource::None:
            return false;
    }
    return false;
}

const ProjectionPrinterInventoryResponseSlot* findUniqueResponseSlot(
    const ProjectionPrinterInventoryResponse& response, uint8_t container_id,
    int32_t slot) {
    const ProjectionPrinterInventoryResponseSlot* match = nullptr;
    for (const ProjectionPrinterInventoryResponseSlot& candidate : response.slots) {
        if (candidate.container_id != container_id || candidate.has_dynamic_container_id ||
            static_cast<int32_t>(candidate.slot) != slot) {
            continue;
        }
        // A response plan must name each physical slot exactly once. Duplicate
        // entries are ambiguous without the native request ID, so fail closed.
        if (match) return nullptr;
        match = &candidate;
    }
    return match;
}

ResponseMoveProof confirmInventoryMoveFromResponseAndLive(
    const PendingInventoryMove& pending,
    const ProjectionPrinterInventoryResponse& response,
    int32_t* destination_network_stack_id) {
    if (destination_network_stack_id) *destination_network_stack_id = 0;
    if (pending.response_session_generation == 0U || !response.valid || response.rejected ||
        response.request_id != pending.request_id ||
        response.session_generation != pending.response_session_generation ||
        response.response_generation <= pending.response_generation_before_send) {
        return ResponseMoveProof::NotObserved;
    }

    // The native sender captured the exact request ID, and the mailbox returns
    // one fully decoded entry for that ID. A valid v859 entry may legitimately
    // carry extra changed slots or be bundled beside other requests, so match
    // only our two unique physical slots and verify the live container below.
    if (response.layout == ProjectionPrinterInventoryResponseLayout::None ||
        pending.source_inventory_slot < 9 || pending.source_inventory_slot > 35 ||
        pending.destination_hotbar_slot < 0 || pending.destination_hotbar_slot > 8) {
        return ResponseMoveProof::Mismatch;
    }
    const ProjectionPrinterInventoryResponseSlot* const destination =
        findUniqueResponseSlot(response, kProjectionPrinterHotbarRequestContainer,
                               pending.destination_hotbar_slot);
    const ProjectionPrinterInventoryResponseSlot* const source =
        findUniqueResponseSlot(response, kProjectionPrinterInventoryRequestContainer,
                               pending.source_inventory_slot);
    if (!destination || !source || destination->count != pending.expected_source_count ||
        destination->network_stack_id <= 0) {
        return ResponseMoveProof::Mismatch;
    }
    if (!pending.expected_destination_occupied) {
        if (source->count != 0U || source->network_stack_id != 0) {
            return ResponseMoveProof::Mismatch;
        }
    } else if (source->count != pending.expected_destination_count ||
               source->network_stack_id <= 0) {
        return ResponseMoveProof::Mismatch;
    }

    if (destination_network_stack_id) {
        *destination_network_stack_id = destination->network_stack_id;
    }

    ProjectionPrinterLiveInventorySnapshot live;
    std::string ignored_error;
    if (!ReadProjectionPrinterLiveInventorySnapshot(&live, &ignored_error) || !live.ready) {
        // The response is real, but without a fresh native read it is not safe
        // to treat it as a client-side commit. Keep the older conservative
        // confirmation path available until the next tick.
        return ResponseMoveProof::NotObserved;
    }
    const ProjectionPrinterLiveInventorySlot& live_destination = live.slots[
        static_cast<size_t>(pending.destination_hotbar_slot)];
    const ProjectionPrinterLiveInventorySlot& live_source = live.slots[
        static_cast<size_t>(pending.source_inventory_slot)];
    if (!live_destination.occupied || !live_destination.has_network_stack_id ||
        live_destination.network_stack_id != destination->network_stack_id ||
        live_source.network_stack_id != source->network_stack_id ||
        live_source.occupied != pending.expected_destination_occupied) {
        // Without an inventory screen the native response listeners may not
        // publish a reassigned net ID. Correct only our still-intact optimistic
        // pair, backed by this exact accepted response. A user's later change
        // to either slot must never be overwritten with the old packet bytes.
        if (pending.client_slot_refresh_queued &&
            live_destination.occupied && live_destination.has_network_stack_id &&
            live_destination.network_stack_id == pending.expected_source_network_stack_id &&
            live_source.occupied == pending.expected_destination_occupied &&
            live_source.network_stack_id == (pending.expected_destination_occupied
                ? pending.expected_destination_network_stack_id : 0)) {
            return ResponseMoveProof::ClientRefreshRequired;
        }
        return ResponseMoveProof::Mismatch;
    }
    return ResponseMoveProof::Confirmed;
}

// For a full-stack Place into a slot which was proven empty at the native send
// boundary, the source->empty and destination->ordinary-ID transition is an
// independent commit proof.  This intentionally covers only the empty-hotbar
// path: an occupied destination is a Swap and cannot be attributed safely
// without the exact ItemStackResponse request ID and semantic slot data.
bool confirmEmptyDestinationInventoryMoveFromLive(
    const PendingInventoryMove& pending, int32_t* destination_network_stack_id) {
    if (destination_network_stack_id) *destination_network_stack_id = 0;
    if (pending.client_slot_refresh_queued || pending.expected_destination_occupied ||
        pending.source_inventory_slot < 9 || pending.source_inventory_slot > 35 ||
        pending.destination_hotbar_slot < 0 || pending.destination_hotbar_slot > 8 ||
        pending.expected_source_network_stack_id <= 0 || pending.expected_source_count == 0U) {
        return false;
    }

    ProjectionPrinterLiveInventorySnapshot live;
    std::string ignored_error;
    if (!ReadProjectionPrinterLiveInventorySnapshot(&live, &ignored_error) || !live.ready) {
        return false;
    }
    const ProjectionPrinterLiveInventorySlot& source = live.slots[
        static_cast<size_t>(pending.source_inventory_slot)];
    const ProjectionPrinterLiveInventorySlot& destination = live.slots[
        static_cast<size_t>(pending.destination_hotbar_slot)];
    // `MoveProjectionPrinterBackpackItemToHotbar` had already reread the exact
    // original source ID and an empty destination immediately before sending a
    // full Place action.  A native state transition to this pair therefore
    // proves a committed move without trusting the delayed Python item cache.
    if (source.occupied || source.has_network_stack_id || source.network_stack_id != 0 ||
        !destination.occupied || !destination.has_network_stack_id ||
        destination.network_stack_id <= 0) {
        return false;
    }
    if (destination_network_stack_id) {
        *destination_network_stack_id = destination.network_stack_id;
    }
    return true;
}

bool samePendingInventoryMove(const PendingInventoryMove& left,
                              const PendingInventoryMove& right) {
    return left.inventory_session_token == right.inventory_session_token &&
        left.source_inventory_slot == right.source_inventory_slot &&
        left.destination_hotbar_slot == right.destination_hotbar_slot &&
        left.expected_source_network_stack_id == right.expected_source_network_stack_id &&
        left.verification_source == right.verification_source &&
        left.mailbox_session_generation == right.mailbox_session_generation &&
        left.response_session_generation == right.response_session_generation &&
        left.mailbox_revision_before_send == right.mailbox_revision_before_send &&
        left.response_generation_before_send == right.response_generation_before_send &&
        left.request_id == right.request_id;
}

bool requestHotbarSelection(int32_t target_slot) {
    std::string ignored_error;
    if (RequestProjectionPrinterNativeHotbarSelection(target_slot, &ignored_error)) return true;

    // Compatibility bootstrap only: on a late Xposed load the existing HUD may
    // predate the native lifecycle hook. This sends the same stock key event
    // that the old implementation used; its controller call is observed by
    // the native module, so following attempts move onto the direct listener
    // + network selection path. It never writes an ItemStack or invents a
    // packet, and remains a fallback when the guarded native model is absent.
    if (target_slot < 0 || target_slot > 8) return false;
    const std::string statements =
        "import gui\n"
        "_infinitecz_printer_select_result = 'failed'\n"
        "try:\n"
        "  gui.simulate_keyboard_event(" + std::to_string(49 + target_slot) + ", 1)\n"
        "  gui.simulate_keyboard_event(" + std::to_string(49 + target_slot) + ", 0)\n"
        "  _infinitecz_printer_select_result = 'switching'\n"
        "except BaseException:\n"
        "  _infinitecz_printer_select_result = 'failed'\n";
    std::string response;
    return PythonUtils::PyEvalUtf8(statements, "_infinitecz_printer_select_result", &response) &&
        response == "switching";
}

MaterialSelection selectMaterialForTarget(const ProjectionPrinterTarget& target,
                                           const InventorySnapshot& inventory,
                                           const InventorySlot** material_slot) {
    if (material_slot) *material_slot = nullptr;
    if (!inventory.available) return MaterialSelection::Failed;
    // Prefer an already visible material even if the snapshot happens to list
    // backpack slots first. Opening a player-inventory session is reserved for
    // a genuine hotbar miss.
    for (const InventorySlot& slot : inventory.slots) {
        if (slot.slot < 0 || slot.slot > 8) continue;
        if (!materialMatchesTarget(slot, target)) {
            continue;
        }
        if (material_slot) *material_slot = &slot;
        return slot.slot == inventory.selected_hotbar_slot
            ? MaterialSelection::Ready : MaterialSelection::SwitchingHotbar;
    }
    // The exchange path is gated by an actual native inventory session,
    // cancellation of its UI handler, the matching ItemStackResponse and a
    // fresh live hotbar snapshot. Returning InBackpack only identifies a
    // candidate here; onGameTick performs every effect boundary separately.
    for (const InventorySlot& slot : inventory.slots) {
        if (slot.slot < 9 || slot.slot > 35) continue;
        if (!materialMatchesTarget(slot, target)) continue;
        if (material_slot) *material_slot = &slot;
        return MaterialSelection::InBackpack;
    }
    return MaterialSelection::Missing;
}

enum class PendingStateAdjustmentMaterial : uint8_t {
    Ready,
    EmptyButSafe,
    SnapshotUnavailable,
    Changed,
};

// The first placement may legitimately consume or replace the native stack ID.
// Before a later normal-use state click, take a fresh snapshot instead of
// retaining that old ID.  This prevents an in-place hotbar swap by the player
// from using an unrelated item on an already placed redstone block.
PendingStateAdjustmentMaterial readPendingStateAdjustmentMaterial(
    const PendingPlacement& pending, InventorySlot* output) {
    if (output) *output = InventorySlot{};
    InventorySnapshot inventory;
    if (!readInventorySnapshot(&inventory, true) || !inventory.available) {
        return PendingStateAdjustmentMaterial::SnapshotUnavailable;
    }
    if (pending.selected_hotbar_slot < 0 || pending.selected_hotbar_slot > 8 ||
        inventory.selected_hotbar_slot != pending.selected_hotbar_slot) {
        return PendingStateAdjustmentMaterial::Changed;
    }
    const InventorySlot* const material = findInventorySlot(
        inventory, pending.selected_hotbar_slot);
    const NetworkInventorySlot* const network_slot = inventory.network_ready
        ? &inventory.network_slots[static_cast<size_t>(pending.selected_hotbar_slot)] : nullptr;
    // The final source item can be consumed by the placement itself.  An empty
    // hand may still use a repeater/comparator/daylight detector normally, but
    // only accept that case when the fresh native/mailbox identity view also
    // proves this selected slot is empty.
    if (!material) {
        if (network_slot && !network_slot->occupied) {
            return PendingStateAdjustmentMaterial::EmptyButSafe;
        }
        return network_slot ? PendingStateAdjustmentMaterial::Changed
                            : PendingStateAdjustmentMaterial::SnapshotUnavailable;
    }
    if (!materialMatchesTarget(*material, pending.target) ||
        (network_slot && !network_slot->occupied)) {
        return PendingStateAdjustmentMaterial::Changed;
    }
    if (output) *output = *material;
    return PendingStateAdjustmentMaterial::Ready;
}

std::string scopedStatus(std::string value) {
    constexpr size_t kMaximumStatusBytes = 180U;
    if (value.size() > kMaximumStatusBytes) value.resize(kMaximumStatusBytes);
    return value;
}

bool isTransientPacketError(std::string_view error) {
    return containsToken(error, "LoopbackPacketSender") ||
           containsToken(error, "local player") || containsToken(error, "selected ItemStack") ||
           containsToken(error, "inventory is unavailable");
}

enum class PendingRedstoneAdjustmentKind : uint8_t {
    None,
    RepeaterDelay,
    ComparatorMode,
    DaylightDetectorMode,
};

struct PendingRedstoneAdjustment {
    PendingRedstoneAdjustmentKind kind = PendingRedstoneAdjustmentKind::None;
    uint8_t clicks_needed = 0U;
    uint16_t current_static_aux = 0U;
    uint16_t expected_static_aux_after_click = 0U;
};

bool resolvePendingRedstoneAdjustment(const ProjectionPrinterTarget& target,
                                      const NativeBlockView& actual,
                                      PendingRedstoneAdjustment* output) {
    if (!output || !actual.name ||
        !ProjectionBlockMaterialMatches(target.name, target.aux, *actual.name, actual.aux)) {
        return false;
    }
    *output = {};
    if (IsProjectionDaylightDetector(target.name) &&
        IsProjectionDaylightDetector(*actual.name)) {
        output->kind = PendingRedstoneAdjustmentKind::DaylightDetectorMode;
        const bool target_inverted = IsProjectionInvertedDaylightDetector(target.name);
        const bool actual_inverted = IsProjectionInvertedDaylightDetector(*actual.name);
        output->clicks_needed = target_inverted == actual_inverted ? 0U : 1U;
        output->current_static_aux = actual_inverted ? 1U : 0U;
        output->expected_static_aux_after_click = target_inverted ? 1U : 0U;
        return true;
    }
    uint16_t expected_aux = 0U;
    if (!effectivePlacementAux(target, &expected_aux)) return false;

    uint8_t expected_direction = 0U;
    uint8_t expected_delay = 0U;
    uint8_t actual_direction = 0U;
    uint8_t actual_delay = 0U;
    if (TryProjectionRepeaterState(target.name, expected_aux, &expected_direction,
                                   &expected_delay)) {
        if (!TryProjectionRepeaterState(*actual.name, actual.aux, &actual_direction,
                                       &actual_delay) || actual_direction != expected_direction) {
            return false;
        }
        output->kind = PendingRedstoneAdjustmentKind::RepeaterDelay;
        output->clicks_needed = static_cast<uint8_t>(
            (static_cast<unsigned int>(expected_delay) + 4U - actual_delay) & 0x03U);
        output->current_static_aux = static_cast<uint16_t>(actual.aux & UINT16_C(0x000F));
        const uint8_t next_delay = static_cast<uint8_t>((actual_delay % 4U) + 1U);
        output->expected_static_aux_after_click = static_cast<uint16_t>(
            (actual.aux & UINT16_C(0x0003)) |
            (static_cast<uint16_t>(next_delay - 1U) << 2U));
        return true;
    }

    bool expected_subtract = false;
    bool actual_subtract = false;
    if (TryProjectionComparatorState(target.name, expected_aux, &expected_direction,
                                    &expected_subtract)) {
        if (!TryProjectionComparatorState(*actual.name, actual.aux, &actual_direction,
                                         &actual_subtract) || actual_direction != expected_direction) {
            return false;
        }
        output->kind = PendingRedstoneAdjustmentKind::ComparatorMode;
        output->clicks_needed = expected_subtract == actual_subtract ? 0U : 1U;
        output->current_static_aux = static_cast<uint16_t>(actual.aux & UINT16_C(0x0007));
        output->expected_static_aux_after_click = static_cast<uint16_t>(
            (actual.aux & UINT16_C(0x0003)) |
            (actual_subtract ? 0U : UINT16_C(0x0004)));
        return true;
    }
    return false;
}

const char* pendingRedstoneAdjustmentLabel(PendingRedstoneAdjustmentKind kind) {
    switch (kind) {
        case PendingRedstoneAdjustmentKind::RepeaterDelay: return "中继器延迟";
        case PendingRedstoneAdjustmentKind::ComparatorMode: return "比较器模式";
        case PendingRedstoneAdjustmentKind::DaylightDetectorMode: return "日光传感器模式";
        case PendingRedstoneAdjustmentKind::None: return "红石状态";
    }
    return "红石状态";
}

// A silent player-inventory ContainerOpen is kept separate from ordinary
// coordinate-bound container capture.  The supplied backpack-manager flow
// leaves the local player inventory (container id 0) open at the protocol
// level and retires only its local silent-session state; only non-zero windows
// receive a native close.  This helper is only called on the game tick;
// cancellation itself is thread-safe and can also be requested by JNI.
void finishProjectionPrinterInventorySession(uint64_t token) noexcept {
    if (token == 0U) return;
    try {
        ProjectionPrinterInventorySessionCloseRequest close;
        if (GetProjectionPrinterInventorySessionCloseRequest(token, &close) &&
            close.requires_client_close) {
            // Retire only the exact printer-owned remote window. No stock
            // inventory screen is created for this silent session.
            ExpectProjectionPrinterInventoryPresentationClose(close.container_id,
                                                               close.container_type);
            std::string ignored_error;
            if (ContainerClosePacketSender::send(close.container_id,
                                                 close.container_type,
                                                 &ignored_error)) {
                MarkProjectionPrinterInventorySessionCloseSent(
                    token, close.container_id, close.container_type);
            }
        }
        CancelProjectionPrinterInventorySession(token);
        ClearProjectionPrinterInventoryPresentationGate();
    } catch (...) {
        // The mailbox remains fail-closed. A later game tick will still check
        // its quarantine tail and issue a close if a delayed window appears.
        CancelProjectionPrinterInventorySession(token);
        ClearProjectionPrinterInventoryPresentationGate();
    }
}

void serviceProjectionPrinterInventorySessionQuarantine() noexcept {
    try {
        ProjectionPrinterInventorySessionQuarantine quarantine;
        if (!PollProjectionPrinterInventorySessionQuarantine(&quarantine) ||
            !quarantine.container_opened || quarantine.container_closed ||
            quarantine.client_close_sent || !quarantine.requires_client_close) {
            return;
        }
        std::string ignored_error;
        if (ContainerClosePacketSender::send(quarantine.container_id,
                                             quarantine.container_type,
                                             &ignored_error)) {
            MarkProjectionPrinterInventorySessionQuarantineCloseSent(
                quarantine.container_id, quarantine.container_type);
        }
    } catch (...) {
        // A session's one-second quarantine is intentionally best-effort;
        // never let cleanup interfere with the LocalPlayer tick.
    }
}

}  // namespace

struct ProjectionPrinterRuntime::RuntimeState {
    bool enabled = false;
    bool reachability_preview_enabled = false;
    int32_t blocks_per_second = kDefaultPrinterRate;
    ProjectionPrinterState current_state = ProjectionPrinterState::Off;
    std::string current_status = "打印机已关闭";
    uint64_t projection_generation = 0;
    uint64_t plan_identity = 0;
    uint64_t display_scope_total = 0;
    bool display_scope_total_known = false;
    uint64_t completed_count = 0;
    uint64_t skipped_count = 0;
    std::unordered_set<PrinterPosition, PrinterPositionHash> completed;
    std::unordered_set<PrinterPosition, PrinterPositionHash> skipped;
    std::unordered_map<PrinterPosition, uint8_t, PrinterPositionHash>
        confirmation_failures;
    std::optional<PendingPlacement> pending;
    std::optional<PendingSilentRotation> pending_silent_rotation;
    std::optional<PendingInventoryMove> pending_inventory_move;
    std::optional<VerifiedHotbarOverride> verified_hotbar_override;
    std::array<std::optional<InventoryMoveRetryGuard>,
               kProjectionPrinterPlayerInventorySlotCount> inventory_move_retry_guards{};
    uint64_t inventory_move_retry_session_generation = 0U;
    uint64_t next_inventory_session_token = 1U;
    uint64_t active_inventory_session_token = 0U;
    bool active_inventory_session_content_ready = false;
    // The generation which was current before this session sent Interact(OpenInventory),
    // plus the later generation that actually completed the slow stock
    // ContainerOpen path and hidden screen handoff. A silent move is forbidden
    // until the latter is present.
    uint64_t inventory_session_presentation_generation_before_open = 0U;
    uint64_t active_inventory_session_presentation_generation = 0U;
    std::chrono::steady_clock::time_point inventory_session_opened_at{};
    uint64_t inventory_session_close_token = 0U;
    uint8_t inventory_session_close_ticks_remaining = 0U;
    std::chrono::steady_clock::time_point next_dispatch{};
    std::chrono::steady_clock::time_point next_inventory_attempt{};
    std::chrono::steady_clock::time_point next_reachability_preview_refresh{};
    // This cache belongs to the material-preview UI, not to a printer move.
    // It intentionally contains only value data copied out of the game API.
    bool material_inventory_refresh_requested = false;
    bool material_inventory_ready = false;
    uint64_t material_inventory_revision = 0U;
    uint64_t material_inventory_mailbox_session_generation = 0U;
    std::vector<ProjectionPrinterMaterialInventoryEntry> material_inventory_entries;
    std::chrono::steady_clock::time_point material_inventory_updated_at{};
    std::chrono::steady_clock::time_point next_material_inventory_refresh{};
};

ProjectionPrinterRuntime::~ProjectionPrinterRuntime() = default;

ProjectionPrinterRuntime& ProjectionPrinterRuntime::instance() {
    static ProjectionPrinterRuntime runtime;
    return runtime;
}

ProjectionPrinterRuntime::RuntimeState* ProjectionPrinterRuntime::stateData() {
    if (!runtime_) runtime_ = std::make_unique<RuntimeState>();
    return runtime_.get();
}

const ProjectionPrinterRuntime::RuntimeState* ProjectionPrinterRuntime::stateData() const {
    return runtime_.get();
}

void ProjectionPrinterRuntime::setEnabled(bool enabled) {
    uint64_t session_to_cancel = 0U;
    ProjectionPrinterSilentRotation::Ticket rotation_to_cancel = 0U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (data->enabled == enabled) return;
        data->enabled = enabled;
        data->pending.reset();
        if (data->pending_silent_rotation) {
            rotation_to_cancel = data->pending_silent_rotation->ticket;
            data->pending_silent_rotation.reset();
        }
        data->pending_inventory_move.reset();
        data->inventory_move_retry_guards = {};
        data->inventory_move_retry_session_generation = 0U;
        data->next_dispatch = std::chrono::steady_clock::time_point{};
        if (!enabled) {
            session_to_cancel = data->active_inventory_session_token;
            data->active_inventory_session_token = 0U;
            data->active_inventory_session_content_ready = false;
            data->inventory_session_presentation_generation_before_open = 0U;
            data->active_inventory_session_presentation_generation = 0U;
            data->inventory_session_opened_at = {};
            data->inventory_session_close_token = 0U;
            data->inventory_session_close_ticks_remaining = 0U;
            data->verified_hotbar_override.reset();
            data->confirmation_failures.clear();
            data->current_state = ProjectionPrinterState::Off;
            data->current_status = "打印机已关闭";
        } else {
            data->current_state = ProjectionPrinterState::Idle;
            data->current_status = "等待投影、玩家位置和可放置目标";
        }
    }
    // This only changes the mailbox state; native close emission is deferred
    // to the LocalPlayer game tick, where it is safe to touch the sender.
    if (!enabled) {
        // Do not let a response-confirmed local slot refresh from a disabled
        // printer enter the next game receive poll.
        ClearProjectionPrinterInventoryClientSync();
    }
    if (session_to_cancel != 0U) {
        CancelProjectionPrinterInventorySession(session_to_cancel);
        ClearProjectionPrinterInventoryPresentationGate();
    }
    if (rotation_to_cancel != 0U) {
        ProjectionPrinterSilentRotation::cancel(rotation_to_cancel);
    }
}

bool ProjectionPrinterRuntime::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const RuntimeState* const data = stateData();
    return data && data->enabled;
}

void ProjectionPrinterRuntime::setRate(int32_t blocks_per_second) {
    std::lock_guard<std::mutex> lock(mutex_);
    stateData()->blocks_per_second = std::max(kMinimumPrinterRate,
                                               std::min(kMaximumPrinterRate,
                                                        blocks_per_second));
}

int32_t ProjectionPrinterRuntime::rate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const RuntimeState* const data = stateData();
    return data ? data->blocks_per_second : kDefaultPrinterRate;
}

void ProjectionPrinterRuntime::setReachabilityPreviewEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        data->reachability_preview_enabled = enabled;
        // Permit a first scan immediately after the UI toggle or a manual
        // re-enable, rather than waiting behind a prior throttled tick.
        data->next_reachability_preview_refresh = std::chrono::steady_clock::time_point{};
    }
    BuildProjectionRenderer::instance().setReachabilityPreviewEnabled(enabled);
}

bool ProjectionPrinterRuntime::reachabilityPreviewEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const RuntimeState* const data = stateData();
    return data && data->reachability_preview_enabled;
}

ProjectionPrinterState ProjectionPrinterRuntime::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const RuntimeState* const data = stateData();
    return data ? data->current_state : ProjectionPrinterState::Off;
}

std::string ProjectionPrinterRuntime::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const RuntimeState* const data = stateData();
    return data ? data->current_status : "打印机已关闭";
}

uint64_t ProjectionPrinterRuntime::totalBlocks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const RuntimeState* const data = stateData();
    if (!data || !data->display_scope_total_known) return 0;
    return std::max(data->display_scope_total, data->completed_count);
}

uint64_t ProjectionPrinterRuntime::placedBlocks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const RuntimeState* const data = stateData();
    return data ? data->completed_count : 0;
}

uint64_t ProjectionPrinterRuntime::skippedBlocks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const RuntimeState* const data = stateData();
    return data ? data->skipped_count : 0;
}

void ProjectionPrinterRuntime::queryMaterialInventorySnapshot(
        ProjectionPrinterMaterialInventorySnapshot* output) {
    if (!output) return;
    *output = ProjectionPrinterMaterialInventorySnapshot{};
    try {
        const auto now = std::chrono::steady_clock::now();
        const uint64_t mailbox_session_generation =
            GetProjectionPrinterInventoryMailboxSessionGeneration();
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        // A caller may be a Java UI worker; request work and return the last
        // immutable-value copy rather than entering the engine from JNI.
        data->material_inventory_refresh_requested = true;
        if (data->material_inventory_ready &&
            data->material_inventory_mailbox_session_generation !=
                mailbox_session_generation) {
            // StartGame/Disconnect/world boundaries invalidate native item
            // identities. Do not show a former world's green materials even
            // for the short cache-age grace period.
            data->material_inventory_ready = false;
            data->material_inventory_entries.clear();
            data->material_inventory_updated_at = {};
        }
        output->revision = data->material_inventory_revision;
        if (!data->material_inventory_ready ||
            data->material_inventory_updated_at.time_since_epoch().count() == 0 ||
            now - data->material_inventory_updated_at > kMaterialInventoryMaximumAge) {
            output->status = ProjectionPrinterMaterialInventoryStatus::Pending;
            return;
        }
        output->status = ProjectionPrinterMaterialInventoryStatus::Ready;
        output->entries = data->material_inventory_entries;
    } catch (...) {
        // The overlay must fail open: an allocation failure simply leaves every
        // material in its normal (non-green) presentation until a later poll.
        *output = ProjectionPrinterMaterialInventorySnapshot{};
        output->status = ProjectionPrinterMaterialInventoryStatus::Unavailable;
    }
}

void ProjectionPrinterRuntime::clear() {
    uint64_t session_to_cancel = 0U;
    ProjectionPrinterSilentRotation::Ticket rotation_to_cancel = 0U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        const int32_t retained_rate = data->blocks_per_second;
        session_to_cancel = data->active_inventory_session_token;
        if (data->pending_silent_rotation) {
            rotation_to_cancel = data->pending_silent_rotation->ticket;
        }
        *data = RuntimeState{};
        data->blocks_per_second = retained_rate;
    }
    BuildProjectionRenderer::instance().clearReachabilityPreview();
    ClearProjectionPrinterInventoryClientSync();
    if (session_to_cancel != 0U) {
        CancelProjectionPrinterInventorySession(session_to_cancel);
        ClearProjectionPrinterInventoryPresentationGate();
    }
    if (rotation_to_cancel != 0U) {
        ProjectionPrinterSilentRotation::cancel(rotation_to_cancel);
    }
}

void ProjectionPrinterRuntime::onGameTick() {
    // Run even while the printer is disabled: a previous JNI disable can have
    // raced a delayed ContainerOpen, whose short quarantine still deserves a
    // best-effort native close on this safe game thread.
    serviceProjectionPrinterInventorySessionQuarantine();
    ServiceProjectionPrinterInventoryPresentationGateOnGameThread();
    // Service the read-only material-list cache before checking whether the
    // printer itself is enabled.  The preview is useful independently of
    // automatic printing, but its embedded-Python item read still belongs on
    // this verified game tick rather than on a JNI/UI thread.
    bool refresh_material_inventory = false;
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (data->material_inventory_refresh_requested &&
            now >= data->next_material_inventory_refresh) {
            data->material_inventory_refresh_requested = false;
            data->next_material_inventory_refresh = now +
                kMaterialInventoryRefreshInterval;
            refresh_material_inventory = true;
        }
    }
    if (refresh_material_inventory) {
        bool snapshot_ready = false;
        const uint64_t mailbox_session_before =
            GetProjectionPrinterInventoryMailboxSessionGeneration();
        std::vector<ProjectionPrinterMaterialInventoryEntry> material_entries;
        try {
            InventorySnapshot inventory;
            snapshot_ready = readInventorySnapshot(&inventory, false) && inventory.available;
            if (snapshot_ready) {
                material_entries.reserve(inventory.slots.size());
                for (const InventorySlot& slot : inventory.slots) {
                    if (slot.name.empty() || slot.count <= 0) continue;
                    ProjectionPrinterMaterialInventoryEntry entry;
                    entry.item_name = slot.name;
                    entry.item_aux = slot.aux;
                    entry.count = static_cast<uint64_t>(slot.count);
                    material_entries.push_back(std::move(entry));
                }
            }
        } catch (...) {
            snapshot_ready = false;
            material_entries.clear();
        }
        const uint64_t mailbox_session_after =
            GetProjectionPrinterInventoryMailboxSessionGeneration();
        if (mailbox_session_before != mailbox_session_after) {
            snapshot_ready = false;
            material_entries.clear();
        }
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            // Never retain an old green result after an unsuccessful current
            // read.  A failed cache is represented as pending/unavailable by
            // the UI and will be retried by its next low-frequency poll.
            data->material_inventory_ready = snapshot_ready;
            data->material_inventory_mailbox_session_generation = snapshot_ready
                ? mailbox_session_after : 0U;
            if (snapshot_ready) {
                data->material_inventory_entries = std::move(material_entries);
            } else {
                data->material_inventory_entries.clear();
            }
            data->material_inventory_updated_at = std::chrono::steady_clock::now();
            ++data->material_inventory_revision;
            if (data->material_inventory_revision == 0U) {
                ++data->material_inventory_revision;
            }
        } catch (...) {
            // The printer must remain independent from a presentation cache.
        }
    }
    // All engine, world-reader and embedded-Python interaction intentionally
    // happens after taking a small configuration snapshot. JNI calls may
    // toggle or clear the printer at any time, so every commit below checks
    // the projection identity again under the lock.
    int32_t configured_rate = kDefaultPrinterRate;
    uint64_t observed_generation = 0;
    uint64_t observed_plan = 0;
    bool printer_enabled = false;
    bool reachability_preview_enabled = false;
    bool reachability_preview_due = false;
    std::optional<PendingPlacement> pending;
    std::optional<PendingSilentRotation> pending_silent_rotation;
    std::optional<PendingInventoryMove> pending_inventory_move;
    const uint64_t inventory_session_generation =
        GetProjectionPrinterInventoryMailboxSessionGeneration();
    const auto tick_now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (!data->enabled && !data->reachability_preview_enabled) return;
        printer_enabled = data->enabled;
        reachability_preview_enabled = data->reachability_preview_enabled;
        if (reachability_preview_enabled &&
            tick_now >= data->next_reachability_preview_refresh) {
            reachability_preview_due = true;
            data->next_reachability_preview_refresh = tick_now +
                kReachabilityPreviewRefreshInterval;
        }
        if (printer_enabled &&
            data->inventory_move_retry_session_generation != inventory_session_generation) {
            data->inventory_move_retry_guards = {};
            data->inventory_move_retry_session_generation = inventory_session_generation;
        }
        configured_rate = data->blocks_per_second;
        observed_generation = data->projection_generation;
        observed_plan = data->plan_identity;
        pending = data->pending;
        pending_silent_rotation = data->pending_silent_rotation;
        pending_inventory_move = data->pending_inventory_move;
    }

    // Do not close the hidden player-inventory window in the same tick that
    // observed a successful request.  The response has already reached the
    // native container at that point, but the hotbar's listener/render commit
    // can run on the following LocalPlayer ticks.  Reference inventory flows
    // keep their silent window open across transactions for the same reason.
    uint64_t session_to_finish_after_ui_commit = 0U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (data->enabled && data->inventory_session_close_token != 0U) {
            if (data->inventory_session_close_token !=
                    data->active_inventory_session_token) {
                data->inventory_session_close_token = 0U;
                data->inventory_session_close_ticks_remaining = 0U;
            } else if (!data->pending_inventory_move) {
                if (data->inventory_session_close_ticks_remaining != 0U) {
                    --data->inventory_session_close_ticks_remaining;
                }
                if (data->inventory_session_close_ticks_remaining == 0U) {
                    session_to_finish_after_ui_commit =
                        data->inventory_session_close_token;
                    data->inventory_session_close_token = 0U;
                    data->active_inventory_session_token = 0U;
                    data->active_inventory_session_content_ready = false;
                    data->inventory_session_presentation_generation_before_open = 0U;
                    data->active_inventory_session_presentation_generation = 0U;
                    data->inventory_session_opened_at = {};
                }
            }
        }
    }
    if (session_to_finish_after_ui_commit != 0U) {
        finishProjectionPrinterInventorySession(session_to_finish_after_ui_commit);
    }

    // Preview-only operation is deliberately throttled before it reaches any
    // game/world query. The previously published immutable render snapshot
    // remains visible during this short interval.
    if (!printer_enabled && !reachability_preview_due) return;

    if (!BuildProjectionRenderer::instance().isEnabled()) {
        if (reachability_preview_enabled) {
            BuildProjectionRenderer::instance().clearReachabilityPreview();
        }
        if (!printer_enabled) return;
        uint64_t session_to_cancel = 0U;
        ProjectionPrinterSilentRotation::Ticket rotation_to_cancel = 0U;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled) {
                session_to_cancel = data->active_inventory_session_token;
                data->active_inventory_session_token = 0U;
                data->active_inventory_session_content_ready = false;
                data->inventory_session_presentation_generation_before_open = 0U;
                data->active_inventory_session_presentation_generation = 0U;
                data->inventory_session_opened_at = {};
                data->inventory_session_close_token = 0U;
                data->inventory_session_close_ticks_remaining = 0U;
                data->pending.reset();
                if (data->pending_silent_rotation) {
                    rotation_to_cancel = data->pending_silent_rotation->ticket;
                    data->pending_silent_rotation.reset();
                }
                data->pending_inventory_move.reset();
                data->current_state = ProjectionPrinterState::Idle;
                data->current_status = "投影已隐藏，打印机不会处理不可见方块";
            }
        }
        if (session_to_cancel != 0U) {
            finishProjectionPrinterInventorySession(session_to_cancel);
        }
        if (rotation_to_cancel != 0U) {
            ProjectionPrinterSilentRotation::cancel(rotation_to_cancel);
        }
        return;
    }

    int32_t player_x = 0;
    int32_t player_y = 0;
    int32_t player_z = 0;
    if (!NativeWorldAccess::getLocalPlayerBlockPosition(&player_x, &player_y, &player_z)) {
        if (reachability_preview_enabled) {
            BuildProjectionRenderer::instance().clearReachabilityPreview();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (data->enabled) {
            data->current_state = ProjectionPrinterState::Idle;
            data->current_status = "等待本地玩家进入可读取的世界";
        }
        return;
    }

    ProjectionPrinterQuery query;
    query.player_x = player_x;
    query.player_y = player_y;
    query.player_z = player_z;
    query.horizontal_radius_blocks = kInteractionHorizontalRadius;
    query.vertical_radius_blocks = kInteractionVerticalRadius;
    query.maximum_targets = kMaximumCandidateTargets;
    query.restrict_to_display_scope = true;
    ProjectionPrinterQueryResult query_result;
    std::string query_error;
    if (!BuildProjectionRuntime::instance().queryNearbyPrinterTargets(query, &query_result,
                                                                         &query_error)) {
        if (reachability_preview_enabled && reachability_preview_due) {
            BuildProjectionRenderer::instance().clearReachabilityPreview();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (data->enabled) {
            data->current_state = ProjectionPrinterState::Idle;
            data->current_status = scopedStatus(query_error.empty()
                ? "等待投影原始数据可用" : query_error);
        }
        return;
    }

    // This visual-only path intentionally stops before any printer session,
    // material, status or packet machinery. It evaluates exactly the existing
    // target/support predicates, so the mint preview remains trustworthy even
    // while automatic printing is off.
    if (!printer_enabled) {
        if (reachability_preview_enabled && reachability_preview_due) {
            publishReachabilityPreviewForQuery(query_result, player_x, player_y, player_z);
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    uint64_t session_to_cancel_for_plan_change = 0U;
    ProjectionPrinterSilentRotation::Ticket rotation_to_cancel_for_plan_change = 0U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (!data->enabled) return;
        if (data->projection_generation != query_result.generation ||
            data->plan_identity != query_result.plan_identity) {
            session_to_cancel_for_plan_change = data->active_inventory_session_token;
            data->active_inventory_session_token = 0U;
            data->active_inventory_session_content_ready = false;
            data->inventory_session_presentation_generation_before_open = 0U;
            data->active_inventory_session_presentation_generation = 0U;
            data->inventory_session_opened_at = {};
            data->inventory_session_close_token = 0U;
            data->inventory_session_close_ticks_remaining = 0U;
            data->projection_generation = query_result.generation;
            data->plan_identity = query_result.plan_identity;
            data->completed.clear();
            data->skipped.clear();
            data->confirmation_failures.clear();
            data->completed_count = 0;
            data->skipped_count = 0;
            data->pending.reset();
            if (data->pending_silent_rotation) {
                rotation_to_cancel_for_plan_change = data->pending_silent_rotation->ticket;
                data->pending_silent_rotation.reset();
            }
            data->pending_inventory_move.reset();
            data->verified_hotbar_override.reset();
            data->next_dispatch = now;
            data->next_inventory_attempt = now;
        }
        if (query_result.display_scope_block_count_known) {
            data->display_scope_total = query_result.display_scope_block_count;
            data->display_scope_total_known = true;
        } else {
            data->display_scope_total_known = false;
            data->display_scope_total = 0;
        }
        observed_generation = data->projection_generation;
        observed_plan = data->plan_identity;
        pending = data->pending;
        pending_silent_rotation = data->pending_silent_rotation;
        pending_inventory_move = data->pending_inventory_move;
        configured_rate = data->blocks_per_second;
    }
    if (session_to_cancel_for_plan_change != 0U) {
        finishProjectionPrinterInventorySession(session_to_cancel_for_plan_change);
    }
    if (rotation_to_cancel_for_plan_change != 0U) {
        ProjectionPrinterSilentRotation::cancel(rotation_to_cancel_for_plan_change);
        pending_silent_rotation.reset();
    }

    if (query_result.status == ProjectionPrinterQueryStatus::NoProjection ||
        query_result.status == ProjectionPrinterQueryStatus::SourceUnavailable) {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (data->enabled && data->projection_generation == observed_generation &&
            data->plan_identity == observed_plan) {
            data->current_state = ProjectionPrinterState::Idle;
            data->current_status = "等待投影原始数据完成加载";
        }
        return;
    }

    // A native inventory transaction has already been submitted. Do not
    // inspect candidates or send ItemUse while it is in flight. Confirmation
    // prefers a fresh live-native snapshot paired with current item-component
    // semantics, with the original passive mailbox as a limited fallback.
    if (pending_inventory_move) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const RuntimeState* const data = stateData();
            if (!data || !data->enabled ||
                data->projection_generation != observed_generation ||
                data->plan_identity != observed_plan || !data->pending_inventory_move ||
                !samePendingInventoryMove(*data->pending_inventory_move,
                                          *pending_inventory_move)) {
                return;
            }
            if (now < data->next_inventory_attempt) return;
        }

        // Do this before reading the Python item component.  A successful
        // ItemStackResponse can already have committed to the native Container
        // while that component still reports the old empty hotbar slot.
        ProjectionPrinterInventoryResponse response_before_python;
        const bool has_response_before_python =
            GetProjectionPrinterInventoryResponseByRequestId(
                pending_inventory_move->request_id, &response_before_python);
        const bool exact_rejection = has_response_before_python &&
            response_before_python.rejected &&
            response_before_python.request_id == pending_inventory_move->request_id &&
            response_before_python.session_generation ==
                pending_inventory_move->response_session_generation &&
            response_before_python.response_generation >
                pending_inventory_move->response_generation_before_send;
        // The verified v859 enum names status 4 ScreenHandlerEndRequestFailed.
        // It describes request teardown and does not prove the material move
        // was rolled back. Keep waiting for independent remote slot evidence.
        const bool response_completion_failed =
            projection_inventory_evidence::isRequestCompletionFailure(
                exact_rejection, response_before_python.rejection_status);
        int32_t response_destination_network_stack_id = 0;
        const ResponseMoveProof response_proof = has_response_before_python
            ? confirmInventoryMoveFromResponseAndLive(
                  *pending_inventory_move, response_before_python,
                  &response_destination_network_stack_id)
            : ResponseMoveProof::NotObserved;
        const bool response_confirmed = response_proof == ResponseMoveProof::Confirmed;
        if (response_proof == ResponseMoveProof::ClientRefreshRequired &&
            !pending_inventory_move->response_client_refresh_queued) {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled && data->projection_generation == observed_generation &&
                data->plan_identity == observed_plan && data->pending_inventory_move &&
                samePendingInventoryMove(*data->pending_inventory_move,
                                          *pending_inventory_move) &&
                confirmInventoryMoveFromResponseAndLive(
                    *pending_inventory_move, response_before_python,
                    &response_destination_network_stack_id) ==
                        ResponseMoveProof::ClientRefreshRequired) {
                const auto* response_source = findUniqueResponseSlot(
                    response_before_python, kProjectionPrinterInventoryRequestContainer,
                    pending_inventory_move->source_inventory_slot);
                std::string correction_error;
                uint64_t correction_ticket = 0U;
                if (response_source && QueueProjectionPrinterInventoryClientSyncResponseCorrection(
                        pending_inventory_move->client_slot_refresh_ticket,
                        response_source->network_stack_id,
                        response_destination_network_stack_id, &correction_ticket,
                        &correction_error)) {
                    LogProjectionPrinterInventoryDiagnostic(
                        "local_correction_queue request=%d previous_ticket=%llu ticket=%llu "
                        "source_net=%d destination_net=%d",
                        pending_inventory_move->request_id,
                        static_cast<unsigned long long>(pending_inventory_move->client_slot_refresh_ticket),
                        static_cast<unsigned long long>(correction_ticket),
                        response_source->network_stack_id, response_destination_network_stack_id);
                    data->pending_inventory_move->response_client_refresh_queued = true;
                    data->pending_inventory_move->response_client_refresh_ticket = correction_ticket;
                    data->next_inventory_attempt = now + std::chrono::milliseconds(100);
                    data->current_state = ProjectionPrinterState::WaitingMaterial;
                    data->current_status = "材料交换已确认，正在同步快捷栏物品信息";
                    return;
                }
            }
        }
        int32_t live_destination_network_stack_id = 0;
        const bool live_empty_destination_confirmed = !response_confirmed &&
            confirmEmptyDestinationInventoryMoveFromLive(
                *pending_inventory_move, &live_destination_network_stack_id);
        if (response_confirmed || live_empty_destination_confirmed) {
            const int32_t confirmed_destination_network_stack_id = response_confirmed
                ? response_destination_network_stack_id : live_destination_network_stack_id;
            const bool selection_requested = requestHotbarSelection(
                pending_inventory_move->destination_hotbar_slot);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                RuntimeState* const data = stateData();
                if (data->enabled && data->projection_generation == observed_generation &&
                    data->plan_identity == observed_plan && data->pending_inventory_move &&
                    samePendingInventoryMove(*data->pending_inventory_move,
                                              *pending_inventory_move)) {
                    VerifiedHotbarOverride override_material;
                    override_material.hotbar_slot = pending_inventory_move->destination_hotbar_slot;
                    override_material.network_stack_id = confirmed_destination_network_stack_id;
                    override_material.name = pending_inventory_move->expected_name;
                    override_material.aux = pending_inventory_move->expected_aux;
                    override_material.count = pending_inventory_move->expected_source_count;
                    override_material.expires_at = now + kVerifiedHotbarOverrideLifetime;
                    data->verified_hotbar_override = std::move(override_material);
                    LogProjectionPrinterInventoryDiagnostic(
                        "move_confirmed request=%d proof=%s destination=%d net=%d selection_requested=%d",
                        pending_inventory_move->request_id,
                        response_confirmed ? "matched_response_and_live" : "live_empty_destination",
                        pending_inventory_move->destination_hotbar_slot,
                        confirmed_destination_network_stack_id, selection_requested ? 1 : 0);
                    data->pending_inventory_move.reset();
                    if (data->active_inventory_session_token ==
                        pending_inventory_move->inventory_session_token) {
                        // The native response is independently proven. Retain
                        // the protocol session through the following tick
                        // while the native selection listeners commit.
                        data->inventory_session_close_token =
                            pending_inventory_move->inventory_session_token;
                        data->inventory_session_close_ticks_remaining =
                            kInventoryPostMoveUiCommitTicks;
                    }
                    data->next_inventory_attempt = now + kInventoryRetryDelay;
                    data->next_dispatch = data->next_inventory_attempt;
                    data->current_state = selection_requested ? ProjectionPrinterState::Running
                                                              : ProjectionPrinterState::WaitingMaterial;
                    if (response_confirmed) {
                        if (pending_inventory_move->client_slot_refresh_queued) {
                            data->current_status = selection_requested
                                ? "材料已确认换入热栏，快捷栏已同步并自动切换"
                                : "材料已确认换入热栏，快捷栏已同步";
                        } else {
                            data->current_status = selection_requested
                                ? "已由服务器响应确认材料换入热栏，正在自动切换"
                                : "已由服务器响应确认材料换入热栏，等待自动切换";
                        }
                    } else {
                        data->current_status = selection_requested
                            ? "已由游戏库存状态确认材料换入热栏，正在自动切换"
                            : "已由游戏库存状态确认材料换入热栏，等待自动切换";
                    }
                }
            }
            return;
        }
        // A locally refreshed hotbar is presentation, not acceptance: the live
        // fallback above is disabled for those moves, so it cannot hide an
        // exact server rejection by observing our own optimistic slot pair.
        if (exact_rejection && !response_completion_failed) {
            LogProjectionPrinterInventoryDiagnostic(
                "move_abort request=%d reason=response_status status=%u generation=%llu",
                pending_inventory_move->request_id,
                static_cast<unsigned>(response_before_python.rejection_status),
                static_cast<unsigned long long>(response_before_python.response_generation));
            logInventoryMoveNativeState("abort", *pending_inventory_move);
            CancelProjectionPrinterInventoryClientSyncTicket(
                pending_inventory_move->client_slot_refresh_ticket);
            CancelProjectionPrinterInventoryClientSyncTicket(
                pending_inventory_move->response_client_refresh_ticket);
            bool close_session = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                RuntimeState* const data = stateData();
                if (data->enabled && data->projection_generation == observed_generation &&
                    data->plan_identity == observed_plan && data->pending_inventory_move &&
                    samePendingInventoryMove(*data->pending_inventory_move,
                                              *pending_inventory_move)) {
                    data->pending_inventory_move.reset();
                    close_session = data->active_inventory_session_token ==
                        pending_inventory_move->inventory_session_token;
                    if (close_session) {
                        data->active_inventory_session_token = 0U;
                        data->active_inventory_session_content_ready = false;
                        data->inventory_session_presentation_generation_before_open = 0U;
                        data->active_inventory_session_presentation_generation = 0U;
                        data->inventory_session_opened_at = {};
                        data->inventory_session_close_token = 0U;
                        data->inventory_session_close_ticks_remaining = 0U;
                    }
                    data->next_inventory_attempt = now + kInventoryMoveFailureDelay;
                    data->next_dispatch = data->next_inventory_attempt;
                    data->current_state = ProjectionPrinterState::WaitingMaterial;
                    data->current_status = "本次交换请求返回异常（响应代码 " +
                        std::to_string(static_cast<unsigned int>(
                            response_before_python.rejection_status)) +
                        "），等待该槽位重新同步，未重复提交";
                }
            }
            if (close_session) {
                finishProjectionPrinterInventorySession(
                    pending_inventory_move->inventory_session_token);
            }
            return;
        }
        if (response_proof == ResponseMoveProof::Mismatch) {
            // Even after an exact request-ID match, an unexpected response-slot
            // plan is not evidence that this transaction failed: a future
            // server can include extra semantic slots in the same entry. It is
            // deliberately not allowed to authorize a block placement, but it
            // must not cancel a move which may already have committed to the
            // live player container.
        }

        // This deadline also applies when the native/Python snapshot is
        // unavailable. Checking it below those early returns used to make an
        // unavailable cache keep a submitted move pending forever.
        if (now - pending_inventory_move->sent_at >= kInventoryMoveConfirmationTimeout) {
            LogProjectionPrinterInventoryDiagnostic(
                "move_timeout request=%d elapsed_ms=%lld exact_rejection=%d status=%u proof=%u "
                "source_quarantine=retained",
                pending_inventory_move->request_id,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - pending_inventory_move->sent_at).count()),
                exact_rejection ? 1 : 0,
                exact_rejection ? static_cast<unsigned>(response_before_python.rejection_status) : 0U,
                static_cast<unsigned>(response_proof));
            logInventoryMoveNativeState("timeout", *pending_inventory_move);
            CancelProjectionPrinterInventoryClientSyncTicket(
                pending_inventory_move->client_slot_refresh_ticket);
            CancelProjectionPrinterInventoryClientSyncTicket(
                pending_inventory_move->response_client_refresh_ticket);
            bool close_session = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                RuntimeState* const data = stateData();
                if (data->enabled && data->projection_generation == observed_generation &&
                    data->plan_identity == observed_plan && data->pending_inventory_move &&
                    samePendingInventoryMove(*data->pending_inventory_move,
                                              *pending_inventory_move)) {
                    data->pending_inventory_move.reset();
                    close_session = data->active_inventory_session_token ==
                        pending_inventory_move->inventory_session_token;
                    if (close_session) {
                        data->active_inventory_session_token = 0U;
                        data->active_inventory_session_content_ready = false;
                        data->inventory_session_presentation_generation_before_open = 0U;
                        data->active_inventory_session_presentation_generation = 0U;
                        data->inventory_session_opened_at = {};
                        data->inventory_session_close_token = 0U;
                        data->inventory_session_close_ticks_remaining = 0U;
                    }
                    data->next_inventory_attempt = now + kInventoryMoveFailureDelay;
                    data->next_dispatch = data->next_inventory_attempt;
                    data->current_state = ProjectionPrinterState::WaitingMaterial;
                    data->current_status = response_completion_failed
                        ? "背包请求收尾异常（代码 4），移动结果尚待同步，未重复提交"
                        : "材料交换结果尚未同步，已保留本次记录；其他热栏材料仍可继续打印";
                }
            }
            if (close_session) {
                finishProjectionPrinterInventorySession(
                    pending_inventory_move->inventory_session_token);
            }
            return;
        }

        InventorySnapshot moved_inventory;
        // Force a fresh native container snapshot for confirmation even when
        // the original move was authorized by InventoryContent.  On current
        // Bedrock clients, a successful ItemStackRequest commonly arrives as
        // ItemStackResponse and mutates the local container without producing
        // an InventorySlot packet, so the passive mailbox can remain at its
        // pre-move revision indefinitely.
        if (!readInventorySnapshot(&moved_inventory, true)) {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled && data->projection_generation == observed_generation &&
                data->plan_identity == observed_plan && data->pending_inventory_move &&
                samePendingInventoryMove(*data->pending_inventory_move,
                                         *pending_inventory_move)) {
                data->next_inventory_attempt = now + kInventoryRetryDelay;
                data->current_state = ProjectionPrinterState::WaitingMaterial;
                data->current_status = response_completion_failed
                    ? "背包请求收尾异常，正在核对已移动材料"
                    : "等待客户端确认背包材料已换入热栏";
            }
            return;
        }
        // A later rejection response likewise cannot be tied to this move.
        // `response_generation` establishes only ordering within the shared
        // session; it does not establish request identity.  Treat it as
        // diagnostic information and continue waiting for an independently
        // attributable success proof or for the normal confirmation timeout.
        // In particular, never report a server rejection for a move merely
        // because some other inventory action was rejected in the interval.
        // A live snapshot is the current authoritative confirmation path for
        // either authorization source.  If live capture is temporarily
        // unavailable, retain the original mailbox-only fallback rather than
        // treating a stale or unrelated session as success.
        const bool has_live_confirmation =
            moved_inventory.network_ready &&
            moved_inventory.network_source == InventoryVerificationSource::LiveNative;
        const bool same_verification_source = has_live_confirmation ||
            (pending_inventory_move->verification_source ==
                 InventoryVerificationSource::Mailbox &&
             moved_inventory.mailbox_ready &&
             moved_inventory.mailbox_session_generation ==
                 pending_inventory_move->mailbox_session_generation);
        if (!same_verification_source) {
            bool close_session = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                RuntimeState* const data = stateData();
                if (data->enabled && data->projection_generation == observed_generation &&
                    data->plan_identity == observed_plan && data->pending_inventory_move &&
                    samePendingInventoryMove(*data->pending_inventory_move,
                                             *pending_inventory_move)) {
                    if (pending_inventory_move->verification_source ==
                            InventoryVerificationSource::Mailbox &&
                        moved_inventory.mailbox_ready) {
                        LogProjectionPrinterInventoryDiagnostic(
                            "move_abort request=%d reason=inventory_session_changed before=%llu after=%llu",
                            pending_inventory_move->request_id,
                            static_cast<unsigned long long>(pending_inventory_move->mailbox_session_generation),
                            static_cast<unsigned long long>(moved_inventory.mailbox_session_generation));
                        data->pending_inventory_move.reset();
                        close_session = data->active_inventory_session_token ==
                            pending_inventory_move->inventory_session_token;
                        if (close_session) {
                            data->active_inventory_session_token = 0U;
                            data->active_inventory_session_content_ready = false;
                            data->inventory_session_presentation_generation_before_open = 0U;
                            data->active_inventory_session_presentation_generation = 0U;
                            data->inventory_session_opened_at = {};
                            data->inventory_session_close_token = 0U;
                            data->inventory_session_close_ticks_remaining = 0U;
                        }
                        data->next_inventory_attempt = now + kInventoryMoveFailureDelay;
                        data->next_dispatch = data->next_inventory_attempt;
                        data->current_state = ProjectionPrinterState::WaitingMaterial;
                        data->current_status = "库存会话已刷新，已取消未确认的材料交换";
                    } else {
                        data->next_inventory_attempt = now + kInventoryRetryDelay;
                        data->current_state = ProjectionPrinterState::WaitingMaterial;
                        data->current_status = pending_inventory_move->verification_source ==
                                InventoryVerificationSource::LiveNative
                            ? "等待游戏当前物品栏确认材料已换入热栏"
                            : "等待完整的玩家物品栏同步后确认材料交换";
                    }
                }
            }
            if (close_session) {
                finishProjectionPrinterInventorySession(
                    pending_inventory_move->inventory_session_token);
            }
            return;
        }
        if (inventoryMoveConfirmed(*pending_inventory_move, moved_inventory)) {
            const bool selection_requested = requestHotbarSelection(
                pending_inventory_move->destination_hotbar_slot);
            LogProjectionPrinterInventoryDiagnostic(
                "move_confirmed request=%d proof=%s destination=%d selection_requested=%d "
                "mailbox_revision=%llu",
                pending_inventory_move->request_id,
                pending_inventory_move->client_slot_refresh_queued ? "remote_mailbox" : "inventory_snapshot",
                pending_inventory_move->destination_hotbar_slot, selection_requested ? 1 : 0,
                static_cast<unsigned long long>(moved_inventory.mailbox_revision));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                RuntimeState* const data = stateData();
                if (data->enabled && data->projection_generation == observed_generation &&
                    data->plan_identity == observed_plan && data->pending_inventory_move &&
                    samePendingInventoryMove(*data->pending_inventory_move,
                                              *pending_inventory_move)) {
                    data->pending_inventory_move.reset();
                    if (data->active_inventory_session_token ==
                        pending_inventory_move->inventory_session_token) {
                        // Let the stock ItemStackResponse/container listeners
                        // finish their UI commit before emitting ContainerClose.
                        data->inventory_session_close_token =
                            pending_inventory_move->inventory_session_token;
                        data->inventory_session_close_ticks_remaining =
                            kInventoryPostMoveUiCommitTicks;
                    }
                    data->next_inventory_attempt = now + kInventoryRetryDelay;
                    data->next_dispatch = data->next_inventory_attempt;
                    data->current_state = selection_requested ? ProjectionPrinterState::Running
                                                              : ProjectionPrinterState::WaitingMaterial;
                    data->current_status = selection_requested
                        ? "材料已确认换入热栏，正在切换后继续放置"
                        : "材料已确认换入热栏，等待客户端切换该槽位";
                }
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled && data->projection_generation == observed_generation &&
                data->plan_identity == observed_plan && data->pending_inventory_move &&
                samePendingInventoryMove(*data->pending_inventory_move,
                                         *pending_inventory_move)) {
                data->next_inventory_attempt = now + std::chrono::milliseconds(100);
                data->current_state = ProjectionPrinterState::WaitingMaterial;
                data->current_status = response_completion_failed
                    ? "背包请求收尾异常，正在核对已移动材料"
                    :
                    GetProjectionPrinterInventoryClientSyncTicketState(
                        pending_inventory_move->client_slot_refresh_ticket) ==
                        ProjectionPrinterInventoryClientSyncTicketState::Pending
                    ? "已提交材料交换，等待客户端接收快捷栏更新"
                    : "快捷栏更新已交给游戏，等待材料交换确认";
            }
        }
        return;
    }

    // Back off material/support/cold-cache scans as well as packet sends. A
    // pending placement is exempt because it must be observed promptly.
    if (!pending) {
        std::lock_guard<std::mutex> lock(mutex_);
        const RuntimeState* const data = stateData();
        if (!data || !data->enabled || data->projection_generation != observed_generation ||
            data->plan_identity != observed_plan || now < data->next_dispatch) {
            return;
        }
    }

    NativeWorldReader reader;
    if (!reader.open()) {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (data->enabled && data->projection_generation == observed_generation &&
            data->plan_identity == observed_plan) {
            data->current_state = ProjectionPrinterState::WaitingSupport;
            data->current_status = "世界区块暂不可读，等待真实方块数据";
        }
        return;
    }

    // A send is never treated as success by itself. First observe the world
    // result, then decide whether the server accepted or rejected it.
    if (pending) {
        NativeBlockView actual;
        const bool readable = reader.getBlockView(pending->position.x, pending->position.y,
                                                  pending->position.z, &actual);
        bool stair_state_unavailable = false;
        if (readable && targetMatchesBlock(pending->target, actual, true,
                                           &stair_state_unavailable)) {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled && data->projection_generation == observed_generation &&
                data->plan_identity == observed_plan && data->pending &&
                samePendingStateAdjustmentSnapshot(*data->pending, *pending)) {
                if (data->completed.insert(pending->position).second) ++data->completed_count;
                data->confirmation_failures.erase(pending->position);
                data->pending.reset();
                data->current_state = ProjectionPrinterState::Running;
                data->current_status = "已确认一个方块，继续选择附近目标";
            }
            return;
        }
        if (readable && isPartialDoubleSlabTarget(pending->target, actual)) {
            // First half of a double slab reached the world. It is progress,
            // not a mismatched occupied target: release the pending action and
            // let the normal candidate pass dispatch the later merge click.
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled && data->projection_generation == observed_generation &&
                data->plan_identity == observed_plan && data->pending &&
                samePendingStateAdjustmentSnapshot(*data->pending, *pending)) {
                data->confirmation_failures.erase(pending->position);
                data->pending.reset();
                data->next_dispatch = now + printerDispatchInterval(configured_rate);
                data->current_state = ProjectionPrinterState::Running;
                data->current_status = "已确认双层半砖的第一层，等待合并第二层";
            }
            return;
        }
        const auto abandonPendingStateAdjustment = [&](ProjectionPrinterState terminal_state,
                                                        std::string status) {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled && data->projection_generation == observed_generation &&
                data->plan_identity == observed_plan && data->pending &&
                samePendingStateAdjustmentSnapshot(*data->pending, *pending)) {
                if (data->skipped.insert(pending->position).second) ++data->skipped_count;
                data->confirmation_failures.erase(pending->position);
                data->pending.reset();
                data->next_dispatch = now + kTransientFailureRetry;
                data->current_state = terminal_state;
                data->current_status = std::move(status);
            }
        };
        if (readable && actual.name && !isEmptyPlacementTarget(*actual.name) &&
            !stair_state_unavailable) {
            PendingRedstoneAdjustment adjustment;
            if (resolvePendingRedstoneAdjustment(pending->target, actual, &adjustment) &&
                adjustment.clicks_needed != 0U) {
                if (!actual.type_token) {
                    if (now - pending->sent_at >= kPlacementConfirmationTimeout) {
                        abandonPendingStateAdjustment(
                            ProjectionPrinterState::Idle,
                            "红石方块始终没有真实运行时数据，已保留现有方块");
                    } else {
                        std::lock_guard<std::mutex> lock(mutex_);
                        RuntimeState* const data = stateData();
                        if (data->enabled && data->projection_generation == observed_generation &&
                            data->plan_identity == observed_plan && data->pending &&
                            samePendingStateAdjustmentSnapshot(*data->pending, *pending)) {
                            data->current_state = ProjectionPrinterState::WaitingSupport;
                            data->current_status = "等待红石方块的真实运行时数据";
                        }
                    }
                    return;
                }

                // A successful normal-use packet has to produce exactly the
                // next delay/mode state we calculated.  Treat a delayed,
                // out-of-order or player-caused different state as an unsafe
                // result rather than clicking again and cycling past the target.
                if (pending->state_adjustment_pending) {
                    if (adjustment.current_static_aux !=
                        pending->state_expected_after_adjustment) {
                        if (now - pending->sent_at >= kPlacementConfirmationTimeout) {
                            abandonPendingStateAdjustment(
                                ProjectionPrinterState::Idle,
                                std::string("服务器未确认预期的") +
                                    pendingRedstoneAdjustmentLabel(adjustment.kind) +
                                    "状态，已保留现有方块");
                        } else {
                            std::lock_guard<std::mutex> lock(mutex_);
                            RuntimeState* const data = stateData();
                            if (data->enabled &&
                                data->projection_generation == observed_generation &&
                                data->plan_identity == observed_plan && data->pending &&
                                samePendingStateAdjustmentSnapshot(*data->pending, *pending)) {
                                data->current_state = ProjectionPrinterState::Running;
                                data->current_status = std::string("等待服务器确认") +
                                    pendingRedstoneAdjustmentLabel(adjustment.kind);
                            }
                        }
                        return;
                    }
                    // This was an intermediate repeater delay.  Commit only
                    // the observed state, then wait for the user-configured
                    // action interval before considering another click.
                    std::lock_guard<std::mutex> lock(mutex_);
                    RuntimeState* const data = stateData();
                    if (data->enabled && data->projection_generation == observed_generation &&
                        data->plan_identity == observed_plan && data->pending &&
                        samePendingStateAdjustmentSnapshot(*data->pending, *pending)) {
                        data->pending->state_adjustment_pending = false;
                        data->pending->state_adjustment_send_failures = 0U;
                        data->pending->sent_at = now;
                        data->next_dispatch = now +
                            stateAdjustmentDispatchInterval(configured_rate);
                        data->current_state = ProjectionPrinterState::Running;
                        data->current_status = std::string("已确认") +
                            pendingRedstoneAdjustmentLabel(adjustment.kind) +
                            "状态，等待下一次安全调整";
                    }
                    return;
                }

                if (now - pending->sent_at < stateAdjustmentDispatchInterval(configured_rate)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    RuntimeState* const data = stateData();
                    if (data->enabled && data->projection_generation == observed_generation &&
                        data->plan_identity == observed_plan && data->pending &&
                        samePendingStateAdjustmentSnapshot(*data->pending, *pending)) {
                        data->current_state = ProjectionPrinterState::Running;
                        data->current_status = "等待红石方块稳定后按速度限制调整";
                    }
                    return;
                }
                if (pending->state_adjustment_count >= kMaximumPlacementConfirmationAttempts) {
                    abandonPendingStateAdjustment(ProjectionPrinterState::Idle,
                                                  "红石状态调整超过安全次数，已保留现有方块");
                    return;
                }

                InventorySlot adjustment_material;
                const PendingStateAdjustmentMaterial material_state =
                    readPendingStateAdjustmentMaterial(*pending, &adjustment_material);
                if (material_state != PendingStateAdjustmentMaterial::Ready &&
                    material_state != PendingStateAdjustmentMaterial::EmptyButSafe) {
                    if (material_state == PendingStateAdjustmentMaterial::Changed) {
                        abandonPendingStateAdjustment(
                            ProjectionPrinterState::WaitingMaterial,
                            "当前手持材料或选中热栏已变化，未调整红石状态并保留现有方块");
                    } else if (now - pending->sent_at >= kPlacementConfirmationTimeout) {
                        abandonPendingStateAdjustment(
                            ProjectionPrinterState::WaitingMaterial,
                            "无法确认当前热栏材料，未调整红石状态并保留现有方块");
                    } else {
                        std::lock_guard<std::mutex> lock(mutex_);
                        RuntimeState* const data = stateData();
                        if (data->enabled && data->projection_generation == observed_generation &&
                            data->plan_identity == observed_plan && data->pending &&
                            samePendingStateAdjustmentSnapshot(*data->pending, *pending)) {
                            data->current_state = ProjectionPrinterState::WaitingMaterial;
                            data->current_status = "等待确认当前热栏材料后再调整红石状态";
                        }
                    }
                    return;
                }

                std::string adjustment_error;
                std::lock_guard<std::mutex> lock(mutex_);
                RuntimeState* const data = stateData();
                if (!data->enabled || data->projection_generation != observed_generation ||
                    data->plan_identity != observed_plan || !data->pending ||
                    !samePendingStateAdjustmentSnapshot(*data->pending, *pending)) {
                    return;
                }
                const bool sent = ContainerOpenPacketSender::send(
                    pending->position.x, pending->position.y, pending->position.z,
                    actual.type_token, 1, &adjustment_error, pending->selected_hotbar_slot,
                    adjustment_material.network_verified,
                    adjustment_material.network_stack_id);
                if (!sent) {
                    if (containsToken(adjustment_error,
                                      "selected hotbar slot changed before ItemUse")) {
                        if (data->skipped.insert(pending->position).second) ++data->skipped_count;
                        data->confirmation_failures.erase(pending->position);
                        data->pending.reset();
                        data->next_dispatch = now + kTransientFailureRetry;
                        data->current_state = ProjectionPrinterState::WaitingMaterial;
                        data->current_status =
                            "热栏在红石状态调整前已变化，未发包并保留现有方块";
                        return;
                    }
                    PendingPlacement& live_pending = *data->pending;
                    if (live_pending.state_adjustment_send_failures <
                        std::numeric_limits<uint8_t>::max()) {
                        ++live_pending.state_adjustment_send_failures;
                    }
                    live_pending.sent_at = now;
                    live_pending.state_adjustment_pending = false;
                    if (live_pending.state_adjustment_send_failures >=
                        kMaximumStateAdjustmentSendFailures) {
                        if (data->skipped.insert(pending->position).second) ++data->skipped_count;
                        data->confirmation_failures.erase(pending->position);
                        data->pending.reset();
                        data->next_dispatch = now + kTransientFailureRetry;
                        data->current_state = ProjectionPrinterState::Idle;
                        data->current_status = "红石状态交互多次未发送，已保留现有方块";
                    } else {
                        data->next_dispatch = now +
                            stateAdjustmentDispatchInterval(configured_rate);
                        data->current_state = isTransientPacketError(adjustment_error)
                            ? ProjectionPrinterState::WaitingSupport : ProjectionPrinterState::Error;
                        data->current_status = scopedStatus(adjustment_error.empty()
                            ? "红石状态交互未发送，稍后安全重试" : adjustment_error);
                    }
                    return;
                }
                data->pending->state_adjustment_pending = true;
                data->pending->state_before_adjustment = adjustment.current_static_aux;
                data->pending->state_expected_after_adjustment =
                    adjustment.expected_static_aux_after_click;
                data->pending->state_adjustment_send_failures = 0U;
                ++data->pending->state_adjustment_count;
                data->pending->sent_at = now;
                data->next_dispatch = now + stateAdjustmentDispatchInterval(configured_rate);
                data->current_state = ProjectionPrinterState::Running;
                data->current_status = std::string("已请求调整") +
                    pendingRedstoneAdjustmentLabel(adjustment.kind) + "，等待世界确认";
                return;
            }
        }
        if (readable && actual.name && !isEmptyPlacementTarget(*actual.name) &&
            !stair_state_unavailable) {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled && data->projection_generation == observed_generation &&
                data->plan_identity == observed_plan && data->pending &&
                data->pending->position == pending->position) {
                if (data->skipped.insert(pending->position).second) ++data->skipped_count;
                data->confirmation_failures.erase(pending->position);
                data->pending.reset();
                data->current_state = ProjectionPrinterState::Idle;
                data->current_status = "服务器返回了不同方块，已跳过该坐标";
            }
            return;
        }
        if (now - pending->sent_at < kPlacementConfirmationTimeout) {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled && data->projection_generation == observed_generation &&
                data->plan_identity == observed_plan) {
                data->current_state = ProjectionPrinterState::Running;
                data->current_status = "等待服务器确认刚才的放置";
            }
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (data->enabled && data->projection_generation == observed_generation &&
            data->plan_identity == observed_plan && data->pending &&
            data->pending->position == pending->position) {
            uint8_t& attempts = data->confirmation_failures[pending->position];
            if (attempts < std::numeric_limits<uint8_t>::max()) ++attempts;
            data->pending.reset();
            if (attempts < kMaximumPlacementConfirmationAttempts) {
                data->next_dispatch = now + kConfirmationRetryDelay;
                data->current_state = ProjectionPrinterState::Idle;
                data->current_status = "放置尚未被世界确认，稍后安全重试（" +
                    std::to_string(static_cast<unsigned int>(attempts)) + "/" +
                    std::to_string(static_cast<unsigned int>(
                        kMaximumPlacementConfirmationAttempts)) + "）";
            } else {
                if (data->skipped.insert(pending->position).second) ++data->skipped_count;
                data->confirmation_failures.erase(pending->position);
                data->next_dispatch = now + kTransientFailureRetry;
                data->current_state = ProjectionPrinterState::Idle;
                data->current_status = "多次放置均未被世界确认，已跳过该坐标以避免发包循环";
            }
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (!data->enabled || data->projection_generation != observed_generation ||
            data->plan_identity != observed_plan || now < data->next_dispatch) {
            return;
        }
    }

    std::vector<Candidate> candidates;
    candidates.reserve(query_result.targets.size());
    bool saw_waiting_support = false;
    uint64_t newly_completed = 0;
    uint64_t newly_skipped = 0;
    for (const ProjectionPrinterTarget& target : query_result.targets) {
        const PrinterPosition position{target.x, target.y, target.z};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const RuntimeState* const data = stateData();
            if (!data || !data->enabled || data->projection_generation != observed_generation ||
                data->plan_identity != observed_plan || data->completed.count(position) != 0U ||
                data->skipped.count(position) != 0U) {
                continue;
            }
        }
        if (!isSupportedTarget(target)) {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled && data->projection_generation == observed_generation &&
                data->plan_identity == observed_plan && data->skipped.insert(position).second) {
                ++data->skipped_count;
                ++newly_skipped;
            }
            continue;
        }
        double squared_distance = 0.0;
        if (!targetWithinReach(target, player_x, player_y, player_z, &squared_distance)) {
            saw_waiting_support = true;
            continue;
        }
        NativeBlockView actual;
        if (!reader.getBlockView(target.x, target.y, target.z, &actual) || !actual.name) {
            saw_waiting_support = true;
            continue;
        }
        // Stairs carry their facing and top/bottom state outside the ordinary
        // legacy aux field.  Resolve it before deciding whether an occupied
        // projection coordinate is already complete; otherwise every correct
        // stair would be mistaken for an incorrect occupied block and skipped.
        bool stair_state_unavailable = false;
        if (targetMatchesBlock(target, actual, isStairTarget(target.name),
                               &stair_state_unavailable)) {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled && data->projection_generation == observed_generation &&
                data->plan_identity == observed_plan && data->completed.insert(position).second) {
                ++data->completed_count;
                ++newly_completed;
            }
            continue;
        }
        if (stair_state_unavailable) {
            // The client block snapshot can briefly lag behind chunk data.
            // It is not evidence of a wrong stair, so keep the target pending
            // until its state is readable instead of permanently skipping it.
            saw_waiting_support = true;
            continue;
        }
        // Never replace or break an occupied but incorrect block.  Placement
        // is limited to a known empty air target; even normally replaceable
        // vegetation is left untouched rather than overwritten.
        const bool partial_double_slab = isPartialDoubleSlabTarget(target, actual);
        if ((!isEmptyPlacementTarget(*actual.name) && !partial_double_slab) ||
            isFluid(*actual.name)) {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled && data->projection_generation == observed_generation &&
                data->plan_identity == observed_plan && data->skipped.insert(position).second) {
                ++data->skipped_count;
                ++newly_skipped;
            }
            continue;
        }
        PlacementSupport support;
        const bool support_found = findTargetPlacementSupport(&reader, target, &support);
        if (!support_found) {
            saw_waiting_support = true;
            continue;
        }
        candidates.push_back(Candidate{target, position, support, squared_distance});
    }

    if (reachability_preview_enabled && reachability_preview_due) {
        std::vector<ProjectionReachabilityPreviewPosition> positions;
        positions.reserve(candidates.size());
        for (const Candidate& candidate : candidates) {
            positions.push_back({candidate.position.x, candidate.position.y,
                                 candidate.position.z});
        }
        BuildProjectionRenderer::instance().publishReachabilityPreview(
            query_result.plan_identity, std::move(positions));
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left,
                                                        const Candidate& right) {
        if (left.squared_distance != right.squared_distance) {
            return left.squared_distance < right.squared_distance;
        }
        if (left.position.y != right.position.y) return left.position.y < right.position.y;
        if (left.position.x != right.position.x) return left.position.x < right.position.x;
        return left.position.z < right.position.z;
    });

    // A yaw-directed block's server-facing yaw is synchronized by the existing
    // outgoing packet hook before its ItemUse. Keep this exact candidate pinned
    // across the two ticks: silently rotating for one target and then placing another
    // would be indistinguishable from an ordinary wrong placement.
    if (pending_silent_rotation) {
        const PendingSilentRotation pending_rotation = *pending_silent_rotation;
        const auto clear_pending_rotation = [&](const std::string& status) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                RuntimeState* const data = stateData();
                if (data->enabled && data->projection_generation == observed_generation &&
                    data->plan_identity == observed_plan && data->pending_silent_rotation &&
                    data->pending_silent_rotation->ticket == pending_rotation.ticket) {
                    data->pending_silent_rotation.reset();
                    data->next_dispatch = now + kTransientFailureRetry;
                    data->current_state = ProjectionPrinterState::WaitingSupport;
                    data->current_status = status;
                }
            }
            ProjectionPrinterSilentRotation::cancel(pending_rotation.ticket);
        };
        if (now - pending_rotation.armed_at >= kSilentRotationConfirmationTimeout) {
            clear_pending_rotation("未等到真实玩家输入包，已取消本次静默目标朝向同步");
            return;
        }
        if (!ProjectionPrinterSilentRotation::isSynchronized(pending_rotation.ticket)) {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled && data->projection_generation == observed_generation &&
                data->plan_identity == observed_plan && data->pending_silent_rotation &&
                data->pending_silent_rotation->ticket == pending_rotation.ticket) {
                data->current_state = ProjectionPrinterState::Running;
                data->current_status = "正在静默同步目标朝向，玩家视角不会移动";
            }
            return;
        }
        const auto selected = std::find_if(candidates.begin(), candidates.end(),
            [&](const Candidate& candidate) {
                return candidate.position == pending_rotation.position;
            });
        if (selected == candidates.end()) {
            clear_pending_rotation("目标方块已变化、超出范围或失去支撑，已取消本次放置");
            return;
        }
        Candidate pinned_candidate = *selected;
        candidates.clear();
        candidates.push_back(std::move(pinned_candidate));
    }

    if (candidates.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (data->enabled && data->projection_generation == observed_generation &&
            data->plan_identity == observed_plan) {
            data->next_dispatch = now + (query_result.truncated
                ? std::chrono::milliseconds(100) : kTransientFailureRetry);
            if (query_result.truncated) {
                // A cold spatial partition is being decoded off the game
                // thread (or this query reached its bounded target cap). It is
                // not evidence that the visible scope has finished.
                data->current_state = ProjectionPrinterState::Idle;
                data->current_status = "正在读取附近投影分区，暂不判定打印完成";
            // `completed`/`skipped` deliberately retain positions from this
            // whole projection so that a player who walks away and back does
            // not resend an interaction.  The display scope, however, moves
            // with range/layer settings and player position.  Never compare
            // those global counters with this scope-local raw total: that
            // would falsely report completion after changing layer/range.
            } else if (data->display_scope_total_known &&
                       data->display_scope_total == 0U) {
                data->current_state = ProjectionPrinterState::Complete;
                data->current_status = "当前显示范围内没有未完成的可打印普通方块";
            } else if (query_result.targets.empty()) {
                data->current_state = ProjectionPrinterState::Idle;
                data->current_status = "当前显示范围内附近没有投影方块，移动到目标附近后继续";
            } else if (saw_waiting_support) {
                data->current_state = ProjectionPrinterState::WaitingSupport;
                data->current_status = "附近投影目标缺少合法支撑面或不在交互距离内";
            } else if (newly_completed != 0U || newly_skipped != 0U) {
                data->current_state = ProjectionPrinterState::Running;
                data->current_status = "已刷新附近目标状态，继续搜索可放置方块";
            } else {
                data->current_state = ProjectionPrinterState::Idle;
                data->current_status = "当前可达目标均已处理，移动后会继续搜索显示范围";
            }
        }
        return;
    }

    std::optional<VerifiedHotbarOverride> verified_hotbar_override;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (data->enabled && data->projection_generation == observed_generation &&
            data->plan_identity == observed_plan && data->verified_hotbar_override) {
            if (now >= data->verified_hotbar_override->expires_at) {
                data->verified_hotbar_override.reset();
            } else {
                verified_hotbar_override = data->verified_hotbar_override;
            }
        }
    }

    bool verified_override_live = false;
    bool verified_override_selected = false;
    if (verified_hotbar_override) {
        ProjectionPrinterLiveInventorySnapshot live;
        std::string ignored_live_error;
        if (ReadProjectionPrinterLiveInventorySnapshot(&live, &ignored_live_error) && live.ready) {
            const int32_t slot = verified_hotbar_override->hotbar_slot;
            const bool slot_matches = slot >= 0 && slot <= 8 &&
                live.slots[static_cast<size_t>(slot)].occupied &&
                live.slots[static_cast<size_t>(slot)].has_network_stack_id &&
                live.slots[static_cast<size_t>(slot)].network_stack_id ==
                    verified_hotbar_override->network_stack_id;
            if (slot_matches) {
                verified_override_live = true;
                std::string ignored_selection_error;
                verified_override_selected = VerifyProjectionPrinterSelectedHotbarItem(
                    slot, verified_hotbar_override->network_stack_id,
                    &ignored_selection_error);
            } else {
                // Do not carry a response-derived material across any native
                // stack-ID change. A normal refreshed snapshot may still use
                // the new item, but this bridge is deliberately one-shot.
                std::lock_guard<std::mutex> lock(mutex_);
                RuntimeState* const data = stateData();
                if (data->verified_hotbar_override &&
                    data->verified_hotbar_override->hotbar_slot == slot &&
                    data->verified_hotbar_override->network_stack_id ==
                        verified_hotbar_override->network_stack_id) {
                    data->verified_hotbar_override.reset();
                }
                verified_hotbar_override.reset();
            }
        }
    }

    InventorySnapshot inventory;
    // The snapshot combines the read-only native hotbar identity with the
    // passive 36-slot mailbox. The latter is used only after a verified hotbar
    // miss and only through the guarded native inventory-session lifecycle.
    const bool inventory_available = readInventorySnapshot(&inventory, true);
    if (!inventory_available && !verified_override_live) {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (data->enabled && data->projection_generation == observed_generation &&
            data->plan_identity == observed_plan) {
            data->next_dispatch = now + kInventoryRetryDelay;
            data->current_state = ProjectionPrinterState::WaitingMaterial;
            data->current_status = "无法读取玩家物品栏，等待客户端物品组件就绪";
        }
        return;
    }

    bool waiting_for_submitted_inventory_source = false;
    if (inventory_available) {
        std::array<bool, kProjectionPrinterPlayerInventorySlotCount> blocked_sources{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (!data->enabled || data->projection_generation != observed_generation ||
                data->plan_identity != observed_plan) {
                return;
            }
            for (size_t slot = 9U; slot < data->inventory_move_retry_guards.size(); ++slot) {
                auto& guard = data->inventory_move_retry_guards[slot];
                if (!guard) continue;
                if (inventoryMoveRetrySourceChanged(*guard, inventory)) {
                    guard.reset();
                } else {
                    blocked_sources[slot] = true;
                }
            }
        }
        // Filter only submitted backpack sources. Hotbar choices and other
        // backpack stacks keep participating in the normal nearest-target scan.
        inventory.slots.erase(std::remove_if(inventory.slots.begin(), inventory.slots.end(),
            [&](const InventorySlot& material) {
                if (material.slot < 9 || material.slot > 35 ||
                    !blocked_sources[static_cast<size_t>(material.slot)]) {
                    return false;
                }
                waiting_for_submitted_inventory_source = true;
                return true;
            }), inventory.slots.end());
    }

    const Candidate* selected_candidate = nullptr;
    const InventorySlot* selected_material = nullptr;
    std::optional<InventorySlot> response_verified_material;
    const Candidate* backpack_candidate = nullptr;
    const InventorySlot* backpack_material = nullptr;
    int32_t selected_hotbar_slot = -1;
    bool material_in_backpack = false;
    bool switching_hotbar = false;
    bool using_verified_hotbar_override = false;
    for (const Candidate& candidate : candidates) {
        if (verified_override_live && verified_hotbar_override &&
            ProjectionBlockMaterialMatches(candidate.target.name, candidate.target.aux,
                                           verified_hotbar_override->name,
                                           verified_hotbar_override->aux)) {
            InventorySlot material;
            material.slot = verified_hotbar_override->hotbar_slot;
            material.name = verified_hotbar_override->name;
            material.aux = verified_hotbar_override->aux;
            material.count = static_cast<int32_t>(verified_hotbar_override->count);
            material.network_verified = true;
            material.network_stack_id = verified_hotbar_override->network_stack_id;
            response_verified_material = std::move(material);
            selected_candidate = &candidate;
            selected_material = &*response_verified_material;
            selected_hotbar_slot = verified_hotbar_override->hotbar_slot;
            switching_hotbar = !verified_override_selected;
            using_verified_hotbar_override = true;
            break;
        }
        if (!inventory_available) continue;
        const InventorySlot* material = nullptr;
        const MaterialSelection selection =
            selectMaterialForTarget(candidate.target, inventory, &material);
        if (selection == MaterialSelection::Ready) {
            selected_candidate = &candidate;
            selected_material = material;
            selected_hotbar_slot = material ? material->slot : -1;
            break;
        }
        if (selection == MaterialSelection::SwitchingHotbar) {
            selected_candidate = &candidate;
            selected_material = material;
            selected_hotbar_slot = material ? material->slot : -1;
            switching_hotbar = true;
            break;
        }
        if (selection == MaterialSelection::InBackpack) {
            material_in_backpack = true;
            // Candidates are nearest-first.  If the first one is still only
            // API-visible, prefer a later candidate whose exact native stack
            // identity has already been verified by the selected snapshot.
            if (!backpack_candidate ||
                (material && material->network_verified &&
                 (!backpack_material || !backpack_material->network_verified))) {
                backpack_candidate = &candidate;
                backpack_material = material;
            }
        }
    }

    if (!selected_candidate && backpack_candidate && backpack_material) {
        int32_t destination_hotbar_slot = -1;
        const bool source_verified = backpack_material->network_verified &&
            backpack_material->slot >= 9 && backpack_material->slot <= 35;
        const bool destination_found = source_verified &&
            chooseBackpackMoveDestination(inventory, &destination_hotbar_slot);
        if (!source_verified || !destination_found) {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled && data->projection_generation == observed_generation &&
                data->plan_identity == observed_plan) {
                data->next_inventory_attempt = now + kInventoryRetryDelay;
                data->next_dispatch = data->next_inventory_attempt;
                data->current_state = ProjectionPrinterState::WaitingMaterial;
                data->current_status = inventory.network_ready
                    ? "材料或可用热栏槽位已变化，等待下一次安全库存检查"
                    : "等待游戏当前物品栏就绪后自动换入热栏";
            }
            return;
        }
        const NetworkInventorySlot& destination_network = inventory.network_slots[
            static_cast<size_t>(destination_hotbar_slot)];
        const InventorySlot* const destination_material = findInventorySlot(
            inventory, destination_hotbar_slot);
        if (destination_network.occupied && !destination_material) {
            std::lock_guard<std::mutex> lock(mutex_);
            RuntimeState* const data = stateData();
            if (data->enabled && data->projection_generation == observed_generation &&
                data->plan_identity == observed_plan) {
                data->next_inventory_attempt = now + kInventoryRetryDelay;
                data->next_dispatch = data->next_inventory_attempt;
                data->current_state = ProjectionPrinterState::WaitingMaterial;
                data->current_status = "热栏目标槽位状态无法安全确认，等待下一次库存检查";
            }
            return;
        }
        ProjectionPrinterInventoryMoveRequest move_request;
        move_request.source_inventory_slot = backpack_material->slot;
        move_request.destination_hotbar_slot = destination_hotbar_slot;
        move_request.expected_source_network_stack_id = backpack_material->network_stack_id;
        move_request.expected_destination_occupied = destination_network.occupied;
        move_request.expected_destination_network_stack_id =
            destination_network.network_stack_id;
        move_request.expected_source_count = static_cast<uint16_t>(backpack_material->count);

        std::string move_error;
        // This is a second effect boundary.  It shares the ItemUse mutex so a
        // projection clear, disable or authorization revocation cannot return
        // while a late native ItemStackRequest is about to be submitted.
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (!data->enabled || data->projection_generation != observed_generation ||
            data->plan_identity != observed_plan || data->pending ||
            data->pending_inventory_move || now < data->next_inventory_attempt) {
            return;
        }
        const auto& existing_source_guard = data->inventory_move_retry_guards[
            static_cast<size_t>(move_request.source_inventory_slot)];
        if (existing_source_guard) {
            data->next_inventory_attempt = now + kInventoryRetryDelay;
            data->next_dispatch = data->next_inventory_attempt;
            data->current_state = ProjectionPrinterState::WaitingMaterial;
            data->current_status = "该背包材料已提交交换，等待槽位重新同步，未重复提交";
            return;
        }
        uint64_t inventory_session_token = data->active_inventory_session_token;
        if (inventory_session_token != 0U &&
            data->inventory_session_close_token == inventory_session_token) {
            // A new move can reuse the still-open silent player inventory.
            // It supersedes the short post-response close grace period.
            data->inventory_session_close_token = 0U;
            data->inventory_session_close_ticks_remaining = 0U;
        }
        if (inventory_session_token == 0U) {
            // The incoming printer-owned ContainerOpen is observed normally;
            // only its UI presentation handler is cancelled. Require that hook
            // before sending OpenInventory so the request cannot pop a screen.
            if (!IsProjectionPrinterInventoryPresentationGateInstalled() ||
                !IsProjectionPrinterInventoryPresentationGateReadyForOpen()) {
                data->next_inventory_attempt = now + kInventoryRetryDelay;
                data->next_dispatch = data->next_inventory_attempt;
                data->current_state = ProjectionPrinterState::WaitingMaterial;
                data->current_status = IsProjectionPrinterInventoryPresentationGateInstalled()
                    ? "等待上一轮背包会话结束"
                    : "背包界面拦截未就绪，暂不发起材料交换";
                return;
            }
            uint64_t new_token = data->next_inventory_session_token++;
            if (new_token == 0U) new_token = data->next_inventory_session_token++;
            if (new_token == 0U || !ArmProjectionPrinterInventorySession(new_token)) {
                data->next_inventory_attempt = now + kInventoryRetryDelay;
                data->next_dispatch = data->next_inventory_attempt;
                data->current_state = ProjectionPrinterState::WaitingMaterial;
                data->current_status = "等待上一轮静默背包会话完全结束";
                return;
            }
            const uint64_t presentation_generation_before_open =
                GetProjectionPrinterInventoryPresentationGateArmGeneration();
            std::string open_error;
            if (!PlayerInventoryOpenPacketSender::send(&open_error)) {
                CancelProjectionPrinterInventorySession(new_token);
                ClearProjectionPrinterInventoryPresentationGate();
                data->next_inventory_attempt = now + kInventoryMoveFailureDelay;
                data->next_dispatch = data->next_inventory_attempt;
                data->current_state = ProjectionPrinterState::WaitingMaterial;
                data->current_status = scopedStatus(open_error.empty()
                    ? "无法静默打开玩家背包以交换材料"
                    : "无法静默打开玩家背包：" + open_error);
                return;
            }
            data->active_inventory_session_token = new_token;
            data->active_inventory_session_content_ready = false;
            data->inventory_session_presentation_generation_before_open =
                presentation_generation_before_open;
            data->active_inventory_session_presentation_generation = 0U;
            data->inventory_session_opened_at = now;
            data->next_inventory_attempt = now + std::chrono::milliseconds(100);
            data->next_dispatch = data->next_inventory_attempt;
            data->current_state = ProjectionPrinterState::WaitingMaterial;
            data->current_status = "已静默请求玩家背包，等待服务器同步材料";
            return;
        }

        ProjectionPrinterInventorySessionResult session_result;
        const ProjectionPrinterInventorySessionPollState session_state =
            PollProjectionPrinterInventorySession(inventory_session_token, &session_result);
        const bool opened_without_content_refresh =
            session_state == ProjectionPrinterInventorySessionPollState::WaitingForContent;
        if (session_state != ProjectionPrinterInventorySessionPollState::Ready &&
            !opened_without_content_refresh) {
            const bool timed_out = data->inventory_session_opened_at !=
                    std::chrono::steady_clock::time_point{} &&
                now - data->inventory_session_opened_at >= kInventoryOpenConfirmationTimeout;
            if (timed_out || session_state == ProjectionPrinterInventorySessionPollState::Failed ||
                session_state == ProjectionPrinterInventorySessionPollState::Closed ||
                session_state == ProjectionPrinterInventorySessionPollState::Inactive) {
                data->active_inventory_session_token = 0U;
                data->active_inventory_session_content_ready = false;
                data->inventory_session_presentation_generation_before_open = 0U;
                data->active_inventory_session_presentation_generation = 0U;
                data->inventory_session_opened_at = {};
                data->inventory_session_close_token = 0U;
                data->inventory_session_close_ticks_remaining = 0U;
                data->next_inventory_attempt = now + kInventoryMoveFailureDelay;
                data->next_dispatch = data->next_inventory_attempt;
                data->current_state = ProjectionPrinterState::WaitingMaterial;
                if (timed_out) {
                    data->current_status = "静默玩家背包未在限定时间内完成同步；稍后重试";
                } else if (!session_result.error.empty()) {
                    data->current_status = scopedStatus(
                        "静默玩家背包会话失败：" + session_result.error);
                } else {
                    data->current_status = "静默玩家背包会话已关闭；稍后重试";
                }
                finishProjectionPrinterInventorySession(inventory_session_token);
                return;
            }
            data->next_inventory_attempt = now + std::chrono::milliseconds(100);
            data->next_dispatch = data->next_inventory_attempt;
            data->current_state = ProjectionPrinterState::WaitingMaterial;
            data->current_status = session_state ==
                    ProjectionPrinterInventorySessionPollState::WaitingForOpen
                ? "等待服务器确认静默打开玩家背包"
                : "等待玩家背包材料同步到客户端";
            return;
        }
        if (!data->active_inventory_session_content_ready) {
            uint64_t completed_presentation_generation = 0U;
            if (!GetProjectionPrinterInventoryPresentationGateCompletedGenerationAfter(
                    data->inventory_session_presentation_generation_before_open,
                    &completed_presentation_generation)) {
                const bool presentation_timed_out = data->inventory_session_opened_at !=
                        std::chrono::steady_clock::time_point{} &&
                    now - data->inventory_session_opened_at >=
                        kInventoryOpenConfirmationTimeout;
                if (presentation_timed_out) {
                    data->active_inventory_session_token = 0U;
                    data->active_inventory_session_content_ready = false;
                    data->inventory_session_presentation_generation_before_open = 0U;
                    data->active_inventory_session_presentation_generation = 0U;
                    data->inventory_session_opened_at = {};
                    data->inventory_session_close_token = 0U;
                    data->inventory_session_close_ticks_remaining = 0U;
                    data->next_inventory_attempt = now + kInventoryMoveFailureDelay;
                    data->next_dispatch = data->next_inventory_attempt;
                    data->current_state = ProjectionPrinterState::WaitingMaterial;
                    data->current_status =
                        "未确认背包界面已被拦截，本次材料交换已取消；稍后重试";
                    finishProjectionPrinterInventorySession(inventory_session_token);
                    return;
                }
                data->next_inventory_attempt = now + std::chrono::milliseconds(50);
                data->next_dispatch = data->next_inventory_attempt;
                data->current_state = ProjectionPrinterState::WaitingMaterial;
                data->current_status = "等待背包界面拦截完成";
                return;
            }
            data->active_inventory_session_presentation_generation =
                completed_presentation_generation;
            // If content arrived, the receive hook has only just returned it to
            // the game. If it did not arrive, the server has still acknowledged
            // the player-inventory open and the native client retains a live
            // verified inventory snapshot. In both cases, leave one small tick
            // interval before ScopeBegin reads the container.
            data->active_inventory_session_content_ready = true;
            data->next_inventory_attempt = now + std::chrono::milliseconds(50);
            data->next_dispatch = data->next_inventory_attempt;
            data->current_state = ProjectionPrinterState::WaitingMaterial;
            data->current_status = opened_without_content_refresh
                ? "服务器未补发背包内容，正在使用当前材料快照交换"
                : "玩家背包已同步，正在准备原生材料交换";
            return;
        }
        if (data->active_inventory_session_presentation_generation == 0U ||
            !HasProjectionPrinterInventoryPresentationGateCompleted(
                data->active_inventory_session_presentation_generation)) {
            data->active_inventory_session_token = 0U;
            data->active_inventory_session_content_ready = false;
            data->inventory_session_presentation_generation_before_open = 0U;
            data->active_inventory_session_presentation_generation = 0U;
            data->inventory_session_opened_at = {};
            data->inventory_session_close_token = 0U;
            data->inventory_session_close_ticks_remaining = 0U;
            data->next_inventory_attempt = now + kInventoryMoveFailureDelay;
            data->next_dispatch = data->next_inventory_attempt;
            data->current_state = ProjectionPrinterState::WaitingMaterial;
            data->current_status = "背包静默会话已失效，未提交材料交换；稍后重试";
            finishProjectionPrinterInventorySession(inventory_session_token);
            return;
        }
        // The reference sorter builds updates from its live Supplies stacks.
        // A late injection may never see login's full InventoryContent, and
        // opening player inventory does not require the server to resend it.
        // Serialize the current native stacks. Prepare before Move/Swap and commit
        // source->destination to the receive FIFO in the very same game tick.
        // Do not fall back to an old wire snapshot after live validation fails:
        // a stack can retain its net ID while its count or NBT has changed.
        ProjectionPrinterInventoryClientSyncPreparedMove immediate_client_sync_prepared;
        std::string immediate_client_sync_error;
        const bool client_sync_prepared = PrepareProjectionPrinterLiveInventoryClientSyncMove(
            move_request, &immediate_client_sync_prepared, &immediate_client_sync_error);
        if (!client_sync_prepared) {
            data->active_inventory_session_token = 0U;
            data->active_inventory_session_content_ready = false;
            data->inventory_session_presentation_generation_before_open = 0U;
            data->active_inventory_session_presentation_generation = 0U;
            data->inventory_session_opened_at = {};
            data->inventory_session_close_token = 0U;
            data->inventory_session_close_ticks_remaining = 0U;
            data->next_inventory_attempt = now + kInventoryMoveFailureDelay;
            data->next_dispatch = data->next_inventory_attempt;
            data->current_state = ProjectionPrinterState::WaitingMaterial;
            data->current_status = scopedStatus(immediate_client_sync_error.empty()
                ? "无法读取当前背包物品以刷新快捷栏，本次交换已取消"
                : "快捷栏刷新准备失败：" + immediate_client_sync_error);
            finishProjectionPrinterInventorySession(inventory_session_token);
            return;
        }
        const uint64_t response_session_generation =
            GetProjectionPrinterInventoryMailboxSessionGeneration();
        ProjectionPrinterInventoryResponse response_before_move;
        const uint64_t response_generation_before_move =
            GetProjectionPrinterInventoryResponse(&response_before_move) &&
                response_session_generation != 0U &&
                response_before_move.session_generation == response_session_generation
            ? response_before_move.response_generation : 0U;
        int32_t submitted_request_id = 0;
        const bool moved = MoveProjectionPrinterBackpackItemToHotbar(
            move_request, &move_error, &submitted_request_id);
        if (!moved) {
            DiscardProjectionPrinterInventoryClientSyncPreparedMove(
                &immediate_client_sync_prepared);
            data->next_inventory_attempt = now + kInventoryRetryDelay;
            data->next_dispatch = data->next_inventory_attempt;
            data->current_state = ProjectionPrinterState::WaitingMaterial;
            data->current_status = scopedStatus(move_error.empty()
                ? "材料无法安全换入热栏" : move_error);
            return;
        }
        // The verified native allocator emits signed negative request IDs for
        // this client build.  Zero therefore cannot identify a newly submitted
        // ItemStackRequest.  Do not create an ID=0 pending state: a later
        // unrelated response with that sentinel value could otherwise be
        // mistaken for this move. The sent request is left to the game's normal
        // session cleanup, while the printer fails closed and re-reads material
        // before trying again.
        if (submitted_request_id == 0) {
            DiscardProjectionPrinterInventoryClientSyncPreparedMove(
                &immediate_client_sync_prepared);
            CancelProjectionPrinterInventorySession(inventory_session_token);
            ClearProjectionPrinterInventoryPresentationGate();
            if (data->active_inventory_session_token == inventory_session_token) {
                data->active_inventory_session_token = 0U;
                data->active_inventory_session_content_ready = false;
                data->inventory_session_presentation_generation_before_open = 0U;
                data->active_inventory_session_presentation_generation = 0U;
                data->inventory_session_opened_at = {};
                data->inventory_session_close_token = 0U;
                data->inventory_session_close_ticks_remaining = 0U;
            }
            data->next_inventory_attempt = now + kInventoryMoveFailureDelay;
            data->next_dispatch = data->next_inventory_attempt;
            data->current_state = ProjectionPrinterState::WaitingMaterial;
            data->current_status =
                "原生材料交换请求编号无效，已停止本轮并等待库存重新同步";
            return;
        }
        uint64_t client_slot_refresh_ticket = 0U;
        const bool client_slot_refresh_queued =
            CommitProjectionPrinterInventoryClientSyncPreparedMove(
                &immediate_client_sync_prepared, &client_slot_refresh_ticket,
                &immediate_client_sync_error);
        LogProjectionPrinterInventoryDiagnostic(
            "local_queue request=%d queued=%d ticket=%llu source=%d destination=%d "
            "source_net=%d destination_net=%d source_count=%u destination_count=%u "
            "session=%llu response_generation_before=%llu error=%s",
            submitted_request_id, client_slot_refresh_queued ? 1 : 0,
            static_cast<unsigned long long>(client_slot_refresh_ticket),
            move_request.source_inventory_slot, move_request.destination_hotbar_slot,
            move_request.expected_source_network_stack_id,
            move_request.expected_destination_network_stack_id,
            static_cast<unsigned>(move_request.expected_source_count),
            static_cast<unsigned>(destination_material ? destination_material->count : destination_network.count),
            static_cast<unsigned long long>(response_session_generation),
            static_cast<unsigned long long>(response_generation_before_move),
            client_slot_refresh_queued ? "none" : immediate_client_sync_error.c_str());
        PendingInventoryMove pending_move;
        pending_move.inventory_session_token = inventory_session_token;
        pending_move.source_inventory_slot = backpack_material->slot;
        pending_move.destination_hotbar_slot = destination_hotbar_slot;
        pending_move.expected_name = backpack_material->name;
        pending_move.expected_aux = backpack_material->aux;
        pending_move.expected_source_count = static_cast<uint16_t>(backpack_material->count);
        pending_move.expected_source_runtime_item_id = backpack_material->runtime_item_id;
        pending_move.expected_source_network_stack_id = backpack_material->network_stack_id;
        pending_move.expected_destination_occupied = destination_network.occupied;
        pending_move.expected_destination_network_stack_id =
            destination_network.network_stack_id;
        pending_move.expected_destination_name = destination_material
            ? destination_material->name : std::string{};
        pending_move.expected_destination_runtime_item_id = destination_network.runtime_item_id;
        pending_move.expected_destination_aux = destination_material
            ? destination_material->aux : destination_network.aux;
        pending_move.expected_destination_count = static_cast<uint16_t>(
            destination_material ? destination_material->count : destination_network.count);
        pending_move.mailbox_session_generation = response_session_generation;
        pending_move.response_session_generation = response_session_generation;
        pending_move.mailbox_revision_before_send = inventory.mailbox_revision;
        pending_move.client_slot_refresh_queued = client_slot_refresh_queued;
        pending_move.client_slot_refresh_ticket = client_slot_refresh_ticket;
        pending_move.response_generation_before_send = response_generation_before_move;
        pending_move.request_id = submitted_request_id;
        pending_move.verification_source = inventory.network_source;
        pending_move.sent_at = now;
        InventoryMoveRetryGuard retry_guard;
        retry_guard.submitted = pending_move;
        retry_guard.remote_source_known = inventory.mailbox_ready;
        if (retry_guard.remote_source_known) {
            retry_guard.remote_source_before_send = inventory.remote_inventory.slots[
                static_cast<size_t>(pending_move.source_inventory_slot)];
        }
        data->inventory_move_retry_guards[
            static_cast<size_t>(pending_move.source_inventory_slot)] = std::move(retry_guard);
        data->pending_inventory_move = std::move(pending_move);
        data->next_inventory_attempt = now + std::chrono::milliseconds(100);
        data->next_dispatch = data->next_inventory_attempt;
        data->current_state = ProjectionPrinterState::Running;
        if (client_slot_refresh_queued) {
            data->current_status = inventory.network_source == InventoryVerificationSource::LiveNative
                ? "已提交材料交换，正在更新快捷栏显示"
                : "已提交材料交换，正在更新快捷栏显示并等待库存确认";
        } else {
            data->current_status = scopedStatus(immediate_client_sync_error.empty()
                ? "已提交材料交换，等待游戏当前物品栏确认"
                : "已提交材料交换，但快捷栏本地刷新未入队：" +
                    immediate_client_sync_error);
        }
        return;
    }

    if (!selected_candidate) {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (data->enabled && data->projection_generation == observed_generation &&
            data->plan_identity == observed_plan) {
            if (pending_silent_rotation && data->pending_silent_rotation &&
                data->pending_silent_rotation->ticket == pending_silent_rotation->ticket) {
                const ProjectionPrinterSilentRotation::Ticket ticket =
                    data->pending_silent_rotation->ticket;
                data->pending_silent_rotation.reset();
                ProjectionPrinterSilentRotation::cancel(ticket);
            }
            data->next_dispatch = now + kInventoryRetryDelay;
            data->current_state = ProjectionPrinterState::WaitingMaterial;
            data->current_status = waiting_for_submitted_inventory_source
                ? "已提交的背包材料尚未同步，等待槽位变化；仍会使用其他可用热栏材料"
                : material_in_backpack
                ? "材料在背包中，等待安全换入热栏"
                : "当前范围没有可用材料，继续保留其他投影目标";
        }
        return;
    }

    if (switching_hotbar) {
        const bool requested = requestHotbarSelection(selected_hotbar_slot);
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeState* const data = stateData();
        if (data->enabled && data->projection_generation == observed_generation &&
            data->plan_identity == observed_plan) {
            if (pending_silent_rotation && data->pending_silent_rotation &&
                data->pending_silent_rotation->ticket == pending_silent_rotation->ticket) {
                const ProjectionPrinterSilentRotation::Ticket ticket =
                    data->pending_silent_rotation->ticket;
                data->pending_silent_rotation.reset();
                ProjectionPrinterSilentRotation::cancel(ticket);
            }
            data->next_dispatch = now + kInventoryRetryDelay;
            data->current_state = requested ? ProjectionPrinterState::Running
                                            : ProjectionPrinterState::WaitingMaterial;
            data->current_status = requested ? "正在切换所需热栏材料"
                                             : "无法切换到所需热栏材料";
        }
        return;
    }

    const bool selected_requires_silent_yaw =
        selected_candidate->support.requires_silent_yaw;
    ProjectionPrinterSilentRotation::Ticket synchronized_rotation_ticket = 0U;
    if (selected_requires_silent_yaw) {
        if (pending_silent_rotation) {
            if (!(pending_silent_rotation->position == selected_candidate->position) ||
                !ProjectionPrinterSilentRotation::isSynchronized(
                    pending_silent_rotation->ticket)) {
                std::lock_guard<std::mutex> lock(mutex_);
                RuntimeState* const data = stateData();
                if (data->enabled && data->projection_generation == observed_generation &&
                    data->plan_identity == observed_plan) {
                    data->current_state = ProjectionPrinterState::Running;
                        data->current_status = "等待目标方块的静默朝向同步完成";
                }
                return;
            }
            synchronized_rotation_ticket = pending_silent_rotation->ticket;
        } else {
            std::string rotation_error;
            ProjectionPrinterSilentRotation::Ticket ticket = 0U;
            if (!ProjectionPrinterSilentRotation::armYaw(
                    selected_candidate->support.silent_yaw, &ticket, &rotation_error)) {
                std::lock_guard<std::mutex> lock(mutex_);
                RuntimeState* const data = stateData();
                if (data->enabled && data->projection_generation == observed_generation &&
                    data->plan_identity == observed_plan) {
                    data->next_dispatch = now + kTransientFailureRetry;
                    data->current_state = ProjectionPrinterState::Error;
                        data->current_status = scopedStatus(rotation_error.empty()
                            ? "无法准备目标方块的静默朝向" : rotation_error);
                }
                return;
            }
            bool retained = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                RuntimeState* const data = stateData();
                if (data->enabled && data->projection_generation == observed_generation &&
                    data->plan_identity == observed_plan && !data->pending &&
                    !data->pending_inventory_move && !data->pending_silent_rotation) {
                    data->pending_silent_rotation = PendingSilentRotation{
                        selected_candidate->position, ticket, now};
                    data->current_state = ProjectionPrinterState::Running;
                    data->current_status = "已准备目标方块朝向，等待真实输入包静默同步";
                    retained = true;
                }
            }
            if (!retained) ProjectionPrinterSilentRotation::cancel(ticket);
            return;
        }
    }

    std::string packet_error;
    // This is the effect boundary.  `clear`, disable and authorization
    // revocation take the same mutex, so once any of them returns it is
    // impossible for an already selected candidate to issue a late ItemUse.
    // Keep the critical section short: all world, projection and Python work
    // above happened without this lock.
    std::lock_guard<std::mutex> lock(mutex_);
    RuntimeState* const data = stateData();
    if (!data->enabled || data->projection_generation != observed_generation ||
        data->plan_identity != observed_plan || data->pending ||
        data->pending_inventory_move) {
        return;
    }
    const bool sent = selected_candidate->support.has_click_override
        ? ContainerOpenPacketSender::sendWithClick(
              selected_candidate->support.x, selected_candidate->support.y,
              selected_candidate->support.z, selected_candidate->support.native_block,
              selected_candidate->support.face, selected_candidate->support.click_override,
              &packet_error, selected_hotbar_slot,
              selected_material && selected_material->network_verified,
              selected_material ? selected_material->network_stack_id : 0)
        : ContainerOpenPacketSender::send(
              selected_candidate->support.x, selected_candidate->support.y,
              selected_candidate->support.z, selected_candidate->support.native_block,
              selected_candidate->support.face, &packet_error, selected_hotbar_slot,
              selected_material && selected_material->network_verified,
              selected_material ? selected_material->network_stack_id : 0);
    if (!sent) {
        if (synchronized_rotation_ticket != 0U && data->pending_silent_rotation &&
            data->pending_silent_rotation->ticket == synchronized_rotation_ticket) {
            data->pending_silent_rotation.reset();
            ProjectionPrinterSilentRotation::cancel(synchronized_rotation_ticket);
        }
        data->next_dispatch = now + kTransientFailureRetry;
        data->current_state = isTransientPacketError(packet_error)
            ? ProjectionPrinterState::WaitingSupport : ProjectionPrinterState::Error;
        data->current_status = scopedStatus(packet_error.empty()
            ? "放置数据包未发送" : packet_error);
        return;
    }
    if (using_verified_hotbar_override && data->verified_hotbar_override &&
        data->verified_hotbar_override->hotbar_slot == selected_hotbar_slot &&
        selected_material && data->verified_hotbar_override->network_stack_id ==
            selected_material->network_stack_id) {
        // The native sender has just revalidated this exact selected ItemStack
        // and emitted the first automatic ItemUse. Do not carry the bridge
        // across a later stack-count/network-ID update.
        data->verified_hotbar_override.reset();
    }
    data->pending = PendingPlacement{selected_candidate->target,
                                     selected_candidate->position, now,
                                     selected_hotbar_slot};
    if (synchronized_rotation_ticket != 0U && data->pending_silent_rotation &&
        data->pending_silent_rotation->ticket == synchronized_rotation_ticket) {
        data->pending_silent_rotation.reset();
        ProjectionPrinterSilentRotation::finish(synchronized_rotation_ticket);
    }
    data->next_dispatch = now + std::chrono::milliseconds(
        std::max<int64_t>(1, 1000LL / std::max<int32_t>(kMinimumPrinterRate,
                                                         configured_rate)));
    data->current_state = ProjectionPrinterState::Running;
    data->current_status = "已发送合法放置请求，等待世界确认";
}

}  // namespace build_import
