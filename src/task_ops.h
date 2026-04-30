#ifndef IPMAN_TASK_OPS_H
#define IPMAN_TASK_OPS_H

#include "dispatch.h"

/*
 * Resolve the task identifier from a request's params, accepting
 * uid, id, or label+scope. Reports validation_failed on bad input
 * and not_found if no matching task exists.
 */
int ipman_read_task_selector(cJSON *params, sqlite3 *db,
                            sqlite3_int64 *task_id_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out);

cJSON *ipman_task_from_row(sqlite3_stmt *stmt);

int ipman_op_task_create(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out);
int ipman_op_task_get(const ipman_request_t *req, sqlite3 *db,
                     cJSON **result_out,
                     ipman_error_code_t *err_code_out,
                     const char **err_msg_out);
int ipman_op_task_update(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out);
int ipman_op_task_move(const ipman_request_t *req, sqlite3 *db,
                      cJSON **result_out,
                      ipman_error_code_t *err_code_out,
                      const char **err_msg_out);
int ipman_op_task_list(const ipman_request_t *req, sqlite3 *db,
                      cJSON **result_out,
                      ipman_error_code_t *err_code_out,
                      const char **err_msg_out);
int ipman_op_task_assign(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out);
int ipman_op_task_unassign(const ipman_request_t *req, sqlite3 *db,
                          cJSON **result_out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out);
int ipman_op_task_set_priority(const ipman_request_t *req, sqlite3 *db,
                              cJSON **result_out,
                              ipman_error_code_t *err_code_out,
                              const char **err_msg_out);
int ipman_op_task_set_type(const ipman_request_t *req, sqlite3 *db,
                          cJSON **result_out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out);
int ipman_op_task_set_origin(const ipman_request_t *req, sqlite3 *db,
                            cJSON **result_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out);
int ipman_op_task_link_external(const ipman_request_t *req, sqlite3 *db,
                               cJSON **result_out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out);
int ipman_op_task_comment_add(const ipman_request_t *req, sqlite3 *db,
                             cJSON **result_out,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out);
int ipman_op_task_transition(const ipman_request_t *req, sqlite3 *db,
                            cJSON **result_out,
                            ipman_error_code_t *err_code_out,
                            const char **err_msg_out);
int ipman_op_task_defer(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out);
int ipman_op_task_cancel(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out);
int ipman_op_task_link_dependency(const ipman_request_t *req, sqlite3 *db,
                                 cJSON **result_out,
                                 ipman_error_code_t *err_code_out,
                                 const char **err_msg_out);
int ipman_op_task_unlink_dependency(const ipman_request_t *req, sqlite3 *db,
                                   cJSON **result_out,
                                   ipman_error_code_t *err_code_out,
                                   const char **err_msg_out);
int ipman_op_task_mark_duplicate(const ipman_request_t *req, sqlite3 *db,
                                cJSON **result_out,
                                ipman_error_code_t *err_code_out,
                                const char **err_msg_out);
int ipman_op_task_replace(const ipman_request_t *req, sqlite3 *db,
                         cJSON **result_out,
                         ipman_error_code_t *err_code_out,
                         const char **err_msg_out);
int ipman_op_task_close(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out);
int ipman_op_task_reopen(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out);

extern const ipman_param_desc_t ipman_op_task_create_params[];
extern const ipman_param_desc_t ipman_op_task_get_params[];
extern const ipman_param_desc_t ipman_op_task_update_params[];
extern const ipman_param_desc_t ipman_op_task_move_params[];
extern const ipman_param_desc_t ipman_op_task_list_params[];
extern const ipman_param_desc_t ipman_op_task_assign_params[];
extern const ipman_param_desc_t ipman_op_task_unassign_params[];
extern const ipman_param_desc_t ipman_op_task_set_priority_params[];
extern const ipman_param_desc_t ipman_op_task_set_type_params[];
extern const ipman_param_desc_t ipman_op_task_set_origin_params[];
extern const ipman_param_desc_t ipman_op_task_link_external_params[];
extern const ipman_param_desc_t ipman_op_task_comment_add_params[];
extern const ipman_param_desc_t ipman_op_task_transition_params[];
extern const ipman_param_desc_t ipman_op_task_defer_params[];
extern const ipman_param_desc_t ipman_op_task_cancel_params[];
extern const ipman_param_desc_t ipman_op_task_link_dependency_params[];
extern const ipman_param_desc_t ipman_op_task_unlink_dependency_params[];
extern const ipman_param_desc_t ipman_op_task_mark_duplicate_params[];
extern const ipman_param_desc_t ipman_op_task_replace_params[];
extern const ipman_param_desc_t ipman_op_task_close_params[];
extern const ipman_param_desc_t ipman_op_task_reopen_params[];

#endif
