// SPDX-License-Identifier: GPL-3.0-or-later
#include <random>
#include <catch2/catch_test_macros.hpp>
#include "tests/video_core/ordered_fifo_program.h"
#include "video_core/ordered_fifo_macro.h"

TEST_CASE("Ordered FIFO recipe agrees with the original interpreter on arbitrary fetched words",
          "[ordered_fifo]") {
    using Plan = VideoCommon::OrderedFifoMacro;
    constexpr u32 mode = 0x556;
    auto native = OrderedFifoTest::Encode(OrderedFifoTest::Instructions(mode));
    const auto plan = Plan::Certify(native, 0, mode);
    REQUIRE(plan);
    std::mt19937 random{717};
    Tegra::TuringMacroInterpreter mme;
    std::vector<u32> fifo;
    for (u32 i = 0; i < 257; ++i) {
        Plan::Params p{};
        Plan::Data report{};
        for (auto& word : p)
            word = static_cast<u32>(random());
        for (auto& word : report)
            word = static_cast<u32>(random());
        if (i & 1)
            report[4] = report[0];
        if (i & 2)
            report[5] = report[1];
        Plan::Events observed;
        REQUIRE(plan->Evaluate(mme, fifo, p, report, observed));
        const Plan::Events expected{
            {{0x154, p[1]}, {0x155, p[2]}, {0x158, 6}, {mode, plan->recipe.Result(p, report)}}};
        REQUIRE(observed == expected);
    }
    // Changing the original image after the executable was captured cannot change it.
    std::fill(native.begin(), native.end(), 0);
    REQUIRE_FALSE(Plan::Certify(native, 0, mode));
    Plan::Events output;
    REQUIRE(plan->Evaluate(mme, fifo, {1, 2, 3}, {1, 2, 3, 4, 1, 2}, output));
    REQUIRE(output.back() == std::pair<u32, u32>{mode, 1});
}
TEST_CASE("Ordered FIFO proof rejects changes to either predicate path and the END delay",
          "[ordered_fifo]") {
    using namespace OrderedFifoTest;
    const auto check = [](auto change) {
        auto ins = Instructions(0x556);
        change(ins);
        REQUIRE_FALSE(VideoCommon::OrderedFifoMacro::Certify(Encode(ins), 0, 0x556));
    };
    check([](auto& p) { p[4].imm = 0; });
    check([](auto& p) { p[4].a.x = 28; });
    check([](auto& p) { p[3].imm2 = 5; });
    check([](auto& p) { p[3].imm = 0x157; });
    check([](auto& p) { p[1].output = 5; });
    check([](auto& p) { p[5].b.x = 28; });
    check([](auto& p) { p[8].a.op = 15; });
    check([](auto& p) { p[9].pred = 1; });
    check([](auto& p) { p[9].a.op = 14; });
    check([](auto& p) { p.back().a.op = 22; });
    check([](auto& p) { p.back().a.op = 30; });
    check([](auto& p) { p.back().a.op = 24; });
    check([](auto& p) { p.back().imm = 0x554; });
    check([](auto& p) { p.pop_back(); });
}
