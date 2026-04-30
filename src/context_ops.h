/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_CONTEXT_OPS_H
#define IPMAN_CONTEXT_OPS_H

#include "dispatch.h"

int ipman_op_workspace_context_get(const ipman_request_t *req, sqlite3 *db,
                                  cJSON **result_out,
                                  ipman_error_code_t *err_code_out,
                                  const char **err_msg_out);
int ipman_op_plan_activate(const ipman_request_t *req, sqlite3 *db,
                          cJSON **result_out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out);
int ipman_op_plan_deactivate(const ipman_request_t *req, sqlite3 *db,
                            cJSON **result_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out);
int ipman_op_phase_set_current(const ipman_request_t *req, sqlite3 *db,
                              cJSON **result_out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out);
int ipman_op_phase_clear_current(const ipman_request_t *req, sqlite3 *db,
                                cJSON **result_out,
                                ipman_error_code_t *err_code_out,
                                const char **err_msg_out);
int ipman_op_task_set_current(const ipman_request_t *req, sqlite3 *db,
                             cJSON **result_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out);
int ipman_op_task_clear_current(const ipman_request_t *req, sqlite3 *db,
                               cJSON **result_out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out);
int ipman_context_repair_plan_cursor(sqlite3 *db,
                                    const ipman_request_t *req,
                                    sqlite3_int64 plan_id,
                                    const char *details,
                                    ipman_error_code_t *err_code_out,
                                    const char **err_msg_out);

extern const ipman_param_desc_t ipman_op_workspace_context_get_params[];
extern const ipman_param_desc_t ipman_op_plan_activate_params[];
extern const ipman_param_desc_t ipman_op_plan_deactivate_params[];
extern const ipman_param_desc_t ipman_op_phase_set_current_params[];
extern const ipman_param_desc_t ipman_op_phase_clear_current_params[];
extern const ipman_param_desc_t ipman_op_task_set_current_params[];
extern const ipman_param_desc_t ipman_op_task_clear_current_params[];

#endif
