/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "project_ops.h"

#include "db.h"
#include "json_helpers.h"
#include "validation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int read_optional_string(cJSON *params, const char *field,
                                const char **out,
                                ipman_error_code_t *err_code_out,
                                const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, field);
    if (item == NULL || cJSON_IsNull(item)) {
        *out = NULL;
        return 0;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "optional text fields must be strings or null";
        return -1;
    }
    *out = item->valuestring;
    return 0;
}

static int read_optional_positive_int(cJSON *params, const char *field,
                                      int default_value, int max_value,
                                      int *out,
                                      ipman_error_code_t *err_code_out,
                                      const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, field);
    if (item == NULL || cJSON_IsNull(item)) {
        *out = default_value;
        return 0;
    }
    if (!cJSON_IsNumber(item)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "numeric fields must be numbers";
        return -1;
    }
    double v = item->valuedouble;
    if (v != (double)(long long)v || v < 1 || v > (double)max_value) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "numeric value out of allowed range";
        return -1;
    }
    *out = (int)v;
    return 0;
}

cJSON *ipman_project_load(sqlite3 *db) {
    const char *sql =
        "SELECT id, name, description, created_at, updated_at "
        "FROM project WHERE id = 1;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    rc = sqlite3_step(stmt);
    cJSON *project = NULL;
    if (rc == SQLITE_ROW) {
        project = cJSON_CreateObject();
        if (project != NULL) {
            cJSON_AddNumberToObject(project, "id",
                                    (double)sqlite3_column_int64(stmt, 0));
            ipman_json_add_text_or_null(project, "name",
                                        sqlite3_column_text(stmt, 1));
            ipman_json_add_text_or_null(project, "description",
                                        sqlite3_column_text(stmt, 2));
            ipman_json_add_text_or_null(project, "created_at",
                                        sqlite3_column_text(stmt, 3));
            ipman_json_add_text_or_null(project, "updated_at",
                                        sqlite3_column_text(stmt, 4));
        }
    }
    sqlite3_finalize(stmt);
    return project;
}

const ipman_param_desc_t ipman_op_project_get_params[] = {
    { NULL },
};

int ipman_op_project_get(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out) {
    (void)req;
    cJSON *project = ipman_project_load(db);
    if (project == NULL) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to load project";
        return -1;
    }
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(project);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build project response";
        return -1;
    }
    cJSON_AddItemToObject(result, "project", project);
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_project_update_params[] = {
    { "name" }, { "description" },
    { NULL },
};

int ipman_op_project_update(const ipman_request_t *req, sqlite3 *db,
                            cJSON **result_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out) {
    const char *name = NULL;
    const char *description = NULL;
    if (read_optional_string(req->params, "name", &name,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "description", &description,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (name == NULL && description == NULL) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "project.update requires name or description";
        return -1;
    }
    if (name != NULL && is_blank(name)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "name must be non-empty when provided";
        return -1;
    }
    if (description != NULL && is_blank(description)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "description must be non-empty when provided";
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    cJSON *old_project = ipman_project_load(db);
    char *old_json = old_project != NULL ? cJSON_PrintUnformatted(old_project) : NULL;
    if (old_project == NULL || old_json == NULL) {
        if (old_project != NULL) cJSON_Delete(old_project);
        if (old_json != NULL) cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to load project";
        return -1;
    }
    /* COALESCE preserves existing values for fields the caller omitted. */
    const char *update_sql =
        "UPDATE project SET "
        "name = COALESCE(?, name), "
        "description = COALESCE(?, description), "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = 1;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, update_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cJSON_Delete(old_project);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to update project";
        return -1;
    }
    bind_optional_text(stmt, 1, name);
    bind_optional_text(stmt, 2, description);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || sqlite3_changes(db) != 1) {
        cJSON_Delete(old_project);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to update project";
        return -1;
    }
    cJSON *new_project = ipman_project_load(db);
    char *new_json = new_project != NULL ? cJSON_PrintUnformatted(new_project) : NULL;
    if (new_project == NULL || new_json == NULL) {
        if (new_project != NULL) cJSON_Delete(new_project);
        if (new_json != NULL) cJSON_free(new_json);
        cJSON_Delete(old_project);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to load updated project";
        return -1;
    }

    /* Audit event. entity_type='project', entity_id=1 (single-row table). */
    const char *event_sql =
        "INSERT INTO events("
        "entity_type, entity_id, event_type, actor, summary, details, "
        "old_value, new_value, related_entity_type, related_entity_id, "
        "request_id"
        ") VALUES ('project', 1, 'project_updated', ?, 'project updated', "
        "NULL, ?, ?, NULL, NULL, ?);";
    sqlite3_stmt *evt = NULL;
    rc = sqlite3_prepare_v2(db, event_sql, -1, &evt, NULL);
    if (rc != SQLITE_OK) {
        cJSON_Delete(new_project);
        cJSON_free(new_json);
        cJSON_Delete(old_project);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record project event";
        return -1;
    }
    sqlite3_bind_text(evt, 1, req->actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(evt, 2, old_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(evt, 3, new_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(evt, 4, req->request_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(evt);
    sqlite3_finalize(evt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(new_project);
        cJSON_free(new_json);
        cJSON_Delete(old_project);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record project event";
        return -1;
    }
    cJSON_Delete(old_project);
    cJSON_free(old_json);
    cJSON_free(new_json);

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(new_project);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build project response";
        return -1;
    }
    cJSON_AddItemToObject(result, "project", new_project);
    if (run_sql(db, "COMMIT;") != 0) {
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to commit project change";
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_project_history_params[] = {
    { "limit" }, { "offset" },
    { NULL },
};

int ipman_op_project_history(const ipman_request_t *req, sqlite3 *db,
                             cJSON **result_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out) {
    int limit = 100;
    int offset = 0;
    if (read_optional_positive_int(req->params, "limit", 100, 500, &limit,
                                   err_code_out, err_msg_out) != 0) {
        return -1;
    }
    cJSON *offset_item = cJSON_GetObjectItemCaseSensitive(req->params, "offset");
    if (offset_item != NULL && !cJSON_IsNull(offset_item)) {
        if (!cJSON_IsNumber(offset_item)) {
            *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
            *err_msg_out = "offset must be a non-negative integer";
            return -1;
        }
        double v = offset_item->valuedouble;
        if (v != (double)(long long)v || v < 0) {
            *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
            *err_msg_out = "offset must be a non-negative integer";
            return -1;
        }
        offset = (int)v;
    }

    /* Project-level activity feed: emits events tied directly to the
     * project (project_updated, tool_*, env_var_*) plus the major plan
     * lifecycle events that are most useful at the workspace level
     * (created, closed, archived, reopened, activated, deactivated).
     * Phase, task, and comment events are not included — those belong to
     * plan.history / event.list with a tighter scope. */
    const char *sql =
        "SELECT id, entity_type, entity_id, event_type, actor, event_at, "
        "summary, details, old_value, new_value, related_entity_type, "
        "related_entity_id, request_id "
        "FROM events "
        "WHERE entity_type = 'project' "
        "   OR (entity_type = 'plan' AND event_type IN ("
        "       'plan_created','plan_closed','plan_archived',"
        "       'plan_reopened','plan_activated','plan_deactivated'"
        "   )) "
        "ORDER BY event_at DESC, id DESC "
        "LIMIT ? OFFSET ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query project history";
        return -1;
    }
    sqlite3_bind_int(stmt, 1, limit);
    sqlite3_bind_int(stmt, 2, offset);

    cJSON *result = cJSON_CreateObject();
    cJSON *events = cJSON_CreateArray();
    if (result == NULL || events == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (events != NULL) cJSON_Delete(events);
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build project history";
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *event = cJSON_CreateObject();
        if (event == NULL) {
            cJSON_Delete(result);
            cJSON_Delete(events);
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build project history";
            return -1;
        }
        cJSON_AddNumberToObject(event, "id",
                                (double)sqlite3_column_int64(stmt, 0));
        ipman_json_add_text_or_null(event, "entity_type",
                                    sqlite3_column_text(stmt, 1));
        cJSON_AddNumberToObject(event, "entity_id",
                                (double)sqlite3_column_int64(stmt, 2));
        ipman_json_add_text_or_null(event, "event_type",
                                    sqlite3_column_text(stmt, 3));
        ipman_json_add_text_or_null(event, "actor",
                                    sqlite3_column_text(stmt, 4));
        ipman_json_add_text_or_null(event, "event_at",
                                    sqlite3_column_text(stmt, 5));
        ipman_json_add_text_or_null(event, "summary",
                                    sqlite3_column_text(stmt, 6));
        ipman_json_add_text_or_null(event, "details",
                                    sqlite3_column_text(stmt, 7));
        ipman_json_add_text_or_null(event, "old_value",
                                    sqlite3_column_text(stmt, 8));
        ipman_json_add_text_or_null(event, "new_value",
                                    sqlite3_column_text(stmt, 9));
        ipman_json_add_text_or_null(event, "related_entity_type",
                                    sqlite3_column_text(stmt, 10));
        if (sqlite3_column_type(stmt, 11) == SQLITE_NULL) {
            cJSON_AddNullToObject(event, "related_entity_id");
        } else {
            cJSON_AddNumberToObject(event, "related_entity_id",
                                    (double)sqlite3_column_int64(stmt, 11));
        }
        ipman_json_add_text_or_null(event, "request_id",
                                    sqlite3_column_text(stmt, 12));
        cJSON_AddItemToArray(events, event);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(result);
        cJSON_Delete(events);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query project history";
        return -1;
    }
    int returned = cJSON_GetArraySize(events);
    cJSON_AddItemToObject(result, "events", events);
    cJSON_AddNumberToObject(result, "limit", limit);
    cJSON_AddNumberToObject(result, "offset", offset);
    cJSON_AddBoolToObject(result, "has_more", returned == limit);
    cJSON_AddNumberToObject(result, "total_count", returned);
    *result_out = result;
    return 0;
}
