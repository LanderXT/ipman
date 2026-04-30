/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_EXPORT_OPS_H
#define IPMAN_EXPORT_OPS_H

#include "dispatch.h"
#include "protocol.h"

#include <sqlite3.h>

/*
 * plan.export — read-only canonical snapshot of one plan and its dependents.
 *
 * Inputs:
 *   plan_id (int, required)
 *
 * Output (result.export):
 *   {
 *     "export_format_version": 3,
 *     "schema_version":        <int>,
 *     "generated_at":          "<ISO-8601 UTC>",
 *     "plan":      { uid, label, id, ... },
 *     "phases":    [ { uid, label, id, ... }, ordered by id asc ],
 *     "tasks":     [ { uid, label, id, ... }, ordered by id asc ],
 *     "comments":  [ ... plan + phase + task scope, includes invalidated, id asc ],
 *     "instructions": [ ... plan + phase + task scope, includes invalidated, id asc ],
 *     "events":    [ ... plan + phase + task scope, id asc ],
 *     "closures":  [ ... plan + phase + task scope, id asc ],
 *     "relations": [ ... task_relations where both endpoints are in this plan, id asc ]
 *   }
 *
 * v3 vs. v2: adds first-class instructions.
 *
 * v2 vs. v1: plan/phase/task rows lead with uid/label and include local_seq;
 * NULL-valued optional fields are omitted entirely instead of being emitted
 * as explicit `null`. Older snapshots remain valid v1 documents.
 *
 * Emits no events. Touches no state.
 */
int ipman_op_plan_export(const ipman_request_t *req, sqlite3 *db,
                        cJSON **result_out,
                        ipman_error_code_t *err_code_out,
                        const char **err_msg_out);
extern const ipman_param_desc_t ipman_op_plan_export_params[];

#endif
