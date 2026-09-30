#include "MapAnvilDebugBridge.h"

#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE

#include "MapAnvilCreatedOutputProbe.h"
#include "MapAnvilNativeActionTrace.h"
#include "MapAnvilRecipeProbe.h"
#include "MapAnvilWireDiagnostic.h"
#include "MapManualOutboundTrace.h"
#include "MapInventoryTransfer.h"
#include "NativeWorldAccess.h"
#include "ProjectionPrinterInventoryMailbox.h"
#include "../main.h"

#include <android/log.h>

#include <array>
#include <chrono>
#include <mutex>
#include <string>

namespace build_import {
namespace {

constexpr const char* kTag = "Infinitecz_MapAnvilProbe";
constexpr uint64_t kTicket = 1U;
// The old map-items getter never ran during two confirmed anvil renames.
// Keep its instrumentation available for targeted research, but do not arm
// this disproven candidate during ordinary Debug manual traces.
constexpr bool kEnableLegacyMapItemsGetterProbe = false;
constexpr size_t kMaximumRenameRequests = 4U;
constexpr uint8_t kMaximumNativeSnapshotAttempts = 8U;
constexpr auto kSnapshotInterval = std::chrono::milliseconds(500);

struct TrackedRequest {
    int32_t request_id = 0;
    uint64_t session_generation = 0;
    uint64_t response_generation = 0;
    bool response_logged = false;
};

struct BridgeState {
    bool initialized = false;
    bool opened = false;
    bool closed = false;
    bool invalidated = false;
    bool block_checked = false;
    bool block_is_anvil = false;
    bool uuid_seen = false;
    bool uuid_ambiguous = false;
    bool probe_attempted = false;
    bool probe_armed = false;
    bool probe_result_logged = false;
    bool created_probe_attempted = false;
    bool created_probe_armed = false;
    bool created_match_logged = false;
    bool created_variant_seen = false;
    bool native_action_attempted = false;
    bool native_action_armed = false;
    bool native_result_logged = false;
    bool unbound_fallback_consumed = false;
    bool unbound_fallback_pending = false;
    uint8_t native_recipe_logged_mask = 0;
    bool manual_input_seen = false;
    bool provisional_identity = false;
    uint8_t window_id = 0;
    uint8_t container_type = 0;
    uint8_t snapshot_attempts = 0;
    int32_t manual_input_request_id = 0;
    int32_t manual_input_network_id = 0;
    int32_t unbound_input_request_id = 0;
    int32_t unbound_input_network_id = 0;
    int32_t provisional_network_id = 0;
    int32_t created_variant_request_id = 0;
    int32_t created_variant_network_id = 0;
    int32_t created_variant_secondary_id = 0;
    int32_t created_variant_tag = 0;
    uint32_t created_logged_count = 0;
    uint32_t created_logged_mask = 0;
    uint32_t created_logged_callbacks = 0;
    uint32_t created_logged_unreadable = 0;
    int32_t x = 0, y = 0, z = 0;
    int64_t map_uuid = -1;
    MapAnvilMapIdentityIndex identity_index;
    std::string open_packet;
    std::chrono::steady_clock::time_point deadline{};
    std::chrono::steady_clock::time_point next_snapshot{};
    std::array<TrackedRequest, kMaximumRenameRequests> requests{};
    size_t request_count = 0;
};

std::mutex g_mutex;
BridgeState g_bridge;
MapAnvilWireDiagnostic g_wire;

uint32_t packetId(std::string_view packet) noexcept {
    uint32_t value = 0;
    for (size_t index = 0; index < 5U && index < packet.size(); ++index) {
        const uint8_t next = static_cast<uint8_t>(packet[index]);
        if (index == 4U && (next & 0xF0U) != 0U) return UINT32_MAX;
        value |= static_cast<uint32_t>(next & 0x7FU) << (index * 7U);
        if ((next & 0x80U) == 0U) return value & 0x3FFU;
    }
    return UINT32_MAX;
}

bool isAnvilName(const std::string& name) noexcept {
    return name == "anvil" || name == "minecraft:anvil" ||
           name == "chipped_anvil" || name == "minecraft:chipped_anvil" ||
           name == "damaged_anvil" || name == "minecraft:damaged_anvil";
}

unsigned long long verifiedGameCallerRva(uintptr_t caller_pc) noexcept {
    const uintptr_t base = Main::getBaseAddress();
    if (!base || caller_pc < base || caller_pc - base < 4U) return 0ULL;
    uintptr_t verified_instruction = 0;
    return ResolveMinecraftExecutableOffset(
               base, caller_pc - base - 4U, 4U, &verified_instruction) &&
               verified_instruction == caller_pc - 4U
               ? static_cast<unsigned long long>(caller_pc - base)
               : 0ULL;
}

void logMapItem(const MapAnvilDiagnosticMapItem& item,
                uint32_t inventory_id) noexcept {
    // No custom-name text or item-extra bytes are emitted to logcat.
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "item inventory=%u slot=%u runtime=%d count=%u net_present=%d net=%d "
        "uuid=%lld name_status=%u name_source=%u name_bytes=%zu",
        inventory_id, static_cast<unsigned>(item.slot), item.runtime_item_id,
        static_cast<unsigned>(item.count), item.has_network_stack_id ? 1 : 0,
        item.network_stack_id, static_cast<long long>(item.map_uuid),
        static_cast<unsigned>(item.name_status),
        static_cast<unsigned>(item.name_source), item.name.size());
}

void logProbeCountersLocked(const char* phase) noexcept {
    if (!g_bridge.probe_armed) return;
    MapAnvilRecipeProbeDiagnostics counters;
    if (!ReadMapAnvilRecipeProbeDiagnostics(kTicket, &counters)) return;
    // Only counters are emitted: no item NBT, stack bytes, or user-entered
    // name. Getter/copy callbacks never log or perform I/O themselves.
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "recipe_probe_counters phase=%s getter=%u guard=%u "
        "uuid_read=%u,%u uuid_match=%u,%u correlated=%u "
        "copy=%u no_getter=%u storage_mismatch=%u unreadable=%u observed=%u",
        phase, static_cast<unsigned>(counters.getter_entries),
        static_cast<unsigned>(counters.getter_guard_rejected),
        static_cast<unsigned>(counters.first_uuid_read),
        static_cast<unsigned>(counters.second_uuid_read),
        static_cast<unsigned>(counters.first_uuid_match),
        static_cast<unsigned>(counters.second_uuid_match),
        static_cast<unsigned>(counters.getter_correlated),
        static_cast<unsigned>(counters.copy_entries),
        static_cast<unsigned>(counters.copy_without_getter),
        static_cast<unsigned>(counters.copy_storage_mismatch),
        static_cast<unsigned>(counters.copy_network_id_unreadable),
        static_cast<unsigned>(counters.copy_observed));
}

void logCreatedProbeLocked(const char* phase) noexcept {
    if (!g_bridge.created_probe_armed) return;
    MapAnvilCreatedOutputSnapshot snapshot;
    if (!ReadMapAnvilCreatedOutputProbe(kTicket, &snapshot)) return;
    const uint32_t new_record_bits = MapAnvilCreatedOutputNewRecordBits(
        snapshot, g_bridge.created_logged_mask);
    const bool unchanged_tick = std::string_view(phase) == "game_tick" &&
        snapshot.callback_count == g_bridge.created_logged_callbacks &&
        snapshot.unreadable_count == g_bridge.created_logged_unreadable &&
        snapshot.captured_count == g_bridge.created_logged_count &&
        new_record_bits == 0U;
    if (!unchanged_tick) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "created_output_probe phase=%s generic_attempts=%u anvil_attempts=%u "
            "generic_header_hits=%u anvil_header_hits=%u "
            "generic_first=%d:%u:%u:%u anvil_first=%d:%u:%u:%u "
            "callbacks=%u unreadable=%u captured=%u "
            "manual_variant=%d source_confirmed=%d",
            phase, snapshot.generic_attempts, snapshot.anvil_attempts,
            snapshot.generic_header_hits, snapshot.anvil_header_hits,
            snapshot.generic_first_header.seen ? 1 : 0,
            snapshot.generic_first_header.kind,
            static_cast<unsigned>(snapshot.generic_first_header.container_name),
            static_cast<unsigned>(snapshot.generic_first_header.slot),
            snapshot.anvil_first_header.seen ? 1 : 0,
            snapshot.anvil_first_header.kind,
            static_cast<unsigned>(snapshot.anvil_first_header.container_name),
            static_cast<unsigned>(snapshot.anvil_first_header.slot),
            snapshot.callback_count, snapshot.unreadable_count,
            snapshot.captured_count, g_bridge.created_variant_seen ? 1 : 0,
            g_bridge.manual_input_seen && g_bridge.uuid_seen &&
                !g_bridge.provisional_identity && !g_bridge.uuid_ambiguous ? 1 : 0);
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "created_output_entry phase=%s anvil_sim=%d:%u:0x%llx "
            "anvil_type2=%d:%u:0x%llx craft_ctor=%d:%u:0x%llx recorded=%u",
            phase,
            snapshot.anvil_simulation_entry.installed ? 1 : 0,
            snapshot.anvil_simulation_entry.calls,
            verifiedGameCallerRva(
                snapshot.anvil_simulation_entry.first_caller_pc),
            snapshot.anvil_type2_entry.installed ? 1 : 0,
            snapshot.anvil_type2_entry.calls,
            verifiedGameCallerRva(
                snapshot.anvil_type2_entry.first_caller_pc),
            snapshot.craft_optional_constructor.installed ? 1 : 0,
            snapshot.craft_optional_constructor.calls,
            verifiedGameCallerRva(
                snapshot.craft_optional_constructor.first_caller_pc),
            snapshot.craft_constructor_recorded);
        for (uint32_t index = 0; index < snapshot.captured_count; ++index) {
            const auto& record = snapshot.records[index];
            if (record.callback_index >= 32U ||
                (new_record_bits & (1U << record.callback_index)) == 0U) continue;
            const char* site = record.site == 2U ? "anvil_before_consume" :
                               record.site == 1U ? "generic_after_copy" :
                               "unknown";
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "created_output_record site=%s kind=%u container=%u dynamic=%u:%u "
                "slot=%u count=%u id=%d id2=%d tag=%d",
                site, record.kind, static_cast<unsigned>(record.container_name),
                static_cast<unsigned>(record.has_dynamic_id), record.dynamic_id,
                static_cast<unsigned>(record.slot), record.count,
                record.network_id,
                record.secondary_id, record.tag);
        }
        g_bridge.created_logged_count = snapshot.captured_count;
        g_bridge.created_logged_mask |= new_record_bits;
        g_bridge.created_logged_callbacks = snapshot.callback_count;
        g_bridge.created_logged_unreadable = snapshot.unreadable_count;
    }
    if (g_bridge.created_match_logged || !g_bridge.created_variant_seen ||
        !g_bridge.manual_input_seen || g_bridge.provisional_identity ||
        g_bridge.uuid_ambiguous || !g_bridge.uuid_seen) return;
    for (uint32_t index = 0; index < snapshot.captured_count; ++index) {
        const auto& record = snapshot.records[index];
        if (record.site != 2U || record.kind != 4U ||
            record.container_name != 61U || record.slot != 50U ||
            record.network_id != g_bridge.created_variant_network_id ||
            record.secondary_id != g_bridge.created_variant_secondary_id ||
            record.tag != g_bridge.created_variant_tag) continue;
        g_bridge.created_match_logged = true;
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "created_output_correlated site=anvil_before_consume request=%d "
            "window=%u uuid=%lld id=%d id2=%d tag=%d",
            g_bridge.created_variant_request_id,
            static_cast<unsigned>(g_bridge.window_id),
            static_cast<long long>(g_bridge.map_uuid),
            record.network_id, record.secondary_id, record.tag);
        break;
    }
}

void logNativeActionTraceLocked(const char* phase) noexcept {
    if (!g_bridge.native_action_armed) return;
    MapAnvilNativeActionTraceSnapshot snapshot;
    if (!ReadMapAnvilNativeActionTrace(kTicket, &snapshot)) return;
    if (snapshot.result_click_seen && !g_bridge.native_result_logged) {
        g_bridge.native_result_logged = true;
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "native_result_click phase=%s window=%u w1=%d w3=%d "
            "same_tick_thread=%d callback_calls=%u unbound=%d",
            phase, static_cast<unsigned>(g_bridge.window_id),
            snapshot.result_w1, snapshot.result_w3,
            snapshot.result_click_on_arm_thread ? 1 : 0,
            snapshot.callback_calls,
            g_bridge.unbound_fallback_consumed ? 1 : 0);
    }
    for (uint8_t i = 0; i < snapshot.recipe_count; ++i) {
        const uint32_t id = snapshot.recipe_ids[i];
        const uint8_t bit = static_cast<uint8_t>(1U << i);
        if (id != 0U && (g_bridge.native_recipe_logged_mask & bit) == 0U) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "native_recipe_source phase=%s window=%u ordinal=%u recipe=%u "
                "lookup_calls=%u",
                phase, static_cast<unsigned>(g_bridge.window_id),
                static_cast<unsigned>(i), id, snapshot.recipe_lookup_calls);
            g_bridge.native_recipe_logged_mask |= bit;
        }
    }
}

void invalidateLocked(const char* reason) noexcept {
    if (g_bridge.invalidated) return;
    g_bridge.invalidated = true;
    if (g_bridge.native_action_armed) {
        logNativeActionTraceLocked(reason);
        MapAnvilNativeActionTraceSnapshot snapshot;
        if (ReadMapAnvilNativeActionTrace(kTicket, &snapshot)) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "native_action_trace_end reason=%s unbound=%d callback_calls=%u "
                "result_seen=%d recipe_calls=%u",
                reason, g_bridge.unbound_fallback_consumed ? 1 : 0,
                snapshot.callback_calls, snapshot.result_click_seen ? 1 : 0,
                snapshot.recipe_lookup_calls);
        }
        DisarmMapAnvilNativeActionTrace(kTicket);
        g_bridge.native_action_armed = false;
    }
    if (g_bridge.created_probe_armed) {
        logCreatedProbeLocked(reason);
        DisarmMapAnvilCreatedOutputProbe(kTicket);
        g_bridge.created_probe_armed = false;
    }
    if (g_bridge.probe_armed) {
        logProbeCountersLocked(reason);
        DisarmMapAnvilRecipeProbe(kTicket);
        g_bridge.probe_armed = false;
    }
    g_wire.Disarm(kTicket);
    __android_log_print(ANDROID_LOG_INFO, kTag, "closed reason=%s", reason);
}

void correlateManualInputLocked() noexcept {
    if (!g_bridge.manual_input_seen || g_bridge.uuid_ambiguous) return;
    if (g_bridge.provisional_identity &&
        g_bridge.provisional_network_id != g_bridge.manual_input_network_id) {
        g_bridge.uuid_ambiguous = true;
        if (g_bridge.created_probe_armed) {
            logCreatedProbeLocked("input_identity_mismatch");
            DisarmMapAnvilCreatedOutputProbe(kTicket);
            g_bridge.created_probe_armed = false;
        }
        if (g_bridge.probe_armed) {
            DisarmMapAnvilRecipeProbe(kTicket);
            g_bridge.probe_armed = false;
        }
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "input_map_identity provisional_mismatch request=%d selected_net=%d source_net=%d",
            g_bridge.manual_input_request_id, g_bridge.provisional_network_id,
            g_bridge.manual_input_network_id);
        return;
    }
    int64_t map_uuid = -1;
    const auto match = g_bridge.identity_index.Resolve(
        g_bridge.manual_input_network_id, &map_uuid);
    if (match == MapAnvilIdentityMatch::Missing) return;
    if (match == MapAnvilIdentityMatch::Ambiguous ||
        (g_bridge.uuid_seen && g_bridge.map_uuid != map_uuid)) {
        g_bridge.uuid_ambiguous = true;
        if (g_bridge.created_probe_armed) {
            logCreatedProbeLocked("input_identity_ambiguous");
            DisarmMapAnvilCreatedOutputProbe(kTicket);
            g_bridge.created_probe_armed = false;
        }
        if (g_bridge.probe_armed) {
            DisarmMapAnvilRecipeProbe(kTicket);
            g_bridge.probe_armed = false;
        }
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "input_map_identity ambiguous request=%d net=%d",
            g_bridge.manual_input_request_id, g_bridge.manual_input_network_id);
        return;
    }
    if (g_bridge.provisional_identity) {
        g_bridge.provisional_identity = false;
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "input_map_identity confirmed request=%d net=%d uuid=%lld",
            g_bridge.manual_input_request_id, g_bridge.manual_input_network_id,
            static_cast<long long>(map_uuid));
        return;
    }
    if (g_bridge.uuid_seen) return;
    g_bridge.map_uuid = map_uuid;
    g_bridge.uuid_seen = true;
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "input_map_identity matched request=%d net=%d uuid=%lld",
        g_bridge.manual_input_request_id, g_bridge.manual_input_network_id,
        static_cast<long long>(map_uuid));
}

}  // namespace

void InitMapAnvilDebugBridge() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_bridge.initialized) return;
        g_bridge.initialized = true;
        if (g_wire.ArmDebugFirstType5Candidate()) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "ready first_type5_candidate_only max_window_seconds=90 max_events=32 "
                "legacy_map_items_getter_probe=disabled manual_unbound_fallback=available");
        } else {
            g_bridge.invalidated = true;
        }
    } catch (...) {
    }
}

void ObserveMapAnvilDebugInbound(std::string_view packet) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_bridge.initialized || g_bridge.invalidated) return;
        const uint32_t packet_id = packetId(packet);
        if (packet_id == 0x05U || packet_id == 0x0BU || packet_id == 0x3DU) {
            // A world/session transition invalidates every prior window and
            // request ID. The one-shot debug trace never silently re-arms.
            if (g_bridge.opened || g_bridge.unbound_fallback_consumed)
                invalidateLocked("world_or_session_change");
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (g_bridge.opened && now >= g_bridge.deadline) {
            invalidateLocked("deadline");
            return;
        }
        MapAnvilDiagnosticEvent event;
        if (g_wire.Observe(packet, now, &event)) {
            switch (event.kind) {
                case MapAnvilDiagnosticEventKind::Open:
                    g_bridge.opened = true;
                    g_bridge.window_id = event.window_id;
                    g_bridge.container_type = event.container_type;
                    g_bridge.x = event.x;
                    g_bridge.y = event.y;
                    g_bridge.z = event.z;
                    g_bridge.open_packet.assign(packet.data(), packet.size());
                    g_bridge.deadline = now + std::chrono::seconds(90);
                    g_bridge.next_snapshot = now;
                    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "candidate_open window=%u type=%u pos=%d,%d,%d bytes=%zu block_unverified=1",
                        static_cast<unsigned>(event.window_id),
                        static_cast<unsigned>(event.container_type),
                        event.x, event.y, event.z, packet.size());
                    break;
                case MapAnvilDiagnosticEventKind::Content:
                case MapAnvilDiagnosticEventKind::Slot:
                    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "window_update kind=%u inventory=%u slots=%u changed=%u maps=%u "
                        "full_name_present=%d full_name=%u dynamic=%d dynamic_id=%u",
                        static_cast<unsigned>(event.kind), event.inventory_id,
                        event.slot_count, static_cast<unsigned>(event.changed_slot),
                        static_cast<unsigned>(event.matching_map_count),
                        event.has_full_container_name ? 1 : 0,
                        static_cast<unsigned>(event.full_container_name),
                        event.has_dynamic_container_id ? 1 : 0,
                        event.dynamic_container_id);
                    for (uint8_t index = 0; index < event.matching_map_count; ++index) {
                        const auto& item = event.matching_maps[index];
                        logMapItem(item, event.inventory_id);
                        if (item.has_network_stack_id) {
                            g_bridge.identity_index.Observe(
                                item.network_stack_id, item.map_uuid);
                        }
                    }
                    correlateManualInputLocked();
                    break;
                case MapAnvilDiagnosticEventKind::Close:
                    g_bridge.closed = true;
                    if (g_bridge.native_action_armed) {
                        logNativeActionTraceLocked("window_close");
                        DisarmMapAnvilNativeActionTrace(kTicket);
                        g_bridge.native_action_armed = false;
                    }
                    if (g_bridge.created_probe_armed) {
                        logCreatedProbeLocked("window_close");
                        DisarmMapAnvilCreatedOutputProbe(kTicket);
                        g_bridge.created_probe_armed = false;
                    }
                    if (g_bridge.probe_armed) {
                        logProbeCountersLocked("window_close");
                        MapAnvilRecipeObservation observation;
                        const auto status = ReadMapAnvilRecipeProbe(
                            kTicket, g_bridge.map_uuid, &observation);
                        if (!g_bridge.probe_result_logged &&
                            (status == MapAnvilRecipeProbeStatus::Observed ||
                             status == MapAnvilRecipeProbeStatus::Ambiguous ||
                             status == MapAnvilRecipeProbeStatus::Expired)) {
                            g_bridge.probe_result_logged = true;
                            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "recipe_probe_result status=%u window=%u uuid=%lld recipe=%u",
                                static_cast<unsigned>(status),
                                static_cast<unsigned>(observation.window_id),
                                static_cast<long long>(observation.map_uuid),
                                observation.recipe_network_id);
                        }
                        DisarmMapAnvilRecipeProbe(kTicket);
                        g_bridge.probe_armed = false;
                        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "recipe_probe_disarm reason=window_close");
                    }
                    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "candidate_close window=%u type=%u",
                        static_cast<unsigned>(event.window_id),
                        static_cast<unsigned>(event.container_type));
                    break;
                case MapAnvilDiagnosticEventKind::None:
                    break;
            }
        }
        if (packet_id != 0x94U ||
            (!g_bridge.opened && !g_bridge.unbound_fallback_consumed)) return;
        for (size_t index = 0; index < g_bridge.request_count; ++index) {
            auto& request = g_bridge.requests[index];
            if (request.response_logged) continue;
            ProjectionPrinterInventoryResponse response;
            if (!GetProjectionPrinterInventoryResponseByRequestId(
                    request.request_id, &response) ||
                response.session_generation != request.session_generation ||
                response.response_generation <= request.response_generation) continue;
            request.response_logged = true;
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "rename_response request=%d status=%u accepted=%d rejected=%d "
                "layout=%u entries=%u slots=%zu",
                request.request_id, static_cast<unsigned>(response.status),
                response.valid ? 1 : 0, response.rejected ? 1 : 0,
                static_cast<unsigned>(response.layout), response.entry_count,
                response.slots.size());
            for (const auto& slot : response.slots) {
                __android_log_print(ANDROID_LOG_INFO, kTag,
                    "rename_response_slot request=%d container=%u slot=%u "
                    "hotbar=%u count=%u net=%d custom_name_bytes=%zu",
                    request.request_id, static_cast<unsigned>(slot.container_id),
                    static_cast<unsigned>(slot.slot),
                    static_cast<unsigned>(slot.hotbar_slot),
                    static_cast<unsigned>(slot.count), slot.network_stack_id,
                    slot.custom_name.size());
            }
        }
    } catch (...) {
    }
}

void ObserveMapAnvilDebugOutboundInput(
    int32_t request_id, int32_t source_network_stack_id) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_bridge.initialized || g_bridge.invalidated || g_bridge.closed ||
            request_id == 0 || source_network_stack_id <= 0) return;
        if (!g_bridge.opened) {
            // The automatic map Place can arrive before this diagnostic bridge
            // observes ContainerOpen. It must never arm the unbound manual
            // action trace: that installs two extra Dobby instruments on the
            // live game thread, while production rename uses neither trace.
            return;
        }
        if (std::chrono::steady_clock::now() >= g_bridge.deadline) return;
        if (g_bridge.manual_input_seen) {
            if (g_bridge.manual_input_request_id == request_id &&
                g_bridge.manual_input_network_id == source_network_stack_id) return;
            // Multiple possible input items in one manual window cannot be
            // matched to a later rename with certainty.
            g_bridge.uuid_ambiguous = true;
            if (g_bridge.created_probe_armed) {
                logCreatedProbeLocked("multiple_inputs");
                DisarmMapAnvilCreatedOutputProbe(kTicket);
                g_bridge.created_probe_armed = false;
            }
            if (g_bridge.probe_armed) {
                DisarmMapAnvilRecipeProbe(kTicket);
                g_bridge.probe_armed = false;
            }
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "input_map_identity ambiguous_multiple_inputs first_request=%d next_request=%d",
                g_bridge.manual_input_request_id, request_id);
            return;
        }
        g_bridge.manual_input_seen = true;
        g_bridge.manual_input_request_id = request_id;
        g_bridge.manual_input_network_id = source_network_stack_id;
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "input_request request=%d source_net=%d native_candidates=%zu",
            request_id, source_network_stack_id, g_bridge.identity_index.Size());
        correlateManualInputLocked();
    } catch (...) {
    }
}

void ObserveMapAnvilDebugOutboundRename(int32_t request_id,
                                        uint32_t observed_recipe_id,
                                        uint32_t custom_name_count) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if ((!g_bridge.opened && !g_bridge.unbound_fallback_consumed) ||
            g_bridge.invalidated || g_bridge.closed ||
            request_id == 0 || observed_recipe_id == 0 || custom_name_count == 0 ||
            g_bridge.request_count >= kMaximumRenameRequests ||
            std::chrono::steady_clock::now() >= g_bridge.deadline) return;
        for (size_t index = 0; index < g_bridge.request_count; ++index) {
            if (g_bridge.requests[index].request_id == request_id) return;
        }
        TrackedRequest request;
        request.request_id = request_id;
        request.session_generation =
            GetProjectionPrinterInventoryMailboxSessionGeneration();
        ProjectionPrinterInventoryResponse latest;
        if (GetProjectionPrinterInventoryResponse(&latest)) {
            request.response_generation = latest.response_generation;
        }
        g_bridge.requests[g_bridge.request_count++] = request;
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "rename_request request=%d observed_recipe=%u custom_name_count=%u "
            "window=%u block_verified=%d map_uuid_known=%d source_confirmed=%d "
            "unbound=%d",
            request_id, observed_recipe_id, custom_name_count,
            static_cast<unsigned>(g_bridge.window_id),
            g_bridge.block_is_anvil ? 1 : 0,
            g_bridge.uuid_seen && !g_bridge.uuid_ambiguous ? 1 : 0,
            g_bridge.manual_input_seen && g_bridge.uuid_seen &&
                !g_bridge.provisional_identity && !g_bridge.uuid_ambiguous ? 1 : 0,
            g_bridge.unbound_fallback_consumed ? 1 : 0);
        logProbeCountersLocked("rename_request");
        logCreatedProbeLocked("rename_request");
    } catch (...) {
    }
}

void ObserveMapAnvilDebugOutboundCreatedOutputVariant(
    int32_t request_id, int32_t network_id, int32_t secondary_id,
    int32_t tag) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_bridge.created_probe_armed || !g_bridge.opened ||
            g_bridge.invalidated || g_bridge.closed ||
            !g_bridge.block_is_anvil || !g_bridge.uuid_seen ||
            g_bridge.uuid_ambiguous || request_id == 0 ||
            std::chrono::steady_clock::now() >= g_bridge.deadline) return;
        // Only a verified manual source 61:50 may reach this bridge API. The
        // exact three words are compared with the native record; neither a
        // negative ID nor a recipe ID is inferred from the action.
        if (g_bridge.created_variant_seen &&
            (g_bridge.created_variant_request_id != request_id ||
             g_bridge.created_variant_network_id != network_id ||
             g_bridge.created_variant_secondary_id != secondary_id ||
             g_bridge.created_variant_tag != tag)) {
            logCreatedProbeLocked("ambiguous_manual_variants");
            DisarmMapAnvilCreatedOutputProbe(kTicket);
            g_bridge.created_probe_armed = false;
            return;
        }
        g_bridge.created_variant_seen = true;
        g_bridge.created_variant_request_id = request_id;
        g_bridge.created_variant_network_id = network_id;
        g_bridge.created_variant_secondary_id = secondary_id;
        g_bridge.created_variant_tag = tag;
        logCreatedProbeLocked("manual_variant");
    } catch (...) {
    }
}

void TickMapAnvilDebugBridge() noexcept {
    try {
        // The production result sender fingerprints the original anvil
        // handler. Keep every optional trace that instruments it disabled.
        constexpr bool kEnableManualAnvilInstruments = false;
        std::unique_lock<std::mutex> lock(g_mutex);
        if (g_bridge.invalidated ||
            (!g_bridge.opened && !g_bridge.unbound_fallback_consumed)) return;
        if (std::chrono::steady_clock::now() >= g_bridge.deadline) {
            invalidateLocked("deadline");
            return;
        }
        if (kEnableManualAnvilInstruments &&
            g_bridge.unbound_fallback_pending &&
            !g_bridge.native_action_attempted) {
            g_bridge.native_action_attempted = true;
            g_bridge.unbound_fallback_pending = false;
            const int32_t request = g_bridge.unbound_input_request_id;
            const int32_t source_net = g_bridge.unbound_input_network_id;
            lock.unlock();
            const bool armed = ArmMapAnvilUnboundNativeActionTrace(
                Main::getBaseAddress(), kTicket);
            lock.lock();
            if (g_bridge.invalidated) {
                if (armed) DisarmMapAnvilNativeActionTrace(kTicket);
                return;
            }
            g_bridge.native_action_armed = armed;
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "native_action_trace_arm success=%d unbound=1 request=%d "
                "source_net=%d window_unverified=1 map_uuid_unverified=1 "
                "callback_anvil_guard=1 read_only=1 max_seconds=90",
                armed ? 1 : 0, request, source_net);
        }
        if (!g_bridge.opened) {
            if (g_bridge.native_action_armed)
                logNativeActionTraceLocked("game_tick_unbound");
            return;
        }
        if (!g_bridge.block_checked) {
            // NativeWorldAccess is only legal on the local-player tick. The
            // type-5 packet alone is never treated as block identity proof.
            // Release the bridge mutex during engine access so the receive
            // thread can still record a close/world transition immediately.
            const uint8_t window = g_bridge.window_id;
            const int32_t x = g_bridge.x, y = g_bridge.y, z = g_bridge.z;
            lock.unlock();
            NativeBlockInfo block;
            const bool native_read = NativeWorldAccess::getBlock(x, y, z, &block);
            const bool anvil = native_read && isAnvilName(block.name);
            lock.lock();
            if (g_bridge.invalidated || g_bridge.window_id != window) return;
            g_bridge.block_checked = true;
            g_bridge.block_is_anvil = anvil;
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "candidate_block window=%u native_read=%d anvil=%d",
                static_cast<unsigned>(g_bridge.window_id),
                native_read ? 1 : 0, anvil ? 1 : 0);
        }
        if (g_bridge.block_is_anvil &&
            (!g_bridge.uuid_seen || g_bridge.provisional_identity) &&
            !g_bridge.uuid_ambiguous &&
            g_bridge.snapshot_attempts < kMaximumNativeSnapshotAttempts &&
            std::chrono::steady_clock::now() >= g_bridge.next_snapshot) {
            // Capture the source stack while it is still in the player's
            // inventory. The manual Place may remove it before the anvil sends
            // any UUID-bearing slot update, as seen in the 08:50 trace.
            const uint8_t window = g_bridge.window_id;
            lock.unlock();
            MapInventorySnapshot snapshot;
            const bool read = ReadMapInventorySnapshot(&snapshot, nullptr);
            lock.lock();
            if (g_bridge.invalidated || g_bridge.window_id != window) return;
            ++g_bridge.snapshot_attempts;
            g_bridge.next_snapshot = std::chrono::steady_clock::now() +
                kSnapshotInterval;
            uint32_t observed_maps = 0;
            uint32_t filled_maps = 0;
            int32_t sole_network_id = 0;
            int64_t sole_uuid = -1;
            if (read && snapshot.ready && snapshot.network_ready) {
                for (const auto& item : snapshot.slots) {
                    if (!item.filled_map || item.count == 0U) continue;
                    ++filled_maps;
                    if (!item.native_occupied || !item.has_map_uuid ||
                        !item.has_network_stack_id ||
                        item.network_stack_id <= 0) continue;
                    g_bridge.identity_index.Observe(
                        item.network_stack_id, item.map_uuid);
                    sole_network_id = item.network_stack_id;
                    sole_uuid = item.map_uuid;
                    ++observed_maps;
                }
            }
            // With exactly one native-confirmed filled map in the complete
            // player inventory, arm the read-only getter probe before the
            // manual Place can cause the client to compute a recipe. The
            // later Place source network ID must confirm this provisional
            // identity; a mismatch disarms the probe.
            if (!g_bridge.uuid_seen && !g_bridge.manual_input_seen &&
                filled_maps == 1U && observed_maps == 1U &&
                sole_network_id > 0 && sole_uuid != -1) {
                g_bridge.map_uuid = sole_uuid;
                g_bridge.uuid_seen = true;
                g_bridge.provisional_identity = true;
                g_bridge.provisional_network_id = sole_network_id;
                __android_log_print(ANDROID_LOG_INFO, kTag,
                    "provisional_map_identity net=%d uuid=%lld source_unconfirmed=1",
                    sole_network_id, static_cast<long long>(sole_uuid));
            }
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "native_inventory_snapshot attempt=%u ready=%d filled=%u maps=%u candidates=%zu input_seen=%d",
                static_cast<unsigned>(g_bridge.snapshot_attempts),
                read && snapshot.ready && snapshot.network_ready ? 1 : 0,
                filled_maps, observed_maps,
                g_bridge.identity_index.Size(), g_bridge.manual_input_seen ? 1 : 0);
            correlateManualInputLocked();
        }
        if (kEnableLegacyMapItemsGetterProbe && !g_bridge.probe_attempted &&
            g_bridge.block_is_anvil &&
            g_bridge.uuid_seen && !g_bridge.uuid_ambiguous && !g_bridge.closed) {
            g_bridge.probe_attempted = true;
            const uint8_t window = g_bridge.window_id;
            const uint8_t type = g_bridge.container_type;
            const int64_t map_uuid = g_bridge.map_uuid;
            const std::string open_packet = g_bridge.open_packet;
            MapAnvilWindowEvidence evidence;
            evidence.container_open_packet = open_packet;
            evidence.window_id = window;
            evidence.observed_container_type = type;
            evidence.anvil_block_and_window_confirmed = true;
            lock.unlock();
            const bool armed = ArmMapAnvilRecipeProbe(
                Main::getBaseAddress(), kTicket, map_uuid, evidence);
            lock.lock();
            if (g_bridge.invalidated || g_bridge.window_id != window ||
                g_bridge.closed) {
                if (armed) DisarmMapAnvilRecipeProbe(kTicket);
                return;
            }
            g_bridge.probe_armed = armed;
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "recipe_probe_arm success=%d window=%u uuid=%lld",
                armed ? 1 : 0, static_cast<unsigned>(window),
                static_cast<long long>(map_uuid));
        }
        // The automatic result sender verifies the original game's handler
        // prologue before calling it. These legacy manual diagnostics install
        // Dobby instruments into that same live anvil path, so they must stay
        // off while automatic map storage is enabled. They provide logs only;
        // neither contributes recipe or output evidence to production flow.
        if (kEnableManualAnvilInstruments &&
            !g_bridge.created_probe_attempted && g_bridge.block_is_anvil &&
            g_bridge.uuid_seen && !g_bridge.uuid_ambiguous &&
            !g_bridge.closed) {
            g_bridge.created_probe_attempted = true;
            const uint8_t window = g_bridge.window_id;
            const uint8_t type = g_bridge.container_type;
            const int64_t map_uuid = g_bridge.map_uuid;
            const std::string open_packet = g_bridge.open_packet;
            MapAnvilWindowEvidence evidence;
            evidence.container_open_packet = open_packet;
            evidence.window_id = window;
            evidence.observed_container_type = type;
            evidence.anvil_block_and_window_confirmed = true;
            lock.unlock();
            const bool packet_profile_ready =
                PrimeMapManualOutboundTraceProfile();
            const bool armed = packet_profile_ready &&
                ArmMapAnvilCreatedOutputProbe(
                    Main::getBaseAddress(), kTicket, map_uuid, evidence);
            lock.lock();
            if (g_bridge.invalidated || g_bridge.window_id != window ||
                g_bridge.closed) {
                if (armed) DisarmMapAnvilCreatedOutputProbe(kTicket);
                return;
            }
            g_bridge.created_probe_armed = armed;
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "created_output_probe_arm success=%d packet_profile=%d window=%u uuid=%lld "
                "provisional=%d max_seconds=90 max_callbacks=32",
                armed ? 1 : 0, packet_profile_ready ? 1 : 0,
                static_cast<unsigned>(window),
                static_cast<long long>(map_uuid),
                g_bridge.provisional_identity ? 1 : 0);
        }
        if (kEnableManualAnvilInstruments &&
            !g_bridge.native_action_attempted && g_bridge.block_is_anvil &&
            g_bridge.manual_input_seen && g_bridge.uuid_seen &&
            !g_bridge.provisional_identity && !g_bridge.uuid_ambiguous &&
            !g_bridge.closed) {
            g_bridge.native_action_attempted = true;
            const uint8_t window = g_bridge.window_id;
            const int64_t map_uuid = g_bridge.map_uuid;
            lock.unlock();
            const bool armed = ArmMapAnvilNativeActionTrace(
                Main::getBaseAddress(), kTicket, window, map_uuid);
            lock.lock();
            if (g_bridge.invalidated || g_bridge.window_id != window ||
                g_bridge.closed) {
                if (armed) DisarmMapAnvilNativeActionTrace(kTicket);
                return;
            }
            g_bridge.native_action_armed = armed;
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "native_action_trace_arm success=%d window=%u uuid=%lld "
                "read_only=1 max_seconds=90",
                armed ? 1 : 0, static_cast<unsigned>(window),
                static_cast<long long>(map_uuid));
        }
        if (g_bridge.native_action_armed) logNativeActionTraceLocked("game_tick");
        if (g_bridge.created_probe_armed) logCreatedProbeLocked("game_tick");
        if (g_bridge.probe_armed && !g_bridge.probe_result_logged) {
            MapAnvilRecipeObservation observation;
            const auto status = ReadMapAnvilRecipeProbe(
                kTicket, g_bridge.map_uuid, &observation);
            if (status == MapAnvilRecipeProbeStatus::Observed ||
                status == MapAnvilRecipeProbeStatus::Ambiguous ||
                status == MapAnvilRecipeProbeStatus::Expired) {
                g_bridge.probe_result_logged = true;
                __android_log_print(ANDROID_LOG_INFO, kTag,
                    "recipe_probe_result status=%u window=%u uuid=%lld recipe=%u",
                    static_cast<unsigned>(status),
                    static_cast<unsigned>(observation.window_id),
                    static_cast<long long>(observation.map_uuid),
                    observation.recipe_network_id);
            }
        }
    } catch (...) {
    }
}

void ClearMapAnvilDebugBridge() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_bridge.opened || g_bridge.unbound_fallback_consumed)
            invalidateLocked("revoked");
    } catch (...) {
    }
}

}  // namespace build_import

#else

namespace build_import {
void InitMapAnvilDebugBridge() noexcept {}
void ObserveMapAnvilDebugInbound(std::string_view) noexcept {}
void ObserveMapAnvilDebugOutboundInput(int32_t, int32_t) noexcept {}
void ObserveMapAnvilDebugOutboundRename(int32_t, uint32_t, uint32_t) noexcept {}
void ObserveMapAnvilDebugOutboundCreatedOutputVariant(
    int32_t, int32_t, int32_t, int32_t) noexcept {}
void TickMapAnvilDebugBridge() noexcept {}
void ClearMapAnvilDebugBridge() noexcept {}
}  // namespace build_import

#endif
