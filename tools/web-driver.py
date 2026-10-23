#!/usr/bin/env python3
"""Load a page in a headless browser through its WebDriver and print the text of #kai-stdout.

usage: web-driver.py <chrome|firefox> <url> [steps.js]

A steps file holds scripts separated by lines of `//--`; each runs in the page once the
output has settled, and the output settles again before the next.
"""
import json
import shutil
import subprocess
import sys
import time
import urllib.request

DRIVERS = {"chrome": "chromedriver", "firefox": "geckodriver"}
CAPS = {
    "chrome": {"browserName": "chrome", "goog:chromeOptions": {"args": ["--headless=new", "--no-sandbox", "--disable-gpu"]}},
    "firefox": {"browserName": "firefox", "moz:firefoxOptions": {"args": ["-headless"]}},
}
PORT = 9515
SETTLE_SECONDS = 1.5
STEP_SETTLE_SECONDS = 0.6
DEADLINE_SECONDS = 30


def call(method, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", data=data, method=method)
    req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)["value"]


def wait_ready(proc):
    for _ in range(300):
        if proc.poll() is not None:
            sys.exit("web-driver: the driver exited early")
        try:
            call("GET", "/status")
            return
        except OSError:
            time.sleep(0.1)
    sys.exit("web-driver: the driver did not come up")


def page_text(sid):
    script = "var e = document.getElementById('kai-stdout'); return e ? e.textContent : null;"
    return call("POST", f"/session/{sid}/execute/sync", {"script": script, "args": []})


def run_script(sid, script):
    call("POST", f"/session/{sid}/execute/sync", {"script": script, "args": []})


def settled_text(sid, settle=SETTLE_SECONDS):
    last, since = "", time.time()
    deadline = time.time() + DEADLINE_SECONDS
    while time.time() < deadline:
        text = page_text(sid) or ""
        if text != last:
            last, since = text, time.time()
        elif last and time.time() - since >= settle:
            return last
        time.sleep(0.2)
    return last


def main():
    browser, url = sys.argv[1], sys.argv[2]
    steps = open(sys.argv[3]).read().split("\n//--\n") if len(sys.argv) > 3 else []
    exe = shutil.which(DRIVERS[browser])
    if exe is None:
        sys.exit(f"web-driver: {DRIVERS[browser]} not found")
    proc = subprocess.Popen([exe, f"--port={PORT}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_ready(proc)
        sid = call("POST", "/session", {"capabilities": {"alwaysMatch": CAPS[browser]}})["sessionId"]
        try:
            call("POST", f"/session/{sid}/url", {"url": url})
            text = settled_text(sid)
            for step in steps:
                run_script(sid, step)
                text = settled_text(sid, STEP_SETTLE_SECONDS)
            sys.stdout.write(text)
        finally:
            call("DELETE", f"/session/{sid}")
    finally:
        proc.terminate()
        proc.wait()


main()
