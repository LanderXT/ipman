/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_KEY_H
#define IPMAN_KEY_H

#include <stddef.h>

/*
 * Derive the SQLCipher page-encryption key for this workspace.
 *
 * The derivation binds the key to the local machine and the calling user, so
 * the same workspace cannot be opened by another UID or on another host even
 * if the .ipman directory is copied. Inputs:
 *   - /etc/machine-id (fallback /var/lib/dbus/machine-id) — host identifier
 *   - getuid() — calling user's UID
 *   - <home_path>/keysalt — 32 random bytes generated at `ipman init`
 *
 * These are run through Argon2id (libsodium crypto_pwhash) tuned for the
 * INTERACTIVE op/mem profile (~50 ms per derivation on commodity hardware).
 * The output is exactly IPMAN_KEY_BYTES bytes, suitable for hex-encoding into
 *   PRAGMA key = "x'<hex>'";
 *
 * Threat model: this defends against accidental access (other tools opening
 * the file, partial-write corruption flagged by SQLCipher's per-page HMAC).
 * It does NOT defend against a process running as the same UID that
 * reimplements this derivation — which is by design, see plan 2.
 *
 * On success, fills `out` with IPMAN_KEY_BYTES bytes and returns 0.
 * On failure, logs a diagnostic on stderr, leaves `out` untouched, and
 * returns non-zero. Failure modes:
 *   - both machine-id paths missing or unreadable
 *   - <home_path>/keysalt missing (caller must run `ipman init` first)
 *   - keysalt has wrong size or loose permissions (any bit outside 0600)
 *   - crypto_pwhash internal failure (out of memory, parameter validation)
 *
 * `out` must point to a buffer of at least IPMAN_KEY_BYTES bytes. The caller
 * is responsible for sodium_memzero on that buffer once the key has been
 * applied to the SQLite handle.
 */

#define IPMAN_KEY_BYTES 32

int ipman_key_derive(const char *home_path, unsigned char *out);

/*
 * Derive a transport key from a user-supplied passphrase.
 *
 * Uses the same Argon2id INTERACTIVE profile as ipman_key_derive so the
 * security margin is identical. The password input is the raw passphrase
 * bytes (not machine-bound), making the resulting key portable across hosts.
 *
 * passphrase / passphrase_len: UTF-8 passphrase, not NUL-terminated.
 * salt: must be exactly crypto_pwhash_SALTBYTES (16) random bytes.
 * out: must point to a buffer of at least IPMAN_KEY_BYTES (32) bytes.
 *
 * Returns 0 on success, -1 on failure (out of memory or sodium_init failure).
 * Caller is responsible for sodium_memzero on `out` after use.
 */
int ipman_key_derive_passphrase(const unsigned char *passphrase,
                                size_t passphrase_len,
                                const unsigned char *salt,
                                unsigned char *out);

/*
 * Ensure <home_path>/keysalt exists with the expected size, mode, and owner.
 *
 * If the file is missing, generate 32 random bytes (libsodium randombytes_buf)
 * and write atomically (tmp → fsync → rename) at mode 0600. Refuses to
 * overwrite an existing keysalt — overwriting would lock out the encrypted
 * database. If an existing keysalt has the wrong size, mode, or owner, returns
 * an error so the caller can surface a corruption diagnostic instead of
 * silently regenerating.
 *
 * Idempotent for the well-formed-existing case. Intended to run during
 * `ipman init` (alongside ipman_home_ensure).
 *
 * Returns 0 on success, non-zero on failure (with a diagnostic on stderr).
 */
int ipman_keysalt_ensure(const char *home_path);

#endif
