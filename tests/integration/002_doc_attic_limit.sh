#!/bin/sh
set -eu

# Attic limit enforcement on workspace.refresh_agent_docs.
#
# When .ipman/.attic accumulates too many evicted artifacts, the explicit
# refresh op refuses with attic_full so the operator notices and cleans
# .ipman/.attic before continuing. The implicit refresh during open_workspace
# is intentionally NOT enforced — other ops continue to work even when attic
# is over the limit, so the user is not bricked while they investigate.
#
# Limit is configurable via IPMAN_ATTIC_LIMIT (default 100). Warn fires at
# half the limit. We use IPMAN_ATTIC_LIMIT=4 here to keep the test fast:
# warn at 2, refuse at 4.

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-attic-limit.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

call_ipman_with_limit() {
    limit=$1
    payload=$2
    stderr_capture=$3
    printf '%s\n' "$payload" \
        | IPMAN_HOME="$TMP/home" IPMAN_ATTIC_LIMIT="$limit" "$BIN" 2>"$stderr_capture"
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

IPMAN_HOME="$TMP/home" "$BIN" init >/dev/null 2>/dev/null

# --- Stage 1: plant 3 orphans, refresh under limit=4. -------------------
# Expected: success, files_removed=3, attic_files_total=3, warn fires
# (3 >= warn threshold of 2).
for i in 1 2 3; do
    touch "$TMP/home/operations/ipman.op.fake$i.schema.md"
done

stderr1="$TMP/stderr1"
r1=$(call_ipman_with_limit 4 \
    '{"protocol_version":2,"request_id":"a","actor":"test","op":"workspace.refresh_agent_docs","params":{}}' \
    "$stderr1")
expect_ok "$r1"
printf '%s' "$r1" | jq -e '
  .result.files_removed == 3
  and .result.attic_files_total == 3
' >/dev/null
grep -q "attic approaching limit" "$stderr1" || {
    echo "expected warn log at 3/4, missing in stderr" >&2
    cat "$stderr1" >&2
    exit 1
}

# --- Stage 2: plant 2 more orphans, refresh under limit=4. --------------
# After eviction, attic holds 5 total -> exceeds limit=4 -> refuse with
# attic_full. Note: the work itself (writes + eviction) DID happen this
# invocation; the error tells the user to clean before the next refresh.
for i in 4 5; do
    touch "$TMP/home/operations/ipman.op.fake$i.schema.md"
done

stderr2="$TMP/stderr2"
r2=$(call_ipman_with_limit 4 \
    '{"protocol_version":2,"request_id":"b","actor":"test","op":"workspace.refresh_agent_docs","params":{}}' \
    "$stderr2")
printf '%s' "$r2" | jq -e '
  .ok == false
  and .error.code == "attic_full"
' >/dev/null
# .attic now has 5 files even though the op returned attic_full — confirm.
attic_count=$(find "$TMP/home/.attic" -type f | wc -l | tr -d ' ')
test "$attic_count" = "5" || {
    echo "expected 5 files in .attic after refused refresh, found $attic_count" >&2
    exit 1
}

# --- Stage 3: still over limit -> still refuses, even with no new orphans
# to evict. Confirms the limit is on attic state, not on this-run additions.
stderr3="$TMP/stderr3"
r3=$(call_ipman_with_limit 4 \
    '{"protocol_version":2,"request_id":"c","actor":"test","op":"workspace.refresh_agent_docs","params":{}}' \
    "$stderr3")
printf '%s' "$r3" | jq -e '.ok == false and .error.code == "attic_full"' >/dev/null

# --- Stage 4: clean .attic, refresh succeeds with no removals. ----------
rm -rf "$TMP/home/.attic"
stderr4="$TMP/stderr4"
r4=$(call_ipman_with_limit 4 \
    '{"protocol_version":2,"request_id":"d","actor":"test","op":"workspace.refresh_agent_docs","params":{}}' \
    "$stderr4")
expect_ok "$r4"
printf '%s' "$r4" | jq -e '
  .result.files_removed == 0
  and .result.attic_files_total == 0
  and .result.attic_dir == ""
' >/dev/null

# --- Stage 5: env var override raises the limit so the same state passes.
# Re-plant 3 orphans, refresh under limit=10 -> success, no warn (3 < 5).
for i in 1 2 3; do
    touch "$TMP/home/operations/ipman.op.fake$i.schema.md"
done
stderr5="$TMP/stderr5"
r5=$(call_ipman_with_limit 10 \
    '{"protocol_version":2,"request_id":"e","actor":"test","op":"workspace.refresh_agent_docs","params":{}}' \
    "$stderr5")
expect_ok "$r5"
printf '%s' "$r5" | jq -e '.result.files_removed == 3' >/dev/null
if grep -q "attic approaching limit" "$stderr5"; then
    echo "did not expect warn log at 3/10, but found one in stderr" >&2
    cat "$stderr5" >&2
    exit 1
fi

# --- Stage 6: implicit refresh (any non-refresh op) must succeed even
# when attic is over the limit. This is the bricking-safety guarantee.
# Plant another wave of orphans, then run plan.list with limit=2.
for i in 6 7 8; do
    touch "$TMP/home/operations/ipman.op.fake$i.schema.md"
done
stderr6="$TMP/stderr6"
r6=$(call_ipman_with_limit 2 \
    '{"protocol_version":2,"request_id":"f","actor":"test","op":"plan.list","params":{}}' \
    "$stderr6")
expect_ok "$r6"

echo "ok 002_doc_attic_limit"
