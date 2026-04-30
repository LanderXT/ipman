#ifndef IPMAN_LOG_H
#define IPMAN_LOG_H

/*
 * Structured stderr logger. Output format: one line per event, tokens of
 * the form key=value separated by spaces. First token is always level=...;
 * second is msg=... (quoted, single-line, with escaping). Additional tokens
 * are caller-supplied key=value pairs via the printf-style fmt argument.
 *
 * The `msg` parameter is treated as a literal string (never as a format
 * string) and is written with double-quote escaping for \, ", and control
 * characters (\n, \r, \t). The `fmt` parameter is a printf format for the
 * trailing key=value pairs; all current call sites use hardcoded format
 * strings with %s, %d, etc.
 *
 * No timestamp is emitted — the shell/systemd journal adds one.
 */

void ipman_log_info(const char *msg, const char *fmt, ...);
void ipman_log_warn(const char *msg, const char *fmt, ...);
void ipman_log_error(const char *msg, const char *fmt, ...);

#endif
