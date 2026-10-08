# SPDX-License-Identifier: MIT
"""Actual GTK window, async indexed listing and debounced search under Xvfb."""
import os, pathlib, subprocess, sys, tempfile, time
with tempfile.TemporaryDirectory(prefix="th-gui-") as tmp:
    base = pathlib.Path(tmp)
    root = base / "files"; root.mkdir()
    (root / "smoke.txt").write_text("GTK smoke fixture")
    (root / "subdir").mkdir(); (root / "subdir" / "nested-smoke.txt").write_text("nested")
    cfg = base / "config" / "treehound"; cfg.mkdir(parents=True)
    (cfg / "treehound.conf").write_text(f"root = {root}\nwatch = true\nreconcile_interval_hours = 0\n")
    runtime = base / "runtime"; runtime.mkdir(mode=0o700)
    env = dict(os.environ, XDG_CONFIG_HOME=str(base / "config"), XDG_STATE_HOME=str(base / "state"), XDG_RUNTIME_DIR=str(runtime), TREEHOUND_GUI_SMOKE="1", GTK_A11Y="none")
    daemon = subprocess.Popen([sys.argv[1]], env=env, stderr=subprocess.PIPE)
    try:
        for _ in range(100):
            p = subprocess.run([sys.argv[2], "status", "--json"], env=env, capture_output=True)
            if p.returncode == 0: break
            time.sleep(.02)
        time.sleep(.1)
        gui = subprocess.run(["xvfb-run", "-a", "dbus-run-session", "--", sys.argv[2]], env=env, capture_output=True, timeout=20)
        assert gui.returncode == 0, (gui.returncode, gui.stdout, gui.stderr)
        assert b"CRITICAL" not in gui.stderr and b"WARNING" not in gui.stderr, gui.stderr
    finally:
        daemon.terminate(); assert daemon.wait(timeout=5) == 0
print("GTK browse/search integration passed")
