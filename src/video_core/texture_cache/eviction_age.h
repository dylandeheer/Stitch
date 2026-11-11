// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>

namespace VideoCommon {

// The diagnostic minimum age must not prevent eviction under critical GPU memory pressure.
[[nodiscard]] constexpr std::uint64_t TextureGcEffectiveMinAge(
    std::uint64_t base_age, std::uint64_t diagnostic_min_age, bool aggressive_mode) {
    return aggressive_mode ? base_age : (std::max)(base_age, diagnostic_min_age);
}

} // namespace VideoCommon
