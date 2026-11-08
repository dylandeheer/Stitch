// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "video_core/dma_pusher.h"
namespace Tegra {

struct DmaPusherTestAccess {
    static bool Step(DmaPusher& pusher) {
        return pusher.Step();
    }
    static bool IsMemoryWriter(const Engines::EngineInterface& engine, Engines::EngineTypes type,
                               u32 method) {
        return DmaPusher::IsMemoryWriter(engine, type, method);
    }
};

} // namespace Tegra
