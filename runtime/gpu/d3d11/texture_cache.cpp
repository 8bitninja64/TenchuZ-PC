#include "texture_cache.h"
#include "host/settings.h"
#include "kernel/kernel.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace tz::gpu {
namespace {

template <typename T> void release(T*& object) {
    if (object) object->Release();
    object = nullptr;
}

// How a guest format is stored on the host.
struct FormatInfo {
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    uint32_t blockSize = 1;     // texels per block edge (4 for BC formats)
    uint32_t bytesPerBlock = 4;
    enum class Convert { Copy, R5G6B5, R5G5B5A1, R4G4B4A4 } convert = Convert::Copy;
};

bool formatInfo(uint32_t format, FormatInfo& info) {
    using C = FormatInfo::Convert;
    switch (format) {
    case 2: info = {DXGI_FORMAT_R8_UNORM, 1, 1}; return true;                       // 8
    case 3: info = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 2, C::R5G5B5A1}; return true;     // 1_5_5_5
    case 4: info = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 2, C::R5G6B5}; return true;       // 5_6_5
    case 6: case 14: info = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 4}; return true;        // 8_8_8_8
    case 7: info = {DXGI_FORMAT_R10G10B10A2_UNORM, 1, 4}; return true;              // 2_10_10_10
    case 10: info = {DXGI_FORMAT_R8G8_UNORM, 1, 2}; return true;                    // 8_8
    case 15: info = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 2, C::R4G4B4A4}; return true;    // 4_4_4_4
    case 18: info = {DXGI_FORMAT_BC1_UNORM, 4, 8}; return true;                     // DXT1
    case 19: info = {DXGI_FORMAT_BC2_UNORM, 4, 16}; return true;                    // DXT2_3
    case 20: info = {DXGI_FORMAT_BC3_UNORM, 4, 16}; return true;                    // DXT4_5
    case 24: info = {DXGI_FORMAT_R16_UNORM, 1, 2}; return true;                     // 16
    case 25: info = {DXGI_FORMAT_R16G16_UNORM, 1, 4}; return true;                  // 16_16
    case 26: info = {DXGI_FORMAT_R16G16B16A16_UNORM, 1, 8}; return true;            // 16_16_16_16
    case 30: info = {DXGI_FORMAT_R16_FLOAT, 1, 2}; return true;
    case 31: info = {DXGI_FORMAT_R16G16_FLOAT, 1, 4}; return true;
    case 32: info = {DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 8}; return true;
    case 36: info = {DXGI_FORMAT_R32_FLOAT, 1, 4}; return true;
    case 37: info = {DXGI_FORMAT_R32G32_FLOAT, 1, 8}; return true;
    case 38: info = {DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16}; return true;
    case 49: info = {DXGI_FORMAT_BC5_UNORM, 4, 16}; return true;                    // DXN
    case 59: info = {DXGI_FORMAT_BC4_UNORM, 4, 8}; return true;                     // DXT5A
    default: return false;
    }
}

uint32_t log2Bytes(uint32_t bytes) {
    uint32_t log = 0;
    while ((1u << log) < bytes) ++log;
    return log;
}

// Block offset of (x, y) in a 2D tiled surface whose width (in blocks) is
// padded to 32. Tiles are 32x32 blocks with a fixed interleave inside.
uint32_t tiledOffset(uint32_t x, uint32_t y, uint32_t widthInBlocks, uint32_t logBpp) {
    const uint32_t alignedWidth = (widthInBlocks + 31) & ~31u;
    const uint32_t macro = ((x >> 5) + (y >> 5) * (alignedWidth >> 5)) << (logBpp + 7);
    const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << logBpp;
    const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + logBpp)) + ((y & 1) << 4);
    return (((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> logBpp;
}

void swapEndian(uint8_t* data, size_t size, uint32_t endian) {
    for (size_t i = 0; i + 3 < size; i += 4) {
        uint32_t v;
        std::memcpy(&v, data + i, 4);
        switch (endian) {
        case 1: v = ((v << 8) & 0xFF00FF00) | ((v >> 8) & 0x00FF00FF); break;
        case 2: v = _byteswap_ulong(v); break;
        case 3: v = (v << 16) | (v >> 16); break;
        default: break;
        }
        std::memcpy(data + i, &v, 4);
    }
}

uint64_t hashBytes(const uint8_t* data, size_t size) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i + 8 <= size; i += 8) {
        uint64_t word;
        std::memcpy(&word, data + i, 8);
        hash = (hash ^ word) * 1099511628211ull;
    }
    return hash;
}

uint64_t sampledHash(const uint8_t* data, size_t size) {
    uint64_t hash = 1469598103934665603ull;
    const size_t words = size / 8;
    const size_t step = (std::max<size_t>)(1, words / 256);
    for (size_t i = 0; i < words; i += step) {
        uint64_t word;
        std::memcpy(&word, data + i * 8, 8);
        hash = (hash ^ word) * 1099511628211ull;
    }
    return hash;
}

D3D11_TEXTURE_ADDRESS_MODE addressMode(uint32_t clamp) {
    switch (clamp) {
    case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 2: case 4: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 3: case 5: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default: return D3D11_TEXTURE_ADDRESS_BORDER;
    }
}

// Copies a texture's top level into a render target (for formats without
// automatic mip generation, such as BC).
const char* kExpandShader = R"(
Texture2D<float4> source : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 position : SV_Position) : SV_Target { return source.Load(int3(position.xy, 0)); }
)";

// Host format holding the mip chain: GenerateMips needs a renderable format.
DXGI_FORMAT mipFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return format;
    }
}

}  // namespace

void TextureCache::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    device_ = device;
    context_ = context;
    ID3DBlob* code = nullptr;
    if (SUCCEEDED(D3DCompile(kExpandShader, strlen(kExpandShader), "expand", nullptr, nullptr, "vs", "vs_5_0", 0, 0,
                             &code, nullptr))) {
        device_->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &expandVertex_);
        release(code);
    }
    if (SUCCEEDED(D3DCompile(kExpandShader, strlen(kExpandShader), "expand", nullptr, nullptr, "ps", "ps_5_0", 0, 0,
                             &code, nullptr))) {
        device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &expandPixel_);
        release(code);
    }
}

// Builds a full mip chain from the top level. The guest's own mip levels are
// not read (their packed layout is not implemented); box-filtered host mips
// are a close stand-in and remove the shimmer of unfiltered minification.
ID3D11ShaderResourceView* TextureCache::withMips(ID3D11Texture2D* base, ID3D11ShaderResourceView* baseView,
                                                 uint32_t levels) {
    D3D11_TEXTURE2D_DESC baseDesc{};
    base->GetDesc(&baseDesc);
    const DXGI_FORMAT format = mipFormat(baseDesc.Format);
    UINT support = 0;
    if (FAILED(device_->CheckFormatSupport(format, &support)) || !(support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN))
        return nullptr;
    D3D11_TEXTURE2D_DESC desc = baseDesc;
    desc.Format = format;
    desc.MipLevels = levels;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture))) return nullptr;
    if (baseDesc.Format == format) {
        context_->CopySubresourceRegion(texture, 0, 0, 0, 0, base, 0, nullptr);
    } else {
        ID3D11RenderTargetView* target = nullptr;
        D3D11_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = format;
        rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        device_->CreateRenderTargetView(texture, &rtv, &target);
        if (!target || !expandVertex_ || !expandPixel_) {
            release(target);
            release(texture);
            return nullptr;
        }
        // Draw state is rebuilt for every guest draw, so it can be overwritten here.
        const D3D11_VIEWPORT viewport{0, 0, float(desc.Width), float(desc.Height), 0, 1};
        context_->OMSetRenderTargets(1, &target, nullptr);
        context_->RSSetViewports(1, &viewport);
        context_->RSSetState(nullptr);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(nullptr, 0);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(expandVertex_, nullptr, 0);
        context_->GSSetShader(nullptr, nullptr, 0);
        context_->PSSetShader(expandPixel_, nullptr, 0);
        context_->PSSetShaderResources(0, 1, &baseView);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        context_->PSSetShaderResources(0, 1, &none);
        ID3D11RenderTargetView* noTarget = nullptr;
        context_->OMSetRenderTargets(1, &noTarget, nullptr);
        release(target);
    }
    ID3D11ShaderResourceView* view = nullptr;
    device_->CreateShaderResourceView(texture, nullptr, &view);
    if (view) context_->GenerateMips(view);
    release(texture);  // the view keeps it alive
    return view;
}

void TextureCache::invalidate(uint32_t physicalAddress) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (std::get<0>(it->first) == physicalAddress) {
            release(it->second.view);
            release(it->second.texture);
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

ID3D11ShaderResourceView* TextureCache::view(const TextureFetch& fetch) {
    if (fetch.type() != 2 || fetch.dimension() != 1) return nullptr;  // 2D only for now
    FormatInfo info;
    if (!formatInfo(fetch.format(), info)) {
        TZ_TRACE("texture", "[texture] unsupported format %u", fetch.format());
        return nullptr;
    }
    const uint32_t width = fetch.width(), height = fetch.height();
    const uint32_t blocksWide = (width + info.blockSize - 1) / info.blockSize;
    const uint32_t blocksHigh = (height + info.blockSize - 1) / info.blockSize;
    // Rows are padded to the pitch (texels) and tiles to 32 blocks.
    uint32_t pitchBlocks = (std::max)(fetch.pitchTexels() / info.blockSize, blocksWide);
    const uint32_t paddedWide = (pitchBlocks + 31) & ~31u, paddedHigh = (blocksHigh + 31) & ~31u;
    const size_t guestSize = fetch.tiled() ? size_t(paddedWide) * paddedHigh * info.bytesPerBlock
                                           : size_t(pitchBlocks) * blocksHigh * info.bytesPerBlock;
    const uint32_t address = fetch.baseAddress() & 0x1FFFFFFF;
    if (!address || guestSize > (64u << 20)) return nullptr;

    const auto key = std::make_tuple(address, fetch.format(), width, height, fetch.words[0] & 0xFFC00000u);
    auto& entry = entries_[key];
    if (entry.view && entry.lastCheckFrame == frame_) return entry.view;
    const uint8_t* guest = memory->host(Memory::fromPhysical(address));
    // A sampled checksum every frame catches whole-texture updates (movie frames);
    // a full hash every 64th frame (staggered per texture) catches small edits.
    const uint64_t sample = sampledHash(guest, guestSize);
    entry.lastCheckFrame = frame_;
    const bool fullCheck = ((frame_ + address / 4096) & 63) == 0;
    if (entry.view && entry.sample == sample && !fullCheck) return entry.view;
    const uint64_t hash = hashBytes(guest, guestSize);
    entry.sample = sample;
    if (entry.view && entry.hash == hash) return entry.view;

    std::vector<uint8_t> raw(guest, guest + guestSize);
    swapEndian(raw.data(), raw.size(), fetch.endian());
    // Linear blocks in host order.
    std::vector<uint8_t> blocks(size_t(blocksWide) * blocksHigh * info.bytesPerBlock);
    const uint32_t logBpp = log2Bytes(info.bytesPerBlock);
    for (uint32_t y = 0; y < blocksHigh; ++y) {
        for (uint32_t x = 0; x < blocksWide; ++x) {
            const size_t source = fetch.tiled() ? size_t(tiledOffset(x, y, pitchBlocks, logBpp)) * info.bytesPerBlock
                                                : (size_t(y) * pitchBlocks + x) * info.bytesPerBlock;
            if (source + info.bytesPerBlock > raw.size()) continue;
            std::memcpy(&blocks[(size_t(y) * blocksWide + x) * info.bytesPerBlock], &raw[source], info.bytesPerBlock);
        }
    }
    // Expand packed 16-bit formats so channel x is red.
    std::vector<uint8_t> converted;
    const uint8_t* upload = blocks.data();
    uint32_t rowPitch = blocksWide * info.bytesPerBlock;
    if (info.convert != FormatInfo::Convert::Copy) {
        converted.resize(size_t(width) * height * 4);
        for (size_t i = 0; i < size_t(width) * height; ++i) {
            uint16_t v;
            std::memcpy(&v, &blocks[i * 2], 2);
            uint8_t* out = &converted[i * 4];
            switch (info.convert) {
            case FormatInfo::Convert::R5G6B5:
                out[0] = uint8_t((v & 31) * 255 / 31);
                out[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
                out[2] = uint8_t((v >> 11) * 255 / 31);
                out[3] = 255;
                break;
            case FormatInfo::Convert::R5G5B5A1:
                out[0] = uint8_t((v & 31) * 255 / 31);
                out[1] = uint8_t(((v >> 5) & 31) * 255 / 31);
                out[2] = uint8_t(((v >> 10) & 31) * 255 / 31);
                out[3] = (v >> 15) ? 255 : 0;
                break;
            default:  // 4_4_4_4
                out[0] = uint8_t((v & 15) * 17);
                out[1] = uint8_t(((v >> 4) & 15) * 17);
                out[2] = uint8_t(((v >> 8) & 15) * 17);
                out[3] = uint8_t((v >> 12) * 17);
                break;
            }
        }
        upload = converted.data();
        rowPitch = width * 4;
    }

    release(entry.view);
    release(entry.texture);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = info.blockSize > 1 ? blocksWide * info.blockSize : width;
    desc.Height = info.blockSize > 1 ? blocksHigh * info.blockSize : height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = info.dxgi;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{upload, rowPitch, 0};
    if (FAILED(device_->CreateTexture2D(&desc, &data, &entry.texture))) {
        logf("[texture] cannot create %ux%u texture (format %u)", width, height, fetch.format());
        return nullptr;
    }
    device_->CreateShaderResourceView(entry.texture, nullptr, &entry.view);
    // Mipmapped textures (max level in fetch word 4, bits 6-9) get a host mip chain.
    const uint32_t maxLevel = (fetch.words[4] >> 6) & 15;
    if (maxLevel && fetch.mipFilter() != 2 && !traceEnabled("nomips")) {
        uint32_t levels = 1;
        while (levels <= maxLevel && ((std::max)(desc.Width, desc.Height) >> levels)) ++levels;
        if (levels > 1)
            if (auto* mipped = withMips(entry.texture, entry.view, levels)) {
                release(entry.view);
                entry.view = mipped;
            }
    }
    entry.hash = hash;
    TZ_TRACE("texture", "[texture] %08X %ux%u format %u %s endian %u mips %u-%u filter %u/%u/%u mipaddr %08X", address,
             width, height, fetch.format(), fetch.tiled() ? "tiled" : "linear", fetch.endian(),
             (fetch.words[4] >> 2) & 15, (fetch.words[4] >> 6) & 15, fetch.magFilter(), fetch.minFilter(),
             fetch.mipFilter(), fetch.words[5] & 0xFFFFF000);
    return entry.view;
}

ID3D11SamplerState* TextureCache::sampler(const TextureFetch& fetch, uint32_t instructionFilters) {
    // Filter values: 0 point, 1 linear, 2 basemap (mip only: no mipmapping), 3 = take
    // the other source's. The instruction's fields override the constant's.
    auto pick = [](uint32_t instruction, uint32_t constant) { return instruction != 3 ? instruction : constant; };
    const uint32_t mag = pick(instructionFilters & 3, fetch.magFilter());
    const uint32_t min = pick((instructionFilters >> 2) & 3, fetch.minFilter());
    const uint32_t mip = pick((instructionFilters >> 4) & 3, fetch.mipFilter());
    const uint32_t key = fetch.clampX() | fetch.clampY() << 3 | fetch.clampZ() << 6 | mag << 9 | min << 11 | mip << 13;
    auto& state = samplers_[key];
    if (state) return state;
    D3D11_SAMPLER_DESC desc{};
    const bool linearMag = mag != 0, linearMin = min != 0;
    const bool linearMip = mip == 1;
    desc.Filter = D3D11_ENCODE_BASIC_FILTER(linearMin ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                            linearMag ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                            linearMip ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                            D3D11_FILTER_REDUCTION_TYPE_STANDARD);
    // Anisotropic filtering (TenchuZ.ini) for trilinear-filtered textures.
    const int anisotropy = host::settings().anisotropy;
    if (anisotropy > 1 && linearMag && linearMin && linearMip) {
        desc.Filter = D3D11_FILTER_ANISOTROPIC;
        desc.MaxAnisotropy = UINT(std::clamp(anisotropy, 2, 16));
    }
    desc.AddressU = addressMode(fetch.clampX());
    desc.AddressV = addressMode(fetch.clampY());
    desc.AddressW = addressMode(fetch.clampZ());
    desc.MaxLOD = mip == 2 ? 0.0f : D3D11_FLOAT32_MAX;  // basemap: level 0 only
    device_->CreateSamplerState(&desc, &state);
    return state;
}

}  // namespace tz::gpu
