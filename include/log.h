#ifndef PARODUS2RBUS_LOG_H
#define PARODUS2RBUS_LOG_H

#include <stdio.h>
#include <stdarg.h>

/* Simple log levels */
#define P2R_LEVEL_ERROR 0
#define P2R_LEVEL_WARN  1
#define P2R_LEVEL_INFO  2
#define P2R_LEVEL_DEBUG 3

extern int g_p2r_log_level;

/* Portable variadic logging without GNU empty __VA_ARGS__ extension.
 * Approach: always require at least a format string; callers pass "" if none.
 */
static inline void p2r_log_internal(int level, const char* file, int line, const char* prefix, const char* fmt, ...){
    if(level > g_p2r_log_level) return;
    fprintf(stderr, "[parodus2rbus] %s:%d: %s", file, line, prefix);
    if(fmt && *fmt){
        va_list ap; va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
}

/* Portable macro wrapper: require at least an empty format string "". */
/* Two-tier macros: base with explicit args, helpers to detect at least one variadic token. */
/* Simpler solution: provide two forms per level: with and without extra args. */
#define LOGE0(msg)        p2r_log_internal(P2R_LEVEL_ERROR, __FILE__, __LINE__, "ERROR: ", msg)
#define LOGW0(msg)        p2r_log_internal(P2R_LEVEL_WARN,  __FILE__, __LINE__, "WARN:  ", msg)
#define LOGI0(msg)        p2r_log_internal(P2R_LEVEL_INFO,  __FILE__, __LINE__, "INFO:  ", msg)
#define LOGD0(msg)        p2r_log_internal(P2R_LEVEL_DEBUG, __FILE__, __LINE__, "DEBUG: ", msg)

#define LOGE(fmt, ...)    p2r_log_internal(P2R_LEVEL_ERROR, __FILE__, __LINE__, "ERROR: ", fmt, __VA_ARGS__)
#define LOGW(fmt, ...)    p2r_log_internal(P2R_LEVEL_WARN,  __FILE__, __LINE__, "WARN:  ", fmt, __VA_ARGS__)
#define LOGI(fmt, ...)    p2r_log_internal(P2R_LEVEL_INFO,  __FILE__, __LINE__, "INFO:  ", fmt, __VA_ARGS__)
#define LOGD(fmt, ...)    p2r_log_internal(P2R_LEVEL_DEBUG, __FILE__, __LINE__, "DEBUG: ", fmt, __VA_ARGS__)

#endif
