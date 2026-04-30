/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_VALIDATION_H
#define IPMAN_VALIDATION_H

#include <stddef.h>

#include "cJSON.h"
#include "protocol.h"
#include "sqlite3.h"

#define IPMAN_TITLE_MAX_BYTES       512
#define IPMAN_SUMMARY_MAX_BYTES     2048
#define IPMAN_DESCRIPTION_MAX_BYTES 16384

/*
 * Read params[field] as a positive integer (>= 1) and write it to *out.
 * On failure, sets *err_code_out + *err_msg_out and returns -1; on success
 * returns 0.
 */
int ipman_read_positive_id(cJSON *params, const char *field,
                          sqlite3_int64 *out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out);

/*
 * Enforce the per-field byte caps for `title` (<=512), `summary` (<=2048),
 * and `description` (<=16384). NULL values are accepted (no-op). Returns 0
 * if `value` is within the cap or `field` is unknown; -1 with err set
 * otherwise.
 */
int ipman_validate_text_field(const char *field,
                             const char *value,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out);

/*
 * Validate that `value` is either NULL, an ISO-8601 calendar date
 * (YYYY-MM-DD), or an ISO-8601 datetime (YYYY-MM-DDThh:mm:ss[.sss][Z|±hh:mm]).
 * Returns 0 on success, -1 with err set otherwise.
 */
int ipman_validate_date_field(const char *value,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out);

/*
 * Generate a URL-safe slug from `title` into `out_slug` (size `max_len`).
 * Output is lowercase ASCII alnum runs joined by '-', leading/trailing
 * dashes trimmed, NUL-terminated. Truncates silently when the slug would
 * exceed max_len-1; safe to call with max_len==0 (no-op).
 */
void ipman_slugify(const char *title, char *out_slug, size_t max_len);

#endif
