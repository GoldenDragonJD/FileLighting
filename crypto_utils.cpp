#include "crypto_utils.hpp"
#include <iostream>

namespace crypto {

bool init() {
    if (sodium_init() < 0) {
        return false;
    }
    return true;
}

bool is_aes256gcm_available() {
    return crypto_aead_aes256gcm_is_available() == 1;
}

std::vector<unsigned char> generate_key() {
    std::vector<unsigned char> key(crypto_aead_aes256gcm_KEYBYTES);
    crypto_aead_aes256gcm_keygen(key.data());
    return key;
}

std::vector<unsigned char> encrypt_data(const std::vector<unsigned char>& plaintext, const std::vector<unsigned char>& key) {
    if (!is_aes256gcm_available()) {
        throw std::runtime_error("Hardware AES-256-GCM is not available on this CPU");
    }

    if (key.size() != crypto_aead_aes256gcm_KEYBYTES) {
        throw std::invalid_argument("Invalid key size");
    }

    std::vector<unsigned char> nonce(crypto_aead_aes256gcm_NPUBBYTES);
    randombytes_buf(nonce.data(), nonce.size());

    std::vector<unsigned char> ciphertext(plaintext.size() + crypto_aead_aes256gcm_ABYTES);
    unsigned long long ciphertext_len;

    crypto_aead_aes256gcm_encrypt(
        ciphertext.data(), &ciphertext_len,
        plaintext.data(), plaintext.size(),
        nullptr, 0, // Additional data
        nullptr, // Secret nonce
        nonce.data(),
        key.data()
    );

    std::vector<unsigned char> result;
    result.reserve(nonce.size() + ciphertext_len);
    result.insert(result.end(), nonce.begin(), nonce.end());
    result.insert(result.end(), ciphertext.begin(), ciphertext.begin() + ciphertext_len);

    return result;
}

std::vector<unsigned char> decrypt_data(const std::vector<unsigned char>& ciphertext_with_nonce, const std::vector<unsigned char>& key) {
    if (!is_aes256gcm_available()) {
        throw std::runtime_error("Hardware AES-256-GCM is not available on this CPU");
    }

    if (key.size() != crypto_aead_aes256gcm_KEYBYTES) {
        throw std::invalid_argument("Invalid key size");
    }

    if (ciphertext_with_nonce.size() < crypto_aead_aes256gcm_NPUBBYTES + crypto_aead_aes256gcm_ABYTES) {
        throw std::runtime_error("Ciphertext too short (missing nonce or MAC)");
    }

    const unsigned char* nonce = ciphertext_with_nonce.data();
    const unsigned char* ciphertext = ciphertext_with_nonce.data() + crypto_aead_aes256gcm_NPUBBYTES;
    unsigned long long ciphertext_len = ciphertext_with_nonce.size() - crypto_aead_aes256gcm_NPUBBYTES;

    std::vector<unsigned char> decrypted(ciphertext_len - crypto_aead_aes256gcm_ABYTES);
    unsigned long long decrypted_len;

    if (crypto_aead_aes256gcm_decrypt(
        decrypted.data(), &decrypted_len,
        nullptr,
        ciphertext, ciphertext_len,
        nullptr, 0,
        nonce,
        key.data()
    ) != 0) {
        throw std::runtime_error("Decryption failed (forged data or incorrect key)");
    }

    decrypted.resize(decrypted_len);
    return decrypted;
}

} // namespace crypto
