# Sub-tasks / Checklist — Deferred Decision (v2.4 Phase 27)

**Date:** 2026-05-04  
**Decision:** PUNT — no implementation in v2.4  
**Branch:** v2.4-slug-import-docs

---

## What was considered

External-review feedback flagged the absence of a sub-task or checklist concept. Phase 27 evaluated whether this gap was worth closing now.

Three options were compared:

**Option A — leverage existing `parent_task_id`**  
`parent_task_id` already exists in the schema (`migrations/0001_initial_schema.sql:130`), is accepted by `task.create` and `task.replace`, is returned by `task.get`, `task.list`, `export`, and `context` queries, and is remapped correctly by `import_ops.c`. Validation enforces same-plan constraint. The only gap is that `task.list` has no `parent_task_id` filter and there is no CLI shortcut. Adding those would touch roughly three places: `ipman_op_task_list_params[]`, the query builder in `ipman_op_task_list`, and `main.c` for a `--subtask` CLI shortcut.

**Option B — new `sub_task` entity**  
New table, new migration, three new ops (`subtask.add`, `subtask.list`, `subtask.toggle`). Conceptually overlaps with `parent_task_id`; introduces double-modeling risk. High complexity for unconfirmed demand.

**Option C — JSON checklist field on task**  
`checklist` JSON array column on `tasks`. No new tables or ops. Cheap but no per-item history, no cross-task queries, and mutation requires `task.update` with a structured payload. Inflexible and hard to surface well.

---

## Why we punted

1. **No real user request.** The driver was external-review feedback, not a filed issue or user complaint. CLAUDE.md: "Don't add features beyond what was asked."

2. **Infrastructure already works.** `parent_task_id` is already stored, validated, returned, exported, and imported. An agent or user can already create a sub-task today with `task.create` + `parent_task_id`. The gap is documentation and UX surface, not capability — the same pattern as Phase 25.

3. **Options B and C are speculative.** Neither has a confirmed advantage over Option A. Building them now would be YAGNI at its clearest.

4. **Option A is small when demand surfaces.** The code change for a `parent_task_id` filter on `task.list` plus a `--subtask` CLI shortcut is roughly 20–30 lines across two files. It is not a reason to do it now; it is a reason to feel comfortable waiting.

---

## When to revisit

Promote to v2.5 if any of the following occur:
- A real user asks "how do I list sub-tasks of task X?"
- An agent session produces confusion because sub-tasks are not surfaced in `task.list` output.
- The `--next` or `-N` render path needs sub-tree visibility.

---

## Recommended starting point if revisited

**Option A only.** Changes required:
- `src/task_ops.c`: add `parent_task_id` to `ipman_op_task_list_params[]` and the count/select SQL WHERE clauses.
- `src/main.c`: add `--subtask <parent-id>` shortcut that sets `parent_task_id` in `task.create` params.
- `src/agent_docs.c`: document the new filter on `task.list`.
- No migration. No new ops. No new entity.

Cycle detection is not needed unless the UI allows reparenting (it currently doesn't). Parent-must-be-same-plan validation already exists (`validate_task_in_plan` at `src/task_ops.c:1734`).
