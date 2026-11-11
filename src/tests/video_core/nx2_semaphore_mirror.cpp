// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"

TEST_CASE("Nx2SemaphoreMirror maps second-page releases to the first page", "[video_core][nx2]") {
    constexpr u64 region = 0x2106027000;
    constexpr u64 other = 0x2106100000;
    Tegra::Nx2ClearSemaphoreRegions();
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0x1010) == 0);

    Tegra::Nx2AddSemaphoreRegion(region);
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0x1000) == region);
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0x1010) == region + 0x10);
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0x1ffc) == region + 0xffc);
    CHECK(Tegra::Nx2SemaphoreMirror(region) == 0);
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0xffc) == 0);
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0x2000) == 0);
    CHECK(Tegra::Nx2SemaphoreMirror(other + 0x1010) == 0);

    Tegra::Nx2AddSemaphoreRegion(other);
    Tegra::Nx2AddSemaphoreRegion(region);
    CHECK(Tegra::Nx2SemaphoreMirror(other + 0x1010) == other + 0x10);
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0x1010) == region + 0x10);

    Tegra::Nx2RemoveSemaphoreRegion(region);
    CHECK(Tegra::Nx2SemaphoreMirror(region + 0x1010) == 0);
    CHECK(Tegra::Nx2SemaphoreMirror(other + 0x1010) == other + 0x10);

    Tegra::Nx2ClearSemaphoreRegions();
    CHECK(Tegra::Nx2SemaphoreMirror(other + 0x1010) == 0);
}


TEST_CASE("Nx2MirrorShadow keeps mirrored values out of words with direct releases", "[video_core][nx2]") {
    constexpr u64 region = 0x2106027000;
    constexpr u64 device = 0x80001000;
    u32 backing{};
    auto& shadow = Tegra::Nx2MirrorShadow::Get();
    Tegra::Nx2ClearSemaphoreRegions();
    Tegra::Nx2AddSemaphoreRegion(region);

    CHECK_FALSE(shadow.Diverts(device, &backing));
    shadow.NoteDirectRelease(device + 0x10, region + 0x1010, &backing);
    CHECK_FALSE(shadow.Diverts(device + 0x10, &backing));

    shadow.NoteDirectRelease(device, region, &backing);
    CHECK(shadow.Diverts(device, &backing));
    CHECK(shadow.Diverts(device + 2, &backing));
    CHECK_FALSE(shadow.Diverts(device + 4, &backing));

    CHECK_FALSE(shadow.Read(device, 7, &backing).has_value());
    shadow.Set(device, 42, 7, &backing);
    CHECK(shadow.Read(device, 7, &backing) == 42u);
    CHECK(shadow.Read(device, 8, &backing) == std::nullopt);
    CHECK(shadow.Read(device, 7, &backing) == std::nullopt);

    shadow.Set(device, 43, 7, &backing);
    shadow.NoteDirectRelease(device, region, &backing);
    CHECK(shadow.Read(device, 7, &backing) == std::nullopt);

    const auto max_with = [](u32 payload) { return [payload](u32 current) { return std::max(current, payload); }; };
    shadow.Set(device, 10, 0, &backing);
    CHECK(shadow.NoteCombinedRelease(device, region, 0, 3, &backing, max_with(3)) == 10u);
    CHECK(shadow.Read(device, 3, &backing) == 10u);
    CHECK(shadow.NoteCombinedRelease(device, region, 3, 12, &backing, max_with(12)) == 12u);
    CHECK(shadow.Read(device, 12, &backing) == 12u);
    CHECK(shadow.NoteCombinedRelease(device + 4, region + 4, 5, 6, &backing, max_with(6)) == 6u);
    CHECK(shadow.Read(device + 4, 6, &backing) == std::nullopt);

    shadow.Set(device, 44, 7, &backing);
    Tegra::Nx2ClearSemaphoreRegions();
    CHECK_FALSE(shadow.Diverts(device, &backing));
    CHECK(shadow.Read(device, 7, &backing) == std::nullopt);
}

TEST_CASE("Nx2SemaphoreMirror finds every region, past the first 32", "[video_core][nx2]") {
    constexpr u64 base = 0x2106000000;
    constexpr u64 count = 140;
    const auto start = [](u64 index) { return base + index * 0x3000; };
    Tegra::Nx2ClearSemaphoreRegions();
    for (u64 index = 0; index < count; ++index) {
        Tegra::Nx2AddSemaphoreRegion(start(index));
    }
    Tegra::Nx2AddSemaphoreRegion(start(0));
    CHECK(Tegra::g_nx2_semaphore_regions.size() == count);
    for (u64 index = 0; index < count; ++index) {
        CAPTURE(index);
        CHECK(Tegra::Nx2SemaphoreMirror(start(index) + 0x1010) == start(index) + 0x10);
        CHECK(Tegra::Nx2InSemaphoreFirstPage(start(index) + 0x10));
        CHECK(Tegra::Nx2SemaphoreMirror(start(index) + 0x2000) == 0);
        CHECK_FALSE(Tegra::Nx2InSemaphoreFirstPage(start(index) + 0x2000));
    }

    Tegra::Nx2RemoveSemaphoreRegion(start(count - 1));
    CHECK(Tegra::Nx2SemaphoreMirror(start(count - 1) + 0x1010) == 0);
    CHECK(Tegra::Nx2SemaphoreMirror(start(count - 2) + 0x1010) == start(count - 2) + 0x10);
    Tegra::Nx2ClearSemaphoreRegions();
    CHECK(Tegra::Nx2SemaphoreMirror(start(40) + 0x1010) == 0);
}

TEST_CASE("Destroying a semaphore region retires its mirrored word and preserves other queues",
          "[video_core][nx2][semaphore_mirror_remap]") {
    constexpr u64 region = 0x20000, other = 0x30000;
    constexpr u64 device = 0x4000, other_device = 0x5000;
    u32 old_word{}, new_word{}, other_word{};
    Tegra::Nx2ClearSemaphoreRegions();
    Tegra::Nx2AddSemaphoreRegion(region);
    Tegra::Nx2AddSemaphoreRegion(other);
    auto& shadow = Tegra::Nx2MirrorShadow::Get();
    shadow.NoteDirectRelease(device, region, &old_word);
    shadow.NoteDirectRelease(other_device, other, &other_word);
    shadow.Set(device, 8, 0, &old_word);
    shadow.Set(other_device, 11, 0, &other_word);
    CHECK_FALSE(shadow.Read(device, 0, &new_word));
    shadow.Set(device, 8, 0, &old_word);
    Tegra::Nx2RemoveSemaphoreRegion(region);
    Tegra::Nx2AddSemaphoreRegion(region);
    CHECK_FALSE(shadow.Diverts(device, &old_word));
    CHECK_FALSE(shadow.Read(device, 0, &old_word));
    CHECK(shadow.Read(other_device, 0, &other_word) == 11U);
    Tegra::Nx2ClearSemaphoreRegions();
}
