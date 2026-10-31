// SPDX-License-Identifier: MIT

#include "vkbridge/screenshot.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
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
constexpr VkDeviceSize kAttestationBytes = 8;

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
    uint32_t memory_type_index = 0;
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
        destroy_handles_only();
    }

    // Keeps the Vulkan handles but forgets them, so the destructor does not destroy
    // objects that may still be in use after a device loss.
    void release() noexcept
    {
        destroy_handles_only();
    }

  private:
    void destroy_handles_only() noexcept
    {
        device = nullptr;
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        view = VK_NULL_HANDLE;
        render_pass = VK_NULL_HANDLE;
        framebuffer = VK_NULL_HANDLE;
        format = VK_FORMAT_UNDEFINED;
        memory_type_index = 0;
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

    void release() noexcept
    {
        device = VK_NULL_HANDLE;
        pool = VK_NULL_HANDLE;
        commands = VK_NULL_HANDLE;
    }
};

// Owns a device-local buffer for the duration of the screenshot call.
struct BufferLease {
    Device *device = nullptr;
    BufferObject object;

    BufferLease() = default;
    BufferLease(const BufferLease &) = delete;
    BufferLease &operator=(const BufferLease &) = delete;
    ~BufferLease()
    {
        if (device != nullptr) {
            release_buffer(*device, object);
        }
    }

    void release() noexcept
    {
        device = nullptr;
        object = BufferObject{};
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
    target.memory_type_index = memory->type_index;

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

// Records the copy of the rendered image into the device-local attestation input and
// the compute dispatch that checksums it. `swizzle` converts B8G8R8A8 words so the
// checksum matches the RGBA order the host receives.
Status record_attestation(VkCommandBuffer commands, const RenderTarget &target,
                          const ScreenshotLayout &layout, VkBuffer image_bytes,
                          const Oracle &oracle, VkBuffer attestation, VkBuffer staging,
                          VkDeviceSize attestation_offset, const ScreenshotOptions &options)
{
    const VkDeviceSize byte_count = static_cast<VkDeviceSize>(layout.width) * layout.height * 4u;

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {layout.width, layout.height, 1};
    vkCmdCopyImageToBuffer(commands, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           image_bytes, 1, &region);

    VkBufferMemoryBarrier to_compute{};
    to_compute.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_compute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_compute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_compute.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_compute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_compute.buffer = image_bytes;
    to_compute.offset = 0;
    to_compute.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &to_compute, 0,
                         nullptr);

    Status status = oracle.record(commands, image_bytes, 0, static_cast<uint32_t>(byte_count / 4),
                                  options.attestation_nonce, target.bgra, attestation);
    if (!status) {
        return status;
    }

    VkBufferMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_transfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.buffer = attestation;
    to_transfer.offset = 0;
    to_transfer.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &to_transfer, 0,
                         nullptr);

    VkBufferCopy result_copy{};
    result_copy.srcOffset = 0;
    result_copy.dstOffset = attestation_offset;
    result_copy.size = kAttestationBytes;
    vkCmdCopyBuffer(commands, attestation, staging, 1, &result_copy);
    return status_ok();
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
    const VkDeviceSize staging_bytes = byte_count + kAttestationBytes;

    const Result<void *> mapped = staging.ensure(staging_bytes);
    if (!mapped) {
        return err_result<ScreenshotImage>(mapped.status());
    }

    RenderTarget target;
    Status status = create_render_target(device, options, target);
    if (!status) {
        return err_result<ScreenshotImage>(status);
    }

    BufferLease image_bytes;
    image_bytes.device = &device;
    Result<BufferObject> allocated_image = allocate_buffer(
        device, byte_count, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        MemoryUse::DeviceLocal);
    if (!allocated_image) {
        return err_result<ScreenshotImage>(allocated_image.status());
    }
    image_bytes.object = allocated_image.value();

    BufferLease attestation;
    attestation.device = &device;
    Result<BufferObject> allocated_attestation =
        allocate_buffer(device, kAttestationBytes,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        MemoryUse::DeviceLocal);
    if (!allocated_attestation) {
        return err_result<ScreenshotImage>(allocated_attestation.status());
    }
    attestation.object = allocated_attestation.value();

    Result<Oracle> created_oracle = Oracle::create(device);
    if (!created_oracle) {
        return err_result<ScreenshotImage>(created_oracle.status());
    }
    std::unique_ptr<Oracle> oracle(new Oracle(std::move(created_oracle.value())));

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
    status = record_attestation(commands, target, layout, image_bytes.object.buffer, *oracle,
                                attestation.object.buffer, staging.buffer(), byte_count, options);
    if (!status) {
        return err_result<ScreenshotImage>(status);
    }

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

    // From here on the submission is in flight: any error must drain the device before
    // the RAII owners destroy the fence, command buffer, image, memory and pipeline.
    status = fence->wait(kScreenshotTimeoutNs);
    if (!status) {
        const Status idle_status = vk_status(vkDeviceWaitIdle(device.handle()), "vkDeviceWaitIdle");
        if (!idle_status) {
            // The device did not drain (it is lost or reset). Destroying handles that may
            // still be in use is undefined behaviour, so deliberately leak them; the
            // caller reports the original failure and the process exits.
            target.release();
            lease.release();
            image_bytes.release();
            attestation.release();
            fence->release();
            oracle.release();
            return err_result<ScreenshotImage>(
                Status(status.code(), status.to_string() + "; vkDeviceWaitIdle also failed (" +
                                          idle_status.to_string() +
                                          "), so the in-flight fence, command buffer, image, "
                                          "buffers and compute pipeline were deliberately leaked"));
        }
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
    image.attestation.checksum = read_le32(source + byte_count);
    image.attestation.fingerprint = read_le32(source + byte_count + 4);

    MemoryTypeInfo type_info;
    if (memory_type_info(device, target.memory_type_index, type_info)) {
        image.render_target_device_local = type_info.device_local;
        image.render_target_host_visible = type_info.host_visible;
    }
    return ok_result<ScreenshotImage>(std::move(image));
}

} // namespace vkbridge
