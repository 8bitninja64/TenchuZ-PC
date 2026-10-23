#pragma once
// Guest textures as D3D11 shader resources.
#include <d3d11.h>
#include <cstdint>
#include <map>
#include <tuple>

namespace tz::gpu {

// Decoded texture fetch constant (6 dwords).
struct TextureFetch {
    uint32_t words[6] = {};
    uint32_t type() const { return words[0] & 3; }
    uint32_t clampX() const { return (words[0] >> 10) & 7; }
    uint32_t clampY() const { return (words[0] >> 13) & 7; }
    uint32_t clampZ() const { return (words[0] >> 16) & 7; }
    uint32_t pitchTexels() const { return ((words[0] >> 22) & 0x1FF) * 32; }
    bool tiled() const { return words[0] >> 31; }
    uint32_t format() const { return words[1] & 0x3F; }
    uint32_t endian() const { return (words[1] >> 6) & 3; }
    uint32_t baseAddress() const { return words[1] & 0xFFFFF000; }
    uint32_t width() const { return (words[2] & 0x1FFF) + 1; }    // 2D
    uint32_t height() const { return ((words[2] >> 13) & 0x1FFF) + 1; }
    uint32_t swizzle() const { return (words[3] >> 1) & 0xFFF; }
    uint32_t magFilter() const { return (words[3] >> 19) & 3; }
    uint32_t minFilter() const { return (words[3] >> 21) & 3; }
    uint32_t mipFilter() const { return (words[3] >> 23) & 3; }
    uint32_t dimension() const { return (words[5] >> 9) & 3; }    // 0 1D, 1 2D, 2 3D, 3 cube
};

class TextureCache {
public:
    void initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    // Returns a view for the fetch constant, or nullptr when unsupported.
    ID3D11ShaderResourceView* view(const TextureFetch& fetch);
    // instructionFilters: mag/min/mip from the fetch instruction (2 bits each, 3 = use the constant's).
    ID3D11SamplerState* sampler(const TextureFetch& fetch, uint32_t instructionFilters = 0x3F);
    // Drops cached uploads when a resolve overwrites guest memory.
    void invalidate(uint32_t physicalAddress);
    // Uploads are revalidated against guest memory at most once per frame.
    void beginFrame() { ++frame_; }

private:
    ID3D11ShaderResourceView* withMips(ID3D11Texture2D* base, ID3D11ShaderResourceView* baseView, uint32_t levels);
    ID3D11VertexShader* expandVertex_ = nullptr;
    ID3D11PixelShader* expandPixel_ = nullptr;
    struct Entry {
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* view = nullptr;
        uint64_t hash = 0;
        uint64_t lastCheckFrame = 0;
        uint64_t sample = 0;  // sampled checksum at the last check
        // Validation by write watching; data rewritten in most frames (movies) goes
        // back to per-frame checksums.
        uint64_t lastWriteFrame = 0;
        uint32_t writes = 0;
        bool volatileData = false;
    };
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, Entry> entries_;
    std::map<uint32_t, ID3D11SamplerState*> samplers_;
    uint64_t frame_ = 1;
};

}  // namespace tz::gpu
