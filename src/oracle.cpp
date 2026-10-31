// SPDX-License-Identifier: MIT

#include "vkbridge/oracle.hpp"

#include <limits>
#include <utility>

#include "vkbridge/oracle_spirv.hpp"
#include "vkbridge/pattern.hpp"
#include "vkbridge/vk_result.hpp"

namespace vkbridge
{
namespace
{

constexpr uint32_t kPushConstantBytes = 16;
constexpr uint32_t kDescriptorCount = 2;

} // namespace

uint32_t attestation_checksum(const uint8_t *data, std::size_t bytes, uint32_t nonce)
{
    uint32_t checksum = 0;
    const std::size_t words = bytes / 4;
    for (std::size_t word = 0; word < words; ++word) {
        checksum ^= read_le32(data + word * 4);
    }
    return checksum ^ nonce;
}

Result<Oracle> Oracle::create(Device &device)
{
    if (device.handle() == VK_NULL_HANDLE) {
        return err_result<Oracle>(Error::InvalidArgument, "oracle needs a valid device");
    }
    if ((device.queue_flags() & VK_QUEUE_COMPUTE_BIT) == 0) {
        return err_result<Oracle>(Error::Unsupported,
                                  "the selected queue family has no compute support, so the "
                                  "device-side oracle cannot run");
    }

    Oracle oracle;
    oracle.device_ = &device;

    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = kOracleSpirvWordCount * sizeof(std::uint32_t);
    shader_info.pCode = kOracleSpirv;
    Status status =
        vk_status(vkCreateShaderModule(device.handle(), &shader_info, nullptr, &oracle.shader_),
                  "vkCreateShaderModule");
    if (!status) {
        return err_result<Oracle>(status);
    }

    VkDescriptorSetLayoutBinding bindings[kDescriptorCount] = {};
    for (uint32_t index = 0; index < kDescriptorCount; ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = kDescriptorCount;
    layout_info.pBindings = bindings;
    status = vk_status(
        vkCreateDescriptorSetLayout(device.handle(), &layout_info, nullptr, &oracle.set_layout_),
        "vkCreateDescriptorSetLayout");
    if (!status) {
        oracle.destroy();
        return err_result<Oracle>(status);
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = kPushConstantBytes;

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &oracle.set_layout_;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    status = vk_status(vkCreatePipelineLayout(device.handle(), &pipeline_layout_info, nullptr,
                                              &oracle.pipeline_layout_),
                       "vkCreatePipelineLayout");
    if (!status) {
        oracle.destroy();
        return err_result<Oracle>(status);
    }

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = oracle.shader_;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = oracle.pipeline_layout_;
    status = vk_status(vkCreateComputePipelines(device.handle(), VK_NULL_HANDLE, 1, &pipeline_info,
                                                nullptr, &oracle.pipeline_),
                       "vkCreateComputePipelines");
    if (!status) {
        oracle.destroy();
        return err_result<Oracle>(status);
    }

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = kDescriptorCount;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    status = vk_status(
        vkCreateDescriptorPool(device.handle(), &pool_info, nullptr, &oracle.descriptor_pool_),
        "vkCreateDescriptorPool");
    if (!status) {
        oracle.destroy();
        return err_result<Oracle>(status);
    }

    VkDescriptorSetAllocateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = oracle.descriptor_pool_;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &oracle.set_layout_;
    status =
        vk_status(vkAllocateDescriptorSets(device.handle(), &set_info, &oracle.descriptor_set_),
                  "vkAllocateDescriptorSets");
    if (!status) {
        oracle.destroy();
        return err_result<Oracle>(status);
    }

    return ok_result<Oracle>(std::move(oracle));
}

Oracle::Oracle(Oracle &&other) noexcept
{
    *this = std::move(other);
}

Oracle &Oracle::operator=(Oracle &&other) noexcept
{
    if (this != &other) {
        destroy();
        device_ = other.device_;
        shader_ = other.shader_;
        set_layout_ = other.set_layout_;
        pipeline_layout_ = other.pipeline_layout_;
        pipeline_ = other.pipeline_;
        descriptor_pool_ = other.descriptor_pool_;
        descriptor_set_ = other.descriptor_set_;

        other.device_ = nullptr;
        other.shader_ = VK_NULL_HANDLE;
        other.set_layout_ = VK_NULL_HANDLE;
        other.pipeline_layout_ = VK_NULL_HANDLE;
        other.pipeline_ = VK_NULL_HANDLE;
        other.descriptor_pool_ = VK_NULL_HANDLE;
        other.descriptor_set_ = VK_NULL_HANDLE;
    }
    return *this;
}

Oracle::~Oracle()
{
    destroy();
}

void Oracle::destroy() noexcept
{
    if (device_ != nullptr && device_->handle() != VK_NULL_HANDLE) {
        VkDevice handle = device_->handle();
        if (descriptor_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(handle, descriptor_pool_, nullptr);
        }
        if (pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(handle, pipeline_, nullptr);
        }
        if (pipeline_layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(handle, pipeline_layout_, nullptr);
        }
        if (set_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(handle, set_layout_, nullptr);
        }
        if (shader_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(handle, shader_, nullptr);
        }
    }
    device_ = nullptr;
    shader_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_set_ = VK_NULL_HANDLE;
}

Status Oracle::record(VkCommandBuffer commands, VkBuffer source, uint32_t first_word,
                      uint32_t word_count, uint32_t nonce, bool swizzle_red_blue,
                      VkBuffer result) const
{
    if (commands == VK_NULL_HANDLE) {
        return status_fail(Error::InvalidArgument, "oracle dispatch needs a command buffer");
    }
    if (source == VK_NULL_HANDLE || result == VK_NULL_HANDLE) {
        return status_fail(Error::InvalidArgument, "oracle buffers are not valid");
    }
    if (word_count == 0) {
        return status_fail(Error::InvalidArgument, "the oracle range must be non-empty");
    }
    if (word_count > std::numeric_limits<uint32_t>::max() - first_word) {
        return status_fail(Error::InvalidArgument, "the oracle range overflows 32-bit words");
    }

    VkDescriptorBufferInfo source_info{};
    source_info.buffer = source;
    source_info.offset = 0;
    source_info.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo result_info{};
    result_info.buffer = result;
    result_info.offset = 0;
    result_info.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t index = 0; index < kDescriptorCount; ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = descriptor_set_;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[index].pBufferInfo = index == 0 ? &source_info : &result_info;
    }
    vkUpdateDescriptorSets(device_->handle(), kDescriptorCount, writes, 0, nullptr);

    const uint32_t parameters[kPushConstantBytes / sizeof(uint32_t)] = {
        first_word,
        word_count,
        nonce,
        swizzle_red_blue ? 1u : 0u,
    };

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0, 1,
                            &descriptor_set_, 0, nullptr);
    vkCmdPushConstants(commands, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       kPushConstantBytes, parameters);
    // One workgroup: the shader itself strides over the whole range and reduces the
    // XOR through shared memory, so the single result word cannot race.
    vkCmdDispatch(commands, 1, 1, 1);
    return status_ok();
}

} // namespace vkbridge
