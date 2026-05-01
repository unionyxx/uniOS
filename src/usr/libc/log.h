#pragma once

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum LogLevel
{
    LOG_LEVEL_TRACE = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
} LogLevel;

void log_vmessage(LogLevel level, const char *scope, const char *fmt, va_list ap);
void log_message(LogLevel level, const char *scope, const char *fmt, ...);

#ifdef __cplusplus
#define LOG_TRACE(scope, fmt, ...) log_message(LOG_LEVEL_TRACE, scope, fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOG_INFO(scope, fmt, ...) log_message(LOG_LEVEL_INFO, scope, fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOG_WARN(scope, fmt, ...) log_message(LOG_LEVEL_WARN, scope, fmt __VA_OPT__(, ) __VA_ARGS__)
#define LOG_ERROR(scope, fmt, ...) log_message(LOG_LEVEL_ERROR, scope, fmt __VA_OPT__(, ) __VA_ARGS__)
#else
#define LOG_TRACE(scope, fmt, ...) log_message(LOG_LEVEL_TRACE, scope, fmt, __VA_ARGS__)
#define LOG_INFO(scope, fmt, ...) log_message(LOG_LEVEL_INFO, scope, fmt, __VA_ARGS__)
#define LOG_WARN(scope, fmt, ...) log_message(LOG_LEVEL_WARN, scope, fmt, __VA_ARGS__)
#define LOG_ERROR(scope, fmt, ...) log_message(LOG_LEVEL_ERROR, scope, fmt, __VA_ARGS__)
#endif

#ifdef __cplusplus
}
#endif
