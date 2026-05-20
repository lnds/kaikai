#!/usr/bin/env python3
"""Smoke test for textDocument/documentSymbol (issue #447 v2)."""
import json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KAILSP_BIN = os.path.join(ROOT, "tools", "kai-lsp", "kai-lsp")
KAIC2 = os.path.join(ROOT, "stage2", "kaic2")

if not os.path.exists(KAILSP_BIN):
    sys.stderr.write(f"kai-lsp binary not found at {KAILSP_BIN}\n")
    sys.exit(2)

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

src = """fn add(x: Int, y: Int) : Int = x + y

fn double(n: Int) : Int = n * 2

fn main() {
  let z = add(double(3), 4)
  z
}
"""

send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})
init = recv()
assert init["result"]["capabilities"]["documentSymbolProvider"] is True, init

send({"jsonrpc":"2.0","method":"initialized","params":{}})
URI = "file:///tmp/symbols.kai"
send({"jsonrpc":"2.0","method":"textDocument/didOpen",
      "params":{"textDocument":{"uri":URI,"languageId":"kaikai","version":1,"text":src}}})
recv()  # publishDiagnostics

send({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol",
      "params":{"textDocument":{"uri":URI}}})
r = recv()
syms = r["result"]
names = [s["name"] for s in syms]
assert "add" in names, names
assert "double" in names, names
assert "main" in names, names
# All should be Function kind (12).
assert all(s["kind"] == 12 for s in syms), syms

send({"jsonrpc":"2.0","id":3,"method":"shutdown","params":None})
recv()
send({"jsonrpc":"2.0","method":"exit"})
proc.stdin.close()
proc.wait(timeout=5)

print("document_symbols: OK")
print(f"  symbols: {names}")
