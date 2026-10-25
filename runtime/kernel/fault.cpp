#include "fault.h"

#include <atomic>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <ucontext.h>
#endif

namespace tz::fault {
namespace {

std::atomic<Handler> g_handlers[8];
std::atomic<unsigned> g_count{0};

bool dispatch(Info& info) {
    const unsigned count = g_count.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i)
        if (Handler handler = g_handlers[i].load(std::memory_order_acquire); handler && handler(info)) return true;
    return false;
}

#ifdef _WIN32
LONG CALLBACK onException(EXCEPTION_POINTERS* exception) {
    const auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& context = *exception->ContextRecord;
    Info info{record->ExceptionInformation[1], record->ExceptionInformation[0] == 1,
              {&context.Rax, &context.Rcx, &context.Rdx, &context.Rbx, &context.Rsp, &context.Rbp, &context.Rsi,
               &context.Rdi, &context.R8, &context.R9, &context.R10, &context.R11, &context.R12, &context.R13,
               &context.R14, &context.R15},
              &context.Rip};
    return dispatch(info) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

void install() { AddVectoredExceptionHandler(1, onException); }
#else
struct sigaction g_previous[2];

void onSignal(int signal, siginfo_t* signalInfo, void* data) {
    auto* context = static_cast<ucontext_t*>(data);
    auto* gregs = reinterpret_cast<uint64_t*>(context->uc_mcontext.gregs);
    Info info{reinterpret_cast<uintptr_t>(signalInfo->si_addr), (gregs[REG_ERR] & 2) != 0,
              {&gregs[REG_RAX], &gregs[REG_RCX], &gregs[REG_RDX], &gregs[REG_RBX], &gregs[REG_RSP], &gregs[REG_RBP],
               &gregs[REG_RSI], &gregs[REG_RDI], &gregs[REG_R8], &gregs[REG_R9], &gregs[REG_R10], &gregs[REG_R11],
               &gregs[REG_R12], &gregs[REG_R13], &gregs[REG_R14], &gregs[REG_R15]},
              &gregs[REG_RIP]};
    if (dispatch(info)) return;
    // Not ours: the previous handler, or the default action (the fault repeats and kills us).
    const struct sigaction& previous = g_previous[signal == SIGBUS];
    if ((previous.sa_flags & SA_SIGINFO) && previous.sa_sigaction) {
        previous.sa_sigaction(signal, signalInfo, data);
    } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
        previous.sa_handler(signal);
    } else {
        std::signal(signal, SIG_DFL);
    }
}

void install() {
    struct sigaction action {};
    action.sa_sigaction = onSignal;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &g_previous[0]);
    sigaction(SIGBUS, &action, &g_previous[1]);
}
#endif

}  // namespace

void addHandler(Handler handler) {
    static std::once_flag once;
    static std::mutex mutex;
    std::call_once(once, install);
    std::lock_guard lock(mutex);
    const unsigned index = g_count.load();
    if (index >= 8) return;
    g_handlers[index].store(handler, std::memory_order_release);
    g_count.store(index + 1, std::memory_order_release);
}

}  // namespace tz::fault
