#!/usr/bin/env python3
"""Smoke test for hole diagnostics (issue #447 v3.1).

Unfilled holes (`?` and `?name`) surface in publishDiagnostics as
warnings carrying the inferred expected type. This lets editors
underline holes inline and feed agents the type they need to
generate code for.
"""
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

# One named hole + one anonymous hole. Both reach the typer because
# their containing functions are well-typed except for the hole body.
src = """fn celsius_to_fahrenheit(c: Real) : Real = ?conversion
fn fahrenheit_to_celsius(f: Real) : Real = ?
"""

send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}); recv()
send({"jsonrpc":"2.0","method":"initialized","params":{}})
URI = "file:///tmp/hole_diags.kai"
send({"jsonrpc":"2.0","method":"textDocument/didOpen",
      "params":{"textDocument":{"uri":URI,"languageId":"kaikai","version":1,"text":src}}})
diag = recv()
diags = diag["params"]["diagnostics"]

assert len(diags) == 2, diags

named = next((d for d in diags if "?conversion" in d["message"]), None)
anon = next((d for d in diags if d["message"] == "unfilled hole ?: expected Real"), None)

assert named, f"named hole diagnostic missing: {diags}"
assert anon, f"anonymous hole diagnostic missing: {diags}"

for d in (named, anon):
    assert d["severity"] == 2, d                              # Warning
    assert d["source"] == "kaikai", d
    assert "Real" in d["message"], d
    # Hole position is non-trivial (line/character > 0 since the
    # holes don't sit at the start of the file).
    assert d["range"]["start"]["character"] > 0, d

send({"jsonrpc":"2.0","id":99,"method":"shutdown"}); recv()
send({"jsonrpc":"2.0","method":"exit"})
proc.stdin.close(); proc.wait(timeout=5)

print("hole_diagnostics: OK")
print(f"  named hole -> {named['message']}")
print(f"  anon hole  -> {anon['message']}")
