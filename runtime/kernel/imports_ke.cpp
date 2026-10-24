// Kernel executive imports: threads, synchronization, TLS, memory, time.
#include "kernel.h"
#include <intrin.h>
#include "guest/host.h"
#include <thread>

using namespace tz;

namespace {

const int64_t* readTimeout(uint32_t pointer, int64_t& storage) {
    if (!pointer) return nullptr;
    storage = int64_t(load64(pointer));
    return &storage;
}

std::atomic<uint32_t> tlsBitmap[2];

}  // namespace

// ---- Threads ----------------------------------------------------------------

// ExCreateThread(handle*, stackSize, threadId*, xapiStartup, start, context, flags)
TZ_IMPORT(ExCreateThread) {
    const uint32_t handleOut = ctx.r3.u32, stackSize = ctx.r4.u32, idOut = ctx.r5.u32;
    const uint32_t startup = ctx.r6.u32, entry = ctx.r7.u32, argument = ctx.r8.u32, flags = ctx.r9.u32;
    uint32_t id = 0;
    TZ_TRACE("thread", "[thread] ExCreateThread entry %08X flags %08X", entry, flags);
    // Bits 24-29 are an affinity mask; the thread starts on its lowest processor.
    int processor = -1;
    if (const uint32_t affinity = (flags >> 24) & 0x3F) {
        unsigned long lowest = 0;
        _BitScanForward(&lowest, affinity);
        processor = int(lowest);
    }
    const uint32_t handle = createThread(stackSize, entry, argument, startup, flags & 1, id, nullptr, processor);
    if (handleOut) store32(handleOut, handle);
    if (idOut) store32(idOut, id);
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(ExTerminateThread) { throw ThreadExit{ctx.r3.u32}; }

TZ_IMPORT(KeResumeThread) {
    auto thread = threadByKthread(ctx.r3.u32);
    TZ_TRACE("thread", "[thread] KeResumeThread %08X -> %s", ctx.r3.u32, thread ? thread->name.c_str() : "unknown");
    ctx.r3.u64 = thread ? ResumeThread(thread->host) : 0;
}

TZ_IMPORT(NtResumeThread) {
    auto object = lookupHandleAs<ThreadObject>(ctx.r3.u32);
    TZ_TRACE("thread", "[thread] NtResumeThread %08X -> %s", ctx.r3.u32, object ? object->thread->name.c_str() : "unknown");
    if (!object) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    const DWORD previous = ResumeThread(object->thread->host);
    if (ctx.r4.u32) store32(ctx.r4.u32, previous);
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(NtSuspendThread) {
    auto object = lookupHandleAs<ThreadObject>(ctx.r3.u32);
    if (!object) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    const DWORD previous = SuspendThread(object->thread->host);
    if (ctx.r4.u32) store32(ctx.r4.u32, previous);
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(KeSetAffinityThread) {
    // (kthread, affinity, previous*). Host scheduling is left to Windows, but the
    // thread's processor number moves to the lowest allowed one: titles index
    // per-processor data with it (the audio workers' barrier needs distinct slots).
    auto thread = threadByKthread(ctx.r3.u32);
    TZ_TRACE("thread", "[thread] KeSetAffinityThread %08X (%s) affinity %X", ctx.r3.u32,
             thread ? thread->name.c_str() : "unknown", ctx.r4.u32);
    uint32_t previous = 0x3F;
    if (thread && thread->pcr) {
        uint8_t* number = memory->host(thread->pcr + 0x10C);
        previous = 1u << *number;
        const uint32_t affinity = ctx.r4.u32 & 0x3F;
        if (affinity) {
            unsigned long processor = 0;
            _BitScanForward(&processor, affinity);
            *number = uint8_t(processor);
        }
    }
    if (ctx.r5.u32) store32(ctx.r5.u32, previous);
    ctx.r3.u64 = previous;
}

TZ_IMPORT(KeSetBasePriorityThread) { ctx.r3.u64 = 0; }

TZ_IMPORT(KeDelayExecutionThread) {
    // (mode, alertable, interval*)
    const int64_t interval = ctx.r5.u32 ? int64_t(load64(ctx.r5.u32)) : 0;
    ctx.r3.u64 = delayExecution(ctx.r4.u32 != 0, interval);
}

TZ_IMPORT(KeGetCurrentProcessType) { ctx.r3.u64 = 1; }  // title process
TZ_IMPORT(KeEnableFpuExceptions) {}
TZ_IMPORT(KeEnterCriticalRegion) {}
TZ_IMPORT(KeLeaveCriticalRegion) {}
TZ_IMPORT(KiApcNormalRoutineNop) {}
TZ_IMPORT(KeLockL2) { ctx.r3.u64 = 0; }
TZ_IMPORT(KeUnlockL2) {}

// ---- Dispatcher objects -----------------------------------------------------------

TZ_IMPORT(KeSetEvent) {
    TZ_TRACE("evt", "[evt] %-20s KeSetEvent %08X type %u state %d lr %08X", currentThread()->name.c_str(), ctx.r3.u32,
             *memory->host(ctx.r3.u32), int32_t(load32(ctx.r3.u32 + 4)), uint32_t(ctx.lr));
    ctx.r3.u64 = uint32_t(setEvent(ctx.r3.u32));
}
TZ_IMPORT(KeResetEvent) {
    TZ_TRACE("evt", "[evt] %-20s KeResetEvent %08X state %d lr %08X", currentThread()->name.c_str(), ctx.r3.u32,
             int32_t(load32(ctx.r3.u32 + 4)), uint32_t(ctx.lr));
    ctx.r3.u64 = uint32_t(resetEvent(ctx.r3.u32));
}

TZ_IMPORT(KeInitializeSemaphore) {
    // (semaphore, count, limit): header type 5 plus limit at +0x10.
    const uint32_t semaphore = ctx.r3.u32;
    *memory->host(semaphore) = kSemaphore;
    *memory->host(semaphore + 2) = 5;
    store32(semaphore + 4, ctx.r4.u32);
    store32(semaphore + 8, semaphore + 8);
    store32(semaphore + 12, semaphore + 8);
    store32(semaphore + 16, ctx.r5.u32);
    wakeWaiters();
}

TZ_IMPORT(KeReleaseSemaphore) {
    // (semaphore, increment, adjustment, wait) -> previous count
    int32_t previous = 0;
    if (releaseSemaphore(ctx.r3.u32, int32_t(ctx.r5.u32), previous) != kStatusSuccess)
        fatal("KeReleaseSemaphore exceeded the semaphore limit");
    ctx.r3.u64 = uint32_t(previous);
}

TZ_IMPORT(KeWaitForSingleObject) {
    // (object, reason, mode, alertable, timeout*)
    int64_t storage;
    const int64_t* timeout = readTimeout(ctx.r7.u32, storage);
    TZ_TRACE("evt", "[evt] %-20s KeWaitForSingleObject %08X type %u state %d lr %08X%s", currentThread()->name.c_str(),
             ctx.r3.u32, *memory->host(ctx.r3.u32), int32_t(load32(ctx.r3.u32 + 4)), uint32_t(ctx.lr),
             timeout ? " (timeout)" : "");
    const uint32_t object = ctx.r3.u32;
    ctx.r3.u64 = waitForObjects({object}, false, ctx.r6.u32 != 0, timeout);
    TZ_TRACE("evt", "[evt] %-20s   woke %08X status %X", currentThread()->name.c_str(), object, ctx.r3.u32);
}

TZ_IMPORT(KeWaitForMultipleObjects) {
    // (count, objects*, waitType, reason, mode, alertable, timeout*, waitBlocks)
    std::vector<uint32_t> objects(ctx.r3.u32);
    for (uint32_t i = 0; i < objects.size(); ++i) objects[i] = load32(ctx.r4.u32 + i * 4);
    int64_t storage;
    const int64_t* timeout = readTimeout(ctx.r9.u32, storage);
    TZ_TRACE("evt", "[evt] %-20s KeWaitForMultipleObjects count %u first %08X all %u lr %08X",
             currentThread()->name.c_str(), uint32_t(objects.size()), objects.empty() ? 0 : objects[0],
             ctx.r5.u32 == 0, uint32_t(ctx.lr));
    ctx.r3.u64 = waitForObjects(objects, ctx.r5.u32 == 0, ctx.r8.u32 != 0, timeout);
    TZ_TRACE("evt", "[evt] %-20s KeWaitForMultipleObjects -> %08X", currentThread()->name.c_str(), ctx.r3.u32);
}

namespace {
struct EventObject : KernelObject {
    EventObject() : KernelObject(Kind::Event) {}
};
struct TimerObject : KernelObject {
    TimerObject() : KernelObject(Kind::Timer) {}
    std::mutex mutex;
    uint64_t generation = 0;
};

uint32_t dispatcherForHandle(uint32_t handle) {
    auto object = lookupHandle(handle);
    return object ? object->dispatcher : 0;
}
}  // namespace

TZ_IMPORT(NtCreateEvent) {
    // (handle*, attributes*, type, initialState). Type 0 = notification.
    auto object = std::make_shared<EventObject>();
    object->dispatcher = createDispatcherObject(ctx.r5.u32 ? kSynchronizationEvent : kNotificationEvent, 16,
                                                ctx.r6.u32 ? 1 : 0);
    store32(ctx.r3.u32, createHandle(object));
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(NtSetEvent) {
    const uint32_t dispatcher = dispatcherForHandle(ctx.r3.u32);
    if (!dispatcher) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    const int32_t previous = setEvent(dispatcher);
    if (ctx.r4.u32) store32(ctx.r4.u32, uint32_t(previous));
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(NtClearEvent) {
    const uint32_t dispatcher = dispatcherForHandle(ctx.r3.u32);
    if (!dispatcher) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    resetEvent(dispatcher);
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(NtWaitForSingleObjectEx) {
    // (handle, mode, alertable, timeout*)
    const uint32_t dispatcher = dispatcherForHandle(ctx.r3.u32);
    if (!dispatcher) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    int64_t storage;
    const int64_t* timeout = readTimeout(ctx.r6.u32, storage);
    ctx.r3.u64 = waitForObjects({dispatcher}, false, ctx.r5.u32 != 0, timeout);
}

TZ_IMPORT(NtCreateTimer) {
    // (handle*, attributes*, type). Type 0 = notification timer.
    auto object = std::make_shared<TimerObject>();
    object->dispatcher = createDispatcherObject(ctx.r5.u32 ? kSynchronizationTimer : kNotificationTimer, 0x28, 0);
    store32(ctx.r3.u32, createHandle(object));
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(NtSetTimerEx) {
    // (handle, dueTime*, apcRoutine, apcMode, apcContext, resume, period(ms), previousState*)
    auto timer = lookupHandleAs<TimerObject>(ctx.r3.u32);
    if (!timer) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    const int64_t due = int64_t(load64(ctx.r4.u32));
    const uint32_t routine = ctx.r5.u32, context = ctx.r7.u32;
    const uint32_t period = ctx.r9.u32;
    if (ctx.r10.u32) store32(ctx.r10.u32, load32(timer->dispatcher + 4));
    resetEvent(timer->dispatcher);
    uint64_t generation;
    {
        std::lock_guard lock(timer->mutex);
        generation = ++timer->generation;
    }
    GuestThread* owner = currentThread();
    std::thread([timer, due, routine, context, period, generation, owner] {
        auto next = std::chrono::steady_clock::now() +
                    std::chrono::nanoseconds(due < 0 ? -due * 100 : 0);
        for (;;) {
            std::this_thread::sleep_until(next);
            {
                std::lock_guard lock(timer->mutex);
                if (timer->generation != generation) return;
            }
            signalObject(timer->dispatcher, 1);
            if (routine) {
                FILETIME now;
                GetSystemTimeAsFileTime(&now);
                queueApc(owner, Apc{routine, {context, now.dwLowDateTime, now.dwHighDateTime}});
            }
            if (!period) return;
            next += std::chrono::milliseconds(period);
        }
    }).detach();
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(NtCancelTimer) {
    auto timer = lookupHandleAs<TimerObject>(ctx.r3.u32);
    if (!timer) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    {
        std::lock_guard lock(timer->mutex);
        ++timer->generation;
    }
    if (ctx.r4.u32) store32(ctx.r4.u32, load32(timer->dispatcher + 4));
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(NtClose) { ctx.r3.u64 = closeHandle(ctx.r3.u32) ? kStatusSuccess : kStatusInvalidHandle; }

TZ_IMPORT(NtDuplicateObject) {
    // (source, target*, options)
    const uint32_t handle = duplicateHandle(ctx.r3.u32);
    if (!handle) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    if (ctx.r4.u32) store32(ctx.r4.u32, handle);
    if (ctx.r5.u32 & 1) closeHandle(ctx.r3.u32);  // DUPLICATE_CLOSE_SOURCE
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(ObReferenceObjectByHandle) {
    // (handle, objectType, object*) -> guest body of the object
    auto object = lookupHandle(ctx.r3.u32);
    if (!object) { ctx.r3.u64 = kStatusInvalidHandle; return; }
    uint32_t body = object->dispatcher;
    if (!body) fatal("ObReferenceObjectByHandle on a non-dispatcher object");
    if (ctx.r5.u32) store32(ctx.r5.u32, body);
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(ObDereferenceObject) {}

// ---- Spin locks and IRQL ----------------------------------------------------------

namespace {
void spinAcquire(uint32_t lock) {
    auto* word = memory->host<volatile long>(lock);
    const long owner = long(_byteswap_ulong(currentContext().r13.u32));
    while (_InterlockedCompareExchange(word, owner, 0) != 0) YieldProcessor();
}
void spinRelease(uint32_t lock) { _InterlockedExchange(memory->host<volatile long>(lock), 0); }
}  // namespace

TZ_IMPORT(KfAcquireSpinLock) { spinAcquire(ctx.r3.u32); ctx.r3.u64 = 0; }
TZ_IMPORT(KfReleaseSpinLock) { spinRelease(ctx.r3.u32); }
TZ_IMPORT(KeAcquireSpinLockAtRaisedIrql) { spinAcquire(ctx.r3.u32); }
TZ_IMPORT(KeReleaseSpinLockFromRaisedIrql) { spinRelease(ctx.r3.u32); }
TZ_IMPORT(KeRaiseIrqlToDpcLevel) { ctx.r3.u64 = 0; }
TZ_IMPORT(KfLowerIrql) {}

// ---- Thread local storage -----------------------------------------------------------
// Dynamic slots follow the static TLS block of each thread.

TZ_IMPORT(KeTlsAlloc) {
    for (uint32_t index = 0; index < 64; ++index) {
        auto& word = tlsBitmap[index / 32];
        const uint32_t bit = 1u << (index % 32);
        if (word.fetch_or(bit) & bit) continue;
        ctx.r3.u64 = index;
        return;
    }
    ctx.r3.u64 = 0xFFFFFFFF;
}

TZ_IMPORT(KeTlsFree) {
    const uint32_t index = ctx.r3.u32;
    if (index < 64) tlsBitmap[index / 32].fetch_and(~(1u << (index % 32)));
    ctx.r3.u64 = index < 64;
}

TZ_IMPORT(KeTlsGetValue) {
    const uint32_t index = ctx.r3.u32;
    ctx.r3.u64 = index < 64 ? load32(currentThread()->tls + staticTlsSize() + index * 4) : 0;
}

TZ_IMPORT(KeTlsSetValue) {
    const uint32_t index = ctx.r3.u32;
    if (index < 64) store32(currentThread()->tls + staticTlsSize() + index * 4, ctx.r4.u32);
    ctx.r3.u64 = index < 64;
}

// ---- Memory -------------------------------------------------------------------------

TZ_IMPORT(NtAllocateVirtualMemory) {
    // (base*, size*, allocationType, protect, zeroBits). MEM_RESERVE 0x2000,
    // MEM_COMMIT 0x1000; the high bits select page sizes and are ignored.
    const uint32_t basePtr = ctx.r3.u32, sizePtr = ctx.r4.u32, type = ctx.r5.u32;
    const uint32_t requested = load32(basePtr), size = load32(sizePtr);
    TZ_TRACE("mem", "[mem] NtAllocateVirtualMemory base=%08X size=%X type=%X protect=%X", requested, size, type,
             ctx.r6.u32);
    if (!size || !(type & 0x3000)) { ctx.r3.u64 = kStatusInvalidParameter; return; }
    uint32_t address = requested;
    const auto existing = requested ? memory->queryVirtual(requested) : Memory::RegionInfo{};
    if (!requested || (type & 0x2000) || existing.state == 0x10000) {
        address = memory->reserveVirtual(size, requested);
        if (!address) { ctx.r3.u64 = kStatusNoMemory; return; }
    }
    uint32_t span = size;
    if (type & 0x1000) {
        // Commit whole pages covering [address, address + size). MEM_LARGE_PAGES
        // (0x20000000) means 64 KiB pages; callers rely on the rounded size.
        const uint32_t pageMask = type & 0x20000000 ? 0xFFFFu : 0xFFFu;
        const uint32_t first = address & ~pageMask;
        span = uint32_t(((uint64_t(address) + size + pageMask) & ~uint64_t(pageMask)) - first);
        address = first;
        if (!memory->commitVirtual(address, span)) { ctx.r3.u64 = kStatusNoMemory; return; }
    } else {
        span = (size + 0xFFFF) & ~0xFFFFu;
    }
    store32(basePtr, address);
    store32(sizePtr, span);
    TZ_TRACE("mem", "[mem]   -> %08X size %X", address, span);
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(NtFreeVirtualMemory) {
    // (base*, size*, freeType). MEM_DECOMMIT 0x4000, MEM_RELEASE 0x8000.
    const uint32_t address = load32(ctx.r3.u32);
    const uint32_t size = ctx.r4.u32 ? load32(ctx.r4.u32) : 0;
    TZ_TRACE("mem", "[mem] NtFreeVirtualMemory base=%08X size=%X type=%X", address, size, ctx.r5.u32);
    if (ctx.r5.u32 & 0x8000) {
        ctx.r3.u64 = memory->free(address) ? kStatusSuccess : kStatusInvalidParameter;
        return;
    }
    if (ctx.r5.u32 & 0x4000) {
        auto region = memory->queryVirtual(address);
        const uint32_t span = size ? size : region.allocationBase + memory->allocationSize(region.allocationBase) - address;
        ctx.r3.u64 = memory->decommitVirtual(address, span) ? kStatusSuccess : kStatusInvalidParameter;
        return;
    }
    ctx.r3.u64 = kStatusInvalidParameter;
}

TZ_IMPORT(NtQueryVirtualMemory) {
    // (address, MEMORY_BASIC_INFORMATION*)
    const uint32_t info = ctx.r4.u32;
    const auto region = memory->queryVirtual(ctx.r3.u32);
    const bool committed = region.state == 0x1000;
    store32(info + 0, region.base);
    store32(info + 4, region.allocationBase);
    store32(info + 8, region.allocationBase ? 4 : 0);   // allocation protect
    store32(info + 12, region.size);
    store32(info + 16, region.state);
    store32(info + 20, committed ? 4 : 0);               // PAGE_READWRITE
    store32(info + 24, region.allocationBase ? 0x20000 : 0);  // MEM_PRIVATE
    ctx.r3.u64 = kStatusSuccess;
}

TZ_IMPORT(MmAllocatePhysicalMemoryEx) {
    // (flags, size, protect, minAddress, maxAddress, alignment)
    const uint32_t size = ctx.r4.u32, protect = ctx.r5.u32;
    const uint32_t alias = protect & 0x20000000 ? 0xA0000000 : protect & 0x80000000 ? 0xC0000000 : 0xE0000000;
    const uint32_t pageSize = alias == 0xA0000000 ? 0x10000 : alias == 0xC0000000 ? 0x1000000 : 0x1000;
    const uint32_t alignment = (std::max)(ctx.r8.u32, pageSize);
    const uint32_t maximum = ctx.r7.u32 ? ctx.r7.u32 : Memory::kPhysicalSize - 1;
    const uint32_t address = memory->allocatePhysical((size + pageSize - 1) & ~(pageSize - 1), alignment, alias,
                                                      ctx.r6.u32, maximum);
    TZ_TRACE("mem", "[mem] MmAllocatePhysicalMemoryEx size=%X protect=%X -> %08X", size, protect, address);
    if (!address) logf("[mem] physical allocation of 0x%X bytes failed", size);
    ctx.r3.u64 = address;
}

TZ_IMPORT(MmFreePhysicalMemory) { memory->free(ctx.r4.u32); }
TZ_IMPORT(MmGetPhysicalAddress) {
    const uint32_t address = ctx.r3.u32;
    ctx.r3.u64 = address >= 0xA0000000 ? Memory::physicalAddress(address) : address;
}
TZ_IMPORT(MmQueryAddressProtect) { ctx.r3.u64 = 4; }  // PAGE_READWRITE

TZ_IMPORT(MmQueryStatistics) {
    // MM_STATISTICS: u32 length first; page counts follow.
    const uint32_t stats = ctx.r3.u32;
    const uint32_t length = load32(stats);
    if (length != 0x68) { ctx.r3.u64 = kStatusBufferTooSmall; return; }
    std::memset(memory->host(stats + 4), 0, length - 4);
    const uint32_t totalPages = Memory::kPhysicalSize / 0x1000;
    const uint32_t usedPages = uint32_t(memory->physicalBytesInUse() / 0x1000);
    store32(stats + 4, totalPages);
    store32(stats + 8, 0x200);                          // kernel pages
    store32(stats + 12, totalPages - usedPages - 0x200); // available pages
    store32(stats + 32, usedPages);                     // title physical pages
    ctx.r3.u64 = kStatusSuccess;
}

// ---- Time -----------------------------------------------------------------------------

TZ_IMPORT(KeQueryPerformanceFrequency) { ctx.r3.u64 = 49875000; }

TZ_IMPORT(KeQuerySystemTime) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    store64(ctx.r3.u32, uint64_t(now.dwHighDateTime) << 32 | now.dwLowDateTime);
}

// ---- Failure paths ----------------------------------------------------------------------

TZ_IMPORT(KeBugCheck) { fatal("KeBugCheck 0x%08X", ctx.r3.u32); }
TZ_IMPORT(KeBugCheckEx) {
    fatal("KeBugCheckEx 0x%08X (%08X %08X %08X %08X)", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}
TZ_IMPORT(DbgBreakPoint) { logf("[kernel] DbgBreakPoint from 0x%08X ignored", uint32_t(ctx.lr)); }
TZ_IMPORT(HalReturnToFirmware) {
    logf("[kernel] HalReturnToFirmware(%u): title requested shutdown", ctx.r3.u32);
    ExitProcess(0);
}
TZ_IMPORT(ExRegisterTitleTerminateNotification) {}
