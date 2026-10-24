// Temporary investigation hooks (enabled with TENCHU_TRACE=heap).
#include "kernel.h"
#include "host/settings.h"
#include "guest/host.h"
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


// RtlAllocateHeap(heap, flags, size)
TZ_OVERRIDE(82413CB0) {
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

// D3D functions that emit WAIT_REG_MEM packets (TENCHU_TRACE=d3dwait).
#define TZ_TRACE_WRAP(address) \
    TZ_OVERRIDE(address) {                                                                    \
        TZ_TRACE("d3dwait", "[d3d] sub_" #address " r3=%08X r4=%08X r5=%08X r6=%08X lr=%08X",     \
                 ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));               \
        __imp__sub_##address(ctx, base);                                                         \
    }
TZ_TRACE_WRAP(8241BF50)
TZ_TRACE_WRAP(8241D770)
TZ_TRACE_WRAP(8241E768)
TZ_TRACE_WRAP(82426498)
TZ_TRACE_WRAP(8242AC58)
TZ_TRACE_WRAP(8242ED10)

// Present (device): the presentation interval at device+13248 sets how many
// vblanks a frame stays on screen (the title uses 2: 30 fps). Unlocked frame
// rates use 1; TENCHU_PRESENT_INTERVAL overrides it.
TZ_OVERRIDE(8241BDE0) {
    static const int interval = [] {
        char value[16]{};
        if (GetEnvironmentVariableA("TENCHU_PRESENT_INTERVAL", value, sizeof(value))) return atoi(value);
        return host::settings().frameRateLimit == 30 ? -1 : 1;
    }();
    if (interval >= 0) store32(ctx.r3.u32 + 13248, uint32_t(interval));
    __imp__sub_8241BDE0(ctx, base);
}

// D3D segment kicks (TENCHU_TRACE=kickseg): (device, segments*, count).
TZ_OVERRIDE(824262B8) {
    if (traceEnabled("kickseg")) {
        for (uint32_t i = 0; i < ctx.r5.u32 && i < 4; ++i)
            logf("[kickseg] %-20s lr %08X segment %08X size %X", currentThread()->name.c_str(), uint32_t(ctx.lr),
                 load32(ctx.r4.u32 + i * 8 + 4), load32(ctx.r4.u32 + i * 8));
    }
    __imp__sub_824262B8(ctx, base);
}

// Scratch-1 release paths (TENCHU_TRACE=release).
namespace {
uint32_t scratchOne() { return load32(0xE000B004); }
}
TZ_OVERRIDE(8241BD50) {
    const uint32_t before = scratchOne(), param = ctx.r3.u32;
    const uint32_t device = load32(load32(0x82000800));  // *VdGlobalDevice
    const uint32_t vcounter = PPCMmioRead32(0x7FC80000 + 0x194C * 4);
    __imp__sub_8241BD50(ctx, base);
    TZ_TRACE("release", "[release] vsync callback param %08X vcounter %u dev %u/%u -> %u scratch1 %X -> %X", param,
             vcounter, load32(device + 15124), load32(device + 15128), load32(device + 15132), before, scratchOne());
}
TZ_OVERRIDE(82425D88) {
    const uint32_t before = scratchOne(), source = ctx.r3.u32;
    __imp__sub_82425D88(ctx, base);
    const uint32_t after = scratchOne();
    if (before != after) TZ_TRACE("release", "[release] interrupt source %u scratch1 %X -> %X", source, before, after);
}

// Recorded command list interpreter (TENCHU_TRACE=cmdlist): (list*, state*).
TZ_OVERRIDE(824318A8) {
    TZ_TRACE("cmdlist", "[cmdlist] run list %08X state %08X first %08X lr %08X", ctx.r3.u32, ctx.r4.u32,
             load32(ctx.r3.u32), uint32_t(ctx.lr));
    __imp__sub_824318A8(ctx, base);
}
