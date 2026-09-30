#include "MapAnvilRenameJournal.h"

#include "MapJournalAtomicCommit.h"
#include "MapTileNaming.h"

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
constexpr size_t kTitleBytes = 32U;
// Version 4 records a pre-click no-resend barrier, the anvil input net ID,
// and the native packet's actual output hotbar slot. Older sidecars cannot
// distinguish an asynchronously queued click from an unsent click and fail
// closed instead of being upgraded.
constexpr size_t kWorldOffset = 164U;
constexpr size_t kTitleOffset = kWorldOffset + kWorldBytes;
constexpr size_t kChecksumOffset = kTitleOffset + kTitleBytes;
constexpr size_t kDiskBytes = kChecksumOffset + sizeof(uint64_t);
constexpr uint32_t kDiskVersion = 4U;
constexpr std::array<uint8_t, 8> kMagic{{'M', 'A', 'R', 'J', 'R', 'N', '4', 0}};
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
std::mutex g_journal_mutex;

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

bool negativeOdd(int32_t id) noexcept {
    return id < 0 && (static_cast<uint32_t>(id) & 1U) != 0U;
}

bool emptyCorrelation(const MapAnvilResponseCorrelation& value) noexcept {
    return value.session_generation == 0U &&
        value.response_generation_before_send == 0U &&
        value.window_token == 0U && value.window_id == 0U &&
        value.source_hotbar_slot == -1;
}

bool validCorrelation(const MapAnvilResponseCorrelation& value,
                      bool input) noexcept {
    return value.session_generation != 0U && value.window_token != 0U &&
        value.window_id != 0U && value.window_id != 0xFFU &&
        (input ? value.source_hotbar_slot >= 0 &&
                 value.source_hotbar_slot <= 8
               : value.source_hotbar_slot == -1);
}

bool validRecord(const MapAnvilRenameRecord& record, std::string* error) {
    if (record.world_id.empty() || record.world_id.size() > kWorldBytes ||
        record.world_id.find('\0') != std::string::npos ||
        record.dimension_id < 0 || record.dimension_id > 255 ||
        record.columns == 0U || record.rows == 0U ||
        static_cast<uint64_t>(record.columns) * record.rows != record.tile_count ||
        record.tile_count == 0U || record.tile_count > 65536U ||
        record.tile_cursor >= record.tile_count) {
        return fail(error, "map anvil journal has invalid world or tile identity");
    }
    constexpr int32_t lo = std::numeric_limits<int32_t>::min() + 1;
    constexpr int32_t hi = std::numeric_limits<int32_t>::max() - 1;
    if (record.anvil_x <= lo || record.anvil_x >= hi ||
        record.anvil_y <= lo || record.anvil_y >= hi ||
        record.anvil_z <= lo || record.anvil_z >= hi ||
        record.map_runtime_item_id <= 0 || record.map_uuid == -1 ||
        record.input_source_network_stack_id <= 0) {
        return fail(error, "map anvil journal has invalid anvil or item identity");
    }
    std::string generated;
    if (!FormatMapTileName(record.tile_cursor, record.columns, record.rows,
                           &generated) ||
        record.expected_title.empty() ||
        record.expected_title.size() > kTitleBytes ||
        record.expected_title != generated) {
        return fail(error, "map anvil journal title does not match its tile");
    }
    switch (record.phase) {
        case MapAnvilRenamePhase::Prepared:
            if (record.input_request_id || record.craft_request_id ||
                record.craft_input_network_stack_id ||
                record.craft_destination_hotbar_slot != -1 ||
                record.craft_accepted_output_network_stack_id ||
                record.renamed_network_stack_id ||
                !emptyCorrelation(record.input_response) ||
                !emptyCorrelation(record.craft_response)) break;
            return true;
        case MapAnvilRenamePhase::InputDispatchArmed:
        case MapAnvilRenamePhase::InputResponseAccepted:
        case MapAnvilRenamePhase::InputConfirmed:
            if (!negativeOdd(record.input_request_id) ||
                record.craft_request_id ||
                record.craft_input_network_stack_id ||
                record.craft_destination_hotbar_slot != -1 ||
                record.craft_accepted_output_network_stack_id ||
                record.renamed_network_stack_id ||
                !validCorrelation(record.input_response, true) ||
                !emptyCorrelation(record.craft_response)) break;
            return true;
        case MapAnvilRenamePhase::CraftClickArmed:
            if (!negativeOdd(record.input_request_id) ||
                record.craft_request_id ||
                record.craft_input_network_stack_id <= 0 ||
                record.craft_destination_hotbar_slot != -1 ||
                record.craft_accepted_output_network_stack_id ||
                record.renamed_network_stack_id ||
                !validCorrelation(record.input_response, true) ||
                !emptyCorrelation(record.craft_response)) break;
            return true;
        case MapAnvilRenamePhase::CraftDispatchArmed:
            if (!negativeOdd(record.input_request_id) ||
                !negativeOdd(record.craft_request_id) ||
                record.input_request_id == record.craft_request_id ||
                record.craft_input_network_stack_id <= 0 ||
                record.craft_destination_hotbar_slot < 0 ||
                record.craft_destination_hotbar_slot > 8 ||
                record.craft_accepted_output_network_stack_id ||
                record.renamed_network_stack_id ||
                !validCorrelation(record.input_response, true) ||
                !validCorrelation(record.craft_response, false) ||
                record.craft_response.session_generation !=
                    record.input_response.session_generation ||
                record.craft_response.window_token !=
                    record.input_response.window_token ||
                record.craft_response.window_id !=
                    record.input_response.window_id) break;
            return true;
        case MapAnvilRenamePhase::CraftResponseAccepted:
            if (!negativeOdd(record.input_request_id) ||
                !negativeOdd(record.craft_request_id) ||
                record.input_request_id == record.craft_request_id ||
                record.craft_input_network_stack_id <= 0 ||
                record.craft_destination_hotbar_slot < 0 ||
                record.craft_destination_hotbar_slot > 8 ||
                record.craft_accepted_output_network_stack_id <= 0 ||
                record.renamed_network_stack_id ||
                !validCorrelation(record.input_response, true) ||
                !validCorrelation(record.craft_response, false) ||
                record.craft_response.session_generation !=
                    record.input_response.session_generation ||
                record.craft_response.window_token !=
                    record.input_response.window_token ||
                record.craft_response.window_id !=
                    record.input_response.window_id) break;
            return true;
        case MapAnvilRenamePhase::RenamedMapConfirmed:
            if (!negativeOdd(record.input_request_id) ||
                !negativeOdd(record.craft_request_id) ||
                record.input_request_id == record.craft_request_id ||
                record.craft_input_network_stack_id <= 0 ||
                record.craft_destination_hotbar_slot < 0 ||
                record.craft_destination_hotbar_slot > 8 ||
                record.craft_accepted_output_network_stack_id <= 0 ||
                record.renamed_network_stack_id <= 0 ||
                record.renamed_network_stack_id !=
                    record.craft_accepted_output_network_stack_id ||
                !validCorrelation(record.input_response, true) ||
                !validCorrelation(record.craft_response, false) ||
                record.craft_response.session_generation !=
                    record.input_response.session_generation ||
                record.craft_response.window_token !=
                    record.input_response.window_token ||
                record.craft_response.window_id !=
                    record.input_response.window_id) break;
            return true;
    }
    return fail(error, "map anvil journal has invalid phase or request IDs");
}

template <size_t N>
void put16(std::array<uint8_t, N>* bytes, size_t offset, uint16_t value) {
    for (size_t i = 0; i < 2U; ++i) (*bytes)[offset + i] =
        static_cast<uint8_t>(value >> (i * 8U));
}
template <size_t N>
void put32(std::array<uint8_t, N>* bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4U; ++i) (*bytes)[offset + i] =
        static_cast<uint8_t>(value >> (i * 8U));
}
template <size_t N>
void put64(std::array<uint8_t, N>* bytes, size_t offset, uint64_t value) {
    for (size_t i = 0; i < 8U; ++i) (*bytes)[offset + i] =
        static_cast<uint8_t>(value >> (i * 8U));
}
uint16_t get16(const std::array<uint8_t, kDiskBytes>& bytes, size_t offset) {
    uint16_t result = 0;
    for (size_t i = 0; i < 2U; ++i) result |=
        static_cast<uint16_t>(bytes[offset + i]) << (i * 8U);
    return result;
}
uint32_t get32(const std::array<uint8_t, kDiskBytes>& bytes, size_t offset) {
    uint32_t result = 0;
    for (size_t i = 0; i < 4U; ++i) result |=
        static_cast<uint32_t>(bytes[offset + i]) << (i * 8U);
    return result;
}
uint64_t get64(const std::array<uint8_t, kDiskBytes>& bytes, size_t offset) {
    uint64_t result = 0;
    for (size_t i = 0; i < 8U; ++i) result |=
        static_cast<uint64_t>(bytes[offset + i]) << (i * 8U);
    return result;
}
int32_t signed32(uint32_t value) {
    int32_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
int64_t signed64(uint64_t value) {
    int64_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
uint64_t checksum(const std::array<uint8_t, kDiskBytes>& bytes) {
    uint64_t hash = kFnvOffset;
    for (size_t i = 0; i < kChecksumOffset; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

std::array<uint8_t, kDiskBytes> encode(const MapAnvilRenameRecord& record) {
    std::array<uint8_t, kDiskBytes> bytes{};
    std::memcpy(bytes.data(), kMagic.data(), kMagic.size());
    put32(&bytes, 8U, kDiskVersion);
    put32(&bytes, 12U, static_cast<uint32_t>(kDiskBytes));
    put32(&bytes, 16U, static_cast<uint32_t>(record.phase));
    put32(&bytes, 20U, static_cast<uint32_t>(record.dimension_id));
    put64(&bytes, 24U, record.tile_cursor);
    put64(&bytes, 32U, record.tile_count);
    put32(&bytes, 40U, static_cast<uint32_t>(record.anvil_x));
    put32(&bytes, 44U, static_cast<uint32_t>(record.anvil_y));
    put32(&bytes, 48U, static_cast<uint32_t>(record.anvil_z));
    put32(&bytes, 52U, static_cast<uint32_t>(record.map_runtime_item_id));
    put64(&bytes, 56U, static_cast<uint64_t>(record.map_uuid));
    put32(&bytes, 64U, static_cast<uint32_t>(record.input_source_network_stack_id));
    put32(&bytes, 68U, static_cast<uint32_t>(record.input_request_id));
    put32(&bytes, 72U, static_cast<uint32_t>(record.craft_request_id));
    put32(&bytes, 76U, static_cast<uint32_t>(record.renamed_network_stack_id));
    put32(&bytes, 80U, record.columns);
    put32(&bytes, 84U, record.rows);
    put16(&bytes, 88U, static_cast<uint16_t>(record.world_id.size()));
    put16(&bytes, 90U, static_cast<uint16_t>(record.expected_title.size()));
    put64(&bytes, 92U, record.input_response.session_generation);
    put64(&bytes, 100U, record.input_response.response_generation_before_send);
    put64(&bytes, 108U, record.input_response.window_token);
    put32(&bytes, 116U, record.input_response.window_id);
    put32(&bytes, 120U,
          static_cast<uint32_t>(record.input_response.source_hotbar_slot));
    put64(&bytes, 124U, record.craft_response.session_generation);
    put64(&bytes, 132U, record.craft_response.response_generation_before_send);
    put64(&bytes, 140U, record.craft_response.window_token);
    put32(&bytes, 148U, record.craft_response.window_id);
    put32(&bytes, 152U,
          static_cast<uint32_t>(record.craft_accepted_output_network_stack_id));
    put32(&bytes, 156U,
          static_cast<uint32_t>(record.craft_input_network_stack_id));
    put32(&bytes, 160U,
          static_cast<uint32_t>(record.craft_destination_hotbar_slot));
    std::memcpy(bytes.data() + kWorldOffset, record.world_id.data(),
                record.world_id.size());
    std::memcpy(bytes.data() + kTitleOffset, record.expected_title.data(),
                record.expected_title.size());
    put64(&bytes, kChecksumOffset, checksum(bytes));
    return bytes;
}

bool decode(const std::array<uint8_t, kDiskBytes>& bytes,
            MapAnvilRenameRecord* output, std::string* error) {
    if (!output || std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0 ||
        get32(bytes, 8U) != kDiskVersion ||
        get32(bytes, 12U) != kDiskBytes ||
        get64(bytes, kChecksumOffset) != checksum(bytes)) {
        return fail(error, "map anvil journal header or checksum is invalid");
    }
    const uint16_t world_length = get16(bytes, 88U);
    const uint16_t title_length = get16(bytes, 90U);
    if (!world_length || world_length > kWorldBytes ||
        !title_length || title_length > kTitleBytes) {
        return fail(error, "map anvil journal string lengths are invalid");
    }
    for (size_t i = kWorldOffset + world_length; i < kTitleOffset; ++i) {
        if (bytes[i]) return fail(error, "map anvil journal world padding is invalid");
    }
    for (size_t i = kTitleOffset + title_length; i < kChecksumOffset; ++i) {
        if (bytes[i]) return fail(error, "map anvil journal title padding is invalid");
    }
    MapAnvilRenameRecord record;
    record.world_id.assign(reinterpret_cast<const char*>(bytes.data() + kWorldOffset),
                           world_length);
    record.expected_title.assign(
        reinterpret_cast<const char*>(bytes.data() + kTitleOffset), title_length);
    record.phase = static_cast<MapAnvilRenamePhase>(get32(bytes, 16U));
    record.dimension_id = signed32(get32(bytes, 20U));
    record.tile_cursor = get64(bytes, 24U);
    record.tile_count = get64(bytes, 32U);
    record.anvil_x = signed32(get32(bytes, 40U));
    record.anvil_y = signed32(get32(bytes, 44U));
    record.anvil_z = signed32(get32(bytes, 48U));
    record.map_runtime_item_id = signed32(get32(bytes, 52U));
    record.map_uuid = signed64(get64(bytes, 56U));
    record.input_source_network_stack_id = signed32(get32(bytes, 64U));
    record.input_request_id = signed32(get32(bytes, 68U));
    record.craft_request_id = signed32(get32(bytes, 72U));
    record.renamed_network_stack_id = signed32(get32(bytes, 76U));
    record.columns = get32(bytes, 80U);
    record.rows = get32(bytes, 84U);
    const uint32_t input_window_id = get32(bytes, 116U);
    const uint32_t craft_window_id = get32(bytes, 148U);
    if (input_window_id > 0xFFU || craft_window_id > 0xFFU) {
        return fail(error, "map anvil journal window ID exceeds one byte");
    }
    record.input_response.session_generation = get64(bytes, 92U);
    record.input_response.response_generation_before_send = get64(bytes, 100U);
    record.input_response.window_token = get64(bytes, 108U);
    record.input_response.window_id = static_cast<uint8_t>(input_window_id);
    record.input_response.source_hotbar_slot = signed32(get32(bytes, 120U));
    record.craft_response.session_generation = get64(bytes, 124U);
    record.craft_response.response_generation_before_send = get64(bytes, 132U);
    record.craft_response.window_token = get64(bytes, 140U);
    record.craft_response.window_id = static_cast<uint8_t>(craft_window_id);
    record.craft_accepted_output_network_stack_id = signed32(get32(bytes, 152U));
    record.craft_input_network_stack_id = signed32(get32(bytes, 156U));
    record.craft_destination_hotbar_slot = signed32(get32(bytes, 160U));
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

MapAnvilRenameLoad loadUnlocked(const std::string& map_state_path,
                                MapAnvilRenameRecord* output,
                                std::string* error) {
    if (!output || map_state_path.empty()) {
        fail(error, "map anvil journal path or output is unavailable");
        return MapAnvilRenameLoad::Unsafe;
    }
    *output = {};
    const std::string path = MapAnvilRenameJournalPath(map_state_path);
    bool journal_exists = false;
    bool temporary_exists = false;
    if (!pathExists(path + ".tmp", &temporary_exists) || temporary_exists ||
        !pathExists(path, &journal_exists)) {
        fail(error, "map anvil journal has an unresolved temporary file or path error");
        return MapAnvilRenameLoad::Unsafe;
    }
    if (!journal_exists) {
        if (error) error->clear();
        return MapAnvilRenameLoad::Missing;
    }
    struct stat status {};
    if (stat(path.c_str(), &status) != 0 ||
        status.st_size != static_cast<decltype(status.st_size)>(kDiskBytes)) {
        fail(error, "map anvil journal size is invalid");
        return MapAnvilRenameLoad::Unsafe;
    }
    std::array<uint8_t, kDiskBytes> bytes{};
    std::ifstream input(path, std::ios::binary);
    char trailing = 0;
    if (!input || !input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) ||
        input.read(&trailing, 1) || !decode(bytes, output, error)) {
        if (error && error->empty()) *error = "map anvil journal cannot be read";
        return MapAnvilRenameLoad::Unsafe;
    }
    if (error) error->clear();
    return MapAnvilRenameLoad::Loaded;
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
    if (fd < 0) return fail(error, "cannot create temporary map anvil journal");
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
        return fail(error, "cannot durably write map anvil journal");
    }
#if defined(_WIN32)
    const std::wstring wide_temporary = utf8Wide(temporary);
    const std::wstring wide_path = utf8Wide(path);
    const DWORD flags = MOVEFILE_WRITE_THROUGH |
        (replace ? MOVEFILE_REPLACE_EXISTING : 0U);
    if (wide_temporary.empty() || wide_path.empty() ||
        !MoveFileExW(wide_temporary.c_str(), wide_path.c_str(), flags)) {
        std::remove(temporary.c_str());
        return fail(error, "cannot finalize map anvil journal");
    }
#else
    if (!(replace ? std::rename(temporary.c_str(), path.c_str()) == 0 :
                    CommitMapJournalNoReplace(temporary, path))) {
        std::remove(temporary.c_str());
        return fail(error, "cannot finalize map anvil journal");
    }
    if (!syncParent(path)) {
        return fail(error, "cannot sync map anvil journal directory");
    }
#endif
    if (error) error->clear();
    return true;
}

bool sameCorrelation(const MapAnvilResponseCorrelation& a,
                     const MapAnvilResponseCorrelation& b) noexcept {
    return a.session_generation == b.session_generation &&
        a.response_generation_before_send == b.response_generation_before_send &&
        a.window_token == b.window_token && a.window_id == b.window_id &&
        a.source_hotbar_slot == b.source_hotbar_slot;
}

bool sameRecord(const MapAnvilRenameRecord& a,
                 const MapAnvilRenameRecord& b) {
    return a.world_id == b.world_id && a.dimension_id == b.dimension_id &&
        a.tile_cursor == b.tile_cursor && a.tile_count == b.tile_count &&
        a.columns == b.columns && a.rows == b.rows &&
        a.anvil_x == b.anvil_x && a.anvil_y == b.anvil_y &&
        a.anvil_z == b.anvil_z &&
        a.map_runtime_item_id == b.map_runtime_item_id &&
        a.map_uuid == b.map_uuid &&
        a.input_source_network_stack_id == b.input_source_network_stack_id &&
        a.expected_title == b.expected_title &&
        a.input_request_id == b.input_request_id &&
        a.craft_request_id == b.craft_request_id &&
        a.craft_input_network_stack_id == b.craft_input_network_stack_id &&
        a.craft_destination_hotbar_slot == b.craft_destination_hotbar_slot &&
        a.craft_accepted_output_network_stack_id ==
            b.craft_accepted_output_network_stack_id &&
        a.renamed_network_stack_id == b.renamed_network_stack_id &&
        sameCorrelation(a.input_response, b.input_response) &&
        sameCorrelation(a.craft_response, b.craft_response) &&
        a.phase == b.phase;
}

bool loadMatching(const std::string& path, const MapAnvilRenameRecord& expected,
                  MapAnvilRenameRecord* actual, std::string* error) {
    if (loadUnlocked(path, actual, error) != MapAnvilRenameLoad::Loaded ||
        !sameRecord(*actual, expected)) {
        return fail(error, "map anvil journal no longer matches expected state");
    }
    return true;
}

bool inputProofMatches(const MapAnvilRenameRecord& record,
                        const MapAnvilInputProof& proof) {
    return proof.native_readback &&
        proof.fresh_window_token == record.input_response.window_token &&
        proof.window_id == record.input_response.window_id &&
        proof.world_id == record.world_id &&
        proof.dimension_id == record.dimension_id &&
        proof.anvil_x == record.anvil_x &&
        proof.anvil_y == record.anvil_y &&
        proof.anvil_z == record.anvil_z && proof.input_slot == 1U &&
        proof.count == 1U &&
        proof.runtime_item_id == record.map_runtime_item_id &&
        // Place may assign the input slot a new net ID. The UUID from the
        // fresh native slot readback, not net-ID continuity, proves identity.
        proof.network_stack_id > 0 &&
        proof.map_uuid == record.map_uuid;
}

}  // namespace

std::string MapAnvilRenameJournalPath(const std::string& map_state_path) {
    return map_state_path + ".anvil_pending";
}

bool BeginMapAnvilRenameJournal(const std::string& map_state_path,
                                const MapAnvilRenameRecord& prepared,
                                std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (prepared.phase != MapAnvilRenamePhase::Prepared ||
        !validRecord(prepared, error)) return false;
    MapAnvilRenameRecord existing;
    if (loadUnlocked(map_state_path, &existing, error) != MapAnvilRenameLoad::Missing) {
        return fail(error, "a previous map anvil rename is unresolved");
    }
    return writeAtomic(MapAnvilRenameJournalPath(map_state_path),
                       encode(prepared), false, error);
}

MapAnvilRenameLoad LoadMapAnvilRenameJournal(
    const std::string& map_state_path, MapAnvilRenameRecord* output,
    std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    return loadUnlocked(map_state_path, output, error);
}

bool ArmMapAnvilInputDispatch(const std::string& map_state_path,
                               const MapAnvilRenameRecord& expected_prepared,
                               int32_t request_id,
                               const MapAnvilResponseCorrelation& correlation,
                               std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (expected_prepared.phase != MapAnvilRenamePhase::Prepared ||
        !negativeOdd(request_id) || !validCorrelation(correlation, true)) {
        return fail(error, "map anvil input send has no durable response identity");
    }
    MapAnvilRenameRecord actual;
    if (!loadMatching(map_state_path, expected_prepared, &actual, error)) return false;
    actual.phase = MapAnvilRenamePhase::InputDispatchArmed;
    actual.input_request_id = request_id;
    actual.input_response = correlation;
    return writeAtomic(MapAnvilRenameJournalPath(map_state_path),
                       encode(actual), true, error);
}

bool NoteMapAnvilInputAccepted(const std::string& map_state_path,
                               const MapAnvilRenameRecord& expected_armed,
                               int32_t request_id, std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (expected_armed.phase != MapAnvilRenamePhase::InputDispatchArmed ||
        request_id != expected_armed.input_request_id) {
        return fail(error, "map anvil input response does not match its armed request");
    }
    MapAnvilRenameRecord actual;
    if (!loadMatching(map_state_path, expected_armed, &actual, error)) return false;
    actual.phase = MapAnvilRenamePhase::InputResponseAccepted;
    return writeAtomic(MapAnvilRenameJournalPath(map_state_path),
                       encode(actual), true, error);
}

bool ConfirmMapAnvilInput(const std::string& map_state_path,
                          const MapAnvilRenameRecord& expected,
                          const MapAnvilInputProof& proof,
                          std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (expected.phase != MapAnvilRenamePhase::InputResponseAccepted) {
        return fail(error, "map anvil input has no accepted server response");
    }
    MapAnvilRenameRecord actual;
    if (!loadMatching(map_state_path, expected, &actual, error) ||
        !inputProofMatches(actual, proof)) {
        return fail(error, "fresh anvil input does not match the original map UUID");
    }
    actual.phase = MapAnvilRenamePhase::InputConfirmed;
    return writeAtomic(MapAnvilRenameJournalPath(map_state_path),
                       encode(actual), true, error);
}

bool ArmMapAnvilCraftClick(const std::string& map_state_path,
                            const MapAnvilRenameRecord& expected_input_confirmed,
                            const MapAnvilInputProof& pre_click_input,
                            std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (expected_input_confirmed.phase != MapAnvilRenamePhase::InputConfirmed) {
        return fail(error, "map anvil result click has no confirmed input");
    }
    MapAnvilRenameRecord actual;
    if (!loadMatching(map_state_path, expected_input_confirmed, &actual, error) ||
        !inputProofMatches(actual, pre_click_input)) {
        return fail(error, "fresh anvil input changed before result click");
    }
    actual.phase = MapAnvilRenamePhase::CraftClickArmed;
    actual.craft_input_network_stack_id = pre_click_input.network_stack_id;
    return writeAtomic(MapAnvilRenameJournalPath(map_state_path),
                       encode(actual), true, error);
}

bool ArmMapAnvilCraftDispatch(const std::string& map_state_path,
                               const MapAnvilRenameRecord& expected_click_armed,
                               const MapAnvilInputProof& pre_click_input,
                               int32_t request_id,
                               const MapAnvilResponseCorrelation& correlation,
                               uint8_t destination_hotbar_slot,
                               std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (expected_click_armed.phase != MapAnvilRenamePhase::CraftClickArmed ||
        !negativeOdd(request_id) || !validCorrelation(correlation, false) ||
        destination_hotbar_slot > 8U ||
        correlation.session_generation !=
            expected_click_armed.input_response.session_generation ||
        correlation.window_token !=
            expected_click_armed.input_response.window_token ||
        correlation.window_id !=
            expected_click_armed.input_response.window_id ||
        pre_click_input.fresh_window_token != correlation.window_token ||
        pre_click_input.window_id != correlation.window_id ||
        pre_click_input.network_stack_id !=
            expected_click_armed.craft_input_network_stack_id ||
        request_id == expected_click_armed.input_request_id) {
        return fail(error, "map anvil craft send lost its original window or response identity");
    }
    MapAnvilRenameRecord actual;
    if (!loadMatching(map_state_path, expected_click_armed, &actual, error) ||
        !inputProofMatches(actual, pre_click_input) ||
        actual.craft_input_network_stack_id !=
            pre_click_input.network_stack_id) {
        return fail(error, "pre-click anvil input identity changed before craft dispatch");
    }
    actual.phase = MapAnvilRenamePhase::CraftDispatchArmed;
    actual.craft_request_id = request_id;
    actual.craft_destination_hotbar_slot = destination_hotbar_slot;
    actual.craft_response = correlation;
    return writeAtomic(MapAnvilRenameJournalPath(map_state_path),
                       encode(actual), true, error);
}

bool NoteMapAnvilCraftAccepted(const std::string& map_state_path,
                               const MapAnvilRenameRecord& expected_armed,
                               int32_t request_id,
                               int32_t accepted_output_network_stack_id,
                               std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (expected_armed.phase != MapAnvilRenamePhase::CraftDispatchArmed ||
        request_id != expected_armed.craft_request_id ||
        accepted_output_network_stack_id <= 0) {
        return fail(error, "map anvil craft response does not match its armed request");
    }
    MapAnvilRenameRecord actual;
    if (!loadMatching(map_state_path, expected_armed, &actual, error)) return false;
    actual.phase = MapAnvilRenamePhase::CraftResponseAccepted;
    actual.craft_accepted_output_network_stack_id =
        accepted_output_network_stack_id;
    return writeAtomic(MapAnvilRenameJournalPath(map_state_path),
                       encode(actual), true, error);
}

bool ConfirmMapAnvilRenamedOutput(const std::string& map_state_path,
                                  const MapAnvilRenameRecord& expected,
                                  const MapAnvilOutputProof& proof,
                                  std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (expected.phase != MapAnvilRenamePhase::CraftResponseAccepted) {
        return fail(error, "map anvil craft has no accepted server response");
    }
    MapAnvilRenameRecord actual;
    if (!loadMatching(map_state_path, expected, &actual, error)) return false;
    if (!proof.native_readback || proof.world_id != actual.world_id ||
        proof.dimension_id != actual.dimension_id || proof.count != 1U ||
        proof.runtime_item_id != actual.map_runtime_item_id ||
        proof.network_stack_id <= 0 ||
        proof.network_stack_id !=
            actual.craft_accepted_output_network_stack_id ||
        proof.map_uuid != actual.map_uuid ||
        proof.name_source != MapItemNameSource::DisplayName ||
        proof.name != actual.expected_title) {
        return fail(error, "renamed map UUID or exact display name is unconfirmed");
    }
    actual.phase = MapAnvilRenamePhase::RenamedMapConfirmed;
    actual.renamed_network_stack_id = proof.network_stack_id;
    return writeAtomic(MapAnvilRenameJournalPath(map_state_path),
                       encode(actual), true, error);
}

MapAnvilRenameRecovery ClassifyMapAnvilRenameRecovery(
    const MapAnvilRenameRecord& journal, const std::string& world_id,
    int32_t dimension_id, uint64_t checkpoint_tile_cursor,
    uint64_t tile_count) noexcept {
    if (!validRecord(journal, nullptr) || journal.world_id != world_id ||
        journal.dimension_id != dimension_id || journal.tile_count != tile_count) {
        return MapAnvilRenameRecovery::Unsafe;
    }
    if (checkpoint_tile_cursor == journal.tile_cursor) {
        switch (journal.phase) {
            case MapAnvilRenamePhase::Prepared:
                return MapAnvilRenameRecovery::FreshInputPreflight;
            case MapAnvilRenamePhase::InputDispatchArmed:
            case MapAnvilRenamePhase::InputResponseAccepted:
                return MapAnvilRenameRecovery::ReconcileInputNoResend;
            case MapAnvilRenamePhase::InputConfirmed:
                return MapAnvilRenameRecovery::FreshCraftPreflight;
            case MapAnvilRenamePhase::CraftClickArmed:
                return MapAnvilRenameRecovery::ReconcileCraftClickNoResend;
            case MapAnvilRenamePhase::CraftDispatchArmed:
            case MapAnvilRenamePhase::CraftResponseAccepted:
                return MapAnvilRenameRecovery::ReconcileCraftNoResend;
            case MapAnvilRenamePhase::RenamedMapConfirmed:
                return MapAnvilRenameRecovery::ProceedToChestStorage;
        }
    }
    if (checkpoint_tile_cursor == journal.tile_cursor + 1U &&
        journal.phase == MapAnvilRenamePhase::RenamedMapConfirmed) {
        return MapAnvilRenameRecovery::ClearAfterStorageCommit;
    }
    return MapAnvilRenameRecovery::Unsafe;
}

bool ClearCommittedMapAnvilRenameJournal(
    const std::string& map_state_path,
    const MapAnvilRenameRecord& expected_confirmed,
    const MapChestTransferRecord& confirmed_chest,
    uint64_t checkpoint_tile_cursor, std::string* error) {
    std::lock_guard<std::mutex> lock(g_journal_mutex);
    if (expected_confirmed.phase != MapAnvilRenamePhase::RenamedMapConfirmed ||
        checkpoint_tile_cursor != expected_confirmed.tile_cursor + 1U ||
        (confirmed_chest.phase != MapChestTransferPhase::ReopenConfirmed &&
         confirmed_chest.phase != MapChestTransferPhase::InventoryConfirmed &&
         confirmed_chest.phase != MapChestTransferPhase::AcceptedAndClosed) ||
        confirmed_chest.world_id != expected_confirmed.world_id ||
        confirmed_chest.dimension_id != expected_confirmed.dimension_id ||
        confirmed_chest.tile_cursor != expected_confirmed.tile_cursor ||
        confirmed_chest.tile_count != expected_confirmed.tile_count ||
        confirmed_chest.source_map_uuid != expected_confirmed.map_uuid ||
        confirmed_chest.source_runtime_item_id !=
            expected_confirmed.map_runtime_item_id) {
        return fail(error, "map anvil journal cannot clear before matching chest and cursor commit");
    }
    MapChestTransferRecord persisted_chest;
    if (LoadMapChestTransferJournal(map_state_path, &persisted_chest, error) !=
            MapChestJournalLoad::Loaded ||
        persisted_chest.phase != confirmed_chest.phase ||
        persisted_chest.world_id != confirmed_chest.world_id ||
        persisted_chest.dimension_id != confirmed_chest.dimension_id ||
        persisted_chest.tile_cursor != confirmed_chest.tile_cursor ||
        persisted_chest.tile_count != confirmed_chest.tile_count ||
        persisted_chest.source_map_uuid != confirmed_chest.source_map_uuid ||
        persisted_chest.source_runtime_item_id !=
            confirmed_chest.source_runtime_item_id ||
        persisted_chest.request_id != confirmed_chest.request_id) {
        return fail(error, "matching confirmed chest journal is not durably present");
    }
    MapAnvilRenameRecord actual;
    if (!loadMatching(map_state_path, expected_confirmed, &actual, error)) return false;
    const std::string path = MapAnvilRenameJournalPath(map_state_path);
    if (std::remove(path.c_str()) != 0) {
        return fail(error, "cannot remove committed map anvil journal");
    }
#if !defined(_WIN32)
    if (!syncParent(path)) return fail(error, "cannot sync cleared map anvil journal");
#endif
    if (error) error->clear();
    return true;
}

}  // namespace build_import
