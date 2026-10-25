#include "xenos_backend.h"
#include "gpu/registers.h"
#include "gpu/d3d11/texture_cache.h"
#include "gpu/d3d11/post_effects.h"
#include "kernel/kernel.h"
#include "kernel/control.h"
#include "host/settings.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <filesystem>
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
    float textureScale[16][2];
    uint32_t draw[4];
};

// Layout of cbuffer ClipConstants in the translated vertex shaders (user clip planes).
struct ClipConstants {
    float planes[6][4];
    uint32_t enabled[4];  // [0]: plane mask
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

// Depth resolve: the first sample's depth becomes an R32_FLOAT texel, as k_24_8
// textures sample on the GPU. MULTISAMPLED selects the source type. Float depth
// buffers (D24FS8) hold 20e4 floats in EDRAM; those bits are what a k_24_8 fetch
// returns as unorm, so they are re-encoded (`swap` carries that flag here).
const char* kDepthCopyShader = R"(
#ifdef MULTISAMPLED
Texture2DMS<float> source : register(t0);
#else
Texture2D<float> source : register(t0);
#endif
cbuffer Copy : register(b0) { int2 sourceOrigin; int2 destOrigin; uint swap; uint3 unused; };
float Float20e4AsUnorm(float value) {
    uint bits = asuint(saturate(value));
    if (bits < 0x38800000u) {  // below the smallest normal 20e4 exponent
        uint shift = 113u - (bits >> 23);
        bits = shift < 24u ? (0x800000u | (bits & 0x7FFFFFu)) >> shift : 0u;
    } else {
        bits += 0xC8000000u;  // rebias the exponent from 127 to 15
    }
    uint encoded = ((bits + 3u + ((bits >> 3) & 1u)) >> 3) & 0xFFFFFFu;
    return float(encoded) / 16777215.0;
}
float4 ps(float4 position : SV_Position) : SV_Target {
    int2 coord = int2(position.xy) - destOrigin + sourceOrigin;
#ifdef MULTISAMPLED
    float depth = source.Load(coord, 0);
#else
    float depth = source.Load(int3(coord, 0));
#endif
    return (swap ? Float20e4AsUnorm(depth) : depth).xxxx;
}
)";

// Expands Xenos rectangle lists: three corners in, the fourth extrapolated opposite
// the right-angle corner.
const char* kRectangleGeometryShader = R"(
struct Interpolators {
    float4 position : SV_Position;
    float4 t[16] : TEXCOORD0;
    float4 clip0 : SV_ClipDistance0;
    float2 clip1 : SV_ClipDistance1;
};
[maxvertexcount(4)]
void main(triangle Interpolators v[3], inout TriangleStream<Interpolators> stream) {
    float2 p0 = v[0].position.xy / v[0].position.w;
    float2 p1 = v[1].position.xy / v[1].position.w;
    float2 p2 = v[2].position.xy / v[2].position.w;
    // The corner whose edges are closest to perpendicular (normalized dot products).
    float c0 = abs(dot(normalize(p1 - p0), normalize(p2 - p0)));
    float c1 = abs(dot(normalize(p2 - p1), normalize(p0 - p1)));
    float c2 = abs(dot(normalize(p0 - p2), normalize(p1 - p2)));
    // Rotate (keeping the winding) so the right-angle corner comes first; ties keep v0.
    uint corner = c0 <= c1 + 1e-4 && c0 <= c2 + 1e-4 ? 0 : (c1 <= c2 ? 1 : 2);
    Interpolators a = v[corner], b = v[(corner + 1) % 3], c = v[(corner + 2) % 3];
    Interpolators fourth;
    fourth.position = b.position + c.position - a.position;
    [unroll] for (int i = 0; i < 16; ++i) fourth.t[i] = b.t[i] + c.t[i] - a.t[i];
    fourth.clip0 = b.clip0 + c.clip0 - a.clip0;
    fourth.clip1 = b.clip1 + c.clip1 - a.clip1;
    stream.Append(a);
    stream.Append(b);
    stream.Append(c);
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

// The scene composite (DOF blur and bloom over the resolved scene, a full-screen rectangle
// into the 1280-pitch target): the last 3D draw of a gameplay frame; the HUD follows it
// in the same target, then the target is resolved to the front buffer.
constexpr uint64_t kSceneCompositeShader = 0x7582CB38B8D839B4ull;
// The bloom chain's first step: 16 taps of the 1280x720 scene (fetch constant 1) into 320x180.
constexpr uint64_t kBloomShrinkShader = 0x723D9B291D342106ull;

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
    uint64_t uploadedBytes = 0;  // per frame (trace 'hosttime')
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> vertexStats;  // trace 'vbstats
    uint8_t* ringMapped = nullptr;

    // ---- Guest memory mirror: physical memory on the GPU, uploaded per 4 KiB page ----
    // Vertex data is read from here in place: a page is uploaded once and write-watched
    // (Memory::watchPages), and re-uploaded only after the guest writes it. Pages the
    // title rewrites every frame (skinning, particles) would fault constantly, so those
    // are marked hot for a while and their streams are copied into the ring per draw
    // instead. TENCHU_TRACE=nomirror uses the ring for everything.
    ID3D11Buffer* mirror = nullptr;
    ID3D11ShaderResourceView* mirrorView = nullptr;
    std::vector<uint8_t> pageUploads;      // re-uploads in the current window
    std::vector<uint8_t> pageSeen;         // uploaded at least once
    std::vector<uint64_t> pageHotUntil;    // frame number
    std::vector<uint64_t> hotBits;         // pages with pageHotUntil > frames, rebuilt per window
    uint64_t mirrorWindowStart = 0;
    uint64_t mirroredBytes = 0;            // per frame (trace 'hosttime')

    void createMirror() {
        if (traceEnabled("nomirror")) return;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = Memory::kPhysicalSize;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &mirror))) {
            logf("[d3d11] no guest memory mirror (512 MB buffer refused); vertex data goes through the ring");
            return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        view.BufferEx.NumElements = Memory::kPhysicalSize / 4;
        view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(device->CreateShaderResourceView(mirror, &view, &mirrorView))) {
            release(mirror);
            return;
        }
        pageUploads.assign(Memory::kPhysicalPages, 0);
        pageSeen.assign(Memory::kPhysicalPages, 0);
        pageHotUntil.assign(Memory::kPhysicalPages, 0);
        hotBits.assign(Memory::kPhysicalPages / 64, 0);
    }

    bool rangeHot(uint32_t first, uint32_t last) const {
        for (uint32_t word = first >> 6; word <= last >> 6; ++word) {
            uint64_t bits = hotBits[word];
            if (word == first >> 6) bits &= ~0ull << (first & 63);
            if (word == last >> 6) bits &= ~0ull >> (63 - (last & 63));
            if (bits) return true;
        }
        return false;
    }

    // Makes [physical, physical + size) current in the mirror. False: the range has
    // hot pages (or there is no mirror) and should be copied into the ring.
    bool mirrorRange(uint32_t physical, uint32_t size) {
        if (!mirror || !size || uint64_t(physical) + size > Memory::kPhysicalSize) return false;
        const uint32_t first = physical >> 12, last = (physical + size - 1) >> 12;
        if (rangeHot(first, last)) return false;
        if (memory->rangeWatched(Memory::kWatchMirror, first, last)) return true;  // nothing written since the upload
        uint32_t run = 0;
        for (uint32_t page = first; page <= last + 1; ++page) {
            if (page <= last && !memory->pageWatched(Memory::kWatchMirror, page)) {
                ++run;
                continue;
            }
            if (!run) continue;
            const uint32_t runFirst = page - run;
            memory->watchPages(Memory::kWatchMirror, runFirst, run);  // before copying: later writes fault
            const D3D11_BOX box{runFirst * 0x1000u, 0, 0, page * 0x1000u, 1, 1};
            context->UpdateSubresource(mirror, 0, &box, memory->host(Memory::fromPhysical(runFirst * 0x1000u)), 0, 0);
            mirroredBytes += uint64_t(run) * 0x1000;
            for (uint32_t p = runFirst; p < page; ++p) {
                if (pageSeen[p] && pageUploads[p] < 255) ++pageUploads[p];
                pageSeen[p] = 1;
            }
            run = 0;
        }
        return true;
    }

    // Once per frame: pages re-uploaded in most frames of the last window turn hot.
    void updateHotPages() {
        if (!mirror || frames - mirrorWindowStart < 30) return;
        std::fill(hotBits.begin(), hotBits.end(), 0ull);
        for (uint32_t page = 0; page < Memory::kPhysicalPages; ++page) {
            if (pageUploads[page] >= 15) pageHotUntil[page] = frames + 1800;
            pageUploads[page] = 0;
            if (pageHotUntil[page] > frames) hotBits[page >> 6] |= 1ull << (page & 63);
        }
        mirrorWindowStart = frames;
    }

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
    // Starts a new ring generation now unless `total` bytes still fit, so that a
    // draw's uploads never straddle a wrap. Wrapping maps the ring with DISCARD,
    // which renames it: data copied before the wrap is not in the buffer the draw
    // then reads (one-frame garbage geometry in one tile).
    void reserveUploads(uint64_t total) {
        if (total <= kRingSize && ringOffset + total > kRingSize) {
            endUploads();
            ringOffset = 0;
        }
    }

    // Returns the byte offset of the copy, or ~0u if it does not fit.
    uint32_t upload(const void* data, uint32_t size) {
        size = (size + 15) & ~15u;
        if (size > kRingSize) return ~0u;
        if (ringOffset + size > kRingSize) {
            // Only reachable when a draw's uploads exceed the whole ring.
            logf("[d3d11] upload ring wrapped inside a draw");
            endUploads();
            ringOffset = 0;
        }
        beginUploads();
        const uint32_t offset = ringOffset;
        uploadedBytes += size;
        std::memcpy(ringMapped + offset, data, size);
        ringOffset += size;
        return offset;
    }

    // Diagnostic hashes (trace 'flickerlog').
    static uint64_t hashFloats(const float (&values)[256 * 4]) {
        uint64_t hash = 1469598103934665603ull;
        for (float value : values) {
            uint32_t bits;
            std::memcpy(&bits, &value, 4);
            hash = (hash ^ bits) * 1099511628211ull;
        }
        return hash;
    }

    static uint64_t sampleChecksum(const uint8_t* data, uint32_t size) {
        uint64_t hash = 1469598103934665603ull;
        const uint32_t words = size / 4;
        const uint32_t step = (std::max)(1u, words / 64);
        for (uint32_t i = 0; i < words; i += step) {
            uint32_t word;
            std::memcpy(&word, data + size_t(i) * 4, 4);
            hash = (hash ^ word) * 1099511628211ull;
        }
        return hash;
    }

    // ---- Constant buffers ------------------------------------------------------------
    ID3D11Buffer* vertexConstants = nullptr;
    // Last contents written to the constant buffers; filled with a NaN pattern at
    // start-up so the first draw always uploads.
    float lastVertexConstants[256 * 4], lastPixelConstants[256 * 4];
    std::vector<uint32_t> indexScratch, drawIndices;
    ID3D11Buffer* pixelConstants = nullptr;
    ID3D11Buffer* sharedConstants = nullptr;
    ID3D11Buffer* clipConstants = nullptr;

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
    uint32_t bufferWrites[3] = {};  // per frame (trace 'hosttime'): shared, vertex, pixel constants

    // Writes one of the three constant blocks (0 vertex, 1 pixel, 2 shared).
    void writeConstants(int slot, const void* data, UINT size) {
        bufferWrites[slot == 2 ? 0 : slot == 0 ? 1 : 2]++;
        writeBuffer(slot == 0 ? vertexConstants : slot == 1 ? pixelConstants : sharedConstants, data, size);
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
        ID3D11GeometryShader* streamOut = nullptr;  // TENCHU_PROBE_VS: captures SV_Position + t0
        xenos::ShaderInfo info;
        bool failed = false;
        std::atomic<bool> done{false};  // set by the compiling thread after everything above
    };
    // Entries never move (workers fill them in); the map itself belongs to the
    // command processor thread.
    std::unordered_map<uint64_t, std::unique_ptr<CompiledShader>> shaderCache;

    // Shaders compile on worker threads; a draw whose shader is not ready yet is
    // skipped: a short pop-in the first time a scene appears instead of a freeze
    // (the largest shaders take seconds in FXC). TENCHU_SYNC_SHADERS=1 compiles inline.
    struct ShaderJob {
        uint64_t hash = 0;
        xenos::ShaderType type{};
        std::vector<uint32_t> words;
        CompiledShader* entry = nullptr;
    };
    std::mutex shaderJobMutex;
    std::condition_variable shaderJobReady;
    std::deque<ShaderJob> shaderJobs;
    std::vector<std::thread> shaderWorkers;

    void shaderWorker() {
        for (;;) {
            ShaderJob job;
            {
                std::unique_lock lock(shaderJobMutex);
                shaderJobReady.wait(lock, [&] { return !shaderJobs.empty(); });
                job = std::move(shaderJobs.front());
                shaderJobs.pop_front();
            }
            build(job.hash, job.type, job.words, *job.entry);
            job.entry->done.store(true, std::memory_order_release);
        }
    }
    ID3D11GeometryShader* rectangleShader = nullptr;
    ID3D11PixelShader* flatShader = nullptr;
    ID3D11VertexShader* copyVertexShader = nullptr;
    ID3D11PixelShader* copyPixelShader = nullptr;
    ID3D11PixelShader* depthCopyShaders[2] = {};  // single-sample, multisampled source
    ID3D11Buffer* copyConstants = nullptr;

    static std::filesystem::path shaderCacheDirectory() {
        static const std::filesystem::path directory = [] {
            auto path = userDirectory() / "shadercache";
            std::error_code error;
            std::filesystem::create_directories(path, error);
            return path;
        }();
        return directory;
    }

    CompiledShader* shader(uint64_t hash, xenos::ShaderType type) {
        auto& slot = shaderCache[hash];
        if (!slot) {
            slot = std::make_unique<CompiledShader>();
            const auto* words = shaderSource(hash);
            if (!words) {
                slot->failed = true;
                slot->done = true;
                return nullptr;
            }
            static const bool synchronous = [] {
                char value[8]{};
                return GetEnvironmentVariableA("TENCHU_SYNC_SHADERS", value, sizeof(value)) && value[0] == '1';
            }();
            if (synchronous) {
                build(hash, type, *words, *slot);
                slot->done = true;
            } else {
                if (shaderWorkers.empty()) {
                    const unsigned count = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 6u);
                    for (unsigned i = 0; i < count; ++i) shaderWorkers.emplace_back([this] { shaderWorker(); });
                    for (auto& worker : shaderWorkers) worker.detach();
                }
                {
                    std::lock_guard lock(shaderJobMutex);
                    shaderJobs.push_back({hash, type, *words, slot.get()});
                }
                shaderJobReady.notify_one();
            }
        }
        CompiledShader& entry = *slot;
        if (!entry.done.load(std::memory_order_acquire) || entry.failed) return nullptr;
        return &entry;
    }

    // Translates and compiles one shader into entry (any thread: the D3D11 device
    // is thread-safe for resource creation).
    void build(uint64_t hash, xenos::ShaderType type, const std::vector<uint32_t>& words, CompiledShader& entry) {
        auto translated = xenos::translate(xenos::decode(type, words));
        entry.info = translated.info;
        // TENCHU_PS_OVERRIDE=<hash>=<hlsl> replaces that pixel shader's color output, e.g. r[6].
        static const std::string override = [] {
            char value[256]{};
            return GetEnvironmentVariableA("TENCHU_PS_OVERRIDE", value, sizeof(value)) ? std::string(value) : std::string();
        }();
        if (type == xenos::ShaderType::Pixel && !override.empty()) {
            char prefix[20];
            snprintf(prefix, sizeof(prefix), "%016llX=", (unsigned long long)hash);
            const size_t at = translated.hlsl.find("    // Fixed-function alpha test");
            if (override.rfind(prefix, 0) == 0 && at != std::string::npos)
                // A leading '!' assigns all four components (alpha included).
                translated.hlsl.insert(at, override[17] == '!'
                                               ? "    oC[0] = " + override.substr(18) + ";\n"
                                               : "    oC[0] = float4((" + override.substr(17) + ").xyz, 1.0);\n");
        }
        if (!translated.error.empty())
            logf("[d3d11] shader %016llX: %s (translated anyway)", (unsigned long long)hash, translated.error.c_str());
        // TENCHU_DUMP_SHADERS=<dir>: writes each translated shader as <dir>/<hash>.vs/.ps.hlsl.
        static const std::wstring dumpDirectory = [] {
            wchar_t value[MAX_PATH]{};
            return GetEnvironmentVariableW(L"TENCHU_DUMP_SHADERS", value, MAX_PATH) ? std::wstring(value) : std::wstring();
        }();
        if (!dumpDirectory.empty()) {
            wchar_t name[40];
            swprintf(name, 40, L"\\%016llX.%ls.hlsl", (unsigned long long)hash, type == xenos::ShaderType::Pixel ? L"ps" : L"vs");
            if (std::FILE* file = _wfopen((dumpDirectory + name).c_str(), L"wb")) {
                std::fwrite(translated.hlsl.data(), 1, translated.hlsl.size(), file);
                std::fclose(file);
            }
        }
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const bool pixel = type == xenos::ShaderType::Pixel;
        const char* profile = pixel ? "ps_5_0" : "vs_5_0";
        // TENCHU_SHADER_OPT: optimization level 0-3 (default 1), or -1 to skip optimization.
        static const int optimization = [] {
            char value[8]{};
            return GetEnvironmentVariableA("TENCHU_SHADER_OPT", value, sizeof(value)) ? atoi(value) : 1;
        }();
        const UINT flags = optimization < 0 ? D3DCOMPILE_SKIP_OPTIMIZATION
                         : optimization == 0 ? D3DCOMPILE_OPTIMIZATION_LEVEL0
                         : optimization == 1 ? D3DCOMPILE_OPTIMIZATION_LEVEL1
                         : optimization == 2 ? D3DCOMPILE_OPTIMIZATION_LEVEL2
                                             : D3DCOMPILE_OPTIMIZATION_LEVEL3;
        // Compiled bytecode is cached on disk, keyed by the translated source, profile and
        // flags, so a translator change never reuses stale code.
        uint64_t sourceHash = 1469598103934665603ull;
        for (const char c : translated.hlsl) sourceHash = (sourceHash ^ uint8_t(c)) * 1099511628211ull;
        sourceHash = (sourceHash ^ flags) * 1099511628211ull;
        char cacheName[64];
        snprintf(cacheName, sizeof(cacheName), "%s_%016llX.dxbc", profile, (unsigned long long)sourceHash);
        const auto cachePath = shaderCacheDirectory() / cacheName;
        if (std::FILE* file = _wfopen(cachePath.c_str(), L"rb")) {
            std::fseek(file, 0, SEEK_END);
            const long size = std::ftell(file);
            std::fseek(file, 0, SEEK_SET);
            if (size > 0 && SUCCEEDED(D3DCreateBlob(size_t(size), &code)) &&
                std::fread(code->GetBufferPointer(), 1, size_t(size), file) != size_t(size))
                release(code);
            std::fclose(file);
        }
        TZ_TRACE("compile", "[d3d11] %s %s shader %016llX (%zu cached)", code ? "loading" : "compiling",
                 pixel ? "pixel" : "vertex", (unsigned long long)hash, size_t(0));
        const auto compileStart = std::chrono::steady_clock::now();
        struct CompileTimer {
            std::chrono::steady_clock::time_point start;
            uint64_t hash;
            ~CompileTimer() {
                TZ_TRACE("compile", "[d3d11] shader %016llX ready in %.0f ms", (unsigned long long)hash,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
        } compileTimer{compileStart, hash};
        if (!code &&
            SUCCEEDED(D3DCompile(translated.hlsl.data(), translated.hlsl.size(), nullptr, nullptr, nullptr, "main",
                                 profile, flags, 0, &code, &errors))) {
            if (std::FILE* file = _wfopen(cachePath.c_str(), L"wb")) {
                std::fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), file);
                std::fclose(file);
            }
        }
        if (!code) {
            logf("[d3d11] shader %016llX failed to compile: %s", (unsigned long long)hash,
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            entry.failed = true;
        } else if (pixel) {
            device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &entry.pixel);
        } else {
            device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &entry.vertex);
            char probe[32]{};
            if (GetEnvironmentVariableA("TENCHU_PROBE_VS", probe, sizeof(probe)) && strtoull(probe, nullptr, 16) == hash) {
                const D3D11_SO_DECLARATION_ENTRY declaration[] = {{0, "SV_Position", 0, 0, 4, 0}, {0, "TEXCOORD", 0, 0, 4, 0}};
                const UINT stride = 32;
                device->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(), code->GetBufferSize(), declaration, 2,
                                                             &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr,
                                                             &entry.streamOut);
            }
        }
        release(code);
        release(errors);
    }

    // ---- Resolution scaling ----------------------------------------------------------
    // Host render targets and resolved textures are renderScale times the guest
    // size; guest coordinates are scaled only where they meet host resources.
    // Fractional at 1.5 (1080p): guest coordinates round to host pixels the same way
    // everywhere, so neighboring rectangles (tiles, resolves) still meet exactly.
    const float renderScale = std::clamp(host::settings().resolutionScale, 1.0f, 4.0f);
    LONG scaled(LONG value) const { return LONG(std::lround(double(value) * renderScale)); }
    UINT scaled(UINT value) const { return UINT(std::lround(double(value) * renderScale)); }
    D3D11_RECT scaled(const D3D11_RECT& rect) const {
        return {scaled(rect.left), scaled(rect.top), scaled(rect.right), scaled(rect.bottom)};
    }

    // ---- MSAA override -----------------------------------------------------------------
    // [graphics] msaa (applied at start): the host sample count of the title's 2x targets.
    // Target keys and guest sizes keep the title's count (the 640x1024 tile layout depends
    // on it). Every format a target can have must support the count, else it halves.
    const uint32_t msaaSamples = validatedMsaa();
    uint32_t validatedMsaa() const {
        uint32_t samples = uint32_t(host::settings().msaa);
        // The 640-pitch tiles are 640x1024 times the scale squared: at scale 3, 8x D32S8X24
        // depth alone takes about 380 MB.
        if (renderScale >= 3) samples = (std::min)(samples, 4u);
        static const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
                                              DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                              DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
                                              DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
        for (; samples > 2; samples /= 2) {
            bool supported = true;
            for (const DXGI_FORMAT format : formats) {
                UINT levels = 0;
                supported = supported && SUCCEEDED(device->CheckMultisampleQualityLevels(format, samples, &levels)) &&
                            levels;
            }
            if (supported) break;
            logf("[d3d11] %ux MSAA is not supported for every target format", samples);
        }
        if (samples != 2) logf("[d3d11] %ux MSAA for the title's 2x targets", samples);
        return samples;
    }

    // ---- Render targets (EDRAM) --------------------------------------------------
    // Each distinct EDRAM base/pitch/format/samples is a host render target.
    struct RenderTarget {
        ID3D11Texture2D* texture = nullptr;
        ID3D11RenderTargetView* color = nullptr;
        ID3D11DepthStencilView* depth = nullptr;
        // Single-sample copy used by resolves of multisampled targets.
        ID3D11Texture2D* resolveScratch = nullptr;
        ID3D11ShaderResourceView* resolveScratchView = nullptr;
        ID3D11ShaderResourceView* view = nullptr;  // single-sample targets only
        ID3D11ShaderResourceView* depthView = nullptr;  // depth targets (multisampled or not)
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
        const uint32_t guestHeight = (std::max)(16u, height / (samples >= 2 ? 2 : 1));
        const uint32_t hostSamples = samples == 2 ? msaaSamples : samples;
        desc.Width = scaled(UINT(pitch));
        desc.Height = scaled(UINT(guestHeight));
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = hostSamples;
        desc.Usage = D3D11_USAGE_DEFAULT;
        if (isDepth) {
            // Typeless so depth resolves can read it.
            desc.Format = format ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
            desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        } else {
            desc.Format = colorFormat(format);
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        }
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &target.texture))) {
            logf("[d3d11] cannot create %ux%u render target", desc.Width, desc.Height);
            return nullptr;
        }
        if (isDepth) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format = format ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
            dsv.ViewDimension = hostSamples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
            if (FAILED(device->CreateDepthStencilView(target.texture, &dsv, &target.depth)))
                logf("[d3d11] cannot create depth view (format %u, %u samples)", format, samples);
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = format ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            srv.ViewDimension = hostSamples > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            if (FAILED(device->CreateShaderResourceView(target.texture, &srv, &target.depthView)))
                logf("[d3d11] cannot create depth shader view (format %u, %u samples)", format, samples);
        } else {
            device->CreateRenderTargetView(target.texture, nullptr, &target.color);
            if (hostSamples > 1) {
                desc.SampleDesc.Count = 1;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                device->CreateTexture2D(&desc, nullptr, &target.resolveScratch);
                if (target.resolveScratch)
                    device->CreateShaderResourceView(target.resolveScratch, nullptr, &target.resolveScratchView);
            } else {
                device->CreateShaderResourceView(target.texture, nullptr, &target.view);
            }
        }
        target.width = pitch;
        target.height = guestHeight;
        target.samples = hostSamples;
        target.format = desc.Format;
        TZ_TRACE("rt", "[d3d11] %s target base %u pitch %u format %u samples %u (host %u) -> %ux%u",
                 isDepth ? "depth" : "color", base, pitch, format, samples, hostSamples, desc.Width, desc.Height);
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
            desc.Width = scaled(UINT(width));
            desc.Height = scaled(UINT(height));
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
    std::map<std::array<uint32_t, 5>, ID3D11BlendState*> blendStates;
    std::map<std::pair<uint32_t, uint32_t>, ID3D11DepthStencilState*> depthStates;
    std::map<uint32_t, ID3D11RasterizerState*> rasterStates;
    std::map<uint32_t, ID3D11SamplerState*> samplers;
    TextureCache textures;
    PostEffects postEffects;
    ID3D11Texture2D* whiteTexture = nullptr;
    ID3D11ShaderResourceView* whiteView = nullptr;

    ID3D11BlendState* blendState(const uint32_t control[4], uint32_t colorMask) {
        const std::array<uint32_t, 5> key{control[0], control[1], control[2], control[3], colorMask};
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
        static const bool noClip = traceEnabled("nozclip"), noScissor = traceEnabled("noscissor");
        desc.DepthClipEnable = !clipDisabled && !noClip;
        desc.ScissorEnable = !noScissor;
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
        createMirror();
        vertexConstants = createConstantBuffer(256 * 16);
        pixelConstants = createConstantBuffer(256 * 16);
        sharedConstants = createConstantBuffer((sizeof(SharedConstants) + 15) & ~15u);
        clipConstants = createConstantBuffer(sizeof(ClipConstants));

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
        for (int multisampled = 0; multisampled < 2; ++multisampled) {
            const D3D_SHADER_MACRO defines[] = {{"MULTISAMPLED", "1"}, {nullptr, nullptr}};
            if (SUCCEEDED(D3DCompile(kDepthCopyShader, strlen(kDepthCopyShader), "depthcopy",
                                     multisampled ? defines : nullptr, nullptr, "ps", "ps_5_0", 0, 0, &code, &errors)))
                device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                          &depthCopyShaders[multisampled]);
            else
                logf("[d3d11] depth copy shader: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            release(code);
            release(errors);
        }
        copyConstants = createConstantBuffer(32);
        std::memset(lastVertexConstants, 0xFF, sizeof(lastVertexConstants));
        std::memset(lastPixelConstants, 0xFF, sizeof(lastPixelConstants));

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
        postEffects.initialize(device, context);
    }

    // ---- Indices -------------------------------------------------------------------
    // Builds a host list of guest vertex indices for a D3D11 topology.
    bool buildIndices(const DrawRequest& request, uint32_t primitive, uint32_t count, std::vector<uint32_t>& out,
                      D3D11_PRIMITIVE_TOPOLOGY& topology, bool& rectangles) {
        const uint32_t source = (request.initiator >> 6) & 3;
        const bool wide = (request.initiator >> 11) & 1;
        std::vector<uint32_t>& indices = indexScratch;  // reused across draws
        indices.resize(count);
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
    // Returns the number of bindings written to views/states (at most 16).
    UINT bindTextures(const xenos::ShaderInfo& info, SharedConstants& shared, ID3D11ShaderResourceView** views,
                      ID3D11SamplerState** states) {
        const UINT count = UINT((std::min)(info.textures.size(), size_t(16)));
        for (UINT slot = 0; slot < count; ++slot) {
            views[slot] = whiteView;
            states[slot] = defaultSampler();
        }
        for (size_t slot = 0; slot < count; ++slot) {
            TextureFetch fetch;
            for (int i = 0; i < 6; ++i)
                fetch.words[i] = reg(reg::SHADER_CONSTANT_FETCH_00_0 + info.textures[slot].fetchConstant * 6 + i);
            shared.textureSwizzle[slot] = fetch.swizzle();
            shared.textureSize[slot][0] = float(fetch.width());
            shared.textureSize[slot][1] = float(fetch.height());
            states[slot] = textures.sampler(fetch, info.textures[slot].filters);
            auto it = resolved.find(fetch.baseAddress() & 0x1FFFFFFF);
            if (it != resolved.end() && it->second.view) {
                if (traceFrame())
                    logf("[draw]   texture %u uses resolved %08X (%ux%u; fetch %ux%u)", uint32_t(slot), it->first,
                         it->second.width, it->second.height, fetch.width(), fetch.height());
                views[slot] = it->second.view;
                shared.textureScale[slot][0] = float(fetch.width()) / float((std::max)(1u, it->second.width));
                shared.textureScale[slot][1] = float(fetch.height()) / float((std::max)(1u, it->second.height));
                continue;
            }
            if (traceFrame())
                logf("[draw]   texture %u at %08X format %u %ux%u", uint32_t(slot), fetch.baseAddress(), fetch.format(),
                     fetch.width(), fetch.height());
            const auto textureStart = stamp();
            if (auto* view = textures.view(fetch)) views[slot] = view;
            else if (traceFrame())
                logf("[draw]   texture %u UNSUPPORTED: type %u dimension %u format %u (shader dim %u)", uint32_t(slot),
                     fetch.type(), fetch.dimension(), fetch.format(), info.textures[slot].dimension);
            addTime(hostTime[0], textureStart);
        }
        return count;
    }

    // The resolved texture a shader's texture binding reads (as bindTextures found it), if any.
    ResolvedTexture* resolvedBinding(const xenos::ShaderInfo& info, UINT slot) {
        TextureFetch fetch;
        for (int i = 0; i < 6; ++i)
            fetch.words[i] = reg(reg::SHADER_CONSTANT_FETCH_00_0 + info.textures[slot].fetchConstant * 6 + i);
        auto it = resolved.find(fetch.baseAddress() & 0x1FFFFFFF);
        return it != resolved.end() && it->second.view ? &it->second : nullptr;
    }

    // Enhanced lighting: the scene composite reads its scene (fetch constant 2) with
    // ambient occlusion and sharpening applied (post_effects.h), computed from the depth
    // resolve it reads through fetch constant 3. The substitute has the resolve's size and
    // format, so the binding's texture scale stays right.
    void substituteCompositeScene(const xenos::ShaderInfo& info, ID3D11ShaderResourceView** views, UINT count) {
        int sceneSlot = -1, depthSlot = -1;
        for (UINT slot = 0; slot < count; ++slot) {
            if (info.textures[slot].fetchConstant == 2) sceneSlot = int(slot);
            else if (info.textures[slot].fetchConstant == 3) depthSlot = int(slot);
        }
        ResolvedTexture* scene = sceneSlot >= 0 ? resolvedBinding(info, UINT(sceneSlot)) : nullptr;
        ResolvedTexture* depth = depthSlot >= 0 ? resolvedBinding(info, UINT(depthSlot)) : nullptr;
        if (!scene || !depth || views[sceneSlot] != scene->view) {
            postEffects.skipped("the composite does not read a resolved scene and depth");
            return;
        }
        auto* view = postEffects.processScene(scene->texture, scene->view, depth->texture, depth->view, resolveGeneration);
        if (!view) return;
        views[sceneSlot] = view;
        forgetBoundState();  // the passes bound their own pipeline (like resolve())
        if (dumpFrame()) {
            CreateDirectoryA("build/resolved", nullptr);
            saveTexture(postEffects.output(), "build/resolved/postfx_scene.bmp");
        }
    }

    // Stable bloom ([graphics] stable_bloom): at resolution scales above 1 the bloom shrink
    // reads the scene box-filtered back to its guest size, the footprint its taps were made
    // for (post_effects.h). Normalized UVs, so the binding's texture scale stays right.
    void substituteBloomSource(const xenos::ShaderInfo& info, ID3D11ShaderResourceView** views, UINT count) {
        ResolvedTexture* scene = count ? resolvedBinding(info, 0) : nullptr;
        if (!scene || views[0] != scene->view) return;
        auto* view = postEffects.bloomSource(scene->view, scene->width, scene->height, uint32_t(renderScale), resolveGeneration);
        if (view) {
            views[0] = view;
            forgetBoundState();  // the pass bound its own pipeline
        }
    }

    // TENCHU_TRACE=postfx: names other shaders that look like a scene composite (a
    // rectangle into a single-sample target, reading a resolved R32 depth and a resolved
    // RGBA8 image of the same size), e.g. cutscene or underwater variants not hooked yet.
    std::set<uint64_t> compositeCandidates;
    void noteCompositeCandidate(uint64_t psHash, const xenos::ShaderInfo& info, UINT count) {
        if (compositeCandidates.count(psHash)) return;
        bool depth = false, color = false;
        UINT width = 0, height = 0;
        for (UINT slot = 0; slot < count; ++slot)
            if (ResolvedTexture* texture = resolvedBinding(info, slot)) {
                if (texture->format == DXGI_FORMAT_R32_FLOAT) depth = true;
                else if (texture->format == DXGI_FORMAT_R8G8B8A8_UNORM) color = true;
                else continue;
                if (width && (texture->width != width || texture->height != height)) return;
                width = texture->width;
                height = texture->height;
            }
        if (!depth || !color) return;
        compositeCandidates.insert(psHash);
        logf("[postfx] pixel shader %016llX looks like a scene composite (%ux%u), not hooked",
             (unsigned long long)psHash, width, height);
    }

    // ---- Draw ----------------------------------------------------------------------
    // Host time per frame (trace 'hosttime'): texture uploads, draws, resolves, present.
    std::chrono::steady_clock::duration hostTime[6]{};  // + constants, D3D11 draw call
    // Clock reads cost on every draw: only taken with the trace on.
    static bool hostTiming() {
        static const bool on = traceEnabled("hosttime");
        return on;
    }
    static std::chrono::steady_clock::time_point stamp() {
        return hostTiming() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    }
    static void addTime(std::chrono::steady_clock::duration& total, std::chrono::steady_clock::time_point start) {
        if (hostTiming()) total += std::chrono::steady_clock::now() - start;
    }
    // Pipeline state last set on the context: draws only call D3D for changes.
    // Everything else that binds state (resolves, present, mip generation) makes
    // the next draw set all of it again (forgetBoundState).
    struct BoundState {
        bool valid = false;
        ID3D11RenderTargetView* colors[4]{};
        ID3D11DepthStencilView* depth = nullptr;
        D3D11_VIEWPORT viewport{};
        D3D11_RECT scissor{};
        ID3D11RasterizerState* raster = nullptr;
        ID3D11BlendState* blend = nullptr;
        float blendColor[4]{};
        ID3D11DepthStencilState* depthStencil = nullptr;
        UINT stencilRef = 0;
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        ID3D11VertexShader* vertex = nullptr;
        ID3D11GeometryShader* geometry = nullptr;
        ID3D11PixelShader* pixel = nullptr;
        bool texturesValid = false;  // D3D unbinds views that alias new render targets
        ID3D11ShaderResourceView* vertexViews[16]{};
        ID3D11SamplerState* vertexSamplers[16]{};
        ID3D11ShaderResourceView* pixelViews[16]{};
        ID3D11SamplerState* pixelSamplers[16]{};
        SharedConstants shared{};
        bool sharedValid = false;
        ClipConstants clip{};
        bool clipValid = false;
    } bound;
    uint64_t texturePipelineChanges = 0;
    void forgetTextureBindings() {
        bound.texturesValid = false;
        std::memset(bound.vertexViews, 0, sizeof(bound.vertexViews));
        std::memset(bound.vertexSamplers, 0, sizeof(bound.vertexSamplers));
        std::memset(bound.pixelViews, 0, sizeof(bound.pixelViews));
        std::memset(bound.pixelSamplers, 0, sizeof(bound.pixelSamplers));
    }
    void forgetBoundState() {
        bound.valid = false;
        forgetTextureBindings();
    }
    struct ScopedTime {
        std::chrono::steady_clock::duration& total;
        std::chrono::steady_clock::time_point start = stamp();
        ~ScopedTime() { addTime(total, start); }
    };
    void draw(const DrawRequest& request, uint64_t vsHash, uint64_t psHash) {
        ScopedTime timed{hostTime[1]};
        const uint32_t edramMode = reg(reg::RB_MODECONTROL) & 7;
        if (edramMode == 6) {
            resolve();
            return;
        }
        if (edramMode != 4 && edramMode != 5) { ++dropped[0]; return; }  // mode
        const uint32_t primitive = request.initiator & 0x3F;
        const uint32_t count = request.initiator >> 16;
        if (!count) { ++dropped[1]; return; }  // count

        // TENCHU_SKIP_VS=<hash>: drop that vertex shader's draws (finding which draw is which).
        static const uint64_t skipVertexShader = [] {
            char value[32]{};
            return GetEnvironmentVariableA("TENCHU_SKIP_VS", value, sizeof(value)) ? strtoull(value, nullptr, 16) : 0ull;
        }();
        if (skipVertexShader && vsHash == skipVertexShader) return;
        // TENCHU_FIND_WRITER=<vs hash>: log which guest code writes that shader's vertex data.
        static const uint64_t findWriter = [] {
            char value[32]{};
            return GetEnvironmentVariableA("TENCHU_FIND_WRITER", value, sizeof(value)) ? strtoull(value, nullptr, 16) : 0ull;
        }();
        auto* vs = shader(vsHash, xenos::ShaderType::Vertex);
        auto* ps = edramMode == 4 ? shader(psHash, xenos::ShaderType::Pixel) : nullptr;
        if (!vs || (edramMode == 4 && !ps)) { ++dropped[2]; return; }  // shader

        // Direct draws: list primitives whose vertex buffers and indices are all current
        // in the mirror read their indices in the vertex shader, so the index list is
        // neither converted nor uploaded. Strips, fans, quads, inline indices and hot
        // (rewritten every frame) data take the host path below.
        const uint32_t indexSource = (request.initiator >> 6) & 3;
        const bool wideIndices = (request.initiator >> 11) & 1;
        const auto& bindings = vs->info.vertexFetchConstants;
        const size_t streamCount = (std::min)(bindings.size(), size_t(16));
        static const bool noDirect = traceEnabled("nodirect");
        bool direct = mirror && !noDirect && (primitive <= 4 || primitive == 8) && primitive >= 1 &&
                      (indexSource == 2 || (indexSource == 0 && !(request.indexBase & 3)));
        for (size_t slot = 0; direct && slot < streamCount; ++slot) {
            const uint32_t base = reg::SHADER_CONSTANT_FETCH_00_0 + bindings[slot] * 2;
            const uint32_t size = (std::max)(((reg(base + 1) >> 2) & 0xFFFFFF) * 4, 16u);
            direct = mirrorRange(reg(base) & 0x1FFFFFFC, size);
        }
        if (direct && indexSource == 0) direct = mirrorRange(request.indexBase & 0x1FFFFFFF, count * (wideIndices ? 4 : 2));
        if (findWriter && vsHash == findWriter && !bindings.empty()) {
            const uint32_t base = reg::SHADER_CONSTANT_FETCH_00_0 + bindings[0] * 2;
            const uint32_t address = reg(base) & 0x1FFFFFFC, size = ((reg(base + 1) >> 2) & 0xFFFFFF) * 4;
            static int logged = 0;
            if (logged++ < 20) logf("[writer] draw reads %08X size %X", address, size);
            memory->probeWrites(address >> 12, ((address + (std::max)(size, 4u) - 1) >> 12) - (address >> 12) + 1);
        }

        std::vector<uint32_t>& indices = drawIndices;  // reused across draws
        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        bool rectangles = primitive == 8;
        uint32_t vertexCount = count;
        if (direct) {
            topology = primitive == 1 ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
                     : primitive == 2 ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                     : primitive == 3 ? D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP
                                      : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        } else {
            if (!buildIndices(request, primitive, count, indices, topology, rectangles)) {
                TZ_TRACE("draw", "[d3d11] unsupported primitive type %u", primitive);
                ++dropped[6];
                return;
            }
            if (indices.empty()) { ++dropped[3]; return; }  // indices
            vertexCount = uint32_t(indices.size());
        }

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
        if (!firstTarget) {
            if (traceFrame())
                logf("[draw] DROPPED (no target) prim %u count %u vs %016llX ps %016llX colormask %X psmask %X depthctl %08X "
                     "rt %08X depth %08X modecontrol %X",
                     primitive, count, (unsigned long long)vsHash, (unsigned long long)psHash, colorMask,
                     ps ? ps->info.colorTargetMask : 0, depthControl, reg(reg::RB_COLOR_INFO), reg(reg::RB_DEPTH_INFO),
                     reg(reg::RB_MODECONTROL));
            ++dropped[4];
            return;
        }  // target

        // Uploads: vertex data per binding, then the index list.
        SharedConstants shared{};
        for (int i = 0; i < 8; ++i) shared.bools[i] = reg(reg::SHADER_CONSTANT_BOOL_000_031 + i);
        for (int i = 0; i < 32; ++i) shared.loops[i] = reg(reg::SHADER_CONSTANT_LOOP_00 + i);
        if (direct) {
            for (size_t slot = 0; slot < streamCount; ++slot) {
                const uint32_t base = reg::SHADER_CONSTANT_FETCH_00_0 + bindings[slot] * 2;
                shared.vertexEndian[slot] = (reg(base + 1) & 3) | 4;
                shared.vertexBase[slot] = reg(base) & 0x1FFFFFFC;
            }
            shared.draw[0] = indexSource == 2 ? ~0u : request.indexBase & 0x1FFFFFFF;
            shared.draw[1] = reg(reg::VGT_INDX_OFFSET);
            shared.draw[2] = indexSource == 2 ? 0 : 1 | (wideIndices ? 2 : 0) | (request.indexEndian & 3) << 2;
        } else {
        // Vertex data not current in the mirror is copied for every draw: dynamic buffers
        // change between draws of a frame. Streams indexed by the raw vertex index copy
        // only the bytes this draw's index range can reach.
        uint32_t minIndex = ~0u, maxIndex = 0;
        for (const uint32_t index : indices) {
            minIndex = (std::min)(minIndex, index);
            maxIndex = (std::max)(maxIndex, index);
        }
        const uint32_t indexOffset = reg(reg::VGT_INDX_OFFSET);
        const uint32_t firstVertex = minIndex + indexOffset, lastVertex = maxIndex + indexOffset;
        const bool rangeKnown = !indices.empty() && firstVertex <= lastVertex;  // no wraparound
        // Plan every upload of this draw first: they must land in the same ring
        // generation, since a wrap renames the buffer the draw will read.
        struct StreamUpload { uint32_t address, start, length; bool mirrored; };
        StreamUpload streams[16];
        uint64_t total = (uint64_t(indices.size()) * 4 + 15) & ~15ull;
        for (size_t slot = 0; slot < streamCount; ++slot) {
            const uint32_t base = reg::SHADER_CONSTANT_FETCH_00_0 + bindings[slot] * 2;
            const uint32_t word0 = reg(base), word1 = reg(base + 1);
            const uint32_t address = word0 & ~3u, size = (std::max)(((word1 >> 2) & 0xFFFFFF) * 4, 16u);
            shared.vertexEndian[slot] = word1 & 3;
            static const bool vbStats = traceEnabled("vbstats"), wholeBuffers = traceEnabled("wholevb");
            if (vbStats) vertexStats[{address, size}]++;
            uint64_t start = 0, end = size;
            if (rangeKnown && slot < vs->info.vertexStreams.size() && vs->info.vertexStreams[slot].direct &&
                !wholeBuffers) {
                const auto& stream = vs->info.vertexStreams[slot];
                start = (uint64_t(firstVertex) * stream.minStride + stream.minOffset) * 4;
                end = (std::min)((uint64_t(lastVertex) * stream.maxStride + stream.maxEnd) * 4, uint64_t(size));
                if (start >= end) start = end = 0;  // out of range: the draw reads nothing valid
            }
            streams[slot] = {address, uint32_t(start), (std::max)(uint32_t(end - start), 16u), false};
            streams[slot].mirrored = mirrorRange((address & 0x1FFFFFFF) + uint32_t(start), streams[slot].length);
            if (streams[slot].mirrored) shared.vertexEndian[slot] |= 4;  // read from the mirror
            else total += (uint64_t(streams[slot].length) + 15) & ~15ull;
        }
        reserveUploads(total);
        for (size_t slot = 0; slot < streamCount; ++slot) {
            const auto& stream = streams[slot];
            if (stream.mirrored) {
                shared.vertexBase[slot] = stream.address & 0x1FFFFFFF;
                continue;
            }
            const uint32_t offset = upload(memory->host(Memory::fromPhysical(stream.address + stream.start)), stream.length);
            // The shader adds (index * stride + offset) * 4 to the base; wraparound is intended.
            shared.vertexBase[slot] = offset == ~0u ? ~0u : offset - stream.start;
        }
        shared.draw[0] = upload(indices.data(), uint32_t(indices.size() * 4));
        shared.draw[1] = reg(reg::VGT_INDX_OFFSET);
        endUploads();
        if (shared.draw[0] == ~0u) { ++dropped[5]; return; }  // upload
        }

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
        // Enhanced lighting: depth-writing draws vote for the camera projection that ambient
        // occlusion rebuilds view positions with (post_effects.h).
        if (depthView && (depthControl & 4) && postEffects.wantsProjection())
            postEffects.noteProjection(reinterpret_cast<const float*>(registers) + reg::SHADER_CONSTANT_000_X, scale[2],
                                       offset[2]);
        const uint32_t colorControl = reg(reg::RB_COLORCONTROL);
        shared.alphaTest[0] = (colorControl >> 3) & 1 ? float(colorControl & 7) : 7.0f;
        shared.alphaTest[1] = asFloat(reg(reg::RB_ALPHA_REF));
        for (int i = 0; i < 16; ++i) {
            shared.textureSize[i][0] = shared.textureSize[i][1] = shared.textureSize[i][2] = 1.0f;
            shared.textureScale[i][0] = shared.textureScale[i][1] = 1.0f;
            shared.textureSwizzle[i] = 0 | 1 << 3 | 2 << 6 | 3 << 9;
        }
        ID3D11ShaderResourceView* pixelViews[16]{};
        ID3D11ShaderResourceView* vertexViews[16]{};
        ID3D11SamplerState* pixelSamplers[16]{};
        ID3D11SamplerState* vertexSamplers[16]{};
        // Vertex and pixel shaders share SharedConstants; pixel bindings win on overlap.
        const UINT vertexViewCount = bindTextures(vs->info, shared, vertexViews, vertexSamplers);
        const UINT pixelViewCount = ps ? bindTextures(ps->info, shared, pixelViews, pixelSamplers) : 0;
        // Uploading or mipmapping a texture draws with its own pipeline state.
        if (textures.pipelineChanges() != texturePipelineChanges) {
            texturePipelineChanges = textures.pipelineChanges();
            forgetBoundState();
        }
        if (ps && psHash == kSceneCompositeShader) {
            if (postEffects.enabled()) substituteCompositeScene(ps->info, pixelViews, pixelViewCount);
        } else if (ps && psHash == kBloomShrinkShader) {
            if (renderScale >= 2 && host::settings().stableBloom)
                substituteBloomSource(ps->info, pixelViews, pixelViewCount);
        } else if (ps && rectangles && tracePostFx && firstTarget->samples == 1 && pixelViewCount >= 2) {
            noteCompositeCandidate(psHash, ps->info, pixelViewCount);
        }
        if (!bound.sharedValid || std::memcmp(&shared, &bound.shared, sizeof(shared)) != 0) {
            bound.shared = shared;
            bound.sharedValid = true;
            writeConstants(2, &shared, sizeof(shared));
        }
        // User clip planes: PA_CL_CLIP_CNTL enables them unless clipping is disabled.
        // UCP_CULL_ONLY (bit 17, whole primitives culled) is approximated by clipping.
        static const bool noUserClip = traceEnabled("noucp");
        ClipConstants clip{};
        const uint32_t clipControl = reg(reg::PA_CL_CLIP_CNTL);
        clip.enabled[0] = (clipControl >> 16) & 1 || noUserClip ? 0 : clipControl & 63;
        for (int plane = 0; plane < 6; ++plane)
            if ((clip.enabled[0] >> plane) & 1)
                for (int lane = 0; lane < 4; ++lane) clip.planes[plane][lane] = asFloat(reg(reg::PA_CL_UCP_0_X + plane * 4 + lane));
        if (!bound.clipValid || std::memcmp(&clip, &bound.clip, sizeof(clip)) != 0) {
            bound.clip = clip;
            bound.clipValid = true;
            writeBuffer(clipConstants, &clip, sizeof(clip));
        }

        const auto constantsStart = stamp();
        // Constant buffers are rewritten only when the registers changed.
        // The command processor flags constant register writes (constantsWritten);
        // the registers hold the floats in buffer layout, so they are copied as is.
        static const bool alwaysConstants = traceEnabled("noconstskip");
        // std::atomic<uint32_t> has uint32_t's layout; only this thread writes them.
        const auto* constantRegisters = reinterpret_cast<const uint32_t*>(registers) + reg::SHADER_CONSTANT_000_X;
        // Titles rewrite constants with the same values between draws: a 4 KB compare
        // with the last upload is much cheaper than mapping the buffer again.
        if (alwaysConstants || constantsWritten[0].exchange(false, std::memory_order_relaxed)) {
            if (alwaysConstants || std::memcmp(lastVertexConstants, constantRegisters, 256 * 16) != 0) {
                std::memcpy(lastVertexConstants, constantRegisters, 256 * 16);
                writeConstants(0, constantRegisters, 256 * 16);
            }
        }
        if (ps && (alwaysConstants || constantsWritten[1].exchange(false, std::memory_order_relaxed))) {
            if (alwaysConstants || std::memcmp(lastPixelConstants, constantRegisters + 1024, 256 * 16) != 0) {
                std::memcpy(lastPixelConstants, constantRegisters + 1024, 256 * 16);
                writeConstants(1, constantRegisters + 1024, 256 * 16);
            }
        }
        addTime(hostTime[4], constantsStart);

        // Pipeline (only what changed since the last draw).
        if (!bound.valid) {
            context->IASetInputLayout(nullptr);
            ID3D11Buffer* vsBuffers[3] = {vertexConstants, sharedConstants, clipConstants};
            context->VSSetConstantBuffers(0, 3, vsBuffers);
            ID3D11Buffer* psBuffers[2] = {pixelConstants, sharedConstants};
            context->PSSetConstantBuffers(0, 2, psBuffers);
        }
        if (!bound.valid || std::memcmp(colorViews, bound.colors, sizeof(colorViews)) != 0 || depthView != bound.depth) {
            context->OMSetRenderTargets(4, colorViews, depthView);
            std::memcpy(bound.colors, colorViews, sizeof(colorViews));
            bound.depth = depthView;
            forgetTextureBindings();
        }
        const D3D11_VIEWPORT viewport{0, 0, float(scaled(UINT(width))), float(scaled(UINT(height))), 0, 1};
        if (!bound.valid || std::memcmp(&viewport, &bound.viewport, sizeof(viewport)) != 0) {
            context->RSSetViewports(1, &viewport);
            bound.viewport = viewport;
        }
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
        const D3D11_RECT hostScissor = scaled(scissor);
        if (!bound.valid || std::memcmp(&hostScissor, &bound.scissor, sizeof(hostScissor)) != 0) {
            context->RSSetScissorRects(1, &hostScissor);
            bound.scissor = hostScissor;
        }
        auto* raster = rasterState(reg(reg::PA_SU_SC_MODE_CNTL), (reg(reg::PA_CL_CLIP_CNTL) >> 16) & 1);
        if (!bound.valid || raster != bound.raster) {
            context->RSSetState(raster);
            bound.raster = raster;
        }
        const uint32_t blendControl[4] = {reg(reg::RB_BLENDCONTROL0), reg(reg::RB_BLENDCONTROL1),
                                          reg(reg::RB_BLENDCONTROL2), reg(reg::RB_BLENDCONTROL3)};
        const float blendColor[4] = {asFloat(reg(reg::RB_BLEND_RED)), asFloat(reg(reg::RB_BLEND_RED + 1)),
                                     asFloat(reg(reg::RB_BLEND_RED + 2)), asFloat(reg(reg::RB_BLEND_RED + 3))};
        auto* blend = blendState(blendControl, colorMask);
        if (!bound.valid || blend != bound.blend || std::memcmp(blendColor, bound.blendColor, sizeof(blendColor)) != 0) {
            context->OMSetBlendState(blend, blendColor, 0xFFFFFFFF);
            bound.blend = blend;
            std::memcpy(bound.blendColor, blendColor, sizeof(blendColor));
        }
        const uint32_t stencil = reg(reg::RB_STENCILREFMASK);
        static const bool noDepth = traceEnabled("nodepth");
        // TENCHU_NODEPTH_PS=<hash>: depth test off for one pixel shader (debugging).
        static const uint64_t noDepthShader = [] {
            char value[32]{};
            return GetEnvironmentVariableA("TENCHU_NODEPTH_PS", value, sizeof(value)) ? strtoull(value, nullptr, 16) : 0ull;
        }();
        const bool skipDepth = noDepth || (noDepthShader && psHash == noDepthShader);
        auto* depthStencil = depthState(skipDepth ? 0 : depthControl, stencil);
        if (!bound.valid || depthStencil != bound.depthStencil || (stencil & 0xFF) != bound.stencilRef) {
            context->OMSetDepthStencilState(depthStencil, stencil & 0xFF);
            bound.depthStencil = depthStencil;
            bound.stencilRef = stencil & 0xFF;
        }
        if (!bound.valid || topology != bound.topology) {
            context->IASetPrimitiveTopology(topology);
            bound.topology = topology;
        }
        if (!bound.valid || vs->vertex != bound.vertex) {
            context->VSSetShader(vs->vertex, nullptr, 0);
            bound.vertex = vs->vertex;
        }
        if (!bound.texturesValid) {
            ID3D11ShaderResourceView* vertexData[2] = {ringView, mirrorView};
            context->VSSetShaderResources(0, mirrorView ? 2 : 1, vertexData);
        }
        if (vertexViewCount &&
            (std::memcmp(vertexViews, bound.vertexViews, vertexViewCount * sizeof(vertexViews[0])) != 0 ||
             std::memcmp(vertexSamplers, bound.vertexSamplers, vertexViewCount * sizeof(vertexSamplers[0])) != 0)) {
            context->VSSetShaderResources(8, vertexViewCount, vertexViews);
            context->VSSetSamplers(0, vertexViewCount, vertexSamplers);
            std::memcpy(bound.vertexViews, vertexViews, sizeof(vertexViews));
            std::memcpy(bound.vertexSamplers, vertexSamplers, sizeof(vertexSamplers));
        }
        ID3D11GeometryShader* geometry = rectangles ? rectangleShader : nullptr;
        if (!bound.valid || geometry != bound.geometry) {
            context->GSSetShader(geometry, nullptr, 0);
            bound.geometry = geometry;
        }
        if (ps) {
            // TENCHU_FLAT_PRIM=<type> limits the magenta override to one primitive type.
            static const int flatPrimitive = [] {
                char value[16]{};
                return GetEnvironmentVariableA("TENCHU_FLAT_PRIM", value, sizeof(value)) ? atoi(value) : -1;
            }();
            static const bool flatAll = traceEnabled("flat");
            const bool flat = flatAll || int(primitive) == flatPrimitive;
            ID3D11PixelShader* pixel = flat ? flatShader : ps->pixel;
            if (!bound.valid || pixel != bound.pixel) {
                context->PSSetShader(pixel, nullptr, 0);
                bound.pixel = pixel;
            }
            if (pixelViewCount &&
                (std::memcmp(pixelViews, bound.pixelViews, pixelViewCount * sizeof(pixelViews[0])) != 0 ||
                 std::memcmp(pixelSamplers, bound.pixelSamplers, pixelViewCount * sizeof(pixelSamplers[0])) != 0)) {
                context->PSSetShaderResources(0, pixelViewCount, pixelViews);
                context->PSSetSamplers(0, pixelViewCount, pixelSamplers);
                std::memcpy(bound.pixelViews, pixelViews, sizeof(pixelViews));
                std::memcpy(bound.pixelSamplers, pixelSamplers, sizeof(pixelSamplers));
            }
        } else if (!bound.valid || bound.pixel) {
            context->PSSetShader(nullptr, nullptr, 0);
            bound.pixel = nullptr;
        }
        bound.texturesValid = true;  // the vertex data views (t0, t1) are bound
        bound.valid = true;
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
        // TENCHU_PROBE_VS=<hash>: on traced frames, print that shader's constants and first vertices.
        static const uint64_t probeShader = [] {
            char value[32]{};
            return GetEnvironmentVariableA("TENCHU_PROBE_VS", value, sizeof(value)) ? strtoull(value, nullptr, 16) : 0ull;
        }();
        if (traceFrame() && probeShader && vsHash == probeShader) {
            for (int c = 0; c < 256; ++c) {
                const float x = asFloat(reg(reg::SHADER_CONSTANT_000_X + c * 4)), y = asFloat(reg(reg::SHADER_CONSTANT_000_X + c * 4 + 1)),
                            z = asFloat(reg(reg::SHADER_CONSTANT_000_X + c * 4 + 2)), w = asFloat(reg(reg::SHADER_CONSTANT_000_X + c * 4 + 3));
                if (x || y || z || w) logf("[probe]   c%d = %g %g %g %g", c, x, y, z, w);
            }
            for (int b = 0; b < 8; ++b) logf("[probe]   bool %d = %08X", b, reg(reg::SHADER_CONSTANT_BOOL_000_031 + b));
            if (!bindings.empty()) {
                const uint32_t base = reg::SHADER_CONSTANT_FETCH_00_0 + bindings[0] * 2;
                const uint32_t address = reg(base) & ~3u, endian = reg(base + 1) & 3;
                // TENCHU_PROBE_VERTICES=<n>: first n vertices of binding 0, in its own stride.
                static const uint32_t probeCount = [] {
                    char value[16]{};
                    return GetEnvironmentVariableA("TENCHU_PROBE_VERTICES", value, sizeof(value)) ? uint32_t(atoi(value)) : 2u;
                }();
                const uint32_t stride = vs->info.vertexStreams.empty() ? 10 : (std::max)(1u, vs->info.vertexStreams[0].maxStride);
                for (uint32_t v = 0; v < probeCount; ++v) {
                    std::string words;
                    char cell[16];
                    for (uint32_t k = 0; k < stride && k < 16; ++k) {
                        uint32_t raw;
                        std::memcpy(&raw, memory->host(Memory::fromPhysical(address + (v * stride + k) * 4)), 4);
                        snprintf(cell, sizeof(cell), " %08X", gpuSwap(raw, endian));
                        words += cell;
                    }
                    logf("[probe]   vertex %u:%s", v, words.c_str());
                }
            }
        }
        ID3D11Query* statistics = nullptr;
        if (traceFrame()) {
            const D3D11_QUERY_DESC query{D3D11_QUERY_PIPELINE_STATISTICS, 0};
            device->CreateQuery(&query, &statistics);
            context->Begin(statistics);
        }
        const auto drawStart = stamp();
        if (reg(reg::PA_SC_WINDOW_OFFSET)) ++tileDraws[1]; else ++tileDraws[0];
        if (flickerLog) {
            char line[512];
            const uint32_t fetchBase = reg::SHADER_CONSTANT_FETCH_00_0 + (vs->info.vertexFetchConstants.empty() ? 0 : vs->info.vertexFetchConstants[0] * 2);
            snprintf(line, sizeof(line),
                     "prim %u n %u vs %016llX ps %016llX rt %08X wo %08X sc %ld,%ld-%ld,%ld scale %.1f %.1f off %.1f %.1f vb %08X/%u ib %08X mode %X "
                     "vc %016llX pc %016llX vd %016llX",
                     primitive, count, (unsigned long long)vsHash, (unsigned long long)psHash, reg(reg::RB_COLOR_INFO),
                     reg(reg::PA_SC_WINDOW_OFFSET), scissor.left, scissor.top, scissor.right, scissor.bottom, scale[0],
                     scale[1], offset[0], offset[1], reg(fetchBase) & ~3u, ((reg(fetchBase + 1) >> 2) & 0xFFFFFF) * 4,
                     request.indexBase, reg(reg::PA_SU_SC_MODE_CNTL), (unsigned long long)hashFloats(lastVertexConstants),
                     (unsigned long long)hashFloats(lastPixelConstants),
                     (unsigned long long)sampleChecksum(memory->host(Memory::fromPhysical(reg(fetchBase) & ~3u)),
                                                        (std::min)(4096u, (std::max)(16u, ((reg(fetchBase + 1) >> 2) & 0xFFFFFF) * 4))));
            frameLog.emplace_back(line);
        }
        if (vs->streamOut && traceFrame()) {
            // Capture the first vertices' outputs (debug), then draw normally.
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = 32 * 64;
            desc.BindFlags = D3D11_BIND_STREAM_OUTPUT;
            ID3D11Buffer* target = nullptr;
            device->CreateBuffer(&desc, nullptr, &target);
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Buffer* staging = nullptr;
            device->CreateBuffer(&desc, nullptr, &staging);
            const UINT zero = 0;
            ID3D11GeometryShader* previous = nullptr;
            context->GSGetShader(&previous, nullptr, nullptr);
            context->GSSetShader(vs->streamOut, nullptr, 0);
            context->SOSetTargets(1, &target, &zero);
            const D3D11_PRIMITIVE_TOPOLOGY topologyBefore = topology;
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            context->Draw((std::min)(UINT(vertexCount), 64u), 0);
            ID3D11Buffer* none = nullptr;
            context->SOSetTargets(1, &none, &zero);
            context->GSSetShader(previous, nullptr, 0);
            release(previous);
            context->IASetPrimitiveTopology(topologyBefore);
            forgetBoundState();
            context->CopyResource(staging, target);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                const float* v = static_cast<const float*>(mapped.pData);
                for (int i = 0; i < 6; ++i)
                    logf("[probe]   out %d: pos %g %g %g %g  t0 %g %g %g %g", i, v[i * 8], v[i * 8 + 1], v[i * 8 + 2],
                         v[i * 8 + 3], v[i * 8 + 4], v[i * 8 + 5], v[i * 8 + 6], v[i * 8 + 7]);
                context->Unmap(staging, 0);
            }
            release(staging);
            release(target);
        }
        context->Draw(UINT(vertexCount), 0);
        if (psHash == kSceneCompositeShader && renderer.hdrOutput()) copySceneOnly(*firstTarget, hostScissor);
        addTime(hostTime[5], drawStart);
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
    uint32_t tileDraws[2] = {}, resolves = 0;  // per frame (swap trace)
    const bool flickerLog = traceEnabled("flickerlog");
    const bool tracePostFx = traceEnabled("postfx");
    uint64_t resolveGeneration = 0;  // counts every resolve (post effects reuse results within one)
    std::vector<std::string> frameLog, previousFrameLog;  // flickerlog: this and the last good frame
    // Draws skipped since the last swap: mode, count, shader, indices, target, upload, primitive.
    uint64_t dropped[7] = {};
    // TENCHU_TRACE_FRAME=<n> logs every draw of frame n.
    bool traceFrame() const {
        static const uint64_t frame = [] {
            char value[32]{};
            return GetEnvironmentVariableA("TENCHU_TRACE_FRAME", value, sizeof(value)) ? strtoull(value, nullptr, 10) : 0ull;
        }();
        return (frame && frames == frame) || frames == liveTraceFrame;
    }
    uint64_t liveTraceFrame = ~0ull;  // set by the control file's traceframe command
    // Resolved textures are saved to build/resolved on traced frames with the dumpresolved
    // category, or on the frame after a dumpframe control command (without tracing).
    bool dumpFrame() const { return (traceFrame() && traceEnabled("dumpresolved")) || frames == liveDumpFrame; }
    uint64_t liveDumpFrame = ~0ull;

    // ---- Resolve -------------------------------------------------------------------
    void resolve() {
        forgetBoundState();  // resolves draw with their own pipeline state
        ScopedTime timed{hostTime[2]};
        ++resolves;
        ++resolveGeneration;
        if (flickerLog) {
            char line[160];
            snprintf(line, sizeof(line), "resolve control %08X dest %08X pitch %08X wo %08X", reg(reg::RB_COPY_CONTROL),
                     reg(reg::RB_COPY_DEST_BASE), reg(reg::RB_COPY_DEST_PITCH), reg(reg::PA_SC_WINDOW_OFFSET));
            frameLog.emplace_back(line);
        }
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
        // Pixels whose centers lie inside the rectangle (vertices sit on pixel edges, -0.5..639.5).
        auto edge = [](float value) { return LONG(std::floor(value + 0.5f)); };
        D3D11_RECT rect{edge(minX) + offsetX, edge(minY) + offsetY, edge(maxX) + offsetX, edge(maxY) + offsetY};
        rect.left = (std::max)(0L, rect.left);
        rect.top = (std::max)(0L, rect.top);

        RenderTarget* source = nullptr;
        if (sourceSelect < 4) {
            const uint32_t infoRegisters[4] = {reg::RB_COLOR_INFO, reg::RB_COLOR1_INFO, reg::RB_COLOR2_INFO,
                                               reg::RB_COLOR3_INFO};
            const uint32_t info = reg(infoRegisters[sourceSelect]);
            source = renderTarget(info & 0xFFF, pitch, (info >> 16) & 0xF, samples, false);
        } else if (sourceSelect == 4) {
            const uint32_t info = reg(reg::RB_DEPTH_INFO);
            source = renderTarget(info & 0xFFF, pitch, (info >> 16) & 1, samples, true);
        }
        const bool depthSource = sourceSelect == 4;
        const DXGI_FORMAT destFormat = !source ? DXGI_FORMAT_UNKNOWN : depthSource ? DXGI_FORMAT_R32_FLOAT : source->format;
        TZ_TRACE("resolve", "[d3d11] resolve src %u verts %.1f,%.1f-%.1f,%.1f wo %08X mode %08X surf %08X dinfo %08X",
                 sourceSelect, minX, minY, maxX, maxY, windowOffset, reg(reg::PA_SU_SC_MODE_CNTL), surfaceInfo,
                 reg(reg::RB_DEPTH_INFO));
        TZ_TRACE("resolve", "[d3d11] resolve src %u rect %ld,%ld-%ld,%ld -> %08X pitch %08X info %08X cmd %u clear %d%d",
                 sourceSelect, rect.left, rect.top, rect.right, rect.bottom, reg(reg::RB_COPY_DEST_BASE),
                 reg(reg::RB_COPY_DEST_PITCH), reg(reg::RB_COPY_DEST_INFO), command, clearColor, clearDepth);
        if (traceFrame())
            logf("[draw] resolve src %u rect %ld,%ld-%ld,%ld -> %08X cmd %u clear %d%d", sourceSelect, rect.left, rect.top,
                 rect.right, rect.bottom, reg(reg::RB_COPY_DEST_BASE), command, clearColor, clearDepth);

        if (source && command != 3 && rect.right > rect.left && rect.bottom > rect.top) {
            rect.right = (std::min)(LONG(source->width), rect.right);
            rect.bottom = (std::min)(LONG(source->height), rect.bottom);
            const uint32_t destination = reg(reg::RB_COPY_DEST_BASE) & 0x1FFFFFFF;
            const uint32_t destPitch = reg(reg::RB_COPY_DEST_PITCH);
            const UINT destWidth = (std::max)(1u, destPitch & 0x3FFF), destHeight = (std::max)(1u, (destPitch >> 16) & 0x3FFF);
            uint32_t base = destination;
            UINT destX = 0, destY = 0;
            ResolvedTexture* target = containingResolved(destination, base, destX, destY);
            if (target && base != destination) {
                // A resolve into part of a larger texture (a later tile, or the lower half of
                // a reflection map resolved with its own smaller destination height): keep
                // writing into the containing texture when the layout matches and it fits.
                const bool fits = target->width == destWidth && target->format == destFormat &&
                                  destY + destHeight <= target->height;
                if (!fits) {
                    target = nullptr;
                    destX = destY = 0;
                    base = destination;
                }
            } else if (target && (target->width != destWidth || target->height != destHeight || target->format != destFormat)) {
                // A different texture now lives here.
                destX = destY = 0;
            }
            if (!target || base == destination) {
                target = resolvedTexture(destination, destWidth, destHeight, destFormat);
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
                ID3D11ShaderResourceView* sourceView = depthSource ? source->depthView : source->view;
                ID3D11PixelShader* copyShader = depthSource ? depthCopyShaders[source->samples > 1] : copyPixelShader;
                if (source->samples > 1 && !depthSource) {
                    if (!source->resolveScratch) return;
                    context->ResolveSubresource(source->resolveScratch, 0, source->texture, 0, source->format);
                    single = source->resolveScratch;
                    sourceView = source->resolveScratchView;
                }
                if (!sourceView || !copyShader) return;
                const bool swap = !depthSource && ((reg(reg::RB_COPY_DEST_INFO) >> 24) & 1);
                const bool float24 = depthSource && ((reg(reg::RB_DEPTH_INFO) >> 16) & 1);
                const int32_t constants[8] = {int32_t(scaled(rect.left)), int32_t(scaled(rect.top)),
                                              int32_t(scaled(UINT(destX))), int32_t(scaled(UINT(destY))),
                                              swap || float24 ? 1 : 0, 0, 0, 0};
                writeBuffer(copyConstants, constants, sizeof(constants));
                context->OMSetRenderTargets(1, &target->target, nullptr);
                const D3D11_VIEWPORT viewport{float(scaled(UINT(destX))), float(scaled(UINT(destY))),
                                              float(scaled(rect.right) - scaled(rect.left)),
                                              float(scaled(rect.bottom) - scaled(rect.top)), 0, 1};
                context->RSSetViewports(1, &viewport);
                context->RSSetState(nullptr);
                context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
                context->OMSetDepthStencilState(nullptr, 0);
                context->IASetInputLayout(nullptr);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(copyVertexShader, nullptr, 0);
                context->GSSetShader(nullptr, nullptr, 0);
                context->PSSetShader(copyShader, nullptr, 0);
                context->PSSetShaderResources(0, 1, &sourceView);
                context->PSSetConstantBuffers(0, 1, &copyConstants);
                context->Draw(3, 0);
                ID3D11ShaderResourceView* none = nullptr;
                context->PSSetShaderResources(0, 1, &none);
                target->swapped = swap;
                if (dumpFrame() && single) {
                    CreateDirectoryA("build/resolved", nullptr);
                    char path[96];
                    snprintf(path, sizeof(path), "build/resolved/source_%08X.bmp", destination);
                    saveTexture(single, path);
                }
            }
        }
        RenderTarget* colorTarget = source;
        if (depthSource) {
            const uint32_t info = reg(reg::RB_COLOR_INFO);
            colorTarget = renderTarget(info & 0xFFF, pitch, (info >> 16) & 0xF, samples, false);
        }
        if (clearColor && colorTarget) {
            // RB_COLOR_CLEAR holds a packed ARGB color for 32-bit targets.
            const uint32_t value = reg(reg::RB_COLOR_CLEAR);
            const float color[4] = {((value >> 16) & 0xFF) / 255.0f, ((value >> 8) & 0xFF) / 255.0f,
                                    (value & 0xFF) / 255.0f, (value >> 24) / 255.0f};
            ID3D11DeviceContext1* context1 = nullptr;
            if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1)))) {
                const D3D11_RECT hostRect = scaled(rect);
                context1->ClearView(colorTarget->color, color, &hostRect, 1);
                release(context1);
            } else {
                context->ClearRenderTargetView(colorTarget->color, color);
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

    // ---- HDR: the scene before the HUD -------------------------------------------------
    // A copy of the composite's target taken before the HUD is drawn over it: the present
    // expands the highlights only where the front buffer still matches it, so the HUD,
    // menus and movies (no composite) stay at paper white.
    ID3D11Texture2D* sceneOnly = nullptr;
    ID3D11ShaderResourceView* sceneOnlyView = nullptr;
    uint64_t sceneOnlyFrame = ~0ull;

    void copySceneOnly(const RenderTarget& target, const D3D11_RECT& scissor) {
        // The front buffer is resolved from the target's top left corner.
        const UINT width = (std::min)(UINT(scissor.right), scaled(UINT(target.width)));
        const UINT height = (std::min)(UINT(scissor.bottom), scaled(UINT(target.height)));
        if (scissor.left || scissor.top || target.samples != 1 || !width || !height) return;
        D3D11_TEXTURE2D_DESC desc{};
        if (sceneOnly) sceneOnly->GetDesc(&desc);
        if (sceneOnly && (desc.Width != width || desc.Height != height || desc.Format != target.format)) {
            release(sceneOnlyView);
            release(sceneOnly);
        }
        if (!sceneOnly) {
            desc = {};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = target.format;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &sceneOnly))) return;
            device->CreateShaderResourceView(sceneOnly, nullptr, &sceneOnlyView);
        }
        const D3D11_BOX box{0, 0, 0, width, height, 1};
        context->CopySubresourceRegion(sceneOnly, 0, 0, 0, 0, target.texture, 0, &box);
        sceneOnlyFrame = frames;
    }

    // ---- Swap ---------------------------------------------------------------------
    void swap(uint32_t frontBuffer) {
        forgetBoundState();  // present and dumps use the context too
        // TENCHU_PROBE_RANGE=<first page hex>,<last page hex>: log the writers of those pages, every frame.
        static const std::pair<uint32_t, uint32_t> probeRange = [] {
            char value[64]{};
            if (!GetEnvironmentVariableA("TENCHU_PROBE_RANGE", value, sizeof(value))) return std::pair<uint32_t, uint32_t>{0, 0};
            uint32_t first = 0, last = 0;
            sscanf_s(value, "%x,%x", &first, &last);
            return std::pair<uint32_t, uint32_t>{first, last};
        }();
        if (probeRange.second > probeRange.first)
            memory->probeWrites(probeRange.first, probeRange.second - probeRange.first + 1);
        auto it = resolved.find(frontBuffer & 0x1FFFFFFF);
        if (it == resolved.end()) it = resolved.find(Memory::physicalAddress(frontBuffer));
        if (dumpFrame()) {
            CreateDirectoryA("build/resolved", nullptr);
            for (const auto& [address, entry] : resolved) {
                char path[96];
                snprintf(path, sizeof(path), "build/resolved/%08X_%ux%u.bmp", address, entry.width, entry.height);
                if (entry.texture) saveTexture(entry.texture, path);
            }
        }
        // The scene before the HUD, when this frame had a composite of the front buffer's size.
        ID3D11ShaderResourceView* scene = nullptr;
        if (it != resolved.end() && sceneOnly && sceneOnlyFrame == frames) {
            D3D11_TEXTURE2D_DESC desc{};
            sceneOnly->GetDesc(&desc);
            if (desc.Width == scaled(UINT(it->second.width)) && desc.Height == scaled(UINT(it->second.height)))
                scene = sceneOnlyView;
        }
        const auto presentStart = stamp();
        renderer.present(it != resolved.end() ? it->second.view : nullptr,
                         it != resolved.end() && it->second.swapped, scene);
        TZ_TRACE("swap", "[d3d11] swap %llu %08X (%s), tiles %u/%u, resolves %u, %llu draws, dropped %llu/%llu/%llu/%llu/%llu/%llu/%llu",
                 (unsigned long long)frames, frontBuffer, it != resolved.end() ? "resolved" : "missing", tileDraws[0],
                 tileDraws[1], resolves, (unsigned long long)draws,
                 (unsigned long long)dropped[0], (unsigned long long)dropped[1], (unsigned long long)dropped[2],
                 (unsigned long long)dropped[3], (unsigned long long)dropped[4], (unsigned long long)dropped[5],
                 (unsigned long long)dropped[6]);
        addTime(hostTime[3], presentStart);
        if (traceEnabled("hosttime")) {
            auto ms = [](std::chrono::steady_clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
            logf("[hosttime] draws %.1f ms (textures %.1f, constants %.1f, Draw %.1f), resolves %.1f, present %.1f, "
                 "uploads %.1f MB, mirrored %.1f MB, buffer writes %u/%u/%u, draws %llu", ms(hostTime[1]), ms(hostTime[0]),
                 ms(hostTime[4]), ms(hostTime[5]), ms(hostTime[2]), ms(hostTime[3]), uploadedBytes / 1048576.0,
                 mirroredBytes / 1048576.0, bufferWrites[0], bufferWrites[1], bufferWrites[2], (unsigned long long)draws);
            std::fill(std::begin(bufferWrites), std::end(bufferWrites), 0u);
        }
        if (traceEnabled("vbstats") && !vertexStats.empty()) {
            uint64_t unique = 0;
            for (const auto& [key, count] : vertexStats) unique += key.second;
            logf("[vbstats] %zu distinct vertex ranges, %.1f MB distinct", vertexStats.size(), unique / 1048576.0);
            std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> top;
            for (const auto& [key, count] : vertexStats) top.push_back({count, key});
            std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return uint64_t(a.first) * a.second.second > uint64_t(b.first) * b.second.second; });
            for (size_t i = 0; i < top.size() && i < 6; ++i)
                logf("[vbstats]   %08X size %u x%u", top[i].second.first, top[i].second.second, top[i].first);
            vertexStats.clear();
        }
        for (auto& time : hostTime) time = {};
        uploadedBytes = 0;
        mirroredBytes = 0;
        if (flickerLog) {
            if (renderer.takeSuspicious()) {
                static int dumps = 0;
                if (dumps++ < 4) {
                    logf("[flicker] suspicious frame %llu: previous frame then this one", (unsigned long long)frames);
                    for (const auto& line : previousFrameLog) logf("[flicker] prev %s", line.c_str());
                    for (const auto& line : frameLog) logf("[flicker] this %s", line.c_str());
                }
            }
            previousFrameLog.swap(frameLog);
            frameLog.clear();
        }
        draws = 0;
        tileDraws[0] = tileDraws[1] = resolves = 0;
        std::fill(std::begin(dropped), std::end(dropped), 0);
        ++frames;
        updateHotPages();
        postEffects.endFrame();
        if (control::takeFlag("traceframe")) liveTraceFrame = frames;
        if (control::takeFlag("dumpframe")) liveDumpFrame = frames;
        textures.beginFrame();
    }
};

XenosBackend::XenosBackend(D3D11Renderer& renderer, const std::atomic<uint32_t>* registers, ShaderSource shaders)
    : impl_(std::make_unique<Impl>(renderer, registers, std::move(shaders))) {}
XenosBackend::~XenosBackend() = default;
void XenosBackend::draw(const DrawRequest& request, uint64_t vs, uint64_t ps) { impl_->draw(request, vs, ps); }
void XenosBackend::swap(uint32_t frontBuffer) { impl_->swap(frontBuffer); }
void XenosBackend::presentAgain() {
    impl_->forgetBoundState();  // presenting binds its own pipeline state
    impl_->renderer.presentAgain();
}

}  // namespace tz::gpu
