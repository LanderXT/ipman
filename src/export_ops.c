/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "export_ops.h"

#include "json_helpers.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * The export's row builders share the canonical ipman_json_* helpers in
 * json_helpers.{c,h} with the read-side builders in plan_ops/phase_ops/
 * task_ops/comment_ops/event_ops. Both ends serializing through one set
 * of helpers keeps the v2 null-omission convention in lockstep.
 *
 * Canonical guarantees:
 *   - cJSON preserves insertion order, so per-entity key order is fixed.
 *   - Every array query uses ORDER BY id ASC.
 *   - The caller serializes with cJSON_PrintUnformatted, giving stable bytes.
 */

static cJSON *plan_row_to_json(sqlite3_stmt *stmt) {
    cJSON *plan = cJSON_CreateObject();
    if (plan == NULL) return NULL;
    /* Mirrors plan_from_row in plan_ops.c: uid/label first, then numeric id. */
    ipman_json_add_text_or_null(plan, "uid",           sqlite3_column_text(stmt, 16));
    ipman_json_add_text_or_null(plan, "label",         sqlite3_column_text(stmt, 17));
    cJSON_AddNumberToObject(plan, "id",
                            (double)sqlite3_column_int64(stmt, 0));
    ipman_json_add_text_or_null(plan, "code",          sqlite3_column_text(stmt, 1));
    ipman_json_add_text_or_null(plan, "title",         sqlite3_column_text(stmt, 2));
    ipman_json_add_text_or_null(plan, "summary",       sqlite3_column_text(stmt, 3));
    ipman_json_add_text_or_null(plan, "description",   sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null(plan, "status",        sqlite3_column_text(stmt, 5));
    ipman_json_add_text_or_null(plan, "priority",      sqlite3_column_text(stmt, 6));
    ipman_json_add_text_or_null(plan, "created_at",    sqlite3_column_text(stmt, 7));
    ipman_json_add_text_or_null(plan, "updated_at",    sqlite3_column_text(stmt, 8));
    ipman_json_add_text_or_null(plan, "opened_at",     sqlite3_column_text(stmt, 9));
    ipman_json_add_text_or_null(plan, "closed_at",     sqlite3_column_text(stmt, 10));
    ipman_json_add_text_or_null(plan, "archived_at",   sqlite3_column_text(stmt, 11));
    ipman_json_add_text_or_null(plan, "owner",         sqlite3_column_text(stmt, 12));
    ipman_json_add_text_or_null(plan, "target_date",   sqlite3_column_text(stmt, 13));
    ipman_json_add_tags(plan, "tags",          sqlite3_column_text(stmt, 14));
    ipman_json_add_text_or_null(plan, "version_label", sqlite3_column_text(stmt, 15));
    return plan;
}

static cJSON *phase_row_to_json(sqlite3_stmt *stmt) {
    cJSON *phase = cJSON_CreateObject();
    if (phase == NULL) return NULL;
    /* Mirrors ipman_phase_from_row in phase_ops.c. */
    ipman_json_add_text_or_null  (phase, "uid",               sqlite3_column_text(stmt, 14));
    ipman_json_add_text_or_null  (phase, "label",             sqlite3_column_text(stmt, 15));
    cJSON_AddNumberToObject(phase, "id",      (double)sqlite3_column_int64(stmt, 0));
    cJSON_AddNumberToObject(phase, "plan_id", (double)sqlite3_column_int64(stmt, 1));
    ipman_json_add_text_or_null  (phase, "title",             sqlite3_column_text(stmt, 2));
    ipman_json_add_text_or_null  (phase, "summary",           sqlite3_column_text(stmt, 3));
    ipman_json_add_text_or_null  (phase, "description",       sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null  (phase, "status",            sqlite3_column_text(stmt, 5));
    cJSON_AddNumberToObject(phase, "sequence_no",
                            (double)sqlite3_column_int64(stmt, 6));
    cJSON_AddNumberToObject(phase, "local_seq",
                            (double)sqlite3_column_int64(stmt, 16));
    ipman_json_add_text_or_null  (phase, "created_at",        sqlite3_column_text(stmt, 7));
    ipman_json_add_text_or_null  (phase, "updated_at",        sqlite3_column_text(stmt, 8));
    ipman_json_add_text_or_null  (phase, "opened_at",         sqlite3_column_text(stmt, 9));
    ipman_json_add_text_or_null  (phase, "closed_at",         sqlite3_column_text(stmt, 10));
    ipman_json_add_text_or_null  (phase, "owner",             sqlite3_column_text(stmt, 11));
    ipman_json_add_text_or_null  (phase, "target_start_date", sqlite3_column_text(stmt, 12));
    ipman_json_add_text_or_null  (phase, "target_end_date",   sqlite3_column_text(stmt, 13));
    return phase;
}

static cJSON *task_row_to_json(sqlite3_stmt *stmt) {
    cJSON *task = cJSON_CreateObject();
    if (task == NULL) return NULL;
    /* Mirrors ipman_task_from_row in task_ops.c. */
    ipman_json_add_text_or_null  (task, "uid",              sqlite3_column_text(stmt, 27));
    ipman_json_add_text_or_null  (task, "label",            sqlite3_column_text(stmt, 28));
    cJSON_AddNumberToObject(task, "id",      (double)sqlite3_column_int64(stmt, 0));
    cJSON_AddNumberToObject(task, "plan_id", (double)sqlite3_column_int64(stmt, 1));
    ipman_json_add_int64_or_null (task, "phase_id",         stmt, 2);
    ipman_json_add_int64_or_null (task, "parent_task_id",   stmt, 3);
    ipman_json_add_text_or_null  (task, "title",            sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null  (task, "summary",          sqlite3_column_text(stmt, 5));
    ipman_json_add_text_or_null  (task, "description",      sqlite3_column_text(stmt, 6));
    ipman_json_add_text_or_null  (task, "status",           sqlite3_column_text(stmt, 7));
    ipman_json_add_text_or_null  (task, "resolution",       sqlite3_column_text(stmt, 8));
    ipman_json_add_text_or_null  (task, "priority",         sqlite3_column_text(stmt, 9));
    ipman_json_add_text_or_null  (task, "task_type",        sqlite3_column_text(stmt, 10));
    ipman_json_add_text_or_null  (task, "origin_type",      sqlite3_column_text(stmt, 11));
    ipman_json_add_text_or_null  (task, "assignee",         sqlite3_column_text(stmt, 12));
    ipman_json_add_text_or_null  (task, "created_at",       sqlite3_column_text(stmt, 13));
    ipman_json_add_text_or_null  (task, "updated_at",       sqlite3_column_text(stmt, 14));
    ipman_json_add_text_or_null  (task, "started_at",       sqlite3_column_text(stmt, 15));
    ipman_json_add_text_or_null  (task, "closed_at",        sqlite3_column_text(stmt, 16));
    ipman_json_add_text_or_null  (task, "deferred_until",   sqlite3_column_text(stmt, 17));
    ipman_json_add_text_or_null  (task, "blocked_reason",   sqlite3_column_text(stmt, 18));
    ipman_json_add_text_or_null  (task, "reason_code",      sqlite3_column_text(stmt, 19));
    ipman_json_add_text_or_null  (task, "reason_text",      sqlite3_column_text(stmt, 20));
    ipman_json_add_text_or_null  (task, "due_date",         sqlite3_column_text(stmt, 21));
    ipman_json_add_text_or_null  (task, "target_start_date",sqlite3_column_text(stmt, 22));
    ipman_json_add_text_or_null  (task, "estimate",         sqlite3_column_text(stmt, 23));
    ipman_json_add_text_or_null  (task, "origin_ref_type",  sqlite3_column_text(stmt, 24));
    ipman_json_add_int64_or_null (task, "origin_ref_id",    stmt, 25);
    ipman_json_add_int64_or_null (task, "origin_task_id",   stmt, 26);
    cJSON_AddNumberToObject(task, "local_seq",
                            (double)sqlite3_column_int64(stmt, 29));
    return task;
}

static cJSON *comment_row_to_json(sqlite3_stmt *stmt) {
    cJSON *c = cJSON_CreateObject();
    if (c == NULL) return NULL;
    cJSON_AddNumberToObject(c, "id",         (double)sqlite3_column_int64(stmt, 0));
    ipman_json_add_text_or_null  (c, "entity_type",     sqlite3_column_text(stmt, 1));
    cJSON_AddNumberToObject(c, "entity_id",  (double)sqlite3_column_int64(stmt, 2));
    ipman_json_add_text_or_null  (c, "comment_type",    sqlite3_column_text(stmt, 3));
    ipman_json_add_text_or_null  (c, "body",            sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null  (c, "author",          sqlite3_column_text(stmt, 5));
    ipman_json_add_text_or_null  (c, "created_at",      sqlite3_column_text(stmt, 6));
    ipman_json_add_text_or_null  (c, "updated_at",      sqlite3_column_text(stmt, 7));
    ipman_json_add_text_or_null  (c, "invalidated_at",  sqlite3_column_text(stmt, 8));
    ipman_json_add_text_or_null  (c, "invalidated_by",  sqlite3_column_text(stmt, 9));
    return c;
}

static cJSON *instruction_row_to_json(sqlite3_stmt *stmt) {
    cJSON *i = cJSON_CreateObject();
    if (i == NULL) return NULL;
    cJSON_AddNumberToObject(i, "id",         (double)sqlite3_column_int64(stmt, 0));
    ipman_json_add_text_or_null  (i, "entity_type",      sqlite3_column_text(stmt, 1));
    cJSON_AddNumberToObject(i, "entity_id",  (double)sqlite3_column_int64(stmt, 2));
    ipman_json_add_text_or_null  (i, "instruction_type", sqlite3_column_text(stmt, 3));
    ipman_json_add_text_or_null  (i, "body",             sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null  (i, "author",           sqlite3_column_text(stmt, 5));
    ipman_json_add_text_or_null  (i, "created_at",       sqlite3_column_text(stmt, 6));
    ipman_json_add_text_or_null  (i, "updated_at",       sqlite3_column_text(stmt, 7));
    ipman_json_add_text_or_null  (i, "invalidated_at",   sqlite3_column_text(stmt, 8));
    ipman_json_add_text_or_null  (i, "invalidated_by",   sqlite3_column_text(stmt, 9));
    return i;
}

static cJSON *event_row_to_json(sqlite3_stmt *stmt) {
    cJSON *e = cJSON_CreateObject();
    if (e == NULL) return NULL;
    cJSON_AddNumberToObject(e, "id",         (double)sqlite3_column_int64(stmt, 0));
    ipman_json_add_text_or_null  (e, "entity_type",     sqlite3_column_text(stmt, 1));
    cJSON_AddNumberToObject(e, "entity_id",  (double)sqlite3_column_int64(stmt, 2));
    ipman_json_add_text_or_null  (e, "event_type",      sqlite3_column_text(stmt, 3));
    ipman_json_add_text_or_null  (e, "actor",           sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null  (e, "event_at",        sqlite3_column_text(stmt, 5));
    ipman_json_add_text_or_null  (e, "summary",         sqlite3_column_text(stmt, 6));
    ipman_json_add_json_or_null  (e, "details",         sqlite3_column_text(stmt, 7));
    ipman_json_add_json_or_null  (e, "old_value",       sqlite3_column_text(stmt, 8));
    ipman_json_add_json_or_null  (e, "new_value",       sqlite3_column_text(stmt, 9));
    ipman_json_add_text_or_null  (e, "related_entity_type", sqlite3_column_text(stmt, 10));
    ipman_json_add_int64_or_null (e, "related_entity_id",   stmt, 11);
    ipman_json_add_text_or_null  (e, "request_id",      sqlite3_column_text(stmt, 12));
    return e;
}

static cJSON *closure_row_to_json(sqlite3_stmt *stmt) {
    cJSON *c = cJSON_CreateObject();
    if (c == NULL) return NULL;
    cJSON_AddNumberToObject(c, "id",          (double)sqlite3_column_int64(stmt, 0));
    ipman_json_add_text_or_null  (c, "entity_type",      sqlite3_column_text(stmt, 1));
    cJSON_AddNumberToObject(c, "entity_id",   (double)sqlite3_column_int64(stmt, 2));
    ipman_json_add_text_or_null  (c, "closure_status",   sqlite3_column_text(stmt, 3));
    ipman_json_add_text_or_null  (c, "resolution",       sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null  (c, "outcome_summary",  sqlite3_column_text(stmt, 5));
    ipman_json_add_text_or_null  (c, "closing_comment",  sqlite3_column_text(stmt, 6));
    ipman_json_add_text_or_null  (c, "lessons_learned",  sqlite3_column_text(stmt, 7));
    ipman_json_add_text_or_null  (c, "open_items_summary", sqlite3_column_text(stmt, 8));
    cJSON_AddNumberToObject(c, "followup_needed",
                            (double)sqlite3_column_int64(stmt, 9));
    ipman_json_add_text_or_null  (c, "created_at",       sqlite3_column_text(stmt, 10));
    ipman_json_add_text_or_null  (c, "author",           sqlite3_column_text(stmt, 11));
    ipman_json_add_int64_or_null (c, "event_id",         stmt, 12);
    return c;
}

static cJSON *relation_row_to_json(sqlite3_stmt *stmt) {
    cJSON *r = cJSON_CreateObject();
    if (r == NULL) return NULL;
    cJSON_AddNumberToObject(r, "id",
                            (double)sqlite3_column_int64(stmt, 0));
    cJSON_AddNumberToObject(r, "from_task_id",
                            (double)sqlite3_column_int64(stmt, 1));
    cJSON_AddNumberToObject(r, "to_task_id",
                            (double)sqlite3_column_int64(stmt, 2));
    ipman_json_add_text_or_null(r, "relation_type", sqlite3_column_text(stmt, 3));
    ipman_json_add_text_or_null(r, "created_at",    sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null(r, "created_by",    sqlite3_column_text(stmt, 5));
    ipman_json_add_text_or_null(r, "notes",         sqlite3_column_text(stmt, 6));
    return r;
}

/*
 * Generic single-array loader. Prepares `sql`, binds plan_id to ?1, walks
 * rows, calls `row_to_json` per row. Returns a cJSON array on success or
 * NULL on any failure (caller treats NULL as fatal).
 */
static cJSON *collect_array(sqlite3 *db, const char *sql,
                            sqlite3_int64 plan_id,
                            cJSON *(*row_to_json)(sqlite3_stmt *)) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, plan_id);
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *row = row_to_json(stmt);
        if (row == NULL) {
            cJSON_Delete(array);
            sqlite3_finalize(stmt);
            return NULL;
        }
        cJSON_AddItemToArray(array, row);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(array);
        return NULL;
    }
    return array;
}

static int read_plan_id(cJSON *params, sqlite3_int64 *out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out) {
    if (!cJSON_IsObject(params)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "params must be an object";
        return -1;
    }
    cJSON *raw = cJSON_GetObjectItemCaseSensitive(params, "plan_id");
    if (raw == NULL) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "plan_id is required";
        return -1;
    }
    if (!cJSON_IsNumber(raw)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "plan_id must be a positive integer";
        return -1;
    }
    double v = raw->valuedouble;
    if (v < 1.0 || (double)(sqlite3_int64)v != v) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "plan_id must be a positive integer";
        return -1;
    }
    *out = (sqlite3_int64)v;
    return 0;
}

static int load_schema_version(sqlite3 *db, int *out) {
    const char *sql = "SELECT MAX(version) FROM schema_metadata;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    int v = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW &&
        sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        v = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    *out = v;
    return 0;
}

static void format_now_utc(char *buf, size_t len) {
    /* ISO 8601 "YYYY-MM-DDTHH:MM:SSZ". Used only for the export header
     * (intentionally second-precision; the export data itself preserves the
     * sub-second timestamps the rows carry). */
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

const ipman_param_desc_t ipman_op_plan_export_params[] = {
    { "plan_id" },
    { NULL },
};

int ipman_op_plan_export(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out) {
    sqlite3_int64 plan_id = 0;
    if (read_plan_id(req->params, &plan_id, err_code_out, err_msg_out) != 0) {
        return -1;
    }

    /* Plan: prepare separately so we can distinguish not_found from other
     * errors before doing all the dependent queries. Column order matches
     * load_plan_checked in plan_ops.c so plan_row_to_json can read uid/label
     * at indices 16/17. */
    const char *plan_sql =
        "SELECT id, code, title, summary, description, status, priority, "
        "created_at, updated_at, opened_at, closed_at, archived_at, owner, "
        "target_date, tags, version_label, uid, label "
        "FROM plans WHERE id = ?;";
    sqlite3_stmt *plan_stmt = NULL;
    if (sqlite3_prepare_v2(db, plan_sql, -1, &plan_stmt, NULL) != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to load plan";
        return -1;
    }
    sqlite3_bind_int64(plan_stmt, 1, plan_id);
    int step_rc = sqlite3_step(plan_stmt);
    if (step_rc != SQLITE_ROW) {
        sqlite3_finalize(plan_stmt);
        if (step_rc == SQLITE_DONE) {
            *err_code_out = IPMAN_ERR_NOT_FOUND;
            *err_msg_out = "plan not found";
        } else {
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to load plan";
        }
        return -1;
    }
    cJSON *plan = plan_row_to_json(plan_stmt);
    sqlite3_finalize(plan_stmt);
    if (plan == NULL) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build plan json";
        return -1;
    }

    /* Per-table queries. All ORDER BY id ASC for canonical output.
     * Column tails (uid, label, local_seq) keep the historical 0..N indices
     * stable so the row builders don't have to renumber. */
    const char *phases_sql =
        "SELECT id, plan_id, title, summary, description, status, sequence_no, "
        "created_at, updated_at, opened_at, closed_at, owner, "
        "target_start_date, target_end_date, uid, label, local_seq "
        "FROM phases WHERE plan_id = ? ORDER BY id ASC;";

    const char *tasks_sql =
        "SELECT id, plan_id, phase_id, parent_task_id, title, summary, "
        "description, status, resolution, priority, task_type, origin_type, "
        "assignee, created_at, updated_at, started_at, closed_at, "
        "deferred_until, blocked_reason, reason_code, reason_text, due_date, "
        "target_start_date, estimate, origin_ref_type, origin_ref_id, "
        "origin_task_id, uid, label, local_seq "
        "FROM tasks WHERE plan_id = ? ORDER BY id ASC;";

    /* Comments: scoped to the plan + its phases + its tasks. */
    const char *comments_sql =
        "SELECT id, entity_type, entity_id, comment_type, body, author, "
        "created_at, updated_at, invalidated_at, invalidated_by "
        "FROM comments WHERE "
        "(entity_type = 'plan'  AND entity_id = ?1) OR "
        "(entity_type = 'phase' AND entity_id IN (SELECT id FROM phases WHERE plan_id = ?1)) OR "
        "(entity_type = 'task'  AND entity_id IN (SELECT id FROM tasks  WHERE plan_id = ?1)) "
        "ORDER BY id ASC;";

    const char *instructions_sql =
        "SELECT id, entity_type, entity_id, instruction_type, body, author, "
        "created_at, updated_at, invalidated_at, invalidated_by "
        "FROM instructions WHERE "
        "(entity_type = 'plan'  AND entity_id = ?1) OR "
        "(entity_type = 'phase' AND entity_id IN (SELECT id FROM phases WHERE plan_id = ?1)) OR "
        "(entity_type = 'task'  AND entity_id IN (SELECT id FROM tasks  WHERE plan_id = ?1)) "
        "ORDER BY id ASC;";

    const char *events_sql =
        "SELECT id, entity_type, entity_id, event_type, actor, event_at, "
        "summary, details, old_value, new_value, related_entity_type, "
        "related_entity_id, request_id "
        "FROM events WHERE "
        "(entity_type = 'plan'  AND entity_id = ?1) OR "
        "(entity_type = 'phase' AND entity_id IN (SELECT id FROM phases WHERE plan_id = ?1)) OR "
        "(entity_type = 'task'  AND entity_id IN (SELECT id FROM tasks  WHERE plan_id = ?1)) "
        "ORDER BY id ASC;";

    const char *closures_sql =
        "SELECT id, entity_type, entity_id, closure_status, resolution, "
        "outcome_summary, closing_comment, lessons_learned, "
        "open_items_summary, followup_needed, created_at, author, event_id "
        "FROM closure_records WHERE "
        "(entity_type = 'plan'  AND entity_id = ?1) OR "
        "(entity_type = 'phase' AND entity_id IN (SELECT id FROM phases WHERE plan_id = ?1)) OR "
        "(entity_type = 'task'  AND entity_id IN (SELECT id FROM tasks  WHERE plan_id = ?1)) "
        "ORDER BY id ASC;";

    /* Relations: only those whose endpoints both belong to this plan.
     * Cross-plan relations are intentionally omitted to keep the export
     * boundary closed. */
    const char *relations_sql =
        "SELECT id, from_task_id, to_task_id, relation_type, created_at, "
        "created_by, notes "
        "FROM task_relations WHERE "
        "from_task_id IN (SELECT id FROM tasks WHERE plan_id = ?1) AND "
        "to_task_id   IN (SELECT id FROM tasks WHERE plan_id = ?1) "
        "ORDER BY id ASC;";

    cJSON *phases    = collect_array(db, phases_sql,    plan_id, phase_row_to_json);
    cJSON *tasks     = collect_array(db, tasks_sql,     plan_id, task_row_to_json);
    cJSON *comments  = collect_array(db, comments_sql,  plan_id, comment_row_to_json);
    cJSON *instructions = collect_array(db, instructions_sql, plan_id, instruction_row_to_json);
    cJSON *events    = collect_array(db, events_sql,    plan_id, event_row_to_json);
    cJSON *closures  = collect_array(db, closures_sql,  plan_id, closure_row_to_json);
    cJSON *relations = collect_array(db, relations_sql, plan_id, relation_row_to_json);

    if (phases == NULL || tasks == NULL || comments == NULL ||
        instructions == NULL ||
        events == NULL || closures == NULL || relations == NULL) {
        cJSON_Delete(plan);
        if (phases    != NULL) cJSON_Delete(phases);
        if (tasks     != NULL) cJSON_Delete(tasks);
        if (comments  != NULL) cJSON_Delete(comments);
        if (instructions != NULL) cJSON_Delete(instructions);
        if (events    != NULL) cJSON_Delete(events);
        if (closures  != NULL) cJSON_Delete(closures);
        if (relations != NULL) cJSON_Delete(relations);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to assemble export";
        return -1;
    }

    int schema_version = 0;
    if (load_schema_version(db, &schema_version) != 0) {
        cJSON_Delete(plan);
        cJSON_Delete(phases); cJSON_Delete(tasks); cJSON_Delete(comments);
        cJSON_Delete(instructions);
        cJSON_Delete(events); cJSON_Delete(closures); cJSON_Delete(relations);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to read schema version";
        return -1;
    }

    char generated_at[32];
    format_now_utc(generated_at, sizeof generated_at);

    cJSON *export_obj = cJSON_CreateObject();
    cJSON *result     = cJSON_CreateObject();
    if (export_obj == NULL || result == NULL) {
        if (export_obj != NULL) cJSON_Delete(export_obj);
        if (result     != NULL) cJSON_Delete(result);
        cJSON_Delete(plan);
        cJSON_Delete(phases); cJSON_Delete(tasks); cJSON_Delete(comments);
        cJSON_Delete(instructions);
        cJSON_Delete(events); cJSON_Delete(closures); cJSON_Delete(relations);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build export envelope";
        return -1;
    }

    /* Insertion order is the canonical key order. Don't reorder these.
     *
     * export_format_version 3 (vs. earlier 2):
     *   - Adds instructions.
     *
     * export_format_version 2 (vs. earlier 1):
     *   - plan/phase/task rows now lead with `uid` and `label`, then `id`.
     *   - phase/task rows include `local_seq` (matching the read APIs).
     *   - Optional entity fields with NULL values are omitted from the JSON
     *     entirely (description, closed_at, due_date, …) instead of being
     *     emitted as explicit `null`. Workspace-context sentinels and
     *     response-envelope nulls are unaffected — they carry meaning. */
    cJSON_AddNumberToObject(export_obj, "export_format_version", 3);
    cJSON_AddNumberToObject(export_obj, "schema_version",
                            (double)schema_version);
    cJSON_AddStringToObject(export_obj, "generated_at", generated_at);
    cJSON_AddItemToObject  (export_obj, "plan",      plan);
    cJSON_AddItemToObject  (export_obj, "phases",    phases);
    cJSON_AddItemToObject  (export_obj, "tasks",     tasks);
    cJSON_AddItemToObject  (export_obj, "comments",  comments);
    cJSON_AddItemToObject  (export_obj, "instructions", instructions);
    cJSON_AddItemToObject  (export_obj, "events",    events);
    cJSON_AddItemToObject  (export_obj, "closures",  closures);
    cJSON_AddItemToObject  (export_obj, "relations", relations);

    cJSON_AddItemToObject(result, "export", export_obj);
    *result_out = result;
    return 0;
}
