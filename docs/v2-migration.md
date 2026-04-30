# ipman v1 → v2 migration guide

ipman v2 is a breaking change. The protocol version bumps from `1` to `2`, and several legacy fields are removed from request inputs and response outputs. This document lists every change, why it was made, and the before/after pattern for each.

If you are running v1 against a v2 binary, every request will fail immediately with:

```
{"ok":false,"error":{"code":"invalid_request","message":"protocol_version must be 2; ipman v2.0 dropped support for v1 envelopes — bump protocol_version to 2"}}
```

That is the first thing to fix.

---

## What changed at a glance

| Surface | v1 | v2 |
|---|---|---|
| Envelope `protocol_version` | `1` | `2` |
| Selector for non-lookup ops | `id`, `uid`, `label`, `code` (with priority) | `id` only |
| Plan-scoped label resolution | `plan_uid` or `plan_label` | `plan_id` only |
| Task creation parent reference | `parent_task_id` or `parent_uid` or `parent_label` | `parent_task_id` only |
| `entity_ref` / `related_entity_ref` on events | emitted | removed |
| `from_task_ref` / `to_task_ref` on relations | emitted | removed |
| `code` on plan API responses | emitted | removed (input still accepted) |
| `uid` on plan/phase/task API responses | emitted | removed (storage only) |
| `plan.activate` selector | `id` or `code` | `id` or `code` (unchanged) |
| Lookup ops (`plan.lookup`, `phase.lookup`, `task.lookup`) | available | available (the single legitimate name → id path) |

`plan.export` is the deliberate exception: it still emits `code` and `uid` for archival round-trip preservation. Exports are snapshots, not API responses.

---

## Migration patterns

### 1. Bump every envelope to `protocol_version: 2`

```diff
- {"protocol_version":1,"request_id":"r1","actor":"agent","op":"task.get","params":{"id":42}}
+ {"protocol_version":2,"request_id":"r1","actor":"agent","op":"task.get","params":{"id":42}}
```

### 2. Replace `uid` selectors with `id`

If you have a `uid` like `task_42`, the trailing integer is the `id`. Pass it directly:

```diff
- {"op":"task.get","params":{"uid":"task_42"}}
+ {"op":"task.get","params":{"id":42}}
```

If you do not have the integer at hand, call `task.lookup` once and reuse the returned `id`:

```sh
# Resolve uid to id once
echo '{"protocol_version":2,"request_id":"L","actor":"agent","op":"task.lookup","params":{"uid":"task_42"}}' | ipman
# → {"ok":true,"result":{"id":42}}

# Then use the id everywhere else
echo '{"protocol_version":2,"request_id":"G","actor":"agent","op":"task.get","params":{"id":42}}' | ipman
```

### 3. Replace `label` selectors with a `*.lookup` call

For plans:

```diff
- {"op":"plan.get","params":{"label":"ship-login-refactor"}}
+ {"op":"plan.lookup","params":{"label":"ship-login-refactor"}}  // → {"id":1}
+ {"op":"plan.get","params":{"id":1}}
```

For phases and tasks, label resolution requires `plan_id` scope (not `plan_uid` or `plan_label` anymore):

```diff
- {"op":"task.get","params":{"label":"move-jwt-verification","plan_uid":"plan_1"}}
+ {"op":"task.lookup","params":{"label":"move-jwt-verification","plan_id":1}}  // → {"id":42}
+ {"op":"task.get","params":{"id":42}}
```

If you only have `plan_uid`, resolve plan first, then phase/task:

```sh
# plan_uid → plan_id
echo '{"protocol_version":2,"request_id":"L1","actor":"agent","op":"plan.lookup","params":{"uid":"plan_1"}}' | ipman
# → {"ok":true,"result":{"id":1}}

# label + plan_id → task_id
echo '{"protocol_version":2,"request_id":"L2","actor":"agent","op":"task.lookup","params":{"label":"move-jwt-verification","plan_id":1}}' | ipman
# → {"ok":true,"result":{"id":42}}
```

### 4. Replace plan `code` selectors with `plan.lookup` (except `plan.activate`)

```diff
- {"op":"plan.get","params":{"code":"REL-001"}}
+ {"op":"plan.lookup","params":{"code":"REL-001"}}  // → {"id":1}
+ {"op":"plan.get","params":{"id":1}}
```

`plan.activate` is the single non-lookup op that still accepts `code` directly — establishing the active plan is a session entry point, and forcing a `plan.lookup` first would add friction to the most common gesture:

```sh
# Still works in v2
echo '{"protocol_version":2,"request_id":"A","actor":"agent","op":"plan.activate","params":{"code":"REL-001"}}' | ipman
```

### 5. Replace `parent_uid` / `parent_label` on task creation with `parent_task_id`

```diff
- {"op":"task.create","params":{"plan_id":1,"phase_id":3,"title":"Child","parent_uid":"task_5"}}
+ {"op":"task.lookup","params":{"uid":"task_5"}}  // → {"id":5}
+ {"op":"task.create","params":{"plan_id":1,"phase_id":3,"title":"Child","parent_task_id":5}}
```

The same applies to `task.replace` if you were passing a parent by uid or label.

### 6. Stop reading `uid`, `code`, `entity_ref`, `from_task_ref`, `to_task_ref` from responses

These fields are no longer emitted by API responses. Parsers that hard-code them will hit `KeyError` (or equivalent). Migration:

```diff
  // v1 plan.get response
- {"plan":{"uid":"plan_1","label":"ship-login-refactor","id":1,"code":"REL-001","title":"...",...}}
  // v2 plan.get response
+ {"plan":{"id":1,"label":"ship-login-refactor","title":"...",...}}
```

```diff
  // v1 event.list events
- {"events":[{"id":67,"entity_type":"phase","entity_id":2,"entity_ref":"P1/F2",...}]}
  // v2 event.list events
+ {"events":[{"id":67,"entity_type":"phase","entity_id":2,...}]}
```

If you need a human-friendly display string, compose one from `entity_type` and `entity_id` yourself, or call `*.get` to fetch the current `label`. Do not cache derived strings — they were derived from mutable state in v1 (`phase.move` would silently change them), which is why they are gone.

### 7. Update error-handling for `validation_failed`

Requests that send a removed selector now fail with `validation_failed` and `details.kind: "unknown_parameter"` instead of the v1 `not_found`-by-uid path:

```json
{
  "ok": false,
  "error": {
    "code": "validation_failed",
    "message": "request contains unknown parameter(s)",
    "details": {"kind": "unknown_parameter", "received": ["uid"], "allowed": ["id"]}
  }
}
```

If your client retries on `not_found`, do not retry `validation_failed` — the request shape itself is wrong.

---

## What did NOT change

- **Stored fields**: `code` and `label` are still stored on plans; `label` is still stored on phases and tasks. They round-trip through `plan.export` and survive on `*.create`. Only API response shapes shrank, not the data model.
- **Origin metadata**: `origin_ref_type` and `origin_ref_id` on tasks (e.g. `"review"` + `"PR-12"`) are stored user-supplied fields, not derived breadcrumbs. They stay.
- **Request envelope shape**: the same five fields (`protocol_version`, `request_id`, `actor`, `op`, `params`) — only `protocol_version`'s value changed.
- **Error code semantics**: `invalid_request` still means malformed envelope, `validation_failed` still means bad params, `not_found` still means missing entity, `conflict` still means state violation. Exit-code split (1 for fatal, 0 for semantic) is unchanged.
- **`plan.export`**: archival snapshots still include `uid` and `code` so plans can be re-imported with their identifiers intact.

---

## Why these changes

`id` is the only identifier the database guarantees as stable, unique, and never silently mutated. `uid` was a thin wrapper around `id` (`task_<id>`); `entity_ref` was derived from mutable state and could change under your feet (`phase.move` rewrites `local_seq`). Every place where the API let you "use a name" instead of `id` was either redundant (with `uid`) or fragile (with derived refs). v2 names this directly: pass `id`; if you need to translate a name, do it explicitly via `*.lookup` and reuse the resulting `id`. The lookup ops are cheap, and the translation cost is paid once per workflow rather than implicitly on every call.
