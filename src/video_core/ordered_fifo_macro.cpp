// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/ordered_fifo_macro.h"

namespace VideoCommon {
namespace {
using MME = Tegra::TuringMacroInterpreter;

// A source can retain an input's identity through integer identities. bits bounds all one bits.
enum class Kind { Uninitialized, Constant, Linear, DifferenceWord, Computed };
struct Value {
    Kind kind{};
    u32 bits{};
    // For a Linear word, xor_terms identifies the original input words XORed together.
    // Input numbers 0–2 are fetched parameters, 3–8 are the independent FIFO words.
    u32 xor_terms{}, xor_constant{};
    bool operator==(const Value&) const = default;
};
Value Const(u32 value) {
    return {Kind::Constant, value};
}
Value Linear(u32 terms, u32 literal = 0) {
    return terms ? Value{Kind::Linear, ~0U, terms, literal} : Const(literal);
}
Value Input(u32 index) {
    return Linear(1U << index);
}
Value Unknown(u32 bits = ~0U) {
    return bits ? Value{Kind::Computed, bits} : Const(0);
}

struct State {
    std::array<Value, 24> regs{};
    u32 address{}, increment{}, consumed{}, emitted{};
    std::optional<bool> differs;
};

std::optional<Value> Source(u32 reg, unsigned lane, const MME::Decoded& d, const State& s,
                            const std::array<Value, 2>& loads) {
    if (reg < 24)
        return s.regs[reg].kind == Kind::Uninitialized ? std::nullopt : std::optional{s.regs[reg]};
    if (reg == 24)
        return Const(0);
    if (reg == 25 || reg == 26)
        return Const(static_cast<u32>(
            static_cast<s32>(static_cast<s16>(d.imm[reg == 25 ? lane : 1 - lane]))));
    if (reg == 27)
        return Const((d.imm[0] << 16) | d.imm[1]);
    if (reg == 28 || reg == 29)
        return loads[reg - 28];
    return std::nullopt;
}

std::optional<Value> Alu(u32 op, Value x, Value y) {
    // Only wrap-defined ADD, OR, XOR and the known sequential EXTENDED barrier. No later
    // instruction in this subset consumes the carry. Everything else uses the normal front.
    if (op == 7)
        return x == Const(0x1000) ? std::optional{Const(0)} : std::nullopt;
    if (op != 0 && op != 14 && op != 15)
        return std::nullopt;
    if (x == Const(0))
        return y;
    if (y == Const(0))
        return x;
    if (x.kind == Kind::Constant && y.kind == Kind::Constant) {
        if (op == 0)
            return Const(x.bits + y.bits);
        return Const(op == 14 ? x.bits | y.bits : x.bits ^ y.bits);
    }
    if (op == 15 && (x.kind == Kind::Linear || x.kind == Kind::Constant) &&
        (y.kind == Kind::Linear || y.kind == Kind::Constant)) {
        return Linear(x.xor_terms ^ y.xor_terms,
                      (x.kind == Kind::Constant ? x.bits : x.xor_constant) ^
                          (y.kind == Kind::Constant ? y.bits : y.xor_constant));
    }
    if (op == 14) {
        const Value lo = Linear((1U << 3) | (1U << 7));
        const Value hi = Linear((1U << 4) | (1U << 8));
        if ((x == lo && y == hi) || (x == hi && y == lo))
            return Value{Kind::DifferenceWord, ~0U};
    }
    return Unknown(op == 0 ? ~0U : (x.bits | y.bits));
}

std::optional<Value> Output(u32 sel, const MME::Decoded& d, const std::array<Value, 2>& alu,
                            const std::array<Value, 2>& loads) {
    if (sel == 1 || sel == 2)
        return alu[sel - 1];
    if (sel == 3 || sel == 4)
        return loads[sel - 3];
    if (sel == 5 || sel == 6)
        return Const(d.imm[sel - 5]);
    if (sel == 8 || sel == 9)
        return Const(d.imm[sel - 8] >> 12);
    if (sel == 10)
        return Const((d.imm[0] << 16) | d.imm[1]);
    return std::nullopt;
}

bool Advance(const MME::Decoded& d, State& s, u32 mask, const std::array<Value, 2>& loads,
             u32 render_mode) {
    std::array<Value, 2> alu{Const(0), Const(0)};
    for (unsigned i = 0; i < 2; ++i) {
        if (!(mask & (1U << i)))
            continue;
        auto x = Source(d.src_a[i], i, d, s, loads), y = Source(d.src_b[i], i, d, s, loads);
        if (!x || !y || d.dst[i] > 24)
            return false;
        auto v = Alu(d.op[i], *x, *y);
        if (!v)
            return false;
        alu[i] = *v;
    }
    for (unsigned i = 0; i < 2; ++i)
        if ((mask & (1U << i)) && d.dst[i] < 24)
            s.regs[d.dst[i]] = alu[i];
    for (unsigned i = 0; i < 2; ++i) {
        if (!(mask & (4U << i)))
            continue;
        if (d.out_method[i]) {
            const auto addr = Output(d.out_method[i], d, alu, loads);
            if (!addr || addr->kind != Kind::Constant)
                return false;
            s.address = addr->bits & 0xfff;
            s.increment = (addr->bits >> 12) & 15;
        }
        if (!d.out_emit[i])
            continue;
        const auto v = Output(d.out_emit[i], d, alu, loads);
        if (!v)
            return false;
        switch (s.emitted++) {
        case 0:
            if (s.address != 0x154 || *v != Input(1))
                return false;
            break;
        case 1:
            if (s.address != 0x155 || *v != Input(2))
                return false;
            break;
        case 2:
            if (s.address != 0x158 || *v != Const(6))
                return false;
            break;
        case 3:
            if (s.address != render_mode || !s.differs || *v != Linear(1, *s.differs ? 1 : 0))
                return false;
            break;
        default:
            return false;
        }
        s.address += s.increment;
    }
    return true;
}
} // namespace

std::optional<OrderedFifoMacro> OrderedFifoMacro::Certify(std::span<const u32> image, u32 start,
                                                          u32 render_mode) {
    if (start >= image.size() / 3)
        return std::nullopt;
    std::vector<State> paths(1), next;

    bool delay{};
    for (size_t i = 0; i < MaxInstructions && i < image.size() / 3 - start; ++i) {
        const size_t p = (static_cast<size_t>(start) + i) * 3;
        const auto d = MME::Decode({image[p + 2], image[p + 1], image[p]});
        next.clear();
        for (auto s : paths) {
            std::array<Value, 2> loads{Const(0), Const(0)};
            for (unsigned lane = 0; lane < 2; ++lane) {
                if (!d.load[lane])
                    continue;
                if (s.consumed >= (s.emitted >= 3 ? 9U : 3U))
                    return std::nullopt;
                loads[lane] = Input(s.consumed);
                ++s.consumed;
            }
            std::array<u32, 2> masks{15, 15};
            size_t count = 1;
            bool test_difference{};
            if (d.pred_mode) {
                const auto v = Source(d.pred_reg, 0, d, s, loads);
                if (!v)
                    return std::nullopt;
                if (v->kind == Kind::Constant)
                    masks[0] = v->bits ? d.mask_true : d.mask_false;
                else {
                    masks = {d.mask_true, d.mask_false};
                    test_difference = v->kind == Kind::DifferenceWord;
                    count = test_difference || masks[0] != masks[1] ? 2 : 1;
                }
            }
            for (size_t k = 0; k < count; ++k) {
                State branch = s;
                if (test_difference) {
                    const bool nonzero = k == 0;
                    if (branch.differs && *branch.differs != nonzero)
                        continue;
                    branch.differs = nonzero;
                }
                if (next.size() >= 16 || !Advance(d, branch, masks[k], loads, render_mode))
                    return std::nullopt;
                next.push_back(branch);
            }
        }
        paths.swap(next);
        if (delay) {
            if (!std::ranges::all_of(
                    paths, [](const auto& s) { return s.emitted == 4 && s.consumed == 9; }))
                return std::nullopt;
            OrderedFifoMacro plan;
            plan.size = static_cast<unsigned>((i + 1) * 3);
            std::copy_n(image.begin() + static_cast<size_t>(start) * 3, plan.size,
                        plan.code.begin());
            return plan;
        }
        delay = d.end;
    }
    return std::nullopt;
}
} // namespace VideoCommon
