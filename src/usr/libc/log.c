#include "log.h"

#include <stdarg.h>
#include <stddef.h>

#include "stdio.h"
#include "string.h"
#include "unistd.h"

static const char *level_mark(LogLevel level)
{
    switch (level) {
        case LOG_LEVEL_TRACE:
            return "[.]";
        case LOG_LEVEL_INFO:
            return "[i]";
        case LOG_LEVEL_WARN:
            return "[!]";
        case LOG_LEVEL_ERROR:
            return "[-]";
        default:
            return "[i]";
    }
}

void log_vmessage(LogLevel level, const char *scope, const char *fmt, va_list ap)
{
    char message[768];
    int message_len = vsnprintf(message, sizeof(message), fmt, ap);
    if (message_len < 0)
        return;

    const char *resolved_scope = (scope && scope[0] != '\0') ? scope : "app";
    uint64_t ticks = get_ticks();
    uint64_t seconds = ticks / 1000;
    uint64_t millis = ticks % 1000;

    char line[896];
    int line_len = snprintf(line, sizeof(line), "[%llu.%03llu] %-4s %s %s",
                            (unsigned long long)seconds, (unsigned long long)millis, resolved_scope,
                            level_mark(level), message);
    if (line_len <= 0)
        return;

    size_t write_len = (size_t)line_len;
    if (write_len >= sizeof(line))
        write_len = sizeof(line) - 1;
    if (write_len == 0)
        return;

    if (line[write_len - 1] != '\n' && write_len + 1 < sizeof(line)) {
        line[write_len++] = '\n';
        line[write_len] = '\0';
    }

    write(1, line, write_len);
}

void log_message(LogLevel level, const char *scope, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vmessage(level, scope, fmt, ap);
    va_end(ap);
}
