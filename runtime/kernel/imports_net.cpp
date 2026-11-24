// Opt-in System Link prototype. Guest integers are big endian. Socket addresses
// contain a guest-order family followed by network-order port/address bytes.
// Protocol references (implementation here targets our own kernel):
// github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xam/apps/xgi_app.cc
// github.com/Llamapixels/TenchuZRecomp/blob/main/docs/multiplayer.md
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#endif
#include "kernel.h"
#include "network.h"
#include "guest/host.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <random>
#include <thread>
#include <map>

using namespace tz;
namespace {
#ifdef _WIN32
using Socket = SOCKET;
using Length = int;
constexpr Socket invalid = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
int socketError() { return WSAGetLastError(); }
#else
using Socket = int;
using Length = socklen_t;
constexpr Socket invalid = -1;
void closeSocket(Socket s) { close(s); }
int translateSocketError(int error) {
    switch (error) {
    case 0: return 0;
    case EAGAIN: return 10035; case EINPROGRESS: return 10036;
    case EALREADY: return 10037; case ENOTSOCK: return 10038;
    case EMSGSIZE: return 10040; case EOPNOTSUPP: return 10045;
    case EAFNOSUPPORT: return 10047; case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049; case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051; case ECONNRESET: return 10054;
    case ENOBUFS: return 10055; case EISCONN: return 10056;
    case ENOTCONN: return 10057; case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061; case EHOSTUNREACH: return 10065;
    case EINTR: return 10004; case EFAULT: return 10014;
    default: return 10022;
    }
}
int socketError() { return translateSocketError(errno); }
#endif
thread_local uint32_t lastError = 0;
void fail(PPCContext& ctx, uint32_t error = 10050) {
    if (error != 10035 && net::enabled()) TZ_TRACE("net", "[net] socket failure %u lr=%08X", error, uint32_t(ctx.lr));
    lastError = error; ctx.r3.u64 = 0xFFFFFFFF;
}
void result(PPCContext& ctx, int value) {
    if (value < 0) fail(ctx, socketError()); else ctx.r3.u64 = uint32_t(value);
}
uint32_t envAddress(const char* name) {
    in_addr ip{}; const char* value = std::getenv(name);
    return value && inet_pton(AF_INET, value, &ip) == 1 ? ip.s_addr : 0;
}
bool startup() {
    static const bool ok = [] {
#ifdef _WIN32
        WSADATA data{}; return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
        return true;
#endif
    }(); return ok;
}
struct SocketObject : KernelObject {
    explicit SocketObject(Socket s, bool active = true) : KernelObject(Kind::Other), native(s), pollReady(active) {}
    ~SocketObject() override { if (native != invalid) closeSocket(native); }
    Socket native;
    std::atomic<bool> pollReady;
    std::mutex mutex;
    std::shared_ptr<KernelObject> event;
    uint32_t mask = 0;
    bool writeNotified = false;
    bool deferredTitleBind = false;
    sockaddr_in titleBind{};
    std::map<uint32_t, std::chrono::steady_clock::time_point> peers;
};
struct NetworkState {
    std::mutex mutex;
    std::vector<std::weak_ptr<SocketObject>> sockets;
    // Last member: joined before destroying the watch list and mutex.
    std::jthread worker;
    void watch(const std::shared_ptr<SocketObject>& object) {
        std::lock_guard lock(mutex); sockets.push_back(object);
        if (worker.joinable()) return;
        worker = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                {
                    std::lock_guard listLock(mutex);
                    for (auto it = sockets.begin(); it != sockets.end();) {
                        auto s = it->lock();
                        if (!s) { it = sockets.erase(it); continue; }
                        ++it; std::lock_guard socketLock(s->mutex);
                        if (!s->event || !s->mask || s->native == invalid || !s->pollReady.load()) continue;
#ifdef _WIN32
                        WSAPOLLFD p{s->native, 0, 0};
#else
                        pollfd p{s->native, 0, 0};
#endif
                        if (s->mask & (1 | 8 | 32)) p.events |= POLLIN;
                        if (!s->writeNotified && (s->mask & (2 | 16))) p.events |= POLLOUT;
#ifdef _WIN32
                        int ready = WSAPoll(&p, 1, 0);
#else
                        int ready = poll(&p, 1, 0);
#endif
                        if (ready > 0 && p.revents) {
                            if (p.revents & POLLOUT) s->writeNotified = true;
                            setEvent(s->event->dispatcher);
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }
};
NetworkState& state() { static NetworkState value; return value; }
std::shared_ptr<SocketObject> socketFor(PPCContext& ctx) {
    if (!net::enabled()) { fail(ctx); return {}; }
    auto s = lookupHandleAs<SocketObject>(ctx.r4.u32);
    if (!s || s->native == invalid) { fail(ctx, 10038); return {}; }
    return s;
}
bool readAddress(uint32_t ptr, uint32_t length, sockaddr_in& addr) {
    if (!ptr || length < 16 || load16(ptr) != AF_INET) return false;
    addr = {}; addr.sin_family = AF_INET;
    std::memcpy(&addr.sin_port, memory->host(ptr + 2), 2);
    // Desktop peers use unprivileged ports, so Linux never needs root/caps.
    const uint16_t port = ntohs(addr.sin_port);
    if (port >= 1000 && port <= 1004) addr.sin_port = htons(port + 10000);
    std::memcpy(&addr.sin_addr, memory->host(ptr + 4), 4); return true;
}
bool addressCapacity(uint32_t ptr, uint32_t lengthPtr) {
    return !ptr || (lengthPtr && load32(lengthPtr) >= 16);
}
void writeAddress(uint32_t ptr, uint32_t lengthPtr, const sockaddr_in& addr) {
    if (!ptr) return;
    std::memset(memory->host(ptr), 0, 16); store16(ptr, AF_INET);
    const uint16_t port = ntohs(addr.sin_port);
    store16(ptr + 2, port >= 11000 && port <= 11004 ? port - 10000 : port);
    std::memcpy(memory->host(ptr + 4), &addr.sin_addr, 4); store32(lengthPtr, 16);
}
bool bufferValid(uint32_t ptr, uint32_t length) {
    return length <= 16 * 1024 * 1024 && (!length || (ptr && uint64_t(ptr) + length <= 0x100000000ull));
}
int nonblocking(Socket s, uint32_t enabled) {
#ifdef _WIN32
    u_long value = enabled; return ioctlsocket(s, FIONBIO, &value);
#else
    int value = enabled != 0; return ioctl(s, FIONBIO, &value);
#endif
}
int bindTitleSocket(SocketObject& s, bool outgoing) {
    if (!s.deferredTitleBind) return 0;
    sockaddr_in address = s.titleBind;
    // The title binds both ends to 1003. Reusing that TCP four-tuple after a
    // disconnect hits TIME_WAIT on desktop stacks. Peer identity is the IP;
    // only listeners need the title's fixed port, clients can use a fresh one.
    if (outgoing) address.sin_port = 0;
    int rc = bind(s.native, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (!rc) s.deferredTitleBind = false;
    return rc;
}
void writeXnAddress(uint32_t ptr, uint32_t ip) {
    std::memset(memory->host(ptr), 0, 36);
    store32(ptr, ip);
    // The title matches peers by abEnet (sub_8236B180), not by ina alone.
    // Reverse lookups must reconstruct the same ID that a peer advertises.
    store16(ptr + 10, 0x0200);
    store32(ptr + 12, ip);
}
} // namespace

namespace tz::net {
void beginDisconnect(uint32_t handle) {
    if (!enabled()) return;
    auto s = lookupHandleAs<SocketObject>(handle);
    if (!s) return;
    int type = 0; Length length = sizeof(type);
    if (!getsockopt(s->native, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &length) && type == SOCK_STREAM) {
        int rc = shutdown(s->native, 1); // SD_SEND / SHUT_WR; allow the receive drain.
        TZ_TRACE("net", "[net] disconnect handle=%X send shutdown=%d", handle, rc);
    }
}
bool enabled() {
    static const bool value = [] {
        const char* opt = std::getenv("TENCHU_SYSTEM_LINK");
        return opt && std::strcmp(opt, "1") == 0 && startup() && envAddress("TENCHU_NET_BIND");
    }(); return value;
}
uint64_t playerId() {
    // Explicit local address distinguishes test copies. Save paths stay fixed.
    return enabled() ? 0xE000000000000000ull | uint64_t(ntohl(envAddress("TENCHU_NET_BIND"))) : 0xE000000054454E43ull;
}
const char* playerName() {
    const char* name = std::getenv("TENCHU_NET_NAME");
    return enabled() && name && *name ? name : "Player";
}
void randomBytes(uint32_t ptr, uint32_t count) {
    static std::mutex mutex; static std::random_device random;
    std::lock_guard lock(mutex);
    for (uint32_t i = 0; i < count; ++i) *memory->host(ptr + i) = uint8_t(random());
}
void titleAddress(uint32_t ptr) {
    writeXnAddress(ptr, ntohl(envAddress("TENCHU_NET_BIND")));
}
uint32_t dispatch(uint32_t app, uint32_t message, uint32_t buffer, uint32_t size) {
    if (!enabled()) return kErrorFunctionFailed;
    TZ_TRACE("net", "[net] app=%X message=%X buffer=%X size=%u", app, message, buffer, size);
    if (app == 0xFB) switch (message) {
    case 0xB0010: // XGI create layout documented by Xenia/ReXGlue.
        if (!buffer || (size && size < 28)) return kErrorInvalidParameter;
        if ((load32(buffer + 4) & 1) && load32(buffer + 20)) {
            uint32_t info = load32(buffer + 20);
            randomBytes(info, 8); *memory->host(info) &= 0x3F;
            titleAddress(info + 8); randomBytes(info + 44, 16);
        }
        if (auto nonce = load32(buffer + 24)) randomBytes(nonce, 8);
        return 0;
    case 0xB0006: case 0xB0007: // local context/property
    case 0xB0011: case 0xB0012: // delete/join
    case 0xB0013: case 0xB0014: // start/end
    case 0xB0018: // leave
        return 0;
    default: break;
    }
    return kErrorFunctionFailed;
}
} // namespace tz::net

TZ_IMPORT(NetDll_WSAStartup) {
    if (ctx.r5.u32) { std::memset(memory->host(ctx.r5.u32), 0, 0x190); store16(ctx.r5.u32, 0x0202); }
    ctx.r3.u64 = startup() ? 0 : 10050;
}
TZ_IMPORT(NetDll_WSACleanup) { ctx.r3.u64 = 0; }
TZ_IMPORT(NetDll_WSAGetLastError) { ctx.r3.u64 = lastError; }
TZ_IMPORT(NetDll_XNetStartup) { ctx.r3.u64 = 0; }
TZ_IMPORT(NetDll_XNetCleanup) { ctx.r3.u64 = 0; }
TZ_IMPORT(NetDll_XNetGetEthernetLinkStatus) { ctx.r3.u64 = net::enabled() ? 0xB : 0; }
TZ_IMPORT(NetDll_XNetGetTitleXnAddr) {
    if (ctx.r4.u32) {
        if (net::enabled()) net::titleAddress(ctx.r4.u32);
        else std::memset(memory->host(ctx.r4.u32), 0, 36);
    }
    ctx.r3.u64 = net::enabled() ? 6 : 1; // STATIC | ETHERNET
}
TZ_IMPORT(NetDll_XNetRandom) {
    if (!bufferValid(ctx.r4.u32, ctx.r5.u32)) { ctx.r3.u64 = 10014; return; }
    net::randomBytes(ctx.r4.u32, ctx.r5.u32); ctx.r3.u64 = 0;
}
TZ_IMPORT(NetDll_XNetCreateKey) {
    if (!net::enabled()) { ctx.r3.u64 = 10050; return; }
    if (!ctx.r4.u32 || !ctx.r5.u32) { ctx.r3.u64 = 10014; return; }
    net::randomBytes(ctx.r4.u32, 8); *memory->host(ctx.r4.u32) &= 0x3F;
    net::randomBytes(ctx.r5.u32, 16); ctx.r3.u64 = 0;
}
TZ_IMPORT(NetDll_XNetRegisterKey) { ctx.r3.u64 = net::enabled() ? 0 : 10050; }
TZ_IMPORT(NetDll_XNetUnregisterKey) { ctx.r3.u64 = 0; }
TZ_IMPORT(NetDll_XNetXnAddrToInAddr) {
    if (!net::enabled()) { ctx.r3.u64 = 10050; return; }
    if (!ctx.r4.u32 || !ctx.r6.u32) { ctx.r3.u64 = 10014; return; }
    std::memcpy(memory->host(ctx.r6.u32), memory->host(ctx.r4.u32), 4); ctx.r3.u64 = 0;
}
TZ_IMPORT(NetDll_XNetInAddrToXnAddr) {
    if (!net::enabled()) { ctx.r3.u64 = 10050; return; }
    if (!ctx.r5.u32) { ctx.r3.u64 = 10014; return; }
    writeXnAddress(ctx.r5.u32, ctx.r4.u32);
    if (ctx.r6.u32) std::memset(memory->host(ctx.r6.u32), 0, 8);
    ctx.r3.u64 = 0;
}
TZ_IMPORT(NetDll_XNetQosListen) { ctx.r3.u64 = net::enabled() ? 0 : 10050; }
TZ_IMPORT(NetDll_XNetQosLookup) { ctx.r3.u64 = 10050; }
TZ_IMPORT(NetDll_XNetQosRelease) { ctx.r3.u64 = 0; }
TZ_IMPORT(NetDll_WSACreateEvent) {
    auto e = std::make_shared<KernelObject>(KernelObject::Kind::Event);
    e->dispatcher = createDispatcherObject(kNotificationEvent, 16, 0); ctx.r3.u64 = createHandle(e);
}
TZ_IMPORT(NetDll_WSACloseEvent) { ctx.r3.u64 = closeHandle(ctx.r3.u32); }
TZ_IMPORT(NetDll_WSAResetEvent) {
    auto e = lookupHandle(ctx.r3.u32);
    if (e && e->dispatcher) resetEvent(e->dispatcher);
    ctx.r3.u64 = e && e->dispatcher;
}
TZ_IMPORT(NetDll_WSAEventSelect) {
    auto s = socketFor(ctx); if (!s) return;
    auto e = lookupHandle(ctx.r5.u32);
    if (ctx.r6.u32 && (!e || !e->dispatcher)) { fail(ctx, 10006); return; }
    std::lock_guard lock(s->mutex);
    s->event = e; s->mask = ctx.r6.u32; s->writeNotified = false;
    result(ctx, nonblocking(s->native, 1));
}
TZ_IMPORT(NetDll_WSAWaitForMultipleEvents) {
    // No caller argument on this export.
    uint32_t count = ctx.r3.u32;
    if (!count || count > 64 || !ctx.r4.u32) { fail(ctx, 10022); return; }
    std::vector<uint32_t> events;
    for (uint32_t i = 0; i < count; ++i) {
        auto e = lookupHandle(load32(ctx.r4.u32 + 4 * i));
        if (!e || !e->dispatcher) { fail(ctx, 10006); return; }
        events.push_back(e->dispatcher);
    }
    int64_t timeout = -int64_t(ctx.r6.u32) * 10000;
    uint32_t status = waitForObjects(events, ctx.r5.u32 != 0, ctx.r7.u32 != 0,
                                    ctx.r6.u32 == 0xFFFFFFFF ? nullptr : &timeout);
    ctx.r3.u64 = status == kStatusTimeout ? 258 : status;
}
TZ_IMPORT(NetDll_socket) {
    if (!net::enabled()) { fail(ctx); return; }
    if (ctx.r4.u32 != AF_INET || (ctx.r5.u32 != SOCK_STREAM && ctx.r5.u32 != SOCK_DGRAM)) { fail(ctx, 10047); return; }
    int protocol = ctx.r6.u32 == 254 ? IPPROTO_UDP : ctx.r6.s32;
    Socket native = socket(AF_INET, ctx.r5.s32, protocol);
    if (native == invalid) { fail(ctx, socketError()); return; }
    auto s = std::make_shared<SocketObject>(native, ctx.r5.u32 != SOCK_STREAM); ctx.r3.u64 = createHandle(s); state().watch(s);
    TZ_TRACE("net", "[net] socket handle=%X type=%u protocol=%d", ctx.r3.u32, ctx.r5.u32, protocol);
}
TZ_IMPORT(NetDll_closesocket) {
    auto s = socketFor(ctx); if (!s) return;
    uint32_t handle = ctx.r4.u32;
    // Outstanding calls keep the native socket alive; avoid recycling its OS
    // descriptor underneath a polling/receive thread.
    { std::lock_guard lock(s->mutex); s->event.reset(); }
    ctx.r3.u64 = closeHandle(handle) ? 0 : 0xFFFFFFFF;
}
TZ_IMPORT(NetDll_shutdown) { auto s = socketFor(ctx); if (s) result(ctx, shutdown(s->native, ctx.r5.s32)); }
TZ_IMPORT(NetDll_ioctlsocket) {
    auto s = socketFor(ctx); if (!s) return;
    if (!ctx.r6.u32) { fail(ctx, 10014); return; }
    if (ctx.r5.u32 == 0x8004667E) { result(ctx, nonblocking(s->native, load32(ctx.r6.u32))); return; }
    if (ctx.r5.u32 == 0x4004667F) {
#ifdef _WIN32
        u_long count = 0; int rc = ioctlsocket(s->native, FIONREAD, &count);
#else
        int count = 0; int rc = ioctl(s->native, FIONREAD, &count);
#endif
        if (!rc) store32(ctx.r6.u32, uint32_t(count)); result(ctx, rc); return;
    }
    fail(ctx, 10045);
}
namespace {
int socketOption(uint32_t option) {
    switch (option) {
    case 4: return SO_REUSEADDR; case 8: return SO_KEEPALIVE;
    case 0x20: return SO_BROADCAST; case 0x1001: return SO_SNDBUF;
    case 0x1002: return SO_RCVBUF; case 0x1007: return SO_ERROR;
    default: return -1;
    }
}
}
TZ_IMPORT(NetDll_setsockopt) {
    auto s = socketFor(ctx); if (!s) return;
    if (ctx.r5.u32 == 0xFFFF && (ctx.r6.u32 == 0x5801 || ctx.r6.u32 == 0x5802)) { ctx.r3.u64 = 0; return; }
    int option = socketOption(ctx.r6.u32);
    if (ctx.r5.u32 != 0xFFFF || option < 0) { fail(ctx, 10042); return; }
    if (!ctx.r7.u32 || ctx.r8.u32 != 4) { fail(ctx, 10014); return; }
    int value = int(load32(ctx.r7.u32));
    result(ctx, setsockopt(s->native, SOL_SOCKET, option, reinterpret_cast<const char*>(&value), sizeof(value)));
}
TZ_IMPORT(NetDll_getsockopt) {
    auto s = socketFor(ctx); if (!s) return;
    int option = socketOption(ctx.r6.u32);
    if (ctx.r5.u32 != 0xFFFF || option < 0) { fail(ctx, 10042); return; }
    if (!ctx.r7.u32 || !ctx.r8.u32 || load32(ctx.r8.u32) < 4) { fail(ctx, 10014); return; }
    int value = 0; Length length = sizeof(value);
    int rc = getsockopt(s->native, SOL_SOCKET, option, reinterpret_cast<char*>(&value), &length);
#ifndef _WIN32
    if (!rc && option == SO_ERROR) value = translateSocketError(value);
#endif
    if (!rc) { store32(ctx.r7.u32, uint32_t(value)); store32(ctx.r8.u32, 4); } result(ctx, rc);
}
TZ_IMPORT(NetDll_bind) {
    auto s = socketFor(ctx); if (!s) return;
    sockaddr_in addr{};
    if (!readAddress(ctx.r5.u32, ctx.r6.u32, addr)) { fail(ctx, 10014); return; }
    if (!addr.sin_addr.s_addr) addr.sin_addr.s_addr = envAddress("TENCHU_NET_BIND");
    int type = 0; Length typeLength = sizeof(type);
    if (ntohs(addr.sin_port) == 11003 &&
        !getsockopt(s->native, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &typeLength) && type == SOCK_STREAM) {
        if (s->deferredTitleBind || s->pollReady.load()) { fail(ctx, 10022); return; }
        s->titleBind = addr; s->deferredTitleBind = true;
        ctx.r3.u64 = 0;
        return;
    }
    result(ctx, bind(s->native, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
    TZ_TRACE("net", "[net] bind port=%u result=%X error=%u", ntohs(addr.sin_port), ctx.r3.u32, lastError);
}
TZ_IMPORT(NetDll_connect) {
    auto s = socketFor(ctx); if (!s) return;
    sockaddr_in addr{};
    if (!readAddress(ctx.r5.u32, ctx.r6.u32, addr)) { fail(ctx, 10014); return; }
    if (bindTitleSocket(*s, true) < 0) { fail(ctx, socketError()); return; }
    int rc = connect(s->native, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const int error = rc < 0 ? socketError() : 0;
    // poll() reports HUP on an unconnected TCP socket. Winsock must not signal
    // a close before the title has even started connecting or listening.
    if (!rc || error == 10035 || error == 10036 || error == 10037) s->pollReady.store(true);
    TZ_TRACE("net", "[net] connect handle=%X ip=%08X port=%u result=%d", ctx.r4.u32, ntohl(addr.sin_addr.s_addr), ntohs(addr.sin_port), rc);
#ifndef _WIN32
    // Winsock reports WSAEWOULDBLOCK for a pending nonblocking connection;
    // POSIX uses EINPROGRESS. The title branches on the Winsock value.
    if (rc < 0 && error == 10036) { fail(ctx, 10035); return; }
#endif
    if (rc < 0) fail(ctx, error); else ctx.r3.u64 = uint32_t(rc);
}
TZ_IMPORT(NetDll_listen) {
    auto s = socketFor(ctx); if (!s) return;
    if (bindTitleSocket(*s, false) < 0) { fail(ctx, socketError()); return; }
    int rc = listen(s->native, ctx.r5.s32);
    if (!rc) s->pollReady.store(true);
    result(ctx, rc);
}
TZ_IMPORT(NetDll_accept) {
    auto s = socketFor(ctx); if (!s) return;
    if (!addressCapacity(ctx.r5.u32, ctx.r6.u32)) { fail(ctx, 10014); return; }
    sockaddr_in addr{}; Length length = sizeof(addr);
    Socket native = accept(s->native, reinterpret_cast<sockaddr*>(&addr), &length);
    if (native == invalid) { fail(ctx, socketError()); return; }
    writeAddress(ctx.r5.u32, ctx.r6.u32, addr);
    auto peer = std::make_shared<SocketObject>(native); ctx.r3.u64 = createHandle(peer); state().watch(peer);
    TZ_TRACE("net", "[net] accept handle=%X ip=%08X port=%u", ctx.r3.u32, ntohl(addr.sin_addr.s_addr), ntohs(addr.sin_port));
}
TZ_IMPORT(NetDll_getsockname) {
    auto s = socketFor(ctx); if (!s) return;
    if (!addressCapacity(ctx.r5.u32, ctx.r6.u32)) { fail(ctx, 10014); return; }
    if (s->deferredTitleBind) {
        writeAddress(ctx.r5.u32, ctx.r6.u32, s->titleBind); ctx.r3.u64 = 0; return;
    }
    sockaddr_in addr{}; Length length = sizeof(addr);
    int rc = getsockname(s->native, reinterpret_cast<sockaddr*>(&addr), &length);
    if (!rc) writeAddress(ctx.r5.u32, ctx.r6.u32, addr); result(ctx, rc);
}
TZ_IMPORT(NetDll_send) {
    auto s = socketFor(ctx); if (!s) return;
    if (!bufferValid(ctx.r5.u32, ctx.r6.u32)) { fail(ctx, 10014); return; }
    int flags = ctx.r7.s32;
#ifndef _WIN32
    flags |= MSG_NOSIGNAL;
#endif
    result(ctx, int(send(s->native, memory->host<const char>(ctx.r5.u32), ctx.r6.s32, flags)));
    if (ctx.r3.u32 == 0xFFFFFFFF && lastError == 10035) { std::lock_guard lock(s->mutex); s->writeNotified = false; }
}
TZ_IMPORT(NetDll_recv) {
    auto s = socketFor(ctx); if (!s) return;
    if (!bufferValid(ctx.r5.u32, ctx.r6.u32)) { fail(ctx, 10014); return; }
    memory->prepareHostWrite(ctx.r5.u32, ctx.r6.u32);
    result(ctx, int(recv(s->native, memory->host<char>(ctx.r5.u32), ctx.r6.s32, ctx.r7.s32)));
}
TZ_IMPORT(NetDll_sendto) {
    auto s = socketFor(ctx); if (!s) return;
    sockaddr_in addr{};
    if (!bufferValid(ctx.r5.u32, ctx.r6.u32) || !readAddress(ctx.r8.u32, ctx.r9.u32, addr)) { fail(ctx, 10014); return; }
    std::vector<uint32_t> destinations;
    if (addr.sin_addr.s_addr == INADDR_BROADCAST) {
        if (auto join = envAddress("TENCHU_NET_JOIN")) destinations.push_back(join);
        else destinations.push_back(INADDR_BROADCAST);
        std::lock_guard lock(s->mutex); auto now = std::chrono::steady_clock::now();
        for (auto it = s->peers.begin(); it != s->peers.end();) {
            if (now - it->second > std::chrono::seconds(10)) it = s->peers.erase(it);
            else { if (std::find(destinations.begin(), destinations.end(), it->first) == destinations.end()) destinations.push_back(it->first); ++it; }
        }
        // Search responses go straight back to known searchers. Sending the
        // same reply by broadcast as well produces duplicate host entries.
        if (ntohs(addr.sin_port) == 11004 && ctx.r6.u32 == 112 && destinations.size() > 1)
            destinations.erase(std::remove(destinations.begin(), destinations.end(), INADDR_BROADCAST), destinations.end());
    } else destinations.push_back(addr.sin_addr.s_addr);
    int sent = -1;
    for (auto ip : destinations) {
        addr.sin_addr.s_addr = ip;
        int rc = int(sendto(s->native, memory->host<const char>(ctx.r5.u32), ctx.r6.s32, ctx.r7.s32,
                            reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
        if (rc >= 0) sent = rc;
        if (ntohs(addr.sin_port) == 11004) TZ_TRACE("net", "[net] discovery send ip=%08X bytes=%d", ntohl(ip), rc);
        if (ntohs(addr.sin_port) == 11000 && ctx.r6.u32 >= 8)
            TZ_TRACE("netpack", "[netpack] send h=%X ip=%08X n=%d header=%08X %08X", ctx.r4.u32, ntohl(ip), rc, load32(ctx.r5.u32), load32(ctx.r5.u32+4));
    }
    result(ctx, sent);
}
TZ_IMPORT(NetDll_recvfrom) {
    auto s = socketFor(ctx); if (!s) return;
    if (!bufferValid(ctx.r5.u32, ctx.r6.u32) || !addressCapacity(ctx.r8.u32, ctx.r9.u32)) { fail(ctx, 10014); return; }
    sockaddr_in addr{}; Length length = sizeof(addr);
    memory->prepareHostWrite(ctx.r5.u32, ctx.r6.u32);
    int rc = int(recvfrom(s->native, memory->host<char>(ctx.r5.u32), ctx.r6.s32, ctx.r7.s32,
                          reinterpret_cast<sockaddr*>(&addr), &length));
    if (rc >= 0) {
        if (rc >= 8) TZ_TRACE("netpack", "[netpack] recv h=%X ip=%08X port=%u n=%d header=%08X %08X", ctx.r4.u32, ntohl(addr.sin_addr.s_addr), ntohs(addr.sin_port), rc, load32(ctx.r5.u32), load32(ctx.r5.u32+4));
        if (ntohs(addr.sin_port) == 11004) TZ_TRACE("net", "[net] discovery recv ip=%08X bytes=%d", ntohl(addr.sin_addr.s_addr), rc);
        writeAddress(ctx.r8.u32, ctx.r9.u32, addr); std::lock_guard lock(s->mutex);
        if (addr.sin_addr.s_addr != envAddress("TENCHU_NET_BIND") &&
            (s->peers.size() < 64 || s->peers.count(addr.sin_addr.s_addr)))
            s->peers[addr.sin_addr.s_addr] = std::chrono::steady_clock::now();
    }
    result(ctx, rc);
}
TZ_IMPORT(NetDll_select) {
    if (!net::enabled()) { fail(ctx); return; }
    std::array<fd_set, 3> sets;
    std::array<std::vector<std::pair<uint32_t, std::shared_ptr<SocketObject>>>, 3> handles;
    uint32_t addresses[] = {ctx.r5.u32, ctx.r6.u32, ctx.r7.u32}; Socket maximum = 0;
    for (int i = 0; i < 3; ++i) {
        FD_ZERO(&sets[i]); if (!addresses[i]) continue;
        uint32_t count = load32(addresses[i]); if (count > 64) { fail(ctx, 10022); return; }
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t h = load32(addresses[i] + 4 + j * 4); auto s = lookupHandleAs<SocketObject>(h);
            if (!s || s->native == invalid) { fail(ctx, 10038); return; }
#ifndef _WIN32
            if (s->native >= FD_SETSIZE) { fail(ctx, 10024); return; }
#endif
            FD_SET(s->native, &sets[i]); maximum = std::max(maximum, s->native); handles[i].emplace_back(h, s);
        }
    }
    timeval timeout{};
    if (ctx.r8.u32) { timeout.tv_sec = load32(ctx.r8.u32); timeout.tv_usec = load32(ctx.r8.u32 + 4); }
    int rc = select(int(maximum + 1), addresses[0] ? &sets[0] : nullptr, addresses[1] ? &sets[1] : nullptr,
                    addresses[2] ? &sets[2] : nullptr, ctx.r8.u32 ? &timeout : nullptr);
    if (rc >= 0) for (int i = 0; i < 3; ++i) if (addresses[i]) {
        uint32_t count = 0;
        for (auto& [h, s] : handles[i]) if (FD_ISSET(s->native, &sets[i])) store32(addresses[i] + 4 + 4 * count++, h);
        store32(addresses[i], count);
    }
    result(ctx, rc);
}
