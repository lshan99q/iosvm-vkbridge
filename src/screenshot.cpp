// SPDX-License-Identifier: MIT

#include "vkbridge/screenshot.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "vkbridge/fence.hpp"
#include "vkbridge/pattern.hpp"
#include "vkbridge/vk_result.hpp"

namespace vkbridge
{
namespace
{

constexpr uint64_t kScreenshotTimeoutNs = 5'000'000'000ull;
constexpr uint32_t kMaxScreenshotDimension = 16384;

VkClearColorValue clear_value_from_rgba(uint32_t rgba)
{
    VkClearColorValue value{};
    value.float32[0] = static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f;
    value.float32[1] = static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f;
    value.float32[2] = static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f;
    value.float32[3] = static_cast<float>(rgba & 0xFFu) / 255.0f;
    return value;
}

bool format_supports_colour_attachment(VkPhysicalDevice physical_device, VkFormat format)
{
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
    const VkFormatFeatureFlags needed =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    return (properties.optimalTilingFeatures & needed) == needed;
}

struct RenderTarget {
    Device *device = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool bgra = false;

    RenderTarget() = default;
    RenderTarget(const RenderTarget &) = delete;
    RenderTarget &operator=(const RenderTarget &) = delete;
    ~RenderTarget()
    {
        destroy();
    }

    void destroy() noexcept
    {
        if (device != nullptr && device->handle() != VK_NULL_HANDLE) {
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device->handle(), framebuffer, nullptr);
            }
            if (render_pass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(device->handle(), render_pass, nullptr);
            }
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device->handle(), view, nullptr);
            }
            if (image != VK_NULL_HANDLE) {
                vkDestroyImage(device->handle(), image, nullptr);
            }
            if (memory != VK_NULL_HANDLE) {
                vkFreeMemory(device->handle(), memory, nullptr);
            }
        }
        device = nullptr;
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        view = VK_NULL_HANDLE;
        render_pass = VK_NULL_HANDLE;
        framebuffer = VK_NULL_HANDLE;
        format = VK_FORMAT_UNDEFINED;
        bgra = false;
    }
};

struct CommandBufferLease {
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;

    CommandBufferLease() = default;
    CommandBufferLease(const CommandBufferLease &) = delete;
    CommandBufferLease &operator=(const CommandBufferLease &) = delete;
    ~CommandBufferLease()
    {
        if (commands != VK_NULL_HANDLE && device != VK_NULL_HANDLE && pool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device, pool, 1, &commands);
        }
    }
};

Status create_render_target(Device &device, const ScreenshotOptions &options, RenderTarget &target)
{
    if (format_supports_colour_attachment(device.physical_device(), VK_FORMAT_R8G8B8A8_UNORM)) {
        target.format = VK_FORMAT_R8G8B8A8_UNORM;
    } else if (format_supports_colour_attachment(device.physical_device(),
                                                 VK_FORMAT_B8G8R8A8_UNORM)) {
        target.format = VK_FORMAT_B8G8R8A8_UNORM;
        target.bgra = true;
    } else {
        return status_fail(Error::Unsupported,
                           "no 8-bit RGBA colour attachment format with transfer support");
    }
    target.device = &device;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = target.format;
    image_info.extent = {options.width, options.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    Status status = vk_status(vkCreateImage(device.handle(), &image_info, nullptr, &target.image),
                              "vkCreateImage");
    if (!status) {
        return status;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device.handle(), target.image, &requirements);
    Result<MemoryBlock> memory = allocate_memory(device, requirements, MemoryUse::DeviceLocal);
    if (!memory) {
        return memory.status();
    }
    status = vk_status(vkBindImageMemory(device.handle(), target.image, memory->memory, 0),
                       "vkBindImageMemory");
    if (!status) {
        free_memory(device, memory.value());
        return status;
    }
    target.memory = memory->memory;

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = target.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = target.format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    status = vk_status(vkCreateImageView(device.handle(), &view_info, nullptr, &target.view),
                       "vkCreateImageView");
    if (!status) {
        return status;
    }

    VkAttachmentDescription attachment{};
    attachment.format = target.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference colour_reference{};
    colour_reference.attachment = 0;
    colour_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colour_reference;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = 0;
    dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;
    status = vk_status(
        vkCreateRenderPass(device.handle(), &render_pass_info, nullptr, &target.render_pass),
        "vkCreateRenderPass");
    if (!status) {
        return status;
    }

    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = target.render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &target.view;
    framebuffer_info.width = options.width;
    framebuffer_info.height = options.height;
    framebuffer_info.layers = 1;
    return vk_status(
        vkCreateFramebuffer(device.handle(), &framebuffer_info, nullptr, &target.framebuffer),
        "vkCreateFramebuffer");
}

void record_frame(VkCommandBuffer commands, const RenderTarget &target,
                  const ScreenshotLayout &layout, VkBuffer readback_buffer)
{
    VkClearValue background{};
    background.color = clear_value_from_rgba(layout.background_rgba);

    VkRenderPassBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.renderPass = target.render_pass;
    begin_info.framebuffer = target.framebuffer;
    begin_info.renderArea = {{0, 0}, {layout.width, layout.height}};
    begin_info.clearValueCount = 1;
    begin_info.pClearValues = &background;
    vkCmdBeginRenderPass(commands, &begin_info, VK_SUBPASS_CONTENTS_INLINE);

    for (const ColorRect &rect : layout.rects) {
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear.colorAttachment = 0;
        clear.clearValue.color = clear_value_from_rgba(rect.rgba);

        VkClearRect clear_rect{};
        clear_rect.rect = {{static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y)},
                           {rect.width, rect.height}};
        clear_rect.baseArrayLayer = 0;
        clear_rect.layerCount = 1;
        vkCmdClearAttachments(commands, 1, &clear, 1, &clear_rect);
    }

    vkCmdEndRenderPass(commands);

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = target.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {layout.width, layout.height, 1};
    vkCmdCopyImageToBuffer(commands, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_buffer, 1, &region);
}

} // namespace

Result<ScreenshotImage> render_screenshot(Device &device, VkCommandPool command_pool,
                                          StagingArena &staging, const ScreenshotOptions &options)
{
    if (device.handle() == VK_NULL_HANDLE) {
        return err_result<ScreenshotImage>(Error::InvalidArgument, "device is not valid");
    }
    if (!device.graphics_capable()) {
        return err_result<ScreenshotImage>(Error::Unsupported, "screenshot needs a graphics queue");
    }
    if (options.width == 0 || options.height == 0 || options.width > kMaxScreenshotDimension ||
        options.height > kMaxScreenshotDimension) {
        return err_result<ScreenshotImage>(Error::InvalidArgument,
                                           "screenshot dimensions are out of range");
    }

    const uint64_t pixel_count = static_cast<uint64_t>(options.width) * options.height;
    const VkDeviceSize byte_count = static_cast<VkDeviceSize>(pixel_count) * 4;

    const Result<void *> mapped = staging.ensure(byte_count);
    if (!mapped) {
        return err_result<ScreenshotImage>(mapped.status());
    }

    RenderTarget target;
    Status status = create_render_target(device, options, target);
    if (!status) {
        return err_result<ScreenshotImage>(status);
    }

    VkCommandBufferAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    status = vk_status(vkAllocateCommandBuffers(device.handle(), &allocate_info, &commands),
                       "vkAllocateCommandBuffers");
    if (!status) {
        return err_result<ScreenshotImage>(status);
    }
    CommandBufferLease lease;
    lease.device = device.handle();
    lease.pool = command_pool;
    lease.commands = commands;

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    status = vk_status(vkBeginCommandBuffer(commands, &begin_info), "vkBeginCommandBuffer");
    if (!status) {
        return err_result<ScreenshotImage>(status);
    }

    const ScreenshotLayout layout = make_screenshot_layout(options.width, options.height);
    record_frame(commands, target, layout, staging.buffer());

    status = vk_status(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
    if (!status) {
        return err_result<ScreenshotImage>(status);
    }

    Result<Fence> fence = Fence::create(device.handle(), 0);
    if (!fence) {
        return err_result<ScreenshotImage>(fence.status());
    }
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &commands;
    status =
        vk_status(vkQueueSubmit(device.queue(), 1, &submit_info, fence->handle()), "vkQueueSubmit");
    if (!status) {
        return err_result<ScreenshotImage>(status);
    }
    status = fence->wait(kScreenshotTimeoutNs);
    if (!status) {
        return err_result<ScreenshotImage>(status);
    }

    status = staging.invalidate_all();
    if (!status) {
        return err_result<ScreenshotImage>(status);
    }

    ScreenshotImage image;
    image.width = options.width;
    image.height = options.height;
    image.rgba.resize(static_cast<size_t>(byte_count));

    const uint8_t *source = static_cast<const uint8_t *>(*mapped);
    if (target.bgra) {
        for (uint64_t index = 0; index < pixel_count; ++index) {
            const size_t offset = static_cast<size_t>(index) * 4;
            image.rgba[offset + 0] = source[offset + 2];
            image.rgba[offset + 1] = source[offset + 1];
            image.rgba[offset + 2] = source[offset + 0];
            image.rgba[offset + 3] = source[offset + 3];
        }
    } else {
        std::copy(source, source + byte_count, image.rgba.begin());
    }

    image.unique_colours =
        count_unique_colours(image.rgba.data(), static_cast<size_t>(pixel_count));
    return ok_result<ScreenshotImage>(std::move(image));
}

} // namespace vkbridge
