#include "MapNativeAnvilResultActionBridge.h"

#include "MapNativeAnvilCraftPacket.h"
#include "MapTileNaming.h"

#include <atomic>
#include <climits>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && \
    defined(__ANDROID__) && defined(__aarch64__)
#include "MapNativeAnvilManagerProbe.h"
#include "MapNativeAnvilResultGroupProbe.h"
#include "ProjectionPrinterInventoryMailbox.h"
#include "../main.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include <android/log.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace build_import {
namespace {

bool reject(std::string* error, const char* reason) {
    if (error) *error = reason;
    return false;
}

bool isFilledMap(const std::string& identifier) noexcept {
    return identifier == "minecraft:map" || identifier == "map" ||
        identifier == "minecraft:filled_map" || identifier == "filled_map" ||
        identifier == "minecraft:locator_map" || identifier == "locator_map";
}

#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && \
    defined(__ANDROID__) && defined(__aarch64__)
// Both APK variants include the verified result-click path. Device-side
// debug properties must not silently disable it after a reboot; the exact
// same-window input, named preview, result group, packet shape, fingerprint,
// and pre-send durable journal checks below remain mandatory.
bool automaticResultClickSupported() noexcept {
    return true;
}
constexpr uintptr_t kResultHandlerRva = 0x05FD22E8ULL;
constexpr uint8_t kResultHandlerFingerprint[32] = {
    0xFF,0xC3,0x02,0xD1,0xFD,0x7B,0x07,0xA9,0xF7,0x43,0x00,0xF9,0xF6,0x57,0x09,0xA9,
    0xF4,0x4F,0x0A,0xA9,0xFD,0xC3,0x01,0x91,0x57,0xD0,0x3B,0xD5,0x49,0x00,0x80,0x52,
};
constexpr uintptr_t kRequestPacketVtableRva = 0x12951140ULL;

// The game's result handler queues its ItemStackRequest; sendToServer may run
// on a later tick after Submit has returned. No pointer into the Submit stack
// may survive that return. The one active click is owned here until its exact
// outbound request is durably armed or the process exits.
struct PendingCraftClick {
    std::string map_state_path;
    MapAnvilRenameRecord click_record;
    MapAnvilInputProof input;
    std::string expected_title;
    int32_t input_network_id = 0;
    uint32_t preview_recipe_network_id = 0;
    long game_thread_id = 0;
    uint64_t window_token = 0;
    uint8_t window_id = 0;
    bool dispatch_attempted = false;
    bool dispatched = false;
    MapAnvilCraftSubmission submission{};
};

std::mutex g_pending_mutex;
std::optional<PendingCraftClick> g_pending_click;
std::atomic<bool> g_intercept_enabled{false};
// Request IDs may be reused after reconnect. Retain the exact (session, ID)
// pair so replacing the active click cannot release a delayed replay.
std::set<std::pair<uint64_t, int32_t>> g_dispatched_request_ids;

long currentThreadId() noexcept { return ::syscall(SYS_gettid); }

void logCraft(const char* event, const PendingCraftClick& click,
              int32_t request_id, const char* detail = "") noexcept {
    __android_log_print(ANDROID_LOG_INFO, "Infinitecz_MapAnvilAuto",
        "%s window=%u token=%llu request=%d thread=%ld detail=%s",
        event, static_cast<unsigned>(click.window_id),
        static_cast<unsigned long long>(click.window_token), request_id,
        click.game_thread_id, detail);
}

bool readPointer(uintptr_t address, uintptr_t* output) noexcept {
    if (!address || !output ||
        !IsMemoryReadable(reinterpret_cast<const void*>(address),
                          sizeof(uintptr_t))) return false;
    std::memcpy(output, reinterpret_cast<const void*>(address),
                sizeof(uintptr_t));
    return true;
}

bool isCurrentRequestPacket(uintptr_t base, const void* packet) noexcept {
    if (!base || !packet ||
        base > std::numeric_limits<uintptr_t>::max() -
                   kRequestPacketVtableRva) return false;
    uintptr_t vtable = 0;
    return readPointer(reinterpret_cast<uintptr_t>(packet), &vtable) &&
        vtable == base + kRequestPacketVtableRva;
}

bool responseBaseline(uint64_t expected_session, uint64_t* generation) noexcept {
    if (!generation || !expected_session ||
        GetProjectionPrinterInventoryMailboxSessionGeneration() !=
            expected_session) return false;
    ProjectionPrinterInventoryResponse prior;
    if (GetProjectionPrinterInventoryResponse(&prior) &&
        prior.session_generation == expected_session) {
        *generation = prior.response_generation;
    } else {
        *generation = 0;
    }
    return true;
}
#endif

}  // namespace

MapNativeAnvilOutboundDisposition ClassifyMapNativeAnvilOutbound(
    bool intent_active, bool is_item_stack_request,
    bool request_already_seen) noexcept {
    if (!intent_active) return MapNativeAnvilOutboundDisposition::PassUnrelated;
    if (!is_item_stack_request)
        return MapNativeAnvilOutboundDisposition::PassUnrelated;
    if (request_already_seen)
        return MapNativeAnvilOutboundDisposition::SuppressScopedUnexpected;
    return MapNativeAnvilOutboundDisposition::ExamineScopedRequest;
}

bool IsMapNativeAnvilAutomaticResultDispatchVerified() noexcept {
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && \
    defined(__ANDROID__) && defined(__aarch64__)
    return automaticResultClickSupported();
#else
    return false;
#endif
}

bool ValidateMapNativeAnvilResultActionGate(
    const MapAnvilRenameRecord& record,
    const MapAnvilInputProof& proof,
    const ContainerCaptureResult& live_window,
    const ProjectionPrinterNativeAnvilInputMapSnapshot& input,
    const ProjectionPrinterNativeAnvilPreviewMapSnapshot& preview,
    std::string* title, std::string* error) {
    if (title) title->clear();
    if (error) error->clear();
    if (!title || record.phase != MapAnvilRenamePhase::InputConfirmed ||
        record.world_id.empty() || record.map_runtime_item_id <= 0 ||
        record.map_uuid == -1 || record.input_request_id >= 0 ||
        (static_cast<uint32_t>(record.input_request_id) & 1U) == 0U ||
        record.craft_request_id != 0 ||
        record.input_response.session_generation == 0U ||
        record.input_response.window_token == 0U ||
        record.input_response.window_id == 0U ||
        record.input_response.window_id == 0xFFU) {
        return reject(error, "confirmed anvil input journal identity is invalid");
    }
    if (!proof.native_readback || proof.world_id != record.world_id ||
        proof.dimension_id != record.dimension_id ||
        proof.anvil_x != record.anvil_x ||
        proof.anvil_y != record.anvil_y ||
        proof.anvil_z != record.anvil_z ||
        proof.fresh_window_token != record.input_response.window_token ||
        proof.window_id != record.input_response.window_id ||
        proof.input_slot != 1U || proof.count != 1U ||
        proof.runtime_item_id != record.map_runtime_item_id ||
        proof.network_stack_id <= 0 || proof.map_uuid != record.map_uuid) {
        return reject(error, "native anvil input proof differs from the journal");
    }
    if (live_window.token != proof.fresh_window_token ||
        live_window.container_id != proof.window_id ||
        live_window.container_type != 5U ||
        live_window.slot_count != 3U ||
        !live_window.container_opened || live_window.container_closed ||
        live_window.x != proof.anvil_x || live_window.y != proof.anvil_y ||
        live_window.z != proof.anvil_z || !live_window.error.empty()) {
        return reject(error, "live anvil window is no longer the input window");
    }
    if (input.runtime_item_id != record.map_runtime_item_id ||
        input.network_stack_id != proof.network_stack_id ||
        input.count != 1U || input.map_uuid != record.map_uuid ||
        !isFilledMap(input.item_identifier)) {
        return reject(error, "native anvil input map changed before result click");
    }
    std::string formatted;
    if (!FormatMapTileName(record.tile_cursor, record.columns,
                           record.rows, &formatted) ||
        formatted.empty() || formatted.size() > 22U ||
        formatted != record.expected_title) {
        return reject(error, "anvil result title differs from the tile plan");
    }
    if (preview.runtime_item_id != record.map_runtime_item_id ||
        preview.count != 1U || preview.map_uuid != record.map_uuid ||
        !isFilledMap(preview.item_identifier) ||
        preview.display_name != formatted) {
        return reject(error, "native anvil result preview is not the named map");
    }
    *title = std::move(formatted);
    return true;
}

bool BeforeMapNativeAnvilResultOutboundPacket(const void* packet) noexcept {
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && \
    defined(__ANDROID__) && defined(__aarch64__)
    if (!g_intercept_enabled.load(std::memory_order_acquire)) return true;
    try {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        if (!g_pending_click && g_dispatched_request_ids.empty()) return true;
        const uintptr_t base = Main::getBaseAddress();
        if (!isCurrentRequestPacket(base, packet)) return true;
        MapNativeAnvilCraftPacketShape shape;
        const bool decoded = DecodeMapNativeAnvilCraftPacketCandidate(
            base, packet, &shape);
        const uint64_t current_session =
            GetProjectionPrinterInventoryMailboxSessionGeneration();
        if (decoded && g_dispatched_request_ids.count(
                {current_session, shape.request_id}) != 0U) {
            if (g_pending_click) {
                logCraft("result_replay_suppressed", *g_pending_click,
                         shape.request_id);
            }
            return false;
        }
        if (!g_pending_click || g_pending_click->dispatched) return true;
        PendingCraftClick& pending = *g_pending_click;
        // The observed handler and its delayed sender both run on the game
        // tick thread. Never let an off-thread ItemStackRequest escape while
        // this click is armed, but do not poison the expected later send.
        if (currentThreadId() != pending.game_thread_id) {
            logCraft("result_off_thread_suppressed", pending,
                     decoded ? shape.request_id : 0);
            return false;
        }
        if (pending.dispatch_attempted) {
            logCraft("result_second_request_suppressed", pending,
                     decoded ? shape.request_id : 0);
            return false;
        }
        pending.dispatch_attempted = true;
        if (!decoded || !IsMapNativeAnvilCraftPacketCandidate(
                shape, pending.expected_title, pending.input_network_id) ||
            shape.recipe_network_id != pending.preview_recipe_network_id ||
            pending.click_record.phase != MapAnvilRenamePhase::CraftClickArmed) {
            logCraft("result_shape_suppressed", pending,
                     decoded ? shape.request_id : 0);
            __android_log_print(ANDROID_LOG_INFO, "Infinitecz_MapAnvilAuto",
                "result_shape_detail decoded=%d actions=%u recipe=%u expected_recipe=%u "
                "input_net=%d expected_input_net=%d destination=%u",
                decoded ? 1 : 0, decoded ? shape.action_count : 0U,
                decoded ? shape.recipe_network_id : 0U,
                pending.preview_recipe_network_id,
                decoded ? shape.consumed_network_id : 0,
                pending.input_network_id,
                decoded ? static_cast<unsigned>(shape.destination_slot) : 0U);
            return false;
        }
        ContainerCaptureResult current;
        const auto capture_state = PollContainerCapture(
            pending.window_token, &current);
        if (capture_state != ContainerCapturePollState::Ready ||
            current.token != pending.window_token ||
            !current.container_opened || current.container_closed ||
            current.container_id != pending.window_id ||
            current.container_type != 5U || current.slot_count != 3U ||
            current.x != pending.input.anvil_x ||
            current.y != pending.input.anvil_y ||
            current.z != pending.input.anvil_z ||
            !current.error.empty()) {
            logCraft("result_window_suppressed", pending, shape.request_id);
            return false;
        }
        uint64_t generation = 0;
        if (!responseBaseline(
                pending.click_record.input_response.session_generation,
                &generation)) {
            logCraft("result_session_suppressed", pending, shape.request_id);
            return false;
        }
        const MapAnvilResponseCorrelation correlation{
            pending.click_record.input_response.session_generation,
            generation, pending.window_token, pending.window_id, -1};
        // Reserve this request ID before fsync. If the write is ambiguous,
        // retaining the reservation prevents this process from forwarding a
        // second packet with the same ID.
        if (!g_dispatched_request_ids.insert(
                {correlation.session_generation, shape.request_id}).second) {
            logCraft("result_duplicate_id_suppressed", pending,
                     shape.request_id);
            return false;
        }
        std::string error;
        if (!ArmMapAnvilCraftDispatch(
                pending.map_state_path, pending.click_record, pending.input,
                shape.request_id, correlation, shape.destination_slot,
                &error)) {
            logCraft("result_dispatch_fsync_failed", pending,
                     shape.request_id, error.c_str());
            return false;
        }
        pending.submission.request_id = shape.request_id;
        pending.submission.response_session_generation =
            correlation.session_generation;
        pending.submission.response_generation_before_send =
            correlation.response_generation_before_send;
        pending.submission.window_token = correlation.window_token;
        pending.submission.window_id = correlation.window_id;
        pending.submission.dynamic_recipe_network_id = shape.recipe_network_id;
        pending.submission.consumed_input_network_stack_id =
            shape.consumed_network_id;
        pending.submission.destination_hotbar_slot = shape.destination_slot;
        pending.dispatched = true;
        logCraft("result_dispatch_fsynced", pending, shape.request_id);
        return true;
    } catch (...) {
        // An exception after a possible fsync is ambiguous. Never forward an
        // unverified packet or retry this click in-process.
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        if (g_pending_click && !g_pending_click->dispatched)
            g_pending_click->dispatch_attempted = true;
        return false;
    }
#else
    (void)packet;
    return true;
#endif
}

bool SubmitMapNativeAnvilResultAction(
    uintptr_t minecraft_base,
    const MapNativeAnvilResultActionRequest& request,
    const MapVisibleAnvilWindowSession& session,
    MapNativeAnvilResultActionOutcome* outcome,
    std::string* error) {
    if (outcome) *outcome = {};
    if (error) error->clear();
#if defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE && \
    defined(__ANDROID__) && defined(__aarch64__)
    if (!outcome || !request.confirmed || !request.input ||
        request.map_state_path.empty() || !request.now_ms) {
        return reject(error, "native result action request or send intent is invalid");
    }
    try {
        {
            std::lock_guard<std::mutex> lock(g_pending_mutex);
            if (g_pending_click && !g_pending_click->dispatched) {
                return reject(error,
                    "previous native anvil result click remains armed; never retry");
            }
        }
        const auto& record = *request.confirmed;
        const auto& proof = *request.input;
        if (record.input_response.window_token == 0U ||
            proof.fresh_window_token != record.input_response.window_token ||
            !minecraft_base || minecraft_base != Main::getBaseAddress()) {
            return reject(error, "native result action module/window identity changed");
        }
        ContainerCaptureResult current;
        if (!session.verifiedLiveCapture(proof.fresh_window_token,
                                         request.now_ms, &current, error)) return false;
        uintptr_t manager = 0;
        if (!BorrowMapNativeAnvilManagerForVerifiedWindow(
                minecraft_base, proof.fresh_window_token, request.now_ms,
                session, &manager)) {
            return reject(error, "native anvil result manager/window is not bound");
        }
        ProjectionPrinterNativeAnvilInputMapSnapshot input;
        ProjectionPrinterNativeAnvilPreviewMapSnapshot preview;
        if (!ReadProjectionPrinterNativeAnvilInputMap(
                manager, record.map_runtime_item_id, record.map_uuid,
                &input, error) ||
            !ReadProjectionPrinterNativeAnvilPreviewMap(
                manager, record.map_runtime_item_id, record.map_uuid,
                record.expected_title, &preview, error)) return false;
        std::string title;
        if (!ValidateMapNativeAnvilResultActionGate(
                record, proof, current, input, preview, &title, error)) return false;
        if (preview.dynamic_recipe_network_id == 0U) {
            return reject(error, "native anvil result preview recipe is unavailable");
        }
        MapNativeAnvilResultGroupObservation group;
        if (!ProbeMapNativeAnvilResultGroup(
                 minecraft_base, proof.fresh_window_token, session,
                 &group, error) || !group.exact_match) return false;
        __android_log_print(ANDROID_LOG_INFO, "Infinitecz_MapAnvilAuto",
            "result_preflight_ok window=%u title_bytes=%zu group_exact=%d approved=%d",
            static_cast<unsigned>(current.container_id), title.size(),
            group.exact_match ? 1 : 0, automaticResultClickSupported() ? 1 : 0);
        uintptr_t screen = 0, screen_manager = 0, handler = 0;
        const bool screen_bound = BorrowMapNativeAnvilScreenForCurrentThread(
            minecraft_base, proof.fresh_window_token, &screen);
        const bool screen_address_safe = screen_bound &&
            screen <= std::numeric_limits<uintptr_t>::max() - 0xE40U;
        const bool manager_read = screen_address_safe &&
            readPointer(screen + 0xE40U, &screen_manager);
        const bool manager_matches = manager_read && screen_manager == manager;
        const bool handler_resolved = ResolveMinecraftExecutableOffset(
            minecraft_base, kResultHandlerRva,
            sizeof(kResultHandlerFingerprint), &handler);
        const bool handler_readable = handler_resolved &&
            IsMemoryReadable(reinterpret_cast<const void*>(handler),
                             sizeof(kResultHandlerFingerprint));
        const bool handler_matches = handler_readable &&
            std::memcmp(reinterpret_cast<const void*>(handler),
                        kResultHandlerFingerprint,
                        sizeof(kResultHandlerFingerprint)) == 0;
        if (!manager_matches || !handler_matches) {
            uint32_t observed_head = 0;
            if (handler_readable) {
                std::memcpy(&observed_head,
                            reinterpret_cast<const void*>(handler),
                            sizeof(observed_head));
            }
            __android_log_print(ANDROID_LOG_INFO, "Infinitecz_MapAnvilAuto",
                "result_handler_guard screen=%d manager_read=%d manager_match=%d "
                "resolved=%d readable=%d fingerprint=%d head=%08x",
                screen_bound ? 1 : 0, manager_read ? 1 : 0,
                manager_matches ? 1 : 0, handler_resolved ? 1 : 0,
                handler_readable ? 1 : 0, handler_matches ? 1 : 0,
                observed_head);
            return reject(error, "native anvil result handler ABI mismatch");
        }
        // The stock handler can queue the packet for a later game tick.
        // Persist the click itself before invoking it, then keep only owned
        // values in the pending bridge; no runtime/session/stack pointer may
        // be accessed by the later sender hook.
        if (!ArmMapAnvilCraftClick(request.map_state_path, record, proof,
                                  error)) return false;
        MapAnvilRenameRecord click_record;
        if (LoadMapAnvilRenameJournal(request.map_state_path, &click_record,
                                      error) != MapAnvilRenameLoad::Loaded ||
            click_record.phase != MapAnvilRenamePhase::CraftClickArmed ||
            click_record.world_id != record.world_id ||
            click_record.dimension_id != record.dimension_id ||
            click_record.tile_cursor != record.tile_cursor ||
            click_record.map_uuid != record.map_uuid ||
            click_record.expected_title != record.expected_title ||
            click_record.input_response.window_token != current.token ||
            click_record.input_response.window_id != current.container_id ||
            click_record.craft_input_network_stack_id !=
                proof.network_stack_id) {
            return reject(error,
                "durably armed anvil click could not be re-read exactly");
        }
        PendingCraftClick pending;
        pending.map_state_path = request.map_state_path;
        pending.click_record = std::move(click_record);
        pending.input = proof;
        pending.expected_title = std::move(title);
        pending.input_network_id = input.network_stack_id;
        pending.preview_recipe_network_id =
            preview.dynamic_recipe_network_id;
        pending.game_thread_id = currentThreadId();
        pending.window_token = current.token;
        pending.window_id = current.container_id;
        if (pending.game_thread_id <= 0) {
            return reject(error, "native anvil click game thread is unavailable");
        }
        {
            std::lock_guard<std::mutex> lock(g_pending_mutex);
            if (g_pending_click && !g_pending_click->dispatched) {
                return reject(error,
                    "another native anvil result click became armed");
            }
            g_pending_click.emplace(std::move(pending));
            g_intercept_enabled.store(true, std::memory_order_release);
            logCraft("result_click_fsynced", *g_pending_click, 0);
        }
        const std::string result_group = "anvil_result_items";
        using NativeResultHandler = void (*)(void*, int32_t,
                                              const std::string*, int32_t);
        reinterpret_cast<NativeResultHandler>(handler)(
            reinterpret_cast<void*>(screen), INT_MAX, &result_group, 0);
        bool dispatch_attempted = false;
        bool dispatched = false;
        {
            std::lock_guard<std::mutex> lock(g_pending_mutex);
            if (g_pending_click &&
                g_pending_click->window_token == current.token &&
                g_pending_click->click_record.tile_cursor ==
                    record.tile_cursor) {
                dispatch_attempted = g_pending_click->dispatch_attempted;
                dispatched = g_pending_click->dispatched;
                if (dispatched) {
                    outcome->submission = g_pending_click->submission;
                    outcome->dynamic_recipe_network_id =
                        g_pending_click->submission.dynamic_recipe_network_id;
                    outcome->send_invoked = true;
                }
            }
        }
        __android_log_print(ANDROID_LOG_INFO, "Infinitecz_MapAnvilAuto",
            "result_handler_returned click_armed=1 dispatch_attempted=%d "
            "dispatch_fsynced=%d async_pending=%d",
            dispatch_attempted ? 1 : 0, dispatched ? 1 : 0,
            (!dispatch_attempted && !dispatched) ? 1 : 0);
        if (dispatch_attempted && !dispatched) {
            return reject(error,
                "native result request was suppressed after an ambiguous dispatch");
        }
        return true;
    } catch (...) {
        if (outcome) outcome->outcome_uncertain = true;
        return reject(error,
            "native result click outcome is uncertain; reconcile before retry");
    }
#else
    (void)minecraft_base;
    (void)request;
    (void)session;
    return reject(error, "native anvil result action is unavailable in this build");
#endif
}

}  // namespace build_import
