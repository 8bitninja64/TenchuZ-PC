#include "renderer.h"
#include "host/overlay.h"
#include "kernel/kernel.h"
#include "kernel/control.h"
#include "host/settings.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace tz::gpu {
namespace {

std::atomic<HWND> window{nullptr};

// Full-screen triangle that samples the front buffer.
const char* kBlitShader = R"(
Texture2D source : register(t0);
SamplerState linearSampler : register(s0);
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output vs(uint id : SV_VertexID) {
    Output o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(Output i) : SV_Target { return float4(source.Sample(linearSampler, i.uv).rgb, 1.0); }
float4 psSwapped(Output i) : SV_Target { return float4(source.Sample(linearSampler, i.uv).bgr, 1.0); }

Texture2D sceneOnly : register(t1);
cbuffer Blit : register(b0) {
    uint hasScene;        // HDR: sceneOnly holds the image before the HUD
    uint swapRedBlue;     // the front buffer resolve swapped red and blue
    uint filter;          // 0 bilinear, 1 Catmull-Rom (upscaling), 2 box (downscaling)
    uint taps;            // box: samples per axis
    float2 sourceSize;    // texels
    float2 footprint;     // source texels per window pixel
};
// Catmull-Rom (bicubic) from 9 bilinear samples: the 4x4 kernel's two middle weights per
// axis merge into one sample between them (after Phill Djonov and Matt Pettineo). Sharper
// than bilinear when the window is larger than the image.
float3 CatmullRom(float2 uv) {
    float2 position = uv * sourceSize, center = floor(position - 0.5) + 0.5, f = position - center;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t0 = (center - 1.0) / sourceSize, t12 = (center + w2 / w12) / sourceSize, t3 = (center + 2.0) / sourceSize;
    float3 row0 = source.SampleLevel(linearSampler, float2(t0.x, t0.y), 0).rgb * w0.x +
                  source.SampleLevel(linearSampler, float2(t12.x, t0.y), 0).rgb * w12.x +
                  source.SampleLevel(linearSampler, float2(t3.x, t0.y), 0).rgb * w3.x;
    float3 row12 = source.SampleLevel(linearSampler, float2(t0.x, t12.y), 0).rgb * w0.x +
                   source.SampleLevel(linearSampler, float2(t12.x, t12.y), 0).rgb * w12.x +
                   source.SampleLevel(linearSampler, float2(t3.x, t12.y), 0).rgb * w3.x;
    float3 row3 = source.SampleLevel(linearSampler, float2(t0.x, t3.y), 0).rgb * w0.x +
                  source.SampleLevel(linearSampler, float2(t12.x, t3.y), 0).rgb * w12.x +
                  source.SampleLevel(linearSampler, float2(t3.x, t3.y), 0).rgb * w3.x;
    return saturate(row0 * w0.y + row12 * w12.y + row3 * w3.y);  // the negative lobes overshoot at edges
}
// Box filter: the mean of taps x taps bilinear samples spread over the window pixel's
// footprint. Internal resolutions above the window size stop shimmering.
float3 Box(float2 uv) {
    float2 step = footprint / (sourceSize * taps), start = uv - step * (taps - 1) * 0.5;
    float3 sum = 0.0;
    for (uint y = 0; y < taps; ++y)
        for (uint x = 0; x < taps; ++x) sum += source.SampleLevel(linearSampler, start + step * float2(x, y), 0).rgb;
    return sum / (taps * taps);
}
float3 Scaled(float2 uv) {
    float3 color;
    if (filter == 1) color = CatmullRom(uv);
    else if (filter == 2) color = Box(uv);
    else color = source.SampleLevel(linearSampler, uv, 0).rgb;
    return swapRedBlue ? color.bgr : color;
}
float4 psScaled(Output i) : SV_Target { return float4(Scaled(i.uv), 1.0); }

// HDR: alpha marks the 3D scene (1: highlights are expanded) apart from the HUD, menus and
// movies (0: they stay at paper white). Scene pixels still match the copy of the target
// taken right after the scene composite, before the HUD was drawn over it (both compared
// with the same bilinear samples, whatever the scaling filter).
float4 psMasked(Output i) : SV_Target {
    float scene = 0.0;
    if (hasScene) {
        float3 shown = source.SampleLevel(linearSampler, i.uv, 0).rgb;
        if (swapRedBlue) shown = shown.bgr;  // the front buffer resolve may swap red and blue, the copy does not
        scene = all(abs(shown - sceneOnly.SampleLevel(linearSampler, i.uv, 0).rgb) <= 2.0 / 255.0) ? 1.0 : 0.0;
    }
    return float4(Scaled(i.uv), scene);
}
)";

// SDR game image + menu -> HDR10. The game's output is display-referred SDR (gamma 2.2):
// mid tones keep their brightness at paper white, highlights of the 3D scene (the game
// image's alpha) are expanded toward the peak (by luminance, so hues stay), then Rec.709 ->
// Rec.2020 and the PQ curve, dithered to 10 bits. The menu is composited in linear light at
// paper white.
const char* kHdrShader = R"(
Texture2D game : register(t0);
Texture2D ui : register(t1);
cbuffer Hdr : register(b0) { float paperWhite; float peak; float expansion; float highlights; };
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
static const float3x3 k709To2020 = {0.6274040, 0.3292820, 0.0433136,
                                    0.0690970, 0.9195400, 0.0113612,
                                    0.0163916, 0.0880132, 0.8955950};
float3 PQ(float3 nits) {
    float3 y = pow(saturate(nits / 10000.0), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}
// Interleaved gradient noise (Jimenez): a cheap, even dither pattern.
float IGN(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }
float4 ps(Output i) : SV_Target {
    int3 p = int3(i.position.xy, 0);
    float4 g = game.Load(p);
    float3 color = pow(saturate(g.rgb), 2.2);
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    float boost = 1.0 + (peak / paperWhite - 1.0) * pow(luminance, expansion) * highlights * g.a;
    float3 nits = min(color * paperWhite * boost, peak);
    float4 menu = ui.Load(p);
    if (menu.a > 0.0) nits = nits * (1.0 - menu.a) + pow(saturate(menu.rgb / menu.a), 2.2) * menu.a * paperWhite;
    return float4(PQ(mul(k709To2020, nits)) + (IGN(i.position.xy) - 0.5) / 1023.0, 1.0);
}
)";

template <typename T> void release(T*& object) {
    if (object) object->Release();
    object = nullptr;
}

// Layout of cbuffer Blit in kBlitShader.
struct BlitConstants {
    uint32_t hasScene, swapRedBlue, filter, taps;
    float sourceSize[2], footprint[2];
};

// Size of the texture behind a shader resource view.
void viewSize(ID3D11ShaderResourceView* view, UINT& width, UINT& height) {
    ID3D11Resource* resource = nullptr;
    ID3D11Texture2D* texture = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    view->GetResource(&resource);
    if (resource && SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))))
        texture->GetDesc(&desc);
    release(texture);
    release(resource);
    width = desc.Width;
    height = desc.Height;
}

void writeBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const void* data, size_t size) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, data, size);
    context->Unmap(buffer, 0);
}

ID3DBlob* compile(const char* source, const char* entry, const char* profile) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(D3DCompile(source, strlen(source), "blit", nullptr, nullptr, entry, profile, 0, 0, &code, &errors))) {
        logf("[d3d11] shader compile failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        release(errors);
        return nullptr;
    }
    release(errors);
    return code;
}

// Windows' "SDR content brightness" for a monitor in HDR mode, in nits (0 if unknown: the
// callers keep their previous value, 200 at first).
float windowsSdrWhiteNits(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return 0.0f;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return 0.0f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return 0.0f;
    paths.resize(pathCount);
    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), path.sourceInfo.adapterId,
                         path.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            wcscmp(source.viewGdiDeviceName, info.szDevice) != 0)
            continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(white), path.targetInfo.adapterId,
                        path.targetInfo.id};
        // SDRWhiteLevel is in thousandths of 80 nits (1000 = 80 nits).
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel)
            return float(white.SDRWhiteLevel) / 1000.0f * 80.0f;
    }
    return 0.0f;
}

// Windows' SDR brightness slider does not make the DXGI factory stale, so its value is
// polled while HDR output uses it, on a thread of its own: QueryDisplayConfig can take
// milliseconds, as long as a frame at high rates.
std::atomic<HMONITOR> sdrWhiteMonitor{nullptr};  // the monitor to poll; nullptr: none
std::mutex sdrWhiteMutex;
HMONITOR sdrWhitePolledMonitor = nullptr;  // the last result and its monitor
float sdrWhitePolled = 0.0f;

void pollSdrWhite(HMONITOR monitor) {
    sdrWhiteMonitor = monitor;
    if (!monitor) return;
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            for (;;) {
                if (const HMONITOR polled = sdrWhiteMonitor.load()) {
                    const float nits = windowsSdrWhiteNits(polled);
                    if (nits <= 0.0f) continue;  // a failed query (display changing): keep the last
                    std::lock_guard lock(sdrWhiteMutex);
                    sdrWhitePolledMonitor = polled;
                    sdrWhitePolled = nits;
                }
                Sleep(2000);
            }
        }).detach();
    });
}

// The last polled SDR white level of a monitor, or 0.
float polledSdrWhite(HMONITOR monitor) {
    std::lock_guard lock(sdrWhiteMutex);
    return sdrWhitePolledMonitor == monitor ? sdrWhitePolled : 0.0f;
}

}  // namespace

void setOutputWindow(HWND value) { window = value; }
HWND outputWindow() { return window; }

bool D3D11Renderer::initialize(HWND target) {
    window_ = target;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (traceEnabled("d3ddebug")) flags |= D3D11_CREATE_DEVICE_DEBUG;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    // Prefer the discrete GPU on hybrid laptops.
    IDXGIAdapter1* preferred = nullptr;
    IDXGIFactory6* factory6 = nullptr;
    if (!traceEnabled("defaultadapter") &&
        SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory6)))) {
        factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                             reinterpret_cast<void**>(&preferred));
        release(factory6);
    }
    HRESULT created = D3D11CreateDevice(preferred, preferred ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                        nullptr, flags, levels, 2, D3D11_SDK_VERSION, &device_, nullptr, &context_);
    if (FAILED(created) && preferred)
        created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                    &device_, nullptr, &context_);
    if (preferred) {
        DXGI_ADAPTER_DESC1 description{};
        preferred->GetDesc1(&description);
        char name[128]{};
        WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, sizeof(name), nullptr, nullptr);
        logf("[d3d11] adapter: %s", name);
    }
    release(preferred);
    if (FAILED(created)) {
        logf("[d3d11] device creation failed");
        return false;
    }
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const HRESULT hr = factory->CreateSwapChainForHwnd(device_, window_, &desc, nullptr, nullptr, &swapChain_);
    factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
    release(factory);
    release(adapter);
    release(dxgiDevice);
    if (FAILED(hr)) {
        logf("[d3d11] swap chain creation failed (%08lX)", hr);
        return false;
    }
    host::overlay::initialize(window_, device_, context_);
    ID3DBlob* vs = compile(kBlitShader, "vs", "vs_5_0");
    ID3DBlob* ps = compile(kBlitShader, "ps", "ps_5_0");
    ID3DBlob* swapped = compile(kBlitShader, "psSwapped", "ps_5_0");
    if (!vs || !ps || !swapped) return false;
    device_->CreatePixelShader(swapped->GetBufferPointer(), swapped->GetBufferSize(), nullptr, &blitSwappedPixelShader_);
    release(swapped);
    if (ID3DBlob* masked = compile(kBlitShader, "psMasked", "ps_5_0")) {
        device_->CreatePixelShader(masked->GetBufferPointer(), masked->GetBufferSize(), nullptr, &blitMaskedPixelShader_);
        release(masked);
    }
    if (ID3DBlob* scaled = compile(kBlitShader, "psScaled", "ps_5_0")) {
        device_->CreatePixelShader(scaled->GetBufferPointer(), scaled->GetBufferSize(), nullptr, &blitScaledPixelShader_);
        release(scaled);
    }
    device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &blitVertexShader_);
    device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &blitPixelShader_);
    release(vs);
    release(ps);
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sampler, &linearSampler_);
    if (ID3DBlob* hdr = compile(kHdrShader, "ps", "ps_5_0")) {
        device_->CreatePixelShader(hdr->GetBufferPointer(), hdr->GetBufferSize(), nullptr, &hdrPixelShader_);
        release(hdr);
    }
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = 16;
    constants.Usage = D3D11_USAGE_DYNAMIC;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&constants, nullptr, &hdrConstants_);
    constants.ByteWidth = sizeof(BlitConstants);
    device_->CreateBuffer(&constants, nullptr, &blitConstants_);
    logf("[d3d11] renderer ready");
    return true;
}

// Refresh rate of the monitor showing the window (0 if unknown).
uint32_t D3D11Renderer::refreshRate() const {
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) return 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0;
    return mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
}

// Vsync interval for Present. When the refresh rate is a multiple of the frame
// rate limit (60 on 60/120/180/240 Hz), vsync alone paces frames, which locks
// them to the display; otherwise the command processor's timer paces them.
uint32_t D3D11Renderer::syncInterval() {
    if (!host::settings().vsync) return 0;
    const int fps = host::activeFrameRateLimit();
    if (fps <= 0) return 1;
    if (frame_ % 120 == 1) refresh_ = refreshRate();  // follows monitor changes
    if (refresh_ && refresh_ % uint32_t(fps) == 0 && refresh_ / uint32_t(fps) <= 4) return refresh_ / uint32_t(fps);
    return 1;
}

bool D3D11Renderer::pacedByVsync() const {
    const int limit = host::activeFrameRateLimit();
    return host::settings().vsync && limit > 0 && host::settings().frameRateLimit != 30 && refresh_ &&
           refresh_ % uint32_t(limit) == 0 && refresh_ / uint32_t(limit) <= 4;
}

// Whether Windows HDR is on for the monitor showing the window. A factory's outputs keep
// the color space they had when it was created, so a stale factory (IsCurrent is false
// after HDR toggles and display changes) is replaced; otherwise the outputs are enumerated
// again only when the window moved to another monitor.
bool D3D11Renderer::monitorHdr() {
    const HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    const bool current = outputFactory_ && outputFactory_->IsCurrent();
    if (current && monitor == outputMonitor_) return monitorHdr_;
    if (!current) {
        release(outputFactory_);
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&outputFactory_)))) return false;
    }
    outputMonitor_ = monitor;
    monitorHdr_ = false;
    monitorPeakNits_ = 0.0f;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; outputFactory_->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            IDXGIOutput6* output6 = nullptr;
            DXGI_OUTPUT_DESC1 desc{};
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6))) &&
                SUCCEEDED(output6->GetDesc1(&desc)) && desc.Monitor == monitor) {
                monitorHdr_ = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                monitorPeakNits_ = desc.MaxLuminance;
            }
            release(output6);
            release(output);
        }
        release(adapter);
    }
    if (const float nits = monitorHdr_ ? windowsSdrWhiteNits(monitor) : 0.0f; nits > 0.0f) sdrWhiteNits_ = nits;
    return monitorHdr_;
}

void D3D11Renderer::hdrLevels(float& paperWhite, float& peak) const {
    const host::Settings& s = host::settings();
    // EDID peaks below 400 nits are usually wrong (HDR400 is the lowest certification).
    peak = s.hdrPeakNits > 0 ? float(s.hdrPeakNits) : monitorPeakNits_ >= 400.0f ? monitorPeakNits_ : 1000.0f;
    paperWhite = (std::max)(s.hdrPaperWhiteNits > 0 ? float(s.hdrPaperWhiteNits) : sdrWhiteNits_, 80.0f);
    peak = (std::max)(peak, paperWhite);
}

// HDR10 metadata for displays that tone-map by it: SDR content (Rec.709 primaries, D65)
// expanded up to the peak, averaging around paper white.
void D3D11Renderer::updateHdrMetadata(float paperWhite, float peak) {
    if (paperWhite == metadataPaperWhite_ && peak == metadataPeak_) return;
    metadataPaperWhite_ = paperWhite;
    metadataPeak_ = peak;
    IDXGISwapChain4* swapChain4 = nullptr;
    if (FAILED(swapChain_->QueryInterface(__uuidof(IDXGISwapChain4), reinterpret_cast<void**>(&swapChain4)))) return;
    DXGI_HDR_METADATA_HDR10 metadata{};
    // Chromaticity coordinates scaled by 50000.
    const UINT16 primaries[8] = {32000, 16500, 15000, 30000, 7500, 3000, 15635, 16450};
    std::memcpy(metadata.RedPrimary, primaries, sizeof(primaries));  // red, green, blue, white point
    metadata.MaxMasteringLuminance = UINT(peak * 10000.0f);  // both in 0.0001 nits
    metadata.MinMasteringLuminance = 1;
    metadata.MaxContentLightLevel = UINT16(peak);
    metadata.MaxFrameAverageLightLevel = UINT16(paperWhite);
    if (FAILED(swapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(metadata), &metadata)))
        TZ_TRACE("hdr", "[d3d11] HDR10 metadata refused");
    release(swapChain4);
}

// After every present the back buffer is still bound (the blit, the HDR pass and the
// menu leave it there): during play the next frame's draws replace it, but the F1 menu
// re-presents with nothing in between, and DXGI refuses ResizeBuffers while a view of a
// buffer is bound (HDR and fullscreen toggles from the menu failed).
void D3D11Renderer::unbindBackBuffer() {
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* none[2] = {};
    context_->PSSetShaderResources(0, 2, none);
    context_->Flush();  // the flip model releases buffers lazily
}

// Switches the swap chain between SDR and HDR10 when the setting or the monitor changes
// (checked every two seconds; the setting is checked every frame).
void D3D11Renderer::updateHdr() {
    static bool available = false;
    if (frame_ % 120 == 1) {
        static const bool force = traceEnabled("forcehdr");  // the HDR path on any monitor (testing)
        const bool now = monitorHdr() || force;
        if (now != available || frame_ == 1)
            logf("[d3d11] monitor HDR %s (peak %.0f nits, SDR white %.0f nits)", now ? "on" : "off", monitorPeakNits_,
                 sdrWhiteNits_);
        available = now;
        host::setHdrAvailable(available);
        // Follow Windows' SDR brightness slider while automatic paper white uses it.
        const bool followSdrWhite = hdr_ && host::settings().hdrPaperWhiteNits == 0 && monitorHdr_;
        if (const float polled = followSdrWhite ? polledSdrWhite(outputMonitor_) : 0.0f; polled > 0.0f)
            sdrWhiteNits_ = polled;
        pollSdrWhite(followSdrWhite ? outputMonitor_ : nullptr);
        // What "auto" stands for, shown in the F1 menu.
        const float autoPeak = monitorPeakNits_ >= 400.0f ? monitorPeakNits_ : 1000.0f;
        host::setHdrAutoNits(int(autoPeak + 0.5f), int((std::max)(sdrWhiteNits_, 80.0f) + 0.5f));
    }
    const bool want = host::settings().hdr && available && hdrPixelShader_ && blitMaskedPixelShader_;
    if (want == hdr_) return;
    release(backBufferView_);
    releaseHdrTextures();
    unbindBackBuffer();
    const DXGI_FORMAT format = want ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    if (FAILED(swapChain_->ResizeBuffers(0, 0, 0, format, 0))) {
        logf("[d3d11] cannot switch the swap chain to %s", want ? "HDR10" : "SDR");
        return;
    }
    const DXGI_COLOR_SPACE_TYPE space =
        want ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    IDXGISwapChain3* swapChain3 = nullptr;
    UINT support = 0;
    if (SUCCEEDED(swapChain_->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swapChain3))) &&
        SUCCEEDED(swapChain3->CheckColorSpaceSupport(space, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        swapChain3->SetColorSpace1(space);
    release(swapChain3);
    // HDR10 metadata goes with each change of the levels (present); SDR has none.
    metadataPaperWhite_ = metadataPeak_ = -1.0f;
    IDXGISwapChain4* swapChain4 = nullptr;
    if (!want && SUCCEEDED(swapChain_->QueryInterface(__uuidof(IDXGISwapChain4), reinterpret_cast<void**>(&swapChain4))))
        swapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
    release(swapChain4);
    backBufferFormat_ = format;
    hdr_ = want;
    width_ = height_ = 0;  // resizeIfNeeded recreates the views
    logf("[d3d11] output %s", hdr_ ? "HDR10" : "SDR");
}

void D3D11Renderer::releaseHdrTextures() {
    release(sdrResource_);
    release(sdrView_);
    release(sdr_);
    release(uiResource_);
    release(uiView_);
    release(ui_);
}

void D3D11Renderer::createHdrTextures() {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (auto [texture, view, resource] : {std::tuple{&sdr_, &sdrView_, &sdrResource_}, std::tuple{&ui_, &uiView_, &uiResource_}}) {
        device_->CreateTexture2D(&desc, nullptr, texture);
        if (!*texture) continue;
        device_->CreateRenderTargetView(*texture, nullptr, view);
        device_->CreateShaderResourceView(*texture, nullptr, resource);
    }
}

void D3D11Renderer::resizeIfNeeded() {
    RECT client{};
    GetClientRect(window_, &client);
    const UINT width = (std::max)(1L, client.right - client.left), height = (std::max)(1L, client.bottom - client.top);
    if (backBufferView_ && width == width_ && height == height_) return;
    release(backBufferView_);
    releaseHdrTextures();
    unbindBackBuffer();
    if (FAILED(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)))
        logf("[d3d11] cannot resize the swap chain to %ux%u", width, height);
    ID3D11Texture2D* backBuffer = nullptr;
    swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    device_->CreateRenderTargetView(backBuffer, nullptr, &backBufferView_);
    release(backBuffer);
    width_ = width;
    height_ = height;
    if (hdr_) createHdrTextures();
}

void D3D11Renderer::present(ID3D11ShaderResourceView* source, bool swapRedBlue, ID3D11ShaderResourceView* sceneOnly) {
    presentFrame(source, swapRedBlue, sceneOnly, false);
}

void D3D11Renderer::presentAgain() { presentFrame(lastSource_, lastSwapRedBlue_, lastSceneOnly_, true); }

void D3D11Renderer::presentFrame(ID3D11ShaderResourceView* source, bool swapRedBlue, ID3D11ShaderResourceView* sceneOnly,
                                 bool repeat) {
    ++frame_;
    updateHdr();
    resizeIfNeeded();
    const bool hdr = hdr_ && sdrView_ && uiView_;
    ID3D11RenderTargetView* target = hdr ? sdrView_ : backBufferView_;
    if (!repeat) {
        if (source) source->AddRef();
        if (lastSource_) lastSource_->Release();
        lastSource_ = source;
        lastSwapRedBlue_ = swapRedBlue;
        if (sceneOnly) sceneOnly->AddRef();
        if (lastSceneOnly_) lastSceneOnly_->Release();
        lastSceneOnly_ = sceneOnly;
        // Frame rate of new game frames, averaged over half a second.
        static auto windowStart = std::chrono::steady_clock::now();
        static uint32_t frames = 0;
        ++frames;
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - windowStart).count();
        if (seconds >= 0.5) {
            fps_ = float(frames / seconds);
            frames = 0;
            windowStart = std::chrono::steady_clock::now();
        }
    }
    if (source) {
        // Letterbox the 16:9 game image into the window.
        float width = float(width_), height = float(height_);
        if (width / height > 16.0f / 9.0f) width = height * 16.0f / 9.0f;
        else height = width * 9.0f / 16.0f;
        const float black[4] = {0, 0, 0, 1};
        context_->ClearRenderTargetView(target, black);
        const D3D11_VIEWPORT viewport{(width_ - width) / 2, (height_ - height) / 2, width, height, 0, 1};
        context_->RSSetViewports(1, &viewport);
        context_->OMSetRenderTargets(1, &target, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetInputLayout(nullptr);
        context_->VSSetShader(blitVertexShader_, nullptr, 0);
        // Scaling: a box filter when the image is larger than its place in the window (internal
        // resolution above the window size), Catmull-Rom ([display] upscale_filter) when smaller.
        UINT sourceWidth = 0, sourceHeight = 0;
        viewSize(source, sourceWidth, sourceHeight);
        BlitConstants constants{sceneOnly ? 1u : 0u, swapRedBlue ? 1u : 0u, 0, 1,
                                {float(sourceWidth), float(sourceHeight)},
                                {float(sourceWidth) / width, float(sourceHeight) / height}};
        const float footprint = (std::max)(constants.footprint[0], constants.footprint[1]);
        if (footprint > 1.001f) {
            constants.filter = 2;
            constants.taps = std::clamp(uint32_t(std::ceil(footprint - 0.001f)), 2u, 8u);
        } else if (footprint < 0.999f && host::settings().upscaleBicubic) {
            constants.filter = 1;
        }
        if (!blitScaledPixelShader_) constants.filter = 0;
        if (hdr || constants.filter) {
            // The scene mask goes into alpha in HDR mode (kBlitShader psMasked).
            writeBuffer(context_, blitConstants_, &constants, sizeof(constants));
            context_->PSSetShader(hdr ? blitMaskedPixelShader_ : blitScaledPixelShader_, nullptr, 0);
            context_->PSSetConstantBuffers(0, 1, &blitConstants_);
        } else {
            context_->PSSetShader(swapRedBlue ? blitSwappedPixelShader_ : blitPixelShader_, nullptr, 0);
        }
        ID3D11ShaderResourceView* views[2] = {source, hdr ? sceneOnly : nullptr};
        context_->PSSetShaderResources(0, hdr ? 2 : 1, views);
        context_->PSSetSamplers(0, 1, &linearSampler_);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(nullptr, 0);
        context_->RSSetState(nullptr);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* none[2] = {};
        context_->PSSetShaderResources(0, hdr ? 2 : 1, none);
    } else {
        // Nothing resolved yet: a slowly cycling color shows frame pacing.
        const float t = float(frame_) / 60.0f;
        const float color[4] = {0.1f + 0.1f * std::sin(t), 0.1f, 0.1f + 0.1f * std::cos(t), 1.0f};
        context_->ClearRenderTargetView(target, color);
    }
    if (!repeat) captureIfRequested();  // game frames only, without the menu
    if (hdr) {
        const float clear[4] = {0, 0, 0, 0};
        context_->ClearRenderTargetView(uiView_, clear);
        host::overlay::render(uiView_, fps_);
        float paperWhite = 0.0f, peak = 0.0f;
        hdrLevels(paperWhite, peak);
        updateHdrMetadata(paperWhite, peak);
        // Highlights 100% expands by luminance^4; higher percentages reach into the mid tones.
        const float highlights = float(host::settings().hdrHighlights) / 100.0f;
        const float constants[4] = {paperWhite, peak, 4.0f / (std::max)(highlights, 0.01f),
                                    highlights > 0.0f ? 1.0f : 0.0f};
        writeBuffer(context_, hdrConstants_, constants, sizeof(constants));
        const D3D11_VIEWPORT full{0, 0, float(width_), float(height_), 0, 1};
        context_->RSSetViewports(1, &full);
        context_->OMSetRenderTargets(1, &backBufferView_, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetInputLayout(nullptr);
        context_->VSSetShader(blitVertexShader_, nullptr, 0);
        context_->PSSetShader(hdrPixelShader_, nullptr, 0);
        ID3D11ShaderResourceView* views[2] = {sdrResource_, uiResource_};
        context_->PSSetShaderResources(0, 2, views);
        context_->PSSetConstantBuffers(0, 1, &hdrConstants_);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(nullptr, 0);
        context_->RSSetState(nullptr);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* none[2] = {};
        context_->PSSetShaderResources(0, 2, none);
    } else {
        host::overlay::render(backBufferView_, fps_);
    }
    const HRESULT presented = swapChain_->Present(syncInterval(), 0);
    if (frame_ % 300 == 0)
        TZ_TRACE("window", "[window] foreground %d iconic %d present %08X", GetForegroundWindow() == window_,
                 IsIconic(window_) != 0, unsigned(presented));
    if (traceEnabled("present")) {
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        logf("[present] %.2f ms", std::chrono::duration<double, std::milli>(now - last).count());
        last = now;
    }
}

void D3D11Renderer::captureIfRequested() {
    static const auto settings = [] {
        wchar_t value[MAX_PATH]{};
        std::pair<std::filesystem::path, uint64_t> result{{}, 0};
        if (!GetEnvironmentVariableW(L"TENCHU_SCREENSHOTS", value, MAX_PATH)) return result;
        std::wstring text = value;
        const size_t comma = text.find(L',');
        result.first = text.substr(0, comma);
        result.second = comma == std::wstring::npos ? 60 : std::stoull(text.substr(comma + 1));
        std::filesystem::create_directories(result.first);
        return result;
    }();
    std::filesystem::path target;
    if (std::string requested = control::takeScreenshotRequest(); !requested.empty()) {
        target = requested;
    } else if (!settings.first.empty() && settings.second && frame_ % settings.second == 0) {
        char name[64];
        snprintf(name, sizeof(name), "frame_%06llu.bmp", (unsigned long long)frame_);
        target = settings.first / name;
    } else {
        return;
    }
    // In HDR mode the SDR image before the conversion (the back buffer is 10-bit PQ).
    ID3D11Texture2D* backBuffer = nullptr;
    if (hdr_ && sdr_) {
        backBuffer = sdr_;
        backBuffer->AddRef();
    } else {
        swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    }
    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    device_->CreateTexture2D(&desc, nullptr, &staging);
    context_->CopyResource(staging, backBuffer);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        // 32-bit bottom-up BMP.
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{};
        info.biSize = sizeof(info);
        info.biWidth = LONG(desc.Width);
        info.biHeight = LONG(desc.Height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        const uint32_t imageSize = desc.Width * desc.Height * 4;
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + imageSize;
        // Flicker hunting (trace 'flickerlog'): a right half that is almost all
        // black while the left half is not marks a suspicious frame.
        if (traceEnabled("flickerlog") && desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            uint32_t dark[2] = {}, total[2] = {};
            for (UINT y = desc.Height / 8; y < desc.Height * 7 / 8; y += 4)
                for (UINT x = 0; x < desc.Width; x += 4) {
                    const uint8_t* p = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch + x * 4;
                    const int half = x >= desc.Width / 2;
                    ++total[half];
                    if (p[0] + p[1] + p[2] < 9) ++dark[half];
                }
            lastCaptureSuspicious_ = dark[1] * 100 > total[1] * 85 && dark[0] * 100 < total[0] * 60;
        }
        // TENCHU_TRACE=hudmask: in HDR mode, the scene mask (alpha) instead of the image:
        // white for the 3D scene, black for the HUD, menus and movies.
        static const bool hudMask = traceEnabled("hudmask");
        std::vector<uint8_t> mask(hudMask && hdr_ ? desc.Width * 4 : 0);
        if (FILE* out = _wfopen(target.c_str(), L"wb")) {
            fwrite(&file, sizeof(file), 1, out);
            fwrite(&info, sizeof(info), 1, out);
            for (UINT y = desc.Height; y-- > 0;) {
                const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
                if (!mask.empty()) {
                    for (UINT x = 0; x < desc.Width; ++x) {
                        mask[x * 4] = mask[x * 4 + 1] = mask[x * 4 + 2] = row[x * 4 + 3];
                        mask[x * 4 + 3] = 255;
                    }
                    row = mask.data();
                }
                fwrite(row, 4, desc.Width, out);
            }
            fclose(out);
        }
        context_->Unmap(staging, 0);
    }
    release(staging);
    release(backBuffer);
}

}  // namespace tz::gpu
