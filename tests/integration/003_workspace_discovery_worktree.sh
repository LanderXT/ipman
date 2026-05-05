#!/bin/sh
set -eu

# Workspace discovery: walk upward to find the repo root and resolve
# .ipman/ relative to it, not to cwd. Closes the silent-fork hazard
# where an agent in a subdir or worktree initializes a stray .ipman/
# alongside the workspace whose data the user actually cares about.
#
# Three scenarios in one test:
#   (a) cwd is a subdir of a repo with an existing .ipman/ at the root
#       -> ipman ops resolve to the repo root .ipman/, not the subdir.
#   (b) cwd is the repo root with NO .ipman/ but the agent runs init
#       from a subdir -> init refuses with a clear message.
#   (c) cwd is a git worktree (has a `.git` FILE not directory) and no
#       .ipman/ at the worktree root -> read ops still find the worktree
#       root, write ops fail loud rather than initializing a stray
#       workspace.
#
# IPMAN_HOME explicit set always bypasses discovery (operator escape hatch).

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-discovery.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

# --- Scenario (a): subdir of a repo with existing .ipman/ at root. ----
mkdir -p "$TMP/repo-a/sub/deeper"
( cd "$TMP/repo-a" && git init -q )
# Initialize ipman at the repo root.
( cd "$TMP/repo-a" && env -u IPMAN_HOME "$BIN" init >/dev/null 2>&1 )
test -d "$TMP/repo-a/.ipman" || { echo "(a) setup failed: no .ipman at root" >&2; exit 1; }

# From a subdir, an op without IPMAN_HOME must resolve to the repo root .ipman/.
# Use plan.list (read op): it should succeed and return zero plans.
out_a=$(cd "$TMP/repo-a/sub/deeper" && env -u IPMAN_HOME printf '%s\n' \
    '{"protocol_version":2,"request_id":"a","actor":"test","op":"plan.list","params":{}}' \
    | "$BIN" 2>/dev/null)
printf '%s' "$out_a" | jq -e '.ok == true and (.result.plans | length) == 0' >/dev/null \
    || { echo "(a) op from subdir did not resolve to repo root .ipman" >&2; printf '%s\n' "$out_a" >&2; exit 1; }

# Confirm no stray .ipman/ was created in the subdir.
test ! -e "$TMP/repo-a/sub/.ipman" || { echo "(a) stray .ipman in subdir" >&2; exit 1; }
test ! -e "$TMP/repo-a/sub/deeper/.ipman" || { echo "(a) stray .ipman in deeper subdir" >&2; exit 1; }

# --- Scenario (b): init from subdir of a repo refuses. ---------------
mkdir -p "$TMP/repo-b/sub"
( cd "$TMP/repo-b" && git init -q )

# init from the subdir must refuse — agent should be forced to cd to root.
# The refuse is a semantic error (validation_failed): JSON envelope on stdout
# with .ok=false; exit code stays 0 because it is recoverable.
init_b=$(cd "$TMP/repo-b/sub" && env -u IPMAN_HOME "$BIN" init 2>/dev/null)
printf '%s' "$init_b" | jq -e '.ok == false and .error.code == "validation_failed"' >/dev/null \
    || { echo "(b) init from subdir did not refuse with validation_failed" >&2; printf '%s\n' "$init_b" >&2; exit 1; }
printf '%s' "$init_b" | jq -e '.error.message | test("subdir|root|repo")' >/dev/null \
    || { echo "(b) refuse message missing repo/subdir/root keyword" >&2; printf '%s\n' "$init_b" >&2; exit 1; }
test ! -e "$TMP/repo-b/.ipman"      || { echo "(b) .ipman created at root despite refuse" >&2; exit 1; }
test ! -e "$TMP/repo-b/sub/.ipman"  || { echo "(b) .ipman created in subdir" >&2; exit 1; }

# init from the actual root succeeds.
( cd "$TMP/repo-b" && env -u IPMAN_HOME "$BIN" init >/dev/null 2>&1 )
test -d "$TMP/repo-b/.ipman" || { echo "(b) init at root failed" >&2; exit 1; }

# --- Scenario (c): git worktree (.git is a FILE pointing into the main
# repo's .git/worktrees/<name>). No .ipman/ at the worktree root. ----
mkdir -p "$TMP/repo-c"
( cd "$TMP/repo-c" && git init -q && git -c user.email=t@x -c user.name=t commit --allow-empty -m init -q )
WORKTREE="$TMP/wt-c"
( cd "$TMP/repo-c" && git worktree add -q "$WORKTREE" 2>/dev/null )
# Sanity: .git in the worktree is a FILE, not a directory.
test -f "$WORKTREE/.git" || { echo "(c) worktree setup did not produce .git file" >&2; exit 1; }

# An op from the worktree without IPMAN_HOME and without a worktree-local
# .ipman must NOT silently create one. plan.list (read) should fail with
# "not initialized" rather than succeed against a freshly-spawned dir.
out_c=$(cd "$WORKTREE" && env -u IPMAN_HOME printf '%s\n' \
    '{"protocol_version":2,"request_id":"c","actor":"test","op":"plan.list","params":{}}' \
    | "$BIN" 2>/dev/null || true)
printf '%s' "$out_c" | jq -e '.ok == false' >/dev/null \
    || { echo "(c) read op should have failed in worktree without .ipman" >&2; printf '%s\n' "$out_c" >&2; exit 1; }
test ! -e "$WORKTREE/.ipman" || { echo "(c) stray .ipman created in worktree" >&2; exit 1; }

# init from the worktree root IS allowed (the worktree IS the resolved repo
# root for the worktree's perspective). This is the expected escape hatch
# for "I deliberately want a per-worktree workspace".
( cd "$WORKTREE" && env -u IPMAN_HOME "$BIN" init >/dev/null 2>&1 )
test -d "$WORKTREE/.ipman" || { echo "(c) deliberate init at worktree root failed" >&2; exit 1; }

# --- IPMAN_HOME bypass: when explicit, no discovery, no refuse. ------
mkdir -p "$TMP/explicit-home"
( IPMAN_HOME="$TMP/explicit-home/.ipman" "$BIN" init >/dev/null 2>&1 )
test -d "$TMP/explicit-home/.ipman" || { echo "explicit IPMAN_HOME bypass failed" >&2; exit 1; }

# --- Anchor-at-cwd: cwd IS the resolved repo root, init succeeds. ---
# We need a directory whose nearest .git ancestor is cwd itself, so the
# "init must run at repo root" check sees rel=="." and lets init
# proceed. Plant a stub .git here to short-circuit the upward walk
# before it hits whatever the host OS may have at /tmp/.git or higher.
ANCHORED="$TMP/anchored"
mkdir -p "$ANCHORED/.git"
( cd "$ANCHORED" && env -u IPMAN_HOME "$BIN" init >/dev/null 2>&1 )
test -d "$ANCHORED/.ipman" || { echo "anchored init at cwd failed" >&2; exit 1; }

echo "ok 003_workspace_discovery_worktree"
