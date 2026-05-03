/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "tool_ops.h"

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

static int read_required_string(cJSON *params, const char *field,
                                const char **out,
                                ipman_error_code_t *err_code_out,
                                const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, field);
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        is_blank(item->valuestring)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "required text fields must be non-empty strings";
        return -1;
    }
    *out = item->valuestring;
    return 0;
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

static int read_optional_bool_default(cJSON *params, const char *field,
                                      int default_true, int *out,
                                      ipman_error_code_t *err_code_out,
                                      const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, field);
    if (item == NULL || cJSON_IsNull(item)) {
        *out = default_true ? 1 : 0;
        return 0;
    }
    if (!cJSON_IsBool(item)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "boolean fields must be true or false";
        return -1;
    }
    *out = cJSON_IsTrue(item) ? 1 : 0;
    return 0;
}

static int read_optional_bool_present(cJSON *params, const char *field,
                                      int *present_out, int *out,
                                      ipman_error_code_t *err_code_out,
                                      const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, field);
    if (item == NULL || cJSON_IsNull(item)) {
        *present_out = 0;
        return 0;
    }
    if (!cJSON_IsBool(item)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "boolean fields must be true or false";
        return -1;
    }
    *present_out = 1;
    *out = cJSON_IsTrue(item) ? 1 : 0;
    return 0;
}

static cJSON *tool_from_row(sqlite3_stmt *stmt) {
    cJSON *tool = cJSON_CreateObject();
    if (tool == NULL) return NULL;
    cJSON_AddNumberToObject(tool, "id",
                            (double)sqlite3_column_int64(stmt, 0));
    ipman_json_add_text_or_null(tool, "name", sqlite3_column_text(stmt, 1));
    ipman_json_add_text_or_null(tool, "version_constraint",
                                sqlite3_column_text(stmt, 2));
    ipman_json_add_text_or_null(tool, "purpose", sqlite3_column_text(stmt, 3));
    ipman_json_add_text_or_null(tool, "install_hint",
                                sqlite3_column_text(stmt, 4));
    cJSON_AddBoolToObject(tool, "required",
                          sqlite3_column_int(stmt, 5) ? 1 : 0);
    ipman_json_add_text_or_null(tool, "author", sqlite3_column_text(stmt, 6));
    ipman_json_add_text_or_null(tool, "created_at",
                                sqlite3_column_text(stmt, 7));
    ipman_json_add_text_or_null(tool, "updated_at",
                                sqlite3_column_text(stmt, 8));
    ipman_json_add_text_or_null(tool, "invalidated_at",
                                sqlite3_column_text(stmt, 9));
    ipman_json_add_text_or_null(tool, "invalidated_by",
                                sqlite3_column_text(stmt, 10));
    return tool;
}

static int format_tool_details(char *buffer, size_t buffer_size,
                               const char *op, sqlite3_int64 tool_id) {
    int written = snprintf(buffer, buffer_size,
                           "{\"op\":\"%s\",\"tool_id\":%lld}",
                           op, (long long)tool_id);
    return written >= 0 && (size_t)written < buffer_size ? 0 : -1;
}

static int insert_event(sqlite3 *db,
                        const char *event_type, const char *actor,
                        const char *request_id, const char *summary,
                        const char *details, const char *old_json,
                        const char *new_json) {
    /* Tool changes are project-level audit entries. entity_id is fixed at 1
     * because the project table is single-row by construction. */
    const char *sql =
        "INSERT INTO events("
        "entity_type, entity_id, event_type, actor, summary, details, "
        "old_value, new_value, related_entity_type, related_entity_id, "
        "request_id"
        ") VALUES ('project', 1, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, event_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, summary, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 4, details);
    bind_optional_text(stmt, 5, old_json);
    bind_optional_text(stmt, 6, new_json);
    sqlite3_bind_null(stmt, 7);
    sqlite3_bind_null(stmt, 8);
    sqlite3_bind_text(stmt, 9, request_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int insert_tool(sqlite3 *db,
                       const char *name, const char *version_constraint,
                       const char *purpose, const char *install_hint,
                       int required, const char *author,
                       sqlite3_int64 *tool_id_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out) {
    const char *sql =
        "INSERT INTO tools("
        "name, version_constraint, purpose, install_hint, required, author"
        ") VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 2, version_constraint);
    bind_optional_text(stmt, 3, purpose);
    bind_optional_text(stmt, 4, install_hint);
    sqlite3_bind_int(stmt, 5, required ? 1 : 0);
    sqlite3_bind_text(stmt, 6, author, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        *tool_id_out = sqlite3_last_insert_rowid(db);
        return 0;
    }
    if (rc == SQLITE_CONSTRAINT) {
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "another active tool with this name already exists";
        return -1;
    }
    *err_code_out = IPMAN_ERR_INTERNAL;
    *err_msg_out = "failed to add tool";
    return -1;
}

static cJSON *load_tool(sqlite3 *db, sqlite3_int64 tool_id) {
    const char *sql =
        "SELECT id, name, version_constraint, purpose, install_hint, "
        "required, author, created_at, updated_at, invalidated_at, "
        "invalidated_by FROM tools WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, tool_id);
    rc = sqlite3_step(stmt);
    cJSON *tool = NULL;
    if (rc == SQLITE_ROW) tool = tool_from_row(stmt);
    sqlite3_finalize(stmt);
    return tool;
}

static int load_tool_meta(sqlite3 *db, sqlite3_int64 tool_id,
                          int *invalidated_out) {
    const char *sql =
        "SELECT invalidated_at IS NOT NULL FROM tools WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, tool_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *invalidated_out = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return 1;
    }
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) return 0;
    return -1;
}

static int update_tool(sqlite3 *db, sqlite3_int64 tool_id,
                       const char *name, const char *version_constraint,
                       const char *purpose, const char *install_hint,
                       int required_present, int required,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out) {
    /*
     * COALESCE preserves the existing value when the bound parameter is
     * NULL — i.e., the caller did not include that field. There is no
     * way to *clear* an optional field through update; that's intentional
     * for v2.3. Use invalidate + re-add if the shape needs to change.
     */
    const char *sql =
        "UPDATE tools SET "
        "name = COALESCE(?, name), "
        "version_constraint = COALESCE(?, version_constraint), "
        "purpose = COALESCE(?, purpose), "
        "install_hint = COALESCE(?, install_hint), "
        "required = CASE WHEN ? = 1 THEN ? ELSE required END, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ? AND invalidated_at IS NULL;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    bind_optional_text(stmt, 1, name);
    bind_optional_text(stmt, 2, version_constraint);
    bind_optional_text(stmt, 3, purpose);
    bind_optional_text(stmt, 4, install_hint);
    sqlite3_bind_int(stmt, 5, required_present ? 1 : 0);
    sqlite3_bind_int(stmt, 6, required ? 1 : 0);
    sqlite3_bind_int64(stmt, 7, tool_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE && sqlite3_changes(db) == 1) return 0;
    if (rc == SQLITE_CONSTRAINT) {
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "another active tool with this name already exists";
        return -1;
    }
    *err_code_out = IPMAN_ERR_INTERNAL;
    *err_msg_out = "failed to update tool";
    return -1;
}

static int invalidate_tool(sqlite3 *db, sqlite3_int64 tool_id,
                           const char *actor) {
    const char *sql =
        "UPDATE tools SET "
        "invalidated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "invalidated_by = ? "
        "WHERE id = ? AND invalidated_at IS NULL;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, tool_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

static char *json_print_owned(cJSON *item) {
    if (item == NULL) return NULL;
    return cJSON_PrintUnformatted(item);
}

static int commit_result_owned(sqlite3 *db, cJSON **result_io,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out) {
    if (run_sql(db, "COMMIT;") != 0) {
        cJSON_Delete(*result_io);
        *result_io = NULL;
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to commit tool change";
        return -1;
    }
    return 0;
}

const ipman_param_desc_t ipman_op_tool_add_params[] = {
    { "name" }, { "version_constraint" }, { "purpose" },
    { "install_hint" }, { "required" },
    { NULL },
};

int ipman_op_tool_add(const ipman_request_t *req, sqlite3 *db,
                      cJSON **result_out,
                      ipman_error_code_t *err_code_out,
                      const char **err_msg_out) {
    const char *name = NULL;
    const char *version_constraint = NULL;
    const char *purpose = NULL;
    const char *install_hint = NULL;
    int required = 1;
    if (read_required_string(req->params, "name", &name,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "version_constraint",
                             &version_constraint,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "purpose", &purpose,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "install_hint", &install_hint,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_bool_default(req->params, "required", 1, &required,
                                   err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (version_constraint != NULL && is_blank(version_constraint)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "version_constraint must be non-empty when provided";
        return -1;
    }
    if (purpose != NULL && is_blank(purpose)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "purpose must be non-empty when provided";
        return -1;
    }
    if (install_hint != NULL && is_blank(install_hint)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "install_hint must be non-empty when provided";
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    sqlite3_int64 tool_id = 0;
    if (insert_tool(db, name, version_constraint, purpose,
                    install_hint, required, req->actor, &tool_id,
                    err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    cJSON *tool = load_tool(db, tool_id);
    cJSON *result = cJSON_CreateObject();
    char details[160];
    int details_ok = format_tool_details(details, sizeof details,
                                         "tool.add", tool_id);
    if (tool == NULL || result == NULL || details_ok != 0) {
        if (tool != NULL) cJSON_Delete(tool);
        if (result != NULL) cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build tool response";
        return -1;
    }
    if (insert_event(db, "tool_added", req->actor, req->request_id,
                     "tool added", details, NULL, NULL) != 0) {
        cJSON_Delete(tool);
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record tool event";
        return -1;
    }
    cJSON_AddItemToObject(result, "tool", tool);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_tool_list_params[] = {
    { "include_invalidated" },
    { NULL },
};

int ipman_op_tool_list(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out) {
    int include_invalidated = 0;
    if (read_optional_bool_default(req->params, "include_invalidated", 0,
                                   &include_invalidated,
                                   err_code_out, err_msg_out) != 0) {
        return -1;
    }
    const char *sql =
        "SELECT id, name, version_constraint, purpose, install_hint, "
        "required, author, created_at, updated_at, invalidated_at, "
        "invalidated_by "
        "FROM tools "
        "WHERE (? OR invalidated_at IS NULL) "
        "ORDER BY id ASC;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to list tools";
        return -1;
    }
    sqlite3_bind_int(stmt, 1, include_invalidated);

    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();
    if (result == NULL || tools == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (tools != NULL) cJSON_Delete(tools);
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build tool list";
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *tool = tool_from_row(stmt);
        if (tool == NULL) {
            cJSON_Delete(result);
            cJSON_Delete(tools);
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build tool list";
            return -1;
        }
        cJSON_AddItemToArray(tools, tool);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(result);
        cJSON_Delete(tools);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to list tools";
        return -1;
    }
    cJSON_AddItemToObject(result, "tools", tools);
    cJSON_AddBoolToObject(result, "include_invalidated", include_invalidated);
    cJSON_AddBoolToObject(result, "has_more", 0);
    cJSON_AddNumberToObject(result, "total_count",
                            cJSON_GetArraySize(tools));
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_tool_update_params[] = {
    { "id" }, { "name" }, { "version_constraint" }, { "purpose" },
    { "install_hint" }, { "required" },
    { NULL },
};

int ipman_op_tool_update(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out) {
    sqlite3_int64 tool_id = 0;
    const char *name = NULL;
    const char *version_constraint = NULL;
    const char *purpose = NULL;
    const char *install_hint = NULL;
    int required_present = 0;
    int required = 0;
    if (ipman_read_positive_id(req->params, "id", &tool_id,
                              err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "name", &name,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "version_constraint",
                             &version_constraint,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "purpose", &purpose,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "install_hint", &install_hint,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_bool_present(req->params, "required",
                                   &required_present, &required,
                                   err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (name == NULL && version_constraint == NULL && purpose == NULL &&
        install_hint == NULL && !required_present) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "tool.update requires at least one mutable field";
        return -1;
    }
    if (name != NULL && is_blank(name)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "name must be non-empty when provided";
        return -1;
    }
    if (version_constraint != NULL && is_blank(version_constraint)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "version_constraint must be non-empty when provided";
        return -1;
    }
    if (purpose != NULL && is_blank(purpose)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "purpose must be non-empty when provided";
        return -1;
    }
    if (install_hint != NULL && is_blank(install_hint)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "install_hint must be non-empty when provided";
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    int invalidated = 0;
    int meta = load_tool_meta(db, tool_id, &invalidated);
    if (meta == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "tool not found";
        return -1;
    }
    if (meta < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to load tool";
        return -1;
    }
    if (invalidated) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "tool is invalidated";
        return -1;
    }
    cJSON *old_tool = load_tool(db, tool_id);
    char *old_json = json_print_owned(old_tool);
    if (old_tool == NULL || old_json == NULL) {
        if (old_tool != NULL) cJSON_Delete(old_tool);
        if (old_json != NULL) cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build tool event";
        return -1;
    }
    if (update_tool(db, tool_id, name, version_constraint, purpose,
                    install_hint, required_present, required,
                    err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_tool);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    cJSON *new_tool = load_tool(db, tool_id);
    cJSON *result = cJSON_CreateObject();
    char *new_json = json_print_owned(new_tool);
    char details[160];
    int details_ok = format_tool_details(details, sizeof details,
                                         "tool.update", tool_id);
    if (new_tool == NULL || result == NULL || new_json == NULL ||
        details_ok != 0) {
        if (new_tool != NULL) cJSON_Delete(new_tool);
        if (result != NULL) cJSON_Delete(result);
        if (new_json != NULL) cJSON_free(new_json);
        cJSON_Delete(old_tool);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build tool response";
        return -1;
    }
    if (insert_event(db, "tool_updated", req->actor, req->request_id,
                     "tool updated", details, old_json, new_json) != 0) {
        cJSON_Delete(new_tool);
        cJSON_Delete(result);
        cJSON_Delete(old_tool);
        cJSON_free(old_json);
        cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record tool event";
        return -1;
    }
    cJSON_Delete(old_tool);
    cJSON_free(old_json);
    cJSON_free(new_json);
    cJSON_AddItemToObject(result, "tool", new_tool);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_tool_invalidate_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_tool_invalidate(const ipman_request_t *req, sqlite3 *db,
                             cJSON **result_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out) {
    sqlite3_int64 tool_id = 0;
    if (ipman_read_positive_id(req->params, "id", &tool_id,
                              err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    int invalidated = 0;
    int meta = load_tool_meta(db, tool_id, &invalidated);
    if (meta == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "tool not found";
        return -1;
    }
    if (meta < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to load tool";
        return -1;
    }
    if (invalidated) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "tool is already invalidated";
        return -1;
    }
    cJSON *old_tool = load_tool(db, tool_id);
    char *old_json = json_print_owned(old_tool);
    if (old_tool == NULL || old_json == NULL) {
        if (old_tool != NULL) cJSON_Delete(old_tool);
        if (old_json != NULL) cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build tool event";
        return -1;
    }
    if (invalidate_tool(db, tool_id, req->actor) != 0) {
        cJSON_Delete(old_tool);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to invalidate tool";
        return -1;
    }
    cJSON *new_tool = load_tool(db, tool_id);
    cJSON *result = cJSON_CreateObject();
    char *new_json = json_print_owned(new_tool);
    char details[160];
    int details_ok = format_tool_details(details, sizeof details,
                                         "tool.invalidate", tool_id);
    if (new_tool == NULL || result == NULL || new_json == NULL ||
        details_ok != 0) {
        if (new_tool != NULL) cJSON_Delete(new_tool);
        if (result != NULL) cJSON_Delete(result);
        if (new_json != NULL) cJSON_free(new_json);
        cJSON_Delete(old_tool);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build tool response";
        return -1;
    }
    if (insert_event(db, "tool_invalidated", req->actor, req->request_id,
                     "tool invalidated", details, old_json, new_json) != 0) {
        cJSON_Delete(new_tool);
        cJSON_Delete(result);
        cJSON_Delete(old_tool);
        cJSON_free(old_json);
        cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record tool event";
        return -1;
    }
    cJSON_Delete(old_tool);
    cJSON_free(old_json);
    cJSON_free(new_json);
    cJSON_AddItemToObject(result, "tool", new_tool);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}
