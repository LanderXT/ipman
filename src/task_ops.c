/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "task_ops.h"
#include "context_ops.h"
#include "db.h"
#include "event_ops.h"
#include "json_helpers.h"
#include "log.h"
#include "plan_ops.h"
#include "validation.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define TASK_LIST_DEFAULT_LIMIT 100
#define TASK_LIST_MAX_LIMIT 500

typedef struct {
    int title_set;
    const char *title;
    int summary_set;
    const char *summary;
    int description_set;
    const char *description;
    int due_date_set;
    const char *due_date;
    int target_start_date_set;
    const char *target_start_date;
    int estimate_set;
    const char *estimate;
    int blocked_reason_set;
    const char *blocked_reason;
    int reason_code_set;
    const char *reason_code;
    int reason_text_set;
    const char *reason_text;
} task_update_t;

typedef struct {
    const char *outcome_summary;
    const char *closing_comment;
    const char *lessons_learned;
    const char *open_items_summary;
    int followup_needed;
    /*
     * Git auto-capture fields (Phase 11). All optional; populated by the CLI
     * when --close runs inside a git repo, or by any client that chooses to
     * send them. The op handler does not invoke git itself; these fields are
     * borrowed pointers into the cJSON request tree.
     *
     *  - commit_sha:      string (or NULL when absent).
     *  - dirty/dirty_set: tri-state — dirty_set=0 means absent (column NULL);
     *                    dirty_set=1 with dirty=0 means clean repo;
     *                    dirty_set=1 with dirty=1 means dirty repo.
     *  - files_changed:   cJSON array of strings (or NULL when absent);
     *                    serialized at INSERT time.
     */
    const char *commit_sha;
    int dirty;
    int dirty_set;
    const cJSON *files_changed;
} task_close_t;

typedef struct {
    sqlite3_int64 id;
    sqlite3_int64 from_task_id;
    sqlite3_int64 to_task_id;
    const char *relation_type;
    const char *created_at;
    const char *created_by;
    const char *notes;
} task_relation_row_t;

typedef struct {
    const char *from_status;
    const char *to_status;
    const char *operation;
    int generic_allowed;
} task_transition_rule_t;

static const task_transition_rule_t k_task_transition_rules[] = {
    { "todo",        "in_progress", "task.transition", 1 },
    { "todo",        "blocked",     "task.transition", 1 },
    { "in_progress", "todo",        "task.transition", 1 },
    { "in_progress", "blocked",     "task.transition", 1 },
    { "blocked",     "todo",        "task.transition", 1 },
    { "blocked",     "in_progress", "task.transition", 1 },
    { "deferred",    "todo",        "task.transition", 1 },
    { "deferred",    "in_progress", "task.transition", 1 },

    { "todo",        "deferred",    "task.defer", 0 },
    { "in_progress", "deferred",    "task.defer", 0 },
    { "blocked",     "deferred",    "task.defer", 0 },
    { "deferred",    "deferred",    "task.defer", 0 },

    { "todo",        "canceled",    "task.cancel", 0 },
    { "in_progress", "canceled",    "task.cancel", 0 },
    { "blocked",     "canceled",    "task.cancel", 0 },
    { "deferred",    "canceled",    "task.cancel", 0 },

    { "todo",        "canceled",    "task.mark_duplicate", 0 },
    { "in_progress", "canceled",    "task.mark_duplicate", 0 },
    { "blocked",     "canceled",    "task.mark_duplicate", 0 },
    { "deferred",    "canceled",    "task.mark_duplicate", 0 },

    { "todo",        "canceled",    "task.replace", 0 },
    { "in_progress", "canceled",    "task.replace", 0 },
    { "blocked",     "canceled",    "task.replace", 0 },
    { "deferred",    "canceled",    "task.replace", 0 },

    { "todo",        "done",        "task.close", 0 },
    { "in_progress", "done",        "task.close", 0 },
    { "blocked",     "done",        "task.close", 0 },
    { "deferred",    "done",        "task.close", 0 },

    { "done",        "todo",        "task.reopen", 0 },
    { "canceled",    "todo",        "task.reopen", 0 },
};

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

static int is_task_resolution(const char *value) {
    static const char *allowed[] = {
        "completed", "canceled", "not_planned",
        "replaced", "duplicate", "discarded",
    };
    size_t count = sizeof allowed / sizeof allowed[0];
    for (size_t pos = 0; pos < count; ++pos) {
        if (strcmp(value, allowed[pos]) == 0) return 1;
    }
    return 0;
}

static int is_cancel_resolution(const char *value) {
    return strcmp(value, "canceled") == 0 ||
           strcmp(value, "not_planned") == 0 ||
           strcmp(value, "discarded") == 0 ||
           strcmp(value, "duplicate") == 0;
}

static ipman_error_code_t create_sqlite_error_code(sqlite3 *db, int rc) {
    int extended = sqlite3_extended_errcode(db);
    if (rc == SQLITE_CONSTRAINT ||
        (extended & 0xff) == SQLITE_CONSTRAINT) {
        return IPMAN_ERR_CONFLICT;
    }
    return IPMAN_ERR_INTERNAL;
}

static int is_dependency_relation_type(const char *value) {
    return strcmp(value, "blocks") == 0 ||
           strcmp(value, "blocked_by") == 0 ||
           strcmp(value, "related") == 0 ||
           strcmp(value, "duplicates") == 0;
}

static const char *reverse_relation_type(const char *value) {
    if (strcmp(value, "blocks") == 0) return "blocked_by";
    if (strcmp(value, "blocked_by") == 0) return "blocks";
    if (strcmp(value, "related") == 0) return "related";
    return NULL;
}

static const task_transition_rule_t *find_transition_rule(
    const char *from_status,
    const char *to_status,
    const char *operation) {
    size_t count = sizeof k_task_transition_rules / sizeof k_task_transition_rules[0];
    for (size_t pos = 0; pos < count; ++pos) {
        const task_transition_rule_t *rule = &k_task_transition_rules[pos];
        if (strcmp(rule->from_status, from_status) == 0 &&
            strcmp(rule->to_status, to_status) == 0 &&
            (operation == NULL || strcmp(rule->operation, operation) == 0)) {
            return rule;
        }
    }
    return NULL;
}

static int generic_transition_allowed(const char *from_status,
                                      const char *to_status) {
    const task_transition_rule_t *rule =
        find_transition_rule(from_status, to_status, NULL);
    return rule != NULL && rule->generic_allowed;
}

static int transition_allowed_for_op(const char *from_status,
                                     const char *to_status,
                                     const char *operation) {
    return find_transition_rule(from_status, to_status, operation) != NULL;
}

static const char *task_transition_error_message(const char *from_status,
                                                 const char *to_status) {
    static char message[128];
    if (from_status == NULL || to_status == NULL) {
        return "task status is invalid";
    }
    if (strcmp(from_status, to_status) == 0) {
        snprintf(message, sizeof message,
                 "Task is already in status %s", to_status);
        return message;
    }
    if (strcmp(from_status, "done") == 0 ||
        strcmp(from_status, "canceled") == 0) {
        snprintf(message, sizeof message,
                 "Task has status %s and cannot be transitioned",
                 from_status);
        return message;
    }
    snprintf(message, sizeof message,
             "Transition from %s to %s is not allowed",
             from_status, to_status);
    return message;
}

/* Attach a structured error.details payload describing why a task transition
 * was rejected. `allowed_next[]` enumerates every transition reachable from
 * the current status (including semantic ops like `task.defer`); clients that
 * used `task.transition` for a transition that requires a specific op learn
 * which op to call from `required_operation`. */
static void attach_task_transition_error_details(const char *from_status,
                                                 const char *attempted_status) {
    cJSON *details = cJSON_CreateObject();
    if (details == NULL) return;

    if (cJSON_AddStringToObject(details, "kind", "invalid_state_transition") == NULL ||
        cJSON_AddStringToObject(details, "entity_type", "task") == NULL) {
        cJSON_Delete(details);
        return;
    }
    if (from_status != NULL) {
        cJSON_AddStringToObject(details, "current_status", from_status);
    } else {
        cJSON_AddNullToObject(details, "current_status");
    }
    if (attempted_status != NULL) {
        cJSON_AddStringToObject(details, "attempted_status", attempted_status);
    } else {
        cJSON_AddNullToObject(details, "attempted_status");
    }

    cJSON *allowed = cJSON_CreateArray();
    if (allowed == NULL) {
        cJSON_Delete(details);
        return;
    }
    cJSON_AddItemToObject(details, "allowed_next", allowed);
    if (from_status != NULL) {
        size_t count =
            sizeof k_task_transition_rules / sizeof k_task_transition_rules[0];
        for (size_t pos = 0; pos < count; ++pos) {
            const task_transition_rule_t *rule = &k_task_transition_rules[pos];
            if (strcmp(rule->from_status, from_status) != 0) continue;
            cJSON *entry = cJSON_CreateObject();
            if (entry == NULL) continue;
            cJSON_AddStringToObject(entry, "status", rule->to_status);
            cJSON_AddStringToObject(entry, "operation", rule->operation);
            cJSON_AddItemToArray(allowed, entry);
        }
    }

    const char *required_op = NULL;
    if (from_status != NULL && attempted_status != NULL) {
        const task_transition_rule_t *rule =
            find_transition_rule(from_status, attempted_status, NULL);
        if (rule != NULL) required_op = rule->operation;
    }
    if (required_op != NULL) {
        cJSON_AddStringToObject(details, "required_operation", required_op);
    } else {
        cJSON_AddNullToObject(details, "required_operation");
    }

    ipman_error_attach_details(details);
}

static void attach_phase_terminal_error_details(sqlite3_int64 phase_id,
                                                const char *phase_status) {
    cJSON *details = cJSON_CreateObject();
    if (details == NULL) return;
    if (cJSON_AddStringToObject(details, "kind", "phase_terminal") == NULL ||
        cJSON_AddStringToObject(details, "entity_type", "phase") == NULL) {
        cJSON_Delete(details);
        return;
    }
    cJSON_AddNumberToObject(details, "phase_id", (double)phase_id);
    if (phase_status != NULL) {
        cJSON_AddStringToObject(details, "phase_status", phase_status);
    } else {
        cJSON_AddNullToObject(details, "phase_status");
    }
    cJSON_AddStringToObject(details, "required_operation", "phase.reopen");
    ipman_error_attach_details(details);
}

static int is_priority(const char *value) {
    static const char *allowed[] = {
        "low", "medium", "high", "critical",
    };
    size_t count = sizeof allowed / sizeof allowed[0];
    for (size_t pos = 0; pos < count; ++pos) {
        if (strcmp(value, allowed[pos]) == 0) return 1;
    }
    return 0;
}

static int is_task_type(const char *value) {
    static const char *allowed[] = {
        "task", "research", "bug", "decision", "review", "documentation",
    };
    size_t count = sizeof allowed / sizeof allowed[0];
    for (size_t pos = 0; pos < count; ++pos) {
        if (strcmp(value, allowed[pos]) == 0) return 1;
    }
    return 0;
}

static int is_origin_type(const char *value) {
    static const char *allowed[] = {
        "planned", "addendum", "discovered",
        "replacement", "carryover", "external_request",
    };
    size_t count = sizeof allowed / sizeof allowed[0];
    for (size_t pos = 0; pos < count; ++pos) {
        if (strcmp(value, allowed[pos]) == 0) return 1;
    }
    return 0;
}

static int is_external_ref_type(const char *value) {
    static const char *allowed[] = {
        "github_issue", "gitlab_issue", "commit",
        "pr", "adr", "url", "file",
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

int ipman_read_task_selector(cJSON *params, sqlite3 *db,
                            sqlite3_int64 *task_id_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out) {
    (void)db;
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(params, "id");
    if (!id_item || !cJSON_IsNumber(id_item)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "id required (use task.lookup to resolve uid/label to id)";
        return -1;
    }
    if (id_item->valuedouble < 1.0) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "id must be a positive integer";
        return -1;
    }
    *task_id_out = (sqlite3_int64)id_item->valuedouble;
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

static int read_nullable_id_update(cJSON *params, const char *field,
                                   int *present_out,
                                   sqlite3_int64 *out,
                                   ipman_error_code_t *err_code_out,
                                   const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, field);
    *present_out = 0;
    *out = 0;
    if (item == NULL) return 0;
    *present_out = 1;
    if (cJSON_IsNull(item)) return 0;
    if (!cJSON_IsNumber(item) || item->valuedouble < 1.0) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "id must be a positive integer or null";
        return -1;
    }
    *out = (sqlite3_int64)item->valuedouble;
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
    if (ipman_validate_text_field(field, item->valuestring,
                                 err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if ((strcmp(field, "due_date") == 0 ||
         strcmp(field, "target_start_date") == 0 ||
         strcmp(field, "deferred_until") == 0) &&
        ipman_validate_date_field(item->valuestring,
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
    *out = item->valuestring;
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
    if ((strcmp(field, "due_date") == 0 ||
         strcmp(field, "target_start_date") == 0) &&
        ipman_validate_date_field(item->valuestring,
                                 err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *value_out = item->valuestring;
    return 0;
}

static int read_optional_task_status(cJSON *params, const char **out,
                                     ipman_error_code_t *err_code_out,
                                     const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "status");
    if (item == NULL || cJSON_IsNull(item)) {
        *out = "todo";
        return 0;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        !is_task_status(item->valuestring)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "status must be a valid task status";
        return -1;
    }
    *out = item->valuestring;
    return 0;
}

static int read_required_task_status(cJSON *params, const char **out,
                                     ipman_error_code_t *err_code_out,
                                     const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "status");
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        !is_task_status(item->valuestring)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "status must be a valid task status";
        return -1;
    }
    *out = item->valuestring;
    return 0;
}

static int read_required_task_resolution(cJSON *params, const char **out,
                                         ipman_error_code_t *err_code_out,
                                         const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "resolution");
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        !is_task_resolution(item->valuestring)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "resolution must be a valid task resolution";
        return -1;
    }
    *out = item->valuestring;
    return 0;
}

static int read_required_relation_type(cJSON *params, const char **out,
                                       ipman_error_code_t *err_code_out,
                                       const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "relation_type");
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        !is_dependency_relation_type(item->valuestring)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "relation_type must be blocks, blocked_by, related, or duplicates";
        return -1;
    }
    *out = item->valuestring;
    return 0;
}

static int read_optional_priority(cJSON *params, const char **out,
                                  ipman_error_code_t *err_code_out,
                                  const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "priority");
    if (item == NULL || cJSON_IsNull(item)) {
        *out = "medium";
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

static int read_optional_task_type(cJSON *params, const char **out,
                                   ipman_error_code_t *err_code_out,
                                   const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "task_type");
    if (item == NULL || cJSON_IsNull(item)) {
        *out = "task";
        return 0;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        !is_task_type(item->valuestring)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "task_type must be a valid task type";
        return -1;
    }
    *out = item->valuestring;
    return 0;
}

static int read_optional_origin_type(cJSON *params, const char **out,
                                     ipman_error_code_t *err_code_out,
                                     const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, "origin_type");
    if (item == NULL || cJSON_IsNull(item)) {
        *out = "planned";
        return 0;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        !is_origin_type(item->valuestring)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "origin_type must be a valid origin type";
        return -1;
    }
    *out = item->valuestring;
    return 0;
}

static int read_task_update(cJSON *params, task_update_t *update,
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
        read_update_string(params, "due_date", 1,
                           &update->due_date_set, &update->due_date,
                           err_code_out, err_msg_out) != 0 ||
        read_update_string(params, "target_start_date", 1,
                           &update->target_start_date_set,
                           &update->target_start_date,
                           err_code_out, err_msg_out) != 0 ||
        read_update_string(params, "estimate", 1,
                           &update->estimate_set, &update->estimate,
                           err_code_out, err_msg_out) != 0 ||
        read_update_string(params, "blocked_reason", 1,
                           &update->blocked_reason_set,
                           &update->blocked_reason,
                           err_code_out, err_msg_out) != 0 ||
        read_update_string(params, "reason_code", 1,
                           &update->reason_code_set, &update->reason_code,
                           err_code_out, err_msg_out) != 0 ||
        read_update_string(params, "reason_text", 1,
                           &update->reason_text_set, &update->reason_text,
                           err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (!update->title_set &&
        !update->summary_set &&
        !update->description_set &&
        !update->due_date_set &&
        !update->target_start_date_set &&
        !update->estimate_set &&
        !update->blocked_reason_set &&
        !update->reason_code_set &&
        !update->reason_text_set) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "at least one updatable field is required";
        return -1;
    }
    return 0;
}

static int read_task_close(cJSON *params, task_close_t *close_data,
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
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(params, "commit_sha",
                             &close_data->commit_sha,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }

    cJSON *followup = cJSON_GetObjectItemCaseSensitive(params, "followup_needed");
    if (followup != NULL && !cJSON_IsNull(followup)) {
        if (!cJSON_IsBool(followup)) {
            *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
            *err_msg_out = "followup_needed must be a boolean";
            return -1;
        }
        close_data->followup_needed = cJSON_IsTrue(followup) ? 1 : 0;
    }

    cJSON *dirty = cJSON_GetObjectItemCaseSensitive(params, "dirty");
    if (dirty != NULL && !cJSON_IsNull(dirty)) {
        if (!cJSON_IsBool(dirty)) {
            *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
            *err_msg_out = "dirty must be a boolean";
            return -1;
        }
        close_data->dirty = cJSON_IsTrue(dirty) ? 1 : 0;
        close_data->dirty_set = 1;
    }

    cJSON *files = cJSON_GetObjectItemCaseSensitive(params, "files_changed");
    if (files != NULL && !cJSON_IsNull(files)) {
        if (!cJSON_IsArray(files)) {
            *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
            *err_msg_out = "files_changed must be an array of strings or null";
            return -1;
        }
        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, files) {
            if (!cJSON_IsString(entry) || entry->valuestring == NULL) {
                *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
                *err_msg_out = "files_changed must be an array of strings or null";
                return -1;
            }
        }
        close_data->files_changed = files;
    }
    return 0;
}

static int read_task_terminal_closure(cJSON *params,
                                      const char *comment_fallback,
                                      task_close_t *close_data,
                                      const char *missing_message,
                                      ipman_error_code_t *err_code_out,
                                      const char **err_msg_out) {
    const char *closing_comment = NULL;
    memset(close_data, 0, sizeof *close_data);
    if (read_optional_string(params, "closing_comment", &closing_comment,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(params, "outcome_summary",
                             &close_data->outcome_summary,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(params, "lessons_learned",
                             &close_data->lessons_learned,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(params, "open_items_summary",
                             &close_data->open_items_summary,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (closing_comment != NULL && is_blank(closing_comment)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "closing_comment must be non-empty when provided";
        return -1;
    }
    if (closing_comment == NULL) closing_comment = comment_fallback;
    if (is_blank(closing_comment)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = missing_message;
        return -1;
    }
    if (close_data->outcome_summary != NULL &&
        is_blank(close_data->outcome_summary)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "outcome_summary must be non-empty when provided";
        return -1;
    }
    close_data->closing_comment = closing_comment;
    if (close_data->outcome_summary == NULL) {
        close_data->outcome_summary = closing_comment;
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

static int entity_exists(sqlite3 *db, const char *table, sqlite3_int64 id) {
    const char *sql = NULL;
    if (strcmp(table, "plans") == 0) {
        sql = "SELECT 1 FROM plans WHERE id = ?;";
    } else if (strcmp(table, "tasks") == 0) {
        sql = "SELECT 1 FROM tasks WHERE id = ?;";
    } else {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_ROW) return 1;
    if (rc == SQLITE_DONE) return 0;
    return -1;
}

static int phase_plan_id(sqlite3 *db, sqlite3_int64 phase_id,
                         sqlite3_int64 *plan_id_out) {
    const char *sql = "SELECT plan_id FROM phases WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, phase_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *plan_id_out = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return 1;
    }
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) return 0;
    return -1;
}

static int load_phase_status(sqlite3 *db, sqlite3_int64 phase_id,
                             char *status_out, size_t status_out_size) {
    const char *sql = "SELECT status FROM phases WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, phase_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char *status = sqlite3_column_text(stmt, 0);
        if (status_out_size > 0) {
            if (status == NULL) {
                status_out[0] = '\0';
            } else {
                snprintf(status_out, status_out_size, "%s",
                         (const char *)status);
            }
        }
        sqlite3_finalize(stmt);
        return 1;
    }
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) return 0;
    return -1;
}

static int task_plan_id(sqlite3 *db, sqlite3_int64 task_id,
                        sqlite3_int64 *plan_id_out) {
    const char *sql = "SELECT plan_id FROM tasks WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, task_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *plan_id_out = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return 1;
    }
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) return 0;
    return -1;
}

cJSON *ipman_task_from_row(sqlite3_stmt *stmt) {
    cJSON *task = cJSON_CreateObject();
    if (task == NULL) return NULL;
    sqlite3_int64 plan_id   = sqlite3_column_int64(stmt, 1);
    sqlite3_int64 local_seq = sqlite3_column_int64(stmt, 27);

    cJSON_AddNumberToObject(task, "id", (double)sqlite3_column_int64(stmt, 0));
    ipman_json_add_text_or_null(task, "label", sqlite3_column_text(stmt, 29));
    cJSON_AddNumberToObject(task, "plan_id", (double)plan_id);
    ipman_json_add_int64_or_null(task, "phase_id", stmt, 2);
    ipman_json_add_int64_or_null(task, "parent_task_id", stmt, 3);
    ipman_json_add_text_or_null(task, "title", sqlite3_column_text(stmt, 4));
    ipman_json_add_text_or_null(task, "summary", sqlite3_column_text(stmt, 5));
    ipman_json_add_text_or_null(task, "description", sqlite3_column_text(stmt, 6));
    ipman_json_add_text_or_null(task, "status", sqlite3_column_text(stmt, 7));
    ipman_json_add_text_or_null(task, "resolution", sqlite3_column_text(stmt, 8));
    ipman_json_add_text_or_null(task, "priority", sqlite3_column_text(stmt, 9));
    ipman_json_add_text_or_null(task, "task_type", sqlite3_column_text(stmt, 10));
    ipman_json_add_text_or_null(task, "origin_type", sqlite3_column_text(stmt, 11));
    ipman_json_add_text_or_null(task, "assignee", sqlite3_column_text(stmt, 12));
    ipman_json_add_text_or_null(task, "created_at", sqlite3_column_text(stmt, 13));
    ipman_json_add_text_or_null(task, "updated_at", sqlite3_column_text(stmt, 14));
    ipman_json_add_text_or_null(task, "started_at", sqlite3_column_text(stmt, 15));
    ipman_json_add_text_or_null(task, "closed_at", sqlite3_column_text(stmt, 16));
    ipman_json_add_text_or_null(task, "deferred_until", sqlite3_column_text(stmt, 17));
    ipman_json_add_text_or_null(task, "blocked_reason", sqlite3_column_text(stmt, 18));
    ipman_json_add_text_or_null(task, "reason_code", sqlite3_column_text(stmt, 19));
    ipman_json_add_text_or_null(task, "reason_text", sqlite3_column_text(stmt, 20));
    ipman_json_add_text_or_null(task, "due_date", sqlite3_column_text(stmt, 21));
    ipman_json_add_text_or_null(task, "target_start_date", sqlite3_column_text(stmt, 22));
    ipman_json_add_text_or_null(task, "estimate", sqlite3_column_text(stmt, 23));
    ipman_json_add_text_or_null(task, "origin_ref_type", sqlite3_column_text(stmt, 24));
    ipman_json_add_text_or_null(task, "origin_ref_id", sqlite3_column_text(stmt, 25));
    ipman_json_add_int64_or_null(task, "origin_task_id", stmt, 26);
    cJSON_AddNumberToObject(task, "local_seq", (double)local_seq);
    return task;
}

static cJSON *load_task_checked(sqlite3 *db,
                                sqlite3_int64 task_id,
                                int *load_failed_out) {
    if (load_failed_out != NULL) *load_failed_out = 0;
    const char *sql =
        "SELECT id, plan_id, phase_id, parent_task_id, title, summary, "
        "description, status, resolution, priority, task_type, origin_type, "
        "assignee, created_at, updated_at, started_at, closed_at, "
        "deferred_until, blocked_reason, reason_code, reason_text, due_date, "
        "target_start_date, estimate, origin_ref_type, origin_ref_id, "
        "origin_task_id, local_seq, "
        "uid, label "
        "FROM tasks WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (load_failed_out != NULL) *load_failed_out = 1;
        return NULL;
    }
    sqlite3_bind_int64(stmt, 1, task_id);
    rc = sqlite3_step(stmt);
    cJSON *task = NULL;
    if (rc == SQLITE_ROW) {
        task = ipman_task_from_row(stmt);
        if (task == NULL && load_failed_out != NULL) *load_failed_out = 1;
    } else if (rc != SQLITE_DONE && load_failed_out != NULL) {
        *load_failed_out = 1;
    }
    sqlite3_finalize(stmt);
    return task;
}

static cJSON *load_task(sqlite3 *db, sqlite3_int64 task_id) {
    return load_task_checked(db, task_id, NULL);
}

static int load_task_or_error(sqlite3 *db,
                              sqlite3_int64 task_id,
                              cJSON **task_out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out) {
    int load_failed = 0;
    *task_out = load_task_checked(db, task_id, &load_failed);
    if (*task_out != NULL) return 0;
    *err_code_out = load_failed ? IPMAN_ERR_INTERNAL : IPMAN_ERR_NOT_FOUND;
    *err_msg_out = load_failed ? "failed to load task" : "task not found";
    return -1;
}

static cJSON *relation_json_from_row(const task_relation_row_t *row) {
    cJSON *relation = cJSON_CreateObject();
    if (relation == NULL) return NULL;
    cJSON_AddNumberToObject(relation, "id", (double)row->id);
    cJSON_AddNumberToObject(relation, "from_task_id",
                            (double)row->from_task_id);
    cJSON_AddNumberToObject(relation, "to_task_id",
                            (double)row->to_task_id);
    cJSON_AddStringToObject(relation, "relation_type", row->relation_type);
    cJSON_AddStringToObject(relation, "created_at", row->created_at);
    cJSON_AddStringToObject(relation, "created_by", row->created_by);
    if (row->notes != NULL) {
        cJSON_AddStringToObject(relation, "notes", row->notes);
    }
    return relation;
}

static cJSON *relation_from_stmt(sqlite3_stmt *stmt) {
    task_relation_row_t row;
    row.id = sqlite3_column_int64(stmt, 0);
    row.from_task_id = sqlite3_column_int64(stmt, 1);
    row.to_task_id = sqlite3_column_int64(stmt, 2);
    row.relation_type = (const char *)sqlite3_column_text(stmt, 3);
    row.created_at = (const char *)sqlite3_column_text(stmt, 4);
    row.created_by = (const char *)sqlite3_column_text(stmt, 5);
    row.notes = (const char *)sqlite3_column_text(stmt, 6);
    return relation_json_from_row(&row);
}

static cJSON *load_task_relation(sqlite3 *db, sqlite3_int64 relation_id) {
    const char *sql =
        "SELECT id, from_task_id, to_task_id, relation_type, created_at, "
        "created_by, notes FROM task_relations WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, relation_id);
    rc = sqlite3_step(stmt);
    cJSON *relation = NULL;
    if (rc == SQLITE_ROW) relation = relation_from_stmt(stmt);
    sqlite3_finalize(stmt);
    return relation;
}

static cJSON *load_task_relation_by_key(sqlite3 *db,
                                        sqlite3_int64 from_task_id,
                                        sqlite3_int64 to_task_id,
                                        const char *relation_type) {
    const char *sql =
        "SELECT id, from_task_id, to_task_id, relation_type, created_at, "
        "created_by, notes FROM task_relations "
        "WHERE from_task_id = ? AND to_task_id = ? AND relation_type = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, from_task_id);
    sqlite3_bind_int64(stmt, 2, to_task_id);
    sqlite3_bind_text(stmt, 3, relation_type, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    cJSON *relation = NULL;
    if (rc == SQLITE_ROW) relation = relation_from_stmt(stmt);
    sqlite3_finalize(stmt);
    return relation;
}

static sqlite3_int64 task_json_plan_id(cJSON *task) {
    cJSON *plan_id = cJSON_GetObjectItemCaseSensitive(task, "plan_id");
    if (!cJSON_IsNumber(plan_id)) return 0;
    return (sqlite3_int64)plan_id->valuedouble;
}

static sqlite3_int64 task_json_nullable_id(cJSON *task, const char *field) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(task, field);
    if (!cJSON_IsNumber(item)) return 0;
    return (sqlite3_int64)item->valuedouble;
}

static const char *task_json_status(cJSON *task) {
    cJSON *status = cJSON_GetObjectItemCaseSensitive(task, "status");
    return cJSON_IsString(status) ? status->valuestring : NULL;
}

static const char *task_json_string(cJSON *task, const char *field) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(task, field);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static const char *task_json_resolution(cJSON *task) {
    cJSON *resolution = cJSON_GetObjectItemCaseSensitive(task, "resolution");
    return cJSON_IsString(resolution) ? resolution->valuestring : NULL;
}

static char *json_print_owned(cJSON *item) {
    if (item == NULL) return NULL;
    return cJSON_PrintUnformatted(item);
}

static cJSON *result_with_task(cJSON *task) {
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) return NULL;
    cJSON_AddItemToObject(result, "task", task);
    return result;
}

static int insert_task_event_with_id(sqlite3 *db,
                                     sqlite3_int64 task_id,
                                     const char *event_type,
                                     const char *actor,
                                     const char *request_id,
                                     const char *summary,
                                     const char *details,
                                     const char *old_json,
                                     const char *new_json,
                                     const char *related_entity_type,
                                     sqlite3_int64 related_entity_id,
                                     sqlite3_int64 *event_id_out) {
    const char *sql =
        "INSERT INTO events("
        "entity_type, entity_id, event_type, actor, summary, details, "
        "old_value, new_value, related_entity_type, related_entity_id, "
        "request_id"
        ") VALUES ('task', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, task_id);
    sqlite3_bind_text(stmt, 2, event_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, summary, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 5, details);
    bind_optional_text(stmt, 6, old_json);
    bind_optional_text(stmt, 7, new_json);
    bind_optional_text(stmt, 8, related_entity_type);
    if (related_entity_id > 0) {
        sqlite3_bind_int64(stmt, 9, related_entity_id);
    } else {
        sqlite3_bind_null(stmt, 9);
    }
    sqlite3_bind_text(stmt, 10, request_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE && event_id_out != NULL) {
        *event_id_out = sqlite3_last_insert_rowid(db);
    }
    return rc == SQLITE_DONE ? 0 : -1;
}

static int insert_task_event(sqlite3 *db,
                             sqlite3_int64 task_id,
                             const char *event_type,
                             const char *actor,
                             const char *request_id,
                             const char *summary,
                             const char *details,
                             const char *old_json,
                             const char *new_json,
                             const char *related_entity_type,
                             sqlite3_int64 related_entity_id) {
    return insert_task_event_with_id(db, task_id, event_type, actor,
                                     request_id, summary, details, old_json,
                                     new_json, related_entity_type,
                                     related_entity_id, NULL);
}

static int finish_task_change_event(sqlite3 *db,
                                    const ipman_request_t *req,
                                    cJSON *old_task,
                                    sqlite3_int64 task_id,
                                    const char *event_type,
                                    const char *summary,
                                    const char *details,
                                    const char *related_entity_type,
                                    sqlite3_int64 related_entity_id,
                                    sqlite3_int64 *event_id_out,
                                    cJSON **result_out,
                                    ipman_error_code_t *err_code_out,
                                    const char **err_msg_out) {
    cJSON *new_task = load_task(db, task_id);
    cJSON *result = NULL;
    char *old_json = json_print_owned(old_task);
    char *new_json = json_print_owned(new_task);
    if (new_task == NULL || old_json == NULL || new_json == NULL) {
        if (new_task != NULL) cJSON_Delete(new_task);
        if (old_json != NULL) cJSON_free(old_json);
        if (new_json != NULL) cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build task event";
        return -1;
    }
    if (insert_task_event_with_id(db, task_id, event_type, req->actor,
                                  req->request_id, summary, details, old_json,
                                  new_json, related_entity_type,
                                  related_entity_id, event_id_out) != 0) {
        cJSON_Delete(new_task);
        cJSON_free(old_json);
        cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record task event";
        return -1;
    }
    cJSON_free(old_json);
    cJSON_free(new_json);

    result = result_with_task(new_task);
    if (result == NULL) {
        cJSON_Delete(new_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build task response";
        return -1;
    }
    *result_out = result;
    return 0;
}

static int finish_task_change(sqlite3 *db,
                              const ipman_request_t *req,
                              cJSON *old_task,
                              sqlite3_int64 task_id,
                              const char *summary,
                              const char *details,
                              cJSON **result_out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out) {
    return finish_task_change_event(db, req, old_task, task_id,
                                    "task_updated", summary, details, NULL, 0,
                                    NULL, result_out, err_code_out,
                                    err_msg_out);
}

static int commit_result_owned(sqlite3 *db, cJSON **result_io,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out) {
    if (run_sql(db, "COMMIT;") != 0) {
        cJSON_Delete(*result_io);
        *result_io = NULL;
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to commit task change";
        return -1;
    }
    return 0;
}

static int validate_phase_in_plan(sqlite3 *db,
                                  sqlite3_int64 phase_id,
                                  sqlite3_int64 plan_id,
                                  ipman_error_code_t *err_code_out,
                                  const char **err_msg_out) {
    sqlite3_int64 actual_plan_id = 0;
    int rc = phase_plan_id(db, phase_id, &actual_plan_id);
    if (rc == 0) {
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "phase not found";
        return -1;
    }
    if (rc < 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to check phase";
        return -1;
    }
    if (actual_plan_id != plan_id) {
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "phase belongs to a different plan";
        return -1;
    }
    return 0;
}

static int validate_task_in_plan(sqlite3 *db,
                                 sqlite3_int64 task_id,
                                 sqlite3_int64 plan_id,
                                 const char *not_found_msg,
                                 const char *conflict_msg,
                                 ipman_error_code_t *err_code_out,
                                 const char **err_msg_out) {
    sqlite3_int64 actual_plan_id = 0;
    int rc = task_plan_id(db, task_id, &actual_plan_id);
    if (rc == 0) {
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = not_found_msg;
        return -1;
    }
    if (rc < 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to check task";
        return -1;
    }
    if (actual_plan_id != plan_id) {
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = conflict_msg;
        return -1;
    }
    return 0;
}

static int task_replaces_relation_count(sqlite3 *db, sqlite3_int64 task_id) {
    const char *sql =
        "SELECT COUNT(*) FROM task_relations "
        "WHERE from_task_id = ? AND relation_type = 'replaces';";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, task_id);
    rc = sqlite3_step(stmt);
    int count = -1;
    if (rc == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

static int task_relation_exists(sqlite3 *db,
                                sqlite3_int64 from_task_id,
                                sqlite3_int64 to_task_id,
                                const char *relation_type) {
    const char *sql =
        "SELECT COUNT(*) FROM task_relations "
        "WHERE from_task_id = ? AND to_task_id = ? AND relation_type = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, from_task_id);
    sqlite3_bind_int64(stmt, 2, to_task_id);
    sqlite3_bind_text(stmt, 3, relation_type, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    int count = -1;
    if (rc == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

static int validate_task_state_coherence(sqlite3 *db,
                                         sqlite3_int64 task_id,
                                         ipman_error_code_t *err_code_out,
                                         const char **err_msg_out) {
    cJSON *task = NULL;
    if (load_task_or_error(db, task_id, &task,
                           err_code_out, err_msg_out) != 0) {
        return -1;
    }
    const char *status = task_json_status(task);
    const char *resolution = task_json_resolution(task);
    int ok = 1;
    if (status == NULL) {
        ok = 0;
    } else if (strcmp(status, "done") == 0) {
        ok = resolution != NULL && strcmp(resolution, "completed") == 0;
    } else if (strcmp(status, "canceled") == 0) {
        ok = resolution != NULL &&
             (is_cancel_resolution(resolution) ||
              strcmp(resolution, "replaced") == 0);
        if (ok && strcmp(resolution, "replaced") == 0) {
            int count = task_replaces_relation_count(db, task_id);
            if (count < 0) {
                cJSON_Delete(task);
                *err_code_out = IPMAN_ERR_INTERNAL;
                *err_msg_out = "failed to validate replacement relation";
                return -1;
            }
            ok = count == 1;
        }
    } else {
        ok = resolution == NULL;
    }
    cJSON_Delete(task);
    if (!ok) {
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "task status and resolution are inconsistent";
        return -1;
    }
    return 0;
}

static int update_task_transition_state(sqlite3 *db,
                                        sqlite3_int64 task_id,
                                        const char *status) {
    const char *sql =
        "UPDATE tasks SET status = ?, resolution = NULL, closed_at = NULL, "
        "deferred_until = NULL, reason_code = NULL, reason_text = NULL, "
        "blocked_reason = CASE WHEN ? = 'blocked' THEN blocked_reason ELSE NULL END, "
        "started_at = CASE "
        "WHEN ? = 'in_progress' AND started_at IS NULL "
        "THEN strftime('%Y-%m-%dT%H:%M:%fZ', 'now') ELSE started_at END, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, task_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

static int update_task_deferred_state(sqlite3 *db,
                                      sqlite3_int64 task_id,
                                      const char *deferred_until,
                                      const char *reason_code,
                                      const char *reason_text) {
    const char *sql =
        "UPDATE tasks SET status = 'deferred', resolution = NULL, "
        "closed_at = NULL, deferred_until = ?, reason_code = ?, "
        "reason_text = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    bind_optional_text(stmt, 1, deferred_until);
    bind_optional_text(stmt, 2, reason_code);
    bind_optional_text(stmt, 3, reason_text);
    sqlite3_bind_int64(stmt, 4, task_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

static int update_task_canceled_state(sqlite3 *db,
                                      sqlite3_int64 task_id,
                                      const char *resolution,
                                      const char *reason_code,
                                      const char *reason_text) {
    const char *sql =
        "UPDATE tasks SET status = 'canceled', resolution = ?, "
        "closed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "deferred_until = NULL, reason_code = ?, reason_text = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, resolution, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 2, reason_code);
    bind_optional_text(stmt, 3, reason_text);
    sqlite3_bind_int64(stmt, 4, task_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

static int update_task_closed_state(sqlite3 *db, sqlite3_int64 task_id) {
    const char *sql =
        "UPDATE tasks SET status = 'done', resolution = 'completed', "
        "closed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "deferred_until = NULL, reason_code = NULL, reason_text = NULL, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, task_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

static int update_task_reopened_state(sqlite3 *db, sqlite3_int64 task_id) {
    const char *sql =
        "UPDATE tasks SET status = 'todo', resolution = NULL, closed_at = NULL, "
        "deferred_until = NULL, blocked_reason = NULL, "
        "reason_code = NULL, reason_text = NULL, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, task_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

static int insert_task(sqlite3 *db,
                       sqlite3_int64 plan_id,
                       int has_phase_id,
                       sqlite3_int64 phase_id,
                       int has_parent_task_id,
                       sqlite3_int64 parent_task_id,
                       const char *title,
                       const char *summary,
                       const char *description,
                       const char *status,
                       const char *priority,
                       const char *task_type,
                       const char *origin_type,
                       const char *assignee,
                       const char *due_date,
                       const char *target_start_date,
                       const char *estimate,
                       const char *origin_ref_type,
                       const char *origin_ref_id,
                       int has_origin_task_id,
                       sqlite3_int64 origin_task_id,
                       const char *label,
                       sqlite3_int64 *task_id_out) {
    const char *sql =
        "INSERT INTO tasks("
        "plan_id, phase_id, parent_task_id, title, summary, description, "
        "status, priority, task_type, origin_type, assignee, due_date, "
        "target_start_date, estimate, origin_ref_type, origin_ref_id, "
        "origin_task_id, label"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, plan_id);
    if (has_phase_id) sqlite3_bind_int64(stmt, 2, phase_id);
    else sqlite3_bind_null(stmt, 2);
    if (has_parent_task_id) sqlite3_bind_int64(stmt, 3, parent_task_id);
    else sqlite3_bind_null(stmt, 3);
    sqlite3_bind_text(stmt, 4, title, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 5, summary);
    bind_optional_text(stmt, 6, description);
    sqlite3_bind_text(stmt, 7, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, priority, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, task_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, origin_type, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 11, assignee);
    bind_optional_text(stmt, 12, due_date);
    bind_optional_text(stmt, 13, target_start_date);
    bind_optional_text(stmt, 14, estimate);
    bind_optional_text(stmt, 15, origin_ref_type);
    bind_optional_text(stmt, 16, origin_ref_id);
    if (has_origin_task_id) sqlite3_bind_int64(stmt, 17, origin_task_id);
    else sqlite3_bind_null(stmt, 17);
    
    char generated_label[64] = {0};
    if (label != NULL && !is_blank(label)) {
        sqlite3_bind_text(stmt, 18, label, -1, SQLITE_TRANSIENT);
    } else {
        ipman_slugify(title, generated_label, sizeof(generated_label));
        if (generated_label[0] == '\0') {
            sqlite3_bind_null(stmt, 18);
        } else {
            sqlite3_bind_text(stmt, 18, generated_label, -1, SQLITE_TRANSIENT);
        }
    }
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return rc;
    sqlite3_int64 new_id = sqlite3_last_insert_rowid(db);
    *task_id_out = new_id;
    
    char uid_buf[64];
    snprintf(uid_buf, sizeof(uid_buf), "task_%lld", (long long)new_id);
    
    if (label == NULL || is_blank(label)) {
        if (generated_label[0] == '\0') {
            snprintf(generated_label, sizeof(generated_label), "task-%lld", (long long)new_id);
        }
        const char *update_sql = "UPDATE tasks SET uid = ?, label = ? WHERE id = ?;";
        rc = sqlite3_prepare_v2(db, update_sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) return rc;
        sqlite3_bind_text(stmt, 1, uid_buf, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, generated_label, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, new_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) return rc;
    } else {
        const char *update_sql = "UPDATE tasks SET uid = ? WHERE id = ?;";
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

const ipman_param_desc_t ipman_op_task_create_params[] = {
    { "plan_id" }, { "phase_id" }, { "parent_task_id" },
    { "origin_task_id" },
    { "title" }, { "summary" }, { "description" },
    { "status" }, { "priority" }, { "task_type" }, { "origin_type" },
    { "assignee" }, { "due_date" }, { "target_start_date" }, { "estimate" },
    { "origin_ref_type" }, { "origin_ref_id" }, { "label" },
    { NULL },
};

int ipman_op_task_create(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out) {
    sqlite3_int64 plan_id = 0;
    int has_phase_id = 0;
    sqlite3_int64 phase_id = 0;
    int has_parent_task_id = 0;
    sqlite3_int64 parent_task_id = 0;
    int has_origin_task_id = 0;
    sqlite3_int64 origin_task_id = 0;
    const char *title = NULL;
    const char *summary = NULL;
    const char *description = NULL;
    const char *status = NULL;
    const char *priority = NULL;
    const char *task_type = NULL;
    const char *origin_type = NULL;
    const char *assignee = NULL;
    const char *due_date = NULL;
    const char *target_start_date = NULL;
    const char *estimate = NULL;
    const char *origin_ref_type = NULL;
    const char *origin_ref_id = NULL;
    const char *label = NULL;

    if (ipman_read_positive_id(req->params, "plan_id", &plan_id,
                         err_code_out, err_msg_out) != 0 ||
        read_optional_id(req->params, "phase_id", &has_phase_id, &phase_id,
                         err_code_out, err_msg_out) != 0 ||
        read_optional_id(req->params, "parent_task_id", &has_parent_task_id,
                         &parent_task_id, err_code_out, err_msg_out) != 0 ||
        read_nullable_id_update(req->params, "origin_task_id",
                                &has_origin_task_id, &origin_task_id,
                                err_code_out, err_msg_out) != 0 ||
        read_required_title(req->params, &title, err_code_out,
                            err_msg_out) != 0 ||
        read_optional_task_status(req->params, &status, err_code_out,
                                  err_msg_out) != 0 ||
        read_optional_priority(req->params, &priority, err_code_out,
                               err_msg_out) != 0 ||
        read_optional_task_type(req->params, &task_type, err_code_out,
                                err_msg_out) != 0 ||
        read_optional_origin_type(req->params, &origin_type, err_code_out,
                                  err_msg_out) != 0 ||
        read_optional_string(req->params, "summary", &summary,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "description", &description,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "assignee", &assignee,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "due_date", &due_date,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "target_start_date",
                             &target_start_date, err_code_out,
                             err_msg_out) != 0 ||
        read_optional_string(req->params, "estimate", &estimate,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "origin_ref_type", &origin_ref_type,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "origin_ref_id", &origin_ref_id,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "label", &label,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (origin_task_id == 0) has_origin_task_id = 0;
    if (strcmp(status, "todo") != 0) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "task.create status must be todo";
        return -1;
    }

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    int exists = entity_exists(db, "plans", plan_id);
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
    if (has_phase_id &&
        validate_phase_in_plan(db, phase_id, plan_id,
                               err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    if (has_parent_task_id &&
        validate_task_in_plan(db, parent_task_id, plan_id,
                              "parent task not found",
                              "parent task belongs to a different plan",
                              err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    if (has_origin_task_id &&
        validate_task_in_plan(db, origin_task_id, plan_id,
                              "origin task not found",
                              "origin task belongs to a different plan",
                              err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }

    sqlite3_int64 task_id = 0;
    int insert_rc = insert_task(db, plan_id, has_phase_id, phase_id,
                                has_parent_task_id, parent_task_id, title,
                                summary, description, status, priority,
                                task_type, origin_type, assignee, due_date,
                                target_start_date, estimate, origin_ref_type,
                                origin_ref_id, has_origin_task_id,
                                origin_task_id, label, &task_id);
    if (insert_rc != 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = create_sqlite_error_code(db, insert_rc);
        *err_msg_out = "failed to create task";
        return -1;
    }

    cJSON *task = load_task(db, task_id);
    cJSON *result = cJSON_CreateObject();
    char *task_json = json_print_owned(task);
    if (task == NULL || result == NULL || task_json == NULL) {
        if (task != NULL) cJSON_Delete(task);
        if (result != NULL) cJSON_Delete(result);
        if (task_json != NULL) cJSON_free(task_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build task response";
        return -1;
    }
    if (insert_task_event(db, task_id, "task_created", req->actor,
                          req->request_id, "task created",
                          "{\"op\":\"task.create\"}", NULL, task_json,
                          NULL, 0) != 0) {
        cJSON_free(task_json);
        cJSON_Delete(task);
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record task event";
        return -1;
    }
    cJSON_free(task_json);
    cJSON_AddItemToObject(result, "task", task);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_task_get_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_task_get(const ipman_request_t *req, sqlite3 *db,
                     cJSON **result_out,
                     ipman_error_code_t *err_code_out,
                     const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0) {
        return -1;
    }
    cJSON *task = NULL;
    if (load_task_or_error(db, task_id, &task,
                           err_code_out, err_msg_out) != 0) {
        return -1;
    }
    cJSON *result = result_with_task(task);
    if (result == NULL) {
        cJSON_Delete(task);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build task response";
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_task_lookup_params[] = {
    { "uid" }, { "label" },
    { "plan_id" },
    { NULL },
};

int ipman_op_task_lookup(const ipman_request_t *req, sqlite3 *db,
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

    sqlite3_int64 task_id = 0;
    if (has_uid) {
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(db, "SELECT id FROM tasks WHERE uid = ?",
                                    -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to prepare task lookup";
            return -1;
        }
        sqlite3_bind_text(stmt, 1, uid_item->valuestring, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_NOT_FOUND;
            *err_msg_out = "task not found by uid";
            return -1;
        }
        task_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    } else {
        sqlite3_int64 plan_id = 0;
        if (ipman_resolve_plan_scope(req->params, db, &plan_id,
                                     err_code_out, err_msg_out) != 0) return -1;
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(db,
            "SELECT id FROM tasks WHERE label = ? AND plan_id = ?",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to prepare task lookup";
            return -1;
        }
        sqlite3_bind_text(stmt, 1, label_item->valuestring, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, plan_id);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_NOT_FOUND;
            *err_msg_out = "task not found by label";
            return -1;
        }
        task_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }

    cJSON *result = cJSON_CreateObject();
    if (result == NULL ||
        cJSON_AddNumberToObject(result, "id", (double)task_id) == NULL) {
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

static int update_task_fields(sqlite3 *db,
                              sqlite3_int64 task_id,
                              const task_update_t *update) {
    const char *sql =
        "UPDATE tasks SET "
        "title = CASE WHEN ? THEN ? ELSE title END, "
        "summary = CASE WHEN ? THEN ? ELSE summary END, "
        "description = CASE WHEN ? THEN ? ELSE description END, "
        "due_date = CASE WHEN ? THEN ? ELSE due_date END, "
        "target_start_date = CASE WHEN ? THEN ? ELSE target_start_date END, "
        "estimate = CASE WHEN ? THEN ? ELSE estimate END, "
        "blocked_reason = CASE WHEN ? THEN ? ELSE blocked_reason END, "
        "reason_code = CASE WHEN ? THEN ? ELSE reason_code END, "
        "reason_text = CASE WHEN ? THEN ? ELSE reason_text END, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    bind_update_text(stmt, 1, 2, update->title_set, update->title);
    bind_update_text(stmt, 3, 4, update->summary_set, update->summary);
    bind_update_text(stmt, 5, 6, update->description_set,
                     update->description);
    bind_update_text(stmt, 7, 8, update->due_date_set, update->due_date);
    bind_update_text(stmt, 9, 10, update->target_start_date_set,
                     update->target_start_date);
    bind_update_text(stmt, 11, 12, update->estimate_set, update->estimate);
    bind_update_text(stmt, 13, 14, update->blocked_reason_set,
                     update->blocked_reason);
    bind_update_text(stmt, 15, 16, update->reason_code_set,
                     update->reason_code);
    bind_update_text(stmt, 17, 18, update->reason_text_set,
                     update->reason_text);
    sqlite3_bind_int64(stmt, 19, task_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

const ipman_param_desc_t ipman_op_task_update_params[] = {
    { "id" }, { "title" }, { "summary" }, { "description" },
    { "due_date" }, { "target_start_date" }, { "estimate" },
    { "blocked_reason" }, { "reason_code" }, { "reason_text" },
    { NULL },
};

int ipman_op_task_update(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    task_update_t update;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_task_update(req->params, &update, err_code_out,
                         err_msg_out) != 0) {
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    cJSON *old_task = NULL;
    if (load_task_or_error(db, task_id, &old_task,
                           err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    if (update.blocked_reason_set) {
        const char *cur_status = task_json_status(old_task);
        if (cur_status == NULL || strcmp(cur_status, "blocked") != 0) {
            cJSON_Delete(old_task);
            run_sql(db, "ROLLBACK;");
            *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
            *err_msg_out = "blocked_reason can only be set on a blocked task";
            return -1;
        }
    }
    if (update_task_fields(db, task_id, &update) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to update task";
        return -1;
    }
    int rc = finish_task_change(db, req, old_task, task_id, "task updated",
                                "{\"op\":\"task.update\"}", result_out,
                                err_code_out, err_msg_out);
    cJSON_Delete(old_task);
    if (rc != 0) return -1;
    return commit_result_owned(db, result_out, err_code_out, err_msg_out);
}

static int update_task_phase(sqlite3 *db,
                             sqlite3_int64 task_id,
                             int has_phase_id,
                             sqlite3_int64 phase_id) {
    const char *sql =
        "UPDATE tasks SET phase_id = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    if (has_phase_id) {
        sqlite3_bind_int64(stmt, 1, phase_id);
    } else {
        sqlite3_bind_null(stmt, 1);
    }
    sqlite3_bind_int64(stmt, 2, task_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

const ipman_param_desc_t ipman_op_task_move_params[] = {
    { "id" }, { "phase_id" },
    { NULL },
};

int ipman_op_task_move(const ipman_request_t *req, sqlite3 *db,
                      cJSON **result_out,
                      ipman_error_code_t *err_code_out,
                      const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    int phase_present = 0;
    sqlite3_int64 phase_id = 0;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_nullable_id_update(req->params, "phase_id", &phase_present,
                                &phase_id, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (!phase_present) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "phase_id is required and may be null";
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    cJSON *old_task = NULL;
    if (load_task_or_error(db, task_id, &old_task,
                           err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    sqlite3_int64 plan_id = task_json_plan_id(old_task);
    if (phase_id > 0 &&
        validate_phase_in_plan(db, phase_id, plan_id,
                               err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    if (update_task_phase(db, task_id, phase_id > 0, phase_id) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to move task";
        return -1;
    }
    int rc = finish_task_change(db, req, old_task, task_id, "task moved",
                                "{\"op\":\"task.move\"}", result_out,
                                err_code_out, err_msg_out);
    cJSON_Delete(old_task);
    if (rc != 0) return -1;
    return commit_result_owned(db, result_out, err_code_out, err_msg_out);
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

const ipman_param_desc_t ipman_op_task_list_params[] = {
    { "plan_id" }, { "phase_id" },
    { "status" }, { "origin_type" },
    { "assignee" }, { "priority" }, { "resolution" },
    { "blocked" }, { "deferred" }, { "added_after_original" },
    { "canceled" }, { "replaced" }, { "blocked_by_other" }, { "pending" },
    { "limit" }, { "offset" },
    { NULL },
};

int ipman_op_task_list(const ipman_request_t *req, sqlite3 *db,
                      cJSON **result_out,
                      ipman_error_code_t *err_code_out,
                      const char **err_msg_out) {
    int has_plan_id = 0;
    sqlite3_int64 plan_id = 0;
    int has_phase_id = 0;
    sqlite3_int64 phase_id = 0;
    const char *status = NULL;
    const char *origin_type = NULL;
    const char *assignee = NULL;
    const char *priority = NULL;
    const char *resolution = NULL;
    int has_blocked = 0;
    int blocked = 0;
    int has_deferred = 0;
    int deferred = 0;
    int has_added_after_original = 0;
    int added_after_original = 0;
    int has_canceled = 0;
    int canceled = 0;
    int has_replaced = 0;
    int replaced = 0;
    int has_blocked_by_other = 0;
    int blocked_by_other = 0;
    int has_pending = 0;
    int pending = 0;
    int limit = TASK_LIST_DEFAULT_LIMIT;
    int offset = 0;

    if (read_optional_id(req->params, "plan_id", &has_plan_id, &plan_id,
                         err_code_out, err_msg_out) != 0 ||
        read_optional_id(req->params, "phase_id", &has_phase_id, &phase_id,
                         err_code_out, err_msg_out) != 0 ||
        read_optional_filter(req->params, "status", &status,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_filter(req->params, "origin_type", &origin_type,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_filter(req->params, "assignee", &assignee,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_filter(req->params, "priority", &priority,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_filter(req->params, "resolution", &resolution,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_bool(req->params, "blocked", &has_blocked, &blocked,
                           err_code_out, err_msg_out) != 0 ||
        read_optional_bool(req->params, "deferred", &has_deferred, &deferred,
                           err_code_out, err_msg_out) != 0 ||
        read_optional_bool(req->params, "added_after_original",
                           &has_added_after_original, &added_after_original,
                           err_code_out, err_msg_out) != 0 ||
        read_optional_bool(req->params, "canceled", &has_canceled, &canceled,
                           err_code_out, err_msg_out) != 0 ||
        read_optional_bool(req->params, "replaced", &has_replaced, &replaced,
                           err_code_out, err_msg_out) != 0 ||
        read_optional_bool(req->params, "blocked_by_other",
                           &has_blocked_by_other, &blocked_by_other,
                           err_code_out, err_msg_out) != 0 ||
        read_optional_bool(req->params, "pending", &has_pending, &pending,
                           err_code_out, err_msg_out) != 0 ||
        read_list_int(req->params, "limit", TASK_LIST_DEFAULT_LIMIT, &limit,
                      err_code_out, err_msg_out) != 0 ||
        read_list_int(req->params, "offset", 0, &offset,
                      err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (limit < 1 || limit > TASK_LIST_MAX_LIMIT) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "limit must be between 1 and 500";
        return -1;
    }
    if ((status != NULL && !is_task_status(status)) ||
        (origin_type != NULL && !is_origin_type(origin_type)) ||
        (priority != NULL && !is_priority(priority)) ||
        (resolution != NULL && !is_task_resolution(resolution))) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "task.list filters contain an invalid enum value";
        return -1;
    }

    /* Count total matching rows */
    int rc = 0;
    const char *count_sql =
        "SELECT COUNT(*) FROM tasks "
        "WHERE (? = 0 OR plan_id = ?) "
        "AND (? = 0 OR phase_id = ?) "
        "AND (? IS NULL OR status = ?) "
        "AND (? IS NULL OR origin_type = ?) "
        "AND (? IS NULL OR assignee = ?) "
        "AND (? IS NULL OR priority = ?) "
        "AND (? IS NULL OR resolution = ?) "
        "AND (? = 0 OR (? = 1 AND status = 'blocked') "
        "    OR (? = 0 AND status <> 'blocked')) "
        "AND (? = 0 OR (? = 1 AND status = 'deferred') "
        "    OR (? = 0 AND status <> 'deferred')) "
        "AND (? = 0 OR (? = 1 AND origin_type <> 'planned') "
        "    OR (? = 0 AND origin_type = 'planned')) "
        "AND (? = 0 OR (? = 1 AND status = 'canceled') "
        "    OR (? = 0 AND status <> 'canceled')) "
        "AND (? = 0 OR (? = 1 AND resolution = 'replaced') "
        "    OR (? = 0 AND (resolution IS NULL OR resolution <> 'replaced'))) "
        "AND (? = 0 OR (? = 1 AND EXISTS ("
        "        SELECT 1 FROM task_relations tr "
        "        WHERE tr.from_task_id = tasks.id "
        "        AND tr.relation_type = 'blocked_by')) "
        "    OR (? = 0 AND NOT EXISTS ("
        "        SELECT 1 FROM task_relations tr "
        "        WHERE tr.from_task_id = tasks.id "
        "        AND tr.relation_type = 'blocked_by'))) "
        "AND (? = 0 OR (? = 1 AND status IN "
        "        ('todo', 'in_progress', 'blocked', 'deferred')) "
        "    OR (? = 0 AND status NOT IN "
        "        ('todo', 'in_progress', 'blocked', 'deferred')));";
    sqlite3_stmt *count_stmt = NULL;
    rc = sqlite3_prepare_v2(db, count_sql, -1, &count_stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to count tasks";
        return -1;
    }
    sqlite3_bind_int(count_stmt, 1, has_plan_id);
    sqlite3_bind_int64(count_stmt, 2, plan_id);
    sqlite3_bind_int(count_stmt, 3, has_phase_id);
    sqlite3_bind_int64(count_stmt, 4, phase_id);
    bind_optional_text(count_stmt, 5, status);
    bind_optional_text(count_stmt, 6, status);
    bind_optional_text(count_stmt, 7, origin_type);
    bind_optional_text(count_stmt, 8, origin_type);
    bind_optional_text(count_stmt, 9, assignee);
    bind_optional_text(count_stmt, 10, assignee);
    bind_optional_text(count_stmt, 11, priority);
    bind_optional_text(count_stmt, 12, priority);
    bind_optional_text(count_stmt, 13, resolution);
    bind_optional_text(count_stmt, 14, resolution);
    sqlite3_bind_int(count_stmt, 15, has_blocked);
    sqlite3_bind_int(count_stmt, 16, blocked);
    sqlite3_bind_int(count_stmt, 17, blocked);
    sqlite3_bind_int(count_stmt, 18, has_deferred);
    sqlite3_bind_int(count_stmt, 19, deferred);
    sqlite3_bind_int(count_stmt, 20, deferred);
    sqlite3_bind_int(count_stmt, 21, has_added_after_original);
    sqlite3_bind_int(count_stmt, 22, added_after_original);
    sqlite3_bind_int(count_stmt, 23, added_after_original);
    sqlite3_bind_int(count_stmt, 24, has_canceled);
    sqlite3_bind_int(count_stmt, 25, canceled);
    sqlite3_bind_int(count_stmt, 26, canceled);
    sqlite3_bind_int(count_stmt, 27, has_replaced);
    sqlite3_bind_int(count_stmt, 28, replaced);
    sqlite3_bind_int(count_stmt, 29, replaced);
    sqlite3_bind_int(count_stmt, 30, has_blocked_by_other);
    sqlite3_bind_int(count_stmt, 31, blocked_by_other);
    sqlite3_bind_int(count_stmt, 32, blocked_by_other);
    sqlite3_bind_int(count_stmt, 33, has_pending);
    sqlite3_bind_int(count_stmt, 34, pending);
    sqlite3_bind_int(count_stmt, 35, pending);
    rc = sqlite3_step(count_stmt);
    int total_count = (rc == SQLITE_ROW) ? sqlite3_column_int(count_stmt, 0) : 0;
    sqlite3_finalize(count_stmt);

    const char *sql =
        "SELECT id, plan_id, phase_id, parent_task_id, title, summary, "
        "description, status, resolution, priority, task_type, origin_type, "
        "assignee, created_at, updated_at, started_at, closed_at, "
        "deferred_until, blocked_reason, reason_code, reason_text, due_date, "
        "target_start_date, estimate, origin_ref_type, origin_ref_id, "
        "origin_task_id, local_seq, "
        "uid, label "
        "FROM tasks "
        "WHERE (? = 0 OR plan_id = ?) "
        "AND (? = 0 OR phase_id = ?) "
        "AND (? IS NULL OR status = ?) "
        "AND (? IS NULL OR origin_type = ?) "
        "AND (? IS NULL OR assignee = ?) "
        "AND (? IS NULL OR priority = ?) "
        "AND (? IS NULL OR resolution = ?) "
        "AND (? = 0 OR (? = 1 AND status = 'blocked') "
        "    OR (? = 0 AND status <> 'blocked')) "
        "AND (? = 0 OR (? = 1 AND status = 'deferred') "
        "    OR (? = 0 AND status <> 'deferred')) "
        "AND (? = 0 OR (? = 1 AND origin_type <> 'planned') "
        "    OR (? = 0 AND origin_type = 'planned')) "
        "AND (? = 0 OR (? = 1 AND status = 'canceled') "
        "    OR (? = 0 AND status <> 'canceled')) "
        "AND (? = 0 OR (? = 1 AND resolution = 'replaced') "
        "    OR (? = 0 AND (resolution IS NULL OR resolution <> 'replaced'))) "
        "AND (? = 0 OR (? = 1 AND EXISTS ("
        "        SELECT 1 FROM task_relations tr "
        "        WHERE tr.from_task_id = tasks.id "
        "        AND tr.relation_type = 'blocked_by')) "
        "    OR (? = 0 AND NOT EXISTS ("
        "        SELECT 1 FROM task_relations tr "
        "        WHERE tr.from_task_id = tasks.id "
        "        AND tr.relation_type = 'blocked_by'))) "
        "AND (? = 0 OR (? = 1 AND status IN "
        "        ('todo', 'in_progress', 'blocked', 'deferred')) "
        "    OR (? = 0 AND status NOT IN "
        "        ('todo', 'in_progress', 'blocked', 'deferred'))) "
        "ORDER BY id ASC LIMIT ? OFFSET ?;";
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query tasks";
        return -1;
    }
    sqlite3_bind_int(stmt, 1, has_plan_id);
    sqlite3_bind_int64(stmt, 2, plan_id);
    sqlite3_bind_int(stmt, 3, has_phase_id);
    sqlite3_bind_int64(stmt, 4, phase_id);
    bind_optional_text(stmt, 5, status);
    bind_optional_text(stmt, 6, status);
    bind_optional_text(stmt, 7, origin_type);
    bind_optional_text(stmt, 8, origin_type);
    bind_optional_text(stmt, 9, assignee);
    bind_optional_text(stmt, 10, assignee);
    bind_optional_text(stmt, 11, priority);
    bind_optional_text(stmt, 12, priority);
    bind_optional_text(stmt, 13, resolution);
    bind_optional_text(stmt, 14, resolution);
    sqlite3_bind_int(stmt, 15, has_blocked);
    sqlite3_bind_int(stmt, 16, blocked);
    sqlite3_bind_int(stmt, 17, blocked);
    sqlite3_bind_int(stmt, 18, has_deferred);
    sqlite3_bind_int(stmt, 19, deferred);
    sqlite3_bind_int(stmt, 20, deferred);
    sqlite3_bind_int(stmt, 21, has_added_after_original);
    sqlite3_bind_int(stmt, 22, added_after_original);
    sqlite3_bind_int(stmt, 23, added_after_original);
    sqlite3_bind_int(stmt, 24, has_canceled);
    sqlite3_bind_int(stmt, 25, canceled);
    sqlite3_bind_int(stmt, 26, canceled);
    sqlite3_bind_int(stmt, 27, has_replaced);
    sqlite3_bind_int(stmt, 28, replaced);
    sqlite3_bind_int(stmt, 29, replaced);
    sqlite3_bind_int(stmt, 30, has_blocked_by_other);
    sqlite3_bind_int(stmt, 31, blocked_by_other);
    sqlite3_bind_int(stmt, 32, blocked_by_other);
    sqlite3_bind_int(stmt, 33, has_pending);
    sqlite3_bind_int(stmt, 34, pending);
    sqlite3_bind_int(stmt, 35, pending);
    sqlite3_bind_int(stmt, 36, limit + 1);
    sqlite3_bind_int(stmt, 37, offset);

    cJSON *result = cJSON_CreateObject();
    cJSON *tasks = cJSON_CreateArray();
    if (result == NULL || tasks == NULL) {
        if (result != NULL) cJSON_Delete(result);
        if (tasks != NULL) cJSON_Delete(tasks);
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build task list";
        return -1;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *task = ipman_task_from_row(stmt);
        if (task == NULL) {
            cJSON_Delete(result);
            cJSON_Delete(tasks);
            sqlite3_finalize(stmt);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build task list";
            return -1;
        }
        cJSON_AddItemToArray(tasks, task);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        cJSON_Delete(result);
        cJSON_Delete(tasks);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query tasks";
        return -1;
    }
    int has_more = 0;
    int row_count = cJSON_GetArraySize(tasks);
    if (row_count > limit) {
        has_more = 1;
        cJSON *last = cJSON_DetachItemFromArray(tasks, row_count - 1);
        cJSON_Delete(last);
    }
    cJSON_AddItemToObject(result, "tasks", tasks);
    cJSON_AddNumberToObject(result, "limit", limit);
    cJSON_AddNumberToObject(result, "offset", offset);
    cJSON_AddBoolToObject(result, "has_more", has_more);
    cJSON_AddNumberToObject(result, "total_count", total_count);
    *result_out = result;
    return 0;
}

static int update_task_assignee(sqlite3 *db,
                                sqlite3_int64 task_id,
                                const char *assignee) {
    const char *sql =
        "UPDATE tasks SET assignee = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    bind_optional_text(stmt, 1, assignee);
    sqlite3_bind_int64(stmt, 2, task_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

static int change_task_with_old(sqlite3 *db,
                                const ipman_request_t *req,
                                sqlite3_int64 task_id,
                                const char *summary,
                                const char *details,
                                int (*changer)(sqlite3 *, sqlite3_int64,
                                               const char *),
                                const char *value,
                                cJSON **result_out,
                                ipman_error_code_t *err_code_out,
                                const char **err_msg_out) {
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    cJSON *old_task = NULL;
    if (load_task_or_error(db, task_id, &old_task,
                           err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    if (changer(db, task_id, value) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to update task";
        return -1;
    }
    int rc = finish_task_change(db, req, old_task, task_id, summary, details,
                                result_out, err_code_out, err_msg_out);
    cJSON_Delete(old_task);
    if (rc != 0) return -1;
    return commit_result_owned(db, result_out, err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_task_assign_params[] = {
    { "id" }, { "assignee" },
    { NULL },
};

int ipman_op_task_assign(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    const char *assignee = NULL;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_required_string(req->params, "assignee", &assignee,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    return change_task_with_old(db, req, task_id, "task assigned",
                                "{\"op\":\"task.assign\"}",
                                update_task_assignee, assignee, result_out,
                                err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_task_unassign_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_task_unassign(const ipman_request_t *req, sqlite3 *db,
                          cJSON **result_out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0) {
        return -1;
    }
    return change_task_with_old(db, req, task_id, "task unassigned",
                                "{\"op\":\"task.unassign\"}",
                                update_task_assignee, NULL, result_out,
                                err_code_out, err_msg_out);
}

static int update_task_priority(sqlite3 *db,
                                sqlite3_int64 task_id,
                                const char *priority) {
    const char *sql =
        "UPDATE tasks SET priority = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, priority, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, task_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

const ipman_param_desc_t ipman_op_task_set_priority_params[] = {
    { "id" }, { "priority" },
    { NULL },
};

int ipman_op_task_set_priority(const ipman_request_t *req, sqlite3 *db,
                              cJSON **result_out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    const char *priority = NULL;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_required_string(req->params, "priority", &priority,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (!is_priority(priority)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "priority must be one of low, medium, high, critical";
        return -1;
    }
    return change_task_with_old(db, req, task_id, "task priority changed",
                                "{\"op\":\"task.set_priority\"}",
                                update_task_priority, priority, result_out,
                                err_code_out, err_msg_out);
}

static int update_task_type(sqlite3 *db,
                            sqlite3_int64 task_id,
                            const char *task_type) {
    const char *sql =
        "UPDATE tasks SET task_type = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, task_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, task_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

const ipman_param_desc_t ipman_op_task_set_type_params[] = {
    { "id" }, { "task_type" },
    { NULL },
};

int ipman_op_task_set_type(const ipman_request_t *req, sqlite3 *db,
                          cJSON **result_out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    const char *task_type = NULL;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_required_string(req->params, "task_type", &task_type,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (!is_task_type(task_type)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "task_type must be a valid task type";
        return -1;
    }
    return change_task_with_old(db, req, task_id, "task type changed",
                                "{\"op\":\"task.set_type\"}",
                                update_task_type, task_type, result_out,
                                err_code_out, err_msg_out);
}

static int update_task_origin(sqlite3 *db,
                              sqlite3_int64 task_id,
                              const char *origin_type,
                              const char *origin_ref_type,
                              const char *origin_ref_id,
                              int has_origin_task_id,
                              sqlite3_int64 origin_task_id) {
    const char *sql =
        "UPDATE tasks SET origin_type = ?, origin_ref_type = ?, "
        "origin_ref_id = ?, origin_task_id = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, origin_type, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 2, origin_ref_type);
    bind_optional_text(stmt, 3, origin_ref_id);
    if (has_origin_task_id) {
        sqlite3_bind_int64(stmt, 4, origin_task_id);
    } else {
        sqlite3_bind_null(stmt, 4);
    }
    sqlite3_bind_int64(stmt, 5, task_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

static int update_task_external_ref(sqlite3 *db,
                                    sqlite3_int64 task_id,
                                    const char *ref_type,
                                    const char *ref_id) {
    const char *sql =
        "UPDATE tasks SET origin_ref_type = ?, origin_ref_id = ?, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
        "WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, ref_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, ref_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, task_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
}

const ipman_param_desc_t ipman_op_task_set_origin_params[] = {
    { "id" }, { "origin_type" },
    { "origin_ref_type" }, { "origin_ref_id" }, { "origin_task_id" },
    { NULL },
};

int ipman_op_task_set_origin(const ipman_request_t *req, sqlite3 *db,
                            cJSON **result_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    const char *origin_type = NULL;
    const char *origin_ref_type = NULL;
    const char *origin_ref_id = NULL;
    int origin_task_present = 0;
    sqlite3_int64 origin_task_id = 0;

    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_required_string(req->params, "origin_type", &origin_type,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "origin_ref_type", &origin_ref_type,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "origin_ref_id", &origin_ref_id,
                             err_code_out, err_msg_out) != 0 ||
        read_nullable_id_update(req->params, "origin_task_id",
                                &origin_task_present, &origin_task_id,
                                err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (origin_task_id == 0) origin_task_present = 0;
    if (!is_origin_type(origin_type)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "origin_type must be a valid origin type";
        return -1;
    }
    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    cJSON *old_task = NULL;
    if (load_task_or_error(db, task_id, &old_task,
                           err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    if (origin_task_present && origin_task_id > 0 &&
        validate_task_in_plan(db, origin_task_id, task_json_plan_id(old_task),
                              "origin task not found",
                              "origin task belongs to a different plan",
                              err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    if (update_task_origin(db, task_id, origin_type, origin_ref_type,
                           origin_ref_id, origin_task_present,
                           origin_task_id) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to set task origin";
        return -1;
    }
    int rc = finish_task_change(db, req, old_task, task_id,
                                "task origin changed",
                                "{\"op\":\"task.set_origin\"}", result_out,
                                err_code_out, err_msg_out);
    cJSON_Delete(old_task);
    if (rc != 0) return -1;
    return commit_result_owned(db, result_out, err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_task_link_external_params[] = {
    { "task_id" }, { "ref_type" }, { "ref_id" },
    { NULL },
};

int ipman_op_task_link_external(const ipman_request_t *req, sqlite3 *db,
                               cJSON **result_out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    const char *ref_type = NULL;
    const char *ref_id = NULL;
    cJSON *old_task = NULL;
    cJSON *details_obj = NULL;
    char *details_json = NULL;

    if (ipman_read_positive_id(req->params, "task_id", &task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_required_string(req->params, "ref_type", &ref_type,
                             err_code_out, err_msg_out) != 0 ||
        read_required_string(req->params, "ref_id", &ref_id,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (!is_external_ref_type(ref_type)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out =
            "ref_type must be github_issue, gitlab_issue, commit, pr, adr, url, or file";
        return -1;
    }

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    if (load_task_or_error(db, task_id, &old_task,
                           err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    if (update_task_external_ref(db, task_id, ref_type, ref_id) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to link task external reference";
        return -1;
    }
    details_obj = cJSON_CreateObject();
    if (details_obj == NULL ||
        cJSON_AddStringToObject(details_obj, "op",
                                "task.link_external") == NULL ||
        cJSON_AddStringToObject(details_obj, "ref_type", ref_type) == NULL ||
        cJSON_AddStringToObject(details_obj, "ref_id", ref_id) == NULL) {
        if (details_obj != NULL) cJSON_Delete(details_obj);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build task external reference details";
        return -1;
    }
    details_json = cJSON_PrintUnformatted(details_obj);
    cJSON_Delete(details_obj);
    if (details_json == NULL) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to serialize task external reference details";
        return -1;
    }
    int rc = finish_task_change_event(db, req, old_task, task_id,
                                      "task_linked_external",
                                      "task external reference linked",
                                      details_json, NULL, 0, NULL, result_out,
                                      err_code_out, err_msg_out);
    cJSON_free(details_json);
    cJSON_Delete(old_task);
    if (rc != 0) return -1;
    return commit_result_owned(db, result_out, err_code_out, err_msg_out);
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

static int insert_comment(sqlite3 *db,
                          sqlite3_int64 task_id,
                          const char *comment_type,
                          const char *body,
                          const char *author,
                          sqlite3_int64 *comment_id_out) {
    const char *sql =
        "INSERT INTO comments(entity_type, entity_id, comment_type, body, author) "
        "VALUES ('task', ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, task_id);
    sqlite3_bind_text(stmt, 2, comment_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, body, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, author, -1, SQLITE_TRANSIENT);
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

const ipman_param_desc_t ipman_op_task_comment_add_params[] = {
    { "id" }, { "body" }, { "comment_type" },
    { NULL },
};

int ipman_op_task_comment_add(const ipman_request_t *req, sqlite3 *db,
                             cJSON **result_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    const char *comment_type = NULL;
    const char *body = NULL;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_required_string(req->params, "body", &body,
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
    int exists = entity_exists(db, "tasks", task_id);
    if (exists == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "task not found";
        return -1;
    }
    if (exists < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to check task";
        return -1;
    }
    sqlite3_int64 comment_id = 0;
    if (insert_comment(db, task_id, comment_type, body, req->actor,
                       &comment_id) != 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to add task comment";
        return -1;
    }
    cJSON *comment = load_comment(db, comment_id);
    cJSON *result = cJSON_CreateObject();
    char details[96];
    snprintf(details, sizeof details,
             "{\"op\":\"task.comment_add\",\"comment_id\":%lld}",
             (long long)comment_id);
    if (comment == NULL || result == NULL) {
        if (comment != NULL) cJSON_Delete(comment);
        if (result != NULL) cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build comment response";
        return -1;
    }
    if (insert_task_event(db, task_id, "comment_added", req->actor,
                          req->request_id, "comment added", details, NULL,
                          NULL, "task", task_id) != 0) {
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

static int insert_optional_task_comment(sqlite3 *db,
                                        sqlite3_int64 task_id,
                                        const char *comment,
                                        const char *actor,
                                        sqlite3_int64 *comment_id_out) {
    *comment_id_out = 0;
    if (comment == NULL) return 0;
    return insert_comment(db, task_id, "closure_note_ref", comment, actor,
                          comment_id_out);
}

static int insert_task_relation(sqlite3 *db,
                                sqlite3_int64 from_task_id,
                                sqlite3_int64 to_task_id,
                                const char *relation_type,
                                const char *actor,
                                const char *notes,
                                sqlite3_int64 *relation_id_out) {
    const char *sql =
        "INSERT INTO task_relations("
        "from_task_id, to_task_id, relation_type, created_by, notes"
        ") VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, from_task_id);
    sqlite3_bind_int64(stmt, 2, to_task_id);
    sqlite3_bind_text(stmt, 3, relation_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, actor, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 5, notes);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    *relation_id_out = sqlite3_last_insert_rowid(db);
    return 0;
}

static int delete_task_relation(sqlite3 *db,
                                sqlite3_int64 from_task_id,
                                sqlite3_int64 to_task_id,
                                const char *relation_type,
                                int *deleted_out) {
    const char *sql =
        "DELETE FROM task_relations "
        "WHERE from_task_id = ? AND to_task_id = ? AND relation_type = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, from_task_id);
    sqlite3_bind_int64(stmt, 2, to_task_id);
    sqlite3_bind_text(stmt, 3, relation_type, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    *deleted_out = sqlite3_changes(db);
    return 0;
}

static int blocks_path_exists(sqlite3 *db,
                              sqlite3_int64 from_task_id,
                              sqlite3_int64 to_task_id,
                              int *exists_out) {
    const char *sql =
        "WITH RECURSIVE reachable(task_id) AS ("
        "  SELECT to_task_id FROM task_relations "
        "  WHERE from_task_id = ? AND relation_type = 'blocks'"
        "  UNION "
        "  SELECT tr.to_task_id FROM task_relations tr "
        "  JOIN reachable r ON tr.from_task_id = r.task_id "
        "  WHERE tr.relation_type = 'blocks'"
        ") "
        "SELECT 1 FROM reachable WHERE task_id = ? LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, from_task_id);
    sqlite3_bind_int64(stmt, 2, to_task_id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *exists_out = 1;
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        *exists_out = 0;
        return 0;
    }
    return -1;
}

static int insert_task_replacement_relation(sqlite3 *db,
                                            sqlite3_int64 old_task_id,
                                            sqlite3_int64 new_task_id,
                                            const char *actor,
                                            const char *notes,
                                            sqlite3_int64 *relation_id_out) {
    const char *sql =
        "INSERT INTO task_relations("
        "from_task_id, to_task_id, relation_type, created_by, notes"
        ") VALUES (?, ?, 'replaces', ?, ?);";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, old_task_id);
    sqlite3_bind_int64(stmt, 2, new_task_id);
    sqlite3_bind_text(stmt, 3, actor, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 4, notes);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    *relation_id_out = sqlite3_last_insert_rowid(db);
    return 0;
}

static int insert_task_closure_record(sqlite3 *db,
                                      sqlite3_int64 task_id,
                                      const char *closure_status,
                                      const char *resolution,
                                      const task_close_t *close_data,
                                      const char *actor,
                                      sqlite3_int64 event_id) {
    const char *sql =
        "INSERT INTO closure_records("
        "entity_type, entity_id, closure_status, resolution, outcome_summary, "
        "closing_comment, lessons_learned, open_items_summary, "
        "followup_needed, author, event_id, "
        "commit_sha, dirty, files_changed_json"
        ") VALUES ('task', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    char *files_changed_json = NULL;
    if (close_data->files_changed != NULL) {
        files_changed_json = cJSON_PrintUnformatted(close_data->files_changed);
        if (files_changed_json == NULL) return -1;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (files_changed_json != NULL) cJSON_free(files_changed_json);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, task_id);
    sqlite3_bind_text(stmt, 2, closure_status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, resolution, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, close_data->outcome_summary, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, close_data->closing_comment, -1, SQLITE_TRANSIENT);
    bind_optional_text(stmt, 6, close_data->lessons_learned);
    bind_optional_text(stmt, 7, close_data->open_items_summary);
    sqlite3_bind_int(stmt, 8, close_data->followup_needed);
    sqlite3_bind_text(stmt, 9, actor, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 10, event_id);
    bind_optional_text(stmt, 11, close_data->commit_sha);
    if (close_data->dirty_set) {
        sqlite3_bind_int(stmt, 12, close_data->dirty);
    } else {
        sqlite3_bind_null(stmt, 12);
    }
    bind_optional_text(stmt, 13, files_changed_json);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (files_changed_json != NULL) cJSON_free(files_changed_json);
    return rc == SQLITE_DONE ? 0 : -1;
}

const ipman_param_desc_t ipman_op_task_transition_params[] = {
    { "id" }, { "status" },
    { NULL },
};

int ipman_op_task_transition(const ipman_request_t *req, sqlite3 *db,
                            cJSON **result_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    const char *status = NULL;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_required_task_status(req->params, &status,
                                  err_code_out, err_msg_out) != 0) {
        return -1;
    }

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    cJSON *old_task = NULL;
    if (load_task_or_error(db, task_id, &old_task,
                           err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    const char *old_status = task_json_status(old_task);
    if (old_status == NULL ||
        !generic_transition_allowed(old_status, status)) {
        const char *message = task_transition_error_message(old_status, status);
        attach_task_transition_error_details(old_status, status);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = message;
        return -1;
    }
    if (update_task_transition_state(db, task_id, status) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to transition task";
        return -1;
    }
    if (validate_task_state_coherence(db, task_id,
                                      err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    int rc = finish_task_change_event(db, req, old_task, task_id,
                                      "task_status_changed",
                                      "task status changed",
                                      "{\"op\":\"task.transition\"}", NULL, 0,
                                      NULL, result_out,
                                      err_code_out, err_msg_out);
    cJSON_Delete(old_task);
    if (rc != 0) return -1;
    return commit_result_owned(db, result_out, err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_task_defer_params[] = {
    { "id" }, { "deferred_until" }, { "reason_code" }, { "reason_text" },
    { NULL },
};

int ipman_op_task_defer(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    const char *deferred_until = NULL;
    const char *reason_code = NULL;
    const char *reason_text = NULL;
    int has_deferred_until =
        cJSON_GetObjectItemCaseSensitive(req->params, "deferred_until") != NULL;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "deferred_until", &deferred_until,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "reason_code", &reason_code,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "reason_text", &reason_text,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (is_blank(reason_code) && is_blank(reason_text)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "task.defer requires reason_code or reason_text";
        return -1;
    }

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    cJSON *old_task = NULL;
    if (load_task_or_error(db, task_id, &old_task,
                           err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    const char *old_status = task_json_status(old_task);
    if (!has_deferred_until && old_status != NULL &&
        strcmp(old_status, "deferred") == 0) {
        deferred_until = task_json_string(old_task, "deferred_until");
    }
    if (old_status == NULL ||
        !transition_allowed_for_op(old_status, "deferred", "task.defer")) {
        const char *message =
            task_transition_error_message(old_status, "deferred");
        attach_task_transition_error_details(old_status, "deferred");
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = message;
        return -1;
    }
    if (update_task_deferred_state(db, task_id, deferred_until, reason_code,
                                   reason_text) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to defer task";
        return -1;
    }
    if (validate_task_state_coherence(db, task_id,
                                      err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    int rc = finish_task_change_event(db, req, old_task, task_id,
                                      "task_deferred", "task deferred",
                                      "{\"op\":\"task.defer\"}", NULL, 0,
                                      NULL, result_out,
                                      err_code_out, err_msg_out);
    cJSON_Delete(old_task);
    if (rc != 0) return -1;
    return commit_result_owned(db, result_out, err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_task_cancel_params[] = {
    { "id" }, { "resolution" },
    { "reason_code" }, { "reason_text" }, { "comment" },
    /* optional terminal-closure fields, read by read_task_terminal_closure: */
    { "closing_comment" }, { "outcome_summary" },
    { "lessons_learned" }, { "open_items_summary" }, { "followup_needed" },
    { NULL },
};

int ipman_op_task_cancel(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    const char *resolution = NULL;
    const char *reason_code = NULL;
    const char *reason_text = NULL;
    const char *comment = NULL;
    task_close_t close_data;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_required_task_resolution(req->params, &resolution,
                                      err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "reason_code", &reason_code,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "reason_text", &reason_text,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "comment", &comment,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (!is_cancel_resolution(resolution)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "task.cancel resolution must be canceled, not_planned, discarded, or duplicate";
        return -1;
    }
    if (comment != NULL && is_blank(comment)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "comment must be non-empty when provided";
        return -1;
    }
    if (read_task_terminal_closure(req->params, comment, &close_data,
                                   "closing_comment is required for task cancellation",
                                   err_code_out, err_msg_out) != 0) {
        return -1;
    }

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    cJSON *old_task = NULL;
    if (load_task_or_error(db, task_id, &old_task,
                           err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    const char *old_status = task_json_status(old_task);
    if (old_status == NULL ||
        !transition_allowed_for_op(old_status, "canceled", "task.cancel")) {
        const char *message =
            task_transition_error_message(old_status, "canceled");
        attach_task_transition_error_details(old_status, "canceled");
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = message;
        return -1;
    }
    sqlite3_int64 comment_id = 0;
    if (update_task_canceled_state(db, task_id, resolution, reason_code,
                                   reason_text) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to cancel task";
        return -1;
    }
    if (insert_optional_task_comment(db, task_id, comment, req->actor,
                                     &comment_id) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to add task comment";
        return -1;
    }
    if (ipman_context_repair_plan_cursor(db, req, task_json_plan_id(old_task),
                                        "{\"op\":\"task.cancel\"}",
                                        err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    if (validate_task_state_coherence(db, task_id,
                                      err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    char details[128];
    snprintf(details, sizeof details,
             "{\"op\":\"task.cancel\",\"comment_id\":%lld}",
             (long long)comment_id);
    sqlite3_int64 event_id = 0;
    int rc = finish_task_change_event(db, req, old_task, task_id,
                                      "task_canceled", "task canceled",
                                      details, NULL, 0, &event_id, result_out,
                                      err_code_out, err_msg_out);
    cJSON_Delete(old_task);
    if (rc != 0) return -1;
    if (insert_task_closure_record(db, task_id, "canceled", resolution,
                                   &close_data, req->actor, event_id) != 0) {
        cJSON_Delete(*result_out);
        *result_out = NULL;
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record task closure";
        return -1;
    }
    return commit_result_owned(db, result_out, err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_task_link_dependency_params[] = {
    { "id" }, { "target_task_id" }, { "relation_type" }, { "notes" },
    { NULL },
};

int ipman_op_task_link_dependency(const ipman_request_t *req, sqlite3 *db,
                                 cJSON **result_out,
                                 ipman_error_code_t *err_code_out,
                                 const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    sqlite3_int64 target_task_id = 0;
    const char *relation_type = NULL;
    const char *notes = NULL;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0 ||
        ipman_read_positive_id(req->params, "target_task_id", &target_task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_required_relation_type(req->params, &relation_type,
                                    err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "notes", &notes,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (task_id == target_task_id) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "task relation endpoints must be different";
        return -1;
    }

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    sqlite3_int64 plan_id = 0;
    sqlite3_int64 target_plan_id = 0;
    int rc = task_plan_id(db, task_id, &plan_id);
    if (rc == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "task not found";
        return -1;
    }
    if (rc < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to check task";
        return -1;
    }
    rc = task_plan_id(db, target_task_id, &target_plan_id);
    if (rc == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "target task not found";
        return -1;
    }
    if (rc < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to check target task";
        return -1;
    }
    if (plan_id != target_plan_id) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "target task belongs to a different plan";
        return -1;
    }

    sqlite3_int64 blocker_id = task_id;
    sqlite3_int64 blocked_id = target_task_id;
    if (strcmp(relation_type, "blocked_by") == 0) {
        blocker_id = target_task_id;
        blocked_id = task_id;
    }
    if (strcmp(relation_type, "blocks") == 0 ||
        strcmp(relation_type, "blocked_by") == 0) {
        int cycle = 0;
        if (blocks_path_exists(db, blocked_id, blocker_id, &cycle) != 0) {
            run_sql(db, "ROLLBACK;");
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to check dependency cycle";
            return -1;
        }
        if (cycle) {
            run_sql(db, "ROLLBACK;");
            *err_code_out = IPMAN_ERR_CONFLICT;
            *err_msg_out = "task dependency would create a cycle";
            return -1;
        }
    }

    sqlite3_int64 relation_id = 0;
    sqlite3_int64 reverse_relation_id = 0;
    const char *reverse_type = reverse_relation_type(relation_type);
    if (insert_task_relation(db, task_id, target_task_id, relation_type,
                             req->actor, notes, &relation_id) != 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "failed to create task relation";
        return -1;
    }
    if (reverse_type != NULL &&
        insert_task_relation(db, target_task_id, task_id, reverse_type,
                             req->actor, notes, &reverse_relation_id) != 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "failed to create reverse task relation";
        return -1;
    }

    cJSON *relation = load_task_relation(db, relation_id);
    cJSON *reverse_relation = NULL;
    if (reverse_relation_id > 0) {
        reverse_relation = load_task_relation(db, reverse_relation_id);
    }
    cJSON *result = cJSON_CreateObject();
    char *new_json = json_print_owned(relation);
    if (relation == NULL || result == NULL || new_json == NULL ||
        (reverse_relation_id > 0 && reverse_relation == NULL)) {
        if (relation != NULL) cJSON_Delete(relation);
        if (reverse_relation != NULL) cJSON_Delete(reverse_relation);
        if (result != NULL) cJSON_Delete(result);
        if (new_json != NULL) cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build relation response";
        return -1;
    }
    char details[192];
    snprintf(details, sizeof details,
             "{\"op\":\"task.link_dependency\",\"relation_type\":\"%s\","
             "\"relation_id\":%lld,\"reverse_relation_id\":%lld}",
             relation_type, (long long)relation_id,
             (long long)reverse_relation_id);
    if (insert_task_event(db, task_id, "task_updated", req->actor,
                          req->request_id, "task relation linked", details,
                          NULL, new_json, "task", target_task_id) != 0) {
        cJSON_Delete(relation);
        if (reverse_relation != NULL) cJSON_Delete(reverse_relation);
        cJSON_Delete(result);
        cJSON_free(new_json);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record relation event";
        return -1;
    }
    cJSON_free(new_json);
    cJSON_AddItemToObject(result, "relation", relation);
    if (reverse_relation == NULL) {
        cJSON_AddNullToObject(result, "reverse_relation");
    } else {
        cJSON_AddItemToObject(result, "reverse_relation", reverse_relation);
    }
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_task_unlink_dependency_params[] = {
    { "id" }, { "target_task_id" }, { "relation_type" },
    { NULL },
};

int ipman_op_task_unlink_dependency(const ipman_request_t *req, sqlite3 *db,
                                   cJSON **result_out,
                                   ipman_error_code_t *err_code_out,
                                   const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    sqlite3_int64 target_task_id = 0;
    const char *relation_type = NULL;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0 ||
        ipman_read_positive_id(req->params, "target_task_id", &target_task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_required_relation_type(req->params, &relation_type,
                                    err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (task_id == target_task_id) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "task relation endpoints must be different";
        return -1;
    }

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    sqlite3_int64 plan_id = 0;
    sqlite3_int64 target_plan_id = 0;
    int rc = task_plan_id(db, task_id, &plan_id);
    if (rc == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "task not found";
        return -1;
    }
    if (rc < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to check task";
        return -1;
    }
    rc = task_plan_id(db, target_task_id, &target_plan_id);
    if (rc == 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "target task not found";
        return -1;
    }
    if (rc < 0) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to check target task";
        return -1;
    }
    if (plan_id != target_plan_id) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "target task belongs to a different plan";
        return -1;
    }

    cJSON *relation = load_task_relation_by_key(db, task_id, target_task_id,
                                                relation_type);
    if (relation == NULL) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "task relation not found";
        return -1;
    }
    const char *reverse_type = reverse_relation_type(relation_type);
    cJSON *reverse_relation = NULL;
    if (reverse_type != NULL) {
        reverse_relation = load_task_relation_by_key(db, target_task_id,
                                                     task_id, reverse_type);
    }
    char *old_json = json_print_owned(relation);
    cJSON *result = cJSON_CreateObject();
    if (old_json == NULL || result == NULL) {
        if (old_json != NULL) cJSON_free(old_json);
        cJSON_Delete(relation);
        if (reverse_relation != NULL) cJSON_Delete(reverse_relation);
        if (result != NULL) cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build relation event";
        return -1;
    }

    int deleted = 0;
    if (delete_task_relation(db, task_id, target_task_id, relation_type,
                             &deleted) != 0) {
        cJSON_free(old_json);
        cJSON_Delete(relation);
        if (reverse_relation != NULL) cJSON_Delete(reverse_relation);
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to remove task relation";
        return -1;
    }
    if (deleted == 0) {
        cJSON_free(old_json);
        cJSON_Delete(relation);
        if (reverse_relation != NULL) cJSON_Delete(reverse_relation);
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_NOT_FOUND;
        *err_msg_out = "task relation not found";
        return -1;
    }
    int reverse_deleted = 0;
    if (reverse_type != NULL &&
        delete_task_relation(db, target_task_id, task_id, reverse_type,
                             &reverse_deleted) != 0) {
        cJSON_free(old_json);
        cJSON_Delete(relation);
        if (reverse_relation != NULL) cJSON_Delete(reverse_relation);
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to remove reverse task relation";
        return -1;
    }

    char details[192];
    snprintf(details, sizeof details,
             "{\"op\":\"task.unlink_dependency\",\"relation_type\":\"%s\","
             "\"reverse_deleted\":%d}",
             relation_type, reverse_deleted);
    if (insert_task_event(db, task_id, "task_updated", req->actor,
                          req->request_id, "task relation unlinked", details,
                          old_json, NULL, "task", target_task_id) != 0) {
        cJSON_free(old_json);
        cJSON_Delete(relation);
        if (reverse_relation != NULL) cJSON_Delete(reverse_relation);
        cJSON_Delete(result);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record relation event";
        return -1;
    }
    cJSON_free(old_json);
    cJSON_AddItemToObject(result, "relation", relation);
    if (reverse_relation == NULL) {
        cJSON_AddNullToObject(result, "reverse_relation");
    } else {
        cJSON_AddItemToObject(result, "reverse_relation", reverse_relation);
    }
    cJSON_AddBoolToObject(result, "deleted", 1);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_task_mark_duplicate_params[] = {
    { "id" }, { "target_task_id" },
    { "reason_code" }, { "reason_text" },
    { "comment" }, { "relation_notes" },
    { "closing_comment" }, { "outcome_summary" },
    { "lessons_learned" }, { "open_items_summary" }, { "followup_needed" },
    { NULL },
};

int ipman_op_task_mark_duplicate(const ipman_request_t *req, sqlite3 *db,
                                cJSON **result_out,
                                ipman_error_code_t *err_code_out,
                                const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    sqlite3_int64 target_task_id = 0;
    const char *reason_code = NULL;
    const char *reason_text = NULL;
    const char *comment = NULL;
    const char *relation_notes = NULL;
    task_close_t close_data;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0 ||
        ipman_read_positive_id(req->params, "target_task_id", &target_task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "reason_code", &reason_code,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "reason_text", &reason_text,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "comment", &comment,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "relation_notes", &relation_notes,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (task_id == target_task_id) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "task relation endpoints must be different";
        return -1;
    }
    if (comment != NULL && is_blank(comment)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "comment must be non-empty when provided";
        return -1;
    }
    if (read_task_terminal_closure(req->params, comment, &close_data,
                                   "closing_comment is required for duplicate marking",
                                   err_code_out, err_msg_out) != 0) {
        return -1;
    }

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    cJSON *old_task = NULL;
    if (load_task_or_error(db, task_id, &old_task,
                           err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    sqlite3_int64 plan_id = task_json_plan_id(old_task);
    if (validate_task_in_plan(db, target_task_id, plan_id,
                              "target task not found",
                              "target task belongs to a different plan",
                              err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    const char *old_status = task_json_status(old_task);
    if (old_status == NULL ||
        !transition_allowed_for_op(old_status, "canceled",
                                   "task.mark_duplicate")) {
        const char *message =
            task_transition_error_message(old_status, "canceled");
        attach_task_transition_error_details(old_status, "canceled");
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = message;
        return -1;
    }
    int dup_exists = task_relation_exists(db, task_id, target_task_id,
                                          "duplicates");
    if (dup_exists < 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to check duplicate relation";
        return -1;
    }
    if (dup_exists > 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "task is already marked as duplicate of the target task";
        return -1;
    }

    char *old_json = json_print_owned(old_task);
    sqlite3_int64 relation_id = 0;
    sqlite3_int64 comment_id = 0;
    if (old_json == NULL) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build task event";
        return -1;
    }
    if (update_task_canceled_state(db, task_id, "duplicate",
                                   reason_code, reason_text) != 0) {
        cJSON_free(old_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to mark task duplicate";
        return -1;
    }
    if (insert_task_relation(db, task_id, target_task_id, "duplicates",
                             req->actor, relation_notes, &relation_id) != 0) {
        cJSON_free(old_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "failed to create duplicate relation";
        return -1;
    }
    if (insert_optional_task_comment(db, task_id, comment, req->actor,
                                     &comment_id) != 0) {
        cJSON_free(old_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to add task comment";
        return -1;
    }
    if (ipman_context_repair_plan_cursor(db, req, task_json_plan_id(old_task),
                                        "{\"op\":\"task.mark_duplicate\"}",
                                        err_code_out, err_msg_out) != 0) {
        cJSON_free(old_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    if (validate_task_state_coherence(db, task_id,
                                      err_code_out, err_msg_out) != 0) {
        cJSON_free(old_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }

    cJSON *new_task = load_task(db, task_id);
    cJSON *relation = load_task_relation(db, relation_id);
    cJSON *result = cJSON_CreateObject();
    char *new_json = json_print_owned(new_task);
    if (new_task == NULL || relation == NULL || result == NULL ||
        new_json == NULL) {
        if (new_task != NULL) cJSON_Delete(new_task);
        if (relation != NULL) cJSON_Delete(relation);
        if (result != NULL) cJSON_Delete(result);
        if (new_json != NULL) cJSON_free(new_json);
        cJSON_free(old_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build duplicate response";
        return -1;
    }
    char details[176];
    snprintf(details, sizeof details,
             "{\"op\":\"task.mark_duplicate\",\"relation_id\":%lld,"
             "\"comment_id\":%lld}",
             (long long)relation_id, (long long)comment_id);
    if (insert_task_event(db, task_id, "task_canceled", req->actor,
                          req->request_id, "task marked duplicate", details,
                          old_json, new_json, "task", target_task_id) != 0) {
        cJSON_Delete(new_task);
        cJSON_Delete(relation);
        cJSON_Delete(result);
        cJSON_free(old_json);
        cJSON_free(new_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record duplicate event";
        return -1;
    }
    sqlite3_int64 event_id = sqlite3_last_insert_rowid(db);
    if (insert_task_closure_record(db, task_id, "canceled", "duplicate",
                                   &close_data, req->actor, event_id) != 0) {
        cJSON_Delete(new_task);
        cJSON_Delete(relation);
        cJSON_Delete(result);
        cJSON_free(old_json);
        cJSON_free(new_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record task closure";
        return -1;
    }
    cJSON_free(old_json);
    cJSON_free(new_json);
    cJSON_Delete(old_task);
    cJSON_AddItemToObject(result, "task", new_task);
    cJSON_AddItemToObject(result, "relation", relation);
    cJSON_AddNumberToObject(result, "comment_id", (double)comment_id);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_task_replace_params[] = {
    { "id" }, { "phase_id" }, { "parent_task_id" },
    { "title" }, { "summary" }, { "description" },
    { "priority" }, { "task_type" }, { "assignee" },
    { "due_date" }, { "target_start_date" }, { "estimate" },
    { "reason_code" }, { "reason_text" },
    { "comment" }, { "relation_notes" },
    { "closing_comment" }, { "outcome_summary" },
    { "lessons_learned" }, { "open_items_summary" }, { "followup_needed" },
    { NULL },
};

int ipman_op_task_replace(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out) {
    sqlite3_int64 old_task_id = 0;
    int phase_present = 0;
    sqlite3_int64 phase_id = 0;
    int parent_present = 0;
    sqlite3_int64 parent_task_id = 0;
    const char *title = NULL;
    const char *summary = NULL;
    const char *description = NULL;
    const char *priority = NULL;
    const char *task_type = NULL;
    const char *assignee = NULL;
    const char *due_date = NULL;
    const char *target_start_date = NULL;
    const char *estimate = NULL;
    const char *reason_code = NULL;
    const char *reason_text = NULL;
    const char *comment = NULL;
    const char *relation_notes = NULL;
    task_close_t close_data;

    if (ipman_read_task_selector(req->params, db, &old_task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_nullable_id_update(req->params, "phase_id", &phase_present,
                                &phase_id, err_code_out, err_msg_out) != 0 ||
        read_nullable_id_update(req->params, "parent_task_id",
                                &parent_present, &parent_task_id,
                                err_code_out, err_msg_out) != 0 ||
        read_required_title(req->params, &title, err_code_out,
                            err_msg_out) != 0 ||
        read_optional_priority(req->params, &priority,
                               err_code_out, err_msg_out) != 0 ||
        read_optional_task_type(req->params, &task_type,
                                err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "summary", &summary,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "description", &description,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "assignee", &assignee,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "due_date", &due_date,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "target_start_date",
                             &target_start_date, err_code_out,
                             err_msg_out) != 0 ||
        read_optional_string(req->params, "estimate", &estimate,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "reason_code", &reason_code,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "reason_text", &reason_text,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "comment", &comment,
                             err_code_out, err_msg_out) != 0 ||
        read_optional_string(req->params, "relation_notes", &relation_notes,
                             err_code_out, err_msg_out) != 0) {
        return -1;
    }
    if (comment != NULL && is_blank(comment)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "comment must be non-empty when provided";
        return -1;
    }
    if (read_task_terminal_closure(req->params, comment, &close_data,
                                   "closing_comment is required for task replacement",
                                   err_code_out, err_msg_out) != 0) {
        return -1;
    }

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    cJSON *old_task = NULL;
    if (load_task_or_error(db, old_task_id, &old_task,
                           err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    const char *old_status = task_json_status(old_task);
    if (old_status == NULL ||
        !transition_allowed_for_op(old_status, "canceled", "task.replace")) {
        const char *message =
            task_transition_error_message(old_status, "canceled");
        attach_task_transition_error_details(old_status, "canceled");
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = message;
        return -1;
    }
    sqlite3_int64 plan_id = task_json_plan_id(old_task);
    if (!phase_present) {
        phase_id = task_json_nullable_id(old_task, "phase_id");
    }
    if (!parent_present) {
        parent_task_id = task_json_nullable_id(old_task, "parent_task_id");
    }
    if (phase_id > 0 &&
        validate_phase_in_plan(db, phase_id, plan_id,
                               err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    if (parent_task_id > 0 &&
        validate_task_in_plan(db, parent_task_id, plan_id,
                              "parent task not found",
                              "parent task belongs to a different plan",
                              err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    if (parent_task_id > 0 && parent_task_id == old_task_id) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "replacement task cannot be a child of the task being replaced";
        return -1;
    }
    int replaces_count = task_replaces_relation_count(db, old_task_id);
    if (replaces_count < 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to check replacement relation";
        return -1;
    }
    if (replaces_count > 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "task already has a replacement relation";
        return -1;
    }

    char *old_json = json_print_owned(old_task);
    sqlite3_int64 new_task_id = 0;
    sqlite3_int64 relation_id = 0;
    sqlite3_int64 comment_id = 0;
    if (old_json == NULL) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build task event";
        return -1;
    }
    if (update_task_canceled_state(db, old_task_id, "replaced",
                                   reason_code, reason_text) != 0) {
        cJSON_free(old_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to replace task";
        return -1;
    }
    if (insert_task(db, plan_id, phase_id > 0, phase_id,
                    parent_task_id > 0, parent_task_id, title, summary,
                    description, "todo", priority, task_type, "replacement",
                    assignee, due_date, target_start_date, estimate, NULL,
                    NULL, 1, old_task_id, NULL, &new_task_id) != 0) {
        cJSON_free(old_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "failed to create replacement task";
        return -1;
    }
    if (insert_task_replacement_relation(db, old_task_id, new_task_id,
                                         req->actor, relation_notes,
                                         &relation_id) != 0) {
        cJSON_free(old_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "failed to create replacement relation";
        return -1;
    }
    if (insert_optional_task_comment(db, old_task_id, comment, req->actor,
                                     &comment_id) != 0) {
        cJSON_free(old_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to add task comment";
        return -1;
    }
    if (ipman_context_repair_plan_cursor(db, req, task_json_plan_id(old_task),
                                        "{\"op\":\"task.replace\"}",
                                        err_code_out, err_msg_out) != 0) {
        cJSON_free(old_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    if (validate_task_state_coherence(db, old_task_id,
                                      err_code_out, err_msg_out) != 0 ||
        validate_task_state_coherence(db, new_task_id,
                                      err_code_out, err_msg_out) != 0) {
        cJSON_free(old_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }

    cJSON *old_after = load_task(db, old_task_id);
    cJSON *new_task = load_task(db, new_task_id);
    char *old_after_json = json_print_owned(old_after);
    char *new_json = json_print_owned(new_task);
    cJSON *result = cJSON_CreateObject();
    if (old_after == NULL || new_task == NULL || old_after_json == NULL ||
        new_json == NULL || result == NULL) {
        if (old_after != NULL) cJSON_Delete(old_after);
        if (new_task != NULL) cJSON_Delete(new_task);
        if (old_after_json != NULL) cJSON_free(old_after_json);
        if (new_json != NULL) cJSON_free(new_json);
        if (result != NULL) cJSON_Delete(result);
        cJSON_free(old_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build replacement response";
        return -1;
    }
    char replace_details[160];
    snprintf(replace_details, sizeof replace_details,
             "{\"op\":\"task.replace\",\"relation_id\":%lld,\"comment_id\":%lld}",
             (long long)relation_id, (long long)comment_id);
    sqlite3_int64 event_id = 0;
    if (insert_task_event_with_id(db, old_task_id, "task_replaced", req->actor,
                                  req->request_id, "task replaced",
                                  replace_details, old_json, old_after_json,
                                  "task", new_task_id, &event_id) != 0 ||
        insert_task_event(db, new_task_id, "task_created", req->actor,
                          req->request_id, "task created",
                          "{\"op\":\"task.replace\"}", NULL, new_json,
                          "task", old_task_id) != 0) {
        cJSON_Delete(old_after);
        cJSON_Delete(new_task);
        cJSON_Delete(result);
        cJSON_free(old_json);
        cJSON_free(old_after_json);
        cJSON_free(new_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record replacement events";
        return -1;
    }
    if (insert_task_closure_record(db, old_task_id, "canceled", "replaced",
                                   &close_data, req->actor, event_id) != 0) {
        cJSON_Delete(old_after);
        cJSON_Delete(new_task);
        cJSON_Delete(result);
        cJSON_free(old_json);
        cJSON_free(old_after_json);
        cJSON_free(new_json);
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record task closure";
        return -1;
    }
    cJSON_free(old_json);
    cJSON_free(old_after_json);
    cJSON_free(new_json);
    cJSON_Delete(old_task);
    cJSON_AddItemToObject(result, "old_task", old_after);
    cJSON_AddItemToObject(result, "new_task", new_task);
    cJSON_AddNumberToObject(result, "relation_id", (double)relation_id);
    if (commit_result_owned(db, &result, err_code_out, err_msg_out) != 0) {
        return -1;
    }
    *result_out = result;
    return 0;
}

const ipman_param_desc_t ipman_op_task_close_params[] = {
    { "id" }, { "outcome_summary" }, { "closing_comment" },
    { "lessons_learned" }, { "open_items_summary" }, { "followup_needed" },
    { "commit_sha" }, { "dirty" }, { "files_changed" },
    { NULL },
};

int ipman_op_task_close(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out) {
    sqlite3_int64 task_id = 0;
    task_close_t close_data;
    if (ipman_read_task_selector(req->params, db, &task_id,
                         err_code_out, err_msg_out) != 0 ||
        read_task_close(req->params, &close_data,
                        err_code_out, err_msg_out) != 0) {
        return -1;
    }

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }
    cJSON *old_task = NULL;
    if (load_task_or_error(db, task_id, &old_task,
                           err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    const char *old_status = task_json_status(old_task);
    const char *old_resolution = task_json_resolution(old_task);
    if (old_status == NULL ||
        !transition_allowed_for_op(old_status, "done", "task.close") ||
        old_resolution != NULL) {
        int transition_rejected =
            (old_status == NULL ||
             !transition_allowed_for_op(old_status, "done", "task.close"));
        const char *message = old_status != NULL &&
                              strcmp(old_status, "done") == 0
            ? task_transition_error_message(old_status, "done")
            : (old_resolution != NULL && !transition_rejected
                ? "Task already has a resolution"
                : task_transition_error_message(old_status, "done"));
        if (transition_rejected) {
            attach_task_transition_error_details(old_status, "done");
        }
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = message;
        return -1;
    }
    if (update_task_closed_state(db, task_id) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to close task";
        return -1;
    }
    if (ipman_context_repair_plan_cursor(db, req, task_json_plan_id(old_task),
                                        "{\"op\":\"task.close\"}",
                                        err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    if (validate_task_state_coherence(db, task_id,
                                      err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    sqlite3_int64 event_id = 0;
    int rc = finish_task_change_event(db, req, old_task, task_id,
                                      "task_closed", "task closed",
                                      "{\"op\":\"task.close\"}", NULL, 0,
                                      &event_id, result_out,
                                      err_code_out, err_msg_out);
    cJSON_Delete(old_task);
    if (rc != 0) return -1;
    if (insert_task_closure_record(db, task_id, "done", "completed",
                                   &close_data, req->actor, event_id) != 0) {
        cJSON_Delete(*result_out);
        *result_out = NULL;
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to record task closure";
        return -1;
    }
    return commit_result_owned(db, result_out, err_code_out, err_msg_out);
}

const ipman_param_desc_t ipman_op_task_reopen_params[] = {
    { "id" },
    { NULL },
};

int ipman_op_task_reopen(const ipman_request_t *req, sqlite3 *db,
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
    cJSON *old_task = NULL;
    if (load_task_or_error(db, task_id, &old_task,
                           err_code_out, err_msg_out) != 0) {
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    const char *old_status = task_json_status(old_task);
    const char *old_resolution = task_json_resolution(old_task);
    if (old_status == NULL ||
        !transition_allowed_for_op(old_status, "todo", "task.reopen")) {
        const char *message = task_transition_error_message(old_status, "todo");
        attach_task_transition_error_details(old_status, "todo");
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = message;
        return -1;
    }
    if (old_resolution != NULL && strcmp(old_resolution, "replaced") == 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_CONFLICT;
        *err_msg_out = "replaced tasks cannot be reopened";
        return -1;
    }
    sqlite3_int64 phase_id = task_json_nullable_id(old_task, "phase_id");
    if (phase_id > 0) {
        char phase_status[32] = {0};
        int found = load_phase_status(db, phase_id, phase_status,
                                      sizeof phase_status);
        if (found <= 0) {
            cJSON_Delete(old_task);
            run_sql(db, "ROLLBACK;");
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to load task phase";
            return -1;
        }
        if (strcmp(phase_status, "completed") == 0 ||
            strcmp(phase_status, "canceled") == 0) {
            attach_phase_terminal_error_details(phase_id, phase_status);
            cJSON_Delete(old_task);
            run_sql(db, "ROLLBACK;");
            *err_code_out = IPMAN_ERR_CONFLICT;
            *err_msg_out = "cannot reopen a task in a terminal phase";
            return -1;
        }
    }
    if (update_task_reopened_state(db, task_id) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to reopen task";
        return -1;
    }
    if (validate_task_state_coherence(db, task_id,
                                      err_code_out, err_msg_out) != 0) {
        cJSON_Delete(old_task);
        run_sql(db, "ROLLBACK;");
        return -1;
    }
    int rc = finish_task_change_event(db, req, old_task, task_id,
                                      "task_reopened", "task reopened",
                                      "{\"op\":\"task.reopen\"}", NULL, 0,
                                      NULL, result_out,
                                      err_code_out, err_msg_out);
    cJSON_Delete(old_task);
    if (rc != 0) return -1;
    return commit_result_owned(db, result_out, err_code_out, err_msg_out);
}
