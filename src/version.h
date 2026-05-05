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
 * with refuse-on-limit. All releases stay wire-additive at
 * protocol_version: 2. See docs/v2.4-slug-import-docs.md and
 * docs/v2.4.1-attic-sweep.md for the most recent changes.
 */
#define IPMAN_VERSION "2.4.1"

#endif
