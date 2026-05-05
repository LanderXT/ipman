/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 *
 * Unit tests for src/cli_selector.c — the v2.1 shortcuts/views selector
 * resolver. Each case bootstraps a hermetic ipman workspace under a
 * temp IPMAN_HOME, populates a small fixture via the v2 protocol, then
 * exercises one resolver path.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cJSON.h"
#include "cli_selector.h"
#include "db.h"
#include "dispatch.h"
#include "ipman_home.h"
#include "ipman_key.h"
#include "migrations.h"
#include "protocol.h"

/* ------------------------------------------------------------------ */
/* Test harness — assert macros, counters, and bootstrap helpers.     */
/* ------------------------------------------------------------------ */

static int g_failures = 0;
static const char *g_case = "(none)";

#define FAIL(msg) do { \
    fprintf(stderr, "  FAIL [%s] %s (at %s:%d)\n", \
            g_case, (msg), __FILE__, __LINE__); \
    g_failures++; \
    return; \
} while (0)

#define FAILF(fmt, ...) do { \
    fprintf(stderr, "  FAIL [%s] " fmt " (at %s:%d)\n", \
            g_case, __VA_ARGS__, __FILE__, __LINE__); \
    g_failures++; \
    return; \
} while (0)

#define ASSERT_EQ_INT(actual, expected) do { \
    long _a = (long)(actual), _e = (long)(expected); \
    if (_a != _e) FAILF("expected %ld, got %ld", _e, _a); \
} while (0)

#define ASSERT_CONTAINS(haystack, needle) do { \
    const char *_h = (haystack), *_n = (needle); \
    if (_h == NULL || strstr(_h, _n) == NULL) \
        FAILF("expected error to contain '%s', got '%s'", _n, _h ? _h : "(null)"); \
} while (0)

/* In-process op dispatch — same shape as src/main.c::call_op but no stderr.
 * Returns the detached `result` cJSON on ok, or NULL on error. */
static cJSON *test_call_op(sqlite3 *db, const char *op, cJSON *params) {
    ipman_request_t req;
    memset(&req, 0, sizeof req);
    req.protocol_version = 1;
    req.request_id       = "test";
    req.actor            = "test";
    req.op               = op;
    req.params           = params;
    req.root             = NULL;

    cJSON *response = NULL;
    ipman_dispatch(&req, db, &response);
    if (response == NULL) return NULL;
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(response, "ok");
    if (!cJSON_IsTrue(ok)) {
        cJSON_Delete(response);
        return NULL;
    }
    cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(response, "result");
    cJSON_Delete(response);
    return result;
}

static long create_plan(sqlite3 *db, const char *title,
                        const char *label, const char *code) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "title", title);
    cJSON_AddStringToObject(p, "label", label);
    if (code != NULL) cJSON_AddStringToObject(p, "code", code);
    cJSON_AddStringToObject(p, "summary", "fixture");
    cJSON *r = test_call_op(db, "plan.create", p);
    cJSON_Delete(p);
    if (r == NULL) return -1;
    cJSON *plan = cJSON_GetObjectItemCaseSensitive(r, "plan");
    cJSON *id   = plan ? cJSON_GetObjectItemCaseSensitive(plan, "id") : NULL;
    long out = cJSON_IsNumber(id) ? (long)id->valuedouble : -1;
    cJSON_Delete(r);
    return out;
}

static long create_phase(sqlite3 *db, long plan_id,
                         const char *title, const char *label) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "plan_id", (double)plan_id);
    cJSON_AddStringToObject(p, "title", title);
    cJSON_AddStringToObject(p, "label", label);
    cJSON *r = test_call_op(db, "phase.create", p);
    cJSON_Delete(p);
    if (r == NULL) return -1;
    cJSON *phase = cJSON_GetObjectItemCaseSensitive(r, "phase");
    cJSON *id    = phase ? cJSON_GetObjectItemCaseSensitive(phase, "id") : NULL;
    long out = cJSON_IsNumber(id) ? (long)id->valuedouble : -1;
    cJSON_Delete(r);
    return out;
}

static long create_task(sqlite3 *db, long plan_id, long phase_id,
                        const char *title, const char *label) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "plan_id",  (double)plan_id);
    cJSON_AddNumberToObject(p, "phase_id", (double)phase_id);
    cJSON_AddStringToObject(p, "title",    title);
    cJSON_AddStringToObject(p, "label",    label);
    cJSON *r = test_call_op(db, "task.create", p);
    cJSON_Delete(p);
    if (r == NULL) return -1;
    cJSON *task = cJSON_GetObjectItemCaseSensitive(r, "task");
    cJSON *id   = task ? cJSON_GetObjectItemCaseSensitive(task, "id") : NULL;
    long out = cJSON_IsNumber(id) ? (long)id->valuedouble : -1;
    cJSON_Delete(r);
    return out;
}

static int activate_plan(sqlite3 *db, long plan_id) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "id", (double)plan_id);
    cJSON *r = test_call_op(db, "plan.activate", p);
    cJSON_Delete(p);
    if (r == NULL) return -1;
    cJSON_Delete(r);
    return 0;
}

static int deactivate_plan(sqlite3 *db) {
    cJSON *p = cJSON_CreateObject();
    cJSON *r = test_call_op(db, "plan.deactivate", p);
    cJSON_Delete(p);
    if (r == NULL) return -1;
    cJSON_Delete(r);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Fixture — created once, ids referenced from each case.             */
/* ------------------------------------------------------------------ */

typedef struct {
    long plan_id;
    long phase_id;        /* label "alpha-phase" */
    long task_id;         /* label "alpha-task"  */
    long phase_shared_id; /* label "shared"      */
    long task_shared_id;  /* label "shared"      */
} fixture_t;

static int build_fixture(sqlite3 *db, fixture_t *f) {
    f->plan_id = create_plan(db, "Test Plan", "test-plan", "TP-1");
    if (f->plan_id < 0) { fprintf(stderr, "plan.create failed\n"); return -1; }

    f->phase_id = create_phase(db, f->plan_id, "Alpha Phase", "alpha-phase");
    if (f->phase_id < 0) { fprintf(stderr, "phase.create alpha failed\n"); return -1; }

    f->task_id = create_task(db, f->plan_id, f->phase_id, "Alpha Task", "alpha-task");
    if (f->task_id < 0) { fprintf(stderr, "task.create alpha failed\n"); return -1; }

    f->phase_shared_id = create_phase(db, f->plan_id, "Shared", "shared");
    if (f->phase_shared_id < 0) { fprintf(stderr, "phase.create shared failed\n"); return -1; }

    f->task_shared_id = create_task(db, f->plan_id, f->phase_id, "Shared Task", "shared");
    if (f->task_shared_id < 0) { fprintf(stderr, "task.create shared failed\n"); return -1; }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Cases.                                                             */
/* ------------------------------------------------------------------ */

static void case_uid_existing(sqlite3 *db, const fixture_t *f) {
    g_case = "uid_existing";
    char uid[32], err[CLI_SELECTOR_ERR_LEN];
    snprintf(uid, sizeof uid, "task_%ld", f->task_id);
    long id = 0;
    cli_selector_kind_t k = 0;
    int rc = cli_resolve_selector(db, uid, CLI_SELECTOR_KIND_TASK,
                                  &id, &k, err, sizeof err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(id, f->task_id);
    ASSERT_EQ_INT(k, CLI_SELECTOR_KIND_TASK);
}

static void case_uid_not_found(sqlite3 *db, const fixture_t *f) {
    g_case = "uid_not_found";
    (void)f;
    char err[CLI_SELECTOR_ERR_LEN];
    long id = 0;
    cli_selector_kind_t k = 0;
    int rc = cli_resolve_selector(db, "task_999999",
                                  CLI_SELECTOR_KIND_TASK,
                                  &id, &k, err, sizeof err);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_CONTAINS(err, "not found");
}

static void case_uid_kind_mismatch(sqlite3 *db, const fixture_t *f) {
    g_case = "uid_kind_mismatch";
    char uid[32], err[CLI_SELECTOR_ERR_LEN];
    snprintf(uid, sizeof uid, "phase_%ld", f->phase_id);
    long id = 0;
    cli_selector_kind_t k = 0;
    /* Caller wants TASK only; user passed a phase uid. */
    int rc = cli_resolve_selector(db, uid, CLI_SELECTOR_KIND_TASK,
                                  &id, &k, err, sizeof err);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_CONTAINS(err, "phase");
    ASSERT_CONTAINS(err, "task");
}

static void case_numeric_passthrough_single_kind(sqlite3 *db, const fixture_t *f) {
    g_case = "numeric_passthrough_single_kind";
    char arg[16], err[CLI_SELECTOR_ERR_LEN];
    snprintf(arg, sizeof arg, "%ld", f->task_id);
    long id = 0;
    cli_selector_kind_t k = 0;
    int rc = cli_resolve_selector(db, arg, CLI_SELECTOR_KIND_TASK,
                                  &id, &k, err, sizeof err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(id, f->task_id);
    ASSERT_EQ_INT(k, CLI_SELECTOR_KIND_TASK);
}

static void case_numeric_probe_finds_phase(sqlite3 *db, const fixture_t *f) {
    g_case = "numeric_probe_finds_phase";
    char arg[16], err[CLI_SELECTOR_ERR_LEN];
    snprintf(arg, sizeof arg, "%ld", f->phase_id);
    long id = 0;
    cli_selector_kind_t k = 0;
    /* Numeric ids are independent across tables. With TASK_OR_PHASE we
     * probe in TASK > PHASE order; this id likely matches both tables
     * (sqlite autoincrement is per-table). The probe still resolves to
     * one kind — we only assert the call succeeds and returns a kind in
     * the expected mask. */
    int rc = cli_resolve_selector(db, arg, CLI_SELECTOR_KIND_TASK_OR_PHASE,
                                  &id, &k, err, sizeof err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(id, f->phase_id);
    if (k != CLI_SELECTOR_KIND_TASK && k != CLI_SELECTOR_KIND_PHASE)
        FAILF("kind not in TASK_OR_PHASE: 0x%x", (unsigned)k);
}

static void case_numeric_not_found(sqlite3 *db, const fixture_t *f) {
    g_case = "numeric_not_found";
    (void)f;
    char err[CLI_SELECTOR_ERR_LEN];
    long id = 0;
    cli_selector_kind_t k = 0;
    int rc = cli_resolve_selector(db, "999999",
                                  CLI_SELECTOR_KIND_TASK_OR_PHASE,
                                  &id, &k, err, sizeof err);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_CONTAINS(err, "999999");
}

static void case_label_no_active_plan(sqlite3 *db, const fixture_t *f) {
    g_case = "label_no_active_plan";
    (void)f;
    if (deactivate_plan(db) != 0) FAIL("deactivate_plan setup failed");
    char err[CLI_SELECTOR_ERR_LEN];
    long id = 0;
    cli_selector_kind_t k = 0;
    int rc = cli_resolve_selector(db, "alpha-task",
                                  CLI_SELECTOR_KIND_TASK,
                                  &id, &k, err, sizeof err);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_CONTAINS(err, "active plan");
}

static void case_label_with_active_plan(sqlite3 *db, const fixture_t *f) {
    g_case = "label_with_active_plan";
    if (activate_plan(db, f->plan_id) != 0) FAIL("activate_plan setup failed");
    char err[CLI_SELECTOR_ERR_LEN];
    long id = 0;
    cli_selector_kind_t k = 0;
    int rc = cli_resolve_selector(db, "alpha-task",
                                  CLI_SELECTOR_KIND_TASK,
                                  &id, &k, err, sizeof err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(id, f->task_id);
    ASSERT_EQ_INT(k, CLI_SELECTOR_KIND_TASK);
}

static void case_plan_code_selector(sqlite3 *db, const fixture_t *f) {
    g_case = "plan_code_selector";
    char err[CLI_SELECTOR_ERR_LEN];
    long id = 0;
    cli_selector_kind_t k = 0;
    int rc = cli_resolve_selector(db, "TP-1",
                                  CLI_SELECTOR_KIND_PLAN,
                                  &id, &k, err, sizeof err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(id, f->plan_id);
    ASSERT_EQ_INT(k, CLI_SELECTOR_KIND_PLAN);
}

static void case_label_phase_with_active_plan(sqlite3 *db, const fixture_t *f) {
    g_case = "label_phase_with_active_plan";
    if (activate_plan(db, f->plan_id) != 0) FAIL("activate_plan setup failed");
    char err[CLI_SELECTOR_ERR_LEN];
    long id = 0;
    cli_selector_kind_t k = 0;
    int rc = cli_resolve_selector(db, "alpha-phase",
                                  CLI_SELECTOR_KIND_PHASE,
                                  &id, &k, err, sizeof err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(id, f->phase_id);
    ASSERT_EQ_INT(k, CLI_SELECTOR_KIND_PHASE);
}

static void case_label_ambiguity_task_wins(sqlite3 *db, const fixture_t *f) {
    g_case = "label_ambiguity_task_wins";
    if (activate_plan(db, f->plan_id) != 0) FAIL("activate_plan setup failed");
    char err[CLI_SELECTOR_ERR_LEN];
    long id = 0;
    cli_selector_kind_t k = 0;
    /* Both phase and task have label "shared". TASK_OR_PHASE must
     * resolve to the task per documented precedence (TASK > PHASE). */
    int rc = cli_resolve_selector(db, "shared",
                                  CLI_SELECTOR_KIND_TASK_OR_PHASE,
                                  &id, &k, err, sizeof err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(id, f->task_shared_id);
    ASSERT_EQ_INT(k, CLI_SELECTOR_KIND_TASK);
}

static void case_label_ambiguity_phase_when_filtered(sqlite3 *db,
                                                     const fixture_t *f) {
    g_case = "label_ambiguity_phase_when_filtered";
    if (activate_plan(db, f->plan_id) != 0) FAIL("activate_plan setup failed");
    char err[CLI_SELECTOR_ERR_LEN];
    long id = 0;
    cli_selector_kind_t k = 0;
    /* Same label, but this caller only accepts PHASE — must skip the
     * task and resolve to the phase. */
    int rc = cli_resolve_selector(db, "shared",
                                  CLI_SELECTOR_KIND_PHASE,
                                  &id, &k, err, sizeof err);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(id, f->phase_shared_id);
    ASSERT_EQ_INT(k, CLI_SELECTOR_KIND_PHASE);
}

static void case_label_unknown(sqlite3 *db, const fixture_t *f) {
    g_case = "label_unknown";
    if (activate_plan(db, f->plan_id) != 0) FAIL("activate_plan setup failed");
    char err[CLI_SELECTOR_ERR_LEN];
    long id = 0;
    cli_selector_kind_t k = 0;
    int rc = cli_resolve_selector(db, "no-such-label",
                                  CLI_SELECTOR_KIND_TASK,
                                  &id, &k, err, sizeof err);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_CONTAINS(err, "no-such-label");
}

static void case_invalid_args(sqlite3 *db, const fixture_t *f) {
    g_case = "invalid_args";
    (void)f;
    char err[CLI_SELECTOR_ERR_LEN];
    long id = 0;
    cli_selector_kind_t k = 0;
    int rc = cli_resolve_selector(db, "", CLI_SELECTOR_KIND_TASK,
                                  &id, &k, err, sizeof err);
    ASSERT_EQ_INT(rc, -1);
    ASSERT_CONTAINS(err, "invalid");
}

/* ------------------------------------------------------------------ */
/* main — bootstrap workspace, run all cases, report.                 */
/* ------------------------------------------------------------------ */

static int bootstrap_workspace(char *home, size_t home_size, sqlite3 **db_out) {
    char tmpl[] = "/tmp/ipman_clisel_XXXXXX";
    char *home_dir = mkdtemp(tmpl);
    if (home_dir == NULL) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return -1;
    }
    /* mkdtemp creates 0700 already; copy into caller buffer. */
    snprintf(home, home_size, "%s", home_dir);
    if (setenv("IPMAN_HOME", home, 1) != 0) {
        fprintf(stderr, "setenv IPMAN_HOME failed\n"); return -1;
    }

    if (ipman_home_ensure(home) != 0) {
        fprintf(stderr, "ipman_home_ensure failed\n"); return -1;
    }
    if (ipman_keysalt_ensure(home) != 0) {
        fprintf(stderr, "ipman_keysalt_ensure failed\n"); return -1;
    }

    char dbpath[PATH_MAX];
    snprintf(dbpath, sizeof dbpath, "%s/ipman.db", home);

    sqlite3 *db = NULL;
    if (ipman_db_open(home, dbpath, &db) != 0) {
        fprintf(stderr, "ipman_db_open failed\n"); return -1;
    }
    int version = 0;
    if (ipman_migrations_apply(db, NULL, &version) != 0) {
        fprintf(stderr, "ipman_migrations_apply failed\n");
        ipman_db_close(db);
        return -1;
    }
    *db_out = db;
    return 0;
}

static void cleanup_workspace(const char *home) {
    /* mkdtemp dir + key/db; safe rm -rf on a /tmp prefix. */
    if (home == NULL || home[0] == '\0') return;
    if (strncmp(home, "/tmp/ipman_clisel_", 18) != 0) return;
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", home);
    int rc = system(cmd);
    (void)rc;
}

int main(void) {
    char home[PATH_MAX] = "";
    sqlite3 *db = NULL;
    if (bootstrap_workspace(home, sizeof home, &db) != 0) return 2;

    fixture_t f;
    if (build_fixture(db, &f) != 0) {
        ipman_db_close(db);
        cleanup_workspace(home);
        return 2;
    }

    /* Cases that depend on no-active-plan run first. */
    case_uid_existing(db, &f);
    case_uid_not_found(db, &f);
    case_uid_kind_mismatch(db, &f);
    case_numeric_passthrough_single_kind(db, &f);
    case_numeric_probe_finds_phase(db, &f);
    case_numeric_not_found(db, &f);
    case_label_no_active_plan(db, &f);

    /* Re-activate plan for the rest. */
    case_label_with_active_plan(db, &f);
    case_plan_code_selector(db, &f);
    case_label_phase_with_active_plan(db, &f);
    case_label_ambiguity_task_wins(db, &f);
    case_label_ambiguity_phase_when_filtered(db, &f);
    case_label_unknown(db, &f);
    case_invalid_args(db, &f);

    ipman_db_close(db);
    cleanup_workspace(home);

    if (g_failures > 0) {
        fprintf(stderr, "test_cli_selector: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_cli_selector: all cases passed\n");
    return 0;
}
