#include "crypto_utils.hpp"
#include <iostream>

namespace crypto {

bool init() {
    if (sodium_init() < 0) {
        return false;
    }
    return true;
}

std::vector<unsigned char> generate_key() {
    std::vector<unsigned char> key(crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
    crypto_aead_xchacha20poly1305_ietf_keygen(key.data());
    return key;
}

std::vector<unsigned char> encrypt_data(const std::vector<unsigned char>& plaintext, const std::vector<unsigned char>& key) {
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
        throw std::invalid_argument("Invalid key size");
    }

    std::vector<unsigned char> result(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    randombytes_buf(result.data(), crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    
    unsigned long long ciphertext_len;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        result.data() + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, &ciphertext_len,
        plaintext.data(), plaintext.size(),
        nullptr, 0, // Additional data
        nullptr, // Secret nonce
        result.data(), // Nonce
        key.data()
    );
    
    result.resize(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + ciphertext_len);
    return result;
}

std::vector<unsigned char> decrypt_data(const std::vector<unsigned char>& ciphertext_with_nonce, const std::vector<unsigned char>& key) {
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
        throw std::invalid_argument("Invalid key size");
    }

    if (ciphertext_with_nonce.size() < crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        throw std::runtime_error("Ciphertext too short (missing nonce or MAC)");
    }

    const unsigned char* nonce = ciphertext_with_nonce.data();
    const unsigned char* ciphertext = ciphertext_with_nonce.data() + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    unsigned long long ciphertext_len = ciphertext_with_nonce.size() - crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;

    std::vector<unsigned char> decrypted(ciphertext_len - crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long decrypted_len;

    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
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
