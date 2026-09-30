#include "MapPairPlacementJournal.h"
#include "MapJournalAtomicCommit.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <sys/stat.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace build_import {
namespace {

constexpr size_t kWorldOffset = 88U;
constexpr size_t kWorldCapacity = 192U;
constexpr size_t kUuidLengthOffset = kWorldOffset + kWorldCapacity;
constexpr size_t kUuidOffset = 288U;
constexpr size_t kUuidCapacity = 96U;
constexpr size_t kChecksumOffset = kUuidOffset + kUuidCapacity;
constexpr size_t kDiskBytes = kChecksumOffset + 8U;
constexpr uint32_t kVersion = 2U;
constexpr uint64_t kMaximumJobMaps = 65536U;
constexpr std::array<uint8_t, 8> kMagic{{'M', 'P', 'P', 'L', 'A', 'N', '2', 0}};
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
std::mutex g_mutex;

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

template <typename UInt>
void put(std::array<uint8_t, kDiskBytes>* bytes, size_t offset, UInt value) {
    for (size_t i = 0; i < sizeof(UInt); ++i) {
        (*bytes)[offset + i] = static_cast<uint8_t>(value >> (i * 8U));
    }
}

template <typename UInt>
UInt get(const std::array<uint8_t, kDiskBytes>& bytes, size_t offset) {
    UInt result = 0;
    for (size_t i = 0; i < sizeof(UInt); ++i) {
        result |= static_cast<UInt>(bytes[offset + i]) << (i * 8U);
    }
    return result;
}

int32_t signed32(uint32_t bits) {
    int32_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint64_t checksum(const std::array<uint8_t, kDiskBytes>& bytes) {
    uint64_t hash = kFnvOffset;
    for (size_t i = 0; i < kChecksumOffset; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

bool sameBounds(const BlockBounds& a, const BlockBounds& b) {
    return a.min_x == b.min_x && a.min_y == b.min_y && a.min_z == b.min_z &&
           a.max_x == b.max_x && a.max_y == b.max_y && a.max_z == b.max_z;
}

bool sameRecord(const MapPairPlacementRecord& a,
                const MapPairPlacementRecord& b) {
    return a.world_id == b.world_id && a.dimension_id == b.dimension_id &&
           a.tile_count == b.tile_count &&
           sameBounds(a.artwork_bounds, b.artwork_bounds) &&
           a.pair == b.pair && a.phase == b.phase &&
           a.support_created == b.support_created &&
           a.support_retry_used == b.support_retry_used &&
           a.platform_side == b.platform_side &&
           a.active_command_uuid == b.active_command_uuid;
}

bool isAirName(const NativeBlockInfo* block) {
    return block && (block->name == "minecraft:air" ||
                     block->name == "minecraft:cave_air" ||
                     block->name == "minecraft:void_air");
}

bool validCommandUuid(const std::string& uuid) {
    if (uuid.empty() || uuid.size() > kUuidCapacity) return false;
    for (const char character : uuid) {
        const bool letter = (character >= 'a' && character <= 'z') ||
                            (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!letter && !digit && character != '-' && character != '_') {
            return false;
        }
    }
    return true;
}

bool exactNativePlacement(MapPairPlacementStep step,
                          const NativeBlockInfo* chest,
                          const NativeBlockInfo* anvil) {
    if (!chest) return false;
    if (step == MapPairPlacementStep::Support) {
        return chest->name == "minecraft:stone" && anvil &&
               anvil->name == "minecraft:stone";
    }
    if (chest->name != "minecraft:chest") return false;
    if (step == MapPairPlacementStep::Chest) return true;
    return step == MapPairPlacementStep::Anvil && anvil &&
           anvil->name == "minecraft:anvil";
}

bool exactPadReadback(const MapPairPlacementRecord& record,
                      MapPairPlacementStep step,
                      const NativeBlockInfo* first,
                      const NativeBlockInfo* second,
                      bool expect_air) {
    if (step != MapPairPlacementStep::Support || record.platform_side == 0U) {
        return true;
    }
    return expect_air ? isAirName(first) && isAirName(second)
                      : first && second &&
                            first->name == "minecraft:stone" &&
                            second->name == "minecraft:stone";
}

bool validRecord(const MapPairPlacementRecord& record, std::string* error) {
    if (record.world_id.empty() || record.world_id.size() > kWorldCapacity ||
        record.world_id.find('\0') != std::string::npos ||
        record.dimension_id < 0 || record.dimension_id > 255 ||
        record.tile_count == 0U || record.tile_count > kMaximumJobMaps ||
        !record.artwork_bounds.isValid()) {
        return fail(error, "map pair plan has invalid identity or bounds");
    }
    switch (record.phase) {
        case MapPairPlacementPhase::Selected:
        case MapPairPlacementPhase::ChestDispatchArmed:
        case MapPairPlacementPhase::ChestConfirmed:
        case MapPairPlacementPhase::AnvilDispatchArmed:
        case MapPairPlacementPhase::PairConfirmed:
        case MapPairPlacementPhase::SupportDispatchArmed:
        case MapPairPlacementPhase::SupportConfirmed:
        case MapPairPlacementPhase::SupportSelected:
            break;
        default:
            return fail(error, "map pair plan has an unknown phase");
    }
    const bool support_phase =
        record.phase == MapPairPlacementPhase::SupportSelected ||
        record.phase == MapPairPlacementPhase::SupportDispatchArmed ||
        record.phase == MapPairPlacementPhase::SupportConfirmed;
    if ((support_phase && !record.support_created) ||
        (record.phase == MapPairPlacementPhase::Selected &&
         record.support_created) ||
        (record.support_retry_used && !record.support_created) ||
        record.platform_side > 2U ||
        (record.platform_side != 0U && !record.support_created)) {
        return fail(error, "map pair plan has an invalid support mode");
    }
    const bool armed = record.phase == MapPairPlacementPhase::ChestDispatchArmed ||
                       record.phase == MapPairPlacementPhase::AnvilDispatchArmed ||
                       record.phase == MapPairPlacementPhase::SupportDispatchArmed;
    if (armed ? !validCommandUuid(record.active_command_uuid) :
                !record.active_command_uuid.empty()) {
        return fail(error, "map pair plan has an invalid active command UUID");
    }
    const std::vector<MapChestAnvilPosition> candidates =
        EnumerateMapChestAnvilCandidates(record.artwork_bounds);
    bool planned = false;
    for (const MapChestAnvilPosition& candidate : candidates) {
        if (candidate == record.pair) {
            planned = true;
            break;
        }
    }
    if (!planned) return fail(error, "map pair is outside the frozen placement plan");
    return true;
}

std::array<uint8_t, kDiskBytes> encode(const MapPairPlacementRecord& record) {
    std::array<uint8_t, kDiskBytes> bytes{};
    std::memcpy(bytes.data(), kMagic.data(), kMagic.size());
    put<uint32_t>(&bytes, 8U, kVersion);
    put<uint32_t>(&bytes, 12U, static_cast<uint32_t>(kDiskBytes));
    put<uint32_t>(&bytes, 16U, static_cast<uint32_t>(record.phase));
    put<uint32_t>(&bytes, 20U, static_cast<uint32_t>(record.dimension_id));
    put<uint64_t>(&bytes, 24U, record.tile_count);
    const std::array<int32_t, 12> coordinates{{
        record.artwork_bounds.min_x, record.artwork_bounds.min_y,
        record.artwork_bounds.min_z, record.artwork_bounds.max_x,
        record.artwork_bounds.max_y, record.artwork_bounds.max_z,
        record.pair.chest.x, record.pair.chest.y, record.pair.chest.z,
        record.pair.anvil.x, record.pair.anvil.y, record.pair.anvil.z,
    }};
    for (size_t i = 0; i < coordinates.size(); ++i) {
        put<uint32_t>(&bytes, 32U + i * 4U,
                      static_cast<uint32_t>(coordinates[i]));
    }
    put<uint16_t>(&bytes, 80U, static_cast<uint16_t>(record.world_id.size()));
    bytes[82U] = record.support_created ? 1U : 0U;
    bytes[83U] = record.support_retry_used ? 1U : 0U;
    bytes[84U] = record.platform_side;
    std::memcpy(bytes.data() + kWorldOffset, record.world_id.data(),
                record.world_id.size());
    put<uint16_t>(&bytes, kUuidLengthOffset,
                  static_cast<uint16_t>(record.active_command_uuid.size()));
    std::memcpy(bytes.data() + kUuidOffset,
                record.active_command_uuid.data(),
                record.active_command_uuid.size());
    put<uint64_t>(&bytes, kChecksumOffset, checksum(bytes));
    return bytes;
}

bool decode(const std::array<uint8_t, kDiskBytes>& bytes,
            MapPairPlacementRecord* output, std::string* error) {
    if (!output || std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0 ||
        get<uint32_t>(bytes, 8U) != kVersion ||
        get<uint32_t>(bytes, 12U) != kDiskBytes ||
        get<uint64_t>(bytes, kChecksumOffset) != checksum(bytes)) {
        return fail(error, "map pair plan header or checksum is invalid");
    }
    const uint16_t world_size = get<uint16_t>(bytes, 80U);
    const uint16_t uuid_size = get<uint16_t>(bytes, kUuidLengthOffset);
    if (world_size == 0U || world_size > kWorldCapacity) {
        return fail(error, "map pair plan world identity length is invalid");
    }
    if (uuid_size > kUuidCapacity) {
        return fail(error, "map pair plan command UUID length is invalid");
    }
    if (bytes[82U] > 1U) {
        return fail(error, "map pair plan support marker is invalid");
    }
    if (bytes[83U] > 1U) {
        return fail(error, "map pair plan support retry marker is invalid");
    }
    if (bytes[84U] > 2U) {
        return fail(error, "map pair plan platform side is invalid");
    }
    for (size_t i = 85U; i < kWorldOffset; ++i) {
        if (bytes[i] != 0U) return fail(error, "map pair plan reserved bytes are invalid");
    }
    for (size_t i = kWorldOffset + world_size; i < kUuidLengthOffset; ++i) {
        if (bytes[i] != 0U) return fail(error, "map pair plan padding is invalid");
    }
    for (size_t i = kUuidLengthOffset + 2U; i < kUuidOffset; ++i) {
        if (bytes[i] != 0U) return fail(error, "map pair plan reserved bytes are invalid");
    }
    for (size_t i = kUuidOffset + uuid_size; i < kChecksumOffset; ++i) {
        if (bytes[i] != 0U) return fail(error, "map pair plan UUID padding is invalid");
    }
    MapPairPlacementRecord record;
    record.phase = static_cast<MapPairPlacementPhase>(get<uint32_t>(bytes, 16U));
    record.support_created = bytes[82U] != 0U;
    record.support_retry_used = bytes[83U] != 0U;
    record.platform_side = bytes[84U];
    record.dimension_id = signed32(get<uint32_t>(bytes, 20U));
    record.tile_count = get<uint64_t>(bytes, 24U);
    std::array<int32_t, 12> coordinates{};
    for (size_t i = 0; i < coordinates.size(); ++i) {
        coordinates[i] = signed32(get<uint32_t>(bytes, 32U + i * 4U));
    }
    record.artwork_bounds = {coordinates[0], coordinates[1], coordinates[2],
                             coordinates[3], coordinates[4], coordinates[5]};
    record.pair.chest = {coordinates[6], coordinates[7], coordinates[8]};
    record.pair.anvil = {coordinates[9], coordinates[10], coordinates[11]};
    record.world_id.assign(reinterpret_cast<const char*>(bytes.data() +
                            kWorldOffset), world_size);
    record.active_command_uuid.assign(
        reinterpret_cast<const char*>(bytes.data() + kUuidOffset), uuid_size);
    if (!validRecord(record, error)) return false;
    *output = std::move(record);
    return true;
}

bool exists(const std::string& path, bool* result) {
    struct stat info {};
    if (stat(path.c_str(), &info) == 0) {
        *result = true;
        return true;
    }
    *result = false;
    return errno == ENOENT;
}

#if defined(_WIN32)
std::wstring widePath(const std::string& path) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           path.c_str(), -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(),
                               -1, wide.data(), length) == length ? wide :
                               std::wstring{};
}
#else
bool syncParent(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const std::string parent = slash == std::string::npos ? "." :
        (slash == 0 ? "/" : path.substr(0, slash));
    const int fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool synced = fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    return synced && closed;
}
#endif

bool writeAtomic(const std::string& path,
                 const std::array<uint8_t, kDiskBytes>& bytes,
                 bool replace, std::string* error) {
    const std::string temporary = path + ".tmp";
#if defined(_WIN32)
    const std::wstring temporary_wide = widePath(temporary);
    const std::wstring path_wide = widePath(path);
    if (temporary_wide.empty() || path_wide.empty()) {
        return fail(error, "map pair plan path is not valid UTF-8");
    }
    HANDLE file = CreateFileW(temporary_wide.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return fail(error, "map pair plan temporary file already exists or cannot be created");
    }
    DWORD written = 0;
    const bool saved = WriteFile(file, bytes.data(),
                                 static_cast<DWORD>(bytes.size()), &written,
                                 nullptr) != 0 && written == bytes.size() &&
                       FlushFileBuffers(file) != 0;
    const bool closed = CloseHandle(file) != 0;
    if (!saved || !closed) return fail(error, "map pair plan cannot be synced");
    const DWORD flags = MOVEFILE_WRITE_THROUGH |
        (replace ? MOVEFILE_REPLACE_EXISTING : 0U);
    if (!MoveFileExW(temporary_wide.c_str(), path_wide.c_str(), flags)) {
        return fail(error, "map pair plan cannot be committed");
    }
#else
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                        0600);
    if (fd < 0) {
        return fail(error, "map pair plan temporary file already exists or cannot be created");
    }
    size_t cursor = 0U;
    bool saved = true;
    while (cursor < bytes.size()) {
        const ssize_t count = write(fd, bytes.data() + cursor, bytes.size() - cursor);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { saved = false; break; }
        cursor += static_cast<size_t>(count);
    }
    saved = saved && fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    if (!saved || !closed) return fail(error, "map pair plan cannot be synced");
    if (replace) {
        if (rename(temporary.c_str(), path.c_str()) != 0) {
            return fail(error, "map pair plan cannot be replaced");
        }
    } else {
        if (!CommitMapJournalNoReplace(temporary, path)) {
            return fail(error, "map pair plan cannot be created exclusively");
        }
    }
    if (!syncParent(path)) return fail(error, "map pair plan parent directory cannot be synced");
#endif
    if (error) error->clear();
    return true;
}

MapPairPlacementLoad loadUnlocked(const std::string& map_state_path,
                                   MapPairPlacementRecord* output,
                                   std::string* error) {
    if (map_state_path.empty() || !output) {
        fail(error, "map pair plan path or output is unavailable");
        return MapPairPlacementLoad::Unsafe;
    }
    *output = {};
    const std::string path = MapPairPlacementJournalPath(map_state_path);
    bool temporary_exists = false;
    bool plan_exists = false;
    if (!exists(path + ".tmp", &temporary_exists) || temporary_exists ||
        !exists(path, &plan_exists)) {
        fail(error, "map pair plan has an unresolved temporary file or path error");
        return MapPairPlacementLoad::Unsafe;
    }
    if (!plan_exists) {
        if (error) error->clear();
        return MapPairPlacementLoad::Missing;
    }
    struct stat info {};
    if (stat(path.c_str(), &info) != 0 ||
        info.st_size != static_cast<decltype(info.st_size)>(kDiskBytes)) {
        fail(error, "map pair plan has an invalid file size");
        return MapPairPlacementLoad::Unsafe;
    }
    std::array<uint8_t, kDiskBytes> bytes{};
    std::ifstream input(path, std::ios::binary);
    char extra = 0;
    if (!input || !input.read(reinterpret_cast<char*>(bytes.data()),
                              bytes.size()) || input.read(&extra, 1) ||
        !decode(bytes, output, error)) {
        if (error && error->empty()) *error = "map pair plan cannot be read";
        return MapPairPlacementLoad::Unsafe;
    }
    if (error) error->clear();
    return MapPairPlacementLoad::Loaded;
}

bool replaceExpected(const std::string& map_state_path,
                     const MapPairPlacementRecord& expected,
                     const MapPairPlacementRecord& next,
                     std::string* error) {
    MapPairPlacementRecord current;
    if (loadUnlocked(map_state_path, &current, error) != MapPairPlacementLoad::Loaded ||
        !sameRecord(current, expected)) {
        return fail(error, "map pair plan changed before phase transition");
    }
    return writeAtomic(MapPairPlacementJournalPath(map_state_path), encode(next),
                       true, error);
}

}  // namespace

std::string MapPairPlacementJournalPath(const std::string& map_state_path) {
    return map_state_path + ".pair_plan";
}

bool BeginMapPairPlacementJournal(const std::string& map_state_path,
                                  const MapPairPlacementRecord& selected,
                                  std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (map_state_path.empty() ||
        (selected.phase != MapPairPlacementPhase::SupportSelected &&
         selected.phase != MapPairPlacementPhase::Selected)) {
        return fail(error, "map pair plan requires a path and selected phase");
    }
    if (!validRecord(selected, error)) return false;
    MapPairPlacementRecord existing;
    if (loadUnlocked(map_state_path, &existing, error) != MapPairPlacementLoad::Missing) {
        return fail(error, "map pair plan already exists or is unsafe");
    }
    return writeAtomic(MapPairPlacementJournalPath(map_state_path),
                       encode(selected), false, error);
}

MapPairPlacementLoad LoadMapPairPlacementJournal(
        const std::string& map_state_path, MapPairPlacementRecord* output,
        std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return loadUnlocked(map_state_path, output, error);
}

bool ArmMapPairPlacementDispatch(const std::string& map_state_path,
                                 const MapPairPlacementRecord& expected,
                                 MapPairPlacementStep step,
                                 const std::string& command_uuid,
                                 std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!validCommandUuid(command_uuid)) {
        return fail(error, "map pair placement command UUID is invalid");
    }
    MapPairPlacementRecord next = expected;
    if (step == MapPairPlacementStep::Support &&
        expected.phase == MapPairPlacementPhase::SupportSelected) {
        next.phase = MapPairPlacementPhase::SupportDispatchArmed;
    } else if (step == MapPairPlacementStep::Chest &&
        (expected.phase == MapPairPlacementPhase::Selected ||
         expected.phase == MapPairPlacementPhase::SupportConfirmed)) {
        next.phase = MapPairPlacementPhase::ChestDispatchArmed;
    } else if (step == MapPairPlacementStep::Anvil &&
               expected.phase == MapPairPlacementPhase::ChestConfirmed) {
        next.phase = MapPairPlacementPhase::AnvilDispatchArmed;
    } else {
        return fail(error, "map pair plan cannot arm this placement step");
    }
    next.active_command_uuid = command_uuid;
    return replaceExpected(map_state_path, expected, next, error);
}

bool UpgradeMapPairSupportSelectedPlatform(
        const std::string& map_state_path,
        const MapPairPlacementRecord& expected_selected,
        uint8_t platform_side, std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (expected_selected.phase != MapPairPlacementPhase::SupportSelected ||
        !expected_selected.support_created ||
        expected_selected.platform_side != 0U ||
        !expected_selected.active_command_uuid.empty() ||
        platform_side < 1U || platform_side > 2U) {
        return fail(error, "map pair platform upgrade needs unarmed support");
    }
    MapPairPlacementRecord next = expected_selected;
    next.platform_side = platform_side;
    if (!validRecord(next, error)) return false;
    return replaceExpected(map_state_path, expected_selected, next, error);
}

bool RearmMapPairSupportDispatchOnce(
        const std::string& map_state_path,
        const MapPairPlacementRecord& expected_armed,
        const std::string& replacement_command_uuid,
        const NativeBlockInfo* chest_floor,
        const NativeBlockInfo* anvil_floor,
        std::string* error,
        const NativeBlockInfo* pad_first,
        const NativeBlockInfo* pad_second) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (expected_armed.phase != MapPairPlacementPhase::SupportDispatchArmed ||
        !expected_armed.support_created || expected_armed.support_retry_used ||
        !validCommandUuid(expected_armed.active_command_uuid) ||
        !validCommandUuid(replacement_command_uuid) ||
        replacement_command_uuid == expected_armed.active_command_uuid ||
        !isAirName(chest_floor) || !isAirName(anvil_floor) ||
        !exactPadReadback(expected_armed, MapPairPlacementStep::Support,
                          pad_first, pad_second, true)) {
        return fail(error, "map pair support retry lacks exact air and unused Armed intent");
    }
    MapPairPlacementRecord next = expected_armed;
    next.support_retry_used = true;
    next.active_command_uuid = replacement_command_uuid;
    return replaceExpected(map_state_path, expected_armed, next, error);
}

bool BlockMapPairSupportRetry(
        const std::string& map_state_path,
        const MapPairPlacementRecord& expected_armed,
        std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (expected_armed.phase != MapPairPlacementPhase::SupportDispatchArmed ||
        !expected_armed.support_created || expected_armed.support_retry_used ||
        !validCommandUuid(expected_armed.active_command_uuid)) {
        return fail(error, "map pair support rejection cannot consume retry");
    }
    MapPairPlacementRecord next = expected_armed;
    next.support_retry_used = true;
    return replaceExpected(map_state_path, expected_armed, next, error);
}

bool ConfirmMapPairPlacementStep(const std::string& map_state_path,
                                 const MapPairPlacementRecord& expected_armed,
                                 MapPairPlacementStep step,
                                 const std::string& ack_command_uuid,
                                 MapPairCommandAck ack,
                                 const NativeBlockInfo* chest,
                                 const NativeBlockInfo* anvil,
                                 std::string* error,
                                 const NativeBlockInfo* pad_first,
                                 const NativeBlockInfo* pad_second) {
    std::lock_guard<std::mutex> lock(g_mutex);
    MapPairPlacementRecord next = expected_armed;
    if (step == MapPairPlacementStep::Support &&
        expected_armed.phase == MapPairPlacementPhase::SupportDispatchArmed) {
        next.phase = MapPairPlacementPhase::SupportConfirmed;
    } else if (step == MapPairPlacementStep::Chest &&
        expected_armed.phase == MapPairPlacementPhase::ChestDispatchArmed) {
        next.phase = MapPairPlacementPhase::ChestConfirmed;
    } else if (step == MapPairPlacementStep::Anvil &&
               expected_armed.phase == MapPairPlacementPhase::AnvilDispatchArmed) {
        next.phase = MapPairPlacementPhase::PairConfirmed;
    } else {
        return fail(error, "map pair plan cannot confirm this placement step");
    }
    if (expected_armed.active_command_uuid.empty() ||
        ack_command_uuid != expected_armed.active_command_uuid) {
        return fail(error, "map pair placement ACK UUID does not match armed command");
    }
    if (VerifyMapPairPlacementAfterAck(step, ack, chest, anvil) !=
            MapPairPlacementVerification::Confirmed ||
        !exactPadReadback(expected_armed, step,
                          pad_first, pad_second, false)) {
        return fail(error, "map pair placement lacks accepted ACK and exact native readback");
    }
    next.active_command_uuid.clear();
    return replaceExpected(map_state_path, expected_armed, next, error);
}

bool ConfirmMapPairPlacementStepByNativeReadback(
        const std::string& map_state_path,
        const MapPairPlacementRecord& expected_armed,
        MapPairPlacementStep step,
        const NativeBlockInfo* chest,
        const NativeBlockInfo* anvil,
        std::string* error,
        const NativeBlockInfo* pad_first,
        const NativeBlockInfo* pad_second) {
    std::lock_guard<std::mutex> lock(g_mutex);
    MapPairPlacementRecord next = expected_armed;
    if (step == MapPairPlacementStep::Support &&
        expected_armed.phase == MapPairPlacementPhase::SupportDispatchArmed) {
        next.phase = MapPairPlacementPhase::SupportConfirmed;
    } else if (step == MapPairPlacementStep::Chest &&
               expected_armed.phase == MapPairPlacementPhase::ChestDispatchArmed) {
        next.phase = MapPairPlacementPhase::ChestConfirmed;
    } else if (step == MapPairPlacementStep::Anvil &&
               expected_armed.phase == MapPairPlacementPhase::AnvilDispatchArmed) {
        next.phase = MapPairPlacementPhase::PairConfirmed;
    } else {
        return fail(error, "map pair native recovery has no armed placement step");
    }
    if (!validCommandUuid(expected_armed.active_command_uuid) ||
        !exactNativePlacement(step, chest, anvil) ||
        !exactPadReadback(expected_armed, step,
                          pad_first, pad_second, false)) {
        return fail(error, "map pair native recovery lacks exact block readback");
    }
    next.active_command_uuid.clear();
    return replaceExpected(map_state_path, expected_armed, next, error);
}

MapPairPlacementResume ClassifyMapPairPlacementResume(
        const MapPairPlacementRecord& record, const std::string& world_id,
        int32_t dimension_id, uint64_t tile_count,
        const BlockBounds& artwork_bounds) noexcept {
    if (record.world_id.empty() || world_id != record.world_id ||
        dimension_id != record.dimension_id || tile_count != record.tile_count ||
        !sameBounds(artwork_bounds, record.artwork_bounds)) {
        return MapPairPlacementResume::Unsafe;
    }
    const bool support_phase =
        record.phase == MapPairPlacementPhase::SupportSelected ||
        record.phase == MapPairPlacementPhase::SupportDispatchArmed ||
        record.phase == MapPairPlacementPhase::SupportConfirmed;
    if ((support_phase && !record.support_created) ||
        (record.phase == MapPairPlacementPhase::Selected &&
         record.support_created)) {
        return MapPairPlacementResume::Unsafe;
    }
    switch (record.phase) {
        case MapPairPlacementPhase::SupportSelected:
            return MapPairPlacementResume::SurveySelectedSupport;
        case MapPairPlacementPhase::SupportDispatchArmed:
            return MapPairPlacementResume::AmbiguousSupportDispatch;
        case MapPairPlacementPhase::SupportConfirmed:
            return MapPairPlacementResume::VerifySupportBeforeChest;
        case MapPairPlacementPhase::Selected:
            return MapPairPlacementResume::SurveySelectedPair;
        case MapPairPlacementPhase::ChestDispatchArmed:
            return MapPairPlacementResume::AmbiguousChestDispatch;
        case MapPairPlacementPhase::ChestConfirmed:
            return MapPairPlacementResume::VerifyChestBeforeAnvil;
        case MapPairPlacementPhase::AnvilDispatchArmed:
            return MapPairPlacementResume::AmbiguousAnvilDispatch;
        case MapPairPlacementPhase::PairConfirmed:
            return MapPairPlacementResume::VerifyPairBeforeStorage;
    }
    return MapPairPlacementResume::Unsafe;
}

bool ClearCompletedMapPairPlacementJournal(
        const std::string& map_state_path,
        const MapPairPlacementRecord& expected_confirmed,
        uint64_t checkpoint_tile_cursor, std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (expected_confirmed.phase != MapPairPlacementPhase::PairConfirmed ||
        checkpoint_tile_cursor != expected_confirmed.tile_count ||
        !validRecord(expected_confirmed, error)) {
        return fail(error, "map pair plan cannot be cleared before all maps commit");
    }
    MapPairPlacementRecord current;
    if (loadUnlocked(map_state_path, &current, error) != MapPairPlacementLoad::Loaded ||
        !sameRecord(current, expected_confirmed)) {
        return fail(error, "map pair plan changed before completion cleanup");
    }
    const std::string path = MapPairPlacementJournalPath(map_state_path);
    if (std::remove(path.c_str()) != 0) {
        return fail(error, "map pair plan cannot be removed after final commit");
    }
#if !defined(_WIN32)
    if (!syncParent(path)) return fail(error, "map pair plan removal cannot be synced");
#endif
    if (error) error->clear();
    return true;
}

}  // namespace build_import
