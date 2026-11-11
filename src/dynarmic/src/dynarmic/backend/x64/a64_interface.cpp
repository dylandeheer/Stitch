// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstring>
#include <memory>
#include <mutex>

#include <boost/icl/interval_set.hpp>
#include "common/assert.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/llvm_disassemble.h"
#include <bit>

#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/jitstate_info.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opt_passes.h"

namespace Dynarmic::A64 {

using namespace Backend::X64;

static RunCodeCallbacks GenRunCodeCallbacks(A64::UserCallbacks* cb, CodePtr (*LookupBlock)(void* lookup_block_arg), void* arg, const A64::UserConfig& conf) {
    return RunCodeCallbacks{
        ArgCallback(LookupBlock, reinterpret_cast<u64>(arg)),
        ArgCallback(Devirtualize<&A64::UserCallbacks::AddTicks>(cb)),
        ArgCallback(Devirtualize<&A64::UserCallbacks::GetTicksRemaining>(cb)),
        conf.enable_cycle_counting,
    };
}

static std::function<void(BlockOfCode&)> GenRCP(const A64::UserConfig& conf) {
    return [conf](BlockOfCode& code) {
        if (conf.page_table) {
            code.mov(code.r14, std::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.mov(code.r13, *conf.fastmem_pointer);
        }
    };
}

static Optimization::PolyfillOptions GenPolyfillOptions(const BlockOfCode& code) {
    return Optimization::PolyfillOptions{
        .sha256 = !code.HasHostFeature(HostFeature::SHA),
        .vector_multiply_widen = true,
    };
}

// The sign-extended imm19 counts instructions relative to the branch's own PC.
static s64 CompareLinkDisplacement(u32 word) {
    return static_cast<s32>(word << 8) >> 13;
}

// The first instruction has already been translated normally. The following CMP Wn,Wm
// remains live across two conditional selects and CBZ Wn. The second CMP is identical,
// and both branches target the same PC. No guest data-memory instruction is fused.
static bool IsCompareLinkWindow(const Jit::CompareWindow& w) {
    const u32 cmp = w[1];
    if ((cmp & 0xffe0fc1f) != 0x6b00001f || w[5] != cmp || (w[4] & 0xff000000) != 0x34000000 || (w[6] & 0xff00001f) != 0x54000001)
        return false;
    const u32 lhs = (cmp >> 5) & 31, rhs = (cmp >> 16) & 31;
    for (const u32 select : {w[2], w[3]}) {
        // 32-bit CSEL/CSINC/CSINV/CSNEG, which preserve NZCV.
        if ((select & 0xbfe00800) != 0x1a800000 || (select & 31) == lhs || (select & 31) == rhs)
            return false;
    }
    return lhs != 31 && rhs != 31 && (w[4] & 31) == lhs && CompareLinkDisplacement(w[4]) == CompareLinkDisplacement(w[6]) + 2;
}

struct Jit::Impl final {
public:
    Impl(Jit* jit, UserConfig conf)
        : conf(conf)
        , block_of_code(GenRunCodeCallbacks(conf.callbacks, &GetCurrentBlockThunk, this, conf), JitStateInfo{jit_state}, conf.code_cache_size, GenRCP(conf))
        , emitter(block_of_code, conf, jit)
        , polyfill_options(GenPolyfillOptions(block_of_code))
    {
        ASSERT(conf.page_table_address_space_bits >= 12 && conf.page_table_address_space_bits <= 64);
    }

    ~Impl() = default;

    HaltReason Run() {
        ASSERT(!is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        is_executing = true;
        // TODO: Check code alignment
        const CodePtr current_code_ptr = [this] {
            // RSB optimization
            const u32 new_rsb_ptr = (jit_state.rsb_ptr - 1) & A64JitState::RSB_PTR_MASK;
            if (jit_state.GetUniqueHash() == jit_state.rsb_location_descriptors[new_rsb_ptr]) {
                jit_state.rsb_ptr = new_rsb_ptr;
                return CodePtr(jit_state.rsb_codeptrs[new_rsb_ptr]);
            }
            return CodePtr((uintptr_t(GetCurrentBlock()) + 15) & ~uintptr_t(15));
        }();
        const HaltReason hr = block_of_code.RunCode(&jit_state, current_code_ptr);
        PerformRequestedCacheInvalidation(hr);
        is_executing = false;
        return hr;
    }

    HaltReason Step() {
        ASSERT(!is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        is_executing = true;
        const HaltReason hr = block_of_code.StepCode(&jit_state, GetCurrentSingleStep());
        PerformRequestedCacheInvalidation(hr);
        is_executing = false;
        return hr;
    }

    void ClearCache() {
        std::unique_lock lock{invalidation_mutex};
        invalidate_entire_cache = true;
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void InvalidateCacheRange(u64 start_address, size_t length) {
        std::unique_lock lock{invalidation_mutex};
        const auto end_address = static_cast<u64>(start_address + length - 1);
        const auto range = boost::icl::discrete_interval<u64>::closed(start_address, end_address);
        invalid_cache_ranges.add(range);
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void Reset() {
        ASSERT(!is_executing);
        jit_state = {};
    }

    void HaltExecution(HaltReason hr) {
        Atomic::Or(&jit_state.halt_reason, static_cast<u32>(hr));
    }

    void ClearHalt(HaltReason hr) {
        Atomic::And(&jit_state.halt_reason, ~static_cast<u32>(hr));
    }

    u64 GetSP() const {
        return jit_state.sp;
    }

    void SetSP(u64 value) {
        jit_state.sp = value;
    }

    u64 GetPC() const {
        return jit_state.pc;
    }

    void SetPC(u64 value) {
        jit_state.pc = value;
    }

    u64 GetRegister(size_t index) const {
        if (index == 31)
            return GetSP();
        return jit_state.reg.at(index);
    }

    void SetRegister(size_t index, u64 value) {
        if (index == 31)
            return SetSP(value);
        jit_state.reg.at(index) = value;
    }

    std::array<u64, 31> GetRegisters() const {
        return jit_state.reg;
    }

    void SetRegisters(const std::array<u64, 31>& value) {
        jit_state.reg = value;
    }

    Vector GetVector(size_t index) const {
        return {jit_state.vec.at(index * 2), jit_state.vec.at(index * 2 + 1)};
    }

    void SetVector(size_t index, Vector value) {
        jit_state.vec.at(index * 2) = value[0];
        jit_state.vec.at(index * 2 + 1) = value[1];
    }

    std::array<Vector, 32> GetVectors() const {
        std::array<Vector, 32> ret;
        static_assert(sizeof(ret) == sizeof(jit_state.vec));
        std::memcpy(ret.data(), jit_state.vec.data(), sizeof(jit_state.vec));
        return ret;
    }

    void SetVectors(const std::array<Vector, 32>& value) {
        static_assert(sizeof(value) == sizeof(jit_state.vec));
        std::memcpy(jit_state.vec.data(), value.data(), sizeof(jit_state.vec));
    }

    u32 GetFpcr() const {
        return jit_state.GetFpcr();
    }

    void SetFpcr(u32 value) {
        jit_state.SetFpcr(value);
    }

    u32 GetFpsr() const {
        return jit_state.GetFpsr();
    }

    void SetFpsr(u32 value) {
        jit_state.SetFpsr(value);
    }

    u32 GetPstate() const {
        return jit_state.GetPstate();
    }

    void SetPstate(u32 value) {
        jit_state.SetPstate(value);
    }

    void ClearExclusiveState() {
        jit_state.exclusive_state = 0;
    }

    bool IsExecuting() const {
        return is_executing;
    }

    std::string Disassemble() const {
        const size_t size = reinterpret_cast<const char*>(block_of_code.getCurr()) - reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        auto const* p = reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        return Common::DisassembleX64(p, p + size);
    }

    PrecompileResult Precompile(std::uint64_t location, std::size_t keep_free) {
        ASSERT(!is_executing);
        const IR::LocationDescriptor descriptor{location};
        if (A64::LocationDescriptor{descriptor}.SingleStepping()) {
            return PrecompileResult::Unreadable;
        }
        if (emitter.GetBasicBlock(descriptor)) {
            return PrecompileResult::Present;
        }
        if (block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE + keep_free) {
            return PrecompileResult::Full;
        }
        block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);
        if (!CompileBlock<true>(descriptor)) {
            return PrecompileResult::Unreadable;
        }
        ASSERT(block_of_code.SpaceRemaining() >= keep_free);
        return PrecompileResult::Compiled;
    }

    size_t CodeCacheUsed() const {
        return size_t(reinterpret_cast<const char*>(block_of_code.getCurr()) -
                      reinterpret_cast<const char*>(block_of_code.GetCodeBegin()));
    }

    CompareFilter compare_filter;
    std::function<void(size_t)> compare_observer;
    size_t compare_links{};

private:
    static CodePtr GetCurrentBlockThunk(void* thisptr) {
        Jit::Impl* this_ = static_cast<Jit::Impl*>(thisptr);
        return this_->GetCurrentBlock();
    }

    IR::LocationDescriptor GetCurrentLocation() const {
        return IR::LocationDescriptor{jit_state.GetUniqueHash()};
    }

    CodePtr GetCurrentBlock() {
        return GetBlock(GetCurrentLocation());
    }

    CodePtr GetCurrentSingleStep() {
        return GetBlock(A64::LocationDescriptor{GetCurrentLocation()}.SetSingleStepping(true));
    }

    static constexpr size_t MINIMUM_REMAINING_CODESIZE = 1 * 1024 * 1024;

    CodePtr GetBlock(IR::LocationDescriptor descriptor) {
        if (auto block = emitter.GetBasicBlock(descriptor))
            return block->entrypoint;

        if (block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {
            // Immediately evacuate cache
            invalidate_entire_cache = true;
            PerformRequestedCacheInvalidation(HaltReason::CacheInvalidation);
        }
        block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);

        const CodePtr entrypoint = CompileBlock<false>(descriptor);
        conf.callbacks->BlockCompiled(descriptor.Value());
        return entrypoint;
    }

    template <bool speculative>
    CodePtr CompileBlock(IR::LocationDescriptor descriptor) {
        bool readable = true;
        size_t fetches{};
        const auto get_code = [&](u64 vaddr) -> std::optional<u32> {
            if constexpr (speculative) {
                // Bound even a straight-line block with no guest terminator. The failed read
                // ends translation; the speculative block is discarded before optimization.
                if (fetches == 256) {
                    readable = false;
                    return std::nullopt;
                }
                ++fetches;
            }
            const std::optional<u32> instruction = conf.callbacks->MemoryReadCode(vaddr);
            readable = readable && instruction.has_value();
            return instruction;
        };
        // LocationDescriptor ctor() does important ops (like tflags) do not skip
        auto const arch_descriptor = A64::LocationDescriptor{descriptor};
        ir_block.Reset(arch_descriptor);
        const TranslationOptions options{conf.define_unpredictable_behaviour, conf.wall_clock_cntpct};
        const auto start = arch_descriptor.PC();
        const auto expected = compare_filter && !arch_descriptor.SingleStepping() && !conf.enable_cycle_counting && conf.HasOptimization(OptimizationFlag::BlockLinking)
                                ? compare_filter(start)
                                : std::nullopt;
        bool fold = false;
        if (expected) {
            size_t seen = 0;
            bool matches = true;
            const auto observed = [&](u64 pc) {
                const auto word = get_code(pc);
                matches &= seen < 5 && pc == start + seen * 4 && word == (*expected)[seen];
                ++seen;
                return word;
            };
            A64::Translate(ir_block, arch_descriptor, observed, options);
            fold = matches && seen == 5 && get_code(start + 20) == (*expected)[5] && get_code(start + 24) == (*expected)[6] && IsCompareLinkWindow(*expected);
        } else {
            A64::Translate(ir_block, arch_descriptor, get_code, options);
        }
        if (speculative && !readable) {
            return nullptr;
        }
        Optimization::Optimize(ir_block, conf, polyfill_options);
        if (fold) {
            const auto* branch = std::get_if<IR::Term::CheckBit>(&ir_block.terminal);
            const auto* zero = branch ? std::get_if<IR::Term::LinkBlock>(&branch->then_) : nullptr;
            const auto* nonzero = branch ? std::get_if<IR::Term::LinkBlock>(&branch->else_) : nullptr;
            const u64 target = start + 16 + CompareLinkDisplacement((*expected)[4]) * 4;
            fold = zero && nonzero && zero->next == arch_descriptor.SetPC(target) && nonzero->next == arch_descriptor.SetPC(start + 20);
        }
        if (fold) {
            // A write to either inlined instruction invalidates this block as well.
            ir_block.SetEndLocation(arch_descriptor.AdvancePC(28));
            ++compare_links;
            if (compare_observer)
                compare_observer(compare_links);
            return emitter.Emit(ir_block, true).entrypoint;
        }
        return emitter.Emit(ir_block).entrypoint;
    }

    void PerformRequestedCacheInvalidation(HaltReason hr) {
        if (Has(hr, HaltReason::CacheInvalidation)) {
            std::unique_lock lock{invalidation_mutex};

            ClearHalt(HaltReason::CacheInvalidation);

            if (!invalidate_entire_cache && invalid_cache_ranges.empty()) {
                return;
            }

            jit_state.ResetRSB();
            if (invalidate_entire_cache) {
                block_of_code.ClearCache();
                emitter.ClearCache();
            } else {
                emitter.InvalidateCacheRanges(invalid_cache_ranges);
            }
            invalid_cache_ranges.clear();
            invalidate_entire_cache = false;
        }
    }

    IR::Block ir_block = {LocationDescriptor(0, FP::FPCR(0), false)};
    const UserConfig conf;
    A64JitState jit_state;
    BlockOfCode block_of_code;
    A64EmitX64 emitter;
    Optimization::PolyfillOptions polyfill_options;
    bool is_executing = false;
    bool invalidate_entire_cache = false;
    boost::icl::interval_set<u64> invalid_cache_ranges;
    std::mutex invalidation_mutex;
};

Jit::Jit(UserConfig conf)
        : impl(std::make_unique<Jit::Impl>(this, conf)) {}

Jit::~Jit() = default;

HaltReason Jit::Run() {
    return impl->Run();
}

HaltReason Jit::Step() {
    return impl->Step();
}

void Jit::ClearCache() {
    impl->ClearCache();
}

void Jit::InvalidateCacheRange(u64 start_address, size_t length) {
    impl->InvalidateCacheRange(start_address, length);
}

PrecompileResult Jit::Precompile(std::uint64_t location, std::size_t keep_free) {
    return impl->Precompile(location, keep_free);
}

size_t Jit::CodeCacheUsed() const {
    return impl->CodeCacheUsed();
}

void Jit::Reset() {
    impl->Reset();
}

void Jit::HaltExecution(HaltReason hr) {
    impl->HaltExecution(hr);
}

void Jit::ClearHalt(HaltReason hr) {
    impl->ClearHalt(hr);
}

u64 Jit::GetSP() const {
    return impl->GetSP();
}

void Jit::SetSP(u64 value) {
    impl->SetSP(value);
}

u64 Jit::GetPC() const {
    return impl->GetPC();
}

void Jit::SetPC(u64 value) {
    impl->SetPC(value);
}

u64 Jit::GetRegister(size_t index) const {
    return impl->GetRegister(index);
}

void Jit::SetRegister(size_t index, u64 value) {
    impl->SetRegister(index, value);
}

std::array<u64, 31> Jit::GetRegisters() const {
    return impl->GetRegisters();
}

void Jit::SetRegisters(const std::array<u64, 31>& value) {
    impl->SetRegisters(value);
}

Vector Jit::GetVector(size_t index) const {
    return impl->GetVector(index);
}

void Jit::SetVector(size_t index, Vector value) {
    impl->SetVector(index, value);
}

std::array<Vector, 32> Jit::GetVectors() const {
    return impl->GetVectors();
}

void Jit::SetVectors(const std::array<Vector, 32>& value) {
    impl->SetVectors(value);
}

u32 Jit::GetFpcr() const {
    return impl->GetFpcr();
}

void Jit::SetFpcr(u32 value) {
    impl->SetFpcr(value);
}

u32 Jit::GetFpsr() const {
    return impl->GetFpsr();
}

void Jit::SetFpsr(u32 value) {
    impl->SetFpsr(value);
}

u32 Jit::GetPstate() const {
    return impl->GetPstate();
}

void Jit::SetPstate(u32 value) {
    impl->SetPstate(value);
}

void Jit::SetCompareLinkFilter(CompareFilter filter, std::function<void(size_t)> observer) {
    ClearCache();
    impl->compare_filter = std::move(filter);
    impl->compare_observer = std::move(observer);
}

size_t Jit::CompareLinksCompiled() const {
    return impl->compare_links;
}

void Jit::ClearExclusiveState() {
    impl->ClearExclusiveState();
}

bool Jit::IsExecuting() const {
    return impl->IsExecuting();
}

std::string Jit::Disassemble() const {
    return impl->Disassemble();
}

}  // namespace Dynarmic::A64
