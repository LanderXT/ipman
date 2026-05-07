/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_MIGRATE_OPS_H
#define IPMAN_MIGRATE_OPS_H

/*
 * One-shot conversion of an existing plaintext SQLite database into a
 * SQLCipher-encrypted one, in place.
 *
 * Reads <home_path>/ipman.db, writes <home_path>/ipman.db.encrypted via
 *   ATTACH ... KEY "x'<derived hex>'" + SELECT sqlcipher_export(...)
 * then atomically renames ipman.db → ipman.db.bak and ipman.db.encrypted →
 * ipman.db. The plaintext .bak is intentionally retained as a safety copy
 * — the operator removes it manually after verifying.
 *
 * Idempotent: if ipman.db is already encrypted (does not start with the
 * SQLite plaintext magic), returns 0 without changes.
 *
 * Refuses to proceed if a stale ipman.db.encrypted exists from a prior
 * crashed run — the operator must remove it first.
 *
 * Returns 0 on success or already-encrypted, non-zero on failure (with a
 * diagnostic on stderr).
 */
int ipman_migrate_encrypt(const char *home_path);

/*
 * Emergency plaintext export: dump the encrypted ipman.db to a plaintext
 * SQLite file at `out_path`. The output is a regular .db file that opens
 * with stock sqlite3, intended for machine-reinstall and key-loss recovery.
 *
 * Refuses to overwrite an existing file at `out_path`. Refuses if `out_path`
 * resolves to the live DB (the source path under home_path). The caller is
 * responsible for the --i-understand gate; this function does not enforce it.
 *
 * Returns 0 on success, non-zero on failure (with a diagnostic on stderr).
 */
int ipman_export_plaintext(const char *home_path, const char *out_path);

/*
 * Export the workspace database as a portable bundle to `out_path`.
 *
 * The bundle format is:
 *   [4 bytes magic "IPMX"][1 byte version 0x01][16 bytes Argon2id salt]
 *   [SQLCipher database encrypted with transport_key]
 *
 * where transport_key = Argon2id(passphrase, salt, INTERACTIVE).
 *
 * The passphrase is read from IPMAN_PASSPHRASE env var (scripting) or
 * interactively from /dev/tty with echo suppressed (interactive). When
 * interactive, the passphrase is requested twice and must match.
 *
 * Returns 0 on success, -1 on failure (diagnostic on stderr).
 */
int ipman_export_portable(const char *home_path, const char *out_path);

/*
 * Import a portable bundle created by ipman_export_portable into `home_path`.
 *
 * Reads and validates the bundle header, derives the transport key from the
 * embedded salt and the user passphrase, then transfers the decrypted data
 * into a newly-initialized workspace at `home_path` using the machine-bound
 * key. Applies migrations after import to handle schema upgrades.
 *
 * Refuses if `home_path` already contains an ipman.db (prevents accidental
 * overwrites; user must choose a different IPMAN_HOME or remove the DB).
 *
 * Returns 0 on success, -1 on failure (diagnostic on stderr).
 */
int ipman_import_portable(const char *home_path, const char *bundle_path);

#endif
