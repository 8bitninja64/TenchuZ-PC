#include "translator.h"
#include <cstdio>
#include <map>
#include <set>

namespace tz::xenos {
namespace {

// Constant buffer and helpers common to every translated shader.
const char* kPrelude = R"(
cbuffer FloatConstants : register(b0) { float4 c[256]; };
cbuffer SharedConstants : register(b1) {
    uint4 g_bools[2];          // 256 boolean constants, one bit each
    uint4 g_loops[8];          // 32 loop constants: count, start, step bytes
    uint4 g_vertexEndian[4];   // endian mode per vertex buffer binding
    uint4 g_vertexBase[4];     // byte offset of each vertex buffer binding in guestData
    float4 g_textureSize[16];  // per texture binding: width, height, depth
    uint4 g_textureSwizzle[4]; // per texture binding: 3 bits per component (0-3 channel, 4 zero, 5 one)
    float4 g_positionScale;    // guest clip space -> host clip space: xyz scale
    float4 g_positionOffset;   // xyz offset, multiplied by w
    float4 g_alphaTest;        // x: function (0 never .. 7 always), y: reference
    uint4 g_draw;              // x: index list byte offset (0xFFFFFFFF: auto), y: index offset
};
static const float kFltMax = 3.402823466e+38;
// Xenos multiplies follow the D3D9 rule 0 * x = 0, even for infinities and NaN
// (normalizing a zero vector multiplies 0 by rsq(0) = inf).
float4 XMul(float4 a, float4 b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float XMul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float XDot4(float4 a, float4 b) { float4 m = XMul(a, b); return m.x + m.y + m.z + m.w; }
float XDot3(float4 a, float4 b) { float4 m = XMul(a, b); return m.x + m.y + m.z; }
uint SwapEndian(uint v, uint mode) {
    if (mode == 1) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    if (mode == 2) return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
    if (mode == 3) return (v << 16) | (v >> 16);
    return v;
}
float4 ApplySwizzle(float4 v, uint swizzle) {
    float4 result;
    [unroll] for (uint i = 0; i < 4; ++i) {
        uint select = (swizzle >> (i * 3)) & 7;
        result[i] = select < 4 ? v[select] : (select == 5 ? 1.0 : 0.0);
    }
    return result;
}
bool BoolConstant(uint index) { return (g_bools[index >> 7][(index >> 5) & 3] >> (index & 31)) & 1; }
uint LoopConstant(uint index) { return g_loops[index >> 2][index & 3]; }
float SignExtend(uint v, uint bits) { return float(int(v << (32 - bits)) >> (32 - bits)); }
float4 CubeVector(float4 s) {
    // CUBE operands are src.zzxy and src.yxzz; the direction is (s.z, s.w, s.x).
    float3 d = float3(s.z, s.w, s.x);
    float3 a = abs(d);
    if (a.z >= a.x && a.z >= a.y) return float4(-d.y, d.z < 0 ? -d.x : d.x, 2.0 * d.z, d.z < 0 ? 5.0 : 4.0);
    if (a.y >= a.x) return float4(d.y < 0 ? -d.z : d.z, d.x, 2.0 * d.y, d.y < 0 ? 3.0 : 2.0);
    return float4(-d.y, d.x < 0 ? d.z : -d.z, 2.0 * d.x, d.x < 0 ? 1.0 : 0.0);
}
float3 CubeDirection(float3 coords) {
    // Inverse of CubeVector after the shader's (v / |ma|) * 0.5 + 1.5 mapping.
    float s = (coords.y - 1.5) * 2.0, t = (coords.x - 1.5) * 2.0;
    uint face = uint(coords.z);
    if (face == 0) return float3(1, -t, -s);
    if (face == 1) return float3(-1, -t, s);
    if (face == 2) return float3(s, 1, t);
    if (face == 3) return float3(s, -1, -t);
    if (face == 4) return float3(s, -t, 1);
    return float3(-s, -t, -1);
}
)";

const char* kComponents = "xyzw";

struct Emitter {
    const Program& program;
    TranslatedShader result;
    std::string body;
    int indent = 1;
    bool pixel;
    std::map<uint32_t, uint32_t> vertexBindings;   // fetch constant -> t slot
    std::map<uint32_t, uint32_t> textureBindings;  // fetch constant -> t/s slot
    std::map<uint32_t, uint32_t> textureDimensions;
    uint32_t lastVertexFetchConstant = 0;

    explicit Emitter(const Program& p) : program(p), pixel(p.type == ShaderType::Pixel) {}

    void line(const std::string& text) {
        body.append(size_t(indent) * 4, ' ');
        body += text;
        body += '\n';
    }
    void unsupported(const std::string& what) {
        if (result.error.empty()) result.error = what;
        line("// UNSUPPORTED: " + what);
    }

    // ---- Operands ----------------------------------------------------------------

    // Size of one vertex element in dwords (upper bound 4 for unknown formats).
    static uint32_t vertexFormatDwords(uint32_t format) {
        switch (format) {
        case 6: case 7: case 16: case 17: case 25: case 31: case 33: case 36: return 1;
        case 26: case 32: case 34: case 37: return 2;
        case 57: return 3;
        default: return 4;
        }
    }

    // r0.x (the vertex index on entry) written so far in program order; CF index being
    // emitted; lowest CF index any jump, call or loop can transfer to.
    bool r0xWritten = false;
    bool lastFetchDirect = false;
    uint32_t currentCf = 0;
    uint32_t firstBranchTarget = ~0u;

    void noteTempWrite(uint32_t dest, bool relative, bool writesX) {
        if (relative || (dest == 0 && writesX)) r0xWritten = true;
    }

    std::string tempRegister(uint32_t index, bool relative) {
        if (relative) return "r[clamp(int(" + std::to_string(index) + ") + aL, 0, 63)]";
        return "r[" + std::to_string(index) + "]";
    }

    // Constant operands are numbered in source order; each may be relative.
    std::string constantRegister(const AluInstruction& alu, uint32_t index, int constantOrdinal) {
        const bool relative = constantOrdinal == 0 ? alu.const0RelAbs() : alu.const1RelAbs();
        if (!relative) return "c[" + std::to_string(index) + "]";
        return std::string("c[clamp(int(") + std::to_string(index) + ") + " + (alu.addressAbsolute() ? "a0" : "aL") +
               ", 0, 255)]";
    }

    std::string sourceRegister(const AluInstruction& alu, int source, int& constantOrdinal) {
        const uint32_t reg = alu.sourceRegister(source);
        std::string text;
        bool absolute;
        if (alu.sourceIsTemp(source)) {
            text = tempRegister(reg & 0x3F, reg & 0x40);
            absolute = reg & 0x80;
        } else {
            text = constantRegister(alu, reg, constantOrdinal++);
            absolute = alu.absConstants();
        }
        if (absolute) text = "abs(" + text + ")";
        if (alu.negate(source)) text = "-" + text;
        return text;
    }

    std::string vectorSource(const AluInstruction& alu, int source, int& constantOrdinal) {
        std::string text = sourceRegister(alu, source, constantOrdinal);
        const uint32_t swizzle = alu.swizzle(source);
        std::string components;
        for (int i = 0; i < 4; ++i) components += kComponents[((swizzle >> (i * 2)) + i) & 3];
        return components == "xyzw" ? "(" + text + ")" : "(" + text + ")." + components;
    }

    // ---- ALU ---------------------------------------------------------------------

    std::string vectorExpression(const AluInstruction& alu, int& constantOrdinal, bool& valid) {
        const auto op = alu.vectorOpcode();
        int operands = 2;
        switch (op) {
        case VectorOpcode::Frac: case VectorOpcode::Trunc: case VectorOpcode::Floor: case VectorOpcode::MovA:
            operands = 1;
            break;
        case VectorOpcode::MulAdd: case VectorOpcode::CndEq: case VectorOpcode::CndGe: case VectorOpcode::CndGt:
        case VectorOpcode::Dot2Add:
            operands = 3;
            break;
        default:
            break;
        }
        std::string s[4];
        for (int i = 1; i <= operands; ++i) s[i] = vectorSource(alu, i, constantOrdinal);
        valid = true;
        switch (op) {
        case VectorOpcode::Add: return s[1] + " + " + s[2];
        case VectorOpcode::Mul: return "XMul(" + s[1] + ", " + s[2] + ")";
        case VectorOpcode::Max: return "max(" + s[1] + ", " + s[2] + ")";
        case VectorOpcode::Min: return "min(" + s[1] + ", " + s[2] + ")";
        case VectorOpcode::SetEq: return "float4(" + s[1] + " == " + s[2] + ")";
        case VectorOpcode::SetGt: return "float4(" + s[1] + " > " + s[2] + ")";
        case VectorOpcode::SetGe: return "float4(" + s[1] + " >= " + s[2] + ")";
        case VectorOpcode::SetNe: return "float4(" + s[1] + " != " + s[2] + ")";
        case VectorOpcode::Frac: return "frac(" + s[1] + ")";
        case VectorOpcode::Trunc: return "trunc(" + s[1] + ")";
        case VectorOpcode::Floor: return "floor(" + s[1] + ")";
        case VectorOpcode::MulAdd: return "XMul(" + s[1] + ", " + s[2] + ") + " + s[3];
        case VectorOpcode::CndEq: return "(" + s[1] + " == 0.0) ? " + s[2] + " : " + s[3];
        case VectorOpcode::CndGe: return "(" + s[1] + " >= 0.0) ? " + s[2] + " : " + s[3];
        case VectorOpcode::CndGt: return "(" + s[1] + " > 0.0) ? " + s[2] + " : " + s[3];
        case VectorOpcode::Dot4: return "XDot4(" + s[1] + ", " + s[2] + ").xxxx";
        case VectorOpcode::Dot3: return "XDot3(" + s[1] + ", " + s[2] + ").xxxx";
        case VectorOpcode::Dot2Add:
            return "(XMul((" + s[1] + ").x, (" + s[2] + ").x) + XMul((" + s[1] + ").y, (" + s[2] + ").y) + (" + s[3] +
                   ").x).xxxx";
        case VectorOpcode::Cube: return "CubeVector(" + s[1] + ")";
        case VectorOpcode::Max4: {
            const std::string v = "(" + s[1] + ")";
            return "max(max(" + v + ".x, " + v + ".y), max(" + v + ".z, " + v + ".w)).xxxx";
        }
        case VectorOpcode::Dst:
            return "float4(1.0, (" + s[1] + ").y * (" + s[2] + ").y, (" + s[1] + ").z, (" + s[2] + ").w)";
        case VectorOpcode::MovA:  // maxa: also loads a0
            line("a0 = int(clamp(floor((" + s[1] + ").w + 0.5), -256.0, 255.0));");
            return "max(" + s[1] + ", " + s[2] + ")";
        case VectorOpcode::KillEq: case VectorOpcode::KillGt: case VectorOpcode::KillGe: case VectorOpcode::KillNe: {
            static const char* compare[] = {"==", ">", ">=", "!="};
            const std::string test = "any(" + s[1] + " " +
                                     compare[uint32_t(op) - uint32_t(VectorOpcode::KillEq)] + " " + s[2] + ")";
            if (pixel) {
                line("if (" + test + ") discard;");
                result.info.usesKill = true;
            }
            return "float4(" + test + ".xxxx)";
        }
        case VectorOpcode::PredSetEqPush: case VectorOpcode::PredSetNePush: case VectorOpcode::PredSetGtPush:
        case VectorOpcode::PredSetGePush: {
            static const char* compare[] = {"==", "!=", ">", ">="};
            const char* cmp = compare[uint32_t(op) - uint32_t(VectorOpcode::PredSetEqPush)];
            line(std::string("p = (") + s[1] + ").w == 0.0 && (" + s[2] + ").w " + cmp + " 0.0;");
            return std::string("(p ? 0.0 : (") + s[1] + ").x + 1.0).xxxx";
        }
        default:
            valid = false;
            return "float4(0, 0, 0, 0)";
        }
    }

    std::string scalarExpression(const AluInstruction& alu, int& constantOrdinal) {
        const auto op = alu.scalarOpcode();
        const uint32_t swizzle = alu.swizzle(3);
        const char componentA = kComponents[((swizzle >> 6) + 3) & 3];
        const char componentB = kComponents[swizzle & 3];
        std::string a, b;
        if (alu.isScalarConstantOp()) {
            // src3 is a constant; the second operand is a temporary.
            std::string constant = constantRegister(alu, alu.sourceRegister(3), constantOrdinal++);
            if (alu.absConstants()) constant = "abs(" + constant + ")";
            a = std::string(alu.negate(3) ? "-" : "") + "(" + constant + ")." + componentA;
            b = "r[" + std::to_string(alu.scalarConstantOpTemp()) + "]." + componentB;
        } else {
            const std::string source = sourceRegister(alu, 3, constantOrdinal);
            a = "(" + source + ")." + componentA;
            b = "(" + source + ")." + componentB;
        }
        switch (op) {
        case ScalarOpcode::Add: return a + " + " + b;
        case ScalarOpcode::AddPrev: return a + " + ps";
        case ScalarOpcode::Mul: return "XMul(" + a + ", " + b + ")";
        case ScalarOpcode::MulPrev: return "XMul(" + a + ", ps)";
        case ScalarOpcode::MulPrev2:
            return "(ps == -kFltMax || !isfinite(ps) || !isfinite(" + b + ") || " + b + " <= 0.0) ? -kFltMax : " + a +
                   " * ps";
        case ScalarOpcode::Max: return "max(" + a + ", " + b + ")";
        case ScalarOpcode::Min: return "min(" + a + ", " + b + ")";
        case ScalarOpcode::SetEq: return "float(" + a + " == 0.0)";
        case ScalarOpcode::SetGt: return "float(" + a + " > 0.0)";
        case ScalarOpcode::SetGe: return "float(" + a + " >= 0.0)";
        case ScalarOpcode::SetNe: return "float(" + a + " != 0.0)";
        case ScalarOpcode::Frac: return "frac(" + a + ")";
        case ScalarOpcode::Trunc: return "trunc(" + a + ")";
        case ScalarOpcode::Floor: return "floor(" + a + ")";
        case ScalarOpcode::ExpIeee: return "exp2(" + a + ")";
        case ScalarOpcode::LogClamp: return "(" + a + " == 0.0 ? -kFltMax : max(log2(" + a + "), -kFltMax))";
        case ScalarOpcode::LogIeee: return "log2(" + a + ")";
        case ScalarOpcode::RecipClamp: return "clamp(1.0 / " + a + ", -kFltMax, kFltMax)";
        case ScalarOpcode::RecipFf: return "(isinf(1.0 / " + a + ") ? 0.0 : 1.0 / " + a + ")";
        case ScalarOpcode::RecipIeee: return "(1.0 / " + a + ")";
        case ScalarOpcode::RsqClamp: return "clamp(rsqrt(" + a + "), -kFltMax, kFltMax)";
        case ScalarOpcode::RsqFf: return "(isinf(rsqrt(" + a + ")) ? 0.0 : rsqrt(" + a + "))";
        case ScalarOpcode::RsqIeee: return "rsqrt(" + a + ")";
        case ScalarOpcode::MovA:
            line("a0 = int(clamp(floor(" + a + " + 0.5), -256.0, 255.0));");
            return a;
        case ScalarOpcode::MovAFloor:
            line("a0 = int(clamp(floor(" + a + "), -256.0, 255.0));");
            return a;
        case ScalarOpcode::Sub: return a + " - " + b;
        case ScalarOpcode::SubPrev: return a + " - ps";
        case ScalarOpcode::PredSetEq: line("p = " + a + " == 0.0;"); return "(p ? 0.0 : 1.0)";
        case ScalarOpcode::PredSetNe: line("p = " + a + " != 0.0;"); return "(p ? 0.0 : 1.0)";
        case ScalarOpcode::PredSetGt: line("p = " + a + " > 0.0;"); return "(p ? 0.0 : 1.0)";
        case ScalarOpcode::PredSetGe: line("p = " + a + " >= 0.0;"); return "(p ? 0.0 : 1.0)";
        case ScalarOpcode::PredSetInv:
            line("p = " + a + " == 1.0;");
            return "(p ? 0.0 : (" + a + " == 0.0 ? 1.0 : " + a + "))";
        case ScalarOpcode::PredSetPop:
            line("p = " + a + " - 1.0 <= 0.0;");
            return "(p ? 0.0 : " + a + " - 1.0)";
        case ScalarOpcode::PredSetClr: line("p = false;"); return "kFltMax";
        case ScalarOpcode::PredSetRestore: line("p = " + a + " == 0.0;"); return "(p ? 0.0 : " + a + ")";
        case ScalarOpcode::KillEq: case ScalarOpcode::KillGt: case ScalarOpcode::KillGe: case ScalarOpcode::KillNe:
        case ScalarOpcode::KillOne: {
            static const char* compare[] = {"== 0.0", "> 0.0", ">= 0.0", "!= 0.0", "== 1.0"};
            const std::string test = a + " " + compare[uint32_t(op) - uint32_t(ScalarOpcode::KillEq)];
            if (pixel) {
                line("if (" + test + ") discard;");
                result.info.usesKill = true;
            }
            return "float(" + test + ")";
        }
        case ScalarOpcode::SqrtIeee: return "sqrt(" + a + ")";
        case ScalarOpcode::MulConst0: case ScalarOpcode::MulConst1: return "XMul(" + a + ", " + b + ")";
        case ScalarOpcode::AddConst0: case ScalarOpcode::AddConst1: return a + " + " + b;
        case ScalarOpcode::SubConst0: case ScalarOpcode::SubConst1: return a + " - " + b;
        case ScalarOpcode::Sin: return "sin(" + a + ")";
        case ScalarOpcode::Cos: return "cos(" + a + ")";
        case ScalarOpcode::RetainPrev: return "ps";
        default:
            unsupported("scalar opcode " + std::to_string(uint32_t(op)));
            return "0.0";
        }
    }

    std::string destination(uint32_t dest, bool exported, bool relative) {
        if (!exported) return tempRegister(dest, relative);
        if (!pixel) {
            if (dest == 62) return "oPos";
            if (dest == 63) return "oPoint";
            if (dest < 16) return "o[" + std::to_string(dest) + "]";
        } else {
            if (dest < 4) {
                result.info.colorTargetMask |= 1u << dest;
                return "oC[" + std::to_string(dest) + "]";
            }
            if (dest == 61) {
                result.info.writesDepth = true;
                return "oDepth";
            }
        }
        return "oDiscard";  // memexport and other unsupported targets
    }

    static std::string mask(uint32_t writeMask) {
        std::string text;
        for (int i = 0; i < 4; ++i)
            if (writeMask & (1 << i)) text += kComponents[i];
        return text;
    }

    void emitAlu(const AluInstruction& alu) {
        const auto vop = alu.vectorOpcode();
        const auto sop = alu.scalarOpcode();
        const bool vectorActive = alu.vectorWriteMask() || uint32_t(vop) >= uint32_t(VectorOpcode::PredSetEqPush);
        if (alu.predicated()) {
            line(std::string("if (") + (alu.predicateCondition() ? "p" : "!p") + ") {");
            ++indent;
        }
        line("{");
        ++indent;
        // Both units read their sources before either writes.
        int constantOrdinal = 0;
        if (vectorActive) {
            bool valid;
            const std::string expression = vectorExpression(alu, constantOrdinal, valid);
            if (!valid) unsupported("vector opcode " + std::to_string(uint32_t(vop)));
            line("float4 v = " + expression + ";");
            if (alu.vectorClamp()) line("v = saturate(v);");
        } else if (alu.isScalarConstantOp()) {
            constantOrdinal = 0;
        }
        // Every scalar operation updates ps, even without a destination: compilers park
        // values there for a later *_prev instruction.
        line("float s = " + scalarExpression(alu, constantOrdinal) + ";");
        if (alu.scalarClamp()) line("s = saturate(s);");
        if (vectorActive && alu.vectorWriteMask()) {
            const std::string dest = destination(alu.vectorDest(), alu.exportData(), alu.vectorDestRelative());
            const std::string m = mask(alu.vectorWriteMask());
            line(dest + "." + m + " = v." + m + ";");
        }
        if (!alu.exportData()) {
            if (vectorActive && alu.vectorWriteMask())
                noteTempWrite(alu.vectorDest(), alu.vectorDestRelative(), alu.vectorWriteMask() & 1);
            if (alu.scalarWriteMask()) noteTempWrite(alu.scalarDest(), alu.scalarDestRelative(), alu.scalarWriteMask() & 1);
        }
        if (alu.scalarWriteMask()) {
            const std::string dest = destination(alu.scalarDest(), alu.exportData(), alu.scalarDestRelative());
            const std::string m = mask(alu.scalarWriteMask());
            line(dest + "." + m + " = s.xxxx." + m + ";");
        }
        line("ps = s;");
        --indent;
        line("}");
        if (alu.predicated()) {
            --indent;
            line("}");
        }
    }

    // ---- Fetch ------------------------------------------------------------------

    std::string fetchDestination(const FetchInstruction& fetch, const std::string& value) {
        // 3 bits per component: 0-3 select, 4 = 0.0, 5 = 1.0, 6/7 = keep.
        noteTempWrite(fetch.destRegister(), fetch.destRelative(), (fetch.destSwizzle() & 7) < 6);
        const std::string dest = tempRegister(fetch.destRegister(), fetch.destRelative());
        std::string out;
        for (int i = 0; i < 4; ++i) {
            const uint32_t select = (fetch.destSwizzle() >> (i * 3)) & 7;
            if (select >= 6) continue;
            const std::string source = select < 4 ? value + "." + kComponents[select] : select == 4 ? "0.0" : "1.0";
            out += dest + "." + kComponents[i] + " = " + source + "; ";
        }
        return out;
    }

    void emitVertexFetch(const FetchInstruction& fetch) {
        uint32_t constant = fetch.vertexConstant();
        if (fetch.isMiniFetch()) {
            constant = lastVertexFetchConstant;
        } else {
            lastVertexFetchConstant = constant;
            // The index is the raw vertex index when it comes from an unwritten r0.x and
            // no branch can reach this clause after r0 changes.
            lastFetchDirect = !fetch.sourceRelative() && fetch.sourceRegister() == 0 &&
                              fetch.vertexSourceComponent() == 0 && !r0xWritten && currentCf < firstBranchTarget;
            line("vfIndex = int(floor(" + tempRegister(fetch.sourceRegister(), fetch.sourceRelative()) + "." +
                 kComponents[fetch.vertexSourceComponent()] + " + 0.5));");
        }
        auto [it, inserted] = vertexBindings.emplace(constant, uint32_t(vertexBindings.size()));
        if (inserted) {
            result.info.vertexFetchConstants.push_back(constant);
            result.info.vertexStreams.emplace_back();
        }
        const uint32_t slot = it->second;
        {
            auto& stream = result.info.vertexStreams[slot];
            stream.direct = stream.direct && lastFetchDirect;
            stream.minStride = (std::min)(stream.minStride, fetch.vertexStride());
            stream.maxStride = (std::max)(stream.maxStride, fetch.vertexStride());
            stream.minOffset = (std::min)(stream.minOffset, fetch.vertexOffset());
            stream.maxEnd = (std::max)(stream.maxEnd, fetch.vertexOffset() + vertexFormatDwords(fetch.vertexFormat()));
        }
        const std::string element = "[" + std::to_string(slot / 4) + "][" + std::to_string(slot % 4) + "]";
        line("{");
        ++indent;
        line("uint address = g_vertexBase" + element + " + uint(vfIndex * " + std::to_string(fetch.vertexStride()) +
             " + " + std::to_string(fetch.vertexOffset()) + ") * 4;");
        auto word = [&](int n) {
            return "SwapEndian(guestData.Load(address + " + std::to_string(n * 4) + "), g_vertexEndian" + element + ")";
        };
        const bool isSigned = fetch.vertexSigned();
        const bool normalized = !fetch.vertexInteger();
        // Integer components to float, normalized or not.
        auto component = [&](const std::string& bits, uint32_t width) {
            std::string value = isSigned ? "SignExtend(" + bits + ", " + std::to_string(width) + ")" : "float(" + bits + ")";
            if (normalized) {
                const uint32_t range = isSigned ? (1u << (width - 1)) - 1 : (width == 32 ? 0xFFFFFFFFu : (1u << width) - 1);
                value = isSigned ? "max(" + value + " / " + std::to_string(range) + ".0, -1.0)"
                                 : value + " / " + std::to_string(range) + ".0";
            }
            return value;
        };
        std::string value;
        switch (fetch.vertexFormat()) {
        case 6: {  // 8_8_8_8
            line("uint w0 = " + word(0) + ";");
            value = "float4(" + component("(w0 & 0xFF)", 8) + ", " + component("((w0 >> 8) & 0xFF)", 8) + ", " +
                    component("((w0 >> 16) & 0xFF)", 8) + ", " + component("(w0 >> 24)", 8) + ")";
            break;
        }
        case 7: {  // 2_10_10_10
            line("uint w0 = " + word(0) + ";");
            value = "float4(" + component("(w0 & 0x3FF)", 10) + ", " + component("((w0 >> 10) & 0x3FF)", 10) + ", " +
                    component("((w0 >> 20) & 0x3FF)", 10) + ", " + component("(w0 >> 30)", 2) + ")";
            break;
        }
        case 16: {  // 10_11_11
            line("uint w0 = " + word(0) + ";");
            value = "float4(" + component("(w0 & 0x7FF)", 11) + ", " + component("((w0 >> 11) & 0x7FF)", 11) + ", " +
                    component("(w0 >> 22)", 10) + ", 1.0)";
            break;
        }
        case 17: {  // 11_11_10
            line("uint w0 = " + word(0) + ";");
            value = "float4(" + component("(w0 & 0x3FF)", 10) + ", " + component("((w0 >> 10) & 0x7FF)", 11) + ", " +
                    component("(w0 >> 21)", 11) + ", 1.0)";
            break;
        }
        case 25: {  // 16_16
            line("uint w0 = " + word(0) + ";");
            value = "float4(" + component("(w0 & 0xFFFF)", 16) + ", " + component("(w0 >> 16)", 16) + ", 0.0, 1.0)";
            break;
        }
        case 26: {  // 16_16_16_16
            line("uint w0 = " + word(0) + "; uint w1 = " + word(1) + ";");
            value = "float4(" + component("(w0 & 0xFFFF)", 16) + ", " + component("(w0 >> 16)", 16) + ", " +
                    component("(w1 & 0xFFFF)", 16) + ", " + component("(w1 >> 16)", 16) + ")";
            break;
        }
        case 31: {  // 16_16_FLOAT
            line("uint w0 = " + word(0) + ";");
            value = "float4(f16tof32(w0), f16tof32(w0 >> 16), 0.0, 1.0)";
            break;
        }
        case 32: {  // 16_16_16_16_FLOAT
            line("uint w0 = " + word(0) + "; uint w1 = " + word(1) + ";");
            value = "float4(f16tof32(w0), f16tof32(w0 >> 16), f16tof32(w1), f16tof32(w1 >> 16))";
            break;
        }
        case 33: case 34: case 35: {  // 32, 32_32, 32_32_32_32 integer
            const int count = int(fetch.vertexFormat()) - 32;
            std::string parts[4] = {"0.0", "0.0", "0.0", "1.0"};
            for (int i = 0; i < count; ++i) {
                line("uint w" + std::to_string(i) + " = " + word(i) + ";");
                parts[i] = isSigned ? "float(int(w" + std::to_string(i) + "))" : "float(w" + std::to_string(i) + ")";
                if (normalized) parts[i] += isSigned ? " / 2147483647.0" : " / 4294967295.0";
            }
            value = "float4(" + parts[0] + ", " + parts[1] + ", " + parts[2] + ", " + parts[3] + ")";
            break;
        }
        case 36: case 37: case 38: case 57: {  // 32_FLOAT .. 32_32_32_32_FLOAT
            const int count = fetch.vertexFormat() == 57 ? 3 : int(fetch.vertexFormat()) - 35;
            std::string parts[4] = {"0.0", "0.0", "0.0", "1.0"};
            for (int i = 0; i < count; ++i) parts[i] = "asfloat(" + word(i) + ")";
            value = "float4(" + parts[0] + ", " + parts[1] + ", " + parts[2] + ", " + parts[3] + ")";
            break;
        }
        default:
            unsupported("vertex format " + std::to_string(fetch.vertexFormat()));
            value = "float4(0, 0, 0, 1)";
            break;
        }
        line("float4 fetched = " + value + ";");
        if (fetch.vertexExpAdjust()) line("fetched *= exp2(" + std::to_string(fetch.vertexExpAdjust()) + ".0);");
        line(fetchDestination(fetch, "fetched"));
        --indent;
        line("}");
    }

    void emitTextureFetch(const FetchInstruction& fetch) {
        const uint32_t constant = fetch.textureConstant();
        const uint32_t dimension = fetch.dimension() == 3 ? 4 : fetch.dimension() + 1;
        auto [it, inserted] = textureBindings.emplace(constant, uint32_t(textureBindings.size()));
        if (inserted) {
            result.info.textures.push_back({constant, dimension});
            textureDimensions[it->second] = dimension;
        } else if (textureDimensions[it->second] != dimension) {
            unsupported("texture constant used with several dimensions");
        }
        const uint32_t slot = it->second;
        const std::string source = tempRegister(fetch.sourceRegister(), fetch.sourceRelative());
        std::string coords;
        const uint32_t count = dimension == 1 ? 1 : dimension == 2 ? 2 : 3;
        for (uint32_t i = 0; i < count; ++i) coords += kComponents[(fetch.textureSourceSwizzle() >> (i * 2)) & 3];
        const std::string size = "g_textureSize[" + std::to_string(slot) + "]";
        const std::string texture = "t" + std::to_string(slot), sampler = "s" + std::to_string(slot);
        line("{");
        ++indent;
        const char* vectorType = count == 1 ? "float" : count == 2 ? "float2" : "float3";
        line(std::string(vectorType) + " uv = " + source + "." + coords + ";");
        if (dimension == 4) {
            line("float3 direction = CubeDirection(uv);");
        } else {
            const std::string sizeComponents = std::string(".") + std::string("xyz").substr(0, count);
            if (fetch.unnormalizedCoordinates()) line("uv /= " + size + sizeComponents + ";");
            if (fetch.offsetX() || fetch.offsetY() || fetch.offsetZ()) {
                const std::string offsets[3] = {std::to_string(fetch.offsetX() * 0.5f),
                                                std::to_string(fetch.offsetY() * 0.5f),
                                                std::to_string(fetch.offsetZ() * 0.5f)};
                std::string offset = count == 1 ? offsets[0] : count == 2 ? "float2(" + offsets[0] + ", " + offsets[1] + ")"
                                                         : "float3(" + offsets[0] + ", " + offsets[1] + ", " + offsets[2] + ")";
                line("uv += " + offset + " / " + size + sizeComponents + ";");
            }
        }
        const std::string coordinate = dimension == 4 ? "direction" : "uv";
        if (pixel && !fetch.useRegisterLod())
            line("float4 texel = " + texture + ".Sample(" + sampler + ", " + coordinate + ");");
        else
            line("float4 texel = " + texture + ".SampleLevel(" + sampler + ", " + coordinate + ", 0.0);");
        line("texel = ApplySwizzle(texel, g_textureSwizzle[" + std::to_string(slot / 4) + "][" +
             std::to_string(slot % 4) + "]);");
        line(fetchDestination(fetch, "texel"));
        --indent;
        line("}");
    }

    void emitExec(const CfInstruction& cf) {
        for (uint32_t n = 0; n < cf.execCount(); ++n) {
            const uint32_t slot = cf.execAddress() + n;
            if (slot >= program.slotCount()) {
                unsupported("instruction slot out of range");
                return;
            }
            if (cf.isFetch(n)) {
                const auto fetch = program.fetch(slot);
                const bool predicated = fetch.predicated();
                if (predicated) {
                    line(std::string("if (") + (fetch.predicateCondition() ? "p" : "!p") + ") {");
                    ++indent;
                }
                if (fetch.opcode() == FetchOpcode::VertexFetch) emitVertexFetch(fetch);
                else if (fetch.opcode() == FetchOpcode::TextureFetch) emitTextureFetch(fetch);
                else if (fetch.opcode() == FetchOpcode::SetTexLod || fetch.opcode() == FetchOpcode::SetGradientsH ||
                         fetch.opcode() == FetchOpcode::SetGradientsV)
                    line("// texture LOD/gradient state ignored");
                else
                    unsupported("fetch opcode " + std::to_string(uint32_t(fetch.opcode())));
                if (predicated) {
                    --indent;
                    line("}");
                }
            } else {
                emitAlu(program.alu(slot));
            }
        }
    }

    std::string execCondition(const CfInstruction& cf) {
        switch (cf.opcode()) {
        case CfOpcode::CondExec: case CfOpcode::CondExecEnd:
            return std::string(cf.condition() ? "" : "!") + "BoolConstant(" + std::to_string(cf.boolAddress()) + ")";
        case CfOpcode::CondPredExec: case CfOpcode::CondPredExecEnd:
        case CfOpcode::CondExecPredClean: case CfOpcode::CondExecPredCleanEnd:
            return cf.condition() ? "p" : "!p";
        default:
            return "";
        }
    }

    // ---- Control flow ----------------------------------------------------------------

    bool needsStateMachine() {
        for (const auto& cf : program.controlFlow) {
            const auto op = cf.opcode();
            if (op == CfOpcode::CondJmp || op == CfOpcode::CondCall || op == CfOpcode::Return ||
                op == CfOpcode::LoopStart || op == CfOpcode::LoopEnd)
                return true;
        }
        return false;
    }

    void emitLinear() {
        currentCf = 0;
        for (const auto& cf : program.controlFlow) {
            ++currentCf;
            if (!cf.isExec()) continue;
            const std::string condition = execCondition(cf);
            if (!condition.empty()) {
                line("if (" + condition + ") {");
                ++indent;
            }
            emitExec(cf);
            if (!condition.empty()) {
                --indent;
                line("}");
            }
            if (cf.isEnd()) break;
        }
    }

    void emitStateMachine() {
        const int end = int(program.controlFlow.size());
        for (const auto& cf : program.controlFlow) {
            const auto op = cf.opcode();
            if (op == CfOpcode::CondJmp || op == CfOpcode::CondCall)
                firstBranchTarget = (std::min)(firstBranchTarget, cf.jumpAddress());
            if (op == CfOpcode::LoopStart || op == CfOpcode::LoopEnd)
                firstBranchTarget = (std::min)(firstBranchTarget, cf.loopAddress());
            if (op == CfOpcode::Return) firstBranchTarget = 0;  // returns go to unknown call sites
        }
        line("int pc = 0;");
        line("int loopCounter[4] = {0, 0, 0, 0}; int loopStep[4] = {0, 0, 0, 0}; int loopDepth = 0;");
        line("int callStack[4] = {0, 0, 0, 0}; int callDepth = 0;");
        line("[loop] for (int guard = 0; guard < 65536 && pc >= 0; ++guard) {");
        ++indent;
        line("switch (pc) {");
        for (int i = 0; i < end; ++i) {
            const auto& cf = program.controlFlow[size_t(i)];
            currentCf = uint32_t(i);
            line("case " + std::to_string(i) + ": {");
            ++indent;
            const std::string next = cf.isEnd() ? "-1" : std::to_string(i + 1);
            switch (cf.opcode()) {
            case CfOpcode::LoopStart: {
                const uint32_t loop = cf.loopId() + (pixel ? 16 : 0);
                line("uint loopConstant = LoopConstant(" + std::to_string(loop) + ");");
                line("loopDepth = min(loopDepth + 1, 3);");
                line("loopCounter[loopDepth] = int(loopConstant & 0xFF);");
                line("loopStep[loopDepth] = int(SignExtend((loopConstant >> 16) & 0xFF, 8));");
                line("aL = int((loopConstant >> 8) & 0xFF);");
                line("pc = loopCounter[loopDepth] == 0 ? " + std::to_string(cf.loopAddress()) + " : " + next + ";");
                break;
            }
            case CfOpcode::LoopEnd:
                line("loopCounter[loopDepth] -= 1; aL += loopStep[loopDepth];");
                line("if (loopCounter[loopDepth] > 0) { pc = " + std::to_string(cf.loopAddress()) +
                     "; } else { loopDepth = max(loopDepth - 1, 0); pc = " + next + "; }");
                break;
            case CfOpcode::CondJmp: case CfOpcode::CondCall: {
                std::string condition = "true";
                if (!cf.forceCall())
                    condition = cf.predicatedJump()
                                    ? std::string(cf.condition() ? "p" : "!p")
                                    : std::string(cf.condition() ? "" : "!") + "BoolConstant(" +
                                          std::to_string(cf.boolAddress()) + ")";
                if (cf.opcode() == CfOpcode::CondCall)
                    line("if (" + condition + ") { callStack[min(callDepth, 3)] = " + next +
                         "; callDepth += 1; pc = " + std::to_string(cf.jumpAddress()) + "; } else { pc = " + next + "; }");
                else
                    line("pc = (" + condition + ") ? " + std::to_string(cf.jumpAddress()) + " : " + next + ";");
                break;
            }
            case CfOpcode::Return:
                line("if (callDepth > 0) { callDepth -= 1; pc = callStack[min(callDepth, 3)]; } else { pc = -1; }");
                break;
            default:
                if (cf.isExec()) {
                    const std::string condition = execCondition(cf);
                    if (!condition.empty()) {
                        line("if (" + condition + ") {");
                        ++indent;
                    }
                    emitExec(cf);
                    if (!condition.empty()) {
                        --indent;
                        line("}");
                    }
                }
                line("pc = " + next + ";");
                break;
            }
            line("break;");
            --indent;
            line("}");
        }
        line("default: pc = -1; break;");
        line("}");
        --indent;
        line("}");
    }

    void emit() {
        body.clear();
        if (needsStateMachine()) emitStateMachine();
        else emitLinear();

        std::string hlsl = kPrelude;
        // Resources are known after the body has been generated.
        if (!pixel) {
            // Vertex data of every binding and the host index list share one buffer.
            hlsl += "ByteAddressBuffer guestData : register(t0);\n";
            for (const auto& [constant, slot] : vertexBindings)
                hlsl += "// vertex binding " + std::to_string(slot) + ": fetch constant " + std::to_string(constant) + "\n";
        }
        const uint32_t textureBase = pixel ? 0 : 8;
        for (const auto& [constant, slot] : textureBindings) {
            const uint32_t dimension = textureDimensions[slot];
            const char* type = dimension == 1 ? "Texture1D" : dimension == 2 ? "Texture2D"
                             : dimension == 3 ? "Texture3D" : "TextureCube";
            hlsl += std::string(type) + " t" + std::to_string(slot) + " : register(t" +
                    std::to_string(textureBase + slot) + ");  // texture fetch constant " + std::to_string(constant) +
                    "\nSamplerState s" + std::to_string(slot) + " : register(s" + std::to_string(slot) + ");\n";
        }
        hlsl += "struct Interpolators {\n    float4 position : SV_Position;\n    float4 t[16] : TEXCOORD0;\n};\n";
        if (!pixel) {
            hlsl += "Interpolators main(uint vertexId : SV_VertexID) {\n";
            hlsl += "    float4 r[64];\n    [unroll] for (int i = 0; i < 64; ++i) r[i] = 0.0;\n";
            hlsl += "    uint vertexIndex = g_draw.x == 0xFFFFFFFFu ? vertexId : guestData.Load(g_draw.x + vertexId * 4);\n";
            hlsl += "    r[0].x = float(vertexIndex + g_draw.y);\n";
            hlsl += "    float4 oPos = float4(0, 0, 0, 1), oPoint = 0.0, oDiscard = 0.0;\n";
            hlsl += "    float4 o[16];\n    [unroll] for (int j = 0; j < 16; ++j) o[j] = 0.0;\n";
            hlsl += "    int vfIndex = 0;\n";
        } else {
            hlsl += "struct PixelOutput {\n    float4 color[4] : SV_Target0;\n";
            if (result.info.writesDepth) hlsl += "    float depth : SV_Depth;\n";
            hlsl += "};\nPixelOutput main(Interpolators input) {\n";
            hlsl += "    float4 r[64];\n    [unroll] for (int i = 0; i < 16; ++i) r[i] = input.t[i];\n";
            hlsl += "    [unroll] for (int k = 16; k < 64; ++k) r[k] = 0.0;\n";
            hlsl += "    float4 oC[4];\n    [unroll] for (int j = 0; j < 4; ++j) oC[j] = 0.0;\n";
            hlsl += "    float4 oDepth = input.position.zzzz, oDiscard = 0.0;\n";
        }
        hlsl += "    float ps = 0.0;\n    bool p = false;\n    int a0 = 0;\n    int aL = 0;\n";
        hlsl += body;
        if (!pixel) {
            hlsl += "    Interpolators output;\n";
            hlsl += "    output.position = float4(oPos.xyz * g_positionScale.xyz + g_positionOffset.xyz * oPos.w, oPos.w);\n";
            hlsl += "    output.t = o;\n    return output;\n}\n";
        } else {
            hlsl += "    // Fixed-function alpha test: 0 never, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 always.\n";
            hlsl += "    uint alphaFunction = uint(g_alphaTest.x);\n";
            // Interpolation error must not decide the test: a constant 1.0 attribute
            // can arrive as 0.99999994 on some GPUs and fail "alpha >= 1".
            hlsl += "    float alpha = round(oC[0].w * 4096.0) / 4096.0, reference = g_alphaTest.y;\n";
            hlsl += "    bool alphaPass = alphaFunction == 7 || (alphaFunction == 1 && alpha < reference) ||\n"
                    "        (alphaFunction == 2 && alpha == reference) || (alphaFunction == 3 && alpha <= reference) ||\n"
                    "        (alphaFunction == 4 && alpha > reference) || (alphaFunction == 5 && alpha != reference) ||\n"
                    "        (alphaFunction == 6 && alpha >= reference);\n";
            hlsl += "    if (!alphaPass) discard;\n";
            hlsl += "    PixelOutput output;\n    output.color = oC;\n";
            if (result.info.writesDepth) hlsl += "    output.depth = oDepth.x;\n";
            hlsl += "    return output;\n}\n";
        }
        result.hlsl = std::move(hlsl);
    }
};

}  // namespace

TranslatedShader translate(const Program& program) {
    Emitter emitter(program);
    emitter.emit();
    return std::move(emitter.result);
}

}  // namespace tz::xenos
