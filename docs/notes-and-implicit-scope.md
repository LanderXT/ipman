# `ipman note` and cursor-implicit scope: decision

**Status:** Decided (v2.2). **Decision:** Option A — reject `ipman note` as proposed; the No-goals §2 invariant on cursor-implicit writes stays in force. **Follow-up:** open a v2.3 sub-plan for an *explicit-selector* `ipman --comment <selector>` CLI sugar that addresses the underlying friction without relaxing the invariant.

This document records the rationale so that a future maintainer who sees the friction first-hand does not have to re-derive the trade-off from scratch.

---

## 1. The friction case

Mid-flight, an agent wants to capture an observation against the work it is currently doing — a one-line note like `"trying X, doesn't compile, falling back to Y"`. The natural mental model is *attach this thought to "what I'm working on right now"*: the cursor.

Today, doing that requires three steps over the JSON wire:

1. Read `workspace.context_get` to learn `current_task_id`.
2. Call `task.comment_add` with that `id` and the body.
3. Parse the response, surface failures.

Or, on the CLI, a shell composition that resolves the id and feeds it to a hand-rolled JSON envelope. Either way, the cost-per-observation is high enough that agents skip the note rather than pay the cost. Observations get lost.

A natural fix is a single CLI verb `ipman note "body"` that infers the target from the cursor.

---

## 2. The invariant under threat

`docs/v2.1-ergonomics.md` §2 of No-goals states the rule:

> Every write op still takes an explicit `id`. There is no "close whatever the cursor points at" shortcut, on the wire or in the CLI. The reasoning: cursor-implicit mutations are convenient until two agents share a workspace, at which point a misaligned cursor silently writes to the wrong task. v2.1 keeps the cursor a *navigation* concept, not a *mutation* target.

The failure mode this rules out:

- Agent A starts on task 42, moves the cursor.
- Agent B, holding a stale view, runs `ipman note "blocked by upstream"`.
- The note lands on task 42 instead of the task agent B was actually working on.
- Nobody sees the attribution error until much later — append-only history makes the misattribution permanent.

The invariant is *cheap to enforce* (every write op carries `id`) and *expensive to relax* (silent misattribution is hard to detect after the fact). That asymmetry is what made it a No-goal in the first place.

---

## 3. Options evaluated

### Option A — Reject `ipman note`; keep the invariant intact

**Behavior:** No new verb. Notes continue to require an explicit selector or a JSON envelope with an explicit `id`.

**Pros**
- Invariant intact: no write path can silently misattribute under a stale cursor.
- One-agent and many-agent workspaces behave identically under this rule.
- Predictable: every write op in the CLI follows the same `<verb> <selector> [flags]` shape (`--start <task>`, `--close <task>`, `--close-phase <phase>`, future `--comment <task>`). No carve-outs.
- Consistent with the existing carve-out in §2 itself: when the doc rejected `task.complete_current`, it suggested a *shell composition* (`ipman --close $(ipman --next | …)`) as the local-convenience path. The same path is available for notes today.

**Cons**
- The friction is real and observed. Agents do skip notes today.
- Inconsistent surface area when comparing to comment.add JSON: the wire op already exists, the CLI just hasn't surfaced it yet. Users perceive that as a missing verb.

**Disposition of the friction:** addressed by a *separate* v2.3 sub-plan adding `ipman --comment <selector> --body "..."` (explicit selector required, no cursor inference). That collapses three steps to one without granting the cursor write semantics. Detailed below in §5.

### Option B — Accept `ipman note` with explicit `--on phase|task` scope (cursor-implicit entity-id)

**Behavior:** New verb `ipman note --on task "body"` (or `--on phase`), where the `--on` flag declares the *entity kind* and the cursor supplies the *entity id* within that kind.

**Pros**
- Reduces the worst-case error: a misaligned cursor cannot smuggle a phase-scoped observation onto a task or vice versa.
- More ergonomic than option A; one command covers the common case.

**Cons**
- The dangerous part of cursor-implicit writes is the *entity id*, not the *entity type*. A task-scoped note still lands on the wrong task if two agents share the workspace. The invariant is *narrower* under B but not *intact*.
- Introduces a permanent asymmetry: notes can target the cursor, lifecycle ops cannot. Agents will conflate them: "if I can `note` the cursor, why can't I `close` it?" — and the answer ("comments are append-only, lifecycle is destructive") is a subtle distinction that does not survive contact with a cold reader.
- The marginal ergonomic gain over an explicit-selector `--comment` verb is exactly one selector string per call. That is not worth a partial relaxation of an invariant the project has actively defended.
- Comment misattribution is permanent and hard to reverse: once `comment 9001 → task 42` is in the audit log, the only fix is another comment that says "the previous comment was meant for task 38." That is a worse audit trail than no comment at all.

### Option C — Reopen No-goals §2 for all writes; allow cursor-implicit selectors everywhere

**Behavior:** Every CLI write verb (and optionally every wire op) accepts a missing selector and falls through to the cursor.

**Pros**
- Maximum ergonomic gain: every write becomes a single command.
- Aligns with the mental model of a single agent working in a workspace it owns.

**Cons**
- Maximum blast radius: every write path becomes a candidate for the multi-agent silent-misattribution failure mode. `--close`, `--cancel`, `--defer`, status changes — all of them affect *destructive* state, not append-only history.
- Removes the v2.0/v2.1 invariant the project has defended explicitly. The cost of that invariant is small (one selector per call); the value of it is large (no class of silent-misattribution bugs).
- The dogfooding context for ipman includes shared workspaces between Codex and Claude Code agents. The risk this option introduces is not hypothetical for this project.

---

## 4. Recommendation and final decision

**Recommendation:** **Option A.** Reject `ipman note` as proposed. The invariant on cursor-implicit writes is the kind of defended boundary whose value compounds — every new write verb that respects it gives the user one more place where they cannot misattribute work, and one less category of bug they have to carry in their head.

**The friction is solved differently:** by surfacing the wire op `comment.add` (and its task/phase variants) through an explicit-selector CLI verb in v2.3, not by relaxing the invariant. Three steps collapse to one; the audit trail still names the entity.

**Final decision:** **A.** Topic on cursor-implicit writes is closed for v2.x. F6 ships zero code. v2.3 opens an explicit-selector `ipman --comment` follow-up; that is a separate plan, not a continuation of v2.2.

The same wire-vs-CLI separation that v2.1 §1 uses for selectors (`uid`/`label` accepted on the CLI as sugar over the wire's `id`-only selector) applies here: the wire stays minimal, the CLI surfaces it ergonomically — but the CLI never *adds* a write semantic the wire does not have, and the wire's selector contract is non-negotiable.

---

## 5. Follow-up plan: `ipman --comment <selector>` (v2.3 candidate)

If accepted, this is what addresses the friction without relaxing §2:

```sh
ipman --comment task_42 --body "trying X, doesn't compile, falling back to Y"
ipman --comment task_42 --body "..." --type progress
ipman --comment phase_3 --body "..."
```

Wire mapping (no new ops, no schema change):

- `task_*` selector → `task.comment_add` with `selector` param.
- `phase_*` selector → `phase.comment_add`.
- Numeric or `plan_*` selector → `plan.comment_add` or generic `comment.add`.

Constraints (mirror v2.2 phase verbs):

- Selector required. No `ipman --comment` without a target.
- Selector kind drives the wire op (same dispatch pattern as `--close-phase`).
- `--type` defaults to `general`; other values pass through to `comment_type`.
- `--dry-run` prints the envelope with `request_id="dry-run-comment"`.

Out of scope for the v2.3 sub-plan (each requires its own decision):

- A `--comment` verb that infers the entity from the cursor. (Reaffirmed rejected — that *is* the option this document closed.)
- Multi-line bodies on the CLI (use `--body "$(cat note.txt)"` until friction is observed).
- Bulk attach across multiple selectors.

---

## 6. What this document is *not*

- It is not a deprecation. There is nothing to deprecate; `ipman note` was never shipped.
- It is not a permanent ban on revisiting the topic. If a future release introduces a workspace ownership / locking concept that makes cursor misattribution detectable or impossible, the trade-off changes and the decision can be revisited *with that primitive in hand*. The current decision is conditional on the current threat model.
- It is not a statement that the friction is unimportant. It is a statement that the *right shape* of the fix is an explicit-selector verb, not a cursor-implicit one.
