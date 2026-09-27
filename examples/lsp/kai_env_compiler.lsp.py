#!/usr/bin/env python3
"""`kai lsp` reaches the compiler through the `kai env` variables alone.

Runs the server through `bin/kai lsp` with neither KAILSP_KAIC2 nor
KAI_KAIC2 in the environment and no kaic2 on PATH, then asserts a hover
answers: the only way the server can find the compiler is the KAI_KAIC2
that `kai lsp` exports.

Exit code: 0 on success, 1 on assertion failure.
"""
import json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KAI = os.path.join(ROOT, "bin", "kai")
SRC_FILE = os.path.join(ROOT, "examples", "lsp", "hover_basic.kai")

with open(SRC_FILE) as f:
    src = f.read()

env = {k: v for k, v in os.environ.items() if k not in ("KAILSP_KAIC2", "KAI_KAIC2")}
env["PATH"] = "/usr/bin:/bin"

proc = subprocess.Popen([KAI, "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
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
assert init and init["result"]["capabilities"]["hoverProvider"] is True, init

send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
      "params": {"textDocument": {"uri": "file:///tmp/hover_basic.kai",
                                    "languageId": "kaikai", "version": 1, "text": src}}})
diag_notif = recv()
assert diag_notif.get("method") == "textDocument/publishDiagnostics", diag_notif

send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover",
      "params": {"textDocument": {"uri": "file:///tmp/hover_basic.kai"},
                 "position": {"line": 0, "character": 31}}})
h1 = recv()
assert h1.get("result"), f"no hover: the server did not reach kaic2 ({h1!r})"
val1 = h1["result"]["contents"]["value"]
assert "Int" in val1, f"expected Int hover, got {h1!r}"

send({"jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": None})
recv()
send({"jsonrpc": "2.0", "method": "exit"})
proc.stdin.close()
proc.wait(timeout=5)

print("kai_env_compiler: OK")
