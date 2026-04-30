#include "migrations.h"

#include "log.h"
#include "sqlite3.h"

#include <stddef.h>
#include <string.h>


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

static int migration_checksum_matches(const struct ipman_migration *m,
                                      const char *stored) {
    if (stored == NULL) return 0;
    if (strcmp(stored, m->checksum) == 0) return 1;

    /* Compatibility for databases created with an earlier 0007 build. */
    if (m->version == 7 &&
        strcmp(m->name, "0007_task_relations") == 0 &&
        strcmp(stored,
               "ace5f472c5ada1da853103392ddf0d2c4e6fcf430609bf527d7b6f3762e0c3cc") == 0) {
        return 1;
    }

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
        if (!migration_checksum_matches(m, (const char *)stored)) {
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


static int apply_one(sqlite3 *db, const struct ipman_migration *m) {
    char *err = NULL;
    int restore_foreign_keys = 0;
    /* These rebuild `events`, which is referenced by closure_records. */
    if (m->version == 9 || m->version == 10 ||
        m->version == 11 || m->version == 13 ||
        m->version == 14 || m->version == 19) {
        int rc_fk = sqlite3_exec(db, "PRAGMA foreign_keys=OFF;", NULL, NULL, &err);
        if (rc_fk != SQLITE_OK) {
            ipman_log_error("disable foreign_keys failed",
                           "version=%d rc=%d detail=\"%s\"",
                           m->version, rc_fk, err ? err : "(null)");
            sqlite3_free(err);
            return -1;
        }
        restore_foreign_keys = 1;
    }
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        ipman_log_error("BEGIN failed", "version=%d rc=%d detail=\"%s\"",
                       m->version, rc, err ? err : "(null)");
        sqlite3_free(err);
        if (restore_foreign_keys) {
            sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
        }
        return -1;
    }

    rc = sqlite3_exec(db, m->sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        ipman_log_error("migration SQL failed",
                       "version=%d name=%s rc=%d detail=\"%s\"",
                       m->version, m->name, rc, err ? err : "(null)");
        sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        if (restore_foreign_keys) {
            sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
        }
        return -1;
    }

    if (insert_schema_metadata(db, m) != 0) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        if (restore_foreign_keys) {
            sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
        }
        return -1;
    }

    rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        ipman_log_error("COMMIT failed",
                       "version=%d rc=%d detail=\"%s\"",
                       m->version, rc, err ? err : "(null)");
        sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        if (restore_foreign_keys) {
            sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
        }
        return -1;
    }
    if (restore_foreign_keys) {
        rc = sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, &err);
        if (rc != SQLITE_OK) {
            ipman_log_error("restore foreign_keys failed",
                           "version=%d rc=%d detail=\"%s\"",
                           m->version, rc, err ? err : "(null)");
            sqlite3_free(err);
            return -1;
        }
        sqlite3_stmt *fk_stmt = NULL;
        rc = sqlite3_prepare_v2(db, "PRAGMA foreign_key_check;", -1, &fk_stmt, NULL);
        if (rc != SQLITE_OK) {
            ipman_log_error("foreign_key_check prepare failed",
                           "version=%d rc=%d", m->version, rc);
            return -1;
        }
        rc = sqlite3_step(fk_stmt);
        sqlite3_finalize(fk_stmt);
        if (rc == SQLITE_ROW) {
            ipman_log_error("foreign_key_check failed",
                           "version=%d detail=\"FK violations detected\"",
                           m->version);
            return -1;
        }
        if (rc != SQLITE_DONE) {
            ipman_log_error("foreign_key_check step failed",
                           "version=%d rc=%d", m->version, rc);
            return -1;
        }
    }
    ipman_log_info("applied migration",
                  "version=%d name=%s", m->version, m->name);
    return 0;
}

int ipman_migrations_apply(sqlite3 *db, int *out_version) {
    int meta_present = 0;
    if (has_schema_metadata(db, &meta_present) != 0) return -1;

    int current = 0;
    if (meta_present) {
        if (current_version(db, &current) != 0) return -1;
        if (verify_applied_checksums(db) != 0) return -1;
    }

    int highest = highest_embedded_migration_version();
    if (current > highest) {
        ipman_log_error("DB schema newer than binary",
                       "db_version=%d binary_max=%d",
                       current, highest);
        return -1;
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
