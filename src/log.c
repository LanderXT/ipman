/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    LEVEL_DEBUG = 0,
    LEVEL_INFO  = 1,
    LEVEL_WARN  = 2,
    LEVEL_ERROR = 3,
} ipman_log_level_t;

static ipman_log_level_t g_min_level = LEVEL_WARN;

void ipman_log_init(void) {
    g_min_level = LEVEL_WARN;
    const char *v = getenv("IPMAN_LOG");
    if (v == NULL || *v == '\0') return;
    if      (strcmp(v, "debug") == 0) g_min_level = LEVEL_DEBUG;
    else if (strcmp(v, "info")  == 0) g_min_level = LEVEL_INFO;
    else if (strcmp(v, "warn")  == 0) g_min_level = LEVEL_WARN;
    else if (strcmp(v, "error") == 0) g_min_level = LEVEL_ERROR;
    /* Unknown values: keep default WARN (already reset above). */
}

static void write_escaped(const char *s) {
    fputc('"', stderr);
    for (; *s != '\0'; ++s) {
        switch (*s) {
        case '"':  fputs("\\\"", stderr); break;
        case '\\': fputs("\\\\", stderr); break;
        case '\n': fputs("\\n", stderr);  break;
        case '\r': fputs("\\r", stderr);  break;
        case '\t': fputs("\\t", stderr);  break;
        default:   fputc(*s, stderr);     break;
        }
    }
    fputc('"', stderr);
}

static void ipman_log_emit(const char *level, const char *msg, const char *fmt, va_list ap) {
    fputs("level=", stderr);
    fputs(level, stderr);
    fputs(" msg=", stderr);
    write_escaped(msg);
    if (fmt != NULL && fmt[0] != '\0') {
        fputc(' ', stderr);
        vfprintf(stderr, fmt, ap);
    }
    fputc('\n', stderr);
}

void ipman_log_info(const char *msg, const char *fmt, ...) {
    if (g_min_level > LEVEL_INFO) return;
    va_list ap;
    va_start(ap, fmt);
    ipman_log_emit("info", msg, fmt, ap);
    va_end(ap);
}

void ipman_log_warn(const char *msg, const char *fmt, ...) {
    if (g_min_level > LEVEL_WARN) return;
    va_list ap;
    va_start(ap, fmt);
    ipman_log_emit("warn", msg, fmt, ap);
    va_end(ap);
}

void ipman_log_error(const char *msg, const char *fmt, ...) {
    if (g_min_level > LEVEL_ERROR) return;
    va_list ap;
    va_start(ap, fmt);
    ipman_log_emit("error", msg, fmt, ap);
    va_end(ap);
}
