#!/bin/sh
set -eu

# Integration test for v2.4 UTF-8 slug generation.
# Covers:
#   * plan.create with a Spanish diacritic title produces the correct label
#   * phase.create with a diacritic title produces the correct label
#   * task.create with a diacritic title produces the correct label

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-slug-diacritics.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

call_ipman() {
    printf '%s\n' "$1" | IPMAN_HOME="$TMP/home" "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

IPMAN_HOME="$TMP/home" "$BIN" init >/dev/null 2>/dev/null

# --- plan.create with Spanish diacritic title --------------------------------

plan=$(call_ipman '{"protocol_version":2,"request_id":"p1","actor":"hleal","op":"plan.create","params":{"title":"Consolidación del sistema","summary":"Spanish diacritic test"}}')
expect_ok "$plan"
printf '%s' "$plan" | jq -e '
  .result.plan.label == "consolidacion-del-sistema"
' >/dev/null

# --- phase.create with German diacritic title --------------------------------

plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

phase=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"ph1\",\"actor\":\"hleal\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Müller Übersicht\",\"summary\":\"German diacritic test\"}}")
expect_ok "$phase"
printf '%s' "$phase" | jq -e '
  .result.phase.label == "muller-ubersicht"
' >/dev/null

# --- task.create with French diacritic title ---------------------------------

phase_id=$(printf '%s' "$phase" | jq -r '.result.phase.id')

task=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"t1\",\"actor\":\"hleal\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Café réservé\",\"summary\":\"French diacritic test\"}}")
expect_ok "$task"
printf '%s' "$task" | jq -e '
  .result.task.label == "cafe-reserve"
' >/dev/null

# --- task.create with Polish title (Latin Extended-A) ------------------------
# Exercises U+0142 (ł) and U+017A (ź) — the Latin Extended-A range.

task2=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"t2\",\"actor\":\"hleal\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Łódź relay\",\"summary\":\"Polish diacritic test\"}}")
expect_ok "$task2"
printf '%s' "$task2" | jq -e '
  .result.task.label == "lodz-relay"
' >/dev/null

# --- task.create with German ß title -----------------------------------------
# Exercises a multi-char mapping (ß -> ss).

task3=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"t3\",\"actor\":\"hleal\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$phase_id,\"title\":\"Weißbier zählen\",\"summary\":\"German sharp-s test\"}}")
expect_ok "$task3"
printf '%s' "$task3" | jq -e '
  .result.task.label == "weissbier-zahlen"
' >/dev/null

echo "ok 230_slugify_diacritics"
