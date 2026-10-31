// SPDX-License-Identifier: MIT

#include "vkbridge/replay.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "vkbridge/png.hpp"
#include "vkbridge/vk_result.hpp"

namespace vkbridge
{
namespace
{

constexpr VkDeviceSize kCopyAlignment = 4;
constexpr uint32_t kTimestampQueries = 2;

bool aligned(VkDeviceSize value)
{
    return (value % kCopyAlignment) == 0;
}

std::string describe_sequence(uint64_t sequence)
{
    return "fence sequence " + std::to_string(sequence);
}

} // namespace

Replay::Replay(Device &device, ReplayOptions options)
    : device_(&device), options_(std::move(options)), objects_(options_.max_objects),
      staging_(device)
{
}

Replay::~Replay()
{
    if (device_ == nullptr || device_->handle() == VK_NULL_HANDLE) {
        return;
    }
    VkDevice handle = device_->handle();
    vkDeviceWaitIdle(handle);
    oracle_.reset();
    for (ReplayFenceRecord &record : fences_) {
        if (record.commands != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(handle, pool_, 1, &record.commands);
        }
        if (record.fence != VK_NULL_HANDLE) {
            vkDestroyFence(handle, record.fence, nullptr);
        }
    }
    fences_.clear();
    for (const ObjectRecord &record : objects_.take_all()) {
        if (record.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(handle, record.buffer, nullptr);
        }
        if (record.image != VK_NULL_HANDLE) {
            vkDestroyImage(handle, record.image, nullptr);
        }
        if (record.memory != VK_NULL_HANDLE) {
            vkFreeMemory(handle, record.memory, nullptr);
        }
    }
    if (timestamps_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(handle, timestamps_, nullptr);
    }
    timestamps_ = VK_NULL_HANDLE;
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(handle, pool_, nullptr);
    }
    pool_ = VK_NULL_HANDLE;
}

Result<std::unique_ptr<Replay>> Replay::create(Device &device, ReplayOptions options)
{
    if (device.handle() == VK_NULL_HANDLE) {
        return err_result<std::unique_ptr<Replay>>(Error::InvalidArgument, "device is not valid");
    }
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device.queue_family();

    VkCommandPool pool = VK_NULL_HANDLE;
    const Status status = vk_status(
        vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool), "vkCreateCommandPool");
    if (!status) {
        return err_result<std::unique_ptr<Replay>>(status);
    }
    std::unique_ptr<Replay> replay(new Replay(device, std::move(options)));
    replay->pool_ = pool;

    if (device.info().timestamp_valid_bits != 0) {
        VkQueryPoolCreateInfo query_info{};
        query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = kTimestampQueries;
        VkQueryPool timestamps = VK_NULL_HANDLE;
        const Status query_status =
            vk_status(vkCreateQueryPool(device.handle(), &query_info, nullptr, &timestamps),
                      "vkCreateQueryPool");
        if (!query_status) {
            vkDestroyCommandPool(device.handle(), pool, nullptr);
            return err_result<std::unique_ptr<Replay>>(query_status);
        }
        replay->timestamps_ = timestamps;
    }

    return ok_result<std::unique_ptr<Replay>>(std::move(replay));
}

Status Replay::ensure_recording()
{
    if (recording_active_) {
        return status_ok();
    }
    VkCommandBufferAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = pool_;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;

    VkCommandBuffer commands = VK_NULL_HANDLE;
    Status status =
        vk_status(vkAllocateCommandBuffers(device_->handle(), &allocate_info, &commands),
                  "vkAllocateCommandBuffers");
    if (!status) {
        return status;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    status = vk_status(vkBeginCommandBuffer(commands, &begin_info), "vkBeginCommandBuffer");
    if (!status) {
        vkFreeCommandBuffers(device_->handle(), pool_, 1, &commands);
        return status;
    }

    timestamps_recorded_ = 0;
    if (timestamps_ != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(commands, timestamps_, 0, kTimestampQueries);
    }

    recording_ = commands;
    recording_active_ = true;
    return status_ok();
}

Status Replay::finish_submit(uint64_t *sequence_out)
{
    if (!recording_active_) {
        return status_fail(Error::InvalidArgument, "no commands were recorded");
    }
    VkCommandBuffer commands = recording_;
    Status status = vk_status(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
    recording_ = VK_NULL_HANDLE;
    recording_active_ = false;
    if (!status) {
        vkFreeCommandBuffers(device_->handle(), pool_, 1, &commands);
        return status;
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    status =
        vk_status(vkCreateFence(device_->handle(), &fence_info, nullptr, &fence), "vkCreateFence");
    if (!status) {
        vkFreeCommandBuffers(device_->handle(), pool_, 1, &commands);
        return status;
    }

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &commands;
    const VkResult result = vkQueueSubmit(device_->queue(), 1, &submit_info, fence);
    if (result != VK_SUCCESS) {
        vkDestroyFence(device_->handle(), fence, nullptr);
        vkFreeCommandBuffers(device_->handle(), pool_, 1, &commands);
        return vk_status(result, "vkQueueSubmit");
    }

    // The sequence is allocated only after the submission succeeded, so the numbers are
    // dense: every value in 1..last_sequence_ names a batch that reached the queue.
    const uint64_t sequence = sequence_.next();
    fences_.push_back(ReplayFenceRecord{sequence, fence, commands});
    last_sequence_ = sequence;
    ++submitted_;
    *sequence_out = sequence;
    return status_ok();
}

Result<uint64_t> Replay::submit()
{
    if (!recording_active_) {
        return err_result<uint64_t>(Error::InvalidArgument, "no commands were recorded");
    }
    uint64_t sequence = 0;
    const Status status = finish_submit(&sequence);
    if (!status) {
        return err_result<uint64_t>(status);
    }
    return ok_result<uint64_t>(sequence);
}

Status Replay::wait_fence(uint64_t sequence, uint64_t timeout_ns)
{
    if (sequence == 0) {
        return status_fail(Error::InvalidArgument, "sequence 0 is reserved");
    }
    for (auto record = fences_.begin(); record != fences_.end(); ++record) {
        if (record->sequence != sequence) {
            continue;
        }
        const VkResult result =
            vkWaitForFences(device_->handle(), 1, &record->fence, VK_TRUE, timeout_ns);
        if (result == VK_TIMEOUT) {
            return status_fail(Error::Timeout,
                               describe_sequence(sequence) + " was not signaled in time");
        }
        const Status status = vk_status(result, "vkWaitForFences");
        if (!status) {
            return status;
        }
        // Signaled: neither the fence nor the command buffer is in use anymore. Reap
        // both so fences do not accumulate for the lifetime of the replay.
        if (record->commands != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device_->handle(), pool_, 1, &record->commands);
        }
        vkDestroyFence(device_->handle(), record->fence, nullptr);
        fences_.erase(record);
        return status_ok();
    }
    if (sequence <= last_sequence_) {
        // Every submitted sequence is either still in fences_ or was reaped after its
        // fence signaled, so a repeated wait on a reaped sequence is a success.
        return status_ok();
    }
    return status_fail(Error::NotFound, "unknown " + describe_sequence(sequence));
}

Result<bool> Replay::fence_signaled(uint64_t sequence)
{
    for (const ReplayFenceRecord &record : fences_) {
        if (record.sequence != sequence) {
            continue;
        }
        const VkResult result = vkGetFenceStatus(device_->handle(), record.fence);
        if (result == VK_SUCCESS) {
            return ok_result<bool>(true);
        }
        if (result == VK_NOT_READY) {
            return ok_result<bool>(false);
        }
        return err_result<bool>(vk_status(result, "vkGetFenceStatus"));
    }
    if (sequence <= last_sequence_) {
        return ok_result<bool>(true);
    }
    return err_result<bool>(Error::NotFound, "unknown " + describe_sequence(sequence));
}

Status Replay::wait_all()
{
    while (!fences_.empty()) {
        const Status status = wait_fence(fences_.front().sequence, options_.fence_timeout_ns);
        if (!status) {
            return status;
        }
    }
    return status_ok();
}

Result<VkCommandBuffer> Replay::command_buffer_of(uint64_t sequence) const
{
    for (const ReplayFenceRecord &record : fences_) {
        if (record.sequence == sequence) {
            return ok_result<VkCommandBuffer>(record.commands);
        }
    }
    return err_result<VkCommandBuffer>(Error::NotFound,
                                       describe_sequence(sequence) + " is not outstanding");
}

Result<uint32_t> Replay::record_timestamp()
{
    if (timestamps_ == VK_NULL_HANDLE) {
        return err_result<uint32_t>(Error::Unsupported,
                                    "this device or queue family has no timestamps");
    }
    const Status status = ensure_recording();
    if (!status) {
        return err_result<uint32_t>(status);
    }
    if (timestamps_recorded_ >= kTimestampQueries) {
        return err_result<uint32_t>(Error::CapacityExceeded,
                                    "at most two timestamps fit in one submission");
    }
    const uint32_t token = timestamps_recorded_++;
    vkCmdWriteTimestamp(recording_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps_, token);
    return ok_result<uint32_t>(token);
}

Result<uint64_t> Replay::timestamp_ns(uint32_t token) const
{
    if (timestamps_ == VK_NULL_HANDLE || token >= timestamps_recorded_) {
        return err_result<uint64_t>(Error::NotFound, "timestamp token was not recorded");
    }
    uint64_t ticks = 0;
    const VkResult result =
        vkGetQueryPoolResults(device_->handle(), timestamps_, token, 1, sizeof(ticks), &ticks,
                              sizeof(ticks), VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY) {
        return err_result<uint64_t>(Error::Internal,
                                    "the timestamp query is not ready; wait for the submission");
    }
    if (result != VK_SUCCESS) {
        return err_result<uint64_t>(vk_status(result, "vkGetQueryPoolResults"));
    }
    const double nanoseconds = static_cast<double>(ticks) * device_->info().timestamp_period_ns;
    return ok_result<uint64_t>(static_cast<uint64_t>(std::llround(nanoseconds)));
}

Status Replay::flush()
{
    if (recording_active_) {
        uint64_t sequence = 0;
        const Status status = finish_submit(&sequence);
        if (!status) {
            return status;
        }
    }
    return wait_all();
}

Status Replay::one_shot_copy(VkBuffer src, VkDeviceSize src_offset, VkBuffer dst,
                             VkDeviceSize dst_offset, VkDeviceSize size)
{
    Status status = ensure_recording();
    if (!status) {
        return status;
    }
    VkBufferCopy region{};
    region.srcOffset = src_offset;
    region.dstOffset = dst_offset;
    region.size = size;
    vkCmdCopyBuffer(recording_, src, dst, 1, &region);

    uint64_t sequence = 0;
    status = finish_submit(&sequence);
    if (!status) {
        return status;
    }
    return wait_fence(sequence, options_.fence_timeout_ns);
}

Status Replay::require_buffer(uint32_t handle, VkBufferUsageFlags usage, VkDeviceSize offset,
                              VkDeviceSize size, const ObjectRecord **record) const
{
    if (handle == ObjectTable::kInvalidHandle) {
        return status_fail(Error::InvalidArgument, "handle 0 is reserved");
    }
    const ObjectRecord *found = objects_.find(handle);
    if (found == nullptr) {
        return status_fail(Error::NotFound, "unknown handle " + std::to_string(handle));
    }
    if (found->kind != ObjectKind::Buffer) {
        return status_fail(Error::InvalidArgument,
                           "handle " + std::to_string(handle) + " is not a buffer");
    }
    if ((found->usage & static_cast<uint32_t>(usage)) != static_cast<uint32_t>(usage)) {
        return status_fail(Error::InvalidArgument,
                           "buffer " + std::to_string(handle) + " is missing required usage flags");
    }
    if (offset > found->size || size > found->size - offset) {
        return status_fail(Error::InvalidArgument,
                           "range is outside buffer " + std::to_string(handle));
    }
    *record = found;
    return status_ok();
}

Result<uint32_t> Replay::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
    if (size == 0) {
        return err_result<uint32_t>(Error::InvalidArgument, "buffer size must be non-zero");
    }
    Result<BufferObject> buffer = allocate_buffer(*device_, size, usage, MemoryUse::DeviceLocal);
    if (!buffer) {
        return err_result<uint32_t>(buffer.status());
    }

    ObjectRecord record;
    record.kind = ObjectKind::Buffer;
    record.buffer = buffer->buffer;
    record.memory = buffer->memory.memory;
    record.size = buffer->size;
    record.usage = static_cast<uint32_t>(buffer->usage);
    record.memory_type_index = buffer->memory.type_index;
    record.host_visible = memory_type_is_host_visible(*device_, buffer->memory.type_index);
    record.device_local = memory_type_is_device_local(*device_, buffer->memory.type_index);

    Result<uint32_t> handle = objects_.add(record);
    if (!handle) {
        release_buffer(*device_, buffer.value());
        return err_result<uint32_t>(handle.status());
    }

    // The object table owns the Vulkan handles from here on.
    buffer->buffer = VK_NULL_HANDLE;
    buffer->memory.memory = VK_NULL_HANDLE;
    return handle;
}

Status Replay::write(uint32_t handle, const void *data, size_t size, VkDeviceSize offset)
{
    if (data == nullptr) {
        return status_fail(Error::InvalidArgument, "write source is null");
    }
    if (size == 0) {
        return status_fail(Error::InvalidArgument, "write size must be non-zero");
    }
    if (!aligned(static_cast<VkDeviceSize>(size)) || !aligned(offset)) {
        return status_fail(Error::Misaligned, "write offset and size must be multiples of 4");
    }
    const ObjectRecord *record = nullptr;
    Status status = require_buffer(handle, VK_BUFFER_USAGE_TRANSFER_DST_BIT, offset,
                                   static_cast<VkDeviceSize>(size), &record);
    if (!status) {
        return status;
    }

    status = flush();
    if (!status) {
        return status;
    }
    const Result<void *> mapped = staging_.ensure(static_cast<VkDeviceSize>(size));
    if (!mapped) {
        return mapped.status();
    }
    std::memcpy(*mapped, data, size);
    status = staging_.flush_all();
    if (!status) {
        return status;
    }
    return one_shot_copy(staging_.buffer(), 0, record->buffer, offset,
                         static_cast<VkDeviceSize>(size));
}

Result<std::vector<uint8_t>> Replay::readback(uint32_t handle, VkDeviceSize size,
                                              VkDeviceSize offset)
{
    if (size == 0) {
        return err_result<std::vector<uint8_t>>(Error::InvalidArgument,
                                                "readback size must be non-zero");
    }
    if (!aligned(size) || !aligned(offset)) {
        return err_result<std::vector<uint8_t>>(Error::Misaligned,
                                                "readback offset and size must be multiples of 4");
    }
    if (size > static_cast<VkDeviceSize>(std::numeric_limits<size_t>::max())) {
        return err_result<std::vector<uint8_t>>(Error::SizeTooLarge,
                                                "readback does not fit in host memory");
    }
    const ObjectRecord *record = nullptr;
    Status status = require_buffer(handle, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, offset, size, &record);
    if (!status) {
        return err_result<std::vector<uint8_t>>(status);
    }

    status = flush();
    if (!status) {
        return err_result<std::vector<uint8_t>>(status);
    }
    const Result<void *> mapped = staging_.ensure(size);
    if (!mapped) {
        return err_result<std::vector<uint8_t>>(mapped.status());
    }
    status = one_shot_copy(record->buffer, offset, staging_.buffer(), 0, size);
    if (!status) {
        return err_result<std::vector<uint8_t>>(status);
    }
    status = staging_.invalidate_all();
    if (!status) {
        return err_result<std::vector<uint8_t>>(status);
    }

    std::vector<uint8_t> out(static_cast<size_t>(size));
    std::memcpy(out.data(), *mapped, out.size());
    return ok_result<std::vector<uint8_t>>(std::move(out));
}

Status Replay::blit_copy(uint32_t src, uint32_t dst, VkDeviceSize size, VkDeviceSize src_offset,
                         VkDeviceSize dst_offset, uint32_t attestation, uint32_t nonce)
{
    if (size == 0) {
        return status_fail(Error::InvalidArgument, "copy size must be non-zero");
    }
    if (!aligned(size) || !aligned(src_offset) || !aligned(dst_offset)) {
        return status_fail(Error::Misaligned, "copy offsets and size must be multiples of 4");
    }
    const ObjectRecord *source = nullptr;
    Status status =
        require_buffer(src, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, src_offset, size, &source);
    if (!status) {
        return status;
    }
    const ObjectRecord *destination = nullptr;
    status = require_buffer(dst, VK_BUFFER_USAGE_TRANSFER_DST_BIT, dst_offset, size, &destination);
    if (!status) {
        return status;
    }
    if (source->buffer == destination->buffer && src_offset < dst_offset + size &&
        dst_offset < src_offset + size) {
        return status_fail(Error::InvalidArgument, "overlapping copy on the same buffer");
    }

    status = ensure_recording();
    if (!status) {
        return status;
    }
    VkBufferCopy region{};
    region.srcOffset = src_offset;
    region.dstOffset = dst_offset;
    region.size = size;
    vkCmdCopyBuffer(recording_, source->buffer, destination->buffer, 1, &region);

    if (attestation == ObjectTable::kInvalidHandle) {
        return status_ok();
    }
    if ((destination->usage & static_cast<uint32_t>(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) == 0) {
        return status_fail(Error::InvalidArgument,
                           "an attested copy needs VK_BUFFER_USAGE_STORAGE_BUFFER_BIT on the "
                           "destination");
    }
    if (size / 4 > static_cast<VkDeviceSize>(std::numeric_limits<uint32_t>::max())) {
        return status_fail(Error::SizeTooLarge, "the attested range does not fit in 32-bit words");
    }
    return record_attestation(destination->buffer, static_cast<uint32_t>(dst_offset / 4),
                              static_cast<uint32_t>(size / 4), attestation, nonce);
}

Status Replay::blit_fill(uint32_t handle, VkDeviceSize offset, VkDeviceSize size, uint32_t value32,
                         uint32_t attestation, uint32_t nonce)
{
    if (size == 0) {
        return status_fail(Error::InvalidArgument, "fill size must be non-zero");
    }
    if (size == VK_WHOLE_SIZE) {
        return status_fail(Error::InvalidArgument, "fill size must be explicit");
    }
    if (!aligned(offset) || !aligned(size)) {
        return status_fail(Error::Misaligned, "fill offset and size must be multiples of 4");
    }
    const ObjectRecord *record = nullptr;
    Status status = require_buffer(handle, VK_BUFFER_USAGE_TRANSFER_DST_BIT, offset, size, &record);
    if (!status) {
        return status;
    }

    status = ensure_recording();
    if (!status) {
        return status;
    }
    vkCmdFillBuffer(recording_, record->buffer, offset, size, value32);

    if (attestation == ObjectTable::kInvalidHandle) {
        return status_ok();
    }
    if ((record->usage & static_cast<uint32_t>(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) == 0) {
        return status_fail(Error::InvalidArgument,
                           "an attested fill needs VK_BUFFER_USAGE_STORAGE_BUFFER_BIT on the "
                           "buffer");
    }
    if (size / 4 > static_cast<VkDeviceSize>(std::numeric_limits<uint32_t>::max())) {
        return status_fail(Error::SizeTooLarge, "the attested range does not fit in 32-bit words");
    }
    return record_attestation(record->buffer, static_cast<uint32_t>(offset / 4),
                              static_cast<uint32_t>(size / 4), attestation, nonce);
}

Result<Oracle *> Replay::ensure_oracle()
{
    if (!oracle_) {
        Result<Oracle> created = Oracle::create(*device_);
        if (!created) {
            return err_result<Oracle *>(created.status());
        }
        oracle_ = std::make_unique<Oracle>(std::move(created.value()));
    }
    return ok_result<Oracle *>(oracle_.get());
}

Status Replay::record_attestation(VkBuffer source, uint32_t first_word, uint32_t word_count,
                                  uint32_t attestation, uint32_t nonce)
{
    const ObjectRecord *attestation_record = nullptr;
    Status status = require_buffer(
        attestation, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0, 8,
        &attestation_record);
    if (!status) {
        return status;
    }
    const Result<Oracle *> oracle = ensure_oracle();
    if (!oracle) {
        return oracle.status();
    }

    // The copy/fill wrote the range in this submission; make it visible to the shader.
    VkBufferMemoryBarrier to_compute{};
    to_compute.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_compute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_compute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_compute.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_compute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_compute.buffer = source;
    to_compute.offset = 0;
    to_compute.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(recording_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &to_compute, 0,
                         nullptr);

    status = (*oracle)->record(recording_, source, first_word, word_count, nonce, false,
                               attestation_record->buffer);
    if (!status) {
        return status;
    }

    // The result is read back through the staging path in a later submission; make the
    // shader write available to the transfer stage.
    VkBufferMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_transfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.buffer = attestation_record->buffer;
    to_transfer.offset = 0;
    to_transfer.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(recording_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &to_transfer, 0,
                         nullptr);
    return status_ok();
}

Result<ScreenshotInfo> Replay::screenshot(const std::string &path, const ScreenshotOptions &options)
{
    Status status = flush();
    if (!status) {
        return err_result<ScreenshotInfo>(status);
    }
    if (pool_ == VK_NULL_HANDLE) {
        return err_result<ScreenshotInfo>(Error::Internal, "replay has no command pool");
    }
    Result<ScreenshotImage> rendered = render_screenshot(*device_, pool_, staging_, options);
    if (!rendered) {
        return err_result<ScreenshotInfo>(rendered.status());
    }

    PngImage png;
    png.rgba = rendered->rgba.data();
    png.width = rendered->width;
    png.height = rendered->height;
    status = write_png(path, png);
    if (!status) {
        return err_result<ScreenshotInfo>(status);
    }

    ScreenshotInfo info;
    info.path = path;
    info.width = rendered->width;
    info.height = rendered->height;
    info.unique_colours = rendered->unique_colours;
    info.rgba = std::move(rendered->rgba);
    info.attestation = rendered->attestation;
    info.attestation_nonce = options.attestation_nonce;
    info.render_target_device_local = rendered->render_target_device_local;
    info.render_target_host_visible = rendered->render_target_host_visible;
    return ok_result<ScreenshotInfo>(std::move(info));
}

bool Replay::object_is_host_visible(uint32_t handle) const
{
    const ObjectRecord *record = objects_.find(handle);
    return record != nullptr && record->host_visible;
}

bool Replay::object_is_device_local(uint32_t handle) const
{
    const ObjectRecord *record = objects_.find(handle);
    return record != nullptr && record->device_local;
}

Status Replay::destroy_object(uint32_t handle)
{
    Status status = flush();
    if (!status) {
        return status;
    }
    Result<ObjectRecord> removed = objects_.remove(handle);
    if (!removed) {
        return removed.status();
    }
    if (removed->buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_->handle(), removed->buffer, nullptr);
    }
    if (removed->image != VK_NULL_HANDLE) {
        vkDestroyImage(device_->handle(), removed->image, nullptr);
    }
    if (removed->memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_->handle(), removed->memory, nullptr);
    }
    return status_ok();
}

Status Replay::apply(const Command &command)
{
    switch (command.type) {
    case CommandType::CreateBuffer: {
        Result<uint32_t> handle = create_buffer(static_cast<VkDeviceSize>(command.size),
                                                static_cast<VkBufferUsageFlags>(command.usage));
        if (!handle) {
            return handle.status();
        }
        if (command.handle_out != nullptr) {
            *command.handle_out = handle.value();
        }
        return status_ok();
    }
    case CommandType::Write:
        return write(command.handle, command.bytes, static_cast<size_t>(command.size),
                     static_cast<VkDeviceSize>(command.offset));
    case CommandType::BlitCopy:
        return blit_copy(command.src, command.dst, static_cast<VkDeviceSize>(command.size),
                         static_cast<VkDeviceSize>(command.offset),
                         static_cast<VkDeviceSize>(command.dst_offset), command.attestation,
                         command.nonce);
    case CommandType::BlitFill:
        return blit_fill(command.handle, static_cast<VkDeviceSize>(command.offset),
                         static_cast<VkDeviceSize>(command.size), command.value32,
                         command.attestation, command.nonce);
    case CommandType::Submit: {
        Result<uint64_t> sequence = submit();
        if (!sequence) {
            return sequence.status();
        }
        if (command.sequence_out != nullptr) {
            *command.sequence_out = sequence.value();
        }
        return status_ok();
    }
    case CommandType::WaitFence:
        return wait_fence(command.sequence, command.timeout_ns);
    case CommandType::Readback: {
        if (command.out == nullptr) {
            return status_fail(Error::InvalidArgument, "readback destination is null");
        }
        Result<std::vector<uint8_t>> data =
            readback(command.handle, static_cast<VkDeviceSize>(command.size),
                     static_cast<VkDeviceSize>(command.offset));
        if (!data) {
            return data.status();
        }
        if (command.out_size < data->size()) {
            return status_fail(Error::InvalidArgument, "readback destination is too small");
        }
        std::memcpy(command.out, data->data(), data->size());
        return status_ok();
    }
    case CommandType::Screenshot: {
        if (command.path == nullptr) {
            return status_fail(Error::InvalidArgument, "screenshot path is null");
        }
        Result<ScreenshotInfo> info = screenshot(command.path, command.screenshot);
        if (!info) {
            return info.status();
        }
        return status_ok();
    }
    }
    return status_fail(Error::InvalidArgument, "unknown command type");
}

Status Replay::execute(const Command *commands, size_t count)
{
    if (commands == nullptr && count != 0) {
        return status_fail(Error::InvalidArgument, "command array is null");
    }
    for (size_t index = 0; index < count; ++index) {
        const Status status = apply(commands[index]);
        if (!status) {
            return Status(status.code(),
                          "command " + std::to_string(index) + ": " + status.to_string());
        }
    }
    return status_ok();
}

} // namespace vkbridge
