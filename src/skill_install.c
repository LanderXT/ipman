/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "skill_install.h"

#include "log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SKILL_DIR_MODE  0755
#define SKILL_FILE_MODE 0644

struct skill_target {
    const char *label;
    const char *global_relpath;
    const char *local_root;
    const char *local_skills_dir;
    const char *local_skill_dir;
    const char *local_skill_file;
};

static const struct skill_target g_skill_targets[] = {
    {
        "claude",
        ".claude/skills/ipman/SKILL.md",
        ".claude",
        ".claude/skills",
        ".claude/skills/ipman",
        ".claude/skills/ipman/SKILL.md",
    },
    {
        "codex",
        ".codex/skills/ipman/SKILL.md",
        ".codex",
        ".codex/skills",
        ".codex/skills/ipman",
        ".codex/skills/ipman/SKILL.md",
    },
};

/*
 * Returns:
 *   0  if the directory exists after the call
 *   1  if the path is blocked by an existing non-directory entry
 *  -1  on other filesystem errors
 */
static int mkdir_idempotent(const char *path) {
    if (mkdir(path, SKILL_DIR_MODE) == 0) return 0;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) != 0) return -1;
        return S_ISDIR(st.st_mode) ? 0 : 1;
    }
    return -1;
}

static int skill_global_exists(const char *home,
                               const struct skill_target *target) {
    char global[PATH_MAX];
    int n = snprintf(global, sizeof global, "%s/%s", home,
                     target->global_relpath);
    return n > 0 && (size_t)n < sizeof global && access(global, F_OK) == 0;
}

static int skill_write_local(const struct skill_target *target) {
    int mk_root = mkdir_idempotent(target->local_root);
    int mk_skills = mk_root == 0 ? mkdir_idempotent(target->local_skills_dir) : mk_root;
    int mk_skill = mk_skills == 0 ? mkdir_idempotent(target->local_skill_dir) : mk_skills;

    if (mk_root > 0 || mk_skills > 0 || mk_skill > 0) {
        return 0;
    }
    if (mk_root != 0 || mk_skills != 0 || mk_skill != 0) {
        ipman_log_warn("skill install: failed to create parent dirs",
                      "target=%s errno=%d", target->label, errno);
        return -1;
    }

    FILE *fp = fopen(target->local_skill_file, "w");
    if (fp == NULL) {
        ipman_log_warn("skill install: failed to open SKILL.md",
                      "target=%s errno=%d", target->label, errno);
        return -1;
    }
    size_t wrote = fwrite(ipman_skill_md, 1, ipman_skill_md_len, fp);
    int close_rc = fclose(fp);
    if (wrote != ipman_skill_md_len || close_rc != 0) {
        ipman_log_warn("skill install: failed to write SKILL.md",
                      "target=%s errno=%d", target->label, errno);
        return -1;
    }
    if (chmod(target->local_skill_file, SKILL_FILE_MODE) != 0) {
        /* Non-fatal: file is written under the umask-adjusted default mode. */
        ipman_log_warn("skill install: chmod failed",
                      "target=%s errno=%d", target->label, errno);
    }
    return 0;
}

int ipman_skill_install(void) {
    const char *home = getenv("HOME");
    int rc = 0;

    for (size_t i = 0; i < sizeof g_skill_targets / sizeof g_skill_targets[0];
         i++) {
        const struct skill_target *target = &g_skill_targets[i];
        if (home != NULL && home[0] != '\0' && skill_global_exists(home, target)) {
            continue;
        }
        if (skill_write_local(target) != 0) {
            rc = -1;
        }
    }
    return rc;
}
