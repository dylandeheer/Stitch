// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstring>
#include <limits>
#include <span>

#include "video_core/texture_cache/image_info.h"

namespace VideoCommon {

inline bool IsNativeAtlasShape(const ImageInfo& info, u64 bytes, PixelFormat format,
                               u32 side, u64 footprint) {
    // UploadInChunks has already excluded sparse/converted/unbacked host images. Guest page
    // continuity is independently checked for each read, exactly as on the D16 path.
    return info.native_tiling && info.type == ImageType::e2D &&
           info.format == format && info.size == Extent3D{side, side, 1} &&
           info.resources == SubresourceExtent{1, 1} && info.num_samples == 1 &&
           info.block.height == 4 && info.block.depth == 0 && bytes == footprint;
}

inline bool IsR32AtlasFileRead(const ImageInfo& info, u64 footprint) {
    return IsNativeAtlasShape(info, footprint, PixelFormat::R32_FLOAT, 4096, 64ULL << 20);
}

inline bool IsD16EvidenceAtlas(const ImageInfo& info, u64 footprint) {
    return IsNativeAtlasShape(info, footprint, PixelFormat::D16_UNORM, 12288, 288ULL << 20);
}

// Aggregate classifications of complete, already copied runs. A sum of byte lengths alone
// cannot distinguish a complete footprint from an overlap plus a missing interval.
struct AtlasUploadEvidence {
    explicit AtlasUploadEvidence(GPUVAddr base_, u64 size_) : base{base_}, size{size_} {}

    void Observe(GPUVAddr address, std::span<const u8> bytes, bool from_file) {
        constexpr size_t max_scan = 4ULL << 20;
        if (bytes.empty() || bytes.size() > max_scan || bytes.size() > size - scanned) {
            contiguous = false;
            return;
        }
        if (address < base || address - base != scanned) contiguous = false;
        scanned += bytes.size();
        ++runs;
        if (!from_file) non_file_bytes += bytes.size();
        const u8 first = bytes.front();
        // Adjacent-byte equality proves the entire run matches its first byte, including tails.
        if ((first != 0 && first != 0xff) ||
            std::memcmp(bytes.data(), bytes.data() + 1, bytes.size() - 1) != 0) {
            other_bytes += bytes.size();
        } else if (first == 0) {
            zero_bytes += bytes.size();
        } else {
            ff_bytes += bytes.size();
        }
    }

    bool Complete() const {
        return contiguous && size != 0 && size <= std::numeric_limits<u64>::max() - base &&
               scanned == size;
    }

    GPUVAddr base;
    u64 size;
    u64 scanned{}, runs{}, zero_bytes{}, ff_bytes{}, other_bytes{}, non_file_bytes{}, scan_ns{};
    bool contiguous = true;
};

} // namespace VideoCommon
