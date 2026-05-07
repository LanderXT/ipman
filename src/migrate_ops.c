/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "migrate_ops.h"

#include "ipman_home.h"
#include "ipman_key.h"
#include "log.h"
#include "migrations.h"
#include "passphrase.h"

#include "sqlite3.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sodium.h>

#define SQLITE_MAGIC_LEN 16
static const unsigned char kSqliteMagic[SQLITE_MAGIC_LEN] =
    "SQLite format 3";  /* trailing NUL is the 16th byte */

#define IPMX_MAGIC_LEN   4
#define IPMX_HEADER_LEN  (IPMX_MAGIC_LEN + 1 + crypto_pwhash_SALTBYTES)
static const unsigned char kIpmxMagic[IPMX_MAGIC_LEN] = {0x49, 0x50, 0x4d, 0x58};
static const unsigned char kIpmxVersion = 0x01;

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

static int join_path(char *out, size_t cap, const char *home, const char *name) {
    int n = snprintf(out, cap, "%s/%s", home, name);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

/* fsync the directory containing `path`, making the rename metadata durable
 * across power loss. POSIX rename() returning success only guarantees the
 * change is in cache; a crash before the directory inode is flushed can
 * lose the rename. Failure here is logged at warn — best-effort durability,
 * never fatal. */
static void fsync_dir_of(const char *path) {
    char dirbuf[PATH_MAX];
    if ((size_t)snprintf(dirbuf, sizeof dirbuf, "%s", path) >= sizeof dirbuf) {
        return;
    }
    char *slash = strrchr(dirbuf, '/');
    if (slash == NULL) return;
    *slash = '\0';
    const char *dir = dirbuf[0] != '\0' ? dirbuf : "/";
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        ipman_log_warn("cannot open dir for fsync",
                      "path=%s errno=%d", dir, errno);
        return;
    }
    if (fsync(fd) != 0) {
        ipman_log_warn("fsync dir failed",
                      "path=%s errno=%d", dir, errno);
    }
    close(fd);
}

/* Detect and heal the post-crash window of ipman_migrate_encrypt where
 * rename(db_path -> bak_path) succeeded but rename(enc_path -> db_path)
 * did not run: ipman.db is absent while ipman.db.encrypted holds the
 * fully-written encrypted database. Renaming enc -> db completes the
 * swap, leaving the workspace in the same shape a successful run would.
 *
 * No-op for any other state. Returns 0 if the state is fine OR was healed,
 * -1 if recovery itself errored (operator must intervene). */
static int recover_half_swapped(const char *db_path, const char *enc_path) {
    if (access(db_path, F_OK) == 0) return 0;
    if (errno != ENOENT) {
        ipman_log_error("cannot probe db path during recovery",
                       "path=%s errno=%d", db_path, errno);
        return -1;
    }
    if (access(enc_path, F_OK) != 0) {
        /* db absent and enc absent — workspace is empty/uninitialized,
         * not our problem to fix here. */
        return 0;
    }

    ipman_log_warn("detected half-completed migrate-encrypt; "
                  "renaming encrypted file into place",
                  "from=%s to=%s", enc_path, db_path);
    if (rename(enc_path, db_path) != 0) {
        ipman_log_error("migrate-encrypt recovery rename failed",
                       "from=%s to=%s errno=%d",
                       enc_path, db_path, errno);
        return -1;
    }
    fsync_dir_of(db_path);
    ipman_log_info("migrate-encrypt recovery complete",
                  "db=%s", db_path);
    return 0;
}

/* Returns 1 if the file at `path` is a plaintext SQLite database (starts
 * with the SQLite magic), 0 if it isn't (encrypted, empty, or unrecognized
 * format), -1 on I/O error. */
static int file_is_plaintext_sqlite(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    unsigned char buf[SQLITE_MAGIC_LEN];
    ssize_t n = read(fd, buf, SQLITE_MAGIC_LEN);
    int saved = errno;
    close(fd);
    if (n < 0) { errno = saved; return -1; }
    if (n != SQLITE_MAGIC_LEN) return 0;
    return memcmp(buf, kSqliteMagic, SQLITE_MAGIC_LEN) == 0 ? 1 : 0;
}

static void hex_encode_32(const unsigned char *in, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[in[i] & 0x0f];
    }
    out[64] = '\0';
}

/* Run a single SQL statement; on failure, log with a context label and the
 * SQLCipher error message. Returns SQLite rc. */
static int run_sql(sqlite3 *db, const char *sql, const char *context) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        ipman_log_error("sql failed",
                       "context=%s rc=%d msg=%s",
                       context, rc, errmsg ? errmsg : "(none)");
        sqlite3_free(errmsg);
    }
    return rc;
}

int ipman_migrate_encrypt(const char *home_path) {
    char db_path[PATH_MAX], enc_path[PATH_MAX], bak_path[PATH_MAX];
    if (join_path(db_path,  sizeof db_path,  home_path, "ipman.db") != 0 ||
        join_path(enc_path, sizeof enc_path, home_path, "ipman.db.encrypted") != 0 ||
        join_path(bak_path, sizeof bak_path, home_path, "ipman.db.bak") != 0) {
        ipman_log_error("path too long for migrate-encrypt", "home=%s", home_path);
        return -1;
    }

    /* Heal the half-swapped state from a prior crashed run before any other
     * check. After this returns, db_path either exists or never did; the
     * normal probe takes over from here. */
    if (recover_half_swapped(db_path, enc_path) != 0) return -1;

    int probe = file_is_plaintext_sqlite(db_path);
    if (probe < 0) {
        ipman_log_error("cannot probe DB", "path=%s errno=%d", db_path, errno);
        return -1;
    }
    if (probe == 0) {
        ipman_log_info("DB is already encrypted; nothing to migrate",
                      "path=%s", db_path);
        return 0;
    }

    if (access(enc_path, F_OK) == 0) {
        ipman_log_error("leftover encrypted file from a prior run; "
                       "remove it manually before retrying",
                       "path=%s", enc_path);
        return -1;
    }

    unsigned char key[IPMAN_KEY_BYTES];
    if (ipman_key_derive(home_path, key) != 0) return -1;
    char key_hex[65];
    hex_encode_32(key, key_hex);
    sodium_memzero(key, sizeof key);

    /* Open the encrypted target as main. SQLCipher establishes its
     * encryption context on the main database, then we attach the plaintext
     * with KEY '' as the source and export source -> main. */
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(enc_path, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("cannot open encrypted target",
                       "path=%s rc=%d msg=%s",
                       enc_path, rc,
                       db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db != NULL) sqlite3_close(db);
        sodium_memzero(key_hex, sizeof key_hex);
        return -1;
    }

    /* The "x'<hex>'" form bypasses SQLCipher's internal PBKDF2 — Argon2id
     * already ran in ipman_key_derive. */
    char *key_sql = sqlite3_mprintf("PRAGMA key = \"x'%s'\"", key_hex);
    sodium_memzero(key_hex, sizeof key_hex);
    if (key_sql == NULL) {
        ipman_log_error("sqlite3_mprintf oom", "context=key");
        sqlite3_close(db);
        unlink(enc_path);
        return -1;
    }
    rc = run_sql(db, key_sql, "PRAGMA key");
    sodium_memzero(key_sql, strlen(key_sql));
    sqlite3_free(key_sql);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        unlink(enc_path);
        return -1;
    }

    /* Attach plaintext source with empty KEY (= no encryption). */
    char *attach_sql = sqlite3_mprintf(
        "ATTACH DATABASE %Q AS source KEY ''", db_path);
    if (attach_sql == NULL) {
        ipman_log_error("sqlite3_mprintf oom", "context=attach");
        sqlite3_close(db);
        unlink(enc_path);
        return -1;
    }
    rc = run_sql(db, attach_sql, "ATTACH source");
    sqlite3_free(attach_sql);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        unlink(enc_path);
        return -1;
    }

    /* sqlcipher_export(target [, source]). Default source = 'main'.
     * Here we want source=plaintext (attached as 'source'), target=main. */
    rc = run_sql(db, "SELECT sqlcipher_export('main', 'source')",
                 "sqlcipher_export");
    if (rc != SQLITE_OK) {
        sqlite3_exec(db, "DETACH DATABASE source", NULL, NULL, NULL);
        sqlite3_close(db);
        unlink(enc_path);
        return -1;
    }

    sqlite3_exec(db, "DETACH DATABASE source", NULL, NULL, NULL);
    sqlite3_close(db);

    /* Two-step swap: rename plaintext -> .bak first so we always have at
     * least one valid file on disk between renames. fsync the directory
     * after each rename so a crash here cannot lose the metadata change.
     * If we crash AFTER rename(db→bak) but BEFORE rename(enc→db), the next
     * ipman_migrate_encrypt invocation will detect db missing + enc present
     * and call recover_half_swapped to complete the swap. */
    if (rename(db_path, bak_path) != 0) {
        ipman_log_error("cannot rename plaintext to .bak",
                       "from=%s to=%s errno=%d", db_path, bak_path, errno);
        unlink(enc_path);
        return -1;
    }
    fsync_dir_of(db_path);
    if (rename(enc_path, db_path) != 0) {
        int saved = errno;
        ipman_log_error("cannot rename encrypted into place",
                       "from=%s to=%s errno=%d", enc_path, db_path, saved);
        if (rename(bak_path, db_path) != 0) {
            ipman_log_error("CRITICAL: failed to restore plaintext from .bak; "
                           "next migrate-encrypt run will auto-recover from "
                           "ipman.db.encrypted",
                           "bak=%s expected=%s errno=%d",
                           bak_path, db_path, errno);
        } else {
            fsync_dir_of(db_path);
        }
        return -1;
    }
    fsync_dir_of(db_path);

    ipman_log_info("migrated DB to encrypted format",
                  "encrypted=%s plaintext_backup=%s", db_path, bak_path);
    ipman_log_info("plaintext backup retained -- remove manually after verifying",
                  "backup=%s", bak_path);
    return 0;
}

int ipman_export_plaintext(const char *home_path, const char *out_path) {
    char db_path[PATH_MAX];
    if (join_path(db_path, sizeof db_path, home_path, "ipman.db") != 0) {
        ipman_log_error("path too long for export-plaintext",
                       "home=%s", home_path);
        return -1;
    }

    /* Refuse self-overwrite. realpath() would catch symlinks too, but the
     * source DB cannot be removed mid-export safely anyway -- the literal
     * string match is enough to stop the obvious foot-gun. */
    if (strcmp(db_path, out_path) == 0) {
        ipman_log_error("output path equals the live DB; refusing to overwrite",
                       "path=%s", out_path);
        return -1;
    }

    if (access(out_path, F_OK) == 0) {
        ipman_log_error("output file already exists; remove it first",
                       "path=%s", out_path);
        return -1;
    }

    /* Mirror the forward migrate_encrypt direction: open the file we need to
     * CREATE as main (so SQLITE_OPEN_CREATE applies on the right side), and
     * ATTACH the existing encrypted source with its key. SQLCipher will not
     * auto-create the attached target on a keyed connection, so flipping the
     * roles is what makes both directions work the same way. */
    unsigned char key[IPMAN_KEY_BYTES];
    if (ipman_key_derive(home_path, key) != 0) return -1;
    char key_hex[65];
    hex_encode_32(key, key_hex);
    sodium_memzero(key, sizeof key);

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(out_path, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("cannot open plaintext target",
                       "path=%s rc=%d msg=%s", out_path, rc,
                       db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db != NULL) sqlite3_close(db);
        sodium_memzero(key_hex, sizeof key_hex);
        return -1;
    }

    char *attach_sql = sqlite3_mprintf(
        "ATTACH DATABASE %Q AS source KEY \"x'%s'\"", db_path, key_hex);
    sodium_memzero(key_hex, sizeof key_hex);
    if (attach_sql == NULL) {
        ipman_log_error("sqlite3_mprintf oom", "context=ATTACH source");
        sqlite3_close(db);
        unlink(out_path);
        return -1;
    }
    rc = run_sql(db, attach_sql, "ATTACH source");
    sodium_memzero(attach_sql, strlen(attach_sql));
    sqlite3_free(attach_sql);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        unlink(out_path);
        return -1;
    }

    /* Force a read against the attached source so a corrupted keysalt
     * surfaces as SQLITE_NOTADB here rather than mid-export. */
    rc = run_sql(db, "SELECT count(*) FROM source.sqlite_master",
                 "verify source key");
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        unlink(out_path);
        return -1;
    }

    rc = run_sql(db, "SELECT sqlcipher_export('main', 'source')",
                 "sqlcipher_export plaintext");
    sqlite3_close(db);
    if (rc != SQLITE_OK) {
        unlink(out_path);
        return -1;
    }

    /* Tighten perms post-close: this file holds every byte the encrypted DB
     * was protecting, so default umask (often 0644) is too generous. */
    if (chmod(out_path, S_IRUSR | S_IWUSR) != 0) {
        ipman_log_error("cannot chmod plaintext output to 0600",
                       "path=%s errno=%d", out_path, errno);
        unlink(out_path);
        return -1;
    }

    ipman_log_info("exported plaintext SQLite copy",
                  "source=%s plaintext=%s", db_path, out_path);
    ipman_log_info("plaintext export bypasses encryption -- store securely "
                  "and remove when no longer needed",
                  "path=%s", out_path);
    return 0;
}

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

    /* Extract DB bytes to a temp file in the system temp dir.
     * home_path directory may not exist yet, so we cannot write there. */
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL) tmpdir = "/tmp";
    char tmp_path[PATH_MAX];
    int tn = snprintf(tmp_path, sizeof tmp_path, "%s/ipman-import.%ld.tmp",
                      tmpdir, (long)getpid());
    if (tn < 0 || (size_t)tn >= sizeof tmp_path) {
        fclose(f);
        ipman_log_error("tmp path too long", "tmpdir=%s", tmpdir);
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
