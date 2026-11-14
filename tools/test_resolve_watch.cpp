#include "kernel/kernel.h"
#include <cassert>
#include <cstdio>

namespace tz {
GuestThread* currentThread() { return nullptr; }
std::string guestBacktrace(const PPCContext&, int) { return {}; }
void logf(const char*, ...) {}
}

int main() {
    tz::Memory guest;
    tz::memory = &guest;
    constexpr uint32_t page = 0x2000;
    const auto resolved = tz::Memory::kWatchResolves;
    const auto textures = tz::Memory::kWatchTextures;
    for (uint32_t alias : {0xA2000000u, 0xC2000000u, 0xE1FFF000u}) {
        guest.watchPages(resolved, page, 2);
        guest.watchPages(textures, page, 2);
        assert(guest.rangeWatched(resolved, page, page + 1));
        // A real write through every physical alias must retire GPU resolves.
        *reinterpret_cast<volatile uint32_t*>(guest.host(alias)) = 42;
        assert(!guest.rangeWatched(resolved, page, page + 1));
        assert(guest.pageWatched(resolved, page + 1));
        // Uploading a regular texture must not resurrect the stale resolve.
        guest.watchPages(textures, page, 2);
        assert(!guest.pageWatched(resolved, page));
    }
    guest.watchPages(resolved, page, 2);
    // File reads and other OS writes announce their destination explicitly.
    guest.prepareHostWrite(0xA2000FF0u, 32);
    assert(!guest.pageWatched(resolved, page));
    assert(!guest.pageWatched(resolved, page + 1));
    tz::memory = nullptr;
    std::puts("Resolve write-watch regression passed.");
}
