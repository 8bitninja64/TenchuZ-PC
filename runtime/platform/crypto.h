#pragma once
// The two primitives the runtime needs, without an OS crypto library (Windows used BCrypt):
// SHA-256 (default.xex identity, update downloads) and AES-128-CBC decryption (XEX images).
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tz::crypto {

std::array<uint8_t, 32> sha256(const void* data, size_t size);
// Lower-case hex of sha256().
std::string sha256Hex(const void* data, size_t size);

// In place; size must be a multiple of 16.
void aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16], uint8_t* data, size_t size);

}  // namespace tz::crypto
