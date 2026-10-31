// SPDX-License-Identifier: MIT

#include "test_framework.hpp"
#include "vkbridge/object_table.hpp"

using vkbridge::Error;
using vkbridge::ObjectKind;
using vkbridge::ObjectRecord;
using vkbridge::ObjectTable;

namespace
{

VkBuffer fake_buffer(uintptr_t value)
{
    return reinterpret_cast<VkBuffer>(value);
}

ObjectRecord make_buffer(uint32_t tag, VkDeviceSize size)
{
    ObjectRecord record;
    record.kind = ObjectKind::Buffer;
    record.buffer = fake_buffer(0x1000 + tag);
    record.memory = reinterpret_cast<VkDeviceMemory>(0x2000 + tag);
    record.size = size;
    record.usage = 0x20;
    return record;
}

} // namespace

VKB_TEST(object_table_insert_and_find)
{
    ObjectTable table;
    VKB_CHECK_EQ(table.size(), static_cast<size_t>(0));
    VKB_CHECK_EQ(table.kInvalidHandle, 0u);

    const auto first = table.add(make_buffer(1, 1024));
    const auto second = table.add(make_buffer(2, 2048));
    VKB_CHECK(first.has_value());
    VKB_CHECK(second.has_value());
    VKB_CHECK_EQ(*first, 1u);
    VKB_CHECK_EQ(*second, 2u);
    VKB_CHECK_EQ(table.size(), static_cast<size_t>(2));

    const ObjectRecord *record = table.find(*first);
    VKB_CHECK(record != nullptr);
    VKB_CHECK_EQ(record->handle, 1u);
    VKB_CHECK_EQ(record->size, static_cast<VkDeviceSize>(1024));
    VKB_CHECK(record->buffer == fake_buffer(0x1001));
    VKB_CHECK(record->memory == reinterpret_cast<VkDeviceMemory>(0x2001));

    VKB_CHECK(table.find(999u) == nullptr);
    VKB_CHECK(!table.contains(999u));
    VKB_CHECK(table.contains(2u));
}

VKB_TEST(object_table_rejects_duplicate_and_zero_handles)
{
    ObjectTable table;
    VKB_CHECK(table.add_with_handle(7, make_buffer(1, 4)).has_value());
    const auto duplicate = table.add_with_handle(7, make_buffer(2, 4));
    VKB_CHECK(!duplicate.has_value());
    VKB_CHECK(duplicate.status().code() == Error::DuplicateHandle);

    const auto zero = table.add_with_handle(0, make_buffer(3, 4));
    VKB_CHECK(!zero.has_value());
    VKB_CHECK(zero.status().code() == Error::InvalidArgument);
}

VKB_TEST(object_table_remove_returns_record_and_never_reuses_handles)
{
    ObjectTable table;
    const auto handle = table.add(make_buffer(1, 64));
    VKB_CHECK(handle.has_value());

    const auto removed = table.remove(*handle);
    VKB_CHECK(removed.has_value());
    VKB_CHECK_EQ(removed->size, static_cast<VkDeviceSize>(64));
    VKB_CHECK(table.find(*handle) == nullptr);
    VKB_CHECK_EQ(table.size(), static_cast<size_t>(0));

    const auto missing = table.remove(*handle);
    VKB_CHECK(!missing.has_value());
    VKB_CHECK(missing.status().code() == Error::NotFound);

    // Handles are monotonic, so a stale handle can never alias a new object.
    const auto fresh = table.add(make_buffer(2, 4));
    VKB_CHECK(fresh.has_value());
    VKB_CHECK_EQ(*fresh, 2u);
}

VKB_TEST(object_table_capacity_is_enforced)
{
    ObjectTable table(2);
    VKB_CHECK(table.add(make_buffer(1, 4)).has_value());
    VKB_CHECK(table.add(make_buffer(2, 4)).has_value());
    const auto overflow = table.add(make_buffer(3, 4));
    VKB_CHECK(!overflow.has_value());
    VKB_CHECK(overflow.status().code() == Error::CapacityExceeded);
    VKB_CHECK_EQ(table.size(), static_cast<size_t>(2));
}

VKB_TEST(object_table_take_all_empties_the_table)
{
    ObjectTable table;
    VKB_CHECK(table.add(make_buffer(1, 4)).has_value());
    VKB_CHECK(table.add(make_buffer(2, 8)).has_value());
    const auto records = table.take_all();
    VKB_CHECK_EQ(records.size(), static_cast<size_t>(2));
    VKB_CHECK_EQ(table.size(), static_cast<size_t>(0));

    // New handles continue from where the table left off, not from 1.
    const auto next = table.add(make_buffer(3, 16));
    VKB_CHECK(next.has_value());
    VKB_CHECK_EQ(*next, 3u);
}

VKB_TEST(object_table_keeps_image_records)
{
    ObjectTable table;
    ObjectRecord image;
    image.kind = ObjectKind::Image;
    image.image = reinterpret_cast<VkImage>(0x3000);
    image.size = 320u * 200u * 4u;
    image.usage = 0x13;
    const auto handle = table.add(image);
    VKB_CHECK(handle.has_value());
    const ObjectRecord *found = table.find(*handle);
    VKB_CHECK(found != nullptr);
    VKB_CHECK(found->kind == ObjectKind::Image);
    VKB_CHECK(found->image == reinterpret_cast<VkImage>(0x3000));
    VKB_CHECK_EQ(found->usage, 0x13u);
}
