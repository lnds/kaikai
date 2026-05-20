#!/usr/bin/env python3
"""Smoke test for goto-definition (issue #447 v2).

Drives the language server through initialize + didOpen + definition
and asserts the response points at the function's `fn` line.
"""
import json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KAILSP_BIN = os.path.join(ROOT, "tools", "kai-lsp", "kai-lsp")
KAIC2 = os.path.join(ROOT, "stage2", "kaic2")
SRC_FILE = os.path.join(ROOT, "examples", "lsp", "hover_basic.kai")

if not os.path.exists(KAILSP_BIN):
    sys.stderr.write(f"kai-lsp binary not found at {KAILSP_BIN}\n")
    sys.exit(2)

with open(SRC_FILE) as f:
    src = f.read()

env = dict(os.environ)
env["KAILSP_KAIC2"] = KAIC2

proc = subprocess.Popen([KAILSP_BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, env=env)

def send(msg):
    body = json.dumps(msg).encode()
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

send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})
init = recv()
assert init["result"]["capabilities"]["definitionProvider"] is True, init

send({"jsonrpc":"2.0","method":"initialized","params":{}})
URI = "file:///tmp/goto_def.kai"
send({"jsonrpc":"2.0","method":"textDocument/didOpen",
      "params":{"textDocument":{"uri":URI,"languageId":"kaikai","version":1,"text":src}}})
recv()  # publishDiagnostics

# `add` call at source line 4 col 11 (1-indexed): "  let z = add(3, 4)"
# 0-indexed LSP position: line=3, character=10
send({"jsonrpc":"2.0","id":2,"method":"textDocument/definition",
      "params":{"textDocument":{"uri":URI}, "position":{"line":3,"character":10}}})
r = recv()
result = r["result"]
assert isinstance(result, list) and len(result) == 1, r
loc = result[0]
assert loc["uri"] == URI, loc
# `add` is defined at source line 1 col 1 (1-indexed) -> 0:0 in LSP.
assert loc["range"]["start"]["line"] == 0, loc
assert loc["range"]["start"]["character"] == 0, loc

send({"jsonrpc":"2.0","id":3,"method":"shutdown","params":None})
recv()
send({"jsonrpc":"2.0","method":"exit"})
proc.stdin.close()
proc.wait(timeout=5)

print("goto_def: OK")
print(f"  add call @ 3:10 -> {loc['uri']} line {loc['range']['start']['line']}")
