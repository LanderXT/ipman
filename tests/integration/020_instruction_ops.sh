#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-instructions.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

call_ipman() {
    printf '%s\n' "$1" | IPMAN_HOME="$TMP/home" "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

expect_error_code() {
    response=$1
    code=$2
    printf '%s' "$response" | jq -e --arg code "$code" '
      .ok == false and .error.code == $code
    ' >/dev/null
}

IPMAN_HOME="$TMP/home" "$BIN" init >/dev/null 2>/dev/null

test -f "$TMP/home/operations/ipman.op.instruction.add.schema.md"
test -f "$TMP/home/schemas/ipman.op.instruction.list.request.schema.json"
jq empty "$TMP/home/schemas/ipman.op.instruction.add.request.schema.json"
jq -e '.properties.params.required == ["entity_type","entity_id","body"]' \
    "$TMP/home/schemas/ipman.op.instruction.add.request.schema.json" >/dev/null

plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"INS-001","title":"Instruction Ops"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Implementation\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')

plan_instruction=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"add-plan-instruction\",\"actor\":\"test\",\"op\":\"instruction.add\",\"params\":{\"entity_type\":\"plan\",\"entity_id\":$plan_id,\"instruction_type\":\"constraint\",\"body\":\"Always preserve existing user data.\"}}")
expect_ok "$plan_instruction"
instruction_id=$(printf '%s' "$plan_instruction" | jq -r '.result.instruction.id')
printf '%s' "$plan_instruction" | jq -e '
  .result.instruction.instruction_type == "constraint"
  and .result.instruction.body == "Always preserve existing user data."
' >/dev/null

phase_instruction=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"add-phase-instruction\",\"actor\":\"test\",\"op\":\"instruction.add\",\"params\":{\"entity_type\":\"phase\",\"entity_id\":$phase_id,\"body\":\"Keep phase work API-compatible.\"}}")
expect_ok "$phase_instruction"

plan_active=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"list-plan-active\",\"actor\":\"test\",\"op\":\"instruction.list\",\"params\":{\"entity_type\":\"plan\",\"entity_id\":$plan_id}}")
expect_ok "$plan_active"
printf '%s' "$plan_active" | jq -e '
  .result.total_count == 1
  and .result.instructions[0].id == '"$instruction_id"'
' >/dev/null

updated=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"update-instruction\",\"actor\":\"test\",\"op\":\"instruction.update\",\"params\":{\"id\":$instruction_id,\"instruction_type\":\"guidance\",\"body\":\"Preserve user data unless explicitly asked otherwise.\"}}")
expect_ok "$updated"
printf '%s' "$updated" | jq -e '
  .result.instruction.instruction_type == "guidance"
  and .result.instruction.body == "Preserve user data unless explicitly asked otherwise."
' >/dev/null

events=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"instruction-events\",\"actor\":\"test\",\"op\":\"event.list\",\"params\":{\"entity_type\":\"plan\",\"entity_id\":$plan_id,\"event_type\":\"instruction_updated\"}}")
expect_ok "$events"
printf '%s' "$events" | jq -e '.result.total_count == 1' >/dev/null

invalidated=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"invalidate-instruction\",\"actor\":\"test\",\"op\":\"instruction.invalidate\",\"params\":{\"id\":$instruction_id}}")
expect_ok "$invalidated"
printf '%s' "$invalidated" | jq -e '.result.instruction.invalidated_by == "test"' >/dev/null

plan_active_after=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"list-plan-active-after\",\"actor\":\"test\",\"op\":\"instruction.list\",\"params\":{\"entity_type\":\"plan\",\"entity_id\":$plan_id}}")
expect_ok "$plan_active_after"
printf '%s' "$plan_active_after" | jq -e '.result.total_count == 0' >/dev/null

plan_all=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"list-plan-all\",\"actor\":\"test\",\"op\":\"instruction.list\",\"params\":{\"entity_type\":\"plan\",\"entity_id\":$plan_id,\"include_invalidated\":true}}")
expect_ok "$plan_all"
printf '%s' "$plan_all" | jq -e '
  .result.total_count == 1
  and .result.instructions[0].invalidated_by == "test"
' >/dev/null

expect_error_code "$(call_ipman "{\"protocol_version\":2,\"request_id\":\"invalidate-again\",\"actor\":\"test\",\"op\":\"instruction.invalidate\",\"params\":{\"id\":$instruction_id}}")" "conflict"
expect_error_code "$(call_ipman '{"protocol_version":2,"request_id":"bad-entity","actor":"test","op":"instruction.add","params":{"entity_type":"plan","entity_id":999,"body":"x"}}')" "not_found"

exported=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"export\",\"actor\":\"test\",\"op\":\"plan.export\",\"params\":{\"plan_id\":$plan_id}}")
expect_ok "$exported"
printf '%s' "$exported" | jq -e '
  .result.export.export_format_version == 3
  and (.result.export.instructions | length) == 2
  and any(.result.export.instructions[]; .entity_type == "phase")
' >/dev/null
