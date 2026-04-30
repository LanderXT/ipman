/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "instruction_ops.h"

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

static int is_entity_type(const char *value) {
    return strcmp(value, "plan") == 0 ||
           strcmp(value, "phase") == 0 ||
           strcmp(value, "task") == 0;
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

static cJSON *instruction_from_row(sqlite3_stmt *stmt) {
    cJSON *instruction = cJSON_CreateObject();
    if (instruction == NULL) return NULL;
    cJSON_AddNumberToObject(instruction, "id",
                            (double)sqlite3_column_int64(stmt, 0));
    ipman_json_add_text_or_null(instruction, "entity_type", sqlite3_column_text(stmt, 1));
    cJSON_AddNumberToObject(instruction, "entity_id",
                            (double)sqlite3_column_int64(stmt, 2));
    ipman_json_add_text_or_null(instruction, "instruction_type", sqlite3_column_text(stmt, 3));
    ipman_json_add_text_or_null(instruction, "body", sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null(instruction, "author", sqlite3_column_text(stmt, 5));
    ipman_json_add_text_or_null(instruction, "created_at", sqlite3_column_text(stmt, 6));
    ipman_json_add_text_or_null(instruction, "updated_at", sqlite3_column_text(stmt, 7));
    ipman_json_add_text_or_null(instruction, "invalidated_at", sqlite3_column_text(stmt, 8));
    ipman_json_add_text_or_null(instruction, "invalidated_by", sqlite3_column_text(stmt, 9));
    return instruction;
}

static int format_instruction_details(char *buffer,
                                      size_t buffer_size,
                                      const char *op,
                                      sqlite3_int64 instruction_id) {
    int written = snprintf(buffer, buffer_size,
                           "{\"op\":\"%s\",\"instruction_id\":%lld}",
                           op, (long long)instruction_id);
    return written >= 0 && (size_t)written < buffer_size ? 0 : -1;
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

static int insert_instruction(sqlite3 *db,
                              const char *entity_type,
                              sqlite3_int64 entity_id,
                              const char *instruction_type,
                              const char *body,
                              const char *author,
                              sqlite3_int64 *instruction_id_out) {
    const char *sql =
        "INSERT INTO instructions("
        "entity_type, entity_id, instruction_type, body, author"
        ") VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, entity_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, entity_id);
    sqlite3_bind_text(stmt, 3, instruction_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, body, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, author, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    *instruction_id_out = sqlite3_last_insert_rowid(db);
    return 0;
}

static cJSON *load_instruction(sqlite3 *db, sqlite3_int64 instruction_id) {
    const char *sql =
        "SELECT id, entity_type, entity_id, instruction_type, body, author, "
        "created_at, updated_at, invalidated_at, invalidated_by "
        "FROM instructions WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, instruction_id);
    rc = sqlite3_step(stmt);
    cJSON *instruction = NULL;
    if (rc == SQLITE_ROW) instruction = instruction_from_row(stmt);
    sqlite3_finalize(stmt);
    return instruction;
}

static int load_instruction_meta(sqlite3 *db, sqlite3_int64 instruction_id,
                                 char *entity_type_buf,
                                 size_t entity_type_size,
                                 sqlite3_int64 *entity_id_out,
                                 int *invalidated_out) {
    const char *sql =
        "SELECT entity_type, entity_id, invalidated_at IS NOT NULL "
        "FROM instructions WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, instruction_id);
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
        sqlite3_finalize(stmt);
        return 1;
    }
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) return 0;
    return -1;
}

static int update_instruction(sqlite3 *db,
                              sqlite3_int64 instruction_id,
                              const char *instruction_type,
                              const char *body) {
    const char *sql =
        "UPDATE instructions SET "
        "instruction_type = COALESCE(?, instruction_type), "
        "body = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ? AND invalidated_at IS NULL;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    bind_optional_text(stmt, 1, instruction_type);
    sqlite3_bind_text(stmt, 2, body, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, instruction_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

static int invalidate_instruction(sqlite3 *db,
                                  sqlite3_int64 instruction_id,
                                  const char *actor) {
    const char *sql =
        "UPDATE instructions SET "
        "invalidated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "invalidated_by = ? "
        "WHERE id = ? AND invalidated_at IS NULL;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, instruction_id);
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
        *err_msg_out = "failed to commit instruction change";
        return -1;
    }
    return 0;
}

const ipman_param_desc_t ipman_op_instruction_add_params[] = {
    { "entity_type" }, { "entity_id" }, { "body" }, { "instruction_type" },
    { NULL },
};

int ipman_op_instruction_add(const ipman_request_t *req, sqlite3 *db,
                            cJSON **result_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out) {
    const char *entity_type = NULL;
    sqlite3_int64 entity_id = 0;
    const char *instruction_type = NULL;
    const char *body = NULL;
    if (read_entity_ref(req->params, &entity_type, &entity_id,
                        err_code_out, err_msg_out) != 0 ||
        read_required_string(req->params, "body", &body,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "instruction_type",
                             &instruction_type,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (instruction_type == NULL) instruction_type = "guidance";
    if (is_blank(instruction_type)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "instruction_type must be non-empty when provided";
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
    sqlite3_int64 instruction_id = 0;
    if (insert_instruction(db, entity_type, entity_id, instruction_type, body,
                           req->actor, &instruction_id) != 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to add instruction";
        return -1;
    }
    cJSON *instruction = load_instruction(db, instruction_id);
    cJSON *result = cJSON_CreateObject();
    char details[160];
    int details_ok = format_instruction_details(details, sizeof details,
                                                "instruction.add",
                                                instruction_id);
    if (instruction == NULL || result == NULL || details_ok != 0) {
        if (instruction != NULL) cJSON_Delete(instruction);
        if (result != NULL) cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build instruction response";
        return -1;
    }
    if (insert_event(db, entity_type, entity_id, "instruction_added",
                     req->actor, req->request_id, "instruction added",
                     details, NULL, NULL) != 0) {
        cJSON_Delete(instruction);
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record instruction event";
        return -1;
    }
    cJSON_AddItemToObject(result, "instruction", instruction);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_instruction_list_params[] = {
    { "entity_type" }, { "entity_id" }, { "include_invalidated" },
    { NULL },
};

int ipman_op_instruction_list(const ipman_request_t *req, sqlite3 *db,
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
        "SELECT id, entity_type, entity_id, instruction_type, body, author, "
        "created_at, updated_at, invalidated_at, invalidated_by "
        "FROM instructions "
        "WHERE entity_type = ? AND entity_id = ? "
        "AND (? OR invalidated_at IS NULL) "
        "ORDER BY id ASC;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to list instructions";
        return -1;
    }
    sqlite3_bind_text(stmt, 1, entity_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, entity_id);
    sqlite3_bind_int(stmt, 3, include_invalidated);

    cJSON *result = cJSON_CreateObject();
    cJSON *instructions = cJSON_CreateArray();
    if (result == NULL || instructions == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (instructions != NULL) cJSON_Delete(instructions);
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build instruction list";
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *instruction = instruction_from_row(stmt);
        if (instruction == NULL) {
            cJSON_Delete(result);
            cJSON_Delete(instructions);
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build instruction list";
            return -1;
        }
        cJSON_AddItemToArray(instructions, instruction);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(result);
        cJSON_Delete(instructions);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to list instructions";
        return -1;
    }
    cJSON_AddItemToObject(result, "instructions", instructions);
    cJSON_AddBoolToObject(result, "include_invalidated", include_invalidated);
    cJSON_AddBoolToObject(result, "has_more", 0);
    cJSON_AddNumberToObject(result, "total_count",
                            cJSON_GetArraySize(instructions));
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_instruction_update_params[] = {
    { "id" }, { "body" }, { "instruction_type" },
    { NULL },
};

int ipman_op_instruction_update(const ipman_request_t *req, sqlite3 *db,
                               cJSON **result_out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out) {
    sqlite3_int64 instruction_id = 0;
    const char *instruction_type = NULL;
    const char *body = NULL;
    if (ipman_read_positive_id(req->params, "id", &instruction_id,
                              err_code_out, err_msg_out) != 0 ||
        read_required_string(req->params, "body", &body,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "instruction_type",
                             &instruction_type,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (instruction_type != NULL && is_blank(instruction_type)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "instruction_type must be non-empty when provided";
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
    int meta = load_instruction_meta(db, instruction_id, entity_type,
                                     sizeof entity_type, &entity_id,
                                     &invalidated);
    if (meta == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "instruction not found";
        return -1;
    }
    if (meta < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to load instruction";
        return -1;
    }
    if (invalidated) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "instruction is invalidated";
        return -1;
    }
    cJSON *old_instruction = load_instruction(db, instruction_id);
    char *old_json = json_print_owned(old_instruction);
    if (old_instruction == NULL || old_json == NULL) {
        if (old_instruction != NULL) cJSON_Delete(old_instruction);
        if (old_json != NULL) cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build instruction event";
        return -1;
    }
    if (update_instruction(db, instruction_id, instruction_type, body) != 0) {
        cJSON_Delete(old_instruction);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to update instruction";
        return -1;
    }
    cJSON *new_instruction = load_instruction(db, instruction_id);
    cJSON *result = cJSON_CreateObject();
    char *new_json = json_print_owned(new_instruction);
    char details[160];
    int details_ok = format_instruction_details(details, sizeof details,
                                                "instruction.update",
                                                instruction_id);
    if (new_instruction == NULL || result == NULL || new_json == NULL ||
        details_ok != 0) {
        if (new_instruction != NULL) cJSON_Delete(new_instruction);
        if (result != NULL) cJSON_Delete(result);
        if (new_json != NULL) cJSON_free(new_json);
        cJSON_Delete(old_instruction);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build instruction response";
        return -1;
    }
    if (insert_event(db, entity_type, entity_id, "instruction_updated",
                     req->actor, req->request_id, "instruction updated",
                     details, old_json, new_json) != 0) {
        cJSON_Delete(new_instruction);
        cJSON_Delete(result);
        cJSON_Delete(old_instruction);
        cJSON_free(old_json);
        cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record instruction event";
        return -1;
    }
    cJSON_Delete(old_instruction);
    cJSON_free(old_json);
    cJSON_free(new_json);
    cJSON_AddItemToObject(result, "instruction", new_instruction);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_instruction_invalidate_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_instruction_invalidate(const ipman_request_t *req, sqlite3 *db,
                                   cJSON **result_out,
                                   ipman_error_code_t *err_code_out,
                                   const char **err_msg_out) {
    sqlite3_int64 instruction_id = 0;
    if (ipman_read_positive_id(req->params, "id", &instruction_id,
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
    int meta = load_instruction_meta(db, instruction_id, entity_type,
                                     sizeof entity_type, &entity_id,
                                     &invalidated);
    if (meta == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "instruction not found";
        return -1;
    }
    if (meta < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to load instruction";
        return -1;
    }
    if (invalidated) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "instruction is already invalidated";
        return -1;
    }
    cJSON *old_instruction = load_instruction(db, instruction_id);
    char *old_json = json_print_owned(old_instruction);
    if (old_instruction == NULL || old_json == NULL) {
        if (old_instruction != NULL) cJSON_Delete(old_instruction);
        if (old_json != NULL) cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build instruction event";
        return -1;
    }
    if (invalidate_instruction(db, instruction_id, req->actor) != 0) {
        cJSON_Delete(old_instruction);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to invalidate instruction";
        return -1;
    }
    cJSON *new_instruction = load_instruction(db, instruction_id);
    cJSON *result = cJSON_CreateObject();
    char *new_json = json_print_owned(new_instruction);
    char details[160];
    int details_ok = format_instruction_details(details, sizeof details,
                                                "instruction.invalidate",
                                                instruction_id);
    if (new_instruction == NULL || result == NULL || new_json == NULL ||
        details_ok != 0) {
        if (new_instruction != NULL) cJSON_Delete(new_instruction);
        if (result != NULL) cJSON_Delete(result);
        if (new_json != NULL) cJSON_free(new_json);
        cJSON_Delete(old_instruction);
        cJSON_free(old_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build instruction response";
        return -1;
    }
    if (insert_event(db, entity_type, entity_id, "instruction_invalidated",
                     req->actor, req->request_id, "instruction invalidated",
                     details, old_json, new_json) != 0) {
        cJSON_Delete(new_instruction);
        cJSON_Delete(result);
        cJSON_Delete(old_instruction);
        cJSON_free(old_json);
        cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record instruction event";
        return -1;
    }
    cJSON_Delete(old_instruction);
    cJSON_free(old_json);
    cJSON_free(new_json);
    cJSON_AddItemToObject(result, "instruction", new_instruction);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}
