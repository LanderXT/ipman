/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_HOME_H
#define IPMAN_HOME_H

#include <stddef.h>

/*
 * Resolve and prepare the ipman workspace directory.
 *
 * Resolution precedence:
 *   1. $IPMAN_HOME if set and non-empty (operator escape hatch — total
 *      bypass; never validated against repo state).
 *   2. <repo-root>/.ipman where repo-root is found by walking cwd upward
 *      looking for `.git` (either a directory in a normal clone or a file
 *      in a worktree). Closes the silent-fork hazard where an agent
 *      running in a subdir or worktree without IPMAN_HOME would have
 *      auto-initialized a stray .ipman/ at cwd, divorced from the data
 *      the user actually cares about.
 *   3. ./.ipman relative to cwd, when no .git ancestor exists. Preserves
 *      the legacy behavior for ad-hoc use outside any repo.
 *
 * ipman_home_resolve fills `out` with the path (NUL-terminated). The path
 * stays relative when (3) applies so generated manifests are portable.
 *
 * ipman_home_repo_root_relpath returns 0 if cwd resolves to (2); writes
 * the cwd-relative path of the repo root (e.g. "..", "../..") into `out`,
 * or "." when cwd IS the repo root. Returns -1 when (1) or (3) applies,
 * or when no .git ancestor is found. Used by `init` to refuse running
 * from a subdir of a repo and force the agent to acknowledge its
 * position.
 *
 * ipman_home_require verifies that the directory already exists and that
 * its mode has no bits outside 0700 set (world/group access is refused —
 * this directory will hold the user's entire plan history). Returns 0 on
 * success.
 *
 * ipman_home_ensure performs the same validation, creating the directory
 * (mode 0700) if it does not exist.
 */

int ipman_home_resolve(char *out, size_t outlen);
int ipman_home_repo_root_relpath(char *out, size_t outlen);
int ipman_home_require(const char *path);
int ipman_home_ensure(const char *path);

#endif
