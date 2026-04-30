#ifndef IPMAN_DISPATCH_H
#define IPMAN_DISPATCH_H

#include "cJSON.h"
#include "protocol.h"
#include "sqlite3.h"

/*
 * Operation handler. Returns 0 on success (and transfers ownership of a
 * fresh cJSON result via *result_out) or -1 on failure, in which case it
 * writes an error code and a static-lifetime message via the out-params.
 * `req` and `db` are borrowed, never freed by the handler.
 *
 * Handlers should not emit IPMAN_ERR_INVALID_REQUEST — that is reserved for
 * the protocol parser. IPMAN_ERR_INTERNAL is allowed for operation-time
 * storage or allocation failures and dispatch maps it to fatal exit.
 */
typedef int (*ipman_handler_fn)(const ipman_request_t *req, sqlite3 *db,
                               cJSON **result_out,
                               ipman_error_code_t *err_code_out,
                               const char **err_msg_out);

/*
 * Per-param metadata. Today only the key name; shaped to extend (type hint,
 * required flag, enum values) without touching every call site.
 */
typedef struct {
    const char *name;
} ipman_param_desc_t;

typedef struct {
    const char              *name;     /* dotted, e.g. "noop" or "plan.create" */
    ipman_handler_fn          handler;
    const ipman_param_desc_t *params;   /* NULL-name-sentinel; never NULL pointer */
} ipman_op_t;

size_t      ipman_dispatch_operation_count(void);
const char *ipman_dispatch_operation_name_at(size_t index);
const ipman_param_desc_t *ipman_dispatch_operation_params_at(size_t index);

/*
 * Dispatch a parsed request. Builds the response cJSON tree and writes it
 * to *response_out; the caller owns it. Returns 0 if the response is ok,
 * -1 if the response is a semantic error, or -2 if it is fatal.
 */
int ipman_dispatch(const ipman_request_t *req, sqlite3 *db, cJSON **response_out);

#endif
