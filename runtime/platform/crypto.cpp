#include "crypto.h"

#include <cstring>

namespace tz::crypto {
namespace {

// ---- SHA-256 (FIPS 180-4) ------------------------------------------------------------

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t rotr(uint32_t value, int bits) { return (value >> bits) | (value << (32 - bits)); }

void compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(block[i * 4]) << 24 | uint32_t(block[i * 4 + 1]) << 16 | uint32_t(block[i * 4 + 2]) << 8 |
               block[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6],
             h = state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRoundConstants[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

// ---- AES-128 decryption (FIPS 197) -----------------------------------------------------

struct AesTables {
    uint8_t sbox[256], inverse[256];
    AesTables() {
        // The S-box: multiplicative inverse in GF(2^8) followed by the affine map.
        uint8_t p = 1, q = 1;
        do {
            p = uint8_t(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));
            q ^= q << 1; q ^= q << 2; q ^= q << 4;
            if (q & 0x80) q ^= 0x09;
            const uint8_t x = uint8_t(q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4));
            sbox[p] = uint8_t(x ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i) inverse[sbox[i]] = uint8_t(i);
    }
};
const AesTables& tables() {
    static const AesTables instance;
    return instance;
}

uint8_t xtime(uint8_t value) { return uint8_t(value << 1 ^ (value & 0x80 ? 0x1B : 0)); }
uint8_t multiply(uint8_t a, uint8_t b) {
    uint8_t result = 0;
    for (; b; b >>= 1, a = xtime(a))
        if (b & 1) result ^= a;
    return result;
}

void expandKey(const uint8_t key[16], uint8_t schedule[176]) {
    const auto& t = tables();
    std::memcpy(schedule, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t word[4] = {schedule[i - 4], schedule[i - 3], schedule[i - 2], schedule[i - 1]};
        if (i % 16 == 0) {
            const uint8_t first = word[0];
            word[0] = uint8_t(t.sbox[word[1]] ^ rcon);
            word[1] = t.sbox[word[2]];
            word[2] = t.sbox[word[3]];
            word[3] = t.sbox[first];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; ++j) schedule[i + j] = uint8_t(schedule[i - 16 + j] ^ word[j]);
    }
}

void decryptBlock(const uint8_t schedule[176], uint8_t block[16]) {
    const auto& t = tables();
    for (int i = 0; i < 16; ++i) block[i] ^= schedule[160 + i];
    for (int round = 9; round >= 0; --round) {
        // Inverse ShiftRows (column-major state) + inverse SubBytes.
        uint8_t shifted[16];
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row) shifted[((column + row) % 4) * 4 + row] = block[column * 4 + row];
        for (int i = 0; i < 16; ++i) block[i] = uint8_t(t.inverse[shifted[i]] ^ schedule[round * 16 + i]);
        if (round == 0) break;
        for (int column = 0; column < 4; ++column) {
            uint8_t* c = block + column * 4;
            const uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
            c[0] = uint8_t(multiply(a0, 14) ^ multiply(a1, 11) ^ multiply(a2, 13) ^ multiply(a3, 9));
            c[1] = uint8_t(multiply(a0, 9) ^ multiply(a1, 14) ^ multiply(a2, 11) ^ multiply(a3, 13));
            c[2] = uint8_t(multiply(a0, 13) ^ multiply(a1, 9) ^ multiply(a2, 14) ^ multiply(a3, 11));
            c[3] = uint8_t(multiply(a0, 11) ^ multiply(a1, 13) ^ multiply(a2, 9) ^ multiply(a3, 14));
        }
    }
}

}  // namespace

std::array<uint8_t, 32> sha256(const void* data, size_t size) {
    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    for (; offset + 64 <= size; offset += 64) compress(state, bytes + offset);
    uint8_t tail[128] = {};
    const size_t rest = size - offset;
    std::memcpy(tail, bytes + offset, rest);
    tail[rest] = 0x80;
    const size_t blocks = rest + 9 <= 64 ? 1 : 2;
    const uint64_t bits = uint64_t(size) * 8;
    for (int i = 0; i < 8; ++i) tail[blocks * 64 - 1 - i] = uint8_t(bits >> (i * 8));
    for (size_t i = 0; i < blocks; ++i) compress(state, tail + i * 64);
    std::array<uint8_t, 32> digest{};
    for (int i = 0; i < 32; ++i) digest[i] = uint8_t(state[i / 4] >> (24 - (i % 4) * 8));
    return digest;
}

std::string sha256Hex(const void* data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (const uint8_t byte : sha256(data, size)) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

void aes128CbcDecrypt(const uint8_t key[16], const uint8_t iv[16], uint8_t* data, size_t size) {
    uint8_t schedule[176];
    expandKey(key, schedule);
    uint8_t previous[16], cipher[16];
    std::memcpy(previous, iv, 16);
    for (size_t offset = 0; offset + 16 <= size; offset += 16) {
        std::memcpy(cipher, data + offset, 16);
        decryptBlock(schedule, data + offset);
        for (int i = 0; i < 16; ++i) data[offset + i] ^= previous[i];
        std::memcpy(previous, cipher, 16);
    }
}

}  // namespace tz::crypto
