/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_PASSPHRASE_H
#define IPMAN_PASSPHRASE_H

#include <stddef.h>

/*
 * Read a passphrase into `out` (size `cap`).
 *
 * Resolution order:
 *   1. IPMAN_PASSPHRASE env var — used by tests and scripts.
 *   2. /dev/tty — interactive, echo suppressed, prints `prompt` first.
 *
 * `out_len` is set to the number of bytes written (not including any NUL).
 * An empty passphrase (length 0) is rejected with an error.
 *
 * Returns 0 on success, -1 on error (message written to stderr).
 * Caller MUST sodium_memzero(out, cap) after use.
 */
int ipman_read_passphrase(const char *prompt, char *out, size_t cap,
                          size_t *out_len);

#endif /* IPMAN_PASSPHRASE_H */
