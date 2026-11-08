// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>
#include <vector>
#include <vulkan/vulkan_core.h>

#include "common/common_types.h"

namespace Vulkan {

inline const char* ImagePoolBaselineFile() {
    static const char* const path = std::getenv("STITCH_IMAGE_POOL_BASELINE_FILE");
    return path && *path ? path : nullptr;
}

struct RetiredImageKey {
    VkFormat format;
    u32 width;
    u32 height;
    VkImageUsageFlags usage;
    VkImageCreateFlags flags;
    std::vector<VkFormat> view_formats;

    bool operator==(const RetiredImageKey&) const = default;

    // The remaining VkImageCreateInfo fields and the VMA allocation policy are constant.
    static std::optional<RetiredImageKey> From(const VkImageCreateInfo& ci, bool dedicated) {
        constexpr auto allowed_flags =
            VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        if (dedicated || ci.sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO ||
            ci.imageType != VK_IMAGE_TYPE_2D || ci.mipLevels != 1 || ci.arrayLayers != 1 ||
            ci.samples != VK_SAMPLE_COUNT_1_BIT || ci.extent.depth != 1 ||
            ci.extent.width == 0 || ci.extent.height == 0 || (ci.flags & ~allowed_flags) != 0 ||
            ci.tiling != VK_IMAGE_TILING_OPTIMAL || ci.sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
            ci.queueFamilyIndexCount != 0 || ci.pQueueFamilyIndices ||
            ci.initialLayout != VK_IMAGE_LAYOUT_UNDEFINED ||
            !(ci.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
            return std::nullopt;
        }
        RetiredImageKey key{ci.format, ci.extent.width, ci.extent.height, ci.usage, ci.flags, {}};
        if (ci.pNext) {
            const auto* next = static_cast<const VkBaseInStructure*>(ci.pNext);
            if (next->sType != VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO || next->pNext) {
                return std::nullopt;
            }
            const auto* list = reinterpret_cast<const VkImageFormatListCreateInfo*>(next);
            if (!list->pViewFormats || !list->viewFormatCount) {
                return std::nullopt;
            }
            key.view_formats.assign(list->pViewFormats, list->pViewFormats + list->viewFormatCount);
        }
        return key;
    }
};

struct RetiredImageTicket {
    RetiredImageKey key;
    u64 bytes{};
};

template <class Handle>
class RetiredImagePool {
public:
    static constexpr u64 MaxBytes = 128ULL << 20;
    static constexpr size_t MaxEntries = 64;
    static constexpr u64 MaxFrames = 120;

    Handle Take(RetiredImageTicket& ticket, u64 frame) {
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (frame >= it->available && it->ticket.key == ticket.key) {
                Handle handle = std::move(it->handle);
                ticket.bytes = it->ticket.bytes;
                bytes -= ticket.bytes;
                entries.erase(it);
                return handle;
            }
        }
        return {};
    }

    bool Put(RetiredImageTicket& ticket, Handle& handle, u64 frame, u64 available) {
        if (ticket.bytes == 0 || ticket.bytes > MaxBytes - bytes || entries.size() == MaxEntries) {
            return false;
        }
        entries.push_back({std::move(ticket), std::move(handle), frame, available});
        bytes += entries.back().ticket.bytes;
        return true;
    }

    size_t Trim(u64 frame, bool all) {
        size_t count = 0;
        while (!entries.empty() && (all || frame - entries.front().frame >= MaxFrames)) {
            bytes -= entries.front().ticket.bytes;
            entries.pop_front();
            ++count;
        }
        return count;
    }

    u64 Bytes() const { return bytes; }

private:
    struct Entry {
        RetiredImageTicket ticket;
        Handle handle;
        u64 frame;
        u64 available;
    };
    std::deque<Entry> entries;
    u64 bytes{};
};

} // namespace Vulkan
