// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common/common_types.h"
#include "video_core/engines/maxwell_3d.h"

namespace VideoCommon {

using ClearRegs = Tegra::Engines::Maxwell3D::Regs;

inline bool UnconditionalClear(const ClearRegs& regs) {
    using Enable = ClearRegs::RenderEnable;
    return regs.render_enable_override == Enable::Override::AlwaysRender ||
           (regs.render_enable_override == Enable::Override::UseRenderEnable &&
            regs.render_enable.mode == Enable::Mode::True);
}

// A second attachment can reduce the framebuffer's effective render area. In particular,
// pitch-linear color width registers are bytes, not pixels.
inline bool SoleClearAttachment(const ClearRegs& regs, GPUVAddr address, u32 width, u32 height,
                                bool depth) {
    if (!address || !UnconditionalClear(regs) || regs.clear_surface.layer != 0 ||
        regs.clear_control.use_viewport_clip0 || regs.clear_control.use_clear_rect ||
        regs.surface_clip.x || regs.surface_clip.y || !regs.rasterize_enable) {
        return false;
    }
    const bool has_depth = regs.zeta_enable && regs.zeta.Address();
    if (has_depth != depth || regs.rt_control.count > regs.rt.size()) {
        return false;
    }
    bool found = false;
    for (u32 i = 0; i < regs.rt_control.count; ++i) {
        const auto& rt = regs.rt[i];
        if (!rt.Address() || rt.format == Tegra::RenderTargetFormat::NONE) {
            continue;
        }
        if (depth || found || i != regs.clear_surface.RT || regs.rt_control.Map(i) != i ||
            rt.Address() != address || rt.tile_mode.is_pitch_linear || rt.base_layer != 0 ||
            rt.width != width || rt.height != height || rt.depth != 1) {
            return false;
        }
        found = true;
    }
    if (depth) {
        return regs.zeta.Address() == address && !regs.zeta.tile_mode.is_pitch_linear &&
               regs.zeta_size.width == width && regs.zeta_size.height == height;
    }
    return found && regs.clear_surface.R && regs.clear_surface.G &&
           regs.clear_surface.B && regs.clear_surface.A;
}

} // namespace VideoCommon
