// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <optional>
#include <sirit/sirit.h>

#include "shader_recompiler/frontend/ir/value.h"

namespace Shader::Backend::SPIRV {

// Only the immediately preceding K16 A and B in a straight-line packing sequence.
// Every key element is a per-lane SSA word, in the original guest register order.
struct TensorOperandCache {
    using Words = std::array<IR::Value, 4>;
    struct Operand {
        std::optional<Words> words;
        Sirit::Id matrix{};
    };
    Operand a;
    Operand b;
    bool paired_b{};
};

} // namespace Shader::Backend::SPIRV
