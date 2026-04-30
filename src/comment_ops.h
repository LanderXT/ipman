/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_COMMENT_OPS_H
#define IPMAN_COMMENT_OPS_H

#include "dispatch.h"

int ipman_op_comment_add(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out);
int ipman_op_comment_list(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out);
int ipman_op_comment_update(const ipman_request_t *req, sqlite3 *db,
                           cJSON **result_out,
                           ipman_error_code_t *err_code_out,
                           const char **err_msg_out);
int ipman_op_comment_invalidate(const ipman_request_t *req, sqlite3 *db,
                               cJSON **result_out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out);
int ipman_op_closure_get(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out);
int ipman_op_plan_comment_add(const ipman_request_t *req, sqlite3 *db,
                             cJSON **result_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out);
int ipman_op_phase_comment_add(const ipman_request_t *req, sqlite3 *db,
                              cJSON **result_out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out);

extern const ipman_param_desc_t ipman_op_comment_add_params[];
extern const ipman_param_desc_t ipman_op_comment_list_params[];
extern const ipman_param_desc_t ipman_op_comment_update_params[];
extern const ipman_param_desc_t ipman_op_comment_invalidate_params[];
extern const ipman_param_desc_t ipman_op_closure_get_params[];
extern const ipman_param_desc_t ipman_op_plan_comment_add_params[];
extern const ipman_param_desc_t ipman_op_phase_comment_add_params[];

#endif
