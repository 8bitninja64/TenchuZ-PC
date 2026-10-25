// Tenchu Z Recomp entry point.
#include "kernel/kernel.h"
#include "kernel/filesystem.h"
#include "kernel/module_loader.h"
#include "boot.h"
#include "gpu/d3d11/renderer.h"
#include "host/input.h"
#include "host/settings.h"
#include "host/overlay.h"
#include "host/update_check.h"
#ifdef _WIN32
#include <dbghelp.h>
#else
#include <SDL3/SDL.h>
#include <execinfo.h>
#include <csignal>
#include <ucontext.h>
#include <vector>

#include "platform/window_sdl.h"
#endif
#include <cstring>
#include <chrono>
#include <thread>
#include <cstdio>
#include <exception>
#include <stdexcept>

namespace {
#ifdef _WIN32
// Borderless fullscreen on the window's monitor, or back to the saved window.
void setFullscreen(HWND window, bool fullscreen) {
    static WINDOWPLACEMENT placement{sizeof(placement)};
    const LONG style = GetWindowLongW(window, GWL_STYLE);
    if (fullscreen) {
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor)) return;
        GetWindowPlacement(window, &placement);
        SetWindowLongW(window, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        const RECT& area = monitor.rcMonitor;
        SetWindowPos(window, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(window, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        if (placement.length) SetWindowPlacement(window, &placement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    static bool fullscreen = false;
    if (message == tz::host::overlay::kMessageFullscreen) {
        if ((wParam != 0) != fullscreen) setFullscreen(window, fullscreen = wParam != 0);
        return 0;
    }
    // F1 menu first: it takes keyboard and mouse while open.
    if (message != WM_CLOSE && tz::host::overlay::handleMessage(window, message, wParam, lParam)) return 0;
    switch (message) {
    case WM_CLOSE:
        ExitProcess(0);
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        // Alt+Enter and F11 toggle fullscreen.
        if ((wParam == VK_RETURN && (lParam & (1 << 29))) || wParam == VK_F11) {
            if (!(lParam & (1 << 30))) setFullscreen(window, fullscreen = !fullscreen);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        return 0;  // no menu beep for Alt combinations
    case WM_APP:
        setFullscreen(window, fullscreen = true);
        return 0;
    default:
        break;
    }
    if (tz::host::handleInputMessage(window, message, wParam, lParam)) return 0;
    return DefWindowProcW(window, message, wParam, lParam);
}

// The game window lives on its own thread so the guest never blocks input.
void startWindowThread() {
    std::thread([] {
        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.lpszClassName = L"TenchuZRecomp";
        RegisterClassExW(&windowClass);
        const auto& settings = tz::host::settings();
        RECT rect{0, 0, settings.windowWidth, settings.windowHeight};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"Tenchu Z Recomp", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                      CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                      windowClass.hInstance, nullptr);
        tz::host::initializeInput(window);
        ShowWindow(window, SW_SHOW);
        // Automated runs (TENCHU_CONTROL) start from a background shell, which may not
        // take the foreground, and drivers throttle background windows (NVIDIA: 20 fps
        // by default). A synthetic Alt press lets SetForegroundWindow through.
        if (GetEnvironmentVariableW(L"TENCHU_CONTROL", nullptr, 0)) {
            keybd_event(VK_MENU, 0, KEYEVENTF_EXTENDEDKEY, 0);
            SetForegroundWindow(window);
            keybd_event(VK_MENU, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
        }
        if (settings.fullscreen) PostMessageW(window, WM_APP, 0, 0);
        tz::gpu::setOutputWindow(window);
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }).detach();
}

// A C++ exception (MSVC x64 EH records): the thrown type and, for std::exception, what().
void describeCxxException(const EXCEPTION_RECORD* record) {
    if (record->NumberParameters < 4 || record->ExceptionInformation[0] != 0x19930520) return;
    struct ThrowInfo { uint32_t attributes; int32_t unwind, forwardCompat, catchableTypes; };
    struct CatchableType { uint32_t properties; int32_t type, mdisp, pdisp, vdisp, size, copy; };
    const auto object = record->ExceptionInformation[1];
    const auto* info = reinterpret_cast<const ThrowInfo*>(record->ExceptionInformation[2]);
    const auto image = record->ExceptionInformation[3];
    if (!info || !image) return;
    const auto* types = reinterpret_cast<const int32_t*>(image + info->catchableTypes);
    const char* thrown = nullptr;
    const std::exception* standard = nullptr;
    for (int32_t i = 0; i < types[0]; ++i) {
        const auto* type = reinterpret_cast<const CatchableType*>(image + types[1 + i]);
        const char* name = reinterpret_cast<const char*>(image + type->type + 16);  // TypeDescriptor::name
        if (!thrown) thrown = name;
        if (!std::strcmp(name, ".?AVexception@std@@") && type->pdisp < 0)
            standard = reinterpret_cast<const std::exception*>(object + type->mdisp);
    }
    fprintf(stderr, "C++ exception %s", thrown ? thrown : "?");
    if (standard) fprintf(stderr, ": %s", standard->what());
    fputc('\n', stderr);
}

// The host call stack at the exception, with symbols where PDBs are present.
void printHostStack(const CONTEXT* exceptionContext) {
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    CONTEXT context = *exceptionContext;
    STACKFRAME64 frame{};
    frame.AddrPC = {context.Rip, 0, AddrModeFlat};
    frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
    frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
    fprintf(stderr, "host stack:\n");
    for (int depth = 0; depth < 32; ++depth) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !frame.AddrPC.Offset)
            break;
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        IMAGEHLP_MODULE64 module{sizeof(module)};
        const char* moduleName = SymGetModuleInfo64(process, frame.AddrPC.Offset, &module) ? module.ModuleName : "?";
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
            IMAGEHLP_LINE64 line{sizeof(line)};
            DWORD column = 0;
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &column, &line))
                fprintf(stderr, "  %s!%s+0x%llX (%s:%lu)\n", moduleName, symbol->Name, (unsigned long long)displacement,
                        line.FileName, line.LineNumber);
            else
                fprintf(stderr, "  %s!%s+0x%llX\n", moduleName, symbol->Name, (unsigned long long)displacement);
        } else {
            fprintf(stderr, "  %s!0x%llX\n", moduleName, (unsigned long long)frame.AddrPC.Offset);
        }
    }
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* exception) {
    const auto* record = exception->ExceptionRecord;
    fprintf(stderr, "[FATAL] host exception 0x%08lX at %p", record->ExceptionCode, record->ExceptionAddress);
    if (record->ExceptionCode == 0xE06D7363) {
        fputc('\n', stderr);
        describeCxxException(record);
    }
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
    printHostStack(exception->ContextRecord);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
// Linux: an SDL3 window (DXVK-native presents to it). Its events arrive as the Win32 messages
// the host code handles (runtime/platform/window_sdl.cpp).
void setFullscreen(HWND window, bool fullscreen) {
    // Without a display mode SDL3 makes it borderless fullscreen at the desktop resolution.
    SDL_SetWindowFullscreen(static_cast<SDL_Window*>(window), fullscreen);
}

void windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    static bool fullscreen = false;
    if (message == tz::host::overlay::kMessageFullscreen) {
        if ((wParam != 0) != fullscreen) setFullscreen(window, fullscreen = wParam != 0);
        return;
    }
    // F1 menu first: it takes keyboard and mouse while open.
    if (message != WM_CLOSE && tz::host::overlay::handleMessage(window, message, wParam, lParam)) return;
    switch (message) {
    case WM_CLOSE:
        ExitProcess(0);
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        // Alt+Enter and F11 toggle fullscreen.
        if ((wParam == VK_RETURN && (lParam & (1 << 29))) || wParam == VK_F11) {
            if (!(lParam & (1 << 30))) setFullscreen(window, fullscreen = !fullscreen);
            return;
        }
        break;
    case WM_APP:
        setFullscreen(window, fullscreen = true);
        return;
    default:
        break;
    }
    tz::host::handleInputMessage(window, message, wParam, lParam);
}

// The game window lives on its own thread so the guest never blocks input.
void startWindowThread() {
    std::thread([] {
        if (!SDL_Init(SDL_INIT_VIDEO)) tz::fatal("Cannot open a window: %s", SDL_GetError());
        const auto& settings = tz::host::settings();
        SDL_Window* window = SDL_CreateWindow("Tenchu Z Recomp", settings.windowWidth, settings.windowHeight,
                                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
        if (!window) tz::fatal("Cannot open a window: %s", SDL_GetError());
        tz::host::initializeInput(window);
        if (settings.fullscreen) PostMessageW(window, WM_APP, 0, 0);
        tz::gpu::setOutputWindow(window);
        SDL_Event event;
        while (SDL_WaitEvent(&event)) tz::platform::dispatchEvent(window, event, windowProcedure);
    }).detach();
}

// Faults nobody handled (the guest memory handlers in kernel/fault.cpp come first): where it
// happened, the guest thread's recent functions and the host stack, then the default action.
void crashSignal(int signal, siginfo_t* info, void* data) {
    const auto* context = static_cast<const ucontext_t*>(data);
    const uintptr_t address = uintptr_t(info->si_addr);
    fprintf(stderr, "[FATAL] host signal %d at %p", signal,
            reinterpret_cast<void*>(context->uc_mcontext.gregs[REG_RIP]));
    if ((signal == SIGSEGV || signal == SIGBUS) && tz::memory) {
        const uintptr_t base = uintptr_t(tz::memory->base());
        const bool write = context->uc_mcontext.gregs[REG_ERR] & 2;
        if (address >= base && address - base < 0x100000000ull)
            fprintf(stderr, " (%s guest 0x%08llX)", write ? "write" : "read", (unsigned long long)(address - base));
        else
            fprintf(stderr, " (%s host %p)", write ? "write" : "read", info->si_addr);
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
    fprintf(stderr, "host stack:\n");
    void* frames[48];
    backtrace_symbols_fd(frames, backtrace(frames, 48), 2);
    fflush(stderr);
    std::signal(signal, SIG_DFL);
    raise(signal);
}

void installCrashHandler() {
    struct sigaction action {};
    action.sa_sigaction = crashSignal;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    for (const int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(signal, &action, nullptr);
}
#endif

int run(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    namespace update = tz::host::update;
    update::cleanup();  // the files the last update replaced
    // Automated runs stay offline unless they test the updater (TENCHU_UPDATE_API).
    const bool testing = GetEnvironmentVariableW(L"TENCHU_CONTROL", nullptr, 0) &&
                         !GetEnvironmentVariableW(L"TENCHU_UPDATE_API", nullptr, 0);
    if (tz::host::settings().updateCheck && !testing && !GetEnvironmentVariableW(L"TENCHU_NO_UPDATE_CHECK", nullptr, 0))
        update::checkStart();
    // Test hook (with TENCHU_UPDATE_API): install a found update and restart into it, no clicks.
    if (GetEnvironmentVariableW(L"TENCHU_UPDATE_AUTOINSTALL", nullptr, 0)) {
        std::thread([] {
            for (bool started = false;; std::this_thread::sleep_for(std::chrono::milliseconds(200))) {
                if (!started && update::checkStatus().state == update::Release::Available) {
                    started = true;
                    update::installStart(update::checkStatus());
                }
                if (update::installStatus().state == update::Install::Done) {
                    if (update::restart()) ExitProcess(0);
                    return;
                }
            }
        }).detach();
    }
    try {
        // The disc and a game module translated from it (built on the first start).
        tz::GameSource game;
        if (!tz::prepareGame(argc, argv, game)) return 1;
        if (!GetEnvironmentVariableW(L"TENCHU_HEADLESS", nullptr, 0)) startWindowThread();
        tz::initializeKernel(game.xex);
        for (const char* device : {"game", "d"}) {
            if (game.disc) tz::mountDevice(device, game.disc);
            else tz::mountDevice(device, game.folder);
        }
        std::filesystem::create_directories(tz::userDirectory() / "cache");
        tz::mountDevice("cache", tz::userDirectory() / "cache");
        tz::runTitle();
    } catch (const std::exception& error) {
        fprintf(stderr, "[FATAL] %s\n", error.what());
        return 1;
    }
    return 0;
}
}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetUnhandledExceptionFilter(crashFilter);
    // Real pixels: fullscreen uses the monitor's native resolution.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    return run(argc, argv);
}
#else
int main(int argc, char** argv) {
    installCrashHandler();
    // Started without a terminal (Steam, a file manager, Game Mode): everything logged, DXVK's
    // messages included, goes to TenchuZ.log in the user folder (overwritten at each start).
    if (!isatty(STDERR_FILENO)) {
        std::error_code error;
        const auto directory = tz::userDataDirectory();
        std::filesystem::create_directories(directory, error);
        const std::string path = tz::utf8(directory / "TenchuZ.log");
        if (std::freopen(path.c_str(), "w", stderr)) {
            setvbuf(stderr, nullptr, _IOLBF, 0);
            dup2(STDERR_FILENO, STDOUT_FILENO);
        }
    }
    // DXVK-native presents to SDL3 windows.
    setenv("DXVK_WSI_DRIVER", "SDL3", 0);
    std::vector<std::wstring> wide;
    for (int i = 0; i < argc; ++i) wide.push_back(tz::compat::widen(argv[i]));
    std::vector<wchar_t*> pointers;
    for (auto& argument : wide) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    // Quit without static destructors: guest threads are still running.
    std::fflush(nullptr);
    const int code = run(argc, pointers.data());
    std::fflush(nullptr);
    std::_Exit(code);
}
#endif
