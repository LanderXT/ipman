#!/bin/sh
set -eu

# Integration test for v2.3.2 workspace.export.
# Covers:
#   * Empty workspace: project (auto-named) + empty tools/env_vars/instructions/events.
#   * Populated workspace: project + tools + env_vars + project-instructions + project-events.
#   * env_var examples are revealed (export = backup; caller owns confidentiality).
#   * Invalidated rows are included.

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-workspace-export.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

call_ipman() {
    printf '%s\n' "$1" | IPMAN_HOME="$TMP/home" "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

IPMAN_HOME="$TMP/home" "$BIN" init >/dev/null 2>/dev/null

# Generated docs and schema for the new op exist.
test -f "$TMP/home/operations/ipman.op.workspace.export.schema.md"
test -f "$TMP/home/schemas/ipman.op.workspace.export.request.schema.json"
jq empty "$TMP/home/schemas/ipman.op.workspace.export.request.schema.json"

# --- Empty workspace ------------------------------------------------------

empty=$(call_ipman '{"protocol_version":2,"request_id":"x1","actor":"test","op":"workspace.export","params":{}}')
expect_ok "$empty"
printf '%s' "$empty" | jq -e '
  .result.export.export_format_version == 1
  and (.result.export.schema_version | type) == "number"
  and (.result.export.generated_at | test("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z$"))
  and .result.export.project.id == 1
  and (.result.export.project.name | type) == "string"
  and (.result.export.tools        | length) == 0
  and (.result.export.env_vars     | length) == 0
  and (.result.export.instructions | length) == 0
  and (.result.export.events       | length) == 0
' >/dev/null

# --- Populate, invalidate one of each, then export ------------------------

call_ipman '{"protocol_version":2,"request_id":"u1","actor":"hleal","op":"project.update","params":{"name":"my-app","description":"Test fixture for v2.3.2 workspace.export."}}' >/dev/null

t1=$(call_ipman '{"protocol_version":2,"request_id":"t1","actor":"hleal","op":"tool.add","params":{"name":"node","version_constraint":">=20","purpose":"runtime"}}')
t1_id=$(printf '%s' "$t1" | jq -r '.result.tool.id')

t2=$(call_ipman '{"protocol_version":2,"request_id":"t2","actor":"hleal","op":"tool.add","params":{"name":"jq","required":false}}')
t2_id=$(printf '%s' "$t2" | jq -r '.result.tool.id')

call_ipman "{\"protocol_version\":2,\"request_id\":\"t3\",\"actor\":\"hleal\",\"op\":\"tool.invalidate\",\"params\":{\"id\":$t2_id}}" >/dev/null

call_ipman '{"protocol_version":2,"request_id":"e1","actor":"hleal","op":"env_var.add","params":{"name":"DATABASE_URL","purpose":"primary db","example":"postgres://localhost/dev"}}' >/dev/null
call_ipman '{"protocol_version":2,"request_id":"e2","actor":"hleal","op":"env_var.add","params":{"name":"STRIPE_SECRET","purpose":"webhook","example":"sk_test_xxxxx","sensitive":true}}' >/dev/null

call_ipman '{"protocol_version":2,"request_id":"i1","actor":"hleal","op":"instruction.add","params":{"entity_type":"project","entity_id":1,"instruction_type":"constraint","body":"Apache-2.0 in source headers."}}' >/dev/null
call_ipman '{"protocol_version":2,"request_id":"i2","actor":"hleal","op":"instruction.add","params":{"entity_type":"project","entity_id":1,"body":"Run make test before merging."}}' >/dev/null

# Export.
exp=$(call_ipman '{"protocol_version":2,"request_id":"x2","actor":"test","op":"workspace.export","params":{}}')
expect_ok "$exp"

# Project block populated correctly.
printf '%s' "$exp" | jq -e '
  .result.export.project.name == "my-app"
  and .result.export.project.description == "Test fixture for v2.3.2 workspace.export."
' >/dev/null

# Tools include both active and invalidated rows.
printf '%s' "$exp" | jq -e --argjson t2 "$t2_id" '
  (.result.export.tools | length) == 2
  and (.result.export.tools[] | select(.name == "node")        | .invalidated_at == null)
  and (.result.export.tools[] | select(.id == $t2)             | .invalidated_at != null)
' >/dev/null

# Env vars: examples revealed even when sensitive=true. There is no [sensitive]
# placeholder in a backup — the caller owns the bytes.
printf '%s' "$exp" | jq -e '
  (.result.export.env_vars | length) == 2
  and (.result.export.env_vars[] | select(.name == "DATABASE_URL").example) == "postgres://localhost/dev"
  and (.result.export.env_vars[] | select(.name == "STRIPE_SECRET").example) == "sk_test_xxxxx"
  and (.result.export.env_vars[] | select(.name == "STRIPE_SECRET").sensitive) == true
' >/dev/null

# Project-scoped instructions: both active.
printf '%s' "$exp" | jq -e '
  (.result.export.instructions | length) == 2
  and ((.result.export.instructions | map(.entity_type) | unique) == ["project"])
' >/dev/null

# Events: project_updated + tool_added×2 + tool_invalidated + env_var_added×2 +
# instruction_added×2 = 8 entries on entity_type='project'.
printf '%s' "$exp" | jq -e '
  ((.result.export.events | map(.entity_type) | unique) == ["project"])
  and ([.result.export.events[].event_type] | sort | unique) as $t
  | ($t | index("project_updated")     != null)
  and ($t | index("tool_added")            != null)
  and ($t | index("tool_invalidated")      != null)
  and ($t | index("env_var_added")         != null)
  and ($t | index("instruction_added")     != null)
' >/dev/null

# plan.export still works and stays plan-scoped (does not contaminate with
# workspace data).
plan=$(call_ipman '{"protocol_version":2,"request_id":"p1","actor":"hleal","op":"plan.create","params":{"code":"WX-001","title":"export check"}}')
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')
plan_exp=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"px\",\"actor\":\"test\",\"op\":\"plan.export\",\"params\":{\"plan_id\":$plan_id}}")
expect_ok "$plan_exp"
printf '%s' "$plan_exp" | jq -e '
  (.result.export | has("tools") | not)
  and (.result.export | has("env_vars") | not)
  and (.result.export | has("project") | not)
' >/dev/null

echo "ok 220_workspace_export"
