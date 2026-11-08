// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <spirv/unified1/spirv.hpp11>

#include "common/scope_exit.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"

namespace {
using namespace Shader;

enum class Case {
    Independent,
    DifferentA,
    EarlierWord,
    EarlierVector,
    Barrier,
    Predicated,
    K8,
    ReuseBoth,
    ReorderedA,
    ChangedB,
    SignedZero,
    ReuseBarrier,
    ReusePredicate,
    ReuseK8,
    HalfZero,
    ReuseLongGap,
    ReloadedA,
    PairThenSolo,
    SoloThenPair,
    SoloThenSolo
};

// One subgroup, one storage buffer. Each lane owns A[4], B[2], C[2], B'[2], C'[2], D[2], D'[2].
std::vector<u32> Module(bool pair, Case scenario, bool reuse = false) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool}, conditional{pool}, exit{pool};
    IR::IREmitter ir{block};
    const bool reuse_case = scenario >= Case::ReuseBoth;
    const IR::U32 offset_base = ir.IMul(ir.LocalInvocationIdX(), ir.Imm32(reuse_case ? 80 : 64));
    std::array<IR::Value, 12> words;
    std::array<IR::U32, 20> offsets;
    for (size_t i = 0; i < (reuse_case ? 20U : 16U); ++i) {
        offsets[i] = IR::U32{ir.IAdd(offset_base, ir.Imm32(static_cast<u32>(4 * i)))};
        if (i < words.size()) {
            block.AppendNewInst(IR::Opcode::LoadStorage32, {ir.Imm32(0), offsets[i]});
            words[i] = IR::Value{&block.back()};
        }
    }
    const auto a = ir.CompositeConstruct(words[0], words[1], words[2], words[3]);
    const auto b = ir.CompositeConstruct(words[4], words[5]);
    const auto c = ir.CompositeConstruct(words[6], words[7]);
    std::array<IR::Value, 4> warm{};
    IR::Value second_first_word = words[0];
    IR::Value warm_accumulator;
    const bool warm_solo = scenario == Case::SoloThenPair || scenario == Case::SoloThenSolo;
    const bool test_solo = scenario == Case::PairThenSolo || scenario == Case::SoloThenSolo;
    if (reuse_case) {
        const auto wb = ir.CompositeConstruct(words[8], words[9]);
        const auto wc = ir.CompositeConstruct(words[10], words[11]);
        const auto warm_low = ir.TensorMulAdd(a, b, c, 16);
        const auto warm_high = warm_solo ? warm_low : ir.TensorMulAdd(a, wb, wc, 16);
        warm_accumulator = warm_low;
        warm = {ir.CompositeExtract(warm_low, 0), ir.CompositeExtract(warm_low, 1),
                ir.CompositeExtract(warm_high, 0), ir.CompositeExtract(warm_high, 1)};
        if (scenario == Case::ReuseBarrier) {
            ir.Barrier();
        }
        if (scenario == Case::ReloadedA) {
            block.AppendNewInst(IR::Opcode::LoadStorage32, {ir.Imm32(0), offsets[0]});
            second_first_word = IR::Value{&block.back()};
        }
        if (scenario == Case::ReuseLongGap) {
            for (int i = 0; i < 40; ++i) {
                block.AppendNewInst(IR::Opcode::Identity, {words[0]});
            }
        }
    }
    const bool conditional_reuse = scenario == Case::ReusePredicate;
    IR::IREmitter first{conditional_reuse ? conditional : block};
    const auto fa =
        reuse_case
            ? first.CompositeConstruct(scenario == Case::ReorderedA ? words[1] : second_first_word,
                                       scenario == Case::ReorderedA ? words[0] : words[1], words[2],
                                       words[3])
            : a;
    const auto zero = first.Imm32(0);
    const auto fc =
        warm_solo ? warm_accumulator : (reuse_case ? first.CompositeConstruct(zero, zero) : c);
    const auto d = first.TensorMulAdd(fa, b, fc, scenario == Case::ReuseK8 ? 8 : 16);
    const auto low0 = first.CompositeExtract(d, 0);
    const auto low1 = first.CompositeExtract(d, 1);
    if (scenario == Case::Barrier) {
        ir.Barrier();
    }
    IR::Block& next = (scenario == Case::Predicated || conditional_reuse) ? conditional : block;
    IR::IREmitter later{next};
    const auto a1 =
        later.CompositeConstruct(scenario == Case::ReorderedA ? words[1] : second_first_word,
                                 scenario == Case::ReorderedA ? words[0] : words[1], words[2],
                                 scenario == Case::DifferentA ? words[9] : words[3]);
    const auto b1 = later.CompositeConstruct(
        scenario == Case::EarlierWord ? low1 : (scenario == Case::ChangedB ? words[9] : words[8]),
        words[9]);
    const auto c1 =
        scenario == Case::EarlierVector
            ? d
            : (reuse_case && scenario != Case::HalfZero
                   ? later.CompositeConstruct(
                         later.Imm32(scenario == Case::SignedZero ? 0x80000000u : 0u), zero)
                   : later.CompositeConstruct(words[10], words[11]));
    const auto d1 = test_solo ? d : later.TensorMulAdd(a1, b1, c1, scenario == Case::K8 ? 8 : 16);
    std::array output{low0, low1, later.CompositeExtract(d1, 0), later.CompositeExtract(d1, 1)};
    for (size_t i = 0; i < output.size(); ++i) {
        if (reuse_case) {
            next.AppendNewInst(IR::Opcode::WriteStorage32, {ir.Imm32(0), offsets[16 + i], warm[i]});
        }
        next.AppendNewInst(IR::Opcode::WriteStorage32, {ir.Imm32(0), offsets[12 + i], output[i]});
    }

    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {32, 1, 1};
    using Node = IR::AbstractSyntaxNode;
    program.blocks = {&block};
    program.syntax_list = {{{.block = &block}, Node::Type::Block}};
    if (scenario == Case::Predicated || conditional_reuse) {
        program.blocks.push_back(&conditional);
        program.blocks.push_back(&exit);
        program.syntax_list.push_back(
            {{.if_node = {ir.INotEqual(IR::U32{words[11]}, ir.Imm32(0)), &conditional, &exit}},
             Node::Type::If});
        program.syntax_list.push_back({{.block = &conditional}, Node::Type::Block});
        program.syntax_list.push_back({{.end_if = {&exit}}, Node::Type::EndIf});
        program.syntax_list.push_back({{.block = &exit}, Node::Type::Block});
    }
    program.syntax_list.push_back({{}, Node::Type::Return});
    program.post_order_blocks = program.blocks;
    auto& info = program.info;
    info.uses_local_invocation_id = true;
    info.uses_subgroup_invocation_id = true;
    info.storage_buffers_descriptors.push_back({.count = 1, .is_written = true});
    info.used_storage_buffer_types = IR::Type::U32;
    Profile profile{};
    profile.supported_spirv = 0x00010400;
    profile.support_cooperative_hmma_16x16x16 = true;
    profile.pair_cooperative_hmma_columns = pair;
    profile.reuse_cooperative_hmma_operands = reuse;
    profile.max_compute_shared_memory_size = 16384;
    profile.max_subgroup_size = 32;
    profile.unified_descriptor_binding = true;
    return Backend::SPIRV::EmitSPIRV(profile, program);
}

size_t CountOpcode(const std::vector<u32>& module, spv::Op opcode) {
    size_t results = 0;
    for (size_t i = 5; i < module.size();) {
        const size_t size = module[i] >> 16;
        REQUIRE(size > 0);
        REQUIRE(i + size <= module.size());
        results += (module[i] & 0xffff) == static_cast<u32>(opcode);
        i += size;
    }
    return results;
}
size_t CountMultiplyAdds(const std::vector<u32>& module) {
    return CountOpcode(module, spv::Op::OpCooperativeMatrixMulAddKHR);
}
} // namespace

TEST_CASE("Independent tensor multiply-adds share eight spare cooperative matrix columns",
          "[tensor_column_pair]") {
    for (const bool paired : {false, true}) {
        const auto module = Module(paired, Case::Independent);
        REQUIRE(CountMultiplyAdds(module) == (paired ? 1 : 2));
        if (const char* output = std::getenv("STITCH_TENSOR_TEST_OUTPUT")) {
            std::filesystem::create_directories(output);
            const auto path =
                std::filesystem::path{output} / (paired ? "paired.spv" : "baseline.spv");
            std::ofstream stream(path, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(module.data()),
                         static_cast<std::streamsize>(module.size() * sizeof(u32)));
            REQUIRE(stream.good());
        }
    }
    for (const Case rejected : {Case::DifferentA, Case::EarlierWord, Case::EarlierVector,
                                Case::Barrier, Case::Predicated, Case::K8}) {
        CAPTURE(static_cast<int>(rejected));
        REQUIRE(CountMultiplyAdds(Module(true, rejected)) == 2);
    }
}

TEST_CASE("Tensor marker selection is bounded and keeps previously recorded choices",
          "[tensor_column_pair]") {
    const auto directory = std::filesystem::temp_directory_path() /
                           fmt::format("stitch-tensor-mode-{}",
                                       std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(directory);
    SCOPE_EXIT {
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
    };
    const auto marker = (directory / "baseline").string();
    {
        std::ofstream present{marker};
        REQUIRE(present.good());
    }
    Vulkan::TensorPipelineMode mode, off, unsupported;
    off.Initialize(true, nullptr, marker.c_str());
    unsupported.Initialize(false, "1", marker.c_str());
    CHECK_FALSE(off.Enabled());
    CHECK_FALSE(unsupported.Enabled());
    mode.Initialize(true, "1", marker.c_str());
    REQUIRE(mode.Enabled());
    CHECK_FALSE(mode.Paired());
    const bool queued_original = mode.Paired();
    std::filesystem::remove(marker);
    for (u64 frame = 1; frame < 16; ++frame) {
        mode.TickFrame(frame);
        CHECK_FALSE(mode.Paired());
    }
    mode.TickFrame(16);
    REQUIRE(mode.Paired());
    const bool queued_paired = mode.Paired();
    {
        std::ofstream present{marker};
        REQUIRE(present.good());
    }
    for (u64 frame = 17; frame < 32; ++frame) {
        mode.TickFrame(frame);
        CHECK(mode.Paired());
    }
    mode.TickFrame(32);
    CHECK_FALSE(mode.Paired());
    CHECK_FALSE(queued_original);
    CHECK(queued_paired);
}

TEST_CASE("Bounded tensor operand reuse respects exact SSA words, shape and scope",
          "[tensor_reuse]") {
    struct Expect {
        Case example;
        const char* name;
        int saved;
    };
    for (const auto [scenario, name, saved] :
         {Expect{Case::ReuseBoth, "both", 3}, Expect{Case::ChangedB, "changed_b", 2},
          Expect{Case::ReorderedA, "reordered_a", 2}, Expect{Case::SignedZero, "signed_zero", 2},
          Expect{Case::HalfZero, "half_zero", 2}, Expect{Case::ReuseBarrier, "barrier", 1},
          Expect{Case::ReusePredicate, "predicate", 1}, Expect{Case::ReuseLongGap, "long_gap", 1},
          Expect{Case::ReloadedA, "reloaded", 1}, Expect{Case::ReuseK8, "k8", 1},
          Expect{Case::PairThenSolo, "pair_solo", 2}, Expect{Case::SoloThenPair, "solo_pair", 1},
          Expect{Case::SoloThenSolo, "solo_solo", 2}}) {
        CAPTURE(name);
        const auto old = Module(true, scenario);
        const auto candidate = Module(true, scenario, true);
        const auto loads = spv::Op::OpCooperativeMatrixLoadKHR;
        REQUIRE(CountOpcode(old, loads) == CountOpcode(candidate, loads) + saved);
        REQUIRE(CountMultiplyAdds(old) == CountMultiplyAdds(candidate));
        REQUIRE(CountOpcode(old, spv::Op::OpControlBarrier) ==
                CountOpcode(candidate, spv::Op::OpControlBarrier));
        if (const char* output = std::getenv("STITCH_TENSOR_TEST_OUTPUT")) {
            for (const bool fast : {false, true}) {
                const auto& code = fast ? candidate : old;
                const auto path = std::filesystem::path{output} /
                                  fmt::format("reuse-{}-{}.spv", name, fast ? "new" : "old");
                std::ofstream out(path, std::ios::binary);
                out.write(reinterpret_cast<const char*>(code.data()),
                          static_cast<std::streamsize>(code.size() * sizeof(u32)));
                REQUIRE(out.good());
            }
        }
    }
}

TEST_CASE("Tensor operand baseline and queued choices fall back to the nearest built pipeline",
          "[tensor_reuse]") {
    using Vulkan::TensorPipelineMode;
    using V = Vulkan::TensorVariant;
    const auto file = std::filesystem::temp_directory_path() /
                      fmt::format("stitch-reuse-marker-{}",
                                  std::chrono::steady_clock::now().time_since_epoch().count());
    SCOPE_EXIT {
        std::error_code error;
        std::filesystem::remove(file, error);
    };
    {
        std::ofstream marker{file};
        REQUIRE(marker.good());
    }
    TensorPipelineMode mode;
    mode.Initialize(true, "1", file.string().c_str(), "1");
    const auto before = mode.Requested();
    REQUIRE(before == V::Paired);
    std::filesystem::remove(file);
    mode.TickFrame(15);
    REQUIRE(mode.Requested() == V::Paired);
    mode.TickFrame(16);
    const auto after = mode.Requested();
    REQUIRE(after == V::Reuse);
    REQUIRE(TensorPipelineMode::Available(before, true, true) == V::Paired);
    REQUIRE(TensorPipelineMode::Available(after, true, true) == V::Reuse);
    REQUIRE(TensorPipelineMode::Available(after, true, false) == V::Paired);
    REQUIRE(TensorPipelineMode::Available(after, false, false) == V::Original);
    REQUIRE(TensorPipelineMode::Available(before, false, true) == V::Original);
    REQUIRE(TensorPipelineMode::Available(after, false, true) == V::Reuse);
    REQUIRE(TensorPipelineMode::Available(V::Original, true, true) == V::Original);
}
