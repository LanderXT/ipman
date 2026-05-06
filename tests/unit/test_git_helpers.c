/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 *
 * Unit tests for ipman_git_current_branch.
 * Creates fake .git structures in /tmp to avoid depending on the test
 * runner's actual repo.
 */

#include "git_helpers.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;
static const char *g_case = "(none)";

#define FAILF(fmt, ...) do { \
    fprintf(stderr, "  FAIL [%s] " fmt " (at %s:%d)\n", \
            g_case, __VA_ARGS__, __FILE__, __LINE__); \
    g_failures++; \
    return; \
} while (0)

#define ASSERT_STR_EQ(actual, expected) do { \
    if (strcmp((actual), (expected)) != 0) \
        FAILF("expected \"%s\", got \"%s\"", (expected), (actual)); \
} while (0)

#define ASSERT_EQ_INT(actual, expected) do { \
    int _a = (int)(actual), _e = (int)(expected); \
    if (_a != _e) FAILF("expected %d, got %d", _e, _a); \
} while (0)

/* Build dst = dir + "/" + name using memcpy to avoid -Wformat-truncation. */
static void path_concat(char *dst, size_t dsz, const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl + 1 + nl + 1 > dsz) { fprintf(stderr, "path too long\n"); exit(2); }
    memcpy(dst, dir, dl);
    dst[dl] = '/';
    memcpy(dst + dl + 1, name, nl + 1);
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno)); exit(2); }
    fputs(content, f);
    fclose(f);
}

static char g_saved_cwd[PATH_MAX];
static char g_tmp[PATH_MAX];

static void setup(void) {
    if (getcwd(g_saved_cwd, sizeof g_saved_cwd) == NULL) { perror("getcwd"); exit(2); }
    snprintf(g_tmp, sizeof g_tmp, "/tmp/ipman-test-git.XXXXXX");
    if (mkdtemp(g_tmp) == NULL) { perror("mkdtemp"); exit(2); }
}

static void teardown(void) {
    chdir(g_saved_cwd);
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_tmp);
    (void)system(cmd);
}

/* Create a minimal real .git dir with HEAD pointing to branch_name. */
static void make_git_dir(const char *root, const char *branch_name) {
    char git[PATH_MAX], refs[PATH_MAX], heads[PATH_MAX], head_file[PATH_MAX];
    path_concat(git,       sizeof git,       root, ".git");
    path_concat(refs,      sizeof refs,      git,  "refs");
    path_concat(heads,     sizeof heads,     refs, "heads");
    path_concat(head_file, sizeof head_file, git,  "HEAD");
    mkdir(git,   0755);
    mkdir(refs,  0755);
    mkdir(heads, 0755);
    char content[512];
    snprintf(content, sizeof content, "ref: refs/heads/%s\n", branch_name);
    write_file(head_file, content);
}

/* --- Test cases --- */

static void test_normal_branch(void) {
    g_case = "normal_branch";
    make_git_dir(g_tmp, "main");
    chdir(g_tmp);

    char branch[256];
    ASSERT_EQ_INT(ipman_git_current_branch(branch, sizeof branch), 0);
    ASSERT_STR_EQ(branch, "main");
}

static void test_feature_branch_with_slash(void) {
    g_case = "feature_branch_with_slash";
    /* reuse existing .git dir, just overwrite HEAD */
    char git_dir[PATH_MAX], head_file[PATH_MAX];
    path_concat(git_dir,   sizeof git_dir,   g_tmp,   ".git");
    path_concat(head_file, sizeof head_file, git_dir, "HEAD");
    write_file(head_file, "ref: refs/heads/feature/my-feature\n");
    chdir(g_tmp);

    char branch[256];
    ASSERT_EQ_INT(ipman_git_current_branch(branch, sizeof branch), 0);
    ASSERT_STR_EQ(branch, "feature/my-feature");
}

static void test_detached_head(void) {
    g_case = "detached_head";
    char git_dir[PATH_MAX], head_file[PATH_MAX];
    path_concat(git_dir,   sizeof git_dir,   g_tmp,   ".git");
    path_concat(head_file, sizeof head_file, git_dir, "HEAD");
    write_file(head_file, "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2\n");
    chdir(g_tmp);

    char branch[256];
    ASSERT_EQ_INT(ipman_git_current_branch(branch, sizeof branch), -1);
}

static void test_no_git_dir(void) {
    g_case = "no_git_dir";
    /* Create a subdir with no .git anywhere underneath */
    char no_git[PATH_MAX];
    path_concat(no_git, sizeof no_git, g_tmp, "no_git");
    mkdir(no_git, 0755);
    chdir(no_git);

    char branch[256];
    ASSERT_EQ_INT(ipman_git_current_branch(branch, sizeof branch), -1);
}

static void test_worktree(void) {
    g_case = "worktree";
    /* Simulate a worktree: .git is a file pointing to a separate gitdir */
    char wt_root[PATH_MAX], real_gitdir[PATH_MAX], wt_git_file[PATH_MAX];
    char real_refs[PATH_MAX], real_heads[PATH_MAX], real_head[PATH_MAX];

    path_concat(wt_root,     sizeof wt_root,     g_tmp,       "worktree");
    path_concat(real_gitdir, sizeof real_gitdir, g_tmp,       "real_gitdir");
    path_concat(real_refs,   sizeof real_refs,   real_gitdir, "refs");
    path_concat(real_heads,  sizeof real_heads,  real_refs,   "heads");
    path_concat(real_head,   sizeof real_head,   real_gitdir, "HEAD");
    path_concat(wt_git_file, sizeof wt_git_file, wt_root,     ".git");

    mkdir(wt_root,     0755);
    mkdir(real_gitdir, 0755);
    mkdir(real_refs,   0755);
    mkdir(real_heads,  0755);

    write_file(real_head, "ref: refs/heads/wt-branch\n");

    /* .git file in worktree root */
    char gitdir_content[PATH_MAX + 16];
    snprintf(gitdir_content, sizeof gitdir_content, "gitdir: %s\n", real_gitdir);
    write_file(wt_git_file, gitdir_content);

    chdir(wt_root);
    char branch[256];
    ASSERT_EQ_INT(ipman_git_current_branch(branch, sizeof branch), 0);
    ASSERT_STR_EQ(branch, "wt-branch");
}

int main(void) {
    setup();
    test_normal_branch();
    test_feature_branch_with_slash();
    test_detached_head();
    test_no_git_dir();
    test_worktree();
    teardown();

    if (g_failures > 0) {
        fprintf(stderr, "%d test(s) FAILED\n", g_failures);
        return 1;
    }
    printf("ok test_git_helpers (5 cases)\n");
    return 0;
}
