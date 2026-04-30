#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-cli-activate.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export IPMAN_HOME="$TMP/home"

call_ipman() {
    printf '%s\n' "$1" | "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

"$BIN" init >/dev/null 2>/dev/null

planA=$(call_ipman '{"protocol_version":2,"request_id":"pA","actor":"test","op":"plan.create","params":{"code":"WV-A","title":"Plan A","label":"plan-a"}}')
expect_ok "$planA"
planA_id=$(printf '%s' "$planA" | jq -r '.result.plan.id')
planA_uid="plan_$planA_id"

planB=$(call_ipman '{"protocol_version":2,"request_id":"pB","actor":"test","op":"plan.create","params":{"code":"WV-B","title":"Plan B","label":"plan-b"}}')
expect_ok "$planB"
planB_id=$(printf '%s' "$planB" | jq -r '.result.plan.id')
planB_uid="plan_$planB_id"

# Need a task to exercise the wrong-kind case (task uid given to --activate)
phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$planA_id,\"title\":\"Phase\",\"label\":\"the-phase\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')

task=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"task\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$planA_id,\"phase_id\":$phase_id,\"title\":\"T\",\"label\":\"a-task\"}}")
expect_ok "$task"
task_id=$(printf '%s' "$task" | jq -r '.result.task.id')
task_uid="task_$task_id"

# 1. Happy path: activate plan A, then plan B
"$BIN" --activate "$planA_uid" >/dev/null
ctx=$(call_ipman '{"protocol_version":2,"request_id":"q","actor":"test","op":"workspace.context_get","params":{}}')
printf '%s' "$ctx" | jq -e '.result.context.active_plan_id == '"$planA_id"' ' >/dev/null

"$BIN" --activate "$planB_uid" >/dev/null
ctx=$(call_ipman '{"protocol_version":2,"request_id":"q2","actor":"test","op":"workspace.context_get","params":{}}')
printf '%s' "$ctx" | jq -e '.result.context.active_plan_id == '"$planB_id"' ' >/dev/null

# 2. Invalid selector
if "$BIN" --activate "no-such-plan" >/dev/null 2>&1; then
    echo "expected --activate to fail for unknown selector" >&2
    exit 1
fi

# 3. Wrong kind: --activate expects a plan, reject a task selector
err=$("$BIN" --activate "$task_uid" 2>&1 >/dev/null) || true
printf '%s' "$err" | grep -q "task"

# 4. Dry-run: prints envelope, no DB mutation
# Active plan is currently B; dry-run-activating A should not change it
dry=$("$BIN" --dry-run --activate "$planA_uid")
printf '%s' "$dry" | jq -e '
  .request_id == "dry-run-activate"
  and .op == "plan.activate"
  and .params.id == '"$planA_id"'
' >/dev/null

ctx=$(call_ipman '{"protocol_version":2,"request_id":"f","actor":"test","op":"workspace.context_get","params":{}}')
printf '%s' "$ctx" | jq -e '.result.context.active_plan_id == '"$planB_id"' ' >/dev/null
