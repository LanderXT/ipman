#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-cli-cancel.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export IPMAN_HOME="$TMP/home"

call_ipman() {
    printf '%s\n' "$1" | "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

"$BIN" init >/dev/null 2>/dev/null

plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"WV-3","title":"Cancel Verb","label":"cancel-verb"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Phase\",\"label\":\"the-phase\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')
phase_uid="phase_$phase_id"

task=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"task\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Task to cancel\",\"label\":\"the-task\"}}")
expect_ok "$task"
task_id=$(printf '%s' "$task" | jq -r '.result.task.id')
task_uid="task_$task_id"

activate=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"act\",\"actor\":\"test\",\"op\":\"plan.activate\",\"params\":{\"id\":$plan_id}}")
expect_ok "$activate"

# 1. Happy path: cancel a task
"$BIN" --cancel "$task_uid" \
    --summary "Out of scope" \
    --comment "Deprioritized" >/dev/null

state=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"q\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"id\":$task_id}}")
printf '%s' "$state" | jq -e '
  .result.task.status == "canceled"
  and .result.task.resolution == "canceled"
' >/dev/null

# 2. Invalid selector
if "$BIN" --cancel "no-such-label" --summary "x" --comment "y" >/dev/null 2>&1; then
    echo "expected --cancel to fail for unknown selector" >&2
    exit 1
fi

# 3. Wrong kind: --cancel expects a task, reject a phase selector
err=$("$BIN" --cancel "$phase_uid" --summary "x" --comment "y" 2>&1 >/dev/null) || true
printf '%s' "$err" | grep -q "phase"

# 4. Dry-run: prints envelope, no DB mutation
task2=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"t2\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Second task\"}}")
expect_ok "$task2"
task2_id=$(printf '%s' "$task2" | jq -r '.result.task.id')

dry=$("$BIN" --dry-run --cancel "task_$task2_id" \
        --summary "Would cancel" --comment "Dry run only")
printf '%s' "$dry" | jq -e '
  .request_id == "dry-run-cancel"
  and .op == "task.cancel"
  and .params.id == '"$task2_id"'
  and .params.outcome_summary == "Would cancel"
  and .params.closing_comment == "Dry run only"
' >/dev/null

final=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"f\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"id\":$task2_id}}")
printf '%s' "$final" | jq -e '.result.task.status == "todo"' >/dev/null
