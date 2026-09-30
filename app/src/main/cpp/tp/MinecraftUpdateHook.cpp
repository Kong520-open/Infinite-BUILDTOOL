#include "MinecraftUpdateHook.h"
#include "../build_import/MapAnvilDebugBridge.h"
#include "TpModule.h"
#include "PythonUtils.h"
#include "FunctionsAddress.h"
#include "LoopbackPacketSenderCapture.h"
#include "LightningEffect.h"
#include "../build_import/BuildExportRuntime.h"
#include "../build_import/BuildImportRuntime.h"
#include "../build_import/ProjectionPrinterRuntime.h"
#include "../build_import/ProjectionWorldMatchRuntime.h"
#include "../build_import/PyRpcAckDecoder.h"
#include "../native_auth.h"
#include "../main.h"
#include "../log_control.h"
#include "dobby.h"
#include <openssl/sha.h>
#include <openssl/crypto.h>
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

// Actor::teleportTo 函数签名
// void Actor::teleportTo(Actor* this, Vec3 const& pos, bool a, int b, int c)
// Vec3 通过引用传递（ARM64 下是通过指针）
typedef void (*teleportTo_t)(void*, Vec3*, bool, int, int);

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

// 待执行的 Python 代码
static std::string g_pendingCode;
static std::mutex g_codeMutex;

// 状态标志
static std::atomic<bool> g_hasPendingCode(false);
static std::atomic<bool> g_hookInstalled(false);
static std::atomic<bool> g_firstTick(true);

// 时间控制
static auto g_lastExecTime = std::chrono::steady_clock::now();
static const int EXEC_INTERVAL_MS = 1000;

// 执行结果

static std::mutex g_worldContextMutex;
static std::condition_variable g_worldContextCondition;
static uint64_t g_worldContextRequested = 0;
static uint64_t g_worldContextCompleted = 0;
static std::string g_cachedWorldContext = "unknown";
static uintptr_t g_cachedDimensionToken = 0;
static std::thread::id g_gameThreadId;

static std::mutex g_playerPermissionMutex;
static std::condition_variable g_playerPermissionCondition;
static uint64_t g_playerPermissionRequested = 0;
static uint64_t g_playerPermissionCompleted = 0;
static int g_cachedPlayerPermission = -1;
static bool g_cachedPlayerPermissionValid = false;

static std::mutex g_teleportPermissionProbeMutex;
static std::condition_variable g_teleportPermissionProbeCondition;
static uint64_t g_teleportPermissionProbeRequested = 0;
static uint64_t g_teleportPermissionProbeCompleted = 0;
static bool g_teleportPermissionProbeDispatching = false;
static bool g_teleportPermissionProbeSent = false;
static bool g_teleportPermissionProbeAccepted = false;
static std::string g_teleportPermissionProbeUuid;
static std::string g_teleportPermissionProbeDetail;
static std::string g_teleportPermissionProbeUntaggedFailure;
static uintptr_t g_teleportPermissionProbeDimensionToken = 0;
static int64_t g_teleportPermissionProbeDeadlineNs = 0;
static bool g_teleportPermissionProbePublishesTeleport = false;
static Vec3 g_teleportPermissionProbeTarget = {0, 0, 0};
// A build-export movement probe is deliberately confirmed from native actor
// coordinates rather than from a PyRpc command acknowledgement.  Some servers
// execute /tp correctly but do not return the acknowledgement shape expected
// by the client hook.
static int32_t g_teleportPermissionProbeOriginX = 0;
static int32_t g_teleportPermissionProbeOriginY = 0;
static int32_t g_teleportPermissionProbeOriginZ = 0;
static bool g_teleportPermissionProbeOriginValid = false;
static std::atomic<uint64_t> g_verifiedTeleportCredential{0};
static std::atomic<uintptr_t> g_verifiedTeleportDimensionToken{0};
static std::atomic<int64_t> g_verifiedTeleportAtNs{0};
constexpr int64_t kTeleportPermissionValidityNs = 2LL * 60LL * 1000LL * 1000LL * 1000LL;
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

static void clearSensitiveString(std::string& value) {
    if (!value.empty()) OPENSSL_cleanse(value.data(), value.size());
    std::string empty;
    value.swap(empty);
}

static void CompleteTeleportPermissionProbe(uint64_t request, bool accepted,
                                            std::string detail) {
    std::lock_guard<std::mutex> probe_lock(g_teleportPermissionProbeMutex);
    if (request != g_teleportPermissionProbeRequested ||
        request <= g_teleportPermissionProbeCompleted) {
        return;
    }

    const uintptr_t expected_dimension = g_teleportPermissionProbeDimensionToken;
    const uintptr_t current_dimension =
        build_import::NativeWorldAccess::dimensionToken();
    const int64_t now = monotonicNowNs();
    if (accepted &&
        (g_teleportPermissionProbeDeadlineNs <= 0 ||
         now >= g_teleportPermissionProbeDeadlineNs)) {
        accepted = false;
        detail = "TP command result arrived after the permission probe deadline";
    }
    if (accepted &&
        (expected_dimension == 0 || current_dimension != expected_dimension)) {
        accepted = false;
        detail = "TP command result belongs to a session, world, or dimension that is no longer current";
    }

    g_teleportPermissionProbeAccepted = accepted;
    g_teleportPermissionProbeDispatching = false;
    g_teleportPermissionProbeDetail = std::move(detail);
    g_teleportPermissionProbeCompleted = request;
    if (accepted) {
        g_verifiedTeleportDimensionToken.store(current_dimension, std::memory_order_relaxed);
        g_verifiedTeleportAtNs.store(now, std::memory_order_relaxed);
        // For an export movement probe this release happens only after the
        // local actor position has reached the server-requested centre.
        g_verifiedTeleportCredential.store(request, std::memory_order_release);
    } else {
        g_verifiedTeleportCredential.store(0, std::memory_order_release);
        g_verifiedTeleportDimensionToken.store(0, std::memory_order_relaxed);
        g_verifiedTeleportAtNs.store(0, std::memory_order_relaxed);
    }
    g_teleportPermissionProbeCondition.notify_all();
}

static void ServiceTeleportPermissionProbe() {
    uint64_t request = 0;
    std::string uuid;
    bool dimension_matches = false;
    bool timed_out = false;
    bool movement_probe_waiting_for_arrival = false;
    bool publishes_teleport = false;
    Vec3 target = {0, 0, 0};
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    int32_t origin_z = 0;
    bool origin_valid = false;
    std::string timeout_detail;
    {
        std::lock_guard<std::mutex> lock(g_teleportPermissionProbeMutex);
        if (g_teleportPermissionProbeCompleted >= g_teleportPermissionProbeRequested) return;
        request = g_teleportPermissionProbeRequested;
        uuid = g_teleportPermissionProbeUuid;
        const int64_t now = monotonicNowNs();
        if (g_teleportPermissionProbeDeadlineNs > 0 &&
            now >= g_teleportPermissionProbeDeadlineNs) {
            timed_out = true;
            timeout_detail = g_teleportPermissionProbePublishesTeleport
                ? "TP did not move the local player to the export region centre"
                : (g_teleportPermissionProbeUntaggedFailure.empty()
                    ? "TP permission probe timed out without a matching command result"
                    : "TP command reported an untagged availability failure and no matching "
                      "command result arrived: " + g_teleportPermissionProbeUntaggedFailure);
        } else if (g_teleportPermissionProbePublishesTeleport &&
                   g_teleportPermissionProbeSent) {
            movement_probe_waiting_for_arrival = true;
            target = g_teleportPermissionProbeTarget;
            origin_x = g_teleportPermissionProbeOriginX;
            origin_y = g_teleportPermissionProbeOriginY;
            origin_z = g_teleportPermissionProbeOriginZ;
            origin_valid = g_teleportPermissionProbeOriginValid;
        } else {
            if (g_teleportPermissionProbeDispatching || g_teleportPermissionProbeSent) return;
            dimension_matches = g_teleportPermissionProbeDimensionToken != 0 &&
                build_import::NativeWorldAccess::dimensionToken() ==
                    g_teleportPermissionProbeDimensionToken;
            if (dimension_matches) g_teleportPermissionProbeDispatching = true;
        }
    }
    if (timed_out) {
        CompleteTeleportPermissionProbe(request, false, std::move(timeout_detail));
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
            CompleteTeleportPermissionProbe(
                request, true, "TP moved the local player to the export region centre");
            LOGI("teleport movement probe %llu reached (%d, %d, %d)",
                 static_cast<unsigned long long>(request), player_x, player_y, player_z);
        }
        return;
    }
    if (!dimension_matches) {
        CompleteTeleportPermissionProbe(
            request, false, "World or dimension changed before sending the TP command");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_teleportPermissionProbeMutex);
        publishes_teleport = request == g_teleportPermissionProbeRequested &&
            g_teleportPermissionProbePublishesTeleport;
        if (publishes_teleport) target = g_teleportPermissionProbeTarget;
    }
    const std::string command = publishes_teleport
        ? "/tp @s " + std::to_string(target.x) + " " + std::to_string(target.y) + " " +
              std::to_string(target.z)
        : "/tp @s ~ ~ ~";
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
        std::lock_guard<std::mutex> lock(g_teleportPermissionProbeMutex);
        request_still_pending = request == g_teleportPermissionProbeRequested &&
            request > g_teleportPermissionProbeCompleted;
        if (request_still_pending) {
            g_teleportPermissionProbeDispatching = false;
            // AvailableCheckFailed has no UUID. It may only be attributed to
            // this command after the Python send itself completed successfully.
            g_teleportPermissionProbeSent = sent;
        }
    }
    if (!request_still_pending) return;
    if (!sent) {
        LOGE("teleport permission probe %llu could not send the TP command",
             static_cast<unsigned long long>(request));
        CompleteTeleportPermissionProbe(
            request, false, "Unable to send the TP permission probe command");
        return;
    }
    LOGI("teleport permission probe %llu sent uuid=%s command=%s",
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

static void ServicePlayerPermissionRequest() {
    // clientlevel permission state is not reliable until the local player has
    // been constructed. Keep the request pending so the first local-player
    // tick, rather than an arbitrary NPC/remote actor tick, performs the read.
    if (!g_localPlayer.load(std::memory_order_acquire)) return;
    uint64_t request = 0;
    {
        std::lock_guard<std::mutex> lock(g_playerPermissionMutex);
        if (g_playerPermissionCompleted >= g_playerPermissionRequested) return;
        request = g_playerPermissionRequested;
    }

    std::string value;
    const bool evaluated = PythonUtils::PyEvalUtf8(
        "import clientlevel\n"
        "_infinitecz_player_permission = clientlevel.get_player_permissions()\n"
        "import operator\n"
        "try:\n"
        "  _infinitecz_permission_string_types = (basestring,)\n"
        "except NameError:\n"
        "  _infinitecz_permission_string_types = (str,)\n"
        "def _infinitecz_permission_true(value):\n"
        "  try:\n"
        "    if value is True: return True\n"
        "    try:\n"
        "      return type(value) in (int, long) and value == 1\n"
        "    except NameError:\n"
        "      return type(value) is int and value == 1\n"
        "  except BaseException:\n"
        "    return False\n"
        "def _infinitecz_permission_text(value):\n"
        "  try:\n"
        "    if not isinstance(value, _infinitecz_permission_string_types): return None\n"
        "    if value == '0': return 0\n"
        "    if value == '1': return 1\n"
        "    if value == '2': return 2\n"
        "    if value == '3': return 3\n"
        "    if value in ('Owner', 'OWNER', 'Operator', 'OPERATOR'): return 2\n"
        "  except BaseException:\n"
        "    pass\n"
        "  return None\n"
        "def _infinitecz_permission_index(value):\n"
        "  try:\n"
        "    if isinstance(value, bool): return None\n"
        "    _text_value = _infinitecz_permission_text(value)\n"
        "    if _text_value is not None: return _text_value\n"
        "    _index_value = operator.index(value)\n"
        "    if isinstance(_index_value, bool): return None\n"
        "    if _index_value < 0 or _index_value > 3: return None\n"
        "    return _index_value\n"
        "  except BaseException:\n"
        "    return None\n"
        "def _infinitecz_permission_mapping(value):\n"
        "  try:\n"
        "    _keys = ('level', 'permission', 'permission_level', 'player_permission',\n"
        "             'player_permission_level', 'value')\n"
        "    _values = []\n"
        "    for _key in _keys:\n"
        "      if _key in value:\n"
        "        _candidate = _infinitecz_permission_index(value[_key])\n"
        "        if _candidate is None: return None\n"
        "        _values.append(_candidate)\n"
        "    if _values and len(set(_values)) == 1: return _values[0]\n"
        "  except BaseException:\n"
        "    pass\n"
        "  return None\n"
        "def _infinitecz_permission_normalize(value):\n"
        "  _candidate = _infinitecz_permission_index(value)\n"
        "  if _candidate is not None: return _candidate\n"
        "  try:\n"
        "    if isinstance(value, dict):\n"
        "      _candidate = _infinitecz_permission_mapping(value)\n"
        "      if _candidate is not None: return _candidate\n"
        "    if isinstance(value, (tuple, list)) and len(value) == 1:\n"
        "      _candidate = _infinitecz_permission_index(value[0])\n"
        "      if _candidate is not None: return _candidate\n"
        "    _values = []\n"
        "    for _name in ('value', 'level', 'permission', 'permission_level',\n"
        "                   'player_permission', 'player_permission_level'):\n"
        "      try:\n"
        "        if hasattr(value, _name):\n"
        "          _candidate = _infinitecz_permission_index(getattr(value, _name))\n"
        "          if _candidate is not None: _values.append(_candidate)\n"
        "      except BaseException:\n"
        "        pass\n"
        "    if _values and len(set(_values)) == 1: return _values[0]\n"
        "  except BaseException:\n"
        "    pass\n"
        "  return None\n"
        "_infinitecz_player_permission_normalized = _infinitecz_permission_normalize("
            "_infinitecz_player_permission)\n"
        "_infinitecz_player_permission_owner = False\n"
        "_infinitecz_player_permission_cheats = False\n"
        "try:\n"
        "  _infinitecz_player_permission_owner = _infinitecz_permission_true(\n"
        "      clientlevel.is_local_player_owner())\n"
        "except BaseException:\n"
        "  pass\n"
        "try:\n"
        "  _infinitecz_player_permission_cheats = _infinitecz_permission_true(\n"
        "      clientlevel.get_allow_cheats())\n"
        "except BaseException:\n"
        "  pass\n"
        "_infinitecz_player_permission_source = 'direct'\n"
        "if (_infinitecz_player_permission_normalized is None and\n"
        "    _infinitecz_player_permission_owner and _infinitecz_player_permission_cheats):\n"
        "  _infinitecz_player_permission_normalized = 2\n"
        "  _infinitecz_player_permission_source = 'owner+cheats'\n"
        "try:\n"
        "  _infinitecz_player_permission_type = (\n"
        "      type(_infinitecz_player_permission).__module__ + '.' +\n"
        "      type(_infinitecz_player_permission).__name__)\n"
        "except BaseException:\n"
        "  _infinitecz_player_permission_type = 'unknown'\n"
        "try:\n"
        "  _infinitecz_player_permission_repr = repr(_infinitecz_player_permission)\n"
        "  try:\n"
        "    if isinstance(_infinitecz_player_permission_repr, unicode):\n"
        "      _infinitecz_player_permission_repr = _infinitecz_player_permission_repr.encode(\n"
        "          'utf-8', 'replace')\n"
        "  except NameError:\n"
        "    pass\n"
        "  _infinitecz_player_permission_repr = str(_infinitecz_player_permission_repr)\n"
        "  _infinitecz_player_permission_repr = _infinitecz_player_permission_repr.replace(\n"
        "      '\\t', ' ').replace('\\r', ' ').replace('\\n', ' ')[:160]\n"
        "except BaseException:\n"
        "  _infinitecz_player_permission_repr = '<repr unavailable>'\n",
        "('%s\\t%s\\t%s\\t%s\\t%s\\t%s' % (\n"
        "  'invalid' if _infinitecz_player_permission_normalized is None else\n"
        "      str(_infinitecz_player_permission_normalized),\n"
        "  _infinitecz_player_permission_type, _infinitecz_player_permission_repr,\n"
        "  '1' if _infinitecz_player_permission_owner else '0',\n"
        "  '1' if _infinitecz_player_permission_cheats else '0',\n"
        "  _infinitecz_player_permission_source))",
        &value);

    int permission = -1;
    bool valid = false;
    std::string normalized = value;
    const size_t first_tab = value.find('\t');
    if (first_tab != std::string::npos) normalized.resize(first_tab);
    if (evaluated && normalized.size() == 1 && normalized[0] >= '0' &&
        normalized[0] <= '3') {
        permission = normalized[0] - '0';
        valid = true;
    }
    const auto diagnosticField = [&value](size_t field_index) {
        size_t begin = 0;
        for (size_t index = 0; index < field_index; ++index) {
            const size_t separator = value.find('\t', begin);
            if (separator == std::string::npos) return std::string();
            begin = separator + 1;
        }
        const size_t end = value.find('\t', begin);
        return value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    };
    const std::string raw_type = diagnosticField(1);
    const std::string raw_repr = diagnosticField(2);
    const std::string owner_flag = diagnosticField(3);
    const std::string cheats_flag = diagnosticField(4);
    const std::string normalization_source = diagnosticField(5);
    {
        std::lock_guard<std::mutex> lock(g_playerPermissionMutex);
        // Revocation may invalidate and complete outstanding requests while
        // this game-thread query is evaluating. Never let an older result
        // overwrite that fail-closed completion.
        if (request > g_playerPermissionCompleted) {
            g_cachedPlayerPermission = permission;
            g_cachedPlayerPermissionValid = valid;
            g_playerPermissionCompleted = request;
        }
    }
    g_playerPermissionCondition.notify_all();
    if (valid) {
        LOGI("player-permission request %llu completed with level %d source=%s "
             "raw_type=%s raw_repr=%s owner=%s cheats=%s",
             static_cast<unsigned long long>(request), permission,
             normalization_source.empty() ? "unknown" : normalization_source.c_str(),
             raw_type.empty() ? "unknown" : raw_type.c_str(),
             raw_repr.empty() ? "unknown" : raw_repr.c_str(),
             owner_flag == "1" ? "true" : "false",
             cheats_flag == "1" ? "true" : "false");
    } else {
        LOGE("player-permission request %llu failed: evaluated=%s normalized=%s "
             "raw_type=%s raw_repr=%s owner=%s cheats=%s",
             static_cast<unsigned long long>(request), evaluated ? "true" : "false",
             normalized.empty() ? "invalid" : normalized.c_str(),
             raw_type.empty() ? "unknown" : raw_type.c_str(),
             raw_repr.empty() ? "unknown" : raw_repr.c_str(),
             owner_flag == "1" ? "true" : "false",
             cheats_flag == "1" ? "true" : "false");
    }
}

// Actor::normalTick hook 函数
static void Actor_normalTick_Hook(void* actor) {
    // Match the reference ActorTick lifecycle: record the actor marked with a
    // GameMode before its original tick, then only run player modules for the
    // remembered local-player instance afterwards.
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
    ServiceTeleportPermissionProbe();

    // Unlike world identity, permission state can be invalid before the local
    // player exists. Service it only on the confirmed local-player tick so an
    // early remote-actor tick cannot complete the request with a false denial.
    ServicePlayerPermissionRequest();

    if (g_firstTick.exchange(false)) {
        LOGI("★ Actor::normalTick hook 已激活！");
        g_lastExecTime = std::chrono::steady_clock::now();
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

    // 检查是否有待执行的 Python 代码
    if (!g_hasPendingCode.load()) return;

    // 限频
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastExecTime).count();
    if (elapsed < EXEC_INTERVAL_MS) return;
    g_lastExecTime = now;

    std::string code;
    {
        std::lock_guard<std::mutex> lock(g_codeMutex);
        if (!g_hasPendingCode.load(std::memory_order_acquire)) return;
        code.swap(g_pendingCode);
        g_hasPendingCode.store(false, std::memory_order_release);
    }

    if (code.empty()) return;

    LOGI("★ 从 Actor::normalTick 执行 Python 代码 (%zu bytes)", code.size());
    PythonUtils::PyExec(code, true);
    LOGI("★ Python 代码执行完成");

    clearSensitiveString(code);
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

bool ProbeTeleportPermissionOnGameThread(std::string* detail, int timeout_ms) {
    if (detail) detail->clear();
    const bool session_authorized = IsNativeSessionAuthorized();
    const bool hook_installed = g_hookInstalled.load(std::memory_order_acquire);
    const uintptr_t base_address = Main::getBaseAddress();
    const bool feedback_hook_ready = session_authorized && base_address != 0 &&
        LightningEffect::init(base_address);
    if (!session_authorized || timeout_ms <= 0 || !hook_installed ||
        !feedback_hook_ready) {
        if (detail) {
            *detail = !session_authorized
                ? "Native session is unavailable"
                : (!hook_installed ? "Local-player tick hook is unavailable"
                   : (!feedback_hook_ready ? "Command-result receive hook is unavailable"
                                           : "Invalid TP probe timeout"));
        }
        return false;
    }

    std::thread::id game_thread_id;
    {
        std::lock_guard<std::mutex> lock(g_worldContextMutex);
        game_thread_id = g_gameThreadId;
    }
    // A command ACK cannot arrive while its receive/game thread is blocked in
    // this function. The export UI always calls from its worker thread.
    if (game_thread_id == std::this_thread::get_id()) {
        if (detail) *detail = "TP permission probe cannot wait on the game thread";
        return false;
    }

    const uintptr_t request_dimension =
        build_import::NativeWorldAccess::dimensionToken();
    if (request_dimension == 0) {
        if (detail) *detail = "Local player or dimension is unavailable";
        return false;
    }
    CancelPendingTeleportRequest();

    std::unique_lock<std::mutex> lock(g_teleportPermissionProbeMutex);
    if (g_teleportPermissionProbeCompleted < g_teleportPermissionProbeRequested) {
        if (detail) *detail = "Another TP permission probe is already running";
        return false;
    }

    const uint64_t request = ++g_teleportPermissionProbeRequested;
    const uint64_t clock_value = static_cast<uint64_t>(monotonicNowNs());
    g_teleportPermissionProbeUuid =
        std::string(build_import::kBuildExportTeleportProbeUuidPrefix) +
        std::to_string(clock_value) + "_" + std::to_string(request);
    g_teleportPermissionProbeDispatching = false;
    g_teleportPermissionProbeSent = false;
    g_teleportPermissionProbeAccepted = false;
    g_teleportPermissionProbeDetail = "Waiting for the TP command result";
    g_teleportPermissionProbeUntaggedFailure.clear();
    g_teleportPermissionProbeDimensionToken = request_dimension;
    g_teleportPermissionProbeDeadlineNs = monotonicNowNs() +
        static_cast<int64_t>(timeout_ms) * 1000LL * 1000LL;
    g_teleportPermissionProbePublishesTeleport = false;
    g_teleportPermissionProbeTarget = {0, 0, 0};
    g_teleportPermissionProbeOriginValid = false;
    g_verifiedTeleportCredential.store(0, std::memory_order_release);
    g_verifiedTeleportDimensionToken.store(0, std::memory_order_release);
    g_verifiedTeleportAtNs.store(0, std::memory_order_release);

    const bool completed = g_teleportPermissionProbeCondition.wait_for(
        lock, std::chrono::milliseconds(timeout_ms), [&]() {
            return g_teleportPermissionProbeCompleted >= request;
        });
    if (!completed) {
        if (request == g_teleportPermissionProbeRequested &&
            request > g_teleportPermissionProbeCompleted) {
            g_teleportPermissionProbeAccepted = false;
            g_teleportPermissionProbeDispatching = false;
            g_teleportPermissionProbeSent = false;
            g_teleportPermissionProbeDetail =
                g_teleportPermissionProbeUntaggedFailure.empty()
                    ? "TP permission probe timed out without a matching command result"
                    : "TP command reported an untagged availability failure and no matching "
                      "command result arrived: " +
                      g_teleportPermissionProbeUntaggedFailure;
            g_teleportPermissionProbeCompleted = request;
            g_verifiedTeleportCredential.store(0, std::memory_order_release);
            g_verifiedTeleportDimensionToken.store(0, std::memory_order_release);
            g_verifiedTeleportAtNs.store(0, std::memory_order_release);
        }
        LOGE("teleport permission probe %llu timed out after %d ms",
             static_cast<unsigned long long>(request), timeout_ms);
    }
    const bool accepted = request == g_teleportPermissionProbeRequested &&
        g_teleportPermissionProbeCompleted >= request &&
        g_teleportPermissionProbeAccepted;
    const std::string result_detail = g_teleportPermissionProbeDetail;
    lock.unlock();
    if (detail) *detail = result_detail;
    return accepted;
}

bool ObserveTeleportPermissionProbePacket(const std::string& packet) {
    build_import::PyRpcAckEvent event;
    if (!build_import::decodePyRpcAckPacket(packet, &event)) return false;

    uint64_t request = 0;
    bool uuid_result_pending = false;
    bool untagged_failure_pending = false;
    {
        std::lock_guard<std::mutex> lock(g_teleportPermissionProbeMutex);
        uuid_result_pending =
            (g_teleportPermissionProbeDispatching || g_teleportPermissionProbeSent) &&
            g_teleportPermissionProbeCompleted < g_teleportPermissionProbeRequested;
        untagged_failure_pending = g_teleportPermissionProbeSent &&
            g_teleportPermissionProbeCompleted < g_teleportPermissionProbeRequested;
        request = g_teleportPermissionProbeRequested;
        if (event.kind == build_import::PyRpcAckEventKind::AfterExecuteCommand ||
            event.kind == build_import::PyRpcAckEventKind::ExecuteCommandOutput) {
            if (!build_import::isBuildExportTeleportProbeUuid(event.uuid) ||
                event.uuid != g_teleportPermissionProbeUuid) {
                return false;
            }
            if (event.kind == build_import::PyRpcAckEventKind::ExecuteCommandOutput) {
                return true;
            }
            if (!uuid_result_pending) return true;
            // The build-export probe intentionally trusts only an observed
            // position change.  Do not let a server-specific ACK shape turn a
            // successful or failed movement into a false decision.
            if (g_teleportPermissionProbePublishesTeleport) return true;
        } else if (event.kind == build_import::PyRpcAckEventKind::AvailableCheckFailed) {
            if (!untagged_failure_pending) return false;
            // This event has no UUID, so it cannot by itself prove which
            // command failed. Keep it only as timeout diagnostics; a matching
            // AfterExecuteCommandEvent remains authoritative.
            g_teleportPermissionProbeUntaggedFailure = event.reason.empty()
                ? "availability check failed without a reason" : event.reason;
            return false;
        } else {
            return false;
        }
    }

    if (event.kind == build_import::PyRpcAckEventKind::AfterExecuteCommand) {
        const bool accepted = event.execute_result;
        CompleteTeleportPermissionProbe(
            request, accepted,
            accepted ? "TP command succeeded; operator permission verified"
                     : "TP command was rejected; operator permission is unavailable");
        LOGI("teleport permission probe %llu completed accepted=%d uuid=%s",
             static_cast<unsigned long long>(request), accepted ? 1 : 0,
             event.uuid.c_str());
        return true;
    }

    return false;
}

bool HasVerifiedTeleportCommandPermission() {
    if (!IsNativeSessionAuthorized()) {
        InvalidateTeleportCommandPermission();
        return false;
    }
    const uint64_t credential =
        g_verifiedTeleportCredential.load(std::memory_order_acquire);
    if (credential == 0) return false;
    const uintptr_t verified =
        g_verifiedTeleportDimensionToken.load(std::memory_order_relaxed);
    const int64_t verified_at = g_verifiedTeleportAtNs.load(std::memory_order_relaxed);
    const int64_t now = monotonicNowNs();
    const bool current = verified != 0 && verified_at > 0 && now >= verified_at &&
        now - verified_at <= kTeleportPermissionValidityNs &&
        build_import::NativeWorldAccess::dimensionToken() == verified;
    if (!current) InvalidateTeleportCommandPermission();
    return current;
}

void InvalidateTeleportCommandPermission() noexcept {
    {
        std::lock_guard<std::mutex> lock(g_teleportPermissionProbeMutex);
        g_verifiedTeleportCredential.store(0, std::memory_order_release);
        g_verifiedTeleportDimensionToken.store(0, std::memory_order_relaxed);
        g_verifiedTeleportAtNs.store(0, std::memory_order_relaxed);
        if (g_teleportPermissionProbeCompleted < g_teleportPermissionProbeRequested) {
            g_teleportPermissionProbeAccepted = false;
            g_teleportPermissionProbeCompleted = g_teleportPermissionProbeRequested;
        }
        g_teleportPermissionProbeDispatching = false;
        g_teleportPermissionProbeSent = false;
        g_teleportPermissionProbeDetail.clear();
        g_teleportPermissionProbeUntaggedFailure.clear();
        g_teleportPermissionProbeDimensionToken = 0;
        g_teleportPermissionProbeDeadlineNs = 0;
        g_teleportPermissionProbePublishesTeleport = false;
        g_teleportPermissionProbeOriginValid = false;
    }
    g_teleportPermissionProbeCondition.notify_all();
}

// 提交 Python 代码
void SubmitPythonCode(const std::string& code) {
    if (!IsNativeOperationAuthorized()) return;
    std::lock_guard<std::mutex> lock(g_codeMutex);
    clearSensitiveString(g_pendingCode);
    g_pendingCode = code;
    g_hasPendingCode.store(true, std::memory_order_release);
    LOGI("★ Python 代码已提交 (%zu bytes)", code.size());
}

// Requests a real server TP and confirms it from the local actor's position.
// This runs on Actor::normalTick, so NativeWorldAccess is allowed to read the
// actor coordinate directly.  No native Actor::teleportTo is published here:
// doing so would make a denied server command look successful.
bool RequestTeleport(float x, float y, float z) {
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

    std::lock_guard<std::mutex> probe_lock(g_teleportPermissionProbeMutex);
    if (build_import::NativeWorldAccess::dimensionToken() != request_dimension) {
        return false;
    }

    const bool probe_pending =
        g_teleportPermissionProbeCompleted < g_teleportPermissionProbeRequested;
    const bool same_target =
        g_teleportPermissionProbeTarget.x == x &&
        g_teleportPermissionProbeTarget.y == y &&
        g_teleportPermissionProbeTarget.z == z;
    if (probe_pending) {
        if (g_teleportPermissionProbePublishesTeleport &&
            g_teleportPermissionProbeDimensionToken == request_dimension && same_target) {
            return true;
        }
        // Supersede a stale probe.  The previous server command cannot be
        // recalled, but its coordinate arrival is no longer accepted for this
        // batch and the next tick will send the new command.
        ++g_teleportPermissionProbeRequested;
        g_teleportPermissionProbeCompleted = g_teleportPermissionProbeRequested;
        g_teleportPermissionProbePublishesTeleport = false;
        g_teleportPermissionProbeAccepted = true;
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
    if (g_teleportPermissionProbePublishesTeleport &&
        !g_teleportPermissionProbeAccepted && same_target) {
        LOGE("teleport request rejected: the preceding coordinate verification failed (%s)",
             g_teleportPermissionProbeDetail.empty()
                 ? "local player did not reach the target"
                 : g_teleportPermissionProbeDetail.c_str());
        return false;
    }

    const uint64_t request = ++g_teleportPermissionProbeRequested;
    const int64_t now = monotonicNowNs();
    g_teleportPermissionProbeUuid =
        std::string(build_import::kBuildExportTeleportProbeUuidPrefix) +
        std::to_string(static_cast<uint64_t>(now)) + "_" + std::to_string(request);
    g_teleportPermissionProbeDispatching = false;
    g_teleportPermissionProbeSent = false;
    g_teleportPermissionProbeAccepted = false;
    g_teleportPermissionProbeDetail = "Sending TP to the export region centre";
    g_teleportPermissionProbeUntaggedFailure.clear();
    g_teleportPermissionProbeDimensionToken = request_dimension;
    g_teleportPermissionProbeDeadlineNs = now + 6LL * 1000LL * 1000LL * 1000LL;
    g_teleportPermissionProbePublishesTeleport = true;
    g_teleportPermissionProbeTarget = target;
    g_teleportPermissionProbeOriginX = player_x;
    g_teleportPermissionProbeOriginY = player_y;
    g_teleportPermissionProbeOriginZ = player_z;
    g_teleportPermissionProbeOriginValid = true;
    g_verifiedTeleportCredential.store(0, std::memory_order_release);
    g_verifiedTeleportDimensionToken.store(0, std::memory_order_relaxed);
    g_verifiedTeleportAtNs.store(0, std::memory_order_relaxed);
    LOGI("teleport movement probe %llu queued from (%d, %d, %d) to (%.1f, %.1f, %.1f)",
         static_cast<unsigned long long>(request), player_x, player_y, player_z, x, y, z);
    return true;
}

bool IsTeleportPermissionProbePending() {
    std::lock_guard<std::mutex> lock(g_teleportPermissionProbeMutex);
    return g_teleportPermissionProbePublishesTeleport &&
        g_teleportPermissionProbeCompleted < g_teleportPermissionProbeRequested;
}

void CancelPendingTeleportRequest() {
    // This also completes any in-flight movement probe, so a late successful
    // acknowledgement cannot republish a teleport after export cancellation.
    InvalidateTeleportCommandPermission();
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
    if (!IsNativeSessionAuthorized() || !output || timeout_ms <= 0 ||
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

bool QueryPlayerPermissionOnGameThread(int* output, int timeout_ms) {
    const bool session_authorized = IsNativeSessionAuthorized();
    const bool hook_installed = g_hookInstalled.load(std::memory_order_acquire);
    if (!session_authorized || !output || timeout_ms <= 0 || !hook_installed) {
        LOGE("player-permission query rejected before dispatch: session=%s output=%s "
             "timeout_ms=%d hook=%s",
             session_authorized ? "active" : "inactive",
             output ? "present" : "null", timeout_ms,
             hook_installed ? "installed" : "missing");
        return false;
    }
    *output = -1;

    std::thread::id game_thread_id;
    {
        std::lock_guard<std::mutex> lock(g_worldContextMutex);
        game_thread_id = g_gameThreadId;
    }

    std::unique_lock<std::mutex> lock(g_playerPermissionMutex);
    const uint64_t request = ++g_playerPermissionRequested;
    if (game_thread_id == std::this_thread::get_id()) {
        lock.unlock();
        ServicePlayerPermissionRequest();
        lock.lock();
    } else if (!g_playerPermissionCondition.wait_for(
                   lock, std::chrono::milliseconds(timeout_ms),
                   [&]() { return g_playerPermissionCompleted >= request; })) {
        LOGE("player-permission request %llu timed out after %d ms (local_player=%s)",
             static_cast<unsigned long long>(request), timeout_ms,
             g_localPlayer.load(std::memory_order_acquire) ? "captured" : "missing");
        return false;
    }
    if (g_playerPermissionCompleted < request || !g_cachedPlayerPermissionValid) {
        LOGE("player-permission request %llu completed without a valid level",
             static_cast<unsigned long long>(request));
        return false;
    }
    *output = g_cachedPlayerPermission;
    return true;
}
