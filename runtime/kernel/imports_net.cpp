// Networking imports. The port is offline: startup succeeds, the Ethernet
// link reports down and every socket operation fails with WSAENETDOWN.
#include "kernel.h"
#include "guest/host.h"

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
TZ_IMPORT(NetDll_WSAStartup) {
    // (caller, version, WSADATA*)
    if (ctx.r5.u32) std::memset(memory->host(ctx.r5.u32), 0, 0x190);
    if (ctx.r5.u32) store16(ctx.r5.u32, 0x0202);
    ctx.r3.u64 = 0;
}
TZ_IMPORT(NetDll_WSACleanup) { ctx.r3.u64 = 0; }
TZ_IMPORT(NetDll_WSAGetLastError) { ctx.r3.u64 = lastError; }
TZ_IMPORT(NetDll_XNetStartup) { ctx.r3.u64 = 0; }
TZ_IMPORT(NetDll_XNetCleanup) { ctx.r3.u64 = 0; }
TZ_IMPORT(NetDll_XNetGetEthernetLinkStatus) { ctx.r3.u64 = 0; }  // no link

TZ_IMPORT(NetDll_XNetGetTitleXnAddr) {
    // (caller, XNADDR*) -> XNET_GET_XNADDR_NONE
    if (ctx.r4.u32) std::memset(memory->host(ctx.r4.u32), 0, 36);
    ctx.r3.u64 = 1;
}

TZ_IMPORT(NetDll_XNetRandom) {
    // (caller, buffer, length)
    for (uint32_t i = 0; i < ctx.r5.u32; ++i) *memory->host(ctx.r4.u32 + i) = uint8_t(rand());
    ctx.r3.u64 = 0;
}

TZ_IMPORT(NetDll_XNetCreateKey) { ctx.r3.u64 = kNetworkDown; }
TZ_IMPORT(NetDll_XNetRegisterKey) { ctx.r3.u64 = kNetworkDown; }
TZ_IMPORT(NetDll_XNetUnregisterKey) { ctx.r3.u64 = 0; }
TZ_IMPORT(NetDll_XNetXnAddrToInAddr) { ctx.r3.u64 = kNetworkDown; }
TZ_IMPORT(NetDll_XNetInAddrToXnAddr) { ctx.r3.u64 = kNetworkDown; }
TZ_IMPORT(NetDll_XNetQosListen) { ctx.r3.u64 = kNetworkDown; }
TZ_IMPORT(NetDll_XNetQosLookup) { ctx.r3.u64 = kNetworkDown; }
TZ_IMPORT(NetDll_XNetQosRelease) { ctx.r3.u64 = 0; }

TZ_IMPORT(NetDll_WSACreateEvent) {
    auto event = std::make_shared<KernelObject>(KernelObject::Kind::Event);
    event->dispatcher = createDispatcherObject(kNotificationEvent, 16, 0);
    ctx.r3.u64 = createHandle(event);
}
TZ_IMPORT(NetDll_WSACloseEvent) { ctx.r3.u64 = closeHandle(ctx.r3.u32); }
TZ_IMPORT(NetDll_WSAResetEvent) {
    auto event = lookupHandle(ctx.r3.u32);
    if (event && event->dispatcher) resetEvent(event->dispatcher);
    ctx.r3.u64 = event != nullptr;
}
TZ_IMPORT(NetDll_WSAEventSelect) { failSocket(ctx); }
TZ_IMPORT(NetDll_WSAWaitForMultipleEvents) { ctx.r3.u64 = 0x102; }  // WSA_WAIT_TIMEOUT

TZ_IMPORT(NetDll_socket) { failSocket(ctx); }
TZ_IMPORT(NetDll_closesocket) { ctx.r3.u64 = 0; }
TZ_IMPORT(NetDll_shutdown) { failSocket(ctx); }
TZ_IMPORT(NetDll_setsockopt) { failSocket(ctx); }
TZ_IMPORT(NetDll_getsockopt) { failSocket(ctx); }
TZ_IMPORT(NetDll_ioctlsocket) { failSocket(ctx); }
TZ_IMPORT(NetDll_bind) { failSocket(ctx); }
TZ_IMPORT(NetDll_connect) { failSocket(ctx); }
TZ_IMPORT(NetDll_listen) { failSocket(ctx); }
TZ_IMPORT(NetDll_accept) { failSocket(ctx); }
TZ_IMPORT(NetDll_select) { failSocket(ctx); }
TZ_IMPORT(NetDll_recv) { failSocket(ctx); }
TZ_IMPORT(NetDll_recvfrom) { failSocket(ctx); }
TZ_IMPORT(NetDll_send) { failSocket(ctx); }
TZ_IMPORT(NetDll_sendto) { failSocket(ctx); }
TZ_IMPORT(NetDll_getsockname) { failSocket(ctx); }
