#pragma once
// Xbox 360 kernel emulation shared by the import implementations.
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "ppc_context.h"
#include "memory.h"

namespace tz {

// NTSTATUS values used by the imports.
enum : uint32_t {
    kStatusSuccess = 0x00000000,
    kStatusAbandoned = 0x00000080,
    kStatusUserApc = 0x000000C0,
    kStatusAlerted = 0x00000101,
    kStatusTimeout = 0x00000102,
    kStatusPending = 0x00000103,
    kStatusNoMoreFiles = 0x80000006,
    kStatusUnsuccessful = 0xC0000001,
    kStatusNotImplemented = 0xC0000002,
    kStatusInvalidHandle = 0xC0000008,
    kStatusInvalidParameter = 0xC000000D,
    kStatusNoSuchFile = 0xC000000F,
    kStatusEndOfFile = 0xC0000011,
    kStatusNoMemory = 0xC0000017,
    kStatusAccessDenied = 0xC0000022,
    kStatusBufferTooSmall = 0xC0000023,
    kStatusObjectTypeMismatch = 0xC0000024,
    kStatusObjectNameInvalid = 0xC0000033,
    kStatusObjectNameNotFound = 0xC0000034,
    kStatusObjectNameCollision = 0xC0000035,
    kStatusObjectPathNotFound = 0xC000003A,
    kStatusSemaphoreLimitExceeded = 0xC0000047,
    kStatusMutantNotOwned = 0xC0000046,
    kStatusNotADirectory = 0xC0000103,
    kStatusFileIsADirectory = 0xC00000BA,
    kStatusNotSupported = 0xC00000BB,
};

// Win32 error codes returned by XAM.
enum : uint32_t {
    kErrorSuccess = 0,
    kErrorFileNotFound = 2,
    kErrorPathNotFound = 3,
    kErrorAccessDenied = 5,
    kErrorInvalidParameter = 87,
    kErrorInsufficientBuffer = 122,
    kErrorAlreadyExists = 183,
    kErrorNoMoreFiles = 18,
    kErrorNotFound = 1168,
    kErrorIoPending = 997,
    kErrorCancelled = 1223,
    kErrorFunctionFailed = 1627,
    kErrorNoSuchUser = 1317,
    kErrorDeviceNotConnected = 1167,
};

// Dispatcher header object types.
enum : uint8_t {
    kNotificationEvent = 0,
    kSynchronizationEvent = 1,
    kMutant = 2,
    kSemaphore = 5,
    kThread = 6,
    kNotificationTimer = 8,
    kSynchronizationTimer = 9,
};

uint32_t load32(uint32_t address);
// Guest MMIO and null reads through faults (kernel/mmio_fault.cpp).
void installMmioFaultHandler();
void store32(uint32_t address, uint32_t value);
uint16_t load16(uint32_t address);
void store16(uint32_t address, uint16_t value);
uint64_t load64(uint32_t address);
void store64(uint32_t address, uint64_t value);
std::string loadString(uint32_t address, size_t maxLength = 4096);
std::u16string loadWideString(uint32_t address, size_t maxLength = 4096);
// ANSI_STRING { u16 length; u16 maximum; u32 buffer; }
std::string loadAnsiString(uint32_t ansiString);

// Thread-local guest state for the current host thread.
struct GuestThread;
GuestThread* currentThread();
PPCContext& currentContext();

// Calls a guest function on the current thread with up to eight integer
// arguments. Volatile guest registers are saved and restored around the call.
uint32_t callGuest(uint32_t function, std::initializer_list<uint64_t> arguments = {});

// Kernel objects reachable through handles.
struct KernelObject {
    enum class Kind { Event, Semaphore, Mutant, Timer, Thread, File, NotifyListener, Enumerator, Other };
    explicit KernelObject(Kind kind) : kind(kind) {}
    virtual ~KernelObject() = default;
    Kind kind;
    // Guest dispatcher header for waitable objects, 0 otherwise.
    uint32_t dispatcher = 0;
};

uint32_t createHandle(std::shared_ptr<KernelObject> object);
std::shared_ptr<KernelObject> lookupHandle(uint32_t handle);
bool closeHandle(uint32_t handle);
uint32_t duplicateHandle(uint32_t handle);
template <typename T> std::shared_ptr<T> lookupHandleAs(uint32_t handle) {
    return std::dynamic_pointer_cast<T>(lookupHandle(handle));
}

// Allocates a waitable guest dispatcher object in kernel memory.
uint32_t createDispatcherObject(uint8_t type, uint32_t size, int32_t signalState);

// Dispatcher operations. All signal-state changes made by the kernel go
// through these so waiting threads are woken.
int32_t setEvent(uint32_t event);
int32_t resetEvent(uint32_t event);
int32_t pulseEvent(uint32_t event);
uint32_t releaseSemaphore(uint32_t semaphore, int32_t adjustment, int32_t& previous);
uint32_t releaseMutant(uint32_t mutant, int32_t& previous);
void signalObject(uint32_t dispatcher, int32_t state);
void wakeWaiters();

// timeout: nullptr = infinite, otherwise NT 100ns units (negative relative).
uint32_t waitForObjects(const std::vector<uint32_t>& dispatchers, bool waitAll, bool alertable,
                        const int64_t* timeout);
uint32_t delayExecution(bool alertable, int64_t interval);

// Asynchronous procedure calls run on the target thread during alertable waits.
struct Apc {
    uint32_t routine = 0;
    uint64_t arguments[3] = {};
};
void queueApc(GuestThread* thread, const Apc& apc);

// Guest threads.
struct ThreadObject;
struct GuestThread {
    PPCContext context{};
    uint32_t id = 0;
    uint32_t pcr = 0;
    uint32_t kthread = 0;
    uint32_t stackBase = 0;  // lowest address
    uint32_t stackSize = 0;
    uint32_t tls = 0;        // static TLS block, followed by 64 dynamic slots
    HANDLE host = nullptr;
    std::weak_ptr<ThreadObject> object;
    std::mutex apcMutex;
    std::deque<Apc> apcs;
    std::string name;
};
struct ThreadObject : KernelObject {
    ThreadObject() : KernelObject(Kind::Thread) {}
    std::shared_ptr<GuestThread> thread;
    uint32_t entry = 0, argument = 0, startup = 0;
    std::atomic<uint32_t> exitCode{0};
};

// Thrown to unwind a guest thread from ExTerminateThread.
struct ThreadExit { uint32_t code; };

// Creates the guest thread state for the calling host thread.
std::shared_ptr<GuestThread> attachThread(uint32_t stackSize, const char* name);
// Creates and optionally starts a new guest thread. Returns its handle.
uint32_t createThread(uint32_t stackSize, uint32_t entry, uint32_t argument, uint32_t startup,
                      bool suspended, uint32_t& threadId, uint32_t* kthreadOut = nullptr, int processor = -1);
// Runs a guest function on a new host thread that is not visible to the
// guest as an XThread (interrupt and audio callbacks).
std::shared_ptr<GuestThread> createSystemThread(const char* name);
// Samples the calling thread's host instruction pointer (TENCHU_TRACE=hostprof).
void startHostProfiler(const char* name);
std::shared_ptr<GuestThread> threadByKthread(uint32_t kthread);

// Kernel bring-up: loads the executable, data imports and the function table.
void initializeKernel(const std::filesystem::path& gameDirectory);
const std::filesystem::path& gameDirectory();
const std::filesystem::path& userDirectory();
uint32_t xexHeaderAddress();
uint32_t xexEntryPoint();
uint32_t staticTlsSize();
void runTitle();

// Diagnostics.
bool traceEnabled(const char* category);
void logf(const char* format, ...);
// Categories are literals: each site looks its category up once.
#define TZ_TRACE(category, ...) \
    do { static const bool tzTraceOn = ::tz::traceEnabled(category); if (tzTraceOn) ::tz::logf(__VA_ARGS__); } while (0)
[[noreturn]] void fatal(const char* format, ...);
// Return addresses of the current guest call chain, walking frame back chains
// (each frame stores its caller's LR at back chain - 8).
std::string guestBacktrace(const PPCContext& ctx, int depth = 16);

// Guest printf (sprintf, _vsnprintf, DbgPrint).
struct GuestArgs {
    // Reads 64-bit argument slots: r(first)..r10, then the caller's stack.
    static GuestArgs fromRegisters(const PPCContext& ctx, int firstRegister);
    // Reads 64-bit slots from a guest va_list pointer.
    static GuestArgs fromVaList(uint32_t vaList);
    uint64_t next();
    const PPCContext* ctx = nullptr;
    int index = 0;
    uint32_t vaList = 0;
};
std::string formatGuest(const std::string& format, GuestArgs& args);

}  // namespace tz
