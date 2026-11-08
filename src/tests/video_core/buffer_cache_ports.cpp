// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <barrier>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "common/page_table.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/gpu_dirty_memory_manager.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/control/channel_state.h"
#include "video_core/gpu.h"
#include "video_core/texture_cache/atlas_upload_evidence.h"
#include "video_core/texture_cache/util.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace {

struct Runtime;

struct Buffer : VideoCommon::BufferBase {
    Buffer(Runtime&, VideoCommon::NullBufferParams params) : BufferBase{params} {}
    Buffer(Runtime&, DAddr address, u32 size, bool mapped)
        : BufferBase{address, size}, bytes(size), host_mapped{mapped} {}

    void MarkUsage(u64 offset, u64 size) {
        REQUIRE(offset + size <= bytes.size());
    }
    std::span<u8> Mapped() {
        return host_mapped ? std::span{bytes} : std::span<u8>{};
    }
    u64 LastUsageTick() const {
        return tick;
    }

    std::vector<u8> bytes;
    bool host_mapped{};
    u64 tick{};
};

struct Staging {
    Buffer* buffer;
    u64 offset;
    std::span<u8> mapped_span;
    bool host_coherent{true};
};

struct Runtime {
    bool CanReportMemoryUsage() const {
        return false;
    }
    u64 GetDeviceLocalMemory() const {
        return 0;
    }
    u64 GetMaxStorageBufferRange() const {
        return 1ULL << 30;
    }
    u32 GetStorageBufferAlignment() const {
        return alignment;
    }
    u32 GetUniformBufferAlignment() const {
        return 1;
    }
    bool IsFree(u64) const {
        return true;
    }
    void WaitSubmittedTick(u64) {}
    bool CanReorderUpload(const Buffer&, std::span<VideoCommon::BufferCopy>) const {
        return false;
    }
    u64 CurrentTick() const {
        return current_tick;
    }
    u64 QueueEarlyHostRead() {
        return current_tick;
    }
    bool EarlyHostReadReady(u64 tick) {
        return tick <= completed_tick;
    }
    u64 CompletedEarlyReadTick() {
        return completed_tick;
    }
    bool EarlyTickSubmitted(u64 tick) const {
        return tick <= completed_tick;
    }
    std::optional<Staging> TryEarlyStaging(size_t bytes) {
        if (fail_loan)
            return std::nullopt;
        return DownloadStagingBuffer(bytes, true);
    }
    bool InvalidateEarlyHostRead(Staging& ref) {
        if (fail_invalidate)
            return false;
        if (!ref.host_coherent) {
            std::copy_n(ref.buffer->bytes.data() + ref.offset, ref.mapped_span.size(),
                        ref.mapped_span.data());
            ++early_invalidations;
        }
        return true;
    }
    void ReleaseEarlyHostRead(Staging& ref, u64 tick) {
        last_release_tick = tick;
        FreeDeferredStagingBuffer(ref);
    }
    void CompleteGpu() {
        for (auto& copy : pending)
            copy();
        pending.clear();
        completed_tick = current_tick++;
    }
    void FreeDeferredStagingBuffer(Staging& ref) {
        // The pool retains released Vulkan resources through their submission's tick.
        REQUIRE(std::any_of(deferred.begin(), deferred.end(),
                            [&](const auto& p) { return p.get() == ref.buffer; }));
        ++deferred_releases;
    }
    Staging DownloadStagingBuffer(u64 size, bool keep = false) {
        if (keep) {
            auto& loan =
                deferred.emplace_back(std::make_unique<Buffer>(*this, 0, size + 128, false));
            if (noncoherent_early) {
                auto& cpu = noncoherent_views.emplace_back(size, 0xff);
                return {loan.get(), 128, cpu, false};
            }
            return {loan.get(), 128, std::span{loan->bytes}.subspan(128)};
        }
        staging = std::make_unique<Buffer>(*this, 0, size + 128, false);
        ++allocations;
        return {staging.get(), 128, std::span{staging->bytes}.subspan(128), !noncoherent_reference};
    }
    Staging UploadStagingBuffer(u64 size) {
        return DownloadStagingBuffer(size);
    }
    void CopyBuffer(Buffer* dst, Buffer& src, std::span<const VideoCommon::BufferCopy> copies,
                    bool) {
        for (const auto& copy : copies) {
            REQUIRE(copy.src_offset + copy.size <= src.bytes.size());
            REQUIRE(copy.dst_offset + copy.size <= dst->bytes.size());
            pending.push_back([dst, &src, copy] {
                std::memcpy(dst->bytes.data() + copy.dst_offset, src.bytes.data() + copy.src_offset,
                            copy.size);
            });
            ++downloads;
        }
    }
    void CopyBuffer(Buffer& dst, Buffer* src, std::span<const VideoCommon::BufferCopy> copies, bool,
                    bool) {
        for (const auto& copy : copies) {
            REQUIRE(copy.src_offset + copy.size <= src->bytes.size());
            REQUIRE(copy.dst_offset + copy.size <= dst.bytes.size());
            std::memcpy(dst.bytes.data() + copy.dst_offset, src->bytes.data() + copy.src_offset,
                        copy.size);
            ++uploads;
            upload_bytes += copy.size;
        }
    }
    void Finish() {
        if (before_completion) {
            before_completion();
        }
        CompleteGpu();
        ++finishes;
    }
    void Wait(u64 tick) {
        if (before_completion) {
            before_completion();
        }
        waited_tick = tick;
        ++waits;
    }
    void FinishForHostRead() {
        ++host_read_barriers;
        Finish();
    }
    void BindUniformBuffer(Buffer&, u32, u32) {}
    std::span<u8> BindMappedUniformBuffer(size_t, u32, u32 size) {
        stream.resize(size);
        return stream;
    }

    u64 current_tick{1}, completed_tick{}, deferred_releases{}, last_release_tick{},
        early_invalidations{};
    bool noncoherent_early{}, noncoherent_reference{}, fail_loan{}, fail_invalidate{};
    std::vector<std::vector<u8>> noncoherent_views;
    std::vector<std::unique_ptr<Buffer>> deferred;
    u32 alignment{256};
    u32 allocations{}, downloads{}, uploads{}, finishes{}, waits{};
    u32 host_read_barriers{};
    u64 upload_bytes{}, waited_tick{};
    std::unique_ptr<Buffer> staging;
    std::vector<u8> stream;
    std::vector<std::function<void()>> pending;
    std::function<void()> before_completion;
};

struct Tracker : VideoCommon::MemoryTrackerBase<Tegra::MaxwellDeviceMemoryManager> {
    using MemoryTrackerBase::MemoryTrackerBase;
    void MarkRegionAsCpuModified(DAddr address, u64 size) {
        ++cpu_marks;
        MemoryTrackerBase::MarkRegionAsCpuModified(address, size);
    }
    u32 cpu_marks{};
};

struct Policy {
    using Runtime = ::Runtime;
    using Buffer = ::Buffer;
    using Async_Buffer = Staging;
    using MemoryTracker = Tracker;
    static constexpr bool IS_OPENGL = false;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS = false;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT = false;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = false;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = false;
    static constexpr bool USE_MEMORY_MAPS = true;
    static constexpr bool SEPARATE_IMAGE_BUFFER_BINDINGS = false;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = true;
};

using Cache = VideoCommon::BufferCache<Policy>;

} // namespace

namespace VideoCommon {

struct BufferCacheTestAccess {
    static void EarlyMode(Cache& cache, bool verify = false) {
        cache.early_reads = std::make_unique<Cache::Early>("/unused-test-marker", verify);
        cache.early_reads->active = true;
    }
    static auto& EarlyState(Cache& cache) {
        return *cache.early_reads;
    }
    static void StorageWrite(Cache& cache, BufferId buffer, DAddr addr, u64 size, bool compute) {
        cache.MarkWrittenBuffer(buffer, addr, static_cast<u32>(size), compute);
    }

    static void DemandBatch(Cache& cache, u64 frame, bool bounded) {
        cache.frame_tick = frame;
        cache.read_batch_baseline_file = "test";
        cache.bounded_read_batch = bounded;
    }

    static Binding Bind(Cache& cache, Tegra::MemoryManager& memory, GPUVAddr descriptor,
                        bool written = false, bool native = false, bool table = false) {
        cache.gpu_memory = &memory;
        return cache.StorageBufferBinding(descriptor, 0, written, native, table);
    }
    static BufferId Add(Cache& cache, DAddr address, u32 size, bool mapped = false) {
        const auto id = cache.slot_buffers.insert(cache.runtime, address, size, mapped);
        cache.Register(id);
        cache.memory_tracker.ForEachUploadRange(address, size, [](auto, auto) {});
        return id;
    }
    static Buffer& Get(Cache& cache, BufferId id) {
        return cache.slot_buffers[id];
    }
    static void Dirty(Cache& cache, DAddr address, u64 size) {
        cache.memory_tracker.MarkRegionAsGpuModified(address, size);
        cache.gpu_modified_ranges.Add(address, size);
        cache.uncommitted_gpu_modified_ranges.Add(address, size);
        cache.committed_gpu_modified_ranges.emplace_back().Add(address, size);
        cache.pending_download_ranges.emplace_back().Add(address, size);
    }
    static bool CoarseGpuDirty(Cache& cache, DAddr address) {
        return cache.memory_tracker.IsRegionGpuModified(address, 1);
    }
    static bool Pending(Cache& cache, DAddr address, u64 size) {
        if (cache.gpu_modified_ranges.Intersects(address, size) ||
            cache.uncommitted_gpu_modified_ranges.Intersects(address, size)) {
            return true;
        }
        for (const auto& ranges : cache.committed_gpu_modified_ranges) {
            if (ranges.Intersects(address, size))
                return true;
        }
        for (const auto& ranges : cache.pending_download_ranges) {
            if (ranges.Intersects(address, size))
                return true;
        }
        return false;
    }
    static void DrawWrite(Cache& cache, DAddr page) {
        std::scoped_lock lock{cache.nx2_list_mutex};
        auto& entry = cache.nx2_list_pages[page];
        entry.draw_written = true;
        ++entry.writes;
    }
    static void Download(Cache& cache, std::span<const std::pair<DAddr, u64>> ranges) {
        cache.DownloadRangesMemory(ranges);
    }
    static void Uniform(Cache& cache, BufferCacheChannelInfo& channel, BufferId id, DAddr address,
                        u32 size) {
        cache.channel_state = &channel;
        cache.maxwell3d = &channel.maxwell3d;
        cache.gpu_memory = &channel.gpu_memory;
        channel.uniform_buffers[0][0] = {.device_addr = address, .size = size, .buffer_id = id};
        cache.BindHostGraphicsUniformBuffer(0, 0, 0, true);
    }
    static void NextFrame(Cache& cache) {
        cache.native_indirect_uniforms_this_frame.clear();
    }
    static u32 CpuMarks(Cache& cache) {
        return cache.memory_tracker.cpu_marks;
    }
};

} // namespace VideoCommon

namespace {

using Access = VideoCommon::BufferCacheTestAccess;

struct Fixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device{physical};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    std::optional<Tegra::MemoryManager> memory;
    Runtime runtime;
    std::unique_ptr<Cache> cache;
    std::unique_ptr<Kernel::KProcess> process;
    Core::Asid asid{};
    u8* bytes{};

    Fixture() {
        system.Initialize();
        process = std::make_unique<Kernel::KProcess>(system.Kernel());
        auto& table = process->GetPageTable().GetImpl();
        table.Resize(24, 12);
        bytes = physical.GetPointerFromRaw<u8>(0x10000);
        std::memset(bytes, 0, 0x40000);
        for (u64 page = 0; page < 64; ++page) {
            table.entries.GetAndFault(0x10 + page)
                .Store(false, Common::PageType::Memory, 1,
                       reinterpret_cast<uintptr_t>(bytes) - 0x10000);
        }
        process->GetMemory().SetCurrentPageTable(*process);
        asid = device.RegisterProcess(&process->GetMemory());
        device.BindInterface(&rasterizer);
        device.Map(0x10000, 0x10000, 0x40000, asid);
        ResetMemory();
        cache = std::make_unique<Cache>(device, runtime);
    }
    ~Fixture() {
        cache.reset();
        memory.reset();
        device.Unmap(0x10000, 0x40000);
        device.UnregisterProcess(asid);
        system.Kernel().UnregisterKernelObject(process.get());
    }
    void ResetMemory() {
        memory.reset();
        memory.emplace(system, device, 32, 1ULL << 31, 16);
        memory->BindRasterizer(&rasterizer);
        memory->Map(0x100000, 0x10000, 0x10000, Tegra::PTEKind::PITCH, true);
    }
    void Descriptor(GPUVAddr address, u64 size) {
        std::memcpy(bytes, &address, sizeof(address));
        std::memcpy(bytes + 8, &size, sizeof(size));
    }
    VideoCommon::Binding Bind(bool written = false, bool native = false, bool table = false) {
        return Access::Bind(*cache, *memory, 0x100000, written, native, table);
    }
};

} // namespace

TEST_CASE("Storage binding cache tracks map changes and descriptor dependencies",
          "[buffer_ports]") {
    Fixture f;
    constexpr GPUVAddr base = 0x200000;
    f.memory->Map(base, 0x20000, 0x10000, Tegra::PTEKind::PITCH, true);
    f.Descriptor(base + 0x123, 0x456);
    for (int i = 0; i < 3; ++i) {
        const auto binding = f.Bind(true);
        REQUIRE(binding.device_addr == 0x20100);
        REQUIRE(binding.gpu_addr == base + 0x100);
        REQUIRE(binding.size == 0x479);
        REQUIRE(binding.buffer_id == VideoCommon::BufferId{});
        REQUIRE(binding.segment_count == 0);
    }
    REQUIRE(f.Bind().size == 0xf00);
    f.Descriptor(base + 0x123, 0x1456);
    REQUIRE(f.Bind(true).size == 0x1479);
    f.runtime.alignment = 4096;
    REQUIRE(f.Bind(true).device_addr == 0x20000);
    REQUIRE(f.Bind(true).size == 0x1579);
    REQUIRE(f.Bind(true, true).size == 0x10000);
    f.memory->Map(base, 0x30000, 0x10000, Tegra::PTEKind::PITCH, true);
    REQUIRE(f.Bind(true).device_addr == 0x30000);
    f.memory->MapSparse(base, 0x10000, true);
    REQUIRE(f.Bind().size == 0);
    f.memory->Unmap(base, 0x10000);
    REQUIRE(f.Bind().size == 0);
    f.memory->Map(base, 0x20000, 0x2000, Tegra::PTEKind::PITCH, false);
    REQUIRE(f.Bind(true, true).size == 0x2000);
    f.memory->Map(base + 0x1000, 0x40000, 0x1000, Tegra::PTEKind::PITCH, false);
    REQUIRE(f.Bind(true, true).size == 0x1000);
    const auto old_owner = f.memory->GetID();
    f.ResetMemory();
    f.memory->Map(base, 0x40000, 0x10000, Tegra::PTEKind::PITCH, true);
    REQUIRE(f.memory->GetID() != old_owner);
    REQUIRE(f.Bind(true).device_addr == 0x40000);
    f.Descriptor(0, 0x123);
    REQUIRE(f.Bind().size == 0);
}

TEST_CASE("Bulk texture snapshots use current contiguous GPU and physical mappings",
          "[atlas_snapshot]") {
    Fixture f;
    constexpr GPUVAddr va = 0x180000;
    f.memory->Map(va, 0x20000, 0x3000, Tegra::PTEKind::PITCH, false);
    for (size_t i = 0; i < 0x40000; ++i) {
        f.bytes[i] = static_cast<u8>((i * 37) ^ (i >> 12));
    }
    std::array<u8, 0x2200> dest{}, expected{};
    const auto compare = [&](bool should_copy) {
        dest.fill(0xde);
        const auto previous = dest;
        bool read_file = true;
        const bool copied = f.memory->TrySnapshotTextureRun(va + 0x100, dest, &read_file);
#ifdef __linux__
        REQUIRE(read_file == should_copy);
#else
        REQUIRE_FALSE(read_file);
#endif
        REQUIRE(copied == should_copy);
        f.memory->ReadBlockUnsafe(va + 0x100, expected.data(), expected.size());
        if (copied) {
            REQUIRE(dest == expected);
        } else {
            REQUIRE(dest == previous);
        }
    };
    compare(true); // fixture's device mapping defaults to track=false
    f.device.Unmap(0x21000, 0x1000);
    compare(false); // reject interior physical hole; scalar reference supplies zero
    REQUIRE(expected[0xf00] == 0);
    f.device.Map(0x21000, 0x20000, 0x1000, f.asid);
    compare(false); // middle page aliases the first physical page
    // Heap preallocation may describe contiguous guest virtual memory even though the
    // installed DAddr mappings above currently repeat one physical page.
    f.device.TrackContinuity(0x20000, 0x20000, 0x3000, f.asid);
    compare(false); // A continuity hint must never substitute for installed physical PTEs.
    REQUIRE(expected[0x0f00] == f.bytes[0x10000]);
    REQUIRE(expected[0x0f00] != f.bytes[0x11000]);
    f.device.Unmap(0x21000, 0x1000);
    f.device.Map(0x21000, 0x21000, 0x1000, f.asid);
    compare(true);
    f.memory->Map(va + 0x1000, 0x30000, 0x1000, Tegra::PTEKind::PITCH, false);
    compare(false); // GPU virtual pages no longer consecutive in device space
    f.memory->MapSparse(va + 0x1000, 0x1000, false);
    compare(false);
    f.memory->Map(va, 0x30000, 0x3000, Tegra::PTEKind::PITCH, false);
    compare(true); // address reused, new backing
    REQUIRE(dest[0] == f.bytes[0x20100]);
    std::array<u8, 2> small{};
    REQUIRE_FALSE(f.memory->TrySnapshotTextureRun(~GPUVAddr{0}, small));
    REQUIRE_FALSE(f.memory->TrySnapshotTextureRun(va, std::span<u8>{}));
    std::vector<u8> over_limit(4 * 1024 * 1024 + 1);
    REQUIRE_FALSE(f.memory->TrySnapshotTextureRun(va, over_limit));
}

TEST_CASE("An atlas snapshot declines live GMMU PTE updates before run publication",
          "[atlas_snapshot][atlas_mapping_writer]") {
    Fixture f;
    const bool big = GENERATE(false, true);
    constexpr GPUVAddr va = 0x180000;
    const size_t bytes = big ? 0x10000 : 0x1000;
    f.memory->Map(va, 0x20000, bytes, Tegra::PTEKind::PITCH, big);
    f.bytes[0x10000] = 0xa3;
    f.bytes[0x20000] = 0x56;
    std::array<u8, 16> dest{}, reference{};
    REQUIRE(f.memory->TrySnapshotTextureRun(va, dest));
    REQUIRE(dest.front() == 0xa3);

    class PausedMapper : public Null::RasterizerNull {
    public:
        using Null::RasterizerNull::RasterizerNull;
        void ModifyGPUMemory(size_t, GPUVAddr, u64) override {
            std::unique_lock lock{mutex};
            entered = true;
            cv.notify_all();
            timed_out = !cv.wait_for(lock, std::chrono::seconds{5}, [&] { return resume; });
        }
        std::mutex mutex;
        std::condition_variable cv;
        bool entered{}, resume{}, timed_out{};
    } callback{f.gpu};

    f.memory->BindRasterizer(&callback);
    SCOPE_EXIT { f.memory->BindRasterizer(&f.rasterizer); };
    std::thread mapper{[&] { f.memory->MapSparse(va, bytes, big); }};
    bool entered;
    {
        std::unique_lock lock{callback.mutex};
        entered = callback.cv.wait_for(lock, std::chrono::seconds{5}, [&] { return callback.entered; });
    }
    dest.fill(0xfe);
    // Callback is after SetEntry(Reserved) and before UpdateContinuousRuns. The original
    // walker must now see zeros; the optimization may not copy the old physical page.
    bool copied = false;
    if (entered) {
        bool read_file = true;
        copied = f.memory->TrySnapshotTextureRun(va, dest, &read_file);
        if (read_file) copied = true; // A rejected writer must not enter the backing file path.
        f.memory->ReadBlockUnsafe(va, reference.data(), reference.size());
    }
    {
        std::scoped_lock lock{callback.mutex};
        callback.resume = true;
        callback.cv.notify_all();
    }
    mapper.join();
    REQUIRE(entered);
    REQUIRE_FALSE(callback.timed_out); // Reader did not wait for the callback to return.
    REQUIRE_FALSE(copied);
    REQUIRE(std::ranges::all_of(dest, [](u8 x) { return x == 0xfe; }));
    REQUIRE(std::ranges::all_of(reference, [](u8 x) { return x == 0; }));
    REQUIRE_FALSE(f.memory->TrySnapshotTextureRun(va, dest));
    f.memory->BindRasterizer(&f.rasterizer);
    f.memory->Map(va, 0x30000, bytes, Tegra::PTEKind::PITCH, big);
    REQUIRE(f.memory->TrySnapshotTextureRun(va, dest));
    REQUIRE(dest.front() == 0x56);
}

TEST_CASE("Storage binding cache preserves native and table size policies", "[buffer_ports]") {
    Fixture f;
    constexpr GPUVAddr base = 0x4000000;
    f.memory->Map(base, 0x10000000, 80 << 20, Tegra::PTEKind::PITCH, true);
    f.Descriptor(base, 256);
    REQUIRE(f.Bind().size == 4096);
    REQUIRE(f.Bind(false, true).size == 8 << 20);
    REQUIRE(f.Bind(false, false, true).size == 80 << 20);
    REQUIRE(f.Bind(true, false, true).size == 256);
    f.Descriptor(base, (1ULL << 32) | 256);
    REQUIRE(f.Bind().size == 8 << 20);
    REQUIRE(f.Bind(false, false, true).size == 80 << 20);
    f.Descriptor(base + (79 << 20), 0);
    REQUIRE(f.Bind(false, false, true).size == 1 << 20);
}

TEST_CASE("Storage binding cache does not reuse a destroyed address space", "[buffer_ports]") {
    Fixture f;
    f.memory->Map(0x200000, 0x20000, 0x10000, Tegra::PTEKind::PITCH, true);
    f.Descriptor(0x200000, 0x1000);
    REQUIRE(f.Bind(true).device_addr == 0x20000);
    const auto serial = f.memory->MapSerial();
    const auto owner = f.memory->GetID();
    const auto* address = &*f.memory;
    f.ResetMemory();
    f.memory->Map(0x200000, 0x30000, 0x10000, Tegra::PTEKind::PITCH, true);
    REQUIRE(&*f.memory == address);
    REQUIRE(f.memory->MapSerial() == serial);
    REQUIRE(f.memory->GetID() != owner);
    REQUIRE(f.Bind(true).device_addr == 0x30000);
}

TEST_CASE("Batch downloads clamp buffers, deduplicate pages and finish before publication",
          "[buffer_ports]") {
    Fixture f;
    const auto first = Access::Add(*f.cache, 0x20000, 0x10000);
    const auto second = Access::Add(*f.cache, 0x30000, 0x10000);
    std::ranges::fill(Access::Get(*f.cache, first).bytes, 0x12);
    std::ranges::fill(Access::Get(*f.cache, second).bytes, 0x34);
    Access::Dirty(*f.cache, 0x2f000, 0x2000);
    Access::Dirty(*f.cache, 0x32000, 0x1000);
    f.runtime.before_completion = [&] {
        REQUIRE(f.bytes[0x1f000] == 0);
        REQUIRE(f.bytes[0x20000] == 0);
    };
    const std::array<std::pair<DAddr, u64>, 3> ranges{{
        {0x2f008, 0x1000},
        {0x2f000, 0x2000},
        {0x20000, 0},
    }};
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.allocations == 1);
    REQUIRE(f.runtime.downloads == 2);
    REQUIRE(f.bytes[0x1f000] == 0x12);
    REQUIRE(f.bytes[0x1ffff] == 0x12);
    REQUIRE(f.bytes[0x20000] == 0x34);
    REQUIRE(f.bytes[0x20fff] == 0x34);
    REQUIRE(f.bytes[0x22000] == 0);
    REQUIRE_FALSE(Access::Pending(*f.cache, 0x2f000, 0x2000));
    REQUIRE(Access::Pending(*f.cache, 0x32000, 0x1000));
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.finishes == 1);
}

TEST_CASE("Mapped and mixed batches make device writes host visible in one completion",
          "[buffer_ports]") {
    Fixture f;
    const auto first = Access::Add(*f.cache, 0x20000, 0x10000, true);
    const auto second = Access::Add(*f.cache, 0x30000, 0x10000, true);
    const auto clean = Access::Add(*f.cache, 0x40000, 0x10000, true);
    auto& a = Access::Get(*f.cache, first);
    auto& b = Access::Get(*f.cache, second);
    a.tick = 8;
    b.tick = 19;
    Access::Get(*f.cache, clean).tick = 99;
    const std::array<std::pair<DAddr, u64>, 3> ranges{{
        {0x20000, 0x1000},
        {0x30000, 0x1000},
        {0x40000, 0x1000},
    }};
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    Access::Dirty(*f.cache, 0x30000, 0x1000);
    f.runtime.before_completion = [&] {
        REQUIRE(f.runtime.host_read_barriers == 1);
        REQUIRE(f.bytes[0x10000] == 0);
        REQUIRE(f.bytes[0x20000] == 0);
        std::ranges::fill(a.bytes, 0x56);
        std::ranges::fill(b.bytes, 0x78);
    };
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.waits == 0);
    REQUIRE(f.runtime.host_read_barriers == 1);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.allocations == 0);
    REQUIRE(f.bytes[0x10000] == 0x56);
    REQUIRE(f.bytes[0x20000] == 0x78);
    b.host_mapped = false;
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    Access::Dirty(*f.cache, 0x30000, 0x1000);
    f.runtime.before_completion = [&] {
        REQUIRE(f.runtime.host_read_barriers == 2);
        REQUIRE(f.bytes[0x10000] == 0x56);
        REQUIRE(f.bytes[0x20000] == 0x78);
        std::ranges::fill(a.bytes, 0x9a);
        std::ranges::fill(b.bytes, 0xbc);
    };
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.waits == 0);
    REQUIRE(f.runtime.finishes == 2);
    REQUIRE(f.runtime.downloads == 1);
    REQUIRE(f.bytes[0x10000] == 0x9a);
    REQUIRE(f.bytes[0x20000] == 0xbc);
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.host_read_barriers == 2);
    REQUIRE(f.runtime.finishes == 2);
}

TEST_CASE("Single mapped download completes host visibility before guest publication",
          "[buffer_ports]") {
    Fixture f;
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000, true);
    auto& buffer = Access::Get(*f.cache, id);
    buffer.tick = 7;
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    f.runtime.before_completion = [&] {
        REQUIRE(f.runtime.host_read_barriers == 1);
        REQUIRE(f.bytes[0x10000] == 0);
        std::ranges::fill(buffer.bytes, 0xcd);
    };
    f.cache->DownloadMemory(0x20000, 0x1000);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.waits == 0);
    REQUIRE(f.runtime.allocations == 0);
    REQUIRE(f.bytes[0x10000] == 0xcd);
    f.cache->DownloadMemory(0x20000, 0x1000);
    REQUIRE(f.runtime.finishes == 1);
}

TEST_CASE("Command page batch publishes only completed matching write generations",
          "[buffer_ports]") {
    Fixture f;
    const bool mapped = GENERATE(false, true);
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000, mapped);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0x42);
    Access::Dirty(*f.cache, 0x20000, 0x3000);
    Access::DrawWrite(*f.cache, 0x20000);
    Access::DrawWrite(*f.cache, 0x21000);
    Access::DrawWrite(*f.cache, 0x22000);
    std::array<u8, 4> out{};
    using Read = Cache::Nx2ListRead;
    f.runtime.before_completion = [&] {
        REQUIRE(f.runtime.host_read_barriers == 1);
        REQUIRE(f.bytes[0x10000] == 0);
        REQUIRE(f.cache->Nx2ReadListCopy(0x20000, out, true) == Read::Unknown);
        Access::DrawWrite(*f.cache, 0x22000);
    };
    const std::array<DAddr, 1> requested{0x20000};
    f.cache->Nx2FlushListPages(requested);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.downloads == (mapped ? 0U : 3U));
    REQUIRE(f.cache->Nx2ReadListCopy(0x20000, out, true) == Read::Synced);
    REQUIRE(f.cache->Nx2ReadListCopy(0x21000, out, true) == Read::Synced);
    REQUIRE(f.cache->Nx2ReadListCopy(0x22000, out, true) == Read::Unknown);
    REQUIRE(f.bytes[0x12000] == 0x42);
    REQUIRE(f.cache->Nx2ReadListCopy(0x21000, out, false) == Read::NeedsSync);
    Access::DrawWrite(*f.cache, 0x21000);
    REQUIRE(f.cache->Nx2ReadListCopy(0x21000, out, true) == Read::Unknown);
}

TEST_CASE("Command page readers remain ordered during batch completion", "[buffer_ports]") {
    struct {
        Core::DeviceMemory physical;
        Tegra::MaxwellDeviceMemoryManager device{physical};
        Runtime runtime;
        std::unique_ptr<Cache> cache = std::make_unique<Cache>(device, runtime);
    } f;
    Access::Add(*f.cache, 0x20000, 0x10000);
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    Access::DrawWrite(*f.cache, 0x20000);
    std::barrier rendezvous{2};
    Cache::Nx2ListRead before{}, after{};
    std::jthread reader{[&] {
        std::array<u8, 4> out{};
        rendezvous.arrive_and_wait();
        before = f.cache->Nx2ReadListCopy(0x20000, out, true);
        rendezvous.arrive_and_wait();
        rendezvous.arrive_and_wait();
        after = f.cache->Nx2ReadListCopy(0x20000, out, true);
    }};
    f.runtime.before_completion = [&] {
        rendezvous.arrive_and_wait();
        rendezvous.arrive_and_wait();
    };
    const std::array<DAddr, 1> requested{0x20000};
    f.cache->Nx2FlushListPages(requested);
    rendezvous.arrive_and_wait();
    reader.join();
    REQUIRE(before == Cache::Nx2ListRead::Unknown);
    REQUIRE(after == Cache::Nx2ListRead::Synced);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.downloads == 1);
}

TEST_CASE("Native uniform banks upload directly without overwriting GPU-owned bytes",
          "[buffer_ports]") {
    Fixture f;
    f.memory->EnableNativeTiling();
    Tegra::Control::ChannelState state{0};
    state.memory_manager = &*f.memory;
    state.payload.emplace(f.system, *f.memory, state);
    VideoCommon::BufferCacheChannelInfo channel{state};
    VideoCommon::UniformBufferSizes sizes{};
    sizes[0][0] = 0x10000;
    channel.uniform_buffer_sizes = &sizes;
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    std::memset(f.bytes + 0x10000, 0x5a, 0x10000);
    Access::Uniform(*f.cache, channel, id, 0x20000, 0x10000);
    REQUIRE(f.runtime.upload_bytes == 0x10000);
    REQUIRE(Access::CpuMarks(*f.cache) == 0);
    REQUIRE(Access::Get(*f.cache, id).bytes[0xffff] == 0x5a);
    Access::Uniform(*f.cache, channel, id, 0x20000, 0x10000);
    REQUIRE(f.runtime.uploads == 1);
    Access::NextFrame(*f.cache);
    Access::Dirty(*f.cache, 0x21000, 0x1000);
    std::memset(f.bytes + 0x10000, 0xa5, 0x10000);
    Access::Uniform(*f.cache, channel, id, 0x20000, 0x10000);
    REQUIRE(f.runtime.uploads == 1);
    REQUIRE(Access::Get(*f.cache, id).bytes[0x1000] == 0x5a);
}

TEST_CASE("Native uniform uploads stop at holes and keep unaligned bank boundaries",
          "[buffer_ports]") {
    Fixture f;
    f.memory->EnableNativeTiling();
    Tegra::Control::ChannelState state{0};
    state.memory_manager = &*f.memory;
    state.payload.emplace(f.system, *f.memory, state);
    VideoCommon::BufferCacheChannelInfo channel{state};
    VideoCommon::UniformBufferSizes sizes{};
    sizes[0][0] = 0x10000;
    channel.uniform_buffer_sizes = &sizes;
    const auto id = Access::Add(*f.cache, 0x20000, 0x20000);
    std::memset(f.bytes + 0x10000, 0x6b, 0x20000);
    f.device.Unmap(0x22000, 0x1000);
    Access::Uniform(*f.cache, channel, id, 0x20123, 0x10000);
    REQUIRE(f.runtime.upload_bytes == 0x1edd);
    const auto& data = Access::Get(*f.cache, id).bytes;
    REQUIRE(data[0x122] == 0);
    REQUIRE(data[0x123] == 0x6b);
    REQUIRE(data[0x1fff] == 0x6b);
    REQUIRE(data[0x2000] == 0);
    REQUIRE(data[0x3000] == 0);
    REQUIRE(Access::CpuMarks(*f.cache) == 0);
    f.device.Map(0x22000, 0x22000, 0x1000, f.asid);
    Access::NextFrame(*f.cache);
    Access::Uniform(*f.cache, channel, id, 0x20123, 0x10000);
    REQUIRE(f.runtime.upload_bytes == 0x11edd);
    REQUIRE(data[0x10122] == 0x6b);
    REQUIRE(data[0x10123] == 0);
}

TEST_CASE("Downloads keep CPU writes collected and not yet applied", "[buffer_ports]") {
    Fixture f;
    const auto staged = Access::Add(*f.cache, 0x20000, 0x10000);
    const auto mapped = Access::Add(*f.cache, 0x30000, 0x10000, true);
    std::ranges::fill(Access::Get(*f.cache, staged).bytes, 0x12);
    std::ranges::fill(Access::Get(*f.cache, mapped).bytes, 0x34);
    f.cache->SetPendingCpuWrites([](const std::function<void(DAddr, size_t)>& callback) {
        callback(0x20100, 0x10);
        callback(0x30ff8, 0x10);
    });
    std::memset(f.bytes + 0x10100, 0xee, 0x10);
    std::memset(f.bytes + 0x20ff8, 0xee, 0x10);
    Access::Dirty(*f.cache, 0x20000, 0x2000);
    Access::Dirty(*f.cache, 0x30000, 0x2000);
    const std::array<std::pair<DAddr, u64>, 1> ranges{{{0x20000, 0x2000}}};
    Access::Download(*f.cache, ranges);
    REQUIRE(f.bytes[0x100ff] == 0x12);
    REQUIRE(std::all_of(f.bytes + 0x10100, f.bytes + 0x10110, [](u8 b) { return b == 0xee; }));
    REQUIRE(f.bytes[0x10110] == 0x12);
    REQUIRE(f.bytes[0x11fff] == 0x12);
    f.cache->DownloadMemory(0x30000, 0x2000);
    REQUIRE(f.bytes[0x20ff7] == 0x34);
    REQUIRE(std::all_of(f.bytes + 0x20ff8, f.bytes + 0x21008, [](u8 b) { return b == 0xee; }));
    REQUIRE(f.bytes[0x21008] == 0x34);
}

TEST_CASE("A synchronous GPU download keeps CPU writes completed during its wait",
          "[buffer_ports][download_landing]") {
    Fixture f;
    const bool mapped = GENERATE(false, true);
    const bool batched = GENERATE(false, true);
    CAPTURE(mapped, batched);
    constexpr DAddr address = 0x20000;
    const auto id = Access::Add(*f.cache, address, 0x10000, mapped);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0x71);
    Access::Dirty(*f.cache, address, 0x2000);
    bool written = false;
    f.cache->SetPendingCpuWrites([&](const std::function<void(DAddr, size_t)>& visit) {
        if (written) {
            visit(address + 0x100, 64);
        }
    });
    f.runtime.before_completion = [&] {
        written = true;
        std::fill_n(f.bytes + 0x10100, 64, 0xe3);
    };
    if (batched) {
        const std::array<std::pair<DAddr, u64>, 1> ranges{{{address, 0x2000}}};
        Access::Download(*f.cache, ranges);
    } else {
        f.cache->DownloadMemory(address, 0x2000);
    }
    REQUIRE(written);
    REQUIRE(f.bytes[0x100ff] == 0x71);
    REQUIRE(std::all_of(f.bytes + 0x10100, f.bytes + 0x10140, [](u8 b) { return b == 0xe3; }));
    REQUIRE(f.bytes[0x10140] == 0x71);
}

TEST_CASE("A reconstructed manager reads the new backing at a cached device address",
          "[memory_manager][download_landing]") {
    Fixture f;
    std::optional<Tegra::MaxwellDeviceMemoryManager> slot;
    constexpr DAddr address = 0x60000;
    f.bytes[0x10000] = 0x51;
    f.bytes[0x20000] = 0xa9;
    const auto read = [&](VAddr backing) {
        auto& manager = slot.emplace(f.physical);
        manager.BindInterface(&f.rasterizer);
        const auto asid = manager.RegisterProcess(&f.process->GetMemory());
        manager.Map(address, backing, 0x1000, asid);
        u8 result{};
        manager.ReadBlockUnsafe(address, &result, sizeof(result));
        manager.Unmap(address, 0x1000);
        manager.UnregisterProcess(asid);
        slot.reset();
        return result;
    };
    REQUIRE(read(0x20000) == 0x51);
    REQUIRE(read(0x30000) == 0xa9);
}

TEST_CASE("A CPU store published after the GPU landing snapshot keeps the newest bytes",
          "[buffer_ports][download_landing]") {
    Fixture f;
    constexpr DAddr alias = 0x60000;
    constexpr DAddr original = 0x20000;
    const bool aliased = GENERATE(false, true);
    const bool gate_enabled = GENERATE(false, true); // Negative control reproduces the lost store.
    CAPTURE(aliased, gate_enabled);
    const DAddr landing_address = aliased ? alias : original;
    if (aliased) f.device.Map(alias, original, 0x1000, f.asid);
    const auto id = Access::Add(*f.cache, landing_address, 0x1000);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0x56);
    Access::Dirty(*f.cache, landing_address, 0x1000);
    const auto before = f.device.download_landing.WaitCount();
    Core::GPUDirtyMemoryManager collected;
    std::atomic<bool> go{}, stored{};
    bool writer_entered = false;
    size_t aliases_seen{};
    std::thread writer{[&] {
        go.wait(false);
        Common::ScratchBuffer<u32> aliases;
        u8* dest = f.bytes + 0x10100;
        f.device.ApplyOpOnPointer(dest, aliases, [&](DAddr address) {
            ++aliases_seen;
            collected.Collect(address, 16);
            if (gate_enabled) f.device.download_landing.Await(address, 16);
        });
        std::fill_n(dest, 16, 0xe1);
        stored.store(true);
    }};
    f.cache->SetPendingCpuWrites([&](const std::function<void(DAddr, size_t)>& visit) {
        collected.ForEachPending(visit);
        go.store(true);
        go.notify_one();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!stored.load() && f.device.download_landing.WaitCount() == before &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        writer_entered = stored.load() || f.device.download_landing.WaitCount() != before;
    });
    f.cache->DownloadMemory(landing_address, 0x1000);
    writer.join();
    f.cache->SetPendingCpuWrites({});
    REQUIRE(writer_entered);
    REQUIRE(aliases_seen == (aliased ? 2 : 1));
    REQUIRE(f.device.download_landing.WaitCount() - before == (gate_enabled ? 1 : 0));
    const u8 expected = gate_enabled ? 0xe1 : 0x56;
    REQUIRE(std::all_of(f.bytes + 0x10100, f.bytes + 0x10110,
                        [expected](u8 value) { return value == expected; }));
    REQUIRE(f.bytes[0x100ff] == 0x56);
    REQUIRE(f.bytes[0x10110] == 0x56);
    if (aliased) f.device.Unmap(alias, 0x1000);
}

TEST_CASE("Readback tracing recognizes exact page reuse after compute storage writes",
          "[readback_trace]") {
    VideoCore::ReadbackTrace::ComputePages trace;
    constexpr DAddr page = 0x14000;
    REQUIRE_FALSE(trace.Read(page + 1, page + 4097));
    REQUIRE_FALSE(trace.Read(page, page));
    REQUIRE_FALSE(trace.Read(page, page + 4096));
    trace.Write(page + 4096, 4);
    REQUIRE_FALSE(trace.Read(page, page + 4096));
    trace.Write(page - 2, 8);
    REQUIRE(trace.Read(page, page + 4096));
    REQUIRE_FALSE(trace.Read(page, page + 4096));
    trace.Write(page, 16 * 4096 + 1);
    REQUIRE_FALSE(trace.Read(page, page + 4096));
    trace.Write(page, 8);
    constexpr DAddr collision = page + 64 * 4096;
    REQUIRE_FALSE(trace.Read(collision, collision + 4096));
    REQUIRE_FALSE(trace.Read(page, page + 4096));
}


TEST_CASE("Clean optional read hints preserve page-level GPU ownership",
          "[buffer_ports][bounded_clean_hint]") {
    Fixture f;
    const bool bounded = GENERATE(false, true);
    constexpr DAddr demand = 0x20000;
    constexpr DAddr hint = 0x21000;
    const auto id = Access::Add(*f.cache, demand, 0x2000);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0xa5);
    const bool old = Tegra::g_nx2_ampere.exchange(true);
    SCOPE_EXIT { Tegra::g_nx2_ampere = old; };
    Access::Dirty(*f.cache, demand, 0x2000);
    Access::DemandBatch(*f.cache, 1, bounded);
    REQUIRE_FALSE(f.cache->GetFlushArea(hint, 4)->preemtive);
    REQUIRE_FALSE(f.cache->GetFlushArea(demand, 4)->preemtive);
    // The CPU owns all exact bytes on this hinted page, while page-level tracking
    // still routes new CPU writes through the GPU dirty-write collector.
    const std::array<std::pair<DAddr, u64>, 1> claimed{{{hint, 0x1000}}};
    f.cache->ClaimCpuWrites(claimed);
    REQUIRE_FALSE(Access::Pending(*f.cache, hint, 0x1000));
    REQUIRE(Access::CoarseGpuDirty(*f.cache, hint));
    f.cache->DownloadMemory(demand, 0x1000);
    REQUIRE(f.bytes[0x10000] == 0xa5);
    REQUIRE(f.bytes[0x11000] == 0);
    REQUIRE(Access::CoarseGpuDirty(*f.cache, hint));
}

TEST_CASE("Bounded buffer readback lands full demand and leaves cold GPU data pending",
          "[buffer_ports][bounded_read_batch]") {
    Fixture f;
    const bool mapped = GENERATE(false, true);
    const bool bounded = GENERATE(false, true);
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000, mapped);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0xa5);
    const bool old = Tegra::g_nx2_ampere.exchange(true);
    SCOPE_EXIT { Tegra::g_nx2_ampere = old; };
    Access::Dirty(*f.cache, 0x20000, 0x10000);
    Access::DemandBatch(*f.cache, 3, bounded);
    REQUIRE_FALSE(f.cache->GetFlushArea(0x20100, 4)->preemtive);
    Access::DemandBatch(*f.cache, 12, bounded);
    for (u64 page = 1; page < 11; ++page) {
        REQUIRE_FALSE(f.cache->GetFlushArea(0x20000 + 4096 * page, 4)->preemtive);
    }
    // Eight small recent optional pages plus the complete three-page demand.
    REQUIRE_FALSE(f.cache->GetFlushArea(0x2d000, 0x3000)->preemtive);
    f.cache->DownloadMemory(0x2d000, 0x3000);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.bytes[0x10000] == (bounded ? 0 : 0xa5));
    REQUIRE(f.bytes[0x11000] == (bounded ? 0 : 0xa5));
    REQUIRE(f.bytes[0x13000] == 0xa5);
    REQUIRE(f.bytes[0x1afff] == 0xa5);
    REQUIRE(f.bytes[0x1b000] == 0);
    REQUIRE(f.bytes[0x1d000] == 0xa5);
    REQUIRE(f.bytes[0x1ffff] == 0xa5);
    REQUIRE(Access::Pending(*f.cache, 0x20000, 0x1000) == bounded);
    // A cold page remains fully coherent on a later demand.
    f.cache->DownloadMemory(0x20000, 0x1000);
    REQUIRE(f.bytes[0x10000] == 0xa5);
    REQUIRE_FALSE(Access::Pending(*f.cache, 0x20000, 0x1000));
}


TEST_CASE("Optional buffer pages cannot follow a remap or identical physical alias",
          "[buffer_ports][bounded_read_batch]") {
    Fixture f;
    const bool remap_after_learning = GENERATE(false, true);
    constexpr DAddr demand = 0x20000;
    constexpr DAddr extra = 0x21000;
    const bool old = Tegra::g_nx2_ampere.exchange(true);
    SCOPE_EXIT { Tegra::g_nx2_ampere = old; };
    if (!remap_after_learning) f.device.Map(0x60000, extra, 0x1000, f.asid);
    SCOPE_EXIT {
        if (!remap_after_learning) f.device.Unmap(0x60000, 0x1000);
    };
    const auto id = Access::Add(*f.cache, demand, 0x2000);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0xa8);
    Access::Dirty(*f.cache, demand, 0x2000);
    Access::DemandBatch(*f.cache, 1, true);
    REQUIRE_FALSE(f.cache->GetFlushArea(extra, 4)->preemtive);
    REQUIRE_FALSE(f.cache->GetFlushArea(demand, 4)->preemtive);
    if (remap_after_learning) {
        f.device.Unmap(extra, 0x1000);
        f.device.Map(extra, 0x30000, 0x1000, f.asid);
    }
    f.cache->DownloadMemory(demand, 0x1000);
    REQUIRE(f.bytes[0x10000] == 0xa8);
    REQUIRE(f.bytes[0x11000] == 0);
    REQUIRE(f.bytes[0x20000] == 0);
    REQUIRE(Access::Pending(*f.cache, extra, 0x1000));
    REQUIRE(f.runtime.finishes == 1);
}

namespace {
struct EarlyFixture : Fixture {
    static constexpr DAddr page = 0x20000;
    VideoCommon::BufferId id{};
    bool old_ampere{};
    explicit EarlyFixture(bool verify = false) {
        old_ampere = Tegra::g_nx2_ampere.exchange(true);
        Access::EarlyMode(*cache, verify);
        id = Access::Add(*cache, page, 0x4000);
        for (int i = 0; i < 3; ++i) {
            Compute(0x10 + i);
            Demand();
        }
        REQUIRE(Access::EarlyState(*cache).stats.copies == 0);
        Compute(0x42);
        REQUIRE(Access::EarlyState(*cache).stats.copies == 1);
    }
    ~EarlyFixture() {
        Tegra::g_nx2_ampere = old_ampere;
    }
    void Compute(u8 value) {
        std::fill_n(Access::Get(*cache, id).bytes.begin(), 4096, value);
        Access::StorageWrite(*cache, id, page, 4096, true);
        cache->RecordEarlyCpuReads();
    }
    bool Demand() {
        REQUIRE_FALSE(cache->GetFlushArea(page, 1)->preemtive);
        const bool hit = cache->TryEarlyCpuRead(page, 4096);
        if (!hit)
            cache->DownloadMemory(page, 4096);
        return hit;
    }
};
} // namespace

TEST_CASE("A completed early buffer snapshot only lands on demand and keeps CPU stores",
          "[early_buffer]") {
    EarlyFixture f;
    REQUIRE(f.bytes[0x10001] == 0x12);
    REQUIRE(Access::Pending(*f.cache, f.page, 4096));
    f.runtime.CompleteGpu();
    REQUIRE(f.bytes[0x10001] == 0x12);
    f.cache->SetPendingCpuWrites([](const auto& visit) { visit(0x20040, 64); });
    std::fill_n(f.bytes + 0x10040, 64, 0xfe);
    const auto waits = f.runtime.finishes;
    REQUIRE(f.Demand());
    REQUIRE(f.runtime.finishes == waits);
    REQUIRE(f.bytes[0x10000] == 0x42);
    REQUIRE(f.bytes[0x10040] == 0xfe);
    REQUIRE(f.bytes[0x10080] == 0x42);
    REQUIRE_FALSE(Access::Pending(*f.cache, f.page, 4096));
    REQUIRE(Access::EarlyState(*f.cache).stats.hit_bytes == 4032);
    f.cache->RecordEarlyCpuReads();
    REQUIRE(f.runtime.deferred_releases == 1);
}

TEST_CASE("Ineligible early snapshots leave the full original demand available", "[early_buffer]") {
    EarlyFixture f;
    const auto invalidation = GENERATE(0, 1, 2, 3, 4, 5, 6);
    CAPTURE(invalidation);
    if (invalidation != 0)
        f.runtime.CompleteGpu();
    switch (invalidation) {
    case 0:
        break; // Queued but not submitted/completed.
    case 1:
        Access::StorageWrite(*f.cache, f.id, f.page + 32, 4, false);
        break;
    case 2:
        f.cache->OutdateEarlyCpuReads(f.page + 36, 4);
        break; // Query copy/host upload/DMA.
    case 3:
        f.device.Map(0x60000, f.page, 4096, f.asid);
        break;
    case 4:
        f.device.ExcludeEarlyReadReport(f.page - 4, 16);
        break; // Cross-page semaphore/report tail.
    case 5:
        ++Access::Get(*f.cache, f.id).early_read_instance;
        break; // Slot reused.
    case 6:
        Access::EarlyState(*f.cache).active = false;
        break;
    }
    REQUIRE_FALSE(f.cache->TryEarlyCpuRead(f.page, 4096));
    REQUIRE(Access::Pending(*f.cache, f.page, 4096));
    f.cache->DownloadMemory(f.page, 4096);
    REQUIRE(f.bytes[0x10000] == 0x42);
    if (invalidation == 3)
        f.device.Unmap(0x60000, 4096);
}

TEST_CASE("Early verification compares only GPU-owned bytes and returns the original on mismatch",
          "[early_buffer]") {
    const bool corrupt = GENERATE(false, true);
    EarlyFixture f{true};
    f.runtime.CompleteGpu();
    // Deliberately bypass all production writer invalidation, proving VERIFY catches a stale byte.
    if (corrupt)
        Access::Get(*f.cache, f.id).bytes[1] = 0x7d;
    f.cache->SetPendingCpuWrites([](const auto& visit) { visit(0x20040, 64); });
    std::fill_n(f.bytes + 0x10040, 64, 0xee);
    REQUIRE(f.Demand());
    auto& state = Access::EarlyState(*f.cache);
    REQUIRE(state.stats.comparisons == 1);
    REQUIRE(state.stats.compared_bytes == 4032);
    REQUIRE(state.stats.masked_bytes == 64);
    REQUIRE(state.stats.mismatches == (corrupt ? 1 : 0));
    REQUIRE(state.failed == corrupt);
    REQUIRE(f.bytes[0x10001] == (corrupt ? 0x7d : 0x42));
    REQUIRE(f.bytes[0x10040] == 0xee);
}

TEST_CASE("A snapshot cannot replace a multi-page fault or an unserved byte range",
          "[early_buffer]") {
    EarlyFixture f;
    f.runtime.CompleteGpu();
    Access::Dirty(*f.cache, f.page + 4096, 4096);
    std::fill_n(Access::Get(*f.cache, f.id).bytes.begin() + 4096, 4096, 0x7b);
    REQUIRE_FALSE(f.cache->TryEarlyCpuRead(f.page, 8192));
    REQUIRE_FALSE(f.cache->TryEarlyCpuRead(f.page + 4, 4096));
    f.cache->DownloadMemory(f.page, 8192);
    REQUIRE(f.bytes[0x10000] == 0x42);
    REQUIRE(f.bytes[0x11001] == 0x7b);
}

TEST_CASE("Early training expires unread dispatches and never trusts an aliased report",
          "[early_buffer]") {
    EarlyFixture f;
    for (int i = 0; i < 6; ++i) {
        f.runtime.CompleteGpu();
        f.Compute(0x55);
    }
    auto& state = Access::EarlyState(*f.cache);
    const auto& page = state.Slot(f.page);
    REQUIRE(page.reads_after_write == 0);
    REQUIRE_FALSE(page.requested);
    // The report's old alias disappears. Its physical bytes remain excluded.
    f.device.Map(0x60000, f.page, 4096, f.asid);
    f.device.ExcludeEarlyReadReport(0x60040, 16);
    f.device.Unmap(0x60000, 4096);
    REQUIRE_FALSE(f.device.WithEarlyReadPage(f.page, f.device.MappingGeneration(), [] {}));
}

TEST_CASE("Reaching the early-read budget leaves the demand on the standard download",
          "[early_buffer]") {
    EarlyFixture f;
    f.runtime.CompleteGpu();
    REQUIRE(f.Demand());
    auto& state = Access::EarlyState(*f.cache);
    state.frame_bytes = 1024 * 1024;
    f.Compute(0xab);
    REQUIRE(state.stats.caps == 1);
    REQUIRE_FALSE(f.Demand());
    REQUIRE(f.bytes[0x10000] == 0xab);
}

TEST_CASE("Fully CPU-owned early verify bytes are not reported as compared", "[early_buffer]") {
    EarlyFixture f{true};
    f.runtime.CompleteGpu();
    f.cache->SetPendingCpuWrites([&](const auto& visit) { visit(f.page, 4096); });
    REQUIRE(f.Demand());
    const auto& stats = Access::EarlyState(*f.cache).stats;
    REQUIRE(stats.compared_bytes == 0);
    REQUIRE(stats.comparisons == 0);
    REQUIRE(stats.empty_checks == 1);
    REQUIRE(stats.masked_bytes == 4096);
    REQUIRE(f.bytes[0x10001] == 0x12);
}

TEST_CASE(
    "An early snapshot lands only the current dirty subranges and preserves the rest of its page",
    "[early_buffer]") {
    EarlyFixture f;
    f.runtime.CompleteGpu();
    REQUIRE(f.Demand());
    std::fill_n(f.bytes + 0x10000, 4096, 0xde);
    std::fill_n(Access::Get(*f.cache, f.id).bytes.begin(), 4096, 0x79);
    Access::StorageWrite(*f.cache, f.id, f.page + 0x120, 0x60, true);
    f.cache->RecordEarlyCpuReads();
    f.runtime.CompleteGpu();
    f.cache->SetPendingCpuWrites([&](const auto& visit) { visit(f.page + 0x144, 4); });
    REQUIRE(f.Demand());
    const auto* ptr = f.bytes + 0x10000;
    for (size_t i = 0; i < 4096; ++i) {
        const bool owned = i >= 0x120 && i < 0x180 && !(i >= 0x144 && i < 0x148);
        REQUIRE(ptr[i] == (owned ? 0x79 : 0xde));
    }
}

TEST_CASE("Early staging survives cancellation before submission and completion before a read",
          "[early_buffer]") {
    EarlyFixture f;
    const auto start_tick = f.runtime.current_tick;
    auto& state = Access::EarlyState(*f.cache);
    // Do not submit or complete the copies. Observe more requests than the loan budget.
    for (size_t i = 0; i < 40; ++i) {
        REQUIRE_FALSE(f.cache->GetFlushArea(f.page, 4)->preemtive);
        f.Compute(0xa4);
    }
    REQUIRE(f.runtime.deferred.size() == 32);
    REQUIRE(f.runtime.deferred_releases == 0);
    REQUIRE(state.stats.caps > 0);
    state.Reset();
    f.cache->RecordEarlyCpuReads();
    REQUIRE(f.runtime.deferred_releases == 0);
    f.runtime.CompleteGpu();
    f.cache->RecordEarlyCpuReads();
    REQUIRE(f.runtime.deferred_releases == 32);
    REQUIRE(f.runtime.last_release_tick == start_tick);
    REQUIRE(f.bytes[0x10001] == 0x12);
    REQUIRE_FALSE(
        f.Demand()); // Full normal read remains after every speculative loan was canceled.
    REQUIRE(f.bytes[0x10001] == 0xa4);
}

TEST_CASE("Noncoherent early allocations invalidate host memory after the GPU tick",
          "[early_buffer]") {
    EarlyFixture f;
    f.runtime.CompleteGpu();
    REQUIRE(f.Demand());
    f.runtime.noncoherent_early = true;
    f.Compute(0x4b);
    f.runtime.CompleteGpu();
    f.cache->RecordEarlyCpuReads(); // A completed valid loan remains unavailable for pool reuse.
    const auto held = f.runtime.deferred_releases;
    REQUIRE(f.Demand());
    REQUIRE(f.bytes[0x10001] == 0x4b);
    REQUIRE(f.runtime.early_invalidations == 1);
    REQUIRE(f.runtime.deferred_releases == held);
}

TEST_CASE("VERIFY never certifies an unqualified original noncoherent staging allocation",
          "[early_buffer]") {
    EarlyFixture f{true};
    f.runtime.CompleteGpu();
    f.runtime.noncoherent_reference = true;
    REQUIRE(f.Demand());
    const auto& s = Access::EarlyState(*f.cache).stats;
    REQUIRE(s.noncoherent_reference == 1);
    REQUIRE(s.comparisons == 0);
    REQUIRE(s.compared_bytes == 0);
    REQUIRE(s.mismatches == 0);
}

TEST_CASE("A late shadow comparison never counts as a ready acceleration opportunity",
          "[early_buffer]") {
    EarlyFixture f{true};
    REQUIRE(f.Demand());
    const auto& stats = Access::EarlyState(*f.cache).stats;
    REQUIRE(stats.late == 1);
    REQUIRE(stats.unsubmitted == 1);
    REQUIRE(stats.ready_checks == 0);
    REQUIRE(stats.late_checks == 1);
    REQUIRE(stats.compared_bytes == 4096);
    REQUIRE(f.bytes[0x10000] == 0x42);
}

TEST_CASE("Cache teardown returns an outstanding deferred loan with its original retirement tick",
          "[early_buffer]") {
    EarlyFixture f;
    const auto issuing_tick = f.runtime.current_tick;
    REQUIRE(f.runtime.completed_tick < issuing_tick);
    // BufferCache never sends or waits for a speculative command when the session ends.
    f.cache.reset();
    REQUIRE(f.runtime.deferred_releases == 1);
    REQUIRE(f.runtime.last_release_tick == issuing_tick);
    REQUIRE(f.runtime.completed_tick < issuing_tick);
}

TEST_CASE("A failed speculative loan or host invalidation keeps the complete mandatory read",
          "[early_buffer]") {
    const bool loan_failure = GENERATE(false, true);
    EarlyFixture f;
    f.runtime.CompleteGpu();
    REQUIRE(f.Demand());
    f.runtime.fail_loan = loan_failure;
    f.runtime.fail_invalidate = !loan_failure;
    f.Compute(0x37);
    f.runtime.CompleteGpu();
    REQUIRE_FALSE(f.Demand());
    REQUIRE(f.bytes[0x10001] == 0x37);
    REQUIRE(Access::EarlyState(*f.cache).stats.unavailable == 1);
}

TEST_CASE("Source policy preserves decoded R32 bytes through both copy paths", "[atlas_upload_source]") {
    Fixture f;
    constexpr GPUVAddr va = 0x180000;
    constexpr size_t bytes = 16384;
    f.memory->Map(va, 0x20000, bytes, Tegra::PTEKind::PITCH, false);
    f.memory->EnableNativeTiling();
    for (size_t i = 0; i < 0x40000; ++i) f.bytes[i] = static_cast<u8>((i * 37) ^ (i >> 12));
    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::e2D;
    info.native_tiling = true;
    info.format = VideoCommon::PixelFormat::R32_FLOAT;
    info.size = {256, 16, 1};
    info.resources = {1, 1};
    info.block = {0, 1, 0};
    info.layer_stride = VideoCommon::CalculateLayerStride(info);
    const std::array range{std::pair<u64, u64>{0, bytes}};
    const auto plan = VideoCommon::PlanUnswizzleImageRows(va, info, range);
    REQUIRE(plan.layout_valid);
    REQUIRE(plan.runs.size() == 1);
    REQUIRE(plan.total_host_bytes == bytes);
    for (const bool hole : {false, true}) {
        if (hole) f.memory->MapSparse(va + 4096, 4096, false);
        std::vector<u8> expected(bytes, 0xc9), actual(bytes, 0xf3);
        VideoCommon::ExecuteUnswizzlePlan(*f.memory, plan, expected);
        VideoCommon::UnswizzleTimings timing{};
        VideoCommon::ExecuteUnswizzlePlan(*f.memory, plan, actual, &timing, true, true);
        REQUIRE(actual == expected);
        REQUIRE(timing.bulk_bytes == (hole ? 0 : bytes));
#ifdef __linux__
        REQUIRE(timing.file_read_bytes == (hole ? 0 : bytes));
#endif
    }
}

TEST_CASE("Native write marks cover shader-visible padding independent of the CPU snapshot",
          "[buffer_ports][storage_extent]") {
    Fixture f;
    constexpr GPUVAddr base = 0x200000;
    f.memory->Map(base, 0x20000, 0x10000, Tegra::PTEKind::PITCH, true);
    const auto buffer = Access::Add(*f.cache, 0x20000, 0x10000);
    const u32 alignment = GENERATE(16, 256, 4096);
    f.runtime.alignment = alignment;
    for (int pass = 0; pass < 2; ++pass) { // Cold construction, then the cached binding.
        f.Descriptor(base, 0); // CPU sees no padding.
        const auto binding = f.Bind(true, true);
        REQUIRE(binding.gpu_addr == base);
        REQUIRE(binding.size == 0x10000); // Reads retain the complete binding.
        const Shader::StorageWriteExtent extent{.bounded = true, .const_end = 4090};
        const auto reach = Shader::ResolveStorageWriteExtent(
            extent, {1, 1, 1}, std::array<u32, 3>{1, 1, 1});
        REQUIRE(reach.has_value());
        const auto mark = static_cast<u32>(
            Shader::AlignedStorageWriteMark(*reach, alignment, binding.size));
        // Every shader-visible pointer pad is covered even if its UBO differs from the CPU word.
        for (u32 shader_pad = 0; shader_pad < alignment; ++shader_pad) {
            REQUIRE(mark >= *reach + shader_pad);
        }
        Access::StorageWrite(*f.cache, buffer, binding.device_addr, mark, true);
        REQUIRE(Access::Pending(*f.cache, binding.device_addr + mark - 1, 1));
        REQUIRE_FALSE(Access::Pending(*f.cache, binding.device_addr + mark, 1));
    }
}
