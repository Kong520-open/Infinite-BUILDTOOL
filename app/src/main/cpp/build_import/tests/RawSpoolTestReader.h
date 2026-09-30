#ifndef INFINITE_TEXTURE_RAW_SPOOL_TEST_READER_H
#define INFINITE_TEXTURE_RAW_SPOOL_TEST_READER_H

#include "../BuildImportTypes.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace build_import_test {

struct LegacyRawDiskRecord {
    int32_t x;
    int32_t y;
    int32_t z;
    uint8_t aux;
    uint8_t flags;
    uint16_t name_length;
};

static_assert(sizeof(LegacyRawDiskRecord) == 16,
              "unexpected legacy raw spool record layout");

struct DecodedRawRecord {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    uint16_t aux = 0;
    uint8_t flags = 0;
    std::string name;
};

inline std::vector<std::string> readRawPalette(const std::filesystem::path& spool_path) {
    const std::filesystem::path path =
        spool_path.parent_path() / build_import::kRawPaletteFileName;
    std::ifstream input(path, std::ios::binary);
    assert(input);
    build_import::RawPaletteHeaderV2 header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    assert(input && header.magic == build_import::kRawPaletteMagic &&
           header.version == build_import::kRawPaletteVersion && header.reserved == 0);
    std::vector<std::string> names;
    names.reserve(header.entry_count);
    for (uint32_t index = 0; index < header.entry_count; ++index) {
        uint16_t length = 0;
        input.read(reinterpret_cast<char*>(&length), sizeof(length));
        assert(input && length != 0);
        std::string name(length, '\0');
        input.read(&name[0], length);
        assert(input);
        names.push_back(std::move(name));
    }
    assert(input.peek() == std::char_traits<char>::eof());
    return names;
}

inline std::vector<DecodedRawRecord> readRawSpool(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    uint32_t magic = 0;
    input.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    input.clear();
    input.seekg(0, std::ios::beg);
    assert(input);
    std::vector<DecodedRawRecord> records;
    if (magic == build_import::kRawSpoolMagic) {
        build_import::RawSpoolHeaderV2 header{};
        input.read(reinterpret_cast<char*>(&header), sizeof(header));
        assert(input && header.version == build_import::kRawSpoolVersion &&
               header.kind == build_import::kRawSpoolKindBlocks &&
                header.record_size == sizeof(build_import::RawSpoolRecordV3));
        const std::vector<std::string> names = readRawPalette(path);
        while (true) {
            build_import::RawSpoolRecordV3 record{};
            input.read(reinterpret_cast<char*>(&record), sizeof(record));
            if (input.eof() && input.gcount() == 0) break;
            assert(input.gcount() == static_cast<std::streamsize>(sizeof(record)) &&
                   record.reserved == 0 && record.name_id < names.size());
            records.push_back({record.x, record.y, record.z, record.aux,
                               record.flags, names[record.name_id]});
        }
        return records;
    }

    while (true) {
        LegacyRawDiskRecord record{};
        input.read(reinterpret_cast<char*>(&record), sizeof(record));
        if (input.eof() && input.gcount() == 0) break;
        assert(input.gcount() == static_cast<std::streamsize>(sizeof(record)) &&
               record.name_length != 0);
        std::string name(record.name_length, '\0');
        input.read(&name[0], record.name_length);
        assert(input);
        records.push_back({record.x, record.y, record.z, record.aux,
                           record.flags, std::move(name)});
    }
    return records;
}

}  // namespace build_import_test

#endif  // INFINITE_TEXTURE_RAW_SPOOL_TEST_READER_H
