#pragma once
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "unistd.h"

// Safe growable NUL-terminated string for freestanding userspace. The buffer
// always carries a trailing NUL so c_str() is always valid, and indexed
// access is bounds-checked in debug builds (terminating with a diagnostic
// instead of corrupting memory). Growable operations report failure with a
// false return rather than aborting, so callers can react to OOM.
class String
{
    char *m_data = nullptr;
    size_t m_len = 0;
    size_t m_cap = 0; // includes space for the NUL

    static void bounds_fail(size_t index, size_t len)
    {
        LOG_ERROR("string", "index %zu out of range (size %zu); terminating", index, len);
        exit(124);
    }

    bool ensure(size_t needed_cap)
    {
        if (needed_cap <= m_cap)
            return true;
        size_t grown = m_cap ? m_cap : 16;
        while (grown < needed_cap) {
            if (grown > (SIZE_MAX / 2)) {
                grown = needed_cap;
                break;
            }
            grown *= 2;
        }
        if (grown < needed_cap)
            grown = needed_cap;
        char *next = static_cast<char *>(realloc(m_data, grown));
        if (!next)
            return false;
        m_data = next;
        m_cap = grown;
        return true;
    }

public:
    String()
    {
        ensure(1);
        if (m_data)
            m_data[0] = '\0';
    }

    String(const char *s) : String()
    {
        if (s)
            assign(s);
    }

    String(const String &other) : String()
    {
        assign(other);
    }

    String &operator=(const String &other)
    {
        if (this != &other)
            assign(other);
        return *this;
    }

    String(String &&other) noexcept : m_data(other.m_data), m_len(other.m_len), m_cap(other.m_cap)
    {
        other.m_data = nullptr;
        other.m_len = 0;
        other.m_cap = 0;
    }

    String &operator=(String &&other) noexcept
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

    ~String()
    {
        free(m_data);
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

    // Always valid; an empty string returns "".
    const char *c_str() const
    {
        return m_data ? m_data : "";
    }

    bool reserve(size_t new_cap)
    {
        return ensure(new_cap + 1);
    }

    bool assign(const char *s)
    {
        if (!s)
            s = "";
        size_t n = strlen(s);
        if (!ensure(n + 1))
            return false;
        memcpy(m_data, s, n + 1);
        m_len = n;
        return true;
    }

    bool assign(const String &other)
    {
        return assign(other.c_str());
    }

    bool append(char c)
    {
        if (!ensure(m_len + 2))
            return false;
        m_data[m_len++] = c;
        m_data[m_len] = '\0';
        return true;
    }

    bool append(const char *s)
    {
        if (!s)
            return true;
        size_t n = strlen(s);
        if (n == 0)
            return true;
        if (!ensure(m_len + n + 1))
            return false;
        memcpy(m_data + m_len, s, n);
        m_len += n;
        m_data[m_len] = '\0';
        return true;
    }

    bool append(const String &other)
    {
        return append(other.c_str());
    }

    void clear()
    {
        m_len = 0;
        if (m_data)
            m_data[0] = '\0';
    }

    bool equals(const char *s) const
    {
        return strcmp(c_str(), s ? s : "") == 0;
    }
    bool equals(const String &other) const
    {
        return equals(other.c_str());
    }

    // Unchecked hot-path access; bounds-checked in debug builds.
    char &operator[](size_t index)
    {
#ifdef DEBUG
        if (index >= m_len)
            bounds_fail(index, m_len);
#endif
        return m_data[index];
    }
    char operator[](size_t index) const
    {
#ifdef DEBUG
        if (index >= m_len)
            bounds_fail(index, m_len);
#endif
        return m_data[index];
    }

    char *begin()
    {
        return m_data;
    }
    char *end()
    {
        return m_data + m_len;
    }
    const char *begin() const
    {
        return m_data;
    }
    const char *end() const
    {
        return m_data + m_len;
    }
};
