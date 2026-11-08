// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string_view>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/rasterizer_download_area.h"

namespace VideoCore::ReadbackTrace {

using Clock = std::chrono::steady_clock;

inline bool Enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("STITCH_READBACK_TRACE");
        return value && std::string_view{value} == "1";
    }();
    return enabled;
}

inline u64 ElapsedNs(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
}

struct Flush {
    Clock::time_point began{}, ended{};
    u64 wait_ns{}, buffer_bytes{};
};

inline thread_local Flush* active{};

class Scope {
public:
    explicit Scope(Flush& flush) : previous{active} {
        active = &flush;
        flush.began = Clock::now();
    }
    ~Scope() {
        active->ended = Clock::now();
        active = previous;
    }
private:
    Flush* previous;
};

// Per GPU instance. Retain the first 128 distinct ranges per source in each window.
class Windows {
    struct Range {
        DAddr begin{}, end{};
        u64 reads{}, wall{};
    };
    struct Totals {
        u64 reads{}, bytes{}, queue{}, flush{}, wake{}, wait{}, buffer_bytes{}, overflow{};
        std::array<Range, 128> ranges{};
        size_t count{};
    };
public:
    void Add(const RasterizerDownloadArea& area, const Flush& event, Clock::time_point arrival,
             Clock::time_point returned) {
        std::scoped_lock lock{mutex};
        const auto now = Clock::now();
        if (since == Clock::time_point{}) {
            since = now;
        }
        auto& source = sources[static_cast<size_t>(area.source)];
        ++source.reads;
        source.bytes += area.end_address - area.start_address;
        source.buffer_bytes += event.buffer_bytes;
        source.queue += ElapsedNs(arrival, event.began);
        source.flush += ElapsedNs(event.began, event.ended);
        source.wake += ElapsedNs(event.ended, returned);
        source.wait += event.wait_ns;
        auto first = source.ranges.begin();
        auto last = first + source.count;
        auto found = std::find_if(first, last, [&](const Range& range) {
            return range.begin == area.start_address && range.end == area.end_address;
        });
        if (found != last || source.count < source.ranges.size()) {
            if (found == last) {
                *found = {.begin = area.start_address, .end = area.end_address};
                ++source.count;
            }
            ++found->reads;
            found->wall += ElapsedNs(arrival, returned);
        } else {
            ++source.overflow;
        }
        if (now - since < std::chrono::seconds{5}) {
            return;
        }
        constexpr std::array names{"other", "texture", "buffer"};
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto& t = sources[i];
            if (t.reads == 0) continue;
            const auto hottest = std::max_element(t.ranges.begin(), t.ranges.begin() + t.count,
                [](const Range& a, const Range& b) { return a.wall < b.wall; });
            LOG_INFO(HW_GPU,
                "Stitch readbacks window_ms={:.0f} source={} reads={} requested_bytes={} "
                "landed_buffer_bytes={} queue_ms={:.3f} flush_ms={:.3f} wake_ms={:.3f} "
                "vk_finish_wait_ms={:.3f} hottest=[{:#x},{:#x}) hot_reads={} hot_wall_ms={:.3f} untracked_reads={}",
                ElapsedNs(since, now) / 1.e6, names[i], t.reads, t.bytes, t.buffer_bytes,
                t.queue / 1.e6, t.flush / 1.e6, t.wake / 1.e6, t.wait / 1.e6,
                hottest->begin, hottest->end, hottest->reads, hottest->wall / 1.e6, t.overflow);
        }
        sources = {};
        since = now;
    }
private:
    std::mutex mutex;
    std::array<Totals, 3> sources{};
    Clock::time_point since{};
};

} // namespace VideoCore::ReadbackTrace
