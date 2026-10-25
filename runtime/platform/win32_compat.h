#pragma once
// Linux: the handful of small Win32 calls and constants the runtime uses everywhere, on top of
// DXVK-native's windows.h (types, COM, D3D/DXGI). Force-included into every runtime and app
// source on Linux (CMakeLists.txt); the bigger Windows pieces (memory, faults, window, audio,
// HTTP, dialogs) have their own Linux implementations behind #ifdef _WIN32.
#ifndef _WIN32
#include <windows.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sched.h>
#include <immintrin.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
#ifndef WM_APP
#define WM_APP 0x8000
#endif
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif

#define _lock_file flockfile
#define _unlock_file funlockfile
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define sscanf_s sscanf  // numeric conversions only (no %s/%c buffer sizes)

inline unsigned char _BitScanForward(unsigned long* index, unsigned long mask) {
    if (!mask) return 0;
    *index = static_cast<unsigned long>(__builtin_ctzl(mask));
    return 1;
}
inline uint16_t _byteswap_ushort(uint16_t value) { return __builtin_bswap16(value); }
inline uint32_t _byteswap_ulong(uint32_t value) { return __builtin_bswap32(value); }
inline uint64_t _byteswap_uint64(uint64_t value) { return __builtin_bswap64(value); }

namespace tz::compat {
// UTF-8 <-> wchar_t (UTF-32 on Linux).
inline std::string narrow(const wchar_t* text) {
    std::string out;
    for (; text && *text; ++text) {
        const uint32_t c = uint32_t(*text);
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += {char(0xC0 | (c >> 6)), char(0x80 | (c & 0x3F))};
        else if (c < 0x10000) out += {char(0xE0 | (c >> 12)), char(0x80 | ((c >> 6) & 0x3F)), char(0x80 | (c & 0x3F))};
        else out += {char(0xF0 | (c >> 18)), char(0x80 | ((c >> 12) & 0x3F)), char(0x80 | ((c >> 6) & 0x3F)), char(0x80 | (c & 0x3F))};
    }
    return out;
}
inline std::wstring widen(const char* text) {
    std::wstring out;
    const auto* s = reinterpret_cast<const unsigned char*>(text);
    while (s && *s) {
        uint32_t c = *s++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3F >> extra;
        for (; extra && (*s & 0xC0) == 0x80; --extra) c = (c << 6) | (*s++ & 0x3F);
        out += wchar_t(c);
    }
    return out;
}
}  // namespace tz::compat

// Environment: returns the value's length (0 when unset), like Windows; a null or too small
// buffer returns the size needed including the terminator.
inline DWORD GetEnvironmentVariableA(const char* name, char* buffer, DWORD size) {
    const char* value = std::getenv(name);
    if (!value) return 0;
    const size_t length = std::strlen(value);
    if (!buffer || size <= length) return DWORD(length + 1);
    std::memcpy(buffer, value, length + 1);
    return DWORD(length);
}
inline DWORD GetEnvironmentVariableW(const wchar_t* name, wchar_t* buffer, DWORD size) {
    const char* value = std::getenv(tz::compat::narrow(name).c_str());
    if (!value) return 0;
    const std::wstring wide = tz::compat::widen(value);
    if (!buffer || size <= wide.size()) return DWORD(wide.size() + 1);
    std::wmemcpy(buffer, wide.c_str(), wide.size() + 1);
    return DWORD(wide.size());
}

[[noreturn]] inline void ExitProcess(UINT code) {
    std::fflush(nullptr);
    _exit(int(code));
}
inline void Sleep(DWORD milliseconds) {
    timespec duration{time_t(milliseconds / 1000), long(milliseconds % 1000) * 1000000L};
    while (nanosleep(&duration, &duration) == -1 && errno == EINTR) {
    }
}
inline uint64_t GetTickCount64() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1000 + uint64_t(now.tv_nsec) / 1000000;
}
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency) {
    frequency->QuadPart = 1000000000;
    return TRUE;
}
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* counter) {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    counter->QuadPart = int64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
    return TRUE;
}
inline BOOL SwitchToThread() { return sched_yield() == 0; }
// Thread priorities need privileges on Linux: the scheduler's defaults stay.
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
inline HANDLE GetCurrentThread() { return reinterpret_cast<HANDLE>(intptr_t(-2)); }
inline BOOL SetThreadPriority(HANDLE, int) { return TRUE; }

// High-resolution waitable timers as the GPU threads use them (set a relative due time, then
// wait for it): an absolute CLOCK_MONOTONIC deadline and clock_nanosleep.
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#define TIMER_ALL_ACCESS 0x1F0003
#ifndef INFINITE
#define INFINITE 0xFFFFFFFFu
#endif
inline HANDLE CreateWaitableTimerExW(void*, const wchar_t*, DWORD, DWORD) { return reinterpret_cast<HANDLE>(new timespec{}); }
inline BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER* due, LONG, void*, void*, BOOL) {
    auto* deadline = static_cast<timespec*>(timer);
    clock_gettime(CLOCK_MONOTONIC, deadline);
    // Negative: relative, in 100 ns units (the only form used).
    const int64_t nanoseconds = (due->QuadPart < 0 ? -due->QuadPart : due->QuadPart) * 100 + deadline->tv_nsec;
    deadline->tv_sec += time_t(nanoseconds / 1000000000);
    deadline->tv_nsec = long(nanoseconds % 1000000000);
    return TRUE;
}
inline DWORD WaitForSingleObject(HANDLE timer, DWORD) {
    const auto* deadline = static_cast<const timespec*>(timer);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, nullptr) == EINTR) {
    }
    return 0;
}
inline DWORD GetCurrentThreadId() { return DWORD(gettid()); }

// The executable's path (module must be null: the program itself).
inline DWORD GetModuleFileNameW(HMODULE module, wchar_t* buffer, DWORD size) {
    (void)module;
    char path[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0 || !size) return 0;
    path[length] = 0;
    const std::wstring wide = tz::compat::widen(path);
    const size_t count = wide.size() < size - 1 ? wide.size() : size - 1;
    std::wmemcpy(buffer, wide.c_str(), count);
    buffer[count] = 0;
    return DWORD(count);
}

inline FILE* _wfopen(const wchar_t* path, const wchar_t* mode) {
    return std::fopen(tz::compat::narrow(path).c_str(), tz::compat::narrow(mode).c_str());
}
inline FILE* _wfopen(const char* path, const wchar_t* mode) { return std::fopen(path, tz::compat::narrow(mode).c_str()); }
inline BOOL CreateDirectoryA(const char* path, void*) { return mkdir(path, 0755) == 0 || errno == EEXIST; }
inline int strncpy_s(char* destination, size_t size, const char* source, size_t count) {
    if (!destination || !size) return EINVAL;
    size_t n = std::strlen(source);
    if (count != _TRUNCATE && count < n) n = count;
    if (n >= size) n = size - 1;
    std::memcpy(destination, source, n);
    destination[n] = 0;
    return 0;
}
template <size_t N> int strncpy_s(char (&destination)[N], const char* source, size_t count) {
    return strncpy_s(destination, N, source, count);
}

#include "win32_files.h"

// Debug screenshots (.bmp).
#pragma pack(push, 2)
typedef struct BITMAPFILEHEADER {
    WORD bfType;
    DWORD bfSize;
    WORD bfReserved1, bfReserved2;
    DWORD bfOffBits;
} BITMAPFILEHEADER;
#pragma pack(pop)
typedef struct BITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth, biHeight;
    WORD biPlanes, biBitCount;
    DWORD biCompression, biSizeImage;
    LONG biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;

// The D3D11 headers of DXVK-native predate filter reduction types (STANDARD is 0).
#ifndef D3D11_FILTER_REDUCTION_TYPE_STANDARD
#define D3D11_FILTER_REDUCTION_TYPE_STANDARD 0
#endif

// XInput's state layout (the guest's XamInputGetState fills it).
typedef struct XINPUT_GAMEPAD {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XINPUT_GAMEPAD;
typedef struct XINPUT_STATE {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
} XINPUT_STATE;

// Virtual-key codes: the keyboard bindings are stored and compared as Windows key codes; the
// Linux window translates SDL keys into them (host/input.cpp).
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_F1 0x70
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5

// Window messages: the Linux window (runtime/platform/window_sdl.cpp) translates SDL events
// into these, and PostMessageW queues one as an SDL event.
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_CLOSE 0x0010
#define WM_INPUT 0x00FF
#define WM_KEYFIRST 0x0100
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_KEYLAST 0x0109
#define WM_MOUSEFIRST 0x0200
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MOUSEWHEEL 0x020A
#define WM_XBUTTONDOWN 0x020B
#define WM_XBUTTONUP 0x020C
#define WM_MOUSELAST 0x020E
#define XBUTTON1 1
#define XBUTTON2 2
#define GET_XBUTTON_WPARAM(wParam) ((WORD)(((uintptr_t)(wParam)) >> 16))
BOOL PostMessageW(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
#endif  // !_WIN32
