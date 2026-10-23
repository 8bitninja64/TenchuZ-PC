#include "ucode.h"
#include <cstdio>

namespace tz::xenos {

Program decode(ShaderType type, const std::vector<uint32_t>& words) {
    Program program;
    program.type = type;
    program.words = words;
    // Control flow occupies the start of the program; it ends at the first
    // EXEC_END. Instructions are packed two per three dwords.
    for (size_t pair = 0; pair * 3 + 2 < words.size(); ++pair) {
        const uint64_t w0 = words[pair * 3], w1 = words[pair * 3 + 1], w2 = words[pair * 3 + 2];
        const CfInstruction first{w0 | ((w1 & 0xFFFF) << 32)};
        const CfInstruction second{(w1 >> 16) | (w2 << 16)};
        program.controlFlow.push_back(first);
        if (first.isEnd()) break;
        program.controlFlow.push_back(second);
        if (second.isEnd()) break;
    }
    return program;
}

namespace {
const char* kVectorNames[] = {"add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor",
                              "mad", "cndeq", "cndge", "cndgt", "dp4", "dp3", "dp2add", "cube", "max4",
                              "setp_eq_push", "setp_ne_push", "setp_gt_push", "setp_ge_push", "kill_eq",
                              "kill_gt", "kill_ge", "kill_ne", "dst", "mova"};
const char* kScalarNames[] = {"adds", "adds_prev", "muls", "muls_prev", "muls_prev2", "maxs", "mins", "seqs",
                              "sgts", "sges", "snes", "frcs", "truncs", "floors", "exp", "logc", "log", "rcpc",
                              "rcpf", "rcp", "rsqc", "rsqf", "rsq", "movas", "movas_floor", "subs", "subs_prev",
                              "setp_eq", "setp_ne", "setp_gt", "setp_ge", "setp_inv", "setp_pop", "setp_clr",
                              "setp_rstr", "kills_eq", "kills_gt", "kills_ge", "kills_ne", "kills_one", "sqrt",
                              "op41", "mulsc0", "mulsc1", "addsc0", "addsc1", "subsc0", "subsc1", "sin", "cos",
                              "retain_prev"};
const char* kCfNames[] = {"nop", "exec", "exece", "cexec", "cexece", "cpexec", "cpexece", "loop", "endloop",
                          "ccall", "ret", "cjmp", "alloc", "cexec_pc", "cexece_pc", "vs_fetch_done"};

std::string swizzleText(uint32_t swizzle, int count) {
    // Component i reads source component (i + field) & 3.
    std::string text = ".";
    for (int i = 0; i < count; ++i) text += "xyzw"[((swizzle >> (i * 2)) + i) & 3];
    return text == ".xyzw" ? "" : text;
}

std::string maskText(uint32_t mask) {
    std::string text = ".";
    for (int i = 0; i < 4; ++i) text += mask & (1 << i) ? "xyzw"[i] : '_';
    return text;
}

std::string sourceText(const AluInstruction& alu, int source, bool scalar, int* constantOrdinal = nullptr) {
    const uint32_t reg = alu.sourceRegister(source);
    std::string text = alu.negate(source) ? "-" : "";
    if (!alu.sourceIsTemp(source) && constantOrdinal) {
        const bool relative = (*constantOrdinal)++ == 0 ? alu.const0RelAbs() : alu.const1RelAbs();
        if (relative) {
            text += std::string("c[") + std::to_string(reg) + (alu.addressAbsolute() ? "+a0]" : "+aL]");
            const uint32_t swizzle = alu.swizzle(source);
            return text + (scalar ? std::string(".") + "xyzw"[((swizzle >> 6) + 3) & 3] + "xyzw"[swizzle & 3]
                                  : swizzleText(swizzle, 4));
        }
    }
    if (alu.sourceIsTemp(source)) {
        text += (reg & 0x80 ? "|r" : "r") + std::to_string(reg & 0x3F) + (reg & 0x40 ? "[aL]" : "") +
                (reg & 0x80 ? "|" : "");
    } else {
        text += (alu.absConstants() ? "|c" : "c") + std::to_string(reg) + (alu.absConstants() ? "|" : "");
    }
    if (scalar) {
        const uint32_t swizzle = alu.swizzle(source);
        text += ".";
        text += "xyzw"[((swizzle >> 6) + 3) & 3];
        text += "xyzw"[swizzle & 3];
    } else {
        text += swizzleText(alu.swizzle(source), 4);
    }
    return text;
}

std::string destText(ShaderType type, uint32_t dest, bool exported) {
    if (!exported) return "r" + std::to_string(dest);
    if (type == ShaderType::Vertex) {
        if (dest == 62) return "oPos";
        if (dest == 63) return "oPts";
        if (dest < 16) return "o" + std::to_string(dest);
        return "export" + std::to_string(dest);
    }
    if (dest < 4) return "oC" + std::to_string(dest);
    if (dest == 61) return "oDepth";
    return "export" + std::to_string(dest);
}

int vectorOperandCount(VectorOpcode op) {
    switch (op) {
    case VectorOpcode::Frac:
    case VectorOpcode::Trunc:
    case VectorOpcode::Floor:
    case VectorOpcode::MovA:
        return 1;
    case VectorOpcode::MulAdd:
    case VectorOpcode::CndEq:
    case VectorOpcode::CndGe:
    case VectorOpcode::CndGt:
    case VectorOpcode::Dot2Add:
        return 3;
    default:
        return 2;
    }
}
}  // namespace

std::string disassemble(const Program& program) {
    std::string out;
    char line[256];
    for (size_t i = 0; i < program.controlFlow.size(); ++i) {
        const auto& cf = program.controlFlow[i];
        const auto op = cf.opcode();
        if (cf.isExec()) {
            snprintf(line, sizeof(line), "%3zu %s addr=%u count=%u%s\n", i, kCfNames[uint32_t(op)],
                     cf.execAddress(), cf.execCount(),
                     op == CfOpcode::Exec || op == CfOpcode::ExecEnd ? "" : (cf.condition() ? " if b/p" : " if !b/p"));
            out += line;
            for (uint32_t n = 0; n < cf.execCount(); ++n) {
                const uint32_t slot = cf.execAddress() + n;
                if (slot >= program.slotCount()) {
                    out += "      <slot out of range>\n";
                    continue;
                }
                if (cf.isFetch(n)) {
                    const auto fetch = program.fetch(slot);
                    std::string dest = "r" + std::to_string(fetch.destRegister()) + ".";
                    for (int c = 0; c < 4; ++c) dest += "xyzw01?_"[(fetch.destSwizzle() >> (c * 3)) & 7];
                    if (fetch.opcode() == FetchOpcode::VertexFetch) {
                        snprintf(line, sizeof(line),
                                 "      vfetch %s, r%u.%c, vf%u fmt=%u stride=%u off=%u%s%s%s\n", dest.c_str(),
                                 fetch.sourceRegister(), "xyzw"[fetch.vertexSourceComponent()],
                                 fetch.vertexConstant(), fetch.vertexFormat(), fetch.vertexStride(),
                                 fetch.vertexOffset(), fetch.vertexSigned() ? " signed" : "",
                                 fetch.vertexInteger() ? " int" : " norm", fetch.isMiniFetch() ? " mini" : "");
                    } else if (fetch.opcode() == FetchOpcode::TextureFetch) {
                        std::string coords;
                        for (int c = 0; c < 3; ++c) coords += "xyzw"[(fetch.textureSourceSwizzle() >> (c * 2)) & 3];
                        snprintf(line, sizeof(line), "      tfetch%uD %s, r%u.%s, tf%u%s\n",
                                 fetch.dimension() + 1, dest.c_str(), fetch.sourceRegister(), coords.c_str(),
                                 fetch.textureConstant(), fetch.dimension() == 3 ? " (cube)" : "");
                    } else {
                        snprintf(line, sizeof(line), "      fetch op%u %s, r%u\n", uint32_t(fetch.opcode()),
                                 dest.c_str(), fetch.sourceRegister());
                    }
                    out += line;
                    continue;
                }
                const auto alu = program.alu(slot);
                const auto vop = alu.vectorOpcode();
                const auto sop = alu.scalarOpcode();
                const char* pred = alu.predicated() ? (alu.predicateCondition() ? "(p) " : "(!p) ") : "";
                if (alu.vectorWriteMask() || uint32_t(vop) >= 20) {
                    std::string text = std::string(pred) +
                                       (uint32_t(vop) < 30 ? kVectorNames[uint32_t(vop)] : "vop?") +
                                       (alu.vectorClamp() ? "_sat " : " ") +
                                       destText(program.type, alu.vectorDest(), alu.exportData()) +
                                       maskText(alu.vectorWriteMask());
                    int constants = 0;
                    for (int s = 1; s <= vectorOperandCount(vop); ++s) text += ", " + sourceText(alu, s, false, &constants);
                    out += "      " + text + "\n";
                }
                if (sop != ScalarOpcode::RetainPrev) {  // a scalar op without a destination still sets ps
                    std::string operands;
                    if (alu.isScalarConstantOp()) {
                        // Constant component a, temporary component b.
                        const uint32_t swizzle = alu.swizzle(3);
                        operands = std::string(alu.negate(3) ? "-" : "") + "c" +
                                   std::to_string(alu.sourceRegister(3)) + "." + "xyzw"[((swizzle >> 6) + 3) & 3] +
                                   ", r" + std::to_string(alu.scalarConstantOpTemp()) + "." + "xyzw"[swizzle & 3];
                    } else {
                        operands = sourceText(alu, 3, true);
                    }
                    std::string text = std::string(pred) + "    + " +
                                       (uint32_t(sop) < 51 ? kScalarNames[uint32_t(sop)] : "sop?") +
                                       (alu.scalarClamp() ? "_sat " : " ") +
                                       destText(program.type, alu.scalarDest(), alu.exportData()) +
                                       maskText(alu.scalarWriteMask()) + ", " + operands;
                    out += "      " + text + "\n";
                }
            }
        } else if (op == CfOpcode::Alloc) {
            snprintf(line, sizeof(line), "%3zu alloc buffer=%u size=%u\n", i, cf.allocBuffer(), cf.allocSize());
            out += line;
        } else if (op == CfOpcode::Nop) {
            continue;
        } else {
            snprintf(line, sizeof(line), "%3zu %s addr=%u\n", i, kCfNames[uint32_t(op)], cf.jumpAddress());
            out += line;
        }
    }
    return out;
}

}  // namespace tz::xenos
