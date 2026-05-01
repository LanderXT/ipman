#!/bin/sh
# Regression: plan.export must preserve every v2.1 closure-evidence field
# (commit_sha, dirty, files_changed, validations_run, decisions) and use the
# same JSON shape as closure.get. Closure-record migrations 0002 and 0003
# added these columns; the export SELECT must include them so the snapshot
# is the durable audit-trail it claims to be.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-plan-export-evidence.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export IPMAN_HOME="$TMP/home"

call_ipman() {
    printf '%s\n' "$1" | "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

"$BIN" init >/dev/null 2>/dev/null

plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"title":"Export Evidence","label":"export-evidence"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"ph\",\"actor\":\"test\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Implementation\"}}")
expect_ok "$phase"
phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')

task=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"tk\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Round-trip evidence\"}}")
expect_ok "$task"
task_id=$(printf '%s' "$task" | jq -r '.result.task.id')

call_ipman "{\"protocol_version\":2,\"request_id\":\"st\",\"actor\":\"test\",\"op\":\"task.transition\",\"params\":{\"id\":$task_id,\"status\":\"in_progress\"}}" >/dev/null

# Close the task with the full v2.1 evidence shape via the JSON wire so the
# stored row contains every field this regression covers.
close=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"cl\",\"actor\":\"test\",\"op\":\"task.close\",\"params\":{\"id\":$task_id,\"outcome_summary\":\"shipped\",\"closing_comment\":\"verified\",\"commit_sha\":\"deadbeef1234\",\"dirty\":false,\"files_changed\":[\"src/foo.c\",\"src/bar.c\"],\"validations_run\":[{\"cmd\":\"make test\",\"status\":\"passed\"},{\"cmd\":\"make lint\",\"status\":\"passed\"}],\"decisions\":[\"use approach X\",\"reject Y\"]}}")
expect_ok "$close"

export_resp=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"ex\",\"actor\":\"test\",\"op\":\"plan.export\",\"params\":{\"plan_id\":$plan_id}}")
expect_ok "$export_resp"

# Pull the closure record by id (we just created it; id=1 is fine but use jq
# to be explicit so future tests can add closures without breaking us).
closure_json=$(printf '%s' "$export_resp" | jq -c --argjson tid "$task_id" \
    '.result.export.closures[] | select(.entity_type == "task" and .entity_id == $tid)')
[ -n "$closure_json" ] || { echo "no closure for task $task_id in export" >&2; exit 1; }

# Every v2.1 field must round-trip identically through the export.
printf '%s' "$closure_json" | jq -e '
    .commit_sha == "deadbeef1234"
    and .dirty == false
    and (.dirty | type == "boolean")
    and .files_changed == ["src/foo.c","src/bar.c"]
    and .validations_run == [
        {"cmd":"make test","status":"passed"},
        {"cmd":"make lint","status":"passed"}
    ]
    and .decisions == ["use approach X","reject Y"]
' >/dev/null

# followup_needed must be a boolean in the export, not an integer — same
# shape as closure.get returns. (Pre-fix it was rendered as the int 0/1.)
printf '%s' "$closure_json" | jq -e '.followup_needed | type == "boolean"' >/dev/null
