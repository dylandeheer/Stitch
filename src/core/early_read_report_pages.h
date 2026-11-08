// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include "common/common_types.h"

namespace Core {

inline bool EarlyBufferReadsRequested() {
    static const bool enabled = [] {
        const char* marker = std::getenv("STITCH_EARLY_READ_BASELINE_FILE");
        return marker && *marker;
    }();
    return enabled;
}

// Never read a semaphore/query page from a speculative copy. Physical page tags are
// monotonic per device; hash collisions can only disqualify another page.
class EarlyReadReportPages {
public:
    void Exclude(u32 physical_page) {
        const u32 bit = Hash(physical_page);
        words[bit / 64].fetch_or(u64{1} << (bit % 64), std::memory_order_release);
    }
    bool Excluded(u32 physical_page) const {
        const u32 bit = Hash(physical_page);
        return words[bit / 64].load(std::memory_order_acquire) & (u64{1} << (bit % 64));
    }

private:
    static u32 Hash(u32 page) {
        return (page * 0x9e3779b9U) >> 16;
    }
    std::array<std::atomic<u64>, 1024> words{};
};

} // namespace Core
