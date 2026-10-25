// Guest MMIO through access faults. Recompiled loads and stores are plain host
// accesses (no per-access range checks); the GPU and APU register ranges are left
// unmapped, so the few accesses that reach them per frame fault here. The handler
// decodes the faulting x86 move, performs it on the device emulation and resumes
// after the instruction.
#include "kernel.h"
#include "kernel/fault.h"
#include "kernel/memory.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <cstdlib>
#include <cstring>

uint32_t PPCMmioRead32(uint32_t address);
void PPCMmioWrite32(uint32_t address, uint32_t value);

namespace tz {
namespace {

bool isMmio(uint32_t address) {
    return (address >= 0x7FC00000u && address < 0x7FD00000u) || (address >= 0x7FE00000u && address < 0x7FF00000u);
}

// A decoded register <-> memory move.
struct Move {
    unsigned length = 0;     // instruction bytes
    unsigned size = 0;       // access width in bytes
    bool store = false;
    bool immediate = false;  // store of an immediate
    uint64_t value = 0;      // immediate
    unsigned reg = 0;        // register operand
    bool highByte = false;   // AH/CH/DH/BH (8-bit, no REX)
    bool zeroExtend = false; // movzx
};

// Decodes mov/movzx forms with a memory operand (ModRM mod != 3). The address
// itself comes from the fault, so only the instruction length matters for it.
bool decode(const uint8_t* code, Move& move) {
    unsigned i = 0;
    bool operandSize16 = false;
    uint8_t rex = 0;
    for (;; ++i) {
        if (code[i] == 0x66) operandSize16 = true;
        else if (code[i] == 0xF2 || code[i] == 0xF3 || code[i] == 0x2E || code[i] == 0x3E) continue;
        else break;
    }
    if ((code[i] & 0xF0) == 0x40) rex = code[i++];
    const bool wide = rex & 8;
    unsigned immediateBytes = 0;
    const uint8_t opcode = code[i++];
    switch (opcode) {
    case 0x8B: move.size = wide ? 8 : operandSize16 ? 2 : 4; break;  // mov r, m
    case 0x89: move.size = wide ? 8 : operandSize16 ? 2 : 4; move.store = true; break;
    case 0x8A: move.size = 1; break;
    case 0x88: move.size = 1; move.store = true; break;
    case 0xC6: move.size = 1; move.store = move.immediate = true; immediateBytes = 1; break;
    case 0xC7:
        move.size = wide ? 8 : operandSize16 ? 2 : 4;
        move.store = move.immediate = true;
        immediateBytes = operandSize16 ? 2 : 4;
        break;
    case 0x0F: {
        const uint8_t second = code[i++];
        if (second != 0xB6 && second != 0xB7) return false;
        move.size = second == 0xB6 ? 1 : 2;
        move.zeroExtend = true;
        break;
    }
    default:
        return false;
    }
    const uint8_t modrm = code[i++];
    const unsigned mod = modrm >> 6, rm = modrm & 7;
    if (mod == 3) return false;
    move.reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
    move.highByte = move.size == 1 && !move.zeroExtend && !rex && move.reg >= 4 && !move.immediate;
    if (rm == 4) {
        const uint8_t sib = code[i++];
        if (mod == 0 && (sib & 7) == 5) i += 4;
    } else if (mod == 0 && rm == 5) {
        i += 4;  // RIP-relative
    }
    if (mod == 1) i += 1;
    if (mod == 2) i += 4;
    if (immediateBytes) {
        uint32_t imm = 0;
        std::memcpy(&imm, code + i, immediateBytes);
        if (immediateBytes == 4 && wide) move.value = uint64_t(int64_t(int32_t(imm)));
        else move.value = imm;
        i += immediateBytes;
    }
    move.length = i;
    return true;
}

bool mmioFault(fault::Info& info) {
    if (!memory) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory->base());
    const uintptr_t host = info.address;
    if (host < base || host >= base + 0x100000000ull) return false;
    const uint32_t address = uint32_t(host - base);
    if (!isMmio(address)) return false;
    const auto* code = reinterpret_cast<const uint8_t*>(*info.rip);
    Move move;
    if (!decode(code, move)) {
        logf("[mmio] cannot emulate the access to %08X at %p: %02X %02X %02X %02X %02X %02X", address, code, code[0],
             code[1], code[2], code[3], code[4], code[5]);
        return false;
    }
    uint64_t& reg = *info.regs[(move.highByte ? move.reg - 4 : move.reg) & 15];
    if (move.store) {
        // Registers hold the guest value byte-swapped (memory order): swap it back.
        const uint64_t raw = move.immediate ? move.value : (move.highByte ? reg >> 8 : reg);
        if (move.size == 4) PPCMmioWrite32(address, _byteswap_ulong(uint32_t(raw)));
        // Narrower and wider device writes were ignored before as well.
    } else {
        // Device registers are 32-bit; other widths read as zero (as before).
        const uint64_t raw = move.size == 4 ? _byteswap_ulong(PPCMmioRead32(address)) : 0;
        if (move.zeroExtend || move.size == 4) reg = uint32_t(raw);            // writes zero-extend
        else if (move.size == 8) reg = raw;
        else if (move.size == 2) reg = (reg & ~0xFFFFull) | (raw & 0xFFFF);
        else if (move.highByte) reg = (reg & ~0xFF00ull) | ((raw & 0xFF) << 8);
        else reg = (reg & ~0xFFull) | (raw & 0xFF);
    }
    *info.rip += move.length;
    return true;
}

}  // namespace

void installMmioFaultHandler() {
    fault::addHandler(mmioFault);
    // Null pointer reads return zero, as the per-access checks did.
#ifdef _WIN32
    VirtualAlloc(memory->base(), 0x10000, MEM_COMMIT, PAGE_READWRITE);
#else
    mprotect(memory->base(), 0x10000, PROT_READ | PROT_WRITE);
#endif
}

}  // namespace tz
