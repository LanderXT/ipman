/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_SKILL_INSTALL_H
#define IPMAN_SKILL_INSTALL_H

#include <stddef.h>

/*
 * Embedded SKILL.md bytes, produced by scripts/embed_skill.sh at build time
 * from .claude/skills/ipman/SKILL.md. The array is NUL-terminated; _len
 * excludes the terminator.
 */
extern const unsigned char ipman_skill_md[];
extern const size_t        ipman_skill_md_len;

/*
 * Ensure project-local copies of the ipman skill exist for supported agents.
 *
 * Policy: for each supported target, if the matching user-global install
 * exists, do nothing (respect it). Otherwise write the workspace-local
 * copy under the current directory (mode 0644), creating any missing parent
 * directories with mode 0755.
 *
 * Returns 0 on success or when no action is required, -1 if any required
 * workspace-local copy could not be written. The caller logs but does not
 * fail the whole init — the workspace is still usable without a local skill.
 */
int ipman_skill_install(void);

#endif
