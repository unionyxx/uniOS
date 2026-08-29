#include "stdio.h"

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include "string.h"

int putchar(int c)
{
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int puts(const char *s)
{
    size_t len = strlen(s);
    write(1, s, len);
    putchar('\n');
    return 0;
}

static void itoa_internal(uint64_t n, char *buf, int *ti, int base, bool uppercase)
{
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    if (n == 0) {
        buf[(*ti)++] = '0';
        return;
    }
    while (n > 0) {
        buf[(*ti)++] = digits[n % (uint64_t)base];
        n /= (uint64_t)base;
    }
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
    if (size == 0)
        return 0;
    int written = 0;
    const char *f = format;
    while (*f && (size_t)written < size - 1) {
        if (*f == '%') {
            f++;
            if (*f == '%') {
                str[written++] = '%';
                f++;
                continue;
            }

            bool left_justify = false;
            if (*f == '-') {
                left_justify = true;
                f++;
            }

            char pad_char = ' ';
            if (*f == '0') {
                pad_char = '0';
                f++;
            }

            int width = 0;
            while (*f >= '0' && *f <= '9') {
                width = width * 10 + (*f - '0');
                f++;
            }

            int length_mod = 0;
            while (*f == 'l') {
                length_mod++;
                f++;
            }

            if (*f == 's') {
                const char *s = va_arg(ap, const char *);
                if (!s)
                    s = "(null)";
                int len = (int)strlen(s);
                if (!left_justify) {
                    while (width > len && (size_t)written < size - 1) {
                        str[written++] = ' ';
                        width--;
                    }
                }
                while (*s && (size_t)written < size - 1) {
                    str[written++] = *s++;
                }
                if (left_justify) {
                    while (width > len && (size_t)written < size - 1) {
                        str[written++] = ' ';
                        width--;
                    }
                }
                f++;
            } else if (*f == 'd' || *f == 'i') {
                int64_t n;
                if (length_mod >= 2)
                    n = va_arg(ap, int64_t);
                else if (length_mod == 1)
                    n = va_arg(ap, long);
                else
                    n = (int64_t)va_arg(ap, int);

                bool negative = n < 0;
                uint64_t un = negative ? (uint64_t)-(n + 1) + 1u : (uint64_t)n;

                char tmp[64];
                int ti = 0;
                itoa_internal(un, tmp, &ti, 10, false);

                int content = ti + (negative ? 1 : 0);
                int pad_count = width > content ? width - content : 0;
                if (!left_justify) {
                    if (pad_char == '0') {
                        if (negative && (size_t)written < size - 1)
                            str[written++] = '-';
                        while (pad_count > 0 && (size_t)written < size - 1) {
                            str[written++] = '0';
                            pad_count--;
                        }
                    } else {
                        while (pad_count > 0 && (size_t)written < size - 1) {
                            str[written++] = ' ';
                            pad_count--;
                        }
                        if (negative && (size_t)written < size - 1)
                            str[written++] = '-';
                    }
                } else {
                    if (negative && (size_t)written < size - 1)
                        str[written++] = '-';
                }
                while (ti > 0 && (size_t)written < size - 1) {
                    str[written++] = tmp[--ti];
                }
                if (left_justify) {
                    while (pad_count > 0 && (size_t)written < size - 1) {
                        str[written++] = ' ';
                        pad_count--;
                    }
                }
                f++;
            } else if (*f == 'u' || *f == 'o' || *f == 'x' || *f == 'X') {
                uint64_t n;
                if (length_mod >= 2)
                    n = va_arg(ap, uint64_t);
                else if (length_mod == 1)
                    n = va_arg(ap, unsigned long);
                else
                    n = (uint64_t)va_arg(ap, unsigned int);

                int base = (*f == 'o') ? 8 : ((*f == 'x' || *f == 'X') ? 16 : 10);
                char tmp[64];
                int ti = 0;
                itoa_internal(n, tmp, &ti, base, (*f == 'X'));

                int pad_count = width > ti ? width - ti : 0;
                if (!left_justify) {
                    while (pad_count > 0 && (size_t)written < size - 1) {
                        str[written++] = pad_char;
                        pad_count--;
                    }
                }
                while (ti > 0 && (size_t)written < size - 1) {
                    str[written++] = tmp[--ti];
                }
                if (left_justify) {
                    while (pad_count > 0 && (size_t)written < size - 1) {
                        str[written++] = ' ';
                        pad_count--;
                    }
                }
                f++;
            } else if (*f == 'p') {
                uint64_t n = (uint64_t)va_arg(ap, void *);
                if ((size_t)written < size - 1)
                    str[written++] = '0';
                if ((size_t)written < size - 1)
                    str[written++] = 'x';
                char tmp[16];
                int ti = 0;
                itoa_internal(n, tmp, &ti, 16, false);
                while (ti < 16)
                    tmp[ti++] = '0';
                while (ti > 0 && (size_t)written < size - 1) {
                    str[written++] = tmp[--ti];
                }
                f++;
            } else if (*f == 'c') {
                str[written++] = (char)va_arg(ap, int);
                f++;
            } else {
                str[written++] = '%';
            }
        } else {
            str[written++] = *f++;
        }
    }
    str[written] = '\0';
    return written;
}

int vsprintf(char *str, const char *format, va_list ap)
{
    return vsnprintf(str, 0x7FFFFFFF, format, ap);
}

int snprintf(char *str, size_t size, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(str, size, format, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *str, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int ret = vsprintf(str, format, ap);
    va_end(ap);
    return ret;
}

int printf(const char *format, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    if (ret > 0)
        write(1, buf, (size_t)ret);
    return ret;
}
