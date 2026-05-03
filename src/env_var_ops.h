/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_ENV_VAR_OPS_H
#define IPMAN_ENV_VAR_OPS_H

#include "dispatch.h"

int ipman_op_env_var_add(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out);
int ipman_op_env_var_list(const ipman_request_t *req, sqlite3 *db,
                          cJSON **result_out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out);
int ipman_op_env_var_update(const ipman_request_t *req, sqlite3 *db,
                            cJSON **result_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out);
int ipman_op_env_var_invalidate(const ipman_request_t *req, sqlite3 *db,
                                cJSON **result_out,
                                ipman_error_code_t *err_code_out,
                                const char **err_msg_out);

extern const ipman_param_desc_t ipman_op_env_var_add_params[];
extern const ipman_param_desc_t ipman_op_env_var_list_params[];
extern const ipman_param_desc_t ipman_op_env_var_update_params[];
extern const ipman_param_desc_t ipman_op_env_var_invalidate_params[];

/*
 * Build a "row" cJSON object from a SELECT statement that produced the
 * 12 columns of the env_vars table in the canonical order used internally.
 * Exposed so the workspace.context_get aggregator can reuse the renderer
 * without re-deriving the column→field mapping.
 *
 * `reveal` controls whether the `example` field is masked when sensitive=1.
 * When `reveal == 0` and the row's sensitive flag is set, the returned
 * object replaces the example with the literal sentinel "[sensitive]"
 * (or omits the field entirely when no example was stored).
 *
 * Returns a newly allocated cJSON object owned by the caller, or NULL on
 * allocation failure.
 */
cJSON *ipman_env_var_row_to_json(sqlite3_stmt *stmt, int reveal);

#endif
