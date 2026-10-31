// SPDX-License-Identifier: MIT

#include "test_framework.hpp"
#include "vkbridge/sequence.hpp"

using vkbridge::CompletionOrder;
using vkbridge::SequenceSource;

VKB_TEST(sequence_source_is_monotonic)
{
    SequenceSource source;
    VKB_CHECK_EQ(source.peek(), static_cast<uint64_t>(1));
    VKB_CHECK_EQ(source.next(), static_cast<uint64_t>(1));
    VKB_CHECK_EQ(source.next(), static_cast<uint64_t>(2));
    VKB_CHECK_EQ(source.next(), static_cast<uint64_t>(3));
    VKB_CHECK_EQ(source.peek(), static_cast<uint64_t>(4));

    SequenceSource offset(1000);
    VKB_CHECK_EQ(offset.next(), static_cast<uint64_t>(1000));
    VKB_CHECK_EQ(offset.next(), static_cast<uint64_t>(1001));
}

VKB_TEST(completion_order_accepts_in_order_completions)
{
    CompletionOrder order;
    VKB_CHECK(order.observe(10));
    VKB_CHECK(order.observe(11));
    VKB_CHECK(order.observe(12));
    VKB_CHECK(!order.inverted());
    VKB_CHECK_EQ(order.high_watermark(), static_cast<uint64_t>(12));
    VKB_CHECK_EQ(order.observed(), static_cast<size_t>(3));
    VKB_CHECK_EQ(order.order().size(), static_cast<size_t>(3));
}

VKB_TEST(completion_order_detects_inversions)
{
    CompletionOrder order;
    VKB_CHECK(order.observe(5));
    VKB_CHECK(!order.observe(4));
    VKB_CHECK(order.inverted());
    VKB_CHECK_EQ(order.high_watermark(), static_cast<uint64_t>(5));
    VKB_CHECK_EQ(order.observed(), static_cast<size_t>(2));

    order.clear();
    VKB_CHECK(!order.inverted());
    VKB_CHECK_EQ(order.high_watermark(), static_cast<uint64_t>(0));
    VKB_CHECK_EQ(order.observed(), static_cast<size_t>(0));
}

VKB_TEST(completion_order_equal_sequence_is_not_an_inversion)
{
    CompletionOrder order;
    VKB_CHECK(order.observe(3));
    VKB_CHECK(order.observe(3));
    VKB_CHECK(!order.inverted());
    VKB_CHECK_EQ(order.high_watermark(), static_cast<uint64_t>(3));
}
