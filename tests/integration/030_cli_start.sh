#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-cli-start.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export IPMAN_HOME="$TMP/home"

call_ipman() {
    printf '%s\n' "$1" | "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

"$BIN" init >/dev/null 2>/dev/null

plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"WV-1","title":"Write Verbs","label":"write-verbs"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Phase\",\"label\":\"the-phase\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')
phase_uid="phase_$phase_id"

task=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"task\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Task title\",\"label\":\"the-task\"}}")
expect_ok "$task"
task_id=$(printf '%s' "$task" | jq -r '.result.task.id')
task_uid="task_$task_id"

activate=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"act\",\"actor\":\"test\",\"op\":\"plan.activate\",\"params\":{\"id\":$plan_id}}")
expect_ok "$activate"

# 1. Happy path: --start a task by uid
"$BIN" --start "$task_uid" >/dev/null
state=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"q1\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"id\":$task_id}}")
printf '%s' "$state" | jq -e '.result.task.status == "in_progress"' >/dev/null

# 2. Invalid selector: nonexistent label fails
if "$BIN" --start "no-such-label" >/dev/null 2>&1; then
    echo "expected --start to fail for unknown selector" >&2
    exit 1
fi

# 3. Wrong kind: --start expects a task, reject a phase selector
err=$("$BIN" --start "$phase_uid" 2>&1 >/dev/null) || true
printf '%s' "$err" | grep -q "phase"

# 4. Dry-run: prints envelope on stdout, exit 0, no DB mutation
reset=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"r\",\"actor\":\"test\",\"op\":\"task.transition\",\"params\":{\"id\":$task_id,\"status\":\"todo\"}}")
expect_ok "$reset"

dry=$("$BIN" --dry-run --start "$task_uid")
printf '%s' "$dry" | jq -e '
  .request_id == "dry-run-start"
  and .op == "task.transition"
  and .params.id == '"$task_id"'
  and .params.status == "in_progress"
' >/dev/null

final=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"f\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"id\":$task_id}}")
printf '%s' "$final" | jq -e '.result.task.status == "todo"' >/dev/null
