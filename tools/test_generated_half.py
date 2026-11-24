"""Prepare an exhaustive test of the actual generated FLOAT16_2 instruction.

Run after recompile.py, then compile build/multiplayer/test_generated_half.cpp.
Uses Python's IEEE half conversion as an independent reference. Generated
instruction text stays in ignored build/, never in the repository.
"""
from pathlib import Path
import re
import struct

root = Path(__file__).resolve().parents[1]
source = (root / "build/generated/ppc_recomp.9.cpp").read_text()
match = re.search(r"// vupkd3d128 v0,v0,12\n(.*?)(?=\s*//)", source, re.S)
assert match and "__builtin_debugtrap" not in match[1], "FLOAT16_2 is still unimplemented"
out = root / "build/multiplayer"
out.mkdir(parents=True, exist_ok=True)
reference = b"".join(struct.pack("<f", struct.unpack("<e", struct.pack("<H", h))[0]) for h in range(65536))
(out / "half-reference.bin").write_bytes(reference)
test = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
union Vector { uint16_t u16[8]; uint32_t u32[4]; float f32[4]; };
struct Context { Vector v0; };
void unpack(Context& ctx) {
    Vector vTemp{};
INSTRUCTION
}
bool equal(uint32_t a, uint32_t b) {
    if ((b & 0x7FFFFFFF) > 0x7F800000) return (a & 0x7FFFFFFF) > 0x7F800000;
    return a == b;
}
int main() {
    uint32_t reference[65536];
    FILE* f = std::fopen("build/multiplayer/half-reference.bin", "rb");
    assert(f && std::fread(reference, 4, 65536, f) == 65536); std::fclose(f);
    for (uint32_t h = 0; h < 65536; ++h) {
        Context ctx{};
        ctx.v0.u16[1] = uint16_t(h); ctx.v0.u16[0] = uint16_t(h ^ 0x1234);
        unpack(ctx); // in-place source/destination aliasing, as in multiplayer
        assert(equal(ctx.v0.u32[3], reference[h]));
        assert(equal(ctx.v0.u32[2], reference[h ^ 0x1234]));
        assert(ctx.v0.u32[1] == 0 && ctx.v0.u32[0] == 0x3F800000);
    }
    std::puts("Generated half unpack: all 65536 values, both lanes and aliasing passed.");
}
'''
(out / "test_generated_half.cpp").write_text(test.replace("INSTRUCTION", match[1]), newline="\n")
print(out / "test_generated_half.cpp")
