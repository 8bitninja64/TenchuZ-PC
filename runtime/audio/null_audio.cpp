// Null audio device: the render driver client is called at the hardware rate
// (256 samples at 48 kHz) and its output is discarded.
#include "kernel/kernel.h"
#include "ppc_recomp_shared.h"
#include <chrono>
#include <thread>

using namespace tz;

namespace {
struct Client {
    std::atomic<uint32_t> callback{0};
    std::atomic<uint32_t> context{0};
    std::atomic<bool> active{false};
};
Client clients[8];
std::atomic<bool> started{false};

void audioMain() {
    createSystemThread("audio-driver");
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        next += std::chrono::microseconds(5333);
        std::this_thread::sleep_until(next);
        for (auto& client : clients)
            if (client.active) callGuest(client.callback, {client.context.load()});
    }
}
}  // namespace

PPC_FUNC(__imp__XAudioRegisterRenderDriverClient) {
    // (callback* = {function, context}, driver*)
    for (uint32_t i = 0; i < 8; ++i) {
        if (clients[i].active) continue;
        clients[i].callback = load32(ctx.r3.u32);
        clients[i].context = load32(ctx.r3.u32 + 4);
        clients[i].active = true;
        store32(ctx.r4.u32, 0x41550000 | i);
        if (!started.exchange(true)) std::thread(audioMain).detach();
        ctx.r3.u64 = 0;
        return;
    }
    ctx.r3.u64 = 0x80070057;  // E_INVALIDARG
}

PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient) {
    clients[ctx.r3.u32 & 7].active = false;
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame) { ctx.r3.u64 = 0; }

PPC_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask) {
    store32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume) {
    const float volume = 1.0f;
    uint32_t bits;
    std::memcpy(&bits, &volume, 4);
    store32(ctx.r4.u32, bits);
    ctx.r3.u64 = 0;
}

// XMA decoder contexts are 64-byte hardware records in physical memory.
PPC_FUNC(__imp__XMACreateContext) {
    const uint32_t context = memory->allocatePhysical(0x1000, 0x1000, 0xA0000000);
    if (context) std::memset(memory->host(context), 0, 64);
    store32(ctx.r3.u32, context);
    ctx.r3.u64 = context ? 0 : 0x8007000E;
}

PPC_FUNC(__imp__XMAReleaseContext) {
    memory->free(ctx.r3.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__Refresh) { ctx.r3.u64 = 0; }
