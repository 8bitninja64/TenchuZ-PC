#pragma once
// Direct3D 11 backend for the Xenos command processor.
// All methods run on the command processor thread, which owns the context.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <cstdint>

namespace tz::gpu {

class D3D11Renderer {
public:
    bool initialize(HWND window);
    bool ready() const { return context_ != nullptr; }
    ID3D11Device* device() const { return device_; }
    ID3D11DeviceContext* context() const { return context_; }

    // Shows a frame. `source` is the front buffer contents when available;
    // `swapRedBlue` is set when it was resolved with red and blue exchanged.
    void present(ID3D11ShaderResourceView* source, bool swapRedBlue = false);
    // True when Present's vsync interval holds frames at the frame rate limit.
    bool pacedByVsync() const;

private:
    void resizeIfNeeded();
    uint32_t refreshRate() const;
    uint32_t syncInterval();
    uint32_t refresh_ = 0;
    // TENCHU_SCREENSHOTS=<dir>,<every N frames> saves presented frames as BMP.
    void captureIfRequested();
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain1* swapChain_ = nullptr;
    ID3D11RenderTargetView* backBufferView_ = nullptr;
    ID3D11VertexShader* blitVertexShader_ = nullptr;
    ID3D11PixelShader* blitPixelShader_ = nullptr;
    ID3D11PixelShader* blitSwappedPixelShader_ = nullptr;
    ID3D11SamplerState* linearSampler_ = nullptr;
    HWND window_ = nullptr;
    UINT width_ = 0, height_ = 0;
    uint64_t frame_ = 0;
};

// The window the game renders into, created by the app's UI thread.
void setOutputWindow(HWND window);
HWND outputWindow();

}  // namespace tz::gpu
