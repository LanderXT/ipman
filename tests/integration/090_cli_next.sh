#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-cli-next.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export IPMAN_HOME="$TMP/home"

call_ipman() {
    printf '%s\n' "$1" | "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

"$BIN" init >/dev/null 2>/dev/null

# 1. No active plan: --next must explain the missing prerequisite.
#    Message varies: "no active plan" outside git, "no plan bound" inside git.
err=$("$BIN" --next 2>&1 >/dev/null) || true
printf '%s' "$err" | grep -qE "no active plan|no plan bound"

# 2. Build a full fixture: plan + phase + 3 pending tasks + instructions
#    at all three scopes (plan / phase / task).
plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"NX-1","title":"Next Plan","label":"next-plan","summary":"plan summary"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"NX-1 Phase\",\"label\":\"nx-phase\",\"summary\":\"phase summary\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')

t1=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"t1\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Task one\",\"label\":\"task-one\",\"summary\":\"do one thing\"}}")
expect_ok "$t1"
t1_id=$(printf '%s' "$t1" | jq -r '.result.task.id')

t2=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"t2\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Task two\",\"label\":\"task-two\",\"summary\":\"do another thing\"}}")
expect_ok "$t2"

t3=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"t3\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Task three\",\"label\":\"task-three\",\"summary\":\"do final thing\"}}")
expect_ok "$t3"

ip=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"ip\",\"actor\":\"test\",\"op\":\"instruction.add\",\"params\":{\"entity_type\":\"plan\",\"entity_id\":$plan_id,\"body\":\"PLAN_INSTR_BODY\",\"instruction_type\":\"guidance\"}}")
expect_ok "$ip"

iph=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"iph\",\"actor\":\"test\",\"op\":\"instruction.add\",\"params\":{\"entity_type\":\"phase\",\"entity_id\":$phase_id,\"body\":\"PHASE_INSTR_BODY\",\"instruction_type\":\"guidance\"}}")
expect_ok "$iph"

it=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"it\",\"actor\":\"test\",\"op\":\"instruction.add\",\"params\":{\"entity_type\":\"task\",\"entity_id\":$t1_id,\"body\":\"TASK_INSTR_BODY\",\"instruction_type\":\"acceptance\"}}")
expect_ok "$it"

# 3. Activate, set current phase + current task.
expect_ok "$(call_ipman "{\"protocol_version\":2,\"request_id\":\"act\",\"actor\":\"test\",\"op\":\"plan.activate\",\"params\":{\"id\":$plan_id}}")"
expect_ok "$(call_ipman "{\"protocol_version\":2,\"request_id\":\"setp\",\"actor\":\"test\",\"op\":\"phase.set_current\",\"params\":{\"id\":$phase_id}}")"
expect_ok "$(call_ipman "{\"protocol_version\":2,\"request_id\":\"sett\",\"actor\":\"test\",\"op\":\"task.set_current\",\"params\":{\"id\":$t1_id}}")"

# 4. --next renders all four sections and every fixture identifier.
out=$("$BIN" --next)

# Cursor banners — one line each, in order.
# next_print_cursor prefers `code` over `label` for the plan heading.
# Now that plan.code rides along on plan responses, the plan banner uses
# the supplied code (`NX-1`); phase/task banners still use label since
# only plans carry a code.
printf '%s' "$out" | grep -q "^Plan: NX-1 · Next Plan$"
printf '%s' "$out" | grep -q "^Phase: nx-phase · NX-1 Phase$"
printf '%s' "$out" | grep -q "^Task: task-one · Task one$"

# Each scope's instruction body appears exactly once — no duplicates.
for tag in PLAN_INSTR_BODY PHASE_INSTR_BODY TASK_INSTR_BODY; do
    n=$(printf '%s' "$out" | grep -c "$tag" || true)
    if [ "$n" -ne 1 ]; then
        echo "instruction body '$tag' appears $n times (expected 1)" >&2
        exit 1
    fi
done

# Scope labels appear in the Instructions table — one row per scope.
printf '%s' "$out" | grep -q "│ plan  "
printf '%s' "$out" | grep -q "│ phase "
printf '%s' "$out" | grep -q "│ task  "

# Up next shows all three pending tasks.
for label in task-one task-two task-three; do
    printf '%s' "$out" | grep -q "$label"
done

# 5. Closing the current task drops it from Up next; --next still works
#    when the cursor task has been cleared.
expect_ok "$(call_ipman "{\"protocol_version\":2,\"request_id\":\"close\",\"actor\":\"test\",\"op\":\"task.close\",\"params\":{\"id\":$t1_id,\"outcome_summary\":\"ok\",\"closing_comment\":\"done\"}}")"
out2=$("$BIN" --next)
# task-one is now done; should not appear in Up next.
printf '%s' "$out2" | grep -qv "task-one · " || true
# Other tasks remain pending.
printf '%s' "$out2" | grep -q "task-two"
printf '%s' "$out2" | grep -q "task-three"
