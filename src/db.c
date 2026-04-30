#include "db.h"

#include "log.h"
#include "ipman_key.h"

#include <sodium.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int apply_pragma(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        ipman_log_error("pragma failed", "sql=\"%s\" rc=%d detail=\"%s\"",
                       sql, rc, err ? err : "(null)");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static void hex_encode_32(const unsigned char *in, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[in[i] & 0x0f];
    }
    out[64] = '\0';
}

/* Derive the SQLCipher page key from `home_path` and apply it to `db`. The
 * "x'<hex>'" form feeds the raw 32-byte key directly -- SQLCipher would
 * otherwise run PBKDF2 over the literal, but we already did Argon2id in
 * ipman_key_derive. After applying the key we run a trivial SELECT against
 * sqlite_master to force page-1 decryption: a stale or corrupted keysalt
 * surfaces here as SQLITE_NOTADB instead of bleeding into the first real
 * query as a confusing "no such table" error. */
static int apply_key(sqlite3 *db, const char *home_path) {
    unsigned char key[IPMAN_KEY_BYTES];
    if (ipman_key_derive(home_path, key) != 0) return -1;

    char key_hex[65];
    hex_encode_32(key, key_hex);
    sodium_memzero(key, sizeof key);

    char *key_sql = sqlite3_mprintf("PRAGMA key = \"x'%s'\"", key_hex);
    sodium_memzero(key_hex, sizeof key_hex);
    if (key_sql == NULL) {
        ipman_log_error("sqlite3_mprintf oom", "context=PRAGMA key");
        return -1;
    }

    char *err = NULL;
    int rc = sqlite3_exec(db, key_sql, NULL, NULL, &err);
    sodium_memzero(key_sql, strlen(key_sql));
    sqlite3_free(key_sql);
    if (rc != SQLITE_OK) {
        ipman_log_error("PRAGMA key failed",
                       "rc=%d detail=\"%s\"", rc, err ? err : "(null)");
        sqlite3_free(err);
        return -1;
    }

    rc = sqlite3_exec(db, "SELECT count(*) FROM sqlite_master",
                      NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        ipman_log_error("key verification failed (wrong key or not a SQLCipher DB)",
                       "rc=%d detail=\"%s\"", rc, err ? err : "(null)");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int ipman_db_open_flags(const char *home_path, const char *path,
                              int flags, sqlite3 **db) {
    sqlite3 *handle = NULL;
    int rc = sqlite3_open_v2(path, &handle, flags, NULL);
    if (rc != SQLITE_OK) {
        ipman_log_error("sqlite3_open_v2 failed",
                       "path=%s rc=%d detail=\"%s\"",
                       path, rc,
                       handle ? sqlite3_errmsg(handle) : sqlite3_errstr(rc));
        if (handle) sqlite3_close(handle);
        return -1;
    }

    /* busy_timeout is a connection-level config that does no I/O, so we
     * set it before the verify SELECT inside apply_key. Without this,
     * concurrent writers race on page 1 during decryption and surface as
     * SQLITE_BUSY misclassified as "wrong key". */
    if (apply_pragma(handle, "PRAGMA busy_timeout=5000;") != 0) {
        sqlite3_close(handle);
        return -1;
    }

    /* PRAGMA key must run before any DB I/O. journal_mode=WAL below would
     * otherwise touch the rollback journal pre-decryption and fail. */
    if (apply_key(handle, home_path) != 0) {
        sqlite3_close(handle);
        return -1;
    }

    if (apply_pragma(handle, "PRAGMA journal_mode=WAL;") != 0 ||
        apply_pragma(handle, "PRAGMA foreign_keys=ON;")  != 0) {
        sqlite3_close(handle);
        return -1;
    }

    *db = handle;
    return 0;
}

int ipman_db_open(const char *home_path, const char *path, sqlite3 **db) {
    return ipman_db_open_flags(home_path, path,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, db);
}

int ipman_db_open_existing(const char *home_path, const char *path,
                          sqlite3 **db) {
    return ipman_db_open_flags(home_path, path, SQLITE_OPEN_READWRITE, db);
}

/* Walk the prepared-statement list on `db` and finalize every entry,
 * logging the truncated SQL text of each one as a leak diagnostic. The
 * connection itself is not freed here — the caller decides whether to
 * retry sqlite3_close. Returns the number of statements finalized. */
static int finalize_leaked_statements(sqlite3 *db) {
    int leaked = 0;
    sqlite3_stmt *stmt;
    while ((stmt = sqlite3_next_stmt(db, NULL)) != NULL) {
        const char *sql = sqlite3_sql(stmt);
        ipman_log_warn("finalizing leaked sqlite3_stmt",
                      "sql=\"%.120s\"", sql ? sql : "(unknown)");
        sqlite3_finalize(stmt);
        ++leaked;
    }
    return leaked;
}

void ipman_db_close(sqlite3 *db) {
    if (db == NULL) return;
    int rc = sqlite3_close(db);
    if (rc == SQLITE_OK) return;

    if (rc == SQLITE_BUSY) {
        /* Surface the leak: a SQLITE_BUSY here means at least one prepared
         * statement was never finalized. Log each leaked statement with its
         * SQL so an operator can localize the bug, finalize them, then retry
         * close. After finalization the connection should close cleanly; if
         * not, log that too — the underlying pointer is still ours and the
         * connection is left open by sqlite3_close on BUSY. */
        int leaked = finalize_leaked_statements(db);
        ipman_log_error("sqlite3_close BUSY — finalized leaked statements",
                       "leaked=%d", leaked);
        rc = sqlite3_close(db);
        if (rc != SQLITE_OK) {
            ipman_log_error("sqlite3_close non-OK after finalize",
                           "rc=%d", rc);
        }
        return;
    }

    ipman_log_error("sqlite3_close non-OK", "rc=%d", rc);
}

#define IPMAN_BEGIN_RETRY_DEFAULT_MS 30000L
#define IPMAN_BEGIN_RETRY_CAP_MS     200

/* Parse IPMAN_BEGIN_RETRY_MS. Returns the configured budget; on missing,
 * empty, or unparseable values, falls back to the default and logs once. */
static long resolve_begin_retry_budget_ms(void) {
    const char *raw = getenv("IPMAN_BEGIN_RETRY_MS");
    if (raw == NULL || raw[0] == '\0') return IPMAN_BEGIN_RETRY_DEFAULT_MS;
    errno = 0;
    char *endp = NULL;
    long v = strtol(raw, &endp, 10);
    if (errno != 0 || endp == raw || (endp != NULL && *endp != '\0') || v < 0) {
        ipman_log_warn("invalid IPMAN_BEGIN_RETRY_MS; using default",
                      "value=\"%s\" default=%ld", raw,
                      IPMAN_BEGIN_RETRY_DEFAULT_MS);
        return IPMAN_BEGIN_RETRY_DEFAULT_MS;
    }
    return v;
}

int ipman_db_begin_immediate(sqlite3 *db) {
    long budget_ms = resolve_begin_retry_budget_ms();
    long elapsed_ms = 0;
    int  sleep_ms = 10;
    int  attempt = 0;

    for (;;) {
        ++attempt;
        char *err = NULL;
        int rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, &err);
        if (rc == SQLITE_OK) {
            sqlite3_free(err);
            if (attempt > 1) {
                ipman_log_info("BEGIN IMMEDIATE acquired after retry",
                              "attempts=%d elapsed_ms=%ld",
                              attempt, elapsed_ms);
            }
            return 0;
        }
        if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
            ipman_log_error("BEGIN IMMEDIATE failed",
                           "rc=%d detail=\"%s\" attempts=%d",
                           rc, err ? err : "(null)", attempt);
            sqlite3_free(err);
            return -1;
        }
        sqlite3_free(err);

        if (elapsed_ms >= budget_ms) {
            ipman_log_error("BEGIN IMMEDIATE busy budget exhausted",
                           "budget_ms=%ld elapsed_ms=%ld attempts=%d",
                           budget_ms, elapsed_ms, attempt);
            return -1;
        }

        struct timespec ts;
        ts.tv_sec  = sleep_ms / 1000;
        ts.tv_nsec = ((long)sleep_ms % 1000) * 1000000L;
        /* nanosleep can return early on signal; we just account elapsed
         * time and let the budget gate the loop. */
        (void)nanosleep(&ts, NULL);
        elapsed_ms += sleep_ms;
        sleep_ms = sleep_ms < IPMAN_BEGIN_RETRY_CAP_MS
                       ? sleep_ms * 2
                       : IPMAN_BEGIN_RETRY_CAP_MS;
    }
}
