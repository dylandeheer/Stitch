// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <deque>
#include <memory>
#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/clear_discard.h"
#include "video_core/texture_cache/retire_completed_images.h"
#include "video_core/vulkan_common/retired_image_pool.h"

namespace {
VkImageCreateInfo Target() {
    return {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {8, 4, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
}
using Key = Vulkan::RetiredImageKey;
using Pool = Vulkan::RetiredImagePool<std::unique_ptr<int>>;
}

TEST_CASE("Only complete supported creation policies can match", "[retired_image_pool]") {
    auto ci = Target();
    const auto original = Key::From(ci, false);
    REQUIRE(original);
    REQUIRE_FALSE(Key::From(ci, true));
    const auto unsupported = [&](auto change) {
        auto other = ci;
        change(other);
        CHECK_FALSE(Key::From(other, false));
    };
    unsupported([](auto& x) { x.imageType = VK_IMAGE_TYPE_3D; });
    unsupported([](auto& x) { x.tiling = VK_IMAGE_TILING_LINEAR; });
    unsupported([](auto& x) { x.samples = VK_SAMPLE_COUNT_2_BIT; });
    unsupported([](auto& x) { x.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT; });
    unsupported([](auto& x) { x.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED; });
    unsupported([](auto& x) { x.sharingMode = VK_SHARING_MODE_CONCURRENT; });
    unsupported([](auto& x) { x.queueFamilyIndexCount = 1; });
    unsupported([](auto& x) { x.mipLevels = 2; });
    unsupported([](auto& x) { x.arrayLayers = 2; });
    unsupported([](auto& x) { x.extent.depth = 2; });
    unsupported([](auto& x) { x.usage = VK_IMAGE_USAGE_SAMPLED_BIT; });
    const auto different = [&](auto change) {
        auto other = ci;
        change(other);
        const auto key = Key::From(other, false);
        CHECK(key);
        CHECK(key != original);
    };
    different([](auto& x) { ++x.extent.width; });
    different([](auto& x) { ++x.extent.height; });
    different([](auto& x) { x.format = VK_FORMAT_B8G8R8A8_UNORM; });
    different([](auto& x) { x.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; });
    different([](auto& x) { x.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT; });

    std::array formats{ci.format, VK_FORMAT_R8G8B8A8_SRGB};
    VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, nullptr,
                                     2, formats.data()};
    ci.pNext = &list;
    ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    const auto key = Key::From(ci, false);
    REQUIRE(key);
    std::swap(formats[0], formats[1]);
    REQUIRE(Key::From(ci, false) != key);
    std::swap(formats[0], formats[1]);
    REQUIRE(Key::From(ci, false) == key);
    --list.viewFormatCount;
    REQUIRE(Key::From(ci, false) != key);
    list.viewFormatCount = 0;
    REQUIRE_FALSE(Key::From(ci, false));
    ++list.viewFormatCount;
    list.pNext = &list;
    REQUIRE_FALSE(Key::From(ci, false));
    list.pNext = nullptr;
    list.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    REQUIRE_FALSE(Key::From(ci, false));
}

TEST_CASE("Only completed timeline entries can enter the target pool", "[retired_image_pool]") {
    Pool pool, other_renderer;
    struct Runtime {
        u64 done{};
        bool IsFree(u64 tick) const { return tick <= done; }
    } runtime{10};
    struct Image {
        Pool* pool;
        Vulkan::RetiredImageTicket ticket;
        std::unique_ptr<int> handle;
        void RecycleRetiredTarget(u64 available) { REQUIRE(pool->Put(ticket, handle, 5, available)); }
    };
    struct Entry { u64 tick, bytes; Image image; };
    auto key = *Key::From(Target(), false);
    std::deque<Entry> queue;
    queue.push_back({10, 16, {&pool, {key, 32}, std::make_unique<int>(1)}});
    queue.push_back({11, 48, {&pool, {key, 64}, std::make_unique<int>(2)}});
    u64 sentenced = 64;
    Vulkan::RetiredImageTicket request{key};
    VideoCommon::RetireCompletedImages(queue, runtime, sentenced, u64{13});
    REQUIRE(sentenced == 48);
    REQUIRE(queue.size() == 1);
    REQUIRE(pool.Bytes() == 32);
    REQUIRE_FALSE(other_renderer.Take(request, 13));
    REQUIRE_FALSE(pool.Take(request, 12));
    REQUIRE(*pool.Take(request, 13) == 1);
    REQUIRE(request.bytes == 32);
    REQUIRE_FALSE(pool.Take(request, 13));
    runtime.done = 11;
    VideoCommon::RetireCompletedImages(queue, runtime, sentenced, u64{13});
    REQUIRE(sentenced == 0);
    REQUIRE(queue.empty());
    REQUIRE(*pool.Take(request, 13) == 2);
    REQUIRE(request.bytes == 64);
}

TEST_CASE("Retired targets stay bounded until synchronous destruction", "[retired_image_pool]") {
    struct Counted {
        explicit Counted(unsigned* count_) : count{count_} {}
        ~Counted() { ++*count; }
        unsigned* count;
    };
    using Handles = Vulkan::RetiredImagePool<std::unique_ptr<Counted>>;
    const auto key = *Key::From(Target(), false);
    unsigned destroyed = 0;
    {
        Handles pool;
        Vulkan::RetiredImageTicket first{key, Handles::MaxBytes};
        auto handle = std::make_unique<Counted>(&destroyed);
        REQUIRE(pool.Put(first, handle, 7, 7));
        handle = std::make_unique<Counted>(&destroyed);
        Vulkan::RetiredImageTicket excess{key, 1};
        REQUIRE_FALSE(pool.Put(excess, handle, 7, 7));
        REQUIRE(handle);
        REQUIRE(pool.Trim(7 + Handles::MaxFrames - 1, false) == 0);
        REQUIRE(pool.Trim(7 + Handles::MaxFrames, false) == 1);
        REQUIRE(destroyed == 1);
        for (size_t i = 0; i < Handles::MaxEntries; ++i) {
            Vulkan::RetiredImageTicket ticket{key, 1};
            REQUIRE(pool.Put(ticket, handle, 150, 150));
            handle = std::make_unique<Counted>(&destroyed);
        }
        REQUIRE_FALSE(pool.Put(excess, handle, 150, 150));
        REQUIRE(pool.Trim(150, true) == Handles::MaxEntries);
        REQUIRE(pool.Bytes() == 0);
        REQUIRE(destroyed == Handles::MaxEntries + 1);
        REQUIRE(pool.Put(excess, handle, 150, 150));
    }
    REQUIRE(destroyed == Handles::MaxEntries + 2);
}

TEST_CASE("Partial and predicated attachment clears retain guest contents", "[retired_image_pool]") {
    VideoCommon::ClearRegs regs{};
    using Enable = VideoCommon::ClearRegs::RenderEnable;
    regs.render_enable.mode = Enable::Mode::True;
    regs.rasterize_enable = 1;
    regs.rt_control.count.Assign(1);
    auto& rt = regs.rt[0];
    rt.format = Tegra::RenderTargetFormat::A8R8G8B8_UNORM;
    rt.address_low = 4096;
    rt.width = 1024;
    rt.height = 512;
    rt.depth.Assign(1);
    regs.clear_surface.R.Assign(1);
    regs.clear_surface.G.Assign(1);
    regs.clear_surface.B.Assign(1);
    regs.clear_surface.A.Assign(1);
    const auto eligible = [&] { return VideoCommon::SoleClearAttachment(regs, 4096, 1024, 512, false); };
    REQUIRE(eligible());
    regs.rt_control.count.Assign(2);
    regs.rt[1] = rt;
    regs.rt[1].address_low = 8192;
    regs.rt[1].tile_mode.is_pitch_linear.Assign(1); // width register = 1024 bytes = 256 pixels
    REQUIRE_FALSE(eligible());
    regs.rt_control.count.Assign(1);
    rt.tile_mode.is_pitch_linear.Assign(1);
    REQUIRE_FALSE(eligible());
    rt.tile_mode.is_pitch_linear.Assign(0);
    regs.render_enable.mode = Enable::Mode::Conditional;
    REQUIRE_FALSE(eligible());
    regs.render_enable.mode = Enable::Mode::IfEqual;
    REQUIRE_FALSE(eligible());
    regs.render_enable_override = Enable::Override::AlwaysRender;
    REQUIRE(eligible());
    regs.render_enable_override = Enable::Override::NeverRender;
    REQUIRE_FALSE(eligible());
    regs.render_enable_override = Enable::Override::AlwaysRender;
    regs.clear_surface.A.Assign(0);
    REQUIRE_FALSE(eligible());
    regs.clear_surface.A.Assign(1);
    regs.clear_surface.layer.Assign(1);
    REQUIRE_FALSE(eligible());
    regs.clear_surface.layer.Assign(0);
    regs.zeta_enable = 1;
    regs.zeta.address_low = 16384;
    REQUIRE_FALSE(eligible());
    regs.rt_control.count.Assign(0);
    regs.zeta_size.width = 1024;
    regs.zeta_size.height = 512;
    REQUIRE(VideoCommon::SoleClearAttachment(regs, 16384, 1024, 512, true));
    REQUIRE_FALSE(VideoCommon::SoleClearAttachment(regs, 4096, 1024, 512, true));
}
