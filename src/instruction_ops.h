#ifndef IPMAN_INSTRUCTION_OPS_H
#define IPMAN_INSTRUCTION_OPS_H

#include "dispatch.h"

int ipman_op_instruction_add(const ipman_request_t *req, sqlite3 *db,
                            cJSON **result_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out);
int ipman_op_instruction_list(const ipman_request_t *req, sqlite3 *db,
                             cJSON **result_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out);
int ipman_op_instruction_update(const ipman_request_t *req, sqlite3 *db,
                               cJSON **result_out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out);
int ipman_op_instruction_invalidate(const ipman_request_t *req, sqlite3 *db,
                                   cJSON **result_out,
                                   ipman_error_code_t *err_code_out,
                                   const char **err_msg_out);

extern const ipman_param_desc_t ipman_op_instruction_add_params[];
extern const ipman_param_desc_t ipman_op_instruction_list_params[];
extern const ipman_param_desc_t ipman_op_instruction_update_params[];
extern const ipman_param_desc_t ipman_op_instruction_invalidate_params[];

#endif
