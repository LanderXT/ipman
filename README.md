<div align="center">

<img src="ipman-logo.png" alt="ipman — Implementation Plan Manager" width="220">

# ipman — Implementation Plan Manager

**Durable, encrypted plan-tracking for AI coding agents and the humans who supervise them.**

Single C binary · SQLCipher-encrypted SQLite · JSON-on-stdin protocol · Linux

</div>

---

`ipman` is an implementation-plan database with two surfaces: a **JSON request/response protocol** for AI agents (Claude Code, Codex, your own) and a **terse human CLI** (`ipman -S`, `ipman -L`, `ipman --render`) for the operator. Plans live in an encrypted SQLite file inside the project (`./.ipman/ipman.db`), so context survives between agent sessions, between agents, and between you and the agent.

It is the missing piece for agents that already write good code but forget what they are doing the moment the conversation compacts.

## Table of contents

- [Why ipman?](#why-ipman)
- [Quickstart](#quickstart)
- [Two surfaces, one database](#two-surfaces-one-database)
- [Concepts](#concepts)
- [How agents discover the API](#how-agents-discover-the-api)
- [Security](#security)
- [Human CLI reference](#human-cli-reference)
- [Operations reference](#operations-reference)
- [Building from source](#building-from-source)
- [Project layout](#project-layout)
- [Status and roadmap](#status-and-roadmap)
- [Contributing](#contributing)
- [License](#license)

## Why ipman?

Modern coding agents are good at executing one task; they are bad at remembering *why* they were doing it. Markdown TODO files rot. Issue trackers are not local. Conversation history evaporates on `/compact`. Agents that work for hours need durable, structured state that they themselves can read and write.

`ipman` is shaped around that need:

- **Plans, phases, and tasks** are first-class objects with stable identifiers (`plan_1`, `phase_3`, `task_42`) — agents can reference them across sessions without scanning prose.
- **Closure records** preserve outcomes: when a task is closed, cancelled, deferred, or replaced, the audit trail keeps the *why*, not just the new status.
- **Standing instructions** attach durable guidance to a plan or phase ("preserve backward compatibility until v2"), so future agents pick up the constraints automatically.
- **Encrypted at rest** by default — your in-progress work and design notes never sit in plaintext on disk.
- **Single static-ish binary**: one process, one SQLite file, no daemon, no server, no network.
- **Self-documenting**: after `init`, the `.ipman/` directory contains per-operation request schemas the agent can read instead of guessing.

If you have ever asked an agent "what was the plan again?" three turns in a row, this is the shape of the fix.

## Quickstart

### 1. Install

Linux (Debian/Ubuntu shown; adapt for your distro):

```sh
sudo apt install build-essential pkg-config libsqlcipher-dev libsodium-dev
git clone <your-fork-url> ipman && cd ipman
make BUILD=release
make install                     # installs to ~/.local/bin/ipman
                                 # plus skill bundles for Claude Code and Codex
```

Confirm it works:

```sh
ipman -U                         # show usage
```

### 2. Initialize a workspace

`ipman` operates per-project. From your project root:

```sh
ipman init
```

That creates `./.ipman/` (mode `0700`) with the encrypted database, the per-project key salt, and a generated `START-HERE.md` for any agent that opens the directory.

### 3. Create a plan

Either as a human:

```sh
ipman --status                   # nothing yet — no active plan
```

…or as an agent (the actual primary use case), via JSON on stdin:

```sh
echo '{"protocol_version":1,"request_id":"r1","actor":"agent",
       "op":"plan.create",
       "params":{"title":"Ship login refactor",
                 "summary":"Split auth from session handling",
                 "priority":"high"}}' | ipman
```

Response:

```json
{"request_id":"r1","ok":true,"result":{"plan":{
  "uid":"plan_1","label":"ship-login-refactor","id":1,"code":"P1",
  "title":"Ship login refactor","status":"open","priority":"high",
  "created_at":"2026-04-30T12:33:11.373Z", ...}}}
```

After a few more `plan.activate`, `phase.create`, and `task.create` calls, the human view becomes:

```
$ ipman -S
┌───────────────┬──────────────────────────┐
│     Field     │           Value          │
├───────────────┼──────────────────────────┤
│ Active plan   │ P1 · Ship login refactor │
├───────────────┼──────────────────────────┤
│ Current phase │ none                     │
├───────────────┼──────────────────────────┤
│ Current task  │ none                     │
├───────────────┼──────────────────────────┤
│ Pending tasks │ 2                        │
└───────────────┴──────────────────────────┘

$ ipman -L
┌───────────────────────────────────────────┬──────────┬────────┬────────────────────────────────┐
│                   Label                   │ Priority │ Status │             Title              │
├───────────────────────────────────────────┼──────────┼────────┼────────────────────────────────┤
│ move-jwt-verification-into-middleware     │ medium   │ todo   │ Move JWT verification ...      │
│ replace-session-cookie-storage-with-redis │ medium   │ todo   │ Replace session cookie ...     │
└───────────────────────────────────────────┴──────────┴────────┴────────────────────────────────┘
```

That is the full feedback loop: the agent edits the plan, you read it, you push back, the agent picks up the changes.

## Two surfaces, one database

The same data is reachable two ways. Pick the surface by who is calling.

| | **Agent surface** | **Human surface** |
|---|---|---|
| Invocation | `… \| ipman` (JSON envelope on stdin) | `ipman <subcommand>` |
| Output | JSON on stdout, exit code via [error semantics](#error-semantics) | Box-drawn tables on stdout, errors on stderr |
| Operations | All 61 operations (`plan.create`, `task.transition`, `closure.get`, …) | Read-only views: `status`, `ls`, `show`, `log`, `render` |
| Mutations | Yes | No — humans steer agents, not the database |
| Use case | Agent-driven planning, transitions, comments | Inspection, review, supervision |

The agent does the writes; the human watches.

## Concepts

### Hierarchy

```
Plan ── one per implementation effort, holds outcome and tags
 └── Phase ── ordered milestones inside a plan
      └── Task ── atomic units of work; carry status, type, priority, origin
```

Every entity carries identifier fields that fall into **three layers**: the row's identity, the handles you reference it by, and the breadcrumb that shows it on screen.

| Layer | Field(s) | What it is and when to use it |
|---|---|---|
| **Identity** | `id` | The internal SQLite primary key (e.g. `42`). Use when an op asks for `id`. Workspace-scoped — two workspaces both have a `task_1`. |
| **Handles** | `uid`, `label`, `code` | Three ways an op can accept the entity as a selector. `uid` (`task_42`) is type-prefixed and immutable — preferred for agent-to-agent references. `label` (`move-jwt-verification`) is a human-friendly slug, scoped per parent and renameable — preferred in prose and comments. `code` (`P1`) exists only on plans — short shorthand for the most-referenced entity. |
| **Breadcrumb** | `entity_ref` (and `*_ref` variants) | Computed at read time, emitted on `event.list` results and task relations as `P1`, `P1/F3`, `P1/T7`. Output-only: derived from mutable upstream state (`phase.move` rewrites `P1/F3` → `P1/F2`), so do not store it and **do not pass it back as a selector**. |

Quick guide: pass `uid` between agents and across sessions; type `label` in comments; show `entity_ref` to humans.

### Status, resolution, and origin are different things

A common bug in self-rolled trackers is conflating these:

- **`status`** — where the entity is *now* (`todo`, `in_progress`, `done`, `canceled`, …).
- **`resolution`** — *how* a terminal state was reached (`completed`, `not_planned`, `discarded`, `duplicate`).
- **`origin_type`** — *where* the work came from (`planned`, `discovered`, `requested`).

Closure operations (`task.close`, `task.cancel`, `task.replace`, `task.mark_duplicate`) set the resolution explicitly so the audit trail can answer "did we actually finish, or did we drop it?".

### Audit trail and closure memory

Every state transition emits an event. Every terminal transition writes a `closure_record` capturing:

- `outcome_summary` — what happened in one line
- `closing_comment` — the note the agent or human attached
- `lessons_learned` — durable knowledge worth carrying into the next plan
- `open_items_summary` — what was deliberately *not* done

Re-opened entities preserve their previous closure record, so an agent that resumes work two weeks later can read why it was paused.

### Standing instructions

Use `instruction.add` for durable guidance ("never modify migrations after merge", "this plan must preserve backward compatibility"). Use `comment.add` for conversational notes and decisions in flight. The two are deliberately distinct surfaces: agents reading at session start are pointed at instructions first.

## How agents discover the API

`ipman init` (and the periodic `workspace.refresh_agent_docs`) writes auto-generated documentation into `.ipman/`:

```
.ipman/
├── START-HERE.md                        # entry point for any agent
├── manifest.json                        # machine-readable op list
├── indexes/
│   ├── ipman.index.operations.md        # alphabetical
│   ├── ipman.index.by-entity.md         # grouped by plan/phase/task/...
│   └── ipman.index.by-workflow.md       # grouped by intent (handoff, closure, ...)
├── operations/
│   └── ipman.op.<entity>.<verb>.schema.md      # human-readable per-op schema
└── schemas/
    └── ipman.op.<entity>.<verb>.request.schema.json   # JSON Schema for the request
```

These files are regenerated from the binary on every `init`, so the documentation cannot drift from the runtime — there is an integration test (`tests/integration/000_operation_docs_parity.sh`) that fails the build if it does.

The bundled skill at `.claude/skills/ipman/SKILL.md` instructs agents to read those files instead of guessing parameter shapes. Drop the skill into your Claude Code or Codex install (`make install-skills` does this) and your agent will know how to use `ipman` on first contact.

### Error semantics

| Code | Meaning | Exit code |
|---|---|---|
| `invalid_request` | Malformed JSON or envelope | 1 (fatal) |
| `internal_error` | Database/IO failure | 1 (fatal) |
| `unknown_op` | Op name not registered | 0 (semantic) |
| `validation_failed` | Bad params or business-rule violation | 0 (semantic) |
| `not_found` | Entity missing | 0 (semantic) |
| `conflict` | Bad state transition, duplicate, circular reference | 0 (semantic) |

The exit-code split lets agents distinguish "the request itself is broken — stop retrying" from "the request ran and reported a normal failure mode — read `error.code`".

## Security

`ipman.db` is encrypted at rest using **SQLCipher** (AES-256 page-level encryption) with a key derived from a per-workspace 32-byte random salt via **libsodium's Argon2id** KDF. Specifically:

- The salt lives in `.ipman/keysalt` (mode `0600`).
- The KDF passphrase is derived from `IPMAN_KEY` if set, else from a stable per-user secret in your home directory.
- `.ipman/` itself is created with mode `0700` and the binary refuses to run if the directory has loose permissions.
- For emergency inspection, `ipman export --plaintext --i-understand <out.db>` writes a plaintext SQLite copy. The `--i-understand` flag is mandatory and the operation is logged.
- For migrating existing plaintext databases (early dev builds), `ipman --migrate-encrypt` rewrites the file in place.

This is *defense at rest*, not a sandbox. Anyone who can run `ipman` as your user can read the database. Treat it like an SSH key.

## Human CLI reference

```
ipman -I  / --init                  Initialize workspace
ipman -S  / --status                Active plan, current phase, current task, pending count
ipman -L  / --ls                    List pending tasks for the active plan
ipman -SH / --show <selector>       Detail for a task or phase (uid, label, or id)
ipman -LG / --log                   Recent workspace events
ipman -R  / --render <plan>         Render plan as Markdown (code, uid, label, or id)
ipman -U  / --usage                 Show full help

Maintenance:
  --migrate-encrypt                 Convert a plaintext ipman.db to encrypted
  export --plaintext --i-understand <out.db>
                                    Emergency dump to plaintext SQLite
  sql "SQL..."                      Ad-hoc SQL escape hatch (developer only)

Agent protocol:
  ipman < request.json              JSON request/response on stdin/stdout
  -B / --b64 < request.b64          Same, with base64-encoded JSON input
```

Each form is interchangeable: `ipman -S` ≡ `ipman status` ≡ `ipman --status`.

## Operations reference

The runtime exposes 61 operations across nine entities. The full, always-current list lives at `.ipman/indexes/ipman.index.operations.md` after init; here is the shape:

| Entity | Common verbs |
|---|---|
| `plan` | `create`, `activate`, `update`, `list`, `get`, `progress`, `history`, `comment_add`, `archive`, `close`, `reopen`, `export`, `deactivate` |
| `phase` | `create`, `update`, `move`, `list`, `get`, `list_tasks`, `progress`, `history`, `comment_add`, `close`, `reopen`, `set_current`, `clear_current` |
| `task` | `create`, `update`, `move`, `list`, `get`, `transition`, `defer`, `cancel`, `replace`, `mark_duplicate`, `close`, `reopen`, `comment_add`, `assign`, `unassign`, `set_current`, `clear_current`, `set_priority`, `set_type`, `set_origin`, `link_dependency`, `unlink_dependency`, `link_external` |
| `comment` | `add`, `list`, `update`, `invalidate` |
| `instruction` | `add`, `list`, `update`, `invalidate` |
| `event` | `list` |
| `closure` | `get` |
| `workspace` | `context_get`, `refresh_agent_docs` |
| `noop` | `noop` (round-trip / health check) |

Read `.ipman/operations/ipman.op.<entity>.<verb>.schema.md` for the parameters of any single operation.

## Building from source

### Requirements

- Linux (POSIX-targeted; not currently tested on macOS or BSD)
- A C11 compiler (`gcc` or `clang`)
- `make`, `pkg-config`
- `libsqlcipher-dev` (provides the SQLite C API plus AES page encryption)
- `libsodium-dev` (provides Argon2id)
- `bash`, `xxd`, `sha256sum` (used by the migration/skill embedders)

### Build targets

| Command | What it does |
|---|---|
| `make` | Dev build (`-O0`, `-Werror`, debug symbols) |
| `make BUILD=release` | Release build (`-O2`, stripped) |
| `make test` | Build, then run unit and integration tests |
| `make install` | Install binary to `$BINDIR` (default `~/.local/bin`) plus Claude/Codex skill bundles |
| `make install-skills` | Install only the agent skills, no binary |
| `make run` | Init a throwaway workspace and call `noop` (smoke test) |
| `make clean` | Remove `build/` |

Override paths with `PREFIX=`, `BINDIR=`, `CLAUDE_SKILLDIR=`, `CODEX_SKILLDIR=`, or `DESTDIR=` for staged installs.

### Testing

```sh
make test
```

Runs three integration scripts:

- `000_operation_docs_parity.sh` — every registered op has a generated schema and vice versa
- `010_schema_allowlist_behavior.sh` — request validation rejects unknown params
- `020_instruction_ops.sh` — instruction lifecycle round-trip

Plus unit tests under `tests/unit/`.

## Project layout

```
ipman/
├── src/                  # All C sources — one .c/.h pair per module
│   ├── main.c            # Entry point, human CLI, argv routing
│   ├── dispatch.c        # Operation registry (single source of truth)
│   ├── protocol.c        # JSON envelope + error codes
│   ├── db.c              # SQLCipher connection setup
│   ├── migrations.c      # Embedded-migration runner
│   ├── *_ops.c           # One file per entity (plan, phase, task, …)
│   └── agent_docs.c      # Per-op schema and index generators
├── migrations/
│   └── 0001_initial_schema.sql   # Consolidated baseline schema
├── scripts/              # Build-time embedders (sql, skill)
├── tests/
│   ├── integration/      # Shell-driven black-box tests
│   └── unit/             # C unit tests
├── third_party/cjson/    # Vendored cJSON (tag-pinned)
├── .claude/skills/ipman/ # Bundled Claude Code skill (also baked into the binary)
├── Makefile
└── ipman-logo.png
```

The "single source of truth" pattern is deliberate: the dispatch table in `src/dispatch.c` drives the operation list, the `OperationSpec` table in `src/agent_docs.c` drives the generated docs, and the parity test fails if they diverge.

## Status and roadmap

**v1.0** — initial public release. Stable surfaces:

- JSON request/response protocol (`protocol_version: 1`)
- All 61 operations
- Encrypted SQLite storage layout
- `.ipman/` generated documentation tree
- Bundled Claude Code skill

Planned (no commitment yet):

- macOS build support
- Codex skill parity tests
- Optional plain-SQLite mode for environments where SQLCipher is hard to install
- Additional task relationship types (`blocks`, `informs`, …)

Breaking changes will bump `protocol_version` and ship a migration; the embedded migration runner already supports the upgrade path.

## Contributing

Bug reports and patches welcome. Before opening a PR:

1. `make test` passes locally.
2. New operations include a schema entry in `OperationSpec` and a regression test.
3. Schema migrations are append-only — never edit a shipped migration in place.
4. The README and SKILL.md stay in sync with the binary (the parity test catches operation drift; doc text is on you).

The code follows a "no surprises" style: small functions, explicit error returns (`int rc`), no global state, no clever macros. Match the surrounding style and you will be fine.

## License

`ipman` is released under the **Apache License, Version 2.0**. See [`LICENSE`](LICENSE) for the full text and [`NOTICE`](NOTICE) for third-party attribution. Source files carry an SPDX header (`SPDX-License-Identifier: Apache-2.0`).

This means you can use, modify, embed, and redistribute `ipman` (including in commercial and closed-source products), provided you preserve attribution and the license text. Apache-2.0 also includes an explicit patent grant — see Section 3 of the License.
