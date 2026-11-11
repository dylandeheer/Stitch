// SPDX-License-Identifier: GPL-3.0-or-later

#include <latch>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/instruction_page_cache.h"

TEST_CASE("Concurrent instruction invalidation survives an in-flight JIT page fill",
          "[jit_warmup][instruction_page]") {
    Core::InstructionPageCache cache;
    std::latch copied{1}, invalidated{1};
    u32 backing_word = 11;
    std::thread other_core([&] {
        copied.wait();
        backing_word = 22;
        cache.Invalidate();
        invalidated.count_down();
    });
    size_t fills{};
    const auto fill = [&](u64 base, Dynarmic::CodePage& page) {
        page.inst[3] = backing_word + static_cast<u32>(base);
        if (++fills == 1) {
            copied.count_down();
            invalidated.wait();
        }
    };
    const u32 first = cache.Read(12, fill);
    other_core.join();
    REQUIRE(first == 11);
    REQUIRE(cache.Read(12, fill) == 22);
    REQUIRE(fills == 2);
    REQUIRE(cache.Read(12, fill) == 22);
    REQUIRE(fills == 2);
    REQUIRE(cache.Read(Dynarmic::CODE_PAGE_SIZE + 12, fill) == 22 + Dynarmic::CODE_PAGE_SIZE);
    REQUIRE(fills == 3);
    cache.ResetLocal();
    REQUIRE(cache.Read(Dynarmic::CODE_PAGE_SIZE + 12, fill) == 22 + Dynarmic::CODE_PAGE_SIZE);
    REQUIRE(fills == 4);
}
