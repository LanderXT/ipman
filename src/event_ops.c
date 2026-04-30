/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "event_ops.h"

#include "json_helpers.h"

#include <stdio.h>
#include <string.h>

#define EVENT_LIST_DEFAULT_LIMIT 100
#define EVENT_LIST_MAX_LIMIT 500

static int is_entity_type(const char *value) {
    return strcmp(value, "plan") == 0 ||
           strcmp(value, "phase") == 0 ||
           strcmp(value, "task") == 0;
}

static int is_event_type(const char *value) {
    static const char *allowed[] = {
        "plan_created",
        "plan_updated",
        "plan_closed",
        "plan_archived",
        "plan_reopened",
        "plan_activated",
        "plan_deactivated",
        "phase_created",
        "phase_updated",
        "phase_moved",
        "phase_closed",
        "phase_reopened",
        "phase_current_changed",
        "task_created",
        "task_updated",
        "task_linked_external",
        "task_status_changed",
        "task_deferred",
        "task_canceled",
        "task_replaced",
        "task_reopened",
        "task_closed",
        "task_current_changed",
        "comment_added",
        "comment_updated",
        "comment_invalidated",
        "instruction_added",
        "instruction_updated",
        "instruction_invalidated",
    };
    size_t count = sizeof allowed / sizeof allowed[0];
    for (size_t pos = 0; pos < count; ++pos) {
        if (strcmp(value, allowed[pos]) == 0) return 1;
    }
    return 0;
}

static void bind_optional_text(sqlite3_stmt *stmt, int index, const char *value) {
    if (value == NULL) {
        sqlite3_bind_null(stmt, index);
    } else {
        sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT);
    }
}

static int read_optional_filter(cJSON *params, const char *field,
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
        *err_msg_out = "filters must be strings or null";
        return -1;
    }
    *out = item->valuestring;
    return 0;
}

static int read_optional_id(cJSON *params, const char *field,
                            int *set_out,
                            sqlite3_int64 *out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, field);
    *set_out = 0;
    *out = 0;
    if (item == NULL || cJSON_IsNull(item)) return 0;
    if (!cJSON_IsNumber(item) || item->valuedouble < 1.0) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "entity_id must be a positive integer or null";
        return -1;
    }
    *set_out = 1;
    *out = (sqlite3_int64)item->valuedouble;
    return 0;
}

static int read_list_int(cJSON *params, const char *field,
                         int default_value,
                         int *out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, field);
    if (item == NULL || cJSON_IsNull(item)) {
        *out = default_value;
        return 0;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "paging values must be non-negative integers";
        return -1;
    }
    *out = (int)item->valuedouble;
    return 0;
}

cJSON *ipman_event_from_row(sqlite3_stmt *stmt) {
    cJSON *event = cJSON_CreateObject();
    if (event == NULL) return NULL;
    cJSON_AddNumberToObject(event, "id", (double)sqlite3_column_int64(stmt, 0));
    ipman_json_add_text_or_null(event, "entity_type", sqlite3_column_text(stmt, 1));
    cJSON_AddNumberToObject(event, "entity_id",
                            (double)sqlite3_column_int64(stmt, 2));
    ipman_json_add_text_or_null(event, "event_type", sqlite3_column_text(stmt, 3));
    ipman_json_add_text_or_null(event, "actor", sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null(event, "event_at", sqlite3_column_text(stmt, 5));
    ipman_json_add_text_or_null(event, "summary", sqlite3_column_text(stmt, 6));
    ipman_json_add_json_or_null(event, "details", sqlite3_column_text(stmt, 7));
    ipman_json_add_json_or_null(event, "old_value", sqlite3_column_text(stmt, 8));
    ipman_json_add_json_or_null(event, "new_value", sqlite3_column_text(stmt, 9));
    ipman_json_add_text_or_null(event, "related_entity_type", sqlite3_column_text(stmt, 10));
    ipman_json_add_int64_or_null(event, "related_entity_id", stmt, 11);
    ipman_json_add_text_or_null(event, "request_id", sqlite3_column_text(stmt, 12));
    return event;
}

const ipman_param_desc_t ipman_op_event_list_params[] = {
    { "entity_type" }, { "entity_id" }, { "event_type" },
    { "from" }, { "to" }, { "request_id" },
    { "limit" }, { "offset" },
    { NULL },
};

int ipman_op_event_list(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out) {
    const char *entity_type = NULL;
    int has_entity_id = 0;
    sqlite3_int64 entity_id = 0;
    const char *event_type = NULL;
    const char *from = NULL;
    const char *to = NULL;
    const char *request_id = NULL;
    int limit = EVENT_LIST_DEFAULT_LIMIT;
    int offset = 0;

    if (read_optional_filter(req->params, "entity_type", &entity_type,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_id(req->params, "entity_id", &has_entity_id, &entity_id,
                         err_code_out, err_msg_out) != 0 ||
        read_optional_filter(req->params, "event_type", &event_type,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_filter(req->params, "from", &from,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_filter(req->params, "to", &to,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_filter(req->params, "request_id", &request_id,
                             err_code_out, err_msg_out) != 0 ||
        read_list_int(req->params, "limit", EVENT_LIST_DEFAULT_LIMIT, &limit,
                      err_code_out, err_msg_out) != 0 ||
        read_list_int(req->params, "offset", 0, &offset,
                      err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (entity_type == NULL && has_entity_id) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "entity_type is required when entity_id is provided";
        return -1;
    }
    if (entity_type != NULL && !is_entity_type(entity_type)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "entity_type must be plan, phase, or task";
        return -1;
    }
    if (event_type != NULL && !is_event_type(event_type)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "event_type must be a known event type";
        return -1;
    }
    if (limit < 1 || limit > EVENT_LIST_MAX_LIMIT) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "limit must be between 1 and 500";
        return -1;
    }

    /* Count total matching rows */
    int rc = 0;
    const char *count_sql =
        "SELECT COUNT(*) FROM events "
        "WHERE (? IS NULL OR entity_type = ?) "
        "AND (? = 0 OR entity_id = ?) "
        "AND (? IS NULL OR event_type = ?) "
        "AND (? IS NULL OR event_at >= ?) "
        "AND (? IS NULL OR event_at <= ?) "
        "AND (? IS NULL OR request_id = ?);";
    sqlite3_stmt *count_stmt = NULL;
    rc = sqlite3_prepare_v2(db, count_sql, -1, &count_stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to count events";
        return -1;
    }
    bind_optional_text(count_stmt, 1,  entity_type);
    bind_optional_text(count_stmt, 2,  entity_type);
    sqlite3_bind_int   (count_stmt, 3,  has_entity_id);
    sqlite3_bind_int64 (count_stmt, 4,  entity_id);
    bind_optional_text(count_stmt, 5,  event_type);
    bind_optional_text(count_stmt, 6,  event_type);
    bind_optional_text(count_stmt, 7,  from);
    bind_optional_text(count_stmt, 8,  from);
    bind_optional_text(count_stmt, 9,  to);
    bind_optional_text(count_stmt, 10, to);
    bind_optional_text(count_stmt, 11, request_id);
    bind_optional_text(count_stmt, 12, request_id);
    rc = sqlite3_step(count_stmt);
    int total_count = (rc == SQLITE_ROW) ? sqlite3_column_int(count_stmt, 0) : 0;
    sqlite3_finalize(count_stmt);

    const char *sql =
        "SELECT id, entity_type, entity_id, event_type, actor, event_at, "
        "summary, details, old_value, new_value, related_entity_type, "
        "related_entity_id, request_id "
        "FROM events "
        "WHERE (? IS NULL OR entity_type = ?) "
        "AND (? = 0 OR entity_id = ?) "
        "AND (? IS NULL OR event_type = ?) "
        "AND (? IS NULL OR event_at >= ?) "
        "AND (? IS NULL OR event_at <= ?) "
        "AND (? IS NULL OR request_id = ?) "
        "ORDER BY id DESC LIMIT ? OFFSET ?;";
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query events";
        return -1;
    }
    bind_optional_text(stmt, 1,  entity_type);
    bind_optional_text(stmt, 2,  entity_type);
    sqlite3_bind_int   (stmt, 3,  has_entity_id);
    sqlite3_bind_int64 (stmt, 4,  entity_id);
    bind_optional_text(stmt, 5,  event_type);
    bind_optional_text(stmt, 6,  event_type);
    bind_optional_text(stmt, 7,  from);
    bind_optional_text(stmt, 8,  from);
    bind_optional_text(stmt, 9,  to);
    bind_optional_text(stmt, 10, to);
    bind_optional_text(stmt, 11, request_id);
    bind_optional_text(stmt, 12, request_id);
    sqlite3_bind_int   (stmt, 13, limit + 1);
    sqlite3_bind_int   (stmt, 14, offset);

    cJSON *result = cJSON_CreateObject();
    cJSON *events = cJSON_CreateArray();
    if (result == NULL || events == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (events != NULL) cJSON_Delete(events);
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build event list";
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *event = ipman_event_from_row(stmt);
        if (event == NULL) {
            cJSON_Delete(result);
            cJSON_Delete(events);
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build event list";
            return -1;
        }
        cJSON_AddItemToArray(events, event);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(result);
        cJSON_Delete(events);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query events";
        return -1;
    }
    int has_more = 0;
    int row_count = cJSON_GetArraySize(events);
    if (row_count > limit) {
        has_more = 1;
        cJSON *last = cJSON_DetachItemFromArray(events, row_count - 1);
        cJSON_Delete(last);
    }
    cJSON_AddItemToObject(result, "events", events);
    cJSON_AddNumberToObject(result, "limit", limit);
    cJSON_AddNumberToObject(result, "offset", offset);
    cJSON_AddBoolToObject(result, "has_more", has_more);
    cJSON_AddNumberToObject(result, "total_count", total_count);
    *result_out = result;
    return 0;
}
