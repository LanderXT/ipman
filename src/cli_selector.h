/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_CLI_SELECTOR_H
#define IPMAN_CLI_SELECTOR_H

#include <stddef.h>

#include "sqlite3.h"

/*
 * CLI-side resolver for the v2.1 shortcuts and views.
 *
 * v2.0 op handlers accept `id` only; `uid` and `label` were moved to dedicated
 * `*.lookup` ops. To keep CLI ergonomics the same as v1 ("ipman --show
 * ship-login-refactor"), the CLI shortcuts and views need to translate
 * user-friendly arguments to numeric ids before dispatching. This module
 * centralizes that translation so every verb shares one parsing rule, one
 * error message set, and one definition of "active plan scope".
 */

/*
 * Bitmask of entity kinds the caller is willing to accept. Combine with
 * bitwise OR or use the convenience masks below.
 */
typedef enum {
    CLI_SELECTOR_KIND_TASK  = 1u << 0,
    CLI_SELECTOR_KIND_PHASE = 1u << 1,
    CLI_SELECTOR_KIND_PLAN  = 1u << 2
} cli_selector_kind_t;

#define CLI_SELECTOR_KIND_TASK_OR_PHASE \
    ((cli_selector_kind_t)(CLI_SELECTOR_KIND_TASK | CLI_SELECTOR_KIND_PHASE))
#define CLI_SELECTOR_KIND_ANY \
    ((cli_selector_kind_t)(CLI_SELECTOR_KIND_TASK \
                         | CLI_SELECTOR_KIND_PHASE \
                         | CLI_SELECTOR_KIND_PLAN))

/* Recommended size for `err_buf` so we never truncate a real diagnostic. */
#define CLI_SELECTOR_ERR_LEN 256

/*
 * Resolve a CLI selector argument to a canonical (kind, id) pair.
 *
 * Accepted forms:
 *   "42"            — Numeric passthrough. id = 42; kind = the only kind set
 *                     in `expected` if unambiguous, otherwise probed via
 *                     *.get in TASK > PHASE > PLAN order.
 *   "task_42"       — uid prefix; resolved via task.lookup({uid}).
 *   "phase_3"       — uid prefix; resolved via phase.lookup({uid}).
 *   "plan_1"        — uid prefix; resolved via plan.lookup({uid}).
 *   "any-label"     — Bare label. For TASK/PHASE: workspace.context_get →
 *                     active_plan.id, then *.lookup({label, plan_id}). For
 *                     PLAN: plan.lookup({label}) directly (plans are not
 *                     plan-scoped). Lookups are tried in TASK > PHASE > PLAN
 *                     order; the first hit within `expected` wins.
 *
 * `expected` is a bitmask: the resolver fails with a readable message when
 * the discovered kind is not in the set (e.g. caller wanted PLAN but the user
 * passed `task_42`).
 *
 * On success: returns 0 and writes `*id_out` and `*kind_out`.
 * On failure: returns -1, writes a NUL-terminated message to `err_buf`
 *             (truncated to fit `err_buf_size`), and leaves the out-params
 *             untouched. `err_buf` may be NULL only if `err_buf_size == 0`.
 *
 * The resolver does not print anything; callers decide how to surface errors.
 * Use the resulting id in op-specific calls as `{"id": N}`.
 */
int cli_resolve_selector(sqlite3 *db,
                         const char *arg,
                         cli_selector_kind_t expected,
                         long *id_out,
                         cli_selector_kind_t *kind_out,
                         char *err_buf, size_t err_buf_size);

#endif /* IPMAN_CLI_SELECTOR_H */
