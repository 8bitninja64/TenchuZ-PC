// Runtime library imports: critical sections, strings, time, exceptions, printf.
#include "kernel.h"
#include "ppc_recomp_shared.h"
#include <array>

using namespace tz;

// ---- Critical sections ------------------------------------------------------
// RTL_CRITICAL_SECTION: dispatcher header (16), lock count +0x10,
// recursion count +0x14, owning KTHREAD +0x18. Ownership is arbitrated on the
// host; the guest fields mirror it for code that inspects them.

namespace {
struct Stripe {
    std::mutex mutex;
    std::condition_variable condition;
};
std::array<Stripe, 64> stripes;
Stripe& stripeFor(uint32_t section) { return stripes[(section >> 4) % stripes.size()]; }

uint32_t owner() { return load32(currentContext().r13.u32 + 0x100); }

bool tryEnter(uint32_t section, uint32_t self) {
    const uint32_t current = load32(section + 0x18);
    if (current == self) {
        store32(section + 0x14, load32(section + 0x14) + 1);
        store32(section + 0x10, load32(section + 0x10) + 1);
        return true;
    }
    if (current) return false;
    store32(section + 0x18, self);
    store32(section + 0x14, 1);
    store32(section + 0x10, load32(section + 0x10) + 1);
    return true;
}
}  // namespace

PPC_FUNC(__imp__RtlInitializeCriticalSection) {
    const uint32_t section = ctx.r3.u32;
    std::memset(memory->host(section), 0, 28);
    *memory->host(section) = kSynchronizationEvent;
    *memory->host(section + 2) = 4;
    store32(section + 8, section + 8);
    store32(section + 12, section + 8);
    store32(section + 0x10, 0xFFFFFFFF);
}

PPC_FUNC(__imp__RtlEnterCriticalSection) {
    const uint32_t section = ctx.r3.u32, self = owner();
    auto& stripe = stripeFor(section);
    std::unique_lock lock(stripe.mutex);
    stripe.condition.wait(lock, [&] { return tryEnter(section, self); });
}

PPC_FUNC(__imp__RtlTryEnterCriticalSection) {
    const uint32_t section = ctx.r3.u32, self = owner();
    auto& stripe = stripeFor(section);
    std::lock_guard lock(stripe.mutex);
    ctx.r3.u64 = tryEnter(section, self);
}

PPC_FUNC(__imp__RtlLeaveCriticalSection) {
    const uint32_t section = ctx.r3.u32;
    auto& stripe = stripeFor(section);
    bool released = false;
    {
        std::lock_guard lock(stripe.mutex);
        if (load32(section + 0x18) != owner()) fatal("RtlLeaveCriticalSection by a non-owner");
        store32(section + 0x10, load32(section + 0x10) - 1);
        const uint32_t recursion = load32(section + 0x14) - 1;
        store32(section + 0x14, recursion);
        if (!recursion) {
            store32(section + 0x18, 0);
            released = true;
        }
    }
    if (released) stripe.condition.notify_all();
}

// ---- Strings and memory ---------------------------------------------------------

PPC_FUNC(__imp__RtlInitAnsiString) {
    const uint32_t target = ctx.r3.u32, source = ctx.r4.u32;
    const uint16_t length = uint16_t(source ? loadString(source, 0xFFFE).size() : 0);
    store16(target, length);
    store16(target + 2, source ? length + 1 : 0);
    store32(target + 4, source);
}

PPC_FUNC(__imp__RtlMultiByteToUnicodeN) {
    // (unicode, maxBytes, bytesWritten*, multiByte, multiByteBytes)
    const uint32_t count = (std::min)(ctx.r4.u32 / 2, ctx.r7.u32);
    for (uint32_t i = 0; i < count; ++i) store16(ctx.r3.u32 + i * 2, *memory->host(ctx.r6.u32 + i));
    if (ctx.r5.u32) store32(ctx.r5.u32, count * 2);
    ctx.r3.u64 = kStatusSuccess;
}

PPC_FUNC(__imp__RtlUnicodeToMultiByteN) {
    // (multiByte, maxBytes, bytesWritten*, unicode, unicodeBytes)
    const uint32_t count = (std::min)(ctx.r4.u32, ctx.r7.u32 / 2);
    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t c = load16(ctx.r6.u32 + i * 2);
        *memory->host(ctx.r3.u32 + i) = uint8_t(c < 0x100 ? c : '?');
    }
    if (ctx.r5.u32) store32(ctx.r5.u32, count);
    ctx.r3.u64 = kStatusSuccess;
}

PPC_FUNC(__imp__RtlCompareMemoryUlong) {
    // (source, length, pattern) -> number of matching bytes
    uint32_t matched = 0;
    for (uint32_t offset = 0; offset + 4 <= ctx.r4.u32; offset += 4, matched += 4)
        if (load32(ctx.r3.u32 + offset) != ctx.r5.u32) break;
    ctx.r3.u64 = matched;
}

PPC_FUNC(__imp__RtlFillMemoryUlong) {
    for (uint32_t offset = 0; offset + 4 <= ctx.r4.u32; offset += 4) store32(ctx.r3.u32 + offset, ctx.r5.u32);
}

PPC_FUNC(__imp__RtlNtStatusToDosError) {
    switch (ctx.r3.u32) {
    case kStatusSuccess: ctx.r3.u64 = kErrorSuccess; break;
    case kStatusNoSuchFile:
    case kStatusObjectNameNotFound: ctx.r3.u64 = kErrorFileNotFound; break;
    case kStatusObjectPathNotFound: ctx.r3.u64 = kErrorPathNotFound; break;
    case kStatusAccessDenied: ctx.r3.u64 = kErrorAccessDenied; break;
    case kStatusObjectNameCollision: ctx.r3.u64 = kErrorAlreadyExists; break;
    case kStatusEndOfFile: ctx.r3.u64 = 38; break;  // ERROR_HANDLE_EOF
    case kStatusNoMoreFiles: ctx.r3.u64 = kErrorNoMoreFiles; break;
    case kStatusPending: ctx.r3.u64 = kErrorIoPending; break;
    case kStatusInvalidParameter: ctx.r3.u64 = kErrorInvalidParameter; break;
    case kStatusNoMemory: ctx.r3.u64 = 8; break;  // ERROR_NOT_ENOUGH_MEMORY
    default:
        ctx.r3.u64 = ctx.r3.u32 & 0xFFFF;
        break;
    }
}

// ---- Time --------------------------------------------------------------------------
// TIME_FIELDS: year, month, day, hour, minute, second, milliseconds, weekday (u16).

PPC_FUNC(__imp__RtlTimeToTimeFields) {
    const uint64_t time = load64(ctx.r3.u32);
    FILETIME file{uint32_t(time), uint32_t(time >> 32)};
    SYSTEMTIME system{};
    FileTimeToSystemTime(&file, &system);
    const uint32_t fields = ctx.r4.u32;
    store16(fields + 0, system.wYear);
    store16(fields + 2, system.wMonth);
    store16(fields + 4, system.wDay);
    store16(fields + 6, system.wHour);
    store16(fields + 8, system.wMinute);
    store16(fields + 10, system.wSecond);
    store16(fields + 12, system.wMilliseconds);
    store16(fields + 14, system.wDayOfWeek);
}

PPC_FUNC(__imp__RtlTimeFieldsToTime) {
    const uint32_t fields = ctx.r3.u32;
    SYSTEMTIME system{};
    system.wYear = load16(fields + 0);
    system.wMonth = load16(fields + 2);
    system.wDay = load16(fields + 4);
    system.wHour = load16(fields + 6);
    system.wMinute = load16(fields + 8);
    system.wSecond = load16(fields + 10);
    system.wMilliseconds = load16(fields + 12);
    FILETIME file{};
    if (!SystemTimeToFileTime(&system, &file)) { ctx.r3.u64 = 0; return; }
    store64(ctx.r4.u32, uint64_t(file.dwHighDateTime) << 32 | file.dwLowDateTime);
    ctx.r3.u64 = 1;
}

// ---- Executable --------------------------------------------------------------------

PPC_FUNC(__imp__RtlImageXexHeaderField) {
    // (xexHeader, key) -> inline value or pointer to the field data
    const uint32_t header = ctx.r3.u32, key = ctx.r4.u32;
    const uint32_t count = load32(header + 20);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entry = header + 24 + i * 8;
        if (load32(entry) != key) continue;
        ctx.r3.u64 = (key & 0xFF) <= 1 ? load32(entry + 4) : header + load32(entry + 4);
        return;
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XexCheckExecutablePrivilege) { ctx.r3.u64 = 0; }

PPC_FUNC(__imp__XexGetModuleHandle) {
    // (name, handle*). Only the system modules are known.
    const std::string name = loadString(ctx.r3.u32);
    TZ_TRACE("xex", "[xex] XexGetModuleHandle(%s)", name.c_str());
    if (_stricmp(name.c_str(), "xam.xex") && _stricmp(name.c_str(), "xboxkrnl.exe")) {
        store32(ctx.r4.u32, 0);
        ctx.r3.u64 = kStatusNoSuchFile;
        return;
    }
    store32(ctx.r4.u32, _stricmp(name.c_str(), "xam.xex") ? 0x80000001 : 0x80000002);
    ctx.r3.u64 = kStatusSuccess;
}

PPC_FUNC(__imp__XexGetProcedureAddress) {
    // (module, ordinal, address*). Dynamic lookups are not supported.
    logf("[xex] XexGetProcedureAddress(module %08X, ordinal %u) unavailable", ctx.r3.u32, ctx.r4.u32);
    store32(ctx.r5.u32, 0);
    ctx.r3.u64 = 0xC0000139;  // STATUS_ENTRYPOINT_NOT_FOUND
}

// ---- Exceptions --------------------------------------------------------------------

PPC_FUNC(__imp__RtlRaiseException) {
    // EXCEPTION_RECORD: code +0, flags +4, record +8, address +0xC,
    // parameter count +0x10, parameters +0x14.
    const uint32_t record = ctx.r3.u32;
    const uint32_t code = load32(record);
    if (code == 0x406D1388 && load32(record + 0x14) == 0x1000) {
        // The XDK SetThreadName convention.
        const std::string name = loadString(load32(record + 0x18), 64);
        const uint32_t threadId = load32(record + 0x1C);
        if (threadId == 0xFFFFFFFF && currentThread()) currentThread()->name = name;
        TZ_TRACE("thread", "[thread] name %08X = %s", threadId, name.c_str());
        return;
    }
    fatal("RtlRaiseException code 0x%08X from 0x%08X", code, uint32_t(ctx.lr));
}

PPC_FUNC(__imp__RtlCaptureContext) { std::memset(memory->host(ctx.r3.u32), 0, 0x100); }
PPC_FUNC(__imp__RtlUnwind) { fatal("RtlUnwind: guest structured exception unwinding is not supported"); }
PPC_FUNC(__imp____C_specific_handler) { fatal("__C_specific_handler reached"); }

// ---- printf --------------------------------------------------------------------------

namespace {
void writeBounded(uint32_t buffer, uint32_t capacity, const std::string& text, bool terminateOnTruncate) {
    const size_t count = (std::min<size_t>)(text.size(), capacity);
    std::memcpy(memory->host(buffer), text.data(), count);
    if (count < capacity) *memory->host(buffer + uint32_t(count)) = 0;
    else if (terminateOnTruncate && capacity) *memory->host(buffer + capacity - 1) = 0;
}
}  // namespace

PPC_FUNC(__imp__sprintf) {
    auto args = GuestArgs::fromRegisters(ctx, 5);
    const std::string text = formatGuest(loadString(ctx.r4.u32), args);
    std::memcpy(memory->host(ctx.r3.u32), text.c_str(), text.size() + 1);
    ctx.r3.u64 = uint32_t(text.size());
}

PPC_FUNC(__imp___snprintf) {
    // (buffer, count, format, ...). Returns -1 on truncation (MSVC semantics).
    auto args = GuestArgs::fromRegisters(ctx, 6);
    const std::string text = formatGuest(loadString(ctx.r5.u32), args);
    writeBounded(ctx.r3.u32, ctx.r4.u32, text, false);
    ctx.r3.u64 = text.size() > ctx.r4.u32 ? uint64_t(-1) : text.size();
}

PPC_FUNC(__imp___vsnprintf) {
    // (buffer, count, format, va_list)
    auto args = GuestArgs::fromVaList(ctx.r6.u32);
    const std::string text = formatGuest(loadString(ctx.r5.u32), args);
    writeBounded(ctx.r3.u32, ctx.r4.u32, text, false);
    ctx.r3.u64 = text.size() > ctx.r4.u32 ? uint64_t(-1) : text.size();
}

PPC_FUNC(__imp__DbgPrint) {
    auto args = GuestArgs::fromRegisters(ctx, 4);
    std::string text = formatGuest(loadString(ctx.r3.u32), args);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (traceEnabled("dbg")) logf("[guest] %s", text.c_str());
    ctx.r3.u64 = kStatusSuccess;
}
