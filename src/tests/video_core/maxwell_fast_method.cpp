// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "tests/video_core/dma_pusher_test_access.h"
#include "tests/video_core/ordered_fifo_program.h"
#include "video_core/control/channel_state.h"

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/macro_dma_coverage.h"
#include "video_core/macro_fifo_shape.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace VideoCommon {

struct GpuSplitTestAccess {
    static void Initialize(GpuSplit& split, Core::System& system, u64 position = 0) {
        split.system = &system;
        split.ring = std::make_unique<u64[]>(GpuSplit::RingWords);
        split.write_position = position;
        split.consumed_cache = position;
        split.published_local = position;
        split.publish_every = GpuSplit::RingWords;
    }

    static u64 End(const GpuSplit& split) {
        return split.write_position;
    }

    static void Start(GpuSplit& split, Core::System& system) {
        Initialize(split, system);
        split.active = true;
        split.back_thread = std::jthread([&split](std::stop_token stop) { split.BackLoop(stop); });
    }

    static u64 NearWrap() {
        return GpuSplit::RingWords - 1;
    }

    static u64 Execute(GpuSplit& split, u64 position, u64 available) {
        const size_t index = position & GpuSplit::RingMask;
        const size_t size = split.Execute(
            &split.ring[index], std::min<u64>(available - position, GpuSplit::RingWords - index));
        return position + (size != 0 ? size : GpuSplit::RingWords - index);
    }

    static bool FrontWaits(const GpuSplit& split) {
        return split.front_waiting.load(std::memory_order_seq_cst);
    }

    static void FifoMarker(GpuSplit& split, const char* path) {
        split.ordered_fifo_marker = path;
        split.ordered_fifo_polls = 0;
    }
    static void OrderedMode(GpuSplit& split, GpuSplit::OrderedFifoMode mode) {
        split.ordered_fifo_mode = mode;
        split.ordered_fifo_marker = nullptr;
        split.front_macros = true;
    }

    static void EnableWriterFrontier(GpuSplit& split, bool verify = false) {
        split.writer_requested = true;
        split.writer_verify = verify;
        static_cast<void>(split.SelectWriterFrontier());
    }

    static u64 WritesTarget(const GpuSplit& split, const Tegra::MemoryManager& memory,
                            GPUVAddr address, u64 size) {
        return split.WritesTarget(memory, address, size);
    }

    /// Whether writes to two device addresses share a slot of the noted write positions.
    static bool ShareWriteSlot(DAddr a, DAddr b) {
        return GpuSplit::WriteSlot(a >> GpuSplit::WriteGranuleBits) ==
               GpuSplit::WriteSlot(b >> GpuSplit::WriteGranuleBits);
    }
};

} // namespace VideoCommon

namespace {

using Tegra::Engines::Maxwell3D;

constexpr u32 CbSize = MAXWELL3D_REG_INDEX(const_buffer.size);
constexpr u32 CbAddressHigh = MAXWELL3D_REG_INDEX(const_buffer.address_high);
constexpr u32 CbAddressLow = MAXWELL3D_REG_INDEX(const_buffer.address_low);
constexpr u32 CbOffset = MAXWELL3D_REG_INDEX(const_buffer.offset);
constexpr u32 CbData = MAXWELL3D_REG_INDEX(const_buffer.buffer);
constexpr u32 DrawBegin = MAXWELL3D_REG_INDEX(draw.begin);
constexpr u32 DrawEnd = MAXWELL3D_REG_INDEX(draw.end);
constexpr u32 ShadowControl = MAXWELL3D_REG_INDEX(shadow_ram_control);

// Executable methods without memory or macro effects a null rasterizer cannot take.
constexpr std::array<u32, 16> executable_methods{
    DrawBegin,
    DrawEnd,
    MAXWELL3D_REG_INDEX(vertex_buffer.first),
    MAXWELL3D_REG_INDEX(vertex_buffer.count),
    MAXWELL3D_REG_INDEX(index_buffer.first),
    MAXWELL3D_REG_INDEX(index_buffer.count),
    MAXWELL3D_REG_INDEX(topology_override),
    MAXWELL3D_REG_INDEX(bind_groups[0].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[1].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[2].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[3].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[4].raw_config),
    MAXWELL3D_REG_INDEX(vertex_array_instance_first),
    MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent),
    Maxwell3D::RootTables::SelectMethod,
    Maxwell3D::RootTables::LoadMethod,
};

struct Fixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    Maxwell3D slow{memory};
    Maxwell3D fast{memory};
    std::vector<u32> state_methods;

    Fixture() {
        slow.fast_maxwell3d = false;
        fast.fast_maxwell3d = true;
        for (auto* engine : {&slow, &fast}) {
            engine->BindRasterizer(&rasterizer);
            VideoCommon::Dirty::SetupDirtyFlags(engine->dirty.tables);
            // Give every register a flag so that a missed or extra write shows.
            for (u32 method = 0; method < Maxwell3D::Regs::NUM_REGS; ++method) {
                if (engine->dirty.tables[1][method] == 0) {
                    engine->dirty.tables[1][method] =
                        static_cast<u8>(VideoCommon::Dirty::LastCommonEntry + method % 61);
                }
            }
            engine->dirty.flags.reset();
            // An unmapped buffer: the upload writes nothing, the offset still advances.
            engine->CallMethod(system, CbAddressHigh, 0x1, true);
            engine->CallMethod(system, CbAddressLow, 0x2000'0000, true);
            engine->CallMethod(system, CbSize, 0x10000, true);
        }
        for (u32 method = 0; method < Maxwell3D::Regs::NUM_REGS; ++method) {
            if (!slow.execution_mask[method] && (method < CbSize || method > CbAddressLow)) {
                state_methods.push_back(method);
            }
        }
    }

    void Call(u32 method, u32 argument, bool is_last) {
        slow.CallMethod(system, method, argument, is_last);
        fast.CallMethod(system, method, argument, is_last);
    }

    void CallMulti(u32 method, const std::vector<u32>& words) {
        const u32 amount = static_cast<u32>(words.size());
        slow.CallMultiMethod(system, method, words.data(), amount, amount);
        fast.CallMultiMethod(system, method, words.data(), amount, amount);
    }

    void Compare() const {
        REQUIRE(fast.regs.reg_array == slow.regs.reg_array);
        REQUIRE(fast.shadow_state.reg_array == slow.shadow_state.reg_array);
        REQUIRE(fast.shadow_state.shadow_ram_control == slow.shadow_state.shadow_ram_control);
        REQUIRE(fast.dirty.flags == slow.dirty.flags);
        const auto& a = fast.draw_manager.draw_state;
        const auto& b = slow.draw_manager.draw_state;
        REQUIRE(a.topology == b.topology);
        REQUIRE(a.draw_mode == b.draw_mode);
        REQUIRE(a.draw_indexed == b.draw_indexed);
        REQUIRE(a.draw_element_count == b.draw_element_count);
        REQUIRE(a.base_index == b.base_index);
        REQUIRE(a.base_instance == b.base_instance);
        REQUIRE(a.instance_count == b.instance_count);
        REQUIRE(a.drawn_instances == b.drawn_instances);
        REQUIRE(std::memcmp(&a.vertex_buffer, &b.vertex_buffer, sizeof(a.vertex_buffer)) == 0);
        REQUIRE(std::memcmp(&a.index_buffer, &b.index_buffer, sizeof(a.index_buffer)) == 0);
        for (size_t stage = 0; stage < fast.state.shader_stages.size(); ++stage) {
            for (size_t slot = 0; slot < Maxwell3D::Regs::MaxConstBuffers; ++slot) {
                const auto& x = fast.state.shader_stages[stage].const_buffers[slot];
                const auto& y = slow.state.shader_stages[stage].const_buffers[slot];
                REQUIRE(x.address == y.address);
                REQUIRE(x.size == y.size);
                REQUIRE(x.enabled == y.enabled);
            }
        }
        REQUIRE(fast.root_tables.tables == slow.root_tables.tables);
        REQUIRE(fast.root_tables.selected == slow.root_tables.selected);
        REQUIRE(fast.root_tables.offset == slow.root_tables.offset);
    }
};

u32 RandomArgument(std::mt19937& rng, u32 method, const Maxwell3D& engine) {
    // Mostly values the register already holds or small ones, as the macros write them.
    switch (rng() % 4) {
    case 0:
        return engine.regs.reg_array[method];
    case 1:
        return rng() % 4;
    default:
        break;
    }
    if (method == DrawBegin) {
        // Instance id in bits 26-27: first, subsequent (an instanced loop) or unchanged.
        return (static_cast<u32>(rng() % 3) << 26) | (rng() % 8);
    }
    if (method == Maxwell3D::RootTables::SelectMethod) {
        return (rng() % 8) | ((rng() % 64 * 4) << 8);
    }
    return rng() % 64;
}

/// A Turing MME instruction that sends the next macro parameter to `method` (as in
/// native_macro_fifo.cpp); `end` makes the instruction after it the macro's last.
std::array<u32, 3> FifoInstruction(u32 method, bool end) {
    std::array<u32, 3> bits{};
    const auto field = [&](u32 offset, u32 count, u32 value) {
        for (u32 bit = 0; bit < count; ++bit) {
            bits[(offset + bit) / 32] |= ((value >> bit) & 1) << ((offset + bit) % 32);
        }
    };
    field(0, 1, end);
    for (u32 lane = 0; lane < 2; ++lane) {
        field(15 + lane * 36, 5, 24);
        field(20 + lane * 36, 5, 24);
        field(25 + lane * 36, 5, 24);
    }
    field(30, 16, method);
    field(82, 3, 5);
    field(85, 4, 3);
    return {bits[2], bits[1], bits[0]};
}

} // Anonymous namespace

TEST_CASE("Maxwell3D fast method dispatch matches CallMethod on random method streams",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    for (const u32 seed : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u}) {
        CAPTURE(seed);
        std::mt19937 rng{seed};
        for (u32 step = 0; step < 20000; ++step) {
            CAPTURE(step);
            const u32 kind = rng() % 100;
            if (kind < 45) {
                const u32 method = f.state_methods[rng() % f.state_methods.size()];
                f.Call(method, RandomArgument(rng, method, f.slow), rng() % 2 == 0);
            } else if (kind < 75) {
                const u32 method = executable_methods[rng() % executable_methods.size()];
                f.Call(method, RandomArgument(rng, method, f.slow), rng() % 2 == 0);
            } else if (kind < 85) {
                f.Call(CbData + rng() % 16, rng(), true);
            } else if (kind < 88) {
                f.Call(CbOffset, (rng() % 0x4000) * 4, true);
            } else if (kind < 92) {
                std::vector<u32> words(1 + rng() % 8);
                for (u32& word : words) {
                    word = rng() % 3 == 0 ? rng() : rng() % 4;
                }
                const u32 method = rng() % 2 == 0
                                       ? CbData + rng() % 16
                                       : f.state_methods[rng() % f.state_methods.size()];
                f.CallMulti(method, words);
            } else if (kind < 94) {
                // Track, track with filter, passthrough or replay.
                f.Call(ShadowControl, rng() % 4, true);
            } else if (kind < 97) {
                for (auto* engine : {&f.slow, &f.fast}) {
                    engine->dirty.flags.reset();
                }
            } else {
                for (auto* engine : {&f.slow, &f.fast}) {
                    engine->draw_manager.DrawDeferred(*engine);
                }
            }
            f.Compare();
        }
    }
}

TEST_CASE("Maxwell3D fast method dispatch issues deferred instanced draws as CallMethod does",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    constexpr u32 subsequent = 1u << 26;
    const auto draw = [&f](u32 begin) {
        f.Call(DrawBegin, begin, true);
        f.Call(MAXWELL3D_REG_INDEX(vertex_buffer.first), 0, true);
        f.Call(MAXWELL3D_REG_INDEX(vertex_buffer.count), 3, true);
        f.Call(DrawEnd, 0, true);
        f.Compare();
    };
    draw(0);
    draw(subsequent);
    draw(subsequent);
    REQUIRE(f.fast.draw_manager.HasDeferredDraw());
    // A constant buffer upload, a bind and state changes each end the loop.
    f.Call(CbData, 7, true);
    f.Compare();
    REQUIRE_FALSE(f.fast.draw_manager.HasDeferredDraw());
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(bind_groups[4].raw_config), 1 | (3 << 4), true);
    f.Compare();
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(topology_override), 5, true);
    f.Compare();
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(global_base_instance_index), 9, true);
    f.Compare();
    REQUIRE_FALSE(f.fast.draw_manager.HasDeferredDraw());
}

TEST_CASE("Maxwell3D constant buffer word runs match single CallMethod writes",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    std::mt19937 rng{11};
    constexpr u32 subsequent = 1u << 26;
    for (u32 step = 0; step < 20000; ++step) {
        CAPTURE(step);
        switch (rng() % 8) {
        case 0:
            f.Call(ShadowControl, rng() % 4, true);
            break;
        case 1:
            // Near the end too, where words are dropped.
            f.Call(CbSize, rng() % 2 == 0 ? 0x10000 : 0x100 + rng() % 0x40, true);
            break;
        case 2:
            f.Call(CbOffset, rng() % 3 == 0 ? 0xF0 + (rng() % 8) * 4 : (rng() % 0x400) * 4, true);
            break;
        case 3:
            // An instanced loop the first word ends.
            f.Call(DrawBegin, rng() % 2 == 0 ? 0 : subsequent, true);
            f.Call(DrawEnd, 0, true);
            break;
        default: {
            const u32 count = 1 + rng() % 64;
            std::vector<u32> methods(count);
            std::vector<u32> arguments(count);
            const u32 first = CbData + rng() % 16;
            for (u32 i = 0; i < count; ++i) {
                methods[i] = rng() % 2 == 0 ? first : CbData + (first - CbData + i) % 16;
                arguments[i] = rng() % 2 == 0 ? rng() : rng() % 4;
            }
            for (u32 i = 0; i < count; ++i) {
                f.slow.CallMethod(f.system, methods[i], arguments[i], true);
            }
            f.fast.CallConstBufferWords(f.system, methods.data(), arguments.data(), count);
            break;
        }
        }
        f.Compare();
    }
}

TEST_CASE("GpuSplit CB batching respects device remaps within GPU big pages",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    auto* replacement = f.physical.GetPointerFromRaw<u8>(0x30000);
    for (u64 page = 0; page < 16; ++page) {
        table.entries.GetAndFault(4 + page)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing) - 0x4000);
    }
    table.entries.GetAndFault(0x20).Store(false, Common::PageType::Memory, 1,
                                        reinterpret_cast<uintptr_t>(replacement) - 0x20000);
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(0x10000, 0x10000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(0x10000, 0x4000, 0x10000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(0x100000, 0x10000, 0x10000, Tegra::PTEKind::PITCH, true);
    f.device_memory.Unmap(0x11000, 0x1000);
    f.device_memory.Map(0x11000, 0x20000, 0x1000, asid);
    REQUIRE(f.memory.GetPointer(0x100ffc) == backing + 0xffc);
    REQUIRE(f.memory.GetPointer(0x101000) == replacement);

    for (auto* engine : {&f.slow, &f.fast}) {
        std::memset(backing, 0, 0x10000);
        std::memset(replacement, 0, 0x1000);
        VideoCommon::GpuSplit split;
        Access::Initialize(split, f.system);
        split.MacroWrite(engine, CbAddressHigh, 0);
        split.MacroWrite(engine, CbAddressLow, 0x100ffc);
        split.MacroWrite(engine, CbSize, 8);
        split.MacroWrite(engine, CbOffset, 0);
        split.MacroWrite(engine, CbData, 0x12345678);
        split.MacroWrite(engine, CbData + 1, 0x87654321);
        u64 position{};
        while (position < Access::End(split)) {
            position = Access::Execute(split, position, Access::End(split));
        }
        u32 first{}, second{}, stale{};
        std::memcpy(&first, backing + 0xffc, sizeof(first));
        std::memcpy(&second, replacement, sizeof(second));
        std::memcpy(&stale, backing + 0x1000, sizeof(stale));
        CAPTURE(engine == &f.fast);
        CAPTURE(stale);
        REQUIRE(first == 0x12345678);
        REQUIRE(second == 0x87654321);
        REQUIRE(stale == 0);
        REQUIRE(engine->regs.const_buffer.offset == 8);
    }
    f.Compare();
}

TEST_CASE("GpuSplit dispatch preserves mapped uploads and macro boundaries",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    constexpr GPUVAddr gpu_address = 0x100000;
    constexpr DAddr device_address = 0x10000;
    for (u64 page = 0; page < 3; ++page) {
        const VAddr virtual_address = 0x4000 + page * 0x1000;
        auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000 + page * 0x8000);
        table.entries.GetAndFault(virtual_address >> 12)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing) - virtual_address);
    }
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(device_address, 0x3000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(device_address, 0x4000, 0x3000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(gpu_address, device_address, 0x3000, Tegra::PTEKind::PITCH, false);

    struct Snapshot {
        std::array<u8, 0x3000> bytes{};
        std::array<u32, Maxwell3D::Regs::NUM_REGS> regs{};
        std::array<u32, Maxwell3D::Regs::NUM_REGS> shadow{};
        Maxwell3D::DirtyState::Flags dirty{};
        GPUVAddr segment{};
    };
    const auto snapshot = [&](const Maxwell3D& engine) {
        Snapshot result;
        f.memory.ReadBlockUnsafe(gpu_address, result.bytes.data(), result.bytes.size());
        result.regs = engine.regs.reg_array;
        result.shadow = engine.shadow_state.reg_array;
        result.dirty = engine.dirty.flags;
        result.segment = engine.current_dma_segment;
        return result;
    };

    for (const bool wrap : {false, true}) {
        CAPTURE(wrap);
        std::vector<Snapshot> expected;
        for (auto* engine : {&f.slow, &f.fast}) {
            VideoCommon::GpuSplit split;
            struct ProbeEngine final : Tegra::Engines::EngineInterface {
                u32 writes{};
                void CallMethod(Core::System&, u32, u32, bool) override {
                    ++writes;
                }
                void CallMultiMethod(Core::System&, u32, const u32*, u32 amount, u32) override {
                    writes += amount;
                }
            } probe;
            const u64 start = wrap ? Access::NearWrap() : 0;
            Access::Initialize(split, f.system, start);
            std::vector<u64> boundaries;
            std::array<u8, 0x3000> cleared{};
            f.memory.WriteBlockUnsafe(gpu_address, cleared.data(), cleared.size());
            engine->current_dma_segment = 0;
            engine->dirty.flags.reset();
            const auto state = [&](u32 method, u32 value) {
                split.Method(engine, method, value, true, nullptr);
                boundaries.push_back(Access::End(split));
            };
            const auto direct = [&](u32 method, u32 value) {
                split.MacroWrite(engine, method, value);
                boundaries.push_back(Access::End(split));
            };
            size_t observed{};
            const auto inspect = [&] {
                split.Defer([&, engine] {
                    auto actual = snapshot(*engine);
                    if (engine == &f.slow) {
                        expected.push_back(std::move(actual));
                    } else {
                        REQUIRE(observed < expected.size());
                        const auto& reference = expected[observed];
                        REQUIRE(actual.bytes == reference.bytes);
                        REQUIRE(actual.regs == reference.regs);
                        REQUIRE(actual.shadow == reference.shadow);
                        REQUIRE(actual.dirty == reference.dirty);
                        REQUIRE(actual.segment == reference.segment);
                    }
                    ++observed;
                });
                boundaries.push_back(Access::End(split));
            };
            state(ShadowControl, 0);
            state(CbAddressHigh, 0);
            state(CbAddressLow, static_cast<u32>(gpu_address));
            state(CbSize, 0x3000);
            state(CbOffset, 0xffb);
            direct(MAXWELL3D_REG_INDEX(global_base_instance_index), 17);
            inspect();
            for (u32 i = 0; i < 129; ++i) {
                if (i % 2 == 0) {
                    direct(CbData + i % 16, 0x12340000 + i);
                } else {
                    state(CbData + i % 16, 0x56780000 + i);
                }
            }
            inspect();
            state(ShadowControl, 3);
            state(CbOffset, 0x1ffd);
            for (u32 i = 0; i < 33; ++i) {
                direct(CbData + i % 16, 0xdeadbeef);
            }
            inspect();
            state(ShadowControl, 2);
            state(CbOffset, 0x1ffd);
            direct(DrawBegin, 0);
            direct(DrawEnd, 0);
            direct(DrawBegin, 1u << 26);
            direct(DrawEnd, 0);
            direct(CbData, 0x11223344);
            direct(CbData + 1, 0xaabbccdd);
            const GPUVAddr segment = 0x12345678;
            split.Method(engine, CbData + 2, 0x87654321, false, &segment);
            boundaries.push_back(Access::End(split));
            direct(CbData + 3, 0x76543210);
            split.MacroWrite(&probe, 0x10, 42);
            boundaries.push_back(Access::End(split));
            direct(CbData + 4, 0x65432100);
            const std::array<u32, 3> multi{19, 20, 21};
            split.MultiMethod(engine, CbData, multi.data(), 3, 3, &segment);
            boundaries.push_back(Access::End(split));
            inspect();
            state(CbSize, 7);
            state(CbOffset, 0);
            direct(CbData, 0x99aabbcc);
            direct(CbData + 1, 0xddeeff00);
            inspect();

            u64 position = start;
            if (wrap) {
                for (const u64 available : boundaries) {
                    while (position < available) {
                        position = Access::Execute(split, position, available);
                        REQUIRE(position <= available);
                    }
                }
            } else {
                while (position < Access::End(split)) {
                    position = Access::Execute(split, position, Access::End(split));
                    REQUIRE(position <= Access::End(split));
                }
            }
            REQUIRE(observed == expected.size());
            REQUIRE(probe.writes == 1);
        }
        f.Compare();
    }
}

TEST_CASE("GpuSplit CB batching preserves words at the GPU address space end",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    table.entries.GetAndFault(4).Store(false, Common::PageType::Memory, 1,
                                     reinterpret_cast<uintptr_t>(backing) - 0x4000);
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(0x10000, 0x1000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(0x10000, 0x4000, 0x1000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(0xfffff000, 0x10000, 0x1000, Tegra::PTEKind::PITCH, false);
    for (auto* engine : {&f.slow, &f.fast}) {
        std::memset(backing, 0, 0x1000);
        VideoCommon::GpuSplit split;
        Access::Initialize(split, f.system);
        split.MacroWrite(engine, CbAddressHigh, 0);
        split.MacroWrite(engine, CbAddressLow, 0xfffffffcu);
        split.MacroWrite(engine, CbSize, 8);
        split.MacroWrite(engine, CbOffset, 0);
        split.MacroWrite(engine, CbData, 0x12345678);
        split.MacroWrite(engine, CbData + 1, 0x87654321);
        u64 position{};
        while (position < Access::End(split)) {
            position = Access::Execute(split, position, Access::End(split));
        }
        u32 word{};
        std::memcpy(&word, backing + 0xffc, sizeof(word));
        CAPTURE(engine == &f.fast);
        REQUIRE(word == 0x12345678);
        REQUIRE(engine->regs.const_buffer.offset == 8);
    }
    f.Compare();
}

TEST_CASE("Maxwell3D fast method dispatch cost", "[.][maxwell_fast_method_bench]") {
    Fixture f;
    // Per draw, as the back thread sees Ocarina's: state writes the front did not drop, a
    // constant buffer update, three binds and a draw.
    std::vector<std::pair<u32, u32>> stream;
    std::mt19937 rng{1};
    for (u32 draw = 0; draw < 4096; ++draw) {
        for (u32 i = 0; i < 12; ++i) {
            stream.emplace_back(f.state_methods[rng() % 512], rng() % 8);
        }
        stream.emplace_back(CbOffset, 0x640);
        stream.emplace_back(CbData, draw);
        stream.emplace_back(CbData + 1, 0);
        stream.emplace_back(CbData + 2, draw * 3);
        for (const u32 stage : {0u, 2u, 4u}) {
            stream.emplace_back(MAXWELL3D_REG_INDEX(bind_groups[0].raw_config) + stage * 8,
                                1 | (rng() % 8) << 4);
        }
        stream.emplace_back(DrawBegin, 0);
        stream.emplace_back(MAXWELL3D_REG_INDEX(vertex_buffer.first), rng() % 4);
        stream.emplace_back(MAXWELL3D_REG_INDEX(vertex_buffer.count), 3 + rng() % 4);
        stream.emplace_back(DrawEnd, 0);
    }
    const auto run = [&](Maxwell3D& engine) {
        const auto start = std::chrono::steady_clock::now();
        for (u32 round = 0; round < 50; ++round) {
            for (const auto& [method, argument] : stream) {
                engine.CallMethod(f.system, method, argument, true);
            }
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                   .count() /
               (50.0 * static_cast<double>(stream.size()));
    };
    run(f.slow);
    run(f.fast);
    const double slow_ns = run(f.slow);
    const double fast_ns = run(f.fast);
    WARN("CallMethod: " << slow_ns << " ns per method, fast path " << fast_ns << " ns");

    // Three constant buffer words (a draw macro's c[0][0x640..0x648]) one by one or as a run.
    constexpr std::array<u32, 3> methods{CbData, CbData + 1, CbData + 2};
    constexpr std::array<u32, 3> arguments{1, 2, 3};
    const auto words = [&](bool as_run) {
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < 1'000'000; ++i) {
            f.fast.CallMethod(f.system, CbOffset, 0x640, true);
            if (as_run) {
                f.fast.CallConstBufferWords(f.system, methods.data(), arguments.data(), 3);
            } else {
                for (u32 w = 0; w < 3; ++w) {
                    f.fast.CallMethod(f.system, methods[w], arguments[w], true);
                }
            }
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                   .count() /
               1'000'000.0;
    };
    words(false);
    words(true);
    const double single_ns = words(false);
    const double run_ns = words(true);
    WARN("Three constant buffer words: " << single_ns << " ns one by one, " << run_ns
                                         << " ns as a run (unmapped buffer)");
}

TEST_CASE("Full split drains preserve aliased uploads, partial coverage and queued remaps",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    constexpr VAddr cpu_address = 0x4000;
    constexpr DAddr device_address = 0x10000;
    constexpr GPUVAddr gpu_address = 0x100000;
    constexpr GPUVAddr alias = 0x200000;
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    for (u64 page = 0; page < 2; ++page) {
        const VAddr address = cpu_address + page * 0x1000;
        table.entries.GetAndFault(address >> 12)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing + page * 0x1000) - address);
    }
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    f.device_memory.Map(device_address, cpu_address, 0x2000, asid);
    SCOPE_EXIT {
        f.device_memory.Unmap(device_address, 0x2000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(gpu_address, device_address, 0x2000, Tegra::PTEKind::PITCH, false);
    f.memory.Map(alias, device_address, 0x1000, Tegra::PTEKind::PITCH, false);
    std::memset(backing, 0, 0x2000);

    VideoCommon::GpuSplit split;
    Access::Start(split, f.system);
    SCOPE_EXIT { split.Stop(); };
    const auto state = [&](u32 method, u32 value) {
        split.Method(&f.fast, method, value, true, nullptr);
    };
    const auto upload = [&](GPUVAddr address, u32 size, std::vector<u32> words) {
        state(ShadowControl, 0);
        state(CbAddressHigh, static_cast<u32>(address >> 32));
        state(CbAddressLow, static_cast<u32>(address));
        state(CbSize, size);
        state(CbOffset, 0);
        split.MultiMethod(&f.fast, CbData, words.data(), static_cast<u32>(words.size()),
                          static_cast<u32>(words.size()), nullptr);
        std::ranges::fill(words, 0xdeadbeef);
    };

    upload(gpu_address, 16, {1, 2, 3, 4});
    upload(alias + 4, 4, {20, 30});
    upload(gpu_address + 0x1000, 8, {50, 60});
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    std::array<u32, 4> read{};
    f.memory.ReadBlockUnsafe(gpu_address, read.data(), sizeof(read));
    REQUIRE(read == std::array<u32, 4>{1, 20, 3, 4});
    REQUIRE(f.memory.Read<u32>(alias + 4) == 20);
    REQUIRE(f.memory.Read<u32>(gpu_address + 0x1000) == 50);

    split.Defer([&] {
        f.memory.Map(alias, device_address + 0x1000, 0x1000, Tegra::PTEKind::PITCH, false);
    });
    upload(alias, 4, {70});
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    REQUIRE(f.memory.Read<u32>(gpu_address) == 1);
    REQUIRE(f.memory.Read<u32>(gpu_address + 0x1000) == 70);
    REQUIRE(f.memory.Read<u32>(alias + 4) == 60);
}

TEST_CASE("Split consumption does not complete deferred downstream callbacks",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    VideoCommon::GpuSplit split;
    Access::Start(split, f.system);
    SCOPE_EXIT { split.Stop(); };
    std::vector<std::function<void()>> deferred;
    std::array<u32, 3> output{};
    for (u32 i = 0; i < output.size(); ++i) {
        split.Defer([&, i] { deferred.emplace_back([&, i] { output[i] = i + 1; }); });
    }
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    REQUIRE(deferred.size() == 3);
    REQUIRE(output == std::array<u32, 3>{});
    split.Run([&] {
        for (auto& operation : deferred) {
            operation();
        }
        deferred.clear();
    });
    REQUIRE(output == std::array<u32, 3>{1, 2, 3});
}

// A prototype callback with private code/FIFO and an explicit overlapping dirty-buffer/query
// source. It exercises real split-ring ordering and MME decoding without enabling any title path.
TEST_CASE("Captured FIFO macro keeps its ring position and Stitch read-publication order",
          "[video_core][gpu_split_dependencies][macro_dma_coverage]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    constexpr u32 mode = MAXWELL3D_REG_INDEX(render_enable.mode);
    std::vector<u32> code;
    for (const auto instruction : {FifoInstruction(0x154, false), FifoInstruction(0x155, false),
                                   FifoInstruction(0x158, true), FifoInstruction(mode, false)}) {
        code.insert(code.end(), instruction.begin(), instruction.end());
    }
    REQUIRE(VideoCommon::MacroFifoShape(code, 0, mode) == "linear-fifo-shape");
    REQUIRE(VideoCommon::MacroDmaCoverage::CleanEntry(false, false, false, 2));
    REQUIRE_FALSE(VideoCommon::MacroDmaCoverage::CleanEntry(false, false, true, 2));
    auto persistent = code;
    const auto ram = FifoInstruction(0x157, true);
    std::copy(ram.begin(), ram.end(), persistent.begin() + 6);
    REQUIRE(VideoCommon::MacroFifoShape(persistent, 0, mode) == "dma-to-ram");
    // Last instruction follows END. Both faults terminate the real interpreter after the FIFO
    // read, but before the render-control write. The diagnostic cannot claim a complete shape.
    auto fault = code;
    fault[11] |= 7U << 10; // EXTENDED with zero X, which the interpreter rejects.
    REQUIRE(VideoCommon::MacroFifoShape(fault, 0, mode) == "extended");
    fault = code;
    fault[11] |= 16U << 10; // MERGE, valid destination
    REQUIRE(VideoCommon::MacroFifoShape(fault, 0, mode) == "linear-fifo-shape");
    fault[10] |= 1U << 13; // immediate0 bit 15 in native RAM word order
    REQUIRE(VideoCommon::MacroFifoShape(fault, 0, mode) == "invalid-merge");
    auto parameters = std::vector<u32>{0, 0x4000, 1};
    struct State {
        u32 dirty_word{}, report_word{}, landed_word{}, mode_word{};
        bool success{};
        size_t consumed{};
        std::vector<u32> events;
        bool operator==(const State&) const = default;
    };
    const auto before = [](State& state) {
        state.events.push_back(1);
        state.dirty_word = 11;
        state.report_word = 22;
    };
    const auto after = [](State& state) {
        state.events.push_back(2);
        state.dirty_word = 33;
    };
    const auto interpret = [=](State& state, const auto& image, auto fifo, bool buffer_first) {
        Tegra::TuringMacroInterpreter interpreter;
        const auto result = interpreter.Execute(image, 0, fifo, [](u32) { return 0U; },
            [&](u32 method, u32 value) {
                state.events.push_back(method);
                if (method == 0x158 && value == 1) {
                    const auto buffer = [&] { state.landed_word = state.dirty_word; };
                    const auto query = [&] { state.landed_word = state.report_word; };
                    if (buffer_first) {
                        buffer();
                        query();
                    } else {
                        query();
                        buffer();
                    }
                    fifo.push_back(state.landed_word);
                }
                if (method == mode) {
                    state.mode_word = value;
                }
                return true;
            });
        state.success = result.success;
        state.consumed = result.consumed;
    };
    State front, wrong_order, back;
    before(front);
    interpret(front, code, parameters, true);
    after(front);
    REQUIRE(front.success);
    REQUIRE(front.consumed == 4);
    REQUIRE(front.mode_word == 22);
    before(wrong_order);
    interpret(wrong_order, code, parameters, false);
    after(wrong_order);
    REQUIRE(wrong_order.mode_word == 11);

    Fixture f;
    f.system.Initialize();
    VideoCommon::GpuSplit split;
    Access::Start(split, f.system);
    SCOPE_EXIT { split.Stop(); };
    split.Defer([&] { before(back); });
    split.DeferFunction([&, image = code, fifo = parameters] { interpret(back, image, fifo, true); });
    split.Defer([&] { after(back); });
    code = std::move(persistent);
    parameters.clear();
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    REQUIRE(back == front);
}

TEST_CASE("Split restart resets writer positions without replaying old releases",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    VideoCommon::GpuSplit split;
    u32 released{};
    u32 release_count{};
    for (u32 session = 1; session <= 3; ++session) {
        Access::Start(split, f.system);
        SCOPE_EXIT { split.Stop(); };
        REQUIRE(Access::End(split) == 0);
        REQUIRE_FALSE(split.WaitForWriters());
        REQUIRE_FALSE(split.WaitForSync());
        split.Defer([&, session] {
            released = session;
            ++release_count;
        });
        split.NoteWriter();
        split.NoteSync();
        split.Drain(VideoCommon::DrainSite::Acquire);
        REQUIRE(released == session);
        REQUIRE(release_count == session);
        split.Stop();
        REQUIRE_FALSE(split.Active());
        REQUIRE_FALSE(split.BackRunning());
        REQUIRE(Access::End(split) == 0);
    }
}

TEST_CASE("Maxwell3D takes its macros back from the split's front once the split stops",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    VideoCommon::GpuSplit split;
    Access::Initialize(split, f.system);
    auto& engine = f.fast;
    constexpr u32 StartAddressPointer = MAXWELL3D_REG_INDEX(load_mme.start_address_ptr);
    constexpr u32 StartAddress = MAXWELL3D_REG_INDEX(load_mme.start_address);
    const u32 state = f.state_methods.front();
    // The front takes the macros over and tracks a state write in its shadow RAM copy.
    engine.FrontCallMethod(f.system, split, state, 0x1234, true);
    REQUIRE(engine.shadow_state.reg_array[state] != 0x1234);
    // The split stopped: the next engine method runs on this thread, not the split's back.
    engine.CallMethod(f.system, StartAddressPointer, 0, true);
    engine.CallMethod(f.system, StartAddress, 0x40, true);
    CHECK(engine.shadow_state.reg_array[state] == 0x1234);
    CHECK(engine.GetNativeMacroPositions()[0] == 0x40);
}

TEST_CASE("Maxwell3D keeps state written after the split stops over the front's shadow RAM",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    VideoCommon::GpuSplit split;
    Access::Initialize(split, f.system);
    auto& engine = f.fast;
    const u32 state = f.state_methods.front();
    engine.FrontCallMethod(f.system, split, state, 0x1234, true);
    // After the stop, DmaPusher sinks a state write and consumes the sink before the next
    // executable method: the handback must come first so that write wins.
    engine.method_sink.emplace_back(state, 0x5678);
    engine.ConsumeSink(f.system);
    engine.CallMethod(f.system, MAXWELL3D_REG_INDEX(load_mme.start_address_ptr), 0, true);
    CHECK(engine.shadow_state.reg_array[state] == 0x5678);
    CHECK(engine.regs.reg_array[state] == 0x5678);
}

TEST_CASE("Maxwell3D's split front uploads macro code sent as one multi-method run",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    VideoCommon::GpuSplit split;
    Access::Initialize(split, f.system);
    auto& engine = f.fast;
    engine.SetNativeTuringMacroMode(true);
    // GCC 13's loop unswitching dropped these words from the front's per-word loop (#500).
    constexpr u32 pointer = 0x262;
    constexpr std::array<u32, 6> code{0x740003, 0x18c00000, 0x318c0001,
                                      0x740003, 0x18c00000, 0x318c0000};
    engine.FrontCallMethod(f.system, split, MAXWELL3D_REG_INDEX(load_mme.instruction_ptr), pointer,
                           true);
    engine.FrontCallMultiMethod(f.system, split, MAXWELL3D_REG_INDEX(load_mme.instruction),
                                code.data(), code.size(), code.size());
    const auto& macros = engine.GetNativeMacroEngine();
    REQUIRE(macros.native_write_offset == pointer * 3 + code.size());
    CHECK(std::equal(code.begin(), code.end(), macros.native_code.begin() + pointer * 3));
}

TEST_CASE("Split macro DMA reads wait only for the queued work that can write the words",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    using Writes = VideoCommon::GpuSplit::Writes;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT {
        f.system.Kernel().UnregisterKernelObject(&process);
    };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    constexpr VAddr cpu_address = 0x4000;
    constexpr DAddr device_address = 0x10000;
    constexpr GPUVAddr gpu_address = 0x100000;
    constexpr GPUVAddr alias = 0x200000; // The first page again
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    for (u64 page = 0; page < 2; ++page) {
        const VAddr address = cpu_address + page * 0x1000;
        table.entries.GetAndFault(address >> 12)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing + page * 0x1000) - address);
    }
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    f.device_memory.Map(device_address, cpu_address, 0x2000, asid);
    SCOPE_EXIT {
        f.device_memory.Unmap(device_address, 0x2000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(gpu_address, device_address, 0x2000, Tegra::PTEKind::PITCH, false);
    f.memory.Map(alias, device_address, 0x1000, Tegra::PTEKind::PITCH, false);
    std::memset(backing, 0, 0x2000);
    // The words read below, in two 256-byte granules that do not share a write slot.
    constexpr GPUVAddr word = gpu_address + 0x40;
    constexpr GPUVAddr other = gpu_address + 0x140;
    REQUIRE_FALSE(Access::ShareWriteSlot(device_address + 0x40, device_address + 0x140));

    {
        // Where reads wait: writes are placed by device memory, any GPU address of it.
        VideoCommon::GpuSplit split;
        Access::Initialize(split, f.system);
        Access::EnableWriterFrontier(split);
        split.Defer([] {}, Writes::Noted);
        split.NoteWrite(f.memory, alias + 0x48, 4);
        const u64 noted = Access::End(split);
        CHECK(Access::WritesTarget(split, f.memory, word, 0x18) == noted);
        CHECK(Access::WritesTarget(split, f.memory, other, 0x18) == 0);
        split.Defer([] {}, Writes::Noted);
        split.NoteWrite(f.memory, 0x300000, 16); // Incomplete mappings cannot narrow a writer.
        CHECK(Access::WritesTarget(split, f.memory, other, 0x18) == Access::End(split));
        // Writes too long to place, and work nobody noted the writes of, count as anywhere.
        split.Defer([] {}, Writes::Noted);
        split.NoteWrite(f.memory, gpu_address + 0x1000, 0x8000);
        CHECK(Access::WritesTarget(split, f.memory, other, 0x18) == Access::End(split));
        split.Defer([] {});
        CHECK(Access::WritesTarget(split, f.memory, other, 0x18) == Access::End(split));
    }

    // The 3D engine's front, against a running back: macro 0 reads one word with
    // MME_DMA_READ_FIFOED at the address in its first two parameters and sends it to a scratch
    // register (the load after the read takes the word).
    auto& engine = f.fast;
    engine.SetNativeTuringMacroMode(true);
    constexpr u32 state = MAXWELL3D_REG_INDEX(shadow_scratch) + 1;
    VideoCommon::GpuSplit split;
    Access::Start(split, f.system);
    SCOPE_EXIT {
        split.Stop();
    };
    Access::EnableWriterFrontier(split);
    std::array<std::atomic<bool>, 7> release{};
    std::atomic<bool> done{};
    std::thread reader;
    SCOPE_EXIT {
        for (auto& flag : release) {
            flag = true;
        }
        if (reader.joinable()) {
            reader.join();
        }
    };
    const auto front = [&](u32 method, u32 argument) {
        engine.FrontCallMethod(f.system, split, method, argument, true);
    };
    std::vector<u32> code;
    for (const auto instruction : {FifoInstruction(0x154, false), FifoInstruction(0x155, false),
                                   FifoInstruction(0x158, true), FifoInstruction(state, false)}) {
        code.insert(code.end(), instruction.begin(), instruction.end());
    }
    front(MAXWELL3D_REG_INDEX(load_mme.instruction_ptr), 0);
    engine.FrontCallMultiMethod(f.system, split, MAXWELL3D_REG_INDEX(load_mme.instruction),
                                code.data(), static_cast<u32>(code.size()),
                                static_cast<u32>(code.size()));
    front(MAXWELL3D_REG_INDEX(load_mme.start_address_ptr), 0);
    front(MAXWELL3D_REG_INDEX(load_mme.start_address), 0);
    const auto dma_read = [&](GPUVAddr address) {
        engine.FrontCallMethod(f.system, split, 0xe00, static_cast<u32>(address >> 32), false);
        engine.FrontCallMethod(f.system, split, 0xe01, static_cast<u32>(address), false);
        engine.FrontCallMethod(f.system, split, 0xe01, 1, true);
    };
    const auto read_on_reader = [&](GPUVAddr address) {
        done = false;
        reader = std::thread([&, address] {
            dma_read(address);
            done = true;
        });
    };
    const auto finishes = [&done] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return done.load();
    };
    const auto waits = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!Access::FrontWaits(split) && !done.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        return !done.load();
    };
    // A constant buffer word, sent through the front.
    const auto upload = [&](GPUVAddr address, u32 value) {
        front(CbAddressHigh, static_cast<u32>(address >> 32));
        front(CbAddressLow, static_cast<u32>(address));
        front(CbSize, 0x100);
        front(CbOffset, 0);
        front(CbData, value);
    };
    // An entry the back stays in until its flag is set.
    const auto blocking = [&split](std::atomic<bool>& flag, Writes writes) {
        split.Defer(
            [&flag] {
                while (!flag.load()) {
                    std::this_thread::yield();
                }
            },
            writes);
        split.Publish();
    };

    // The first macro also ends the front's assumption that a draw may be pending.
    upload(word, 0x11111111);
    upload(other, 0x22222222);
    dma_read(word);
    split.Drain(VideoCommon::DrainSite::Other);
    REQUIRE(engine.regs.reg_array[state] == 0x11111111);

    // Work that writes nothing and a write of other words, still queued: the read goes ahead.
    blocking(release[0], Writes::Noted);
    upload(other, 0x33333333);
    read_on_reader(word);
    REQUIRE(finishes());
    reader.join();
    release[0] = true;
    split.Drain(VideoCommon::DrainSite::Other);
    CHECK(engine.regs.reg_array[state] == 0x11111111);
    CHECK(f.memory.Read<u32>(other) == 0x33333333);

    // A write of the word through its other GPU address: the read waits for it and sees it.
    blocking(release[1], Writes::Noted);
    upload(alias + 0x40, 0x44444444);
    read_on_reader(word);
    REQUIRE(waits());
    release[1] = true;
    REQUIRE(finishes());
    reader.join();
    split.Drain(VideoCommon::DrainSite::Other);
    CHECK(engine.regs.reg_array[state] == 0x44444444);

    // Work whose writes are not noted: the read waits for it.
    blocking(release[2], Writes::Anywhere);
    read_on_reader(other);
    REQUIRE(waits());
    release[2] = true;
    REQUIRE(finishes());
    reader.join();
    split.Drain(VideoCommon::DrainSite::Other);
    CHECK(engine.regs.reg_array[state] == 0x33333333);

    // A 3D draw can first write any SSBO, including through the emulated task/mesh stages.
    // Its query page need not have been marked GPU-written before execution on the back.
    front(MAXWELL3D_REG_INDEX(draw.begin), 0);
    CHECK(Access::WritesTarget(split, f.memory, other, 16) == Access::End(split));
    front(MAXWELL3D_REG_INDEX(draw.end), 0);
    CHECK(Access::WritesTarget(split, f.memory, word, 16) == Access::End(split));
    // A macro closes the front's draw-pending window by queuing FinishFrontMacro.
    dma_read(word);
    split.Drain();

    // Distinct device pages alias the same physical RAM. The buffer/query caches are keyed
    // by device address, so the baseline must also run when there is no pending noted writer.
    constexpr DAddr second_device = 0x30000;
    constexpr GPUVAddr second_gpu = 0x300000;
    REQUIRE_FALSE(Access::ShareWriteSlot(device_address + 0x40, second_device + 0x40));
    {
        f.device_memory.Map(second_device, cpu_address, 0x1000, asid);
        SCOPE_EXIT { f.device_memory.Unmap(second_device, 0x1000); };
        f.memory.Map(second_gpu, second_device, 0x1000, Tegra::PTEKind::PITCH, false);
        REQUIRE(f.memory.GpuToCpuAddress(word) != f.memory.GpuToCpuAddress(second_gpu + 0x40));
        REQUIRE(f.device_memory.GetPointer<u8>(device_address + 0x40) ==
                f.device_memory.GetPointer<u8>(second_device + 0x40));
        blocking(release[5], Writes::Noted);
        upload(word, 0xabcdef01);
        read_on_reader(second_gpu + 0x40);
        CHECK(waits());
        release[5] = true;
        REQUIRE(finishes());
        reader.join();
        split.Drain();
        CHECK(engine.regs.reg_array[state] == 0xabcdef01);
        blocking(release[6], Writes::Noted);
        read_on_reader(second_gpu + 0x40);
        CHECK(waits());
        release[6] = true;
        REQUIRE(finishes());
        reader.join();
        split.Drain();
    }

    // Deliberately misclassify a writer to exercise the verification fail-safe. The front reads
    // the old value, waits in the reference path, then must deliver the *new* value to the MME.
    Access::EnableWriterFrontier(split, true);
    split.Defer([&] {
        while (!release[3].load()) {
            std::this_thread::yield();
        }
        f.memory.Write<u32>(word, 0x55555555);
    }, Writes::Noted);
    split.Publish();
    read_on_reader(word);
    REQUIRE(waits());
    release[3] = true;
    REQUIRE(finishes());
    reader.join();
    split.Drain();
    CHECK(engine.regs.reg_array[state] == 0x55555555);
    REQUIRE_FALSE(split.TracksMemoryWrites());
    // The latched mismatch survives later reads and marker polls.
    blocking(release[4], Writes::Noted);
    read_on_reader(other);
    REQUIRE(waits());
    release[4] = true;
    REQUIRE(finishes());
    reader.join();
    split.Drain();
    CHECK(engine.regs.reg_array[state] == 0x33333333);
}

TEST_CASE("MME fast reads require stable unique physical backing on every source page",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    constexpr VAddr cpu = 0x4000;
    constexpr DAddr device = 0x10000, second = 0x30000;
    constexpr GPUVAddr gpu = 0x100000;
    auto* backing = f.physical.GetPointerFromRaw<u8>(0);
    for (u64 i = 0; i < 2; ++i) {
        const auto address = cpu + 0x1000 * i;
        table.entries.GetAndFault(address >> 12)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing + 0x1000 * i) - address);
    }
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(gpu, device, 0x2000, Tegra::PTEKind::PITCH, false);
    f.device_memory.Map(device, cpu, 0x1000, asid);
    SCOPE_EXIT {
        f.device_memory.Unmap(device, 0x1000);
        f.device_memory.UnregisterProcess(asid);
    };
    REQUIRE(f.device_memory.GetPhysicalRawAddressFromDAddr(device) == 0);
    REQUIRE(f.device_memory.GetPhysicalRawAddressFromDAddr(device + 0x1000) == 0);
    const u64 first = f.memory.DeviceMappingGeneration();
    f.memory.Write<u32>(gpu, 0xe145f127);
    f.memory.Write<u32>(gpu + 0xffc, 0xaaaabbbb);
    std::array<u32, 2> words{};
    REQUIRE(f.memory.TryReadUniqueWords(gpu, words, first)); // Physical page zero really exists.
    CHECK(words[0] == 0xe145f127);
    REQUIRE_FALSE(f.memory.TryReadUniqueWords(gpu + 0xffc, words, first)); // Second PTE missing.
    f.device_memory.Map(device + 0x1000, cpu + 0x1000, 0x1000, asid);
    SCOPE_EXIT { f.device_memory.Unmap(device + 0x1000, 0x1000); };
    const auto next = f.memory.DeviceMappingGeneration();
    f.memory.Write<u32>(gpu + 0x1000, 0xccccdddd);
    REQUIRE_FALSE(f.memory.TryReadUniqueWords(gpu + 0xffc, words, first)); // Stale cache checks.
    REQUIRE(f.memory.TryReadUniqueWords(gpu + 0xffc, words, next));
    CHECK(words[0] == 0xaaaabbbb);
    CHECK(words[1] == 0xccccdddd);

    VideoCommon::GpuSplit split;
    Access::Initialize(split, f.system);
    Access::EnableWriterFrontier(split);
    split.Defer([] {}, VideoCommon::GpuSplit::Writes::Noted);
    split.NoteWrite(f.memory, gpu + 0x40, 4);
    const auto pending = Access::End(split);
    CHECK(Access::WritesTarget(split, f.memory, gpu + 0x140, 4) == 0);
    f.device_memory.Map(second, cpu + 0x1000, 0x1000, asid);
    SCOPE_EXIT { f.device_memory.Unmap(second, 0x1000); };
    const auto shared = f.memory.DeviceMappingGeneration();
    REQUIRE_FALSE(f.memory.TryReadUniqueWords(gpu + 0xffc, words, shared)); // Only last page aliased.
    CHECK(Access::WritesTarget(split, f.memory, gpu + 0x140, 4) == pending);
    split.Defer([] {}, VideoCommon::GpuSplit::Writes::Noted);
    split.NoteWrite(f.memory, gpu + 0x240, 4);
    CHECK(Access::WritesTarget(split, f.memory, gpu + 0x140, 4) == pending);
}

// A real split ring, native engine and dual publication. The old dirty buffer disagrees with
// the held report. Reversing the publication order changes the architectural condition.
TEST_CASE("Ordered FIFO macros preserve report order, tracked state and captured command words",
          "[video_core][ordered_fifo_ring]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    using Split = VideoCommon::GpuSplit;
    constexpr u32 render = MAXWELL3D_REG_INDEX(render_enable.mode);
    constexpr auto word = GPUVAddr{0x100020};
    constexpr auto device = DAddr{0x10020};
    enum class Route {
        Original,
        Verify,
        Candidate,
        StopHandoff,
        ObserveDirect,
        BorrowedWord,
        DeferredRefresh,
        SuspensionRefresh,
        PendingSink,
        Instance,
        Alias,
        Replay
    };
    const auto route = GENERATE(Route::Original, Route::Verify, Route::Candidate,
                                Route::PendingSink, Route::Instance, Route::Alias, Route::Replay,
                                Route::StopHandoff, Route::ObserveDirect, Route::BorrowedWord,
                                Route::DeferredRefresh, Route::SuspensionRefresh);
    const bool deferred =
        route == Route::Candidate || route == Route::StopHandoff || route == Route::ObserveDirect;
    CAPTURE(static_cast<int>(route));
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT {
        f.system.Kernel().UnregisterKernelObject(&process);
    };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    constexpr VAddr cpu = 0x4000;
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    table.entries.GetAndFault(cpu >> 12).Store(false, Common::PageType::Memory, 1,
                                               reinterpret_cast<uintptr_t>(backing) - cpu);
    process.GetMemory().SetCurrentPageTable(process);
    auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    struct Reports final : Null::RasterizerNull {
        Reports(Tegra::GPU& gpu, u8* bytes) : RasterizerNull{gpu}, dest{bytes} {}
        bool MustFlushRegion(DAddr at, u64 bytes, VideoCommon::CacheType kind) override {
            return at == device && bytes == 24 && kind == VideoCommon::CacheType::BufferCache;
        }
        void FlushRegion(DAddr at, u64 bytes, VideoCommon::CacheType kind) override {
            if (!MustFlushRegion(at, bytes, kind))
                return;
            events.push_back(1);
            const std::array<u32, 6> unequal{1, 2, 3, 4, 5, 6};
            std::memcpy(dest, unequal.data(), 24);
        }
        void FlushQueryResults(DAddr at, u64 bytes) override {
            if (at != device || bytes != 24)
                return;
            events.push_back(2);
            // Completing a query fence may also publish a different query-only command word.
            if (command_word)
                *command_word = 0x531;
            const std::array<u32, 6> equal{19, 23, 61, 71, 19, 23};
            std::memcpy(dest, equal.data(), 24);
        }
        u32* command_word{};
        u8* dest;
        std::vector<u32> events;
    } reports{f.gpu, backing + 0x20};
    f.device_memory.BindInterface(&reports);
    f.device_memory.Map(0x10000, cpu, 0x1000, asid);
    f.memory.BindRasterizer(&reports);
    f.memory.Map(word - 0x20, 0x10000, 0x1000, Tegra::PTEKind::PITCH, false);
    SCOPE_EXIT {
        f.device_memory.Unmap(0x10000, 0x1000);
        f.device_memory.UnregisterProcess(asid);
    };
    auto& split = Split::Get();
    split.OrderedFifoStats() = {};
    Access::OrderedMode(split, Split::OrderedFifoMode::Off);
    Access::Start(split, f.system);
    SCOPE_EXIT {
        split.Stop();
    };
    auto& engine = f.fast;
    engine.BindRasterizer(&reports);
    engine.SetNativeTuringMacroMode(true);
    const auto one = [&](u32 m, u32 v) { engine.FrontCallMethod(f.system, split, m, v, true); };
    const auto upload = [&](u32 start, const std::vector<u32>& native) {
        one(MAXWELL3D_REG_INDEX(load_mme.instruction_ptr), start);
        engine.FrontCallMultiMethod(f.system, split, MAXWELL3D_REG_INDEX(load_mme.instruction),
                                    native.data(), static_cast<u32>(native.size()),
                                    static_cast<u32>(native.size()));
    };
    const auto call = [&](u32 p0) {
        engine.FrontCallMethod(f.system, split, 0xe00, p0, false);
        engine.FrontCallMethod(f.system, split, 0xe01, 0, false);
        engine.FrontCallMethod(f.system, split, 0xe01, static_cast<u32>(word), true);
    };
    one(ShadowControl, 1);
    const auto original = OrderedFifoTest::Instructions(render);
    upload(0, OrderedFifoTest::Encode(original));
    constexpr u32 witness = MAXWELL3D_REG_INDEX(shadow_scratch) + 1;
    using Insn = OrderedFifoTest::Instruction;
    // This independent macro observes the *frontend* MME register file, then publishes
    // that value through an unrelated tracked register, making stale mirrors observable.
    upload(64, OrderedFifoTest::Encode(
                   {Insn{.a = {22, 0, 25, 24}, .imm = render, .end = true},
                    Insn{.a = {0, 24, 0, 24}, .imm = witness, .method = 5, .output = 1}}));
    one(MAXWELL3D_REG_INDEX(load_mme.start_address_ptr), 0);
    one(MAXWELL3D_REG_INDEX(load_mme.start_address), 0);
    one(MAXWELL3D_REG_INDEX(load_mme.start_address), 64);
    call(0);
    split.Drain();
    REQUIRE(reports.events == std::vector<u32>{1, 2});
    REQUIRE(engine.regs.reg_array[render] == 0);
    reports.events.clear();
    Access::OrderedMode(split, Split::OrderedFifoMode::Verify);
    const auto init_scope = split.EnterFifoCommandScope(Split::CommandBacking::Owned);
    call(0); // Every new exact code image must match its original call before it may move.
    split.LeaveFifoCommandScope(init_scope);
    split.Drain();
    REQUIRE(split.OrderedFifoStats().checked == 1);
    reports.events.clear();
    Access::OrderedMode(split, route == Route::Original ? Split::OrderedFifoMode::Off
                               : route == Route::Verify ? Split::OrderedFifoMode::Verify
                                                        : Split::OrderedFifoMode::On);
    if (route == Route::BorrowedWord || route == Route::DeferredRefresh ||
        route == Route::SuspensionRefresh) {
        using namespace Tegra;
        Control::ChannelState channel{1};
        channel.memory_manager = &f.memory;
        DmaPusher parser{f.system, f.memory, channel};
        parser.BindRasterizer(&reports);
        parser.BindSubchannel(&engine, 0, Engines::EngineTypes::Maxwell3D);
        const auto header = [](u32 method, u32 count, SubmissionMode mode) {
            CommandHeader h{};
            h.method.Assign(method);
            h.method_count.Assign(count);
            h.mode.Assign(mode);
            return h;
        };
        const auto list = [](GPUVAddr address, u32 count) {
            CommandList out;
            CommandListHeader entry{};
            entry.addr.Assign(address);
            entry.size.Assign(count);
            out.command_lists.push_back(entry);
            return out;
        };
        constexpr GPUVAddr commands_address = 0x100300;
        auto* live = reinterpret_cast<CommandHeader*>(backing + 0x300);
        const std::array words{header(0xe00, 3, SubmissionMode::IncreaseOnce),
                               CommandHeader{.argument = 1},
                               CommandHeader{.argument = 0},
                               CommandHeader{.argument = static_cast<u32>(word)},
                               header(witness, 1, SubmissionMode::NonIncreasing),
                               CommandHeader{.argument = 0}};
        std::copy(words.begin(), words.end(), live);
        reports.command_word = &live[5].argument;
        const bool borrowed = route == Route::BorrowedWord;
        auto first = list(commands_address, borrowed ? 6 : 4);
        if (!borrowed) {
            first.submitted_offsets = {0};
            first.submitted_words.assign(words.begin(), words.begin() + 4);
        }
        parser.Push(std::move(first));
        std::atomic<bool> entered{}, release{}, returned{};
        split.Defer([&] {
            entered.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
                std::this_thread::yield();
        });
        split.Publish();
        while (!entered.load(std::memory_order_acquire))
            std::this_thread::yield();
        u32 refreshed{};
        std::thread front([&] {
            DmaPusherTestAccess::Step(parser);
            if (!borrowed) {
                auto following = list(commands_address + 16, 2);
                following.submitted_offsets = {0};
                following.submitted_words.resize(2);
                if (route == Route::DeferredRefresh)
                    following.deferred_entries = {0};
                else
                    following.snapshot_suspensions = 2;
                parser.Push(std::move(following));
                refreshed = parser.dma_pushbuffer.back().submitted_words[1].argument;
            }
            returned.store(true, std::memory_order_release);
        });
        SCOPE_EXIT {
            release.store(true, std::memory_order_release);
            if (front.joinable())
                front.join();
            split.Drain();
        };
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!returned.load(std::memory_order_acquire) && !Access::FrontWaits(split) &&
               std::chrono::steady_clock::now() < limit)
            std::this_thread::yield();
        REQUIRE(Access::FrontWaits(split));
        REQUIRE_FALSE(returned.load(std::memory_order_acquire));
        release.store(true, std::memory_order_release);
        front.join();
        split.Drain();
        REQUIRE(reports.events == std::vector<u32>{1, 2});
        REQUIRE(split.OrderedFifoStats().deferred == (borrowed ? 0U : 1U));
        if (borrowed) {
            REQUIRE(split.OrderedFifoStats().borrowed == 1);
            // Sunk state writes become architectural values when consumed.
            split.Run([&] { engine.ConsumeSink(f.system); });
            REQUIRE(engine.regs.reg_array[witness] == 0x531);
        } else {
            REQUIRE(refreshed == 0x531);
            REQUIRE(split.OrderedFifoStats().groups == 1);
            REQUIRE(split.OrderedFifoStats().waited_groups == 1);
        }
        return;
    }
    const auto scope = split.EnterFifoCommandScope(Split::CommandBacking::Owned);
    SCOPE_EXIT {
        split.LeaveFifoCommandScope(scope);
    };
    if (route == Route::Alias)
        f.device_memory.Map(0x20000, cpu, 0x1000, asid);
    SCOPE_EXIT {
        if (route == Route::Alias)
            f.device_memory.Unmap(0x20000, 0x1000);
    };
    if (route == Route::PendingSink)
        one(MAXWELL3D_REG_INDEX(shadow_scratch) + 1, 526);
    if (route == Route::Instance)
        one(DrawBegin, 1U << 26);
    if (route == Route::Replay)
        one(ShadowControl, 3);
    std::atomic<bool> entered{}, release{}, returned{};
    split.Defer([&] {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
    });
    split.Publish();
    while (!entered.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::thread frontend([&] {
        call(1);
        returned.store(true, std::memory_order_release);
    });
    SCOPE_EXIT {
        release.store(true, std::memory_order_release);
        if (frontend.joinable())
            frontend.join();
        split.Drain();
    };
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!returned.load(std::memory_order_acquire) && !Access::FrontWaits(split) &&
           std::chrono::steady_clock::now() < until)
        std::this_thread::yield();
    if (deferred) {
        REQUIRE(returned.load(std::memory_order_acquire));
        frontend.join();
        // Rebind and mutate the backing before the deferred callback is allowed to execute.
        // The old front, back and proof cache must agree on the call that was already captured.
        auto changed = original;
        changed.back().output = 6;
        changed.back().imm2 = 0;
        upload(0, OrderedFifoTest::Encode(changed));
    } else {
        REQUIRE(Access::FrontWaits(split));
        REQUIRE_FALSE(returned.load(std::memory_order_acquire));
    }
    u32 seen_by_following_work{};
    if (deferred) {
        split.Defer([&] {
            seen_by_following_work = engine.regs.reg_array[render];
            // A later operation may reuse the query address. The earlier call must have read it.
            const std::array<u32, 6> overwritten{10, 20, 30, 40, 50, 60};
            std::memcpy(backing + 0x20, overwritten.data(), sizeof(overwritten));
        });
    }
    release.store(true, std::memory_order_release);
    if (frontend.joinable())
        frontend.join();
    split.Drain();
    REQUIRE(reports.events == std::vector<u32>{1, 2}); // VERIFY cannot run either effect twice.
    REQUIRE(engine.regs.reg_array[render] == (route == Route::Replay ? 0 : 1));
    REQUIRE(split.OrderedFifoStats().deferred == (deferred ? 1U : 0U));
    REQUIRE(split.OrderedFifoStats().checked == (route == Route::Verify ? 2U : 1U));
    REQUIRE(split.OrderedFifoStats().mismatches == 0);
    if (deferred) {
        REQUIRE(seen_by_following_work == 1);
    }
    if (route == Route::StopHandoff) {
        split.Stop();
        engine.CallMethod(f.system, ShadowControl, 3, true);
        engine.CallMethod(f.system, render, 0, true);
        REQUIRE(engine.regs.reg_array[render] == 1);
        REQUIRE(engine.shadow_state.reg_array[render] == 1);
    }
    if (route == Route::ObserveDirect) {
        one(0xe02, 0);
        split.Drain();
        REQUIRE(engine.regs.reg_array[witness] == 1);
        REQUIRE(split.OrderedFifoStats().reconciled == 1);
    }
    if (route == Route::Candidate) {
        // Track->Replay must use the backend-owned result, and later out-of-split handoff
        // must never copy a stale frontend shadow over it.
        one(ShadowControl, 3);
        one(render, 0);
        split.Drain();
        REQUIRE(engine.regs.reg_array[render] == 1);
        REQUIRE(engine.shadow_state.reg_array[render] == 1);
        one(ShadowControl, 1);
        one(0xe02, 0);
        split.Drain();
        REQUIRE(engine.regs.reg_array[witness] == 1);
        call(1); // Mutated code now emits a different constant and must use the ordinary path.
        split.Drain();
        REQUIRE(engine.regs.reg_array[render] == 0);
    }
}

TEST_CASE("Ordered FIFO mode file requires explicit verified promotion and fails to baseline",
          "[video_core][ordered_fifo_ring]") {
    using Split = VideoCommon::GpuSplit;
    using Mode = Split::OrderedFifoMode;
    using Access = VideoCommon::GpuSplitTestAccess;
    Split split;
    Access::OrderedMode(split, Mode::Verify);
    REQUIRE(split.SelectOrderedFifoMode() == Mode::Verify);
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("ordered-fifo-marker-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(directory));
    SCOPE_EXIT {
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
    };
    const auto path = (directory / "mode").string();
    const auto select = [&] {
        Access::FifoMarker(split, path.c_str());
        return split.SelectOrderedFifoMode();
    };
    REQUIRE(select() == Mode::Off);
    std::ofstream(path) << "candidate\n";
    REQUIRE(select() == Mode::Verify);
    split.OrderedFifoStats().checked = 1;
    REQUIRE(select() == Mode::On);
    std::ofstream(path) << "baseline\n";
    REQUIRE(split.SelectOrderedFifoMode() == Mode::On); // sampled mode captured until next poll
    REQUIRE(select() == Mode::Off);
    std::ofstream(path) << "verify\n";
    REQUIRE(select() == Mode::Verify);
    std::ofstream(path) << "candidate\npartial";
    REQUIRE(select() == Mode::Off);
    std::filesystem::remove(path);
    REQUIRE(select() == Mode::Off);
    std::ofstream(path) << "candidate";
    split.RejectOrderedFifo();
    REQUIRE(select() == Mode::Off);
}
