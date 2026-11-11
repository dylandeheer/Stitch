// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstring>
#include <semaphore>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include "common/page_table.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "tests/common/host_memory_test_access.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <sys/uio.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace {
bool ProbeMemoryByte(u8* address, bool write = false) {
    u8 value{};
#ifdef _WIN32
    if (write) {
        MEMORY_BASIC_INFORMATION info{};
        return VirtualQuery(address, &info, sizeof(info)) == sizeof(info) &&
               info.Protect == PAGE_READWRITE;
    }
    SIZE_T transferred{};
    return ReadProcessMemory(GetCurrentProcess(), address, &value, 1, &transferred) &&
           transferred == 1;
#elif defined(__linux__)
    iovec local{&value, 1}, remote{address, 1};
    return (write ? process_vm_writev(getpid(), &local, 1, &remote, 1, 0)
                  : process_vm_readv(getpid(), &local, 1, &remote, 1, 0)) == 1;
#elif defined(__APPLE__)
    if (write) {
        return mach_vm_write(mach_task_self(), reinterpret_cast<mach_vm_address_t>(address),
                             reinterpret_cast<vm_offset_t>(&value), 1) == KERN_SUCCESS;
    }
    mach_vm_size_t transferred{};
    return mach_vm_read_overwrite(mach_task_self(), reinterpret_cast<mach_vm_address_t>(address),
                                  1, reinterpret_cast<mach_vm_address_t>(&value), &transferred) ==
               KERN_SUCCESS && transferred == 1;
#else
    return false;
#endif
}
} // namespace

TEST_CASE("Memory acquire reads and exclusive publication use mapped backing", "[hwacod_memory]") {
    ScopedSystemTestContext context;
    Core::System system;
    Core::Memory::Memory unbound{system};
    u32 value = 99;
    REQUIRE_FALSE(unbound.Read32Acquire(0x4000, value));
    REQUIRE(value == 0);

    system.Initialize();
    Kernel::KProcess process{system.Kernel()};
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto* page = system.DeviceMemory().buffer.BackingBasePointer() + 0x10000;
    std::memset(page, 0, 0x1000);
    table.entries.GetAndFault(4).Store(false, Common::PageType::Memory, 1,
                                     reinterpret_cast<uintptr_t>(page) - 0x4000);
    auto& memory = process.GetMemory();
    memory.SetCurrentPageTable(process);
    REQUIRE(memory.Read32Acquire(0x4000, value));
    REQUIRE(value == 0);
    REQUIRE(memory.WriteExclusive32(0x4000, 0x12345678, 0));
    REQUIRE_FALSE(memory.WriteExclusive32(0x4000, 99, 0));
    REQUIRE(memory.Read32Acquire(0x4000, value));
    REQUIRE(value == 0x12345678);
    REQUIRE(memory.WriteExclusive32(0x4ffc, 0xffffffff, 0));
    REQUIRE(memory.Read32Acquire(0x4ffc, value));
    REQUIRE(value == 0xffffffff);
    for (const u64 address : {0x4001ULL, 0x5000ULL, 0x100000ULL}) {
        value = 99;
        REQUIRE_FALSE(memory.Read32Acquire(address, value));
        REQUIRE(value == 0);
    }

    for (u32 iteration = 0; iteration < 32; ++iteration) {
        CAPTURE(iteration);
        const u32 zero{};
        REQUIRE(memory.WriteBlock(0x4000, &zero, sizeof(zero)));
        std::array<u32, 16> expected{};
        expected.fill(iteration + 1);
        bool written{};
        std::jthread writer{[&] {
            written = memory.WriteBlock(0x4080, expected.data(), sizeof(expected)) &&
                      memory.WriteExclusive32(0x4000, 1, 0);
        }};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        bool acquired{};
        do {
            if (!memory.Read32Acquire(0x4000, value)) {
                break;
            }
            acquired = value == 1;
            if (!acquired) {
                std::this_thread::yield();
            }
        } while (!acquired && std::chrono::steady_clock::now() < deadline);
        std::array<u32, 16> observed{};
        const bool read = acquired && memory.ReadBlock(0x4080, observed.data(), sizeof(observed));
        writer.join();
        REQUIRE(written);
        REQUIRE(acquired);
        REQUIRE(read);
        REQUIRE(observed == expected);
    }
}

TEST_CASE("Virtual address ranges that wrap past the address space end are invalid",
          "[hwacod_memory]") {
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    Kernel::KProcess process{system.Kernel()};
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto* page = system.DeviceMemory().buffer.BackingBasePointer() + 0x10000;
    table.entries.GetAndFault(4).Store(false, Common::PageType::Memory, 1,
                                     reinterpret_cast<uintptr_t>(page) - 0x4000);
    auto& memory = process.GetMemory();
    memory.SetCurrentPageTable(process);
    REQUIRE(memory.IsValidVirtualAddressRange(0x4000, 0x1000));
    REQUIRE(memory.IsValidVirtualAddressRange(0x4ff8, 8));
    REQUIRE_FALSE(memory.IsValidVirtualAddressRange(0x4ffc, 8));
    REQUIRE_FALSE(memory.IsValidVirtualAddressRange(~u64{0} - 3, 8));
    REQUIRE_FALSE(memory.IsValidVirtualAddressRange(0x4000, ~u64{0}));
}

TEST_CASE("Cached reprotection never grants reads to an armed page",
          "[core][read_trap_protection][hwacod_memory]") {
#if !defined(_WIN32) && !defined(__linux__) && !defined(__APPLE__)
    SKIP("This platform has no non-faulting process-memory probe");
#endif
    ScopedSystemTestContext context;
    const bool previous_reactive = Settings::values.use_reactive_flushing.GetValue();
    const bool previous_force = Settings::values.force_reactive_flushing;
    const auto previous_backend = Settings::values.cpu_backend.GetValue();
    Settings::values.use_reactive_flushing.SetValue(false);
    Settings::values.force_reactive_flushing = false;
    Settings::values.cpu_backend.SetValue(Settings::CpuBackend::Dynarmic);
    SCOPE_EXIT {
        Settings::values.use_reactive_flushing.SetValue(previous_reactive);
        Settings::values.force_reactive_flushing = previous_force;
        Settings::values.cpu_backend.SetValue(previous_backend);
    };

    Core::System system;
    REQUIRE(system.Initialize());
    Kernel::KProcess process{system.Kernel()};
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto& memory = process.GetMemory();
    memory.SetCurrentPageTable(process);
    auto& host = system.DeviceMemory().buffer;
    REQUIRE(host.VirtualBasePointer() != nullptr);
    table.fastmem_arena = host.VirtualBasePointer();

    constexpr u64 Begin = 0x10000, Armed = 0x20000, Last = 0x30000, Size = 0x30000;
    memory.MapMemoryRegion(table, Begin, Size, Core::DramMemoryMap::Base + Begin,
                           Common::MemoryPermission::ReadWrite, false);
    memory.RasterizerMarkRegionCached(Begin, Size, true);
    memory.RasterizerArmReadTraps(Armed, 0x1000);
    auto* const base = host.VirtualBasePointer();
    REQUIRE(ProbeMemoryByte(base + Begin));
    REQUIRE_FALSE(ProbeMemoryByte(base + Armed));
    REQUIRE(ProbeMemoryByte(base + Last));
    REQUIRE_FALSE(ProbeMemoryByte(base + Begin, true));

    struct Probe {
        u8* base;
        std::binary_semaphore ready{0}, finished{0};
        bool observed{}, completed{}, waited{};
        std::array<bool, 3> readable{};
    } probe{base};
    std::jthread reader{[&] {
        if (!probe.ready.try_acquire_for(std::chrono::seconds{5})) {
            return;
        }
        probe.readable = {ProbeMemoryByte(base + Begin), ProbeMemoryByte(base + Armed),
                          ProbeMemoryByte(base + Last)};
        probe.completed = true;
        probe.finished.release();
    }};
    {
        Common::HostMemoryTestAccess::ProtectionObserver observer{
            &probe, [](void* context, size_t, size_t, Common::MemoryPermission) {
                auto& state = *static_cast<Probe*>(context);
                if (state.observed) {
                    return;
                }
                state.observed = true;
                state.ready.release();
                state.waited = state.finished.try_acquire_for(std::chrono::seconds{5});
            }};
        memory.RasterizerMarkRegionCached(Begin, Size, true);
    }
    reader.join();
    REQUIRE(probe.observed);
    REQUIRE(probe.waited);
    REQUIRE(probe.completed);
    CHECK(probe.readable[0]);
    CHECK_FALSE(probe.readable[1]);
    CHECK(probe.readable[2]);
    CHECK_FALSE(ProbeMemoryByte(base + Armed));
    CHECK_FALSE(ProbeMemoryByte(base + Begin, true));

    // Uncaching restores the original read/write policy. Recaching keeps the outstanding trap.
    memory.RasterizerMarkRegionCached(Begin, Size, false);
    CHECK(ProbeMemoryByte(base + Armed));
    CHECK(ProbeMemoryByte(base + Armed, true));
    memory.RasterizerMarkRegionCached(Begin, Size, true);
    CHECK_FALSE(ProbeMemoryByte(base + Armed));
    CHECK(ProbeMemoryByte(base + Last));

    std::jthread armer{[&] {
        for (int i = 0; i < 128; ++i) {
            memory.RasterizerArmReadTraps(Begin, 0x1000);
        }
    }};
    for (int i = 0; i < 128; ++i) {
        memory.RasterizerMarkRegionCached(Begin, Size, true);
    }
    armer.join();
    CHECK_FALSE(ProbeMemoryByte(base + Begin));
    CHECK_FALSE(ProbeMemoryByte(base + Armed));
    CHECK(ProbeMemoryByte(base + Last));

    memory.RasterizerMarkRegionCached(Begin, Size, false);
    memory.UnmapRegion(table, Begin, Size, false);
    memory.MapMemoryRegion(table, Begin, Size, Core::DramMemoryMap::Base + Begin,
                           Common::MemoryPermission::ReadWrite, false);
    CHECK(ProbeMemoryByte(base + Armed));
    CHECK(ProbeMemoryByte(base + Armed, true));
}
