#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Homero Leal
"""MCP server that exposes ipman operations as tools via JSON-RPC 2.0 / LSP framing."""

import json
import os
import subprocess
import sys


def _ipman_home() -> str:
    home = os.environ.get("IPMAN_HOME", "")
    if not home:
        raise RuntimeError("IPMAN_HOME is not set")
    return home


def _ipman_bin() -> str:
    return os.environ.get("IPMAN_BIN", "ipman")


def _load_manifest() -> dict:
    path = os.path.join(_ipman_home(), "manifest.json")
    with open(path) as f:
        return json.load(f)


def _params_schema(op_entry: dict) -> dict:
    schema_path = op_entry.get("request_schema_json", "")
    if not schema_path or not os.path.isfile(schema_path):
        return {"type": "object", "properties": {}}
    with open(schema_path) as f:
        schema = json.load(f)
    return schema.get("properties", {}).get("params", {"type": "object", "properties": {}})


def _build_tool_list(manifest: dict) -> list:
    tools = []
    for op in manifest.get("operations", []):
        name = op["op"]
        params_schema = _params_schema(op)
        tools.append({
            "name": name,
            "description": params_schema.get("description", name),
            "inputSchema": {
                "type": "object",
                "properties": params_schema.get("properties", {}),
                "required": params_schema.get("required", []),
                "additionalProperties": False,
            },
        })
    return tools


def _call_ipman(op: str, arguments: dict, request_id) -> dict:
    payload = {
        "protocol_version": 2,
        "request_id": str(request_id),
        "actor": "mcp",
        "op": op,
        "params": arguments,
    }
    env = {**os.environ, "IPMAN_HOME": _ipman_home()}
    try:
        proc = subprocess.run(
            [_ipman_bin()],
            input=json.dumps(payload).encode("utf-8"),
            capture_output=True,
            env=env,
        )
    except FileNotFoundError:
        return {"isError": True, "content": [{"type": "text", "text": f"ipman binary not found: {_ipman_bin()}"}]}

    if proc.returncode != 0 or not proc.stdout:
        stderr = proc.stderr.decode("utf-8", errors="replace")
        return {"isError": True, "content": [{"type": "text", "text": f"ipman exited {proc.returncode}: {stderr}"}]}

    try:
        response = json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        return {"isError": True, "content": [{"type": "text", "text": f"invalid ipman output: {e}"}]}

    if not response.get("ok"):
        err = response.get("error", {})
        msg = f"{err.get('code', 'error')}: {err.get('message', 'unknown error')}"
        return {"isError": True, "content": [{"type": "text", "text": msg}]}

    result = response.get("result", {})
    return {
        "structuredContent": result,
        "content": [{"type": "text", "text": json.dumps(result)}],
    }


def _read_message() -> dict | None:
    headers: dict[str, str] = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        decoded = line.decode("utf-8", errors="replace").rstrip("\r\n")
        if not decoded:
            if headers:
                break
            continue
        if ":" in decoded:
            key, value = decoded.split(":", 1)
            headers[key.strip().lower()] = value.strip()

    length = int(headers.get("content-length", 0))
    if length <= 0:
        return None
    body = sys.stdin.buffer.read(length)
    return json.loads(body.decode("utf-8"))


def _send_message(msg: dict) -> None:
    body = json.dumps(msg, separators=(",", ":")).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(body)
    sys.stdout.buffer.flush()


def _handle(message: dict, tools: list) -> dict | None:
    method = message.get("method", "")
    msg_id = message.get("id")
    params = message.get("params", {})

    if method == "initialize":
        return {
            "jsonrpc": "2.0",
            "id": msg_id,
            "result": {
                "protocolVersion": "2025-03-26",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "ipman-mcp", "version": "1.0.0"},
            },
        }

    if method == "notifications/initialized":
        return None

    if method == "tools/list":
        return {
            "jsonrpc": "2.0",
            "id": msg_id,
            "result": {"tools": tools},
        }

    if method == "tools/call":
        name = params.get("name", "")
        arguments = params.get("arguments", {})
        call_result = _call_ipman(name, arguments, msg_id)
        return {
            "jsonrpc": "2.0",
            "id": msg_id,
            "result": call_result,
        }

    if msg_id is not None:
        return {
            "jsonrpc": "2.0",
            "id": msg_id,
            "error": {"code": -32601, "message": f"Method not found: {method}"},
        }
    return None


def main() -> None:
    manifest = _load_manifest()
    tools = _build_tool_list(manifest)

    while True:
        message = _read_message()
        if message is None:
            break
        response = _handle(message, tools)
        if response is not None:
            _send_message(response)


if __name__ == "__main__":
    main()
