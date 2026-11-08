// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <utility>

#include "common/logging.h"
#include "common/thread_cpu_clock.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"

namespace Vulkan {

// Counters are owned by the single split-back executor. Present/channel notifications only
// publish small atomics. Instrumented helpers on another thread cannot see the sampled draw.
class DrawCpuProbe {
public:
    enum class Zone : size_t {
        Preflush,
        Stages,
        Fixed,
        Lookup,
        Geometry,
        StageBuffers,
        Targets,
        ConfigureEnqueue,
        DrawRecord,
        Coverage,
        Count
    };
    static constexpr size_t Zones = static_cast<size_t>(Zone::Count);
    using Times = std::array<u64, Zones>;
    using CpuClock = std::optional<u64> (*)();
    explicit DrawCpuProbe(const char* path_, CpuClock cpu_ = Common::CurrentThreadCpuTimeNs)
        : path{path_}, cpu{cpu_} {}
    void Present() {
        presents.fetch_add(1, std::memory_order_relaxed);
    }
    void Bind(s32 id) {
        channel.store(id, std::memory_order_relaxed);
    }

    class Scope {
    public:
        explicit Scope(Zone zone_) : probe{sample}, zone{zone_} {
            if (probe) {
                if (std::exchange(probe->inside_zone, true))
                    probe->valid = false;
                start = probe->Read();
            }
        }
        ~Scope() {
            if (probe) {
                const u64 end = probe->Read();
                if (end >= start)
                    probe->times[static_cast<size_t>(zone)] += end - start;
                else
                    probe->valid = false;
                probe->inside_zone = false;
            }
        }
        Scope(const Scope&) = delete;

    private:
        DrawCpuProbe* probe;
        Zone zone;
        u64 start{};
    };

    bool Begin(u64 tick, u64 epoch, bool in_pass) {
        if (finished)
            return false;
        if (open) {
            valid = false;
            interrupted = true;
            return false;
        }
        const u64 frame = presents.load(std::memory_order_relaxed);
        if (active && (frame != last_frame || (attempts & 1023) == 0) && Clock::now() >= deadline) {
            Finish();
            return false;
        }
        if (frame != last_frame) {
            last_frame = frame;
            if (active) {
                previous_known = false; // never chain across presentations
            } else if (frame >= next_poll) {
                next_poll = frame + 16;
                std::error_code ec;
                if (std::filesystem::exists(path, ec) && !ec) {
                    active = true;
                    deadline = Clock::now() + std::chrono::seconds(10);
                    LOG_INFO(Render_Vulkan, "Draw CPU window started frame={}", frame);
                }
            }
        }
        if (!active)
            return false;
        open = true;
        interrupted = false;
        seen = false;
        row = &unknown;
        entry_tick = tick;
        entry_epoch = epoch;
        entry_pass = in_pass;
        const auto n = attempts++;
        sampled = (n & 127) == (((n >> 7) * 73) & 127);
        if (sampled) {
            valid = true;
            inside_zone = false;
            times = {};
            sample = this;
            begin = Read();
        }
        return true;
    }

    void Found(const std::array<u64, 6>& hashes) {
        Scope coverage{Zone::Coverage};
        if (last_row && last_row->hashes == hashes) {
            row = last_row;
            return;
        }
        for (size_t i = 0; i < used; ++i) {
            if (rows[i].hashes == hashes) {
                row = last_row = &rows[i];
                return;
            }
        }
        if (used == rows.size()) {
            row = &overflow;
            return;
        }
        row = last_row = &rows[used++];
        row->hashes = hashes;
    }

    void Observe(const GraphicsPipelineCacheKey& key, bool indexed, bool indirect,
                 bool regular_no_images, bool guard_ok, u64 tick, u64 epoch, bool in_pass) {
        Scope coverage{Zone::Coverage};
        const s32 ch = channel.load(std::memory_order_relaxed);
        const unsigned kind =
            static_cast<unsigned>(indexed) | (static_cast<unsigned>(indirect) << 1);
        const bool shape = !indirect && regular_no_images && guard_ok && in_pass && ch >= 0;
        ++row->observed;
        row->direct_no_images += !indirect && regular_no_images;
        row->shape += shape;
        if (previous_known) {
            ++row->pairs;
            const bool same = previous_key == key;
            const bool same_ch = ch >= 0 && ch == previous_channel;
            const bool same_kind = kind == previous_kind;
            const bool same_pass = previous_pass && entry_pass && in_pass &&
                                   previous_epoch == entry_epoch && epoch == entry_epoch &&
                                   previous_tick == entry_tick && tick == entry_tick;
            row->same_key += same;
            row->same_channel += same_ch;
            row->same_kind += same_kind;
            row->same_pass += same_pass;
            row->repeat += same && same_ch && same_kind && same_pass && shape && previous_shape;
            if (!same)
                previous_key = key;
        } else {
            previous_key = key;
        }
        seen = true;
        previous_shape = shape;
        previous_pass = in_pass;
        previous_channel = ch;
        previous_kind = kind;
        previous_tick = tick;
        previous_epoch = epoch;
    }

    void End(bool submitted) {
        ++row->draws;
        row->submitted += submitted;
        previous_known = submitted && seen && !interrupted;
        if (sampled) {
            const u64 end = Read();
            sample = nullptr;
            u64 zoned{};
            for (u64 t : times)
                zoned += t;
            if (valid && !inside_zone && end >= begin && end - begin >= zoned) {
                ++row->samples;
                row->total += end - begin;
                for (size_t i = 0; i < Zones; ++i)
                    row->ns[i] += times[i];
            } else
                ++row->invalid;
        }
        open = false;
    }

private:
    using Clock = std::chrono::steady_clock;
    struct Row {
        std::array<u64, 6> hashes{};
        u64 draws{}, submitted{}, observed{}, direct_no_images{}, shape{}, pairs{}, same_key{},
            same_channel{}, same_kind{}, same_pass{}, repeat{}, samples{}, invalid{}, total{};
        Times ns{};
    };
    u64 Read() {
        const auto t = cpu();
        if (!t)
            valid = false;
        return t.value_or(0);
    }
    void Finish() {
        finished = true;
        active = false;
        const auto report = [&](const Row& r, const char* type) {
            if (!r.draws)
                return;
            u64 accounted{};
            for (const auto time : r.ns)
                accounted += time;
            LOG_INFO(Render_Vulkan,
                     "Draw CPU row={} stages={:016x},{:016x},{:016x},{:016x},{:016x},{:016x} "
                     "attempts={} submitted={} observed={} direct_regular_no_images={} "
                     "basic_shape={} pairs={} same_key={} same_channel={} same_kind={} "
                     "same_pass_tick={} repeat_shape={} samples={} invalid={} total_cpu_ns={} "
                     "preflush={} stages={} fixed={} pipeline_lookup={} geometry={} "
                     "stage_buffers={} rt_feedback={} configure_enqueue={} draw_record={} "
                     "coverage={} remainder={}",
                     type, r.hashes[0], r.hashes[1], r.hashes[2], r.hashes[3], r.hashes[4],
                     r.hashes[5], r.draws, r.submitted, r.observed, r.direct_no_images, r.shape,
                     r.pairs, r.same_key, r.same_channel, r.same_kind, r.same_pass, r.repeat,
                     r.samples, r.invalid, r.total, r.ns[0], r.ns[1], r.ns[2], r.ns[3], r.ns[4],
                     r.ns[5], r.ns[6], r.ns[7], r.ns[8], r.ns[9], r.total - accounted);
        };
        for (size_t i = 0; i < used; ++i)
            report(rows[i], "group");
        report(overflow, "overflow");
        report(unknown, "unknown");
        LOG_INFO(Render_Vulkan, "Draw CPU window complete attempts={} groups={} frame={}", attempts,
                 used, last_frame);
    }
    static inline thread_local DrawCpuProbe* sample{};
    std::filesystem::path path;
    CpuClock cpu;
    std::atomic<u64> presents{};
    std::atomic<s32> channel{-1};
    Clock::time_point deadline{};
    u64 last_frame{~u64{0}}, next_poll{}, attempts{}, begin{}, entry_tick{}, entry_epoch{},
        previous_tick{}, previous_epoch{};
    s32 previous_channel{-1};
    unsigned previous_kind{};
    bool active{}, finished{}, open{}, interrupted{}, sampled{}, valid{}, inside_zone{}, seen{},
        previous_known{}, previous_shape{}, previous_pass{}, entry_pass{};
    Times times{};
    GraphicsPipelineCacheKey previous_key{};
    std::array<Row, 64> rows{};
    Row unknown{}, overflow{};
    Row *row{}, *last_row{};
    size_t used{};
};
} // namespace Vulkan
