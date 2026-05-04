# Plan Import Shortcut — Design Spec

**Date:** 2026-05-04  
**Phase:** v2.4 Phase 26  
**Status:** Implemented

---

## Scope

`ipman --import-plan <file.json>` reads a JSON file produced by `plan.export`
(export_format_version 2 or 3) and registers a complete plan tree in the
current workspace as a *new* plan. It allocates fresh IDs for every entity and
re-maps all cross-references.

This is a **CLI shortcut** (client-side orchestrator in `src/main.c` +
`src/import_ops.c`), not a JSON op. It does not touch `dispatch.c` or add a
new wire op. The orchestrator calls the existing in-process dispatch path
(`call_op` / `ipman_dispatch`) for every mutation, exactly as other shortcuts
(`run_close`, `run_cancel`) do.

---

## What is imported vs skipped

| Section | Action | Why |
|---|---|---|
| `plan` | `plan.create` | Becomes the new top-level row |
| `phases` | `phase.create` (in original array order) | Preserves sequence_no |
| `tasks` | `task.create` (in array order, parent remapped) | phase_id and parent_task_id remapped |
| `comments` | `comment.add` (plan/phase/task scopes) | Context; entity_id remapped |
| `instructions` | `instruction.add` (plan/phase/task scopes only) | Guidance; project-scoped entries skipped |
| `events` | **SKIP** | Auto-emitted by new ops; importing originals would corrupt audit trail |
| `closures` | **SKIP** | Closures bind to events; importing as todo/open is cleaner semantically |
| `relations` | `task.link_dependency` (from_task_id and to_task_id remapped) | Dependency graph |

**Project-scoped instructions** (entity_type=`project`) are skipped — they
belong to the workspace, not to the plan being imported.

---

## Imported state

All entities are imported in their *creation-default* state:
- Plans: `status=open`
- Phases: `status=open`
- Tasks: `status=todo`

Terminal states (closed, canceled, archived) are NOT round-tripped. This is
intentional: importing a plan is a fresh start. A future phase (v2.5+) can
add optional closed-state round-trip if users request it.

Read-only fields in the export envelope (`uid`, `label`, `local_seq`,
`created_at`, `updated_at`, `opened_at`, `closed_at`, `archived_at`,
`started_at`) are **ignored** — they are auto-assigned by the `*.create` ops.

---

## ID remap strategy

A flat array maps original IDs to new IDs for each entity type:

```
import_remap_t {
    sqlite3_int64 *phase_old[N];  sqlite3_int64 phase_new[N];
    sqlite3_int64 *task_old[N];   sqlite3_int64 task_new[N];
}
```

After `plan.create`, the new plan ID is known. `phase.create` maps each
original phase_id to a new phase_id. `task.create` maps each original task_id
to a new task_id and remaps `phase_id` and `parent_task_id` via the remap
table before the call.

`task.link_dependency` remaps `from_task_id` and `to_task_id` via the task
remap table.

---

## Rollback algorithm (saga / compensating actions)

There are no DB-level transactions across the whole import. Atomicity is
maintained by tracking created IDs and calling compensating ops in reverse
order if any step fails.

Cleanup order: tasks first (they reference phases), then phases (they
reference plan), then plan. For each created entity:

- **Tasks**: `task.cancel(id, resolution=canceled, outcome_summary=..., closing_comment=...)`
- **Phases**: `phase.close(id, outcome=canceled, outcome_summary=..., closing_comment=...)`
- **Plan**: `plan.close(id, outcome=canceled, outcome_summary=..., closing_comment=...)`

A single `goto cleanup` after `plan.create` succeeds ensures cleanup is
reached on every subsequent failure. Cleanup errors are logged but do not
abort the loop — partial state is flagged in stderr. Cleanup itself will fail
if a task or phase was already created in a non-todo/open state (not possible
in this importer) or if the plan was already closed (not possible since we
just created it).

---

## CLI shape

```
ipman --import-plan <file.json>
ipman import-plan  <file.json>    # bare-word alias
```

No other flags. The JSON file path is the sole positional argument.

On success: prints `imported plan <id>: <title>` to stdout, exits 0.  
On failure: prints the error to stderr, exits 1. Workspace is left clean (or
a warning is printed if cleanup partially failed).

---

## Expected error modes

| Condition | Behaviour |
|---|---|
| File not found / unreadable | Exit 1 with "cannot open file" message |
| File is not valid JSON | Exit 1 with "invalid JSON" |
| Missing `plan` key | Exit 1 with "export envelope missing 'plan'" |
| `plan.create` fails (e.g. duplicate code) | Exit 1; no cleanup needed (nothing created) |
| `phase.create` fails mid-import | Exit 1; saga rollback cancels phases + plan |
| `task.create` fails mid-import | Exit 1; saga rollback cancels tasks + phases + plan |
| Cleanup op fails | Log warning to stderr; continue cleanup; exit 1 |
| Import of comments/instructions/relations fails | Log warning; continue; exit 0 with note (non-fatal) |

Comments, instructions, and relations failures are non-fatal: the plan tree
itself is usable even if decorations are partially missing.
