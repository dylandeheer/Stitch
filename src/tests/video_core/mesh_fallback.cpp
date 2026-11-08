// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace {
using namespace Shader;
namespace SPIRV = Shader::Backend::SPIRV;

constexpr u32 OUTPUT_BINDING = 4;
constexpr u32 PAYLOAD_BINDING = 5;
constexpr u32 OFFSETS_BINDING = 6;
constexpr u32 ARGS_BINDING = 7;

struct Instruction {
    spv::Op op;
    std::span<const u32> operands;
};

std::vector<Instruction> Instructions(const std::vector<u32>& code) {
    std::vector<Instruction> result;
    for (size_t offset = 5; offset < code.size();) {
        const u32 count{code[offset] >> 16};
        REQUIRE(count != 0);
        REQUIRE(offset + count <= code.size());
        result.push_back({static_cast<spv::Op>(code[offset] & 0xffff),
                          std::span<const u32>(code).subspan(offset + 1, count - 1)});
        offset += count;
    }
    return result;
}

spv::ExecutionModel Model(const std::vector<u32>& code) {
    for (const Instruction& inst : Instructions(code)) {
        if (inst.op == spv::Op::OpEntryPoint) {
            return static_cast<spv::ExecutionModel>(inst.operands[0]);
        }
    }
    FAIL("no entry point");
    return {};
}

bool HasCapability(const std::vector<u32>& code, spv::Capability capability) {
    return std::ranges::any_of(Instructions(code), [&](const Instruction& inst) {
        return inst.op == spv::Op::OpCapability &&
               inst.operands[0] == static_cast<u32>(capability);
    });
}

bool HasExtension(const std::vector<u32>& code, std::string_view name) {
    return std::ranges::any_of(Instructions(code), [&](const Instruction& inst) {
        return inst.op == spv::Op::OpExtension &&
               std::string_view{reinterpret_cast<const char*>(inst.operands.data())} == name;
    });
}

bool HasVariable(const std::vector<u32>& code, spv::StorageClass storage_class) {
    return std::ranges::any_of(Instructions(code), [&](const Instruction& inst) {
        return inst.op == spv::Op::OpVariable &&
               inst.operands[2] == static_cast<u32>(storage_class);
    });
}

bool UsesStorageClass(const std::vector<u32>& code, spv::StorageClass storage_class) {
    return std::ranges::any_of(Instructions(code), [&](const Instruction& inst) {
        return (inst.op == spv::Op::OpTypePointer &&
                inst.operands[1] == static_cast<u32>(storage_class)) ||
               (inst.op == spv::Op::OpVariable &&
                inst.operands[2] == static_cast<u32>(storage_class));
    });
}

std::vector<u32> Bindings(const std::vector<u32>& code) {
    std::vector<u32> bindings;
    for (const Instruction& inst : Instructions(code)) {
        if (inst.op == spv::Op::OpDecorate &&
            inst.operands[1] == static_cast<u32>(spv::Decoration::Binding)) {
            bindings.push_back(inst.operands[2]);
        }
    }
    std::ranges::sort(bindings);
    return bindings;
}

// EDEN_MESH_FALLBACK_TEST_OUTPUT=<dir>: write the modules for spirv-val.
void Dump(const std::vector<u32>& code, std::string_view name) {
    const char* const directory{std::getenv("EDEN_MESH_FALLBACK_TEST_OUTPUT")};
    if (!directory) {
        return;
    }
    std::filesystem::create_directories(directory);
    std::ofstream file{std::filesystem::path{directory} / (std::string{name} + ".spv"),
                       std::ios::binary};
    file.write(reinterpret_cast<const char*>(code.data()),
               static_cast<std::streamsize>(code.size() * sizeof(u32)));
    REQUIRE(file.good());
}

MeshInfo TriangleMesh() {
    MeshInfo mesh;
    mesh.payload_words = 32;
    mesh.max_vertices = 3;
    mesh.max_primitives = 1;
    mesh.topology = 4;
    for (const IR::Attribute attribute : {IR::Attribute::PositionX, IR::Attribute::PositionY,
                                          IR::Attribute::PositionZ, IR::Attribute::PositionW}) {
        mesh.vertex_lines.push_back(static_cast<u32>(attribute));
    }
    return mesh;
}

MeshInfo PrimitiveColorsMesh() {
    MeshInfo mesh{TriangleMesh()};
    mesh.max_vertices = 4;
    mesh.max_primitives = 33;
    mesh.primitive_lines = {static_cast<u32>(IR::Attribute::Generic2X),
                            static_cast<u32>(IR::Attribute::Generic2Y)};
    return mesh;
}

// Two triangles share vertices 0 and 2. Place the second triangle across a 32-primitive
// stage-buffer bank boundary and give it a different color.
void WritePrimitiveColors(IR::IREmitter& ir) {
    constexpr std::array<std::array<f32, 2>, 4> corners{{
        {-0.75f, -0.75f},
        {0.75f, -0.75f},
        {0.75f, 0.75f},
        {-0.75f, 0.75f},
    }};
    const auto attribute = [&](u32 word, f32 value) {
        ir.IsbeWriteAttribute(ir.Imm32(word * 4), ir.Imm32(std::bit_cast<u32>(value)));
    };
    for (u32 vertex = 0; vertex < corners.size(); ++vertex) {
        attribute(vertex, corners[vertex][0]);
        attribute(32 + vertex, corners[vertex][1]);
        attribute(64 + vertex, 0.5f);
        attribute(96 + vertex, 1.0f);
    }
    for (u32 primitive = 0; primitive < 33; ++primitive) {
        std::array<u32, 3> indices{};
        if (primitive == 0) {
            indices = {0, 1, 2};
        } else if (primitive == 32) {
            indices = {0, 2, 3};
        }
        for (u32 corner = 0; corner < 3; ++corner) {
            ir.IsbeWriteMapU8(ir.Imm32(4 + primitive * 3 + corner), ir.Imm32(indices[corner]));
        }
        const u32 word{128 + (primitive / 32) * 64 + primitive % 32};
        attribute(word, primitive == 0 ? 1.0f : 0.0f);
        attribute(word + 32, primitive == 32 ? 1.0f : 0.0f);
    }
    ir.IsbeWriteMapU32(ir.Imm32(3), ir.Imm32(33));
}

Profile MakeProfile() {
    Profile profile;
    profile.supported_spirv = 0x00010500;
    return profile;
}

// A one-triangle mesh program (or the task program launching it), as the frontend lowers them.
std::vector<u32> EmitProgram(Stage stage, bool fallback, bool with_task,
                             const MeshInfo& mesh = TriangleMesh()) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = stage;
    program.workgroup_size = {mesh.primitive_lines.empty() ? 32U : 1U, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    program.mesh = mesh;
    program.info.is_task = stage == Stage::Task;
    program.info.is_mesh = stage == Stage::Mesh;
    program.info.uses_workgroup_id = stage != Stage::Fragment;
    program.info.uses_local_invocation_id = stage != Stage::Fragment;
    IR::IREmitter ir{block};
    ir.Prologue();
    if (stage == Stage::Fragment) {
        program.info.loads.Set(IR::Attribute::Generic2X, true);
        program.info.loads.Set(IR::Attribute::Generic2Y, true);
        program.info.stores_frag_color[0] = true;
        program.info.frag_color_components[0] = 0xf;
        ir.SetFragColor(0, 0, ir.GetAttribute(IR::Attribute::Generic2X));
        ir.SetFragColor(0, 1, ir.GetAttribute(IR::Attribute::Generic2Y));
        ir.SetFragColor(0, 2, ir.Imm32(0.0f));
        ir.SetFragColor(0, 3, ir.Imm32(1.0f));
    } else if (stage == Stage::Mesh && !mesh.primitive_lines.empty()) {
        WritePrimitiveColors(ir);
    } else if (stage == Stage::Mesh) {
        const IR::U32 index{ir.IsbeRead(ir.Imm32(0))};
        ir.IsbeWriteAttribute(ir.Imm32(0), index);
        ir.IsbeWriteMapU8(ir.Imm32(4), ir.Imm32(0));
        ir.IsbeWriteMapU32(ir.Imm32(3), ir.Imm32(1));
    } else {
        ir.IsbeWriteAttribute(ir.Imm32(4), ir.Imm32(1)); // one mesh workgroup
    }
    ir.Epilogue();

    RuntimeInfo runtime_info;
    if (stage == Stage::Fragment) {
        runtime_info.previous_stage_stores = program.info.loads;
        runtime_info.per_primitive_generics.set(2);
        runtime_info.emulate_per_primitive_inputs = fallback;
    } else {
        runtime_info.mesh_as_compute = fallback;
    }
    runtime_info.mesh_output_binding = OUTPUT_BINDING;
    if (with_task) {
        runtime_info.mesh_task_payload_binding = PAYLOAD_BINDING;
        runtime_info.mesh_task_offsets_binding = OFFSETS_BINDING;
        runtime_info.mesh_task_args_binding = ARGS_BINDING;
        runtime_info.mesh_task_payload_stride = program.mesh.payload_words;
    }
    Backend::Bindings bindings;
    return SPIRV::EmitSPIRV(MakeProfile(), runtime_info, program, bindings);
}

bool LocationHasDecoration(const std::vector<u32>& code, u32 location, spv::Decoration decoration) {
    const auto instructions{Instructions(code)};
    for (const Instruction& inst : instructions) {
        if (inst.op != spv::Op::OpDecorate ||
            inst.operands[1] != static_cast<u32>(spv::Decoration::Location) ||
            inst.operands[2] != location) {
            continue;
        }
        return std::ranges::any_of(instructions, [&](const Instruction& other) {
            return other.op == spv::Op::OpDecorate && other.operands[0] == inst.operands[0] &&
                   other.operands[1] == static_cast<u32>(decoration);
        });
    }
    return false;
}

void RequireNoMeshShading(const std::vector<u32>& code) {
    REQUIRE(Model(code) == spv::ExecutionModel::GLCompute);
    REQUIRE_FALSE(HasCapability(code, spv::Capability::MeshShadingEXT));
    REQUIRE_FALSE(HasExtension(code, "SPV_EXT_mesh_shader"));
    REQUIRE_FALSE(UsesStorageClass(code, spv::StorageClass::TaskPayloadWorkgroupEXT));
    REQUIRE_FALSE(HasVariable(code, spv::StorageClass::Output));
}
} // Anonymous namespace

TEST_CASE("Mesh fallback leaves native task and mesh programs as mesh shaders",
          "[video_core][spirv][mesh_fallback]") {
    const std::vector<u32> mesh{EmitProgram(Stage::Mesh, false, false)};
    Dump(mesh, "native-mesh");
    REQUIRE(Model(mesh) == spv::ExecutionModel::MeshEXT);
    REQUIRE(HasCapability(mesh, spv::Capability::MeshShadingEXT));
    REQUIRE(Bindings(mesh).empty());

    const std::vector<u32> task{EmitProgram(Stage::Task, false, false)};
    Dump(task, "native-task");
    REQUIRE(Model(task) == spv::ExecutionModel::TaskEXT);
    REQUIRE(UsesStorageClass(task, spv::StorageClass::TaskPayloadWorkgroupEXT));
}

TEST_CASE("Mesh fallback emits mesh programs as compute shaders writing storage buffers",
          "[video_core][spirv][mesh_fallback]") {
    const std::vector<u32> alone{EmitProgram(Stage::Mesh, true, false)};
    Dump(alone, "fallback-mesh");
    RequireNoMeshShading(alone);
    REQUIRE(Bindings(alone) == std::vector<u32>{OUTPUT_BINDING});

    const std::vector<u32> after_task{EmitProgram(Stage::Mesh, true, true)};
    Dump(after_task, "fallback-mesh-after-task");
    RequireNoMeshShading(after_task);
    REQUIRE(Bindings(after_task) ==
            std::vector<u32>{OUTPUT_BINDING, PAYLOAD_BINDING, OFFSETS_BINDING, ARGS_BINDING});
}

TEST_CASE("Mesh fallback emits task programs as compute shaders writing their payload",
          "[video_core][spirv][mesh_fallback]") {
    const std::vector<u32> task{EmitProgram(Stage::Task, true, true)};
    Dump(task, "fallback-task");
    RequireNoMeshShading(task);
    REQUIRE(Bindings(task) == std::vector<u32>{PAYLOAD_BINDING});
}

TEST_CASE("Mesh fallback vertex shader pulls the compute output",
          "[video_core][spirv][mesh_fallback]") {
    const MeshInfo mesh{TriangleMesh()};
    const std::vector<u32> alone{
        SPIRV::EmitMeshEmulationVertex(MakeProfile(), mesh, OUTPUT_BINDING, ~0U, false)};
    Dump(alone, "fallback-vertex");
    REQUIRE(Model(alone) == spv::ExecutionModel::Vertex);
    REQUIRE(Bindings(alone) == std::vector<u32>{OUTPUT_BINDING});

    const std::vector<u32> after_task{
        SPIRV::EmitMeshEmulationVertex(MakeProfile(), mesh, OUTPUT_BINDING, ARGS_BINDING, true)};
    Dump(after_task, "fallback-vertex-after-task");
    REQUIRE(Model(after_task) == spv::ExecutionModel::Vertex);
    REQUIRE(Bindings(after_task) == std::vector<u32>{OUTPUT_BINDING, ARGS_BINDING});
}

TEST_CASE("Mesh fallback distribution pass binds its buffers and constants",
          "[video_core][spirv][mesh_fallback]") {
    const std::vector<u32> code{SPIRV::EmitMeshEmulationDistribution(MakeProfile())};
    Dump(code, "fallback-distribution");
    REQUIRE(Model(code) == spv::ExecutionModel::GLCompute);
    REQUIRE(Bindings(code) == std::vector<u32>{0, 1, 2, 3});
    REQUIRE(UsesStorageClass(code, spv::StorageClass::PushConstant));
    const auto instructions{Instructions(code)};
    const bool has_constants{std::ranges::any_of(instructions, [](const Instruction& inst) {
        return inst.op == spv::Op::OpTypeStruct &&
               inst.operands.size() == 1 + sizeof(SPIRV::MeshEmulationDistributionConstants) / 4;
    })};
    REQUIRE(has_constants);
}

TEST_CASE("Mesh fallback preserves generic per-primitive triangle colors",
          "[video_core][spirv][mesh_fallback]") {
    const MeshInfo mesh{PrimitiveColorsMesh()};
    REQUIRE(SPIRV::CanEmulatePrimitiveOutputs(mesh));
    REQUIRE(SPIRV::MeshEmulationBlockWords(mesh) == 4 * 4 + 33 * 3 + 33 * 2);

    const auto compute{EmitProgram(Stage::Mesh, true, false, mesh)};
    const auto vertex{
        SPIRV::EmitMeshEmulationVertex(MakeProfile(), mesh, OUTPUT_BINDING, ~0U, false)};
    const auto fragment{EmitProgram(Stage::Fragment, true, false, mesh)};
    Dump(compute, "primitive-colors-compute");
    Dump(vertex, "primitive-colors-vertex");
    Dump(fragment, "primitive-colors-fragment");
    RequireNoMeshShading(compute);
    for (const auto* code : {&vertex, &fragment}) {
        REQUIRE(LocationHasDecoration(*code, 2, spv::Decoration::Flat));
        REQUIRE_FALSE(LocationHasDecoration(*code, 2, spv::Decoration::PerPrimitiveEXT));
        REQUIRE_FALSE(HasCapability(*code, spv::Capability::MeshShadingEXT));
        REQUIRE_FALSE(HasExtension(*code, "SPV_EXT_mesh_shader"));
    }
    const auto native_fragment{EmitProgram(Stage::Fragment, false, false, mesh)};
    REQUIRE(LocationHasDecoration(native_fragment, 2, spv::Decoration::PerPrimitiveEXT));
    REQUIRE(HasCapability(native_fragment, spv::Capability::MeshShadingEXT));
}

TEST_CASE("Mesh fallback excludes unsupported primitive built-ins and overlapping locations",
          "[video_core][spirv][mesh_fallback]") {
    MeshInfo mesh{PrimitiveColorsMesh()};
    for (const auto builtin :
         {IR::Attribute::Layer, IR::Attribute::ViewportIndex, IR::Attribute::PrimitiveId}) {
        mesh.primitive_lines = {static_cast<u32>(builtin)};
        REQUIRE_FALSE(SPIRV::CanEmulatePrimitiveOutputs(mesh));
    }
    mesh.primitive_lines = {static_cast<u32>(IR::Attribute::Generic2W)};
    mesh.vertex_lines.push_back(static_cast<u32>(IR::Attribute::Generic2Z));
    REQUIRE_FALSE(SPIRV::CanEmulatePrimitiveOutputs(mesh));
    mesh.primitive_lines = {static_cast<u32>(IR::Attribute::Generic3W)};
    REQUIRE(SPIRV::CanEmulatePrimitiveOutputs(mesh));
}

TEST_CASE("Mesh fallback bounds direct dispatches and storage ranges before allocation",
          "[video_core][spirv][mesh_fallback]") {
    constexpr u32 largest{(std::numeric_limits<u32>::max)()};
    constexpr u64 four_gib{u64{1} << 32};
    REQUIRE(SPIRV::MaxMeshEmulationInputGroups(false, 4, 65535, four_gib) == 65535);
    REQUIRE(SPIRV::MaxMeshEmulationInputGroups(false, 128, largest, 1023) == 7);
    REQUIRE(SPIRV::MaxMeshEmulationInputGroups(false, 128, largest, 1024) == 8);
    REQUIRE(SPIRV::MaxMeshEmulationInputGroups(false, four_gib, largest, four_gib) == 0);
    REQUIRE(SPIRV::MaxMeshEmulationInputGroups(false, 4, largest, four_gib) == largest / 4);
    REQUIRE(SPIRV::MaxMeshEmulationInputGroups(true, 128, largest, four_gib) == 131072);
}

TEST_CASE("Mesh fallback checks implicit shared arrays for each compute pass",
          "[video_core][spirv][mesh_fallback]") {
    const MeshInfo mesh{PrimitiveColorsMesh()};
    // Task has its 32-word payload. Mesh adds 128 position words, 2 banks of 64 primitive
    // attribute words, 103 index-map bytes stored individually as words, and one count.
    REQUIRE(SPIRV::MeshEmulationSharedBytes(mesh, Stage::Task) == 32 * sizeof(u32));
    REQUIRE(SPIRV::MeshEmulationSharedBytes(mesh, Stage::Mesh) == 392 * sizeof(u32));
}
