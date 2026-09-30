#include "MapInventoryTransfer.h"

#include "ContainerClosePacketSender.h"
#include "PlayerInventoryOpenPacketSender.h"
#include "ProjectionPrinterInventoryClientSync.h"
#include "ProjectionPrinterInventoryMailbox.h"
#include "ProjectionPrinterInventoryMoveEvidence.h"
#include "ProjectionPrinterInventoryPresentationGate.h"
#include "ProjectionPrinterInventorySession.h"
#include "ProjectionPrinterNativeHotbarSelection.h"
#include "ProjectionPrinterRuntime.h"
#include "ItemRuntimeRegistry.h"
#include "../tp/PythonUtils.h"

#include <atomic>
#include <cctype>
#include <chrono>

namespace build_import {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kOpenTimeout = std::chrono::milliseconds(2500);
constexpr auto kMoveTimeout = std::chrono::milliseconds(2500);
constexpr auto kPostMoveReadyTimeout = std::chrono::seconds(5);
constexpr uint8_t kPostMoveCloseTicks = 3;
constexpr uint8_t kHotbarRequestContainer = 29U;
constexpr uint8_t kInventoryRequestContainer = 30U;
std::atomic<uint64_t> g_next_map_inventory_token{1U};

uint64_t nextSessionToken() {
    // Printer-owned tokens start at one. Keep importer tokens in a disjoint
    // namespace so a late cancellation cannot accidentally match its session.
    return (uint64_t{1} << 63U) |
        g_next_map_inventory_token.fetch_add(1U, std::memory_order_relaxed);
}

const ProjectionPrinterInventoryResponseSlot* uniqueResponseSlot(
    const ProjectionPrinterInventoryResponse& response, uint8_t container,
    int32_t physical_slot) {
    const ProjectionPrinterInventoryResponseSlot* found = nullptr;
    for (const auto& slot : response.slots) {
        if (slot.container_id != container ||
            slot.slot != static_cast<uint8_t>(physical_slot)) continue;
        if (found) return nullptr;
        found = &slot;
    }
    return found;
}

bool sameNativeStack(const ProjectionPrinterLiveInventorySlot& actual,
                     bool expected_occupied, int32_t expected_id) {
    if (actual.occupied != expected_occupied) return false;
    return expected_occupied
        ? actual.has_network_stack_id && actual.network_stack_id == expected_id
        : !actual.has_network_stack_id && actual.network_stack_id == 0;
}

bool sameNamedMap(const MapInventoryTransfer::NamedFilledMapIdentity& expected,
                  int32_t slot, int32_t network_id,
                  std::string* error) {
    ProjectionPrinterNativeInventoryFilledMapMatch actual;
    return ReadProjectionPrinterNativeInventoryFilledMapMatch(
               expected.runtime_item_id, expected.map_uuid, expected.title,
               &actual, error) &&
           actual.inventory_slot == slot &&
           actual.network_stack_id == network_id && actual.count == 1U;
}

void applyResolvedMapName(MapInventorySlot* slot, const std::string& resolved_name) {
    if (!slot || resolved_name.empty()) return;
    std::string name;
    name.reserve(resolved_name.size());
    for (const unsigned char ch : resolved_name) {
        name.push_back(static_cast<char>(std::tolower(ch)));
    }
    constexpr std::string_view kMinecraftPrefix = "minecraft:";
    std::string_view item_name(name);
    if (item_name.substr(0, kMinecraftPrefix.size()) == kMinecraftPrefix) {
        item_name.remove_prefix(kMinecraftPrefix.size());
    }
    slot->empty_map = item_name == "empty_map" || item_name == "emptymap";
    slot->filled_map = item_name == "map" || item_name == "filled_map" ||
        item_name == "locator_map";
}

}  // namespace

bool ReadMapInventorySnapshot(MapInventorySnapshot* output, std::string* error) {
    if (!output) {
        if (error) *error = "map inventory output is unavailable";
        return false;
    }
    *output = {};
    ProjectionPrinterNativeInventorySnapshot native;
    if (!ReadProjectionPrinterNativeInventorySnapshot(&native, error) || !native.ready) {
        return false;
    }
    if (!IsItemRuntimeRegistryReady()) {
        if (error) *error = "物品运行时注册表尚未就绪，无法原生确认空白地图";
        return false;
    }
    output->selected_hotbar_slot = native.selected_hotbar_slot;
    output->network_ready = true;
    for (size_t slot = 0; slot < output->slots.size(); ++slot) {
        auto& item = output->slots[slot];
        const auto& source = native.slots[slot];
        item.occupied = source.occupied;
        item.native_occupied = source.occupied;
        item.has_network_stack_id = source.has_network_stack_id;
        item.network_stack_id = source.network_stack_id;
        item.count = source.count;
        if (!source.occupied) continue;
        std::string identifier;
        if (ResolveItemRuntimeId(source.runtime_item_id, &identifier) !=
                ItemRuntimeResolveStatus::Ready || identifier.empty()) {
            // An unrelated custom item may not appear in the captured
            // registry. It remains occupied, but can never be treated as a
            // blank or filled map without a verified identifier.
            continue;
        }
        applyResolvedMapName(&item, identifier);
        if (item.filled_map) {
            int64_t uuid = -1;
            if (ReadProjectionPrinterNativeMapUuid(
                    static_cast<int32_t>(slot), item.network_stack_id,
                    &uuid, nullptr)) {
                item.has_map_uuid = true;
                item.map_uuid = uuid;
            }
            // Preserve filled-map kind/count/network ID even if its UUID is
            // temporarily unreadable. Otherwise an older map could disappear
            // from the pre-use baseline and be mistaken for the new map later.
            // The runtime must require has_map_uuid before treating a new
            // filled map as confirmed texture evidence.
        }
    }
    output->ready = true;
    if (error) error->clear();
    return true;
}

bool MapInventoryTransfer::requestSelection(int32_t slot) const {
    std::string ignored;
    if (RequestProjectionPrinterNativeHotbarSelection(slot, &ignored)) return true;
    // Late XP injection can predate the native HUD lifecycle hook. Use the
    // same bounded bootstrap key event as the working projection printer.
    if (slot < 0 || slot > 8) return false;
    const std::string script =
        "import gui\n"
        "_infinitecz_map_select_result = 'failed'\n"
        "try:\n"
        "  gui.simulate_keyboard_event(" + std::to_string(49 + slot) + ", 1)\n"
        "  gui.simulate_keyboard_event(" + std::to_string(49 + slot) + ", 0)\n"
        "  _infinitecz_map_select_result = 'switching'\n"
        "except BaseException: pass\n";
    std::string response;
    return PythonUtils::PyEvalUtf8(script, "_infinitecz_map_select_result", &response) &&
        response == "switching";
}

void MapInventoryTransfer::finishSession() noexcept {
    if (session_token_ == 0U) return;
    ProjectionPrinterInventorySessionCloseRequest close;
    if (GetProjectionPrinterInventorySessionCloseRequest(session_token_, &close) &&
        close.requires_client_close) {
        ExpectProjectionPrinterInventoryPresentationClose(close.container_id,
                                                           close.container_type);
        std::string ignored;
        if (ContainerClosePacketSender::send(close.container_id, close.container_type,
                                             &ignored)) {
            MarkProjectionPrinterInventorySessionCloseSent(
                session_token_, close.container_id, close.container_type);
        }
    }
    CancelProjectionPrinterInventorySession(session_token_);
    ClearProjectionPrinterInventoryPresentationGate();
    session_token_ = 0U;
}

void MapInventoryTransfer::cancel() noexcept {
    if (local_refresh_ticket_ != 0U) {
        CancelProjectionPrinterInventoryClientSyncTicket(local_refresh_ticket_);
    }
    if (correction_ticket_ != 0U) {
        CancelProjectionPrinterInventoryClientSyncTicket(correction_ticket_);
    }
    // This method is also reached from UI-thread pause/cancel. Native close
    // emission is left to the shared session quarantine's LocalPlayer tick.
    if (session_token_ != 0U) {
        CancelProjectionPrinterInventorySession(session_token_);
        ClearProjectionPrinterInventoryPresentationGate();
    }
    stage_ = Stage::Idle;
    target_kind_ = Kind::Empty;
    session_token_ = 0U;
    presentation_generation_before_open_ = 0U;
    presentation_generation_ = 0U;
    presentation_settled_ = false;
    close_ticks_remaining_ = 0;
    opened_at_ = {};
    next_action_at_ = {};
    move_sent_at_ = {};
    post_move_ready_at_ = {};
    move_ = {};
    request_id_ = 0;
    response_session_generation_ = 0;
    response_generation_before_send_ = 0;
    remote_revision_before_send_ = 0;
    local_refresh_ticket_ = 0;
    correction_ticket_ = 0;
    destination_count_before_ = 0;
    confirmed_destination_network_id_ = 0;
    named_map_active_ = false;
    named_map_ = {};
}

MapInventoryTransfer::Result MapInventoryTransfer::fail(
    std::string* error, const std::string& detail) {
    if (error) *error = detail;
    cancel();
    return Result::Failed;
}

MapInventoryTransfer::Result MapInventoryTransfer::tick(
    Kind kind, int32_t exact_network_stack_id,
    ReadyItem* output, std::string* error,
    const NamedFilledMapIdentity* named_map) {
    if (output) *output = {};
    if (error) error->clear();
    const auto now = Clock::now();

    if (named_map) {
        if (kind != Kind::Filled || exact_network_stack_id <= 0 ||
            named_map->runtime_item_id <= 0 || named_map->map_uuid == -1 ||
            named_map->title.empty() ||
            (named_map_active_ &&
             (named_map_.runtime_item_id != named_map->runtime_item_id ||
              named_map_.map_uuid != named_map->map_uuid ||
              named_map_.title != named_map->title))) {
            return fail(error, "改名地图热栏交换的身份发生变化；已暂停");
        }
        if (!named_map_active_) {
            if (stage_ != Stage::Idle) {
                return fail(error, "改名地图热栏交换与上一任务重叠；已暂停");
            }
            named_map_ = *named_map;
            named_map_active_ = true;
        }
    } else if (named_map_active_) {
        return fail(error, "改名地图热栏交换丢失了原生身份；已暂停");
    }

    if (stage_ == Stage::Closing) {
        if (close_ticks_remaining_ > 0U) {
            --close_ticks_remaining_;
            return Result::Waiting;
        }
        finishSession();
        stage_ = Stage::PostMoveReady;
        post_move_ready_at_ = now;
        return Result::Waiting;
    }

    if (stage_ == Stage::PostMoveReady) {
        if (now - post_move_ready_at_ >= kPostMoveReadyTimeout) {
            return fail(error, "地图换入热栏后原生槽位或选中状态未及时同步；已暂停");
        }
        const int32_t slot = move_.destination_hotbar_slot;
        if (slot < 0 || slot > 8) {
            return fail(error, "地图换入后的热栏槽位无效");
        }
        ProjectionPrinterLiveInventorySnapshot live;
        if (!ReadProjectionPrinterLiveInventorySnapshot(&live, nullptr) || !live.ready ||
            !sameNativeStack(live.slots[static_cast<size_t>(slot)], true,
                             confirmed_destination_network_id_)) {
            return Result::Waiting;
        }
        std::string verify_error;
        if (!VerifyProjectionPrinterSelectedHotbarItem(
                slot, confirmed_destination_network_id_, &verify_error)) {
            if (!requestSelection(slot)) {
                return fail(error, "无法切换到刚从背包取出的地图");
            }
            return Result::Waiting;
        }
        if (named_map_active_ &&
            !sameNamedMap(named_map_, slot,
                          confirmed_destination_network_id_, nullptr)) {
            return Result::Waiting;
        }
        // The source kind/count was checked before Move and the exact server
        // response (or independent remote-slot revision for status 4) proved
        // this destination. The embedded Python item cache can lag behind the
        // native hotbar for several ticks, so it is not a second requirement.
        if (output) *output = {slot, confirmed_destination_network_id_,
                               move_.expected_source_count};
        stage_ = Stage::Idle;
        named_map_active_ = false;
        named_map_ = {};
        return Result::Ready;
    }

    if (stage_ == Stage::MovePending) {
        if (now - move_sent_at_ >= kMoveTimeout) {
            return fail(error, "地图已提交背包交换，但服务端结果未确认；已暂停避免重复移动");
        }
        ProjectionPrinterInventoryResponse response;
        if (!GetProjectionPrinterInventoryResponseByRequestId(request_id_, &response) ||
            response.session_generation != response_session_generation_ ||
            response.response_generation <= response_generation_before_send_) {
            return Result::Waiting;
        }
        int32_t confirmed_source_id = 0;
        int32_t confirmed_destination_id = 0;
        if (response.rejected) {
            if (!projection_inventory_evidence::isRequestCompletionFailure(
                    true, response.rejection_status)) {
                return fail(error, "地图背包交换被服务器拒绝；已暂停避免重复提交");
            }
            // Bedrock status 4 may be a screen-handler close failure *after*
            // Move/Swap committed. The optimistic client InventorySlot pair is
            // not server evidence. Accept only a newer remote-only mailbox
            // revision proving the exact source/destination slot transition.
            ProjectionPrinterInventorySnapshot remote;
            if (!GetProjectionPrinterInventorySnapshot(&remote) || !remote.ready ||
                remote.session_generation != response_session_generation_ ||
                remote.revision <= remote_revision_before_send_) {
                return Result::Waiting;
            }
            const auto& source =
                remote.slots[static_cast<size_t>(move_.source_inventory_slot)];
            const auto& destination =
                remote.slots[static_cast<size_t>(move_.destination_hotbar_slot)];
            if (!destination.occupied || !destination.has_network_stack_id ||
                destination.count != move_.expected_source_count ||
                destination.network_stack_id <= 0 ||
                source.occupied != move_.expected_destination_occupied ||
                source.count != (move_.expected_destination_occupied
                    ? destination_count_before_ : 0U) ||
                (move_.expected_destination_occupied &&
                 (!source.has_network_stack_id || source.network_stack_id <= 0)) ||
                (!move_.expected_destination_occupied &&
                 (source.has_network_stack_id || source.network_stack_id != 0))) {
                return Result::Waiting;
            }
            confirmed_source_id = source.network_stack_id;
            confirmed_destination_id = destination.network_stack_id;
        } else {
            if (!response.valid || response.layout ==
                    ProjectionPrinterInventoryResponseLayout::None) {
                return fail(error, "地图背包交换响应格式无法验证；已暂停");
            }
            const auto* destination = uniqueResponseSlot(
                response, kHotbarRequestContainer, move_.destination_hotbar_slot);
            const auto* source = uniqueResponseSlot(
                response, kInventoryRequestContainer, move_.source_inventory_slot);
            if (!destination || !source ||
                destination->count != move_.expected_source_count ||
                destination->network_stack_id <= 0 ||
                source->count != (move_.expected_destination_occupied
                    ? destination_count_before_ : 0U) ||
                (move_.expected_destination_occupied && source->network_stack_id <= 0) ||
                (!move_.expected_destination_occupied && source->network_stack_id != 0)) {
                return fail(error, "地图背包交换响应槽位与预期不符；已暂停");
            }
            confirmed_source_id = source->network_stack_id;
            confirmed_destination_id = destination->network_stack_id;
        }
        ProjectionPrinterLiveInventorySnapshot live;
        if (!ReadProjectionPrinterLiveInventorySnapshot(&live, nullptr) || !live.ready) {
            return Result::Waiting;
        }
        const auto& live_source = live.slots[static_cast<size_t>(move_.source_inventory_slot)];
        const auto& live_destination =
            live.slots[static_cast<size_t>(move_.destination_hotbar_slot)];
        const bool response_applied =
            sameNativeStack(live_source, move_.expected_destination_occupied,
                            confirmed_source_id) &&
            sameNativeStack(live_destination, true, confirmed_destination_id);
        if (!response_applied) {
            const bool optimistic_pair_intact =
                sameNativeStack(live_source, move_.expected_destination_occupied,
                                move_.expected_destination_network_stack_id) &&
                sameNativeStack(live_destination, true,
                                move_.expected_source_network_stack_id);
            if (!optimistic_pair_intact || local_refresh_ticket_ == 0U) {
                return fail(error, "地图交换后客户端槽位未能安全同步；已暂停");
            }
            if (correction_ticket_ == 0U) {
                std::string correction_error;
                if (!QueueProjectionPrinterInventoryClientSyncResponseCorrection(
                        local_refresh_ticket_, confirmed_source_id,
                        confirmed_destination_id, &correction_ticket_,
                        &correction_error)) {
                    return fail(error, correction_error.empty()
                        ? "地图交换后快捷栏刷新失败；已暂停" : correction_error);
                }
            }
            return Result::Waiting;
        }
        confirmed_destination_network_id_ = confirmed_destination_id;
        stage_ = Stage::Closing;
        close_ticks_remaining_ = kPostMoveCloseTicks;
        return Result::Waiting;
    }

    if (stage_ == Stage::Opening) {
        if (now - opened_at_ >= kOpenTimeout) {
            return fail(error, "静默背包未在限定时间内确认打开；已暂停");
        }
        ProjectionPrinterInventorySessionResult session;
        const auto state = PollProjectionPrinterInventorySession(session_token_, &session);
        if (state == ProjectionPrinterInventorySessionPollState::Failed ||
            state == ProjectionPrinterInventorySessionPollState::Closed ||
            state == ProjectionPrinterInventorySessionPollState::Inactive) {
            return fail(error, session.error.empty()
                ? "静默背包会话失效；已暂停" : session.error);
        }
        if (state != ProjectionPrinterInventorySessionPollState::Ready &&
            state != ProjectionPrinterInventorySessionPollState::WaitingForContent) {
            return Result::Waiting;
        }
        if (!presentation_settled_) {
            if (!GetProjectionPrinterInventoryPresentationGateCompletedGenerationAfter(
                    presentation_generation_before_open_, &presentation_generation_)) {
                return Result::Waiting;
            }
            presentation_settled_ = true;
            next_action_at_ = now + std::chrono::milliseconds(50);
            return Result::Waiting;
        }
        if (now < next_action_at_) return Result::Waiting;
        if (!HasProjectionPrinterInventoryPresentationGateCompleted(
                presentation_generation_)) {
            return fail(error, "背包界面拦截失效；已取消地图交换");
        }
        MapInventorySnapshot inventory;
        std::string snapshot_error;
        if (!ReadMapInventorySnapshot(&inventory, &snapshot_error) ||
            !inventory.ready || !inventory.network_ready) {
            return fail(error, snapshot_error.empty()
                ? "无法复核地图背包槽位；已取消交换" : snapshot_error);
        }
        const auto& source = inventory.slots[static_cast<size_t>(move_.source_inventory_slot)];
        const auto& destination =
            inventory.slots[static_cast<size_t>(move_.destination_hotbar_slot)];
        const bool source_kind_matches = target_kind_ == Kind::Empty
            ? source.empty_map : source.filled_map;
        if (!source_kind_matches || !source.native_occupied ||
            source.count != move_.expected_source_count ||
            source.network_stack_id != move_.expected_source_network_stack_id ||
            destination.occupied != move_.expected_destination_occupied ||
            destination.native_occupied != move_.expected_destination_occupied ||
            destination.network_stack_id != move_.expected_destination_network_stack_id) {
            return fail(error, "地图或热栏槽位在交换前发生变化；已取消交换");
        }
        if (named_map_active_) {
            std::string named_error;
            if (!sameNamedMap(named_map_, move_.source_inventory_slot,
                              move_.expected_source_network_stack_id,
                              &named_error)) {
                return fail(error, named_error.empty()
                    ? "改名地图的 UUID 或名称在背包交换前发生变化；已取消交换"
                    : named_error);
            }
        }
        ProjectionPrinterInventoryClientSyncPreparedMove prepared;
        std::string move_error;
        if (!PrepareProjectionPrinterLiveInventoryClientSyncMove(
                move_, &prepared, &move_error)) {
            return fail(error, move_error.empty()
                ? "无法准备地图快捷栏同步；已取消交换" : move_error);
        }
        response_session_generation_ =
            GetProjectionPrinterInventoryMailboxSessionGeneration();
        ProjectionPrinterInventorySnapshot remote_before;
        remote_revision_before_send_ =
            GetProjectionPrinterInventorySnapshot(&remote_before) &&
                remote_before.ready &&
                remote_before.session_generation == response_session_generation_
            ? remote_before.revision : 0U;
        ProjectionPrinterInventoryResponse previous;
        response_generation_before_send_ =
            GetProjectionPrinterInventoryResponse(&previous) &&
                previous.session_generation == response_session_generation_
            ? previous.response_generation : 0U;
        if (!MoveProjectionPrinterBackpackItemToHotbar(
                move_, &move_error, &request_id_)) {
            DiscardProjectionPrinterInventoryClientSyncPreparedMove(&prepared);
            return fail(error, move_error.empty()
                ? "地图无法安全换入热栏" : move_error);
        }
        if (request_id_ == 0) {
            DiscardProjectionPrinterInventoryClientSyncPreparedMove(&prepared);
            return fail(error, "地图交换请求编号无效；已暂停避免重复移动");
        }
        std::string refresh_error;
        if (!CommitProjectionPrinterInventoryClientSyncPreparedMove(
                &prepared, &local_refresh_ticket_, &refresh_error)) {
            return fail(error, refresh_error.empty()
                ? "地图交换已提交，但快捷栏刷新未入队；已暂停" : refresh_error);
        }
        move_sent_at_ = now;
        stage_ = Stage::MovePending;
        return Result::Waiting;
    }

    MapInventorySnapshot inventory;
    std::string snapshot_error;
    if (!ReadMapInventorySnapshot(&inventory, &snapshot_error) || !inventory.ready) {
        return fail(error, snapshot_error.empty()
            ? "无法读取玩家物品栏中的空白地图" : snapshot_error);
    }
    if (!inventory.network_ready) {
        return fail(error, "地图物品栏与游戏原生槽位不同步；已暂停");
    }
    int32_t named_slot = -1;
    if (named_map_active_) {
        ProjectionPrinterNativeInventoryFilledMapMatch exact;
        std::string named_error;
        if (!ReadProjectionPrinterNativeInventoryFilledMapMatch(
                named_map_.runtime_item_id, named_map_.map_uuid,
                named_map_.title, &exact, &named_error) ||
            exact.network_stack_id != exact_network_stack_id ||
            exact.inventory_slot < 0 || exact.inventory_slot >= 36) {
            return fail(error, named_error.empty()
                ? "改名地图的原生身份与交换源不一致；已暂停" : named_error);
        }
        named_slot = exact.inventory_slot;
    }
    target_kind_ = kind;
    int32_t map_hotbar_slot = -1;
    for (int32_t slot = 0; slot <= 8; ++slot) {
        const auto& item = inventory.slots[static_cast<size_t>(slot)];
        if (!(kind == Kind::Empty ? item.empty_map : item.filled_map) ||
            (exact_network_stack_id > 0 &&
             item.network_stack_id != exact_network_stack_id)) continue;
        if (slot == inventory.selected_hotbar_slot) {
            map_hotbar_slot = slot;
            break;
        }
        if (map_hotbar_slot < 0) map_hotbar_slot = slot;
    }
    if (map_hotbar_slot >= 0) {
        if (named_map_active_ && map_hotbar_slot != named_slot) {
            return fail(error, "改名地图热栏槽位与原生身份不一致；已暂停");
        }
        const auto& item = inventory.slots[static_cast<size_t>(map_hotbar_slot)];
        if (!item.native_occupied || !item.has_network_stack_id ||
            item.network_stack_id <= 0) {
            return fail(error, "空白地图的原生物品标识不可用；已暂停");
        }
        if (inventory.selected_hotbar_slot != map_hotbar_slot) {
            if (!requestSelection(map_hotbar_slot)) {
                return fail(error, "无法切换到空白地图所在的热栏槽位");
            }
            return Result::Waiting;
        }
        std::string verify_error;
        if (!VerifyProjectionPrinterSelectedHotbarItem(
                map_hotbar_slot, item.network_stack_id, &verify_error)) {
            return fail(error, verify_error.empty()
                ? "所选空白地图在发包前发生变化" : verify_error);
        }
        if (output) *output = {map_hotbar_slot, item.network_stack_id, item.count};
        named_map_active_ = false;
        named_map_ = {};
        return Result::Ready;
    }
    int32_t map_backpack_slot = -1;
    for (int32_t slot = 9; slot <= 35; ++slot) {
        const auto& item = inventory.slots[static_cast<size_t>(slot)];
        const bool kind_matches = kind == Kind::Empty ? item.empty_map : item.filled_map;
        if (kind_matches &&
            (exact_network_stack_id == 0 ||
             item.network_stack_id == exact_network_stack_id) &&
            item.native_occupied && item.has_network_stack_id &&
            item.network_stack_id > 0 && item.count > 0 && item.count <= 255U) {
            map_backpack_slot = slot;
            break;
        }
    }
    if (map_backpack_slot < 0) return Result::Missing;
    if (named_map_active_ && map_backpack_slot != named_slot) {
        return fail(error, "改名地图背包槽位与原生身份不一致；已暂停");
    }
    if (ProjectionPrinterRuntime::instance().enabled()) {
        return fail(error, "投影打印机仍在运行；请关闭打印机后继续自动制图");
    }
    int32_t destination_slot = -1;
    for (int32_t slot = 0; slot <= 8; ++slot) {
        if (slot == inventory.selected_hotbar_slot) continue;
        const auto& item = inventory.slots[static_cast<size_t>(slot)];
        if (!item.occupied && !item.native_occupied) {
            destination_slot = slot;
            break;
        }
    }
    if (destination_slot < 0) {
        for (int32_t slot = 0; slot <= 8; ++slot) {
            if (slot == inventory.selected_hotbar_slot) continue;
            const auto& item = inventory.slots[static_cast<size_t>(slot)];
            if (item.occupied && item.native_occupied && item.has_network_stack_id &&
                item.network_stack_id > 0) {
                destination_slot = slot;
                break;
            }
        }
    }
    if (destination_slot < 0) {
        return fail(error, "没有可安全交换的非当前热栏槽位");
    }
    if (!IsProjectionPrinterInventoryPresentationGateInstalled() ||
        !IsProjectionPrinterInventoryPresentationGateReadyForOpen()) {
        return fail(error, "背包静默界面拦截未就绪；不会弹出背包界面");
    }
    const uint64_t token = nextSessionToken();
    if (!ArmProjectionPrinterInventorySession(token)) return Result::Waiting;
    session_token_ = token;
    const auto& source = inventory.slots[static_cast<size_t>(map_backpack_slot)];
    const auto& destination = inventory.slots[static_cast<size_t>(destination_slot)];
    move_.source_inventory_slot = map_backpack_slot;
    move_.destination_hotbar_slot = destination_slot;
    move_.expected_source_network_stack_id = source.network_stack_id;
    move_.expected_source_count = source.count;
    move_.expected_destination_occupied = destination.occupied;
    move_.expected_destination_network_stack_id = destination.network_stack_id;
    destination_count_before_ = destination.count;
    presentation_generation_before_open_ =
        GetProjectionPrinterInventoryPresentationGateArmGeneration();
    std::string open_error;
    if (!PlayerInventoryOpenPacketSender::send(&open_error)) {
        return fail(error, open_error.empty()
            ? "无法静默打开玩家背包以获取空白地图" : open_error);
    }
    opened_at_ = now;
    stage_ = Stage::Opening;
    return Result::Waiting;
}

}  // namespace build_import
