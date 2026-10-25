#pragma once
// Linux: the Win32 file and time calls the kernel's I/O and time imports use (imports_io.cpp,
// xdvdfs.cpp, imports_rtl.cpp), on POSIX file descriptors. A file HANDLE is the descriptor.
// Included by win32_compat.h.
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <ctime>

typedef struct FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;
typedef struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;
typedef struct OVERLAPPED {
    ULONG_PTR Internal, InternalHigh;
    DWORD Offset, OffsetHigh;
    HANDLE hEvent;
} OVERLAPPED;
typedef struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
typedef enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef enum FILE_INFO_BY_HANDLE_CLASS { FileEndOfFileInfo = 6 } FILE_INFO_BY_HANDLE_CLASS;
typedef struct FILE_END_OF_FILE_INFO {
    LARGE_INTEGER EndOfFile;
} FILE_END_OF_FILE_INFO;

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_READ_FAULT 30
#define ERROR_HANDLE_EOF 38
#define ERROR_FILE_EXISTS 80

namespace tz::compat {
inline thread_local DWORD lastError = 0;
inline DWORD errorFromErrno(int error) {
    switch (error) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_FILE_EXISTS;
    case EBADF: return ERROR_INVALID_HANDLE;
    default: return ERROR_READ_FAULT;
    }
}
inline BOOL fail() {
    lastError = errorFromErrno(errno);
    return FALSE;
}
inline int descriptor(HANDLE handle) { return int(reinterpret_cast<intptr_t>(handle)); }
// FILETIME counts 100 ns units since 1601-01-01.
constexpr uint64_t kUnixEpoch100ns = 116444736000000000ull;
inline FILETIME fileTime(uint64_t value) { return FILETIME{DWORD(value), DWORD(value >> 32)}; }
inline FILETIME fileTime(const timespec& time) {
    return fileTime(kUnixEpoch100ns + uint64_t(time.tv_sec) * 10000000ull + uint64_t(time.tv_nsec) / 100);
}
inline void attributeData(const struct stat& info, WIN32_FILE_ATTRIBUTE_DATA* data) {
    data->dwFileAttributes = S_ISDIR(info.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    data->ftCreationTime = fileTime(info.st_mtim);
    data->ftLastAccessTime = fileTime(info.st_atim);
    data->ftLastWriteTime = fileTime(info.st_mtim);
    data->nFileSizeHigh = DWORD(uint64_t(info.st_size) >> 32);
    data->nFileSizeLow = DWORD(info.st_size);
}
}  // namespace tz::compat

inline DWORD GetLastError() { return tz::compat::lastError; }
inline void SetLastError(DWORD error) { tz::compat::lastError = error; }

// Paths are UTF-8 (std::filesystem::path::c_str() on Linux); the wchar_t forms convert.
inline HANDLE CreateFileW(const char* path, DWORD access, DWORD, void*, DWORD creation, DWORD, HANDLE) {
    int flags = O_CLOEXEC | ((access & GENERIC_WRITE) ? O_RDWR : O_RDONLY);
    switch (creation) {
    case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: flags |= O_CREAT; break;
    case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
    default: break;
    }
    const int fd = open(path, flags, 0644);
    if (fd < 0) {
        tz::compat::fail();
        return INVALID_HANDLE_VALUE;
    }
    return reinterpret_cast<HANDLE>(intptr_t(fd));
}
inline BOOL CloseHandle(HANDLE handle) { return close(tz::compat::descriptor(handle)) == 0 ? TRUE : tz::compat::fail(); }

// Positioned reads and writes (the offset comes from the OVERLAPPED); end of file reads 0 bytes.
inline BOOL ReadFile(HANDLE handle, void* buffer, DWORD length, DWORD* read, OVERLAPPED* overlapped) {
    const off_t offset = off_t(uint64_t(overlapped->OffsetHigh) << 32 | overlapped->Offset);
    DWORD total = 0;
    while (total < length) {
        const ssize_t count = pread(tz::compat::descriptor(handle), static_cast<char*>(buffer) + total, length - total,
                                    offset + total);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            *read = total;
            return tz::compat::fail();
        }
        if (count == 0) break;
        total += DWORD(count);
    }
    *read = total;
    if (!total && length) {
        SetLastError(ERROR_HANDLE_EOF);
        return FALSE;
    }
    return TRUE;
}
inline BOOL WriteFile(HANDLE handle, const void* buffer, DWORD length, DWORD* written, OVERLAPPED* overlapped) {
    const off_t offset = off_t(uint64_t(overlapped->OffsetHigh) << 32 | overlapped->Offset);
    DWORD total = 0;
    while (total < length) {
        const ssize_t count = pwrite(tz::compat::descriptor(handle), static_cast<const char*>(buffer) + total,
                                     length - total, offset + total);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            *written = total;
            return tz::compat::fail();
        }
        total += DWORD(count);
    }
    *written = total;
    return TRUE;
}
inline BOOL FlushFileBuffers(HANDLE handle) { return fsync(tz::compat::descriptor(handle)) == 0 ? TRUE : tz::compat::fail(); }
inline BOOL SetFileInformationByHandle(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS, void* info, DWORD) {
    const auto* end = static_cast<const FILE_END_OF_FILE_INFO*>(info);
    return ftruncate(tz::compat::descriptor(handle), off_t(end->EndOfFile.QuadPart)) == 0 ? TRUE : tz::compat::fail();
}
inline BOOL GetFileSizeEx(HANDLE handle, LARGE_INTEGER* size) {
    struct stat info {};
    if (fstat(tz::compat::descriptor(handle), &info) != 0) return tz::compat::fail();
    size->QuadPart = info.st_size;
    return TRUE;
}
inline BOOL GetFileTime(HANDLE handle, FILETIME* creation, FILETIME* access, FILETIME* write) {
    struct stat info {};
    if (fstat(tz::compat::descriptor(handle), &info) != 0) return tz::compat::fail();
    if (creation) *creation = tz::compat::fileTime(info.st_mtim);
    if (access) *access = tz::compat::fileTime(info.st_atim);
    if (write) *write = tz::compat::fileTime(info.st_mtim);
    return TRUE;
}
inline DWORD GetFileAttributesW(const char* path) {
    struct stat info {};
    if (stat(path, &info) != 0) {
        tz::compat::fail();
        return INVALID_FILE_ATTRIBUTES;
    }
    return S_ISDIR(info.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
inline BOOL GetFileAttributesExW(const char* path, GET_FILEEX_INFO_LEVELS, void* data) {
    struct stat info {};
    if (stat(path, &info) != 0) return tz::compat::fail();
    tz::compat::attributeData(info, static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(data));
    return TRUE;
}

inline HANDLE CreateFileW(const wchar_t* path, DWORD access, DWORD share, void* security, DWORD creation, DWORD flags,
                          HANDLE templateFile) {
    return CreateFileW(tz::compat::narrow(path).c_str(), access, share, security, creation, flags, templateFile);
}
inline DWORD GetFileAttributesW(const wchar_t* path) { return GetFileAttributesW(tz::compat::narrow(path).c_str()); }
inline BOOL GetFileAttributesExW(const wchar_t* path, GET_FILEEX_INFO_LEVELS level, void* data) {
    return GetFileAttributesExW(tz::compat::narrow(path).c_str(), level, data);
}

inline void GetSystemTimePreciseAsFileTime(FILETIME* time) {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    *time = tz::compat::fileTime(now);
}
inline void GetSystemTimeAsFileTime(FILETIME* time) { GetSystemTimePreciseAsFileTime(time); }

// UTC conversions (days from civil, Howard Hinnant's algorithm).
inline BOOL FileTimeToSystemTime(const FILETIME* file, SYSTEMTIME* system) {
    const uint64_t value = uint64_t(file->dwHighDateTime) << 32 | file->dwLowDateTime;
    const uint64_t milliseconds = value / 10000;
    int64_t days = int64_t(milliseconds / 86400000ull);
    const uint64_t dayMs = milliseconds % 86400000ull;
    system->wDayOfWeek = WORD((days + 1) % 7);  // 1601-01-01 was a Monday
    days -= 134774;                             // to days since 1970-01-01
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned dayOfEra = unsigned(days - era * 146097);
    const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthIndex = (5 * dayOfYear + 2) / 153;
    const unsigned day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
    const unsigned month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
    system->wYear = WORD(int64_t(yearOfEra) + era * 400 + (month <= 2));
    system->wMonth = WORD(month);
    system->wDay = WORD(day);
    system->wHour = WORD(dayMs / 3600000);
    system->wMinute = WORD(dayMs / 60000 % 60);
    system->wSecond = WORD(dayMs / 1000 % 60);
    system->wMilliseconds = WORD(dayMs % 1000);
    return TRUE;
}
inline BOOL SystemTimeToFileTime(const SYSTEMTIME* system, FILETIME* file) {
    if (system->wMonth < 1 || system->wMonth > 12 || system->wDay < 1 || system->wDay > 31 || system->wYear < 1601 ||
        system->wHour > 23 || system->wMinute > 59 || system->wSecond > 59 || system->wMilliseconds > 999)
        return FALSE;
    const int64_t year = int64_t(system->wYear) - (system->wMonth <= 2);
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = unsigned(year - era * 400);
    const unsigned month = system->wMonth;
    const unsigned dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + system->wDay - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    const int64_t days = era * 146097 + int64_t(dayOfEra) - 719468 + 134774;  // since 1601-01-01
    const uint64_t milliseconds = uint64_t(days) * 86400000ull + system->wHour * 3600000ull +
                                  system->wMinute * 60000ull + system->wSecond * 1000ull + system->wMilliseconds;
    *file = tz::compat::fileTime(milliseconds * 10000);
    return TRUE;
}
