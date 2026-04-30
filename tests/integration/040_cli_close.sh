#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-cli-close.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export IPMAN_HOME="$TMP/home"

call_ipman() {
    printf '%s\n' "$1" | "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

"$BIN" init >/dev/null 2>/dev/null

plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"WV-2","title":"Close Verb","label":"close-verb"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Phase\",\"label\":\"the-phase\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')
phase_uid="phase_$phase_id"

task=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"task\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Task to close\",\"label\":\"the-task\"}}")
expect_ok "$task"
task_id=$(printf '%s' "$task" | jq -r '.result.task.id')
task_uid="task_$task_id"

activate=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"act\",\"actor\":\"test\",\"op\":\"plan.activate\",\"params\":{\"id\":$plan_id}}")
expect_ok "$activate"

# 1. Happy path: close a task with required summary/comment plus optional flags
"$BIN" --close "$task_uid" \
    --summary "Implemented and verified" \
    --comment "Standard closure flow" \
    --lessons "Selectors made this trivial" \
    --open-items "None" \
    --followup >/dev/null

state=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"q\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"id\":$task_id}}")
printf '%s' "$state" | jq -e '
  .result.task.status == "done"
  and .result.task.resolution == "completed"
' >/dev/null

# Verify closure record carries the optional fields
closure=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"c\",\"actor\":\"test\",\"op\":\"closure.get\",\"params\":{\"entity_type\":\"task\",\"entity_id\":$task_id}}")
printf '%s' "$closure" | jq -e '
  .result.active_closure.outcome_summary == "Implemented and verified"
  and .result.active_closure.lessons_learned == "Selectors made this trivial"
  and .result.active_closure.followup_needed == true
' >/dev/null

# 2. Invalid selector
if "$BIN" --close "no-such-label" --summary "x" --comment "y" >/dev/null 2>&1; then
    echo "expected --close to fail for unknown selector" >&2
    exit 1
fi

# 3. Wrong kind: --close expects a task, reject a phase selector
err=$("$BIN" --close "$phase_uid" --summary "x" --comment "y" 2>&1 >/dev/null) || true
printf '%s' "$err" | grep -q "phase"

# 4. Dry-run: prints envelope, no DB mutation
task2=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"t2\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Second task\"}}")
expect_ok "$task2"
task2_id=$(printf '%s' "$task2" | jq -r '.result.task.id')

dry=$("$BIN" --dry-run --close "task_$task2_id" \
        --summary "Would close" --comment "Dry run only")
printf '%s' "$dry" | jq -e '
  .request_id == "dry-run-close"
  and .op == "task.close"
  and .params.id == '"$task2_id"'
  and .params.outcome_summary == "Would close"
  and .params.closing_comment == "Dry run only"
' >/dev/null

final=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"f\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"id\":$task2_id}}")
printf '%s' "$final" | jq -e '.result.task.status == "todo"' >/dev/null
