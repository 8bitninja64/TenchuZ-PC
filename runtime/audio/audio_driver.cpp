// XAudio render driver: the title's mixer callback produces 256-sample frames of
// six planar big-endian float channels at 48 kHz, which play through an XAudio2
// source voice. Without an audio device the callback is ticked at the hardware
// rate and its output dropped. TENCHU_NOAUDIO=1 forces the silent path;
// TENCHU_AUDIO_DUMP=<file.wav> also records the submitted frames.
#include "kernel/kernel.h"
#include "ppc_recomp_shared.h"
#include <windows.h>
#include <xaudio2.h>
#include <array>
#include <chrono>
#include <mutex>
#include <thread>

using namespace tz;

namespace {
constexpr uint32_t kChannels = 6;
constexpr uint32_t kFrameSamples = 256;
constexpr uint32_t kRate = 48000;
constexpr uint32_t kBufferCount = 16;
// Frames kept queued ahead of the device (5.33 ms each).
constexpr uint32_t kQueuedTarget = 4;
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out to avoid ksmedia.h's GUID linkage.
constexpr GUID kFloatSubFormat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

struct Client {
    std::atomic<uint32_t> callback{0};
    std::atomic<uint32_t> context{0};
    std::atomic<bool> active{false};
};
Client clients[8];
std::atomic<bool> started{false};

struct VoiceCallback : IXAudio2VoiceCallback {
    void STDMETHODCALLTYPE OnBufferEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) noexcept override {
        logf("[audio] voice error %08X", uint32_t(error));
    }
};

struct Output {
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;
    VoiceCallback callback;
    std::array<std::array<float, kFrameSamples * kChannels>, kBufferCount> buffers{};
    uint32_t nextBuffer = 0;
    std::mutex mutex;

    bool open() {
        char value[8]{};
        if (GetEnvironmentVariableA("TENCHU_NOAUDIO", value, sizeof(value)) && value[0] == '1') return false;
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return false;
        if (FAILED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
        if (FAILED(engine->CreateMasteringVoice(&master))) return fail("mastering voice");
        WAVEFORMATEXTENSIBLE format{};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = kChannels;
        format.Format.nSamplesPerSec = kRate;
        format.Format.wBitsPerSample = 32;
        format.Format.nBlockAlign = kChannels * 4;
        format.Format.nAvgBytesPerSec = kRate * kChannels * 4;
        format.Format.cbSize = sizeof(format) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                               SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        format.SubFormat = kFloatSubFormat;
        if (FAILED(engine->CreateSourceVoice(&voice, &format.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &callback)))
            return fail("source voice");
        voice->Start();
        logf("[audio] XAudio2 output ready");
        return true;
    }
    bool fail(const char* what) {
        logf("[audio] cannot create %s; audio disabled", what);
        if (engine) engine->Release();
        engine = nullptr;
        return false;
    }
    // Six-channel float WAV of everything submitted (debugging).
    FILE* dump = nullptr;
    uint32_t dumpedBytes = 0;
    void record(const float* frame) {
        static const bool enabled = [this] {
            char path[MAX_PATH]{};
            if (!GetEnvironmentVariableA("TENCHU_AUDIO_DUMP", path, sizeof(path))) return false;
            dump = fopen(path, "wb");
            return dump != nullptr;
        }();
        if (!enabled) return;
        const uint32_t bytes = kFrameSamples * kChannels * 4;
        dumpedBytes += bytes;
        // Rewrite the header each time so the file stays valid if the process exits.
        struct Header {
            char riff[4] = {'R', 'I', 'F', 'F'};
            uint32_t riffSize;
            char wave[4] = {'W', 'A', 'V', 'E'}, fmt[4] = {'f', 'm', 't', ' '};
            uint32_t fmtSize = 16;
            uint16_t formatTag = 3, channels = kChannels;
            uint32_t rate = kRate, byteRate = kRate * kChannels * 4;
            uint16_t blockAlign = kChannels * 4, bits = 32;
            char data[4] = {'d', 'a', 't', 'a'};
            uint32_t dataSize;
        } header;
        header.riffSize = 36 + dumpedBytes;
        header.dataSize = dumpedBytes;
        fseek(dump, 0, SEEK_SET);
        fwrite(&header, sizeof(header), 1, dump);
        fseek(dump, 0, SEEK_END);
        fwrite(frame, bytes, 1, dump);
        fflush(dump);
    }
    uint32_t queued() {
        XAUDIO2_VOICE_STATE state{};
        voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return state.BuffersQueued;
    }
    void submit(uint32_t samples) {
        std::lock_guard lock(mutex);
        if (queued() >= kBufferCount - 1) return;  // device stalled: drop rather than overwrite
        auto& buffer = buffers[nextBuffer++ % kBufferCount];
        // Planar big-endian channels to interleaved host floats.
        for (uint32_t channel = 0; channel < kChannels; ++channel)
            for (uint32_t i = 0; i < kFrameSamples; ++i) {
                const uint32_t bits = load32(samples + (channel * kFrameSamples + i) * 4);
                std::memcpy(&buffer[i * kChannels + channel], &bits, 4);
            }
        record(buffer.data());
        XAUDIO2_BUFFER submission{};
        submission.AudioBytes = UINT32(sizeof(buffer));
        submission.pAudioData = reinterpret_cast<const BYTE*>(buffer.data());
        voice->SubmitSourceBuffer(&submission);
    }
};
Output output;
bool outputReady = false;

void runClients() {
    for (auto& client : clients)
        if (client.active) callGuest(client.callback, {client.context.load()});
}

void audioMain() {
    createSystemThread("audio-driver");
    outputReady = output.open();
    // The callback only wakes the title's mixer thread, which submits a frame
    // later; tick at the hardware rate and skip ticks while the device queue is
    // full, so the device's consumption paces the mixer.
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        next += std::chrono::microseconds(5333);
        const auto now = std::chrono::steady_clock::now();
        if (next < now - std::chrono::milliseconds(50)) next = now;  // resync after a stall
        std::this_thread::sleep_until(next);
        if (outputReady && output.queued() >= kQueuedTarget) continue;
        runClients();
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

PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame) {
    // (driver, samples*)
    static std::atomic<uint64_t> submitted{0};
    if (submitted++ % 2000 == 0)
        TZ_TRACE("audio", "[audio] frame %llu from %08X", (unsigned long long)submitted.load(), ctx.r4.u32);
    if (outputReady && ctx.r4.u32) output.submit(ctx.r4.u32);
    ctx.r3.u64 = 0;
}

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

PPC_FUNC(__imp__Refresh) { ctx.r3.u64 = 0; }
