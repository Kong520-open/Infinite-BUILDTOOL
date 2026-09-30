#include "NativeWorldAccess.h"
#include "BuildProjectionRenderer.h"

#include "../main.h"
#include "../tp/FunctionsAddress.h"
#include "../tp/LoopbackPacketSenderCapture.h"
#include "../tp/MinecraftUpdateHook.h"
#include "../tp/PythonUtils.h"

#include <chrono>
#include <array>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <link.h>
#include <limits>
#include <string_view>
#include <atomic>
#include <android/log.h>

namespace build_import {
namespace {

#define BUILD_EXPORT_LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO, "Infinitecz_BuildExport", __VA_ARGS__)

constexpr size_t kClientInstanceRegionVtableIndex = 35;
constexpr size_t kBlockSourceGetBlockVtableIndex = 2;
// BlockLegacy::getData(const Block&) in the matched client is the two
// instruction accessor `ldrh w0, [x1, #0x8a]; ret` (RVA 0xE433D54).  The
// old 0x3c probe read an unrelated member, which made equal modern block
// states look different (most visibly stair facings).  Keep this field next
// to the legacy pointer offset: mData ends at 0x8b and the pointer is aligned
// at 0x90.
constexpr uintptr_t kBlockAuxOffset = 0x8A;
constexpr uintptr_t kBlockLegacyOffset = 0x90;
constexpr uintptr_t kBlockLegacyRawNameOffset = 0xA0;
constexpr uintptr_t kBlockLegacyFullNameOffset = 0x130;
constexpr uintptr_t kBlockLegacyIdOffset = 0x244;

// Actor's raw position member is version-specific.  Do not guess an offset
// here: a plausible but wrong pointer can look readable and silently export
// the wrong region.  The client Python position component is version-stable
// and is queried on the local-player game thread instead.
// One tick of cache meant a Python evaluation on nearly every game tick. The
// consumers are a 16-block arrival radius, a 2 s staleness check and teleport
// arrival detection, so a few ticks of staleness is harmless while removing
// most of the interpreter round trips.
constexpr int64_t kPythonPositionCacheNs = 150LL * 1000LL * 1000LL;

struct PythonPositionCache {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int64_t refreshed_at_ns = 0;
    bool valid = false;
};

PythonPositionCache g_pythonPositionCache;

enum class WorldReaderDiagnostic : uint8_t {
    None = 0,
    LocalPlayerUnavailable,
    ClientInstanceUnavailable,
    ClientVtableUnavailable,
    RegionVirtualSlotUnavailable,
    RegionUnavailable,
    RegionVtableUnavailable,
    GetBlockVirtualSlotUnavailable,
    BlockUnavailable,
    BlockLayoutUnreadable,
    BlockLegacyUnavailable,
    LegacyLayoutUnreadable,
    LegacyNameUnavailable,
};

std::atomic<WorldReaderDiagnostic> g_worldReaderDiagnostic{WorldReaderDiagnostic::None};

// These are deliberately bounded.  The export reader runs many times per
// tick, so unrestricted diagnostics would itself make a large export stutter.
// The markers remain long enough to identify which legacy ABI call faults
// immediately after a teleport reaches the target region.
std::atomic<uint32_t> g_worldReaderAbiLogBudget{0};
std::atomic<uint32_t> g_containerSnapshotLogBudget{0};

constexpr uint32_t kWorldReaderAbiLogBudget = 48;
constexpr uint32_t kContainerSnapshotLogBudget = 16;

bool shouldLogWorldReaderAbi() {
    // Called several times per scanned block. Once the budget is spent this
    // must not keep issuing atomic read-modify-writes forever.
    if (g_worldReaderAbiLogBudget.load(std::memory_order_relaxed) >=
        kWorldReaderAbiLogBudget) {
        return false;
    }
    return g_worldReaderAbiLogBudget.fetch_add(1, std::memory_order_relaxed) <
        kWorldReaderAbiLogBudget;
}

bool shouldLogContainerSnapshot() {
    if (g_containerSnapshotLogBudget.load(std::memory_order_relaxed) >=
        kContainerSnapshotLogBudget) {
        return false;
    }
    return g_containerSnapshotLogBudget.fetch_add(1, std::memory_order_relaxed) <
        kContainerSnapshotLogBudget;
}

unsigned long long relativeToMinecraft(uintptr_t address) {
    const uintptr_t base_address = Main::getBaseAddress();
    return (base_address && address >= base_address)
        ? static_cast<unsigned long long>(address - base_address) : 0ULL;
}

void setWorldReaderDiagnostic(WorldReaderDiagnostic value) {
    g_worldReaderDiagnostic.store(value, std::memory_order_release);
}

int64_t monotonicNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct ExecutableAddressQuery {
    uintptr_t module_base = 0;
    uintptr_t address = 0;
    bool module_found = false;
    bool executable = false;
};

bool checkedAdd(uintptr_t base, uintptr_t offset, uintptr_t* result) {
    if (!result || base > std::numeric_limits<uintptr_t>::max() - offset) return false;
    *result = base + offset;
    return true;
}

bool checkedAdd(int64_t base, int64_t offset, int64_t* result) {
    if (!result || (offset > 0 && base > std::numeric_limits<int64_t>::max() - offset) ||
        (offset < 0 && base < std::numeric_limits<int64_t>::min() - offset)) {
        return false;
    }
    *result = base + offset;
    return true;
}

bool checkedInt32(int64_t value, int32_t* result) {
    if (!result || value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    *result = static_cast<int32_t>(value);
    return true;
}

bool parsePythonBlockPosition(const std::string& value, int32_t* x, int32_t* y, int32_t* z) {
    if (!x || !y || !z) return false;
    double raw_x = 0.0;
    double raw_y = 0.0;
    double raw_z = 0.0;
    char extra = '\0';
    if (std::sscanf(value.c_str(), "%lf,%lf,%lf%c", &raw_x, &raw_y, &raw_z, &extra) != 3 ||
        !std::isfinite(raw_x) || !std::isfinite(raw_y) || !std::isfinite(raw_z)) {
        return false;
    }
    const double floor_x = std::floor(raw_x);
    const double floor_y = std::floor(raw_y);
    const double floor_z = std::floor(raw_z);
    if (floor_x < std::numeric_limits<int32_t>::min() ||
        floor_x > std::numeric_limits<int32_t>::max() ||
        floor_y < std::numeric_limits<int32_t>::min() ||
        floor_y > std::numeric_limits<int32_t>::max() ||
        floor_z < std::numeric_limits<int32_t>::min() ||
        floor_z > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    *x = static_cast<int32_t>(floor_x);
    *y = static_cast<int32_t>(floor_y);
    *z = static_cast<int32_t>(floor_z);
    return true;
}

bool decodeHexText(std::string_view value, std::string* output) {
    if (!output || (value.size() & 1U) != 0U || value.size() > 2U * 1024U * 1024U) {
        return false;
    }
    const auto nibble = [](char character, uint8_t* result) {
        if (!result) return false;
        if (character >= '0' && character <= '9') *result = static_cast<uint8_t>(character - '0');
        else if (character >= 'a' && character <= 'f') *result = static_cast<uint8_t>(character - 'a' + 10);
        else if (character >= 'A' && character <= 'F') *result = static_cast<uint8_t>(character - 'A' + 10);
        else return false;
        return true;
    };
    try {
        output->clear();
        output->reserve(value.size() / 2U);
        for (size_t index = 0; index < value.size(); index += 2U) {
            uint8_t high = 0;
            uint8_t low = 0;
            if (!nibble(value[index], &high) || !nibble(value[index + 1U], &low)) return false;
            output->push_back(static_cast<char>((high << 4U) | low));
        }
    } catch (const std::bad_alloc&) {
        output->clear();
        return false;
    }
    return true;
}

bool parsePythonInteger(std::string_view value, int32_t fallback, int32_t* output) {
    if (!output || value.empty() || value.size() > 32U) {
        if (output) *output = fallback;
        return false;
    }
    std::string copy(value);
    char* end = nullptr;
    const long parsed = std::strtol(copy.c_str(), &end, 10);
    if (end == copy.c_str() || *end != '\0' || parsed < std::numeric_limits<int32_t>::min() ||
        parsed > std::numeric_limits<int32_t>::max()) {
        *output = fallback;
        return false;
    }
    *output = static_cast<int32_t>(parsed);
    return true;
}

bool parsePythonBool(std::string_view value, bool fallback, bool* output) {
    int32_t parsed = fallback ? 1 : 0;
    const bool valid = parsePythonInteger(value, parsed, &parsed);
    if (output) *output = valid ? parsed != 0 : fallback;
    return valid;
}

bool parsePythonCommandBlockData(const std::string& value,
                                 NativeCommandBlockData* output) {
    if (!output) return false;
    *output = NativeCommandBlockData{};
    size_t start = 0;
    std::array<std::string_view, 11U> fields{};
    for (size_t index = 0; index < fields.size(); ++index) {
        const size_t end = value.find('|', start);
        if (end == std::string::npos) {
            if (index + 1U != fields.size()) return false;
            fields[index] = std::string_view(value).substr(start);
            start = value.size();
            break;
        }
        fields[index] = std::string_view(value).substr(start, end - start);
        start = end + 1U;
    }
    if (start != value.size() || (fields[0] != "0" && fields[0] != "1")) return false;
    const bool entity_available = fields[0] == "1";
    if (!decodeHexText(fields[1], &output->command) ||
        !decodeHexText(fields[2], &output->name) ||
        !decodeHexText(fields[3], &output->last_output)) {
        return false;
    }
    int32_t parsed = 0;
    if (!parsePythonInteger(fields[4], 0, &parsed) || parsed < 0 || parsed > 2) return false;
    output->mode = static_cast<uint16_t>(parsed);
    if (!parsePythonBool(fields[5], true, &output->redstone_mode) ||
        !parsePythonBool(fields[6], false, &output->conditional) ||
        !parsePythonBool(fields[7], false, &output->output_tracked) ||
        !parsePythonInteger(fields[8], 0, &output->tick_delay) ||
        !parsePythonBool(fields[9], false, &output->executing_on_first_tick)) {
        return false;
    }
    int32_t shell_aux = -1;
    if (!parsePythonInteger(fields[10], -1, &shell_aux) || shell_aux < -1 ||
        shell_aux > std::numeric_limits<uint16_t>::max()) {
        return false;
    }
    if (shell_aux >= 0) {
        output->shell_aux_available = true;
        output->shell_aux = static_cast<uint16_t>(shell_aux);
    }
    output->available = entity_available;
    return true;
}

bool parsePythonBlockSnapshot(const std::string& value,
                              NativeBlockSnapshot* output) {
    if (!output) return false;
    *output = NativeBlockSnapshot{};
    std::array<std::string_view, 4U> fields{};
    size_t start = 0;
    for (size_t index = 0; index < fields.size(); ++index) {
        const size_t separator = value.find('|', start);
        if (separator == std::string::npos) {
            if (index + 1U != fields.size()) return false;
            fields[index] = std::string_view(value).substr(start);
            start = value.size();
            break;
        }
        fields[index] = std::string_view(value).substr(start, separator - start);
        start = separator + 1U;
    }
    if (start != value.size()) {
        return false;
    }
    if (!fields[0].empty()) {
        if (!decodeHexText(fields[0], &output->client_identifier)) return false;
        output->client_block_available = !output->client_identifier.empty();
    }
    int32_t parsed_aux = -1;
    if (!fields[1].empty() && !parsePythonInteger(fields[1], -1, &parsed_aux)) return false;
    if (parsed_aux >= 0 && parsed_aux <= std::numeric_limits<uint16_t>::max()) {
        output->client_aux_available = true;
        output->client_aux = static_cast<uint16_t>(parsed_aux);
    } else if (parsed_aux != -1) {
        return false;
    }
    if (!fields[2].empty()) {
        if (!decodeHexText(fields[2], &output->state_json)) return false;
        output->state_available = !output->state_json.empty();
    }
    if (!fields[3].empty()) {
        if (!decodeHexText(fields[3], &output->entity_json)) return false;
        output->entity_available = !output->entity_json.empty();
    }
    return true;
}

bool queryPythonBlockSnapshot(int32_t x, int32_t y, int32_t z,
                              NativeBlockSnapshot* output,
                              bool include_container_items,
                              int32_t dimension_id) {
    if (!output) return false;
    const bool log_container_snapshot =
        include_container_items && shouldLogContainerSnapshot();
    if (log_container_snapshot) {
        BUILD_EXPORT_LOGI(
            "[container-snapshot] begin pos=(%d,%d,%d) dimension=%d source=client-block-entity",
            x, y, z, dimension_id);
    }
    std::string value;
    // The public client SDK only promises GetBlock(name, aux) and
    // GetBlockEntityData(). A few NetEase builds expose BlockState through
    // the same client factory, so probe it defensively and retain the result
    // when present. Do not import or invoke server components from this
    // client interpreter: their native bindings require a server-system
    // context and can fault before Python can raise an exception.
    //
    // GetBlockEntityData() is still retained for container candidates. The
    // public SDK does not promise inventory contents, but a compatible client
    // implementation may expose container methods on its client Item
    // component or include Items in the returned entity payload. The normal
    // container codec will preserve the four supported fields from either.
    // All conversion stays in Python so wrapper objects and Python-2
    // long/bytes values do not require private CPython symbols here.
    const std::string statements =
        "import binascii, json\n"
        "import mod.client.extraClientApi as _infinitecz_snapshot_api\n"
        "_infinitecz_snapshot_pos = (" + std::to_string(x) + "," +
            std::to_string(y) + "," + std::to_string(z) + ")\n"
        "_infinitecz_snapshot_include_container = " +
            std::string(include_container_items ? "True" : "False") + "\n"
        "_infinitecz_snapshot_dimension = " + std::to_string(dimension_id) + "\n"
        "_infinitecz_snapshot_block = None\n"
        "_infinitecz_snapshot_state = None\n"
        "_infinitecz_snapshot_entity = None\n"
        "try:\n"
        "  _infinitecz_snapshot_factory = _infinitecz_snapshot_api.GetEngineCompFactory()\n"
        "  _infinitecz_snapshot_info = _infinitecz_snapshot_factory.CreateBlockInfo(_infinitecz_snapshot_api.GetLevelId())\n"
        "  if hasattr(_infinitecz_snapshot_info, 'GetBlock'):\n"
        "    _infinitecz_snapshot_block = _infinitecz_snapshot_info.GetBlock(_infinitecz_snapshot_pos)\n"
        "  if hasattr(_infinitecz_snapshot_info, 'GetBlockEntityData'):\n"
        "    _infinitecz_snapshot_entity = _infinitecz_snapshot_info.GetBlockEntityData(_infinitecz_snapshot_pos)\n"
        "  if hasattr(_infinitecz_snapshot_info, 'GetBlockNew'):\n"
        "    _infinitecz_snapshot_state = _infinitecz_snapshot_info.GetBlockNew(_infinitecz_snapshot_pos)\n"
        "  if _infinitecz_snapshot_state is None and hasattr(_infinitecz_snapshot_factory, 'CreateBlockState'):\n"
        "    _infinitecz_snapshot_state_comp = _infinitecz_snapshot_factory.CreateBlockState(_infinitecz_snapshot_api.GetLevelId())\n"
        "    if hasattr(_infinitecz_snapshot_state_comp, 'GetBlockStates'):\n"
        "      _infinitecz_snapshot_state = _infinitecz_snapshot_state_comp.GetBlockStates(_infinitecz_snapshot_pos)\n"
        "except BaseException:\n"
        "  pass\n"
        "try:\n"
        "  _infinitecz_snapshot_unicode = unicode\n"
        "except NameError:\n"
        "  _infinitecz_snapshot_unicode = str\n"
        "try:\n"
        "  _infinitecz_snapshot_long = long\n"
        "except NameError:\n"
        "  _infinitecz_snapshot_long = int\n"
        "def _infinitecz_snapshot_unwrap(_value):\n"
        "  try:\n"
        "    for _depth in range(16):\n"
        "      if isinstance(_value, dict) and '__value__' in _value:\n"
        "        _next = _value['__value__']\n"
        "        if _next is _value: break\n"
        "        _value = _next\n"
        "        continue\n"
        "      break\n"
        "  except BaseException:\n"
        "    pass\n"
        "  return _value\n"
        "def _infinitecz_snapshot_safe(_value, _depth=0):\n"
        "  if _depth > 64: return {'__type__':'depth'}\n"
        "  _value = _infinitecz_snapshot_unwrap(_value)\n"
        "  try:\n"
        "    if _value is None or isinstance(_value, bool): return _value\n"
        "    if isinstance(_value, (_infinitecz_snapshot_unicode,)):\n"
        "      return _value\n"
        "    if isinstance(_value, bytes) and not isinstance(_value, _infinitecz_snapshot_unicode):\n"
        "      return {'__type__':'bytes','hex':binascii.hexlify(_value).decode('ascii')}\n"
        "    if isinstance(_value, (_infinitecz_snapshot_long, int, float)):\n"
        "      return _value\n"
        "    if isinstance(_value, dict):\n"
        "      _items = {}\n"
        "      for _key, _item in _value.items():\n"
        "        _items[str(_key)] = _infinitecz_snapshot_safe(_item, _depth + 1)\n"
        "      return _items\n"
        "    if isinstance(_value, (list, tuple, set)):\n"
        "      return [_infinitecz_snapshot_safe(_item, _depth + 1) for _item in _value]\n"
        "    if hasattr(_value, '__dict__'):\n"
        "      return _infinitecz_snapshot_safe(vars(_value), _depth + 1)\n"
        "  except BaseException:\n"
        "    pass\n"
        "  try: return {'__type__':'repr','value':str(_value)}\n"
        "  except BaseException: return {'__type__':'unavailable'}\n"
        "def _infinitecz_snapshot_client_inventory(_component):\n"
        "  if _component is None or not hasattr(_component, 'GetContainerSize') or not hasattr(_component, 'GetContainerItem'): return None\n"
        "  try:\n"
        "    _size = _component.GetContainerSize(_infinitecz_snapshot_pos, _infinitecz_snapshot_dimension)\n"
        "    if not isinstance(_size, (_infinitecz_snapshot_long, int)) or _size < 0 or _size > 256: return None\n"
        "    _items = []\n"
        "    for _slot in range(int(_size)):\n"
        "      try:\n"
        "        _item = _component.GetContainerItem(_infinitecz_snapshot_pos, _slot, _infinitecz_snapshot_dimension, False)\n"
        "      except TypeError:\n"
        "        _item = _component.GetContainerItem(_infinitecz_snapshot_pos, _slot, _infinitecz_snapshot_dimension)\n"
        "      _item = _infinitecz_snapshot_unwrap(_item)\n"
        "      if isinstance(_item, dict):\n"
        "        _entry = dict(_item)\n"
        "        _entry['Slot'] = _slot\n"
        "        _items.append(_entry)\n"
        "    return _items\n"
        "  except BaseException:\n"
        "    return None\n"
        "if _infinitecz_snapshot_include_container:\n"
        "  _container_items = None\n"
        "  try:\n"
        "    _client_player = _infinitecz_snapshot_api.GetLocalPlayerId()\n"
        "    _client_item = _infinitecz_snapshot_factory.CreateItem(_client_player)\n"
        "    _container_items = _infinitecz_snapshot_client_inventory(_client_item)\n"
        "  except BaseException:\n"
        "    pass\n"
        "  if _container_items is not None:\n"
        "    _infinitecz_snapshot_entity = {'Items': _container_items}\n"
        "def _infinitecz_snapshot_dump(_value):\n"
        "  if _value is None: return ''\n"
        "  try:\n"
        "    _raw = json.dumps(_infinitecz_snapshot_safe(_value), sort_keys=True, separators=(',',':'), ensure_ascii=False)\n"
        "    if not isinstance(_raw, bytes): _raw = _raw.encode('utf-8')\n"
        "    return binascii.hexlify(_raw).decode('ascii')\n"
        "  except BaseException:\n"
        "    return ''\n"
        "def _infinitecz_snapshot_text(_value):\n"
        "  if _value is None: return ''\n"
        "  _value = _infinitecz_snapshot_unwrap(_value)\n"
        "  try:\n"
        "    if isinstance(_value, _infinitecz_snapshot_unicode):\n"
        "      _raw = _value.encode('utf-8')\n"
        "    elif isinstance(_value, bytes):\n"
        "      _raw = _value\n"
        "    else:\n"
        "      _raw = str(_value)\n"
        "      if not isinstance(_raw, bytes): _raw = _raw.encode('utf-8')\n"
        "    return binascii.hexlify(_raw).decode('ascii')\n"
        "  except BaseException:\n"
        "    return ''\n";
    const std::string expression =
        "'%s|%d|%s|%s' % ("
        "_infinitecz_snapshot_text(_infinitecz_snapshot_block[0] if _infinitecz_snapshot_block is not None and len(_infinitecz_snapshot_block) > 0 else None), "
        "int(_infinitecz_snapshot_block[1]) if _infinitecz_snapshot_block is not None and len(_infinitecz_snapshot_block) > 1 else -1, "
        "_infinitecz_snapshot_dump(_infinitecz_snapshot_state), "
        "_infinitecz_snapshot_dump(_infinitecz_snapshot_entity))";
    const bool evaluated = PythonUtils::PyEvalUtf8(statements, expression, &value);
    const bool parsed = evaluated && parsePythonBlockSnapshot(value, output);
    if (log_container_snapshot) {
        BUILD_EXPORT_LOGI(
            "[container-snapshot] end pos=(%d,%d,%d) evaluated=%d parsed=%d entity=%d bytes=%zu",
            x, y, z, evaluated ? 1 : 0, parsed ? 1 : 0,
            output->entity_available ? 1 : 0, output->entity_json.size());
    }
    return parsed;
}

bool queryPythonCommandBlockData(int32_t x, int32_t y, int32_t z,
                                 NativeCommandBlockData* output) {
    if (!output) return false;
    std::string value;
    const std::string statements =
        "import binascii\n"
        "import mod.client.extraClientApi as _infinitecz_entity_api\n"
        "_infinitecz_entity_data = None\n"
        "_infinitecz_entity_block = None\n"
        "try:\n"
        "  _infinitecz_entity_comp = _infinitecz_entity_api.GetEngineCompFactory().CreateBlockInfo(_infinitecz_entity_api.GetLevelId())\n"
        "  _infinitecz_entity_pos = (" + std::to_string(x) + "," +
        std::to_string(y) + "," + std::to_string(z) + ")\n"
        "  if hasattr(_infinitecz_entity_comp, 'GetBlockEntityData'):\n"
        "    _infinitecz_entity_data = _infinitecz_entity_comp.GetBlockEntityData(_infinitecz_entity_pos)\n"
        "  if hasattr(_infinitecz_entity_comp, 'GetBlock'):\n"
        "    _infinitecz_entity_block = _infinitecz_entity_comp.GetBlock(_infinitecz_entity_pos)\n"
        "except BaseException:\n"
        "  _infinitecz_entity_data = None\n"
        "  _infinitecz_entity_block = None\n"
        "def _infinitecz_entity_unwrap(_value):\n"
        "  try:\n"
        "    for _depth in range(8):\n"
        "      if isinstance(_value, dict) and '__value__' in _value:\n"
        "        _next = _value['__value__']\n"
        "        if _next is _value: break\n"
        "        _value = _next\n"
        "        continue\n"
        "      break\n"
        "  except BaseException:\n"
        "    pass\n"
        "  return _value\n"
        "def _infinitecz_entity_pick(_keys, _default=None):\n"
        "  _data = _infinitecz_entity_unwrap(_infinitecz_entity_data)\n"
        "  for _key in _keys:\n"
        "    try:\n"
        "      if isinstance(_data, dict) and _key in _data:\n"
        "        return _infinitecz_entity_unwrap(_data[_key])\n"
        "      if _data is not None and hasattr(_data, _key):\n"
        "        return _infinitecz_entity_unwrap(getattr(_data, _key))\n"
        "    except BaseException:\n"
        "      pass\n"
        "  return _default\n"
        "try:\n"
        "  _infinitecz_entity_unicode = unicode\n"
        "except NameError:\n"
        "  _infinitecz_entity_unicode = str\n"
        "def _infinitecz_entity_hex(_value):\n"
        "  try:\n"
        "    _value = _infinitecz_entity_unwrap(_value)\n"
        "    if _value is None: return ''\n"
        "    if isinstance(_value, _infinitecz_entity_unicode):\n"
        "      _raw = _value.encode('utf-8')\n"
        "    elif isinstance(_value, bytes):\n"
        "      _raw = _value\n"
        "    else:\n"
        "      _raw = str(_value)\n"
        "    return binascii.hexlify(_raw).decode('ascii')\n"
        "  except BaseException:\n"
        "    return ''\n"
        "def _infinitecz_entity_int(_keys, _default):\n"
        "  try:\n"
        "    _value = _infinitecz_entity_pick(_keys, _default)\n"
        "    if isinstance(_value, (str, _infinitecz_entity_unicode)):\n"
        "      _lower = _value.lower()\n"
        "      if _lower in ('impulse', 'normal'): return 0\n"
        "      if _lower in ('repeat', 'repeating'): return 1\n"
        "      if _lower in ('chain',): return 2\n"
        "    return int(_value)\n"
        "  except BaseException:\n"
        "    return _default\n"
        "def _infinitecz_entity_bool(_keys, _default):\n"
        "  try:\n"
        "    _value = _infinitecz_entity_pick(_keys, _default)\n"
        "    if isinstance(_value, bool): return 1 if _value else 0\n"
        "    if isinstance(_value, (str, _infinitecz_entity_unicode)):\n"
        "      return 1 if _value.lower() in ('1', 'true', 'yes') else 0\n"
        "    return 1 if int(_value) != 0 else 0\n"
        "  except BaseException:\n"
        "    return 1 if _default else 0\n"
        "def _infinitecz_entity_redstone():\n"
        "  _infinitecz_entity_missing = object()\n"
        "  _infinitecz_entity_value = _infinitecz_entity_pick(('needsRedstone', 'NeedsRedstone', 'redstoneMode'), _infinitecz_entity_missing)\n"
        "  if _infinitecz_entity_value is not _infinitecz_entity_missing:\n"
        "    return _infinitecz_entity_bool(('needsRedstone', 'NeedsRedstone', 'redstoneMode'), 1)\n"
        "  _infinitecz_entity_value = _infinitecz_entity_pick(('auto',), _infinitecz_entity_missing)\n"
        "  if _infinitecz_entity_value is _infinitecz_entity_missing:\n"
        "    return 1\n"
        "  try:\n"
        "    if isinstance(_infinitecz_entity_value, (str, _infinitecz_entity_unicode)):\n"
        "      return 0 if _infinitecz_entity_value.lower() in ('1', 'true', 'yes') else 1\n"
        "    return 0 if int(_infinitecz_entity_value) != 0 else 1\n"
        "  except BaseException:\n"
        "    return 1\n"
        "def _infinitecz_entity_block_aux():\n"
        "  try:\n"
        "    if _infinitecz_entity_block is None or len(_infinitecz_entity_block) < 2:\n"
        "      return -1\n"
        "    return int(_infinitecz_entity_unwrap(_infinitecz_entity_block[1]))\n"
        "  except BaseException:\n"
        "    return -1\n";
    const std::string expression =
        "('%d|%s|%s|%s|%d|%d|%d|%d|%d|%d|%d' % ("
        "1 if _infinitecz_entity_data is not None else 0, "
        "_infinitecz_entity_hex(_infinitecz_entity_pick(('Command', 'command', 'cmd'), '')), "
        "_infinitecz_entity_hex(_infinitecz_entity_pick(('CustomName', 'customName', 'Name', 'name'), '')), "
        "_infinitecz_entity_hex(_infinitecz_entity_pick(('LastOutput', 'lastOutput'), '')), "
        "_infinitecz_entity_int(('CommandBlockMode', 'commandBlockMode', 'LPCommandMode', 'mode'), 0), "
        "_infinitecz_entity_redstone(), "
        "_infinitecz_entity_bool(('conditionalMode', 'ConditionalMode', 'conditional'), 0), "
        "_infinitecz_entity_bool(('TrackOutput', 'trackOutput', 'outputTracked'), 0), "
        "_infinitecz_entity_int(('TickDelay', 'tickDelay'), 0), "
        "_infinitecz_entity_bool(('ExecuteOnFirstTick', 'executeOnFirstTick'), 0), "
        "_infinitecz_entity_block_aux()))";
    return PythonUtils::PyEvalUtf8(statements, expression, &value) &&
        parsePythonCommandBlockData(value, output);
}

bool queryPythonBlockPosition(int32_t* x, int32_t* y, int32_t* z) {
    std::string value;
    static const std::string kStatements =
        "import mod.client.extraClientApi as _infinitecz_position_api\n"
        "_infinitecz_position_player = _infinitecz_position_api.GetLocalPlayerId()\n"
        "if _infinitecz_position_player is None or str(_infinitecz_position_player) in ('', '-1'):\n"
        "  raise RuntimeError('local player is unavailable')\n"
        "_infinitecz_position_value = _infinitecz_position_api.GetEngineCompFactory().CreatePos("
        "_infinitecz_position_player).GetPos()\n"
        "if _infinitecz_position_value is None or len(_infinitecz_position_value) < 3:\n"
        "  raise RuntimeError('local player position is unavailable')\n";
    static const std::string kExpression =
        "'%.6f,%.6f,%.6f' % (float(_infinitecz_position_value[0]), "
        "float(_infinitecz_position_value[1]), float(_infinitecz_position_value[2]))";
    return PythonUtils::PyEvalUtf8(kStatements, kExpression, &value) &&
        parsePythonBlockPosition(value, x, y, z);
}

bool queryRenderCameraBlockPosition(int32_t* x, int32_t* y, int32_t* z) {
    if (!x || !y || !z) return false;
    float camera_x = 0.0f;
    float camera_y = 0.0f;
    float camera_z = 0.0f;
    if (!GetLatestBuildRenderCameraPosition(&camera_x, &camera_y, &camera_z)) return false;
    const double block_x = std::floor(static_cast<double>(camera_x));
    const double block_y = std::floor(static_cast<double>(camera_y));
    const double block_z = std::floor(static_cast<double>(camera_z));
    if (block_x < std::numeric_limits<int32_t>::min() ||
        block_x > std::numeric_limits<int32_t>::max() ||
        block_y < std::numeric_limits<int32_t>::min() ||
        block_y > std::numeric_limits<int32_t>::max() ||
        block_z < std::numeric_limits<int32_t>::min() ||
        block_z > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    *x = static_cast<int32_t>(block_x);
    *y = static_cast<int32_t>(block_y);
    *z = static_cast<int32_t>(block_z);
    return true;
}

bool isMinecraftModulePath(const char* path) {
    if (!path) return false;
    const char* file_name = std::strrchr(path, '/');
    file_name = file_name ? file_name + 1 : path;
    return std::strcmp(file_name, "libminecraftpe.so") == 0 ||
           std::strcmp(file_name, "libminecraftpe.so (deleted)") == 0;
}

int findExecutableAddress(dl_phdr_info* info, size_t, void* opaque) {
    auto* query = static_cast<ExecutableAddressQuery*>(opaque);
    if (!query || static_cast<uintptr_t>(info->dlpi_addr) != query->module_base) return 0;

    const char* path = info->dlpi_name;
    if (!isMinecraftModulePath(path)) return 0;
    query->module_found = true;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) == 0) continue;

        uintptr_t start = 0;
        uintptr_t end = 0;
        if (!checkedAdd(query->module_base, static_cast<uintptr_t>(header.p_vaddr), &start) ||
            !checkedAdd(start, static_cast<uintptr_t>(header.p_memsz), &end)) {
            continue;
        }
        uintptr_t address_end = 0;
        if (checkedAdd(query->address, sizeof(uint32_t), &address_end) &&
            query->address >= start && address_end <= end) {
            query->executable = true;
            return 1;
        }
    }
    return 1;
}

bool knownAbiProfile() {
#if defined(__aarch64__)
    return sizeof(void*) == 8 && kClientInstanceRegionVtableIndex == 35 &&
        kBlockSourceGetBlockVtableIndex == 2 && kBlockAuxOffset == 0x8A &&
        kBlockLegacyOffset == 0x90 && kBlockLegacyRawNameOffset == 0xA0 &&
        kBlockLegacyFullNameOffset == 0x130 && kBlockLegacyIdOffset == 0x244;
#else
    return false;
#endif
}

bool isMinecraftExecutableAddress(uintptr_t module_base, uintptr_t address) {
    if (!knownAbiProfile() || !module_base || !address || (address & 0x3U) != 0) {
        return false;
    }
    ExecutableAddressQuery query{module_base, address};
    dl_iterate_phdr(findExecutableAddress, &query);
    return query.module_found && query.executable;
}

template <typename Function>
bool resolveProfileFunction(uintptr_t offset, Function* output) {
    if (!output) return false;
    *output = nullptr;
    const uintptr_t base_address = Main::getBaseAddress();
    uintptr_t address = 0;
    if (!offset || !checkedAdd(base_address, offset, &address) ||
        !isMinecraftExecutableAddress(base_address, address)) {
        return false;
    }
    *output = reinterpret_cast<Function>(address);
    return true;
}

bool readPointerAt(const void* object, uintptr_t offset, void** output) {
    if (!object || !output) return false;
    *output = nullptr;
    uintptr_t address = 0;
    if (!checkedAdd(reinterpret_cast<uintptr_t>(object), offset, &address) ||
        !IsMemoryReadable(reinterpret_cast<const void*>(address), sizeof(void*))) {
        return false;
    }
    std::memcpy(output, reinterpret_cast<const void*>(address), sizeof(*output));
    return true;
}

void* getClientInstance(void* actor) {
    if (!actor || !IsMemoryReadable(actor, sizeof(void*))) return nullptr;
    using Function = void* (*)(void*);
    Function function = nullptr;
    return resolveProfileFunction(FunctionsAddress::Actor_getClientInstance, &function)
        ? function(actor) : nullptr;
}

void* getRegion(void* actor) {
    const uintptr_t base_address = Main::getBaseAddress();
    void* client = getClientInstance(actor);
    void* raw_vtable = nullptr;
    if (!client) {
        setWorldReaderDiagnostic(WorldReaderDiagnostic::ClientInstanceUnavailable);
        return nullptr;
    }
    if (!readPointerAt(client, 0, &raw_vtable) || !raw_vtable) {
        setWorldReaderDiagnostic(WorldReaderDiagnostic::ClientVtableUnavailable);
        return nullptr;
    }
    void* raw_function = nullptr;
    if (!readPointerAt(raw_vtable, kClientInstanceRegionVtableIndex * sizeof(void*),
                        &raw_function) || !raw_function ||
        !isMinecraftExecutableAddress(base_address,
                                       reinterpret_cast<uintptr_t>(raw_function))) {
        setWorldReaderDiagnostic(WorldReaderDiagnostic::RegionVirtualSlotUnavailable);
        return nullptr;
    }

    if (shouldLogWorldReaderAbi()) {
        BUILD_EXPORT_LOGI(
            "world ABI: getRegion invoke actor=%p client=%p vtable=%p slot=%zu target=%p rva=0x%llx",
            actor, client, raw_vtable, kClientInstanceRegionVtableIndex, raw_function,
            relativeToMinecraft(reinterpret_cast<uintptr_t>(raw_function)));
    }
    using Function = void* (*)(void*);
    void* region = reinterpret_cast<Function>(raw_function)(client);
    if (shouldLogWorldReaderAbi()) {
        BUILD_EXPORT_LOGI("world ABI: getRegion returned region=%p", region);
    }
    if (!region) setWorldReaderDiagnostic(WorldReaderDiagnostic::RegionUnavailable);
    return region;
}

bool resolveGetBlockFunction(void* region, void** output) {
    if (!output) return false;
    *output = nullptr;
    const uintptr_t base_address = Main::getBaseAddress();
    void* raw_vtable = nullptr;
    if (!region || !readPointerAt(region, 0, &raw_vtable) || !raw_vtable) {
        setWorldReaderDiagnostic(WorldReaderDiagnostic::RegionVtableUnavailable);
        return false;
    }
    void* raw_function = nullptr;
    if (!readPointerAt(raw_vtable, kBlockSourceGetBlockVtableIndex * sizeof(void*),
                        &raw_function) || !raw_function ||
        !isMinecraftExecutableAddress(base_address,
                                       reinterpret_cast<uintptr_t>(raw_function))) {
        setWorldReaderDiagnostic(WorldReaderDiagnostic::GetBlockVirtualSlotUnavailable);
        return false;
    }
    if (shouldLogWorldReaderAbi()) {
        BUILD_EXPORT_LOGI(
            "world ABI: getBlock resolved region=%p vtable=%p slot=%zu target=%p rva=0x%llx",
            region, raw_vtable, kBlockSourceGetBlockVtableIndex, raw_function,
            relativeToMinecraft(reinterpret_cast<uintptr_t>(raw_function)));
    }
    *output = raw_function;
    return true;
}

std::string namespaced(std::string value) {
    if (value.empty()) return value;
    if (value.find(':') == std::string::npos) value.insert(0, "minecraft:");
    return value;
}

}  // namespace

bool NativeWorldReader::open() {
    void* actor = GetLocalPlayerPointer();
    if (!actor) {
        setWorldReaderDiagnostic(WorldReaderDiagnostic::LocalPlayerUnavailable);
        reset();
        return false;
    }
    if (shouldLogWorldReaderAbi()) {
        BUILD_EXPORT_LOGI("world ABI: reader open actor=%p", actor);
    }
    void* region = getRegion(actor);
    void* function = nullptr;
    if (!region || !resolveGetBlockFunction(region, &function)) {
        reset();
        return false;
    }
    const auto get_block = reinterpret_cast<GetBlockFunction>(function);
    if (region_ != region || get_block_ != get_block) {
        legacy_cache_.clear();
        ++source_generation_;
    }
    region_ = region;
    get_block_ = get_block;
    setWorldReaderDiagnostic(WorldReaderDiagnostic::None);
    return true;
}

void NativeWorldReader::reset() {
    if (region_ || get_block_ || !legacy_cache_.empty()) {
        ++source_generation_;
    }
    region_ = nullptr;
    get_block_ = nullptr;
    legacy_cache_.clear();
    validated_blocks_.clear();
}

bool NativeWorldReader::getBlockView(int32_t x, int32_t y, int32_t z,
                                     NativeBlockView* output) {
    if (!output) return false;
    *output = NativeBlockView{};
    if (!region_ || !get_block_) return false;

    const NativeBlockPos position{x, y, z};
    if (shouldLogWorldReaderAbi()) {
        BUILD_EXPORT_LOGI(
            "world ABI: getBlock(BlockPos*) invoke region=%p target=%p rva=0x%llx pos=(%d,%d,%d)",
            region_, reinterpret_cast<void*>(get_block_),
            relativeToMinecraft(reinterpret_cast<uintptr_t>(get_block_)),
            position.x, position.y, position.z);
    }
    void* block = get_block_(region_, &position);
    if (shouldLogWorldReaderAbi()) {
        BUILD_EXPORT_LOGI("world ABI: getBlock returned block=%p", block);
    }
    if (!block) {
        setWorldReaderDiagnostic(WorldReaderDiagnostic::BlockUnavailable);
        return false;
    }
    const uintptr_t block_base = reinterpret_cast<uintptr_t>(block);
    uintptr_t aux_address = 0;
    uintptr_t legacy_address = 0;
    if (!checkedAdd(block_base, kBlockAuxOffset, &aux_address) ||
        !checkedAdd(block_base, kBlockLegacyOffset, &legacy_address) ||
        legacy_address > std::numeric_limits<uintptr_t>::max() - sizeof(void*)) {
        setWorldReaderDiagnostic(WorldReaderDiagnostic::BlockLayoutUnreadable);
        return false;
    }
    // The readability probe costs two pipe syscalls, so it runs once per
    // distinct Block instance rather than once per scanned block. The engine
    // interns one Block per block state and the cache is dropped whenever the
    // BlockSource changes, so a stale pointer cannot survive into a new world.
    if (validated_blocks_.find(block) == validated_blocks_.end()) {
        if (!IsMemoryReadable(reinterpret_cast<const void*>(aux_address),
                              legacy_address + sizeof(void*) - aux_address)) {
            setWorldReaderDiagnostic(WorldReaderDiagnostic::BlockLayoutUnreadable);
            return false;
        }
        // Guard against an engine build that does not intern block states:
        // an unbounded set would grow with the scanned volume instead.
        constexpr size_t kMaximumValidatedBlocks = 1u << 16;
        if (validated_blocks_.size() >= kMaximumValidatedBlocks) {
            validated_blocks_.clear();
        }
        validated_blocks_.insert(block);
    }

    void* legacy = nullptr;
    uint16_t aux = 0;
    std::memcpy(&aux, reinterpret_cast<const void*>(aux_address), sizeof(aux));
    std::memcpy(&legacy, reinterpret_cast<const void*>(legacy_address), sizeof(legacy));
    if (shouldLogWorldReaderAbi()) {
        BUILD_EXPORT_LOGI("world ABI: block layout block=%p aux=%u legacy=%p", block,
                          static_cast<unsigned int>(aux), legacy);
    }
    if (!legacy) {
        setWorldReaderDiagnostic(WorldReaderDiagnostic::BlockLegacyUnavailable);
        return false;
    }

    const auto cached = legacy_cache_.find(legacy);
    if (cached != legacy_cache_.end()) {
        output->name = &cached->second.name;
        // The Block object is interned per complete state.  Using the
        // BlockLegacy pointer here collapses distinct modern states (for
        // example repeater delay/powered variants) into one cache entry.
        output->type_token = block;
        output->aux = aux;
        output->legacy_id = cached->second.id;
        return !output->name->empty();
    }

    const uintptr_t legacy_base = reinterpret_cast<uintptr_t>(legacy);
    uintptr_t metadata_begin = 0;
    uintptr_t metadata_end = 0;
    if (!checkedAdd(legacy_base, kBlockLegacyRawNameOffset, &metadata_begin) ||
        !checkedAdd(legacy_base, kBlockLegacyIdOffset + sizeof(int32_t), &metadata_end) ||
        metadata_end < metadata_begin ||
        !IsMemoryReadable(reinterpret_cast<const void*>(metadata_begin),
                          metadata_end - metadata_begin)) {
        setWorldReaderDiagnostic(WorldReaderDiagnostic::LegacyLayoutUnreadable);
        return false;
    }

    const auto read_string = [&](uintptr_t offset, std::string* value) {
        const auto* raw = reinterpret_cast<const std::string*>(legacy_base + offset);
        const size_t size = raw->size();
        if (size == 0 || size > 256 || !IsMemoryReadable(raw->data(), size)) return false;
        value->assign(raw->data(), size);
        return true;
    };

    LegacyInfo info;
    if (!read_string(kBlockLegacyRawNameOffset, &info.name)) {
        if (!read_string(kBlockLegacyFullNameOffset, &info.name)) {
            setWorldReaderDiagnostic(WorldReaderDiagnostic::LegacyNameUnavailable);
            return false;
        }
    }
    info.name = namespaced(std::move(info.name));
    std::memcpy(&info.id, reinterpret_cast<const void*>(legacy_base + kBlockLegacyIdOffset),
                sizeof(info.id));
    if (info.name.empty()) {
        setWorldReaderDiagnostic(WorldReaderDiagnostic::LegacyNameUnavailable);
        return false;
    }
    const auto inserted = legacy_cache_.emplace(legacy, std::move(info)).first;
    output->name = &inserted->second.name;
    output->type_token = block;
    output->aux = aux;
    output->legacy_id = inserted->second.id;
    return true;
}

bool NativeWorldReader::getBlock(int32_t x, int32_t y, int32_t z,
                                 NativeBlockInfo* output) {
    if (!output) return false;
    *output = NativeBlockInfo{};
    NativeBlockView view;
    if (!getBlockView(x, y, z, &view) || !view.name) return false;
    output->name = *view.name;
    output->aux = view.aux;
    output->legacy_id = view.legacy_id;
    return true;
}

bool NativeWorldAccess::getBlock(int32_t x, int32_t y, int32_t z, NativeBlockInfo* output) {
    NativeWorldReader reader;
    return reader.open() && reader.getBlock(x, y, z, output);
}

bool NativeWorldAccess::getBlockSnapshot(int32_t x, int32_t y, int32_t z,
                                         NativeBlockSnapshot* output,
                                         bool include_container_items,
                                         int32_t dimension_id) {
    if (!output) return false;
    *output = NativeBlockSnapshot{};
    return queryPythonBlockSnapshot(x, y, z, output, include_container_items,
                                    dimension_id);
}

bool NativeWorldAccess::getCommandBlockData(int32_t x, int32_t y, int32_t z,
                                            NativeCommandBlockData* output) {
    if (!output) return false;
    *output = NativeCommandBlockData{};
    return queryPythonCommandBlockData(x, y, z, output);
}

bool NativeWorldAccess::isRegionReadable(const BlockBounds& bounds) {
    if (!bounds.isValid()) return false;
    const int32_t center_x = static_cast<int32_t>((static_cast<int64_t>(bounds.min_x) + bounds.max_x) / 2);
    const int32_t center_y = static_cast<int32_t>((static_cast<int64_t>(bounds.min_y) + bounds.max_y) / 2);
    const int32_t center_z = static_cast<int32_t>((static_cast<int64_t>(bounds.min_z) + bounds.max_z) / 2);
    const int32_t positions[5][3] = {
        {bounds.min_x, center_y, bounds.min_z}, {bounds.max_x, center_y, bounds.min_z},
        {bounds.min_x, center_y, bounds.max_z}, {bounds.max_x, center_y, bounds.max_z},
        {center_x, center_y, center_z},
    };
    NativeWorldReader reader;
    if (!reader.open()) return false;
    NativeBlockView block;
    for (const auto& position : positions) {
        if (!reader.getBlockView(position[0], position[1], position[2], &block)) return false;
    }
    return true;
}

bool NativeWorldAccess::isChunkReadable(const ChunkCoord& chunk, int32_t chunk_size,
                                        int32_t sample_y) {
    if (chunk_size <= 0) return false;
    const int64_t min_x64 = static_cast<int64_t>(chunk.x) * chunk_size;
    const int64_t min_z64 = static_cast<int64_t>(chunk.z) * chunk_size;
    int64_t max_x64 = 0;
    int64_t max_z64 = 0;
    int64_t center_x64 = 0;
    int64_t center_z64 = 0;
    if (!checkedAdd(min_x64, static_cast<int64_t>(chunk_size) - 1, &max_x64) ||
        !checkedAdd(min_z64, static_cast<int64_t>(chunk_size) - 1, &max_z64) ||
        !checkedAdd(min_x64, static_cast<int64_t>(chunk_size / 2), &center_x64) ||
        !checkedAdd(min_z64, static_cast<int64_t>(chunk_size / 2), &center_z64)) {
        return false;
    }
    int32_t min_x = 0;
    int32_t min_z = 0;
    int32_t max_x = 0;
    int32_t max_z = 0;
    int32_t center_x = 0;
    int32_t center_z = 0;
    if (!checkedInt32(min_x64, &min_x) || !checkedInt32(min_z64, &min_z) ||
        !checkedInt32(max_x64, &max_x) || !checkedInt32(max_z64, &max_z) ||
        !checkedInt32(center_x64, &center_x) || !checkedInt32(center_z64, &center_z)) {
        return false;
    }
    const int32_t positions[5][2] = {
        {min_x, min_z}, {max_x, min_z}, {min_x, max_z},
        {max_x, max_z}, {center_x, center_z},
    };
    NativeWorldReader reader;
    if (!reader.open()) return false;
    NativeBlockView block;
    for (const auto& position : positions) {
        if (!reader.getBlockView(position[0], sample_y, position[1], &block)) return false;
    }
    return true;
}

uintptr_t NativeWorldAccess::dimensionToken() {
    void* actor = GetLocalPlayerPointer();
    if (!actor || !IsMemoryReadable(actor, sizeof(void*))) return 0;
    using Function = void* (*)(void*);
    Function function = nullptr;
    return resolveProfileFunction(FunctionsAddress::Actor_getDimension, &function)
        ? reinterpret_cast<uintptr_t>(function(actor)) : 0;
}

bool NativeWorldAccess::getLocalPlayerBlockPosition(int32_t* x, int32_t* y, int32_t* z) {
    if (!x || !y || !z) return false;
    const int64_t now = monotonicNowNs();
    if (g_pythonPositionCache.valid && now >= g_pythonPositionCache.refreshed_at_ns &&
        now - g_pythonPositionCache.refreshed_at_ns <= kPythonPositionCacheNs) {
        *x = g_pythonPositionCache.x;
        *y = g_pythonPositionCache.y;
        *z = g_pythonPositionCache.z;
        return true;
    }

    int32_t current_x = 0;
    int32_t current_y = 0;
    int32_t current_z = 0;
    if (!queryPythonBlockPosition(&current_x, &current_y, &current_z) &&
        !queryRenderCameraBlockPosition(&current_x, &current_y, &current_z)) {
        return false;
    }
    g_pythonPositionCache = {current_x, current_y, current_z, now, true};
    *x = current_x;
    *y = current_y;
    *z = current_z;
    return true;
}

void NativeWorldAccess::invalidateLocalPlayerBlockPositionCache() {
    g_pythonPositionCache = {};
}

const char* NativeWorldAccess::lastWorldReaderDiagnostic() {
    switch (g_worldReaderDiagnostic.load(std::memory_order_acquire)) {
        case WorldReaderDiagnostic::None: return "native world reader ready";
        case WorldReaderDiagnostic::LocalPlayerUnavailable: return "local player is unavailable";
        case WorldReaderDiagnostic::ClientInstanceUnavailable: return "Actor::getClientInstance returned null";
        case WorldReaderDiagnostic::ClientVtableUnavailable: return "ClientInstance vtable is unavailable";
        case WorldReaderDiagnostic::RegionVirtualSlotUnavailable:
            return "ClientInstance region virtual slot is not valid";
        case WorldReaderDiagnostic::RegionUnavailable: return "ClientInstance region is unavailable";
        case WorldReaderDiagnostic::RegionVtableUnavailable: return "BlockSource vtable is unavailable";
        case WorldReaderDiagnostic::GetBlockVirtualSlotUnavailable:
            return "BlockSource getBlock virtual slot is not valid";
        case WorldReaderDiagnostic::BlockUnavailable: return "getBlock returned null";
        case WorldReaderDiagnostic::BlockLayoutUnreadable: return "Block layout offsets are not valid";
        case WorldReaderDiagnostic::BlockLegacyUnavailable: return "BlockLegacy pointer is null";
        case WorldReaderDiagnostic::LegacyLayoutUnreadable: return "BlockLegacy layout offsets are not valid";
        case WorldReaderDiagnostic::LegacyNameUnavailable: return "BlockLegacy name layout is not valid";
    }
    return "unknown native world reader failure";
}

}  // namespace build_import
