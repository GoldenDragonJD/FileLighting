#pragma once

#include <vector>
#include <string>
#include <stdexcept>
#include <sodium.h>

namespace crypto {

    // Initialize the libsodium library. Must be called once before any crypto operations.
    // Returns true on success, false on failure.
    bool init();

    // Check if hardware AES-256-GCM is available on this CPU
    bool is_aes256gcm_available();

    // Generates a random 256-bit key suitable for AES-256-GCM
    std::vector<unsigned char> generate_key();

    // Encrypts plaintext using AES-256-GCM (Hardware accelerated)
    // Returns a vector containing [nonce (12 bytes) || ciphertext || mac (16 bytes)]
    std::vector<unsigned char> encrypt_data(const std::vector<unsigned char>& plaintext, const std::vector<unsigned char>& key);

    // Decrypts ciphertext (which must include the prepended 12-byte nonce) using AES-256-GCM
    // Returns the plaintext on success, throws std::runtime_error on failure
    std::vector<unsigned char> decrypt_data(const std::vector<unsigned char>& ciphertext_with_nonce, const std::vector<unsigned char>& key);

} // namespace crypto
