#include "gpu/gpu_progress.h"

#include <atomic>

#ifdef _WIN32
#include <windows.h>
#else
#include <climits>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace tz::gpu {
namespace {

std::atomic<uint32_t> g_progress{0};
std::atomic<int> g_waiters{0};  // waking costs a system call: only with someone asleep

}  // namespace

uint32_t progress() { return g_progress.load(std::memory_order_acquire); }

void noteProgress() {
    g_progress.fetch_add(1, std::memory_order_acq_rel);
    if (g_waiters.load(std::memory_order_acquire) == 0) return;
#ifdef _WIN32
    WakeByAddressAll(&g_progress);
#else
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&g_progress), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
#endif
}

void waitProgress(uint32_t seen, uint32_t microseconds) {
    g_waiters.fetch_add(1, std::memory_order_acq_rel);
    // A change between reading `seen` and sleeping makes the wait return at once.
#ifdef _WIN32
    WaitOnAddress(&g_progress, &seen, sizeof(seen), (microseconds + 999) / 1000);
#else
    const timespec timeout{0, long(microseconds) * 1000};
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&g_progress), FUTEX_WAIT_PRIVATE, seen, &timeout, nullptr, 0);
#endif
    g_waiters.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace tz::gpu
