/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_TOOL_OPS_H
#define IPMAN_TOOL_OPS_H

#include "dispatch.h"

int ipman_op_tool_add(const ipman_request_t *req, sqlite3 *db,
                      cJSON **result_out,
                      ipman_error_code_t *err_code_out,
                      const char **err_msg_out);
int ipman_op_tool_list(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out);
int ipman_op_tool_update(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out);
int ipman_op_tool_invalidate(const ipman_request_t *req, sqlite3 *db,
                             cJSON **result_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out);

extern const ipman_param_desc_t ipman_op_tool_add_params[];
extern const ipman_param_desc_t ipman_op_tool_list_params[];
extern const ipman_param_desc_t ipman_op_tool_update_params[];
extern const ipman_param_desc_t ipman_op_tool_invalidate_params[];

#endif
