/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

/*
 * Workspace and plan context management.
 *
 * ## Design: why separate plan-level and workspace-level cursors
 *
 * The workspace has one active plan (workspace_context.active_plan_id).
 * Each plan has its own saved phase and task cursors (plan_contexts).
 * When plan.activate switches the active plan, the previous plan's
 * phase/task cursors are preserved — re-activating that plan restores
 * them. This avoids losing context when switching between plans.
 *
 * ## Invariants
 *
 * 1. active_plan_id always references a non-terminal plan, or is NULL.
 *    Closing/archiving a plan clears active_plan_id if it was active.
 *
 * 2. current_phase_id, when set, must belong to the active plan and be
 *    non-terminal. Setting a terminal phase as current is rejected.
 *
 * 3. current_task_id, when set, must belong to the active plan and be
 *    non-terminal. If the task has a phase_id, that phase is
 *    auto-synced to current_phase_id. If the task has no phase,
 *    current_phase_id is cleared.
 *
 * 4. When current_phase_id is changed (manually or auto-synced),
 *    current_task_id is nulled if the task no longer belongs to the
 *    new phase. A task without a phase survives any phase change.
 *
 * 5. Setting a task/phase cursor that already matches the current value
 *    is a no-op (returns current context without emitting an event).
 *
 * ## Auto-cleanup
 *
 * Closing a plan does NOT clear its plan_contexts row — only
 * active_plan_id is nulled. The plan's saved phase/task remain for
 * inspection. They are not queryable as "current" because no plan is
 * active.
 */

#include "context_ops.h"
#include "db.h"
#include "env_var_ops.h"
#include "json_helpers.h"
#include "project_ops.h"
#include "phase_ops.h"
#include "task_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    sqlite3_int64 active_plan_id;
    sqlite3_int64 current_phase_id;
    sqlite3_int64 current_task_id;
} context_ids_t;

typedef struct {
    sqlite3_int64 id;
    sqlite3_int64 plan_id;
    sqlite3_int64 phase_id;
    int has_phase_id;
    char status[32];
} task_ref_t;

static int load_task_ref(sqlite3 *db, sqlite3_int64 task_id, task_ref_t *ref);
static int phase_is_terminal(sqlite3 *db, sqlite3_int64 phase_id, int *out);

static int is_blank(const char *value) {
    if (value == NULL) return 1;
    while (*value != '\0') {
        if (*value != ' ' && *value != '\t' &&
            *value != '\n' && *value != '\r') {
            return 0;
        }
        ++value;
    }
    return 1;
}

static int run_sql(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static void bind_optional_text(sqlite3_stmt *stmt, int index, const char *value) {
    if (value == NULL) {
        sqlite3_bind_null(stmt, index);
    } else {
        sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT);
    }
}

static void bind_optional_id(sqlite3_stmt *stmt, int index, sqlite3_int64 value) {
    if (value == 0) {
        sqlite3_bind_null(stmt, index);
    } else {
        sqlite3_bind_int64(stmt, index, value);
    }
}

static int read_plan_selector(cJSON *params,
                              sqlite3_int64 *id_out,
                              const char **code_out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out) {
    cJSON *id = cJSON_GetObjectItemCaseSensitive(params, "id");
    cJSON *code = cJSON_GetObjectItemCaseSensitive(params, "code");
    int has_id = id != NULL && !cJSON_IsNull(id);
    int has_code = code != NULL && !cJSON_IsNull(code);
    if (has_id == has_code) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "exactly one of id or code is required";
        return -1;
    }
    if (has_id) {
        if (!cJSON_IsNumber(id) || id->valuedouble < 1.0) {
            *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
            *err_msg_out = "id must be a positive integer";
            return -1;
        }
        *id_out = (sqlite3_int64)id->valuedouble;
        *code_out = NULL;
        return 0;
    }
    if (!cJSON_IsString(code) || code->valuestring == NULL ||
        is_blank(code->valuestring)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "code must be a non-empty string";
        return -1;
    }
    *id_out = 0;
    *code_out = code->valuestring;
    return 0;
}

static cJSON *plan_from_row(sqlite3_stmt *stmt) {
    cJSON *plan = cJSON_CreateObject();
    if (plan == NULL) return NULL;
    cJSON_AddNumberToObject(plan, "id", (double)sqlite3_column_int64(stmt, 0));
    ipman_json_add_text_or_null(plan, "code", sqlite3_column_text(stmt, 1));
    ipman_json_add_text_or_null(plan, "label", sqlite3_column_text(stmt, 17));
    ipman_json_add_text_or_null(plan, "title", sqlite3_column_text(stmt, 2));
    ipman_json_add_text_or_null(plan, "summary", sqlite3_column_text(stmt, 3));
    ipman_json_add_text_or_null(plan, "description", sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null(plan, "status", sqlite3_column_text(stmt, 5));
    ipman_json_add_text_or_null(plan, "priority", sqlite3_column_text(stmt, 6));
    ipman_json_add_text_or_null(plan, "created_at", sqlite3_column_text(stmt, 7));
    ipman_json_add_text_or_null(plan, "updated_at", sqlite3_column_text(stmt, 8));
    ipman_json_add_text_or_null(plan, "opened_at", sqlite3_column_text(stmt, 9));
    ipman_json_add_text_or_null(plan, "closed_at", sqlite3_column_text(stmt, 10));
    ipman_json_add_text_or_null(plan, "archived_at", sqlite3_column_text(stmt, 11));
    ipman_json_add_text_or_null(plan, "owner", sqlite3_column_text(stmt, 12));
    ipman_json_add_text_or_null(plan, "target_date", sqlite3_column_text(stmt, 13));
    ipman_json_add_tags(plan, "tags", sqlite3_column_text(stmt, 14));
    ipman_json_add_text_or_null(plan, "version_label", sqlite3_column_text(stmt, 15));
    return plan;
}

static cJSON *load_plan_by_ref(sqlite3 *db,
                               sqlite3_int64 plan_id,
                               const char *code) {
    const char *sql_by_id =
        "SELECT id, code, title, summary, description, status, priority, "
        "created_at, updated_at, opened_at, closed_at, archived_at, owner, "
        "target_date, tags, version_label, uid, label "
        "FROM plans WHERE id = ?;";
    const char *sql_by_code =
        "SELECT id, code, title, summary, description, status, priority, "
        "created_at, updated_at, opened_at, closed_at, archived_at, owner, "
        "target_date, tags, version_label, uid, label "
        "FROM plans WHERE code = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, code == NULL ? sql_by_id : sql_by_code,
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    if (code == NULL) {
        sqlite3_bind_int64(stmt, 1, plan_id);
    } else {
        sqlite3_bind_text(stmt, 1, code, -1, SQLITE_TRANSIENT);
    }
    rc = sqlite3_step(stmt);
    cJSON *plan = NULL;
    if (rc == SQLITE_ROW) plan = plan_from_row(stmt);
    sqlite3_finalize(stmt);
    return plan;
}

static cJSON *load_plan(sqlite3 *db, sqlite3_int64 plan_id) {
    return load_plan_by_ref(db, plan_id, NULL);
}

static cJSON *load_phase(sqlite3 *db, sqlite3_int64 phase_id) {
    const char *sql =
        "SELECT id, plan_id, title, summary, description, status, sequence_no, "
        "created_at, updated_at, opened_at, closed_at, owner, "
        "target_start_date, target_end_date, local_seq, uid, label "
        "FROM phases WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, phase_id);
    rc = sqlite3_step(stmt);
    cJSON *phase = NULL;
    if (rc == SQLITE_ROW) phase = ipman_phase_from_row(stmt);
    sqlite3_finalize(stmt);
    return phase;
}

static cJSON *load_task(sqlite3 *db, sqlite3_int64 task_id) {
    const char *sql =
        "SELECT id, plan_id, phase_id, parent_task_id, title, summary, "
        "description, status, resolution, priority, task_type, origin_type, "
        "assignee, created_at, updated_at, started_at, closed_at, "
        "deferred_until, blocked_reason, reason_code, reason_text, due_date, "
        "target_start_date, estimate, origin_ref_type, origin_ref_id, "
        "origin_task_id, local_seq, uid, label "
        "FROM tasks WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, task_id);
    rc = sqlite3_step(stmt);
    cJSON *task = NULL;
    if (rc == SQLITE_ROW) task = ipman_task_from_row(stmt);
    sqlite3_finalize(stmt);
    return task;
}

static sqlite3_int64 json_int64(cJSON *object, const char *field) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, field);
    if (!cJSON_IsNumber(item)) return 0;
    return (sqlite3_int64)item->valuedouble;
}

static const char *json_string(cJSON *object, const char *field) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, field);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int load_context_ids(sqlite3 *db, context_ids_t *ids) {
    const char *sql =
        "SELECT wc.active_plan_id, pc.current_phase_id, pc.current_task_id "
        "FROM (SELECT 1) seed "
        "LEFT JOIN workspace_context wc ON wc.id = 1 "
        "LEFT JOIN plan_contexts pc ON pc.plan_id = wc.active_plan_id;";
    sqlite3_stmt *stmt = NULL;
    memset(ids, 0, sizeof *ids);
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            ids->active_plan_id = sqlite3_column_int64(stmt, 0);
        }
        if (sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
            ids->current_phase_id = sqlite3_column_int64(stmt, 1);
        }
        if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
            ids->current_task_id = sqlite3_column_int64(stmt, 2);
        }
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int require_active_plan(sqlite3 *db,
                               sqlite3_int64 *plan_id_out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out) {
    context_ids_t ids;
    if (load_context_ids(db, &ids) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to read workspace context";
        return -1;
    }
    if (ids.active_plan_id == 0) {
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "no active plan";
        return -1;
    }
    *plan_id_out = ids.active_plan_id;
    return 0;
}

static cJSON *load_active_tools(sqlite3 *db) {
    const char *sql =
        "SELECT id, name, version_constraint, purpose, install_hint, "
        "required, author, created_at, updated_at "
        "FROM tools WHERE invalidated_at IS NULL "
        "ORDER BY id ASC;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *row = cJSON_CreateObject();
        if (row == NULL) {
            cJSON_Delete(array);
            sqlite3_finalize(stmt);
            return NULL;
        }
        cJSON_AddNumberToObject(row, "id",
                                (double)sqlite3_column_int64(stmt, 0));
        ipman_json_add_text_or_null(row, "name", sqlite3_column_text(stmt, 1));
        ipman_json_add_text_or_null(row, "version_constraint",
                                    sqlite3_column_text(stmt, 2));
        ipman_json_add_text_or_null(row, "purpose",
                                    sqlite3_column_text(stmt, 3));
        ipman_json_add_text_or_null(row, "install_hint",
                                    sqlite3_column_text(stmt, 4));
        cJSON_AddBoolToObject(row, "required",
                              sqlite3_column_int(stmt, 5) ? 1 : 0);
        ipman_json_add_text_or_null(row, "author", sqlite3_column_text(stmt, 6));
        ipman_json_add_text_or_null(row, "created_at",
                                    sqlite3_column_text(stmt, 7));
        ipman_json_add_text_or_null(row, "updated_at",
                                    sqlite3_column_text(stmt, 8));
        cJSON_AddItemToArray(array, row);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(array);
        return NULL;
    }
    return array;
}

static cJSON *load_active_env_vars(sqlite3 *db, int reveal) {
    const char *sql =
        "SELECT id, name, purpose, example, sensitive, required, "
        "author, created_at, updated_at, invalidated_at, invalidated_by "
        "FROM env_vars WHERE invalidated_at IS NULL "
        "ORDER BY id ASC;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *row = ipman_env_var_row_to_json(stmt, reveal);
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

static cJSON *load_context_with_requirements(sqlite3 *db, int reveal_env);

static cJSON *load_context(sqlite3 *db) {
    return load_context_with_requirements(db, /*reveal_env=*/0);
}

static cJSON *load_context_with_requirements(sqlite3 *db, int reveal_env) {
    const char *sql =
        "SELECT wc.active_plan_id, wc.updated_at, wc.updated_by, "
        "pc.current_phase_id, pc.current_task_id, pc.updated_at, pc.updated_by "
        "FROM (SELECT 1) seed "
        "LEFT JOIN workspace_context wc ON wc.id = 1 "
        "LEFT JOIN plan_contexts pc ON pc.plan_id = wc.active_plan_id;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    sqlite3_int64 active_plan_id = 0;
    sqlite3_int64 current_phase_id = 0;
    sqlite3_int64 current_task_id = 0;
    if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        active_plan_id = sqlite3_column_int64(stmt, 0);
    }
    if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
        current_phase_id = sqlite3_column_int64(stmt, 3);
    }
    if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
        current_task_id = sqlite3_column_int64(stmt, 4);
    }

    cJSON *context = cJSON_CreateObject();
    if (context == NULL) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    if (active_plan_id == 0) {
        cJSON_AddNullToObject(context, "active_plan_id");
    } else {
        cJSON_AddNumberToObject(context, "active_plan_id", (double)active_plan_id);
    }
    if (current_phase_id == 0) {
        cJSON_AddNullToObject(context, "current_phase_id");
    } else {
        cJSON_AddNumberToObject(context, "current_phase_id",
                                (double)current_phase_id);
    }
    if (current_task_id == 0) {
        cJSON_AddNullToObject(context, "current_task_id");
    } else {
        cJSON_AddNumberToObject(context, "current_task_id",
                                (double)current_task_id);
    }
    ipman_json_add_text_or_null(context, "workspace_updated_at", sqlite3_column_text(stmt, 1));
    ipman_json_add_text_or_null(context, "workspace_updated_by", sqlite3_column_text(stmt, 2));
    ipman_json_add_text_or_null(context, "plan_context_updated_at", sqlite3_column_text(stmt, 5));
    ipman_json_add_text_or_null(context, "plan_context_updated_by", sqlite3_column_text(stmt, 6));
    sqlite3_finalize(stmt);

    cJSON *plan = NULL;
    cJSON *phase = NULL;
    cJSON *task = NULL;
    if (active_plan_id != 0) {
        plan = load_plan(db, active_plan_id);
        if (plan == NULL) {
            cJSON_Delete(context);
            return NULL;
        }
    }
    if (current_phase_id != 0) {
        phase = load_phase(db, current_phase_id);
        if (phase == NULL) {
            if (plan != NULL) cJSON_Delete(plan);
            cJSON_Delete(context);
            return NULL;
        }
    }
    if (current_task_id != 0) {
        task = load_task(db, current_task_id);
        if (task == NULL) {
            if (plan != NULL) cJSON_Delete(plan);
            if (phase != NULL) cJSON_Delete(phase);
            cJSON_Delete(context);
            return NULL;
        }
    }

    if (plan == NULL) cJSON_AddNullToObject(context, "active_plan");
    else cJSON_AddItemToObject(context, "active_plan", plan);
    if (phase == NULL) cJSON_AddNullToObject(context, "current_phase");
    else cJSON_AddItemToObject(context, "current_phase", phase);
    if (task == NULL) cJSON_AddNullToObject(context, "current_task");
    else cJSON_AddItemToObject(context, "current_task", task);

    /* Project block: workspace-level metadata + the tools and env_vars
     * the project requires. Always present (single-row table guarantees
     * the project row exists from the moment migration 0004 runs). */
    cJSON *project = ipman_project_load(db);
    cJSON *tools = load_active_tools(db);
    cJSON *env_vars = load_active_env_vars(db, reveal_env);
    if (project == NULL || tools == NULL || env_vars == NULL) {
        if (project != NULL) cJSON_Delete(project);
        if (tools != NULL) cJSON_Delete(tools);
        if (env_vars != NULL) cJSON_Delete(env_vars);
        cJSON_Delete(context);
        return NULL;
    }
    cJSON_AddItemToObject(project, "tools", tools);
    cJSON_AddItemToObject(project, "env_vars", env_vars);
    cJSON_AddItemToObject(context, "project", project);
    return context;
}

static cJSON *result_with_context(cJSON *context) {
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) return NULL;
    cJSON_AddItemToObject(result, "context", context);
    return result;
}

static int ensure_plan_context(sqlite3 *db,
                               sqlite3_int64 plan_id,
                               const char *actor) {
    const char *sql =
        "INSERT INTO plan_contexts(plan_id, updated_by) VALUES(?, ?) "
        "ON CONFLICT(plan_id) DO NOTHING;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, plan_id);
    sqlite3_bind_text(stmt, 2, actor, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int upsert_workspace_context(sqlite3 *db,
                                    sqlite3_int64 active_plan_id,
                                    const char *actor) {
    const char *sql =
        "INSERT INTO workspace_context(id, active_plan_id, updated_by) "
        "VALUES(1, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "active_plan_id = excluded.active_plan_id, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "updated_by = excluded.updated_by;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    bind_optional_id(stmt, 1, active_plan_id);
    sqlite3_bind_text(stmt, 2, actor, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int insert_context_event(sqlite3 *db,
                                sqlite3_int64 plan_id,
                                const char *event_type,
                                const char *actor,
                                const char *request_id,
                                const char *summary,
                                const char *details,
                                const char *old_json,
                                const char *new_json,
                                const char *related_entity_type,
                                sqlite3_int64 related_entity_id) {
    const char *sql =
        "INSERT INTO events("
        "entity_type, entity_id, event_type, actor, summary, details, "
        "old_value, new_value, related_entity_type, related_entity_id, "
        "request_id"
        ") VALUES ('plan', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, plan_id);
    sqlite3_bind_text(stmt, 2, event_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, summary, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 5, details);
    bind_optional_text(stmt, 6, old_json);
    bind_optional_text(stmt, 7, new_json);
    bind_optional_text(stmt, 8, related_entity_type);
    bind_optional_id(stmt, 9, related_entity_id);
    sqlite3_bind_text(stmt, 10, request_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int emit_context_event(sqlite3 *db,
                              const ipman_request_t *req,
                              sqlite3_int64 plan_id,
                              const char *event_type,
                              const char *summary,
                              const char *details,
                              cJSON *old_context,
                              cJSON *new_context,
                              const char *related_entity_type,
                              sqlite3_int64 related_entity_id,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out) {
    char *old_json = cJSON_PrintUnformatted(old_context);
    char *new_json = cJSON_PrintUnformatted(new_context);
    if (old_json == NULL || new_json == NULL) {
        if (old_json != NULL) cJSON_free(old_json);
        if (new_json != NULL) cJSON_free(new_json);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to serialize context event";
        return -1;
    }
    int rc = insert_context_event(db, plan_id, event_type, req->actor,
                                  req->request_id, summary, details,
                                  old_json, new_json, related_entity_type,
                                  related_entity_id);
    cJSON_free(old_json);
    cJSON_free(new_json);
    if (rc != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record context event";
        return -1;
    }
    return 0;
}

static int finish_with_context(sqlite3 *db,
                               cJSON **result_out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out) {
    cJSON *context = load_context(db);
    cJSON *result = NULL;
    if (context == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    result = result_with_context(context);
    if (result == NULL) {
        cJSON_Delete(context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    if (run_sql(db, "COMMIT;") != 0) {
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to commit workspace context";
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_workspace_context_get_params[] = {
    { "reveal" },
    { NULL },
};

int ipman_op_workspace_context_get(const ipman_request_t *req, sqlite3 *db,
                                  cJSON **result_out,
                                  ipman_error_code_t *err_code_out,
                                  const char **err_msg_out) {
    int reveal = 0;
    cJSON *reveal_item = cJSON_GetObjectItemCaseSensitive(req->params, "reveal");
    if (reveal_item != NULL && !cJSON_IsNull(reveal_item)) {
        if (!cJSON_IsBool(reveal_item)) {
            *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
            *err_msg_out = "reveal must be true or false";
            return -1;
        }
        reveal = cJSON_IsTrue(reveal_item) ? 1 : 0;
    }
    cJSON *context = load_context_with_requirements(db, reveal);
    if (context == NULL) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    cJSON *result = result_with_context(context);
    if (result == NULL) {
        cJSON_Delete(context);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_plan_activate_params[] = {
    { "id" }, { "code" },
    { NULL },
};

int ipman_op_plan_activate(const ipman_request_t *req, sqlite3 *db,
                          cJSON **result_out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out) {
    sqlite3_int64 selector_id = 0;
    const char *code = NULL;
    if (read_plan_selector(req->params, &selector_id, &code,
                           err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    cJSON *plan = load_plan_by_ref(db, selector_id, code);
    if (plan == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "plan not found";
        return -1;
    }
    const char *status = json_string(plan, "status");
    if (status == NULL ||
        strcmp(status, "completed") == 0 ||
        strcmp(status, "canceled") == 0 ||
        strcmp(status, "archived") == 0) {
        cJSON_Delete(plan);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "cannot activate a terminal plan";
        return -1;
    }
    sqlite3_int64 plan_id = json_int64(plan, "id");
    cJSON_Delete(plan);

    context_ids_t old_ids;
    if (load_context_ids(db, &old_ids) != 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to read workspace context";
        return -1;
    }
    cJSON *old_context = load_context(db);
    if (old_context == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    if (ensure_plan_context(db, plan_id, req->actor) != 0 ||
        (old_ids.active_plan_id != plan_id &&
         upsert_workspace_context(db, plan_id, req->actor) != 0)) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to activate plan";
        return -1;
    }
    if (ipman_context_repair_plan_cursor(db, req, plan_id,
                                        "{\"op\":\"plan.activate\"}",
                                        err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        return -1;
    }

    cJSON *new_context = load_context(db);
    if (new_context == NULL) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    if (old_ids.active_plan_id != plan_id &&
        emit_context_event(db, req, plan_id, "plan_activated",
                           "plan activated",
                           "{\"op\":\"plan.activate\"}",
                           old_context, new_context, NULL, 0,
                           err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_context);
        cJSON_Delete(new_context);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    cJSON_Delete(old_context);
    cJSON_Delete(new_context);
    return finish_with_context(db, result_out, err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_plan_deactivate_params[] = {
    { NULL },
};

int ipman_op_plan_deactivate(const ipman_request_t *req, sqlite3 *db,
                            cJSON **result_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out) {
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    context_ids_t old_ids;
    if (load_context_ids(db, &old_ids) != 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to read workspace context";
        return -1;
    }
    cJSON *old_context = load_context(db);
    if (old_context == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    if (old_ids.active_plan_id == 0) {
        cJSON_Delete(old_context);
        return finish_with_context(db, result_out, err_code_out, err_msg_out);
    }
    if (upsert_workspace_context(db, 0, req->actor) != 0) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to deactivate plan";
        return -1;
    }
    cJSON *new_context = load_context(db);
    if (new_context == NULL) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    if (emit_context_event(db, req, old_ids.active_plan_id, "plan_deactivated",
                           "plan deactivated",
                           "{\"op\":\"plan.deactivate\"}",
                           old_context, new_context, NULL, 0,
                           err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_context);
        cJSON_Delete(new_context);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    cJSON_Delete(old_context);
    cJSON_Delete(new_context);
    return finish_with_context(db, result_out, err_code_out, err_msg_out);
}

static int update_current_phase(sqlite3 *db,
                                sqlite3_int64 plan_id,
                                sqlite3_int64 phase_id,
                                const char *actor) {
    const char *sql =
        "UPDATE plan_contexts SET "
        "current_phase_id = ?, "
        "current_task_id = CASE "
        "    WHEN ? IS NULL THEN CASE "
        "        WHEN current_task_id IN ("
        "            SELECT id FROM tasks WHERE phase_id IS NOT NULL"
        "        ) THEN NULL ELSE current_task_id END "
        "    WHEN current_task_id IN ("
        "        SELECT id FROM tasks WHERE phase_id = ?"
        "    ) THEN current_task_id ELSE NULL END, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "updated_by = ? "
        "WHERE plan_id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    bind_optional_id(stmt, 1, phase_id);
    bind_optional_id(stmt, 2, phase_id);
    bind_optional_id(stmt, 3, phase_id);
    sqlite3_bind_text(stmt, 4, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, plan_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

const ipman_param_desc_t ipman_op_phase_set_current_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_phase_set_current(const ipman_request_t *req, sqlite3 *db,
                              cJSON **result_out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out) {
    sqlite3_int64 phase_id = 0;
    if (ipman_read_phase_selector(req->params, db, &phase_id,
                                 err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    sqlite3_int64 active_plan_id = 0;
    if (require_active_plan(db, &active_plan_id, err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    cJSON *phase = load_phase(db, phase_id);
    if (phase == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "phase not found";
        return -1;
    }
    if (json_int64(phase, "plan_id") != active_plan_id) {
        cJSON_Delete(phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "phase does not belong to the active plan";
        return -1;
    }
    const char *status = json_string(phase, "status");
    if (status == NULL ||
        strcmp(status, "completed") == 0 ||
        strcmp(status, "canceled") == 0) {
        cJSON_Delete(phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "cannot set a terminal phase as current";
        return -1;
    }
    cJSON_Delete(phase);

    context_ids_t old_ids;
    if (load_context_ids(db, &old_ids) != 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to read workspace context";
        return -1;
    }
    cJSON *old_context = load_context(db);
    if (old_context == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    if (old_ids.current_phase_id == phase_id) {
        cJSON_Delete(old_context);
        return finish_with_context(db, result_out, err_code_out, err_msg_out);
    }
    if (ensure_plan_context(db, active_plan_id, req->actor) != 0 ||
        update_current_phase(db, active_plan_id, phase_id, req->actor) != 0) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to set current phase";
        return -1;
    }
    cJSON *new_context = load_context(db);
    if (new_context == NULL) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    if (emit_context_event(db, req, active_plan_id, "phase_current_changed",
                           "current phase changed",
                           "{\"op\":\"phase.set_current\"}",
                           old_context, new_context, "phase", phase_id,
                           err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_context);
        cJSON_Delete(new_context);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    cJSON_Delete(old_context);
    cJSON_Delete(new_context);
    return finish_with_context(db, result_out, err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_phase_clear_current_params[] = {
    { NULL },
};

int ipman_op_phase_clear_current(const ipman_request_t *req, sqlite3 *db,
                                cJSON **result_out,
                                ipman_error_code_t *err_code_out,
                                const char **err_msg_out) {
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    sqlite3_int64 active_plan_id = 0;
    if (require_active_plan(db, &active_plan_id, err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    context_ids_t old_ids;
    if (load_context_ids(db, &old_ids) != 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to read workspace context";
        return -1;
    }
    cJSON *old_context = load_context(db);
    if (old_context == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    if (old_ids.current_phase_id == 0) {
        cJSON_Delete(old_context);
        return finish_with_context(db, result_out, err_code_out, err_msg_out);
    }
    if (ensure_plan_context(db, active_plan_id, req->actor) != 0 ||
        update_current_phase(db, active_plan_id, 0, req->actor) != 0) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to clear current phase";
        return -1;
    }
    cJSON *new_context = load_context(db);
    if (new_context == NULL) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    if (emit_context_event(db, req, active_plan_id, "phase_current_changed",
                           "current phase changed",
                           "{\"op\":\"phase.clear_current\"}",
                           old_context, new_context, "phase",
                           old_ids.current_phase_id,
                           err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_context);
        cJSON_Delete(new_context);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    cJSON_Delete(old_context);
    cJSON_Delete(new_context);
    return finish_with_context(db, result_out, err_code_out, err_msg_out);
}

static int load_task_ref(sqlite3 *db, sqlite3_int64 task_id, task_ref_t *ref) {
    const char *sql =
        "SELECT id, plan_id, phase_id, status FROM tasks WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    memset(ref, 0, sizeof *ref);
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, task_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        ref->id = sqlite3_column_int64(stmt, 0);
        ref->plan_id = sqlite3_column_int64(stmt, 1);
        if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
            ref->phase_id = sqlite3_column_int64(stmt, 2);
            ref->has_phase_id = 1;
        }
        const unsigned char *status = sqlite3_column_text(stmt, 3);
        if (status != NULL) {
            snprintf(ref->status, sizeof ref->status, "%s",
                     (const char *)status);
        }
        sqlite3_finalize(stmt);
        return 1;
    }
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) return 0;
    return -1;
}

static int phase_is_terminal(sqlite3 *db, sqlite3_int64 phase_id, int *out) {
    const char *sql =
        "SELECT status IN ('completed', 'canceled') FROM phases WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, phase_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return 1;
    }
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) return 0;
    return -1;
}

static int load_saved_plan_cursor(sqlite3 *db,
                                  sqlite3_int64 plan_id,
                                  int *found_out,
                                  sqlite3_int64 *phase_id_out,
                                  sqlite3_int64 *task_id_out) {
    const char *sql =
        "SELECT current_phase_id, current_task_id "
        "FROM plan_contexts WHERE plan_id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, plan_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *found_out = 1;
        *phase_id_out = sqlite3_column_type(stmt, 0) == SQLITE_NULL
            ? 0
            : sqlite3_column_int64(stmt, 0);
        *task_id_out = sqlite3_column_type(stmt, 1) == SQLITE_NULL
            ? 0
            : sqlite3_column_int64(stmt, 1);
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        *found_out = 0;
        *phase_id_out = 0;
        *task_id_out = 0;
        return 0;
    }
    return -1;
}

static int update_saved_plan_cursor(sqlite3 *db,
                                    sqlite3_int64 plan_id,
                                    sqlite3_int64 phase_id,
                                    sqlite3_int64 task_id,
                                    const char *actor) {
    const char *sql =
        "UPDATE plan_contexts SET "
        "current_phase_id = ?, "
        "current_task_id = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "updated_by = ? "
        "WHERE plan_id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    bind_optional_id(stmt, 1, phase_id);
    bind_optional_id(stmt, 2, task_id);
    sqlite3_bind_text(stmt, 3, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, plan_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int ipman_context_repair_plan_cursor(sqlite3 *db,
                                    const ipman_request_t *req,
                                    sqlite3_int64 plan_id,
                                    const char *details,
                                    ipman_error_code_t *err_code_out,
                                    const char **err_msg_out) {
    int found = 0;
    sqlite3_int64 old_phase_id = 0;
    sqlite3_int64 old_task_id = 0;
    if (load_saved_plan_cursor(db, plan_id, &found,
                               &old_phase_id, &old_task_id) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to read saved plan cursor";
        return -1;
    }
    if (!found) return 0;

    sqlite3_int64 new_phase_id = old_phase_id;
    sqlite3_int64 new_task_id = old_task_id;
    task_ref_t current_task_ref;
    int task_loaded = 0;

    if (old_task_id != 0) {
        int task_found = load_task_ref(db, old_task_id, &current_task_ref);
        if (task_found <= 0) {
            new_task_id = 0;
        } else {
            task_loaded = 1;
            if (current_task_ref.plan_id != plan_id ||
                current_task_ref.status[0] == '\0' ||
                strcmp(current_task_ref.status, "done") == 0 ||
                strcmp(current_task_ref.status, "canceled") == 0) {
                new_task_id = 0;
            } else if (current_task_ref.has_phase_id) {
                int terminal = 0;
                int phase_found =
                    phase_is_terminal(db, current_task_ref.phase_id, &terminal);
                if (phase_found <= 0 || terminal) new_task_id = 0;
            }
        }
    }

    if (old_phase_id != 0) {
        int terminal = 0;
        int phase_found = phase_is_terminal(db, old_phase_id, &terminal);
        if (phase_found <= 0 || terminal) {
            new_phase_id = 0;
            if (old_task_id != 0) {
                if (!task_loaded) {
                    int task_found = load_task_ref(db, old_task_id, &current_task_ref);
                    if (task_found > 0) task_loaded = 1;
                }
                if (!task_loaded ||
                    (current_task_ref.has_phase_id &&
                     current_task_ref.phase_id == old_phase_id)) {
                    new_task_id = 0;
                }
            }
        }
    }

    if (old_phase_id == new_phase_id && old_task_id == new_task_id) return 0;

    context_ids_t active_ids;
    if (load_context_ids(db, &active_ids) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to read workspace context";
        return -1;
    }

    cJSON *old_context = NULL;
    cJSON *new_context = NULL;
    int is_active_plan = active_ids.active_plan_id == plan_id;
    if (is_active_plan) {
        old_context = load_context(db);
        if (old_context == NULL) {
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build workspace context";
            return -1;
        }
    }

    if (update_saved_plan_cursor(db, plan_id, new_phase_id, new_task_id,
                                 req->actor) != 0) {
        if (old_context != NULL) cJSON_Delete(old_context);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to repair saved plan cursor";
        return -1;
    }
    if (!is_active_plan) return 0;

    new_context = load_context(db);
    if (new_context == NULL) {
        cJSON_Delete(old_context);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    if (old_phase_id != new_phase_id &&
        emit_context_event(db, req, plan_id, "phase_current_changed",
                           "current phase changed", details,
                           old_context, new_context, "phase", old_phase_id,
                           err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_context);
        cJSON_Delete(new_context);
        return -1;
    }
    if (old_task_id != new_task_id &&
        emit_context_event(db, req, plan_id, "task_current_changed",
                           "current task changed", details,
                           old_context, new_context, "task", old_task_id,
                           err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_context);
        cJSON_Delete(new_context);
        return -1;
    }
    cJSON_Delete(old_context);
    cJSON_Delete(new_context);
    return 0;
}

static int update_current_task(sqlite3 *db,
                               sqlite3_int64 plan_id,
                               sqlite3_int64 task_id,
                               sqlite3_int64 phase_id,
                               int sync_phase,
                               const char *actor) {
    const char *sql =
        "UPDATE plan_contexts SET "
        "current_task_id = ?, "
        "current_phase_id = CASE WHEN ? THEN ? ELSE current_phase_id END, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "updated_by = ? "
        "WHERE plan_id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    bind_optional_id(stmt, 1, task_id);
    sqlite3_bind_int(stmt, 2, sync_phase);
    bind_optional_id(stmt, 3, phase_id);
    sqlite3_bind_text(stmt, 4, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, plan_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

const ipman_param_desc_t ipman_op_task_set_current_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_task_set_current(const ipman_request_t *req, sqlite3 *db,
                             cJSON **result_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    if (ipman_read_task_selector(req->params, db, &task_id,
                                err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    sqlite3_int64 active_plan_id = 0;
    if (require_active_plan(db, &active_plan_id, err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    task_ref_t task_ref;
    int found = load_task_ref(db, task_id, &task_ref);
    if (found <= 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = found == 0 ? IPMAN_ERR_NOT_FOUND : IPMAN_ERR_INTERNAL;
        *err_msg_out = found == 0 ? "task not found" : "failed to read task";
        return -1;
    }
    if (task_ref.plan_id != active_plan_id) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "task does not belong to the active plan";
        return -1;
    }
    if (task_ref.status[0] == '\0' ||
        strcmp(task_ref.status, "done") == 0 ||
        strcmp(task_ref.status, "canceled") == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "cannot set a terminal task as current";
        return -1;
    }
    if (task_ref.has_phase_id) {
        int terminal = 0;
        found = phase_is_terminal(db, task_ref.phase_id, &terminal);
        if (found <= 0 || terminal) {
            run_sql(db, "ROLLBACK;");
            *err_code_out = found < 0 ? IPMAN_ERR_INTERNAL : IPMAN_ERR_CONFLICT;
            *err_msg_out = found < 0 ? "failed to read task phase" :
                                       "cannot set a task in a terminal phase as current";
            return -1;
        }
    }

    context_ids_t old_ids;
    if (load_context_ids(db, &old_ids) != 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to read workspace context";
        return -1;
    }
    cJSON *old_context = load_context(db);
    if (old_context == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    sqlite3_int64 sync_phase_id = task_ref.has_phase_id ? task_ref.phase_id : 0;
    int phase_already_correct = task_ref.has_phase_id
        ? old_ids.current_phase_id == task_ref.phase_id
        : old_ids.current_phase_id == 0;
    if (old_ids.current_task_id == task_id && phase_already_correct) {
        cJSON_Delete(old_context);
        return finish_with_context(db, result_out, err_code_out, err_msg_out);
    }
    if (ensure_plan_context(db, active_plan_id, req->actor) != 0 ||
        update_current_task(db, active_plan_id, task_id, sync_phase_id,
                            1, req->actor) != 0) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to set current task";
        return -1;
    }
    cJSON *new_context = load_context(db);
    if (new_context == NULL) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    if (emit_context_event(db, req, active_plan_id, "task_current_changed",
                           "current task changed",
                           "{\"op\":\"task.set_current\"}",
                           old_context, new_context, "task", task_id,
                           err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_context);
        cJSON_Delete(new_context);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    cJSON_Delete(old_context);
    cJSON_Delete(new_context);
    return finish_with_context(db, result_out, err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_task_clear_current_params[] = {
    { NULL },
};

int ipman_op_task_clear_current(const ipman_request_t *req, sqlite3 *db,
                               cJSON **result_out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out) {
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    sqlite3_int64 active_plan_id = 0;
    if (require_active_plan(db, &active_plan_id, err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    context_ids_t old_ids;
    if (load_context_ids(db, &old_ids) != 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to read workspace context";
        return -1;
    }
    cJSON *old_context = load_context(db);
    if (old_context == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    if (old_ids.current_task_id == 0) {
        cJSON_Delete(old_context);
        return finish_with_context(db, result_out, err_code_out, err_msg_out);
    }
    if (ensure_plan_context(db, active_plan_id, req->actor) != 0 ||
        update_current_task(db, active_plan_id, 0, 0, 0, req->actor) != 0) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to clear current task";
        return -1;
    }
    cJSON *new_context = load_context(db);
    if (new_context == NULL) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build workspace context";
        return -1;
    }
    if (emit_context_event(db, req, active_plan_id, "task_current_changed",
                           "current task changed",
                           "{\"op\":\"task.clear_current\"}",
                           old_context, new_context, "task",
                           old_ids.current_task_id,
                           err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_context);
        cJSON_Delete(new_context);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    cJSON_Delete(old_context);
    cJSON_Delete(new_context);
    return finish_with_context(db, result_out, err_code_out, err_msg_out);
}
