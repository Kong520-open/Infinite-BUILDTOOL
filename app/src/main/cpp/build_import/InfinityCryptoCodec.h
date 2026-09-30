#ifndef INFINITE_TEXTURE_INFINITY_CRYPTO_CODEC_H
#define INFINITE_TEXTURE_INFINITY_CRYPTO_CODEC_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace build_import {

struct InfinityCryptoOptions {
    std::function<bool()> cancellation_requested;
    std::shared_ptr<std::mutex> publish_mutex;
    std::atomic<bool>* publication_committed = nullptr;
    // Zero means no caller-imposed limit. The encrypted file length is still
    // validated before any plaintext is written.
    uint64_t maximum_plaintext_bytes = 0;
};

// Authenticated, streaming envelope for an Infinity building payload. The
// caller owns key provisioning: no key or password-derived material is stored
// in the envelope, and this class intentionally has no built-in fallback key.
class InfinityCryptoCodec {
public:
    using Key = std::array<uint8_t, 32>;

    static constexpr uint32_t kEnvelopeVersion = 1;
    static constexpr size_t kKeySize = 32;
    static constexpr size_t kNonceSize = 12;
    static constexpr size_t kTagSize = 16;
    static constexpr size_t kHeaderSize = 64;

    static bool encryptFileAtomic(const std::string& plaintext_path,
                                  const std::string& encrypted_path,
                                  const Key& key,
                                  const InfinityCryptoOptions& options = {},
                                  std::string* error = nullptr);

    static bool decryptFileAtomic(const std::string& encrypted_path,
                                  const std::string& plaintext_path,
                                  const Key& key,
                                  const InfinityCryptoOptions& options = {},
                                  std::string* error = nullptr);

    // A successful probe only answers whether the envelope magic is present.
    // Full version, size and authentication checks happen during decryption.
    static bool probeEncryptedFile(const std::string& path,
                                   bool* encrypted,
                                   std::string* error = nullptr);
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_INFINITY_CRYPTO_CODEC_H
