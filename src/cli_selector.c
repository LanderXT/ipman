/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "cli_selector.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "dispatch.h"
#include "protocol.h"

/* ------------------------------------------------------------------ */
/* Internal: in-process op dispatch with error capture.               */
/* ------------------------------------------------------------------ */

static void set_err(char *err_buf, size_t err_buf_size, const char *fmt, ...) {
    if (err_buf == NULL || err_buf_size == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_buf, err_buf_size, fmt, ap);
    va_end(ap);
}

/*
 * Dispatch `op` with `params` in-process. On ok: returns the detached
 * `result` cJSON (caller frees with cJSON_Delete). On error: writes the
 * server message to `err_buf` and returns NULL. `params` is borrowed.
 */
static cJSON *dispatch_op(sqlite3 *db,
                          const char *op,
                          cJSON *params,
                          char *err_buf, size_t err_buf_size) {
    ipman_request_t req;
    memset(&req, 0, sizeof req);
    req.protocol_version = 1;          /* in-process; no parser involved */
    req.request_id       = "cli-selector";
    req.actor            = "cli";
    req.op               = op;
    req.params           = params;
    req.root             = NULL;

    cJSON *response = NULL;
    ipman_dispatch(&req, db, &response);
    if (response == NULL) {
        set_err(err_buf, err_buf_size, "internal error calling %s", op);
        return NULL;
    }

    cJSON *ok_item = cJSON_GetObjectItemCaseSensitive(response, "ok");
    if (!cJSON_IsTrue(ok_item)) {
        cJSON *err = cJSON_GetObjectItemCaseSensitive(response, "error");
        cJSON *msg = err ? cJSON_GetObjectItemCaseSensitive(err, "message") : NULL;
        set_err(err_buf, err_buf_size, "%s",
                (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown error");
        cJSON_Delete(response);
        return NULL;
    }

    cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(response, "result");
    cJSON_Delete(response);
    return result;
}

/* ------------------------------------------------------------------ */
/* Internal: form classification and per-form resolution.             */
/* ------------------------------------------------------------------ */

/* Per-kind static table: keeps the kind/prefix/op-name mapping in one
 * place so adding a kind in the future is a single-row change. */
typedef struct {
    cli_selector_kind_t kind;
    const char         *prefix;     /* uid prefix incl. underscore */
    size_t              prefix_len;
    const char         *lookup_op;
    const char         *get_op;
    const char         *display;    /* singular noun for messages */
} kind_meta_t;

static const kind_meta_t k_kinds[] = {
    { CLI_SELECTOR_KIND_TASK,  "task_",  5, "task.lookup",  "task.get",  "task"  },
    { CLI_SELECTOR_KIND_PHASE, "phase_", 6, "phase.lookup", "phase.get", "phase" },
    { CLI_SELECTOR_KIND_PLAN,  "plan_",  5, "plan.lookup",  "plan.get",  "plan"  },
};
static const size_t k_kinds_n = sizeof k_kinds / sizeof k_kinds[0];

static const kind_meta_t *meta_for(cli_selector_kind_t k) {
    for (size_t i = 0; i < k_kinds_n; i++)
        if (k_kinds[i].kind == k) return &k_kinds[i];
    return NULL;
}

static int parse_positive_long(const char *s, long *out) {
    if (s == NULL || *s == '\0') return -1;
    /* Reject leading whitespace and signs: ids are bare positive integers. */
    for (const char *p = s; *p; p++)
        if (!isdigit((unsigned char)*p)) return -1;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v <= 0) return -1;
    *out = v;
    return 0;
}

/* Try `entity.get` with {id} and return 1 if it exists, 0 if not_found,
 * -1 on any other error (message captured to err_buf). */
static int probe_get_by_id(sqlite3 *db, const kind_meta_t *m, long id,
                           char *err_buf, size_t err_buf_size) {
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        set_err(err_buf, err_buf_size, "out of memory");
        return -1;
    }
    cJSON_AddNumberToObject(p, "id", (double)id);

    char inner_err[CLI_SELECTOR_ERR_LEN];
    inner_err[0] = '\0';
    cJSON *r = dispatch_op(db, m->get_op, p, inner_err, sizeof inner_err);
    cJSON_Delete(p);

    if (r != NULL) { cJSON_Delete(r); return 1; }
    /* not_found is the only "soft" failure when probing across kinds. */
    if (strstr(inner_err, "not found") != NULL) return 0;
    set_err(err_buf, err_buf_size, "%s", inner_err);
    return -1;
}

/* Resolve uid via `*.lookup` and return id, or -1 on error. */
static int lookup_by_uid(sqlite3 *db, const kind_meta_t *m, const char *uid,
                        long *id_out,
                        char *err_buf, size_t err_buf_size) {
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        set_err(err_buf, err_buf_size, "out of memory");
        return -1;
    }
    cJSON_AddStringToObject(p, "uid", uid);

    cJSON *r = dispatch_op(db, m->lookup_op, p, err_buf, err_buf_size);
    cJSON_Delete(p);
    if (r == NULL) return -1;

    cJSON *id_j = cJSON_GetObjectItemCaseSensitive(r, "id");
    if (!cJSON_IsNumber(id_j)) {
        cJSON_Delete(r);
        set_err(err_buf, err_buf_size,
                "%s lookup did not return an id", m->display);
        return -1;
    }
    *id_out = (long)id_j->valuedouble;
    cJSON_Delete(r);
    return 0;
}

/* Resolve label via `*.lookup`. For task/phase, requires `plan_id`; for plan,
 * label is global. Returns 1 on hit, 0 on not_found, -1 on other errors. */
static int lookup_by_label(sqlite3 *db, const kind_meta_t *m,
                           const char *label, long plan_id,
                           long *id_out,
                           char *err_buf, size_t err_buf_size) {
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        set_err(err_buf, err_buf_size, "out of memory");
        return -1;
    }
    cJSON_AddStringToObject(p, "label", label);
    if (m->kind != CLI_SELECTOR_KIND_PLAN)
        cJSON_AddNumberToObject(p, "plan_id", (double)plan_id);

    char inner_err[CLI_SELECTOR_ERR_LEN];
    inner_err[0] = '\0';
    cJSON *r = dispatch_op(db, m->lookup_op, p, inner_err, sizeof inner_err);
    cJSON_Delete(p);
    if (r == NULL) {
        if (strstr(inner_err, "not found") != NULL) return 0;
        set_err(err_buf, err_buf_size, "%s", inner_err);
        return -1;
    }

    cJSON *id_j = cJSON_GetObjectItemCaseSensitive(r, "id");
    if (!cJSON_IsNumber(id_j)) {
        cJSON_Delete(r);
        set_err(err_buf, err_buf_size,
                "%s lookup did not return an id", m->display);
        return -1;
    }
    *id_out = (long)id_j->valuedouble;
    cJSON_Delete(r);
    return 1;
}

/* Read active_plan.id from workspace.context_get. Returns 0 on success
 * with `*plan_id_out` set, -1 if no active plan or on error. */
static int get_active_plan_id(sqlite3 *db, long *plan_id_out,
                              char *err_buf, size_t err_buf_size) {
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        set_err(err_buf, err_buf_size, "out of memory");
        return -1;
    }
    cJSON *r = dispatch_op(db, "workspace.context_get", p,
                           err_buf, err_buf_size);
    cJSON_Delete(p);
    if (r == NULL) return -1;

    cJSON *ctx  = cJSON_GetObjectItemCaseSensitive(r, "context");
    cJSON *plan = ctx ? cJSON_GetObjectItemCaseSensitive(ctx, "active_plan") : NULL;
    cJSON *id_j = plan ? cJSON_GetObjectItemCaseSensitive(plan, "id") : NULL;
    int ok = cJSON_IsNumber(id_j);
    if (ok) *plan_id_out = (long)id_j->valuedouble;
    cJSON_Delete(r);

    if (!ok) {
        set_err(err_buf, err_buf_size,
                "label selector requires an active plan "
                "(use `ipman --activate <plan>` first)");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry point.                                                */
/* ------------------------------------------------------------------ */

static int single_kind(cli_selector_kind_t mask, cli_selector_kind_t *out) {
    /* Power-of-two test: returns 1 iff exactly one bit is set. */
    if (mask == 0 || (mask & (mask - 1)) != 0) return 0;
    if (out) *out = mask;
    return 1;
}

/* Format the accepted kinds in a bitmask as "task", "task or phase",
 * "task, phase, or plan", etc. Always writes a NUL-terminated string. */
static void format_kinds(cli_selector_kind_t mask, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;
    out[0] = '\0';
    const char *names[3];
    size_t n = 0;
    for (size_t i = 0; i < k_kinds_n && n < 3; i++)
        if ((mask & k_kinds[i].kind) != 0) names[n++] = k_kinds[i].display;

    if (n == 0)       snprintf(out, out_size, "(none)");
    else if (n == 1)  snprintf(out, out_size, "%s", names[0]);
    else if (n == 2)  snprintf(out, out_size, "%s or %s", names[0], names[1]);
    else              snprintf(out, out_size, "%s, %s, or %s",
                               names[0], names[1], names[2]);
}

int cli_resolve_selector(sqlite3 *db,
                         const char *arg,
                         cli_selector_kind_t expected,
                         long *id_out,
                         cli_selector_kind_t *kind_out,
                         char *err_buf, size_t err_buf_size) {
    if (err_buf != NULL && err_buf_size > 0) err_buf[0] = '\0';

    if (db == NULL || arg == NULL || arg[0] == '\0' ||
        id_out == NULL || kind_out == NULL || expected == 0) {
        set_err(err_buf, err_buf_size, "invalid selector arguments");
        return -1;
    }

    /* --- Form 1: uid prefix (task_/phase_/plan_) -------------------- */
    for (size_t i = 0; i < k_kinds_n; i++) {
        const kind_meta_t *m = &k_kinds[i];
        if (strncmp(arg, m->prefix, m->prefix_len) != 0) continue;
        if ((expected & m->kind) == 0) {
            char accepted[64];
            format_kinds(expected, accepted, sizeof accepted);
            set_err(err_buf, err_buf_size,
                    "selector `%s` is a %s, but this command requires %s",
                    arg, m->display, accepted);
            return -1;
        }
        long id = 0;
        if (lookup_by_uid(db, m, arg, &id, err_buf, err_buf_size) != 0)
            return -1;
        *id_out   = id;
        *kind_out = m->kind;
        return 0;
    }

    /* --- Form 2: numeric passthrough -------------------------------- */
    long numeric_id = 0;
    if (parse_positive_long(arg, &numeric_id) == 0) {
        cli_selector_kind_t only;
        if (single_kind(expected, &only)) {
            /* Caller's expected kind is unambiguous; trust the id. */
            *id_out   = numeric_id;
            *kind_out = only;
            return 0;
        }
        /* Probe in TASK > PHASE > PLAN order to disambiguate. */
        for (size_t i = 0; i < k_kinds_n; i++) {
            const kind_meta_t *m = &k_kinds[i];
            if ((expected & m->kind) == 0) continue;
            int hit = probe_get_by_id(db, m, numeric_id, err_buf, err_buf_size);
            if (hit < 0) return -1;       /* unrelated dispatch error */
            if (hit > 0) {
                *id_out   = numeric_id;
                *kind_out = m->kind;
                return 0;
            }
        }
        set_err(err_buf, err_buf_size, "no entity with id %ld", numeric_id);
        return -1;
    }

    /* --- Form 3: bare label ----------------------------------------- */
    long plan_id = 0;
    int need_plan_scope =
        (expected & (CLI_SELECTOR_KIND_TASK | CLI_SELECTOR_KIND_PHASE)) != 0;
    if (need_plan_scope &&
        get_active_plan_id(db, &plan_id, err_buf, err_buf_size) != 0)
        return -1;

    for (size_t i = 0; i < k_kinds_n; i++) {
        const kind_meta_t *m = &k_kinds[i];
        if ((expected & m->kind) == 0) continue;
        long id = 0;
        int hit = lookup_by_label(db, m, arg, plan_id,
                                  &id, err_buf, err_buf_size);
        if (hit < 0) return -1;
        if (hit > 0) {
            *id_out   = id;
            *kind_out = m->kind;
            return 0;
        }
    }

    set_err(err_buf, err_buf_size,
            "no task, phase, or plan matches label `%s`", arg);
    /* If only one kind was expected, refine the message. */
    cli_selector_kind_t only;
    if (single_kind(expected, &only)) {
        const kind_meta_t *m = meta_for(only);
        if (m != NULL)
            set_err(err_buf, err_buf_size,
                    "no %s matches label `%s`", m->display, arg);
    }
    return -1;
}
