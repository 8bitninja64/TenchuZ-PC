#pragma once
// Translates Xenos shader microcode to Shader Model 5 HLSL.
#include "ucode.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz::xenos {

// Resource bindings shared between the translator and the renderer.
//   b0  float constants c0-c255 (vertex) or c256-c511 (pixel), as c[0..255]
//   b1  SharedConstants (see shaderPrelude())
//   t0+ raw vertex buffers, one per used vertex fetch constant (VS)
//   t0+ textures and s0+ samplers, one per used texture fetch constant (PS/VS)
struct ShaderInfo {
    // Vertex fetch constants read by the shader, in binding order (t0, t1, ...).
    std::vector<uint32_t> vertexFetchConstants;
    // What each binding reads, parallel to vertexFetchConstants. With `direct`
    // set, every fetch is indexed by the unmodified vertex index (r0.x), so the
    // bytes read lie in [(min * minStride + minOffset) * 4, (max * maxStride + maxEnd) * 4)
    // for the draw's smallest and largest index.
    struct VertexStream {
        bool direct = true;
        uint32_t minStride = ~0u, maxStride = 0;  // dwords per vertex
        uint32_t minOffset = ~0u, maxEnd = 0;     // dwords from the vertex start
    };
    std::vector<VertexStream> vertexStreams;
    // Texture fetch constants and their dimension (1-3, 4 = cube), binding order.
    // filters: the fetch instruction's mag/min/mip filter fields (2 bits each, 3 = use
    // the fetch constant's), which override the constant's filtering.
    struct Texture { uint32_t fetchConstant; uint32_t dimension; uint32_t filters; };
    std::vector<Texture> textures;
    bool writesDepth = false;
    bool usesKill = false;
    uint32_t colorTargetMask = 0;  // pixel shader exports oC0-oC3
};

struct TranslatedShader {
    std::string hlsl;
    ShaderInfo info;
    std::string error;  // nonempty when the program uses unsupported features
};

TranslatedShader translate(const Program& program);

}  // namespace tz::xenos
