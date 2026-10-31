// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "vkbridge/device.hpp"
#include "vkbridge/fence.hpp"
#include "vkbridge/memory.hpp"
#include "vkbridge/object_table.hpp"
#include "vkbridge/oracle.hpp"
#include "vkbridge/screenshot.hpp"
#include "vkbridge/sequence.hpp"
#include "vkbridge/status.hpp"

namespace vkbridge
{
enum class CommandType : uint32_t {
    CreateBuffer = 1,
    Write,
    BlitCopy,
    BlitFill,
    Submit,
    WaitFence,
    Readback,
    Screenshot,
};

// Small explicit command description, the shape of the eventual guest wire
// format. Pointers in it are local-only: a serialized protocol would hand the
// payloads and replies through shared memory instead.
struct Command {
    CommandType type = CommandType::CreateBuffer;
    uint32_t handle = 0;
    uint32_t src = 0;
    uint32_t dst = 0;
    uint32_t usage = 0;   // VkBufferUsageFlags for CreateBuffer
    uint32_t value32 = 0; // BlitFill pattern
    uint64_t offset = 0;
    uint64_t dst_offset = 0;
    uint64_t size = 0;
    uint64_t sequence = 0;       // WaitFence target
    uint64_t timeout_ns = 0;     // WaitFence timeout
    const void *bytes = nullptr; // Write source
    void *out = nullptr;         // Readback destination
    size_t out_size = 0;         // Readback capacity
    const char *path = nullptr;  // Screenshot destination
    ScreenshotOptions screenshot{};
    // Device-side attestation for BlitCopy / BlitFill: a device-local storage buffer
    // the oracle shader writes into, plus the nonce mixed into its two result words.
    uint32_t attestation = 0;
    uint32_t nonce = 0;
    // Local-only outputs for CreateBuffer / Submit.
    uint32_t *handle_out = nullptr;
    uint64_t *sequence_out = nullptr;
};

struct ReplayOptions {
    size_t max_objects = 4096;
    uint64_t fence_timeout_ns = 5'000'000'000ull;
};

struct ScreenshotInfo {
    std::string path;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t unique_colours = 0;
    // Tightly packed RGBA8 copy of what was written, so the caller can compare
    // the GPU frame against its own model without reading the PNG back.
    std::vector<uint8_t> rgba;
    // Words the compute-shader oracle wrote after reading the rendered image on the
    // device. A host-only implementation of the render path never produces them.
    Attestation attestation;
    uint32_t attestation_nonce = 0;
    bool render_target_device_local = false;
    bool render_target_host_visible = false;
};

struct ReplayFenceRecord {
    uint64_t sequence = 0;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
};

// Replays explicit commands on the device queue. Copy and fill use real Vulkan
// commands (vkCmdCopyBuffer / vkCmdFillBuffer); host transfers only move bytes
// between the persistent staging buffer and device-local buffers.
//
// The Device (and the Instance it came from) must outlive this object.
class Replay
{
  public:
    static Result<std::unique_ptr<Replay>> create(Device &device, ReplayOptions options = {});
    ~Replay();

    Replay(const Replay &) = delete;
    Replay &operator=(const Replay &) = delete;

    Result<uint32_t> create_buffer(VkDeviceSize size, VkBufferUsageFlags usage);
    Status write(uint32_t handle, const void *data, size_t size, VkDeviceSize offset = 0);
    // When `attestation` is non-zero it must be a device-local storage buffer of at
    // least eight bytes. The copy/fill and the oracle dispatch are recorded into the
    // same command buffer, so a host-side replacement of this call leaves the
    // attestation buffer untouched: the shader is the only writer of its two words.
    // The oracle keeps one persistent descriptor set, so record at most one attested
    // copy/fill per submission and submit before recording the next one.
    Status blit_copy(uint32_t src, uint32_t dst, VkDeviceSize size, VkDeviceSize src_offset = 0,
                     VkDeviceSize dst_offset = 0, uint32_t attestation = 0, uint32_t nonce = 0);
    Status blit_fill(uint32_t handle, VkDeviceSize offset, VkDeviceSize size, uint32_t value32,
                     uint32_t attestation = 0, uint32_t nonce = 0);
    Result<uint64_t> submit();
    Status wait_fence(uint64_t sequence, uint64_t timeout_ns);
    Result<bool> fence_signaled(uint64_t sequence);
    Result<std::vector<uint8_t>> readback(uint32_t handle, VkDeviceSize size,
                                          VkDeviceSize offset = 0);
    Result<ScreenshotInfo> screenshot(const std::string &path,
                                      const ScreenshotOptions &options = {});
    Status destroy_object(uint32_t handle);

    // True when this object's allocation is host-visible. A caller that claims a result came
    // from the device can assert this is false first.
    bool object_is_host_visible(uint32_t handle) const;
    // True when this object's allocation is on a device-local heap.
    bool object_is_device_local(uint32_t handle) const;

    Status apply(const Command &command);
    Status execute(const Command *commands, size_t count);

    // Records a device timestamp at this point of the pending submission and returns a
    // token that timestamp_ns() resolves. Timestamps are written at the bottom of the
    // pipe, so a pair around a command measures that command's device time. The query
    // pool is reset when a recording starts: the previous submission must have been
    // waited on before the next recording begins.
    Result<uint32_t> record_timestamp();
    // Nanoseconds reported by a timestamp token. Only valid once the submission that
    // recorded it has signaled.
    Result<uint64_t> timestamp_ns(uint32_t token) const;
    bool device_timestamps_supported() const
    {
        return timestamps_ != VK_NULL_HANDLE;
    }

    // Submits anything still recording and waits for every outstanding fence.
    Status flush();

    Device &device()
    {
        return *device_;
    }
    const ObjectTable &objects() const
    {
        return objects_;
    }
    uint64_t last_sequence() const
    {
        return last_sequence_;
    }
    size_t submitted_batches() const
    {
        return submitted_;
    }
    const StagingArena &staging() const
    {
        return staging_;
    }
    // Fences that have been submitted but not yet waited on and reaped.
    size_t live_fences() const
    {
        return fences_.size();
    }
    // Command buffer of a submission that is still outstanding; NotFound once the
    // submission was waited on and its command buffer freed.
    Result<VkCommandBuffer> command_buffer_of(uint64_t sequence) const;

  private:
    Replay(Device &device, ReplayOptions options);

    Status ensure_recording();
    Status finish_submit(uint64_t *sequence_out);
    Status wait_all();
    Status one_shot_copy(VkBuffer src, VkDeviceSize src_offset, VkBuffer dst,
                         VkDeviceSize dst_offset, VkDeviceSize size);
    Status require_buffer(uint32_t handle, VkBufferUsageFlags usage, VkDeviceSize offset,
                          VkDeviceSize size, const ObjectRecord **record) const;
    Result<Oracle *> ensure_oracle();
    Status record_attestation(VkBuffer source, uint32_t first_word, uint32_t word_count,
                              uint32_t attestation, uint32_t nonce);

    Device *device_ = nullptr;
    ReplayOptions options_;
    ObjectTable objects_;
    StagingArena staging_;
    SequenceSource sequence_;
    std::unique_ptr<Oracle> oracle_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer recording_ = VK_NULL_HANDLE;
    bool recording_active_ = false;
    VkQueryPool timestamps_ = VK_NULL_HANDLE;
    uint32_t timestamps_recorded_ = 0;
    std::vector<ReplayFenceRecord> fences_;
    uint64_t last_sequence_ = 0;
    size_t submitted_ = 0;
};

} // namespace vkbridge
