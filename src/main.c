/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

/*
 * ipman — Implementation Plan Manager
 *
 * Per-invocation flow: require initialized home + DB, read stdin, parse
 * request, dispatch to handler, serialize response on stdout. Logs go to
 * stderr. `ipman init` creates the home + DB explicitly.
 *
 * Exit codes:
 *   0 — a well-formed response (ok:true or ok:false with a semantic error)
 *       was produced.
 *   1 — the protocol could not be executed: malformed input, internal
 *       error, DB unreachable, etc. A best-effort error envelope is still
 *       emitted on stdout so callers can surface the code. In b64 mode the
 *       stdout envelope is base64-encoded.
 */

#include "db.h"
#include "agent_docs.h"
#include "dispatch.h"
#include "io.h"
#include "log.h"
#include "migrate_ops.h"
#include "sql_ops.h"
#include "migrations.h"
#include "ipman_home.h"
#include "ipman_key.h"
#include "protocol.h"
#include "render_md.h"
#include "skill_install.h"
#include "cli_output.h"

#include <cJSON.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_b64_mode = 0;

/* Default cap on stdin payload size (16 MiB). Overridable per-process via the
 * IPMAN_MAX_REQUEST_BYTES env var. Mitigates OWASP A04 unbounded-input risk on
 * a CLI that any agent can pipe into. */
#define IPMAN_DEFAULT_MAX_REQUEST_BYTES (16u * 1024u * 1024u)

/* Parse IPMAN_MAX_REQUEST_BYTES. Returns the configured limit; on missing,
 * empty, or unparseable values, falls back to the default and logs once. */
static size_t resolve_max_request_bytes(void) {
    const char *raw = getenv("IPMAN_MAX_REQUEST_BYTES");
    if (raw == NULL || raw[0] == '\0') {
        return IPMAN_DEFAULT_MAX_REQUEST_BYTES;
    }
    errno = 0;
    char *endp = NULL;
    unsigned long long v = strtoull(raw, &endp, 10);
    if (errno != 0 || endp == raw || (endp != NULL && *endp != '\0')) {
        ipman_log_warn("invalid IPMAN_MAX_REQUEST_BYTES; using default",
                      "value=\"%s\" default=%u", raw,
                      (unsigned)IPMAN_DEFAULT_MAX_REQUEST_BYTES);
        return IPMAN_DEFAULT_MAX_REQUEST_BYTES;
    }
    if (v > (unsigned long long)((size_t)-1)) {
        ipman_log_warn("IPMAN_MAX_REQUEST_BYTES exceeds size_t; using default",
                      "value=\"%s\"", raw);
        return IPMAN_DEFAULT_MAX_REQUEST_BYTES;
    }
    return (size_t)v;
}

static void emit_response_text(const char *json) {
    static const char encode_fail_b64[] =
        "eyJyZXF1ZXN0X2lkIjpudWxsLCJvayI6ZmFsc2UsImVycm9yIjp7ImNvZGUiOiJpbnRlcm5hbF9lcnJvciIsIm1lc3NhZ2UiOiJmYWlsZWQgdG8gZW5jb2RlIHJlc3BvbnNlIn19";

    if (g_b64_mode) {
        char *encoded = NULL;
        if (ipman_base64_encode((const unsigned char *)json, strlen(json), &encoded) != 0) {
            fputs(encode_fail_b64, stdout);
            fputc('\n', stdout);
            return;
        }
        fputs(encoded, stdout);
        fputc('\n', stdout);
        free(encoded);
        return;
    }

    fputs(json, stdout);
    fputc('\n', stdout);
}

static void emit_response(cJSON *resp) {
    if (resp == NULL) {
        /* Out-of-memory during response construction. Produce the simplest
         * possible fatal envelope by hand so callers still get valid JSON. */
        const char *fallback =
            "{\"request_id\":null,\"ok\":false,"
            "\"error\":{\"code\":\"internal_error\","
            "\"message\":\"failed to build response\"}}";
        emit_response_text(fallback);
        return;
    }
    char *s = cJSON_PrintUnformatted(resp);
    if (s == NULL) {
        const char *fallback =
            "{\"request_id\":null,\"ok\":false,"
            "\"error\":{\"code\":\"internal_error\","
            "\"message\":\"failed to serialize response\"}}";
        emit_response_text(fallback);
        return;
    }

    emit_response_text(s);
    cJSON_free(s);
}

static int emit_fatal(const char *request_id, ipman_error_code_t code,
                      const char *msg) {
    cJSON *resp = ipman_response_err(request_id, code, msg, NULL);
    emit_response(resp);
    if (resp) cJSON_Delete(resp);
    return 1;
}

static void print_usage(FILE *out) {
    fputs(
        "ipman - Implementation Plan Manager\n"
        "\n"
        "Each form below is interchangeable with its bare-word and (where\n"
        "shown) short-flag equivalents: `ipman -S` = `ipman --status` =\n"
        "`ipman status`, and `ipman export ...` = `ipman --export ...`.\n"
        "\n"
        "Human commands (read-only):\n"
        "  -S  / --status              Show active plan, phase, task and pending count\n"
        "  -L  / --ls                  List pending tasks for the active plan\n"
        "  -SH / --show <selector>     Show detail for a task or phase (uid, label, or id)\n"
        "  -LG / --log                 Show recent workspace events\n"
        "  -R  / --render <plan>       Render plan as Markdown (code, uid, label, or id)\n"
        "  -I  / --init                Initialize workspace\n"
        "  -U  / --usage               Show this help\n"
        "\n"
        "Agent protocol (JSON on stdin):\n"
        "  ipman < request.json\n"
        "  -B / --b64 < request.b64    Read stdin as base64-encoded JSON\n"
        "\n"
        "Maintenance:\n"
        "  --migrate-encrypt           Convert a plaintext ipman.db to encrypted in-place\n"
        "  export --plaintext --i-understand <out.db>\n"
        "                              Emergency dump: write a plaintext SQLite copy\n"
        "                              of the encrypted DB to <out.db>\n"
        "  sql \"SQL...\"                Run ad-hoc SQL (developer / test escape-hatch)\n"
        "\n"
        "Default storage: ./.ipman/ipman.db\n"
        "Docs after init: .ipman/START-HERE.md\n",
        out);
}

static int build_db_path(const char *home, char *dbpath, size_t dbpath_len) {
    int n = snprintf(dbpath, dbpath_len, "%s/ipman.db", home);
    if (n < 0 || (size_t)n >= dbpath_len) {
        ipman_log_error("db path too long", "home=%s", home);
        return -1;
    }
    return 0;
}

static int open_workspace(sqlite3 **db_out, char *home, size_t home_len,
                          char *dbpath, size_t dbpath_len,
                          int create, int *schema_version_out) {
    if (ipman_home_resolve(home, home_len) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL, "cannot resolve ipman home");
    }
    if (create) {
        if (ipman_home_ensure(home) != 0) {
            return emit_fatal(NULL, IPMAN_ERR_INTERNAL,
                              "cannot prepare ipman home");
        }
        if (ipman_keysalt_ensure(home) != 0) {
            return emit_fatal(NULL, IPMAN_ERR_INTERNAL,
                              "cannot prepare keysalt");
        }
    } else if (ipman_home_require(home) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL,
                          "ipman workspace is not initialized; run `ipman init`");
    }

    if (build_db_path(home, dbpath, dbpath_len) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL, "db path too long");
    }
    if (!create && access(dbpath, F_OK) != 0) {
        ipman_log_error("ipman database missing", "path=%s", dbpath);
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL,
                          "ipman workspace is not initialized; run `ipman init`");
    }

    sqlite3 *db = NULL;
    int db_rc = create ? ipman_db_open(home, dbpath, &db)
                       : ipman_db_open_existing(home, dbpath, &db);
    if (db_rc != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL, "cannot open database");
    }

    int schema_version = 0;
    if (ipman_migrations_apply(db, &schema_version) != 0) {
        ipman_db_close(db);
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL, "migration apply failed");
    }

    if (ipman_agent_docs_refresh(home, dbpath, NULL) != 0) {
        ipman_db_close(db);
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL,
                          "agent docs generation failed");
    }

    *db_out = db;
    if (schema_version_out != NULL) *schema_version_out = schema_version;
    return 0;
}

static int run_render(const char *selector) {
    char home[PATH_MAX];
    char dbpath[PATH_MAX];
    sqlite3 *db = NULL;
    int rc = open_workspace(&db, home, sizeof home, dbpath, sizeof dbpath,
                            0, NULL);
    if (rc != 0) return rc;

    sqlite3_int64 plan_id = 0;
    if (ipman_render_resolve_plan(db, selector, &plan_id) != 0) {
        fprintf(stderr, "ipman render: plan not found: %s\n", selector);
        ipman_db_close(db);
        return 1;
    }

    int render_rc = ipman_render_md(db, plan_id, stdout);
    ipman_db_close(db);
    return render_rc != 0 ? 1 : 0;
}

static int run_init(void) {
    char home[PATH_MAX];
    char dbpath[PATH_MAX];
    sqlite3 *db = NULL;
    int schema_version = 0;
    int rc = open_workspace(&db, home, sizeof home, dbpath, sizeof dbpath,
                            1, &schema_version);
    if (rc != 0) return rc;

    /* Best-effort: make the workspace-local skill discoverable for Claude
     * Code and Codex when the corresponding global install is absent. Failure
     * is logged but does not fail init — the workspace remains usable. */
    (void)ipman_skill_install();

    cJSON *result = cJSON_CreateObject();
    if (result != NULL) {
        cJSON_AddBoolToObject(result, "initialized", 1);
        cJSON_AddStringToObject(result, "home", home);
        cJSON_AddStringToObject(result, "db_path", dbpath);
        cJSON_AddNumberToObject(result, "schema_version", schema_version);
    }
    cJSON *resp = ipman_response_ok(NULL, result);
    emit_response(resp);
    if (resp) cJSON_Delete(resp);
    ipman_db_close(db);
    return 0;
}

static int run_migrate_encrypt(void) {
    char home[PATH_MAX];
    if (ipman_home_resolve(home, sizeof home) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL, "cannot resolve ipman home");
    }
    if (ipman_home_require(home) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL,
                          "ipman workspace is not initialized; run `ipman init` first");
    }
    /* Ensure the keysalt exists before deriving — for workspaces that pre-date
     * the encryption work, this generates it on first migrate. */
    if (ipman_keysalt_ensure(home) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL, "cannot prepare keysalt");
    }
    if (ipman_migrate_encrypt(home) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL, "migrate-encrypt failed");
    }
    return 0;
}

/* Parse `ipman export --plaintext --i-understand <output>` argv (argv[1] is
 * already known to be the export verb). Order of the two flags is free, but
 * both are required; <output> is the lone positional. Mismatches print a
 * usage line on stderr and return non-zero. */
static int run_export(int argc, char **argv) {
    int saw_plaintext = 0;
    int saw_understand = 0;
    const char *out_path = NULL;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--plaintext") == 0) {
            saw_plaintext = 1;
        } else if (strcmp(argv[i], "--i-understand") == 0) {
            saw_understand = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "ipman export: unknown flag: %s\n", argv[i]);
            return 1;
        } else if (out_path == NULL) {
            out_path = argv[i];
        } else {
            fprintf(stderr, "ipman export: unexpected extra argument: %s\n",
                    argv[i]);
            return 1;
        }
    }
    if (!saw_plaintext) {
        fprintf(stderr,
                "ipman export: --plaintext is required (no other modes yet)\n");
        return 1;
    }
    if (!saw_understand) {
        fprintf(stderr,
                "ipman export --plaintext: refused without --i-understand.\n"
                "This writes an unencrypted copy of your DB; pass\n"
                "  --i-understand to acknowledge.\n");
        return 1;
    }
    if (out_path == NULL) {
        fprintf(stderr,
                "usage: ipman export --plaintext --i-understand <output.db>\n");
        return 1;
    }

    char home[PATH_MAX];
    if (ipman_home_resolve(home, sizeof home) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL, "cannot resolve ipman home");
    }
    if (ipman_home_require(home) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL,
                          "ipman workspace is not initialized; run `ipman init` first");
    }
    if (ipman_export_plaintext(home, out_path) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL, "export --plaintext failed");
    }
    return 0;
}

/* Developer / test escape-hatch: open the workspace through the standard
 * keyed open and run ad-hoc SQL. Output format mirrors `sqlite3` defaults
 * (one row per line, '|' between columns) so this drops in for tests that
 * previously called `sqlite3 "$DB" "..."`. Single SQL string as positional
 * argv. Errors land on stderr; rows on stdout. */
static int run_sql_cmd(int argc, char **argv) {
    const char *sql = NULL;
    for (int i = 2; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "ipman sql: unknown flag: %s\n", argv[i]);
            return 1;
        } else if (sql == NULL) {
            sql = argv[i];
        } else {
            fprintf(stderr, "ipman sql: only one SQL string is supported "
                            "(got extra: %s)\n", argv[i]);
            return 1;
        }
    }
    if (sql == NULL) {
        fprintf(stderr, "usage: ipman sql \"SQL statement\"\n");
        return 1;
    }

    char home[PATH_MAX], dbpath[PATH_MAX];
    sqlite3 *db = NULL;
    int ws_rc = open_workspace(&db, home, sizeof home,
                               dbpath, sizeof dbpath, 0, NULL);
    if (ws_rc != 0) return ws_rc;

    int sql_rc = ipman_sql_run(db, sql, stdout);
    ipman_db_close(db);
    return sql_rc != 0 ? 1 : 0;
}

/* ---- command parsing ---------------------------------------------- */

/* Map any supported form (bare word, -X, --word) to a canonical name.
 * Returns NULL for unrecognized tokens. */
static const char *parse_command(const char *arg) {
    if (arg == NULL) return NULL;
    if (strcmp(arg, "init")     == 0 || strcmp(arg, "-I")  == 0 || strcmp(arg, "--init")   == 0) return "init";
    if (strcmp(arg, "migrate-encrypt") == 0 || strcmp(arg, "--migrate-encrypt") == 0) return "migrate-encrypt";
    if (strcmp(arg, "export") == 0 || strcmp(arg, "--export") == 0) return "export";
    if (strcmp(arg, "sql") == 0 || strcmp(arg, "--sql") == 0) return "sql";
    if (strcmp(arg, "render")   == 0 || strcmp(arg, "-R")  == 0 || strcmp(arg, "--render")  == 0) return "render";
    if (strcmp(arg, "status")   == 0 || strcmp(arg, "-S")  == 0 || strcmp(arg, "--status")  == 0) return "status";
    if (strcmp(arg, "ls")       == 0 || strcmp(arg, "-L")  == 0 || strcmp(arg, "--ls")      == 0) return "ls";
    if (strcmp(arg, "show")     == 0 || strcmp(arg, "-SH") == 0 || strcmp(arg, "--show")    == 0) return "show";
    if (strcmp(arg, "log")      == 0 || strcmp(arg, "-LG") == 0 || strcmp(arg, "--log")     == 0) return "log";
    if (strcmp(arg, "b64")      == 0 || strcmp(arg, "-B")  == 0 || strcmp(arg, "--b64")     == 0) return "b64";
    if (strcmp(arg, "usage")    == 0 || strcmp(arg, "-U")  == 0 || strcmp(arg, "--usage")   == 0) return "usage";
    return NULL;
}

/* ---- internal dispatch helper ------------------------------------- */

/* Call a read op from CLI context. Caller provides a cJSON params object
 * (may be an empty object; must not be NULL). Returns the detached
 * result node on success, NULL on error (message already printed to stderr).
 * Caller owns the returned cJSON and must free it with cJSON_Delete(). */
static cJSON *call_op(sqlite3 *db, const char *op, cJSON *params) {
    ipman_request_t req;
    memset(&req, 0, sizeof req);
    req.protocol_version = 1;
    req.request_id       = "cli";
    req.actor            = "cli";
    req.op               = op;
    req.params           = params;
    req.root             = NULL;

    cJSON *response = NULL;
    ipman_dispatch(&req, db, &response);
    if (response == NULL) {
        fprintf(stderr, "ipman: internal error calling %s\n", op);
        return NULL;
    }

    cJSON *ok_item = cJSON_GetObjectItemCaseSensitive(response, "ok");
    if (!cJSON_IsTrue(ok_item)) {
        cJSON *err = cJSON_GetObjectItemCaseSensitive(response, "error");
        cJSON *msg = err ? cJSON_GetObjectItemCaseSensitive(err, "message") : NULL;
        fprintf(stderr, "ipman: %s\n",
                (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown error");
        cJSON_Delete(response);
        return NULL;
    }

    cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(response, "result");
    cJSON_Delete(response);
    return result;
}

/* ---- human CLI commands ------------------------------------------- */

static int run_status(void) {
    char home[PATH_MAX], dbpath[PATH_MAX];
    sqlite3 *db = NULL;
    if (open_workspace(&db, home, sizeof home, dbpath, sizeof dbpath, 0, NULL) != 0)
        return 1;

    cJSON *ctx_params = cJSON_CreateObject();
    cJSON *ctx = call_op(db, "workspace.context_get", ctx_params);
    cJSON_Delete(ctx_params);
    if (ctx == NULL) { ipman_db_close(db); return 1; }

    cJSON *context     = cJSON_GetObjectItemCaseSensitive(ctx, "context");
    cJSON *active_plan = context ? cJSON_GetObjectItemCaseSensitive(context, "active_plan")  : NULL;
    cJSON *cur_phase   = context ? cJSON_GetObjectItemCaseSensitive(context, "current_phase") : NULL;
    cJSON *cur_task    = context ? cJSON_GetObjectItemCaseSensitive(context, "current_task")  : NULL;

    /* Build plan label: "CODE · Title" or "none" */
    char plan_val[256]  = "none";
    char phase_val[256] = "none";
    char task_val[256]  = "none";
    int  pending        = 0;

    if (cJSON_IsObject(active_plan)) {
        cJSON *code  = cJSON_GetObjectItemCaseSensitive(active_plan, "code");
        cJSON *title = cJSON_GetObjectItemCaseSensitive(active_plan, "title");
        snprintf(plan_val, sizeof plan_val, "%s · %s",
                 (code  && cJSON_IsString(code))  ? code->valuestring  : "?",
                 (title && cJSON_IsString(title))  ? title->valuestring : "?");

        /* Pending task count */
        cJSON *plan_id_item = cJSON_GetObjectItemCaseSensitive(active_plan, "id");
        if (cJSON_IsNumber(plan_id_item)) {
            cJSON *lp = cJSON_CreateObject();
            cJSON_AddNumberToObject(lp, "plan_id", plan_id_item->valuedouble);
            cJSON_AddBoolToObject(lp, "pending", 1);
            cJSON_AddNumberToObject(lp, "limit", 500);
            cJSON *lr = call_op(db, "task.list", lp);
            cJSON_Delete(lp);
            if (lr != NULL) {
                cJSON *tasks = cJSON_GetObjectItemCaseSensitive(lr, "tasks");
                if (cJSON_IsArray(tasks)) pending = cJSON_GetArraySize(tasks);
                cJSON_Delete(lr);
            }
        }
    }
    if (cJSON_IsObject(cur_phase)) {
        cJSON *label = cJSON_GetObjectItemCaseSensitive(cur_phase, "label");
        cJSON *title = cJSON_GetObjectItemCaseSensitive(cur_phase, "title");
        snprintf(phase_val, sizeof phase_val, "%s · %s",
                 (label && cJSON_IsString(label)) ? label->valuestring : "?",
                 (title && cJSON_IsString(title)) ? title->valuestring : "?");
    }
    if (cJSON_IsObject(cur_task)) {
        cJSON *label = cJSON_GetObjectItemCaseSensitive(cur_task, "label");
        cJSON *title = cJSON_GetObjectItemCaseSensitive(cur_task, "title");
        snprintf(task_val, sizeof task_val, "%s · %s",
                 (label && cJSON_IsString(label)) ? label->valuestring : "?",
                 (title && cJSON_IsString(title)) ? title->valuestring : "?");
    }

    char pending_str[32];
    snprintf(pending_str, sizeof pending_str, "%d", pending);

    cli_table_t t;
    const char *headers[] = {"Field", "Value"};
    cli_table_init(&t, 2, headers);
    const char *r0[] = {"Active plan",    plan_val};
    const char *r1[] = {"Current phase",  phase_val};
    const char *r2[] = {"Current task",   task_val};
    const char *r3[] = {"Pending tasks",  pending_str};
    cli_table_add_row(&t, r0);
    cli_table_add_row(&t, r1);
    cli_table_add_row(&t, r2);
    cli_table_add_row(&t, r3);
    cli_table_print(&t, stdout);
    cli_table_free(&t);

    cJSON_Delete(ctx);
    ipman_db_close(db);
    return 0;
}

static int run_ls(void) {
    char home[PATH_MAX], dbpath[PATH_MAX];
    sqlite3 *db = NULL;
    if (open_workspace(&db, home, sizeof home, dbpath, sizeof dbpath, 0, NULL) != 0)
        return 1;

    /* Get active plan id from context */
    cJSON *ctx_params = cJSON_CreateObject();
    cJSON *ctx = call_op(db, "workspace.context_get", ctx_params);
    cJSON_Delete(ctx_params);
    if (ctx == NULL) { ipman_db_close(db); return 1; }

    cJSON *context  = cJSON_GetObjectItemCaseSensitive(ctx, "context");
    cJSON *plan_obj = context ? cJSON_GetObjectItemCaseSensitive(context, "active_plan") : NULL;
    cJSON *plan_id  = plan_obj ? cJSON_GetObjectItemCaseSensitive(plan_obj, "id") : NULL;

    if (!cJSON_IsNumber(plan_id)) {
        fprintf(stderr, "ipman: no active plan — run `ipman --init` and `plan.activate`\n");
        cJSON_Delete(ctx);
        ipman_db_close(db);
        return 1;
    }

    cJSON *lp = cJSON_CreateObject();
    cJSON_AddNumberToObject(lp, "plan_id", plan_id->valuedouble);
    cJSON_AddBoolToObject(lp, "pending", 1);
    cJSON_AddNumberToObject(lp, "limit", 500);
    cJSON_Delete(ctx);

    cJSON *lr = call_op(db, "task.list", lp);
    cJSON_Delete(lp);
    if (lr == NULL) { ipman_db_close(db); return 1; }

    cli_table_t t;
    const char *headers[] = {"Label", "Priority", "Status", "Title"};
    cli_table_init(&t, 4, headers);

    cJSON *tasks = cJSON_GetObjectItemCaseSensitive(lr, "tasks");
    cJSON *task;
    cJSON_ArrayForEach(task, tasks) {
        cJSON *label    = cJSON_GetObjectItemCaseSensitive(task, "label");
        cJSON *priority = cJSON_GetObjectItemCaseSensitive(task, "priority");
        cJSON *status   = cJSON_GetObjectItemCaseSensitive(task, "status");
        cJSON *title    = cJSON_GetObjectItemCaseSensitive(task, "title");
        const char *row[] = {
            (label    && cJSON_IsString(label))    ? label->valuestring    : "",
            (priority && cJSON_IsString(priority)) ? priority->valuestring : "",
            (status   && cJSON_IsString(status))   ? status->valuestring   : "",
            (title    && cJSON_IsString(title))    ? title->valuestring    : "",
        };
        cli_table_add_row(&t, row);
    }

    cli_table_print(&t, stdout);
    cli_table_free(&t);
    cJSON_Delete(lr);
    ipman_db_close(db);
    return 0;
}

/* Fields to skip in --show output (internal or deprecated). */
static int show_skip_field(const char *name) {
    static const char * const skip[] = {
        "local_seq", "plan_id", "phase_id", "parent_task_id",
        "origin_task_id", NULL
    };
    for (int i = 0; skip[i]; i++)
        if (strcmp(name, skip[i]) == 0) return 1;
    return 0;
}

static int run_show(const char *selector) {
    char home[PATH_MAX], dbpath[PATH_MAX];
    sqlite3 *db = NULL;
    if (open_workspace(&db, home, sizeof home, dbpath, sizeof dbpath, 0, NULL) != 0)
        return 1;

    cJSON *result = NULL;

    if (strncmp(selector, "task_", 5) == 0) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "uid", selector);
        result = call_op(db, "task.get", p);
        cJSON_Delete(p);
    } else if (strncmp(selector, "phase_", 6) == 0) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "uid", selector);
        result = call_op(db, "phase.get", p);
        cJSON_Delete(p);
    } else {
        char *end = NULL;
        long id = strtol(selector, &end, 10);
        if (end != selector && *end == '\0' && id > 0) {
            /* Numeric: try task, then phase */
            cJSON *p = cJSON_CreateObject();
            cJSON_AddNumberToObject(p, "id", (double)id);
            result = call_op(db, "task.get", p);
            if (result == NULL) result = call_op(db, "phase.get", p);
            cJSON_Delete(p);
        } else {
            /* Label: needs active plan scope.
             * workspace.context_get embeds a partial plan without uid, so we
             * fetch the full plan via plan.get to get the uid. */
            cJSON *ctx_p = cJSON_CreateObject();
            cJSON *ctx   = call_op(db, "workspace.context_get", ctx_p);
            cJSON_Delete(ctx_p);
            cJSON *context    = ctx ? cJSON_GetObjectItemCaseSensitive(ctx, "context") : NULL;
            cJSON *plan_embed = context ? cJSON_GetObjectItemCaseSensitive(context, "active_plan") : NULL;
            cJSON *plan_id_j  = plan_embed ? cJSON_GetObjectItemCaseSensitive(plan_embed, "id") : NULL;

            if (!cJSON_IsNumber(plan_id_j)) {
                fprintf(stderr, "ipman show: label selector requires an active plan\n");
                if (ctx) cJSON_Delete(ctx);
                ipman_db_close(db);
                return 1;
            }

            cJSON *plan_p = cJSON_CreateObject();
            cJSON_AddNumberToObject(plan_p, "id", plan_id_j->valuedouble);
            cJSON *plan_r = call_op(db, "plan.get", plan_p);
            cJSON_Delete(plan_p);
            cJSON_Delete(ctx);

            cJSON *plan_full = plan_r ? cJSON_GetObjectItemCaseSensitive(plan_r, "plan") : NULL;
            cJSON *plan_uid  = plan_full ? cJSON_GetObjectItemCaseSensitive(plan_full, "uid") : NULL;
            if (!cJSON_IsString(plan_uid)) {
                if (plan_r) cJSON_Delete(plan_r);
                ipman_db_close(db);
                return 1;
            }

            /* Try task first, then phase */
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "label",    selector);
            cJSON_AddStringToObject(p, "plan_uid", plan_uid->valuestring);
            result = call_op(db, "task.get", p);
            if (result == NULL) result = call_op(db, "phase.get", p);
            cJSON_Delete(p);
            cJSON_Delete(plan_r);
        }
    }

    if (result == NULL) { ipman_db_close(db); return 1; }

    /* result has either "task" or "phase" key */
    cJSON *entity = cJSON_GetObjectItemCaseSensitive(result, "task");
    if (entity == NULL) entity = cJSON_GetObjectItemCaseSensitive(result, "phase");

    cli_table_t t;
    const char *headers[] = {"Field", "Value"};
    cli_table_init(&t, 2, headers);

    if (cJSON_IsObject(entity)) {
        cJSON *field;
        cJSON_ArrayForEach(field, entity) {
            if (field->string == NULL || show_skip_field(field->string)) continue;
            char val[512] = "";
            if (cJSON_IsString(field))      snprintf(val, sizeof val, "%s", field->valuestring);
            else if (cJSON_IsNumber(field)) snprintf(val, sizeof val, "%.0f", field->valuedouble);
            else if (cJSON_IsTrue(field))   snprintf(val, sizeof val, "true");
            else if (cJSON_IsFalse(field))  snprintf(val, sizeof val, "false");
            else if (cJSON_IsNull(field))   continue;
            else continue;
            const char *row[] = {field->string, val};
            cli_table_add_row(&t, row);
        }
    }

    cli_table_print(&t, stdout);
    cli_table_free(&t);
    cJSON_Delete(result);
    ipman_db_close(db);
    return 0;
}

static int run_log(void) {
    char home[PATH_MAX], dbpath[PATH_MAX];
    sqlite3 *db = NULL;
    if (open_workspace(&db, home, sizeof home, dbpath, sizeof dbpath, 0, NULL) != 0)
        return 1;

    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "limit", 20);
    cJSON *result = call_op(db, "event.list", params);
    cJSON_Delete(params);
    if (result == NULL) { ipman_db_close(db); return 1; }

    cli_table_t t;
    const char *headers[] = {"When", "Event", "Entity", "Actor"};
    cli_table_init(&t, 4, headers);

    cJSON *events = cJSON_GetObjectItemCaseSensitive(result, "events");
    cJSON *ev;
    cJSON_ArrayForEach(ev, events) {
        cJSON *when        = cJSON_GetObjectItemCaseSensitive(ev, "event_at");
        cJSON *event_type  = cJSON_GetObjectItemCaseSensitive(ev, "event_type");
        cJSON *entity_type = cJSON_GetObjectItemCaseSensitive(ev, "entity_type");
        cJSON *entity_id   = cJSON_GetObjectItemCaseSensitive(ev, "entity_id");
        cJSON *actor       = cJSON_GetObjectItemCaseSensitive(ev, "actor");

        char entity_str[64] = "";
        if (cJSON_IsString(entity_type) && cJSON_IsNumber(entity_id))
            snprintf(entity_str, sizeof entity_str, "%s/%d",
                     entity_type->valuestring, (int)entity_id->valuedouble);

        /* Trim the timestamp to date+time without sub-seconds */
        char when_str[32] = "";
        if (cJSON_IsString(when)) {
            snprintf(when_str, sizeof when_str, "%.19s", when->valuestring);
        }

        const char *row[] = {
            when_str,
            (event_type  && cJSON_IsString(event_type))  ? event_type->valuestring  : "",
            entity_str,
            (actor       && cJSON_IsString(actor))        ? actor->valuestring       : "",
        };
        cli_table_add_row(&t, row);
    }

    cli_table_print(&t, stdout);
    cli_table_free(&t);
    cJSON_Delete(result);
    ipman_db_close(db);
    return 0;
}

int main(int argc, char **argv) {
    const char *cmd = (argc >= 2) ? parse_command(argv[1]) : NULL;

    if (cmd == NULL && argc >= 2) {
        fprintf(stderr, "unknown command: %s\nRun `ipman --usage` for help.\n", argv[1]);
        return 1;
    }
    if (cmd != NULL && strcmp(cmd, "usage")  == 0) { print_usage(stdout); return 0; }
    if (cmd != NULL && strcmp(cmd, "init")   == 0) { return run_init(); }
    if (cmd != NULL && strcmp(cmd, "migrate-encrypt") == 0) { return run_migrate_encrypt(); }
    if (cmd != NULL && strcmp(cmd, "export") == 0) { return run_export(argc, argv); }
    if (cmd != NULL && strcmp(cmd, "sql")    == 0) { return run_sql_cmd(argc, argv); }
    if (cmd != NULL && strcmp(cmd, "status") == 0) { return run_status(); }
    if (cmd != NULL && strcmp(cmd, "ls")     == 0) { return run_ls(); }
    if (cmd != NULL && strcmp(cmd, "log")    == 0) { return run_log(); }
    if (cmd != NULL && strcmp(cmd, "render") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: ipman --render <plan>\n");
            return 1;
        }
        return run_render(argv[2]);
    }
    if (cmd != NULL && strcmp(cmd, "show") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: ipman --show <uid|label|id>\n");
            return 1;
        }
        return run_show(argv[2]);
    }
    if (cmd != NULL && strcmp(cmd, "b64") == 0) {
        g_b64_mode = 1;
    }
    if (isatty(STDIN_FILENO)) {
        print_usage(stdout);
        return 0;
    }

    /* ---- open initialized workspace + migrations ------------------ */
    char home[PATH_MAX];
    char dbpath[PATH_MAX];
    sqlite3 *db = NULL;
    int schema_version = 0;
    int workspace_rc = open_workspace(&db, home, sizeof home,
                                      dbpath, sizeof dbpath,
                                      0, &schema_version);
    if (workspace_rc != 0) return workspace_rc;

    /* ---- read stdin ---------------------------------------------- */
    char   *body = NULL;
    size_t  body_len = 0;
    size_t  max_bytes = resolve_max_request_bytes();
    int     read_rc = ipman_read_all(stdin, max_bytes, &body, &body_len);
    if (read_rc == -2) {
        cJSON *details = cJSON_CreateObject();
        if (details != NULL) {
            cJSON_AddNumberToObject(details, "limit_bytes", (double)max_bytes);
            cJSON_AddNumberToObject(details, "received_bytes", (double)body_len);
        }
        char msg[160];
        snprintf(msg, sizeof msg,
                 "request body exceeds %zu-byte limit", max_bytes);
        cJSON *resp = ipman_response_err(NULL, IPMAN_ERR_INVALID_REQUEST,
                                        msg, details);
        emit_response(resp);
        if (resp) cJSON_Delete(resp);
        ipman_db_close(db);
        return 1;
    }
    if (read_rc != 0) {
        ipman_db_close(db);
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL, "failed to read stdin");
    }
    if (g_b64_mode) {
        char *decoded = NULL;
        size_t decoded_len = 0;
        if (ipman_base64_decode(body, body_len, &decoded, &decoded_len) != 0) {
            free(body);
            ipman_db_close(db);
            return emit_fatal(NULL, IPMAN_ERR_INVALID_REQUEST,
                              "invalid base64 input");
        }
        free(body);
        body = decoded;
        body_len = decoded_len;
    }

    /* ---- parse --------------------------------------------------- */
    ipman_request_t     req;
    ipman_error_code_t  perr_code = IPMAN_ERR_INVALID_REQUEST;
    const char        *perr_msg  = NULL;
    char              *parsed_id = NULL;
    int rc = ipman_request_parse(body, &req, &perr_code, &perr_msg, &parsed_id);
    free(body);

    if (rc != 0) {
        ipman_log_warn("parse failed",
                      "code=%s request_id=%s msg=\"%s\"",
                      ipman_error_code_str(perr_code),
                      parsed_id ? parsed_id : "(null)",
                      perr_msg ? perr_msg : "");
        cJSON *resp = ipman_response_err(parsed_id, perr_code, perr_msg, NULL);
        emit_response(resp);
        if (resp) cJSON_Delete(resp);
        free(parsed_id);
        ipman_db_close(db);
        return ipman_error_is_fatal(perr_code) ? 1 : 0;
    }
    free(parsed_id); /* we have req.request_id borrowed from req.root now */

    /* ---- dispatch ------------------------------------------------- */
    ipman_log_info("request",
                  "op=%s request_id=%s actor=%s",
                  req.op, req.request_id, req.actor);

    cJSON *resp = NULL;
    int disp_rc = ipman_dispatch(&req, db, &resp);
    emit_response(resp);
    if (resp) cJSON_Delete(resp);

    cJSON_Delete(req.root);
    ipman_db_close(db);

    (void)schema_version;
    return disp_rc == -2 ? 1 : 0;
}
