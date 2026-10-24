// Native versions of hot guest functions (same results and side effects, faster host
// code; TENCHU_TRACE=verifynative also runs the original and reports differences,
// nonative disables them) and frame-rate fixes for code written for 30 fps.
#include "kernel.h"
#include "kernel/memory.h"
#include "host/settings.h"
#include "ppc_recomp_shared.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

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
        // An iteration (cursor -1) scans the table once and keeps the key's records
        // in table order; its next calls pick from them instead of rescanning.
        struct Match { uint32_t index; int32_t order; };
        thread_local std::vector<Match> matches;  // per thread, like the guest loop using it
        thread_local uint32_t cachedKey = 0, cachedRecords = 0, cachedCount = ~0u;
        const uint32_t count = load32(kTable);
        const int32_t after = int32_t(load32(kCursor));
        if (after == -1 || ctx.r3.u32 != cachedKey || records != cachedRecords || count != cachedCount) {
            matches.clear();
            const uint32_t key = _byteswap_ulong(ctx.r3.u32);  // compare in guest byte order
            const uint8_t* record = memory->host(records);
            for (uint32_t i = 0; i < count; ++i, record += 80)
                if (*reinterpret_cast<const uint32_t*>(record) == key)
                    matches.push_back({i, int32_t(_byteswap_ulong(*reinterpret_cast<const uint32_t*>(record + 4)))});
            cachedKey = ctx.r3.u32;
            cachedRecords = records;
            cachedCount = count;
        }
        for (const auto& match : matches) {
            if (match.order > after) {
                result = records + match.index * 80;
                cursor = match.order;
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

// Frame time (sub_822929D0 stores milliseconds per frame at 0x82B57824). Effect code
// rounds it to whole 30 Hz ticks, int(ms * 0.03 + 0.5), which is 0 at 120 fps (water
// ripples never expired), or to whole milliseconds. The midasm hooks in config/tenchuz.toml
// give those sites carried whole values whose running sum follows real time.
namespace {
std::atomic<float> g_quantizedStep{0.0f};
std::atomic<float> g_ticks30{0.0f};
}
extern "C" PPC_FUNC(__imp__sub_822929D0);
PPC_FUNC(sub_822929D0) {
    __imp__sub_822929D0(ctx, base);
    static double carry = 0.0;
    uint32_t bits = load32(0x82B57824);
    float step;
    std::memcpy(&step, &bits, 4);
    carry += step;
    const double whole = std::floor(carry);
    carry -= whole;
    g_quantizedStep.store(float(whole), std::memory_order_relaxed);
    // 30 Hz ticks for sites computing int(ms * 0.03 + 0.5): carried so that the ticks
    // follow real time at any rate (1 per frame at 30 fps as before; 0,0,0,1... at 120).
    static double ticks = 0.0;
    ticks += step * 0.03;
    const double tickCount = std::floor(ticks + 0.5);
    ticks -= tickCount;
    g_ticks30.store(float(tickCount), std::memory_order_relaxed);
}

// Before the fctiwz of int(ms * 0.03 + 0.5): the carried tick count (+0.25 truncates to it).
void FrameTicks30MidAsmHook(PPCRegister& f0) {
    static const bool off = traceEnabled("noquantstep");
    if (!off) f0.f64 = g_ticks30.load(std::memory_order_relaxed) + 0.25;
}

// After loads of the frame time that are truncated to whole milliseconds.
void QuantizedFrameStepMidAsmHook(PPCRegister& f0) {
    static const bool off = traceEnabled("noquantstep");
    if (!off) f0.f64 = g_quantizedStep.load(std::memory_order_relaxed);
}

// HUD indicator (sub_822BE208). Its fade rates are computed once from the tick count
// (64 or 32 per tick) and then applied every frame; with ticks they would latch 0 on
// most frames above 30 fps. These give the per-frame rate for the real frame time
// instead (16 and 8 at 120 fps; 64 and 32 at 30 fps as before).
namespace {
float frameMilliseconds() {
    const uint32_t bits = load32(0x82B57824);
    float ms;
    std::memcpy(&ms, &bits, 4);
    return ms;
}
}  // namespace

void IndicatorFadeInRateMidAsmHook(PPCRegister& r11) {
    r11.u64 = uint64_t((std::max)(1, int(64.0f * frameMilliseconds() * 0.03f + 0.5f)));
}

void IndicatorFadeOutRateMidAsmHook(PPCRegister& r9) {
    r9.u64 = uint64_t((std::max)(1, int(32.0f * frameMilliseconds() * 0.03f + 0.5f)));
}

// The display timer (+288) is pinned at 150 while the indicator fades out; a frame that
// adds no tick must not drop back to the "not expired" branch (which restores alpha).
void IndicatorHoldMidAsmHook(PPCRegister& r11, PPCRegister& r29) {
    if (r29.s32 == 0 && r11.s32 == 150) r11.s64 = 151;
}

// Wave water (sub_823C30E8, type 1): each wave's phase advances by a constant per
// frame (the type 2 water beside it uses the frame time). Scaled to the real frame
// time so the waves keep their 30 fps speed (unchanged at 30 fps).
void WavePhaseStepMidAsmHook(PPCRegister& f0) {
    f0.f64 *= frameMilliseconds() * 0.03;
}

// Front end (title screen, load prompt, main menu): the game mode's per-frame
// callbacks (task at the head of the scheduler, +24/+28). Their animations (the
// flashing "Press Start", the burning Z) advance per frame, so presents are held to
// 30 fps while they run (host::activeFrameRateLimit).
#define TZ_FRONT_END(address)                              extern "C" PPC_FUNC(__imp__sub_##address);             PPC_FUNC(sub_##address) {                                  host::noteFrontEndFrame();                             __imp__sub_##address(ctx, base);                   }
TZ_FRONT_END(822A5CB8)  // title screen
TZ_FRONT_END(822A2370)
TZ_FRONT_END(822A2CC0)  // "Load game?"
TZ_FRONT_END(822A55B8)  // main menu
TZ_FRONT_END(822A24B8)
