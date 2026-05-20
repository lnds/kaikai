#!/usr/bin/env python3
"""Smoke test for `kai lsp` hover (issue #447).

Drives the language server through initialize + didOpen + hover +
shutdown and asserts the hover result for two positions matches
the inferred kaikai types.

Exit code: 0 on success, 1 on assertion failure.
"""
import json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KAILSP_BIN = os.path.join(ROOT, "tools", "kai-lsp", "kai-lsp")
KAIC2 = os.path.join(ROOT, "stage2", "kaic2")
SRC_FILE = os.path.join(ROOT, "examples", "lsp", "hover_basic.kai")

if not os.path.exists(KAILSP_BIN):
    sys.stderr.write(f"kai-lsp binary not found at {KAILSP_BIN}; build with 'make' first\n")
    sys.exit(2)

with open(SRC_FILE) as f:
    src = f.read()

env = dict(os.environ)
env["KAILSP_KAIC2"] = KAIC2

proc = subprocess.Popen([KAILSP_BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, env=env)

def send(msg):
    body = json.dumps(msg).encode("utf-8")
    proc.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
    proc.stdin.flush()

def recv():
    h = b""
    while b"\r\n\r\n" not in h:
        b = proc.stdout.read(1)
        if not b:
            return None
        h += b
    n = int(h.decode().split("\r\n")[0].split(":", 1)[1].strip())
    return json.loads(proc.stdout.read(n))

send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
init = recv()
assert init["result"]["capabilities"]["hoverProvider"] is True, init

send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
      "params": {"textDocument": {"uri": "file:///tmp/hover_basic.kai",
                                    "languageId": "kaikai", "version": 1, "text": src}}})

# Hover at line 0, col 31 — the `x` in `x + y`. Expect Int.
send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover",
      "params": {"textDocument": {"uri": "file:///tmp/hover_basic.kai"},
                 "position": {"line": 0, "character": 31}}})
h1 = recv()
val1 = h1["result"]["contents"]["value"]
assert "Int" in val1, f"expected Int hover, got {h1!r}"

# Hover at line 3, col 10 — the `add` call. Expect (Int, Int) -> Int.
send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/hover",
      "params": {"textDocument": {"uri": "file:///tmp/hover_basic.kai"},
                 "position": {"line": 3, "character": 10}}})
h2 = recv()
val2 = h2["result"]["contents"]["value"]
assert "Int" in val2 and "->" in val2, f"expected fn hover, got {h2!r}"

send({"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": None})
sh = recv()
assert sh["result"] is None, sh

send({"jsonrpc": "2.0", "method": "exit"})
proc.stdin.close()
proc.wait(timeout=5)

print("hover_basic: OK")
print(f"  position 0:31 (x) -> {val1}")
print(f"  position 3:10 (add) -> {val2}")
