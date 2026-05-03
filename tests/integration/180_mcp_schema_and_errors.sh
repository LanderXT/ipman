#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-mcp.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

IPMAN_HOME="$TMP/home" "$BIN" init >/dev/null

PYTHONDONTWRITEBYTECODE=1 IPMAN_HOME="$TMP/home" IPMAN_BIN="$BIN" ROOT="$ROOT" TMP="$TMP" python3 - <<'PY'
import os
import sys

sys.path.insert(0, os.path.join(os.environ["ROOT"], "mcp"))
import ipman_mcp

tools = ipman_mcp._build_tool_list(ipman_mcp._load_manifest())
by_name = {tool["name"]: tool for tool in tools}

task_defer = by_name["task.defer"]["inputSchema"]
assert "allOf" in task_defer, "MCP inputSchema dropped task.defer reason alternatives"

empty_home = os.path.join(os.environ["TMP"], "empty-home")
os.makedirs(empty_home, exist_ok=True)
old_home = os.environ["IPMAN_HOME"]
try:
    os.environ["IPMAN_HOME"] = empty_home
    result = ipman_mcp._call_ipman("noop", {}, "fatal-smoke")
finally:
    os.environ["IPMAN_HOME"] = old_home

assert result.get("isError") is True, result
text = result["content"][0]["text"]
assert "internal_error" in text and "not initialized" in text, text
PY
