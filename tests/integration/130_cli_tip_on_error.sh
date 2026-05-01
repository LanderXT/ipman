#!/bin/sh
# Verify the parse-error tip end-to-end: when ipman_request_parse rejects
# the envelope, stderr gains a one-line `ipman: tip — ...` nudge after the
# JSON error response on stdout. The tip must NOT fire for op-level errors
# (validation_failed, not_found, etc.) where the envelope was well-formed.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-tip.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

IPMAN_HOME="$TMP/home" "$BIN" init >/dev/null

run_split() {
    # Capture stdout in $1 and stderr in $2 from a single binary invocation.
    body=$1
    out_file=$2
    err_file=$3
    printf '%s' "$body" | IPMAN_HOME="$TMP/home" "$BIN" \
        >"$out_file" 2>"$err_file" || true
}

# 1. Empty stdin → invalid_request on stdout, tip on stderr.
run_split '' "$TMP/out1" "$TMP/err1"
if ! jq -e '.ok == false and .error.code == "invalid_request"' "$TMP/out1" >/dev/null; then
    echo "FAIL: empty stdin did not produce invalid_request" >&2
    cat "$TMP/out1" >&2
    exit 1
fi
if ! grep -q 'ipman: tip' "$TMP/err1"; then
    echo "FAIL: empty stdin missing tip on stderr" >&2
    cat "$TMP/err1" >&2
    exit 1
fi

# 2. Empty JSON object → still a parse failure (missing protocol_version),
#    tip fires.
run_split '{}' "$TMP/out2" "$TMP/err2"
if ! jq -e '.ok == false and .error.code == "invalid_request"' "$TMP/out2" >/dev/null; then
    echo "FAIL: '{}' did not produce invalid_request" >&2
    cat "$TMP/out2" >&2
    exit 1
fi
if ! grep -q 'ipman: tip' "$TMP/err2"; then
    echo "FAIL: '{}' missing tip on stderr" >&2
    cat "$TMP/err2" >&2
    exit 1
fi

# 3. Bad JSON syntax → tip fires.
run_split 'not json at all' "$TMP/out3" "$TMP/err3"
if ! jq -e '.ok == false and .error.code == "invalid_request"' "$TMP/out3" >/dev/null; then
    echo "FAIL: bad JSON did not produce invalid_request" >&2
    cat "$TMP/out3" >&2
    exit 1
fi
if ! grep -q 'ipman: tip' "$TMP/err3"; then
    echo "FAIL: bad JSON missing tip on stderr" >&2
    cat "$TMP/err3" >&2
    exit 1
fi

# 4. Well-formed envelope, unknown op → op-level unknown_op. Tip MUST NOT fire.
run_split '{"protocol_version":2,"request_id":"r1","actor":"tester","op":"no.such","params":{}}' \
    "$TMP/out4" "$TMP/err4"
if ! jq -e '.ok == false and .error.code == "unknown_op"' "$TMP/out4" >/dev/null; then
    echo "FAIL: unknown op did not produce unknown_op" >&2
    cat "$TMP/out4" >&2
    exit 1
fi
if grep -q 'ipman: tip' "$TMP/err4"; then
    echo "FAIL: tip leaked on op-level error" >&2
    cat "$TMP/err4" >&2
    exit 1
fi

# 5. Well-formed envelope, validation_failed (e.g. plan.get with bad params).
run_split '{"protocol_version":2,"request_id":"r2","actor":"tester","op":"plan.get","params":{}}' \
    "$TMP/out5" "$TMP/err5"
if ! jq -e '.ok == false' "$TMP/out5" >/dev/null; then
    echo "FAIL: plan.get with empty params did not error" >&2
    cat "$TMP/out5" >&2
    exit 1
fi
if grep -q 'ipman: tip' "$TMP/err5"; then
    echo "FAIL: tip leaked on validation_failed" >&2
    cat "$TMP/err5" >&2
    exit 1
fi

# 6. Stdout is JSON-only on parse failure: stderr text must not contaminate
#    stdout. Stdout from case 1 must parse as a single JSON object.
if ! jq -e 'type == "object"' "$TMP/out1" >/dev/null; then
    echo "FAIL: stdout on parse failure is not a single JSON object" >&2
    cat "$TMP/out1" >&2
    exit 1
fi
