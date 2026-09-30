#include "InfinityCompressionCodec.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace build_import;

namespace {

std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

void writeBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    assert(output);
}

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "infinity_compression_codec_test";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);

    const std::filesystem::path source = directory / "source.bin";
    std::vector<uint8_t> plaintext(1024U * 1024U);
    for (size_t index = 0; index < plaintext.size(); ++index) {
        plaintext[index] = static_cast<uint8_t>("ICBUILD-minecraft:stone\n"[index % 24U]);
    }
    writeBytes(source, plaintext);

    InfinityCompressionOptions options;
    options.maximum_uncompressed_bytes = 2ULL * 1024ULL * 1024ULL;
    options.maximum_compressed_bytes = 2ULL * 1024ULL * 1024ULL;
    const std::filesystem::path compressed = directory / "payload.compressed";
    std::string error;
    assert(InfinityCompressionCodec::compressFile(source.string(), compressed.string(),
                                                   options, &error));
    assert(error.empty());
    assert(std::filesystem::file_size(compressed) < plaintext.size());
    bool detected = false;
    assert(InfinityCompressionCodec::probeCompressedFile(compressed.string(), &detected,
                                                          &error));
    assert(detected);

    const std::filesystem::path restored = directory / "restored.bin";
    assert(InfinityCompressionCodec::decompressFile(compressed.string(), restored.string(),
                                                     options, &error));
    assert(readBytes(restored) == plaintext);

    InfinityCompressionOptions limited = options;
    limited.maximum_uncompressed_bytes = plaintext.size() - 1U;
    const std::filesystem::path rejected = directory / "rejected.bin";
    assert(!InfinityCompressionCodec::decompressFile(compressed.string(), rejected.string(),
                                                      limited, &error));
    assert(error.find("configured size limit") != std::string::npos);
    assert(!std::filesystem::exists(rejected));

    std::vector<uint8_t> damaged = readBytes(compressed);
    damaged.pop_back();
    const std::filesystem::path truncated = directory / "truncated.compressed";
    writeBytes(truncated, damaged);
    error.clear();
    assert(!InfinityCompressionCodec::decompressFile(truncated.string(), rejected.string(),
                                                      options, &error));
    assert(!error.empty());
    assert(!std::filesystem::exists(rejected));

    damaged = readBytes(compressed);
    damaged.push_back(0x7fU);
    const std::filesystem::path trailing = directory / "trailing.compressed";
    writeBytes(trailing, damaged);
    error.clear();
    assert(!InfinityCompressionCodec::decompressFile(trailing.string(), rejected.string(),
                                                      options, &error));
    assert(error.find("trailing data") != std::string::npos);
    assert(!std::filesystem::exists(rejected));

    InfinityCompressionOptions cancelled;
    cancelled.cancellation_requested = [] { return true; };
    const std::filesystem::path cancelled_output = directory / "cancelled.compressed";
    error.clear();
    assert(!InfinityCompressionCodec::compressFile(source.string(), cancelled_output.string(),
                                                    cancelled, &error));
    assert(error.find("cancelled") != std::string::npos);
    assert(!std::filesystem::exists(cancelled_output));

    const std::filesystem::path empty = directory / "empty.bin";
    writeBytes(empty, {});
    const std::filesystem::path empty_compressed = directory / "empty.compressed";
    const std::filesystem::path empty_restored = directory / "empty-restored.bin";
    assert(InfinityCompressionCodec::compressFile(empty.string(), empty_compressed.string(),
                                                   options, &error));
    assert(InfinityCompressionCodec::decompressFile(empty_compressed.string(),
                                                     empty_restored.string(), options, &error));
    assert(std::filesystem::file_size(empty_restored) == 0);

    std::filesystem::remove_all(directory, ignored);
    return 0;
}
