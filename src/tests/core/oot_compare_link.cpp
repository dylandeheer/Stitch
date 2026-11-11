// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "../../dynarmic/tests/A64/testenv.h"

namespace {
using Jit = Dynarmic::A64::Jit;
constexpr u64 Base = 0x810000;
constexpr u64 StringByte = 0x1234fff;
constexpr std::array<u32, 11> Program{
    0x39400144,             // LDRB W4,[X10]
    0x6b04007f,             // CMP W3,W4
    0x5a933671,             // CNEG W17,W19,HS
    0x1a910294,             // CSEL W20,W20,W17,EQ
    0x340000a3,             // CBZ W3,finish
    0x6b04007f,             // CMP W3,W4
    0x54000061,             // B.NE finish
    0x52800125, 0xd4000001, // MOV W5,#9; SVC 0
    0x52800045, 0xd4000001, // finish: MOV W5,#2; SVC 0
};

class Environment final : public A64TestEnv {
public:
    Jit* jit{};
    bool halt_on_read{};
    u8 value{};
    std::vector<std::pair<u64, size_t>> reads;
    std::optional<u32> MemoryReadCode(u64 pc) override {
        if (pc < Base || (pc - Base) % 4 || (pc - Base) / 4 >= code_mem.size())
            return {};
        return code_mem[(pc - Base) / 4];
    }
    u64 MemoryRead(u64 addr, size_t size) override {
        reads.emplace_back(addr, size);
        if (halt_on_read)
            jit->HaltExecution();
        return value;
    }
    void CallSVC(u32) override {
        jit->HaltExecution();
    }
};

struct Outcome {
    std::array<u64, 31> registers;
    std::array<Vector, 32> vectors;
    u64 pc{}, sp{};
    u32 flags{};
    std::vector<std::pair<u64, size_t>> reads;
    bool operator==(const Outcome&) const = default;
};

struct Fixture {
    Environment env;
    Jit jit;
    explicit Fixture(bool candidate, bool cycles = false, u8* fastmem = nullptr)
        : jit{[&] {
              Dynarmic::A64::UserConfig c;
              c.callbacks = &env;
              c.enable_cycle_counting = cycles;
              if (fastmem) {
                  c.fastmem_pointer = reinterpret_cast<std::uintptr_t>(fastmem);
                  c.fastmem_address_space_bits = 16;
                  c.silently_mirror_fastmem = false;
                  c.recompile_on_fastmem_failure = false;
              }
              return c;
          }()} {
        env.jit = &jit;
        env.code_mem.assign(Program.begin(), Program.end());
        if (candidate)
            jit.SetCompareLinkFilter([](u64 pc) -> std::optional<Jit::CompareWindow> {
                if (pc != Base)
                    return {};
                Jit::CompareWindow w{};
                std::copy_n(Program.begin(), w.size(), w.begin());
                return w;
            });
    }
    Outcome Run(u32 lhs, u8 rhs, u64 pc = Base, bool halt = false, bool step = false,
                u64 data_address = StringByte) {
        std::array<u64, 31> regs{};
        for (size_t i = 0; i < regs.size(); ++i)
            regs[i] = 0xabcde00000000000ULL + i * 17;
        regs[3] = lhs;
        regs[10] = data_address;
        std::array<Vector, 32> vectors{};
        for (size_t i = 0; i < vectors.size(); ++i)
            vectors[i] = {i * 11, i * 19 + 1};
        env.ticks_left = 1000;
        env.value = rhs;
        env.halt_on_read = halt;
        env.reads.clear();
        jit.ClearHalt(Dynarmic::HaltReason::UserDefined1);
        jit.ClearHalt(Dynarmic::HaltReason::Step);
        jit.SetRegisters(regs);
        jit.SetVectors(vectors);
        jit.SetPC(pc);
        jit.SetSP(0x8000);
        jit.SetPstate(0x90000000);
        if (step)
            (void)jit.Step();
        else
            (void)jit.Run();
        return {jit.GetRegisters(), jit.GetVectors(), jit.GetPC(),
                jit.GetSP(),        jit.GetPstate(),  env.reads};
    }
};
} // namespace

TEST_CASE("A fused compare preserves byte reads, registers, flags and both branch checkpoints",
          "[oot_compare_link]") {
    Fixture original{false}, candidate{true};
    for (u32 lhs : {0u, 1u, 127u, 128u, 255u})
        for (u8 rhs : std::array<u8, 5>{0, 1, 127, 128, 255}) {
            const auto ref = original.Run(lhs, rhs);
            REQUIRE(ref == candidate.Run(lhs, rhs));
            REQUIRE(ref.reads.size() == 1);
            REQUIRE(ref.reads[0].first == StringByte);
            REQUIRE(ref.reads[0].second == 1);
            REQUIRE(ref.registers[5] == (lhs && lhs == rhs ? 9 : 2));
        }
    REQUIRE(candidate.jit.CompareLinksCompiled() == 1);
    for (u32 lhs : {0u, 7u}) {
        const auto ref = original.Run(lhs, 7, Base, true);
        REQUIRE(ref == candidate.Run(lhs, 7, Base, true));
        REQUIRE(ref.pc == Base + (lhs == 0 ? 36 : 20));
    }
    for (unsigned offset : {4u, 8u, 12u, 16u, 20u, 24u})
        REQUIRE(original.Run(9, 9, Base + offset) == candidate.Run(9, 9, Base + offset));
}

TEST_CASE("Guest code invalidation includes both folded words and restores the baseline",
          "[oot_compare_link]") {
    Fixture original{false}, candidate{true};
    REQUIRE(original.Run(7, 7) == candidate.Run(7, 7));
    REQUIRE(candidate.jit.CompareLinksCompiled() == 1);
    size_t changed = 0;
    SECTION("Change the second comparison") {
        changed = 5;
    } // CMP W3,WZR makes 7 unequal.
    SECTION("Change the second branch") {
        changed = 6;
    } // B.EQ reverses the answer.
    for (auto* f : {&original, &candidate}) {
        f->env.code_mem[changed] = changed == 5 ? 0x6b1f007f : (Program[6] ^ 1);
        f->jit.InvalidateCacheRange(Base + 4 * changed, 4);
    }
    const auto ref = original.Run(7, 7);
    REQUIRE(ref.registers[5] == 2);
    REQUIRE(ref == candidate.Run(7, 7));
    REQUIRE(candidate.jit.CompareLinksCompiled() == 1);
}

TEST_CASE("Unverified words, cycle accounting and single stepping retain the normal code",
          "[oot_compare_link]") {
    Fixture stepping{true};
    REQUIRE(stepping.Run(1, 1, Base, false, true).pc == Base + 4);
    REQUIRE(stepping.jit.CompareLinksCompiled() == 0);
    Fixture cycles{true, true};
    REQUIRE(cycles.Run(1, 1).registers[5] == 9);
    REQUIRE(cycles.jit.CompareLinksCompiled() == 0);
    Fixture disabled{true};
    REQUIRE(disabled.Run(1, 1).registers[5] == 9);
    disabled.jit.SetCompareLinkFilter({});
    REQUIRE(disabled.Run(1, 1).registers[5] == 9);
    REQUIRE(disabled.jit.CompareLinksCompiled() == 1);
    Fixture mismatch{true};
    mismatch.env.code_mem[3] = 0xd503201f; // NOP replaces the selected move.
    REQUIRE(mismatch.Run(1, 1).registers[5] == 9);
    REQUIRE(mismatch.jit.CompareLinksCompiled() == 0);
}

#if defined(__linux__)
TEST_CASE("A scalar compare stops at the fastmem guard and retains out-of-range fallback",
          "[oot_compare_link]") {
    const size_t page = sysconf(_SC_PAGESIZE);
    REQUIRE(page < 0x10000);
    struct Mapping {
        u8* p;
        size_t size;
        ~Mapping() {
            if (p != MAP_FAILED)
                munmap(p, size);
        }
    } mapping{static_cast<u8*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)),
              2 * page};
    REQUIRE(mapping.p != MAP_FAILED);
    REQUIRE(mprotect(mapping.p + page, page, PROT_NONE) == 0);
    mapping.p[page - 1] = 0xa5;
    Fixture original{false, false, mapping.p}, candidate{true, false, mapping.p};
    const auto edge = original.Run(0xa5, 0, Base, false, false, page - 1);
    REQUIRE(edge == candidate.Run(0xa5, 0, Base, false, false, page - 1));
    REQUIRE(edge.reads.empty()); // the last accessible byte went directly through fastmem
    const auto missing = original.Run(7, 7, Base, false, false, 0x10000);
    REQUIRE(missing == candidate.Run(7, 7, Base, false, false, 0x10000));
    REQUIRE(missing.reads.size() ==
            1); // Non-mirrored address outside the configured window uses the original callback.
    REQUIRE(missing.reads[0].first == 0x10000);
    REQUIRE(missing.reads[0].second == 1);
    REQUIRE(candidate.jit.CompareLinksCompiled() == 1);
}
#endif

TEST_CASE("Warmup preserves checked compare links and folded tail invalidation",
          "[oot_compare_link][jit_warmup]") {
    for (size_t changed : {size_t{5}, size_t{6}}) {
        Fixture original{false}, warmed{true};
        REQUIRE(warmed.jit.Precompile(Base, 0) == Dynarmic::A64::PrecompileResult::Compiled);
        REQUIRE(warmed.env.reads.empty());
        REQUIRE(warmed.jit.CompareLinksCompiled() == 1);
        REQUIRE(original.Run(7, 7) == warmed.Run(7, 7));
        for (auto* fixture : {&original, &warmed}) {
            fixture->env.code_mem[changed] = changed == 5 ? 0x6b1f007f : (Program[6] ^ 1);
            fixture->jit.InvalidateCacheRange(Base + changed * 4, 4);
        }
        // A pending invalidation is applied before execution, even if warmup sees an old block.
        REQUIRE(warmed.jit.Precompile(Base, 0) == Dynarmic::A64::PrecompileResult::Present);
        const auto expected = original.Run(7, 7);
        REQUIRE(expected.registers[5] == 2);
        REQUIRE(expected == warmed.Run(7, 7));
        REQUIRE(warmed.jit.CompareLinksCompiled() == 1);
    }
}
