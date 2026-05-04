/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IMPORT_OPS_H
#define IMPORT_OPS_H

#include <sqlite3.h>

/*
 * Import a plan.export envelope (JSON file produced by plan.export) into the
 * current workspace as a new plan, allocating fresh IDs for every entity and
 * re-mapping all cross-references.
 *
 * On success returns 0 and writes the new plan's numeric ID into *new_plan_id_out.
 * On failure returns non-zero, writes a human-readable message into *err_msg_out
 * (caller must free), and attempts saga-pattern cleanup of any partially created
 * entities.
 *
 * Parameters:
 *   db            - open ipman database
 *   json_path     - path to the JSON file produced by plan.export
 *   new_plan_id_out - receives the newly created plan's id on success
 *   err_msg_out   - receives a heap string on failure (caller frees)
 */
int ipman_import_plan_envelope(sqlite3 *db,
                               const char *json_path,
                               sqlite3_int64 *new_plan_id_out,
                               char **err_msg_out);

#endif /* IMPORT_OPS_H */
