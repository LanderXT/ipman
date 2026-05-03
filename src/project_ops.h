/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_PROJECT_OPS_H
#define IPMAN_PROJECT_OPS_H

#include "dispatch.h"

int ipman_op_project_get(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out);
int ipman_op_project_update(const ipman_request_t *req, sqlite3 *db,
                            cJSON **result_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out);
int ipman_op_project_history(const ipman_request_t *req, sqlite3 *db,
                             cJSON **result_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out);

extern const ipman_param_desc_t ipman_op_project_get_params[];
extern const ipman_param_desc_t ipman_op_project_update_params[];
extern const ipman_param_desc_t ipman_op_project_history_params[];

/*
 * Build a JSON object for the single project row, with id/name/description
 * and timestamps. Returns a newly allocated cJSON object owned by the
 * caller, or NULL if the row is missing or allocation fails. Used by
 * workspace.context_get to surface project metadata as part of the
 * context payload.
 */
cJSON *ipman_project_load(sqlite3 *db);

#endif
