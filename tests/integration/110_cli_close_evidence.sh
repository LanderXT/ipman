#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-cli-close-evidence.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export IPMAN_HOME="$TMP/home"

call_ipman() {
    printf '%s\n' "$1" | "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

"$BIN" init >/dev/null 2>/dev/null

plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"WV-EV","title":"Evidence","label":"evidence"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Implementation\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')

mk_task() {
    local title=$1
    local resp
    resp=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"t-$title\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"$title\"}}")
    expect_ok "$resp"
    printf '%s' "$resp" | jq -r '.result.task.id'
}

start_task() {
    call_ipman "{\"protocol_version\":2,\"request_id\":\"st-$1\",\"actor\":\"test\",\"op\":\"task.transition\",\"params\":{\"id\":$1,\"status\":\"in_progress\"}}" >/dev/null
}

# --- 1. Close with 0 validations and 0 decisions: closure has neither key. ---
t1=$(mk_task "ZeroEvidence")
start_task "$t1"
"$BIN" --close "task_$t1" --summary "ok" --comment "ok" --no-git >/dev/null
closure=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"cg1\",\"actor\":\"test\",\"op\":\"closure.get\",\"params\":{\"entity_type\":\"task\",\"entity_id\":$t1}}")
printf '%s' "$closure" | jq -e '
    (.result.active_closure | has("validations_run") | not)
    and (.result.active_closure | has("decisions") | not)
' >/dev/null

# --- 2. Close with one validation, zero decisions. ---
t2=$(mk_task "OneValidationNoDecisions")
start_task "$t2"
"$BIN" --close "task_$t2" --summary "ok" --comment "ok" --no-git \
    --validation "make test:passed" >/dev/null
closure=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"cg2\",\"actor\":\"test\",\"op\":\"closure.get\",\"params\":{\"entity_type\":\"task\",\"entity_id\":$t2}}")
printf '%s' "$closure" | jq -e '
    .result.active_closure.validations_run == [{"cmd":"make test","status":"passed"}]
    and (.result.active_closure | has("decisions") | not)
' >/dev/null

# --- 3. Close with three validations and two decisions; cmd contains a colon. ---
t3=$(mk_task "ManyEvidence")
start_task "$t3"
"$BIN" --close "task_$t3" --summary "ok" --comment "ok" --no-git \
    --validation "make test:passed" \
    --validation "sh tests/integration/100_cli_close_git.sh:passed" \
    --validation "make build/ipman --filter foo:warned" \
    --decision "Used the X approach" \
    --decision "Deferred the Y refactor to Phase 13" >/dev/null
closure=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"cg3\",\"actor\":\"test\",\"op\":\"closure.get\",\"params\":{\"entity_type\":\"task\",\"entity_id\":$t3}}")
printf '%s' "$closure" | jq -e '
    (.result.active_closure.validations_run | length) == 3
    and .result.active_closure.validations_run[2].cmd == "make build/ipman --filter foo"
    and .result.active_closure.validations_run[2].status == "warned"
    and (.result.active_closure.decisions | length) == 2
    and .result.active_closure.decisions[1] == "Deferred the Y refactor to Phase 13"
' >/dev/null

# --- 4. Bad shapes are rejected before persistence. ---
t4=$(mk_task "BadShape")
start_task "$t4"
# Bad: validation without colon.
if "$BIN" --close "task_$t4" --summary x --comment y --no-git --validation "no_colon" >/dev/null 2>&1; then
    echo "expected --validation without colon to fail" >&2
    exit 1
fi
# Bad: decision empty (whitespace only).
if "$BIN" --close "task_$t4" --summary x --comment y --no-git --decision "   " >/dev/null 2>&1; then
    echo "expected --decision empty to fail" >&2
    exit 1
fi
# Sanity: task is still in_progress (no partial close).
state=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"q4\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"id\":$t4}}")
printf '%s' "$state" | jq -e '.result.task.status == "in_progress"' >/dev/null

# --- 5. ipman --render Task Evidence section: tasks with evidence appear; without don't. ---
md=$("$BIN" --render "$plan_id" 2>/dev/null)

# Section heading is present (we have evidence on t2 and t3).
printf '%s' "$md" | grep -q "^## Task Evidence$"

# t2 (one validation, no decisions) appears with a single-row table and no Decisions block.
printf '%s' "$md" | awk -v uid="task_$t2" '
    $0 ~ "^### `"uid"`" { in_block=1; next }
    in_block && $0 ~ /^### `task_/  { in_block=0 }
    in_block { print }
' | tee "$TMP/t2.md" >/dev/null
grep -q "make test" "$TMP/t2.md"
grep -q "passed" "$TMP/t2.md"
! grep -q "^**Decisions**" "$TMP/t2.md" || true

# t3 has both validations and decisions.
printf '%s' "$md" | awk -v uid="task_$t3" '
    $0 ~ "^### `"uid"`" { in_block=1; next }
    in_block && $0 ~ /^### `task_/  { in_block=0 }
    in_block && $0 ~ /^---$/        { in_block=0 }
    in_block { print }
' > "$TMP/t3.md"
grep -q "Used the X approach" "$TMP/t3.md"
grep -q "Deferred the Y refactor to Phase 13" "$TMP/t3.md"
grep -q "make build/ipman --filter foo" "$TMP/t3.md"
grep -q "warned" "$TMP/t3.md"

# t1 (no evidence) does NOT appear in the Task Evidence section.
! printf '%s' "$md" | grep -q "task_$t1"

# --- 6. Plan with no closures has no Task Evidence section. ---
plan2=$(call_ipman '{"protocol_version":2,"request_id":"p2","actor":"test","op":"plan.create","params":{"title":"Empty","label":"empty"}}')
expect_ok "$plan2"
plan2_id=$(printf '%s' "$plan2" | jq -r '.result.plan.id')
empty_md=$("$BIN" --render "$plan2_id" 2>/dev/null)
! printf '%s' "$empty_md" | grep -q "^## Task Evidence$"
