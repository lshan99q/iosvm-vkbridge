// SPDX-License-Identifier: MIT

#include "vkbridge/status.hpp"

namespace vkbridge
{

const char *error_name(Error error)
{
    switch (error) {
    case Error::Ok:
        return "ok";
    case Error::InvalidArgument:
        return "invalid argument";
    case Error::NotFound:
        return "not found";
    case Error::DuplicateHandle:
        return "duplicate handle";
    case Error::HandleExhausted:
        return "handle space exhausted";
    case Error::CapacityExceeded:
        return "capacity exceeded";
    case Error::SizeTooLarge:
        return "size too large";
    case Error::Misaligned:
        return "misaligned offset or size";
    case Error::Unsupported:
        return "unsupported";
    case Error::NoDevice:
        return "no usable Vulkan device";
    case Error::DeviceCreateFailed:
        return "device creation failed";
    case Error::OutOfMemory:
        return "out of memory";
    case Error::VulkanError:
        return "Vulkan call failed";
    case Error::Timeout:
        return "timeout";
    case Error::IoError:
        return "I/O error";
    case Error::Internal:
        return "internal error";
    }
    return "unknown error";
}

std::string Status::to_string() const
{
    if (detail_.empty()) {
        return error_name(code_);
    }
    if (code_ == Error::Ok) {
        return detail_;
    }
    return std::string(error_name(code_)) + ": " + detail_;
}

Status status_ok()
{
    return Status();
}

Status status_fail(Error code, std::string detail)
{
    return Status(code, std::move(detail));
}

} // namespace vkbridge
