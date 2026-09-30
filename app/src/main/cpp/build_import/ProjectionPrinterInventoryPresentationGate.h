#ifndef INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_PRESENTATION_GATE_H
#define INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_PRESENTATION_GATE_H

#include <cstdint>
#include <string_view>

namespace build_import {

// Installs the fingerprinted ContainerOpen(type=0xFF) UI-call gate. It skips
// only a printer-owned call to LocalPlayer::openInventory and returns success
// to the existing packet handler. It never creates an invisible screen or
// changes ScreenSettings; all other stock packet handling remains intact.
bool InitProjectionPrinterInventoryPresentationGate(uintptr_t minecraft_base) noexcept;
bool IsProjectionPrinterInventoryPresentationGateInstalled() noexcept;

// A readiness probe only. The raw receive hook arms after identifying the
// exact printer-owned ContainerOpen, so manual inventory opens stay stock.
bool IsProjectionPrinterInventoryPresentationGateReadyForOpen() noexcept;

// Bind the one-shot generation to this already-validated wire packet's actual
// window ID/type. The UI-call gate matches those bytes in the native handler
// and verifies the LocalPlayer virtual target before suppressing presentation.
// Ticket lifetime follows the owning session, including explicit cancellation.
bool ArmProjectionPrinterInventoryPresentationGate(std::string_view packet) noexcept;

// Successful arms receive process-monotonic generations. Runtime captures the
// latest value before sending OpenInventory and only accepts newer completion.
uint64_t GetProjectionPrinterInventoryPresentationGateArmGeneration() noexcept;

// Completion means the exact printer-owned UI call was cancelled; it does not
// require screen creation or a stock ScreenManager handoff.
bool HasProjectionPrinterInventoryPresentationGateCompleted(uint64_t arm_generation) noexcept;
bool GetProjectionPrinterInventoryPresentationGateCompletedGenerationAfter(
    uint64_t previous_generation, uint64_t* output_generation) noexcept;

// Compatibility lifecycle notifications. No screen is created/owned by this
// gate, so protocol close state is managed only by the inventory session.
void ExpectProjectionPrinterInventoryPresentationClose(uint8_t container_id,
                                                       uint8_t container_type) noexcept;
void ObserveProjectionPrinterInventoryPresentationPacket(std::string_view packet) noexcept;

// Invalidate this generation and any outstanding ticket without touching game
// UI or inventory objects. A stale completed generation cannot authorize reuse.
void ClearProjectionPrinterInventoryPresentationGate() noexcept;
void ServiceProjectionPrinterInventoryPresentationGateOnGameThread() noexcept;

}  // namespace build_import

#endif  // INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_PRESENTATION_GATE_H
