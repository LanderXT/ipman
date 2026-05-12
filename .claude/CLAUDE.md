
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
