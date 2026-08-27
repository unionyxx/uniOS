#pragma once
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "unistd.h"

// Safe dynamic array for freestanding userspace.
//
// T must be trivially copyable: growth and element moves are done with
// realloc/memmove, which is what every current use (row records, handles,
// scalars) wants anyway and keeps the container allocation-free on the hot
// path. Indexed access is bounds-checked in debug builds and terminates the
// process with a diagnostic instead of corrupting memory; release builds keep
// unchecked operator[] for speed and expose at() for callers that want a
// checked lookup without aborting.
template <typename T>
class Vec
{
    static_assert(__is_trivially_copyable(T), "Vec<T> requires a trivially copyable T");

    T *m_data = nullptr;
    size_t m_len = 0;
    size_t m_cap = 0;

    static void bounds_fail(size_t index, size_t len)
    {
        LOG_ERROR("vec", "index %zu out of range (size %zu); terminating", index, len);
        exit(124);
    }

public:
    Vec() = default;

    ~Vec()
    {
        free(m_data);
    }

    Vec(const Vec &) = delete;
    Vec &operator=(const Vec &) = delete;

    Vec(Vec &&other) noexcept : m_data(other.m_data), m_len(other.m_len), m_cap(other.m_cap)
    {
        other.m_data = nullptr;
        other.m_len = 0;
        other.m_cap = 0;
    }

    Vec &operator=(Vec &&other) noexcept
    {
        if (this != &other) {
            free(m_data);
            m_data = other.m_data;
            m_len = other.m_len;
            m_cap = other.m_cap;
            other.m_data = nullptr;
            other.m_len = 0;
            other.m_cap = 0;
        }
        return *this;
    }

    size_t size() const
    {
        return m_len;
    }
    bool empty() const
    {
        return m_len == 0;
    }
    size_t capacity() const
    {
        return m_cap;
    }
    T *data()
    {
        return m_data;
    }
    const T *data() const
    {
        return m_data;
    }

    // Grows capacity to at least new_cap. Returns false on allocation
    // failure; the vector is left unchanged.
    bool reserve(size_t new_cap)
    {
        if (new_cap <= m_cap)
            return true;
        size_t grown = m_cap ? m_cap : 4;
        while (grown < new_cap) {
            if (grown > (SIZE_MAX / 2)) {
                grown = new_cap;
                break;
            }
            grown *= 2;
        }
        if (grown < new_cap)
            grown = new_cap;
        T *next = static_cast<T *>(realloc(m_data, grown * sizeof(T)));
        if (!next)
            return false;
        m_data = next;
        m_cap = grown;
        return true;
    }

    // Resizes to new_len; new elements are value-initialized (zeroed).
    bool resize(size_t new_len)
    {
        if (new_len > m_len && !reserve(new_len))
            return false;
        if (new_len > m_len)
            memset(m_data + m_len, 0, (new_len - m_len) * sizeof(T));
        m_len = new_len;
        return true;
    }

    bool push(const T &value)
    {
        if (m_len >= m_cap && !reserve(m_len + 1))
            return false;
        m_data[m_len++] = value;
        return true;
    }

    void pop()
    {
        if (m_len > 0)
            m_len--;
    }

    void clear()
    {
        m_len = 0;
    }

    // Inserts value before position (0..size). Returns false if position is
    // out of range or the grow fails.
    bool insert(size_t position, const T &value)
    {
        if (position > m_len)
            return false;
        if (m_len >= m_cap && !reserve(m_len + 1))
            return false;
        memmove(m_data + position + 1, m_data + position, (m_len - position) * sizeof(T));
        m_data[position] = value;
        m_len++;
        return true;
    }

    // Removes the element at position, shifting the tail down.
    bool remove(size_t position)
    {
        if (position >= m_len)
            return false;
        memmove(m_data + position, m_data + position + 1, (m_len - position - 1) * sizeof(T));
        m_len--;
        return true;
    }

    // Unchecked hot-path access; bounds-checked in debug builds.
    T &operator[](size_t index)
    {
#ifdef DEBUG
        if (index >= m_len)
            bounds_fail(index, m_len);
#endif
        return m_data[index];
    }
    const T &operator[](size_t index) const
    {
#ifdef DEBUG
        if (index >= m_len)
            bounds_fail(index, m_len);
#endif
        return m_data[index];
    }

    // Checked access; nullptr when out of range.
    T *at(size_t index)
    {
        return index < m_len ? &m_data[index] : nullptr;
    }
    const T *at(size_t index) const
    {
        return index < m_len ? &m_data[index] : nullptr;
    }

    T *begin()
    {
        return m_data;
    }
    T *end()
    {
        return m_data + m_len;
    }
    const T *begin() const
    {
        return m_data;
    }
    const T *end() const
    {
        return m_data + m_len;
    }
};
