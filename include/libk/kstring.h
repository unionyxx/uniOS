#pragma once
#include <kernel/cpu.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Kernel String Utilities
// ============================================================================
// Shared string functions to avoid duplication across kernel modules.
// These are minimal implementations suitable for kernel use.
// ============================================================================

namespace kstring {

class string_view
{
    const char *ptr_;
    size_t len_;

public:
    constexpr string_view() : ptr_(nullptr), len_(0)
    {
    }
    constexpr string_view(const char *s) : ptr_(s), len_(0)
    {
        if (s) {
            while (s[len_])
                len_++;
        }
    }
    constexpr string_view(const char *s, size_t len) : ptr_(s), len_(len)
    {
    }

    [[nodiscard]] constexpr const char *data() const noexcept
    {
        return ptr_;
    }
    [[nodiscard]] constexpr size_t size() const noexcept
    {
        return len_;
    }
    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return len_ == 0;
    }

    [[nodiscard]] constexpr char operator[](size_t i) const
    {
        return ptr_[i];
    }

    [[nodiscard]] bool operator==(string_view other) const noexcept
    {
        if (len_ != other.len_)
            return false;
        for (size_t i = 0; i < len_; i++) {
            if (ptr_[i] != other.ptr_[i])
                return false;
        }
        return true;
    }

    [[nodiscard]] bool starts_with(string_view other) const noexcept
    {
        if (len_ < other.len_)
            return false;
        for (size_t i = 0; i < other.len_; i++) {
            if (ptr_[i] != other.ptr_[i])
                return false;
        }
        return true;
    }
};

// String comparison (returns 0 if equal)
inline int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *static_cast<const unsigned char *>(static_cast<const void *>(s1)) - *static_cast<const unsigned char *>(static_cast<const void *>(s2));
}

// String comparison with length limit
inline int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0)
        return 0;
    return *static_cast<const unsigned char *>(static_cast<const void *>(s1)) - *static_cast<const unsigned char *>(static_cast<const void *>(s2));
}

// String length
inline size_t strlen(const char *s)
{
    size_t len = 0;
    while (*s++)
        len++;
    return len;
}

// String copy with length limit
inline char *strncpy(char *dst, const char *src, size_t n)
{
    char *ret = dst;
    while (n && (*dst = *src)) {
        dst++;
        src++;
        n--;
    }
    while (n--)
        *dst++ = '\0';
    return ret;
}

// String concatenation with length limit
inline char *strncat(char *dst, const char *src, size_t n)
{
    char *ret = dst;
    while (*dst)
        dst++;
    while (n && (*dst = *src)) {
        dst++;
        src++;
        n--;
    }
    if (n == 0)
        *dst = '\0';
    return ret;
}

// String character search
inline char *strchr(const char *s, int c)
{
    while (*s != (char)c) {
        if (!*s++)
            return nullptr;
    }
    return (char *)s;
}

// String character search (last occurrence)
inline char *strrchr(const char *s, int c)
{
    char *last = nullptr;
    do {
        if (*s == (char)c)
            last = (char *)s;
    } while (*s++);
    return last;
}

// Substring search
inline char *strstr(const char *haystack, const char *needle)
{
    if (!*needle)
        return (char *)haystack;
    for (; *haystack; haystack++) {
        if (*haystack != *needle)
            continue;
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n)
            return (char *)haystack;
    }
    return nullptr;
}

// Memory set
inline void *memset(void *dst, int c, size_t n)
{
    if (n >= 64 && g_cpu_features.has_erms) {
        asm volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(c) : "memory");
        return dst;
    }

    uint8_t *d = (uint8_t *)dst;
    uint64_t val8 = (uint8_t)c;
    uint64_t val64 =
        (val8 << 56) | (val8 << 48) | (val8 << 40) | (val8 << 32) | (val8 << 24) | (val8 << 16) | (val8 << 8) | val8;

    // Align to 8 bytes
    while (n && ((uintptr_t)d & 7)) {
        *d++ = (uint8_t)c;
        n--;
    }

    // Write 64-bit chunks
    while (n >= 8) {
        *(uint64_t *)d = val64;
        d += 8;
        n -= 8;
    }

    // Remaining bytes
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

// Memory copy
inline void *memcpy(void *dst, const void *src, size_t n)
{
    if (n >= 64 && g_cpu_features.has_erms) {
        asm volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
        return dst;
    }

    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    // If both pointers have the same alignment relative to 8, we can use 64-bit copies
    if (((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
        while (n && ((uintptr_t)d & 7)) {
            *d++ = *s++;
            n--;
        }
        while (n >= 8) {
            *(uint64_t *)d = *(const uint64_t *)s;
            d += 8;
            s += 8;
            n -= 8;
        }
    }

    // Remaining bytes (or if alignments didn't match)
    while (n--)
        *d++ = *s++;
    return dst;
}

// Memory compare
inline int memcmp(const void *s1, const void *s2, size_t n)
{
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    while (n--) {
        if (*p1 != *p2)
            return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}

// Integer to string (decimal)
inline int itoa(int64_t value, char *buf, int base = 10)
{
    char *p = buf;
    char *p1;
    bool negative = false;

    if (value < 0 && base == 10) {
        negative = true;
        value = -value;
    }

    uint64_t uvalue = (uint64_t)value;

    do {
        int digit = (int)(uvalue % (uint64_t)base);
        *p++ = (char)((digit < 10) ? ('0' + digit) : ('a' + digit - 10));
        uvalue /= (uint64_t)base;
    } while (uvalue);

    if (negative)
        *p++ = '-';

    int len = (int)(p - buf);
    *p-- = '\0';

    // Reverse
    p1 = buf;
    while (p1 < p) {
        char tmp = *p1;
        *p1++ = *p;
        *p-- = tmp;
    }

    return len;
}

// Format size in human-readable form (e.g. 1.2 KB)
inline void format_size(size_t size, char *buf)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double s = (double)size;
    while (s >= 1024 && unit < 4) {
        s /= 1024;
        unit++;
    }

    if (unit == 0) {
        itoa((int64_t)size, buf);
        char *p = buf;
        while (*p)
            p++;
        *p++ = ' ';
        *p++ = 'B';
        *p = '\0';
    } else {
        // Simple decimal formatting (1 decimal place)
        int64_t whole = (int64_t)s;
        int64_t frac = (int64_t)((s - (double)whole) * 10.0);
        itoa(whole, buf);
        char *p = buf;
        while (*p)
            p++;
        *p++ = '.';
        itoa(frac, p);
        while (*p)
            p++;
        *p++ = ' ';
        const char *u = units[unit];
        while (*u)
            *p++ = *u++;
        *p = '\0';
    }
}

// Memory move (safe for overlapping regions)
inline void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d < s) {
        if (n >= 64 && g_cpu_features.has_erms) {
            asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
            return dst;
        }
        // Copy forward - same as memcpy
        if (((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
            while (n && ((uintptr_t)d & 7)) {
                *d++ = *s++;
                n--;
            }
            while (n >= 8) {
                *(uint64_t *)d = *(const uint64_t *)s;
                d += 8;
                s += 8;
                n -= 8;
            }
        }
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        if (n >= 64 && g_cpu_features.has_erms) {
            d += n - 1;
            s += n - 1;
            asm volatile("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
            return dst;
        }
        // Copy backward
        d += n;
        s += n;
        if (((uintptr_t)d & 7) == ((uintptr_t)s & 7)) {
            while (n && ((uintptr_t)d & 7)) {
                n--;
                *--d = *--s;
            }
            while (n >= 8) {
                n -= 8;
                d -= 8;
                s -= 8;
                *(uint64_t *)d = *(const uint64_t *)s;
            }
        }
        while (n--)
            *--d = *--s;
    }
    return dst;
}

// Convenience aliases for kernel code
// These provide semantically clear names and avoid need for local definitions

// Zero memory region (equivalent to memset(ptr, 0, size))
inline void zero_memory(void *ptr, size_t size)
{
    memset(ptr, 0, size);
}

// Copy memory (equivalent to memcpy, wrapper for clarity)
inline void copy_memory(void *dst, const void *src, size_t size)
{
    memcpy(dst, src, size);
}

} // namespace kstring
