#!/usr/bin/env python3
"""Smoke test for textDocument/completion (issue #447 v3)."""
import json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KAILSP_BIN = os.path.join(ROOT, "tools", "kai-lsp", "kai-lsp")
KAIC2 = os.path.join(ROOT, "stage2", "kaic2")

if not os.path.exists(KAILSP_BIN):
    sys.stderr.write(f"kai-lsp binary not found at {KAILSP_BIN}\n")
    sys.exit(2)

env = dict(os.environ); env["KAILSP_KAIC2"] = KAIC2
proc = subprocess.Popen([KAILSP_BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)

def send(msg):
    body = json.dumps(msg).encode()
    proc.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body); proc.stdin.flush()
def recv():
    h = b""
    while b"\r\n\r\n" not in h:
        b = proc.stdout.read(1)
        if not b: return None
        h += b
    n = int(h.decode().split("\r\n")[0].split(":", 1)[1].strip())
    return json.loads(proc.stdout.read(n))

src = "fn greet(name: String) : Unit / Stdout = print(string_concat(\"hi \", name))\n"

send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})
init = recv()
caps = init["result"]["capabilities"]
assert "completionProvider" in caps, caps

send({"jsonrpc":"2.0","method":"initialized","params":{}})
URI = "file:///tmp/completion.kai"
send({"jsonrpc":"2.0","method":"textDocument/didOpen",
      "params":{"textDocument":{"uri":URI,"languageId":"kaikai","version":1,"text":src}}})
recv()  # publishDiagnostics

send({"jsonrpc":"2.0","id":2,"method":"textDocument/completion",
      "params":{"textDocument":{"uri":URI},"position":{"line":0,"character":50}}})
r = recv()
items = r["result"]
labels = {it["label"] for it in items}

# User-defined fn shows up
assert "greet" in labels, "greet missing"
# Prelude builtins
for needed in ("print", "string_concat", "int_to_string", "read_line"):
    assert needed in labels, f"prelude `{needed}` missing"
# Stdlib core fn
assert "map" in labels, "stdlib `map` missing"

# Detail strings must include the function arrow.
greet = next(it for it in items if it["label"] == "greet")
assert "->" in greet["detail"], greet

send({"jsonrpc":"2.0","id":3,"method":"shutdown","params":None}); recv()
send({"jsonrpc":"2.0","method":"exit"})
proc.stdin.close(); proc.wait(timeout=5)

print("completion: OK")
print(f"  total candidates: {len(items)}")
print(f"  greet detail: {greet['detail']}")
