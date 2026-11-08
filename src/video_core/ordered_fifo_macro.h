// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <vector>
#include "common/common_types.h"
#include "video_core/macro_turing.h"

namespace VideoCommon {

// Certify a narrow effect shape, independent of method/start/hash. Native execution still
// computes the result. Unknown predicates explore both outcomes; a failure in either rejects.
struct OrderedFifoMacro {
    static constexpr unsigned MaxInstructions = 16;
    using Params = std::array<u32, 3>;
    using Data = std::array<u32, 6>;
    using Events = std::array<std::pair<u32, u32>, 4>;
    // The executable copied into a ring entry after the proof below succeeds. It is a value,
    // independent of native RAM and all future uploads/rebinds.
    struct Recipe {
        u32 Result(const Params& p, const Data& words) const {
            const bool differs = words[0] != words[4] || words[1] != words[5];
            return differs ? p[0] ^ 1U : p[0];
        }
    };
    Recipe recipe{};
    std::array<u32, MaxInstructions * 3> code{};
    unsigned size{};

    static std::optional<OrderedFifoMacro> Certify(std::span<const u32> image, u32 start,
                                                   u32 render_mode);

    // Uses the real interpreter after the same six actual words have been materialized. The
    // caller's dedicated scratch is reused; the certificate prohibits all data-RAM access.
    bool Evaluate(Tegra::TuringMacroInterpreter& mme, std::vector<u32>& fifo, const Params& params,
                  const Data& data, Events& events) const {
        fifo.assign(params.begin(), params.end());
        size_t n{};
        const auto result = mme.Execute(
            std::span{code}.first(size), 0, fifo, [](u32) { return u32{}; },
            [&](u32 method, u32 value) {
                if (n == events.size())
                    return false;
                events[n++] = {method, value};
                if (method == 0x158)
                    fifo.insert(fifo.end(), data.begin(), data.end());
                return true;
            });
        return result.success && result.consumed == 9 && n == events.size();
    }
};
} // namespace VideoCommon
