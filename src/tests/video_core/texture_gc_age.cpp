// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/eviction_age.h"

TEST_CASE("Texture GC diagnostic age cannot block emergency VRAM eviction",
          "[video_core][texture_cache]") {
    using VideoCommon::TextureGcEffectiveMinAge;
    // No override: original 50/25/10-frame behavior is unchanged.
    CHECK(TextureGcEffectiveMinAge(50, 0, false) == 50);
    CHECK(TextureGcEffectiveMinAge(25, 0, false) == 25);
    CHECK(TextureGcEffectiveMinAge(10, 0, true) == 10);

    // Preserve the diagnostic override while collection is non-emergency.
    CHECK(TextureGcEffectiveMinAge(50, 600, false) == 600);
    CHECK(TextureGcEffectiveMinAge(25, 600, false) == 600);
    CHECK(TextureGcEffectiveMinAge(50, 20, false) == 50);

    // #1290: the persisted 600-frame override previously blocked all recent
    // candidates even during the critical-pressure 10-frame collector pass.
    CHECK(TextureGcEffectiveMinAge(10, 600, true) == 10);
    CHECK(TextureGcEffectiveMinAge(10, 9000, true) == 10);
    CHECK(TextureGcEffectiveMinAge(10, 0, true) == 10);
}
