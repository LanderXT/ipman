/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "plan_ops.h"
#include "db.h"
#include "event_ops.h"
#include "json_helpers.h"
#include "validation.h"

#include <stdlib.h>
#include <stdio.h>
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

static int is_plan_status(const char *value) {
    static const char *allowed[] = {
        "open", "in_progress", "paused", "completed", "canceled", "archived",
    };
    size_t count = sizeof allowed / sizeof allowed[0];
    for (size_t index = 0; index < count; ++index) {
        if (strcmp(value, allowed[index]) == 0) return 1;
    }
    return 0;
}

static int is_terminal_plan_status(const char *status) {
    return strcmp(status, "completed") == 0 ||
           strcmp(status, "canceled") == 0 ||
           strcmp(status, "archived") == 0;
}

static ipman_error_code_t create_sqlite_error_code(sqlite3 *db, int rc) {
    int extended = sqlite3_extended_errcode(db);
    if (rc == SQLITE_CONSTRAINT ||
        (extended & 0xff) == SQLITE_CONSTRAINT) {
        return IPMAN_ERR_CONFLICT;
    }
    return IPMAN_ERR_INTERNAL;
}

static int is_priority(const char *value) {
    static const char *allowed[] = {
        "low", "medium", "high", "critical",
    };
    size_t count = sizeof allowed / sizeof allowed[0];
    for (size_t index = 0; index < count; ++index) {
        if (strcmp(value, allowed[index]) == 0) return 1;
    }
    return 0;
}

typedef struct {
    int title_set;
    const char *title;
    int summary_set;
    const char *summary;
    int description_set;
    const char *description;
    int priority_set;
    const char *priority;
    int owner_set;
    const char *owner;
    int target_date_set;
    const char *target_date;
    int tags_set;
    char *tags_json;
    int version_label_set;
    const char *version_label;
} plan_update_t;

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
    if (ipman_validate_text_field(field, item->valuestring,
                                 err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (strcmp(field, "target_date") == 0 &&
        ipman_validate_date_field(item->valuestring,
                                 err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *out = item->valuestring;
    return 0;
}

static int read_required_title(cJSON *params, const char **out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "title");
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        is_blank(item->valuestring)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "title must be a non-empty string";
        return -1;
    }
    if (ipman_validate_text_field("title", item->valuestring,
                                 err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *out = item->valuestring;
    return 0;
}

static int read_status(cJSON *params, const char **out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "status");
    if (item == NULL || cJSON_IsNull(item)) {
        *out = "open";
        return 0;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        !is_plan_status(item->valuestring)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "status must be a valid plan status";
        return -1;
    }
    const char *s = item->valuestring;
    if (strcmp(s, "completed") == 0 || strcmp(s, "archived") == 0 ||
        strcmp(s, "canceled") == 0) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "status cannot be a terminal state on creation";
        return -1;
    }
    *out = s;
    return 0;
}

static int read_priority(cJSON *params, const char **out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "priority");
    if (item == NULL || cJSON_IsNull(item)) {
        *out = NULL;
        return 0;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        !is_priority(item->valuestring)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "priority must be one of low, medium, high, critical";
        return -1;
    }
    *out = item->valuestring;
    return 0;
}

static int read_tags(cJSON *params, char **tags_json_out,
                     ipman_error_code_t *err_code_out,
                     const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "tags");
    *tags_json_out = NULL;
    if (item == NULL || cJSON_IsNull(item)) return 0;
    if (!cJSON_IsArray(item)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "tags must be an array of strings or null";
        return -1;
    }
    cJSON *tag = NULL;
    cJSON_ArrayForEach(tag, item) {
        if (!cJSON_IsString(tag) || tag->valuestring == NULL) {
            *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
            *err_msg_out = "tags must be an array of strings or null";
            return -1;
        }
    }
    *tags_json_out = cJSON_PrintUnformatted(item);
    if (*tags_json_out == NULL) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to serialize tags";
        return -1;
    }
    return 0;
}

static int read_plan_selector(sqlite3 *db, cJSON *params,
                              sqlite3_int64 *plan_id_out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out) {
    (void)db;
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!id_item || !cJSON_IsNumber(id_item)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "id required (use plan.lookup to resolve uid/label/code to id)";
        return -1;
    }
    if (id_item->valuedouble < 1.0) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "id must be a positive integer";
        return -1;
    }
    *plan_id_out = (sqlite3_int64)id_item->valuedouble;
    return 0;
}

int ipman_resolve_plan_scope(cJSON *params, sqlite3 *db,
                             sqlite3_int64 *plan_id_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out) {
    (void)db;
    cJSON *plan_id_item = cJSON_GetObjectItemCaseSensitive(params, "plan_id");
    if (plan_id_item == NULL || !cJSON_IsNumber(plan_id_item) ||
        plan_id_item->valuedouble < 1.0) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "plan_id required (use plan.lookup to resolve plan_uid/plan_label to plan_id)";
        return -1;
    }
    *plan_id_out = (sqlite3_int64)plan_id_item->valuedouble;
    return 0;
}

static int read_optional_since(cJSON *params,
                               sqlite3_int64 *since_out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out) {
    cJSON *since = cJSON_GetObjectItemCaseSensitive(params, "since");
    if (since == NULL || cJSON_IsNull(since)) {
        *since_out = 0;
        return 0;
    }
    if (!cJSON_IsNumber(since) || since->valuedouble < 1.0) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "since must be a positive event id";
        return -1;
    }
    *since_out = (sqlite3_int64)since->valuedouble;
    return 0;
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

#define PLAN_LIST_DEFAULT_LIMIT 100
#define PLAN_LIST_MAX_LIMIT 500

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

static int read_update_string(cJSON *params, const char *field,
                              int allow_null,
                              int *set_out,
                              const char **value_out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, field);
    *set_out = 0;
    *value_out = NULL;
    if (item == NULL) return 0;
    *set_out = 1;
    if (cJSON_IsNull(item) && allow_null) return 0;
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "updated fields must be strings or null";
        return -1;
    }
    if (strcmp(field, "title") == 0 && is_blank(item->valuestring)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "title must be a non-empty string";
        return -1;
    }
    if (ipman_validate_text_field(field, item->valuestring,
                                 err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (strcmp(field, "target_date") == 0 &&
        ipman_validate_date_field(item->valuestring,
                                 err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *value_out = item->valuestring;
    return 0;
}

static int read_update_tags(cJSON *params,
                            int *set_out,
                            char **tags_json_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "tags");
    *set_out = 0;
    *tags_json_out = NULL;
    if (item == NULL) return 0;
    *set_out = 1;
    if (cJSON_IsNull(item)) return 0;
    if (!cJSON_IsArray(item)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "tags must be an array of strings or null";
        return -1;
    }
    cJSON *tag = NULL;
    cJSON_ArrayForEach(tag, item) {
        if (!cJSON_IsString(tag) || tag->valuestring == NULL) {
            *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
            *err_msg_out = "tags must be an array of strings or null";
            return -1;
        }
    }
    *tags_json_out = cJSON_PrintUnformatted(item);
    if (*tags_json_out == NULL) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to serialize tags";
        return -1;
    }
    return 0;
}

static int read_plan_update(cJSON *params,
                            plan_update_t *update,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out) {
    memset(update, 0, sizeof *update);
    if (read_update_string(params, "title", 0,
                           &update->title_set, &update->title,
                           err_code_out, err_msg_out) != 0 ||
        read_update_string(params, "summary", 1,
                           &update->summary_set, &update->summary,
                           err_code_out, err_msg_out) != 0 ||
        read_update_string(params, "description", 1,
                           &update->description_set, &update->description,
                           err_code_out, err_msg_out) != 0 ||
        read_update_string(params, "priority", 1,
                           &update->priority_set, &update->priority,
                           err_code_out, err_msg_out) != 0 ||
        read_update_string(params, "owner", 1,
                           &update->owner_set, &update->owner,
                           err_code_out, err_msg_out) != 0 ||
        read_update_string(params, "target_date", 1,
                           &update->target_date_set, &update->target_date,
                           err_code_out, err_msg_out) != 0 ||
        read_update_string(params, "version_label", 1,
                           &update->version_label_set, &update->version_label,
                           err_code_out, err_msg_out) != 0 ||
        read_update_tags(params, &update->tags_set, &update->tags_json,
                         err_code_out, err_msg_out) != 0) {
        if (update->tags_json != NULL) cJSON_free(update->tags_json);
        return -1;
    }
    if (update->priority_set && update->priority != NULL &&
        !is_priority(update->priority)) {
        if (update->tags_json != NULL) cJSON_free(update->tags_json);
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "priority must be one of low, medium, high, critical";
        return -1;
    }
    if (!update->title_set &&
        !update->summary_set &&
        !update->description_set &&
        !update->priority_set &&
        !update->owner_set &&
        !update->target_date_set &&
        !update->tags_set &&
        !update->version_label_set) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "at least one updatable field is required";
        return -1;
    }
    return 0;
}

static int run_sql(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return rc;
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

static cJSON *plan_from_row(sqlite3_stmt *stmt) {
    cJSON *plan = cJSON_CreateObject();
    if (plan == NULL) return NULL;

    /* uid/label first so consumers (humans, agents) reach the durable
     * identifiers before the internal numeric `id`. */
    ipman_json_add_text_or_null(plan, "uid",           sqlite3_column_text(stmt, 16));
    ipman_json_add_text_or_null(plan, "label",         sqlite3_column_text(stmt, 17));
    cJSON_AddNumberToObject(plan, "id", (double)sqlite3_column_int64(stmt, 0));
    ipman_json_add_text_or_null(plan, "code", sqlite3_column_text(stmt, 1));
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

static cJSON *load_plan_checked(sqlite3 *db,
                                sqlite3_int64 plan_id,
                                int *load_failed_out) {
    if (load_failed_out != NULL) *load_failed_out = 0;
    const char *sql =
        "SELECT id, code, title, summary, description, status, priority, "
        "created_at, updated_at, opened_at, closed_at, archived_at, owner, "
        "target_date, tags, version_label, uid, label "
        "FROM plans WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (load_failed_out != NULL) *load_failed_out = 1;
        return NULL;
    }
    sqlite3_bind_int64(stmt, 1, plan_id);
    rc = sqlite3_step(stmt);
    cJSON *plan = NULL;
    if (rc == SQLITE_ROW) {
        plan = plan_from_row(stmt);
        if (plan == NULL && load_failed_out != NULL) *load_failed_out = 1;
    } else if (rc != SQLITE_DONE && load_failed_out != NULL) {
        *load_failed_out = 1;
    }
    sqlite3_finalize(stmt);
    return plan;
}

static cJSON *load_plan(sqlite3 *db, sqlite3_int64 plan_id) {
    return load_plan_checked(db, plan_id, NULL);
}


static sqlite3_int64 plan_json_id(cJSON *plan) {
    cJSON *id = cJSON_GetObjectItemCaseSensitive(plan, "id");
    if (!cJSON_IsNumber(id)) return 0;
    return (sqlite3_int64)id->valuedouble;
}

static const char *plan_json_status(cJSON *plan) {
    cJSON *status = cJSON_GetObjectItemCaseSensitive(plan, "status");
    if (!cJSON_IsString(status)) return NULL;
    return status->valuestring;
}

static int insert_plan(sqlite3 *db,
                       const char *code,
                       const char *title,
                       const char *summary,
                       const char *description,
                       const char *status,
                       const char *priority,
                       const char *owner,
                       const char *target_date,
                       const char *tags_json,
                       const char *version_label,
                       const char *label,
                       sqlite3_int64 *plan_id_out) {
    const char *sql =
        "INSERT INTO plans("
        "code, title, summary, description, status, priority, owner, "
        "target_date, tags, version_label, label"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "PREPARE ERROR: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    bind_optional_text(stmt, 1, code);
    sqlite3_bind_text(stmt, 2, title, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 3, summary);
    bind_optional_text(stmt, 4, description);
    sqlite3_bind_text(stmt, 5, status, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 6, priority);
    bind_optional_text(stmt, 7, owner);
    bind_optional_text(stmt, 8, target_date);
    bind_optional_text(stmt, 9, tags_json);
    bind_optional_text(stmt, 10, version_label);
    
    char generated_label[64] = {0};
    if (label != NULL && !is_blank(label)) {
        sqlite3_bind_text(stmt, 11, label, -1, SQLITE_TRANSIENT);
    } else {
        ipman_slugify(title, generated_label, sizeof(generated_label));
        if (generated_label[0] == '\0') {
            sqlite3_bind_null(stmt, 11);
        } else {
            sqlite3_bind_text(stmt, 11, generated_label, -1, SQLITE_TRANSIENT);
        }
    }
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return rc;
    sqlite3_int64 new_id = sqlite3_last_insert_rowid(db);
    *plan_id_out = new_id;
    
    char uid_buf[64];
    snprintf(uid_buf, sizeof(uid_buf), "plan_%lld", (long long)new_id);
    
    if (label == NULL || is_blank(label)) {
        if (generated_label[0] == '\0') {
            snprintf(generated_label, sizeof(generated_label), "plan-%lld", (long long)new_id);
        }
        const char *update_sql = "UPDATE plans SET uid = ?, label = ? WHERE id = ?;";
        rc = sqlite3_prepare_v2(db, update_sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) return rc;
        sqlite3_bind_text(stmt, 1, uid_buf, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, generated_label, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, new_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) return rc;
    } else {
        const char *update_sql = "UPDATE plans SET uid = ? WHERE id = ?;";
        rc = sqlite3_prepare_v2(db, update_sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) return rc;
        sqlite3_bind_text(stmt, 1, uid_buf, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, new_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) return rc;
    }
    return 0;
}

static int insert_plan_event(sqlite3 *db,
                             sqlite3_int64 plan_id,
                             const char *event_type,
                             const char *actor,
                             const char *request_id,
                             const char *summary,
                             const char *details,
                             const char *old_json,
                             const char *new_json,
                             sqlite3_int64 *event_id_out) {
    const char *sql =
        "INSERT INTO events("
        "entity_type, entity_id, event_type, actor, summary, details, "
        "old_value, new_value, request_id"
        ") VALUES ('plan', ?, ?, ?, ?, ?, ?, ?, ?);";
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
    sqlite3_bind_text(stmt, 8, request_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    if (event_id_out != NULL) *event_id_out = sqlite3_last_insert_rowid(db);
    return 0;
}

const ipman_param_desc_t ipman_op_plan_create_params[] = {
    { "title" }, { "code" }, { "summary" }, { "description" },
    { "status" }, { "priority" }, { "owner" },
    { "target_date" }, { "version_label" }, { "tags" },
    { "label" },
    { NULL },
};

int ipman_op_plan_create(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out) {
    const char *title = NULL;
    const char *code = NULL;
    const char *summary = NULL;
    const char *description = NULL;
    const char *status = NULL;
    const char *priority = NULL;
    const char *owner = NULL;
    const char *target_date = NULL;
    const char *version_label = NULL;
    const char *label = NULL;
    char *tags_json = NULL;

    if (read_required_title(req->params, &title, err_code_out, err_msg_out) != 0 ||
        read_status(req->params, &status, err_code_out, err_msg_out) != 0 ||
        read_priority(req->params, &priority, err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "code", &code, err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "summary", &summary, err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "description", &description, err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "owner", &owner, err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "target_date", &target_date, err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "version_label", &version_label, err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "label", &label, err_code_out, err_msg_out) != 0 ||
        read_tags(req->params, &tags_json, err_code_out, err_msg_out) != 0) {
        if (tags_json != NULL) cJSON_free(tags_json);
        return -1;
    }

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        if (tags_json != NULL) cJSON_free(tags_json);
        return -1;
    }

    sqlite3_int64 plan_id = 0;
    int insert_rc = insert_plan(db, code, title, summary, description, status,
                                priority, owner, target_date, tags_json,
                                version_label, label, &plan_id);
    if (insert_rc != 0) {
        fprintf(stderr, "INSERT PLAN SQLITE ERROR: %s\n", sqlite3_errmsg(db));
        run_sql(db, "ROLLBACK;");
        *err_code_out = create_sqlite_error_code(db, insert_rc);
        *err_msg_out = "failed to create plan";
        if (tags_json != NULL) cJSON_free(tags_json);
        return -1;
    }
    if (tags_json != NULL) cJSON_free(tags_json);

    cJSON *plan = load_plan(db, plan_id);
    cJSON *result = cJSON_CreateObject();
    if (plan == NULL || result == NULL) {
        if (plan != NULL) cJSON_Delete(plan);
        if (result != NULL) cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build plan response";
        return -1;
    }

    char *plan_json = cJSON_PrintUnformatted(plan);
    if (plan_json == NULL) {
        cJSON_Delete(plan);
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to serialize plan event";
        return -1;
    }

    if (insert_plan_event(db, plan_id, "plan_created", req->actor,
                          req->request_id, "plan created",
                          "{\"op\":\"plan.create\"}", NULL, plan_json,
                          NULL) != 0) {
        cJSON_free(plan_json);
        cJSON_Delete(plan);
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record plan event";
        return -1;
    }
    cJSON_free(plan_json);

    cJSON_AddItemToObject(result, "plan", plan);
    if (run_sql(db, "COMMIT;") != 0) {
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to commit plan create";
        return -1;
    }

    *result_out = result;
    return 0;
}

static void bind_update_text(sqlite3_stmt *stmt,
                             int flag_index,
                             int value_index,
                             int is_set,
                             const char *value) {
    sqlite3_bind_int(stmt, flag_index, is_set);
    bind_optional_text(stmt, value_index, value);
}

static int update_plan_fields(sqlite3 *db,
                              sqlite3_int64 plan_id,
                              const plan_update_t *update) {
    const char *sql =
        "UPDATE plans SET "
        "title = CASE WHEN ? THEN ? ELSE title END, "
        "summary = CASE WHEN ? THEN ? ELSE summary END, "
        "description = CASE WHEN ? THEN ? ELSE description END, "
        "priority = CASE WHEN ? THEN ? ELSE priority END, "
        "owner = CASE WHEN ? THEN ? ELSE owner END, "
        "target_date = CASE WHEN ? THEN ? ELSE target_date END, "
        "tags = CASE WHEN ? THEN ? ELSE tags END, "
        "version_label = CASE WHEN ? THEN ? ELSE version_label END, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    bind_update_text(stmt, 1, 2, update->title_set, update->title);
    bind_update_text(stmt, 3, 4, update->summary_set, update->summary);
    bind_update_text(stmt, 5, 6, update->description_set, update->description);
    bind_update_text(stmt, 7, 8, update->priority_set, update->priority);
    bind_update_text(stmt, 9, 10, update->owner_set, update->owner);
    bind_update_text(stmt, 11, 12, update->target_date_set, update->target_date);
    bind_update_text(stmt, 13, 14, update->tags_set, update->tags_json);
    bind_update_text(stmt, 15, 16, update->version_label_set,
                     update->version_label);
    sqlite3_bind_int64(stmt, 17, plan_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

static int update_plan_status(sqlite3 *db,
                              sqlite3_int64 plan_id,
                              const char *status,
                              const char *mode) {
    const char *close_sql =
        "UPDATE plans SET status = ?, "
        "closed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    const char *archive_sql =
        "UPDATE plans SET status = 'archived', "
        "archived_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    const char *reopen_sql =
        "UPDATE plans SET status = 'open', closed_at = NULL, archived_at = NULL, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    const char *sql = close_sql;
    if (strcmp(mode, "archive") == 0) sql = archive_sql;
    if (strcmp(mode, "reopen") == 0) sql = reopen_sql;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    if (strcmp(mode, "close") == 0) {
        sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, plan_id);
    } else {
        sqlite3_bind_int64(stmt, 1, plan_id);
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

static char *json_print_owned(cJSON *item) {
    if (item == NULL) return NULL;
    return cJSON_PrintUnformatted(item);
}

static cJSON *result_with_plan(cJSON *plan) {
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) return NULL;
    cJSON_AddItemToObject(result, "plan", plan);
    return result;
}

static int finish_plan_change(sqlite3 *db,
                              const ipman_request_t *req,
                              cJSON *old_plan,
                              sqlite3_int64 plan_id,
                              const char *event_type,
                              const char *summary,
                              const char *details,
                              sqlite3_int64 *event_id_out,
                              cJSON **result_out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out) {
    cJSON *new_plan = load_plan(db, plan_id);
    cJSON *result = NULL;
    char *old_json = json_print_owned(old_plan);
    char *new_json = json_print_owned(new_plan);
    if (new_plan == NULL || old_json == NULL || new_json == NULL) {
        if (new_plan != NULL) cJSON_Delete(new_plan);
        if (old_json != NULL) cJSON_free(old_json);
        if (new_json != NULL) cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build plan event";
        return -1;
    }
    if (insert_plan_event(db, plan_id, event_type, req->actor, req->request_id,
                          summary, details, old_json, new_json,
                          event_id_out) != 0) {
        cJSON_Delete(new_plan);
        cJSON_free(old_json);
        cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record plan event";
        return -1;
    }
    cJSON_free(old_json);
    cJSON_free(new_json);

    result = result_with_plan(new_plan);
    if (result == NULL) {
        cJSON_Delete(new_plan);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build plan response";
        return -1;
    }
    *result_out = result;
    return 0;
}

static int commit_plan_result_owned(sqlite3 *db, cJSON **result_io,
                                    ipman_error_code_t *err_code_out,
                                    const char **err_msg_out) {
    if (run_sql(db, "COMMIT;") != 0) {
        cJSON_Delete(*result_io);
        *result_io = NULL;
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to commit plan change";
        return -1;
    }
    return 0;
}

const ipman_param_desc_t ipman_op_plan_get_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_plan_get(const ipman_request_t *req, sqlite3 *db,
                     cJSON **result_out,
                     ipman_error_code_t *err_code_out,
                     const char **err_msg_out) {
    sqlite3_int64 plan_id = 0;
    if (read_plan_selector(db, req->params, &plan_id, err_code_out, err_msg_out) != 0) return -1;
    int load_failed = 0;
    cJSON *plan = load_plan_checked(db, plan_id, &load_failed);
    if (plan == NULL) {
        *err_code_out = load_failed ? IPMAN_ERR_INTERNAL : IPMAN_ERR_NOT_FOUND;
        *err_msg_out = load_failed ? "failed to load plan" : "plan not found";
        return -1;
    }
    cJSON *result = result_with_plan(plan);
    if (result == NULL) {
        cJSON_Delete(plan);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build plan response";
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_plan_lookup_params[] = {
    { "uid" }, { "label" }, { "code" },
    { NULL },
};

int ipman_op_plan_lookup(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out) {
    if (cJSON_GetObjectItemCaseSensitive(req->params, "id") != NULL) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "id is not a valid lookup input; lookup resolves uid/label/code to id";
        return -1;
    }
    cJSON *uid_item   = cJSON_GetObjectItemCaseSensitive(req->params, "uid");
    cJSON *label_item = cJSON_GetObjectItemCaseSensitive(req->params, "label");
    cJSON *code_item  = cJSON_GetObjectItemCaseSensitive(req->params, "code");
    const char *sql = NULL;
    const char *value = NULL;
    const char *not_found_msg = NULL;
    if (uid_item && cJSON_IsString(uid_item) && uid_item->valuestring) {
        sql = "SELECT id FROM plans WHERE uid = ?";
        value = uid_item->valuestring;
        not_found_msg = "plan not found by uid";
    } else if (label_item && cJSON_IsString(label_item) && label_item->valuestring) {
        sql = "SELECT id FROM plans WHERE label = ?";
        value = label_item->valuestring;
        not_found_msg = "plan not found by label";
    } else if (code_item && cJSON_IsString(code_item) && code_item->valuestring) {
        sql = "SELECT id FROM plans WHERE code = ?";
        value = code_item->valuestring;
        not_found_msg = "plan not found by code";
    } else {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "lookup requires one of: uid, label, code";
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to prepare lookup query";
        return -1;
    }
    sqlite3_bind_text(stmt, 1, value, -1, SQLITE_STATIC);
    sqlite3_int64 plan_id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        plan_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    } else {
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = not_found_msg;
        return -1;
    }
    cJSON *result = cJSON_CreateObject();
    if (result == NULL ||
        cJSON_AddNumberToObject(result, "id", (double)plan_id) == NULL) {
        if (result != NULL) cJSON_Delete(result);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build lookup response";
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_plan_update_params[] = {
    { "id" },
    { "title" }, { "summary" }, { "description" },
    { "priority" }, { "owner" }, { "target_date" },
    { "version_label" }, { "tags" },
    { NULL },
};

int ipman_op_plan_update(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out) {
    sqlite3_int64 selector_id = 0;
    plan_update_t update;
    if (read_plan_selector(db, req->params, &selector_id, err_code_out, err_msg_out) != 0 ||
        read_plan_update(req->params, &update, err_code_out, err_msg_out) != 0) return -1;
    
    if (ipman_db_begin_immediate(db) != 0) {
        if (update.tags_json != NULL) cJSON_free(update.tags_json);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    int load_failed = 0;
    cJSON *old_plan = load_plan_checked(db, selector_id, &load_failed);
    if (old_plan == NULL) {
        if (update.tags_json != NULL) cJSON_free(update.tags_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = load_failed ? IPMAN_ERR_INTERNAL : IPMAN_ERR_NOT_FOUND;
        *err_msg_out = load_failed ? "failed to load plan" : "plan not found";
        return -1;
    }
    const char *old_status = plan_json_status(old_plan);
    if (old_status != NULL && is_terminal_plan_status(old_status)) {
        if (update.tags_json != NULL) cJSON_free(update.tags_json);
        cJSON_Delete(old_plan);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "cannot update a plan in a terminal state";
        return -1;
    }
    sqlite3_int64 plan_id = plan_json_id(old_plan);
    if (update_plan_fields(db, plan_id, &update) != 0) {
        if (update.tags_json != NULL) cJSON_free(update.tags_json);
        cJSON_Delete(old_plan);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "failed to update plan";
        return -1;
    }
    if (update.tags_json != NULL) cJSON_free(update.tags_json);
    int rc = finish_plan_change(db, req, old_plan, plan_id, "plan_updated",
                                "plan updated", "{\"op\":\"plan.update\"}",
                                NULL, result_out, err_code_out, err_msg_out);
    cJSON_Delete(old_plan);
    if (rc != 0) return -1;
    return commit_plan_result_owned(db, result_out, err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_plan_list_params[] = {
    { "status" }, { "owner" }, { "tag" },
    { "limit" }, { "offset" },
    { NULL },
};

int ipman_op_plan_list(const ipman_request_t *req, sqlite3 *db,
                      cJSON **result_out,
                      ipman_error_code_t *err_code_out,
                      const char **err_msg_out) {
    const char *status = NULL;
    const char *owner = NULL;
    const char *tag = NULL;
    int limit = PLAN_LIST_DEFAULT_LIMIT;
    int offset = 0;
    if (read_optional_filter(req->params, "status", &status,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_filter(req->params, "owner", &owner,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_filter(req->params, "tag", &tag,
                             err_code_out, err_msg_out) != 0 ||
        read_list_int(req->params, "limit", PLAN_LIST_DEFAULT_LIMIT, &limit,
                      err_code_out, err_msg_out) != 0 ||
        read_list_int(req->params, "offset", 0, &offset,
                      err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (status != NULL && !is_plan_status(status)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "status must be a valid plan status";
        return -1;
    }
    if (limit < 1 || limit > PLAN_LIST_MAX_LIMIT) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "limit must be between 1 and 500";
        return -1;
    }

    /* Count total matching rows */
    int rc = 0;
    const char *count_sql =
        "SELECT COUNT(*) FROM plans "
        "WHERE ((? IS NULL AND status <> 'archived') "
        "OR (? IS NOT NULL AND status = ?)) "
        "AND (? IS NULL OR owner = ?) "
        "AND (? IS NULL OR EXISTS ("
        "    SELECT 1 FROM json_each(plans.tags) WHERE value = ?"
        "));";
    sqlite3_stmt *count_stmt = NULL;
    rc = sqlite3_prepare_v2(db, count_sql, -1, &count_stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to count plans";
        return -1;
    }
    bind_optional_text(count_stmt, 1, status);
    bind_optional_text(count_stmt, 2, status);
    bind_optional_text(count_stmt, 3, status);
    bind_optional_text(count_stmt, 4, owner);
    bind_optional_text(count_stmt, 5, owner);
    bind_optional_text(count_stmt, 6, tag);
    bind_optional_text(count_stmt, 7, tag);
    rc = sqlite3_step(count_stmt);
    int total_count = (rc == SQLITE_ROW) ? sqlite3_column_int(count_stmt, 0) : 0;
    sqlite3_finalize(count_stmt);

    const char *sql =
        "SELECT id, code, title, summary, description, status, priority, "
        "created_at, updated_at, opened_at, closed_at, archived_at, owner, "
        "target_date, tags, version_label, uid, label "
        "FROM plans "
        "WHERE ((? IS NULL AND status <> 'archived') "
        "OR (? IS NOT NULL AND status = ?)) "
        "AND (? IS NULL OR owner = ?) "
        "AND (? IS NULL OR EXISTS ("
        "    SELECT 1 FROM json_each(plans.tags) WHERE value = ?"
        ")) "
        "ORDER BY updated_at DESC, id DESC LIMIT ? OFFSET ?;";
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query plans";
        return -1;
    }
    bind_optional_text(stmt, 1, status);
    bind_optional_text(stmt, 2, status);
    bind_optional_text(stmt, 3, status);
    bind_optional_text(stmt, 4, owner);
    bind_optional_text(stmt, 5, owner);
    bind_optional_text(stmt, 6, tag);
    bind_optional_text(stmt, 7, tag);
    sqlite3_bind_int(stmt, 8, limit + 1);
    sqlite3_bind_int(stmt, 9, offset);

    cJSON *result = cJSON_CreateObject();
    cJSON *plans = cJSON_CreateArray();
    if (result == NULL || plans == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (plans != NULL) cJSON_Delete(plans);
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build plan list";
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *plan = plan_from_row(stmt);
        if (plan == NULL) {
            cJSON_Delete(result);
            cJSON_Delete(plans);
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build plan list";
            return -1;
        }
        cJSON_AddItemToArray(plans, plan);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(result);
        cJSON_Delete(plans);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query plans";
        return -1;
    }
    int has_more = 0;
    int row_count = cJSON_GetArraySize(plans);
    if (row_count > limit) {
        has_more = 1;
        cJSON *last = cJSON_DetachItemFromArray(plans, row_count - 1);
        cJSON_Delete(last);
    }
    cJSON_AddItemToObject(result, "plans", plans);
    cJSON_AddNumberToObject(result, "limit", limit);
    cJSON_AddNumberToObject(result, "offset", offset);
    cJSON_AddBoolToObject(result, "has_more", has_more);
    cJSON_AddNumberToObject(result, "total_count", total_count);
    *result_out = result;
    return 0;
}

typedef struct {
    const char *outcome_summary;
    const char *closing_comment;
    const char *lessons_learned;
    const char *open_items_summary;
    int followup_needed;
} plan_close_t;

static int read_required_string_local(cJSON *params, const char *field,
                                      const char **out,
                                      ipman_error_code_t *err_code_out,
                                      const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, field);
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        is_blank(item->valuestring)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "closure text fields must be non-empty strings";
        return -1;
    }
    *out = item->valuestring;
    return 0;
}

static int read_plan_close(cJSON *params, plan_close_t *close_data,
                           ipman_error_code_t *err_code_out,
                           const char **err_msg_out) {
    memset(close_data, 0, sizeof *close_data);
    if (read_required_string_local(params, "outcome_summary",
                                   &close_data->outcome_summary,
                                   err_code_out, err_msg_out) != 0 ||
        read_required_string_local(params, "closing_comment",
                                   &close_data->closing_comment,
                                   err_code_out, err_msg_out) != 0 ||
        read_optional_string(params, "lessons_learned",
                             &close_data->lessons_learned,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(params, "open_items_summary",
                             &close_data->open_items_summary,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }

    cJSON *followup = cJSON_GetObjectItemCaseSensitive(params, "followup_needed");
    if (followup == NULL || cJSON_IsNull(followup)) {
        close_data->followup_needed = 0;
        return 0;
    }
    if (!cJSON_IsBool(followup)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "followup_needed must be a boolean";
        return -1;
    }
    close_data->followup_needed = cJSON_IsTrue(followup) ? 1 : 0;
    return 0;
}

static int insert_plan_closure_record(sqlite3 *db,
                                      sqlite3_int64 plan_id,
                                      const char *status,
                                      const plan_close_t *close_data,
                                      const char *actor,
                                      sqlite3_int64 event_id) {
    const char *sql =
        "INSERT INTO closure_records("
        "entity_type, entity_id, closure_status, resolution, outcome_summary, "
        "closing_comment, lessons_learned, open_items_summary, "
        "followup_needed, author, event_id"
        ") VALUES ('plan', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, plan_id);
    sqlite3_bind_text(stmt, 2, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_null(stmt, 3);
    sqlite3_bind_text(stmt, 4, close_data->outcome_summary, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, close_data->closing_comment, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 6, close_data->lessons_learned);
    bind_optional_text(stmt, 7, close_data->open_items_summary);
    sqlite3_bind_int(stmt, 8, close_data->followup_needed);
    sqlite3_bind_text(stmt, 9, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 10, event_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int maybe_clear_active_plan(sqlite3 *db, sqlite3_int64 plan_id,
                                   const char *actor) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "UPDATE workspace_context SET active_plan_id = NULL, updated_by = ?"
        " WHERE id = 1 AND active_plan_id = ?;";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, actor, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, plan_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

static int plan_transition(const ipman_request_t *req,
                           sqlite3 *db,
                           const char *mode,
                           cJSON **result_out,
                           ipman_error_code_t *err_code_out,
                           const char **err_msg_out) {
    sqlite3_int64 selector_id = 0;
    if (read_plan_selector(db, req->params, &selector_id, err_code_out, err_msg_out) != 0) return -1;
    const char *outcome = "completed";
    plan_close_t close_data;
    memset(&close_data, 0, sizeof close_data);
    if (strcmp(mode, "close") == 0) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(req->params, "outcome");
        if (item != NULL && !cJSON_IsNull(item)) {
            if (!cJSON_IsString(item) || item->valuestring == NULL ||
                (strcmp(item->valuestring, "completed") != 0 &&
                 strcmp(item->valuestring, "canceled") != 0)) {
                *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
                *err_msg_out = "outcome must be completed or canceled";
                return -1;
            }
            outcome = item->valuestring;
        }
        if (read_plan_close(req->params, &close_data,
                            err_code_out, err_msg_out) != 0) {
            return -1;
        }
    }

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    cJSON *old_plan = load_plan(db, selector_id);
    if (old_plan == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "plan not found";
        return -1;
    }
    sqlite3_int64 plan_id = plan_json_id(old_plan);
    const char *status = plan_json_status(old_plan);
    int allowed = 0;
    if (strcmp(mode, "close") == 0) {
        allowed = status != NULL &&
                  (strcmp(status, "open") == 0 ||
                   strcmp(status, "in_progress") == 0 ||
                   strcmp(status, "paused") == 0);
    } else if (strcmp(mode, "archive") == 0) {
        allowed = status != NULL &&
                  (strcmp(status, "completed") == 0 ||
                   strcmp(status, "canceled") == 0);
    } else {
        allowed = status != NULL &&
                  (strcmp(status, "completed") == 0 ||
                   strcmp(status, "canceled") == 0 ||
                   strcmp(status, "archived") == 0);
    }
    if (!allowed) {
        cJSON_Delete(old_plan);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "invalid plan transition";
        return -1;
    }

    const char *next_status = outcome;
    if (strcmp(mode, "archive") == 0) next_status = "archived";
    if (strcmp(mode, "reopen") == 0) next_status = "open";
    if (update_plan_status(db, plan_id, next_status, mode) != 0) {
        cJSON_Delete(old_plan);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "failed to update plan status";
        return -1;
    }

    if (strcmp(mode, "reopen") != 0) {
        if (maybe_clear_active_plan(db, plan_id, req->actor) != 0) {
            cJSON_Delete(old_plan);
            run_sql(db, "ROLLBACK;");
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to clear active plan";
            return -1;
        }
    }

    const char *event_type = "plan_closed";
    const char *summary = "plan closed";
    const char *details = "{\"op\":\"plan.close\"}";
    if (strcmp(mode, "archive") == 0) {
        event_type = "plan_archived";
        summary = "plan archived";
        details = "{\"op\":\"plan.archive\"}";
    } else if (strcmp(mode, "reopen") == 0) {
        event_type = "plan_reopened";
        summary = "plan reopened";
        details = "{\"op\":\"plan.reopen\"}";
    }
    sqlite3_int64 event_id = 0;
    int rc = finish_plan_change(db, req, old_plan, plan_id, event_type,
                                summary, details, &event_id, result_out,
                                err_code_out, err_msg_out);
    cJSON_Delete(old_plan);
    if (rc != 0) return -1;
    if (strcmp(mode, "close") == 0) {
        if (insert_plan_closure_record(db, plan_id, outcome, &close_data,
                                       req->actor, event_id) != 0) {
            cJSON_Delete(*result_out);
            *result_out = NULL;
            run_sql(db, "ROLLBACK;");
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to record plan closure";
            return -1;
        }
    }
    return commit_plan_result_owned(db, result_out, err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_plan_close_params[] = {
    { "id" }, { "outcome" },
    { "outcome_summary" }, { "closing_comment" },
    { "lessons_learned" }, { "open_items_summary" },
    { "followup_needed" },
    { NULL },
};

int ipman_op_plan_close(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out) {
    return plan_transition(req, db, "close", result_out,
                           err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_plan_archive_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_plan_archive(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out) {
    return plan_transition(req, db, "archive", result_out,
                           err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_plan_reopen_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_plan_reopen(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out) {
    return plan_transition(req, db, "reopen", result_out,
                           err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_plan_history_params[] = {
    { "id" }, { "since" },
    { NULL },
};

int ipman_op_plan_history(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out) {
    sqlite3_int64 selector_id = 0;
    sqlite3_int64 since = 0;
    if (read_plan_selector(db, req->params, &selector_id, err_code_out, err_msg_out) != 0 ||
        read_optional_since(req->params, &since, err_code_out, err_msg_out) != 0) return -1;
    cJSON *plan = load_plan(db, selector_id);
    if (plan == NULL) {
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "plan not found";
        return -1;
    }
    sqlite3_int64 plan_id = plan_json_id(plan);
    cJSON_Delete(plan);

    const char *sql =
        "SELECT id, entity_type, entity_id, event_type, actor, event_at, "
        "summary, details, old_value, new_value, related_entity_type, "
        "related_entity_id, request_id "
        "FROM events "
        "WHERE entity_type = 'plan' AND entity_id = ? AND (? = 0 OR id > ?) "
        "ORDER BY id DESC;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query plan history";
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, plan_id);
    sqlite3_bind_int64(stmt, 2, since);
    sqlite3_bind_int64(stmt, 3, since);

    cJSON *result = cJSON_CreateObject();
    cJSON *events = cJSON_CreateArray();
    if (result == NULL || events == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (events != NULL) cJSON_Delete(events);
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build plan history";
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *event = ipman_event_from_row(stmt);
        if (event == NULL) {
            cJSON_Delete(result);
            cJSON_Delete(events);
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build plan history";
            return -1;
        }
        ipman_attach_entity_ref(db, event, "entity_type", "entity_id",
                               "entity_ref");
        ipman_attach_entity_ref(db, event, "related_entity_type",
                               "related_entity_id", "related_entity_ref");
        cJSON_AddItemToArray(events, event);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(result);
        cJSON_Delete(events);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query plan history";
        return -1;
    }
    cJSON_AddItemToObject(result, "events", events);
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_plan_progress_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_plan_progress(const ipman_request_t *req, sqlite3 *db,
                          cJSON **result_out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out) {
    sqlite3_int64 selector_id = 0;
    if (read_plan_selector(db, req->params, &selector_id, err_code_out, err_msg_out) != 0) return -1;
    cJSON *plan = load_plan(db, selector_id);
    if (plan == NULL) {
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "plan not found";
        return -1;
    }
    sqlite3_int64 plan_id = plan_json_id(plan);
    cJSON_Delete(plan);

    const char *sql =
        "SELECT status, COUNT(*) FROM tasks WHERE plan_id = ? GROUP BY status;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query plan progress";
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, plan_id);

    sqlite3_int64 todo = 0;
    sqlite3_int64 in_progress = 0;
    sqlite3_int64 blocked = 0;
    sqlite3_int64 deferred = 0;
    sqlite3_int64 done = 0;
    sqlite3_int64 canceled = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *status = (const char *)sqlite3_column_text(stmt, 0);
        sqlite3_int64 count = sqlite3_column_int64(stmt, 1);
        if (status == NULL) continue;
        if (strcmp(status, "todo") == 0) todo = count;
        else if (strcmp(status, "in_progress") == 0) in_progress = count;
        else if (strcmp(status, "blocked") == 0) blocked = count;
        else if (strcmp(status, "deferred") == 0) deferred = count;
        else if (strcmp(status, "done") == 0) done = count;
        else if (strcmp(status, "canceled") == 0) canceled = count;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query plan progress";
        return -1;
    }

    sqlite3_int64 total = todo + in_progress + blocked + deferred + done + canceled;
    double percentage = total == 0 ? 0.0 : ((double)done * 100.0) / (double)total;
    cJSON *result = cJSON_CreateObject();
    cJSON *counts = cJSON_CreateObject();
    if (result == NULL || counts == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (counts != NULL) cJSON_Delete(counts);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build plan progress";
        return -1;
    }
    cJSON_AddNumberToObject(result, "plan_id", (double)plan_id);
    cJSON_AddNumberToObject(counts, "todo", (double)todo);
    cJSON_AddNumberToObject(counts, "in_progress", (double)in_progress);
    cJSON_AddNumberToObject(counts, "blocked", (double)blocked);
    cJSON_AddNumberToObject(counts, "deferred", (double)deferred);
    cJSON_AddNumberToObject(counts, "done", (double)done);
    cJSON_AddNumberToObject(counts, "canceled", (double)canceled);
    cJSON_AddItemToObject(result, "counts", counts);
    cJSON_AddNumberToObject(result, "total", (double)total);
    cJSON_AddNumberToObject(result, "completion_percentage", percentage);
    *result_out = result;
    return 0;
}
