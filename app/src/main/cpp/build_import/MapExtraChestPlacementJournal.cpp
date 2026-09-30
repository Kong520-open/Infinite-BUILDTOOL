#include "MapExtraChestPlacementJournal.h"
#include "MapJournalAtomicCommit.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
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

constexpr size_t kWorldOffset = 80U;
constexpr size_t kWorldCapacity = 192U;
constexpr size_t kUuidOffset = kWorldOffset + kWorldCapacity;
constexpr size_t kUuidCapacity = 96U;
constexpr size_t kChecksumOffset = kUuidOffset + kUuidCapacity;
constexpr size_t kDiskBytes = kChecksumOffset + 8U;
constexpr uint32_t kVersion = 1U;
constexpr std::array<uint8_t, 8> kMagic{{'M', 'E', 'X', 'C', 'H', 'S', 'T', '1'}};
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

bool sameIdentity(const MapExtraChestPlacementRecord& a,
                  const MapExtraChestPlacementRecord& b) {
    return a.world_id == b.world_id && a.dimension_id == b.dimension_id &&
           a.tile_count == b.tile_count &&
           sameBounds(a.artwork_bounds, b.artwork_bounds);
}

bool sameRecord(const MapExtraChestPlacementRecord& a,
                const MapExtraChestPlacementRecord& b) {
    return sameIdentity(a, b) && a.chest_index == b.chest_index &&
           a.chest == b.chest && a.phase == b.phase &&
           a.support_created == b.support_created &&
           a.support_retry_used == b.support_retry_used &&
           a.platform_corner == b.platform_corner &&
           a.command_uuid == b.command_uuid;
}

bool isAirName(const std::string& name) {
    return name == "minecraft:air" || name == "minecraft:cave_air" ||
           name == "minecraft:void_air";
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

bool isPlannedSite(const BlockBounds& bounds,
                   const MapChestPosition& chest) {
    const std::vector<MapChestPosition> candidates =
        EnumerateMapChestCandidates(bounds);
    for (const MapChestPosition& candidate : candidates) {
        if (candidate == chest) return true;
    }
    return false;
}

bool validRecord(const MapExtraChestPlacementRecord& record,
                 std::string* error) {
    if (record.world_id.empty() || record.world_id.size() > kWorldCapacity ||
        record.world_id.find('\0') != std::string::npos ||
        record.dimension_id < 0 || record.dimension_id > 255 ||
        record.tile_count <= kMapChestSlotCount ||
        record.tile_count > kMaximumMapTileCount ||
        !record.artwork_bounds.isValid() ||
        record.chest_index == 0U ||
        static_cast<uint64_t>(record.chest_index) * kMapChestSlotCount >=
            record.tile_count ||
        !isPlannedSite(record.artwork_bounds, record.chest)) {
        return fail(error, "extra chest plan has invalid identity, index or site");
    }
    switch (record.phase) {
        case MapExtraChestPlacementPhase::Selected:
        case MapExtraChestPlacementPhase::DispatchArmed:
        case MapExtraChestPlacementPhase::Confirmed:
        case MapExtraChestPlacementPhase::SupportSelected:
        case MapExtraChestPlacementPhase::SupportDispatchArmed:
        case MapExtraChestPlacementPhase::SupportConfirmed:
            break;
        default:
            return fail(error, "extra chest plan has unknown phase");
    }
    const bool support_phase =
        record.phase == MapExtraChestPlacementPhase::SupportSelected ||
        record.phase == MapExtraChestPlacementPhase::SupportDispatchArmed ||
        record.phase == MapExtraChestPlacementPhase::SupportConfirmed;
    if ((support_phase && !record.support_created) ||
        (record.phase == MapExtraChestPlacementPhase::Selected &&
         record.support_created) ||
        (record.support_retry_used && !record.support_created) ||
        record.platform_corner > 4U ||
        (record.platform_corner != 0U && !record.support_created)) {
        return fail(error, "extra chest plan has invalid support mode");
    }
    if ((record.phase == MapExtraChestPlacementPhase::Selected ||
         record.phase == MapExtraChestPlacementPhase::SupportSelected) ?
            !record.command_uuid.empty() :
            !validCommandUuid(record.command_uuid)) {
        return fail(error, "extra chest plan has invalid command UUID");
    }
    return true;
}

std::array<uint8_t, kDiskBytes> encode(
        const MapExtraChestPlacementRecord& record) {
    std::array<uint8_t, kDiskBytes> bytes{};
    std::memcpy(bytes.data(), kMagic.data(), kMagic.size());
    put<uint32_t>(&bytes, 8U, kVersion);
    put<uint32_t>(&bytes, 12U, static_cast<uint32_t>(kDiskBytes));
    put<uint32_t>(&bytes, 16U, static_cast<uint32_t>(record.phase));
    put<uint32_t>(&bytes, 20U, static_cast<uint32_t>(record.dimension_id));
    put<uint64_t>(&bytes, 24U, record.tile_count);
    put<uint32_t>(&bytes, 32U, record.chest_index);
    const std::array<int32_t, 9> coordinates{{
        record.artwork_bounds.min_x, record.artwork_bounds.min_y,
        record.artwork_bounds.min_z, record.artwork_bounds.max_x,
        record.artwork_bounds.max_y, record.artwork_bounds.max_z,
        record.chest.x, record.chest.y, record.chest.z,
    }};
    for (size_t i = 0; i < coordinates.size(); ++i) {
        put<uint32_t>(&bytes, 36U + i * 4U,
                      static_cast<uint32_t>(coordinates[i]));
    }
    put<uint16_t>(&bytes, 72U, static_cast<uint16_t>(record.world_id.size()));
    put<uint16_t>(&bytes, 74U,
                  static_cast<uint16_t>(record.command_uuid.size()));
    bytes[76U] = record.support_created ? 1U : 0U;
    bytes[77U] = record.support_retry_used ? 1U : 0U;
    bytes[78U] = record.platform_corner;
    std::memcpy(bytes.data() + kWorldOffset, record.world_id.data(),
                record.world_id.size());
    std::memcpy(bytes.data() + kUuidOffset, record.command_uuid.data(),
                record.command_uuid.size());
    put<uint64_t>(&bytes, kChecksumOffset, checksum(bytes));
    return bytes;
}

bool decode(const std::array<uint8_t, kDiskBytes>& bytes,
            MapExtraChestPlacementRecord* output, std::string* error) {
    if (!output || std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0 ||
        get<uint32_t>(bytes, 8U) != kVersion ||
        get<uint32_t>(bytes, 12U) != kDiskBytes ||
        get<uint64_t>(bytes, kChecksumOffset) != checksum(bytes)) {
        return fail(error, "extra chest plan header or checksum is invalid");
    }
    const uint16_t world_size = get<uint16_t>(bytes, 72U);
    const uint16_t uuid_size = get<uint16_t>(bytes, 74U);
    if (world_size == 0U || world_size > kWorldCapacity ||
        uuid_size > kUuidCapacity) {
        return fail(error, "extra chest plan field length is invalid");
    }
    if (bytes[76U] > 1U) {
        return fail(error, "extra chest plan support marker is invalid");
    }
    if (bytes[77U] > 1U) {
        return fail(error, "extra chest plan support retry marker is invalid");
    }
    if (bytes[78U] > 4U) {
        return fail(error, "extra chest plan platform corner is invalid");
    }
    for (size_t i = 79U; i < kWorldOffset; ++i) {
        if (bytes[i] != 0U) return fail(error, "extra chest plan reserved bytes are invalid");
    }
    for (size_t i = kWorldOffset + world_size; i < kUuidOffset; ++i) {
        if (bytes[i] != 0U) return fail(error, "extra chest plan world padding is invalid");
    }
    for (size_t i = kUuidOffset + uuid_size; i < kChecksumOffset; ++i) {
        if (bytes[i] != 0U) return fail(error, "extra chest plan UUID padding is invalid");
    }
    const uint32_t index = get<uint32_t>(bytes, 32U);
    if (index > UINT16_MAX) return fail(error, "extra chest index is invalid");
    MapExtraChestPlacementRecord record;
    record.phase =
        static_cast<MapExtraChestPlacementPhase>(get<uint32_t>(bytes, 16U));
    record.support_created = bytes[76U] != 0U;
    record.support_retry_used = bytes[77U] != 0U;
    record.platform_corner = bytes[78U];
    record.dimension_id = signed32(get<uint32_t>(bytes, 20U));
    record.tile_count = get<uint64_t>(bytes, 24U);
    record.chest_index = static_cast<uint16_t>(index);
    std::array<int32_t, 9> coordinates{};
    for (size_t i = 0; i < coordinates.size(); ++i) {
        coordinates[i] = signed32(get<uint32_t>(bytes, 36U + i * 4U));
    }
    record.artwork_bounds = {coordinates[0], coordinates[1], coordinates[2],
                             coordinates[3], coordinates[4], coordinates[5]};
    record.chest = {coordinates[6], coordinates[7], coordinates[8]};
    record.world_id.assign(reinterpret_cast<const char*>(bytes.data() +
                           kWorldOffset), world_size);
    record.command_uuid.assign(reinterpret_cast<const char*>(bytes.data() +
                               kUuidOffset), uuid_size);
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
        return fail(error, "extra chest plan path is not valid UTF-8");
    }
    HANDLE file = CreateFileW(temporary_wide.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return fail(error, "extra chest plan temporary file exists or cannot be created");
    }
    DWORD written = 0;
    const bool saved = WriteFile(file, bytes.data(),
                                 static_cast<DWORD>(bytes.size()), &written,
                                 nullptr) != 0 && written == bytes.size() &&
                       FlushFileBuffers(file) != 0;
    const bool closed = CloseHandle(file) != 0;
    if (!saved || !closed) return fail(error, "extra chest plan cannot be synced");
    const DWORD flags = MOVEFILE_WRITE_THROUGH |
        (replace ? MOVEFILE_REPLACE_EXISTING : 0U);
    if (!MoveFileExW(temporary_wide.c_str(), path_wide.c_str(), flags)) {
        return fail(error, "extra chest plan cannot be committed");
    }
#else
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                        0600);
    if (fd < 0) {
        return fail(error, "extra chest plan temporary file exists or cannot be created");
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
    if (!saved || !closed) return fail(error, "extra chest plan cannot be synced");
    if (replace) {
        if (rename(temporary.c_str(), path.c_str()) != 0) {
            return fail(error, "extra chest plan cannot be replaced");
        }
    } else {
        if (!CommitMapJournalNoReplace(temporary, path)) {
            return fail(error, "extra chest plan cannot be created exclusively");
        }
    }
    if (!syncParent(path)) return fail(error, "extra chest plan parent cannot be synced");
#endif
    if (error) error->clear();
    return true;
}

MapExtraChestPlacementLoad loadUnlocked(
        const std::string& map_state_path, uint16_t chest_index,
        MapExtraChestPlacementRecord* output, std::string* error) {
    if (map_state_path.empty() || chest_index == 0U || !output) {
        fail(error, "extra chest plan path, index or output is unavailable");
        return MapExtraChestPlacementLoad::Unsafe;
    }
    *output = {};
    const std::string path =
        MapExtraChestPlacementJournalPath(map_state_path, chest_index);
    bool temporary_exists = false;
    bool plan_exists = false;
    if (!exists(path + ".tmp", &temporary_exists) || temporary_exists ||
        !exists(path, &plan_exists)) {
        fail(error, "extra chest plan has an unresolved temporary file or path error");
        return MapExtraChestPlacementLoad::Unsafe;
    }
    if (!plan_exists) {
        if (error) error->clear();
        return MapExtraChestPlacementLoad::Missing;
    }
    struct stat info {};
    if (stat(path.c_str(), &info) != 0 ||
        info.st_size != static_cast<decltype(info.st_size)>(kDiskBytes)) {
        fail(error, "extra chest plan has an invalid file size");
        return MapExtraChestPlacementLoad::Unsafe;
    }
    std::array<uint8_t, kDiskBytes> bytes{};
    std::ifstream input(path, std::ios::binary);
    if (!input || !input.read(reinterpret_cast<char*>(bytes.data()),
                              bytes.size()) || !decode(bytes, output, error) ||
        output->chest_index != chest_index) {
        if (error && error->empty()) *error = "extra chest plan cannot be read or index mismatches path";
        return MapExtraChestPlacementLoad::Unsafe;
    }
    if (error) error->clear();
    return MapExtraChestPlacementLoad::Loaded;
}

bool replaceExpected(const std::string& map_state_path,
                     const MapExtraChestPlacementRecord& expected,
                     const MapExtraChestPlacementRecord& next,
                     std::string* error) {
    MapExtraChestPlacementRecord current;
    if (loadUnlocked(map_state_path, expected.chest_index, &current, error) !=
            MapExtraChestPlacementLoad::Loaded ||
        !sameRecord(current, expected)) {
        return fail(error, "extra chest plan changed before phase transition");
    }
    return writeAtomic(MapExtraChestPlacementJournalPath(
                           map_state_path, expected.chest_index),
                       encode(next), true, error);
}

bool matchingReadback(const MapChestPosition& position,
                      const MapExtraChestReadback& evidence) {
    return evidence.position == position && evidence.available &&
           evidence.block.name == "minecraft:chest";
}

bool matchingSupportReadback(const MapChestPosition& chest,
                             const MapExtraChestReadback& evidence) {
    return evidence.position ==
               MapChestPosition{chest.x, chest.y - 1, chest.z} &&
           evidence.available && evidence.block.name == "minecraft:stone";
}

bool matchingPlatformPadReadbacks(
        const MapExtraChestPlacementRecord& record,
        const std::array<MapExtraChestReadback, 3>* pads,
        bool expect_air) {
    if (record.platform_corner == 0U) return true;
    std::array<MapChestPosition, 4> cells;
    if (!pads || !BuildMapExtraSupportPlatformCells(
            record.artwork_bounds, record.chest,
            record.platform_corner, &cells)) return false;
    for (size_t index = 0; index < pads->size(); ++index) {
        const auto& readback = (*pads)[index];
        if (!readback.available ||
            !(readback.position == cells[index + 1U]) ||
            !(expect_air ? isAirName(readback.block.name)
                          : readback.block.name == "minecraft:stone")) {
            return false;
        }
    }
    return true;
}

bool uniqueCommandAgainstPrior(const std::string& map_state_path,
                               const MapExtraChestPlacementRecord& target,
                               const std::string& uuid, std::string* error) {
    // A UUID must be unique among durable extra-chest commands in this job.
    // The pair journal drops its historical UUID after confirmation, so the
    // scheduler must also generate globally unique values across the job.
    for (uint16_t index = 1U; index < target.chest_index; ++index) {
        MapExtraChestPlacementRecord prior;
        if (loadUnlocked(map_state_path, index, &prior, error) !=
                MapExtraChestPlacementLoad::Loaded ||
            prior.phase != MapExtraChestPlacementPhase::Confirmed ||
            !sameIdentity(prior, target) || prior.command_uuid == uuid) {
            return fail(error, "extra chest prior journal is absent, stale or reuses UUID");
        }
    }
    return true;
}

bool sameOrFaceAdjacent(const MapChestPosition& a,
                        const MapChestPosition& b) {
    const int64_t dx = static_cast<int64_t>(a.x) - b.x;
    const int64_t dy = static_cast<int64_t>(a.y) - b.y;
    const int64_t dz = static_cast<int64_t>(a.z) - b.z;
    const int64_t distance = (dx < 0 ? -dx : dx) +
                             (dy < 0 ? -dy : dy) +
                             (dz < 0 ? -dz : dz);
    return distance <= 1;
}

}  // namespace

bool ResolveMapTileChestAddress(uint64_t tile_cursor, uint64_t tile_count,
                                MapTileChestAddress* address) noexcept {
    if (!address || tile_count == 0U || tile_count > kMaximumMapTileCount ||
        tile_cursor >= tile_count) return false;
    address->chest_index = static_cast<uint16_t>(tile_cursor / kMapChestSlotCount);
    address->slot = static_cast<uint8_t>(tile_cursor % kMapChestSlotCount);
    return true;
}

std::string MapExtraChestPlacementJournalPath(
        const std::string& map_state_path, uint16_t chest_index) {
    return map_state_path + ".extra_chest_" + std::to_string(chest_index) +
           ".plan";
}

bool BeginMapExtraChestPlacementJournal(
        const std::string& map_state_path,
        const MapExtraChestPlacementRecord& selected,
        std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (map_state_path.empty() ||
        (selected.phase != MapExtraChestPlacementPhase::Selected &&
         selected.phase != MapExtraChestPlacementPhase::SupportSelected) ||
        !validRecord(selected, error)) {
        return fail(error, "extra chest plan requires valid selected phase and path");
    }
    MapExtraChestPlacementRecord existing;
    if (loadUnlocked(map_state_path, selected.chest_index, &existing, error) !=
        MapExtraChestPlacementLoad::Missing) {
        return fail(error, "extra chest plan already exists or is unsafe");
    }
    return writeAtomic(MapExtraChestPlacementJournalPath(
                           map_state_path, selected.chest_index),
                       encode(selected), false, error);
}

MapExtraChestPlacementLoad LoadMapExtraChestPlacementJournal(
        const std::string& map_state_path, uint16_t chest_index,
        MapExtraChestPlacementRecord* output, std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return loadUnlocked(map_state_path, chest_index, output, error);
}

bool ArmMapExtraChestSupportDispatch(
        const std::string& map_state_path,
        const MapExtraChestPlacementRecord& expected_selected,
        const std::string& unique_command_uuid, std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (expected_selected.phase != MapExtraChestPlacementPhase::SupportSelected ||
        !expected_selected.support_created ||
        !validCommandUuid(unique_command_uuid) ||
        !uniqueCommandAgainstPrior(map_state_path, expected_selected,
                                   unique_command_uuid, error)) {
        return fail(error, "extra chest support cannot arm invalid phase or UUID");
    }
    MapExtraChestPlacementRecord next = expected_selected;
    next.phase = MapExtraChestPlacementPhase::SupportDispatchArmed;
    next.command_uuid = unique_command_uuid;
    return replaceExpected(map_state_path, expected_selected, next, error);
}

bool UpgradeMapExtraChestSupportSelectedPlatform(
        const std::string& map_state_path,
        const MapExtraChestPlacementRecord& expected_selected,
        uint8_t platform_corner, std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (expected_selected.phase != MapExtraChestPlacementPhase::SupportSelected ||
        !expected_selected.support_created ||
        expected_selected.platform_corner != 0U ||
        !expected_selected.command_uuid.empty() ||
        platform_corner < 1U || platform_corner > 4U) {
        return fail(error, "extra chest platform upgrade needs unarmed support");
    }
    MapExtraChestPlacementRecord next = expected_selected;
    next.platform_corner = platform_corner;
    if (!validRecord(next, error)) return false;
    return replaceExpected(map_state_path, expected_selected, next, error);
}

bool RearmMapExtraChestSupportDispatchOnce(
        const std::string& map_state_path,
        const MapExtraChestPlacementRecord& expected_armed,
        const std::string& replacement_command_uuid,
        const MapExtraChestReadback& support_readback,
        std::string* error,
        const std::array<MapExtraChestReadback, 3>* pad_readbacks) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const MapChestPosition support{
        expected_armed.chest.x, expected_armed.chest.y - 1,
        expected_armed.chest.z};
    if (expected_armed.phase != MapExtraChestPlacementPhase::SupportDispatchArmed ||
        !expected_armed.support_created || expected_armed.support_retry_used ||
        !validCommandUuid(expected_armed.command_uuid) ||
        !validCommandUuid(replacement_command_uuid) ||
        replacement_command_uuid == expected_armed.command_uuid ||
        !support_readback.available || !(support_readback.position == support) ||
        !isAirName(support_readback.block.name) ||
        !matchingPlatformPadReadbacks(expected_armed, pad_readbacks, true) ||
        !uniqueCommandAgainstPrior(map_state_path, expected_armed,
                                   replacement_command_uuid, error)) {
        return fail(error, "extra chest support retry lacks exact air and unused Armed intent");
    }
    MapExtraChestPlacementRecord next = expected_armed;
    next.support_retry_used = true;
    next.command_uuid = replacement_command_uuid;
    return replaceExpected(map_state_path, expected_armed, next, error);
}

bool BlockMapExtraChestSupportRetry(
        const std::string& map_state_path,
        const MapExtraChestPlacementRecord& expected_armed,
        std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (expected_armed.phase != MapExtraChestPlacementPhase::SupportDispatchArmed ||
        !expected_armed.support_created || expected_armed.support_retry_used ||
        !validCommandUuid(expected_armed.command_uuid)) {
        return fail(error, "extra chest support rejection cannot consume retry");
    }
    MapExtraChestPlacementRecord next = expected_armed;
    next.support_retry_used = true;
    return replaceExpected(map_state_path, expected_armed, next, error);
}

bool ConfirmMapExtraChestSupport(
        const std::string& map_state_path,
        const MapExtraChestPlacementRecord& expected_armed,
        const std::string& ack_command_uuid, MapPairCommandAck ack,
        const MapExtraChestReadback& support_readback,
        std::string* error,
        const std::array<MapExtraChestReadback, 3>* pad_readbacks) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (expected_armed.phase != MapExtraChestPlacementPhase::SupportDispatchArmed ||
        !expected_armed.support_created ||
        expected_armed.command_uuid.empty() ||
        ack_command_uuid != expected_armed.command_uuid ||
        ack != MapPairCommandAck::Accepted ||
        !matchingSupportReadback(expected_armed.chest, support_readback) ||
        !matchingPlatformPadReadbacks(expected_armed, pad_readbacks, false)) {
        return fail(error, "extra chest support lacks matching accepted ACK and exact stone readback");
    }
    MapExtraChestPlacementRecord next = expected_armed;
    next.phase = MapExtraChestPlacementPhase::SupportConfirmed;
    return replaceExpected(map_state_path, expected_armed, next, error);
}

bool ConfirmMapExtraChestSupportByNativeReadback(
        const std::string& map_state_path,
        const MapExtraChestPlacementRecord& expected_armed,
        const MapExtraChestReadback& support_readback,
        std::string* error,
        const std::array<MapExtraChestReadback, 3>* pad_readbacks) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (expected_armed.phase != MapExtraChestPlacementPhase::SupportDispatchArmed ||
        !expected_armed.support_created ||
        !validCommandUuid(expected_armed.command_uuid) ||
        !matchingSupportReadback(expected_armed.chest, support_readback) ||
        !matchingPlatformPadReadbacks(expected_armed, pad_readbacks, false)) {
        return fail(error, "extra chest support native recovery lacks exact stone readback");
    }
    MapExtraChestPlacementRecord next = expected_armed;
    next.phase = MapExtraChestPlacementPhase::SupportConfirmed;
    return replaceExpected(map_state_path, expected_armed, next, error);
}

bool ArmMapExtraChestPlacementDispatch(
        const std::string& map_state_path,
        const MapExtraChestPlacementRecord& expected_selected,
        const std::string& unique_command_uuid, std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool legacy =
        expected_selected.phase == MapExtraChestPlacementPhase::Selected &&
        !expected_selected.support_created;
    const bool new_support =
        expected_selected.phase == MapExtraChestPlacementPhase::SupportConfirmed &&
        expected_selected.support_created;
    if ((!legacy && !new_support) ||
        !validCommandUuid(unique_command_uuid) ||
        (new_support && expected_selected.command_uuid == unique_command_uuid)) {
        return fail(error, "extra chest plan cannot arm invalid phase or UUID");
    }
    if (!uniqueCommandAgainstPrior(map_state_path, expected_selected,
                                   unique_command_uuid, error)) return false;
    MapExtraChestPlacementRecord next = expected_selected;
    next.phase = MapExtraChestPlacementPhase::DispatchArmed;
    next.command_uuid = unique_command_uuid;
    return replaceExpected(map_state_path, expected_selected, next, error);
}

bool ConfirmMapExtraChestPlacement(
        const std::string& map_state_path,
        const MapExtraChestPlacementRecord& expected_armed,
        const std::string& ack_command_uuid, MapPairCommandAck ack,
        const MapExtraChestReadback& chest_readback, std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (expected_armed.phase != MapExtraChestPlacementPhase::DispatchArmed ||
        expected_armed.command_uuid.empty() ||
        ack_command_uuid != expected_armed.command_uuid ||
        ack != MapPairCommandAck::Accepted ||
        !matchingReadback(expected_armed.chest, chest_readback)) {
        return fail(error, "extra chest lacks matching accepted ACK and exact native readback");
    }
    MapExtraChestPlacementRecord next = expected_armed;
    next.phase = MapExtraChestPlacementPhase::Confirmed;
    return replaceExpected(map_state_path, expected_armed, next, error);
}

bool ConfirmMapExtraChestPlacementByNativeReadback(
        const std::string& map_state_path,
        const MapExtraChestPlacementRecord& expected_armed,
        const MapExtraChestReadback& chest_readback,
        std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (expected_armed.phase != MapExtraChestPlacementPhase::DispatchArmed ||
        !validCommandUuid(expected_armed.command_uuid) ||
        !matchingReadback(expected_armed.chest, chest_readback)) {
        return fail(error, "extra chest native recovery lacks exact chest readback");
    }
    MapExtraChestPlacementRecord next = expected_armed;
    next.phase = MapExtraChestPlacementPhase::Confirmed;
    return replaceExpected(map_state_path, expected_armed, next, error);
}

MapExtraChestPlacementResume ClassifyMapExtraChestPlacementResume(
        const MapExtraChestPlacementRecord& record, const std::string& world_id,
        int32_t dimension_id, uint64_t tile_count,
        const BlockBounds& artwork_bounds, uint16_t chest_index) noexcept {
    if (record.world_id.empty() || world_id != record.world_id ||
        dimension_id != record.dimension_id || tile_count != record.tile_count ||
        !sameBounds(artwork_bounds, record.artwork_bounds) ||
        chest_index != record.chest_index) {
        return MapExtraChestPlacementResume::Unsafe;
    }
    switch (record.phase) {
        case MapExtraChestPlacementPhase::SupportSelected:
            return record.support_created
                ? MapExtraChestPlacementResume::SurveySelectedSupport
                : MapExtraChestPlacementResume::Unsafe;
        case MapExtraChestPlacementPhase::SupportDispatchArmed:
            return record.support_created
                ? MapExtraChestPlacementResume::AmbiguousSupportDispatch
                : MapExtraChestPlacementResume::Unsafe;
        case MapExtraChestPlacementPhase::SupportConfirmed:
            return record.support_created
                ? MapExtraChestPlacementResume::VerifySupportBeforeChest
                : MapExtraChestPlacementResume::Unsafe;
        case MapExtraChestPlacementPhase::Selected:
            return !record.support_created
                ? MapExtraChestPlacementResume::SurveySelectedSite
                : MapExtraChestPlacementResume::Unsafe;
        case MapExtraChestPlacementPhase::DispatchArmed:
            return MapExtraChestPlacementResume::AmbiguousDispatchNoResend;
        case MapExtraChestPlacementPhase::Confirmed:
            return MapExtraChestPlacementResume::VerifyConfirmedChest;
    }
    return MapExtraChestPlacementResume::Unsafe;
}

bool ValidateMapExtraChestPlanChain(
        const MapPairPlacementRecord& confirmed_pair,
        const std::vector<MapExtraChestPlacementRecord>& prior_extras,
        const std::vector<MapExtraChestReadback>& ordered_readbacks,
        const MapExtraChestPlacementRecord& target,
        std::string* error) {
    if (!validRecord(target, error) ||
        confirmed_pair.phase != MapPairPlacementPhase::PairConfirmed ||
        confirmed_pair.world_id != target.world_id ||
        confirmed_pair.dimension_id != target.dimension_id ||
        confirmed_pair.tile_count != target.tile_count ||
        !sameBounds(confirmed_pair.artwork_bounds, target.artwork_bounds) ||
        prior_extras.size() != static_cast<size_t>(target.chest_index - 1U) ||
        ordered_readbacks.size() != static_cast<size_t>(target.chest_index) ||
        !matchingReadback(confirmed_pair.pair.chest, ordered_readbacks[0])) {
        return fail(error, "extra chest plan chain lacks confirmed pair or live readbacks");
    }
    if (target.chest == confirmed_pair.pair.anvil) {
        return fail(error, "extra chest plan overlaps the tool anvil");
    }
    std::vector<MapChestPosition> occupied;
    occupied.reserve(ordered_readbacks.size());
    occupied.push_back(confirmed_pair.pair.chest);
    for (size_t i = 0; i < prior_extras.size(); ++i) {
        const MapExtraChestPlacementRecord& prior = prior_extras[i];
        if (!validRecord(prior, error) || !sameIdentity(prior, target) ||
            prior.chest_index != i + 1U ||
            prior.phase != MapExtraChestPlacementPhase::Confirmed ||
            !matchingReadback(prior.chest, ordered_readbacks[i + 1U])) {
            return fail(error, "extra chest plan chain has a gap or stale chest");
        }
        for (const MapChestPosition& position : occupied) {
            if (sameOrFaceAdjacent(position, prior.chest)) {
                return fail(error, "extra chest plan chain contains adjacent chests");
            }
        }
        for (size_t previous = 0; previous < i; ++previous) {
            if (prior.command_uuid == prior_extras[previous].command_uuid) {
                return fail(error, "extra chest plan chain reuses command UUID");
            }
        }
        if (!target.command_uuid.empty() &&
            target.command_uuid == prior.command_uuid) {
            return fail(error, "extra chest target reuses command UUID");
        }
        occupied.push_back(prior.chest);
    }
    for (const MapChestPosition& position : occupied) {
        if (sameOrFaceAdjacent(position, target.chest)) {
            return fail(error, "extra chest target duplicates or adjoins a prior chest");
        }
    }
    if (error) error->clear();
    return true;
}

bool ClearCompletedMapExtraChestPlacementJournal(
        const std::string& map_state_path,
        const MapExtraChestPlacementRecord& expected_confirmed,
        uint64_t checkpoint_tile_cursor, std::string* error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (expected_confirmed.phase != MapExtraChestPlacementPhase::Confirmed ||
        checkpoint_tile_cursor != expected_confirmed.tile_count ||
        !validRecord(expected_confirmed, error)) {
        return fail(error, "extra chest plan cannot be cleared before final map commit");
    }
    MapExtraChestPlacementRecord current;
    if (loadUnlocked(map_state_path, expected_confirmed.chest_index,
                     &current, error) != MapExtraChestPlacementLoad::Loaded ||
        !sameRecord(current, expected_confirmed)) {
        return fail(error, "extra chest plan changed before completion cleanup");
    }
    const std::string path = MapExtraChestPlacementJournalPath(
        map_state_path, expected_confirmed.chest_index);
    if (std::remove(path.c_str()) != 0) {
        return fail(error, "extra chest plan cannot be removed after final commit");
    }
#if !defined(_WIN32)
    if (!syncParent(path)) return fail(error, "extra chest plan removal cannot be synced");
#endif
    if (error) error->clear();
    return true;
}

}  // namespace build_import
