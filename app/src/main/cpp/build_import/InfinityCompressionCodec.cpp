#include "InfinityCompressionCodec.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include <brotli/decode.h>
#include <brotli/encode.h>

namespace build_import {
namespace {

constexpr std::array<uint8_t, 8> kMagic = {
    'I', 'N', 'F', 'C', 'O', 'M', 'P', 0,
};
constexpr uint32_t kAlgorithmBrotli = 1;
constexpr size_t kIoChunkSize = 256U * 1024U;

class OutputFileGuard {
public:
    explicit OutputFileGuard(std::string path) : path_(std::move(path)) {}
    ~OutputFileGuard() {
        if (armed_) std::remove(path_.c_str());
    }

    OutputFileGuard(const OutputFileGuard&) = delete;
    OutputFileGuard& operator=(const OutputFileGuard&) = delete;

    void dismiss() noexcept { armed_ = false; }

private:
    std::string path_;
    bool armed_ = true;
};

struct EncoderDeleter {
    void operator()(BrotliEncoderState* state) const noexcept {
        BrotliEncoderDestroyInstance(state);
    }
};

struct DecoderDeleter {
    void operator()(BrotliDecoderState* state) const noexcept {
        BrotliDecoderDestroyInstance(state);
    }
};

using Encoder = std::unique_ptr<BrotliEncoderState, EncoderDeleter>;
using Decoder = std::unique_ptr<BrotliDecoderState, DecoderDeleter>;

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool validPath(const std::string& path) {
    return !path.empty() && path.find('\0') == std::string::npos;
}

bool cancelled(const InfinityCompressionOptions& options) {
    return options.cancellation_requested && options.cancellation_requested();
}

bool getFileSize(std::ifstream* input, uint64_t* size) {
    if (!input || !size) return false;
    input->seekg(0, std::ios::end);
    const std::streampos end = input->tellg();
    if (end < std::streampos(0)) return false;
    *size = static_cast<uint64_t>(end);
    input->seekg(0, std::ios::beg);
    return static_cast<bool>(*input);
}

void writeLe32(uint8_t* output, uint32_t value) {
    for (size_t index = 0; index < 4; ++index) {
        output[index] = static_cast<uint8_t>((value >> (index * 8U)) & 0xffU);
    }
}

void writeLe64(uint8_t* output, uint64_t value) {
    for (size_t index = 0; index < 8; ++index) {
        output[index] = static_cast<uint8_t>((value >> (index * 8U)) & 0xffU);
    }
}

uint32_t readLe32(const uint8_t* input) {
    uint32_t value = 0;
    for (size_t index = 0; index < 4; ++index) {
        value |= static_cast<uint32_t>(input[index]) << (index * 8U);
    }
    return value;
}

uint64_t readLe64(const uint8_t* input) {
    uint64_t value = 0;
    for (size_t index = 0; index < 8; ++index) {
        value |= static_cast<uint64_t>(input[index]) << (index * 8U);
    }
    return value;
}

std::array<uint8_t, InfinityCompressionCodec::kHeaderSize> makeHeader(uint64_t size) {
    std::array<uint8_t, InfinityCompressionCodec::kHeaderSize> header{};
    std::copy(kMagic.begin(), kMagic.end(), header.begin());
    writeLe32(header.data() + 8U, InfinityCompressionCodec::kFormatVersion);
    writeLe32(header.data() + 12U,
              static_cast<uint32_t>(InfinityCompressionCodec::kHeaderSize));
    writeLe32(header.data() + 16U, kAlgorithmBrotli);
    writeLe32(header.data() + 20U, 0);
    writeLe64(header.data() + 24U, size);
    return header;
}

bool validatePaths(const std::string& input_path,
                   const std::string& output_path,
                   std::string* error) {
    if (!validPath(input_path) || !validPath(output_path)) {
        return fail(error, "Infinity compression path is invalid");
    }
    if (input_path == output_path) {
        return fail(error, "Infinity compression input and output paths must be distinct");
    }
    return true;
}

bool writeProduced(std::ofstream* output,
                   const std::vector<uint8_t>& buffer,
                   size_t produced,
                   uint64_t* total,
                   uint64_t maximum,
                   std::string* error) {
    if (!output || !total) return fail(error, "invalid Infinity compression output state");
    if (*total > std::numeric_limits<uint64_t>::max() - produced ||
        (maximum != 0 && *total + produced > maximum)) {
        return fail(error, "Infinity compressed payload exceeds the configured size limit");
    }
    if (produced != 0) {
        output->write(reinterpret_cast<const char*>(buffer.data()),
                      static_cast<std::streamsize>(produced));
        if (!*output) return fail(error, "cannot write Infinity compressed payload");
        *total += produced;
    }
    return true;
}

}  // namespace

bool InfinityCompressionCodec::compressFile(
        const std::string& uncompressed_path,
        const std::string& compressed_path,
        const InfinityCompressionOptions& options,
        std::string* error) {
    if (error) error->clear();
    if (!validatePaths(uncompressed_path, compressed_path, error)) return false;
    if (cancelled(options)) return fail(error, "Infinity compression cancelled");

    std::ifstream input(uncompressed_path, std::ios::binary);
    uint64_t input_size = 0;
    if (!input || !getFileSize(&input, &input_size)) {
        return fail(error, "cannot open Infinity compression input");
    }
    if (options.maximum_uncompressed_bytes != 0 &&
        input_size > options.maximum_uncompressed_bytes) {
        return fail(error, "Infinity uncompressed payload exceeds the configured size limit");
    }

    Encoder encoder(BrotliEncoderCreateInstance(nullptr, nullptr, nullptr));
    if (!encoder ||
        !BrotliEncoderSetParameter(encoder.get(), BROTLI_PARAM_QUALITY, 5) ||
        !BrotliEncoderSetParameter(encoder.get(), BROTLI_PARAM_LGWIN, 22)) {
        return fail(error, "cannot initialize Infinity Brotli encoder");
    }
    if (input_size <= std::numeric_limits<uint32_t>::max()) {
        BrotliEncoderSetParameter(encoder.get(), BROTLI_PARAM_SIZE_HINT,
                                  static_cast<uint32_t>(input_size));
    }

    std::remove(compressed_path.c_str());
    OutputFileGuard guard(compressed_path);
    std::ofstream output(compressed_path, std::ios::binary | std::ios::trunc);
    if (!output) return fail(error, "cannot create Infinity compressed staging file");
    const auto header = makeHeader(input_size);
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    if (!output) return fail(error, "cannot write Infinity compression header");

    try {
        std::vector<uint8_t> input_buffer(kIoChunkSize);
        std::vector<uint8_t> output_buffer(kIoChunkSize);
        uint64_t remaining = input_size;
        size_t available_in = 0;
        const uint8_t* next_in = nullptr;
        uint64_t compressed_bytes = 0;
        while (!BrotliEncoderIsFinished(encoder.get())) {
            if (cancelled(options)) return fail(error, "Infinity compression cancelled");
            if (available_in == 0 && remaining != 0) {
                const size_t requested = static_cast<size_t>(
                    std::min<uint64_t>(remaining, input_buffer.size()));
                input.read(reinterpret_cast<char*>(input_buffer.data()),
                           static_cast<std::streamsize>(requested));
                if (input.gcount() != static_cast<std::streamsize>(requested)) {
                    return fail(error, "Infinity input changed while it was being compressed");
                }
                remaining -= requested;
                available_in = requested;
                next_in = input_buffer.data();
            }

            size_t available_out = output_buffer.size();
            uint8_t* next_out = output_buffer.data();
            const BrotliEncoderOperation operation = remaining == 0
                ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;
            if (!BrotliEncoderCompressStream(encoder.get(), operation, &available_in,
                                             &next_in, &available_out, &next_out, nullptr)) {
                return fail(error, "cannot Brotli-compress Infinity payload");
            }
            const size_t produced = output_buffer.size() - available_out;
            if (!writeProduced(&output, output_buffer, produced, &compressed_bytes,
                               options.maximum_compressed_bytes, error)) {
                return false;
            }
            if (produced == 0 && available_in == 0 && remaining == 0 &&
                !BrotliEncoderIsFinished(encoder.get())) {
                return fail(error, "Infinity Brotli encoder made no progress");
            }
        }
        if (available_in != 0 || remaining != 0 ||
            input.peek() != std::char_traits<char>::eof()) {
            return fail(error, "Infinity input changed while it was being compressed");
        }
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory for Infinity compression buffers");
    }

    output.flush();
    output.close();
    if (!output) return fail(error, "cannot finish Infinity compressed staging file");
    guard.dismiss();
    if (error) error->clear();
    return true;
}

bool InfinityCompressionCodec::decompressFile(
        const std::string& compressed_path,
        const std::string& uncompressed_path,
        const InfinityCompressionOptions& options,
        std::string* error) {
    if (error) error->clear();
    if (!validatePaths(compressed_path, uncompressed_path, error)) return false;
    if (cancelled(options)) return fail(error, "Infinity decompression cancelled");

    std::ifstream input(compressed_path, std::ios::binary);
    uint64_t file_size = 0;
    if (!input || !getFileSize(&input, &file_size)) {
        return fail(error, "cannot open Infinity compressed input");
    }
    if (file_size < kHeaderSize) return fail(error, "Infinity compressed file is truncated");
    const uint64_t compressed_size = file_size - kHeaderSize;
    if (options.maximum_compressed_bytes != 0 &&
        compressed_size > options.maximum_compressed_bytes) {
        return fail(error, "Infinity compressed payload exceeds the configured size limit");
    }

    std::array<uint8_t, kHeaderSize> header{};
    input.read(reinterpret_cast<char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size()) ||
        !std::equal(kMagic.begin(), kMagic.end(), header.begin())) {
        return fail(error, "file is not an Infinity compressed payload");
    }
    if (readLe32(header.data() + 8U) != kFormatVersion ||
        readLe32(header.data() + 12U) != kHeaderSize) {
        return fail(error, "unsupported Infinity compression version");
    }
    if (readLe32(header.data() + 16U) != kAlgorithmBrotli) {
        return fail(error, "unsupported Infinity compression algorithm");
    }
    if (readLe32(header.data() + 20U) != 0) {
        return fail(error, "unsupported Infinity compression flags");
    }
    const uint64_t expected_size = readLe64(header.data() + 24U);
    if (options.maximum_uncompressed_bytes != 0 &&
        expected_size > options.maximum_uncompressed_bytes) {
        return fail(error, "Infinity uncompressed payload exceeds the configured size limit");
    }

    Decoder decoder(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr));
    if (!decoder) return fail(error, "cannot initialize Infinity Brotli decoder");
    std::remove(uncompressed_path.c_str());
    OutputFileGuard guard(uncompressed_path);
    std::ofstream output(uncompressed_path, std::ios::binary | std::ios::trunc);
    if (!output) return fail(error, "cannot create Infinity decompressed staging file");

    try {
        std::vector<uint8_t> input_buffer(kIoChunkSize);
        std::vector<uint8_t> output_buffer(kIoChunkSize);
        uint64_t remaining = compressed_size;
        uint64_t produced_total = 0;
        size_t available_in = 0;
        const uint8_t* next_in = nullptr;
        while (true) {
            if (cancelled(options)) return fail(error, "Infinity decompression cancelled");
            if (available_in == 0 && remaining != 0) {
                const size_t requested = static_cast<size_t>(
                    std::min<uint64_t>(remaining, input_buffer.size()));
                input.read(reinterpret_cast<char*>(input_buffer.data()),
                           static_cast<std::streamsize>(requested));
                if (input.gcount() != static_cast<std::streamsize>(requested)) {
                    return fail(error, "Infinity compressed payload is truncated");
                }
                remaining -= requested;
                available_in = requested;
                next_in = input_buffer.data();
            }

            size_t available_out = output_buffer.size();
            uint8_t* next_out = output_buffer.data();
            const BrotliDecoderResult result = BrotliDecoderDecompressStream(
                decoder.get(), &available_in, &next_in, &available_out, &next_out, nullptr);
            const size_t produced = output_buffer.size() - available_out;
            if (produced_total > expected_size || produced > expected_size - produced_total) {
                return fail(error, "Infinity Brotli stream exceeds its declared size");
            }
            if (produced != 0) {
                output.write(reinterpret_cast<const char*>(output_buffer.data()),
                             static_cast<std::streamsize>(produced));
                if (!output) return fail(error, "cannot write Infinity decompressed payload");
                produced_total += produced;
            }

            if (result == BROTLI_DECODER_RESULT_SUCCESS) {
                if (available_in != 0 || remaining != 0) {
                    return fail(error, "Infinity compressed payload has trailing data");
                }
                if (produced_total != expected_size) {
                    return fail(error, "Infinity decompressed size does not match its header");
                }
                break;
            }
            if (result == BROTLI_DECODER_RESULT_ERROR) {
                const char* detail = BrotliDecoderErrorString(
                    BrotliDecoderGetErrorCode(decoder.get()));
                return fail(error, std::string("cannot decode Infinity Brotli payload") +
                    (detail ? std::string(": ") + detail : std::string()));
            }
            if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT &&
                available_in == 0 && remaining == 0) {
                return fail(error, "Infinity Brotli payload ends before decompression is complete");
            }
            if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT && produced == 0) {
                return fail(error, "Infinity Brotli decoder made no output progress");
            }
        }
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory for Infinity decompression buffers");
    }

    output.flush();
    output.close();
    if (!output) return fail(error, "cannot finish Infinity decompressed staging file");
    guard.dismiss();
    if (error) error->clear();
    return true;
}

bool InfinityCompressionCodec::probeCompressedFile(const std::string& path,
                                                    bool* compressed,
                                                    std::string* error) {
    if (error) error->clear();
    if (!compressed) return fail(error, "Infinity compression probe result is null");
    *compressed = false;
    if (!validPath(path)) return fail(error, "Infinity compression probe path is invalid");
    std::ifstream input(path, std::ios::binary);
    if (!input) return fail(error, "cannot open file for Infinity compression probe");
    std::array<uint8_t, kMagic.size()> prefix{};
    input.read(reinterpret_cast<char*>(prefix.data()),
               static_cast<std::streamsize>(prefix.size()));
    if (input.gcount() == static_cast<std::streamsize>(prefix.size())) {
        *compressed = std::equal(kMagic.begin(), kMagic.end(), prefix.begin());
    }
    return true;
}

}  // namespace build_import
