#pragma once

enum class Error
{
    None = 0,
    InvalidParam = 1,
    NoMemory = 2,
    NotFound = 3,
    Exists = 4,
    PermissionDenied = 5,
    NotSupported = 6,
    IOError = 7,
    BufferTooSmall = 8,
    Unknown = 99
};

template <typename T>
struct Result
{
    T value;
    Error error;

    Result(T v) : value(v), error(Error::None)
    {
    }
    Result(Error e) : value{}, error(e)
    {
    }

    bool ok() const
    {
        return error == Error::None;
    }
    Error err() const
    {
        return error;
    }

    T &operator*()
    {
        return value;
    }
    T *operator->()
    {
        return &value;
    }
};

// Specialization for void results
template <>
struct Result<void>
{
    Error error;

    Result() : error(Error::None)
    {
    }
    Result(Error e) : error(e)
    {
    }

    bool ok() const
    {
        return error == Error::None;
    }
    Error err() const
    {
        return error;
    }

    static Result success()
    {
        return Result();
    }
};
