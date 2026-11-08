// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <limits>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

constexpr u32 NUM_TEXTURE_SCALING_WORDS = 4;
constexpr u32 NUM_IMAGE_SCALING_WORDS = 2;
constexpr u32 NUM_TEXTURE_AND_IMAGE_SCALING_WORDS =
    NUM_TEXTURE_SCALING_WORDS + NUM_IMAGE_SCALING_WORDS;

struct RescalingLayout {
    alignas(16) std::array<u32, NUM_TEXTURE_SCALING_WORDS> rescaling_textures;
    alignas(16) std::array<u32, NUM_IMAGE_SCALING_WORDS> rescaling_images;
    u32 down_factor;
};
struct RenderAreaLayout {
    std::array<f32, 4> render_area;
};
constexpr u32 RESCALING_LAYOUT_WORDS_OFFSET = offsetof(RescalingLayout, rescaling_textures);
constexpr u32 RESCALING_LAYOUT_DOWN_FACTOR_OFFSET = offsetof(RescalingLayout, down_factor);
constexpr u32 RENDERAREA_LAYOUT_OFFSET = offsetof(RenderAreaLayout, render_area);

[[nodiscard]] std::vector<u32> EmitSPIRV(const Profile& profile, const RuntimeInfo& runtime_info, IR::Program& program, Bindings& bindings);
[[nodiscard]] inline std::vector<u32> EmitSPIRV(const Profile& profile, IR::Program& program) {
    Bindings binding;
    return EmitSPIRV(profile, {}, program, binding);
}

/// Words per lane in the buffer of EmitCooperativeHmmaTest: the guest registers a0 a1 a2 a3 b0 b1
/// c0 c1 as input, then d0 d1 as output.
constexpr u32 COOPERATIVE_HMMA_TEST_LANE_WORDS = 10;

/// A compute shader of one warp running the HMMA.16816.F16 lowering on the registers in its storage
/// buffer (set 0, binding 0), to check at startup that the driver's matrix layout is the expected.
[[nodiscard]] std::vector<u32> EmitCooperativeHmmaTest(u32 spirv_version);

/// Mesh emulation without VK_EXT_mesh_shader. The mesh program runs as a compute shader
/// (RuntimeInfo::mesh_as_compute) that writes one block per workgroup to a u32 storage buffer:
/// max_vertices * vertex_lines words (vertex v, line l at v * vertex_lines + l), then
/// max_primitives * 3 indices (degenerate triangles past the primitive count), then one
/// primitive_lines-word record per primitive. All three corners read the same primitive record.
[[nodiscard]] inline u32 MeshEmulationBlockWords(const MeshInfo& mesh) {
    return mesh.max_vertices * static_cast<u32>(mesh.vertex_lines.size()) +
           mesh.max_primitives * (3 + static_cast<u32>(mesh.primitive_lines.size()));
}

/// Scalar workgroup allocations made by DefineMesh when the program runs as compute.
[[nodiscard]] inline u64 MeshEmulationSharedBytes(const MeshInfo& mesh, Stage stage) {
    u64 words{(std::max)(mesh.payload_words, 1U)};
    if (stage == Stage::Mesh) {
        const auto bank_words = [](u32 elements, size_t lines) {
            return ((u64{elements} + 31) / 32) * 32 * lines;
        };
        const u64 attributes{bank_words(mesh.max_vertices, mesh.vertex_lines.size()) +
                             bank_words(mesh.max_primitives, mesh.primitive_lines.size())};
        // One u32 per map byte, then the separate primitive-count word.
        const u64 map_words{4 + u64{mesh.PrimitiveVertices()} * mesh.max_primitives};
        words += (std::max)(attributes, u64{1}) + map_words + 1;
    }
    return words * sizeof(u32);
}

/// Can the pulling vertex shader represent these per-primitive attributes at ordinary locations?
[[nodiscard]] bool CanEmulatePrimitiveOutputs(const MeshInfo& mesh);

/// Vertex shader drawing the emulation's output as a triangle list of
/// groups * max_primitives * 3 vertices, with the mesh program's vertex outputs. With a task
/// program (args_binding != ~0) the blocks start at the arguments' MESH_EMULATION_ARGS_BASE.
[[nodiscard]] std::vector<u32> EmitMeshEmulationVertex(const Profile& profile,
                                                       const MeshInfo& mesh, u32 binding,
                                                       u32 args_binding, bool convert_depth);

constexpr u32 MESH_EMULATION_TASK_SEARCH_STEPS = 17;

/// Bound the direct dispatch and its per-group staging buffer before allocating either.
[[nodiscard]] constexpr u32 MaxMeshEmulationInputGroups(bool has_task, u64 bytes_per_group,
                                                        u32 max_dispatch_x, u64 max_storage_range) {
    // StagingBufferPool sizes its backing allocation from a u32 byte count.
    const u64 max_range{(std::min)(max_storage_range, u64{(std::numeric_limits<u32>::max)()})};
    const u32 lookup_limit{has_task ? 1U << MESH_EMULATION_TASK_SEARCH_STEPS : max_dispatch_x};
    return bytes_per_group == 0 ? 0
                                : static_cast<u32>(std::min<u64>(
                                      {max_dispatch_x, lookup_limit, max_range / bytes_per_group}));
}

struct MeshEmulationArenaUsage {
    u32 requested_words;
    u32 largest_rejected_dispatch;
};

/// Mesh emulation with a task program: push constants of the distribution pass.
struct MeshEmulationDistributionConstants {
    u32 tasks;              ///< task workgroups of the draw
    u32 payload_words;      ///< words per task in the payload buffer (count at word 1)
    u32 capacity_words;     ///< words of the output arena
    u32 block_words;        ///< output words per mesh workgroup
    u32 vertices_per_group; ///< draw vertices per mesh workgroup
    u32 max_dispatch_x;     ///< device limit for the generated vkCmdDispatchIndirect
};
/// Words of the distribution's argument buffer: vkCmdDispatchIndirect at 0, the mesh workgroups
/// the tasks asked for at 3, vkCmdDrawIndirect at MESH_EMULATION_DRAW_ARGS_OFFSET, then the
/// draw's first output word in the arena and its task count.
constexpr u32 MESH_EMULATION_ARGS_REQUESTED = 3;
constexpr u32 MESH_EMULATION_DRAW_ARGS_OFFSET = 16;
constexpr u32 MESH_EMULATION_ARGS_BASE = 8;
constexpr u32 MESH_EMULATION_ARGS_TASKS = 9;
constexpr u32 MESH_EMULATION_ARGS_WORDS = 10;

/// Distribution pass (one invocation) between the task and mesh dispatches, set 0: payload
/// buffer (binding 0), offsets (1: first mesh workgroup of each task), arguments (2) and the
/// arena counter (3: output words allocated so far, kept counting past the arena's end). The
/// draw's output is allocated from the arena every draw of the submission shares; mesh
/// workgroups that do not fit are not dispatched.
[[nodiscard]] std::vector<u32> EmitMeshEmulationDistribution(const Profile& profile);

} // namespace Shader::Backend::SPIRV
