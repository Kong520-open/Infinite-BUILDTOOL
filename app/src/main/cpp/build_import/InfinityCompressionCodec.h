#ifndef INFINITE_TEXTURE_INFINITY_COMPRESSION_CODEC_H
#define INFINITE_TEXTURE_INFINITY_COMPRESSION_CODEC_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace build_import {

struct InfinityCompressionOptions {
    std::function<bool()> cancellation_requested;
    // Zero disables the corresponding caller-imposed limit.
    uint64_t maximum_uncompressed_bytes = 0;
    uint64_t maximum_compressed_bytes = 0;
};

// Streaming Brotli wrapper stored inside the authenticated Infinity envelope.
// The explicit header distinguishes current compressed payloads from legacy
// encrypted files whose plaintext begins directly with the ICBUILD header.
class InfinityCompressionCodec {
public:
    static constexpr uint32_t kFormatVersion = 1;
    static constexpr size_t kHeaderSize = 32;

    static bool compressFile(const std::string& uncompressed_path,
                             const std::string& compressed_path,
                             const InfinityCompressionOptions& options = {},
                             std::string* error = nullptr);

    static bool decompressFile(const std::string& compressed_path,
                               const std::string& uncompressed_path,
                               const InfinityCompressionOptions& options = {},
                               std::string* error = nullptr);

    static bool probeCompressedFile(const std::string& path,
                                    bool* compressed,
                                    std::string* error = nullptr);
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_INFINITY_COMPRESSION_CODEC_H
