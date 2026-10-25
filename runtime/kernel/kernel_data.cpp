// Kernel variables the title imports by address.
#include "kernel.h"
#include "xex.h"
#include "module_loader.h"
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>
#ifdef _WIN32
#include <timeapi.h>
#endif

namespace tz {
namespace {

uint32_t allocateString(const char* text) {
    const uint32_t size = uint32_t(std::strlen(text) + 1);
    const uint32_t address = memory->allocateKernel(size, 4);
    std::memcpy(memory->host(address), text, size);
    return address;
}

uint32_t initializeCriticalSectionObject() {
    // RTL_CRITICAL_SECTION: synchronization event header, lock count -1.
    const uint32_t section = createDispatcherObject(kSynchronizationEvent, 28, 0);
    store32(section + 16, 0xFFFFFFFF);
    return section;
}

// Loader entry: image base +0x18, entry point +0x1C, image size +0x20,
// XEX header +0x58.
uint32_t createModuleEntry(const XexImage& xex) {
    const uint32_t entry = memory->allocateKernel(0x80, 16);
    store32(entry + 0x18, xex.loadAddress);
    store32(entry + 0x1C, xex.entryPoint);
    store32(entry + 0x20, uint32_t(xex.image.size()));
    store32(entry + 0x58, xexHeaderAddress());
    return entry;
}

}  // namespace

void startTimeStampBundle(uint32_t bundle) {
    // { u64 interrupt time; u64 system time; u32 tick count (ms) }, updated every millisecond.
    std::thread([bundle] {
#ifdef _WIN32
        timeBeginPeriod(1);
#endif
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            const uint64_t interrupt = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100);
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            store64(bundle + 0, interrupt);
            store64(bundle + 8, uint64_t(now.dwHighDateTime) << 32 | now.dwLowDateTime);
            store32(bundle + 16, uint32_t(interrupt / 10000));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
}

void installDataImports(const XexImage& xex) {
    const GameModule* module = gameModule();
    for (uint32_t i = 0; i < module->dataImportCount; ++i) {
        const uint32_t slot = module->dataImports[i].slot;
        const std::string_view id = module->dataImports[i].name;
        uint32_t value = 0;
        if (id == "XboxKrnlVersion") {
            value = memory->allocateKernel(8, 4);
            store16(value + 0, 2);
            store16(value + 2, 0);
            store16(value + 4, 17559);
            store16(value + 6, 0);
        } else if (id == "VdGpuClockInMHz") {
            value = memory->allocateKernel(4, 4);
            store32(value, 500);
        } else if (id == "VdHSIOCalibrationLock") {
            value = initializeCriticalSectionObject();
        } else if (id == "KeCertMonitorData" || id == "KeDebugMonitorData") {
            value = memory->allocateKernel(0x100, 16);
        } else if (id == "VdGlobalDevice") {
            value = memory->allocateKernel(4, 4);
        } else if (id == "XexExecutableModuleHandle") {
            value = memory->allocateKernel(4, 4);
            store32(value, createModuleEntry(xex));
        } else if (id == "ExLoadedCommandLine") {
            value = allocateString("\"default.xex\"");
        } else if (id == "KeTimeStampBundle") {
            value = memory->allocateKernel(24, 8);
            startTimeStampBundle(value);
        } else if (id == "ExThreadObjectType") {
            value = memory->allocateKernel(0x40, 16);
        } else {
            fatal("Unhandled kernel data import %s", module->dataImports[i].name);
        }
        store32(slot, value);
    }
}

}  // namespace tz
