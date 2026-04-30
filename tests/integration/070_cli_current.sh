#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-cli-current.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export IPMAN_HOME="$TMP/home"

call_ipman() {
    printf '%s\n' "$1" | "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

"$BIN" init >/dev/null 2>/dev/null

plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"WV-5","title":"Current Verb","label":"current-verb"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')
plan_uid="plan_$plan_id"

phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Phase\",\"label\":\"the-phase\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')
phase_uid="phase_$phase_id"

task=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"task\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Task X\",\"label\":\"the-task\"}}")
expect_ok "$task"
task_id=$(printf '%s' "$task" | jq -r '.result.task.id')
task_uid="task_$task_id"

activate=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"act\",\"actor\":\"test\",\"op\":\"plan.activate\",\"params\":{\"id\":$plan_id}}")
expect_ok "$activate"

# 1. Happy path: --current auto-detects task vs phase
"$BIN" --current "$task_uid" >/dev/null
ctx=$(call_ipman '{"protocol_version":2,"request_id":"q","actor":"test","op":"workspace.context_get","params":{}}')
printf '%s' "$ctx" | jq -e '.result.context.current_task_id == '"$task_id"' ' >/dev/null

"$BIN" --current "$phase_uid" >/dev/null
ctx=$(call_ipman '{"protocol_version":2,"request_id":"q2","actor":"test","op":"workspace.context_get","params":{}}')
printf '%s' "$ctx" | jq -e '.result.context.current_phase_id == '"$phase_id"' ' >/dev/null

# 2. Invalid selector
if "$BIN" --current "no-such-label" >/dev/null 2>&1; then
    echo "expected --current to fail for unknown selector" >&2
    exit 1
fi

# 3. Wrong kind: --current accepts task or phase, reject a plan selector
err=$("$BIN" --current "$plan_uid" 2>&1 >/dev/null) || true
printf '%s' "$err" | grep -q "plan"

# 4. Dry-run: prints envelope, no DB mutation
# Reset current task to null so we can detect mutation
clear=$(call_ipman '{"protocol_version":2,"request_id":"clr","actor":"test","op":"task.clear_current","params":{}}')
expect_ok "$clear"

dry=$("$BIN" --dry-run --current "$task_uid")
printf '%s' "$dry" | jq -e '
  .request_id == "dry-run-current"
  and .op == "task.set_current"
  and .params.id == '"$task_id"'
' >/dev/null

ctx=$(call_ipman '{"protocol_version":2,"request_id":"f","actor":"test","op":"workspace.context_get","params":{}}')
printf '%s' "$ctx" | jq -e '.result.context.current_task_id == null' >/dev/null
