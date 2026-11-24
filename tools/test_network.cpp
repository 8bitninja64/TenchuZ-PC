// Real loopback traffic through the guest ABI; no game data. Small dispatcher
// fixture isolates network event delivery from the rest of the runtime.
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "kernel/kernel.h"
#include "kernel/network.h"
#include "guest/host.h"
#include <cassert>
#include <chrono>
#include <thread>

namespace tz {
std::mutex fixtureMutex;
std::map<uint32_t, std::shared_ptr<KernelObject>> objects;
std::map<uint32_t, bool> events;
GuestThread* currentThread() { return nullptr; }
std::string guestBacktrace(const PPCContext&, int) { return {}; }
void logf(const char*, ...) {}
bool traceEnabled(const char*) { return false; }
uint32_t load32(uint32_t p) { return memory->read32(p); }
uint16_t load16(uint32_t p) { return memory->read16(p); }
void store32(uint32_t p, uint32_t v) { memory->write32(p, v); }
void store16(uint32_t p, uint16_t v) { memory->write16(p, v); }
uint32_t createHandle(std::shared_ptr<KernelObject> o) {
    std::lock_guard lock(fixtureMutex); static uint32_t next = 1;
    objects[next] = std::move(o); return next++;
}
std::shared_ptr<KernelObject> lookupHandle(uint32_t h) {
    std::lock_guard lock(fixtureMutex); auto it = objects.find(h);
    return it == objects.end() ? nullptr : it->second;
}
bool closeHandle(uint32_t h) { std::lock_guard lock(fixtureMutex); return objects.erase(h) != 0; }
uint32_t createDispatcherObject(uint8_t, uint32_t, int32_t) {
    std::lock_guard lock(fixtureMutex); static uint32_t next = 1; events[next] = false; return next++;
}
int32_t setEvent(uint32_t e) { std::lock_guard lock(fixtureMutex); events[e] = true; return 0; }
int32_t resetEvent(uint32_t e) { std::lock_guard lock(fixtureMutex); events[e] = false; return 0; }
uint32_t waitForObjects(const std::vector<uint32_t>& es, bool all, bool, const int64_t* timeout) {
    assert(!timeout || *timeout <= 0); // relative milliseconds, not absolute time
    auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout ? -*timeout * 100 : 1000000000);
    do {
        { std::lock_guard lock(fixtureMutex); bool every = true;
          for (size_t i = 0; i < es.size(); ++i) {
              if (events[es[i]] && !all) return uint32_t(i);
              every &= events[es[i]];
          }
          if (all && every) return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < until);
    return kStatusTimeout;
}
}
using namespace tz;
uint32_t call(const char* name, std::initializer_list<uint32_t> args) {
    PPCContext ctx{}; int i = 0;
    PPCRegister* regs[] = {&ctx.r3,&ctx.r4,&ctx.r5,&ctx.r6,&ctx.r7,&ctx.r8,&ctx.r9,&ctx.r10};
    for (auto value : args) regs[i++]->u64 = value;
    auto fn = reinterpret_cast<PPCFunc*>(host::registeredFunctions().at(std::string("__imp__NetDll_") + name).address);
    (*fn)(ctx, memory->base()); return ctx.r3.u32;
}
void address(uint32_t p, uint16_t port, uint32_t ip = 0x7F000001) {
    std::memset(memory->host(p), 0, 16); store16(p, 2); store16(p + 2, port); store32(p + 4, ip);
}
int main(int argc, char**) {
#ifdef _WIN32
    _putenv_s("TENCHU_SYSTEM_LINK", argc > 1 ? "0" : "1");
    _putenv_s("TENCHU_NET_BIND", "127.0.0.1");
#else
    setenv("TENCHU_SYSTEM_LINK", argc > 1 ? "0" : "1", 1);
    setenv("TENCHU_NET_BIND", "127.0.0.1", 1);
#endif
    Memory guest; memory = &guest;
    uint32_t p = guest.allocateVirtual(0x10000), a = p, b = p + 32, len = p + 64, data = p + 128, out = p + 256;
    if (argc > 1) {
        assert(call("socket", {1,2,2,17}) == 0xFFFFFFFF);
        assert(call("WSAGetLastError", {}) == 10050);
        assert(call("XNetGetEthernetLinkStatus", {1}) == 0);
        std::puts("Offline network behavior passed."); return 0;
    }
    uint32_t udp = call("socket", {1,2,2,254}); assert(udp != 0xFFFFFFFF);
    address(a, 0); assert(call("bind", {1,udp,a,16}) == 0);
    store32(len,16); assert(call("getsockname", {1,udp,b,len}) == 0);
    assert(load16(b) == 2 && load16(b+2) && load32(b+4) == 0x7F000001);
    uint32_t event = call("WSACreateEvent", {});
    assert(call("WSAEventSelect", {1,udp,event,1}) == 0);
    assert(call("recv", {1,udp,out,32,0}) == 0xFFFFFFFF);
    assert(call("WSAGetLastError", {}) == 10035);
    store32(p+80,event);
    assert(call("WSAWaitForMultipleEvents", {1,p+80,0,20,0}) == 258);
    std::memcpy(memory->host(data), "network-byte-order", 19);
    assert(call("sendto", {1,udp,data,19,0,b,16}) == 19);
    assert(call("WSAWaitForMultipleEvents", {1,p+80,0,1000,0}) == 0);
    store32(len,16); assert(call("recvfrom", {1,udp,out,32,0,a,len}) == 19);
    assert(std::memcmp(memory->host(data),memory->host(out),19) == 0);
    assert(load16(a+2) == load16(b+2) && load32(a+4) == 0x7F000001);
    assert(call("closesocket", {1,udp}) == 0);
    assert(call("send", {1,udp,data,1,0}) == 0xFFFFFFFF);
    assert(call("WSAGetLastError", {}) == 10038);
    assert(call("WSACloseEvent", {event}) == 1);
    uint32_t server = call("socket", {1,2,1,6}); address(a,0);
    assert(call("bind", {1,server,a,16}) == 0);
    assert(call("listen", {1,server,1}) == 0);
    store32(len,16); assert(call("getsockname", {1,server,b,len}) == 0);
    uint32_t client = call("socket", {1,2,1,6});
    uint32_t pendingEvent = call("WSACreateEvent", {});
    assert(call("WSAEventSelect", {1,client,pendingEvent,33}) == 0);
    store32(p+80,pendingEvent);
    assert(call("WSAWaitForMultipleEvents", {1,p+80,0,30,0}) == 258);
    store32(p+84,1);
    assert(call("ioctlsocket", {1,client,0x8004667E,p+84}) == 0);
    const uint32_t connecting = call("connect", {1,client,b,16});
    assert(connecting == 0 || (connecting == 0xFFFFFFFF && call("WSAGetLastError", {}) == 10035));
    uint32_t set = p+400, timeout = p+800;
    store32(set,1); store32(set+4,client); store32(timeout,1); store32(timeout+4,0);
    assert(call("select", {1,0,0,set,0,timeout}) == 1);
    store32(len,4);
    assert(call("getsockopt", {1,client,0xFFFF,0x1007,p+88,len}) == 0 && load32(p+88) == 0);
    store32(len,16); uint32_t peer = call("accept", {1,server,a,len}); assert(peer != 0xFFFFFFFF);
    assert(call("send", {1,client,data,19,0}) == 19);
    store32(set,1); store32(set+4,peer); store32(timeout,1); store32(timeout+4,0);
    assert(call("select", {1,0,set,0,0,timeout}) == 1 && load32(set+4) == peer);
    assert(call("recv", {1,peer,out,32,0}) == 19);
    assert(std::memcmp(memory->host(data),memory->host(out),19) == 0);
    net::beginDisconnect(client);
    assert(call("recv", {1,peer,out,32,0}) == 0); // orderly peer disconnect
    net::beginDisconnect(peer);
    store32(set,1); store32(set+4,client);
    assert(call("select", {1,0,set,0,0,timeout}) == 1);
    assert(call("recv", {1,client,out,32,0}) == 0);
    for (auto s : {server,client,peer}) assert(call("closesocket", {1,s}) == 0);
    assert(call("WSACloseEvent", {pendingEvent}) == 1);
    // A reserved but non-listening local port gives a deterministic refusal.
    uint32_t reserved = call("socket", {1,2,1,6}); address(a,0);
    assert(call("bind", {1,reserved,a,16}) == 0);
    store32(len,16); assert(call("getsockname", {1,reserved,b,len}) == 0);
    uint32_t refused = call("socket", {1,2,1,6}); store32(p+84,1);
    assert(call("ioctlsocket", {1,refused,0x8004667E,p+84}) == 0);
    assert(call("connect", {1,refused,b,16}) == 0xFFFFFFFF);
    const uint32_t refusal = call("WSAGetLastError", {});
    assert(refusal == 10035 || refusal == 10061);
    if (refusal == 10035) {
        uint32_t errors = p+600;
        store32(set,1); store32(set+4,refused);
        store32(errors,1); store32(errors+4,refused);
        store32(timeout,8); store32(timeout+4,0);
        assert(call("select", {1,0,0,set,errors,timeout}) > 0);
        store32(len,4);
        assert(call("getsockopt", {1,refused,0xFFFF,0x1007,p+88,len}) == 0);
        assert(load32(p+88) == 10061); // guest WSAECONNREFUSED, not POSIX errno
    }
    assert(call("closesocket", {1,reserved}) == 0);
    assert(call("closesocket", {1,refused}) == 0);
    // Title clients also request port 1003. Reconnect immediately, without
    // waiting for the old desktop TCP four-tuple to leave TIME_WAIT.
    server = call("socket", {1,2,1,6}); address(a,1003,0x7F000009);
    assert(call("bind", {1,server,a,16}) == 0);
    assert(call("listen", {1,server,4}) == 0);
    store32(len,16); assert(call("getsockname", {1,server,b,len}) == 0);
    assert(load16(b+2) == 1003);
    // The advertised guest port stays 1003; the actual OS listener is 11003.
    auto nativeClient = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(nativeClient != decltype(nativeClient)(-1));
    sockaddr_in nativeAddress{};
    nativeAddress.sin_family = AF_INET;
    nativeAddress.sin_addr.s_addr = htonl(0x7F000009);
    nativeAddress.sin_port = htons(11003);
    assert(::connect(nativeClient, reinterpret_cast<sockaddr*>(&nativeAddress), sizeof(nativeAddress)) == 0);
    store32(len,16); peer = call("accept", {1,server,a,len});
    assert(peer != 0xFFFFFFFF);
    net::beginDisconnect(peer);
    char eof{}; assert(::recv(nativeClient, &eof, 1, 0) == 0);
#ifdef _WIN32
    closesocket(nativeClient);
#else
    close(nativeClient);
#endif
    assert(call("closesocket", {1,peer}) == 0);
    uint16_t previousPort = 0;
    for (int cycle = 0; cycle < 3; ++cycle) {
        client = call("socket", {1,2,1,6}); address(a,1003,0x7F000008);
        assert(call("bind", {1,client,a,16}) == 0);
        assert(call("connect", {1,client,b,16}) == 0);
        store32(len,16); peer = call("accept", {1,server,a,len});
        assert(peer != 0xFFFFFFFF && load32(a+4) == 0x7F000008);
        const uint16_t sourcePort = load16(a+2);
        assert(sourcePort && sourcePort != 1003 && sourcePort != previousPort);
        previousPort = sourcePort;
        assert(call("send", {1,client,data,19,0}) == 19);
        assert(call("recv", {1,peer,out,32,0}) == 19);
        net::beginDisconnect(client);
        assert(call("recv", {1,peer,out,32,0}) == 0);
        net::beginDisconnect(peer);
        assert(call("recv", {1,client,out,32,0}) == 0);
        assert(call("closesocket", {1,client}) == 0);
        assert(call("closesocket", {1,peer}) == 0);
    }
    assert(call("closesocket", {1,server}) == 0);
    assert(call("XNetGetTitleXnAddr", {1,p+1000}) == 6);
    assert(load32(p+1000) == 0x7F000001);
    for (uint32_t ip = 0x7F000001; ip <= 0x7F000004; ++ip) {
        // Model each peer's advertised address and round-trip the same ABI the
        // title uses before comparing abEnet in its four-player roster.
        net::titleAddress(p+1000);
        store32(p+1000, ip); store32(p+1012, ip);
        assert(call("XNetXnAddrToInAddr", {1,p+1000,0,p+1100}) == 0);
        assert(load32(p+1100) == ip);
        assert(call("XNetInAddrToXnAddr", {1,ip,p+1200,0}) == 0);
        assert(std::memcmp(memory->host(p+1000),memory->host(p+1200),36) == 0);
    }
    assert(net::playerId() == 0xE00000007F000001ull);
    std::puts("Guest networking: UDP, TCP, nonblocking errors, events, select and addresses passed.");
}
