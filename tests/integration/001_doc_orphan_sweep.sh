#!/bin/sh
set -eu

# Sweep stale agent-doc artifacts in workspace.refresh_agent_docs.
#
# Without sweep: files written by an older binary version (and no longer
# emitted by the current one — ops renamed/removed, entities removed, etc.)
# stay on disk indefinitely, growing the workspace and confusing agents that
# read .ipman/ directly.
#
# With sweep: refresh enumerates the generated subdirs, diffs against the
# artifact list it just wrote, and moves the leftovers to
# .ipman/.attic/<timestamp>/<rel-path>. Eviction (rather than unlink) keeps
# the data recoverable until the operator removes .attic explicitly. The
# count is capped — see 002_doc_attic_limit.sh for that side.

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-doc-orphan.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

call_ipman() {
    printf '%s\n' "$1" | IPMAN_HOME="$TMP/home" "$BIN" 2>/dev/null
}

expect_ok() {
    printf '%s' "$1" | jq -e '.ok == true' >/dev/null
}

IPMAN_HOME="$TMP/home" "$BIN" init >/dev/null 2>/dev/null

# Plant three orphans across three different generated subdirs. Names mimic
# the real artifact-naming pattern so a buggy sweep that whitelists by prefix
# would still need to know they shouldn't be there.
ORPHAN_OP="$TMP/home/operations/ipman.op.removed.fake.schema.md"
ORPHAN_SCHEMA="$TMP/home/schemas/ipman.op.removed.fake.request.schema.json"
ORPHAN_EXAMPLE="$TMP/home/examples/ipman.op.removed.fake.input.example.md"

printf 'orphan op doc — should be evicted\n' > "$ORPHAN_OP"
printf '{"orphan":true}\n' > "$ORPHAN_SCHEMA"
printf 'orphan example — should be evicted\n' > "$ORPHAN_EXAMPLE"

test -f "$ORPHAN_OP"
test -f "$ORPHAN_SCHEMA"
test -f "$ORPHAN_EXAMPLE"

# Run refresh. Expect 3 files moved to a fresh attic subdir.
refresh=$(call_ipman '{"protocol_version":2,"request_id":"r1","actor":"test","op":"workspace.refresh_agent_docs","params":{}}')
expect_ok "$refresh"

printf '%s' "$refresh" | jq -e '
  .result.files_removed == 3
  and .result.attic_files_total == 3
  and (.result.attic_dir | test("^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}-[0-9]{2}-[0-9]{2}\\.[0-9]{6}Z$"))
' >/dev/null

attic_dir=$(printf '%s' "$refresh" | jq -r '.result.attic_dir')

# Originals gone from live tree.
test ! -e "$ORPHAN_OP"        || { echo "orphan op doc survived sweep" >&2; exit 1; }
test ! -e "$ORPHAN_SCHEMA"    || { echo "orphan schema survived sweep" >&2; exit 1; }
test ! -e "$ORPHAN_EXAMPLE"   || { echo "orphan example survived sweep" >&2; exit 1; }

# Originals are recoverable from .attic/<ts>/<subdir>/<filename>.
test -f "$TMP/home/.attic/$attic_dir/operations/ipman.op.removed.fake.schema.md"
test -f "$TMP/home/.attic/$attic_dir/schemas/ipman.op.removed.fake.request.schema.json"
test -f "$TMP/home/.attic/$attic_dir/examples/ipman.op.removed.fake.input.example.md"

# Legitimate files untouched (no over-deletion).
test -f "$TMP/home/operations/ipman.op.task.create.schema.md"
test -f "$TMP/home/schemas/ipman.op.task.create.request.schema.json"
test -f "$TMP/home/examples/ipman.op.task.create.input.example.md"
test -f "$TMP/home/manifest.json"

# Sweep must NOT touch the database, keysalt, or init lock — those are not
# generated artifacts and live alongside the docs.
test -f "$TMP/home/ipman.db"
test -f "$TMP/home/keysalt"

# Second refresh on a clean tree: nothing to evict, no new attic subdir,
# attic_files_total still reflects the leftovers from the first run.
again=$(call_ipman '{"protocol_version":2,"request_id":"r2","actor":"test","op":"workspace.refresh_agent_docs","params":{}}')
expect_ok "$again"
printf '%s' "$again" | jq -e '
  .result.files_removed == 0
  and .result.attic_dir == ""
  and .result.attic_files_total == 3
' >/dev/null

# Only ONE timestamp directory under .attic/ — the second refresh did not
# create an empty one.
attic_subdirs=$(find "$TMP/home/.attic" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ')
test "$attic_subdirs" = "1" || { echo "expected exactly 1 attic subdir, found $attic_subdirs" >&2; exit 1; }

echo "ok 001_doc_orphan_sweep"
