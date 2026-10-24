#include "renderer.h"
#include "host/overlay.h"
#include "kernel/kernel.h"
#include "kernel/control.h"
#include "host/settings.h"
#include <d3dcompiler.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

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
)";

template <typename T> void release(T*& object) {
    if (object) object->Release();
    object = nullptr;
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
    device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &blitVertexShader_);
    device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &blitPixelShader_);
    release(vs);
    release(ps);
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sampler, &linearSampler_);
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

void D3D11Renderer::resizeIfNeeded() {
    RECT client{};
    GetClientRect(window_, &client);
    const UINT width = (std::max)(1L, client.right - client.left), height = (std::max)(1L, client.bottom - client.top);
    if (backBufferView_ && width == width_ && height == height_) return;
    release(backBufferView_);
    swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* backBuffer = nullptr;
    swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    device_->CreateRenderTargetView(backBuffer, nullptr, &backBufferView_);
    release(backBuffer);
    width_ = width;
    height_ = height;
}

void D3D11Renderer::present(ID3D11ShaderResourceView* source, bool swapRedBlue) { present(source, swapRedBlue, false); }

void D3D11Renderer::presentAgain() { present(lastSource_, lastSwapRedBlue_, true); }

void D3D11Renderer::present(ID3D11ShaderResourceView* source, bool swapRedBlue, bool repeat) {
    resizeIfNeeded();
    ++frame_;
    if (!repeat) {
        if (source) source->AddRef();
        if (lastSource_) lastSource_->Release();
        lastSource_ = source;
        lastSwapRedBlue_ = swapRedBlue;
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
        context_->ClearRenderTargetView(backBufferView_, black);
        const D3D11_VIEWPORT viewport{(width_ - width) / 2, (height_ - height) / 2, width, height, 0, 1};
        context_->RSSetViewports(1, &viewport);
        context_->OMSetRenderTargets(1, &backBufferView_, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetInputLayout(nullptr);
        context_->VSSetShader(blitVertexShader_, nullptr, 0);
        context_->PSSetShader(swapRedBlue ? blitSwappedPixelShader_ : blitPixelShader_, nullptr, 0);
        context_->PSSetShaderResources(0, 1, &source);
        context_->PSSetSamplers(0, 1, &linearSampler_);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context_->OMSetDepthStencilState(nullptr, 0);
        context_->RSSetState(nullptr);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        context_->PSSetShaderResources(0, 1, &none);
    } else {
        // Nothing resolved yet: a slowly cycling color shows frame pacing.
        const float t = float(frame_) / 60.0f;
        const float color[4] = {0.1f + 0.1f * std::sin(t), 0.1f, 0.1f + 0.1f * std::cos(t), 1.0f};
        context_->ClearRenderTargetView(backBufferView_, color);
    }
    if (!repeat) captureIfRequested();  // game frames only, without the menu
    host::overlay::render(backBufferView_, fps_);
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
    ID3D11Texture2D* backBuffer = nullptr;
    swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
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
        if (FILE* out = _wfopen(target.c_str(), L"wb")) {
            fwrite(&file, sizeof(file), 1, out);
            fwrite(&info, sizeof(info), 1, out);
            for (UINT y = desc.Height; y-- > 0;)
                fwrite(static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, 4, desc.Width, out);
            fclose(out);
        }
        context_->Unmap(staging, 0);
    }
    release(staging);
    release(backBuffer);
}

}  // namespace tz::gpu
