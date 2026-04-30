# ipman operation contract audit

Date: 2026-04-29

This audit records the current contract gaps between implemented operations,
dispatch allowlists, generated docs, generated JSON schemas, and observed
runtime behavior. The goal is to make ipman predictable for agents and scripts:
if a parameter is accepted, it must have defined behavior; if runtime accepts a
request, generated documentation and schemas must not reject it.

## Source of truth

- Public operation set: `src/dispatch.c`.
- Runtime parameter allowlist: each operation's `ipman_op_*_params`.
- Runtime behavior: operation handlers and selector helpers.
- Human docs: `src/agent_docs.c` generated markdown plus `.claude/skills/ipman/SKILL.md`.
- Machine docs: generated `schemas/ipman.op.*.request.schema.json`.

## Findings and decisions

### Operation set parity

Dispatch, `OperationSpec`, and generated manifest all expose the same 58
operations. Keep this invariant and add regression coverage so drift is caught
automatically.

### Selector schemas over-require `id`

Many runtime handlers accept selector alternatives such as `uid`, `id`, or
`label` with plan scope, but generated JSON schemas mark `id` as required. This
causes schemas to reject valid runtime requests.

Decision: fix generated JSON schemas to model selector alternatives. Preserve
current runtime selector compatibility and document selector precedence instead
of introducing a breaking "exactly one selector" rule across existing ops.

### Accepted but ignored params

Some allowlists accept keys that handlers ignore or reject:

- `plan.list` accepts `uid` and `label`, but does not filter on them.
- `comment.update` and `comment.invalidate` accept selector keys, but require numeric `id`.
- `plan.comment_add` accepts selector keys, but fixed plan comments require numeric `id`.

Decision: remove stale allowlist entries unless the handler is explicitly
extended. Prefer the smaller public contract for these cases because the extra
keys never worked as documented behavior.

### Weak generated JSON schemas

Generated schemas currently use empty property schemas (`{}`), so they only
partially document allowed keys and required keys. They do not encode types,
nullable fields, enums, booleans, arrays, or selector dependencies.

Decision: add focused schema metadata in the docs generator for high-value
types and enums. Keep the schema generator simple and local to
`src/agent_docs.c`; do not introduce a new schema framework.

### Selector ambiguity

Most selector helpers choose by precedence when multiple selectors are present:
task/phase selectors prefer `uid`, then `id`, then `label`; most plan selectors
prefer `uid`, then `id`, then `label`, then `code`. `plan.activate` is stricter
and requires exactly one of `id` or `code`.

Decision: document current precedence to preserve compatibility. Generated
schemas should allow the valid selector forms and avoid implying that only
numeric `id` is accepted.

## Validation target

The fix is complete when:

- Operation set parity is mechanically tested.
- Stale accepted params are rejected or implemented.
- Generated JSON schemas accept valid `uid` and scoped `label` selector examples.
- Generated docs and bundled skill text describe selector semantics consistently.
- `make test` has a meaningful local test surface and passes.
