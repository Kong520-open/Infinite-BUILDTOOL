#include "MapChestTransferJournal.h"
#include "MapChestStorageVerification.h"
#include "MapJournalAtomicCommit.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <sys/stat.h>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace build_import {
namespace {

constexpr size_t kWorldBytes = 192U;
constexpr size_t kWorldOffset = 88U;
constexpr size_t kChecksumOffset = kWorldOffset + kWorldBytes;
constexpr size_t kDiskBytes = kChecksumOffset + sizeof(uint64_t);
constexpr uint32_t kDiskVersion = 3U;
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr uint64_t kMaximumSingleChestMaps = 27U;
constexpr uint64_t kMaximumAutomaticMapTiles = 65536U;
constexpr std::array<uint8_t, 8> kMagic{{'M', 'C', 'T', 'J', 'R', 'N', '3', 0}};

std::mutex g_journal_mutex;

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

void put16(std::array<uint8_t, kDiskBytes>* bytes, size_t offset, uint16_t value) {
    for (size_t i = 0; i < 2U; ++i) (*bytes)[offset + i] =
        static_cast<uint8_t>(value >> (i * 8U));
}

void put32(std::array<uint8_t, kDiskBytes>* bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4U; ++i) (*bytes)[offset + i] =
        static_cast<uint8_t>(value >> (i * 8U));
}

void put64(std::array<uint8_t, kDiskBytes>* bytes, size_t offset, uint64_t value) {
    for (size_t i = 0; i < 8U; ++i) (*bytes)[offset + i] =
        static_cast<uint8_t>(value >> (i * 8U));
}

uint16_t get16(const std::array<uint8_t, kDiskBytes>& bytes, size_t offset) {
    uint16_t value = 0;
    for (size_t i = 0; i < 2U; ++i) value |=
        static_cast<uint16_t>(bytes[offset + i]) << (i * 8U);
    return value;
}

uint32_t get32(const std::array<uint8_t, kDiskBytes>& bytes, size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4U; ++i) value |=
        static_cast<uint32_t>(bytes[offset + i]) << (i * 8U);
    return value;
}

uint64_t get64(const std::array<uint8_t, kDiskBytes>& bytes, size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8U; ++i) value |=
        static_cast<uint64_t>(bytes[offset + i]) << (i * 8U);
    return value;
}

int32_t signed32(uint32_t bits) {
    int32_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

int64_t signed64(uint64_t bits) {
    int64_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint64_t hashBytes(const std::array<uint8_t, kDiskBytes>& bytes) {
    uint64_t hash = kFnvOffset;
    for (size_t i = 0; i < kChecksumOffset; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

bool validRecord(const MapChestTransferRecord& record, std::string* error) {
    if (record.world_id.empty() || record.world_id.size() > kWorldBytes ||
        record.world_id.find('\0') != std::string::npos) {
        return fail(error, "map chest journal has invalid world identity");
    }
    if (record.dimension_id < 0 || record.dimension_id > 255 ||
        record.tile_count == 0 || record.tile_count > kMaximumAutomaticMapTiles ||
        record.tile_cursor >= record.tile_count ||
        record.chest_slot >= kMaximumSingleChestMaps ||
        static_cast<uint64_t>(record.chest_slot) !=
            record.tile_cursor % kMaximumSingleChestMaps ||
        static_cast<uint64_t>(record.chest_index) !=
            record.tile_cursor / kMaximumSingleChestMaps) {
        return fail(error, "map chest journal has invalid tile or dimension bounds");
    }
    constexpr int32_t lo = std::numeric_limits<int32_t>::min() + 1;
    constexpr int32_t hi = std::numeric_limits<int32_t>::max() - 1;
    if (record.chest_x <= lo || record.chest_x >= hi ||
        record.chest_y <= lo || record.chest_y >= hi ||
        record.chest_z <= lo || record.chest_z >= hi ||
        record.source_runtime_item_id <= 0 ||
        record.source_network_stack_id <= 0 || record.source_map_uuid == -1 ||
        record.pre_send_capture_token == 0) {
        return fail(error, "map chest journal has invalid position or source identity");
    }
    switch (record.phase) {
        case MapChestTransferPhase::Prepared:
            if (record.request_id != 0) return fail(error, "prepared map chest journal has request ID");
            break;
        case MapChestTransferPhase::DispatchArmed:
        case MapChestTransferPhase::ResponseAccepted:
        case MapChestTransferPhase::ReopenConfirmed:
        case MapChestTransferPhase::InventoryConfirmed:
        case MapChestTransferPhase::AcceptedAndClosed:
            if (record.request_id >= 0 || record.request_id % 2 == 0) {
                return fail(error, "armed map chest journal has invalid negative odd request ID");
            }
            break;
        default:
            return fail(error, "map chest journal has unknown phase");
    }
    return true;
}

std::array<uint8_t, kDiskBytes> encode(const MapChestTransferRecord& record) {
    std::array<uint8_t, kDiskBytes> bytes{};
    std::memcpy(bytes.data(), kMagic.data(), kMagic.size());
    put32(&bytes, 8U, kDiskVersion);
    put32(&bytes, 12U, static_cast<uint32_t>(bytes.size()));
    put32(&bytes, 16U, static_cast<uint32_t>(record.phase));
    put32(&bytes, 20U, static_cast<uint32_t>(record.source_runtime_item_id));
    put64(&bytes, 24U, record.tile_cursor);
    put64(&bytes, 32U, record.tile_count);
    put32(&bytes, 40U, static_cast<uint32_t>(record.dimension_id));
    put32(&bytes, 44U, static_cast<uint32_t>(record.chest_x));
    put32(&bytes, 48U, static_cast<uint32_t>(record.chest_y));
    put32(&bytes, 52U, static_cast<uint32_t>(record.chest_z));
    put32(&bytes, 56U, record.chest_slot);
    put32(&bytes, 60U, static_cast<uint32_t>(record.source_network_stack_id));
    put64(&bytes, 64U, static_cast<uint64_t>(record.source_map_uuid));
    put64(&bytes, 72U, record.pre_send_capture_token);
    put32(&bytes, 80U, static_cast<uint32_t>(record.request_id));
    put16(&bytes, 84U, static_cast<uint16_t>(record.world_id.size()));
    put16(&bytes, 86U, record.chest_index);
    std::memcpy(bytes.data() + kWorldOffset, record.world_id.data(),
                record.world_id.size());
    put64(&bytes, kChecksumOffset, hashBytes(bytes));
    return bytes;
}

bool decode(const std::array<uint8_t, kDiskBytes>& bytes,
            MapChestTransferRecord* output, std::string* error) {
    if (!output || std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0 ||
        get32(bytes, 8U) != kDiskVersion || get32(bytes, 12U) != kDiskBytes ||
        get64(bytes, kChecksumOffset) != hashBytes(bytes)) {
        return fail(error, "map chest journal header or checksum is invalid");
    }
    const uint16_t world_length = get16(bytes, 84U);
    if (world_length == 0 || world_length > kWorldBytes) {
        return fail(error, "map chest journal world identity length is invalid");
    }
    for (size_t i = kWorldOffset + world_length; i < kChecksumOffset; ++i) {
        if (bytes[i] != 0) return fail(error, "map chest journal reserved bytes are invalid");
    }
    MapChestTransferRecord record;
    record.world_id.assign(reinterpret_cast<const char*>(bytes.data() + kWorldOffset),
                           world_length);
    record.phase = static_cast<MapChestTransferPhase>(get32(bytes, 16U));
    record.source_runtime_item_id = signed32(get32(bytes, 20U));
    record.tile_cursor = get64(bytes, 24U);
    record.tile_count = get64(bytes, 32U);
    record.dimension_id = signed32(get32(bytes, 40U));
    record.chest_x = signed32(get32(bytes, 44U));
    record.chest_y = signed32(get32(bytes, 48U));
    record.chest_z = signed32(get32(bytes, 52U));
    const uint32_t slot = get32(bytes, 56U);
    if (slot > std::numeric_limits<uint8_t>::max()) {
        return fail(error, "map chest journal destination slot is invalid");
    }
    record.chest_slot = static_cast<uint8_t>(slot);
    record.chest_index = get16(bytes, 86U);
    record.source_network_stack_id = signed32(get32(bytes, 60U));
    record.source_map_uuid = signed64(get64(bytes, 64U));
    record.pre_send_capture_token = get64(bytes, 72U);
    record.request_id = signed32(get32(bytes, 80U));
    if (!validRecord(record, error)) return false;
    *output = std::move(record);
    return true;
}

bool pathExists(const std::string& path, bool* exists) {
    struct stat status {};
    if (stat(path.c_str(), &status) == 0) {
        *exists = true;
        return true;
    }
    *exists = false;
    return errno == ENOENT;
}

MapChestJournalLoad loadUnlocked(const std::string& map_state_path,
                                 MapChestTransferRecord* output,
                                 std::string* error) {
    if (!output || map_state_path.empty()) {
        fail(error, "map chest journal path or output is unavailable");
        return MapChestJournalLoad::Unsafe;
    }
    *output = {};
    const std::string path = MapChestTransferJournalPath(map_state_path);
    bool temporary_exists = false;
    bool journal_exists = false;
    if (!pathExists(path + ".tmp", &temporary_exists) || temporary_exists ||
        !pathExists(path, &journal_exists)) {
        fail(error, "map chest journal has an unresolved temporary file or path error");
        return MapChestJournalLoad::Unsafe;
    }
    if (!journal_exists) {
        if (error) error->clear();
        return MapChestJournalLoad::Missing;
    }
    struct stat status {};
    if (stat(path.c_str(), &status) != 0 ||
        status.st_size != static_cast<decltype(status.st_size)>(kDiskBytes)) {
        fail(error, "map chest journal size is invalid");
        return MapChestJournalLoad::Unsafe;
    }
    std::array<uint8_t, kDiskBytes> bytes{};
    std::ifstream input(path, std::ios::binary);
    char trailing = 0;
    if (!input || !input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) ||
        input.read(&trailing, 1) || !decode(bytes, output, error)) {
        if (error && error->empty()) *error = "map chest journal cannot be read";
        return MapChestJournalLoad::Unsafe;
    }
    if (error) error->clear();
    return MapChestJournalLoad::Loaded;
}

#if defined(_WIN32)
std::wstring utf8Wide(const std::string& value) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           value.c_str(), -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), -1,
                            wide.data(), length) != length) return {};
    return wide;
}
#else
bool syncParent(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const std::string parent = slash == std::string::npos ? "." :
        (slash == 0 ? "/" : path.substr(0, slash));
    const int fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool okay = fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    return okay && closed;
}
#endif

bool writeAtomic(const std::string& path,
                 const std::array<uint8_t, kDiskBytes>& bytes,
                 bool replace, std::string* error) {
    const std::string temporary = path + ".tmp";
#if defined(_WIN32)
    const int fd = _open(temporary.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                         _S_IREAD | _S_IWRITE);
#else
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                        0600);
#endif
    if (fd < 0) return fail(error, "cannot create temporary map chest journal");
    size_t written = 0;
    bool okay = true;
    while (written < bytes.size()) {
#if defined(_WIN32)
        const int count = _write(fd, bytes.data() + written,
                                 static_cast<unsigned int>(bytes.size() - written));
#else
        const ssize_t count = write(fd, bytes.data() + written,
                                    bytes.size() - written);
#endif
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { okay = false; break; }
        written += static_cast<size_t>(count);
    }
#if defined(_WIN32)
    if (okay && _commit(fd) != 0) okay = false;
    if (_close(fd) != 0) okay = false;
#else
    if (okay && fsync(fd) != 0) okay = false;
    if (close(fd) != 0) okay = false;
#endif
    if (!okay) {
        std::remove(temporary.c_str());
        return fail(error, "cannot durably write map chest journal");
    }
#if defined(_WIN32)
    const std::wstring wide_temporary = utf8Wide(temporary);
    const std::wstring wide_path = utf8Wide(path);
    const DWORD flags = MOVEFILE_WRITE_THROUGH |
        (replace ? MOVEFILE_REPLACE_EXISTING : 0U);
    if (wide_temporary.empty() || wide_path.empty() ||
        !MoveFileExW(wide_temporary.c_str(), wide_path.c_str(), flags)) {
        std::remove(temporary.c_str());
        return fail(error, "cannot finalize map chest journal");
    }
#else
    if ((replace ? std::rename(temporary.c_str(), path.c_str()) == 0 :
                   CommitMapJournalNoReplace(temporary, path)) == false) {
        std::remove(temporary.c_str());
        return fail(error, "cannot finalize map chest journal");
    }
    if (!syncParent(path)) {
        return fail(error, "cannot sync map chest journal directory");
    }
#endif
    if (error) error->clear();
    return true;
}

bool sameRecord(const MapChestTransferRecord& a,
                const MapChestTransferRecord& b) {
    return a.world_id == b.world_id && a.dimension_id == b.dimension_id &&
        a.tile_cursor == b.tile_cursor && a.tile_count == b.tile_count &&
        a.chest_x == b.chest_x && a.chest_y == b.chest_y &&
        a.chest_z == b.chest_z && a.chest_index == b.chest_index &&
        a.chest_slot == b.chest_slot &&
        a.source_runtime_item_id == b.source_runtime_item_id &&
        a.source_network_stack_id == b.source_network_stack_id &&
        a.source_map_uuid == b.source_map_uuid &&
        a.pre_send_capture_token == b.pre_send_capture_token &&
        a.request_id == b.request_id && a.phase == b.phase;
}

bool sameIdentityAndRequest(const MapChestTransferRecord& a,
                            const MapChestTransferRecord& b) {
    MapChestTransferRecord normalized = a;
    normalized.phase = b.phase;
    return sameRecord(normalized, b);
}

bool loadMatching(const std::string& map_state_path,
                  const MapChestTransferRecord& expected,
                  MapChestTransferRecord* actual, std::string* error) {
    if (loadUnlocked(map_state_path, actual, error) != MapChestJournalLoad::Loaded ||
        !sameRecord(*actual, expected)) {
        return fail(error, "map chest journal no longer matches expected state");
    }
    return true;
}

} // namespace

std::string MapChestTransferJournalPath(const std::string& map_state_path) {
    return map_state_path + ".chest_pending";
}

bool BeginMapChestTransferJournal(const std::string& map_state_path,
                                  const MapChestTransferRecord& prepared,
                                  std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (prepared.phase != MapChestTransferPhase::Prepared ||
        !validRecord(prepared, error)) return false;
    MapChestTransferRecord existing;
    if (loadUnlocked(map_state_path, &existing, error) != MapChestJournalLoad::Missing) {
        return fail(error, "a previous map chest transfer is unresolved");
    }
    return writeAtomic(MapChestTransferJournalPath(map_state_path),
                       encode(prepared), false, error);
}

MapChestJournalLoad LoadMapChestTransferJournal(
    const std::string& map_state_path, MapChestTransferRecord* output,
    std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    return loadUnlocked(map_state_path, output, error);
}

bool RefreshPreparedMapChestTransferJournal(
    const std::string& map_state_path,
    const MapChestTransferRecord& expected_prepared,
    int32_t fresh_source_network_stack_id, uint64_t fresh_empty_capture_token,
    std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (expected_prepared.phase != MapChestTransferPhase::Prepared ||
        fresh_source_network_stack_id <= 0 || fresh_empty_capture_token == 0) {
        return fail(error, "fresh map chest preflight identity is invalid");
    }
    MapChestTransferRecord actual;
    if (!loadMatching(map_state_path, expected_prepared, &actual, error)) return false;
    actual.source_network_stack_id = fresh_source_network_stack_id;
    actual.pre_send_capture_token = fresh_empty_capture_token;
    if (!validRecord(actual, error)) return false;
    return writeAtomic(MapChestTransferJournalPath(map_state_path),
                       encode(actual), true, error);
}

bool ArmMapChestTransferDispatch(const std::string& map_state_path,
                                 const MapChestTransferRecord& expected_prepared,
                                 int32_t request_id, std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (expected_prepared.phase != MapChestTransferPhase::Prepared ||
        request_id >= 0 || request_id % 2 == 0) {
        return fail(error, "map chest send cannot be armed without reserved request ID");
    }
    MapChestTransferRecord actual;
    if (!loadMatching(map_state_path, expected_prepared, &actual, error)) return false;
    actual.phase = MapChestTransferPhase::DispatchArmed;
    actual.request_id = request_id;
    return writeAtomic(MapChestTransferJournalPath(map_state_path),
                       encode(actual), true, error);
}

bool NoteMapChestTransferAccepted(const std::string& map_state_path,
                                  const MapChestTransferRecord& expected_armed,
                                  int32_t request_id, std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (expected_armed.phase != MapChestTransferPhase::DispatchArmed ||
        request_id == 0 || expected_armed.request_id != request_id) {
        return fail(error, "map chest response request ID does not match armed send");
    }
    MapChestTransferRecord actual;
    if (!loadMatching(map_state_path, expected_armed, &actual, error)) return false;
    actual.phase = MapChestTransferPhase::ResponseAccepted;
    return writeAtomic(MapChestTransferJournalPath(map_state_path),
                       encode(actual), true, error);
}

bool ConfirmMapChestTransferReopen(const std::string& map_state_path,
                                   const MapChestTransferRecord& expected,
                                   const ContainerCaptureResult& reopen,
                                   std::string_view expected_name,
                                   MapItemNameSource expected_name_source,
                                   std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (expected.phase != MapChestTransferPhase::DispatchArmed &&
        expected.phase != MapChestTransferPhase::ResponseAccepted &&
        expected.phase != MapChestTransferPhase::ReopenConfirmed) {
        return fail(error, "map chest reopen cannot confirm an unarmed transfer");
    }
    MapChestTransferRecord actual;
    if (loadUnlocked(map_state_path, &actual, error) != MapChestJournalLoad::Loaded ||
        !sameIdentityAndRequest(expected, actual) ||
        (actual.phase != MapChestTransferPhase::DispatchArmed &&
         actual.phase != MapChestTransferPhase::ResponseAccepted &&
         actual.phase != MapChestTransferPhase::ReopenConfirmed)) {
        return fail(error, "map chest reopen does not match pending request");
    }
    if (reopen.token == actual.pre_send_capture_token ||
        VerifyNamedFilledMapInReopenedChest(
            reopen, reopen.token, actual.chest_x, actual.chest_y,
            actual.chest_z, actual.chest_slot, actual.source_map_uuid,
            actual.source_runtime_item_id, expected_name, expected_name_source) !=
            NamedFilledMapChestReopenResult::Confirmed) {
        return fail(error, "reopened chest lacks a fresh matching map UUID and title proof");
    }
    if (actual.phase == MapChestTransferPhase::ReopenConfirmed) {
        if (error) error->clear();
        return true;
    }
    actual.phase = MapChestTransferPhase::ReopenConfirmed;
    return writeAtomic(MapChestTransferJournalPath(map_state_path),
                       encode(actual), true, error);
}

bool ConfirmMapChestTransferInventoryAbsent(
        const std::string& map_state_path,
        const MapChestTransferRecord& expected_accepted,
        const MapChestInventoryAbsenceProof& proof, std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (expected_accepted.phase != MapChestTransferPhase::ResponseAccepted ||
        proof.accepted_request_id != expected_accepted.request_id ||
        proof.absent_map_uuid != expected_accepted.source_map_uuid ||
        !proof.complete_native_inventory_scan ||
        !proof.expected_uuid_absent) {
        return fail(error,
            "map chest inventory absence lacks an accepted matching Place and complete native scan");
    }
    MapChestTransferRecord actual;
    if (!loadMatching(map_state_path, expected_accepted, &actual, error)) {
        return false;
    }
    actual.phase = MapChestTransferPhase::InventoryConfirmed;
    return writeAtomic(MapChestTransferJournalPath(map_state_path),
                       encode(actual), true, error);
}

bool ConfirmMapChestTransferAcceptedClose(
        const std::string& map_state_path,
        const MapChestTransferRecord& expected_accepted,
        const MapChestAcceptedCloseProof& proof, std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (expected_accepted.phase != MapChestTransferPhase::ResponseAccepted ||
        expected_accepted.request_id >= 0 ||
        proof.accepted_request_id != expected_accepted.request_id ||
        !proof.original_chest_window_settled) {
        return fail(error,
            "map chest completion needs accepted Place and original chest settlement");
    }
    MapChestTransferRecord actual;
    if (!loadMatching(map_state_path, expected_accepted, &actual, error)) {
        return false;
    }
    actual.phase = MapChestTransferPhase::AcceptedAndClosed;
    return writeAtomic(MapChestTransferJournalPath(map_state_path),
                       encode(actual), true, error);
}

MapChestRecoveryAction ClassifyMapChestTransferRecovery(
    const MapChestTransferRecord& journal, const std::string& world_id,
    int32_t dimension_id, uint64_t checkpoint_tile_cursor,
    uint64_t expected_tile_count, int32_t chest_x, int32_t chest_y,
    int32_t chest_z, uint8_t expected_slot) noexcept {
    if (!validRecord(journal, nullptr) || journal.world_id != world_id ||
        journal.dimension_id != dimension_id ||
        journal.tile_count != expected_tile_count ||
        journal.chest_x != chest_x || journal.chest_y != chest_y ||
        journal.chest_z != chest_z || journal.chest_slot != expected_slot) {
        return MapChestRecoveryAction::Unsafe;
    }
    if (checkpoint_tile_cursor == journal.tile_cursor) {
        switch (journal.phase) {
            case MapChestTransferPhase::Prepared:
                return MapChestRecoveryAction::FreshPreflightRequired;
            case MapChestTransferPhase::DispatchArmed:
            case MapChestTransferPhase::ResponseAccepted:
                return MapChestRecoveryAction::ReopenChestNoResend;
            case MapChestTransferPhase::ReopenConfirmed:
            case MapChestTransferPhase::InventoryConfirmed:
            case MapChestTransferPhase::AcceptedAndClosed:
                return MapChestRecoveryAction::CommitTileCursorNoResend;
            default:
                return MapChestRecoveryAction::Unsafe;
        }
    }
    if (checkpoint_tile_cursor == journal.tile_cursor + 1U &&
        (journal.phase == MapChestTransferPhase::ReopenConfirmed ||
         journal.phase == MapChestTransferPhase::InventoryConfirmed ||
         journal.phase == MapChestTransferPhase::AcceptedAndClosed)) {
        return MapChestRecoveryAction::ClearJournalAfterCursorCommit;
    }
    return MapChestRecoveryAction::Unsafe;
}

bool ClearCommittedMapChestTransferJournal(
    const std::string& map_state_path,
    const MapChestTransferRecord& expected_confirmed,
    uint64_t checkpoint_tile_cursor, std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if ((expected_confirmed.phase != MapChestTransferPhase::ReopenConfirmed &&
         expected_confirmed.phase != MapChestTransferPhase::InventoryConfirmed &&
         expected_confirmed.phase != MapChestTransferPhase::AcceptedAndClosed) ||
        checkpoint_tile_cursor != expected_confirmed.tile_cursor + 1U) {
        return fail(error, "map chest journal cannot clear before cursor commit");
    }
    MapChestTransferRecord actual;
    if (!loadMatching(map_state_path, expected_confirmed, &actual, error)) return false;
    const std::string path = MapChestTransferJournalPath(map_state_path);
    if (std::remove(path.c_str()) != 0) {
        return fail(error, "cannot remove committed map chest journal");
    }
#if !defined(_WIN32)
    if (!syncParent(path)) return fail(error, "cannot sync cleared map chest journal");
#endif
    if (error) error->clear();
    return true;
}

} // namespace build_import
