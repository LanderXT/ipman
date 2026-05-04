/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

/*
 * ipman_import_plan_envelope — CLI shortcut orchestrator for --import-plan.
 *
 * Reads a JSON file produced by plan.export, validates the top-level shape,
 * then drives a sequence of standard ops through the in-process dispatch path:
 *
 *   plan.create
 *   phase.create  (one per phase, in array order)
 *   task.create   (one per task, in array order; phase_id and parent_task_id remapped)
 *   comment.add   (plan/phase/task scopes only; entity_id remapped)
 *   instruction.add (plan/phase/task scopes only; project-scoped entries skipped)
 *   task.link_dependency (from_task_id and to_task_id remapped)
 *
 * Events, closures, and project-scoped instructions are skipped — events are
 * auto-emitted by the new ops; closures bind to events; project-scoped
 * instructions belong to the workspace, not the plan.
 *
 * All entities are imported in creation-default state (open/todo).
 *
 * Rollback (saga / compensating actions):
 *   On failure after plan.create succeeds, a single goto cleanup path calls
 *   task.cancel / phase.close(outcome=canceled) / plan.close(outcome=canceled)
 *   in reverse order to leave the workspace clean.
 */

#include "import_ops.h"
#include "dispatch.h"
#include "protocol.h"

#include <cJSON.h>
#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/*
 * Read an entire file into a NUL-terminated heap string.
 * Returns 0 on success (caller frees *out), -1 on failure.
 */
static int read_file(const char *path, char **out) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (buf == NULL) { fclose(f); return -1; }

    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if ((long)n != sz) { free(buf); return -1; }
    buf[sz] = '\0';
    *out = buf;
    return 0;
}

/*
 * In-process dispatch: build a synthetic request, call ipman_dispatch,
 * check ok:true.
 * Returns a detached "result" cJSON on success (caller owns and must free),
 * or NULL on failure (error message already printed to stderr by the caller
 * after this function returns — we just return NULL).
 */
static cJSON *dispatch_op(sqlite3 *db, const char *op, cJSON *params) {
    ipman_request_t req;
    memset(&req, 0, sizeof req);
    req.protocol_version = 2;
    req.request_id       = "import";
    req.actor            = "import";
    req.op               = op;
    req.params           = params;
    req.root             = NULL;

    cJSON *response = NULL;
    ipman_dispatch(&req, db, &response);
    if (response == NULL) return NULL;

    cJSON *ok_item = cJSON_GetObjectItemCaseSensitive(response, "ok");
    if (!cJSON_IsTrue(ok_item)) {
        /* Let the caller format a more contextual error; just return NULL. */
        cJSON_Delete(response);
        return NULL;
    }

    cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(response, "result");
    cJSON_Delete(response);
    return result;
}

/*
 * Like dispatch_op but also extracts error message into *err_msg_out.
 * The returned message is a heap string (caller frees).
 */
static cJSON *dispatch_op_err(sqlite3 *db, const char *op, cJSON *params,
                              char **err_msg_out) {
    ipman_request_t req;
    memset(&req, 0, sizeof req);
    req.protocol_version = 2;
    req.request_id       = "import";
    req.actor            = "import";
    req.op               = op;
    req.params           = params;
    req.root             = NULL;

    cJSON *response = NULL;
    ipman_dispatch(&req, db, &response);
    if (response == NULL) {
        if (err_msg_out)
            *err_msg_out = strdup("internal error: dispatch returned NULL");
        return NULL;
    }

    cJSON *ok_item = cJSON_GetObjectItemCaseSensitive(response, "ok");
    if (!cJSON_IsTrue(ok_item)) {
        if (err_msg_out) {
            cJSON *err  = cJSON_GetObjectItemCaseSensitive(response, "error");
            cJSON *msg  = err ? cJSON_GetObjectItemCaseSensitive(err, "message") : NULL;
            const char *s = (msg && cJSON_IsString(msg))
                            ? msg->valuestring : "unknown error";
            *err_msg_out = strdup(s);
        }
        cJSON_Delete(response);
        return NULL;
    }

    cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(response, "result");
    cJSON_Delete(response);
    return result;
}

/* ------------------------------------------------------------------ */
/* ID remap                                                           */
/* ------------------------------------------------------------------ */

/*
 * Simple flat remap: a pair of parallel arrays mapping old ids to new ids.
 * We allocate REMAP_INIT slots and double as needed.
 */
#define REMAP_INIT 64

typedef struct {
    sqlite3_int64 *old_ids;
    sqlite3_int64 *new_ids;
    int            count;
    int            cap;
} id_remap_t;

static int remap_init(id_remap_t *r) {
    r->old_ids = malloc(REMAP_INIT * sizeof(sqlite3_int64));
    r->new_ids = malloc(REMAP_INIT * sizeof(sqlite3_int64));
    if (r->old_ids == NULL || r->new_ids == NULL) {
        free(r->old_ids); free(r->new_ids);
        r->old_ids = r->new_ids = NULL;
        return -1;
    }
    r->count = 0;
    r->cap   = REMAP_INIT;
    return 0;
}

static void remap_free(id_remap_t *r) {
    free(r->old_ids);
    free(r->new_ids);
    r->old_ids = r->new_ids = NULL;
    r->count = r->cap = 0;
}

static int remap_add(id_remap_t *r, sqlite3_int64 old_id,
                     sqlite3_int64 new_id) {
    if (r->count >= r->cap) {
        int new_cap = r->cap * 2;
        sqlite3_int64 *oa = realloc(r->old_ids,
                                    (size_t)new_cap * sizeof(sqlite3_int64));
        sqlite3_int64 *na = realloc(r->new_ids,
                                    (size_t)new_cap * sizeof(sqlite3_int64));
        if (oa == NULL || na == NULL) {
            /* On partial realloc failure: free whichever succeeded */
            if (oa != NULL) r->old_ids = oa;
            if (na != NULL) r->new_ids = na;
            return -1;
        }
        r->old_ids = oa;
        r->new_ids = na;
        r->cap     = new_cap;
    }
    r->old_ids[r->count] = old_id;
    r->new_ids[r->count] = new_id;
    r->count++;
    return 0;
}

/* Returns the remapped new id, or 0 if old_id is not in the map. */
static sqlite3_int64 remap_lookup(const id_remap_t *r, sqlite3_int64 old_id) {
    for (int i = 0; i < r->count; i++) {
        if (r->old_ids[i] == old_id) return r->new_ids[i];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Rollback helpers                                                   */
/* ------------------------------------------------------------------ */

/*
 * Attempt to cancel a task that was created by this import. Failures are
 * logged to stderr but do not stop the cleanup loop.
 */
static void rollback_task(sqlite3 *db, sqlite3_int64 task_id) {
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        fprintf(stderr, "import: rollback OOM canceling task %lld\n",
                (long long)task_id);
        return;
    }
    cJSON_AddNumberToObject(p, "id",              (double)task_id);
    cJSON_AddStringToObject(p, "resolution",      "canceled");
    cJSON_AddStringToObject(p, "outcome_summary", "import rollback");
    cJSON_AddStringToObject(p, "closing_comment", "import failed; rolling back");
    cJSON *r = dispatch_op(db, "task.cancel", p);
    cJSON_Delete(p);
    if (r == NULL) {
        fprintf(stderr, "import: rollback failed canceling task %lld"
                " (manual cleanup may be needed)\n", (long long)task_id);
    } else {
        cJSON_Delete(r);
    }
}

/*
 * Attempt to close (cancel) a phase that was created by this import.
 */
static void rollback_phase(sqlite3 *db, sqlite3_int64 phase_id) {
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        fprintf(stderr, "import: rollback OOM canceling phase %lld\n",
                (long long)phase_id);
        return;
    }
    cJSON_AddNumberToObject(p, "id",              (double)phase_id);
    cJSON_AddStringToObject(p, "outcome",         "canceled");
    cJSON_AddStringToObject(p, "outcome_summary", "import rollback");
    cJSON_AddStringToObject(p, "closing_comment", "import failed; rolling back");
    cJSON *r = dispatch_op(db, "phase.close", p);
    cJSON_Delete(p);
    if (r == NULL) {
        fprintf(stderr, "import: rollback failed canceling phase %lld"
                " (manual cleanup may be needed)\n", (long long)phase_id);
    } else {
        cJSON_Delete(r);
    }
}

/*
 * Attempt to close (cancel) the plan that was created by this import.
 */
static void rollback_plan(sqlite3 *db, sqlite3_int64 plan_id) {
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        fprintf(stderr, "import: rollback OOM canceling plan %lld\n",
                (long long)plan_id);
        return;
    }
    cJSON_AddNumberToObject(p, "id",              (double)plan_id);
    cJSON_AddStringToObject(p, "outcome",         "canceled");
    cJSON_AddStringToObject(p, "outcome_summary", "import rollback");
    cJSON_AddStringToObject(p, "closing_comment", "import failed; rolling back");
    cJSON *r = dispatch_op(db, "plan.close", p);
    cJSON_Delete(p);
    if (r == NULL) {
        fprintf(stderr, "import: rollback failed canceling plan %lld"
                " (manual cleanup may be needed)\n", (long long)plan_id);
    } else {
        cJSON_Delete(r);
    }
}

/* ------------------------------------------------------------------ */
/* Per-section importers                                              */
/* ------------------------------------------------------------------ */

/*
 * Import a single phase. Returns the new phase_id on success or 0 on failure.
 * On failure, *err_msg_out is set (caller frees).
 */
static sqlite3_int64 import_phase(sqlite3 *db, cJSON *phase_obj,
                                  sqlite3_int64 new_plan_id,
                                  char **err_msg_out) {
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        *err_msg_out = strdup("out of memory building phase params");
        return 0;
    }

    cJSON_AddNumberToObject(p, "plan_id", (double)new_plan_id);

    /* Required: title */
    cJSON *title = cJSON_GetObjectItemCaseSensitive(phase_obj, "title");
    if (!cJSON_IsString(title) || title->valuestring == NULL) {
        cJSON_Delete(p);
        *err_msg_out = strdup("phase missing required 'title' field");
        return 0;
    }
    cJSON_AddStringToObject(p, "title", title->valuestring);

    /* Optional fields */
    cJSON *summary = cJSON_GetObjectItemCaseSensitive(phase_obj, "summary");
    if (cJSON_IsString(summary) && summary->valuestring != NULL)
        cJSON_AddStringToObject(p, "summary", summary->valuestring);

    cJSON *description = cJSON_GetObjectItemCaseSensitive(phase_obj, "description");
    if (cJSON_IsString(description) && description->valuestring != NULL)
        cJSON_AddStringToObject(p, "description", description->valuestring);

    cJSON *owner = cJSON_GetObjectItemCaseSensitive(phase_obj, "owner");
    if (cJSON_IsString(owner) && owner->valuestring != NULL)
        cJSON_AddStringToObject(p, "owner", owner->valuestring);

    cJSON *tstart = cJSON_GetObjectItemCaseSensitive(phase_obj, "target_start_date");
    if (cJSON_IsString(tstart) && tstart->valuestring != NULL)
        cJSON_AddStringToObject(p, "target_start_date", tstart->valuestring);

    cJSON *tend = cJSON_GetObjectItemCaseSensitive(phase_obj, "target_end_date");
    if (cJSON_IsString(tend) && tend->valuestring != NULL)
        cJSON_AddStringToObject(p, "target_end_date", tend->valuestring);

    cJSON *label = cJSON_GetObjectItemCaseSensitive(phase_obj, "label");
    if (cJSON_IsString(label) && label->valuestring != NULL)
        cJSON_AddStringToObject(p, "label", label->valuestring);

    /* sequence_no: preserve from export */
    cJSON *seq = cJSON_GetObjectItemCaseSensitive(phase_obj, "sequence_no");
    if (cJSON_IsNumber(seq) && seq->valuedouble >= 1.0)
        cJSON_AddNumberToObject(p, "sequence_no", seq->valuedouble);

    /* phase.create requires status=open — that's the default, but be explicit */
    cJSON_AddStringToObject(p, "status", "open");

    cJSON *result = dispatch_op_err(db, "phase.create", p, err_msg_out);
    cJSON_Delete(p);
    if (result == NULL) return 0;

    cJSON *phase = cJSON_GetObjectItemCaseSensitive(result, "phase");
    cJSON *id    = phase ? cJSON_GetObjectItemCaseSensitive(phase, "id") : NULL;
    sqlite3_int64 new_id = (cJSON_IsNumber(id)) ? (sqlite3_int64)id->valuedouble : 0;
    cJSON_Delete(result);

    if (new_id <= 0) {
        *err_msg_out = strdup("phase.create returned unexpected result shape");
        return 0;
    }
    return new_id;
}

/*
 * Import a single task. Returns the new task_id on success or 0 on failure.
 */
static sqlite3_int64 import_task(sqlite3 *db, cJSON *task_obj,
                                 sqlite3_int64 new_plan_id,
                                 const id_remap_t *phase_remap,
                                 const id_remap_t *task_remap,
                                 char **err_msg_out) {
    cJSON *p = cJSON_CreateObject();
    if (p == NULL) {
        *err_msg_out = strdup("out of memory building task params");
        return 0;
    }

    cJSON_AddNumberToObject(p, "plan_id", (double)new_plan_id);

    /* Required: title */
    cJSON *title = cJSON_GetObjectItemCaseSensitive(task_obj, "title");
    if (!cJSON_IsString(title) || title->valuestring == NULL) {
        cJSON_Delete(p);
        *err_msg_out = strdup("task missing required 'title' field");
        return 0;
    }
    cJSON_AddStringToObject(p, "title", title->valuestring);

    /* phase_id: remap if present */
    cJSON *phase_id_item = cJSON_GetObjectItemCaseSensitive(task_obj, "phase_id");
    if (cJSON_IsNumber(phase_id_item) && phase_id_item->valuedouble > 0) {
        sqlite3_int64 old_phase_id = (sqlite3_int64)phase_id_item->valuedouble;
        sqlite3_int64 new_phase_id = remap_lookup(phase_remap, old_phase_id);
        if (new_phase_id > 0) {
            cJSON_AddNumberToObject(p, "phase_id", (double)new_phase_id);
        }
        /* If remap not found, skip phase_id — orphaned task gets no phase */
    }

    /* parent_task_id: remap if present */
    cJSON *parent_id_item = cJSON_GetObjectItemCaseSensitive(task_obj, "parent_task_id");
    if (cJSON_IsNumber(parent_id_item) && parent_id_item->valuedouble > 0) {
        sqlite3_int64 old_parent = (sqlite3_int64)parent_id_item->valuedouble;
        sqlite3_int64 new_parent = remap_lookup(task_remap, old_parent);
        if (new_parent > 0) {
            cJSON_AddNumberToObject(p, "parent_task_id", (double)new_parent);
        }
    }

    /* Optional fields */
    cJSON *summary = cJSON_GetObjectItemCaseSensitive(task_obj, "summary");
    if (cJSON_IsString(summary) && summary->valuestring != NULL)
        cJSON_AddStringToObject(p, "summary", summary->valuestring);

    cJSON *description = cJSON_GetObjectItemCaseSensitive(task_obj, "description");
    if (cJSON_IsString(description) && description->valuestring != NULL)
        cJSON_AddStringToObject(p, "description", description->valuestring);

    cJSON *priority = cJSON_GetObjectItemCaseSensitive(task_obj, "priority");
    if (cJSON_IsString(priority) && priority->valuestring != NULL)
        cJSON_AddStringToObject(p, "priority", priority->valuestring);

    cJSON *task_type = cJSON_GetObjectItemCaseSensitive(task_obj, "task_type");
    if (cJSON_IsString(task_type) && task_type->valuestring != NULL)
        cJSON_AddStringToObject(p, "task_type", task_type->valuestring);

    cJSON *assignee = cJSON_GetObjectItemCaseSensitive(task_obj, "assignee");
    if (cJSON_IsString(assignee) && assignee->valuestring != NULL)
        cJSON_AddStringToObject(p, "assignee", assignee->valuestring);

    cJSON *due_date = cJSON_GetObjectItemCaseSensitive(task_obj, "due_date");
    if (cJSON_IsString(due_date) && due_date->valuestring != NULL)
        cJSON_AddStringToObject(p, "due_date", due_date->valuestring);

    cJSON *tstart = cJSON_GetObjectItemCaseSensitive(task_obj, "target_start_date");
    if (cJSON_IsString(tstart) && tstart->valuestring != NULL)
        cJSON_AddStringToObject(p, "target_start_date", tstart->valuestring);

    cJSON *estimate = cJSON_GetObjectItemCaseSensitive(task_obj, "estimate");
    if (cJSON_IsString(estimate) && estimate->valuestring != NULL)
        cJSON_AddStringToObject(p, "estimate", estimate->valuestring);

    cJSON *origin_type = cJSON_GetObjectItemCaseSensitive(task_obj, "origin_type");
    if (cJSON_IsString(origin_type) && origin_type->valuestring != NULL)
        cJSON_AddStringToObject(p, "origin_type", origin_type->valuestring);

    cJSON *origin_ref_type = cJSON_GetObjectItemCaseSensitive(task_obj, "origin_ref_type");
    if (cJSON_IsString(origin_ref_type) && origin_ref_type->valuestring != NULL)
        cJSON_AddStringToObject(p, "origin_ref_type", origin_ref_type->valuestring);

    cJSON *origin_ref_id = cJSON_GetObjectItemCaseSensitive(task_obj, "origin_ref_id");
    if (cJSON_IsNumber(origin_ref_id) && origin_ref_id->valuedouble > 0)
        cJSON_AddNumberToObject(p, "origin_ref_id",
                                origin_ref_id->valuedouble);

    cJSON *label = cJSON_GetObjectItemCaseSensitive(task_obj, "label");
    if (cJSON_IsString(label) && label->valuestring != NULL)
        cJSON_AddStringToObject(p, "label", label->valuestring);

    /* task.create requires status=todo — enforce it */
    cJSON_AddStringToObject(p, "status", "todo");

    cJSON *result = dispatch_op_err(db, "task.create", p, err_msg_out);
    cJSON_Delete(p);
    if (result == NULL) return 0;

    cJSON *task = cJSON_GetObjectItemCaseSensitive(result, "task");
    cJSON *id   = task ? cJSON_GetObjectItemCaseSensitive(task, "id") : NULL;
    sqlite3_int64 new_id = (cJSON_IsNumber(id)) ? (sqlite3_int64)id->valuedouble : 0;
    cJSON_Delete(result);

    if (new_id <= 0) {
        *err_msg_out = strdup("task.create returned unexpected result shape");
        return 0;
    }
    return new_id;
}

/*
 * Import comments for the plan tree. Skips invalidated comments. Failures
 * are non-fatal: we log a warning and continue.
 *
 * entity_type is "plan", "phase", or "task". entity_id is the OLD id from
 * the export; we remap it via phase_remap / task_remap.
 */
static void import_comments(sqlite3 *db, cJSON *comments,
                             sqlite3_int64 new_plan_id,
                             const id_remap_t *phase_remap,
                             const id_remap_t *task_remap) {
    if (!cJSON_IsArray(comments)) return;
    cJSON *c;
    cJSON_ArrayForEach(c, comments) {
        /* Skip invalidated */
        cJSON *inv = cJSON_GetObjectItemCaseSensitive(c, "invalidated_at");
        if (cJSON_IsString(inv) && inv->valuestring != NULL) continue;

        cJSON *etype = cJSON_GetObjectItemCaseSensitive(c, "entity_type");
        cJSON *eid   = cJSON_GetObjectItemCaseSensitive(c, "entity_id");
        cJSON *body  = cJSON_GetObjectItemCaseSensitive(c, "body");
        cJSON *ctype = cJSON_GetObjectItemCaseSensitive(c, "comment_type");

        if (!cJSON_IsString(etype) || !cJSON_IsNumber(eid) ||
            !cJSON_IsString(body)  || body->valuestring == NULL) continue;

        sqlite3_int64 old_eid = (sqlite3_int64)eid->valuedouble;
        sqlite3_int64 new_eid = 0;

        const char *et = etype->valuestring;
        if (strcmp(et, "plan") == 0) {
            new_eid = new_plan_id;
        } else if (strcmp(et, "phase") == 0) {
            new_eid = remap_lookup(phase_remap, old_eid);
        } else if (strcmp(et, "task") == 0) {
            new_eid = remap_lookup(task_remap, old_eid);
        } else {
            /* project scope or unknown — skip */
            continue;
        }
        if (new_eid <= 0) continue;

        cJSON *p = cJSON_CreateObject();
        if (p == NULL) continue;
        cJSON_AddStringToObject(p, "entity_type", et);
        cJSON_AddNumberToObject(p, "entity_id",   (double)new_eid);
        cJSON_AddStringToObject(p, "body",         body->valuestring);
        if (cJSON_IsString(ctype) && ctype->valuestring != NULL)
            cJSON_AddStringToObject(p, "comment_type", ctype->valuestring);
        cJSON *r = dispatch_op(db, "comment.add", p);
        cJSON_Delete(p);
        if (r == NULL) {
            fprintf(stderr,
                    "import: warning — failed to import comment for %s/%lld"
                    " (non-fatal)\n", et, (long long)old_eid);
        } else {
            cJSON_Delete(r);
        }
    }
}

/*
 * Import instructions for the plan tree. Skips project-scoped instructions
 * and invalidated entries. Failures are non-fatal.
 */
static void import_instructions(sqlite3 *db, cJSON *instructions,
                                 sqlite3_int64 new_plan_id,
                                 const id_remap_t *phase_remap,
                                 const id_remap_t *task_remap) {
    if (!cJSON_IsArray(instructions)) return;
    cJSON *ins;
    cJSON_ArrayForEach(ins, instructions) {
        /* Skip invalidated */
        cJSON *inv = cJSON_GetObjectItemCaseSensitive(ins, "invalidated_at");
        if (cJSON_IsString(inv) && inv->valuestring != NULL) continue;

        cJSON *etype = cJSON_GetObjectItemCaseSensitive(ins, "entity_type");
        cJSON *eid   = cJSON_GetObjectItemCaseSensitive(ins, "entity_id");
        cJSON *body  = cJSON_GetObjectItemCaseSensitive(ins, "body");
        cJSON *itype = cJSON_GetObjectItemCaseSensitive(ins, "instruction_type");

        if (!cJSON_IsString(etype) || !cJSON_IsNumber(eid) ||
            !cJSON_IsString(body)  || body->valuestring == NULL) continue;

        sqlite3_int64 old_eid = (sqlite3_int64)eid->valuedouble;
        sqlite3_int64 new_eid = 0;

        const char *et = etype->valuestring;
        if (strcmp(et, "plan") == 0) {
            new_eid = new_plan_id;
        } else if (strcmp(et, "phase") == 0) {
            new_eid = remap_lookup(phase_remap, old_eid);
        } else if (strcmp(et, "task") == 0) {
            new_eid = remap_lookup(task_remap, old_eid);
        } else {
            /* project scope or unknown — skip */
            continue;
        }
        if (new_eid <= 0) continue;

        cJSON *p = cJSON_CreateObject();
        if (p == NULL) continue;
        cJSON_AddStringToObject(p, "entity_type", et);
        cJSON_AddNumberToObject(p, "entity_id",   (double)new_eid);
        cJSON_AddStringToObject(p, "body",         body->valuestring);
        if (cJSON_IsString(itype) && itype->valuestring != NULL)
            cJSON_AddStringToObject(p, "instruction_type", itype->valuestring);
        cJSON *r = dispatch_op(db, "instruction.add", p);
        cJSON_Delete(p);
        if (r == NULL) {
            fprintf(stderr,
                    "import: warning — failed to import instruction for %s/%lld"
                    " (non-fatal)\n", et, (long long)old_eid);
        } else {
            cJSON_Delete(r);
        }
    }
}

/*
 * Import task relations. Failures are non-fatal.
 */
static void import_relations(sqlite3 *db, cJSON *relations,
                              const id_remap_t *task_remap) {
    if (!cJSON_IsArray(relations)) return;
    cJSON *rel;
    cJSON_ArrayForEach(rel, relations) {
        cJSON *from   = cJSON_GetObjectItemCaseSensitive(rel, "from_task_id");
        cJSON *to     = cJSON_GetObjectItemCaseSensitive(rel, "to_task_id");
        cJSON *rtype  = cJSON_GetObjectItemCaseSensitive(rel, "relation_type");
        cJSON *notes  = cJSON_GetObjectItemCaseSensitive(rel, "notes");

        if (!cJSON_IsNumber(from) || !cJSON_IsNumber(to) ||
            !cJSON_IsString(rtype) || rtype->valuestring == NULL) continue;

        sqlite3_int64 new_from = remap_lookup(task_remap,
                                              (sqlite3_int64)from->valuedouble);
        sqlite3_int64 new_to   = remap_lookup(task_remap,
                                              (sqlite3_int64)to->valuedouble);
        if (new_from <= 0 || new_to <= 0) continue;
        if (new_from == new_to) continue;

        cJSON *p = cJSON_CreateObject();
        if (p == NULL) continue;
        cJSON_AddNumberToObject(p, "id",             (double)new_from);
        cJSON_AddNumberToObject(p, "target_task_id", (double)new_to);
        cJSON_AddStringToObject(p, "relation_type",  rtype->valuestring);
        if (cJSON_IsString(notes) && notes->valuestring != NULL)
            cJSON_AddStringToObject(p, "notes", notes->valuestring);
        cJSON *r = dispatch_op(db, "task.link_dependency", p);
        cJSON_Delete(p);
        if (r == NULL) {
            fprintf(stderr,
                    "import: warning — failed to import relation %lld->%lld"
                    " (non-fatal)\n",
                    (long long)new_from, (long long)new_to);
        } else {
            cJSON_Delete(r);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main entry point                                                   */
/* ------------------------------------------------------------------ */

int ipman_import_plan_envelope(sqlite3 *db,
                               const char *json_path,
                               sqlite3_int64 *new_plan_id_out,
                               char **err_msg_out) {
    char *file_contents = NULL;
    cJSON *root = NULL;
    id_remap_t phase_remap, task_remap;
    int phase_remap_ok = 0, task_remap_ok = 0;
    sqlite3_int64 new_plan_id = 0;
    int ret = -1;

    /* ---- 1. Read and parse the file -------------------------------- */
    if (read_file(json_path, &file_contents) != 0) {
        if (err_msg_out) {
            char msg[512];
            snprintf(msg, sizeof msg, "cannot open file: %s", json_path);
            *err_msg_out = strdup(msg);
        }
        return -1;
    }

    root = cJSON_Parse(file_contents);
    free(file_contents);
    file_contents = NULL;

    if (root == NULL) {
        if (err_msg_out)
            *err_msg_out = strdup("invalid JSON in import file");
        return -1;
    }

    /* ---- 2. Validate top-level shape ------------------------------- */
    cJSON *export_obj = cJSON_GetObjectItemCaseSensitive(root, "export");
    if (export_obj == NULL) {
        /* Some export envelopes wrap under result.export */
        cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
        if (result != NULL)
            export_obj = cJSON_GetObjectItemCaseSensitive(result, "export");
    }
    /* Also accept the raw export object at the root */
    cJSON *plan_obj = NULL;
    if (export_obj != NULL) {
        plan_obj = cJSON_GetObjectItemCaseSensitive(export_obj, "plan");
    } else {
        plan_obj = cJSON_GetObjectItemCaseSensitive(root, "plan");
        if (plan_obj != NULL) export_obj = root;
    }

    if (plan_obj == NULL || !cJSON_IsObject(plan_obj)) {
        if (err_msg_out)
            *err_msg_out = strdup("export envelope missing 'plan' object");
        cJSON_Delete(root);
        return -1;
    }

    cJSON *phases_arr      = cJSON_GetObjectItemCaseSensitive(export_obj, "phases");
    cJSON *tasks_arr       = cJSON_GetObjectItemCaseSensitive(export_obj, "tasks");
    cJSON *comments_arr    = cJSON_GetObjectItemCaseSensitive(export_obj, "comments");
    cJSON *instructions_arr = cJSON_GetObjectItemCaseSensitive(export_obj, "instructions");
    cJSON *relations_arr   = cJSON_GetObjectItemCaseSensitive(export_obj, "relations");

    /* ---- 3. Initialise remap tables -------------------------------- */
    if (remap_init(&phase_remap) != 0 || remap_init(&task_remap) != 0) {
        if (err_msg_out) *err_msg_out = strdup("out of memory");
        remap_free(&phase_remap);
        remap_free(&task_remap);
        cJSON_Delete(root);
        return -1;
    }
    phase_remap_ok = 1;
    task_remap_ok  = 1;

    /* ---- 4. Create the plan --------------------------------------- */
    {
        cJSON *p = cJSON_CreateObject();
        if (p == NULL) {
            if (err_msg_out) *err_msg_out = strdup("out of memory");
            goto done;
        }

        /* Required: title */
        cJSON *title = cJSON_GetObjectItemCaseSensitive(plan_obj, "title");
        if (!cJSON_IsString(title) || title->valuestring == NULL) {
            cJSON_Delete(p);
            if (err_msg_out)
                *err_msg_out = strdup("plan missing required 'title' field");
            goto done;
        }
        cJSON_AddStringToObject(p, "title", title->valuestring);

        /* Optional fields */
        cJSON *code = cJSON_GetObjectItemCaseSensitive(plan_obj, "code");
        if (cJSON_IsString(code) && code->valuestring != NULL)
            cJSON_AddStringToObject(p, "code", code->valuestring);

        cJSON *summary = cJSON_GetObjectItemCaseSensitive(plan_obj, "summary");
        if (cJSON_IsString(summary) && summary->valuestring != NULL)
            cJSON_AddStringToObject(p, "summary", summary->valuestring);

        cJSON *description = cJSON_GetObjectItemCaseSensitive(plan_obj, "description");
        if (cJSON_IsString(description) && description->valuestring != NULL)
            cJSON_AddStringToObject(p, "description", description->valuestring);

        cJSON *priority = cJSON_GetObjectItemCaseSensitive(plan_obj, "priority");
        if (cJSON_IsString(priority) && priority->valuestring != NULL)
            cJSON_AddStringToObject(p, "priority", priority->valuestring);

        cJSON *owner = cJSON_GetObjectItemCaseSensitive(plan_obj, "owner");
        if (cJSON_IsString(owner) && owner->valuestring != NULL)
            cJSON_AddStringToObject(p, "owner", owner->valuestring);

        cJSON *target_date = cJSON_GetObjectItemCaseSensitive(plan_obj, "target_date");
        if (cJSON_IsString(target_date) && target_date->valuestring != NULL)
            cJSON_AddStringToObject(p, "target_date", target_date->valuestring);

        cJSON *version_label = cJSON_GetObjectItemCaseSensitive(plan_obj, "version_label");
        if (cJSON_IsString(version_label) && version_label->valuestring != NULL)
            cJSON_AddStringToObject(p, "version_label", version_label->valuestring);

        cJSON *tags = cJSON_GetObjectItemCaseSensitive(plan_obj, "tags");
        if (cJSON_IsArray(tags))
            cJSON_AddItemToObject(p, "tags", cJSON_Duplicate(tags, 1));

        cJSON *label = cJSON_GetObjectItemCaseSensitive(plan_obj, "label");
        if (cJSON_IsString(label) && label->valuestring != NULL)
            cJSON_AddStringToObject(p, "label", label->valuestring);

        /* Always create as open */
        cJSON_AddStringToObject(p, "status", "open");

        char *perr = NULL;
        cJSON *result = dispatch_op_err(db, "plan.create", p, &perr);
        cJSON_Delete(p);
        if (result == NULL) {
            if (err_msg_out) {
                char msg[512];
                snprintf(msg, sizeof msg, "plan.create failed: %s",
                         perr ? perr : "unknown");
                *err_msg_out = strdup(msg);
            }
            free(perr);
            goto done;
        }

        cJSON *np  = cJSON_GetObjectItemCaseSensitive(result, "plan");
        cJSON *nid = np ? cJSON_GetObjectItemCaseSensitive(np, "id") : NULL;
        new_plan_id = cJSON_IsNumber(nid) ? (sqlite3_int64)nid->valuedouble : 0;
        cJSON_Delete(result);

        if (new_plan_id <= 0) {
            if (err_msg_out)
                *err_msg_out = strdup("plan.create returned unexpected shape");
            goto done;
        }
    }

    /*
     * plan.create succeeded. From this point on, any failure triggers
     * saga cleanup via goto cleanup.
     */

    /* ---- 5. Create phases ----------------------------------------- */
    if (cJSON_IsArray(phases_arr)) {
        cJSON *phase_obj_item;
        cJSON_ArrayForEach(phase_obj_item, phases_arr) {
            cJSON *old_id_item = cJSON_GetObjectItemCaseSensitive(phase_obj_item, "id");
            if (!cJSON_IsNumber(old_id_item)) {
                if (err_msg_out)
                    *err_msg_out = strdup("phase entry missing 'id' field");
                goto cleanup;
            }
            sqlite3_int64 old_phase_id = (sqlite3_int64)old_id_item->valuedouble;

            char *perr = NULL;
            sqlite3_int64 new_phase_id = import_phase(db, phase_obj_item,
                                                       new_plan_id, &perr);
            if (new_phase_id <= 0) {
                if (err_msg_out) {
                    char msg[512];
                    snprintf(msg, sizeof msg, "phase.create failed: %s",
                             perr ? perr : "unknown");
                    *err_msg_out = strdup(msg);
                }
                free(perr);
                goto cleanup;
            }
            free(perr);

            if (remap_add(&phase_remap, old_phase_id, new_phase_id) != 0) {
                if (err_msg_out) *err_msg_out = strdup("out of memory");
                goto cleanup;
            }
        }
    }

    /* ---- 6. Create tasks ------------------------------------------ */
    if (cJSON_IsArray(tasks_arr)) {
        cJSON *task_obj_item;
        cJSON_ArrayForEach(task_obj_item, tasks_arr) {
            cJSON *old_id_item = cJSON_GetObjectItemCaseSensitive(task_obj_item, "id");
            if (!cJSON_IsNumber(old_id_item)) {
                if (err_msg_out)
                    *err_msg_out = strdup("task entry missing 'id' field");
                goto cleanup;
            }
            sqlite3_int64 old_task_id = (sqlite3_int64)old_id_item->valuedouble;

            char *perr = NULL;
            sqlite3_int64 new_task_id = import_task(db, task_obj_item,
                                                     new_plan_id,
                                                     &phase_remap,
                                                     &task_remap,
                                                     &perr);
            if (new_task_id <= 0) {
                if (err_msg_out) {
                    char msg[512];
                    snprintf(msg, sizeof msg, "task.create failed: %s",
                             perr ? perr : "unknown");
                    *err_msg_out = strdup(msg);
                }
                free(perr);
                goto cleanup;
            }
            free(perr);

            if (remap_add(&task_remap, old_task_id, new_task_id) != 0) {
                if (err_msg_out) *err_msg_out = strdup("out of memory");
                goto cleanup;
            }
        }
    }

    /* ---- 7. Comments (non-fatal) ---------------------------------- */
    import_comments(db, comments_arr, new_plan_id, &phase_remap, &task_remap);

    /* ---- 8. Instructions (non-fatal) ------------------------------ */
    import_instructions(db, instructions_arr, new_plan_id,
                        &phase_remap, &task_remap);

    /* ---- 9. Relations (non-fatal) --------------------------------- */
    import_relations(db, relations_arr, &task_remap);

    /* Success */
    if (new_plan_id_out) *new_plan_id_out = new_plan_id;
    ret = 0;
    goto done;

cleanup:
    /*
     * Saga rollback. Cancel tasks first (they reference phases), then phases
     * (they reference plan), then plan. Walk in reverse insertion order.
     */
    for (int i = task_remap.count - 1; i >= 0; i--)
        rollback_task(db, task_remap.new_ids[i]);
    for (int i = phase_remap.count - 1; i >= 0; i--)
        rollback_phase(db, phase_remap.new_ids[i]);
    if (new_plan_id > 0)
        rollback_plan(db, new_plan_id);

done:
    if (phase_remap_ok) remap_free(&phase_remap);
    if (task_remap_ok)  remap_free(&task_remap);
    cJSON_Delete(root);
    return ret;
}
