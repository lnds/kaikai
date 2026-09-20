"""Observe redirect credentials on the wire using two loopback origins."""
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from contextlib import ExitStack
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
SECRETS = {"authorization", "proxy-authorization", "cookie", "cookie2"}


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        self.respond()

    def do_GET(self):
        self.respond()

    def respond(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.server.requests.append((self.command, self.path, self.headers, body))
        status, location = self.server.routes.get(self.path, (200, None))
        self.send_response(status)
        if location is not None:
            self.send_header("Location", location)
        self.send_header("Content-Length", "2")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(b"ok")

    def log_message(self, *args):
        pass


def start_server(stack):
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.requests = []
    server.routes = {}
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    stack.callback(server.server_close)
    stack.callback(thread.join, 5)
    stack.callback(server.shutdown)
    return server


def check_headers(request, secret, authority):
    _, _, headers, _ = request
    for name in SECRETS:
        values = headers.get_all(name, [])
        assert bool(values) == secret, (name, values, secret)
    assert headers.get_all("Host") == [authority], headers.get_all("Host")
    assert headers["X-Trace"] == "keep-me"
    if secret:
        assert headers.get_all("Authorization") == ["secret-a", "secret-b"]


def run_case(binary, first, second, label, destination, secret, status=307):
    first.requests.clear()
    second.requests.clear()
    first.routes = {"/start": (status, destination)}
    second.routes = {}
    start = f"http://localhost:{first.server_port}/start"
    result = subprocess.run([str(binary), start, "POST"], capture_output=True,
                            text=True, timeout=15, check=True)
    assert result.stdout.strip() == "ok", result.stdout
    assert len(first.requests) + len(second.requests) == 2
    assert first.requests[0][2]["Authorization"] == "secret-a"
    target = second.requests[-1] if second.requests else first.requests[-1]
    authority = destination.split("/")[2] if "://" in destination else f"localhost:{first.server_port}"
    if destination.startswith("//"):
        authority = destination.split("/")[2]
    check_headers(target, secret, authority)
    assert target[1] == "/end", target[1]
    expected = ("GET", b"") if status == 302 else ("POST", b"payload")
    assert (target[0], target[3]) == expected, (label, target)
    print(f"redirect {label}: ok")


def check_chain(binary, first, second):
    first.requests.clear()
    second.requests.clear()
    home = f"http://localhost:{first.server_port}"
    away = f"http://localhost:{second.server_port}"
    first.routes = {"/start": (307, away + "/away")}
    second.routes = {"/away": (308, home + "/back")}
    subprocess.run([str(binary), home + "/start", "POST"], check=True,
                   capture_output=True, timeout=15)
    assert len(first.requests) == 2 and len(second.requests) == 1
    check_headers(second.requests[0], False, f"localhost:{second.server_port}")
    check_headers(first.requests[1], False, f"localhost:{first.server_port}")
    print("redirect return to original origin: ok")


def main():
    driver = os.environ.get("KAI_TEST_DRIVER", str(ROOT / "bin/kai"))
    backend = os.environ.get("KAI_TEST_BACKEND", "c")
    env = dict(os.environ)
    env.setdefault("KAI_STDLIB", str(ROOT / "stdlib"))
    with tempfile.TemporaryDirectory() as work, ExitStack() as stack:
        binary = Path(work) / "redirect-client"
        subprocess.run([driver, "build", f"--backend={backend}",
                        str(ROOT / "tests/stdlib/http_redirect_client.kai"),
                        "-o", str(binary)], env=env, check=True, timeout=180)
        first = start_server(stack)
        second = start_server(stack)
        port = first.server_port
        cases = [
            ("different host", f"http://127.0.0.1:{port}/end", False, 307),
            ("same origin path", "/end", True, 307),
            ("host case", f"http://LOCALHOST:{port}/end", True, 308),
            ("different port", f"http://localhost:{second.server_port}/end", False, 308),
            ("network path", f"//127.0.0.1:{second.server_port}/end", False, 307),
            ("POST rewrite", f"http://localhost:{second.server_port}/end", False, 302),
        ]
        for case in cases:
            run_case(binary, first, second, *case)
        check_chain(binary, first, second)


if __name__ == "__main__":
    main()
