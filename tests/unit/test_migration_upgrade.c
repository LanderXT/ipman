/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 *
 * Regression test for the v3 -> v4 upgrade FK violation.
 *
 * Migration 0004 rebuilds the events table (CREATE _new + INSERT SELECT
 * + DROP + RENAME). closure_records has FOREIGN KEY (event_id)
 * REFERENCES events(id) ON DELETE RESTRICT. With PRAGMA foreign_keys=ON
 * (the production setting from src/db.c), DROP TABLE events triggers an
 * implicit DELETE that RESTRICT blocks whenever closures hold real
 * event_id references -- the migration aborts with SQLITE_CONSTRAINT.
 *
 * Reproduction steps in this test:
 *   1. Apply migrations with version <= 3 to a fresh SQLite handle.
 *   2. Seed one event + one closure_record that references it.
 *   3. Call ipman_migrations_apply() -- which must apply v4+ on top.
 *
 * Before the runner fix, step 3 fails. After the fix (foreign_keys
 * toggled OFF for the migration window plus foreign_key_check inside
 * the transaction) it must succeed and the seed data must survive.
 */

#include "log.h"
#include "migrations.h"
#include "sqlite3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;
static const char *g_case = "(none)";

#define FAILF(fmt, ...) do { \
    fprintf(stderr, "  FAIL [%s] " fmt " (at %s:%d)\n", \
            g_case, __VA_ARGS__, __FILE__, __LINE__); \
    g_failures++; \
    return; \
} while (0)

#define ASSERT_EQ_INT(actual, expected) do { \
    long _a = (long)(actual), _e = (long)(expected); \
    if (_a != _e) FAILF("expected %ld, got %ld", _e, _a); \
} while (0)

#define ASSERT_OK(rc, what) do { \
    if ((rc) != SQLITE_OK) FAILF("%s: rc=%d detail=\"%s\"", \
                                 (what), (rc), sqlite3_errmsg(db)); \
} while (0)

/* Apply each embedded migration whose version is <= cap, exec'ing the
 * SQL and recording the entry in schema_metadata with its embedded
 * checksum so that ipman_migrations_apply()'s later checksum-verify
 * step accepts the state. Returns 0 on success, -1 on failure. */
static int seed_schema_at(sqlite3 *db, int cap) {
    for (size_t i = 0; i < ipman_migrations_count; ++i) {
        const struct ipman_migration *m = &ipman_migrations[i];
        if (m->version > cap) continue;
        char *err = NULL;
        if (sqlite3_exec(db, m->sql, NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr, "seed %d %s failed: %s\n",
                    m->version, m->name, err ? err : "(null)");
            sqlite3_free(err);
            return -1;
        }
        sqlite3_stmt *st = NULL;
        const char *ins =
            "INSERT INTO schema_metadata(version, name, checksum) VALUES(?, ?, ?);";
        if (sqlite3_prepare_v2(db, ins, -1, &st, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_int (st, 1, m->version);
        sqlite3_bind_text(st, 2, m->name,     -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, m->checksum, -1, SQLITE_STATIC);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return -1;
    }
    return 0;
}

/* Insert a single closure_record that references a single event so the
 * v3 -> v4 rebuild has at least one parent row in events whose deletion
 * RESTRICT would block. */
static int seed_closure_referencing_event(sqlite3 *db) {
    char *err = NULL;
    /* events has no FK to plans (entity_id is CHECK >0 only) and
     * closure_records has no FK to plans either, so we don't need a
     * plans row for this test. The only edge that matters is
     * closure_records.event_id -> events.id. */
    const char *sql =
        "INSERT INTO events(entity_type, entity_id, event_type, actor, summary) "
        "  VALUES('plan', 1, 'plan_created', 'tester', 'seed');"
        "INSERT INTO closure_records("
        "  entity_type, entity_id, closure_status, resolution, "
        "  outcome_summary, closing_comment, author, event_id) "
        "  VALUES('plan', 1, 'closed', 'completed', "
        "         'ok', 'wrap up', 'tester', 1);";
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "seed closure failed: %s\n", err ? err : "(null)");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int latest_embedded_version(void) {
    int top = 0;
    for (size_t i = 0; i < ipman_migrations_count; ++i) {
        if (ipman_migrations[i].version > top) top = ipman_migrations[i].version;
    }
    return top;
}

static void case_v3_with_data_upgrades_to_latest(void) {
    g_case = "v3_with_data_upgrades_to_latest";

    sqlite3 *db = NULL;
    char path[] = "/tmp/ipman-mig-test.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { FAILF("mkstemp: %s", strerror(errno)); }
    close(fd);
    unlink(path);

    int rc = sqlite3_open(path, &db);
    ASSERT_OK(rc, "sqlite3_open");

    /* Reproduce production's connection-level default. */
    rc = sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    ASSERT_OK(rc, "pragma foreign_keys=ON");

    if (seed_schema_at(db, 3) != 0) {
        sqlite3_close(db);
        unlink(path);
        FAILF("%s", "could not seed v3 schema");
    }
    if (seed_closure_referencing_event(db) != 0) {
        sqlite3_close(db);
        unlink(path);
        FAILF("%s", "could not seed closure->event row");
    }

    int out_version = -1;
    int apply_rc = ipman_migrations_apply(db, NULL, &out_version);
    if (apply_rc != 0) {
        sqlite3_close(db);
        unlink(path);
        FAILF("%s", "ipman_migrations_apply failed (the bug)");
    }
    ASSERT_EQ_INT(out_version, latest_embedded_version());

    /* Data must survive the events rebuild. */
    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT count(*) FROM events WHERE id=1;", -1, &st, NULL);
    ASSERT_OK(rc, "prepare event count");
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) FAILF("event count step rc=%d", rc);
    int event_count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    ASSERT_EQ_INT(event_count, 1);

    rc = sqlite3_prepare_v2(db,
        "SELECT event_id FROM closure_records WHERE id=1;", -1, &st, NULL);
    ASSERT_OK(rc, "prepare closure event_id");
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) FAILF("closure step rc=%d", rc);
    int linked_event = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    ASSERT_EQ_INT(linked_event, 1);

    /* Foreign keys must be re-enabled after the runner returns. */
    rc = sqlite3_prepare_v2(db, "PRAGMA foreign_keys;", -1, &st, NULL);
    ASSERT_OK(rc, "prepare pragma foreign_keys");
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) FAILF("pragma step rc=%d", rc);
    int fk_state = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    ASSERT_EQ_INT(fk_state, 1);

    sqlite3_close(db);
    unlink(path);
    fprintf(stdout, "  PASS [%s]\n", g_case);
}

/* Preflight test: a database that already contains an FK orphan must
 * be rejected BEFORE any migration runs. The pre-flight check is the
 * one invariant that transactional rollback cannot give us — if the DB
 * was corrupted by an old buggy ipman, applying migrations on top of
 * it would propagate the corruption forward. Here we deliberately
 * insert an orphan with foreign_keys=OFF (the only way to make SQLite
 * accept an FK violation), then prove that ipman_migrations_apply()
 * refuses to touch it and leaves schema_metadata at v3. */
static void case_preflight_rejects_orphan(void) {
    g_case = "preflight_rejects_orphan";

    sqlite3 *db = NULL;
    char path[] = "/tmp/ipman-mig-preflight.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { FAILF("mkstemp: %s", strerror(errno)); }
    close(fd);
    unlink(path);

    int rc = sqlite3_open(path, &db);
    ASSERT_OK(rc, "sqlite3_open");

    rc = sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    ASSERT_OK(rc, "pragma foreign_keys=ON");

    if (seed_schema_at(db, 3) != 0) {
        sqlite3_close(db); unlink(path);
        FAILF("%s", "could not seed v3 schema");
    }

    /* Sneak past FK enforcement to plant an orphan. */
    char *err = NULL;
    rc = sqlite3_exec(db,
        "PRAGMA foreign_keys=OFF;"
        "INSERT INTO closure_records("
        "  entity_type, entity_id, closure_status, resolution, "
        "  outcome_summary, closing_comment, author, event_id) "
        "  VALUES('plan', 1, 'closed', 'completed', "
        "         'ok', 'wrap up', 'tester', 9999);"
        "PRAGMA foreign_keys=ON;",
        NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "plant orphan failed: %s\n", err ? err : "(null)");
        sqlite3_free(err);
        sqlite3_close(db); unlink(path);
        FAILF("%s", "could not plant orphan");
    }

    int out_version = -1;
    int apply_rc = ipman_migrations_apply(db, NULL, &out_version);
    if (apply_rc == 0) {
        sqlite3_close(db); unlink(path);
        FAILF("%s", "apply returned 0 on a corrupted DB (expected non-zero)");
    }

    /* Schema must be untouched: still at v3, not v4+. */
    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT COALESCE(MAX(version),0) FROM schema_metadata;",
        -1, &st, NULL);
    ASSERT_OK(rc, "prepare schema_metadata MAX");
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) FAILF("schema_metadata step rc=%d", rc);
    int post_version = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    ASSERT_EQ_INT(post_version, 3);

    sqlite3_close(db);
    unlink(path);
    fprintf(stdout, "  PASS [%s]\n", g_case);
}

/* Backup test: when db_path is passed and there are pending migrations,
 * a snapshot copy must be written to <db_path>.bak-v<current> BEFORE
 * the first migration runs. The backup is the only rollback we have
 * across the multi-migration window if a future migration introduces a
 * non-transactional bug (corrupt sidecar, partial WAL flush, etc.). */
static void case_backup_created_for_pending_upgrade(void) {
    g_case = "backup_created_for_pending_upgrade";

    char path[] = "/tmp/ipman-mig-backup.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { FAILF("mkstemp: %s", strerror(errno)); }
    close(fd);
    unlink(path);

    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    ASSERT_OK(rc, "sqlite3_open");

    rc = sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    ASSERT_OK(rc, "pragma foreign_keys=ON");

    if (seed_schema_at(db, 3) != 0) {
        sqlite3_close(db); unlink(path);
        FAILF("%s", "could not seed v3 schema");
    }
    if (seed_closure_referencing_event(db) != 0) {
        sqlite3_close(db); unlink(path);
        FAILF("%s", "could not seed closure->event row");
    }

    int out_version = -1;
    int apply_rc = ipman_migrations_apply(db, path, &out_version);
    if (apply_rc != 0) {
        sqlite3_close(db); unlink(path);
        FAILF("%s", "apply failed unexpectedly");
    }

    char bak[256];
    snprintf(bak, sizeof bak, "%s.bak-v3", path);
    struct stat sb;
    if (stat(bak, &sb) != 0) {
        sqlite3_close(db);
        unlink(path);
        FAILF("backup not created at %s: %s", bak, strerror(errno));
    }
    if (sb.st_size <= 0) {
        sqlite3_close(db);
        unlink(path);
        unlink(bak);
        FAILF("backup is empty (%ld bytes)", (long)sb.st_size);
    }

    sqlite3_close(db);
    unlink(path);
    unlink(bak);
    fprintf(stdout, "  PASS [%s]\n", g_case);
}

int main(void) {
    case_v3_with_data_upgrades_to_latest();
    case_preflight_rejects_orphan();
    case_backup_created_for_pending_upgrade();

    if (g_failures > 0) {
        fprintf(stderr, "test_migration_upgrade: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "test_migration_upgrade: ok\n");
    return 0;
}
