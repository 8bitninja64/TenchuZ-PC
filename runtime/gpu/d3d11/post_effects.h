#pragma once
// Scene effects the original does not have ("enhanced lighting"), applied where the title
// composites its 3D scene, before the HUD is drawn: ambient occlusion (a simplified GTAO,
// after Intel's XeGTAO, MIT) from the resolved depth, and contrast-adaptive sharpening
// (after AMD FidelityFX CAS, MIT). The composite then reads the processed copy instead of
// the scene resolve; guest memory and every other draw are untouched. Also the steadier
// bloom source for internal resolutions above 720p. Runs on the command processor thread,
// which owns the context.
#include <d3d11.h>
#include <cstdint>

namespace tz::gpu {

// The camera projection recovered from a draw's vertex constants, D3D style (w = view z):
// x_ndc = px * x / z, y_ndc = py * y / z, z_ndc = a + b / z. The viewport then stores
// depth = zScale * z_ndc + zOffset (the title uses reversed Z: -1, +1).
struct Projection {
    float px = 0, py = 0, a = 0, b = 0, zScale = 1, zOffset = 0;
};

class PostEffects {
public:
    void initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    // Ambient occlusion or sharpening is on (settings [graphics]).
    bool enabled() const;
    // Draws that write depth offer their vertex constants (c0-c10 are read) while ambient
    // occlusion is on: the frame's camera projection is the one most of them hold.
    bool wantsProjection() const;
    void noteProjection(const float* constants, float zScale, float zOffset);
    // The composite's scene input with the effects applied (a texture of the same size and
    // format), or nullptr to leave the frame as the title drew it. scene is the resolved
    // RGBA8 scene, depth the resolved R32 depth (20e4 floats as unorm, xenos_backend.cpp
    // Float20e4AsUnorm) of the same size. generation changes whenever guest resolves ran,
    // so a second composite of the same inputs reuses the result.
    ID3D11ShaderResourceView* processScene(ID3D11Texture2D* scene, ID3D11ShaderResourceView* sceneView,
                                           ID3D11Texture2D* depth, ID3D11ShaderResourceView* depthView,
                                           uint64_t generation);
    // Stable bloom at resolution scales above 1: the bloom shrink's 16 taps sample the scaled
    // scene at fixed UV offsets, so each covers 1/scale^2 of its original footprint and small
    // highlights sparkle. It reads this instead: the scene averaged back to its guest size
    // (FP16, so dim lights keep their energy), as The Darkness Recomp's logical bloom view.
    ID3D11ShaderResourceView* bloomSource(ID3D11ShaderResourceView* sceneView, UINT guestWidth, UINT guestHeight,
                                          uint32_t scale, uint64_t generation);
    // Why the composite was left alone (logged with TENCHU_TRACE=postfx).
    void skipped(const char* reason);
    // The last processed scene (dumpframe), or nullptr.
    ID3D11Texture2D* output() const { return output_.texture; }
    // Once per frame, at the swap.
    void endFrame();

private:
    struct Target {
        ID3D11Texture2D* texture = nullptr;
        ID3D11RenderTargetView* target = nullptr;
        ID3D11ShaderResourceView* view = nullptr;
        UINT width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };
    bool ensure(Target& target, UINT width, UINT height, DXGI_FORMAT format);
    void bindCommonState();
    void writeConstants(const void* data, size_t size);
    void pass(ID3D11PixelShader* shader, const Target& target, ID3D11ShaderResourceView* const* inputs, UINT count,
              const Target* second = nullptr);
    const Projection* projection() const;
    void logStatus();

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    bool ready_ = false;  // shaders compiled
    ID3D11VertexShader* vertex_ = nullptr;
    ID3D11PixelShader* prepass_ = nullptr;  // half-res view depth
    ID3D11PixelShader* gtao_ = nullptr;     // half-res (visibility, view depth)
    ID3D11PixelShader* blur_ = nullptr;     // depth-aware, one axis
    ID3D11PixelShader* gtaoIndirect_ = nullptr;  // the same plus bounced light (experimental)
    ID3D11PixelShader* blurIndirect_ = nullptr;
    ID3D11PixelShader* apply_ = nullptr;    // full res: sharpening, upsampled occlusion
    ID3D11PixelShader* downsample_ = nullptr;  // stable bloom: box filter to the guest size
    ID3D11Buffer* constants_ = nullptr;
    ID3D11SamplerState* pointClamp_ = nullptr;
    ID3D11SamplerState* linearClamp_ = nullptr;
    Target viewDepth_, occlusion_[2], indirect_[2], output_, bloom_;

    // Projection votes of this frame's depth-writing draws; the winner of the last frame
    // stands in when a frame has none.
    struct Candidate {
        Projection projection;
        uint32_t votes = 0;
    };
    Candidate candidates_[8];
    uint32_t candidateCount_ = 0, offered_ = 0;
    Projection lastFrame_;
    bool lastFrameValid_ = false;

    // The result for the last inputs (a second composite of the same frame reuses it).
    ID3D11ShaderResourceView* lastScene_ = nullptr;
    ID3D11ShaderResourceView* lastDepth_ = nullptr;
    uint64_t lastGeneration_ = ~0ull;
    bool lastValid_ = false;
    ID3D11ShaderResourceView* bloomScene_ = nullptr;
    uint64_t bloomGeneration_ = ~0ull;
    bool bloomValid_ = false;
    // TENCHU_TRACE=postfx: once a second, the projection and how composites went.
    const char* skipReason_ = nullptr;
    uint32_t applied_ = 0, skippedCount_ = 0;
    uint64_t statusAt_ = 0;
};

}  // namespace tz::gpu
