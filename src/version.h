/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_VERSION_H
#define IPMAN_VERSION_H

/*
 * Binary release version, semver-ish. Bumped per release; the protocol
 * version is independent (see protocol.c) and only changes on a wire-
 * breaking cutover. v2.2 was purely additive over v2.0/v2.1; v2.3 added
 * the project entity plus the tools and env_vars registry; v2.3.1 is a
 * patch that extends instructions to accept entity_type='project' and
 * surfaces them under context.project.instructions — also wire-additive.
 * See docs/v2.3-tools-and-env-vars.md.
 */
#define IPMAN_VERSION "2.3.1"

#endif
