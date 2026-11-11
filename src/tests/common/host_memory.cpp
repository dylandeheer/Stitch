// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <vector>
#include <cstring>

#include <catch2/catch_test_macros.hpp>
#include "tests/common/host_memory_test_access.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

#include "common/host_memory.h"
#include "common/literals.h"

using Common::HostMemory;
using namespace Common::Literals;

static constexpr size_t VIRTUAL_SIZE = 1ULL << 39;
static constexpr size_t BACKING_SIZE = 4_GiB;
static constexpr auto PERMS = Common::MemoryPermission::ReadWrite;
static constexpr auto HEAP = false;

static bool MapsSingleGuestPages() {
#ifdef _WIN32
    return true;
#else
    return sysconf(_SC_PAGESIZE) == 0x1000;
#endif
}

TEST_CASE("HostMemory: Initialize and deinitialize", "[common]") {
    {
        HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
        REQUIRE(mem.BackingBasePointer() != nullptr);
    }
    {
        HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
        REQUIRE(mem.BackingBasePointer() != nullptr);
    }
}

TEST_CASE("HostMemory: Simple map", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x8000, 0x1000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[0] = 50;
    REQUIRE(data[0] == 50);
}

TEST_CASE("HostMemory: Aligned contiguous map", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x40000, 0x80000, 0x10000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x40000;
    data[0] = 1;
    data[0x5fff] = 2;
    data[0xffff] = 3;
    REQUIRE(mem.BackingBasePointer()[0x80000] == 1);
    REQUIRE(mem.BackingBasePointer()[0x85fff] == 2);
    REQUIRE(mem.BackingBasePointer()[0x8ffff] == 3);
}

TEST_CASE("HostMemory: Simple mirror map", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);
    mem.Map(0x8000, 0x4000, 0x1000, PERMS, HEAP);

    volatile u8* const mirror_a = mem.VirtualBasePointer() + 0x5000;
    volatile u8* const mirror_b = mem.VirtualBasePointer() + 0x8000;
    mirror_b[0] = 76;
    REQUIRE(mirror_a[0x1000] == 76);
}

TEST_CASE("HostMemory: Simple unmap", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[75] = 50;
    REQUIRE(data[75] == 50);

    mem.Unmap(0x5000, 0x2000, HEAP);
}

TEST_CASE("HostMemory: Simple unmap and remap", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);

    volatile u8* const data = mem.VirtualBasePointer() + 0x5000;
    data[0] = 50;
    REQUIRE(data[0] == 50);

    mem.Unmap(0x5000, 0x2000, HEAP);

    mem.Map(0x5000, 0x3000, 0x2000, PERMS, HEAP);
    REQUIRE(data[0] == 50);

    mem.Map(0x7000, 0x2000, 0x5000, PERMS, HEAP);
    REQUIRE(data[0x3000] == 50);
}

TEST_CASE("HostMemory: Nieche allocation", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x20000, PERMS, HEAP);
    mem.Unmap(0x0000, 0x4000, HEAP);
    mem.Map(0x1000, 0, 0x2000, PERMS, HEAP);
    mem.Map(0x3000, 0, 0x1000, PERMS, HEAP);
    mem.Map(0, 0, 0x1000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Full unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x8000, 0x4000, HEAP);
    mem.Map(0x6000, 0, 0x16000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Right out of bounds unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x2000, 0x4000, HEAP);
    mem.Map(0x2000, 0x80000, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Left out of bounds unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);

    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x6000, 0x4000, HEAP);
    mem.Map(0x8000, 0, 0x2000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Multiple placeholder unmap", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x1b000, PERMS, HEAP);
    mem.Unmap(0x3000, 0x1c000, HEAP);
    mem.Map(0x3000, 0, 0x20000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap between placeholders", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x0000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x2000, 0x4000, HEAP);
    mem.Map(0x2000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap to origin", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x4000, 0x4000, HEAP);
    mem.Map(0, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Unmap to right", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0, 0x4000, PERMS, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
    mem.Unmap(0x8000, 0x4000, HEAP);
    mem.Map(0x8000, 0, 0x4000, PERMS, HEAP);
}

TEST_CASE("HostMemory: Partial right unmap check bindings", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x1000] = 17;

    mem.Unmap(0x6000, 0x2000, HEAP);

    REQUIRE(ptr[0x1000] == 17);
}

TEST_CASE("HostMemory: Partial left unmap check bindings", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x3000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x4000, 0x2000, HEAP);

    REQUIRE(ptr[0x3000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Partial middle unmap check bindings", "[common]") {
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x4000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x0000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x1000, 0x2000, HEAP);

    REQUIRE(ptr[0x0000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("HostMemory: Partial sparse middle unmap and check bindings", "[common]") {
    if (!MapsSingleGuestPages()) {
        SKIP("Host pages larger than 4 KiB only map whole host pages");
    }
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x4000, 0x10000, 0x2000, PERMS, HEAP);
    mem.Map(0x6000, 0x20000, 0x2000, PERMS, HEAP);

    volatile u8* const ptr = mem.VirtualBasePointer() + 0x4000;
    ptr[0x0000] = 19;
    ptr[0x3fff] = 12;

    mem.Unmap(0x5000, 0x2000, HEAP);

    REQUIRE(ptr[0x0000] == 19);
    REQUIRE(ptr[0x3fff] == 12);
}

TEST_CASE("Bounded backing file reads preserve data across aliases and holes", "[atlas_pread]") {
    HostMemory mem(8_MiB, 16_MiB);
    REQUIRE(mem.BackingBasePointer());
    std::array<u8, 0x2200> scratch{};
    REQUIRE_FALSE(mem.TryReadBacking(8_MiB, scratch));
    REQUIRE_FALSE(mem.TryReadBacking(8_MiB - 4, scratch));
    REQUIRE_FALSE(mem.TryReadBacking(~size_t{}, scratch));
    REQUIRE_FALSE(mem.TryReadBacking(0, std::span<u8>{}));
    std::vector<u8> huge(4_MiB + 1);
    REQUIRE_FALSE(mem.TryReadBacking(0, huge));
#ifdef __linux__
    if (!MapsSingleGuestPages() || !mem.VirtualBasePointer()) SKIP("requires a shared 4 KiB fastmem mapping");
    mem.Map(0x10000, 0x8000, 0x3000, PERMS, HEAP);
    mem.Map(0x20000, 0x8000, 0x3000, PERMS, HEAP);
    auto* const alias = mem.VirtualBasePointer() + 0x10000;
    auto* const mirror = mem.VirtualBasePointer() + 0x20000;
    for (size_t i = 0; i < 4096; ++i) alias[i] = static_cast<u8>((i * 37) ^ 0xbc);
    std::fill_n(mirror + 0x2000, 0x1000, 0x5a);
    mem.Protect(0x10000, 0x3000, static_cast<Common::MemoryPermission>(0));
    REQUIRE(mem.TryReadBacking(0x8100, scratch));
    REQUIRE(std::equal(scratch.begin(), scratch.end(), mirror + 0x100));
    REQUIRE(std::all_of(scratch.begin()+0xf00, scratch.begin()+0x1f00, [](u8 b){ return b == 0; }));
    mirror[0x123] = 0xde;
    REQUIRE(mem.TryReadBacking(0x8123, std::span{scratch}.first(1)));
    REQUIRE(scratch.front() == 0xde);
#else
    REQUIRE_FALSE(mem.TryReadBacking(0, scratch));
#endif
}

TEST_CASE("An anonymous physical fallback never reads a backing file", "[atlas_pread]") {
    Common::HostMemoryTestAccess control(false);
    HostMemory mem(0x10000, 0x400000);
    REQUIRE(mem.BackingBasePointer());
    std::array<u8, 32> scratch{};
    REQUIRE_FALSE(mem.TryReadBacking(0, scratch));
}
#ifdef _WIN32
TEST_CASE("HostMemory: Trim zero pages keeps contents", "[common]") {
    constexpr size_t Page = 0x1000;
    HostMemory mem(BACKING_SIZE, VIRTUAL_SIZE);
    REQUIRE(mem.BackingBasePointer() != nullptr);
    mem.Map(0x10000, 0x100000, 2 * Page, PERMS, HEAP);
    // Two views next to each other: one run that VirtualUnlock can't take as a whole.
    mem.Map(0x12000, 0x200000, Page, PERMS, HEAP);
    mem.Map(0x13000, 0x300000, Page, PERMS, HEAP);
    u8* const view = mem.VirtualBasePointer() + 0x10000;
    u8* const backing = mem.BackingBasePointer();
    std::memset(view, 0, Page);
    std::memset(view + Page, 0x5a, Page);
    std::memset(view + 2 * Page, 0, 2 * Page);
    std::memset(backing + 0x400000, 0, Page);

    const auto resident = [](const void* address) {
        PSAPI_WORKING_SET_EX_INFORMATION info{};
        info.VirtualAddress = const_cast<void*>(address);
        REQUIRE(QueryWorkingSetEx(GetCurrentProcess(), &info, sizeof(info)));
        return info.VirtualAttributes.Valid != 0;
    };
    REQUIRE(resident(view));
    REQUIRE(!resident(backing + 0x100000 + Page));

    const auto [looked_at, trimmed] = mem.TrimZeroPages();
    REQUIRE(looked_at >= 5 * Page);
    REQUIRE(trimmed >= 4 * Page);
    REQUIRE(!resident(view));
    REQUIRE(!resident(view + 2 * Page));
    REQUIRE(!resident(view + 3 * Page));
    REQUIRE(!resident(backing + 0x400000));
    REQUIRE(resident(view + Page));
    REQUIRE(!resident(backing + 0x100000 + Page));

    const auto all = [](const u8* page, u8 value) {
        return std::all_of(page, page + Page, [value](u8 byte) { return byte == value; });
    };
    REQUIRE(all(view, 0));
    REQUIRE(all(view + Page, 0x5a));
    REQUIRE(all(view + 2 * Page, 0));
    REQUIRE(all(view + 3 * Page, 0));
    REQUIRE(all(backing + 0x400000, 0));
}
#endif
