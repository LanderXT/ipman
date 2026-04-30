#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-cli-defer.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export IPMAN_HOME="$TMP/home"

call_ipman() {
    printf '%s\n' "$1" | "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

"$BIN" init >/dev/null 2>/dev/null

plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"WV-4","title":"Defer Verb","label":"defer-verb"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Phase\",\"label\":\"the-phase\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')
phase_uid="phase_$phase_id"

task=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"task\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Task to defer\",\"label\":\"the-task\"}}")
expect_ok "$task"
task_id=$(printf '%s' "$task" | jq -r '.result.task.id')
task_uid="task_$task_id"

activate=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"act\",\"actor\":\"test\",\"op\":\"plan.activate\",\"params\":{\"id\":$plan_id}}")
expect_ok "$activate"

# 1. Happy path: defer a task with reason text + code
"$BIN" --defer "$task_uid" \
    --reason-text "Blocked by upstream API change" \
    --reason-code "external_dependency" >/dev/null

state=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"q\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"id\":$task_id}}")
printf '%s' "$state" | jq -e '
  .result.task.status == "deferred"
  and .result.task.reason_text == "Blocked by upstream API change"
  and .result.task.reason_code == "external_dependency"
' >/dev/null

# 2. Invalid selector
if "$BIN" --defer "no-such-label" --reason-text "x" >/dev/null 2>&1; then
    echo "expected --defer to fail for unknown selector" >&2
    exit 1
fi

# 3. Wrong kind: --defer expects a task, reject a phase selector
err=$("$BIN" --defer "$phase_uid" --reason-text "x" 2>&1 >/dev/null) || true
printf '%s' "$err" | grep -q "phase"

# 4. Dry-run: prints envelope, no DB mutation (reason-code optional, omit it)
task2=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"t2\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Second task\"}}")
expect_ok "$task2"
task2_id=$(printf '%s' "$task2" | jq -r '.result.task.id')

dry=$("$BIN" --dry-run --defer "task_$task2_id" \
        --reason-text "Would defer")
printf '%s' "$dry" | jq -e '
  .request_id == "dry-run-defer"
  and .op == "task.defer"
  and .params.id == '"$task2_id"'
  and .params.reason_text == "Would defer"
  and (.params | has("reason_code") | not)
' >/dev/null

final=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"f\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"id\":$task2_id}}")
printf '%s' "$final" | jq -e '.result.task.status == "todo"' >/dev/null
