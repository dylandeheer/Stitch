// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <filesystem>
#include <span>

#include "common/common_types.h"

namespace VideoCommon {

// Private instruction-only capture for the observed split-front executable. Gen is recorded,
// not matched: the upload count can change between launches without changing the executable.
class MacroImageCapture {
public:
    static constexpr u32 WordCount = 0x8000;
    static constexpr u64 TargetHash = 0x3ed7146766a3d774ULL;
    struct Entry {
        s32 channel;
        u64 address_space;
        u32 method, start;
        u64 generation;
    };
    // Belongs to the serial front of one Maxwell3D channel. Avoid hashing 128 KiB on every call.
    struct CheckedImage {
        const void* owner{};
        u64 generation{};
    };
    enum class Result { Ignored, Mismatch, Saved, Failed };

    explicit MacroImageCapture(u64 expected = TargetHash) : expected_hash{expected} {}

    Result Try(const std::filesystem::path& file, Entry entry, const void* owner,
               std::span<const u32> code, CheckedImage& checked);

private:
    const u64 expected_hash;
    std::atomic<bool> claimed{};
};

} // namespace VideoCommon
