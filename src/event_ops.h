#ifndef IPMAN_EVENT_OPS_H
#define IPMAN_EVENT_OPS_H

#include "dispatch.h"

#include <stddef.h>

/*
 * Compute the human-readable ref for an entity. On success writes the ref
 * (plain plan code for plans, "<code>/F<local_seq>" for phases, or
 * "<code>/T<local_seq>" for tasks) into buf. On failure to resolve (entity
 * missing, unknown type, prepare failure) leaves buf as an empty string.
 */
void ipman_compute_entity_ref(sqlite3 *db,
                             const char *entity_type,
                             sqlite3_int64 entity_id,
                             char *buf, size_t cap);

cJSON *ipman_event_from_row(sqlite3_stmt *stmt);

/*
 * Read the entity type/id pair from `obj` and attach a ref string under
 * `ref_field`. Adds null when the type/id pair is missing or does not
 * resolve. Used to enrich event and relation responses.
 */
void ipman_attach_entity_ref(sqlite3 *db, cJSON *obj,
                            const char *type_field,
                            const char *id_field,
                            const char *ref_field);

int ipman_op_event_list(const ipman_request_t *req, sqlite3 *db,
                       cJSON **result_out,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out);
extern const ipman_param_desc_t ipman_op_event_list_params[];

#endif
