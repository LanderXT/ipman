/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_PLAN_OPS_H
#define IPMAN_PLAN_OPS_H

#include "dispatch.h"

/*
 * Resolve a plan scope from plan_id, plan_uid, or plan_label keys
 * (plan_id wins when valid, then plan_uid, then plan_label). Used by
 * the *.lookup ops to translate child-entity scopes to a plan_id.
 * Reports validation_failed if no scope key is provided and not_found
 * if the referenced plan does not exist.
 */
int ipman_resolve_plan_scope(cJSON *params, sqlite3 *db,
                             sqlite3_int64 *plan_id_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out);

int ipman_op_plan_create(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out);
int ipman_op_plan_get(const ipman_request_t *req, sqlite3 *db,
                     cJSON **result_out,
                     ipman_error_code_t *err_code_out,
                     const char **err_msg_out);
int ipman_op_plan_lookup(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out);
int ipman_op_plan_update(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out);
int ipman_op_plan_list(const ipman_request_t *req, sqlite3 *db,
                      cJSON **result_out,
                      ipman_error_code_t *err_code_out,
                      const char **err_msg_out);
int ipman_op_plan_close(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out);
int ipman_op_plan_archive(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out);
int ipman_op_plan_reopen(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out);
int ipman_op_plan_history(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out);
int ipman_op_plan_progress(const ipman_request_t *req, sqlite3 *db,
                          cJSON **result_out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out);

extern const ipman_param_desc_t ipman_op_plan_create_params[];
extern const ipman_param_desc_t ipman_op_plan_get_params[];
extern const ipman_param_desc_t ipman_op_plan_lookup_params[];
extern const ipman_param_desc_t ipman_op_plan_update_params[];
extern const ipman_param_desc_t ipman_op_plan_list_params[];
extern const ipman_param_desc_t ipman_op_plan_close_params[];
extern const ipman_param_desc_t ipman_op_plan_archive_params[];
extern const ipman_param_desc_t ipman_op_plan_reopen_params[];
extern const ipman_param_desc_t ipman_op_plan_history_params[];
extern const ipman_param_desc_t ipman_op_plan_progress_params[];

#endif
