/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "comment_ops.h"

#include "db.h"
#include "json_helpers.h"
#include "phase_ops.h"
#include "task_ops.h"
#include "validation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMMENT_EDIT_WINDOW_MINUTES 15.0

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

static int format_comment_details(char *buffer,
                                  size_t buffer_size,
                                  const char *op,
                                  sqlite3_int64 comment_id) {
    int written = snprintf(buffer, buffer_size,
                           "{\"op\":\"%s\",\"comment_id\":%lld}",
                           op, (long long)comment_id);
    return written >= 0 && (size_t)written < buffer_size ? 0 : -1;
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

static int read_optional_bool(cJSON *params, const char *field, int *out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, field);
    if (item == NULL || cJSON_IsNull(item)) {
        *out = 0;
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

static int is_entity_type(const char *value) {
    return strcmp(value, "plan") == 0 ||
           strcmp(value, "phase") == 0 ||
           strcmp(value, "task") == 0;
}

static int read_entity_ref(cJSON *params, const char **entity_type_out,
                           sqlite3_int64 *entity_id_out,
                           ipman_error_code_t *err_code_out,
                           const char **err_msg_out) {
    const char *entity_type = NULL;
    if (read_required_string(params, "entity_type", &entity_type,
                             err_code_out, err_msg_out) != 0 ||
        ipman_read_positive_id(params, "entity_id", entity_id_out,
                         err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (!is_entity_type(entity_type)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "entity_type must be plan, phase, or task";
        return -1;
    }
    *entity_type_out = entity_type;
    return 0;
}

static const char *entity_table(const char *entity_type) {
    if (strcmp(entity_type, "plan") == 0) return "plans";
    if (strcmp(entity_type, "phase") == 0) return "phases";
    if (strcmp(entity_type, "task") == 0) return "tasks";
    return NULL;
}

static int entity_exists(sqlite3 *db, const char *entity_type,
                         sqlite3_int64 entity_id) {
    const char *table = entity_table(entity_type);
    const char *sql = NULL;
    if (table == NULL) return -1;
    if (strcmp(table, "plans") == 0) {
        sql = "SELECT 1 FROM plans WHERE id = ?;";
    } else if (strcmp(table, "phases") == 0) {
        sql = "SELECT 1 FROM phases WHERE id = ?;";
    } else {
        sql = "SELECT 1 FROM tasks WHERE id = ?;";
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, entity_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_ROW) return 1;
    if (rc == SQLITE_DONE) return 0;
    return -1;
}

static int validate_entity_exists(sqlite3 *db, const char *entity_type,
                                  sqlite3_int64 entity_id,
                                  ipman_error_code_t *err_code_out,
                                  const char **err_msg_out) {
    int exists = entity_exists(db, entity_type, entity_id);
    if (exists == 1) return 0;
    if (exists == 0) {
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "entity not found";
        return -1;
    }
    *err_code_out = IPMAN_ERR_INTERNAL;
    *err_msg_out = "failed to check entity";
    return -1;
}

static cJSON *comment_from_row(sqlite3_stmt *stmt) {
    cJSON *comment = cJSON_CreateObject();
    if (comment == NULL) return NULL;
    cJSON_AddNumberToObject(comment, "id",
                            (double)sqlite3_column_int64(stmt, 0));
    ipman_json_add_text_or_null(comment, "entity_type", sqlite3_column_text(stmt, 1));
    cJSON_AddNumberToObject(comment, "entity_id",
                            (double)sqlite3_column_int64(stmt, 2));
    ipman_json_add_text_or_null(comment, "comment_type", sqlite3_column_text(stmt, 3));
    ipman_json_add_text_or_null(comment, "body", sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null(comment, "author", sqlite3_column_text(stmt, 5));
    ipman_json_add_text_or_null(comment, "created_at", sqlite3_column_text(stmt, 6));
    ipman_json_add_text_or_null(comment, "updated_at", sqlite3_column_text(stmt, 7));
    ipman_json_add_text_or_null(comment, "invalidated_at", sqlite3_column_text(stmt, 8));
    ipman_json_add_text_or_null(comment, "invalidated_by", sqlite3_column_text(stmt, 9));
    return comment;
}

static cJSON *closure_from_row(sqlite3_stmt *stmt) {
    cJSON *closure = cJSON_CreateObject();
    if (closure == NULL) return NULL;
    cJSON_AddNumberToObject(closure, "id",
                            (double)sqlite3_column_int64(stmt, 0));
    ipman_json_add_text_or_null(closure, "entity_type", sqlite3_column_text(stmt, 1));
    cJSON_AddNumberToObject(closure, "entity_id",
                            (double)sqlite3_column_int64(stmt, 2));
    ipman_json_add_text_or_null(closure, "closure_status", sqlite3_column_text(stmt, 3));
    ipman_json_add_text_or_null(closure, "resolution", sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null(closure, "outcome_summary", sqlite3_column_text(stmt, 5));
    ipman_json_add_text_or_null(closure, "closing_comment", sqlite3_column_text(stmt, 6));
    ipman_json_add_text_or_null(closure, "lessons_learned", sqlite3_column_text(stmt, 7));
    ipman_json_add_text_or_null(closure, "open_items_summary", sqlite3_column_text(stmt, 8));
    cJSON_AddBoolToObject(closure, "followup_needed",
                          sqlite3_column_int(stmt, 9) ? 1 : 0);
    ipman_json_add_text_or_null(closure, "created_at", sqlite3_column_text(stmt, 10));
    ipman_json_add_text_or_null(closure, "author", sqlite3_column_text(stmt, 11));
    cJSON_AddNumberToObject(closure, "event_id",
                            (double)sqlite3_column_int64(stmt, 12));
    return closure;
}

static int insert_event(sqlite3 *db,
                        const char *entity_type,
                        sqlite3_int64 entity_id,
                        const char *event_type,
                        const char *actor,
                        const char *request_id,
                        const char *summary,
                        const char *details,
                        const char *old_json,
                        const char *new_json) {
    const char *sql =
        "INSERT INTO events("
        "entity_type, entity_id, event_type, actor, summary, details, "
        "old_value, new_value, related_entity_type, related_entity_id, "
        "request_id"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, entity_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, entity_id);
    sqlite3_bind_text(stmt, 3, event_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, summary, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 6, details);
    bind_optional_text(stmt, 7, old_json);
    bind_optional_text(stmt, 8, new_json);
    sqlite3_bind_null(stmt, 9);
    sqlite3_bind_null(stmt, 10);
    sqlite3_bind_text(stmt, 11, request_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int insert_comment(sqlite3 *db,
                          const char *entity_type,
                          sqlite3_int64 entity_id,
                          const char *comment_type,
                          const char *body,
                          const char *author,
                          sqlite3_int64 *comment_id_out) {
    const char *sql =
        "INSERT INTO comments(entity_type, entity_id, comment_type, body, author) "
        "VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, entity_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, entity_id);
    sqlite3_bind_text(stmt, 3, comment_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, body, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, author, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    *comment_id_out = sqlite3_last_insert_rowid(db);
    return 0;
}

static cJSON *load_comment(sqlite3 *db, sqlite3_int64 comment_id) {
    const char *sql =
        "SELECT id, entity_type, entity_id, comment_type, body, author, "
        "created_at, updated_at, invalidated_at, invalidated_by "
        "FROM comments WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, comment_id);
    rc = sqlite3_step(stmt);
    cJSON *comment = NULL;
    if (rc == SQLITE_ROW) comment = comment_from_row(stmt);
    sqlite3_finalize(stmt);
    return comment;
}

static int load_comment_meta(sqlite3 *db, sqlite3_int64 comment_id,
                             char *entity_type_buf, size_t entity_type_size,
                             sqlite3_int64 *entity_id_out,
                             int *invalidated_out,
                             int *editable_out) {
    const char *sql =
        "SELECT entity_type, entity_id, invalidated_at IS NOT NULL, "
        "((julianday('now') - julianday(created_at)) * 24.0 * 60.0) <= ? "
        "FROM comments WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_double(stmt, 1, COMMENT_EDIT_WINDOW_MINUTES);
    sqlite3_bind_int64(stmt, 2, comment_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char *entity_type = sqlite3_column_text(stmt, 0);
        if (entity_type == NULL ||
            strlen((const char *)entity_type) + 1 > entity_type_size) {
            sqlite3_finalize(stmt);
            return -1;
        }
        strcpy(entity_type_buf, (const char *)entity_type);
        *entity_id_out = sqlite3_column_int64(stmt, 1);
        *invalidated_out = sqlite3_column_int(stmt, 2);
        *editable_out = sqlite3_column_int(stmt, 3);
        sqlite3_finalize(stmt);
        return 1;
    }
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) return 0;
    return -1;
}

static int update_comment_body(sqlite3 *db, sqlite3_int64 comment_id,
                               const char *body) {
    const char *sql =
        "UPDATE comments SET body = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ? AND invalidated_at IS NULL;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, body, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, comment_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

static int invalidate_comment(sqlite3 *db, sqlite3_int64 comment_id,
                              const char *actor) {
    const char *sql =
        "UPDATE comments SET "
        "invalidated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "invalidated_by = ? "
        "WHERE id = ? AND invalidated_at IS NULL;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, comment_id);
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
        *err_msg_out = "failed to commit comment change";
        return -1;
    }
    return 0;
}

static int comment_add_common(const ipman_request_t *req,
                              sqlite3 *db,
                              const char *fixed_entity_type,
                              cJSON **result_out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out) {
    const char *entity_type = fixed_entity_type;
    sqlite3_int64 entity_id = 0;
    const char *comment_type = NULL;
    const char *body = NULL;
    if (fixed_entity_type == NULL) {
        if (read_entity_ref(req->params, &entity_type, &entity_id,
                            err_code_out, err_msg_out) != 0) {
            return -1;
        }
    } else if (strcmp(fixed_entity_type, "phase") == 0) {
        if (ipman_read_phase_selector(req->params, db, &entity_id,
                                     err_code_out, err_msg_out) != 0) {
            return -1;
        }
    } else if (strcmp(fixed_entity_type, "task") == 0) {
        if (ipman_read_task_selector(req->params, db, &entity_id,
                                    err_code_out, err_msg_out) != 0) {
            return -1;
        }
    } else if (ipman_read_positive_id(req->params, "id", &entity_id,
                                err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (read_required_string(req->params, "body", &body,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "comment_type", &comment_type,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (comment_type == NULL) comment_type = "general";
    if (is_blank(comment_type)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "comment_type must be non-empty when provided";
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    if (validate_entity_exists(db, entity_type, entity_id,
                               err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    sqlite3_int64 comment_id = 0;
    if (insert_comment(db, entity_type, entity_id, comment_type, body,
                       req->actor, &comment_id) != 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to add comment";
        return -1;
    }
    cJSON *comment = load_comment(db, comment_id);
    cJSON *result = cJSON_CreateObject();
    char details[128];
    int details_ok = format_comment_details(details, sizeof details,
                                            req->op, comment_id);
    if (comment == NULL || result == NULL) {
        if (comment != NULL) cJSON_Delete(comment);
        if (result != NULL) cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build comment response";
        return -1;
    }
    if (details_ok != 0) {
        cJSON_Delete(comment);
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build comment event details";
        return -1;
    }
    if (insert_event(db, entity_type, entity_id, "comment_added", req->actor,
                     req->request_id, "comment added", details, NULL,
                     NULL) != 0) {
        cJSON_Delete(comment);
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record comment event";
        return -1;
    }
    cJSON_AddItemToObject(result, "comment", comment);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_comment_add_params[] = {
    { "entity_type" }, { "entity_id" }, { "body" }, { "comment_type" },
    { NULL },
};

int ipman_op_comment_add(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out) {
    return comment_add_common(req, db, NULL, result_out,
                              err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_plan_comment_add_params[] = {
    { "id" }, { "body" }, { "comment_type" },
    { NULL },
};

int ipman_op_plan_comment_add(const ipman_request_t *req, sqlite3 *db,
                             cJSON **result_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out) {
    return comment_add_common(req, db, "plan", result_out,
                              err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_phase_comment_add_params[] = {
    { "id" }, { "body" }, { "comment_type" },
    { NULL },
};

int ipman_op_phase_comment_add(const ipman_request_t *req, sqlite3 *db,
                              cJSON **result_out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out) {
    return comment_add_common(req, db, "phase", result_out,
                              err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_comment_list_params[] = {
    { "entity_type" }, { "entity_id" }, { "include_invalidated" },
    { NULL },
};

int ipman_op_comment_list(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out) {
    const char *entity_type = NULL;
    sqlite3_int64 entity_id = 0;
    int include_invalidated = 0;
    if (read_entity_ref(req->params, &entity_type, &entity_id,
                        err_code_out, err_msg_out) != 0 ||
        read_optional_bool(req->params, "include_invalidated",
                           &include_invalidated,
                           err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (validate_entity_exists(db, entity_type, entity_id,
                               err_code_out, err_msg_out) != 0) {
        return -1;
    }

    const char *sql =
        "SELECT id, entity_type, entity_id, comment_type, body, author, "
        "created_at, updated_at, invalidated_at, invalidated_by "
        "FROM comments "
        "WHERE entity_type = ? AND entity_id = ? "
        "AND (? OR invalidated_at IS NULL) "
        "ORDER BY id ASC;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to list comments";
        return -1;
    }
    sqlite3_bind_text(stmt, 1, entity_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, entity_id);
    sqlite3_bind_int(stmt, 3, include_invalidated);

    cJSON *result = cJSON_CreateObject();
    cJSON *comments = cJSON_CreateArray();
    if (result == NULL || comments == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (comments != NULL) cJSON_Delete(comments);
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build comment list";
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *comment = comment_from_row(stmt);
        if (comment == NULL) {
            cJSON_Delete(result);
            cJSON_Delete(comments);
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build comment list";
            return -1;
        }
        cJSON_AddItemToArray(comments, comment);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(result);
        cJSON_Delete(comments);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to list comments";
        return -1;
    }
    cJSON_AddItemToObject(result, "comments", comments);
    cJSON_AddBoolToObject(result, "include_invalidated", include_invalidated);
    cJSON_AddBoolToObject(result, "has_more", 0);
    cJSON_AddNumberToObject(result, "total_count", cJSON_GetArraySize(comments));
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_comment_update_params[] = {
    { "id" }, { "body" },
    { NULL },
};

int ipman_op_comment_update(const ipman_request_t *req, sqlite3 *db,
                           cJSON **result_out,
                           ipman_error_code_t *err_code_out,
                           const char **err_msg_out) {
    sqlite3_int64 comment_id = 0;
    const char *body = NULL;
    if (ipman_read_positive_id(req->params, "id", &comment_id,
                         err_code_out, err_msg_out) != 0 ||
        read_required_string(req->params, "body", &body,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    char entity_type[16];
    sqlite3_int64 entity_id = 0;
    int invalidated = 0;
    int editable = 0;
    int meta = load_comment_meta(db, comment_id, entity_type,
                                 sizeof entity_type, &entity_id,
                                 &invalidated, &editable);
    if (meta == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "comment not found";
        return -1;
    }
    if (meta < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to load comment";
        return -1;
    }
    if (invalidated) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "comment is invalidated";
        return -1;
    }
    if (!editable) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "comment edit window has expired";
        return -1;
    }
    cJSON *old_comment = load_comment(db, comment_id);
    char *old_json = json_print_owned(old_comment);
    if (old_comment == NULL || old_json == NULL) {
        if (old_comment != NULL) cJSON_Delete(old_comment);
        if (old_json != NULL) cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build comment event";
        return -1;
    }
    if (update_comment_body(db, comment_id, body) != 0) {
        cJSON_Delete(old_comment);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to update comment";
        return -1;
    }
    cJSON *new_comment = load_comment(db, comment_id);
    cJSON *result = cJSON_CreateObject();
    char *new_json = json_print_owned(new_comment);
    char details[128];
    int details_ok = format_comment_details(details, sizeof details,
                                            "comment.update", comment_id);
    if (new_comment == NULL || result == NULL || new_json == NULL) {
        if (new_comment != NULL) cJSON_Delete(new_comment);
        if (result != NULL) cJSON_Delete(result);
        if (new_json != NULL) cJSON_free(new_json);
        cJSON_Delete(old_comment);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build comment response";
        return -1;
    }
    if (details_ok != 0) {
        cJSON_Delete(new_comment);
        cJSON_Delete(result);
        cJSON_Delete(old_comment);
        cJSON_free(old_json);
        cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build comment event details";
        return -1;
    }
    if (insert_event(db, entity_type, entity_id, "comment_updated",
                     req->actor, req->request_id, "comment updated",
                     details, old_json, new_json) != 0) {
        cJSON_Delete(new_comment);
        cJSON_Delete(result);
        cJSON_Delete(old_comment);
        cJSON_free(old_json);
        cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record comment event";
        return -1;
    }
    cJSON_Delete(old_comment);
    cJSON_free(old_json);
    cJSON_free(new_json);
    cJSON_AddItemToObject(result, "comment", new_comment);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_comment_invalidate_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_comment_invalidate(const ipman_request_t *req, sqlite3 *db,
                               cJSON **result_out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out) {
    sqlite3_int64 comment_id = 0;
    if (ipman_read_positive_id(req->params, "id", &comment_id,
                         err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    char entity_type[16];
    sqlite3_int64 entity_id = 0;
    int invalidated = 0;
    int editable = 0;
    int meta = load_comment_meta(db, comment_id, entity_type,
                                 sizeof entity_type, &entity_id,
                                 &invalidated, &editable);
    (void)editable;
    if (meta == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "comment not found";
        return -1;
    }
    if (meta < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to load comment";
        return -1;
    }
    if (invalidated) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "comment is already invalidated";
        return -1;
    }
    cJSON *old_comment = load_comment(db, comment_id);
    char *old_json = json_print_owned(old_comment);
    if (old_comment == NULL || old_json == NULL) {
        if (old_comment != NULL) cJSON_Delete(old_comment);
        if (old_json != NULL) cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build comment event";
        return -1;
    }
    if (invalidate_comment(db, comment_id, req->actor) != 0) {
        cJSON_Delete(old_comment);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to invalidate comment";
        return -1;
    }
    cJSON *new_comment = load_comment(db, comment_id);
    cJSON *result = cJSON_CreateObject();
    char *new_json = json_print_owned(new_comment);
    char details[128];
    int details_ok = format_comment_details(details, sizeof details,
                                            "comment.invalidate", comment_id);
    if (new_comment == NULL || result == NULL || new_json == NULL) {
        if (new_comment != NULL) cJSON_Delete(new_comment);
        if (result != NULL) cJSON_Delete(result);
        if (new_json != NULL) cJSON_free(new_json);
        cJSON_Delete(old_comment);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build comment response";
        return -1;
    }
    if (details_ok != 0) {
        cJSON_Delete(new_comment);
        cJSON_Delete(result);
        cJSON_Delete(old_comment);
        cJSON_free(old_json);
        cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build comment event details";
        return -1;
    }
    if (insert_event(db, entity_type, entity_id, "comment_invalidated",
                     req->actor, req->request_id, "comment invalidated",
                     details, old_json, new_json) != 0) {
        cJSON_Delete(new_comment);
        cJSON_Delete(result);
        cJSON_Delete(old_comment);
        cJSON_free(old_json);
        cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record comment event";
        return -1;
    }
    cJSON_Delete(old_comment);
    cJSON_free(old_json);
    cJSON_free(new_json);
    cJSON_AddItemToObject(result, "comment", new_comment);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

static int entity_has_active_closure(sqlite3 *db,
                                     const char *entity_type,
                                     sqlite3_int64 entity_id,
                                     int *active_out) {
    const char *sql = NULL;
    if (strcmp(entity_type, "plan") == 0) {
        sql = "SELECT status IN ('completed', 'canceled', 'archived') "
              "FROM plans WHERE id = ?;";
    } else if (strcmp(entity_type, "phase") == 0) {
        sql = "SELECT status IN ('completed', 'canceled') "
              "FROM phases WHERE id = ?;";
    } else {
        sql = "SELECT status IN ('done', 'canceled') "
              "FROM tasks WHERE id = ?;";
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, entity_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *active_out = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_finalize(stmt);
    return -1;
}

const ipman_param_desc_t ipman_op_closure_get_params[] = {
    { "entity_type" }, { "entity_id" },
    { NULL },
};

int ipman_op_closure_get(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out) {
    const char *entity_type = NULL;
    sqlite3_int64 entity_id = 0;
    if (read_entity_ref(req->params, &entity_type, &entity_id,
                        err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (validate_entity_exists(db, entity_type, entity_id,
                               err_code_out, err_msg_out) != 0) {
        return -1;
    }
    const char *sql =
        "SELECT id, entity_type, entity_id, closure_status, resolution, "
        "outcome_summary, closing_comment, lessons_learned, "
        "open_items_summary, followup_needed, created_at, author, event_id "
        "FROM closure_records "
        "WHERE entity_type = ? AND entity_id = ? "
        "ORDER BY id DESC;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to load closure records";
        return -1;
    }
    sqlite3_bind_text(stmt, 1, entity_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, entity_id);
    cJSON *result = cJSON_CreateObject();
    cJSON *closures = cJSON_CreateArray();
    cJSON *active_closure = NULL;
    if (result == NULL || closures == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (closures != NULL) cJSON_Delete(closures);
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build closure response";
        return -1;
    }
    int active_entity = 0;
    if (entity_has_active_closure(db, entity_type, entity_id,
                                  &active_entity) != 0) {
        cJSON_Delete(result);
        cJSON_Delete(closures);
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to inspect entity closure state";
        return -1;
    }
    int first = 1;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *closure = closure_from_row(stmt);
        cJSON *active_copy = NULL;
        if (closure == NULL) {
            cJSON_Delete(result);
            cJSON_Delete(closures);
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build closure response";
            return -1;
        }
        if (first && active_entity) {
            active_copy = cJSON_Duplicate(closure, 1);
            if (active_copy == NULL) {
                cJSON_Delete(closure);
                cJSON_Delete(result);
                cJSON_Delete(closures);
                sqlite3_finalize(stmt);
                *err_code_out = IPMAN_ERR_INTERNAL;
                *err_msg_out = "failed to build closure response";
                return -1;
            }
            active_closure = active_copy;
        }
        cJSON_AddItemToArray(closures, closure);
        first = 0;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        if (active_closure != NULL) cJSON_Delete(active_closure);
        cJSON_Delete(result);
        cJSON_Delete(closures);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to load closure records";
        return -1;
    }
    if (active_closure == NULL) {
        cJSON_AddNullToObject(result, "active_closure");
    } else {
        cJSON_AddItemToObject(result, "active_closure", active_closure);
    }
    cJSON_AddItemToObject(result, "closures", closures);
    *result_out = result;
    return 0;
}
