/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_SQL_OPS_H
#define IPMAN_SQL_OPS_H

#include "sqlite3.h"

#include <stdio.h>

/*
 * Execute one or more SQL statements against `db`, writing any result rows
 * to `out` in the default sqlite3 CLI format: one row per line, columns
 * separated by '|', no headers, NULL rendered as empty string.
 *
 * Multiple semicolon-separated statements are supported -- the function
 * walks the buffer with sqlite3_prepare_v2 and steps each statement to
 * completion before moving on. DDL and DML statements yield no rows.
 *
 * On the first SQL error logs a diagnostic on stderr and returns -1; rows
 * already printed for prior statements remain on stdout. On success returns 0.
 *
 * Intended as a developer / test escape-hatch reachable via `ipman sql`.
 * This is NOT a separate trust boundary: callers who can run ipman can
 * already run ipman export --plaintext, so no extra gating is enforced here.
 */
int ipman_sql_run(sqlite3 *db, const char *sql, FILE *out);

#endif
