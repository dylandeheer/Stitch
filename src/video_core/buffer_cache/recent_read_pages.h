// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include "common/common_types.h"

namespace VideoCommon {

// Optional co-downloads only. The caller always services the entire faulting range first.
class RecentReadPages {
public:
    static constexpr u64 PageSize = 4096;
    static constexpr size_t Capacity = 8;
    static constexpr u64 MaxAge = 2;

    void Read(DAddr begin, DAddr end, u64 frame) {
        if (end <= begin || end - begin != PageSize || begin % PageSize != 0) {
            return;
        }
        const auto same = std::find_if(pages.begin(), pages.end(),
                                       [begin](const Page& p) { return p.valid && p.base == begin; });
        const auto last = same == pages.end() ? pages.end() - 1 : same;
        std::move_backward(pages.begin(), last, last + 1);
        pages.front() = {begin, frame, true};
    }

    template <class Visit>
    void ForEachExtra(DAddr begin, u64 size, u64 frame, Visit visit) const {
        for (const auto& p : pages) {
            if (!p.valid || frame - p.frame > MaxAge) {
                continue;
            }
            const bool overlaps = p.base >= begin ? p.base - begin < size : begin - p.base < PageSize;
            if (!overlaps) {
                visit(p.base, PageSize);
            }
        }
    }

    void Forget(DAddr begin, u64 size) {
        for (auto& p : pages) {
            if (p.valid && (p.base >= begin ? p.base - begin < size : begin - p.base < PageSize)) {
                p.valid = false;
            }
        }
    }

private:
    struct Page { DAddr base; u64 frame; bool valid; };
    std::array<Page, Capacity> pages{};
};

} // namespace VideoCommon
