#include "InfinityCryptoCodec.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using build_import::InfinityCryptoCodec;

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    assert(output);
}

bool containsText(const std::vector<uint8_t>& bytes, const std::string& text) {
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) != bytes.end();
}

void removeTestFile(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + ".part").c_str());
}

}  // namespace

int main() {
    const std::string source_path = "infinity_crypto_source.bin";
    const std::string encrypted_path = "infinity_crypto_one.infinity";
    const std::string encrypted_again_path = "infinity_crypto_two.infinity";
    const std::string decrypted_path = "infinity_crypto_decrypted.bin";
    const std::string tampered_path = "infinity_crypto_tampered.infinity";
    const std::string rejected_path = "infinity_crypto_rejected.bin";
    const std::string truncated_path = "infinity_crypto_truncated.infinity";
    const std::array<std::string, 7> paths = {
        source_path, encrypted_path, encrypted_again_path, decrypted_path,
        tampered_path, rejected_path, truncated_path,
    };
    for (const std::string& path : paths) removeTestFile(path);

    const std::string sensitive =
        "minecraft:redstone_torch command=title @a secret_palette_text";
    std::vector<uint8_t> plaintext(700'123U);
    for (size_t index = 0; index < plaintext.size(); ++index) {
        plaintext[index] = static_cast<uint8_t>((index * 131U + 17U) & 0xffU);
    }
    std::copy(sensitive.begin(), sensitive.end(), plaintext.begin() + 300'000U);
    writeFile(source_path, plaintext);

    InfinityCryptoCodec::Key key{};
    for (size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<uint8_t>(index * 7U + 3U);
    }
    std::string error;
    assert(InfinityCryptoCodec::encryptFileAtomic(source_path, encrypted_path,
                                                   key, {}, &error));
    assert(error.empty());
    const std::vector<uint8_t> encrypted = readFile(encrypted_path);
    assert(encrypted.size() == plaintext.size() + InfinityCryptoCodec::kHeaderSize +
           InfinityCryptoCodec::kTagSize);
    assert(!containsText(encrypted, sensitive));

    bool is_encrypted = false;
    assert(InfinityCryptoCodec::probeEncryptedFile(encrypted_path, &is_encrypted, &error));
    assert(is_encrypted);
    assert(InfinityCryptoCodec::probeEncryptedFile(source_path, &is_encrypted, &error));
    assert(!is_encrypted);

    assert(InfinityCryptoCodec::decryptFileAtomic(encrypted_path, decrypted_path,
                                                   key, {}, &error));
    assert(error.empty());
    assert(readFile(decrypted_path) == plaintext);

    // A fresh random GCM nonce must produce a different envelope for the same
    // payload and key.
    assert(InfinityCryptoCodec::encryptFileAtomic(source_path, encrypted_again_path,
                                                   key, {}, &error));
    const std::vector<uint8_t> encrypted_again = readFile(encrypted_again_path);
    assert(encrypted_again != encrypted);
    assert(!std::equal(encrypted.begin() + 32, encrypted.begin() + 44,
                       encrypted_again.begin() + 32));

    std::vector<uint8_t> tampered = encrypted;
    tampered[InfinityCryptoCodec::kHeaderSize + 123U] ^= 0x80U;
    writeFile(tampered_path, tampered);
    assert(!InfinityCryptoCodec::decryptFileAtomic(tampered_path, rejected_path,
                                                    key, {}, &error));
    assert(error.find("authentication failed") != std::string::npos);
    assert(!std::ifstream(rejected_path, std::ios::binary));
    assert(!std::ifstream(rejected_path + ".part", std::ios::binary));

    InfinityCryptoCodec::Key wrong_key = key;
    wrong_key.front() ^= 0xffU;
    assert(!InfinityCryptoCodec::decryptFileAtomic(encrypted_path, rejected_path,
                                                    wrong_key, {}, &error));
    assert(error.find("authentication failed") != std::string::npos);

    tampered.resize(tampered.size() - 1U);
    writeFile(truncated_path, tampered);
    assert(!InfinityCryptoCodec::decryptFileAtomic(truncated_path, rejected_path,
                                                    key, {}, &error));
    assert(error.find("length") != std::string::npos);

    build_import::InfinityCryptoOptions cancelled;
    cancelled.cancellation_requested = [] { return true; };
    assert(!InfinityCryptoCodec::encryptFileAtomic(source_path, rejected_path,
                                                    key, cancelled, &error));
    assert(error.find("cancelled") != std::string::npos);
    assert(!std::ifstream(rejected_path, std::ios::binary));

    build_import::InfinityCryptoOptions limited;
    limited.maximum_plaintext_bytes = plaintext.size() - 1U;
    assert(!InfinityCryptoCodec::decryptFileAtomic(encrypted_path, rejected_path,
                                                    key, limited, &error));
    assert(error.find("size limit") != std::string::npos);

    for (const std::string& path : paths) removeTestFile(path);
    return 0;
}
