// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include "common/common_types.h"
#include "dynarmic/interface/code_page.h"

namespace Core {

class InstructionPageCache {
public:
    /// May run on a different core while the owner is filling its page.
    void Invalidate() {
        generation.fetch_add(1, std::memory_order_release);
    }

    /// Called only by the owner on leaving warm-up or at a local instruction barrier.
    void ResetLocal() {
        address = ~u64{0};
    }

    template <typename Fill>
    u32 Read(u64 vaddr, Fill&& fill) {
        constexpr u64 mask = Dynarmic::CODE_PAGE_SIZE - 1;
        const u64 base = vaddr & ~mask;
        const u64 before_fill = generation.load(std::memory_order_acquire);
        if (base != address || before_fill != cached_generation) {
            fill(base, page);
            address = base;
            // An invalidation during fill must still force the next read to refetch.
            cached_generation = before_fill;
        }
        return page.inst[(vaddr & mask) / sizeof(u32)];
    }

private:
    std::atomic<u64> generation{};
    u64 cached_generation{};
    u64 address{~u64{0}};
    Dynarmic::CodePage page;
};

} // namespace Core
