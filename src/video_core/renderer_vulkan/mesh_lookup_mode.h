// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <filesystem>
#include <string_view>

#include "common/common_types.h"
#include "common/logging.h"

namespace Vulkan {

class MeshLookupMode {
public:
    void Initialize(const char* setting, const char* baseline_path) {
        enabled = setting && std::string_view{setting} == "1";
        if (enabled) {
            marker = baseline_path ? baseline_path : "";
            bounded.store(ReadMarker(), std::memory_order_relaxed);
        }
    }
    bool Enabled() const {
        return enabled;
    }
    bool Bounded() const {
        return bounded.load(std::memory_order_relaxed);
    }
    void Built(bool success) {
        attempted.fetch_add(1, std::memory_order_relaxed);
        if (success)
            ready.fetch_add(1, std::memory_order_relaxed);
    }
    void Bound(bool selected) {
        (selected ? bounded_binds : original_binds).fetch_add(1, std::memory_order_relaxed);
    }
    void TickFrame(u64 frame) {
        if (!enabled)
            return;
        const bool old = Bounded();
        const bool next = frame % 16 == 0 ? ReadMarker() : old;
        if (next != old || frame == 1 || frame % 120 == 0) {
            bounded.store(next, std::memory_order_relaxed);
            LOG_INFO(Render_Vulkan,
                     "Mesh lookup frame={} prior={} next={} eligible={} ready={} "
                     "original_binds={} bounded_binds={}",
                     frame, old ? "bounded" : "original", next ? "bounded" : "original",
                     attempted.load(std::memory_order_relaxed),
                     ready.load(std::memory_order_relaxed),
                     original_binds.exchange(0, std::memory_order_relaxed),
                     bounded_binds.exchange(0, std::memory_order_relaxed));
        }
    }

private:
    bool ReadMarker() const {
        if (marker.empty())
            return true;
        std::error_code ec;
        return !std::filesystem::exists(marker, ec) && !ec;
    }
    std::filesystem::path marker;
    bool enabled{};
    std::atomic_bool bounded{};
    std::atomic<u64> attempted{}, ready{}, bounded_binds{}, original_binds{};
};

} // namespace Vulkan
