#include "memory.h"
#include "fault.h"
#include "kernel.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <cstring>
#include <string>
#include <stdexcept>

namespace tz {
Memory* memory = nullptr;

namespace {
constexpr uint64_t kAddressSpace = 0x100000000ull;
constexpr uint32_t kPage = 0x1000;

uint64_t alignUp(uint64_t value, uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
}  // namespace

#ifdef _WIN32
Memory::Memory() {
    auto kernelbase = GetModuleHandleW(L"kernelbase.dll");
    auto reserve = reinterpret_cast<decltype(&VirtualAlloc2)>(GetProcAddress(kernelbase, "VirtualAlloc2"));
    auto mapView = reinterpret_cast<decltype(&MapViewOfFile3)>(GetProcAddress(kernelbase, "MapViewOfFile3"));
    if (!reserve || !mapView) throw std::runtime_error("Windows 10 1803 or newer is required");

    base_ = static_cast<uint8_t*>(reserve(GetCurrentProcess(), nullptr, kAddressSpace,
                                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    if (!base_) throw std::runtime_error("Cannot reserve the 4 GiB guest address space");

    // Split the placeholder into: plain [0, A0000000), three physical views,
    // and plain [FFFFF000, 4G). The E view starts 4 KiB into physical memory.
    struct Region { uint64_t begin, end; int64_t physical; };
    const Region regions[] = {
        {0x00000000, 0xA0000000, -1},
        {0xA0000000, 0xC0000000, 0},
        {0xC0000000, 0xE0000000, 0},
        {0xE0000000, 0xFFFFF000, kPage},
        {0xFFFFF000, kAddressSpace, -1},
    };
    for (const auto& region : regions) {
        if (region.end == kAddressSpace) break;
        if (!VirtualFree(base_ + region.begin, size_t(region.end - region.begin),
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            throw std::runtime_error("Cannot split the guest address placeholder");
    }
    backing_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0,
                                  kPhysicalSize, nullptr);
    if (!backing_) throw std::runtime_error("Cannot create guest physical memory");
    for (const auto& region : regions) {
        const size_t size = size_t(region.end - region.begin);
        if (region.physical < 0) {
            if (!reserve(GetCurrentProcess(), base_ + region.begin, size, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                         PAGE_NOACCESS, nullptr, 0))
                throw std::runtime_error("Cannot reserve guest virtual memory");
        } else if (!mapView(backing_, GetCurrentProcess(), base_ + region.begin, uint64_t(region.physical), size,
                            MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) {
            throw std::runtime_error("Cannot map guest physical memory");
        }
    }
    // Kernel-owned structures (data imports, objects) live here permanently.
    if (!commit(kKernelBegin, kKernelEnd - kKernelBegin))
        throw std::runtime_error("Cannot commit kernel memory");
}
#else
// Linux: one PROT_NONE reservation; physical memory is a memfd mapped three times over it.
Memory::Memory() {
    void* reservation = mmap(nullptr, kAddressSpace, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reservation == MAP_FAILED) throw std::runtime_error("Cannot reserve the 4 GiB guest address space");
    base_ = static_cast<uint8_t*>(reservation);
    const int physical = memfd_create("tenchu-physical", MFD_CLOEXEC);
    if (physical < 0 || ftruncate(physical, kPhysicalSize) != 0)
        throw std::runtime_error("Cannot create guest physical memory");
    struct View { uint64_t begin, end; off_t offset; };
    const View views[] = {{0xA0000000, 0xC0000000, 0}, {0xC0000000, 0xE0000000, 0}, {0xE0000000, 0xFFFFF000, kPage}};
    for (const auto& view : views) {
        if (mmap(base_ + view.begin, size_t(view.end - view.begin), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                 physical, view.offset) == MAP_FAILED)
            throw std::runtime_error("Cannot map guest physical memory");
    }
    close(physical);
    if (!commit(kKernelBegin, kKernelEnd - kKernelBegin))
        throw std::runtime_error("Cannot commit kernel memory");
}
#endif

// Writes to watched physical pages: unprotect and let the store run again.
static bool watchedPageFault(fault::Info& info) {
    if (!info.write || !memory) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory->base());
    const uintptr_t host = info.address;
    if (host < base + 0xA0000000ull || host >= base + 0xFFFFF000ull) return false;
    const uint32_t physical = Memory::physicalAddress(uint32_t(host - base));
    return memory->unwatchForWrite(physical >> 12);
}

static void protect(void* address, size_t size, bool readOnly) {
#ifdef _WIN32
    DWORD old;
    VirtualProtect(address, size, readOnly ? PAGE_READONLY : PAGE_READWRITE, &old);
#else
    mprotect(address, size, readOnly ? PROT_READ : PROT_READ | PROT_WRITE);
#endif
}

void Memory::protectPhysical(uint32_t firstPage, uint32_t count, bool readOnly) {
    const size_t offset = size_t(firstPage) * kPage, size = size_t(count) * kPage;
    protect(base_ + 0xA0000000ull + offset, size, readOnly);
    protect(base_ + 0xC0000000ull + offset, size, readOnly);
    // The E view maps physical memory from its second page on.
    if (firstPage == 0) {
        if (count == 1) return;
        protect(base_ + 0xE0000000ull, size - kPage, readOnly);
    } else {
        protect(base_ + 0xE0000000ull + offset - kPage, size, readOnly);
    }
}

void Memory::watchPages(WatchClient client, uint32_t firstPage, uint32_t count) {
    static const bool installed = (fault::addHandler(watchedPageFault), true);
    (void)installed;
    std::lock_guard lock(watchMutex_);
    uint32_t run = 0;
    for (uint32_t page = firstPage; page <= firstPage + count; ++page) {
        const bool inRange = page < firstPage + count && page < kPhysicalPages;
        if (inRange) valid_[client][page >> 6].fetch_or(1ull << (page & 63), std::memory_order_acq_rel);
        if (inRange && !pageProtected(page)) {
            // Protect before the caller copies: a write after this point faults.
            protected_[page >> 6].fetch_or(1ull << (page & 63), std::memory_order_acq_rel);
            ++run;
        } else if (run) {
            protectPhysical(page - run, run, true);
            run = 0;
        }
    }
}

void Memory::probeWrites(uint32_t firstPage, uint32_t count) {
    std::lock_guard lock(watchMutex_);
    for (uint32_t page = firstPage; page < firstPage + count && page < kPhysicalPages; ++page) {
        probed_[page >> 6].fetch_or(1ull << (page & 63));
        if (!pageProtected(page)) {
            protected_[page >> 6].fetch_or(1ull << (page & 63));
            protectPhysical(page, 1, true);
        }
    }
}

bool Memory::unwatchForWrite(uint32_t physicalPage) {
    if (physicalPage >= kPhysicalPages) return false;
    if ((probed_[physicalPage >> 6].fetch_and(~(1ull << (physicalPage & 63))) >> (physicalPage & 63)) & 1) {
        // Distinct call stacks only (the same writers recur every frame).
        static std::mutex seenMutex;
        static std::vector<std::string> seen;
        if (auto* thread = currentThread()) {
            const std::string stack = guestBacktrace(thread->context, 10);
            std::lock_guard seenLock(seenMutex);
            if (seen.size() < 400 && std::find(seen.begin(), seen.end(), stack) == seen.end()) {
                seen.push_back(stack);
                logf("[writer] page %05X written by %s in %08X |%s", physicalPage, thread->name.c_str(),
                     thread->context.lastFunction, stack.c_str());
            }
        }
    }
    std::lock_guard lock(watchMutex_);
    if (pageProtected(physicalPage)) {
        const uint64_t clear = ~(1ull << (physicalPage & 63));
        for (auto& valid : valid_) valid[physicalPage >> 6].fetch_and(clear, std::memory_order_acq_rel);
        protected_[physicalPage >> 6].fetch_and(clear, std::memory_order_acq_rel);
        protectPhysical(physicalPage, 1, false);
        return true;
    }
    // Another thread already unprotected it, or the page was never ours to protect.
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(base_ + 0xA0000000ull + size_t(physicalPage) * kPage, &info, sizeof(info));
    return info.Protect == PAGE_READWRITE;
#else
    // Physical views are only ever read-only through protectPhysical, and that ran under
    // watchMutex_: the page is writable again.
    return true;
#endif
}

bool Memory::rangeWatched(WatchClient client, uint32_t firstPage, uint32_t lastPage) const {
    if (lastPage >= kPhysicalPages || firstPage > lastPage) return false;
    for (uint32_t word = firstPage >> 6; word <= lastPage >> 6; ++word) {
        uint64_t need = ~0ull;
        if (word == firstPage >> 6) need &= ~0ull << (firstPage & 63);
        if (word == lastPage >> 6) need &= ~0ull >> (63 - (lastPage & 63));
        if ((valid_[client][word].load(std::memory_order_acquire) & need) != need) return false;
    }
    return true;
}

void Memory::prepareHostWrite(uint32_t address, uint32_t size) {
    if (address < 0xA0000000 || !size) return;
    const uint32_t first = physicalAddress(address) >> 12;
    const uint32_t last = (physicalAddress(address) + size - 1) >> 12;
    for (uint32_t page = first; page <= last && page < kPhysicalPages; ++page)
        if (pageProtected(page)) unwatchForWrite(page);
}

Memory::~Memory() {
    // The process is exiting; the OS reclaims the reservation and views.
}

bool Memory::commit(uint32_t address, uint32_t size) {
    if (!size || uint64_t(address) + size > 0xA0000000ull || address < 0x10000) return false;
#ifdef _WIN32
    return VirtualAlloc(base_ + address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(base_ + address, size, PROT_READ | PROT_WRITE) == 0;
#endif
}

bool Memory::decommit(uint32_t address, uint32_t size) {
    if (!size || uint64_t(address) + size > 0xA0000000ull) return false;
#ifdef _WIN32
    return VirtualFree(base_ + address, size, MEM_DECOMMIT) != 0;
#else
    // Anonymous private pages read back as zero after MADV_DONTNEED, like a fresh commit.
    madvise(base_ + address, size, MADV_DONTNEED);
    return mprotect(base_ + address, size, PROT_NONE) == 0;
#endif
}

bool Memory::firstFit(std::map<uint32_t, uint32_t>& used, uint64_t begin, uint64_t end,
                      uint32_t size, uint32_t alignment, uint32_t& result) {
    uint64_t start = alignUp(begin, alignment);
    for (const auto& [address, bytes] : used) {
        if (uint64_t(address) + bytes <= start) continue;
        if (start + size <= address) break;
        start = alignUp(uint64_t(address) + bytes, alignment);
    }
    if (start + size > end) return false;
    used.emplace(uint32_t(start), size);
    result = uint32_t(start);
    return true;
}

uint32_t Memory::allocateVirtual(uint32_t size, uint32_t alignment) {
    const uint32_t address = reserveVirtual(size, 0);
    if (address) commitVirtual(address, size);
    (void)alignment;
    return address;
}

uint32_t Memory::reserveVirtual(uint32_t size, uint32_t requested) {
    std::lock_guard lock(mutex_);
    if (committed_.empty()) committed_.assign((kVirtualEnd - kVirtualBegin) / kPage, false);
    size = uint32_t(alignUp(size, 0x10000));
    uint32_t address;
    if (requested) {
        // An exact placement must not overlap an existing reservation.
        address = requested & ~0xFFFFu;
        if (address < kVirtualBegin || uint64_t(address) + size > kVirtualEnd) return 0;
        auto next = virtual_.lower_bound(address);
        if (next != virtual_.end() && next->first < address + size) return 0;
        if (next != virtual_.begin() && std::prev(next)->first + std::prev(next)->second > address) return 0;
        virtual_.emplace(address, size);
    } else if (!firstFit(virtual_, kVirtualBegin, kVirtualEnd, size, 0x10000, address)) {
        return 0;
    }
    // The host keeps the whole reservation accessible; guest commit state is tracked separately.
    if (!commit(address, size)) { virtual_.erase(address); return 0; }
    std::memset(base_ + address, 0, size);
    return address;
}

bool Memory::commitVirtual(uint32_t address, uint32_t size) {
    std::lock_guard lock(mutex_);
    const uint32_t first = address & ~(kPage - 1);
    const uint64_t last = alignUp(uint64_t(address) + size, kPage);
    if (first < kVirtualBegin || last > kVirtualEnd || committed_.empty()) return false;
    for (uint64_t page = first; page < last; page += kPage) {
        const size_t index = size_t((page - kVirtualBegin) / kPage);
        if (committed_[index]) continue;
        std::memset(base_ + page, 0, kPage);
        committed_[index] = true;
    }
    return true;
}

bool Memory::decommitVirtual(uint32_t address, uint32_t size) {
    std::lock_guard lock(mutex_);
    const uint32_t first = address & ~(kPage - 1);
    const uint64_t last = alignUp(uint64_t(address) + size, kPage);
    if (first < kVirtualBegin || last > kVirtualEnd || committed_.empty()) return false;
    for (uint64_t page = first; page < last; page += kPage) {
        committed_[size_t((page - kVirtualBegin) / kPage)] = false;
        std::memset(base_ + page, 0, kPage);
    }
    return true;
}

Memory::RegionInfo Memory::queryVirtual(uint32_t address) const {
    std::lock_guard lock(mutex_);
    RegionInfo info{address & ~(kPage - 1), 0, kPage, 0x10000};
    auto it = virtual_.upper_bound(address);
    if (it == virtual_.begin() || committed_.empty()) return info;
    --it;
    if (address >= uint64_t(it->first) + it->second) {
        // Free: extend to the next reservation.
        info.size = (it = virtual_.upper_bound(address)) == virtual_.end() ? kVirtualEnd - info.base : it->first - info.base;
        return info;
    }
    info.allocationBase = it->first;
    const uint64_t end = uint64_t(it->first) + it->second;
    auto pageCommitted = [&](uint64_t page) { return bool(committed_[size_t((page - kVirtualBegin) / kPage)]); };
    const bool state = pageCommitted(info.base);
    uint64_t page = info.base;
    while (page < end && pageCommitted(page) == state) page += kPage;
    info.size = uint32_t(page - info.base);
    info.state = state ? 0x1000 : 0x2000;
    return info;
}

uint32_t Memory::allocateStack(uint32_t size) {
    std::lock_guard lock(mutex_);
    // One uncommitted guard page below each stack.
    size = uint32_t(alignUp(size, 0x10000));
    uint32_t reservation;
    if (!firstFit(stacks_, kStackBegin, kStackEnd, size + 0x10000, 0x10000, reservation)) return 0;
    if (!commit(reservation + 0x10000, size)) { stacks_.erase(reservation); return 0; }
    std::memset(base_ + reservation + 0x10000, 0, size);
    return reservation + 0x10000;
}

uint32_t Memory::allocatePhysical(uint32_t size, uint32_t alignment, uint32_t alias,
                                  uint32_t minPhysical, uint32_t maxPhysical) {
    std::lock_guard lock(mutex_);
    size = uint32_t(alignUp(size, kPage));
    alignment = (std::max)(alignment, kPage);
    // The E view is biased by one page, so its first page is unreachable.
    const uint32_t bias = alias == 0xE0000000 ? kPage : 0;
    const uint64_t end = (std::min)(uint64_t(maxPhysical) + 1, uint64_t(kPhysicalSize));
    uint32_t offset;
    if (!firstFit(physical_, (std::max)(minPhysical, bias), end, size, alignment, offset)) return 0;
    std::memset(base_ + 0xA0000000ull + offset, 0, size);
    const uint32_t address = alias + offset - bias;
    physicalAliases_[address] = offset;
    return address;
}

uint32_t Memory::allocateKernel(uint32_t size, uint32_t alignment) {
    std::lock_guard lock(mutex_);
    const uint64_t address = alignUp(kernelCursor_, alignment);
    if (address + size > kKernelEnd) return 0;
    kernelCursor_ = uint32_t(address + size);
    std::memset(base_ + address, 0, size);
    return uint32_t(address);
}

bool Memory::free(uint32_t address) {
    std::lock_guard lock(mutex_);
    if (auto it = physicalAliases_.find(address); it != physicalAliases_.end()) {
        physical_.erase(it->second);
        physicalAliases_.erase(it);
        return true;
    }
    if (auto it = virtual_.find(address); it != virtual_.end()) {
        for (uint64_t page = it->first; page < uint64_t(it->first) + it->second; page += kPage)
            committed_[size_t((page - kVirtualBegin) / kPage)] = false;
        decommit(it->first, it->second);
        virtual_.erase(it);
        return true;
    }
    if (auto it = stacks_.find(address - 0x10000); it != stacks_.end()) {
        decommit(address, it->second - 0x10000);
        stacks_.erase(it);
        return true;
    }
    return false;
}

uint32_t Memory::allocationSize(uint32_t address) const {
    std::lock_guard lock(mutex_);
    if (auto it = physicalAliases_.find(address); it != physicalAliases_.end())
        return physical_.at(it->second);
    if (auto it = virtual_.find(address); it != virtual_.end()) return it->second;
    return 0;
}

bool Memory::query(uint32_t address, uint32_t& regionBase, uint32_t& regionSize) const {
    std::lock_guard lock(mutex_);
    auto find = [&](const std::map<uint32_t, uint32_t>& used, uint32_t key, uint32_t& start, uint32_t& bytes) {
        auto it = used.upper_bound(key);
        if (it == used.begin()) return false;
        --it;
        if (key >= uint64_t(it->first) + it->second) return false;
        start = it->first;
        bytes = it->second;
        return true;
    };
    if (address >= 0xA0000000) {
        uint32_t offset, bytes;
        if (!find(physical_, physicalAddress(address), offset, bytes)) return false;
        regionBase = address - (physicalAddress(address) - offset);
        regionSize = bytes;
        return true;
    }
    return find(virtual_, address, regionBase, regionSize) || find(stacks_, address, regionBase, regionSize);
}

uint64_t Memory::physicalBytesInUse() const {
    std::lock_guard lock(mutex_);
    uint64_t total = 0;
    for (const auto& [offset, bytes] : physical_) total += bytes;
    return total;
}

uint32_t Memory::read32(uint32_t address) const { return _byteswap_ulong(*host<uint32_t>(address)); }
void Memory::write32(uint32_t address, uint32_t value) { *host<uint32_t>(address) = _byteswap_ulong(value); }
uint16_t Memory::read16(uint32_t address) const { return _byteswap_ushort(*host<uint16_t>(address)); }
void Memory::write16(uint32_t address, uint16_t value) { *host<uint16_t>(address) = _byteswap_ushort(value); }
uint64_t Memory::read64(uint32_t address) const { return _byteswap_uint64(*host<uint64_t>(address)); }
void Memory::write64(uint32_t address, uint64_t value) { *host<uint64_t>(address) = _byteswap_uint64(value); }

}  // namespace tz
