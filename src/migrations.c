/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "migrations.h"

#include "log.h"
#include "sqlite3.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


static int has_schema_metadata(sqlite3 *db, int *found) {
    const char *sql =
        "SELECT 1 FROM sqlite_master "
        "WHERE type='table' AND name='schema_metadata' LIMIT 1;";
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("prepare has_schema_metadata",
                       "rc=%d detail=\"%s\"", rc, sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_step(st);
    *found = (rc == SQLITE_ROW) ? 1 : 0;
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        ipman_log_error("step has_schema_metadata",
                       "rc=%d detail=\"%s\"", rc, sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}


static int highest_embedded_migration_version(void) {
    int highest = 0;
    for (size_t i = 0; i < ipman_migrations_count; ++i) {
        if (ipman_migrations[i].version > highest) {
            highest = ipman_migrations[i].version;
        }
    }
    return highest;
}

static int insert_schema_metadata(sqlite3 *db, const struct ipman_migration *m) {
    const char *ins =
        "INSERT INTO schema_metadata(version, name, checksum) VALUES(?, ?, ?);";
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, ins, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("prepare schema_metadata insert",
                       "rc=%d detail=\"%s\"", rc, sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_int (st, 1, m->version);
    sqlite3_bind_text(st, 2, m->name,     -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, m->checksum, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        ipman_log_error("insert schema_metadata failed",
                       "version=%d rc=%d detail=\"%s\"",
                       m->version, rc, sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

static int current_version(sqlite3 *db, int *out) {
    const char *sql = "SELECT COALESCE(MAX(version), 0) FROM schema_metadata;";
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("prepare current_version",
                       "rc=%d detail=\"%s\"", rc, sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        ipman_log_error("step current_version",
                       "rc=%d detail=\"%s\"", rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return -1;
    }
    *out = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return 0;
}

static int verify_applied_checksums(sqlite3 *db) {
    const char *sql =
        "SELECT version, checksum FROM schema_metadata ORDER BY version;";
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("prepare verify_applied_checksums",
                       "rc=%d detail=\"%s\"", rc, sqlite3_errmsg(db));
        return -1;
    }
    int result = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int v = sqlite3_column_int(st, 0);
        const unsigned char *stored = sqlite3_column_text(st, 1);
        const struct ipman_migration *m = NULL;
        for (size_t i = 0; i < ipman_migrations_count; ++i) {
            if (ipman_migrations[i].version == v) { m = &ipman_migrations[i]; break; }
        }
        if (m == NULL) {
            ipman_log_error("applied migration not found in binary",
                           "version=%d hint=\"DB was written by a newer ipman\"",
                           v);
            result = -1;
            break;
        }
        if (stored == NULL || strcmp((const char *)stored, m->checksum) != 0) {
            ipman_log_error("migration checksum mismatch",
                           "version=%d name=%s stored=%s embedded=%s",
                           v, m->name,
                           stored ? (const char *)stored : "(null)",
                           m->checksum);
            result = -1;
            break;
        }
    }
    if (rc != SQLITE_DONE && result == 0) {
        ipman_log_error("step verify_applied_checksums",
                       "rc=%d detail=\"%s\"", rc, sqlite3_errmsg(db));
        result = -1;
    }
    sqlite3_finalize(st);
    return result;
}


/* Run PRAGMA foreign_key_check inside the migration's transaction. The
 * pragma returns one row per orphan (table, rowid, parent, fkid); zero
 * rows means the migration left FKs intact. Returns 0 on clean state,
 * -1 if any orphan is reported or the pragma itself errored. */
static int verify_no_fk_orphans(sqlite3 *db, const struct ipman_migration *m) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA foreign_key_check;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("prepare foreign_key_check",
                       "version=%d rc=%d detail=\"%s\"",
                       m->version, rc, sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const unsigned char *table = sqlite3_column_text(st, 0);
        ipman_log_error("foreign_key_check reported orphan",
                       "version=%d name=%s table=\"%s\"",
                       m->version, m->name,
                       table ? (const char *)table : "(unknown)");
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        ipman_log_error("step foreign_key_check",
                       "version=%d rc=%d detail=\"%s\"",
                       m->version, rc, sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

static int apply_one(sqlite3 *db, const struct ipman_migration *m) {
    char *err = NULL;
    int result = -1;
    int in_tx = 0;

    /* PRAGMA foreign_keys cannot be changed inside a transaction, so
     * disable enforcement before BEGIN. The DROP+RENAME table-rebuild
     * idiom (used by migrations 0004, 0005, and the consolidated 0001)
     * would otherwise trigger ON DELETE RESTRICT on rows in dependent
     * tables when the implicit DELETE fires during DROP. The
     * foreign_key_check below is the safety net that proves the
     * migration didn't leave orphans before we commit. See
     * sqlite.org/lang_altertable.html "Making Other Kinds Of Table
     * Schema Changes" for the canonical recipe. */
    int rc = sqlite3_exec(db, "PRAGMA foreign_keys=OFF;", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        ipman_log_error("foreign_keys=OFF failed",
                       "version=%d rc=%d detail=\"%s\"",
                       m->version, rc, err ? err : "(null)");
        sqlite3_free(err);
        return -1;
    }

    rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        ipman_log_error("BEGIN failed", "version=%d rc=%d detail=\"%s\"",
                       m->version, rc, err ? err : "(null)");
        sqlite3_free(err);
        goto cleanup;
    }
    in_tx = 1;

    rc = sqlite3_exec(db, m->sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        ipman_log_error("migration SQL failed",
                       "version=%d name=%s rc=%d detail=\"%s\"",
                       m->version, m->name, rc, err ? err : "(null)");
        sqlite3_free(err);
        goto cleanup;
    }

    if (verify_no_fk_orphans(db, m) != 0) goto cleanup;

    if (insert_schema_metadata(db, m) != 0) goto cleanup;

    rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        ipman_log_error("COMMIT failed",
                       "version=%d rc=%d detail=\"%s\"",
                       m->version, rc, err ? err : "(null)");
        sqlite3_free(err);
        goto cleanup;
    }
    in_tx = 0;
    result = 0;

cleanup:
    if (in_tx) sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    /* Always restore foreign_keys, even on the failure path: the
     * connection survives the failed migration (the daemon may keep
     * using it for diagnostics) and must not be left with FK
     * enforcement disabled. */
    if (sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL) != SQLITE_OK) {
        ipman_log_error("foreign_keys=ON failed (post-migration restore)",
                       "version=%d", m->version);
        result = -1;
    }
    if (result == 0) {
        ipman_log_info("applied migration",
                      "version=%d name=%s", m->version, m->name);
    }
    return result;
}

/* Snapshot the source DB to a backup file before applying migrations.
 *
 * Steps: (1) PRAGMA wal_checkpoint(TRUNCATE) merges any pending WAL
 * frames into the main DB file so a byte-level copy is a complete
 * snapshot. ipman holds the only handle during init, so the truncating
 * checkpoint is safe -- no other reader/writer can be mid-transaction.
 * (2) Copy src -> dst.tmp, fsync, then atomically rename to dst. The
 * .tmp + rename dance means a crash mid-copy does not leave a
 * truncated file at the final name.
 *
 * Returns 0 on success, -1 on any error (also logs a diagnostic). */
static int backup_db_file(sqlite3 *db, const char *src, const char *dst) {
    char *err = NULL;
    if (sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE);",
                     NULL, NULL, &err) != SQLITE_OK) {
        ipman_log_error("wal_checkpoint failed",
                       "detail=\"%s\"", err ? err : "(null)");
        sqlite3_free(err);
        return -1;
    }

    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        ipman_log_error("backup tmp path too long", "dst=%s", dst);
        return -1;
    }

    int sfd = open(src, O_RDONLY | O_CLOEXEC);
    if (sfd < 0) {
        ipman_log_error("backup open src failed",
                       "src=%s detail=\"%s\"", src, strerror(errno));
        return -1;
    }
    int dfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (dfd < 0) {
        ipman_log_error("backup open dst failed",
                       "tmp=%s detail=\"%s\"", tmp, strerror(errno));
        close(sfd);
        return -1;
    }

    char buf[64 * 1024];
    ssize_t rd;
    while ((rd = read(sfd, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < rd) {
            ssize_t wr = write(dfd, buf + off, (size_t)(rd - off));
            if (wr < 0) {
                if (errno == EINTR) continue;
                ipman_log_error("backup write failed",
                               "tmp=%s detail=\"%s\"", tmp, strerror(errno));
                close(sfd); close(dfd); unlink(tmp);
                return -1;
            }
            off += wr;
        }
    }
    if (rd < 0) {
        ipman_log_error("backup read failed",
                       "src=%s detail=\"%s\"", src, strerror(errno));
        close(sfd); close(dfd); unlink(tmp);
        return -1;
    }
    if (fsync(dfd) != 0) {
        ipman_log_error("backup fsync failed",
                       "tmp=%s detail=\"%s\"", tmp, strerror(errno));
        close(sfd); close(dfd); unlink(tmp);
        return -1;
    }
    close(sfd);
    if (close(dfd) != 0) {
        ipman_log_error("backup close failed",
                       "tmp=%s detail=\"%s\"", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, dst) != 0) {
        ipman_log_error("backup rename failed",
                       "tmp=%s dst=%s detail=\"%s\"",
                       tmp, dst, strerror(errno));
        unlink(tmp);
        return -1;
    }
    ipman_log_info("backup created", "path=%s", dst);
    return 0;
}

/* Pre-flight check on the existing DB before mutating anything. This
 * is the only invariant transactional rollback cannot give us: if the
 * DB was already corrupted by an older buggy ipman (or by external
 * tampering), applying migrations on top would propagate the damage
 * forward. integrity_check covers page-level corruption that the
 * FK-only check inside apply_one cannot see; foreign_key_check is
 * here for early-exit (apply_one would catch it later, but we save
 * work by failing before the first migration's SQL even runs). */
static int preflight_check(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("prepare integrity_check",
                       "rc=%d detail=\"%s\"", rc, sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        ipman_log_error("step integrity_check",
                       "rc=%d detail=\"%s\"", rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        return -1;
    }
    const unsigned char *result = sqlite3_column_text(st, 0);
    int ok = (result != NULL && strcmp((const char *)result, "ok") == 0);
    if (!ok) {
        ipman_log_error("preflight integrity_check failed",
                       "detail=\"%s\"",
                       result ? (const char *)result : "(null)");
    }
    sqlite3_finalize(st);
    if (!ok) return -1;

    rc = sqlite3_prepare_v2(db, "PRAGMA foreign_key_check;", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("prepare foreign_key_check (preflight)",
                       "rc=%d detail=\"%s\"", rc, sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const unsigned char *table = sqlite3_column_text(st, 0);
        ipman_log_error("preflight foreign_key_check found orphan",
                       "table=\"%s\"",
                       table ? (const char *)table : "(unknown)");
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        ipman_log_error("step foreign_key_check (preflight)",
                       "rc=%d detail=\"%s\"", rc, sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

int ipman_migrations_apply(sqlite3 *db, const char *db_path, int *out_version) {
    int meta_present = 0;
    if (has_schema_metadata(db, &meta_present) != 0) return -1;

    int current = 0;
    if (meta_present) {
        if (current_version(db, &current) != 0) return -1;
        if (verify_applied_checksums(db) != 0) return -1;
        /* preflight only makes sense once the DB has tables. A fresh
         * DB has no schema_metadata yet -- there's nothing to verify. */
        if (preflight_check(db) != 0) return -1;
    }

    int highest = highest_embedded_migration_version();
    if (current > highest) {
        ipman_log_error("DB schema newer than binary",
                       "db_version=%d binary_max=%d",
                       current, highest);
        return -1;
    }

    if (current < highest) {
        int pending = 0;
        for (size_t i = 0; i < ipman_migrations_count; ++i) {
            if (ipman_migrations[i].version > current) ++pending;
        }
        ipman_log_info("migration plan",
                      "from=%d to=%d count=%d", current, highest, pending);

        /* Pre-migration snapshot. Skipped on a fresh DB (current=0)
         * because there is nothing to roll back to, and skipped when
         * db_path is NULL (in-memory or temp-file tests). */
        if (db_path != NULL && current > 0) {
            char bak[PATH_MAX];
            int n = snprintf(bak, sizeof bak, "%s.bak-v%d", db_path, current);
            if (n < 0 || (size_t)n >= sizeof bak) {
                ipman_log_error("backup path too long", "src=%s", db_path);
                return -1;
            }
            if (backup_db_file(db, db_path, bak) != 0) return -1;
        }
    }

    for (size_t i = 0; i < ipman_migrations_count; ++i) {
        const struct ipman_migration *m = &ipman_migrations[i];
        if (m->version <= current) continue;
        if (apply_one(db, m) != 0) return -1;
        current = m->version;
    }

    *out_version = current;
    return 0;
}
