#pragma once

#include <vector>
#include <string>
#include <stdexcept>
#include <sodium.h>

namespace crypto {

    // Initialize the libsodium library. Must be called once before any crypto operations.
    // Returns true on success, false on failure.
    bool init();

    // Generates a random 256-bit key suitable for XChaCha20-Poly1305
    std::vector<unsigned char> generate_key();

    // Encrypts plaintext using XChaCha20-Poly1305
    // Returns a vector containing [nonce (24 bytes) || ciphertext || mac (16 bytes)]
    std::vector<unsigned char> encrypt_data(const std::vector<unsigned char>& plaintext, const std::vector<unsigned char>& key);

    // Decrypts ciphertext (which must include the prepended 24-byte nonce) using XChaCha20-Poly1305
    // Returns the plaintext on success, throws std::runtime_error on failure
    std::vector<unsigned char> decrypt_data(const std::vector<unsigned char>& ciphertext_with_nonce, const std::vector<unsigned char>& key);

} // namespace crypto
