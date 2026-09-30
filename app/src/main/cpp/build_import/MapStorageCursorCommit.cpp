#include "MapStorageCursorCommit.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

#if defined(_WIN32)
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace build_import {
namespace {

struct DeferredDataStateDiskV1 {
    uint32_t magic = 0x31534444U;
    uint32_t version = 1U;
    uint64_t record_count = 0U;
    uint64_t cursor = 0U;
};
static_assert(sizeof(DeferredDataStateDiskV1) == 24U,
              "map cursor state must match BuildImportRuntime DDS1 layout");

bool fail(std::string* error, const char* reason) {
    if (error) *error = reason;
    return false;
}

bool readExactState(const std::string& path, DeferredDataStateDiskV1* state) {
    if (!state) return false;
    std::ifstream input(path, std::ios::binary);
    char trailing = 0;
    return input && input.read(reinterpret_cast<char*>(state), sizeof(*state)) &&
        !input.read(&trailing, 1) && state->magic == 0x31534444U &&
        state->version == 1U;
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
        slash == 0U ? "/" : path.substr(0U, slash);
    const int fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool synced = fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    return synced && closed;
}
#endif

bool replaceAndSync(const std::string& temporary,
                    const std::string& path, std::string* error) {
#if defined(_WIN32)
    const std::wstring wide_temporary = utf8Wide(temporary);
    const std::wstring wide_path = utf8Wide(path);
    if (wide_temporary.empty() || wide_path.empty() ||
        !MoveFileExW(wide_temporary.c_str(), wide_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return fail(error, "map cursor commit atomic replacement failed");
    }
#else
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        return fail(error, "map cursor commit atomic replacement failed");
    }
    if (!syncParent(path)) {
        return fail(error, "map cursor commit directory sync failed; reload state before retry");
    }
#endif
    if (error) error->clear();
    return true;
}

bool writeExactState(const std::string& path,
                     const DeferredDataStateDiskV1& state,
                     std::string* error) {
    // This temp name is intentionally distinct from the legacy .tmp writer.
    // A leftover file indicates an interrupted commit and must not silently
    // be overwritten or misread as a fresh cursor transaction.
    const std::string temporary = path + ".storage-commit.tmp";
#if defined(_WIN32)
    const int fd = _open(temporary.c_str(),
                         _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                         _S_IREAD | _S_IWRITE);
#else
    const int fd = open(temporary.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#endif
    if (fd < 0) {
        if (errno == EEXIST) {
            DeferredDataStateDiskV1 interrupted;
            if (readExactState(temporary, &interrupted) &&
                std::memcmp(&interrupted, &state, sizeof(state)) == 0) {
                // The file bytes were fully written before the prior crash.
                // A verified chest journal still guards this call, so safely
                // finish its pending atomic replacement without resending
                // any map or inventory request.
                return replaceAndSync(temporary, path, error);
            }
        }
        return fail(error, "map cursor commit temporary state is unsafe or cannot be created");
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(&state);
    size_t written = 0U;
    bool okay = true;
    while (written < sizeof(state)) {
#if defined(_WIN32)
        const int amount = _write(fd, bytes + written,
                                  static_cast<unsigned int>(sizeof(state) - written));
#else
        const ssize_t amount = write(fd, bytes + written,
                                     sizeof(state) - written);
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
    if (!okay) {
        std::remove(temporary.c_str());
        return fail(error, "map cursor commit data was not durably written");
    }
    return replaceAndSync(temporary, path, error);
}

}  // namespace

bool CommitVerifiedMapTileCursor(const std::string& state_path,
                                 uint64_t tile_count,
                                 uint64_t expected_cursor,
                                 std::string* error) {
    if (error) error->clear();
    if (state_path.empty() || tile_count == 0U ||
        expected_cursor >= tile_count) {
        return fail(error, "map cursor commit plan is invalid");
    }
    DeferredDataStateDiskV1 old_state;
    if (!readExactState(state_path, &old_state) ||
        old_state.record_count != tile_count ||
        old_state.cursor != expected_cursor) {
        return fail(error, "map cursor state does not match the verified chest tile");
    }
    DeferredDataStateDiskV1 next = old_state;
    next.cursor = expected_cursor + 1U;
    return writeExactState(state_path, next, error);
}

}  // namespace build_import
