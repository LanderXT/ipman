#include "migrations.h"

#include "log.h"
#include "sqlite3.h"

#include <stddef.h>
#include <string.h>

static const char *baseline_schema_sql[] = {
    "CREATE TABLE IF NOT EXISTS schema_metadata ("
    "version INTEGER PRIMARY KEY,"
    "name TEXT NOT NULL,"
    "applied_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "checksum TEXT NOT NULL"
    ");",

    "CREATE TABLE IF NOT EXISTS plans ("
    "id INTEGER PRIMARY KEY,"
    "code TEXT UNIQUE,"
    "title TEXT NOT NULL CHECK (length(trim(title)) > 0),"
    "summary TEXT,"
    "description TEXT,"
    "status TEXT NOT NULL DEFAULT 'open' CHECK (status IN ("
    "'open','in_progress','paused','completed','canceled','archived'"
    ")),"
    "priority TEXT CHECK (priority IS NULL OR priority IN ("
    "'low','medium','high','critical'"
    ")),"
    "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "opened_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "closed_at TEXT,"
    "archived_at TEXT,"
    "owner TEXT,"
    "target_date TEXT,"
    "tags TEXT,"
    "version_label TEXT," "uid TEXT," "label TEXT"
    ");",

    "CREATE TABLE IF NOT EXISTS events ("
    "id INTEGER PRIMARY KEY,"
    "entity_type TEXT NOT NULL CHECK (entity_type IN ('plan','phase','task')),"
    "entity_id INTEGER NOT NULL CHECK (entity_id > 0),"
    "event_type TEXT NOT NULL CHECK (event_type IN ("
    "'plan_created','plan_updated','plan_closed','plan_archived','plan_reopened',"
    "'plan_activated','plan_deactivated',"
    "'phase_created','phase_updated','phase_moved','phase_closed','phase_reopened',"
    "'phase_current_changed',"
    "'task_created','task_updated','task_linked_external',"
    "'task_status_changed','task_deferred',"
    "'task_canceled','task_replaced','task_reopened','task_closed',"
    "'task_current_changed','comment_added','comment_updated','comment_invalidated',"
    "'instruction_added','instruction_updated','instruction_invalidated'"
    ")),"
    "actor TEXT NOT NULL CHECK (length(trim(actor)) > 0),"
    "event_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "summary TEXT NOT NULL,"
    "details TEXT,"
    "old_value TEXT,"
    "new_value TEXT,"
    "related_entity_type TEXT CHECK ("
    "related_entity_type IS NULL OR related_entity_type IN ('plan','phase','task')"
    "),"
    "related_entity_id INTEGER CHECK ("
    "related_entity_id IS NULL OR related_entity_id > 0"
    "),"
    "request_id TEXT"
    ");",

    "CREATE TABLE IF NOT EXISTS phases ("
    "id INTEGER PRIMARY KEY,"
    "plan_id INTEGER NOT NULL,"
    "title TEXT NOT NULL CHECK (length(trim(title)) > 0),"
    "summary TEXT,"
    "description TEXT,"
    "status TEXT NOT NULL DEFAULT 'open' CHECK (status IN ("
    "'open','in_progress','blocked','completed','canceled'"
    ")),"
    "sequence_no INTEGER NOT NULL CHECK (sequence_no > 0),"
    "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "opened_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "closed_at TEXT,"
    "owner TEXT,"
    "target_start_date TEXT,"
    "target_end_date TEXT,"
    "local_seq INTEGER," "uid TEXT," "label TEXT,"
    "FOREIGN KEY (plan_id) REFERENCES plans(id) ON DELETE RESTRICT,"
    "UNIQUE (plan_id, sequence_no),"
    "UNIQUE (id, plan_id)"
    ");",

    "CREATE TABLE IF NOT EXISTS tasks ("
    "id INTEGER PRIMARY KEY,"
    "plan_id INTEGER NOT NULL,"
    "phase_id INTEGER,"
    "parent_task_id INTEGER,"
    "title TEXT NOT NULL CHECK (length(trim(title)) > 0),"
    "summary TEXT,"
    "description TEXT,"
    "status TEXT NOT NULL DEFAULT 'todo' CHECK (status IN ("
    "'todo','in_progress','blocked','deferred','done','canceled'"
    ")),"
    "resolution TEXT CHECK (resolution IS NULL OR resolution IN ("
    "'completed','canceled','not_planned','replaced','duplicate','discarded'"
    ")),"
    "priority TEXT NOT NULL DEFAULT 'medium' CHECK (priority IN ("
    "'low','medium','high','critical'"
    ")),"
    "task_type TEXT NOT NULL DEFAULT 'task' CHECK (task_type IN ("
    "'task','research','bug','decision','review','documentation'"
    ")),"
    "origin_type TEXT NOT NULL DEFAULT 'planned' CHECK (origin_type IN ("
    "'planned','addendum','discovered','replacement','carryover','external_request'"
    ")),"
    "assignee TEXT,"
    "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "started_at TEXT,"
    "closed_at TEXT,"
    "deferred_until TEXT,"
    "blocked_reason TEXT,"
    "reason_code TEXT,"
    "reason_text TEXT,"
    "due_date TEXT,"
    "target_start_date TEXT,"
    "estimate TEXT,"
    "origin_ref_type TEXT,"
    "origin_ref_id TEXT,"
    "origin_task_id INTEGER,"
    "local_seq INTEGER," "uid TEXT," "label TEXT,"
    "FOREIGN KEY (plan_id) REFERENCES plans(id) ON DELETE RESTRICT,"
    "FOREIGN KEY (phase_id, plan_id) REFERENCES phases(id, plan_id) "
    "ON DELETE RESTRICT,"
    "FOREIGN KEY (parent_task_id, plan_id) REFERENCES tasks(id, plan_id) "
    "ON DELETE RESTRICT,"
    "FOREIGN KEY (origin_task_id) REFERENCES tasks(id) ON DELETE RESTRICT,"
    "UNIQUE (id, plan_id)"
    ");",

    "CREATE TABLE IF NOT EXISTS comments ("
    "id INTEGER PRIMARY KEY,"
    "entity_type TEXT NOT NULL CHECK (entity_type IN ('plan','phase','task')),"
    "entity_id INTEGER NOT NULL CHECK (entity_id > 0),"
    "comment_type TEXT NOT NULL DEFAULT 'general',"
    "body TEXT NOT NULL CHECK (length(trim(body)) > 0),"
    "author TEXT NOT NULL CHECK (length(trim(author)) > 0),"
    "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "updated_at TEXT,"
    "invalidated_at TEXT,"
    "invalidated_by TEXT"
    ");",

    "CREATE TABLE IF NOT EXISTS closure_records ("
    "id INTEGER PRIMARY KEY,"
    "entity_type TEXT NOT NULL CHECK (entity_type IN ('plan','phase','task')),"
    "entity_id INTEGER NOT NULL CHECK (entity_id > 0),"
    "closure_status TEXT NOT NULL CHECK (length(trim(closure_status)) > 0),"
    "resolution TEXT CHECK (resolution IS NULL OR resolution IN ("
    "'completed','canceled','not_planned','replaced','duplicate','discarded'"
    ")),"
    "outcome_summary TEXT NOT NULL CHECK (length(trim(outcome_summary)) > 0),"
    "closing_comment TEXT NOT NULL CHECK (length(trim(closing_comment)) > 0),"
    "lessons_learned TEXT,"
    "open_items_summary TEXT,"
    "followup_needed INTEGER NOT NULL DEFAULT 0 CHECK (followup_needed IN (0, 1)),"
    "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "author TEXT NOT NULL CHECK (length(trim(author)) > 0),"
    "event_id INTEGER NOT NULL,"
    "FOREIGN KEY (event_id) REFERENCES events(id) ON DELETE RESTRICT"
    ");",

    "CREATE TABLE IF NOT EXISTS instructions ("
    "id INTEGER PRIMARY KEY,"
    "entity_type TEXT NOT NULL CHECK (entity_type IN ('plan','phase','task')),"
    "entity_id INTEGER NOT NULL CHECK (entity_id > 0),"
    "instruction_type TEXT NOT NULL DEFAULT 'guidance' CHECK (length(trim(instruction_type)) > 0),"
    "body TEXT NOT NULL CHECK (length(trim(body)) > 0),"
    "author TEXT NOT NULL CHECK (length(trim(author)) > 0),"
    "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "updated_at TEXT,"
    "invalidated_at TEXT,"
    "invalidated_by TEXT"
    ");",

    "CREATE TABLE IF NOT EXISTS task_relations ("
    "id INTEGER PRIMARY KEY,"
    "from_task_id INTEGER NOT NULL,"
    "to_task_id INTEGER NOT NULL,"
    "relation_type TEXT NOT NULL CHECK (relation_type IN ("
    "'blocks','blocked_by','related','duplicates','replaces'"
    ")),"
    "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "created_by TEXT NOT NULL CHECK (length(trim(created_by)) > 0),"
    "notes TEXT,"
    "FOREIGN KEY (from_task_id) REFERENCES tasks(id) ON DELETE RESTRICT,"
    "FOREIGN KEY (to_task_id) REFERENCES tasks(id) ON DELETE RESTRICT,"
    "CHECK (from_task_id <> to_task_id),"
    "UNIQUE (from_task_id, to_task_id, relation_type)"
    ");",

    "CREATE TABLE IF NOT EXISTS workspace_context ("
    "id INTEGER PRIMARY KEY CHECK (id = 1),"
    "active_plan_id INTEGER,"
    "updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "updated_by TEXT NOT NULL CHECK (length(trim(updated_by)) > 0),"
    "FOREIGN KEY (active_plan_id) REFERENCES plans(id) ON DELETE RESTRICT"
    ");",

    "CREATE TABLE IF NOT EXISTS plan_contexts ("
    "plan_id INTEGER PRIMARY KEY,"
    "current_phase_id INTEGER,"
    "current_task_id INTEGER,"
    "updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),"
    "updated_by TEXT NOT NULL CHECK (length(trim(updated_by)) > 0),"
    "FOREIGN KEY (plan_id) REFERENCES plans(id) ON DELETE RESTRICT,"
    "FOREIGN KEY (current_phase_id, plan_id) REFERENCES phases(id, plan_id) "
    "ON DELETE RESTRICT,"
    "FOREIGN KEY (current_task_id, plan_id) REFERENCES tasks(id, plan_id) "
    "ON DELETE RESTRICT"
    ");",

    "CREATE INDEX IF NOT EXISTS idx_tasks_plan ON tasks(plan_id);"
    "CREATE INDEX IF NOT EXISTS idx_tasks_phase ON tasks(phase_id) "
    "WHERE phase_id IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);"
    "CREATE INDEX IF NOT EXISTS idx_tasks_resolution ON tasks(resolution) "
    "WHERE resolution IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS idx_tasks_deferred_until ON tasks(deferred_until) "
    "WHERE deferred_until IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS idx_tasks_origin ON tasks(origin_type);"
    "CREATE INDEX IF NOT EXISTS idx_tasks_assignee ON tasks(assignee) "
    "WHERE assignee IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS idx_tasks_origin_task ON tasks(origin_task_id) "
    "WHERE origin_task_id IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS idx_events_entity "
    "ON events(entity_type, entity_id, event_at);"
    "CREATE INDEX IF NOT EXISTS idx_comments_entity "
    "ON comments(entity_type, entity_id, created_at);"
    "CREATE INDEX IF NOT EXISTS idx_closure_records_entity "
    "ON closure_records(entity_type, entity_id, created_at);"
    "CREATE INDEX IF NOT EXISTS idx_instructions_entity "
    "ON instructions(entity_type, entity_id, created_at);"
    "CREATE INDEX IF NOT EXISTS idx_task_relations_from "
    "ON task_relations(from_task_id, relation_type);"
    "CREATE INDEX IF NOT EXISTS idx_task_relations_to "
    "ON task_relations(to_task_id, relation_type);"
    "CREATE INDEX IF NOT EXISTS idx_tasks_priority ON tasks(priority);"
    "CREATE INDEX IF NOT EXISTS idx_phases_plan_status ON phases(plan_id, status);"
    "CREATE INDEX IF NOT EXISTS idx_events_event_at ON events(event_at);"
    "CREATE INDEX IF NOT EXISTS idx_events_type_at ON events(event_type, event_at);"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_phases_plan_local_seq "
    "ON phases(plan_id, local_seq);"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_tasks_plan_local_seq "
    "ON tasks(plan_id, local_seq);"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_plans_uid ON plans(uid);"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_plans_label ON plans(label);"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_phases_uid ON phases(uid);"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_phases_plan_label ON phases(plan_id, label);"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_tasks_uid ON tasks(uid);"
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_tasks_plan_label ON tasks(plan_id, label);",

    "CREATE TRIGGER IF NOT EXISTS phases_fill_local_seq "
    "AFTER INSERT ON phases "
    "WHEN new.local_seq IS NULL "
    "BEGIN "
    "  UPDATE phases SET local_seq = COALESCE("
    "    (SELECT MAX(local_seq) + 1 FROM phases "
    "       WHERE plan_id = new.plan_id AND id <> new.id), 1) "
    "  WHERE id = new.id; "
    "END;",

    "CREATE TRIGGER IF NOT EXISTS tasks_fill_local_seq "
    "AFTER INSERT ON tasks "
    "WHEN new.local_seq IS NULL "
    "BEGIN "
    "  UPDATE tasks SET local_seq = COALESCE("
    "    (SELECT MAX(local_seq) + 1 FROM tasks "
    "       WHERE plan_id = new.plan_id AND id <> new.id), 1) "
    "  WHERE id = new.id; "
    "END;",

    "CREATE TRIGGER IF NOT EXISTS plans_autogen_code "
    "AFTER INSERT ON plans "
    "WHEN new.code IS NULL "
    "BEGIN "
    "  UPDATE plans SET code = 'P' || id WHERE id = new.id; "
    "END;",
};

static const size_t baseline_schema_sql_count =
    sizeof baseline_schema_sql / sizeof baseline_schema_sql[0];

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

static int has_user_objects(sqlite3 *db, int *found) {
    const char *sql =
        "SELECT 1 FROM sqlite_master "
        "WHERE name NOT LIKE 'sqlite_%' LIMIT 1;";
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("prepare has_user_objects",
                       "rc=%d detail=\"%s\"", rc, sqlite3_errmsg(db));
        return -1;
    }
    rc = sqlite3_step(st);
    *found = (rc == SQLITE_ROW) ? 1 : 0;
    sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        ipman_log_error("step has_user_objects",
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

static int create_baseline_schema(sqlite3 *db, int *out_version) {
    char *err = NULL;
    int highest = highest_embedded_migration_version();
    if (highest == 0) {
        ipman_log_error("no embedded migrations", "");
        return -1;
    }

    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        ipman_log_error("BEGIN baseline failed", "rc=%d detail=\"%s\"",
                       rc, err ? err : "(null)");
        sqlite3_free(err);
        return -1;
    }

    for (size_t i = 0; i < baseline_schema_sql_count; ++i) {
        rc = sqlite3_exec(db, baseline_schema_sql[i], NULL, NULL, &err);
        if (rc != SQLITE_OK) {
            ipman_log_error("baseline schema failed",
                           "statement=%zu rc=%d detail=\"%s\"",
                           i + 1, rc, err ? err : "(null)");
            sqlite3_free(err);
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            return -1;
        }
    }

    for (size_t i = 0; i < ipman_migrations_count; ++i) {
        if (insert_schema_metadata(db, &ipman_migrations[i]) != 0) {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            return -1;
        }
    }

    rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        ipman_log_error("COMMIT baseline failed", "rc=%d detail=\"%s\"",
                       rc, err ? err : "(null)");
        sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
    }

    *out_version = highest;
    ipman_log_info("created schema baseline",
                  "version=%d migrations_recorded=%zu",
                  highest, ipman_migrations_count);
    return 0;
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
    } else {
        int user_objects = 0;
        if (has_user_objects(db, &user_objects) != 0) return -1;
        if (!user_objects) {
            return create_baseline_schema(db, out_version);
        }
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
