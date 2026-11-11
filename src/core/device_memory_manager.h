// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <deque>
#include <memory>
#include <mutex>
#include <source_location>
#include <vector>
#include <span>

#include "common/common_types.h"
#include "common/range_mutex.h"
#include "common/scratch_buffer.h"
#include "common/sparse_large_vector.h"
#include "common/thread_worker.h"
#include "core/download_landing.h"
#include "core/early_read_report_pages.h"

namespace Core {

constexpr size_t DEVICE_PAGEBITS = 12ULL;
constexpr size_t DEVICE_PAGESIZE = 1ULL << DEVICE_PAGEBITS;
constexpr size_t DEVICE_PAGEMASK = DEVICE_PAGESIZE - 1ULL;

class DeviceMemory;

namespace Memory {
class Memory;
}

template <typename DTraits>
struct DeviceMemoryManagerAllocator;

struct Asid {
    size_t id;
};

template <typename Traits>
class DeviceMemoryManager {
    using DeviceInterface = typename Traits::DeviceInterface;
    using DeviceMethods = typename Traits::DeviceMethods;

public:
    DeviceMemoryManager(const DeviceMemory& device_memory);
    ~DeviceMemoryManager();

    static constexpr bool HAS_FLUSH_INVALIDATION = true;
    static constexpr size_t AS_BITS = Traits::device_virtual_bits;

    void BindInterface(DeviceInterface* device_inter);

    void ExcludeEarlyReadPointer(const u8* ptr, size_t size) {
        if (!Core::EarlyBufferReadsRequested() || !ptr || !size || !IsPhysicalPointer(ptr))
            return;
        std::scoped_lock lock{mapping_guard};
        const auto physical = GetRawPhysicalAddr(ptr);
        const auto last = (physical + size - 1) >> page_bits;
        for (auto page = physical >> page_bits; page <= last; ++page) {
            early_read_reports.Exclude(static_cast<u32>(page));
        }
    }

    void ExcludeEarlyReadReport(DAddr address, u64 size) {
        if (!size || address >= device_as_size || size > device_as_size - address)
            return;
        std::scoped_lock lock{mapping_guard};
        const auto last = (address + size - 1) >> page_bits;
        for (auto page = address >> page_bits; page <= last; ++page) {
            const auto backing = tracked_entries[page].compressed_physical_ptr;
            if (backing) {
                early_read_reports.Exclude(backing - 1);
                // Some report engines memcpy a scalar through the first host pointer.
                if (page < last)
                    early_read_reports.Exclude(backing);
            }
        }
    }

    template <typename Func>
    bool WithEarlyReadPage(DAddr page, u64 generation, Func&& action) {
        const std::array<DAddr, 1> addresses{page};
        bool used{};
        WithUniquePhysicalPages(addresses, generation, [&] {
            const u32 backing = tracked_entries[page >> page_bits].compressed_physical_ptr;
            if (!early_read_reports.Excluded(backing - 1)) {
                action();
                used = true;
            }
        });
        return used;
    }
    DownloadLanding download_landing;

    /// Marks the memory behind [address, address + size) cold (Common::HostMemory::HintCold),
    /// from a low-priority thread: data the host GPU has its own copy of, e.g. uploaded textures.
    void HintCold(DAddr address, size_t size);

    DAddr Allocate(size_t size);
    void AllocateFixed(DAddr start, size_t size);
    void Free(DAddr start, size_t size);

    void Map(DAddr address, VAddr virtual_address, size_t size, Asid asid, bool track = false);

    void Unmap(DAddr address, size_t size);

    void TrackContinuityImpl(DAddr address, VAddr virtual_address, size_t size, Asid asid);
    void TrackContinuity(DAddr address, VAddr virtual_address, size_t size, Asid asid) {
        std::scoped_lock lk(mapping_guard);
        TrackContinuityImpl(address, virtual_address, size, asid);
    }

    /// Changes whenever Map or Unmap ran: host pointers GetPointer returned before are only good
    /// while it stays the same.
    [[nodiscard]] u64 TranslationGeneration() const noexcept {
        return translation_generation.load(std::memory_order_acquire);
    }

    // Write / Read
    template <typename T>
    T* GetPointer(DAddr address);

    template <typename T>
    const T* GetPointer(DAddr address) const;

    template <typename Func>
    void ApplyOpOnPAddr(PAddr address, Common::ScratchBuffer<u32>& buffer, Func&& operation) {
        DAddr subbits = DAddr(address & page_mask);
        const u32 base = compressed_device_addr[(address >> page_bits)];
        if ((base >> MULTI_FLAG_BITS) == 0) [[likely]] {
            const DAddr d_address = (DAddr(base) << page_bits) + subbits;
            operation(d_address);
            return;
        }
        InnerGatherDeviceAddresses(buffer, address);
        for (u32 value : buffer) {
            operation((DAddr(value) << page_bits) + subbits);
        }
    }

    /// Whether p points into the emulated physical memory, the only memory with device addresses.
    bool IsPhysicalPointer(const u8* p) const {
        return (GetRawPhysicalAddr(p) >> page_bits) < compressed_device_addr.size();
    }

    template <typename Func>
    void ApplyOpOnPointer(const u8* p, Common::ScratchBuffer<u32>& buffer, Func&& operation) {
        PAddr address = GetRawPhysicalAddr<u8>(p);
        ApplyOpOnPAddr(address, buffer, operation);
    }

    PAddr GetPhysicalRawAddressFromDAddr(DAddr address) const {
        PAddr subbits = PAddr(address & page_mask);
        auto paddr = tracked_entries[(address >> page_bits)].compressed_physical_ptr;
        if (paddr == 0) {
            return 0;
        }
        return (PAddr(paddr - 1) << page_bits) + subbits;
    }

    [[nodiscard]] u64 MappingGeneration() const {
        return translation_generation.load(std::memory_order_acquire);
    }

    /// Reads only while Map/Unmap cannot change the mappings. The caller supplies one address
    /// per 4 KiB page touched, and must not acquire cache locks or wait for the GPU in copy().
    template <typename Func>
    bool WithUniquePhysicalPages(std::span<const DAddr> addresses, u64 generation, Func&& copy) {
        std::scoped_lock lk(mapping_guard);
        if (MappingGeneration() != generation || addresses.empty()) {
            return false;
        }
        for (const DAddr address : addresses) {
            const auto page = address >> page_bits;
            if (!page || page >= tracked_entries.size()) {
                return false;
            }
            // Encoded raw physical page plus one: physical page zero is a valid mapping.
            const u32 backing = tracked_entries[page].compressed_physical_ptr;
            if (!backing || backing - 1U >= compressed_device_addr.size() ||
                tracked_entries[0].compressed_physical_ptr == backing ||
                compressed_device_addr[backing - 1U] != page) {
                return false; // Unmapped, aliased, or inconsistent reverse mapping.
            }
            // A host report can memcpy an unaligned u64 across a physical page end. If our
            // bytes could include that tail, its start must also have the expected device page.
            if ((address & page_mask) < sizeof(u64) - 1 && backing > 1) {
                const u32 previous = compressed_device_addr[backing - 2U];
                if ((previous && previous != page - 1) ||
                    tracked_entries[0].compressed_physical_ptr == backing - 1U) {
                    return false;
                }
            }
        }
        copy();
        return true;
    }

    template <typename T>
    void Write(DAddr address, T value);

    template <typename T>
    T Read(DAddr address) const;

    // Completes the memcpy while Map/Unmap cannot replace the host pages. No pointer escapes.
    bool TrySnapshotContiguous(DAddr address, std::span<u8> output);
    bool TrySnapshotContiguous(DAddr address, std::span<u8> output, bool* file_read);
    u8* GetSpan(const DAddr src_addr, const std::size_t size);
    const u8* GetSpan(const DAddr src_addr, const std::size_t size) const;

    void ReadBlock(DAddr address, void* dest_pointer, size_t size,
                   std::source_location caller = std::source_location::current());
    void ReadBlockUnsafe(DAddr address, void* dest_pointer, size_t size,
                         std::source_location caller = std::source_location::current());
    void WriteBlock(DAddr address, const void* src_pointer, size_t size);
    void WriteBlockUnsafe(DAddr address, const void* src_pointer, size_t size);

    Asid RegisterProcess(Memory::Memory* memory);
    void UnregisterProcess(Asid id);

    void UpdatePagesCachedCount(DAddr addr, size_t size, s32 delta);

    // New batch API to update multiple ranges with a single lock acquisition.
    void UpdatePagesCachedBatch(std::span<const std::pair<DAddr, size_t>> ranges, s32 delta);

    /// Traps the next guest CPU read of these pages, to learn which GPU writes the CPU reads.
    void ArmReadTraps(DAddr addr, size_t size);

private:
    struct TranslationEntry {
        DAddr guest_page{};
        u8* host_ptr{};
    };

    /// A thread's last four page translations of one manager. Several threads read through
    /// them (the GPU thread, the fencing thread, service threads): one cache shared by all was
    /// cleared by Map on a service thread between the GPU thread's null check of an entry and
    /// its use, and the GPU thread copied from address 0 (title 1 crashed every few minutes).
    struct TranslationCache {
        const void* owner{};
        u64 generation{};
        std::array<TranslationEntry, 4> slots{};
        u32 cursor{};
    };
    static inline thread_local TranslationCache t_cache{};

    /// This thread's translations, emptied when Map or Unmap ran since they were made.
    TranslationCache& Translations() {
        const u64 current = translation_generation.load(std::memory_order_acquire);
        if (t_cache.owner != this || t_cache.generation != current) {
            t_cache = TranslationCache{.owner = this, .generation = current};
        }
        return t_cache;
    }

    // Internal helper that performs the update assuming the caller already holds the necessary lock.
    void UpdatePagesCachedCountNoLock(DAddr addr, size_t size, s32 delta);

private:
    static constexpr size_t device_virtual_bits = Traits::device_virtual_bits;
    static constexpr size_t device_as_size = 1ULL << device_virtual_bits;
    static constexpr size_t physical_min_bits = 32;
    static constexpr size_t physical_max_bits = 35; // covers the 12 GiB layout plus nx2 graphics server memory
    static constexpr size_t page_bits = 12;
    static constexpr size_t page_size = 1ULL << page_bits;
    static constexpr size_t page_mask = page_size - 1ULL;
    static constexpr u32 physical_address_base = 1U << page_bits;
    static constexpr u32 MULTI_FLAG_BITS = 31;
    static constexpr u32 MULTI_FLAG = 1U << MULTI_FLAG_BITS;
    static constexpr u32 MULTI_MASK = ~MULTI_FLAG;

    template <typename T>
    T* GetPointerFromRaw(PAddr addr) {
        return reinterpret_cast<T*>(physical_base + addr);
    }

    template <typename T>
    const T* GetPointerFromRaw(PAddr addr) const {
        return reinterpret_cast<T*>(physical_base + addr);
    }

    template <typename T>
    PAddr GetRawPhysicalAddr(const T* ptr) const {
        return static_cast<PAddr>(reinterpret_cast<uintptr_t>(ptr) - physical_base);
    }

    void WalkBlock(const DAddr addr, const std::size_t size, auto on_unmapped, auto on_memory,
                   auto increment);

    void CutContinuityAt(size_t page);

    void InnerGatherDeviceAddresses(Common::ScratchBuffer<u32>& buffer, PAddr address);

    std::unique_ptr<DeviceMemoryManagerAllocator<Traits>> impl;

    const uintptr_t physical_base;
    const DeviceMemory* device_memory_ptr;
    DeviceInterface* device_inter;
    std::unique_ptr<Common::ThreadWorker> cold_hint_worker;

    struct TrackedEntry {
        VAddr cpu_backing_address;
        u32 continuity_tracker;
        u32 compressed_physical_ptr;
    };
    Common::SparseLargeVector<u32> compressed_device_addr;
    Common::SparseLargeVector<TrackedEntry> tracked_entries;

    // Process memory interfaces

    std::deque<size_t> id_pool;
    std::deque<Memory::Memory*> registered_processes;

    // Memory protection management

    static constexpr size_t guest_max_as_bits = 39;
    static constexpr size_t guest_as_size = 1ULL << guest_max_as_bits;
    static constexpr size_t guest_mask = guest_as_size - 1ULL;
    static constexpr size_t asid_start_bit = guest_max_as_bits;

    std::pair<Asid, VAddr> ExtractCPUBacking(size_t page_index) {
        auto content = tracked_entries[page_index].cpu_backing_address;
        const VAddr address = content & guest_mask;
        const Asid asid{static_cast<size_t>(content >> asid_start_bit)};
        return std::make_pair(asid, address);
    }

    constexpr void InsertCPUBacking(size_t page_index, VAddr address, Asid asid) {
        tracked_entries.GetUnchecked(page_index).cpu_backing_address = address | (asid.id << asid_start_bit);
    }

    /// Generations come from one counter shared by every manager. A thread's translations name
    /// their manager by address, and a manager made where a destroyed one was (a test fixture
    /// on the stack, a System made again in the same process) would otherwise start with the
    /// same generation and take the old manager's host pointers for its own.
    static u64 NextTranslationGeneration() {
        static std::atomic<u64> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }
    EarlyReadReportPages early_read_reports;
    std::atomic<u64> translation_generation{NextTranslationGeneration()}; ///< New at Map and Unmap
    using CounterType = u8;
    using CounterAtomicType = std::atomic_uint8_t;
    static constexpr size_t subentries = 8 / sizeof(CounterType);
    static constexpr size_t subentries_mask = subentries - 1;
    static constexpr size_t subentries_shift =
        std::countr_zero(sizeof(u64)) - std::countr_zero(sizeof(CounterType));
    class CounterEntry final {
    public:
        CounterEntry() = default;

        CounterAtomicType& Count(std::size_t page) {
            return values[page & subentries_mask];
        }

        const CounterAtomicType& Count(std::size_t page) const {
            return values[page & subentries_mask];
        }

    private:
        std::array<CounterAtomicType, subentries> values{};
    };
    static_assert(sizeof(CounterEntry) == subentries * sizeof(CounterType),
                  "CounterEntry should be 8 bytes!");

    static constexpr size_t num_counter_entries =
        (1ULL << (device_virtual_bits - page_bits)) / subentries;
    using CachedPages = std::array<CounterEntry, num_counter_entries>;
    std::unique_ptr<CachedPages> cached_pages;
    Common::RangeMutex counter_guard;
    std::mutex mapping_guard;


};

} // namespace Core
