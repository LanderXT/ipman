#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

if ! command -v git >/dev/null 2>&1; then
    echo "skipping: git not on PATH" >&2
    exit 0
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-cli-close-git.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export IPMAN_HOME="$TMP/home"

call_ipman() {
    printf '%s\n' "$1" | "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

"$BIN" init >/dev/null 2>/dev/null

plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"WV-GIT","title":"Close With Git","label":"close-with-git"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

mk_task() {
    local title=$1
    local resp
    resp=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"t-$title\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"$title\"}}")
    expect_ok "$resp"
    printf '%s' "$resp" | jq -r '.result.task.id'
}

# --- Set up a controlled git repo with two commits so HEAD~1 exists. ---
WS="$TMP/repo"
mkdir "$WS"
(
    cd "$WS"
    git -c init.defaultBranch=main init -q
    git config user.email "test@example"
    git config user.name "Test"
    printf 'first\n' > a.c
    git add a.c
    git -c commit.gpgsign=false commit -q -m "first"
    printf 'second\n' > b.c
    git add b.c
    git -c commit.gpgsign=false commit -q -m "second"
)
sha_head=$(cd "$WS" && git rev-parse HEAD)

# --- 1. In-repo plain --close auto-captures sha + dirty=false + files [b.c] ---
t1=$(mk_task "InRepoPlain")
dry=$(cd "$WS" && IPMAN_HOME="$IPMAN_HOME" "$BIN" --dry-run --close "task_$t1" \
        --summary "ok" --comment "ok")
printf '%s' "$dry" | jq -e --arg sha "$sha_head" '
    .params.commit_sha == $sha
    and .params.dirty == false
    and (.params.files_changed | sort) == ["b.c"]
' >/dev/null

# --- 2. --no-git suppresses every git field even inside the repo. ---
t2=$(mk_task "NoGit")
dry=$(cd "$WS" && IPMAN_HOME="$IPMAN_HOME" "$BIN" --dry-run --close "task_$t2" \
        --summary "ok" --comment "ok" --no-git)
printf '%s' "$dry" | jq -e '
    (.params | has("commit_sha") | not)
    and (.params | has("dirty") | not)
    and (.params | has("files_changed") | not)
' >/dev/null

# --- 3. Dirty working tree: uncommitted change flips dirty to true. ---
t3=$(mk_task "DirtyTree")
printf 'mod\n' > "$WS/a.c"
dry=$(cd "$WS" && IPMAN_HOME="$IPMAN_HOME" "$BIN" --dry-run --close "task_$t3" \
        --summary "ok" --comment "ok")
printf '%s' "$dry" | jq -e '.params.dirty == true' >/dev/null
(cd "$WS" && git checkout -q -- a.c)

# --- 4. --commit override replaces auto-captured sha. ---
t4=$(mk_task "CommitOverride")
dry=$(cd "$WS" && IPMAN_HOME="$IPMAN_HOME" "$BIN" --dry-run --close "task_$t4" \
        --summary "ok" --comment "ok" --commit "FORCED_SHA")
printf '%s' "$dry" | jq -e '
    .params.commit_sha == "FORCED_SHA"
    and (.params.files_changed | sort) == ["b.c"]
' >/dev/null

# --- 5. --files override replaces auto-captured diff (CSV trimmed). ---
t5=$(mk_task "FilesOverride")
dry=$(cd "$WS" && IPMAN_HOME="$IPMAN_HOME" "$BIN" --dry-run --close "task_$t5" \
        --summary "ok" --comment "ok" --files "x.c, y.c , z.c")
printf '%s' "$dry" | jq -e '.params.files_changed == ["x.c","y.c","z.c"]' >/dev/null

# --- 6. --no-git + --commit is rejected with a sharp error message. ---
t6=$(mk_task "Conflict")
err=$("$BIN" --close "task_$t6" --summary x --comment y --no-git --commit abc 2>&1 >/dev/null) || rc=$?
[ "${rc:-0}" -ne 0 ]
printf '%s' "$err" | grep -q "no-git cannot be combined"

# --- 7. Outside any git repo: silent fail — no fields populated. ---
NONREPO="$TMP/nonrepo"
mkdir "$NONREPO"
# Sanity: confirm the dir is not inside a repo before relying on the assertion.
if (cd "$NONREPO" && git rev-parse --is-inside-work-tree >/dev/null 2>&1); then
    echo "skipping out-of-repo check: $NONREPO is unexpectedly inside a git repo" >&2
else
    t7=$(mk_task "OutOfRepo")
    dry=$(cd "$NONREPO" && IPMAN_HOME="$IPMAN_HOME" "$BIN" --dry-run --close "task_$t7" \
            --summary "ok" --comment "ok")
    printf '%s' "$dry" | jq -e '
        (.params | has("commit_sha") | not)
        and (.params | has("dirty") | not)
        and (.params | has("files_changed") | not)
    ' >/dev/null

    # Outside repo with --commit override: only commit_sha present.
    t7b=$(mk_task "OutOfRepoCommit")
    dry=$(cd "$NONREPO" && IPMAN_HOME="$IPMAN_HOME" "$BIN" --dry-run --close "task_${t7b}" \
            --summary "ok" --comment "ok" --commit "MANUAL")
    printf '%s' "$dry" | jq -e '
        .params.commit_sha == "MANUAL"
        and (.params | has("dirty") | not)
        and (.params | has("files_changed") | not)
    ' >/dev/null
fi

# --- 8. End-to-end persistence: real --close inside repo, then closure.get. ---
t8=$(mk_task "Persisted")
(cd "$WS" && IPMAN_HOME="$IPMAN_HOME" "$BIN" --close "task_$t8" \
        --summary "Implemented" --comment "Verified" >/dev/null)

closure=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"cg\",\"actor\":\"test\",\"op\":\"closure.get\",\"params\":{\"entity_type\":\"task\",\"entity_id\":$t8}}")
printf '%s' "$closure" | jq -e --arg sha "$sha_head" '
    .result.active_closure.commit_sha == $sha
    and .result.active_closure.dirty == false
    and (.result.active_closure.files_changed | sort) == ["b.c"]
' >/dev/null

# --- 9. End-to-end with --no-git: closure has no git fields. ---
t9=$(mk_task "PersistedNoGit")
(cd "$WS" && IPMAN_HOME="$IPMAN_HOME" "$BIN" --close "task_$t9" \
        --summary "ok" --comment "ok" --no-git >/dev/null)

closure=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"cgn\",\"actor\":\"test\",\"op\":\"closure.get\",\"params\":{\"entity_type\":\"task\",\"entity_id\":$t9}}")
printf '%s' "$closure" | jq -e '
    (.result.active_closure | has("commit_sha") | not)
    and (.result.active_closure | has("dirty") | not)
    and (.result.active_closure | has("files_changed") | not)
' >/dev/null
