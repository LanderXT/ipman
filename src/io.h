/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_IO_H
#define IPMAN_IO_H

#include <stdio.h>

/*
 * Read up to `max_bytes` from `fp` into a freshly-allocated NUL-terminated
 * buffer. `max_bytes` is the maximum payload size accepted; passing 0 disables
 * the cap (unbounded read).
 *
 * Returns 0 on success: *out_buf holds the buffer (caller frees with free())
 * and *out_len holds the byte count (excluding the trailing NUL).
 *
 * Returns -1 on read/allocation failure. *out_buf is left unset.
 *
 * Returns -2 when input exceeds `max_bytes`. *out_buf is set to NULL and
 * *out_len reports the bytes consumed before the cap was hit (>= max_bytes),
 * so the caller can surface it in a structured error.
 *
 * Empty input is NOT treated as an error — callers get a zero-length string
 * and must decide what to do.
 */
int ipman_read_all(FILE *fp, size_t max_bytes,
                  char **out_buf, size_t *out_len);

/*
 * Standard base64 helpers used by the CLI transport mode. The decoder accepts
 * ASCII whitespace between encoded characters. Returned buffers are
 * NUL-terminated for convenience, but *out_len is the authoritative byte count.
 */
int ipman_base64_encode(const unsigned char *in, size_t in_len, char **out);
int ipman_base64_decode(const char *in, size_t in_len, char **out, size_t *out_len);

#endif
