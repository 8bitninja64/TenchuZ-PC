#pragma once
// Xenos (Xbox 360 GPU) shader microcode decoding.
//
// A program starts with control flow instructions, two 48-bit instructions
// per three dwords. EXEC instructions address 96-bit ALU or fetch
// instructions by index (three dwords each, counted from the program start).
#include <cstdint>
#include <string>
#include <vector>

namespace tz::xenos {

enum class ShaderType : uint8_t { Vertex = 0, Pixel = 1 };

// ---- Control flow -------------------------------------------------------------

enum class CfOpcode : uint8_t {
    Nop = 0,
    Exec = 1,
    ExecEnd = 2,
    CondExec = 3,
    CondExecEnd = 4,
    CondPredExec = 5,
    CondPredExecEnd = 6,
    LoopStart = 7,
    LoopEnd = 8,
    CondCall = 9,
    Return = 10,
    CondJmp = 11,
    Alloc = 12,
    CondExecPredClean = 13,
    CondExecPredCleanEnd = 14,
    MarkVsFetchDone = 15,
};

struct CfInstruction {
    uint64_t bits = 0;  // 48 bits
    CfOpcode opcode() const { return CfOpcode((bits >> 44) & 0xF); }
    uint32_t field(int shift, int width) const { return uint32_t((bits >> shift) & ((1ull << width) - 1)); }

    // EXEC family
    uint32_t execAddress() const { return field(0, 12); }
    uint32_t execCount() const { return field(12, 3); }
    uint32_t serialize() const { return field(16, 12); }
    uint32_t boolAddress() const { return field(34, 8); }
    bool condition() const { return field(42, 1); }
    // Instruction i of an exec is a fetch when its serialize bit pair has bit 0 set.
    bool isFetch(uint32_t i) const { return (serialize() >> (i * 2)) & 1; }
    bool isEnd() const {
        const auto op = opcode();
        return op == CfOpcode::ExecEnd || op == CfOpcode::CondExecEnd || op == CfOpcode::CondPredExecEnd ||
               op == CfOpcode::CondExecPredCleanEnd;
    }
    bool isExec() const {
        const auto op = opcode();
        return op == CfOpcode::Exec || op == CfOpcode::ExecEnd || op == CfOpcode::CondExec ||
               op == CfOpcode::CondExecEnd || op == CfOpcode::CondPredExec || op == CfOpcode::CondPredExecEnd ||
               op == CfOpcode::CondExecPredClean || op == CfOpcode::CondExecPredCleanEnd;
    }

    // LOOP_START / LOOP_END
    uint32_t loopAddress() const { return field(0, 13); }
    uint32_t loopId() const { return field(16, 5); }

    // COND_JMP / COND_CALL
    uint32_t jumpAddress() const { return field(0, 13); }
    bool forceCall() const { return field(13, 1); }       // unconditional
    bool predicatedJump() const { return field(14, 1); }

    // ALLOC: buffer 1 = position/interpolators (VS) or colors (PS), 2 = position?, 3 = memexport
    uint32_t allocSize() const { return field(0, 3); }
    uint32_t allocBuffer() const { return field(41, 2); }
};

// ---- ALU -----------------------------------------------------------------------------

enum class VectorOpcode : uint8_t {
    Add = 0, Mul, Max, Min, SetEq, SetGt, SetGe, SetNe, Frac, Trunc, Floor, MulAdd, CndEq, CndGe, CndGt,
    Dot4, Dot3, Dot2Add, Cube, Max4, PredSetEqPush, PredSetNePush, PredSetGtPush, PredSetGePush,
    KillEq, KillGt, KillGe, KillNe, Dst, MovA,
};

enum class ScalarOpcode : uint8_t {
    Add = 0, AddPrev, Mul, MulPrev, MulPrev2, Max, Min, SetEq, SetGt, SetGe, SetNe, Frac, Trunc, Floor,
    ExpIeee, LogClamp, LogIeee, RecipClamp, RecipFf, RecipIeee, RsqClamp, RsqFf, RsqIeee, MovA, MovAFloor,
    Sub, SubPrev, PredSetEq, PredSetNe, PredSetGt, PredSetGe, PredSetInv, PredSetPop, PredSetClr,
    PredSetRestore, KillEq, KillGt, KillGe, KillNe, KillOne, SqrtIeee, Unused41, MulConst0, MulConst1,
    AddConst0, AddConst1, SubConst0, SubConst1, Sin, Cos, RetainPrev,
};

struct AluInstruction {
    uint32_t words[3];
    uint32_t bits(int word, int shift, int width) const { return (words[word] >> shift) & ((1u << width) - 1); }

    uint32_t vectorDest() const { return bits(0, 0, 6); }
    bool vectorDestRelative() const { return bits(0, 6, 1); }
    bool absConstants() const { return bits(0, 7, 1); }
    uint32_t scalarDest() const { return bits(0, 8, 6); }
    bool scalarDestRelative() const { return bits(0, 14, 1); }
    bool exportData() const { return bits(0, 15, 1); }
    uint32_t vectorWriteMask() const { return bits(0, 16, 4); }
    uint32_t scalarWriteMask() const { return bits(0, 20, 4); }
    bool vectorClamp() const { return bits(0, 24, 1); }
    bool scalarClamp() const { return bits(0, 25, 1); }
    ScalarOpcode scalarOpcode() const { return ScalarOpcode(bits(0, 26, 6)); }

    // Sources are numbered 1..3; src3 feeds the scalar unit.
    uint32_t swizzle(int source) const { return bits(1, (3 - source) * 8, 8); }
    bool negate(int source) const { return bits(1, 24 + (3 - source), 1); }
    bool predicateCondition() const { return bits(1, 27, 1); }
    bool predicated() const { return bits(1, 28, 1); }
    bool addressAbsolute() const { return bits(1, 29, 1); }
    bool const1RelAbs() const { return bits(1, 30, 1); }
    bool const0RelAbs() const { return bits(1, 31, 1); }

    uint32_t sourceRegister(int source) const { return bits(2, (3 - source) * 8, 8); }
    VectorOpcode vectorOpcode() const { return VectorOpcode(bits(2, 24, 5)); }
    // true: temporary register, false: float constant
    bool sourceIsTemp(int source) const { return bits(2, 29 + (3 - source), 1); }

    // MUL/ADD/SUB_CONST_0/1 take src3 as a constant and a second, temporary
    // operand whose register number is packed into otherwise unused bits.
    bool isScalarConstantOp() const {
        const auto op = scalarOpcode();
        return op >= ScalarOpcode::MulConst0 && op <= ScalarOpcode::SubConst1;
    }
    uint32_t scalarConstantOpTemp() const {
        return (uint32_t(scalarOpcode()) & 1) | (swizzle(3) & 0x3C) | (bits(2, 29, 1) << 1);
    }
};

// ---- Fetch ------------------------------------------------------------------------

enum class FetchOpcode : uint8_t {
    VertexFetch = 0,
    TextureFetch = 1,
    GetBorderColorFrac = 16,
    GetCompTexLod = 17,
    GetGradients = 18,
    GetWeights = 19,
    SetTexLod = 24,
    SetGradientsH = 25,
    SetGradientsV = 26,
};

struct FetchInstruction {
    uint32_t words[3];
    uint32_t bits(int word, int shift, int width) const { return (words[word] >> shift) & ((1u << width) - 1); }

    FetchOpcode opcode() const { return FetchOpcode(bits(0, 0, 5)); }
    uint32_t sourceRegister() const { return bits(0, 5, 6); }
    bool sourceRelative() const { return bits(0, 11, 1); }
    uint32_t destRegister() const { return bits(0, 12, 6); }
    bool destRelative() const { return bits(0, 18, 1); }
    // 3 bits per component: 0-3 select, 4 = 0.0, 5 = 1.0, 7 = keep.
    uint32_t destSwizzle() const { return bits(1, 0, 12); }
    bool predicated() const { return bits(1, 31, 1); }
    bool predicateCondition() const { return bits(2, 31, 1); }

    // Vertex fetch
    uint32_t vertexConstant() const { return bits(0, 20, 5) * 3 + bits(0, 25, 2); }
    uint32_t vertexSourceComponent() const { return bits(0, 30, 2); }
    bool vertexSigned() const { return bits(1, 12, 1); }
    bool vertexInteger() const { return bits(1, 13, 1); }  // false: normalized fraction
    uint32_t vertexFormat() const { return bits(1, 16, 6); }
    int32_t vertexExpAdjust() const { return int32_t(bits(1, 24, 6) << 26) >> 26; }
    uint32_t vertexStride() const { return bits(2, 0, 8); }    // dwords
    uint32_t vertexOffset() const { return bits(2, 8, 22); }   // dwords
    bool isMiniFetch() const { return bits(0, 19, 1) == 0; }   // reuses the previous full fetch's index

    // Texture fetch
    uint32_t textureConstant() const { return bits(0, 20, 5); }
    bool unnormalizedCoordinates() const { return bits(0, 25, 1); }
    uint32_t textureSourceSwizzle() const { return bits(0, 26, 6); }  // 2 bits per component, absolute
    uint32_t magFilter() const { return bits(1, 12, 2); }
    uint32_t minFilter() const { return bits(1, 14, 2); }
    uint32_t mipFilter() const { return bits(1, 16, 2); }
    bool useComputedLod() const { return bits(1, 28, 1); }
    bool useRegisterLod() const { return bits(1, 29, 1); }
    int32_t lodBias() const { return int32_t(bits(2, 2, 7) << 25) >> 25; }
    uint32_t dimension() const { return bits(2, 14, 2); }  // 0 1D, 1 2D, 2 3D/stacked, 3 cube
    // Texel offsets in half-texel units.
    int32_t offsetX() const { return int32_t(bits(2, 16, 5) << 27) >> 27; }
    int32_t offsetY() const { return int32_t(bits(2, 21, 5) << 27) >> 27; }
    int32_t offsetZ() const { return int32_t(bits(2, 26, 5) << 27) >> 27; }
};

// A decoded program. Instruction slots are three dwords each.
struct Program {
    ShaderType type = ShaderType::Vertex;
    std::vector<uint32_t> words;  // host-endian
    std::vector<CfInstruction> controlFlow;
    uint32_t slotCount() const { return uint32_t(words.size() / 3); }
    AluInstruction alu(uint32_t slot) const {
        return {{words[slot * 3], words[slot * 3 + 1], words[slot * 3 + 2]}};
    }
    FetchInstruction fetch(uint32_t slot) const {
        return {{words[slot * 3], words[slot * 3 + 1], words[slot * 3 + 2]}};
    }
};

// Decodes control flow until the first EXEC_END (or the end of the words).
Program decode(ShaderType type, const std::vector<uint32_t>& words);
std::string disassemble(const Program& program);

}  // namespace tz::xenos
