// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan.h>

#include "vkbridge/device.hpp"
#include "vkbridge/status.hpp"

namespace vkbridge
{
// Second word the oracle shader writes, XORed with the per-call nonce. A result that
// does not carry this marker is not a fresh shader execution.
constexpr uint32_t kAttestationFingerprint = 0x4F524143u; // "ORAC"

// The two words the compute-shader oracle writes into a device-local buffer.
struct Attestation {
    uint32_t checksum = 0;
    uint32_t fingerprint = 0;
};

// What the device-side oracle must have produced for a byte range: the XOR of every
// little-endian 32-bit word, then the nonce. `bytes` must be a multiple of four.
uint32_t attestation_checksum(const uint8_t *data, std::size_t bytes, uint32_t nonce);

// Compute pipeline around shaders/oracle.comp. The shader reads a storage-buffer word
// range, XORs the words (optionally swapping the R and B bytes of each word), and writes
// {checksum ^ nonce, kAttestationFingerprint ^ nonce} into a second storage buffer.
//
// A host-side implementation of the operation being checked never records this dispatch,
// so the attestation buffer keeps its sentinel. The expected result is never written by
// the host: only the shader writes these two words.
class Oracle
{
  public:
    static Result<Oracle> create(Device &device);
    ~Oracle();

    Oracle(const Oracle &) = delete;
    Oracle &operator=(const Oracle &) = delete;
    Oracle(Oracle &&other) noexcept;
    Oracle &operator=(Oracle &&other) noexcept;

    // Records the dispatch. `source` is read from word `first_word` for `word_count`
    // words; the two result words are written at the start of `result`. Both buffers
    // must have been created with VK_BUFFER_USAGE_STORAGE_BUFFER_BIT and must be large
    // enough. The descriptor set is persistent, so the caller must submit and wait for
    // this record() before calling record() again.
    Status record(VkCommandBuffer commands, VkBuffer source, uint32_t first_word,
                  uint32_t word_count, uint32_t nonce, bool swizzle_red_blue,
                  VkBuffer result) const;

  private:
    Oracle() = default;
    void destroy() noexcept;

    Device *device_ = nullptr;
    VkShaderModule shader_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
};

} // namespace vkbridge
