// Null graphics device: emulates enough of the Xenos command processor for
// the XDK Direct3D library to run (ring buffer, fences, interrupts, vblank)
// without drawing anything.
#include "kernel/kernel.h"
#include "ppc_recomp_shared.h"
#include <array>
#include <chrono>
#include <thread>

using namespace tz;

namespace {

// Register indices (dword offsets from 0x7FC80000).
enum : uint32_t {
    kRegRbRptr = 0x01C4,
    kRegRbWptr = 0x01C5,
    kRegScratchUmsk = 0x0570,
    kRegScratchAddr = 0x0571,
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
    bool started = false;
};
Gpu gpu;

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
    *memory->host<uint32_t>(Memory::fromPhysical(address & ~3u)) = gpuSwap(value, address & 3);
}
uint32_t readPhysical(uint32_t address) {
    return gpuSwap(*memory->host<uint32_t>(Memory::fromPhysical(address & ~3u)), address & 3);
}

void dispatchInterrupt(uint32_t source) {
    const uint32_t callback = gpu.interruptCallback.load();
    if (!callback) return;
    std::lock_guard lock(gpu.interruptMutex);
    callGuest(callback, {source, gpu.interruptData.load()});
}

void writeRegister(uint32_t index, uint32_t value) {
    index &= 0xFFFF;
    gpu.registers[index] = value;
    if (index >= kRegScratch0 && index < kRegScratch0 + 8) {
        const uint32_t slot = index - kRegScratch0;
        if (gpu.registers[kRegScratchUmsk] & (1u << slot))
            store32(Memory::fromPhysical(gpu.registers[kRegScratchAddr] + slot * 4), value);
    } else if (index == kRegCoherStatusHost) {
        // Cache flushes complete immediately.
        gpu.registers[index] = value & ~0x80000000u;
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
    switch (opcode) {
    case 0x3F:    // INDIRECT_BUFFER
    case 0x37: {  // INDIRECT_BUFFER_PFD
        const uint32_t address = reader.read();
        const uint32_t size = reader.read() & 0xFFFFF;
        reader.skip(count - 2);
        if (depth > 4) fatal("Indirect buffers nested too deeply");
        Reader indirect{Memory::fromPhysical(address), 0, 0, size, false};
        execute(indirect, depth + 1);
        break;
    }
    case 0x3C: {  // WAIT_REG_MEM
        const uint32_t info = reader.read(), poll = reader.read(), reference = reader.read();
        const uint32_t mask = reader.read();
        reader.skip(count - 4);
        const auto start = std::chrono::steady_clock::now();
        bool warned = false;
        for (;;) {
            const uint32_t value = info & 0x10 ? readPhysical(poll) : gpu.registers[poll & 0xFFFF].load();
            if (compare(info, value & mask, reference)) break;
            if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
                logf("[gpu] WAIT_REG_MEM stalled: %s %08X value %08X ref %08X mask %08X fn %u",
                     info & 0x10 ? "mem" : "reg", poll, value, reference, mask, info & 7);
                warned = true;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        break;
    }
    case 0x21: {  // REG_RMW
        const uint32_t info = reader.read(), andMask = reader.read(), orMask = reader.read();
        reader.skip(count - 3);
        const uint32_t index = info & 0x1FFF;
        const uint32_t andValue = info >> 31 ? gpu.registers[andMask & 0x1FFF].load() : andMask;
        const uint32_t orValue = (info >> 30) & 1 ? gpu.registers[orMask & 0x1FFF].load() : orMask;
        writeRegister(index, (gpu.registers[index] & andValue) | orValue);
        break;
    }
    case 0x45: {  // COND_WRITE
        const uint32_t info = reader.read(), poll = reader.read(), reference = reader.read();
        const uint32_t mask = reader.read(), target = reader.read(), data = reader.read();
        reader.skip(count - 6);
        const uint32_t value = info & 0x10 ? readPhysical(poll) : gpu.registers[poll & 0xFFFF].load();
        if (compare(info, value & mask, reference)) {
            if (info & 0x100) writePhysical(target, data);
            else writeRegister(target, data);
        }
        break;
    }
    case 0x58: {  // EVENT_WRITE_SHD
        const uint32_t initiator = reader.read(), address = reader.read(), value = reader.read();
        reader.skip(count - 3);
        writePhysical(address, initiator >> 31 ? uint32_t(gpu.frames.load()) : value);
        break;
    }
    case 0x3D: {  // MEM_WRITE
        const uint32_t address = reader.read() ;
        for (uint32_t i = 1; i < count; ++i) writePhysical(address + (i - 1) * 4, reader.read());
        break;
    }
    case 0x54: {  // INTERRUPT
        reader.skip(count);
        dispatchInterrupt(1);
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
    case 0x22:  // DRAW_INDX
    case 0x36:  // DRAW_INDX_2
        ++gpu.draws;
        reader.skip(count);
        break;
    default:
        reader.skip(count);
        break;
    }
}

void execute(Reader& reader, int depth) {
    while (reader.available()) {
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
            std::unique_lock lock(gpu.mutex);
            gpu.wake.wait(lock, [] { return gpu.ringDwords && gpu.registers[kRegRbWptr] != gpu.readIndex; });
            writeIndex = gpu.registers[kRegRbWptr] % gpu.ringDwords;
        }
        Reader ring{Memory::fromPhysical(gpu.ringBase), gpu.ringDwords, gpu.readIndex, writeIndex, true};
        execute(ring, 0);
        {
            std::lock_guard lock(gpu.mutex);
            gpu.readIndex = writeIndex;
            gpu.registers[kRegRbRptr] = writeIndex;
            if (gpu.rptrWriteback) store32(Memory::fromPhysical(gpu.rptrWriteback), writeIndex);
        }
    }
}

void vblankMain() {
    createSystemThread("gpu-vblank");
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
        dispatchInterrupt(0);
    }
}

void startGpu() {
    std::lock_guard lock(gpu.mutex);
    if (gpu.started) return;
    gpu.started = true;
    char value[32]{};
    if (GetEnvironmentVariableA("TENCHU_EXIT_AFTER_FRAMES", value, sizeof(value)))
        gpu.exitAfterFrames = strtoull(value, nullptr, 10);
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
        case kRegD1ModeVCounter: return 0x000002D0;
        case kRegInterruptStatus: return 1;
        case kRegD1ModeViewportSize: return 0x050002D0;
        default: return gpu.registers[index];
        }
    }
    TZ_TRACE("mmio", "[mmio] read %08X", address);
    return 0;
}

void PPCMmioWrite32(uint32_t address, uint32_t value) {
    if (address >= kGpuMmioBase && address < kGpuMmioBase + 0x10000) {
        const uint32_t index = (address - kGpuMmioBase) / 4;
        if (index == kRegRbWptr) {
            {
                std::lock_guard lock(gpu.mutex);
                gpu.registers[index] = value;
            }
            gpu.wake.notify_all();
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
        std::lock_guard lock(gpu.mutex);
        gpu.ringBase = ctx.r3.u32;
        gpu.ringDwords = (1u << (ctx.r4.u32 + 3)) / 4;
        gpu.readIndex = 0;
        gpu.registers[kRegRbRptr] = 0;
    }
    logf("[gpu] ring buffer at %08X, %u dwords", ctx.r3.u32, (1u << (ctx.r4.u32 + 3)) / 4);
    startGpu();
}

PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack) {
    // (physical address, log2 block size)
    std::lock_guard lock(gpu.mutex);
    gpu.rptrWriteback = ctx.r3.u32;
}

PPC_FUNC(__imp__VdSetGraphicsInterruptCallback) {
    gpu.interruptData = ctx.r4.u32;
    gpu.interruptCallback = ctx.r3.u32;
    startGpu();
}

PPC_FUNC(__imp__VdGetSystemCommandBuffer) {
    std::memset(memory->host(ctx.r3.u32), 0, 0x94);
    store32(ctx.r3.u32, 0xBEEF0000);
    store32(ctx.r4.u32, 0xBEEF0001);
}

PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) {}

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
    // The 64-dword slot reserved in the command buffer becomes filler.
    for (uint32_t i = 0; i < 64; ++i) store32(ctx.r3.u32 + i * 4, 0x80000000);
    const uint64_t frame = ++gpu.frames;
    if (frame == 1 || frame % 600 == 0 || traceEnabled("swap"))
        logf("[gpu] VdSwap frame %llu (%llu draw packets so far)", (unsigned long long)frame,
             (unsigned long long)gpu.draws.load());
    if (gpu.exitAfterFrames && frame >= gpu.exitAfterFrames) {
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
