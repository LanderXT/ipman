/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_DB_H
#define IPMAN_DB_H

#include "sqlite3.h"

/*
 * Open the ipman SQLite database at `path`, creating it if absent.
 * `home_path` is the workspace directory containing `keysalt`; it is used
 * to derive the SQLCipher page-encryption key (Argon2id over machine-id +
 * UID + keysalt) which is then applied via PRAGMA key.
 *
 * Applies our baseline pragmas on every connection (after the key):
 *   journal_mode=WAL, foreign_keys=ON, busy_timeout=5000.
 *
 * On success writes the handle to *db and returns 0. On failure logs
 * a diagnostic on stderr, leaves *db untouched, and returns non-zero.
 * Caller is responsible for ipman_db_close on success.
 */

int ipman_db_open(const char *home_path, const char *path, sqlite3 **db);
int ipman_db_open_existing(const char *home_path, const char *path, sqlite3 **db);
void ipman_db_close(sqlite3 *db);

/*
 * Run BEGIN IMMEDIATE with retry-on-busy. PRAGMA busy_timeout already gives
 * us up to 5 s per attempt, but under heavy concurrent contention a single
 * exhausted timeout still surfaces SQLITE_BUSY to the caller and gets
 * mapped to a confusing internal_error response. This wrapper retries with
 * exponential backoff (capped at 200 ms between attempts) up to a total
 * budget. Override the budget via IPMAN_BEGIN_RETRY_MS (default 30000).
 *
 * Returns 0 on success, -1 on failure (with a diagnostic on stderr that
 * names the budget and attempt count, so triage can distinguish a real
 * deadlock from a too-tight budget).
 */
int ipman_db_begin_immediate(sqlite3 *db);

#endif
