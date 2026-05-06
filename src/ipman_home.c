/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "ipman_home.h"

#include "log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Walk upward from cwd looking for a `.git` entry (directory in a normal
 * clone, file in a git worktree). On hit, write the absolute path of the
 * containing directory to `out` and return 0. On no hit (cwd is not inside
 * any repo) or any error, return -1. */
static int find_repo_root_abs(char *out, size_t outlen) {
    char start[PATH_MAX];
    if (getcwd(start, sizeof start) == NULL) return -1;

    char cur[PATH_MAX];
    size_t n = strlen(start);
    if (n >= sizeof cur) return -1;
    memcpy(cur, start, n + 1);

    for (;;) {
        char probe[PATH_MAX];
        int pn = snprintf(probe, sizeof probe, "%s/.git", cur);
        if (pn < 0 || (size_t)pn >= sizeof probe) return -1;

        struct stat st;
        if (lstat(probe, &st) == 0) {
            /* Both directory (.git/) and file (worktree pointer) count.
             * Anything else (symlink, socket) we treat as not-a-marker. */
            if (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode)) {
                size_t curlen = strlen(cur);
                if (curlen >= outlen) return -1;
                memcpy(out, cur, curlen + 1);
                return 0;
            }
        }

        /* Walk up. Stop at the filesystem root. */
        if (strcmp(cur, "/") == 0) return -1;
        char *slash = strrchr(cur, '/');
        if (slash == NULL) return -1;
        if (slash == cur) {
            cur[1] = '\0';   /* keep just "/" for the next probe */
        } else {
            *slash = '\0';
        }
    }
}

/* Compute a cwd-relative path that lands at `target`. Both arguments must
 * be absolute. Returns 0 on success, -1 on failure. Examples:
 *   cwd=/a/b/c/d   target=/a/b   -> "../.."
 *   cwd=/a/b       target=/a/b   -> "."
 */
static int relativize_to_cwd(const char *target, char *out, size_t outlen) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd) == NULL) return -1;

    /* Find common prefix that ends on a directory boundary. */
    size_t i = 0;
    size_t last_slash = 0;
    while (cwd[i] != '\0' && target[i] != '\0' && cwd[i] == target[i]) {
        if (cwd[i] == '/') last_slash = i;
        ++i;
    }
    /* Boundary handling: equal strings, or one is a prefix of the other
     * exactly at a `/`. */
    int cwd_done = cwd[i] == '\0';
    int tgt_done = target[i] == '\0';
    if (cwd_done && tgt_done) {
        if (outlen < 2) return -1;
        memcpy(out, ".", 2);
        return 0;
    }
    if (cwd_done && target[i] == '/') {
        last_slash = i;
    } else if (tgt_done && cwd[i] == '/') {
        last_slash = i;
    }

    /* Count "../" hops needed to climb from cwd up to the common ancestor. */
    int ups = 0;
    for (size_t j = last_slash; cwd[j] != '\0'; ++j) {
        if (cwd[j] == '/' && cwd[j + 1] != '\0') ++ups;
    }

    /* Build "../"*ups + target_suffix. */
    size_t pos = 0;
    for (int k = 0; k < ups; ++k) {
        if (pos + 3 >= outlen) return -1;
        out[pos++] = '.';
        out[pos++] = '.';
        out[pos++] = (k == ups - 1) ? '\0' : '/';
    }
    if (ups == 0) {
        if (pos + 1 >= outlen) return -1;
        out[pos] = '\0';
    }

    /* Append the part of `target` after the common ancestor, if any. */
    const char *suffix = target + last_slash;
    if (*suffix == '/') ++suffix;
    if (*suffix != '\0') {
        size_t slen = strlen(suffix);
        if (ups > 0) {
            if (pos + 1 + slen + 1 > outlen) return -1;
            out[pos - 1] = '/';   /* convert trailing NUL of last "../" to "/" */
            out[pos++] = '\0';    /* will be overwritten */
            --pos;
        } else {
            if (pos + slen + 1 > outlen) return -1;
        }
        memcpy(out + pos, suffix, slen + 1);
    }
    return 0;
}

int ipman_home_resolve(char *out, size_t outlen) {
    const char *override = getenv("IPMAN_HOME");
    if (override != NULL && override[0] != '\0') {
        int n = snprintf(out, outlen, "%s", override);
        if (n < 0 || (size_t)n >= outlen) {
            ipman_log_error("ipman_home path too long", "source=IPMAN_HOME");
            return -1;
        }
        return 0;
    }

    /* No explicit IPMAN_HOME — try to anchor at the repo root so subdir/
     * worktree positioning never silently forks the workspace. */
    char root[PATH_MAX];
    if (find_repo_root_abs(root, sizeof root) == 0) {
        char rel[PATH_MAX];
        if (relativize_to_cwd(root, rel, sizeof rel) == 0) {
            int n = (strcmp(rel, ".") == 0)
                  ? snprintf(out, outlen, ".ipman")
                  : snprintf(out, outlen, "%s/.ipman", rel);
            if (n < 0 || (size_t)n >= outlen) {
                ipman_log_error("ipman_home path too long", "source=repo_root");
                return -1;
            }
            return 0;
        }
        /* Fall through to legacy default if relativization overflowed —
         * better to keep working than to refuse a valid invocation. */
    }

    int n = snprintf(out, outlen, ".ipman");
    if (n < 0 || (size_t)n >= outlen) {
        ipman_log_error("ipman_home path too long", "source=default");
        return -1;
    }
    return 0;
}

int ipman_home_repo_root_relpath(char *out, size_t outlen) {
    const char *override = getenv("IPMAN_HOME");
    if (override != NULL && override[0] != '\0') return -1;

    char root[PATH_MAX];
    if (find_repo_root_abs(root, sizeof root) != 0) return -1;
    return relativize_to_cwd(root, out, outlen);
}

int ipman_home_repo_root_abs(char *out, size_t outlen) {
    return find_repo_root_abs(out, outlen);
}

static int ipman_home_validate_stat(const char *path, const struct stat *st) {
    if (!S_ISDIR(st->st_mode)) {
        ipman_log_error("ipman_home is not a directory", "path=%s", path);
        return -1;
    }
    /* Reject any bits outside 0700 — plan history is sensitive. */
    mode_t extra = st->st_mode & 0077;
    if (extra != 0) {
        ipman_log_error("ipman_home has loose permissions",
                       "path=%s mode=%04o",
                       path, (unsigned)(st->st_mode & 0777));
        return -1;
    }
    if (st->st_uid != geteuid()) {
        ipman_log_error("ipman_home not owned by current user",
                       "path=%s", path);
        return -1;
    }
    return 0;
}

int ipman_home_require(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        ipman_log_error("ipman_home missing", "path=%s errno=%d", path, errno);
        return -1;
    }
    return ipman_home_validate_stat(path, &st);
}

int ipman_home_ensure(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return ipman_home_validate_stat(path, &st);

    if (errno != ENOENT) {
        ipman_log_error("cannot stat ipman_home", "path=%s errno=%d",
                       path, errno);
        return -1;
    }
    if (mkdir(path, 0700) != 0) {
        ipman_log_error("cannot create ipman_home", "path=%s errno=%d",
                       path, errno);
        return -1;
    }
    if (chmod(path, 0700) != 0) {
        ipman_log_error("cannot chmod ipman_home", "path=%s errno=%d",
                       path, errno);
        return -1;
    }
    ipman_log_info("created ipman_home", "path=%s mode=0700", path);
    return 0;
}
