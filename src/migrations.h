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
 * On success, writes the resulting schema version into *out_version and
 * returns 0.
 */
int ipman_migrations_apply(sqlite3 *db, int *out_version);

#endif
