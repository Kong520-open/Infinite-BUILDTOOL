#include "InfinityCryptoCodec.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace build_import {
namespace {

constexpr std::array<uint8_t, 8> kMagic = {
    'I', 'N', 'F', 'C', 'R', 'Y', 'P', 'T',
};
constexpr uint32_t kAlgorithmAes256Gcm = 1;
constexpr size_t kIoChunkSize = 256U * 1024U;
// SP 800-38D limits one GCM invocation to 2^39 - 256 plaintext bits.
constexpr uint64_t kMaximumGcmPlaintextBytes = (1ULL << 36U) - 32U;

class TemporaryFileGuard {
public:
    explicit TemporaryFileGuard(std::string path) : path_(std::move(path)) {}
    ~TemporaryFileGuard() {
        if (armed_) std::remove(path_.c_str());
    }

    TemporaryFileGuard(const TemporaryFileGuard&) = delete;
    TemporaryFileGuard& operator=(const TemporaryFileGuard&) = delete;

    void dismiss() noexcept { armed_ = false; }

private:
    std::string path_;
    bool armed_ = true;
};

struct CipherContextDeleter {
    void operator()(EVP_CIPHER_CTX* context) const noexcept {
        EVP_CIPHER_CTX_free(context);
    }
};

using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter>;

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

std::string cryptoError(std::string_view operation) {
    const unsigned long code = ERR_get_error();
    if (code == 0) return std::string(operation);
    std::array<char, 256> message{};
    ERR_error_string_n(code, message.data(), message.size());
    return std::string(operation) + ": " + message.data();
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

bool validPath(const std::string& path) {
    return !path.empty() && path.find('\0') == std::string::npos;
}

bool cancelled(const InfinityCryptoOptions& options) {
    return options.cancellation_requested && options.cancellation_requested();
}

bool getFileSize(std::ifstream* input, uint64_t* size) {
    if (!input || !size) return false;
    input->seekg(0, std::ios::end);
    const std::streampos end = input->tellg();
    if (end < std::streampos(0)) return false;
    const auto unsigned_end = static_cast<uint64_t>(end);
    input->seekg(0, std::ios::beg);
    if (!*input) return false;
    *size = unsigned_end;
    return true;
}

bool syncFile(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "r+b");
    if (!file) return false;
#if defined(_WIN32)
    const bool synced = _commit(_fileno(file)) == 0;
#else
    const bool synced = fsync(fileno(file)) == 0;
#endif
    const bool closed = std::fclose(file) == 0;
    return synced && closed;
}

void syncParentDirectory(const std::string& path) {
#if !defined(_WIN32)
    const size_t separator = path.find_last_of('/');
    const std::string parent = separator == std::string::npos ? "." :
        (separator == 0 ? "/" : path.substr(0, separator));
    const int descriptor = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor >= 0) {
        fsync(descriptor);
        close(descriptor);
    }
#else
    (void)path;
#endif
}

bool publishFile(const std::string& temporary, const std::string& destination) {
#if defined(_WIN32)
    return MoveFileExA(temporary.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    return std::rename(temporary.c_str(), destination.c_str()) == 0;
#endif
}

bool finishAndPublish(std::ofstream* output,
                      const std::string& temporary_path,
                      const std::string& destination_path,
                      const InfinityCryptoOptions& options,
                      TemporaryFileGuard* guard,
                      std::string* error) {
    if (!output || !guard) return fail(error, "invalid Infinity crypto output state");
    output->flush();
    output->close();
    if (!*output || !syncFile(temporary_path)) {
        return fail(error, "cannot finish Infinity crypto temporary file");
    }

    std::unique_lock<std::mutex> publish_lock;
    if (options.publish_mutex) {
        publish_lock = std::unique_lock<std::mutex>(*options.publish_mutex);
    }
    if (cancelled(options)) return fail(error, "Infinity crypto operation cancelled");
    if (!publishFile(temporary_path, destination_path)) {
        const int publish_error = errno;
        return fail(error, "cannot publish Infinity crypto file: " +
            std::string(std::strerror(publish_error)));
    }
    guard->dismiss();
    syncParentDirectory(destination_path);
    if (options.publication_committed) {
        options.publication_committed->store(true, std::memory_order_release);
    }
    return true;
}

std::array<uint8_t, InfinityCryptoCodec::kHeaderSize> makeHeader(
        uint64_t plaintext_size,
        const std::array<uint8_t, InfinityCryptoCodec::kNonceSize>& nonce) {
    std::array<uint8_t, InfinityCryptoCodec::kHeaderSize> header{};
    std::copy(kMagic.begin(), kMagic.end(), header.begin());
    writeLe32(header.data() + 8, InfinityCryptoCodec::kEnvelopeVersion);
    writeLe32(header.data() + 12, static_cast<uint32_t>(InfinityCryptoCodec::kHeaderSize));
    writeLe32(header.data() + 16, kAlgorithmAes256Gcm);
    writeLe32(header.data() + 20, 0);
    writeLe64(header.data() + 24, plaintext_size);
    std::copy(nonce.begin(), nonce.end(), header.begin() + 32);
    return header;
}

bool validateHeader(const std::array<uint8_t, InfinityCryptoCodec::kHeaderSize>& header,
                    uint64_t encrypted_size,
                    const InfinityCryptoOptions& options,
                    uint64_t* plaintext_size,
                    std::array<uint8_t, InfinityCryptoCodec::kNonceSize>* nonce,
                    std::string* error) {
    if (!std::equal(kMagic.begin(), kMagic.end(), header.begin())) {
        return fail(error, "file is not an encrypted Infinity envelope");
    }
    if (readLe32(header.data() + 8) != InfinityCryptoCodec::kEnvelopeVersion) {
        return fail(error, "unsupported Infinity encryption version");
    }
    if (readLe32(header.data() + 12) != InfinityCryptoCodec::kHeaderSize) {
        return fail(error, "invalid Infinity encryption header size");
    }
    if (readLe32(header.data() + 16) != kAlgorithmAes256Gcm) {
        return fail(error, "unsupported Infinity encryption algorithm");
    }
    if (readLe32(header.data() + 20) != 0) {
        return fail(error, "unsupported Infinity encryption flags");
    }
    for (size_t index = 44; index < header.size(); ++index) {
        if (header[index] != 0) {
            return fail(error, "invalid reserved Infinity encryption header data");
        }
    }

    const uint64_t decoded_size = readLe64(header.data() + 24);
    constexpr uint64_t overhead = InfinityCryptoCodec::kHeaderSize +
        InfinityCryptoCodec::kTagSize;
    if (decoded_size > std::numeric_limits<uint64_t>::max() - overhead ||
        encrypted_size != decoded_size + overhead) {
        return fail(error, "Infinity encrypted file length does not match its header");
    }
    if (decoded_size > kMaximumGcmPlaintextBytes) {
        return fail(error, "Infinity encrypted payload exceeds the AES-GCM size limit");
    }
    if (options.maximum_plaintext_bytes != 0 &&
        decoded_size > options.maximum_plaintext_bytes) {
        return fail(error, "Infinity encrypted payload exceeds the configured size limit");
    }
    *plaintext_size = decoded_size;
    std::copy_n(header.begin() + 32, nonce->size(), nonce->begin());
    return true;
}

bool validateOperationPaths(const std::string& input_path,
                            const std::string& output_path,
                            std::string* error) {
    if (!validPath(input_path) || !validPath(output_path)) {
        return fail(error, "Infinity crypto path is invalid");
    }
    const std::string part_path = output_path + ".part";
    if (input_path == output_path || input_path == part_path) {
        return fail(error, "Infinity crypto input and output paths must be distinct");
    }
    return true;
}

}  // namespace

bool InfinityCryptoCodec::encryptFileAtomic(const std::string& plaintext_path,
                                            const std::string& encrypted_path,
                                            const Key& key,
                                            const InfinityCryptoOptions& options,
                                            std::string* error) {
    if (error) error->clear();
    if (!validateOperationPaths(plaintext_path, encrypted_path, error)) return false;
    if (cancelled(options)) return fail(error, "Infinity encryption cancelled");

    std::ifstream input(plaintext_path, std::ios::binary);
    uint64_t plaintext_size = 0;
    if (!input || !getFileSize(&input, &plaintext_size)) {
        return fail(error, "cannot open Infinity plaintext input");
    }
    if (plaintext_size > kMaximumGcmPlaintextBytes) {
        return fail(error, "Infinity plaintext exceeds the AES-GCM size limit");
    }
    if (options.maximum_plaintext_bytes != 0 &&
        plaintext_size > options.maximum_plaintext_bytes) {
        return fail(error, "Infinity plaintext exceeds the configured size limit");
    }

    std::array<uint8_t, kNonceSize> nonce{};
    ERR_clear_error();
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        return fail(error, cryptoError("cannot generate a secure Infinity nonce"));
    }
    const auto header = makeHeader(plaintext_size, nonce);

    CipherContext context(EVP_CIPHER_CTX_new());
    if (!context) return fail(error, cryptoError("cannot create Infinity cipher context"));
    int produced = 0;
    if (EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1 ||
        EVP_EncryptUpdate(context.get(), nullptr, &produced, header.data(),
                          static_cast<int>(header.size())) != 1) {
        return fail(error, cryptoError("cannot initialize Infinity encryption"));
    }

    const std::string part_path = encrypted_path + ".part";
    std::remove(part_path.c_str());
    TemporaryFileGuard guard(part_path);
    std::ofstream output(part_path, std::ios::binary | std::ios::trunc);
    if (!output) return fail(error, "cannot create Infinity encrypted temporary file");
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    if (!output) return fail(error, "cannot write Infinity encryption header");

    try {
        std::vector<uint8_t> input_buffer(kIoChunkSize);
        std::vector<uint8_t> output_buffer(kIoChunkSize + EVP_MAX_BLOCK_LENGTH);
        uint64_t remaining = plaintext_size;
        while (remaining != 0) {
            if (cancelled(options)) return fail(error, "Infinity encryption cancelled");
            const size_t requested = static_cast<size_t>(
                std::min<uint64_t>(remaining, input_buffer.size()));
            input.read(reinterpret_cast<char*>(input_buffer.data()),
                       static_cast<std::streamsize>(requested));
            if (input.gcount() != static_cast<std::streamsize>(requested)) {
                return fail(error, "Infinity plaintext changed while it was being encrypted");
            }
            if (EVP_EncryptUpdate(context.get(), output_buffer.data(), &produced,
                                  input_buffer.data(), static_cast<int>(requested)) != 1) {
                return fail(error, cryptoError("cannot encrypt Infinity payload"));
            }
            output.write(reinterpret_cast<const char*>(output_buffer.data()), produced);
            if (!output) return fail(error, "cannot write Infinity encrypted payload");
            remaining -= requested;
        }
        if (input.peek() != std::char_traits<char>::eof()) {
            return fail(error, "Infinity plaintext changed while it was being encrypted");
        }

        if (EVP_EncryptFinal_ex(context.get(), output_buffer.data(), &produced) != 1) {
            return fail(error, cryptoError("cannot finish Infinity encryption"));
        }
        if (produced != 0) {
            output.write(reinterpret_cast<const char*>(output_buffer.data()), produced);
        }
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory for Infinity encryption buffers");
    }

    std::array<uint8_t, kTagSize> tag{};
    if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(tag.size()), tag.data()) != 1) {
        return fail(error, cryptoError("cannot get Infinity authentication tag"));
    }
    output.write(reinterpret_cast<const char*>(tag.data()),
                 static_cast<std::streamsize>(tag.size()));
    if (!output) return fail(error, "cannot write Infinity authentication tag");
    if (!finishAndPublish(&output, part_path, encrypted_path, options, &guard, error)) {
        return false;
    }
    if (error) error->clear();
    return true;
}

bool InfinityCryptoCodec::decryptFileAtomic(const std::string& encrypted_path,
                                            const std::string& plaintext_path,
                                            const Key& key,
                                            const InfinityCryptoOptions& options,
                                            std::string* error) {
    if (error) error->clear();
    if (!validateOperationPaths(encrypted_path, plaintext_path, error)) return false;
    if (cancelled(options)) return fail(error, "Infinity decryption cancelled");

    std::ifstream input(encrypted_path, std::ios::binary);
    uint64_t encrypted_size = 0;
    if (!input || !getFileSize(&input, &encrypted_size)) {
        return fail(error, "cannot open Infinity encrypted input");
    }
    if (encrypted_size < kHeaderSize + kTagSize) {
        return fail(error, "Infinity encrypted file is truncated");
    }
    std::array<uint8_t, kHeaderSize> header{};
    input.read(reinterpret_cast<char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
        return fail(error, "cannot read Infinity encryption header");
    }
    uint64_t plaintext_size = 0;
    std::array<uint8_t, kNonceSize> nonce{};
    if (!validateHeader(header, encrypted_size, options, &plaintext_size, &nonce, error)) {
        return false;
    }

    ERR_clear_error();
    CipherContext context(EVP_CIPHER_CTX_new());
    if (!context) return fail(error, cryptoError("cannot create Infinity cipher context"));
    int produced = 0;
    if (EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1 ||
        EVP_DecryptUpdate(context.get(), nullptr, &produced, header.data(),
                          static_cast<int>(header.size())) != 1) {
        return fail(error, cryptoError("cannot initialize Infinity decryption"));
    }

    const std::string part_path = plaintext_path + ".part";
    std::remove(part_path.c_str());
    TemporaryFileGuard guard(part_path);
    std::ofstream output(part_path, std::ios::binary | std::ios::trunc);
    if (!output) return fail(error, "cannot create Infinity plaintext temporary file");

    try {
        std::vector<uint8_t> input_buffer(kIoChunkSize);
        std::vector<uint8_t> output_buffer(kIoChunkSize + EVP_MAX_BLOCK_LENGTH);
        uint64_t remaining = plaintext_size;
        while (remaining != 0) {
            if (cancelled(options)) return fail(error, "Infinity decryption cancelled");
            const size_t requested = static_cast<size_t>(
                std::min<uint64_t>(remaining, input_buffer.size()));
            input.read(reinterpret_cast<char*>(input_buffer.data()),
                       static_cast<std::streamsize>(requested));
            if (input.gcount() != static_cast<std::streamsize>(requested)) {
                return fail(error, "Infinity encrypted payload is truncated");
            }
            if (EVP_DecryptUpdate(context.get(), output_buffer.data(), &produced,
                                  input_buffer.data(), static_cast<int>(requested)) != 1) {
                return fail(error, cryptoError("cannot decrypt Infinity payload"));
            }
            output.write(reinterpret_cast<const char*>(output_buffer.data()), produced);
            if (!output) return fail(error, "cannot write Infinity plaintext payload");
            remaining -= requested;
        }

        std::array<uint8_t, kTagSize> tag{};
        input.read(reinterpret_cast<char*>(tag.data()),
                   static_cast<std::streamsize>(tag.size()));
        if (input.gcount() != static_cast<std::streamsize>(tag.size())) {
            return fail(error, "Infinity encrypted file has no authentication tag");
        }
        if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG,
                                static_cast<int>(tag.size()), tag.data()) != 1) {
            return fail(error, cryptoError("cannot set Infinity authentication tag"));
        }
        ERR_clear_error();
        if (EVP_DecryptFinal_ex(context.get(), output_buffer.data(), &produced) != 1) {
            return fail(error,
                        "Infinity authentication failed: wrong key or corrupted file");
        }
        if (produced != 0) {
            output.write(reinterpret_cast<const char*>(output_buffer.data()), produced);
            if (!output) return fail(error, "cannot finish Infinity plaintext payload");
        }
    } catch (const std::bad_alloc&) {
        return fail(error, "not enough memory for Infinity decryption buffers");
    }

    if (!finishAndPublish(&output, part_path, plaintext_path, options, &guard, error)) {
        return false;
    }
    if (error) error->clear();
    return true;
}

bool InfinityCryptoCodec::probeEncryptedFile(const std::string& path,
                                             bool* encrypted,
                                             std::string* error) {
    if (error) error->clear();
    if (!encrypted) return fail(error, "Infinity encryption probe result is null");
    *encrypted = false;
    if (!validPath(path)) return fail(error, "Infinity encryption probe path is invalid");
    std::ifstream input(path, std::ios::binary);
    if (!input) return fail(error, "cannot open file for Infinity encryption probe");
    std::array<uint8_t, kMagic.size()> prefix{};
    input.read(reinterpret_cast<char*>(prefix.data()),
               static_cast<std::streamsize>(prefix.size()));
    if (input.gcount() != static_cast<std::streamsize>(prefix.size())) return true;
    *encrypted = std::equal(kMagic.begin(), kMagic.end(), prefix.begin());
    return true;
}

}  // namespace build_import
