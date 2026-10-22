// Null graphics device: emulates enough of the Xenos command processor for
// the XDK Direct3D library to run (ring buffer, fences, interrupts, vblank)
// without drawing anything.
#include "kernel/kernel.h"
#include "gpu/d3d11/renderer.h"
#include "ppc_recomp_shared.h"
#include <array>
#include <filesystem>
#include <set>
#include <intrin.h>
#include <chrono>
#include <thread>

using namespace tz;

namespace {

// Register indices (dword offsets from 0x7FC80000).
enum : uint32_t {
    kRegRbRptr = 0x01C4,
    kRegRbWptr = 0x01C5,
    kRegScratchUmsk = 0x01DC,
    kRegScratchAddr = 0x01DD,
    kRegScratch0 = 0x0578,
    kRegCoherStatusHost = 0x0A31,
    kRegEdramTiming = 0x0F00,
    kRegBcControl = 0x0F01,
    kRegD1ModeVCounter = 0x194C,
    kRegInterruptStatus = 0x1951,
    kRegD1ModeViewportSize = 0x1961,
};

constexpr uint32_t kGpuMmioBase = 0x7FC80000;

struct Gpu {
    std::array<std::atomic<uint32_t>, 0x10000> registers{};
    std::mutex mutex;
    std::condition_variable wake;
    uint32_t ringBase = 0;       // physical
    uint32_t ringDwords = 0;
    uint32_t readIndex = 0;
    uint32_t rptrWriteback = 0;  // physical, 0 = disabled
    std::atomic<uint32_t> interruptCallback{0};
    std::atomic<uint32_t> interruptData{0};
    std::mutex interruptMutex;
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> draws{0};
    uint64_t exitAfterFrames = 0;
    std::atomic<int64_t> lastVblankNs{0};
    std::atomic<uint32_t> vblanks{0};
    // Tiling predication: packets with header bit 0 run only when
    // (binSelect & binMask) != 0.
    uint64_t binMask = ~0ull;
    uint64_t binSelect = ~0ull;
    bool started = false;
};
Gpu gs;  // command processor and device state

// Host renderer, owned by the command processor thread.
gpu::D3D11Renderer renderer;
bool rendererFailed = false;

void presentFrame(uint32_t frontBuffer) {
    if (!renderer.ready() && !rendererFailed) {
        if (HWND window = gpu::outputWindow()) rendererFailed = !renderer.initialize(window);
    }
    if (renderer.ready()) renderer.present(nullptr);
    (void)frontBuffer;
}

// VdSwap writes this private packet into the slot D3D reserves for the
// kernel, so the frame is presented when the GPU reaches it.
constexpr uint32_t kSwapOpcode = 0x64;
constexpr uint32_t kSwapMagic = 0x53574150;  // 'SWAP'

// ---- Shader capture -------------------------------------------------------------
// Microcode arrives through IM_LOAD (pointer) or IM_LOAD_IMMEDIATE (inline).
struct ShaderCapture {
    std::mutex mutex;
    std::set<uint64_t> seen;
    std::filesystem::path dumpDirectory;
    uint64_t current[2] = {};  // vertex, pixel
};
ShaderCapture shaders;

uint64_t hashWords(const std::vector<uint32_t>& words) {
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t word : words) hash = (hash ^ word) * 1099511628211ull;
    return hash;
}

void captureShader(uint32_t type, std::vector<uint32_t> words) {
    const uint64_t hash = hashWords(words);
    std::lock_guard lock(shaders.mutex);
    shaders.current[type & 1] = hash;
    if (!shaders.seen.insert(hash).second || shaders.dumpDirectory.empty()) return;
    char name[64];
    snprintf(name, sizeof(name), "%s_%016llX.bin", type & 1 ? "ps" : "vs", (unsigned long long)hash);
    FILE* file = _wfopen((shaders.dumpDirectory / name).c_str(), L"wb");
    if (!file) return;
    // Stored big-endian, as in guest memory.
    for (uint32_t& word : words) word = _byteswap_ulong(word);
    fwrite(words.data(), 4, words.size(), file);
    fclose(file);
}
// Guest address of the packet being executed, and indirect buffer depth.
thread_local uint32_t packetAddress = 0;
thread_local int packetDepth = 0;
thread_local uint32_t ibStack[8] = {};

// Per-frame command processor time accounting (TENCHU_TRACE=gputime).
std::atomic<int64_t> waitRegMemNs{0}, waitMemNs{0}, interruptNs{0}, idleNs{0};
int64_t nanosSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

uint32_t gpuSwap(uint32_t value, uint32_t endian) {
    switch (endian & 3) {
    case 1: return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);  // 8in16
    case 2: return _byteswap_ulong(value);                                     // 8in32
    case 3: return (value << 16) | (value >> 16);                               // 16in32
    default: return value;
    }
}

// Command-processor memory accesses use physical addresses with the endian
// mode in the low two bits.
void writePhysical(uint32_t address, uint32_t value) {
    TZ_TRACE("cp", "[cp] write %08X = %08X", address, value);
    *memory->host<uint32_t>(Memory::fromPhysical(address & ~3u)) = gpuSwap(value, address & 3);
}
uint32_t readPhysical(uint32_t address) {
    return gpuSwap(*memory->host<uint32_t>(Memory::fromPhysical(address & ~3u)), address & 3);
}

// The handler runs as hardware thread `processor` (PCR +0x10C); D3D uses it to
// acknowledge per-CPU interrupt requests.
void dispatchInterrupt(uint32_t source, uint32_t processor) {
    const uint32_t callback = gs.interruptCallback.load();
    if (source != 0) TZ_TRACE("irq", "[irq] source %u -> callback %08X", source, callback);
    if (!callback) return;
    std::lock_guard lock(gs.interruptMutex);
    *memory->host(currentContext().r13.u32 + 0x10C) = uint8_t(processor);
    callGuest(callback, {source, gs.interruptData.load()});
    if (source != 0) TZ_TRACE("irq", "[irq] source %u handled; scratch0 memory %08X", source,
                              load32(Memory::fromPhysical(gs.registers[kRegScratchAddr])));
}

void writeRegister(uint32_t index, uint32_t value) {
    index &= 0xFFFF;
    if (index == kRegScratchUmsk || index == kRegScratchAddr || (index >= kRegScratch0 && index < kRegScratch0 + 8))
        TZ_TRACE("scratch", "[cp] reg %04X = %08X (packet %08X depth %d, IBs %08X %08X)", index, value, packetAddress,
                 packetDepth, ibStack[0], ibStack[1]);
    gs.registers[index] = value;
    if (index >= kRegScratch0 && index < kRegScratch0 + 8) {
        const uint32_t slot = index - kRegScratch0;
        if (gs.registers[kRegScratchUmsk] & (1u << slot))
            store32(Memory::fromPhysical(gs.registers[kRegScratchAddr] + slot * 4), value);
    } else if (index == kRegCoherStatusHost) {
        // Cache flushes complete immediately.
        gs.registers[index] = value & ~0x80000000u;
    }
}

bool compare(uint32_t function, uint32_t value, uint32_t reference) {
    switch (function & 7) {
    case 0: return false;
    case 1: return value < reference;
    case 2: return value <= reference;
    case 3: return value == reference;
    case 4: return value != reference;
    case 5: return value >= reference;
    case 6: return value > reference;
    default: return true;
    }
}

// Sequential dword reader over the ring (wrapping) or an indirect buffer.
struct Reader {
    uint32_t base;     // guest address
    uint32_t dwords;   // buffer size, for wrapping (0 = linear)
    uint32_t index;
    uint32_t end;
    bool ring;
    uint32_t available() const {
        return ring ? (end + dwords - index) % dwords : end - index;
    }
    uint32_t read() {
        const uint32_t value = load32(base + index * 4);
        index = ring ? (index + 1) % dwords : index + 1;
        return value;
    }
    void skip(uint32_t count) {
        index = ring ? (index + count) % dwords : index + count;
    }
};

void execute(Reader& reader, int depth);

void executeType3(Reader& reader, uint32_t header, int depth) {
    const uint32_t opcode = (header >> 8) & 0x7F;
    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
    if ((header & 1) && !(gs.binSelect & gs.binMask)) {
        reader.skip(count);
        return;
    }
    switch (opcode) {
    case 0x50:  // SET_BIN_MASK
        gs.binMask = reader.read();
        gs.binMask |= uint64_t(reader.read()) << 32;
        reader.skip(count - 2);
        break;
    case 0x51:  // SET_BIN_SELECT
        gs.binSelect = reader.read();
        gs.binSelect |= uint64_t(reader.read()) << 32;
        reader.skip(count - 2);
        break;
    case 0x60:  // SET_BIN_MASK_LO
        gs.binMask = (gs.binMask & 0xFFFFFFFF00000000ull) | reader.read();
        reader.skip(count - 1);
        break;
    case 0x61:  // SET_BIN_MASK_HI
        gs.binMask = (gs.binMask & 0xFFFFFFFFull) | uint64_t(reader.read()) << 32;
        reader.skip(count - 1);
        break;
    case 0x62:  // SET_BIN_SELECT_LO
        gs.binSelect = (gs.binSelect & 0xFFFFFFFF00000000ull) | reader.read();
        reader.skip(count - 1);
        break;
    case 0x63:  // SET_BIN_SELECT_HI
        gs.binSelect = (gs.binSelect & 0xFFFFFFFFull) | uint64_t(reader.read()) << 32;
        reader.skip(count - 1);
        break;
    case 0x3F:    // INDIRECT_BUFFER
    case 0x37: {  // INDIRECT_BUFFER_PFD
        const uint32_t address = reader.read();
        const uint32_t size = reader.read() & 0xFFFFF;
        reader.skip(count - 2);
        if (traceEnabled("ib") && depth == 0) {
            uint32_t hash = 2166136261u;
            for (uint32_t i = 0; i < size; ++i) hash = (hash ^ load32(Memory::fromPhysical(address) + i * 4)) * 16777619u;
            logf("[cp] IB %08X size %X hash %08X from ring index %X", address, size, hash, (packetAddress - reader.base) / 4);
        }
        if (depth < 8) ibStack[depth] = address;
        if (depth > 4) fatal("Indirect buffers nested too deeply");
        Reader indirect{Memory::fromPhysical(address), 0, 0, size, false};
        execute(indirect, depth + 1);
        break;
    }
    case 0x3C: {  // WAIT_REG_MEM
        const uint32_t info = reader.read(), poll = reader.read(), reference = reader.read();
        const uint32_t mask = reader.read();
        reader.skip(count - 4);
        TZ_TRACE("cp", "[cp] wait %s %08X == fn%u ref %08X mask %08X", info & 0x10 ? "mem" : "reg", poll, info & 7,
                 reference, mask);
        const auto start = std::chrono::steady_clock::now();
        bool warned = false;
        for (uint32_t attempt = 0;; ++attempt) {
            const uint32_t value = info & 0x10 ? readPhysical(poll) : gs.registers[poll & 0xFFFF].load();
            if (compare(info, value & mask, reference)) break;
            // Handshakes with the CPU usually resolve within microseconds:
            // spin, then yield, and only sleep for long waits.
            if (attempt < 256) {
                _mm_pause();
                continue;
            }
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (!warned && elapsed > std::chrono::seconds(2)) {
                logf("[gpu] WAIT_REG_MEM stalled: %s %08X value %08X ref %08X mask %08X fn %u",
                     info & 0x10 ? "mem" : "reg", poll, value, reference, mask, info & 7);
                warned = true;
            }
            if (elapsed < std::chrono::milliseconds(4)) SwitchToThread();
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const int64_t waited = nanosSince(start);
        (info & 0x10 ? waitMemNs : waitRegMemNs) += waited;
        if (waited > 5000000)
            TZ_TRACE("slowwait", "[cp] slow wait %.1f ms: %s %08X ref %08X", waited / 1e6, info & 0x10 ? "mem" : "reg",
                     poll, reference);
        break;
    }
    case 0x21: {  // REG_RMW
        const uint32_t info = reader.read(), andMask = reader.read(), orMask = reader.read();
        reader.skip(count - 3);
        const uint32_t index = info & 0x1FFF;
        const uint32_t andValue = info >> 31 ? gs.registers[andMask & 0x1FFF].load() : andMask;
        const uint32_t orValue = (info >> 30) & 1 ? gs.registers[orMask & 0x1FFF].load() : orMask;
        writeRegister(index, (gs.registers[index] & andValue) | orValue);
        break;
    }
    case 0x45: {  // COND_WRITE
        const uint32_t info = reader.read(), poll = reader.read(), reference = reader.read();
        const uint32_t mask = reader.read(), target = reader.read(), data = reader.read();
        reader.skip(count - 6);
        const uint32_t value = info & 0x10 ? readPhysical(poll) : gs.registers[poll & 0xFFFF].load();
        if (compare(info, value & mask, reference)) {
            if (info & 0x100) writePhysical(target, data);
            else writeRegister(target, data);
        }
        break;
    }
    case 0x58: {  // EVENT_WRITE_SHD
        const uint32_t initiator = reader.read(), address = reader.read(), value = reader.read();
        reader.skip(count - 3);
        if (initiator >> 31) TZ_TRACE("shd", "[cp] EVENT_WRITE_SHD counter to %08X (vblank %u)", address, gs.vblanks.load());
        // Bit 31 of the initiator writes the vblank counter instead of the value.
        writePhysical(address, initiator >> 31 ? gs.vblanks.load() : value);
        break;
    }
    case 0x3D: {  // MEM_WRITE
        const uint32_t address = reader.read() ;
        for (uint32_t i = 1; i < count; ++i) writePhysical(address + (i - 1) * 4, reader.read());
        break;
    }
    case 0x54: {  // INTERRUPT: one dispatch per CPU in the mask
        const uint32_t cpuMask = reader.read();
        reader.skip(count - 1);
        TZ_TRACE("cp", "[cp] interrupt mask %02X", cpuMask);
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t cpu = 0; cpu < 6; ++cpu)
            if (cpuMask & (1u << cpu)) dispatchInterrupt(1, cpu);
        interruptNs += nanosSince(start);
        break;
    }
    case 0x2D:    // SET_CONSTANT
    case 0x55: {  // SET_CONSTANT2
        const uint32_t offset = reader.read();
        uint32_t index = offset & 0x7FF;
        if (opcode == 0x2D) {
            static constexpr uint32_t bases[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
            const uint32_t type = (offset >> 16) & 0xFF;
            index += type < 5 ? bases[type] : 0;
        } else {
            index = offset & 0xFFFF;
        }
        for (uint32_t i = 1; i < count; ++i) writeRegister(index + i - 1, reader.read());
        break;
    }
    case 0x27: {  // IM_LOAD: microcode at a physical address
        const uint32_t addressType = reader.read(), startSize = reader.read();
        reader.skip(count - 2);
        std::vector<uint32_t> words(startSize & 0xFFFF);
        const uint32_t source = Memory::fromPhysical(addressType & ~3u);
        for (size_t i = 0; i < words.size(); ++i) words[i] = load32(source + uint32_t(i) * 4);
        captureShader(addressType & 3, std::move(words));
        break;
    }
    case 0x2B: {  // IM_LOAD_IMMEDIATE: microcode follows
        const uint32_t type = reader.read(), startSize = reader.read();
        const uint32_t size = (std::min)(startSize & 0xFFFF, count - 2);
        std::vector<uint32_t> words(size);
        for (uint32_t& word : words) word = reader.read();
        reader.skip(count - 2 - size);
        captureShader(type & 3, std::move(words));
        break;
    }
    case kSwapOpcode: {  // private: present the front buffer
        const uint32_t magic = reader.read(), frontBuffer = reader.read();
        reader.skip(count - 2);
        if (magic == kSwapMagic) presentFrame(frontBuffer);
        break;
    }
    case 0x22:  // DRAW_INDX
    case 0x36:  // DRAW_INDX_2
        ++gs.draws;
        reader.skip(count);
        break;
    default:
        TZ_TRACE("cp", "[cp] type3 opcode %02X count %u", opcode, count);
        reader.skip(count);
        break;
    }
}

void publishReadPointer(uint32_t index) {
    gs.registers[kRegRbRptr] = index;
    if (gs.rptrWriteback) store32(Memory::fromPhysical(gs.rptrWriteback), index);
}

void execute(Reader& reader, int depth) {
    while (reader.available()) {
        if (reader.ring) publishReadPointer(reader.index);
        packetAddress = reader.base + reader.index * 4;
        packetDepth = depth;
        const uint32_t header = reader.read();
        switch (header >> 30) {
        case 0: {  // register writes
            const uint32_t index = header & 0x7FFF, count = ((header >> 16) & 0x3FFF) + 1;
            const bool single = (header >> 15) & 1;
            for (uint32_t i = 0; i < count; ++i) writeRegister(single ? index : index + i, reader.read());
            break;
        }
        case 1: {  // two register writes
            const uint32_t first = reader.read(), second = reader.read();
            writeRegister(header & 0x7FF, first);
            writeRegister((header >> 11) & 0x7FF, second);
            break;
        }
        case 2:  // filler
            break;
        default:
            executeType3(reader, header, depth);
            break;
        }
    }
}

void commandProcessorMain() {
    createSystemThread("gpu-command-processor");
    for (;;) {
        uint32_t writeIndex;
        {
            std::unique_lock lock(gs.mutex);
            const auto idleStart = std::chrono::steady_clock::now();
            gs.wake.wait(lock, [] { return gs.ringDwords && gs.registers[kRegRbWptr] != gs.readIndex; });
            idleNs += nanosSince(idleStart);
            writeIndex = gs.registers[kRegRbWptr] % gs.ringDwords;
        }
        Reader ring{Memory::fromPhysical(gs.ringBase), gs.ringDwords, gs.readIndex, writeIndex, true};
        execute(ring, 0);
        {
            std::lock_guard lock(gs.mutex);
            gs.readIndex = writeIndex;
            publishReadPointer(writeIndex);
        }
    }
}

void vblankMain() {
    createSystemThread("gpu-vblank");
    // TENCHU_VBLANK_US overrides the 60 Hz interval (diagnostics only).
    char value[32]{};
    const int64_t interval = GetEnvironmentVariableA("TENCHU_VBLANK_US", value, sizeof(value)) ? atoll(value) : 16667;
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        next += std::chrono::microseconds(interval);
        std::this_thread::sleep_until(next);
        gs.lastVblankNs = std::chrono::steady_clock::now().time_since_epoch().count();
        ++gs.vblanks;
        dispatchInterrupt(0, 2);
    }
}

void startGpu() {
    std::lock_guard lock(gs.mutex);
    if (gs.started) return;
    gs.started = true;
    wchar_t dump[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"TENCHU_DUMP_SHADERS", dump, MAX_PATH)) {
        shaders.dumpDirectory = dump;
        std::filesystem::create_directories(shaders.dumpDirectory);
    }
    char value[32]{};
    if (GetEnvironmentVariableA("TENCHU_EXIT_AFTER_FRAMES", value, sizeof(value)))
        gs.exitAfterFrames = strtoull(value, nullptr, 10);
    std::thread(commandProcessorMain).detach();
    std::thread(vblankMain).detach();
}

// X_VIDEO_MODE: 1280x720 progressive widescreen at 60 Hz.
void writeVideoMode(uint32_t mode) {
    std::memset(memory->host(mode), 0, 48);
    store32(mode + 0, 1280);
    store32(mode + 4, 720);
    store32(mode + 8, 0);       // interlaced
    store32(mode + 12, 1);      // widescreen
    store32(mode + 16, 1);      // high definition
    const float refresh = 60.0f;
    uint32_t refreshBits;
    std::memcpy(&refreshBits, &refresh, 4);
    store32(mode + 20, refreshBits);
    store32(mode + 24, 1);      // video standard: NTSC
    store32(mode + 28, 0x4A);
    store32(mode + 32, 0x01);
}

}  // namespace

// ---- MMIO -----------------------------------------------------------------------

uint32_t PPCMmioRead32(uint32_t address) {
    if (address >= kGpuMmioBase && address < kGpuMmioBase + 0x10000) {
        const uint32_t index = (address - kGpuMmioBase) / 4;
        switch (index) {
        case kRegEdramTiming: return 0x08100748;
        case kRegBcControl: return 0x0000200E;
        case kRegD1ModeVCounter: {
            // 750 lines per 60 Hz frame; lines 720-749 are vertical blank.
            const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
            const int64_t sinceVblank = now - gs.lastVblankNs.load();
            const int64_t line = (sinceVblank / 1000) * 750 / 16667;
            return uint32_t((line + 720) % 750);
        }
        case kRegInterruptStatus: return 1;
        case kRegD1ModeViewportSize: return 0x050002D0;
        default: return gs.registers[index];
        }
    }
    TZ_TRACE("mmio", "[mmio] read %08X", address);
    return 0;
}

void PPCMmioWrite32(uint32_t address, uint32_t value) {
    if (address >= kGpuMmioBase && address < kGpuMmioBase + 0x10000) {
        const uint32_t index = (address - kGpuMmioBase) / 4;
        if (index == kRegRbWptr) {
            TZ_TRACE("kick", "[kick] wptr %X (rptr %X) by %s lr %08X", value, gs.readIndex,
                     currentThread() ? currentThread()->name.c_str() : "host",
                     currentThread() ? uint32_t(currentThread()->context.lr) : 0);
            {
                std::lock_guard lock(gs.mutex);
                gs.registers[index] = value;
            }
            gs.wake.notify_all();
            return;
        }
        writeRegister(index, value);
        return;
    }
    TZ_TRACE("mmio", "[mmio] write %08X = %08X", address, value);
}

// ---- Video imports ------------------------------------------------------------------

PPC_FUNC(__imp__VdInitializeRingBuffer) {
    // (physical address, log2 of size in quadwords)
    {
        std::lock_guard lock(gs.mutex);
        gs.ringBase = ctx.r3.u32;
        gs.ringDwords = (1u << (ctx.r4.u32 + 3)) / 4;
        gs.readIndex = 0;
        gs.registers[kRegRbRptr] = 0;
    }
    logf("[gpu] ring buffer at %08X, %u dwords", ctx.r3.u32, (1u << (ctx.r4.u32 + 3)) / 4);
    startGpu();
}

PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack) {
    // (physical address, log2 block size)
    logf("[gpu] read pointer writeback %08X", ctx.r3.u32);
    std::lock_guard lock(gs.mutex);
    gs.rptrWriteback = ctx.r3.u32;
}

PPC_FUNC(__imp__VdSetGraphicsInterruptCallback) {
    logf("[gpu] interrupt callback %08X data %08X", ctx.r3.u32, ctx.r4.u32);
    gs.interruptData = ctx.r4.u32;
    gs.interruptCallback = ctx.r3.u32;
    startGpu();
}

PPC_FUNC(__imp__VdGetSystemCommandBuffer) {
    std::memset(memory->host(ctx.r3.u32), 0, 0x94);
    store32(ctx.r3.u32, 0xBEEF0000);
    store32(ctx.r4.u32, 0xBEEF0001);
}

PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) {
    logf("[gpu] GPU identifier address %08X", ctx.r3.u32);
}

PPC_FUNC(__imp__VdInitializeScalerCommandBuffer) {
    // Nine arguments; the destination is the ninth. Fill it with filler packets.
    const uint32_t destination = load32(ctx.r1.u32 + 0x54);
    constexpr uint32_t kDwords = 0x1CC / 4;
    if (destination)
        for (uint32_t i = 0; i < kDwords; ++i) store32(destination + i * 4, 0x80000000);
    ctx.r3.u64 = kDwords;
}

PPC_FUNC(__imp__VdSwap) {
    // (commandBuffer*, fetchConstant*, ..., frontBuffer, colorFormat, colorSpace, ...)
    if (traceEnabled("swapargs")) {
        logf("[gpu] VdSwap r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X s9=%08X s10=%08X",
             ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
             load32(ctx.r1.u32 + 0x54), load32(ctx.r1.u32 + 0x5C));
        for (uint32_t r : {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32})
            if (r >= 0x10000) logf("[gpu]   *%08X = %08X", r, load32(r));
    }
    // Fill the 64-dword slot reserved in the command buffer with our swap packet.
    const uint32_t frontBuffer = ctx.r8.u32 ? load32(ctx.r8.u32) : 0;
    store32(ctx.r3.u32 + 0, 0xC0000000u | (62u << 16) | (kSwapOpcode << 8));
    store32(ctx.r3.u32 + 4, kSwapMagic);
    store32(ctx.r3.u32 + 8, frontBuffer);
    for (uint32_t i = 3; i < 64; ++i) store32(ctx.r3.u32 + i * 4, 0);
    const uint64_t frame = ++gs.frames;
    if (traceEnabled("gputime")) {
        static auto last = std::chrono::steady_clock::now();
        logf("[gputime] frame %llu: %.1f ms total, cp idle %.1f, reg waits %.1f, mem waits %.1f, interrupts %.1f",
             (unsigned long long)frame, nanosSince(last) / 1e6, idleNs.exchange(0) / 1e6, waitRegMemNs.exchange(0) / 1e6,
             waitMemNs.exchange(0) / 1e6, interruptNs.exchange(0) / 1e6);
        last = std::chrono::steady_clock::now();
    }
    if (frame == 1 || frame % 600 == 0 || traceEnabled("swap"))
        logf("[gpu] VdSwap frame %llu (%llu draw packets so far)", (unsigned long long)frame,
             (unsigned long long)gs.draws.load());
    if (gs.exitAfterFrames && frame >= gs.exitAfterFrames) {
        logf("[gpu] reached %llu frames, exiting", (unsigned long long)frame);
        fflush(stderr);
        ExitProcess(0);
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdQueryVideoMode) { writeVideoMode(ctx.r3.u32); }
PPC_FUNC(__imp__XGetVideoMode) { writeVideoMode(ctx.r3.u32); }
PPC_FUNC(__imp__VdQueryVideoFlags) { ctx.r3.u64 = 3; }  // widescreen, 720p capable

PPC_FUNC(__imp__VdGetCurrentDisplayInformation) {
    const uint32_t info = ctx.r3.u32;
    std::memset(memory->host(info), 0, 0x58);
    store16(info + 0x00, 1280);
    store16(info + 0x02, 720);
    store16(info + 0x48, 1280);
    store16(info + 0x4A, 720);
    const float refresh = 60.0f;
    uint32_t bits;
    std::memcpy(&bits, &refresh, 4);
    store32(info + 0x4C, bits);
    store16(info + 0x56, 1280);
}

PPC_FUNC(__imp__VdGetCurrentDisplayGamma) {
    store32(ctx.r3.u32, 1);
    const float power = 2.22222233f;
    uint32_t bits;
    std::memcpy(&bits, &power, 4);
    store32(ctx.r4.u32, bits);
}

PPC_FUNC(__imp__VdInitializeEngines) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdShutdownEngines) {}
PPC_FUNC(__imp__VdSetDisplayMode) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdPersistDisplay) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdRetrainEDRAM) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdRetrainEDRAMWorker) {}
PPC_FUNC(__imp__VdEnableDisableClockGating) {}
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }
