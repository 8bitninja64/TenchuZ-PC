#include "kernel.h"
#include "xex.h"
#include "guest/host.h"
#include "module_loader.h"
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <shlobj.h>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace tz {
namespace {

std::filesystem::path gameDir;
std::filesystem::path userDir;
uint32_t xexHeader = 0;
uint32_t entryPoint = 0;
uint32_t defaultStackSize = 0x40000;
std::vector<uint8_t> tlsTemplate;
uint32_t tlsSize = 0;

thread_local std::shared_ptr<GuestThread> tlsThread;

// ---- Handles -------------------------------------------------------------

std::mutex handleMutex;
std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> handles;
uint32_t nextHandle = 0xF8000004;

// ---- Dispatcher ------------------------------------------------------------

std::mutex dispatcherMutex;
std::condition_variable dispatcherCondition;
struct MutantState { uint32_t owner = 0; uint32_t recursion = 0; };
std::unordered_map<uint32_t, MutantState> mutants;

uint8_t objectType(uint32_t dispatcher) { return *memory->host(dispatcher); }
int32_t signalState(uint32_t dispatcher) { return int32_t(load32(dispatcher + 4)); }
void setSignalState(uint32_t dispatcher, int32_t value) { store32(dispatcher + 4, uint32_t(value)); }

uint32_t currentKthread() { return currentThread() ? currentThread()->kthread : 0; }

bool isSignaled(uint32_t dispatcher) {
    switch (objectType(dispatcher)) {
    case kMutant: {
        auto it = mutants.find(dispatcher);
        return signalState(dispatcher) > 0 || (it != mutants.end() && it->second.owner == currentKthread());
    }
    default:
        return signalState(dispatcher) > 0;
    }
}

void acquire(uint32_t dispatcher) {
    switch (objectType(dispatcher)) {
    case kSynchronizationEvent:
    case kSynchronizationTimer:
        setSignalState(dispatcher, 0);
        break;
    case kSemaphore:
        setSignalState(dispatcher, signalState(dispatcher) - 1);
        break;
    case kMutant: {
        auto& state = mutants[dispatcher];
        state.owner = currentKthread();
        ++state.recursion;
        setSignalState(dispatcher, 0);
        break;
    }
    default:
        break;
    }
}

using Clock = std::chrono::steady_clock;

// FILETIME epoch to now, in 100ns units.
int64_t systemTime100ns() {
    FILETIME time;
    GetSystemTimePreciseAsFileTime(&time);
    return int64_t(uint64_t(time.dwHighDateTime) << 32 | time.dwLowDateTime);
}

Clock::time_point deadlineFor(int64_t timeout) {
    if (timeout < 0) return Clock::now() + std::chrono::nanoseconds(-timeout * 100);
    const int64_t remaining = timeout - systemTime100ns();
    return Clock::now() + std::chrono::nanoseconds((std::max<int64_t>)(remaining, 0) * 100);
}

bool runPendingApcs() {
    auto* thread = currentThread();
    if (!thread) return false;
    bool ran = false;
    for (;;) {
        Apc apc;
        {
            std::lock_guard lock(thread->apcMutex);
            if (thread->apcs.empty()) break;
            apc = thread->apcs.front();
            thread->apcs.pop_front();
        }
        callGuest(apc.routine, {apc.arguments[0], apc.arguments[1], apc.arguments[2]});
        ran = true;
    }
    return ran;
}

bool hasPendingApcs() {
    auto* thread = currentThread();
    if (!thread) return false;
    std::lock_guard lock(thread->apcMutex);
    return !thread->apcs.empty();
}

// ---- Threads ---------------------------------------------------------------

std::mutex threadMutex;
std::map<uint32_t, std::weak_ptr<GuestThread>> threadsByKthread;
std::atomic<uint32_t> nextThreadId{1};
std::atomic<uint32_t> nextPcrSlot{0};

// Exited threads return their PCR page, stack and TLS block for reuse.
std::mutex recycleMutex;
std::vector<uint32_t> freePcrSlots;
std::vector<uint32_t> freeTlsBlocks;

uint32_t takePcrSlot() {
    std::lock_guard lock(recycleMutex);
    if (!freePcrSlots.empty()) {
        const uint32_t slot = freePcrSlots.back();
        freePcrSlots.pop_back();
        return slot;
    }
    return nextPcrSlot.fetch_add(1);
}

uint32_t takeTlsBlock() {
    std::lock_guard lock(recycleMutex);
    if (!freeTlsBlocks.empty()) {
        const uint32_t block = freeTlsBlocks.back();
        freeTlsBlocks.pop_back();
        return block;
    }
    return memory->allocateKernel(tlsSize + 64 * 4, 16);
}

void recycleThreadState(const GuestThread& thread) {
    std::lock_guard lock(recycleMutex);
    freePcrSlots.push_back((thread.pcr - Memory::kPcrBegin) / 0x1000);
    freeTlsBlocks.push_back(thread.tls);
    memory->free(thread.stackBase);
}

// Per-thread kernel structures. The PCR page holds the TLS pointer at +0,
// the current KTHREAD at +0x100 and the processor number at +0x10C.
void setupThreadState(GuestThread& thread, uint32_t stackSize, uint32_t processor) {
    const uint32_t slot = takePcrSlot();
    if (slot >= 0x100) fatal("Too many live guest threads");
    thread.pcr = Memory::kPcrBegin + slot * 0x1000;
    if (slot == 0 && !memory->commit(Memory::kPcrBegin, 0x100000)) fatal("Cannot commit PCR pages");
    std::memset(memory->host(thread.pcr), 0, 0x1000);
    thread.stackSize = stackSize;
    thread.stackBase = memory->allocateStack(stackSize);
    if (!thread.stackBase) fatal("Cannot allocate a %u byte guest stack", stackSize);
    thread.kthread = createDispatcherObject(kThread, 0x200, 0);
    thread.tls = takeTlsBlock();
    if (!thread.kthread || !thread.tls) fatal("Kernel memory exhausted");
    std::memcpy(memory->host(thread.tls), tlsTemplate.data(), tlsTemplate.size());
    thread.id = nextThreadId.fetch_add(1);

    store32(thread.pcr + 0x000, thread.tls);
    store32(thread.pcr + 0x100, thread.kthread);
    *memory->host(thread.pcr + 0x10C) = uint8_t(processor);
    store32(thread.kthread + 0x5C, thread.stackBase + stackSize);
    store32(thread.kthread + 0x60, thread.stackBase);
    store32(thread.kthread + 0x68, thread.tls);
    store32(thread.kthread + 0x14C, thread.id);

    thread.context = PPCContext{};
    thread.context.r1.u64 = thread.stackBase + stackSize - 0x100;
    thread.context.r13.u64 = thread.pcr;
    thread.context.fpscr.loadFromHost();
    std::lock_guard lock(threadMutex);
    threadsByKthread[thread.kthread] = std::weak_ptr<GuestThread>();
}

void registerThread(const std::shared_ptr<GuestThread>& thread) {
    std::lock_guard lock(threadMutex);
    threadsByKthread[thread->kthread] = thread;
}

void finishThread(ThreadObject& object, uint32_t code) {
    object.exitCode = code;
    signalObject(object.dispatcher, 1);
}

}  // namespace

// ---- Memory helpers ----------------------------------------------------------

uint32_t load32(uint32_t address) { return memory->read32(address); }
void store32(uint32_t address, uint32_t value) { memory->write32(address, value); }
uint16_t load16(uint32_t address) { return memory->read16(address); }
void store16(uint32_t address, uint16_t value) { memory->write16(address, value); }
uint64_t load64(uint32_t address) { return memory->read64(address); }
void store64(uint32_t address, uint64_t value) { memory->write64(address, value); }

std::string loadString(uint32_t address, size_t maxLength) {
    std::string result;
    if (!address) return result;
    for (size_t i = 0; i < maxLength; ++i) {
        const char c = char(*memory->host(address + uint32_t(i)));
        if (!c) break;
        result.push_back(c);
    }
    return result;
}

std::u16string loadWideString(uint32_t address, size_t maxLength) {
    std::u16string result;
    if (!address) return result;
    for (size_t i = 0; i < maxLength; ++i) {
        const char16_t c = char16_t(load16(address + uint32_t(i * 2)));
        if (!c) break;
        result.push_back(c);
    }
    return result;
}

std::string loadAnsiString(uint32_t ansiString) {
    if (!ansiString) return {};
    const uint16_t length = load16(ansiString);
    const uint32_t buffer = load32(ansiString + 4);
    if (!buffer) return {};
    return std::string(memory->host<const char>(buffer), length);
}

// ---- Diagnostics ---------------------------------------------------------------

bool traceEnabled(const char* category) {
    static const std::string filter = [] {
        char value[256]{};
        return GetEnvironmentVariableA("TENCHU_TRACE", value, sizeof(value)) ? std::string(value) : std::string();
    }();
    if (filter.empty()) return false;
    if (filter == "all") return true;
    // Categories are string literals: remember the answer per pointer, so hot paths
    // stay cheap while tracing (and profiles stay representative).
    thread_local std::pair<const char*, bool> known[64];
    thread_local int knownCount = 0;
    for (int i = 0; i < knownCount; ++i)
        if (known[i].first == category) return known[i].second;
    const bool enabled = filter.find(category) != std::string::npos;
    if (knownCount < 64) known[knownCount++] = {category, enabled};
    return enabled;
}

void logf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    _lock_file(stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    _unlock_file(stderr);
    va_end(args);
}

std::string guestBacktrace(const PPCContext& ctx, int depth) {
    std::string result;
    char cell[12];
    snprintf(cell, sizeof(cell), " %08X", uint32_t(ctx.lr));
    result += cell;
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < depth; ++i) {
        if (sp < 0x40000000 || sp >= 0x80000000) break;
        const uint32_t previous = load32(sp);
        if (previous <= sp || previous >= 0x80000000) break;
        const uint32_t lr = load32(previous - 8);
        if (lr < 0x82000000 || lr >= 0x83000000) break;
        snprintf(cell, sizeof(cell), " %08X", lr);
        result += cell;
        sp = previous;
    }
    return result;
}

void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    _lock_file(stderr);
    fputs("[FATAL] ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    if (auto* thread = currentThread()) {
        const auto& ctx = thread->context;
        fprintf(stderr, "thread %u (%s) last guest function 0x%08X lr 0x%08X\nrecent:", thread->id,
                thread->name.c_str(), ctx.lastFunction, uint32_t(ctx.lr));
        const uint32_t count = (std::min)(ctx.traceIndex, 64u);
        for (uint32_t i = 0; i < count; ++i) fprintf(stderr, " %08X", ctx.trace[(ctx.traceIndex - count + i) & 63]);
        fputc('\n', stderr);
    }
    _unlock_file(stderr);
    fflush(stderr);
    va_end(args);
    ExitProcess(1);
}

// ---- Threads -------------------------------------------------------------------

GuestThread* currentThread() { return tlsThread.get(); }
PPCContext& currentContext() {
    if (!tlsThread) fatal("Guest context requested on a host-only thread");
    return tlsThread->context;
}

uint32_t callGuest(uint32_t function, std::initializer_list<uint64_t> arguments) {
    auto& ctx = currentContext();
    if (arguments.size() > 8) fatal("callGuest supports eight arguments");
    PPCFunc* host = (function & 3) == 0 && function >= PPC_CODE_BASE && function < PPC_CODE_BASE + PPC_CODE_SIZE
                        ? PPC_LOOKUP_FUNC(memory->base(), function) : nullptr;
    if (!host) fatal("Call to untranslated guest function 0x%08X", function);
    const PPCContext saved = ctx;
    // Leave the caller's frame untouched.
    ctx.r1.u64 = (ctx.r1.u64 - 0x200) & ~uint64_t(15);
    int index = 0;
    PPCRegister* registers[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    for (uint64_t argument : arguments) registers[index++]->u64 = argument;
    host(ctx, memory->base());
    const uint32_t result = ctx.r3.u32;
    ctx = saved;
    return result;
}

std::shared_ptr<GuestThread> threadByKthread(uint32_t kthread) {
    std::lock_guard lock(threadMutex);
    auto it = threadsByKthread.find(kthread);
    return it == threadsByKthread.end() ? nullptr : it->second.lock();
}

std::shared_ptr<GuestThread> attachThread(uint32_t stackSize, const char* name) {
    auto thread = std::make_shared<GuestThread>();
    setupThreadState(*thread, stackSize, 0);
    thread->name = name;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread->host, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    registerThread(thread);
    tlsThread = thread;
    return thread;
}

std::shared_ptr<GuestThread> createSystemThread(const char* name) {
    return attachThread(0x40000, name);
}

namespace {
struct ThreadStart {
    std::shared_ptr<ThreadObject> object;
};

DWORD WINAPI guestThreadMain(void* parameter) {
    std::unique_ptr<ThreadStart> start(static_cast<ThreadStart*>(parameter));
    auto object = start->object;
    tlsThread = object->thread;
    auto& ctx = tlsThread->context;
    uint32_t code = 0;
    try {
        if (object->startup)
            code = callGuest(object->startup, {object->entry, object->argument});
        else
            code = callGuest(object->entry, {object->argument});
    } catch (const ThreadExit& exit) {
        code = exit.code;
    }
    (void)ctx;
    TZ_TRACE("thread", "[thread] %u exited with %u", tlsThread->id, code);
    finishThread(*object, code);
    {
        // The KTHREAD stays valid for waiters; the rest is reused.
        std::lock_guard lock(threadMutex);
        threadsByKthread.erase(tlsThread->kthread);
    }
    recycleThreadState(*tlsThread);
    tlsThread.reset();
    return 0;
}
}  // namespace

uint32_t createThread(uint32_t stackSize, uint32_t entry, uint32_t argument, uint32_t startup, bool suspended,
                      uint32_t& threadId, uint32_t* kthreadOut, int processor) {
    auto object = std::make_shared<ThreadObject>();
    object->thread = std::make_shared<GuestThread>();
    object->entry = entry;
    object->argument = argument;
    object->startup = startup;
    if (!stackSize) stackSize = defaultStackSize;
    stackSize = (std::max)(stackSize, 0x10000u);
    static std::atomic<uint32_t> nextProcessor{0};
    setupThreadState(*object->thread, stackSize, processor >= 0 ? uint32_t(processor) : nextProcessor.fetch_add(1) % 6);
    object->thread->object = object;
    object->dispatcher = object->thread->kthread;
    char name[32];
    snprintf(name, sizeof(name), "guest-%08X", entry);
    object->thread->name = name;
    registerThread(object->thread);

    auto* start = new ThreadStart{object};
    // Host stack must hold deep recursive translated call chains.
    HANDLE host = CreateThread(nullptr, 16 * 1024 * 1024, guestThreadMain, start,
                               CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!host) fatal("CreateThread failed");
    object->thread->host = host;
    threadId = object->thread->id;
    if (kthreadOut) *kthreadOut = object->thread->kthread;
    const uint32_t handle = createHandle(object);
    TZ_TRACE("thread", "[thread] create %u entry=%08X arg=%08X startup=%08X stack=%X%s", threadId, entry,
             argument, startup, stackSize, suspended ? " suspended" : "");
    if (!suspended) ResumeThread(host);
    return handle;
}

void queueApc(GuestThread* thread, const Apc& apc) {
    {
        std::lock_guard lock(thread->apcMutex);
        thread->apcs.push_back(apc);
    }
    wakeWaiters();
}

// ---- Handles -------------------------------------------------------------------

uint32_t createHandle(std::shared_ptr<KernelObject> object) {
    std::lock_guard lock(handleMutex);
    const uint32_t handle = nextHandle;
    nextHandle += 4;
    handles.emplace(handle, std::move(object));
    return handle;
}

std::shared_ptr<KernelObject> lookupHandle(uint32_t handle) {
    if (handle == 0xFFFFFFFE) {
        auto* thread = currentThread();
        return thread ? std::static_pointer_cast<KernelObject>(thread->object.lock()) : nullptr;
    }
    std::lock_guard lock(handleMutex);
    auto it = handles.find(handle);
    return it == handles.end() ? nullptr : it->second;
}

bool closeHandle(uint32_t handle) {
    std::lock_guard lock(handleMutex);
    return handles.erase(handle) != 0;
}

uint32_t duplicateHandle(uint32_t handle) {
    auto object = lookupHandle(handle);
    return object ? createHandle(object) : 0;
}

// ---- Dispatcher ------------------------------------------------------------------

uint32_t createDispatcherObject(uint8_t type, uint32_t size, int32_t signal) {
    const uint32_t address = memory->allocateKernel(size, 16);
    if (!address) return 0;
    *memory->host(address) = type;
    *memory->host(address + 2) = uint8_t(size / 4);
    setSignalState(address, signal);
    // Empty wait list: both links point at the list head.
    store32(address + 8, address + 8);
    store32(address + 12, address + 8);
    return address;
}

void wakeWaiters() {
    { std::lock_guard lock(dispatcherMutex); }
    dispatcherCondition.notify_all();
}

void signalObject(uint32_t dispatcher, int32_t state) {
    {
        std::lock_guard lock(dispatcherMutex);
        setSignalState(dispatcher, state);
    }
    dispatcherCondition.notify_all();
}

int32_t setEvent(uint32_t event) {
    int32_t previous;
    {
        std::lock_guard lock(dispatcherMutex);
        previous = signalState(event);
        setSignalState(event, 1);
    }
    dispatcherCondition.notify_all();
    return previous;
}

int32_t resetEvent(uint32_t event) {
    std::lock_guard lock(dispatcherMutex);
    const int32_t previous = signalState(event);
    setSignalState(event, 0);
    return previous;
}

int32_t pulseEvent(uint32_t event) {
    const int32_t previous = setEvent(event);
    std::this_thread::yield();
    resetEvent(event);
    return previous;
}

uint32_t releaseSemaphore(uint32_t semaphore, int32_t adjustment, int32_t& previous) {
    {
        std::lock_guard lock(dispatcherMutex);
        previous = signalState(semaphore);
        const int32_t limit = int32_t(load32(semaphore + 16));
        if (int64_t(previous) + adjustment > limit) return kStatusSemaphoreLimitExceeded;
        setSignalState(semaphore, previous + adjustment);
    }
    dispatcherCondition.notify_all();
    return kStatusSuccess;
}

uint32_t releaseMutant(uint32_t mutant, int32_t& previous) {
    {
        std::lock_guard lock(dispatcherMutex);
        previous = signalState(mutant);
        auto it = mutants.find(mutant);
        if (it == mutants.end() || it->second.owner != currentKthread()) return kStatusMutantNotOwned;
        if (--it->second.recursion == 0) {
            mutants.erase(it);
            setSignalState(mutant, 1);
        }
    }
    dispatcherCondition.notify_all();
    return kStatusSuccess;
}

uint32_t waitForObjects(const std::vector<uint32_t>& dispatchers, bool waitAll, bool alertable,
                        const int64_t* timeout) {
    const bool poll = timeout && *timeout == 0;
    const bool finite = timeout && *timeout != 0;
    const auto deadline = finite ? deadlineFor(*timeout) : Clock::time_point::max();
    std::unique_lock lock(dispatcherMutex);
    for (;;) {
        if (alertable && hasPendingApcs()) {
            lock.unlock();
            runPendingApcs();
            return kStatusUserApc;
        }
        if (waitAll) {
            bool all = !dispatchers.empty();
            for (uint32_t object : dispatchers) all = all && isSignaled(object);
            if (all) {
                for (uint32_t object : dispatchers) acquire(object);
                return kStatusSuccess;
            }
        } else {
            for (size_t i = 0; i < dispatchers.size(); ++i) {
                if (!isSignaled(dispatchers[i])) continue;
                acquire(dispatchers[i]);
                return kStatusSuccess + uint32_t(i);
            }
        }
        if (poll || (finite && Clock::now() >= deadline)) return kStatusTimeout;
        // Recheck periodically: guest code may change signal states inline.
        const auto slice = Clock::now() + std::chrono::milliseconds(20);
        dispatcherCondition.wait_until(lock, (std::min)(slice, deadline));
    }
}

uint32_t delayExecution(bool alertable, int64_t interval) {
    if (alertable) {
        if (hasPendingApcs()) {
            runPendingApcs();
            return kStatusUserApc;
        }
        const uint32_t status = waitForObjects({}, false, true, &interval);
        return status == kStatusTimeout ? kStatusSuccess : status;
    }
    if (interval == 0) {
        SwitchToThread();
        return kStatusSuccess;
    }
    const auto deadline = deadlineFor(interval);
    std::this_thread::sleep_until(deadline);
    return kStatusSuccess;
}

// ---- Bring-up --------------------------------------------------------------------

const std::filesystem::path& gameDirectory() { return gameDir; }
const std::filesystem::path& userDirectory() { return userDir; }
uint32_t xexHeaderAddress() { return xexHeader; }
uint32_t xexEntryPoint() { return entryPoint; }
uint32_t staticTlsSize() { return tlsSize; }

void installDataImports(const XexImage& xex);
void startTimeStampBundle(uint32_t bundle);

void initializeKernel(const std::filesystem::path& directory) {
    gameDir = std::filesystem::weakly_canonical(directory);
    wchar_t* appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        userDir = std::filesystem::path(appData) / L"TenchuZRecomp";
        CoTaskMemFree(appData);
    } else {
        userDir = gameDir.parent_path() / "user";
    }
    std::filesystem::create_directories(userDir);

    memory = new Memory();
    installMmioFaultHandler();
    const GameModule* module = gameModule();
    if (!module) throw std::runtime_error("No game module is loaded");
    const XexImage xex = loadXex(gameDir / "default.xex");
    if (xex.loadAddress != PPC_IMAGE_BASE || xex.image.size() > PPC_IMAGE_SIZE || xex.entryPoint != module->entryPoint)
        throw std::runtime_error("default.xex does not match the translated executable");
    entryPoint = xex.entryPoint;
    if (!memory->commit(uint32_t(PPC_IMAGE_BASE), uint32_t(PPC_IMAGE_SIZE)))
        throw std::runtime_error("Cannot map the game image");
    std::memcpy(memory->host(uint32_t(PPC_IMAGE_BASE)), xex.image.data(), xex.image.size());

    xexHeader = memory->allocateKernel(uint32_t(xex.header.size()), 16);
    std::memcpy(memory->host(xexHeader), xex.header.data(), xex.header.size());

    uint32_t tlsInfo = 0;
    if (xex.field(0x20104, tlsInfo)) {
        const uint32_t rawAddress = load32(xexHeader + tlsInfo + 4);
        tlsSize = (load32(xexHeader + tlsInfo + 8) + 15) & ~15u;
        const uint32_t rawSize = load32(xexHeader + tlsInfo + 12);
        tlsTemplate.assign(tlsSize, 0);
        std::memcpy(tlsTemplate.data(), memory->host(rawAddress), (std::min)(rawSize, tlsSize));
    }
    uint32_t stackField = 0;
    if (xex.field(0x20200, stackField)) defaultStackSize = (std::max)(stackField, 0x10000u);

    // Function lookup table for indirect calls.
    const uint32_t table = uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE);
    if (!memory->commit(table, uint32_t(PPC_CODE_SIZE * 2)))
        throw std::runtime_error("Cannot map the function table");
    size_t functions = 0;
    for (auto* mapping = module->functions; mapping->host; ++mapping, ++functions)
        PPC_LOOKUP_FUNC(memory->base(), mapping->guest) = mapping->host;

    installDataImports(xex);
    logf("[kernel] image loaded: entry 0x%08X, %zu functions, TLS %u bytes, default stack 0x%X", entryPoint,
         functions, tlsSize, defaultStackSize);
}

namespace {
// Periodically prints where every guest thread is (TENCHU_TRACE=watch).
void startWatchdog() {
    if (!traceEnabled("watch")) return;
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::vector<std::shared_ptr<GuestThread>> threads;
            {
                std::lock_guard lock(threadMutex);
                for (const auto& [kthread, weak] : threadsByKthread)
                    if (auto thread = weak.lock()) threads.push_back(thread);
            }
            for (const auto& thread : threads) {
                const auto& ctx = thread->context;
                std::string recent;
                char cell[12];
                const uint32_t count = (std::min)(ctx.traceIndex, 64u);
                for (uint32_t i = count > 12 ? count - 12 : 0; i < count; ++i) {
                    const uint32_t entry = ctx.trace[(ctx.traceIndex - count + i) & 63];
                    if (entry >= 0x82653F00 && entry < 0x82654C00) continue;
                    snprintf(cell, sizeof(cell), " %08X", entry);
                    recent += cell;
                }
                logf("[watch] %-24s fn %08X lr %08X |%s", thread->name.c_str(), ctx.lastFunction, uint32_t(ctx.lr),
                     recent.c_str());
                logf("[watch]   backtrace:%s", guestBacktrace(ctx, 24).c_str());
            }
        }
    }).detach();
}
}  // namespace

namespace {
std::atomic<int> watchedWrites{0};

LONG CALLBACK onWatchedWrite(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    info->ContextRecord->Dr6 = 0;
    if (watchedWrites++ < 40)
        if (auto* thread = currentThread())
            logf("[watchw] %s fn %08X lr %08X value %08X |%s", thread->name.c_str(), thread->context.lastFunction,
                 uint32_t(thread->context.lr),
                 _byteswap_ulong(*reinterpret_cast<const uint32_t*>(info->ContextRecord->Dr0)),
                 guestBacktrace(thread->context, 8).c_str());
    return EXCEPTION_CONTINUE_EXECUTION;
}
}  // namespace

namespace {
constexpr uint32_t kCountedCode = 0x00A00000;  // 0x82000000-0x82A00000
uint32_t* entryCounts = nullptr;
}  // namespace

// The module's counter pointer: null in modules built without TENCHU_ENTRY_COUNTS.
uint32_t* volatile* moduleCounts() {
    const GameModule* module = gameModule();
    return module ? module->entryCounts : nullptr;
}

void startEntryCounts() {
    if (!moduleCounts()) {
        logf("[count] not available: configure with -DTENCHU_ENTRY_COUNTS=ON");
        return;
    }
    if (!entryCounts) entryCounts = new uint32_t[kCountedCode / 4];
    std::memset(entryCounts, 0, kCountedCode);
    *moduleCounts() = entryCounts;
}

void stopEntryCounts(const std::string& path) {
    if (!moduleCounts()) return;
    *moduleCounts() = nullptr;
    if (!entryCounts) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));  // let in-flight increments land
    FILE* file = fopen(path.c_str(), "w");
    if (!file) return;
    size_t functions = 0;
    for (uint32_t i = 0; i < kCountedCode / 4; ++i)
        if (entryCounts[i]) {
            fprintf(file, "%08X %u\n", 0x82000000u + i * 4, entryCounts[i]);
            ++functions;
        }
    fclose(file);
    logf("[count] %zu functions written to %s", functions, path.c_str());
}

void watchGuestWrites(uint32_t address) {
    static bool installed = (AddVectoredExceptionHandler(1, onWatchedWrite), true);
    (void)installed;
    // Debug registers are per thread and a thread cannot reliably change its own:
    // a helper suspends each guest thread in turn.
    const uint64_t host = uint64_t(memory->base() + (address & ~3u));
    std::thread([host] {
        std::vector<std::shared_ptr<GuestThread>> threads;
        {
            std::lock_guard lock(threadMutex);
            for (const auto& [kthread, weak] : threadsByKthread)
                if (auto thread = weak.lock()) threads.push_back(thread);
        }
        for (const auto& thread : threads) {
            if (!thread->host || SuspendThread(thread->host) == DWORD(-1)) continue;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread->host, &context)) {
                context.Dr0 = host;
                context.Dr7 = (context.Dr7 & ~0xF0003ull) | 1 | (1ull << 16) | (3ull << 18);  // L0, write, 4 bytes
                SetThreadContext(thread->host, &context);
            }
            ResumeThread(thread->host);
        }
        logf("[watchw] armed on %zu threads", threads.size());
    }).detach();
}

namespace {
// Samples each thread's current guest function every millisecond for ten
// seconds after a delay, then prints the hottest functions (TENCHU_TRACE=prof).
void startProfiler() {
    if (!traceEnabled("prof")) return;
    // TENCHU_PROF=<start s>,<duration s> moves the window (default 8,10).
    int startSeconds = 8, durationSeconds = 10;
    char window[32]{};
    if (GetEnvironmentVariableA("TENCHU_PROF", window, sizeof(window))) sscanf_s(window, "%d,%d", &startSeconds, &durationSeconds);
    std::thread([startSeconds, durationSeconds] {
        std::this_thread::sleep_for(std::chrono::seconds(startSeconds));
        std::map<std::string, std::map<uint32_t, uint32_t>> samples;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(durationSeconds);
        while (std::chrono::steady_clock::now() < end) {
            {
                std::lock_guard lock(threadMutex);
                for (const auto& [kthread, weak] : threadsByKthread)
                    if (auto thread = weak.lock()) {
                        // Attribute to the latest entry that is not a register save/restore helper.
                        const auto& ctx = thread->context;
                        uint32_t function = 0;
                        for (uint32_t back = 1; back <= 16 && back <= ctx.traceIndex; ++back) {
                            const uint32_t entry = ctx.trace[(ctx.traceIndex - back) & 63];
                            if (entry < 0x82653F00 || entry >= 0x82654C00) { function = entry; break; }
                        }
                        ++samples[thread->name + "#" + std::to_string(thread->id)][function];
                    }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (const auto& [name, histogram] : samples) {
            std::vector<std::pair<uint32_t, uint32_t>> sorted;
            uint32_t total = 0;
            for (const auto& [function, count] : histogram) { sorted.emplace_back(count, function); total += count; }
            std::sort(sorted.rbegin(), sorted.rend());
            std::string line;
            char cell[32];
            for (size_t i = 0; i < sorted.size() && i < 12; ++i) {
                snprintf(cell, sizeof(cell), " %08X:%u%%", sorted[i].second, sorted[i].first * 100 / total);
                line += cell;
            }
            logf("[prof] %-24s%s", name.c_str(), line.c_str());
        }
    }).detach();
}
}  // namespace

void runTitle() {
    startWatchdog();
    startProfiler();
    auto thread = attachThread(defaultStackSize, "main");
    auto object = std::make_shared<ThreadObject>();
    object->thread = thread;
    object->dispatcher = thread->kthread;
    object->entry = entryPoint;
    thread->object = object;
    createHandle(object);
    try {
        callGuest(entryPoint);
        logf("[kernel] title entry point returned");
    } catch (const ThreadExit& exit) {
        logf("[kernel] main thread exited with %u", exit.code);
    }
}

}  // namespace tz

[[noreturn]] void PPCRecompFailure(const PPCContext& ctx, uint32_t address, const char* reason) {
    (void)ctx;
    tz::fatal("guest=0x%08X: %s", address, reason);
}

uint64_t PPCQueryTimebase() {
    // The Xenon timebase runs at 49.875 MHz.
    static const uint64_t frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const uint64_t ticks = uint64_t(now.QuadPart);
    return ticks / frequency * 49875000ull + ticks % frequency * 49875000ull / frequency;
}
