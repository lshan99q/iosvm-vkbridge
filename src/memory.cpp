// SPDX-License-Identifier: MIT

#include "vkbridge/memory.hpp"

#include <algorithm>
#include <utility>

#include "vkbridge/vk_result.hpp"

namespace vkbridge
{
namespace
{

Result<uint32_t> find_memory_type(const VkPhysicalDeviceMemoryProperties &properties,
                                  uint32_t type_bits, VkMemoryPropertyFlags required,
                                  VkMemoryPropertyFlags preferred)
{
    bool have_fallback = false;
    uint32_t fallback = 0;
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((type_bits & (1u << index)) == 0) {
            continue;
        }
        const VkMemoryPropertyFlags flags = properties.memoryTypes[index].propertyFlags;
        if ((flags & required) != required) {
            continue;
        }
        if ((flags & preferred) == preferred) {
            return ok_result<uint32_t>(index);
        }
        if (!have_fallback) {
            fallback = index;
            have_fallback = true;
        }
    }
    if (have_fallback) {
        return ok_result<uint32_t>(fallback);
    }
    return err_result<uint32_t>(Error::Unsupported, "no compatible memory type");
}

} // namespace

Result<MemoryBlock> allocate_memory(Device &device, const VkMemoryRequirements &requirements,
                                    MemoryUse use)
{
    if (device.handle() == VK_NULL_HANDLE) {
        return err_result<MemoryBlock>(Error::InvalidArgument, "device is not valid");
    }
    if (requirements.size == 0) {
        return err_result<MemoryBlock>(Error::InvalidArgument, "memory requirements are empty");
    }

    const bool host_visible = use == MemoryUse::HostVisible;
    const VkMemoryPropertyFlags required =
        host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkMemoryPropertyFlags preferred = host_visible ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0;

    const VkPhysicalDeviceMemoryProperties &properties = device.memory_properties();
    Result<uint32_t> type =
        find_memory_type(properties, requirements.memoryTypeBits, required, preferred);
    if (!type && !host_visible) {
        // Some UMA/software drivers have no separate device-local heap; accept any
        // compatible type in that case rather than failing the allocation.
        type = find_memory_type(properties, requirements.memoryTypeBits, 0,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (!type) {
        return err_result<MemoryBlock>(type.status());
    }

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = type.value();

    VkDeviceMemory memory = VK_NULL_HANDLE;
    Status status = vk_status(vkAllocateMemory(device.handle(), &allocate_info, nullptr, &memory),
                              "vkAllocateMemory");
    if (!status) {
        return err_result<MemoryBlock>(status);
    }

    MemoryBlock block;
    block.memory = memory;
    block.size = requirements.size;
    block.type_index = type.value();

    if (host_visible) {
        void *mapped = nullptr;
        status = vk_status(vkMapMemory(device.handle(), memory, 0, VK_WHOLE_SIZE, 0, &mapped),
                           "vkMapMemory");
        if (!status) {
            vkFreeMemory(device.handle(), memory, nullptr);
            return err_result<MemoryBlock>(status);
        }
        block.mapped = mapped;
    }

    return ok_result<MemoryBlock>(std::move(block));
}

Status free_memory(Device &device, MemoryBlock &block) noexcept
{
    if (block.memory == VK_NULL_HANDLE) {
        return status_ok();
    }
    if (block.mapped != nullptr && device.handle() != VK_NULL_HANDLE) {
        vkUnmapMemory(device.handle(), block.memory);
    }
    if (device.handle() != VK_NULL_HANDLE) {
        vkFreeMemory(device.handle(), block.memory, nullptr);
    }
    block = MemoryBlock{};
    return status_ok();
}

Result<BufferObject> allocate_buffer(Device &device, VkDeviceSize size, VkBufferUsageFlags usage,
                                     MemoryUse use)
{
    if (size == 0) {
        return err_result<BufferObject>(Error::InvalidArgument, "buffer size must be non-zero");
    }
    const VkDeviceSize limit = device.info().max_buffer_bytes;
    if (limit != 0 && size > limit) {
        return err_result<BufferObject>(Error::SizeTooLarge,
                                        "buffer exceeds maxMemoryAllocationSize");
    }

    // Every buffer must be reachable by the staging copy path.
    const VkBufferUsageFlags effective_usage =
        usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkBufferCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    create_info.size = size;
    create_info.usage = effective_usage;
    create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    Status status = vk_status(vkCreateBuffer(device.handle(), &create_info, nullptr, &buffer),
                              "vkCreateBuffer");
    if (!status) {
        return err_result<BufferObject>(status);
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device.handle(), buffer, &requirements);
    Result<MemoryBlock> memory = allocate_memory(device, requirements, use);
    if (!memory) {
        vkDestroyBuffer(device.handle(), buffer, nullptr);
        return err_result<BufferObject>(memory.status());
    }

    status = vk_status(vkBindBufferMemory(device.handle(), buffer, memory->memory, 0),
                       "vkBindBufferMemory");
    if (!status) {
        free_memory(device, memory.value());
        vkDestroyBuffer(device.handle(), buffer, nullptr);
        return err_result<BufferObject>(status);
    }

    BufferObject object;
    object.buffer = buffer;
    object.memory = memory.value();
    object.size = size;
    object.usage = effective_usage;
    return ok_result<BufferObject>(std::move(object));
}

Status release_buffer(Device &device, BufferObject &buffer) noexcept
{
    if (buffer.buffer != VK_NULL_HANDLE && device.handle() != VK_NULL_HANDLE) {
        vkDestroyBuffer(device.handle(), buffer.buffer, nullptr);
    }
    buffer.buffer = VK_NULL_HANDLE;
    free_memory(device, buffer.memory);
    buffer.size = 0;
    buffer.usage = 0;
    return status_ok();
}

StagingArena::StagingArena(Device &device) : device_(&device) {}

StagingArena::StagingArena(StagingArena &&other) noexcept
{
    take_from(other);
}

StagingArena &StagingArena::operator=(StagingArena &&other) noexcept
{
    if (this != &other) {
        destroy();
        take_from(other);
    }
    return *this;
}

StagingArena::~StagingArena()
{
    destroy();
}

void StagingArena::take_from(StagingArena &other) noexcept
{
    device_ = other.device_;
    buffer_ = other.buffer_;
    memory_ = other.memory_;
    capacity_ = other.capacity_;
    mapped_ = other.mapped_;
    coherent_ = other.coherent_;

    other.device_ = nullptr;
    other.buffer_ = VK_NULL_HANDLE;
    other.memory_ = VK_NULL_HANDLE;
    other.capacity_ = 0;
    other.mapped_ = nullptr;
    other.coherent_ = true;
}

Result<void *> StagingArena::ensure(VkDeviceSize bytes)
{
    if (device_ == nullptr || device_->handle() == VK_NULL_HANDLE) {
        return err_result<void *>(Error::InvalidArgument, "staging arena needs a valid device");
    }
    if (bytes == 0) {
        return err_result<void *>(Error::InvalidArgument, "staging request must be non-zero");
    }
    if (mapped_ != nullptr && bytes <= capacity_) {
        return ok_result<void *>(mapped_);
    }

    const VkDeviceSize target =
        std::max<VkDeviceSize>(bytes, std::max<VkDeviceSize>(capacity_ * 2, 64 * 1024));
    Result<BufferObject> allocated = allocate_buffer(*device_, target, 0, MemoryUse::HostVisible);
    if (!allocated) {
        return err_result<void *>(allocated.status());
    }
    if (allocated->memory.mapped == nullptr) {
        release_buffer(*device_, allocated.value());
        return err_result<void *>(Error::Internal, "staging buffer is not host-visible");
    }

    destroy();
    buffer_ = allocated->buffer;
    memory_ = allocated->memory.memory;
    mapped_ = allocated->memory.mapped;
    capacity_ = allocated->size;
    coherent_ =
        (device_->memory_properties().memoryTypes[allocated->memory.type_index].propertyFlags &
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    // The arena now owns the Vulkan objects; stop the Result from being released.
    allocated->buffer = VK_NULL_HANDLE;
    allocated->memory.memory = VK_NULL_HANDLE;
    allocated->memory.mapped = nullptr;

    return ok_result<void *>(mapped_);
}

Status StagingArena::flush_all()
{
    if (device_ == nullptr || memory_ == VK_NULL_HANDLE || coherent_) {
        return status_ok();
    }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = memory_;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    return vk_status(vkFlushMappedMemoryRanges(device_->handle(), 1, &range),
                     "vkFlushMappedMemoryRanges");
}

Status StagingArena::invalidate_all()
{
    if (device_ == nullptr || memory_ == VK_NULL_HANDLE || coherent_) {
        return status_ok();
    }
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = memory_;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    return vk_status(vkInvalidateMappedMemoryRanges(device_->handle(), 1, &range),
                     "vkInvalidateMappedMemoryRanges");
}

void StagingArena::destroy() noexcept
{
    if (device_ != nullptr && device_->handle() != VK_NULL_HANDLE) {
        if (mapped_ != nullptr && memory_ != VK_NULL_HANDLE) {
            vkUnmapMemory(device_->handle(), memory_);
        }
        if (buffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_->handle(), buffer_, nullptr);
        }
        if (memory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_->handle(), memory_, nullptr);
        }
    }
    buffer_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    capacity_ = 0;
    mapped_ = nullptr;
    coherent_ = true;
}

} // namespace vkbridge
