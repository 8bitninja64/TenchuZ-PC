#pragma once
// Executes Xenos draws, resolves and swaps with Direct3D 11.
// Runs on the command processor thread.
#include "gpu/d3d11/renderer.h"
#include "gpu/xenos/translator.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace tz::gpu {

// Set by the command processor when a vertex (0) or pixel (1) shader constant
// register is written; draws rebuild their constant buffers only then.
extern std::atomic<bool> constantsWritten[2];

struct DrawRequest {
    uint32_t initiator = 0;          // VGT_DRAW_INITIATOR
    uint32_t indexBase = 0;          // DMA index buffer (physical)
    uint32_t indexEndian = 0;
    std::vector<uint32_t> inlineIndices;  // DRAW_INDX_2 immediate data, guest dwords
};

// Microcode lookup by the hash the command processor assigned.
using ShaderSource = std::function<const std::vector<uint32_t>*(uint64_t hash)>;

class XenosBackend {
public:
    XenosBackend(D3D11Renderer& renderer, const std::atomic<uint32_t>* registers, ShaderSource shaders);
    ~XenosBackend();
    void draw(const DrawRequest& request, uint64_t vertexShader, uint64_t pixelShader);
    void swap(uint32_t frontBuffer);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tz::gpu
