#!/bin/sh
set -eu

# README op-count parity: any "<N> operations" / "<N> ops" claim in
# README.md must match the canonical op count from the binary.
#
# Why: the README contains six magic numbers describing the surface
# size, and one of them (in the MCP section) drifted from 65 to 76 over
# v2.3 → v2.4 without anyone updating the prose. v2.4.2 shipped with
# that drift still present. This test grounds README claims against the
# binary's actual count so future drift fails the build.
#
# What counts as a claim? Anything matching the regex
#   <N>(\s+|-)(operations?|ops)
# in README.md. We exclude lines that are clearly NOT claims about the
# total surface size — phrases like "(2 ops)" inside code-flow examples
# describe a count of ops in a specific composition, not the registry
# size, and live alongside the canonical claims without contradicting
# them.

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-readme-count.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

IPMAN_HOME="$TMP/home" "$BIN" init >/dev/null 2>/dev/null

# Canonical count from the manifest, excluding `noop` (a smoke-test
# liveness check that is not part of the documented op surface; the
# README's claims describe what agents call, and noop is internal).
canonical=$(jq '[.operations[] | select(.op != "noop")] | length' "$TMP/home/manifest.json")

if [ -z "$canonical" ] || [ "$canonical" = "null" ]; then
    echo "could not read op count from manifest" >&2
    exit 1
fi

# Extract every README claim that names a total-surface count. Two grammars:
#   1. "<N> operations"  / "<N> ops"        (e.g. "76 operations across…")
#   2. "<N> of them"                         (used in the MCP paragraph)
# Compositional counts inside parenthetical examples ("(2 ops)", "(~8 ops)")
# are filtered out — they describe call sequences, not the registry size.
claims="$TMP/claims"
# Capture only the number immediately preceding the surface-count phrases,
# so README line numbers from grep -n are not mistaken for claims.
grep -oE '[0-9]+ (ops|operations) (after|across|registered)|[0-9]+ of them after `ipman init`|exposes the [0-9]+ ops' "$ROOT/README.md" \
    | grep -oE '[0-9]+' \
    | sort -u >"$claims"

if [ ! -s "$claims" ]; then
    echo "no README claims matched the surface-count grammars; test pattern may be stale" >&2
    exit 1
fi

# Every distinct number found must equal the canonical count.
fail=0
while IFS= read -r n; do
    if [ "$n" != "$canonical" ]; then
        echo "README claims $n ops/operations but the binary registers $canonical" >&2
        fail=1
    fi
done <"$claims"

if [ "$fail" = "1" ]; then
    echo "see grep -nE '[0-9]+ (ops|operations)|[0-9]+ of them' README.md to locate the drift" >&2
    exit 1
fi

claim_count=$(wc -l <"$claims" | tr -d ' ')
echo "ok 005_readme_count_parity ($claim_count distinct claims, all = $canonical)"
