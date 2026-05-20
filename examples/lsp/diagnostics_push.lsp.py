#!/usr/bin/env python3
"""Smoke test for publishDiagnostics (issue #447 v2).

didOpen on a source with a type mismatch should emit a
publishDiagnostics notification with severity 1 (Error). didChange
to a clean source should emit an empty diagnostics array to clear
the marker.
"""
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

bad = """fn double(x: Int) : Int = x * 2

fn main() : Int / Console {
  print(int_to_string(double("not an int")))
  0
}
"""
good = "fn main() : Unit / Console = print(\"ok\")\n"

send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})
recv()
send({"jsonrpc":"2.0","method":"initialized","params":{}})

URI = "file:///tmp/diag.kai"
send({"jsonrpc":"2.0","method":"textDocument/didOpen",
      "params":{"textDocument":{"uri":URI,"languageId":"kaikai","version":1,"text":bad}}})
d1 = recv()
assert d1["method"] == "textDocument/publishDiagnostics", d1
diags = d1["params"]["diagnostics"]
assert len(diags) >= 1, d1
assert diags[0]["severity"] == 1, diags  # Error
assert diags[0]["source"] == "kaikai", diags

send({"jsonrpc":"2.0","method":"textDocument/didChange",
      "params":{"textDocument":{"uri":URI,"version":2},
                "contentChanges":[{"text":good}]}})
d2 = recv()
assert d2["method"] == "textDocument/publishDiagnostics", d2
assert d2["params"]["diagnostics"] == [], d2

send({"jsonrpc":"2.0","id":3,"method":"shutdown","params":None})
recv()
send({"jsonrpc":"2.0","method":"exit"})
proc.stdin.close()
proc.wait(timeout=5)

print("diagnostics_push: OK")
print(f"  bad source -> {len(diags)} diagnostic(s), severity={diags[0]['severity']}")
print(f"  fixed source -> diagnostics cleared")
