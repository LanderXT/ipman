#ifndef IPMAN_PLAN_OPS_H
#define IPMAN_PLAN_OPS_H

#include "dispatch.h"

int ipman_op_plan_create(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out);
int ipman_op_plan_get(const ipman_request_t *req, sqlite3 *db,
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
extern const ipman_param_desc_t ipman_op_plan_update_params[];
extern const ipman_param_desc_t ipman_op_plan_list_params[];
extern const ipman_param_desc_t ipman_op_plan_close_params[];
extern const ipman_param_desc_t ipman_op_plan_archive_params[];
extern const ipman_param_desc_t ipman_op_plan_reopen_params[];
extern const ipman_param_desc_t ipman_op_plan_history_params[];
extern const ipman_param_desc_t ipman_op_plan_progress_params[];

#endif
