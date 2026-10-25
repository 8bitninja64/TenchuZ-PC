#pragma once
// Access faults in guest memory (write-watched pages, MMIO), on both platforms: a Windows
// vectored exception handler or a Linux SIGSEGV/SIGBUS handler builds an Info and asks the
// registered handlers in order; the first that returns true resumes the thread (registers and
// rip as it left them). Unhandled faults go on to the next handler (the crash reporter).
#include <cstdint>

namespace tz::fault {

struct Info {
    uintptr_t address;   // faulting host address
    bool write;          // the access was a write
    uint64_t* regs[16];  // rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8-r15 (x86 encoding order)
    uint64_t* rip;
};
using Handler = bool (*)(Info& info);

// Handlers live for the whole process; at most 8.
void addHandler(Handler handler);

}  // namespace tz::fault
