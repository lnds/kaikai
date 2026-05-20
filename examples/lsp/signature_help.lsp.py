#!/usr/bin/env python3
"""Smoke test for textDocument/signatureHelp (issue #447 v3)."""
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

src = """fn add(x: Int, y: Int) : Int = x + y

fn main() : Unit / Stdout = {
  print(int_to_string(add(3, 4)))
}
"""

send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})
init = recv()
caps = init["result"]["capabilities"]
assert "signatureHelpProvider" in caps, caps

send({"jsonrpc":"2.0","method":"initialized","params":{}})
URI = "file:///tmp/sighelp.kai"
send({"jsonrpc":"2.0","method":"textDocument/didOpen",
      "params":{"textDocument":{"uri":URI,"languageId":"kaikai","version":1,"text":src}}})
recv()  # publishDiagnostics

# Position the cursor inside `add(3, 4)` — line 3 col 25 (0-indexed)
# is just past `add(`. The walker returns the innermost enclosing
# call, which is `add` here.
send({"jsonrpc":"2.0","id":2,"method":"textDocument/signatureHelp",
      "params":{"textDocument":{"uri":URI},"position":{"line":3,"character":25}}})
r = recv()
result = r["result"]
assert result is not None, r
sigs = result["signatures"]
assert len(sigs) == 1, result
label = sigs[0]["label"]
assert "add" in label, label
assert "Int" in label, label

# Also exercise the outer `int_to_string` call by pointing the
# cursor between `int_to_string(` and `add(`.
send({"jsonrpc":"2.0","id":3,"method":"textDocument/signatureHelp",
      "params":{"textDocument":{"uri":URI},"position":{"line":3,"character":21}}})
r2 = recv()
label2 = r2["result"]["signatures"][0]["label"]
assert "int_to_string" in label2, label2

send({"jsonrpc":"2.0","id":99,"method":"shutdown","params":None}); recv()
send({"jsonrpc":"2.0","method":"exit"})
proc.stdin.close(); proc.wait(timeout=5)

print("signature_help: OK")
print(f"  inside add(...)         -> {label}")
print(f"  inside int_to_string(...) -> {label2}")
