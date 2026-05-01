#!/bin/sh
# Verify IPMAN_LOG env-var gating end-to-end: the binary must call
# ipman_log_init() at startup, the default level must silence info, and
# IPMAN_LOG=info must restore the structured stream on stderr. Stdout
# (the JSON envelope) must be unaffected by any IPMAN_LOG value.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-log-env.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

# 1. Default (unset): init emits info lines on stderr only when IPMAN_LOG
#    permits info. With nothing set, stderr must be silent.
home_a="$TMP/a"
err=$(IPMAN_HOME="$home_a" "$BIN" init 2>&1 >/dev/null)
if printf '%s' "$err" | grep -q 'level=info'; then
    echo "FAIL: default level leaked info logs to stderr" >&2
    echo "  stderr: $err" >&2
    exit 1
fi

# 2. IPMAN_LOG=info: stderr now carries info lines (created ipman_home,
#    created keysalt, applied migration ...).
home_b="$TMP/b"
err=$(IPMAN_HOME="$home_b" IPMAN_LOG=info "$BIN" init 2>&1 >/dev/null)
if ! printf '%s' "$err" | grep -q 'level=info'; then
    echo "FAIL: IPMAN_LOG=info did not emit info lines" >&2
    echo "  stderr: $err" >&2
    exit 1
fi

# 3. IPMAN_LOG=error: info AND warn must be suppressed under normal init.
home_c="$TMP/c"
err=$(IPMAN_HOME="$home_c" IPMAN_LOG=error "$BIN" init 2>&1 >/dev/null)
if printf '%s' "$err" | grep -q 'level=info\|level=warn'; then
    echo "FAIL: IPMAN_LOG=error leaked info or warn logs" >&2
    echo "  stderr: $err" >&2
    exit 1
fi

# 4. IPMAN_LOG=garbage: unknown value falls back to default (warn). Init
#    must succeed, and stderr must be silent for info-level call sites.
home_d="$TMP/d"
out=$(IPMAN_HOME="$home_d" IPMAN_LOG=verbose "$BIN" init 2>&1)
if ! printf '%s' "$out" | grep -q '"ok":true'; then
    echo "FAIL: unknown IPMAN_LOG value broke init" >&2
    echo "  output: $out" >&2
    exit 1
fi
err=$(IPMAN_HOME="$TMP/d2" IPMAN_LOG=verbose "$BIN" init 2>&1 >/dev/null)
if printf '%s' "$err" | grep -q 'level=info'; then
    echo "FAIL: unknown IPMAN_LOG value did not fall back to default" >&2
    exit 1
fi

# 5. Stdout (JSON envelope) is independent of IPMAN_LOG. Compare init
#    response under default vs IPMAN_LOG=debug — both must be valid ok JSON.
out_default=$(IPMAN_HOME="$TMP/e" "$BIN" init 2>/dev/null)
out_debug=$(IPMAN_HOME="$TMP/f" IPMAN_LOG=debug "$BIN" init 2>/dev/null)
printf '%s' "$out_default" | jq -e '.ok == true' >/dev/null
printf '%s' "$out_debug"   | jq -e '.ok == true' >/dev/null
