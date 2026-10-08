# SPDX-License-Identifier: MIT
"""Real daemon/IPC/filesystem integration; Python is a test-only dependency."""
import json, os, pathlib, signal, socket, struct, subprocess, sys, tempfile, time

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
        assert len(call("list", parent_id=roots[0]["entry_id"])["items"]) == 6
        assert not call("list")["ok"]
        assert not call("treemap")["ok"]
        tree = call("treemap", parent_id=roots[0]["entry_id"], metric="logical")
        assert tree["ok"] and tree["children"] == 6 and len(tree["items"]) == 6 and tree["other_count"] == 0, tree
        assert not call("treemap", parent_id=roots[0]["entry_id"], metric="unknown")["ok"]
        assert call("search", query="hello space.txt", exact=True)["items"][0]["name"] == "hello space.txt"
        assert not call("search", query="hello", exact=True)["items"]
        assert call("search", extension="pdf")["items"][0]["name"] == "unicodé.pdf"
        def cli(*args):
            return subprocess.run([sys.argv[2], *args, "--socket", sockpath], capture_output=True, timeout=10)
        c = cli("search", "bad", "--json")
        assert c.returncode == 0 and json.loads(c.stdout)["items"][0]["name"] == "bad\udcff.txt", c
        assert cli("search", "no-match-ever").returncode == 1
        assert cli("search", "hello", "other").returncode == 2
        assert cli("status", "--wait").returncode == 2
        c = cli("verify", "--wait", "--json")
        assert c.returncode == 0, c
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
        bulk = root / "bulk"; bulk.mkdir()
        for i in range(600): (bulk / f"item-{i}.dat").write_bytes(b"x")
        seq = call("verify")["seq"]; assert wait_idle()["completed_seq"] >= seq
        directory = call("search", query=str(bulk), exact=True)["items"][0]
        tree = call("treemap", parent_id=directory["id"], metric="logical")
        assert tree["children"] == 600 and len(tree["items"]) == 512 and tree["other_count"] == 88 and tree["other_weight"] == 88, tree
        assert tree["total_weight"] == 600
        second = subprocess.run(command, capture_output=True, timeout=5)
        assert second.returncode == 3
    finally:
        proc.terminate(); assert proc.wait(timeout=5) == 0
    (root / "offline.txt").write_bytes(b"offline change")
    other = base / "other"
    other.mkdir(); (other / "deep").mkdir(); (other / "deep" / "overflow-marker.txt").write_bytes(b"old")
    cfg.write_text(f"root = {root}\nroot = {other}\nwatch = true\nreconcile_interval_hours = 0\n")
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
        assert call("mounts")["ok"]
        assert not call("config_save", config="watch = maybe")["ok"]
        saved = call("config_save", config=f"root = {root}\nroot = {other}\nwatch = true\nreconcile_interval_hours = 0\n")
        assert saved["ok"] and not saved["restart_required"]
        wait_idle()
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
        # Exercise the kernel's actual overflow path without changing sysctls.
        limit = int(pathlib.Path("/proc/sys/fs/inotify/max_queued_events").read_text())
        assert limit <= 1000000, "host queue too large for bounded overflow fixture"
        proc.send_signal(signal.SIGSTOP)
        try:
            for i in range(limit + 100):
                path = root / f"overflow-{i}"
                path.touch(); path.unlink()
            (other / "deep" / "overflow-marker.txt").write_bytes(b"changed while queue overflowed")
            (root / "overflow-survivor.txt").write_bytes(b"survives")
        finally:
            proc.send_signal(signal.SIGCONT)
        eventually("overflow-survivor.txt", 1)
        deadline = time.monotonic() + 15
        while call("search", query="overflow-marker.txt")["items"][0]["size"] != 30:
            assert time.monotonic() < deadline, "overflow did not reconcile the other root"
            time.sleep(.02)
        wait_idle()
    finally:
        proc.terminate(); assert proc.wait(timeout=5) == 0
    # An inaccessible root keeps its cached records with an honest Offline state.
    root.rename(base / "disconnected")
    proc = start()
    try:
        assert next(r for r in call("roots")["roots"] if r["path"] == str(root))["status"] == "offline"
        assert call("search", query="overflow-survivor")["items"]
    finally:
        proc.terminate(); assert proc.wait(timeout=5) == 0
    (base / "disconnected").rename(root)
    proc = start()
    proc.kill(); assert proc.wait(timeout=5) == -signal.SIGKILL
    (root / "after-crash.txt").write_bytes(b"recover")
    proc = start()
    try:
        assert call("search", query="after-crash.txt")["items"]
        assert all(r["status"] == "verified" for r in call("roots")["roots"])
        import sqlite3
        with sqlite3.connect(base / "state" / "index.db") as db:
            assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
    finally:
        proc.terminate(); assert proc.wait(timeout=5) == 0
print("daemon integration passed")
