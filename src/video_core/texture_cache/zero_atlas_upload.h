// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <limits>
#include <optional>

#include "video_core/texture_cache/image_base.h"
#include "video_core/texture_cache/util.h"

namespace VideoCommon {

inline bool IsFirstAtlasUpload(bool initialized, ImageFlagBits flags) {
    // Tracking was enabled just before RefreshContents. A registered image can already have views.
    return !initialized && False(flags & ~ImageFlagBits::Tracked);
}

// Require a tight scalar atlas footprint on both sides: successive guest byte intervals and
// consecutive horizontal fragments of complete, equally tall bands at level/layer zero.
inline bool CoversWholeAtlas(std::span<const UnswizzlePlan> plans, GPUVAddr base, u64 size,
                             u32 width, u32 height, PixelFormat format) {
    if (format != PixelFormat::D16_UNORM && format != PixelFormat::R32_FLOAT)
        return false;
    const u32 bpp_log2 = format == PixelFormat::D16_UNORM ? 1 : 2;
    const u32 texel_bytes = 1U << bpp_log2;
    if (!width || !height || !size || size % texel_bytes ||
        size / texel_bytes != u64{width} * height ||
        size > std::numeric_limits<u64>::max() - base) {
        return false;
    }
    u64 consumed{};
    u32 x{}, y{}, band_height{};
    for (const auto& plan : plans) {
        if (!plan.layout_valid || plan.bpp_log2 != bpp_log2 || plan.runs.empty() ||
            plan.runs.size() != plan.copies.size()) {
            return false;
        }
        u64 host{};
        for (size_t i = 0; i < plan.runs.size(); ++i) {
            const auto& run = plan.runs[i];
            const auto& copy = plan.copies[i];
            const auto extent = copy.image_extent;
            const auto sub = copy.image_subresource;
            if (x == 0)
                band_height = extent.height;
            if (sub.base_level || sub.base_layer || sub.num_layers != 1 ||
                copy.image_offset != Offset3D{static_cast<s32>(x), static_cast<s32>(y), 0} ||
                extent.depth != 1 || !extent.width || !extent.height ||
                extent.height != band_height || extent.width > width - x ||
                extent.height > height - y || copy.buffer_row_length != extent.width ||
                copy.buffer_image_height != extent.height || run.run_tiles != extent.width ||
                run.tile_rows != extent.height || run.src_addr != base + consumed ||
                !run.guest_bytes || run.guest_bytes > (4ULL << 20) ||
                run.guest_bytes > size - consumed || run.host_offset != host ||
                copy.buffer_offset != host || run.host_bytes != run.guest_bytes ||
                run.host_bytes != copy.buffer_size ||
                run.host_bytes != u64{extent.width} * extent.height * texel_bytes) {
                return false;
            }
            host += run.host_bytes;
            consumed += run.guest_bytes;
            x += extent.width;
            if (x == width) {
                x = 0;
                y += extent.height;
            }
        }
        if (host != plan.total_host_bytes)
            return false;
    }
    return consumed == size && x == 0 && y == height;
}

// Plans have been proven complete before the first read. Capture each run exactly once.
// On first mixed data, synthesize already consumed zero bytes; never reread their guest addresses.
template <typename Capture, typename Allocate, typename Decode, typename Upload>
bool UploadZeroAtlasSnapshots(std::span<const UnswizzlePlan> plans, Capture&& capture,
                              Allocate&& allocate, Decode&& decode, Upload&& upload) {
    using Staging = decltype(allocate(size_t{}));
    std::vector<u8> bytes;
    bool all_zero = true;
    for (size_t chunk = 0; chunk < plans.size(); ++chunk) {
        const auto& plan = plans[chunk];
        std::optional<Staging> staging;
        for (const auto& run : plan.runs) {
            const bool zero = capture(run, bytes);
            if (all_zero) {
                if (zero)
                    continue;
                all_zero = false;
                for (size_t i = 0; i < chunk; ++i) {
                    const auto& prefix = plans[i];
                    auto prior = allocate(prefix.total_host_bytes);
                    std::fill_n(prior.mapped_span.begin(), prefix.total_host_bytes, u8{});
                    upload(prior, prefix);
                }
            }
            if (!staging) {
                staging.emplace(allocate(plan.total_host_bytes));
                std::fill_n(staging->mapped_span.begin(), run.host_offset, u8{});
            }
            decode(plan, run, bytes, staging->mapped_span);
        }
        if (staging)
            upload(*staging, plan);
    }
    return all_zero;
}

} // namespace VideoCommon
