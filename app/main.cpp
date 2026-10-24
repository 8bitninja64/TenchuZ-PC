// Tenchu Z Recomp entry point.
#include "kernel/kernel.h"
#include "kernel/filesystem.h"
#include "kernel/module_loader.h"
#include "boot.h"
#include "gpu/d3d11/renderer.h"
#include "host/input.h"
#include "host/settings.h"
#include "host/overlay.h"
#include <thread>
#include <cstdio>
#include <exception>
#include <stdexcept>

namespace {
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
        HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"Tenchu Z", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
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
    // Real pixels: fullscreen uses the monitor's native resolution.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    setvbuf(stdout, nullptr, _IONBF, 0);
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
