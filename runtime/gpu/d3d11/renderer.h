#pragma once
// Direct3D 11 backend for the Xenos command processor.
// All methods run on the command processor thread, which owns the context.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <utility>

namespace tz::gpu {

class D3D11Renderer {
public:
    bool initialize(HWND window);
    bool ready() const { return context_ != nullptr; }
    ID3D11Device* device() const { return device_; }
    ID3D11DeviceContext* context() const { return context_; }

    // Shows a frame. `source` is the front buffer contents when available;
    // `swapRedBlue` is set when it was resolved with red and blue exchanged. `sceneOnly`
    // (HDR output only) is the same image before the HUD was drawn, without the swap.
    void present(ID3D11ShaderResourceView* source, bool swapRedBlue = false,
                 ID3D11ShaderResourceView* sceneOnly = nullptr);
    // Shows the last frame again (the F1 menu over a paused game).
    void presentAgain();
    // True while the swap chain is HDR10 (frames then want a sceneOnly image).
    bool hdrOutput() const { return hdr_; }
    // True when Present's vsync interval holds frames at the frame rate limit.
    bool pacedByVsync() const;
    // True when the last captured frame looked like a flicker frame (trace 'flickerlog').
    bool takeSuspicious() { return std::exchange(lastCaptureSuspicious_, false); }

private:
    // HDR10 output (settings hdr, with Windows HDR on for the window's monitor): the game
    // image and the F1 menu are drawn into SDR textures, then expanded into the HDR back
    // buffer (inverse tone map, Rec.2020, PQ). Otherwise frames go to the back buffer as before.
    void updateHdr();
    bool monitorHdr();
    void createHdrTextures();
    void releaseHdrTextures();
    // ResizeBuffers fails while a view of a swap chain buffer is still bound.
    void unbindBackBuffer();
    // Paper white and peak in nits: the settings, or for 0 (auto) the display's values.
    void hdrLevels(float& paperWhite, float& peak) const;
    void updateHdrMetadata(float paperWhite, float peak);
    bool hdr_ = false;  // the swap chain is HDR10 now
    // Output enumeration for monitorHdr, kept until DXGI marks the factory stale or the
    // window moves to another monitor (enumerating every adapter costs milliseconds).
    IDXGIFactory1* outputFactory_ = nullptr;
    HMONITOR outputMonitor_ = nullptr;
    bool monitorHdr_ = false;
    float monitorPeakNits_ = 0.0f;  // DXGI_OUTPUT_DESC1::MaxLuminance (from the EDID)
    float sdrWhiteNits_ = 200.0f;   // Windows "SDR content brightness" for that monitor
    float metadataPaperWhite_ = -1.0f, metadataPeak_ = -1.0f;  // last HDR10 metadata sent
    DXGI_FORMAT backBufferFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    ID3D11Texture2D* sdr_ = nullptr;  // the game image (SDR, window size)
    ID3D11RenderTargetView* sdrView_ = nullptr;
    ID3D11ShaderResourceView* sdrResource_ = nullptr;
    ID3D11Texture2D* ui_ = nullptr;  // the F1 menu and FPS counter (premultiplied alpha)
    ID3D11RenderTargetView* uiView_ = nullptr;
    ID3D11ShaderResourceView* uiResource_ = nullptr;
    ID3D11PixelShader* hdrPixelShader_ = nullptr;
    ID3D11Buffer* hdrConstants_ = nullptr;
    ID3D11PixelShader* blitMaskedPixelShader_ = nullptr;  // the game image with the scene mask in alpha
    ID3D11Buffer* blitConstants_ = nullptr;
    ID3D11ShaderResourceView* lastSceneOnly_ = nullptr;
    void resizeIfNeeded();
    uint32_t refreshRate() const;
    uint32_t syncInterval();
    uint32_t refresh_ = 0;
    bool lastCaptureSuspicious_ = false;
    // TENCHU_SCREENSHOTS=<dir>,<every N frames> saves presented frames as BMP (the SDR image).
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
    ID3D11ShaderResourceView* lastSource_ = nullptr;
    bool lastSwapRedBlue_ = false;
    bool repeating_ = false;
    float fps_ = 0.0f;  // new game frames per second (F1 menu counter)
    void presentFrame(ID3D11ShaderResourceView* source, bool swapRedBlue, ID3D11ShaderResourceView* sceneOnly,
                      bool repeat);
};

// The window the game renders into, created by the app's UI thread.
void setOutputWindow(HWND window);
HWND outputWindow();

}  // namespace tz::gpu
