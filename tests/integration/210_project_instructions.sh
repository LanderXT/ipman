#!/bin/sh
set -eu

# Integration test for v2.3.1 project-scoped instructions.
# Covers:
#   * instruction.add with entity_type='project' (and entity_id=1)
#   * entity_id != 1 rejected for project scope
#   * instruction.list at project scope
#   * instruction.update / invalidate work transparently for project rows
#   * workspace.context_get exposes context.project.instructions[]
#   * project.history captures instruction events on the project

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-project-instr.XXXXXX")
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

# --- instruction.add at project scope --------------------------------------

apache=$(call_ipman '{"protocol_version":2,"request_id":"a1","actor":"hleal","op":"instruction.add","params":{"entity_type":"project","entity_id":1,"instruction_type":"constraint","body":"Apache-2.0 in all source headers."}}')
expect_ok "$apache"
apache_id=$(printf '%s' "$apache" | jq -r '.result.instruction.id')
printf '%s' "$apache" | jq -e '
  .result.instruction.entity_type == "project"
  and .result.instruction.entity_id == 1
  and .result.instruction.instruction_type == "constraint"
' >/dev/null

tests=$(call_ipman '{"protocol_version":2,"request_id":"a2","actor":"hleal","op":"instruction.add","params":{"entity_type":"project","entity_id":1,"body":"Run make test before merging."}}')
expect_ok "$tests"

# entity_id != 1 for project scope is rejected with a precise message.
bad_id=$(call_ipman '{"protocol_version":2,"request_id":"a3","actor":"hleal","op":"instruction.add","params":{"entity_type":"project","entity_id":2,"body":"x"}}')
expect_error_code "$bad_id" "validation_failed"
printf '%s' "$bad_id" | jq -e '.error.message | contains("entity_id must be 1")' >/dev/null

# --- instruction.list at project scope -------------------------------------

list=$(call_ipman '{"protocol_version":2,"request_id":"l1","actor":"test","op":"instruction.list","params":{"entity_type":"project","entity_id":1}}')
expect_ok "$list"
printf '%s' "$list" | jq -e '
  .result.total_count == 2
  and (.result.instructions | map(.body)) == ["Apache-2.0 in all source headers.","Run make test before merging."]
' >/dev/null

# --- instruction.update / invalidate work for project rows -----------------

upd=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"u1\",\"actor\":\"hleal\",\"op\":\"instruction.update\",\"params\":{\"id\":$apache_id,\"body\":\"Apache-2.0 + SPDX-License-Identifier in all source headers.\"}}")
expect_ok "$upd"

inv=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"i1\",\"actor\":\"hleal\",\"op\":\"instruction.invalidate\",\"params\":{\"id\":$apache_id}}")
expect_ok "$inv"

active=$(call_ipman '{"protocol_version":2,"request_id":"l2","actor":"test","op":"instruction.list","params":{"entity_type":"project","entity_id":1}}')
printf '%s' "$active" | jq -e '.result.total_count == 1' >/dev/null

# --- workspace.context_get surfaces project instructions -------------------

ctx=$(call_ipman '{"protocol_version":2,"request_id":"c1","actor":"test","op":"workspace.context_get","params":{}}')
expect_ok "$ctx"
printf '%s' "$ctx" | jq -e '
  (.result.context.project.instructions | length) == 1
  and (.result.context.project.instructions[0].body) == "Run make test before merging."
  and (.result.context.project.instructions[0].entity_type) == "project"
' >/dev/null

# --- project.history captures instruction events on the project ------------

history=$(call_ipman '{"protocol_version":2,"request_id":"h1","actor":"test","op":"project.history","params":{}}')
expect_ok "$history"
printf '%s' "$history" | jq -e '
  ([.result.events[].event_type] | sort | unique) as $types
  | ($types | index("instruction_added")        != null)
  and ($types | index("instruction_updated")    != null)
  and ($types | index("instruction_invalidated") != null)
' >/dev/null

# --- non-project entity_type still rejects entity_id <= 0 ------------------

bad_neg=$(call_ipman '{"protocol_version":2,"request_id":"n1","actor":"test","op":"instruction.add","params":{"entity_type":"plan","entity_id":0,"body":"x"}}')
expect_error_code "$bad_neg" "validation_failed"

echo "ok 210_project_instructions"
