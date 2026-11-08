// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/atlas_upload_evidence.h"
#include "video_core/texture_cache/util.h"
#include "video_core/texture_cache/zero_depth_upload.h"

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

TEST_CASE("Only fresh backed D16 first-use storage can use a depth clear", "[zero_depth_upload]") {
    REQUIRE(IsFirstDepthUpload(false, ImageFlagBits::Tracked));
    REQUIRE_FALSE(IsFirstDepthUpload(true, ImageFlagBits::Tracked));
    for (auto bit :
         {ImageFlagBits::Registered, ImageFlagBits::Remapped, ImageFlagBits::GpuModified,
          ImageFlagBits::Alias, ImageFlagBits::BadOverlap, ImageFlagBits::Sparse,
          ImageFlagBits::Rescaled, ImageFlagBits::Unbacked, ImageFlagBits::TrimmedLevels}) {
        CHECK_FALSE(IsFirstDepthUpload(false, ImageFlagBits::Tracked | bit));
    }
}

TEST_CASE("A depth clear requires exact tiled source and full host subresource coverage",
          "[zero_depth_upload]") {
    constexpr GPUVAddr base = 0x12345000;
    constexpr size_t size = 288ULL << 20;
    constexpr size_t chunk = 32ULL << 20;
    auto info = Target(PixelFormat::D16_UNORM, 12288);
    info.layer_stride = CalculateLayerStride(info);
    std::vector<UnswizzlePlan> plans;
    for (size_t offset = 0; offset < size; offset += chunk) {
        const std::array range{std::pair<u64, u64>{offset, offset + chunk}};
        plans.emplace_back(PlanUnswizzleImageRows(base, info, range));
    }
    const auto covers = [&] { return CoversWholeD16(plans, base, size, 12288, 12288); };
    REQUIRE(covers());
    SECTION("Guest gap at the end") {
        ++plans.back().runs.back().src_addr;
    }
    SECTION("Destination overlap and hole of equal area") {
        --plans[1].copies.front().image_offset.x;
    }
    SECTION("Accidental second mip") {
        ++plans.back().copies.back().image_subresource.base_level;
    }
    SECTION("Second layer") {
        ++plans.front().copies.front().image_subresource.num_layers;
    }
    SECTION("Padded source") {
        ++plans[2].runs.back().guest_bytes;
    }
    SECTION("Short staging allocation") {
        --plans[1].total_host_bytes;
    }
    SECTION("Staging offset overlap") {
        --plans.back().copies.back().buffer_offset;
    }
    REQUIRE_FALSE(covers());
}

TEST_CASE("D16 owned runs survive source mutation and late mixed walker fallback",
          "[zero_depth_upload]") {
    constexpr GPUVAddr base = 512;
    constexpr size_t width = 256, height = 512;
    auto info = Target(PixelFormat::D16_UNORM, width);
    info.size.height = height;
    info.layer_stride = CalculateLayerStride(info);
    const auto size = CalculateGuestSizeInBytes(info);
    std::vector<UnswizzlePlan> plans;
    constexpr u64 chunk_size = 96 * 1024;
    for (u64 offset = 0; offset < size; offset += chunk_size) {
        const std::array range{std::pair{offset, std::min<u64>(size, offset + chunk_size)}};
        plans.emplace_back(PlanUnswizzleImageRows(base, info, range));
    }
    REQUIRE(CoversWholeD16(plans, base, size, width, height));
    std::vector<u8> source(size);
    bool expect_clear = false;
    SECTION("All zero") {
        expect_clear = true;
    }
    SECTION("Short-distance from start") {
        source.front() = 0x31;
    }
    SECTION("Last fallback snapshot is the only mixed one") {
        source.back() = 0x71;
    }
    SECTION("Chunk boundary") {
        source[chunk_size] = 0x51;
    }
    SECTION("Every byte FF requires original upload") {
        std::fill(source.begin(), source.end(), 0xff);
    }
    const auto original = source;
    std::vector<u8> received;
    std::vector<u8> expected;
    size_t next_read = 0, allocations = 0;
    D16UploadEvidence evidence{base, size};
    const auto capture = [&](const UnswizzleRowRun& r, std::vector<u8>& owned) {
        const size_t offset = r.src_addr - base;
        REQUIRE(offset == next_read);
        next_read += r.guest_bytes;
        owned.assign(source.begin() + offset, source.begin() + next_read);
        std::fill(source.begin() + offset, source.begin() + next_read, 0xed);
        const auto before = evidence.zero_bytes;
        evidence.Observe(r.src_addr, owned, next_read != size);
        return evidence.zero_bytes - before == r.guest_bytes;
    };
    struct Map {
        std::vector<u8> mapped_span;
    };
    const auto allocate = [&](size_t n) {
        ++allocations;
        return Map{std::vector<u8>(n, 0x99)};
    };
    const auto upload = [&](Map& map, const UnswizzlePlan& p) {
        received.insert(received.end(), map.mapped_span.begin(), map.mapped_span.end());
        std::vector<u8> baseline(p.total_host_bytes);
        for (const auto& r : p.runs) {
            const auto pristine = std::span{original}.subspan(r.src_addr - base, r.guest_bytes);
            DetileOwnedRun(p, r, pristine, baseline, true);
        }
        expected.insert(expected.end(), baseline.begin(), baseline.end());
    };
    const auto result = UploadD16Snapshots(
        plans, capture, allocate,
        [](const auto& p, const auto& r, auto& src, auto& dst) {
            DetileOwnedRun(p, r, src, dst, true);
        },
        upload);
    REQUIRE(result == expect_clear);
    REQUIRE(next_read == size);
    REQUIRE(evidence.Complete());
    REQUIRE(evidence.non_file_bytes == plans.back().runs.back().guest_bytes);
    REQUIRE(allocations == (expect_clear ? 0 : plans.size()));
    REQUIRE(received == expected);
    REQUIRE(received.size() == (expect_clear ? 0 : size));
}
