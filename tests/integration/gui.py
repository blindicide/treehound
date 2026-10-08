# SPDX-License-Identifier: MIT
"""Actual GTK window, async indexed browsing, treemap navigation and history capture under Xvfb."""
import base64, configparser, os, re, pathlib, subprocess, sys, tempfile, time
with tempfile.TemporaryDirectory(prefix="th-gui-") as tmp:
    base = pathlib.Path(tmp)
    root = base / "files"; root.mkdir()
    (root / "smoke.txt").write_text("GTK smoke fixture")
    (root / "subdir").mkdir(); (root / "subdir" / "nested-smoke.txt").write_text("nested")
    cfg = base / "config" / "treehound"; cfg.mkdir(parents=True)
    (cfg / "treehound.conf").write_text(f"root = {root}\nwatch = true\nreconcile_interval_hours = 0\n")
    runtime = base / "runtime"; runtime.mkdir(mode=0o700)
    env = dict(os.environ, XDG_CONFIG_HOME=str(base / "config"), XDG_STATE_HOME=str(base / "state"), XDG_RUNTIME_DIR=str(runtime), TREEHOUND_GUI_SMOKE="1", GTK_A11Y="none", GSK_RENDERER="cairo", GTK_USE_PORTAL="0", GSETTINGS_BACKEND="memory")
    daemon = subprocess.Popen([sys.argv[1]], env=env, stderr=subprocess.PIPE)
    try:
        for _ in range(100):
            p = subprocess.run([sys.argv[2], "status", "--json"], env=env, capture_output=True)
            if p.returncode == 0: break
            time.sleep(.02)
        time.sleep(.1)
        gui = subprocess.run(["xvfb-run", "-a", "dbus-run-session", "--", sys.argv[2]], env=env, capture_output=True, timeout=20)
        assert gui.returncode == 0, (gui.returncode, gui.stdout, gui.stderr)
        prefs=configparser.ConfigParser();prefs.read(cfg / "gui.conf")
        assert base64.b64decode(prefs["navigation"]["bookmarks"].rstrip(';')).decode()==str(root)
        again=subprocess.run(["xvfb-run","-a","dbus-run-session","--",sys.argv[2]],env=env,capture_output=True,timeout=20)
        assert again.returncode==0,(again.returncode,again.stderr)
        app_diagnostics = [line for line in (gui.stderr+again.stderr).splitlines() if re.search(rb"\(treehound[: -]", line)]
        assert not any(b"CRITICAL" in line or b"WARNING" in line for line in app_diagnostics), gui.stderr
    finally:
        daemon.terminate(); assert daemon.wait(timeout=5) == 0
print("GTK browse/search/treemap navigation/history capture integration passed")
