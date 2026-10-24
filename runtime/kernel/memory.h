#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace tz {

// Guest address space layout (Xbox 360 conventions):
//   0x40000000-0x70000000  virtual allocations (NtAllocateVirtualMemory)
//   0x70000000-0x7F000000  guest thread stacks
//   0x7FF00000-0x80000000  per-thread PCR/TLS/KTHREAD pages
//   0x80000000-0x90000000  kernel data, XEX header, image, function table
//   0xA0000000/0xC0000000/0xE0000000  views of the same 512 MiB physical memory
class Memory {
public:
    static constexpr uint32_t kPhysicalSize = 0x20000000;
    static constexpr uint32_t kVirtualBegin = 0x40000000;
    static constexpr uint32_t kVirtualEnd = 0x70000000;
    static constexpr uint32_t kStackBegin = 0x70000000;
    static constexpr uint32_t kStackEnd = 0x7F000000;
    static constexpr uint32_t kPcrBegin = 0x7FF00000;
    static constexpr uint32_t kKernelBegin = 0x80000000;
    static constexpr uint32_t kKernelEnd = 0x81000000;

    Memory();
    ~Memory();
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;

    uint8_t* base() const { return base_; }
    template <typename T = uint8_t> T* host(uint32_t address) const {
        return reinterpret_cast<T*>(base_ + address);
    }

    // Commits plain (non-physical) guest memory, zero-filled on first commit.
    bool commit(uint32_t address, uint32_t size);
    bool decommit(uint32_t address, uint32_t size);

    // Bump/first-fit allocators. All return 0 on failure.
    uint32_t allocateVirtual(uint32_t size, uint32_t alignment = 0x10000);

    // NtAllocateVirtualMemory semantics with per-page commit state.
    // reserveVirtual places the reservation at `requested` when nonzero.
    uint32_t reserveVirtual(uint32_t size, uint32_t requested = 0);
    // Commits pages inside a reservation; newly committed pages read as zero.
    bool commitVirtual(uint32_t address, uint32_t size);
    bool decommitVirtual(uint32_t address, uint32_t size);
    struct RegionInfo {
        uint32_t base, allocationBase, size, state;  // state: 0x1000 commit, 0x2000 reserve, 0x10000 free
    };
    RegionInfo queryVirtual(uint32_t address) const;
    uint32_t allocateStack(uint32_t size);
    uint32_t allocatePhysical(uint32_t size, uint32_t alignment, uint32_t alias,
                              uint32_t minPhysical = 0, uint32_t maxPhysical = kPhysicalSize - 1);
    // Small zeroed blocks for kernel-owned guest structures.
    uint32_t allocateKernel(uint32_t size, uint32_t alignment = 16);
    bool free(uint32_t address);
    uint32_t allocationSize(uint32_t address) const;
    // Region holding address, if it is a live allocation.
    bool query(uint32_t address, uint32_t& regionBase, uint32_t& regionSize) const;

    static uint32_t physicalAddress(uint32_t address) {
        return (address & 0x1FFFFFFF) + (address >= 0xE0000000 ? 0x1000 : 0);
    }
    // GPU addresses are physical; the 0xA0000000 view maps them 1:1.
    static uint32_t fromPhysical(uint32_t physical) { return 0xA0000000 | (physical & 0x1FFFFFFF); }

    uint32_t read32(uint32_t address) const;
    void write32(uint32_t address, uint32_t value);
    uint16_t read16(uint32_t address) const;
    void write16(uint32_t address, uint16_t value);
    uint64_t read64(uint32_t address) const;
    void write64(uint32_t address, uint64_t value);

    uint64_t physicalBytesInUse() const;

    // ---- Write watching of physical memory (4 KiB pages) -------------------------
    // Host copies of guest memory (GPU mirror, textures) stay valid until the guest
    // writes the page: watchPages marks the page valid for a client and write-protects
    // it in all three views; the first write faults, and the vectored handler
    // unprotects the page and clears it for every client. Writes made by the OS on our
    // behalf do not fault, so callers announce them (prepareHostWrite).
    static constexpr uint32_t kPhysicalPages = kPhysicalSize >> 12;
    enum WatchClient : uint32_t { kWatchMirror, kWatchTextures, kWatchShaders, kWatchClients };
    void watchPages(WatchClient client, uint32_t firstPage, uint32_t count);
    bool pageWatched(WatchClient client, uint32_t page) const {
        return (valid_[client][page >> 6].load(std::memory_order_acquire) >> (page & 63)) & 1;
    }
    // True when every page in [firstPage, lastPage] is valid for the client.
    bool rangeWatched(WatchClient client, uint32_t firstPage, uint32_t lastPage) const;
    void prepareHostWrite(uint32_t address, uint32_t size);
    // Debugging (TENCHU_FIND_WRITER): logs the guest function that next writes these pages.
    void probeWrites(uint32_t firstPage, uint32_t count);
    // Fault handler entry: true when the write may be retried.
    bool unwatchForWrite(uint32_t physicalPage);

private:
    void protectPhysical(uint32_t firstPage, uint32_t count, bool readOnly);
    bool pageProtected(uint32_t page) const {
        return (protected_[page >> 6].load(std::memory_order_acquire) >> (page & 63)) & 1;
    }
    std::unique_ptr<std::atomic<uint64_t>[]> valid_[kWatchClients] = {
        std::unique_ptr<std::atomic<uint64_t>[]>(new std::atomic<uint64_t>[kPhysicalPages / 64]()),
        std::unique_ptr<std::atomic<uint64_t>[]>(new std::atomic<uint64_t>[kPhysicalPages / 64]()),
        std::unique_ptr<std::atomic<uint64_t>[]>(new std::atomic<uint64_t>[kPhysicalPages / 64]())};
    std::unique_ptr<std::atomic<uint64_t>[]> protected_{new std::atomic<uint64_t>[kPhysicalPages / 64]()};
    std::mutex watchMutex_;
    std::unique_ptr<std::atomic<uint64_t>[]> probed_{new std::atomic<uint64_t>[kPhysicalPages / 64]()};

    static bool firstFit(std::map<uint32_t, uint32_t>& used, uint64_t begin, uint64_t end,
                         uint32_t size, uint32_t alignment, uint32_t& result);
    uint8_t* base_ = nullptr;
    void* backing_ = nullptr;
    mutable std::mutex mutex_;
    std::map<uint32_t, uint32_t> virtual_;   // guest address -> size
    std::vector<bool> committed_;            // per 4 KiB page of the virtual range
    std::map<uint32_t, uint32_t> stacks_;
    std::map<uint32_t, uint32_t> physical_;  // physical offset -> size
    std::map<uint32_t, uint32_t> physicalAliases_;  // guest address -> physical offset
    uint32_t kernelCursor_ = kKernelBegin;
};

extern Memory* memory;

}  // namespace tz
