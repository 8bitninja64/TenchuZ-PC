#include "memory.h"
#include <windows.h>
#include <cstring>
#include <stdexcept>

namespace tz {
Memory* memory = nullptr;

namespace {
constexpr uint64_t kAddressSpace = 0x100000000ull;
constexpr uint32_t kPage = 0x1000;

uint64_t alignUp(uint64_t value, uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
}  // namespace

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

Memory::~Memory() {
    // The process is exiting; the OS reclaims the reservation and views.
}

bool Memory::commit(uint32_t address, uint32_t size) {
    if (!size || uint64_t(address) + size > 0xA0000000ull || address < 0x10000) return false;
    return VirtualAlloc(base_ + address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

bool Memory::decommit(uint32_t address, uint32_t size) {
    if (!size || uint64_t(address) + size > 0xA0000000ull) return false;
    return VirtualFree(base_ + address, size, MEM_DECOMMIT) != 0;
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
    std::lock_guard lock(mutex_);
    size = uint32_t(alignUp(size, kPage));
    alignment = (std::max)(alignment, kPage);
    uint32_t address;
    if (!firstFit(virtual_, kVirtualBegin, kVirtualEnd, size, alignment, address)) return 0;
    if (!commit(address, size)) { virtual_.erase(address); return 0; }
    std::memset(base_ + address, 0, size);
    return address;
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
