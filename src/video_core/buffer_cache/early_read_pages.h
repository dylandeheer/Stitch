// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>

#include "common/common_types.h"

namespace VideoCommon {

// All fields are owned by the buffer-cache mutex. Staging allocations remain deferred
// until the issuing GPU tick completes, including canceled, replaced and disabled cases.
template <typename BufferId, typename Staging>
struct EarlyReadPages {
    static constexpr u64 PageSize = 4096;
    static constexpr size_t Learned = 64, Pending = 32;
    struct Page {
        DAddr address{};
        u64 version{};
        BufferId writer{};
        u8 reads_after_write{}, missed{};
        bool valid{}, compute{}, read{}, requested{};
    };
    struct Copy {
        DAddr page{};
        u64 version{}, generation{}, tick{}, instance{};
        BufferId buffer{};
        Staging staging;
        bool canceled{};
    };
    struct Stats {
        u64 copies{}, bytes{}, hits{}, hit_bytes{}, cpu_only{}, late{}, unsubmitted{},
            ready_checks{}, late_checks{}, stale{}, blocked{}, comparisons{}, compared_bytes{},
            masked_bytes{}, empty_checks{}, remapped_checks{}, noncoherent_reference{},
            unavailable{}, mismatches{}, caps{};
    };
    struct Shadow {
        DAddr page;
        std::array<u8, PageSize> original{};
        std::bitset<PageSize> readable{};
        size_t masked{};
        bool uncertain{};
    };
    std::array<Page, Learned> pages{};
    std::array<std::optional<Copy>, Pending> copies{};
    const std::filesystem::path marker;
    const bool verify;
    bool active{}, failed{};
    u64 next_version{}, next_buffer_instance{}, frame_bytes{}, generation{};
    Stats stats{};
    Shadow* shadow{};

    EarlyReadPages(std::filesystem::path marker_, bool verify_)
        : marker{std::move(marker_)}, verify{verify_} {}

    void Poll() {
        std::error_code error;
        const bool next = !failed && !std::filesystem::exists(marker, error) && !error;
        if (active != next) {
            Reset();
            active = next;
        }
    }
    void Reset() {
        pages = {};
        for (auto& copy : copies)
            if (copy)
                copy->canceled = true;
    }
    void ObserveGeneration(u64 gen) {
        if (generation == gen)
            return;
        Reset();
        generation = gen;
    }
    Page& Slot(DAddr page) {
        const u64 hash = (page >> 12) * 0x9e3779b97f4a7c15ULL;
        return pages[hash >> 58];
    }
    void Demand(DAddr page) {
        auto& p = Slot(page);
        if (!p.valid || p.address != page) {
            p = {.address = page, .version = ++next_version, .valid = true};
        }
        if (p.compute && !p.read) {
            if (p.reads_after_write < 2)
                ++p.reads_after_write;
            p.missed = 0;
            p.read = true;
        }
    }
    void Write(DAddr start, u64 size, BufferId writer = {}, bool compute = false) {
        if (!active || !size)
            return;
        const auto update = [&](Page& p) {
            if (!p.valid ||
                !(start <= p.address ? p.address - start < size : start - p.address < PageSize))
                return;
            if (compute) {
                if (p.compute && !p.read && p.missed < 4)
                    ++p.missed;
                if (p.missed == 4)
                    p.reads_after_write = 0;
            }
            p.version = ++next_version;
            p.writer = writer;
            p.compute = compute;
            p.read = false;
            p.requested = compute && p.reads_after_write >= 2;
        };
        if (size <= 16 * PageSize && start <= ~DAddr{} - size) {
            const DAddr last = (start + size - 1) >> 12;
            for (DAddr i = start >> 12; i <= last; ++i) {
                auto& p = Slot(i << 12);
                if (p.valid && p.address == i << 12)
                    update(p);
            }
        } else {
            for (auto& p : pages)
                update(p);
        }
    }
    bool Current(const Copy& c) {
        const auto& p = Slot(c.page);
        return !c.canceled && p.valid && p.address == c.page && p.version == c.version &&
               c.generation == generation && p.writer == c.buffer;
    }
    void Compare(DAddr address, const u8* data, size_t size) {
        if (!shadow || !size)
            return;
        const DAddr start = std::max(address, shadow->page);
        const DAddr end = std::min(address + size, shadow->page + PageSize);
        if (start >= end)
            return;
        const auto offset = start - shadow->page;
        std::memcpy(shadow->original.data() + offset, data + (start - address), end - start);
        for (size_t i = offset; i < offset + (end - start); ++i)
            shadow->readable.set(i);
    }
};
} // namespace VideoCommon
