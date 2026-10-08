# SPDX-License-Identifier: MIT
"""Real daemon/IPC/filesystem integration; Python is a test-only dependency."""
import json, os, pathlib, socket, struct, subprocess, sys, tempfile, time

with tempfile.TemporaryDirectory(prefix="th-") as tmp:
    base = pathlib.Path(tmp)
    root = base / "files"
    root.mkdir()
    (root / "empty").mkdir()
    (root / "hello space.txt").write_bytes(b"abc")
    (root / "unicodé.pdf").write_bytes(b"data")
    os.link(root / "hello space.txt", root / "hard.txt")
    os.symlink("empty", root / "link")
    fd = os.open(os.fsencode(root) + b"/bad\xff.txt", os.O_CREAT | os.O_WRONLY, 0o600)
    os.write(fd, b"x"); os.close(fd)
    cfg = base / "config"
    cfg.write_text(f"root = {root}\nwatch = false\nreconcile_interval_hours = 0\n")
    sockpath = str(base / "run" / "daemon.sock")
    command = [sys.argv[1], "--config", str(cfg), "--database", str(base / "state" / "index.db"), "--socket", sockpath]
    def call(cmd, **kw):
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(5); s.connect(sockpath)
            data = json.dumps(dict(v=1, cmd=cmd, **kw)).encode()
            s.sendall(struct.pack("!I", len(data)) + data)
            def read(n):
                b = b""
                while len(b) < n:
                    part = s.recv(n-len(b))
                    assert part, "unexpected EOF"
                    b += part
                return b
            return json.loads(read(struct.unpack("!I", read(4))[0]))
    def wait_idle():
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                status = call("status")
                if not status["busy"] and status["queued"] == 0: return status
            except (FileNotFoundError, ConnectionRefusedError): pass
            time.sleep(.02)
        raise AssertionError("daemon did not become idle")
    def start():
        proc = subprocess.Popen(command)
        try: wait_idle()
        except BaseException: proc.terminate(); proc.wait(); raise
        return proc
    proc = start()
    try:
        assert os.stat(sockpath).st_mode & 0o777 == 0o600
        roots = call("roots")["roots"]
        assert roots[0]["files"] == 5, roots  # pathname count includes hardlinks and symlink
        r = call("search", query="hello")
        assert r["ok"] and r["used_index"] and len(r["items"]) == 1, r
        assert call("search", query="*.pdf")["items"][0]["name"] == "unicodé.pdf"
        assert call("search", query="bad")["items"][0]["name"] == "bad\udcff.txt"
        assert not call("search", query="\u0000")["ok"]
        assert not call("unknown")["ok"]
        assert not call("rebuild")["ok"]
        assert len(call("search", limit=2)["items"]) == 2
        (root / "hello space.txt").rename(root / "renamed.txt")
        # Cached search must not traverse the filesystem.
        assert len(call("search", query="hello")["items"]) == 1
        seq = call("verify")["seq"]
        assert wait_idle()["completed_seq"] >= seq
        assert not call("search", query="hello")["items"]
        assert call("search", query="renamed")["items"]
        second = subprocess.run(command, capture_output=True, timeout=5)
        assert second.returncode == 3
    finally:
        proc.terminate(); assert proc.wait(timeout=5) == 0
    (root / "offline.txt").write_bytes(b"offline change")
    cfg.write_text(f"root = {root}\nwatch = true\nreconcile_interval_hours = 0\n")
    proc = start()
    def eventually(query, count):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            result = call("search", query=query)["items"]
            if len(result) == count: return result
            time.sleep(.02)
        raise AssertionError(f"live query {query}: expected {count}, got {result}")
    try:
        assert call("status")["live_indexing"]
        assert call("search", query="offline")["items"]
        (root / "live.txt").write_bytes(b"live")
        assert eventually("live.txt", 1)[0]["size"] == 4
        (root / "live.txt").write_bytes(b"longer live data")
        deadline = time.monotonic() + 10
        while call("search", query="live.txt")["items"][0]["size"] != 16:
            assert time.monotonic() < deadline
            time.sleep(.02)
        (root / "newdir").mkdir()
        (root / "newdir" / "nested.txt").write_bytes(b"nested")
        nested_id = eventually("nested.txt", 1)[0]["id"]
        (root / "newdir").rename(root / "moved")
        assert eventually(str(root / "moved" / "nested.txt"), 1)[0]["id"] == nested_id
        (root / "moved" / "nested.txt").unlink()
        eventually("nested.txt", 0)
        (root / "live.txt").unlink()
        eventually("live.txt", 0)
        seq = call("rebuild", root_id=roots[0]["id"])["seq"]
        assert wait_idle()["completed_seq"] >= seq
        assert call("search", query="offline")["items"]
    finally:
        proc.terminate(); assert proc.wait(timeout=5) == 0
print("daemon integration passed")
