#include "string.h"

#include <emmintrin.h>

size_t strlen(const char *s)
{
    size_t len = 0;
    while (*s++)
        len++;
    return len;
}

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++))
        ;
    return dest;
}

char *strcat(char *dest, const char *src)
{
    char *d = dest;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dest;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *ret = dst;
    while (n && (*dst++ = *src++))
        n--;
    while (n--)
        *dst++ = '\0';
    return ret;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *ret = dst;
    while (*dst)
        dst++;
    while (n && (*dst++ = *src++))
        n--;
    if (n == 0)
        *dst = '\0';
    return ret;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    if (n == 0)
        return 0;
    while (n-- > 1 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char *strchr(const char *s, int c)
{
    while (*s != (char)c) {
        if (!*s++)
            return NULL;
    }
    return (char *)s;
}

char *strrchr(const char *s, int c)
{
    char *last = NULL;
    do {
        if (*s == (char)c)
            last = (char *)s;
    } while (*s++);
    return last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle)
        return (char *)haystack;
    for (; *haystack; haystack++) {
        if (*haystack != *needle)
            continue;
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n)
            return (char *)haystack;
    }
    return (char *)0;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;

    while (n && ((uintptr_t)d & 15)) {
        *d++ = (uint8_t)c;
        n--;
    }

    if (n >= 16) {
        __m128i v = _mm_set1_epi8((char)c);
        while (n >= 64) {
            _mm_store_si128((__m128i *)(d + 0), v);
            _mm_store_si128((__m128i *)(d + 16), v);
            _mm_store_si128((__m128i *)(d + 32), v);
            _mm_store_si128((__m128i *)(d + 48), v);
            d += 64;
            n -= 64;
        }
        while (n >= 16) {
            _mm_store_si128((__m128i *)d, v);
            d += 16;
            n -= 16;
        }
    }

    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (n && ((uintptr_t)d & 15)) {
        *d++ = *s++;
        n--;
    }

    while (n >= 64) {
        _mm_store_si128((__m128i *)(d + 0), _mm_loadu_si128((const __m128i *)(s + 0)));
        _mm_store_si128((__m128i *)(d + 16), _mm_loadu_si128((const __m128i *)(s + 16)));
        _mm_store_si128((__m128i *)(d + 32), _mm_loadu_si128((const __m128i *)(s + 32)));
        _mm_store_si128((__m128i *)(d + 48), _mm_loadu_si128((const __m128i *)(s + 48)));
        d += 64;
        s += 64;
        n -= 64;
    }

    while (n >= 16) {
        _mm_store_si128((__m128i *)d, _mm_loadu_si128((const __m128i *)s));
        d += 16;
        s += 16;
        n -= 16;
    }

    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0)
        return dst;

    if (d < s) {
        while (n && ((uintptr_t)d & 15)) {
            *d++ = *s++;
            n--;
        }
        while (n >= 64) {
            _mm_store_si128((__m128i *)(d + 0), _mm_loadu_si128((const __m128i *)(s + 0)));
            _mm_store_si128((__m128i *)(d + 16), _mm_loadu_si128((const __m128i *)(s + 16)));
            _mm_store_si128((__m128i *)(d + 32), _mm_loadu_si128((const __m128i *)(s + 32)));
            _mm_store_si128((__m128i *)(d + 48), _mm_loadu_si128((const __m128i *)(s + 48)));
            d += 64;
            s += 64;
            n -= 64;
        }
        while (n >= 16) {
            _mm_store_si128((__m128i *)d, _mm_loadu_si128((const __m128i *)s));
            d += 16;
            s += 16;
            n -= 16;
        }
        while (n--)
            *d++ = *s++;
    } else {
        // Backward copy: align the moving tail, then stream 16B stores downward.
        while (n && ((uintptr_t)(d + n) & 15)) {
            n--;
            d[n] = s[n];
        }
        while (n >= 64) {
            n -= 64;
            _mm_store_si128((__m128i *)(d + n + 48), _mm_loadu_si128((const __m128i *)(s + n + 48)));
            _mm_store_si128((__m128i *)(d + n + 32), _mm_loadu_si128((const __m128i *)(s + n + 32)));
            _mm_store_si128((__m128i *)(d + n + 16), _mm_loadu_si128((const __m128i *)(s + n + 16)));
            _mm_store_si128((__m128i *)(d + n), _mm_loadu_si128((const __m128i *)(s + n)));
        }
        while (n >= 16) {
            n -= 16;
            _mm_store_si128((__m128i *)(d + n), _mm_loadu_si128((const __m128i *)(s + n)));
        }
        while (n > 0) {
            n--;
            d[n] = s[n];
        }
    }
    return dst;
}

int memcmp(const void *s1, const void *s2, size_t n)
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

int itoa(int64_t value, char *buf, int base)
{
    if (!buf || base < 2 || base > 36)
        return 0;

    char *p = buf;
    char *p1;
    int negative = 0;
    uint64_t uvalue;

    if (value < 0 && base == 10) {
        negative = 1;
        // Avoid signed overflow for INT64_MIN.
        uvalue = (uint64_t)(-(value + 1)) + 1u;
    } else {
        uvalue = (uint64_t)value;
    }

    do {
        int digit = (int)(uvalue % (uint64_t)base);
        *p++ = (char)((digit < 10) ? ('0' + digit) : ('a' + digit - 10));
        uvalue /= (uint64_t)base;
    } while (uvalue);
    if (negative)
        *p++ = '-';
    int len = (int)(p - buf);
    *p-- = '\0';
    p1 = buf;
    while (p1 < p) {
        char tmp = *p1;
        *p1++ = *p;
        *p-- = tmp;
    }
    return len;
}

static char *strtok_state = NULL;
char *strtok(char *str, const char *delim)
{
    if (str == NULL)
        str = strtok_state;
    if (str == NULL)
        return NULL;
    while (*str) {
        const char *d = delim;
        while (*d) {
            if (*str == *d)
                break;
            d++;
        }
        if (*d == '\0')
            break;
        str++;
    }
    if (*str == '\0') {
        strtok_state = NULL;
        return NULL;
    }
    char *token_start = str;
    while (*str) {
        const char *d = delim;
        while (*d) {
            if (*str == *d)
                break;
            d++;
        }
        if (*d != '\0') {
            *str = '\0';
            strtok_state = str + 1;
            return token_start;
        }
        str++;
    }
    strtok_state = NULL;
    return token_start;
}
