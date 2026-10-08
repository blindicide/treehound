# SPDX-License-Identifier: MIT
"""A metadata I/O failure cannot turn a cached entry into a deletion."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix="treehound-scan-fault-") as directory:
    base = Path(directory)
    root = base / "files"; root.mkdir()
    marker = root / "cached-marker"; marker.write_bytes(b"cached")
    (root / "other").write_bytes(b"other")
    cfg = base / "config"
    cfg.write_text(f"root = {root}\nwatch = true\nreconcile_interval_hours = 0\n")
    endpoint = str(base / "run" / "socket")
    command = [sys.argv[1], "--config", str(cfg), "--database", str(base / "state" / "index"), "--socket", endpoint]
    def call(cmd, **args):
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(3); sock.connect(endpoint)
            data = json.dumps(dict(v=1, cmd=cmd, **args)).encode()
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
                state = call("status")
                if not state["busy"] and not state["queued"]: return
            except (FileNotFoundError, ConnectionRefusedError): pass
            time.sleep(.02)
        raise AssertionError("daemon did not settle")
    proc = subprocess.Popen(command)
    try:
        idle()
        before = call("search", query="cached-marker")["items"][0]
        assert call("roots")["roots"][0]["status"] == "verified"
    finally:
        proc.terminate(); assert proc.wait(timeout=5) == 0
    marker.write_bytes(b"changed offline")
    proc = subprocess.Popen(command, env=dict(os.environ, TREEHOUND_TEST_STAT_FAIL="cached-marker"))
    try:
        idle()
        cached = call("search", query="cached-marker")["items"]
        assert cached, "metadata failure deleted cached record"
        assert cached[0]["id"] == before["id"] and cached[0]["size"] == 6, cached
        assert call("search", query="other")["items"], "partial scan lost sibling"
        current = call("roots")["roots"][0]
        assert current["status"] == "stale", current
        assert call("verify", root_id=current["id"])["ok"]
        idle()
        recovered = call("search", query="cached-marker")["items"][0]
        assert recovered["id"] == before["id"] and recovered["size"] == 15, recovered
        assert call("roots")["roots"][0]["status"] == "verified"
    finally:
        proc.terminate(); assert proc.wait(timeout=5) == 0
