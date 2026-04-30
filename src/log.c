#include "log.h"

#include <stdarg.h>
#include <stdio.h>

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
    va_list ap;
    va_start(ap, fmt);
    ipman_log_emit("info", msg, fmt, ap);
    va_end(ap);
}

void ipman_log_warn(const char *msg, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ipman_log_emit("warn", msg, fmt, ap);
    va_end(ap);
}

void ipman_log_error(const char *msg, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ipman_log_emit("error", msg, fmt, ap);
    va_end(ap);
}
