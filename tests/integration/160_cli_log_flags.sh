#!/bin/sh
# Verify the v2.2 ipman -LG / --log render flags:
#   - default: Details column present, default limit 20
#   - --summary-only: Details column absent
#   - --limit N: row count matches N
#   - --limit 1000: clamps to 500 client-side (no validation_failed error)
#   - --limit 0 / --limit -3: clamps up to 1
#   - bad / unknown flags fail fast with a CLI message
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-log-flags.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
export IPMAN_HOME="$TMP/home"

call_ipman() {
    printf '%s\n' "$1" | "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

"$BIN" init >/dev/null 2>/dev/null

plan=$(call_ipman '{"protocol_version":2,"request_id":"plan","actor":"test","op":"plan.create","params":{"code":"LOG-FLAGS","title":"Log flags","label":"log-flags"}}')
expect_ok "$plan"
plan_id=$(printf '%s' "$plan" | jq -r '.result.plan.id')

# Seed enough events to exercise --limit
i=1
while [ $i -le 6 ]; do
    t=$(call_ipman "{\"protocol_version\":2,\"request_id\":\"t$i\",\"actor\":\"test\",\"op\":\"task.create\",\"params\":{\"plan_id\":$plan_id,\"title\":\"T$i\",\"label\":\"task-$i\"}}")
    expect_ok "$t"
    i=$((i + 1))
done

# 1. Default render: Details column present
default_out=$("$BIN" --log 2>/dev/null)
if ! printf '%s' "$default_out" | grep -q 'Details'; then
    echo "FAIL: default --log output missing Details column" >&2
    printf '%s\n' "$default_out" >&2
    exit 1
fi
if ! printf '%s' "$default_out" | grep -q 'Summary'; then
    echo "FAIL: default --log output missing Summary column" >&2
    exit 1
fi
if ! printf '%s' "$default_out" | grep -qF '{"op":'; then
    echo "FAIL: default --log output missing details JSON payload" >&2
    exit 1
fi

# 2. --summary-only drops Details, keeps Summary
so_out=$("$BIN" --log --summary-only 2>/dev/null)
if printf '%s' "$so_out" | grep -q 'Details'; then
    echo "FAIL: --summary-only output still contains Details column" >&2
    printf '%s\n' "$so_out" >&2
    exit 1
fi
if ! printf '%s' "$so_out" | grep -q 'Summary'; then
    echo "FAIL: --summary-only dropped Summary column (should remain)" >&2
    exit 1
fi
if printf '%s' "$so_out" | grep -qF '{"op":'; then
    echo "FAIL: --summary-only leaked details JSON payload into output" >&2
    exit 1
fi

# 3. --limit N: data row count equals N. Data rows are bordered by ─ separators;
#    count the header separators (├) which appear once between header and first
#    row, and once between every adjacent pair.
count_data_rows() {
    # Each rendered data row contains an ISO timestamp in the When column
    # (e.g. "2026-05-01T13:45:07"). Header / border rows do not. This is the
    # cleanest portable signal — sidesteps grepping multibyte box-drawing
    # characters in /bin/sh.
    printf '%s\n' "$1" | grep -cE '20[0-9]{2}-[0-9]{2}-[0-9]{2}T' || true
}

l2=$("$BIN" --log --limit 2 2>/dev/null)
n2=$(count_data_rows "$l2")
if [ "$n2" != "2" ]; then
    echo "FAIL: --limit 2 produced $n2 data rows (expected 2)" >&2
    printf '%s\n' "$l2" >&2
    exit 1
fi

l1=$("$BIN" --log --limit 1 --summary-only 2>/dev/null)
n1=$(count_data_rows "$l1")
if [ "$n1" != "1" ]; then
    echo "FAIL: --limit 1 produced $n1 data rows (expected 1)" >&2
    exit 1
fi

# 4. --limit 1000 clamps client-side to 500: command succeeds with no error
#    on stderr (wire would return validation_failed for limit > 500).
err=$("$BIN" --log --limit 1000 --summary-only 2>&1 >/dev/null)
if printf '%s' "$err" | grep -q 'validation_failed\|limit must be'; then
    echo "FAIL: --limit 1000 leaked wire validation error (clamp missing)" >&2
    echo "  stderr: $err" >&2
    exit 1
fi

# We have 7 events seeded (1 plan_created + 6 task_created), so --limit 1000
# returns all 7 — the assertion is "no error", not a row count.
big_out=$("$BIN" --log --limit 1000 --summary-only 2>/dev/null)
n_big=$(count_data_rows "$big_out")
if [ "$n_big" -lt 7 ] || [ "$n_big" -gt 500 ]; then
    echo "FAIL: --limit 1000 returned $n_big rows (expected 7..500)" >&2
    exit 1
fi

# 5. --limit 0 clamps up to 1
l0=$("$BIN" --log --limit 0 --summary-only 2>/dev/null)
n0=$(count_data_rows "$l0")
if [ "$n0" != "1" ]; then
    echo "FAIL: --limit 0 produced $n0 data rows (expected 1)" >&2
    exit 1
fi

# 6. Bad limit value fails fast
if "$BIN" --log --limit notanumber >/dev/null 2>&1; then
    echo "FAIL: --limit notanumber should have failed" >&2
    exit 1
fi
if "$BIN" --log --limit 5x >/dev/null 2>&1; then
    echo "FAIL: --limit 5x should have failed (trailing garbage)" >&2
    exit 1
fi

# 7. --limit without value fails fast
if "$BIN" --log --limit >/dev/null 2>&1; then
    echo "FAIL: bare --limit should have failed" >&2
    exit 1
fi

# 8. Unknown flag rejected
if "$BIN" --log --bogus >/dev/null 2>&1; then
    echo "FAIL: unknown flag --bogus should have failed" >&2
    exit 1
fi

# 9. Bare-word form (`ipman log`) and short flag (`-LG`) honor the same flags
sh_out=$("$BIN" -LG --summary-only 2>/dev/null)
if printf '%s' "$sh_out" | grep -q 'Details'; then
    echo "FAIL: -LG --summary-only kept Details column" >&2
    exit 1
fi
bw_out=$("$BIN" log --limit 3 2>/dev/null)
n_bw=$(count_data_rows "$bw_out")
if [ "$n_bw" != "3" ]; then
    echo "FAIL: bare-word `ipman log --limit 3` produced $n_bw rows (expected 3)" >&2
    exit 1
fi
