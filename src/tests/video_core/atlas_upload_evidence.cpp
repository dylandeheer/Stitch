// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/atlas_upload_evidence.h"
#include "video_core/texture_cache/util.h"

namespace {
using namespace VideoCommon;
ImageInfo Target(PixelFormat format, u32 side) {
    ImageInfo info;
    info.type = ImageType::e2D;
    info.native_tiling = true;
    info.format = format;
    info.size = {side, side, 1};
    info.resources = {1, 1};
    info.block = {0, 4, 0};
    return info;
}
}

TEST_CASE("R32 backing-file reads are limited to the observed plain atlas", "[atlas_upload_evidence]") {
    constexpr u64 bytes = 64ULL << 20;
    ImageInfo full = Target(PixelFormat::R32_FLOAT, 4096);
    REQUIRE(IsR32AtlasFileRead(full, bytes));
    REQUIRE_FALSE(IsR32AtlasFileRead(full, bytes - 1));
    auto reject = [&](auto mutate) {
        auto info = full;
        mutate(info);
        CHECK_FALSE(IsR32AtlasFileRead(info, bytes));
    };
    reject([](auto& i){ i.native_tiling = false; });
    reject([](auto& i){ i.type = ImageType::e3D; });
    reject([](auto& i){ i.format = PixelFormat::R32_UINT; });
    reject([](auto& i){ --i.size.width; });
    reject([](auto& i){ ++i.size.depth; });
    reject([](auto& i){ ++i.resources.levels; });
    reject([](auto& i){ ++i.resources.layers; });
    reject([](auto& i){ i.num_samples = 2; });
    reject([](auto& i){ ++i.block.height; });
    reject([](auto& i){ ++i.block.depth; });
    REQUIRE_FALSE(IsD16EvidenceAtlas(full, bytes));
}

TEST_CASE("Owned snapshot evidence includes tails, mixed runs and fallback bytes", "[atlas_upload_evidence]") {
    constexpr GPUVAddr base = 4096;
    D16UploadEvidence evidence{base, 12};
    const std::array<u8, 1> zero{0};
    const std::array<u8, 3> high{0xff, 0xff, 0xff};
    const std::array<u8, 5> last{0, 0, 0, 0, 1};
    const std::array<u8, 3> other{0x7a, 0x7a, 0x7a};
    evidence.Observe(base, zero, true);
    evidence.Observe(base + 1, high, false);
    evidence.Observe(base + 4, last, false);
    REQUIRE_FALSE(evidence.Complete());
    evidence.Observe(base + 9, other, true);
    REQUIRE(evidence.Complete());
    REQUIRE(evidence.zero_bytes == 1);
    REQUIRE(evidence.ff_bytes == 3);
    REQUIRE(evidence.other_bytes == 8); // counts whole runs, not individual nonzero texels
    REQUIRE(evidence.non_file_bytes == 8);
    REQUIRE(evidence.runs == 4);
    REQUIRE(evidence.scanned == 12);
}

TEST_CASE("A footprint sum cannot disguise holes, duplicate source addresses or wrap", "[atlas_upload_evidence]") {
    const std::array<u8, 4> data{};
    D16UploadEvidence gap{0x1000, 8};
    gap.Observe(0x1000, data, true);
    gap.Observe(0x1005, data, true);
    REQUIRE(gap.scanned == 8);
    REQUIRE_FALSE(gap.Complete());
    D16UploadEvidence overlap{0x1000, 8};
    overlap.Observe(0x1000, data, true);
    overlap.Observe(0x1000, data, false);
    REQUIRE(overlap.scanned == 8);
    REQUIRE_FALSE(overlap.Complete());
    D16UploadEvidence overflow{~u64{0} - 2, 4};
    overflow.Observe(~u64{0} - 2, data, true);
    REQUIRE_FALSE(overflow.Complete());
    D16UploadEvidence large{0, (4U << 20) + 1};
    const std::vector<u8> oversized((4U << 20) + 1);
    large.Observe(0, oversized, false);
    REQUIRE_FALSE(large.Complete());
    REQUIRE(large.scanned == 0);
}

TEST_CASE("All nine D16 chunks cover the full guest footprint exactly once", "[atlas_upload_evidence]") {
    constexpr GPUVAddr base = 0x8180fa0000;
    constexpr size_t bytes = 288ULL << 20;
    constexpr size_t chunk_size = 32ULL << 20;
    auto info = Target(PixelFormat::D16_UNORM, 12288);
    info.layer_stride = CalculateLayerStride(info);
    REQUIRE(CalculateGuestSizeInBytes(info) == bytes);
    REQUIRE(IsD16EvidenceAtlas(info, bytes));
    D16UploadEvidence observed{base, bytes};
    std::vector<u8> scratch(4U << 20);
    for (size_t offset = 0; offset < bytes; offset += chunk_size) {
        const std::array range{std::pair<u64,u64>{offset, offset + chunk_size}};
        const auto plan = PlanUnswizzleImageRows(base, info, range);
        REQUIRE(plan.layout_valid);
        for (const auto& r : plan.runs) {
            REQUIRE(r.guest_bytes <= scratch.size());
            const bool last = r.src_addr + r.guest_bytes == base + bytes;
            if (last) scratch[r.guest_bytes - 1] = 1;
            observed.Observe(r.src_addr, std::span{scratch}.first(r.guest_bytes), !last);
        }
    }
    REQUIRE(observed.Complete());
    REQUIRE(observed.runs == 102);
    REQUIRE(observed.non_file_bytes == 3U << 20);
    REQUIRE(observed.zero_bytes == bytes - observed.non_file_bytes);
    REQUIRE(observed.other_bytes == observed.non_file_bytes);
    REQUIRE(observed.ff_bytes == 0);
}
