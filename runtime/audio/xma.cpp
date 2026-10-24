// XMA decoder hardware: 320 64-byte context records in physical memory, driven
// through the APU registers at 0x7FEA0000 (kick, lock, clear bitmasks). Enabled
// contexts are decoded on a worker thread with FFmpeg's XMA2 decoder in raw-frame
// mode (tools/build_xma_codec.py) into their 16-bit big-endian PCM output rings.
// Layout and register semantics follow Xenia's XmaDecoder/XmaContext.
#include "audio/xma.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "guest/host.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using namespace tz;

namespace {
constexpr uint32_t kContextCount = 320;
constexpr uint32_t kContextSize = 64;
constexpr uint32_t kPacketBytes = 2048;
constexpr uint32_t kBlockBytes = 256;

// Register indices (dword offsets from 0x7FEA0000).
constexpr uint32_t kRegContextArrayAddress = 0x600;
constexpr uint32_t kRegCurrentContextIndex = 0x606;
constexpr uint32_t kRegNextContextIndex = 0x608;
constexpr uint32_t kRegKick = 0x650;   // ..0x659, one bit per context
constexpr uint32_t kRegLock = 0x690;   // ..0x699
constexpr uint32_t kRegClear = 0x6A0;  // ..0x6A9

// Context record fields (each dword big-endian in guest memory).
struct ContextData {
    uint32_t words[16];
    uint32_t get(int word, int shift, int bits) const { return (words[word] >> shift) & ((1u << bits) - 1); }
    void set(int word, int shift, int bits, uint32_t value) {
        const uint32_t mask = ((1u << bits) - 1) << shift;
        words[word] = (words[word] & ~mask) | ((value << shift) & mask);
    }
    uint32_t inputPackets(int buffer) const { return get(buffer, 0, 12); }
    bool inputValid(int buffer) const { return get(0, 20 + buffer, 1); }
    void setInputValid(int buffer, bool valid) { set(0, 20 + buffer, 1, valid); }
    uint32_t outputBlocks() const { return get(0, 22, 5); }
    uint32_t outputWriteOffset() const { return get(0, 27, 5); }
    void setOutputWriteOffset(uint32_t value) { set(0, 27, 5, value); }
    uint32_t sampleRate() const { return get(1, 27, 2); }
    bool stereo() const { return get(1, 29, 1); }
    bool outputValid() const { return get(1, 31, 1); }
    void setOutputValid(bool valid) { set(1, 31, 1, valid); }
    uint32_t inputReadOffset() const { return get(2, 0, 26); }
    void setInputReadOffset(uint32_t bits) { set(2, 0, 26, bits); }
    uint32_t loopCount() const { return get(0, 12, 8); }
    void setLoopCount(uint32_t value) { set(0, 12, 8, value); }
    uint32_t loopStart() const { return get(3, 0, 26); }
    uint32_t loopEnd() const { return get(4, 0, 26); }
    uint32_t currentBuffer() const { return get(4, 31, 1); }
    void setCurrentBuffer(uint32_t buffer) { set(4, 31, 1, buffer); }
    uint32_t inputPointer(int buffer) const { return words[5 + buffer]; }
    uint32_t outputPointer() const { return words[7]; }
    uint32_t outputReadOffset() const { return get(9, 0, 5); }
};

struct Context {
    std::mutex mutex;
    bool allocated = false;
    bool enabled = false;
    // Decoder state.
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    bool codecStereo = false;
    uint32_t nextPacket = 0;          // packet index within the current input buffer
    // The title positions a stream with the input read offset (bits); nextPacket
    // follows it when the stream starts and whenever the title moves it.
    bool positioned = false;
    uint32_t storedReadOffset = 0;    // the read offset this decoder last stored
    std::deque<int16_t> pending;      // decoded interleaved samples not yet written

    void resetDecoder() {
        if (codec) avcodec_free_context(&codec);
        pending.clear();
        nextPacket = 0;
        positioned = false;
    }
};

struct Decoder {
    uint32_t arrayAddress = 0;  // guest (0xA0000000 view) address of context 0
    std::array<Context, kContextCount> contexts;
    std::mutex mutex;  // allocation and wakeups
    std::condition_variable wake;
    bool workPending = false;
    uint32_t currentContext = 0;
    std::thread worker;
};

void workerMain();

// The context array exists from the first use of the decoder: titles read its
// address (register 0x600) before allocating contexts.
Decoder& decoder() {
    static Decoder* instance = [] {
        auto* d = new Decoder;
        d->arrayAddress = memory->allocatePhysical(kContextCount * kContextSize, 0x1000, 0xA0000000);
        std::memset(memory->host(d->arrayAddress), 0, kContextCount * kContextSize);
        d->worker = std::thread(workerMain);
        d->worker.detach();
        return d;
    }();
    return *instance;
}

bool openCodec(Context& context, bool stereo) {
    // FFmpeg logs every bad packet to stderr (blocking writes); TENCHU_TRACE=ffmpeg keeps them.
    static const bool quiet = [] {
        av_log_set_level(traceEnabled("ffmpeg") ? AV_LOG_WARNING : AV_LOG_FATAL);
        return true;
    }();
    (void)quiet;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    if (!codec) return false;
    context.codec = avcodec_alloc_context3(codec);
    av_channel_layout_default(&context.codec->ch_layout, stereo ? 2 : 1);
    context.codec->sample_rate = 48000;  // the decoder does not depend on it
    context.codec->block_align = kPacketBytes;
    // XMA2WAVEFORMATEX: one stream, no channel mask.
    context.codec->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
    context.codec->extradata_size = 34;
    context.codec->extradata[0] = 1;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "tenchu_raw_frames", "1", 0);
    const int result = avcodec_open2(context.codec, codec, &options);
    av_dict_free(&options);
    if (result < 0) {
        logf("[xma] cannot open decoder (%d)", result);
        avcodec_free_context(&context.codec);
        return false;
    }
    if (!context.packet) context.packet = av_packet_alloc();
    if (!context.frame) context.frame = av_frame_alloc();
    context.codecStereo = stereo;
    return true;
}

ContextData loadContext(uint32_t address) {
    ContextData data;
    for (int i = 0; i < 16; ++i) data.words[i] = load32(address + i * 4);
    return data;
}

// Decodes one packet's frames into context.pending. Returns false on failure.
bool decodePacket(Context& context, const uint8_t* bytes) {
    av_new_packet(context.packet, kPacketBytes);
    std::memcpy(context.packet->data, bytes, kPacketBytes);
    int result = avcodec_send_packet(context.codec, context.packet);
    av_packet_unref(context.packet);
    if (result < 0) return false;
    for (;;) {
        result = avcodec_receive_frame(context.codec, context.frame);
        if (result < 0) break;
        const int channels = context.frame->ch_layout.nb_channels;
        const auto* left = reinterpret_cast<const float*>(context.frame->extended_data[0]);
        const auto* right = channels > 1 ? reinterpret_cast<const float*>(context.frame->extended_data[1]) : left;
        for (int i = 0; i < context.frame->nb_samples; ++i) {
            context.pending.push_back(int16_t(std::clamp(left[i], -1.0f, 1.0f) * 32767.0f));
            if (context.codecStereo) context.pending.push_back(int16_t(std::clamp(right[i], -1.0f, 1.0f) * 32767.0f));
        }
        av_frame_unref(context.frame);
    }
    return true;
}

constexpr uint32_t kMaxPacketsPerPass = 64;

// One decoding pass over an enabled context: fill its output ring from its
// input buffers as far as both allow. Called with the context mutex held.
void work(uint32_t index, Context& context) {
    const uint32_t address = decoder().arrayAddress + index * kContextSize;
    ContextData data = loadContext(address);
    const ContextData original = data;
    TZ_TRACE("xmawork", "[xma] work %u: w0 %08X w1 %08X in %08X/%08X out %08X read %u write %u", index, data.words[0],
             data.words[1], data.inputPointer(0), data.inputPointer(1), data.outputPointer(), data.outputReadOffset(),
             data.outputWriteOffset());
    if (!data.outputValid() || !data.outputBlocks()) return;
    if (data.loopCount())
        TZ_TRACE("xmaloop", "[xma] context %u loops %u start %u end %u packets %u/%u", index, data.loopCount(),
                 data.loopStart(), data.loopEnd(), data.inputPackets(0), data.inputPackets(1));

    if (context.codec && context.codecStereo != data.stereo()) context.resetDecoder();
    if (!context.codec && !openCodec(context, data.stereo())) return;
    if (!context.positioned || data.inputReadOffset() != context.storedReadOffset) {
        // Start (or seek) where the title points: streams need not begin at packet 0.
        context.nextPacket = data.inputReadOffset() / (kPacketBytes * 8);
        context.positioned = true;
        avcodec_flush_buffers(context.codec);
    }

    const uint32_t capacity = data.outputBlocks() * kBlockBytes;
    uint32_t writeOffset = data.outputWriteOffset() * kBlockBytes;
    const uint32_t readOffset = data.outputReadOffset() * kBlockBytes;
    // Free space in whole blocks; equal offsets mean the ring is empty.
    uint32_t space = writeOffset < readOffset ? readOffset - writeOffset : capacity - writeOffset + readOffset;
    const uint32_t output = Memory::fromPhysical(data.outputPointer());
    const uint32_t sampleBytes = 2;
    const uint32_t blockSamples = kBlockBytes / sampleBytes;

    // Bounded work per pass (the context mutex is held), and loops that yield no
    // audio end instead of spinning: a loop region that decodes to nothing after the
    // flush at each wrap once kept this loop (and the title's XMACreateContext,
    // waiting for the mutex) busy forever.
    uint32_t packetsThisPass = 0, silentWraps = 0;
    size_t samplesAtWrap = SIZE_MAX;
    while (space >= kBlockBytes && packetsThisPass < kMaxPacketsPerPass) {
        if (context.pending.size() >= blockSamples) {
            for (uint32_t i = 0; i < blockSamples; ++i) {
                store16(output + writeOffset + i * 2, uint16_t(context.pending.front()));
                context.pending.pop_front();
            }
            writeOffset = (writeOffset + kBlockBytes) % capacity;
            space -= kBlockBytes;
            continue;
        }
        // Need more input: the current buffer, else the other one.
        uint32_t buffer = data.currentBuffer();
        if (!data.inputValid(buffer) || !data.inputPackets(buffer)) {
            buffer ^= 1;
            if (!data.inputValid(buffer) || !data.inputPackets(buffer)) break;  // starved
            data.setCurrentBuffer(buffer);
            context.nextPacket = 0;
        }
        if (context.nextPacket >= data.inputPackets(buffer)) {
            // A carried skip passes this whole buffer too.
            const uint32_t carry = context.nextPacket - data.inputPackets(buffer);
            data.setInputValid(buffer, false);
            data.setCurrentBuffer(buffer ^ 1);
            context.nextPacket = carry;
            continue;
        }
        const uint32_t packetAddress =
            Memory::fromPhysical(data.inputPointer(buffer)) + context.nextPacket * kPacketBytes;
        TZ_TRACE("xma", "[xma] context %u: packet %u of buffer %u (%u), pending %zu", index, context.nextPacket, buffer,
                 data.inputPackets(buffer), context.pending.size());
        const size_t before = context.pending.size();
        ++packetsThisPass;
        const bool decoded = decodePacket(context, memory->host(packetAddress));
        if (traceEnabled("xmapeak")) {
            int peak = 0;
            for (size_t i = before; i < context.pending.size(); ++i) peak = (std::max)(peak, std::abs(int(context.pending[i])));
            logf("[xma] context %u: +%zu samples, peak %d, header %08X", index, context.pending.size() - before, peak,
                 load32(packetAddress));
        }
        if (!decoded) {
            // Rate-limited: some titles' streams fail often, and each line is a blocking write.
            static std::atomic<uint64_t> failures{0};
            const uint64_t failure = ++failures;
            if (failure <= 20 || failure % 10000 == 0)
                logf("[xma] context %u: packet decode failed (%llu so far)", index, (unsigned long long)failure);
            // A fresh codec continues with the next packet: resetting the position too
            // retried the same packet forever (endless failures, lag).
            const uint32_t at = context.nextPacket;
            context.resetDecoder();
            context.nextPacket = at;
            context.positioned = true;
            if (!openCodec(context, data.stereo())) break;
        }
        // Sounds with more than two channels interleave several streams' packets: the
        // header's last byte counts the following packets that belong to other streams
        // (decoding them as this stream's gave garbage, decoder resets and lag).
        const uint32_t skip = memory->host(packetAddress)[3];
        context.nextPacket += 1 + skip;
        data.setInputReadOffset(context.nextPacket * kPacketBytes * 8 + 32);
        // Hardware looping (XMASetLoopData), at packet granularity: once the
        // packet holding the loop end is decoded, continue from the packet holding
        // the loop start. 255 loops is endless.
        const uint32_t loops = data.loopCount();
        const uint32_t loopEndPacket = data.loopEnd() / (kPacketBytes * 8);
        if (loops && context.nextPacket > loopEndPacket) {
            silentWraps = context.pending.size() == samplesAtWrap ? silentWraps + 1 : 0;
            samplesAtWrap = context.pending.size();
            if (silentWraps >= 2) {
                logf("[xma] context %u: loop %u-%u yields no audio; playing on without looping", index,
                     data.loopStart(), data.loopEnd());
                data.setLoopCount(0);
                continue;
            }
            if (loops != 255) data.setLoopCount(loops - 1);
            context.nextPacket = data.loopStart() / (kPacketBytes * 8);
            avcodec_flush_buffers(context.codec);
            data.setInputReadOffset(data.loopStart());
            continue;
        }
        if (context.nextPacket >= data.inputPackets(buffer)) {
            // Consumed: hand the buffer back and continue with the other one; a skip
            // that runs past the end carries into it (as Xenia does).
            const uint32_t carry = context.nextPacket - data.inputPackets(buffer);
            data.setInputValid(buffer, false);
            data.setCurrentBuffer(buffer ^ 1);
            context.nextPacket = carry;
            data.setInputReadOffset(carry * kPacketBytes * 8 + 32);
        }
    }
    data.setOutputWriteOffset(writeOffset / kBlockBytes);
    // A full ring goes back to the title until it consumes some of it.
    if (space < kBlockBytes) data.setOutputValid(false);

    // Store only the words the hardware owns (0-2, 4); the title may have
    // changed others concurrently.
    for (int word : {0, 1, 2, 4})
        if (data.words[word] != original.words[word]) {
            // Merge: keep bits the title owns from the current guest value.
            uint32_t current = load32(address + word * 4);
            uint32_t owned = 0;
            if (word == 0) owned = (0xFFu << 12) | (3u << 20) | (0x1Fu << 27);  // loops, input valid, write offset
            if (word == 1) owned = 1u << 31;                      // output valid
            if (word == 2) owned = 0x3FFFFFF;                     // input read offset
            if (word == 4) owned = 1u << 31;                      // current buffer
            store32(address + word * 4, (current & ~owned) | (data.words[word] & owned));
        }
    context.storedReadOffset = data.inputReadOffset();
}

void workerMain() {
    createSystemThread("xma-decoder");
    auto& d = decoder();
    for (;;) {
        {
            std::unique_lock lock(d.mutex);
            d.wake.wait_for(lock, std::chrono::milliseconds(2), [&] { return d.workPending; });
            d.workPending = false;
        }
        for (uint32_t i = 0; i < kContextCount; ++i) {
            auto& context = d.contexts[i];
            std::lock_guard lock(context.mutex);
            if (context.allocated && context.enabled) work(i, context);
        }
    }
}

void forEachBit(uint32_t registerIndex, uint32_t base, uint32_t value, void (*action)(uint32_t)) {
    for (uint32_t bit = 0; bit < 32; ++bit)
        if (value & (1u << bit)) {
            const uint32_t index = (registerIndex - base) * 32 + bit;
            if (index < kContextCount) action(index);
        }
}
}  // namespace

namespace {
uint32_t readRegisterValue(uint32_t address);
void writeRegisterValue(uint32_t address, uint32_t value);
}  // namespace

namespace tz::xma {

bool ownsAddress(uint32_t address) { return address >= 0x7FEA0000 && address < 0x7FEB0000; }

// The APU registers are little-endian: values cross the bus byte-swapped.
uint32_t readRegister(uint32_t address) { return _byteswap_ulong(readRegisterValue(address)); }

void writeRegister(uint32_t address, uint32_t value) { writeRegisterValue(address, _byteswap_ulong(value)); }

}  // namespace tz::xma

namespace {
uint32_t readRegisterValue(uint32_t address) {
    auto& d = decoder();
    const uint32_t index = (address & 0xFFFF) / 4;
    switch (index) {
    case kRegContextArrayAddress: return Memory::physicalAddress(d.arrayAddress);
    case kRegCurrentContextIndex: {
        // Titles wait for the hardware to move past a context; keep it moving.
        std::lock_guard lock(d.mutex);
        d.currentContext = (d.currentContext + 1) % kContextCount;
        return d.currentContext;
    }
    case kRegNextContextIndex: return (d.currentContext + 1) % kContextCount;
    default: return 0;
    }
}

void writeRegisterValue(uint32_t address, uint32_t value) {
    auto& d = decoder();
    const uint32_t index = (address & 0xFFFF) / 4;
    TZ_TRACE("xmareg", "[xma] write %08X = %08X", address, value);
    if (index >= kRegKick && index < kRegKick + 10) {
        forEachBit(index, kRegKick, value, [](uint32_t context) {
            auto& c = decoder().contexts[context];
            std::lock_guard lock(c.mutex);
            c.enabled = true;
        });
        {
            std::lock_guard lock(d.mutex);
            d.workPending = true;
        }
        d.wake.notify_one();
    } else if (index >= kRegLock && index < kRegLock + 10) {
        forEachBit(index, kRegLock, value, [](uint32_t context) {
            auto& c = decoder().contexts[context];
            std::lock_guard lock(c.mutex);  // waits for a pass in progress
            c.enabled = false;
        });
    } else if (index >= kRegClear && index < kRegClear + 10) {
        forEachBit(index, kRegClear, value, [](uint32_t context) {
            auto& c = decoder().contexts[context];
            std::lock_guard lock(c.mutex);
            c.resetDecoder();
            // Clearing resets the hardware-owned progress fields.
            const uint32_t address = decoder().arrayAddress + context * kContextSize;
            ContextData data = loadContext(address);
            data.setInputReadOffset(32);
            data.setOutputWriteOffset(0);
            store32(address + 8, data.words[2]);
            store32(address, data.words[0]);
        });
    }
}

}  // namespace

TZ_IMPORT(XMACreateContext) {
    // (context*) -> status
    auto& d = decoder();
    std::lock_guard lock(d.mutex);
    for (uint32_t i = 0; i < kContextCount; ++i) {
        auto& context = d.contexts[i];
        std::lock_guard contextLock(context.mutex);
        if (context.allocated) continue;
        context.allocated = true;
        context.enabled = false;
        context.resetDecoder();
        const uint32_t address = d.arrayAddress + i * kContextSize;
        std::memset(memory->host(address), 0, kContextSize);
        store32(ctx.r3.u32, address);
        TZ_TRACE("xma", "[xma] XMACreateContext -> %u (%08X)", i, address);
        ctx.r3.u64 = 0;
        return;
    }
    store32(ctx.r3.u32, 0);
    ctx.r3.u64 = 0x8007000E;  // E_OUTOFMEMORY
}

TZ_IMPORT(XMAReleaseContext) {
    auto& d = decoder();
    const uint32_t index = (ctx.r3.u32 - d.arrayAddress) / kContextSize;
    if (d.arrayAddress && ctx.r3.u32 >= d.arrayAddress && index < kContextCount) {
        auto& context = d.contexts[index];
        std::lock_guard lock(context.mutex);
        context.allocated = false;
        context.enabled = false;
        context.resetDecoder();
    }
    ctx.r3.u64 = 0;
}
