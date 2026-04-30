/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_EVENT_OPS_H
#define IPMAN_EVENT_OPS_H

#include "dispatch.h"

#include <stddef.h>

cJSON *ipman_event_from_row(sqlite3_stmt *stmt);

int ipman_op_event_list(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out);
extern const ipman_param_desc_t ipman_op_event_list_params[];

#endif
