// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>

#include "common/common_types.h"

namespace Core {

// One buffer cache serializes its landings. Separate systems have separate gates.
class DownloadLanding {
public:
    class Scope {
    public:
        Scope(DownloadLanding& gate_, DAddr begin, DAddr end) : gate{gate_} {
            gate.begin.store(begin, std::memory_order_relaxed);
            gate.end.store(end, std::memory_order_relaxed);
            gate.sequence.fetch_add(1, std::memory_order_seq_cst);
            // Publish before inspecting pending CPU writes on every guest core.
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        ~Scope() {
            gate.sequence.fetch_add(1, std::memory_order_release);
            gate.sequence.notify_all();
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        DownloadLanding& gate;
    };

    // Called after publishing the guest write and before storing it. Either the
    // snapshot contains the write or its store follows the overlapping landing.
    void Await(DAddr address, u64 size) const {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const u64 active = sequence.load(std::memory_order_acquire);
        if ((active & 1) == 0) {
            return;
        }
        const DAddr first = begin.load(std::memory_order_relaxed);
        const DAddr last = end.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sequence.load(std::memory_order_acquire) != active) {
            return;
        }
        if (address < last && (address >= first || size > first - address)) {
            waits.fetch_add(1, std::memory_order_relaxed);
            sequence.wait(active, std::memory_order_acquire);
        }
    }

    [[nodiscard]] u64 WaitCount() const {
        return waits.load(std::memory_order_relaxed);
    }

private:
    mutable std::atomic<u64> waits{};
    std::atomic<u64> sequence{};
    std::atomic<DAddr> begin{};
    std::atomic<DAddr> end{};
};

} // namespace Core
