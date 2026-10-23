// Host sampling profiler for one thread (TENCHU_TRACE=hostprof): samples the
// instruction pointer every millisecond for TENCHU_PROF's window and prints
// the hottest functions, resolved through the executable's PDB.
#include "kernel/kernel.h"
#include "kernel/control.h"

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace tz {

void startHostProfiler(const char* name) {
    if (!traceEnabled("hostprof")) return;
    HANDLE target = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    std::string label = name;
    std::thread([target, label] {
        int startSeconds = 8, durationSeconds = 10;
        char window[32]{};
        if (GetEnvironmentVariableA("TENCHU_PROF", window, sizeof(window)))
            sscanf_s(window, "%d,%d", &startSeconds, &durationSeconds);
        // With a control file, the 'profile' command starts the window instead (for
        // every profiled thread: the first one to see the flag tells the others).
        static std::atomic<bool> started{false};
        if (control::active()) {
            while (!started) {
                if (control::takeFlag("profile")) started = true;
                else std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(startSeconds));
        }
        std::map<DWORD64, uint32_t> samples, callers;
        const DWORD64 exeBase = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(durationSeconds);
        uint32_t total = 0;
        while (std::chrono::steady_clock::now() < end) {
            if (SuspendThread(target) != DWORD(-1)) {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(target, &context)) {
                    ++samples[context.Rip];
                    ++total;
                    // Also attribute the sample to the first caller inside the executable.
                    DWORD64 caller = 0;
                    CONTEXT frame = context;
                    for (int depth = 0; depth < 32; ++depth) {
                        DWORD64 imageBase = 0;
                        auto* function = RtlLookupFunctionEntry(frame.Rip, &imageBase, nullptr);
                        if (imageBase == exeBase && depth > 0) {
                            caller = frame.Rip;
                            break;
                        }
                        if (!function) {
                            frame.Rip = *reinterpret_cast<DWORD64*>(frame.Rsp);
                            frame.Rsp += 8;
                        } else {
                            void* handlerData = nullptr;
                            DWORD64 establisher = 0;
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, frame.Rip, function, &frame, &handlerData,
                                             &establisher, nullptr);
                        }
                        if (!frame.Rip) break;
                    }
                    if (caller) ++callers[caller];
                }
                ResumeThread(target);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // DbgHelp is single-threaded: one report at a time.
        static std::mutex symbols;
        std::lock_guard lock(symbols);
        HANDLE process = GetCurrentProcess();
        static const bool initialized = [process] {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            return SymInitialize(process, nullptr, TRUE) != FALSE;
        }();
        (void)initialized;
        auto report = [&](const std::map<DWORD64, uint32_t>& counts, const char* title) {
        std::map<std::string, uint32_t> byFunction;
        for (const auto& [address, count] : counts) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            IMAGEHLP_MODULE64 module{sizeof(module)};
            std::string function = SymGetModuleInfo64(process, address, &module) ? std::string(module.ModuleName) : "?";
            if (SymFromAddr(process, address, &displacement, symbol))
                function += std::string("!") + symbol->Name + (displacement > 0x10000 ? " (far)" : "");
            byFunction[function] += count;
        }
        std::vector<std::pair<uint32_t, std::string>> sorted;
        for (const auto& [function, count] : byFunction) sorted.push_back({count, function});
        std::sort(sorted.rbegin(), sorted.rend());
        logf("[hostprof] %s: %u samples, %s", label.c_str(), total, title);
        for (size_t i = 0; i < sorted.size() && i < 30; ++i)
            logf("[hostprof]   %5.1f%%  %s", 100.0 * sorted[i].first / (std::max)(1u, total), sorted[i].second.c_str());
        };
        report(samples, "by location");
        report(callers, "by first caller in the executable (outside calls only)");
    }).detach();
}

}  // namespace tz
