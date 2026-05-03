#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-doc-examples.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

IPMAN_HOME="$TMP/home" "$BIN" init >/dev/null

TMP="$TMP" python3 - <<'PY'
import glob
import json
import os
import re
import sys

home = os.path.join(os.environ["TMP"], "home")
errors = []

for path in sorted(glob.glob(os.path.join(home, "examples", "ipman.op.*.input.example.md"))):
    name = os.path.basename(path)[len("ipman.op."):-len(".input.example.md")]
    text = open(path, encoding="utf-8").read()
    match = re.search(r"```json\n(.*?)\n```", text, re.S)
    if not match:
        errors.append(f"{name}: missing json block")
        continue

    example = json.loads(match.group(1))
    schema_path = os.path.join(home, "schemas", f"ipman.op.{name}.request.schema.json")
    schema = json.load(open(schema_path, encoding="utf-8"))
    params_schema = schema["properties"]["params"]

    allowed = set(params_schema.get("properties", {}))
    used = set(example.get("params", {}))
    unknown = used - allowed
    if unknown:
        errors.append(f"{name}: example uses unknown params {sorted(unknown)}")

    direct_required = set(params_schema.get("required", []))
    missing = direct_required - used
    if missing:
        errors.append(f"{name}: example misses schema required {sorted(missing)}")

if errors:
    print("\n".join(errors))
    sys.exit(1)
PY
