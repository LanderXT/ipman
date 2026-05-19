# Branch-Aware Workspace Context

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When the user switches git branches, ipman automatically surfaces the plan that was active on that branch — no manual `--activate` required.

**Architecture:** A new `branch_contexts` table maps `branch_name → active_plan_id`. `plan.activate` upserts a binding for the current git branch (detected by reading `.git/HEAD`). `workspace.context_get` reads the current branch and uses its bound plan when one exists, falling back to the global `workspace_context.active_plan_id` when no binding exists or when the user is in detached HEAD. Two new ops — `workspace.list_branch_bindings` and `workspace.unbind_branch` — expose the bindings for inspection and cleanup.

**Tech Stack:** C, SQLite (via SQLCipher), cJSON, shell integration tests

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `migrations/0006_branch_contexts.sql` | Create | Schema for `branch_contexts` table |
| `src/git_helpers.h` | Create | `ipman_git_current_branch()` declaration |
| `src/git_helpers.c` | Create | Read current branch from `.git/HEAD` |
| `src/ipman_home.h` | Modify | Export `ipman_home_repo_root_abs()` |
| `src/ipman_home.c` | Modify | Implement `ipman_home_repo_root_abs()` |
| `src/context_ops.h` | Modify | Declare 2 new op functions + param arrays |
| `src/context_ops.c` | Modify | `plan.activate` branch binding; `context_get` branch lookup; 2 new ops |
| `src/dispatch.c` | Modify | Register 2 new ops |
| `src/agent_docs.c` | Modify | Document 2 new ops; update workspace_context entity description |
| `README.md` | Modify | Update op count claim: 76 → 78 |
| `Makefile` | Modify | Add unit test rule for `test_git_helpers` |
| `tests/unit/test_git_helpers.c` | Create | Unit test for `ipman_git_current_branch` |
| `tests/integration/140_branch_context.sh` | Create | Integration tests for full branch flow |

> **Makefile note:** `src/*.c` and `migrations/*.sql` are auto-discovered via `$(wildcard)` — no rule changes needed for `git_helpers.c` or `0006_branch_contexts.sql`. Only the unit test binary needs an explicit rule.

---

### Task 1: Migration — `branch_contexts` table

**Files:**
- Create: `migrations/0006_branch_contexts.sql`

- [ ] **Step 1: Write the migration SQL**

```sql
-- Migration 0006: per-branch workspace context
--
-- branch_contexts maps a git branch name to the plan that should be
-- active when the user is on that branch. plan.activate upserts a row
-- here when called inside a git repo; workspace.context_get reads it
-- to auto-select the active plan by branch.
--
-- ON DELETE RESTRICT on active_plan_id: branches can outlive a plan
-- (the plan may be closed), so we keep stale bindings rather than
-- silently deleting them. context_get skips terminal plans and falls
-- back to the global workspace_context.
CREATE TABLE IF NOT EXISTS branch_contexts (
    branch_name    TEXT PRIMARY KEY CHECK (length(trim(branch_name)) > 0),
    active_plan_id INTEGER NOT NULL,
    updated_at     TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_by     TEXT NOT NULL CHECK (length(trim(updated_by)) > 0),
    FOREIGN KEY (active_plan_id) REFERENCES plans(id) ON DELETE RESTRICT
);
```

- [ ] **Step 2: Verify the migration is picked up by the build**

```bash
make all 2>&1 | tail -5
```
Expected: build succeeds. If `embed_migrations.sh` ran, the output will reference `0006`.

- [ ] **Step 3: Verify the table exists after init**

```bash
TMP=$(mktemp -d)
IPMAN_HOME="$TMP" ./build/ipman init >/dev/null
sqlite3 "$TMP/ipman.db" ".tables"
```
Expected output includes: `branch_contexts`

- [ ] **Step 4: Commit**

```bash
git add migrations/0006_branch_contexts.sql
git commit -m "feat(migration): add branch_contexts table for per-branch plan binding"
```

---

### Task 2: Expose repo root + new `git_helpers.c/h`

**Files:**
- Modify: `src/ipman_home.h`
- Modify: `src/ipman_home.c`
- Create: `src/git_helpers.h`
- Create: `src/git_helpers.c`

> **Context:** `find_repo_root_abs` in `ipman_home.c` is `static`. Add a thin public wrapper so `git_helpers.c` can use it without duplicating the logic.

- [ ] **Step 1: Add `ipman_home_repo_root_abs` to `src/ipman_home.h`**

Add after the existing `ipman_home_repo_root_relpath` declaration:

```c
/* Write the absolute path of the repo root (the directory that contains
 * .git) into out. Returns 0 on success, -1 if not in a git repo. */
int ipman_home_repo_root_abs(char *out, size_t outlen);
```

- [ ] **Step 2: Implement `ipman_home_repo_root_abs` in `src/ipman_home.c`**

Add just before the final `ipman_home_ensure` function:

```c
int ipman_home_repo_root_abs(char *out, size_t outlen) {
    return find_repo_root_abs(out, outlen);
}
```

- [ ] **Step 3: Write `src/git_helpers.h`**

```c
/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_GIT_HELPERS_H
#define IPMAN_GIT_HELPERS_H

#include <stddef.h>

/* Write the current git branch name into out (e.g. "main", "feature/foo").
 * Reads .git/HEAD directly; no subprocess is spawned.
 * Returns 0 on success, -1 if not in a git repo or if HEAD is detached. */
int ipman_git_current_branch(char *out, size_t outlen);

#endif
```

- [ ] **Step 4: Write `src/git_helpers.c`**

```c
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
```

- [ ] **Step 5: Build and verify no compile errors**

```bash
make all 2>&1 | grep -E "error:|warning:" | head -20
```
Expected: no output (clean build)

- [ ] **Step 6: Commit**

```bash
git add src/ipman_home.h src/ipman_home.c src/git_helpers.h src/git_helpers.c
git commit -m "feat: add git_helpers for reading current branch from .git/HEAD"
```

---

### Task 3: Unit test for `ipman_git_current_branch`

**Files:**
- Create: `tests/unit/test_git_helpers.c`
- Modify: `Makefile`

- [ ] **Step 1: Write `tests/unit/test_git_helpers.c`**

```c
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
    snprintf(g_tmp, sizeof g_tmp, "%s/ipman-test-git.XXXXXX", P_tmpdir ? P_tmpdir : "/tmp");
    if (mkdtemp(g_tmp) == NULL) { perror("mkdtemp"); exit(2); }
}

static void teardown(void) {
    chdir(g_saved_cwd);
    /* remove tmp dir — simple recursive rm for two-level structure */
    char git_dir[PATH_MAX], refs[PATH_MAX], heads[PATH_MAX];
    snprintf(git_dir, sizeof git_dir, "%s/.git", g_tmp);
    snprintf(refs,    sizeof refs,    "%s/refs",  git_dir);
    snprintf(heads,   sizeof heads,   "%s/heads", refs);
    /* best-effort cleanup */
    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_tmp);
    (void)system(cmd);
}

/* Create a minimal real .git dir with HEAD pointing to branch_name. */
static void make_git_dir(const char *root, const char *branch_name) {
    char git[PATH_MAX], refs[PATH_MAX], heads[PATH_MAX], head_file[PATH_MAX];
    snprintf(git,       sizeof git,       "%s/.git",             root);
    snprintf(refs,      sizeof refs,      "%s/refs",             git);
    snprintf(heads,     sizeof heads,     "%s/heads",            refs);
    snprintf(head_file, sizeof head_file, "%s/HEAD",             git);
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
    char head_file[PATH_MAX];
    snprintf(head_file, sizeof head_file, "%s/.git/HEAD", g_tmp);
    write_file(head_file, "ref: refs/heads/feature/my-feature\n");
    chdir(g_tmp);

    char branch[256];
    ASSERT_EQ_INT(ipman_git_current_branch(branch, sizeof branch), 0);
    ASSERT_STR_EQ(branch, "feature/my-feature");
}

static void test_detached_head(void) {
    g_case = "detached_head";
    char head_file[PATH_MAX];
    snprintf(head_file, sizeof head_file, "%s/.git/HEAD", g_tmp);
    write_file(head_file, "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2\n");
    chdir(g_tmp);

    char branch[256];
    ASSERT_EQ_INT(ipman_git_current_branch(branch, sizeof branch), -1);
}

static void test_no_git_dir(void) {
    g_case = "no_git_dir";
    /* Create a subdir with no .git anywhere underneath */
    char no_git[PATH_MAX];
    snprintf(no_git, sizeof no_git, "%s/no_git", g_tmp);
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

    snprintf(wt_root,     sizeof wt_root,     "%s/worktree", g_tmp);
    snprintf(real_gitdir, sizeof real_gitdir, "%s/real_gitdir", g_tmp);
    snprintf(real_refs,   sizeof real_refs,   "%s/refs",  real_gitdir);
    snprintf(real_heads,  sizeof real_heads,  "%s/heads", real_refs);
    snprintf(real_head,   sizeof real_head,   "%s/HEAD",  real_gitdir);
    snprintf(wt_git_file, sizeof wt_git_file, "%s/.git",  wt_root);

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
```

- [ ] **Step 2: Add unit test rule to `Makefile`**

In `Makefile`, after the `$(BUILD_DIR)/tests/test_migration_upgrade` rule (around line 206), add:

```makefile
$(BUILD_DIR)/tests/test_git_helpers: tests/unit/test_git_helpers.c \
		$(BUILD_DIR)/git_helpers.o \
		$(BUILD_DIR)/ipman_home.o \
		$(BUILD_DIR)/log.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(SQLCIPHER_CFLAGS) $(SODIUM_CFLAGS) -Isrc -o $@ $^ $(LDLIBS)
```

- [ ] **Step 3: Run the unit test and verify it passes**

```bash
make $(BUILD_DIR)/tests/test_git_helpers 2>&1 | tail -5
./build/tests/test_git_helpers
```
Expected: `ok test_git_helpers (5 cases)`

- [ ] **Step 4: Commit**

```bash
git add tests/unit/test_git_helpers.c Makefile
git commit -m "test(unit): add test_git_helpers for branch detection"
```

---

### Task 4: Modify `plan.activate` to register branch binding

**Files:**
- Modify: `src/context_ops.c`

> **Context:** `ipman_op_plan_activate` is at line 730 of `context_ops.c`. After `ensure_plan_context` + `upsert_workspace_context` succeed (around line 780), add a branch upsert. The function already owns a transaction; the branch upsert joins it.

- [ ] **Step 1: Add `#include "git_helpers.h"` to `src/context_ops.c`**

At the top of `src/context_ops.c`, after the existing includes:

```c
#include "git_helpers.h"
```

- [ ] **Step 2: Add the `upsert_branch_context` static helper**

Add after `upsert_workspace_context` (around line 586):

```c
static int upsert_branch_context(sqlite3 *db,
                                 const char *branch_name,
                                 sqlite3_int64 plan_id,
                                 const char *actor) {
    const char *sql =
        "INSERT INTO branch_contexts(branch_name, active_plan_id, updated_by) "
        "VALUES(?1, ?2, ?3) "
        "ON CONFLICT(branch_name) DO UPDATE SET "
        "active_plan_id = excluded.active_plan_id, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
        "updated_by = excluded.updated_by;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, branch_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, plan_id);
    sqlite3_bind_text(stmt, 3, actor, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}
```

- [ ] **Step 3: Call `upsert_branch_context` inside `ipman_op_plan_activate`**

Find this block in `ipman_op_plan_activate` (around line 780):

```c
    if (ensure_plan_context(db, plan_id, req->actor) != 0 ||
        (old_ids.active_plan_id != plan_id &&
         upsert_workspace_context(db, plan_id, req->actor) != 0)) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to activate plan";
        return -1;
    }
```

Replace with:

```c
    if (ensure_plan_context(db, plan_id, req->actor) != 0 ||
        (old_ids.active_plan_id != plan_id &&
         upsert_workspace_context(db, plan_id, req->actor) != 0)) {
        cJSON_Delete(old_context);
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to activate plan";
        return -1;
    }
    /* Bind current git branch → this plan. Best-effort: not in a git
     * repo and detached HEAD are silent non-errors. */
    char branch[256];
    if (ipman_git_current_branch(branch, sizeof branch) == 0) {
        if (upsert_branch_context(db, branch, plan_id, req->actor) != 0) {
            cJSON_Delete(old_context);
            run_sql(db, "ROLLBACK;");
            *err_code_out = IPMAN_ERR_INTERNAL;
            *err_msg_out = "failed to record branch binding";
            return -1;
        }
    }
```

- [ ] **Step 4: Build and verify no errors**

```bash
make all 2>&1 | grep -E "error:|warning:" | head -20
```
Expected: no output

- [ ] **Step 5: Smoke-test that activate still works normally**

```bash
TMP=$(mktemp -d)
IPMAN_HOME="$TMP" ./build/ipman init >/dev/null
echo '{"protocol_version":2,"request_id":"p1","actor":"test","op":"plan.create","params":{"title":"Test","code":"T-1"}}' \
    | IPMAN_HOME="$TMP" ./build/ipman | jq '.ok'
# → true
echo '{"protocol_version":2,"request_id":"a1","actor":"test","op":"plan.activate","params":{"code":"T-1"}}' \
    | IPMAN_HOME="$TMP" ./build/ipman | jq '.ok'
# → true
```

- [ ] **Step 6: Commit**

```bash
git add src/context_ops.c
git commit -m "feat: plan.activate registers git branch → plan binding in branch_contexts"
```

---

### Task 5: Modify `workspace.context_get` to use branch binding

**Files:**
- Modify: `src/context_ops.c`

> **Context:** `load_context_with_requirements` at line 429 reads `active_plan_id` from `workspace_context`. After that read, insert a branch lookup that overrides `active_plan_id` when a live binding exists.

- [ ] **Step 1: Add `lookup_branch_plan` static helper**

Add after `upsert_branch_context`:

```c
/* Look up the plan bound to branch_name in branch_contexts.
 * Returns 0 and writes plan_id into *out if a row exists.
 * Returns -1 if no row or on error. *out is set to 0 on miss. */
static int lookup_branch_plan(sqlite3 *db,
                              const char *branch_name,
                              sqlite3_int64 *out) {
    *out = 0;
    const char *sql =
        "SELECT active_plan_id FROM branch_contexts "
        "WHERE branch_name = ?1 LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, branch_name, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}
```

- [ ] **Step 2: Add `plan_is_nonterminal` static helper**

Add after `lookup_branch_plan`:

```c
/* Return 1 if the plan exists and is in a non-terminal status,
 * 0 otherwise. Used to skip stale branch bindings. */
static int plan_is_nonterminal(sqlite3 *db, sqlite3_int64 plan_id) {
    const char *sql =
        "SELECT 1 FROM plans WHERE id = ?1 "
        "AND status NOT IN ('completed','canceled','archived') LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, plan_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? 1 : 0;
}
```

- [ ] **Step 3: Modify `load_context_with_requirements`**

Find these lines in `load_context_with_requirements` (around line 444):

```c
    sqlite3_int64 active_plan_id = 0;
    sqlite3_int64 current_phase_id = 0;
    sqlite3_int64 current_task_id = 0;
    if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        active_plan_id = sqlite3_column_int64(stmt, 0);
    }
    if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
        current_phase_id = sqlite3_column_int64(stmt, 3);
    }
    if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
        current_task_id = sqlite3_column_int64(stmt, 4);
    }
```

Replace with:

```c
    sqlite3_int64 active_plan_id = 0;
    sqlite3_int64 current_phase_id = 0;
    sqlite3_int64 current_task_id = 0;
    if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        active_plan_id = sqlite3_column_int64(stmt, 0);
    }
    if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
        current_phase_id = sqlite3_column_int64(stmt, 3);
    }
    if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
        current_task_id = sqlite3_column_int64(stmt, 4);
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    /* Branch override: if we can detect the current git branch and it
     * has a live binding, that plan takes precedence over the global
     * workspace_context.active_plan_id. Stale bindings (terminal plans)
     * are silently skipped. */
    char git_branch[256] = {0};
    int branch_bound = 0;
    if (ipman_git_current_branch(git_branch, sizeof git_branch) == 0) {
        sqlite3_int64 branch_plan_id = 0;
        if (lookup_branch_plan(db, git_branch, &branch_plan_id) == 0 &&
            branch_plan_id != 0 &&
            plan_is_nonterminal(db, branch_plan_id)) {
            active_plan_id = branch_plan_id;
            branch_bound = 1;
            /* Re-read phase/task cursors from the branch plan's context,
             * since they may differ from the global workspace cursor. */
            const char *cursor_sql =
                "SELECT current_phase_id, current_task_id "
                "FROM plan_contexts WHERE plan_id = ?1;";
            sqlite3_stmt *cs = NULL;
            if (sqlite3_prepare_v2(db, cursor_sql, -1, &cs, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(cs, 1, branch_plan_id);
                if (sqlite3_step(cs) == SQLITE_ROW) {
                    current_phase_id = (sqlite3_column_type(cs, 0) != SQLITE_NULL)
                                       ? sqlite3_column_int64(cs, 0) : 0;
                    current_task_id  = (sqlite3_column_type(cs, 1) != SQLITE_NULL)
                                       ? sqlite3_column_int64(cs, 1) : 0;
                }
                sqlite3_finalize(cs);
            }
        }
    }
```

> **Important:** The original code calls `sqlite3_finalize(stmt)` at line 484, after the new block. Remove that call since we moved it above.

Find and remove the standalone `sqlite3_finalize(stmt);` that would now be a double-finalize. The new block above already finalizes `stmt` before the branch override logic. Look for:

```c
    sqlite3_finalize(stmt);

    cJSON *plan = NULL;
```

Replace with just:

```c
    cJSON *plan = NULL;
```

- [ ] **Step 4: Add `git_branch` and `branch_bound` to the context JSON**

After the block that sets `active_plan_id` on the context object (around line 463):

```c
    if (active_plan_id == 0) {
        cJSON_AddNullToObject(context, "active_plan_id");
    } else {
        cJSON_AddNumberToObject(context, "active_plan_id", (double)active_plan_id);
    }
```

Add after it:

```c
    ipman_json_add_text_or_null(context, "git_branch",
                                git_branch[0] ? git_branch : NULL);
    cJSON_AddBoolToObject(context, "branch_bound", branch_bound);
```

- [ ] **Step 5: Build and verify no errors**

```bash
make all 2>&1 | grep -E "error:|warning:" | head -20
```

- [ ] **Step 6: Smoke-test context_get includes new fields**

```bash
TMP=$(mktemp -d)
IPMAN_HOME="$TMP" ./build/ipman init >/dev/null
echo '{"protocol_version":2,"request_id":"q1","actor":"test","op":"workspace.context_get","params":{}}' \
    | IPMAN_HOME="$TMP" ./build/ipman | jq '.result.context | {git_branch, branch_bound}'
```
Expected: `{"git_branch": "main", "branch_bound": false}` (or whatever branch you're on)

- [ ] **Step 7: Commit**

```bash
git add src/context_ops.c
git commit -m "feat: workspace.context_get uses branch-specific active plan when bound"
```

---

### Task 6: New ops — `workspace.list_branch_bindings` + `workspace.unbind_branch`

**Files:**
- Modify: `src/context_ops.c`
- Modify: `src/context_ops.h`

- [ ] **Step 1: Declare the two new ops in `src/context_ops.h`**

Add before `#endif`:

```c
int ipman_op_workspace_list_branch_bindings(const ipman_request_t *req,
                                            sqlite3 *db,
                                            cJSON **result_out,
                                            ipman_error_code_t *err_code_out,
                                            const char **err_msg_out);
int ipman_op_workspace_unbind_branch(const ipman_request_t *req,
                                     sqlite3 *db,
                                     cJSON **result_out,
                                     ipman_error_code_t *err_code_out,
                                     const char **err_msg_out);

extern const ipman_param_desc_t ipman_op_workspace_list_branch_bindings_params[];
extern const ipman_param_desc_t ipman_op_workspace_unbind_branch_params[];
```

- [ ] **Step 2: Implement `workspace.list_branch_bindings` in `src/context_ops.c`**

Add after `ipman_op_plan_deactivate`:

```c
const ipman_param_desc_t ipman_op_workspace_list_branch_bindings_params[] = {
    { NULL },
};

int ipman_op_workspace_list_branch_bindings(const ipman_request_t *req,
                                            sqlite3 *db,
                                            cJSON **result_out,
                                            ipman_error_code_t *err_code_out,
                                            const char **err_msg_out) {
    (void)req;
    const char *sql =
        "SELECT bc.branch_name, bc.active_plan_id, bc.updated_at, bc.updated_by, "
        "p.title, p.status "
        "FROM branch_contexts bc "
        "LEFT JOIN plans p ON p.id = bc.active_plan_id "
        "ORDER BY bc.branch_name;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to query branch bindings";
        return -1;
    }

    cJSON *bindings = cJSON_CreateArray();
    if (bindings == NULL) {
        sqlite3_finalize(stmt);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to allocate bindings array";
        return -1;
    }

    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        cJSON *b = cJSON_CreateObject();
        if (b == NULL) { cJSON_Delete(bindings); sqlite3_finalize(stmt);
                         *err_code_out = IPMAN_ERR_INTERNAL;
                         *err_msg_out = "failed to allocate binding"; return -1; }
        ipman_json_add_text_or_null(b, "branch_name",    sqlite3_column_text(stmt, 0));
        cJSON_AddNumberToObject(b, "active_plan_id", sqlite3_column_double(stmt, 1));
        ipman_json_add_text_or_null(b, "updated_at",     sqlite3_column_text(stmt, 2));
        ipman_json_add_text_or_null(b, "updated_by",     sqlite3_column_text(stmt, 3));
        ipman_json_add_text_or_null(b, "plan_title",     sqlite3_column_text(stmt, 4));
        ipman_json_add_text_or_null(b, "plan_status",    sqlite3_column_text(stmt, 5));
        cJSON_AddItemToArray(bindings, b);
    }
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        cJSON_Delete(bindings);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "error reading branch bindings";
        return -1;
    }

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) { cJSON_Delete(bindings);
                          *err_code_out = IPMAN_ERR_INTERNAL;
                          *err_msg_out = "failed to allocate result"; return -1; }
    cJSON_AddItemToObject(result, "bindings", bindings);
    cJSON_AddNumberToObject(result, "count", (double)cJSON_GetArraySize(bindings));
    *result_out = result;
    return 0;
}
```

- [ ] **Step 3: Implement `workspace.unbind_branch` in `src/context_ops.c`**

Add after `workspace.list_branch_bindings`:

```c
const ipman_param_desc_t ipman_op_workspace_unbind_branch_params[] = {
    { "branch" },
    { NULL },
};

int ipman_op_workspace_unbind_branch(const ipman_request_t *req,
                                     sqlite3 *db,
                                     cJSON **result_out,
                                     ipman_error_code_t *err_code_out,
                                     const char **err_msg_out) {
    /* Accept explicit branch param; fall back to current git branch. */
    char branch_buf[256] = {0};
    const char *branch = NULL;
    cJSON *branch_item = cJSON_GetObjectItemCaseSensitive(req->params, "branch");
    if (branch_item != NULL && cJSON_IsString(branch_item) &&
        branch_item->valuestring != NULL && branch_item->valuestring[0] != '\0') {
        branch = branch_item->valuestring;
    } else {
        if (ipman_git_current_branch(branch_buf, sizeof branch_buf) == 0) {
            branch = branch_buf;
        }
    }
    if (branch == NULL || branch[0] == '\0') {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "branch param required when not in a git repo or in detached HEAD";
        return -1;
    }

    if (ipman_db_begin_immediate(db) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to begin transaction";
        return -1;
    }

    const char *sql = "DELETE FROM branch_contexts WHERE branch_name = ?1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to prepare unbind statement";
        return -1;
    }
    sqlite3_bind_text(stmt, 1, branch, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        run_sql(db, "ROLLBACK;");
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to delete branch binding";
        return -1;
    }

    int deleted = sqlite3_changes(db);
    if (run_sql(db, "COMMIT;") != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to commit";
        return -1;
    }

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to allocate result";
        return -1;
    }
    ipman_json_add_text_or_null(result, "branch", branch);
    cJSON_AddBoolToObject(result, "deleted", deleted > 0);
    *result_out = result;
    return 0;
}
```

- [ ] **Step 4: Build and verify no errors**

```bash
make all 2>&1 | grep -E "error:|warning:" | head -20
```

- [ ] **Step 5: Commit**

```bash
git add src/context_ops.c src/context_ops.h
git commit -m "feat: add workspace.list_branch_bindings and workspace.unbind_branch ops"
```

---

### Task 7: Register ops in `dispatch.c` + document in `agent_docs.c`

**Files:**
- Modify: `src/dispatch.c`
- Modify: `src/agent_docs.c`
- Modify: `README.md`

- [ ] **Step 1: Register the two new ops in `src/dispatch.c`**

Find in `dispatch.c`:

```c
    { "workspace.context_get",        ipman_op_workspace_context_get,           ipman_op_workspace_context_get_params },
    { "workspace.export",             ipman_op_workspace_export,                ipman_op_workspace_export_params },
    { "workspace.refresh_agent_docs", ipman_op_workspace_refresh_agent_docs,    ipman_op_workspace_refresh_agent_docs_params },
```

Replace with:

```c
    { "workspace.context_get",           ipman_op_workspace_context_get,              ipman_op_workspace_context_get_params },
    { "workspace.export",                ipman_op_workspace_export,                   ipman_op_workspace_export_params },
    { "workspace.list_branch_bindings",  ipman_op_workspace_list_branch_bindings,     ipman_op_workspace_list_branch_bindings_params },
    { "workspace.refresh_agent_docs",    ipman_op_workspace_refresh_agent_docs,       ipman_op_workspace_refresh_agent_docs_params },
    { "workspace.unbind_branch",         ipman_op_workspace_unbind_branch,            ipman_op_workspace_unbind_branch_params },
```

Also add `#include "context_ops.h"` if it isn't already at the top of `dispatch.c` (check first).

- [ ] **Step 2: Add the two new ops to `agent_docs.c` OperationSpec table**

Find in `agent_docs.c` the block containing `"workspace.context_get"` and `"workspace.export"` entries and add after `"workspace.export"`:

```c
    { "workspace.list_branch_bindings", "workspace_context", "inspect", "List all git branch → plan bindings recorded in this workspace.", "none", "none", "params must be an object, normally empty.", "Initialized workspace.", "No business data changes.", "internal_error.", "result.bindings (array), result.count.", "workspace.context_get, workspace.unbind_branch, plan.activate", "{}", "", "bindings, count" },
    { "workspace.unbind_branch",        "workspace_context", "ongoing execution", "Remove the git branch → plan binding for the given branch (or the current branch when no param is supplied).", "branch (optional)", "branch", "branch must be non-empty when provided.", "Initialized workspace; if branch is omitted, must be inside a git repo with a named branch checked out.", "Deletes the branch_contexts row for the named branch. No-op if the branch was not bound.", "validation_failed, internal_error.", "result.branch, result.deleted.", "workspace.list_branch_bindings, plan.activate", "{\"branch\":\"feature/my-feature\"}", "", "branch, deleted" },
```

- [ ] **Step 3: Update `workspace_context` entity description in `agent_docs.c`**

Find the `workspace_context` entity row (contains `"workspace_context"` and `"workspace.context_get, plan.activate,"`). Update the operations list and description to include the two new ops:

Replace its operations field value from:
```
"workspace.context_get, plan.activate, plan.deactivate, phase.set_current, phase.clear_current, task.set_current, task.clear_current."
```
with:
```
"workspace.context_get, plan.activate, plan.deactivate, phase.set_current, phase.clear_current, task.set_current, task.clear_current, workspace.list_branch_bindings, workspace.unbind_branch."
```

And update its description field to mention branch bindings:
Replace:
```
"The active plan is global to the workspace. Current phase/task are explicit per-plan cursors and are not inferred from statuses."
```
with:
```
"The active plan is global to the workspace, but plan.activate also records a git branch → plan binding (branch_contexts). workspace.context_get uses the current branch's binding when one exists, falling back to the global active_plan_id. Current phase/task are explicit per-plan cursors and are not inferred from statuses."
```

- [ ] **Step 4: Update README.md op count (76 → 78)**

In `README.md`, update every occurrence of the op count claim:

```
# Find all occurrences:
grep -n "76 ops\|76 operations" README.md
```

Replace `76 ops` → `78 ops` and `76 operations` → `78 operations` (the entity count "13 entities" stays the same — the new ops are under `workspace_context`).

- [ ] **Step 5: Build and verify parity tests pass**

```bash
make all
TMP=$(mktemp -d)
IPMAN_HOME="$TMP" ./build/ipman init >/dev/null
sh tests/integration/000_operation_docs_parity.sh
sh tests/integration/005_readme_count_parity.sh
```
Expected: both print `ok`

- [ ] **Step 6: Commit**

```bash
git add src/dispatch.c src/agent_docs.c README.md
git commit -m "feat: register workspace.list_branch_bindings + workspace.unbind_branch ops"
```

---

### Task 8: Integration tests for the full branch flow

**Files:**
- Create: `tests/integration/140_branch_context.sh`

- [ ] **Step 1: Write `tests/integration/140_branch_context.sh`**

```sh
#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-branch-ctx.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export IPMAN_HOME="$TMP/home"

# Set up a real git repo so ipman_git_current_branch can detect branches.
REPO="$TMP/repo"
mkdir "$REPO"
git -C "$REPO" init -q
git -C "$REPO" -c user.email=t@t.com -c user.name=T commit --allow-empty -q -m "init"
cd "$REPO"

"$BIN" init >/dev/null

call_ipman() { printf '%s\n' "$1" | "$BIN" 2>/dev/null; }
expect_ok()  { printf '%s' "$1" | jq -e '.ok == true' >/dev/null; }

# Create two plans
planA=$(call_ipman '{"protocol_version":2,"request_id":"pA","actor":"t","op":"plan.create","params":{"title":"Plan A","code":"A-1"}}')
expect_ok "$planA"
planA_id=$(printf '%s' "$planA" | jq -r '.result.plan.id')

planB=$(call_ipman '{"protocol_version":2,"request_id":"pB","actor":"t","op":"plan.create","params":{"title":"Plan B","code":"B-1"}}')
expect_ok "$planB"
planB_id=$(printf '%s' "$planB" | jq -r '.result.plan.id')

# --- Test 1: activate plan A on main → branch binding created ---
git -C "$REPO" checkout -q main 2>/dev/null || git -C "$REPO" checkout -q -b main
"$BIN" --activate "plan_${planA_id}" >/dev/null

ctx=$(call_ipman '{"protocol_version":2,"request_id":"q1","actor":"t","op":"workspace.context_get","params":{}}')
printf '%s' "$ctx" | jq -e ".result.context.active_plan_id == $planA_id" >/dev/null
printf '%s' "$ctx" | jq -e '.result.context.branch_bound == true' >/dev/null
printf '%s' "$ctx" | jq -e '.result.context.git_branch == "main"' >/dev/null

# --- Test 2: switch to feature branch, activate plan B ---
git -C "$REPO" checkout -q -b feature/my-feature
"$BIN" --activate "plan_${planB_id}" >/dev/null

ctx=$(call_ipman '{"protocol_version":2,"request_id":"q2","actor":"t","op":"workspace.context_get","params":{}}')
printf '%s' "$ctx" | jq -e ".result.context.active_plan_id == $planB_id" >/dev/null
printf '%s' "$ctx" | jq -e '.result.context.git_branch == "feature/my-feature"' >/dev/null

# --- Test 3: switch back to main → automatically gets plan A ---
git -C "$REPO" checkout -q main
ctx=$(call_ipman '{"protocol_version":2,"request_id":"q3","actor":"t","op":"workspace.context_get","params":{}}')
printf '%s' "$ctx" | jq -e ".result.context.active_plan_id == $planA_id" >/dev/null
printf '%s' "$ctx" | jq -e '.result.context.git_branch == "main"' >/dev/null

# --- Test 4: workspace.list_branch_bindings shows both bindings ---
bindings=$(call_ipman '{"protocol_version":2,"request_id":"lb1","actor":"t","op":"workspace.list_branch_bindings","params":{}}')
expect_ok "$bindings"
printf '%s' "$bindings" | jq -e '.result.count == 2' >/dev/null
printf '%s' "$bindings" | jq -e '[.result.bindings[].branch_name] | contains(["main","feature/my-feature"])' >/dev/null

# --- Test 5: workspace.unbind_branch removes the feature binding ---
unbind=$(call_ipman '{"protocol_version":2,"request_id":"ub1","actor":"t","op":"workspace.unbind_branch","params":{"branch":"feature/my-feature"}}')
expect_ok "$unbind"
printf '%s' "$unbind" | jq -e '.result.deleted == true' >/dev/null

bindings2=$(call_ipman '{"protocol_version":2,"request_id":"lb2","actor":"t","op":"workspace.list_branch_bindings","params":{}}')
printf '%s' "$bindings2" | jq -e '.result.count == 1' >/dev/null

# --- Test 6: unbind non-existent branch → ok, deleted=false ---
noop=$(call_ipman '{"protocol_version":2,"request_id":"ub2","actor":"t","op":"workspace.unbind_branch","params":{"branch":"no-such-branch"}}')
expect_ok "$noop"
printf '%s' "$noop" | jq -e '.result.deleted == false' >/dev/null

echo "ok 140_branch_context"
```

- [ ] **Step 2: Make the script executable**

```bash
chmod +x tests/integration/140_branch_context.sh
```

- [ ] **Step 3: Run the integration test**

```bash
make all
sh tests/integration/140_branch_context.sh
```
Expected: `ok 140_branch_context`

- [ ] **Step 4: Run the full test suite**

```bash
make test
```
Expected: all tests pass

- [ ] **Step 5: Commit**

```bash
git add tests/integration/140_branch_context.sh
git commit -m "test(integration): branch-aware context — activate, switch, unbind"
```

---

### Task 9: Update `-N` handoff view to show git branch

**Files:**
- Modify: `src/main.c`

> **Context:** `run_next` at line 1883 calls `workspace.context_get` and reads `active_plan_id`, `current_phase_id`, `current_task_id` from the context. Add reading of `git_branch` and `branch_bound`, then print them in the project banner area.

- [ ] **Step 1: Read `git_branch` + `branch_bound` in `run_next`**

Find in `run_next` (around line 1895):

```c
    cJSON *context  = cJSON_GetObjectItemCaseSensitive(ctx, "context");
    cJSON *ap_id    = context ? cJSON_GetObjectItemCaseSensitive(context, "active_plan_id")   : NULL;
    cJSON *cph_id   = context ? cJSON_GetObjectItemCaseSensitive(context, "current_phase_id") : NULL;
    cJSON *ctk_id   = context ? cJSON_GetObjectItemCaseSensitive(context, "current_task_id")  : NULL;
    long plan_id  = (cJSON_IsNumber(ap_id))  ? (long)ap_id->valuedouble  : 0;
    long phase_id = (cJSON_IsNumber(cph_id)) ? (long)cph_id->valuedouble : 0;
    long task_id  = (cJSON_IsNumber(ctk_id)) ? (long)ctk_id->valuedouble : 0;
    cJSON *project = context ? cJSON_DetachItemFromObjectCaseSensitive(context, "project") : NULL;
    cJSON_Delete(ctx);
```

Replace with:

```c
    cJSON *context  = cJSON_GetObjectItemCaseSensitive(ctx, "context");
    cJSON *ap_id    = context ? cJSON_GetObjectItemCaseSensitive(context, "active_plan_id")   : NULL;
    cJSON *cph_id   = context ? cJSON_GetObjectItemCaseSensitive(context, "current_phase_id") : NULL;
    cJSON *ctk_id   = context ? cJSON_GetObjectItemCaseSensitive(context, "current_task_id")  : NULL;
    cJSON *gb_item  = context ? cJSON_GetObjectItemCaseSensitive(context, "git_branch")       : NULL;
    cJSON *bb_item  = context ? cJSON_GetObjectItemCaseSensitive(context, "branch_bound")     : NULL;
    long plan_id  = (cJSON_IsNumber(ap_id))  ? (long)ap_id->valuedouble  : 0;
    long phase_id = (cJSON_IsNumber(cph_id)) ? (long)cph_id->valuedouble : 0;
    long task_id  = (cJSON_IsNumber(ctk_id)) ? (long)ctk_id->valuedouble : 0;
    const char *git_branch  = (cJSON_IsString(gb_item)) ? gb_item->valuestring : NULL;
    int          branch_bound = cJSON_IsTrue(bb_item);
    cJSON *project = context ? cJSON_DetachItemFromObjectCaseSensitive(context, "project") : NULL;
    cJSON_Delete(ctx);
```

- [ ] **Step 2: Print branch context in `run_next` after the "no active plan" check**

Find (around line 1909):

```c
    if (plan_id <= 0) {
        /* No active plan, but the project block is still useful ... */
        next_print_project(stdout, project);
        if (project) cJSON_Delete(project);
        fprintf(stderr,
                "ipman next: no active plan — run `ipman --activate <plan>` first\n");
        ipman_db_close(db);
        return 1;
    }
```

Replace with:

```c
    if (plan_id <= 0) {
        next_print_project(stdout, project);
        if (project) cJSON_Delete(project);
        if (git_branch != NULL) {
            fprintf(stderr, "ipman next: on branch '%s' — no plan bound; "
                    "run `ipman --activate <plan>` to bind one\n", git_branch);
        } else {
            fprintf(stderr,
                    "ipman next: no active plan — run `ipman --activate <plan>` first\n");
        }
        ipman_db_close(db);
        return 1;
    }
    /* Print branch context before the plan view. */
    if (git_branch != NULL) {
        if (branch_bound) {
            fprintf(stdout, "Branch  %s  (bound)\n", git_branch);
        } else {
            fprintf(stdout, "Branch  %s\n", git_branch);
        }
    }
```

- [ ] **Step 3: Build and test `-N` manually**

```bash
make all
TMP=$(mktemp -d)
IPMAN_HOME="$TMP" ./build/ipman init >/dev/null
echo '{"protocol_version":2,"request_id":"p1","actor":"t","op":"plan.create","params":{"title":"My Plan","code":"MP-1"}}' \
    | IPMAN_HOME="$TMP" ./build/ipman | jq '.ok'
IPMAN_HOME="$TMP" ./build/ipman --activate plan_1 >/dev/null
IPMAN_HOME="$TMP" ./build/ipman -N
```
Expected: output includes `Branch  <current-branch>  (bound)` before the plan cursor block.

- [ ] **Step 4: Run the full test suite one final time**

```bash
make test
```
Expected: all tests pass

- [ ] **Step 5: Commit**

```bash
git add src/main.c
git commit -m "feat: -N handoff view shows current git branch and binding status"
```

---

## Self-Review Checklist

- [x] **Spec coverage:** Every requirement is covered — automatic binding on `plan.activate`, automatic lookup on `context_get`, inspect via `list_branch_bindings`, cleanup via `unbind_branch`, `-N` visibility.
- [x] **Placeholder scan:** All code blocks are complete and reference only types/functions defined in this plan or already in the codebase.
- [x] **Type consistency:** `upsert_branch_context`, `lookup_branch_plan`, and `plan_is_nonterminal` helpers are defined in Task 4/5 and referenced in Tasks 5/6. `ipman_git_current_branch` is defined in Task 2 and referenced in Tasks 4, 5, 6.
- [x] **Parity tests:** README count updated 76→78; agent_docs.c entries added for both ops; dispatch.c updated. Tasks 7 step 5 runs the parity tests explicitly before commit.
- [x] **Double-finalize guard:** Task 5 explicitly removes the now-duplicate `sqlite3_finalize(stmt)` call in `load_context_with_requirements`.
