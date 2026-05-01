#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-cli-close-phase.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export IPMAN_HOME="$TMP/home"

call_ipman() {
    printf '%s\n' "$1" | "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

"$BIN" init >/dev/null 2>/dev/null

plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"CP-1","title":"Close Phase","label":"close-phase"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Phase A\",\"label\":\"phase-a\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')
phase_uid="phase_$phase_id"

task=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"task\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Child\",\"label\":\"child\"}}")
expect_ok "$task"
task_id=$(printf '%s' "$task" | jq -r '.result.task.id')
task_uid="task_$task_id"

activate=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"act\",\"actor\":\"test\",\"op\":\"plan.activate\",\"params\":{\"id\":$plan_id}}")
expect_ok "$activate"

# 1. Precondition surfaces: cannot close a phase with non-terminal child tasks
if "$BIN" --close-phase "$phase_uid" --summary "early" --comment "should fail" >/dev/null 2>&1; then
    echo "expected --close-phase to fail with non-terminal child task" >&2
    exit 1
fi

# Resolve the child task before closing the phase
"$BIN" --close "$task_uid" --summary "child done" --comment "ok" >/dev/null

# 2. Happy path: close the phase with summary, comment, lessons, open-items, followup
"$BIN" --close-phase "$phase_uid" \
    --summary "All tasks resolved" \
    --comment "Phase complete" \
    --lessons "Verify preconditions before close" \
    --open-items "None" \
    --followup >/dev/null

state=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"q\",\"actor\":\"test\",\"op\":\"phase.get\",\"params\":{\"id\":$phase_id}}")
printf '%s' "$state" | jq -e '.result.phase.status == "completed"' >/dev/null

closure=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"c\",\"actor\":\"test\",\"op\":\"closure.get\",\"params\":{\"entity_type\":\"phase\",\"entity_id\":$phase_id}}")
printf '%s' "$closure" | jq -e '
  .result.active_closure.outcome_summary == "All tasks resolved"
  and .result.active_closure.lessons_learned == "Verify preconditions before close"
  and .result.active_closure.followup_needed == true
' >/dev/null

# 3. Wrong kind: --close-phase rejects a task selector
err=$("$BIN" --close-phase "$task_uid" --summary "x" --comment "y" 2>&1 >/dev/null) || true
printf '%s' "$err" | grep -q "task"

# 4. Dry-run: prints envelope with outcome=completed, no DB mutation
phase2=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"p2\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Phase B\",\"label\":\"phase-b\"}}")
expect_ok "$phase2"
phase2_id=$(printf '%s' "$phase2" | jq -r '.result.phase.id')

dry=$("$BIN" --dry-run --close-phase "phase_$phase2_id" \
        --summary "Would close" --comment "Dry run only")
printf '%s' "$dry" | jq -e '
  .request_id == "dry-run-close-phase"
  and .op == "phase.close"
  and .params.id == '"$phase2_id"'
  and .params.outcome == "completed"
  and .params.outcome_summary == "Would close"
  and .params.closing_comment == "Dry run only"
' >/dev/null

final=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"f\",\"actor\":\"test\",\"op\":\"phase.get\",\"params\":{\"id\":$phase2_id}}")
printf '%s' "$final" | jq -e '.result.phase.status == "open"' >/dev/null
