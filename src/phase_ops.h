/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_PHASE_OPS_H
#define IPMAN_PHASE_OPS_H

#include "dispatch.h"

cJSON *ipman_phase_from_row(sqlite3_stmt *stmt);

/*
 * Resolve the phase identifier from a request's params, accepting
 * uid, id, or label+scope. Reports validation_failed on bad input
 * and not_found if no matching phase exists.
 */
int ipman_read_phase_selector(cJSON *params, sqlite3 *db,
                             sqlite3_int64 *phase_id_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out);

int ipman_op_phase_create(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out);
int ipman_op_phase_get(const ipman_request_t *req, sqlite3 *db,
                      cJSON **result_out,
                      ipman_error_code_t *err_code_out,
                      const char **err_msg_out);
int ipman_op_phase_update(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out);
int ipman_op_phase_move(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out);
int ipman_op_phase_close(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out);
int ipman_op_phase_reopen(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out);
int ipman_op_phase_list_tasks(const ipman_request_t *req, sqlite3 *db,
                             cJSON **result_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out);
int ipman_op_phase_list(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out);
int ipman_op_phase_history(const ipman_request_t *req, sqlite3 *db,
                          cJSON **result_out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out);
int ipman_op_phase_progress(const ipman_request_t *req, sqlite3 *db,
                           cJSON **result_out,
                           ipman_error_code_t *err_code_out,
                           const char **err_msg_out);

extern const ipman_param_desc_t ipman_op_phase_create_params[];
extern const ipman_param_desc_t ipman_op_phase_get_params[];
extern const ipman_param_desc_t ipman_op_phase_update_params[];
extern const ipman_param_desc_t ipman_op_phase_move_params[];
extern const ipman_param_desc_t ipman_op_phase_close_params[];
extern const ipman_param_desc_t ipman_op_phase_reopen_params[];
extern const ipman_param_desc_t ipman_op_phase_list_tasks_params[];
extern const ipman_param_desc_t ipman_op_phase_list_params[];
extern const ipman_param_desc_t ipman_op_phase_history_params[];
extern const ipman_param_desc_t ipman_op_phase_progress_params[];

#endif
