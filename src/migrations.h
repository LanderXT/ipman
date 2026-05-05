/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_MIGRATIONS_H
#define IPMAN_MIGRATIONS_H

#include "sqlite3.h"

#include <stddef.h>

/*
 * Embedded migration registry.
 *
 * Each entry carries the full SQL text (embedded at build time from
 * migrations/NNNN_name.sql by scripts/embed_migrations.sh) plus the
 * SHA-256 hex of the source file. Fresh empty databases run every
 * embedded migration in version order and record each one in
 * schema_metadata; existing databases apply only migrations newer than
 * their recorded version. Stored hashes are verified on every later
 * startup — any edit of an already-applied migration will be detected.
 *
 * The `ipman_migrations` array is defined in the generated file
 * build/gen/migrations_data.c, kept sorted by ascending version.
 */

struct ipman_migration {
    int          version;
    const char  *name;
    const char  *sql;
    const char  *checksum;  /* SHA-256 hex, lower case, 64 chars + NUL. */
};

extern const struct ipman_migration ipman_migrations[];
extern const size_t                ipman_migrations_count;

/*
 * Apply all migrations with version > current. A fresh empty database
 * has current = 0, so every embedded migration is applied; an existing
 * database applies only those newer than the recorded version. Each
 * migration runs in its own transaction; on failure the transaction
 * rolls back and the function returns non-zero. A migration whose
 * stored checksum does not match the embedded one is also a fatal
 * error (migrations are append-only).
 *
 * Before applying anything, a pre-flight check runs PRAGMA
 * integrity_check and PRAGMA foreign_key_check on the existing DB and
 * aborts with non-zero if either reports trouble — this guards against
 * mutating an already-corrupted database.
 *
 * If `db_path` is non-NULL AND there are pending migrations to apply,
 * a snapshot of `db_path` is copied to `<db_path>.bak-v<current>`
 * before the first migration runs. Subsequent invocations at the same
 * source-version overwrite the backup. Pass NULL to skip the backup
 * (useful for tests that operate on temp/in-memory DBs).
 *
 * On success, writes the resulting schema version into *out_version and
 * returns 0.
 */
int ipman_migrations_apply(sqlite3 *db, const char *db_path, int *out_version);

#endif
