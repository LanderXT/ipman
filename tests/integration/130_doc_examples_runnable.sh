#!/bin/sh
# Regression: every auto-generated operation example must be runnable as
# written. The 000_operation_docs_parity test only checks that op names
# match across the dispatch registry, the manifest, and the doc/schema sets;
# it never executes the example payloads, so doc/runtime drift in the
# example_params column of agent_docs.c slips through (the v2.1 release
# shipped seven plan.* examples that used `code` as a selector after the
# allowlists had been narrowed to `id`-only).
#
# This test runs every example payload against a fresh, populated workspace
# (one plan, one phase, one task) and rejects any response whose error
# details flag the example body as malformed. The two failure shapes that
# matter are:
#
#   - validation_failed with details.kind == "unknown_parameter"
#       → the example sends a field the allowlist rejects (drift).
#   - validation_failed with no details.kind, e.g. "closure text fields
#     must be non-empty strings"
#       → the example omits a runtime-required field (the case that bit
#         the original plan.close example).
#
# Workspace-state errors (not_found, conflict, transition rules, lookups
# that don't match the seeded labels) are *expected* for many examples
# against a generic fixture and are explicitly not flagged.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-doc-examples.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

call_in() {
    home=$1
    shift
    printf '%s\n' "$1" | IPMAN_HOME="$home" "$BIN" 2>/dev/null
}

# Seed a workspace that satisfies the most common selectors used by the
# auto-generated examples: plan id=1 (open, code REL-001), phase id=1
# (open), task id=1 (todo). Each example runs against its own copy so
# state mutations from one example never poison another.
seed_workspace() {
    home=$1
    IPMAN_HOME="$home" "$BIN" init >/dev/null 2>&1
    call_in "$home" '{"protocol_version":2,"request_id":"p","actor":"t","op":"plan.create","params":{"code":"REL-001","title":"Release 1","label":"release-1","priority":"high"}}' >/dev/null
    call_in "$home" '{"protocol_version":2,"request_id":"a","actor":"t","op":"plan.activate","params":{"id":1}}' >/dev/null
    call_in "$home" '{"protocol_version":2,"request_id":"ph1","actor":"t","op":"phase.create","params":{"plan_id":1,"label":"implementation","title":"Implementation"}}' >/dev/null
    # A second phase lets phase.move's example ({"id":1,"sequence_no":2})
    # exercise a real reorder rather than tripping the range check.
    call_in "$home" '{"protocol_version":2,"request_id":"ph2","actor":"t","op":"phase.create","params":{"plan_id":1,"label":"verification","title":"Verification"}}' >/dev/null
    call_in "$home" '{"protocol_version":2,"request_id":"tk","actor":"t","op":"task.create","params":{"plan_id":1,"phase_id":1,"label":"implement-feature","title":"Implement feature"}}' >/dev/null
}

# A reference workspace gives us the example file paths.
ref_home="$TMP/ref"
seed_workspace "$ref_home" >/dev/null

failures=0
for f in "$ref_home"/examples/*.input.example.md; do
    op=$(basename "$f" .input.example.md | sed 's/^ipman.op.//')
    payload=$(awk '/^```json$/{flag=1;next}/^```$/{flag=0}flag' "$f")
    [ -z "$payload" ] && continue

    case_home="$TMP/$op"
    seed_workspace "$case_home"
    out=$(call_in "$case_home" "$payload")
    rm -rf "$case_home"

    # Decide pass/fail. The script-level invariant: a malformed example body
    # is one where the dispatcher rejects validation. Workspace-state
    # errors (not_found / conflict / etc.) are tolerated.
    code=$(printf '%s' "$out" | jq -r '.error.code // empty')
    if [ "$code" != "validation_failed" ]; then
        continue
    fi

    msg=$(printf '%s' "$out" | jq -r '.error.message // ""')
    kind=$(printf '%s' "$out" | jq -r '.error.details.kind // ""')
    printf 'broken example: %s — %s%s\n' "$op" "$msg" \
        "${kind:+ (kind=$kind)}" >&2
    failures=$((failures + 1))
done

if [ "$failures" -ne 0 ]; then
    printf '%d example(s) fail validation as written\n' "$failures" >&2
    exit 1
fi
