// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <openssl/evp.h>
#include "common/common_types.h"
#include "common/hex_util.h"

namespace Core {
// Only the loader captures these words, after user mods, for the two digest-matched code pages.
struct OotLookupSites {
    using Window = std::array<u32, 7>;
    static constexpr std::array<u64, 2> page_offsets{0x58f000, 0x591000};
    static constexpr std::array<u64, 2> first_load{0x58fc9c, 0x591e5c};
    u64 base;
    std::array<std::array<Window, 7>, 2> windows{};

    static std::shared_ptr<const OotLookupSites> FromModule(std::span<const u8> module, u64 base,
                                                            u64 text_begin, u64 text_size) {
        constexpr std::array<std::string_view, 2> sha256{
            "02D9C2DC30A85D5C0942117F1898BE88DC94ED8E74BA5468D9B48E1B24A15414",
            "66D9CEE3F0D52D76B6B2484DE7A1EEAFDBD831984015C91D6E51EEC676FA38B2"};
        if (base & 4095)
            return {};
        auto sites = std::make_shared<OotLookupSites>();
        sites->base = base;
        for (size_t i = 0; i < page_offsets.size(); ++i) {
            const auto offset = page_offsets[i];
            if (offset < text_begin || offset - text_begin > text_size ||
                text_size - (offset - text_begin) < 4096 || offset > module.size() ||
                module.size() - offset < 4096)
                return {};
            std::array<u8, 32> digest{};
            unsigned int n{};
            if (EVP_Digest(module.data() + offset, 4096, digest.data(), &n, EVP_sha256(),
                           nullptr) != 1 ||
                n != digest.size() || Common::HexToString(digest) != sha256[i])
                return {};
            for (size_t lane = 0; lane < 7; ++lane) {
                std::memcpy(sites->windows[i][lane].data(),
                            module.data() + first_load[i] + lane * 36, sizeof(Window));
            }
        }
        return sites;
    }

    static bool MayContain(u64 pc) {
        // Module mappings are 4 KiB aligned. Avoid acquiring a shared_ptr on ordinary JIT misses.
        const u64 offset = pc & 4095;
        for (const u64 first : first_load) {
            const u64 delta = offset - (first & 4095);
            if (delta < 7 * 36 && delta % 36 == 0)
                return true;
        }
        return false;
    }

    std::optional<Window> Find(u64 pc) const {
        if (pc < base)
            return std::nullopt;
        const u64 offset = pc - base;
        for (size_t i = 0; i < first_load.size(); ++i) {
            if (offset < first_load[i])
                continue;
            const u64 delta = offset - first_load[i];
            if (delta < 7 * 36 && delta % 36 == 0)
                return windows[i][delta / 36];
        }
        return std::nullopt;
    }
};
} // namespace Core
