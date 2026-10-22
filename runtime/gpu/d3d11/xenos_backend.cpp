#include "xenos_backend.h"
#include "gpu/registers.h"
#include "gpu/d3d11/texture_cache.h"
#include "kernel/kernel.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace tz::gpu {
namespace {

template <typename T> void release(T*& object) {
    if (object) object->Release();
    object = nullptr;
}

float asFloat(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

uint32_t gpuSwap(uint32_t value, uint32_t endian) {
    switch (endian & 3) {
    case 1: return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
    case 2: return _byteswap_ulong(value);
    case 3: return (value << 16) | (value >> 16);
    default: return value;
    }
}

// Layout of cbuffer SharedConstants in the translated shaders.
struct SharedConstants {
    uint32_t bools[8];
    uint32_t loops[32];
    uint32_t vertexEndian[16];
    uint32_t vertexBase[16];
    float textureSize[16][4];
    uint32_t textureSwizzle[16];
    float positionScale[4];
    float positionOffset[4];
    float alphaTest[4];
    uint32_t draw[4];
};

// TENCHU_TRACE=flat: every pixel shader becomes flat magenta (geometry debugging).
const char* kFlatPixelShader = R"(
struct Interpolators { float4 position : SV_Position; float4 t[16] : TEXCOORD0; };
float4 main(Interpolators input) : SV_Target { return float4(1, 0, 1, 1); }
)";

// Resolve copy: reads source texels with an optional red/blue swap.
const char* kCopyShader = R"(
Texture2D<float4> source : register(t0);
cbuffer Copy : register(b0) { int2 sourceOrigin; int2 destOrigin; uint swap; uint3 unused; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 position : SV_Position) : SV_Target {
    float4 color = source.Load(int3(int2(position.xy) - destOrigin + sourceOrigin, 0));
    return swap ? color.bgra : color;
}
)";

// Expands Xenos rectangle lists: three corners in, the fourth extrapolated.
const char* kRectangleGeometryShader = R"(
struct Interpolators { float4 position : SV_Position; float4 t[16] : TEXCOORD0; };
[maxvertexcount(4)]
void main(triangle Interpolators v[3], inout TriangleStream<Interpolators> stream) {
    Interpolators fourth;
    fourth.position = v[1].position + v[2].position - v[0].position;
    [unroll] for (int i = 0; i < 16; ++i) fourth.t[i] = v[1].t[i] + v[2].t[i] - v[0].t[i];
    stream.Append(v[0]);
    stream.Append(v[1]);
    stream.Append(v[2]);
    stream.Append(fourth);
}
)";

DXGI_FORMAT colorFormat(uint32_t format) {
    switch (format) {
    case 0: case 1: return DXGI_FORMAT_R8G8B8A8_UNORM;                // 8_8_8_8 (and gamma)
    case 2: case 10: return DXGI_FORMAT_R10G10B10A2_UNORM;            // 2_10_10_10
    case 3: case 12: return DXGI_FORMAT_R16G16B16A16_FLOAT;           // 2_10_10_10_FLOAT
    case 4: case 6: return DXGI_FORMAT_R16G16_FLOAT;                  // 16_16, 16_16_FLOAT
    case 5: case 7: return DXGI_FORMAT_R16G16B16A16_FLOAT;            // 16_16_16_16(_FLOAT)
    case 14: return DXGI_FORMAT_R32_FLOAT;
    case 15: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

D3D11_BLEND blendFactor(uint32_t factor) {
    switch (factor) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 4: return D3D11_BLEND_SRC_COLOR;
    case 5: return D3D11_BLEND_INV_SRC_COLOR;
    case 6: return D3D11_BLEND_SRC_ALPHA;
    case 7: return D3D11_BLEND_INV_SRC_ALPHA;
    case 8: return D3D11_BLEND_DEST_COLOR;
    case 9: return D3D11_BLEND_INV_DEST_COLOR;
    case 10: return D3D11_BLEND_DEST_ALPHA;
    case 11: return D3D11_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D11_BLEND_BLEND_FACTOR;
    case 13: case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D11_BLEND_SRC_ALPHA_SAT;
    default: return D3D11_BLEND_ONE;
    }
}

D3D11_BLEND alphaBlendFactor(uint32_t factor) {
    // Color factors are not valid for the alpha channel.
    switch (factor) {
    case 4: return D3D11_BLEND_SRC_ALPHA;
    case 5: return D3D11_BLEND_INV_SRC_ALPHA;
    case 8: return D3D11_BLEND_DEST_ALPHA;
    case 9: return D3D11_BLEND_INV_DEST_ALPHA;
    default: return blendFactor(factor);
    }
}

D3D11_BLEND_OP blendOperation(uint32_t operation) {
    switch (operation) {
    case 1: return D3D11_BLEND_OP_SUBTRACT;
    case 2: return D3D11_BLEND_OP_MIN;
    case 3: return D3D11_BLEND_OP_MAX;
    case 4: return D3D11_BLEND_OP_REV_SUBTRACT;
    default: return D3D11_BLEND_OP_ADD;
    }
}

D3D11_STENCIL_OP stencilOperation(uint32_t operation) {
    static const D3D11_STENCIL_OP operations[] = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_ZERO,
                                                  D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_INCR_SAT,
                                                  D3D11_STENCIL_OP_DECR_SAT, D3D11_STENCIL_OP_INVERT,
                                                  D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_DECR};
    return operations[operation & 7];
}

D3D11_COMPARISON_FUNC comparison(uint32_t function) {
    return D3D11_COMPARISON_FUNC(D3D11_COMPARISON_NEVER + (function & 7));
}

}  // namespace

struct XenosBackend::Impl {
    D3D11Renderer& renderer;
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    const std::atomic<uint32_t>* registers;
    ShaderSource shaderSource;

    uint32_t reg(uint32_t index) const { return registers[index].load(std::memory_order_relaxed); }

    // ---- Upload ring: vertex data and index lists, read as a raw buffer ----------
    static constexpr uint32_t kRingSize = 64 << 20;
    ID3D11Buffer* ring = nullptr;
    ID3D11ShaderResourceView* ringView = nullptr;
    uint32_t ringOffset = 0;
    uint8_t* ringMapped = nullptr;

    void beginUploads() {
        if (ringMapped) return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        context->Map(ring, 0, ringOffset == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped);
        ringMapped = static_cast<uint8_t*>(mapped.pData);
    }
    void endUploads() {
        if (!ringMapped) return;
        context->Unmap(ring, 0);
        ringMapped = nullptr;
    }
    // Returns the byte offset of the copy, or ~0u if it does not fit.
    uint32_t upload(const void* data, uint32_t size) {
        size = (size + 15) & ~15u;
        if (size > kRingSize) return ~0u;
        if (ringOffset + size > kRingSize) {
            endUploads();
            ringOffset = 0;
        }
        beginUploads();
        const uint32_t offset = ringOffset;
        std::memcpy(ringMapped + offset, data, size);
        ringOffset += size;
        return offset;
    }

    // ---- Constant buffers ------------------------------------------------------------
    ID3D11Buffer* vertexConstants = nullptr;
    ID3D11Buffer* pixelConstants = nullptr;
    ID3D11Buffer* sharedConstants = nullptr;

    ID3D11Buffer* createConstantBuffer(UINT size) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = size;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Buffer* buffer = nullptr;
        device->CreateBuffer(&desc, nullptr, &buffer);
        return buffer;
    }
    void writeBuffer(ID3D11Buffer* buffer, const void* data, size_t size) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
        std::memcpy(mapped.pData, data, size);
        context->Unmap(buffer, 0);
    }

    // ---- Shaders -------------------------------------------------------------------
    struct CompiledShader {
        ID3D11VertexShader* vertex = nullptr;
        ID3D11PixelShader* pixel = nullptr;
        xenos::ShaderInfo info;
        bool failed = false;
    };
    std::unordered_map<uint64_t, CompiledShader> shaderCache;
    ID3D11GeometryShader* rectangleShader = nullptr;
    ID3D11PixelShader* flatShader = nullptr;
    ID3D11VertexShader* copyVertexShader = nullptr;
    ID3D11PixelShader* copyPixelShader = nullptr;
    ID3D11Buffer* copyConstants = nullptr;

    CompiledShader* shader(uint64_t hash, xenos::ShaderType type) {
        auto [it, inserted] = shaderCache.try_emplace(hash);
        CompiledShader& entry = it->second;
        if (!inserted) return entry.failed ? nullptr : &entry;
        const auto* words = shaderSource(hash);
        if (!words) {
            entry.failed = true;
            return nullptr;
        }
        const auto translated = xenos::translate(xenos::decode(type, *words));
        entry.info = translated.info;
        if (!translated.error.empty())
            logf("[d3d11] shader %016llX: %s (translated anyway)", (unsigned long long)hash, translated.error.c_str());
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const bool pixel = type == xenos::ShaderType::Pixel;
        if (FAILED(D3DCompile(translated.hlsl.data(), translated.hlsl.size(), nullptr, nullptr, nullptr, "main",
                              pixel ? "ps_5_0" : "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
            logf("[d3d11] shader %016llX failed to compile: %s", (unsigned long long)hash,
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            entry.failed = true;
        } else if (pixel) {
            device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &entry.pixel);
        } else {
            device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &entry.vertex);
        }
        release(code);
        release(errors);
        return entry.failed ? nullptr : &entry;
    }

    // ---- Render targets (EDRAM) --------------------------------------------------
    // Each distinct EDRAM base/pitch/format/samples is a host render target.
    struct RenderTarget {
        ID3D11Texture2D* texture = nullptr;
        ID3D11RenderTargetView* color = nullptr;
        ID3D11DepthStencilView* depth = nullptr;
        UINT width = 0, height = 0, samples = 1;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, bool>, RenderTarget> renderTargets;

    RenderTarget* renderTarget(uint32_t base, uint32_t pitch, uint32_t format, uint32_t samples, bool isDepth) {
        if (!pitch) return nullptr;
        auto& target = renderTargets[{base, pitch, format, samples, isDepth}];
        if (target.texture) return &target;
        // EDRAM is 2048 tiles of 80x16 samples. Size every target of a given pitch
        // and sample count alike: D3D11 needs color and depth bindings to match.
        const uint32_t tilesPerRow = (pitch + 79) / 80;
        const uint32_t height = (std::min)(2048u, 2048 / tilesPerRow * 16);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = pitch;
        desc.Height = (std::max)(16u, height / (samples >= 2 ? 2 : 1));
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = samples;
        desc.Usage = D3D11_USAGE_DEFAULT;
        if (isDepth) {
            desc.Format = format ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
            desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        } else {
            desc.Format = colorFormat(format);
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        }
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &target.texture))) {
            logf("[d3d11] cannot create %ux%u render target", desc.Width, desc.Height);
            return nullptr;
        }
        if (isDepth) device->CreateDepthStencilView(target.texture, nullptr, &target.depth);
        else device->CreateRenderTargetView(target.texture, nullptr, &target.color);
        target.width = desc.Width;
        target.height = desc.Height;
        target.samples = samples;
        target.format = desc.Format;
        TZ_TRACE("rt", "[d3d11] %s target base %u pitch %u format %u samples %u -> %ux%u", isDepth ? "depth" : "color",
                 base, pitch, format, samples, desc.Width, desc.Height);
        return &target;
    }

    // ---- Resolved textures (guest memory written by EDRAM copies) ------------------
    // A resolve destination is a whole tiled texture (RB_COPY_DEST_PITCH gives its
    // pitch and height); tiled resolves land at offsets inside it.
    struct ResolvedTexture {
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* view = nullptr;
        UINT width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t guestSize = 0;  // bytes covered in guest memory (tiled, 4 bytes per pixel)
        ID3D11RenderTargetView* target = nullptr;
        bool swapped = false;    // red and blue exchanged (RB_COPY_DEST_INFO bit 24)
    };
    std::map<uint32_t, ResolvedTexture> resolved;  // by physical base address

    // The resolved texture covering an address, and the pixel offset of that address.
    ResolvedTexture* containingResolved(uint32_t address, uint32_t& baseAddress, UINT& x, UINT& y) {
        auto it = resolved.upper_bound(address);
        if (it == resolved.begin()) return nullptr;
        --it;
        if (!it->second.texture || address >= it->first + it->second.guestSize) return nullptr;
        // 32x32 macro tiles of 4 KiB, row-major across the 32-aligned width.
        const uint32_t macroTile = (address - it->first) / 4096;
        const uint32_t macroPerRow = (it->second.width + 31) / 32;
        x = (macroTile % macroPerRow) * 32;
        y = (macroTile / macroPerRow) * 32;
        baseAddress = it->first;
        return &it->second;
    }

    ResolvedTexture* resolvedTexture(uint32_t address, UINT width, UINT height, DXGI_FORMAT format) {
        auto& entry = resolved[address];
        entry.guestSize = ((width + 31) & ~31u) * ((height + 31) & ~31u) * 4;
        if (entry.texture && (entry.width != width || entry.height != height || entry.format != format)) {
            release(entry.target);
            release(entry.view);
            release(entry.texture);
        }
        if (!entry.texture) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &entry.texture))) return nullptr;
            device->CreateShaderResourceView(entry.texture, nullptr, &entry.view);
            device->CreateRenderTargetView(entry.texture, nullptr, &entry.target);
            entry.width = width;
            entry.height = height;
            entry.format = format;
        }
        return &entry;
    }

    // ---- State objects ---------------------------------------------------------------
    std::map<std::vector<uint32_t>, ID3D11BlendState*> blendStates;
    std::map<std::pair<uint32_t, uint32_t>, ID3D11DepthStencilState*> depthStates;
    std::map<uint32_t, ID3D11RasterizerState*> rasterStates;
    std::map<uint32_t, ID3D11SamplerState*> samplers;
    TextureCache textures;
    ID3D11Texture2D* whiteTexture = nullptr;
    ID3D11ShaderResourceView* whiteView = nullptr;

    ID3D11BlendState* blendState(const uint32_t control[4], uint32_t colorMask) {
        std::vector<uint32_t> key(control, control + 4);
        key.push_back(colorMask);
        auto& state = blendStates[key];
        if (state) return state;
        D3D11_BLEND_DESC desc{};
        desc.IndependentBlendEnable = TRUE;
        for (int i = 0; i < 4; ++i) {
            const uint32_t c = control[i];
            auto& target = desc.RenderTarget[i];
            target.SrcBlend = blendFactor(c & 0x1F);
            target.BlendOp = blendOperation((c >> 5) & 7);
            target.DestBlend = blendFactor((c >> 8) & 0x1F);
            target.SrcBlendAlpha = alphaBlendFactor((c >> 16) & 0x1F);
            target.BlendOpAlpha = blendOperation((c >> 21) & 7);
            target.DestBlendAlpha = alphaBlendFactor((c >> 24) & 0x1F);
            const bool opaque = (c & 0x1FFF) == 0x0001 && ((c >> 16) & 0x1FFF) == 0x0001;
            target.BlendEnable = !opaque;
            target.RenderTargetWriteMask = UINT8((colorMask >> (i * 4)) & 0xF);
        }
        device->CreateBlendState(&desc, &state);
        return state;
    }

    ID3D11DepthStencilState* depthState(uint32_t control, uint32_t refMask) {
        auto& state = depthStates[{control, refMask & 0xFFFF00}];
        if (state) return state;
        D3D11_DEPTH_STENCIL_DESC desc{};
        desc.DepthEnable = (control >> 1) & 1;
        desc.DepthWriteMask = (control >> 2) & 1 ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.DepthFunc = comparison(control >> 4);
        desc.StencilEnable = control & 1;
        desc.StencilReadMask = UINT8(refMask >> 8);
        desc.StencilWriteMask = UINT8(refMask >> 16);
        desc.FrontFace.StencilFunc = comparison(control >> 8);
        desc.FrontFace.StencilFailOp = stencilOperation(control >> 11);
        desc.FrontFace.StencilPassOp = stencilOperation(control >> 14);
        desc.FrontFace.StencilDepthFailOp = stencilOperation(control >> 17);
        if ((control >> 7) & 1) {
            desc.BackFace.StencilFunc = comparison(control >> 20);
            desc.BackFace.StencilFailOp = stencilOperation(control >> 23);
            desc.BackFace.StencilPassOp = stencilOperation(control >> 26);
            desc.BackFace.StencilDepthFailOp = stencilOperation(control >> 29);
        } else {
            desc.BackFace = desc.FrontFace;
        }
        device->CreateDepthStencilState(&desc, &state);
        return state;
    }

    ID3D11RasterizerState* rasterState(uint32_t modeControl, bool clipDisabled) {
        // Culling, winding and clipping.
        const uint32_t key = (modeControl & 7) | (clipDisabled ? 8 : 0);
        auto& state = rasterStates[key];
        if (state) return state;
        D3D11_RASTERIZER_DESC desc{};
        desc.FillMode = D3D11_FILL_SOLID;
        static const bool noCull = traceEnabled("nocull");
        const bool cullFront = !noCull && (key & 1), cullBack = !noCull && (key & 2);
        desc.CullMode = cullFront && !cullBack ? D3D11_CULL_FRONT : cullBack && !cullFront ? D3D11_CULL_BACK
                                                                                           : D3D11_CULL_NONE;
        // Face bit 2: 0 = counter-clockwise front faces. The host y flip keeps winding.
        desc.FrontCounterClockwise = (key & 4) == 0;
        desc.DepthClipEnable = !clipDisabled;
        desc.ScissorEnable = TRUE;
        device->CreateRasterizerState(&desc, &state);
        return state;
    }

    ID3D11SamplerState* defaultSampler() {
        auto& state = samplers[0];
        if (state) return state;
        D3D11_SAMPLER_DESC desc{};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&desc, &state);
        return state;
    }

    // ---- Setup ---------------------------------------------------------------------
    Impl(D3D11Renderer& r, const std::atomic<uint32_t>* regs, ShaderSource source)
        : renderer(r), device(r.device()), context(r.context()), registers(regs), shaderSource(std::move(source)) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = kRingSize;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        device->CreateBuffer(&desc, nullptr, &ring);
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        view.BufferEx.NumElements = kRingSize / 4;
        view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        device->CreateShaderResourceView(ring, &view, &ringView);
        vertexConstants = createConstantBuffer(256 * 16);
        pixelConstants = createConstantBuffer(256 * 16);
        sharedConstants = createConstantBuffer((sizeof(SharedConstants) + 15) & ~15u);

        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        if (SUCCEEDED(D3DCompile(kRectangleGeometryShader, strlen(kRectangleGeometryShader), "rect", nullptr, nullptr,
                                 "main", "gs_5_0", 0, 0, &code, &errors)))
            device->CreateGeometryShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &rectangleShader);
        release(code);
        release(errors);
        if (SUCCEEDED(D3DCompile(kFlatPixelShader, strlen(kFlatPixelShader), "flat", nullptr, nullptr, "main", "ps_5_0",
                                 0, 0, &code, &errors)))
            device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &flatShader);
        release(code);
        release(errors);
        if (SUCCEEDED(D3DCompile(kCopyShader, strlen(kCopyShader), "copy", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &code,
                                 &errors)))
            device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &copyVertexShader);
        release(code);
        release(errors);
        if (SUCCEEDED(D3DCompile(kCopyShader, strlen(kCopyShader), "copy", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &code,
                                 &errors)))
            device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &copyPixelShader);
        release(code);
        release(errors);
        copyConstants = createConstantBuffer(32);

        const uint32_t white = 0xFFFFFFFF;
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = texture.Height = 1;
        texture.MipLevels = texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_IMMUTABLE;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{&white, 4, 4};
        device->CreateTexture2D(&texture, &data, &whiteTexture);
        device->CreateShaderResourceView(whiteTexture, nullptr, &whiteView);
        textures.initialize(device, context);
    }

    // ---- Indices -------------------------------------------------------------------
    // Builds a host list of guest vertex indices for a D3D11 topology.
    bool buildIndices(const DrawRequest& request, uint32_t primitive, uint32_t count, std::vector<uint32_t>& out,
                      D3D11_PRIMITIVE_TOPOLOGY& topology, bool& rectangles) {
        const uint32_t source = (request.initiator >> 6) & 3;
        const bool wide = (request.initiator >> 11) & 1;
        std::vector<uint32_t> indices(count);
        if (source == 2) {
            for (uint32_t i = 0; i < count; ++i) indices[i] = i;
        } else {
            const uint8_t* data = nullptr;
            std::vector<uint32_t> immediate;
            uint32_t endian = request.indexEndian;
            if (source == 0) {
                data = memory->host(Memory::fromPhysical(request.indexBase));
            } else {
                data = reinterpret_cast<const uint8_t*>(request.inlineIndices.data());
                endian = 2;  // immediate data is in big-endian packet dwords
            }
            for (uint32_t i = 0; i < count; ++i) {
                if (wide) {
                    uint32_t raw;
                    std::memcpy(&raw, data + i * 4, 4);
                    indices[i] = gpuSwap(raw, endian);
                } else {
                    uint32_t raw;
                    std::memcpy(&raw, data + (i & ~1u) * 2, 4);
                    const uint32_t swapped = gpuSwap(raw, endian);
                    indices[i] = (i & 1) ? swapped >> 16 : swapped & 0xFFFF;
                }
            }
        }
        const uint32_t reset = reg(reg::VGT_MULTI_PRIM_IB_RESET_INDX) & (wide ? 0xFFFFFFFFu : 0xFFFFu);
        rectangles = false;
        out.clear();
        switch (primitive) {
        case 1: topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; out = indices; break;
        case 2: topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; out = indices; break;
        case 3: topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; out = indices; break;
        case 4: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; out = indices; break;
        case 5:  // triangle fan
            topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            for (uint32_t i = 1; i + 1 < count; ++i) out.insert(out.end(), {indices[0], indices[i], indices[i + 1]});
            break;
        case 6: {  // triangle strip, with primitive reset
            topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            uint32_t start = 0;
            for (uint32_t i = 0; i <= count; ++i) {
                if (i < count && indices[i] != reset) continue;
                for (uint32_t k = start; k + 2 < i; ++k) {
                    if ((k - start) & 1) out.insert(out.end(), {indices[k + 1], indices[k], indices[k + 2]});
                    else out.insert(out.end(), {indices[k], indices[k + 1], indices[k + 2]});
                }
                start = i + 1;
            }
            break;
        }
        case 8:  // rectangle list: expanded by the geometry shader
            topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            out = indices;
            rectangles = true;
            break;
        case 13:  // quad list
            topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            for (uint32_t i = 0; i + 3 < count; i += 4)
                out.insert(out.end(), {indices[i], indices[i + 1], indices[i + 2], indices[i], indices[i + 2], indices[i + 3]});
            break;
        default:
            return false;
        }
        return true;
    }

    // Views, samplers, sizes and swizzles for a shader's texture bindings.
    void bindTextures(const xenos::ShaderInfo& info, SharedConstants& shared, std::vector<ID3D11ShaderResourceView*>& views,
                      std::vector<ID3D11SamplerState*>& states) {
        views.assign(info.textures.size(), whiteView);
        states.assign(info.textures.size(), defaultSampler());
        for (size_t slot = 0; slot < info.textures.size() && slot < 16; ++slot) {
            TextureFetch fetch;
            for (int i = 0; i < 6; ++i)
                fetch.words[i] = reg(reg::SHADER_CONSTANT_FETCH_00_0 + info.textures[slot].fetchConstant * 6 + i);
            shared.textureSwizzle[slot] = fetch.swizzle();
            shared.textureSize[slot][0] = float(fetch.width());
            shared.textureSize[slot][1] = float(fetch.height());
            states[slot] = textures.sampler(fetch);
            auto it = resolved.find(fetch.baseAddress() & 0x1FFFFFFF);
            if (it != resolved.end() && it->second.view) {
                if (traceFrame()) logf("[draw]   texture %u uses resolved %08X", uint32_t(slot), it->first);
                views[slot] = it->second.view;
                continue;
            }
            if (traceFrame())
                logf("[draw]   texture %u at %08X format %u %ux%u", uint32_t(slot), fetch.baseAddress(), fetch.format(),
                     fetch.width(), fetch.height());
            if (auto* view = textures.view(fetch)) views[slot] = view;
        }
    }

    // ---- Draw ----------------------------------------------------------------------
    void draw(const DrawRequest& request, uint64_t vsHash, uint64_t psHash) {
        const uint32_t edramMode = reg(reg::RB_MODECONTROL) & 7;
        if (edramMode == 6) {
            resolve();
            return;
        }
        if (edramMode != 4 && edramMode != 5) return;
        const uint32_t primitive = request.initiator & 0x3F;
        const uint32_t count = request.initiator >> 16;
        if (!count) return;

        auto* vs = shader(vsHash, xenos::ShaderType::Vertex);
        auto* ps = edramMode == 4 ? shader(psHash, xenos::ShaderType::Pixel) : nullptr;
        if (!vs || (edramMode == 4 && !ps)) return;

        std::vector<uint32_t> indices;
        D3D11_PRIMITIVE_TOPOLOGY topology;
        bool rectangles;
        if (!buildIndices(request, primitive, count, indices, topology, rectangles)) {
            TZ_TRACE("draw", "[d3d11] unsupported primitive type %u", primitive);
            return;
        }
        if (indices.empty()) return;

        // Render targets.
        const uint32_t surfaceInfo = reg(reg::RB_SURFACE_INFO);
        const uint32_t pitch = surfaceInfo & 0x3FFF;
        const uint32_t samples = 1u << ((surfaceInfo >> 16) & 3);
        const uint32_t colorMask = reg(reg::RB_COLOR_MASK);
        ID3D11RenderTargetView* colorViews[4] = {};
        RenderTarget* firstTarget = nullptr;
        if (edramMode == 4) {
            const uint32_t infoRegisters[4] = {reg::RB_COLOR_INFO, reg::RB_COLOR1_INFO, reg::RB_COLOR2_INFO,
                                               reg::RB_COLOR3_INFO};
            for (int i = 0; i < 4; ++i) {
                if (!((ps->info.colorTargetMask >> i) & 1) || !((colorMask >> (i * 4)) & 0xF)) continue;
                const uint32_t info = reg(infoRegisters[i]);
                if (auto* target = renderTarget(info & 0xFFF, pitch, (info >> 16) & 0xF, samples, false)) {
                    colorViews[i] = target->color;
                    if (!firstTarget) firstTarget = target;
                }
            }
        }
        const uint32_t depthControl = reg(reg::RB_DEPTHCONTROL);
        ID3D11DepthStencilView* depthView = nullptr;
        if (depthControl & 7) {
            const uint32_t info = reg(reg::RB_DEPTH_INFO);
            if (auto* target = renderTarget(info & 0xFFF, pitch, (info >> 16) & 1, samples, true)) {
                depthView = target->depth;
                if (!firstTarget) firstTarget = target;
            }
        }
        if (!firstTarget) return;

        // Uploads: vertex data per binding, then the index list.
        SharedConstants shared{};
        for (int i = 0; i < 8; ++i) shared.bools[i] = reg(reg::SHADER_CONSTANT_BOOL_000_031 + i);
        for (int i = 0; i < 32; ++i) shared.loops[i] = reg(reg::SHADER_CONSTANT_LOOP_00 + i);
        const auto& bindings = vs->info.vertexFetchConstants;
        for (size_t slot = 0; slot < bindings.size() && slot < 16; ++slot) {
            const uint32_t base = reg::SHADER_CONSTANT_FETCH_00_0 + bindings[slot] * 2;
            const uint32_t word0 = reg(base), word1 = reg(base + 1);
            const uint32_t address = word0 & ~3u, size = ((word1 >> 2) & 0xFFFFFF) * 4;
            shared.vertexEndian[slot] = word1 & 3;
            shared.vertexBase[slot] = upload(memory->host(Memory::fromPhysical(address)), (std::max)(size, 16u));
        }
        shared.draw[0] = upload(indices.data(), uint32_t(indices.size() * 4));
        shared.draw[1] = reg(reg::VGT_INDX_OFFSET);
        endUploads();
        if (shared.draw[0] == ~0u) return;

        // Guest screen space -> host clip space.
        const uint32_t vte = reg(reg::PA_CL_VTE_CNTL);
        const float width = float(firstTarget->width), height = float(firstTarget->height);
        float scale[3] = {1, 1, 1}, offset[3] = {0, 0, 0};
        for (int axis = 0; axis < 3; ++axis) {
            if ((vte >> (axis * 2)) & 1) scale[axis] = asFloat(reg(reg::PA_CL_VPORT_XSCALE + axis * 2));
            if ((vte >> (axis * 2 + 1)) & 1) offset[axis] = asFloat(reg(reg::PA_CL_VPORT_XSCALE + axis * 2 + 1));
        }
        if ((reg(reg::PA_SU_SC_MODE_CNTL) >> 16) & 1) {
            const uint32_t windowOffset = reg(reg::PA_SC_WINDOW_OFFSET);
            offset[0] += float(int32_t(windowOffset << 17) >> 17);
            offset[1] += float(int32_t(windowOffset << 1) >> 17);
        }
        // D3D9-style pixel centers sit at integer coordinates.
        if (!(reg(reg::PA_SU_VTX_CNTL) & 1)) {
            offset[0] += 0.5f;
            offset[1] += 0.5f;
        }
        shared.positionScale[0] = scale[0] * 2.0f / width;
        shared.positionScale[1] = -scale[1] * 2.0f / height;
        shared.positionScale[2] = scale[2];
        shared.positionOffset[0] = offset[0] * 2.0f / width - 1.0f;
        shared.positionOffset[1] = 1.0f - offset[1] * 2.0f / height;
        shared.positionOffset[2] = offset[2];
        const uint32_t colorControl = reg(reg::RB_COLORCONTROL);
        shared.alphaTest[0] = (colorControl >> 3) & 1 ? float(colorControl & 7) : 7.0f;
        shared.alphaTest[1] = asFloat(reg(reg::RB_ALPHA_REF));
        for (int i = 0; i < 16; ++i) {
            shared.textureSize[i][0] = shared.textureSize[i][1] = shared.textureSize[i][2] = 1.0f;
            shared.textureSwizzle[i] = 0 | 1 << 3 | 2 << 6 | 3 << 9;
        }
        std::vector<ID3D11ShaderResourceView*> pixelViews, vertexViews;
        std::vector<ID3D11SamplerState*> pixelSamplers, vertexSamplers;
        // Vertex and pixel shaders share SharedConstants; pixel bindings win on overlap.
        bindTextures(vs->info, shared, vertexViews, vertexSamplers);
        if (ps) bindTextures(ps->info, shared, pixelViews, pixelSamplers);
        writeBuffer(sharedConstants, &shared, sizeof(shared));

        float constants[256 * 4];
        for (int i = 0; i < 256 * 4; ++i) constants[i] = asFloat(reg(reg::SHADER_CONSTANT_000_X + i));
        writeBuffer(vertexConstants, constants, sizeof(constants));
        if (ps) {
            for (int i = 0; i < 256 * 4; ++i) constants[i] = asFloat(reg(reg::SHADER_CONSTANT_000_X + 1024 + i));
            writeBuffer(pixelConstants, constants, sizeof(constants));
        }

        // Pipeline.
        context->OMSetRenderTargets(4, colorViews, depthView);
        const D3D11_VIEWPORT viewport{0, 0, width, height, 0, 1};
        context->RSSetViewports(1, &viewport);
        const uint32_t scissorTl = reg(reg::PA_SC_WINDOW_SCISSOR_TL), scissorBr = reg(reg::PA_SC_WINDOW_SCISSOR_BR);
        D3D11_RECT scissor{LONG(scissorTl & 0x7FFF), LONG((scissorTl >> 16) & 0x7FFF),
                           LONG(scissorBr & 0x7FFF), LONG((scissorBr >> 16) & 0x7FFF)};
        // The window offset moves the scissor too unless bit 31 disables it.
        if (!(scissorTl >> 31)) {
            const uint32_t windowOffset = reg(reg::PA_SC_WINDOW_OFFSET);
            const LONG dx = int32_t(windowOffset << 17) >> 17, dy = int32_t(windowOffset << 1) >> 17;
            scissor.left = (std::max)(0L, scissor.left + dx);
            scissor.right = (std::max)(0L, scissor.right + dx);
            scissor.top = (std::max)(0L, scissor.top + dy);
            scissor.bottom = (std::max)(0L, scissor.bottom + dy);
        }
        context->RSSetScissorRects(1, &scissor);
        context->RSSetState(rasterState(reg(reg::PA_SU_SC_MODE_CNTL), (reg(reg::PA_CL_CLIP_CNTL) >> 16) & 1));
        const uint32_t blendControl[4] = {reg(reg::RB_BLENDCONTROL0), reg(reg::RB_BLENDCONTROL1),
                                          reg(reg::RB_BLENDCONTROL2), reg(reg::RB_BLENDCONTROL3)};
        const float blendColor[4] = {asFloat(reg(reg::RB_BLEND_RED)), asFloat(reg(reg::RB_BLEND_RED + 1)),
                                     asFloat(reg(reg::RB_BLEND_RED + 2)), asFloat(reg(reg::RB_BLEND_RED + 3))};
        context->OMSetBlendState(blendState(blendControl, colorMask), blendColor, 0xFFFFFFFF);
        const uint32_t stencil = reg(reg::RB_STENCILREFMASK);
        static const bool noDepth = traceEnabled("nodepth");
        context->OMSetDepthStencilState(depthState(noDepth ? 0 : depthControl, stencil), stencil & 0xFF);

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(topology);
        context->VSSetShader(vs->vertex, nullptr, 0);
        ID3D11Buffer* vsBuffers[2] = {vertexConstants, sharedConstants};
        context->VSSetConstantBuffers(0, 2, vsBuffers);
        context->VSSetShaderResources(0, 1, &ringView);
        if (!vertexViews.empty()) {
            context->VSSetShaderResources(8, UINT(vertexViews.size()), vertexViews.data());
            context->VSSetSamplers(0, UINT(vertexSamplers.size()), vertexSamplers.data());
        }
        context->GSSetShader(rectangles ? rectangleShader : nullptr, nullptr, 0);
        if (ps) {
            // TENCHU_FLAT_PRIM=<type> limits the magenta override to one primitive type.
            static const int flatPrimitive = [] {
                char value[16]{};
                return GetEnvironmentVariableA("TENCHU_FLAT_PRIM", value, sizeof(value)) ? atoi(value) : -1;
            }();
            static const bool flatAll = traceEnabled("flat");
            const bool flat = flatAll || int(primitive) == flatPrimitive;
            context->PSSetShader(flat ? flatShader : ps->pixel, nullptr, 0);
            ID3D11Buffer* psBuffers[2] = {pixelConstants, sharedConstants};
            context->PSSetConstantBuffers(0, 2, psBuffers);
            if (!pixelViews.empty()) {
                context->PSSetShaderResources(0, UINT(pixelViews.size()), pixelViews.data());
                context->PSSetSamplers(0, UINT(pixelSamplers.size()), pixelSamplers.data());
            }
        } else {
            context->PSSetShader(nullptr, nullptr, 0);
        }
        if (traceFrame()) {
            logf("[draw] prim %u count %u -> %zu vs %016llX ps %016llX rt %08X/%08X depth %08X ctl %08X mask %X "
                 "mode %X clip %X vte %X scale %.3f %.3f %.3f off %.3f %.3f %.3f scissor %ld,%ld-%ld,%ld target %ux%u",
                 primitive, count, indices.size(), (unsigned long long)vsHash, (unsigned long long)psHash,
                 reg(reg::RB_SURFACE_INFO), reg(reg::RB_COLOR_INFO), reg(reg::RB_DEPTH_INFO), depthControl, colorMask,
                 reg(reg::PA_SU_SC_MODE_CNTL), reg(reg::PA_CL_CLIP_CNTL), vte, scale[0], scale[1], scale[2], offset[0], offset[1], offset[2], scissor.left, scissor.top,
                 scissor.right, scissor.bottom, firstTarget->width, firstTarget->height);
            logf("[draw]   window offset %08X scissor tl %08X br %08X blend %08X colorcontrol %08X alpharef %08X",
                 reg(reg::PA_SC_WINDOW_OFFSET), scissorTl, scissorBr, reg(reg::RB_BLENDCONTROL0),
                 reg(reg::RB_COLORCONTROL), reg(reg::RB_ALPHA_REF));
        }
        ID3D11Query* statistics = nullptr;
        if (traceFrame()) {
            const D3D11_QUERY_DESC query{D3D11_QUERY_PIPELINE_STATISTICS, 0};
            device->CreateQuery(&query, &statistics);
            context->Begin(statistics);
        }
        context->Draw(UINT(indices.size()), 0);
        if (statistics) {
            context->End(statistics);
            D3D11_QUERY_DATA_PIPELINE_STATISTICS data{};
            while (context->GetData(statistics, &data, sizeof(data), 0) == S_FALSE) {}
            logf("[draw]   stats: vs %llu, clipper in %llu out %llu, ps %llu", data.VSInvocations, data.CInvocations,
                 data.CPrimitives, data.PSInvocations);
            release(statistics);
        }
        ++draws;
    }
    uint64_t draws = 0;
    uint64_t frames = 0;
    // TENCHU_TRACE_FRAME=<n> logs every draw of frame n.
    bool traceFrame() const {
        static const uint64_t frame = [] {
            char value[32]{};
            return GetEnvironmentVariableA("TENCHU_TRACE_FRAME", value, sizeof(value)) ? strtoull(value, nullptr, 10) : 0ull;
        }();
        return frame && frames == frame;
    }

    // ---- Resolve -------------------------------------------------------------------
    void resolve() {
        const uint32_t copyControl = reg(reg::RB_COPY_CONTROL);
        const uint32_t sourceSelect = copyControl & 7;
        const bool clearColor = (copyControl >> 8) & 1, clearDepth = (copyControl >> 9) & 1;
        const uint32_t command = (copyControl >> 20) & 3;
        const uint32_t surfaceInfo = reg(reg::RB_SURFACE_INFO);
        const uint32_t pitch = surfaceInfo & 0x3FFF;
        const uint32_t samples = 1u << ((surfaceInfo >> 16) & 3);

        // The copied rectangle is given by three 32_32_FLOAT vertices in fetch constant 0.
        const uint32_t fetch0 = reg(reg::SHADER_CONSTANT_FETCH_00_0) & ~3u;
        const uint32_t fetchEndian = reg(reg::SHADER_CONSTANT_FETCH_00_0 + 1) & 3;
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (int v = 0; v < 3; ++v) {
            uint32_t rawX, rawY;
            std::memcpy(&rawX, memory->host(Memory::fromPhysical(fetch0 + v * 8)), 4);
            std::memcpy(&rawY, memory->host(Memory::fromPhysical(fetch0 + v * 8 + 4)), 4);
            const float x = asFloat(gpuSwap(rawX, fetchEndian)), y = asFloat(gpuSwap(rawY, fetchEndian));
            minX = (std::min)(minX, x); maxX = (std::max)(maxX, x);
            minY = (std::min)(minY, y); maxY = (std::max)(maxY, y);
        }
        const uint32_t windowOffset = reg(reg::PA_SC_WINDOW_OFFSET);
        const int offsetX = int32_t(windowOffset << 17) >> 17, offsetY = int32_t(windowOffset << 1) >> 17;
        D3D11_RECT rect{LONG(minX) + offsetX, LONG(minY) + offsetY, LONG(maxX) + offsetX, LONG(maxY) + offsetY};
        rect.left = (std::max)(0L, rect.left);
        rect.top = (std::max)(0L, rect.top);

        RenderTarget* source = nullptr;
        if (sourceSelect < 4) {
            const uint32_t infoRegisters[4] = {reg::RB_COLOR_INFO, reg::RB_COLOR1_INFO, reg::RB_COLOR2_INFO,
                                               reg::RB_COLOR3_INFO};
            const uint32_t info = reg(infoRegisters[sourceSelect]);
            source = renderTarget(info & 0xFFF, pitch, (info >> 16) & 0xF, samples, false);
        }
        TZ_TRACE("resolve", "[d3d11] resolve src %u rect %ld,%ld-%ld,%ld -> %08X pitch %08X info %08X cmd %u clear %d%d",
                 sourceSelect, rect.left, rect.top, rect.right, rect.bottom, reg(reg::RB_COPY_DEST_BASE),
                 reg(reg::RB_COPY_DEST_PITCH), reg(reg::RB_COPY_DEST_INFO), command, clearColor, clearDepth);

        if (source && command != 3 && rect.right > rect.left && rect.bottom > rect.top) {
            rect.right = (std::min)(LONG(source->width), rect.right);
            rect.bottom = (std::min)(LONG(source->height), rect.bottom);
            const uint32_t destination = reg(reg::RB_COPY_DEST_BASE) & 0x1FFFFFFF;
            const uint32_t destPitch = reg(reg::RB_COPY_DEST_PITCH);
            const UINT destWidth = (std::max)(1u, destPitch & 0x3FFF), destHeight = (std::max)(1u, (destPitch >> 16) & 0x3FFF);
            uint32_t base = destination;
            UINT destX = 0, destY = 0;
            ResolvedTexture* target = containingResolved(destination, base, destX, destY);
            if (target && (target->width != destWidth || target->height != destHeight || target->format != source->format)) {
                // A different texture now lives here.
                if (base != destination) target = nullptr;
                destX = destY = 0;
                base = destination;
            }
            if (!target || base == destination) {
                target = resolvedTexture(destination, destWidth, destHeight, source->format);
                destX = destY = 0;
            }
            textures.invalidate(base);
            TZ_TRACE("resolve", "[d3d11]   into %08X at %u,%u (%ux%u)", base, destX, destY, target ? target->width : 0,
                     target ? target->height : 0);
            if (target) {
                // Clip the copy to the destination texture.
                rect.right = (std::min)(rect.right, LONG(rect.left + (target->width - (std::min)(destX, target->width))));
                rect.bottom = (std::min)(rect.bottom, LONG(rect.top + (target->height - (std::min)(destY, target->height))));
                // Multisampled targets are averaged first; the copy shader then
                // places the rectangle and applies the red/blue swap.
                ID3D11Texture2D* single = nullptr;
                ID3D11Texture2D* copySource = source->texture;
                if (source->samples > 1) {
                    D3D11_TEXTURE2D_DESC desc{};
                    source->texture->GetDesc(&desc);
                    desc.SampleDesc.Count = 1;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    if (FAILED(device->CreateTexture2D(&desc, nullptr, &single))) return;
                    context->ResolveSubresource(single, 0, source->texture, 0, source->format);
                    copySource = single;
                }
                ID3D11ShaderResourceView* sourceView = nullptr;
                device->CreateShaderResourceView(copySource, nullptr, &sourceView);
                const bool swap = (reg(reg::RB_COPY_DEST_INFO) >> 24) & 1;
                const int32_t constants[8] = {int32_t(rect.left), int32_t(rect.top), int32_t(destX), int32_t(destY),
                                              swap ? 1 : 0, 0, 0, 0};
                writeBuffer(copyConstants, constants, sizeof(constants));
                context->OMSetRenderTargets(1, &target->target, nullptr);
                const D3D11_VIEWPORT viewport{float(destX), float(destY), float(rect.right - rect.left),
                                              float(rect.bottom - rect.top), 0, 1};
                context->RSSetViewports(1, &viewport);
                context->RSSetState(nullptr);
                context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                context->OMSetDepthStencilState(nullptr, 0);
                context->IASetInputLayout(nullptr);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(copyVertexShader, nullptr, 0);
                context->GSSetShader(nullptr, nullptr, 0);
                context->PSSetShader(copyPixelShader, nullptr, 0);
                context->PSSetShaderResources(0, 1, &sourceView);
                context->PSSetConstantBuffers(0, 1, &copyConstants);
                context->Draw(3, 0);
                ID3D11ShaderResourceView* none = nullptr;
                context->PSSetShaderResources(0, 1, &none);
                target->swapped = swap;
                if (traceFrame() && traceEnabled("dumpresolved") && single) {
                    CreateDirectoryA("build/resolved", nullptr);
                    char path[96];
                    snprintf(path, sizeof(path), "build/resolved/source_%08X.bmp", destination);
                    saveTexture(single, path);
                }
                release(sourceView);
                release(single);
            }
        }
        if (clearColor && source) {
            // RB_COLOR_CLEAR holds a packed ARGB color for 32-bit targets.
            const uint32_t value = reg(reg::RB_COLOR_CLEAR);
            const float color[4] = {((value >> 16) & 0xFF) / 255.0f, ((value >> 8) & 0xFF) / 255.0f,
                                    (value & 0xFF) / 255.0f, (value >> 24) / 255.0f};
            ID3D11DeviceContext1* context1 = nullptr;
            if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1)))) {
                context1->ClearView(source->color, color, &rect, 1);
                release(context1);
            } else {
                context->ClearRenderTargetView(source->color, color);
            }
        }
        if (clearDepth) {
            const uint32_t info = reg(reg::RB_DEPTH_INFO);
            if (auto* depth = renderTarget(info & 0xFFF, pitch, (info >> 16) & 1, samples, true)) {
                const uint32_t value = reg(reg::RB_DEPTH_CLEAR);
                context->ClearDepthStencilView(depth->depth, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                               float(value >> 8) / 16777215.0f, UINT8(value & 0xFF));
            }
        }
    }

    // Saves an RGBA8 texture as a 32-bit BMP (debugging).
    void saveTexture(ID3D11Texture2D* texture, const char* path) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.SampleDesc.Count != 1) return;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return;
        context->CopyResource(staging, texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            BITMAPFILEHEADER file{};
            BITMAPINFOHEADER info{};
            info.biSize = sizeof(info);
            info.biWidth = LONG(desc.Width);
            info.biHeight = LONG(desc.Height);
            info.biPlanes = 1;
            info.biBitCount = 32;
            file.bfType = 0x4D42;
            file.bfOffBits = sizeof(file) + sizeof(info);
            file.bfSize = file.bfOffBits + desc.Width * desc.Height * 4;
            if (FILE* out = fopen(path, "wb")) {
                fwrite(&file, sizeof(file), 1, out);
                fwrite(&info, sizeof(info), 1, out);
                std::vector<uint8_t> row(desc.Width * 4);
                for (UINT y = desc.Height; y-- > 0;) {
                    const uint8_t* source = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
                    for (UINT x = 0; x < desc.Width; ++x) {
                        row[x * 4 + 0] = source[x * 4 + 2];
                        row[x * 4 + 1] = source[x * 4 + 1];
                        row[x * 4 + 2] = source[x * 4 + 0];
                        row[x * 4 + 3] = source[x * 4 + 3];
                    }
                    fwrite(row.data(), 1, row.size(), out);
                }
                fclose(out);
            }
            context->Unmap(staging, 0);
        }
        release(staging);
    }

    // ---- Swap ---------------------------------------------------------------------
    void swap(uint32_t frontBuffer) {
        auto it = resolved.find(frontBuffer & 0x1FFFFFFF);
        if (it == resolved.end()) it = resolved.find(Memory::physicalAddress(frontBuffer));
        if (traceFrame() && traceEnabled("dumpresolved")) {
            CreateDirectoryA("build/resolved", nullptr);
            for (const auto& [address, entry] : resolved) {
                char path[96];
                snprintf(path, sizeof(path), "build/resolved/%08X_%ux%u.bmp", address, entry.width, entry.height);
                if (entry.texture) saveTexture(entry.texture, path);
            }
        }
        renderer.present(it != resolved.end() ? it->second.view : nullptr,
                         it != resolved.end() && it->second.swapped);
        TZ_TRACE("swap", "[d3d11] swap %08X (%s), %llu draws", frontBuffer,
                 it != resolved.end() ? "resolved" : "missing", (unsigned long long)draws);
        draws = 0;
        ++frames;
    }
};

XenosBackend::XenosBackend(D3D11Renderer& renderer, const std::atomic<uint32_t>* registers, ShaderSource shaders)
    : impl_(std::make_unique<Impl>(renderer, registers, std::move(shaders))) {}
XenosBackend::~XenosBackend() = default;
void XenosBackend::draw(const DrawRequest& request, uint64_t vs, uint64_t ps) { impl_->draw(request, vs, ps); }
void XenosBackend::swap(uint32_t frontBuffer) { impl_->swap(frontBuffer); }

}  // namespace tz::gpu
