#!/bin/sh
set -eu

# Error-code parity: every string in src/protocol.c::k_code_strings must
# be listed in the auto-generated envelope doc, and vice versa.
#
# Why: the "Known error codes:" sentence in agent_docs.c is hand-authored
# (the comment at agent_docs.c:558-560 explicitly warns that the list
# mirrors src/protocol.c and must be kept in sync — the generator does
# not introspect the enum). v2.4.1 shipped IPMAN_ERR_ATTIC_FULL in the
# enum but missed updating that sentence; the gap was caught by a
# post-merge review. This test makes the WARN-comment a machine-checked
# guarantee instead of a discipline ask.

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-error-parity.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

IPMAN_HOME="$TMP/home" "$BIN" init >/dev/null 2>/dev/null

src_codes="$TMP/src.codes"
doc_codes="$TMP/doc.codes"

# Pull the canonical list from src/protocol.c — every quoted string inside
# the k_code_strings array. awk extracts the value after the equals sign
# in lines like:   [IPMAN_ERR_NOT_FOUND] = "not_found",
sed -n '/k_code_strings\[\] = {/,/^};/p' "$ROOT/src/protocol.c" \
    | awk -F'"' '/= "/ { print $2 }' \
    | sort >"$src_codes"

# Pull the codes listed in the auto-generated envelope doc — anything
# inside backticks on the "Known error codes:" line.
grep -m1 "Known error codes" "$TMP/home/protocol/ipman.protocol.envelope.md" \
    | grep -oE '`[a-z_]+`' \
    | tr -d '`' \
    | sort >"$doc_codes"

src_count=$(wc -l <"$src_codes" | tr -d ' ')
doc_count=$(wc -l <"$doc_codes" | tr -d ' ')

if [ "$src_count" = "0" ] || [ "$doc_count" = "0" ]; then
    echo "extraction failed: src=$src_count doc=$doc_count" >&2
    exit 1
fi

if ! diff -u "$src_codes" "$doc_codes"; then
    echo "error code parity failed: src/protocol.c::k_code_strings does not match the envelope doc" >&2
    echo "see comment at src/agent_docs.c:558 — update both places when adding/removing a code" >&2
    exit 1
fi

echo "ok 004_protocol_error_code_parity ($src_count codes)"
