// Temporary investigation hooks (enabled with TENCHU_TRACE=heap).
#include "kernel.h"
#include "ppc_recomp_shared.h"
#include <set>

using namespace tz;

namespace {
// XAPI heap: 128 free list heads (flink, blink) at heap + 0x180.
int firstBrokenList(uint32_t heap) {
    for (uint32_t list = 0; list < 128; ++list) {
        const uint32_t head = heap + 0x180 + list * 8;
        if (load32(head) == 0 || load32(head + 4) == 0) return int(list);
    }
    return -1;
}
std::mutex heapMutex;
std::set<uint32_t> heaps;
}  // namespace

extern "C" PPC_FUNC(__imp__sub_82413CB0);

// RtlAllocateHeap(heap, flags, size)
PPC_FUNC(sub_82413CB0) {
    if (traceEnabled("heap")) {
        const uint32_t heap = ctx.r3.u32;
        std::lock_guard lock(heapMutex);
        if (heaps.insert(heap).second) logf("[heap] first allocation from heap %08X (lr %08X)", heap, uint32_t(ctx.lr));
        for (uint32_t known : heaps) {
            const int broken = firstBrokenList(known);
            if (broken < 0) continue;
            logf("[heap] heap %08X list %d is broken at allocation(heap %08X, size %X) lr %08X", known, broken, heap,
                 ctx.r5.u32, uint32_t(ctx.lr));
            const uint32_t count = (std::min)(ctx.traceIndex, 64u);
            std::string trace;
            char cell[12];
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t entry = ctx.trace[(ctx.traceIndex - count + i) & 63];
                if (entry >= 0x82653F00 && entry < 0x82654C00) continue;  // register save helpers
                snprintf(cell, sizeof(cell), " %08X", entry);
                trace += cell;
            }
            logf("[heap] recent:%s", trace.c_str());
            heaps.erase(known);
            break;
        }
    }
    __imp__sub_82413CB0(ctx, base);
}
