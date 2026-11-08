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

enum class Case { Independent, DifferentA, EarlierWord, EarlierVector, Barrier, Predicated, K8 };

// One subgroup, one storage buffer. Each lane owns A[4], B[2], C[2], B'[2], C'[2], D[2], D'[2].
std::vector<u32> Module(bool pair, Case scenario) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool}, conditional{pool}, exit{pool};
    IR::IREmitter ir{block};
    const IR::U32 offset_base = ir.IMul(ir.LocalInvocationIdX(), ir.Imm32(64));
    std::array<IR::Value, 12> words;
    std::array<IR::U32, 16> offsets;
    for (size_t i = 0; i < offsets.size(); ++i) {
        offsets[i] = IR::U32{ir.IAdd(offset_base, ir.Imm32(static_cast<u32>(4 * i)))};
        if (i < words.size()) {
            block.AppendNewInst(IR::Opcode::LoadStorage32, {ir.Imm32(0), offsets[i]});
            words[i] = IR::Value{&block.back()};
        }
    }
    const auto a = ir.CompositeConstruct(words[0], words[1], words[2], words[3]);
    const auto b = ir.CompositeConstruct(words[4], words[5]);
    const auto c = ir.CompositeConstruct(words[6], words[7]);
    const auto d = ir.TensorMulAdd(a, b, c, 16);
    const auto low0 = ir.CompositeExtract(d, 0);
    const auto low1 = ir.CompositeExtract(d, 1);
    if (scenario == Case::Barrier) {
        ir.Barrier();
    }
    IR::Block& next = scenario == Case::Predicated ? conditional : block;
    IR::IREmitter later{next};
    const auto a1 = later.CompositeConstruct(words[0], words[1], words[2],
                                             scenario == Case::DifferentA ? words[9] : words[3]);
    const auto b1 =
        later.CompositeConstruct(scenario == Case::EarlierWord ? low1 : words[8], words[9]);
    const auto c1 =
        scenario == Case::EarlierVector ? d : later.CompositeConstruct(words[10], words[11]);
    const auto d1 = later.TensorMulAdd(a1, b1, c1, scenario == Case::K8 ? 8 : 16);
    const std::array output{low0, low1, later.CompositeExtract(d1, 0),
                            later.CompositeExtract(d1, 1)};
    for (size_t i = 0; i < output.size(); ++i) {
        next.AppendNewInst(IR::Opcode::WriteStorage32, {ir.Imm32(0), offsets[12 + i], output[i]});
    }

    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {32, 1, 1};
    using Node = IR::AbstractSyntaxNode;
    program.blocks = {&block};
    program.syntax_list = {{{.block = &block}, Node::Type::Block}};
    if (scenario == Case::Predicated) {
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
    profile.max_compute_shared_memory_size = 16384;
    profile.max_subgroup_size = 32;
    profile.unified_descriptor_binding = true;
    return Backend::SPIRV::EmitSPIRV(profile, program);
}

size_t CountMultiplyAdds(const std::vector<u32>& module) {
    size_t results = 0;
    for (size_t i = 5; i < module.size();) {
        const size_t size = module[i] >> 16;
        REQUIRE(size > 0);
        REQUIRE(i + size <= module.size());
        results += (module[i] & 0xffff) == static_cast<u32>(spv::Op::OpCooperativeMatrixMulAddKHR);
        i += size;
    }
    return results;
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
