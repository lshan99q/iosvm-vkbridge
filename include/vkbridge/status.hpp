// SPDX-License-Identifier: MIT
#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <utility>

namespace vkbridge
{
// Coarse error domain for the host backend. The hot replay path never throws:
// every fallible operation returns a Status or a Result<T>.
enum class Error {
    Ok = 0,
    InvalidArgument,
    NotFound,
    DuplicateHandle,
    HandleExhausted,
    CapacityExceeded,
    SizeTooLarge,
    Misaligned,
    Unsupported,
    NoDevice,
    DeviceCreateFailed,
    OutOfMemory,
    VulkanError,
    Timeout,
    IoError,
    Internal,
};

const char *error_name(Error error);

// A failure value: an error code plus an optional human-readable detail string.
// It is only ever constructed empty for the success case, so allocating a string
// on failure is acceptable.
class Status
{
  public:
    Status() = default;
    explicit Status(Error code) : code_(code) {}
    Status(Error code, std::string detail) : code_(code), detail_(std::move(detail)) {}

    bool ok() const
    {
        return code_ == Error::Ok;
    }
    explicit operator bool() const
    {
        return ok();
    }

    Error code() const
    {
        return code_;
    }
    const std::string &detail() const
    {
        return detail_;
    }
    std::string to_string() const;

  private:
    Error code_ = Error::Ok;
    std::string detail_;
};

Status status_ok();
Status status_fail(Error code, std::string detail = {});

// std::expected-style result without C++23 and without exceptions.
template <typename T> class Result
{
  public:
    Result(T &&value) : value_(std::move(value)) {}
    Result(const T &value) : value_(value) {}
    Result(Status status) : status_(std::move(status))
    {
        assert(!status_.ok() && "successful Status cannot carry a value");
    }

    Result(const Result &) = default;
    Result(Result &&) noexcept = default;
    Result &operator=(const Result &) = default;
    Result &operator=(Result &&) noexcept = default;

    bool has_value() const
    {
        return value_.has_value();
    }
    explicit operator bool() const
    {
        return has_value();
    }

    const Status &status() const
    {
        return status_;
    }

    T &value()
    {
        assert(value_.has_value());
        return *value_;
    }
    const T &value() const
    {
        assert(value_.has_value());
        return *value_;
    }
    T &operator*()
    {
        return value();
    }
    const T &operator*() const
    {
        return value();
    }
    T *operator->()
    {
        return &value();
    }
    const T *operator->() const
    {
        return &value();
    }

  private:
    std::optional<T> value_;
    Status status_;
};

template <typename T> Result<T> ok_result(T value)
{
    return Result<T>(std::move(value));
}

template <typename T> Result<T> err_result(Error code, std::string detail = {})
{
    return Result<T>(Status(code, std::move(detail)));
}

template <typename T> Result<T> err_result(const Status &status)
{
    return Result<T>(status);
}

} // namespace vkbridge
