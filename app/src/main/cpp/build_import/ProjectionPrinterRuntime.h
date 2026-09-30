#ifndef INFINITE_TEXTURE_PROJECTION_PRINTER_RUNTIME_H
#define INFINITE_TEXTURE_PROJECTION_PRINTER_RUNTIME_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace build_import {

// This numeric layout is consumed by the injected Kotlin overlay. Keep the
// values stable: a disabled printer is distinct from an enabled printer which
// simply has no reachable work at the current player position.
enum class ProjectionPrinterState : uint8_t {
    Off = 0,
    Idle = 1,
    Running = 2,
    WaitingMaterial = 3,
    WaitingSupport = 4,
    Complete = 5,
    Error = 6,
};

// A read-only inventory view for the material-list overlay.  Entries come from
// the player's ordinary 36-slot inventory (0..8 hotbar and 9..35 backpack),
// and are refreshed only on the verified local-player game tick.  JNI/UI
// callers therefore never access the game item component directly.
enum class ProjectionPrinterMaterialInventoryStatus : uint8_t {
    Pending,
    Ready,
    Unavailable,
};

struct ProjectionPrinterMaterialInventoryEntry {
    std::string item_name;
    uint16_t item_aux = 0;
    uint64_t count = 0;
};

struct ProjectionPrinterMaterialInventorySnapshot {
    ProjectionPrinterMaterialInventoryStatus status =
        ProjectionPrinterMaterialInventoryStatus::Pending;
    uint64_t revision = 0;
    std::vector<ProjectionPrinterMaterialInventoryEntry> entries;
};

// Game-thread runtime for the projection printer. It deliberately issues at
// most one normal ItemUse placement at a time and waits for NativeWorldReader
// to observe the resulting world block before it counts the placement. The
// runtime never changes or breaks an already occupied target coordinate.
class ProjectionPrinterRuntime {
public:
    static ProjectionPrinterRuntime& instance();

    void setEnabled(bool enabled);
    bool enabled() const;
    void setRate(int32_t blocks_per_second);
    int32_t rate() const;

    // Controls only the read-only visual preview of nearby targets that the
    // printer could legally place right now. It may run while automatic
    // printing is off and never sends packets or changes inventory state.
    void setReachabilityPreviewEnabled(bool enabled);
    bool reachabilityPreviewEnabled() const;

    ProjectionPrinterState state() const;
    std::string status() const;
    uint64_t totalBlocks() const;
    uint64_t placedBlocks() const;
    uint64_t skippedBlocks() const;

    // Returns the latest safe inventory cache and asks the game tick to refresh
    // it.  This is intentionally non-blocking: the caller may be a Kotlin UI
    // worker thread, while the item component can only be read on game tick.
    void queryMaterialInventorySnapshot(
        ProjectionPrinterMaterialInventorySnapshot* output);

    // Must only be called from the verified local-player game tick.
    void onGameTick();
    // Cancels pending interaction and invalidates all state derived from the
    // active projection. Safe for projection reload, clear, and revocation.
    void clear();

private:
    ProjectionPrinterRuntime() = default;
    ~ProjectionPrinterRuntime();
    ProjectionPrinterRuntime(const ProjectionPrinterRuntime&) = delete;
    ProjectionPrinterRuntime& operator=(const ProjectionPrinterRuntime&) = delete;

    struct RuntimeState;
    RuntimeState* stateData();
    const RuntimeState* stateData() const;

    mutable std::mutex mutex_;
    std::unique_ptr<RuntimeState> runtime_;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_PROJECTION_PRINTER_RUNTIME_H
