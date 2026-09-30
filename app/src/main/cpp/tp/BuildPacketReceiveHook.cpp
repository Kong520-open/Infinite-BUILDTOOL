#include "BuildPacketReceiveHook.h"

#include "../build_import/BuildImportRuntime.h"
#include "../build_import/ContainerCaptureMailbox.h"
#include "../build_import/ItemRuntimeRegistry.h"
#include "../build_import/MapAnvilDebugBridge.h"
#include "../build_import/MapAnvilClientSyncDelivery.h"
#include "../build_import/MapChestClientSyncDelivery.h"
#include "../build_import/MapTextureObservation.h"
#include "../build_import/ProjectionPrinterInventoryClientSync.h"
#include "../build_import/ProjectionPrinterInventoryDiagnostics.h"
#include "../build_import/ProjectionPrinterInventoryMailbox.h"
#include "../build_import/ProjectionPrinterInventoryPresentationGate.h"
#include "../build_import/ProjectionPrinterInventorySession.h"
#include "../build_import/ProjectionPrinterNativeHotbarSelection.h"
#include "../build_import/SignEditSessionMailbox.h"
#include "../log_control.h"
#include "FunctionsAddress.h"
#include "MinecraftUpdateHook.h"
#include "dobby.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#define LOG_TAG "NetworkReceiveHook"

namespace {

using NewReceivePacket = int (*)(void*, std::string&, void*);

std::atomic<bool> g_receiveHookInstalled{false};
std::atomic<uint64_t> g_receiveHookCallCount{0};
std::mutex g_hookInstallMutex;
NewReceivePacket g_originalReceivePacket = nullptr;

constexpr size_t kMaxSuppressedInternalPacketsPerPoll = 256U;

bool tryDeliverProjectionPrinterClientSync(void* connection, void* networkSystem,
                                           std::string& packet) noexcept {
    try {
        // This hook is shared by client and integrated-server receive loops.
        // Only the first-argument instance that delivered the printer's
        // matched ContainerOpen may drain its client-bound slot updates.
        // The third argument remains scratch state, never an identity gate.
        if (!connection) {
            return false;
        }
        build_import::ProjectionPrinterInventoryClientSyncQueuedPacket queued;
        if (!build_import::TakeProjectionPrinterInventoryClientSyncPacket(connection, &queued)) {
            return false;
        }
        if (queued.bytes.empty()) {
            build_import::LogProjectionPrinterInventoryDiagnostic(
                "local_deliver_cancel ticket=%llu index=%u reason=empty_packet",
                static_cast<unsigned long long>(queued.ticket),
                static_cast<unsigned>(queued.packet_index));
            build_import::CancelProjectionPrinterInventoryClientSyncTicket(queued.ticket);
            return false;
        }
        packet.swap(queued.bytes);
        // Returning 0 hands this packet to the stock receive dispatcher. Do
        // not run it through our observers or suppression loop: it is a local
        // presentation update, not a fresh server inventory acknowledgement.
        const bool completed = build_import::CompleteProjectionPrinterInventoryClientSyncPacket(
            queued.ticket, queued.packet_index);
        build_import::LogProjectionPrinterInventoryDiagnostic(
            "local_deliver ticket=%llu index=%u role=%s bytes=%zu complete=%d "
            "connection=%p scratch=%p game_thread=%d ingress=bound_client_fifo",
            static_cast<unsigned long long>(queued.ticket),
            static_cast<unsigned>(queued.packet_index),
            queued.packet_index == 0U ? "source" : "destination", packet.size(),
            completed ? 1 : 0, connection, networkSystem, IsMinecraftUpdateGameThread() ? 1 : 0);
        return true;
    } catch (...) {
        // Network hooks must never unwind into the game client.
        return false;
    }
}

bool tryDeliverMapAnvilClientSync(void* connection, std::string& packet) noexcept {
    try {
        if (!connection) return false;
        build_import::MapAnvilClientSyncQueuedPacket queued;
        if (!build_import::TakeMapAnvilClientSyncPacket(connection, &queued)) {
            return false;
        }
        if (queued.bytes.empty()) {
            build_import::ClearMapAnvilClientSync();
            return false;
        }
        if (build_import::GetMapAnvilClientSyncTicketState(queued.ticket) !=
                build_import::MapAnvilClientSyncTicketState::Pending) {
            return false;
        }
        packet.swap(queued.bytes);
        const bool completed = build_import::CompleteMapAnvilClientSyncPacket(
            queued.ticket, queued.packet_index);
        if (!completed) {
            packet.clear();
            return false;
        }
        LOGI("[map-anvil-local-sync] deliver ticket=%llu index=%u bytes=%zu complete=%d",
             static_cast<unsigned long long>(queued.ticket),
             static_cast<unsigned>(queued.packet_index), packet.size(),
             completed ? 1 : 0);
        return true;
    } catch (...) {
        return false;
    }
}

bool tryDeliverMapChestClientSync(void* connection, std::string& packet) noexcept {
    try {
        if (!connection) return false;
        build_import::MapChestClientSyncQueuedPacket queued;
        if (!build_import::TakeMapChestClientSyncPacket(connection, &queued)) {
            return false;
        }
        if (queued.bytes.empty()) {
            build_import::ClearMapChestClientSync();
            return false;
        }
        if (build_import::GetMapChestClientSyncTicketState(queued.ticket) !=
                build_import::MapChestClientSyncTicketState::Pending) {
            return false;
        }
        packet.swap(queued.bytes);
        // This is a local stock-client presentation update for an already
        // accepted Place, not another server packet or inventory action.
        if (!build_import::CompleteMapChestClientSyncPacket(queued.ticket)) {
            packet.clear();
            return false;
        }
        LOGI("[map-chest-local-sync] deliver ticket=%llu bytes=%zu connection=%p",
             static_cast<unsigned long long>(queued.ticket), packet.size(),
             connection);
        return true;
    } catch (...) {
        return false;
    }
}

int HookReceivePacket(void* connection, std::string& packet, void* networkSystem) {
    if (tryDeliverProjectionPrinterClientSync(connection, networkSystem, packet)) {
        return 0;
    }
    if (tryDeliverMapAnvilClientSync(connection, packet)) return 0;
    if (tryDeliverMapChestClientSync(connection, packet)) return 0;
    const uint64_t callIndex =
        g_receiveHookCallCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool sampleCall = callIndex <= 3 || (callIndex & 0xFFU) == 0;
    if (sampleCall) {
        LOGI("[recv] entered #%llu conn=%p ns=%p",
             static_cast<unsigned long long>(callIndex), connection, networkSystem);
    }

    int result = 1;
    if (g_originalReceivePacket) {
        result = g_originalReceivePacket(connection, packet, networkSystem);
    } else {
        LOGI("[recv] original function is null");
    }
    if (sampleCall) {
        LOGI("[recv] original #%llu result=%d bytes=%zu",
             static_cast<unsigned long long>(callIndex), result, packet.size());
    }

    if (result == 0) {
        size_t suppressedCount = 0U;
        for (;;) {
            // Diagnostic only: inspect manual chest traffic before the
            // automatic capture gate decides whether it owns this packet.
            // This observer never edits packet or requests suppression.
            build_import::ObserveManualChestTracePacket(packet);
            build_import::ObserveItemRuntimeRegistryPacket(packet);
            build_import::ObserveMapTexturePacket(packet);
            build_import::ObserveProjectionPrinterInventoryClientSyncPacket(packet);
            // The printer observers are deliberately passive for every packet
            // except a printer-owned ContainerOpen. InventoryContent, Slot and
            // ItemStackResponse must still travel through the stock client so
            // its real HUD/net-stack model observes a confirmed material move.
            build_import::ObserveProjectionPrinterInventoryPacket(packet);
            build_import::ObserveMapAnvilDebugInbound(packet);
            build_import::ObserveProjectionPrinterInventoryPresentationPacket(packet);
            const auto printer_open =
                build_import::ObserveProjectionPrinterInventorySessionPacket(packet);
            bool suppressPrinterOpen = false;
            switch (printer_open) {
                case build_import::ProjectionPrinterInventorySessionReceiveDisposition::ActiveOpen:
                    build_import::LogProjectionPrinterInventoryDiagnostic(
                        "open_receive disposition=active bytes=%zu connection=%p scratch=%p game_thread=%d",
                        packet.size(), connection, networkSystem, IsMinecraftUpdateGameThread() ? 1 : 0);
                    // Arm only after the wire packet has been proven to be the
                    // printer's response. Arming before sending OpenInventory
                    // could accidentally hide a player-opened backpack.
                    if (!build_import::ArmProjectionPrinterInventoryPresentationGate(packet)) {
                        build_import::LogProjectionPrinterInventoryDiagnostic(
                            "open_presentation_arm success=0");
                        build_import::FailProjectionPrinterInventorySessionPresentationGate();
                        suppressPrinterOpen = true;
                    } else {
                        build_import::BindProjectionPrinterInventoryClientSyncIngress(connection);
                        build_import::LogProjectionPrinterInventoryDiagnostic(
                            "client_ingress_bind connection=%p", connection);
                    }
                    break;
                case build_import::ProjectionPrinterInventorySessionReceiveDisposition::QuarantinedOpen:
                    build_import::LogProjectionPrinterInventoryDiagnostic(
                        "open_receive disposition=quarantined");
                    // A cancelled request may still receive a late window.
                    // Keep only that exact tail from reopening game UI.
                    suppressPrinterOpen = true;
                    break;
                case build_import::ProjectionPrinterInventorySessionReceiveDisposition::WorldReset:
                    // The session observer discarded its active/quarantine
                    // state. Release the independent hidden-screen gate too so
                    // an old world can never retain a presentation ticket.
                    build_import::ClearProjectionPrinterInventoryPresentationGate();
                    build_import::ClearMapAnvilClientSync();
                    build_import::ClearMapChestClientSync();
                    break;
                case build_import::ProjectionPrinterInventorySessionReceiveDisposition::Pass:
                    break;
            }
            build_import::VisibleAnvilCaptureEvent anvil_event;
            build_import::VisibleChestCaptureEvent chest_event;
            const bool suppressContainer =
                build_import::ObserveContainerCapturePacket(
                    packet, &anvil_event, &chest_event);
            if (anvil_event.kind ==
                    build_import::VisibleAnvilCaptureEventKind::Open &&
                !suppressPrinterOpen && !suppressContainer) {
                build_import::BindMapAnvilClientSyncIngress(
                    connection, anvil_event.token, anvil_event.window_id);
            } else if (anvil_event.kind ==
                       build_import::VisibleAnvilCaptureEventKind::Close) {
                build_import::CancelMapAnvilClientSyncWindow(anvil_event.token);
            }
            if (chest_event.kind ==
                    build_import::VisibleChestCaptureEventKind::Open &&
                !suppressPrinterOpen && !suppressContainer) {
                build_import::BindMapChestClientSyncIngress(
                    connection, chest_event.token, chest_event.window_id);
            } else if (chest_event.kind ==
                       build_import::VisibleChestCaptureEventKind::Close) {
                build_import::CancelMapChestClientSyncWindow(chest_event.token);
            }
            const bool suppressSign =
                build_import::ObserveSignEditSessionPacket(packet);
            if (!suppressPrinterOpen && !suppressContainer && !suppressSign &&
                !ObserveBuildExportTeleportPacket(packet) &&
                !build_import::BuildImportRuntime::instance().onRawNetworkPacket(packet)) {
                break;
            }

            packet.clear();
            if (++suppressedCount >= kMaxSuppressedInternalPacketsPerPoll ||
                !g_originalReceivePacket) {
                result = 1;
                break;
            }
            result = g_originalReceivePacket(connection, packet, networkSystem);
            if (result != 0) {
                packet.clear();
                break;
            }
        }
    }

    return result;
}

}  // namespace

bool BuildPacketReceiveHook::init(uintptr_t baseAddress) {
    if (baseAddress == 0) return false;

    std::lock_guard<std::mutex> installLock(g_hookInstallMutex);
    if (!g_receiveHookInstalled.load(std::memory_order_acquire)) {
        const uintptr_t receiveAddress =
            baseAddress + FunctionsAddress::newReceivePacket_Hook;
        LOGI("installing newReceivePacket hook at 0x%lx",
             static_cast<unsigned long>(receiveAddress));
        const int result = DobbyHook(
            reinterpret_cast<void*>(receiveAddress),
            reinterpret_cast<void*>(HookReceivePacket),
            reinterpret_cast<void**>(&g_originalReceivePacket));
        const bool ready = result == 0 && g_originalReceivePacket != nullptr;
        g_receiveHookInstalled.store(ready, std::memory_order_release);
        LOGI("newReceivePacket hook ready=%d", ready ? 1 : 0);
    }
    // Optional and fail-closed: this retains the real HUD model so printer
    // material selection can switch between already-visible hotbar slots.
    build_import::InitProjectionPrinterNativeHotbarSelection(baseAddress);
    // This uses the receive hook's already validated version-specific entry
    // points. The printer requires the ContainerOpen UI-handler cancellation
    // gate before opening a silent inventory session.
    if (isReceiveHookReady()) {
        build_import::InitMapAnvilDebugBridge();
        (void)build_import::InitProjectionPrinterInventoryPresentationGate(baseAddress);
    }
    return isReceiveHookReady();
}

bool BuildPacketReceiveHook::isReceiveHookReady() {
    return g_receiveHookInstalled.load(std::memory_order_acquire) &&
        g_originalReceivePacket != nullptr;
}
