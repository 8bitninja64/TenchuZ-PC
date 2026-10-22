#include "xex.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace tz {
namespace {

constexpr uint8_t kRetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};

uint32_t be32(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 4 > data.size()) throw std::runtime_error("Truncated XEX field");
    return uint32_t(data[offset]) << 24 | uint32_t(data[offset + 1]) << 16 |
           uint32_t(data[offset + 2]) << 8 | data[offset + 3];
}

uint16_t be16(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 2 > data.size()) throw std::runtime_error("Truncated XEX field");
    return uint16_t(data[offset] << 8 | data[offset + 1]);
}

// AES-128-CBC with a zero IV, in place.
void decryptCbc(const uint8_t* key, uint8_t* data, size_t size) {
    if (size % 16) throw std::runtime_error("Encrypted XEX data is not block aligned");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    auto check = [](NTSTATUS status) {
        if (!BCRYPT_SUCCESS(status)) throw std::runtime_error("AES decryption failed");
    };
    try {
        check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0));
        check(BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
                                sizeof(BCRYPT_CHAIN_MODE_CBC), 0));
        check(BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0, (PUCHAR)key, 16, 0));
        uint8_t iv[16] = {};
        ULONG written = 0;
        check(BCryptDecrypt(handle, data, ULONG(size), nullptr, iv, sizeof(iv), data, ULONG(size), &written, 0));
    } catch (...) {
        if (handle) BCryptDestroyKey(handle);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
    BCryptDestroyKey(handle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
}

}  // namespace

bool XexImage::field(uint32_t key, uint32_t& value) const {
    const uint32_t count = be32(header, 20);
    for (uint32_t i = 0; i < count; ++i) {
        if (be32(header, 24 + i * 8) != key) continue;
        value = be32(header, 28 + i * 8);
        return true;
    }
    return false;
}

XexImage loadXex(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open " + path.string());
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 24 || std::memcmp(data.data(), "XEX2", 4))
        throw std::runtime_error("default.xex is not an XEX2 executable");

    XexImage xex;
    const uint32_t headerSize = be32(data, 8);
    const uint32_t security = be32(data, 16);
    if (headerSize > data.size() || security + 0x180 > headerSize)
        throw std::runtime_error("Invalid XEX header layout");
    xex.header.assign(data.begin(), data.begin() + headerSize);
    const uint32_t imageSize = be32(data, security + 4);
    xex.loadAddress = be32(data, security + 0x110);

    uint32_t formatOffset = 0;
    if (!xex.field(0x3FF, formatOffset)) throw std::runtime_error("XEX has no file format header");
    const uint32_t formatSize = be32(data, formatOffset);
    const uint16_t encryption = be16(data, formatOffset + 4);
    const uint16_t compression = be16(data, formatOffset + 6);
    if (encryption > 1 || compression > 1)
        throw std::runtime_error("Only uncompressed or basic-compressed XEX files are supported");
    if (!xex.field(0x10100, xex.entryPoint)) throw std::runtime_error("XEX has no entry point");

    std::vector<uint8_t> payload(data.begin() + headerSize, data.end());
    if (encryption) {
        uint8_t sessionKey[16];
        std::memcpy(sessionKey, data.data() + security + 0x150, 16);
        decryptCbc(kRetailKey, sessionKey, 16);
        payload.resize(payload.size() & ~size_t(15));
        decryptCbc(sessionKey, payload.data(), payload.size());
    }

    xex.image.assign(imageSize, 0);
    if (compression == 0) {
        if (payload.size() < imageSize) throw std::runtime_error("Truncated XEX image");
        std::memcpy(xex.image.data(), payload.data(), imageSize);
    } else {
        // Basic compression: (data size, zero-fill size) block pairs.
        size_t source = 0, destination = 0;
        for (uint32_t offset = formatOffset + 8; offset + 8 <= formatOffset + formatSize; offset += 8) {
            const uint32_t blockSize = be32(data, offset), zeroSize = be32(data, offset + 4);
            if (source + blockSize > payload.size() || destination + blockSize + zeroSize > imageSize)
                throw std::runtime_error("Invalid XEX block table");
            std::memcpy(xex.image.data() + destination, payload.data() + source, blockSize);
            source += blockSize;
            destination += blockSize + zeroSize;
        }
    }
    if (xex.image.size() < 2 || xex.image[0] != 'M' || xex.image[1] != 'Z')
        throw std::runtime_error("Decoded XEX image is invalid (wrong key or unsupported revision)");
    return xex;
}

}  // namespace tz
