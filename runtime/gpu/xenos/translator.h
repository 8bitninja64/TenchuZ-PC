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
    // Texture fetch constants and their dimension (1-3, 4 = cube), binding order.
    struct Texture { uint32_t fetchConstant; uint32_t dimension; };
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
