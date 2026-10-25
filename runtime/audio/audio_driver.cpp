// XAudio render driver: the title's mixer callback produces 256-sample frames of
// six planar big-endian float channels at 48 kHz, which play through an XAudio2
// source voice (Linux: an SDL3 audio stream). Without an audio device the callback is ticked at the hardware
// rate and its output dropped. TENCHU_NOAUDIO=1 forces the silent path;
// TENCHU_AUDIO_DUMP=<file.wav> also records the submitted frames.
#include "kernel/kernel.h"
#include "host/settings.h"
#include "host/overlay.h"
#include "guest/host.h"
#ifdef _WIN32
#include <windows.h>
#include <xaudio2.h>
#else
#include <SDL3/SDL.h>
#endif
#include <array>
#include <cstring>
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
#ifdef _WIN32
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out to avoid ksmedia.h's GUID linkage.
constexpr GUID kFloatSubFormat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
#endif

struct Client {
    std::atomic<uint32_t> callback{0};
    std::atomic<uint32_t> context{0};
    std::atomic<bool> active{false};
};
Client clients[8];
std::atomic<bool> started{false};

#ifdef _WIN32
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
        master->SetVolume(host::settings().mute ? 0.0f : host::settings().volume / 100.0f);
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
#else
// Linux: an SDL3 audio stream in the title's format (SDL's 5.1 order matches XAudio's).
struct Output {
    SDL_AudioStream* stream = nullptr;
    std::array<float, kFrameSamples * kChannels> buffer{};
    std::mutex mutex;

    bool open() {
        char value[8]{};
        if (GetEnvironmentVariableA("TENCHU_NOAUDIO", value, sizeof(value)) && value[0] == '1') return false;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            logf("[audio] SDL audio unavailable (%s); audio disabled", SDL_GetError());
            return false;
        }
        const SDL_AudioSpec spec{SDL_AUDIO_F32, int(kChannels), int(kRate)};
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) {
            logf("[audio] cannot open an audio device (%s); audio disabled", SDL_GetError());
            return false;
        }
        SDL_SetAudioStreamGain(stream, host::settings().mute ? 0.0f : host::settings().volume / 100.0f);
        SDL_ResumeAudioStreamDevice(stream);
        logf("[audio] SDL audio output ready (%s)", SDL_GetCurrentAudioDriver());
        return true;
    }
    uint32_t queued() {
        const int bytes = SDL_GetAudioStreamQueued(stream);
        return bytes > 0 ? uint32_t(bytes) / uint32_t(sizeof(buffer)) : 0;
    }
    void pause(bool paused) {
        std::lock_guard lock(mutex);
        if (paused) SDL_PauseAudioStreamDevice(stream);
        else SDL_ResumeAudioStreamDevice(stream);
    }
    void setVolume(float volume) {
        if (stream) SDL_SetAudioStreamGain(stream, volume);
    }
    void submit(uint32_t samples) {
        std::lock_guard lock(mutex);
        if (queued() >= kBufferCount - 1) return;  // device stalled: drop rather than queue forever
        for (uint32_t channel = 0; channel < kChannels; ++channel)
            for (uint32_t i = 0; i < kFrameSamples; ++i) {
                const uint32_t bits = load32(samples + (channel * kFrameSamples + i) * 4);
                std::memcpy(&buffer[i * kChannels + channel], &bits, 4);
            }
        record(buffer.data());
        SDL_PutAudioStreamData(stream, buffer.data(), int(sizeof(buffer)));
    }
#endif
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
#ifdef _WIN32
    void setVolume(float volume) {
        if (master) master->SetVolume(volume);
    }
    uint32_t queued() {
        XAUDIO2_VOICE_STATE state{};
        voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return state.BuffersQueued;
    }
    // While the menu pauses the game the device stops (queued sound waits too).
    void pause(bool paused) {
        std::lock_guard lock(mutex);
        if (paused) voice->Stop();
        else voice->Start();
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
#endif
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
    bool paused = false;
    for (;;) {
        // The menu pauses the game: no mixing and no sound until it closes.
        if (host::overlay::isOpen() != paused) {
            paused = !paused;
            if (outputReady) output.pause(paused);
        }
        if (paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            next = std::chrono::steady_clock::now();
            continue;
        }
        next += std::chrono::microseconds(5333);
        const auto now = std::chrono::steady_clock::now();
        if (next < now - std::chrono::milliseconds(50)) next = now;  // resync after a stall
        std::this_thread::sleep_until(next);
        if (outputReady && output.queued() >= kQueuedTarget) continue;
        runClients();
    }
}
}  // namespace

namespace tz::audio {
// Master volume (0-1) from the F1 menu.
void setOutputVolume(float volume) {
    std::lock_guard lock(output.mutex);
    output.setVolume(volume);
}
}  // namespace tz::audio

TZ_IMPORT(XAudioRegisterRenderDriverClient) {
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

TZ_IMPORT(XAudioUnregisterRenderDriverClient) {
    clients[ctx.r3.u32 & 7].active = false;
    ctx.r3.u64 = 0;
}

TZ_IMPORT(XAudioSubmitRenderDriverFrame) {
    // (driver, samples*)
    static std::atomic<uint64_t> submitted{0};
    if (submitted++ % 2000 == 0)
        TZ_TRACE("audio", "[audio] frame %llu from %08X", (unsigned long long)submitted.load(), ctx.r4.u32);
    if (outputReady && ctx.r4.u32) output.submit(ctx.r4.u32);
    ctx.r3.u64 = 0;
}

TZ_IMPORT(XAudioGetVoiceCategoryVolumeChangeMask) {
    store32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0;
}

TZ_IMPORT(XAudioGetVoiceCategoryVolume) {
    const float volume = 1.0f;
    uint32_t bits;
    std::memcpy(&bits, &volume, 4);
    store32(ctx.r4.u32, bits);
    ctx.r3.u64 = 0;
}

TZ_IMPORT(Refresh) { ctx.r3.u64 = 0; }
