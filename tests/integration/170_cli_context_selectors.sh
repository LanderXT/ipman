#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-cli-context.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

HOME_DIR="$TMP/home"
IPMAN_HOME="$HOME_DIR" "$BIN" init >/dev/null

printf '%s\n' \
  '{"protocol_version":2,"request_id":"p","actor":"test","op":"plan.create","params":{"code":"P1","title":"Context Plan","label":"context-plan"}}' \
  | IPMAN_HOME="$HOME_DIR" "$BIN" >/dev/null

printf '%s\n' \
  '{"protocol_version":2,"request_id":"ph","actor":"test","op":"phase.create","params":{"plan_id":1,"title":"Alpha Phase","label":"alpha-phase"}}' \
  | IPMAN_HOME="$HOME_DIR" "$BIN" >/dev/null

printf '%s\n' \
  '{"protocol_version":2,"request_id":"t","actor":"test","op":"task.create","params":{"plan_id":1,"phase_id":1,"title":"Alpha Task","label":"alpha-task"}}' \
  | IPMAN_HOME="$HOME_DIR" "$BIN" >/dev/null

IPMAN_HOME="$HOME_DIR" "$BIN" --activate P1 >/dev/null
IPMAN_HOME="$HOME_DIR" "$BIN" --current alpha-task >/dev/null

status_out="$TMP/status.txt"
IPMAN_HOME="$HOME_DIR" "$BIN" -S >"$status_out"

grep -q 'P1 .* Context Plan' "$status_out"
grep -q 'alpha-phase .* Alpha Phase' "$status_out"
grep -q 'alpha-task .* Alpha Task' "$status_out"

printf '%s\n' \
  '{"protocol_version":2,"request_id":"ctx","actor":"test","op":"workspace.context_get","params":{}}' \
  | IPMAN_HOME="$HOME_DIR" "$BIN" \
  | jq -e '
      .ok == true
      and .result.context.active_plan.code == "P1"
      and .result.context.active_plan.label == "context-plan"
      and .result.context.current_phase.label == "alpha-phase"
      and .result.context.current_task.label == "alpha-task"
    ' >/dev/null
