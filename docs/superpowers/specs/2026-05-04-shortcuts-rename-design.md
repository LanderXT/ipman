# Design: Rename "human surface" → `shortcuts` / `views` / `admin`

**Status:** approved (brainstorm) — implementation pending
**Date:** 2026-05-04
**Author:** Homero Leal
**Target version:** TBD (operator decides between v2.3.3 and v2.4 at commit time; this is docs-and-comments only, `protocol_version` stays at `2`)

---

## 1. Problem

Today ipman markets itself as having "two surfaces": a JSON op protocol for agents, and a "human CLI" / "human surface" for operators. Reports indicate that some users find the dual-surface story confusing — they cannot predict which CLI commands map to a single JSON op (and could be replayed as JSON) versus which ones are aggregator views with no JSON equivalent. The label "human" describes *who* uses it, not *what it does*, so it does not help users build the mental model they need.

A second-order issue: the premise *"every operation must be implemented as JSON in/out, with a subset exposed as a CLI convenience"* is true for the write verbs and `--show` / `--log`, but not for `--status`, `--ls`, `--next`, `--render`, which compose multiple ops client-side. Calling all of them "shortcuts" papers over a real architectural distinction.

## 2. Goals

- Replace the "two surfaces" framing with a three-way taxonomy that names what each CLI command actually does.
- Keep the user's premise intact: *the JSON op surface is canonical; CLI verbs are conveniences over it.*
- Zero behavioral change. No flags renamed, no ops added or removed, no schema changes, `protocol_version` stays at `2`.
- Reduce the surface area of the "what is this command?" question for new users (especially agents reading docs).

## 3. Non-goals

- **No new JSON ops.** We are not creating `workspace.overview` to back `-N`. The aggregator views remain client-side compositions; we name that fact instead of hiding it.
- **No flag renames.** `-N`, `--render`, `--start`, etc. all stay exactly as today. Both short and long forms remain interchangeable per the existing v2.1 ergonomic rule.
- **No function-symbol renames in C.** `run_status`, `run_next`, etc. keep their names — high churn for low value. Comments at the top of each function are updated.
- **No new short flags for write shortcuts.** `--start` does not gain a `-st`. That is a separate ergonomics decision.
- **No changes to the `instruction` entity** (project/plan/phase/task standing instructions, v2.3.1). Different concept; no collision.
- **Historical documents are not rewritten.** `docs/v2.1-ergonomics.md` keeps its title "ipman v2.1 — human ergonomics" because that is the historical name of that release. Forward-looking prose inside is updated.

## 4. Taxonomy

### 4.1 The four buckets

```
JSON in/out (canonical, complete)        → ops          (77 today: plan.create, task.transition, …)
   │
   ├── 1:1 thin wrapper in CLI           → shortcut    (--start, --show, --log, --close, …)
   └── client-side composite in CLI      → view        (-S, -L, -N, -R)

Outside the op model (escape hatches)    → admin       (-I, -U, --migrate-encrypt, export, sql)
```

### 4.2 Rule of thumb

To classify any current or future CLI command, ask:

> *"Is there a single JSON op that returns / performs exactly this?"*

- **Yes** → `shortcut` (e.g. `--show` → `task.get` or `phase.get`)
- **No, it composes multiple ops** → `view` (e.g. `-N` chains ~8 ops)
- **It does not fit the op model at all** (DB bootstrap, encryption migration, ad-hoc SQL, help text) → `admin`

### 4.3 The two orthogonal axes

The categorization is independent of the existing short/long-form rule. **Both axes apply to every command:**

| Axis | Decides | Example |
|---|---|---|
| Form (short / long / bareword) | How you type it | `-R` ≡ `--render` ≡ `render` |
| Category (op / shortcut / view / admin) | What it does internally | `--render` is a view |

The rename only affects the second axis. Every existing `-X` flag retains its `--word` and bareword equivalents.

## 5. Definitive mapping (verified against `src/main.c` @ commit 9845e91)

### 5.1 Shortcuts (1:1 wrapper over a single JSON op)

| CLI | Backing op | Notes |
|---|---|---|
| `-SH` / `--show` / `show` | `task.get` or `phase.get` | Selector kind drives which op is dispatched |
| `-LG` / `--log` / `log` | `event.list` | `--summary-only` and `--limit` map to op params |
| `--start` | `task.transition({status:"in_progress"})` | |
| `--close` | `task.close` | Includes closure record (summary, lessons, validations, decisions) |
| `--cancel` | `task.cancel` (resolution=`canceled`) | Verb does not expose other resolutions |
| `--defer` | `task.defer` | |
| `--close-phase` | `phase.close` (`outcome=completed`) | v2.2 |
| `--cancel-phase` | `phase.close` (`outcome=canceled`) | v2.2. No standalone `phase.cancel` op — same op, different `outcome` param |
| `--current` | `task.set_current` or `phase.set_current` | Auto-detected by selector kind |
| `--activate` | `plan.activate` | |

### 5.2 Views (client-side composition of multiple ops)

| CLI | Composes | Notes |
|---|---|---|
| `-S` / `--status` / `status` | `workspace.context_get` + `task.list` (pending) | 2 ops |
| `-L` / `--ls` / `ls` | `workspace.context_get` + `task.list` (pending, current plan) | 2 ops |
| `-N` / `--next` / `next` | `workspace.context_get` + `plan.get` + `phase.get` + `task.get` + `instruction.list` ×3 + `task.list` | ~8 ops; the canonical handoff view |
| `-R` / `--render` / `render` | Walks the entire plan tree and emits Markdown | Many ops |

### 5.3 Admin (outside the op model)

| CLI | What it does | Why not a shortcut |
|---|---|---|
| `-I` / `--init` / `init` | Bootstraps `./.ipman/`, creates encrypted DB, writes `START-HERE.md` | Pre-DB; no op exists until init runs |
| `-U` / `--usage` / `usage` | Prints help text | Documentation, not data |
| `-V` / `--version` / `version` | Prints binary version | Build metadata |
| `--migrate-encrypt` | Converts plaintext DB → encrypted DB in place | One-shot file operation |
| `export --plaintext --i-understand <out.db>` | Dumps to plaintext SQLite for emergency recovery | Escape hatch by design |
| `sql "SQL..."` | Ad-hoc SQL escape hatch | Bypasses the op layer entirely |

### 5.4 Modifiers (orthogonal flags, not commands)

These are **flags that combine with commands**, not commands themselves. They retain their current behavior and documentation but get an explicit "modifier" callout so readers do not mistake them for shortcuts.

| Flag | Modifies | Effect |
|---|---|---|
| `--dry-run` | Any write shortcut | Prints the JSON envelope that would be sent; does not touch DB |
| `-B` / `--b64` | JSON input on stdin | Decodes base64 before parsing |

## 6. User-facing narrative

The README dual-surface table is replaced by a three-row block (plus an admin footnote):

> **ipman exposes the same data three ways. Pick the one that fits the call site:**
>
> - **`ops`** (JSON in / JSON out) — the complete, canonical surface. 77 operations covering every read and every mutation. This is what AI agents and scripts call.
> - **`shortcuts`** (CLI verbs) — thin wrappers over a single op. Use them when supervising directly: start a task, close it with a summary, defer one with a reason. Each shortcut is exactly one op under the hood, so the audit trail is identical to the agent-driven path.
> - **`views`** (CLI verbs) — compose multiple ops into a single rendered display. `-N` (the handoff view) replaces a sequence of 8 envelopes with one call. Views save typing for humans; agents that need the constituent data should call the underlying ops directly.
>
> Setup, maintenance, and emergency escape hatches live under **`admin`** (`init`, `--migrate-encrypt`, `export --plaintext`, `sql`). They sit outside the op model on purpose.

## 7. Files to update

### 7.1 Documentation

| File | What changes |
|---|---|
| `README.md` | (a) Replace the two-column table at line 135 with the three-bullet narrative from §6 (or a three-row table; pick whichever renders cleaner). (b) Section heading at line 273: "Human CLI reference" → "CLI reference"; subsections reorganized into "Shortcuts" / "Views" / "Admin" (current "Read-only" / "Write" / "Context" / "Setup" / "Maintenance" headings collapse into the new taxonomy — see §7.4 for the mapping). (c) TOC entry at line 28: "Human CLI reference" → "CLI reference". (d) Prose updates at lines 7, 15, 129, 143, 469: "human CLI" / "human surface" / "humans who supervise them" — the surface terms get renamed; "humans who supervise" stays (refers to the operator). |
| `.claude/skills/ipman/SKILL.md` | Section 5 already titled "CLI shortcuts (v2.1+ ergonomics)" — split the current "Read shortcuts" subsection into "Read shortcuts" (`-SH`, `-LG`) and "Read views" (`-S`, `-L`, `-N`, `-R`). At line 73, replace "the human-CLI veneer" with "the CLI shortcuts and views". |
| `docs/v2.2-ergonomics.md` | Line 3: "human discovery surface" → "CLI discovery surface". Line 25: "the human CLI had `--start`, `--close`, `--cancel`, and `--defer`…" → "the CLI shortcuts had `--start`, `--close`, `--cancel`, and `--defer`…". Line 145: "Short human label" → keep ("human" here describes the label's readability, not the surface). |
| `docs/v2.1-ergonomics.md` | Title stays at line 1 (historical). Line 3: "thin CLI veneer over that protocol so a supervising human" — the "supervising human" stays (refers to the operator); "CLI veneer" stays (it is precisely correct). Line 185: "teach the mapping to a human reviewer" stays (refers to a person). No edits needed in this file beyond a possible one-line introductory note pointing readers at the new taxonomy in the README. |
| `docs/v2-migration.md` | Line 30: "human CLI display" → "CLI display". Line 136: "human-friendly display string" stays (describes data property, not surface). |
| `docs/notes-and-implicit-scope.md` | No "human" mentions found in this file (verified via grep). No changes required. |

### 7.2 Code (comments and embedded strings only)

| File | What changes |
|---|---|
| `src/main.c` | Update only these comments (all describe the CLI surface): line 623 `---- human CLI commands ----` → `---- CLI shortcuts and views ----`; line 834 `--start <selector>: human verb to mark a task in_progress` → `--start <selector>: shortcut for task.transition({status:"in_progress"})`; line 1272 `The human verb does not expose the other resolutions` → `The shortcut does not expose the other resolutions`; line 1495 `Resolves a task selector and dispatches task.defer. The human verb` → `Resolves a task selector and dispatches task.defer. The shortcut`; line 1583 `--current <selector>: human verb to set the current task or phase` → `--current <selector>: shortcut to set the current task or phase`; line 1634 `--activate <selector>: human verb to set the active plan for the workspace` → `--activate <selector>: shortcut to set the active plan for the workspace`. **Stays unchanged:** line 134 `Human-readable nudge` (output style, not the surface) and line 1497 `audit trail stays human-readable` (data property, not the surface). |
| `src/cli_selector.h` | `CLI-side resolver for the v2.1 human surface.` → `CLI-side resolver for the v2.1 shortcuts and views.`; `the human verbs need to translate user-friendly arguments` → `the CLI shortcuts and views need to translate user-friendly arguments`. |
| `tests/unit/test_cli_selector.c` | See §7.5 below. |
| `src/agent_docs.c` | Lines 458, 464: replace "human CLI" with "CLI shortcuts and views" in the agent-facing top-level overview. **Stays unchanged:** lines 161, 505, 512, 1265 (`human-readable handle / slug / display`) — they describe data properties, not the surface. |

### 7.3 README section reorganization (current → new)

The current `Human CLI reference` section in `README.md` has these subsections (lines 279–349):

```
### Read-only        →  ### Shortcuts                       (-SH, -LG)
                        ### Views                           (-S, -L, -N, -R)
### Write            →  ### Shortcuts (write)               (--start, --close, --cancel, --defer,
                                                             --close-phase, --cancel-phase)
### Context          →  (merged into Shortcuts (write))     (--current, --activate)
### Setup            →  ### Admin                           (-I, -U, -V)
### Agent protocol   →  ### Agent protocol                  (kept as-is — this is the JSON wire)
### Maintenance      →  (merged into Admin)                 (--migrate-encrypt, export, sql)
### Examples         →  ### Examples                        (kept; refresh language)
```

Final section order under `## CLI reference`:
1. Shortcuts (read)
2. Views
3. Shortcuts (write)
4. Admin
5. Agent protocol
6. Examples

The `--dry-run` modifier is documented at the end of "Shortcuts (write)" with a "Modifiers" subhead (or inline note) — it is not a command in its own right.

### 7.4 Files explicitly NOT touched

- Any `*.c` / `*.h` symbol names (functions, types, enums, macros). Comment-only changes.
- Any JSON op handler (`*_ops.c`, `dispatch.c`, schemas under `.ipman/`).
- The `agent_docs` payload entries for the `instruction` entity (no collision with the surface rename).
- `Makefile`, `migrations/`, `mcp/`, `third_party/`.
- Embedded short labels in error messages — the codes are part of the protocol; we do not touch them.

### 7.5 Test that needs an update

`tests/unit/test_cli_selector.c` line 4: `the v2.1 human-surface selector` → `the v2.1 shortcuts/views selector`. Comment-only; no test logic changes.

## 8. Backward compatibility

- **Wire protocol:** unchanged. `protocol_version` stays at `2`. Every op name, params shape, and response shape is identical.
- **CLI behavior:** unchanged. Every flag (`-N`, `--render`, `--start`, …) keeps its current binding and output. Both short and long forms remain interchangeable.
- **Agents:** the `agent_docs.c` strings get terminology updates but the structured operation catalog is untouched. Agents that parse `--agent-docs` JSON keep working; agents that read the prose see the new vocabulary.
- **`.ipman/` regenerated docs:** the generator (`workspace.refresh_agent_docs`) regenerates from current source, so `.ipman/START-HERE.md`, `.ipman/indexes/*.md` etc. naturally pick up the new wording on the next refresh. No migration step required.

## 9. Verification plan

1. **Build:** `make clean && make` succeeds with zero warnings.
2. **Tests:** `make test` (or the project's test target) passes. `tests/unit/test_cli_selector.c` is the only test file with a comment change; behavior is unchanged.
3. **`ipman --usage`:** Output reflects the new section headers (Shortcuts / Views / Admin) and references the new vocabulary.
4. **`ipman --agent-docs`:** Top-level overview text uses "CLI shortcuts and views" instead of "human CLI". Per-entity payloads unchanged.
5. **`ipman -N` smoke test:** Run inside a workspace; output identical to pre-rename behavior.
6. **Manual README pass:** Render in a Markdown viewer; tables and TOC look right.

## 10. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Agents have memorized the term "human CLI" and expect to see it in docs | The vocabulary is replaced, but the operational guidance is identical. The new term is more descriptive. Cost of one-time relearn is low. |
| README diff is large and noisy | All changes are content-organizational; no code in the diff. Reviewers can scan headings to confirm shape. |
| Some borderline phrases ("supervising human", "human-readable") get over-renamed and lose meaning | Categorization in §7.2 distinguishes phrases about *the surface* (rename) from phrases about *a person* or *a data property* (keep). |
| The `docs/v2.1-ergonomics.md` title "human ergonomics" creates inconsistency | Accepted. Historical release docs are versioned snapshots; rewriting them is revisionism. The current README and SKILL.md are what users land on first. |

## 11. Rollout

Single commit on `main` with conventional message (version label resolved at commit time):

```
docs(v2.x.x): rename "human surface" to shortcuts/views/admin taxonomy

Replaces the dual-surface ("agent surface" / "human surface") framing
with a three-way taxonomy that names what each CLI command actually
does. Pure docs and comments — no flag, op, schema, or symbol renames.
```

No PR (user works on `main` directly per repo convention). **No push without explicit user authorization.**
