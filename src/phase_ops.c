/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "phase_ops.h"
#include "context_ops.h"
#include "db.h"
#include "event_ops.h"
#include "json_helpers.h"
#include "plan_ops.h"
#include "task_ops.h"
#include "validation.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define PHASE_LIST_DEFAULT_LIMIT 100
#define PHASE_LIST_MAX_LIMIT 500

typedef struct {
    int title_set;
    const char *title;
    int summary_set;
    const char *summary;
    int description_set;
    const char *description;
    int owner_set;
    const char *owner;
    int target_start_date_set;
    const char *target_start_date;
    int target_end_date_set;
    const char *target_end_date;
} phase_update_t;

typedef struct {
    const char *outcome_summary;
    const char *closing_comment;
    const char *lessons_learned;
    const char *open_items_summary;
    int followup_needed;
} phase_close_t;

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

static int is_phase_status(const char *value) {
    static const char *allowed[] = {
        "open", "in_progress", "blocked", "completed", "canceled",
    };
    size_t count = sizeof allowed / sizeof allowed[0];
    for (size_t pos = 0; pos < count; ++pos) {
        if (strcmp(value, allowed[pos]) == 0) return 1;
    }
    return 0;
}

static int is_terminal_phase_status(const char *status) {
    return strcmp(status, "completed") == 0 ||
           strcmp(status, "canceled") == 0;
}

static ipman_error_code_t create_sqlite_error_code(sqlite3 *db, int rc) {
    int extended = sqlite3_extended_errcode(db);
    if (rc == SQLITE_CONSTRAINT ||
        (extended & 0xff) == SQLITE_CONSTRAINT) {
        return IPMAN_ERR_CONFLICT;
    }
    return IPMAN_ERR_INTERNAL;
}

static int is_task_status(const char *value) {
    static const char *allowed[] = {
        "todo", "in_progress", "blocked", "deferred", "done", "canceled",
    };
    size_t count = sizeof allowed / sizeof allowed[0];
    for (size_t pos = 0; pos < count; ++pos) {
        if (strcmp(value, allowed[pos]) == 0) return 1;
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

int ipman_read_phase_selector(cJSON *params, sqlite3 *db,
                             sqlite3_int64 *phase_id_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out) {
    (void)db;
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!id_item || !cJSON_IsNumber(id_item)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "id required (use phase.lookup to resolve uid/label to id)";
        return -1;
    }
    if (id_item->valuedouble < 1.0) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "id must be a positive integer";
        return -1;
    }
    *phase_id_out = (sqlite3_int64)id_item->valuedouble;
    return 0;
}

static int read_phase_list_tasks_id(cJSON *params,
                                    sqlite3 *db,
                                    sqlite3_int64 *phase_id_out,
                                    ipman_error_code_t *err_code_out,
                                    const char **err_msg_out) {
    (void)db;
    cJSON *id = cJSON_GetObjectItemCaseSensitive(params, "id");
    cJSON *phase_id = cJSON_GetObjectItemCaseSensitive(params, "phase_id");
    int has_id = id != NULL && !cJSON_IsNull(id);
    int has_phase_id = phase_id != NULL && !cJSON_IsNull(phase_id);

    if (!has_id && !has_phase_id) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "id or phase_id is required";
        return -1;
    }
    sqlite3_int64 id_value = 0;
    sqlite3_int64 phase_id_value = 0;
    if (has_id) {
        if (!cJSON_IsNumber(id) || id->valuedouble < 1.0) {
            *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
            *err_msg_out = "id must be a positive integer";
            return -1;
        }
        id_value = (sqlite3_int64)id->valuedouble;
    }
    if (has_phase_id) {
        if (!cJSON_IsNumber(phase_id) || phase_id->valuedouble < 1.0) {
            *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
            *err_msg_out = "phase_id must be a positive integer";
            return -1;
        }
        phase_id_value = (sqlite3_int64)phase_id->valuedouble;
    }
    if (has_id && has_phase_id && id_value != phase_id_value) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "id and phase_id must match";
        return -1;
    }
    *phase_id_out = has_id ? id_value : phase_id_value;
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
        *err_msg_out = "id must be a positive integer or null";
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

static int read_optional_bool(cJSON *params, const char *field,
                              int *set_out,
                              int *value_out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, field);
    *set_out = 0;
    *value_out = 0;
    if (item == NULL || cJSON_IsNull(item)) return 0;
    if (!cJSON_IsBool(item)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "boolean filters must be true or false";
        return -1;
    }
    *set_out = 1;
    *value_out = cJSON_IsTrue(item) ? 1 : 0;
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
    if ((strcmp(field, "target_start_date") == 0 ||
         strcmp(field, "target_end_date") == 0) &&
        ipman_validate_date_field(item->valuestring,
                                 err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *out = item->valuestring;
    return 0;
}

static int read_required_string(cJSON *params, const char *field,
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

static int read_optional_phase_status(cJSON *params, const char **out,
                                      ipman_error_code_t *err_code_out,
                                      const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "status");
    if (item == NULL || cJSON_IsNull(item)) {
        *out = "open";
        return 0;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        !is_phase_status(item->valuestring)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "status must be a valid phase status";
        return -1;
    }
    *out = item->valuestring;
    return 0;
}

static int read_optional_sequence(cJSON *params, sqlite3_int64 *out,
                                  ipman_error_code_t *err_code_out,
                                  const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "sequence_no");
    if (item == NULL || cJSON_IsNull(item)) {
        *out = 0;
        return 0;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 1.0) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "sequence_no must be a positive integer";
        return -1;
    }
    *out = (sqlite3_int64)item->valuedouble;
    return 0;
}

static int read_required_move_sequence(cJSON *params,
                                       sqlite3_int64 *out,
                                       ipman_error_code_t *err_code_out,
                                       const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "sequence_no");
    if (item == NULL || cJSON_IsNull(item)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "sequence_no is required";
        return -1;
    }
    if (!cJSON_IsNumber(item)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "sequence_no must be a number";
        return -1;
    }
    if (item->valuedouble < 1.0 ||
        item->valuedouble != (double)(sqlite3_int64)item->valuedouble) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "sequence_no must be a positive integer";
        return -1;
    }
    *out = (sqlite3_int64)item->valuedouble;
    return 0;
}

static int read_optional_since(cJSON *params, sqlite3_int64 *out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "since");
    if (item == NULL || cJSON_IsNull(item)) {
        *out = 0;
        return 0;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 1.0) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "since must be a positive event id";
        return -1;
    }
    *out = (sqlite3_int64)item->valuedouble;
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
    if ((strcmp(field, "target_start_date") == 0 ||
         strcmp(field, "target_end_date") == 0) &&
        ipman_validate_date_field(item->valuestring,
                                 err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *value_out = item->valuestring;
    return 0;
}

static int read_phase_update(cJSON *params, phase_update_t *update,
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
        read_update_string(params, "owner", 1,
                           &update->owner_set, &update->owner,
                           err_code_out, err_msg_out) != 0 ||
        read_update_string(params, "target_start_date", 1,
                           &update->target_start_date_set,
                           &update->target_start_date,
                           err_code_out, err_msg_out) != 0 ||
        read_update_string(params, "target_end_date", 1,
                           &update->target_end_date_set,
                           &update->target_end_date,
                           err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (!update->title_set &&
        !update->summary_set &&
        !update->description_set &&
        !update->owner_set &&
        !update->target_start_date_set &&
        !update->target_end_date_set) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "at least one updatable field is required";
        return -1;
    }
    return 0;
}

static int read_phase_close(cJSON *params, phase_close_t *close_data,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out) {
    memset(close_data, 0, sizeof *close_data);
    if (read_required_string(params, "outcome_summary",
                             &close_data->outcome_summary,
                             err_code_out, err_msg_out) != 0 ||
        read_required_string(params, "closing_comment",
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

cJSON *ipman_phase_from_row(sqlite3_stmt *stmt) {
    cJSON *phase = cJSON_CreateObject();
    if (phase == NULL) return NULL;

    sqlite3_int64 phase_id = sqlite3_column_int64(stmt, 0);
    sqlite3_int64 plan_id  = sqlite3_column_int64(stmt, 1);
    sqlite3_int64 seq      = sqlite3_column_int64(stmt, 6);
    sqlite3_int64 local_seq = sqlite3_column_int64(stmt, 14);

    /* uid/label first; numeric ids follow. */
    ipman_json_add_text_or_null(phase, "uid",               sqlite3_column_text(stmt, 15));
    ipman_json_add_text_or_null(phase, "label",             sqlite3_column_text(stmt, 16));
    cJSON_AddNumberToObject(phase, "id", (double)phase_id);
    cJSON_AddNumberToObject(phase, "plan_id", (double)plan_id);
    ipman_json_add_text_or_null(phase, "title", sqlite3_column_text(stmt, 2));
    ipman_json_add_text_or_null(phase, "summary", sqlite3_column_text(stmt, 3));
    ipman_json_add_text_or_null(phase, "description", sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null(phase, "status", sqlite3_column_text(stmt, 5));
    cJSON_AddNumberToObject(phase, "sequence_no", (double)seq);
    cJSON_AddNumberToObject(phase, "local_seq", (double)local_seq);
    ipman_json_add_text_or_null(phase, "created_at", sqlite3_column_text(stmt, 7));
    ipman_json_add_text_or_null(phase, "updated_at", sqlite3_column_text(stmt, 8));
    ipman_json_add_text_or_null(phase, "opened_at", sqlite3_column_text(stmt, 9));
    ipman_json_add_text_or_null(phase, "closed_at", sqlite3_column_text(stmt, 10));
    ipman_json_add_text_or_null(phase, "owner", sqlite3_column_text(stmt, 11));
    ipman_json_add_text_or_null(phase, "target_start_date", sqlite3_column_text(stmt, 12));
    ipman_json_add_text_or_null(phase, "target_end_date",   sqlite3_column_text(stmt, 13));
    return phase;
}

static cJSON *load_phase_checked(sqlite3 *db,
                                 sqlite3_int64 phase_id,
                                 int *load_failed_out) {
    if (load_failed_out != NULL) *load_failed_out = 0;
    const char *sql =
        "SELECT p.id, p.plan_id, p.title, p.summary, p.description, p.status, "
        "p.sequence_no, p.created_at, p.updated_at, p.opened_at, p.closed_at, "
        "p.owner, p.target_start_date, p.target_end_date, p.local_seq, "
        "p.uid, p.label "
        "FROM phases p "
        "WHERE p.id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (load_failed_out != NULL) *load_failed_out = 1;
        return NULL;
    }
    sqlite3_bind_int64(stmt, 1, phase_id);
    rc = sqlite3_step(stmt);
    cJSON *phase = NULL;
    if (rc == SQLITE_ROW) {
        phase = ipman_phase_from_row(stmt);
        if (phase == NULL && load_failed_out != NULL) *load_failed_out = 1;
    } else if (rc != SQLITE_DONE && load_failed_out != NULL) {
        *load_failed_out = 1;
    }
    sqlite3_finalize(stmt);
    return phase;
}

static cJSON *load_phase(sqlite3 *db, sqlite3_int64 phase_id) {
    return load_phase_checked(db, phase_id, NULL);
}

static sqlite3_int64 phase_json_plan_id(cJSON *phase) {
    cJSON *plan_id = cJSON_GetObjectItemCaseSensitive(phase, "plan_id");
    if (!cJSON_IsNumber(plan_id)) return 0;
    return (sqlite3_int64)plan_id->valuedouble;
}

static sqlite3_int64 phase_json_sequence(cJSON *phase) {
    cJSON *sequence = cJSON_GetObjectItemCaseSensitive(phase, "sequence_no");
    if (!cJSON_IsNumber(sequence)) return 0;
    return (sqlite3_int64)sequence->valuedouble;
}

static const char *phase_json_status(cJSON *phase) {
    cJSON *status = cJSON_GetObjectItemCaseSensitive(phase, "status");
    if (!cJSON_IsString(status)) return NULL;
    return status->valuestring;
}

static int plan_exists(sqlite3 *db, sqlite3_int64 plan_id) {
    const char *sql = "SELECT 1 FROM plans WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, plan_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_ROW) return 1;
    if (rc == SQLITE_DONE) return 0;
    return -1;
}

static int plan_is_terminal(sqlite3 *db, sqlite3_int64 plan_id) {
    const char *sql =
        "SELECT 1 FROM plans WHERE id = ? "
        "AND status IN ('archived', 'canceled', 'completed');";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, plan_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_ROW) return 1;
    if (rc == SQLITE_DONE) return 0;
    return -1;
}

static sqlite3_int64 phase_count(sqlite3 *db, sqlite3_int64 plan_id) {
    const char *sql = "SELECT COUNT(*) FROM phases WHERE plan_id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, plan_id);
    rc = sqlite3_step(stmt);
    sqlite3_int64 count = -1;
    if (rc == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

static sqlite3_int64 phase_max_sequence(sqlite3 *db, sqlite3_int64 plan_id) {
    const char *sql =
        "SELECT COALESCE(MAX(sequence_no), 0) FROM phases WHERE plan_id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, plan_id);
    rc = sqlite3_step(stmt);
    sqlite3_int64 max_sequence = -1;
    if (rc == SQLITE_ROW) max_sequence = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return max_sequence;
}


static sqlite3_int64 get_next_phase_sequence_no(sqlite3 *db, sqlite3_int64 plan_id) {
    sqlite3_int64 max_seq = phase_max_sequence(db, plan_id);
    return max_seq < 0 ? -1 : max_seq + 1;
}

static int insert_phase(sqlite3 *db,
                        sqlite3_int64 plan_id,
                        const char *title,
                        const char *summary,
                        const char *description,
                        const char *status,
                        sqlite3_int64 sequence_no,
                        const char *owner,
                        const char *target_start_date,
                        const char *target_end_date,
                        const char *label,
                        sqlite3_int64 *phase_id_out) {
    const char *sql =
        "INSERT INTO phases("
        "plan_id, title, summary, description, status, sequence_no, owner, "
        "target_start_date, target_end_date, label"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, plan_id);
    sqlite3_bind_text(stmt, 2, title, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 3, summary);
    bind_optional_text(stmt, 4, description);
    sqlite3_bind_text(stmt, 5, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, sequence_no);
    bind_optional_text(stmt, 7, owner);
    bind_optional_text(stmt, 8, target_start_date);
    bind_optional_text(stmt, 9, target_end_date);
    
    char generated_label[64] = {0};
    if (label != NULL && !is_blank(label)) {
        sqlite3_bind_text(stmt, 10, label, -1, SQLITE_TRANSIENT);
    } else {
        ipman_slugify(title, generated_label, sizeof(generated_label));
        if (generated_label[0] == '\0') {
            sqlite3_bind_null(stmt, 10);
        } else {
            sqlite3_bind_text(stmt, 10, generated_label, -1, SQLITE_TRANSIENT);
        }
    }
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return rc;
    sqlite3_int64 new_id = sqlite3_last_insert_rowid(db);
    *phase_id_out = new_id;
    
    char uid_buf[64];
    snprintf(uid_buf, sizeof(uid_buf), "phase_%lld", (long long)new_id);
    
    if (label == NULL || is_blank(label)) {
        if (generated_label[0] == '\0') {
            snprintf(generated_label, sizeof(generated_label), "phase-%lld", (long long)new_id);
        }
        const char *update_sql = "UPDATE phases SET uid = ?, label = ? WHERE id = ?;";
        rc = sqlite3_prepare_v2(db, update_sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) return rc;
        sqlite3_bind_text(stmt, 1, uid_buf, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, generated_label, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, new_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) return rc;
    } else {
        const char *update_sql = "UPDATE phases SET uid = ? WHERE id = ?;";
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

static int insert_phase_event(sqlite3 *db,
                              sqlite3_int64 phase_id,
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
        ") VALUES ('phase', ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, phase_id);
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

static char *json_print_owned(cJSON *item) {
    if (item == NULL) return NULL;
    return cJSON_PrintUnformatted(item);
}

static cJSON *result_with_phase(cJSON *phase) {
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) return NULL;
    cJSON_AddItemToObject(result, "phase", phase);
    return result;
}

static int finish_phase_change(sqlite3 *db,
                               const ipman_request_t *req,
                               cJSON *old_phase,
                               sqlite3_int64 phase_id,
                               const char *event_type,
                               const char *summary,
                               const char *details,
                               sqlite3_int64 *event_id_out,
                               cJSON **result_out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out) {
    cJSON *new_phase = load_phase(db, phase_id);
    cJSON *result = NULL;
    char *old_json = json_print_owned(old_phase);
    char *new_json = json_print_owned(new_phase);
    if (new_phase == NULL || old_json == NULL || new_json == NULL) {
        if (new_phase != NULL) cJSON_Delete(new_phase);
        if (old_json != NULL) cJSON_free(old_json);
        if (new_json != NULL) cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build phase event";
        return -1;
    }
    if (insert_phase_event(db, phase_id, event_type, req->actor,
                           req->request_id, summary, details, old_json,
                           new_json, event_id_out) != 0) {
        cJSON_Delete(new_phase);
        cJSON_free(old_json);
        cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record phase event";
        return -1;
    }
    cJSON_free(old_json);
    cJSON_free(new_json);

    result = result_with_phase(new_phase);
    if (result == NULL) {
        cJSON_Delete(new_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build phase response";
        return -1;
    }
    *result_out = result;
    return 0;
}

static int commit_result_owned(sqlite3 *db, cJSON **result_io,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out) {
    if (run_sql(db, "COMMIT;") != 0) {
        cJSON_Delete(*result_io);
        *result_io = NULL;
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to commit phase change";
        return -1;
    }
    return 0;
}

const ipman_param_desc_t ipman_op_phase_create_params[] = {
    { "plan_id" }, { "title" }, { "summary" }, { "description" },
    { "status" }, { "sequence_no" }, { "owner" },
    { "target_start_date" }, { "target_end_date" }, { "label" },
    { NULL },
};

int ipman_op_phase_create(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out) {
    sqlite3_int64 plan_id = 0;
    sqlite3_int64 sequence_no = 0;
    const char *title = NULL;
    const char *summary = NULL;
    const char *description = NULL;
    const char *status = NULL;
    const char *owner = NULL;
    const char *target_start_date = NULL;
    const char *target_end_date = NULL;
    const char *label = NULL;

    if (ipman_read_positive_id(req->params, "plan_id", &plan_id,
                         err_code_out, err_msg_out) != 0 ||
        read_required_title(req->params, &title, err_code_out, err_msg_out) != 0 ||
        read_optional_phase_status(req->params, &status,
                                   err_code_out, err_msg_out) != 0 ||
        read_optional_sequence(req->params, &sequence_no,
                               err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "summary", &summary,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "description", &description,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "owner", &owner,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "target_start_date",
                             &target_start_date, err_code_out,
                             err_msg_out) != 0 ||
        read_optional_string(req->params, "target_end_date",
                             &target_end_date, err_code_out,
                             err_msg_out) != 0 ||
        read_optional_string(req->params, "label",
                             &label, err_code_out,
                             err_msg_out) != 0) {
        return -1;
    }
    if (strcmp(status, "open") != 0) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "phase.create status must be open";
        return -1;
    }

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    int exists = plan_exists(db, plan_id);
    if (exists == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "plan not found";
        return -1;
    }
    if (exists < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to check plan";
        return -1;
    }
    int terminal = plan_is_terminal(db, plan_id);
    if (terminal < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to check plan status";
        return -1;
    }
    if (terminal) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "cannot create phase in a terminal plan";
        return -1;
    }

    if (sequence_no == 0) {
        sequence_no = get_next_phase_sequence_no(db, plan_id);
        if (sequence_no <= 0) {
            run_sql(db, "ROLLBACK;");
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to calculate sequence_no";
            return -1;
        }
    }

    sqlite3_int64 phase_id = 0;
    int insert_rc = insert_phase(db, plan_id, title, summary, description,
                                 status, sequence_no, owner, target_start_date,
                                 target_end_date, label, &phase_id);
    if (insert_rc != 0) {
        fprintf(stderr, "INSERT PHASE SQLITE ERROR: %s\n", sqlite3_errmsg(db));
        run_sql(db, "ROLLBACK;");
        *err_code_out = insert_rc == -2 ? IPMAN_ERR_VALIDATION_FAILED
                                        : create_sqlite_error_code(db, insert_rc);
        *err_msg_out = insert_rc == -2 ? "sequence_no is outside phase range"
                                       : "failed to create phase";
        return -1;
    }

    cJSON *phase = load_phase(db, phase_id);
    cJSON *result = cJSON_CreateObject();
    char *phase_json = json_print_owned(phase);
    if (phase == NULL || result == NULL || phase_json == NULL) {
        if (phase != NULL) cJSON_Delete(phase);
        if (result != NULL) cJSON_Delete(result);
        if (phase_json != NULL) cJSON_free(phase_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build phase response";
        return -1;
    }
    if (insert_phase_event(db, phase_id, "phase_created", req->actor,
                           req->request_id, "phase created",
                           "{\"op\":\"phase.create\"}", NULL, phase_json,
                           NULL) != 0) {
        cJSON_free(phase_json);
        cJSON_Delete(phase);
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record phase event";
        return -1;
    }
    cJSON_free(phase_json);
    cJSON_AddItemToObject(result, "phase", phase);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_phase_get_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_phase_get(const ipman_request_t *req, sqlite3 *db,
                      cJSON **result_out,
                      ipman_error_code_t *err_code_out,
                      const char **err_msg_out) {
    sqlite3_int64 phase_id = 0;
    if (ipman_read_phase_selector(req->params, db, &phase_id,
                         err_code_out, err_msg_out) != 0) {
        return -1;
    }
    int load_failed = 0;
    cJSON *phase = load_phase_checked(db, phase_id, &load_failed);
    if (phase == NULL) {
        *err_code_out = load_failed ? IPMAN_ERR_INTERNAL : IPMAN_ERR_NOT_FOUND;
        *err_msg_out = load_failed ? "failed to load phase" : "phase not found";
        return -1;
    }
    cJSON *result = result_with_phase(phase);
    if (result == NULL) {
        cJSON_Delete(phase);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build phase response";
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_phase_lookup_params[] = {
    { "uid" }, { "label" },
    { "plan_id" },
    { NULL },
};

int ipman_op_phase_lookup(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out) {
    if (cJSON_GetObjectItemCaseSensitive(req->params, "id") != NULL) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "id is not a valid lookup input; lookup resolves uid/label to id";
        return -1;
    }
    cJSON *uid_item = cJSON_GetObjectItemCaseSensitive(req->params, "uid");
    cJSON *label_item = cJSON_GetObjectItemCaseSensitive(req->params, "label");
    int has_uid = uid_item != NULL && cJSON_IsString(uid_item) &&
                  uid_item->valuestring && uid_item->valuestring[0] != '\0';
    int has_label = label_item != NULL && cJSON_IsString(label_item) &&
                    label_item->valuestring && label_item->valuestring[0] != '\0';
    if (!has_uid && !has_label) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "lookup requires uid or label";
        return -1;
    }

    sqlite3_int64 phase_id = 0;
    if (has_uid) {
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(db, "SELECT id FROM phases WHERE uid = ?",
                                    -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to prepare phase lookup";
            return -1;
        }
        sqlite3_bind_text(stmt, 1, uid_item->valuestring, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_NOT_FOUND;
            *err_msg_out = "phase not found by uid";
            return -1;
        }
        phase_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    } else {
        sqlite3_int64 plan_id = 0;
        if (ipman_resolve_plan_scope(req->params, db, &plan_id,
                                     err_code_out, err_msg_out) != 0) return -1;
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(db,
            "SELECT id FROM phases WHERE label = ? AND plan_id = ?",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to prepare phase lookup";
            return -1;
        }
        sqlite3_bind_text(stmt, 1, label_item->valuestring, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, plan_id);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_NOT_FOUND;
            *err_msg_out = "phase not found by label";
            return -1;
        }
        phase_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }

    cJSON *result = cJSON_CreateObject();
    if (result == NULL ||
        cJSON_AddNumberToObject(result, "id", (double)phase_id) == NULL) {
        if (result != NULL) cJSON_Delete(result);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build lookup response";
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

static int update_phase_fields(sqlite3 *db,
                               sqlite3_int64 phase_id,
                               const phase_update_t *update) {
    const char *sql =
        "UPDATE phases SET "
        "title = CASE WHEN ? THEN ? ELSE title END, "
        "summary = CASE WHEN ? THEN ? ELSE summary END, "
        "description = CASE WHEN ? THEN ? ELSE description END, "
        "owner = CASE WHEN ? THEN ? ELSE owner END, "
        "target_start_date = CASE WHEN ? THEN ? ELSE target_start_date END, "
        "target_end_date = CASE WHEN ? THEN ? ELSE target_end_date END, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    bind_update_text(stmt, 1, 2, update->title_set, update->title);
    bind_update_text(stmt, 3, 4, update->summary_set, update->summary);
    bind_update_text(stmt, 5, 6, update->description_set, update->description);
    bind_update_text(stmt, 7, 8, update->owner_set, update->owner);
    bind_update_text(stmt, 9, 10, update->target_start_date_set,
                     update->target_start_date);
    bind_update_text(stmt, 11, 12, update->target_end_date_set,
                     update->target_end_date);
    sqlite3_bind_int64(stmt, 13, phase_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

const ipman_param_desc_t ipman_op_phase_update_params[] = {
    { "id" }, { "title" }, { "summary" }, { "description" },
    { "owner" }, { "target_start_date" }, { "target_end_date" },
    { NULL },
};

int ipman_op_phase_update(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out) {
    sqlite3_int64 phase_id = 0;
    phase_update_t update;
    if (ipman_read_phase_selector(req->params, db, &phase_id,
                         err_code_out, err_msg_out) != 0 ||
        read_phase_update(req->params, &update, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    int load_failed = 0;
    cJSON *old_phase = load_phase_checked(db, phase_id, &load_failed);
    if (old_phase == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = load_failed ? IPMAN_ERR_INTERNAL : IPMAN_ERR_NOT_FOUND;
        *err_msg_out = load_failed ? "failed to load phase" : "phase not found";
        return -1;
    }
    const char *old_status = phase_json_status(old_phase);
    if (old_status != NULL && is_terminal_phase_status(old_status)) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "cannot update a phase in a terminal state";
        return -1;
    }
    if (update_phase_fields(db, phase_id, &update) != 0) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "failed to update phase";
        return -1;
    }
    int rc = finish_phase_change(db, req, old_phase, phase_id, "phase_updated",
                                 "phase updated", "{\"op\":\"phase.update\"}",
                                 NULL, result_out, err_code_out, err_msg_out);
    cJSON_Delete(old_phase);
    if (rc != 0) return -1;
    return commit_result_owned(db, result_out, err_code_out, err_msg_out);
}

static int move_phase_sequence(sqlite3 *db,
                               sqlite3_int64 plan_id,
                               sqlite3_int64 phase_id,
                               sqlite3_int64 old_sequence,
                               sqlite3_int64 new_sequence) {
    sqlite3_int64 max_sequence = phase_max_sequence(db, plan_id);
    if (max_sequence < 0) return -1;
    sqlite3_int64 offset = max_sequence + 1000;

    const char *move_self =
        "UPDATE phases SET sequence_no = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, move_self, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, old_sequence + offset);
    sqlite3_bind_int64(stmt, 2, phase_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    const char *to_temp_up =
        "UPDATE phases SET sequence_no = sequence_no + ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE plan_id = ? AND sequence_no >= ? AND sequence_no < ?;";
    const char *to_temp_down =
        "UPDATE phases SET sequence_no = sequence_no + ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE plan_id = ? AND sequence_no > ? AND sequence_no <= ?;";
    const char *sql = new_sequence < old_sequence ? to_temp_up : to_temp_down;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, offset);
    sqlite3_bind_int64(stmt, 2, plan_id);
    sqlite3_bind_int64(stmt, 3, new_sequence < old_sequence
                                 ? new_sequence
                                 : old_sequence);
    sqlite3_bind_int64(stmt, 4, new_sequence < old_sequence
                                 ? old_sequence
                                 : new_sequence);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    const char *from_temp_up =
        "UPDATE phases SET sequence_no = sequence_no - ? + 1 "
        "WHERE plan_id = ? AND sequence_no >= ? AND sequence_no < ?;";
    const char *from_temp_down =
        "UPDATE phases SET sequence_no = sequence_no - ? - 1 "
        "WHERE plan_id = ? AND sequence_no > ? AND sequence_no <= ?;";
    sql = new_sequence < old_sequence ? from_temp_up : from_temp_down;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, offset);
    sqlite3_bind_int64(stmt, 2, plan_id);
    sqlite3_bind_int64(stmt, 3, new_sequence < old_sequence
                                 ? new_sequence + offset
                                 : old_sequence + offset);
    sqlite3_bind_int64(stmt, 4, new_sequence < old_sequence
                                 ? old_sequence + offset
                                 : new_sequence + offset);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    rc = sqlite3_prepare_v2(db, move_self, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, new_sequence);
    sqlite3_bind_int64(stmt, 2, phase_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

const ipman_param_desc_t ipman_op_phase_move_params[] = {
    { "id" }, { "sequence_no" },
    { NULL },
};

int ipman_op_phase_move(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out) {
    sqlite3_int64 phase_id = 0;
    sqlite3_int64 sequence_no = 0;
    if (ipman_read_phase_selector(req->params, db, &phase_id,
                         err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (read_required_move_sequence(req->params, &sequence_no,
                                    err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    int load_failed = 0;
    cJSON *old_phase = load_phase_checked(db, phase_id, &load_failed);
    if (old_phase == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = load_failed ? IPMAN_ERR_INTERNAL : IPMAN_ERR_NOT_FOUND;
        *err_msg_out = load_failed ? "failed to load phase" : "phase not found";
        return -1;
    }
    const char *old_phase_status = phase_json_status(old_phase);
    if (old_phase_status != NULL && is_terminal_phase_status(old_phase_status)) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "cannot move a phase in a terminal state";
        return -1;
    }
    sqlite3_int64 plan_id = phase_json_plan_id(old_phase);
    sqlite3_int64 count = phase_count(db, plan_id);
    sqlite3_int64 old_sequence = phase_json_sequence(old_phase);
    if (count < 0 || old_sequence < 1) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to inspect phase order";
        return -1;
    }
    if (sequence_no > count) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "sequence_no is outside phase range";
        return -1;
    }
    if (sequence_no == old_sequence) {
        cJSON *phase = load_phase(db, phase_id);
        cJSON *result = phase == NULL ? NULL : result_with_phase(phase);
        if (result == NULL) {
            if (phase != NULL) cJSON_Delete(phase);
            cJSON_Delete(old_phase);
            run_sql(db, "ROLLBACK;");
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build phase response";
            return -1;
        }
        cJSON_Delete(old_phase);
        if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
            return -1;
        }
        *result_out = result;
        return 0;
    }
    if (move_phase_sequence(db, plan_id, phase_id, old_sequence, sequence_no) != 0) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "failed to move phase";
        return -1;
    }
    int rc = finish_phase_change(db, req, old_phase, phase_id, "phase_moved",
                                 "phase moved", "{\"op\":\"phase.move\"}",
                                 NULL, result_out, err_code_out, err_msg_out);
    cJSON_Delete(old_phase);
    if (rc != 0) return -1;
    return commit_result_owned(db, result_out, err_code_out, err_msg_out);
}

static int update_phase_status(sqlite3 *db,
                               sqlite3_int64 phase_id,
                               const char *status,
                               const char *mode) {
    const char *close_sql =
        "UPDATE phases SET status = ?, "
        "closed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    const char *reopen_sql =
        "UPDATE phases SET status = 'open', closed_at = NULL, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    const char *sql = strcmp(mode, "reopen") == 0 ? reopen_sql : close_sql;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    if (strcmp(mode, "close") == 0) {
        sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, phase_id);
    } else {
        sqlite3_bind_int64(stmt, 1, phase_id);
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

static int count_nonterminal_tasks(sqlite3 *db, sqlite3_int64 phase_id) {
    const char *sql =
        "SELECT COUNT(*) FROM tasks "
        "WHERE phase_id = ? AND status NOT IN ('done', 'canceled');";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, phase_id);
    rc = sqlite3_step(stmt);
    int count = -1;
    if (rc == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

static int insert_closure_record(sqlite3 *db,
                                 sqlite3_int64 phase_id,
                                 const char *status,
                                 const phase_close_t *close_data,
                                 const char *actor,
                                 sqlite3_int64 event_id) {
    const char *sql =
        "INSERT INTO closure_records("
        "entity_type, entity_id, closure_status, resolution, outcome_summary, "
        "closing_comment, lessons_learned, open_items_summary, "
        "followup_needed, author, event_id"
        ") VALUES ('phase', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, phase_id);
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

const ipman_param_desc_t ipman_op_phase_close_params[] = {
    { "id" }, { "outcome" },
    { "outcome_summary" }, { "closing_comment" },
    { "lessons_learned" }, { "open_items_summary" },
    { "followup_needed" },
    { NULL },
};

int ipman_op_phase_close(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out) {
    sqlite3_int64 phase_id = 0;
    const char *outcome = "completed";
    phase_close_t close_data;
    if (ipman_read_phase_selector(req->params, db, &phase_id,
                         err_code_out, err_msg_out) != 0 ||
        read_phase_close(req->params, &close_data,
                         err_code_out, err_msg_out) != 0) {
        return -1;
    }
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

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    int load_failed = 0;
    cJSON *old_phase = load_phase_checked(db, phase_id, &load_failed);
    if (old_phase == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = load_failed ? IPMAN_ERR_INTERNAL : IPMAN_ERR_NOT_FOUND;
        *err_msg_out = load_failed ? "failed to load phase" : "phase not found";
        return -1;
    }
    sqlite3_int64 close_plan_id = phase_json_plan_id(old_phase);
    int close_terminal = plan_is_terminal(db, close_plan_id);
    if (close_terminal < 0) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to check plan status";
        return -1;
    }
    if (close_terminal) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "cannot close phase in a terminal plan";
        return -1;
    }
    const char *status = phase_json_status(old_phase);
    int allowed = status != NULL &&
                  (strcmp(status, "open") == 0 ||
                   strcmp(status, "in_progress") == 0 ||
                   strcmp(status, "blocked") == 0);
    if (!allowed) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "invalid phase transition";
        return -1;
    }
    int open_tasks = count_nonterminal_tasks(db, phase_id);
    if (open_tasks < 0) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to inspect phase tasks";
        return -1;
    }
    if (open_tasks > 0) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "phase has non-terminal tasks";
        return -1;
    }
    if (update_phase_status(db, phase_id, outcome, "close") != 0) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "failed to update phase status";
        return -1;
    }
    if (ipman_context_repair_plan_cursor(db, req, close_plan_id,
                                        "{\"op\":\"phase.close\"}",
                                        err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    sqlite3_int64 event_id = 0;
    int rc = finish_phase_change(db, req, old_phase, phase_id, "phase_closed",
                                 "phase closed", "{\"op\":\"phase.close\"}",
                                 &event_id, result_out,
                                 err_code_out, err_msg_out);
    cJSON_Delete(old_phase);
    if (rc != 0) return -1;
    if (insert_closure_record(db, phase_id, outcome, &close_data,
                              req->actor, event_id) != 0) {
        cJSON_Delete(*result_out);
        *result_out = NULL;
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record phase closure";
        return -1;
    }
    return commit_result_owned(db, result_out, err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_phase_reopen_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_phase_reopen(const ipman_request_t *req, sqlite3 *db,
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
    int load_failed = 0;
    cJSON *old_phase = load_phase_checked(db, phase_id, &load_failed);
    if (old_phase == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = load_failed ? IPMAN_ERR_INTERNAL : IPMAN_ERR_NOT_FOUND;
        *err_msg_out = load_failed ? "failed to load phase" : "phase not found";
        return -1;
    }
    const char *status = phase_json_status(old_phase);
    int allowed = status != NULL &&
                  (strcmp(status, "completed") == 0 ||
                   strcmp(status, "canceled") == 0);
    if (!allowed) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "invalid phase transition";
        return -1;
    }
    if (update_phase_status(db, phase_id, "open", "reopen") != 0) {
        cJSON_Delete(old_phase);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "failed to update phase status";
        return -1;
    }
    /*
     * Closure records are immutable audit memory. Reopening a phase changes
     * which closure is active by changing phase status; closure.get only
     * exposes active_closure for terminal entities.
     */
    int rc = finish_phase_change(db, req, old_phase, phase_id,
                                 "phase_reopened", "phase reopened",
                                 "{\"op\":\"phase.reopen\"}", NULL,
                                 result_out, err_code_out, err_msg_out);
    cJSON_Delete(old_phase);
    if (rc != 0) return -1;
    return commit_result_owned(db, result_out, err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_phase_list_tasks_params[] = {
    { "id" }, { "phase_id" }, { "status" }, { "limit" }, { "offset" },
    { NULL },
};

int ipman_op_phase_list_tasks(const ipman_request_t *req, sqlite3 *db,
                             cJSON **result_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out) {
    sqlite3_int64 phase_id = 0;
    const char *status = NULL;
    int limit = PHASE_LIST_DEFAULT_LIMIT;
    int offset = 0;
    if (read_phase_list_tasks_id(req->params, db, &phase_id,
                                 err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "status", &status,
                             err_code_out, err_msg_out) != 0 ||
        read_list_int(req->params, "limit", PHASE_LIST_DEFAULT_LIMIT, &limit,
                      err_code_out, err_msg_out) != 0 ||
        read_list_int(req->params, "offset", 0, &offset,
                      err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (status != NULL && !is_task_status(status)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "status must be a valid task status";
        return -1;
    }
    if (limit < 1 || limit > PHASE_LIST_MAX_LIMIT) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "limit must be between 1 and 500";
        return -1;
    }
    int load_failed = 0;
    cJSON *phase = load_phase_checked(db, phase_id, &load_failed);
    if (phase == NULL) {
        *err_code_out = load_failed ? IPMAN_ERR_INTERNAL : IPMAN_ERR_NOT_FOUND;
        *err_msg_out = load_failed ? "failed to load phase" : "phase not found";
        return -1;
    }
    cJSON_Delete(phase);

    const char *sql =
        "SELECT id, plan_id, phase_id, parent_task_id, title, summary, "
        "description, status, resolution, priority, task_type, origin_type, "
        "assignee, created_at, updated_at, started_at, closed_at, "
        "deferred_until, blocked_reason, reason_code, reason_text, due_date, "
        "target_start_date, estimate, origin_ref_type, origin_ref_id, "
        "origin_task_id, local_seq, "
        "(SELECT code FROM plans WHERE plans.id = tasks.plan_id), "
        "uid, label "
        "FROM tasks "
        "WHERE phase_id = ? AND (? IS NULL OR status = ?) "
        "ORDER BY id ASC LIMIT ? OFFSET ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query phase tasks";
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, phase_id);
    bind_optional_text(stmt, 2, status);
    bind_optional_text(stmt, 3, status);
    sqlite3_bind_int(stmt, 4, limit);
    sqlite3_bind_int(stmt, 5, offset);

    cJSON *result = cJSON_CreateObject();
    cJSON *tasks = cJSON_CreateArray();
    if (result == NULL || tasks == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (tasks != NULL) cJSON_Delete(tasks);
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build phase task list";
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *task = ipman_task_from_row(stmt);
        if (task == NULL) {
            cJSON_Delete(result);
            cJSON_Delete(tasks);
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build phase task list";
            return -1;
        }
        cJSON_AddItemToArray(tasks, task);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(result);
        cJSON_Delete(tasks);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query phase tasks";
        return -1;
    }
    cJSON_AddItemToObject(result, "tasks", tasks);
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_phase_list_params[] = {
    { "plan_id" }, { "status" }, { "completed_or_blocked" },
    { "limit" }, { "offset" },
    { NULL },
};

int ipman_op_phase_list(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out) {
    int has_plan_id = 0;
    sqlite3_int64 plan_id = 0;
    const char *status = NULL;
    int has_completed_or_blocked = 0;
    int completed_or_blocked = 0;
    int limit = PHASE_LIST_DEFAULT_LIMIT;
    int offset = 0;

    if (read_optional_id(req->params, "plan_id", &has_plan_id, &plan_id,
                         err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "status", &status,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_bool(req->params, "completed_or_blocked",
                           &has_completed_or_blocked, &completed_or_blocked,
                           err_code_out, err_msg_out) != 0 ||
        read_list_int(req->params, "limit", PHASE_LIST_DEFAULT_LIMIT, &limit,
                      err_code_out, err_msg_out) != 0 ||
        read_list_int(req->params, "offset", 0, &offset,
                      err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (status != NULL && !is_phase_status(status)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "status must be a valid phase status";
        return -1;
    }
    if (status != NULL && has_completed_or_blocked && completed_or_blocked) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "status and completed_or_blocked cannot be combined";
        return -1;
    }
    if (limit < 1 || limit > PHASE_LIST_MAX_LIMIT) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "limit must be between 1 and 500";
        return -1;
    }

    /* Count total matching rows */
    int rc = 0;
    const char *count_sql =
        "SELECT COUNT(*) FROM phases p "
        "WHERE (? = 0 OR p.plan_id = ?) "
        "AND (? IS NULL OR p.status = ?) "
        "AND (? = 0 OR p.status IN ('completed', 'blocked'));";
    sqlite3_stmt *count_stmt = NULL;
    rc = sqlite3_prepare_v2(db, count_sql, -1, &count_stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to count phases";
        return -1;
    }
    sqlite3_bind_int(count_stmt, 1, has_plan_id);
    sqlite3_bind_int64(count_stmt, 2, plan_id);
    bind_optional_text(count_stmt, 3, status);
    bind_optional_text(count_stmt, 4, status);
    sqlite3_bind_int(count_stmt, 5, has_completed_or_blocked && completed_or_blocked);
    rc = sqlite3_step(count_stmt);
    int total_count = (rc == SQLITE_ROW) ? sqlite3_column_int(count_stmt, 0) : 0;
    sqlite3_finalize(count_stmt);

    const char *sql =
        "SELECT p.id, p.plan_id, p.title, p.summary, p.description, p.status, "
        "p.sequence_no, p.created_at, p.updated_at, p.opened_at, p.closed_at, "
        "p.owner, p.target_start_date, p.target_end_date, p.local_seq, "
        "p.uid, p.label "
        "FROM phases p "
        "WHERE (? = 0 OR p.plan_id = ?) "
        "AND (? IS NULL OR p.status = ?) "
        "AND (? = 0 OR p.status IN ('completed', 'blocked')) "
        "ORDER BY p.plan_id ASC, p.sequence_no ASC LIMIT ? OFFSET ?;";
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query phases";
        return -1;
    }
    sqlite3_bind_int(stmt, 1, has_plan_id);
    sqlite3_bind_int64(stmt, 2, plan_id);
    bind_optional_text(stmt, 3, status);
    bind_optional_text(stmt, 4, status);
    sqlite3_bind_int(stmt, 5, has_completed_or_blocked && completed_or_blocked);
    sqlite3_bind_int(stmt, 6, limit + 1);
    sqlite3_bind_int(stmt, 7, offset);

    cJSON *result = cJSON_CreateObject();
    cJSON *phases = cJSON_CreateArray();
    if (result == NULL || phases == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (phases != NULL) cJSON_Delete(phases);
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build phase list";
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *phase = ipman_phase_from_row(stmt);
        if (phase == NULL) {
            cJSON_Delete(result);
            cJSON_Delete(phases);
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build phase list";
            return -1;
        }
        cJSON_AddItemToArray(phases, phase);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(result);
        cJSON_Delete(phases);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query phases";
        return -1;
    }
    int has_more = 0;
    int row_count = cJSON_GetArraySize(phases);
    if (row_count > limit) {
        has_more = 1;
        cJSON *last = cJSON_DetachItemFromArray(phases, row_count - 1);
        cJSON_Delete(last);
    }
    cJSON_AddItemToObject(result, "phases", phases);
    cJSON_AddNumberToObject(result, "limit", limit);
    cJSON_AddNumberToObject(result, "offset", offset);
    cJSON_AddBoolToObject(result, "has_more", has_more);
    cJSON_AddNumberToObject(result, "total_count", total_count);
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_phase_progress_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_phase_progress(const ipman_request_t *req, sqlite3 *db,
                           cJSON **result_out,
                           ipman_error_code_t *err_code_out,
                           const char **err_msg_out) {
    sqlite3_int64 phase_id = 0;
    if (ipman_read_phase_selector(req->params, db, &phase_id,
                         err_code_out, err_msg_out) != 0) {
        return -1;
    }
    int load_failed = 0;
    cJSON *phase = load_phase_checked(db, phase_id, &load_failed);
    if (phase == NULL) {
        *err_code_out = load_failed ? IPMAN_ERR_INTERNAL : IPMAN_ERR_NOT_FOUND;
        *err_msg_out = load_failed ? "failed to load phase" : "phase not found";
        return -1;
    }
    sqlite3_int64 plan_id = phase_json_plan_id(phase);
    cJSON_Delete(phase);

    const char *sql =
        "SELECT status, COUNT(*) FROM tasks WHERE phase_id = ? GROUP BY status;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query phase progress";
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, phase_id);

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
        *err_msg_out = "failed to query phase progress";
        return -1;
    }

    sqlite3_int64 total = todo + in_progress + blocked + deferred + done;
    double percentage = total == 0 ? 0.0 : ((double)done * 100.0) / (double)total;
    cJSON *result = cJSON_CreateObject();
    cJSON *counts = cJSON_CreateObject();
    if (result == NULL || counts == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (counts != NULL) cJSON_Delete(counts);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build phase progress";
        return -1;
    }
    cJSON_AddNumberToObject(result, "phase_id", (double)phase_id);
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

const ipman_param_desc_t ipman_op_phase_history_params[] = {
    { "id" }, { "since" }, { "limit" }, { "offset" },
    { NULL },
};

int ipman_op_phase_history(const ipman_request_t *req, sqlite3 *db,
                          cJSON **result_out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out) {
    sqlite3_int64 phase_id = 0;
    sqlite3_int64 since = 0;
    int limit = PHASE_LIST_DEFAULT_LIMIT;
    int offset = 0;
    if (ipman_read_phase_selector(req->params, db, &phase_id,
                         err_code_out, err_msg_out) != 0 ||
        read_optional_since(req->params, &since,
                            err_code_out, err_msg_out) != 0 ||
        read_list_int(req->params, "limit", PHASE_LIST_DEFAULT_LIMIT, &limit,
                      err_code_out, err_msg_out) != 0 ||
        read_list_int(req->params, "offset", 0, &offset,
                      err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (limit < 1 || limit > PHASE_LIST_MAX_LIMIT) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "limit must be between 1 and 500";
        return -1;
    }
    int load_failed = 0;
    cJSON *phase = load_phase_checked(db, phase_id, &load_failed);
    if (phase == NULL) {
        *err_code_out = load_failed ? IPMAN_ERR_INTERNAL : IPMAN_ERR_NOT_FOUND;
        *err_msg_out = load_failed ? "failed to load phase" : "phase not found";
        return -1;
    }
    cJSON_Delete(phase);

    const char *sql =
        "SELECT id, entity_type, entity_id, event_type, actor, event_at, "
        "summary, details, old_value, new_value, related_entity_type, "
        "related_entity_id, request_id "
        "FROM events "
        "WHERE entity_type = 'phase' AND entity_id = ? AND (? = 0 OR id > ?) "
        "ORDER BY id DESC LIMIT ? OFFSET ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query phase history";
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, phase_id);
    sqlite3_bind_int64(stmt, 2, since);
    sqlite3_bind_int64(stmt, 3, since);
    sqlite3_bind_int(stmt, 4, limit);
    sqlite3_bind_int(stmt, 5, offset);

    cJSON *result = cJSON_CreateObject();
    cJSON *events = cJSON_CreateArray();
    if (result == NULL || events == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (events != NULL) cJSON_Delete(events);
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build phase history";
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *event = ipman_event_from_row(stmt);
        if (event == NULL) {
            cJSON_Delete(result);
            cJSON_Delete(events);
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build phase history";
            return -1;
        }
        cJSON_AddItemToArray(events, event);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(result);
        cJSON_Delete(events);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query phase history";
        return -1;
    }
    cJSON_AddItemToObject(result, "events", events);
    *result_out = result;
    return 0;
}
