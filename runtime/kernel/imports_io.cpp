// File system imports. Guest device roots map onto host directories.
#include "kernel.h"
#include "filesystem.h"
#include "guest/host.h"
#include <algorithm>
#include <map>
#include <shared_mutex>

using namespace tz;

namespace {

std::shared_mutex mountMutex;
std::map<std::string, std::filesystem::path> mounts;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(tolower(c)); });
    return text;
}

struct FileObject : KernelObject {
    FileObject() : KernelObject(Kind::File) {}
    ~FileObject() override { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::filesystem::path path;
    std::string guestPath;
    bool directory = false;
    std::mutex mutex;
    uint64_t position = 0;
};

uint64_t fileTime(const FILETIME& time) { return uint64_t(time.dwHighDateTime) << 32 | time.dwLowDateTime; }

// FILE_NETWORK_OPEN_INFORMATION (56 bytes).
void writeNetworkOpenInfo(uint32_t info, const WIN32_FILE_ATTRIBUTE_DATA& data) {
    const uint64_t size = uint64_t(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
    store64(info + 0, fileTime(data.ftCreationTime));
    store64(info + 8, fileTime(data.ftLastAccessTime));
    store64(info + 16, fileTime(data.ftLastWriteTime));
    store64(info + 24, fileTime(data.ftLastWriteTime));
    store64(info + 32, (size + 0xFFF) & ~0xFFFull);
    store64(info + 40, size);
    const bool directory = data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    store32(info + 48, directory ? 0x10 : 0x80);
    store32(info + 52, 0);
}

void completeIo(uint32_t ioStatus, uint32_t status, uint32_t information) {
    if (!ioStatus) return;
    store32(ioStatus, status);
    store32(ioStatus + 4, information);
}

// OBJECT_ATTRIBUTES { root directory handle; ANSI_STRING* name; attributes }
std::string objectName(uint32_t attributes, std::filesystem::path* relativeRoot) {
    const uint32_t root = load32(attributes);
    std::string name = loadAnsiString(load32(attributes + 4));
    if (root && relativeRoot) {
        auto object = lookupHandleAs<FileObject>(root);
        if (object) *relativeRoot = object->path;
    }
    return name;
}

bool resolveAttributes(uint32_t attributes, std::filesystem::path& path, std::string& guestName) {
    std::filesystem::path root;
    guestName = objectName(attributes, &root);
    if (!root.empty()) {
        std::string relative = guestName;
        std::replace(relative.begin(), relative.end(), '\\', '/');
        while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
        path = root / std::filesystem::u8path(relative);
        return true;
    }
    return resolveGuestPath(guestName, path);
}

uint32_t openFile(PPCContext& ctx, uint32_t handleOut, uint32_t access, uint32_t attributes, uint32_t ioStatus,
                  uint32_t disposition, uint32_t options) {
    std::filesystem::path path;
    std::string guestName;
    if (!resolveAttributes(attributes, path, guestName)) {
        TZ_TRACE("file", "[file] unmapped path %s", guestName.c_str());
        completeIo(ioStatus, kStatusObjectPathNotFound, 0);
        return kStatusObjectPathNotFound;
    }
    const DWORD attributesOnDisk = GetFileAttributesW(path.c_str());
    const bool exists = attributesOnDisk != INVALID_FILE_ATTRIBUTES;
    const bool isDirectory = exists && (attributesOnDisk & FILE_ATTRIBUTE_DIRECTORY);
    const bool wantDirectory = options & 0x1;     // FILE_DIRECTORY_FILE
    const bool wantFile = options & 0x40;         // FILE_NON_DIRECTORY_FILE
    uint32_t status = kStatusSuccess, information = 1;  // FILE_OPENED

    if (exists && wantFile && isDirectory) status = kStatusFileIsADirectory;
    else if (exists && wantDirectory && !isDirectory) status = kStatusNotADirectory;
    else if (!exists && (disposition == 1 || disposition == 4)) {
        status = std::filesystem::exists(path.parent_path()) ? kStatusObjectNameNotFound : kStatusObjectPathNotFound;
    } else if (exists && disposition == 2) {
        status = kStatusObjectNameCollision;
    }
    if (status != kStatusSuccess) {
        TZ_TRACE("file", "[file] open %s -> %08X", guestName.c_str(), status);
        completeIo(ioStatus, status, 0);
        return status;
    }

    auto object = std::make_shared<FileObject>();
    object->path = path;
    object->guestPath = guestName;
    if (isDirectory || (!exists && wantDirectory)) {
        if (!exists) {
            std::error_code error;
            std::filesystem::create_directories(path, error);
            information = 2;  // FILE_CREATED
        }
        object->directory = true;
    } else {
        const bool write = access & (0x40000000 | 0x2 | 0x4 | 0x10000);  // GENERIC_WRITE, FILE_WRITE_DATA, APPEND, DELETE
        DWORD creation = OPEN_EXISTING;
        switch (disposition) {
        case 0: creation = CREATE_ALWAYS; information = 3; break;           // supersede
        case 2: creation = CREATE_NEW; information = 2; break;              // create
        case 3: creation = OPEN_ALWAYS; information = exists ? 1 : 2; break;
        case 4: creation = TRUNCATE_EXISTING; information = 3; break;       // overwrite
        case 5: creation = CREATE_ALWAYS; information = exists ? 3 : 2; break;
        default: break;
        }
        object->handle = CreateFileW(path.c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0),
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, creation,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (object->handle == INVALID_HANDLE_VALUE) {
            status = GetLastError() == ERROR_ACCESS_DENIED ? kStatusAccessDenied : kStatusObjectNameNotFound;
            completeIo(ioStatus, status, 0);
            return status;
        }
    }
    store32(handleOut, createHandle(object));
    completeIo(ioStatus, kStatusSuccess, information);
    TZ_TRACE("file", "[file] open %s ok", guestName.c_str());
    if (traceEnabled("bt")) logf("[bt] open %s from%s", guestName.c_str(), guestBacktrace(ctx).c_str());
    (void)ctx;
    return kStatusSuccess;
}

void signalCompletion(uint32_t event, uint32_t apcRoutine, uint32_t apcContext, uint32_t ioStatus) {
    if (event) {
        auto object = lookupHandle(event);
        if (object && object->dispatcher) setEvent(object->dispatcher);
    }
    if (apcRoutine) queueApc(currentThread(), Apc{apcRoutine, {apcContext, ioStatus, 0}});
}

}  // namespace

namespace tz {

void mountDevice(const std::string& root, const std::filesystem::path& directory) {
    std::unique_lock lock(mountMutex);
    mounts[lower(root)] = directory;
}

void unmountDevice(const std::string& root) {
    std::unique_lock lock(mountMutex);
    mounts.erase(lower(root));
}

bool resolveGuestPath(const std::string& guestPath, std::filesystem::path& hostPath) {
    std::string path = guestPath;
    std::replace(path.begin(), path.end(), '/', '\\');
    std::string rootName, rest;
    const std::string lowered = lower(path);
    if (lowered.rfind("\\device\\cdrom0", 0) == 0) {
        rootName = "game";
        rest = path.substr(14);
    } else if (const size_t colon = path.find(':'); colon != std::string::npos) {
        rootName = lower(path.substr(0, colon));
        while (!rootName.empty() && rootName.front() == '\\') rootName.erase(rootName.begin());
        rest = path.substr(colon + 1);
    } else {
        return false;
    }
    std::filesystem::path root;
    {
        std::shared_lock lock(mountMutex);
        auto it = mounts.find(rootName);
        if (it == mounts.end()) return false;
        root = it->second;
    }
    std::filesystem::path result = root;
    size_t start = 0;
    while (start <= rest.size()) {
        const size_t end = rest.find('\\', start);
        const std::string part = rest.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") return false;
        if (!part.empty() && part != ".") result /= std::filesystem::u8path(part);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    hostPath = result;
    return true;
}

}  // namespace tz

TZ_IMPORT(NtCreateFile) {
    // (handle*, access, attributes*, ioStatus*, allocationSize*, fileAttributes,
    //  shareAccess, disposition, options [stack])
    const uint32_t options = load32(ctx.r1.u32 + 0x54);
    ctx.r3.u64 = openFile(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, options);
}

TZ_IMPORT(NtOpenFile) {
    // (handle*, access, attributes*, ioStatus*, shareAccess, options)
    ctx.r3.u64 = openFile(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 1, ctx.r8.u32);
}

TZ_IMPORT(NtReadFile) {
    // (handle, event, apcRoutine, apcContext, ioStatus*, buffer, length, offset*)
    auto file = lookupHandleAs<FileObject>(ctx.r3.u32);
    if (!file || file->directory) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    const uint32_t ioStatus = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32;
    std::lock_guard lock(file->mutex);
    if (ctx.r10.u32) {
        const uint64_t offset = load64(ctx.r10.u32);
        if (offset != 0xFFFFFFFFFFFFFFFEull) file->position = offset;  // -2: use the file pointer
    }
    OVERLAPPED overlapped{};
    overlapped.Offset = uint32_t(file->position);
    overlapped.OffsetHigh = uint32_t(file->position >> 32);
    DWORD read = 0;
    memory->prepareHostWrite(buffer, length);  // ReadFile cannot write write-watched pages
    const BOOL ok = ReadFile(file->handle, memory->host(buffer), length, &read, &overlapped);
    file->position += read;
    const uint32_t status = !ok && GetLastError() != ERROR_HANDLE_EOF ? kStatusUnsuccessful
                          : (read == 0 && length) ? kStatusEndOfFile : kStatusSuccess;
    TZ_TRACE("io", "[io] read %s @%llX len %X -> %X into %08X (lr %08X)", file->guestPath.c_str(),
             (unsigned long long)(file->position - read), length, read, buffer, uint32_t(ctx.lr));
    completeIo(ioStatus, status, read);
    signalCompletion(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ioStatus);
    ctx.r3.u64 = status;
}

TZ_IMPORT(NtWriteFile) {
    // (handle, event, apcRoutine, apcContext, ioStatus*, buffer, length, offset*)
    auto file = lookupHandleAs<FileObject>(ctx.r3.u32);
    if (!file || file->directory) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    const uint32_t ioStatus = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32;
    std::lock_guard lock(file->mutex);
    if (ctx.r10.u32) {
        const uint64_t offset = load64(ctx.r10.u32);
        if (offset != 0xFFFFFFFFFFFFFFFEull) file->position = offset;
    }
    OVERLAPPED overlapped{};
    overlapped.Offset = uint32_t(file->position);
    overlapped.OffsetHigh = uint32_t(file->position >> 32);
    DWORD written = 0;
    const BOOL ok = WriteFile(file->handle, memory->host(buffer), length, &written, &overlapped);
    file->position += written;
    const uint32_t status = ok ? kStatusSuccess : kStatusUnsuccessful;
    completeIo(ioStatus, status, written);
    signalCompletion(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ioStatus);
    ctx.r3.u64 = status;
}

TZ_IMPORT(NtFlushBuffersFile) {
    auto file = lookupHandleAs<FileObject>(ctx.r3.u32);
    if (file && !file->directory) FlushFileBuffers(file->handle);
    completeIo(ctx.r4.u32, kStatusSuccess, 0);
    ctx.r3.u64 = file ? kStatusSuccess : kStatusInvalidHandle;
}

TZ_IMPORT(NtQueryInformationFile) {
    // (handle, ioStatus*, info, length, class)
    auto file = lookupHandleAs<FileObject>(ctx.r3.u32);
    if (!file) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    const uint32_t ioStatus = ctx.r4.u32, info = ctx.r5.u32, length = ctx.r6.u32, infoClass = ctx.r7.u32;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    GetFileAttributesExW(file->path.c_str(), GetFileExInfoStandard, &data);
    const uint64_t size = uint64_t(data.nFileSizeHigh) << 32 | data.nFileSizeLow;
    uint32_t written = 0;
    switch (infoClass) {
    case 5:  // FileStandardInformation
        store64(info + 0, (size + 0xFFF) & ~0xFFFull);
        store64(info + 8, size);
        store32(info + 16, 1);
        *memory->host(info + 20) = 0;
        *memory->host(info + 21) = file->directory;
        written = 24;
        break;
    case 14:  // FilePositionInformation
        store64(info, file->position);
        written = 8;
        break;
    case 34:  // FileNetworkOpenInformation
        writeNetworkOpenInfo(info, data);
        written = 56;
        break;
    case 4:  // FileBasicInformation
        store64(info + 0, fileTime(data.ftCreationTime));
        store64(info + 8, fileTime(data.ftLastAccessTime));
        store64(info + 16, fileTime(data.ftLastWriteTime));
        store64(info + 24, fileTime(data.ftLastWriteTime));
        store32(info + 32, file->directory ? 0x10 : 0x80);
        written = 40;
        break;
    default:
        fatal("NtQueryInformationFile class %u", infoClass);
    }
    (void)length;
    completeIo(ioStatus, kStatusSuccess, written);
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(NtSetInformationFile) {
    // (handle, ioStatus*, info, length, class)
    auto file = lookupHandleAs<FileObject>(ctx.r3.u32);
    if (!file) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    const uint32_t ioStatus = ctx.r4.u32, info = ctx.r5.u32, infoClass = ctx.r7.u32;
    switch (infoClass) {
    case 14:  // FilePositionInformation
        file->position = load64(info);
        break;
    case 19:  // FileAllocationInformation
    case 20: {  // FileEndOfFileInformation
        FILE_END_OF_FILE_INFO end{};
        end.EndOfFile.QuadPart = int64_t(load64(info));
        if (infoClass == 20) SetFileInformationByHandle(file->handle, FileEndOfFileInfo, &end, sizeof(end));
        break;
    }
    case 13:  // FileDispositionInformation
        if (*memory->host(info)) {
            CloseHandle(file->handle);
            file->handle = INVALID_HANDLE_VALUE;
            std::error_code error;
            std::filesystem::remove(file->path, error);
        }
        break;
    case 4:  // FileBasicInformation
        break;
    default:
        fatal("NtSetInformationFile class %u", infoClass);
    }
    completeIo(ioStatus, kStatusSuccess, 0);
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(NtQueryFullAttributesFile) {
    // (attributes*, FILE_NETWORK_OPEN_INFORMATION*)
    std::filesystem::path path;
    std::string guestName;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!resolveAttributes(ctx.r3.u32, path, guestName) ||
        !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        TZ_TRACE("file", "[file] attributes %s -> not found", guestName.c_str());
        ctx.r3.u64 = kStatusObjectNameNotFound;
        return;
    }
    writeNetworkOpenInfo(ctx.r4.u32, data);
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(NtQueryVolumeInformationFile) {
    // (handle, ioStatus*, info, length, class)
    const uint32_t ioStatus = ctx.r4.u32, info = ctx.r5.u32, infoClass = ctx.r7.u32;
    uint32_t written = 0;
    switch (infoClass) {
    case 1:  // FileFsVolumeInformation
        store64(info + 0, 0);
        store32(info + 8, 0x54454E43);  // serial
        store32(info + 12, 0);
        *memory->host(info + 16) = 0;
        written = 18;
        break;
    case 3:  // FileFsSizeInformation: plenty of free space
        store64(info + 0, 0x100000);
        store64(info + 8, 0x80000);
        store32(info + 16, 32);
        store32(info + 20, 512);
        written = 24;
        break;
    case 5: {  // FileFsAttributeInformation
        store32(info + 0, 0x6);
        store32(info + 4, 255);
        const char16_t name[] = u"FATX";
        store32(info + 8, 8);
        for (int i = 0; i < 4; ++i) store16(info + 12 + i * 2, uint16_t(name[i]));
        written = 20;
        break;
    }
    default:
        fatal("NtQueryVolumeInformationFile class %u", infoClass);
    }
    completeIo(ioStatus, kStatusSuccess, written);
    ctx.r3.u64 = kStatusSuccess;
}
