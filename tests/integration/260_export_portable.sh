#!/bin/sh
set -eu

# Integration test: portable export/import round-trip.
#
# Creates a source workspace with 1 plan, 1 phase, 2 tasks.
# Exports as a portable bundle (IPMAN_PASSPHRASE bypasses interactive prompt).
# Imports into a fresh workspace on the same machine.
# Verifies: plan exists with same title, phase count=1, task count=2.

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-export-portable.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

SRC_HOME="$TMP/src-home"
DST_HOME="$TMP/dst-home"
BUNDLE="$TMP/workspace.ipman"
PASSPHRASE="test-passphrase-round-trip"

# Helper: call ipman op in SRC workspace, return JSON
src_call() {
    printf '%s\n' "$1" | IPMAN_HOME="$SRC_HOME" "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

# ---- Bootstrap source workspace -------------------------------------------
IPMAN_HOME="$SRC_HOME" "$BIN" init >/dev/null 2>/dev/null

plan=$(src_call '{"protocol_version":2,"request_id":"p1","actor":"t","op":"plan.create","params":{"title":"Portable Test Plan","priority":"medium"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

ph=$(src_call "{\"protocol_version\":2,\"request_id\":\"ph1\",\"actor\":\"t\",\"op\":\"phase.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"Phase One\",\"sequence_no\":1}}")
expect_ok "$ph"
ph_id=$(printf '%s' "$ph" | jq -r '.result.phase.id')

t1=$(src_call "{\"protocol_version\":2,\"request_id\":\"t1\",\"actor\":\"t\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$ph_id,\"title\":\"Task Alpha\"}}")
expect_ok "$t1"

t2=$(src_call "{\"protocol_version\":2,\"request_id\":\"t2\",\"actor\":\"t\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"phase_id\":$ph_id,\"title\":\"Task Beta\"}}")
expect_ok "$t2"

# ---- Export portable bundle ------------------------------------------------
IPMAN_PASSPHRASE="$PASSPHRASE" IPMAN_HOME="$SRC_HOME" "$BIN" export-portable --output "$BUNDLE"

# Verify bundle file exists and has the IPMX magic header
if [ ! -f "$BUNDLE" ]; then
    echo "FAIL: bundle file not created" >&2
    exit 1
fi

magic=$(head -c 4 "$BUNDLE" | od -An -tx1 | tr -d ' \n')
if [ "$magic" != "49504d58" ]; then
    echo "FAIL: bundle has wrong magic: $magic (expected 49504d58)" >&2
    exit 1
fi

# Verify bundle permissions are 0600
perms=$(stat -c '%a' "$BUNDLE")
if [ "$perms" != "600" ]; then
    echo "FAIL: bundle permissions are $perms, expected 600" >&2
    exit 1
fi

# ---- Import into fresh workspace -------------------------------------------
IPMAN_PASSPHRASE="$PASSPHRASE" IPMAN_HOME="$DST_HOME" "$BIN" import-portable "$BUNDLE"

# Helper: call ipman op in DST workspace
dst_call() {
    printf '%s\n' "$1" | IPMAN_HOME="$DST_HOME" "$BIN" 2>/dev/null
}

# Verify plan exists with same title
plans=$(dst_call '{"protocol_version":2,"request_id":"lp","actor":"t","op":"plan.list","params":{}}')
expect_ok "$plans"

plan_title=$(printf '%s' "$plans" | jq -r '.result.plans[0].title')
if [ "$plan_title" != "Portable Test Plan" ]; then
    echo "FAIL: imported plan title '$plan_title' != 'Portable Test Plan'" >&2
    exit 1
fi

dst_plan_id=$(printf '%s' "$plans" | jq -r '.result.plans[0].id')

# Verify phase count = 1
phases=$(dst_call "{\"protocol_version\":2,\"request_id\":\"lph\",\"actor\":\"t\",\"op\":\"phase.list\",\"params\":{\"plan_id\":$dst_plan_id}}")
expect_ok "$phases"
phase_count=$(printf '%s' "$phases" | jq '.result.phases | length')
if [ "$phase_count" -ne 1 ]; then
    echo "FAIL: expected 1 phase, got $phase_count" >&2
    exit 1
fi

# Verify task count = 2
tasks=$(dst_call "{\"protocol_version\":2,\"request_id\":\"lt\",\"actor\":\"t\",\"op\":\"task.list\",\"params\":{\"plan_id\":$dst_plan_id}}")
expect_ok "$tasks"
task_count=$(printf '%s' "$tasks" | jq '.result.tasks | length')
if [ "$task_count" -ne 2 ]; then
    echo "FAIL: expected 2 tasks, got $task_count" >&2
    exit 1
fi

# ---- Wrong passphrase is rejected ------------------------------------------
wrong_import_out=$(IPMAN_PASSPHRASE="wrong-passphrase" IPMAN_HOME="$TMP/dst-wrong" "$BIN" import-portable "$BUNDLE" 2>&1 || true)
if [ -d "$TMP/dst-wrong/.ipman" ] && [ -f "$TMP/dst-wrong/.ipman/ipman.db" ]; then
    echo "FAIL: import with wrong passphrase should not create database" >&2
    exit 1
fi

echo "PASS: export-portable / import-portable round-trip"
