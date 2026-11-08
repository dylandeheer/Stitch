// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <vector>
#include "common/common_types.h"

namespace OrderedFifoTest {
// Synthetic program spelling out the public two-counter comparison in twelve instructions.
// It is intentionally a different instruction sequence from the private target.
struct Alu {
    u32 op{}, dst{24}, x{24}, y{24};
};
struct Instruction {
    Alu a{}, b{};
    u32 imm{}, imm2{}, method{}, output{}, pred{}, predicate{24};
    bool end{};
    std::array<u32, 3> Encode() const {
        std::array<u32, 3> bits{};
        const auto field = [&](u32 n, u32 width, u32 value) {
            for (u32 i = 0; i < width; ++i)
                bits[(n + i) / 32] |= ((value >> i) & 1) << ((n + i) % 32);
        };
        field(0, 1, end);
        field(1, 4, pred);
        field(5, 5, predicate);
        for (u32 i = 0; i < 2; ++i) {
            const auto& lane = i ? b : a;
            field(10 + i * 36, 5, lane.op);
            field(15 + i * 36, 5, lane.dst);
            field(20 + i * 36, 5, lane.x);
            field(25 + i * 36, 5, lane.y);
        }
        field(30, 16, imm);
        field(66, 16, imm2);
        field(82, 3, method);
        field(85, 4, output);
        return {bits[2], bits[1], bits[0]};
    }
};
inline std::vector<Instruction> Instructions(u32 mode) {
    return {
        {.a = {0, 0, 28, 24}},
        {.imm = 0x154, .method = 5, .output = 3},
        {.imm = 0x155, .method = 5, .output = 3},
        {.imm = 0x158, .imm2 = 6, .method = 5, .output = 6},
        {.a = {7, 24, 25, 24}, .imm = 0x1000},
        {.a = {0, 1, 28, 24}, .b = {0, 2, 29, 24}},
        {.a = {0, 24, 28, 29}},
        {.a = {15, 1, 1, 28}, .b = {15, 2, 2, 29}},
        {.a = {14, 3, 1, 2}},
        {.a = {15, 0, 0, 25}, .imm = 1, .pred = 2, .predicate = 3},
        {.end = true},
        {.a = {0, 24, 0, 24}, .imm = mode, .method = 5, .output = 1},
    };
}
inline std::vector<u32> Encode(const std::vector<Instruction>& instructions) {
    std::vector<u32> words;
    for (const auto& i : instructions) {
        const auto bits = i.Encode();
        words.insert(words.end(), bits.begin(), bits.end());
    }
    return words;
}
} // namespace OrderedFifoTest
