/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_GIT_HELPERS_H
#define IPMAN_GIT_HELPERS_H

#include <stddef.h>

/* Write the current git branch name into out (e.g. "main", "feature/foo").
 * Reads .git/HEAD directly; no subprocess is spawned.
 * Returns 0 on success, -1 if not in a git repo or if HEAD is detached. */
int ipman_git_current_branch(char *out, size_t outlen);

#endif
