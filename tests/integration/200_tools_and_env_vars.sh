#!/bin/sh
set -eu

# Integration test for v2.3 project entity + tools/env_vars registry.
# Covers: project.get / update / history; tool and env_var CRUD scoped to
# the project (no plan_id); name regex enforcement for env_vars; sensitive-
# example masking with reveal opt-in; conflict on duplicate active names;
# workspace.context_get integration with the new context.project block.

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-project-v23.XXXXXX")
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

# Generated docs and request schemas exist for all 11 new ops.
for op in project.get project.update project.history \
          tool.add tool.list tool.update tool.invalidate \
          env_var.add env_var.list env_var.update env_var.invalidate; do
    test -f "$TMP/home/operations/ipman.op.${op}.schema.md"
    test -f "$TMP/home/schemas/ipman.op.${op}.request.schema.json"
    jq empty "$TMP/home/schemas/ipman.op.${op}.request.schema.json"
done

# --- project.get / update / history ----------------------------------------

# init seeded the row with name='unnamed' and no description.
init_project=$(call_ipman '{"protocol_version":2,"request_id":"p1","actor":"test","op":"project.get","params":{}}')
expect_ok "$init_project"
printf '%s' "$init_project" | jq -e '
  .result.project.id == 1
  and .result.project.name == "unnamed"
  and .result.project.description == null
  and .result.project.updated_at == null
' >/dev/null

# project.update sets name and description, emits project_updated event.
updated=$(call_ipman '{"protocol_version":2,"request_id":"p2","actor":"hleal","op":"project.update","params":{"name":"ipman","description":"Implementation Plan Manager."}}')
expect_ok "$updated"
printf '%s' "$updated" | jq -e '
  .result.project.name == "ipman"
  and .result.project.description == "Implementation Plan Manager."
  and .result.project.updated_at != null
' >/dev/null

# project.update with no fields → validation_failed.
empty_update=$(call_ipman '{"protocol_version":2,"request_id":"p3","actor":"test","op":"project.update","params":{}}')
expect_error_code "$empty_update" "validation_failed"

# project.update with only description (name preserved).
desc_only=$(call_ipman '{"protocol_version":2,"request_id":"p4","actor":"test","op":"project.update","params":{"description":"Auditable plans for agents."}}')
expect_ok "$desc_only"
printf '%s' "$desc_only" | jq -e '
  .result.project.name == "ipman"
  and .result.project.description == "Auditable plans for agents."
' >/dev/null

# --- tool.add (no plan_id required) ----------------------------------------

node_tool=$(call_ipman '{"protocol_version":2,"request_id":"t1","actor":"test","op":"tool.add","params":{"name":"node","version_constraint":">=20","purpose":"runtime","install_hint":"mise use node@20"}}')
expect_ok "$node_tool"
node_tool_id=$(printf '%s' "$node_tool" | jq -r '.result.tool.id')
printf '%s' "$node_tool" | jq -e '
  .result.tool.name == "node"
  and (.result.tool | has("plan_id") | not)
' >/dev/null

jq_tool=$(call_ipman '{"protocol_version":2,"request_id":"t2","actor":"test","op":"tool.add","params":{"name":"jq","required":false,"install_hint":"brew install jq"}}')
expect_ok "$jq_tool"
jq_tool_id=$(printf '%s' "$jq_tool" | jq -r '.result.tool.id')
printf '%s' "$jq_tool" | jq -e '.result.tool.required == false' >/dev/null

# Duplicate active name → conflict.
dup=$(call_ipman '{"protocol_version":2,"request_id":"t3","actor":"test","op":"tool.add","params":{"name":"node"}}')
expect_error_code "$dup" "conflict"

# --- tool.list / update / invalidate ---------------------------------------

tool_list=$(call_ipman '{"protocol_version":2,"request_id":"t4","actor":"test","op":"tool.list","params":{}}')
expect_ok "$tool_list"
printf '%s' "$tool_list" | jq -e '
  .result.total_count == 2
  and (.result.tools | map(.name)) == ["node","jq"]
' >/dev/null

bumped=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"t5\",\"actor\":\"test\",\"op\":\"tool.update\",\"params\":{\"id\":$node_tool_id,\"version_constraint\":\">=22\"}}")
expect_ok "$bumped"
printf '%s' "$bumped" | jq -e '
  .result.tool.version_constraint == ">=22"
  and .result.tool.purpose == "runtime"
' >/dev/null

inv=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"t6\",\"actor\":\"hleal\",\"op\":\"tool.invalidate\",\"params\":{\"id\":$jq_tool_id}}")
expect_ok "$inv"

# --- env_var.add (regex validation) ----------------------------------------

db=$(call_ipman '{"protocol_version":2,"request_id":"e1","actor":"test","op":"env_var.add","params":{"name":"DATABASE_URL","purpose":"primary postgres","example":"postgres://localhost:5432/dev"}}')
expect_ok "$db"
db_id=$(printf '%s' "$db" | jq -r '.result.env_var.id')
printf '%s' "$db" | jq -e '(.result.env_var | has("plan_id") | not)' >/dev/null

stripe=$(call_ipman '{"protocol_version":2,"request_id":"e2","actor":"test","op":"env_var.add","params":{"name":"STRIPE_SECRET","purpose":"payments webhook","example":"sk_live_xxxxx","sensitive":true}}')
expect_ok "$stripe"

oauth=$(call_ipman '{"protocol_version":2,"request_id":"e3","actor":"test","op":"env_var.add","params":{"name":"OAUTH2_CLIENT_ID","purpose":"google sso"}}')
expect_ok "$oauth"

# Lowercase rejected.
lower=$(call_ipman '{"protocol_version":2,"request_id":"e4","actor":"test","op":"env_var.add","params":{"name":"db_url","purpose":"x"}}')
expect_error_code "$lower" "validation_failed"

# Leading digit rejected.
digit=$(call_ipman '{"protocol_version":2,"request_id":"e5","actor":"test","op":"env_var.add","params":{"name":"2BAD","purpose":"x"}}')
expect_error_code "$digit" "validation_failed"

# Empty purpose rejected (required).
no_purpose=$(call_ipman '{"protocol_version":2,"request_id":"e6","actor":"test","op":"env_var.add","params":{"name":"PORT"}}')
expect_error_code "$no_purpose" "validation_failed"

# --- env_var.list with masking ---------------------------------------------

masked=$(call_ipman '{"protocol_version":2,"request_id":"e7","actor":"test","op":"env_var.list","params":{}}')
expect_ok "$masked"
printf '%s' "$masked" | jq -e '
  .result.reveal == false
  and .result.total_count == 3
  and (.result.env_vars[] | select(.name == "STRIPE_SECRET").example) == "[sensitive]"
  and (.result.env_vars[] | select(.name == "DATABASE_URL").example) == "postgres://localhost:5432/dev"
' >/dev/null

revealed=$(call_ipman '{"protocol_version":2,"request_id":"e8","actor":"test","op":"env_var.list","params":{"reveal":true}}')
expect_ok "$revealed"
printf '%s' "$revealed" | jq -e '
  .result.reveal == true
  and (.result.env_vars[] | select(.name == "STRIPE_SECRET").example) == "sk_live_xxxxx"
' >/dev/null

# --- env_var.update + invalidate -------------------------------------------

flipped=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"e9\",\"actor\":\"test\",\"op\":\"env_var.update\",\"params\":{\"id\":$db_id,\"sensitive\":true}}")
expect_ok "$flipped"

remasked=$(call_ipman '{"protocol_version":2,"request_id":"e10","actor":"test","op":"env_var.list","params":{}}')
printf '%s' "$remasked" | jq -e '
  (.result.env_vars[] | select(.name == "DATABASE_URL").example) == "[sensitive]"
' >/dev/null

env_inv=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"e11\",\"actor\":\"hleal\",\"op\":\"env_var.invalidate\",\"params\":{\"id\":$db_id}}")
expect_ok "$env_inv"

# --- workspace.context_get with project block ------------------------------

# Create + activate a plan so context has the cursor portion populated.
plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"V23-001","title":"v2.3 demo"}}')
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')
call_ipman "{\"protocol_version\":2,\"request_id\":\"act\",\"actor\":\"test\",\"op\":\"plan.activate\",\"params\":{\"id\":$plan_id}}" >/dev/null

ctx=$(call_ipman '{"protocol_version":2,"request_id":"ctx1","actor":"test","op":"workspace.context_get","params":{}}')
expect_ok "$ctx"
printf '%s' "$ctx" | jq -e '
  .result.context.project.name == "ipman"
  and .result.context.project.description == "Auditable plans for agents."
  and (.result.context.project.tools | length) == 1
  and (.result.context.project.tools[0].name) == "node"
  and (.result.context.project.env_vars | length) == 2
  and ((.result.context.project.env_vars[] | select(.name == "STRIPE_SECRET").example) == "[sensitive]")
' >/dev/null

ctx_revealed=$(call_ipman '{"protocol_version":2,"request_id":"ctx2","actor":"test","op":"workspace.context_get","params":{"reveal":true}}')
expect_ok "$ctx_revealed"
printf '%s' "$ctx_revealed" | jq -e '
  (.result.context.project.env_vars[] | select(.name == "STRIPE_SECRET").example) == "sk_live_xxxxx"
' >/dev/null

# --- project.history -------------------------------------------------------

history=$(call_ipman '{"protocol_version":2,"request_id":"h1","actor":"test","op":"project.history","params":{"limit":50}}')
expect_ok "$history"
printf '%s' "$history" | jq -e '
  ([.result.events[].event_type] | sort | unique) as $types
  | ($types | index("project_updated") != null)
  and ($types | index("tool_added")        != null)
  and ($types | index("tool_updated")      != null)
  and ($types | index("tool_invalidated")  != null)
  and ($types | index("env_var_added")     != null)
  and ($types | index("env_var_updated")   != null)
  and ($types | index("env_var_invalidated") != null)
  and ($types | index("plan_created")  != null)
  and ($types | index("plan_activated") != null)
' >/dev/null

# project.history excludes phase/task/comment events. Create some to verify.
phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"phase\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"impl\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')

history_after=$(call_ipman '{"protocol_version":2,"request_id":"h2","actor":"test","op":"project.history","params":{}}')
printf '%s' "$history_after" | jq -e '
  ([.result.events[].event_type] | sort | unique) as $types
  | ($types | index("phase_created") == null)
' >/dev/null

# --- limit / offset --------------------------------------------------------

paginated=$(call_ipman '{"protocol_version":2,"request_id":"h3","actor":"test","op":"project.history","params":{"limit":1}}')
printf '%s' "$paginated" | jq -e '
  .result.limit == 1
  and (.result.events | length) == 1
  and .result.has_more == true
' >/dev/null

bad_limit=$(call_ipman '{"protocol_version":2,"request_id":"h4","actor":"test","op":"project.history","params":{"limit":600}}')
expect_error_code "$bad_limit" "validation_failed"

echo "ok 200_tools_and_env_vars"
