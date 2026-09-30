#include "MinecraftUpdateHook.h"
#include "../build_import/MapAnvilDebugBridge.h"
#include "PythonUtils.h"
#include "FunctionsAddress.h"
#include "LoopbackPacketSenderCapture.h"
#include "../build_import/BuildExportRuntime.h"
#include "../build_import/BuildImportRuntime.h"
#include "../build_import/ProjectionPrinterRuntime.h"
#include "../build_import/ProjectionWorldMatchRuntime.h"
#include "../build_import/PyRpcAckDecoder.h"
#include "../main.h"
#include "../log_control.h"
#include "dobby.h"
#include <openssl/sha.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>

#define LOG_TAG "Infinitecz_C_MinecraftUpdateHook"

// Actor::normalTick 原始函数指针
using ActorNormalTickFunction = void (*)(void*);
static void* Actor_normalTick_Origin = nullptr;

// 本地玩家指针（从 hook 中获取）
static std::atomic<void*> g_localPlayer(nullptr);
static std::atomic<int64_t> g_localPlayerLastTickNs(0);

namespace {

constexpr int64_t kLocalPlayerFreshnessNs = 1000LL * 1000LL * 1000LL;

using ActorGetClientInstanceFunction = void* (*)(void*);
std::atomic<uintptr_t> g_actorGetClientInstanceAddress{0};

int64_t monotonicNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool checkedFieldAddress(void* object, uintptr_t offset, const void** address) {
    if (!object || !address) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(object);
    if (base > std::numeric_limits<uintptr_t>::max() - offset) return false;
    *address = reinterpret_cast<const void*>(base + offset);
    return true;
}

bool readPointerField(void* object, uintptr_t offset, void** value) {
    if (!value) return false;
    *value = nullptr;
    const void* address = nullptr;
    if (!checkedFieldAddress(object, offset, &address) ||
        !IsMemoryReadable(address, sizeof(void*))) {
        return false;
    }
    std::memcpy(value, address, sizeof(*value));
    return true;
}

bool isLocalPlayerCandidate(void* actor) {
    if (!actor || !IsMemoryReadable(actor, sizeof(void*))) return false;

    // GameMode is a LocalPlayer member whose field offset changed between game
    // versions (the old 0x13B0 check is not a valid identity test anymore).
    // Actor::getClientInstance is instead a verified current-version getter;
    // remote actors have no usable client instance, while the local player does.
    uintptr_t address = g_actorGetClientInstanceAddress.load(std::memory_order_acquire);
    if (!address) {
        const uintptr_t base_address = Main::getBaseAddress();
        if (!base_address || !FunctionsAddress::Actor_getClientInstance ||
            !ResolveMinecraftExecutableOffset(base_address,
                                              FunctionsAddress::Actor_getClientInstance,
                                              sizeof(uint32_t), &address)) {
            return false;
        }
        g_actorGetClientInstanceAddress.store(address, std::memory_order_release);
    }

    const auto get_client = reinterpret_cast<ActorGetClientInstanceFunction>(address);
    void* client_instance = get_client(actor);
    return client_instance && IsMemoryReadable(client_instance, sizeof(void*));
}

std::string sha256Hex(const std::string& value) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (!SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest)) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(SHA256_DIGEST_LENGTH * 2, '0');
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        result[i * 2] = kHex[(digest[i] >> 4U) & 0x0fU];
        result[i * 2 + 1] = kHex[digest[i] & 0x0fU];
    }
    return result;
}

}  // namespace

void* GetLocalPlayerPointer() {
    void* actor = g_localPlayer.load(std::memory_order_acquire);
    if (!actor) return nullptr;
    const int64_t last_tick = g_localPlayerLastTickNs.load(std::memory_order_acquire);
    const int64_t now = monotonicNowNs();
    if (last_tick <= 0 || now < last_tick || now - last_tick > kLocalPlayerFreshnessNs ||
        !isLocalPlayerCandidate(actor)) {
        g_localPlayer.compare_exchange_strong(actor, nullptr, std::memory_order_acq_rel);
        return nullptr;
    }
    return g_localPlayer.load(std::memory_order_acquire) == actor ? actor : nullptr;
}

static std::atomic<bool> g_hookInstalled(false);
static std::atomic<bool> g_firstTick(true);

static std::mutex g_worldContextMutex;
static std::condition_variable g_worldContextCondition;
static uint64_t g_worldContextRequested = 0;
static uint64_t g_worldContextCompleted = 0;
static std::string g_cachedWorldContext = "unknown";
static uintptr_t g_cachedDimensionToken = 0;
static std::thread::id g_gameThreadId;

static std::mutex g_buildExportTeleportMutex;
static uint64_t g_buildExportTeleportRequested = 0;
static uint64_t g_buildExportTeleportCompleted = 0;
static bool g_buildExportTeleportDispatching = false;
static bool g_buildExportTeleportSent = false;
static bool g_buildExportTeleportAccepted = false;
static std::string g_buildExportTeleportUuid;
static std::string g_buildExportTeleportDetail;
static std::string g_buildExportTeleportUntaggedFailure;
static uintptr_t g_buildExportTeleportDimensionToken = 0;
static int64_t g_buildExportTeleportDeadlineNs = 0;
static bool g_buildExportTeleportActive = false;
static Vec3 g_buildExportTeleportTarget = {0, 0, 0};
// A build-export movement probe is deliberately confirmed from native actor
// coordinates rather than from a PyRpc command acknowledgement.  Some servers
// execute /tp correctly but do not return the acknowledgement shape expected
// by the client hook.
static int32_t g_buildExportTeleportOriginX = 0;
static int32_t g_buildExportTeleportOriginY = 0;
static int32_t g_buildExportTeleportOriginZ = 0;
static bool g_buildExportTeleportOriginValid = false;
constexpr int32_t kTeleportArrivalHorizontalRadius = 3;
constexpr int32_t kTeleportArrivalVerticalRadius = 16;

static bool isNearTeleportTarget(int32_t x, int32_t y, int32_t z,
                                 const Vec3& target) {
    const int64_t expected_x = static_cast<int64_t>(std::floor(target.x));
    const int64_t expected_y = static_cast<int64_t>(std::floor(target.y));
    const int64_t expected_z = static_cast<int64_t>(std::floor(target.z));
    return std::llabs(static_cast<int64_t>(x) - expected_x) <=
               kTeleportArrivalHorizontalRadius &&
        std::llabs(static_cast<int64_t>(z) - expected_z) <=
               kTeleportArrivalHorizontalRadius &&
        std::llabs(static_cast<int64_t>(y) - expected_y) <=
               kTeleportArrivalVerticalRadius;
}

static void CompleteBuildExportTeleport(uint64_t request, bool accepted,
                                       std::string detail) {
    std::lock_guard<std::mutex> probe_lock(g_buildExportTeleportMutex);
    if (request != g_buildExportTeleportRequested ||
        request <= g_buildExportTeleportCompleted) {
        return;
    }

    const uintptr_t expected_dimension = g_buildExportTeleportDimensionToken;
    const uintptr_t current_dimension =
        build_import::NativeWorldAccess::dimensionToken();
    const int64_t now = monotonicNowNs();
    if (accepted &&
        (g_buildExportTeleportDeadlineNs <= 0 ||
         now >= g_buildExportTeleportDeadlineNs)) {
        accepted = false;
        detail = "Export teleport arrived after the movement deadline";
    }
    if (accepted &&
        (expected_dimension == 0 || current_dimension != expected_dimension)) {
        accepted = false;
        detail = "TP command result belongs to a session, world, or dimension that is no longer current";
    }

    g_buildExportTeleportAccepted = accepted;
    g_buildExportTeleportDispatching = false;
    g_buildExportTeleportDetail = std::move(detail);
    g_buildExportTeleportCompleted = request;
}

static void ServiceBuildExportTeleport() {
    uint64_t request = 0;
    std::string uuid;
    bool dimension_matches = false;
    bool timed_out = false;
    bool movement_probe_waiting_for_arrival = false;
    Vec3 target = {0, 0, 0};
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    int32_t origin_z = 0;
    bool origin_valid = false;
    std::string timeout_detail;
    {
        std::lock_guard<std::mutex> lock(g_buildExportTeleportMutex);
        if (g_buildExportTeleportCompleted >= g_buildExportTeleportRequested) return;
        request = g_buildExportTeleportRequested;
        uuid = g_buildExportTeleportUuid;
        const int64_t now = monotonicNowNs();
        if (g_buildExportTeleportDeadlineNs > 0 &&
            now >= g_buildExportTeleportDeadlineNs) {
            timed_out = true;
            timeout_detail = "TP did not move the local player to the export region centre";
            if (!g_buildExportTeleportUntaggedFailure.empty()) {
                timeout_detail += ": " + g_buildExportTeleportUntaggedFailure;
            }
        } else if (g_buildExportTeleportActive &&
                   g_buildExportTeleportSent) {
            movement_probe_waiting_for_arrival = true;
            target = g_buildExportTeleportTarget;
            origin_x = g_buildExportTeleportOriginX;
            origin_y = g_buildExportTeleportOriginY;
            origin_z = g_buildExportTeleportOriginZ;
            origin_valid = g_buildExportTeleportOriginValid;
        } else {
            if (g_buildExportTeleportDispatching || g_buildExportTeleportSent) return;
            dimension_matches = g_buildExportTeleportDimensionToken != 0 &&
                build_import::NativeWorldAccess::dimensionToken() ==
                    g_buildExportTeleportDimensionToken;
            if (dimension_matches) g_buildExportTeleportDispatching = true;
        }
    }
    if (timed_out) {
        CompleteBuildExportTeleport(request, false, std::move(timeout_detail));
        return;
    }
    if (movement_probe_waiting_for_arrival) {
        int32_t player_x = 0;
        int32_t player_y = 0;
        int32_t player_z = 0;
        if (!build_import::NativeWorldAccess::getLocalPlayerBlockPosition(
                &player_x, &player_y, &player_z)) {
            return;
        }
        const bool arrived = isNearTeleportTarget(player_x, player_y, player_z, target);
        const bool moved = !origin_valid ||
            std::llabs(static_cast<int64_t>(player_x) - origin_x) > 1 ||
            std::llabs(static_cast<int64_t>(player_y) - origin_y) > 1 ||
            std::llabs(static_cast<int64_t>(player_z) - origin_z) > 1;
        if (arrived && moved) {
            CompleteBuildExportTeleport(
                request, true, "TP moved the local player to the export region centre");
            LOGI("teleport movement probe %llu reached (%d, %d, %d)",
                 static_cast<unsigned long long>(request), player_x, player_y, player_z);
        }
        return;
    }
    if (!dimension_matches) {
        CompleteBuildExportTeleport(
            request, false, "World or dimension changed before sending the TP command");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_buildExportTeleportMutex);
        if (request != g_buildExportTeleportRequested ||
            request <= g_buildExportTeleportCompleted ||
            !g_buildExportTeleportActive) return;
        target = g_buildExportTeleportTarget;
    }
    const std::string command =
        "/tp @s " + std::to_string(target.x) + " " + std::to_string(target.y) + " " +
        std::to_string(target.z);
    std::string code =
        "import msgpack, _pynetmodule\n"
        "import mod.client.extraClientApi as _infinitecz_tp_api\n"
        "def _infinitecz_tp_tuple(value):\n"
        "  return {'__type__': 'tuple', 'value': list(value)} if isinstance(value, tuple) else value\n"
        "_infinitecz_tp_player = _infinitecz_tp_api.GetLocalPlayerId()\n"
        "if _infinitecz_tp_player is None or str(_infinitecz_tp_player) in ('', '-1'):\n"
        "  raise RuntimeError('local player is unavailable')\n"
        "_infinitecz_tp_packet = msgpack.packb(('ModEventC2S', "
        "('Minecraft', 'aiCommand', 'ExecuteCommandEvent', "
        "{'playerId': _infinitecz_tp_player, 'cmd': '" + command + "', 'uuid': '" +
        uuid + "', 'aiModel': '-1'}), None), use_bin_type=True, strict_types=True, "
        "default=_infinitecz_tp_tuple)\n"
        "_pynetmodule.send2server(98247598, _infinitecz_tp_packet, "
        "len(_infinitecz_tp_packet))\n";
    const bool sent = PythonUtils::PyExecChecked(code, true);
    bool request_still_pending = false;
    {
        std::lock_guard<std::mutex> lock(g_buildExportTeleportMutex);
        request_still_pending = request == g_buildExportTeleportRequested &&
            request > g_buildExportTeleportCompleted;
        if (request_still_pending) {
            g_buildExportTeleportDispatching = false;
            // AvailableCheckFailed has no UUID. It may only be attributed to
            // this command after the Python send itself completed successfully.
            g_buildExportTeleportSent = sent;
        }
    }
    if (!request_still_pending) return;
    if (!sent) {
        LOGE("build export teleport %llu could not send the TP command",
             static_cast<unsigned long long>(request));
        CompleteBuildExportTeleport(
            request, false, "Unable to send the export TP command");
        return;
    }
    LOGI("build export teleport %llu sent uuid=%s command=%s",
         static_cast<unsigned long long>(request), uuid.c_str(), command.c_str());
}

static void ServiceWorldContextRequest() {
    uint64_t request = 0;
    {
        std::lock_guard<std::mutex> lock(g_worldContextMutex);
        if (g_worldContextCompleted >= g_worldContextRequested) return;
        request = g_worldContextRequested;
    }

    setenv("INFINITECZ_WORLD_CONTEXT", "unknown", 1);
    setenv("INFINITECZ_WORLD_CONTEXT_DEBUG", "python_not_executed", 1);
    setenv("INFINITECZ_WORLD_IDENTITY_KIND", "", 1);
    setenv("INFINITECZ_WORLD_IDENTITY_VALUE", "", 1);
    setenv("INFINITECZ_WORLD_DIMENSION", "", 1);
    // Decompiled client launch paths populate mc_game_ctrl with the saved
    // level id (Single), published game/server id (Network/Rental/Domain), or
    // owner level id (LAN/TAN). LobbyGame's room entity id is session-scoped,
    // so it deliberately falls through to volatile.
    std::string code(
        "import os\n"
        "_infinitecz_context = 'unknown'\n"
        "_infinitecz_identity_kind = ''\n"
        "_infinitecz_identity_value = ''\n"
        "_infinitecz_dimension = ''\n"
        "_infinitecz_debug = []\n"
        "def _infinitecz_utf8(value):\n"
        "  try:\n"
        "    if isinstance(value, unicode):\n"
        "      return value.encode('utf-8', 'replace')\n"
        "  except NameError:\n"
        "    pass\n"
        "  try:\n"
        "    return str(value)\n"
        "  except BaseException:\n"
        "    return '<unprintable>'\n"
        "try:\n"
        "  import mod.client.extraClientApi as api\n"
        "  level_id = api.GetLevelId()\n"
        "  player_id = api.GetLocalPlayerId()\n"
        "  level_text = '' if level_id is None else _infinitecz_utf8(level_id)\n"
        "  player_text = '' if player_id is None else _infinitecz_utf8(player_id)\n"
        "  _infinitecz_debug.append('level_id=' + level_text)\n"
        "  _infinitecz_debug.append('player_id=' + player_text)\n"
        "  level_available = level_text not in ('', '-1')\n"
        "  player_available = player_text not in ('', '-1')\n"
        "  dimension_id = -1\n"
        "  dimension_source = 'not_queried'\n"
        "  if level_available:\n"
        "    try:\n"
        "      import clientlevel\n"
        "      dimension_id = int(clientlevel.get_current_dimension())\n"
        "      dimension_source = 'clientlevel.get_current_dimension'\n"
        "    except BaseException as dimension_error:\n"
        "      _infinitecz_debug.append('dimension_primary_error=' + _infinitecz_utf8(dimension_error))\n"
        "      try:\n"
        "        dimension_id = int(api.GetEngineCompFactory().CreateGame(level_id).GetCurrentDimension())\n"
        "        dimension_source = 'CreateGame.GetCurrentDimension'\n"
        "      except BaseException as fallback_error:\n"
        "        _infinitecz_debug.append('dimension_fallback_error=' + _infinitecz_utf8(fallback_error))\n"
        "        dimension_id = -1\n"
        "        dimension_source = 'failed'\n"
        "  _infinitecz_debug.append('dimension_raw=' + str(dimension_id))\n"
        "  _infinitecz_debug.append('dimension_source=' + dimension_source)\n"
        // GetCurrentDimension deliberately returns -1 while a client is
        // finishing login or changing dimensions. Level/player IDs still prove
        // that a live world exists; use a volatile fallback context in that
        // short window and let the native dimension token guard later changes.
        "  dimension_available = dimension_id != -1\n"
        "  if level_available and not dimension_available:\n"
        "    dimension_id = 0\n"
        "  if level_available and dimension_id != -1:\n"
        "    identity_kind = 'volatile'\n"
        "    identity_value = level_text + '|' + (player_text if player_available else 'no-player')\n"
        "    try:\n"
        "      import mc_game_ctrl\n"
        "      game_info = mc_game_ctrl.instance.getCurGameInfo() or {}\n"
        "      game_type = _infinitecz_utf8(game_info.get('gameType', ''))\n"
        "      _infinitecz_debug.append('game_type=' + game_type)\n"
        "      _infinitecz_debug.append('game_id=' + _infinitecz_utf8(game_info.get('id', '')))\n"
        "      _infinitecz_debug.append('game_level_id=' + _infinitecz_utf8(game_info.get('level_id', '')))\n"
        "      _infinitecz_debug.append('owner_id=' + _infinitecz_utf8(game_info.get('owner_id', '')))\n"
        "      persistent_id = ''\n"
        "      persistent_extra = ''\n"
        "      if game_type == 'Single':\n"
        "        persistent_id = _infinitecz_utf8(game_info.get('id', ''))\n"
        "        try:\n"
        "          import clientlevel\n"
        "          persistent_extra = _infinitecz_utf8(clientlevel.get_level_path() or '')\n"
        "        except BaseException:\n"
        "          persistent_extra = ''\n"
        "      elif game_type in ('NetworkGame', 'RentalGame', 'DomainGame'):\n"
        "        persistent_id = _infinitecz_utf8(game_info.get('id', ''))\n"
        "      elif game_type in ('TanLobbyServer', 'TanLobbyClient', 'LanLobbyServer', 'LanLobbyClient'):\n"
        "        persistent_id = _infinitecz_utf8(game_info.get('level_id', ''))\n"
        "        persistent_extra = _infinitecz_utf8(game_info.get('owner_id', ''))\n"
        "      if persistent_id not in ('', '-1', 'None'):\n"
        "        identity_kind = 'stable'\n"
        "        identity_value = game_type + '|' + persistent_id + '|' + persistent_extra\n"
        "      else:\n"
        "        identity_value += '|' + game_type + '|' + _infinitecz_utf8(game_info.get('id', '')) + '|' + _infinitecz_utf8(game_info.get('level_id', ''))\n"
        "    except BaseException as game_info_error:\n"
        "      _infinitecz_debug.append('game_info_error=' + _infinitecz_utf8(game_info_error))\n"
        "    if not dimension_available or not player_available:\n"
        "      identity_kind = 'volatile'\n"
        "      identity_value += '|live-context-unavailable'\n"
        "    _infinitecz_identity_kind = identity_kind\n"
        "    _infinitecz_identity_value = identity_value\n"
        "    _infinitecz_dimension = str(dimension_id)\n"
        "except BaseException as context_error:\n"
        "  _infinitecz_debug.append('context_error=' + _infinitecz_utf8(context_error))\n"
        "  _infinitecz_context = 'unknown'\n"
        "try:\n"
        "  os.environ['INFINITECZ_WORLD_CONTEXT'] = _infinitecz_context\n"
        "  os.environ['INFINITECZ_WORLD_CONTEXT_DEBUG'] = '; '.join(_infinitecz_debug)\n"
        "  os.environ['INFINITECZ_WORLD_IDENTITY_KIND'] = _infinitecz_identity_kind\n"
        "  os.environ['INFINITECZ_WORLD_IDENTITY_VALUE'] = _infinitecz_identity_value\n"
        "  os.environ['INFINITECZ_WORLD_DIMENSION'] = _infinitecz_dimension\n"
        "except BaseException:\n"
        "  pass\n");
    const bool executed = PythonUtils::PyExecChecked(code, true);
    const char* debug_raw = std::getenv("INFINITECZ_WORLD_CONTEXT_DEBUG");
    const char* identity_kind_raw = std::getenv("INFINITECZ_WORLD_IDENTITY_KIND");
    const char* identity_value_raw = std::getenv("INFINITECZ_WORLD_IDENTITY_VALUE");
    const char* dimension_raw = std::getenv("INFINITECZ_WORLD_DIMENSION");
    std::string context = "unknown";
    if (executed && identity_kind_raw && identity_value_raw && dimension_raw &&
        (std::strcmp(identity_kind_raw, "stable") == 0 ||
         std::strcmp(identity_kind_raw, "volatile") == 0) &&
        *identity_value_raw && std::strlen(identity_value_raw) < 4096) {
        char* dimension_end = nullptr;
        errno = 0;
        const long dimension = std::strtol(dimension_raw, &dimension_end, 10);
        const std::string digest = sha256Hex(identity_value_raw);
        if (errno == 0 && dimension_end && dimension_end != dimension_raw &&
            *dimension_end == '\0' &&
            dimension >= std::numeric_limits<int32_t>::min() &&
            dimension <= std::numeric_limits<int32_t>::max() && !digest.empty()) {
            context = std::string(identity_kind_raw) + ":v1:" + digest + "|" +
                      std::to_string(dimension);
        }
    }
    LOGI("world-context raw values: python=%s; %s; result=%s",
         executed ? "executed" : "failed",
         debug_raw && *debug_raw ? debug_raw : "debug_unavailable",
         context.c_str());
    if (context == "unknown") {
        LOGE("world-context request %llu returned no usable level context (python=%s)",
             static_cast<unsigned long long>(request), executed ? "executed" : "failed");
    } else {
        LOGI("world-context request %llu completed (%s)",
             static_cast<unsigned long long>(request),
             context.compare(0, 12, "volatile:v1:") == 0 ? "live/volatile" : "stable");
    }
    const uintptr_t dimension_token = context == "unknown"
        ? 0 : build_import::NativeWorldAccess::dimensionToken();
    // A runtime-only level/entity id is useful for detecting changes during
    // this process, but must never masquerade as a checkpoint identity. Add
    // the live Dimension token to reduce accidental reuse after switching
    // worlds in one process. BuildImportUi rejects every volatile:v1 context
    // for checkpoint restore.
    if (dimension_token != 0 && context.compare(0, 12, "volatile:v1:") == 0) {
        const size_t separator = context.rfind('|');
        if (separator != std::string::npos) {
            context.insert(separator, ":" + std::to_string(dimension_token));
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_worldContextMutex);
        g_cachedWorldContext = std::move(context);
        g_cachedDimensionToken = dimension_token;
        g_worldContextCompleted = request;
    }
    g_worldContextCondition.notify_all();
}

static void Actor_normalTick_Hook(void* actor) {
    // Record the verified local player before its original tick, then drive
    // building operations only for that same actor after the tick.
    if (isLocalPlayerCandidate(actor)) {
        g_localPlayer.store(actor, std::memory_order_release);
        g_localPlayerLastTickNs.store(monotonicNowNs(), std::memory_order_release);
    }

    // 先调用原始函数
    const auto original = reinterpret_cast<ActorNormalTickFunction>(
        __atomic_load_n(&Actor_normalTick_Origin, __ATOMIC_ACQUIRE));
    if (original) {
        original(actor);
    }

    // Context requests only need the client Python/game thread. Do this before
    // the local-player gate: the GameMode field offset is version-sensitive and
    // must not make a valid in-world context query time out.
    {
        std::lock_guard<std::mutex> lock(g_worldContextMutex);
        g_gameThreadId = std::this_thread::get_id();
    }
    ServiceWorldContextRequest();

    if (!actor || actor != g_localPlayer.load(std::memory_order_acquire)) return;

    // Build export verifies TP from a real command to the active region centre
    // and the observed local-player coordinate. Sending is done on this tick
    // and the coordinate check never stalls the game thread.
    ServiceBuildExportTeleport();

    if (g_firstTick.exchange(false)) {
        LOGI("★ Actor::normalTick hook 已激活！");
    }

    build_import::TickMapAnvilDebugBridge();
    static bool runtime_exception_logged = false;
    {
        try {
            build_import::BuildImportRuntime::instance().onGameTick();
            runtime_exception_logged = false;
        } catch (const std::exception& error) {
            if (!runtime_exception_logged) {
                LOGE("BuildImportRuntime::onGameTick exception contained: %s", error.what());
                runtime_exception_logged = true;
            }
        } catch (...) {
            if (!runtime_exception_logged) {
                LOGE("BuildImportRuntime::onGameTick unknown exception contained");
                runtime_exception_logged = true;
            }
        }
    }

    {
        static bool export_runtime_exception_logged = false;
        try {
            build_import::BuildExportRuntime::instance().onGameTick();
            export_runtime_exception_logged = false;
        } catch (const std::exception& error) {
            if (!export_runtime_exception_logged) {
                LOGE("BuildExportRuntime::onGameTick exception contained: %s", error.what());
                export_runtime_exception_logged = true;
            }
        } catch (...) {
            if (!export_runtime_exception_logged) {
                LOGE("BuildExportRuntime::onGameTick unknown exception contained");
                export_runtime_exception_logged = true;
            }
        }
    }

    {
        static bool printer_runtime_exception_logged = false;
        try {
            build_import::ProjectionPrinterRuntime::instance().onGameTick();
            printer_runtime_exception_logged = false;
        } catch (const std::exception& error) {
            if (!printer_runtime_exception_logged) {
                LOGE("ProjectionPrinterRuntime::onGameTick exception contained: %s", error.what());
                printer_runtime_exception_logged = true;
            }
        } catch (...) {
            if (!printer_runtime_exception_logged) {
                LOGE("ProjectionPrinterRuntime::onGameTick unknown exception contained");
                printer_runtime_exception_logged = true;
            }
        }
    }

    // Keep the visual comparison on the same verified local-player game tick
    // as NativeWorldReader. The renderer receives only immutable value
    // snapshots, never engine-owned block pointers.
    {
        static bool projection_match_runtime_exception_logged = false;
        try {
            build_import::ProjectionWorldMatchRuntime::instance().onGameTick();
            projection_match_runtime_exception_logged = false;
        } catch (const std::exception& error) {
            if (!projection_match_runtime_exception_logged) {
                LOGE("ProjectionWorldMatchRuntime::onGameTick exception contained: %s",
                     error.what());
                projection_match_runtime_exception_logged = true;
            }
        } catch (...) {
            if (!projection_match_runtime_exception_logged) {
                LOGE("ProjectionWorldMatchRuntime::onGameTick unknown exception contained");
                projection_match_runtime_exception_logged = true;
            }
        }
    }

}

// 初始化 hook
bool InitMinecraftUpdateHook(uintptr_t baseAddr) {
    if (g_hookInstalled.load()) {
        LOGI("Hook 已安装，跳过");
        return true;
    }
    uintptr_t targetAddr = 0;
    if (!ResolveMinecraftExecutableOffset(baseAddr, FunctionsAddress::Minecraft_update_Hook,
                                          16, &targetAddr)) {
        LOGE("Actor::normalTick target is not executable in the Minecraft image: "
             "base=%p offset=%p", reinterpret_cast<void*>(baseAddr),
             reinterpret_cast<void*>(FunctionsAddress::Minecraft_update_Hook));
        return false;
    }
    LOGI("★ 尝试安装 Actor::normalTick hook: 0x%lx (offset=0x%lx)",
         (unsigned long)targetAddr, (unsigned long)FunctionsAddress::Minecraft_update_Hook);

    int res = DobbyHook(
        (void*)targetAddr,
        (void*)Actor_normalTick_Hook,
        &Actor_normalTick_Origin
    );

    const void* original = __atomic_load_n(&Actor_normalTick_Origin, __ATOMIC_ACQUIRE);
    if (res == 0 && original) {
        LOGI("★ DobbyHook 安装成功！原始函数: %p", original);
        g_hookInstalled.store(true);
        return true;
    } else {
        LOGE("× DobbyHook 安装失败，错误码: %d", res);
        return false;
    }
}

bool ObserveBuildExportTeleportPacket(const std::string& packet) {
    build_import::PyRpcAckEvent event;
    if (!build_import::decodePyRpcAckPacket(packet, &event)) return false;

    std::lock_guard<std::mutex> lock(g_buildExportTeleportMutex);
    if (event.kind == build_import::PyRpcAckEventKind::AfterExecuteCommand ||
        event.kind == build_import::PyRpcAckEventKind::ExecuteCommandOutput) {
        // Only hide feedback belonging to our own export movement request.
        // Arrival is verified from player coordinates, never from an ACK.
        return build_import::isBuildExportTeleportProbeUuid(event.uuid) &&
            event.uuid == g_buildExportTeleportUuid;
    }
    if (event.kind == build_import::PyRpcAckEventKind::AvailableCheckFailed &&
        g_buildExportTeleportSent &&
        g_buildExportTeleportCompleted < g_buildExportTeleportRequested) {
        // This event has no UUID. Preserve it in the game UI and use it only
        // as extra timeout diagnostics for the pending export movement.
        g_buildExportTeleportUntaggedFailure = event.reason.empty()
            ? "availability check failed without a reason" : event.reason;
    }
    return false;
}

void CancelBuildExportTeleport() {
    std::lock_guard<std::mutex> lock(g_buildExportTeleportMutex);
    if (g_buildExportTeleportCompleted < g_buildExportTeleportRequested) {
        g_buildExportTeleportAccepted = false;
        g_buildExportTeleportCompleted = g_buildExportTeleportRequested;
    }
    g_buildExportTeleportDispatching = false;
    g_buildExportTeleportSent = false;
    g_buildExportTeleportDetail.clear();
    g_buildExportTeleportUntaggedFailure.clear();
    g_buildExportTeleportDimensionToken = 0;
    g_buildExportTeleportDeadlineNs = 0;
    g_buildExportTeleportActive = false;
    g_buildExportTeleportOriginValid = false;
}

// Requests a real server TP and confirms it from the local actor's position.
// This runs on Actor::normalTick, so NativeWorldAccess is allowed to read the
// actor coordinate directly.  No native Actor::teleportTo is published here:
// doing so would make a denied server command look successful.
bool RequestBuildExportTeleport(float x, float y, float z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return false;
    }

    const uintptr_t request_dimension = build_import::NativeWorldAccess::dimensionToken();
    int32_t player_x = 0;
    int32_t player_y = 0;
    int32_t player_z = 0;
    if (request_dimension == 0 ||
        !build_import::NativeWorldAccess::getLocalPlayerBlockPosition(
            &player_x, &player_y, &player_z)) {
        return false;
    }
    const Vec3 target = {x, y, z};

    std::lock_guard<std::mutex> probe_lock(g_buildExportTeleportMutex);
    if (build_import::NativeWorldAccess::dimensionToken() != request_dimension) {
        return false;
    }

    const bool probe_pending =
        g_buildExportTeleportCompleted < g_buildExportTeleportRequested;
    const bool same_target =
        g_buildExportTeleportTarget.x == x &&
        g_buildExportTeleportTarget.y == y &&
        g_buildExportTeleportTarget.z == z;
    if (probe_pending) {
        if (g_buildExportTeleportActive &&
            g_buildExportTeleportDimensionToken == request_dimension && same_target) {
            return true;
        }
        // Supersede a stale probe.  The previous server command cannot be
        // recalled, but its coordinate arrival is no longer accepted for this
        // batch and the next tick will send the new command.
        ++g_buildExportTeleportRequested;
        g_buildExportTeleportCompleted = g_buildExportTeleportRequested;
        g_buildExportTeleportActive = false;
        g_buildExportTeleportAccepted = true;
    }

    // Being already inside the target arrival window needs no server command
    // and is safe: the scanner can use the currently loaded region directly.
    // This also covers a player who reached the target only after a probe
    // deadline expired: the observed position outranks the timed-out probe.
    if (isNearTeleportTarget(player_x, player_y, player_z, target)) return true;

    // A failed movement probe stays rejected for the same coordinates, so the
    // caller can observe the failure. A different target (for example a
    // shorter hop toward the same region on servers that refuse long-distance
    // TPs) starts a fresh probe that must again prove real player movement.
    if (g_buildExportTeleportActive &&
        !g_buildExportTeleportAccepted && same_target) {
        LOGE("teleport request rejected: the preceding coordinate verification failed (%s)",
             g_buildExportTeleportDetail.empty()
                 ? "local player did not reach the target"
                 : g_buildExportTeleportDetail.c_str());
        return false;
    }

    const uint64_t request = ++g_buildExportTeleportRequested;
    const int64_t now = monotonicNowNs();
    g_buildExportTeleportUuid =
        std::string(build_import::kBuildExportTeleportProbeUuidPrefix) +
        std::to_string(static_cast<uint64_t>(now)) + "_" + std::to_string(request);
    g_buildExportTeleportDispatching = false;
    g_buildExportTeleportSent = false;
    g_buildExportTeleportAccepted = false;
    g_buildExportTeleportDetail = "Sending TP to the export region centre";
    g_buildExportTeleportUntaggedFailure.clear();
    g_buildExportTeleportDimensionToken = request_dimension;
    g_buildExportTeleportDeadlineNs = now + 6LL * 1000LL * 1000LL * 1000LL;
    g_buildExportTeleportActive = true;
    g_buildExportTeleportTarget = target;
    g_buildExportTeleportOriginX = player_x;
    g_buildExportTeleportOriginY = player_y;
    g_buildExportTeleportOriginZ = player_z;
    g_buildExportTeleportOriginValid = true;
    LOGI("teleport movement probe %llu queued from (%d, %d, %d) to (%.1f, %.1f, %.1f)",
         static_cast<unsigned long long>(request), player_x, player_y, player_z, x, y, z);
    return true;
}

bool IsBuildExportTeleportPending() {
    std::lock_guard<std::mutex> lock(g_buildExportTeleportMutex);
    return g_buildExportTeleportActive &&
        g_buildExportTeleportCompleted < g_buildExportTeleportRequested;
}

bool IsMinecraftUpdateGameThread() noexcept {
    if (!g_hookInstalled.load(std::memory_order_acquire)) return false;
    try {
        std::lock_guard<std::mutex> lock(g_worldContextMutex);
        return g_gameThreadId != std::thread::id{} &&
            g_gameThreadId == std::this_thread::get_id();
    } catch (...) {
        return false;
    }
}

bool QueryWorldContextOnGameThread(std::string* output, int timeout_ms) {
    if (!output || timeout_ms <= 0 ||
        !g_hookInstalled.load(std::memory_order_acquire)) {
        return false;
    }
    std::unique_lock<std::mutex> lock(g_worldContextMutex);
    const uint64_t request = ++g_worldContextRequested;
    if (g_gameThreadId == std::this_thread::get_id()) {
        // A JNI caller can occasionally already be on the Actor tick thread.
        // Waiting for that same thread would always time out, so service the
        // request inline while the engine/Python context is valid.
        lock.unlock();
        ServiceWorldContextRequest();
        lock.lock();
    } else {
        if (!g_worldContextCondition.wait_for(
                lock, std::chrono::milliseconds(timeout_ms),
                [&]() { return g_worldContextCompleted >= request; })) {
            return false;
        }
    }
    if (g_worldContextCompleted < request) return false;
    *output = g_cachedWorldContext;
    return !output->empty() && *output != "unknown";
}

uintptr_t GetCachedDimensionTokenForWorld(const std::string& world_context) {
    std::lock_guard<std::mutex> lock(g_worldContextMutex);
    return world_context == g_cachedWorldContext ? g_cachedDimensionToken : 0;
}
