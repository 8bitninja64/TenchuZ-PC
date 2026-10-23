// Native versions of hot guest functions: same results and side effects, faster
// host code. TENCHU_TRACE=verifynative also runs the original and reports any
// difference; TENCHU_TRACE=nonative disables the replacements.
#include "kernel.h"
#include "kernel/memory.h"
#include "ppc_recomp_shared.h"

#include <cstdlib>

using namespace tz;

namespace {
bool nativeEnabled() {
    static const bool enabled = !traceEnabled("nonative");
    return enabled;
}
bool verifyNative() {
    static const bool verify = traceEnabled("verifynative");
    return verify;
}
}  // namespace

// Zone iterator: returns the first 80-byte record in the table at 0x833B1DD8
// ({u32 count, u32 records}) whose key (+0) matches r3 and whose order value (+4,
// signed) is greater than the cursor at 0x833B1ED4, and advances the cursor to it
// (-1 and null when there is none). Callers walk every record of a key one call at
// a time, so the original scan from the start dominated the frame thread (~1/3).
extern "C" PPC_FUNC(__imp__sub_823D4440);
PPC_FUNC(sub_823D4440) {
    if (!nativeEnabled()) return __imp__sub_823D4440(ctx, base);
    constexpr uint32_t kTable = 0x833B1DD8, kCursor = 0x833B1ED4;
    const uint32_t records = load32(kTable + 4);
    uint32_t result = 0;
    int32_t cursor = -1;
    bool writeCursor = true;
    if (!records) {
        writeCursor = false;  // the original returns without touching the cursor
    } else {
        const uint32_t count = load32(kTable);
        const int32_t after = int32_t(load32(kCursor));
        const uint32_t key = _byteswap_ulong(ctx.r3.u32);  // compare in guest byte order
        const uint8_t* record = memory->host(records);
        for (uint32_t i = 0; i < count; ++i, record += 80) {
            if (*reinterpret_cast<const uint32_t*>(record) != key) continue;
            const int32_t order = int32_t(_byteswap_ulong(*reinterpret_cast<const uint32_t*>(record + 4)));
            if (order > after) {
                result = records + i * 80;
                cursor = order;
                break;
            }
        }
    }
    if (verifyNative()) {
        const uint32_t key = ctx.r3.u32, before = load32(kCursor);
        __imp__sub_823D4440(ctx, base);
        const uint32_t expectedCursor = load32(kCursor);
        store32(kCursor, before);
        if (ctx.r3.u32 != result || (writeCursor && expectedCursor != uint32_t(cursor)))
            logf("[native] sub_823D4440 mismatch: key %08X native %08X/%d original %08X/%d", key, result,
                 cursor, ctx.r3.u32, int32_t(expectedCursor));
    }
    if (writeCursor) store32(kCursor, uint32_t(cursor));
    ctx.r3.u64 = result;
}
