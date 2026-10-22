// Networking imports. The port is offline: startup succeeds, the Ethernet
// link reports down and every socket operation fails with WSAENETDOWN.
#include "kernel.h"
#include "ppc_recomp_shared.h"

using namespace tz;

namespace {
constexpr uint32_t kSocketError = 0xFFFFFFFF;
constexpr uint32_t kNetworkDown = 10050;  // WSAENETDOWN
thread_local uint32_t lastError = 0;

void failSocket(PPCContext& ctx) {
    lastError = kNetworkDown;
    ctx.r3.u64 = kSocketError;
}
}  // namespace

// The first argument of every NetDll export is the calling stack (title = 1).
PPC_FUNC(__imp__NetDll_WSAStartup) {
    // (caller, version, WSADATA*)
    if (ctx.r5.u32) std::memset(memory->host(ctx.r5.u32), 0, 0x190);
    if (ctx.r5.u32) store16(ctx.r5.u32, 0x0202);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NetDll_WSACleanup) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__NetDll_WSAGetLastError) { ctx.r3.u64 = lastError; }
PPC_FUNC(__imp__NetDll_XNetStartup) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__NetDll_XNetCleanup) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus) { ctx.r3.u64 = 0; }  // no link

PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr) {
    // (caller, XNADDR*) -> XNET_GET_XNADDR_NONE
    if (ctx.r4.u32) std::memset(memory->host(ctx.r4.u32), 0, 36);
    ctx.r3.u64 = 1;
}

PPC_FUNC(__imp__NetDll_XNetRandom) {
    // (caller, buffer, length)
    for (uint32_t i = 0; i < ctx.r5.u32; ++i) *memory->host(ctx.r4.u32 + i) = uint8_t(rand());
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_XNetCreateKey) { ctx.r3.u64 = kNetworkDown; }
PPC_FUNC(__imp__NetDll_XNetRegisterKey) { ctx.r3.u64 = kNetworkDown; }
PPC_FUNC(__imp__NetDll_XNetUnregisterKey) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__NetDll_XNetXnAddrToInAddr) { ctx.r3.u64 = kNetworkDown; }
PPC_FUNC(__imp__NetDll_XNetInAddrToXnAddr) { ctx.r3.u64 = kNetworkDown; }
PPC_FUNC(__imp__NetDll_XNetQosListen) { ctx.r3.u64 = kNetworkDown; }
PPC_FUNC(__imp__NetDll_XNetQosLookup) { ctx.r3.u64 = kNetworkDown; }
PPC_FUNC(__imp__NetDll_XNetQosRelease) { ctx.r3.u64 = 0; }

PPC_FUNC(__imp__NetDll_WSACreateEvent) {
    auto event = std::make_shared<KernelObject>(KernelObject::Kind::Event);
    event->dispatcher = createDispatcherObject(kNotificationEvent, 16, 0);
    ctx.r3.u64 = createHandle(event);
}
PPC_FUNC(__imp__NetDll_WSACloseEvent) { ctx.r3.u64 = closeHandle(ctx.r3.u32); }
PPC_FUNC(__imp__NetDll_WSAResetEvent) {
    auto event = lookupHandle(ctx.r3.u32);
    if (event && event->dispatcher) resetEvent(event->dispatcher);
    ctx.r3.u64 = event != nullptr;
}
PPC_FUNC(__imp__NetDll_WSAEventSelect) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_WSAWaitForMultipleEvents) { ctx.r3.u64 = 0x102; }  // WSA_WAIT_TIMEOUT

PPC_FUNC(__imp__NetDll_socket) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_closesocket) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__NetDll_shutdown) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_setsockopt) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_getsockopt) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_ioctlsocket) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_bind) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_connect) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_listen) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_accept) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_select) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_recv) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_recvfrom) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_send) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_sendto) { failSocket(ctx); }
PPC_FUNC(__imp__NetDll_getsockname) { failSocket(ctx); }
