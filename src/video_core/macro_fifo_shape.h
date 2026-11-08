// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <span>
#include <string_view>
#include "common/common_types.h"
#include "video_core/macro_turing.h"

namespace VideoCommon {

// Diagnostic only. Classify a particularly simple physical method stream without running it.
// Every branch, predicate and dynamic method selection is rejected, including in the END delay
// slot. A matching shape does not establish register ownership, valid parameters, or safe
// scheduling.
inline std::string_view MacroFifoShape(std::span<const u32> code, u32 start, u32 render_mode) {
    using Interpreter = Tegra::TuringMacroInterpreter;
    constexpr size_t max_steps = 64;
    u32 method{}, increment{}, initialized{};
    bool high{}, low{}, fifo{}, last{};
    for (size_t step = 0; step < max_steps; ++step) {
        if (size_t{start} >= code.size() / 3 || step >= code.size() / 3 - start)
            return "missing-end";
        const auto pos = (size_t{start} + step) * 3;
        // Native instruction RAM stores the most significant word first (see Execute).
        const auto d = Interpreter::Decode({code[pos + 2], code[pos + 1], code[pos]});
        if (d.pred_mode)
            return "predicate";
        for (u32 i = 0; i < 2; ++i) {
            // EXTENDED requires x == 0x1000. This classifier does not track ALU values.
            if (d.op[i] == 7)
                return "extended";
            if (d.op[i] == 16 && ((d.imm[i] >> 10) & 63) >= 32)
                return "invalid-merge";
            if (d.op[i] == 22)
                return "engine-register-read";
            if (d.op[i] == 30 || d.op[i] == 31)
                return "data-ram";
            if (d.op[i] >= 23)
                return "control-flow";
            for (const u32 src : {d.src_a[i], d.src_b[i]}) {
                if (src > 29 || (src < 24 && !(initialized & (1U << src))))
                    return "unproven-register";
            }
            if (d.dst[i] > 24)
                return "unproven-register";
        }
        for (u32 i = 0; i < 2; ++i) {
            if (d.dst[i] < 24)
                initialized |= 1U << d.dst[i];
            const auto select = d.out_method[i];
            if (select) {
                if (select != 5 && select != 6)
                    return "dynamic-method";
                method = d.imm[select - 5] & 0xfff;
                increment = d.imm[select - 5] >> 12;
            }
            if (!d.out_emit[i])
                continue;
            if (d.out_emit[i] == 7 || d.out_emit[i] > 10)
                return "invalid-selector";
            if (method == 0x154)
                high = true;
            else if (method == 0x155)
                low = true;
            else if (method == 0x157)
                return "dma-to-ram";
            else if (method == 0x158) {
                if (!high || !low)
                    return "incoming-dma-address";
                fifo = true;
            } else if (method != render_mode)
                return "other-engine-effect";
            method += increment;
        }
        if (last)
            return fifo ? "linear-fifo-shape" : "no-fifo";
        last = d.end;
    }
    return "instruction-bound";
}
} // namespace VideoCommon
