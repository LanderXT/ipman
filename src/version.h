/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_VERSION_H
#define IPMAN_VERSION_H

/*
 * Binary release version, semver-ish. Bumped per release; the protocol
 * version is independent (see protocol.c) and only changes on a wire-
 * breaking cutover. v2.2 is purely additive over the v2.0/v2.1 wire — see
 * docs/v2.2-ergonomics.md.
 */
#define IPMAN_VERSION "2.2.0"

#endif
