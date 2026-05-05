#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-doc-parity.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

IPMAN_HOME="$TMP/home" "$BIN" init >/dev/null 2>/dev/null

dispatch_ops="$TMP/dispatch.ops"
spec_ops="$TMP/spec.ops"
manifest_ops="$TMP/manifest.ops"
operation_doc_ops="$TMP/operation-doc.ops"
request_schema_ops="$TMP/request-schema.ops"

sed -n '/static const ipman_op_t k_ops\[\]/,/^};/p' "$ROOT/src/dispatch.c" \
    | awk -F'"' '/^[[:space:]]*\{ "/ { print $2 }' \
    | sort >"$dispatch_ops"

sed -n '/static const OperationSpec k_operation_specs\[\]/,/^};/p' "$ROOT/src/agent_docs.c" \
    | awk -F'"' '/^[[:space:]]*\{ "/ { print $2 }' \
    | sort >"$spec_ops"

jq -r '.operations[].op' "$TMP/home/manifest.json" \
    | sort >"$manifest_ops"

find "$TMP/home/operations" -name 'ipman.op.*.schema.md' -print \
    | sed -E 's#.*/ipman\.op\.(.*)\.schema\.md#\1#' \
    | sort >"$operation_doc_ops"

find "$TMP/home/schemas" -name 'ipman.op.*.request.schema.json' -print \
    | sed -E 's#.*/ipman\.op\.(.*)\.request\.schema\.json#\1#' \
    | sort >"$request_schema_ops"

assert_same_ops() {
    label=$1
    actual=$2
    if ! diff -u "$dispatch_ops" "$actual"; then
        echo "$label operation set differs from dispatch registry" >&2
        exit 1
    fi
}

assert_same_ops "OperationSpec" "$spec_ops"
assert_same_ops "manifest" "$manifest_ops"
assert_same_ops "operation docs" "$operation_doc_ops"
assert_same_ops "request schemas" "$request_schema_ops"

op_count=$(wc -l <"$dispatch_ops" | tr -d ' ')
jq -e --argjson n "$op_count" '
  (.operations | length) == $n
  and all(.operations[]; (.schema_doc | type == "string")
      and (.example_doc | type == "string")
      and (.request_schema_json | type == "string"))
' "$TMP/home/manifest.json" >/dev/null

find "$TMP/home/schemas" -name '*.json' -print0 \
    | xargs -0 -n1 jq empty

# A freshly initialized workspace must have ZERO files in the generated subdirs
# beyond what the manifest tracks. Catches future drift where a new artifact
# kind starts being written to disk but is not added to manifest.files[], or
# where stale files survive a refresh because sweep_orphans missed a directory.
disk_files="$TMP/disk.files"
manifest_files="$TMP/manifest.files"
gendirs_re="/(protocol|concepts|entities|operations|examples|workflows|indexes|schemas|guides)/"

find "$TMP/home/protocol" "$TMP/home/concepts" "$TMP/home/entities" \
     "$TMP/home/operations" "$TMP/home/examples" "$TMP/home/workflows" \
     "$TMP/home/indexes" "$TMP/home/schemas" "$TMP/home/guides" \
     -type f -print 2>/dev/null \
    | sort >"$disk_files"

jq -r --arg re "$gendirs_re" '.files[] | select(test($re))' "$TMP/home/manifest.json" \
    | sort >"$manifest_files"

if ! diff -u "$manifest_files" "$disk_files"; then
    echo "freshly initialized workspace has files not tracked in manifest" >&2
    exit 1
fi
