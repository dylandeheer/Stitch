// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/alias_copy.h"

namespace {

using namespace VideoCommon;

ImageInfo Image2D(PixelFormat format, u32 width, u32 height, s32 levels, s32 layers) {
    ImageInfo info;
    info.type = ImageType::e2D;
    info.format = format;
    info.size = {width, height, 1};
    info.resources = {levels, layers};
    info.layer_stride = CalculateLayerStride(info);
    return info;
}

TEST_CASE("Joined GPU copies must replace every texel of every subresource", "[image_alias_copy]") {
    const auto info = Image2D(PixelFormat::D32_FLOAT, 32, 16, 3, 2);
    ImageBase dst{info, 0x20000, 0x40000};
    ImageBase src{info, dst.gpu_addr, dst.cpu_addr};
    src.flags = ImageFlagBits::GpuModified;
    REQUIRE(AddImageAlias(dst, src, ImageId{1}, ImageId{2}));
    auto copies = dst.aliased_images.front().copies;
    REQUIRE(AliasCopyReplacesUpload(dst, src, copies));
    SECTION("missing mip") { copies.pop_back(); }
    SECTION("repeated mip") { copies[1] = copies[0]; }
    SECTION("layer subset") { copies[1].dst_subresource.num_layers = 1; }
    SECTION("destination layer offset") { copies[1].dst_subresource.base_layer = 1; }
    SECTION("source layers unavailable") { copies[1].src_subresource.base_layer = 1; }
    SECTION("source level unavailable") { copies[1].src_subresource.base_level = 3; }
    SECTION("source offset") { copies[1].src_offset.x = 1; }
    SECTION("destination offset") { copies[1].dst_offset.y = 1; }
    SECTION("last row missing") { --copies[1].extent.height; }
    SECTION("last column missing") { --copies[1].extent.width; }
    SECTION("depth empty") { copies[1].extent.depth = 0; }
    SECTION("source too small") { --src.info.size.width; }
    SECTION("format reinterpretation") { src.info.format = PixelFormat::R32_FLOAT; }
    SECTION("sample mismatch") { dst.info.num_samples = 2; }
    SECTION("newer guest bytes") { src.flags |= ImageFlagBits::CpuModified; }
    SECTION("source never drawn") { src.flags &= ~ImageFlagBits::GpuModified; }
    SECTION("storage states requiring different initialization") {
        for (const auto flag : {ImageFlagBits::Sparse, ImageFlagBits::Unbacked,
                                ImageFlagBits::SparseTiles, ImageFlagBits::SparseLayers,
                                ImageFlagBits::TrimmedLevels, ImageFlagBits::UnwrittenSparse,
                                ImageFlagBits::Remapped, ImageFlagBits::Converted,
                                ImageFlagBits::Rescaled, ImageFlagBits::IsDecoding}) {
            src.flags |= flag;
            CHECK_FALSE(AliasCopyReplacesUpload(dst, src, copies));
            src.flags &= ~flag;
            dst.flags |= flag;
            CHECK_FALSE(AliasCopyReplacesUpload(dst, src, copies));
            dst.flags &= ~flag;
        }
        return;
    }
    CHECK_FALSE(AliasCopyReplacesUpload(dst, src, copies));
}

TEST_CASE("Resized light targets preserve uncovered guest pixels", "[image_alias_copy]") {
    // Model a full lighting pass that adds to an existing target. An alias one column narrower
    // must retain the guest contents in that column; shrinking it back needs no guest data.
    const auto large = Image2D(PixelFormat::R16G16B16A16_FLOAT, 8, 4, 1, 1);
    const auto small = Image2D(large.format, 7, 4, 1, 1);
    ImageBase wider{large, 0x20000, 0x40000};
    ImageBase narrower{small, wider.gpu_addr, wider.cpu_addr};
    wider.flags = narrower.flags = ImageFlagBits::GpuModified;
    REQUIRE(AddImageAlias(wider, narrower, ImageId{1}, ImageId{2}));
    for (const bool growing : {true, false}) {
        CAPTURE(growing);
        const auto& dst = growing ? wider : narrower;
        const auto& src = growing ? narrower : wider;
        const auto& copies = dst.aliased_images.front().copies;
        REQUIRE(copies.size() == 1);
        const bool skip = AliasCopyReplacesUpload(dst, src, copies);
        REQUIRE(skip == !growing);
        std::array<u32, 32> pixels;
        pixels.fill(skip ? 0xBAD : 10);
        for (u32 y = 0; y < copies[0].extent.height; ++y) {
            for (u32 x = 0; x < copies[0].extent.width; ++x) {
                pixels[y * dst.info.size.width + x] = 20;
            }
        }
        for (u32 y = 0; y < dst.info.size.height; ++y) {
            for (u32 x = 0; x < dst.info.size.width; ++x) {
                CHECK(pixels[y * dst.info.size.width + x] + 3 == (x == 7 ? 13 : 23));
            }
        }
    }
}

} // namespace
