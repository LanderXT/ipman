#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-cli-cancel-phase.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export IPMAN_HOME="$TMP/home"

call_ipman() {
    printf '%s\n' "$1" | "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

"$BIN" init >/dev/null 2>/dev/null

plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"XP-1","title":"Cancel Phase","label":"cancel-phase"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Phase X\",\"label\":\"phase-x\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')
phase_uid="phase_$phase_id"

task=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"task\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Child\",\"label\":\"child\"}}")
expect_ok "$task"
task_id=$(printf '%s' "$task" | jq -r '.result.task.id')

# Resolve child so the phase can be terminated
"$BIN" --close "task_$task_id" --summary "child done" --comment "ok" >/dev/null

activate=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"act\",\"actor\":\"test\",\"op\":\"plan.activate\",\"params\":{\"id\":$plan_id}}")
expect_ok "$activate"

# 1. Happy path: cancel the phase by uid; verify status=canceled and closure record
"$BIN" --cancel-phase "$phase_uid" \
    --summary "Out of scope for v2.2" \
    --comment "Deprioritized after planning review" >/dev/null

state=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"q\",\"actor\":\"test\",\"op\":\"phase.get\",\"params\":{\"id\":$phase_id}}")
printf '%s' "$state" | jq -e '.result.phase.status == "canceled"' >/dev/null

closure=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"c\",\"actor\":\"test\",\"op\":\"closure.get\",\"params\":{\"entity_type\":\"phase\",\"entity_id\":$phase_id}}")
printf '%s' "$closure" | jq -e '
  .result.active_closure.outcome_summary == "Out of scope for v2.2"
  and .result.active_closure.closing_comment == "Deprioritized after planning review"
' >/dev/null

# 2. Wrong kind: --cancel-phase rejects a task selector
err=$("$BIN" --cancel-phase "task_$task_id" --summary "x" --comment "y" 2>&1 >/dev/null) || true
printf '%s' "$err" | grep -q "task"

# 3. Dry-run: prints envelope with outcome=canceled, no DB mutation
phase2=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"p2\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Phase Y\",\"label\":\"phase-y\"}}")
expect_ok "$phase2"
phase2_id=$(printf '%s' "$phase2" | jq -r '.result.phase.id')

dry=$("$BIN" --dry-run --cancel-phase "phase-y" \
        --summary "Would cancel" --comment "Dry run only")
printf '%s' "$dry" | jq -e '
  .request_id == "dry-run-cancel-phase"
  and .op == "phase.close"
  and .params.id == '"$phase2_id"'
  and .params.outcome == "canceled"
  and .params.outcome_summary == "Would cancel"
  and .params.closing_comment == "Dry run only"
' >/dev/null

final=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"f\",\"actor\":\"test\",\"op\":\"phase.get\",\"params\":{\"id\":$phase2_id}}")
printf '%s' "$final" | jq -e '.result.phase.status == "open"' >/dev/null
