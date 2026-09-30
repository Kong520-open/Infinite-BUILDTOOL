#include "../MapStorageCursorCommit.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

struct DiskState {
    uint32_t magic = 0x31534444U;
    uint32_t version = 1U;
    uint64_t record_count = 0U;
    uint64_t cursor = 0U;
};
static_assert(sizeof(DiskState) == 24U);

DiskState readState(const std::filesystem::path& path) {
    DiskState value;
    std::ifstream input(path, std::ios::binary);
    char trailing = 0;
    assert(input.read(reinterpret_cast<char*>(&value), sizeof(value)));
    assert(!input.read(&trailing, 1));
    return value;
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const fs::path directory = fs::temp_directory_path() /
        ("buildtool-map-cursor-" + std::to_string(stamp));
    assert(fs::create_directory(directory));
    const fs::path state_path = directory / "map_creation.state";
    const DiskState initial{0x31534444U, 1U, 3U, 1U};
    {
        std::ofstream output(state_path, std::ios::binary);
        assert(output.write(reinterpret_cast<const char*>(&initial),
                            sizeof(initial)));
    }
    std::string error;
    assert(build_import::CommitVerifiedMapTileCursor(
        state_path.string(), 3U, 1U, &error));
    assert(error.empty());
    const DiskState committed = readState(state_path);
    assert(committed.magic == initial.magic);
    assert(committed.version == initial.version);
    assert(committed.record_count == initial.record_count);
    assert(committed.cursor == 2U);
    assert(!build_import::CommitVerifiedMapTileCursor(
        state_path.string(), 3U, 1U, &error));
    assert(readState(state_path).cursor == 2U);
    assert(!build_import::CommitVerifiedMapTileCursor(
        state_path.string(), 4U, 2U, &error));

    const fs::path leftover = state_path.string() + ".storage-commit.tmp";
    {
        std::ofstream output(leftover, std::ios::binary);
        assert(output.write("incomplete", 10));
    }
    assert(!build_import::CommitVerifiedMapTileCursor(
        state_path.string(), 3U, 2U, &error));
    assert(readState(state_path).cursor == 2U);
    assert(fs::remove(leftover));
    const DiskState interrupted{0x31534444U, 1U, 3U, 3U};
    {
        std::ofstream output(leftover, std::ios::binary);
        assert(output.write(reinterpret_cast<const char*>(&interrupted),
                            sizeof(interrupted)));
    }
    assert(build_import::CommitVerifiedMapTileCursor(
        state_path.string(), 3U, 2U, &error));
    assert(readState(state_path).cursor == 3U);
    assert(!fs::exists(leftover));
    assert(fs::remove(state_path));
    assert(fs::remove(directory));
}
