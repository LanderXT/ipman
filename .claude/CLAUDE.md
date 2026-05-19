
<!-- ipman-managed -->
## ipman workspace

After `ipman init`, the following must be in `.gitignore`:

- `.ipman/ipman.db` — binary SQLite file; no meaningful diff.
- `.ipman/keysalt` — 32-byte secret; committing it defeats encryption.
- `.ipman/ipman.db.bak` — plaintext backup; large and unencrypted.

`ipman init` manages these entries automatically.

## Project instructions

Before acting on any task in this repository, read the project-level instructions stored in ipman. They are workspace-wide constraints that survive plan lifecycle and apply to every implementation decision — ignoring them produces incorrect outcomes regardless of what the task says.

The fastest path: `ipman -N` or `workspace.context_get` returns them under `context.project.instructions`. Instructions with `priority=critical` are non-negotiable.

## Commit hygiene

`task.close` is metadata; the git commit is the durable artifact. They drift apart unless every closure references the commit(s) that delivered the work.

- **Commit before close.** Implement → `git commit` → `task.close`. Never close a code-touching task whose work is still uncommitted.
- **Subject prefix:** `<plan.code>/T<local_seq>: <summary>` — e.g., `P13/T4: extract auth guard`. Both `plan.code` and `task.local_seq` are returned by `task.get` and surfaced in `ipman -N`. Greppable: `git log --grep "^P13/"`.
- **SHA in the closure.** `outcome_summary` must contain a `commit: <sha>` line. Multiple commits batched into one task: `commit: <sha1>, <sha2>`. Genuinely code-less task (research, decision, doc bundled elsewhere): `no-commit: <reason>`.
- **One feature branch per plan.** `feat/<plan.code>-<slug>` (e.g., `feat/P13-audit-fixes`). Don't work branchlessly on `main` — branchless work pools changes across tasks and turns audits into archaeology.

Why this is critical, not stylistic: without the SHA link, "did the closed tasks actually ship?" requires re-reading every diff. Plans #11–#13 are the historical failure mode this rule prevents.
