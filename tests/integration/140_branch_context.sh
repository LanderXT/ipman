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
