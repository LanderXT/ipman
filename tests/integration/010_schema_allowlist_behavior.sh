#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-schema-behavior.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

call_ipman() {
    printf '%s\n' "$1" | IPMAN_HOME="$TMP/home" "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

expect_unknown_param() {
    response=$1
    param=$2
    printf '%s' "$response" | jq -e --arg param "$param" '
      .ok == false
      and .error.code == "validation_failed"
      and .error.details.kind == "unknown_parameter"
      and (.error.details.received | index($param) != null)
    ' >/dev/null
}

expect_validation_failed() {
    printf '%s' "$1" | jq -e '
      .ok == false
      and .error.code == "validation_failed"
    ' >/dev/null
}

has_anyof_required() {
    schema=$1
    required_json=$2
    jq -e --argjson req "$required_json" '
      any(.properties.params.allOf[]?.anyOf[]?; .required == $req)
    ' "$schema" >/dev/null
}

has_oneof_required() {
    schema=$1
    required_json=$2
    jq -e --argjson req "$required_json" '
      any(.properties.params.allOf[]?.oneOf[]?; .required == $req)
    ' "$schema" >/dev/null
}

IPMAN_HOME="$TMP/home" "$BIN" init >/dev/null 2>/dev/null

task_get_schema="$TMP/home/schemas/ipman.op.task.get.request.schema.json"
task_cancel_schema="$TMP/home/schemas/ipman.op.task.cancel.request.schema.json"
phase_list_tasks_schema="$TMP/home/schemas/ipman.op.phase.list_tasks.request.schema.json"
plan_activate_schema="$TMP/home/schemas/ipman.op.plan.activate.request.schema.json"
plan_list_schema="$TMP/home/schemas/ipman.op.plan.list.request.schema.json"
comment_update_schema="$TMP/home/schemas/ipman.op.comment.update.request.schema.json"
comment_invalidate_schema="$TMP/home/schemas/ipman.op.comment.invalidate.request.schema.json"
plan_comment_add_schema="$TMP/home/schemas/ipman.op.plan.comment_add.request.schema.json"

has_anyof_required "$task_get_schema" '["uid"]'
has_anyof_required "$task_get_schema" '["id"]'
has_anyof_required "$task_get_schema" '["label","plan_uid"]'
has_anyof_required "$task_get_schema" '["label","plan_label"]'
has_anyof_required "$phase_list_tasks_schema" '["uid"]'
has_anyof_required "$phase_list_tasks_schema" '["label","plan_uid"]'
has_anyof_required "$phase_list_tasks_schema" '["label","plan_label"]'
has_anyof_required "$phase_list_tasks_schema" '["phase_id"]'
has_anyof_required "$task_cancel_schema" '["closing_comment"]'
has_anyof_required "$task_cancel_schema" '["comment"]'
has_oneof_required "$plan_activate_schema" '["id"]'
has_oneof_required "$plan_activate_schema" '["code"]'

jq -e '(.properties.params.required // []) | index("id") | not' "$task_get_schema" >/dev/null
jq -e '.properties.params.properties | has("uid") | not' "$plan_list_schema" >/dev/null
jq -e '.properties.params.properties | has("uid") | not' "$comment_update_schema" >/dev/null
jq -e '.properties.params.properties | has("label") | not' "$comment_invalidate_schema" >/dev/null
jq -e '.properties.params.properties | has("uid") | not' "$plan_comment_add_schema" >/dev/null

plan=$(call_ipman '{"protocol_version":1,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"TST-001","title":"Schema Behavior"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')
plan_uid=$(printf '%s' "$plan" | jq -r '.result.plan.uid')
plan_label=$(printf '%s' "$plan" | jq -r '.result.plan.label')

phase=$(call_ipman "{\"protocol_version\":1,\"request_id\":\"phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Implementation\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')
phase_uid=$(printf '%s' "$phase" | jq -r '.result.phase.uid')
phase_label=$(printf '%s' "$phase" | jq -r '.result.phase.label')

other_phase=$(call_ipman "{\"protocol_version\":1,\"request_id\":\"other-phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Verification\"}}")
expect_ok "$other_phase"
other_phase_uid=$(printf '%s' "$other_phase" | jq -r '.result.phase.uid')

task=$(call_ipman "{\"protocol_version\":1,\"request_id\":\"task\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Write tests\"}}")
expect_ok "$task"
task_uid=$(printf '%s' "$task" | jq -r '.result.task.uid')
task_label=$(printf '%s' "$task" | jq -r '.result.task.label')

expect_ok "$(call_ipman "{\"protocol_version\":1,\"request_id\":\"get-uid\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"uid\":\"$task_uid\"}}")"
expect_ok "$(call_ipman "{\"protocol_version\":1,\"request_id\":\"get-label\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"label\":\"$task_label\",\"plan_uid\":\"$plan_uid\"}}")"
expect_ok "$(call_ipman "{\"protocol_version\":1,\"request_id\":\"list-phase-id\",\"actor\":\"test\",\"op\":\"phase.list_tasks\",\"params\":{\"phase_id\":$phase_id}}")"
expect_ok "$(call_ipman "{\"protocol_version\":1,\"request_id\":\"list-phase-uid\",\"actor\":\"test\",\"op\":\"phase.list_tasks\",\"params\":{\"uid\":\"$phase_uid\"}}")"
expect_ok "$(call_ipman "{\"protocol_version\":1,\"request_id\":\"list-phase-label\",\"actor\":\"test\",\"op\":\"phase.list_tasks\",\"params\":{\"label\":\"$phase_label\",\"plan_uid\":\"$plan_uid\"}}")"
expect_ok "$(call_ipman "{\"protocol_version\":1,\"request_id\":\"get-plan-label\",\"actor\":\"test\",\"op\":\"plan.get\",\"params\":{\"label\":\"$plan_label\"}}")"

activate_both=$(call_ipman "{\"protocol_version\":1,\"request_id\":\"activate-both\",\"actor\":\"test\",\"op\":\"plan.activate\",\"params\":{\"id\":$plan_id,\"code\":\"TST-001\"}}")
expect_validation_failed "$activate_both"

phase_selector_mismatch=$(call_ipman "{\"protocol_version\":1,\"request_id\":\"phase-selector-mismatch\",\"actor\":\"test\",\"op\":\"phase.list_tasks\",\"params\":{\"phase_id\":$phase_id,\"uid\":\"$other_phase_uid\"}}")
expect_validation_failed "$phase_selector_mismatch"

expect_unknown_param "$(call_ipman '{"protocol_version":1,"request_id":"bad-plan-list","actor":"test","op":"plan.list","params":{"uid":"plan_1"}}')" "uid"
expect_unknown_param "$(call_ipman '{"protocol_version":1,"request_id":"bad-comment-update","actor":"test","op":"comment.update","params":{"uid":"comment_1","body":"x"}}')" "uid"
expect_unknown_param "$(call_ipman '{"protocol_version":1,"request_id":"bad-comment-invalidate","actor":"test","op":"comment.invalidate","params":{"label":"old-note"}}')" "label"
expect_unknown_param "$(call_ipman '{"protocol_version":1,"request_id":"bad-plan-comment","actor":"test","op":"plan.comment_add","params":{"uid":"plan_1","body":"x"}}')" "uid"
