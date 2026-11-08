#include <fstream>
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <thread>

#ifdef _MSC_VER
#include <intrin.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstring>
#include <string>

#include "common/cityhash.h"
#include "common/dkb_cpu_trace.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu_split.h"
#include "video_core/macro_dma_coverage.h"
#include "video_core/macro_fifo_shape.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"

namespace VideoCommon {

namespace {

void Pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

// All counters belong to the split front. Stop reads them after the front has stopped.
class FrontWaitProbe {
    using Clock = std::chrono::steady_clock;

    struct Counts {
        u64 calls{}, blocked{}, words{}, max_words{}, wait_ns{}, max_ns{}, parks{}, park_ns{};
        std::array<u64, 6> latency{};
    };

public:
    static constexpr size_t RingSpace = drain_site_names.size();

    bool Configured() const { return configured; }
    bool Active() const { return active; }

    void Start() {
        const char* setting = std::getenv("STITCH_SPLIT_WAIT_TRACE");
        configured = setting && std::string_view{setting} == "1";
        if (!configured) {
            return;
        }
        const char* file = std::getenv("STITCH_SPLIT_WAIT_TRACE_FILE");
        marker = file ? file : "";
        const char* macro = std::getenv("STITCH_MACRO_DMA_COVERAGE");
        macro_enabled = macro && std::string_view{macro} == "1";
        Poll(true);
    }

    void Poll(bool initial = false) {
        if (!initial && ++checks < 64) {
            return;
        }
        checks = 0;
        const auto now = Clock::now();
        if (now < next_poll) {
            return;
        }
        next_poll = now + std::chrono::milliseconds{250};
        std::error_code error;
        const bool requested = marker.empty() || (std::filesystem::exists(marker, error) && !error);
        if (active && (!requested || now >= deadline)) {
            Report(now, requested ? "complete" : "marker-off");
        }
        if (requested && !latched) {
            ++window;
            counts = {};
            if (macro_enabled) {
                coverage = {};
                lists = {};
            }
            begin = now;
            deadline = now + std::chrono::seconds{10};
            active = true;
            LOG_INFO(HW_GPU, "Split front wait start window={} gpu_profile={}", window,
                     Nx2Profile::Enabled());
        }
        latched = requested;
    }

    bool MacroActive() const {
        return active && macro_enabled;
    }
    void BeginMacro(s32 channel, u64 as_id, u32 method, u32 start, const void* owner,
                    u64 generation, std::span<const u32> code, bool fifo, u32 words,
                    size_t parameters, bool pending, bool instance, bool sink, u32 shadow,
                    u32 render_mode, u64& seen) {
        clean_entry = MacroDmaCoverage::CleanEntry(pending, instance, sink, shadow); // Passthrough
        macro_read = coverage.Begin(
            window, seen, fifo, words, {as_id, generation, 0, channel, method, start}, owner, code,
            parameters, pending, instance, sink, shadow,
            [](std::span<const u32> code_words) {
                return Common::CityHash64(reinterpret_cast<const char*>(code_words.data()),
                                          code_words.size_bytes());
            },
            [render_mode](auto words_, u32 entry) {
                return MacroFifoShape(words_, entry, render_mode);
            });
    }
    void EndMacro() {
        macro_read = nullptr;
    }
    void ListSite(GpuSplit::ListWriterSite site) {
        list_site = static_cast<size_t>(site);
    }
    void Rechecked(GpuSplit::ListWriterSite site, bool became_gpu_written) {
        if (MacroActive()) {
            auto& row = lists[static_cast<size_t>(site)];
            ++row.rechecks;
            row.changed += became_gpu_written;
        }
    }

    void Observe(size_t site, u64 pending_words) {
        macro_wait = nullptr;
        list_wait = nullptr;
        if (macro_enabled && site == static_cast<size_t>(DrainSite::ListWriters) &&
            list_site < lists.size()) {
            auto& detail = lists[list_site];
            ++detail.calls;
            if (pending_words) {
                ++detail.blocked;
                list_wait = &detail;
            }
        }
        if (macro_read) {
            if (site == static_cast<size_t>(DrainSite::MacroDmaRead))
                macro_kind = 0;
            else if (site == static_cast<size_t>(DrainSite::MacroDmaFlush))
                macro_kind = 1;
            else if (site == static_cast<size_t>(DrainSite::MacroDmaWrites))
                macro_kind = 2;
            else
                macro_kind = 3;
            if (macro_kind < 3) {
                ++macro_read->sync_calls[macro_kind];
                if (pending_words) {
                    ++macro_read->blocked[macro_kind];
                    macro_wait = macro_read;
                }
            }
        }
        auto& row = counts[site];
        ++row.calls;
        if (pending_words != 0) {
            ++row.blocked;
            row.words += pending_words;
            row.max_words = (std::max)(row.max_words, pending_words);
            current = &row;
            wait_start = Clock::now();
        }
    }

    void FinishWait() {
        const auto now = Clock::now();
        const auto ns = Elapsed(wait_start, now);
        if (list_wait) {
            list_wait->ns += ns;
            list_wait->max_ns = std::max(list_wait->max_ns, ns);
            list_wait = nullptr;
        }
        if (macro_wait) {
            macro_wait->wait_ns[macro_kind] += ns;
            if (clean_entry && macro_kind == 0)
                macro_wait->clean_entry_drain_ns += ns;
            macro_wait->max_ns[macro_kind] = std::max(macro_wait->max_ns[macro_kind], ns);
            macro_wait = nullptr;
        }
        current->wait_ns += ns;
        current->max_ns = (std::max)(current->max_ns, ns);
        constexpr std::array<u64, 5> limits{2000, 10000, 50000, 250000, 1000000};
        const auto bin = std::upper_bound(limits.begin(), limits.end(), ns) - limits.begin();
        ++current->latency[static_cast<size_t>(bin)];
        current = nullptr;
        if (now >= deadline) {
            Report(now, "complete");
        }
    }

    void BeginPark() { park_start = Clock::now(); }
    void FinishPark() {
        ++current->parks;
        current->park_ns += Elapsed(park_start, Clock::now());
    }

    void Stop() {
        if (active) {
            Report(Clock::now(), "stop");
        }
        *this = {};
    }

private:
    static u64 Elapsed(Clock::time_point first, Clock::time_point last) {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(last - first).count());
    }

    void Report(Clock::time_point now, const char* reason) {
        active = false;
        macro_read = nullptr;
        macro_wait = nullptr;
        if (macro_enabled) {
            auto report_group = [&](const MacroDmaCoverage::Group& row, bool excess) {
                if (!row.invocations && !row.reads[0].requests && !row.reads[1].requests)
                    return;
                LOG_INFO(HW_GPU,
                         "Macro DMA coverage window={} overflow={} channel={} as={} "
                         "method={:#x} start={:#x} generation={} code_ram_hash={:016x} "
                         "invocations={} parameter_words={} entry_draw_pending={} "
                         "entry_instance_loop={} entry_sink_pending={} "
                         "entry_shadow_modes={},{},{},{} shape={}",
                         window, excess, row.key.channel, row.key.as_id, row.key.method,
                         row.key.start, row.key.code_generation, row.key.ram_hash, row.invocations,
                         row.parameter_words, row.draw_pending, row.instance_loop, row.sink_pending,
                         row.shadow_modes[0], row.shadow_modes[1], row.shadow_modes[2],
                         row.shadow_modes[3], row.shape);
                for (size_t i = 0; i < 2; ++i) {
                    const auto& r = row.reads[i];
                    if (!r.requests)
                        continue;
                    LOG_INFO(
                        HW_GPU,
                        "Macro DMA waits window={} overflow={} channel={} as={} method={:#x} "
                        "start={:#x} generation={} code_ram_hash={:016x} target={} dma_calls={} "
                        "words={} "
                        "drain_calls={} flush_calls={} writer_calls={} drain_blocked={} "
                        "flush_blocked={} "
                        "writer_blocked={} drain_ns={} flush_ns={} writer_ns={} max_drain_ns={} "
                        "max_flush_ns={} max_writer_ns={} max_words={} clean_entry_drain_ns={}",
                        window, excess, row.key.channel, row.key.as_id, row.key.method,
                        row.key.start, row.key.code_generation, row.key.ram_hash,
                        i ? "fifo" : "data_ram", r.requests, r.words, r.sync_calls[0],
                        r.sync_calls[1], r.sync_calls[2], r.blocked[0], r.blocked[1], r.blocked[2],
                        r.wait_ns[0], r.wait_ns[1], r.wait_ns[2], r.max_ns[0], r.max_ns[1],
                        r.max_ns[2], r.max_words, r.clean_entry_drain_ns);
                }
            };
            for (size_t i = 0; i < coverage.group_count; ++i)
                report_group(coverage.groups[i], false);
            report_group(coverage.overflow, true);
            LOG_INFO(HW_GPU,
                     "Macro DMA coverage summary window={} groups={} code_images={} "
                     "mutated_image_reads={} excess_image_reads={} excess_group_reads={}",
                     window, coverage.group_count, coverage.hash_count,
                     coverage.changed_image_reads, coverage.excess_image_reads,
                     coverage.excess_group_reads);
            constexpr std::array list_names{"fresh-header", "continued-method", "resumed-entry",
                                            "known-gpu-page"};
            for (size_t i = 0; i < lists.size(); ++i) {
                const auto& row = lists[i];
                if (row.calls)
                    LOG_INFO(HW_GPU,
                             "List writer coverage window={} reason={} calls={} blocked={} "
                             "wait_ns={} max_ns={} rechecked={} became_gpu_written={}",
                             window, list_names[i], row.calls, row.blocked, row.ns, row.max_ns,
                             row.rechecks, row.changed);
            }
        }
        LOG_INFO(HW_GPU, "Split front wait end window={} wall_ns={} reason={}", window,
                 Elapsed(begin, now), reason);
        for (size_t i = 0; i < counts.size(); ++i) {
            const auto& c = counts[i];
            if (c.calls == 0) {
                continue;
            }
            LOG_INFO(HW_GPU,
                     "Split front wait window={} site='{}' calls={} blocked={} wait_ns={} "
                     "max_ns={} pending_words={} max_words={} parks={} park_ns={} "
                     "bins_lt_2_10_50_250_1000us_then_ge={},{},{},{},{},{}",
                     window, i == RingSpace ? "ring space" : drain_site_names[i], c.calls,
                     c.blocked, c.wait_ns, c.max_ns, c.words, c.max_words, c.parks, c.park_ns,
                     c.latency[0], c.latency[1], c.latency[2], c.latency[3], c.latency[4], c.latency[5]);
        }
    }

    std::array<Counts, RingSpace + 1> counts{};
    Counts* current{};
    MacroDmaCoverage coverage;
    MacroDmaCoverage::Direction* macro_read{};
    MacroDmaCoverage::Direction* macro_wait{};
    size_t macro_kind{};
    bool macro_enabled{}, clean_entry{};
    struct ListDetail {
        u64 calls{}, blocked{}, ns{}, max_ns{}, rechecks{}, changed{};
    };
    std::array<ListDetail, static_cast<size_t>(GpuSplit::ListWriterSite::Count)> lists{};
    ListDetail* list_wait{};
    size_t list_site{};
    std::filesystem::path marker;
    Clock::time_point next_poll{}, begin{}, deadline{}, wait_start{}, park_start{};
    u64 window{};
    u32 checks{};
    bool configured{}, active{}, latched{};
};

FrontWaitProbe front_wait_probe;
[[maybe_unused]] std::atomic<u32> g_back_thread_id{};

} // Anonymous namespace

void GpuSplit::CheckOwner(const char* where) {
#ifdef _WIN32
    // Also while the back is idle: its idle hook submits (Scheduler::FlushDeferred).
    if (!instance.back_running.load(std::memory_order_acquire)) {
        return;
    }
    const DWORD thread = GetCurrentThreadId();
    if (thread == g_back_thread_id.load(std::memory_order_relaxed)) {
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1) >= 12) {
        return;
    }
    std::string stack;
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module{};
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* base_name = std::strrchr(name, '\\');
        stack += fmt::format(" {}+{:#x}", base_name ? base_name + 1 : name,
                             reinterpret_cast<uintptr_t>(frames[i]) -
                                 reinterpret_cast<uintptr_t>(module));
    }
    LOG_ERROR(HW_GPU, "GPU split owner check: {} on thread {} while the back runs; stack:{}",
              where, thread, stack);
#else
    (void)where;
#endif
}

GpuSplit GpuSplit::instance;

void GpuSplit::MaybeStart(Core::System& system_) {
    if (active && front_wait_probe.Configured()) {
        front_wait_probe.Poll();
    }
    static const char mode = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT");
        return value != nullptr ? value[0] : '2';
    }();
    const bool wanted = mode == '1' || mode == '2';
    if (!wanted || !Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        return;
    }
    // The front reads command lists straight from guest memory; a read that flushes the caches
    // first (safe DMA, accurate GPU) would touch back-owned state. The settings can change
    // mid-game: hand the engines back to the front (and start again when they allow it).
    const bool safe_reads = Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh()
                                                          : Settings::IsDMALevelSafe();
    const bool allowed = !safe_reads && !Settings::IsGPULevelHigh() &&
                         Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
    if (active && !allowed) {
        Drain();
        Stop();
        LOG_INFO(HW_GPU, "GPU thread split: stopped, the accuracy settings need the caches on the front");
        return;
    }
    if (active || !allowed) {
        return;
    }
    const auto enabled = [](const char* name) {
        const char* value = std::getenv(name);
        return value && std::string_view{value} == "1";
    };
    const char* const fifo = std::getenv("STITCH_ORDERED_FIFO_MACRO");
    ordered_fifo_mode =
        fifo && std::string_view{fifo} == "verify" ? OrderedFifoMode::Verify : OrderedFifoMode::Off;
    ordered_fifo_marker = std::getenv("STITCH_ORDERED_FIFO_MODE_FILE");
    ordered_fifo_polls = 0;
    ordered_fifo_selected = OrderedFifoMode::Verify;
    ordered_fifo_stats = {};
    fifo_position = fifo_batch = fifo_observer_joins = 0;
    fifo_backing = CommandBacking::GuestMayChange;
    front_wait_probe.Start();
    writer_requested = enabled("STITCH_WRITER_FRONTIER");
    writer_verify = enabled("STITCH_WRITER_VERIFY");
    writer_marker = std::getenv("STITCH_WRITER_FRONTIER_FILE");
    system = &system_;
    check_owner = std::getenv("EDEN_NX2_SPLIT_CHECK") != nullptr;
    ring = std::make_unique<u64[]>(RingWords);
    back_thread = std::jthread([this](std::stop_token stop_token) { BackLoop(stop_token); });
    active = true;
    ++starts;
    const char* jit = std::getenv("EDEN_NX2_MME_JIT");
    front_macros = mode == '2' && (jit == nullptr || jit[0] != '0');
    LOG_INFO(HW_GPU, "GPU thread split: engine methods run on a second thread{}",
             front_macros ? ", macros on the first" : "");
}

void GpuSplit::Stop() {
    if (!back_thread.joinable()) {
        return;
    }
    back_thread.request_stop();
    wake.fetch_add(1, std::memory_order_seq_cst);
    wake.notify_one();
    back_thread.join();
    front_wait_probe.Stop();
    active = false;
    // The split is static and outlives the emulation session. The next session's back thread
    // starts reading at 0: with the old positions kept it skipped through the new, empty ring a
    // wrap at a time and never reached the old write position (the next game's first GPU work
    // never ran, it waited on its syncpoint forever), and the engines it held were freed.
    ring.reset();
    fifo_position = fifo_batch = 0;
    fifo_backing = CommandBacking::GuestMayChange;
    write_position = 0;
    last_writer_position = 0;
    last_sync_position = 0;
    last_any_write = last_noted_write = writer_mapping_generation = 0;
    write_positions.fill(0);
    writer_requested = writer_frontier = writer_verify = false;
    // A failed verification stays latched for the life of this split, even across Stop/Start.
    writer_marker = nullptr;
    writer_marker_reads = 0;
    writer_attempts = writer_waits = writer_fast = writer_compared = 0;
    published_local = 0;
    consumed_cache = 0;
    publish_every = PublishEvery;
    front_engine = nullptr;
    back_engine = nullptr;
    front_macros = false;
    published.store(0, std::memory_order_relaxed);
    consumed.store(0, std::memory_order_relaxed);
    back_waiting.store(false, std::memory_order_relaxed);
    front_waiting.store(false, std::memory_order_relaxed);
}

u64* GpuSplit::ReserveSlow(size_t words) {
    size_t index = write_position & RingMask;
    if (index + words > RingWords) {
        // Entries never wrap: skip to the start of the ring.
        const size_t rest = RingWords - index;
        WaitForSpace(rest);
        ring[index] = static_cast<u64>(Kind::Wrap);
        write_position += rest;
        index = 0;
    }
    WaitForSpace(words);
    return &ring[index];
}

void GpuSplit::WaitForSpace(size_t words) {
    const u64 needed = write_position + words - consumed_cache;
    if (front_wait_probe.Active()) {
        front_wait_probe.Observe(FrontWaitProbe::RingSpace, needed > RingWords ? needed - RingWords : 0);
    }
    if (needed <= RingWords) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (write_position + words - consumed_cache <= RingWords) {
            if (front_wait_probe.Active()) {
                front_wait_probe.FinishWait();
            }
            return;
        }
        if (spins < 4096) {
            Pause();
        } else {
            std::this_thread::yield();
        }
    }
}

void GpuSplit::Publish() {
    if (published_local == write_position) {
        return;
    }
    published_local = write_position;
    publish_every = std::min<size_t>(publish_every * 2, PublishEvery);
    published.store(write_position, std::memory_order_seq_cst);
    if (back_waiting.load(std::memory_order_seq_cst)) {
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }
}

GpuSplit::CommandBacking GpuSplit::EnterFifoCommandScope(CommandBacking backing) {
    JoinOrderedFifo(FifoJoin::FrontObserver);
    return std::exchange(fifo_backing, backing);
}
void GpuSplit::LeaveFifoCommandScope(CommandBacking previous) {
    // Push can refresh deferred entries / re-snapshot after suspension before the next
    // list-writer gate. Never expose such frontend observers to an unfinished publication.
    JoinOrderedFifo(FifoJoin::EndOfEntry);
    fifo_backing = previous;
}
void GpuSplit::NoteOrderedFifoWriter() {
    NoteWriter();
    fifo_position = write_position;
    ++fifo_batch;
}
void GpuSplit::JoinOrderedFifo(FifoJoin reason) {
    if (!fifo_position)
        return;
    const auto target = std::exchange(fifo_position, 0);
    const auto batch = std::exchange(fifo_batch, 0);
    auto& c = ordered_fifo_stats;
    ++c.groups;
    c.peak_group = std::max(c.peak_group, batch);
    c.later_ring_words += write_position - target;
    fifo_observer_joins += reason == FifoJoin::FrontObserver;
    const auto start = std::chrono::steady_clock::now();
    if (WaitFor(target, DrainSite::FifoPublication)) {
        ++c.waited_groups;
        c.wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
    }
    if ((c.groups & 0xffff) == 0)
        LOG_INFO(HW_GPU,
                 "Ordered FIFO publication groups={} blocked={} wait_ns={} "
                 "observer_joins={} deferred={} peak_group={} queued_words_after_fifo={}",
                 c.groups, c.waited_groups, c.wait_ns, fifo_observer_joins, c.deferred,
                 c.peak_group, c.later_ring_words);
}

GpuSplit::OrderedFifoMode GpuSplit::SelectOrderedFifoMode() {
    if (ordered_fifo_failed || ordered_fifo_mode == OrderedFifoMode::Off)
        return OrderedFifoMode::Off;
    if (!ordered_fifo_marker || !*ordered_fifo_marker)
        return ordered_fifo_mode;
    if ((ordered_fifo_polls++ % 4096) == 0) {
        OrderedFifoMode next = OrderedFifoMode::Off;
        const std::filesystem::path file{ordered_fifo_marker};
        std::error_code ec;
        if (file.is_absolute() && std::filesystem::is_regular_file(file, ec) && !ec) {
            std::ifstream stream(file, std::ios::binary);
            std::array<char, 12> bytes{};
            stream.read(bytes.data(), bytes.size());
            if (stream.eof() && !stream.bad()) {
                std::string_view value(bytes.data(), static_cast<size_t>(stream.gcount()));
                if (value.ends_with("\n"))
                    value.remove_suffix(1);
                if (value == "verify")
                    next = OrderedFifoMode::Verify;
                else if (value == "candidate")
                    next =
                        ordered_fifo_stats.checked ? OrderedFifoMode::On : OrderedFifoMode::Verify;
            }
        }
        if (next != ordered_fifo_selected)
            LOG_INFO(HW_GPU, "Ordered FIFO mode={} checked={} mismatches={}",
                     next == OrderedFifoMode::On       ? "candidate"
                     : next == OrderedFifoMode::Verify ? "verify"
                                                       : "baseline",
                     ordered_fifo_stats.checked, ordered_fifo_stats.mismatches);
        ordered_fifo_selected = next;
    }
    return ordered_fifo_selected;
}

bool GpuSplit::MacroDmaTraceActive() const {
    return front_wait_probe.MacroActive();
}

void GpuSplit::BeginMacroDmaTrace(s32 channel, u64 as_id, u32 method, u32 start, const void* owner,
                                  u64 generation, std::span<const u32> code, bool fifo, u32 words,
                                  size_t parameters, bool pending, bool instance, bool sink,
                                  u32 shadow, u32 render_mode, u64& seen) {
    front_wait_probe.BeginMacro(channel, as_id, method, start, owner, generation, code, fifo, words,
                                parameters, pending, instance, sink, shadow, render_mode, seen);
}
void GpuSplit::EndMacroDmaTrace() {
    front_wait_probe.EndMacro();
}

void GpuSplit::Drain(DrainSite site) {
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (front_wait_probe.Active()) {
        front_wait_probe.Observe(static_cast<size_t>(site), write_position - consumed_cache);
    }
    if (consumed_cache == write_position) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    const u64 start = Nx2Profile::Enabled() ? Nx2Profile::Ticks() : 0;
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache == write_position) {
            publish_every = 8;
            if (front_wait_probe.Active()) {
                front_wait_probe.FinishWait();
            }
            Nx2Profile::AddDrain(site, start);
            return;
        }
        Backoff(spins, write_position);
    }
}

void GpuSplit::NoteListRecheck(ListWriterSite site, bool became_gpu_written) {
    front_wait_probe.Rechecked(site, became_gpu_written);
}

bool GpuSplit::WaitForWriters(ListWriterSite site) {
    if (front_wait_probe.MacroActive())
        front_wait_probe.ListSite(site);
    return WaitFor(std::min(last_writer_position, write_position), DrainSite::ListWriters);
}

bool GpuSplit::WaitForSync() {
    return WaitFor(std::min(last_sync_position, write_position), DrainSite::ListSync);
}

namespace {

/// Calls `func(device_address, size)` for each mapped piece of [address, address + size) within
/// one 4 KiB GPU page. False means a piece was not mapped at classification time.
template <typename Func>
bool ForEachDeviceRange(const Tegra::MemoryManager& memory, GPUVAddr address, u64 size,
                        Func&& func) {
    constexpr GPUVAddr page_size = 0x1000;
    const GPUVAddr end = address + size;
    for (GPUVAddr begin = address; begin < end;) {
        const GPUVAddr page_end = begin + std::min(page_size - (begin & (page_size - 1)), end - begin);
        const auto device = memory.GpuToCpuAddress(begin);
        if (!device) {
            return false;
        }
        func(*device, page_end - begin);
        begin = page_end;
    }
    return true;
}

} // Anonymous namespace

void GpuSplit::NoteWrite(const Tegra::MemoryManager& memory, GPUVAddr address, u64 size) {
    const u64 generation = memory.DeviceMappingGeneration();
    if (generation != writer_mapping_generation) {
        // A previous queued write might have already obtained a pointer to its old backing.
        last_any_write = std::max(last_any_write, last_noted_write);
        writer_mapping_generation = generation;
    }
    // A write may allocate backing for a Switch 2 sparse page. Never predict its address.
    if (size > MaxNotedBytes || address + size < address ||
        (address < Tegra::g_nx2_sparse_high.load(std::memory_order_relaxed) &&
         address + size > Tegra::g_nx2_sparse_low.load(std::memory_order_relaxed))) {
        NoteAnyWrite();
        return;
    }
    if (!ForEachDeviceRange(memory, address, size,
                            [this](DAddr device, u64 bytes) { NoteDeviceWrite(device, bytes); })) {
        NoteAnyWrite();
    }
}

void GpuSplit::NoteDeviceWrite(DAddr address, u64 size) {
    const u64 last = (address + size - 1) >> WriteGranuleBits;
    for (u64 granule = address >> WriteGranuleBits; granule <= last; ++granule) {
        write_positions[WriteSlot(granule)] = write_position;
    }
    last_noted_write = write_position;
}

u64 GpuSplit::WritesTarget(const Tegra::MemoryManager& memory, GPUVAddr address, u64 size) const {
    if (memory.DeviceMappingGeneration() != writer_mapping_generation ||
        size > MaxNotedBytes || address + size < address) {
        return std::max(last_any_write, last_noted_write);
    }
    u64 target = last_any_write;
    if (!ForEachDeviceRange(memory, address, size, [this, &target](DAddr device, u64 bytes) {
            target = std::max(target, DeviceWritesTarget(device, bytes));
        })) {
        return std::max(last_any_write, last_noted_write);
    }
    return target;
}

u64 GpuSplit::DeviceWritesTarget(DAddr address, u64 size) const {
    u64 target = 0;
    const u64 last = (address + size - 1) >> WriteGranuleBits;
    for (u64 granule = address >> WriteGranuleBits; granule <= last; ++granule) {
        target = std::max(target, write_positions[WriteSlot(granule)]);
    }
    return target;
}

bool GpuSplit::WaitForWrites(const Tegra::MemoryManager& memory, GPUVAddr address, u64 size) {
    return WaitFor(std::min(WritesTarget(memory, address, size), write_position),
                   DrainSite::MacroDmaWrites);
}

bool GpuSplit::SelectWriterFrontier() {
    if (!writer_requested || writer_rejected) {
        return false;
    }
    if ((writer_marker_reads++ & 1023U) == 0) {
        std::error_code error;
        const bool enable = !writer_marker || std::filesystem::exists(writer_marker, error);
        if (enable != writer_frontier) {
            // Every entry sent before tracking was enabled is an unknown writer.
            last_any_write = write_position;
            writer_frontier = enable;
            LOG_INFO(HW_GPU, "Macro writer frontier {} (verify={})", enable, writer_verify);
        }
    }
    return writer_frontier;
}

void GpuSplit::CountWriterRead(bool waited, bool used, bool compared) {
    ++writer_attempts;
    writer_waits += waited;
    writer_fast += used;
    writer_compared += compared;
    if ((writer_attempts & 65535U) == 0) {
        LOG_INFO(HW_GPU, "Macro writer reads: attempts={} waited_for_writer={} "
                         "no_cache_flush={} compared={}",
                 writer_attempts, writer_waits, writer_fast, writer_compared);
    }
}

void GpuSplit::RejectWriterFrontier() {
    writer_rejected = true;
    writer_frontier = false;
}

bool GpuSplit::WaitFor(u64 target, DrainSite site) {
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (front_wait_probe.Active()) {
        front_wait_probe.Observe(static_cast<size_t>(site), target > consumed_cache ? target - consumed_cache : 0);
    }
    if (consumed_cache >= target) {
        return false;
    }
    Nx2Profile::Scope scope{Nx2Profile::Zone::WriterWait};
    wait_target.store(target, std::memory_order_seq_cst);
    Publish();
    const u64 start = Nx2Profile::Enabled() ? Nx2Profile::Ticks() : 0;
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache >= target) {
            wait_target.store(~u64{0}, std::memory_order_relaxed);
            if (front_wait_probe.Active()) {
                front_wait_probe.FinishWait();
            }
            Nx2Profile::AddDrain(site, start);
            return true;
        }
        Backoff(spins, target);
    }
}

void GpuSplit::Backoff(u32 spins, u64 target) {
    if (spins < 16384) {
        Pause();
        return;
    }
    if (!back_running.load(std::memory_order_acquire)) {
        std::this_thread::yield();
        return;
    }
    // A long wait sleeps until the back stores a new position (port of #161's front_wake): the
    // back checks front_waiting after each store, the front checks the position after setting it.
    front_waiting.store(true, std::memory_order_seq_cst);
    const u32 seen = front_wake.load(std::memory_order_seq_cst);
    if (consumed.load(std::memory_order_seq_cst) < target) {
        if (front_wait_probe.Active()) {
            front_wait_probe.BeginPark();
        }
        front_wake.wait(seen, std::memory_order_seq_cst);
        if (front_wait_probe.Active()) {
            front_wait_probe.FinishPark();
        }
    }
    front_waiting.store(false, std::memory_order_relaxed);
}

void GpuSplit::SwitchEngine(Tegra::Engines::EngineInterface* engine) {
    front_engine = engine;
    u64* const entry = Reserve(2);
    entry[0] = static_cast<u64>(Kind::Engine);
    entry[1] = reinterpret_cast<u64>(engine);
    Commit(2);
}

void GpuSplit::MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                           u32 amount, u32 methods_pending, const GPUVAddr* segment,
                           Writes writes) {
    SelectEngine(engine);
    // Long runs (constant buffer and inline uploads) go in pieces, as when a method's data
    // continues in the next GPFIFO entry.
    while (amount > 0) {
        const u32 piece = std::min(amount, MaxMultiWords);
        const size_t head = segment != nullptr ? 3 : 2;
        const size_t size = head + (piece + 1) / 2;
        u64* const entry = Reserve(size);
        entry[0] = static_cast<u64>(Kind::Multi) | (segment != nullptr ? HasSegment : 0) |
                   (u64{method & 0xffff} << 8) | (u64{piece} << 32);
        entry[1] = methods_pending;
        if (segment != nullptr) {
            entry[2] = *segment;
        }
        entry[size - 1] = 0;
        std::memcpy(entry + head, words, size_t{piece} * sizeof(u32));
        Commit(size);
        words += piece;
        amount -= piece;
        methods_pending -= piece;
    }
    if (writes == Writes::Anywhere) {
        NoteAnyWrite();
    }
}

void GpuSplit::DeferFunction(std::function<void()>&& func) {
    auto* const heap = new std::function<void()>(std::move(func));
    Defer([heap] {
        (*heap)();
        delete heap;
    });
}

size_t GpuSplit::Execute(const u64* entry, size_t words) {
    const u64 head = entry[0];
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return 0;
    case Kind::Engine:
        back_engine = reinterpret_cast<Tegra::Engines::EngineInterface*>(entry[1]);
        return 2;
    case Kind::Method: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 argument = static_cast<u32>(head >> 32);
        auto* const engine = back_engine;
        if (engine->fast_maxwell3d) {
            // Nx2Perf: as below without the profile zones (off with fast_maxwell3d) and the
            // virtual call.
            using Tegra::Engines::Maxwell3D;
            auto* const maxwell3d = static_cast<Maxwell3D*>(engine);
            constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
            if (method - cb_data < 16 && (head & HasSegment) == 0) {
                // Single constant buffer words in a row (macros write them one by one) go up in
                // one write. They are executable: each entry would only consume the sink first.
                std::array<u32, 64> methods;
                std::array<u32, 64> arguments;
                methods[0] = method;
                arguments[0] = argument;
                u32 count = 1;
                while (count < methods.size() && count < words) {
                    const u64 next = entry[count];
                    const u32 next_method = static_cast<u32>((next >> 8) & 0xffff);
                    if (static_cast<Kind>(next & 0xf) != Kind::Method || (next & HasSegment) != 0 ||
                        next_method - cb_data >= 16) {
                        break;
                    }
                    methods[count] = next_method;
                    arguments[count] = static_cast<u32>(next >> 32);
                    ++count;
                }
                maxwell3d->ConsumeSink(*system);
                if (count == 1) {
                    maxwell3d->CallMethod(*system, method, argument, true);
                } else {
                    maxwell3d->CallConstBufferWords(*system, methods.data(), arguments.data(),
                                                    count);
                }
                return count;
            }
            if ((head & Direct) != 0) {
                maxwell3d->ConsumeSink(*system);
                maxwell3d->CallMethod(*system, method, argument, true);
            } else if (!maxwell3d->execution_mask[method]) {
                maxwell3d->method_sink.emplace_back(method, argument);
            } else {
                maxwell3d->ConsumeSink(*system);
                if ((head & HasSegment) != 0) {
                    maxwell3d->current_dma_segment = entry[1];
                }
                maxwell3d->CallMethod(*system, method, argument, (head & IsLast) != 0);
            }
            return (head & HasSegment) != 0 ? 2 : 1;
        }
        if ((head & Direct) != 0) {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            engine->CallMethod(*system, method, argument, true);
        } else if (!engine->execution_mask[method]) {
            engine->method_sink.emplace_back(method, argument);
        } else {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            if ((head & HasSegment) != 0) {
                engine->current_dma_segment = entry[1];
            }
            engine->CallMethod(*system, method, argument, (head & IsLast) != 0);
        }
        return (head & HasSegment) != 0 ? 2 : 1;
    }
    case Kind::Multi: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 amount = static_cast<u32>(head >> 32);
        const size_t data = (head & HasSegment) != 0 ? 3 : 2;
        auto* const engine = back_engine;
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
        engine->ConsumeSink(*system);
        if ((head & HasSegment) != 0) {
            engine->current_dma_segment = entry[2];
        }
        engine->CallMultiMethod(*system, method, reinterpret_cast<const u32*>(entry + data),
                                amount, static_cast<u32>(entry[1]));
        return data + (amount + 1) / 2;
    }
    case Kind::Call: {
        const size_t payload = static_cast<size_t>((head >> 8) & 0xffffff);
        reinterpret_cast<void (*)(const u64*)>(entry[1])(entry + 2);
        return 2 + payload;
    }
    }
    return 1;
}

void GpuSplit::BackLoop(std::stop_token stop_token) {
    const Common::DkbCpuTrace::Registration cpu_trace{Common::DkbCpuTrace::Role::GpuBack};
    Common::SetCurrentThreadName("GPUBack");
    on_back_thread = true;
    back_running.store(true, std::memory_order_release);
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    if (Settings::values.pin_gpu_threads.GetValue()) {
        Common::PinCurrentThread(Common::PinnedRole::GpuBack);
    }
    system->RegisterHostThread();
#ifdef _WIN32
    g_back_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
#endif
    u64 read_position = 0;
    while (!stop_token.stop_requested()) {
        Nx2Profile::Report("GPU back thread");
        u64 available = published.load(std::memory_order_acquire);
        if (available == read_position) {
            Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWait};
            // The front usually sends more within microseconds: spin briefly before sleeping.
            for (u32 spins = 0; spins < 8192 && available == read_position &&
                                !hook_requested.load(std::memory_order_relaxed);
                 ++spins) {
                Pause();
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position && idle_hook != nullptr) {
                hook_requested.store(false, std::memory_order_seq_cst);
                idle_hook(idle_argument);
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                // Publish bumps `wake` when it sees back_waiting: read it before the last look
                // at `published`, so a publish in between ends the wait at once.
                back_waiting.store(true, std::memory_order_seq_cst);
                const u32 seen = wake.load(std::memory_order_seq_cst);
                available = published.load(std::memory_order_seq_cst);
                // A request made after the hook ran (RequestIdleHook) runs it before sleeping.
                if (available == read_position && !stop_token.stop_requested() &&
                    !hook_requested.load(std::memory_order_seq_cst)) {
                    wake.wait(seen, std::memory_order_seq_cst);
                }
                back_waiting.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        u64 last_store = read_position;
        while (read_position != available) {
            const size_t index = read_position & RingMask;
            const size_t size =
                Execute(&ring[index], std::min<u64>(available - read_position, RingWords - index));
            read_position += size != 0 ? size : RingWords - index;
            if (hook_requested.load(std::memory_order_relaxed) &&
                idle_hook != nullptr) [[unlikely]] {
                hook_requested.store(false, std::memory_order_seq_cst);
                idle_hook(idle_argument);
            }
            // Hand ring space back now and then while working through a long batch, and at once
            // when the front waits for this position (WaitForWriters).
            if ((read_position - last_store >= 4096 ||
                 read_position >= wait_target.load(std::memory_order_relaxed)) &&
                read_position != available) {
                consumed.store(read_position, std::memory_order_seq_cst);
                last_store = read_position;
                NotifyFront();
            }
        }
        consumed.store(read_position, std::memory_order_seq_cst);
        NotifyFront();
    }
    back_running.store(false, std::memory_order_seq_cst);
    front_wake.fetch_add(1, std::memory_order_seq_cst);
    front_wake.notify_all();
}

} // namespace VideoCommon
