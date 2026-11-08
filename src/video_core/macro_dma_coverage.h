// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "common/common_types.h"

namespace VideoCommon {

// Front-thread only. One identity per channel, executable image, actual bound entry and method.
// Hash the whole native macro RAM plus the separately logged start: branches can leave a
// guessed local instruction range. Excess identities retain aggregate timing in an overflow row.
struct MacroDmaCoverage {
    static constexpr size_t MaxGroups = 64;
    struct Key {
        u64 as_id{}, code_generation{}, ram_hash{};
        s32 channel{-1};
        u32 method{}, start{};
        bool operator==(const Key&) const = default;
    };
    struct Direction {
        u64 requests{}, words{}, max_words{}, clean_entry_drain_ns{};
        std::array<u64, 3> sync_calls{}, blocked{}, wait_ns{}, max_ns{};
    };
    struct Group {
        Key key{};
        std::string_view shape{"unknown"};
        u64 invocations{}, parameter_words{}, draw_pending{}, instance_loop{}, sink_pending{};
        std::array<u64, 4> shadow_modes{};
        std::array<Direction, 2> reads{}; // persistent data RAM, private FIFO
    };
    static constexpr bool CleanEntry(bool pending, bool instance, bool sink, u32 shadow) {
        return !pending && !instance && !sink && shadow == 2; // Passthrough
    }
    struct Fingerprint {
        const void* owner{};
        u64 as_id{}, generation{}, hash{};
        s32 channel{-1};
    };

    template <typename Hash, typename Analyze>
    Direction* Begin(u64 window, u64& seen_window, bool fifo, u32 words, Key key, const void* owner,
                     std::span<const u32> code, size_t parameters, bool pending, bool instance,
                     bool sink, u32 shadow, Hash hash, Analyze analyze) {
        const Fingerprint* match{};
        for (size_t i = 0; owner && i < hash_count; ++i) {
            const auto& f = hashes[i];
            if (f.owner == owner && f.generation == key.code_generation && f.as_id == key.as_id &&
                f.channel == key.channel) {
                match = &f;
                break;
            }
        }
        if (owner && !match && hash_count < MaxGroups) {
            auto& f = hashes[hash_count++];
            f = {owner, key.as_id, key.code_generation, hash(code), key.channel};
            match = &f;
        }
        if (!owner)
            ++changed_image_reads;
        else if (!match)
            ++excess_image_reads;
        Group* row = &overflow;
        if (match) {
            key.ram_hash = match->hash;
            for (size_t i = 0; i < group_count; ++i) {
                if (groups[i].key == key) {
                    row = &groups[i];
                    break;
                }
            }
            if (row == &overflow && group_count < MaxGroups) {
                row = &groups[group_count++];
                row->key = key;
                row->shape = analyze(code, key.start);
            }
        }
        if (match && row == &overflow)
            ++excess_group_reads;
        if (seen_window != window) {
            seen_window = window;
            ++row->invocations;
            row->parameter_words += parameters;
            row->draw_pending += pending;
            row->instance_loop += instance;
            row->sink_pending += sink;
            if (shadow < row->shadow_modes.size())
                ++row->shadow_modes[shadow];
        }
        auto& direction = row->reads[fifo ? 1 : 0];
        ++direction.requests;
        direction.words += words;
        if (words > direction.max_words)
            direction.max_words = words;
        return &direction;
    }
    size_t hash_count{}, group_count{};
    u64 changed_image_reads{}, excess_image_reads{}, excess_group_reads{};
    std::array<Fingerprint, MaxGroups> hashes{};
    std::array<Group, MaxGroups> groups{};
    Group overflow{};
};
} // namespace VideoCommon
