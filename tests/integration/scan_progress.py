# SPDX-License-Identifier: MIT
"""Persisted first-scan detection and live progress through the real daemon IPC."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix="treehound-progress-") as directory:
    base = Path(directory)
    root = base / "files"
    config = base / "config"
    config.write_text(f"root = {root}\nwatch = false\nreconcile_interval_hours = 0\n")
    endpoint = str(base / "run" / "socket")
    command = [sys.argv[1], "--config", str(config), "--database", str(base / "state" / "index.db"), "--socket", endpoint]

    def call(cmd, **args):
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(3)
            connection.connect(endpoint)
            request = json.dumps(dict(v=1, cmd=cmd, **args)).encode()
            connection.sendall(struct.pack("!I", len(request)) + request)

            def read(count):
                result = b""
                while len(result) < count:
                    part = connection.recv(count - len(result))
                    assert part, "daemon closed connection"
                    result += part
                return result

            return json.loads(read(struct.unpack("!I", read(4))[0]))

    def wait_idle():
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            try:
                status = call("status")
                if not status["busy"] and not status["queued"]:
                    return
            except (FileNotFoundError, ConnectionRefusedError):
                pass
            time.sleep(.02)
        raise AssertionError("daemon did not become idle")

    def start():
        process = subprocess.Popen(command)
        wait_idle()
        return process

    process = start()
    try:
        offline = call("roots")["roots"][0]
        assert offline["status"] == "offline" and offline["first_scan"], offline
        assert not offline["scan_active"] and offline["scan_entries"] == 0, offline
        root.mkdir()
        for index in range(1500):
            child = root / f"directory-{index:04d}"
            child.mkdir()
            (child / "item.txt").write_bytes(b"x")
        issued = call("verify", root_id=offline["id"])["seq"]
        deadline = time.monotonic() + 15
        active = None
        while time.monotonic() < deadline:
            current = call("roots")["roots"][0]
            assert current["first_scan"], current
            if current["scan_active"] and current["scan_entries"] > 0:
                active = current
                break
            time.sleep(.01)
        assert active is not None, "no live first-scan progress observed"
        assert active["scan_path"].startswith(str(root)), active
        wait_idle()
        done = call("roots")["roots"][0]
        assert done["status"] == "verified" and not done["first_scan"], done
        assert not done["scan_active"] and done["entry_id"] > 0, done
        assert call("status")["completed_seq"] >= issued
    finally:
        process.terminate()
        assert process.wait(timeout=5) == 0

    process = start()
    try:
        cached = call("roots")["roots"][0]
        assert not cached["first_scan"] and cached["entry_id"] == done["entry_id"], cached
        assert cached["status"] == "verified", cached
        issued = call("rebuild", root_id=cached["id"])["seq"]
        wait_idle()
        rebuilt = call("roots")["roots"][0]
        assert not rebuilt["first_scan"] and rebuilt["status"] == "verified", rebuilt
        assert call("status")["completed_seq"] >= issued
    finally:
        process.terminate()
        assert process.wait(timeout=5) == 0

print("Fresh, live and cached scan progress integration passed")
