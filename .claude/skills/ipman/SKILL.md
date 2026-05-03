---
name: ipman
description: "Use this skill when you need to read or update plans, phases, tasks, comments, instructions, project metadata, tools, env_vars, or workspace context via the `ipman` CLI from Codex, Claude Code, or another agent session. Trigger when: the user asks about the current plan, wants to create or update a task or phase, asks for workspace context, asks what's in progress, asks what the project does or what tools / env vars it requires, or any operation involving plan.*, phase.*, task.*, comment.*, instruction.*, project.*, tool.*, env_var.*, event.list, or workspace.context_get."
version: 1.0.0
---

# ipman skill

Use this skill when you need to read or update plans, phases, tasks, or workspace state via the `ipman` CLI.

## 1. Bootstrap

```sh
ls .ipman/ipman.db 2>/dev/null || ipman init
```

After init, read the full operation catalog and parameter shapes:

```sh
cat .ipman/START-HERE.md
```

## 2. Request format

Every call is a JSON envelope piped to `ipman` on stdin:

```json
{
  "protocol_version": 2,
  "request_id": "<short-unique-string>",
  "actor": "agent",
  "op": "<entity.verb>",
  "params": {}
}
```

Example:

```sh
echo '{"protocol_version":2,"request_id":"r1","actor":"agent","op":"plan.create","params":{"title":"My Plan","summary":"Short description","priority":"medium"}}' | ipman
```

## 3. Response shape

```
ok:true  → {"request_id":"...","ok":true,"result":{...}}
ok:false → {"request_id":"...","ok":false,"error":{"code":"...","message":"..."}}
```

- Check `ok` first.
- `ok:false` with exit 0 = semantic error (validation, not found, conflict) — readable in `error.code` / `error.message`.
- Exit 1 = fatal (bad JSON input, DB unreachable) — do not retry without fixing the cause.

## 4. Discovering Operations

The `.ipman/` directory contains auto-generated documentation that is always current (refreshed by `workspace.refresh_agent_docs`). Read these files instead of guessing params:

| What you need | Where to look |
|---|---|
| All operations alphabetically | `cat .ipman/indexes/ipman.index.operations.md` |
| Operations grouped by entity | `cat .ipman/indexes/ipman.index.by-entity.md` |
| Operations grouped by workflow intent | `cat .ipman/indexes/ipman.index.by-workflow.md` |
| One operation's full schema | `cat .ipman/operations/ipman.op.<entity>.<verb>.schema.md` |
| One operation's JSON request schema | `cat .ipman/schemas/ipman.op.<entity>.<verb>.request.schema.json` |

Schema files document required/optional params, preconditions, side effects, and output fields. The naming convention is `ipman.op.{entity}.{verb}.schema.md`.
JSON request schemas document allowed params, primitive types/enums, required scalar fields, selector alternatives, and documented closure/update alternatives. Runtime validation remains authoritative for database existence, state transitions, and cross-field business rules.

**Read the schema file for the operation you need — don't guess params.**

## 5. CLI shortcuts (v2.1+ ergonomics)

For routine reads and a curated set of writes, prefer the human-CLI veneer over hand-crafted JSON envelopes. Every veneer command is a thin client over the same op an agent would send, so semantics are identical — you just save the envelope boilerplate.

### Read shortcuts

```sh
ipman -S                         # active plan, current phase, current task, pending count
ipman -L                         # pending tasks for the active plan
ipman -SH <selector>             # detail for a task or phase
ipman -LG [--summary-only] [--limit N]
                                 # recent events. v2.2: --summary-only drops Details column;
                                 # --limit N (1-500, default 20) caps rows (clamped if out of range).
ipman -N                         # active plan + cursor + standing instructions + Up next pending
ipman -R <plan>                  # render plan as Markdown
```

`ipman -N` is the **handoff super-shortcut**: a single call replaces the typical sequence `workspace.context_get` → `plan.get` → `phase.get` → `task.get` → `instruction.list` (×3 scopes). Use it whenever you would have started a session with five separate envelopes.

### Write shortcuts

```sh
ipman --start  <selector>
ipman --close  <selector> --summary <text> --comment <text>
                          [--lessons <text>] [--open-items <text>] [--followup]
                          [--validation <cmd:status>]... [--decision <text>]...
ipman --cancel <selector> --summary <text> --comment <text>
ipman --defer  <selector> --reason-text <text> [--reason-code <code>]
ipman --close-phase  <selector> --summary <text> --comment <text>      # v2.2
ipman --cancel-phase <selector> --summary <text> --comment <text>      # v2.2
ipman --current  <selector>      # set current task or phase (auto-detected)
ipman --activate <selector>      # set the active plan
ipman --dry-run                  # combine with any write verb to print the JSON
                                 # that would be sent and exit without touching the DB
```

`--dry-run` is the agent-friendliest way to confirm a write before committing it.

### When to drop back to JSON

Use the JSON envelope (sections 2–4) when you need an op with no veneer (most lookups, `plan.export`, `closure.get`, instruction CRUD), when scripting batches, or when you need the full structured response. Selector forms (`id`, `uid`, or `label`) work the same in both surfaces.

If you need the full menu, run `ipman --usage`. Parse-level envelope errors also nudge you toward `ipman --usage` or `ipman --next`.

## 6. Workflow Examples

### Task Lifecycle

Create a task, start it, then close it:

```sh
echo '{"protocol_version":2,"request_id":"r1","actor":"agent","op":"task.create","params":{"plan_id":1,"title":"Fix login bug"}}' | ipman
# → result.task.id, e.g. 42

echo '{"protocol_version":2,"request_id":"r2","actor":"agent","op":"task.transition","params":{"id":42,"status":"in_progress"}}' | ipman

echo '{"protocol_version":2,"request_id":"r3","actor":"agent","op":"task.close","params":{"id":42,"outcome_summary":"Fixed null check in auth module","closing_comment":"Root cause was missing null guard"}}' | ipman
```

### Cancel a Task

Cancel a task (work will not be done) with a closure record:

```sh
echo '{"protocol_version":2,"request_id":"r1","actor":"agent","op":"task.cancel","params":{"id":42,"resolution":"not_planned","outcome_summary":"Decided not to implement - out of scope","closing_comment":"Deprioritized after planning review"}}' | ipman
```

`task.cancel` sets the supplied terminal resolution (`canceled`, `not_planned`, `discarded`, or `duplicate`); `task.close` sets `resolution=completed`. Use the right one — they mean different things in the audit trail.

### Task Replacement

Cancel an old task and atomically create its replacement:

```sh
echo '{"protocol_version":2,"request_id":"r4","actor":"agent","op":"task.replace","params":{"id":42,"title":"Fix login bug (revised approach)","closing_comment":"Original approach was too narrow","outcome_summary":"Superseded by broader auth refactor"}}' | ipman
# → result.old_task (canceled), result.new_task (created), result.relation_id
```

### Context Navigation

Set the active plan, current phase, and current task:

```sh
echo '{"protocol_version":2,"request_id":"r5","actor":"agent","op":"plan.activate","params":{"id":1}}' | ipman

echo '{"protocol_version":2,"request_id":"r6","actor":"agent","op":"phase.set_current","params":{"id":3}}' | ipman

echo '{"protocol_version":2,"request_id":"r7","actor":"agent","op":"task.set_current","params":{"id":42}}' | ipman

echo '{"protocol_version":2,"request_id":"r8","actor":"agent","op":"workspace.context_get","params":{}}' | ipman
# → result.context.active_plan_id, current_phase_id, current_task_id
```

### Close or Archive a Plan

Always export before closing or archiving — the snapshot survives outside the DB:

```sh
echo '{"protocol_version":2,"request_id":"r9","actor":"agent","op":"plan.export","params":{"id":1}}' | ipman
# → save result.snapshot to version control or a handoff artifact

echo '{"protocol_version":2,"request_id":"r10","actor":"agent","op":"plan.close","params":{"id":1,"outcome":"completed","outcome_summary":"All phases delivered"}}' | ipman
```

### Defer and Resume

Defer a task, then resume it later:

```sh
echo '{"protocol_version":2,"request_id":"r9","actor":"agent","op":"task.defer","params":{"id":42,"reason_text":"Blocked by upstream API change","reason_code":"external_dependency"}}' | ipman

# Resume: transition back to todo or in_progress (deferred is not terminal)
echo '{"protocol_version":2,"request_id":"r10","actor":"agent","op":"task.transition","params":{"id":42,"status":"todo"}}' | ipman
```

### Audit Trail

Read recent events and closure memory:

```sh
echo '{"protocol_version":2,"request_id":"r11","actor":"agent","op":"event.list","params":{"limit":20}}' | ipman

echo '{"protocol_version":2,"request_id":"r12","actor":"agent","op":"closure.get","params":{"entity_type":"task","entity_id":42}}' | ipman
# → outcome_summary, lessons_learned, open_items_summary
```

### Standing Instructions

Record durable guidance that future agents should follow for a plan, phase, or task:

```sh
echo '{"protocol_version":2,"request_id":"r13","actor":"agent","op":"instruction.add","params":{"entity_type":"plan","entity_id":1,"instruction_type":"guidance","body":"Preserve backward compatibility unless explicitly told otherwise."}}' | ipman

echo '{"protocol_version":2,"request_id":"r14","actor":"agent","op":"instruction.list","params":{"entity_type":"plan","entity_id":1}}' | ipman
```

Use instructions for standing constraints and operating guidance. Use comments for conversational notes, progress, and decisions.

For operations not covered here, see `.ipman/operations/ipman.op.*.schema.md` for the full param list.

## 7. Error Codes

| Code | Meaning | Retryable? | Action |
|---|---|---|---|
| `invalid_request` | Malformed JSON, missing/invalid envelope fields | No | Fix the request structure |
| `unknown_op` | Op name not in dispatch table | No | Fix the op name; check `.ipman/indexes/` |
| `validation_failed` | Invalid params, wrong types, business rule violation, or an ambiguous label that resolves to multiple entities | No | Fix the params; read the schema file |
| `not_found` | Entity does not exist | No | Check the ID; verify entity exists |
| `conflict` | State violation (terminal state, duplicate, circular) | No | Check entity state; use reopen if terminal |
| `internal_error` | Database error, OOM, unexpected failure | Maybe | May be transient, but usually fatal |

Every error — fatal or semantic — emits a JSON envelope on stdout. The exit code is what distinguishes them:

- `invalid_request` and `internal_error` cause **exit code 1**: the request could not be executed (malformed JSON, unreachable DB, etc.). Stdout still receives a best-effort error envelope so the caller can surface the code; do not retry without fixing the cause.
- All other codes cause **exit code 0**: the operation ran but reported a semantic outcome via `ok:false` (validation, not found, conflict, …). Read `error.code` and `error.message` to decide next steps.

## 8. Agent Handoff

At session start, recover scope and context:

```sh
# 1. Discover active plan, current phase, current task
echo '{"protocol_version":2,"request_id":"h1","actor":"agent","op":"workspace.context_get","params":{}}' | ipman

# 2. Inspect the active plan
echo '{"protocol_version":2,"request_id":"h2","actor":"agent","op":"plan.get","params":{"id":1}}' | ipman

# 3. Read standing instructions for the active plan
echo '{"protocol_version":2,"request_id":"h3","actor":"agent","op":"instruction.list","params":{"entity_type":"plan","entity_id":1}}' | ipman

# 4. Catch up on recent changes
echo '{"protocol_version":2,"request_id":"h4","actor":"agent","op":"event.list","params":{"limit":20}}' | ipman

# 5. Inspect current task
echo '{"protocol_version":2,"request_id":"h5","actor":"agent","op":"task.get","params":{"id":42}}' | ipman

# 6. Recover closure memory for closed entities
echo '{"protocol_version":2,"request_id":"h6","actor":"agent","op":"closure.get","params":{"entity_type":"task","entity_id":42}}' | ipman
```

Key points:
- `id` is canonical for both input and output. Every entity op (`*.get`, `*.update`, lifecycle ops, etc.) accepts only `id` as a selector. Ordinary entity responses return `id` and `label`; `uid` is storage/export-only, and plan `code` is surfaced only where activation context or exports need the display handle.
- To resolve a `label` or plan `code` into an `id`, call the matching `*.lookup` op: `plan.lookup` accepts `uid`/`label`/`code`; `phase.lookup` and `task.lookup` accept `uid` or `label` (label requires `plan_id` scope). Lookup ops return `{id: N}` only — call them once, then use the `id` everywhere else.
- `plan.activate` is the single non-lookup op that still accepts `code` directly (alongside `id`), since it is the entry point that establishes the active plan for a session.
- `status`, `resolution`, and `origin_type` are three independent concepts — don't conflate them.
- Never `DELETE FROM` main entities; use close/cancel/archive operations instead.
- See `.ipman/workflows/ipman.workflow.agent-handoff.md` for the full handoff workflow.
