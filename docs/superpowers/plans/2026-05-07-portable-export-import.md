# Portable Export/Import Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `ipman export-portable --output <file>` and `ipman import-portable <file>` commands so a workspace can be moved between machines without losing its encrypted database.

**Architecture:** A 21-byte header (`IPMX` magic + version byte + 16-byte Argon2id salt) is prepended to a standard SQLCipher database encrypted with a user passphrase-derived transport key. Export uses the existing `ATTACH + sqlcipher_export` pattern from `migrate_ops.c`. Import reverses the process: extracts the DB to a temp file, derives the transport key from the header salt + passphrase, transfers into a freshly-initialized workspace using the machine-bound key. Migrations are applied after transfer to handle schema upgrades.

**Tech Stack:** C11, SQLCipher (`ATTACH` + `sqlcipher_export`), libsodium (`crypto_pwhash` Argon2id INTERACTIVE, `randombytes_buf`, `sodium_memzero`, `sodium_memcmp`), POSIX termios for passphrase input.

---

## Bundle format

```
Offset  Len  Field
     0    4  Magic: "IPMX" (0x49 0x50 0x4d 0x58)
     4    1  Version: 0x01
     5   16  Argon2id salt (crypto_pwhash_SALTBYTES = 16 bytes)
    21    N  SQLCipher database bytes encrypted with transport_key
```

`transport_key = crypto_pwhash(passphrase, salt, INTERACTIVE)` — same Argon2id profile as the machine key, different password input.

---

## Task 1: `ipman_key_derive_passphrase()` — unit test + implementation

**Files:**
- Modify: `src/ipman_key.h`
- Modify: `src/ipman_key.c`
- Create: `tests/unit/test_transport_key.c`
- Modify: `Makefile` (add recipe for `test_transport_key`)

- [ ] **Step 1: Write the failing unit test**

Create `tests/unit/test_transport_key.c`:

```c
/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "ipman_key.h"

#include <assert.h>
#include <sodium.h>
#include <string.h>
#include <stdio.h>

static void test_derive_passphrase_deterministic(void) {
    const unsigned char passphrase[] = "hunter2";
    unsigned char salt[crypto_pwhash_SALTBYTES];
    memset(salt, 0xAB, sizeof salt);

    unsigned char key1[IPMAN_KEY_BYTES];
    unsigned char key2[IPMAN_KEY_BYTES];

    assert(ipman_key_derive_passphrase(passphrase, sizeof passphrase - 1,
                                       salt, key1) == 0);
    assert(ipman_key_derive_passphrase(passphrase, sizeof passphrase - 1,
                                       salt, key2) == 0);

    assert(memcmp(key1, key2, IPMAN_KEY_BYTES) == 0);
    printf("PASS: derive_passphrase is deterministic\n");

    sodium_memzero(key1, sizeof key1);
    sodium_memzero(key2, sizeof key2);
}

static void test_derive_passphrase_different_salt(void) {
    const unsigned char passphrase[] = "hunter2";
    unsigned char salt1[crypto_pwhash_SALTBYTES];
    unsigned char salt2[crypto_pwhash_SALTBYTES];
    memset(salt1, 0x11, sizeof salt1);
    memset(salt2, 0x22, sizeof salt2);

    unsigned char key1[IPMAN_KEY_BYTES];
    unsigned char key2[IPMAN_KEY_BYTES];

    assert(ipman_key_derive_passphrase(passphrase, sizeof passphrase - 1,
                                       salt1, key1) == 0);
    assert(ipman_key_derive_passphrase(passphrase, sizeof passphrase - 1,
                                       salt2, key2) == 0);

    assert(memcmp(key1, key2, IPMAN_KEY_BYTES) != 0);
    printf("PASS: different salts produce different keys\n");

    sodium_memzero(key1, sizeof key1);
    sodium_memzero(key2, sizeof key2);
}

static void test_derive_passphrase_different_passphrase(void) {
    const unsigned char pass1[] = "hunter2";
    const unsigned char pass2[] = "hunter3";
    unsigned char salt[crypto_pwhash_SALTBYTES];
    memset(salt, 0x55, sizeof salt);

    unsigned char key1[IPMAN_KEY_BYTES];
    unsigned char key2[IPMAN_KEY_BYTES];

    assert(ipman_key_derive_passphrase(pass1, sizeof pass1 - 1, salt, key1) == 0);
    assert(ipman_key_derive_passphrase(pass2, sizeof pass2 - 1, salt, key2) == 0);

    assert(memcmp(key1, key2, IPMAN_KEY_BYTES) != 0);
    printf("PASS: different passphrases produce different keys\n");

    sodium_memzero(key1, sizeof key1);
    sodium_memzero(key2, sizeof key2);
}

int main(void) {
    if (sodium_init() < 0) {
        fprintf(stderr, "sodium_init failed\n");
        return 1;
    }
    test_derive_passphrase_deterministic();
    test_derive_passphrase_different_salt();
    test_derive_passphrase_different_passphrase();
    printf("All transport key tests passed.\n");
    return 0;
}
```

- [ ] **Step 2: Add recipe to Makefile**

In `Makefile`, after the `test_git_helpers` recipe (around line 219), add:

```makefile
$(BUILD_DIR)/tests/test_transport_key: tests/unit/test_transport_key.c \
		$(BUILD_DIR)/ipman_key.o \
		$(BUILD_DIR)/log.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(SQLCIPHER_CFLAGS) $(SODIUM_CFLAGS) -Isrc -o $@ $^ $(LDLIBS)
```

- [ ] **Step 3: Run tests to confirm they fail**

```bash
make unit 2>&1 | grep -A3 "test_transport_key"
```

Expected: compile error because `ipman_key_derive_passphrase` is not defined yet.

- [ ] **Step 4: Add declaration to `src/ipman_key.h`**

After the `ipman_key_derive()` declaration (line 43), add:

```c
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
```

- [ ] **Step 5: Implement in `src/ipman_key.c`**

Append to the end of `ipman_key.c` (after `ipman_keysalt_ensure`):

```c
int ipman_key_derive_passphrase(const unsigned char *passphrase,
                                size_t passphrase_len,
                                const unsigned char *salt,
                                unsigned char *out) {
    if (sodium_init() < 0) {
        ipman_log_error("sodium_init failed", "rc=-1");
        return -1;
    }

    int rc = crypto_pwhash(out, IPMAN_KEY_BYTES,
                           (const char *)passphrase, passphrase_len,
                           salt,
                           crypto_pwhash_OPSLIMIT_INTERACTIVE,
                           crypto_pwhash_MEMLIMIT_INTERACTIVE,
                           crypto_pwhash_ALG_ARGON2ID13);
    if (rc != 0) {
        ipman_log_error("crypto_pwhash failed (likely out of memory)",
                       "rc=%d", rc);
        return -1;
    }
    return 0;
}
```

- [ ] **Step 6: Run unit tests — confirm they pass**

```bash
make unit 2>&1 | tail -20
```

Expected output includes:
```
PASS: derive_passphrase is deterministic
PASS: different salts produce different keys
PASS: different passphrases produce different keys
All transport key tests passed.
```

- [ ] **Step 7: Commit**

```bash
git add src/ipman_key.h src/ipman_key.c tests/unit/test_transport_key.c Makefile
git commit -m "feat: add ipman_key_derive_passphrase for portable transport keys"
```

---

## Task 2: Passphrase reader

**Files:**
- Create: `src/passphrase.h`
- Create: `src/passphrase.c`

The passphrase reader checks `IPMAN_PASSPHRASE` env var first (used by tests and scripts). If unset, reads from `/dev/tty` with echo suppressed via `termios`.

- [ ] **Step 1: Create `src/passphrase.h`**

```c
/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_PASSPHRASE_H
#define IPMAN_PASSPHRASE_H

#include <stddef.h>

/*
 * Read a passphrase into `out` (size `cap`).
 *
 * Resolution order:
 *   1. IPMAN_PASSPHRASE env var — used by tests and scripts.
 *   2. /dev/tty — interactive, echo suppressed, prints `prompt` first.
 *
 * `out_len` is set to the number of bytes written (not including any NUL).
 * An empty passphrase (length 0) is rejected with an error.
 *
 * Returns 0 on success, -1 on error (message written to stderr).
 * Caller MUST sodium_memzero(out, cap) after use.
 */
int ipman_read_passphrase(const char *prompt, char *out, size_t cap,
                          size_t *out_len);

#endif /* IPMAN_PASSPHRASE_H */
```

- [ ] **Step 2: Create `src/passphrase.c`**

```c
/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "passphrase.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

int ipman_read_passphrase(const char *prompt, char *out, size_t cap,
                          size_t *out_len) {
    /* Test / scripting escape hatch: skip tty interaction entirely. */
    const char *env = getenv("IPMAN_PASSPHRASE");
    if (env != NULL) {
        size_t len = strlen(env);
        if (len == 0) {
            fprintf(stderr, "ipman: IPMAN_PASSPHRASE is set but empty\n");
            return -1;
        }
        if (len >= cap) {
            fprintf(stderr, "ipman: IPMAN_PASSPHRASE too long (max %zu)\n",
                    cap - 1);
            return -1;
        }
        memcpy(out, env, len);
        out[len] = '\0';
        *out_len = len;
        return 0;
    }

    int tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (tty_fd < 0) {
        fprintf(stderr, "ipman: cannot open /dev/tty: %s\n", strerror(errno));
        return -1;
    }

    /* Suppress echo for the duration of the read. */
    struct termios old, noecho;
    if (tcgetattr(tty_fd, &old) != 0) {
        fprintf(stderr, "ipman: tcgetattr failed: %s\n", strerror(errno));
        close(tty_fd);
        return -1;
    }
    noecho = old;
    noecho.c_lflag &= (tcflag_t)~(ECHO | ECHOE | ECHOK | ECHONL);
    if (tcsetattr(tty_fd, TCSAFLUSH, &noecho) != 0) {
        fprintf(stderr, "ipman: tcsetattr failed: %s\n", strerror(errno));
        close(tty_fd);
        return -1;
    }

    /* Write prompt directly to tty (not stdout, which may be redirected). */
    if (prompt != NULL) {
        (void)write(tty_fd, prompt, strlen(prompt));
    }

    size_t pos = 0;
    int rc = 0;
    while (pos < cap - 1) {
        char c;
        ssize_t n = read(tty_fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "\nipman: read passphrase failed: %s\n",
                    strerror(errno));
            rc = -1;
            break;
        }
        if (n == 0 || c == '\n' || c == '\r') break;
        out[pos++] = c;
    }
    out[pos] = '\0';

    /* Restore echo before any early-return path. */
    (void)tcsetattr(tty_fd, TCSAFLUSH, &old);
    (void)write(tty_fd, "\n", 1);
    close(tty_fd);

    if (rc != 0) return -1;

    if (pos == 0) {
        fprintf(stderr, "ipman: passphrase must not be empty\n");
        return -1;
    }

    *out_len = pos;
    return 0;
}
```

- [ ] **Step 3: Build to confirm it compiles**

```bash
make 2>&1 | tail -5
```

Expected: build succeeds (no errors).

- [ ] **Step 4: Commit**

```bash
git add src/passphrase.h src/passphrase.c
git commit -m "feat: add ipman_read_passphrase (termios + IPMAN_PASSPHRASE env)"
```

---

## Task 3: `ipman_export_portable()` in migrate_ops.c

**Files:**
- Modify: `src/migrate_ops.h` (add declaration)
- Modify: `src/migrate_ops.c` (add implementation)

Export writes `[21-byte header][SQLCipher DB encrypted with transport_key]`.

- [ ] **Step 1: Add declarations to `src/migrate_ops.h`**

After the existing declarations in `migrate_ops.h`, add:

```c
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
```

- [ ] **Step 2: Add bundle constants and helpers to `src/migrate_ops.c`**

Add after the existing `#include` block in `migrate_ops.c`:

```c
#include "ipman_key.h"
#include "passphrase.h"

#define IPMX_MAGIC_LEN   4
#define IPMX_HEADER_LEN  (IPMX_MAGIC_LEN + 1 + crypto_pwhash_SALTBYTES)
static const unsigned char kIpmxMagic[IPMX_MAGIC_LEN] = {0x49, 0x50, 0x4d, 0x58};
static const unsigned char kIpmxVersion = 0x01;
```

Then add a static helper `copy_file_bytes()` to stream-copy between two FILE*:

```c
/* Stream-copy all remaining bytes from `src` to `dst` using a 64 KB buffer.
 * Returns 0 on success, -1 on I/O error. */
static int copy_file_bytes(FILE *src, FILE *dst) {
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) return -1;
    }
    return ferror(src) ? -1 : 0;
}
```

And a helper to hex-encode a 32-byte key (already exists as static in migrate_ops.c — reuse it):
> Note: `hex_encode_32` is already a static function defined earlier in `migrate_ops.c` at line 110. Do not add a duplicate; the new code below will use it directly.

- [ ] **Step 3: Implement `ipman_export_portable()` in `src/migrate_ops.c`**

Add after `ipman_export_plaintext()`:

```c
int ipman_export_portable(const char *home_path, const char *out_path) {
    char db_path[PATH_MAX];
    if (join_path(db_path, sizeof db_path, home_path, "ipman.db") != 0) {
        ipman_log_error("path too long", "home=%s", home_path);
        return -1;
    }

    if (strcmp(db_path, out_path) == 0) {
        ipman_log_error("output path equals live DB; refusing", "path=%s", out_path);
        return -1;
    }
    if (access(out_path, F_OK) == 0) {
        ipman_log_error("output file already exists; remove it first",
                       "path=%s", out_path);
        return -1;
    }

    /* Read passphrase (twice when interactive to catch typos). */
    char pass1[1024], pass2[1024];
    size_t pass1_len = 0, pass2_len = 0;
    if (ipman_read_passphrase("Export passphrase: ", pass1, sizeof pass1,
                               &pass1_len) != 0) {
        return -1;
    }

    const char *env_pass = getenv("IPMAN_PASSPHRASE");
    if (env_pass == NULL) {
        /* Interactive: confirm. */
        if (ipman_read_passphrase("Confirm passphrase: ", pass2, sizeof pass2,
                                   &pass2_len) != 0) {
            sodium_memzero(pass1, sizeof pass1);
            return -1;
        }
        if (pass1_len != pass2_len ||
            sodium_memcmp(pass1, pass2, pass1_len) != 0) {
            sodium_memzero(pass1, sizeof pass1);
            sodium_memzero(pass2, sizeof pass2);
            fprintf(stderr, "ipman: passphrases do not match\n");
            return -1;
        }
        sodium_memzero(pass2, sizeof pass2);
    }

    /* Derive transport key. */
    unsigned char salt[crypto_pwhash_SALTBYTES];
    randombytes_buf(salt, sizeof salt);

    unsigned char tkey[IPMAN_KEY_BYTES];
    int krc = ipman_key_derive_passphrase(
        (unsigned char *)pass1, pass1_len, salt, tkey);
    sodium_memzero(pass1, sizeof pass1);
    if (krc != 0) return -1;

    char tkey_hex[65];
    hex_encode_32(tkey, tkey_hex);
    sodium_memzero(tkey, sizeof tkey);

    /* Derive machine key for source. */
    unsigned char mkey[IPMAN_KEY_BYTES];
    if (ipman_key_derive(home_path, mkey) != 0) {
        sodium_memzero(tkey_hex, sizeof tkey_hex);
        return -1;
    }
    char mkey_hex[65];
    hex_encode_32(mkey, mkey_hex);
    sodium_memzero(mkey, sizeof mkey);

    /* Write transport-encrypted DB to a temp file next to the output. */
    char tmp_path[PATH_MAX];
    int tn = snprintf(tmp_path, sizeof tmp_path, "%s.tmp.%ld",
                      out_path, (long)getpid());
    if (tn < 0 || (size_t)tn >= sizeof tmp_path) {
        sodium_memzero(tkey_hex, sizeof tkey_hex);
        sodium_memzero(mkey_hex, sizeof mkey_hex);
        ipman_log_error("tmp path too long", "out=%s", out_path);
        return -1;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(tmp_path, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("cannot open transport-key tmp",
                       "path=%s rc=%d", tmp_path, rc);
        sodium_memzero(tkey_hex, sizeof tkey_hex);
        sodium_memzero(mkey_hex, sizeof mkey_hex);
        return -1;
    }

    char *key_sql = sqlite3_mprintf("PRAGMA key = \"x'%s'\"", tkey_hex);
    sodium_memzero(tkey_hex, sizeof tkey_hex);
    if (key_sql == NULL || run_sql(db, key_sql, "PRAGMA key (transport)") != SQLITE_OK) {
        if (key_sql != NULL) { sodium_memzero(key_sql, strlen(key_sql)); sqlite3_free(key_sql); }
        sodium_memzero(mkey_hex, sizeof mkey_hex);
        sqlite3_close(db);
        unlink(tmp_path);
        return -1;
    }
    sodium_memzero(key_sql, strlen(key_sql));
    sqlite3_free(key_sql);

    char *attach_sql = sqlite3_mprintf(
        "ATTACH DATABASE %Q AS source KEY \"x'%s'\"", db_path, mkey_hex);
    sodium_memzero(mkey_hex, sizeof mkey_hex);
    if (attach_sql == NULL ||
        run_sql(db, attach_sql, "ATTACH source") != SQLITE_OK) {
        if (attach_sql != NULL) { sodium_memzero(attach_sql, strlen(attach_sql)); sqlite3_free(attach_sql); }
        sqlite3_close(db);
        unlink(tmp_path);
        return -1;
    }
    sodium_memzero(attach_sql, strlen(attach_sql));
    sqlite3_free(attach_sql);

    /* Verify source key before committing to the full export. */
    if (run_sql(db, "SELECT count(*) FROM source.sqlite_master",
                "verify source key") != SQLITE_OK) {
        sqlite3_exec(db, "DETACH DATABASE source", NULL, NULL, NULL);
        sqlite3_close(db);
        unlink(tmp_path);
        return -1;
    }

    rc = run_sql(db, "SELECT sqlcipher_export('main', 'source')",
                 "sqlcipher_export portable");
    sqlite3_exec(db, "DETACH DATABASE source", NULL, NULL, NULL);
    sqlite3_close(db);
    if (rc != SQLITE_OK) {
        unlink(tmp_path);
        return -1;
    }

    /* Write bundle: [header][tmp bytes] → out_path */
    FILE *tmp_f = fopen(tmp_path, "rb");
    if (tmp_f == NULL) {
        ipman_log_error("cannot open tmp for bundle assembly",
                       "path=%s errno=%d", tmp_path, errno);
        unlink(tmp_path);
        return -1;
    }

    FILE *out_f = fopen(out_path, "wb");
    if (out_f == NULL) {
        ipman_log_error("cannot create bundle output",
                       "path=%s errno=%d", out_path, errno);
        fclose(tmp_f);
        unlink(tmp_path);
        return -1;
    }

    int write_ok =
        fwrite(kIpmxMagic,      1, IPMX_MAGIC_LEN,               out_f) == IPMX_MAGIC_LEN &&
        fwrite(&kIpmxVersion,   1, 1,                             out_f) == 1 &&
        fwrite(salt,            1, crypto_pwhash_SALTBYTES,       out_f) == crypto_pwhash_SALTBYTES &&
        copy_file_bytes(tmp_f, out_f) == 0;

    fclose(tmp_f);
    unlink(tmp_path);

    if (!write_ok || fclose(out_f) != 0) {
        ipman_log_error("failed to write bundle", "path=%s", out_path);
        unlink(out_path);
        return -1;
    }

    if (chmod(out_path, S_IRUSR | S_IWUSR) != 0) {
        ipman_log_error("cannot chmod bundle to 0600",
                       "path=%s errno=%d", out_path, errno);
        unlink(out_path);
        return -1;
    }

    ipman_log_info("exported portable bundle", "path=%s", out_path);
    return 0;
}
```

- [ ] **Step 4: Build to confirm it compiles**

```bash
make 2>&1 | tail -5
```

Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/migrate_ops.h src/migrate_ops.c
git commit -m "feat: add ipman_export_portable (passphrase-encrypted bundle)"
```

---

## Task 4: `ipman_import_portable()` in migrate_ops.c

**Files:**
- Modify: `src/migrate_ops.c` (add implementation)
- Modify: `src/migrate_ops.h` (already has declaration from Task 3)

Import needs `ipman_home.h`, `ipman_key.h`, `migrations.h`. Check existing includes in `migrate_ops.c`; add any missing ones.

- [ ] **Step 1: Verify includes in `src/migrate_ops.c`**

Confirm `migrate_ops.c` includes or add:
```c
#include "ipman_home.h"
#include "migrations.h"
```

- [ ] **Step 2: Implement `ipman_import_portable()` in `src/migrate_ops.c`**

Add after `ipman_export_portable()`:

```c
int ipman_import_portable(const char *home_path, const char *bundle_path) {
    /* Refuse if target workspace already has a DB. */
    char db_path[PATH_MAX];
    if (join_path(db_path, sizeof db_path, home_path, "ipman.db") != 0) {
        ipman_log_error("path too long", "home=%s", home_path);
        return -1;
    }
    if (access(db_path, F_OK) == 0) {
        ipman_log_error("target workspace already has a database; "
                       "choose a different IPMAN_HOME or remove ipman.db first",
                       "path=%s", db_path);
        return -1;
    }

    /* Read and validate bundle header. */
    FILE *f = fopen(bundle_path, "rb");
    if (f == NULL) {
        ipman_log_error("cannot open bundle", "path=%s errno=%d",
                       bundle_path, errno);
        return -1;
    }

    unsigned char magic[IPMX_MAGIC_LEN];
    unsigned char version;
    unsigned char salt[crypto_pwhash_SALTBYTES];

    if (fread(magic,   1, IPMX_MAGIC_LEN,              f) != IPMX_MAGIC_LEN ||
        fread(&version,1, 1,                            f) != 1 ||
        fread(salt,    1, crypto_pwhash_SALTBYTES,      f) != (size_t)crypto_pwhash_SALTBYTES) {
        fclose(f);
        ipman_log_error("bundle header too short or unreadable",
                       "path=%s", bundle_path);
        return -1;
    }

    if (memcmp(magic, kIpmxMagic, IPMX_MAGIC_LEN) != 0) {
        fclose(f);
        ipman_log_error("not a valid ipman portable bundle (bad magic)",
                       "path=%s", bundle_path);
        return -1;
    }
    if (version != kIpmxVersion) {
        fclose(f);
        ipman_log_error("unsupported bundle version",
                       "path=%s version=%d", bundle_path, (int)version);
        return -1;
    }

    /* Extract DB bytes to a temp file. */
    char tmp_path[PATH_MAX];
    int tn = snprintf(tmp_path, sizeof tmp_path, "%s.import.tmp.%ld",
                      db_path, (long)getpid());
    if (tn < 0 || (size_t)tn >= sizeof tmp_path) {
        fclose(f);
        ipman_log_error("tmp path too long", "db=%s", db_path);
        return -1;
    }

    FILE *tmp_f = fopen(tmp_path, "wb");
    if (tmp_f == NULL) {
        fclose(f);
        ipman_log_error("cannot create import tmp",
                       "path=%s errno=%d", tmp_path, errno);
        return -1;
    }

    if (copy_file_bytes(f, tmp_f) != 0) {
        fclose(f);
        fclose(tmp_f);
        unlink(tmp_path);
        ipman_log_error("failed to extract bundle DB bytes",
                       "bundle=%s", bundle_path);
        return -1;
    }
    fclose(f);
    fclose(tmp_f);

    /* Derive transport key from passphrase. */
    char pass[1024];
    size_t pass_len = 0;
    if (ipman_read_passphrase("Import passphrase: ", pass, sizeof pass,
                               &pass_len) != 0) {
        unlink(tmp_path);
        return -1;
    }

    unsigned char tkey[IPMAN_KEY_BYTES];
    int krc = ipman_key_derive_passphrase(
        (unsigned char *)pass, pass_len, salt, tkey);
    sodium_memzero(pass, sizeof pass);
    if (krc != 0) {
        unlink(tmp_path);
        return -1;
    }
    char tkey_hex[65];
    hex_encode_32(tkey, tkey_hex);
    sodium_memzero(tkey, sizeof tkey);

    /* Verify transport key by opening the extracted DB. */
    sqlite3 *src_db = NULL;
    int rc = sqlite3_open_v2(tmp_path, &src_db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("cannot open extracted bundle DB",
                       "path=%s rc=%d", tmp_path, rc);
        sodium_memzero(tkey_hex, sizeof tkey_hex);
        unlink(tmp_path);
        return -1;
    }

    char *key_sql = sqlite3_mprintf("PRAGMA key = \"x'%s'\"", tkey_hex);
    if (key_sql == NULL ||
        run_sql(src_db, key_sql, "PRAGMA key (verify transport)") != SQLITE_OK) {
        if (key_sql != NULL) { sodium_memzero(key_sql, strlen(key_sql)); sqlite3_free(key_sql); }
        sodium_memzero(tkey_hex, sizeof tkey_hex);
        sqlite3_close(src_db);
        unlink(tmp_path);
        return -1;
    }
    sodium_memzero(key_sql, strlen(key_sql));
    sqlite3_free(key_sql);

    char *err = NULL;
    rc = sqlite3_exec(src_db, "SELECT count(*) FROM sqlite_master",
                      NULL, NULL, &err);
    sqlite3_close(src_db);
    if (rc != SQLITE_OK) {
        ipman_log_error("transport key verification failed (wrong passphrase?)",
                       "rc=%d detail=%s", rc, err ? err : "(null)");
        sqlite3_free(err);
        sodium_memzero(tkey_hex, sizeof tkey_hex);
        unlink(tmp_path);
        return -1;
    }

    /* Initialize target workspace (creates directory + keysalt). */
    if (ipman_home_ensure(home_path) != 0 ||
        ipman_keysalt_ensure(home_path) != 0) {
        sodium_memzero(tkey_hex, sizeof tkey_hex);
        unlink(tmp_path);
        return -1;
    }

    /* Derive machine key for target. */
    unsigned char mkey[IPMAN_KEY_BYTES];
    if (ipman_key_derive(home_path, mkey) != 0) {
        sodium_memzero(tkey_hex, sizeof tkey_hex);
        unlink(tmp_path);
        return -1;
    }
    char mkey_hex[65];
    hex_encode_32(mkey, mkey_hex);
    sodium_memzero(mkey, sizeof mkey);

    /* Transfer: open new machine-keyed target, attach tmp with transport key. */
    sqlite3 *dst_db = NULL;
    rc = sqlite3_open_v2(db_path, &dst_db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("cannot create target DB",
                       "path=%s rc=%d", db_path, rc);
        sodium_memzero(tkey_hex, sizeof tkey_hex);
        sodium_memzero(mkey_hex, sizeof mkey_hex);
        unlink(tmp_path);
        return -1;
    }

    char *dst_key_sql = sqlite3_mprintf("PRAGMA key = \"x'%s'\"", mkey_hex);
    sodium_memzero(mkey_hex, sizeof mkey_hex);
    if (dst_key_sql == NULL ||
        run_sql(dst_db, dst_key_sql, "PRAGMA key (machine)") != SQLITE_OK) {
        if (dst_key_sql != NULL) { sodium_memzero(dst_key_sql, strlen(dst_key_sql)); sqlite3_free(dst_key_sql); }
        sodium_memzero(tkey_hex, sizeof tkey_hex);
        sqlite3_close(dst_db);
        unlink(tmp_path);
        unlink(db_path);
        return -1;
    }
    sodium_memzero(dst_key_sql, strlen(dst_key_sql));
    sqlite3_free(dst_key_sql);

    char *src_attach_sql = sqlite3_mprintf(
        "ATTACH DATABASE %Q AS source KEY \"x'%s'\"", tmp_path, tkey_hex);
    sodium_memzero(tkey_hex, sizeof tkey_hex);
    if (src_attach_sql == NULL ||
        run_sql(dst_db, src_attach_sql, "ATTACH source (transport)") != SQLITE_OK) {
        if (src_attach_sql != NULL) { sodium_memzero(src_attach_sql, strlen(src_attach_sql)); sqlite3_free(src_attach_sql); }
        sqlite3_close(dst_db);
        unlink(tmp_path);
        unlink(db_path);
        return -1;
    }
    sodium_memzero(src_attach_sql, strlen(src_attach_sql));
    sqlite3_free(src_attach_sql);

    rc = run_sql(dst_db, "SELECT sqlcipher_export('main', 'source')",
                 "sqlcipher_export import");
    sqlite3_exec(dst_db, "DETACH DATABASE source", NULL, NULL, NULL);
    sqlite3_close(dst_db);
    unlink(tmp_path);

    if (rc != SQLITE_OK) {
        unlink(db_path);
        return -1;
    }

    /* Apply migrations — the source bundle may be from an older schema version.
     * Pass NULL for db_path so no backup is made (the entire DB was just created). */
    sqlite3 *mig_db = NULL;
    rc = sqlite3_open_v2(db_path, &mig_db, SQLITE_OPEN_READWRITE, NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("cannot reopen imported DB for migrations",
                       "path=%s rc=%d", db_path, rc);
        unlink(db_path);
        return -1;
    }

    /* Re-apply machine key on migration connection. */
    unsigned char mkey2[IPMAN_KEY_BYTES];
    if (ipman_key_derive(home_path, mkey2) != 0) {
        sqlite3_close(mig_db);
        unlink(db_path);
        return -1;
    }
    char mkey2_hex[65];
    hex_encode_32(mkey2, mkey2_hex);
    sodium_memzero(mkey2, sizeof mkey2);

    char *mig_key_sql = sqlite3_mprintf("PRAGMA key = \"x'%s'\"", mkey2_hex);
    sodium_memzero(mkey2_hex, sizeof mkey2_hex);
    if (mig_key_sql == NULL ||
        run_sql(mig_db, mig_key_sql, "PRAGMA key (migration)") != SQLITE_OK) {
        if (mig_key_sql != NULL) { sodium_memzero(mig_key_sql, strlen(mig_key_sql)); sqlite3_free(mig_key_sql); }
        sqlite3_close(mig_db);
        unlink(db_path);
        return -1;
    }
    sodium_memzero(mig_key_sql, strlen(mig_key_sql));
    sqlite3_free(mig_key_sql);

    int schema_version = 0;
    if (ipman_migrations_apply(mig_db, NULL, &schema_version) != 0) {
        sqlite3_close(mig_db);
        unlink(db_path);
        return -1;
    }
    sqlite3_close(mig_db);

    ipman_log_info("imported portable bundle",
                  "bundle=%s db=%s schema_version=%d",
                  bundle_path, db_path, schema_version);
    return 0;
}
```

- [ ] **Step 3: Add missing includes to `src/migrate_ops.c`**

Confirm or add at the top of `migrate_ops.c`:
```c
#include "ipman_home.h"
#include "migrations.h"
```

- [ ] **Step 4: Build to confirm it compiles**

```bash
make 2>&1 | tail -5
```

Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/migrate_ops.c src/migrate_ops.h
git commit -m "feat: add ipman_import_portable (bundle → machine-keyed workspace)"
```

---

## Task 5: CLI wiring in main.c

**Files:**
- Modify: `src/main.c`

Add `run_export_portable()`, `run_import_portable()`, and register both in `parse_command()`.

- [ ] **Step 1: Add `#include "migrate_ops.h"` if not present**

Check that `src/main.c` already includes `migrate_ops.h`. If not, add it near the other includes.

- [ ] **Step 2: Add `run_export_portable()` to `src/main.c`**

Add after `run_export()` (around line 454):

```c
/* Parse `ipman export-portable --output <file>` argv. */
static int run_export_portable(int argc, char **argv) {
    const char *out_path = NULL;
    for (int i = 2; i < argc; ++i) {
        if (strncmp(argv[i], "--output=", 9) == 0) {
            out_path = argv[i] + 9;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "ipman export-portable: unknown flag: %s\n", argv[i]);
            return 1;
        } else if (out_path == NULL) {
            out_path = argv[i];
        } else {
            fprintf(stderr, "ipman export-portable: unexpected argument: %s\n",
                    argv[i]);
            return 1;
        }
    }
    if (out_path == NULL) {
        fprintf(stderr,
                "usage: ipman export-portable --output <file.ipman>\n");
        return 1;
    }

    char home[PATH_MAX];
    if (ipman_home_resolve(home, sizeof home) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL, "cannot resolve ipman home");
    }
    if (ipman_home_require(home) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL,
                          "ipman workspace not initialized; run `ipman init` first");
    }
    if (ipman_export_portable(home, out_path) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL, "export-portable failed");
    }
    fprintf(stdout, "exported to %s\n", out_path);
    return 0;
}
```

- [ ] **Step 3: Add `run_import_portable()` to `src/main.c`**

Add after `run_export_portable()`:

```c
/* Parse `ipman import-portable <file.ipman>` argv. */
static int run_import_portable(int argc, char **argv) {
    const char *bundle_path = NULL;
    for (int i = 2; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "ipman import-portable: unknown flag: %s\n", argv[i]);
            return 1;
        } else if (bundle_path == NULL) {
            bundle_path = argv[i];
        } else {
            fprintf(stderr, "ipman import-portable: unexpected argument: %s\n",
                    argv[i]);
            return 1;
        }
    }
    if (bundle_path == NULL) {
        fprintf(stderr, "usage: ipman import-portable <file.ipman>\n");
        return 1;
    }

    char home[PATH_MAX];
    if (ipman_home_resolve(home, sizeof home) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL, "cannot resolve ipman home");
    }
    if (ipman_import_portable(home, bundle_path) != 0) {
        return emit_fatal(NULL, IPMAN_ERR_INTERNAL, "import-portable failed");
    }
    fprintf(stdout, "imported from %s\n", bundle_path);
    return 0;
}
```

- [ ] **Step 4: Register commands in `parse_command()` in `src/main.c`**

In `parse_command()` (around line 538), after the `import-plan` entry, add:

```c
if (strcmp(arg, "export-portable") == 0 || strcmp(arg, "--export-portable") == 0) return "export-portable";
if (strcmp(arg, "import-portable") == 0 || strcmp(arg, "--import-portable") == 0) return "import-portable";
```

- [ ] **Step 5: Dispatch in `main()` in `src/main.c`**

Find the block in `main()` that dispatches commands (look for `if (strcmp(cmd, "init") == 0)`). Add:

```c
if (strcmp(cmd, "export-portable") == 0) return run_export_portable(argc, argv);
if (strcmp(cmd, "import-portable") == 0) return run_import_portable(argc, argv);
```

Add these entries alongside the other command dispatches (e.g., next to the `export` entry).

- [ ] **Step 6: Build to confirm it compiles**

```bash
make 2>&1 | tail -5
```

Expected: build succeeds.

- [ ] **Step 7: Commit**

```bash
git add src/main.c
git commit -m "feat: wire export-portable / import-portable CLI subcommands"
```

---

## Task 6: Integration test

**Files:**
- Create: `tests/integration/260_export_portable.sh`

The test uses `IPMAN_PASSPHRASE` to bypass interactive passphrase input. It creates a source workspace, exports it as a portable bundle, imports into a fresh workspace, and verifies data integrity.

- [ ] **Step 1: Create `tests/integration/260_export_portable.sh`**

```sh
#!/bin/sh
set -eu

# Integration test: portable export/import round-trip.
#
# Creates a source workspace with 1 plan, 1 phase, 2 tasks.
# Exports as a portable bundle (IPMAN_PASSPHRASE bypasses interactive prompt).
# Imports into a fresh workspace on the same machine.
# Verifies: plan exists with same title, phase count=1, task count=2.

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-export-portable.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

SRC_HOME="$TMP/src-home"
DST_HOME="$TMP/dst-home"
BUNDLE="$TMP/workspace.ipman"
PASSPHRASE="test-passphrase-round-trip"

# Helper: call ipman op in SRC workspace, return JSON
src_call() {
    printf '%s\n' "$1" | IPMAN_HOME="$SRC_HOME" "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

# ---- Bootstrap source workspace -------------------------------------------
IPMAN_HOME="$SRC_HOME" "$BIN" init >/dev/null 2>/dev/null

plan=$(src_call '{"protocol_version":2,"request_id":"p1","actor":"t","op":"plan.create","params":{"title":"Portable Test Plan","priority":"medium"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

ph=$(src_call "{\"protocol_version\":2,\"request_id\":\"ph1\",\"actor\":\"t\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Phase One\",\"sequence_no\":1}}")
expect_ok "$ph"
ph_id=$(printf '%s' "$ph" | jq -r '.result.phase.id')

t1=$(src_call "{\"protocol_version\":2,\"request_id\":\"t1\",\"actor\":\"t\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$ph_id,\"title\":\"Task Alpha\"}}")
expect_ok "$t1"

t2=$(src_call "{\"protocol_version\":2,\"request_id\":\"t2\",\"actor\":\"t\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$ph_id,\"title\":\"Task Beta\"}}")
expect_ok "$t2"

# ---- Export portable bundle ------------------------------------------------
IPMAN_PASSPHRASE="$PASSPHRASE" IPMAN_HOME="$SRC_HOME" "$BIN" export-portable --output "$BUNDLE"

# Verify bundle file exists and has the IPMX magic header
if [ ! -f "$BUNDLE" ]; then
    echo "FAIL: bundle file not created" >&2
    exit 1
fi

magic=$(head -c 4 "$BUNDLE" | od -An -tx1 | tr -d ' \n')
if [ "$magic" != "49504d58" ]; then
    echo "FAIL: bundle has wrong magic: $magic (expected 49504d58)" >&2
    exit 1
fi

# Verify bundle permissions are 0600
perms=$(stat -c '%a' "$BUNDLE")
if [ "$perms" != "600" ]; then
    echo "FAIL: bundle permissions are $perms, expected 600" >&2
    exit 1
fi

# ---- Import into fresh workspace -------------------------------------------
IPMAN_PASSPHRASE="$PASSPHRASE" IPMAN_HOME="$DST_HOME" "$BIN" import-portable "$BUNDLE"

# Helper: call ipman op in DST workspace
dst_call() {
    printf '%s\n' "$1" | IPMAN_HOME="$DST_HOME" "$BIN" 2>/dev/null
}

# Verify plan exists with same title
plans=$(dst_call '{"protocol_version":2,"request_id":"lp","actor":"t","op":"plan.list","params":{}}')
expect_ok "$plans"

plan_title=$(printf '%s' "$plans" | jq -r '.result.plans[0].title')
if [ "$plan_title" != "Portable Test Plan" ]; then
    echo "FAIL: imported plan title '$plan_title' != 'Portable Test Plan'" >&2
    exit 1
fi

dst_plan_id=$(printf '%s' "$plans" | jq -r '.result.plans[0].id')

# Verify phase count = 1
phases=$(dst_call "{\"protocol_version\":2,\"request_id\":\"lph\",\"actor\":\"t\",\"op\":\"phase.list\",\"params\":{\"plan_id\":$dst_plan_id}}")
expect_ok "$phases"
phase_count=$(printf '%s' "$phases" | jq '.result.phases | length')
if [ "$phase_count" -ne 1 ]; then
    echo "FAIL: expected 1 phase, got $phase_count" >&2
    exit 1
fi

# Verify task count = 2
tasks=$(dst_call "{\"protocol_version\":2,\"request_id\":\"lt\",\"actor\":\"t\",\"op\":\"task.list\",\"params\":{\"plan_id\":$dst_plan_id}}")
expect_ok "$tasks"
task_count=$(printf '%s' "$tasks" | jq '.result.tasks | length')
if [ "$task_count" -ne 2 ]; then
    echo "FAIL: expected 2 tasks, got $task_count" >&2
    exit 1
fi

# ---- Wrong passphrase is rejected ------------------------------------------
wrong_import_out=$(IPMAN_PASSPHRASE="wrong-passphrase" IPMAN_HOME="$TMP/dst-wrong" "$BIN" import-portable "$BUNDLE" 2>&1 || true)
if [ -d "$TMP/dst-wrong/.ipman" ] && [ -f "$TMP/dst-wrong/.ipman/ipman.db" ]; then
    echo "FAIL: import with wrong passphrase should not create database" >&2
    exit 1
fi

echo "PASS: export-portable / import-portable round-trip"
```

- [ ] **Step 2: Make test executable and run it**

```bash
chmod +x tests/integration/260_export_portable.sh
make test 2>&1 | tail -30
```

Expected: all tests pass, including:
```
--- tests/integration/260_export_portable.sh ---
PASS: export-portable / import-portable round-trip
```

- [ ] **Step 3: Commit**

```bash
git add tests/integration/260_export_portable.sh
git commit -m "test(integration): portable export/import round-trip"
```

---

## Self-Review

**Spec coverage:**
- Export with passphrase → `ipman_export_portable()` + `run_export_portable()` ✓
- Import into fresh workspace → `ipman_import_portable()` + `run_import_portable()` ✓
- Passphrase-derived transport key (Argon2id) → `ipman_key_derive_passphrase()` ✓
- Bundle format (header + SQLCipher DB) → `IPMX_MAGIC`, `kIpmxVersion`, 21-byte header ✓
- Machine key unchanged on existing workspaces → no changes to `ipman_key_derive()` ✓
- Passphrase confirmation on export → double-prompt in `ipman_export_portable()` ✓
- Wrong passphrase rejected on import → transport key verification before workspace init ✓
- Existing DB at target rejected → `access(db_path, F_OK)` guard ✓
- Schema migrations applied after import → `ipman_migrations_apply()` called ✓
- Integration test with IPMAN_PASSPHRASE escape hatch ✓
- Unit test for key derivation (Tier 1 — key management) ✓

**No placeholder scan:** All code blocks are complete. No "TBD" or "similar to Task N" patterns.

**Type consistency:** `ipman_key_derive_passphrase` signature is consistent across declaration (Task 1 Step 4), implementation (Task 1 Step 5), and call sites (Tasks 3+4).
