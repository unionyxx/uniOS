#pragma once
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Kernel String Utilities
// ============================================================================
// Shared string functions to avoid duplication across kernel modules.
// These are minimal implementations suitable for kernel use.
// ============================================================================

namespace kstring {

// String comparison (returns 0 if equal)
inline int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

// String comparison with length limit
inline int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

// String length
inline size_t strlen(const char* s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

// String copy (unsafe - caller must ensure dst is large enough)
inline char* strcpy(char* dst, const char* src) {
    char* ret = dst;
    while ((*dst++ = *src++));
    return ret;
}

// String copy with length limit
inline char* strncpy(char* dst, const char* src, size_t n) {
    char* ret = dst;
    while (n && (*dst++ = *src++)) n--;
    while (n--) *dst++ = '\0';
    return ret;
}

// Memory set
inline void* memset(void* dst, int c, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

// Memory copy
inline void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}

// Memory compare
inline int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* p1 = (const uint8_t*)s1;
    const uint8_t* p2 = (const uint8_t*)s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

// Integer to string (decimal)
inline int itoa(int64_t value, char* buf, int base = 10) {
    char* p = buf;
    char* p1;
    bool negative = false;
    
    if (value < 0 && base == 10) {
        negative = true;
        value = -value;
    }
    
    uint64_t uvalue = (uint64_t)value;
    
    do {
        int digit = uvalue % base;
        *p++ = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
        uvalue /= base;
    } while (uvalue);
    
    if (negative) *p++ = '-';
    
    int len = p - buf;
    *p-- = '\0';
    
    // Reverse
    p1 = buf;
    while (p1 < p) {
        char tmp = *p1;
        *p1++ = *p--;
        *p-- = tmp;
    }
    
    return len;
}

// Memory move (safe for overlapping regions)
inline void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    
    if (d < s) {
        // Copy forward
        while (n--) *d++ = *s++;
    } else if (d > s) {
        // Copy backward (prevent overwrite when regions overlap)
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

// Convenience aliases for kernel code
// These provide semantically clear names and avoid need for local definitions

// Zero memory region (equivalent to memset(ptr, 0, size))
inline void zero_memory(void* ptr, size_t size) {
    memset(ptr, 0, size);
}

// Copy memory (equivalent to memcpy, wrapper for clarity)
inline void copy_memory(void* dst, const void* src, size_t size) {
    memcpy(dst, src, size);
}

} // namespace kstring
