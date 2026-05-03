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

has_no_anyof() {
    schema=$1
    jq -e '(.properties.params.allOf // []) | all(has("anyOf") | not)' \
        "$schema" >/dev/null
}

IPMAN_HOME="$TMP/home" "$BIN" init >/dev/null 2>/dev/null

task_get_schema="$TMP/home/schemas/ipman.op.task.get.request.schema.json"
task_lookup_schema="$TMP/home/schemas/ipman.op.task.lookup.request.schema.json"
task_cancel_schema="$TMP/home/schemas/ipman.op.task.cancel.request.schema.json"
phase_list_tasks_schema="$TMP/home/schemas/ipman.op.phase.list_tasks.request.schema.json"
plan_activate_schema="$TMP/home/schemas/ipman.op.plan.activate.request.schema.json"
plan_lookup_schema="$TMP/home/schemas/ipman.op.plan.lookup.request.schema.json"
plan_list_schema="$TMP/home/schemas/ipman.op.plan.list.request.schema.json"
plan_get_schema="$TMP/home/schemas/ipman.op.plan.get.request.schema.json"
comment_update_schema="$TMP/home/schemas/ipman.op.comment.update.request.schema.json"
comment_invalidate_schema="$TMP/home/schemas/ipman.op.comment.invalidate.request.schema.json"
plan_comment_add_schema="$TMP/home/schemas/ipman.op.plan.comment_add.request.schema.json"

# v2 schema shape: non-lookup selectors require id and reject uid/label/code.
has_anyof_required "$task_get_schema" '["id"]'
has_anyof_required "$plan_get_schema" '["id"]'
jq -e '.properties.params.properties | has("uid") | not' "$task_get_schema" >/dev/null
jq -e '.properties.params.properties | has("label") | not' "$task_get_schema" >/dev/null
jq -e '.properties.params.properties | has("code") | not' "$plan_get_schema" >/dev/null
jq -e '.properties.params.properties | has("uid") | not' "$plan_get_schema" >/dev/null

# phase.list_tasks keeps the id-or-phase_id alias as anyOf.
has_anyof_required "$phase_list_tasks_schema" '["id"]'
has_anyof_required "$phase_list_tasks_schema" '["phase_id"]'
jq -e '.properties.params.properties | has("uid") | not' "$phase_list_tasks_schema" >/dev/null
jq -e '.properties.params.properties | has("label") | not' "$phase_list_tasks_schema" >/dev/null

# Lookup ops keep their v2-correct selector polymorphism.
has_anyof_required "$plan_lookup_schema" '["uid"]'
has_anyof_required "$plan_lookup_schema" '["label"]'
has_anyof_required "$plan_lookup_schema" '["code"]'
jq -e '.properties.params.properties | has("id") | not' "$plan_lookup_schema" >/dev/null

has_anyof_required "$task_lookup_schema" '["uid"]'
has_anyof_required "$task_lookup_schema" '["label","plan_id"]'
jq -e '.properties.params.properties | has("plan_uid") | not' "$task_lookup_schema" >/dev/null
jq -e '.properties.params.properties | has("plan_label") | not' "$task_lookup_schema" >/dev/null

# task.cancel keeps the closing_comment-or-comment closure-text alternation.
has_anyof_required "$task_cancel_schema" '["closing_comment"]'
has_anyof_required "$task_cancel_schema" '["comment"]'

# plan.activate keeps its id-XOR-code oneOf.
has_oneof_required "$plan_activate_schema" '["id"]'
has_oneof_required "$plan_activate_schema" '["code"]'

# Required at top-level has params declared, but params.required must not list id
# (id appears via allOf/anyOf so selector alternatives stay consistently shaped).
jq -e '(.properties.params.required // []) | index("id") | not' "$task_get_schema" >/dev/null

# Selector params trimmed where they used to leak through.
jq -e '.properties.params.properties | has("uid") | not' "$plan_list_schema" >/dev/null
jq -e '.properties.params.properties | has("uid") | not' "$comment_update_schema" >/dev/null
jq -e '.properties.params.properties | has("label") | not' "$comment_invalidate_schema" >/dev/null
jq -e '.properties.params.properties | has("uid") | not' "$plan_comment_add_schema" >/dev/null

# ----- Behavioral tests against the binary -----

plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"TST-001","title":"Schema Behavior"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Implementation\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')
phase_label=$(printf '%s' "$phase" | jq -r '.result.phase.label')

other_phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"other-phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Verification\"}}")
expect_ok "$other_phase"
other_phase_id=$(printf '%s' "$other_phase" | jq -r '.result.phase.id')

task=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"task\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Write tests\"}}")
expect_ok "$task"
task_id=$(printf '%s' "$task" | jq -r '.result.task.id')

# Verify v2 output shape: uid and code absent on plan response; label still present.
printf '%s' "$plan" | jq -e '.result.plan | has("uid") | not' >/dev/null
printf '%s' "$plan" | jq -e '.result.plan | has("code") | not' >/dev/null
printf '%s' "$plan" | jq -e '.result.plan | has("label")' >/dev/null
printf '%s' "$task" | jq -e '.result.task | has("uid") | not' >/dev/null

# id selectors work on non-lookup ops.
expect_ok "$(call_ipman "{\"protocol_version\":2,\"request_id\":\"get-task-id\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"id\":$task_id}}")"
expect_ok "$(call_ipman "{\"protocol_version\":2,\"request_id\":\"get-plan-id\",\"actor\":\"test\",\"op\":\"plan.get\",\"params\":{\"id\":$plan_id}}")"

# Lookup ops translate handles to id.
plan_lookup_by_label=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"lookup-plan-label\",\"actor\":\"test\",\"op\":\"plan.lookup\",\"params\":{\"label\":\"schema-behavior\"}}")
expect_ok "$plan_lookup_by_label"
printf '%s' "$plan_lookup_by_label" | jq -e --argjson id "$plan_id" '.result == {id:$id}' >/dev/null

task_lookup_by_label=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"lookup-task-label\",\"actor\":\"test\",\"op\":\"task.lookup\",\"params\":{\"label\":\"write-tests\",\"plan_id\":$plan_id}}")
expect_ok "$task_lookup_by_label"
printf '%s' "$task_lookup_by_label" | jq -e --argjson id "$task_id" '.result == {id:$id}' >/dev/null

# phase.list_tasks accepts id or phase_id; mismatching them is rejected.
expect_ok "$(call_ipman "{\"protocol_version\":2,\"request_id\":\"list-by-id\",\"actor\":\"test\",\"op\":\"phase.list_tasks\",\"params\":{\"id\":$phase_id}}")"
expect_ok "$(call_ipman "{\"protocol_version\":2,\"request_id\":\"list-by-phase-id\",\"actor\":\"test\",\"op\":\"phase.list_tasks\",\"params\":{\"phase_id\":$phase_id}}")"
phase_list_mismatch=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"list-mismatch\",\"actor\":\"test\",\"op\":\"phase.list_tasks\",\"params\":{\"id\":$phase_id,\"phase_id\":$other_phase_id}}")
expect_validation_failed "$phase_list_mismatch"

# plan.activate's id-XOR-code: passing both is rejected.
activate_both=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"activate-both\",\"actor\":\"test\",\"op\":\"plan.activate\",\"params\":{\"id\":$plan_id,\"code\":\"TST-001\"}}")
expect_validation_failed "$activate_both"

# v1 selectors are now unknown_parameter on v2 binary.
expect_unknown_param "$(call_ipman "{\"protocol_version\":2,\"request_id\":\"task-by-uid\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"uid\":\"task_1\"}}")" "uid"
expect_unknown_param "$(call_ipman "{\"protocol_version\":2,\"request_id\":\"task-by-label\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"label\":\"$phase_label\",\"plan_uid\":\"plan_1\"}}")" "label"
expect_unknown_param "$(call_ipman "{\"protocol_version\":2,\"request_id\":\"task-by-plan-label\",\"actor\":\"test\",\"op\":\"task.get\",\"params\":{\"id\":1,\"plan_label\":\"x\"}}")" "plan_label"
expect_unknown_param '{"ok":false,"error":{"code":"validation_failed","message":"x","details":{"kind":"unknown_parameter","received":["uid"]}}}' "uid"

expect_unknown_param "$(call_ipman '{"protocol_version":2,"request_id":"bad-plan-list","actor":"test","op":"plan.list","params":{"uid":"plan_1"}}')" "uid"
expect_unknown_param "$(call_ipman '{"protocol_version":2,"request_id":"bad-comment-update","actor":"test","op":"comment.update","params":{"uid":"comment_1","body":"x"}}')" "uid"
expect_unknown_param "$(call_ipman '{"protocol_version":2,"request_id":"bad-comment-invalidate","actor":"test","op":"comment.invalidate","params":{"label":"old-note"}}')" "label"
expect_unknown_param "$(call_ipman '{"protocol_version":2,"request_id":"bad-plan-comment","actor":"test","op":"plan.comment_add","params":{"uid":"plan_1","body":"x"}}')" "uid"

# v1 envelopes are rejected with the migration message. The binary's exit
# code is 1 for invalid_request (protocol-level fatal), so allow the call
# to fail without tripping set -e.
v1_envelope=$(call_ipman '{"protocol_version":1,"request_id":"v1-reject","actor":"test","op":"plan.list","params":{}}' || true)
printf '%s' "$v1_envelope" | jq -e '
  .ok == false
  and .error.code == "invalid_request"
  and (.error.message | test("protocol_version must be 2"))
' >/dev/null
