// SPDX-License-Identifier: MIT

#include "vkbridge/sequence.hpp"

namespace vkbridge
{

bool CompletionOrder::observe(uint64_t sequence)
{
    order_.push_back(sequence);
    if (sequence < high_watermark_) {
        inverted_ = true;
        return false;
    }
    if (sequence > high_watermark_) {
        high_watermark_ = sequence;
    }
    return true;
}

void CompletionOrder::clear()
{
    order_.clear();
    high_watermark_ = 0;
    inverted_ = false;
}

} // namespace vkbridge
