// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/texture_cache/util.h"

namespace VideoCommon {

// AddImageAlias emits one copy per mip, in destination mip order. Only a direct copy from a
// fully backed, up-to-date GPU image can replace the new image's initial guest upload.
[[nodiscard]] inline bool AliasCopyReplacesUpload(const ImageBase& dst, const ImageBase& src,
                                                  std::span<const ImageCopy> copies) {
    constexpr auto excluded = ImageFlagBits::Sparse | ImageFlagBits::Unbacked |
                              ImageFlagBits::SparseLayers | ImageFlagBits::SparseTiles |
                              ImageFlagBits::UnwrittenSparse | ImageFlagBits::TrimmedLevels |
                              ImageFlagBits::Remapped | ImageFlagBits::Rescaled |
                              ImageFlagBits::Converted | ImageFlagBits::IsDecoding;
    if (!src.IsSafeGpuCopy() || True((src.flags | dst.flags) & excluded) ||
        src.info.type != ImageType::e2D || dst.info.type != ImageType::e2D ||
        src.info.num_samples != 1 || dst.info.num_samples != 1 ||
        src.info.format != dst.info.format || src.info.astc_opaque != dst.info.astc_opaque ||
        VideoCore::Surface::DefaultBlockWidth(dst.info.format) != 1 ||
        VideoCore::Surface::DefaultBlockHeight(dst.info.format) != 1 ||
        dst.info.resources.levels <= 0 || dst.info.resources.layers <= 0 ||
        copies.size() != static_cast<size_t>(dst.info.resources.levels)) {
        return false;
    }
    for (s32 level = 0; level < dst.info.resources.levels; ++level) {
        const ImageCopy& copy = copies[level];
        const auto& from = copy.src_subresource;
        const auto& to = copy.dst_subresource;
        if (to.base_level != level || to.base_layer != 0 ||
            to.num_layers != dst.info.resources.layers || from.num_layers != to.num_layers ||
            from.base_level < 0 || from.base_level >= src.info.resources.levels ||
            from.base_layer < 0 || from.base_layer > src.info.resources.layers - from.num_layers ||
            copy.dst_offset != Offset3D{} || copy.src_offset != Offset3D{} ||
            copy.extent != MipSize(dst.info.size, level)) {
            return false;
        }
        const Extent3D available = MipSize(src.info.size, from.base_level);
        if (copy.extent.width > available.width || copy.extent.height > available.height ||
            copy.extent.depth != 1 || available.depth != 1) {
            return false;
        }
    }
    return true;
}

} // namespace VideoCommon
