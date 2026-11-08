// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vulkan/vulkan.h>

namespace Vulkan {

constexpr bool SupportsInitialZeroDepth(VkFormat format, VkImageUsageFlags usage,
                                        VkImageAspectFlags aspect) {
    return format == VK_FORMAT_D16_UNORM && (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
           aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
}

// The caller owns a fresh, bound, single-sample D16 allocation with exactly one level/layer.
template <typename Commands>
void RecordInitialZeroDepth(Commands& commands, VkImage image) {
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = range,
    };
    commands.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             barrier);
    commands.ClearDepthStencilImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, {0.0f, 0}, range);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    commands.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             barrier);
}

} // namespace Vulkan
