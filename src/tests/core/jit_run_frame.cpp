// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../dynarmic/tests/A64/testenv.h"

namespace {
using Jit = Dynarmic::A64::Jit;
using Reason = Dynarmic::HaltReason;
constexpr u64 Base = 0x810000;
constexpr u64 Data = 0x7110;

class Environment final : public A64TestEnv {
public:
    Jit* jit{};
    bool check_frame{};
    bool abort_read{};
    size_t valid_frames{};
    std::vector<u32> calls;
    std::vector<u64> reads;

    u64 MemoryRead(u64 address, size_t size) override {
        reads.push_back(address);
        if (abort_read)
            jit->HaltExecution(Reason::MemoryAbort);
        return size == 1 ? 0xab : 0xcd;
    }

    [[gnu::noinline]] void CallSVC(u32 imm) override {
        if (check_frame) {
            // The native frame of this callback links to the JIT's outer frame. Immediately
            // below that frame is the return address of the call into the generated dispatcher.
            auto* callback = static_cast<std::uintptr_t*>(__builtin_frame_address(0));
            const auto caller = callback[0];
            const auto current = reinterpret_cast<std::uintptr_t>(callback);
            if (caller > current + sizeof(void*) && caller - current < 64 * 1024) {
                auto* frame = reinterpret_cast<const std::uintptr_t*>(caller);
                Dl_info native{};
                const auto inner = frame[-1];
                // Both generated sites are in this fixture's small code cache. The wrapper
                // returns to native C++, which must be discoverable by the dynamic linker.
                if (callback[1] > inner && callback[1] - inner < 4 * 1024 * 1024 &&
                    dladdr(reinterpret_cast<const void*>(frame[1]), &native) != 0) {
                    ++valid_frames;
                }
            }
        }
        calls.push_back(imm);
        jit->HaltExecution();
    }
};

struct Snapshot {
    std::array<u64, 31> registers;
    std::array<Vector, 32> vectors;
    u64 pc, sp;
    u32 flags;
    Reason reason;
    u64 ticks;
    std::vector<u32> calls;
    std::vector<u64> reads;
    bool operator==(const Snapshot&) const = default;
};

struct Fixture {
    Environment env;
    Jit jit;

    explicit Fixture(bool frame, bool cycles, void* fastmem = nullptr, bool recompile = false)
        : jit{[&] {
              Dynarmic::A64::UserConfig c;
              c.callbacks = &env;
              c.use_run_frame = frame;
              c.enable_cycle_counting = cycles;
              c.check_halt_on_memory_access = true;
              c.code_cache_size = 32 * 1024 * 1024;
              if (fastmem) {
                  c.fastmem_pointer = reinterpret_cast<std::uintptr_t>(fastmem);
                  c.fastmem_address_space_bits = 16;
                  c.silently_mirror_fastmem = false;
                  c.recompile_on_fastmem_failure = recompile;
              }
              return c;
          }()} {
        env.jit = &jit;
        env.check_frame = frame;
        env.code_mem_start_address = Base;
        env.code_mem = {
            0x52800060, // MOV W0,#3
            0x14000002, // B loop
            0xd503201f, // NOP
            0x71000400, // loop: SUBS W0,W0,#1
            0x54ffffe1, // B.NE loop
            0x39400041, // LDRB W1,[X2]
            0xd4000001, // SVC 0
            0x52800123, // MOV W3,#9
            0xd4000021, // SVC 1
        };
        for (size_t i = 0; i < 31; ++i)
            jit.SetRegister(i, 0xabc00000 + i);
        for (size_t i = 0; i < 32; ++i)
            jit.SetVector(i, {i, ~u64{i}});
        jit.SetRegister(2, Data);
        jit.SetSP(0x8000);
        jit.SetPstate(0x90000000);
        jit.SetPC(Base);
        env.ticks_left = 512;
    }

    Snapshot Execute(bool step = false) {
        const auto reason = step ? jit.Step() : jit.Run();
        return {jit.GetRegisters(), jit.GetVectors(), jit.GetPC(),
                jit.GetSP(),        jit.GetPstate(),  reason,
                env.ticks_left,     env.calls,        env.reads};
    }
};
} // namespace

TEST_CASE("A run-scoped JIT frame preserves linked execution and native callback frames",
          "[jit_run_frame]") {
    for (const bool cycles : {false, true}) {
        Fixture baseline{false, cycles}, candidate{true, cycles};
        for (size_t pass = 0; pass < 2; ++pass) {
            baseline.jit.SetPC(Base);
            candidate.jit.SetPC(Base);
            const auto first = baseline.Execute();
            REQUIRE(candidate.Execute() == first);
            REQUIRE(first.calls.back() == 0);
            REQUIRE(first.registers[0] == 0);
            REQUIRE(first.registers[1] == 0xab);
            REQUIRE(first.reads.back() == Data);
            REQUIRE(candidate.env.valid_frames == candidate.env.calls.size());
            REQUIRE(candidate.Execute() == baseline.Execute());
            REQUIRE(candidate.env.calls.back() == 1);
            REQUIRE(candidate.jit.GetRegister(3) == 9);
        }
        baseline.jit.SetPC(Base + 32);
        candidate.jit.SetPC(Base + 32);
        REQUIRE(candidate.Execute(true) == baseline.Execute(true));
        REQUIRE(candidate.jit.CodeCacheUsed() < baseline.jit.CodeCacheUsed());
        REQUIRE(candidate.env.valid_frames == candidate.env.calls.size());
    }
}

TEST_CASE("A run-scoped JIT frame returns precisely on a slow memory abort", "[jit_run_frame]") {
    Fixture baseline{false, false}, candidate{true, false};
    baseline.env.abort_read = candidate.env.abort_read = true;
    const auto abort = baseline.Execute();
    REQUIRE(candidate.Execute() == abort);
    REQUIRE((abort.reason & Reason::MemoryAbort) == Reason::MemoryAbort);
    REQUIRE(abort.calls.empty());
    REQUIRE(abort.reads == std::vector<u64>{Data});
    baseline.env.abort_read = candidate.env.abort_read = false;
    REQUIRE(candidate.Execute() == baseline.Execute());
    REQUIRE(candidate.env.valid_frames == 1);
}

// Run this separate diagnostic in a fresh process with PERF_BUILDID_DIR set before JIT creation.
TEST_CASE("A compiled public A64 block removes exactly its three repeated frame instructions",
          "[.jit_run_frame_encoding]") {
    const char* dir = std::getenv("PERF_BUILDID_DIR");
    REQUIRE(dir != nullptr);
    for (const bool frame : {false, true}) {
        Fixture fixture{frame, false};
        const u64 pc = frame ? Base + 0x1000 : Base;
        fixture.env.code_mem_start_address = pc;
        REQUIRE(fixture.jit.Precompile(pc, 0) == Dynarmic::A64::PrecompileResult::Compiled);
        std::ifstream map{std::filesystem::path{dir} /
                          ("perf-" + std::to_string(getpid()) + ".map")};
        const std::string wanted =
            frame ? "a64_0000000000811000_fpcr00000000" : "a64_0000000000810000_fpcr00000000";
        std::uintptr_t address{}, length{};
        std::string symbol;
        while (map >> std::hex >> address >> length >> symbol && symbol != wanted) {
        }
        REQUIRE(symbol == wanted);
        REQUIRE(length < 4096);
        const std::span code{reinterpret_cast<const u8*>(address), length};
        const auto count = [&](const std::array<u8, 8>& insn) {
            size_t n = 0;
            for (auto i = code.begin();
                 (i = std::search(i, code.end(), insn.begin(), insn.end())) != code.end(); ++i)
                ++n;
            return n;
        };
        CAPTURE(frame);
        CHECK(count({0x48, 0x89, 0xac, 0x24, 0x18, 0x04, 0x00, 0x00}) == (frame ? 0 : 1));
        CHECK(count({0x48, 0x8d, 0xac, 0x24, 0x10, 0x04, 0x00, 0x00}) == (frame ? 0 : 1));
        CHECK(count({0x48, 0x8b, 0xac, 0x24, 0x18, 0x04, 0x00, 0x00}) == (frame ? 0 : 1));
    }
}

namespace {
// This hidden fixture runs in an isolated process. Count real SIGSEGVs while forwarding
// the original context, on the same signal stack, to Dynarmic's installed handler.
std::atomic<unsigned> faults{};
static_assert(std::atomic<unsigned>::is_always_lock_free);
struct sigaction previous_action{};
void ForwardFault(int sig, siginfo_t* info, void* context) {
    faults.fetch_add(1, std::memory_order_relaxed);
    previous_action.sa_sigaction(sig, info, context);
}
} // namespace

TEST_CASE("A protected fastmem read resumes from its native signal fallback",
          "[.jit_run_frame_signal]") {
    constexpr size_t size = 1 << 16;
    void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(memory != MAP_FAILED);
    struct Mapping {
        void* data;
        ~Mapping() {
            munmap(data, size);
        }
    } mapping{memory};
    for (const bool recompile : {false, true}) {
        std::vector<Snapshot> reference;
        for (const bool frame : {false, true}) {
            CAPTURE(frame, recompile);
            Fixture fixture{frame, false, memory, recompile};
            REQUIRE(sigaction(SIGSEGV, nullptr, &previous_action) == 0);
            REQUIRE((previous_action.sa_flags & SA_SIGINFO) != 0);
            struct sigaction counting = previous_action;
            counting.sa_sigaction = ForwardFault;
            faults = 0;
            REQUIRE(sigaction(SIGSEGV, &counting, nullptr) == 0);
            struct RestoreSignal {
                ~RestoreSignal() {
                    sigaction(SIGSEGV, &previous_action, nullptr);
                }
            } restore;
            std::vector<Snapshot> observed;
            REQUIRE(mprotect(memory, size, PROT_READ | PROT_WRITE) == 0);
            static_cast<u8*>(memory)[Data] = 0x36;
            fixture.jit.SetPC(Base + 20);
            observed.push_back(fixture.Execute());
            REQUIRE(observed.back().registers[1] == 0x36);
            REQUIRE(observed.back().reads.empty());
            REQUIRE(faults == 0);
            REQUIRE(mprotect(memory, size, PROT_NONE) == 0);
            for (unsigned attempt = 1; attempt <= 2; ++attempt) {
                fixture.jit.SetPC(Base + 20);
                observed.push_back(fixture.Execute());
                REQUIRE(observed.back().registers[1] == 0xab);
                REQUIRE(observed.back().reads == std::vector<u64>(attempt, Data));
                REQUIRE(observed.back().calls == std::vector<u32>(attempt + 1, 0));
                REQUIRE(faults == (recompile ? 1u : attempt));
            }
            REQUIRE(fixture.env.valid_frames == (frame ? 3 : 0));
            if (frame) {
                REQUIRE(observed == reference);
            } else {
                reference = std::move(observed);
            }
        }
    }
}
