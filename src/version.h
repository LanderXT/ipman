/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_VERSION_H
#define IPMAN_VERSION_H

/*
 * Binary release version, semver-ish. Bumped per release; the protocol
 * version is independent (see protocol.c) and only changes on a wire-
 * breaking cutover. v2.2 was purely additive over v2.0/v2.1; v2.3 added
 * the project entity plus the tools and env_vars registry; v2.3.1 added
 * project-scoped instructions; v2.3.2 closes ergonomic gaps (init
 * auto-names the project, --next renders the project block,
 * workspace.export op is the companion to plan.export); v2.4 adds
 * UTF-8 slug transliteration, refactors --usage to a byte-array embed,
 * and ships --import-plan with saga rollback; v2.4.1 closes the
 * orphan-doc drift gap by evicting stale agent-docs to .ipman/.attic
 * with refuse-on-limit; v2.4.2 closes the silent-fork hazard by
 * walking upward from cwd to find the repo root and refusing `init`
 * from a subdir or worktree subdir; v2.4.3 hardens the migration
 * window — toggles foreign_keys around each migration's transaction
 * (unblocks v3->v4 upgrades on populated DBs), runs pre-flight
 * integrity_check + foreign_key_check before mutating, and snapshots
 * ipman.db to ipman.db.bak-v<current> before applying pending
 * migrations. v2.5 adds branch-aware workspace context: a new
 * branch_contexts table (migration 0006), ipman_git_current_branch reads
 * .git/HEAD without spawning a subprocess, plan.activate upserts a git
 * branch → plan binding, workspace.context_get auto-switches the active
 * plan when the user changes branches, and two new ops
 * (workspace.list_branch_bindings, workspace.unbind_branch) expose the
 * bindings for inspection and cleanup. The -N handoff view shows the
 * current branch and binding status. All releases stay wire-additive at
 * protocol_version: 2. See docs/ for per-release notes.
 */
#define IPMAN_VERSION "2.5.0"

#endif
