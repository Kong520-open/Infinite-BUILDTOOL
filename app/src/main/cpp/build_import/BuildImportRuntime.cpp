#include "BuildImportRuntime.h"
#include "BlockCommandFormatter.h"
#include "BlockMapper.h"
#include "BuildImportUndoStore.h"
#include "BuildImportThroughput.h"
#include "CommandBlockPacketSender.h"
#include "CommandBlockWriteGeometry.h"
#include "ContainerEntityCodec.h"
#include "ContainerOpenPacketSender.h"
#include "ExecuteCommandConverter.h"
#include "InfiniteczBuildParser.h"
#include "MapBlankSupplyJournal.h"
#include "MapChestTransferJournal.h"
#include "MapChestWorldPlacement.h"
#include "MapChestProductionPreflight.h"
#include "MapChestNativeSurvey.h"
#include "MapAnvilRenameJournal.h"
#include "MapAnvilClientSyncDelivery.h"
#include "MapChestClientSyncDelivery.h"
#include "MapExtraChestPlacementJournal.h"
#include "MapPairPlacementJournal.h"
#include "MapStorageCursorCommit.h"
#include "MapStorageProductionActions.h"
#include "MapTileNaming.h"
#include "MapNativeAnvilManagerProbe.h"
#include "MapAnvilUiCloseBridge.h"
#include "MapChestUiCloseBridge.h"
#include "MapNativeAnvilNameBridge.h"
#include "MapNativeAnvilResultActionBridge.h"
#include "MapNativeHeldMapEvidence.h"
#include "MapTextureObservation.h"
#include "NativeWorldAccess.h"
#include "ProjectionPrinterInventoryMailbox.h"
#include "ProjectionPrinterInventoryMover.h"
#include "PyRpcAckDecoder.h"
#include "PyRpcEnvelopeBridge.h"
#include "PyRpcPointerBridge.h"
#include "SignBlockActorPacketSender.h"
#include "SignEditSessionMailbox.h"
#include "SignEntityCodec.h"

#include "../tp/PythonUtils.h"
#include "../tp/MinecraftUpdateHook.h"
#include "../main.h"

#define LOG_TAG "Infinitecz_BuildImport"
#include "../log_control.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#if !defined(_WIN32)
#include <fcntl.h>
#endif
#include <functional>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <string_view>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>

#if defined(__ANDROID__) && defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
#include <sys/system_properties.h>
#endif

#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace build_import {
namespace {

constexpr int32_t kNativeChunkSize = 16;
constexpr auto kRpcPollInterval = std::chrono::milliseconds(40);
constexpr auto kRpcBarrierTimeout = std::chrono::seconds(15);
// Ticking-area removals are idempotent and precede the next prepare command
// in the same ordered RPC stream. Some game clients execute them but never
// expose the probe ACK; do not block every import on that missing response.
constexpr auto kCleanupAckGrace = std::chrono::seconds(2);
constexpr auto kUnconfirmedCleanupSettle = std::chrono::milliseconds(350);
constexpr auto kNativeReadabilityFallbackGrace = std::chrono::seconds(2);
constexpr auto kRpcProbeTimeout = std::chrono::seconds(2);
constexpr uint32_t kMaxCleanupBarrierFailures = 4;
constexpr uint32_t kMaxCommandFeedbackSetupFailures = 4;
constexpr uint64_t kMaxUnbarrieredCommands = 4096;
constexpr uint64_t kMaxUnbarrieredBlocks = 524288;
constexpr auto kMaxUnbarrieredAge = std::chrono::milliseconds(6000);
// Normal placement deliberately runs without temporal pacing.  The remaining
// 1024-command batch size and game-tick work budget are transport/frame safety
// bounds only: they split one already-authorized RPC call into manageable
// payloads, but never wait for elapsed time, tokens, or a server queue ACK.
constexpr bool kDataQueueBarriersEnabled = false;
constexpr bool kPhaseSettleWaitingEnabled = false;
// The verified anvil/chest flow is part of automatic map creation in both APK
// variants. Do not gate it on a volatile debug system property: those values
// disappear after a device reboot, silently reverting to maps without names
// or chest storage. Exact window, item, world, and journal checks remain in
// the storage driver before any irreversible request is sent.
bool mapStoragePipelineEnabled() noexcept {
#if defined(__ANDROID__) && defined(__aarch64__) && \
    defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
    return true;
#else
    return false;
#endif
}

bool debugPreexistingMapPair(MapChestAnvilPosition* pair) noexcept {
#if defined(__ANDROID__) && defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
    if (!pair) return false;
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.infinitecz.map_test_pair", value) <= 0)
        return false;
    long long coordinates[6]{};
    int consumed = 0;
    if (std::sscanf(value, "%lld,%lld,%lld:%lld,%lld,%lld%n",
                    &coordinates[0], &coordinates[1], &coordinates[2],
                    &coordinates[3], &coordinates[4], &coordinates[5],
                    &consumed) != 6 || consumed <= 0 ||
        static_cast<size_t>(consumed) != std::strlen(value)) return false;
    for (const long long coordinate : coordinates) {
        if (coordinate < std::numeric_limits<int32_t>::min() ||
            coordinate > std::numeric_limits<int32_t>::max()) return false;
    }
    if (coordinates[1] != coordinates[4] ||
        std::llabs(coordinates[0] - coordinates[3]) +
            std::llabs(coordinates[2] - coordinates[5]) != 1) return false;
    pair->chest = {static_cast<int32_t>(coordinates[0]),
                   static_cast<int32_t>(coordinates[1]),
                   static_cast<int32_t>(coordinates[2])};
    pair->anvil = {static_cast<int32_t>(coordinates[3]),
                   static_cast<int32_t>(coordinates[4]),
                   static_cast<int32_t>(coordinates[5])};
    return true;
#else
    (void)pair;
    return false;
#endif
}

uint64_t mapStorageMonotonicMs(std::chrono::steady_clock::time_point now) noexcept {
    const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return ticks < 0 ? 0U : static_cast<uint64_t>(ticks);
}

std::string parentDirectory(std::string path) {
    while (path.size() > 1U && (path.back() == '/' || path.back() == '\\')) {
        path.pop_back();
    }
    const size_t separator = path.find_last_of("/\\");
    if (separator == std::string::npos) return {};
    if (separator == 0U) return path.substr(0, 1U);
    return path.substr(0, separator);
}

std::string fileName(const std::string& path) {
    const size_t separator = path.find_last_of("/\\");
    return separator == std::string::npos ? path : path.substr(separator + 1U);
}

std::string undoStorageDirectoryForSpool(const std::string& spool_directory) {
    const std::string parent = parentDirectory(spool_directory);
    if (parent.empty()) return {};
    if (fileName(parent) == "jobs") return parentDirectory(parent);
    return parent;
}

std::atomic<uint64_t> g_undo_record_sequence{0};

std::string makeUndoRecordId(const std::string& job_id) {
    const uint64_t sequence =
        g_undo_record_sequence.fetch_add(1, std::memory_order_relaxed) + 1U;
    const uint64_t wall_clock = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const uint64_t job_hash = [&]() {
        uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char byte : job_id) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        return hash;
    }();
    char buffer[128]{};
    std::snprintf(buffer, sizeof(buffer), "undo-%016llx-%016llx-%016llx",
                  static_cast<unsigned long long>(job_hash),
                  static_cast<unsigned long long>(wall_clock),
                  static_cast<unsigned long long>(sequence));
    return buffer;
}

bool directoryExists(const std::string& path) {
    struct stat status {};
    return !path.empty() && stat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode);
}

bool addBoundsVolume(const BlockBounds& bounds, uint64_t* total) {
    if (!total || !bounds.isValid()) return false;
    const uint64_t width = static_cast<uint64_t>(
        static_cast<int64_t>(bounds.max_x) - bounds.min_x + 1);
    const uint64_t height = static_cast<uint64_t>(
        static_cast<int64_t>(bounds.max_y) - bounds.min_y + 1);
    const uint64_t depth = static_cast<uint64_t>(
        static_cast<int64_t>(bounds.max_z) - bounds.min_z + 1);
    if (width != 0U && height > std::numeric_limits<uint64_t>::max() / width) return false;
    const uint64_t area = width * height;
    if (area != 0U && depth > std::numeric_limits<uint64_t>::max() / area) return false;
    const uint64_t volume = area * depth;
    if (volume > std::numeric_limits<uint64_t>::max() - *total) return false;
    *total += volume;
    return true;
}
constexpr size_t kUnthrottledPlacementBatchCommands = 1024;
constexpr uint32_t kMaxSchedulerTransitionsPerTick = 64;
constexpr uint32_t kMaxDeferredWindowUnits = 256;
// At most this many completed regions may keep their ticking area alive
// through a held (settling) removal at once. Two held areas plus the active
// one stay far below the server's ticking-area allowance; beyond the cap the
// scheduler falls back to the classic blocking settle.
constexpr size_t kMaxHeldRegionCleanups = 2;
constexpr auto kCommandBatchBuildBudget = std::chrono::milliseconds(12);
constexpr auto kImportSchedulerTickBudget = std::chrono::milliseconds(12);
constexpr size_t kCommandBatchDeadlineCheckStride = 8;
constexpr const char* kRpcSentCountEnvironment = "INFINITECZ_BUILD_RPC_SENT";
constexpr const char* kRpcAckEnvironment = "INFINITECZ_BUILD_RPC_ACKS";
constexpr const char* kRpcSendDebugEnvironment = "INFINITECZ_BUILD_RPC_SEND_DEBUG";
constexpr const char* kRpcPollDebugEnvironment = "INFINITECZ_BUILD_RPC_POLL_DEBUG";
constexpr const char* kRpcPointerCapabilityEnvironment =
    "INFINITECZ_BUILD_RPC_POINTER_CAPABLE";
constexpr const char* kRpcFastPackerEnvironment =
    "INFINITECZ_BUILD_RPC_FAST_PACKER";
constexpr const char* kRpcPointerFailureEnvironment =
    "INFINITECZ_BUILD_RPC_POINTER_FAILED";
constexpr const char* kRpcPointerProbeEnvironment =
    "INFINITECZ_BUILD_RPC_POINTER_PROBE";
constexpr const char* kClearPlanVersion = "clear-region-grid-v9";
constexpr const char* kPreservePlanVersion = "preserve-region-grid-v9";
constexpr const char* kLegacyGridClearPlanVersion = "clear-region-grid-v8";
constexpr const char* kLegacyGridPreservePlanVersion = "preserve-region-grid-v8";
constexpr const char* kLegacyRegionClearPlanVersion = "clear-region-command-plan-v7";
constexpr const char* kLegacyRegionPreservePlanVersion = "preserve-region-command-plan-v7";
constexpr const char* kLegacyClearPlanVersion = "clear-before-first-placement-v6";
constexpr const char* kLegacyPreservePlanVersion = "preserve-command-plan-v6";
constexpr uint32_t kOldestCurrentCommandPlanCheckpointVersion = 7;

// Command-block payloads are deliberately kept outside checkpoint.bin: the
// ordinary import cursor remains compatible with old plans, while this tiny
// sidecar is advanced only after successful bounded send bursts. Replaying a
// record after a process crash is idempotent (it updates the same block
// position), so the only unsafe direction is advancing the cursor before
// send() succeeds.
constexpr const char* kCommandBlockStateFileName = "command_blocks.state";
constexpr const char* kSignStateFileName = "signs.state";
constexpr const char* kEntityStateFileName = "entities.state";
constexpr const char* kContainerItemStateFileName = "container_items.state";
constexpr const char* kMapCreationStateFileName = "map_creation.state";
constexpr const char* kMapUsePendingSuffix = ".use_pending";
constexpr uint32_t kCommandBlockStateMagic = 0x31534243U;  // "CBS1"
constexpr uint32_t kCommandBlockStateVersion = 1;
// The editor-side command-block update distance is substantially smaller than
// a simulation range. These cells only group load/probe work and nearby
// packet bursts; they are deliberately *not* used as the player's final TP
// target. Every burst anchors itself on its actual pending record and the
// per-record guard below repositions before an edge record reaches the
// server's stricter continuous-coordinate editor check.
constexpr int32_t kCommandBlockCellHorizontalSpanBlocks = 9;
constexpr int32_t kCommandBlockCellVerticalSpanBlocks = 4;
constexpr int32_t kCommandBlockLoadWindowRadiusCells = 1;
// CommandBlockWait uses the tracked probe and native readability check as its
// readiness gate. Do not add a blind TP sleep in front of that evidence.
constexpr auto kCommandBlockCellMinimumWait = std::chrono::milliseconds(0);
constexpr uint32_t kCommandBlockCellLoadRetryLimit = 3;
constexpr uint32_t kCommandBlockTargetRetryLimit = 3;
// Command-block entity packets are sent synchronously on the game thread.
// The old 256/4 ms cap left large blueprints spending most of their time in
// the deferred-data phase even when the normal import rate was high.  A wider
// bounded burst keeps packet construction off the parser thread while making
// command setup materially closer to the configured building speed.
constexpr uint32_t kCommandBlockWritesPerTick = 768;
constexpr auto kCommandBlockWriteTickBudget = std::chrono::milliseconds(10);
constexpr int32_t kSignCellSpanBlocks = 16;
constexpr auto kSignTargetSettleDelay = std::chrono::seconds(1);
constexpr auto kSignEditSessionTimeout = std::chrono::seconds(2);
constexpr auto kSignEditProbeSettleDelay = std::chrono::milliseconds(500);
constexpr uint32_t kSignEditProbeCount = 4;
constexpr auto kSignVerificationPollInterval = std::chrono::milliseconds(100);
constexpr auto kSignVerificationTimeout = std::chrono::seconds(5);
constexpr uint32_t kSignMaximumTargetAttempts = 3;
constexpr int32_t kEntityCellSpanBlocks = 16;
constexpr auto kDeferredCellMinimumWait = std::chrono::milliseconds(20 * 50);
constexpr uint32_t kDeferredCellLoadRetryLimit = 3;
constexpr uint32_t kDeferredTargetRetryLimit = 3;
constexpr int32_t kContainerItemCellSpanBlocks = 16;
// Packet-based container restores already wait for the previous inventory
// transaction to settle. No additional travel delay is needed between targets.
constexpr auto kContainerInterTargetDelay = std::chrono::milliseconds(0);
constexpr int32_t kMapTileSpanBlocks = 128;
constexpr uint64_t kMaximumAutomaticMapTiles = 65536;
constexpr auto kMapTargetSettleDelay = std::chrono::milliseconds(350);
constexpr auto kMapItemSettleDelay = std::chrono::milliseconds(500);
constexpr auto kMapUseConfirmationTimeout = std::chrono::seconds(15);
constexpr auto kMapHoldConfirmationTimeout = std::chrono::seconds(45);
constexpr auto kMapTargetTimeout = std::chrono::seconds(5);
constexpr auto kMapStorageTravelTimeout = std::chrono::seconds(10);
constexpr auto kMapSlotRetryDelay = std::chrono::milliseconds(150);
constexpr uint32_t kMapSlotRetryLimit = 6;

struct CommandBlockStateDiskV1 {
    uint32_t magic = kCommandBlockStateMagic;
    uint32_t version = kCommandBlockStateVersion;
    uint64_t record_count = 0;
    uint64_t cursor = 0;
};

struct DeferredDataStateDiskV1 {
    uint32_t magic = 0x31534444U;  // "DDS1"
    uint32_t version = 1;
    uint64_t record_count = 0;
    uint64_t cursor = 0;
};

static_assert(sizeof(CommandBlockStateDiskV1) == 24,
               "unexpected command-block state sidecar layout");
static_assert(sizeof(DeferredDataStateDiskV1) == 24,
              "unexpected deferred data state sidecar layout");

bool commandBlockStateExists(const std::string& path) {
    struct stat status {};
    return !path.empty() && stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

bool loadCommandBlockState(const std::string& path, uint64_t expected_count,
                           uint64_t* cursor, std::string* error) {
    if (!cursor) {
        if (error) *error = "command-block state cursor output is unavailable";
        return false;
    }
    *cursor = 0;
    if (!commandBlockStateExists(path)) return true;
    std::ifstream input(path, std::ios::binary);
    CommandBlockStateDiskV1 disk;
    char trailing = 0;
    if (!input || !input.read(reinterpret_cast<char*>(&disk), sizeof(disk)) ||
        input.read(&trailing, 1) || disk.magic != kCommandBlockStateMagic ||
        disk.version != kCommandBlockStateVersion || disk.record_count != expected_count ||
        disk.cursor > expected_count) {
        if (error) *error = "command-block progress state is corrupt or belongs to another import";
        return false;
    }
    *cursor = disk.cursor;
    return true;
}

bool saveCommandBlockState(const std::string& path, uint64_t record_count,
                           uint64_t cursor, std::string* error) {
    if (path.empty() || cursor > record_count) {
        if (error) *error = "command-block progress state has invalid bounds";
        return false;
    }
    const std::string temporary = path + ".tmp";
    std::remove(temporary.c_str());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    const CommandBlockStateDiskV1 disk{
        kCommandBlockStateMagic, kCommandBlockStateVersion, record_count, cursor};
    if (!output || !output.write(reinterpret_cast<const char*>(&disk), sizeof(disk))) {
        output.close();
        std::remove(temporary.c_str());
        if (error) *error = "cannot write command-block progress state";
        return false;
    }
    output.flush();
    output.close();
    if (!output) {
        std::remove(temporary.c_str());
        if (error) *error = "cannot flush command-block progress state";
        return false;
    }
#if defined(_WIN32)
    // Android is the production target, but keeping the host test path usable
    // avoids an overwrite failure from MSVCRT's rename implementation.
    std::remove(path.c_str());
#endif
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        if (error) *error = "cannot finalize command-block progress state";
        return false;
    }
    return true;
}

bool loadDeferredDataState(const std::string& path, uint64_t expected_count,
                           uint64_t* cursor, std::string* error) {
    if (!cursor) {
        if (error) *error = "deferred data state cursor output is unavailable";
        return false;
    }
    *cursor = 0;
    if (!commandBlockStateExists(path)) return true;
    std::ifstream input(path, std::ios::binary);
    DeferredDataStateDiskV1 disk;
    char trailing = 0;
    if (!input || !input.read(reinterpret_cast<char*>(&disk), sizeof(disk)) ||
        input.read(&trailing, 1) || disk.magic != 0x31534444U || disk.version != 1 ||
        disk.record_count != expected_count || disk.cursor > expected_count) {
        if (error) *error = "deferred data progress state is corrupt or belongs to another import";
        return false;
    }
    *cursor = disk.cursor;
    return true;
}

bool saveDeferredDataState(const std::string& path, uint64_t record_count,
                           uint64_t cursor, std::string* error) {
    if (path.empty() || cursor > record_count) {
        if (error) *error = "deferred data progress state has invalid bounds";
        return false;
    }
    const std::string temporary = path + ".tmp";
    std::remove(temporary.c_str());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    const DeferredDataStateDiskV1 disk{0x31534444U, 1, record_count, cursor};
    if (!output || !output.write(reinterpret_cast<const char*>(&disk), sizeof(disk))) {
        output.close();
        std::remove(temporary.c_str());
        if (error) *error = "cannot write deferred data progress state";
        return false;
    }
    output.flush();
    output.close();
    if (!output) {
        std::remove(temporary.c_str());
        if (error) *error = "cannot flush deferred data progress state";
        return false;
    }
#if defined(_WIN32)
    std::remove(path.c_str());
#endif
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        if (error) *error = "cannot finalize deferred data progress state";
        return false;
    }
    return true;
}

int32_t commandBlockCellCoordinate(int32_t value, int32_t span) {
    return floorDiv(value, span);
}

int32_t commandBlockCellXzCoordinate(int32_t value) {
    return commandBlockCellCoordinate(value, kCommandBlockCellHorizontalSpanBlocks);
}

int32_t commandBlockCellYCoordinate(int32_t value) {
    return commandBlockCellCoordinate(value, kCommandBlockCellVerticalSpanBlocks);
}

int32_t clampCommandBlockCellToInt32(int64_t value) {
    return static_cast<int32_t>(std::max<int64_t>(std::numeric_limits<int32_t>::min(),
        std::min<int64_t>(std::numeric_limits<int32_t>::max(), value)));
}

BlockBounds commandBlockCellBounds(int32_t cell_x, int32_t cell_y, int32_t cell_z) {
    const auto edge = [](int32_t cell, int32_t span) {
        return static_cast<int64_t>(cell) * span;
    };
    const int64_t min_x = edge(cell_x, kCommandBlockCellHorizontalSpanBlocks);
    const int64_t min_y = edge(cell_y, kCommandBlockCellVerticalSpanBlocks);
    const int64_t min_z = edge(cell_z, kCommandBlockCellHorizontalSpanBlocks);
    return {clampCommandBlockCellToInt32(min_x), clampCommandBlockCellToInt32(min_y),
            clampCommandBlockCellToInt32(min_z),
            clampCommandBlockCellToInt32(min_x + kCommandBlockCellHorizontalSpanBlocks - 1),
            clampCommandBlockCellToInt32(min_y + kCommandBlockCellVerticalSpanBlocks - 1),
            clampCommandBlockCellToInt32(min_z + kCommandBlockCellHorizontalSpanBlocks - 1)};
}

BlockBounds commandBlockLoadWindowBounds(int32_t cell_x, int32_t cell_y, int32_t cell_z) {
    // This wider ticking-area window is intentionally independent of the
    // eight-block editor cell.  It lets adjacent cells reuse a loaded area,
    // while every packet is still guarded by localPlayerIsNearCommandBlockRecord.
    const BlockBounds minimum = commandBlockCellBounds(
        cell_x - kCommandBlockLoadWindowRadiusCells,
        cell_y - kCommandBlockLoadWindowRadiusCells,
        cell_z - kCommandBlockLoadWindowRadiusCells);
    const BlockBounds maximum = commandBlockCellBounds(
        cell_x + kCommandBlockLoadWindowRadiusCells,
        cell_y + kCommandBlockLoadWindowRadiusCells,
        cell_z + kCommandBlockLoadWindowRadiusCells);
    return {minimum.min_x, minimum.min_y, minimum.min_z,
            maximum.max_x, maximum.max_y, maximum.max_z};
}

int32_t deferredCellCoordinate(int32_t value, int32_t span) {
    return floorDiv(value, std::max<int32_t>(1, span));
}

BlockBounds deferredCellBounds(int32_t cell_x, int32_t cell_y, int32_t cell_z, int32_t span) {
    const int64_t safe_span = std::max<int32_t>(1, span);
    const auto edge = [safe_span](int32_t cell) {
        return static_cast<int64_t>(cell) * safe_span;
    };
    const int64_t min_x = edge(cell_x);
    const int64_t min_y = edge(cell_y);
    const int64_t min_z = edge(cell_z);
    return {clampCommandBlockCellToInt32(min_x), clampCommandBlockCellToInt32(min_y),
            clampCommandBlockCellToInt32(min_z),
            clampCommandBlockCellToInt32(min_x + safe_span - 1),
            clampCommandBlockCellToInt32(min_y + safe_span - 1),
            clampCommandBlockCellToInt32(min_z + safe_span - 1)};
}

bool sameDeferredCell(int32_t x, int32_t y, int32_t z,
                      int32_t cell_x, int32_t cell_y, int32_t cell_z, int32_t span) {
    return deferredCellCoordinate(x, span) == cell_x &&
           deferredCellCoordinate(y, span) == cell_y &&
           deferredCellCoordinate(z, span) == cell_z;
}

int64_t mapFloorDiv(int64_t value) {
    constexpr int64_t span = kMapTileSpanBlocks;
    return value >= 0 ? value / span : -((-value + span - 1) / span);
}

int64_t mapCenterForCoordinate(int32_t coordinate) {
    return mapFloorDiv(static_cast<int64_t>(coordinate) + kMapTileSpanBlocks / 2) *
        kMapTileSpanBlocks;
}

bool automaticMapPlan(const BlockBounds& bounds, uint64_t* columns,
                      uint64_t* rows, uint64_t* count,
                      std::string* error = nullptr) {
    if (!columns || !rows || !count || !bounds.isValid() ||
        bounds.min_y == std::numeric_limits<int32_t>::max()) {
        if (error) *error = "pixel-art map bounds are invalid";
        return false;
    }
    const int64_t first_x = mapCenterForCoordinate(bounds.min_x);
    const int64_t first_z = mapCenterForCoordinate(bounds.min_z);
    const int64_t last_x = mapCenterForCoordinate(bounds.max_x);
    const int64_t last_z = mapCenterForCoordinate(bounds.max_z);
    const uint64_t tile_columns = static_cast<uint64_t>(
        (last_x - first_x) / kMapTileSpanBlocks) + 1U;
    const uint64_t tile_rows = static_cast<uint64_t>(
        (last_z - first_z) / kMapTileSpanBlocks) + 1U;
    if (tile_columns == 0 || tile_rows == 0 ||
        tile_columns > std::numeric_limits<uint64_t>::max() / tile_rows) {
        if (error) *error = "pixel-art map grid is too large";
        return false;
    }
    const uint64_t tile_count = tile_columns * tile_rows;
    if (tile_count > kMaximumAutomaticMapTiles) {
        if (error) *error = "pixel-art map grid exceeds the automatic map limit";
        return false;
    }
    if (first_x < std::numeric_limits<int32_t>::min() ||
        first_x > std::numeric_limits<int32_t>::max() ||
        first_z < std::numeric_limits<int32_t>::min() ||
        first_z > std::numeric_limits<int32_t>::max() ||
        last_x < std::numeric_limits<int32_t>::min() ||
        last_x > std::numeric_limits<int32_t>::max() ||
        last_z < std::numeric_limits<int32_t>::min() ||
        last_z > std::numeric_limits<int32_t>::max()) {
        if (error) *error = "pixel-art map centers exceed the supported world range";
        return false;
    }
    *columns = tile_columns;
    *rows = tile_rows;
    *count = tile_count;
    return true;
}

struct MapUsePendingDiskV1 {
    uint32_t magic = 0x3155504DU;  // "MPU1"
    uint32_t version = 1U;
    uint64_t tile_cursor = 0U;
    int32_t hotbar_slot = -1;
    int32_t network_stack_id = 0;
    uint16_t empty_map_count = 0U;
    uint16_t reserved = 0U;
    uint32_t filled_map_total = 0U;
    int32_t filled_map_network_ids_before[36]{};
    int32_t target_x = 0;
    int32_t target_z = 0;
    uint64_t texture_sequence_before = 0U;
};

std::string mapUsePendingPath(const std::string& map_state_path) {
    return map_state_path + kMapUsePendingSuffix;
}

// An existing map-use marker means the map may already have been created.
// Once a chest request is journaled, its item may also have left inventory.
// The ordinary map-use recovery below assumes the opposite, so neither it
// nor spool cleanup may run while either storage sidecar is outstanding.
// The storage driver owns both legacy reopened-chest and current accepted-
// response/original-window-close proof; this inventory-only path has neither.
bool requireNoPendingMapStorage(const std::string& map_state_path,
                                const WorldContext& world,
                                const BlockBounds& artwork_bounds,
                                uint64_t tile_count, uint64_t tile_cursor,
                                std::string* error) {
    // Use the same map_creation.state path as the map-use and chest sidecars.
    // Until a storage recovery driver exists, the inventory-only path may
    // neither recreate a map nor advance its cursor past an anvil request.
    MapAnvilRenameRecord rename;
    std::string rename_error;
    const MapAnvilRenameLoad rename_load = LoadMapAnvilRenameJournal(
        map_state_path, &rename, &rename_error);
    if (rename_load == MapAnvilRenameLoad::Unsafe) {
        if (error) *error = "map anvil rename state cannot be safely read: " +
            (rename_error.empty() ? "unknown journal error" : rename_error);
        return false;
    }
    if (rename_load == MapAnvilRenameLoad::Loaded) {
        const auto recovery = ClassifyMapAnvilRenameRecovery(
            rename, world.world_id, world.dimension_id, tile_cursor, tile_count);
        if (error) {
            *error = recovery == MapAnvilRenameRecovery::Unsafe
                ? "map anvil rename belongs to another map plan or world"
                : "map anvil rename is pending verified storage recovery; "
                  "automatic maps will not continue through the inventory-only path";
        }
        return false;
    }
    MapPairPlacementRecord pair;
    std::string journal_error;
    const MapPairPlacementLoad pair_load = LoadMapPairPlacementJournal(
        map_state_path, &pair, &journal_error);
    if (pair_load == MapPairPlacementLoad::Unsafe) {
        if (error) *error = "map chest/anvil placement state cannot be safely read: " +
            (journal_error.empty() ? "unknown journal error" : journal_error);
        return false;
    }
    if (pair_load == MapPairPlacementLoad::Loaded &&
        ClassifyMapPairPlacementResume(pair, world.world_id, world.dimension_id,
                                       tile_count, artwork_bounds) ==
            MapPairPlacementResume::Unsafe) {
        if (error) *error = "map chest/anvil placement state belongs to another map plan or world";
        return false;
    }

    MapChestTransferRecord transfer;
    journal_error.clear();
    const MapChestJournalLoad transfer_load = LoadMapChestTransferJournal(
        map_state_path, &transfer, &journal_error);
    if (transfer_load == MapChestJournalLoad::Unsafe) {
        if (error) *error = "map chest transfer state cannot be safely read: " +
            (journal_error.empty() ? "unknown journal error" : journal_error);
        return false;
    }
    if (transfer_load == MapChestJournalLoad::Loaded) {
        if (pair_load != MapPairPlacementLoad::Loaded ||
            pair.phase != MapPairPlacementPhase::PairConfirmed) {
            if (error) *error = "map chest transfer has no confirmed matching chest/anvil placement plan";
            return false;
        }
        MapTileChestAddress address;
        if (!ResolveMapTileChestAddress(transfer.tile_cursor, tile_count, &address) ||
            transfer.chest_index != address.chest_index ||
            transfer.chest_slot != address.slot) {
            if (error) *error = "map chest transfer does not match its deterministic chest index and slot";
            return false;
        }
        int32_t expected_chest_x = pair.pair.chest.x;
        int32_t expected_chest_y = pair.pair.chest.y;
        int32_t expected_chest_z = pair.pair.chest.z;
        // A tile after the first 27 must use its own persisted chest, not
        // the original pair's chest. Require the complete confirmed prefix:
        // a missing sidecar can never authorize selecting another location.
        for (uint32_t index = 1; index <= address.chest_index; ++index) {
            MapExtraChestPlacementRecord extra;
            journal_error.clear();
            const auto extra_load = LoadMapExtraChestPlacementJournal(
                map_state_path, static_cast<uint16_t>(index), &extra, &journal_error);
            if (extra_load != MapExtraChestPlacementLoad::Loaded ||
                ClassifyMapExtraChestPlacementResume(
                    extra, world.world_id, world.dimension_id, tile_count,
                    artwork_bounds, static_cast<uint16_t>(index)) !=
                    MapExtraChestPlacementResume::VerifyConfirmedChest) {
                if (error) *error = "map chest transfer has no confirmed extra-chest plan at index " +
                    std::to_string(index) + (journal_error.empty() ? "" : ": " + journal_error);
                return false;
            }
            if (index == address.chest_index) {
                expected_chest_x = extra.chest.x;
                expected_chest_y = extra.chest.y;
                expected_chest_z = extra.chest.z;
            }
        }
        const MapChestRecoveryAction action = ClassifyMapChestTransferRecovery(
            transfer, world.world_id, world.dimension_id, tile_cursor,
            tile_count, expected_chest_x, expected_chest_y,
            expected_chest_z, address.slot);
        if (action == MapChestRecoveryAction::Unsafe) {
            if (error) *error = "map chest transfer does not match the current world, chest, or saved tile cursor";
            return false;
        }
        if (error) {
            const std::string tile = std::to_string(transfer.tile_cursor + 1U);
            switch (action) {
                case MapChestRecoveryAction::FreshPreflightRequired:
                    *error = "map chest transfer for tile " + tile +
                        " was prepared but not sent; fresh source/chest preflight is required";
                    break;
                case MapChestRecoveryAction::ReopenChestNoResend:
                    *error = "map chest transfer for tile " + tile +
                        " may have been sent; confirm its accepted response without resending";
                    break;
                case MapChestRecoveryAction::CommitTileCursorNoResend:
                    *error = "map chest transfer for tile " + tile +
                        " has durable placement proof; its storage cursor can be committed";
                    break;
                case MapChestRecoveryAction::ClearJournalAfterCursorCommit:
                    *error = "map chest transfer for tile " + tile +
                        " has a committed cursor; its journal needs safe cleanup before continuing";
                    break;
                case MapChestRecoveryAction::Unsafe:
                    break;
            }
        }
        return false;
    }
    if (pair_load == MapPairPlacementLoad::Loaded) {
        if (error) {
            *error = "map chest/anvil placement plan is pending storage recovery; "
                "automatic maps will not continue through the inventory-only path";
        }
        return false;
    }
    return true;
}

bool saveMapUsePending(const std::string& map_state_path,
                       const MapUsePendingDiskV1& pending, std::string* error) {
    if (map_state_path.empty() || pending.hotbar_slot < 0 ||
        pending.hotbar_slot > 8 || pending.network_stack_id <= 0 ||
        pending.empty_map_count == 0U) {
        if (error) *error = "map use pending state is invalid";
        return false;
    }
    const std::string path = mapUsePendingPath(map_state_path);
    if (commandBlockStateExists(path)) {
        if (error) *error = "a previous map-use request is still unresolved";
        return false;
    }
    const std::string temporary = path + ".tmp";
    std::remove(temporary.c_str());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(reinterpret_cast<const char*>(&pending),
                                 sizeof(pending))) {
        if (error) *error = "cannot save pending map-use state";
        return false;
    }
    output.flush();
    if (!output) {
        if (error) *error = "cannot flush pending map-use state";
        return false;
    }
    output.close();
    if (!output || std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        if (error) *error = "cannot finalize pending map-use state";
        return false;
    }
    if (error) error->clear();
    return true;
}

bool loadMapUsePending(const std::string& map_state_path,
                       MapUsePendingDiskV1* pending, std::string* error) {
    if (!pending) return false;
    const std::string path = mapUsePendingPath(map_state_path);
    std::ifstream input(path, std::ios::binary);
    char trailing = 0;
    if (!input || !input.read(reinterpret_cast<char*>(pending), sizeof(*pending)) ||
        input.read(&trailing, 1) || pending->magic != 0x3155504DU ||
        pending->version != 1U || pending->hotbar_slot < 0 ||
        pending->hotbar_slot > 8 || pending->network_stack_id <= 0 ||
        pending->empty_map_count == 0U) {
        if (error) *error = "pending map-use state is corrupt";
        return false;
    }
    if (error) error->clear();
    return true;
}

bool findNewFilledMap(const MapInventorySnapshot& inventory,
                      const int32_t* filled_ids_before,
                      int32_t* item_network_id) {
    if (item_network_id) *item_network_id = 0;
    if (!inventory.ready || !inventory.network_ready || !filled_ids_before) return false;
    int32_t found = 0;
    for (size_t slot = 0; slot < inventory.slots.size(); ++slot) {
        const MapInventorySlot& item = inventory.slots[slot];
        if (!item.filled_map || !item.native_occupied ||
            !item.has_network_stack_id || item.network_stack_id <= 0) continue;
        bool existed_before = false;
        for (size_t previous = 0; previous < inventory.slots.size(); ++previous) {
            if (filled_ids_before[previous] == item.network_stack_id) {
                existed_before = true;
                break;
            }
        }
        if (existed_before) continue;
        if (found != 0) return false;  // More than one new map is ambiguous.
        found = item.network_stack_id;
    }
    if (item_network_id) *item_network_id = found;
    return found > 0;
}

bool findFilledMapUuid(const MapInventorySnapshot& inventory,
                       int32_t network_stack_id, int64_t* map_uuid) {
    if (map_uuid) *map_uuid = -1;
    if (!inventory.ready || network_stack_id <= 0 || !map_uuid) return false;
    for (const MapInventorySlot& item : inventory.slots) {
        if (item.filled_map && item.native_occupied &&
            item.has_network_stack_id &&
            item.network_stack_id == network_stack_id &&
            item.has_map_uuid && item.map_uuid != -1) {
            *map_uuid = item.map_uuid;
            return true;
        }
    }
    return false;
}

bool mapArtworkPixelRectangle(const BlockBounds& artwork, int32_t center_x,
                              int32_t center_z, int32_t* x_offset,
                              int32_t* z_offset, int32_t* width,
                              int32_t* height) {
    if (!artwork.isValid() || !x_offset || !z_offset || !width || !height) {
        return false;
    }
    const int64_t min_x = static_cast<int64_t>(center_x) - 64;
    const int64_t min_z = static_cast<int64_t>(center_z) - 64;
    const int64_t first_x = std::max<int64_t>(artwork.min_x, min_x);
    const int64_t first_z = std::max<int64_t>(artwork.min_z, min_z);
    const int64_t last_x = std::min<int64_t>(artwork.max_x, min_x + 127);
    const int64_t last_z = std::min<int64_t>(artwork.max_z, min_z + 127);
    if (first_x > last_x || first_z > last_z) return false;
    *x_offset = static_cast<int32_t>(first_x - min_x);
    *z_offset = static_cast<int32_t>(first_z - min_z);
    *width = static_cast<int32_t>(last_x - first_x + 1);
    *height = static_cast<int32_t>(last_z - first_z + 1);
    return *x_offset >= 0 && *z_offset >= 0 && *width > 0 && *height > 0 &&
        *x_offset + *width <= 128 && *z_offset + *height <= 128;
}

std::string escapeCommandString(std::string_view value) {
    std::string output;
    output.reserve(value.size() + 2);
    output.push_back('"');
    for (const unsigned char character : value) {
        if (character < 0x20 || character == 0x7F) continue;
        if (character == '\\' || character == '"') output.push_back('\\');
        output.push_back(static_cast<char>(character));
    }
    output.push_back('"');
    return output;
}

std::string formatMapStorageFillCommand(const MapChestPosition& first,
                                        const MapChestPosition& last,
                                        std::string_view block) {
    return "/fill " + std::to_string(first.x) + " " +
        std::to_string(first.y) + " " + std::to_string(first.z) + " " +
        std::to_string(last.x) + " " + std::to_string(last.y) + " " +
        std::to_string(last.z) + " " + std::string(block);
}

std::string entitySummonCommand(const EntityRecord& record) {
    std::string command = "/summon " + record.entity_id + " " +
        std::to_string(record.x) + " " + std::to_string(record.y) + " " +
        std::to_string(record.z) + " " + std::to_string(record.yaw) + " " +
        std::to_string(record.pitch);
    // The blank spawn-event placeholder keeps the optional name tag in its
    // documented position.  The parser has already stripped control data and
    // arbitrary JSON/NBT from the source name.
    if (!record.custom_name.empty()) command += " \"\" " + escapeCommandString(record.custom_name);
    return command;
}

bool localPlayerIsNearDeferredTarget(int32_t x, int32_t y, int32_t z,
                                     int64_t radius_blocks) {
    int32_t player_x = 0;
    int32_t player_y = 0;
    int32_t player_z = 0;
    if (!NativeWorldAccess::getLocalPlayerBlockPosition(&player_x, &player_y, &player_z)) {
        // Command-side validation still occurs at the server. Do not turn a
        // transient native-position read failure into a permanent post-import
        // stall after the /tp command was already issued.
        return true;
    }
    const int64_t dx = static_cast<int64_t>(player_x) - x;
    const int64_t dy = static_cast<int64_t>(player_y) - y;
    const int64_t dz = static_cast<int64_t>(player_z) - z;
    return dx * dx + dy * dy + dz * dz <= radius_blocks * radius_blocks;
}

std::vector<std::string> deferredCellPrepareCommands(const BlockBounds& bounds) {
    std::vector<std::string> commands;
    if (!bounds.isValid()) return commands;
    const int32_t teleport_x = clampCommandBlockCellToInt32(
        (static_cast<int64_t>(bounds.min_x) + bounds.max_x) / 2);
    const int32_t teleport_y = clampCommandBlockCellToInt32(
        static_cast<int64_t>(bounds.max_y) + 2);
    const int32_t teleport_z = clampCommandBlockCellToInt32(
        (static_cast<int64_t>(bounds.min_z) + bounds.max_z) / 2);
    commands.push_back("/tickingarea add " + std::to_string(bounds.min_x) + " " +
        std::to_string(bounds.min_y) + " " + std::to_string(bounds.min_z) + " " +
        std::to_string(bounds.max_x) + " " + std::to_string(bounds.max_y) + " " +
        std::to_string(bounds.max_z) + " infinitecz_build true");

    int32_t player_x = 0;
    int32_t player_y = 0;
    int32_t player_z = 0;
    if (NativeWorldAccess::getLocalPlayerBlockPosition(&player_x, &player_y, &player_z)) {
        const double dx = static_cast<double>(teleport_x) - player_x;
        const double dy = static_cast<double>(teleport_y) - player_y;
        const double dz = static_cast<double>(teleport_z) - player_z;
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        constexpr double kMaximumTeleportHopDistance = 96.0;
        constexpr int64_t kMaximumTeleportHopCommands = 512;
        if (std::isfinite(distance) && distance > kMaximumTeleportHopDistance) {
            const int64_t hop_count = std::min<int64_t>(kMaximumTeleportHopCommands,
                static_cast<int64_t>(distance / kMaximumTeleportHopDistance));
            for (int64_t index = 1; index <= hop_count; ++index) {
                const double fraction = static_cast<double>(index) /
                    static_cast<double>(hop_count + 1);
                commands.push_back("/tp @s " + std::to_string(clampCommandBlockCellToInt32(
                    static_cast<int64_t>(std::floor(player_x + dx * fraction)))) + " " +
                    std::to_string(clampCommandBlockCellToInt32(
                    static_cast<int64_t>(std::floor(player_y + dy * fraction)))) + " " +
                    std::to_string(clampCommandBlockCellToInt32(
                    static_cast<int64_t>(std::floor(player_z + dz * fraction)))));
            }
        }
    }
    commands.push_back("/tp @s " + std::to_string(teleport_x) + " " +
                       std::to_string(teleport_y) + " " + std::to_string(teleport_z));
    return commands;
}

std::vector<std::string> containerItemPrepareCommands(const BlockBounds& bounds,
                                                       int32_t target_x,
                                                       int32_t target_y,
                                                       int32_t target_z,
                                                       bool add_ticking_area) {
    std::vector<std::string> commands;
    if (!bounds.isValid()) return commands;
    if (add_ticking_area) {
        commands.push_back("/tickingarea add " + std::to_string(bounds.min_x) + " " +
            std::to_string(bounds.min_y) + " " + std::to_string(bounds.min_z) + " " +
            std::to_string(bounds.max_x) + " " + std::to_string(bounds.max_y) + " " +
            std::to_string(bounds.max_z) + " infinitecz_build true");
    }

    // Bedrock's block-entity editor validates the player against the actual
    // target, so the final TP is deliberately the block directly above the
    // container rather than the centre of a loading cell.
    const int32_t teleport_y = clampCommandBlockCellToInt32(
        static_cast<int64_t>(target_y) + 1);
    int32_t player_x = 0;
    int32_t player_y = 0;
    int32_t player_z = 0;
    if (NativeWorldAccess::getLocalPlayerBlockPosition(&player_x, &player_y, &player_z)) {
        const double dx = static_cast<double>(target_x) - player_x;
        const double dy = static_cast<double>(teleport_y) - player_y;
        const double dz = static_cast<double>(target_z) - player_z;
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        constexpr double kMaximumTeleportHopDistance = 96.0;
        constexpr int64_t kMaximumTeleportHopCommands = 512;
        if (std::isfinite(distance) && distance > kMaximumTeleportHopDistance) {
            const int64_t hop_count = std::min<int64_t>(kMaximumTeleportHopCommands,
                static_cast<int64_t>(distance / kMaximumTeleportHopDistance));
            for (int64_t index = 1; index <= hop_count; ++index) {
                const double fraction = static_cast<double>(index) /
                    static_cast<double>(hop_count + 1);
                commands.push_back("/tp @s " + std::to_string(clampCommandBlockCellToInt32(
                    static_cast<int64_t>(std::floor(player_x + dx * fraction)))) + " " +
                    std::to_string(clampCommandBlockCellToInt32(
                    static_cast<int64_t>(std::floor(player_y + dy * fraction)))) + " " +
                    std::to_string(clampCommandBlockCellToInt32(
                    static_cast<int64_t>(std::floor(player_z + dz * fraction)))));
            }
        }
    }
    commands.push_back(formatContainerTeleportCommand(target_x, target_y, target_z));
    return commands;
}

std::string formatSignEditProbeTeleportCommand(int32_t target_x,
                                                int32_t target_y,
                                                int32_t target_z,
                                                uint32_t probe_index) {
    const double center_x = static_cast<double>(target_x) + 0.5;
    const double center_z = static_cast<double>(target_z) + 0.5;
    double player_x = center_x;
    double player_z = center_z;
    int32_t yaw = 0;
    switch (probe_index % kSignEditProbeCount) {
        case 0:
            player_z -= 2.0;
            yaw = 0;
            break;
        case 1:
            player_z += 2.0;
            yaw = 180;
            break;
        case 2:
            player_x -= 2.0;
            yaw = -90;
            break;
        default:
            player_x += 2.0;
            yaw = 90;
            break;
    }
    const int32_t player_y = clampCommandBlockCellToInt32(
        static_cast<int64_t>(target_y) + 1);
    return "/tp @s " + std::to_string(player_x) + " " +
        std::to_string(player_y) + " " + std::to_string(player_z) + " " +
        std::to_string(yaw) + " 35";
}

bool sameCommandBlockCell(const CommandBlockRecord& record,
                           int32_t cell_x, int32_t cell_y, int32_t cell_z) {
    return commandBlockCellXzCoordinate(record.x) == cell_x &&
           commandBlockCellYCoordinate(record.y) == cell_y &&
           commandBlockCellXzCoordinate(record.z) == cell_z;
}

bool localPlayerIsNearCommandBlockRecord(const CommandBlockRecord& record) {
    int32_t player_x = 0;
    int32_t player_y = 0;
    int32_t player_z = 0;
    if (!NativeWorldAccess::getLocalPlayerBlockPosition(
            &player_x, &player_y, &player_z)) {
        return false;
    }
    // Command-block editor packets are more restrictive than ordinary /fill
    // commands. Verify the *actual* packet target, rather than only the cell
    // centre, before every write. The shared helper intentionally reserves a
    // margin below the observed server limit for fractional player/block
    // positions that the block-aligned native cache cannot represent.
    return isWithinCommandBlockEditorSafeRadius(player_x, player_y, player_z, record);
}

std::string commandBlockIdentifierLeaf(std::string_view identifier) {
    const size_t state_begin = identifier.find('[');
    if (state_begin != std::string_view::npos) identifier = identifier.substr(0, state_begin);
    const size_t separator = identifier.rfind(':');
    if (separator != std::string_view::npos) identifier = identifier.substr(separator + 1);
    std::string leaf;
    leaf.reserve(identifier.size());
    for (const char character : identifier) {
        leaf.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return leaf;
}

const char* commandBlockShellName(uint16_t mode) {
    switch (mode) {
        case kCommandBlockModeImpulse: return "minecraft:command_block";
        case kCommandBlockModeRepeat: return "minecraft:repeating_command_block";
        case kCommandBlockModeChain: return "minecraft:chain_command_block";
        default: return "a supported command block";
    }
}

bool commandBlockTargetMatches(NativeWorldReader* reader, const CommandBlockRecord& record,
                               std::string* error) {
    if (!reader || !reader->open()) {
        if (error) {
            *error = std::string("cannot read command-block target: ") +
                NativeWorldAccess::lastWorldReaderDiagnostic();
        }
        return false;
    }
    NativeBlockInfo actual;
    if (!reader->getBlock(record.x, record.y, record.z, &actual)) {
        if (error) {
            *error = "cannot read command-block target at (" + std::to_string(record.x) +
                "," + std::to_string(record.y) + "," + std::to_string(record.z) + "): " +
                NativeWorldAccess::lastWorldReaderDiagnostic();
        }
        return false;
    }

    const std::string leaf = commandBlockIdentifierLeaf(actual.name);
    const bool matches =
        (record.mode == kCommandBlockModeImpulse &&
         (leaf == "command_block" || leaf == "commandblock" || leaf == "control")) ||
        (record.mode == kCommandBlockModeRepeat &&
         (leaf == "repeating_command_block" || leaf == "repeatingcommandblock")) ||
        (record.mode == kCommandBlockModeChain &&
         (leaf == "chain_command_block" || leaf == "chaincommandblock"));
    if (matches) return true;

    if (error) {
        *error = "command-block target at (" + std::to_string(record.x) + "," +
            std::to_string(record.y) + "," + std::to_string(record.z) + ") is " +
            (actual.name.empty() ? std::string("unknown") : actual.name) +
            "; expected " + commandBlockShellName(record.mode);
    }
    return false;
}

bool usesRegionCommandSpools(const std::string& options_hash) {
    return options_hash == kClearPlanVersion || options_hash == kPreservePlanVersion ||
           options_hash == kLegacyGridClearPlanVersion ||
           options_hash == kLegacyGridPreservePlanVersion ||
           options_hash == kLegacyRegionClearPlanVersion ||
           options_hash == kLegacyRegionPreservePlanVersion;
}

bool usesAdaptiveRegionGrid(const std::string& options_hash) {
    return options_hash == kClearPlanVersion || options_hash == kPreservePlanVersion ||
           options_hash == kLegacyGridClearPlanVersion ||
           options_hash == kLegacyGridPreservePlanVersion;
}

bool isCompatiblePlanVersion(const std::string& options_hash,
                              const ImportConfig& config) {
    const OverwritePolicy policy = config.overwrite_policy;
    // v9 adds an auxiliary, non-spooled foundation step. A checkpoint that
    // requested it cannot safely resume against a v8 (or older) spool plan.
    if (config.place_deny_layer) {
        return policy == OverwritePolicy::ClearImportedBounds
            ? options_hash == kClearPlanVersion
            : options_hash == kPreservePlanVersion;
    }
    if (policy == OverwritePolicy::ClearImportedBounds) {
        return options_hash == kClearPlanVersion ||
               options_hash == kLegacyGridClearPlanVersion ||
               options_hash == kLegacyRegionClearPlanVersion ||
               options_hash == kLegacyClearPlanVersion;
    }
    return options_hash == kPreservePlanVersion ||
            options_hash == kLegacyGridPreservePlanVersion ||
            options_hash == kLegacyRegionPreservePlanVersion ||
            options_hash == kLegacyPreservePlanVersion;
}

bool commandBatchDeadlineReached(
        size_t command_count, std::chrono::steady_clock::time_point deadline) {
    return command_count % kCommandBatchDeadlineCheckStride == 0 &&
        std::chrono::steady_clock::now() >= deadline;
}

std::string pythonBytesLiteral(const std::string& value) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string literal;
    literal.reserve(value.size() + 2);
    literal.push_back('\'');
    for (const unsigned char byte : value) {
        switch (byte) {
            case '\\': literal += "\\\\"; break;
            case '\'': literal += "\\\'"; break;
            case '\n': literal += "\\n"; break;
            case '\r': literal += "\\r"; break;
            case '\t': literal += "\\t"; break;
            default:
                if (byte >= 0x20 && byte <= 0x7e) {
                    literal.push_back(static_cast<char>(byte));
                } else {
                    literal += "\\x";
                    literal.push_back(kHex[(byte >> 4U) & 0x0fU]);
                    literal.push_back(kHex[byte & 0x0fU]);
                }
                break;
        }
    }
    literal.push_back('\'');
    return literal;
}

std::chrono::milliseconds phaseSettleDelay(ImportPhase phase) {
    if (!kPhaseSettleWaitingEnabled) return std::chrono::milliseconds(0);
    if (phase == ImportPhase::Gravity) return std::chrono::milliseconds(750);
    if (phase == ImportPhase::Fluid) return std::chrono::milliseconds(1250);
    if (phase == ImportPhase::Attachment || phase == ImportPhase::DependentAttachment) {
        return std::chrono::milliseconds(300);
    }
    if (phase == ImportPhase::Structure) return std::chrono::milliseconds(100);
    return std::chrono::milliseconds(0);
}

bool supportsDeferredCommit(ImportPhase phase) {
    return phase == ImportPhase::Clear || phase == ImportPhase::Structure;
}

std::chrono::milliseconds conservativeResumeSettleDelay() {
    // Unsafe-phase interruption can intentionally roll the durable cursor all
    // the way back to Clear, which erases the identity of a partially issued
    // Gravity/Fluid phase. Resume is rare, so always apply the longest dynamic
    // settle after stale-area cleanup rather than trying to infer that phase
    // from the normalized checkpoint.
    return phaseSettleDelay(ImportPhase::Fluid);
}

bool readVarUInt(const std::string& input, size_t* cursor, uint32_t* output) {
    if (!cursor || !output) return false;
    uint32_t value = 0;
    for (uint32_t byte_index = 0; byte_index < 5 && *cursor < input.size(); ++byte_index) {
        const uint8_t byte = static_cast<uint8_t>(input[(*cursor)++]);
        if (byte_index == 4 && (byte & 0xF0U) != 0) return false;
        value |= static_cast<uint32_t>(byte & 0x7FU) << (byte_index * 7U);
        if ((byte & 0x80U) == 0) {
            *output = value;
            return true;
        }
    }
    return false;
}

bool readVarInt(const std::string& input, size_t* cursor, int32_t* output) {
    uint32_t encoded = 0;
    if (!output || !readVarUInt(input, cursor, &encoded)) return false;
    *output = static_cast<int32_t>((encoded >> 1U) ^ (0U - (encoded & 1U)));
    return true;
}

struct CanonicalBlock {
    std::string name;
    uint16_t aux = 0;
};

CanonicalBlock canonicalBlock(const std::string& input, uint16_t aux);

// Placement identity deliberately keeps the legacy slab families with their
// full auxiliary value (bit 3 = upper half).  The target command registry
// accepts those classic encodings, and some client builds ignore the modern
// per-material names' data-1 half selector, which flattened every top slab to
// a bottom slab.  Verification still normalizes through canonicalBlock.
CanonicalBlock placementBlock(const std::string& input, uint16_t aux) {
    if (input == "minecraft:stone_slab" || input == "minecraft:stone_slab2" ||
        input == "minecraft:stone_block_slab3" || input == "minecraft:stone_block_slab4" ||
        input == "minecraft:wooden_slab") {
        return {input, aux};
    }
    return canonicalBlock(input, aux);
}

CanonicalBlock canonicalBlock(const std::string& input, uint16_t aux) {
    static const char* const colors[] = {
        "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
        "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
    };
    const auto colored = [&](const char* base, uint8_t index) -> CanonicalBlock {
        return {std::string("minecraft:") + colors[index & 15] + "_" + base, 0};
    };
    if (input == "minecraft:grass") return {"minecraft:grass_block", 0};
    if (input == "minecraft:grass_block") return {input, 0};
    // Current command parsers expose one water/lava identifier and retain the
    // legacy flow level in Aux. Numeric schematics still distinguish IDs 8/9
    // and 10/11, so normalize only the command name here.
    if (input == "minecraft:flowing_water") return {"minecraft:water", aux};
    if (input == "minecraft:flowing_lava") return {"minecraft:lava", aux};
    if (input == "minecraft:stone" && aux <= 6) {
        static const char* const variants[] = {"stone", "granite", "polished_granite", "diorite",
                                                "polished_diorite", "andesite", "polished_andesite"};
        return {std::string("minecraft:") + variants[aux], 0};
    }
    if (input == "minecraft:dirt" && aux <= 2) {
        static const char* const variants[] = {"dirt", "coarse_dirt", "podzol"};
        return {std::string("minecraft:") + variants[aux], 0};
    }
    if (input == "minecraft:planks" && aux <= 5) {
        static const char* const variants[] = {"oak_planks", "spruce_planks", "birch_planks",
                                                "jungle_planks", "acacia_planks", "dark_oak_planks"};
        return {std::string("minecraft:") + variants[aux], 0};
    }
    if (input == "minecraft:sand" && aux <= 1) {
        return {aux == 0 ? "minecraft:sand" : "minecraft:red_sand", 0};
    }
    if (input == "minecraft:wool" && aux < 16) return colored("wool", aux);
    if (input == "minecraft:stained_hardened_clay" && aux < 16) return colored("terracotta", aux);
    if (input == "minecraft:stained_glass" && aux < 16) return colored("stained_glass", aux);
    if (input == "minecraft:stained_glass_pane" && aux < 16) return colored("stained_glass_pane", aux);
    if (input == "minecraft:carpet" && aux < 16) return colored("carpet", aux);
    if (input == "minecraft:concrete" && aux < 16) return colored("concrete", aux);
    if (input == "minecraft:concrete_powder" && aux < 16) return colored("concrete_powder", aux);
    if (input == "minecraft:stonebrick" && aux <= 3) {
        static const char* const variants[] = {"stone_bricks", "mossy_stone_bricks",
                                                "cracked_stone_bricks", "chiseled_stone_bricks"};
        return {std::string("minecraft:") + variants[aux], 0};
    }
    if (input == "minecraft:quartz_block") {
        switch (aux) {
            case 0: return {"minecraft:quartz_block", 0};
            case 1: return {"minecraft:chiseled_quartz_block", 0};
            case 2:
            case 6:
            case 10: return {"minecraft:quartz_pillar", 0};
            case 3:
            case 7:
            case 11: return {"minecraft:smooth_quartz", 0};
            default: break;
        }
    }
    if (input == "minecraft:quartz_pillar") return {input, 0};
    // Legacy stone slabs are split into four target-version groups.  The
    // low three bits select the material and bit 3 (8) selects the upper
    // half.  Emit the flattened name and the modern vertical-half aux (0/1)
    // so both command execution and verification use the same identity.
    if (input == "minecraft:stone_slab") {
        static const char* const variants[] = {
            "smooth_stone_slab", "sandstone_slab", "petrified_oak_slab",
            "cobblestone_slab", "brick_slab", "stone_brick_slab",
            "quartz_slab", "nether_brick_slab",
        };
        const uint8_t variant = static_cast<uint8_t>(aux & 0x07U);
        if (variant < std::size(variants) && (aux & 0xF0U) == 0) {
            return {std::string("minecraft:") + variants[variant],
                    static_cast<uint16_t>((aux & 0x08U) != 0 ? 1 : 0)};
        }
    }
    if (input == "minecraft:double_stone_slab") {
        static const char* const variants[] = {
            "smooth_stone_double_slab", "sandstone_double_slab",
            "petrified_oak_double_slab", "cobblestone_double_slab",
            "brick_double_slab", "stone_brick_double_slab",
            "quartz_double_slab", "nether_brick_double_slab",
        };
        const uint8_t variant = static_cast<uint8_t>(aux & 0x07U);
        if (variant < std::size(variants) && (aux & 0xF0U) == 0) {
            return {std::string("minecraft:") + variants[variant], 0};
        }
    }
    if (input == "minecraft:stone_slab2") {
        static const char* const variants[] = {
            "red_sandstone_slab", "purpur_slab", "prismarine_slab",
            "dark_prismarine_slab", "prismarine_brick_slab",
            "mossy_cobblestone_slab", "smooth_sandstone_slab",
            "red_nether_brick_slab",
        };
        const uint8_t variant = static_cast<uint8_t>(aux & 0x07U);
        if (variant < std::size(variants) && (aux & 0xF0U) == 0) {
            return {std::string("minecraft:") + variants[variant],
                    static_cast<uint16_t>((aux & 0x08U) != 0 ? 1 : 0)};
        }
    }
    if (input == "minecraft:double_stone_slab2") {
        static const char* const variants[] = {
            "red_sandstone_double_slab", "purpur_double_slab",
            "prismarine_double_slab", "dark_prismarine_double_slab",
            "prismarine_brick_double_slab", "mossy_cobblestone_double_slab",
            "smooth_sandstone_double_slab", "red_nether_brick_double_slab",
        };
        const uint8_t variant = static_cast<uint8_t>(aux & 0x07U);
        if (variant < std::size(variants) && (aux & 0xF0U) == 0) {
            return {std::string("minecraft:") + variants[variant], 0};
        }
    }
    if (input == "minecraft:stone_block_slab3") {
        static const char* const variants[] = {
            "end_stone_brick_slab", "smooth_red_sandstone_slab",
            "polished_andesite_slab", "andesite_slab", "diorite_slab",
            "polished_diorite_slab", "granite_slab", "polished_granite_slab",
        };
        const uint8_t variant = static_cast<uint8_t>(aux & 0x07U);
        if (variant < std::size(variants) && (aux & 0xF0U) == 0) {
            return {std::string("minecraft:") + variants[variant],
                    static_cast<uint16_t>((aux & 0x08U) != 0 ? 1 : 0)};
        }
    }
    if (input == "minecraft:double_stone_block_slab3") {
        static const char* const variants[] = {
            "end_stone_brick_double_slab", "smooth_red_sandstone_double_slab",
            "polished_andesite_double_slab", "andesite_double_slab",
            "diorite_double_slab", "polished_diorite_double_slab",
            "granite_double_slab", "polished_granite_double_slab",
        };
        const uint8_t variant = static_cast<uint8_t>(aux & 0x07U);
        if (variant < std::size(variants) && (aux & 0xF0U) == 0) {
            return {std::string("minecraft:") + variants[variant], 0};
        }
    }
    if (input == "minecraft:stone_block_slab4") {
        static const char* const variants[] = {
            "mossy_stone_brick_slab", "smooth_quartz_slab", "normal_stone_slab",
            "cut_sandstone_slab", "cut_red_sandstone_slab",
        };
        const uint8_t variant = static_cast<uint8_t>(aux & 0x07U);
        if (variant < std::size(variants) && (aux & 0xF0U) == 0) {
            return {std::string("minecraft:") + variants[variant],
                    static_cast<uint16_t>((aux & 0x08U) != 0 ? 1 : 0)};
        }
    }
    if (input == "minecraft:double_stone_block_slab4") {
        static const char* const variants[] = {
            "mossy_stone_brick_double_slab", "smooth_quartz_double_slab",
            "normal_stone_double_slab", "cut_sandstone_double_slab",
            "cut_red_sandstone_double_slab",
        };
        const uint8_t variant = static_cast<uint8_t>(aux & 0x07U);
        if (variant < std::size(variants) && (aux & 0xF0U) == 0) {
            return {std::string("minecraft:") + variants[variant], 0};
        }
    }
    if (input == "minecraft:wooden_slab") {
        static const char* const variants[] = {
            "oak_slab", "spruce_slab", "birch_slab", "jungle_slab",
            "acacia_slab", "dark_oak_slab",
        };
        const uint8_t variant = static_cast<uint8_t>(aux & 0x07U);
        if (variant < std::size(variants) && (aux & 0xF0U) == 0) {
            return {std::string("minecraft:") + variants[variant],
                    static_cast<uint16_t>((aux & 0x08U) != 0 ? 1 : 0)};
        }
    }
    if (input == "minecraft:double_wooden_slab") {
        static const char* const variants[] = {
            "oak_double_slab", "spruce_double_slab", "birch_double_slab",
            "jungle_double_slab", "acacia_double_slab", "dark_oak_double_slab",
        };
        const uint8_t variant = aux & 0x07U;
        if (variant < std::size(variants) && (aux & 0xF0U) == 0) {
            return {std::string("minecraft:") + variants[variant], 0};
        }
    }
    // A double slab has no meaningful vertical-half distinction in the
    // legacy command representation. Some client builds nevertheless report
    // aux=1 for its canonical state; collapse it for verification symmetry.
    if (input.size() > std::string("minecraft:").size() + 12 &&
        input.compare(input.size() - 12, 12, "_double_slab") == 0) {
        return {input, 0};
    }
    // Litematica/Sponge and older Bedrock registries use these stair aliases;
    // they refer to the same target states as the flattened identifiers.
    if (input == "minecraft:cobblestone_stairs") return {"minecraft:stone_stairs", aux};
    if (input == "minecraft:end_stone_brick_stairs") {
        return {"minecraft:end_brick_stairs", aux};
    }
    if (input == "minecraft:prismarine_brick_stairs") {
        return {"minecraft:prismarine_bricks_stairs", aux};
    }
    return {input, aux};
}

std::string blockLeaf(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    const size_t state = value.find('[');
    if (state != std::string::npos) value.resize(state);
    const size_t colon = value.rfind(':');
    if (colon != std::string::npos) value.erase(0, colon + 1);
    return value;
}

CanonicalBlock verificationIdentity(const std::string& input, uint16_t aux) {
    std::string qualified = input;
    const size_t state = qualified.find('[');
    if (state != std::string::npos) qualified.resize(state);
    std::transform(qualified.begin(), qualified.end(), qualified.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    const size_t separator = qualified.rfind(':');
    if (separator != std::string::npos &&
        qualified.compare(0, separator, "minecraft") != 0) {
        return {std::move(qualified), aux};
    }
    CanonicalBlock source{input, aux};
    if (const auto flat =
            BlockMapper::flatBlockIdentityForVerification(input, aux)) {
        source.name = flat->command_name;
        source.aux = flat->aux;
    }
    CanonicalBlock canonical = canonicalBlock(source.name, source.aux);
    std::string leaf = blockLeaf(canonical.name);
    uint16_t value = canonical.aux;
    const auto species = [&](const char* const* names, size_t count, uint16_t index) {
        if (index < count) {
            leaf = names[index];
            value = 0;
        }
    };
    if (leaf == "log") {
        static const char* const names[] = {"oak_log", "spruce_log", "birch_log", "jungle_log"};
        species(names, 4, value & 3);
    } else if (leaf == "log2") {
        static const char* const names[] = {"acacia_log", "dark_oak_log"};
        species(names, 2, value & 3);
    } else if (leaf == "leaves") {
        static const char* const names[] = {"oak_leaves", "spruce_leaves", "birch_leaves", "jungle_leaves"};
        species(names, 4, value & 3);
    } else if (leaf == "leaves2") {
        static const char* const names[] = {"acacia_leaves", "dark_oak_leaves"};
        species(names, 2, value & 3);
    }
    static const std::unordered_map<std::string, std::string> aliases = {
        {"noteblock","note_block"}, {"brick_block","bricks"}, {"grass","grass_block"},
        {"hardened_clay","terracotta"}, {"melon_block","melon"}, {"reeds","sugar_cane"},
        {"waterlily","lily_pad"}, {"fence","oak_fence"}, {"fence_gate","oak_fence_gate"},
        {"wooden_door","oak_door"}, {"wooden_button","oak_button"},
        {"wooden_pressure_plate","oak_pressure_plate"}, {"trapdoor","oak_trapdoor"},
        {"lit_pumpkin","jack_o_lantern"}, {"nether_brick","nether_bricks"},
        {"slime","slime_block"}, {"flowing_water","water"}, {"flowing_lava","lava"},
        {"lit_furnace","furnace"}, {"lit_redstone_ore","redstone_ore"},
        {"unpowered_repeater","repeater"}, {"powered_repeater","repeater"},
        {"unpowered_comparator","comparator"}, {"powered_comparator","comparator"},
    };
    const auto alias = aliases.find(leaf);
    if (alias != aliases.end()) leaf = alias->second;
    if (leaf.size() > 5 && leaf.compare(leaf.size() - 5, 5, "_wood") == 0) {
        leaf.replace(leaf.size() - 5, 5, "_log");
    }
    return {leaf, value};
}

bool verificationBlocksEqual(const NativeBlockInfo& actual,
                             const VerificationPlanSample& expected_sample) {
    const CanonicalBlock expected = verificationIdentity(expected_sample.name, expected_sample.aux);
    const CanonicalBlock actual_identity =
        verificationIdentity(actual.name, actual.aux);
    return expected.name == actual_identity.name &&
        (hasVerificationSampleFlag(expected_sample, VerificationSampleFlag::IgnoreAux) ||
         expected.aux == actual_identity.aux);
}

int32_t clampToInt32(int64_t value) {
    return static_cast<int32_t>(std::max<int64_t>(std::numeric_limits<int32_t>::min(),
        std::min<int64_t>(std::numeric_limits<int32_t>::max(), value)));
}

BlockBounds regionCoreBounds(const WorkUnit& unit, int32_t configured_chunk_size) {
    const int64_t chunk_size = std::max<int32_t>(1, configured_chunk_size);
    const int64_t span = std::max<int32_t>(1, unit.region_span);
    const int64_t min_x = regionMinChunkCoordinate(
        unit.region_coord.x, unit.region_span, unit.region_grid_origin.x) * chunk_size;
    const int64_t min_z = regionMinChunkCoordinate(
        unit.region_coord.z, unit.region_span, unit.region_grid_origin.z) * chunk_size;
    return {
        clampToInt32(min_x), unit.load_bounds.min_y, clampToInt32(min_z),
        clampToInt32(min_x + span * chunk_size - 1), unit.load_bounds.max_y,
        clampToInt32(min_z + span * chunk_size - 1),
    };
}

std::string commandPlanPath(const std::string& directory, const ChunkCoord& chunk,
                            ImportPhase phase, bool region_spool,
                            int32_t region_span,
                            const ChunkCoord& region_grid_origin) {
    if (region_spool) {
        const ChunkCoord region = regionForChunk(chunk, region_span, region_grid_origin);
        return directory + "/region_" + std::to_string(region.x) + "_" +
               std::to_string(region.z) + "_phase_" +
               std::to_string(static_cast<uint8_t>(phase)) + ".bsp.cmd";
    }
    return directory + "/chunk_" + std::to_string(chunk.x) + "_" +
           std::to_string(chunk.z) + "_phase_" +
           std::to_string(static_cast<uint8_t>(phase)) + ".bsp.cmd";
}

void cleanupSpoolDirectory(const std::string& directory) {
    if (directory.empty()) return;
    DIR* raw_directory = opendir(directory.c_str());
    if (!raw_directory) return;
    while (dirent* entry = readdir(raw_directory)) {
        const std::string name = entry->d_name;
        const bool chunk_spool =
            (name.rfind("chunk_", 0) == 0 || name.rfind("region_", 0) == 0) &&
            name.find(".bsp") != std::string::npos;
        const bool parser_temp = name == "_blocks.raw" || name == "_data.raw" ||
            name == "_add.raw" || name == "_blockdata.raw" ||
             name == "_litematic_blockstates.raw" ||
             name.rfind("_mcworld_", 0) == 0 ||
             name == kRawPaletteFileName ||
            name == std::string(kRawPaletteFileName) + ".tmp" ||
            (name.rfind("_route_", 0) == 0 && name.size() >= 4 &&
             name.compare(name.size() - 4, 4, ".tmp") == 0) ||
            name.rfind(".input_", 0) == 0 || name.rfind(".pixelart_", 0) == 0;
        const bool runtime_file = name == CommandSpoolBuilder::kVerificationPlanName ||
            name == std::string(CommandSpoolBuilder::kVerificationPlanName) + ".tmp" ||
            name == "checkpoint.bin" || name == "checkpoint.bin.tmp" ||
            name == CommandBlockSpoolWriter::kFileName ||
            name == std::string(CommandBlockSpoolWriter::kFileName) + ".tmp" ||
            name == CommandBlockSpoolWriter::kManifestFileName ||
            name == std::string(CommandBlockSpoolWriter::kManifestFileName) + ".tmp" ||
             name == kCommandBlockStateFileName ||
             name == std::string(kCommandBlockStateFileName) + ".tmp" ||
             name == SignSpoolWriter::kFileName ||
             name == std::string(SignSpoolWriter::kFileName) + ".tmp" ||
             name == kSignStateFileName ||
             name == std::string(kSignStateFileName) + ".tmp" ||
             name == EntitySpoolWriter::kFileName ||
             name == std::string(EntitySpoolWriter::kFileName) + ".tmp" ||
             name == kEntityStateFileName ||
             name == std::string(kEntityStateFileName) + ".tmp" ||
             name == ContainerItemSpoolWriter::kFileName ||
             name == std::string(ContainerItemSpoolWriter::kFileName) + ".tmp" ||
             name == kContainerItemStateFileName ||
             name == std::string(kContainerItemStateFileName) + ".tmp" ||
             name == kMapCreationStateFileName ||
             name == std::string(kMapCreationStateFileName) + ".tmp" ||
             name == std::string(kMapCreationStateFileName) + kMapUsePendingSuffix ||
             name == std::string(kMapCreationStateFileName) + kMapUsePendingSuffix + ".tmp" ||
             name == std::string(kMapCreationStateFileName) + ".blank-supply.pending" ||
             name == std::string(kMapCreationStateFileName) + ".blank-supply.pending.tmp";
        if (chunk_spool || parser_temp || runtime_file) {
            std::remove((directory + "/" + name).c_str());
        }
    }
    closedir(raw_directory);
#if defined(_WIN32)
    _rmdir(directory.c_str());
#else
    rmdir(directory.c_str());
#endif
}

void removeRawChunkSpools(const std::vector<ChunkDescriptor>& chunks) {
    for (const ChunkDescriptor& chunk : chunks) {
        for (const std::string& path : chunk.spool_paths) {
            if (!path.empty()) std::remove(path.c_str());
        }
    }
}

bool containsBounds(const BlockBounds& outer, const BlockBounds& inner) {
    return outer.isValid() && inner.isValid() &&
        inner.min_x >= outer.min_x && inner.max_x <= outer.max_x &&
        inner.min_y >= outer.min_y && inner.max_y <= outer.max_y &&
        inner.min_z >= outer.min_z && inner.max_z <= outer.max_z;
}

void extendBounds(BlockBounds* target, const BlockBounds& addition) {
    if (!target || !addition.isValid()) return;
    if (!target->isValid()) {
        *target = addition;
        return;
    }
    target->min_x = std::min(target->min_x, addition.min_x);
    target->min_y = std::min(target->min_y, addition.min_y);
    target->min_z = std::min(target->min_z, addition.min_z);
    target->max_x = std::max(target->max_x, addition.max_x);
    target->max_y = std::max(target->max_y, addition.max_y);
    target->max_z = std::max(target->max_z, addition.max_z);
}

bool addBlockCount(uint64_t value, uint64_t* total) {
    if (!total || value > std::numeric_limits<uint64_t>::max() - *total) return false;
    *total += value;
    return true;
}

bool catalogExistingSpools(const std::string& directory,
                           std::vector<ChunkDescriptor>* output,
                           uint64_t* total_blocks, int32_t chunk_size,
                           int32_t region_span, bool region_spools,
                           const ChunkCoord& region_grid_origin,
                           std::string* error) {
    if (!output || chunk_size <= 0 || region_span <= 0) {
        if (error) *error = "checkpoint partition geometry is invalid";
        return false;
    }
    std::vector<VerificationChunkPlan> verification_chunks;
    if (!CommandSpoolBuilder::loadVerificationPlan(
            directory + "/" + CommandSpoolBuilder::kVerificationPlanName,
            &verification_chunks, error)) {
        return false;
    }
    std::map<ChunkCoord, VerificationChunkPlan> metadata;
    for (VerificationChunkPlan& chunk : verification_chunks) {
        if (!(chunkForBlock(chunk.imported_bounds.min_x, chunk.imported_bounds.min_z,
                            chunk_size) == chunk.coord) ||
            !(chunkForBlock(chunk.imported_bounds.max_x, chunk.imported_bounds.max_z,
                            chunk_size) == chunk.coord)) {
            if (error) *error = "verification-plan bounds are outside their chunk";
            return false;
        }
        if (!metadata.emplace(chunk.coord, std::move(chunk)).second) {
            if (error) *error = "verification plan contains duplicate chunks";
            return false;
        }
    }
    struct PlanGroup {
        ChunkCoord representative;
        BlockBounds imported_bounds;
        uint64_t total_block_count = 0;
        bool initialized = false;
    };
    std::map<ChunkCoord, PlanGroup> plan_groups;
    for (const auto& entry : metadata) {
        const ChunkCoord key = region_spools
            ? regionForChunk(entry.first, region_span, region_grid_origin) : entry.first;
        PlanGroup& group = plan_groups[key];
        if (!group.initialized) {
            group.representative = entry.first;
            group.initialized = true;
        }
        extendBounds(&group.imported_bounds, entry.second.imported_bounds);
        if (!addBlockCount(entry.second.total_block_count, &group.total_block_count)) {
            if (error) *error = "verification-plan region total overflows";
            return false;
        }
    }

    DIR* raw_directory = opendir(directory.c_str());
    if (!raw_directory) {
        if (error) *error = "cannot open import spool directory";
        return false;
    }
    std::map<ChunkCoord, ChunkDescriptor> catalog;
    while (dirent* entry = readdir(raw_directory)) {
        int x = 0, z = 0, phase_value = 0, consumed = 0;
        const char* format = region_spools
            ? "region_%d_%d_phase_%d.bsp.cmd%n"
            : "chunk_%d_%d_phase_%d.bsp.cmd%n";
        if (std::sscanf(entry->d_name, format, &x, &z, &phase_value, &consumed) != 3 ||
            entry->d_name[consumed] != '\0' ||
            phase_value <= static_cast<int>(ImportPhase::Clear) ||
            phase_value >= static_cast<int>(ImportPhase::Count)) continue;
        const ChunkCoord key{x, z};
        const auto plan = plan_groups.find(key);
        if (plan == plan_groups.end()) {
            closedir(raw_directory);
            if (error) *error = "command spool has no verification-plan partition";
            return false;
        }
        const std::string path = directory + "/" + entry->d_name;
        const BlockBounds& imported_bounds = plan->second.imported_bounds;
        CommandSpoolReader reader(path);
        if (!reader.valid() || reader.commandCount() == 0 || reader.totalBlockCount() == 0) {
            closedir(raw_directory);
            if (error) *error = "cannot read merged command spool catalog entry";
            return false;
        }
        while (!reader.exhausted()) {
            const std::optional<PlannedCommand> command = reader.next();
            const ChunkCoord minimum_chunk = command
                ? chunkForBlock(command->bounds.min_x, command->bounds.min_z, chunk_size)
                : ChunkCoord{};
            const ChunkCoord maximum_chunk = command
                ? chunkForBlock(command->bounds.max_x, command->bounds.max_z, chunk_size)
                : ChunkCoord{};
            const bool correct_partition = command && (region_spools
                ? regionForChunk(minimum_chunk, region_span, region_grid_origin) == key &&
                  regionForChunk(maximum_chunk, region_span, region_grid_origin) == key
                : minimum_chunk == key && maximum_chunk == key);
            if (!command || !containsBounds(imported_bounds, command->bounds) ||
                !correct_partition) {
                closedir(raw_directory);
                if (error) {
                    *error = "merged command spool command is corrupt or outside its partition";
                }
                return false;
            }
        }
        if (reader.failed()) {
            closedir(raw_directory);
            if (error) *error = "merged command spool catalog entry is corrupt";
            return false;
        }
        ChunkDescriptor& descriptor = catalog[key];
        descriptor.coord = plan->second.representative;
        descriptor.region_grid_origin = region_grid_origin;
        descriptor.imported_bounds = plan->second.imported_bounds;
        const size_t phase = static_cast<size_t>(phase_value);
        if (descriptor.has_phase[phase]) {
            closedir(raw_directory);
            if (error) *error = "duplicate merged command spool phase";
            return false;
        }
        descriptor.has_phase[phase] = true;
        descriptor.command_paths[phase] = path;
        descriptor.phase_block_counts[phase] = reader.totalBlockCount();
    }
    closedir(raw_directory);

    output->clear();
    uint64_t aggregate = 0;
    for (const auto& plan : plan_groups) {
        auto found = catalog.find(plan.first);
        if (found == catalog.end()) {
            if (plan.second.total_block_count != 0) {
                if (error) *error = "verification-plan partition has no merged command spool";
                return false;
            }
            ChunkDescriptor empty;
            empty.coord = plan.second.representative;
            empty.region_grid_origin = region_grid_origin;
            empty.imported_bounds = plan.second.imported_bounds;
            found = catalog.emplace(plan.first, std::move(empty)).first;
        }
        ChunkDescriptor& descriptor = found->second;
        uint64_t chunk_total = 0;
        for (size_t phase = 1; phase < kImportPhaseCount; ++phase) {
            if (!descriptor.has_phase[phase]) continue;
            if (descriptor.phase_block_counts[phase] >
                std::numeric_limits<uint64_t>::max() - chunk_total) {
                if (error) *error = "merged command spool partition total overflows";
                return false;
            }
            chunk_total += descriptor.phase_block_counts[phase];
        }
        if (chunk_total != plan.second.total_block_count) {
            if (error) *error = "merged command spools do not match verification-plan totals";
            return false;
        }
        if (chunk_total > std::numeric_limits<uint64_t>::max() - aggregate) {
            if (error) *error = "merged command spool catalog total overflows";
            return false;
        }
        aggregate += chunk_total;
        output->push_back(std::move(descriptor));
    }
    if (output->empty()) {
        if (error) *error = "no persistent chunk spools were found";
        return false;
    }
    if (total_blocks) *total_blocks = aggregate;
    return true;
}

bool sameChunk(const ChunkCoord& left, const ChunkCoord& right) {
    return left.x == right.x && left.z == right.z;
}

bool sameBounds(const BlockBounds& left, const BlockBounds& right) {
    return left.min_x == right.min_x && left.min_y == right.min_y &&
           left.min_z == right.min_z && left.max_x == right.max_x &&
           left.max_y == right.max_y && left.max_z == right.max_z;
}

bool mapStorageSafeArtworkFloor(const NativeBlockInfo& block) {
    if (ClassifyMapChestNativeBlock(block) == MapChestCell::SolidSupport) {
        return true;
    }
    // The pixel-art palette uses legacy auxiliary-colour identifiers. They
    // are full cubes even though the generic chest-site classifier does not
    // know their bare legacy names.
    return block.name == "minecraft:wool" ||
           block.name == "minecraft:stained_hardened_clay" ||
           block.name == "minecraft:concrete";
}

struct MapStorageLandingObservation {
    NativeBlockInfo ground;
    NativeBlockInfo feet;
    NativeBlockInfo head;
    bool ground_read = false;
    bool feet_read = false;
    bool head_read = false;

    bool safe() const {
        return ground_read && feet_read && head_read &&
            mapStorageSafeArtworkFloor(ground) &&
            ClassifyMapChestNativeBlock(feet) == MapChestCell::Air &&
            ClassifyMapChestNativeBlock(head) == MapChestCell::Air;
    }
};

MapStorageLandingObservation observeSafeMapStorageLanding(
        NativeWorldReader* reader, const MapChestPosition& floor) {
    MapStorageLandingObservation observation;
    if (!reader || floor.y > std::numeric_limits<int32_t>::max() - 2) {
        return observation;
    }
    observation.ground_read = reader->getBlock(
        floor.x, floor.y, floor.z, &observation.ground);
    observation.feet_read = reader->getBlock(
        floor.x, floor.y + 1, floor.z, &observation.feet);
    observation.head_read = reader->getBlock(
        floor.x, floor.y + 2, floor.z, &observation.head);
    return observation;
}

bool readSafeMapStorageLanding(NativeWorldReader* reader,
                               const MapChestPosition& floor) {
    return observeSafeMapStorageLanding(reader, floor).safe();
}

bool findSafeMapStoragePlacementLanding(
        NativeWorldReader* reader, const BlockBounds& artwork,
        const MapChestPosition& target, MapChestPosition* floor) {
    if (!reader || !floor || !artwork.isValid() ||
        artwork.max_y > std::numeric_limits<int32_t>::max() - 2) {
        return false;
    }
    const int32_t centre_x = std::clamp(target.x, artwork.min_x, artwork.max_x);
    const int32_t centre_z = std::clamp(target.z, artwork.min_z, artwork.max_z);
    constexpr int32_t kSearchRadius = 8;
    constexpr int64_t kMaximumLandingDistanceSquared = 24LL * 24LL;
    for (int32_t radius = 0; radius <= kSearchRadius; ++radius) {
        for (int32_t dx = -radius; dx <= radius; ++dx) {
            for (int32_t dz = -radius; dz <= radius; ++dz) {
                if (std::max(std::abs(dx), std::abs(dz)) != radius) continue;
                const int64_t x = static_cast<int64_t>(centre_x) + dx;
                const int64_t z = static_cast<int64_t>(centre_z) + dz;
                if (x < artwork.min_x || x > artwork.max_x ||
                    z < artwork.min_z || z > artwork.max_z) continue;
                const int64_t from_target_x = x - target.x;
                const int64_t from_target_z = z - target.z;
                if (from_target_x * from_target_x +
                    from_target_z * from_target_z >
                        kMaximumLandingDistanceSquared) continue;
                const MapChestPosition candidate{
                    static_cast<int32_t>(x), artwork.max_y,
                    static_cast<int32_t>(z)};
                if (readSafeMapStorageLanding(reader, candidate)) {
                    *floor = candidate;
                    return true;
                }
            }
        }
    }
    return false;
}

// Own every record referenced by the classifier until its synchronous tick
// returns. A fresh load on each tick is the only source of journal phase;
// neither a packet ACK nor an in-memory cursor substitutes for these files.
struct MapStorageRuntimeSnapshot {
    MapStorageCoordinatorInput input;
    std::optional<MapPairPlacementRecord> pair;
    std::optional<MapAnvilRenameRecord> rename;
    std::optional<MapAnvilInputProof> fresh_anvil_input;
    std::optional<MapChestTransferRecord> chest;
    bool has_storage_journals = false;
};

// Lives only for one synchronous driver tick. No native window or ItemStack
// pointer is ever stored here; every hook re-reads its own current evidence.
struct MapStorageAnvilRuntimeHookContext {
    BuildImportRuntime* runtime = nullptr;
    const MapStorageProductionHooks* storage_hooks = nullptr;
    int32_t expected_runtime_item_id = 0;
    int64_t expected_map_uuid = -1;
};

bool loadMapStorageRuntimeSnapshot(
        const std::string& map_state_path, const WorldContext& world,
        const BlockBounds& artwork_bounds, uint64_t columns, uint64_t rows,
        uint64_t tile_count, uint64_t checkpoint_cursor, int64_t map_uuid,
        MapStorageRuntimeSnapshot* output, std::string* error) {
    if (!output || map_state_path.empty() || columns == 0U || rows == 0U ||
        columns > UINT32_MAX || rows > UINT32_MAX || tile_count == 0U ||
        tile_count > kMaximumMapTileCount || checkpoint_cursor > tile_count) {
        if (error) *error = "map storage snapshot plan is invalid";
        return false;
    }
    *output = {};
    auto& input = output->input;
    input.world_id = world.world_id;
    input.dimension_id = world.dimension_id;
    input.artwork_bounds = artwork_bounds;
    input.columns = static_cast<uint32_t>(columns);
    input.rows = static_cast<uint32_t>(rows);
    input.tile_count = tile_count;
    input.checkpoint_tile_cursor = checkpoint_cursor;
    input.expected_map_uuid = map_uuid;

    const std::string marker_path = mapUsePendingPath(map_state_path);
    if (commandBlockStateExists(marker_path)) {
        MapUsePendingDiskV1 marker;
        if (!loadMapUsePending(map_state_path, &marker, error)) return false;
        input.has_pending_map_use_marker = true;
        input.pending_map_use_cursor = marker.tile_cursor;
    }

    MapPairPlacementRecord pair;
    const auto pair_load = LoadMapPairPlacementJournal(map_state_path, &pair, error);
    if (pair_load == MapPairPlacementLoad::Unsafe) return false;
    if (pair_load == MapPairPlacementLoad::Loaded) {
        if (ClassifyMapPairPlacementResume(pair, world.world_id,
                world.dimension_id, tile_count, artwork_bounds) ==
            MapPairPlacementResume::Unsafe) {
            if (error) *error = "map storage pair belongs to another world or plan";
            return false;
        }
        output->pair = std::move(pair);
        input.pair = &*output->pair;
        output->has_storage_journals = true;
    }
    MapAnvilRenameRecord rename;
    const auto rename_load = LoadMapAnvilRenameJournal(map_state_path, &rename, error);
    if (rename_load == MapAnvilRenameLoad::Unsafe) return false;
    if (rename_load == MapAnvilRenameLoad::Loaded) {
        if (rename.world_id != world.world_id ||
            rename.dimension_id != world.dimension_id ||
            rename.tile_count != tile_count || rename.columns != columns ||
            rename.rows != rows) {
            if (error) *error = "map storage rename belongs to another world or plan";
            return false;
        }
        output->rename = std::move(rename);
        input.rename = &*output->rename;
        output->has_storage_journals = true;
    }
    MapChestTransferRecord chest;
    const auto chest_load = LoadMapChestTransferJournal(map_state_path, &chest, error);
    if (chest_load == MapChestJournalLoad::Unsafe) return false;
    if (chest_load == MapChestJournalLoad::Loaded) {
        if (chest.world_id != world.world_id ||
            chest.dimension_id != world.dimension_id ||
            chest.tile_count != tile_count ||
            (input.rename && chest.source_map_uuid != input.rename->map_uuid)) {
            if (error) *error = "map storage chest belongs to another map or world";
            return false;
        }
        output->chest = std::move(chest);
        input.chest = &*output->chest;
        output->has_storage_journals = true;
    }
    const uint64_t extra_count = (tile_count + kMapChestSlotCount - 1U) /
        kMapChestSlotCount - 1U;
    bool gap = false;
    for (uint64_t index = 1U; index <= extra_count; ++index) {
        MapExtraChestPlacementRecord extra;
        const auto extra_load = LoadMapExtraChestPlacementJournal(
            map_state_path, static_cast<uint16_t>(index), &extra, error);
        if (extra_load == MapExtraChestPlacementLoad::Unsafe) return false;
        if (extra_load == MapExtraChestPlacementLoad::Missing) {
            gap = true;
            continue;
        }
        if (gap || ClassifyMapExtraChestPlacementResume(
                extra, world.world_id, world.dimension_id, tile_count,
                artwork_bounds, static_cast<uint16_t>(index)) ==
                MapExtraChestPlacementResume::Unsafe) {
            if (error) *error = "map storage extra chest chain has a gap or mismatched plan";
            return false;
        }
        input.extra_chests.push_back(std::move(extra));
        output->has_storage_journals = true;
    }
    if (error) error->clear();
    return true;
}

bool sameUndoRegions(const std::vector<BuildImportUndoRegion>& manifest_regions,
                     const std::vector<ChunkDescriptor>& descriptors) {
    if (manifest_regions.size() != descriptors.size()) return false;
    std::map<ChunkCoord, BlockBounds> expected;
    for (const BuildImportUndoRegion& region : manifest_regions) {
        if (!expected.emplace(region.coord, region.bounds).second) return false;
    }
    for (const ChunkDescriptor& descriptor : descriptors) {
        const auto found = expected.find(descriptor.coord);
        if (found == expected.end() || !sameBounds(found->second, descriptor.imported_bounds)) {
            return false;
        }
    }
    return true;
}

bool completedUndoVolumeForCheckpoint(const CheckpointSnapshot& snapshot,
                                      const std::vector<ChunkDescriptor>& descriptors,
                                      uint64_t* completed) {
    if (!completed) return false;
    *completed = 0;
    std::vector<const ChunkDescriptor*> ordered;
    ordered.reserve(descriptors.size());
    for (const ChunkDescriptor& descriptor : descriptors) ordered.push_back(&descriptor);
    std::sort(ordered.begin(), ordered.end(), [](const ChunkDescriptor* left,
                                                 const ChunkDescriptor* right) {
        return left->coord < right->coord;
    });
    size_t completed_chunks = 0;
    if (snapshot.chunk_index == descriptors.size() && snapshot.phase_index == 0) {
        completed_chunks = descriptors.size();
    } else if (snapshot.phase_index == phaseIndex(ImportPhase::Structure) &&
               snapshot.chunk_index <= descriptors.size()) {
        completed_chunks = snapshot.chunk_index;
    }
    for (size_t index = 0; index < completed_chunks; ++index) {
        if (!addBoundsVolume(ordered[index]->imported_bounds, completed)) return false;
    }
    return true;
}

enum class DirectoryRelation { Same, Different, Unknown };

DirectoryRelation compareExistingDirectories(const std::string& left,
                                             const std::string& right) {
    if (left.empty() || right.empty()) return DirectoryRelation::Unknown;
    struct stat left_status {};
    struct stat right_status {};
    if (stat(left.c_str(), &left_status) != 0 || stat(right.c_str(), &right_status) != 0 ||
        !S_ISDIR(left_status.st_mode) || !S_ISDIR(right_status.st_mode)) {
        return DirectoryRelation::Unknown;
    }
#if defined(_WIN32)
    // MSVCRT may report zero inode values for directories. A matching nonzero
    // file identity is trustworthy; otherwise retain the old directory.
    if (left_status.st_ino != 0 && right_status.st_ino != 0 &&
        left_status.st_dev == right_status.st_dev && left_status.st_ino == right_status.st_ino) {
        return DirectoryRelation::Same;
    }
    return DirectoryRelation::Unknown;
#else
    return left_status.st_dev == right_status.st_dev && left_status.st_ino == right_status.st_ino
        ? DirectoryRelation::Same : DirectoryRelation::Different;
#endif
}

bool isContextSensitiveState(ImportState state) {
    return state == ImportState::Planning || state == ImportState::Running ||
           state == ImportState::Paused || state == ImportState::Verifying;
}

bool isRestoreBusyState(ImportState state) {
    return state == ImportState::Planning || state == ImportState::Running ||
           state == ImportState::Paused || state == ImportState::Verifying ||
           state == ImportState::ClosedForContextChange;
}

bool hasCaseInsensitiveSuffix(const std::string& value, const char* suffix) {
    const size_t suffix_length = std::strlen(suffix);
    if (value.size() < suffix_length) return false;
    const size_t offset = value.size() - suffix_length;
    for (size_t index = 0; index < suffix_length; ++index) {
        const unsigned char left = static_cast<unsigned char>(value[offset + index]);
        const unsigned char right = static_cast<unsigned char>(suffix[index]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

bool validateRestorablePlan(const CheckpointSnapshot& snapshot, std::string* error) {
    if (snapshot.format_version < kOldestCurrentCommandPlanCheckpointVersion ||
        snapshot.format_version > kBuildImportCheckpointCurrentVersion) {
        if (error) {
            *error = "legacy checkpoint command plans must be imported again with the current parser";
        }
        return false;
    }
    if (!isCompatiblePlanVersion(snapshot.identity.options_hash, snapshot.config)) {
        if (error) *error = "checkpoint command-plan version is incompatible with this build";
        return false;
    }
    if (snapshot.config.source_type == ImportSourceType::DeprecatedCommandMusicMp3) {
        if (error) {
            *error = "this checkpoint was created by the removed MP3 command-music importer; "
                     "start a new .mid or .midi import";
        }
        return false;
    }
    const bool litematic = hasCaseInsensitiveSuffix(
        snapshot.identity.source_file, ".litematic");
    const bool bdx = hasCaseInsensitiveSuffix(snapshot.identity.source_file, ".bdx");
    const bool mcworld = hasCaseInsensitiveSuffix(snapshot.identity.source_file, ".mcworld");
    const bool midi_extension = hasCaseInsensitiveSuffix(snapshot.identity.source_file, ".mid") ||
        hasCaseInsensitiveSuffix(snapshot.identity.source_file, ".midi");
    const bool midi = snapshot.config.source_type == ImportSourceType::CommandMusicMidi;
    const bool infinitecz = hasCaseInsensitiveSuffix(
        snapshot.identity.source_file, ".infinity") ||
        hasCaseInsensitiveSuffix(snapshot.identity.source_file, ".IBuild");
    if (midi != midi_extension) {
        if (error) *error = "MIDI checkpoint source type does not match its source file";
        return false;
    }
    const bool mapper_compatible = midi
        ? snapshot.identity.mapper_version == MidiCommandMusicParser::kVersion
        : infinitecz
        ? snapshot.identity.mapper_version == InfiniteczBuildParser::kVersion
        : mcworld
        ? snapshot.identity.mapper_version == McworldParser::kVersion
        : bdx
        ? snapshot.identity.mapper_version == BdxParser::kVersion
        : litematic
            ? snapshot.identity.mapper_version == LitematicParser::kVersion
            : snapshot.identity.mapper_version == BlockMapper::kVersion ||
              snapshot.identity.mapper_version == PixelArtParser::kVersion;
    if (!mapper_compatible) {
        if (error) {
            *error = midi
                ? "MIDI checkpoint uses an obsolete parser or command-music layout; start a new import"
                : infinitecz
                ? "Infinitecz checkpoint uses an obsolete native format version; start a new import"
                : mcworld
                ? "mcworld checkpoint uses an obsolete LevelDB decoder or block mapping; start a new import"
                : bdx
                ? "bdx checkpoint uses an obsolete decoder or block mapping; start a new import"
                : litematic
                    ? "litematic checkpoint uses an obsolete coordinate layout; start a new import"
                    : "checkpoint block-mapper version is incompatible with this build";
        }
        return false;
    }
    return true;
}

std::vector<VerificationPlanSample> selectVerificationSamples(
        const std::vector<VerificationPlanSample>& samples,
        VerificationPrecision precision) {
    std::vector<VerificationPlanSample> stable;
    std::vector<VerificationPlanSample> air;
    stable.reserve(samples.size());
    air.reserve(samples.size());
    for (const VerificationPlanSample& sample : samples) {
        if (hasVerificationSampleFlag(sample, VerificationSampleFlag::ExpectedAir)) {
            air.push_back(sample);
        } else if (BlockMapper::isStableVerificationBlockName(sample.name)) {
            stable.push_back(sample);
        } else {
            const std::string leaf = blockLeaf(sample.name);
            const bool fluid = leaf == "water" || leaf == "flowing_water" ||
                leaf == "lava" || leaf == "flowing_lava";
            if (fluid) {
                VerificationPlanSample normalized = sample;
                normalized.flags |= static_cast<uint8_t>(VerificationSampleFlag::IgnoreAux);
                stable.push_back(std::move(normalized));
            }
        }
    }

    std::vector<VerificationPlanSample> selected;
    selected.reserve(std::min<size_t>(stable.size(), verificationStableSampleLimit(precision)) +
                     std::min<size_t>(air.size(), verificationAirSampleLimit(precision)));
    const auto append_evenly = [&selected](const std::vector<VerificationPlanSample>& source,
                                           uint32_t requested) {
        const size_t count = std::min<size_t>(source.size(), requested);
        if (count == 0) return;
        for (size_t index = 0; index < count; ++index) {
            // Midpoints cover the complete reservoir, including the reserved
            // source/flowing fluid samples appended after structure samples.
            const size_t source_index = std::min(
                source.size() - 1,
                ((index * 2 + 1) * source.size()) / (count * 2));
            selected.push_back(source[source_index]);
        }
    };
    append_evenly(stable, verificationStableSampleLimit(precision));
    append_evenly(air, verificationAirSampleLimit(precision));
    return selected;
}

bool containsPermissionError(const std::string& detail) {
    return detail.find("permission") != std::string::npos ||
           detail.find("not allowed") != std::string::npos ||
           detail.find("denied") != std::string::npos ||
           detail.find("没有权限") != std::string::npos ||
           detail.find("无权限") != std::string::npos ||
           detail.find("权限不足") != std::string::npos;
}

}  // namespace

class CommandBatchPrefetcher {
public:
    using Formatter = std::function<std::string(const PlannedCommand&)>;

    struct TakeResult {
        size_t command_count = 0;
        uint64_t block_count = 0;
        bool exhausted = false;
        bool failed = false;
    };

    CommandBatchPrefetcher(std::string path, uint64_t expected_blocks, Formatter formatter)
        : path_(std::move(path)), expected_blocks_(expected_blocks),
          formatter_(std::move(formatter)) {
        CommandSpoolReader probe(path_);
        if (!probe.valid() || probe.totalBlockCount() != expected_blocks_) {
            error_ = "cannot open or validate merged command spool";
            failed_ = true;
            done_ = true;
            return;
        }
        command_count_ = probe.commandCount();
        valid_ = true;
        worker_ = std::thread(&CommandBatchPrefetcher::run, this);
    }

    ~CommandBatchPrefetcher() { stop(); }

    CommandBatchPrefetcher(const CommandBatchPrefetcher&) = delete;
    CommandBatchPrefetcher& operator=(const CommandBatchPrefetcher&) = delete;

    bool valid() const noexcept { return valid_; }

    uint64_t remainingCommands() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return command_count_ >= consumed_commands_
            ? command_count_ - consumed_commands_ : 0;
    }

    bool exhausted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return done_ && queue_.empty() && !failed_ && consumed_commands_ == command_count_;
    }

    bool failed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed_;
    }

    std::string error() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

    TakeResult take(size_t maximum_commands, double available_block_tokens,
                    double available_command_tokens,
                    std::chrono::steady_clock::time_point deadline,
                    std::vector<std::string>* commands,
                    std::vector<uint32_t>* block_counts) {
        TakeResult result;
        if (!commands || !block_counts || maximum_commands == 0) return result;
        commands->clear();
        block_counts->clear();
        commands->reserve(maximum_commands);
        block_counts->reserve(maximum_commands);

        std::unique_lock<std::mutex> lock(mutex_);
        while (!queue_.empty() && commands->size() < maximum_commands &&
               available_block_tokens >= 1.0 && available_command_tokens >= 1.0 &&
               static_cast<double>(queue_.front().block_count) <= available_block_tokens &&
               !commandBatchDeadlineReached(commands->size(), deadline)) {
            PreparedCommand& prepared = queue_.front();
            const uint32_t blocks = prepared.block_count;
            const size_t bytes = prepared.command.size();
            commands->push_back(std::move(prepared.command));
            block_counts->push_back(blocks);
            queue_.pop_front();
            queued_bytes_ -= std::min(queued_bytes_, bytes);
            available_block_tokens -= static_cast<double>(blocks);
            available_command_tokens -= 1.0;
            result.block_count += blocks;
            ++result.command_count;
            ++consumed_commands_;
        }
        result.failed = failed_;
        result.exhausted = done_ && queue_.empty() && !failed_ &&
            consumed_commands_ == command_count_;
        lock.unlock();
        if (result.command_count != 0) space_available_.notify_one();
        return result;
    }

    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
        }
        space_available_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

private:
    struct PreparedCommand {
        std::string command;
        uint32_t block_count = 0;
    };

    void fail(std::string detail) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
        done_ = true;
        error_ = std::move(detail);
    }

    void run() noexcept {
        try {
            CommandSpoolReader reader(path_);
            if (!reader.valid() || reader.commandCount() != command_count_ ||
                reader.totalBlockCount() != expected_blocks_) {
                fail("merged command spool changed while prefetching");
                return;
            }
            uint64_t produced = 0;
            while (true) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (stop_requested_) return;
                }
                const std::optional<PlannedCommand> planned = reader.next();
                if (!planned) break;
                PreparedCommand prepared{formatter_(*planned), planned->block_count};
                const size_t bytes = prepared.command.size();
                std::unique_lock<std::mutex> lock(mutex_);
                space_available_.wait(lock, [&]() {
                    return stop_requested_ ||
                        (queue_.size() < kMaximumQueuedCommands &&
                         (queued_bytes_ + bytes <= kMaximumQueuedBytes || queue_.empty()));
                });
                if (stop_requested_) return;
                queued_bytes_ += bytes;
                queue_.push_back(std::move(prepared));
                ++produced;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (reader.failed() || produced != command_count_) {
                failed_ = true;
                error_ = "corrupt merged command spool";
            }
            done_ = true;
        } catch (const std::exception& exception) {
            fail(std::string("command prefetch failed: ") + exception.what());
        } catch (...) {
            fail("command prefetch failed unexpectedly");
        }
    }

    static constexpr size_t kMaximumQueuedCommands = 2048;
    static constexpr size_t kMaximumQueuedBytes = 2U * 1024U * 1024U;

    std::string path_;
    uint64_t expected_blocks_ = 0;
    Formatter formatter_;
    bool valid_ = false;
    uint64_t command_count_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable space_available_;
    std::deque<PreparedCommand> queue_;
    size_t queued_bytes_ = 0;
    uint64_t consumed_commands_ = 0;
    bool stop_requested_ = false;
    bool done_ = false;
    bool failed_ = false;
    std::string error_;
    std::thread worker_;
};

BuildImportRuntime& BuildImportRuntime::instance() {
    static BuildImportRuntime runtime;
    return runtime;
}

BuildImportRuntime::~BuildImportRuntime() {
    resetCurrentRun(true);
}

bool BuildImportRuntime::start(BuildImportStartRequest request, std::string* error) try {
    if (restore_in_progress_.load(std::memory_order_acquire)) {
        if (error) *error = "cannot start an import while checkpoint restore is in progress";
        return false;
    }
    if (request.source_type == ImportSourceType::DeprecatedCommandMusicMp3) {
        if (error) *error = "MP3 command-music import has been removed; use a .mid or .midi file";
        return false;
    }
    request.config.source_type = request.source_type;
    request.config.create_maps_after_import =
        request.source_type == ImportSourceType::PixelArtPng &&
        request.pixel_art.create_maps_after_import;
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (restore_in_progress_.load(std::memory_order_acquire)) {
        if (error) *error = "cannot start an import while checkpoint restore is in progress";
        return false;
    }
    // The user-facing range is expressed in real 16x16 Minecraft chunks.  Do
    // not let stale Java defaults leave the parsers on their historical 32-block
    // grid, otherwise a requested 4..8 range would not be exact.
    if (request.config.simulation_chunk_range != 0) {
        request.config.chunk_size = ImportConfig::kVanillaChunkSize;
    }
    request.parse.chunk_size = request.config.chunk_size;
    request.pixel_art.chunk_size = request.config.chunk_size;
    // Preserve-existing imports normally never need air-only logical chunks.
    // The optional deny foundation is different: it must cover the complete
    // source-volume footprint, including sparse partitions with no source
    // block records, so keep those descriptors when either feature is active.
    const bool include_source_volume = request.config.overwrite_policy ==
        OverwritePolicy::ClearImportedBounds || request.config.place_deny_layer ||
        (request.source_type == ImportSourceType::PixelArtPng &&
         request.pixel_art.create_maps_after_import);
    request.parse.include_source_volume = include_source_volume;
    request.pixel_art.include_source_volume = include_source_volume;

    const std::string requested_spool_directory =
        request.source_type == ImportSourceType::PixelArtPng
        ? request.pixel_art.spool_directory : request.parse.spool_directory;
    if (isRestoreBusyState(controller_.state())) {
        if (error) *error = "an import is already active or can still be resumed";
        return false;
    }
    if (worker_.joinable()) {
        if (worker_running_.load(std::memory_order_acquire)) {
            if (error) *error = "the previous parser is still stopping";
            return false;
        }
        worker_.join();
    }
    cancel();
    command_feedback_suppression_unavailable_ = false;
    command_feedback_failure_count_ = 0;
    command_feedback_retry_at_ = {};
    const char* expected_mapper = request.source_type == ImportSourceType::PixelArtPng
        ? PixelArtParser::kVersion
        : request.source_type == ImportSourceType::CommandMusicMidi
            ? MidiCommandMusicParser::kVersion
        : request.source_type == ImportSourceType::Litematic
            ? LitematicParser::kVersion
            : request.source_type == ImportSourceType::Bdx
                ? BdxParser::kVersion
                : request.source_type == ImportSourceType::Mcworld
                    ? McworldParser::kVersion
                    : request.source_type == ImportSourceType::InfiniteczBuild
                        ? InfiniteczBuildParser::kVersion : BlockMapper::kVersion;
    if (!request.identity.mapper_version.empty() &&
        request.identity.mapper_version != expected_mapper) {
        if (error) *error = "requested block-mapper version is incompatible with this build";
        return false;
    }
    request.identity.mapper_version = expected_mapper;
    const bool aggregate_regions = regionSpanForImportConfig(request.config) > 1;
    const char* expected_options = request.config.overwrite_policy ==
            OverwritePolicy::ClearImportedBounds
        ? (aggregate_regions ? kClearPlanVersion : kLegacyClearPlanVersion)
        : (aggregate_regions ? kPreservePlanVersion : kLegacyPreservePlanVersion);
    if (!request.identity.options_hash.empty() &&
        request.identity.options_hash != expected_options) {
        if (error) *error = "requested command-plan version is incompatible with this build";
        return false;
    }
    request.identity.options_hash = expected_options;
    current_world_ = request.world;
    spool_directory_ = requested_spool_directory;
    map_creation_requested_ = request.source_type == ImportSourceType::PixelArtPng &&
        request.pixel_art.create_maps_after_import;
    map_creation_completed_ = false;
    map_state_path_ = map_creation_requested_
        ? requested_spool_directory + "/" + kMapCreationStateFileName
        : std::string();
    undo_storage_directory_ = undoStorageDirectoryForSpool(requested_spool_directory);
    undo_record_id_.clear();
    undo_claim_job_id_.clear();
    durable_undo_block_count_ = 0;
    undo_run_active_ = false;
    LOGI("[lifecycle] start source=%d world=%s dimension=%d rate=%d spool=%s",
         static_cast<int>(request.source_type), request.world.world_id.c_str(),
         request.world.dimension_id, request.config.blocks_per_second,
         requested_spool_directory.c_str());
    if (!controller_.startPlanning(request.identity, request.world, request.config, error)) {
        spool_directory_.clear();
        current_world_ = {};
        return false;
    }
    region_command_spools_ = usesRegionCommandSpools(request.identity.options_hash);
    adaptive_region_grid_ = usesAdaptiveRegionGrid(request.identity.options_hash);
    WorkUnit stale_area;
    const std::string stale_cleanup = controller_.makeCleanupCommand(stale_area);
    if (std::find(pending_cleanup_commands_.begin(), pending_cleanup_commands_.end(),
                  stale_cleanup) == pending_cleanup_commands_.end()) {
        pending_cleanup_commands_.push_back(stale_cleanup);
    }
    cleanup_retry_at_ = {};
    cleanup_barrier_failure_count_ = 0;

    recovery_pending_ = false;
    phase_settle_ready_at_ = {};
    loaded_region_.reset();
    loaded_region_cleanup_command_.clear();
    loaded_region_probe_bounds_ = {};
    loaded_region_fully_confirmed_ = false;
    loaded_region_started_at_ = {};
    confirmed_region_chunks_.clear();
    native_dimension_token_ = GetCachedDimensionTokenForWorld(current_world_.world_id);
    native_context_change_pending_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> event_lock(native_event_mutex_);
        pending_command_error_.clear();
        received_level_chunks_.clear();
        received_rpc_acks_.clear();
    }
    resetVerificationRuntime();
    resetCommandBlockRuntime();
    resetSignRuntime();
    resetContainerRuntime();
    resetEntityRuntime();
    invalidateRpcTransport();
    last_rpc_send_debug_.clear();
    last_rpc_poll_debug_.clear();
    resetActiveUnitRuntime();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = request.source_type == ImportSourceType::PixelArtPng
            ? "converting pixel art"
            : request.source_type == ImportSourceType::CommandMusicMidi
                ? "parsing MIDI score"
            : request.source_type == ImportSourceType::Litematic
                ? "parsing litematic"
            : request.source_type == ImportSourceType::Bdx
                ? "parsing bdx"
            : request.source_type == ImportSourceType::Mcworld
                ? "reading mcworld"
                : request.source_type == ImportSourceType::InfiniteczBuild
                    ? "reading .infinity"
                    : "parsing schematic";
        degradation_notice_.clear();
        total_block_count_ = 0;
        imported_block_count_ = 0;
        blocks_per_second_ = request.config.blocks_per_second;
        suppress_command_feedback_.store(
            request.config.suppress_command_feedback, std::memory_order_release);
    }
    throughput_governor_.resetFeedback();
    resetPlacementBatch();
    const uint64_t generation = run_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    worker_running_.store(true, std::memory_order_release);
    try {
        worker_ = std::thread(&BuildImportRuntime::parseWorker, this,
                              std::move(request), generation);
    } catch (const std::exception& exception) {
        worker_running_.store(false, std::memory_order_release);
        resetCurrentRun(true);
        if (error) *error = std::string("cannot start parser worker: ") + exception.what();
        return false;
    } catch (...) {
        worker_running_.store(false, std::memory_order_release);
        resetCurrentRun(true);
        if (error) *error = "cannot start parser worker";
        return false;
    }
    return true;
} catch (const std::bad_alloc&) {
    try {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
        releaseUndoClaimNoThrow();
        resetCurrentRun(true);
    } catch (...) {}
    try { if (error) *error = "not enough memory to start building import"; } catch (...) {}
    return false;
} catch (const std::exception& exception) {
    try {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
        releaseUndoClaimNoThrow();
        resetCurrentRun(true);
    } catch (...) {}
    try { if (error) *error = std::string("cannot start building import: ") + exception.what(); }
    catch (...) {}
    return false;
} catch (...) {
    try {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
        releaseUndoClaimNoThrow();
        resetCurrentRun(true);
    } catch (...) {}
    try { if (error) *error = "cannot start building import"; } catch (...) {}
    return false;
}

void BuildImportRuntime::releaseUndoClaimNoThrow() noexcept {
    try {
        if (!undo_run_active_ || undo_storage_directory_.empty() ||
            undo_record_id_.empty() || undo_claim_job_id_.empty()) {
            return;
        }
        std::string transition_error;
        BuildImportUndoStore::transitionState(
            undo_storage_directory_, undo_record_id_,
            BuildImportUndoRecordState::Claimed, undo_claim_job_id_,
            BuildImportUndoRecordState::Available, {}, &transition_error);
    } catch (...) {
        // A failed release leaves the record Claimed, which is deliberately
        // fail-closed: a later caller cannot start a second destructive undo.
    }
}

bool BuildImportRuntime::retireUndoClaim(std::string* warning) {
    if (warning) warning->clear();
    if (!undo_run_active_ || undo_storage_directory_.empty() ||
        undo_record_id_.empty() || undo_claim_job_id_.empty()) {
        return true;
    }
    std::string transition_error;
    const BuildImportUndoTransitionResult transition =
        BuildImportUndoStore::transitionState(
            undo_storage_directory_, undo_record_id_,
            BuildImportUndoRecordState::Claimed, undo_claim_job_id_,
            BuildImportUndoRecordState::Consumed, undo_claim_job_id_,
            &transition_error);
    if (transition == BuildImportUndoTransitionResult::Updated) {
        std::string remove_error;
        if (!BuildImportUndoStore::remove(undo_storage_directory_, &remove_error) && warning) {
            *warning = "undo completed, but its consumed record could not be removed";
            if (!remove_error.empty()) *warning += ": " + remove_error;
        }
        return true;
    }
    if (transition == BuildImportUndoTransitionResult::MissingOrInvalid) {
        if (warning) {
            *warning = "undo completed, but its record was already missing or corrupt";
            if (!transition_error.empty()) *warning += ": " + transition_error;
        }
        return true;
    }
    if (transition == BuildImportUndoTransitionResult::WriteFailed) {
        // The record is still Claimed, never Available.  Removing it is a
        // best-effort cleanup; if removal also fails, future undo attempts
        // remain rejected by the lifecycle state.
        std::string remove_error;
        if (BuildImportUndoStore::remove(undo_storage_directory_, &remove_error)) {
            return true;
        }
    }
    if (warning) {
        *warning = transition == BuildImportUndoTransitionResult::Mismatch
            ? "undo completed, but its record changed; the newer record was retained"
            : "undo completed, but its record could not be retired";
        if (!transition_error.empty()) *warning += ": " + transition_error;
    }
    return false;
}

bool BuildImportRuntime::invalidateAvailableUndoRecord(std::string* warning) {
    if (warning) warning->clear();
    if (undo_storage_directory_.empty()) return true;
    std::string load_error;
    const auto manifest = BuildImportUndoStore::load(undo_storage_directory_, &load_error);
    if (!manifest) return true;  // There is no usable stale record to consume.
    const auto state = manifest->state;
    if (state == BuildImportUndoRecordState::Consumed) return true;
    std::string transition_error;
    const auto transition = BuildImportUndoStore::transitionState(
        undo_storage_directory_, manifest->record_id, state, manifest->claim_job_id,
        BuildImportUndoRecordState::Consumed, manifest->claim_job_id, &transition_error);
    if (transition == BuildImportUndoTransitionResult::Updated) {
        std::string remove_error;
        if (!BuildImportUndoStore::remove(undo_storage_directory_, &remove_error) && warning) {
            *warning = "previous undo record was retired but could not be removed";
            if (!remove_error.empty()) *warning += ": " + remove_error;
        }
        return true;
    }
    std::string remove_error;
    if (BuildImportUndoStore::remove(undo_storage_directory_, &remove_error)) return true;
    if (warning) {
        *warning = "previous undo record could not be invalidated";
        if (!transition_error.empty()) *warning += ": " + transition_error;
        if (!remove_error.empty()) *warning += "; " + remove_error;
    }
    return false;
}

bool BuildImportRuntime::undoLastImport(const std::string& storage_directory,
                                        const std::string& spool_directory,
                                        const WorldContext& context,
                                        std::string* error) try {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (restore_in_progress_.load(std::memory_order_acquire)) {
        if (error) *error = "cannot undo while checkpoint restore is in progress";
        return false;
    }
    if (isRestoreBusyState(controller_.state())) {
        if (error) *error = "an import is already active or can still be resumed";
        return false;
    }
    if (worker_.joinable()) {
        if (worker_running_.load(std::memory_order_acquire)) {
            if (error) *error = "the previous planner is still stopping";
            return false;
        }
        worker_.join();
    }
    if (!directoryExists(storage_directory) || !directoryExists(spool_directory) ||
        parentDirectory(spool_directory) != storage_directory + "/jobs") {
        if (error) *error = "undo work directory is invalid";
        return false;
    }
    // Replace any terminal/failed in-memory run first. If it was an earlier
    // undo attempt, resetCurrentRun releases that exact claim before we load
    // the currently available record below.
    cancel();
    std::string manifest_error;
    std::optional<BuildImportUndoManifest> manifest =
        BuildImportUndoStore::load(storage_directory, &manifest_error);
    if (!manifest) {
        if (error) *error = manifest_error.empty()
            ? "there is no completed import to undo" : manifest_error;
        return false;
    }
    if (manifest->state == BuildImportUndoRecordState::Claimed) {
        if (error) *error = "the last import undo record belongs to an unfinished undo task";
        return false;
    } else if (manifest->state == BuildImportUndoRecordState::Consumed) {
        BuildImportUndoStore::remove(storage_directory, nullptr);
        if (error) *error = "the last import undo record has already been consumed";
        return false;
    }
    if (!(manifest->world == context)) {
        if (error) *error = "the last import belongs to a different world or dimension";
        return false;
    }
    uint64_t undo_block_count = 0;
    for (const BuildImportUndoRegion& region : manifest->regions) {
        if (!addBoundsVolume(region.bounds, &undo_block_count)) {
            if (error) *error = "undo region volume is invalid or too large";
            return false;
        }
    }
    if (undo_block_count == 0U) {
        if (error) *error = "the last import has no recorded blocks to clear";
        return false;
    }

    ImportIdentity identity;
    identity.job_id = fileName(spool_directory);
    if (identity.job_id.empty()) {
        if (error) *error = "undo work directory has no job identity";
        return false;
    }
    identity.source_file = BuildImportUndoStore::filePath(storage_directory);
    identity.source_hash = manifest->record_id;
    // Undo plans deliberately keep exact per-chunk bounds. Region aggregation
    // would bridge sparse chunks and could clear blocks the import never owned.
    identity.options_hash = kLegacyClearPlanVersion;
    identity.mapper_version = BlockMapper::kVersion;

    ImportConfig config;
    config.chunk_size = manifest->chunk_size;
    config.simulation_chunk_range = manifest->simulation_chunk_range;
    config.blocks_per_second = manifest->blocks_per_second;
    config.source_type = ImportSourceType::Schematic;
    config.overwrite_policy = OverwritePolicy::ClearImportedBounds;
    config.place_deny_layer = false;
    config.verify_after_import = false;
    config.checkpoint_path = spool_directory + "/checkpoint.bin";

    spool_directory_ = spool_directory;
    undo_storage_directory_ = storage_directory;
    undo_record_id_ = manifest->record_id;
    undo_claim_job_id_ = identity.job_id;
    durable_undo_block_count_ = 0;
    undo_run_active_ = true;
    current_world_ = context;
    std::string claim_error;
    const BuildImportUndoTransitionResult claim = BuildImportUndoStore::transitionState(
        storage_directory, undo_record_id_, BuildImportUndoRecordState::Available, {},
        BuildImportUndoRecordState::Claimed, undo_claim_job_id_, &claim_error);
    if (claim != BuildImportUndoTransitionResult::Updated) {
        spool_directory_.clear();
        undo_storage_directory_.clear();
        undo_record_id_.clear();
        undo_claim_job_id_.clear();
        durable_undo_block_count_ = 0;
        undo_run_active_ = false;
        current_world_ = {};
        if (error) *error = claim_error.empty()
            ? "the undo record could not be claimed" : claim_error;
        return false;
    }
    manifest->state = BuildImportUndoRecordState::Claimed;
    manifest->claim_job_id = undo_claim_job_id_;
    if (!controller_.startPlanning(identity, context, config, error)) {
        resetCurrentRun(true);
        return false;
    }
    region_command_spools_ = false;
    adaptive_region_grid_ = false;
    WorkUnit stale_area;
    const std::string stale_cleanup = controller_.makeCleanupCommand(stale_area);
    if (std::find(pending_cleanup_commands_.begin(), pending_cleanup_commands_.end(),
                  stale_cleanup) == pending_cleanup_commands_.end()) {
        pending_cleanup_commands_.push_back(stale_cleanup);
    }
    cleanup_retry_at_ = {};
    cleanup_barrier_failure_count_ = 0;
    recovery_pending_ = false;
    phase_settle_ready_at_ = {};
    loaded_region_.reset();
    loaded_region_cleanup_command_.clear();
    loaded_region_probe_bounds_ = {};
    loaded_region_fully_confirmed_ = false;
    loaded_region_started_at_ = {};
    confirmed_region_chunks_.clear();
    native_dimension_token_ = GetCachedDimensionTokenForWorld(current_world_.world_id);
    native_context_change_pending_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> event_lock(native_event_mutex_);
        pending_command_error_.clear();
        received_level_chunks_.clear();
        received_rpc_acks_.clear();
    }
    resetVerificationRuntime();
    resetCommandBlockRuntime();
    resetSignRuntime();
    resetContainerRuntime();
    resetEntityRuntime();
    invalidateRpcTransport();
    resetActiveUnitRuntime();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "preparing undo plan";
        degradation_notice_.clear();
        total_block_count_ = undo_block_count;
        imported_block_count_ = 0;
        blocks_per_second_ = config.blocks_per_second;
    }
    throughput_governor_.resetFeedback();
    resetPlacementBatch();

    const uint64_t generation = run_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    worker_running_.store(true, std::memory_order_release);
    try {
        worker_ = std::thread(&BuildImportRuntime::undoPlanningWorker, this,
                              std::move(*manifest), generation);
    } catch (const std::exception& exception) {
        worker_running_.store(false, std::memory_order_release);
        resetCurrentRun(true);
        if (error) *error = std::string("cannot start undo planner: ") + exception.what();
        return false;
    } catch (...) {
        worker_running_.store(false, std::memory_order_release);
        resetCurrentRun(true);
        if (error) *error = "cannot start undo planner";
        return false;
    }
    return true;
} catch (const std::exception& exception) {
    try {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
        resetCurrentRun(true);
    } catch (...) {}
    try { if (error) *error = std::string("cannot start import undo: ") + exception.what(); }
    catch (...) {}
    return false;
} catch (...) {
    try {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
        resetCurrentRun(true);
    } catch (...) {}
    try { if (error) *error = "cannot start import undo"; } catch (...) {}
    return false;
}

bool BuildImportRuntime::discardUndoClaim(const std::string& storage_directory,
                                          const std::string& spool_directory,
                                          std::string* error) try {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (restore_in_progress_.load(std::memory_order_acquire) ||
        isRestoreBusyState(controller_.state())) {
        if (error) *error = "cannot discard an undo checkpoint while an import is active";
        return false;
    }
    if (!directoryExists(storage_directory) ||
        parentDirectory(spool_directory) != storage_directory + "/jobs") {
        if (error) *error = "undo checkpoint directory is invalid";
        return false;
    }
    std::string load_error;
    const auto manifest = BuildImportUndoStore::load(storage_directory, &load_error);
    if (!manifest || manifest->state != BuildImportUndoRecordState::Claimed ||
        manifest->claim_job_id != fileName(spool_directory)) {
        // Missing/corrupt records and unrelated claims are already unusable by
        // this discarded checkpoint. Never mutate an unrelated newer record.
        return true;
    }
    const auto result = BuildImportUndoStore::transitionState(
        storage_directory, manifest->record_id,
        BuildImportUndoRecordState::Claimed, manifest->claim_job_id,
        BuildImportUndoRecordState::Available, {}, error);
    return result == BuildImportUndoTransitionResult::Updated ||
           result == BuildImportUndoTransitionResult::MissingOrInvalid;
} catch (const std::exception& exception) {
    try { if (error) *error = std::string("cannot discard undo checkpoint claim: ") +
                              exception.what(); } catch (...) {}
    return false;
} catch (...) {
    try { if (error) *error = "cannot discard undo checkpoint claim"; } catch (...) {}
    return false;
}

void BuildImportRuntime::undoPlanningWorker(BuildImportUndoManifest manifest,
                                            uint64_t generation) {
    struct WorkerRunningScope {
        explicit WorkerRunningScope(std::atomic<bool>* running) : running_(running) {}
        ~WorkerRunningScope() { running_->store(false, std::memory_order_release); }
        std::atomic<bool>* running_;
    } worker_scope(&worker_running_);
    const std::string planning_directory = spool_directory_;
    const auto abandoned = [&]() {
        return generation != run_generation_.load(std::memory_order_acquire) ||
               controller_.state() != ImportState::Planning;
    };
    try {
        std::vector<ChunkDescriptor> descriptors;
        descriptors.reserve(manifest.regions.size());
        for (const BuildImportUndoRegion& region : manifest.regions) {
            if (abandoned()) {
                cleanupSpoolDirectory(planning_directory);
                return;
            }
            ChunkDescriptor descriptor;
            descriptor.coord = region.coord;
            descriptor.imported_bounds = region.bounds;
            descriptors.push_back(std::move(descriptor));
        }
        std::string planning_error;
        if (!CommandSpoolBuilder::build(
                planning_directory, manifest.blocks_per_second,
                OverwritePolicy::ClearImportedBounds, 1, &descriptors,
                &planning_error, abandoned, false, {}, false, true, 0U)) {
            cleanupSpoolDirectory(planning_directory);
            if (abandoned()) return;
            controller_.failActiveUnit(planning_error.empty()
                ? "cannot prepare undo partitions" : planning_error);
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = planning_error.empty()
                ? "cannot prepare undo partitions" : planning_error;
            return;
        }
        for (ChunkDescriptor& descriptor : descriptors) {
            if (abandoned()) {
                cleanupSpoolDirectory(planning_directory);
                return;
            }
            if (!controller_.addChunk(std::move(descriptor), &planning_error)) {
                cleanupSpoolDirectory(planning_directory);
                if (abandoned()) return;
                controller_.failActiveUnit(planning_error);
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = planning_error;
                return;
            }
        }
        if (!controller_.finishPlanning(&planning_error)) {
            cleanupSpoolDirectory(planning_directory);
            if (abandoned()) return;
            if (controller_.state() == ImportState::Planning) {
                controller_.failActiveUnit(planning_error);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = planning_error.empty() ? "cannot finalize undo plan" : planning_error;
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_ == "preparing undo plan") status_ = "undo plan ready";
    } catch (const std::bad_alloc&) {
        cleanupSpoolDirectory(planning_directory);
        if (!abandoned()) {
            controller_.failActiveUnit("import undo needs more memory than available");
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "import undo needs more memory than available";
        }
    } catch (...) {
        cleanupSpoolDirectory(planning_directory);
        if (!abandoned()) {
            controller_.failActiveUnit("unexpected undo planner failure");
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "unexpected undo planner failure";
        }
    }
}

bool BuildImportRuntime::persistUndoSnapshot(std::string* error) {
    if (error) error->clear();
    if (spool_directory_.empty() || undo_storage_directory_.empty()) {
        if (error) *error = "undo storage is unavailable";
        return false;
    }
    std::vector<VerificationChunkPlan> plans;
    if (!CommandSpoolBuilder::loadVerificationPlan(
            spool_directory_ + "/" + CommandSpoolBuilder::kVerificationPlanName,
            &plans, error)) {
        return false;
    }
    const CheckpointSnapshot checkpoint = controller_.checkpointSnapshot();
    BuildImportUndoManifest manifest;
    manifest.world = checkpoint.world;
    // Old restorable checkpoints used wider logical chunks. Normalize every
    // completed import to the current 16x16 grid so its undo remains compatible
    // with the current scheduler rather than publishing an unusable record.
    manifest.chunk_size = ImportConfig::kVanillaChunkSize;
    manifest.simulation_chunk_range =
        isValidSimulationChunkRange(checkpoint.config.simulation_chunk_range)
        ? checkpoint.config.simulation_chunk_range
        : ImportConfig::kDefaultSimulationChunkRange;
    manifest.blocks_per_second = checkpoint.config.blocks_per_second;
    manifest.record_id = makeUndoRecordId(checkpoint.identity.job_id);
    manifest.state = BuildImportUndoRecordState::Available;
    std::vector<BlockBounds> imported_bounds;
    imported_bounds.reserve(plans.size());
    for (const VerificationChunkPlan& plan : plans) {
        if (plan.imported_bounds.isValid()) imported_bounds.push_back(plan.imported_bounds);
    }
    const std::optional<BlockBounds> foundation = checkpoint.config.place_deny_layer
        ? std::optional<BlockBounds>(checkpoint.config.deny_layer_bounds) : std::nullopt;
    if (!BuildImportUndoStore::buildNormalizedRegions(
            imported_bounds, foundation, &manifest.regions, error)) return false;
    return BuildImportUndoStore::saveAtomically(
        undo_storage_directory_, manifest, error);
}

bool BuildImportRuntime::restore(const std::string& spool_directory,
                                  const WorldContext& context,
                                  std::string* error) try {
    bool expected = false;
    if (!restore_in_progress_.compare_exchange_strong(expected, true,
                                                      std::memory_order_acq_rel)) {
        if (error) *error = "another checkpoint restore is already in progress";
        return false;
    }
    struct RestoreFlagScope {
        explicit RestoreFlagScope(std::atomic<bool>* flag) : flag_(flag) {}
        ~RestoreFlagScope() { flag_->store(false, std::memory_order_release); }
        std::atomic<bool>* flag_;
    } restore_scope(&restore_in_progress_);

    uint64_t baseline_generation = 0;
    ImportState baseline_state = ImportState::Idle;
    {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
        baseline_state = controller_.state();
        if (isRestoreBusyState(baseline_state)) {
            if (error) *error = "cannot restore while another import is active or resumable";
            return false;
        }
        if (worker_.joinable()) {
            if (worker_running_.load(std::memory_order_acquire)) {
                if (error) *error = "the previous parser is still stopping";
                return false;
            }
            worker_.join();
        }
        baseline_generation = run_generation_.load(std::memory_order_acquire);
    }

    auto snapshot = BuildImportCheckpoint::load(spool_directory + "/checkpoint.bin", error);
    if (!snapshot) return false;
    if (!validateRestorablePlan(*snapshot, error)) return false;
    if (!(snapshot->world == context)) {
        if (error) *error = "current world or dimension does not match checkpoint";
        return false;
    }
    std::vector<ChunkDescriptor> descriptors;
    uint64_t total_blocks = 0;
    const int32_t restored_region_span = regionSpanForImportConfig(snapshot->config);
    const ChunkCoord restored_region_origin =
        usesAdaptiveRegionGrid(snapshot->identity.options_hash)
        ? snapshot->config.region_grid_origin : ChunkCoord{};
    if (!catalogExistingSpools(
            spool_directory, &descriptors, &total_blocks,
            snapshot->config.chunk_size, restored_region_span,
            usesRegionCommandSpools(snapshot->identity.options_hash),
            restored_region_origin, error)) {
        return false;
    }
    const std::string restored_map_state_path =
        spool_directory + "/" + kMapCreationStateFileName;
    // Before v13 the map sidecar was the only durable indication that maps
    // were requested. New checkpoints retain that choice in the main file,
    // so losing the sidecar must be a restoration error, not a skipped task.
    const bool restored_map_requested = snapshot->format_version >= 13
        ? snapshot->config.create_maps_after_import
        : snapshot->config.source_type == ImportSourceType::PixelArtPng &&
          commandBlockStateExists(restored_map_state_path);
    if (restored_map_requested && !commandBlockStateExists(restored_map_state_path)) {
        if (error) *error = "automatic map creation state is missing; cannot safely resume";
        return false;
    }
    if (restored_map_requested && snapshot->format_version < 13) {
        // Once this legacy checkpoint is resumed, its next save uses v13.
        // Carry the sidecar-derived intent forward into the new main file.
        snapshot->config.create_maps_after_import = true;
    }
    BlockBounds restored_map_bounds;
    uint64_t restored_map_columns = 0;
    uint64_t restored_map_rows = 0;
    uint64_t restored_map_count = 0;
    uint64_t restored_map_cursor = 0;
    if (restored_map_requested) {
        for (const ChunkDescriptor& descriptor : descriptors) {
            extendBounds(&restored_map_bounds, descriptor.imported_bounds);
        }
        if (!automaticMapPlan(restored_map_bounds, &restored_map_columns,
                              &restored_map_rows, &restored_map_count, error) ||
            !loadDeferredDataState(restored_map_state_path,
                                   restored_map_count, &restored_map_cursor, error)) {
            return false;
        }
        if (mapStoragePipelineEnabled()) {
            MapStorageRuntimeSnapshot storage;
            if (!loadMapStorageRuntimeSnapshot(
                    restored_map_state_path, snapshot->world,
                    restored_map_bounds, restored_map_columns,
                    restored_map_rows, restored_map_count,
                    restored_map_cursor, -1, &storage, error)) return false;
        } else if (!requireNoPendingMapStorage(
                restored_map_state_path, snapshot->world,
                restored_map_bounds, restored_map_count,
                restored_map_cursor, error)) {
            return false;
        }
    }
    const bool undo_checkpoint =
        fileName(snapshot->identity.source_file) == BuildImportUndoStore::kFileName &&
        snapshot->identity.options_hash == kLegacyClearPlanVersion &&
        snapshot->config.overwrite_policy == OverwritePolicy::ClearImportedBounds &&
        !snapshot->config.verify_after_import;
    uint64_t undo_total_blocks = 0;
    uint64_t undo_completed_blocks = 0;
    std::optional<BuildImportUndoManifest> undo_manifest;
    const std::string restored_undo_storage = undo_checkpoint
        ? undoStorageDirectoryForSpool(spool_directory) : std::string();
    if (undo_checkpoint) {
        std::string undo_error;
        undo_manifest = BuildImportUndoStore::load(restored_undo_storage, &undo_error);
        if (!undo_manifest) {
            if (error) *error = undo_error.empty()
                ? "undo checkpoint has no matching claimed record" : undo_error;
            return false;
        }
        if (undo_manifest->state != BuildImportUndoRecordState::Claimed ||
            undo_manifest->record_id != snapshot->identity.source_hash ||
            undo_manifest->claim_job_id != snapshot->identity.job_id ||
            !(undo_manifest->world == snapshot->world) ||
            undo_manifest->chunk_size != snapshot->config.chunk_size ||
            undo_manifest->simulation_chunk_range != snapshot->config.simulation_chunk_range ||
            undo_manifest->blocks_per_second != snapshot->config.blocks_per_second ||
            !sameUndoRegions(undo_manifest->regions, descriptors)) {
            if (error) {
                *error = "undo checkpoint no longer matches its immutable claimed record";
            }
            return false;
        }
        for (const ChunkDescriptor& descriptor : descriptors) {
            if (!addBoundsVolume(descriptor.imported_bounds, &undo_total_blocks)) {
                if (error) *error = "undo checkpoint volume is invalid or too large";
                return false;
            }
        }
        if (!completedUndoVolumeForCheckpoint(
                *snapshot, descriptors, &undo_completed_blocks) ||
            undo_completed_blocks > undo_total_blocks) {
            if (error) *error = "undo checkpoint progress is invalid";
            return false;
        }
    }
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (run_generation_.load(std::memory_order_acquire) != baseline_generation ||
        controller_.state() != baseline_state || isRestoreBusyState(controller_.state())) {
        if (error) *error = "runtime state changed while checkpoint restore was being validated";
        return false;
    }
    const std::string previous_spool_directory = spool_directory_;
    const DirectoryRelation previous_directory_relation =
        compareExistingDirectories(previous_spool_directory, spool_directory);
    if (!controller_.restoreFromCheckpoint(*snapshot, std::move(descriptors), error)) {
        return false;
    }
    const CheckpointSnapshot restored_snapshot = controller_.checkpointSnapshot();
    region_command_spools_ = usesRegionCommandSpools(restored_snapshot.identity.options_hash);
    adaptive_region_grid_ = usesAdaptiveRegionGrid(restored_snapshot.identity.options_hash);
    suppress_command_feedback_.store(
        restored_snapshot.config.suppress_command_feedback, std::memory_order_release);
    command_feedback_suppression_unavailable_ = false;
    command_feedback_failure_count_ = 0;
    command_feedback_retry_at_ = {};

    // Controller restoration is transactional. Only after it commits do we
    // invalidate and replace the terminal runtime state that preceded it.
    run_generation_.fetch_add(1, std::memory_order_acq_rel);
    releaseLoadedRegion(false);
    pending_cleanup_commands_.clear();
    held_region_cleanups_.clear();
    held_region_cleanup_retry_at_ = {};
    hold_active_region_release_until_ = {};
    prefetched_prepare_region_.reset();
    prefetched_prepare_sent_at_ = {};
    cleanup_barrier_uuids_.clear();
    cleanup_barrier_command_count_ = 0;
    cleanup_barrier_poll_at_ = {};
    cleanup_barrier_deadline_ = {};
    cleanup_barrier_failure_count_ = 0;
    cleanup_retry_at_ = {};
    region_add_ready_at_ = {};
    spool_directory_ = spool_directory;
    undo_storage_directory_ = undo_checkpoint
        ? restored_undo_storage : undoStorageDirectoryForSpool(spool_directory);
    undo_record_id_ = undo_manifest ? undo_manifest->record_id : std::string();
    undo_claim_job_id_ = undo_manifest ? undo_manifest->claim_job_id : std::string();
    durable_undo_block_count_ = undo_checkpoint ? undo_completed_blocks : 0;
    undo_run_active_ = undo_checkpoint;
    current_world_ = context;
    native_dimension_token_ = GetCachedDimensionTokenForWorld(current_world_.world_id);
    native_context_change_pending_.store(false, std::memory_order_release);
    invalidateRpcTransport();
    recovery_pending_ = false;
    phase_settle_ready_at_ = {};
    unit_retry_counts_.clear();
    loaded_region_.reset();
    loaded_region_cleanup_command_.clear();
    loaded_region_started_at_ = {};
    confirmed_region_chunks_.clear();
    resetVerificationRuntime();
    resetCommandBlockRuntime();
    resetSignRuntime();
    resetContainerRuntime();
    resetEntityRuntime();
    resetMapCreationRuntime();
    if (restored_map_requested) {
        map_creation_requested_ = true;
        map_source_bounds_ = restored_map_bounds;
        map_tile_columns_ = restored_map_columns;
        map_tile_rows_ = restored_map_rows;
        map_tile_count_ = restored_map_count;
        map_tile_cursor_ = restored_map_cursor;
        map_tile_persisted_cursor_ = restored_map_cursor;
        map_surface_y_ = restored_map_bounds.max_y;
        map_state_path_ = spool_directory + "/" + kMapCreationStateFileName;
    }
    resetActiveUnitRuntime();
    {
        std::lock_guard<std::mutex> event_lock(native_event_mutex_);
        pending_command_error_.clear();
        received_level_chunks_.clear();
        received_rpc_acks_.clear();
    }
    WorkUnit stale_area;
    pending_cleanup_commands_.push_back(controller_.makeCleanupCommand(stale_area));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "checkpoint restored; ready to resume";
        degradation_notice_ = restored_snapshot.degradation_notice;
        total_block_count_ = undo_run_active_ ? undo_total_blocks : total_blocks;
        imported_block_count_ = undo_run_active_
            ? durable_undo_block_count_
            : std::min(total_block_count_, restored_snapshot.completed_block_count);
        blocks_per_second_ = restored_snapshot.config.blocks_per_second;
    }
    throughput_governor_.resetFeedback();
    resetPlacementBatch();
    if (previous_directory_relation == DirectoryRelation::Different) {
        cleanupSpoolDirectory(previous_spool_directory);
    }
    return true;
} catch (const std::bad_alloc&) {
    restore_in_progress_.store(false, std::memory_order_release);
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    run_generation_.fetch_add(1, std::memory_order_acq_rel);
    controller_.cancel(false);
    releaseLoadedRegion(false);
    resetVerificationRuntime();
    resetCommandBlockRuntime();
    resetSignRuntime();
    resetContainerRuntime();
    resetEntityRuntime();
    resetMapCreationRuntime();
    resetActiveUnitRuntime();
    invalidateRpcTransport();
    spool_directory_.clear();
    undo_storage_directory_.clear();
    undo_record_id_.clear();
    undo_claim_job_id_.clear();
    durable_undo_block_count_ = 0;
    undo_run_active_ = false;
    current_world_ = {};
    try { if (error) *error = "not enough memory to validate building checkpoint"; } catch (...) {}
    return false;
} catch (const std::exception& exception) {
    restore_in_progress_.store(false, std::memory_order_release);
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    run_generation_.fetch_add(1, std::memory_order_acq_rel);
    controller_.cancel(false);
    releaseLoadedRegion(false);
    resetVerificationRuntime();
    resetCommandBlockRuntime();
    resetSignRuntime();
    resetContainerRuntime();
    resetEntityRuntime();
    resetMapCreationRuntime();
    resetActiveUnitRuntime();
    invalidateRpcTransport();
    spool_directory_.clear();
    undo_storage_directory_.clear();
    undo_record_id_.clear();
    undo_claim_job_id_.clear();
    durable_undo_block_count_ = 0;
    undo_run_active_ = false;
    current_world_ = {};
    try { if (error) *error = std::string("cannot validate building checkpoint: ") +
                              exception.what(); } catch (...) {}
    return false;
} catch (...) {
    restore_in_progress_.store(false, std::memory_order_release);
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    run_generation_.fetch_add(1, std::memory_order_acq_rel);
    controller_.cancel(false);
    releaseLoadedRegion(false);
    resetVerificationRuntime();
    resetCommandBlockRuntime();
    resetSignRuntime();
    resetContainerRuntime();
    resetEntityRuntime();
    resetMapCreationRuntime();
    resetActiveUnitRuntime();
    invalidateRpcTransport();
    spool_directory_.clear();
    undo_storage_directory_.clear();
    undo_record_id_.clear();
    undo_claim_job_id_.clear();
    durable_undo_block_count_ = 0;
    undo_run_active_ = false;
    current_world_ = {};
    try { if (error) *error = "cannot validate building checkpoint"; } catch (...) {}
    return false;
}

void BuildImportRuntime::parseWorker(BuildImportStartRequest request, uint64_t generation) {
    struct WorkerRunningScope {
        explicit WorkerRunningScope(std::atomic<bool>* running) : running_(running) {}
        ~WorkerRunningScope() { running_->store(false, std::memory_order_release); }
        std::atomic<bool>* running_;
    } worker_scope(&worker_running_);
    const std::string parse_spool_directory =
        request.source_type == ImportSourceType::PixelArtPng
            ? request.pixel_art.spool_directory : request.parse.spool_directory;
    try {
    SchematicParseResult result;
    std::string error;
    const auto abandoned = [&]() {
        return generation != run_generation_.load(std::memory_order_acquire) ||
               controller_.state() != ImportState::Planning;
    };
    // Command-block metadata is deliberately kept out of the raw block spool:
    // raw records are later merged into /fill commands and can be replayed by
    // repair.  This independent sidecar is consumed only after ordinary
    // placement and final verification have both completed.
    std::unique_ptr<CommandBlockSpoolWriter> command_block_writer;
    CommandBlockSpoolManifest command_block_manifest;
    uint64_t converted_legacy_execute_count = 0;
    uint64_t retained_command_block_payload_count = 0;
    std::unique_ptr<SignSpoolWriter> sign_writer;
    std::string sign_spool_path;
    std::unique_ptr<EntitySpoolWriter> entity_writer;
    std::string entity_spool_path;
    std::unique_ptr<ContainerItemSpoolWriter> container_item_writer;
    std::string container_item_spool_path;
    // This worker belongs to a freshly started import, never a restore. Clear
    // every deferred-command artifact even for pixel art: a pixel job has no
    // command-block sink, so leaving an abandoned structure's sidecar here
    // could otherwise cause its commands to be applied after the new job.
    const std::string command_block_spool_path = parse_spool_directory + "/" +
        CommandBlockSpoolWriter::kFileName;
    std::remove(command_block_spool_path.c_str());
    std::remove((command_block_spool_path + ".tmp").c_str());
    std::remove((command_block_spool_path + ".manifest").c_str());
    std::remove((command_block_spool_path + ".manifest.tmp").c_str());
    std::remove((parse_spool_directory + "/" + kCommandBlockStateFileName).c_str());
    std::remove((parse_spool_directory + "/" + kCommandBlockStateFileName + ".tmp").c_str());
    const std::string sign_spool_path_to_clear = parse_spool_directory + "/" +
        SignSpoolWriter::kFileName;
    std::remove(sign_spool_path_to_clear.c_str());
    std::remove((sign_spool_path_to_clear + ".tmp").c_str());
    std::remove((parse_spool_directory + "/" + kSignStateFileName).c_str());
    std::remove((parse_spool_directory + "/" + kSignStateFileName + ".tmp").c_str());
    const std::string entity_spool_path_to_clear = parse_spool_directory + "/" +
        EntitySpoolWriter::kFileName;
    std::remove(entity_spool_path_to_clear.c_str());
    std::remove((entity_spool_path_to_clear + ".tmp").c_str());
    std::remove((parse_spool_directory + "/" + kEntityStateFileName).c_str());
    std::remove((parse_spool_directory + "/" + kEntityStateFileName + ".tmp").c_str());
    const std::string container_item_spool_path_to_clear = parse_spool_directory + "/" +
        ContainerItemSpoolWriter::kFileName;
    std::remove(container_item_spool_path_to_clear.c_str());
    std::remove((container_item_spool_path_to_clear + ".tmp").c_str());
    std::remove((parse_spool_directory + "/" + kContainerItemStateFileName).c_str());
    std::remove((parse_spool_directory + "/" + kContainerItemStateFileName + ".tmp").c_str());
    if (request.source_type != ImportSourceType::PixelArtPng) {
        command_block_writer = std::make_unique<CommandBlockSpoolWriter>(
            parse_spool_directory);
        sign_writer = std::make_unique<SignSpoolWriter>(parse_spool_directory);
        entity_writer = std::make_unique<EntitySpoolWriter>(parse_spool_directory);
        container_item_writer = std::make_unique<ContainerItemSpoolWriter>(parse_spool_directory);
        request.parse.command_block_sink =
            [&command_block_writer, &abandoned, &converted_legacy_execute_count,
             &retained_command_block_payload_count](const CommandBlockRecord& record,
                                                     std::string* sink_error) -> bool {
                if (abandoned()) {
                    if (sink_error) *sink_error = "import cancelled";
                    return false;
                }
                CommandBlockRecord normalized = record;
                const bool converted_execute = normalizeLegacyExecuteCommand(&normalized.command);
                if (!command_block_writer ||
                    !command_block_writer->append(normalized, sink_error)) {
                    if (sink_error && sink_error->empty()) {
                        *sink_error = "cannot append command-block spool record";
                    }
                    return false;
                }
                ++retained_command_block_payload_count;
                if (converted_execute) ++converted_legacy_execute_count;
                return true;
            };
        request.parse.entity_sink =
            [&entity_writer, &abandoned](const EntityRecord& record,
                                         std::string* sink_error) -> bool {
                if (abandoned()) {
                    if (sink_error) *sink_error = "import cancelled";
                    return false;
                }
                if (!entity_writer || !entity_writer->append(record, sink_error)) {
                    if (sink_error && sink_error->empty()) {
                        *sink_error = "cannot append entity spool record";
                    }
                    return false;
                }
                return true;
            };
        request.parse.sign_sink =
            [&sign_writer, &abandoned](const SignRecord& record,
                                        std::string* sink_error) -> bool {
                if (abandoned()) {
                    if (sink_error) *sink_error = "import cancelled";
                    return false;
                }
                if (!sign_writer || !sign_writer->append(record, sink_error)) {
                    if (sink_error && sink_error->empty()) {
                        *sink_error = "cannot append sign spool record";
                    }
                    return false;
                }
                return true;
            };
        request.parse.container_item_sink =
            [&container_item_writer, &abandoned](const ContainerItemRecord& record,
                                                  std::string* sink_error) -> bool {
                if (abandoned()) {
                    if (sink_error) *sink_error = "import cancelled";
                    return false;
                }
                // The importer intentionally supports only slot, item id,
                // count and aux/data. Drop any legacy parser extras before
                // they reach the durable sidecar.
                ContainerItemRecord normalized = record;
                normalized.enchantments.clear();
                if (!container_item_writer ||
                    !container_item_writer->append(normalized, sink_error)) {
                    if (sink_error && sink_error->empty()) {
                        *sink_error = "cannot append container-item spool record";
                    }
                    return false;
                }
                return true;
            };
    }
    std::chrono::steady_clock::time_point last_progress_at{};
    int last_progress_stage = -1;
    const auto publishPlanningProgress = [&](int stage_key, const char* label,
                                             uint64_t completed, uint64_t total) {
        if (abandoned()) return;
        const auto now = std::chrono::steady_clock::now();
        const bool stage_changed = stage_key != last_progress_stage;
        const bool stage_complete = total != 0 && completed >= total;
        if (!stage_changed && !stage_complete &&
            last_progress_at.time_since_epoch().count() != 0 &&
            now - last_progress_at < std::chrono::milliseconds(100)) {
            return;
        }
        std::string progress_status(label);
        if (total != 0) {
            const uint64_t bounded_completed = std::min(completed, total);
            const uint64_t percent = static_cast<uint64_t>(
                static_cast<long double>(bounded_completed) * 100.0L / total);
            progress_status += " (" + std::to_string(percent) + "%, " +
                std::to_string(bounded_completed) + "/" + std::to_string(total) + ")";
        }
        if (abandoned()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation == run_generation_.load(std::memory_order_acquire)) {
                status_ = std::move(progress_status);
            }
        }
        last_progress_stage = stage_key;
        last_progress_at = now;
    };
    const auto parser_progress = [&](const SchematicParseProgress& progress) {
        const bool pixel_art = request.source_type == ImportSourceType::PixelArtPng;
        const bool midi_music = request.source_type == ImportSourceType::CommandMusicMidi;
        const bool litematic = request.source_type == ImportSourceType::Litematic;
        const bool bdx = request.source_type == ImportSourceType::Bdx;
        const bool mcworld = request.source_type == ImportSourceType::Mcworld;
        const bool infinitecz = request.source_type == ImportSourceType::InfiniteczBuild;
        switch (progress.stage) {
            case SchematicParseStage::ReadingSource:
                publishPlanningProgress(
                    0, pixel_art ? "decoding pixel-art source"
                        : midi_music ? "reading MIDI source"
                        : litematic ? "reading litematic source"
                        : bdx ? "reading bdx source"
                        : mcworld ? "reading mcworld source"
                        : infinitecz ? "reading .infinity source" : "reading schematic source",
                    progress.completed, progress.total);
                break;
            case SchematicParseStage::RoutingBlocks:
                publishPlanningProgress(
                    1, pixel_art ? "converting pixel-art rows"
                        : midi_music ? "writing MIDI command chain"
                        : litematic ? "routing litematic blocks"
                        : bdx ? "routing bdx blocks"
                        : mcworld ? "routing mcworld blocks"
                        : infinitecz ? "routing .infinity blocks" : "routing schematic blocks",
                    progress.completed, progress.total);
                break;
            case SchematicParseStage::FinalizingSpools:
                publishPlanningProgress(2, "finalizing chunk spools",
                                        progress.completed, progress.total);
                break;
        }
    };
    request.parse.cancellation_requested = abandoned;
    request.parse.progress_callback = parser_progress;
    request.pixel_art.cancellation_requested = abandoned;
    request.pixel_art.progress_callback = parser_progress;
    bool parsed = false;
    if (request.source_type == ImportSourceType::PixelArtPng) {
        PixelArtParser parser;
        parsed = parser.parse(request.pixel_art, &result, &error);
    } else if (request.source_type == ImportSourceType::CommandMusicMidi) {
        MidiCommandMusicParser parser;
        MidiParseOptions midi_parse_options;
        midi_parse_options.cancellation_requested = request.parse.cancellation_requested;
        MidiCommandMusicOptions midi_options;
        midi_options.output = request.parse;
        parsed = parser.parseFileToCommandMusic(request.parse.source_path, midi_parse_options,
                                                midi_options, nullptr, &result, &error);
    } else if (request.source_type == ImportSourceType::Litematic) {
        LitematicParser parser;
        parsed = parser.parse(request.parse, mapper_, &result, &error);
    } else if (request.source_type == ImportSourceType::Bdx) {
        BdxParser parser;
        parsed = parser.parse(request.parse, mapper_, &result, &error);
    } else if (request.source_type == ImportSourceType::Mcworld) {
        McworldParser parser;
        parsed = parser.parse(request.parse, mapper_, &result, &error);
    } else if (request.source_type == ImportSourceType::InfiniteczBuild) {
        InfiniteczBuildParser parser;
        parsed = parser.parse(request.parse, mapper_, &result, &error);
    } else {
        SchematicParser parser;
        parsed = parser.parse(request.parse, mapper_, &result, &error);
    }
    if (parsed && retained_command_block_payload_count != 0) {
        LOGI("[command-block] retained=%llu legacy_execute_converted=%llu",
             static_cast<unsigned long long>(retained_command_block_payload_count),
             static_cast<unsigned long long>(converted_legacy_execute_count));
    }
    if (parsed && command_block_writer) {
        if (command_block_writer->recordCount() == 0) {
            // Do not leave an empty sidecar: normal imports must remain a
            // direct Verification -> Completed transition.
            command_block_writer->discard();
        } else if (!command_block_writer->finish(&command_block_manifest, &error)) {
            parsed = false;
            if (error.empty()) error = "cannot finalize command-block spool";
        }
    }
    if (parsed && sign_writer) {
        if (sign_writer->recordCount() == 0) {
            sign_writer->discard();
        } else if (!sign_writer->finish(&sign_spool_path, &error)) {
            parsed = false;
            if (error.empty()) error = "cannot finalize sign spool";
        }
    }
    if (parsed && entity_writer) {
        if (entity_writer->recordCount() == 0) {
            entity_writer->discard();
        } else if (!entity_writer->finish(&entity_spool_path, &error)) {
            parsed = false;
            if (error.empty()) error = "cannot finalize entity spool";
        }
    }
    if (parsed && container_item_writer) {
        if (container_item_writer->recordCount() == 0) {
            container_item_writer->discard();
        } else if (!container_item_writer->finish(&container_item_spool_path, &error)) {
            parsed = false;
            if (error.empty()) error = "cannot finalize container-item spool";
        }
    }
    if (parsed && request.source_type == ImportSourceType::Litematic &&
        request.config.overwrite_policy == OverwritePolicy::ClearImportedBounds &&
        result.source_region_count > 1) {
        parsed = false;
        error = "multi-region litematic imports cannot clear existing blocks; "
                "disable overwrite clearing to preserve gaps between regions";
    }
    if (abandoned()) {
        cleanupSpoolDirectory(parse_spool_directory);
        return;
    }
    if (!parsed) {
        LOGE("[planning] parse failed: %s", error.c_str());
        cleanupSpoolDirectory(parse_spool_directory);
        if (abandoned()) return;
        controller_.failActiveUnit(error);
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = error;
        return;
    }
    if (request.source_type == ImportSourceType::PixelArtPng &&
        request.pixel_art.create_maps_after_import) {
        uint64_t map_columns = 0;
        uint64_t map_rows = 0;
        uint64_t map_count = 0;
        if (!automaticMapPlan(result.source_volume_bounds, &map_columns, &map_rows,
                              &map_count, &error) ||
            !saveDeferredDataState(parse_spool_directory + "/" +
                                       kMapCreationStateFileName, map_count, 0, &error)) {
            cleanupSpoolDirectory(parse_spool_directory);
            if (abandoned()) return;
            controller_.failActiveUnit(error.empty()
                ? "cannot initialize automatic map creation state" : error);
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = error.empty()
                ? "cannot initialize automatic map creation state" : error;
            return;
        }
        std::lock_guard<std::mutex> map_lock(mutex_);
        if (generation != run_generation_.load(std::memory_order_acquire)) return;
        map_source_bounds_ = result.source_volume_bounds;
        map_tile_columns_ = map_columns;
        map_tile_rows_ = map_rows;
        map_tile_count_ = map_count;
        map_tile_cursor_ = 0;
        map_tile_persisted_cursor_ = 0;
        map_surface_y_ = result.source_volume_bounds.max_y;
    }
    if (request.config.place_deny_layer) {
        std::string foundation_error;
        const BlockBounds& source_bounds = result.source_volume_bounds;
        if (!source_bounds.isValid()) {
            foundation_error = "cannot determine source bounds for the deny foundation";
        } else if (source_bounds.min_y == std::numeric_limits<int32_t>::min()) {
            // Do not subtract in int32 space: a malformed/hostile source at
            // INT32_MIN would wrap its foundation to the top of the world.
            foundation_error = "source minimum Y is too low to place a deny foundation below it";
        } else {
            const int32_t foundation_y = source_bounds.min_y - 1;
            const BlockBounds foundation_bounds{
                source_bounds.min_x, foundation_y, source_bounds.min_z,
                source_bounds.max_x, foundation_y, source_bounds.max_z};
            if (!controller_.setPlanningDenyLayerBounds(foundation_bounds, &foundation_error)) {
                if (foundation_error.empty()) {
                    foundation_error = "cannot persist deny foundation bounds";
                }
            } else {
                LOGI("[planning] deny foundation bounds=(%d,%d,%d)-(%d,%d,%d)",
                     foundation_bounds.min_x, foundation_bounds.min_y,
                     foundation_bounds.min_z, foundation_bounds.max_x,
                     foundation_bounds.max_y, foundation_bounds.max_z);
            }
        }
        if (!foundation_error.empty()) {
            cleanupSpoolDirectory(parse_spool_directory);
            if (abandoned()) return;
            controller_.failActiveUnit(foundation_error);
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = foundation_error;
            return;
        }
    }
    LOGI("[planning] parse complete blocks=%llu chunks=%zu source_regions=%u",
         static_cast<unsigned long long>(result.imported_block_count), result.chunks.size(),
         result.source_region_count);
    if (result.degraded_block_count != 0) {
        LOGI("[planning] %llu blocks imported with state degradation; first=%s",
             static_cast<unsigned long long>(result.degraded_block_count),
             result.first_degradation_reason.c_str());
    }
    if (result.omitted_entity_count != 0) {
        LOGI("[planning] omitted %llu litematic entities from block-only import",
             static_cast<unsigned long long>(result.omitted_entity_count));
    }
    const char* payload_source = request.source_type == ImportSourceType::CommandMusicMidi
        ? "MIDI command music"
        : request.source_type == ImportSourceType::Bdx
        ? "BDX"
        : request.source_type == ImportSourceType::Mcworld ? "mcworld"
        : request.source_type == ImportSourceType::InfiniteczBuild ? ".infinity" : "source";
    if (result.omitted_block_entity_count != 0) {
        LOGI("[planning] omitted %llu %s block-entity payloads from block-only import",
             static_cast<unsigned long long>(result.omitted_block_entity_count),
              payload_source);
    }
    if (result.omitted_command_block_data_count != 0) {
        LOGI("[planning] omitted %llu %s command-block payloads that could not be preserved",
             static_cast<unsigned long long>(result.omitted_command_block_data_count),
             payload_source);
    }
    if (result.command_block_payload_count != 0) {
        LOGI("[planning] retained %llu deferred command-block payloads for post-import write",
             static_cast<unsigned long long>(result.command_block_payload_count));
    }
    if (result.entity_payload_count != 0) {
        LOGI("[planning] retained %llu deferred entity payloads for post-import write",
             static_cast<unsigned long long>(result.entity_payload_count));
    }
    if (result.container_item_payload_count != 0) {
        LOGI("[planning] retained %llu deferred container items for post-import write",
             static_cast<unsigned long long>(result.container_item_payload_count));
    }
    if (result.sign_payload_count != 0) {
        LOGI("[planning] retained %llu deferred signs for post-import write",
             static_cast<unsigned long long>(result.sign_payload_count));
    }
    std::string planning_notice;
    if (request.source_type == ImportSourceType::CommandMusicMidi &&
        result.source_volume_bounds.isValid()) {
        planning_notice = "MIDI score is ready after import; power the redstone-controlled Music Start "
            "command block at (" + std::to_string(result.source_volume_bounds.min_x) + "," +
            std::to_string(result.source_volume_bounds.min_y) + "," +
            std::to_string(result.source_volume_bounds.min_z) + ") to play it";
    }
    if (result.degraded_block_count != 0) {
        planning_notice = std::to_string(result.degraded_block_count) +
            " blocks have unrepresentable Java states; first: " +
            result.first_degradation_reason;
    }
    if (result.omitted_entity_count != 0) {
        if (!planning_notice.empty()) planning_notice += "; ";
        planning_notice += std::to_string(result.omitted_entity_count) +
            " litematic entities were omitted from this block-only import";
    }
    if (result.omitted_block_entity_count != 0) {
        if (!planning_notice.empty()) planning_notice += "; ";
        planning_notice += std::to_string(result.omitted_block_entity_count) +
            (request.source_type == ImportSourceType::Bdx ? " BDX block-entity payloads were omitted"
             : request.source_type == ImportSourceType::Mcworld ? " mcworld block-entity payloads were omitted"
             : request.source_type == ImportSourceType::InfiniteczBuild ? " .infinity block-entity payloads were omitted"
                        : " block-entity payloads were omitted");
    }
    if (result.omitted_command_block_data_count != 0) {
        if (!planning_notice.empty()) planning_notice += "; ";
        planning_notice += std::to_string(result.omitted_command_block_data_count) +
            (request.source_type == ImportSourceType::Bdx ? " BDX command payloads could not be preserved"
             : request.source_type == ImportSourceType::Mcworld ? " mcworld command payloads could not be preserved"
             : request.source_type == ImportSourceType::InfiniteczBuild ? " .infinity command payloads could not be preserved"
                        : " command-block payloads could not be preserved");
    }
    if (result.command_block_payload_count != 0) {
        if (!planning_notice.empty()) planning_notice += "; ";
        planning_notice += std::to_string(result.command_block_payload_count) +
             " command-block payloads will be written after block verification";
    }
    if (result.sign_payload_count != 0) {
        if (!planning_notice.empty()) planning_notice += "; ";
        planning_notice += std::to_string(result.sign_payload_count) +
            " signs will be restored and verified after block verification";
    }
    if (result.entity_payload_count != 0) {
        if (!planning_notice.empty()) planning_notice += "; ";
        planning_notice += std::to_string(result.entity_payload_count) +
            " entities will be restored after block verification";
    }
    if (result.container_item_payload_count != 0) {
        if (!planning_notice.empty()) planning_notice += "; ";
        planning_notice += std::to_string(result.container_item_payload_count) +
            " container items will be restored after block verification";
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        degradation_notice_ = planning_notice;
        status_ = "optimizing merged commands in C++";
        total_block_count_ = result.imported_block_count;
        imported_block_count_ = 0;
    }
    if (!controller_.setPlanningDegradationNotice(planning_notice, &error)) {
        cleanupSpoolDirectory(parse_spool_directory);
        if (abandoned()) return;
        controller_.failActiveUnit(error);
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = error;
        return;
    }
    const int32_t region_span = regionSpanForImportConfig(request.config);
    const auto command_progress = [&](const CommandSpoolBuildProgress& progress) {
        switch (progress.stage) {
            case CommandSpoolBuildStage::OptimizingChunks:
                publishPlanningProgress(3, "merging blocks into fill commands",
                                        progress.completed, progress.total);
                break;
            case CommandSpoolBuildStage::WritingVerificationPlan:
                publishPlanningProgress(4, "writing verification plan",
                                        progress.completed, progress.total);
                break;
            case CommandSpoolBuildStage::AggregatingRegions:
                publishPlanningProgress(5, "combining neighboring chunk command spools",
                                        progress.completed, progress.total);
                break;
        }
    };
    if (!CommandSpoolBuilder::build(parse_spool_directory,
                                    request.config.blocks_per_second,
                                    request.config.overwrite_policy,
                                    region_span,
                                    &result.chunks, &error, abandoned,
                                     usesAdaptiveRegionGrid(request.identity.options_hash),
                                     command_progress,
                                     request.config.verify_after_import,
                                     request.config.place_deny_layer,
                                     0U)) {
        cleanupSpoolDirectory(parse_spool_directory);
        if (abandoned()) return;
        controller_.failActiveUnit(error);
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = error;
        return;
    }
    // The compact command spools and verification plan now contain everything
    // needed for execution and restore. Keeping the per-block raw copies would
    // double disk use and make recovery scan every block again.
    removeRawChunkSpools(result.chunks);
    for (ChunkDescriptor& descriptor : result.chunks) {
        if (abandoned()) {
            cleanupSpoolDirectory(parse_spool_directory);
            return;
        }
        if (!controller_.addChunk(std::move(descriptor), &error)) {
            cleanupSpoolDirectory(parse_spool_directory);
            if (abandoned()) return;
            controller_.failActiveUnit(error);
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = error;
            return;
        }
    }
    if (abandoned()) {
        cleanupSpoolDirectory(parse_spool_directory);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "finalizing import plan";
    }
    if (!controller_.finishPlanning(&error)) {
        LOGE("[planning] finish failed: %s", error.c_str());
        cleanupSpoolDirectory(parse_spool_directory);
        if (generation != run_generation_.load(std::memory_order_acquire)) return;
        if (controller_.state() == ImportState::Planning) {
            controller_.failActiveUnit(error);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = error;
        return;
    }
    LOGI("[planning] ready blocks=%llu", static_cast<unsigned long long>(result.imported_block_count));
    std::lock_guard<std::mutex> lock(mutex_);
    // A game tick can start the first work unit immediately after
    // finishPlanning changes the controller state. Do not overwrite its newer
    // execution status when the parser worker publishes readiness.
    if (status_ == "finalizing import plan") status_ = "ready";
    } catch (const std::bad_alloc&) {
        cleanupSpoolDirectory(parse_spool_directory);
        if (generation == run_generation_.load(std::memory_order_acquire)) {
            controller_.failActiveUnit("building import needs more memory than available");
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "building import needs more memory than available";
        }
    } catch (...) {
        cleanupSpoolDirectory(parse_spool_directory);
        if (generation == run_generation_.load(std::memory_order_acquire)) {
            controller_.failActiveUnit("unexpected C++ parser or optimizer failure");
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "unexpected C++ parser or optimizer failure";
        }
    }
}

void BuildImportRuntime::onGameTick() try {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (worker_.joinable() && !worker_running_.load(std::memory_order_acquire)) {
        worker_.join();
    }
    struct GameTickScope {
        explicit GameTickScope(bool* flag) : flag_(flag) { *flag_ = true; }
        ~GameTickScope() { *flag_ = false; }
        bool* flag_;
    } game_tick_scope(&in_game_tick_);
    const auto now = std::chrono::steady_clock::now();
    maybeLogPerformanceTelemetry(now);
    if (pollNativeEvents()) {
        serviceMapStorageChest(now);
        serviceMapStorageAnvil(now);
        return;
    }

    ImportState observed_state = controller_.state();
    if (isContextSensitiveState(observed_state)) {
        // Automatic maps use the stable world identity (which already
        // includes the game's dimension ID) and exact window/session IDs.
        // Actor_getDimension may become unreadable while the anvil UI opens;
        // its raw pointer must not pause an otherwise live map transaction.
        if (!map_creation_requested_) {
            const uintptr_t dimension = NativeWorldAccess::dimensionToken();
            if (dimension != 0) {
                if (native_dimension_token_ == 0) native_dimension_token_ = dimension;
                else if (native_dimension_token_ != dimension) {
                    native_dimension_token_ = dimension;
                    onWorldContextChanged({"__native_dimension_changed__", current_world_.dimension_id});
                    serviceMapStorageChest(now);
                    serviceMapStorageAnvil(now);
                    return;
                }
            }
        }
        if (!controller_.worldMatches(current_world_)) {
            onWorldContextChanged(current_world_);
            serviceMapStorageChest(now);
            serviceMapStorageAnvil(now);
            return;
        }
    }
    serviceMapStorageChest(now);
    serviceMapStorageAnvil(now);
    // The initial stale ticking-area removal is queued while planning starts,
    // so suppress feedback before it as well as before the first TP or fill.
    // On terminal paths do the inverse: remove the ticking area first, then
    // restore feedback, keeping cleanup output out of the command UI too.
    if (commandFeedbackSuppressionRequested()) {
        if (!driveCommandFeedback(now)) return;
        if (!flushPendingCleanupCommands(now)) return;
    } else {
        // Cleanup commands belong to the world in which they were created.
        // Poll and close on a context change before issuing any queued removal.
        if (!flushPendingCleanupCommands(now)) return;
        if (!driveCommandFeedback(now)) return;
    }
    // Held removals never gate the tick: a completed dynamic region only has
    // to keep ticking until its settle deadline, everything else proceeds.
    serviceHeldRegionCleanups(now);

    if (recovery_pending_) {
        recovery_pending_ = false;
        const ImportPhase recovery_phase = active_unit_
            ? active_unit_->phase
            : deferred_window_phase_.value_or(ImportPhase::Structure);
        const std::chrono::milliseconds recovery_settle =
            phaseSettleDelay(recovery_phase);
        releaseLoadedRegion(true);
        resetActiveUnitRuntime();
        std::string recovery_error;
        if (!controller_.resume(current_world_, &recovery_error)) {
            pending_resume_settle_delay_ = std::chrono::milliseconds(0);
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "recovery failed: " + recovery_error;
        } else {
            pending_resume_settle_delay_ = recovery_settle;
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "reloading and replaying failed partition";
        }
        return;
    }

    if (phase_settle_ready_at_.time_since_epoch().count()) {
        if (now < phase_settle_ready_at_) return;
        phase_settle_ready_at_ = {};
    }

    observed_state = controller_.state();
    const bool draining_deferred_window =
        stage_ == ExecuteStage::Drain && controller_.hasDeferredUnits();
    if (observed_state == ImportState::Verifying && !draining_deferred_window) {
        // Keep the controller in Verifying until every deferred payload has
        // been applied. That makes pause/world-change checkpoints resumable
        // instead of exposing a structure with silently missing metadata.
        if (command_block_writer_active_) {
            tickCommandBlockWrite(now);
            return;
        }
        if (sign_writer_active_) {
            tickSignWrite(now);
            return;
        }
        if (container_item_writer_active_) {
            tickContainerWrite(now);
            return;
        }
        if (entity_writer_active_) {
            tickEntityWrite(now);
            return;
        }
        if (map_creation_active_) {
            tickMapCreation(now);
            return;
        }
        if (!final_verification_started_) beginFinalVerification();
        if (verification_pending_) tickFinalVerification(now);
        return;
    }
    if (observed_state != ImportState::Running && !draining_deferred_window) return;
    if (region_add_ready_at_.time_since_epoch().count() && now < region_add_ready_at_) return;
    if (region_add_ready_at_.time_since_epoch().count()) region_add_ready_at_ = {};
    if constexpr (kDataQueueBarriersEnabled) {
        // Queue barriers are disabled for the unpaced import path.
        if (!servicePipelinedDataBarrier(now)) return;
    }

    const auto scheduler_deadline = now + kImportSchedulerTickBudget;
    uint32_t scheduler_transitions = 0;
drive_active_stage:
    if (!active_unit_ && stage_ != ExecuteStage::Drain) {
        beginNextUnit();
        if (stage_ != ExecuteStage::Clear && stage_ != ExecuteStage::Build &&
            stage_ != ExecuteStage::Cleanup) return;
    }
    if (stage_ == ExecuteStage::Prepare) {
        if (prepare_index_ < prepare_commands_.size()) {
            const auto begin = prepare_commands_.begin() +
                static_cast<std::ptrdiff_t>(prepare_index_);
            std::vector<std::string> commands(begin, prepare_commands_.end());
            if (std::any_of(commands.begin(), commands.end(), [](const std::string& command) {
                    return command.rfind("/tp ", 0) == 0;
                })) {
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = "teleporting to import region";
            }
            size_t sent_count = 0;
            if (!executeCommands(commands, &sent_count) || sent_count != commands.size()) {
                prepare_index_ += sent_count;
                scheduleActiveUnitRecovery("Python RPC transport failed while loading region");
                return;
            }
            prepare_index_ = prepare_commands_.size();
        }
        startChunkLoadProbe(true);
        stage_ = ExecuteStage::Wait;
        return;
    }
    if (stage_ == ExecuteStage::Wait) {
        bool probe_due = now >= chunk_ready_at_;
        if (!probe_due && chunk_early_probe_at_.time_since_epoch().count() &&
            now >= chunk_early_probe_at_) {
            // Start the authoritative server probe as soon as the short TP
            // grace period expires. The old path waited for both a LevelChunk
            // packet and native readability before it would even send the
            // probe, which made the 350 ms fallback the common fast path.
            probe_due = true;
            chunk_early_probe_at_ = {};
        }
        if (!chunk_loaded_ && probe_due) {
            if (activeChunkProbeReadable(now)) {
                if (++chunk_readable_samples_ >= chunk_required_readable_samples_) {
                    chunk_loaded_ = true;
                    if (server_chunk_probe_ack_timed_out_ &&
                        !server_chunk_probe_confirmed_) {
                        LOGI("[chunk-probe] native-readable without RPC ACK "
                             "samples=%u region=(%d,%d)",
                             chunk_readable_samples_, active_unit_->coord.x,
                             active_unit_->coord.z);
                    }
                    confirmed_region_chunks_.insert(active_unit_->coord);
                    if (region_wait_started_at_.time_since_epoch().count()) {
                        ++telemetry_region_wait_count_;
                        telemetry_region_wait_microseconds_ += static_cast<uint64_t>(
                            std::max<int64_t>(0,
                                std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() -
                                    region_wait_started_at_).count()));
                        region_wait_started_at_ = {};
                    }
                    if (loaded_region_probe_bounds_.isValid() &&
                        sameBounds(server_chunk_probe_bounds_, loaded_region_probe_bounds_)) {
                        loaded_region_fully_confirmed_ = true;
                    }
                }
            } else {
                chunk_readable_samples_ = 0;
            }
        }
        if (chunk_loaded_) {
            stage_ = !clear_plan_.empty() ? ExecuteStage::Clear :
                (!active_unit_->spool_path.empty() ? ExecuteStage::Build : ExecuteStage::Cleanup);
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "region loaded; continuing import";
        } else if (now >= chunk_load_probe_deadline_) {
            scheduleActiveUnitRecovery("region load timed out");
        }
        if (!chunk_loaded_) return;
    }
    if (stage_ == ExecuteStage::Clear) {
        // A deferred static window can stay open while the next region is
        // loaded. If that overlap used up the bounded age, acknowledge the
        // existing queue before adding more clear commands.
        if (dataWindowNeedsDrain() && !tryPipelineDataDrain(now)) {
            startDataDrain(ImportPhase::Clear, ExecuteStage::Clear);
            return;
        }
        if (clear_index_ >= clear_plan_.size()) {
            if (drain_to_build_) {
                drain_to_build_ = false;
                if (dataWindowNeedsDrain() && !tryPipelineDataDrain(now)) {
                    startDataDrain(ImportPhase::Clear, ExecuteStage::Build);
                } else {
                    stage_ = ExecuteStage::Build;
                }
            } else {
                stage_ = ExecuteStage::Cleanup;
            }
            return;
        }
        preparePlacementBatch();
        const size_t remaining_commands = clear_plan_.size() - clear_index_;
        const auto deadline = scheduler_deadline;
        const auto batch_build_started = std::chrono::steady_clock::now();
        batch_commands_.clear();
        batch_block_counts_.clear();
        batch_commands_.reserve(std::min(remaining_commands, max_batch_commands_));
        batch_block_counts_.reserve(std::min(remaining_commands, max_batch_commands_));
        double tentative_block_tokens = available_block_tokens_;
        double tentative_command_tokens = available_command_tokens_;
        while (clear_index_ + batch_commands_.size() < clear_plan_.size() &&
               tentative_block_tokens >= 1.0 && tentative_command_tokens >= 1.0 &&
               batch_commands_.size() < max_batch_commands_ &&
               !commandBatchDeadlineReached(batch_commands_.size(), deadline)) {
            const PlannedCommand& planned =
                clear_plan_[clear_index_ + batch_commands_.size()];
            batch_commands_.push_back(commandFor(planned));
            batch_block_counts_.push_back(planned.block_count);
            tentative_block_tokens -= planned.block_count;
            tentative_command_tokens -= 1.0;
        }
        telemetry_batch_build_microseconds_ += static_cast<uint64_t>(
            std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - batch_build_started).count()));
        if (!batch_commands_.empty()) {
            size_t sent_count = 0;
            const bool complete = executeCommands(batch_commands_, &sent_count);
            uint64_t emitted_blocks = 0;
            for (size_t index = 0; index < sent_count; ++index) {
                emitted_blocks += batch_block_counts_[index];
            }
            clear_index_ += sent_count;
            if (sent_count != 0) {
                noteDataCommandsSent(sent_count, emitted_blocks);
                if (undo_run_active_) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    imported_block_count_ = std::min(
                        total_block_count_, imported_block_count_ + emitted_blocks);
                }
            }
            if (!complete) {
                scheduleActiveUnitRecovery("Python RPC transport failed while clearing region");
                return;
            }
        }
        if (clear_index_ >= clear_plan_.size()) {
            if (drain_to_build_) {
                drain_to_build_ = false;
                if (dataWindowNeedsDrain() && !tryPipelineDataDrain(now)) {
                    startDataDrain(ImportPhase::Clear, ExecuteStage::Build);
                } else {
                    stage_ = ExecuteStage::Build;
                }
            } else {
                stage_ = ExecuteStage::Cleanup;
            }
        } else if (dataWindowNeedsDrain() && !tryPipelineDataDrain(now)) {
            startDataDrain(ImportPhase::Clear, ExecuteStage::Clear);
        }
        if (stage_ != ExecuteStage::Build && stage_ != ExecuteStage::Cleanup) return;
    }
    if (stage_ == ExecuteStage::Build) {
        // Region loading is deliberately overlapped with the previous static
        // command window. Re-check its limits here because the load probe may
        // have consumed the entire age budget.
        if (dataWindowNeedsDrain() && !tryPipelineDataDrain(now)) {
            startDataDrain(active_unit_->phase, ExecuteStage::Build);
            return;
        }
        if (command_prefetcher_ && command_prefetcher_->exhausted()) {
            stage_ = ExecuteStage::Cleanup;
            return;
        }
        preparePlacementBatch();
        const auto deadline = scheduler_deadline;
        const auto batch_build_started = std::chrono::steady_clock::now();
        CommandBatchPrefetcher::TakeResult prefetched;
        if (command_prefetcher_) {
            const double effective_block_tokens = available_block_tokens_;
            prefetched = command_prefetcher_->take(
                max_batch_commands_, effective_block_tokens, available_command_tokens_,
                deadline, &batch_commands_, &batch_block_counts_);
        } else {
            batch_commands_.clear();
            batch_block_counts_.clear();
        }
        telemetry_batch_build_microseconds_ += static_cast<uint64_t>(
            std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - batch_build_started).count()));
        if (!batch_commands_.empty()) {
            size_t sent_count = 0;
            const bool complete = executeCommands(batch_commands_, &sent_count);
            uint64_t emitted = 0;
            for (size_t index = 0; index < sent_count; ++index) {
                emitted += batch_block_counts_[index];
            }
            if (sent_count != 0) {
                noteDataCommandsSent(sent_count, emitted);
                active_unit_emitted_blocks_ += emitted;
                active_command_index_ += sent_count;
                controller_.recordActiveProgress(active_command_index_);
                std::lock_guard<std::mutex> lock(mutex_);
                imported_block_count_ = std::min(total_block_count_, imported_block_count_ + emitted);
            }
            if (!complete) {
                scheduleActiveUnitRecovery("Python RPC transport failed while importing partition");
                return;
            }
        }
        const bool reached_end = prefetched.exhausted ||
            (command_prefetcher_ && command_prefetcher_->failed());
        if (batch_commands_.empty() && !reached_end && command_prefetcher_ &&
            available_block_tokens_ >= 1.0 && available_command_tokens_ >= 1.0 &&
            throughput_governor_.dispatchReady(now)) {
            ++telemetry_prefetch_starvations_;
        }
        if (reached_end) {
            if (command_prefetcher_ && command_prefetcher_->failed()) {
                const std::string prefetch_error = command_prefetcher_->error();
                controller_.failActiveUnit(prefetch_error.empty()
                    ? "corrupt merged command spool" : prefetch_error);
                const uint64_t durable_blocks = undo_run_active_
                    ? durable_undo_block_count_ : controller_.completedBlockCount();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    imported_block_count_ = durable_blocks;
                }
                releaseLoadedRegion(true);
                resetActiveUnitRuntime();
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = prefetch_error.empty()
                    ? "corrupt merged command spool" : prefetch_error;
                return;
            }
            stage_ = ExecuteStage::Cleanup;
        } else if (dataWindowNeedsDrain() && !tryPipelineDataDrain(now)) {
            startDataDrain(active_unit_->phase, ExecuteStage::Build);
        }
        if (stage_ != ExecuteStage::Cleanup) return;
    }
    if (stage_ == ExecuteStage::Drain) {
        preparePlacementBatch();
        const std::string eager_failure = takeDrainFailure();
        if (!eager_failure.empty()) {
            scheduleActiveUnitRecovery(eager_failure);
            return;
        }
        if (!dataCommandsDrained(now)) {
            const std::string failure = takeDrainFailure();
            if (!failure.empty()) {
                scheduleActiveUnitRecovery(failure);
            } else {
                // The barrier wait is otherwise idle time; start loading the
                // next region's chunks under it.
                maybePrefetchNextRegionPrepare(now);
            }
            return;
        }
        const bool closing_deferred_window = !active_unit_ &&
            drain_resume_stage_ == ExecuteStage::None && controller_.hasDeferredUnits();
        if (closing_deferred_window) {
            const ImportPhase completed_phase =
                deferred_window_phase_.value_or(drain_phase_);
            std::string checkpoint_error;
            if (!controller_.commitDeferredUnits(&checkpoint_error)) {
                std::string pause_error;
                const bool checkpointed = controller_.pause(&pause_error);
                if (!checkpointed) controller_.emergencyPauseNoCheckpoint();
                const uint64_t durable_blocks = undo_run_active_
                    ? durable_undo_block_count_ : controller_.completedBlockCount();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    imported_block_count_ = durable_blocks;
                }
                releaseLoadedRegion(true);
                resetActiveUnitRuntime();
                deferred_window_units_ = 0;
                deferred_window_phase_.reset();
                deferred_completed_units_.clear();
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = checkpointed
                    ? "checkpoint write failed after command-window acknowledgement: " +
                      checkpoint_error + "; import paused for idempotent replay"
                    : "checkpoint write failed after command-window acknowledgement: " +
                      checkpoint_error + "; pause checkpoint also failed: " + pause_error;
                return;
            }
            if (undo_run_active_) {
                if (durable_undo_block_count_ >= total_block_count_ ||
                    deferred_undo_volume_ > total_block_count_ - durable_undo_block_count_) {
                    durable_undo_block_count_ = total_block_count_;
                } else {
                    durable_undo_block_count_ += deferred_undo_volume_;
                }
            }
            for (const auto& completed : deferred_completed_units_) {
                const auto retries = unit_retry_counts_.find(completed.first);
                if (retries == unit_retry_counts_.end() ||
                    completed.second >= kImportPhaseCount) continue;
                retries->second[completed.second] = 0;
                if (std::all_of(retries->second.begin(), retries->second.end(),
                                [](uint32_t count) { return count == 0; })) {
                    unit_retry_counts_.erase(retries);
                }
            }
            const ImportState next_state = controller_.state();
            const BuildImportRuntimeMetadata committed = controller_.runtimeMetadata();
            if (next_state == ImportState::Verifying ||
                committed.phase_index != phaseIndex(completed_phase)) {
                const std::chrono::milliseconds settle_delay =
                    phaseSettleDelay(completed_phase);
                if (settle_delay.count() != 0) {
                    phase_settle_ready_at_ = now + settle_delay;
                }
            }
            deferred_window_units_ = 0;
            deferred_window_phase_.reset();
            deferred_completed_units_.clear();
            deferred_undo_volume_ = 0;
            active_unit_last_send_at_ = {};
            resetDrainBarrier();
            stage_ = ExecuteStage::None;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = "server queue drained; command window committed";
            }
            if (controller_.state() == ImportState::Running &&
                !phase_settle_ready_at_.time_since_epoch().count()) {
                beginNextUnit();
                if (active_unit_ && ++scheduler_transitions < kMaxSchedulerTransitionsPerTick &&
                    std::chrono::steady_clock::now() < scheduler_deadline &&
                    (stage_ == ExecuteStage::Clear || stage_ == ExecuteStage::Build ||
                     stage_ == ExecuteStage::Cleanup)) {
                    goto drive_active_stage;
                }
            }
            return;
        }
        if (drain_resume_stage_ != ExecuteStage::None) {
            const ExecuteStage resume = drain_resume_stage_;
            drain_resume_stage_ = ExecuteStage::None;
            active_unit_last_send_at_ = {};
            resetDrainBarrier();
            stage_ = resume;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = resume == ExecuteStage::Clear
                    ? "server caught up; continuing partition clear"
                    : "server caught up; continuing block placement";
            }
            if (++scheduler_transitions < kMaxSchedulerTransitionsPerTick &&
                std::chrono::steady_clock::now() < scheduler_deadline) {
                goto drive_active_stage;
            }
            return;
        }
        active_unit_last_send_at_ = {};
        resetDrainBarrier();
        stage_ = ExecuteStage::Cleanup;
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "server queue drained; committing partition";
        return;
    }
    if (stage_ == ExecuteStage::Cleanup) {
        const ChunkCoord completed_coord = active_unit_->coord;
        const size_t completed_phase = phaseIndex(active_unit_->phase);
        uint64_t completed_undo_volume = 0;
        if (undo_run_active_ && active_unit_->phase == ImportPhase::Clear &&
            !addBoundsVolume(active_unit_->imported_bounds, &completed_undo_volume)) {
            scheduleActiveUnitRecovery("undo partition volume is invalid");
            return;
        }
        if (kDataQueueBarriersEnabled && supportsDeferredCommit(active_unit_->phase)) {
            const ImportPhase window_phase = active_unit_->phase;
            std::string deferred_error;
            if (!controller_.completeActiveUnitDeferred(
                    active_unit_emitted_blocks_, &deferred_error)) {
                controller_.rewindActiveProgress(nullptr);
                std::string pause_error;
                const bool checkpointed = controller_.pause(&pause_error);
                if (!checkpointed) controller_.emergencyPauseNoCheckpoint();
                const uint64_t durable_blocks = undo_run_active_
                    ? durable_undo_block_count_ : controller_.completedBlockCount();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    imported_block_count_ = durable_blocks;
                }
                releaseLoadedRegion(true);
                resetActiveUnitRuntime();
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = "cannot defer completed static partition: " + deferred_error;
                if (!checkpointed) status_ += "; durable pause failed: " + pause_error;
                return;
            }
            if (deferred_window_units_ == 0) {
                deferred_window_phase_ = window_phase;
            }
            ++deferred_window_units_;
            deferred_completed_units_.emplace_back(completed_coord, completed_phase);
            if (undo_run_active_) {
                const uint64_t remaining = durable_undo_block_count_ >= total_block_count_
                    ? 0 : total_block_count_ - durable_undo_block_count_;
                if (deferred_undo_volume_ >= remaining ||
                    completed_undo_volume > remaining - deferred_undo_volume_) {
                    deferred_undo_volume_ = remaining;
                } else {
                    deferred_undo_volume_ += completed_undo_volume;
                }
            }
            const std::optional<WorkUnit> next = controller_.peekNextUnit();
            const bool same_window = next && supportsDeferredCommit(next->phase) &&
                deferred_window_phase_ && next->phase == *deferred_window_phase_;
            const bool close_window = !same_window || dataWindowNeedsDrain() ||
                deferred_window_units_ >= kMaxDeferredWindowUnits;
            resetActiveUnitRuntime(true);
            if (close_window) {
                startDataDrain(window_phase, ExecuteStage::None);
            } else {
                beginNextUnit();
                if (active_unit_ && ++scheduler_transitions < kMaxSchedulerTransitionsPerTick &&
                    std::chrono::steady_clock::now() < scheduler_deadline &&
                    (stage_ == ExecuteStage::Clear || stage_ == ExecuteStage::Build ||
                     stage_ == ExecuteStage::Cleanup)) {
                    goto drive_active_stage;
                }
            }
            return;
        }
        if (kDataQueueBarriersEnabled && active_unit_last_send_at_.time_since_epoch().count()) {
            startDataDrain(active_unit_->phase, ExecuteStage::None);
            return;
        }
        std::string checkpoint_error;
        if (!controller_.completeActiveUnit(active_unit_emitted_blocks_, &checkpoint_error)) {
            controller_.rewindActiveProgress(nullptr);
            std::string pause_error;
            const bool checkpointed = controller_.pause(&pause_error);
            if (!checkpointed) controller_.emergencyPauseNoCheckpoint();
            const uint64_t durable_blocks = undo_run_active_
                ? durable_undo_block_count_ : controller_.completedBlockCount();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                imported_block_count_ = durable_blocks;
            }
            releaseLoadedRegion(true);
            resetActiveUnitRuntime();
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = checkpointed
                ? "checkpoint write failed after partition completion: " + checkpoint_error +
                  "; import paused for safe replay"
                : "checkpoint write failed after partition completion: " + checkpoint_error +
                  "; pause checkpoint also failed: " + pause_error + "; stopped in memory";
            return;
        }
        if (undo_run_active_) {
            if (durable_undo_block_count_ >= total_block_count_ ||
                completed_undo_volume > total_block_count_ - durable_undo_block_count_) {
                durable_undo_block_count_ = total_block_count_;
            } else {
                durable_undo_block_count_ += completed_undo_volume;
            }
        }
        const auto retries = unit_retry_counts_.find(completed_coord);
        if (retries != unit_retry_counts_.end() && completed_phase < kImportPhaseCount) {
            retries->second[completed_phase] = 0;
            if (std::all_of(retries->second.begin(), retries->second.end(),
                            [](uint32_t count) { return count == 0; })) {
                unit_retry_counts_.erase(retries);
            }
        }
        const ImportState next_state = controller_.state();
        const BuildImportRuntimeMetadata completed_metadata = controller_.runtimeMetadata();
        const std::optional<WorkUnit> next = controller_.peekNextUnit();
        // Dynamic blocks need time to settle while the completed ticking area
        // is still loaded. Waiting only at a global phase boundary unloads all
        // earlier regions immediately and can lose trailing gravity/fluid
        // updates in very large imports.
        const bool region_handoff = next &&
            !sameChunk(next->region_coord, active_unit_->region_coord);
        const bool phase_boundary = next_state == ImportState::Verifying ||
            completed_metadata.phase_index != completed_phase;
        if (phase_boundary || region_handoff) {
            const std::chrono::milliseconds settle_delay =
                phaseSettleDelay(active_unit_->phase);
            if (settle_delay.count() != 0) {
                if (!phase_boundary &&
                    held_region_cleanups_.size() < kMaxHeldRegionCleanups) {
                    // A same-phase region handoff does not need to stall the
                    // pipeline. The completed area's removal is held until the
                    // settle deadline, so it keeps ticking while the next
                    // region already loads and builds.
                    hold_active_region_release_until_ =
                        std::chrono::steady_clock::now() + settle_delay;
                } else {
                    phase_settle_ready_at_ =
                        std::chrono::steady_clock::now() + settle_delay;
                }
            }
        }
        resetActiveUnitRuntime();
    }
} catch (const std::bad_alloc&) {
    handleGameTickFailure("not enough memory while advancing the import");
} catch (const std::exception& exception) {
    handleGameTickFailure(exception.what());
} catch (...) {
    handleGameTickFailure("unexpected C++ exception while advancing the import");
}

void BuildImportRuntime::handleGameTickFailure(const char* detail) noexcept {
    try {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
        const ImportState state = controller_.state();
        if (active_unit_) {
            controller_.rewindActiveProgress(nullptr);
            std::lock_guard<std::mutex> lock(mutex_);
            imported_block_count_ = active_unit_imported_start_;
        }
        if (state == ImportState::Verifying) {
            const size_t resume_index = verification_repair_chunk_
                ? verification_repair_sample_begin_ : verification_sample_index_;
            controller_.recordVerificationProgress(resume_index, false, nullptr);
        }
        std::string checkpoint_error;
        const bool checkpointed = controller_.pause(&checkpoint_error);
        if (!checkpointed) controller_.emergencyPauseNoCheckpoint();
        const CheckpointSnapshot paused_snapshot = controller_.checkpointSnapshot();
        const uint64_t durable_blocks = undo_run_active_
            ? durable_undo_block_count_ : paused_snapshot.completed_block_count;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            imported_block_count_ = durable_blocks;
        }
        recovery_pending_ = false;
        // Fold held and prefetched ticking-area removals into the tracked
        // cleanup queue (send_cleanup=true), matching every other pause/terminal
        // path. releaseLoadedRegion(false) here would strand a prefetched
        // /tickingarea add until the next resume()/cancel().
        releaseLoadedRegion(true);
        resetVerificationRuntime();
        resetCommandBlockRuntime();
        resetSignRuntime();
        resetContainerRuntime();
        resetEntityRuntime();
        resetActiveUnitRuntime();
        invalidateRpcTransport();
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "import paused after internal error: " +
            std::string(detail && *detail ? detail : "unknown failure");
        if (!checkpointed && !checkpoint_error.empty()) {
            status_ += "; checkpoint write failed: " + checkpoint_error;
        }
    } catch (...) {
        controller_.emergencyPauseNoCheckpoint();
        try { recovery_pending_ = false; } catch (...) {}
        try { releaseLoadedRegion(false); } catch (...) {}
        try { resetVerificationRuntime(); } catch (...) {}
        try { resetCommandBlockRuntime(); } catch (...) {}
        try { resetSignRuntime(); } catch (...) {}
        try { resetContainerRuntime(); } catch (...) {}
        try { resetEntityRuntime(); } catch (...) {}
        try { resetActiveUnitRuntime(); } catch (...) {}
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "import paused after recovery itself failed";
        } catch (...) {}
    }
}

void BuildImportRuntime::beginNextUnit() {
    const bool preserve_data_window = controller_.hasDeferredUnits();
    std::string acquire_error;
    active_unit_ = controller_.acquireNextUnit(&acquire_error);
    if (!active_unit_) {
        if (!acquire_error.empty()) {
            std::string pause_error;
            const bool checkpointed = controller_.pause(&pause_error);
            if (!checkpointed) controller_.emergencyPauseNoCheckpoint();
            const uint64_t durable_blocks = undo_run_active_
                ? durable_undo_block_count_ : controller_.completedBlockCount();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                imported_block_count_ = durable_blocks;
            }
            recovery_pending_ = false;
            releaseLoadedRegion(true);
            resetVerificationRuntime();
            resetCommandBlockRuntime();
            resetSignRuntime();
            resetContainerRuntime();
            resetEntityRuntime();
            resetActiveUnitRuntime();
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "cannot persist the next active partition: " + acquire_error;
            if (!checkpointed) {
                status_ += "; durable pause failed: " + pause_error +
                    "; stopped in memory";
            }
            return;
        }
        if (controller_.state() == ImportState::Verifying) {
            releaseLoadedRegion(true);
            beginFinalVerification();
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = controller_.failureReason();
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_unit_imported_start_ = imported_block_count_;
        status_ = (active_unit_->phase == ImportPhase::Clear ? "clearing chunk " : "importing chunk ") +
            std::to_string(active_unit_->coord.x) + "," + std::to_string(active_unit_->coord.z);
    }
    active_unit_emitted_blocks_ = 0;
    active_command_index_ = 0;
    const bool has_source_commands = active_unit_->phase != ImportPhase::Clear &&
        !active_unit_->spool_path.empty();
    chunk_loaded_ = false;
    chunk_readable_samples_ = 0;
    chunk_required_readable_samples_ = 3;
    region_wait_started_at_ = {};
    chunk_early_probe_at_ = {};
    chunk_ready_at_ = {};
    chunk_load_probe_deadline_ = {};
    if (!preserve_data_window) active_unit_last_send_at_ = {};
    clear_plan_.clear();
    clear_index_ = 0;
    drain_to_build_ = false;
    drain_resume_stage_ = ExecuteStage::None;
    drain_phase_ = ImportPhase::Structure;
    if (!preserve_data_window) {
        unbarriered_command_count_ = 0;
        unbarriered_block_count_ = 0;
        data_window_started_at_ = {};
    }

    if (active_unit_->phase == ImportPhase::Clear || active_unit_->clear_before_build) {
        WorkUnit clear_unit = *active_unit_;
        clear_unit.phase = ImportPhase::Clear;
        clear_plan_ = controller_.makeClearPlan(clear_unit);
    }
    std::vector<PlannedCommand> foundation_plan =
        controller_.makeDenyFoundationPlan(*active_unit_);
    if (!foundation_plan.empty()) {
        clear_plan_.insert(clear_plan_.end(),
                           std::make_move_iterator(foundation_plan.begin()),
                           std::make_move_iterator(foundation_plan.end()));
    }
    drain_to_build_ = has_source_commands && !clear_plan_.empty();

    const bool reuse_region = loaded_region_ &&
        sameChunk(*loaded_region_, active_unit_->region_coord);
    if (!reuse_region) {
        // A normal region handoff does not need a separate cleanup ACK round
        // trip. The RPC transport preserves command order, so remove the
        // previous tickingarea immediately before adding and teleporting to
        // the next one in the same batch. Terminal paths (pause, cancel,
        // recovery, and context changes) still use releaseLoadedRegion(true)
        // and its tracked cleanup barrier.
        const std::string handoff_cleanup = loaded_region_cleanup_command_;
        const auto hold_release_until = hold_active_region_release_until_;
        hold_active_region_release_until_ = {};
        releaseLoadedRegion(false);
        loaded_region_ = active_unit_->region_coord;
        loaded_region_started_at_ = std::chrono::steady_clock::now();
        confirmed_region_chunks_.clear();
        loaded_region_fully_confirmed_ = false;
        loaded_region_probe_bounds_ = regionCoreBounds(
            *active_unit_, controller_.runtimeMetadata().chunk_size);
        loaded_region_cleanup_command_ = controller_.makeCleanupCommand(*active_unit_);
        // This runs on the game tick, so the local player position may be read
        // directly. Knowing it lets the prepare batch split a long teleport
        // into bounded hops for servers that reject long-distance TPs.
        int32_t player_x = 0;
        int32_t player_y = 0;
        int32_t player_z = 0;
        const bool has_player_position =
            NativeWorldAccess::getLocalPlayerBlockPosition(&player_x, &player_y,
                                                           &player_z);
        prepare_commands_ = controller_.makePrepareCommands(
            *active_unit_, has_player_position, player_x, player_z);
        // Adopt a fresh prefetched prepare batch: the tickingarea add and tp
        // already went out during the previous unit's final drain, so only
        // leftover cleanup commands still need to be sent.
        bool prepare_prefetched = false;
        if (prefetched_prepare_region_) {
            const auto prefetched_at = prefetched_prepare_sent_at_;
            prepare_prefetched =
                sameChunk(*prefetched_prepare_region_, active_unit_->region_coord) &&
                prefetched_at.time_since_epoch().count() &&
                std::chrono::steady_clock::now() - prefetched_at <
                    std::chrono::seconds(30);
            if (!prepare_prefetched) {
                // An orphaned prefetched area must still be removed. The
                // deadline is already due, so the next tick sends it.
                WorkUnit orphan;
                orphan.region_coord = *prefetched_prepare_region_;
                held_region_cleanups_.push_back(
                    {controller_.makeCleanupCommand(orphan),
                     std::chrono::steady_clock::now()});
            } else {
                prepare_commands_.clear();
                loaded_region_started_at_ = prefetched_at;
            }
            prefetched_prepare_region_.reset();
            prefetched_prepare_sent_at_ = {};
        }
        // A held (settling) removal of the same area name must execute before
        // this add so the area is never deleted while it is in use again.
        // (A prefetched batch already consumed any matching held removal.)
        if (const std::optional<std::string> stale =
                takeHeldRegionCleanup(loaded_region_cleanup_command_)) {
            prepare_commands_.insert(prepare_commands_.begin(), *stale);
        }
        if (!handoff_cleanup.empty()) {
            if (hold_release_until.time_since_epoch().count() &&
                std::chrono::steady_clock::now() < hold_release_until &&
                held_region_cleanups_.size() < kMaxHeldRegionCleanups) {
                held_region_cleanups_.push_back({handoff_cleanup, hold_release_until});
            } else {
                prepare_commands_.insert(prepare_commands_.begin(), handoff_cleanup);
            }
        }
        prepare_index_ = 0;
        stage_ = ExecuteStage::Prepare;
    } else {
        prepare_commands_.clear();
        prepare_index_ = 0;
        if (loaded_region_fully_confirmed_ ||
            confirmed_region_chunks_.find(active_unit_->coord) !=
            confirmed_region_chunks_.end()) {
            chunk_loaded_ = true;
            stage_ = !clear_plan_.empty() ? ExecuteStage::Clear :
                (has_source_commands ? ExecuteStage::Build : ExecuteStage::Cleanup);
        } else {
            startChunkLoadProbe(false);
            stage_ = ExecuteStage::Wait;
        }
    }
    if (!has_source_commands) {
        return;
    }
    command_prefetcher_ = std::make_unique<CommandBatchPrefetcher>(
        active_unit_->spool_path, active_unit_->block_count,
        [](const PlannedCommand& command) { return BuildImportRuntime::commandFor(command); });
    if (!command_prefetcher_->valid()) {
        controller_.failActiveUnit("cannot open or validate merged command spool");
        const uint64_t durable_blocks = undo_run_active_
            ? durable_undo_block_count_ : controller_.completedBlockCount();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            imported_block_count_ = durable_blocks;
        }
        releaseLoadedRegion(true);
        resetActiveUnitRuntime();
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "cannot open or validate merged command spool";
    }
}

void BuildImportRuntime::resetActiveUnitRuntime(bool preserve_data_window) {
    command_prefetcher_.reset();
    active_unit_.reset();
    stage_ = ExecuteStage::None;
    prepare_index_ = 0;
    clear_index_ = 0;
    prepare_commands_.clear();
    clear_plan_.clear();
    drain_to_build_ = false;
    drain_resume_stage_ = ExecuteStage::None;
    drain_phase_ = ImportPhase::Structure;
    active_unit_emitted_blocks_ = 0;
    active_command_index_ = 0;
    if (!preserve_data_window) {
        unbarriered_command_count_ = 0;
        unbarriered_block_count_ = 0;
        data_window_started_at_ = {};
        active_unit_last_send_at_ = {};
        deferred_window_units_ = 0;
        deferred_window_phase_.reset();
        deferred_completed_units_.clear();
        deferred_undo_volume_ = 0;
        resetDrainBarrier();
        absorbPipelinedDataBarrier();
    }
    chunk_loaded_ = false;
    chunk_readable_samples_ = 0;
    chunk_required_readable_samples_ = 3;
    region_wait_started_at_ = {};
    chunk_early_probe_at_ = {};
    chunk_ready_at_ = {};
    chunk_load_probe_deadline_ = {};
    resetServerChunkProbe();
}

void BuildImportRuntime::resetVerificationRuntime() {
    ArmMapTextureDiagnostics(false);
    if (map_storage_chest_capture_) map_storage_chest_stop_requested_ = true;
    if (map_storage_anvil_window_) map_storage_anvil_stop_requested_ = true;
    map_inventory_transfer_.cancel();
    map_storage_renamed_transfer_network_id_ = 0;
    map_storage_renamed_transfer_uuid_ = -1;
    map_storage_renamed_transfer_cursor_ = 0;
    map_creation_active_ = false;
    map_slot_attempts_ = 0;
    map_target_ready_at_ = {};
    map_target_deadline_ = {};
    map_slot_retry_at_ = {};
    map_inventory_deadline_ = {};
    map_settle_ready_at_ = {};
    map_settle_log_at_ = {};
    map_texture_log_at_ = {};
    map_use_deadline_ = {};
    map_use_hold_ready_at_ = {};
    map_use_hotbar_slot_ = -1;
    map_use_network_stack_id_ = 0;
    map_use_empty_count_before_ = 0;
    map_use_filled_total_before_ = 0;
    map_use_filled_network_ids_before_.fill(0);
    map_use_new_filled_network_id_ = 0;
    map_use_new_filled_uuid_ = -1;
    map_use_held_filled_network_id_ = 0;
    map_use_texture_sequence_before_ = 0U;
    map_use_item_confirmed_ = false;
    final_verification_started_ = false;
    verification_pending_ = false;
    verification_chunk_plans_.clear();
    verification_samples_.clear();
    verification_sample_index_ = 0;
    verification_unverifiable_chunks_ = 0;
    verification_verified_chunks_ = 0;
    verification_region_coord_.reset();
    verification_readable_chunk_.reset();
    verification_region_load_retries_ = 0;
    verification_mismatch_sample_index_.reset();
    verification_mismatch_observations_ = 0;
    resetVerificationServerProbe();
    verification_repair_chunk_.reset();
    verification_repair_tracker_.reset();
    verification_repair_sample_begin_ = 0;
    verification_repair_phase_ = phaseIndex(ImportPhase::Structure);
    verification_repair_chunk_cursor_ = 0;
    verification_repair_replayed_paths_.clear();
    verification_repair_chunks_.clear();
    verification_repair_halo_chunks_.clear();
    verification_repair_clear_plan_.clear();
    verification_repair_clear_index_ = 0;
    verification_repair_probe_cursor_ = 0;
    verification_repair_probe_samples_ = 0;
    verification_repair_probe_ready_at_ = {};
    verification_repair_probe_deadline_ = {};
    verification_repair_emitted_blocks_ = 0;
    verification_repair_transport_failures_ = 0;
    verification_repair_force_drain_ = false;
    verification_repair_phase_dirty_ = false;
    verification_repair_settle_ready_at_ = {};
    verification_repair_reader_.reset();
    verification_repair_reader_chunk_filter_.clear();
    resetDrainBarrier();
    // Defensive: a background data barrier is always absorbed before the run
    // reaches verification, but clearing it here too means a late ACK can never
    // linger in received_rpc_acks_ if a future change alters that ordering.
    absorbPipelinedDataBarrier();
    resetServerChunkProbe();
}

void BuildImportRuntime::scheduleActiveUnitRecovery(const std::string& reason) {
    ChunkCoord retry_coord{};
    size_t phase = kImportPhaseCount;
    if (active_unit_) {
        retry_coord = active_unit_->coord;
        phase = phaseIndex(active_unit_->phase);
    } else if (controller_.hasDeferredUnits() && !deferred_completed_units_.empty()) {
        retry_coord = deferred_completed_units_.back().first;
        phase = deferred_completed_units_.back().second;
    } else {
        return;
    }
    // A missing/rejected barrier can be caused by the local player entity ID
    // changing after respawn or reconnect. Rebuild the Python closure before
    // every recovery attempt so it resolves GetLocalPlayerId again.
    invalidateRpcTransport();
    constexpr uint32_t kMaxRetries = 8;
    if (phase >= kImportPhaseCount) {
        handleGameTickFailure("active work unit has an invalid retry phase");
        return;
    }
    uint32_t& retry_count = unit_retry_counts_[retry_coord][phase];
    if (retry_count >= kMaxRetries) {
        controller_.rewindActiveProgress();
        std::string checkpoint_error;
        const bool checkpointed = controller_.pause(&checkpoint_error);
        if (!checkpointed) controller_.emergencyPauseNoCheckpoint();
        const CheckpointSnapshot paused_snapshot = controller_.checkpointSnapshot();
        const uint64_t durable_blocks = undo_run_active_
            ? durable_undo_block_count_ : paused_snapshot.completed_block_count;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            imported_block_count_ = durable_blocks;
            status_ = reason + " after retries; import paused";
            if (!checkpointed) {
                status_ += "; checkpoint write failed: " + checkpoint_error +
                    "; stopped in memory";
            }
        }
        recovery_pending_ = false;
        releaseLoadedRegion(true);
        resetVerificationRuntime();
        resetActiveUnitRuntime();
        return;
    }
    ++retry_count;
    controller_.rewindActiveProgress();
    std::string checkpoint_error;
    const bool checkpointed = controller_.pause(&checkpoint_error);
    if (!checkpointed) {
        controller_.emergencyPauseNoCheckpoint();
        const CheckpointSnapshot paused_snapshot = controller_.checkpointSnapshot();
        const uint64_t durable_blocks = undo_run_active_
            ? durable_undo_block_count_ : paused_snapshot.completed_block_count;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            imported_block_count_ = durable_blocks;
            status_ = reason + "; recovery checkpoint write failed: " +
                checkpoint_error + "; stopped in memory";
        }
        recovery_pending_ = false;
        releaseLoadedRegion(true);
        resetVerificationRuntime();
        resetActiveUnitRuntime();
        return;
    }
    const CheckpointSnapshot paused_snapshot = controller_.checkpointSnapshot();
    const bool full_replay = paused_snapshot.phase_index == phaseIndex(ImportPhase::Clear) &&
        paused_snapshot.chunk_index == 0;
    const uint64_t durable_blocks = undo_run_active_
        ? durable_undo_block_count_ : paused_snapshot.completed_block_count;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        imported_block_count_ = durable_blocks;
        status_ = reason + (full_replay ? "; rebuilding the complete import (" :
                                        "; replaying partition (") +
            std::to_string(retry_count) + "/" + std::to_string(kMaxRetries) + ")";
    }
    recovery_pending_ = true;
}

void BuildImportRuntime::releaseLoadedRegion(bool send_cleanup) {
    if (send_cleanup) {
        // Terminal paths (pause, cancel, recovery, context changes) fold any
        // held settling removals into the tracked cleanup barrier so no
        // ticking area outlives the run.
        for (const HeldRegionCleanup& held : held_region_cleanups_) {
            if (std::find(pending_cleanup_commands_.begin(),
                          pending_cleanup_commands_.end(),
                          held.command) == pending_cleanup_commands_.end()) {
                pending_cleanup_commands_.push_back(held.command);
            }
        }
        held_region_cleanups_.clear();
        held_region_cleanup_retry_at_ = {};
        hold_active_region_release_until_ = {};
        if (prefetched_prepare_region_) {
            // A prefetched area that was never adopted must not outlive the
            // run either.
            WorkUnit orphan;
            orphan.region_coord = *prefetched_prepare_region_;
            const std::string orphan_cleanup = controller_.makeCleanupCommand(orphan);
            if (std::find(pending_cleanup_commands_.begin(),
                          pending_cleanup_commands_.end(),
                          orphan_cleanup) == pending_cleanup_commands_.end()) {
                pending_cleanup_commands_.push_back(orphan_cleanup);
            }
            prefetched_prepare_region_.reset();
            prefetched_prepare_sent_at_ = {};
        }
    }
    if (send_cleanup && !loaded_region_cleanup_command_.empty()) {
        const std::string cleanup = loaded_region_cleanup_command_;
        if (std::find(pending_cleanup_commands_.begin(), pending_cleanup_commands_.end(),
                      cleanup) == pending_cleanup_commands_.end()) {
            pending_cleanup_commands_.push_back(cleanup);
        }
    }
    loaded_region_.reset();
    loaded_region_cleanup_command_.clear();
    loaded_region_probe_bounds_ = {};
    loaded_region_started_at_ = {};
    confirmed_region_chunks_.clear();
    loaded_region_fully_confirmed_ = false;
    chunk_loaded_ = false;
    chunk_readable_samples_ = 0;
    chunk_required_readable_samples_ = 3;
    chunk_early_probe_at_ = {};
    chunk_ready_at_ = {};
    chunk_load_probe_deadline_ = {};
    resetServerChunkProbe();
}

void BuildImportRuntime::serviceHeldRegionCleanups(
        std::chrono::steady_clock::time_point now) {
    if (held_region_cleanups_.empty()) return;
    if (held_region_cleanup_retry_at_.time_since_epoch().count() &&
        now < held_region_cleanup_retry_at_) {
        return;
    }
    held_region_cleanup_retry_at_ = {};
    while (!held_region_cleanups_.empty() &&
           now >= held_region_cleanups_.front().release_at) {
        const std::vector<std::string> commands{
            held_region_cleanups_.front().command};
        size_t sent_count = 0;
        if (!executeCommands(commands, &sent_count) ||
            sent_count != commands.size()) {
            // A transport hiccup only keeps the settled area loaded a little
            // longer; retry with a backoff instead of gating the tick.
            held_region_cleanup_retry_at_ = now + std::chrono::milliseconds(500);
            return;
        }
        held_region_cleanups_.pop_front();
    }
}

void BuildImportRuntime::maybePrefetchNextRegionPrepare(
        std::chrono::steady_clock::time_point now) {
    (void)now;
    // Only the final drain of an active (non-deferred) unit has both a known
    // successor and idle barrier time worth hiding.
    if (prefetched_prepare_region_ || !active_unit_ ||
        drain_resume_stage_ != ExecuteStage::None) {
        return;
    }
    if (controller_.state() != ImportState::Running) return;
    const std::optional<WorkUnit> next = controller_.peekNextUnit();
    if (!next || sameChunk(next->region_coord, active_unit_->region_coord)) return;
    int32_t player_x = 0;
    int32_t player_y = 0;
    int32_t player_z = 0;
    const bool has_player_position =
        NativeWorldAccess::getLocalPlayerBlockPosition(&player_x, &player_y,
                                                       &player_z);
    std::vector<std::string> commands = controller_.makePrepareCommands(
        *next, has_player_position, player_x, player_z);
    // A held (settling) removal of the same area name must execute before the
    // prefetched add so the area is never deleted while in use again.
    const std::string next_cleanup = controller_.makeCleanupCommand(*next);
    const std::optional<std::string> stale = takeHeldRegionCleanup(next_cleanup);
    if (stale) commands.insert(commands.begin(), *stale);
    size_t sent_count = 0;
    if (!executeCommands(commands, &sent_count) || sent_count != commands.size()) {
        // Transport trouble surfaces on the barrier path; re-queue the stale
        // removal so the old area cannot leak.
        if (stale && sent_count == 0) {
            held_region_cleanups_.push_front({*stale, std::chrono::steady_clock::now()});
        }
        return;
    }
    prefetched_prepare_region_ = next->region_coord;
    prefetched_prepare_sent_at_ = std::chrono::steady_clock::now();
}

std::optional<std::string> BuildImportRuntime::takeHeldRegionCleanup(
        const std::string& command) {
    for (auto it = held_region_cleanups_.begin();
         it != held_region_cleanups_.end(); ++it) {
        if (it->command == command) {
            std::string taken = std::move(it->command);
            held_region_cleanups_.erase(it);
            return taken;
        }
    }
    return std::nullopt;
}

bool BuildImportRuntime::flushPendingCleanupCommands(
        std::chrono::steady_clock::time_point now) {
    const auto stop_after_cleanup_failure = [&](std::string detail) {
        std::string checkpoint_error;
        const ImportState current_state = controller_.state();
        if (current_state == ImportState::Planning) {
            run_generation_.fetch_add(1, std::memory_order_acq_rel);
            controller_.cancel(true);
        } else if (current_state == ImportState::Running ||
                   current_state == ImportState::Verifying) {
            if (active_unit_) controller_.rewindActiveProgress(nullptr);
            if (!controller_.pause(&checkpoint_error)) {
                controller_.emergencyPauseNoCheckpoint();
            }
        }
        const uint64_t durable_blocks = undo_run_active_
            ? durable_undo_block_count_ : controller_.completedBlockCount();
        recovery_pending_ = false;
        releaseLoadedRegion(false);
        pending_cleanup_commands_.clear();
        held_region_cleanups_.clear();
        held_region_cleanup_retry_at_ = {};
        hold_active_region_release_until_ = {};
        prefetched_prepare_region_.reset();
        prefetched_prepare_sent_at_ = {};
        cleanup_barrier_uuids_.clear();
        cleanup_barrier_command_count_ = 0;
        cleanup_barrier_poll_at_ = {};
        cleanup_barrier_deadline_ = {};
        cleanup_barrier_failure_count_ = 0;
        cleanup_retry_at_ = {};
        region_add_ready_at_ = {};
        pending_resume_settle_delay_ = std::chrono::milliseconds(0);
    invalidateRpcTransport();
        resetVerificationRuntime();
        resetActiveUnitRuntime();
        {
            std::lock_guard<std::mutex> event_lock(native_event_mutex_);
            received_rpc_acks_.clear();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        imported_block_count_ = durable_blocks;
        status_ = std::move(detail);
        if (!checkpoint_error.empty()) {
            status_ += "; durable pause failed: " + checkpoint_error +
                "; stopped in memory";
        }
        return false;
    };

    if (pending_cleanup_commands_.empty() && cleanup_barrier_uuids_.empty()) {
        if (region_add_ready_at_.time_since_epoch().count() && now < region_add_ready_at_) {
            return false;
        }
        region_add_ready_at_ = {};
        cleanup_barrier_failure_count_ = 0;
        if (pending_resume_settle_delay_.count() != 0) {
            phase_settle_ready_at_ = now + pending_resume_settle_delay_;
            pending_resume_settle_delay_ = std::chrono::milliseconds(0);
        }
        return true;
    }
    if (!cleanup_barrier_uuids_.empty()) {
        if (now < cleanup_barrier_poll_at_) return false;
        const RpcResultState state = pollTrackedCommands(cleanup_barrier_uuids_);
        if (state == RpcResultState::Pending && now < cleanup_barrier_deadline_) {
            cleanup_barrier_poll_at_ = now + kRpcPollInterval;
            return false;
        }
        const bool sent_without_ack = state == RpcResultState::Pending;
        if (state == RpcResultState::Accepted || sent_without_ack) {
            LOGI("[cleanup-barrier] %s uuid=%s cleanup_commands=%zu",
                 sent_without_ack ? "sent-unconfirmed" : "accepted",
                 cleanup_barrier_uuids_.empty() ? "<none>" : cleanup_barrier_uuids_.front().c_str(),
                 cleanup_barrier_command_count_);
            if (sent_without_ack) {
                // A missing ACK is not proof of server execution. Only retire
                // the already-sent cleanup; subsequent region readability and
                // final block verification remain authoritative.
                std::lock_guard<std::mutex> event_lock(native_event_mutex_);
                for (const std::string& uuid : cleanup_barrier_uuids_) {
                    received_rpc_acks_.erase(uuid);
                }
            }
            cleanup_barrier_uuids_.clear();
            const size_t acknowledged = std::min(
                cleanup_barrier_command_count_, pending_cleanup_commands_.size());
            pending_cleanup_commands_.erase(
                pending_cleanup_commands_.begin(),
                pending_cleanup_commands_.begin() + static_cast<std::ptrdiff_t>(acknowledged));
            cleanup_barrier_command_count_ = 0;
            cleanup_barrier_failure_count_ = 0;
            cleanup_retry_at_ = {};
            const auto acknowledged_at = std::chrono::steady_clock::now();
            if (pending_cleanup_commands_.empty()) {
                region_add_ready_at_ = sent_without_ack
                    ? acknowledged_at + kUnconfirmedCleanupSettle
                    : std::chrono::steady_clock::time_point{};
                if (pending_resume_settle_delay_.count() != 0) {
                    phase_settle_ready_at_ =
                        acknowledged_at + pending_resume_settle_delay_;
                    pending_resume_settle_delay_ = std::chrono::milliseconds(0);
                }
            } else {
                cleanup_retry_at_ = acknowledged_at + std::chrono::milliseconds(50);
            }
            return pending_cleanup_commands_.empty() && !sent_without_ack;
        }
        LOGE("[cleanup-barrier] %s uuid=%s attempt=%u/%u",
             state == RpcResultState::Rejected ? "rejected" : "timeout",
             cleanup_barrier_uuids_.empty() ? "<none>" : cleanup_barrier_uuids_.front().c_str(),
             cleanup_barrier_failure_count_ + 1U, kMaxCleanupBarrierFailures);
        cleanup_barrier_uuids_.clear();
        cleanup_barrier_command_count_ = 0;
    invalidateRpcTransport();
        if (++cleanup_barrier_failure_count_ >= kMaxCleanupBarrierFailures) {
            return stop_after_cleanup_failure(
                state == RpcResultState::Rejected
                    ? "server repeatedly rejected the ticking-area cleanup barrier; import stopped"
                    : "ticking-area cleanup acknowledgement repeatedly timed out; import stopped");
        }
        cleanup_retry_at_ = now + std::chrono::milliseconds(500);
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = state == RpcResultState::Rejected
            ? "server rejected the ticking-area cleanup barrier"
            : "waiting to retry the ticking-area cleanup barrier";
        return false;
    }
    if (now < cleanup_retry_at_) return false;
    std::vector<std::string> commands = pending_cleanup_commands_;
    commands.push_back("/testfor @s");
    std::vector<std::string> uuids(commands.size());
    uuids.back() = nextRpcUuid();
    LOGI("[cleanup-barrier] dispatch cleanup_commands=%zu total_commands=%zu uuid=%s",
         pending_cleanup_commands_.size(), commands.size(), uuids.back().c_str());
    size_t sent_count = 0;
    if (!executeTrackedCommands(commands, uuids, &sent_count) || sent_count != commands.size()) {
        LOGE("[cleanup-barrier] dispatch failed sent=%zu expected=%zu uuid=%s",
             sent_count, commands.size(), uuids.back().c_str());
        if (++cleanup_barrier_failure_count_ >= kMaxCleanupBarrierFailures) {
            return stop_after_cleanup_failure(
                "Python RPC transport repeatedly failed during ticking-area cleanup; import stopped");
        }
        cleanup_retry_at_ = now + std::chrono::milliseconds(500);
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "waiting for Python RPC transport to clean the previous ticking area";
        return false;
    }
    cleanup_barrier_uuids_.assign(1, uuids.back());
    cleanup_barrier_command_count_ = pending_cleanup_commands_.size();
    const auto sent_at = std::chrono::steady_clock::now();
    cleanup_barrier_poll_at_ = sent_at + kRpcPollInterval;
    cleanup_barrier_deadline_ = sent_at + kCleanupAckGrace;
    cleanup_retry_at_ = {};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "waiting for ticking-area cleanup acknowledgement";
    }
    return false;
}

void BuildImportRuntime::startChunkLoadProbe(bool wait_for_region) {
    if (!active_unit_) return;
    chunk_readable_samples_ = 0;
    chunk_loaded_ = false;
    resetServerChunkProbe();
    const auto now = std::chrono::steady_clock::now();
    region_wait_started_at_ = now;
    chunk_early_probe_at_ = now + std::chrono::milliseconds(100);
    if (wait_for_region) {
        // TP and tickingarea are ordered in the same reliable stream. Give the
        // client a short render-thread grace period, then let the tracked
        // server probe decide readiness instead of always sleeping for the
        // full configured fallback delay (three seconds by default).
        chunk_ready_at_ = now + std::chrono::milliseconds(350);
    } else {
        chunk_ready_at_ = now + std::chrono::milliseconds(100);
    }
    const auto configured_fallback = std::chrono::milliseconds(
        static_cast<int64_t>(std::max<int32_t>(1, active_unit_->wait_ticks)) * 50);
    chunk_load_probe_deadline_ = now +
        std::max(std::chrono::milliseconds(10000),
                 configured_fallback + std::chrono::milliseconds(2000));
}

void BuildImportRuntime::resetServerChunkProbe() {
    if (!server_chunk_probe_uuids_.empty()) {
        std::lock_guard<std::mutex> lock(native_event_mutex_);
        for (const std::string& uuid : server_chunk_probe_uuids_) {
            received_rpc_acks_.erase(uuid);
        }
    }
    server_chunk_probe_bounds_ = {};
    server_chunk_probe_uuids_.clear();
    server_chunk_probe_poll_at_ = {};
    server_chunk_probe_retry_at_ = {};
    server_chunk_probe_deadline_ = {};
    server_chunk_probe_confirmed_ = false;
    server_chunk_probe_transport_failed_ = false;
    server_chunk_probe_ack_timed_out_ = false;
}

BuildImportRuntime::ChunkProbeState BuildImportRuntime::pollServerChunkProbe(
        const BlockBounds& bounds, std::chrono::steady_clock::time_point now,
        std::string* error) {
    if (!bounds.isValid()) {
        if (error) *error = "chunk probe bounds are invalid";
        return ChunkProbeState::TransportFailure;
    }
    if (!server_chunk_probe_bounds_.isValid() ||
        !sameBounds(server_chunk_probe_bounds_, bounds)) {
        resetServerChunkProbe();
        server_chunk_probe_bounds_ = bounds;
    }
    if (server_chunk_probe_confirmed_) return ChunkProbeState::Ready;

    if (server_chunk_probe_uuids_.empty()) {
        if (now < server_chunk_probe_retry_at_) {
            return server_chunk_probe_transport_failed_
                ? ChunkProbeState::TransportFailure : ChunkProbeState::Waiting;
        }
        server_chunk_probe_transport_failed_ = false;
        const int64_t width = static_cast<int64_t>(bounds.max_x) - bounds.min_x + 1;
        const int64_t depth = static_cast<int64_t>(bounds.max_z) - bounds.min_z + 1;
        if (width <= 0 || depth <= 0) {
            if (error) *error = "chunk probe scope is empty";
            return ChunkProbeState::TransportFailure;
        }
        const int32_t sample_y = static_cast<int32_t>(
            (static_cast<int64_t>(bounds.min_y) + bounds.max_y) / 2);
        std::vector<std::string> commands;
        std::vector<std::string> uuids;
        constexpr int64_t kMaximumProbeTileBlocks = 16384;
        constexpr size_t kMaximumProbeCommands = 256;
        commands.reserve(8);
        uuids.reserve(8);
        for (int64_t x_offset = 0; x_offset < width;) {
            const int64_t tile_width = std::min(width - x_offset,
                                                kMaximumProbeTileBlocks);
            const int64_t tile_depth_limit = std::max<int64_t>(
                1, kMaximumProbeTileBlocks / tile_width);
            for (int64_t z_offset = 0; z_offset < depth;) {
                const int64_t tile_depth = std::min(depth - z_offset,
                                                    tile_depth_limit);
                if (commands.size() >= kMaximumProbeCommands) {
                    if (error) *error = "chunk probe scope needs too many tiles";
                    return ChunkProbeState::TransportFailure;
                }
                const int64_t min_x = static_cast<int64_t>(bounds.min_x) + x_offset;
                const int64_t min_z = static_cast<int64_t>(bounds.min_z) + z_offset;
                const int64_t max_x = min_x + tile_width - 1;
                const int64_t max_z = min_z + tile_depth - 1;
                const std::string source_begin = std::to_string(min_x) + " " +
                    std::to_string(sample_y) + " " + std::to_string(min_z);
                const std::string source_end = std::to_string(max_x) + " " +
                    std::to_string(sample_y) + " " + std::to_string(max_z);
                // A rectangular self-comparison proves every chunk touched by
                // the tile is command-usable. The 16K cap stays below the
                // conservative command-volume limit while replacing dozens of
                // one-point RPC probes for the default 3x3 work region.
                commands.push_back("/testforblocks " + source_begin + " " +
                                   source_end + " " + source_begin + " all");
                uuids.push_back(nextRpcUuid());
                z_offset += tile_depth;
            }
            x_offset += tile_width;
        }
        size_t sent_count = 0;
        if (!executeTrackedCommands(commands, uuids, &sent_count) ||
            sent_count != commands.size()) {
            server_chunk_probe_transport_failed_ = true;
            server_chunk_probe_retry_at_ = now + std::chrono::milliseconds(500);
            if (error) *error = "Python RPC transport failed while probing chunks";
            return ChunkProbeState::TransportFailure;
        }
        server_chunk_probe_uuids_ = std::move(uuids);
        const auto sent_at = std::chrono::steady_clock::now();
        server_chunk_probe_poll_at_ = sent_at + kRpcPollInterval;
        server_chunk_probe_deadline_ = sent_at + kRpcProbeTimeout;
        return ChunkProbeState::Waiting;
    }

    if (now < server_chunk_probe_poll_at_) return ChunkProbeState::Waiting;
    const RpcResultState state = pollTrackedCommands(server_chunk_probe_uuids_);
    if (state == RpcResultState::Pending && now < server_chunk_probe_deadline_) {
        server_chunk_probe_poll_at_ = now + kRpcPollInterval;
        return ChunkProbeState::Waiting;
    }
    server_chunk_probe_uuids_.clear();
    if (state == RpcResultState::Accepted) {
        server_chunk_probe_confirmed_ = true;
        server_chunk_probe_transport_failed_ = false;
        server_chunk_probe_ack_timed_out_ = false;
        return ChunkProbeState::Ready;
    }
    if (state == RpcResultState::Rejected) {
        // A false testforblocks result normally means that at least one server
        // chunk is not ready yet. Retry with fresh UUIDs until the outer load
        // deadline decides whether the whole region must be reloaded.
        server_chunk_probe_transport_failed_ = false;
        server_chunk_probe_ack_timed_out_ = false;
        server_chunk_probe_retry_at_ = now + std::chrono::milliseconds(500);
        return ChunkProbeState::Rejected;
    }
    server_chunk_probe_transport_failed_ = true;
    if (state == RpcResultState::Pending) {
        server_chunk_probe_ack_timed_out_ = true;
    }
    server_chunk_probe_retry_at_ = now + std::chrono::milliseconds(500);
    invalidateRpcTransport();
    if (error) *error = "server chunk probe acknowledgement timed out";
    return ChunkProbeState::TransportFailure;
}

BlockBounds BuildImportRuntime::activeChunkProbeBounds() const {
    if (!active_unit_) return {};
    BlockBounds probe_bounds = active_unit_->imported_bounds;
    if (loaded_region_probe_bounds_.isValid()) {
        const int64_t min_chunk_x = floorDiv(loaded_region_probe_bounds_.min_x, kNativeChunkSize);
        const int64_t max_chunk_x = floorDiv(loaded_region_probe_bounds_.max_x, kNativeChunkSize);
        const int64_t min_chunk_z = floorDiv(loaded_region_probe_bounds_.min_z, kNativeChunkSize);
        const int64_t max_chunk_z = floorDiv(loaded_region_probe_bounds_.max_z, kNativeChunkSize);
        const int64_t width = max_chunk_x - min_chunk_x + 1;
        const int64_t depth = max_chunk_z - min_chunk_z + 1;
        // Probe the region core rather than its one-chunk dependency halo. The
        // default 3x3 logical core is 36 native chunks, so one proof covers all
        // command targets without putting the 100-chunk halo in one RPC batch.
        if (width > 0 && depth > 0 && width <= 64 && depth <= 64 &&
            width * depth <= 64) {
            probe_bounds = loaded_region_probe_bounds_;
        }
    }
    return probe_bounds;
}

bool BuildImportRuntime::activeChunkProbeReadable(
        std::chrono::steady_clock::time_point now) {
    const BlockBounds probe_bounds = activeChunkProbeBounds();
    if (!probe_bounds.isValid()) return false;
    std::string probe_error;
    const ChunkProbeState server_state =
        pollServerChunkProbe(probe_bounds, now, &probe_error);
    if (server_state == ChunkProbeState::Ready) {
        chunk_required_readable_samples_ = 1;
        return true;
    }
    if (server_state == ChunkProbeState::Waiting ||
        server_state == ChunkProbeState::Rejected) return false;
    bool any_level_chunk = false;
    const bool complete_level_chunk =
        levelChunkEvidenceForBounds(probe_bounds, &any_level_chunk);
    // A current LevelChunk packet plus readable native blocks is preferred.
    // Already-loaded target chunks may not emit a fresh LevelChunk packet,
    // so after an actual probe ACK timeout also accept stable native
    // readability in the current dimension. Dispatch failures and explicit
    // server rejections never grant this compatibility path.
    chunk_required_readable_samples_ = complete_level_chunk ? 2U : 3U;
    const bool native_fallback_ready = server_chunk_probe_ack_timed_out_ &&
        now >= chunk_ready_at_ + kNativeReadabilityFallbackGrace;
    return (complete_level_chunk || native_fallback_ready) &&
        nativeChunksReadable(probe_bounds);
}

bool BuildImportRuntime::nativeChunksReadable(const BlockBounds& bounds) {
    if (!bounds.isValid()) return false;
    const int32_t min_chunk_x = floorDiv(bounds.min_x, kNativeChunkSize);
    const int32_t max_chunk_x = floorDiv(bounds.max_x, kNativeChunkSize);
    const int32_t min_chunk_z = floorDiv(bounds.min_z, kNativeChunkSize);
    const int32_t max_chunk_z = floorDiv(bounds.max_z, kNativeChunkSize);
    const int64_t width = static_cast<int64_t>(max_chunk_x) - min_chunk_x + 1;
    const int64_t depth = static_cast<int64_t>(max_chunk_z) - min_chunk_z + 1;
    if (width <= 0 || depth <= 0 || width > 64 || depth > 64) return false;
    const int32_t sample_y = static_cast<int32_t>(
        (static_cast<int64_t>(bounds.min_y) + bounds.max_y) / 2);
    for (int64_t z = min_chunk_z; z <= max_chunk_z; ++z) {
        for (int64_t x = min_chunk_x; x <= max_chunk_x; ++x) {
            if (!NativeWorldAccess::isChunkReadable(
                    {static_cast<int32_t>(x), static_cast<int32_t>(z)},
                    kNativeChunkSize, sample_y)) {
                return false;
            }
        }
    }
    return true;
}

bool BuildImportRuntime::levelChunkEvidenceForBounds(const BlockBounds& bounds,
                                                     bool* any_evidence) {
    if (any_evidence) *any_evidence = false;
    if (!bounds.isValid() || !loaded_region_started_at_.time_since_epoch().count()) return false;
    const int32_t min_chunk_x = floorDiv(bounds.min_x, kNativeChunkSize);
    const int32_t max_chunk_x = floorDiv(bounds.max_x, kNativeChunkSize);
    const int32_t min_chunk_z = floorDiv(bounds.min_z, kNativeChunkSize);
    const int32_t max_chunk_z = floorDiv(bounds.max_z, kNativeChunkSize);
    bool complete = true;
    std::lock_guard<std::mutex> lock(native_event_mutex_);
    for (int64_t z = min_chunk_z; z <= max_chunk_z; ++z) {
        for (int64_t x = min_chunk_x; x <= max_chunk_x; ++x) {
            const bool current = received_level_chunks_.wasReceivedSince(
                {static_cast<int32_t>(x), static_cast<int32_t>(z)},
                loaded_region_started_at_);
            if (current && any_evidence) *any_evidence = true;
            if (!current) complete = false;
        }
    }
    return complete;
}

void BuildImportRuntime::noteDataCommandsSent(
        size_t command_count, uint64_t block_count) {
    const auto sent_at = std::chrono::steady_clock::now();
    if constexpr (kDataQueueBarriersEnabled) {
        resetDrainBarrier();
        if (unbarriered_command_count_ == 0 && unbarriered_block_count_ == 0) {
            data_window_started_at_ = sent_at;
        }
        unbarriered_command_count_ = command_count >
                std::numeric_limits<uint64_t>::max() - unbarriered_command_count_
            ? std::numeric_limits<uint64_t>::max()
            : unbarriered_command_count_ + command_count;
        unbarriered_block_count_ = block_count >
                std::numeric_limits<uint64_t>::max() - unbarriered_block_count_
            ? std::numeric_limits<uint64_t>::max()
            : unbarriered_block_count_ + block_count;
    }
    active_unit_last_send_at_ = sent_at;
    telemetry_data_commands_ += command_count;
    telemetry_data_blocks_ += block_count;
}

bool BuildImportRuntime::dataWindowNeedsDrain() const {
    if (!kDataQueueBarriersEnabled) return false;
    return unbarriered_command_count_ >= kMaxUnbarrieredCommands ||
        unbarriered_block_count_ >= kMaxUnbarrieredBlocks ||
        (data_window_started_at_.time_since_epoch().count() &&
         std::chrono::steady_clock::now() - data_window_started_at_ >=
             kMaxUnbarrieredAge);
}

bool BuildImportRuntime::tryPipelineDataDrain(std::chrono::steady_clock::time_point now) {
    if (!kDataQueueBarriersEnabled) {
        (void)now;
        return true;
    }
    (void)now;
    if (!active_unit_last_send_at_.time_since_epoch().count()) return false;
    // Only one background barrier may be outstanding: a second full window
    // means the server is not keeping up, so fall back to the blocking drain.
    if (!pipelined_barrier_uuids_.empty()) return false;
    const std::string uuid = nextRpcUuid();
    size_t sent_count = 0;
    if (!executeTrackedCommands({"/testfor @s"}, {uuid}, &sent_count) ||
        sent_count != 1) {
        // The blocking drain path retries and surfaces transport failures.
        return false;
    }
    pipelined_barrier_uuids_.push_back(uuid);
    const auto sent_at = std::chrono::steady_clock::now();
    pipelined_barrier_sent_at_ = sent_at;
    pipelined_barrier_poll_at_ = sent_at + kRpcPollInterval;
    pipelined_barrier_deadline_ = sent_at + kRpcBarrierTimeout;
    // The window is closed by the in-flight barrier; open the next one so
    // placement continues without a stage change.
    unbarriered_command_count_ = 0;
    unbarriered_block_count_ = 0;
    data_window_started_at_ = {};
    return true;
}

bool BuildImportRuntime::servicePipelinedDataBarrier(
        std::chrono::steady_clock::time_point now) {
    if (!kDataQueueBarriersEnabled) {
        (void)now;
        absorbPipelinedDataBarrier();
        return true;
    }
    if (pipelined_barrier_uuids_.empty()) return true;
    if (now < pipelined_barrier_poll_at_) return true;
    const RpcResultState state = pollTrackedCommands(pipelined_barrier_uuids_);
    if (state == RpcResultState::Pending && now < pipelined_barrier_deadline_) {
        pipelined_barrier_poll_at_ = now + kRpcPollInterval;
        return true;
    }
    const auto sent_at = pipelined_barrier_sent_at_;
    const bool timed_out = state == RpcResultState::Pending;
    absorbPipelinedDataBarrier();
    if (state == RpcResultState::Accepted) {
        const auto acknowledged_at = std::chrono::steady_clock::now();
        throughput_governor_.noteBarrierAcknowledged(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                acknowledged_at - sent_at));
        ++telemetry_barrier_count_;
        telemetry_barrier_microseconds_ += static_cast<uint64_t>(
            std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(
                acknowledged_at - sent_at).count()));
        return true;
    }
    if (timed_out) throughput_governor_.noteBarrierTimeout();
    invalidateRpcTransport();
    scheduleActiveUnitRecovery(state == RpcResultState::Rejected
        ? "server rejected the queue barrier command"
        : "server queue barrier acknowledgement timed out");
    return false;
}

void BuildImportRuntime::absorbPipelinedDataBarrier() {
    if (!pipelined_barrier_uuids_.empty()) {
        std::lock_guard<std::mutex> lock(native_event_mutex_);
        for (const std::string& uuid : pipelined_barrier_uuids_) {
            received_rpc_acks_.erase(uuid);
        }
    }
    pipelined_barrier_uuids_.clear();
    pipelined_barrier_sent_at_ = {};
    pipelined_barrier_poll_at_ = {};
    pipelined_barrier_deadline_ = {};
}

void BuildImportRuntime::startDataDrain(ImportPhase phase, ExecuteStage resume_stage) {
    if (!kDataQueueBarriersEnabled) {
        (void)phase;
        (void)resume_stage;
        resetDrainBarrier();
        return;
    }
    // The blocking barrier is ordered after any in-flight background barrier
    // on the same reliable stream, so its acknowledgement covers both windows.
    // Absorb the background one instead of feeding the governor a latency
    // sample inflated by the blocking wait.
    absorbPipelinedDataBarrier();
    drain_phase_ = phase;
    drain_resume_stage_ = resume_stage;
    stage_ = ExecuteStage::Drain;
    // The barrier is itself just one tracked RPC command. Issue it in the same
    // game tick that closes the data window so the following tick can spend
    // its time waiting for the server ACK instead of paying an avoidable
    // scheduler interval before dispatch.
    if (in_game_tick_ && active_unit_last_send_at_.time_since_epoch().count() &&
        drain_barrier_uuids_.empty()) {
        (void)dataCommandsDrained(std::chrono::steady_clock::now());
    }
}

bool BuildImportRuntime::dataCommandsDrained(std::chrono::steady_clock::time_point now,
                                             bool settle_repair_phase) {
    if (!kDataQueueBarriersEnabled) {
        (void)now;
        (void)settle_repair_phase;
        resetDrainBarrier();
        return true;
    }
    if (!active_unit_last_send_at_.time_since_epoch().count()) return true;
    if (drain_barrier_confirmed_) {
        return now >= drain_settle_ready_at_;
    }

    if (drain_barrier_uuids_.empty()) {
        const std::string uuid = nextRpcUuid();
        size_t sent_count = 0;
        if (!executeTrackedCommands({"/testfor @s"}, {uuid}, &sent_count) ||
            sent_count != 1) {
            drain_failure_ = "Python RPC transport failed while sending the server queue barrier";
            return false;
        }
        drain_barrier_uuids_.push_back(uuid);
        const auto sent_at = std::chrono::steady_clock::now();
        drain_barrier_sent_at_ = sent_at;
        drain_barrier_poll_at_ = sent_at + kRpcPollInterval;
        drain_barrier_deadline_ = sent_at + kRpcBarrierTimeout;
        return false;
    }
    if (now < drain_barrier_poll_at_) return false;
    const RpcResultState state = pollTrackedCommands(drain_barrier_uuids_);
    if (state == RpcResultState::Pending && now < drain_barrier_deadline_) {
        drain_barrier_poll_at_ = now + kRpcPollInterval;
        return false;
    }
    drain_barrier_uuids_.clear();
    if (state != RpcResultState::Accepted) {
        if (state == RpcResultState::Pending && now >= drain_barrier_deadline_) {
            throughput_governor_.noteBarrierTimeout();
        }
    invalidateRpcTransport();
        drain_failure_ = state == RpcResultState::Rejected
            ? "server rejected the queue barrier command"
            : "server queue barrier acknowledgement timed out";
        return false;
    }
    if (drain_barrier_sent_at_.time_since_epoch().count()) {
        const auto acknowledged_at = std::chrono::steady_clock::now();
        const auto barrier_latency = std::chrono::duration_cast<std::chrono::milliseconds>(
            acknowledged_at - drain_barrier_sent_at_);
        throughput_governor_.noteBarrierAcknowledged(
            barrier_latency);
        ++telemetry_barrier_count_;
        telemetry_barrier_microseconds_ += static_cast<uint64_t>(
            std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(
                acknowledged_at - drain_barrier_sent_at_).count()));
    }

    ImportPhase phase = drain_phase_;
    if (verification_repair_chunk_ &&
               verification_repair_phase_ < phaseIndex(ImportPhase::Count)) {
        phase = static_cast<ImportPhase>(verification_repair_phase_);
    } else if (active_unit_) {
        phase = active_unit_->phase;
    }
    // Normal imports settle once when the controller advances to the next
    // global phase. Repair runs have their own scheduler, so retain the phase
    // delay at each repair-phase barrier.
    const std::chrono::milliseconds settle_delay =
        verification_repair_chunk_ && settle_repair_phase
        ? phaseSettleDelay(phase) : std::chrono::milliseconds(0);
    drain_barrier_confirmed_ = true;
    unbarriered_command_count_ = 0;
    unbarriered_block_count_ = 0;
    data_window_started_at_ = {};
    drain_settle_ready_at_ = std::chrono::steady_clock::now() + settle_delay;
    // Token debt limits the next Clear/Build dispatch and survives unit
    // transitions. It must not serialize checkpointing, teleporting or chunk
    // loading after the ordered server barrier has already acknowledged this
    // window; those operations can overlap the debt refill without increasing
    // the data-command rate.
    return settle_delay.count() == 0;
}

bool BuildImportRuntime::repairPhaseDrained(
        ImportPhase phase, std::chrono::steady_clock::time_point now) {
    if (active_unit_last_send_at_.time_since_epoch().count()) {
        drain_phase_ = phase;
        if (!dataCommandsDrained(now, false)) return false;
        active_unit_last_send_at_ = {};
        resetDrainBarrier();
    }
    if (!verification_repair_phase_dirty_) return true;
    if (!verification_repair_settle_ready_at_.time_since_epoch().count()) {
        verification_repair_settle_ready_at_ = now + phaseSettleDelay(phase);
    }
    if (now < verification_repair_settle_ready_at_) return false;
    verification_repair_phase_dirty_ = false;
    verification_repair_settle_ready_at_ = {};
    resetDrainBarrier();
    return true;
}

std::string BuildImportRuntime::takeDrainFailure() {
    std::string failure;
    failure.swap(drain_failure_);
    return failure;
}

void BuildImportRuntime::resetDrainBarrier() {
    if (!drain_barrier_uuids_.empty()) {
        std::lock_guard<std::mutex> lock(native_event_mutex_);
        for (const std::string& uuid : drain_barrier_uuids_) {
            received_rpc_acks_.erase(uuid);
        }
    }
    drain_barrier_uuids_.clear();
    drain_barrier_poll_at_ = {};
    drain_barrier_deadline_ = {};
    drain_barrier_sent_at_ = {};
    drain_settle_ready_at_ = {};
    drain_barrier_confirmed_ = false;
    drain_failure_.clear();
}

void BuildImportRuntime::preparePlacementBatch() {
    available_block_tokens_ = std::numeric_limits<double>::infinity();
    available_command_tokens_ = std::numeric_limits<double>::infinity();
    max_batch_commands_ = kUnthrottledPlacementBatchCommands;
}

void BuildImportRuntime::resetPlacementBatch() {
    preparePlacementBatch();
    throughput_governor_.resetDispatchCadence();
}

void BuildImportRuntime::maybeLogPerformanceTelemetry(
        std::chrono::steady_clock::time_point now) {
    constexpr auto kTelemetryInterval = std::chrono::seconds(5);
    if (!telemetry_window_started_at_.time_since_epoch().count()) {
        telemetry_window_started_at_ = now;
        return;
    }
    if (now - telemetry_window_started_at_ < kTelemetryInterval) return;

    const double elapsed_seconds = std::max(
        0.001, std::chrono::duration<double>(now - telemetry_window_started_at_).count());
    const bool has_activity = telemetry_data_commands_ != 0 ||
        telemetry_rpc_batches_ != 0 || telemetry_barrier_count_ != 0 ||
        telemetry_region_wait_count_ != 0 || telemetry_prefetch_starvations_ != 0;
    if (has_activity) {
        const double average_rpc_batch = telemetry_rpc_batches_ == 0 ? 0.0 :
            static_cast<double>(telemetry_rpc_commands_) / telemetry_rpc_batches_;
        const double average_barrier_ms = telemetry_barrier_count_ == 0 ? 0.0 :
            static_cast<double>(telemetry_barrier_microseconds_) /
                telemetry_barrier_count_ / 1000.0;
        const double average_region_wait_ms = telemetry_region_wait_count_ == 0 ? 0.0 :
            static_cast<double>(telemetry_region_wait_microseconds_) /
                telemetry_region_wait_count_ / 1000.0;
        LOGI("[perf] window=%.2fs blocks=%llu block_rate=%.1f/s data_commands=%llu "
             "command_rate=%.1f/s rpc_batches=%llu avg_rpc_batch=%.1f "
             "python_ms=%.2f batch_build_ms=%.2f barriers=%llu avg_barrier_ms=%.1f "
             "region_waits=%llu avg_region_wait_ms=%.1f prefetch_starvations=%llu "
             "adaptive_scale=%.3f ack_ewma_ms=%.1f",
             elapsed_seconds,
             static_cast<unsigned long long>(telemetry_data_blocks_),
             static_cast<double>(telemetry_data_blocks_) / elapsed_seconds,
             static_cast<unsigned long long>(telemetry_data_commands_),
             static_cast<double>(telemetry_data_commands_) / elapsed_seconds,
             static_cast<unsigned long long>(telemetry_rpc_batches_),
             average_rpc_batch,
             static_cast<double>(telemetry_python_send_microseconds_) / 1000.0,
             static_cast<double>(telemetry_batch_build_microseconds_) / 1000.0,
             static_cast<unsigned long long>(telemetry_barrier_count_),
             average_barrier_ms,
             static_cast<unsigned long long>(telemetry_region_wait_count_),
             average_region_wait_ms,
             static_cast<unsigned long long>(telemetry_prefetch_starvations_),
             throughput_governor_.rateScale(),
             throughput_governor_.ackLatencyEwmaMilliseconds());
    }

    telemetry_window_started_at_ = now;
    telemetry_data_commands_ = 0;
    telemetry_data_blocks_ = 0;
    telemetry_rpc_batches_ = 0;
    telemetry_rpc_commands_ = 0;
    telemetry_python_send_microseconds_ = 0;
    telemetry_batch_build_microseconds_ = 0;
    telemetry_barrier_count_ = 0;
    telemetry_barrier_microseconds_ = 0;
    telemetry_region_wait_count_ = 0;
    telemetry_region_wait_microseconds_ = 0;
    telemetry_prefetch_starvations_ = 0;
}

void BuildImportRuntime::invalidateRpcTransport() noexcept {
    this->rpc_transport_ready_ = false;
    rpc_pointer_transport_ready_ = false;
}

bool BuildImportRuntime::commandFeedbackSuppressionRequested() const {
    const ImportState state = controller_.state();
    // start() queues stale ticking-area cleanup while the parser still owns the
    // planning state. Gate that command too, otherwise a large import leaks a
    // feedback line before its first placement command is issued.
    return !command_feedback_suppression_unavailable_ &&
           suppress_command_feedback_.load(std::memory_order_acquire) &&
           (state == ImportState::Planning || state == ImportState::Running ||
            state == ImportState::Verifying || recovery_pending_);
}

void BuildImportRuntime::resetCommandFeedbackTracking() {
    if (!command_feedback_uuid_.empty()) {
        std::lock_guard<std::mutex> lock(native_event_mutex_);
        received_rpc_acks_.erase(command_feedback_uuid_);
    }
    command_feedback_action_ = CommandFeedbackGate::Action::None;
    command_feedback_uuid_.clear();
    command_feedback_poll_at_ = {};
    command_feedback_deadline_ = {};
}

bool BuildImportRuntime::driveCommandFeedback(
        std::chrono::steady_clock::time_point now) {
    const bool suppress = commandFeedbackSuppressionRequested();
    command_feedback_gate_.requestSuppression(suppress);

    if (command_feedback_action_ != CommandFeedbackGate::Action::None) {
        if (command_feedback_uuid_.empty()) {
            const CommandFeedbackGate::Action action = command_feedback_action_;
            resetCommandFeedbackTracking();
            command_feedback_gate_.complete(action, false);
            command_feedback_retry_at_ = now + std::chrono::milliseconds(500);
            ++command_feedback_failure_count_;
            LOGE("[command-feedback] lost tracked RPC state while setting sendcommandfeedback");
            return false;
        }
        if (now < command_feedback_poll_at_) return false;
        const RpcResultState result = pollTrackedCommands({command_feedback_uuid_});
        if (result == RpcResultState::Pending && now < command_feedback_deadline_) {
            command_feedback_poll_at_ = now + kRpcPollInterval;
            return false;
        }

        const CommandFeedbackGate::Action action = command_feedback_action_;
        const std::string uuid = command_feedback_uuid_;
        const bool accepted = result == RpcResultState::Accepted;
        if (result == RpcResultState::Pending) invalidateRpcTransport();
        resetCommandFeedbackTracking();
        command_feedback_gate_.complete(action, accepted);
        if (!accepted) {
            command_feedback_retry_at_ = now + std::chrono::milliseconds(500);
            ++command_feedback_failure_count_;
            LOGE("[command-feedback] %s %s uuid=%s attempt=%u",
                 action == CommandFeedbackGate::Action::Disable ? "disable" : "restore",
                 result == RpcResultState::Rejected ? "rejected" : "timed out",
                 uuid.c_str(), command_feedback_failure_count_);
            if (action == CommandFeedbackGate::Action::Disable &&
                command_feedback_failure_count_ >= kMaxCommandFeedbackSetupFailures) {
                command_feedback_suppression_unavailable_ = true;
                command_feedback_gate_.requestSuppression(false);
                LOGE("[command-feedback] disable repeatedly failed; continuing import with feedback enabled");
            }
            return false;
        }
        command_feedback_failure_count_ = 0;
        command_feedback_retry_at_ = {};
        LOGI("[command-feedback] %s accepted uuid=%s",
             action == CommandFeedbackGate::Action::Disable ? "disabled" : "restored",
             uuid.c_str());
    }

    const CommandFeedbackGate::Action action = command_feedback_gate_.nextAction();
    if (action == CommandFeedbackGate::Action::None) {
        return suppress ? command_feedback_gate_.readyForSuppression()
                        : command_feedback_gate_.restored();
    }
    if (now < command_feedback_retry_at_) return false;

    const std::string uuid = nextRpcUuid();
    const std::string command = action == CommandFeedbackGate::Action::Disable
        ? "/gamerule sendcommandfeedback false"
        : "/gamerule sendcommandfeedback true";
    // Mark the action before issuing it. A local RPC failure can happen after
    // send2server accepted the bytes, and a later terminal transition must
    // still force the gamerule back to true.
    command_feedback_gate_.markDispatched(action);
    command_feedback_action_ = action;
    command_feedback_uuid_ = uuid;
    size_t sent_count = 0;
    if (!executeTrackedCommands({command}, {uuid}, &sent_count) || sent_count != 1U) {
        resetCommandFeedbackTracking();
        command_feedback_gate_.complete(action, false);
        command_feedback_retry_at_ = now + std::chrono::milliseconds(500);
        ++command_feedback_failure_count_;
        LOGE("[command-feedback] failed to dispatch %s attempt=%u",
             action == CommandFeedbackGate::Action::Disable ? "disable" : "restore",
             command_feedback_failure_count_);
        if (action == CommandFeedbackGate::Action::Disable &&
            command_feedback_failure_count_ >= kMaxCommandFeedbackSetupFailures) {
            command_feedback_suppression_unavailable_ = true;
            command_feedback_gate_.requestSuppression(false);
            LOGE("[command-feedback] disable repeatedly failed; continuing import with feedback enabled");
        }
        return false;
    }
    // Both gamerule transitions are ordered with following importer commands.
    // This client may execute them without exposing an ACK; waiting for the
    // Restore ACK caused an unbounded resend loop after a failed import.
    // A full local send only settles this optional UI-feedback gate, never a
    // block-write or verification result.
    command_feedback_gate_.complete(action, true);
    resetCommandFeedbackTracking();
    command_feedback_failure_count_ = 0;
    command_feedback_retry_at_ = {};
    LOGI("[command-feedback] %s sent-unconfirmed uuid=%s",
         action == CommandFeedbackGate::Action::Disable ? "disable" : "restore",
         uuid.c_str());
    // A new run can owe a Restore from the previous run and simultaneously
    // request suppression. Send the fresh Disable on the next tick first.
    return action == CommandFeedbackGate::Action::Disable || !suppress;
}

bool BuildImportRuntime::ensureRpcTransport() {
    if (rpc_transport_ready_) return true;
    rpc_pointer_transport_ready_ = false;
    setenv(kRpcPointerCapabilityEnvironment, "0", 1);
    setenv(kRpcFastPackerEnvironment, "0", 1);
    setenv(kRpcPointerProbeEnvironment, "0", 1);
    LOGI("[rpc-transport] installing Python transport version=16 in_game_tick=%d",
         in_game_tick_ ? 1 : 0);
    std::string code =
        "import sys, os, struct, msgpack, _pynetmodule\n"
        "import mod.client.extraClientApi as _bi_rpc_api\n"
        "try:\n"
        "  import ctypes as _bi_rpc_ctypes\n"
        "except BaseException:\n"
        "  _bi_rpc_ctypes = None\n"
        "def _build_import_rpc_tuple_default(value):\n"
        "  return {'__type__': 'tuple', 'value': list(value)} if isinstance(value, tuple) else value\n"
        "_build_import_rpc_fast_packer = 0\n"
        "try:\n"
        "  _build_import_rpc_packer = msgpack.Packer(use_bin_type=True, strict_types=True, default=_build_import_rpc_tuple_default)\n"
        "  _build_import_rpc_pack = _build_import_rpc_packer.pack\n"
        "  _build_import_rpc_fast_packer = 1\n"
        "except BaseException:\n"
        "  def _build_import_rpc_pack(value, _packb=msgpack.packb, _default=_build_import_rpc_tuple_default):\n"
        "    return _packb(value, use_bin_type=True, strict_types=True, default=_default)\n"
        "_build_import_rpc_player = [None]\n";
    code += rpcReusableEnvelopePythonSource();
    code +=
        "def _build_import_rpc_blob(blob, tracked=0, _pack_event=_build_import_rpc_pack_event, _net=_pynetmodule, _api=_bi_rpc_api, _os=os, _player=_build_import_rpc_player):\n"
        "  sent = 0\n"
        "  fields = blob.split('\\x00')\n"
        "  if fields and fields[-1] == '':\n"
        "    fields = fields[:-1]\n"
        "  if tracked and (len(fields) & 1):\n"
        "    raise RuntimeError('malformed tracked RPC blob')\n"
        "  requested = len(fields) if not tracked else len(fields) // 2\n"
        "  debug = 'begin requested=' + str(requested)\n"
        "  try:\n"
        "    player_id = _player[0]\n"
        "    if player_id is None or str(player_id) == '' or str(player_id) == '-1':\n"
        "      player_id = _api.GetLocalPlayerId()\n"
        "      _player[0] = player_id\n"
        "    if player_id is None or str(player_id) == '' or str(player_id) == '-1':\n"
        "      raise RuntimeError('local player is not available')\n"
        "    index = 0\n"
        "    while index < len(fields):\n"
        "      cmd = fields[index]\n"
        "      command_uuid = fields[index + 1] if tracked else 'infinitecz_build_silent'\n"
        "      pkt = _pack_event(player_id, cmd, command_uuid)\n"
        "      _net.send2server(98247598, pkt, len(pkt))\n"
        "      sent += 1\n"
        "      index += 2 if tracked else 1\n"
        "    debug = 'ok requested=' + str(requested) + ' sent=' + str(sent) + ' player=' + repr(player_id)\n"
        "  except BaseException as exc:\n"
        "    debug = 'error requested=' + str(requested) + ' sent=' + str(sent) + ' type=' + type(exc).__name__ + ' value=' + repr(exc)\n"
        "  try:\n"
        "    _os.environ['INFINITECZ_BUILD_RPC_SENT'] = str(sent)\n"
        "    _os.environ['INFINITECZ_BUILD_RPC_SEND_DEBUG'] = debug[:2048]\n"
        "  except Exception:\n"
        "    pass\n"
        "  return sent\n"
        "def _build_import_rpc_poll_blob(blob, _api=_bi_rpc_api, _os=os):\n"
        "  uuids = blob.split('\\x00')\n"
        "  if uuids and uuids[-1] == '':\n"
        "    uuids = uuids[:-1]\n"
        "  values = []\n"
        "  debug = 'begin requested=' + str(len(uuids))\n"
        "  try:\n"
        "    system = _api.GetSystem('Minecraft', 'aiCommand')\n"
        "    component = getattr(system, 'Comp', None) if system is not None else None\n"
        "    results = getattr(component, 'mUuid2ExecuteResult', None) if component is not None else None\n"
        "    result_owner = 'component' if results is not None else 'none'\n"
        "    if results is None and system is not None:\n"
        "      results = getattr(system, 'mUuid2ExecuteResult', None)\n"
        "      if results is not None:\n"
        "        result_owner = 'system'\n"
        "    if results is not None:\n"
        "      for command_uuid in uuids:\n"
        "        result = results.get(command_uuid, None)\n"
        "        if result is not None:\n"
        "          values.append(command_uuid + '\\t' + ('1' if bool(result) else '0'))\n"
        "          try:\n"
        "            del results[command_uuid]\n"
        "          except Exception:\n"
        "            pass\n"
        "    try:\n"
        "      result_count = len(results) if results is not None else -1\n"
        "    except BaseException:\n"
        "      result_count = -2\n"
        "    key_sample = []\n"
        "    if results is not None:\n"
        "      try:\n"
        "        for key in list(results.keys())[:5]:\n"
        "          key_sample.append(type(key).__name__ + ':' + repr(key))\n"
        "      except BaseException as sample_exc:\n"
        "        key_sample.append('sample_error:' + repr(sample_exc))\n"
        "    debug = 'ok system=' + ('none' if system is None else type(system).__name__) + ' component=' + ('none' if component is None else type(component).__name__) + ' result_owner=' + result_owner + ' results=' + ('none' if results is None else type(results).__name__) + ' result_count=' + str(result_count) + ' requested=' + repr(uuids[:3]) + ' matched=' + str(len(values)) + ' keys=' + repr(key_sample)\n"
        "  except BaseException as exc:\n"
        "    debug = 'error type=' + type(exc).__name__ + ' value=' + repr(exc) + ' requested=' + repr(uuids[:3])\n"
        "  try:\n"
        "    _os.environ['INFINITECZ_BUILD_RPC_ACKS'] = '\\n'.join(values)\n"
        "    _os.environ['INFINITECZ_BUILD_RPC_POLL_DEBUG'] = debug[:4096]\n"
        "  except Exception:\n"
        "    pass\n"
        "  return len(values)\n"
        "def _build_import_rpc_read_ptr(address, size, _ctypes=_bi_rpc_ctypes):\n"
        "  if _ctypes is None or address <= 0 or size <= 0 or size > 2097152:\n"
        "    raise RuntimeError('RPC pointer payload is unavailable')\n"
        "  return _ctypes.string_at(address, size)\n"
        "def _build_import_rpc_pointer_error(kind, exc, _os=os):\n"
        "  try:\n"
        "    _os.environ['INFINITECZ_BUILD_RPC_POINTER_FAILED'] = '1'\n"
        "    if kind == 'send':\n"
        "      _os.environ['INFINITECZ_BUILD_RPC_SENT'] = '0'\n"
        "      _os.environ['INFINITECZ_BUILD_RPC_SEND_DEBUG'] = ('pointer_error ' + repr(exc))[:2048]\n"
        "    else:\n"
        "      _os.environ['INFINITECZ_BUILD_RPC_ACKS'] = ''\n"
        "      _os.environ['INFINITECZ_BUILD_RPC_POLL_DEBUG'] = ('pointer_error ' + repr(exc))[:4096]\n"
        "  except BaseException:\n"
        "    pass\n"
        "  return 0\n"
        "def _build_import_rpc_ptr(address, size, tracked=0, _read=_build_import_rpc_read_ptr, _send=_build_import_rpc_blob, _failed=_build_import_rpc_pointer_error):\n"
        "  try:\n"
        "    blob = _read(address, size)\n"
        "  except BaseException as exc:\n"
        "    return _failed('send', exc)\n"
        "  return _send(blob, tracked)\n"
        "def _build_import_rpc_poll_ptr(address, size, _read=_build_import_rpc_read_ptr, _poll=_build_import_rpc_poll_blob, _failed=_build_import_rpc_pointer_error):\n"
        "  try:\n"
        "    blob = _read(address, size)\n"
        "  except BaseException as exc:\n"
        "    return _failed('poll', exc)\n"
        "  return _poll(blob)\n"
        "def _build_import_rpc_probe_ptr(address, size, _read=_build_import_rpc_read_ptr, _os=os):\n"
        "  try:\n"
        "    ok = int(_read(address, size) == 'infinitecz_rpc_probe')\n"
        "  except BaseException:\n"
        "    ok = 0\n"
        "  try:\n"
        "    _os.environ['INFINITECZ_BUILD_RPC_POINTER_PROBE'] = str(ok)\n"
        "  except BaseException:\n"
        "    pass\n"
        "  return ok\n"
        "try:\n"
        "  _build_import_rpc_pointer_capable = int(_bi_rpc_ctypes is not None and callable(getattr(_bi_rpc_ctypes, 'string_at', None)))\n"
        "except BaseException:\n"
        "  _build_import_rpc_pointer_capable = 0\n"
        "try:\n"
        "  os.environ['INFINITECZ_BUILD_RPC_POINTER_CAPABLE'] = str(_build_import_rpc_pointer_capable)\n"
        "  os.environ['INFINITECZ_BUILD_RPC_FAST_PACKER'] = str(_build_import_rpc_fast_packer)\n"
        "except BaseException:\n"
        "  pass\n"
        "sys._build_import_rpc_blob = _build_import_rpc_blob\n"
        "sys._build_import_rpc_poll_blob = _build_import_rpc_poll_blob\n"
        "sys._build_import_rpc_ptr = _build_import_rpc_ptr\n"
        "sys._build_import_rpc_poll_ptr = _build_import_rpc_poll_ptr\n"
        "sys._build_import_rpc_probe_ptr = _build_import_rpc_probe_ptr\n"
        "sys._build_import_rpc_version = 16\n";
    rpc_transport_ready_ = PythonUtils::PyExecChecked(code, true);
    const char* raw_pointer_capability = std::getenv(kRpcPointerCapabilityEnvironment);
    const bool pointer_capable = rpc_transport_ready_ && raw_pointer_capability &&
        raw_pointer_capability[0] == '1' && raw_pointer_capability[1] == '\0';
    bool pointer_probe_succeeded = false;
    if (pointer_capable && !rpc_pointer_transport_disabled_) {
        const std::string probe_payload = "infinitecz_rpc_probe";
        const std::string probe_code = makeRpcPointerCall(
            RpcPointerCall::Probe, probe_payload.data(), probe_payload.size());
        setenv(kRpcPointerProbeEnvironment, "0", 1);
        const bool probe_executed = !probe_code.empty() &&
            PythonUtils::PyExecDirectChecked(probe_code);
        const char* raw_probe = std::getenv(kRpcPointerProbeEnvironment);
        pointer_probe_succeeded = probe_executed && raw_probe &&
            raw_probe[0] == '1' && raw_probe[1] == '\0';
        if (!pointer_probe_succeeded) {
            rpc_pointer_transport_disabled_ = true;
            LOGI("[rpc-transport] ctypes pointer probe failed; using literal fallback");
        }
    }
    rpc_pointer_transport_ready_ = pointer_capable && pointer_probe_succeeded &&
        !rpc_pointer_transport_disabled_;
    const char* raw_fast_packer = std::getenv(kRpcFastPackerEnvironment);
    const bool fast_packer = rpc_transport_ready_ && raw_fast_packer &&
        raw_fast_packer[0] == '1' && raw_fast_packer[1] == '\0';
    LOGI("[rpc-transport] install result=%s pointer=%s packer=%s",
         rpc_transport_ready_ ? "success" : "failure",
         rpc_pointer_transport_ready_ ? "ctypes" : "literal-fallback",
         fast_packer ? "cached" : "packb-fallback");
    return rpc_transport_ready_;
}

bool BuildImportRuntime::executeCommand(const std::string& command) {
    return executeCommands(std::vector<std::string>{command});
}

bool BuildImportRuntime::executeCommands(const std::vector<std::string>& commands,
                                          size_t* sent_count) {
    return executeTrackedCommands(commands, {}, sent_count);
}

bool BuildImportRuntime::executeTrackedCommands(
        const std::vector<std::string>& commands,
        const std::vector<std::string>& uuids,
        size_t* sent_count) {
    if (sent_count) *sent_count = 0;
    if (commands.empty()) return true;
    if (!uuids.empty() && uuids.size() != commands.size()) return false;
    if (!in_game_tick_) return false;
    if (!ensureRpcTransport()) {
    invalidateRpcTransport();
        return false;
    }
    setenv(kRpcSentCountEnvironment, "-1", 1);
    setenv(kRpcSendDebugEnvironment, "not-written", 1);
    setenv(kRpcPointerFailureEnvironment, "0", 1);
    size_t payload_size = 0;
    for (size_t index = 0; index < commands.size(); ++index) {
        payload_size += commands[index].size() + 1;
        if (!uuids.empty()) payload_size += uuids[index].size() + 1;
    }
    rpc_payload_buffer_.clear();
    rpc_payload_buffer_.reserve(payload_size);
    for (size_t index = 0; index < commands.size(); ++index) {
        rpc_payload_buffer_ += commands[index];
        rpc_payload_buffer_.push_back('\0');
        if (!uuids.empty()) {
            rpc_payload_buffer_ += uuids[index];
            rpc_payload_buffer_.push_back('\0');
        }
    }
    rpc_code_buffer_.clear();
    if (rpc_pointer_transport_ready_) {
        rpc_code_buffer_ = makeRpcPointerCall(
            uuids.empty() ? RpcPointerCall::SendUntracked : RpcPointerCall::SendTracked,
            rpc_payload_buffer_.data(), rpc_payload_buffer_.size());
    }
    bool pointer_call = !rpc_code_buffer_.empty();
    if (!pointer_call) {
        rpc_code_buffer_ = "import sys\nsys._build_import_rpc_blob(" +
            pythonBytesLiteral(rpc_payload_buffer_) + "," +
            (!uuids.empty() ? "1" : "0") + ")\n";
    }
    const auto python_send_started = std::chrono::steady_clock::now();
    bool executed = PythonUtils::PyExecDirectChecked(rpc_code_buffer_);
    if (pointer_call) {
        const char* raw_pointer_failure = std::getenv(kRpcPointerFailureEnvironment);
        const bool pointer_read_failed = executed && raw_pointer_failure &&
            raw_pointer_failure[0] == '1' && raw_pointer_failure[1] == '\0';
        if (!executed || pointer_read_failed) {
            rpc_pointer_transport_disabled_ = true;
            rpc_pointer_transport_ready_ = false;
        }
        if (pointer_read_failed) {
            // The Python wrapper marks only failures that occur before the
            // payload reaches the send loop, so retrying this batch through
            // the literal bridge cannot duplicate a command prefix.
            setenv(kRpcSentCountEnvironment, "-1", 1);
            setenv(kRpcSendDebugEnvironment, "not-written", 1);
            setenv(kRpcPointerFailureEnvironment, "0", 1);
            rpc_code_buffer_ = "import sys\nsys._build_import_rpc_blob(" +
                pythonBytesLiteral(rpc_payload_buffer_) + "," +
                (!uuids.empty() ? "1" : "0") + ")\n";
            pointer_call = false;
            executed = PythonUtils::PyExecDirectChecked(rpc_code_buffer_);
            LOGI("[rpc-send] ctypes pointer read failed; retried with literal bridge");
        }
    }
    telemetry_python_send_microseconds_ += static_cast<uint64_t>(
        std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - python_send_started).count()));
    ++telemetry_rpc_batches_;
    telemetry_rpc_commands_ += commands.size();
    if (!executed) {
        LOGE("[rpc-send] Python execution failed requested=%zu bridge=%s",
             commands.size(), pointer_call ? "ctypes" : "literal");
    invalidateRpcTransport();
        return false;
    }
    const char* raw_send_debug = std::getenv(kRpcSendDebugEnvironment);
    const std::string send_debug = raw_send_debug ? raw_send_debug : "<missing-debug-env>";
    if (send_debug != last_rpc_send_debug_) {
        last_rpc_send_debug_ = send_debug;
        LOGI("[rpc-send] %s", send_debug.c_str());
    }
    const char* raw_count = std::getenv(kRpcSentCountEnvironment);
    if (!raw_count || *raw_count == '-' || *raw_count == '\0') {
        LOGE("[rpc-send] invalid sent-count value=%s requested=%zu",
             raw_count ? raw_count : "<null>", commands.size());
    invalidateRpcTransport();
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(raw_count, &end, 10);
    if (!end || *end != '\0' || parsed > commands.size()) {
        LOGE("[rpc-send] malformed sent-count value=%s requested=%zu", raw_count, commands.size());
    invalidateRpcTransport();
        return false;
    }
    const size_t issued = static_cast<size_t>(parsed);
    if (sent_count) *sent_count = issued;
    if (issued != commands.size()) {
        LOGE("[rpc-send] partial send issued=%zu requested=%zu", issued, commands.size());
        // A prefix may already be in the reliable RPC stream. Callers account
        // for exactly that prefix, then replay the idempotent partition.
    invalidateRpcTransport();
        return false;
    }
    return true;
}

bool BuildImportRuntime::collectPythonRpcAcks(const std::vector<std::string>& uuids) {
    if (uuids.empty() || !in_game_tick_) return false;
    if (!ensureRpcTransport()) return false;
    setenv(kRpcAckEnvironment, "", 1);
    setenv(kRpcPollDebugEnvironment, "not-written", 1);
    setenv(kRpcPointerFailureEnvironment, "0", 1);
    size_t payload_size = 0;
    for (const std::string& uuid : uuids) {
        payload_size += uuid.size() + 1;
    }
    rpc_payload_buffer_.clear();
    rpc_payload_buffer_.reserve(payload_size);
    for (const std::string& uuid : uuids) {
        rpc_payload_buffer_ += uuid;
        rpc_payload_buffer_.push_back('\0');
    }
    rpc_code_buffer_.clear();
    if (rpc_pointer_transport_ready_) {
        rpc_code_buffer_ = makeRpcPointerCall(
            RpcPointerCall::Poll, rpc_payload_buffer_.data(), rpc_payload_buffer_.size());
    }
    bool pointer_call = !rpc_code_buffer_.empty();
    if (!pointer_call) {
        rpc_code_buffer_ = "import sys\nsys._build_import_rpc_poll_blob(" +
            pythonBytesLiteral(rpc_payload_buffer_) + ")\n";
    }
    bool executed = PythonUtils::PyExecDirectChecked(rpc_code_buffer_);
    if (pointer_call) {
        const char* raw_pointer_failure = std::getenv(kRpcPointerFailureEnvironment);
        const bool pointer_read_failed = executed && raw_pointer_failure &&
            raw_pointer_failure[0] == '1' && raw_pointer_failure[1] == '\0';
        if (!executed || pointer_read_failed) {
            rpc_pointer_transport_disabled_ = true;
            rpc_pointer_transport_ready_ = false;
        }
        if (pointer_read_failed) {
            setenv(kRpcAckEnvironment, "", 1);
            setenv(kRpcPollDebugEnvironment, "not-written", 1);
            setenv(kRpcPointerFailureEnvironment, "0", 1);
            rpc_code_buffer_ = "import sys\nsys._build_import_rpc_poll_blob(" +
                pythonBytesLiteral(rpc_payload_buffer_) + ")\n";
            pointer_call = false;
            executed = PythonUtils::PyExecDirectChecked(rpc_code_buffer_);
            LOGI("[rpc-poll] ctypes pointer read failed; retried with literal bridge");
        }
    }
    if (!executed) {
        LOGE("[rpc-poll] Python execution failed requested=%zu first_uuid=%s bridge=%s",
             uuids.size(), uuids.empty() ? "<none>" : uuids.front().c_str(),
             pointer_call ? "ctypes" : "literal");
    invalidateRpcTransport();
        return false;
    }
    const char* raw_poll_debug = std::getenv(kRpcPollDebugEnvironment);
    const std::string poll_debug = raw_poll_debug ? raw_poll_debug : "<missing-debug-env>";
    if (poll_debug != last_rpc_poll_debug_) {
        last_rpc_poll_debug_ = poll_debug;
        LOGI("[rpc-poll] %s", poll_debug.c_str());
    }
    const char* raw = std::getenv(kRpcAckEnvironment);
    if (!raw || !*raw) return true;
    const size_t raw_size = std::strlen(raw);
    if (raw_size > 64U * 1024U) return false;

    const std::string snapshot(raw, raw_size);
    std::map<std::string, bool> decoded;
    size_t cursor = 0;
    while (cursor < snapshot.size()) {
        const size_t line_end = snapshot.find('\n', cursor);
        const size_t end = line_end == std::string::npos ? snapshot.size() : line_end;
        const size_t separator = snapshot.find('\t', cursor);
        if (separator == std::string::npos || separator >= end ||
            end - separator != 2U) {
            return false;
        }
        const std::string uuid = snapshot.substr(cursor, separator - cursor);
        if (uuid.empty() || uuid.size() > 128U ||
            std::find(uuids.begin(), uuids.end(), uuid) == uuids.end() ||
            (snapshot[separator + 1U] != '0' && snapshot[separator + 1U] != '1')) {
            return false;
        }
        decoded[uuid] = snapshot[separator + 1U] == '1';
        cursor = line_end == std::string::npos ? snapshot.size() : line_end + 1U;
    }
    if (!decoded.empty()) {
        std::lock_guard<std::mutex> lock(native_event_mutex_);
        for (const auto& ack : decoded) {
            received_rpc_acks_[ack.first] = ack.second;
            LOGI("[rpc-ack] source=python uuid=%s accepted=%d",
                 ack.first.c_str(), ack.second ? 1 : 0);
        }
    }
    return true;
}

BuildImportRuntime::RpcResultState BuildImportRuntime::pollTrackedCommands(
        const std::vector<std::string>& uuids, bool* any_rejected,
        std::string* matched_uuid) {
    if (any_rejected) *any_rejected = false;
    if (matched_uuid) matched_uuid->clear();
    if (uuids.empty()) return RpcResultState::Unavailable;
    bool missing_ack = false;
    {
        std::lock_guard<std::mutex> lock(native_event_mutex_);
        for (const std::string& uuid : uuids) {
            if (received_rpc_acks_.find(uuid) == received_rpc_acks_.end()) {
                missing_ack = true;
                break;
            }
        }
    }
    // Once the receive hook has proven that importer ACK packets reach the
    // native decoder, polling the same Python dictionary every 40 ms only
    // adds a Python transition on the game thread. Keep the polling fallback
    // for client builds where no native importer ACK has ever been observed.
    if (missing_ack && !native_rpc_ack_observed_.load(std::memory_order_acquire)) {
        (void)collectPythonRpcAcks(uuids);
    }
    std::lock_guard<std::mutex> lock(native_event_mutex_);
    bool rejected = false;
    bool pending = false;
    for (const std::string& uuid : uuids) {
        const auto found = received_rpc_acks_.find(uuid);
        if (found == received_rpc_acks_.end()) pending = true;
        else if (!found->second) rejected = true;
    }
    // One failed probe already proves the whole batch is not ready. Do not
    // wait for every remaining ACK until the timeout before retrying it.
    if (pending && !rejected) return RpcResultState::Pending;
    if (matched_uuid && uuids.size() == 1U) {
        const auto found = received_rpc_acks_.find(uuids.front());
        if (found != received_rpc_acks_.end()) *matched_uuid = found->first;
    }
    for (const std::string& uuid : uuids) received_rpc_acks_.erase(uuid);
    if (any_rejected) *any_rejected = rejected;
    const RpcResultState state = rejected ? RpcResultState::Rejected : RpcResultState::Accepted;
    LOGI("[rpc-result] state=%s tracked=%zu first_uuid=%s",
         rejected ? "rejected" : "accepted", uuids.size(), uuids.front().c_str());
    return state;
}

std::string BuildImportRuntime::nextRpcUuid() {
    const uint64_t clock_value = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    return "infinitecz_build_" + std::to_string(clock_value) + "_" +
        std::to_string(next_rpc_uuid_++);
}

std::string BuildImportRuntime::commandFor(const PlannedCommand& command) {
    const CanonicalBlock block = placementBlock(command.name, command.aux);
    const std::string suffix = " " + formatPlacementBlockArgument(block.name, block.aux);
    if (command.block_count > 1) {
        return "/fill " + std::to_string(command.bounds.min_x) + " " +
            std::to_string(command.bounds.min_y) + " " + std::to_string(command.bounds.min_z) + " " +
            std::to_string(command.bounds.max_x) + " " + std::to_string(command.bounds.max_y) + " " +
            std::to_string(command.bounds.max_z) + suffix;
    }
    return "/setblock " + std::to_string(command.bounds.min_x) + " " +
        std::to_string(command.bounds.min_y) + " " + std::to_string(command.bounds.min_z) + suffix;
}

std::string BuildImportRuntime::verificationProbeCommandFor(
        const VerificationPlanSample& sample) {
    const CanonicalBlock block = placementBlock(sample.name, sample.aux);
    std::string command = "/testforblock " + std::to_string(sample.x) + " " +
        std::to_string(sample.y) + " " + std::to_string(sample.z) + " " +
        formatVerificationBlockArgument(
            block.name, block.aux,
            hasVerificationSampleFlag(sample, VerificationSampleFlag::IgnoreAux));
    return command;
}

void BuildImportRuntime::resetVerificationServerProbe() {
    const std::string uuid = std::move(verification_server_probe_uuid_);
    verification_server_probe_sample_index_.reset();
    verification_server_probe_poll_at_ = {};
    verification_server_probe_deadline_ = {};
    if (!uuid.empty()) {
        std::lock_guard<std::mutex> lock(native_event_mutex_);
        received_rpc_acks_.erase(uuid);
    }
}

void BuildImportRuntime::beginVerificationServerProbe(
        size_t sample_index, std::chrono::steady_clock::time_point now) {
    if (sample_index >= verification_samples_.size()) {
        finishFinalVerification(false, "verification server probe is outside the sample plan");
        return;
    }
    resetVerificationServerProbe();
    verification_server_probe_sample_index_ = sample_index;
    verification_server_probe_uuid_ = nextRpcUuid();
    const std::string command =
        verificationProbeCommandFor(verification_samples_[sample_index]);
    size_t sent_count = 0;
    if (!executeTrackedCommands(
            {command}, {verification_server_probe_uuid_}, &sent_count) ||
        sent_count != 1U) {
        LOGE("[verification] server probe dispatch failed sample=%zu", sample_index);
        resetVerificationServerProbe();
        scheduleChunkRepair(verification_samples_[sample_index].chunk,
                            sample_index, now);
        return;
    }
    verification_server_probe_poll_at_ = now + kRpcPollInterval;
    verification_server_probe_deadline_ = now + kRpcProbeTimeout;
    LOGI("[verification] native mismatch; server probe sample=%zu uuid=%s command=%s",
         sample_index, verification_server_probe_uuid_.c_str(), command.c_str());
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = "client verification differed; confirming the sampled block on the server";
}

bool BuildImportRuntime::serviceVerificationServerProbe(
        std::chrono::steady_clock::time_point now) {
    if (!verification_server_probe_sample_index_) return false;
    const size_t sample_index = *verification_server_probe_sample_index_;
    if (sample_index >= verification_samples_.size() ||
        sample_index != verification_sample_index_) {
        resetVerificationServerProbe();
        finishFinalVerification(false, "verification server probe cursor changed unexpectedly");
        return true;
    }
    if (now < verification_server_probe_poll_at_) return true;
    const RpcResultState state =
        pollTrackedCommands({verification_server_probe_uuid_});
    if (state == RpcResultState::Pending &&
        now < verification_server_probe_deadline_) {
        verification_server_probe_poll_at_ = now + kRpcPollInterval;
        return true;
    }

    const VerificationPlanSample& sample = verification_samples_[sample_index];
    if (state == RpcResultState::Accepted) {
        NativeBlockInfo actual;
        const bool readable =
            NativeWorldAccess::getBlock(sample.x, sample.y, sample.z, &actual);
        LOGI("[verification] server confirmed sample=%zu pos=(%d,%d,%d) "
             "expected=%s:%u native=%s:%u",
             sample_index, sample.x, sample.y, sample.z, sample.name.c_str(),
             static_cast<unsigned int>(sample.aux),
             readable ? actual.name.c_str() : "<unreadable>",
             readable ? static_cast<unsigned int>(actual.aux) : 0U);
        resetVerificationServerProbe();
        verification_mismatch_sample_index_.reset();
        verification_mismatch_observations_ = 0;
        verification_repair_tracker_.markSamplePassed(sample_index);
        ++verification_sample_index_;
        controller_.recordVerificationProgress(
            verification_sample_index_, false, nullptr);
        verification_sample_ready_at_ = now + std::chrono::milliseconds(1);
        return true;
    }

    LOGI("[verification] server probe did not confirm sample=%zu state=%d; "
         "scheduling repair",
         sample_index, static_cast<int>(state));
    resetVerificationServerProbe();
    scheduleChunkRepair(sample.chunk, sample_index, now);
    return true;
}

void BuildImportRuntime::beginFinalVerification() {
    if (controller_.state() != ImportState::Verifying || final_verification_started_) return;
    final_verification_started_ = true;
    verification_pending_ = true;
    verification_chunk_plans_.clear();
    verification_samples_.clear();
    verification_sample_index_ = 0;
    verification_unverifiable_chunks_ = 0;
    verification_verified_chunks_ = 0;
    verification_region_coord_.reset();
    verification_readable_chunk_.reset();
    verification_region_load_retries_ = 0;
    verification_mismatch_sample_index_.reset();
    verification_mismatch_observations_ = 0;
    resetVerificationServerProbe();
    verification_repair_chunk_.reset();
    verification_repair_tracker_.reset();
    verification_repair_reader_.reset();
    verification_repair_reader_chunk_filter_.clear();
    stage_ = ExecuteStage::Verify;
    const BuildImportRuntimeMetadata metadata = controller_.runtimeMetadata();
    if (!metadata.verify_after_import) {
        LOGI("[verification] skipped by import configuration");
        finishFinalVerification(true);
        return;
    }
    std::string error;
    if (!CommandSpoolBuilder::loadVerificationPlan(
            spool_directory_ + "/" + CommandSpoolBuilder::kVerificationPlanName,
            &verification_chunk_plans_, &error)) {
        finishFinalVerification(false, error);
        return;
    }
    const int32_t span = metadata.region_span;
    const ChunkCoord region_grid_origin = metadata.region_grid_origin;
    const bool serpentine_regions = region_command_spools_;
    std::sort(verification_chunk_plans_.begin(), verification_chunk_plans_.end(),
        [span, region_grid_origin, serpentine_regions](const VerificationChunkPlan& left,
                                                       const VerificationChunkPlan& right) {
            if (!serpentine_regions) return left.coord < right.coord;
            const ChunkCoord left_region = regionForChunk(left.coord, span, region_grid_origin);
            const ChunkCoord right_region = regionForChunk(right.coord, span, region_grid_origin);
            if (left_region.z != right_region.z) return left_region.z < right_region.z;
            if (left_region.x != right_region.x) {
                const bool reverse_row =
                    (static_cast<uint32_t>(left_region.z) & 1U) != 0;
                return reverse_row ? left_region.x > right_region.x
                                   : left_region.x < right_region.x;
            }
            if (left.coord.z != right.coord.z) return left.coord.z < right.coord.z;
            return left.coord.x < right.coord.x;
        });
    uint64_t planned_blocks = 0;
    uint64_t planned_samples = 0;
    for (VerificationChunkPlan& chunk : verification_chunk_plans_) {
        chunk.samples = selectVerificationSamples(
            chunk.samples, metadata.verification_precision);
        if (std::numeric_limits<uint64_t>::max() - planned_blocks < chunk.total_block_count ||
            std::numeric_limits<uint64_t>::max() - planned_samples < chunk.samples.size()) {
            finishFinalVerification(false, "verification plan counters overflow");
            return;
        }
        planned_blocks += chunk.total_block_count;
        planned_samples += chunk.samples.size();
        if (chunk.samples.empty()) ++verification_unverifiable_chunks_;
        else ++verification_verified_chunks_;
        for (const VerificationPlanSample& sample : chunk.samples) {
            verification_samples_.push_back(sample);
        }
    }
    if (planned_blocks != total_block_count_) {
        finishFinalVerification(false, "verification plan does not match imported block total");
        return;
    }
    if (metadata.verification_sample_index > verification_samples_.size()) {
        finishFinalVerification(false, "verification checkpoint cursor exceeds its sample plan");
        return;
    }
    verification_sample_index_ = static_cast<size_t>(metadata.verification_sample_index);
    verification_sample_ready_at_ = std::max(
        std::chrono::steady_clock::now() + std::chrono::milliseconds(200), region_add_ready_at_);
    resetPlacementBatch();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "verifying " + std::to_string(verification_verified_chunks_) +
            " chunks in C++ (" + std::to_string(planned_samples) + " samples; " +
            std::to_string(verification_unverifiable_chunks_) + " without stable samples)";
    }
    if (verification_samples_.empty()) finishFinalVerification(true);
}

void BuildImportRuntime::tickFinalVerification(std::chrono::steady_clock::time_point now) {
    if (!verification_pending_ || now < verification_sample_ready_at_) return;
    if (verification_repair_chunk_) {
        tickChunkRepair(now);
        return;
    }
    if (serviceVerificationServerProbe(now)) return;
    if (verification_sample_index_ >= verification_samples_.size()) {
        finishFinalVerification(true);
        return;
    }
    const VerificationPlanSample& first = verification_samples_[verification_sample_index_];
    const BuildImportRuntimeMetadata metadata = controller_.runtimeMetadata();
    const int32_t span = metadata.region_span;
    const ChunkCoord region_grid_origin = metadata.region_grid_origin;
    const ChunkCoord target_region = regionForChunk(first.chunk, span, region_grid_origin);
    if (!loaded_region_ || !sameChunk(*loaded_region_, target_region)) {
        if (loaded_region_) {
            std::string checkpoint_error;
            if (!controller_.recordVerificationProgress(
                    verification_sample_index_, true, &checkpoint_error)) {
                pauseFinalVerification("cannot persist verification cursor: " + checkpoint_error);
                return;
            }
            releaseLoadedRegion(true);
            verification_sample_ready_at_ = region_add_ready_at_;
            return;
        }
        if (now < region_add_ready_at_) {
            verification_sample_ready_at_ = region_add_ready_at_;
            return;
        }
        beginVerificationRegion(first, now);
        return;
    }
    if (!loaded_region_fully_confirmed_) {
        if (now >= chunk_load_probe_deadline_) {
    invalidateRpcTransport();
            if (++verification_region_load_retries_ > 4) {
                pauseFinalVerification("verification region did not load after retries");
                return;
            }
            releaseLoadedRegion(true);
            verification_readable_chunk_.reset();
            verification_sample_ready_at_ = region_add_ready_at_;
            return;
        }
        bool any_level_chunk = false;
        const bool complete_level_chunk = levelChunkEvidenceForBounds(
            loaded_region_probe_bounds_, &any_level_chunk);
        const bool early_probe_ready =
            chunk_early_probe_at_.time_since_epoch().count() &&
            now >= chunk_early_probe_at_ && complete_level_chunk &&
            nativeChunksReadable(loaded_region_probe_bounds_);
        if (now >= chunk_ready_at_ || early_probe_ready) {
            std::string probe_error;
            const ChunkProbeState server_state = pollServerChunkProbe(
                loaded_region_probe_bounds_, now, &probe_error);
            const bool native_fallback_ready =
                server_chunk_probe_ack_timed_out_ &&
                now >= chunk_ready_at_ + kNativeReadabilityFallbackGrace;
            const bool load_proven = server_state == ChunkProbeState::Ready ||
                (server_state == ChunkProbeState::TransportFailure &&
                 (complete_level_chunk || native_fallback_ready));
            chunk_required_readable_samples_ =
                server_state == ChunkProbeState::Ready ? 1U :
                    (complete_level_chunk ? 2U : 3U);
            if (load_proven && nativeChunksReadable(loaded_region_probe_bounds_)) {
                if (++chunk_readable_samples_ >= chunk_required_readable_samples_) {
                    chunk_loaded_ = true;
                    loaded_region_fully_confirmed_ = true;
                    if (native_fallback_ready &&
                        server_state != ChunkProbeState::Ready) {
                        LOGI("[chunk-probe] verification region native-readable "
                             "without RPC ACK samples=%u",
                             chunk_readable_samples_);
                    }
                }
            } else {
                chunk_readable_samples_ = 0;
            }
        }
        verification_sample_ready_at_ = now + std::chrono::milliseconds(50);
        if (!loaded_region_fully_confirmed_) return;
    }

    const auto deadline = now + std::chrono::milliseconds(2);
    uint32_t checked = 0;
    while (verification_sample_index_ < verification_samples_.size() && checked < 32 &&
           std::chrono::steady_clock::now() < deadline) {
        const VerificationPlanSample& sample = verification_samples_[verification_sample_index_];
        if (!sameChunk(regionForChunk(sample.chunk, span, region_grid_origin), target_region)) {
            return;
        }
        NativeBlockInfo actual;
        const bool readable = NativeWorldAccess::getBlock(sample.x, sample.y, sample.z, &actual);
        if (!readable || !verificationBlocksEqual(actual, sample)) {
            if (!verification_mismatch_sample_index_ ||
                *verification_mismatch_sample_index_ != verification_sample_index_) {
                verification_mismatch_sample_index_ = verification_sample_index_;
                verification_mismatch_observations_ = 1;
            } else {
                ++verification_mismatch_observations_;
            }
            if (verification_mismatch_observations_ < 3) {
                verification_sample_ready_at_ = now + std::chrono::milliseconds(175);
                return;
            }
            verification_mismatch_sample_index_.reset();
            verification_mismatch_observations_ = 0;
            if (!readable) {
                if (++verification_region_load_retries_ > 4) {
                    pauseFinalVerification("verification block remained unreadable after reloads");
                    return;
                }
                releaseLoadedRegion(true);
                verification_readable_chunk_.reset();
                verification_sample_ready_at_ = region_add_ready_at_;
                return;
            }
            beginVerificationServerProbe(verification_sample_index_, now);
            return;
        }
        verification_mismatch_sample_index_.reset();
        verification_mismatch_observations_ = 0;
        verification_repair_tracker_.markSamplePassed(
            verification_sample_index_);
        ++verification_sample_index_;
        ++checked;
    }
    controller_.recordVerificationProgress(verification_sample_index_, false, nullptr);
    verification_sample_ready_at_ = now + std::chrono::milliseconds(1);
}

void BuildImportRuntime::beginVerificationRegion(const VerificationPlanSample& sample,
                                                 std::chrono::steady_clock::time_point now) {
    const BuildImportRuntimeMetadata metadata = controller_.runtimeMetadata();
    const int32_t chunk_size = std::max<int32_t>(1, metadata.chunk_size);
    const int32_t span = metadata.region_span;
    const ChunkCoord region_grid_origin = metadata.region_grid_origin;
    const ChunkCoord region = regionForChunk(sample.chunk, span, region_grid_origin);
    if (!verification_region_coord_ || !sameChunk(*verification_region_coord_, region)) {
        verification_region_coord_ = region;
        verification_region_load_retries_ = 0;
    }
    const int64_t region_min_x = regionMinChunkCoordinate(
        region.x, span, region_grid_origin.x) * chunk_size;
    const int64_t region_min_z = regionMinChunkCoordinate(
        region.z, span, region_grid_origin.z) * chunk_size;
    const int64_t min_x = region_min_x - chunk_size;
    const int64_t min_z = region_min_z - chunk_size;
    verification_load_bounds_ = {
        clampToInt32(min_x), metadata.ticking_area_min_y, clampToInt32(min_z),
        clampToInt32(region_min_x + static_cast<int64_t>(span + 1) * chunk_size - 1),
        metadata.ticking_area_max_y,
        clampToInt32(region_min_z + static_cast<int64_t>(span + 1) * chunk_size - 1),
    };
    loaded_region_probe_bounds_ = {
        clampToInt32(region_min_x), metadata.ticking_area_min_y,
        clampToInt32(region_min_z),
        clampToInt32(region_min_x + static_cast<int64_t>(span) * chunk_size - 1),
        metadata.ticking_area_max_y,
        clampToInt32(region_min_z + static_cast<int64_t>(span) * chunk_size - 1),
    };
    const int64_t probe_native_width =
        static_cast<int64_t>(floorDiv(loaded_region_probe_bounds_.max_x, kNativeChunkSize)) -
        floorDiv(loaded_region_probe_bounds_.min_x, kNativeChunkSize) + 1;
    const int64_t probe_native_depth =
        static_cast<int64_t>(floorDiv(loaded_region_probe_bounds_.max_z, kNativeChunkSize)) -
        floorDiv(loaded_region_probe_bounds_.min_z, kNativeChunkSize) + 1;
    if (probe_native_width <= 0 || probe_native_depth <= 0 ||
        probe_native_width * probe_native_depth > 256) {
        // The tracked probe protocol deliberately caps one batch at 256
        // native chunks. Unusually large custom regions prove the first
        // logical chunk here; direct sample reads still detect and reload any
        // later chunk that is not actually readable.
        const int64_t logical_min_x = static_cast<int64_t>(sample.chunk.x) * chunk_size;
        const int64_t logical_min_z = static_cast<int64_t>(sample.chunk.z) * chunk_size;
        loaded_region_probe_bounds_ = {
            clampToInt32(logical_min_x), metadata.ticking_area_min_y,
            clampToInt32(logical_min_z),
            clampToInt32(logical_min_x + chunk_size - 1),
            metadata.ticking_area_max_y,
            clampToInt32(logical_min_z + chunk_size - 1),
        };
    }
    WorkUnit unit;
    unit.coord = sample.chunk;
    unit.region_coord = region;
    unit.region_grid_origin = region_grid_origin;
    unit.region_span = span;
    unit.imported_bounds = {sample.x, sample.y, sample.z, sample.x, sample.y, sample.z};
    unit.load_bounds = verification_load_bounds_;
    unit.wait_ticks = metadata.chunk_wait_ticks;
    loaded_region_ = region;
    loaded_region_started_at_ = now;
    confirmed_region_chunks_.clear();
    loaded_region_fully_confirmed_ = false;
    loaded_region_cleanup_command_ = controller_.makeCleanupCommand(unit);
    int32_t player_x = 0;
    int32_t player_y = 0;
    int32_t player_z = 0;
    const bool has_player_position =
        NativeWorldAccess::getLocalPlayerBlockPosition(&player_x, &player_y,
                                                       &player_z);
    std::vector<std::string> verification_prepare = controller_.makePrepareCommands(
        unit, has_player_position, player_x, player_z);
    // A held (settling) removal of this area name must run before the add so
    // the verification region is never deleted right after it loads.
    if (const std::optional<std::string> stale =
            takeHeldRegionCleanup(loaded_region_cleanup_command_)) {
        verification_prepare.insert(verification_prepare.begin(), *stale);
    }
    if (!executeCommands(verification_prepare)) {
        releaseLoadedRegion(true);
        verification_sample_ready_at_ = now + std::chrono::seconds(1);
        if (++verification_region_load_retries_ > 4) {
            pauseFinalVerification("Python RPC transport unavailable");
        }
        return;
    }
    const auto prepared_at = std::chrono::steady_clock::now();
    loaded_region_started_at_ = prepared_at;
    verification_readable_chunk_.reset();
    chunk_loaded_ = false;
    chunk_readable_samples_ = 0;
    chunk_required_readable_samples_ = 3;
    resetServerChunkProbe();
    chunk_early_probe_at_ = prepared_at + std::chrono::milliseconds(100);
    chunk_ready_at_ = prepared_at + std::chrono::milliseconds(350);
    chunk_load_probe_deadline_ = prepared_at + std::chrono::seconds(10);
    verification_sample_ready_at_ = chunk_early_probe_at_;
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = "loading verification region " + std::to_string(region.x) + "," +
        std::to_string(region.z);
}

void BuildImportRuntime::scheduleChunkRepair(const ChunkCoord& chunk, size_t sample_index,
                                             std::chrono::steady_clock::time_point now) {
    if (sample_index >= verification_samples_.size()) {
        finishFinalVerification(false, "verification repair sample is outside the plan");
        return;
    }
    const BuildImportRuntimeMetadata metadata = controller_.runtimeMetadata();
    const bool overwrite =
        metadata.overwrite_policy == OverwritePolicy::ClearImportedBounds;
    const int32_t span = metadata.region_span;
    const bool region_spools = region_command_spools_;
    const bool region_scoped = overwrite || region_spools;
    const ChunkCoord region_grid_origin = adaptive_region_grid_
        ? metadata.region_grid_origin : ChunkCoord{};
    const ChunkCoord scope = region_scoped
        ? regionForChunk(chunk, span, region_grid_origin) : chunk;
    if (!verification_repair_tracker_.canAttempt(sample_index, 2)) {
        const VerificationPlanSample& sample = verification_samples_[sample_index];
        NativeBlockInfo actual;
        const bool readable = NativeWorldAccess::getBlock(sample.x, sample.y, sample.z, &actual);
        std::string detail = "chunk " + std::to_string(chunk.x) + "," +
            std::to_string(chunk.z) + " still mismatches at " + std::to_string(sample.x) + "," +
            std::to_string(sample.y) + "," + std::to_string(sample.z) +
            " expected " + sample.name;
        if (readable) detail += " got " + actual.name + ":" + std::to_string(actual.aux);
        pauseFinalVerification(detail);
        return;
    }
    const uint32_t repair_attempt =
        verification_repair_tracker_.beginAttempt(sample_index);
    verification_repair_chunk_ = chunk;
    verification_repair_chunks_.clear();
    verification_repair_halo_chunks_.clear();
    int32_t min_chunk_x = chunk.x;
    int32_t max_chunk_x = chunk.x;
    int32_t min_chunk_z = chunk.z;
    int32_t max_chunk_z = chunk.z;
    for (const VerificationChunkPlan& plan : verification_chunk_plans_) {
        const bool in_scope = region_scoped
            ? sameChunk(regionForChunk(plan.coord, span, region_grid_origin), scope)
            : sameChunk(plan.coord, chunk);
        if (!in_scope) continue;
        verification_repair_chunks_.push_back(plan.coord);
        min_chunk_x = std::min(min_chunk_x, plan.coord.x);
        max_chunk_x = std::max(max_chunk_x, plan.coord.x);
        min_chunk_z = std::min(min_chunk_z, plan.coord.z);
        max_chunk_z = std::max(max_chunk_z, plan.coord.z);
    }
    if (verification_repair_chunks_.empty()) {
        finishFinalVerification(false, "verification repair scope has no chunk metadata");
        return;
    }
    if (overwrite) {
        for (const VerificationChunkPlan& plan : verification_chunk_plans_) {
            if (std::find(verification_repair_chunks_.begin(), verification_repair_chunks_.end(),
                          plan.coord) != verification_repair_chunks_.end()) continue;
            const int64_t x = plan.coord.x;
            const int64_t z = plan.coord.z;
            if (x >= static_cast<int64_t>(min_chunk_x) - 1 &&
                x <= static_cast<int64_t>(max_chunk_x) + 1 &&
                z >= static_cast<int64_t>(min_chunk_z) - 1 &&
                z <= static_cast<int64_t>(max_chunk_z) + 1) {
                verification_repair_halo_chunks_.push_back(plan.coord);
            }
        }
    }
    const auto sampleInScope = [&](const VerificationPlanSample& sample) {
        return region_scoped
            ? sameChunk(regionForChunk(sample.chunk, span, region_grid_origin), scope)
            : sameChunk(sample.chunk, chunk);
    };
    verification_repair_sample_begin_ = sample_index;
    while (verification_repair_sample_begin_ > 0 &&
           sampleInScope(verification_samples_[verification_repair_sample_begin_ - 1])) {
        --verification_repair_sample_begin_;
    }
    std::string checkpoint_error;
    if (!controller_.recordVerificationProgress(
            verification_repair_sample_begin_, true, &checkpoint_error)) {
        pauseFinalVerification("cannot persist repair checkpoint: " + checkpoint_error);
        return;
    }
    verification_repair_chunk_cursor_ = 0;
    verification_repair_replayed_paths_.clear();
    verification_repair_clear_plan_.clear();
    verification_repair_clear_index_ = 0;
    verification_repair_probe_cursor_ = 0;
    verification_repair_probe_samples_ = 0;
    verification_repair_probe_ready_at_ = now + std::chrono::milliseconds(100);
    verification_repair_probe_deadline_ = now + std::chrono::seconds(10);
    verification_repair_emitted_blocks_ = 0;
    verification_repair_transport_failures_ = 0;
    verification_repair_reader_.reset();
    verification_repair_reader_chunk_filter_.clear();
    verification_repair_force_drain_ = false;
    verification_repair_phase_dirty_ = false;
    verification_repair_settle_ready_at_ = {};
    unbarriered_command_count_ = 0;
    unbarriered_block_count_ = 0;
    data_window_started_at_ = {};
    resetDrainBarrier();
    active_unit_last_send_at_ = {};
    std::vector<PlannedCommand> repair_foundation_plan;
    std::set<ChunkCoord> repair_foundation_regions;
    for (const VerificationChunkPlan& plan : verification_chunk_plans_) {
        if (std::find(verification_repair_chunks_.begin(), verification_repair_chunks_.end(),
                      plan.coord) == verification_repair_chunks_.end()) continue;
        WorkUnit repair_unit;
        repair_unit.coord = plan.coord;
        repair_unit.region_coord = regionForChunk(plan.coord, span, region_grid_origin);
        repair_unit.region_grid_origin = region_grid_origin;
        repair_unit.region_span = span;
        repair_unit.imported_bounds = plan.imported_bounds;
        if (overwrite) {
            WorkUnit clear_unit = repair_unit;
            clear_unit.phase = ImportPhase::Clear;
            std::vector<PlannedCommand> clear_plan = controller_.makeClearPlan(clear_unit);
            verification_repair_clear_plan_.insert(
                verification_repair_clear_plan_.end(),
                std::make_move_iterator(clear_plan.begin()),
                std::make_move_iterator(clear_plan.end()));
        }
        // Repair replays source command spools after this auxiliary plan. In
        // preserve mode there is no air-clear plan, but the foundation still
        // needs to be restored before the source blocks are replayed.
        // The regular scheduler owns one foundation slice per aggregated work
        // region. Verification remains per logical chunk, so deduplicate here
        // before regenerating that same region-wide slice during a repair.
        if (repair_foundation_regions.insert(repair_unit.region_coord).second) {
            repair_unit.place_deny_foundation = true;
            std::vector<PlannedCommand> foundation_plan =
                controller_.makeDenyFoundationPlan(repair_unit);
            repair_foundation_plan.insert(
                repair_foundation_plan.end(),
                std::make_move_iterator(foundation_plan.begin()),
                std::make_move_iterator(foundation_plan.end()));
        }
    }
    verification_repair_clear_plan_.insert(
        verification_repair_clear_plan_.end(),
        std::make_move_iterator(repair_foundation_plan.begin()),
        std::make_move_iterator(repair_foundation_plan.end()));
    verification_repair_phase_ = phaseIndex(
        verification_repair_clear_plan_.empty() ? ImportPhase::Structure : ImportPhase::Clear);
    resetPlacementBatch();
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = overwrite
        ? "sample mismatch; rebuilding complete loaded region (" +
            std::to_string(repair_attempt) + "/2)"
        : region_spools
            ? "sample mismatch; replaying complete loaded region (" +
                std::to_string(repair_attempt) + "/2)"
            : "sample mismatch; replaying complete chunk " + std::to_string(chunk.x) + "," +
                std::to_string(chunk.z) + " (" +
                std::to_string(repair_attempt) + "/2)";
}

void BuildImportRuntime::tickChunkRepair(std::chrono::steady_clock::time_point now) {
    if (!verification_repair_chunk_) return;
    const BuildImportRuntimeMetadata metadata = controller_.runtimeMetadata();
    const bool region_spools = region_command_spools_;
    const int32_t region_span = metadata.region_span;
    const ChunkCoord region_grid_origin = adaptive_region_grid_
        ? metadata.region_grid_origin : ChunkCoord{};
    const auto restartForTransport = [&]() {
        resetDrainBarrier();
        verification_repair_force_drain_ =
            active_unit_last_send_at_.time_since_epoch().count() != 0;
    invalidateRpcTransport();
        verification_repair_reader_.reset();
        verification_repair_reader_chunk_filter_.clear();
        verification_repair_phase_ = phaseIndex(
            verification_repair_clear_plan_.empty()
                ? ImportPhase::Structure : ImportPhase::Clear);
        verification_repair_chunk_cursor_ = 0;
        verification_repair_replayed_paths_.clear();
        verification_repair_clear_index_ = 0;
        verification_repair_emitted_blocks_ = 0;
        verification_sample_ready_at_ = now + std::chrono::seconds(1);
        if (++verification_repair_transport_failures_ > 4) {
            pauseFinalVerification("Python RPC transport failed during chunk repair");
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "RPC transport failed; restarting the current repair pass";
    };
    const auto repairWindowReady = [&]() {
        const bool drain_in_progress = !drain_barrier_uuids_.empty() ||
            drain_barrier_confirmed_;
        if (!verification_repair_force_drain_ && !drain_in_progress &&
            !dataWindowNeedsDrain()) return true;
        preparePlacementBatch();
        if (!dataCommandsDrained(now, false)) {
            if (!takeDrainFailure().empty()) restartForTransport();
            return false;
        }
        verification_repair_force_drain_ = false;
        active_unit_last_send_at_ = {};
        resetDrainBarrier();
        return true;
    };

    const size_t repair_probe_count = verification_repair_chunks_.size() +
        verification_repair_halo_chunks_.size();
    if (verification_repair_probe_cursor_ < repair_probe_count) {
        if (now < verification_repair_probe_ready_at_) return;
        const ChunkCoord& probe_chunk =
            verification_repair_probe_cursor_ < verification_repair_chunks_.size()
                ? verification_repair_chunks_[verification_repair_probe_cursor_]
                : verification_repair_halo_chunks_[
                    verification_repair_probe_cursor_ - verification_repair_chunks_.size()];
        const auto metadata = std::find_if(
            verification_chunk_plans_.begin(), verification_chunk_plans_.end(),
            [&](const VerificationChunkPlan& plan) { return plan.coord == probe_chunk; });
        if (metadata == verification_chunk_plans_.end()) {
            finishFinalVerification(false, "repair probe has no chunk metadata");
            return;
        }
        bool any_level_chunk = false;
        const bool complete_level_chunk =
            levelChunkEvidenceForBounds(metadata->imported_bounds, &any_level_chunk);
        std::string probe_error;
        const ChunkProbeState server_state = pollServerChunkProbe(
            metadata->imported_bounds, now, &probe_error);
        const bool load_proven = server_state == ChunkProbeState::Ready ||
            (server_state == ChunkProbeState::TransportFailure && complete_level_chunk);
        const uint32_t required_samples =
            server_state == ChunkProbeState::Ready ? 2U : 3U;
        if (load_proven && nativeChunksReadable(metadata->imported_bounds)) {
            ++verification_repair_probe_samples_;
            if (verification_repair_probe_samples_ >= required_samples) {
                ++verification_repair_probe_cursor_;
                verification_repair_probe_samples_ = 0;
                resetServerChunkProbe();
                verification_repair_probe_deadline_ = now + std::chrono::seconds(10);
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = "repair load check " +
                    std::to_string(verification_repair_probe_cursor_) + "/" +
                    std::to_string(repair_probe_count);
            }
        } else {
            verification_repair_probe_samples_ = 0;
        }
        if (now >= verification_repair_probe_deadline_) {
    invalidateRpcTransport();
            pauseFinalVerification(
                "repair scope chunk " + std::to_string(probe_chunk.x) + "," +
                std::to_string(probe_chunk.z) + " did not load");
            return;
        }
        verification_repair_probe_ready_at_ = now + std::chrono::milliseconds(50);
        return;
    }

    // Repair can span a complete loaded region and every placement phase. Use
    // the same bounded server queue window as the normal Clear/Build path.
    if (!repairWindowReady()) return;

    if (verification_repair_phase_ == phaseIndex(ImportPhase::Clear)) {
        if (verification_repair_clear_index_ < verification_repair_clear_plan_.size()) {
            preparePlacementBatch();
            const size_t remaining_commands =
                verification_repair_clear_plan_.size() - verification_repair_clear_index_;
            const auto deadline = now + kCommandBatchBuildBudget;
            std::vector<std::string> commands;
            std::vector<uint32_t> block_counts;
            commands.reserve(std::min(remaining_commands, max_batch_commands_));
            block_counts.reserve(std::min(remaining_commands, max_batch_commands_));
            double tentative_block_tokens = available_block_tokens_;
            double tentative_command_tokens = available_command_tokens_;
            while (verification_repair_clear_index_ + commands.size() <
                       verification_repair_clear_plan_.size() &&
                   tentative_block_tokens >= 1.0 && tentative_command_tokens >= 1.0 &&
                   commands.size() < max_batch_commands_ &&
                   !commandBatchDeadlineReached(commands.size(), deadline)) {
                const PlannedCommand& planned = verification_repair_clear_plan_[
                    verification_repair_clear_index_ + commands.size()];
                commands.push_back(commandFor(planned));
                block_counts.push_back(planned.block_count);
                tentative_block_tokens -= planned.block_count;
                tentative_command_tokens -= 1.0;
            }
            size_t sent_count = 0;
            const bool complete = executeCommands(commands, &sent_count);
            uint64_t emitted_blocks = 0;
            for (size_t index = 0; index < sent_count; ++index) {
                emitted_blocks += block_counts[index];
            }
            verification_repair_clear_index_ += sent_count;
            if (sent_count != 0) {
                noteDataCommandsSent(sent_count, emitted_blocks);
                verification_repair_phase_dirty_ = true;
            }
            if (!complete) {
                restartForTransport();
                return;
            }
            return;
        }
        preparePlacementBatch();
        if (!repairPhaseDrained(ImportPhase::Clear, now)) {
            if (!takeDrainFailure().empty()) restartForTransport();
            return;
        }
        verification_repair_phase_ = phaseIndex(ImportPhase::Structure);
        verification_repair_chunk_cursor_ = 0;
        verification_repair_replayed_paths_.clear();
        return;
    }

    while (verification_repair_phase_ != phaseIndex(ImportPhase::Count) &&
           !verification_repair_reader_) {
        const ImportPhase phase = static_cast<ImportPhase>(verification_repair_phase_);
        const size_t base_count = verification_repair_chunks_.size();
        const size_t halo_count =
            placementOrderIndex(phase) >= placementOrderIndex(ImportPhase::Attachment)
            ? verification_repair_halo_chunks_.size() : 0;
        const size_t chunk_count = base_count + halo_count;
        if (verification_repair_chunk_cursor_ >= chunk_count) {
            preparePlacementBatch();
            if (!repairPhaseDrained(phase, now)) {
                if (!takeDrainFailure().empty()) restartForTransport();
                return;
            }
            verification_repair_phase_ = phaseIndex(nextPlacementPhase(phase));
            verification_repair_chunk_cursor_ = 0;
            verification_repair_replayed_paths_.clear();
            continue;
        }
        const ChunkCoord& repair_chunk = verification_repair_chunk_cursor_ < base_count
            ? verification_repair_chunks_[verification_repair_chunk_cursor_]
            : verification_repair_halo_chunks_[verification_repair_chunk_cursor_ - base_count];
        const std::string path = commandPlanPath(
            spool_directory_, repair_chunk, phase, region_spools, region_span,
            region_grid_origin);
        // With region spools many chunks share one plan file. Track the paths
        // already visited this phase in a set instead of re-deriving every
        // predecessor's path per step, which was quadratic in the chunk count.
        const bool already_replayed =
            !verification_repair_replayed_paths_.insert(path).second;
        if (already_replayed) {
            ++verification_repair_chunk_cursor_;
            continue;
        }
        std::ifstream exists(path, std::ios::binary);
        if (!exists) {
            ++verification_repair_chunk_cursor_;
            continue;
        }
        verification_repair_reader_ = std::make_unique<CommandSpoolReader>(path);
        if (!verification_repair_reader_->valid()) {
            finishFinalVerification(false, "cannot open chunk repair command spool");
            return;
        }
        verification_repair_reader_chunk_filter_.clear();
        if (region_spools && verification_repair_chunk_cursor_ >= base_count) {
            const ChunkCoord halo_region = regionForChunk(
                repair_chunk, region_span, region_grid_origin);
            for (const ChunkCoord& halo_chunk : verification_repair_halo_chunks_) {
                if (sameChunk(regionForChunk(
                        halo_chunk, region_span, region_grid_origin), halo_region)) {
                    verification_repair_reader_chunk_filter_.insert(halo_chunk);
                }
            }
        }
    }
    if (verification_repair_phase_ == phaseIndex(ImportPhase::Count)) {
        verification_repair_chunk_.reset();
        verification_repair_reader_.reset();
        verification_repair_reader_chunk_filter_.clear();
        verification_sample_index_ = verification_repair_sample_begin_;
        controller_.recordVerificationProgress(verification_sample_index_, false, nullptr);
        verification_readable_chunk_.reset();
        chunk_loaded_ = false;
        active_unit_last_send_at_ = {};
        verification_sample_ready_at_ = now + std::chrono::milliseconds(100);
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "repair pass sent; waiting for the server queue before rechecking";
        return;
    }

    if (verification_repair_reader_ &&
        verification_repair_reader_->exhausted()) {
        verification_repair_reader_.reset();
        verification_repair_reader_chunk_filter_.clear();
        ++verification_repair_chunk_cursor_;
        return;
    }
    preparePlacementBatch();
    const auto deadline = now + kCommandBatchBuildBudget;
    std::vector<std::string> commands;
    std::vector<uint32_t> block_counts;
    bool reached_end = false;
    double tentative_block_tokens = available_block_tokens_;
    double tentative_command_tokens = available_command_tokens_;
    while (tentative_block_tokens >= 1.0 && tentative_command_tokens >= 1.0 &&
           commands.size() < max_batch_commands_ &&
           !commandBatchDeadlineReached(commands.size(), deadline)) {
        auto command = verification_repair_reader_->next();
        if (!command) {
            reached_end = true;
            break;
        }
        if (!verification_repair_reader_chunk_filter_.empty()) {
            const int64_t chunk_size = std::max<int32_t>(1, metadata.chunk_size);
            bool intersects_filtered_chunk = false;
            for (const ChunkCoord& filtered : verification_repair_reader_chunk_filter_) {
                const int64_t chunk_min_x = static_cast<int64_t>(filtered.x) * chunk_size;
                const int64_t chunk_min_z = static_cast<int64_t>(filtered.z) * chunk_size;
                const int64_t chunk_max_x = chunk_min_x + chunk_size - 1;
                const int64_t chunk_max_z = chunk_min_z + chunk_size - 1;
                if (static_cast<int64_t>(command->bounds.max_x) >= chunk_min_x &&
                    static_cast<int64_t>(command->bounds.min_x) <= chunk_max_x &&
                    static_cast<int64_t>(command->bounds.max_z) >= chunk_min_z &&
                    static_cast<int64_t>(command->bounds.min_z) <= chunk_max_z) {
                    intersects_filtered_chunk = true;
                    break;
                }
            }
            // Region stitching can produce a single idempotent /fill spanning
            // several logical chunks. Replay the complete planned cuboid when
            // any selected halo chunk intersects it; dropping cross-chunk
            // commands leaves fluid and gravity seams permanently unrepaired.
            if (!intersects_filtered_chunk) {
                continue;
            }
        }
        commands.push_back(commandFor(*command));
        block_counts.push_back(command->block_count);
        tentative_block_tokens -= command->block_count;
        tentative_command_tokens -= 1.0;
    }
    uint64_t emitted_blocks = 0;
    if (!commands.empty()) {
        size_t sent_count = 0;
        const bool complete = executeCommands(commands, &sent_count);
        for (size_t index = 0; index < sent_count; ++index) {
            emitted_blocks += block_counts[index];
        }
        if (sent_count != 0) {
            noteDataCommandsSent(sent_count, emitted_blocks);
            verification_repair_emitted_blocks_ += emitted_blocks;
            verification_repair_phase_dirty_ = true;
        }
        if (!complete) {
            restartForTransport();
            return;
        }
    }
    if (verification_repair_reader_ &&
        verification_repair_reader_->exhausted()) reached_end = true;
    if (reached_end) {
        if (verification_repair_reader_->failed()) {
            finishFinalVerification(false, "corrupt chunk repair command spool");
            return;
        }
        verification_repair_reader_.reset();
        verification_repair_reader_chunk_filter_.clear();
        ++verification_repair_chunk_cursor_;
    }
}

void BuildImportRuntime::resetCommandBlockRuntime() {
    command_block_reader_.reset();
    command_block_pending_record_.reset();
    command_block_spool_path_.clear();
    command_block_state_path_.clear();
    command_block_cell_bounds_ = {};
    command_block_load_window_bounds_ = {};
    command_block_record_count_ = 0;
    command_block_cursor_ = 0;
    command_block_persisted_cursor_ = 0;
    command_block_cell_x_ = 0;
    command_block_cell_y_ = 0;
    command_block_cell_z_ = 0;
    command_block_cell_load_retries_ = 0;
    command_block_target_retries_ = 0;
    command_block_cell_ready_at_ = {};
    command_block_cell_deadline_ = {};
    command_block_writer_active_ = false;
    command_block_writer_completed_ = false;
    command_block_world_reader_.reset();
}

bool BuildImportRuntime::persistCommandBlockCursor(bool force, std::string* error) {
    if (!command_block_writer_active_ || command_block_state_path_.empty()) {
        if (error) *error = "command-block writer state is not initialized";
        return false;
    }
    if (!force && command_block_cursor_ == command_block_persisted_cursor_ &&
        commandBlockStateExists(command_block_state_path_)) {
        return true;
    }
    if (!saveCommandBlockState(command_block_state_path_, command_block_record_count_,
                               command_block_cursor_, error)) {
        return false;
    }
    command_block_persisted_cursor_ = command_block_cursor_;
    return true;
}

bool BuildImportRuntime::beginCommandBlockWrite(std::string* error) {
    if (command_block_writer_active_) return true;
    if (command_block_writer_completed_) return false;

    const std::string spool_path = spool_directory_ + "/" + CommandBlockSpoolReader::kFileName;
    const std::string manifest_path = spool_path + ".manifest";
    if (!commandBlockStateExists(spool_path) && !commandBlockStateExists(manifest_path)) {
        // No parser emitted a command-block side spool.  This is the normal
        // path for pixel art and ordinary block-only structures.
        return false;
    }
    if (!commandBlockStateExists(spool_path) || !commandBlockStateExists(manifest_path)) {
        if (error) *error = "command-block spool or manifest is missing";
        return false;
    }

    CommandBlockSpoolManifest manifest;
    std::string manifest_error;
    if (!CommandBlockSpoolWriter::readManifest(manifest_path, &manifest, &manifest_error)) {
        if (error) *error = manifest_error.empty()
            ? "cannot read command-block spool manifest" : manifest_error;
        return false;
    }
    auto reader = std::make_unique<CommandBlockSpoolReader>(manifest.spool_path);
    if (!reader->valid()) {
        if (error) *error = "cannot open command-block spool";
        return false;
    }
    if (reader->recordCount() != manifest.record_count ||
        reader->dataBytes() != manifest.data_bytes) {
        if (error) *error = "command-block spool does not match its manifest";
        return false;
    }

    const std::string state_path = spool_directory_ + "/" + kCommandBlockStateFileName;
    uint64_t cursor = 0;
    if (!loadCommandBlockState(state_path, reader->recordCount(), &cursor, error) ||
        !reader->seekRecord(cursor)) {
        if (error && error->empty()) *error = "cannot seek command-block spool cursor";
        return false;
    }

    command_block_spool_path_ = manifest.spool_path;
    command_block_state_path_ = state_path;
    command_block_record_count_ = reader->recordCount();
    command_block_cursor_ = cursor;
    command_block_persisted_cursor_ = cursor;
    command_block_reader_ = std::move(reader);
    command_block_pending_record_ = command_block_reader_->next();
    if (!command_block_pending_record_ && command_block_reader_->failed()) {
        if (error) *error = "command-block spool is corrupt";
        resetCommandBlockRuntime();
        return false;
    }
    command_block_writer_active_ = true;
    command_block_writer_completed_ = false;
    if (!persistCommandBlockCursor(false, error)) {
        resetCommandBlockRuntime();
        return false;
    }
    if (command_block_pending_record_) {
        command_block_cell_x_ = commandBlockCellXzCoordinate(command_block_pending_record_->x);
        command_block_cell_y_ = commandBlockCellYCoordinate(command_block_pending_record_->y);
        command_block_cell_z_ = commandBlockCellXzCoordinate(command_block_pending_record_->z);
        command_block_cell_bounds_ = commandBlockCellBounds(
            command_block_cell_x_, command_block_cell_y_, command_block_cell_z_);
        command_block_load_window_bounds_ = commandBlockLoadWindowBounds(
            command_block_cell_x_, command_block_cell_y_, command_block_cell_z_);
        command_block_cell_load_retries_ = 0;
        command_block_target_retries_ = 0;
        // The last verification cell owns the same stable ticking-area name.
        // Remove it before preparing the first command cell so its bounds do
        // not remain stale on a server that treats same-name additions as no-op.
        releaseLoadedRegion(true);
        stage_ = ExecuteStage::CommandBlockPrepare;
    } else {
        stage_ = ExecuteStage::CommandBlockWrite;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "writing command-block data (" + std::to_string(command_block_cursor_) +
            "/" + std::to_string(command_block_record_count_) + ")";
    }
    LOGI("[command-block] deferred writer started cursor=%llu total=%llu",
         static_cast<unsigned long long>(command_block_cursor_),
         static_cast<unsigned long long>(command_block_record_count_));
    return true;
}

bool BuildImportRuntime::beginCommandBlockCell(std::chrono::steady_clock::time_point now,
                                               std::string* error) {
    if (!command_block_pending_record_) {
        if (error) *error = "command-block cell has no pending record";
        return false;
    }
    const CommandBlockRecord& record = *command_block_pending_record_;
    const int32_t cell_x = commandBlockCellXzCoordinate(record.x);
    const int32_t cell_y = commandBlockCellYCoordinate(record.y);
    const int32_t cell_z = commandBlockCellXzCoordinate(record.z);
    const bool different_cell = cell_x != command_block_cell_x_ ||
        cell_y != command_block_cell_y_ || cell_z != command_block_cell_z_ ||
        !command_block_cell_bounds_.isValid();
    command_block_cell_x_ = cell_x;
    command_block_cell_y_ = cell_y;
    command_block_cell_z_ = cell_z;
    command_block_cell_bounds_ = commandBlockCellBounds(cell_x, cell_y, cell_z);
    if (!command_block_cell_bounds_.isValid()) {
        if (error) *error = "command-block cell bounds are invalid";
        return false;
    }
    const bool reuse_loaded_window = loaded_region_.has_value() &&
        containsBounds(command_block_load_window_bounds_, command_block_cell_bounds_);
    if (!reuse_loaded_window) {
        command_block_load_window_bounds_ = commandBlockLoadWindowBounds(cell_x, cell_y, cell_z);
        if (!command_block_load_window_bounds_.isValid()) {
            if (error) *error = "command-block load window bounds are invalid";
            return false;
        }
    }
    if (different_cell) {
        command_block_cell_load_retries_ = 0;
        command_block_target_retries_ = 0;
    }
    command_block_cell_ready_at_ = {};
    command_block_cell_deadline_ = {};
    command_block_world_reader_.reset();
    resetServerChunkProbe();
    if (!reuse_loaded_window) {
        // A live verification cell may still own infinitecz_build. Defer the
        // next add until its tracked removal has completed at the top of a
        // later tick. Adjacent command-block cells keep their shared window.
        releaseLoadedRegion(true);
    }
    stage_ = ExecuteStage::CommandBlockPrepare;
    (void)now;
    return true;
}

bool BuildImportRuntime::commandBlockCellReady(std::chrono::steady_clock::time_point now,
                                               std::string* error) {
    if (!command_block_cell_bounds_.isValid()) {
        if (error) *error = "command-block load cell is invalid";
        return false;
    }
    if (now < command_block_cell_ready_at_) return false;
    std::string probe_error;
    const ChunkProbeState probe = pollServerChunkProbe(command_block_cell_bounds_, now,
                                                        &probe_error);
    const bool server_ready = probe == ChunkProbeState::Ready;
    const bool native_ready = probe == ChunkProbeState::TransportFailure &&
        nativeChunksReadable(command_block_cell_bounds_);
    if (server_ready || native_ready) {
        if (command_block_pending_record_ &&
            localPlayerIsNearCommandBlockRecord(*command_block_pending_record_)) return true;
        if (error) *error = "local player has not arrived near the command-block target";
        return false;
    }
    if (probe == ChunkProbeState::TransportFailure && error && !probe_error.empty()) {
        *error = probe_error;
    }
    return false;
}

void BuildImportRuntime::pauseCommandBlockWrite(const std::string& detail) {
    std::string state_error;
    const bool persisted = command_block_writer_active_ &&
        persistCommandBlockCursor(true, &state_error);
    command_block_writer_active_ = false;
    command_block_reader_.reset();
    command_block_pending_record_.reset();
    std::string reason = "command-block write paused: " + detail;
    if (!persisted && !state_error.empty()) {
        reason += "; progress state write failed: " + state_error;
    }
    pauseFinalVerification(reason);
}

void BuildImportRuntime::finishCommandBlockWrite() {
    if (!command_block_writer_active_) return;
    std::string state_error;
    if (!persistCommandBlockCursor(true, &state_error)) {
        pauseCommandBlockWrite(state_error.empty()
            ? "cannot persist final command-block cursor" : state_error);
        return;
    }
    if (command_block_cursor_ != command_block_record_count_) {
        pauseCommandBlockWrite("command-block spool ended before every record was written");
        return;
    }
    command_block_writer_active_ = false;
    command_block_reader_.reset();
    command_block_pending_record_.reset();
    command_block_writer_completed_ = true;
    std::remove(command_block_state_path_.c_str());
    std::remove((command_block_state_path_ + ".tmp").c_str());
    LOGI("[command-block] deferred writer complete records=%llu",
         static_cast<unsigned long long>(command_block_record_count_));
    // finishFinalVerification owns the final ticking-area cleanup and removes
    // the command-block spool only after this guard has made re-entry safe.
    finishFinalVerification(true);
}

void BuildImportRuntime::tickCommandBlockWrite(std::chrono::steady_clock::time_point now) {
    if (!command_block_writer_active_) return;
    if (stage_ == ExecuteStage::CommandBlockPrepare) {
        // releaseLoadedRegion(true) may have queued a tracked cleanup.  The
        // common onGameTick prefix drives it before we ever re-add the stable
        // ticking area, so a same-name add cannot accidentally retain old bounds.
        if (!pending_cleanup_commands_.empty() || !cleanup_barrier_uuids_.empty() ||
            (region_add_ready_at_.time_since_epoch().count() && now < region_add_ready_at_)) {
            return;
        }
        if (!command_block_pending_record_) {
            stage_ = ExecuteStage::CommandBlockWrite;
            return;
        }
        const BuildImportRuntimeMetadata metadata = controller_.runtimeMetadata();
        const CommandBlockRecord& pending_record = *command_block_pending_record_;
        const CommandBlockWriteTeleportTarget target =
            commandBlockWriteTeleportTarget(pending_record);
        const int32_t teleport_x = target.x;
        const int32_t teleport_y = target.y;
        const int32_t teleport_z = target.z;
        const bool reuse_loaded_window = loaded_region_.has_value() &&
            containsBounds(command_block_load_window_bounds_, command_block_cell_bounds_);
        if (!command_block_load_window_bounds_.isValid()) {
            pauseCommandBlockWrite("command-block load window is invalid");
            return;
        }
        std::vector<std::string> commands;
        if (!reuse_loaded_window) {
            commands.push_back("/tickingarea add " +
                std::to_string(command_block_load_window_bounds_.min_x) + " " +
                std::to_string(command_block_load_window_bounds_.min_y) + " " +
                std::to_string(command_block_load_window_bounds_.min_z) + " " +
                std::to_string(command_block_load_window_bounds_.max_x) + " " +
                std::to_string(command_block_load_window_bounds_.max_y) + " " +
                std::to_string(command_block_load_window_bounds_.max_z) +
                " infinitecz_build true");
        }

        int32_t player_x = 0;
        int32_t player_y = 0;
        int32_t player_z = 0;
        if (NativeWorldAccess::getLocalPlayerBlockPosition(&player_x, &player_y, &player_z)) {
            const double dx = static_cast<double>(teleport_x) - player_x;
            const double dy = static_cast<double>(teleport_y) - player_y;
            const double dz = static_cast<double>(teleport_z) - player_z;
            const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            constexpr double kMaximumTeleportHopDistance = 96.0;
            constexpr int64_t kMaximumTeleportHopCommands = 512;
            if (std::isfinite(distance) && distance > kMaximumTeleportHopDistance) {
                const int64_t hop_count = std::min<int64_t>(kMaximumTeleportHopCommands,
                    static_cast<int64_t>(distance / kMaximumTeleportHopDistance));
                for (int64_t index = 1; index <= hop_count; ++index) {
                    const double fraction = static_cast<double>(index) /
                        static_cast<double>(hop_count + 1);
                    commands.push_back("/tp @s " + std::to_string(clampCommandBlockCellToInt32(
                        static_cast<int64_t>(std::floor(player_x + dx * fraction)))) + " " +
                        std::to_string(clampCommandBlockCellToInt32(
                        static_cast<int64_t>(std::floor(player_y + dy * fraction)))) + " " +
                        std::to_string(clampCommandBlockCellToInt32(
                        static_cast<int64_t>(std::floor(player_z + dz * fraction)))));
                }
            }
        }
        commands.push_back("/tp @s " + std::to_string(teleport_x) + " " +
                           std::to_string(teleport_y) + " " + std::to_string(teleport_z));
        size_t sent_count = 0;
        if (!executeCommands(commands, &sent_count) || sent_count != commands.size()) {
            pauseCommandBlockWrite("cannot teleport to command-block cell");
            return;
        }
        // The position cache is intentionally short-lived during normal scans,
        // but this transition must observe the result of the TP rather than
        // reuse a pre-teleport coordinate for up to 150 ms.
        NativeWorldAccess::invalidateLocalPlayerBlockPositionCache();
        if (!reuse_loaded_window) {
            loaded_region_ = ChunkCoord{command_block_cell_x_, command_block_cell_z_};
            loaded_region_cleanup_command_ = "/tickingarea remove infinitecz_build";
            loaded_region_probe_bounds_ = command_block_load_window_bounds_;
            loaded_region_started_at_ = now;
            loaded_region_fully_confirmed_ = false;
            confirmed_region_chunks_.clear();
        }
        resetServerChunkProbe();
        command_block_cell_ready_at_ = reuse_loaded_window
            ? now : now + kCommandBlockCellMinimumWait;
        const auto configured_wait = std::chrono::milliseconds(
            static_cast<int64_t>(std::max<int32_t>(20, metadata.chunk_wait_ticks)) * 50);
        command_block_cell_deadline_ = now + std::max(std::chrono::milliseconds(10000),
            configured_wait + std::chrono::milliseconds(2000));
        stage_ = ExecuteStage::CommandBlockWait;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "waiting for command-block cell " +
                std::to_string(command_block_cell_x_) + "," +
                std::to_string(command_block_cell_y_) + "," +
                std::to_string(command_block_cell_z_) + " to load";
        }
        return;
    }

    if (stage_ == ExecuteStage::CommandBlockWait) {
        std::string load_error;
        if (commandBlockCellReady(now, &load_error)) {
            stage_ = ExecuteStage::CommandBlockWrite;
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "writing command-block data (" +
                std::to_string(command_block_cursor_) + "/" +
                std::to_string(command_block_record_count_) + ")";
            return;
        }
        if (now < command_block_cell_deadline_) return;
        if (++command_block_cell_load_retries_ <= kCommandBlockCellLoadRetryLimit) {
            LOGI("[command-block] cell load retry=%u cell=(%d,%d,%d)",
                 command_block_cell_load_retries_, command_block_cell_x_,
                 command_block_cell_y_, command_block_cell_z_);
            resetServerChunkProbe();
            releaseLoadedRegion(true);
            stage_ = ExecuteStage::CommandBlockPrepare;
            return;
        }
        pauseCommandBlockWrite(load_error.empty()
            ? "command-block cell did not load after retries" : load_error);
        return;
    }

    if (stage_ != ExecuteStage::CommandBlockWrite) return;
    if (!command_block_pending_record_) {
        if (command_block_reader_ && command_block_reader_->failed()) {
            pauseCommandBlockWrite("command-block spool became corrupt");
        } else {
            finishCommandBlockWrite();
        }
        return;
    }
    if (!sameCommandBlockCell(*command_block_pending_record_, command_block_cell_x_,
                               command_block_cell_y_, command_block_cell_z_)) {
        std::string cell_error;
        if (!beginCommandBlockCell(now, &cell_error)) pauseCommandBlockWrite(cell_error);
        return;
    }
    // The chunk probe only proves the server can read this cell. The native
    // editor packet has a much tighter, target-specific distance check, so
    // re-check the actual pending target before starting its write burst.
    if (!localPlayerIsNearCommandBlockRecord(*command_block_pending_record_)) {
        stage_ = ExecuteStage::CommandBlockPrepare;
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "repositioning near command-block cell before data write";
        return;
    }

    uint32_t sent = 0;
    const auto write_deadline = std::chrono::steady_clock::now() +
        kCommandBlockWriteTickBudget;
    const auto persist_sent_burst = [&]() {
        if (sent == 0) return true;
        // Saving one bounded burst avoids synchronously rewriting the sidecar
        // for every packet. The cursor never advances on disk before send(),
        // so an interruption can only replay already-written, idempotent data.
        std::string state_error;
        if (persistCommandBlockCursor(true, &state_error)) return true;
        pauseCommandBlockWrite(state_error.empty()
            ? "cannot persist command-block cursor" : state_error);
        return false;
    };
    while (command_block_pending_record_ && sent < kCommandBlockWritesPerTick &&
           std::chrono::steady_clock::now() < write_deadline &&
           sameCommandBlockCell(*command_block_pending_record_, command_block_cell_x_,
                                  command_block_cell_y_, command_block_cell_z_)) {
        // Later records in the same cell may lie farther from a player who
        // moved while a previous packet was being serialized.  Reposition
        // before that record instead of advancing its persistent cursor.
        if (!localPlayerIsNearCommandBlockRecord(*command_block_pending_record_)) {
            if (!persist_sent_burst()) return;
            stage_ = ExecuteStage::CommandBlockPrepare;
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "repositioning near command-block target before data write";
            return;
        }
        // A successful /fill/repair plan only proves that the command-block
        // shell was scheduled. Before serializing its NBT packet, make sure
        // the locally loaded target is still the expected shell. This prevents
        // a delayed chunk, an overwrite collision, or a bad mapping from
        // silently advancing the durable sidecar cursor against air/another
        // block type.
        std::string target_error;
        if (!commandBlockTargetMatches(&command_block_world_reader_,
                                       *command_block_pending_record_,
                                       &target_error)) {
            if (++command_block_target_retries_ <= kCommandBlockTargetRetryLimit) {
                if (!persist_sent_burst()) return;
                LOGI("[command-block] target retry=%u cell=(%d,%d,%d): %s",
                     command_block_target_retries_, command_block_cell_x_,
                     command_block_cell_y_, command_block_cell_z_, target_error.c_str());
                resetServerChunkProbe();
                command_block_world_reader_.reset();
                releaseLoadedRegion(true);
                stage_ = ExecuteStage::CommandBlockPrepare;
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = "reloading command-block target before data write";
                return;
            }
            pauseCommandBlockWrite(target_error.empty()
                ? "command-block target did not appear after retries" : target_error);
            return;
        }
        command_block_target_retries_ = 0;
        // The command-block sidecar may have been produced by an older build,
        // or restored from a checkpoint made before legacy execute conversion
        // was added.  Normalize again at the one point that actually writes
        // the packet so no recovery path can bypass the compatibility layer.
        CommandBlockRecord record_to_send = *command_block_pending_record_;
        const bool converted_execute = normalizeLegacyExecuteCommand(&record_to_send.command);
        if (converted_execute) {
            LOGI("[command-block] normalized legacy execute at (%d,%d,%d) during delivery",
                 record_to_send.x, record_to_send.y, record_to_send.z);
        }
        const bool executes_on_first_tick = record_to_send.executing_on_first_tick;
        std::string packet_error;
        if (!CommandBlockPacketSender::send(record_to_send, &packet_error)) {
            pauseCommandBlockWrite(packet_error.empty()
                ? "native CommandBlockUpdatePacket send failed" : packet_error);
            return;
        }
        ++command_block_cursor_;
        ++sent;
        if (executes_on_first_tick) {
            // This flag can make the block execute as soon as the update is
            // applied. Do not leave it inside an otherwise replayable burst:
            // persist immediately so a pause/restart cannot repeat the whole
            // preceding batch of command updates.
            std::string state_error;
            if (!persistCommandBlockCursor(true, &state_error)) {
                pauseCommandBlockWrite(state_error.empty()
                    ? "cannot persist first-tick command-block cursor" : state_error);
                return;
            }
        }
        command_block_pending_record_ = command_block_reader_->next();
        if (!command_block_pending_record_ && command_block_reader_->failed()) {
            pauseCommandBlockWrite("command-block spool became corrupt");
            return;
        }
    }
    if (!persist_sent_burst()) return;
    if (!command_block_pending_record_) {
        finishCommandBlockWrite();
        return;
    }
    if (!sameCommandBlockCell(*command_block_pending_record_, command_block_cell_x_,
                              command_block_cell_y_, command_block_cell_z_)) {
        std::string cell_error;
        if (!beginCommandBlockCell(now, &cell_error)) pauseCommandBlockWrite(cell_error);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "writing command-block data (" + std::to_string(command_block_cursor_) +
            "/" + std::to_string(command_block_record_count_) + ")";
    }
}


void BuildImportRuntime::resetSignShellProbe() {
    std::string uuid;
    uuid.swap(sign_shell_probe_uuid_);
    sign_shell_probe_poll_at_ = {};
    sign_shell_probe_deadline_ = {};
    if (!uuid.empty()) {
        std::lock_guard<std::mutex> lock(native_event_mutex_);
        received_rpc_acks_.erase(uuid);
    }
}

void BuildImportRuntime::resetSignRuntime() {
    if (sign_edit_session_token_ != 0U) {
        CancelSignEditSession(sign_edit_session_token_);
    }
    resetSignShellProbe();
    sign_reader_.reset();
    sign_pending_record_.reset();
    sign_state_path_.clear();
    sign_record_count_ = 0;
    sign_cursor_ = 0;
    sign_persisted_cursor_ = 0;
    sign_cell_x_ = 0;
    sign_cell_y_ = 0;
    sign_cell_z_ = 0;
    sign_cell_bounds_ = {};
    sign_target_attempts_ = 0;
    sign_target_ready_at_ = {};
    sign_edit_session_deadline_ = {};
    sign_verify_poll_at_ = {};
    sign_verify_deadline_ = {};
    sign_last_verification_error_.clear();
    sign_edit_session_token_ = 0;
    sign_edit_probe_index_ = 0;
    sign_front_written_ = false;
    sign_back_written_ = false;
    sign_active_face_front_ = true;
    sign_face_includes_waxed_ = false;
    sign_writer_active_ = false;
    sign_writer_completed_ = false;
    sign_world_reader_.reset();
}

bool BuildImportRuntime::persistSignCursor(bool force, std::string* error) {
    if (!sign_writer_active_ || sign_state_path_.empty()) {
        if (error) *error = "sign writer state is not initialized";
        return false;
    }
    if (!force && sign_cursor_ == sign_persisted_cursor_ &&
        commandBlockStateExists(sign_state_path_)) {
        return true;
    }
    if (!saveDeferredDataState(sign_state_path_, sign_record_count_, sign_cursor_, error)) {
        return false;
    }
    sign_persisted_cursor_ = sign_cursor_;
    return true;
}

bool BuildImportRuntime::beginSignWrite(std::string* error) {
    if (sign_writer_active_) return true;
    if (sign_writer_completed_) return false;
    const std::string spool_path = spool_directory_ + "/" + SignSpoolWriter::kFileName;
    if (!commandBlockStateExists(spool_path)) return false;
    auto reader = std::make_unique<SignSpoolReader>(spool_path);
    if (!reader->valid()) {
        if (error) *error = "cannot open sign spool";
        return false;
    }
    const std::string state_path = spool_directory_ + "/" + kSignStateFileName;
    uint64_t cursor = 0;
    if (!loadDeferredDataState(state_path, reader->recordCount(), &cursor, error) ||
        !reader->seekRecord(cursor)) {
        if (error && error->empty()) *error = "cannot seek sign spool cursor";
        return false;
    }
    sign_state_path_ = state_path;
    sign_record_count_ = reader->recordCount();
    sign_cursor_ = cursor;
    sign_persisted_cursor_ = cursor;
    sign_reader_ = std::move(reader);
    sign_pending_record_ = sign_reader_->next();
    if (!sign_pending_record_ && sign_reader_->failed()) {
        if (error) *error = "sign spool is corrupt";
        resetSignRuntime();
        return false;
    }
    if (sign_pending_record_ &&
        !SignBlockActorPacketSender::validate(*sign_pending_record_, error)) {
        resetSignRuntime();
        return false;
    }
    sign_writer_active_ = true;
    sign_writer_completed_ = false;
    if (!persistSignCursor(false, error)) {
        resetSignRuntime();
        return false;
    }
    if (sign_pending_record_) {
        const SignRecord& record = *sign_pending_record_;
        sign_cell_x_ = deferredCellCoordinate(record.x, kSignCellSpanBlocks);
        sign_cell_y_ = deferredCellCoordinate(record.y, kSignCellSpanBlocks);
        sign_cell_z_ = deferredCellCoordinate(record.z, kSignCellSpanBlocks);
        sign_cell_bounds_ = deferredCellBounds(
            sign_cell_x_, sign_cell_y_, sign_cell_z_, kSignCellSpanBlocks);
        if (!sign_cell_bounds_.isValid()) {
            if (error) *error = "sign cell bounds are invalid";
            resetSignRuntime();
            return false;
        }
        releaseLoadedRegion(true);
        stage_ = ExecuteStage::SignPrepare;
    } else {
        stage_ = ExecuteStage::SignWrite;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "restoring sign data (" + std::to_string(sign_cursor_) + "/" +
            std::to_string(sign_record_count_) + ")";
    }
    LOGI("[sign] deferred writer started cursor=%llu total=%llu",
         static_cast<unsigned long long>(sign_cursor_),
         static_cast<unsigned long long>(sign_record_count_));
    return true;
}

void BuildImportRuntime::retryOrPauseSignWrite(const std::string& detail) {
    if (sign_edit_session_token_ != 0U) {
        CancelSignEditSession(sign_edit_session_token_);
        sign_edit_session_token_ = 0;
    }
    resetSignShellProbe();
    sign_target_ready_at_ = {};
    sign_edit_session_deadline_ = {};
    sign_verify_poll_at_ = {};
    sign_verify_deadline_ = {};
    sign_last_verification_error_ = detail;
    sign_edit_probe_index_ = 0;
    sign_active_face_front_ = true;
    sign_face_includes_waxed_ = false;
    sign_world_reader_.reset();
    ++sign_target_attempts_;
    if (sign_target_attempts_ >= kSignMaximumTargetAttempts) {
        std::string target;
        if (sign_pending_record_) {
            target = " at (" + std::to_string(sign_pending_record_->x) + "," +
                std::to_string(sign_pending_record_->y) + "," +
                std::to_string(sign_pending_record_->z) + ")";
        }
        pauseSignWrite("target" + target + " failed after " +
            std::to_string(sign_target_attempts_) + " attempts: " + detail);
        return;
    }
    stage_ = ExecuteStage::SignPrepare;
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = "retrying sign data (attempt " +
        std::to_string(sign_target_attempts_ + 1) + "/" +
        std::to_string(kSignMaximumTargetAttempts) + ")";
}

void BuildImportRuntime::pauseSignWrite(const std::string& detail) {
    std::string state_error;
    const bool persisted = sign_writer_active_ && persistSignCursor(true, &state_error);
    sign_writer_active_ = false;
    sign_reader_.reset();
    sign_pending_record_.reset();
    resetSignShellProbe();
    sign_target_ready_at_ = {};
    sign_edit_session_deadline_ = {};
    sign_verify_poll_at_ = {};
    sign_verify_deadline_ = {};
    sign_edit_probe_index_ = 0;
    sign_front_written_ = false;
    sign_back_written_ = false;
    sign_active_face_front_ = true;
    sign_face_includes_waxed_ = false;
    std::string reason = "sign write paused: " + detail;
    if (!persisted && !state_error.empty()) {
        reason += "; progress state write failed: " + state_error;
    }
    pauseFinalVerification(reason);
}

void BuildImportRuntime::finishSignWrite() {
    if (!sign_writer_active_) return;
    std::string state_error;
    if (!persistSignCursor(true, &state_error)) {
        pauseSignWrite(state_error.empty() ? "cannot persist final sign cursor" : state_error);
        return;
    }
    if (sign_cursor_ != sign_record_count_) {
        pauseSignWrite("sign spool ended before every record was verified");
        return;
    }
    sign_writer_active_ = false;
    sign_reader_.reset();
    sign_pending_record_.reset();
    resetSignShellProbe();
    sign_target_ready_at_ = {};
    sign_edit_session_deadline_ = {};
    sign_verify_poll_at_ = {};
    sign_verify_deadline_ = {};
    sign_last_verification_error_.clear();
    sign_edit_probe_index_ = 0;
    sign_front_written_ = false;
    sign_back_written_ = false;
    sign_active_face_front_ = true;
    sign_face_includes_waxed_ = false;
    sign_writer_completed_ = true;
    LOGI("[sign] deferred writer complete records=%llu",
         static_cast<unsigned long long>(sign_record_count_));
    finishFinalVerification(true);
}

void BuildImportRuntime::tickSignWrite(std::chrono::steady_clock::time_point now) {
    if (!sign_writer_active_) return;
    const auto schedule_next_sign_probe = [&]() {
        if (!sign_pending_record_ ||
            sign_edit_probe_index_ + 1U >= kSignEditProbeCount) {
            return false;
        }
        ++sign_edit_probe_index_;
        const SignRecord& record = *sign_pending_record_;
        if (!executeCommand(formatSignEditProbeTeleportCommand(
                record.x, record.y, record.z, sign_edit_probe_index_))) {
            return false;
        }
        NativeWorldAccess::invalidateLocalPlayerBlockPositionCache();
        sign_world_reader_.reset();
        sign_target_ready_at_ = now + kSignEditProbeSettleDelay;
        stage_ = ExecuteStage::SignWait;
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "moving to the other side of sign at " +
            std::to_string(record.x) + "," + std::to_string(record.y) + "," +
            std::to_string(record.z);
        return true;
    };
    if (stage_ == ExecuteStage::SignPrepare) {
        if (!pending_cleanup_commands_.empty() || !cleanup_barrier_uuids_.empty() ||
            (region_add_ready_at_.time_since_epoch().count() && now < region_add_ready_at_)) {
            return;
        }
        if (!sign_pending_record_) {
            stage_ = ExecuteStage::SignWrite;
            return;
        }
        const SignRecord& record = *sign_pending_record_;
        const bool reuse_loaded_cell = loaded_region_.has_value() &&
            containsBounds(loaded_region_probe_bounds_, sign_cell_bounds_);
        std::vector<std::string> commands = containerItemPrepareCommands(
            sign_cell_bounds_, record.x, record.y, record.z, !reuse_loaded_cell);
        if (commands.empty()) {
            retryOrPauseSignWrite("sign cell bounds are invalid");
            return;
        }
        commands.back() = formatSignEditProbeTeleportCommand(
            record.x, record.y, record.z, sign_edit_probe_index_);
        size_t sent_count = 0;
        if (!executeCommands(commands, &sent_count) || sent_count != commands.size()) {
            retryOrPauseSignWrite("cannot teleport above sign target");
            return;
        }
        NativeWorldAccess::invalidateLocalPlayerBlockPositionCache();
        if (!reuse_loaded_cell) {
            loaded_region_ = ChunkCoord{sign_cell_x_, sign_cell_z_};
            loaded_region_cleanup_command_ = "/tickingarea remove infinitecz_build";
            loaded_region_probe_bounds_ = sign_cell_bounds_;
            loaded_region_started_at_ = now;
            loaded_region_fully_confirmed_ = false;
            confirmed_region_chunks_.clear();
        }
        sign_target_ready_at_ = now + kSignTargetSettleDelay;
        stage_ = ExecuteStage::SignWait;
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "waiting above sign at " + std::to_string(record.x) + "," +
            std::to_string(record.y) + "," + std::to_string(record.z);
        return;
    }

    if (stage_ == ExecuteStage::SignWait) {
        if (now < sign_target_ready_at_) return;
        if (!sign_pending_record_) {
            stage_ = ExecuteStage::SignWrite;
            return;
        }
        const SignRecord& record = *sign_pending_record_;
        NativeBlockView block;
        sign_world_reader_.reset();
        if (!sign_world_reader_.open() ||
            !sign_world_reader_.getBlockView(record.x, record.y, record.z, &block) ||
            !block.name || !block.type_token) {
            retryOrPauseSignWrite("native sign target is unavailable");
            return;
        }
        const std::string target_identifier = *block.name;
        if (!deferredSignShellMatches(record.expected_sign_id, target_identifier)) {
            retryOrPauseSignWrite("target shell is " +
                (target_identifier.empty() ? std::string("unavailable") : target_identifier));
            return;
        }
        if (record.has_expected_aux) {
            if (sign_shell_probe_uuid_.empty()) {
                const CanonicalBlock expected_shell = placementBlock(
                    record.expected_sign_id, record.expected_aux);
                const std::string shell_probe_command =
                    "/testforblock " + std::to_string(record.x) + " " +
                    std::to_string(record.y) + " " + std::to_string(record.z) + " " +
                    formatVerificationBlockArgument(
                        expected_shell.name, expected_shell.aux, false);
                sign_shell_probe_uuid_ = nextRpcUuid();
                size_t sent_count = 0;
                if (!executeTrackedCommands(
                        {shell_probe_command}, {sign_shell_probe_uuid_}, &sent_count) ||
                    sent_count != 1U) {
                    resetSignShellProbe();
                    retryOrPauseSignWrite("cannot send exact sign BlockState probe");
                    return;
                }
                sign_shell_probe_poll_at_ = now + kRpcPollInterval;
                sign_shell_probe_deadline_ = now + kRpcProbeTimeout;
                LOGI("[sign] probing exact shell state at (%d,%d,%d): %s",
                     record.x, record.y, record.z, shell_probe_command.c_str());
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = "confirming sign orientation (" +
                    std::to_string(sign_cursor_) + "/" +
                    std::to_string(sign_record_count_) + ")";
                return;
            }
            if (now < sign_shell_probe_poll_at_) return;
            const RpcResultState state =
                pollTrackedCommands({sign_shell_probe_uuid_});
            if (state == RpcResultState::Pending &&
                now < sign_shell_probe_deadline_) {
                sign_shell_probe_poll_at_ = now + kRpcPollInterval;
                return;
            }
            if (state != RpcResultState::Accepted) {
                const bool rejected = state == RpcResultState::Rejected;
                resetSignShellProbe();
                retryOrPauseSignWrite(rejected
                    ? "server reports target sign BlockState differs"
                    : "exact sign BlockState probe timed out");
                return;
            }
            resetSignShellProbe();
        }

        // A crash may occur after the server accepted the final face but
        // before the durable cursor advanced. Verify first so an idempotent
        // resume never tries to reopen an already-waxed completed sign.
        NativeBlockSnapshot existing_snapshot;
        SignRecord existing_record;
        std::string existing_parse_error;
        std::string existing_mismatch;
        bool existing_lighting_normalized = false;
        if (NativeWorldAccess::getBlockSnapshot(
                record.x, record.y, record.z, &existing_snapshot, false,
                current_world_.dimension_id) &&
            existing_snapshot.entity_available &&
            !existing_snapshot.entity_json.empty() &&
            parseSignEntityJson(existing_snapshot.entity_json, &existing_record,
                                &existing_parse_error) &&
            signRecordMatchesWithLightingNormalization(
                record, existing_record, &existing_mismatch,
                &existing_lighting_normalized)) {
            if (existing_lighting_normalized) {
                LOGI("[sign] accepting server-normalized existing state at (%d,%d,%d): %s",
                     record.x, record.y, record.z, existing_mismatch.c_str());
            }
            sign_front_written_ = record.front.present;
            sign_back_written_ = record.back.present;
            sign_verify_poll_at_ = now;
            sign_verify_deadline_ = now + kSignVerificationTimeout;
            stage_ = ExecuteStage::SignWrite;
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "sign data already restored (" +
                std::to_string(sign_cursor_) + "/" +
                std::to_string(sign_record_count_) + ")";
            return;
        }

        uint64_t token = next_sign_edit_session_token_++;
        if (token == 0U) token = next_sign_edit_session_token_++;
        sign_edit_session_token_ = token;
        ArmSignEditSession(token, record.x, record.y, record.z);
        std::string click_error;
        if (!ContainerOpenPacketSender::send(record.x, record.y, record.z,
                                             block.type_token, 1, &click_error)) {
            CancelSignEditSession(token);
            sign_edit_session_token_ = 0;
            retryOrPauseSignWrite(click_error.empty()
                ? "cannot send sign ClickBlock packet" : click_error);
            return;
        }
        sign_edit_session_deadline_ = now + kSignEditSessionTimeout;
        stage_ = ExecuteStage::SignOpenWait;
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "waiting for silent sign editor (" +
            std::to_string(sign_cursor_) + "/" +
            std::to_string(sign_record_count_) + ")";
        return;
    }

    if (stage_ == ExecuteStage::SignOpenWait) {
        if (!sign_pending_record_) {
            retryOrPauseSignWrite("sign record disappeared while opening editor");
            return;
        }
        SignEditSessionResult edit_result;
        const SignEditSessionPollState poll =
            PollSignEditSession(sign_edit_session_token_, &edit_result);
        if (poll == SignEditSessionPollState::WaitingForOpen) {
            if (now >= sign_edit_session_deadline_) {
                retryOrPauseSignWrite("OpenSign packet timed out");
            }
            return;
        }
        if (poll == SignEditSessionPollState::Inactive) {
            retryOrPauseSignWrite("sign edit session became inactive");
            return;
        }
        if (poll == SignEditSessionPollState::Failed) {
            retryOrPauseSignWrite(edit_result.error.empty()
                ? "OpenSign packet capture failed" : edit_result.error);
            return;
        }

        const SignRecord& record = *sign_pending_record_;
        const bool has_face_payload = record.front.present || record.back.present;
        const bool selected_front = edit_result.front_side;
        const bool selected_present = selected_front
            ? record.front.present : record.back.present;
        const bool selected_written = selected_front
            ? sign_front_written_ : sign_back_written_;

        if (has_face_payload && (!selected_present || selected_written)) {
            // A sign editor remains active until the client submits a
            // BlockActorData packet. Re-submit the already-restored face (or
            // the server snapshot for a source-absent face) before probing the
            // opposite side, otherwise the next ClickBlock can be ignored.
            SignRecord close_record = record;
            if (!selected_present) {
                NativeBlockSnapshot snapshot;
                SignRecord observed;
                std::string parse_error;
                if (!NativeWorldAccess::getBlockSnapshot(
                        record.x, record.y, record.z, &snapshot, false,
                        current_world_.dimension_id) ||
                    !snapshot.entity_available || snapshot.entity_json.empty() ||
                    !parseSignEntityJson(snapshot.entity_json, &observed,
                                         &parse_error)) {
                    retryOrPauseSignWrite(parse_error.empty()
                        ? "cannot close unwanted sign face session"
                        : parse_error);
                    return;
                }
                observed.x = record.x;
                observed.y = record.y;
                observed.z = record.z;
                observed.expected_sign_id = record.expected_sign_id;
                close_record = std::move(observed);
            }
            const auto close_face = selected_front
                ? SignBlockActorPacketSender::Face::Front
                : SignBlockActorPacketSender::Face::Back;
            std::string close_error;
            if (!SignBlockActorPacketSender::sendFace(
                    close_record, close_face, false, &close_error)) {
                retryOrPauseSignWrite(close_error.empty()
                    ? "cannot close unwanted sign face session" : close_error);
                return;
            }
            CancelSignEditSession(sign_edit_session_token_);
            sign_edit_session_token_ = 0;
            sign_edit_session_deadline_ = {};
            if (!schedule_next_sign_probe()) {
                retryOrPauseSignWrite("server did not open the remaining sign face");
            }
            return;
        }

        const bool other_face_pending = selected_front
            ? (record.back.present && !sign_back_written_)
            : (record.front.present && !sign_front_written_);
        sign_active_face_front_ = selected_front;
        sign_face_includes_waxed_ = !other_face_pending;
        std::string packet_error;
        const bool sent = has_face_payload
            ? SignBlockActorPacketSender::sendFace(
                record,
                selected_front ? SignBlockActorPacketSender::Face::Front
                               : SignBlockActorPacketSender::Face::Back,
                sign_face_includes_waxed_, &packet_error)
            : SignBlockActorPacketSender::send(record, &packet_error);
        if (!sent) {
            retryOrPauseSignWrite(packet_error.empty()
                ? "cannot send sign block-actor packet" : packet_error);
            return;
        }
        CancelSignEditSession(sign_edit_session_token_);
        sign_edit_session_token_ = 0;
        sign_edit_session_deadline_ = {};
        sign_verify_poll_at_ = now + kSignVerificationPollInterval;
        sign_verify_deadline_ = now + kSignVerificationTimeout;
        sign_last_verification_error_.clear();
        stage_ = ExecuteStage::SignFaceVerify;
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "verifying sign face (" + std::to_string(sign_cursor_) + "/" +
            std::to_string(sign_record_count_) + ")";
        return;
    }

    if (stage_ == ExecuteStage::SignFaceVerify) {
        if (!sign_pending_record_) {
            retryOrPauseSignWrite("sign record disappeared during face verification");
            return;
        }
        if (now < sign_verify_poll_at_) return;
        sign_verify_poll_at_ = now + kSignVerificationPollInterval;

        const SignRecord& record = *sign_pending_record_;
        SignRecord expected = record;
        if (record.front.present || record.back.present) {
            if (sign_active_face_front_) expected.back = {};
            else expected.front = {};
            if (!sign_face_includes_waxed_) {
                expected.has_is_waxed = false;
                expected.is_waxed = false;
            }
        }
        NativeBlockSnapshot snapshot;
        SignRecord observed;
        std::string mismatch;
        std::string parse_error;
        bool matched = false;
        bool lighting_normalized = false;
        if (!NativeWorldAccess::getBlockSnapshot(
                record.x, record.y, record.z, &snapshot, false,
                current_world_.dimension_id)) {
            mismatch = "sign snapshot read failed";
        } else if (!snapshot.entity_available || snapshot.entity_json.empty()) {
            mismatch = "sign block-actor data is unavailable";
        } else if (!parseSignEntityJson(snapshot.entity_json, &observed,
                                        &parse_error)) {
            mismatch = parse_error.empty()
                ? "cannot parse sign block-actor data" : parse_error;
        } else {
            matched = signRecordMatchesWithLightingNormalization(
                expected, observed, &mismatch, &lighting_normalized);
            if (lighting_normalized) {
                LOGI("[sign] accepting server-normalized face state at (%d,%d,%d): %s",
                     record.x, record.y, record.z, mismatch.c_str());
            }
        }
        if (!matched) {
            sign_last_verification_error_ = mismatch.empty()
                ? "sign face data has not reached the server" : mismatch;
            if (now >= sign_verify_deadline_) {
                retryOrPauseSignWrite(sign_last_verification_error_);
            }
            return;
        }

        if (record.front.present || record.back.present) {
            if (sign_active_face_front_) sign_front_written_ = true;
            else sign_back_written_ = true;
        }
        const bool face_pending =
            (record.front.present && !sign_front_written_) ||
            (record.back.present && !sign_back_written_);
        sign_verify_poll_at_ = {};
        sign_verify_deadline_ = {};
        sign_last_verification_error_.clear();
        if (face_pending) {
            if (!schedule_next_sign_probe()) {
                retryOrPauseSignWrite("server did not open the remaining sign face");
            }
            return;
        }
        sign_verify_poll_at_ = now + kSignVerificationPollInterval;
        sign_verify_deadline_ = now + kSignVerificationTimeout;
        stage_ = ExecuteStage::SignWrite;
        return;
    }

    if (stage_ != ExecuteStage::SignWrite) return;
    if (!sign_pending_record_) {
        if (sign_reader_ && sign_reader_->failed()) pauseSignWrite("sign spool became corrupt");
        else finishSignWrite();
        return;
    }
    if (now < sign_verify_poll_at_) return;
    sign_verify_poll_at_ = now + kSignVerificationPollInterval;

    const SignRecord& record = *sign_pending_record_;
    NativeBlockSnapshot snapshot;
    std::string mismatch;
    bool matched = false;
    bool lighting_normalized = false;
    if (!NativeWorldAccess::getBlockSnapshot(record.x, record.y, record.z, &snapshot,
                                             false, current_world_.dimension_id)) {
        mismatch = "sign snapshot read failed";
    } else if (snapshot.client_block_available &&
               !deferredSignShellMatches(record.expected_sign_id,
                                         snapshot.client_identifier)) {
        mismatch = "target shell changed to " + snapshot.client_identifier;
    } else if (!snapshot.entity_available || snapshot.entity_json.empty()) {
        mismatch = "sign block-actor data is unavailable";
    } else {
        SignRecord observed;
        std::string parse_error;
        if (!parseSignEntityJson(snapshot.entity_json, &observed, &parse_error)) {
            mismatch = parse_error.empty() ? "cannot parse sign block-actor data" : parse_error;
        } else {
            matched = signRecordMatchesWithLightingNormalization(
                record, observed, &mismatch, &lighting_normalized);
            if (lighting_normalized) {
                LOGI("[sign] accepting server-normalized state at (%d,%d,%d): %s",
                     record.x, record.y, record.z, mismatch.c_str());
            }
        }
    }
    if (matched) {
        ++sign_cursor_;
        std::string state_error;
        if (!persistSignCursor(true, &state_error)) {
            pauseSignWrite(state_error.empty() ? "cannot persist sign cursor" : state_error);
            return;
        }
        sign_pending_record_ = sign_reader_->next();
        if (!sign_pending_record_ && sign_reader_->failed()) {
            pauseSignWrite("sign spool became corrupt");
            return;
        }
        sign_target_attempts_ = 0;
        resetSignShellProbe();
        sign_edit_probe_index_ = 0;
        sign_front_written_ = false;
        sign_back_written_ = false;
        sign_active_face_front_ = true;
        sign_face_includes_waxed_ = false;
        sign_target_ready_at_ = {};
        sign_verify_poll_at_ = {};
        sign_verify_deadline_ = {};
        sign_last_verification_error_.clear();
        if (!sign_pending_record_) {
            finishSignWrite();
            return;
        }
        std::string validation_error;
        if (!SignBlockActorPacketSender::validate(*sign_pending_record_, &validation_error)) {
            pauseSignWrite(validation_error.empty()
                ? "next sign spool record is invalid" : validation_error);
            return;
        }
        const int32_t next_cell_x = deferredCellCoordinate(
            sign_pending_record_->x, kSignCellSpanBlocks);
        const int32_t next_cell_y = deferredCellCoordinate(
            sign_pending_record_->y, kSignCellSpanBlocks);
        const int32_t next_cell_z = deferredCellCoordinate(
            sign_pending_record_->z, kSignCellSpanBlocks);
        const bool changed_cell = next_cell_x != sign_cell_x_ ||
            next_cell_y != sign_cell_y_ || next_cell_z != sign_cell_z_;
        sign_cell_x_ = next_cell_x;
        sign_cell_y_ = next_cell_y;
        sign_cell_z_ = next_cell_z;
        sign_cell_bounds_ = deferredCellBounds(
            sign_cell_x_, sign_cell_y_, sign_cell_z_, kSignCellSpanBlocks);
        if (!sign_cell_bounds_.isValid()) {
            pauseSignWrite("sign cell bounds are invalid");
            return;
        }
        if (changed_cell) releaseLoadedRegion(true);
        stage_ = ExecuteStage::SignPrepare;
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "restoring sign data (" + std::to_string(sign_cursor_) + "/" +
            std::to_string(sign_record_count_) + ")";
        return;
    }
    sign_last_verification_error_ = mismatch.empty()
        ? "sign data has not reached the server" : mismatch;
    if (now < sign_verify_deadline_) return;
    retryOrPauseSignWrite(sign_last_verification_error_);
}

void BuildImportRuntime::resetContainerRuntime() {
    container_item_reader_.reset();
    container_item_pending_record_.reset();
    container_item_state_path_.clear();
    container_item_record_count_ = 0;
    container_item_cursor_ = 0;
    container_item_persisted_cursor_ = 0;
    container_item_cell_x_ = 0;
    container_item_cell_y_ = 0;
    container_item_cell_z_ = 0;
    container_item_cell_bounds_ = {};
    container_item_target_x_ = 0;
    container_item_target_y_ = 0;
    container_item_target_z_ = 0;
    container_item_target_retries_ = 0;
    container_item_pending_uuid_.clear();
    container_item_cell_ready_at_ = {};
    container_item_next_target_ready_at_ = {};
    container_item_ack_poll_at_ = {};
    container_item_ack_deadline_ = {};
    container_item_writer_active_ = false;
    container_item_writer_completed_ = false;
}

bool BuildImportRuntime::persistContainerCursor(bool force, std::string* error) {
    if (!container_item_writer_active_ || container_item_state_path_.empty()) {
        if (error) *error = "container-item writer state is not initialized";
        return false;
    }
    if (!force && container_item_cursor_ == container_item_persisted_cursor_ &&
        commandBlockStateExists(container_item_state_path_)) {
        return true;
    }
    if (!saveDeferredDataState(container_item_state_path_, container_item_record_count_,
                               container_item_cursor_, error)) {
        return false;
    }
    container_item_persisted_cursor_ = container_item_cursor_;
    return true;
}

bool BuildImportRuntime::beginContainerWrite(std::string* error) {
    if (container_item_writer_active_) return true;
    if (container_item_writer_completed_) return false;
    const std::string spool_path = spool_directory_ + "/" +
        ContainerItemSpoolWriter::kFileName;
    if (!commandBlockStateExists(spool_path)) return false;
    auto reader = std::make_unique<ContainerItemSpoolReader>(spool_path);
    if (!reader->valid()) {
        if (error) *error = "cannot open container-item spool";
        return false;
    }
    const std::string state_path = spool_directory_ + "/" + kContainerItemStateFileName;
    uint64_t cursor = 0;
    if (!loadDeferredDataState(state_path, reader->recordCount(), &cursor, error) ||
        !reader->seekRecord(cursor)) {
        if (error && error->empty()) *error = "cannot seek container-item spool cursor";
        return false;
    }
    container_item_state_path_ = state_path;
    container_item_record_count_ = reader->recordCount();
    container_item_cursor_ = cursor;
    container_item_persisted_cursor_ = cursor;
    container_item_reader_ = std::move(reader);
    container_item_pending_record_ = container_item_reader_->next();
    if (!container_item_pending_record_ && container_item_reader_->failed()) {
        if (error) *error = "container-item spool is corrupt";
        resetContainerRuntime();
        return false;
    }
    container_item_writer_active_ = true;
    container_item_writer_completed_ = false;
    if (!persistContainerCursor(false, error)) {
        resetContainerRuntime();
        return false;
    }
    if (container_item_pending_record_) {
        const ContainerItemRecord& record = *container_item_pending_record_;
        container_item_target_x_ = record.x;
        container_item_target_y_ = record.y;
        container_item_target_z_ = record.z;
        container_item_cell_x_ = deferredCellCoordinate(record.x, kContainerItemCellSpanBlocks);
        container_item_cell_y_ = deferredCellCoordinate(record.y, kContainerItemCellSpanBlocks);
        container_item_cell_z_ = deferredCellCoordinate(record.z, kContainerItemCellSpanBlocks);
        container_item_cell_bounds_ = deferredCellBounds(
            container_item_cell_x_, container_item_cell_y_, container_item_cell_z_,
            kContainerItemCellSpanBlocks);
        if (!container_item_cell_bounds_.isValid()) {
            if (error) *error = "container-item cell bounds are invalid";
            resetContainerRuntime();
            return false;
        }
        releaseLoadedRegion(true);
        stage_ = ExecuteStage::ContainerPrepare;
    } else {
        stage_ = ExecuteStage::ContainerWrite;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "restoring container items (" + std::to_string(container_item_cursor_) +
            "/" + std::to_string(container_item_record_count_) + ")";
    }
    LOGI("[container-item] deferred writer started cursor=%llu total=%llu",
         static_cast<unsigned long long>(container_item_cursor_),
         static_cast<unsigned long long>(container_item_record_count_));
    return true;
}

void BuildImportRuntime::pauseContainerWrite(const std::string& detail) {
    std::string state_error;
    const bool persisted = container_item_writer_active_ &&
        persistContainerCursor(true, &state_error);
    container_item_writer_active_ = false;
    container_item_reader_.reset();
    container_item_pending_record_.reset();
    container_item_pending_uuid_.clear();
    container_item_next_target_ready_at_ = {};
    std::string reason = "container-item write paused: " + detail;
    if (!persisted && !state_error.empty()) {
        reason += "; progress state write failed: " + state_error;
    }
    pauseFinalVerification(reason);
}

void BuildImportRuntime::finishContainerWrite() {
    if (!container_item_writer_active_) return;
    std::string state_error;
    if (!persistContainerCursor(true, &state_error)) {
        pauseContainerWrite(state_error.empty()
            ? "cannot persist final container-item cursor" : state_error);
        return;
    }
    if (container_item_cursor_ != container_item_record_count_) {
        pauseContainerWrite("container-item spool ended before every slot was written");
        return;
    }
    container_item_writer_active_ = false;
    container_item_reader_.reset();
    container_item_pending_record_.reset();
    container_item_pending_uuid_.clear();
    container_item_next_target_ready_at_ = {};
    container_item_writer_completed_ = true;
    LOGI("[container-item] deferred writer complete records=%llu",
         static_cast<unsigned long long>(container_item_record_count_));
    finishFinalVerification(true);
}

void BuildImportRuntime::tickContainerWrite(std::chrono::steady_clock::time_point now) {
    if (!container_item_writer_active_) return;
    if (stage_ == ExecuteStage::ContainerPrepare) {
        if (container_item_next_target_ready_at_.time_since_epoch().count() &&
            now < container_item_next_target_ready_at_) {
            return;
        }
        container_item_next_target_ready_at_ = {};
        if (!pending_cleanup_commands_.empty() || !cleanup_barrier_uuids_.empty() ||
            (region_add_ready_at_.time_since_epoch().count() && now < region_add_ready_at_)) {
            return;
        }
        if (!container_item_pending_record_) {
            stage_ = ExecuteStage::ContainerWrite;
            return;
        }
        const bool reuse_loaded_cell = loaded_region_.has_value() &&
            containsBounds(loaded_region_probe_bounds_, container_item_cell_bounds_);
        const std::vector<std::string> commands = containerItemPrepareCommands(
            container_item_cell_bounds_, container_item_target_x_, container_item_target_y_,
            container_item_target_z_, !reuse_loaded_cell);
        if (commands.empty()) {
            pauseContainerWrite("container-item cell bounds are invalid");
            return;
        }
        size_t sent_count = 0;
        if (!executeCommands(commands, &sent_count) || sent_count != commands.size()) {
            pauseContainerWrite("cannot teleport above container target");
            return;
        }
        NativeWorldAccess::invalidateLocalPlayerBlockPositionCache();
        if (!reuse_loaded_cell) {
            loaded_region_ = ChunkCoord{container_item_cell_x_, container_item_cell_z_};
            loaded_region_cleanup_command_ = "/tickingarea remove infinitecz_build";
            loaded_region_probe_bounds_ = container_item_cell_bounds_;
            loaded_region_started_at_ = now;
            loaded_region_fully_confirmed_ = false;
            confirmed_region_chunks_.clear();
        }
        resetServerChunkProbe();
        // A teleport and ticking-area command are asynchronous from the
        // client runtime's point of view, even when this loading cell was
        // already used by the preceding container. Give the server a fixed
        // settling window before issuing the first /replaceitem command.
        container_item_cell_ready_at_ = now + kDeferredCellMinimumWait;
        stage_ = ExecuteStage::ContainerWait;
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "waiting above container at " + std::to_string(container_item_target_x_) +
            "," + std::to_string(container_item_target_y_) + "," +
            std::to_string(container_item_target_z_);
        return;
    }

    if (stage_ == ExecuteStage::ContainerWait) {
        if (now < container_item_cell_ready_at_) return;

        // The NetEase client position API can remain stale after a packet-side
        // teleport, and the native block-name layout is version-sensitive.
        // Neither is a reliable precondition for a server command. The
        // destination is already fixed by the imported record; let the
        // tracked /replaceitem result decide whether the target is writable.
        stage_ = ExecuteStage::ContainerWrite;
        return;
    }

    if (stage_ != ExecuteStage::ContainerWrite) return;
    if (!container_item_pending_record_) {
        if (container_item_reader_ && container_item_reader_->failed()) {
            pauseContainerWrite("container-item spool became corrupt");
        } else {
            finishContainerWrite();
        }
        return;
    }
    const ContainerItemRecord& record = *container_item_pending_record_;
    if (record.x != container_item_target_x_ || record.y != container_item_target_y_ ||
        record.z != container_item_target_z_) {
        const int32_t next_cell_x = deferredCellCoordinate(record.x, kContainerItemCellSpanBlocks);
        const int32_t next_cell_y = deferredCellCoordinate(record.y, kContainerItemCellSpanBlocks);
        const int32_t next_cell_z = deferredCellCoordinate(record.z, kContainerItemCellSpanBlocks);
        const bool changed_cell = next_cell_x != container_item_cell_x_ ||
            next_cell_y != container_item_cell_y_ || next_cell_z != container_item_cell_z_;
        container_item_target_x_ = record.x;
        container_item_target_y_ = record.y;
        container_item_target_z_ = record.z;
        container_item_cell_x_ = next_cell_x;
        container_item_cell_y_ = next_cell_y;
        container_item_cell_z_ = next_cell_z;
        container_item_cell_bounds_ = deferredCellBounds(
            next_cell_x, next_cell_y, next_cell_z, kContainerItemCellSpanBlocks);
        container_item_target_retries_ = 0;
        resetServerChunkProbe();
        if (changed_cell) releaseLoadedRegion(true);
        stage_ = ExecuteStage::ContainerPrepare;
        return;
    }
    if (!container_item_pending_uuid_.empty()) {
        if (now < container_item_ack_poll_at_) return;
        const RpcResultState result = pollTrackedCommands({container_item_pending_uuid_});
        if (result == RpcResultState::Pending && now < container_item_ack_deadline_) {
            container_item_ack_poll_at_ = now + kRpcPollInterval;
            return;
        }
        if (result != RpcResultState::Accepted) {
            // Replaying the same slot write is idempotent. Retry from the
            // prepare stage in case the teleport or target chunk had not yet
            // settled when the first packet reached the server.
            container_item_pending_uuid_.clear();
            container_item_ack_poll_at_ = {};
            container_item_ack_deadline_ = {};
            if (++container_item_target_retries_ <= kDeferredTargetRetryLimit) {
                resetServerChunkProbe();
                releaseLoadedRegion(true);
                stage_ = ExecuteStage::ContainerPrepare;
                return;
            }
            pauseContainerWrite(result == RpcResultState::Rejected
                ? "server rejected a container replaceitem command after retries"
                : "container replaceitem acknowledgement timed out after retries");
            return;
        }
        container_item_target_retries_ = 0;
        ++container_item_cursor_;
        container_item_pending_uuid_.clear();
        container_item_ack_poll_at_ = {};
        container_item_ack_deadline_ = {};
        std::string state_error;
        if (!persistContainerCursor(true, &state_error)) {
            pauseContainerWrite(state_error.empty()
                ? "cannot persist container-item cursor" : state_error);
            return;
        }
        container_item_pending_record_ = container_item_reader_->next();
        if (!container_item_pending_record_ && container_item_reader_->failed()) {
            pauseContainerWrite("container-item spool became corrupt");
        } else if (container_item_pending_record_ &&
                   (container_item_pending_record_->x != container_item_target_x_ ||
                    container_item_pending_record_->y != container_item_target_y_ ||
                    container_item_pending_record_->z != container_item_target_z_)) {
            // The just-acknowledged record was the final populated slot in
            // this container. Advance to the next target immediately; the
            // acknowledgement above is already the required ordering barrier.
            container_item_next_target_ready_at_ = now + kContainerInterTargetDelay;
        }
        return;
    }
    const std::string uuid = nextRpcUuid();
    size_t sent_count = 0;
    if (!executeTrackedCommands({formatContainerReplaceItemCommand(record)}, {uuid}, &sent_count) ||
        sent_count != 1U) {
        pauseContainerWrite("cannot send container replaceitem command");
        return;
    }
    container_item_pending_uuid_ = uuid;
    container_item_ack_poll_at_ = now + kRpcPollInterval;
    container_item_ack_deadline_ = now + kRpcBarrierTimeout;
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = "restoring container items (" + std::to_string(container_item_cursor_) +
        "/" + std::to_string(container_item_record_count_) + ")";
}

void BuildImportRuntime::resetEntityRuntime() {
    entity_reader_.reset();
    entity_pending_record_.reset();
    entity_state_path_.clear();
    entity_record_count_ = 0;
    entity_cursor_ = 0;
    entity_persisted_cursor_ = 0;
    entity_cell_x_ = 0;
    entity_cell_y_ = 0;
    entity_cell_z_ = 0;
    entity_cell_bounds_ = {};
    entity_cell_load_retries_ = 0;
    entity_pending_uuid_.clear();
    entity_ack_poll_at_ = {};
    entity_ack_deadline_ = {};
    entity_cell_ready_at_ = {};
    entity_cell_deadline_ = {};
    entity_writer_active_ = false;
    entity_writer_completed_ = false;
}

bool BuildImportRuntime::persistEntityCursor(bool force, std::string* error) {
    if (!entity_writer_active_ || entity_state_path_.empty()) {
        if (error) *error = "entity writer state is not initialized";
        return false;
    }
    if (!force && entity_cursor_ == entity_persisted_cursor_ &&
        commandBlockStateExists(entity_state_path_)) {
        return true;
    }
    if (!saveDeferredDataState(entity_state_path_, entity_record_count_, entity_cursor_, error)) {
        return false;
    }
    entity_persisted_cursor_ = entity_cursor_;
    return true;
}

bool BuildImportRuntime::beginEntityWrite(std::string* error) {
    if (entity_writer_active_) return true;
    if (entity_writer_completed_) return false;
    const std::string spool_path = spool_directory_ + "/" + EntitySpoolWriter::kFileName;
    if (!commandBlockStateExists(spool_path)) return false;
    auto reader = std::make_unique<EntitySpoolReader>(spool_path);
    if (!reader->valid()) {
        if (error) *error = "cannot open entity spool";
        return false;
    }
    const std::string state_path = spool_directory_ + "/" + kEntityStateFileName;
    uint64_t cursor = 0;
    if (!loadDeferredDataState(state_path, reader->recordCount(), &cursor, error) ||
        !reader->seekRecord(cursor)) {
        if (error && error->empty()) *error = "cannot seek entity spool cursor";
        return false;
    }
    entity_state_path_ = state_path;
    entity_record_count_ = reader->recordCount();
    entity_cursor_ = cursor;
    entity_persisted_cursor_ = cursor;
    entity_reader_ = std::move(reader);
    entity_pending_record_ = entity_reader_->next();
    if (!entity_pending_record_ && entity_reader_->failed()) {
        if (error) *error = "entity spool is corrupt";
        resetEntityRuntime();
        return false;
    }
    entity_writer_active_ = true;
    entity_writer_completed_ = false;
    if (!persistEntityCursor(false, error)) {
        resetEntityRuntime();
        return false;
    }
    if (entity_pending_record_) {
        const EntityRecord& record = *entity_pending_record_;
        const int32_t x = clampCommandBlockCellToInt32(static_cast<int64_t>(std::floor(record.x)));
        const int32_t y = clampCommandBlockCellToInt32(static_cast<int64_t>(std::floor(record.y)));
        const int32_t z = clampCommandBlockCellToInt32(static_cast<int64_t>(std::floor(record.z)));
        entity_cell_x_ = deferredCellCoordinate(x, kEntityCellSpanBlocks);
        entity_cell_y_ = deferredCellCoordinate(y, kEntityCellSpanBlocks);
        entity_cell_z_ = deferredCellCoordinate(z, kEntityCellSpanBlocks);
        entity_cell_bounds_ = deferredCellBounds(entity_cell_x_, entity_cell_y_, entity_cell_z_,
                                                  kEntityCellSpanBlocks);
        if (!entity_cell_bounds_.isValid()) {
            if (error) *error = "entity cell bounds are invalid";
            resetEntityRuntime();
            return false;
        }
        releaseLoadedRegion(true);
        stage_ = ExecuteStage::EntityPrepare;
    } else {
        stage_ = ExecuteStage::EntityWrite;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "summoning entities (" + std::to_string(entity_cursor_) + "/" +
            std::to_string(entity_record_count_) + ")";
    }
    LOGI("[entity] deferred writer started cursor=%llu total=%llu",
         static_cast<unsigned long long>(entity_cursor_),
         static_cast<unsigned long long>(entity_record_count_));
    return true;
}

void BuildImportRuntime::pauseEntityWrite(const std::string& detail) {
    std::string state_error;
    const bool persisted = entity_writer_active_ && persistEntityCursor(true, &state_error);
    entity_writer_active_ = false;
    entity_reader_.reset();
    entity_pending_record_.reset();
    entity_pending_uuid_.clear();
    std::string reason = "entity write paused: " + detail;
    if (!persisted && !state_error.empty()) reason += "; progress state write failed: " + state_error;
    pauseFinalVerification(reason);
}

void BuildImportRuntime::finishEntityWrite() {
    if (!entity_writer_active_) return;
    std::string state_error;
    if (!persistEntityCursor(true, &state_error)) {
        pauseEntityWrite(state_error.empty() ? "cannot persist final entity cursor" : state_error);
        return;
    }
    if (entity_cursor_ != entity_record_count_) {
        pauseEntityWrite("entity spool ended before every record was written");
        return;
    }
    entity_writer_active_ = false;
    entity_reader_.reset();
    entity_pending_record_.reset();
    entity_pending_uuid_.clear();
    entity_writer_completed_ = true;
    // Unlike a replaceitem write, summon is not perfectly idempotent across a
    // process crash. Keep the EOF state file until full import cleanup so a
    // later resume cannot summon a completed record again.
    LOGI("[entity] deferred writer complete records=%llu",
         static_cast<unsigned long long>(entity_record_count_));
    finishFinalVerification(true);
}

void BuildImportRuntime::tickEntityWrite(std::chrono::steady_clock::time_point now) {
    if (!entity_writer_active_) return;
    if (stage_ == ExecuteStage::EntityPrepare) {
        if (!pending_cleanup_commands_.empty() || !cleanup_barrier_uuids_.empty() ||
            (region_add_ready_at_.time_since_epoch().count() && now < region_add_ready_at_)) {
            return;
        }
        if (!entity_pending_record_) {
            stage_ = ExecuteStage::EntityWrite;
            return;
        }
        const std::vector<std::string> commands = deferredCellPrepareCommands(entity_cell_bounds_);
        if (commands.empty()) {
            pauseEntityWrite("entity cell bounds are invalid");
            return;
        }
        size_t sent_count = 0;
        if (!executeCommands(commands, &sent_count) || sent_count != commands.size()) {
            pauseEntityWrite("cannot teleport to entity cell");
            return;
        }
        const BuildImportRuntimeMetadata metadata = controller_.runtimeMetadata();
        loaded_region_ = ChunkCoord{entity_cell_x_, entity_cell_z_};
        loaded_region_cleanup_command_ = "/tickingarea remove infinitecz_build";
        loaded_region_probe_bounds_ = entity_cell_bounds_;
        loaded_region_started_at_ = now;
        loaded_region_fully_confirmed_ = false;
        confirmed_region_chunks_.clear();
        resetServerChunkProbe();
        entity_cell_ready_at_ = now + kDeferredCellMinimumWait;
        const auto configured_wait = std::chrono::milliseconds(
            static_cast<int64_t>(std::max<int32_t>(20, metadata.chunk_wait_ticks)) * 50);
        entity_cell_deadline_ = now + std::max(std::chrono::milliseconds(10000),
            configured_wait + std::chrono::milliseconds(2000));
        stage_ = ExecuteStage::EntityWait;
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "waiting for entity cell " + std::to_string(entity_cell_x_) + "," +
            std::to_string(entity_cell_y_) + "," + std::to_string(entity_cell_z_) + " to load";
        return;
    }
    if (stage_ == ExecuteStage::EntityWait) {
        std::string load_error;
        bool ready = false;
        if (now >= entity_cell_ready_at_) {
            const ChunkProbeState probe = pollServerChunkProbe(entity_cell_bounds_, now, &load_error);
            ready = probe == ChunkProbeState::Ready ||
                (probe == ChunkProbeState::TransportFailure && nativeChunksReadable(entity_cell_bounds_));
        }
        if (ready && entity_pending_record_ &&
            localPlayerIsNearDeferredTarget(
                clampCommandBlockCellToInt32(static_cast<int64_t>(std::floor(entity_pending_record_->x))),
                clampCommandBlockCellToInt32(static_cast<int64_t>(std::floor(entity_pending_record_->y))),
                clampCommandBlockCellToInt32(static_cast<int64_t>(std::floor(entity_pending_record_->z))), 24)) {
            stage_ = ExecuteStage::EntityWrite;
            return;
        }
        if (now < entity_cell_deadline_) return;
        if (++entity_cell_load_retries_ <= kDeferredCellLoadRetryLimit) {
            resetServerChunkProbe();
            releaseLoadedRegion(true);
            stage_ = ExecuteStage::EntityPrepare;
            return;
        }
        pauseEntityWrite(load_error.empty() ? "entity cell did not load after retries" : load_error);
        return;
    }
    if (stage_ != ExecuteStage::EntityWrite) return;
    if (!entity_pending_record_) {
        if (entity_reader_ && entity_reader_->failed()) pauseEntityWrite("entity spool became corrupt");
        else finishEntityWrite();
        return;
    }
    const EntityRecord& record = *entity_pending_record_;
    const int32_t x = clampCommandBlockCellToInt32(static_cast<int64_t>(std::floor(record.x)));
    const int32_t y = clampCommandBlockCellToInt32(static_cast<int64_t>(std::floor(record.y)));
    const int32_t z = clampCommandBlockCellToInt32(static_cast<int64_t>(std::floor(record.z)));
    if (!sameDeferredCell(x, y, z, entity_cell_x_, entity_cell_y_, entity_cell_z_,
                          kEntityCellSpanBlocks)) {
        entity_cell_x_ = deferredCellCoordinate(x, kEntityCellSpanBlocks);
        entity_cell_y_ = deferredCellCoordinate(y, kEntityCellSpanBlocks);
        entity_cell_z_ = deferredCellCoordinate(z, kEntityCellSpanBlocks);
        entity_cell_bounds_ = deferredCellBounds(entity_cell_x_, entity_cell_y_, entity_cell_z_,
                                                  kEntityCellSpanBlocks);
        entity_cell_load_retries_ = 0;
        releaseLoadedRegion(true);
        stage_ = ExecuteStage::EntityPrepare;
        return;
    }
    if (!localPlayerIsNearDeferredTarget(x, y, z, 24)) {
        releaseLoadedRegion(true);
        stage_ = ExecuteStage::EntityPrepare;
        return;
    }
    if (!entity_pending_uuid_.empty()) {
        if (now < entity_ack_poll_at_) return;
        const RpcResultState result = pollTrackedCommands({entity_pending_uuid_});
        if (result == RpcResultState::Pending && now < entity_ack_deadline_) {
            entity_ack_poll_at_ = now + kRpcPollInterval;
            return;
        }
        if (result != RpcResultState::Accepted) {
            pauseEntityWrite(result == RpcResultState::Rejected
                ? "server rejected an entity summon command"
                : "entity summon acknowledgement timed out");
            return;
        }
        ++entity_cursor_;
        entity_pending_uuid_.clear();
        entity_ack_poll_at_ = {};
        entity_ack_deadline_ = {};
        std::string state_error;
        if (!persistEntityCursor(true, &state_error)) {
            pauseEntityWrite(state_error.empty() ? "cannot persist entity cursor" : state_error);
            return;
        }
        entity_pending_record_ = entity_reader_->next();
        if (!entity_pending_record_ && entity_reader_->failed()) {
            pauseEntityWrite("entity spool became corrupt");
        }
        return;
    }
    const std::string uuid = nextRpcUuid();
    size_t sent_count = 0;
    if (!executeTrackedCommands({entitySummonCommand(record)}, {uuid}, &sent_count) ||
        sent_count != 1U) {
        pauseEntityWrite("cannot send entity summon command");
        return;
    }
    entity_pending_uuid_ = uuid;
    entity_ack_poll_at_ = now + kRpcPollInterval;
    entity_ack_deadline_ = now + kRpcBarrierTimeout;
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = "summoning entities (" + std::to_string(entity_cursor_) + "/" +
        std::to_string(entity_record_count_) + ")";
}

void BuildImportRuntime::resetMapCreationRuntime() {
    ArmMapTextureDiagnostics(false);
    if (map_storage_chest_client_sync_token_ != 0U) {
        CancelMapChestClientSyncWindow(map_storage_chest_client_sync_token_);
    }
    map_storage_chest_client_sync_ticket_ = 0U;
    map_storage_chest_client_sync_token_ = 0U;
    map_storage_chest_client_sync_request_id_ = 0;
    map_storage_chest_client_sync_confirmed_ = false;
    map_storage_chest_client_sync_deadline_ = {};
    map_storage_chest_client_sync_error_.clear();
    if (map_storage_chest_capture_) map_storage_chest_stop_requested_ = true;
    if (map_storage_anvil_window_) map_storage_anvil_stop_requested_ = true;
    map_inventory_transfer_.cancel();
    map_storage_renamed_transfer_network_id_ = 0;
    map_storage_renamed_transfer_uuid_ = -1;
    map_storage_renamed_transfer_cursor_ = 0;
    map_creation_requested_ = false;
    map_creation_active_ = false;
    map_creation_completed_ = false;
    map_storage_pipeline_active_ = false;
    map_source_bounds_ = {};
    map_tile_columns_ = 0;
    map_tile_rows_ = 0;
    map_tile_count_ = 0;
    map_tile_cursor_ = 0;
    map_tile_persisted_cursor_ = 0;
    map_surface_y_ = 0;
    map_target_x_ = 0;
    map_target_z_ = 0;
    map_slot_attempts_ = 0;
    map_state_path_.clear();
    map_target_ready_at_ = {};
    map_target_deadline_ = {};
    map_slot_retry_at_ = {};
    map_inventory_deadline_ = {};
    map_settle_ready_at_ = {};
    map_settle_log_at_ = {};
    map_texture_log_at_ = {};
    map_use_deadline_ = {};
    map_use_hold_ready_at_ = {};
    map_use_hotbar_slot_ = -1;
    map_use_network_stack_id_ = 0;
    map_use_empty_count_before_ = 0;
    map_use_filled_total_before_ = 0;
    map_use_filled_network_ids_before_.fill(0);
    map_use_new_filled_network_id_ = 0;
    map_use_new_filled_uuid_ = -1;
    map_use_held_filled_network_id_ = 0;
    map_use_texture_sequence_before_ = 0U;
    map_use_item_confirmed_ = false;
    map_storage_driver_ = MapStoragePipelineDriver{};
    map_storage_log_at_ = {};
    map_storage_prior_window_wait_started_at_ = {};
    map_storage_chest_terminal_failure_ = false;
    map_storage_rpc_ack_uuid_.clear();
    map_storage_rpc_ack_ = MapPairCommandAck::Pending;
    map_storage_rpc_poll_uuid_.clear();
    map_storage_rpc_poll_deadline_ = {};
    map_storage_travel_target_ = {};
    map_storage_travel_pending_ = false;
    map_storage_travel_ready_at_ = {};
    map_storage_travel_deadline_ = {};
    map_storage_placement_target_ = {};
    map_storage_placement_floor_ = {};
    map_storage_placement_pending_ = false;
    map_storage_placement_ready_ = false;
    map_storage_placement_ready_at_ = {};
    map_storage_placement_arrival_at_ = {};
    map_storage_placement_deadline_ = {};
    map_storage_placement_log_at_ = {};
    map_blank_supply_active_uuid_.clear();
    map_blank_supply_ack_received_ = false;
    map_blank_supply_poll_at_ = {};
    map_blank_supply_deadline_ = {};
}

bool BuildImportRuntime::beginMapCreation(std::string* error) {
    if (error) error->clear();
    if (!map_creation_requested_ || map_creation_completed_) return false;
    if (map_creation_active_) return true;
    map_storage_travel_pending_ = false;
    map_storage_travel_ready_at_ = {};
    map_storage_travel_deadline_ = {};
    map_storage_placement_target_ = {};
    map_storage_placement_floor_ = {};
    map_storage_placement_pending_ = false;
    map_storage_placement_ready_ = false;
    map_storage_placement_ready_at_ = {};
    map_storage_placement_arrival_at_ = {};
    map_storage_placement_deadline_ = {};
    map_storage_placement_log_at_ = {};
    if (map_storage_chest_terminal_failure_ ||
        map_storage_anvil_terminal_failure_) {
        if (error) *error = "previous map container close failed; restart the game before another automatic map task";
        return false;
    }
    if (map_storage_chest_capture_ || map_storage_anvil_window_) {
        // A completed window can retain a process-local adapter while the
        // stock UI's native lifecycle is finishing. Game ticks continue to
        // service both adapters. Do not call this an open server window, and
        // do not discard an old anvil pointer merely to bypass probe rearm.
        const auto now = std::chrono::steady_clock::now();
        if (map_storage_prior_window_wait_started_at_ ==
                std::chrono::steady_clock::time_point{}) {
            map_storage_prior_window_wait_started_at_ = now;
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "waiting for the previous map container lifecycle to settle";
        }
        if (now - map_storage_prior_window_wait_started_at_ <
                std::chrono::seconds(10)) {
            return false; // Retry from final verification on the next tick.
        }
        if (error) {
            *error = map_storage_anvil_window_ &&
                     map_storage_anvil_window_->state() ==
                         MapVisibleAnvilWindowState::Completed &&
                     map_storage_anvil_handoff_ready_
                ? "previous anvil UI closed, but its native lifecycle probe did not retire; restart the game before another automatic map task"
                : "previous map container close did not settle; restart the game before another automatic map task";
        }
        return false;
    }
    map_storage_prior_window_wait_started_at_ = {};

    uint64_t columns = 0;
    uint64_t rows = 0;
    uint64_t count = 0;
    if (map_state_path_.empty() ||
        !automaticMapPlan(map_source_bounds_, &columns, &rows, &count, error) ||
        columns != map_tile_columns_ || rows != map_tile_rows_ ||
        count != map_tile_count_ || map_tile_cursor_ > count) {
        if (error && error->empty()) *error = "automatic map plan changed during import";
        return false;
    }
    const bool storage_enabled = mapStoragePipelineEnabled();
    if (map_storage_pipeline_active_ && !storage_enabled) {
        if (error) *error = "automatic map storage is unavailable in this build";
        return false;
    }
    if (storage_enabled &&
        !IsMapNativeAnvilAutomaticResultDispatchVerified()) {
        if (error) *error =
            "automatic map storage cannot verify anvil result dispatch";
        return false;
    }
    if (!map_storage_pipeline_active_) {
        map_storage_pipeline_active_ = storage_enabled;
    }
    if (map_storage_pipeline_active_) {
        MapStorageRuntimeSnapshot storage;
        if (!loadMapStorageRuntimeSnapshot(
                map_state_path_, current_world_, map_source_bounds_,
                map_tile_columns_, map_tile_rows_, map_tile_count_,
                map_tile_cursor_, map_use_new_filled_uuid_, &storage, error)) {
            return false;
        }
        // Any storage sidecar, or a marker left after a durable cursor
        // advance, must enter the journal driver. The legacy inventory-only
        // recovery below may no longer assume the map is in the backpack.
        if (storage.has_storage_journals ||
            (storage.input.has_pending_map_use_marker &&
             storage.input.pending_map_use_cursor < map_tile_cursor_)) {
            releaseLoadedRegion(true);
            map_creation_active_ = true;
            stage_ = ExecuteStage::MapStorageHandoff;
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "reconciling persisted map storage (" +
                std::to_string(map_tile_cursor_) + "/" +
                std::to_string(map_tile_count_) + ")";
            return true;
        }
    } else if (!requireNoPendingMapStorage(
            map_state_path_, current_world_, map_source_bounds_,
            map_tile_count_, map_tile_cursor_, error)) {
        return false;
    }
    const std::string pending_path = mapUsePendingPath(map_state_path_);
    std::optional<MapUsePendingDiskV1> recovered_use;
    int32_t recovered_filled_network_id = 0;
    int64_t recovered_filled_uuid = -1;
    if (commandBlockStateExists(pending_path)) {
        MapUsePendingDiskV1 pending;
        if (!loadMapUsePending(map_state_path_, &pending, error)) return false;
        if (pending.tile_cursor > map_tile_cursor_) {
            if (error) *error = "pending map-use cursor is ahead of saved progress";
            return false;
        }
        if (pending.tile_cursor == map_tile_cursor_) {
            // A pause or crash between ItemUse and durable cursor commit is
            // ambiguous. Reconcile the original stack and the newly stored
            // filled map; never blindly resend an empty-map ItemUse here.
            MapInventorySnapshot inventory;
            std::string inventory_error;
            if (!ReadMapInventorySnapshot(&inventory, &inventory_error) ||
                !inventory.ready || !inventory.network_ready) {
                if (error) *error = inventory_error.empty()
                    ? "pending map use cannot be reconciled with current inventory"
                    : inventory_error;
                return false;
            }
            uint32_t filled_total = 0U;
            for (const MapInventorySlot& item : inventory.slots) {
                if (item.filled_map) filled_total += item.count;
            }
            // An ItemUse packet does not perform the client's local item-use
            // prediction. With a stack of blank maps, the old slot can still
            // show its original count even after the new filled map arrives.
            // The unique new native stack is the useful reconciliation proof;
            // the pending marker prevents another ItemUse on resume.
            if (filled_total <= pending.filled_map_total ||
                !findNewFilledMap(inventory, pending.filled_map_network_ids_before,
                                  &recovered_filled_network_id)) {
                if (error) *error =
                    "previous map use lacks a unique new filled map; inspect inventory before resuming";
                return false;
            }
            // The UUID can become readable only after the map is held. It is
            // checked together with the target texture before cursor commit.
            findFilledMapUuid(inventory, recovered_filled_network_id,
                              &recovered_filled_uuid);
            recovered_use = pending;
        }
        if (!recovered_use && std::remove(pending_path.c_str()) != 0) {
            if (error) *error = "cannot remove reconciled map-use state";
            return false;
        }
    }
    if (map_surface_y_ == std::numeric_limits<int32_t>::max()) {
        if (error) *error = "pixel-art surface is too high to create maps above it";
        return false;
    }
    if (map_tile_cursor_ == map_tile_count_) {
        // Keep the completed sidecar until controller_.finishVerification()
        // commits the whole import. A crash in that gap must still be
        // restorable when the checkpoint says maps were requested.
        map_creation_completed_ = true;
        return false;
    }

    releaseLoadedRegion(true);
    map_creation_active_ = true;
    ArmMapTextureDiagnostics(true);
    map_slot_attempts_ = 0;
    map_target_ready_at_ = {};
    map_target_deadline_ = {};
    map_slot_retry_at_ = {};
    map_inventory_deadline_ = {};
    map_settle_ready_at_ = {};
    map_settle_log_at_ = {};
    map_texture_log_at_ = {};
    map_use_deadline_ = {};
    map_use_hold_ready_at_ = {};
    map_use_hotbar_slot_ = -1;
    map_use_network_stack_id_ = 0;
    map_use_empty_count_before_ = 0;
    map_use_filled_total_before_ = 0;
    map_use_filled_network_ids_before_.fill(0);
    map_use_new_filled_network_id_ = 0;
    map_use_new_filled_uuid_ = -1;
    map_use_held_filled_network_id_ = 0;
    map_use_texture_sequence_before_ = 0U;
    map_use_item_confirmed_ = false;
    map_blank_supply_active_uuid_.clear();
    map_blank_supply_ack_received_ = false;
    map_blank_supply_poll_at_ = {};
    map_blank_supply_deadline_ = {};
    if (recovered_use) {
        map_target_x_ = recovered_use->target_x;
        map_target_z_ = recovered_use->target_z;
        map_use_hotbar_slot_ = recovered_use->hotbar_slot;
        map_use_network_stack_id_ = recovered_use->network_stack_id;
        map_use_empty_count_before_ = recovered_use->empty_map_count;
        map_use_filled_total_before_ = recovered_use->filled_map_total;
        std::copy(std::begin(recovered_use->filled_map_network_ids_before),
                  std::end(recovered_use->filled_map_network_ids_before),
                  map_use_filled_network_ids_before_.begin());
        // The pending item is identified by its native map_uuid. A completed
        // texture already cached for that exact UUID in this process is valid
        // recovery evidence; after a process restart the cache starts empty
        // and holding the map must receive its pixels again.
        map_use_texture_sequence_before_ = 0U;
        map_use_new_filled_network_id_ = recovered_filled_network_id;
        map_use_new_filled_uuid_ = recovered_filled_uuid;
        map_use_item_confirmed_ = true;
        map_use_deadline_ = std::chrono::steady_clock::now() +
            kMapHoldConfirmationTimeout;
        map_settle_log_at_ = std::chrono::steady_clock::now();
        map_texture_log_at_ = map_settle_log_at_;
        stage_ = ExecuteStage::MapSettle;
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "reconciling the filled map after a paused import (" +
            std::to_string(map_tile_cursor_ + 1U) + "/" +
            std::to_string(map_tile_count_) + ")";
        return true;
    }
    stage_ = ExecuteStage::MapPrepare;
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = "creating maps (" + std::to_string(map_tile_cursor_) + "/" +
        std::to_string(map_tile_count_) + ")";
    return true;
}

void BuildImportRuntime::tickMapCreation(std::chrono::steady_clock::time_point now) {
    if (!map_creation_active_) return;
    if (stage_ == ExecuteStage::MapStorageHandoff) {
        if (!map_storage_pipeline_active_) {
            pauseMapCreation("verified anvil/chest storage handoff is not enabled");
        } else {
            tickMapStorageHandoff();
        }
        return;
    }
    if (map_tile_cursor_ >= map_tile_count_) {
        finishMapCreation();
        return;
    }

    const uint64_t column = map_tile_cursor_ % map_tile_columns_;
    const uint64_t row = map_tile_cursor_ / map_tile_columns_;
    const int64_t target_x = mapCenterForCoordinate(map_source_bounds_.min_x) +
        static_cast<int64_t>(column) * kMapTileSpanBlocks;
    const int64_t target_z = mapCenterForCoordinate(map_source_bounds_.min_z) +
        static_cast<int64_t>(row) * kMapTileSpanBlocks;
    map_target_x_ = static_cast<int32_t>(target_x);
    map_target_z_ = static_cast<int32_t>(target_z);

    if (stage_ == ExecuteStage::MapPrepare) {
        if (!pending_cleanup_commands_.empty() || !cleanup_barrier_uuids_.empty() ||
            (region_add_ready_at_.time_since_epoch().count() && now < region_add_ready_at_)) {
            return;
        }
        const std::vector<std::string> commands = containerItemPrepareCommands(
            map_source_bounds_, map_target_x_, map_surface_y_, map_target_z_, false);
        size_t sent_count = 0;
        if (commands.empty() || !executeCommands(commands, &sent_count) ||
            sent_count != commands.size()) {
            pauseMapCreation("cannot teleport to the next map center");
            return;
        }
        map_target_ready_at_ = now + kMapTargetSettleDelay;
        map_target_deadline_ = now + kMapTargetTimeout;
        stage_ = ExecuteStage::MapWait;
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "creating maps: teleporting to " + std::to_string(map_tile_cursor_ + 1U) +
            "/" + std::to_string(map_tile_count_);
        return;
    }

    if (stage_ == ExecuteStage::MapWait) {
        if (now < map_target_ready_at_) return;
        int32_t player_x = 0;
        int32_t player_y = 0;
        int32_t player_z = 0;
        const bool position_ready = NativeWorldAccess::getLocalPlayerBlockPosition(
            &player_x, &player_y, &player_z) &&
            static_cast<int64_t>(player_x) - map_target_x_ >= -2 &&
            static_cast<int64_t>(player_x) - map_target_x_ <= 2 &&
            static_cast<int64_t>(player_z) - map_target_z_ >= -2 &&
            static_cast<int64_t>(player_z) - map_target_z_ <= 2;
        if (position_ready) {
            map_slot_attempts_ = 0;
            map_slot_retry_at_ = now;
            map_inventory_deadline_ = now + std::chrono::seconds(12);
            stage_ = ExecuteStage::MapSelect;
            return;
        }
        if (now >= map_target_deadline_) {
            pauseMapCreation("map target did not receive the teleport");
        }
        return;
    }

    if (stage_ == ExecuteStage::MapSelect) {
        if (now < map_slot_retry_at_) return;
        MapBlankSupplyIntent supply;
        std::string supply_error;
        const MapBlankSupplyLoad supply_load = LoadMapBlankSupply(
            map_state_path_, &supply, &supply_error);
        if (supply_load == MapBlankSupplyLoad::Unsafe) {
            pauseMapCreation(supply_error.empty()
                ? "pending blank-map supply state is unsafe" : supply_error);
            return;
        }
        if (supply_load == MapBlankSupplyLoad::Loaded) {
            if (!MapBlankSupplyMatches(supply, current_world_.world_id,
                                       current_world_.dimension_id,
                                       map_tile_cursor_, map_tile_count_)) {
                pauseMapCreation("pending blank-map supply belongs to another world or tile");
                return;
            }
            if (map_blank_supply_active_uuid_.empty()) {
                // A recovered intent may already have reached the server.
                // Readback is allowed, but a second /give is not.
                map_blank_supply_active_uuid_ = supply.rpc_uuid;
                map_blank_supply_ack_received_ = false;
                map_blank_supply_poll_at_ = now;
                map_blank_supply_deadline_ = now + std::chrono::seconds(20);
                LOGI("[map-supply] resumed unresolved tile=%llu uuid=%s",
                     static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                     supply.rpc_uuid.c_str());
            } else if (map_blank_supply_active_uuid_ != supply.rpc_uuid) {
                pauseMapCreation("pending blank-map supply UUID changed during observation");
                return;
            }
            MapInventorySnapshot supplied_inventory;
            std::string inventory_error;
            if (ReadMapInventorySnapshot(&supplied_inventory, &inventory_error) &&
                supplied_inventory.ready && supplied_inventory.network_ready) {
                uint32_t empty_count = 0U;
                for (const MapInventorySlot& item : supplied_inventory.slots) {
                    if (item.empty_map) empty_count += item.count;
                }
                if (empty_count > supply.empty_maps_before) {
                    if (!ClearMapBlankSupply(map_state_path_, supply, &supply_error)) {
                        pauseMapCreation(supply_error.empty()
                            ? "blank-map supply readback was confirmed, but its journal could not be cleared"
                            : supply_error);
                        return;
                    }
                    LOGI("[map-supply] confirmed tile=%llu empty_maps=%u uuid=%s",
                         static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                         empty_count, supply.rpc_uuid.c_str());
                    map_blank_supply_active_uuid_.clear();
                    map_blank_supply_ack_received_ = false;
                    map_blank_supply_poll_at_ = {};
                    map_blank_supply_deadline_ = {};
                    map_inventory_transfer_.cancel();
                    map_slot_attempts_ = 0;
                    map_inventory_deadline_ = now + std::chrono::seconds(12);
                    map_slot_retry_at_ = now + kMapSlotRetryDelay;
                    return;
                }
            }
            if (!map_blank_supply_ack_received_ && now >= map_blank_supply_poll_at_) {
                const RpcResultState ack = pollTrackedCommands({supply.rpc_uuid});
                map_blank_supply_poll_at_ = now + std::chrono::milliseconds(100);
                if (ack == RpcResultState::Accepted ||
                    ack == RpcResultState::Rejected) {
                    // A negative RPC result is not proof that /give had no
                    // inventory side effect on this client. Keep the durable
                    // intent and wait for native readback until timeout; a
                    // later resume must never dispatch a second /give.
                    map_blank_supply_ack_received_ = true;
                    LOGI("[map-supply] command_ack tile=%llu uuid=%s accepted=%d; waiting for native inventory",
                         static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                         supply.rpc_uuid.c_str(),
                         ack == RpcResultState::Accepted ? 1 : 0);
                }
            }
            if (now >= map_blank_supply_deadline_) {
                pauseMapCreation(inventory_error.empty()
                    ? "blank-map give may have run, but no native inventory map appeared; "
                      "the durable intent prevents an automatic duplicate give"
                    : "blank-map give cannot be verified in native inventory: " +
                      inventory_error);
                return;
            }
            map_slot_retry_at_ = now + kMapSlotRetryDelay;
            return;
        }

        MapInventoryTransfer::ReadyItem ready_map;
        std::string inventory_error;
        const MapInventoryTransfer::Result selection =
            map_inventory_transfer_.tick(MapInventoryTransfer::Kind::Empty, 0,
                                         &ready_map, &inventory_error);
        if (selection == MapInventoryTransfer::Result::Missing) {
            MapInventorySnapshot before_supply;
            if (!ReadMapInventorySnapshot(&before_supply, &inventory_error) ||
                !before_supply.ready || !before_supply.network_ready) {
                pauseMapCreation(inventory_error.empty()
                    ? "cannot verify inventory before giving a blank map" : inventory_error);
                return;
            }
            uint32_t empty_count = 0U;
            bool free_slot = false;
            for (const MapInventorySlot& item : before_supply.slots) {
                if (item.empty_map) empty_count += item.count;
                if (!item.occupied) free_slot = true;
            }
            if (empty_count != 0U) {
                if (++map_slot_attempts_ >= kMapSlotRetryLimit) {
                    pauseMapCreation("blank maps exist, but their native stack cannot be selected");
                } else {
                    map_slot_retry_at_ = now + kMapSlotRetryDelay;
                }
                return;
            }
            if (!free_slot) {
                pauseMapCreation("inventory has no free slot for an automatically given blank map");
                return;
            }
            supply.world_id = current_world_.world_id;
            supply.dimension_id = current_world_.dimension_id;
            supply.tile_cursor = map_tile_cursor_;
            supply.tile_count = map_tile_count_;
            supply.empty_maps_before = empty_count;
            supply.rpc_uuid = nextRpcUuid();
            if (!ArmMapBlankSupply(map_state_path_, supply, &supply_error)) {
                pauseMapCreation(supply_error.empty()
                    ? "cannot record blank-map give before dispatch" : supply_error);
                return;
            }
            // This explicit item ID is the Bedrock empty map. The exact
            // target stays @s, like the already-working map-center teleport.
            size_t sent_count = 0U;
            const bool sent = executeTrackedCommands(
                {"/give @s minecraft:empty_map 1"}, {supply.rpc_uuid},
                &sent_count);
            if (!sent || sent_count != 1U) {
                pauseMapCreation("blank-map give outcome is ambiguous; "
                    "the durable intent prevents an automatic duplicate give");
                return;
            }
            map_blank_supply_active_uuid_ = supply.rpc_uuid;
            map_blank_supply_ack_received_ = false;
            map_blank_supply_poll_at_ = now + kRpcPollInterval;
            map_blank_supply_deadline_ = now + std::chrono::seconds(20);
            map_slot_retry_at_ = now + kMapSlotRetryDelay;
            LOGI("[map-supply] give_sent tile=%llu/%llu uuid=%s",
                 static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                 static_cast<unsigned long long>(map_tile_count_),
                 supply.rpc_uuid.c_str());
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "giving an empty map (" +
                std::to_string(map_tile_cursor_ + 1U) + "/" +
                std::to_string(map_tile_count_) + ")";
            return;
        }
        if (selection == MapInventoryTransfer::Result::Failed ||
            now >= map_inventory_deadline_) {
            pauseMapCreation(inventory_error.empty()
                ? "automatic map inventory selection timed out" : inventory_error);
            return;
        }
        if (selection == MapInventoryTransfer::Result::Waiting) {
            map_slot_retry_at_ = now + kMapSlotRetryDelay;
            return;
        }

        MapInventorySnapshot before_use;
        if (!ReadMapInventorySnapshot(&before_use, &inventory_error) ||
            !before_use.ready || !before_use.network_ready ||
            before_use.selected_hotbar_slot != ready_map.hotbar_slot ||
            ready_map.hotbar_slot < 0 || ready_map.hotbar_slot > 8 ||
            !before_use.slots[static_cast<size_t>(ready_map.hotbar_slot)].empty_map ||
            before_use.slots[static_cast<size_t>(ready_map.hotbar_slot)].count !=
                ready_map.count ||
            before_use.slots[static_cast<size_t>(ready_map.hotbar_slot)].network_stack_id !=
                ready_map.network_stack_id) {
            if (++map_slot_attempts_ >= kMapSlotRetryLimit) {
                pauseMapCreation(inventory_error.empty()
                    ? "selected empty map changed before it could be used" : inventory_error);
            } else {
                map_slot_retry_at_ = now + kMapSlotRetryDelay;
            }
            return;
        }

        uint32_t filled_total = 0;
        std::array<int32_t, 36> filled_ids_before{};
        for (size_t slot = 0; slot < before_use.slots.size(); ++slot) {
            const MapInventorySlot& item = before_use.slots[slot];
            if (!item.filled_map) continue;
            if (!item.native_occupied || !item.has_network_stack_id ||
                item.network_stack_id <= 0) {
                pauseMapCreation("existing filled map identity cannot be verified before use");
                return;
            }
            filled_total += item.count;
            filled_ids_before[slot] = item.network_stack_id;
        }

        // An inventory swap can take several seconds. The teleport checked
        // before MapSelect is no longer proof of where the player is when
        // this ItemUse is sent; a moved player would anchor the new map to a
        // different tile while still advancing this tile's durable cursor.
        int32_t player_x = 0;
        int32_t player_y = 0;
        int32_t player_z = 0;
        if (!NativeWorldAccess::getLocalPlayerBlockPosition(
                &player_x, &player_y, &player_z) ||
            static_cast<int64_t>(player_x) - map_target_x_ < -2 ||
            static_cast<int64_t>(player_x) - map_target_x_ > 2 ||
            static_cast<int64_t>(player_z) - map_target_z_ < -2 ||
            static_cast<int64_t>(player_z) - map_target_z_ > 2) {
            pauseMapCreation("player left the target map center before map use; no map was consumed");
            return;
        }

        MapUsePendingDiskV1 pending;
        pending.tile_cursor = map_tile_cursor_;
        pending.hotbar_slot = ready_map.hotbar_slot;
        pending.network_stack_id = ready_map.network_stack_id;
        pending.empty_map_count = ready_map.count;
        pending.filled_map_total = filled_total;
        std::copy(filled_ids_before.begin(), filled_ids_before.end(),
                  std::begin(pending.filled_map_network_ids_before));
        pending.target_x = map_target_x_;
        pending.target_z = map_target_z_;
        pending.texture_sequence_before = GetMapTextureReceiveSequence();
        std::string pending_error;
        if (!saveMapUsePending(map_state_path_, pending, &pending_error)) {
            pauseMapCreation(pending_error.empty()
                ? "cannot durably record pending map use" : pending_error);
            return;
        }
        std::string packet_error;
        if (!ContainerOpenPacketSender::useSelectedItem(
                &packet_error, ready_map.hotbar_slot, true,
                ready_map.network_stack_id)) {
            std::remove(mapUsePendingPath(map_state_path_).c_str());
            if (++map_slot_attempts_ >= kMapSlotRetryLimit) {
                pauseMapCreation(packet_error.empty()
                    ? "cannot send the map-use packet" : packet_error);
                return;
            }
            map_slot_retry_at_ = now + kMapSlotRetryDelay;
            return;
        }
        LOGI("[map] use_sent tile=%llu/%llu center=(%d,%d) empty_net_id=%d count=%u",
             static_cast<unsigned long long>(map_tile_cursor_ + 1U),
             static_cast<unsigned long long>(map_tile_count_),
             map_target_x_, map_target_z_, ready_map.network_stack_id,
             static_cast<unsigned>(ready_map.count));
        map_use_hotbar_slot_ = ready_map.hotbar_slot;
        map_use_network_stack_id_ = ready_map.network_stack_id;
        map_use_empty_count_before_ = ready_map.count;
        map_use_filled_total_before_ = filled_total;
        map_use_filled_network_ids_before_ = filled_ids_before;
        map_use_new_filled_network_id_ = 0;
        map_use_new_filled_uuid_ = -1;
        map_use_held_filled_network_id_ = 0;
        map_use_texture_sequence_before_ = pending.texture_sequence_before;
        map_use_item_confirmed_ = false;
        map_slot_attempts_ = 0;
        map_settle_ready_at_ = now + kMapItemSettleDelay;
        map_settle_log_at_ = map_settle_ready_at_;
        map_texture_log_at_ = map_settle_ready_at_;
        map_use_deadline_ = now + kMapUseConfirmationTimeout;
        stage_ = ExecuteStage::MapSettle;
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "waiting for the filled map item (" +
            std::to_string(map_tile_cursor_ + 1U) + "/" +
            std::to_string(map_tile_count_) + ")";
        return;
    }

    if (stage_ == ExecuteStage::MapSettle) {
        if (now < map_settle_ready_at_) return;
        MapInventorySnapshot after_use;
        std::string observation_error;
        const bool observed = !map_use_item_confirmed_ &&
            ReadMapInventorySnapshot(&after_use, &observation_error) &&
            after_use.ready && after_use.network_ready &&
            map_use_hotbar_slot_ >= 0 && map_use_hotbar_slot_ <= 8;
        if (observed && !map_use_item_confirmed_) {
            const MapInventorySlot& old_slot =
                after_use.slots[static_cast<size_t>(map_use_hotbar_slot_)];
            const uint16_t remaining_empty_maps = old_slot.empty_map
                ? old_slot.count : 0U;
            uint32_t filled_total = 0;
            for (const MapInventorySlot& item : after_use.slots) {
                if (item.filled_map) filled_total += item.count;
            }
            int32_t new_filled_network_id = 0;
            const bool unique_new_filled = findNewFilledMap(
                after_use, map_use_filled_network_ids_before_.data(),
                &new_filled_network_id);
            if (now >= map_settle_log_at_) {
                ProjectionPrinterInventorySnapshot remote;
                const bool remote_ready =
                    GetProjectionPrinterInventorySnapshot(&remote) && remote.ready;
                const auto& remote_old = remote.slots[
                    static_cast<size_t>(map_use_hotbar_slot_)];
                std::string maps;
                for (size_t slot = 0; slot < after_use.slots.size(); ++slot) {
                    const MapInventorySlot& item = after_use.slots[slot];
                    if (!item.empty_map && !item.filled_map) continue;
                    if (!maps.empty()) maps += ',';
                    maps += (item.empty_map ? "E" : "F") + std::to_string(slot) +
                        ":" + std::to_string(item.count) +
                        ":" + std::to_string(item.network_stack_id) +
                        ":" + (item.has_map_uuid
                            ? std::to_string(item.map_uuid) : "?");
                }
                LOGI("[map] settle tile=%llu old_slot=%d old_empty=%d old_count=%u "
                     "before=%u filled=%u/%u new_id=%d remote_rev=%llu "
                     "remote_old=%u:%d slots=[%s]",
                     static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                     map_use_hotbar_slot_, old_slot.empty_map ? 1 : 0,
                     static_cast<unsigned>(remaining_empty_maps),
                     static_cast<unsigned>(map_use_empty_count_before_),
                     filled_total, map_use_filled_total_before_,
                     unique_new_filled ? new_filled_network_id : 0,
                     static_cast<unsigned long long>(
                         remote_ready ? remote.revision : 0U),
                     remote_ready ? static_cast<unsigned>(remote_old.count) : 0U,
                     remote_ready ? remote_old.network_stack_id : 0,
                     maps.c_str());
                map_settle_log_at_ = now + std::chrono::seconds(1);
            }
            // ItemUse is sent directly, so a 64-stack can still show count 64
            // locally when the server-created map is already in another slot.
            // Require one new native filled-map identity and a greater filled
            // total; do not treat an unchanged blank count as failure. The
            // map UUID may not become readable until this item is held.
            if (filled_total > map_use_filled_total_before_ &&
                unique_new_filled) {
                map_use_new_filled_network_id_ = new_filled_network_id;
                findFilledMapUuid(after_use, new_filled_network_id,
                                  &map_use_new_filled_uuid_);
                map_use_item_confirmed_ = true;
                LOGI("[map] filled_confirmed tile=%llu uuid=%lld net_id=%d "
                     "old_count=%u/%u",
                     static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                     static_cast<long long>(map_use_new_filled_uuid_),
                     map_use_new_filled_network_id_,
                     static_cast<unsigned>(remaining_empty_maps),
                     static_cast<unsigned>(map_use_empty_count_before_));
                // The item now exists, but moving a filled map from the main
                // inventory and holding it needs a separate deadline.
                map_use_deadline_ = now + kMapHoldConfirmationTimeout;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_ = "filled map created; switching to its slot (" +
                        std::to_string(map_tile_cursor_ + 1U) + "/" +
                        std::to_string(map_tile_count_) + ")";
                }
            }
        } else if (!map_use_item_confirmed_ && now >= map_settle_log_at_) {
            LOGI("[map] settle snapshot_unavailable tile=%llu error=%s",
                 static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                 observation_error.c_str());
            map_settle_log_at_ = now + std::chrono::seconds(1);
        }
        if (map_use_item_confirmed_ && map_use_held_filled_network_id_ == 0 &&
            now >= map_slot_retry_at_) {
            map_slot_retry_at_ = now + kMapSlotRetryDelay;
            MapInventoryTransfer::ReadyItem held_map;
            std::string hold_error;
            const MapInventoryTransfer::Result hold_result =
                map_inventory_transfer_.tick(MapInventoryTransfer::Kind::Filled,
                    map_use_new_filled_network_id_, &held_map, &hold_error);
            if (hold_result == MapInventoryTransfer::Result::Failed) {
                pauseMapCreation(hold_error.empty()
                    ? "new filled map could not be selected for texture loading"
                    : hold_error);
                return;
            }
            if (hold_result == MapInventoryTransfer::Result::Ready) {
                MapInventorySnapshot held_inventory;
                int64_t held_uuid = -1;
                std::string held_error;
                if (!ReadMapInventorySnapshot(&held_inventory, &held_error) ||
                    !held_inventory.ready || !held_inventory.network_ready) {
                    observation_error = held_error;
                } else if (held_inventory.selected_hotbar_slot == held_map.hotbar_slot &&
                           held_inventory.slots[static_cast<size_t>(held_map.hotbar_slot)]
                               .filled_map &&
                           held_inventory.slots[static_cast<size_t>(held_map.hotbar_slot)]
                               .network_stack_id == held_map.network_stack_id) {
                    if (findFilledMapUuid(held_inventory, held_map.network_stack_id,
                                          &held_uuid)) {
                        if (map_use_new_filled_uuid_ != -1 &&
                            map_use_new_filled_uuid_ != held_uuid) {
                            pauseMapCreation("the held map UUID differs from the newly created map");
                            return;
                        }
                        map_use_new_filled_uuid_ = held_uuid;
                    }
                    // A backpack-to-hotbar swap may assign a new network
                    // stack ID. Keep the exact target current so a later
                    // manual hotbar change can be corrected safely.
                    map_use_new_filled_network_id_ = held_map.network_stack_id;
                    map_use_held_filled_network_id_ = held_map.network_stack_id;
                    map_use_hold_ready_at_ = now + std::chrono::seconds(2);
                    LOGI("[map] filled_held tile=%llu slot=%d net_id=%d uuid=%lld",
                         static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                         held_map.hotbar_slot, held_map.network_stack_id,
                         static_cast<long long>(map_use_new_filled_uuid_));
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_ = "holding filled map; waiting for all artwork-area pixels (" +
                        std::to_string(map_tile_cursor_ + 1U) + "/" +
                        std::to_string(map_tile_count_) + ")";
                }
            }
        }
        if (map_use_held_filled_network_id_ > 0 && now >= map_settle_log_at_) {
            MapInventorySnapshot held_inventory;
            std::string held_error;
            if (ReadMapInventorySnapshot(&held_inventory, &held_error) &&
                held_inventory.ready && held_inventory.network_ready &&
                held_inventory.selected_hotbar_slot >= 0 &&
                held_inventory.selected_hotbar_slot <= 8) {
                const MapInventorySlot& selected = held_inventory.slots[
                    static_cast<size_t>(held_inventory.selected_hotbar_slot)];
                if (selected.filled_map &&
                    selected.network_stack_id == map_use_held_filled_network_id_) {
                    int64_t held_uuid = -1;
                    if (findFilledMapUuid(held_inventory,
                                          map_use_held_filled_network_id_, &held_uuid)) {
                        if (map_use_new_filled_uuid_ != -1 &&
                            map_use_new_filled_uuid_ != held_uuid) {
                            pauseMapCreation("the held map UUID changed during texture loading");
                            return;
                        }
                        if (map_use_new_filled_uuid_ == -1) {
                            map_use_new_filled_uuid_ = held_uuid;
                            LOGI("[map] uuid_confirmed tile=%llu uuid=%lld",
                                 static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                                 static_cast<long long>(held_uuid));
                        }
                    }
                } else {
                    map_use_held_filled_network_id_ = 0;
                    map_use_hold_ready_at_ = {};
                    LOGI("[map] held_slot_changed tile=%llu selected=%d",
                         static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                         held_inventory.selected_hotbar_slot);
                }
                LOGI("[map] hold_check tile=%llu selected=%d held_id=%d uuid=%lld",
                     static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                     held_inventory.selected_hotbar_slot,
                     map_use_held_filled_network_id_,
                     static_cast<long long>(map_use_new_filled_uuid_));
            }
            map_settle_log_at_ = now + std::chrono::seconds(1);
        }
        int32_t pixel_x = 0;
        int32_t pixel_z = 0;
        int32_t pixel_width = 0;
        int32_t pixel_height = 0;
        if (!mapArtworkPixelRectangle(map_source_bounds_, map_target_x_, map_target_z_,
                                      &pixel_x, &pixel_z,
                                      &pixel_width, &pixel_height)) {
            pauseMapCreation("artwork does not intersect the selected map tile");
            return;
        }
        MapTextureRectangleCoverage coverage;
        const bool texture_observed = map_use_new_filled_uuid_ != -1 &&
            GetMapTextureRectangleCoverageById(
                map_use_new_filled_uuid_, pixel_x, pixel_z,
                pixel_width, pixel_height, &coverage) &&
            coverage.observation.texture_sequence >
                map_use_texture_sequence_before_ &&
            coverage.observation.origin_x == map_target_x_ &&
            coverage.observation.origin_z == map_target_z_ &&
            coverage.observation.scale_known &&
            coverage.observation.scale == 0U &&
            current_world_.dimension_id >= 0 &&
            current_world_.dimension_id <= 255 &&
            coverage.observation.dimension ==
                static_cast<uint8_t>(current_world_.dimension_id);
        const bool texture_complete = texture_observed && coverage.fully_covered;
        if (map_use_item_confirmed_ && now >= map_texture_log_at_) {
            MapTextureObservation exact;
            MapTextureObservation latest;
            const bool exact_seen = map_use_new_filled_uuid_ != -1 &&
                GetMapTextureObservationById(map_use_new_filled_uuid_, &exact);
            const bool latest_seen = GetLatestMapTextureObservation(&latest);
            LOGI("[map] texture_probe tile=%llu uuid=%lld held=%d expected_origin=(%d,%d) "
                 "expected_dim=%d baseline_seq=%llu receive_seq=%llu map_info_out=%llu "
                 "exact=%d exact_texture_seq=%llu exact_origin=(%d,%d) "
                 "exact_dim=%u exact_scale=%u:%d exact_rect=(%d,%d,%d,%d) "
                 "coverage=%u/%u nonzero=%u matched=%d latest=%d latest_id=%lld",
                 static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                 static_cast<long long>(map_use_new_filled_uuid_),
                 map_use_held_filled_network_id_, map_target_x_, map_target_z_,
                 current_world_.dimension_id,
                 static_cast<unsigned long long>(map_use_texture_sequence_before_),
                 static_cast<unsigned long long>(GetMapTextureReceiveSequence()),
                 static_cast<unsigned long long>(
                     GetOutboundMapInfoRequestAttemptCount()),
                 exact_seen ? 1 : 0,
                 static_cast<unsigned long long>(exact_seen ? exact.texture_sequence : 0U),
                 exact_seen ? exact.origin_x : 0,
                 exact_seen ? exact.origin_z : 0,
                 exact_seen ? static_cast<unsigned>(exact.dimension) : 0U,
                 exact_seen ? static_cast<unsigned>(exact.scale) : 0U,
                 exact_seen && exact.scale_known ? 1 : 0,
                 exact_seen ? exact.last_x_offset : 0,
                 exact_seen ? exact.last_y_offset : 0,
                 exact_seen ? exact.last_width : 0,
                 exact_seen ? exact.last_height : 0,
                 coverage.covered_pixel_count, coverage.pixel_count,
                 coverage.nonzero_pixel_count, texture_observed ? 1 : 0,
                 latest_seen ? 1 : 0,
                 static_cast<long long>(latest_seen ? latest.map_id : -1));
            map_texture_log_at_ = now + std::chrono::seconds(1);
        }
        if (map_use_item_confirmed_ &&
            map_use_held_filled_network_id_ > 0 &&
            now >= map_use_hold_ready_at_ && texture_complete) {
                MapInventorySnapshot final_hold;
                std::string final_hold_error;
                if (!ReadMapInventorySnapshot(&final_hold, &final_hold_error) ||
                    !final_hold.ready || !final_hold.network_ready ||
                    final_hold.selected_hotbar_slot < 0 ||
                    final_hold.selected_hotbar_slot > 8 ||
                    !final_hold.slots[static_cast<size_t>(
                        final_hold.selected_hotbar_slot)].filled_map ||
                    final_hold.slots[static_cast<size_t>(
                        final_hold.selected_hotbar_slot)].network_stack_id !=
                        map_use_held_filled_network_id_) {
                    map_use_held_filled_network_id_ = 0;
                    map_use_hold_ready_at_ = {};
                    return;
                }
                LOGI("[map] pixels_covered tile=%llu uuid=%lld covered=%u/%u nonzero=%u",
                     static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                     static_cast<long long>(map_use_new_filled_uuid_),
                     coverage.covered_pixel_count, coverage.pixel_count,
                     coverage.nonzero_pixel_count);
                if (map_storage_pipeline_active_) {
                    // No irreversible storage operation is performed here.
                    // The storage driver journals rename/Place, confirms the
                    // accepted response and native map absence, then commits
                    // this tile's cursor and removes its map-use marker.
                    stage_ = ExecuteStage::MapStorageHandoff;
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_ = "map pixels confirmed; preparing verified storage (" +
                        std::to_string(map_tile_cursor_ + 1U) + "/" +
                        std::to_string(map_tile_count_) + ")";
                    return;
                }
                ++map_tile_cursor_;
                std::string state_error;
                if (!saveDeferredDataState(map_state_path_, map_tile_count_,
                                           map_tile_cursor_, &state_error)) {
                    pauseMapCreation(state_error.empty()
                        ? "cannot save confirmed automatic map progress" : state_error);
                    return;
                }
                map_tile_persisted_cursor_ = map_tile_cursor_;
                if (std::remove(mapUsePendingPath(map_state_path_).c_str()) != 0) {
                    pauseMapCreation("confirmed map cursor saved, but pending-use marker cleanup failed");
                    return;
                }
                map_use_hotbar_slot_ = -1;
                map_use_network_stack_id_ = 0;
                map_use_empty_count_before_ = 0;
                map_use_filled_total_before_ = 0;
                map_use_filled_network_ids_before_.fill(0);
                map_use_new_filled_network_id_ = 0;
                map_use_new_filled_uuid_ = -1;
                map_use_held_filled_network_id_ = 0;
                map_use_texture_sequence_before_ = 0U;
                map_use_item_confirmed_ = false;
                map_use_deadline_ = {};
                map_use_hold_ready_at_ = {};
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_ = "filled map and artwork-area texture packet coverage confirmed (" +
                        std::to_string(map_tile_cursor_) + "/" +
                        std::to_string(map_tile_count_) + ")";
                }
                if (map_tile_cursor_ == map_tile_count_) finishMapCreation();
                else stage_ = ExecuteStage::MapPrepare;
                return;
        }
        if (now >= map_use_deadline_) {
            if (map_use_item_confirmed_ && map_use_held_filled_network_id_ > 0) {
                LOGE("[map] pixels_timeout tile=%llu uuid=%lld observed=%d covered=%u/%u",
                     static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                     static_cast<long long>(map_use_new_filled_uuid_),
                     texture_observed ? 1 : 0,
                     texture_observed ? coverage.covered_pixel_count : 0U,
                     texture_observed ? coverage.pixel_count : 0U);
                pauseMapCreation(map_use_new_filled_uuid_ == -1
                    ? "filled map is held, but its native map UUID is still unavailable"
                    : texture_observed
                        ? "filled map pixel coverage stopped at " +
                          std::to_string(coverage.covered_pixel_count) + "/" +
                          std::to_string(coverage.pixel_count) +
                          "; keep holding/exploring this map before resuming"
                        : "filled map was created, but its native map UUID had no matching level-0 texture data for the target center; inspect the map before resuming");
            } else {
                LOGE("[map] item_timeout tile=%llu item_confirmed=%d new_id=%d held_id=%d "
                     "observation_error=%s",
                     static_cast<unsigned long long>(map_tile_cursor_ + 1U),
                     map_use_item_confirmed_ ? 1 : 0,
                     map_use_new_filled_network_id_,
                     map_use_held_filled_network_id_, observation_error.c_str());
                pauseMapCreation(observation_error.empty()
                    ? map_use_item_confirmed_
                        ? "new filled map was confirmed but could not be held; inspect inventory before resume"
                        : "map use was sent, but no unique new filled map appeared in the native inventory; inspect inventory before resume"
                    : "map use result cannot be confirmed: " + observation_error);
            }
            return;
        }
        return;
    }

    pauseMapCreation("automatic map stage is invalid");
}

void BuildImportRuntime::clearCompletedMapUseRuntime() {
    map_storage_renamed_transfer_network_id_ = 0;
    map_storage_renamed_transfer_uuid_ = -1;
    map_storage_renamed_transfer_cursor_ = 0;
    map_use_hotbar_slot_ = -1;
    map_use_network_stack_id_ = 0;
    map_use_empty_count_before_ = 0;
    map_use_filled_total_before_ = 0;
    map_use_filled_network_ids_before_.fill(0);
    map_use_new_filled_network_id_ = 0;
    map_use_new_filled_uuid_ = -1;
    map_use_held_filled_network_id_ = 0;
    map_use_texture_sequence_before_ = 0U;
    map_use_item_confirmed_ = false;
    map_use_deadline_ = {};
    map_use_hold_ready_at_ = {};
}

bool BuildImportRuntime::commitStoredMapCursor(
        uint64_t before, uint64_t after,
        const MapChestTransferRecord& chest, std::string* error) {
    if (!in_game_tick_ || !map_creation_active_ ||
        stage_ != ExecuteStage::MapStorageHandoff ||
        before != map_tile_cursor_ || before != map_tile_persisted_cursor_ ||
        after != before + 1U || after > map_tile_count_ ||
        (chest.phase != MapChestTransferPhase::ReopenConfirmed &&
         chest.phase != MapChestTransferPhase::InventoryConfirmed &&
         chest.phase != MapChestTransferPhase::AcceptedAndClosed) ||
        chest.tile_cursor != before || chest.tile_count != map_tile_count_ ||
        chest.world_id != current_world_.world_id ||
        chest.dimension_id != current_world_.dimension_id) {
        if (error) *error = "map cursor commit lacks current verified chest context";
        return false;
    }
    if (!CommitVerifiedMapTileCursor(map_state_path_, map_tile_count_,
                                     before, error)) return false;
    uint64_t reloaded = 0U;
    if (!loadDeferredDataState(map_state_path_, map_tile_count_,
                               &reloaded, error) || reloaded != after) {
        if (error && error->empty()) {
            *error = "durable map cursor commit did not read back exactly";
        }
        return false;
    }
    map_tile_cursor_ = reloaded;
    map_tile_persisted_cursor_ = reloaded;
    return true;
}

bool BuildImportRuntime::finalizeStoredMapUse(
        uint64_t old_cursor, uint64_t committed_cursor,
        std::string* error) {
    if (!in_game_tick_ || !map_creation_active_ ||
        stage_ != ExecuteStage::MapStorageHandoff ||
        old_cursor >= map_tile_count_ ||
        committed_cursor != old_cursor + 1U ||
        committed_cursor != map_tile_cursor_ ||
        committed_cursor != map_tile_persisted_cursor_) {
        if (error) *error = "map-use finalization cursor is inconsistent";
        return false;
    }
    uint64_t persisted = 0U;
    if (!loadDeferredDataState(map_state_path_, map_tile_count_,
                               &persisted, error) ||
        persisted != committed_cursor) {
        if (error && error->empty()) *error = "map-use finalization lacks committed cursor";
        return false;
    }
    MapAnvilRenameRecord rename;
    MapChestTransferRecord chest;
    if (LoadMapAnvilRenameJournal(map_state_path_, &rename, error) !=
            MapAnvilRenameLoad::Missing ||
        LoadMapChestTransferJournal(map_state_path_, &chest, error) !=
            MapChestJournalLoad::Missing) {
        if (error && error->empty()) *error = "map-use finalization found storage journals";
        return false;
    }
    MapUsePendingDiskV1 marker;
    if (!loadMapUsePending(map_state_path_, &marker, error) ||
        marker.tile_cursor != old_cursor) {
        if (error && error->empty()) *error = "map-use marker does not match committed tile";
        return false;
    }
    const std::string path = mapUsePendingPath(map_state_path_);
    if (std::remove(path.c_str()) != 0) {
        if (error) *error = "committed map-use marker cannot be removed";
        return false;
    }
#if defined(_WIN32)
    // This Android-only path must fail closed on a host where directory
    // durability has not been implemented.
    if (error) *error = "map-use marker directory sync is unavailable on host";
    return false;
#else
    const std::string directory = parentDirectory(path);
    const int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        if (error) *error = "map-use marker parent directory cannot be opened";
        return false;
    }
    const bool synced = fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    if (!synced || !closed) {
        if (error) *error = "map-use marker removal durability is uncertain";
        return false;
    }
#endif
    if (map_storage_chest_capture_) {
        map_storage_chest_stop_requested_ = true;
    }
    clearCompletedMapUseRuntime();
    if (error) error->clear();
    return true;
}

void BuildImportRuntime::serviceMapStorageChest(
        std::chrono::steady_clock::time_point now) noexcept {
    if (!map_storage_chest_capture_) return;
    const uint64_t tick_ms = mapStorageMonotonicMs(now);
    if (map_storage_chest_stop_requested_) {
        map_storage_chest_capture_->stop(tick_ms);
    }
    const MapStorageChestCaptureState state =
        map_storage_chest_capture_->tick(tick_ms);
    if (state == MapStorageChestCaptureState::Failed &&
        map_storage_chest_capture_->safeToDiscardAfterFailure()) {
        // Preserve the task-level failure latch while releasing only the
        // adapter that never attempted to open a server-side window.
        map_storage_chest_terminal_failure_ = true;
    }
    if (state == MapStorageChestCaptureState::Stopped ||
        (state == MapStorageChestCaptureState::Failed &&
         map_storage_chest_capture_->safeToDiscardAfterFailure())) {
        if (map_storage_chest_client_sync_token_ != 0U) {
            CancelMapChestClientSyncWindow(map_storage_chest_client_sync_token_);
        }
        map_storage_chest_client_sync_ticket_ = 0U;
        map_storage_chest_client_sync_token_ = 0U;
        map_storage_chest_client_sync_request_id_ = 0;
        map_storage_chest_client_sync_confirmed_ = false;
        map_storage_chest_client_sync_deadline_ = {};
        map_storage_chest_client_sync_error_.clear();
        map_storage_chest_capture_.reset();
        map_storage_chest_preflight_ = {};
#if defined(__ANDROID__)
        map_storage_chest_native_preflight_ = {};
#endif
        map_storage_chest_world_ = {};
        map_storage_chest_dimension_token_ = 0U;
        map_storage_chest_stop_requested_ = false;
    }
}

void BuildImportRuntime::serviceMapStorageAnvil(
        std::chrono::steady_clock::time_point now) noexcept {
    if (!map_storage_anvil_window_) return;
#if !defined(__ANDROID__)
    (void)now;
    map_storage_anvil_terminal_failure_ = true;
#else
    try {
        const uint64_t tick_ms = mapStorageMonotonicMs(now);
        auto state = map_storage_anvil_window_->state();
        if (map_storage_anvil_stop_requested_) {
            if (state == MapVisibleAnvilWindowState::Open) {
                const bool close_sent =
                    map_storage_anvil_window_->close(tick_ms, nullptr);
                if (close_sent && map_storage_anvil_local_close_requested_) {
                    const bool queued =
                        QueueMapAnvilUiClose(map_storage_anvil_token_);
                    LOGI("[map-anvil-ui-close] server_close_sent=1 token=%llu back_queued=%d",
                         static_cast<unsigned long long>(map_storage_anvil_token_),
                         queued ? 1 : 0);
                }
            } else if (state != MapVisibleAnvilWindowState::Completed &&
                       state != MapVisibleAnvilWindowState::Failed &&
                       state != MapVisibleAnvilWindowState::CloseUncertain) {
                map_storage_anvil_window_->abort(
                    tick_ms, "automatic anvil task stopped");
            }
        }
        state = map_storage_anvil_window_->tick(tick_ms);
        if (state == MapVisibleAnvilWindowState::Completed ||
            state == MapVisibleAnvilWindowState::Failed) {
            CancelMapAnvilClientSyncWindow(map_storage_anvil_token_);
            if (state == MapVisibleAnvilWindowState::Completed &&
                map_storage_anvil_local_close_requested_ &&
                !map_storage_anvil_probe_disarmed_) {
                MapNativeAnvilManagerSnapshot screen;
                const bool observed = ReadMapNativeAnvilManagerProbe(
                    Main::getBaseAddress(), map_storage_anvil_token_, &screen);
                const bool screen_retired = observed &&
                    screen.screen_destructor_hits == 1U;
                const bool back_dispatched =
                    WasMapAnvilUiCloseDispatched(map_storage_anvil_token_);
                if (!screen_retired && !back_dispatched) {
                    if (now - map_storage_anvil_local_close_started_at_ <=
                        std::chrono::seconds(5)) {
                        return;
                    }
                    LOGI("[map-anvil-ui-close] timed out waiting for exact-screen Back dispatch token=%llu",
                         static_cast<unsigned long long>(map_storage_anvil_token_));
                    CancelMapAnvilUiClose(map_storage_anvil_token_);
                    DisarmMapNativeAnvilManagerProbe(map_storage_anvil_token_);
                    map_storage_anvil_probe_disarmed_ = true;
                    map_storage_anvil_retirement_started_at_ = now;
                    map_storage_anvil_terminal_failure_ = true;
                    return;
                }
                LOGI("[map-anvil-ui-close] back_dispatched=%d screen_retired=%d token=%llu",
                     back_dispatched ? 1 : 0, screen_retired ? 1 : 0,
                     static_cast<unsigned long long>(map_storage_anvil_token_));
            }
            if (!map_storage_anvil_probe_disarmed_) {
                DisarmMapNativeAnvilManagerProbe(map_storage_anvil_token_);
                map_storage_anvil_probe_disarmed_ = true;
                map_storage_anvil_retirement_started_at_ = now;
            }
            if (state == MapVisibleAnvilWindowState::Failed) {
                CancelMapAnvilUiClose(map_storage_anvil_token_);
                // A failed ClickBlock/Close may have reached the server. Keep
                // the session object and refuse a second numeric window.
                if (!map_storage_anvil_terminal_failure_) {
                    LOGI("[map-anvil-window] failed token=%llu reason=%s",
                         static_cast<unsigned long long>(map_storage_anvil_token_),
                         map_storage_anvil_window_->error().c_str());
                }
                map_storage_anvil_terminal_failure_ = true;
                return;
            }
            // The game's screen/controller may remain cached after the
            // close flow. Their deleting
            // destructors are required before reusing the *probe*, not before
            // attempting a separately verified, reversible chest Open. The
            // chest adapter will require its own exact target/type/content
            // capture before it can submit any item transfer.
            if (!map_storage_anvil_handoff_ready_ &&
                map_storage_anvil_local_close_requested_ &&
                WasMapAnvilUiCloseDispatched(map_storage_anvil_token_) &&
                now - map_storage_anvil_local_close_started_at_ >=
                     std::chrono::milliseconds(200) &&
                current_world_ == map_storage_anvil_world_ &&
                controller_.worldMatches(current_world_)) {
                const uint8_t window_id =
                    map_storage_anvil_window_->openedWindowId();
                const bool server_close_observed =
                    HasVisibleAnvilServerCloseReceipt(
                        map_storage_anvil_token_, window_id);
                // Some servers do not echo a client Close. Wait briefly for
                // this version's usual reply, but let an independently
                // captured exact chest Open be the final server-side proof.
                if (server_close_observed ||
                    now - map_storage_anvil_local_close_started_at_ >=
                        std::chrono::seconds(1)) {
                    map_storage_anvil_handoff_ready_ = true;
                    LOGI("[map-anvil-ui-close] chest handoff may verify its own window token=%llu anvil_window=%u server_close=%d",
                         static_cast<unsigned long long>(map_storage_anvil_token_),
                         static_cast<unsigned>(window_id),
                         server_close_observed ? 1 : 0);
                }
            }
            MapNativeAnvilRearmEvidence retired;
            const bool retirement_read = ReadMapNativeAnvilProbeRetirement(
                Main::getBaseAddress(), map_storage_anvil_token_, &retired);
            if (retirement_read &&
                CanSequentiallyRearmMapNativeAnvilProbe(retired)) {
                CancelMapAnvilUiClose(map_storage_anvil_token_);
                map_storage_anvil_window_.reset();
                map_storage_anvil_preflight_ = {};
                map_storage_anvil_native_preflight_ = {};
                map_storage_anvil_world_ = {};
                map_storage_anvil_dimension_token_ = 0U;
                map_storage_anvil_token_ = 0U;
                map_storage_anvil_stop_requested_ = false;
                map_storage_anvil_local_close_requested_ = false;
                map_storage_anvil_handoff_ready_ = false;
                map_storage_anvil_probe_disarmed_ = false;
                map_storage_anvil_retirement_reported_ = false;
                map_storage_anvil_terminal_failure_ = false;
                map_storage_anvil_name_attempted_ = false;
                map_storage_anvil_name_submitted_ = false;
                map_storage_anvil_name_title_.clear();
                map_storage_anvil_retirement_started_at_ = {};
                map_storage_anvil_local_close_started_at_ = {};
            } else if (now - map_storage_anvil_retirement_started_at_ >
                       std::chrono::seconds(5)) {
                if (!map_storage_anvil_handoff_ready_) {
                    map_storage_anvil_terminal_failure_ = true;
                } else if (!map_storage_anvil_retirement_reported_) {
                    map_storage_anvil_retirement_reported_ = true;
                    LOGI("[map-anvil-ui-close] native retirement pending token=%llu observed=%d mgr=%u/%u/%u/%u screen=%u/%u/%u/%u callbacks=%u ambiguous=%d",
                         static_cast<unsigned long long>(map_storage_anvil_token_),
                         retirement_read ? 1 : 0,
                         retired.manager_constructors, retired.manager_destructors,
                         retired.manager_complete_destructor_epilogues,
                         retired.manager_delete_completions,
                         retired.screen_constructors, retired.screen_destructors,
                         retired.screen_complete_destructor_epilogues,
                         retired.screen_delete_completions,
                         retired.callbacks_in_flight,
                         retired.ambiguous ? 1 : 0);
                }
            }
        }
    } catch (...) {
        map_storage_anvil_terminal_failure_ = true;
    }
#endif
}

bool BuildImportRuntime::observeMapStorageAnvil(
        int32_t x, int32_t y, int32_t z, bool allow_open,
        MapAnvilRenameWindowEvidence* evidence, std::string* error) {
    if (evidence) *evidence = {};
    if (error) error->clear();
#if !defined(__ANDROID__)
    (void)x; (void)y; (void)z; (void)allow_open;
    if (error) *error = "visible anvil session requires Android";
    return false;
#else
    if (!map_storage_pipeline_active_ || !evidence || !in_game_tick_ ||
        !IsMinecraftUpdateGameThread() || !map_creation_active_ ||
        stage_ != ExecuteStage::MapStorageHandoff ||
        map_storage_anvil_stop_requested_ ||
        map_storage_anvil_terminal_failure_ ||
        current_world_.world_id.empty() ||
        !controller_.worldMatches(current_world_)) {
        if (error) *error = "anvil observation lacks the original live game world";
        return false;
    }
    const uint64_t tick_ms = mapStorageMonotonicMs(
        std::chrono::steady_clock::now());
    if (!map_storage_anvil_window_) {
        if (!allow_open) {
            if (error) *error = "original anvil window is gone; never reopen an armed request";
            return false;
        }
        if (map_storage_chest_capture_) {
            if (error) *error = "map chest capture still owns the container mailbox";
            return false;
        }
        MapAnvilRenameRecord record;
        if (LoadMapAnvilRenameJournal(map_state_path_, &record, error) !=
                MapAnvilRenameLoad::Loaded ||
            record.phase != MapAnvilRenamePhase::Prepared ||
            record.world_id != current_world_.world_id ||
            record.dimension_id != current_world_.dimension_id ||
            record.anvil_x != x || record.anvil_y != y ||
            record.anvil_z != z) {
            if (error && error->empty()) {
                *error = "only the prepared map may open a new anvil window";
            }
            return false;
        }
        static std::atomic<uint64_t> sequence{0U};
        const uint64_t next = sequence.fetch_add(
            1U, std::memory_order_relaxed) + 1U;
        if (next >= (uint64_t{1} << 60U)) {
            if (error) *error = "anvil window token sequence exhausted";
            return false;
        }
        const uint64_t token = (uint64_t{2} << 60U) | next;
        const uintptr_t base = Main::getBaseAddress();
        if (!base || !ArmMapNativeAnvilManagerProbe(base, token)) {
            if (error) *error = "native anvil manager probe cannot be armed safely";
            return false;
        }
        map_storage_anvil_preflight_.expected = {
            token, current_world_.world_id, native_dimension_token_,
            x, y, z, 1, 4U};
        map_storage_anvil_native_preflight_ =
            MakeNativeMapVisibleAnvilProductionPreflight(
                &map_storage_anvil_preflight_);
        auto window = std::make_unique<MapVisibleAnvilWindowSession>();
        const MapVisibleAnvilWindowRequest request{token, x, y, z, 1};
        const auto ops = MakeNativeMapVisibleAnvilWindowOps(
            &map_storage_anvil_native_preflight_);
        if (!window->begin(request, ops, tick_ms, error)) {
            DisarmMapNativeAnvilManagerProbe(token);
            return false;
        }
        map_storage_anvil_world_ = current_world_;
        map_storage_anvil_dimension_token_ = native_dimension_token_;
        map_storage_anvil_token_ = token;
        map_storage_anvil_local_close_requested_ = false;
        map_storage_anvil_handoff_ready_ = false;
        map_storage_anvil_local_close_started_at_ = {};
        map_storage_anvil_probe_disarmed_ = false;
        map_storage_anvil_retirement_reported_ = false;
        map_storage_anvil_name_attempted_ = false;
        map_storage_anvil_name_submitted_ = false;
        map_storage_anvil_name_title_.clear();
        map_storage_anvil_window_ = std::move(window);
        (void)map_storage_anvil_window_->tick(tick_ms);
    }
    if (!(current_world_ == map_storage_anvil_world_) ||
        map_storage_anvil_preflight_.expected.x != x ||
        map_storage_anvil_preflight_.expected.y != y ||
        map_storage_anvil_preflight_.expected.z != z) {
        if (error) *error = "anvil window world, dimension or target changed";
        return false;
    }
    if (map_storage_anvil_window_->state() != MapVisibleAnvilWindowState::Open) {
        if (error) *error = "waiting for the original visible anvil window";
        return false;
    }
    ContainerCaptureResult capture;
    if (!map_storage_anvil_window_->verifiedLiveCapture(
            map_storage_anvil_token_, tick_ms, &capture, error)) return false;
    uintptr_t manager = 0U;
    if (!BorrowMapNativeAnvilManagerForVerifiedWindow(
            Main::getBaseAddress(), map_storage_anvil_token_, tick_ms,
            *map_storage_anvil_window_, &manager) || !manager) {
        if (error) *error = "native anvil manager is not bound to this window";
        return false;
    }
    const uint64_t session =
        GetProjectionPrinterInventoryMailboxSessionGeneration();
    if (!session || session !=
            GetProjectionPrinterInventoryMailboxSessionGeneration()) {
        if (error) *error = "anvil response session changed during window read";
        return false;
    }
    evidence->session_generation = session;
    evidence->token = map_storage_anvil_token_;
    evidence->window_id = capture.container_id;
    evidence->container_type = capture.container_type;
    evidence->x = capture.x;
    evidence->y = capture.y;
    evidence->z = capture.z;
    evidence->opened = capture.container_opened;
    evidence->closed = capture.container_closed;
    evidence->content_observed = true;
    evidence->slot_count = capture.slot_count;
    evidence->input_slot_empty = true;
    for (const auto& item : capture.items) {
        if (item.slot == 1U) evidence->input_slot_empty = false;
    }
    return true;
#endif
}

bool BuildImportRuntime::readMapStorageAnvilNativeInput(
        const MapAnvilRenameRecord& record, MapAnvilInputProof* proof,
        std::string* error) {
    if (proof) *proof = {};
    LOGI("[map-anvil-stage] native-input enter phase=%u request=%d",
         static_cast<unsigned>(record.phase), record.input_request_id);
    if (!proof || !map_storage_pipeline_active_ ||
        (record.phase != MapAnvilRenamePhase::InputResponseAccepted &&
         record.phase != MapAnvilRenamePhase::InputConfirmed &&
         record.phase != MapAnvilRenamePhase::CraftDispatchArmed &&
         record.phase != MapAnvilRenamePhase::CraftResponseAccepted)) {
        if (error) *error = "native anvil input request is not in a verified phase";
        return false;
    }
    MapAnvilRenameWindowEvidence opened;
    if (!observeMapStorageAnvil(record.anvil_x, record.anvil_y,
                                record.anvil_z, false, &opened, error) ||
        opened.token != record.input_response.window_token ||
        opened.window_id != record.input_response.window_id ||
        opened.session_generation !=
            record.input_response.session_generation) {
        if (error && error->empty()) {
            *error = "native input belongs to another anvil window";
        }
        return false;
    }
#if !defined(__ANDROID__)
    if (error) *error = "native anvil input requires Android";
    return false;
#else
    const uint64_t tick_ms = mapStorageMonotonicMs(
        std::chrono::steady_clock::now());
    uintptr_t manager = 0U;
    if (!BorrowMapNativeAnvilManagerForVerifiedWindow(
            Main::getBaseAddress(), opened.token, tick_ms,
            *map_storage_anvil_window_, &manager) || !manager) {
        if (error) *error = "native anvil input manager binding was lost";
        return false;
    }
    ProjectionPrinterNativeAnvilInputMapSnapshot native;
    LOGI("[map-anvil-stage] native-input read begin window=%u manager=%p",
         static_cast<unsigned>(opened.window_id),
         reinterpret_cast<void*>(manager));
    if (!ReadProjectionPrinterNativeAnvilInputMap(
            manager, record.map_runtime_item_id, record.map_uuid,
            &native, error)) {
        LOGI("[map-anvil-stage] native-input read failed reason=%s",
             error ? error->c_str() : "unavailable");
        return false;
    }
    LOGI("[map-anvil-stage] native-input read ok window=%u net=%d",
         static_cast<unsigned>(opened.window_id), native.network_stack_id);
    ContainerCaptureResult after;
    if (!map_storage_anvil_window_->verifiedLiveCapture(
            opened.token, tick_ms, &after, error) ||
        after.container_id != opened.window_id ||
        GetProjectionPrinterInventoryMailboxSessionGeneration() !=
            opened.session_generation) {
        if (error && error->empty()) *error = "anvil window changed during native input read";
        return false;
    }
    proof->world_id = record.world_id;
    proof->dimension_id = record.dimension_id;
    proof->anvil_x = record.anvil_x;
    proof->anvil_y = record.anvil_y;
    proof->anvil_z = record.anvil_z;
    proof->fresh_window_token = opened.token;
    proof->window_id = opened.window_id;
    proof->input_slot = 1U;
    proof->count = native.count;
    proof->runtime_item_id = native.runtime_item_id;
    proof->network_stack_id = native.network_stack_id;
    proof->map_uuid = native.map_uuid;
    proof->native_readback = true;
    return true;
#endif
}

bool BuildImportRuntime::captureMapStorageChest(
        const MapChestPosition& position, bool reopen,
        ContainerCaptureResult* capture, std::string* error) {
    if (capture) *capture = {};
    if (error) error->clear();
#if !defined(__ANDROID__)
    (void)position;
    (void)reopen;
    if (error) *error = "native map chest capture is Android-only";
    return false;
#else
    if (!capture || !in_game_tick_ || !IsMinecraftUpdateGameThread() ||
        !map_creation_active_ || stage_ != ExecuteStage::MapStorageHandoff ||
        map_storage_chest_stop_requested_ || current_world_.world_id.empty() ||
        !controller_.worldMatches(current_world_)) {
        if (error) *error = "map chest capture lacks the active game/world context";
        return false;
    }
    // The visible anvil can finish its server Close before its client-side
    // screen/controller handoff has settled. Do not open the next container
    // immediately after that Close; a chest Place in that narrow transition
    // was rejected even though its later read-only chest capture was valid.
    if (!reopen && !map_storage_chest_capture_ &&
        map_storage_anvil_window_ && map_storage_anvil_handoff_ready_ &&
        map_storage_anvil_local_close_started_at_ !=
            std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() -
            map_storage_anvil_local_close_started_at_ <
            std::chrono::milliseconds(800)) {
        if (error) *error = "waiting for anvil-to-chest handoff to settle";
        return false;
    }
    if (!map_storage_chest_capture_) {
        map_storage_chest_client_sync_ticket_ = 0U;
        map_storage_chest_client_sync_token_ = 0U;
        map_storage_chest_client_sync_request_id_ = 0;
        map_storage_chest_client_sync_confirmed_ = false;
        map_storage_chest_client_sync_deadline_ = {};
        map_storage_chest_client_sync_error_.clear();
        map_storage_chest_world_ = current_world_;
        map_storage_chest_dimension_token_ = native_dimension_token_;
        map_storage_chest_preflight_.expected = {
            0U, current_world_.world_id, native_dimension_token_,
            position.x, position.y, position.z, 1, 4U};
        map_storage_chest_native_preflight_ =
            MakeNativeMapChestProductionPreflight(&map_storage_chest_preflight_);
        auto adapter = std::make_unique<MapStorageChestCaptureAdapter>();
        MapStorageChestCaptureConfig config;
        config.window_ops = MakeNativeMapChestWindowOps(
            &map_storage_chest_native_preflight_);
        config.guard_context = this;
        config.try_arm_capture = [](void* context, uint64_t token, int32_t x,
                                    int32_t y, int32_t z) {
            auto* self = static_cast<BuildImportRuntime*>(context);
            if (!self || !self->map_storage_chest_capture_) return false;
            // Only the transfer window needs a stock-client container
            // context. The later window is read-only proof and stays hidden.
            const bool initial = token ==
                self->map_storage_chest_capture_->initial_token();
            const bool armed = initial
                ? TryArmVisibleChestCapture(token, x, y, z)
                : TryArmHiddenChestCapture(token, x, y, z);
            if (armed) {
                LOGI("[map-chest-window] armed token=%llu mode=%s pos=(%d,%d,%d)",
                     static_cast<unsigned long long>(token),
                     initial ? "visible-transfer" : "hidden-proof", x, y, z);
            }
            return armed;
        };
        config.verify_context = [](void* context,
                                   const MapChestPosition& target,
                                   uint64_t token, std::string* detail) {
            auto* self = static_cast<BuildImportRuntime*>(context);
            if (!self->in_game_tick_ || !IsMinecraftUpdateGameThread() ||
                !self->map_storage_chest_capture_ || token == 0U ||
                (token != self->map_storage_chest_capture_->initial_token() &&
                 token != self->map_storage_chest_capture_->reopen_token()) ||
                !(self->current_world_ == self->map_storage_chest_world_)) {
                if (detail) *detail = "map chest window belongs to another game context";
                return false;
            }
            auto& expected = self->map_storage_chest_preflight_.expected;
            if (target.x != expected.x || target.y != expected.y ||
                target.z != expected.z ||
                expected.world_context != self->map_storage_chest_world_.world_id ||
                expected.dimension_token !=
                    self->map_storage_chest_dimension_token_) {
                if (detail) *detail = "map chest target changed while its window was open";
                return false;
            }
            expected.token = token;
            const MapChestWindowRequest request{
                token, target.x, target.y, target.z, expected.face};
            return VerifyMapChestProductionContext(
                &self->map_storage_chest_preflight_, request, detail);
        };
        config.next_token = [](void*) {
            static std::atomic<uint64_t> sequence{0U};
            const uint64_t next = sequence.fetch_add(
                1U, std::memory_order_relaxed) + 1U;
            return next < (uint64_t{1} << 60U)
                ? (uint64_t{1} << 60U) | next : 0U;
        };
        config.ui_context = this;
        config.request_ui_close = [](void* context, uint64_t token,
                                     uint8_t id, uint8_t type) {
            auto* self = static_cast<BuildImportRuntime*>(context);
            if (!self || !self->in_game_tick_ ||
                !IsMinecraftUpdateGameThread() ||
                !self->map_storage_chest_capture_ ||
                (self->map_storage_chest_capture_->state() !=
                     MapStorageChestCaptureState::InitialReady &&
                 self->map_storage_chest_capture_->state() !=
                     MapStorageChestCaptureState::Stopping) ||
                self->map_storage_chest_capture_->initial_token() != token ||
                !(self->current_world_ == self->map_storage_chest_world_) ||
                self->native_dimension_token_ !=
                    self->map_storage_chest_dimension_token_) return false;
            auto& expected = self->map_storage_chest_preflight_.expected;
            expected.token = token;
            const MapChestWindowRequest request{
                token, expected.x, expected.y, expected.z, expected.face};
            std::string detail;
            if (!VerifyMapChestProductionContext(
                    &self->map_storage_chest_preflight_, request, &detail)) {
                LOGI("[map-chest-ui] refusing Back token=%llu window=%u reason=%s",
                     static_cast<unsigned long long>(token),
                     static_cast<unsigned>(id), detail.c_str());
                return false;
            }
            const bool queued = QueueMapChestUiClose(
                token, id, type, expected.x, expected.y, expected.z);
            LOGI("[map-chest-ui] Back queue token=%llu window=%u type=%u result=%d",
                 static_cast<unsigned long long>(token),
                 static_cast<unsigned>(id), static_cast<unsigned>(type),
                 queued ? 1 : 0);
            return queued;
        };
        config.ui_close_dispatched = [](void*, uint64_t token) {
            return WasMapChestUiCloseDispatched(token);
        };
        config.ui_close_outbound_observed = [](void*, uint64_t token) {
            return WasMapChestOutboundCloseObserved(token);
        };
        config.cancel_ui_close = [](void*, uint64_t token) {
            CancelMapChestUiClose(token);
        };
        config.click_face = 1;
        if (!adapter->configure(config, error)) return false;
        map_storage_chest_capture_ = std::move(adapter);
    }
    if (!(current_world_ == map_storage_chest_world_) ||
        native_dimension_token_ != map_storage_chest_dimension_token_) {
        if (error) *error = "map chest task world changed";
        return false;
    }
    if (map_storage_chest_capture_->state() ==
            MapStorageChestCaptureState::Idle) {
        auto& expected = map_storage_chest_preflight_.expected;
        expected.token = 0U;
        expected.x = position.x;
        expected.y = position.y;
        expected.z = position.z;
    }
    if (reopen && map_storage_chest_capture_->state() ==
            MapStorageChestCaptureState::InitialReady &&
        !map_storage_chest_client_sync_confirmed_) {
        const auto now = std::chrono::steady_clock::now();
        const auto refuse = [&](const std::string& detail) {
            map_storage_chest_client_sync_error_ = detail;
            map_storage_chest_terminal_failure_ = true;
            if (error) *error = detail;
            return false;
        };
        if (map_storage_chest_client_sync_deadline_ ==
            std::chrono::steady_clock::time_point{}) {
            map_storage_chest_client_sync_deadline_ = now +
                std::chrono::seconds(8);
        }
        if (now >= map_storage_chest_client_sync_deadline_) {
            return refuse("accepted chest Place did not refresh the local hotbar in time");
        }
        MapChestTransferRecord transfer;
        std::string journal_error;
        if (LoadMapChestTransferJournal(map_state_path_, &transfer,
                                        &journal_error) !=
                MapChestJournalLoad::Loaded ||
            (transfer.phase != MapChestTransferPhase::DispatchArmed &&
             transfer.phase != MapChestTransferPhase::ResponseAccepted) ||
            transfer.request_id >= 0 ||
            transfer.pre_send_capture_token !=
                map_storage_chest_capture_->initial_token() ||
            transfer.tile_cursor != map_tile_cursor_ ||
            transfer.tile_count != map_tile_count_ ||
            transfer.chest_x != position.x ||
            transfer.chest_y != position.y ||
            transfer.chest_z != position.z) {
            return refuse("chest client sync lost its exact durable Place journal: " +
                          journal_error);
        }
        const uint64_t token = transfer.pre_send_capture_token;
        if (map_storage_chest_client_sync_token_ != 0U &&
            (map_storage_chest_client_sync_token_ != token ||
             map_storage_chest_client_sync_request_id_ !=
                 transfer.request_id)) {
            return refuse("chest client sync request or window changed");
        }
        ContainerCaptureResult opened;
        if (PollContainerCapture(token, &opened) !=
                ContainerCapturePollState::Ready ||
            !opened.container_opened || opened.container_closed ||
            opened.container_type != 0U || opened.container_id == 0U ||
            opened.container_id == 0xFFU ||
            opened.x != position.x || opened.y != position.y ||
            opened.z != position.z) {
            return refuse("visible chest closed before hotbar client sync");
        }
        uint8_t source_slot = 0U;
        if (!GetMapChestClientSyncSourceSlot(
                token, transfer.request_id, &source_slot)) {
            return refuse("chest client sync lost its prepared source slot");
        }
        ProjectionPrinterNativeInventorySnapshot native;
        std::string native_error;
        if (!ReadProjectionPrinterNativeInventorySnapshot(&native,
                                                          &native_error) ||
            !native.ready) {
            if (error) *error = "waiting for native hotbar readback: " + native_error;
            return false;
        }
        const auto& source = native.slots[source_slot];
        if (source.occupied) {
            if (!source.has_network_stack_id || source.count != 1U ||
                source.runtime_item_id != transfer.source_runtime_item_id ||
                source.network_stack_id !=
                    transfer.source_network_stack_id) {
                return refuse("hotbar slot changed after the accepted chest Place");
            }
            int64_t live_uuid = -1;
            if (!ReadProjectionPrinterNativeMapUuid(
                    source_slot, source.network_stack_id,
                    &live_uuid, &native_error) ||
                live_uuid != transfer.source_map_uuid) {
                return refuse("hotbar map UUID changed after chest Place");
            }
        }
        if (map_storage_chest_client_sync_ticket_ == 0U) {
            ProjectionPrinterInventoryResponse response;
            if (!GetProjectionPrinterInventoryResponseByRequestId(
                    transfer.request_id, &response)) {
                if (error) *error = "waiting for the exact chest Place response";
                return false;
            }
            std::string expected_title;
            if (!FormatMapTileName(transfer.tile_cursor, map_tile_columns_,
                                   map_tile_rows_, &expected_title)) {
                return refuse("chest client sync cannot determine the map tile title");
            }
            const ProjectionPrinterInventoryResponseSlot* destination = nullptr;
            for (const auto& slot : response.slots) {
                if (slot.container_id == 7U &&
                    slot.slot == transfer.chest_slot) {
                    if (destination) {
                        return refuse("chest Place response repeated its destination slot");
                    }
                    destination = &slot;
                }
            }
            if (!destination) {
                return refuse("chest Place response omitted the destination slot");
            }
            // Some servers omit the optional custom-name string from a
            // successful ItemStackResponse. Reject a contradictory name;
            // the renamed source was already read back with its exact title.
            if (!destination->custom_name.empty() &&
                destination->custom_name != expected_title) {
                return refuse("chest Place response reported a different map title");
            }
            uint64_t ticket = 0U;
            std::string sync_error;
            if (!QueueMapChestClientSyncAcceptedTransfer(
                    token, opened.container_id, transfer.request_id,
                    response, &ticket, &sync_error) || ticket == 0U) {
                return refuse("chest client sync rejected the Place response: " +
                              sync_error);
            }
            if (transfer.phase == MapChestTransferPhase::DispatchArmed &&
                !NoteMapChestTransferAccepted(map_state_path_, transfer,
                                              transfer.request_id,
                                              &journal_error)) {
                return refuse("accepted chest Place could not be journaled: " +
                              journal_error);
            }
            map_storage_chest_client_sync_ticket_ = ticket;
            map_storage_chest_client_sync_token_ = token;
            map_storage_chest_client_sync_request_id_ =
                transfer.request_id;
            LOGI("[map-chest-local-sync] queued request=%d token=%llu window=%u source=%u ticket=%llu",
                 transfer.request_id,
                 static_cast<unsigned long long>(token),
                 static_cast<unsigned>(opened.container_id),
                 static_cast<unsigned>(source_slot),
                 static_cast<unsigned long long>(ticket));
            if (error) *error = "waiting for accepted chest hotbar refresh";
            return false;
        }
        const auto ticket_state = GetMapChestClientSyncTicketState(
            map_storage_chest_client_sync_ticket_);
        if (ticket_state == MapChestClientSyncTicketState::Cancelled ||
            ticket_state == MapChestClientSyncTicketState::Unknown) {
            return refuse("accepted chest hotbar refresh was cancelled");
        }
        if (ticket_state != MapChestClientSyncTicketState::Complete ||
            source.occupied) {
            if (error) *error = "waiting for local hotbar slot to become empty";
            return false;
        }
        LOGI("[map-chest-local-sync] confirmed request=%d token=%llu source=%u ticket=%llu",
             transfer.request_id,
             static_cast<unsigned long long>(token),
             static_cast<unsigned>(source_slot),
             static_cast<unsigned long long>(map_storage_chest_client_sync_ticket_));
        map_storage_chest_client_sync_confirmed_ = true;
        CancelMapChestClientSyncWindow(token);
    }
    const uint64_t now_ms = mapStorageMonotonicMs(
        std::chrono::steady_clock::now());
    if (reopen) {
        // No second ClickBlock: the exact server-accepted Place and local
        // source-slot sync replace the former hidden chest proof.
        return map_storage_chest_capture_->closeAfterPlace(
            position, now_ms, error);
    }
    return map_storage_chest_capture_->capture(
        position, false, now_ms, capture, error);
#endif
}

void BuildImportRuntime::tickMapStorageHandoff() {
    if (map_storage_chest_terminal_failure_ ||
        map_storage_anvil_terminal_failure_ ||
        (map_storage_chest_capture_ &&
        (map_storage_chest_stop_requested_ ||
         map_storage_chest_capture_->state() ==
             MapStorageChestCaptureState::Failed))) {
        std::string reason = map_storage_chest_client_sync_error_.empty()
            ? "automatic chest/anvil window did not close or lost its original world"
            : "automatic chest verification failed: " +
                  map_storage_chest_client_sync_error_;
        if (map_storage_chest_capture_ &&
            !map_storage_chest_capture_->error().empty()) {
            reason += ": " + map_storage_chest_capture_->error();
        }
        if (map_storage_anvil_window_ &&
            !map_storage_anvil_window_->error().empty()) {
            reason += ": " + map_storage_anvil_window_->error();
        }
        pauseMapCreation(reason);
        return;
    }
    MapStorageRuntimeSnapshot storage;
    std::string snapshot_error;
    if (!loadMapStorageRuntimeSnapshot(
            map_state_path_, current_world_, map_source_bounds_,
            map_tile_columns_, map_tile_rows_, map_tile_count_,
            map_tile_cursor_, map_use_new_filled_uuid_,
            &storage, &snapshot_error)) {
        pauseMapCreation("map storage journal read failed: " + snapshot_error);
        return;
    }
    bool debug_preexisting_pair_verified = false;
#if defined(__ANDROID__) && defined(MAP_ANVIL_DEBUG_TRACE) && MAP_ANVIL_DEBUG_TRACE
    // Only a legacy test task that has already journaled its rename may
    // adopt the user's existing pair. Fresh tasks always create their own
    // durable support/chest/anvil plan, even if the old test property remains.
    // This is re-established from native blocks on every tick; it is never a
    // synthetic server ACK or a production placement journal.
    if (!storage.pair &&
        storage.rename &&
        (storage.input.checkpoint_tile_cursor < storage.input.tile_count ||
         storage.input.has_pending_map_use_marker ||
         storage.has_storage_journals)) {
        MapChestAnvilPosition pair;
        NativeWorldReader test_reader;
        NativeBlockInfo chest;
        NativeBlockInfo anvil;
        if (!map_storage_pipeline_active_ ||
            !debugPreexistingMapPair(&pair) || !in_game_tick_ ||
            !IsMinecraftUpdateGameThread() ||
            !controller_.worldMatches(current_world_) ||
            !test_reader.open() ||
            !test_reader.getBlock(pair.chest.x, pair.chest.y,
                                  pair.chest.z, &chest) ||
            !test_reader.getBlock(pair.anvil.x, pair.anvil.y,
                                  pair.anvil.z, &anvil) ||
            chest.name != "minecraft:chest" ||
            (anvil.name != "minecraft:anvil" &&
             anvil.name != "minecraft:chipped_anvil" &&
             anvil.name != "minecraft:damaged_anvil")) {
            pauseMapCreation("Debug preplaced chest/anvil pair is not readable at its exact coordinates");
            return;
        }
        storage.pair.emplace();
        storage.pair->world_id = current_world_.world_id;
        storage.pair->dimension_id = current_world_.dimension_id;
        storage.pair->tile_count = map_tile_count_;
        storage.pair->artwork_bounds = map_source_bounds_;
        storage.pair->pair = pair;
        storage.pair->phase = MapPairPlacementPhase::PairConfirmed;
        storage.input.pair = &*storage.pair;
        debug_preexisting_pair_verified = true;
    }
#endif
    // Only the pre-anvil placement phase may recover a live UUID from the
    // unique newly-created native inventory stack. Once rename/chest intent
    // is journaled, an Armed/ACK recovery never consults backpack state to
    // start another send; the classifier uses durable IDs for no-resend only.
    if (map_use_new_filled_uuid_ == -1 && !storage.input.rename &&
        !storage.input.chest && storage.input.has_pending_map_use_marker &&
        storage.input.pending_map_use_cursor == map_tile_cursor_) {
        MapUsePendingDiskV1 marker;
        if (loadMapUsePending(map_state_path_, &marker, nullptr)) {
            MapInventorySnapshot inventory;
            int32_t network_id = 0;
            int64_t map_uuid = -1;
            if (ReadMapInventorySnapshot(&inventory, nullptr) &&
                findNewFilledMap(inventory,
                                 marker.filled_map_network_ids_before,
                                 &network_id) &&
                findFilledMapUuid(inventory, network_id, &map_uuid)) {
                map_use_new_filled_network_id_ = network_id;
                map_use_new_filled_uuid_ = map_uuid;
                storage.input.expected_map_uuid = map_uuid;
            }
        }
    }
    if (storage.rename &&
        storage.rename->phase == MapAnvilRenamePhase::RenamedMapConfirmed &&
        map_storage_anvil_window_) {
        // A visible anvil owns the same exclusive container mailbox as the
        // later chest. Close its UI and release the mailbox before the chest
        // driver may tick; probe-object retirement is tracked separately.
        map_storage_anvil_stop_requested_ = true;
        map_storage_anvil_local_close_requested_ = true;
        if (map_storage_anvil_local_close_started_at_ ==
            std::chrono::steady_clock::time_point{}) {
            map_storage_anvil_local_close_started_at_ =
                std::chrono::steady_clock::now();
        }
        serviceMapStorageAnvil(std::chrono::steady_clock::now());
        if (map_storage_anvil_window_ &&
            !map_storage_anvil_handoff_ready_) return;
    }
    // The chosen storage cells are deliberately beyond the complete map
    // footprint. After filling the map the player is still at its centre,
    // which can leave the exterior fill target outside the loaded chunks.
    // Reach a surveyed solid pixel-art landing first; an unconfirmed command
    // journal uses this same gate when resumed, but is never resent here.
    const bool active_storage_tile = storage.input.has_pending_map_use_marker &&
        storage.input.pending_map_use_cursor == map_tile_cursor_;
    std::optional<MapChestPosition> placement_target;
    bool extra_chest_ready = true;
    if (storage.pair &&
        storage.pair->phase != MapPairPlacementPhase::PairConfirmed) {
        placement_target = storage.pair->pair.chest;
    } else if (active_storage_tile && storage.pair) {
        MapTileChestAddress address;
        if (!ResolveMapTileChestAddress(map_tile_cursor_, map_tile_count_,
                                        &address)) {
            pauseMapCreation("map storage target chest index is invalid");
            return;
        }
        if (address.chest_index != 0U) {
            const size_t index = static_cast<size_t>(address.chest_index - 1U);
            extra_chest_ready = index < storage.input.extra_chests.size() &&
                storage.input.extra_chests[index].phase ==
                    MapExtraChestPlacementPhase::Confirmed;
            if (index < storage.input.extra_chests.size() &&
                !extra_chest_ready) {
                placement_target = storage.input.extra_chests[index].chest;
            }
        }
    }
    if (placement_target) {
        if (!in_game_tick_ || !IsMinecraftUpdateGameThread() ||
            !controller_.worldMatches(current_world_)) {
            pauseMapCreation("map storage placement travel needs the original live world");
            return;
        }
        const auto placement_now = std::chrono::steady_clock::now();
        if (!(map_storage_placement_target_ == *placement_target)) {
            map_storage_placement_target_ = *placement_target;
            map_storage_placement_floor_ = {};
            map_storage_placement_pending_ = false;
            map_storage_placement_ready_ = false;
            map_storage_placement_ready_at_ = {};
            map_storage_placement_arrival_at_ = {};
            map_storage_placement_deadline_ = {};
            map_storage_placement_log_at_ = {};
        }
        NativeWorldReader landing_reader;
        if (!landing_reader.open()) {
            pauseMapCreation("map storage placement landing cannot read the live world");
            return;
        }
        if (!map_storage_placement_pending_ &&
            !map_storage_placement_ready_) {
            MapChestPosition landing;
            if (!findSafeMapStoragePlacementLanding(
                    &landing_reader, map_source_bounds_,
                    *placement_target, &landing)) {
                LOGI("[map-storage-placement] no-safe-landing target=(%d,%d,%d) artwork=(%d,%d,%d)..(%d,%d,%d)",
                     placement_target->x, placement_target->y,
                     placement_target->z, map_source_bounds_.min_x,
                     map_source_bounds_.min_y, map_source_bounds_.min_z,
                     map_source_bounds_.max_x, map_source_bounds_.max_y,
                     map_source_bounds_.max_z);
                pauseMapCreation(
                    "no solid pixel-art landing within 24 blocks of the planned map storage site");
                return;
            }
            map_storage_placement_floor_ = landing;
            map_storage_placement_pending_ = true;
            map_storage_placement_ready_at_ = {};
            map_storage_placement_arrival_at_ = {};
            map_storage_placement_deadline_ =
                placement_now + kMapStorageTravelTimeout;
            int32_t player_x = 0;
            int32_t player_y = 0;
            int32_t player_z = 0;
            const bool already_landed =
                ReadVerifiedMapNativePlayerBlockPosition(
                    &player_x, &player_y, &player_z, nullptr) &&
                player_x == landing.x && player_z == landing.z;
            if (!already_landed) {
                std::vector<std::string> commands =
                    containerItemPrepareCommands(
                        map_source_bounds_, landing.x, landing.y,
                        landing.z, false);
                if (!commands.empty()) {
                    // An integer /tp X/Z lands on a block corner and can
                    // collide into a neighbouring cell. Only this
                    // pre-placement trip uses the surveyed block centre.
                    commands.back() = "/tp @s " +
                        std::to_string(static_cast<double>(landing.x) + 0.5) +
                        " " + std::to_string(static_cast<int64_t>(landing.y) + 1) +
                        " " + std::to_string(static_cast<double>(landing.z) + 0.5);
                }
                size_t sent = 0U;
                if (commands.empty() || !executeCommands(commands, &sent) ||
                    sent != commands.size()) {
                    pauseMapCreation(
                        "map storage safe-landing teleport outcome is uncertain");
                    return;
                }
                NativeWorldAccess::invalidateLocalPlayerBlockPositionCache();
                LOGI("[map-storage-placement] teleport command=%s",
                     commands.back().c_str());
            }
            LOGI("[map-storage-placement] travel target=(%d,%d,%d) landing_floor=(%d,%d,%d) player=(%d,%d,%d) teleport=%d settle_ms=1000",
                 placement_target->x, placement_target->y,
                 placement_target->z, landing.x, landing.y, landing.z,
                 player_x, player_y, player_z, already_landed ? 0 : 1);
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = "moving to safe map storage placement site";
            return;
        }
        int32_t player_x = 0;
        int32_t player_y = 0;
        int32_t player_z = 0;
        std::string native_position_error;
        const bool position_read =
            ReadVerifiedMapNativePlayerBlockPosition(
                &player_x, &player_y, &player_z, &native_position_error);
        // The server can settle a teleport at a different surface height.
        // The original live-world check above and the native LocalPlayer X/Z
        // prove arrival; Y is logged, never used as an exact-height gate.
        const bool near = position_read &&
            player_x == map_storage_placement_floor_.x &&
            player_z == map_storage_placement_floor_.z;
        const MapStorageLandingObservation landing_observation =
            observeSafeMapStorageLanding(
                &landing_reader, map_storage_placement_floor_);
        const bool landed = near;
        if (map_storage_placement_pending_) {
            if (landed && map_storage_placement_arrival_at_ ==
                    std::chrono::steady_clock::time_point{}) {
                map_storage_placement_arrival_at_ = placement_now;
                map_storage_placement_ready_at_ =
                    placement_now + std::chrono::seconds(1);
            } else if (!landed) {
                map_storage_placement_arrival_at_ = {};
                map_storage_placement_ready_at_ = {};
            }
            if (landed && map_storage_placement_arrival_at_ !=
                    std::chrono::steady_clock::time_point{} &&
                placement_now - map_storage_placement_arrival_at_ >=
                    std::chrono::seconds(1)) {
                map_storage_placement_pending_ = false;
                map_storage_placement_ready_ = true;
                LOGI("[map-storage-placement] arrived target=(%d,%d,%d) landing_floor=(%d,%d,%d) player=(%d,%d,%d) settled=1 landing_safe=%d",
                     placement_target->x, placement_target->y,
                     placement_target->z, map_storage_placement_floor_.x,
                     map_storage_placement_floor_.y,
                     map_storage_placement_floor_.z,
                     player_x, player_y, player_z,
                     landing_observation.safe() ? 1 : 0);
            } else if (placement_now >= map_storage_placement_deadline_) {
                LOGI("[map-storage-placement] timeout target=(%d,%d,%d) floor=(%d,%d,%d) pos_ok=%d player=(%d,%d,%d) near=%d ground=%d:%s feet=%d:%s head=%d:%s safe=%d native_diag=%s pos_error=%s",
                     placement_target->x, placement_target->y,
                     placement_target->z, map_storage_placement_floor_.x,
                     map_storage_placement_floor_.y,
                     map_storage_placement_floor_.z,
                     position_read ? 1 : 0, player_x, player_y, player_z,
                     near ? 1 : 0,
                     landing_observation.ground_read ? 1 : 0,
                     landing_observation.ground_read
                         ? landing_observation.ground.name.c_str() : "unreadable",
                     landing_observation.feet_read ? 1 : 0,
                     landing_observation.feet_read
                         ? landing_observation.feet.name.c_str() : "unreadable",
                     landing_observation.head_read ? 1 : 0,
                     landing_observation.head_read
                         ? landing_observation.head.name.c_str() : "unreadable",
                     landing_observation.safe() ? 1 : 0,
                     NativeWorldAccess::lastWorldReaderDiagnostic(),
                     native_position_error.c_str());
                pauseMapCreation(
                    "map storage safe-landing teleport did not arrive or landing changed");
                return;
            } else {
                if (map_storage_placement_log_at_ ==
                        std::chrono::steady_clock::time_point{} ||
                    placement_now - map_storage_placement_log_at_ >=
                        std::chrono::seconds(1)) {
                    LOGI("[map-storage-placement] waiting target=(%d,%d,%d) floor=(%d,%d,%d) pos_ok=%d player=(%d,%d,%d) near=%d ground=%d:%s feet=%d:%s head=%d:%s safe=%d settled=%d native_diag=%s pos_error=%s",
                         placement_target->x, placement_target->y,
                         placement_target->z, map_storage_placement_floor_.x,
                         map_storage_placement_floor_.y,
                         map_storage_placement_floor_.z,
                         position_read ? 1 : 0, player_x, player_y, player_z,
                         near ? 1 : 0,
                         landing_observation.ground_read ? 1 : 0,
                         landing_observation.ground_read
                             ? landing_observation.ground.name.c_str() : "unreadable",
                         landing_observation.feet_read ? 1 : 0,
                         landing_observation.feet_read
                             ? landing_observation.feet.name.c_str() : "unreadable",
                         landing_observation.head_read ? 1 : 0,
                         landing_observation.head_read
                             ? landing_observation.head.name.c_str() : "unreadable",
                         landing_observation.safe() ? 1 : 0,
                         map_storage_placement_arrival_at_ !=
                                 std::chrono::steady_clock::time_point{} &&
                             placement_now >= map_storage_placement_ready_at_
                             ? 1 : 0,
                         NativeWorldAccess::lastWorldReaderDiagnostic(),
                         native_position_error.c_str());
                    map_storage_placement_log_at_ = placement_now;
                }
                return;
            }
        } else if (!landed) {
            map_storage_placement_ready_ = false;
            map_storage_placement_arrival_at_ = {};
            map_storage_placement_ready_at_ = {};
            return;
        }
    } else {
        map_storage_placement_pending_ = false;
        map_storage_placement_ready_ = false;
        map_storage_placement_arrival_at_ = {};
        map_storage_placement_ready_at_ = {};
    }
    // Map creation leaves the player at the center of a 128x128 tile. The
    // deliberately exterior storage pair may be 60+ blocks away, whereas
    // the native anvil/chest packet preflights permit only four blocks. Move
    // after the blocks are confirmed and before opening either window. A
    // restored journal keeps its original coordinates; this never selects a
    // replacement site for an in-flight rename or chest transaction.
    const bool needs_anvil_interaction = active_storage_tile &&
        (!storage.rename || storage.rename->phase !=
                                MapAnvilRenamePhase::RenamedMapConfirmed);
    const bool needs_chest_interaction = active_storage_tile &&
        storage.rename && storage.rename->phase ==
                              MapAnvilRenamePhase::RenamedMapConfirmed &&
        (!storage.chest ||
         (storage.chest->phase != MapChestTransferPhase::ReopenConfirmed &&
          storage.chest->phase != MapChestTransferPhase::InventoryConfirmed &&
          storage.chest->phase != MapChestTransferPhase::AcceptedAndClosed));
    if (extra_chest_ready && storage.pair &&
        storage.pair->phase == MapPairPlacementPhase::PairConfirmed &&
        (needs_anvil_interaction || needs_chest_interaction)) {
        MapChestPosition destination = storage.pair->pair.chest;
        MapChestPosition standing_floor = destination;
        uint8_t platform_marker = storage.pair->platform_side;
        bool extra_platform = false;
        if (needs_chest_interaction) {
            MapTileChestAddress address;
            if (!ResolveMapTileChestAddress(map_tile_cursor_, map_tile_count_,
                                            &address)) {
                pauseMapCreation("map storage chest index is invalid");
                return;
            }
            if (address.chest_index != 0U) {
                const size_t index = static_cast<size_t>(address.chest_index - 1U);
                if (index >= storage.input.extra_chests.size() ||
                    storage.input.extra_chests[index].phase !=
                        MapExtraChestPlacementPhase::Confirmed) {
                    pauseMapCreation("map storage target chest is not confirmed");
                    return;
                }
                destination = storage.input.extra_chests[index].chest;
                platform_marker =
                    storage.input.extra_chests[index].platform_corner;
                extra_platform = true;
            }
        }
        standing_floor = destination;
        const bool dedicated_standing_cell = platform_marker != 0U;
        if (dedicated_standing_cell) {
            const bool valid_floor = extra_platform
                ? MapExtraPlatformStandingCell(
                    map_source_bounds_, destination, platform_marker,
                    &standing_floor)
                : MapPairPlatformStandingCell(
                    map_source_bounds_, storage.pair->pair,
                    platform_marker, &standing_floor);
            if (!valid_floor) {
                pauseMapCreation("confirmed map storage platform has invalid standing cell");
                return;
            }
        }
        const auto travel_now = std::chrono::steady_clock::now();
        int32_t player_x = 0;
        int32_t player_y = 0;
        int32_t player_z = 0;
        std::string player_position_error;
        if (!ReadVerifiedMapNativePlayerBlockPosition(
                &player_x, &player_y, &player_z,
                &player_position_error)) {
            pauseMapCreation("fresh native player position is unavailable for map storage: " +
                             player_position_error);
            return;
        }
        auto near_storage_cell = [&](const MapChestPosition& cell) {
            const int64_t dx = static_cast<int64_t>(player_x) - cell.x;
            const int64_t dy = static_cast<int64_t>(player_y) - cell.y;
            const int64_t dz = static_cast<int64_t>(player_z) - cell.z;
            return dx >= -2 && dx <= 2 && dy >= -2 && dy <= 2 &&
                dz >= -2 && dz <= 2 && dx * dx + dy * dy + dz * dz <= 4;
        };
        const bool in_reach = near_storage_cell(destination) &&
            (!needs_anvil_interaction ||
             near_storage_cell(storage.pair->pair.anvil));
        const bool on_standing_cell = !dedicated_standing_cell ||
            (player_x == standing_floor.x && player_z == standing_floor.z);
        if (standing_floor.y > std::numeric_limits<int32_t>::max() - 2) {
            pauseMapCreation("map storage platform has no safe teleport height");
            return;
        }
        // A confirmed exterior chest may be outside the currently loaded
        // map-center chunks. Read the landing only after travel reaches its
        // journaled coordinate; an unreadable or stale remote read is not
        // proof of obstruction. A conflicting block observed after arrival
        // still prevents either container window from opening.
        auto landing_state = [&]() {
            NativeBlockInfo floor_block;
            NativeBlockInfo feet_block;
            NativeBlockInfo head_block;
            if (!NativeWorldAccess::getBlock(standing_floor.x, standing_floor.y,
                                             standing_floor.z, &floor_block) ||
                !NativeWorldAccess::getBlock(standing_floor.x, standing_floor.y + 1,
                                             standing_floor.z, &feet_block) ||
                !NativeWorldAccess::getBlock(standing_floor.x, standing_floor.y + 2,
                                             standing_floor.z, &head_block)) {
                return 0;  // chunk not yet natively readable
            }
            // This game build can report synthetic air and then request
            // placeholder blocks while the destination chunk streams in.
            // Neither snapshot proves that the previously confirmed platform
            // was removed. Wait for the bounded travel deadline, without
            // opening either container until stone and clear space are read.
            if (dedicated_standing_cell &&
                (ShouldRetryMapStorageStandingSurvey(
                     ClassifyMapChestNativeBlock(floor_block),
                     ClassifyMapChestNativeBlock(feet_block),
                     ClassifyMapChestNativeBlock(head_block)) ||
                 IsMapStorageNativeReadPlaceholder(floor_block) ||
                 IsMapStorageNativeReadPlaceholder(feet_block) ||
                 IsMapStorageNativeReadPlaceholder(head_block))) {
                if (travel_now >= map_storage_log_at_) {
                    LOGI("[map-storage] standing-unloaded floor=(%d,%d,%d) blocks=%s/%s/%s player=(%d,%d,%d) waiting_for_chunk=1",
                         standing_floor.x, standing_floor.y, standing_floor.z,
                         floor_block.name.c_str(), feet_block.name.c_str(),
                         head_block.name.c_str(),
                         player_x, player_y, player_z);
                    map_storage_log_at_ = travel_now + std::chrono::seconds(1);
                }
                return 0;
            }
            const bool expected_floor = dedicated_standing_cell
                ? floor_block.name == "minecraft:stone"
                : floor_block.name == "minecraft:chest";
            const bool clear = expected_floor &&
                ClassifyMapChestNativeBlock(feet_block) == MapChestCell::Air &&
                ClassifyMapChestNativeBlock(head_block) == MapChestCell::Air;
            if (!clear) {
                LOGI("[map-storage] standing-conflict floor=(%d,%d,%d) blocks=%s/%s/%s player=(%d,%d,%d)",
                     standing_floor.x, standing_floor.y, standing_floor.z,
                     floor_block.name.c_str(), feet_block.name.c_str(),
                     head_block.name.c_str(), player_x, player_y, player_z);
            }
            return clear ? 1 : 2;
        };
        if (map_storage_travel_pending_) {
            if (!(map_storage_travel_target_ == standing_floor)) {
                pauseMapCreation("map storage travel target changed in flight");
                return;
            }
            const bool arrived = in_reach && on_standing_cell;
            const int landing = arrived ? landing_state() : 0;
            if (landing == 2) {
                pauseMapCreation("confirmed map storage standing cell became obstructed");
                return;
            }
            if (arrived && landing == 1 &&
                map_storage_travel_ready_at_ ==
                    std::chrono::steady_clock::time_point{}) {
                map_storage_travel_ready_at_ =
                    travel_now + std::chrono::seconds(1);
                LOGI("[map-storage] standing arrival observed chest=(%d,%d,%d) floor=(%d,%d,%d) player=(%d,%d,%d) settle_ms=1000",
                     destination.x, destination.y, destination.z,
                     standing_floor.x, standing_floor.y, standing_floor.z,
                     player_x, player_y, player_z);
            } else if (!arrived || landing != 1) {
                map_storage_travel_ready_at_ = {};
            }
            if (arrived && landing == 1 &&
                travel_now >= map_storage_travel_ready_at_) {
                LOGI("[map-storage] arrived near chest=(%d,%d,%d) standing=(%d,%d,%d) player=(%d,%d,%d)",
                      destination.x, destination.y, destination.z,
                      standing_floor.x, standing_floor.y, standing_floor.z,
                      player_x, player_y, player_z);
                map_storage_travel_pending_ = false;
                map_storage_travel_ready_at_ = {};
                map_storage_travel_deadline_ = {};
            } else if (travel_now >= map_storage_travel_deadline_) {
                LOGI("[map-storage] travel timeout chest=(%d,%d,%d) standing=(%d,%d,%d) player=(%d,%d,%d) reach=%d on_standing=%d landing=%d",
                      destination.x, destination.y, destination.z,
                      standing_floor.x, standing_floor.y, standing_floor.z,
                      player_x, player_y, player_z, in_reach ? 1 : 0,
                     on_standing_cell ? 1 : 0, landing);
                pauseMapCreation(arrived && landing == 0
                    ? "map storage standing platform did not become readable after teleport"
                    : "map storage teleport did not reach the confirmed standing cell");
                return;
            } else {
                return;
            }
        } else if (dedicated_standing_cell && in_reach && on_standing_cell) {
            const int landing = landing_state();
            if (landing == 2) {
                pauseMapCreation("confirmed map storage standing cell became obstructed");
                return;
            }
            if (landing == 0) return;
        } else if (!in_reach || !on_standing_cell) {
            const MapStorageChestCaptureState chest_state =
                map_storage_chest_capture_
                    ? map_storage_chest_capture_->state()
                    : MapStorageChestCaptureState::Idle;
            const bool chest_verification_in_flight =
                map_storage_chest_capture_ && storage.chest &&
                (storage.chest->phase == MapChestTransferPhase::DispatchArmed ||
                 storage.chest->phase == MapChestTransferPhase::ResponseAccepted) &&
                chest_state != MapStorageChestCaptureState::Idle &&
                chest_state != MapStorageChestCaptureState::Stopping &&
                chest_state != MapStorageChestCaptureState::Stopped &&
                chest_state != MapStorageChestCaptureState::Failed;
            if (chest_verification_in_flight) {
                // The map Place is already journalled and must never be sent
                // again. A transient player-position change must not abandon
                // its in-flight hidden chest before the fresh proof window
                // can close. Any new Open still has the native near-chest
                // preflight; this only lets the existing session progress.
                if (travel_now >= map_storage_log_at_) {
                    LOGI("[map-storage] chest verification remains active while player drifted chest=(%d,%d,%d) standing=(%d,%d,%d) player=(%d,%d,%d) reach=%d on_standing=%d phase=%u capture_state=%u",
                         destination.x, destination.y, destination.z,
                         standing_floor.x, standing_floor.y, standing_floor.z,
                         player_x, player_y, player_z, in_reach ? 1 : 0,
                         on_standing_cell ? 1 : 0,
                         static_cast<unsigned int>(storage.chest->phase),
                         static_cast<unsigned int>(chest_state));
                    map_storage_log_at_ = travel_now + std::chrono::seconds(1);
                }
            } else {
                if ((map_storage_anvil_window_ &&
                     !map_storage_anvil_handoff_ready_) ||
                    map_storage_chest_capture_) {
                    pauseMapCreation("map storage window is open while player is out of reach");
                    return;
                }
                // The initial pair (or this extra chest) was already
                // confirmed in its durable placement journal. After drawing
                // a later map tile the player may be many chunks away, and
                // this game version can return synthetic air for an unloaded
                // platform even though getBlock reports success. Do not
                // mistake that remote read for a changed world. The pending
                // travel path checks the exact floor/feet/head again only
                // after the player reaches and loads this cell, before any
                // anvil or chest window may open.
                std::vector<std::string> commands = containerItemPrepareCommands(
                    map_source_bounds_, standing_floor.x, standing_floor.y,
                    standing_floor.z, false);
                if (commands.empty()) {
                    pauseMapCreation("map storage teleport target is invalid");
                    return;
                }
                if (dedicated_standing_cell) {
                    commands.back() = "/tp @s " +
                        std::to_string(static_cast<double>(standing_floor.x) + 0.5) +
                        " " + std::to_string(static_cast<int64_t>(standing_floor.y) + 1) +
                        " " +
                        std::to_string(static_cast<double>(standing_floor.z) + 0.5);
                }
                map_storage_travel_target_ = standing_floor;
                map_storage_travel_pending_ = true;
                map_storage_travel_ready_at_ = {};
                map_storage_travel_deadline_ = travel_now + kMapStorageTravelTimeout;
                size_t sent = 0U;
                const bool dispatched = executeCommands(commands, &sent);
                LOGI("[map-storage] teleport chest=(%d,%d,%d) standing=(%d,%d,%d) command=%s commands=%zu sent=%zu result=%d",
                      destination.x, destination.y, destination.z,
                      standing_floor.x, standing_floor.y, standing_floor.z,
                      commands.back().c_str(),
                      commands.size(), sent, dispatched ? 1 : 0);
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = "moving to confirmed map chest/anvil (" +
                    std::to_string(map_tile_cursor_ + 1U) + "/" +
                    std::to_string(map_tile_count_) + ")";
                return;
            }
        }
    }
    if (storage.input.expected_map_uuid == -1 && storage.rename &&
        storage.rename->phase == MapAnvilRenamePhase::RenamedMapConfirmed &&
        (!storage.chest ||
         storage.chest->phase == MapChestTransferPhase::Prepared)) {
        // A persisted rename journal identifies the map to look for, but is
        // not proof that the renamed output still exists in player inventory.
        // A fresh chest Place may resume only after a same-tick native scan
        // finds exactly one stack with the journal UUID and exact full title.
        if (!in_game_tick_ || !IsMinecraftUpdateGameThread() ||
            !controller_.worldMatches(current_world_)) {
            pauseMapCreation("renamed-map recovery needs the original live world and game tick");
            return;
        }
        ProjectionPrinterNativeInventoryFilledMapMatch native;
        if (!ReadProjectionPrinterNativeInventoryFilledMapMatch(
                storage.rename->map_runtime_item_id,
                storage.rename->map_uuid,
                storage.rename->expected_title, &native,
                &snapshot_error)) {
            pauseMapCreation("fresh native renamed-map recovery is unavailable: " +
                             snapshot_error);
            return;
        }
        if (native.inventory_slot < 0 || native.inventory_slot >= 36 ||
            native.count != 1U || native.network_stack_id <= 0 ||
            native.runtime_item_id != storage.rename->map_runtime_item_id ||
            native.map_uuid != storage.rename->map_uuid ||
            native.display_name != storage.rename->expected_title) {
            pauseMapCreation("fresh native renamed-map recovery identity changed");
            return;
        }
        // Do not persist this recovered identity. A prepared chest transfer
        // must re-read the native stack on its next tick; Armed transfers use
        // their durable request ID strictly for no-resend reconciliation.
        storage.input.expected_map_uuid = native.map_uuid;
    }
    if (storage.rename &&
        storage.rename->phase == MapAnvilRenamePhase::RenamedMapConfirmed &&
        !map_storage_chest_capture_ &&
        (!storage.chest ||
         storage.chest->phase == MapChestTransferPhase::Prepared)) {
        // The original anvil result action chooses an empty Inventory(12)
        // hotbar slot dynamically. Locate the exact renamed output
        // before allowing the chest sender to prepare its source slot. A
        // recovered Prepared chest intent may also do this while no chest
        // window is active; an Armed/ACKed request must never be retried. The
        // existing map transfer performs the game's native hotbar selection
        // or the verified silent backpack-to-hotbar exchange as needed.
        if (!in_game_tick_ || !IsMinecraftUpdateGameThread() ||
            !controller_.worldMatches(current_world_)) {
            pauseMapCreation("renamed-map hotbar selection needs the original live world");
            return;
        }
        std::string output_error;
        if (map_storage_renamed_transfer_network_id_ == 0) {
            ProjectionPrinterNativeInventoryFilledMapMatch named;
            if (!ReadProjectionPrinterNativeInventoryFilledMapMatch(
                    storage.rename->map_runtime_item_id,
                    storage.rename->map_uuid,
                    storage.rename->expected_title, &named, &output_error) ||
                named.inventory_slot < 0 || named.inventory_slot >= 36 ||
                named.count != 1U || named.network_stack_id <= 0) {
                pauseMapCreation("renamed map is not uniquely readable before chest storage: " +
                                 output_error);
                return;
            }
            map_storage_renamed_transfer_network_id_ = named.network_stack_id;
            map_storage_renamed_transfer_uuid_ = named.map_uuid;
            map_storage_renamed_transfer_cursor_ = storage.rename->tile_cursor;
        } else if (map_storage_renamed_transfer_uuid_ !=
                       storage.rename->map_uuid ||
                   map_storage_renamed_transfer_cursor_ !=
                       storage.rename->tile_cursor) {
            map_inventory_transfer_.cancel();
            pauseMapCreation("renamed-map transfer changed tile identity");
            return;
        }
        MapInventoryTransfer::ReadyItem selected;
        MapInventoryTransfer::NamedFilledMapIdentity named_identity;
        named_identity.runtime_item_id = storage.rename->map_runtime_item_id;
        named_identity.map_uuid = storage.rename->map_uuid;
        named_identity.title = storage.rename->expected_title;
        const auto transfer = map_inventory_transfer_.tick(
            MapInventoryTransfer::Kind::Filled,
            map_storage_renamed_transfer_network_id_,
            &selected, &output_error, &named_identity);
        if (transfer == MapInventoryTransfer::Result::Waiting) return;
        map_storage_renamed_transfer_network_id_ = 0;
        map_storage_renamed_transfer_uuid_ = -1;
        map_storage_renamed_transfer_cursor_ = 0;
        if (transfer != MapInventoryTransfer::Result::Ready) {
            pauseMapCreation("renamed map cannot be selected for chest storage: " +
                             output_error);
            return;
        }
        ProjectionPrinterNativeInventoryFilledMapMatch selected_map;
        if (!ReadProjectionPrinterNativeInventoryFilledMapMatch(
                storage.rename->map_runtime_item_id,
                storage.rename->map_uuid,
                storage.rename->expected_title, &selected_map,
                &output_error) ||
            selected.hotbar_slot < 0 || selected.hotbar_slot > 8 ||
            selected.network_stack_id <= 0 ||
            selected_map.inventory_slot != selected.hotbar_slot ||
            selected_map.network_stack_id != selected.network_stack_id ||
            selected_map.count != 1U) {
            pauseMapCreation("renamed map hotbar identity changed before chest storage: " +
                             output_error);
            return;
        }
        // The production chest preflight still reads the currently held
        // ItemStack and checks its UUID, exact DisplayName, and selected slot.
    }
    if (storage.rename &&
        storage.rename->phase == MapAnvilRenamePhase::InputConfirmed) {
        MapAnvilInputProof proof;
        if (!readMapStorageAnvilNativeInput(*storage.rename, &proof,
                                            &snapshot_error)) {
            pauseMapCreation("fresh native anvil input is unavailable: " +
                             snapshot_error);
            return;
        }
        storage.fresh_anvil_input = std::move(proof);
        storage.input.fresh_anvil_input = &*storage.fresh_anvil_input;
    }
    NativeWorldReader reader;
    MapStorageProductionHooks hooks;
    hooks.context = this;
    hooks.require_live_world = [](void* context, const std::string& world,
                                   int32_t dimension, std::string* error) {
        auto* self = static_cast<BuildImportRuntime*>(context);
        if (!self->in_game_tick_ || !IsMinecraftUpdateGameThread() ||
            !self->map_creation_active_ ||
            self->stage_ != ExecuteStage::MapStorageHandoff ||
            self->current_world_.world_id != world ||
            self->current_world_.dimension_id != dimension ||
            !self->controller_.worldMatches(self->current_world_)) {
            if (error) *error = "map storage needs the original live world and game tick";
            return false;
        }
        return true;
    };
    hooks.new_command_uuid = [](void* context, std::string* uuid,
                                 std::string* error) {
        auto* self = static_cast<BuildImportRuntime*>(context);
        if (!self->in_game_tick_ || !uuid) {
            if (error) *error = "map storage RPC UUID cannot be reserved";
            return false;
        }
        *uuid = self->nextRpcUuid();
        return !uuid->empty();
    };
    hooks.approve_placement_command = [](void* context,
            const std::string& command, std::string* error) {
        auto* self = static_cast<BuildImportRuntime*>(context);
        if (!self || !self->in_game_tick_ || command.empty()) {
            if (error) *error = "map storage fill lacks its live game tick";
            return false;
        }
        MapPairPlacementRecord pair;
        if (LoadMapPairPlacementJournal(self->map_state_path_, &pair, error) !=
                MapPairPlacementLoad::Loaded ||
            pair.world_id != self->current_world_.world_id ||
            pair.dimension_id != self->current_world_.dimension_id ||
            pair.tile_count != self->map_tile_count_) {
            if (error && error->empty()) {
                *error = "map storage fill lacks its durable pair plan";
            }
            return false;
        }
        const auto& chest = pair.pair.chest;
        const auto& anvil = pair.pair.anvil;
        const MapChestPosition support_first{
            std::min(chest.x, anvil.x), chest.y - 1,
            std::min(chest.z, anvil.z)};
        const MapChestPosition support_last{
            std::max(chest.x, anvil.x), chest.y - 1,
            std::max(chest.z, anvil.z)};
        std::string support_command = formatMapStorageFillCommand(
            support_first, support_last, "minecraft:stone");
        if (pair.platform_side != 0U) {
            std::array<MapChestPosition, 4> cells;
            if (!BuildMapPairSupportPlatformCells(
                    self->map_source_bounds_, pair.pair,
                    pair.platform_side, &cells)) {
                if (error) *error = "map storage pair platform geometry is invalid";
                return false;
            }
            support_command = BuildMapSupportPlatformFillCommand(cells);
        }
        if ((pair.support_created &&
             (pair.phase == MapPairPlacementPhase::SupportSelected ||
              (pair.phase == MapPairPlacementPhase::SupportDispatchArmed &&
                !pair.support_retry_used)) &&
              command == support_command) ||
            ((pair.phase == MapPairPlacementPhase::SupportConfirmed ||
              pair.phase == MapPairPlacementPhase::Selected) &&
             command == formatMapStorageFillCommand(
                 chest, chest, "minecraft:chest")) ||
            (pair.phase == MapPairPlacementPhase::ChestConfirmed &&
             command == formatMapStorageFillCommand(
                 anvil, anvil, "minecraft:anvil"))) {
            return true;
        }
        MapTileChestAddress address;
        MapExtraChestPlacementRecord extra;
        if (ResolveMapTileChestAddress(self->map_tile_cursor_,
                                       self->map_tile_count_, &address) &&
            address.chest_index != 0U &&
            LoadMapExtraChestPlacementJournal(
                self->map_state_path_, address.chest_index, &extra,
                nullptr) == MapExtraChestPlacementLoad::Loaded &&
            extra.world_id == self->current_world_.world_id &&
            extra.dimension_id == self->current_world_.dimension_id &&
            extra.tile_count == self->map_tile_count_ &&
            extra.chest.y > std::numeric_limits<int32_t>::min()) {
            const MapChestPosition support{
                extra.chest.x, extra.chest.y - 1, extra.chest.z};
            std::string extra_support_command = formatMapStorageFillCommand(
                support, support, "minecraft:stone");
            if (extra.platform_corner != 0U) {
                std::array<MapChestPosition, 4> cells;
                if (!BuildMapExtraSupportPlatformCells(
                        self->map_source_bounds_, extra.chest,
                        extra.platform_corner, &cells)) {
                    if (error) *error =
                        "map storage extra platform geometry is invalid";
                    return false;
                }
                extra_support_command = BuildMapSupportPlatformFillCommand(cells);
            }
            if ((extra.support_created &&
                  (extra.phase == MapExtraChestPlacementPhase::SupportSelected ||
                   (extra.phase ==
                        MapExtraChestPlacementPhase::SupportDispatchArmed &&
                    !extra.support_retry_used)) &&
                  command == extra_support_command) ||
                ((extra.phase == MapExtraChestPlacementPhase::Selected ||
                  (extra.support_created &&
                   extra.phase == MapExtraChestPlacementPhase::SupportConfirmed)) &&
                 command == formatMapStorageFillCommand(
                     extra.chest, extra.chest, "minecraft:chest"))) {
                return true;
            }
        }
        if (error) *error = "map storage fill differs from its durable plan";
        return false;
    };
    hooks.dispatch_tracked_rpc = [](void* context, const std::string& uuid,
                                     const std::string& command,
                                     std::string* error) {
        auto* self = static_cast<BuildImportRuntime*>(context);
        if (!self->in_game_tick_ || uuid.empty() || command.empty()) {
            if (error) *error = "tracked map placement RPC is invalid";
            return false;
        }
        self->map_storage_rpc_ack_uuid_.clear();
        self->map_storage_rpc_ack_ = MapPairCommandAck::Pending;
        self->map_storage_rpc_poll_uuid_ = uuid;
        self->map_storage_rpc_poll_deadline_ =
            std::chrono::steady_clock::now() + kRpcBarrierTimeout;
        size_t sent = 0U;
        if (!self->executeTrackedCommands({command}, {uuid}, &sent) ||
            sent != 1U) {
            if (error) *error = "tracked map placement RPC outcome is ambiguous";
            return false;
        }
        LOGI("[map-storage-rpc] dispatched uuid=%s command=%s",
             uuid.c_str(), command.c_str());
        return true;
    };
    hooks.poll_tracked_rpc = [](void* context, const std::string& uuid,
                                 MapStorageTrackedRpcReceipt* receipt,
                                 std::string* error) {
        auto* self = static_cast<BuildImportRuntime*>(context);
        if (!self->in_game_tick_ || uuid.empty() || !receipt) {
            if (error) *error = "map placement ACK poll is invalid";
            return false;
        }
        *receipt = {};
        const auto poll_now = std::chrono::steady_clock::now();
        if (self->map_storage_rpc_poll_uuid_ != uuid) {
            // A persisted Armed journal can be resumed after process death.
            // Its command must never be resent, but it gets one bounded
            // observation window for a delayed exact ACK.
            self->map_storage_rpc_poll_uuid_ = uuid;
            self->map_storage_rpc_poll_deadline_ = poll_now + kRpcBarrierTimeout;
        }
        if (self->map_storage_rpc_ack_uuid_ == uuid &&
            self->map_storage_rpc_ack_ != MapPairCommandAck::Pending) {
            receipt->returned_uuid = self->map_storage_rpc_ack_uuid_;
            receipt->ack = self->map_storage_rpc_ack_;
            return true;
        }
        std::string matched_uuid;
        const RpcResultState state = self->pollTrackedCommands(
            {uuid}, nullptr, &matched_uuid);
        if (state == RpcResultState::Unavailable) {
            if (error) *error = "tracked map placement ACK transport is unavailable";
            return false;
        }
        if (state == RpcResultState::Pending) {
            if (poll_now >= self->map_storage_rpc_poll_deadline_) {
                LOGI("[map-storage-rpc] ACK timeout uuid=%s; checking exact native block state",
                     uuid.c_str());
                if (error) *error = "tracked map placement ACK timed out; journal retained";
                return false;
            }
            return true;
        }
        if (matched_uuid != uuid) {
            if (error) *error = "map placement ACK UUID mismatch";
            return false;
        }
        self->map_storage_rpc_ack_uuid_ = matched_uuid;
        self->map_storage_rpc_ack_ = state == RpcResultState::Accepted
            ? MapPairCommandAck::Accepted : MapPairCommandAck::Rejected;
        receipt->returned_uuid = matched_uuid;
        receipt->ack = self->map_storage_rpc_ack_;
        return true;
    };
    hooks.allow_support_retry = [](void* context,
                                    const MapChestPosition& support_cell,
                                    std::string* error) {
        auto* self = static_cast<BuildImportRuntime*>(context);
        if (!self || !self->in_game_tick_ ||
            !IsMinecraftUpdateGameThread() ||
            !self->map_creation_active_ ||
            self->stage_ != ExecuteStage::MapStorageHandoff ||
            !self->controller_.worldMatches(self->current_world_) ||
            !self->map_storage_placement_ready_ ||
            self->map_storage_placement_pending_ ||
            std::chrono::steady_clock::now() <
                self->map_storage_placement_ready_at_ ||
            support_cell.y == std::numeric_limits<int32_t>::max() ||
            self->map_storage_placement_target_.x != support_cell.x ||
            self->map_storage_placement_target_.y != support_cell.y + 1 ||
            self->map_storage_placement_target_.z != support_cell.z) {
            if (error) *error = "support retry has not reached its original live storage site";
            return false;
        }
        int32_t player_x = 0;
        int32_t player_y = 0;
        int32_t player_z = 0;
        if (!ReadVerifiedMapNativePlayerBlockPosition(
                &player_x, &player_y, &player_z, error) ||
            player_x != self->map_storage_placement_floor_.x ||
            player_z != self->map_storage_placement_floor_.z) {
            if (error && error->empty()) {
                *error = "support retry lost its stable near-site X/Z";
            }
            return false;
        }
        return true;
    };
    hooks.capture_chest = [](void* context, const MapChestPosition& position,
                              bool reopen, ContainerCaptureResult* capture,
                              std::string* error) {
        return static_cast<BuildImportRuntime*>(context)->captureMapStorageChest(
            position, reopen, capture, error);
    };
    hooks.read_held_map = [](void*, MapStorageHeldMapEvidence* evidence,
                             std::string* error) {
        return ReadNativeHeldFilledMapEvidence(evidence, error);
    };
    hooks.submit_chest_place = [](void*, const FilledMapToSingleChestRequest& request,
                                  FilledMapChestSubmission* submission,
                                  std::string* error) {
        return SendFilledMapToSingleChest(request, submission, error);
    };
    hooks.commit_cursor_fsynced = [](void* context, uint64_t before,
                                     uint64_t after,
                                     const MapChestTransferRecord& chest,
                                     std::string* error) {
        return static_cast<BuildImportRuntime*>(context)->commitStoredMapCursor(
            before, after, chest, error);
    };
    hooks.finalize_committed_map_use = [](void* context, uint64_t old_cursor,
                                          uint64_t committed_cursor,
                                          std::string* error) {
        return static_cast<BuildImportRuntime*>(context)->finalizeStoredMapUse(
            old_cursor, committed_cursor, error);
    };
    MapStorageAnvilRuntimeHookContext anvil_context;
    anvil_context.runtime = this;
    anvil_context.storage_hooks = &hooks;
    anvil_context.expected_map_uuid = storage.input.expected_map_uuid;
    if (storage.rename) {
        anvil_context.expected_runtime_item_id =
            storage.rename->map_runtime_item_id;
        if (anvil_context.expected_map_uuid == -1) {
            anvil_context.expected_map_uuid = storage.rename->map_uuid;
        }
    }
    MapStorageAnvilHooks anvil_hooks;
    anvil_hooks.context = &anvil_context;
    anvil_hooks.read_response_baseline = [](
            void*, uint64_t* session, uint64_t* generation,
            std::string* error) {
        if (!session || !generation) {
            if (error) *error = "anvil response baseline output is unavailable";
            return false;
        }
        const uint64_t before =
            GetProjectionPrinterInventoryMailboxSessionGeneration();
        ProjectionPrinterInventoryResponse latest;
        const bool has_latest = GetProjectionPrinterInventoryResponse(&latest);
        const uint64_t after =
            GetProjectionPrinterInventoryMailboxSessionGeneration();
        if (!before || before != after ||
            (has_latest && latest.session_generation != before)) {
            if (error) *error = "anvil response mailbox session changed";
            return false;
        }
        *session = before;
        *generation = has_latest ? latest.response_generation : 0U;
        return true;
    };
    anvil_hooks.read_exact_response = [](
            void*, int32_t request_id,
            ProjectionPrinterInventoryResponse* response,
            std::string* error) {
        if (!response || request_id >= 0 ||
            (static_cast<uint32_t>(request_id) & 1U) == 0U) {
            if (error) *error = "anvil response request ID is invalid";
            return false;
        }
        const uint64_t before =
            GetProjectionPrinterInventoryMailboxSessionGeneration();
        const bool found = GetProjectionPrinterInventoryResponseByRequestId(
            request_id, response);
        const uint64_t after =
            GetProjectionPrinterInventoryMailboxSessionGeneration();
        if (!found || !before || before != after ||
            response->session_generation != before ||
            response->request_id != request_id) {
            if (error) *error = found
                ? "anvil response belongs to another mailbox session"
                : "exact anvil response has not arrived";
            return false;
        }
        return true;
    };
    anvil_hooks.read_world = [](
            void* context, const std::string& world_id,
            int32_t dimension_id, int32_t x, int32_t y, int32_t z,
            MapAnvilRenameWorldEvidence* evidence, std::string* error) {
        auto* binding = static_cast<MapStorageAnvilRuntimeHookContext*>(context);
        if (!binding || !binding->runtime || !binding->storage_hooks ||
            !binding->storage_hooks->require_live_world || !evidence ||
            !binding->storage_hooks->require_live_world(
                binding->storage_hooks->context, world_id, dimension_id,
                error)) {
            if (error && error->empty()) *error = "anvil native world is unavailable";
            return false;
        }
        NativeBlockInfo block;
        if (!NativeWorldAccess::getBlock(x, y, z, &block) ||
            block.name.empty()) {
            if (error) *error = "fresh native anvil Block read failed";
            return false;
        }
        evidence->world_context = world_id;
        evidence->block_identifier = std::move(block.name);
        return true;
    };
    anvil_hooks.read_held_map = [](
            void* context, MapAnvilHeldMapEvidence* evidence,
            std::string* error) {
        auto* binding = static_cast<MapStorageAnvilRuntimeHookContext*>(context);
        if (!binding || !evidence || binding->expected_map_uuid == -1) {
            if (error) *error = "current filled-map UUID is unavailable";
            return false;
        }
        ProjectionPrinterNativeSelectedFilledMapSnapshot native;
        if (!ReadProjectionPrinterNativeSelectedFilledMap(
                binding->expected_runtime_item_id,
                binding->expected_map_uuid, &native, error)) return false;
        evidence->selected_hotbar_slot = native.selected_hotbar_slot;
        evidence->network_stack_id = native.network_stack_id;
        evidence->runtime_item_id = native.runtime_item_id;
        evidence->count = native.count;
        evidence->item_identifier = std::move(native.item_identifier);
        evidence->has_map_uuid = true;
        evidence->map_uuid = native.map_uuid;
        return true;
    };
    anvil_hooks.ensure_visible_window = [](
            void* context, int32_t x, int32_t y, int32_t z,
            MapAnvilRenameWindowEvidence* evidence, std::string* error) {
        auto* binding = static_cast<MapStorageAnvilRuntimeHookContext*>(context);
        return binding && binding->runtime &&
            binding->runtime->observeMapStorageAnvil(
                x, y, z, true, evidence, error);
    };
    anvil_hooks.read_visible_window = [](
            void* context, int32_t x, int32_t y, int32_t z,
            MapAnvilRenameWindowEvidence* evidence, std::string* error) {
        auto* binding = static_cast<MapStorageAnvilRuntimeHookContext*>(context);
        return binding && binding->runtime &&
            binding->runtime->observeMapStorageAnvil(
                x, y, z, false, evidence, error);
    };
    anvil_hooks.read_native_input = [](
            void* context, const MapAnvilRenameWindowEvidence& opened,
            MapAnvilInputProof* proof, std::string* error) {
        auto* binding = static_cast<MapStorageAnvilRuntimeHookContext*>(context);
        if (!binding || !binding->runtime) return false;
        MapAnvilRenameRecord record;
        if (LoadMapAnvilRenameJournal(binding->runtime->map_state_path_,
                                      &record, error) !=
                MapAnvilRenameLoad::Loaded ||
            record.input_response.window_token != opened.token ||
            record.input_response.window_id != opened.window_id) {
            if (error && error->empty()) {
                *error = "anvil native input journal or window changed";
            }
            return false;
        }
        return binding->runtime->readMapStorageAnvilNativeInput(
            record, proof, error);
    };
    anvil_hooks.sync_accepted_client_input = [](
            void* context, const MapAnvilRenameRecord& record,
            const MapAnvilRenameWindowEvidence& opened,
            std::string* error) {
        auto* binding = static_cast<MapStorageAnvilRuntimeHookContext*>(context);
        if (!binding || !binding->runtime ||
            record.phase != MapAnvilRenamePhase::InputResponseAccepted ||
            !opened.opened || opened.closed ||
            opened.token != record.input_response.window_token ||
            opened.window_id != record.input_response.window_id ||
            opened.session_generation !=
                record.input_response.session_generation) {
            if (error) *error = "accepted anvil input window changed before local sync";
            return false;
        }
        ProjectionPrinterNativeSelectedFilledMapSnapshot held;
        if (!ReadProjectionPrinterNativeSelectedFilledMap(
                record.map_runtime_item_id, record.map_uuid,
                &held, error) ||
            held.selected_hotbar_slot !=
                record.input_response.source_hotbar_slot ||
            held.network_stack_id != record.input_source_network_stack_id ||
            held.count != 1U) {
            if (error && error->empty()) {
                *error = "accepted anvil input source is no longer the same live map";
            }
            return false;
        }
        ProjectionPrinterInventoryResponse response;
        if (!GetProjectionPrinterInventoryResponseByRequestId(
                record.input_request_id, &response)) {
            if (error) *error = "exact anvil input response was lost before local sync";
            return false;
        }
        uint64_t ticket = 0U;
        const bool queued = QueueMapAnvilClientSyncAcceptedInput(
            opened.token, opened.window_id, record.input_request_id,
            response, &ticket, error);
        if (queued) {
            LOGI("[map-anvil-local-sync] queued request=%d window=%u ticket=%llu state=%u",
                 record.input_request_id,
                 static_cast<unsigned>(opened.window_id),
                 static_cast<unsigned long long>(ticket),
                 static_cast<unsigned>(GetMapAnvilClientSyncTicketState(ticket)));
        }
        return queued;
    };
    anvil_hooks.submit_input = [](
            void*, const MapAnvilInputPlaceRequest& request,
            MapAnvilInputSubmission* submission, std::string* error) {
        return SendMapAnvilInputPlace(request, submission, error);
    };
    anvil_hooks.submit_name_text = [](
            void* context, const MapAnvilRenameWindowEvidence& opened,
            const MapAnvilRenameRecord& record, std::string* error) {
        auto* binding = static_cast<MapStorageAnvilRuntimeHookContext*>(context);
        if (!binding || !binding->runtime ||
            record.phase != MapAnvilRenamePhase::InputConfirmed) {
            if (error) *error = "anvil name callback lacks confirmed native input";
            return false;
        }
        auto* self = binding->runtime;
        MapAnvilRenameWindowEvidence current;
        if (!self->observeMapStorageAnvil(record.anvil_x, record.anvil_y,
                                           record.anvil_z, false, &current,
                                           error) ||
            current.token != opened.token ||
            current.window_id != opened.window_id ||
            current.session_generation != opened.session_generation ||
            current.token != record.input_response.window_token ||
            current.window_id != record.input_response.window_id ||
            !self->map_storage_anvil_window_) {
            if (error && error->empty()) {
                *error = "anvil name callback lost its original window";
            }
            return false;
        }
        if (self->map_storage_anvil_name_attempted_) {
            if (self->map_storage_anvil_name_submitted_ &&
                self->map_storage_anvil_name_title_ == record.expected_title) {
                return true;
            }
            if (error) *error = "anvil name callback outcome is uncertain; never repeat";
            return false;
        }
        self->map_storage_anvil_name_attempted_ = true;
        self->map_storage_anvil_name_title_ = record.expected_title;
        const MapNativeAnvilNameRequest request{
            opened.token, opened.window_id, record.anvil_x,
            record.anvil_y, record.anvil_z, record.tile_cursor,
            record.columns, record.rows, record.expected_title};
#if defined(__ANDROID__)
        LOGI("[map-anvil-stage] rename callback begin window=%u title_bytes=%zu",
             static_cast<unsigned>(opened.window_id),
             record.expected_title.size());
        const bool submitted = SubmitMapNativeAnvilRenameText(
            Main::getBaseAddress(), request,
            mapStorageMonotonicMs(std::chrono::steady_clock::now()),
            *self->map_storage_anvil_window_, error);
        LOGI("[map-anvil-stage] rename callback end submitted=%d reason=%s",
             submitted ? 1 : 0, error ? error->c_str() : "unavailable");
#else
        const bool submitted = false;
        if (error) *error = "native anvil name callback requires Android";
#endif
        if (!submitted) {
            self->map_storage_anvil_terminal_failure_ = true;
            return false;
        }
        self->map_storage_anvil_name_submitted_ = true;
        return true;
    };
    anvil_hooks.read_native_craft_preview = [](
            void* context, const MapAnvilRenameWindowEvidence& opened,
            MapStorageAnvilCraftPreview* preview, std::string* error) {
        if (preview) *preview = {};
        auto* binding = static_cast<MapStorageAnvilRuntimeHookContext*>(context);
        if (!binding || !binding->runtime || !preview) return false;
        auto* self = binding->runtime;
        MapAnvilRenameRecord record;
        if (LoadMapAnvilRenameJournal(self->map_state_path_, &record,
                                      error) != MapAnvilRenameLoad::Loaded ||
            record.phase != MapAnvilRenamePhase::InputConfirmed ||
            record.input_response.window_token != opened.token ||
            record.input_response.window_id != opened.window_id ||
            record.input_response.session_generation !=
                opened.session_generation ||
            !self->map_storage_anvil_window_) {
            if (error && error->empty()) {
                *error = "anvil preview lacks the original confirmed input";
            }
            return false;
        }
        MapAnvilInputProof input;
        if (!self->readMapStorageAnvilNativeInput(record, &input, error)) {
            return false;
        }
#if !defined(__ANDROID__)
        if (error) *error = "native anvil preview requires Android";
        return false;
#else
        const uint64_t tick_ms = mapStorageMonotonicMs(
            std::chrono::steady_clock::now());
        uintptr_t manager = 0U;
        if (!BorrowMapNativeAnvilManagerForVerifiedWindow(
                Main::getBaseAddress(), opened.token, tick_ms,
                *self->map_storage_anvil_window_, &manager) || !manager) {
            if (error) *error = "anvil preview manager binding was lost";
            return false;
        }
        ProjectionPrinterNativeAnvilPreviewMapSnapshot native;
        LOGI("[map-anvil-stage] preview read begin window=%u manager=%p",
             static_cast<unsigned>(opened.window_id),
             reinterpret_cast<void*>(manager));
        if (!ReadProjectionPrinterNativeAnvilPreviewMap(
                manager, record.map_runtime_item_id, record.map_uuid,
                record.expected_title, &native, error)) {
            LOGI("[map-anvil-stage] preview read failed reason=%s",
                 error ? error->c_str() : "unavailable");
            return false;
        }
        LOGI("[map-anvil-stage] preview read ok window=%u recipe=%u",
             static_cast<unsigned>(opened.window_id),
             native.dynamic_recipe_network_id);
        MapAnvilRenameWindowEvidence after;
        if (!self->observeMapStorageAnvil(record.anvil_x, record.anvil_y,
                                           record.anvil_z, false, &after,
                                           error) ||
            after.token != opened.token ||
            after.window_id != opened.window_id ||
            after.session_generation != opened.session_generation) {
            if (error && error->empty()) {
                *error = "anvil preview window changed during native read";
            }
            return false;
        }
        preview->native_readback = native.count == 1U;
        // The authorizing recipe ID comes from the original result-click's
        // same-packet type-15 action, not from a stale UI/member value.
        preview->dynamic_recipe_network_id = 0U;
        preview->input_map_uuid = input.map_uuid;
        preview->output_map_uuid = native.map_uuid;
        preview->output_title = std::move(native.display_name);
        return preview->native_readback;
#endif
    };
    anvil_hooks.submit_craft_and_collect = [](
            void* context, const MapAnvilRenameRecord& record,
            const MapAnvilRenameWindowEvidence& opened,
            const MapAnvilInputProof& input,
            const MapStorageAnvilCraftPreview& preview,
            std::string* error) {
        auto* binding = static_cast<MapStorageAnvilRuntimeHookContext*>(context);
        if (!binding || !binding->runtime ||
            !preview.native_readback ||
            preview.input_map_uuid != record.map_uuid ||
            preview.output_map_uuid != record.map_uuid ||
            preview.output_title != record.expected_title ||
            input.fresh_window_token != opened.token ||
            input.window_id != opened.window_id ||
            opened.token != record.input_response.window_token ||
            opened.window_id != record.input_response.window_id) {
            if (error) *error = "original anvil result action preflight changed";
            return false;
        }
        auto* self = binding->runtime;
        if (!self->map_storage_anvil_window_ ||
            self->map_storage_anvil_window_->state() !=
                MapVisibleAnvilWindowState::Open) {
            if (error) *error = "original anvil result window is not open";
            return false;
        }
        MapNativeAnvilResultActionRequest request;
        request.confirmed = &record;
        request.input = &input;
        request.map_state_path = self->map_state_path_;
        request.now_ms = mapStorageMonotonicMs(
            std::chrono::steady_clock::now());
        MapNativeAnvilResultActionOutcome outcome;
#if defined(__ANDROID__)
        const bool sent = SubmitMapNativeAnvilResultAction(
            Main::getBaseAddress(), request,
            *self->map_storage_anvil_window_, &outcome, error);
#else
        const bool sent = false;
        if (error) *error = "native anvil result action requires Android";
#endif
        // The stock handler queues its ItemStackRequest; the outbound hook
        // captures and journals that request on a later game tick. A true
        // result means the click itself was armed durably and invoked.
        return sent && !outcome.outcome_uncertain;
    };
    // The bridge's own build/session verification gate is also checked below
    // before any fresh input Place. A function pointer alone is not readiness.
    anvil_hooks.read_native_output = [](
            void* context, MapAnvilOutputProof* output,
            std::string* error) {
        auto* binding = static_cast<MapStorageAnvilRuntimeHookContext*>(context);
        if (output) *output = {};
        if (!binding || !binding->runtime || !output) return false;
        auto* self = binding->runtime;
        MapAnvilRenameRecord record;
        if (LoadMapAnvilRenameJournal(self->map_state_path_, &record,
                                      error) != MapAnvilRenameLoad::Loaded ||
            record.phase != MapAnvilRenamePhase::CraftResponseAccepted ||
            record.world_id != self->current_world_.world_id ||
            record.dimension_id != self->current_world_.dimension_id ||
            !self->in_game_tick_ || !IsMinecraftUpdateGameThread() ||
            !self->controller_.worldMatches(self->current_world_)) {
            if (error && error->empty()) {
                *error = "native renamed-map output lacks its live task";
            }
            return false;
        }
        ProjectionPrinterNativeInventoryFilledMapMatch native;
        if (!ReadProjectionPrinterNativeInventoryFilledMapMatch(
                record.map_runtime_item_id, record.map_uuid,
                record.expected_title, &native, error)) return false;
        output->world_id = record.world_id;
        output->dimension_id = record.dimension_id;
        output->count = native.count;
        output->runtime_item_id = native.runtime_item_id;
        output->network_stack_id = native.network_stack_id;
        output->map_uuid = native.map_uuid;
        output->name_source = MapItemNameSource::DisplayName;
        output->name = std::move(native.display_name);
        output->native_readback = true;
        return true;
    };
    MapStorageAnvilAdapter anvil_adapter{map_state_path_, anvil_hooks};
    MapStorageProductionActions actions{
        map_state_path_, &reader, hooks, &anvil_adapter};
    actions.debug_preexisting_pair_verified =
        debug_preexisting_pair_verified;
    if (debug_preexisting_pair_verified && storage.pair) {
        actions.debug_preexisting_pair = storage.pair->pair;
    }
    // Do not start a fresh rename journal or place the map in an anvil unless
    // the entire automatic name/preview/result path is available. Existing
    // Armed journals may still take their read-only no-resend recovery path.
    MapStorageCoordinatorDecision planned;
    std::string plan_error;
    if (!ClassifyMapStorageCoordinator(storage.input, &planned,
                                       &plan_error)) {
        pauseMapCreation(plan_error.empty()
            ? "map storage plan cannot be classified" : plan_error);
        return;
    }
    const bool needs_fresh_anvil_action =
        planned.next == MapStorageNextStep::AwaitRenameJournal ||
        planned.next == MapStorageNextStep::PreflightAnvilInput ||
        planned.next == MapStorageNextStep::PreflightAnvilCraft;
    if (needs_fresh_anvil_action &&
        (!anvil_hooks.submit_name_text ||
         !anvil_hooks.read_native_craft_preview ||
         !anvil_hooks.submit_craft_and_collect ||
         !anvil_hooks.read_native_output ||
         !IsMapNativeAnvilAutomaticResultDispatchVerified())) {
        pauseMapCreation(
            "automatic anvil rename needs verified same-window result dispatch");
        return;
    }
    const auto storage_now = std::chrono::steady_clock::now();
    const bool log_storage_tick = storage_now >= map_storage_log_at_;
    if (log_storage_tick) {
        LOGI("[map-storage-test] before step=%u tile=%llu/%llu pair=%d debug_pair=%d rename=%d chest=%d",
             static_cast<unsigned>(planned.next),
             static_cast<unsigned long long>(map_tile_cursor_),
             static_cast<unsigned long long>(map_tile_count_),
             storage.input.pair ? 1 : 0,
             debug_preexisting_pair_verified ? 1 : 0,
             storage.input.rename ? static_cast<int>(storage.input.rename->phase) : -1,
             storage.input.chest ? static_cast<int>(storage.input.chest->phase) : -1);
        if (storage.input.pair &&
            (storage.input.pair->phase == MapPairPlacementPhase::SupportDispatchArmed ||
             storage.input.pair->phase == MapPairPlacementPhase::ChestDispatchArmed ||
             storage.input.pair->phase == MapPairPlacementPhase::AnvilDispatchArmed)) {
            const auto& pair = *storage.input.pair;
            const bool support = pair.phase == MapPairPlacementPhase::SupportDispatchArmed;
            NativeBlockInfo chest_block;
            NativeBlockInfo anvil_block;
            const bool reader_ready = reader.open();
            const bool chest_read = reader_ready && reader.getBlock(
                pair.pair.chest.x, pair.pair.chest.y - (support ? 1 : 0),
                pair.pair.chest.z, &chest_block);
            const bool anvil_read = reader_ready && reader.getBlock(
                pair.pair.anvil.x, pair.pair.anvil.y - (support ? 1 : 0),
                pair.pair.anvil.z, &anvil_block);
            LOGI("[map-storage-native] phase=%u uuid=%s chest=(%d,%d,%d):%s anvil=(%d,%d,%d):%s",
                 static_cast<unsigned>(pair.phase), pair.active_command_uuid.c_str(),
                 pair.pair.chest.x, pair.pair.chest.y - (support ? 1 : 0),
                 pair.pair.chest.z, chest_read ? chest_block.name.c_str() : "<unreadable>",
                 pair.pair.anvil.x, pair.pair.anvil.y - (support ? 1 : 0),
                 pair.pair.anvil.z, anvil_read ? anvil_block.name.c_str() : "<unreadable>");
        }
        map_storage_log_at_ = storage_now + std::chrono::seconds(1);
    }
    const MapStorageDriveResult result = map_storage_driver_.tick(
        storage.input, MakeMapStorageProductionOps(&actions));
    if (log_storage_tick || result.status == MapStorageDriveStatus::Unsafe ||
        result.status == MapStorageDriveStatus::FailedBeforeDispatch ||
        result.status == MapStorageDriveStatus::WaitingForHandler ||
        (result.status == MapStorageDriveStatus::OneTransitionAttempted &&
         !result.error.empty())) {
        LOGI("[map-storage-test] after step=%u status=%u error=%s",
             static_cast<unsigned>(result.decision.next),
             static_cast<unsigned>(result.status), result.error.c_str());
    }
    if (result.status == MapStorageDriveStatus::Complete) {
        finishMapCreation();
        return;
    }
    if (result.decision.next == MapStorageNextStep::AwaitMapUseMarker &&
        result.status == MapStorageDriveStatus::WaitingForEvidence) {
        if (map_tile_cursor_ >= map_tile_count_ ||
            storage.input.has_pending_map_use_marker) {
            pauseMapCreation("map storage cannot start another tile before marker cleanup");
            return;
        }
        clearCompletedMapUseRuntime();
        stage_ = ExecuteStage::MapPrepare;
        return;
    }
    if (result.status == MapStorageDriveStatus::Unsafe ||
        result.status == MapStorageDriveStatus::FailedBeforeDispatch ||
        result.status == MapStorageDriveStatus::WaitingForHandler ||
        (result.status == MapStorageDriveStatus::OneTransitionAttempted &&
         !result.error.empty())) {
        pauseMapCreation(result.error.empty()
            ? "automatic map storage needs verified anvil/chest adapters"
            : result.error);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = result.status == MapStorageDriveStatus::WaitingForEvidence &&
                      !result.error.empty()
        ? "waiting for map storage: " + result.error
        : "reconciling map storage (" +
              std::to_string(map_tile_cursor_) + "/" +
              std::to_string(map_tile_count_) + ")";
}

void BuildImportRuntime::pauseMapCreation(const std::string& detail) {
    ArmMapTextureDiagnostics(false);
    if (map_storage_chest_capture_) {
        map_storage_chest_stop_requested_ = true;
        if (in_game_tick_) {
            map_storage_chest_capture_->stop(mapStorageMonotonicMs(
                std::chrono::steady_clock::now()));
        }
    }
    if (map_storage_anvil_window_) {
        map_storage_anvil_stop_requested_ = true;
        if (in_game_tick_) {
            serviceMapStorageAnvil(std::chrono::steady_clock::now());
        }
    }
    std::string state_error;
    bool persisted = map_tile_persisted_cursor_ == map_tile_cursor_;
    if (!persisted) {
        persisted = saveDeferredDataState(map_state_path_, map_tile_count_,
                                          map_tile_cursor_, &state_error);
        if (persisted) map_tile_persisted_cursor_ = map_tile_cursor_;
    }
    map_creation_active_ = false;
    stage_ = ExecuteStage::None;
    std::string reason = "automatic map creation paused: " + detail;
    if (!persisted && !state_error.empty()) {
        reason += "; progress state write failed: " + state_error;
    }
    pauseFinalVerification(reason);
}

void BuildImportRuntime::finishMapCreation() {
    if (!map_creation_active_) return;
    ArmMapTextureDiagnostics(false);
    if (map_storage_chest_capture_) {
        map_storage_chest_stop_requested_ = true;
        if (in_game_tick_) {
            map_storage_chest_capture_->stop(mapStorageMonotonicMs(
                std::chrono::steady_clock::now()));
        }
    }
    if (map_storage_anvil_window_) {
        map_storage_anvil_stop_requested_ = true;
        if (in_game_tick_) {
            serviceMapStorageAnvil(std::chrono::steady_clock::now());
        }
    }
    std::string state_error;
    uint64_t confirmed_cursor = 0U;
    const bool saved = map_storage_pipeline_active_
        ? loadDeferredDataState(map_state_path_, map_tile_count_,
                                &confirmed_cursor, &state_error) &&
          confirmed_cursor == map_tile_count_
        : saveDeferredDataState(map_state_path_, map_tile_count_,
                                map_tile_count_, &state_error);
    if (!saved) {
        pauseMapCreation(state_error.empty()
            ? "cannot save final automatic map progress" : state_error);
        return;
    }
    map_tile_cursor_ = map_tile_count_;
    map_tile_persisted_cursor_ = map_tile_count_;
    map_creation_active_ = false;
    map_creation_completed_ = true;
    stage_ = ExecuteStage::None;
    LOGI("[map] automatic map creation complete count=%llu",
         static_cast<unsigned long long>(map_tile_count_));
    finishFinalVerification(true);
}

void BuildImportRuntime::finishFinalVerification(bool success, const std::string& detail) {
    if (success && !command_block_writer_completed_) {
        std::string command_block_error;
        if (beginCommandBlockWrite(&command_block_error)) {
            // Do not mark the controller complete or delete its spool yet. The
            // writer remains under ImportState::Verifying so pause/context
            // changes can resume the independent cursor safely.
            return;
        }
        if (!command_block_error.empty()) {
            pauseFinalVerification("cannot start command-block writer: " + command_block_error);
            return;
        }
    }
    if (success && !sign_writer_completed_) {
        std::string sign_error;
        if (beginSignWrite(&sign_error)) {
            return;
        }
        if (!sign_error.empty()) {
            pauseFinalVerification("cannot start sign writer: " + sign_error);
            return;
        }
    }
    if (success && !container_item_writer_completed_) {
        std::string container_error;
        if (beginContainerWrite(&container_error)) {
            return;
        }
        if (!container_error.empty()) {
            pauseFinalVerification("cannot start container-item writer: " + container_error);
            return;
        }
    }
    if (success && !entity_writer_completed_) {
        std::string entity_error;
        if (beginEntityWrite(&entity_error)) {
            return;
        }
        if (!entity_error.empty()) {
            pauseFinalVerification("cannot start entity writer: " + entity_error);
            return;
        }
    }
    if (success && map_creation_requested_ &&
        !map_storage_pipeline_active_ && !mapStoragePipelineEnabled()) {
        std::string storage_recovery_error;
        if (!requireNoPendingMapStorage(map_state_path_, current_world_,
                                        map_source_bounds_, map_tile_count_,
                                        map_tile_cursor_,
                                        &storage_recovery_error)) {
            pauseFinalVerification("cannot complete automatic map creation: " +
                                   storage_recovery_error);
            return;
        }
    }
    if (success && map_creation_requested_ && !map_creation_completed_) {
        std::string map_error;
        if (beginMapCreation(&map_error)) return;
        // A prior task's already-closed native window may still be retiring.
        // Keep final verification active so the next game tick can retry the
        // startup gate after serviceMapStorageChest/Anvil advances cleanup.
        if (map_error.empty()) return;
        pauseFinalVerification("cannot start automatic map creation: " + map_error);
        return;
    }
    const size_t verified_chunks = verification_verified_chunks_;
    const size_t unverifiable_chunks = verification_unverifiable_chunks_;
    const bool verification_enabled =
        controller_.runtimeMetadata().verify_after_import;
    stage_ = ExecuteStage::None;
    controller_.recordVerificationProgress(verification_sample_index_, false, nullptr);
    releaseLoadedRegion(true);
    std::string checkpoint_error;
    if (!controller_.finishVerification(success, detail, &checkpoint_error)) {
        std::string pause_error;
        if (!controller_.pause(&pause_error)) controller_.emergencyPauseNoCheckpoint();
        resetVerificationRuntime();
        resetCommandBlockRuntime();
        resetSignRuntime();
        resetContainerRuntime();
        resetEntityRuntime();
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "final verification result could not be checkpointed: " + checkpoint_error;
        if (!pause_error.empty()) status_ += "; pause checkpoint failed: " + pause_error;
        return;
    }
    std::string undo_record_warning;
    if (success) {
        std::string undo_error;
        if (undo_run_active_) {
            retireUndoClaim(&undo_record_warning);
        } else if (!persistUndoSnapshot(&undo_error)) {
            // Never leave an older building's record advertised as the latest
            // successful import. Retire it before exposing the failure; the
            // lifecycle state also makes a failed cleanup fail closed.
            undo_record_warning = "undo record unavailable";
            if (!undo_error.empty()) undo_record_warning += ": " + undo_error;
            std::string stale_warning;
            if (!invalidateAvailableUndoRecord(&stale_warning) && !stale_warning.empty()) {
                undo_record_warning += "; " + stale_warning;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            imported_block_count_ = total_block_count_;
        }
        cleanupSpoolDirectory(spool_directory_);
        spool_directory_.clear();
    }
    const bool automatic_maps_requested = map_creation_requested_;
    resetVerificationRuntime();
    resetCommandBlockRuntime();
    resetSignRuntime();
    resetContainerRuntime();
    resetEntityRuntime();
    resetMapCreationRuntime();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!success) {
        status_ = "final verification failed: " + detail;
    } else if (!verification_enabled) {
        status_ = "completed; final verification disabled";
    } else if (unverifiable_chunks == 0) {
        status_ = "completed; all " + std::to_string(verified_chunks) +
            " chunk plans passed C++ sampling";
    } else {
        status_ = "completed with limited verification; " +
            std::to_string(verified_chunks) + " sampled chunks passed and " +
            std::to_string(unverifiable_chunks) +
            " chunks had no stable verification sample";
    }
    if (!undo_record_warning.empty()) status_ += "; " + undo_record_warning;
    if (success && automatic_maps_requested) {
        status_ += "; filled-map items created and artwork-area texture packets covered; "
            "final map appearance still needs visual checking";
    }
    undo_run_active_ = false;
    undo_storage_directory_.clear();
    undo_record_id_.clear();
    undo_claim_job_id_.clear();
    durable_undo_block_count_ = 0;
}

void BuildImportRuntime::pauseFinalVerification(const std::string& detail) {
    LOGE("[final-verification] pausing reason=%s", detail.c_str());
    stage_ = ExecuteStage::None;
    releaseLoadedRegion(true);
    const size_t resume_index = verification_repair_chunk_
        ? verification_repair_sample_begin_ : verification_sample_index_;
    controller_.recordVerificationProgress(resume_index, false, nullptr);
    std::string checkpoint_error;
    const bool checkpointed = controller_.pause(&checkpoint_error);
    if (!checkpointed) controller_.emergencyPauseNoCheckpoint();
    const uint64_t durable_blocks = undo_run_active_
        ? durable_undo_block_count_ : controller_.completedBlockCount();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        imported_block_count_ = durable_blocks;
    }
    recovery_pending_ = false;
    resetVerificationRuntime();
    resetCommandBlockRuntime();
    resetSignRuntime();
    resetContainerRuntime();
    resetEntityRuntime();
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = "final verification paused: " + detail +
        (checkpointed ? "; resume to retry"
                      : "; checkpoint write failed: " + checkpoint_error +
                        "; stopped in memory");
}

bool BuildImportRuntime::onRawNetworkPacket(const std::string& packet) try {
    if (packet.empty()) return false;
    if (isSilentBuildImportRpcPacket(packet)) {
        native_rpc_ack_observed_.store(true, std::memory_order_release);
        return true;
    }
    PyRpcAckEvent rpc_event;
    if (decodePyRpcAckPacket(packet, &rpc_event)) {
        if (rpc_event.kind == PyRpcAckEventKind::AfterExecuteCommand) {
            if (!isBuildImportRpcUuid(rpc_event.uuid)) return false;
            native_rpc_ack_observed_.store(true, std::memory_order_release);
            if (rpc_event.uuid == kBuildImportSilentRpcUuid) return true;
            LOGI("[rpc-ack] source=raw-pyrpc bytes=%zu uuid=%s accepted=%d",
                 packet.size(), rpc_event.uuid.c_str(), rpc_event.execute_result ? 1 : 0);
            std::lock_guard<std::mutex> lock(native_event_mutex_);
            received_rpc_acks_[rpc_event.uuid] = rpc_event.execute_result;
            while (received_rpc_acks_.size() > 1024) {
                received_rpc_acks_.erase(received_rpc_acks_.begin());
            }
            return true;
        }
        if (rpc_event.kind == PyRpcAckEventKind::ExecuteCommandOutput) {
            return shouldSuppressBuildImportRpcFeedback(rpc_event, false);
        }
        const ImportState rpc_state = controller_.state();
        if (rpc_event.kind == PyRpcAckEventKind::AvailableCheckFailed &&
            (rpc_state == ImportState::Planning ||
             rpc_state == ImportState::Running ||
             rpc_state == ImportState::Paused ||
             rpc_state == ImportState::Verifying)) {
            LOGE("[rpc-ack] source=raw-pyrpc permission-failed bytes=%zu reason=%s",
                 packet.size(), rpc_event.reason.c_str());
            std::lock_guard<std::mutex> lock(native_event_mutex_);
            if (pending_command_error_.empty()) {
                pending_command_error_ = rpc_event.reason.empty()
                    ? "permission unavailable for aiCommand"
                    : "permission unavailable for aiCommand: " + rpc_event.reason;
            }
            // _CheckAvailable emits this event for every rejected command. One
            // pending native error is sufficient; forwarding the whole burst
            // would recreate the client-side feedback stall we suppress for
            // successful importer commands.
            return shouldSuppressBuildImportRpcFeedback(rpc_event, true);
        }
        return false;
    }
    size_t cursor = 0;
    uint32_t wire_header = 0;
    if (!readVarUInt(packet, &cursor, &wire_header)) return false;
    const uint32_t packet_id = wire_header & 0x3FFU;
    if (packet_id == kPyRpcPacketId) {
        // Keep diagnostics bounded: a client-version envelope change can
        // otherwise look identical to a server that never acknowledged RPCs.
        const ImportState state = controller_.state();
        if (state == ImportState::Planning || state == ImportState::Running ||
            state == ImportState::Verifying) {
            static std::atomic<uint32_t> unrecognized_pyrpc_count{0};
            const uint32_t count = unrecognized_pyrpc_count.fetch_add(
                1, std::memory_order_relaxed);
            if (count < 4U) {
                uint32_t declared_size = 0;
                const bool has_size = readVarUInt(packet, &cursor, &declared_size);
                LOGI("[rpc-ack] unrecognized-pyrpc bytes=%zu wire_header=%u "
                     "declared_size=%u size_valid=%d",
                     packet.size(), wire_header, declared_size, has_size ? 1 : 0);
            }
        }
        return false;
    }
    if (packet_id == 0x3A) {
        int32_t chunk_x = 0;
        int32_t chunk_z = 0;
        if (!readVarInt(packet, &cursor, &chunk_x) ||
            !readVarInt(packet, &cursor, &chunk_z)) return false;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(native_event_mutex_);
        received_level_chunks_.note({chunk_x, chunk_z}, now);
        return false;
    }
    if (packet_id == 0x05 || packet_id == 0x3D) {
        native_context_change_pending_.store(true, std::memory_order_release);
        return false;
    }
    const ImportState packet_state = controller_.state();
    const bool active_import = packet_state == ImportState::Planning ||
        packet_state == ImportState::Running || packet_state == ImportState::Verifying;
    if (packet_id != 0x4F || !active_import) return false;
    // The server gamerule is best-effort; consume the standard output packet
    // locally as well so a delayed/unsupported gamerule cannot leak feedback.
    const bool suppress_output =
        suppress_command_feedback_.load(std::memory_order_acquire);

    std::string searchable = packet.substr(cursor);
    std::transform(searchable.begin(), searchable.end(), searchable.begin(),
        [](unsigned char character) {
            return character < 0x80 ? static_cast<char>(std::tolower(character))
                                    : static_cast<char>(character);
        });
    // CommandOutput packets do not carry enough ownership information in this
    // hook to associate generic failures with a particular batched command.
    // Final native verification handles placement/load failures. Only a
    // permission rejection is global and safe to act on without correlation.
    static const char* const keywords[] = {
        "permission", "not allowed", "denied", "operator permission",
        "没有权限", "无权限", "权限不足",
    };
    bool failed = false;
    for (const char* keyword : keywords) {
        if (searchable.find(keyword) != std::string::npos) {
            failed = true;
            break;
        }
    }
    if (!failed) return suppress_output;
    std::string detail;
    detail.reserve(std::min<size_t>(320, searchable.size()));
    for (unsigned char byte : searchable) {
        if (detail.size() >= 320) break;
        if (byte >= 0x20 || byte >= 0x80) detail.push_back(static_cast<char>(byte));
        else if (!detail.empty() && detail.back() != ' ') detail.push_back(' ');
    }
    std::lock_guard<std::mutex> lock(native_event_mutex_);
    if (pending_command_error_.empty()) pending_command_error_ = std::move(detail);
    return suppress_output;
} catch (...) {
    // Packet hooks must not unwind through the game's networking thread.
    return false;
}

bool BuildImportRuntime::pollNativeEvents() {
    if (native_context_change_pending_.exchange(false, std::memory_order_acq_rel)) {
        if (isContextSensitiveState(controller_.state())) {
            onWorldContextChanged({"__native_network_context_changed__", current_world_.dimension_id});
        } else {
            if (restore_in_progress_.load(std::memory_order_acquire)) {
                run_generation_.fetch_add(1, std::memory_order_acq_rel);
            }
            releaseLoadedRegion(false);
            pending_cleanup_commands_.clear();
            held_region_cleanups_.clear();
            held_region_cleanup_retry_at_ = {};
            hold_active_region_release_until_ = {};
            prefetched_prepare_region_.reset();
            prefetched_prepare_sent_at_ = {};
            cleanup_barrier_uuids_.clear();
            cleanup_barrier_command_count_ = 0;
            cleanup_barrier_poll_at_ = {};
            cleanup_barrier_deadline_ = {};
            cleanup_barrier_failure_count_ = 0;
            cleanup_retry_at_ = {};
            region_add_ready_at_ = {};
    invalidateRpcTransport();
            std::lock_guard<std::mutex> event_lock(native_event_mutex_);
            received_level_chunks_.clear();
            received_rpc_acks_.clear();
        }
        return true;
    }
    std::string command_error;
    {
        std::lock_guard<std::mutex> lock(native_event_mutex_);
        command_error.swap(pending_command_error_);
    }
    if (command_error.empty()) return false;
    const ImportState current_state = controller_.state();
    const bool deferred_window_draining =
        stage_ == ExecuteStage::Drain && controller_.hasDeferredUnits();
    if (current_state == ImportState::Planning && containsPermissionError(command_error)) {
        run_generation_.fetch_add(1, std::memory_order_acq_rel);
        controller_.cancel(true);
        recovery_pending_ = false;
        releaseLoadedRegion(false);
        pending_cleanup_commands_.clear();
        held_region_cleanups_.clear();
        held_region_cleanup_retry_at_ = {};
        hold_active_region_release_until_ = {};
        prefetched_prepare_region_.reset();
        prefetched_prepare_sent_at_ = {};
        cleanup_barrier_uuids_.clear();
        cleanup_barrier_command_count_ = 0;
        cleanup_barrier_poll_at_ = {};
        cleanup_barrier_deadline_ = {};
        cleanup_barrier_failure_count_ = 0;
        cleanup_retry_at_ = {};
        region_add_ready_at_ = {};
    invalidateRpcTransport();
        resetVerificationRuntime();
        resetActiveUnitRuntime();
        {
            std::lock_guard<std::mutex> event_lock(native_event_mutex_);
            received_rpc_acks_.clear();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        imported_block_count_ = 0;
        status_ = "server rejected command permission; import cancelled before execution";
        return true;
    }
    if (current_state == ImportState::Paused && containsPermissionError(command_error)) {
        recovery_pending_ = false;
        releaseLoadedRegion(false);
        pending_cleanup_commands_.clear();
        held_region_cleanups_.clear();
        held_region_cleanup_retry_at_ = {};
        hold_active_region_release_until_ = {};
        prefetched_prepare_region_.reset();
        prefetched_prepare_sent_at_ = {};
        cleanup_barrier_uuids_.clear();
        cleanup_barrier_command_count_ = 0;
        cleanup_barrier_poll_at_ = {};
        cleanup_barrier_deadline_ = {};
        cleanup_barrier_failure_count_ = 0;
        cleanup_retry_at_ = {};
        region_add_ready_at_ = {};
    invalidateRpcTransport();
        resetVerificationRuntime();
        resetActiveUnitRuntime();
        {
            std::lock_guard<std::mutex> event_lock(native_event_mutex_);
            received_rpc_acks_.clear();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "server rejected ticking-area cleanup permission; import remains paused";
        return true;
    }
    if (current_state == ImportState::Verifying && !deferred_window_draining) {
        pauseFinalVerification("server rejected command permission");
        return true;
    }
    if (current_state != ImportState::Running && !deferred_window_draining) return false;
    if (containsPermissionError(command_error)) {
        if (active_unit_) {
            controller_.rewindActiveProgress();
            std::lock_guard<std::mutex> lock(mutex_);
            imported_block_count_ = active_unit_imported_start_;
        }
        std::string checkpoint_error;
        const bool checkpointed = controller_.pause(&checkpoint_error);
        if (!checkpointed) controller_.emergencyPauseNoCheckpoint();
        const uint64_t durable_blocks = undo_run_active_
            ? durable_undo_block_count_ : controller_.completedBlockCount();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            imported_block_count_ = durable_blocks;
            status_ = checkpointed
                ? "server rejected command permission; import paused"
                : "server rejected command permission; durable pause failed: " +
                  checkpoint_error + "; stopped in memory";
        }
        recovery_pending_ = false;
        releaseLoadedRegion(true);
        resetVerificationRuntime();
        resetActiveUnitRuntime();
        return true;
    }
    return false;
}

void BuildImportRuntime::onWorldContextChanged(const WorldContext& context) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    const CheckpointSnapshot snapshot = controller_.checkpointSnapshot();
    const ImportState current_state = snapshot.state;
    if (!isContextSensitiveState(current_state) || snapshot.world == context) {
        if (restore_in_progress_.load(std::memory_order_acquire) &&
            !(current_world_ == context)) {
            run_generation_.fetch_add(1, std::memory_order_acq_rel);
        }
        current_world_ = context;
        return;
    }
    run_generation_.fetch_add(1, std::memory_order_acq_rel);
    if (current_state == ImportState::Planning) {
        // The actor tick must never wait for a large decoder/inflater that has
        // not reached its next cancellation check yet. The worker observes the
        // generation/state change, cleans its own temporary files, and is
        // joined without blocking by a later tick once it exits.
        controller_.cancel(false);
        current_world_ = context;
        recovery_pending_ = false;
        releaseLoadedRegion(false);
        pending_cleanup_commands_.clear();
        held_region_cleanups_.clear();
        held_region_cleanup_retry_at_ = {};
        hold_active_region_release_until_ = {};
        prefetched_prepare_region_.reset();
        prefetched_prepare_sent_at_ = {};
        cleanup_barrier_uuids_.clear();
        cleanup_barrier_command_count_ = 0;
        cleanup_barrier_poll_at_ = {};
        cleanup_barrier_deadline_ = {};
        cleanup_barrier_failure_count_ = 0;
        cleanup_retry_at_ = {};
        region_add_ready_at_ = {};
        rpc_transport_ready_ = false;
        native_dimension_token_ = 0;
        resetVerificationRuntime();
        resetActiveUnitRuntime();
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "world or dimension changed during parsing; import cancelled before execution";
        return;
    }
    if (active_unit_) {
        controller_.rewindActiveProgress();
        std::lock_guard<std::mutex> lock(mutex_);
        imported_block_count_ = active_unit_imported_start_;
    }
    if (current_state == ImportState::Verifying) {
        const size_t resume_index = verification_repair_chunk_
            ? verification_repair_sample_begin_ : verification_sample_index_;
        controller_.recordVerificationProgress(resume_index, false, nullptr);
    }
    current_world_ = context;
    std::string checkpoint_error;
    const bool checkpointed = controller_.onWorldContextChanged(context, &checkpoint_error);
    const bool context_closed =
        controller_.state() == ImportState::ClosedForContextChange;
    if (!context_closed) {
        controller_.emergencyPauseNoCheckpoint();
        // A failed checkpoint leaves the controller bound to the original
        // world. Keep the runtime bound to it as well so the paused-state
        // context guard does not retry the same failed disk write every tick.
        current_world_ = snapshot.world;
    }
    const uint64_t durable_blocks = undo_run_active_
        ? durable_undo_block_count_ : controller_.completedBlockCount();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        imported_block_count_ = durable_blocks;
    }
    recovery_pending_ = false;
    releaseLoadedRegion(false);
    pending_cleanup_commands_.clear();
    held_region_cleanups_.clear();
    held_region_cleanup_retry_at_ = {};
    hold_active_region_release_until_ = {};
    prefetched_prepare_region_.reset();
    prefetched_prepare_sent_at_ = {};
    cleanup_barrier_uuids_.clear();
    cleanup_barrier_command_count_ = 0;
    cleanup_barrier_poll_at_ = {};
    cleanup_barrier_deadline_ = {};
    cleanup_barrier_failure_count_ = 0;
    cleanup_retry_at_ = {};
    region_add_ready_at_ = {};
    phase_settle_ready_at_ = {};
    pending_resume_settle_delay_ = std::chrono::milliseconds(0);
    invalidateRpcTransport();
    native_dimension_token_ = 0;
    {
        std::lock_guard<std::mutex> event_lock(native_event_mutex_);
        pending_command_error_.clear();
        received_level_chunks_.clear();
        received_rpc_acks_.clear();
    }
    resetVerificationRuntime();
    resetCommandBlockRuntime();
    resetSignRuntime();
    resetContainerRuntime();
    resetEntityRuntime();
    resetActiveUnitRuntime();
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = checkpointed && context_closed
        ? "world or dimension changed; checkpoint saved and import closed"
        : "world or dimension changed; checkpoint write failed: " + checkpoint_error +
          "; import stopped in memory";
}

void BuildImportRuntime::pause() {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (active_unit_) {
        controller_.rewindActiveProgress();
        std::lock_guard<std::mutex> lock(mutex_);
        imported_block_count_ = active_unit_imported_start_;
    }
    if (controller_.state() == ImportState::Verifying) {
        const size_t resume_index = verification_repair_chunk_
            ? verification_repair_sample_begin_ : verification_sample_index_;
        controller_.recordVerificationProgress(resume_index, false, nullptr);
    }
    std::string checkpoint_error;
    const bool checkpointed = controller_.pause(&checkpoint_error);
    if (!checkpointed) controller_.emergencyPauseNoCheckpoint();
    const uint64_t durable_blocks = undo_run_active_
        ? durable_undo_block_count_ : controller_.completedBlockCount();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        imported_block_count_ = durable_blocks;
    }
    recovery_pending_ = false;
    phase_settle_ready_at_ = {};
    pending_resume_settle_delay_ = std::chrono::milliseconds(0);
    releaseLoadedRegion(true);
    resetVerificationRuntime();
    resetCommandBlockRuntime();
    resetSignRuntime();
    resetContainerRuntime();
    resetEntityRuntime();
    resetActiveUnitRuntime();
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = checkpointed
        ? "import paused; active partition will replay on resume"
        : "import paused in memory, but checkpoint write failed: " + checkpoint_error +
          "; execution stopped";
}

bool BuildImportRuntime::resume(const WorldContext& context,
                                std::string* error) {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    if (!controller_.resume(context, error)) return false;
    releaseLoadedRegion(true);
    WorkUnit stale_area;
    const std::string stale_cleanup = controller_.makeCleanupCommand(stale_area);
    if (std::find(pending_cleanup_commands_.begin(), pending_cleanup_commands_.end(),
                  stale_cleanup) == pending_cleanup_commands_.end()) {
        pending_cleanup_commands_.push_back(stale_cleanup);
    }
    cleanup_retry_at_ = {};
    cleanup_barrier_failure_count_ = 0;
    current_world_ = context;
    invalidateRpcTransport();
    command_feedback_suppression_unavailable_ = false;
    command_feedback_failure_count_ = 0;
    command_feedback_retry_at_ = {};
    native_dimension_token_ = GetCachedDimensionTokenForWorld(current_world_.world_id);
    recovery_pending_ = false;
    pending_resume_settle_delay_ = conservativeResumeSettleDelay();
    phase_settle_ready_at_ = {};
    unit_retry_counts_.clear();
    resetVerificationRuntime();
    resetCommandBlockRuntime();
    resetSignRuntime();
    resetContainerRuntime();
    resetEntityRuntime();
    resetActiveUnitRuntime();
    // Keep any outstanding block/command token debt. A quick pause/resume must
    // not turn an oversized fill into a fresh burst that bypasses the limit.
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = controller_.state() == ImportState::Verifying
        ? "resumed final verification" : "resumed; active partition will replay";
    return true;
}

void BuildImportRuntime::cancel() {
    std::lock_guard<std::recursive_mutex> lifecycle_lock(lifecycle_mutex_);
    resetCurrentRun(true);
}

void BuildImportRuntime::resetCurrentRun(bool discard_files) {
    run_generation_.fetch_add(1, std::memory_order_acq_rel);
    if (worker_.joinable()) worker_.join();
    // A cancel can happen immediately after the disable command was sent.
    // Preserve that uncertainty and force a restore before the next run.
    resetCommandFeedbackTracking();
    command_feedback_gate_.prepareForNewRun();
    releaseLoadedRegion(true);
    controller_.cancel(discard_files);
    releaseUndoClaimNoThrow();
    recovery_pending_ = false;
    phase_settle_ready_at_ = {};
    pending_resume_settle_delay_ = std::chrono::milliseconds(0);
    // Any in-flight cleanup barrier belongs to the previous run. Keep the
    // idempotent removal commands queued, but resend them with fresh UUIDs so
    // a cleared/late ACK cannot stall a newly started import for a full timeout.
    cleanup_barrier_uuids_.clear();
    cleanup_barrier_command_count_ = 0;
    cleanup_barrier_poll_at_ = {};
    cleanup_barrier_deadline_ = {};
    cleanup_retry_at_ = {};
    region_add_ready_at_ = {};
    cleanup_barrier_failure_count_ = 0;
    resetVerificationRuntime();
    resetCommandBlockRuntime();
    resetSignRuntime();
    resetContainerRuntime();
    resetEntityRuntime();
    resetMapCreationRuntime();
    resetActiveUnitRuntime();
    if (discard_files) cleanupSpoolDirectory(spool_directory_);
    spool_directory_.clear();
    undo_storage_directory_.clear();
    undo_record_id_.clear();
    undo_claim_job_id_.clear();
    durable_undo_block_count_ = 0;
    undo_run_active_ = false;
    region_command_spools_ = false;
    adaptive_region_grid_ = false;
    invalidateRpcTransport();
    native_context_change_pending_.store(false, std::memory_order_release);
    native_rpc_ack_observed_.store(false, std::memory_order_release);
    native_dimension_token_ = 0;
    unit_retry_counts_.clear();
    {
        std::lock_guard<std::mutex> event_lock(native_event_mutex_);
        pending_command_error_.clear();
        received_level_chunks_.clear();
        received_rpc_acks_.clear();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    total_block_count_ = 0;
    imported_block_count_ = 0;
    blocks_per_second_ = 20;
    available_block_tokens_ = 1.0;
    available_command_tokens_ = 1.0;
    telemetry_window_started_at_ = {};
    telemetry_data_commands_ = 0;
    telemetry_data_blocks_ = 0;
    telemetry_rpc_batches_ = 0;
    telemetry_rpc_commands_ = 0;
    telemetry_python_send_microseconds_ = 0;
    telemetry_batch_build_microseconds_ = 0;
    telemetry_barrier_count_ = 0;
    telemetry_barrier_microseconds_ = 0;
    telemetry_region_wait_count_ = 0;
    telemetry_region_wait_microseconds_ = 0;
    telemetry_prefetch_starvations_ = 0;
    status_.clear();
    degradation_notice_.clear();
    throughput_governor_.setRequestedBlocksPerSecond(20);
    throughput_governor_.resetFeedback();
    throughput_governor_.resetDispatchCadence();
}

ImportState BuildImportRuntime::state() const {
    return controller_.state();
}

std::string BuildImportRuntime::status() const {
    std::string current;
    std::string degradation_notice;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current = status_;
        degradation_notice = degradation_notice_;
    }
    if (current.empty()) current = controller_.failureReason();
    if (!degradation_notice.empty()) {
        if (!current.empty()) current += "; ";
        current += degradation_notice;
    }
    return current;
}

uint64_t BuildImportRuntime::totalBlockCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_block_count_;
}

uint64_t BuildImportRuntime::importedBlockCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return imported_block_count_;
}

}  // namespace build_import
