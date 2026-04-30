/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

/* render_md — Markdown implementation plan document renderer. */

#ifndef IPMAN_RENDER_MD_H
#define IPMAN_RENDER_MD_H

#include <sqlite3.h>
#include <stdio.h>

/* Resolve a plan selector (code like P3, uid like plan_3, label, or numeric
 * id string) to its integer plan_id. Returns 0 on success, -1 if not found. */
int ipman_render_resolve_plan(sqlite3 *db, const char *selector,
                             sqlite3_int64 *plan_id_out);

/* Render a complete Markdown implementation plan document to `out`.
 * Returns 0 on success, -1 on error (message printed to stderr). */
int ipman_render_md(sqlite3 *db, sqlite3_int64 plan_id, FILE *out);

#endif /* IPMAN_RENDER_MD_H */
