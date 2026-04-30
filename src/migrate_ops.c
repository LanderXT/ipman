#include "migrate_ops.h"

#include "log.h"
#include "ipman_key.h"

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
