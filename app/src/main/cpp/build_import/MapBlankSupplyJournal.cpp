#include "MapBlankSupplyJournal.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace build_import {
namespace {

constexpr uint32_t kMagic = 0x3153424DU;  // MBS1
constexpr uint32_t kVersion = 1U;

struct DiskV1 {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    int32_t dimension_id = 0;
    uint32_t empty_maps_before = 0;
    uint64_t tile_cursor = 0;
    uint64_t tile_count = 0;
    char world_id[384]{};
    char rpc_uuid[128]{};
    uint64_t checksum = 0;
};

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

bool exists(const std::string& path) {
    struct stat status {};
    return stat(path.c_str(), &status) == 0;
}

bool valid(const MapBlankSupplyIntent& intent) {
    return !intent.world_id.empty() &&
        intent.world_id.size() < sizeof(DiskV1::world_id) &&
        !intent.rpc_uuid.empty() &&
        intent.rpc_uuid.size() < sizeof(DiskV1::rpc_uuid) &&
        intent.tile_count > 0 && intent.tile_cursor < intent.tile_count &&
        intent.empty_maps_before == 0;
}

uint64_t checksum(const DiskV1& disk) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&disk);
    uint64_t result = 1469598103934665603ULL;
    for (size_t index = 0; index < offsetof(DiskV1, checksum); ++index) {
        result ^= bytes[index];
        result *= 1099511628211ULL;
    }
    return result;
}

bool encode(const MapBlankSupplyIntent& intent, DiskV1* disk) {
    if (!disk || !valid(intent)) return false;
    *disk = DiskV1{};
    disk->dimension_id = intent.dimension_id;
    disk->empty_maps_before = intent.empty_maps_before;
    disk->tile_cursor = intent.tile_cursor;
    disk->tile_count = intent.tile_count;
    std::memcpy(disk->world_id, intent.world_id.data(), intent.world_id.size());
    std::memcpy(disk->rpc_uuid, intent.rpc_uuid.data(), intent.rpc_uuid.size());
    disk->checksum = checksum(*disk);
    return true;
}

bool decode(const DiskV1& disk, MapBlankSupplyIntent* intent) {
    if (!intent || disk.magic != kMagic || disk.version != kVersion ||
        disk.checksum != checksum(disk)) return false;
    const auto* world_end = static_cast<const char*>(
        std::memchr(disk.world_id, 0, sizeof(disk.world_id)));
    const auto* uuid_end = static_cast<const char*>(
        std::memchr(disk.rpc_uuid, 0, sizeof(disk.rpc_uuid)));
    if (!world_end || !uuid_end) return false;
    MapBlankSupplyIntent parsed;
    parsed.world_id.assign(disk.world_id, world_end);
    parsed.dimension_id = disk.dimension_id;
    parsed.tile_cursor = disk.tile_cursor;
    parsed.tile_count = disk.tile_count;
    parsed.empty_maps_before = disk.empty_maps_before;
    parsed.rpc_uuid.assign(disk.rpc_uuid, uuid_end);
    if (!valid(parsed)) return false;
    *intent = std::move(parsed);
    return true;
}

bool readExact(const std::string& path, MapBlankSupplyIntent* intent) {
    DiskV1 disk;
    std::ifstream input(path, std::ios::binary);
    char trailing = 0;
    return input && input.read(reinterpret_cast<char*>(&disk), sizeof(disk)) &&
        !input.read(&trailing, 1) && decode(disk, intent);
}

#if defined(_WIN32)
std::wstring widePath(const std::string& path) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         path.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring output(static_cast<size_t>(size), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               path.c_str(), -1, output.data(), size) == size
        ? output : std::wstring{};
}
#else
bool syncParent(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const std::string parent = slash == std::string::npos ? "." :
        slash == 0 ? "/" : path.substr(0, slash);
    const int fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool synced = fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    return synced && closed;
}
#endif

bool commitRename(const std::string& temporary, const std::string& path,
                  std::string* error) {
#if defined(_WIN32)
    const std::wstring from = widePath(temporary);
    const std::wstring to = widePath(path);
    if (from.empty() || to.empty() ||
        !MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return fail(error, "cannot commit blank-map supply intent");
    }
#else
    if (std::rename(temporary.c_str(), path.c_str()) != 0 ||
        !syncParent(path)) {
        return fail(error, "cannot durably commit blank-map supply intent");
    }
#endif
    return true;
}

}  // namespace

std::string MapBlankSupplyJournalPath(const std::string& map_state_path) {
    return map_state_path + ".blank-supply.pending";
}

bool ArmMapBlankSupply(const std::string& map_state_path,
                       const MapBlankSupplyIntent& intent,
                       std::string* error) {
    if (error) error->clear();
    DiskV1 disk;
    if (map_state_path.empty() || !encode(intent, &disk)) {
        return fail(error, "blank-map supply intent is invalid");
    }
    const std::string path = MapBlankSupplyJournalPath(map_state_path);
    if (exists(path)) return fail(error, "blank-map supply is already pending");
    const std::string temporary = path + ".tmp";
#if defined(_WIN32)
    const int fd = _open(temporary.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                         _S_IREAD | _S_IWRITE);
#else
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                        0600);
#endif
    if (fd < 0) {
        // A leftover partial temp is not proof that dispatch did or did not
        // occur. Refuse to overwrite it, even after a restart.
        return fail(error, "blank-map supply temporary intent already exists or cannot be created");
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(&disk);
    size_t written = 0;
    bool okay = true;
    while (written < sizeof(disk)) {
#if defined(_WIN32)
        const int amount = _write(fd, bytes + written,
                                  static_cast<unsigned int>(sizeof(disk) - written));
#else
        const ssize_t amount = write(fd, bytes + written, sizeof(disk) - written);
#endif
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) { okay = false; break; }
        written += static_cast<size_t>(amount);
    }
#if defined(_WIN32)
    if (okay && _commit(fd) != 0) okay = false;
    if (_close(fd) != 0) okay = false;
#else
    if (okay && fsync(fd) != 0) okay = false;
    if (close(fd) != 0) okay = false;
#endif
    if (!okay) return fail(error, "cannot durably write blank-map supply intent");
    return commitRename(temporary, path, error);
}

MapBlankSupplyLoad LoadMapBlankSupply(
    const std::string& map_state_path, MapBlankSupplyIntent* intent,
    std::string* error) {
    if (error) error->clear();
    if (map_state_path.empty() || !intent) {
        fail(error, "blank-map supply path or output is invalid");
        return MapBlankSupplyLoad::Unsafe;
    }
    const std::string path = MapBlankSupplyJournalPath(map_state_path);
    if (!exists(path)) {
        if (exists(path + ".tmp")) {
            fail(error, "incomplete blank-map supply intent requires inspection");
            return MapBlankSupplyLoad::Unsafe;
        }
        return MapBlankSupplyLoad::Missing;
    }
    if (!readExact(path, intent)) {
        fail(error, "blank-map supply intent is corrupt");
        return MapBlankSupplyLoad::Unsafe;
    }
    return MapBlankSupplyLoad::Loaded;
}

bool MapBlankSupplyMatches(const MapBlankSupplyIntent& intent,
                           const std::string& world_id, int32_t dimension_id,
                           uint64_t tile_cursor, uint64_t tile_count) noexcept {
    return valid(intent) && intent.world_id == world_id &&
        intent.dimension_id == dimension_id &&
        intent.tile_cursor == tile_cursor && intent.tile_count == tile_count;
}

bool ClearMapBlankSupply(const std::string& map_state_path,
                         const MapBlankSupplyIntent& expected,
                         std::string* error) {
    if (error) error->clear();
    MapBlankSupplyIntent actual;
    if (LoadMapBlankSupply(map_state_path, &actual, error) !=
            MapBlankSupplyLoad::Loaded ||
        actual.world_id != expected.world_id ||
        actual.dimension_id != expected.dimension_id ||
        actual.tile_cursor != expected.tile_cursor ||
        actual.tile_count != expected.tile_count ||
        actual.empty_maps_before != expected.empty_maps_before ||
        actual.rpc_uuid != expected.rpc_uuid) {
        return fail(error, "blank-map supply intent changed before cleanup");
    }
    const std::string path = MapBlankSupplyJournalPath(map_state_path);
#if defined(_WIN32)
    if (_unlink(path.c_str()) != 0) {
#else
    if (unlink(path.c_str()) != 0 || !syncParent(path)) {
#endif
        return fail(error, "cannot durably clear blank-map supply intent");
    }
    return true;
}

}  // namespace build_import
