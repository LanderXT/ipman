/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "git_helpers.h"
#include "ipman_home.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int ipman_git_current_branch(char *out, size_t outlen) {
    char root[PATH_MAX];
    if (ipman_home_repo_root_abs(root, sizeof root) != 0) return -1;

    char git_entry[PATH_MAX];
    if (snprintf(git_entry, sizeof git_entry, "%s/.git", root) >= (int)sizeof git_entry)
        return -1;

    struct stat st;
    if (lstat(git_entry, &st) != 0) return -1;

    char head_path[PATH_MAX];
    if (S_ISREG(st.st_mode)) {
        /* Worktree: .git is a file containing "gitdir: /real/gitdir\n" */
        FILE *f = fopen(git_entry, "r");
        if (!f) return -1;
        char buf[PATH_MAX + 16];
        char *line = fgets(buf, sizeof buf, f);
        fclose(f);
        if (!line) return -1;

        const char *prefix = "gitdir: ";
        size_t plen = strlen(prefix);
        if (strncmp(buf, prefix, plen) != 0) return -1;

        size_t dlen = strcspn(buf + plen, "\n\r");
        if (dlen == 0 || plen + dlen + 6 >= sizeof head_path) return -1;
        memcpy(head_path, buf + plen, dlen);
        memcpy(head_path + dlen, "/HEAD", 6);
    } else if (S_ISDIR(st.st_mode)) {
        if (snprintf(head_path, sizeof head_path, "%s/.git/HEAD", root)
            >= (int)sizeof head_path) return -1;
    } else {
        return -1;
    }

    FILE *f = fopen(head_path, "r");
    if (!f) return -1;
    char buf[512];
    char *line = fgets(buf, sizeof buf, f);
    fclose(f);
    if (!line) return -1;

    const char *ref_prefix = "ref: refs/heads/";
    size_t rplen = strlen(ref_prefix);
    if (strncmp(buf, ref_prefix, rplen) != 0) return -1; /* detached HEAD */

    const char *branch = buf + rplen;
    size_t blen = strcspn(branch, "\n\r");
    if (blen == 0 || blen >= outlen) return -1;
    memcpy(out, branch, blen);
    out[blen] = '\0';
    return 0;
}
