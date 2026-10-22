// Tenchu Z Recomp entry point.
#include "kernel/kernel.h"
#include "kernel/filesystem.h"
#include <cstdio>
#include <exception>

namespace {
std::filesystem::path findGameDirectory(int argc, wchar_t** argv) {
    if (argc > 1) return argv[1];
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    // Next to the executable, then the repository layout used during development.
    for (auto directory = std::filesystem::path(executable).parent_path(); !directory.empty();
         directory = directory.parent_path()) {
        if (std::filesystem::exists(directory / "game" / "default.xex")) return directory / "game";
        if (directory == directory.parent_path()) break;
    }
    return L"game";
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* exception) {
    const auto* record = exception->ExceptionRecord;
    fprintf(stderr, "[FATAL] host exception 0x%08lX at %p", record->ExceptionCode, record->ExceptionAddress);
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 && tz::memory) {
        const uintptr_t address = record->ExceptionInformation[1];
        const uintptr_t base = uintptr_t(tz::memory->base());
        fprintf(stderr, " (%s ", record->ExceptionInformation[0] ? "write" : "read");
        if (address >= base && address - base < 0x100000000ull) fprintf(stderr, "guest 0x%08llX)", address - base);
        else fprintf(stderr, "host %p)", reinterpret_cast<void*>(address));
    }
    fputc('\n', stderr);
    if (auto* thread = tz::currentThread()) {
        const auto& ctx = thread->context;
        fprintf(stderr, "thread %u (%s) guest function 0x%08X lr 0x%08X\nrecent:", thread->id, thread->name.c_str(),
                ctx.lastFunction, uint32_t(ctx.lr));
        const uint32_t count = (std::min)(ctx.traceIndex, 64u);
        for (uint32_t i = 0; i < count; ++i) fprintf(stderr, " %08X", ctx.trace[(ctx.traceIndex - count + i) & 63]);
        fputc('\n', stderr);
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetUnhandledExceptionFilter(crashFilter);
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const auto gameDirectory = findGameDirectory(argc, argv);
        tz::initializeKernel(gameDirectory);
        tz::mountDevice("game", tz::gameDirectory());
        tz::mountDevice("d", tz::gameDirectory());
        std::filesystem::create_directories(tz::userDirectory() / "cache");
        tz::mountDevice("cache", tz::userDirectory() / "cache");
        tz::runTitle();
    } catch (const std::exception& error) {
        fprintf(stderr, "[FATAL] %s\n", error.what());
        return 1;
    }
    return 0;
}
