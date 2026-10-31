// SPDX-License-Identifier: MIT

#include "vkbridge/object_table.hpp"

#include <utility>

namespace vkbridge
{

ObjectTable::ObjectTable(size_t max_objects) : max_objects_(max_objects == 0 ? 1 : max_objects) {}

Result<uint32_t> ObjectTable::insert(uint32_t handle, const ObjectRecord &record)
{
    if (handle == kInvalidHandle) {
        return err_result<uint32_t>(Error::InvalidArgument, "handle 0 is reserved");
    }
    if (objects_.size() >= max_objects_) {
        return err_result<uint32_t>(Error::CapacityExceeded, "object table is full");
    }
    if (objects_.find(handle) != objects_.end()) {
        return err_result<uint32_t>(Error::DuplicateHandle, "handle already in use");
    }
    ObjectRecord stored = record;
    stored.handle = handle;
    objects_.emplace(handle, stored);
    return ok_result<uint32_t>(handle);
}

Result<uint32_t> ObjectTable::add(const ObjectRecord &record)
{
    if (objects_.size() >= max_objects_) {
        return err_result<uint32_t>(Error::CapacityExceeded, "object table is full");
    }
    if (next_handle_ == kInvalidHandle) {
        return err_result<uint32_t>(Error::HandleExhausted, "handle counter wrapped");
    }
    const uint32_t handle = next_handle_++;
    return insert(handle, record);
}

Result<uint32_t> ObjectTable::add_with_handle(uint32_t handle, const ObjectRecord &record)
{
    return insert(handle, record);
}

const ObjectRecord *ObjectTable::find(uint32_t handle) const
{
    const auto it = objects_.find(handle);
    return it == objects_.end() ? nullptr : &it->second;
}

ObjectRecord *ObjectTable::find(uint32_t handle)
{
    const auto it = objects_.find(handle);
    return it == objects_.end() ? nullptr : &it->second;
}

bool ObjectTable::contains(uint32_t handle) const
{
    return objects_.find(handle) != objects_.end();
}

Result<ObjectRecord> ObjectTable::remove(uint32_t handle)
{
    const auto it = objects_.find(handle);
    if (it == objects_.end()) {
        return err_result<ObjectRecord>(Error::NotFound, "unknown object handle");
    }
    ObjectRecord record = it->second;
    objects_.erase(it);
    return ok_result<ObjectRecord>(std::move(record));
}

std::vector<ObjectRecord> ObjectTable::take_all()
{
    std::vector<ObjectRecord> records;
    records.reserve(objects_.size());
    for (auto &entry : objects_) {
        records.push_back(std::move(entry.second));
    }
    objects_.clear();
    return records;
}

} // namespace vkbridge
