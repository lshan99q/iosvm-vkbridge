// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vkbridge
{
// Monotonic, thread-safe sequence source. Every submitted batch is tagged with
// the next value, so a fence can be matched to its submission without relying on
// VkFence object identity.
class SequenceSource
{
  public:
    explicit SequenceSource(uint64_t first = 1) : next_(first) {}
    SequenceSource(const SequenceSource &) = delete;
    SequenceSource &operator=(const SequenceSource &) = delete;

    uint64_t next()
    {
        return next_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t peek() const
    {
        return next_.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<uint64_t> next_;
};

// Host-side detector for out-of-order fence completion. Each observed completion
// is reported in the order the host learned about it; a completion below the
// high watermark means a younger submission finished first, i.e. completion order
// no longer matches submission order. With one in-order queue this must never
// happen, which is exactly what the probe asserts.
class CompletionOrder
{
  public:
    // Returns false if this completion is older than one already observed.
    bool observe(uint64_t sequence);
    bool inverted() const
    {
        return inverted_;
    }
    size_t observed() const
    {
        return order_.size();
    }
    uint64_t high_watermark() const
    {
        return high_watermark_;
    }
    const std::vector<uint64_t> &order() const
    {
        return order_;
    }
    void clear();

  private:
    std::vector<uint64_t> order_;
    uint64_t high_watermark_ = 0;
    bool inverted_ = false;
};

} // namespace vkbridge
