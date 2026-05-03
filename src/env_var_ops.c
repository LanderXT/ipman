/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "env_var_ops.h"

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

/*
 * Returns 1 iff `name` matches `^[A-Z_][A-Z0-9_]*$` — the portable env-var
 * name shape used by POSIX shells and 12-factor apps. Empty strings and
 * leading digits are rejected.
 */
static int is_valid_env_var_name(const char *name) {
    if (name == NULL || *name == '\0') return 0;
    unsigned char c0 = (unsigned char)name[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || c0 == '_')) return 0;
    for (const char *p = name + 1; *p != '\0'; ++p) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
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

cJSON *ipman_env_var_row_to_json(sqlite3_stmt *stmt, int reveal) {
    cJSON *row = cJSON_CreateObject();
    if (row == NULL) return NULL;
    cJSON_AddNumberToObject(row, "id",
                            (double)sqlite3_column_int64(stmt, 0));
    ipman_json_add_text_or_null(row, "name", sqlite3_column_text(stmt, 1));
    ipman_json_add_text_or_null(row, "purpose", sqlite3_column_text(stmt, 2));
    int sensitive = sqlite3_column_int(stmt, 4) ? 1 : 0;
    const unsigned char *example = sqlite3_column_text(stmt, 3);
    if (sensitive && !reveal) {
        if (example == NULL) {
            cJSON_AddNullToObject(row, "example");
        } else {
            cJSON_AddStringToObject(row, "example", "[sensitive]");
        }
    } else {
        ipman_json_add_text_or_null(row, "example", example);
    }
    cJSON_AddBoolToObject(row, "required",
                          sqlite3_column_int(stmt, 5) ? 1 : 0);
    cJSON_AddBoolToObject(row, "sensitive", sensitive);
    ipman_json_add_text_or_null(row, "author", sqlite3_column_text(stmt, 6));
    ipman_json_add_text_or_null(row, "created_at",
                                sqlite3_column_text(stmt, 7));
    ipman_json_add_text_or_null(row, "updated_at",
                                sqlite3_column_text(stmt, 8));
    ipman_json_add_text_or_null(row, "invalidated_at",
                                sqlite3_column_text(stmt, 9));
    ipman_json_add_text_or_null(row, "invalidated_by",
                                sqlite3_column_text(stmt, 10));
    return row;
}

static int format_env_var_details(char *buffer, size_t buffer_size,
                                  const char *op, sqlite3_int64 env_var_id) {
    int written = snprintf(buffer, buffer_size,
                           "{\"op\":\"%s\",\"env_var_id\":%lld}",
                           op, (long long)env_var_id);
    return written >= 0 && (size_t)written < buffer_size ? 0 : -1;
}

static int insert_event(sqlite3 *db,
                        const char *event_type, const char *actor,
                        const char *request_id, const char *summary,
                        const char *details, const char *old_json,
                        const char *new_json) {
    /* env_var changes are project-level audit entries. entity_id is fixed at
     * 1 because the project table is single-row by construction. */
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

static int insert_env_var(sqlite3 *db,
                          const char *name, const char *purpose,
                          const char *example, int required, int sensitive,
                          const char *author,
                          sqlite3_int64 *env_var_id_out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out) {
    const char *sql =
        "INSERT INTO env_vars("
        "name, purpose, example, required, sensitive, author"
        ") VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, purpose, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 3, example);
    sqlite3_bind_int(stmt, 4, required ? 1 : 0);
    sqlite3_bind_int(stmt, 5, sensitive ? 1 : 0);
    sqlite3_bind_text(stmt, 6, author, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        *env_var_id_out = sqlite3_last_insert_rowid(db);
        return 0;
    }
    if (rc == SQLITE_CONSTRAINT) {
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "another active env_var with this name already exists";
        return -1;
    }
    *err_code_out = IPMAN_ERR_INTERNAL;
    *err_msg_out = "failed to add env_var";
    return -1;
}

static cJSON *load_env_var(sqlite3 *db, sqlite3_int64 env_var_id, int reveal) {
    const char *sql =
        "SELECT id, name, purpose, example, sensitive, required, "
        "author, created_at, updated_at, invalidated_at, invalidated_by "
        "FROM env_vars WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, env_var_id);
    rc = sqlite3_step(stmt);
    cJSON *row = NULL;
    if (rc == SQLITE_ROW) row = ipman_env_var_row_to_json(stmt, reveal);
    sqlite3_finalize(stmt);
    return row;
}

static int load_env_var_meta(sqlite3 *db, sqlite3_int64 env_var_id,
                             int *invalidated_out) {
    const char *sql =
        "SELECT invalidated_at IS NOT NULL FROM env_vars WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, env_var_id);
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

static int update_env_var(sqlite3 *db, sqlite3_int64 env_var_id,
                          const char *name, const char *purpose,
                          const char *example, int required_present,
                          int required, int sensitive_present, int sensitive,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out) {
    const char *sql =
        "UPDATE env_vars SET "
        "name = COALESCE(?, name), "
        "purpose = COALESCE(?, purpose), "
        "example = COALESCE(?, example), "
        "required = CASE WHEN ? = 1 THEN ? ELSE required END, "
        "sensitive = CASE WHEN ? = 1 THEN ? ELSE sensitive END, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ? AND invalidated_at IS NULL;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    bind_optional_text(stmt, 1, name);
    bind_optional_text(stmt, 2, purpose);
    bind_optional_text(stmt, 3, example);
    sqlite3_bind_int(stmt, 4, required_present ? 1 : 0);
    sqlite3_bind_int(stmt, 5, required ? 1 : 0);
    sqlite3_bind_int(stmt, 6, sensitive_present ? 1 : 0);
    sqlite3_bind_int(stmt, 7, sensitive ? 1 : 0);
    sqlite3_bind_int64(stmt, 8, env_var_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE && sqlite3_changes(db) == 1) return 0;
    if (rc == SQLITE_CONSTRAINT) {
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "another active env_var with this name already exists";
        return -1;
    }
    *err_code_out = IPMAN_ERR_INTERNAL;
    *err_msg_out = "failed to update env_var";
    return -1;
}

static int invalidate_env_var(sqlite3 *db, sqlite3_int64 env_var_id,
                              const char *actor) {
    const char *sql =
        "UPDATE env_vars SET "
        "invalidated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "invalidated_by = ? "
        "WHERE id = ? AND invalidated_at IS NULL;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, env_var_id);
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
        *err_msg_out = "failed to commit env_var change";
        return -1;
    }
    return 0;
}

const ipman_param_desc_t ipman_op_env_var_add_params[] = {
    { "name" }, { "purpose" }, { "example" }, { "required" }, { "sensitive" },
    { NULL },
};

int ipman_op_env_var_add(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out) {
    const char *name = NULL;
    const char *purpose = NULL;
    const char *example = NULL;
    int required = 1;
    int sensitive = 0;
    if (read_required_string(req->params, "name", &name,
                             err_code_out, err_msg_out) != 0 ||
        read_required_string(req->params, "purpose", &purpose,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "example", &example,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_bool_default(req->params, "required", 1, &required,
                                   err_code_out, err_msg_out) != 0 ||
        read_optional_bool_default(req->params, "sensitive", 0, &sensitive,
                                   err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (!is_valid_env_var_name(name)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "name must match ^[A-Z_][A-Z0-9_]*$";
        return -1;
    }
    if (example != NULL && is_blank(example)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "example must be non-empty when provided";
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    sqlite3_int64 env_var_id = 0;
    if (insert_env_var(db, name, purpose, example, required,
                       sensitive, req->actor, &env_var_id,
                       err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    /* On the create response we always return the example as written; the
     * caller already had to know it to send it. Masking is applied on list
     * and on workspace.context_get instead. */
    cJSON *env_var = load_env_var(db, env_var_id, /*reveal=*/1);
    cJSON *result = cJSON_CreateObject();
    char details[160];
    int details_ok = format_env_var_details(details, sizeof details,
                                            "env_var.add", env_var_id);
    if (env_var == NULL || result == NULL || details_ok != 0) {
        if (env_var != NULL) cJSON_Delete(env_var);
        if (result != NULL) cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build env_var response";
        return -1;
    }
    if (insert_event(db, "env_var_added", req->actor, req->request_id,
                     "env_var added", details, NULL, NULL) != 0) {
        cJSON_Delete(env_var);
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record env_var event";
        return -1;
    }
    cJSON_AddItemToObject(result, "env_var", env_var);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_env_var_list_params[] = {
    { "include_invalidated" }, { "reveal" },
    { NULL },
};

int ipman_op_env_var_list(const ipman_request_t *req, sqlite3 *db,
                          cJSON **result_out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out) {
    int include_invalidated = 0;
    int reveal = 0;
    if (read_optional_bool_default(req->params, "include_invalidated", 0,
                                   &include_invalidated,
                                   err_code_out, err_msg_out) != 0 ||
        read_optional_bool_default(req->params, "reveal", 0, &reveal,
                                   err_code_out, err_msg_out) != 0) {
        return -1;
    }
    const char *sql =
        "SELECT id, name, purpose, example, sensitive, required, "
        "author, created_at, updated_at, invalidated_at, invalidated_by "
        "FROM env_vars "
        "WHERE (? OR invalidated_at IS NULL) "
        "ORDER BY id ASC;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to list env_vars";
        return -1;
    }
    sqlite3_bind_int(stmt, 1, include_invalidated);

    cJSON *result = cJSON_CreateObject();
    cJSON *env_vars = cJSON_CreateArray();
    if (result == NULL || env_vars == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (env_vars != NULL) cJSON_Delete(env_vars);
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build env_var list";
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *row = ipman_env_var_row_to_json(stmt, reveal);
        if (row == NULL) {
            cJSON_Delete(result);
            cJSON_Delete(env_vars);
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build env_var list";
            return -1;
        }
        cJSON_AddItemToArray(env_vars, row);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(result);
        cJSON_Delete(env_vars);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to list env_vars";
        return -1;
    }
    cJSON_AddItemToObject(result, "env_vars", env_vars);
    cJSON_AddBoolToObject(result, "include_invalidated", include_invalidated);
    cJSON_AddBoolToObject(result, "reveal", reveal);
    cJSON_AddBoolToObject(result, "has_more", 0);
    cJSON_AddNumberToObject(result, "total_count",
                            cJSON_GetArraySize(env_vars));
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_env_var_update_params[] = {
    { "id" }, { "name" }, { "purpose" }, { "example" },
    { "required" }, { "sensitive" },
    { NULL },
};

int ipman_op_env_var_update(const ipman_request_t *req, sqlite3 *db,
                            cJSON **result_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out) {
    sqlite3_int64 env_var_id = 0;
    const char *name = NULL;
    const char *purpose = NULL;
    const char *example = NULL;
    int required_present = 0;
    int required = 0;
    int sensitive_present = 0;
    int sensitive = 0;
    if (ipman_read_positive_id(req->params, "id", &env_var_id,
                              err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "name", &name,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "purpose", &purpose,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "example", &example,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_bool_present(req->params, "required",
                                   &required_present, &required,
                                   err_code_out, err_msg_out) != 0 ||
        read_optional_bool_present(req->params, "sensitive",
                                   &sensitive_present, &sensitive,
                                   err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (name == NULL && purpose == NULL && example == NULL &&
        !required_present && !sensitive_present) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "env_var.update requires at least one mutable field";
        return -1;
    }
    if (name != NULL && !is_valid_env_var_name(name)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "name must match ^[A-Z_][A-Z0-9_]*$";
        return -1;
    }
    if (purpose != NULL && is_blank(purpose)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "purpose must be non-empty when provided";
        return -1;
    }
    if (example != NULL && is_blank(example)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "example must be non-empty when provided";
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    int invalidated = 0;
    int meta = load_env_var_meta(db, env_var_id, &invalidated);
    if (meta == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "env_var not found";
        return -1;
    }
    if (meta < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to load env_var";
        return -1;
    }
    if (invalidated) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "env_var is invalidated";
        return -1;
    }
    /* Audit values are written with the example revealed regardless of the
     * sensitive flag — old/new_value live inside the events table only and
     * are not surfaced through workspace.context_get or env_var.list. The
     * audit trail must reflect actual changes to be useful. */
    cJSON *old_env_var = load_env_var(db, env_var_id, /*reveal=*/1);
    char *old_json = json_print_owned(old_env_var);
    if (old_env_var == NULL || old_json == NULL) {
        if (old_env_var != NULL) cJSON_Delete(old_env_var);
        if (old_json != NULL) cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build env_var event";
        return -1;
    }
    if (update_env_var(db, env_var_id, name, purpose, example,
                       required_present, required,
                       sensitive_present, sensitive,
                       err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_env_var);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    cJSON *new_env_var = load_env_var(db, env_var_id, /*reveal=*/1);
    cJSON *result = cJSON_CreateObject();
    char *new_json = json_print_owned(new_env_var);
    char details[160];
    int details_ok = format_env_var_details(details, sizeof details,
                                            "env_var.update", env_var_id);
    if (new_env_var == NULL || result == NULL || new_json == NULL ||
        details_ok != 0) {
        if (new_env_var != NULL) cJSON_Delete(new_env_var);
        if (result != NULL) cJSON_Delete(result);
        if (new_json != NULL) cJSON_free(new_json);
        cJSON_Delete(old_env_var);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build env_var response";
        return -1;
    }
    if (insert_event(db, "env_var_updated", req->actor, req->request_id,
                     "env_var updated", details, old_json, new_json) != 0) {
        cJSON_Delete(new_env_var);
        cJSON_Delete(result);
        cJSON_Delete(old_env_var);
        cJSON_free(old_json);
        cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record env_var event";
        return -1;
    }
    cJSON_Delete(old_env_var);
    cJSON_free(old_json);
    cJSON_free(new_json);
    cJSON_AddItemToObject(result, "env_var", new_env_var);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_env_var_invalidate_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_env_var_invalidate(const ipman_request_t *req, sqlite3 *db,
                                cJSON **result_out,
                                ipman_error_code_t *err_code_out,
                                const char **err_msg_out) {
    sqlite3_int64 env_var_id = 0;
    if (ipman_read_positive_id(req->params, "id", &env_var_id,
                              err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    int invalidated = 0;
    int meta = load_env_var_meta(db, env_var_id, &invalidated);
    if (meta == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "env_var not found";
        return -1;
    }
    if (meta < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to load env_var";
        return -1;
    }
    if (invalidated) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "env_var is already invalidated";
        return -1;
    }
    cJSON *old_env_var = load_env_var(db, env_var_id, /*reveal=*/1);
    char *old_json = json_print_owned(old_env_var);
    if (old_env_var == NULL || old_json == NULL) {
        if (old_env_var != NULL) cJSON_Delete(old_env_var);
        if (old_json != NULL) cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build env_var event";
        return -1;
    }
    if (invalidate_env_var(db, env_var_id, req->actor) != 0) {
        cJSON_Delete(old_env_var);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to invalidate env_var";
        return -1;
    }
    cJSON *new_env_var = load_env_var(db, env_var_id, /*reveal=*/1);
    cJSON *result = cJSON_CreateObject();
    char *new_json = json_print_owned(new_env_var);
    char details[160];
    int details_ok = format_env_var_details(details, sizeof details,
                                            "env_var.invalidate", env_var_id);
    if (new_env_var == NULL || result == NULL || new_json == NULL ||
        details_ok != 0) {
        if (new_env_var != NULL) cJSON_Delete(new_env_var);
        if (result != NULL) cJSON_Delete(result);
        if (new_json != NULL) cJSON_free(new_json);
        cJSON_Delete(old_env_var);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build env_var response";
        return -1;
    }
    if (insert_event(db, "env_var_invalidated", req->actor, req->request_id,
                     "env_var invalidated", details, old_json, new_json) != 0) {
        cJSON_Delete(new_env_var);
        cJSON_Delete(result);
        cJSON_Delete(old_env_var);
        cJSON_free(old_json);
        cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record env_var event";
        return -1;
    }
    cJSON_Delete(old_env_var);
    cJSON_free(old_json);
    cJSON_free(new_json);
    cJSON_AddItemToObject(result, "env_var", new_env_var);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}
