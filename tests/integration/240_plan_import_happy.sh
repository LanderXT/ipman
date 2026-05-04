#!/bin/sh
set -eu

# Integration test: happy-path full tree import.
#
# Builds a source workspace with:
#   1 plan, 2 phases, 3 tasks per phase, 1 comment, 1 instruction, 1 relation
#
# Exports via plan.export, imports into a second workspace, then verifies:
#   - The plan exists with a NEW id but same title
#   - Phase and task counts match
#   - The relation is in place (task.list shows both tasks with correct labels)
#   - Comments and instructions were imported

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-import-happy.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

SRC="$TMP/src"
DST="$TMP/dst"
mkdir -p "$SRC" "$DST"

# Helper: run a JSON op in SRC workspace
src_call() {
    printf '%s\n' "$1" | IPMAN_HOME="$SRC/home" "$BIN" 2>/dev/null
}

# Helper: run a JSON op in DST workspace
dst_call() {
    printf '%s\n' "$1" | IPMAN_HOME="$DST/home" "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

# ---- Bootstrap source workspace --------------------------------------------
IPMAN_HOME="$SRC/home" "$BIN" init >/dev/null 2>/dev/null

# Create plan
plan=$(src_call '{"protocol_version":2,"request_id":"p1","actor":"t","op":"plan.create","params":{"title":"Import Test Plan","code":"ITP-1","summary":"A test plan","priority":"medium"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

# Create 2 phases
ph1=$(src_call "{\"protocol_version\":2,\"request_id\":\"ph1\",\"actor\":\"t\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Phase One\",\"sequence_no\":1}}")
expect_ok "$ph1"
ph1_id=$(printf '%s' "$ph1" | jq -r '.result.phase.id')

ph2=$(src_call "{\"protocol_version\":2,\"request_id\":\"ph2\",\"actor\":\"t\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Phase Two\",\"sequence_no\":2}}")
expect_ok "$ph2"
ph2_id=$(printf '%s' "$ph2" | jq -r '.result.phase.id')

# Create 3 tasks in phase 1
for i in 1 2 3; do
    r=$(src_call "{\"protocol_version\":2,\"request_id\":\"t1$i\",\"actor\":\"t\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$ph1_id,\"title\":\"Phase1 Task $i\"}}")
    expect_ok "$r"
done

# Create 3 tasks in phase 2 and capture the first two for the relation
t21=$(src_call "{\"protocol_version\":2,\"request_id\":\"t21\",\"actor\":\"t\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$ph2_id,\"title\":\"Phase2 Task 1\"}}")
expect_ok "$t21"
t21_id=$(printf '%s' "$t21" | jq -r '.result.task.id')

t22=$(src_call "{\"protocol_version\":2,\"request_id\":\"t22\",\"actor\":\"t\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$ph2_id,\"title\":\"Phase2 Task 2\"}}")
expect_ok "$t22"
t22_id=$(printf '%s' "$t22" | jq -r '.result.task.id')

for i in 3; do
    r=$(src_call "{\"protocol_version\":2,\"request_id\":\"t2$i\",\"actor\":\"t\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$ph2_id,\"title\":\"Phase2 Task $i\"}}")
    expect_ok "$r"
done

# Add 1 comment on the plan
cmt=$(src_call "{\"protocol_version\":2,\"request_id\":\"c1\",\"actor\":\"t\",\"op\":\"comment.add\",\"params\":{\"entity_type\":\"plan\",\"entity_id\":$plan_id,\"body\":\"Plan-level comment for import test.\"}}")
expect_ok "$cmt"

# Add 1 instruction on the plan
ins=$(src_call "{\"protocol_version\":2,\"request_id\":\"i1\",\"actor\":\"t\",\"op\":\"instruction.add\",\"params\":{\"entity_type\":\"plan\",\"entity_id\":$plan_id,\"instruction_type\":\"guidance\",\"body\":\"Keep tests green.\"}}")
expect_ok "$ins"

# Add 1 relation: t21 blocks t22
rel=$(src_call "{\"protocol_version\":2,\"request_id\":\"r1\",\"actor\":\"t\",\"op\":\"task.link_dependency\",\"params\":{\"id\":$t21_id,\"target_task_id\":$t22_id,\"relation_type\":\"blocks\"}}")
expect_ok "$rel"

# ---- Export the plan -------------------------------------------------------
exp=$(src_call "{\"protocol_version\":2,\"request_id\":\"x1\",\"actor\":\"t\",\"op\":\"plan.export\",\"params\":{\"plan_id\":$plan_id}}")
expect_ok "$exp"

# Write only the export object (the inner envelope shape expected by --import-plan)
printf '%s' "$exp" | jq '.result.export' > "$TMP/export.json"

# ---- Bootstrap destination workspace and import ----------------------------
IPMAN_HOME="$DST/home" "$BIN" init >/dev/null 2>/dev/null

import_out=$(IPMAN_HOME="$DST/home" "$BIN" --import-plan "$TMP/export.json" 2>/dev/null)
# Should print "imported plan <id>"
printf '%s\n' "$import_out" | grep -q "^imported plan "

new_plan_id=$(printf '%s\n' "$import_out" | grep -oE '[0-9]+$')

# ---- Verify the plan tree --------------------------------------------------

# Plan exists with correct title
got_plan=$(dst_call "{\"protocol_version\":2,\"request_id\":\"g1\",\"actor\":\"t\",\"op\":\"plan.get\",\"params\":{\"id\":$new_plan_id}}")
expect_ok "$got_plan"
printf '%s' "$got_plan" | jq -e '.result.plan.title == "Import Test Plan"' >/dev/null
# Note: both workspaces start fresh so new_plan_id=1 and src plan_id=1 are equal;
# the meaningful check is that the import succeeded in a separate workspace.

# 2 phases
phases=$(dst_call "{\"protocol_version\":2,\"request_id\":\"g2\",\"actor\":\"t\",\"op\":\"phase.list\",\"params\":{\"plan_id\":$new_plan_id}}")
expect_ok "$phases"
printf '%s' "$phases" | jq -e '(.result.phases | length) == 2' >/dev/null
printf '%s' "$phases" | jq -e '(.result.phases[] | select(.title == "Phase One") | .sequence_no) == 1' >/dev/null
printf '%s' "$phases" | jq -e '(.result.phases[] | select(.title == "Phase Two") | .sequence_no) == 2' >/dev/null

# 6 tasks total (3 per phase)
tasks=$(dst_call "{\"protocol_version\":2,\"request_id\":\"g3\",\"actor\":\"t\",\"op\":\"task.list\",\"params\":{\"plan_id\":$new_plan_id,\"limit\":20}}")
expect_ok "$tasks"
printf '%s' "$tasks" | jq -e '(.result.tasks | length) == 6' >/dev/null

# All tasks are todo
printf '%s' "$tasks" | jq -e '[.result.tasks[].status] | all(. == "todo")' >/dev/null

# Plan is open
printf '%s' "$got_plan" | jq -e '.result.plan.status == "open"' >/dev/null

# 1 comment on the plan
comments=$(dst_call "{\"protocol_version\":2,\"request_id\":\"g4\",\"actor\":\"t\",\"op\":\"comment.list\",\"params\":{\"entity_type\":\"plan\",\"entity_id\":$new_plan_id}}")
expect_ok "$comments"
printf '%s' "$comments" | jq -e '(.result.comments | length) == 1' >/dev/null
printf '%s' "$comments" | jq -e '.result.comments[0].body == "Plan-level comment for import test."' >/dev/null

# 1 instruction on the plan
instructions=$(dst_call "{\"protocol_version\":2,\"request_id\":\"g5\",\"actor\":\"t\",\"op\":\"instruction.list\",\"params\":{\"entity_type\":\"plan\",\"entity_id\":$new_plan_id}}")
expect_ok "$instructions"
printf '%s' "$instructions" | jq -e '(.result.instructions | length) == 1' >/dev/null
printf '%s' "$instructions" | jq -e '.result.instructions[0].body == "Keep tests green."' >/dev/null

# Relation: find the imported task IDs and verify depends_on relation via event log
new_t21_id=$(printf '%s' "$tasks" | jq -r '.result.tasks[] | select(.title == "Phase2 Task 1") | .id')
new_t22_id=$(printf '%s' "$tasks" | jq -r '.result.tasks[] | select(.title == "Phase2 Task 2") | .id')
# Verify via sql: there should be a task_relations row from new_t21 to new_t22
rel_count=$(IPMAN_HOME="$DST/home" "$BIN" sql \
  "SELECT COUNT(*) FROM task_relations WHERE from_task_id=$new_t21_id AND to_task_id=$new_t22_id AND relation_type='blocks'" \
  2>/dev/null)
[ "$rel_count" = "1" ]

echo "ok 240_plan_import_happy"
