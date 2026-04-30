/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "dispatch.h"

#include "agent_docs.h"
#include "phase_ops.h"
#include "plan_ops.h"
#include "comment_ops.h"
#include "context_ops.h"
#include "event_ops.h"
#include "export_ops.h"
#include "instruction_ops.h"
#include "task_ops.h"

#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Handlers                                                           */
/* ------------------------------------------------------------------ */

static const ipman_param_desc_t k_noop_params[] = { { NULL } };

static int op_noop(const ipman_request_t *req, sqlite3 *db,
                   cJSON **result_out,
                   ipman_error_code_t *err_code_out,
                   const char **err_msg_out) {
    (void)req;
    (void)db;
    (void)err_code_out;
    (void)err_msg_out;
    *result_out = cJSON_CreateObject();
    if (*result_out == NULL) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registry                                                           */
/* ------------------------------------------------------------------ */

static const ipman_op_t k_ops[] = {
    { "noop",                         op_noop,                                 k_noop_params },
    { "closure.get",                  ipman_op_closure_get,                     ipman_op_closure_get_params },
    { "comment.add",                  ipman_op_comment_add,                     ipman_op_comment_add_params },
    { "comment.invalidate",           ipman_op_comment_invalidate,              ipman_op_comment_invalidate_params },
    { "comment.list",                 ipman_op_comment_list,                    ipman_op_comment_list_params },
    { "comment.update",               ipman_op_comment_update,                  ipman_op_comment_update_params },
    { "event.list",                   ipman_op_event_list,                      ipman_op_event_list_params },
    { "instruction.add",              ipman_op_instruction_add,                 ipman_op_instruction_add_params },
    { "instruction.invalidate",       ipman_op_instruction_invalidate,          ipman_op_instruction_invalidate_params },
    { "instruction.list",             ipman_op_instruction_list,                ipman_op_instruction_list_params },
    { "instruction.update",           ipman_op_instruction_update,              ipman_op_instruction_update_params },
    { "phase.clear_current",          ipman_op_phase_clear_current,             ipman_op_phase_clear_current_params },
    { "phase.close",                  ipman_op_phase_close,                     ipman_op_phase_close_params },
    { "phase.comment_add",            ipman_op_phase_comment_add,               ipman_op_phase_comment_add_params },
    { "phase.create",                 ipman_op_phase_create,                    ipman_op_phase_create_params },
    { "phase.get",                    ipman_op_phase_get,                       ipman_op_phase_get_params },
    { "phase.history",                ipman_op_phase_history,                   ipman_op_phase_history_params },
    { "phase.list",                   ipman_op_phase_list,                      ipman_op_phase_list_params },
    { "phase.list_tasks",             ipman_op_phase_list_tasks,                ipman_op_phase_list_tasks_params },
    { "phase.lookup",                 ipman_op_phase_lookup,                    ipman_op_phase_lookup_params },
    { "phase.move",                   ipman_op_phase_move,                      ipman_op_phase_move_params },
    { "phase.progress",               ipman_op_phase_progress,                  ipman_op_phase_progress_params },
    { "phase.reopen",                 ipman_op_phase_reopen,                    ipman_op_phase_reopen_params },
    { "phase.set_current",            ipman_op_phase_set_current,               ipman_op_phase_set_current_params },
    { "phase.update",                 ipman_op_phase_update,                    ipman_op_phase_update_params },
    { "plan.activate",                ipman_op_plan_activate,                   ipman_op_plan_activate_params },
    { "plan.archive",                 ipman_op_plan_archive,                    ipman_op_plan_archive_params },
    { "plan.close",                   ipman_op_plan_close,                      ipman_op_plan_close_params },
    { "plan.comment_add",             ipman_op_plan_comment_add,                ipman_op_plan_comment_add_params },
    { "plan.create",                  ipman_op_plan_create,                     ipman_op_plan_create_params },
    { "plan.deactivate",              ipman_op_plan_deactivate,                 ipman_op_plan_deactivate_params },
    { "plan.export",                  ipman_op_plan_export,                     ipman_op_plan_export_params },
    { "plan.get",                     ipman_op_plan_get,                        ipman_op_plan_get_params },
    { "plan.history",                 ipman_op_plan_history,                    ipman_op_plan_history_params },
    { "plan.list",                    ipman_op_plan_list,                       ipman_op_plan_list_params },
    { "plan.lookup",                  ipman_op_plan_lookup,                     ipman_op_plan_lookup_params },
    { "plan.progress",                ipman_op_plan_progress,                   ipman_op_plan_progress_params },
    { "plan.reopen",                  ipman_op_plan_reopen,                     ipman_op_plan_reopen_params },
    { "plan.update",                  ipman_op_plan_update,                     ipman_op_plan_update_params },
    { "task.assign",                  ipman_op_task_assign,                     ipman_op_task_assign_params },
    { "task.cancel",                  ipman_op_task_cancel,                     ipman_op_task_cancel_params },
    { "task.clear_current",           ipman_op_task_clear_current,              ipman_op_task_clear_current_params },
    { "task.close",                   ipman_op_task_close,                      ipman_op_task_close_params },
    { "task.comment_add",             ipman_op_task_comment_add,                ipman_op_task_comment_add_params },
    { "task.create",                  ipman_op_task_create,                     ipman_op_task_create_params },
    { "task.defer",                   ipman_op_task_defer,                      ipman_op_task_defer_params },
    { "task.get",                     ipman_op_task_get,                        ipman_op_task_get_params },
    { "task.link_dependency",         ipman_op_task_link_dependency,            ipman_op_task_link_dependency_params },
    { "task.link_external",           ipman_op_task_link_external,              ipman_op_task_link_external_params },
    { "task.list",                    ipman_op_task_list,                       ipman_op_task_list_params },
    { "task.lookup",                  ipman_op_task_lookup,                     ipman_op_task_lookup_params },
    { "task.mark_duplicate",          ipman_op_task_mark_duplicate,             ipman_op_task_mark_duplicate_params },
    { "task.move",                    ipman_op_task_move,                       ipman_op_task_move_params },
    { "task.reopen",                  ipman_op_task_reopen,                     ipman_op_task_reopen_params },
    { "task.replace",                 ipman_op_task_replace,                    ipman_op_task_replace_params },
    { "task.set_current",             ipman_op_task_set_current,                ipman_op_task_set_current_params },
    { "task.set_origin",              ipman_op_task_set_origin,                 ipman_op_task_set_origin_params },
    { "task.set_priority",            ipman_op_task_set_priority,               ipman_op_task_set_priority_params },
    { "task.set_type",                ipman_op_task_set_type,                   ipman_op_task_set_type_params },
    { "task.transition",              ipman_op_task_transition,                 ipman_op_task_transition_params },
    { "task.unassign",                ipman_op_task_unassign,                   ipman_op_task_unassign_params },
    { "task.unlink_dependency",       ipman_op_task_unlink_dependency,          ipman_op_task_unlink_dependency_params },
    { "task.update",                  ipman_op_task_update,                     ipman_op_task_update_params },
    { "workspace.context_get",        ipman_op_workspace_context_get,           ipman_op_workspace_context_get_params },
    { "workspace.refresh_agent_docs", ipman_op_workspace_refresh_agent_docs,    ipman_op_workspace_refresh_agent_docs_params },
};

size_t ipman_dispatch_operation_count(void) {
    return sizeof k_ops / sizeof k_ops[0];
}

const char *ipman_dispatch_operation_name_at(size_t index) {
    if (index >= ipman_dispatch_operation_count()) return NULL;
    return k_ops[index].name;
}

const ipman_param_desc_t *ipman_dispatch_operation_params_at(size_t index) {
    if (index >= ipman_dispatch_operation_count()) return NULL;
    return k_ops[index].params;
}

static const ipman_op_t *find_op(const char *name) {
    size_t n = ipman_dispatch_operation_count();
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(k_ops[i].name, name) == 0) return &k_ops[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Parameter allowlist enforcement                                    */
/* ------------------------------------------------------------------ */

static int param_is_allowed(const char *key, const ipman_param_desc_t *allowed) {
    for (const ipman_param_desc_t *p = allowed; p->name != NULL; ++p) {
        if (strcmp(p->name, key) == 0) return 1;
    }
    return 0;
}

/*
 * Collect every key in `params` that is not listed in `allowed`. Returns 0
 * if all keys are accepted. Returns -1 on rejection and attaches a
 * `details` object with {kind, received, allowed}. Returns -2 on OOM.
 *
 * `params` can be NULL or non-object defensively, but the protocol parser
 * already guarantees it is an object; treat anything else as "no keys".
 */
static int validate_params(const cJSON *params,
                           const ipman_param_desc_t *allowed,
                           ipman_error_code_t *err_code_out,
                           const char **err_msg_out) {
    if (!cJSON_IsObject(params)) return 0;

    cJSON *unknown = cJSON_CreateArray();
    if (unknown == NULL) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to allocate validation buffer";
        return -2;
    }
    int unknown_count = 0;
    for (const cJSON *child = params->child; child != NULL; child = child->next) {
        if (child->string == NULL) continue;
        if (param_is_allowed(child->string, allowed)) continue;
        cJSON *name = cJSON_CreateString(child->string);
        if (name == NULL) {
            cJSON_Delete(unknown);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build validation details";
            return -2;
        }
        cJSON_AddItemToArray(unknown, name);
        ++unknown_count;
    }
    if (unknown_count == 0) {
        cJSON_Delete(unknown);
        return 0;
    }

    cJSON *allowed_arr = cJSON_CreateArray();
    cJSON *details = cJSON_CreateObject();
    if (allowed_arr == NULL || details == NULL) {
        cJSON_Delete(unknown);
        if (allowed_arr != NULL) cJSON_Delete(allowed_arr);
        if (details != NULL) cJSON_Delete(details);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build validation details";
        return -2;
    }
    for (const ipman_param_desc_t *p = allowed; p->name != NULL; ++p) {
        cJSON *name = cJSON_CreateString(p->name);
        if (name == NULL) {
            cJSON_Delete(unknown);
            cJSON_Delete(allowed_arr);
            cJSON_Delete(details);
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to build validation details";
            return -2;
        }
        cJSON_AddItemToArray(allowed_arr, name);
    }
    cJSON_AddStringToObject(details, "kind", "unknown_parameter");
    cJSON_AddItemToObject  (details, "received", unknown);
    cJSON_AddItemToObject  (details, "allowed",  allowed_arr);
    ipman_error_attach_details(details);

    *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
    *err_msg_out = "request contains unknown parameter(s)";
    return -1;
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                         */
/* ------------------------------------------------------------------ */

int ipman_dispatch(const ipman_request_t *req, sqlite3 *db, cJSON **response_out) {
    /* Drop any error details stashed by a prior call; defensive — the ipman
     * CLI runs one dispatch per process, but this keeps the invariant local. */
    cJSON *stale_details = ipman_error_take_details();
    if (stale_details != NULL) cJSON_Delete(stale_details);

    const ipman_op_t *op = find_op(req->op);
    if (op == NULL) {
        *response_out = ipman_response_err(req->request_id,
                                          IPMAN_ERR_UNKNOWN_OP,
                                          "no handler for this op",
                                          NULL);
        return -1;
    }

    ipman_error_code_t err_code = IPMAN_ERR_INTERNAL;
    const char       *err_msg  = "handler returned without setting error";

    int vrc = validate_params(req->params, op->params, &err_code, &err_msg);
    if (vrc != 0) {
        cJSON *details = ipman_error_take_details();
        *response_out = ipman_response_err(req->request_id, err_code, err_msg,
                                           details);
        return ipman_error_is_fatal(err_code) ? -2 : -1;
    }

    cJSON *result = NULL;
    int rc = op->handler(req, db, &result, &err_code, &err_msg);
    cJSON *details = ipman_error_take_details();
    if (rc == 0) {
        /* Successful handlers should not attach details; drop any by mistake. */
        if (details != NULL) cJSON_Delete(details);
        *response_out = ipman_response_ok(req->request_id, result);
        return 0;
    }
    /* Handler signaled failure. */
    if (result != NULL) {
        cJSON_Delete(result);
        result = NULL;
    }
    *response_out = ipman_response_err(req->request_id, err_code, err_msg, details);
    return ipman_error_is_fatal(err_code) ? -2 : -1;
}
