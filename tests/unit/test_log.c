/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 *
 * Unit tests for src/log.c — the IPMAN_LOG env-var-driven level gate.
 *
 * Each case sets IPMAN_LOG, calls ipman_log_init() to (re)derive the
 * threshold, then captures stderr while invoking info/warn/error and
 * asserts which lines appear. ipman_log_init() is documented as
 * idempotent, so cases reuse the same process.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

static int g_failures = 0;
static const char *g_case = "(none)";

#define FAIL(msg) do { \
    fprintf(stderr, "  FAIL [%s] %s (at %s:%d)\n", \
            g_case, (msg), __FILE__, __LINE__); \
    g_failures++; \
    return; \
} while (0)

#define ASSERT_CONTAINS(haystack, needle) do { \
    if (strstr((haystack), (needle)) == NULL) { \
        fprintf(stderr, "  FAIL [%s] expected '%s' in:\n%s\n", \
                g_case, (needle), (haystack)); \
        g_failures++; \
        return; \
    } \
} while (0)

#define ASSERT_NOT_CONTAINS(haystack, needle) do { \
    if (strstr((haystack), (needle)) != NULL) { \
        fprintf(stderr, "  FAIL [%s] did not expect '%s' in:\n%s\n", \
                g_case, (needle), (haystack)); \
        g_failures++; \
        return; \
    } \
} while (0)

/* Redirect stderr (fd 2) to a temp file and return the saved original fd
 * via *saved. Returns 0 on success, -1 on failure. */
static int capture_stderr_start(int *saved, char *path, size_t pathlen) {
    *saved = dup(STDERR_FILENO);
    if (*saved < 0) return -1;
    snprintf(path, pathlen, "/tmp/ipman-log-test-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) { close(*saved); return -1; }
    fflush(stderr);
    if (dup2(fd, STDERR_FILENO) < 0) { close(fd); close(*saved); return -1; }
    close(fd);
    return 0;
}

/* Restore stderr from saved fd; read the captured file into buf (NUL-
 * terminated). Returns 0 on success. */
static int capture_stderr_stop(int saved, const char *path,
                               char *buf, size_t buflen) {
    fflush(stderr);
    if (dup2(saved, STDERR_FILENO) < 0) { close(saved); return -1; }
    close(saved);
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1;
    size_t n = fread(buf, 1, buflen - 1, f);
    buf[n] = '\0';
    fclose(f);
    unlink(path);
    return 0;
}

/* Issue all three log calls, capture stderr into out. */
static int run_log_calls(char *out, size_t outlen) {
    int saved;
    char path[64];
    if (capture_stderr_start(&saved, path, sizeof path) != 0) return -1;
    ipman_log_info("info-msg",   "k=v");
    ipman_log_warn("warn-msg",   "k=v");
    ipman_log_error("error-msg", "k=v");
    return capture_stderr_stop(saved, path, out, outlen);
}

static void case_default_unset(void) {
    g_case = "default (IPMAN_LOG unset)";
    unsetenv("IPMAN_LOG");
    ipman_log_init();
    char out[4096];
    if (run_log_calls(out, sizeof out) != 0) FAIL("capture failed");
    ASSERT_NOT_CONTAINS(out, "level=info");
    ASSERT_CONTAINS(out, "level=warn");
    ASSERT_CONTAINS(out, "level=error");
}

static void case_default_empty(void) {
    g_case = "empty (IPMAN_LOG=\"\")";
    setenv("IPMAN_LOG", "", 1);
    ipman_log_init();
    char out[4096];
    if (run_log_calls(out, sizeof out) != 0) FAIL("capture failed");
    ASSERT_NOT_CONTAINS(out, "level=info");
    ASSERT_CONTAINS(out, "level=warn");
    ASSERT_CONTAINS(out, "level=error");
}

static void case_unknown_value(void) {
    g_case = "unknown (IPMAN_LOG=verbose)";
    setenv("IPMAN_LOG", "verbose", 1);
    ipman_log_init();
    char out[4096];
    if (run_log_calls(out, sizeof out) != 0) FAIL("capture failed");
    /* Unknown value silently falls back to default WARN. */
    ASSERT_NOT_CONTAINS(out, "level=info");
    ASSERT_CONTAINS(out, "level=warn");
    ASSERT_CONTAINS(out, "level=error");
}

static void case_debug(void) {
    g_case = "IPMAN_LOG=debug (legacy, now unknown)";
    setenv("IPMAN_LOG", "debug", 1);
    ipman_log_init();
    char out[4096];
    if (run_log_calls(out, sizeof out) != 0) FAIL("capture failed");
    /* "debug" is no longer a recognized level. Like any unknown value
     * it falls back to default WARN. Regression test against accidentally
     * re-introducing the legacy alias. */
    ASSERT_NOT_CONTAINS(out, "level=info");
    ASSERT_CONTAINS(out, "level=warn");
    ASSERT_CONTAINS(out, "level=error");
}

static void case_info(void) {
    g_case = "IPMAN_LOG=info";
    setenv("IPMAN_LOG", "info", 1);
    ipman_log_init();
    char out[4096];
    if (run_log_calls(out, sizeof out) != 0) FAIL("capture failed");
    ASSERT_CONTAINS(out, "level=info");
    ASSERT_CONTAINS(out, "level=warn");
    ASSERT_CONTAINS(out, "level=error");
}

static void case_warn(void) {
    g_case = "IPMAN_LOG=warn";
    setenv("IPMAN_LOG", "warn", 1);
    ipman_log_init();
    char out[4096];
    if (run_log_calls(out, sizeof out) != 0) FAIL("capture failed");
    ASSERT_NOT_CONTAINS(out, "level=info");
    ASSERT_CONTAINS(out, "level=warn");
    ASSERT_CONTAINS(out, "level=error");
}

static void case_error(void) {
    g_case = "IPMAN_LOG=error";
    setenv("IPMAN_LOG", "error", 1);
    ipman_log_init();
    char out[4096];
    if (run_log_calls(out, sizeof out) != 0) FAIL("capture failed");
    ASSERT_NOT_CONTAINS(out, "level=info");
    ASSERT_NOT_CONTAINS(out, "level=warn");
    ASSERT_CONTAINS(out, "level=error");
}

static void case_idempotent_reset(void) {
    /* After a high-verbosity init, a subsequent init with no env must
     * reset to the default rather than retain the prior setting. */
    g_case = "idempotent reset to default";
    setenv("IPMAN_LOG", "info", 1);
    ipman_log_init();
    unsetenv("IPMAN_LOG");
    ipman_log_init();
    char out[4096];
    if (run_log_calls(out, sizeof out) != 0) FAIL("capture failed");
    ASSERT_NOT_CONTAINS(out, "level=info");
    ASSERT_CONTAINS(out, "level=warn");
    ASSERT_CONTAINS(out, "level=error");
}

int main(void) {
    case_default_unset();
    case_default_empty();
    case_unknown_value();
    case_debug();
    case_info();
    case_warn();
    case_error();
    case_idempotent_reset();
    if (g_failures > 0) {
        fprintf(stderr, "test_log: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_log: all 8 cases passed\n");
    return 0;
}
