/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "ipman_key.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sodium.h>

#define IPMAN_KEYSALT_FILENAME "keysalt"
#define IPMAN_KEYSALT_BYTES    32
#define IPMAN_MACHINE_ID_MAX   64

static const char *const kMachineIdPaths[] = {
    "/etc/machine-id",
    "/var/lib/dbus/machine-id",
    NULL,
};

/* Read up to cap bytes from path. On success returns 0 and writes the byte
 * count to *out_len. require_exact = nonzero forces an exact-size match
 * (returns -1 if read returned fewer bytes than cap). errno is preserved
 * across close() so the caller can log it. */
static int read_small_file(const char *path, void *out_buf, size_t cap,
                           size_t *out_len, int require_exact) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    ssize_t n = read(fd, out_buf, cap);
    int saved = errno;
    close(fd);
    if (n < 0) { errno = saved; return -1; }

    *out_len = (size_t)n;
    if (require_exact && (size_t)n != cap) return -1;
    return 0;
}

static int read_machine_id(unsigned char *out, size_t cap, size_t *out_len) {
    int last_errno = ENOENT;
    for (size_t i = 0; kMachineIdPaths[i] != NULL; ++i) {
        size_t n = 0;
        if (read_small_file(kMachineIdPaths[i], out, cap, &n, /*exact=*/0) != 0) {
            last_errno = errno;
            continue;
        }
        /* machine-id files end with a trailing newline; strip it and any
         * incidental whitespace so the password input is a clean hex string. */
        while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r'
                         || out[n-1] == ' ' || out[n-1] == '\t')) {
            n--;
        }
        if (n == 0) continue;
        *out_len = n;
        return 0;
    }
    ipman_log_error("cannot read machine-id",
                   "tried=/etc/machine-id,/var/lib/dbus/machine-id errno=%d",
                   last_errno);
    return -1;
}

/* Build "<home>/keysalt" into `out` (size `cap`). Returns 0 on success, -1
 * on overflow. Shared by the read and ensure paths so the filename is set
 * exactly once in this translation unit. */
static int keysalt_path(const char *home_path, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s/%s", home_path, IPMAN_KEYSALT_FILENAME);
    if (n < 0 || (size_t)n >= cap) {
        ipman_log_error("keysalt path too long", "home=%s", home_path);
        return -1;
    }
    return 0;
}

/* Validate that a keysalt-shaped file at `path` (already known to exist) has
 * the expected attributes. Returns 0 if OK, -1 with diagnostic otherwise. */
static int validate_keysalt_stat(const char *path, const struct stat *st) {
    if (!S_ISREG(st->st_mode)) {
        ipman_log_error("keysalt is not a regular file", "path=%s", path);
        return -1;
    }
    if ((st->st_mode & 0177) != 0) {
        ipman_log_error("keysalt has loose permissions",
                       "path=%s mode=%04o",
                       path, (unsigned)(st->st_mode & 0777));
        return -1;
    }
    if (st->st_uid != geteuid()) {
        ipman_log_error("keysalt not owned by current user", "path=%s", path);
        return -1;
    }
    if (st->st_size != (off_t)IPMAN_KEYSALT_BYTES) {
        ipman_log_error("keysalt has wrong size",
                       "path=%s expected=%d actual=%lld",
                       path, IPMAN_KEYSALT_BYTES, (long long)st->st_size);
        return -1;
    }
    return 0;
}

static int read_keysalt(const char *home_path, unsigned char *out) {
    char path[PATH_MAX];
    if (keysalt_path(home_path, path, sizeof path) != 0) return -1;

    struct stat st;
    if (stat(path, &st) != 0) {
        ipman_log_error("keysalt missing — run `ipman init` first",
                       "path=%s errno=%d", path, errno);
        return -1;
    }
    if (validate_keysalt_stat(path, &st) != 0) return -1;

    size_t got = 0;
    if (read_small_file(path, out, IPMAN_KEYSALT_BYTES, &got, /*exact=*/1) != 0) {
        ipman_log_error("cannot read keysalt",
                       "path=%s errno=%d got=%zu", path, errno, got);
        return -1;
    }
    return 0;
}

int ipman_key_derive(const char *home_path, unsigned char *out) {
    if (sodium_init() < 0) {
        ipman_log_error("sodium_init failed", "rc=-1");
        return -1;
    }

    unsigned char machine_id[IPMAN_MACHINE_ID_MAX];
    size_t machine_id_len = 0;
    if (read_machine_id(machine_id, sizeof machine_id, &machine_id_len) != 0) {
        return -1;
    }

    unsigned char keysalt[IPMAN_KEYSALT_BYTES];
    if (read_keysalt(home_path, keysalt) != 0) {
        sodium_memzero(machine_id, sizeof machine_id);
        return -1;
    }

    /* Argon2id wiring:
     *   password = machine-id || uid_bytes          (per-machine + per-user)
     *   salt     = first crypto_pwhash_SALTBYTES of keysalt (per-instance)
     *
     * The plan summary phrased it as "machine-id || uid_bytes || salt as the
     * password input" — same security properties either way (keysalt is the
     * sole source of per-instance entropy), but routing keysalt through the
     * salt parameter is the conventional Argon2id usage and keeps the
     * derivation easy to audit by anyone familiar with crypto_pwhash. */
    uid_t uid = getuid();
    unsigned char password[IPMAN_MACHINE_ID_MAX + sizeof(uid_t)];
    memcpy(password, machine_id, machine_id_len);
    memcpy(password + machine_id_len, &uid, sizeof uid);
    size_t password_len = machine_id_len + sizeof uid;

    int rc = crypto_pwhash(out, IPMAN_KEY_BYTES,
                           (const char *)password, password_len,
                           keysalt,
                           crypto_pwhash_OPSLIMIT_INTERACTIVE,
                           crypto_pwhash_MEMLIMIT_INTERACTIVE,
                           crypto_pwhash_ALG_ARGON2ID13);

    sodium_memzero(machine_id, sizeof machine_id);
    sodium_memzero(password, sizeof password);
    sodium_memzero(keysalt, sizeof keysalt);

    if (rc != 0) {
        ipman_log_error("crypto_pwhash failed (likely out of memory)",
                       "rc=%d", rc);
        return -1;
    }
    return 0;
}

/* Acquire an exclusive POSIX write-lock on <home>/.init.lock and return
 * the open fd. Closing the fd releases the lock. Used to serialize the
 * critical section in ipman_keysalt_ensure so two concurrent `ipman init`
 * calls cannot both materialize different salts (the second rename would
 * otherwise clobber the first, breaking decryption for the first caller).
 * The lock file itself is never unlinked — it is the persistent rendezvous
 * point that subsequent inits use to synchronize. */
static int acquire_init_lock(const char *home_path) {
    char lock_path[PATH_MAX];
    int n = snprintf(lock_path, sizeof lock_path, "%s/.init.lock", home_path);
    if (n < 0 || (size_t)n >= sizeof lock_path) {
        ipman_log_error("init lock path too long", "home=%s", home_path);
        return -1;
    }
    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        ipman_log_error("cannot open init lock",
                       "path=%s errno=%d", lock_path, errno);
        return -1;
    }
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;  /* whole file */

    int rc;
    do {
        rc = fcntl(fd, F_SETLKW, &fl);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        int saved = errno;
        close(fd);
        ipman_log_error("cannot acquire init lock",
                       "path=%s errno=%d", lock_path, saved);
        return -1;
    }
    return fd;
}

int ipman_keysalt_ensure(const char *home_path) {
    char path[PATH_MAX];
    if (keysalt_path(home_path, path, sizeof path) != 0) return -1;

    struct stat st;
    if (stat(path, &st) == 0) {
        /* Fast path: keysalt already present. Validate but never overwrite —
         * overwriting would silently lock out any encrypted DB derived from
         * the old salt. */
        return validate_keysalt_stat(path, &st);
    }
    if (errno != ENOENT) {
        ipman_log_error("cannot stat keysalt",
                       "path=%s errno=%d", path, errno);
        return -1;
    }

    /* Slow path: serialize creation. The stat-then-create dance is racy
     * across concurrent ipman invocations; without a lock, two `ipman init`
     * runs can each generate a salt and the loser's rename clobbers the
     * winner's keysalt. */
    int lock_fd = acquire_init_lock(home_path);
    if (lock_fd < 0) return -1;

    /* Re-check under the lock: another process may have created the keysalt
     * between our first stat and our lock acquisition. */
    if (stat(path, &st) == 0) {
        int rc = validate_keysalt_stat(path, &st);
        close(lock_fd);
        return rc;
    }
    if (errno != ENOENT) {
        ipman_log_error("cannot stat keysalt under lock",
                       "path=%s errno=%d", path, errno);
        close(lock_fd);
        return -1;
    }

    if (sodium_init() < 0) {
        ipman_log_error("sodium_init failed", "rc=-1");
        close(lock_fd);
        return -1;
    }

    unsigned char salt[IPMAN_KEYSALT_BYTES];
    randombytes_buf(salt, sizeof salt);

    /* Atomic write: tmp → fsync → rename. The tmp path embeds our PID so a
     * stale tmp from a previously crashed run on a recycled PID is the only
     * collision possible (we hold the init lock, so no live concurrent
     * process can be using this name). O_EXCL refuses any pre-existing tmp;
     * on EEXIST we unlink the stale file once and retry. */
    char tmp_path[PATH_MAX];
    int tn = snprintf(tmp_path, sizeof tmp_path, "%s.tmp.%ld",
                      path, (long)getpid());
    if (tn < 0 || (size_t)tn >= sizeof tmp_path) {
        sodium_memzero(salt, sizeof salt);
        close(lock_fd);
        ipman_log_error("keysalt tmp path too long", "path=%s", path);
        return -1;
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0 && errno == EEXIST) {
        if (unlink(tmp_path) == 0) {
            fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        }
    }
    if (fd < 0) {
        sodium_memzero(salt, sizeof salt);
        close(lock_fd);
        ipman_log_error("cannot create keysalt tmp",
                       "path=%s errno=%d", tmp_path, errno);
        return -1;
    }

    ssize_t w = write(fd, salt, sizeof salt);
    int saved = errno;
    sodium_memzero(salt, sizeof salt);
    if (w != (ssize_t)sizeof salt) {
        close(fd);
        unlink(tmp_path);
        close(lock_fd);
        ipman_log_error("cannot write keysalt tmp",
                       "path=%s errno=%d wrote=%zd",
                       tmp_path, saved, w);
        return -1;
    }
    if (fsync(fd) != 0) {
        saved = errno;
        close(fd);
        unlink(tmp_path);
        close(lock_fd);
        ipman_log_error("cannot fsync keysalt tmp",
                       "path=%s errno=%d", tmp_path, saved);
        return -1;
    }
    close(fd);

    if (rename(tmp_path, path) != 0) {
        saved = errno;
        unlink(tmp_path);
        close(lock_fd);
        ipman_log_error("cannot rename keysalt into place",
                       "tmp=%s path=%s errno=%d", tmp_path, path, saved);
        return -1;
    }

    /* Re-stat the destination right after rename. We just wrote the file
     * with mode 0600 and our own UID, but on a shared host an external
     * actor with mkdir-permission on home_path could chmod or chown it
     * between rename and the next derive. Catching it here turns "init
     * succeeds, next op silently fails" into "init fails with a clear
     * tampering diagnostic." Defense-in-depth — read_keysalt validates
     * again on every derive, so this is the early-warning hook. */
    struct stat st_after;
    if (stat(path, &st_after) != 0) {
        saved = errno;
        close(lock_fd);
        ipman_log_error("cannot stat keysalt after rename",
                       "path=%s errno=%d", path, saved);
        return -1;
    }
    if (validate_keysalt_stat(path, &st_after) != 0) {
        close(lock_fd);
        return -1;
    }

    ipman_log_info("created keysalt", "path=%s mode=0600 bytes=%d",
                  path, IPMAN_KEYSALT_BYTES);
    close(lock_fd);
    return 0;
}

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
