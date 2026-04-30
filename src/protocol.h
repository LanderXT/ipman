/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_PROTOCOL_H
#define IPMAN_PROTOCOL_H

#include "cJSON.h"

/*
 * Request envelope (input on stdin):
 *   { "protocol_version": 1,
 *     "request_id":       "<non-empty string>",
 *     "actor":            "<non-empty string>",
 *     "op":                "<dotted.name>",
 *     "params":            {} }
 *
 * Response envelope (output on stdout):
 *   success: { "request_id": "...", "ok": true,  "result": {} }
 *   error  : { "request_id": "...|null", "ok": false,
 *              "error": { "code": "...", "message": "...",
 *                         "details": {...}? } }
 *
 * `request_id` is null in the response only when the input could not be
 * parsed far enough to read it (the sole case allowed by the envelope).
 */

typedef enum {
    IPMAN_ERR_INVALID_REQUEST = 0,    /* fatal — exit 1 */
    IPMAN_ERR_UNKNOWN_OP,
    IPMAN_ERR_VALIDATION_FAILED,      /* incl. ambiguous label — see msg */
    IPMAN_ERR_NOT_FOUND,
    IPMAN_ERR_CONFLICT,
    IPMAN_ERR_INTERNAL                /* fatal — exit 1 */
} ipman_error_code_t;

const char *ipman_error_code_str(ipman_error_code_t code);
int         ipman_error_is_fatal(ipman_error_code_t code);

typedef struct {
    int          protocol_version;
    const char  *request_id;  /* non-owning; borrows from `root` */
    const char  *actor;       /* non-owning; borrows from `root` */
    const char  *op;          /* non-owning; borrows from `root` */
    cJSON       *params;      /* non-owning; lives inside `root` */
    cJSON       *root;        /* OWNING; cJSON_Delete releases the tree */
} ipman_request_t;

/*
 * Parse `body` (NUL-terminated) into `req`. On success returns 0 and
 * `req->root` owns the parsed tree.
 *
 * On failure returns -1, leaves `req` zeroed, and writes a fatal-grade
 * error code plus a static-lifetime message into the out-params. If the
 * body was well-formed JSON but failed schema validation (e.g. missing
 * `op`), `*request_id_out` is set to a heap string the caller must free;
 * otherwise `*request_id_out` is NULL.
 */
int ipman_request_parse(const char *body,
                       ipman_request_t *req,
                       ipman_error_code_t *err_code_out,
                       const char **err_msg_out,
                       char **request_id_out);

/*
 * Build a response. Returns a freshly-allocated cJSON tree the caller owns.
 * `result` / `details`, when non-NULL, are transferred (ownership moves into
 * the response) and must not be freed separately. `request_id` may be NULL
 * (serialized as JSON null).
 */
cJSON *ipman_response_ok (const char *request_id, cJSON *result);
cJSON *ipman_response_err(const char *request_id,
                         ipman_error_code_t code,
                         const char *message,
                         cJSON *details);

/*
 * Attach a machine-readable `error.details` object to the next error response.
 * Handlers call this immediately before returning -1 to surface structured
 * context alongside the `error.code` and `error.message` strings (RFC 7807
 * style). Ownership of `details` transfers to the protocol layer.
 *
 * Calling twice in one dispatch frees the previous value; dispatch also
 * clears any leftover state before and after each handler invocation, so a
 * successful path cannot accidentally leak details from a prior failure.
 *
 * Safe for the single-shot ipman CLI model (one request per process). A future
 * in-process dispatcher would replace this with a per-request context.
 */
void   ipman_error_attach_details(cJSON *details);
cJSON *ipman_error_take_details  (void);

#endif
