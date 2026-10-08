# SPDX-License-Identifier: MIT
"""Real daemon/index/inotify behavior after one injected ENOSPC syscall."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix="treehound-watch-fault-") as directory:
    base = Path(directory)
    bad, good = base / "bad", base / "good"
    bad.mkdir(); good.mkdir()
    child = bad / "child"; child.mkdir()
    file = child / "marker"; file.write_bytes(b"x")
    cfg = base / "config"
    cfg.write_text(f"root = {bad}\nroot = {good}\nwatch = true\nreconcile_interval_hours = 0\n")
    endpoint = str(base / "run" / "socket")
    env = dict(os.environ, TREEHOUND_TEST_WATCH_FAIL=str(child))
    proc = subprocess.Popen([sys.argv[1], "--config", str(cfg), "--database", str(base / "state" / "index"), "--socket", endpoint], env=env)
    def call(command, **args):
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(3); sock.connect(endpoint)
            data = json.dumps(dict(v=1, cmd=command, **args)).encode()
            sock.sendall(struct.pack("!I", len(data)) + data)
            def read(count):
                result = b""
                while len(result) < count:
                    part = sock.recv(count - len(result))
                    assert part, "closed socket"
                    result += part
                return result
            return json.loads(read(struct.unpack("!I", read(4))[0]))
    def idle():
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                s = call("status")
                if not s["busy"] and not s["queued"]: return
            except (FileNotFoundError, ConnectionRefusedError): pass
            time.sleep(.02)
        raise AssertionError("daemon did not settle")
    try:
        idle()
        roots = {r["path"]: r for r in call("roots")["roots"]}
        assert roots[str(bad)]["status"] == "stale", roots
        assert roots[str(good)]["status"] == "verified", roots
        assert call("verify", root_id=roots[str(bad)]["id"])["ok"]
        idle()
        roots = {r["path"]: r for r in call("roots")["roots"]}
        assert roots[str(bad)]["status"] == "verified", roots
        file.write_bytes(b"recovered")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            items = call("search", query="marker")["items"]
            if items and items[0]["size"] == 9: break
            time.sleep(.02)
        else: raise AssertionError("recovered watch did not update index")
    finally:
        proc.terminate()
        assert proc.wait(timeout=5) == 0
